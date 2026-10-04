// The layout a file is shown in while its replica is made as it is read, as a
// slot store keeps it: computed once from the file's metadata (FillLayout.h,
// RNTupleRewrite.h), stored with the store, and served by everyone exactly as
// stored. Shared by the plugin, which makes stores as files are read, and the
// command line, which converts what is in the byte cache into the same stores.
//
// Thread-safety: the functions are pure (they read and write only their
// arguments). A StoredLayout is filled by one thread and then read; its slot
// table decodes lazily under its own lock (SlotTable.h).
#pragma once

#include "Config.h"
#include "FillLayout.h"
#include "SlotStore.h"
#include "SlotTable.h"
#include "TreeMeta.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ucache::transpose {

// The slot factor: a TTree basket's slot is 3 times its stored length. Fixed,
// not a setting: where every slot sits follows from it, so a store made again
// after one was removed lays out the same grid, and a reader still holding the
// old store's positions reads the same baskets there. (At 3 the ZSTD-1 form of
// all but ~2% of NanoAOD's LZMA baskets fits, 0.1% of the bytes; a basket
// whose form does not fit is served as it was stored.) An existing store is
// served with the factor it records.
constexpr uint32_t kSlotFactor100 = 300;

// Bumped whenever the layout a store was made for could be computed
// differently: a store of another version is replaced, never served.
// 2: RNTuple slot pages carry their checksum.
// 3: the tree states its real total size; the slot factor is fixed.
// 4: a compression setting that names no codec takes the codec the file's own
//    baskets (pages) are stored in, where 3 left those branches as stored.
//    That changes which files and branches are converted, never how a layout
//    is laid out: a version 3 layout is still served as it is, while a
//    version 3 DECLINED store is decided again (see adoptable).
// 5: an RNTuple file's slots lie column by column (4: cluster by cluster). A
//    version 3 or 4 layout is still served as it is. A TTree file's layout is
//    computed as in 4, and its store still says 4, so an earlier release
//    serves it.
constexpr uint32_t kLayoutVersion = 5;
constexpr uint32_t kTTreeLayoutVersion = 4;
constexpr uint32_t kOldestServedLayout = 3;
// Where a store's decision to convert (or to decline) was last changed.
constexpr uint32_t kOldestDecision = 4;
inline uint32_t layoutVersionOf(bool rnt) { return rnt ? kLayoutVersion : kTTreeLayoutVersion; }

// A file's stored layout and the settings it was computed with.
struct StoredLayout {
  // RNTuple only: what decoding a page needs, per slot.
  struct Page {
    uint32_t nbytes = 0;
    bool hasChecksum = false;
  };
  bool rnt = false; // an RNTuple container: slots hold DECODED pages
  FillLayout L;
  std::vector<Page> pages; // RNTuple, by slot
  // The original ranges the layout's relocated metadata stands for (the tree
  // record; or the page list and footer), for the read footprint.
  std::vector<std::pair<uint64_t, uint64_t>> metaOrigin;
  std::vector<std::string> codecs; // the store's: which baskets are converted
  uint32_t slotFactor100 = 0;      // the store's, in hundredths
  // The slot table (L.slots is emptied once a run is set up): for a TTree file
  // only the branches a request has touched are decoded (SlotTable.h).
  SlotTable slots;
  uint32_t nSlots() const { return slots.size(); }
  const FillSlot& slot(uint32_t i) { return slots.at(i); }
};

// Everything a reader could learn from the layout, hashed: two layouts that
// agree on it serve the same bytes at the same offsets.
uint64_t layoutHash(const FillLayout& L, bool rnt);

std::string joinCodecs(const std::vector<std::string>& v);
std::vector<std::string> splitCodecs(const std::string& s);

// A TTree slot's length when nothing capped it: k (in hundredths) times the
// stored basket.
uint32_t plainSlotLen(uint32_t origLen, uint32_t k100);

// The stored form of a layout (what a store keeps beside its header), and
// back. A stored layout is never recomputed: parsing a large tree's metadata
// costs ~200 ms per open, and a layout recomputed by another build could
// differ in its compressed metadata bytes.
std::vector<uint8_t> encodeLayout(const StoredLayout& lay);
// A stored layout blob's raw bytes (it is stored compressed, or raw when that
// did not shrink it). False when they are not a layout's.
bool layoutRaw(const std::vector<uint8_t>& blob, std::vector<uint8_t>& raw);
// Decode a stored layout into `lay`. With `lazy` (a TTree file), the slot table
// is indexed into lay.slots, its runs decoded from `lazy` when first needed;
// otherwise every slot lands in lay.L.slots.
bool decodeLayout(const std::vector<uint8_t>& blob, StoredLayout& lay,
                  SlotTable::Source lazy = nullptr);

// Parse the file through `src` and compute its layout, with lay.codecs and
// the slot factor `k100`, into lay. `fetchHead` is called before an RNTuple
// file is parsed (the plugin reads the file's head as one block there; null =
// nothing to do). False when the file is not served this way: `declined` then
// says whether that is the file's own content (worth remembering: lay.L.error
// says why, or the file has neither container) or a failed read (not), and
// `why` holds the parser's message. `anyCachedProbe`: the codec of branches
// (columns) whose setting names none may come from any basket (page) `src`
// has, not only the one every process asks (unnamedSettingCodec) -- for a
// byte cache's view, when no reader holds a layout's positions.
bool computeLayout(StoredLayout& lay, Source& src, const std::function<bool()>& fetchHead,
                   uint64_t size, uint32_t k100, bool& declined, std::string& why,
                   bool anyCachedProbe = false);

// The replica tier's adoption rule: size always; mtime or checksum only when
// `validate` asks for them (some storage reports differing mtimes for a file
// that has not changed).
bool adoptable(const SlotStoreHeader& h, uint64_t size, uint64_t originMtime, uint8_t cksumKind,
               uint32_t originCksum, ValidateMode validate);

} // namespace ucache::transpose
