// The slot table held a branch at a time: what it decodes on demand must be
// exactly what a full decode gives, only the branches touched are held, one
// request's branches cost one read of the stored layout, and a layout that
// cannot be read again fails loudly rather than answering.
#include "SlotTable.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

using namespace ucache::transpose;

namespace {

// The slot section as the layout blob stores it (one record per slot against
// the one before): branch delta, basket delta - 1, origSeek delta (zigzag),
// origLen, slot length (0 = plain), vSeek - previous end (zigzag).
struct W {
  std::vector<uint8_t> b;
  void varint(uint64_t v) {
    while (v >= 0x80) {
      b.push_back(static_cast<uint8_t>(v | 0x80));
      v >>= 7;
    }
    b.push_back(static_cast<uint8_t>(v));
  }
  void svarint(int64_t v) { varint((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
};

constexpr uint32_t kK100 = 300;
uint32_t plain(uint32_t o) { return static_cast<uint32_t>(uint64_t(o) * kK100 / 100); }

struct Fx {
  std::vector<FillSlot> slots;
  std::vector<uint8_t> raw; // a prefix, then the section
  size_t off = 0;
  SlotTable::Geometry g;
};

// `nb` branches of `per` baskets each, branch-major, the original baskets of
// each cluster interleaved across branches (as ROOT writes them); one capped
// slot and one gap to exercise the explicit encodings.
Fx fixture(uint32_t nb = 20, uint32_t per = 50, unsigned seed = 7) {
  Fx fx;
  std::mt19937 rnd(seed);
  fx.g.slotsBegin = 1 << 20;
  fx.g.originSize = 1ull << 34;
  fx.g.slotFactor100 = kK100;
  uint64_t at = fx.g.slotsBegin;
  for (uint32_t b = 0; b < nb; ++b)
    for (uint32_t k = 0; k < per; ++k) {
      FillSlot s;
      s.branch = 3 + 2 * b; // relocated branches need not be consecutive
      s.basket = k;
      s.origLen = 1000 + rnd() % 30000;
      s.origSeek = 4096 + (uint64_t(k) * nb + b) * 40000 + rnd() % 5000;
      s.vLen = (b == 4 && k == 7) ? s.origLen + 17 : plain(s.origLen); // one explicit length
      if (b == 9 && k == 0)
        at += 4096; // one gap before a run
      s.vSeek = at;
      at += s.vLen;
      fx.slots.push_back(s);
    }
  fx.g.virtualSize = at;
  W w;
  w.b.assign(37, 0xAB); // whatever precedes the section in the blob
  fx.off = w.b.size();
  uint64_t prevOrig = 0, nextV = fx.g.slotsBegin;
  uint32_t prevBranch = 0, prevBasket = 0;
  for (const auto& s : fx.slots) {
    w.svarint(int64_t(s.branch) - int64_t(prevBranch));
    w.svarint(int64_t(s.basket) - int64_t(prevBasket) - 1);
    w.svarint(int64_t(s.origSeek - prevOrig));
    w.varint(s.origLen);
    w.varint(s.vLen == plain(s.origLen) ? 0 : uint64_t(s.vLen) + 1);
    w.svarint(int64_t(s.vSeek - nextV));
    prevBranch = s.branch;
    prevBasket = s.basket;
    prevOrig = s.origSeek;
    nextV = s.vSeek + s.vLen;
  }
  fx.raw = std::move(w.b);
  return fx;
}

bool same(const FillSlot& a, const FillSlot& b) {
  return a.branch == b.branch && a.basket == b.basket && a.origSeek == b.origSeek &&
         a.origLen == b.origLen && a.vSeek == b.vSeek && a.vLen == b.vLen;
}

struct Lazy {
  Fx fx;
  std::atomic<int> reads{0};
  bool fail = false;
  SlotTable t;
  explicit Lazy(Fx f) : fx(std::move(f)) {
    size_t at = fx.off;
    EXPECT_TRUE(t.index(fx.raw, at, static_cast<uint32_t>(fx.slots.size()), fx.g,
                        [this](std::vector<uint8_t>& raw) {
                          ++reads;
                          if (fail)
                            return false;
                          raw = fx.raw;
                          return true;
                        }));
    EXPECT_EQ(at, fx.raw.size());
  }
};

} // namespace

TEST(SlotTable, LazyDecodeIsTheFullDecode) {
  Lazy z(fixture());
  ASSERT_EQ(z.t.size(), z.fx.slots.size());
  EXPECT_EQ(z.t.decodedSlots(), 0u) << "the index holds no entries";
  EXPECT_EQ(z.reads, 0);
  for (uint32_t i = 0; i < z.t.size(); ++i)
    ASSERT_TRUE(same(z.t.at(i), z.fx.slots[i])) << i;
  EXPECT_EQ(z.t.decodedSlots(), z.fx.slots.size());
}

TEST(SlotTable, OnlyTheBranchesTouchedAreHeld) {
  Lazy z(fixture(20, 50));
  const auto& s = z.fx.slots[3 * 50 + 10]; // in the 4th branch
  uint32_t i = 0;
  ASSERT_TRUE(z.t.find(s.vSeek + 5, i));
  EXPECT_EQ(i, 3u * 50 + 10);
  EXPECT_EQ(z.t.decodedSlots(), 50u) << "one branch's run";
  EXPECT_EQ(z.reads, 1);
  ASSERT_TRUE(z.t.find(s.vSeek, i)); // decoded: no read
  EXPECT_EQ(z.reads, 1);
}

TEST(SlotTable, OneRequestsBranchesCostOneRead) {
  Lazy z(fixture(20, 50));
  std::vector<std::pair<uint64_t, uint64_t>> r;
  for (uint32_t b : {1u, 5u, 9u, 13u, 17u}) // a vector read across five branches
    for (uint32_t k : {0u, 10u, 49u}) {
      const auto& s = z.fx.slots[b * 50 + k];
      r.emplace_back(s.vSeek, s.vLen);
    }
  ASSERT_TRUE(z.t.prepareRanges(r));
  EXPECT_EQ(z.reads, 1);
  EXPECT_EQ(z.t.decodedSlots(), 5u * 50);
  for (const auto& [off, len] : r) {
    uint32_t i = 0;
    ASSERT_TRUE(z.t.find(off, i));
  }
  EXPECT_EQ(z.reads, 1) << "everything it needed was decoded at once";
}

// A reader touches its branches one request at a time at first: the stored
// layout is read once for all of them, and let go of once requests stop
// needing new branches (then a new branch reads it again).
TEST(SlotTable, BranchesTouchedOneByOneCostOneRead) {
  Lazy z(fixture(20, 50));
  auto touch = [&z](uint32_t b) {
    const auto& s = z.fx.slots[b * 50 + 3];
    return z.t.prepare(s.vSeek, s.vSeek + 1);
  };
  for (uint32_t b = 0; b < 10; ++b)
    ASSERT_TRUE(touch(b));
  EXPECT_EQ(z.reads, 1) << "ten branches, one read";
  EXPECT_EQ(z.t.decodedSlots(), 10u * 50);
  EXPECT_TRUE(z.t.holdsRaw());
  for (uint32_t n = 0; n + 1 < SlotTable::kKeepQuiet; ++n)
    ASSERT_TRUE(touch(n % 10));
  EXPECT_TRUE(z.t.holdsRaw()) << "one request short of the limit";
  ASSERT_TRUE(touch(0));
  EXPECT_FALSE(z.t.holdsRaw()) << "let go of after kKeepQuiet requests that needed nothing new";
  ASSERT_TRUE(touch(15));
  EXPECT_EQ(z.reads, 2) << "a new branch reads it again";
  for (uint32_t i = 0; i < z.t.size(); ++i)
    ASSERT_TRUE(same(z.t.at(i), z.fx.slots[i])) << i;
}

TEST(SlotTable, FindAndForEachAnswerAsTheFullTableDoes) {
  Lazy z(fixture(12, 30, 11));
  const auto& v = z.fx.slots;
  std::mt19937_64 rnd(5);
  for (int n = 0; n < 2000; ++n) {
    const uint64_t pos = z.fx.g.slotsBegin + rnd() % (z.fx.g.virtualSize - z.fx.g.slotsBegin);
    // The reference: the last slot starting at or before pos.
    auto it = std::upper_bound(v.begin(), v.end(), pos,
                               [](uint64_t p, const FillSlot& s) { return p < s.vSeek; });
    uint32_t i = 0;
    ASSERT_TRUE(z.t.find(pos, i));
    EXPECT_EQ(i, uint32_t(it - v.begin()) - 1);
    // forEach: the slots overlapping [pos, pos + span).
    const uint64_t end = pos + rnd() % 300000;
    std::vector<uint32_t> want, got;
    for (uint32_t k = 0; k < v.size(); ++k)
      if (v[k].vSeek < end && v[k].vSeek + v[k].vLen > pos)
        want.push_back(k);
    ASSERT_TRUE(z.t.forEach(pos, end, [&](uint32_t k, const FillSlot& s) {
      EXPECT_TRUE(same(s, v[k]));
      got.push_back(k);
      return true;
    }));
    EXPECT_EQ(got, want);
  }
  uint32_t i = 0;
  EXPECT_FALSE(z.t.find(z.fx.g.slotsBegin - 1, i));
  EXPECT_FALSE(z.t.find(z.fx.g.virtualSize, i));
}

TEST(SlotTable, AWalkOverEverythingKeepsNothing) {
  Lazy z(fixture(10, 40));
  uint32_t i = 0;
  ASSERT_TRUE(z.t.find(z.fx.slots[0].vSeek, i)); // one branch held
  uint64_t n = 0, bytes = 0;
  ASSERT_TRUE(z.t.forEachAll([&](uint32_t k, const FillSlot& s) {
    EXPECT_TRUE(same(s, z.fx.slots[k]));
    ++n;
    bytes += s.origLen;
  }));
  EXPECT_EQ(n, z.fx.slots.size());
  EXPECT_EQ(z.t.decodedSlots(), 40u) << "still only the branch that was touched";
}

TEST(SlotTable, DecodedSlotsByOriginalOffset) {
  Lazy z(fixture(6, 20));
  // Touch branches 1 and 2: their baskets interleave in the original file.
  ASSERT_TRUE(z.t.prepareSlots({20, 40}));
  const uint64_t a = z.fx.slots[25].origSeek, b = z.fx.slots[28].origSeek + 1;
  std::vector<uint32_t> got;
  z.t.decodedOverlappingOrig(a, b, got);
  std::vector<uint32_t> want;
  for (uint32_t k = 20; k < 60; ++k)
    if (z.fx.slots[k].origSeek < b && z.fx.slots[k].origSeek + z.fx.slots[k].origLen > a)
      want.push_back(k);
  std::sort(want.begin(), want.end(),
            [&](uint32_t x, uint32_t y) { return z.fx.slots[x].origSeek < z.fx.slots[y].origSeek; });
  EXPECT_EQ(got, want);
  EXPECT_FALSE(got.empty());
}

TEST(SlotTable, ALayoutThatCannotBeReadAgainFailsLoudly) {
  Lazy z(fixture());
  z.fail = true;
  uint32_t i = 0;
  EXPECT_FALSE(z.t.find(z.fx.slots[100].vSeek, i));
  EXPECT_FALSE(z.t.prepare(z.fx.slots[0].vSeek, z.fx.slots[0].vSeek + 10));
  EXPECT_THROW(z.t.at(5), std::runtime_error);
  EXPECT_FALSE(z.t.forEach(z.fx.slots[0].vSeek, z.fx.slots[0].vSeek + 10,
                           [](uint32_t, const FillSlot&) { return true; }));
  z.fail = false;
  EXPECT_TRUE(same(z.t.at(5), z.fx.slots[5])) << "and works again once it can";
}

TEST(SlotTable, OtherBytesAtTheRunsPlaceAreRefused) {
  Fx fx = fixture(4, 10);
  SlotTable t;
  size_t at = fx.off;
  std::vector<uint8_t> other = fixture(4, 10, 99).raw; // another layout's bytes
  ASSERT_TRUE(t.index(fx.raw, at, uint32_t(fx.slots.size()), fx.g,
                      [&other](std::vector<uint8_t>& raw) {
                        raw = other;
                        return true;
                      }));
  uint32_t i = 0;
  EXPECT_FALSE(t.find(fx.slots[15].vSeek, i));
  EXPECT_FALSE(t.holdsRaw()) << "bytes that did not decode are not kept";
}

TEST(SlotTable, AMalformedSectionIsNotIndexed) {
  Fx fx = fixture(3, 5);
  // Out of order: the third slot before the second.
  std::swap(fx.slots[1], fx.slots[2]);
  W w;
  uint64_t prevOrig = 0, nextV = fx.g.slotsBegin;
  uint32_t pb = 0, pk = 0;
  for (const auto& s : fx.slots) {
    w.svarint(int64_t(s.branch) - int64_t(pb));
    w.svarint(int64_t(s.basket) - int64_t(pk) - 1);
    w.svarint(int64_t(s.origSeek - prevOrig));
    w.varint(s.origLen);
    w.varint(uint64_t(s.vLen) + 1);
    w.svarint(int64_t(s.vSeek) - int64_t(nextV));
    pb = s.branch;
    pk = s.basket;
    prevOrig = s.origSeek;
    nextV = s.vSeek + s.vLen;
  }
  SlotTable t;
  size_t at = 0;
  EXPECT_FALSE(t.index(w.b, at, uint32_t(fx.slots.size()), fx.g, nullptr));
  EXPECT_EQ(t.size(), 0u);
  // A section shorter than its count.
  Fx f2 = fixture(2, 5);
  at = f2.off;
  std::vector<uint8_t> cut(f2.raw.begin(), f2.raw.end() - 3);
  EXPECT_FALSE(t.index(cut, at, uint32_t(f2.slots.size()), f2.g, nullptr));
}

TEST(SlotTable, AssignedTablesAnswerTheSame) {
  Fx fx = fixture(5, 8);
  SlotTable t;
  t.assign(fx.slots);
  EXPECT_EQ(t.decodedSlots(), fx.slots.size());
  uint32_t i = 0;
  ASSERT_TRUE(t.find(fx.slots[17].vSeek + 1, i));
  EXPECT_EQ(i, 17u);
  for (uint32_t k = 0; k < fx.slots.size(); ++k)
    EXPECT_TRUE(same(t.at(k), fx.slots[k]));
}

TEST(SlotTable, ManyThreadsDecodeEachRunOnce) {
  Lazy z(fixture(40, 25));
  std::vector<std::thread> th;
  std::atomic<int> bad{0};
  for (int n = 0; n < 16; ++n)
    th.emplace_back([&, n] {
      std::mt19937 rnd(n);
      for (int k = 0; k < 2000; ++k) {
        const uint32_t i = rnd() % z.t.size();
        if (!same(z.t.at(i), z.fx.slots[i]))
          ++bad;
        uint32_t j = 0;
        if (!z.t.find(z.fx.slots[i].vSeek, j) || j != i)
          ++bad;
      }
    });
  for (auto& t : th)
    t.join();
  EXPECT_EQ(bad, 0);
  EXPECT_EQ(z.t.decodedSlots(), z.fx.slots.size()) << "each run once";
}
