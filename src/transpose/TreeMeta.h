// ROOT-free structural parser for the transposer:
// extracts exactly what replica building needs from a TFile — header
// geometry, keys, the keys-list, and the streamed TTree's per-branch basket
// tables (name, fWriteBasket, fMaxBaskets, fBasketSeek/fBasketBytes arrays,
// leaf class/title, counter leaf) — with NO ROOT dependency.
//
// The walk is VERSION-EXACT and deliberately narrow: TTree v20, TBranch v13,
// TLeaf v2 (+TLeaf?_v1 subclasses), TObjArray v3 — the layouts present in
// every supported input (verified against an uproot reference dump, whose
// output is this parser's differential ground truth). Anything else
// => parse() fails with a reason: the caller fails open (no transpose),
// never guesses.
//
// Thread-safety: pure functions over caller-owned buffers; no shared state.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ucache::transpose {

// Byte source with presence semantics: `has` says whether a range can be read
// at all (bitmap-gated for cache images, always true for a plain file), `read`
// copies it. Shared by the replica builder and the parser's Source-based
// entry points; the cache-image implementations live with their callers.
struct Source {
  virtual ~Source() = default;
  virtual bool read(void* dst, uint64_t n, uint64_t off) = 0;
  virtual bool has(uint64_t off, uint64_t n) = 0; // false => range not usable
};

struct KeyInfo {
  int32_t nbytes = 0;
  uint16_t ver = 0;
  int32_t objlen = 0;
  uint16_t keylen = 0;
  uint16_t cycle = 0;
  int64_t seekkey = 0;
  std::string cls, name, title;
  uint16_t keyPartLen = 0; // bytes of the TKey part: its fields and three strings
};
// Parses a TKey header at `off`; nullopt on malformed/out-of-bounds.
std::optional<KeyInfo> parseKey(const uint8_t* p, size_t n, uint64_t off);

// What a decode could say about a compressed object's integrity.
//   kPassed        every block carried a check (LZMA CRC32, ZLIB adler32, LZ4
//                  XXH64, ZSTD content checksum) and it held, and the blocks
//                  decoded to exactly the object's length.
//   kFailed        a check failed, a block did not decode, or the lengths do
//                  not add up: these bytes are not the object that was written.
//   kNotCheckable  nothing to judge them by: a block this build cannot decode
//                  (an unknown codec, the old ROOT algorithm, LZ4 without lz4
//                  support) leaves `out` empty; a block that decodes but
//                  carries no check (ZSTD without a checksum, an LZMA stream
//                  declaring none) leaves `out` filled.
enum class FrameCheck : uint8_t { kPassed, kFailed, kNotCheckable };

struct DecodedFrames {
  FrameCheck check = FrameCheck::kFailed;
  std::vector<uint8_t> out; // the object, when it could be decoded
  // The last block's header claimed more input or output than there is, which
  // ROOT tolerates as long as the block's own check holds and the total is
  // right; counted so a report can say so, never a failure by itself.
  bool headerTolerated = false;
  const char* why = ""; // the reason, for kFailed and kNotCheckable
};

// Decode a ROOT multi-block compression container (XZ/ZL/ZS/L4) of an object
// `objlen` bytes long, checking each block as ROOT would. Never throws.
DecodedFrames decodeFrames(const uint8_t* payload, size_t n, uint64_t objlen);

// The same, for callers that only need the bytes: exactly `objlen` bytes, or
// empty when the object failed its check or cannot be decoded here.
std::vector<uint8_t> decompressFrames(const uint8_t* payload, size_t n, uint64_t objlen);

// The TBasket fields that follow the TKey part of a basket's key header.
struct BasketHeader {
  uint16_t version = 0;
  int32_t bufferSize = 0;
  int32_t nevBufSize = 0; // the entry size for fixed-size entries
  int32_t nevBuf = 0;     // entries in the basket
  int32_t last = 0;       // end of the entry data within the buffer (key included)
  uint8_t flag = 0;
};
// Parses the TBasket fields of key `k` at `off`; nullopt when the header is too
// short to hold them.
std::optional<BasketHeader> parseBasketHeader(const uint8_t* p, size_t n, uint64_t off,
                                              const KeyInfo& k);

// The codec a ROOT compression block names in the first two bytes of its
// 9-byte header: "zlib" (ZL), "lzma" (XZ), "zstd" (ZS) or "lz4" (L4). "" for
// anything else, the old ROOT algorithm (CS) included, which is not decoded
// here, and for fewer than 9 bytes. The one reading of these bytes, so that
// every place that asks what a basket or page holds gets the same answer.
std::string blockCodec(const uint8_t* p, size_t n);

// The TFile container level: header geometry, root directory, keys list. This
// is shared by every payload type — a TTree and an RNTuple differ only in the
// class of the key they hang off, so the walk to the keys list is identical
// and is deliberately written once. It is also the layer that takes hostile
// bytes first, and a second copy of these bounds checks would be a second
// copy to keep fuzzed.
struct ContainerMeta {
  bool large = false; // 64-bit header layout (file version > 1000000)
  int64_t fend = 0;
  int64_t keyslistSeek = 0;
  uint64_t dirSeekKeysOff = 0; // file offset of the directory's fSeekKeys
  // The header and the root-directory record carry INDEPENDENT seek widths
  // and need not agree; each is valid only at its own site. See FileMeta.
  int headerSeekWidth = 8;
  int dirSeekWidth = 8;
  int64_t fileSize = 0;
  std::vector<KeyInfo> keys; // the keys list, in file order
  std::string error;         // non-empty => nothing above is meaningful
};

// Walk an open ROOT file to its keys list. Bounds-checked throughout. The
// Source form reads through `src` (a range `src` cannot vouch for reads as
// short, and the walk fails there) and needs the file size the caller knows.
ContainerMeta parseContainer(int fd);
ContainerMeta parseContainer(Source& src, int64_t fileSize);

// Read key `k`'s payload, decompressing it when the key says it is compressed.
// Returns empty on a short read or a failed decompression.
std::vector<uint8_t> readKeyPayload(int fd, const KeyInfo& k);
std::vector<uint8_t> readKeyPayload(Source& src, const KeyInfo& k);

struct BranchInfo {
  std::string name;
  int32_t writeBasket = 0;  // baskets written; validated 0 <= writeBasket <= maxBaskets
  uint32_t maxBaskets = 0;
  int64_t entries = 0;              // the branch's fEntries
  std::vector<int64_t> basketSeek;  // [writeBasket]
  std::vector<int32_t> basketBytes; // [writeBasket]
  // The entry axis: basket i covers entries [basketEntry[i], basketEntry[i+1]).
  // [writeBasket + 1]; the last value is `entries` when ROOT did not store it
  // (a branch whose basket count reached fMaxBaskets).
  std::vector<int64_t> basketEntry;
  std::string leafClass;            // e.g. "TLeafF"
  std::string leafTitle;            // e.g. "Muon_pt[nMuon]/F"
  std::string leafCount;            // counter leaf name ("" = scalar)
  // Blob offsets of the live array values (for the transcoder's patches).
  uint64_t seekArrayOff = 0;  // fBasketSeek[0] within the decompressed blob
  uint64_t bytesArrayOff = 0; // fBasketBytes[0]
  // The branch's compression SETTING (algorithm * 100 + level; -1 inherits the
  // file's). What each basket actually holds is in its own frame headers — a
  // basket that did not shrink is stored raw whatever this says — so this
  // decides what is worth converting, never how to decode.
  int32_t compress = 0;
  // fZipBytes: the sum of this branch's stored basket lengths. Readers derive
  // sizes from it (uproot's step sizing, TTree::Print), so a layout that
  // changes basket lengths patches it; the offset is into the decompressed blob.
  int64_t zipBytes = 0;
  uint64_t zipBytesOff = 0;
  // fTotBytes: the same sum before compression. Below it, fZipBytes says from
  // the metadata alone that some of the branch's baskets are compressed --
  // whatever the setting above claims (see blockCodec).
  int64_t totBytes = 0;
  // fFileName is set: the baskets live in another file, so their seeks mean
  // nothing in this one and nothing may relocate them.
  bool externalFile = false;
};

struct FileMeta {
  bool large = false; // 64-bit header layout (file version > 1000000)
  int64_t fend = 0;
  int64_t keyslistSeek = 0;
  uint64_t dirSeekKeysOff = 0; // file offset of the directory's fSeekKeys
  // The file header and the root-directory record carry INDEPENDENT seek
  // widths, and they are not required to agree. The header goes 64-bit once
  // fEND passes 2 GB; the root directory record goes 64-bit only if its own
  // three seeks (fSeekDir/fSeekParent/fSeekKeys) need it. A file that grew
  // past 2 GB after its keys list had already been written keeps a narrow
  // directory record pointing at an early keys list — a legitimate in-the-wild
  // layout. Each width is therefore only valid at its own site.
  int headerSeekWidth = 8; // fEND/fSeekFree in the header
  int dirSeekWidth = 8;    // fSeekDir/fSeekParent/fSeekKeys in the directory
  KeyInfo treeKey;                // the live (highest-cycle) tree key
  std::vector<uint8_t> treeBlob;  // decompressed tree metadata
  int64_t entries = 0;            // the tree's fEntries
  int64_t autoFlush = 0;          // fAutoFlush (0 = not set; < 0 = bytes, > 0 = entries)
  // Offsets into treeBlob of the tree-level fields ROOT sizes its read cache
  // and estimates cluster lengths from (TTree::GetCacheAutoSize): a layout
  // that makes baskets longer must scale them with it, or the reader's cache
  // no longer holds a cluster and every fill splits.
  uint64_t autoFlushOff = 0;
  int64_t zipBytes = 0;           // the tree's fZipBytes
  uint64_t zipBytesOff = 0;
  std::vector<int64_t> clusterRangeEnd; // fClusterRangeEnd[fNClusterRange]
  std::vector<int64_t> clusterSize;     // fClusterSize[fNClusterRange]
  std::vector<BranchInfo> branches;
  std::string error; // non-empty => parse failed (fail open, no transpose)
};

// Parse `path` (whole file is pread as needed; the tree metadata blob is
// decompressed in memory). On any unsupported version/layout, returns a
// FileMeta with `error` set and no branches.
FileMeta parseFile(const std::string& path, const std::string& tree = "Events");

// The same walk over a Source: for a caller that holds the bytes itself (a
// cache image, a staged fill) rather than a path. `fileSize` is the origin
// size the caller knows. Fails, never guesses, wherever `src.has` is false.
FileMeta parseFile(Source& src, int64_t fileSize, const std::string& tree = "Events");

// The TTree walk alone, over an already decompressed tree record whose key
// header was `keylen` bytes: fills entries, cluster fields and branches into
// `fm`, or sets `fm.error`. Exposed so the walk can be fuzzed and unit-tested
// on crafted blobs without a container around them.
bool parseTreeBlob(const uint8_t* blob, size_t n, uint16_t keylen, FileMeta& fm);

// A basket's key header held against the tree's own record of that basket.
// The key header carries no checksum; these comparisons are what can be
// checked in it. `fail` is set only for fields ROOT relies on when it reads
// the basket (where it sits, where its data ends, how many entries it holds)
// — a contradiction there means the bytes are not the basket that was
// written. Fields ROOT does not use are only noted.
struct BasketKeyCheck {
  bool fail = false;
  std::vector<std::string> problems;  // why `fail` is set
  std::vector<std::string> anomalies; // differences ROOT does not act on
};
BasketKeyCheck checkBasketKey(const KeyInfo& k, const BasketHeader& b, const BranchInfo& br,
                              size_t basket, const std::string& treeName);

} // namespace ucache::transpose
