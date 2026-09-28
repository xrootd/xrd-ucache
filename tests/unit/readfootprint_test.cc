// Read footprints: what a run READ, in origin coordinates.
//
// Every property here was established by a route disagreeing with another in
// the field, not by reasoning. Four routes over one analysis must produce one
// signature, and the ways that failed are what these tests hold shut.
#include "ReadFootprint.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using ucache::ReadFootprint;

namespace {
constexpr uint64_t kB = ReadFootprint::kBucket;
}

TEST(ReadFootprint, EmptyHasNoSignature) {
  ReadFootprint f;
  EXPECT_TRUE(f.empty());
  EXPECT_TRUE(f.sig().empty());
  EXPECT_EQ(f.count(), 0u);
}

TEST(ReadFootprint, OrderOfReadsCannotChangeIt) {
  // Threads deliver reads in whatever order they finish; a signature that
  // depended on that would differ between two identical runs.
  ReadFootprint a, b;
  a.note(0, 10);
  a.note(5 * kB, 10);
  a.note(2 * kB, 10);
  b.note(2 * kB, 10);
  b.note(0, 10);
  b.note(5 * kB, 10);
  EXPECT_EQ(a.sig(), b.sig());
  EXPECT_EQ(a.count(), 3u);
}

TEST(ReadFootprint, ReadingTheSameBucketTwiceIsTheSameAsOnce) {
  // It is a SET of touched regions. A commutative combination that cancelled
  // on repetition (xor of values) would erase a re-read.
  ReadFootprint a, b;
  a.note(0, 10);
  b.note(0, 10);
  b.note(3, 4);
  b.note(0, 10);
  EXPECT_EQ(a.sig(), b.sig());
  EXPECT_EQ(b.count(), 1u);
}

TEST(ReadFootprint, DifferentRegionsGiveDifferentSignatures) {
  ReadFootprint a, b;
  a.note(0, 10);
  b.note(9 * kB, 10);
  EXPECT_NE(a.sig(), b.sig());
}

TEST(ReadFootprint, ARangeSpanningBucketsMarksAllOfThem) {
  ReadFootprint f;
  f.note(kB - 1, 2 * kB); // straddles three
  EXPECT_EQ(f.count(), 3u);
}

// The failure that made four routes give three answers. ROOT fetches a file's
// tail in fixed-size blocks and so asks past the end; a route serving the
// request verbatim recorded those bytes, while a route mapping them through a
// replica's layout did not. Past-EOF bytes are not file content.
TEST(ReadFootprint, ReadsPastTheEndOfTheFileAreNotContent) {
  const uint64_t size = 3 * kB + 100;
  ReadFootprint plain, overrun;
  plain.note(0, size);
  overrun.note(0, size);
  overrun.note(size, 5 * kB); // the tail fetch that runs off the end
  EXPECT_NE(plain.sig(0), overrun.sig(0)) << "unclamped, they differ";
  EXPECT_EQ(plain.sig(size), overrun.sig(size))
      << "clamped to the origin size, the overrun contributes nothing";
  EXPECT_EQ(overrun.count(size), 4u);
}

TEST(ReadFootprint, AHostileRangeIsRefusedRatherThanAllocated) {
  // A corrupt or adversarial length must not turn a diagnostic into an
  // enormous allocation; losing the signature is the right price.
  ReadFootprint f;
  f.note(0, ~uint64_t(0));
  EXPECT_TRUE(f.empty());
}

// The granularity was chosen by measurement: the routes coalesce differently,
// so the byte tier transfers gaps between baskets that a replica read never
// names. At 64 KiB those gaps fell in buckets one route marked and the other
// did not. This pins the property that made 1 MiB the choice -- a sub-bucket
// gap cannot separate two runs -- without pinning the constant itself.
TEST(ReadFootprint, ASmallGapBetweenTwoReadsDoesNotChangeTheAnswer) {
  ReadFootprint merged, split;
  merged.note(0, kB / 2);            // one coalesced request over the gap
  split.note(0, kB / 8);             // the two pieces either side of it
  split.note(kB / 4, kB / 4);
  EXPECT_EQ(merged.sig(), split.sig());
}

TEST(ReadFootprint, AnAbsurdOffsetIsRefusedWithoutAllocating) {
  // The guard is about a plausible FILE, not a plausible integer. One read at
  // a petabyte offset used to size the bitmap for it -- half a gigabyte, held
  // under the lock for the life of the process, from a single bad request or
  // one corrupt mapping in a sidecar.
  ReadFootprint f;
  f.note(1ull << 52, 1);
  EXPECT_TRUE(f.empty()) << "an offset no file can reach must record nothing";
  f.note(4096, 4096); // and the object still works afterwards
  EXPECT_EQ(f.count(1 << 20), 1u);
}

TEST(ReadFootprint, PoisonMakesAFileSayNothingRatherThanSayHalf) {
  // The failure this exists for: a footprint outlives the handle that filled
  // it, so one process can read a file by a route whose offsets cannot be
  // translated (a replica with no map, which contributes nothing) and then by
  // one that can. What survives is a real, non-empty footprint describing PART
  // of the work -- and a partial signature is compared and silently disagrees,
  // where an empty one is correctly read as no evidence.
  ucache::ReadFootprint f;
  f.note(0, 4096);
  f.note(8ull << 20, 4096);
  ASSERT_EQ(f.count(16 << 20), 2u);
  ASSERT_FALSE(f.sig(16 << 20).empty());

  f.poison();
  EXPECT_TRUE(f.poisoned());
  EXPECT_TRUE(f.sig(16 << 20).empty()) << "no signature at all, not a shorter one";
  EXPECT_EQ(f.count(16 << 20), 0u) << "and no bucket count to contradict it";
}

TEST(ReadFootprint, PoisonIsStickyAgainstLaterReads) {
  // Sticky in the direction that matters: the untranslatable read usually
  // comes FIRST, and everything recorded afterwards is still only part of the
  // work. A flag that later reads could clear would restore exactly the
  // confident partial answer it was set to prevent.
  ucache::ReadFootprint f;
  f.poison();
  f.note(0, 4096);
  f.note(1 << 20, 4096);
  EXPECT_TRUE(f.poisoned());
  EXPECT_TRUE(f.sig(16 << 20).empty());
  EXPECT_EQ(f.count(16 << 20), 0u);
}

TEST(ReadFootprint, PoisoningTwiceIsStillPoisoned) {
  ucache::ReadFootprint f;
  f.poison();
  f.poison();
  EXPECT_TRUE(f.poisoned());
}

TEST(ReadFootprint, TheBucketIsExactlyOneMebibyte) {
  // The constant the whole comparison rests on, pinned in BOTH directions.
  // Every other test here is written in units of kBucket and is therefore
  // scale-invariant: changing 1 MiB to 64 KiB -- the size the header records
  // as having made four routes over one analysis give three different answers
  // -- left the entire suite green.
  EXPECT_EQ(ucache::ReadFootprint::kBucket, 1024u * 1024u);
  ucache::ReadFootprint f;
  f.note(0, 1);
  f.note(1024 * 1024 - 1, 1); // last byte of bucket 0
  EXPECT_EQ(f.count(16 << 20), 1u) << "a byte short of the boundary is the same bucket";
  f.note(1024 * 1024, 1); // first byte of bucket 1
  EXPECT_EQ(f.count(16 << 20), 2u) << "and the boundary byte is the next one";
}

TEST(ReadFootprint, SigAndCountAgreeUnderAConcurrentPoison) {
  // The pair must never contradict itself. Reading sig() and count()
  // separately leaves a window: a poison() landing between them yields a
  // confident signature next to a zero bucket count, and a record saying "this
  // run read these exact bytes, and it read nothing" is worse than no record.
  //
  // NEEDS AT LEAST THREE RUNNABLE CORES to mean anything. Pinned to one CPU
  // with taskset it detects the pre-fix shape 0 times in 20; on three or more
  // it detects it every time. A pass on a constrained runner is not evidence.
  //
  // Written as a race on purpose. Every other test in this file is
  // single-threaded, while the class exists to be written from an analysis
  // job's threads through a footprint shared across handles -- so the property
  // that matters most had no test at all.
  for (int round = 0; round < 200; ++round) {
    ReadFootprint f;
    for (int i = 0; i < 64; ++i)
      f.note(static_cast<uint64_t>(i) * ReadFootprint::kBucket, 1);

    std::atomic<bool> go{false};
    std::atomic<int> contradictions{0};
    std::vector<std::thread> ts;

    ts.emplace_back([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      f.poison();
    });
    for (int r = 0; r < 3; ++r)
      ts.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < 200; ++i) {
          std::string sig;
          uint64_t n = 0;
          if (f.sigAndCount(0, sig, n) && (sig.empty() || n == 0))
            contradictions.fetch_add(1, std::memory_order_relaxed);
        }
      });

    go.store(true, std::memory_order_release);
    for (auto& t : ts)
      t.join();

    ASSERT_EQ(contradictions.load(), 0)
        << "sigAndCount returned true with an empty signature or a zero count";
    // And once poisoned it stays that way, for this accessor too.
    std::string sig;
    uint64_t n = 0;
    EXPECT_FALSE(f.sigAndCount(0, sig, n));
  }
}

TEST(ReadFootprint, SigAndCountMatchesTheSingleValueAccessors) {
  // The combined call must not become a second implementation that drifts.
  ReadFootprint f;
  f.note(0, 1);
  f.note(5 * ReadFootprint::kBucket, 3 * ReadFootprint::kBucket);
  // ABOVE the limit on purpose: without a bucket past it, dropping the limit
  // argument entirely -- the likeliest wiring error for this signature -- still
  // agrees with the single-value accessors and the test proves nothing.
  f.note(40 * ReadFootprint::kBucket, 1);
  const uint64_t limit = 16 << 20;
  std::string sig;
  uint64_t n = 0;
  ASSERT_TRUE(f.sigAndCount(limit, sig, n));
  EXPECT_EQ(sig, f.sig(limit));
  EXPECT_EQ(n, f.count(limit));
}

TEST(ReadFootprint, SigAndCountSaysNothingRatherThanEmptyForAnUntouchedFootprint) {
  ReadFootprint f;
  std::string sig = "sentinel";
  uint64_t n = 12345;
  EXPECT_FALSE(f.sigAndCount(0, sig, n)) << "an untouched footprint has nothing to say";
  EXPECT_EQ(sig, "sentinel") << "out-parameters must be left alone on false";
  EXPECT_EQ(n, 12345u);
}

// ---- exact ranges --------------------------------------------------------
// Beside the buckets, the exact byte ranges and the plain byte total: the raw
// facts a later reading of the run interprets. They must be exact where the
// buckets are deliberately coarse, and absent rather than partial.

namespace {
using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;

Ranges rangesOf(const ReadFootprint& f, uint64_t limit = 0) {
  Ranges r;
  EXPECT_TRUE(f.ranges(limit, r));
  return r;
}
} // namespace

TEST(ReadFootprintRanges, TouchingAndOverlappingReadsMerge) {
  ReadFootprint f;
  f.note(100, 50);  // [100, 150)
  f.note(150, 50);  // touches: [100, 200)
  f.note(120, 200); // overlaps: [100, 320)
  f.note(400, 10);  // apart
  EXPECT_EQ(rangesOf(f), (Ranges{{100, 220}, {400, 10}}));
  const auto t = f.totals(0);
  ASSERT_TRUE(t.uniqueKnown);
  EXPECT_EQ(t.uniqueBytes, 230u);
  ASSERT_TRUE(t.origKnown);
  EXPECT_EQ(t.origBytes, 310u) << "re-reads count in the byte total, once in the distinct bytes";
}

TEST(ReadFootprintRanges, OrderOfReadsCannotChangeTheSet) {
  // Threads deliver reads in any order; the merged set must not depend on it,
  // including across the compaction of out-of-order ranges.
  std::vector<std::pair<uint64_t, uint64_t>> reads;
  for (uint64_t i = 0; i < 3000; ++i)
    reads.emplace_back(i * 1000, 400 + (i % 7) * 100); // some touch the next, most do not
  ReadFootprint fwd, rev, shuffled;
  for (const auto& [o, n] : reads)
    fwd.note(o, n);
  for (auto it = reads.rbegin(); it != reads.rend(); ++it)
    rev.note(it->first, it->second);
  for (size_t k = 0; k < reads.size(); ++k) { // a fixed permutation
    const auto& [o, n] = reads[(k * 7919) % reads.size()];
    shuffled.note(o, n);
  }
  const Ranges a = rangesOf(fwd);
  EXPECT_EQ(a, rangesOf(rev));
  EXPECT_EQ(a, rangesOf(shuffled));
  EXPECT_EQ(fwd.totals(0).uniqueBytes, shuffled.totals(0).uniqueBytes);
  EXPECT_EQ(fwd.totals(0).origBytes, shuffled.totals(0).origBytes);
}

TEST(ReadFootprintRanges, ClippedAtTheFilesEnd) {
  // ROOT reads a file's tail in fixed blocks and asks past the end; past-EOF
  // bytes are not content, in the byte total or in the distinct bytes.
  const uint64_t size = 10000;
  ReadFootprint f;
  f.note(0, 300);
  f.note(9000, 4096);  // runs 3096 bytes past the end
  f.note(12000, 100);  // wholly past it
  EXPECT_EQ(rangesOf(f, size), (Ranges{{0, 300}, {9000, 1000}}));
  const auto t = f.totals(size);
  EXPECT_EQ(t.uniqueBytes, 1300u);
  EXPECT_EQ(t.origBytes, 1300u);
  EXPECT_EQ(f.totals(0).origBytes, 300u + 4096 + 100) << "with no limit, everything asked for";
}

TEST(ReadFootprintRanges, TheOverrunIsTakenOffOnlyWhenEveryRequestPastTheEndIsKnown) {
  const uint64_t size = 1 << 20;
  ReadFootprint f;
  // More requests running past the end than the footprint follows: the byte
  // total cannot be clipped exactly, so it is unknown -- never an overcount.
  for (uint64_t i = 0; i < ReadFootprint::kTail + 1; ++i)
    f.note(size - 100 + i, 200);
  const auto t = f.totals(size);
  EXPECT_FALSE(t.origKnown);
  EXPECT_TRUE(t.uniqueKnown) << "the distinct bytes do not depend on it";
  EXPECT_EQ(t.uniqueBytes, 100u);
  // Many requests, but the ones past the end are few: exact.
  ReadFootprint g;
  for (uint64_t i = 0; i < 1000; ++i)
    g.note(i * 100, 100);
  g.note(size - 10, 50);
  g.note(size - 5, 50);
  const auto u = g.totals(size);
  ASSERT_TRUE(u.origKnown);
  EXPECT_EQ(u.origBytes, 1000u * 100 + 10 + 5);
}

TEST(ReadFootprintRanges, PoisonTakesTheRangesAndTheTotalsWithIt) {
  ReadFootprint f;
  f.note(0, 4096);
  f.poison();
  f.note(8192, 4096);
  const auto t = f.totals(1 << 20);
  EXPECT_FALSE(t.origKnown);
  EXPECT_FALSE(t.uniqueKnown);
  Ranges r{{1, 1}};
  EXPECT_FALSE(f.ranges(1 << 20, r));
  EXPECT_EQ(r, (Ranges{{1, 1}})) << "the out-parameter is left alone on false";
}

TEST(ReadFootprintRanges, PastTheCapTheFileHasNoDistinctCount) {
  // One more disjoint range than a file keeps: the ranges go, for good, and
  // the distinct-byte count with them; the byte total is kept.
  const uint64_t before = ReadFootprint::rangeBytesTotal();
  {
    ReadFootprint f;
    for (uint64_t i = 0; i < ReadFootprint::kMaxRanges; ++i)
      f.note(i * 10, 5);
    EXPECT_FALSE(f.rangesOverflowed());
    EXPECT_EQ(f.totals(0).uniqueBytes, ReadFootprint::kMaxRanges * 5);
    EXPECT_GT(ReadFootprint::rangeBytesTotal(), before);
    f.note(ReadFootprint::kMaxRanges * 10, 5);
    EXPECT_TRUE(f.rangesOverflowed());
    const auto t = f.totals(0);
    EXPECT_FALSE(t.uniqueKnown);
    ASSERT_TRUE(t.origKnown);
    EXPECT_EQ(t.origBytes, (ReadFootprint::kMaxRanges + 1) * 5);
    Ranges r;
    EXPECT_FALSE(f.ranges(0, r));
    EXPECT_EQ(ReadFootprint::rangeBytesTotal(), before) << "the memory is given back";
    f.note(0, 1); // and nothing comes back later
    EXPECT_FALSE(f.totals(0).uniqueKnown);
  }
  EXPECT_EQ(ReadFootprint::rangeBytesTotal(), before);
}

TEST(ReadFootprintRanges, OutOfOrderReadsAreCountedAgainstTheCapAfterMerging) {
  // Re-reads of ranges already held never overflow a full set: they merge.
  ReadFootprint f;
  for (uint64_t i = 0; i < ReadFootprint::kMaxRanges; ++i)
    f.note(i * 10, 5);
  for (uint64_t i = ReadFootprint::kMaxRanges; i-- > 0;)
    f.note(i * 10 + 1, 3);
  EXPECT_FALSE(f.rangesOverflowed());
  EXPECT_EQ(f.totals(0).uniqueBytes, ReadFootprint::kMaxRanges * 5);
}

TEST(ReadFootprintRanges, FreezeKeepsTheSetAndGivesMemoryBack) {
  const uint64_t before = ReadFootprint::rangeBytesTotal();
  ReadFootprint f;
  for (uint64_t i = 0; i < 5000; ++i)
    f.note(i * 70000 + (i % 13), 1000 + i % 500);
  f.note(123, 7); // out of order, still pending
  const Ranges want = rangesOf(f);
  const auto t0 = f.totals(0);
  const uint64_t live = ReadFootprint::rangeBytesTotal() - before;
  f.freeze();
  const uint64_t frozen = ReadFootprint::rangeBytesTotal() - before;
  EXPECT_LT(frozen * 2, live) << "frozen " << frozen << " vs live " << live;
  EXPECT_EQ(rangesOf(f), want) << "the same set, read while frozen";
  EXPECT_EQ(f.totals(0).uniqueBytes, t0.uniqueBytes);
  EXPECT_EQ(f.totals(0).origBytes, t0.origBytes) << "freezing is not a read";
  f.note(10ull << 30, 10); // the next read restores them, and adds to them
  Ranges more = want;
  more.emplace_back(10ull << 30, 10);
  EXPECT_EQ(rangesOf(f), more);
  EXPECT_EQ(f.totals(0).origBytes, t0.origBytes + 10);
}

TEST(ReadFootprintRanges, NothingReadIsZeroNotUnknown) {
  ReadFootprint f;
  const auto t = f.totals(1000);
  EXPECT_TRUE(t.origKnown);
  EXPECT_TRUE(t.uniqueKnown);
  EXPECT_EQ(t.origBytes, 0u);
  EXPECT_EQ(t.uniqueBytes, 0u);
  Ranges r{{1, 1}};
  ASSERT_TRUE(f.ranges(1000, r));
  EXPECT_TRUE(r.empty());
}

TEST(ReadFootprintRanges, ConcurrentReadersBuildOneSet) {
  // The class is written from an analysis job's threads through a footprint
  // shared across handles.
  ReadFootprint f;
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t)
    ts.emplace_back([&f, t] {
      for (uint64_t i = 0; i < 20000; ++i)
        f.note((i * 4 + static_cast<uint64_t>(t)) * 100, 50);
    });
  for (auto& t : ts)
    t.join();
  const auto s = f.totals(0);
  EXPECT_EQ(s.uniqueBytes, 80000u * 50);
  EXPECT_EQ(s.origBytes, 80000u * 50);
  EXPECT_EQ(rangesOf(f).size(), 80000u);
}

TEST(ReadFootprintRanges, ARequestOfKnownSizeIsClippedAsItComes) {
  // The cached route knows the file's size and clips at once, with no need to
  // follow the requests that ran furthest; a relayed request, noted with no
  // size, is clipped later. Both kinds add up in one total.
  const uint64_t size = 10000;
  ReadFootprint f;
  for (uint64_t i = 0; i < ReadFootprint::kTail + 5; ++i)
    f.note(size - 50, 100, size); // each 50 past the end; more than kTail of them
  f.note(0, 100);                 // a relayed one, within the file
  f.note(size - 10, 100);         // a relayed one past the end
  const auto t = f.totals(size);
  ASSERT_TRUE(t.origKnown);
  EXPECT_EQ(t.origBytes, (ReadFootprint::kTail + 5) * 50 + 100 + 10);
  EXPECT_EQ(t.uniqueBytes, 100u + 50);
}

TEST(ReadFootprintRanges, TwoSizesForOneFileLeaveTheTotalUnknown) {
  // A file that changed size at the origin between two opens: requests were
  // clipped at different ends, and no one limit describes them.
  ReadFootprint f;
  f.note(0, 100, 1000);
  EXPECT_TRUE(f.totals(1000).origKnown);
  EXPECT_FALSE(f.totals(2000).origKnown) << "clipped at another size than asked";
  f.note(0, 100, 2000);
  EXPECT_FALSE(f.totals(1000).origKnown);
  EXPECT_FALSE(f.totals(2000).origKnown);
  EXPECT_TRUE(f.totals(2000).uniqueKnown) << "the ranges themselves are exact";
}

TEST(ReadFootprintRanges, TheProcessTotalCountsTheMemoryTheRangesOccupy) {
  // Spare capacity is memory all the same: a set grown by doubling holds up to
  // twice what it uses, and a bound on the ranges alone would let the process
  // hold about twice its bound.
  const uint64_t before = ReadFootprint::rangeBytesTotal();
  ReadFootprint f;
  const uint64_t n = 5000;
  for (uint64_t i = 0; i < n; ++i)
    f.note(i * 10, 5);
  const uint64_t held = ReadFootprint::rangeBytesTotal() - before;
  EXPECT_GE(held, n * 16) << "at least the ranges themselves";
  EXPECT_LE(held, 4 * n * 16) << "and no more than the doubling can leave spare";
  // Out-of-order reads wait apart and are merged in; what they occupy is
  // counted too, and all of it goes back when the file is frozen.
  for (uint64_t i = n; i-- > 0;)
    f.note(i * 10 + 1, 2);
  EXPECT_GE(ReadFootprint::rangeBytesTotal() - before, n * 16);
  f.freeze();
  EXPECT_LT(ReadFootprint::rangeBytesTotal() - before, n * 4);
}

TEST(ReadFootprintRanges, PastTheProcessBoundAFileKeepsNoRanges) {
  ReadFootprint::setRangeByteCapForTests(ReadFootprint::rangeBytesTotal() + (200u << 10));
  {
    ReadFootprint a, b;
    for (uint64_t i = 0; i < 8000; ++i) // 125 KiB of ranges, 128 KiB with the doubling
      a.note(i * 10, 5);
    EXPECT_FALSE(a.rangesOverflowed());
    for (uint64_t i = 0; i < 8000; ++i)
      b.note(i * 10, 5);
    EXPECT_TRUE(b.rangesOverflowed()) << "the process has no room left for it";
    EXPECT_FALSE(b.totals(0).uniqueKnown);
    EXPECT_TRUE(b.totals(0).origKnown) << "the byte total does not need the ranges";
    EXPECT_FALSE(a.rangesOverflowed()) << "the file that fitted keeps its own";
    EXPECT_TRUE(a.totals(0).uniqueKnown);
  }
  ReadFootprint::setRangeByteCapForTests(0);
}

TEST(ReadFootprintRanges, AFrozenFileThatNoLongerFitsIsDroppedWhenReadAgain) {
  // Frozen, a file holds a fraction of its ranges' memory; read again it needs
  // all of it back, and if the process has meanwhile spent the room on other
  // files, it keeps no ranges rather than going past the bound.
  const uint64_t base = ReadFootprint::rangeBytesTotal();
  ReadFootprint a;
  for (uint64_t i = 0; i < 8000; ++i)
    a.note(i * 10, 5);
  a.freeze();
  ReadFootprint::setRangeByteCapForTests(ReadFootprint::rangeBytesTotal() + (160u << 10));
  {
    ReadFootprint b;
    for (uint64_t i = 0; i < 6000; ++i) // takes the room
      b.note(i * 10, 5);
    EXPECT_FALSE(b.rangesOverflowed());
    a.note(1 << 30, 5); // thawed: 128 KiB it no longer has room for
    EXPECT_TRUE(a.rangesOverflowed());
    EXPECT_FALSE(a.totals(0).uniqueKnown);
    EXPECT_TRUE(a.totals(0).origKnown);
  }
  ReadFootprint::setRangeByteCapForTests(0);
  EXPECT_EQ(ReadFootprint::rangeBytesTotal(), base) << "everything given back";
}

TEST(ReadFootprintRanges, AForkedChildCountsFromZeroAndNeverBelowIt) {
  // A child's total starts at zero, and a footprint it inherited is not taken
  // off that total for what it held in the parent: counted afresh the next
  // time its ranges change, from what it holds then.
  ReadFootprint* inherited = new ReadFootprint;
  for (uint64_t i = 0; i < 3000; ++i)
    inherited->note(i * 10, 5);
  ASSERT_GT(ReadFootprint::rangeBytesTotal(), 0u);
  ReadFootprint::afterForkChild(); // as the plugin's fork handler does, in the child
  EXPECT_EQ(ReadFootprint::rangeBytesTotal(), 0u);
  inherited->freeze(); // shrinks: nothing to take off a total it is not in
  const uint64_t frozen = ReadFootprint::rangeBytesTotal();
  EXPECT_GT(frozen, 0u) << "counted now, at what it holds";
  EXPECT_LT(frozen, 3000u * 16);
  inherited->poison(); // drops its ranges
  EXPECT_EQ(ReadFootprint::rangeBytesTotal(), 0u);
  {
    ReadFootprint fresh;
    fresh.note(0, 5);
    EXPECT_GT(ReadFootprint::rangeBytesTotal(), 0u);
  }
  EXPECT_EQ(ReadFootprint::rangeBytesTotal(), 0u);
  ReadFootprint* untouched = new ReadFootprint;
  for (uint64_t i = 0; i < 100; ++i)
    untouched->note(i * 10, 5);
  const uint64_t now = ReadFootprint::rangeBytesTotal();
  ReadFootprint::afterForkChild();
  delete untouched; // a parent's footprint destroyed in the child subtracts nothing
  EXPECT_EQ(ReadFootprint::rangeBytesTotal(), 0u);
  (void)now;
  delete inherited;
  EXPECT_EQ(ReadFootprint::rangeBytesTotal(), 0u);
}

TEST(ReadFootprintRanges, ALearnedSizeClipsTheRequestsThatComeAfter) {
  // A relayed file is read with no size at hand; a reader that opens it again
  // and again asks past its end more often than the footprint can follow.
  // Once the size is learned, those requests are clipped as they come.
  const uint64_t size = 1 << 20;
  ReadFootprint f;
  f.note(size - 100, 200); // past the end, of unknown size: followed
  f.learnSize(size);
  for (uint64_t i = 0; i < 3 * ReadFootprint::kTail; ++i)
    f.note(size - 100 + i, 200); // each past the end
  f.note(0, 1000);
  const auto t = f.totals(size);
  ASSERT_TRUE(t.origKnown);
  uint64_t want = 100 + 1000;
  for (uint64_t i = 0; i < 3 * ReadFootprint::kTail; ++i)
    want += 100 - i;
  EXPECT_EQ(t.origBytes, want);
  EXPECT_EQ(t.uniqueBytes, 100u + 1000);
  ReadFootprint g; // without it: unknown, as before
  for (uint64_t i = 0; i < 3 * ReadFootprint::kTail; ++i)
    g.note(size - 100 + i, 200);
  EXPECT_FALSE(g.totals(size).origKnown);
}

// ---------------------------------------------------------- mapped reads --
//
// A read of a rewritten layout reaches the footprint as two lists: the
// original units it touched, whole, for the buckets, and the original bytes it
// carried exactly, for the counts -- or no second list at all, when the read
// covered part of a relocated basket whose bytes have no original offset.

TEST(ReadFootprintMapped, UnitsSignAndExactBytesCount) {
  // One read over part of a relocated unit [3 MiB, +600 KiB) and 4 KiB of
  // bytes in place, whose exact bytes the map could name: the buckets take the
  // unit whole, exactly as note() over it would; the counts take the exact
  // bytes only.
  const uint64_t size = 8 * kB;
  ReadFootprint mapped, wholeUnits;
  const std::vector<ReadFootprint::Span> units{{3 * kB, 600 << 10}, {100, 4096}};
  const std::vector<ReadFootprint::Span> exact{{3 * kB, 600 << 10}, {100, 4096}};
  mapped.noteMapped(units, &exact, size);
  for (const auto& [o, n] : units)
    wholeUnits.note(o, n, size);
  EXPECT_EQ(mapped.sig(size), wholeUnits.sig(size));
  EXPECT_EQ(mapped.buckets(size), wholeUnits.buckets(size));
  const auto t = mapped.totals(size);
  ASSERT_TRUE(t.origKnown);
  ASSERT_TRUE(t.uniqueKnown);
  EXPECT_EQ(t.origBytes, (600u << 10) + 4096u);
  EXPECT_EQ(t.uniqueBytes, (600u << 10) + 4096u);
}

TEST(ReadFootprintMapped, AReadTheMapCannotNameLeavesTheCountsUnknownAndTheSignatureAlone) {
  // The buckets are the identity of the work and must not change; the counts
  // would be a guess, so there are none -- for this read and every one after
  // it, since a total missing part of the reads is not the total.
  const uint64_t size = 8 * kB;
  ReadFootprint f, reference;
  f.note(0, 4096, size);
  reference.note(0, 4096, size);
  const std::vector<ReadFootprint::Span> units{{5 * kB, 300 << 10}};
  f.noteMapped(units, nullptr, size);
  reference.note(5 * kB, 300 << 10, size);
  EXPECT_FALSE(f.totals(size).origKnown);
  EXPECT_FALSE(f.totals(size).uniqueKnown);
  Ranges r;
  EXPECT_FALSE(f.ranges(size, r));
  EXPECT_FALSE(f.poisoned()) << "the signature still stands";
  EXPECT_EQ(f.sig(size), reference.sig(size));
  EXPECT_EQ(f.count(size), reference.count(size));

  // Sticky: later reads the map can name, and plain reads, change nothing.
  const std::vector<ReadFootprint::Span> named{{6 * kB, 4096}};
  f.noteMapped(named, &named, size);
  f.note(7 * kB, 4096, size);
  reference.note(6 * kB, 4096, size);
  reference.note(7 * kB, 4096, size);
  EXPECT_FALSE(f.totals(size).origKnown);
  EXPECT_FALSE(f.totals(size).uniqueKnown);
  EXPECT_EQ(f.sig(size), reference.sig(size));
  f.freeze(); // nothing to keep
  EXPECT_FALSE(f.totals(size).uniqueKnown);
  EXPECT_EQ(f.sig(size), reference.sig(size));
}

TEST(ReadFootprintMapped, UnknownBytesGiveTheirRangeMemoryBack) {
  const uint64_t base = ReadFootprint::rangeBytesTotal();
  {
    ReadFootprint f;
    for (uint64_t i = 0; i < 1000; ++i)
      f.note(i * 8192, 4096, 1ull << 30);
    ASSERT_GT(ReadFootprint::rangeBytesTotal(), base);
    f.bytesUnknown();
    EXPECT_EQ(ReadFootprint::rangeBytesTotal(), base);
    f.note(1ull << 29, 4096, 1ull << 30);
    EXPECT_EQ(ReadFootprint::rangeBytesTotal(), base) << "nothing is kept for a count not reported";
  }
  EXPECT_EQ(ReadFootprint::rangeBytesTotal(), base);
}

TEST(ReadFootprintMapped, PoisonStillWinsOverKnownBytes) {
  ReadFootprint f;
  const std::vector<ReadFootprint::Span> s{{0, 4096}};
  f.noteMapped(s, &s, 1 << 20);
  f.poison();
  EXPECT_TRUE(f.sig(1 << 20).empty());
  EXPECT_FALSE(f.totals(1 << 20).origKnown);
}

namespace {
// A deterministic stand-in for one file of a columnar TTree pass over NanoAOD:
// `n` disjoint ranges, ascending, sized from 256 B to 144 KiB and spaced as
// such a pass reads them -- most gaps between 4 KiB and 1 MiB, some below, a
// tail up to 16 MiB (all drawn log-uniform within their band).
std::vector<std::pair<uint64_t, uint64_t>> columnarReads(uint64_t seed, size_t n) {
  uint64_t s = seed * 0x9e3779b97f4a7c15ull + 1;
  auto unit = [&s] {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<double>(s >> 40) / static_cast<double>(1ull << 24);
  };
  auto logUniform = [&unit](double lo, double hi) {
    return static_cast<uint64_t>(lo * std::pow(hi / lo, unit()));
  };
  std::vector<std::pair<uint64_t, uint64_t>> out;
  out.reserve(n);
  uint64_t at = 1 << 20; // past the file's own records
  for (size_t i = 0; i < n; ++i) {
    const double band = unit();
    const uint64_t gap = band < 0.137   ? logUniform(256, 4096)
                         : band < 0.545 ? logUniform(4096, 65536)
                         : band < 0.935 ? logUniform(65536, 1 << 20)
                                        : logUniform(1 << 20, 16 << 20);
    const uint64_t len = logUniform(256, 144 << 10);
    at += gap + 1;
    out.emplace_back(at, len);
    at += len;
  }
  return out;
}

// One job's pass over `files` files: each opened once for its own records and
// closed, then read (`reads`) with at most `open` files open at once. The
// file's tail is read either first, as ROOT reads the keys list at open, or
// not at all, so every data range extends the set at its end. `freezeAtClose`
// freezes each file as the cache does when its entry goes. Returns the peak
// of the process total over the pass and how many files lost their count.
struct PassResult {
  uint64_t peak = 0;
  size_t unknown = 0;
};
PassResult columnarPass(size_t files, size_t reads, size_t open, bool tailFirst,
                        bool freezeAtClose) {
  const uint64_t base = ReadFootprint::rangeBytesTotal();
  std::vector<std::unique_ptr<ReadFootprint>> fps;
  std::vector<uint64_t> sizes;
  std::deque<ReadFootprint*> live;
  PassResult r;
  for (size_t i = 0; i < files; ++i) {
    const auto rs = columnarReads(i, reads);
    const uint64_t size = rs.back().first + rs.back().second + 4096;
    fps.push_back(std::make_unique<ReadFootprint>());
    sizes.push_back(size);
    ReadFootprint& f = *fps.back();
    f.note(0, 300, size);
    if (tailFirst)
      f.note(size - 3000, 3000, size);
    if (freezeAtClose)
      f.freeze();
    for (const auto& [off, len] : rs)
      f.note(off, len, size);
    live.push_back(&f);
    if (live.size() > open) {
      if (freezeAtClose)
        live.front()->freeze();
      live.pop_front();
    }
    r.peak = std::max(r.peak, ReadFootprint::rangeBytesTotal() - base);
  }
  for (size_t i = 0; i < files; ++i)
    r.unknown += fps[i]->totals(sizes[i]).uniqueKnown ? 0 : 1;
  return r;
}
} // namespace

TEST(ReadFootprintRanges, AFullDatasetPassFitsWhenEachClosedFileIsFrozen) {
  // The process bound at the scale it is set for: one job over 1453 files, a
  // TTree reader leaving about 4,500 disjoint ranges in each (the count a full
  // NanoAOD pass was measured to read), 64 files open at once. The cache
  // freezes a file's ranges when its entry goes: every file keeps its
  // distinct-byte count, and the process stays well inside the bound, in
  // either read order. The control: held open, ranges that only ever extend
  // the set at its end keep the spare room of their doubling, outgrow the
  // bound, and the files read last lose their count.
  constexpr size_t kFiles = 1453, kRanges = 4500, kOpen = 64;
  const uint64_t base = ReadFootprint::rangeBytesTotal();
  for (bool tailFirst : {true, false}) {
    const PassResult r = columnarPass(kFiles, kRanges, kOpen, tailFirst, /*freezeAtClose=*/true);
    EXPECT_EQ(r.unknown, 0u) << "tail first: " << tailFirst;
    EXPECT_LT(r.peak, ReadFootprint::kMaxRangeBytes / 2) << "tail first: " << tailFirst;
    std::printf("[ info ] %zu files x %zu ranges, frozen at close (tail %s): peak %.1f MiB of "
                "%llu MiB\n",
                kFiles, kRanges, tailFirst ? "first" : "never", r.peak / 1048576.0,
                static_cast<unsigned long long>(ReadFootprint::kMaxRangeBytes >> 20));
  }
  const PassResult open = columnarPass(kFiles, kRanges, kOpen, false, /*freezeAtClose=*/false);
  EXPECT_GT(open.unknown, 0u) << "the control: never frozen, the ranges outgrow the bound";
  EXPECT_LE(open.peak, ReadFootprint::kMaxRangeBytes) << "and the bound holds all the same";
  std::printf("[ info ] never frozen: %zu of %zu files lose their count at the bound\n",
              open.unknown, kFiles);
  EXPECT_EQ(ReadFootprint::rangeBytesTotal(), base) << "everything given back";
}
