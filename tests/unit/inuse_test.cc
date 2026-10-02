#include "InUse.h"

#include "TestUtil.h"
#include <fcntl.h>
#include <fstream>
#include <gtest/gtest.h>
#include <sys/file.h>
#include <thread>
#include <unistd.h>

using namespace ucache;
using test::TempDir;

namespace {
const std::string kHash = "ab0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcd";
InUseRange rng(uint64_t store, uint64_t lo, uint64_t hi, uint64_t lastS, uint64_t holdS = 0,
               uint64_t pid = 0) {
  InUseRange r;
  r.storeId = store;
  r.lo = lo;
  r.hi = hi;
  r.lastS = lastS;
  r.holdS = holdS;
  r.pid = pid;
  return r;
}
} // namespace

TEST(InUse, NoRecordReadsAsEmpty) {
  TempDir t;
  RealIO io;
  InUseRecord r;
  ASSERT_TRUE(InUseRecord::load(io, InUseRecord::path(t.path(), kHash), r));
  EXPECT_TRUE(r.ranges.empty());
  EXPECT_FALSE(r.hasSettings);
  EXPECT_EQ(r.lastUseS(), 0u);
  EXPECT_TRUE(r.expired(1000, 86400));
}

TEST(InUse, NotesMergeAndTheFirstSettingsStay) {
  TempDir t;
  RealIO io;
  const std::string p = InUseRecord::path(t.path(), kHash);
  EXPECT_EQ(p, t.path() + "/inuse/ab/" + kHash);
  InUseSettings s1{3, 300, "lzma,zlib"}, s2{4, 300, "lzma"};
  ASSERT_TRUE(InUseRecord::note(io, p, rng(7, 100, 200, 1000), &s1, 86400));
  ASSERT_TRUE(InUseRecord::note(io, p, rng(7, 100, 200, 1500), &s2, 86400)); // same map: newer time
  ASSERT_TRUE(InUseRecord::note(io, p, rng(7, 500, 900, 1200), nullptr, 86400));
  InUseRecord r;
  ASSERT_TRUE(InUseRecord::load(io, p, r));
  ASSERT_EQ(r.ranges.size(), 2u);
  EXPECT_EQ(r.ranges[0].lastS, 1500u);
  EXPECT_EQ(r.highWater, 900u);
  ASSERT_TRUE(r.hasSettings);
  EXPECT_EQ(r.settings.layoutVersion, 3u); // the settings it was first shown with
  EXPECT_EQ(r.settings.codecs, "lzma,zlib");
  EXPECT_EQ(r.lastUseS(), 1500u);
}

TEST(InUse, AMapHandedOutStaysInUseForTheWindow) {
  InUseRecord r;
  r.ranges.push_back(rng(7, 100, 200, 1000));
  EXPECT_TRUE(r.inUse(7, 150, 160, 1000 + 3599, 3600));
  EXPECT_FALSE(r.inUse(7, 150, 160, 1000 + 3600, 3600));
  EXPECT_TRUE(r.inUse(7, 0, 101, 2000, 3600));    // overlaps its first byte
  EXPECT_FALSE(r.inUse(7, 200, 300, 2000, 3600)); // starts at its end
  EXPECT_FALSE(r.inUse(8, 100, 200, 2000, 3600)); // another store's addresses
  EXPECT_FALSE(r.expired(4599, 3600));
  EXPECT_TRUE(r.expired(4600, 3600));
}

TEST(InUse, AHeldMapIsInUseWhateverTheWindow) {
  InUseRecord r;
  r.ranges.push_back(rng(7, 100, 200, 0, 5000, 42));
  const uint64_t grace = 2 * InUseRecord::kHoldRefreshS;
  for (uint64_t w : {uint64_t{0}, uint64_t{60}}) {
    EXPECT_TRUE(r.inUse(7, 100, 200, 5000 + grace - 1, w)) << w;
    EXPECT_FALSE(r.inUse(7, 100, 200, 5000 + grace, w)) << w;
  }
  // Released (a close) with a window of 0: no longer in use once nothing holds it.
  InUseRecord z;
  z.ranges.push_back(rng(7, 100, 200, 9000));
  EXPECT_FALSE(z.inUse(7, 100, 200, 9000, 0));
}

TEST(InUse, EachProcessHoldsAndReleasesItsOwn) {
  // Two processes are handed the same map; one closes: the other's hold keeps
  // it in use with a window of 0. When the second closes too, nothing does.
  TempDir t;
  RealIO io;
  const std::string p = InUseRecord::path(t.path(), kHash);
  ASSERT_TRUE(InUseRecord::note(io, p, rng(7, 100, 200, 1000, 1000, 11), nullptr, 0));
  ASSERT_TRUE(InUseRecord::note(io, p, rng(7, 100, 200, 1001, 1001, 22), nullptr, 0));
  ASSERT_TRUE(InUseRecord::note(io, p, rng(7, 100, 200, 1010, 0, 11), nullptr, 0, /*release=*/true));
  InUseRecord r;
  ASSERT_TRUE(InUseRecord::load(io, p, r));
  EXPECT_TRUE(r.inUse(7, 100, 200, 1010, 0)); // still held by 22
  ASSERT_TRUE(InUseRecord::note(io, p, rng(7, 100, 200, 1020, 0, 22), nullptr, 0, /*release=*/true));
  ASSERT_TRUE(InUseRecord::load(io, p, r));
  EXPECT_FALSE(r.inUse(7, 100, 200, 1020, 0)); // released by both
  // With a window, it counts from the last close.
  const std::string q = InUseRecord::path(t.path(), "cd" + kHash.substr(2));
  ASSERT_TRUE(InUseRecord::note(io, q, rng(7, 100, 200, 1000, 1000, 11), nullptr, 60));
  ASSERT_TRUE(InUseRecord::note(io, q, rng(7, 100, 200, 1020, 0, 11), nullptr, 60, true));
  ASSERT_TRUE(InUseRecord::load(io, q, r));
  EXPECT_TRUE(r.inUse(7, 100, 200, 1020 + 59, 60));
  EXPECT_FALSE(r.inUse(7, 100, 200, 1020 + 60, 60));
  // A refresh keeps a hold; a holder that stopped refreshing lets go.
  ASSERT_TRUE(InUseRecord::note(io, p, rng(7, 100, 200, 0, 2000, 33), nullptr, 0));
  ASSERT_TRUE(InUseRecord::load(io, p, r));
  EXPECT_TRUE(r.inUse(7, 100, 200, 2000 + 2 * InUseRecord::kHoldRefreshS - 1, 0));
  EXPECT_FALSE(r.inUse(7, 100, 200, 2000 + 2 * InUseRecord::kHoldRefreshS, 0));
}

TEST(InUse, ANoteDropsWhatIsNoLongerInUse) {
  TempDir t;
  RealIO io;
  const std::string p = InUseRecord::path(t.path(), kHash);
  ASSERT_TRUE(InUseRecord::note(io, p, rng(7, 100, 200, 1000), nullptr, 3600));
  ASSERT_TRUE(InUseRecord::note(io, p, rng(7, 300, 400, 1000 + 3600), nullptr, 3600));
  InUseRecord r;
  ASSERT_TRUE(InUseRecord::load(io, p, r));
  ASSERT_EQ(r.ranges.size(), 1u);
  EXPECT_EQ(r.ranges[0].lo, 300u);
  EXPECT_EQ(r.highWater, 400u); // the high-water mark is kept as long as the record
}

TEST(InUse, LinesThatDoNotParseAreSkipped) {
  InUseRecord r = InUseRecord::parse("ucache-inuse 1\n"
                                     "range 0000000000000007 100 200 1000 0 0\n"
                                     "range zz 1 2 3 0 0\n"
                                     "range 0000000000000007 300 200 1000 0 0\n" // end before start
                                     "range 7 1 2\n"                           // short
                                     "something else\n"
                                     "high 5000\n"
                                     "settings 3 300 -\n");
  ASSERT_EQ(r.ranges.size(), 1u);
  EXPECT_EQ(r.highWater, 5000u);
  ASSERT_TRUE(r.hasSettings);
  EXPECT_TRUE(r.settings.codecs.empty());
  EXPECT_TRUE(InUseRecord::parse("ucache-inuse 2\nhigh 9\n").ranges.empty());
  EXPECT_EQ(InUseRecord::parse("ucache-inuse 2\nhigh 9\n").highWater, 0u); // unknown version
  // What it writes, it reads.
  InUseRecord back = InUseRecord::parse(r.serialize());
  EXPECT_EQ(back.serialize(), r.serialize());
}

TEST(InUse, SweepRemovesOnlyExpiredRecordsAndSkipsALockedOne) {
  TempDir t;
  RealIO io;
  const std::string a = InUseRecord::path(t.path(), kHash);
  const std::string b = InUseRecord::path(t.path(), "cd" + kHash.substr(2));
  const std::string c = InUseRecord::path(t.path(), "ef" + kHash.substr(2));
  ASSERT_TRUE(InUseRecord::note(io, a, rng(1, 0, 10, 1000), nullptr, 3600));
  ASSERT_TRUE(InUseRecord::note(io, b, rng(1, 0, 10, 9000), nullptr, 3600));
  ASSERT_TRUE(InUseRecord::note(io, c, rng(1, 0, 10, 1000), nullptr, 3600));
  const int held = ::open(c.c_str(), O_RDWR);
  ASSERT_GE(held, 0);
  ASSERT_EQ(::flock(held, LOCK_EX), 0); // a process noting a hand-out right now
  EXPECT_EQ(InUseRecord::sweep(io, t.path(), 10000, 3600), 1);
  struct ::stat st {};
  EXPECT_NE(::stat(a.c_str(), &st), 0); // expired: gone
  EXPECT_EQ(::stat(b.c_str(), &st), 0); // in use: kept
  EXPECT_EQ(::stat(c.c_str(), &st), 0); // locked: left for the next sweep
  ::close(held);
  EXPECT_EQ(InUseRecord::sweep(io, t.path(), 10000, 3600), 1);
}

TEST(InUse, ConcurrentNotesAreAllKept) {
  TempDir t;
  RealIO io;
  const std::string p = InUseRecord::path(t.path(), kHash);
  std::vector<std::thread> ts;
  for (int k = 0; k < 8; ++k)
    ts.emplace_back([&, k] {
      for (int i = 0; i < 25; ++i)
        EXPECT_TRUE(InUseRecord::note(io, p, rng(1, (k * 25 + i) * 10, (k * 25 + i) * 10 + 5, 1000),
                                      nullptr, 86400));
    });
  for (auto& th : ts)
    th.join();
  InUseRecord r;
  ASSERT_TRUE(InUseRecord::load(io, p, r));
  EXPECT_EQ(r.ranges.size(), 200u);
  EXPECT_EQ(r.highWater, 1995u);
}
