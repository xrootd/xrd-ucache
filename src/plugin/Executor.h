// Internal executor: all disk I/O and buffer assembly for the
// cache run here — never on the caller's or XrdCl's callback threads.
//
// fork(): a child has only the thread that forked, so a pool's workers, its
// timer and whatever it had queued stay in the parent. Every Executor
// therefore keeps its locks, queues and threads in a core that belongs to one
// fork generation: afterForkChild() starts a new generation, and each
// Executor builds a new core -- fresh locks, empty queues, new threads -- the
// first time it is used in the child. The parent's core is left behind in the
// child, never run and never destroyed: its tasks are the parent's work.
//
// Thread-safety: fully thread-safe. Cores are intentionally leaked (threads
// end with the process): joining at static destruction could deadlock against
// XrdCl teardown.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace ucache {

struct Stats;

class Executor {
 public:
  explicit Executor(unsigned threads);

  void post(std::function<void()> task);

  // Run `task` on the pool after at least `delayMs`, via a single dedicated
  // timer thread draining a min-deadline queue (delayed
  // dispatch). Used for open-retry backoff so worker threads are never blocked
  // sleeping — a sleep on a worker would starve the pool under mass-concurrent
  // open failures, exactly the scenario retry targets. delayMs == 0 posts now.
  void postAfter(uint64_t delayMs, std::function<void()> task);

  // Process-wide instance, sized per UCACHE_THREADS (0 = the CPUs this process
  // may use: usableCpus()).
  static Executor& instance(unsigned threads = 0);
  // The number of worker threads this executor runs.
  unsigned threads() const { return threads_; }
  // Account the tasks this executor runs in `s` (its pool_* counters and the
  // queue-wait histogram) from now on; null stops. `s` must outlive the
  // executor (the store's counters are leaked with it).
  void setStats(Stats* s) { stats_.store(s, std::memory_order_release); }

  // The current fork generation: 0 in the process that loaded the plugin, one
  // more in each forked child. Other process-wide state keyed on it rebuilds
  // itself the same way (lazily, on first use in the child).
  static uint64_t forkGeneration();
  // Called from the plugin's fork child handler only (single-threaded): starts
  // a new generation. Takes no lock the parent may hold and creates no thread.
  static void afterForkChild();

 private:
  struct Core;
  Core* core(); // this generation's core, built on first use

  const unsigned threads_;
  std::atomic<Core*> core_{nullptr};
  std::atomic<Stats*> stats_{nullptr};
};

} // namespace ucache
