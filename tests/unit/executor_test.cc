// TSan-facing concurrency test for Executor: the new
// postAfter timer thread + min-deadline queue must hand every task to the pool
// exactly once under concurrent scheduling, with no races on its two locks.
// Executor is XrdCl-free, so this runs in the core unit-test binary and is
// exercised automatically by the TSan gate.
//
// Completion is signalled through a heap Latch captured BY VALUE (shared_ptr):
// its mutex gives a real happens-before edge from each task back to the waiter,
// and the heap lifetime means a task on the leaked process-wide Executor never
// touches a freed/reused stack slot. (A bare relaxed atomic on a stack local
// false-races under TSan across tests — a test artifact, since production tasks
// capture st_, a shared_ptr, never stack locals.)
#include "Executor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <dirent.h>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace ucache;

namespace {
struct Latch {
  std::mutex m;
  std::condition_variable cv;
  int count = 0;
  void bump() {
    {
      std::lock_guard<std::mutex> g(m);
      ++count;
    }
    cv.notify_all();
  }
  bool wait(int target, int maxMs = 10000) {
    std::unique_lock<std::mutex> lk(m);
    return cv.wait_for(lk, std::chrono::milliseconds(maxMs), [&] { return count >= target; });
  }
  int get() {
    std::lock_guard<std::mutex> g(m);
    return count;
  }
};
} // namespace

TEST(Executor, PostAfterRunsEveryTaskOnce) {
  auto& ex = Executor::instance();
  auto latch = std::make_shared<Latch>();
  const int N = 500;
  for (int i = 0; i < N; ++i)
    ex.postAfter(i % 10, [latch] { latch->bump(); });
  EXPECT_TRUE(latch->wait(N));
  EXPECT_EQ(latch->get(), N);
}

TEST(Executor, PostAfterConcurrentSchedulersNoRace) {
  auto& ex = Executor::instance();
  auto latch = std::make_shared<Latch>();
  const int T = 8, PER = 200;
  std::vector<std::thread> ts;
  for (int t = 0; t < T; ++t)
    ts.emplace_back([&ex, latch] {
      for (int i = 0; i < PER; ++i)
        ex.postAfter(i % 5, [latch] { latch->bump(); });
    });
  for (auto& th : ts)
    th.join();
  EXPECT_TRUE(latch->wait(T * PER));
  EXPECT_EQ(latch->get(), T * PER);
}

TEST(Executor, PostAfterReleasesByDeadlineNotSubmissionOrder) {
  auto& ex = Executor::instance();
  struct Rec {
    std::mutex m;
    std::condition_variable cv;
    int order = 0, first = 0, second = 0;
  };
  auto rec = std::make_shared<Rec>();
  ex.postAfter(250, [rec] { // submitted first, longer delay
    std::lock_guard<std::mutex> g(rec->m);
    rec->first = ++rec->order;
    rec->cv.notify_all();
  });
  ex.postAfter(20, [rec] { // submitted second, shorter delay
    std::lock_guard<std::mutex> g(rec->m);
    rec->second = ++rec->order;
    rec->cv.notify_all();
  });
  std::unique_lock<std::mutex> lk(rec->m);
  ASSERT_TRUE(
      rec->cv.wait_for(lk, std::chrono::seconds(5), [&] { return rec->first && rec->second; }));
  EXPECT_EQ(rec->second, 1); // shorter deadline released first
  EXPECT_EQ(rec->first, 2);
}

TEST(Executor, PostAfterZeroDelayPostsImmediately) {
  auto& ex = Executor::instance();
  auto latch = std::make_shared<Latch>();
  for (int i = 0; i < 100; ++i)
    ex.postAfter(0, [latch] { latch->bump(); });
  EXPECT_TRUE(latch->wait(100));
  EXPECT_EQ(latch->get(), 100);
}

// ------------------------------------------------------------------- fork()
// A child has only the thread that forked. These tests run code in a forked
// child and read what it reports over a pipe; the child always leaves with
// _exit, so nothing of the test binary (gtest, temp dirs) runs twice. TSan
// refuses to start threads after a multi-threaded fork, so they skip there.
#if defined(__SANITIZE_THREAD__)
#define UCACHE_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UCACHE_TSAN 1
#endif
#endif

namespace {

// Runs `body` in a forked child; returns what it reported, or "TIMEOUT" when it
// had not finished after `ms` (it is then killed).
std::string inChild(const std::function<std::string()>& body, int ms = 10000) {
  int p[2];
  if (::pipe(p) != 0)
    return "PIPE";
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(p[0]);
    const std::string out = body();
    if (::write(p[1], out.data(), out.size()) < 0) {
    }
    ::_exit(0);
  }
  ::close(p[1]);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  bool done = false;
  while (std::chrono::steady_clock::now() < deadline) {
    if (::waitpid(pid, nullptr, WNOHANG) == pid) {
      done = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::string out;
  if (!done) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    out = "TIMEOUT";
  } else {
    char buf[256];
    ssize_t n;
    while ((n = ::read(p[0], buf, sizeof buf)) > 0)
      out.append(buf, static_cast<size_t>(n));
  }
  ::close(p[0]);
  return out;
}

// Threads in this process; -1 where /proc is not there to ask.
int threadCount() {
#if defined(__linux__)
  int n = 0;
  if (DIR* d = ::opendir("/proc/self/task")) {
    while (dirent* e = ::readdir(d))
      if (e->d_name[0] != '.')
        ++n;
    ::closedir(d);
    return n;
  }
#endif
  return -1;
}

void warm(Executor& ex) { // the pool's threads exist, and have run
  auto l = std::make_shared<Latch>();
  ex.post([l] { l->bump(); });
  ASSERT_TRUE(l->wait(1));
}

} // namespace

TEST(ExecutorFork, WithoutStartingOverAChildRunsNothing) {
#ifdef UCACHE_TSAN
  GTEST_SKIP() << "fork with threads under TSan";
#endif
  auto& ex = Executor::instance();
  warm(ex);
  // The mechanism the fork handler exists for: the child's queue has no threads.
  const std::string r = inChild([&ex] {
    auto l = std::make_shared<Latch>();
    ex.post([l] { l->bump(); });
    return std::string(l->wait(1, 1500) ? "ran" : "stuck");
  });
  EXPECT_NE(r, "ran");
}

TEST(ExecutorFork, AChildStartsItsPoolsOverAtFirstUse) {
#ifdef UCACHE_TSAN
  GTEST_SKIP() << "fork with threads under TSan";
#endif
  auto& ex = Executor::instance();
  Executor other(2); // any pool, not only the process-wide one
  warm(ex);
  warm(other);
  const std::string r = inChild([&] {
    Executor::afterForkChild();
    const int before = threadCount(); // the handler starts nothing itself
    auto l = std::make_shared<Latch>();
    ex.post([l] { l->bump(); });
    ex.postAfter(50, [l] { l->bump(); }); // the timer is this process's too
    other.post([l] { l->bump(); });
    const bool all = l->wait(3, 5000);
    return "threads-before=" + std::to_string(before) + " done=" + (all ? "3" : "no");
  });
  if (threadCount() >= 0)
    EXPECT_EQ(r, "threads-before=1 done=3");
  else
    EXPECT_NE(r.find("done=3"), std::string::npos) << r;
}

TEST(ExecutorFork, APoolStartsNoThreadUntilItIsUsed) {
#ifdef UCACHE_TSAN
  GTEST_SKIP() << "fork with threads under TSan";
#endif
  if (threadCount() < 0)
    GTEST_SKIP() << "no /proc to count threads";
  // A pool is often built inside a function-local static: a constructor that
  // started threads would hold the static's guard meanwhile, and a child
  // forked in that window would wait on the guard for good.
  const int before = threadCount();
  Executor lazy(3);
  EXPECT_EQ(threadCount(), before);
  warm(lazy);
  EXPECT_EQ(threadCount(), before + 4); // three workers and the timer, at first use
}

TEST(ExecutorFork, TheParentsQueuedWorkStaysInTheParent) {
#ifdef UCACHE_TSAN
  GTEST_SKIP() << "fork with threads under TSan";
#endif
  Executor one(1);
  auto gate = std::make_shared<Latch>();
  auto started = std::make_shared<Latch>();
  one.post([gate, started] { // holds the only worker until the test lets go
    started->bump();
    gate->wait(1, 20000);
  });
  ASSERT_TRUE(started->wait(1));
  const pid_t parent = ::getpid();
  static std::atomic<int> ranInChild{0};
  auto ranInParent = std::make_shared<Latch>();
  one.post([parent, ranInParent] { // queued behind the gate: the parent's work
    if (::getpid() != parent)
      ranInChild.store(1);
    else
      ranInParent->bump();
  });
  const std::string r = inChild([&one] {
    Executor::afterForkChild();
    auto l = std::make_shared<Latch>();
    one.post([l] { l->bump(); });
    const bool mine = l->wait(1, 5000);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return std::string(mine ? "own" : "none") + (ranInChild.load() ? "+parents" : "");
  });
  EXPECT_EQ(r, "own"); // its own task ran; the parent's queued one did not
  gate->bump();
  EXPECT_TRUE(ranInParent->wait(1)); // and it still runs where it belongs
}

TEST(ExecutorFork, AGrandchildStartsOverToo) {
#ifdef UCACHE_TSAN
  GTEST_SKIP() << "fork with threads under TSan";
#endif
  auto& ex = Executor::instance();
  warm(ex);
  const std::string r = inChild([&ex] {
    Executor::afterForkChild();
    warm(ex); // the child's pool exists and has run: the case a second fork meets
    return inChild([&ex] {
      Executor::afterForkChild();
      auto l = std::make_shared<Latch>();
      ex.post([l] { l->bump(); });
      return std::string(l->wait(1, 5000) ? "ran" : "stuck");
    });
  });
  EXPECT_EQ(r, "ran");
}
