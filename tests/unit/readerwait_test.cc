#include "ReaderWait.h"

#include <gtest/gtest.h>
#include <thread>

using namespace ucache;

TEST(ReaderWait, OneRequestIsItsLatency) {
  ReaderWait::Thread t;
  ReaderWait::Tier tier = ReaderWait::kOrigin;
  ReaderWait::begin(t, 100);
  EXPECT_EQ(ReaderWait::end(t, 350, ReaderWait::kByte, tier), 250u);
  EXPECT_EQ(tier, ReaderWait::kByte);
}

TEST(ReaderWait, OverlappingRequestsOfOneThreadCountOnce) {
  // ROOT issues several vector reads for one fill and waits for all of them:
  // the thread waited 300 us, not 300 + 50.
  ReaderWait::Thread t;
  ReaderWait::Tier tier = ReaderWait::kByte;
  ReaderWait::begin(t, 100);
  ReaderWait::begin(t, 150);
  EXPECT_EQ(ReaderWait::end(t, 200, ReaderWait::kOrigin, tier), 0u);
  EXPECT_EQ(ReaderWait::end(t, 400, ReaderWait::kByte, tier), 300u);
  EXPECT_EQ(tier, ReaderWait::kOrigin); // the costliest tier of the interval
}

TEST(ReaderWait, EachIntervalStartsWithTheCheapestTier) {
  ReaderWait::Thread t;
  ReaderWait::Tier tier = ReaderWait::kByte;
  ReaderWait::begin(t, 0);
  EXPECT_EQ(ReaderWait::end(t, 10, ReaderWait::kSlots, tier), 10u);
  EXPECT_EQ(tier, ReaderWait::kSlots);
  ReaderWait::begin(t, 20);
  EXPECT_EQ(ReaderWait::end(t, 25, ReaderWait::kReplica, tier), 5u);
  EXPECT_EQ(tier, ReaderWait::kReplica);
}

TEST(ReaderWait, ARefusedRequestChargesNoTier) {
  ReaderWait::Thread t;
  ReaderWait::Tier tier = ReaderWait::kOrigin;
  ReaderWait::begin(t, 100);
  ReaderWait::begin(t, 110); // refused at once
  EXPECT_EQ(ReaderWait::cancel(t, 111, tier), 0u);
  EXPECT_EQ(ReaderWait::end(t, 200, ReaderWait::kReplica, tier), 100u);
  EXPECT_EQ(tier, ReaderWait::kReplica);
  ReaderWait::begin(t, 300);
  EXPECT_EQ(ReaderWait::cancel(t, 302, tier), 2u);
  EXPECT_EQ(tier, ReaderWait::kByte);
}

TEST(ReaderWait, AnAnswerWithNoRequestOutstandingIsIgnored) {
  ReaderWait::Thread t;
  ReaderWait::Tier tier = ReaderWait::kByte;
  EXPECT_EQ(ReaderWait::end(t, 50, ReaderWait::kOrigin, tier), 0u);
  ReaderWait::begin(t, 60);
  EXPECT_EQ(ReaderWait::end(t, 70, ReaderWait::kByte, tier), 10u);
  EXPECT_EQ(tier, ReaderWait::kByte); // the stray answer left no tier behind
}

TEST(ReaderWait, OneRecordPerThreadAndForkGeneration) {
  bool created = false;
  auto a = ReaderWait::current(7, created);
  EXPECT_TRUE(created);
  auto b = ReaderWait::current(7, created);
  EXPECT_FALSE(created);
  EXPECT_EQ(a.get(), b.get());
  std::shared_ptr<ReaderWait::Thread> other;
  std::thread([&] {
    bool c = false;
    other = ReaderWait::current(7, c);
    EXPECT_TRUE(c);
  }).join();
  EXPECT_NE(other.get(), a.get());
  // A forked child's thread starts over: the parent's requests never answer there.
  auto c = ReaderWait::current(8, created);
  EXPECT_TRUE(created);
  EXPECT_NE(c.get(), a.get());
}

TEST(ReaderWait, AnswersFromAnotherThreadCloseTheInterval) {
  ReaderWait::Thread t;
  ReaderWait::begin(t, 1000);
  ReaderWait::begin(t, 1001);
  uint64_t got[2] = {0, 0};
  std::thread a([&] {
    ReaderWait::Tier x;
    got[0] = ReaderWait::end(t, 1500, ReaderWait::kSlots, x);
  });
  a.join();
  std::thread b([&] {
    ReaderWait::Tier x;
    got[1] = ReaderWait::end(t, 2000, ReaderWait::kByte, x);
  });
  b.join();
  EXPECT_EQ(got[0] + got[1], 1000u);
}
