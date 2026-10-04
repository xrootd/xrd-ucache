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
#include <functional>
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
  // `recompress_codecs` does not name, or one not converted at all. Any other
  // decline is the layout's own (the file is served from the byte cache).
  bool codecDecline = false;
  // A codec decline for codecs `recompress_codecs` does not name: those the
  // file's baskets (pages) are stored in, comma-separated.
  std::string declinedCodecs;
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
// The length of the 64-bit file header headerWindowForEnd writes at offset 0.
constexpr size_t kLargeHeaderBytes = 75;

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
// (`L.error` set): the reason. `ucache recompress` computes the same layout,
// and declines the same files.
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
//
// Under a mixed map (below) a reader reads a converted basket at its real
// length, from its slot's start: `realLen(i)`, when given, is slot i's real
// length (0 when it has none), and a read of at least that much from the
// slot's start is its whole basket too. `mapMeta` (offset, length), when not
// {0, 0}, is another map's metadata record, read whole as `metaOrigin` too.
bool exactOriginRanges(const FillLayout& L,
                       const std::vector<std::pair<uint64_t, uint64_t>>& metaOrigin, uint64_t off,
                       uint64_t len, std::vector<std::pair<uint64_t, uint64_t>>& out,
                       const std::function<uint32_t(uint32_t)>* realLen = nullptr,
                       std::pair<uint64_t, uint64_t> mapMeta = {0, 0});
// The same over slots held elsewhere (SlotTable.h): `slotsIn(a, b, f)` calls
// f(index, slot) for the slots overlapping [a, b), in order, until f returns
// false; it returns false when the slots cannot be had (the read then counts
// nothing exactly). `L.slots` is not used.
using SlotsIn = std::function<bool(uint64_t, uint64_t,
                                   const std::function<bool(uint32_t, const FillSlot&)>&)>;
bool exactOriginRanges(const FillLayout& L, const SlotsIn& slotsIn,
                       const std::vector<std::pair<uint64_t, uint64_t>>& metaOrigin, uint64_t off,
                       uint64_t len, std::vector<std::pair<uint64_t, uint64_t>>& out,
                       const std::function<uint32_t(uint32_t)>* realLen = nullptr,
                       std::pair<uint64_t, uint64_t> mapMeta = {0, 0});

// ---- Mixed maps (TTree)
//
// The slot layout above is map 0: every basket in a slot, stated at the
// slot's length. A MIXED map is the same address space with one difference:
// the baskets converted when it was made are stated at their REAL length (the
// record's), so a reader reads just the record and sizes its buffers from it;
// the rest are stated at their slot's length, as in map 0, and a slot converted
// later serves its record there, followed by zeros. No basket moves: a slot's
// bytes are the same whichever map a reader holds, so readers holding
// different maps -- reopens, other processes, older releases -- all read the
// same file. A map is its own relocated tree record and keys-list entry; the
// header and every other byte are map 0's.
//
// A map's tree record lies in the reservation layoutForFill makes for map 0's
// (the raw tree record's length, past map 0's compressed one): below the
// slots, so a 32-bit keys-list entry that can address map 0's can address
// them too. A tree record compresses about 5x, so a few maps fit there at once.
struct MapTables {
  uint64_t first = 0, end = 0; // [first, end): 8-byte aligned start, the slots' start
  bool empty() const { return end <= first; }
};
MapTables mapTables(const FillLayout& L);
// The largest place a map's tree record may lie at: past 2 GiB only when the
// tree key and its keys-list entry are both 64-bit.
uint64_t mapSeekLimit(const FillLayout& L);

// A key's record (header, then its payload raw or compressed), decompressed,
// and its header's length. False when the record cannot be decoded.
bool decodeKeyRecord(const std::vector<uint8_t>& rec, std::vector<uint8_t>& blob, uint16_t& keylen);
// Map 0's relocated tree record, decoded: what every map is made from.
inline bool decodeMetaRecord(const FillLayout& L, std::vector<uint8_t>& blob, uint16_t& keylen) {
  return decodeKeyRecord(L.metaRecord, blob, keylen);
}

// The lengths a map's decoded tree record `blob` states that differ from the
// slot's (slot, length), ascending: what the map states at a real length.
// `fields` as for stateRealLengths. False when `blob` does not fit `fields`.
bool statedRealLengths(const std::vector<uint8_t>& blob, const FileMeta& fields,
                       const FillLayout& L, std::vector<std::pair<uint32_t, uint32_t>>& real);

// State the real lengths `real` = (slot, length) in the tree record `blob`:
// each slot's basket's fBasketBytes, and fZipBytes of its branch and of the
// tree by the difference from its original length. `fields` carries where in
// `blob` the arrays and totals are (parseTreeBlob of `blob`, or the metadata
// the layout was made from). False (err set) when a slot's basket is not where
// the layout put it, or a length is out of range.
bool stateRealLengths(std::vector<uint8_t>& blob, const FileMeta& fields, const FillLayout& L,
                      const std::vector<std::pair<uint32_t, uint32_t>>& real, std::string& err);
// The same with the slots looked up through `slot` (index < `nSlots`).
bool stateRealLengths(std::vector<uint8_t>& blob, const FileMeta& fields, uint32_t nSlots,
                      const std::function<const FillSlot&(uint32_t)>& slot,
                      const std::vector<std::pair<uint32_t, uint32_t>>& real, std::string& err);

// State the addresses `seeks` = (slot, address) in the tree record `blob`:
// each slot's basket's fBasketSeek. Lengths are stated by stateRealLengths;
// call that first. `fields`, `nSlots`, `slot` as there. False (err set) when a
// slot's basket is not where the layout put it.
bool stateSeeks(std::vector<uint8_t>& blob, const FileMeta& fields, uint32_t nSlots,
                const std::function<const FillSlot&(uint32_t)>& slot,
                const std::vector<std::pair<uint32_t, uint64_t>>& seeks, std::string& err);

// A map's metadata record for the tree record `blob`, placed at `seek`: map
// 0's key header (same length, fNbytes and fSeekKey set), then `blob`
// compressed ZSTD-1. Never raw: ROOT tells a raw record from a compressed one
// by the length the keys list states, so a reader holding another map's
// entry at that place must find the same form. False when it does not shrink.
bool mapMetaRecord(const FillLayout& L, const std::vector<uint8_t>& blob, uint64_t seek,
                   std::vector<uint8_t>& out, std::string& err);

// A map's keys-list window: map 0's, with the live tree entry pointing at the
// map's metadata record (`seek`, `nbytes`). False when the entry cannot hold
// `seek` (a 32-bit entry past 2 GiB) or is not found.
bool keysListForMap(const FillLayout& L, uint64_t seek, uint32_t nbytes, FillLayout::Window& out,
                    std::string& err);

} // namespace ucache::transpose
