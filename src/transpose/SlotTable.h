// The slot table of a first-pass layout (FillLayout.h), held a branch at a time.
//
// A TTree file's layout has a slot for every basket of every branch in a
// converted codec: 669,445 for a 1336-branch NanoAOD file, 40 bytes each, held
// by every process for every file it has open -- while a job typically reads a
// few dozen of those branches. This table keeps an index of the RUNS (the
// consecutive slots of one branch) and decodes a run's entries only when a
// read first touches it, from the stored layout read again through a
// caller-supplied source. A table can also be given every slot at once
// (assign): RNTuple files, and tests.
//
// The stored encoding is the layout blob's slot section, one record per slot
// against the slot before (the first against zeros and against `slotsBegin`):
// branch delta and basket delta minus one (zigzag varints), original seek
// delta (zigzag), original length (varint), slot length (varint: 0 for the
// plain factor times the original length, else length + 1), and the slot's
// seek minus the previous slot's end (zigzag). Integers are LEB128.
//
// Thread-safety: every member may be called from any thread. A reference
// at() returns stays valid for the table's life. Decoding takes the table's
// own lock and never calls back into the caller except through the source.
#pragma once

#include "FillLayout.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace ucache::transpose {

class SlotTable {
 public:
  // The raw (decompressed) layout bytes the table was indexed from, again.
  // False when they cannot be had; the table then cannot decode more runs.
  using Source = std::function<bool(std::vector<uint8_t>& raw)>;
  struct Geometry {
    uint64_t slotsBegin = 0, virtualSize = 0, originSize = 0;
    uint32_t slotFactor100 = 0; // the plain slot length is origLen x this / 100
    bool rntuple = false;       // a slot may be shorter than its stored page
  };

  SlotTable() = default;
  SlotTable(const SlotTable&) = delete;
  SlotTable& operator=(const SlotTable&) = delete;

  // Every slot, held from now on.
  void assign(std::vector<FillSlot> all);
  // Index `n` slot records at raw[off] without keeping their entries, checking
  // every one as a full decode does (in order, inside the layout, a TTree slot
  // at least its basket, the basket inside the original file). `off` is left
  // after the section. False when malformed; the table is then empty.
  bool index(const std::vector<uint8_t>& raw, size_t& off, uint32_t n, const Geometry& g,
             Source src);

  uint32_t size() const { return n_; }
  // Decode the runs holding the slots that overlap [a, b) of the layout, or
  // the slots given: one source read for all of them. False when the source
  // failed (the slots that were decoded stay).
  bool prepare(uint64_t a, uint64_t b);
  bool prepareRanges(const std::vector<std::pair<uint64_t, uint64_t>>& ranges); // (offset, length)
  bool prepareSlots(const std::vector<uint32_t>& idx);
  // Slot i, decoded if it was not (throws std::runtime_error if it cannot be).
  const FillSlot& at(uint32_t i);
  // The last slot starting at or before `pos`, for `pos` in [slotsBegin,
  // virtualSize). False outside, or when it cannot be decoded.
  bool find(uint64_t pos, uint32_t& i);
  // The slots overlapping [a, b), in order, decoded as needed; `f` returns
  // false to stop. False when they cannot be decoded.
  bool forEach(uint64_t a, uint64_t b, const std::function<bool(uint32_t, const FillSlot&)>& f);
  // Every slot, in order: decoded transiently where the table does not hold
  // it (nothing more is kept). False when the source failed.
  bool forEachAll(const std::function<void(uint32_t, const FillSlot&)>& f);
  // The decoded slots whose ORIGINAL range overlaps [a, b), by original seek.
  // Slots of runs not decoded are not reported.
  void decodedOverlappingOrig(uint64_t a, uint64_t b, std::vector<uint32_t>& out);
  // Slots whose entries the table holds.
  uint64_t decodedSlots() const { return decodedSlots_.load(std::memory_order_relaxed); }

 private:
  struct Run {
    uint32_t first = 0, count = 0;
    uint64_t vStart = 0, vEnd = 0;
    size_t rawOff = 0; // record of slot `first`
    // decoding state before slot `first`
    uint32_t prevBranch = 0, prevBasket = 0;
    uint64_t prevOrig = 0, nextV = 0;
    std::atomic<const FillSlot*> slots{nullptr};
  };
  size_t runOf(uint32_t i) const;           // the run holding slot i
  void runsIn(uint64_t a, uint64_t b, std::vector<size_t>& out) const; // undecoded, overlapping
  bool decodeRuns(const std::vector<size_t>& want); // under mu_
  bool decodeRun(const std::vector<uint8_t>& raw, Run& r, std::unique_ptr<FillSlot[]>& out) const;

  uint32_t n_ = 0;
  Geometry g_;
  Source src_;
  std::unique_ptr<Run[]> runs_;
  size_t nRuns_ = 0;
  std::mutex mu_; // decoding; guards owned_, byOrig_
  std::vector<std::unique_ptr<FillSlot[]>> owned_;
  std::vector<FillSlot> all_; // assign()'s
  std::vector<uint32_t> byOrig_; // decoded slots by original seek (built on first need)
  bool byOrigFresh_ = false;
  std::atomic<uint64_t> decodedSlots_{0};
};

} // namespace ucache::transpose
