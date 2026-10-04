// A copy of a file that the cache shows in another layout (its slot store's),
// made by a program that took the file's size from the
// origin -- fsspec's buffered file, a read loop sized by the namespace stat --
// reads [0, origin size) of that layout and stops, or trims its last piece to
// that size. Those bytes are not the origin's file: the header's end and the
// tree's record in them point into the part past the origin's size, which the
// copy never keeps. The copy would be silently corrupt.
//
// Every reader of the file's CONTENT reads the part past the origin's size
// early on -- the relocated tree record, keys list or RNTuple footer lives
// there -- with a request that starts there. So on one handle:
//
//   - a request that starts at or past the origin's size is a reader's, and
//     tracking stops for good (everything is allowed from then on);
//   - otherwise the request's part below the origin's size is added to what
//     the handle has read, and the request that completes [0, origin size) is
//     refused: the copy fails with an error instead. That includes a last piece
//     that runs past the origin's size (a fixed-size block, an unclamped loop),
//     which a copy sized from the origin trims to that size -- unless the
//     handle's own stat told the program the layout's size (sizeShown()): a
//     loop sized by that reads on past the origin's size and keeps the whole
//     layout, a valid file;
//   - a request that runs past the origin's size WITHOUT completing it (a block
//     that holds the relocated metadata) is a reader's too, and stops tracking;
//   - one request for exactly [0, origin size) is refused at any time -- that
//     is the origin's file asked for by its size, which only a copy asks of a
//     larger layout -- and one request for exactly the whole layout is allowed:
//     that is a valid file;
//   - past kMaxRuns disjoint ranges, which only a random-access reader
//     produces, tracking stops.
//
// Per handle: pieces of one copy spread over several handles, or over forked
// processes sharing one, are not seen together.
//
// Pure policy, no I/O. Thread-safety: fully thread-safe (one lock);
// afterForkChild() remakes the lock in a forked child (single-threaded).
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace ucache {

class CopyGuard {
 public:
  using Range = std::pair<uint64_t, uint64_t>; // offset, length

  // False: refuse these ranges, as asked of a handle shown `shownSize` bytes of
  // a file the origin has `originSize` bytes of (lengths as requested, before
  // any clamp to the shown size). Once a copy was refused, every later request
  // is refused too; once tracking stopped, only an exact [0, originSize) is.
  bool allowRanges(uint64_t originSize, uint64_t shownSize, const Range* ranges, size_t n);
  bool allow(uint64_t originSize, uint64_t shownSize, uint64_t off, uint64_t len) {
    const Range r{off, len};
    return allowRanges(originSize, shownSize, &r, 1);
  }

  // The handle answered a stat with the size of the layout it shows.
  void sizeShown();

  void afterForkChild() { new (&mu_) std::mutex; }

  static constexpr size_t kMaxRuns = 64;

 private:
  void stop(); // mu_ held

  std::mutex mu_;
  bool off_ = false;
  bool refused_ = false;
  bool sizeShown_ = false;
  std::vector<Range> runs_; // disjoint [begin, end) below originSize, sorted
};

} // namespace ucache
