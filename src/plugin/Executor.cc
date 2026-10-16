#include "Executor.h"

#include "Log.h"
#include "Stats.h"
#include "UsableCpus.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <system_error>
#include <utility>

namespace ucache {

namespace {
uint64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::atomic<uint64_t> g_gen{0};
// The pool core the calling thread works for (null for a thread of no pool).
thread_local const void* tCore = nullptr;
// Serializes building a core. Replaced, not unlocked, in a forked child: a
// parent thread may have held it at the fork. Leaked. Constant-initialized and
// made at first use: the plugin can be called from XrdCl's static
// initialization before this library's own dynamic initializers have run.
std::atomic<std::mutex*> g_buildMu{nullptr};
std::mutex& buildMu() {
  std::mutex* m = g_buildMu.load(std::memory_order_acquire);
  if (m)
    return *m;
  auto* fresh = new std::mutex;
  if (g_buildMu.compare_exchange_strong(m, fresh, std::memory_order_acq_rel))
    return *fresh;
  delete fresh; // another thread made it first
  return *m;
}
// Threads that could not be started (std::thread throws std::system_error when the process may
// not make one: an address-space limit, a limit on processes): counted here until the store's
// counters exist (setFailureStats), then there too; said once per process at WARN.
std::atomic<uint64_t> g_threadFailures{0};
std::atomic<Stats*> g_failStats{nullptr};
std::atomic<bool> g_failSaid{false};
// Test hook UCACHE_TEST_THREAD_FAIL_AFTER=N (inert unless set, read at load; testSetThreadFailAfter
// for unit tests): every thread creation of an Executor after the first N fails as if the process
// could not make one.
std::atomic<int64_t> g_failAfter{[] {
  const char* v = ::getenv("UCACHE_TEST_THREAD_FAIL_AFTER");
  return v ? static_cast<int64_t>(std::strtoll(v, nullptr, 10)) : int64_t(-1);
}()};
std::atomic<int64_t> g_threadsMade{0};

// Starts `fn` on a detached thread; false (counted) when no thread could be made, `err` then
// saying why.
template <typename Fn>
bool startThread(Fn&& fn, std::string& err) noexcept {
  const char* why = nullptr;
  try {
    const int64_t limit = g_failAfter.load(std::memory_order_relaxed);
    if (limit >= 0 && g_threadsMade.fetch_add(1, std::memory_order_relaxed) >= limit)
      throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                              "test hook UCACHE_TEST_THREAD_FAIL_AFTER");
    std::thread(std::forward<Fn>(fn)).detach(); // leaked with the core; see header
    return true;
  } catch (const std::system_error& e) {
    try {
      err = e.what();
    } catch (...) {
    }
    why = "system_error";
  } catch (...) {
    why = "an exception";
  }
  try {
    if (err.empty())
      err = why;
  } catch (...) {
  }
  g_threadFailures.fetch_add(1, std::memory_order_relaxed);
  if (Stats* s = g_failStats.load(std::memory_order_acquire))
    s->poolThreadFailures.fetch_add(1, std::memory_order_relaxed);
  return false;
}
// Said once per process: the first pool short of threads.
void sayThreadFailure(unsigned failed, unsigned want, const char* what, const std::string& err) {
  if (g_failSaid.exchange(true))
    return;
  try {
    UCACHE_WARN("could not start %u of %u %s: %s (an address-space or process limit?); the "
                "pool's tasks run on the threads that started, on another pool, or on the thread "
                "that asks (counted in pool_thread_failures)",
                failed, want, what, err.c_str());
  } catch (...) {
  }
}

// Every pool core of the process, for the fallback of a pool that has no worker (cores are
// leaked, so the pointers stay valid). Replaced, not unlocked, in a forked child, as buildMu.
std::atomic<std::mutex*> g_coresMu{nullptr};
std::mutex& coresMu() {
  std::mutex* m = g_coresMu.load(std::memory_order_acquire);
  if (m)
    return *m;
  auto* fresh = new std::mutex;
  if (g_coresMu.compare_exchange_strong(m, fresh, std::memory_order_acq_rel))
    return *fresh;
  delete fresh;
  return *m;
}
std::vector<void*>* g_cores = new std::vector<void*>; // leaked; under coresMu
} // namespace

// One fork generation's pool. Its threads capture the core, never the
// Executor, so a core outlives nothing it depends on.
//
// A thread that cannot be started (std::thread throws when the process may make no more: an
// address-space limit, a limit on processes) never fails the pool: its constructor keeps the
// threads that started and throws nothing. A pool with no worker tries again to start one at a
// post, at most once a second; while it has none, its tasks go to another pool of the process
// that has workers, and when no pool has any, run on the calling thread. That is the last resort,
// and safe there: no post is made with a lock held that a task takes (every post site was read
// for it), and a process that can start no thread at all can do nothing else with the task. A
// pool whose timer thread did not start has its workers serve the delayed tasks; with no worker
// either, a delayed task goes to another pool's timer, or waits in the queue and runs at the
// pool's next post (never lost).
struct Executor::Core {
  Core(unsigned threads, uint64_t gen, const std::atomic<Stats*>* stats)
      : gen(gen), stats(stats), size(threads) {
    std::string err;
    for (unsigned i = 0; i < threads; ++i) {
      if (!startThread([this] { loop(); }, err)) {
        sayThreadFailure(threads - i, threads, "worker threads", err);
        break; // the next ones would fail as well; a post tries again later
      }
      workers.fetch_add(1, std::memory_order_release);
    }
    timerOk.store(startThread([this] { timerLoop(); }, err), std::memory_order_release);
    if (!timerOk.load(std::memory_order_relaxed))
      sayThreadFailure(1, 1, "timer threads", err);
    try {
      std::lock_guard<std::mutex> g(coresMu());
      g_cores->push_back(this);
    } catch (...) {
      // not a fallback for others then; it runs its own tasks as above
    }
  }

  // A pool with no worker: try again to start one (at most once a second).
  bool retryWorker() {
    const uint64_t now = nowNs();
    uint64_t last = lastTryNs.load(std::memory_order_relaxed);
    if (now - last < 1000000000ull || !lastTryNs.compare_exchange_strong(last, now))
      return workers.load(std::memory_order_acquire) > 0;
    std::string err;
    if (startThread([this] { loop(); }, err))
      workers.fetch_add(1, std::memory_order_release);
    if (!timerOk.load(std::memory_order_acquire) && startThread([this] { timerLoop(); }, err))
      timerOk.store(true, std::memory_order_release);
    return workers.load(std::memory_order_acquire) > 0;
  }
  // Another pool of this generation with a worker, for a pool that has none; null for none.
  Core* fallback() {
    std::lock_guard<std::mutex> g(coresMu());
    for (void* p : *g_cores) {
      Core* c = static_cast<Core*>(p);
      if (c != this && c->gen == gen && c->workers.load(std::memory_order_acquire) > 0)
        return c;
    }
    return nullptr;
  }

  void post(std::function<void()> task) {
    if (workers.load(std::memory_order_acquire) == 0 && !retryWorker()) {
      if (Core* other = fallback()) {
        other->post(std::move(task));
        return;
      }
      // No thread anywhere: the task, and any delayed task now due, run here.
      runDueTimersHere();
      task();
      return;
    }
    Stats* s = stats->load(std::memory_order_acquire);
    {
      std::lock_guard<std::mutex> g(mu);
      queue.push_back(Task{std::move(task), s ? nowNs() : 0});
      if (s) {
        const uint64_t n = queue.size();
        uint64_t hw = s->poolQueueHighWater.load(std::memory_order_relaxed);
        while (n > hw && !s->poolQueueHighWater.compare_exchange_weak(hw, n, std::memory_order_relaxed)) {
        }
      }
    }
    cv.notify_one();
  }

  void postAfter(uint64_t delayMs, std::function<void()> task) {
    const uint64_t deadline = nowNs() + delayMs * 1000000ull;
    if (!timerOk.load(std::memory_order_acquire))
      (void)retryWorker();
    if (timerOk.load(std::memory_order_acquire)) {
      {
        std::lock_guard<std::mutex> g(tmu);
        timers.push(Timed{deadline, std::move(task)});
      }
      tcv.notify_one();
      return;
    }
    if (workers.load(std::memory_order_acquire) > 0) {
      // No timer thread: the workers serve the delayed tasks (loop). Under mu, so that a worker
      // that has just looked at the timers and is about to wait does not miss this one.
      {
        std::lock_guard<std::mutex> g(mu);
        std::lock_guard<std::mutex> t(tmu);
        timers.push(Timed{deadline, std::move(task)});
      }
      cv.notify_all();
      return;
    }
    if (Core* other = fallback()) {
      other->postAfter(delayMs, std::move(task));
      return;
    }
    // No thread anywhere: kept, and run at a later post of this pool once due (runDueTimersHere).
    std::lock_guard<std::mutex> g(tmu);
    timers.push(Timed{deadline, std::move(task)});
  }

  // The delayed tasks now due, moved into the work queue; returns the next deadline, 0 for none.
  // Under mu (the caller holds it).
  uint64_t takeDueTimersLocked() {
    std::lock_guard<std::mutex> t(tmu);
    const uint64_t now = nowNs();
    while (!timers.empty() && timers.top().deadlineNs <= now) {
      queue.push_back(Task{std::move(const_cast<Timed&>(timers.top()).task), 0});
      timers.pop();
    }
    return timers.empty() ? 0 : timers.top().deadlineNs;
  }
  // With no thread anywhere: the delayed tasks now due, run on the calling thread.
  void runDueTimersHere() {
    std::vector<std::function<void()>> due;
    {
      std::lock_guard<std::mutex> t(tmu);
      const uint64_t now = nowNs();
      while (!timers.empty() && timers.top().deadlineNs <= now) {
        due.push_back(std::move(const_cast<Timed&>(timers.top()).task));
        timers.pop();
      }
    }
    for (auto& f : due)
      f();
  }

  void loop() {
    for (;;) {
      Task task;
      {
        std::unique_lock<std::mutex> lk(mu);
        for (;;) {
          if (!timerOk.load(std::memory_order_acquire)) {
            // No timer thread: the workers serve the delayed tasks.
            const uint64_t next = takeDueTimersLocked();
            if (!queue.empty())
              break;
            if (next) {
              const uint64_t now = nowNs();
              cv.wait_for(lk, std::chrono::nanoseconds(next > now ? next - now : 0));
            } else {
              cv.wait(lk);
            }
            continue;
          }
          if (!queue.empty())
            break;
          cv.wait(lk);
        }
        task = std::move(queue.front());
        queue.pop_front();
      }
      Stats* s = task.postedNs ? stats->load(std::memory_order_acquire) : nullptr;
      const uint64_t start = s ? nowNs() : 0;
      // Marked when a task runs, not when the thread starts: this library is loaded at run time,
      // so a thread's first touch of its thread_local storage allocates that storage, and with
      // it a malloc arena of the thread's own (64 MiB of address space). A worker that never
      // runs a task then takes none, which a process under an address-space limit needs.
      tCore = this;
      task.fn(); // tasks must not throw (no exceptions cross the ABI)
      if (s) {
        s->poolQueueUs.add((start - task.postedNs) / 1000);
        s->poolBusyUs.fetch_add((nowNs() - start) / 1000, std::memory_order_relaxed);
        s->poolTasks.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  void timerLoop() {
    for (;;) {
      std::function<void()> ready;
      {
        std::unique_lock<std::mutex> lk(tmu);
        if (timers.empty())
          tcv.wait(lk, [this] { return !timers.empty(); });
        const uint64_t now = nowNs();
        const uint64_t deadline = timers.top().deadlineNs;
        if (deadline <= now) {
          ready = std::move(const_cast<Timed&>(timers.top()).task);
          timers.pop();
        } else {
          // Wait until the earliest deadline, or until a nearer one is enqueued.
          tcv.wait_for(lk, std::chrono::nanoseconds(deadline - now));
        }
      }
      if (ready)
        post(std::move(ready)); // hand off to the work queue (mu not held here)
    }
  }

  struct Task {
    std::function<void()> fn;
    uint64_t postedNs = 0; // 0: not accounted (no stats when it was posted)
  };
  const uint64_t gen;
  const std::atomic<Stats*>* const stats; // the Executor's; it is never destroyed
  const unsigned size;                    // the workers it was made for
  std::atomic<unsigned> workers{0};       // the workers that started
  std::atomic<bool> timerOk{false};       // the timer thread started
  std::atomic<uint64_t> lastTryNs{0};     // the last try to start a worker (retryWorker)
  std::mutex mu;
  std::condition_variable cv;
  std::deque<Task> queue;

  // Delayed dispatch: one timer thread drains a min-deadline queue and forwards
  // due tasks to post(). Separate lock so scheduling never contends the work
  // queue.
  struct Timed {
    uint64_t deadlineNs;
    std::function<void()> task;
    bool operator<(const Timed& o) const { return deadlineNs > o.deadlineNs; } // min-heap
  };
  std::mutex tmu;
  std::condition_variable tcv;
  std::priority_queue<Timed> timers;
};

// No thread here: the core is built at first use. A pool built inside a
// function-local static would otherwise hold that static's guard while it
// starts threads, and a child forked in that window would wait on the guard
// for good.
Executor::Executor(unsigned threads) : threads_(threads) {}

Executor::Core* Executor::core() {
  const uint64_t gen = g_gen.load(std::memory_order_acquire);
  Core* c = core_.load(std::memory_order_acquire);
  if (c && c->gen == gen)
    return c;
  std::lock_guard<std::mutex> g(buildMu());
  c = core_.load(std::memory_order_acquire);
  if (c && c->gen == gen)
    return c;
  // The previous generation's core is left as it is: in a child, nothing
  // runs it and nothing may destroy it (its tasks capture the parent's state).
  c = new Core(threads_, gen, &stats_);
  core_.store(c, std::memory_order_release);
  return c;
}

void Executor::post(std::function<void()> task) { core()->post(std::move(task)); }

// Never builds the pool: a question about a pool that has no threads yet starts none (each pool's
// threads take address space, which a process under a limit on it needs for other work).
bool Executor::runsThisThread() {
  const Core* c = core_.load(std::memory_order_acquire);
  return tCore != nullptr && c != nullptr && tCore == c &&
         c->gen == g_gen.load(std::memory_order_acquire);
}

void Executor::postAfter(uint64_t delayMs, std::function<void()> task) {
  if (delayMs == 0) {
    post(std::move(task));
    return;
  }
  core()->postAfter(delayMs, std::move(task));
}

Executor& Executor::instance(unsigned threads) {
  // One thread per read in flight, up to the CPUs this process may use: a
  // reader thread waits for its read, so these threads take no CPU from one
  // that is working, and the work posted here (slot-store reads and decodes,
  // first-pass requests) blocks.
  static Executor* exec = new Executor(threads ? threads : std::max(1u, usableCpus()));
  return *exec;
}

uint64_t Executor::forkGeneration() { return g_gen.load(std::memory_order_acquire); }

uint64_t Executor::threadStartFailures() { return g_threadFailures.load(std::memory_order_relaxed); }

void Executor::setFailureStats(Stats* s) {
  if (s && !g_failStats.exchange(s, std::memory_order_acq_rel))
    s->poolThreadFailures.fetch_add(g_threadFailures.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
}

void Executor::testSetThreadFailAfter(int64_t n) {
  g_threadsMade.store(0, std::memory_order_relaxed);
  g_failAfter.store(n, std::memory_order_relaxed);
}

unsigned Executor::startedWorkers() {
  Core* c = core();
  return c->workers.load(std::memory_order_acquire);
}

void Executor::afterForkChild() {
  g_buildMu.store(new std::mutex, std::memory_order_release); // the old one: leaked
  g_coresMu.store(new std::mutex, std::memory_order_release); // the old one: leaked
  g_gen.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace ucache
