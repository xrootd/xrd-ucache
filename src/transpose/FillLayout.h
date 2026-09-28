// The layout a TTree file is presented in while its replica is being created
// on the first read — computed from the file's metadata alone, before any
// basket has been fetched.
//
// A reader finds each basket through two numbers in the tree metadata:
// fBasketSeek[i] (where) and fBasketBytes[i] (how much to READ). How to decode
// the basket comes from its own key header (fNbytes, fObjlen) and frame
// headers, so a region longer than the basket is legal: the reader decodes
// fNbytes and ignores the rest. That is what lets this layout be fixed before
// any basket exists: every basket of a relocatable branch gets a SLOT of
// k x fBasketBytes[i] (k given in hundredths, the slot rounded down to a byte)
// in an extension past fEND, and the basket's recompressed record is written
// into it when the reader first asks for it.
//
// What the reader is shown, and what never changes for the life of a handle:
//   * the file header with fEND = the virtual end (promoted in place from the
//     32-bit to the 64-bit header layout when the virtual end passes 2^31);
//   * the keys-list record, in place, with the live tree entry pointing at the
//     relocated metadata key;
//   * at metaSeek, the relocated tree-metadata key: the ORIGINAL key header
//     with fNbytes and fSeekKey patched, and the tree record with every
//     relocated basket's seek and length pointing at its slot. It comes FIRST
//     in the extension so that a 32-bit keys-list entry can still address it;
//     the slots after it may lie anywhere, because fBasketSeek is 64-bit;
//   * fZipBytes (tree and branch) and fAutoFlush are NOT scaled: ROOT sizes
//     its read cache from them (TTree::GetCacheAutoSize), so the real total
//     keeps the cache, and the memory it takes, what it is for the original
//     file. A fill holding padded slots then splits into more requests.
//
// Rules this obeys, each checked against ROOT:
//   * no key header ever changes LENGTH — entry offsets inside a basket are
//     absolute positions that include it (an 8-byte change crashes ROOT);
//   * a slot's record carries fSeekKey = the slot's offset (ROOT rejects a
//     basket whose fSeekKey differs from where it was read);
//   * the rest of a slot is ZEROS: for an uncompressed basket ROOT keeps the
//     read length and parses what follows the record as a displacement array.
//
// Thread-safety: pure functions over caller-owned data; no shared state.
#pragma once

#include "TreeMeta.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ucache::transpose {

// One relocated basket: where it is in the origin file, and its slot.
struct FillSlot {
  uint64_t origSeek = 0;
  uint32_t origLen = 0;
  uint64_t vSeek = 0;
  uint32_t vLen = 0;
  uint32_t branch = 0; // index into FileMeta::branches
  uint32_t basket = 0; // index into that branch's baskets
};

struct FillLayout {
  std::string error;        // non-empty => DECLINED: serve the file as it is
  // With `error`: declined for the codec the file is stored in -- one
  // `recompress_codecs` does not name, or one not converted at all. A sweep
  // declines such a file too; any other decline is the slot layout's own, and
  // `ucache recompress` can still build the file's replica from the byte cache.
  bool codecDecline = false;
  uint64_t originSize = 0;  // the original fEND
  uint64_t virtualSize = 0; // the fEND the reader is shown
  // Ranges of the ORIGINAL file that read differently, in place: the header
  // (fEND alone, or the whole header when promoted) and the keys-list record.
  // Ascending, non-overlapping, all below originSize.
  struct Window {
    uint64_t off = 0;
    std::vector<uint8_t> bytes;
  };
  std::vector<Window> windows;
  uint64_t metaSeek = 0;             // the relocated tree-metadata key
  std::vector<uint8_t> metaRecord;   // its complete record, as served
  uint64_t slotsBegin = 0;           // [metaSeek + metaRecord.size(), slotsBegin) reads as zeros
  std::vector<FillSlot> slots;       // ascending vSeek, contiguous from slotsBegin
  std::vector<uint32_t> relocated;   // branch indices, in slot order
};

// The file-header window that makes the header say fEND = `newEnd`: fEND at
// the header's own width at offset 12, or -- when a 32-bit header cannot hold
// it -- the whole header rewritten in place in the 64-bit layout at offset 0,
// as ROOT itself does when a file passes 2 GB. `header` = the file's first
// fBEGIN bytes. False (err set) when the header cannot be read.
bool headerWindowForEnd(const std::vector<uint8_t>& header, uint64_t newEnd, uint64_t& windowOff,
                        std::vector<uint8_t>& window, std::string& err);

// The codec a compression SETTING names: "zlib", "lzma", "lz4" or "zstd";
// "" for no compression, an inherited setting that is itself unset, or an
// algorithm this code does not transcode. -1 inherits `fileCompress`.
std::string codecOfSetting(int32_t compress, int32_t fileCompress);

// The codec the baskets of a file's UNNAMED branches are stored in: branches
// whose setting names no codec (codecOfSetting gives "") while their metadata
// shows compression (fZipBytes below fTotBytes). Setting 1 -- algorithm 0,
// "the global default", which files from older ROOT versions and `hadd -f1`
// carry -- is one such setting; a basket copied in under another setting is
// the other. Named by the compression header of ONE basket read through
// `src`: basket 0 of the branch whose metadata shows the most compression (of
// the next such branch when that basket was stored uncompressed, three at
// most), so a file whose settings name their codecs reads nothing here.
// `codec` = that basket's codec, or "" (no branch in that state, or a codec
// not converted here). False only when a read failed: nothing is decided.
bool unnamedSettingCodec(const FileMeta& fm, const std::vector<uint8_t>& header, Source& src,
                         std::string& codec);

// Compute the layout. `fileSize` = the origin's size, which must equal fEND
// (anything past fEND would lie under the extension). `header` = the file's
// first fBEGIN bytes (at least 75),
// `treeKeyHeader` = the tree key's first keylen bytes, `keysList` = the whole
// keys-list record at fm.keyslistSeek. A branch is relocated when its codec is
// in `codecs`, it has baskets, all inside the file, and none lives in another
// file. Its codec is the one its setting names; for an unnamed branch (see
// unnamedSettingCodec) it is `unnamedCodec`. Declines (error set) rather than
// guess.
FillLayout layoutForFill(const FileMeta& fm, uint64_t fileSize, const std::vector<uint8_t>& header,
                         const std::vector<uint8_t>& treeKeyHeader,
                         const std::vector<uint8_t>& keysList,
                         const std::vector<std::string>& codecs, uint32_t slotFactor100 = 300,
                         const std::string& unnamedCodec = "");

// What to tell a user about a first pass the file's own content declined
// (`L.error` set): the reason, and -- when it is not the codec -- that
// `ucache recompress` can build the replica after the run from what it cached.
std::string declineNote(const FillLayout& L);

// A basket prepared for its slot, from its ORIGINAL record.
struct ConvertedBasket {
  enum Kind { kZstd, kRaw, kOriginal };
  Kind kind = kOriginal;
  // The key header (original length, fNbytes set to the record's length,
  // fSeekKey NOT yet set) followed by the payload. Position-independent: the
  // same record serves any slot, and is what the published replica keeps.
  std::vector<uint8_t> record;
  std::string codec;  // the codec the basket was actually stored in ("" = raw/unknown)
  std::string error;  // non-empty => not a basket record; nothing to place
};

// ZSTD-1 when the basket's actual codec is listed and the result fits the
// slot; else the basket uncompressed when that fits (a basket ZSTD cannot
// shrink); else the original record, which always fits. A basket that cannot
// be decoded is kept as its original record: the reader then gets exactly the
// origin's bytes, and fails exactly as it would have.
//
// What the basket holds is read from the basket itself -- its key header says
// whether it is stored uncompressed, and each compression block's header names
// the block's codec -- never from the branch's setting or from the codec the
// layout took for the branch. So a basket stored in another codec than its
// branch's other baskets (a file merged from inputs written differently) is
// converted when that codec is listed and kept as stored when it is not, and
// a basket stored uncompressed is kept as stored.
ConvertedBasket convertBasket(const uint8_t* record, size_t n, uint32_t slotLen,
                              const std::vector<std::string>& codecs);

// Set a key record's fSeekKey at the key's own width (64-bit above version
// 1000). False when a 32-bit key cannot hold `seek`, or the record is too short.
bool patchKeySeek(uint8_t* record, size_t n, uint64_t seek);

// Write `c` into `out` (exactly slot.vLen bytes): the record with fSeekKey set
// to slot.vSeek, then zeros. False (err set) when the record does not fit, or
// its key is the 32-bit kind and the slot lies past 2^31.
bool placeInSlot(const ConvertedBasket& c, const FillSlot& slot, uint8_t* out, std::string& err);

// The ORIGINAL-file bytes a read of [off, off + len) of the layout carried,
// byte for byte, as (offset, length) -- for counts that must be right or
// absent. The original file's range reads as itself (its in-place windows
// too: each is the same range of the original); a slot read WHOLE is its
// basket or page; the relocated metadata record read whole is `metaOrigin`,
// the original ranges it stands for; the alignment and padding between them
// carry nothing. False when the read covers only PART of a slot or of the
// metadata record: their bytes have no original offset, and `out` is then not
// to be used.
bool exactOriginRanges(const FillLayout& L,
                       const std::vector<std::pair<uint64_t, uint64_t>>& metaOrigin, uint64_t off,
                       uint64_t len, std::vector<std::pair<uint64_t, uint64_t>>& out);

} // namespace ucache::transpose
