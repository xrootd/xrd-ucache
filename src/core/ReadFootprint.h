// Which parts of a file a run actually read, in ORIGINAL-file coordinates.
//
// This is the identity of the WORK, as opposed to the identity of the inputs.
// Two runs over the same files can still be different analyses -- different
// columns, different selection -- and comparing their walls would be
// meaningless. The input signature cannot tell them apart; this can.
//
// Original coordinates are the point. The byte tier, a relay and a
// cache-disabled run all address the origin's own layout, so their requests
// need no translation. A replica does not: it is a rewritten container whose
// baskets live at different offsets and different lengths, so a read of it is
// mapped back through the sidecar's origMap before being recorded here. That
// is what makes a replica run comparable with the baseline that measured it.
//
// Granularity is a fixed bucket rather than the exact byte range, because the
// two routes coalesce differently -- a replica serves ~465 KiB per request
// where the byte tier serves ~42 KiB for the same physics. Whole buckets are
// stable under that; byte ranges are not.
//
// Beside the buckets, a footprint keeps what they deliberately blur: the EXACT
// byte ranges read, merged where they touch or overlap, and the plain byte
// total of the requests, re-reads included. The buckets are the route-stable
// identity of the work; the ranges and the total are raw facts, recorded for
// whatever interprets a run later -- how much of a file was read at all, and
// how much of it more than once.
//
// The two halves take a read of a rewritten layout differently (noteMapped).
// A bucket takes a touched unit whole -- touching part of a recompressed
// basket means the basket was read. The byte counts cannot: a part of such a
// basket has no original offset, and counting its whole original range, or a
// share of it, would be a guess. So the counts take only bytes the layout's
// map names exactly, and a read it cannot name makes them unknown for the rest
// of the process, while the buckets go on as before.
//
// Thread-safety: every method is safe from any thread; one mutex guards the
// whole state. note() takes it once; the common case appends to or extends
// the last range.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ucache {

class ReadFootprint {
 public:
  ReadFootprint() = default;
  ~ReadFootprint();
  ReadFootprint(const ReadFootprint&) = delete;
  ReadFootprint& operator=(const ReadFootprint&) = delete;

  // 1 MiB, chosen by measurement rather than taste. The routes coalesce
  // differently: a byte-tier request merges adjacent pages and transfers the
  // gap between them, while a replica read maps back to the pages alone and
  // never names the gap. At 64 KiB those gaps landed in buckets one route
  // marked and the other did not, and four routes over one analysis produced
  // three different answers; from 256 KiB up they agreed on the sets measured
  // then, and 1 MiB is taken for margin. Discrimination survives it: two
  // analyses reading different columns of the same files share 17% of their
  // buckets at this size, so a different analysis still reads as different
  // work.
  //
  // IT IS MARGIN, NOT A GUARANTEE, and the difference matters to anything
  // that compares two runs. A replica is a rewritten container: the reader
  // asks it for different spans than it asks the original, so the two routes
  // do not merely coalesce differently, they request differently. On a file
  // set where only part of each file was relocated, the reader's spans over
  // the ORIGINAL layout crossed large unrelocated gaps that the replica never
  // asks for, and a whole bucket fell inside such a gap on 6% of the files --
  // the byte route marked it, the replica route did not. Both are honest
  // about what was read; they are answers to slightly different questions.
  // Exact equality of two signatures is therefore evidence of the same work,
  // while inequality is not proof of different work.
  static constexpr uint64_t kBucket = 1024 * 1024;

  // `sizeBytes`, when the caller knows it, is the file's size at the origin:
  // the byte total then takes only the part of the request inside the file,
  // at once. Without it the part past the end is taken off later, from the
  // requests that reached furthest (kTail) -- or at once after all, when the
  // footprint has learned the size meanwhile (learnSize).
  //
  // Never throws: short of memory, the file's ranges (or, for want of the
  // bucket set, the whole footprint) become unknown instead.
  void note(uint64_t off, uint64_t len, uint64_t sizeBytes = 0);

  // A read of a REWRITTEN layout -- a replica or the slot store -- in the
  // original file's coordinates, as (offset, length) pairs, under one lock.
  // `units` are the original ranges it touched, each whole: the buckets take
  // them exactly as note() would. `exact` are the original bytes it carried
  // byte for byte: the byte total and the ranges take those, as note() would.
  // A null `exact` says the layout cannot name them (the read covered part of
  // a relocated basket or page), and the byte counts become unknown
  // (bytesUnknown). Never throws.
  using Span = std::pair<uint64_t, uint64_t>;
  void noteMapped(const std::vector<Span>& units, const std::vector<Span>* exact,
                  uint64_t sizeBytes = 0);

  // The byte total and the distinct bytes are unknown from now on, for the
  // rest of the process; the buckets are untouched, and so is a poison(). For
  // a read whose bytes in the original file cannot be named exactly: unknown
  // beats wrong. Never throws.
  void bytesUnknown();

  // The file's size at the origin, learned by whoever holds it (a record
  // written with it, the end of the file met by a short read): requests noted
  // afterwards with no size are clipped at it as they come, as if they had
  // carried it. A relayed file has no size at hand when its reads are noted,
  // and a reader that asks past the end more often than kTail times -- one
  // that opens the file again and again -- would otherwise leave the byte
  // total unknown.
  void learnSize(uint64_t sizeBytes);

  // `limitBytes` is the file's size at the ORIGIN; buckets beyond it are
  // dropped. Readers routinely ask past the end -- ROOT fetches a file's tail
  // in fixed blocks -- and a route that serves the request verbatim would
  // record those bytes while a route that maps them through a replica's layout
  // would not. Past-EOF bytes are not file content, so neither should sign
  // them. 0 means "no limit known", which only a caller with no size may pass.
  std::string sig(uint64_t limitBytes = 0) const;
  bool empty() const;
  uint64_t count(uint64_t limitBytes = 0) const;

  // Both, under ONE lock. Taking sig() and count() separately lets a poison()
  // from another handle on the same URL land between them, so the record gets a
  // confident signature next to a zero bucket count -- a self-contradicting
  // pair, which is worse than emitting neither. Holding one reference to the
  // footprint does not close that: the two calls still lock independently.
  // Returns false when the footprint has nothing to say (poisoned, or empty),
  // in which case neither out-parameter is written.
  bool sigAndCount(uint64_t limitBytes, std::string& sigOut, uint64_t& countOut) const;

  // Declare this file's footprint permanently UNKNOWN. Sticky, and it wins
  // over anything recorded before or after.
  //
  // Skipping a read that cannot be expressed in the original file's
  // coordinates is not enough, because a footprint outlives the handle that
  // filled it: one process can read a file through a replica with no map
  // (contributing nothing) and then through the byte tier (contributing its
  // ranges), and what is emitted is a CONFIDENT signature covering part of the
  // work. Partial and confident is the one answer worse than none -- an empty
  // signature is treated as no evidence, while a partial one is compared and
  // silently disagrees. Once any read on a file could not be translated, the
  // file has nothing trustworthy to say for the rest of the process.
  void poison();
  bool poisoned() const;
  // Touched bucket indices, ascending. For diagnosis only: comparing two
  // signatures tells you THAT they differ, this tells you where.
  std::vector<uint64_t> buckets(uint64_t limitBytes = 0) const;

  // ---- exact ranges -------------------------------------------------------
  // Bounded twice, because a footprint lives as long as the process: a file
  // keeps at most kMaxRanges disjoint ranges, and the process at most
  // kMaxRangeBytes of them over all its files -- counted as the memory the
  // ranges occupy, spare capacity included, not as the ranges alone. Past
  // either bound the file's ranges are dropped for good (rangesOverflowed()),
  // and it then reports no distinct-byte count rather than part of one; the
  // byte total is kept. A columnar reader leaves thousands of ranges per file
  // -- one per run of adjacent baskets it reads, per cluster -- so the second
  // bound is what keeps a job over a thousand files in check. freeze() stores
  // a file's ranges in about a third of the space while no one reads it; the
  // next read restores them.
  static constexpr size_t kMaxRanges = 100000;
  static constexpr uint64_t kMaxRangeBytes = 128ull << 20;

  // What the file's requests add up to, below `limitBytes` (the file's size at
  // the origin; 0 = no limit known), from ONE locked state.
  struct Totals {
    // The bytes the requests asked for, re-reads counted. Unknown when the
    // footprint is poisoned or its byte counts were made unknown
    // (bytesUnknown), when more requests of unknown size ran past the limit
    // than the footprint follows (kTail), so the overrun cannot be taken off,
    // or when requests were clipped at a size other than the limit.
    bool origKnown = false;
    uint64_t origBytes = 0;
    // The distinct bytes read: unknown when poisoned, when the byte counts
    // were made unknown, or when the ranges overflowed.
    bool uniqueKnown = false;
    uint64_t uniqueBytes = 0;
  };
  // Never throws: short of memory to merge the ranges, the distinct bytes are
  // unknown.
  Totals totals(uint64_t limitBytes) const;

  // The ranges read, merged, clipped to the limit, ascending, as (offset,
  // length); empty when nothing below the limit was read. False, and nothing
  // written, when the footprint is poisoned, its byte counts unknown or its
  // ranges overflowed, or when there is no memory for the copy. Never throws.
  // For diagnosis, as buckets() is: the records carry only the totals.
  bool ranges(uint64_t limitBytes, std::vector<std::pair<uint64_t, uint64_t>>& out) const;
  bool rangesOverflowed() const;

  // Store the ranges compactly (delta-coded) until the next read. For a file
  // no handle reads any more; harmless on one still read, which only pays to
  // restore them. Never throws.
  void freeze();

  // Bytes of ranges held by every footprint in the process (for tests and
  // diagnosis). A forked child starts from zero: its parent's footprints are
  // left behind untouched (CacheStore::afterForkChild), and one it goes on
  // using anyway is counted afresh, from what it holds then, the first time
  // its ranges change -- never subtracted from the child's total for what it
  // held in the parent. Called from a fork child handler: no lock, no
  // allocation.
  static uint64_t rangeBytesTotal();
  static void afterForkChild();
  // Tests only: the process bound in place of kMaxRangeBytes (0 restores it).
  static void setRangeByteCapForTests(uint64_t bytes);

  // How many of the requests reaching furthest are followed, so that the part
  // of them past the file's end can be taken off the byte total.
  static constexpr size_t kTail = 16;

 private:
  // Both assume mu_ is held. The public accessors take the lock and check the
  // poison flag; sigAndCount() does so once for the pair.
  std::string sigLocked(uint64_t limitBytes) const;
  uint64_t countLocked(uint64_t limitBytes) const;

  // The rest assume mu_ is held too. Those that allocate may throw; their
  // callers catch it and treat it as an overflow.
  using Range = std::pair<uint64_t, uint64_t>;
  // The buckets [off, off + len) touches, when it is a range a footprint
  // takes at all (see note()); false otherwise.
  static bool bucketSpan(uint64_t off, uint64_t len, uint64_t& first, uint64_t& last);
  // Marks the buckets; false when there was no memory for them, and the
  // footprint is then poisoned.
  bool noteBucketsLocked(uint64_t first, uint64_t last);
  void noteRangeLocked(uint64_t a, uint64_t b, uint64_t size);
  void bytesUnknownLocked();
  void addRangeLocked(uint64_t a, uint64_t b); // may throw
  void noteTailLocked(uint64_t a, uint64_t b);
  // One more range in v: false (and the ranges overflowed) past a bound.
  bool roomLocked(std::vector<Range>& v);
  void compactLocked();            // may throw
  void thawLocked();               // may throw
  void overflowLocked();           // the ranges are dropped, for good
  void dropRangesLocked();         // frees every range and its share of the total
  uint64_t occupiedLocked() const; // bytes the ranges occupy now
  void settleLocked();             // the process total takes what they occupy now
  void rehold(uint64_t bytes); // this footprint now holds `bytes`
  bool overCap() const;
  // f(begin, end) for each merged range, ascending; false (after calling f for
  // some) only when memory ran out.
  template <typename F> bool forEachMergedLocked(F f) const;

  mutable std::mutex mu_;
  std::vector<uint64_t> bits_;
  bool poisoned_ = false;

  // The exact ranges, [begin, end): iv_ sorted, disjoint and never touching;
  // pend_ recent ones not merged in yet, so an out-of-order read costs an
  // append and a sort now and then, never a shift of the whole set. frozen_
  // holds iv_ delta-coded (frozenCount_ ranges) between readers.
  std::vector<Range> iv_;
  std::vector<Range> pend_;
  std::vector<uint8_t> frozen_;
  size_t frozenCount_ = 0;
  bool overflow_ = false;
  bool bytesLost_ = false;    // bytesUnknown: the ranges and the byte total are gone
  uint64_t held_ = 0;         // bytes this footprint counts in the process total
  uint64_t heldGen_ = 0;      // ... in this fork generation of the total
  uint64_t origRaw_ = 0;      // requests noted with no size, unclipped
  uint64_t origClipped_ = 0;  // requests noted with a size, clipped at it
  uint64_t clipSize_ = 0;     // that size (0: none yet)
  uint64_t knownSize_ = 0;    // learnSize
  bool clipConflict_ = false; // requests clipped at two different sizes
  bool origLost_ = false;     // no memory to follow the tail: the total is unknown
  // Of the requests noted with no size: the kTail that reach furthest, and the
  // furthest end of any other -- enough to clip their total at the file's end
  // exactly, as long as no more than kTail of them ran past it. Made at the
  // first such request: the cached route always knows the size, and most
  // footprints never need it.
  struct Req {
    uint64_t off, end;
  };
  struct Tail {
    Req r[kTail] = {};
    size_t n = 0;
    size_t minAt = 0;
  };
  std::unique_ptr<Tail> tail_;
  uint64_t tailLost_ = 0;
};

} // namespace ucache
