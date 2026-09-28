#include "CpuCounters.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/syscall.h>
#endif

using namespace ucache;

// ---- Scaling a shared counter: the arithmetic on its own ----
//
// A counter switched out for part of its enabled time counted only while it
// held the PMU; `perf stat` scales it by enabled/running, and so must we.

namespace {
CounterReading reading(uint64_t value, uint64_t enabled, uint64_t running) {
  CounterReading r;
  r.value = value;
  r.enabled = enabled;
  r.running = running;
  return r;
}
} // namespace

TEST(ScaleCounter, AFullyMeasuredCountIsTakenAsItIs) {
  const CounterReading r = reading(1000, 500, 500);
  const ScaledCount s = scaleCounter(&r, 1);
  EXPECT_EQ(s.value, 1000u);
  EXPECT_DOUBLE_EQ(s.duty, 1.0);
}

TEST(ScaleCounter, ACountMeasuredHalfTheTimeIsDoubled) {
  const CounterReading r = reading(1000, 800, 400);
  const ScaledCount s = scaleCounter(&r, 1);
  EXPECT_EQ(s.value, 2000u);
  EXPECT_DOUBLE_EQ(s.duty, 0.5);
}

TEST(ScaleCounter, TheShareOfTimeLostIsWhatIsRestored) {
  // The pool of shared batch nodes: 55.8% of the time on the PMU.
  const CounterReading r = reading(558, 1000, 558);
  const ScaledCount s = scaleCounter(&r, 1);
  EXPECT_EQ(s.value, 1000u);
  EXPECT_NEAR(s.duty, 0.558, 1e-12);
}

TEST(ScaleCounter, EachReadingIsScaledOnItsOwnAndTheDutyIsTheirTotal) {
  // A forked child's process-wide counter and one per thread it inherited:
  // each switched in and out on its own, so each is scaled by its own ratio.
  const CounterReading rs[] = {reading(100, 100, 100),  // measured throughout
                               reading(100, 400, 100),  // a quarter of the time
                               reading(300, 500, 300)}; // three fifths
  const ScaledCount s = scaleCounter(rs, 3);
  EXPECT_EQ(s.value, 100u + 400u + 500u);
  EXPECT_DOUBLE_EQ(s.duty, 500.0 / 1000.0);
}

TEST(ScaleCounter, AReadingNeverEnabledAddsNothingAndLosesNothing) {
  const CounterReading rs[] = {reading(0, 0, 0), reading(700, 700, 700)};
  const ScaledCount s = scaleCounter(rs, 2);
  EXPECT_EQ(s.value, 700u);
  EXPECT_DOUBLE_EQ(s.duty, 1.0);
  // Nothing enabled at all: nothing counted and nothing lost.
  const ScaledCount none = scaleCounter(rs, 1);
  EXPECT_EQ(none.value, 0u);
  EXPECT_DOUBLE_EQ(none.duty, 1.0);
  const ScaledCount empty = scaleCounter(nullptr, 0);
  EXPECT_EQ(empty.value, 0u);
  EXPECT_DOUBLE_EQ(empty.duty, 1.0);
}

TEST(ScaleCounter, AReadingEnabledButNeverRunHasNothingToScaleAndPullsTheDutyDown) {
  const CounterReading rs[] = {reading(0, 600, 0), reading(200, 200, 200)};
  const ScaledCount s = scaleCounter(rs, 2);
  EXPECT_EQ(s.value, 200u); // nothing to extrapolate from, so nothing invented
  EXPECT_DOUBLE_EQ(s.duty, 200.0 / 800.0);
}

TEST(ScaleCounter, RunningNeverExceedsEnabled) {
  // Not something the kernel reports, but a ratio above one would scale DOWN.
  const CounterReading r = reading(1000, 400, 500);
  const ScaledCount s = scaleCounter(&r, 1);
  EXPECT_EQ(s.value, 1000u);
  EXPECT_DOUBLE_EQ(s.duty, 1.0);
}

TEST(ScaleCounter, AScaledCountSaturatesRatherThanWrapping) {
  const CounterReading r = reading(UINT64_MAX / 2, 1000, 100);
  EXPECT_EQ(scaleCounter(&r, 1).value, UINT64_MAX);
}

TEST(ScaleCounter, RoundsToTheNearestCount) {
  const CounterReading up = reading(2, 3, 2); // 3.0
  EXPECT_EQ(scaleCounter(&up, 1).value, 3u);
  const CounterReading half = reading(1, 3, 2); // 1.5
  EXPECT_EQ(scaleCounter(&half, 1).value, 2u);
  const CounterReading down = reading(1, 4, 3); // 1.333...
  EXPECT_EQ(scaleCounter(&down, 1).value, 1u);
}

TEST(DutyText, FourDecimalsClampedToTheUnitInterval) {
  EXPECT_EQ(dutyText(1.0), "1.0000");
  EXPECT_EQ(dutyText(0.0), "0.0000");
  EXPECT_EQ(dutyText(0.558), "0.5580");
  EXPECT_EQ(dutyText(0.55786), "0.5579"); // rounded, not truncated
  EXPECT_EQ(dutyText(0.99996), "1.0000");
  EXPECT_EQ(dutyText(0.00004), "0.0000");
  EXPECT_EQ(dutyText(1e-9), "0.0000"); // never exponent form
  EXPECT_EQ(dutyText(1.5), "1.0000");
  EXPECT_EQ(dutyText(-0.2), "0.0000");
  EXPECT_EQ(dutyText(std::nan("")), "0.0000");
}

// ---- The counters themselves, on whatever this machine permits ----

namespace {
// Work the compiler cannot remove.
uint64_t spin(uint64_t n) {
  volatile uint64_t x = 1;
  for (uint64_t i = 0; i < n; ++i)
    x = x * 6364136223846793005ull + 1442695040888963407ull;
  return x;
}
} // namespace

TEST(CpuCounters, ASampleNamesItsSourceOrCarriesNothing) {
  CpuCounters c;
  spin(2000000);
  const CpuCounters::Sample s = c.sample();
  if (!c.available()) {
    // The kernel refused (a container, perf_event_paranoid): nothing to report.
    EXPECT_EQ(s.source, nullptr);
    EXPECT_EQ(s.instructions, 0u);
    EXPECT_EQ(s.cycles, 0u);
    GTEST_SKIP() << "no hardware counters on this machine";
  }
  ASSERT_NE(s.source, nullptr);
#if defined(__linux__)
  EXPECT_STREQ(s.source, "perf");
#elif defined(__APPLE__)
  EXPECT_STREQ(s.source, "rusage");
#endif
  EXPECT_GT(s.instructions, 2000000u); // at least an instruction a round
  EXPECT_GT(s.cycles, 0u);
  EXPECT_GE(s.duty, 0.0);
  EXPECT_LE(s.duty, 1.0);
  spin(1000000);
  const CpuCounters::Sample later = c.sample();
  EXPECT_GT(later.instructions, s.instructions); // counts only grow
}

TEST(CpuCounters, AForkedChildCountsFromItsReopenNotFromTheParentsStart) {
  CpuCounters c;
  if (!c.available())
    GTEST_SKIP() << "no hardware counters on this machine";
  spin(20000000); // the parent's work, which the child must not report
  const uint64_t parent = c.sample().instructions;
  int p[2];
  ASSERT_EQ(::pipe(p), 0);
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    ::close(p[0]);
    c.reopen(); // what the store does in its fork child handler
    spin(1000000);
    const CpuCounters::Sample s = c.sample();
    // One byte per verdict: 1 = own count, sane duty and a named source.
    const char ok = s.source && s.instructions > 1000000u && s.instructions < parent &&
                            s.duty >= 0.0 && s.duty <= 1.0
                        ? 1
                        : 0;
    if (::write(p[1], &ok, 1) != 1) {
    }
    ::_exit(0);
  }
  ::close(p[1]);
  char ok = 0;
  const ssize_t n = ::read(p[0], &ok, 1);
  ::close(p[0]);
  ::waitpid(pid, nullptr, 0);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(ok, 1) << "the child reported the parent's count, or no count of its own";
}

#if defined(__linux__)
namespace {
// A hardware event of our own on this thread, counting, to crowd the PMU.
int hog(uint64_t config) {
  struct perf_event_attr a;
  std::memset(&a, 0, sizeof a);
  a.type = PERF_TYPE_HARDWARE;
  a.size = sizeof a;
  a.config = config;
  a.exclude_kernel = 1;
  a.exclude_hv = 1;
  return static_cast<int>(::syscall(SYS_perf_event_open, &a, 0, -1, -1, PERF_FLAG_FD_CLOEXEC));
}
} // namespace

TEST(CpuCounters, ASharedPmuIsScaledBackToTheWorkDone) {
  // The same work measured twice: once with the PMU to ourselves, once with
  // more events asking for it than it has registers, so the kernel rotates
  // them and each counts only part of the time. Scaled, the second count must
  // still describe the same work -- read bare, it would fall by the duty cycle.
  const uint64_t kWork = 30000000;
  CpuCounters alone;
  if (!alone.available())
    GTEST_SKIP() << "no hardware counters on this machine";
  spin(kWork);
  const CpuCounters::Sample ref = alone.sample();
  int hogs[24];
  int n = 0;
  for (; n < 24; ++n)
    if ((hogs[n] = hog(n % 2 ? PERF_COUNT_HW_INSTRUCTIONS : PERF_COUNT_HW_CPU_CYCLES)) < 0)
      break;
  CpuCounters shared;
  spin(kWork);
  const CpuCounters::Sample s = shared.sample();
  for (int i = 0; i < n; ++i)
    ::close(hogs[i]);
  if (!(s.duty < 0.99))
    GTEST_SKIP() << "the PMU was not shared (" << n << " extra events, duty " << s.duty << ")";
  const double ratio = static_cast<double>(s.instructions) / static_cast<double>(ref.instructions);
  // Measured on a 64-core x86 box: duty 0.13, scaled within 2% of the count
  // taken alone, where the bare value would have read 13% of it.
  EXPECT_NEAR(ratio, 1.0, 0.25) << "duty " << s.duty << ", scaled " << s.instructions
                                << " vs alone " << ref.instructions;
}
#endif
