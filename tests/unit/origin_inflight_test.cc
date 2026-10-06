// The origin-in-flight guard. It is a plain RAII counter over Stats, with no
// XrdCl in it, so its contract is proven here rather than through the plugin.
//
// What it is FOR: how many reads the origin was being asked for at once, which
// is the only width that sets the origin's delivery rate. It therefore has to
// span the READ -- issue to completion -- and not the call that issued it, and
// not the lifetime of whatever object happens to hold it.
#include "OriginInFlight.h"

#include "Executor.h"

#include <gtest/gtest.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using ucache::OriginInFlight;
using ucache::Stats;

TEST(OriginInFlight, CountsUpAtIssueAndDownAtRelease) {
  Stats s;
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
  {
    OriginInFlight g(&s);
    EXPECT_EQ(s.originReadsInFlight.load(), 1u);
    EXPECT_EQ(s.originReadsInFlightHighWater.load(), 1u);
  }
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
  // The high-water is the point of the counter and does not decay.
  EXPECT_EQ(s.originReadsInFlightHighWater.load(), 1u);
}

TEST(OriginInFlight, ReleaseIsWhereTheReadEndsNotWhereTheObjectDies) {
  // The property the read path depends on. A completion handler releases at
  // the TOP, then forwards to the caller; a caller that issues its next read
  // from inside its own handler must not be counted alongside the read that
  // has already landed. Holding until destruction reports two where the origin
  // had one, and the count becomes an artefact of handler lifetimes.
  Stats s;
  OriginInFlight first(&s);
  EXPECT_EQ(s.originReadsInFlight.load(), 1u);
  first.release(); // the read landed; the handler has not been destroyed yet
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
  {
    OriginInFlight next(&s); // what the caller starts from inside its handler
    EXPECT_EQ(s.originReadsInFlight.load(), 1u);
  }
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
  // One read was outstanding at a time, and that is what is reported.
  EXPECT_EQ(s.originReadsInFlightHighWater.load(), 1u);
}

TEST(OriginInFlight, ReleasingTwiceGivesBackOnlyOne) {
  // release() then ~OriginInFlight() is the ordinary path once the handler is
  // destroyed, so a second give-back must be a no-op. Underflowing an unsigned
  // counter here would report about eighteen quintillion reads in flight.
  Stats s;
  {
    OriginInFlight g(&s);
    g.release();
    g.release();
    EXPECT_EQ(s.originReadsInFlight.load(), 0u);
  }
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
}

TEST(OriginInFlight, MovingHandsOverTheOneRead) {
  // The guard is handed into the handler that owns the read, so a move must
  // transfer the debt rather than duplicate or drop it.
  Stats s;
  OriginInFlight a(&s);
  EXPECT_EQ(s.originReadsInFlight.load(), 1u);
  OriginInFlight b(std::move(a));
  EXPECT_EQ(s.originReadsInFlight.load(), 1u) << "the move is not a second read";
  {
    OriginInFlight c;
    c = std::move(b);
    EXPECT_EQ(s.originReadsInFlight.load(), 1u);
  }
  EXPECT_EQ(s.originReadsInFlight.load(), 0u) << "the destination owns it now";
}

TEST(OriginInFlight, MoveAssigningOverALiveGuardGivesThatOneBack) {
  // relayToInner assigns a constructed guard over a default-constructed member.
  // If the destination were ever live, dropping it silently would leak a read
  // that never comes back and pin the counter above zero for the process.
  Stats s;
  OriginInFlight dst(&s);
  OriginInFlight src(&s);
  EXPECT_EQ(s.originReadsInFlight.load(), 2u);
  dst = std::move(src);
  EXPECT_EQ(s.originReadsInFlight.load(), 1u) << "the overwritten guard was given back";
}

TEST(OriginInFlight, NoStatsIsNotACrash) {
  // Handles built before a store exists carry a null Stats; every route
  // constructs the guard unconditionally.
  OriginInFlight g(nullptr);
  g.release();
  OriginInFlight d;
  d.release();
  SUCCEED();
}

TEST(OriginInFlight, HighWaterIsTheConcurrentPeakNotATotal) {
  // Sequential reads are the discriminating case: ten of them one after
  // another are ten reads and a width of ONE, and a counter that failed to
  // give each back would report ten.
  Stats s;
  for (int i = 0; i < 10; ++i)
    OriginInFlight g(&s);
  EXPECT_EQ(s.originReadsInFlightHighWater.load(), 1u);
  {
    OriginInFlight a(&s), b(&s), c(&s);
    EXPECT_EQ(s.originReadsInFlightHighWater.load(), 3u);
  }
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
  EXPECT_EQ(s.originReadsInFlightHighWater.load(), 3u);
}

// ---- the count per server ----------------------------------------------------
// The client keeps one connection per host:port in a process, so the reads in
// flight to one server are the reads on its connection. Each test names its
// own servers: the counts are process-wide.

uint64_t onServer(const char* server) {
  return ucache::serverInFlight(server)->load();
}

TEST(OriginInFlightPerServer, RisesAtIssueAndKeepsTheCountItWasIssuedAt) {
  Stats s;
  OriginInFlight a(&s, "rise-a:1094");
  OriginInFlight b(&s, "rise-a:1094");
  OriginInFlight c(&s, "rise-b:1094");
  EXPECT_EQ(onServer("rise-a:1094"), 2u);
  EXPECT_EQ(onServer("rise-b:1094"), 1u);
  EXPECT_EQ(a.nc, 1u);
  EXPECT_EQ(b.nc, 2u) << "itself included";
  EXPECT_EQ(c.nc, 1u) << "another server's reads are not this one's";
  EXPECT_EQ(s.originReadsInFlight.load(), 3u) << "the process-wide count is unchanged";
  b.release();
  EXPECT_EQ(onServer("rise-a:1094"), 1u);
  EXPECT_EQ(b.nc, 2u) << "kept after release, for the per-file record and the trace";
}

TEST(OriginInFlightPerServer, BackToZeroOnCompletion) {
  // A completion handler gives the read back at its top (release), then is
  // destroyed: one read, given back once.
  Stats s;
  OriginInFlight g(&s, "done:1094");
  EXPECT_EQ(onServer("done:1094"), 1u);
  g.release();
  EXPECT_EQ(onServer("done:1094"), 0u);
  g.release();
  EXPECT_EQ(onServer("done:1094"), 0u) << "a second release gives nothing back";
}

TEST(OriginInFlightPerServer, BackToZeroOnAFailedIssue) {
  // A request the client refuses at once: its handler is deleted unrun.
  Stats s;
  {
    OriginInFlight g(&s, "refused:1094");
  }
  EXPECT_EQ(onServer("refused:1094"), 0u);
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
}

TEST(OriginInFlightPerServer, AMoveHandsOverTheOneRead) {
  Stats s;
  OriginInFlight a(&s, "move:1094");
  OriginInFlight b(std::move(a));
  EXPECT_EQ(onServer("move:1094"), 1u) << "the move is not a second read";
  EXPECT_EQ(b.nc, 1u);
  a.release(); // the moved-from guard owns nothing
  EXPECT_EQ(onServer("move:1094"), 1u);
  OriginInFlight live(&s, "move-over:1094");
  live = std::move(b); // assigning over a live guard gives that one back
  EXPECT_EQ(onServer("move-over:1094"), 0u);
  EXPECT_EQ(onServer("move:1094"), 1u);
  live.release();
  EXPECT_EQ(onServer("move:1094"), 0u);
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
}

TEST(OriginInFlightPerServer, AnUnknownServerOrNoStatsCountsNothingPerServer) {
  Stats s;
  OriginInFlight unnamed(&s); // the handle knew no server at the issue
  EXPECT_EQ(unnamed.nc, 0u);
  EXPECT_EQ(s.originReadsInFlight.load(), 1u) << "still one of the process's reads";
  EXPECT_EQ(ucache::serverInFlight(""), nullptr);
  OriginInFlight nostats(nullptr, "nostats:1094");
  EXPECT_EQ(nostats.nc, 0u);
  EXPECT_EQ(onServer("nostats:1094"), 0u);
}

TEST(OriginInFlightPerServer, AServerIsNamedWithoutItsUserName) {
  // The client's name for a file's data server can carry the URL's user name;
  // the count must not split one server in two by it.
  EXPECT_EQ(ucache::serverOfHostId("alice@host.example:1094"), "host.example:1094");
  EXPECT_EQ(ucache::serverOfHostId("host.example:1094"), "host.example:1094");
  EXPECT_EQ(ucache::serverOfHostId("a@b@[::1]:1094"), "[::1]:1094");
  EXPECT_EQ(ucache::serverOfHostId(""), "");
}

TEST(OriginInFlightPerServer, KeepsItsIssueTime) {
  Stats s;
  OriginInFlight a(&s, "time:1094");
  OriginInFlight b(&s, "time:1094");
  EXPECT_GT(a.t0, 0u);
  EXPECT_LE(a.t0, b.t0);
  const uint64_t t0 = a.t0;
  OriginInFlight moved(std::move(a));
  EXPECT_EQ(moved.t0, t0);
}

// Skipped under the sanitizers, as executor_test's fork tests are: a forked
// child of a process with threads can wait for good on the sanitizer's own
// locks.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define UCACHE_FORK_SKIP 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define UCACHE_FORK_SKIP 1
#endif
#endif

TEST(OriginInFlightPerServer, AForkedChildStartsFromNoReads) {
#ifdef UCACHE_FORK_SKIP
  GTEST_SKIP() << "fork with threads under a sanitizer";
#endif
  // The parent's reads are not the child's: the client drops them in the
  // child, so they would never be given back there.
  Stats s;
  OriginInFlight parentRead(&s, "fork:1094");
  ASSERT_EQ(onServer("fork:1094"), 1u);
  int p[2];
  ASSERT_EQ(::pipe(p), 0);
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(p[0]);
    ucache::Executor::afterForkChild(); // what the plugin's fork child handler does
    Stats cs;
    const uint64_t before = onServer("fork:1094");
    uint64_t nc = 0;
    {
      OriginInFlight childRead(&cs, "fork:1094");
      nc = childRead.nc;
    }
    const std::string out = std::to_string(before) + " " + std::to_string(nc) + " " +
                            std::to_string(onServer("fork:1094"));
    if (::write(p[1], out.data(), out.size()) < 0) {
    }
    ::_exit(0);
  }
  ::close(p[1]);
  char buf[64] = {};
  ssize_t n = 0, got = 0;
  while ((n = ::read(p[0], buf + got, sizeof buf - 1 - static_cast<size_t>(got))) > 0)
    got += n;
  ::close(p[0]);
  ::waitpid(pid, nullptr, 0);
  EXPECT_STREQ(buf, "0 1 0");
  EXPECT_EQ(onServer("fork:1094"), 1u) << "the parent's count is its own";
}
