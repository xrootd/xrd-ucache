// An Executor used from another translation unit's static initialization, before Executor.cc's
// own initializers have run: the case of the plug-in called from XrdCl's static initialization
// (a program that loads the plug-in itself, as `ucache doctor` does, has XrdCl initialized as the
// plug-in's dependency, and XrdCl then calls the plug-in before the plug-in's own initializers).
//
// The early object below has a high init priority, so it is built before every object of default
// priority in this program, Executor.cc's among them. Run twice by ctest:
//   - without UCACHE_TEST_THREAD_FAIL_AFTER: the test hook must be inert, so the pool starts its
//     threads and nothing is counted as a thread that could not start;
//   - with UCACHE_TEST_THREAD_FAIL_AFTER=0: no thread can start anywhere, and every task, delayed
//     or not, runs on the thread that posts it, without a crash.
// A plain program (no test framework), so that nothing else runs before the early object.
#include "Executor.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using ucache::Executor;

namespace {
// Constant-initialized: written by the early object, before any dynamic initializer here.
std::atomic<int> g_ran{0};          // tasks run
std::atomic<int> g_onCaller{0};     // of them, on the thread that posted them
std::atomic<int> g_delayedRan{0};   // the delayed task
std::atomic<int> g_failuresAtInit{-1};
std::atomic<int> g_workersAtInit{-1};
std::atomic<Executor*> g_pool{nullptr};
std::atomic<std::thread::id*> g_initThread{nullptr};

struct EarlyUse {
  EarlyUse() {
    auto* me = new std::thread::id(std::this_thread::get_id()); // leaked
    g_initThread.store(me);
    auto* ex = new Executor(2, "uc-early", "uc-early-timer"); // leaked, as every pool is
    g_pool.store(ex);
    ex->post([me] {
      if (std::this_thread::get_id() == *me)
        g_onCaller.fetch_add(1);
      g_ran.fetch_add(1);
    });
    ex->postAfter(20, [] { g_delayedRan.fetch_add(1); });
    g_workersAtInit.store(static_cast<int>(ex->startedWorkers()));
    g_failuresAtInit.store(static_cast<int>(Executor::threadStartFailures()));
  }
};
__attribute__((init_priority(101))) EarlyUse g_early;

int g_fails = 0;
void check(bool ok, const char* what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_fails;
}
bool waitFor(const std::atomic<int>& v, int want, int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (v.load() < want && std::chrono::steady_clock::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  return v.load() >= want;
}
} // namespace

int main() {
  const char* hook = std::getenv("UCACHE_TEST_THREAD_FAIL_AFTER");
  const bool noThreads = hook && std::atoll(hook) == 0;
  std::printf("mode: %s\n", noThreads ? "UCACHE_TEST_THREAD_FAIL_AFTER=0" : "hook unset");
  if (hook && !noThreads) {
    std::printf("FAIL: run this test without the hook or with it at 0\n");
    return 2;
  }
  Executor& ex = *g_pool.load();
  check(waitFor(g_ran, 1, 5000), "the task posted at static initialization ran");
  check(waitFor(g_delayedRan, 1, 5000), "the delayed task posted at static initialization ran");
  if (!noThreads) {
    check(g_workersAtInit.load() >= 1, "the pool started a worker at static initialization");
    check(g_failuresAtInit.load() == 0, "no thread counted as failing to start (hook inert)");
    check(g_onCaller.load() == 0, "the task ran on a worker, not on the caller");
    check(Executor::threadStartFailures() == 0, "still no thread failed to start");
    // The pool built at static initialization is still known to the process: a pool that can
    // start no worker hands its task to it, the only pool with workers so far.
    Executor::testSetThreadFailAfter(0);
    auto* starved = new Executor(2, "uc-early3", "uc-early3-timer"); // leaked
    const std::thread::id mainId = std::this_thread::get_id();
    std::atomic<int> where{0}; // 1: on the caller, 2: on a worker of another pool
    starved->post([&where, mainId] { where.store(std::this_thread::get_id() == mainId ? 1 : 2); });
    check(waitFor(where, 1, 5000) && where.load() == 2,
          "a pool with no worker hands its task to the pool built at static initialization");
    Executor::testSetThreadFailAfter(-1);
  } else {
    check(g_workersAtInit.load() == 0, "no worker started (hook at 0)");
    check(g_failuresAtInit.load() >= 1, "the failures were counted");
    check(g_onCaller.load() == 1, "the task ran on the thread that posted it");
  }
  // After static initialization: the same pool, and one more, go on as before.
  std::atomic<int> later{0};
  ex.post([&later] { later.fetch_add(1); });
  auto* other = new Executor(2, "uc-early2", "uc-early2-timer"); // leaked
  other->post([&later] { later.fetch_add(1); });
  other->postAfter(10, [&later] { later.fetch_add(1); });
  check(waitFor(later, 3, 5000), "tasks posted after static initialization ran");
  std::printf("%s\n", g_fails ? "FAILED" : "PASSED");
  return g_fails ? 1 : 0;
}
