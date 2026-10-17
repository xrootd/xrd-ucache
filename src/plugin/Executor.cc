#include "Executor.h"

#include "Log.h"
#include "Stats.h"
#include "UsableCpus.h"

#include <algorithm>
#include <chrono>
#include <pthread.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>
#include <utility>

namespace ucache {

// Every object at namespace scope here is constant-initialized (a literal, nullptr, a constexpr
// constructor), never computed by a dynamic initializer: the plug-in can be called from XrdCl's
// static initialization before this library's own dynamic initializers have run (a program that
// loads the plug-in itself, which pulls in XrdCl as its dependency), and the pools are used then.
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
// Test hook UCACHE_TEST_THREAD_FAIL_AFTER=N (inert unless set; testSetThreadFailAfter for unit
// tests): every thread creation of an Executor after the first N fails as if the process could not
// make one. Constant-initialized to "not read yet" and read at the first thread started: a value
// computed by a dynamic initializer would still be zero (fail them all) when the plugin is called
// from XrdCl's static initialization, before this library's own initializers have run.
constexpr int64_t kFailAfterUnread = -2;
std::atomic<int64_t> g_failAfter{kFailAfterUnread};
std::atomic<int64_t> g_threadsMade{0};
int64_t failAfter() {
  int64_t v = g_failAfter.load(std::memory_order_relaxed);
  if (v != kFailAfterUnread)
    return v;
  const char* e = ::getenv("UCACHE_TEST_THREAD_FAIL_AFTER");
  const int64_t read = e ? static_cast<int64_t>(std::strtoll(e, nullptr, 10)) : int64_t(-1);
  const int64_t limit = read < 0 ? int64_t(-1) : read; // a negative value: inert
  // A value set meanwhile (testSetThreadFailAfter) wins.
  if (g_failAfter.compare_exchange_strong(v, limit, std::memory_order_relaxed))
    return limit;
  return v;
}

// Starts `fn` on a detached thread named `name` (at most 15 characters: what `top -H` and
// /proc/<pid>/task/<tid>/comm show); false (counted) when no thread could be made, `err` then
// saying why.
template <typename Fn>
bool startThread(Fn&& fn, std::string& err, const char* name) noexcept {
  const char* why = nullptr;
  try {
    const int64_t limit = failAfter();
    if (limit >= 0 && g_threadsMade.fetch_add(1, std::memory_order_relaxed) >= limit)
      throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                              "test hook UCACHE_TEST_THREAD_FAIL_AFTER");
    std::thread([f = std::forward<Fn>(fn), name]() mutable {
      Executor::nameThisThread(name);
      f();
    }).detach(); // leaked with the core; see header
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
// leaked, so the pointers stay valid). The lock is replaced, not unlocked, in a forked child, as
// buildMu, and the list with it (a parent thread may have been changing it at the fork; the
// parent's cores are of no use to the child). Both constant-initialized and made at first use,
// as buildMu.
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
std::vector<void*>* g_cores = nullptr; // leaked; under coresMu
std::vector<void*>& coresLocked() { // under coresMu; throws std::bad_alloc only
  if (!g_cores)
    g_cores = new std::vector<void*>;
  return *g_cores;
}

// Whether the calling thread may block (sleep out a delay) when a delayed task must run on it
// (no thread anywhere): the plug-in sets a probe that says no on a client callback thread
// (setMayBlockProbe); a thread of a pool of ours never may.
std::atomic<bool (*)()> g_mayBlock{nullptr};
bool callerMayBlock() {
  if (tCore != nullptr)
    return false;
  bool (*probe)() = g_mayBlock.load(std::memory_order_acquire);
  return !probe || probe();
}
// A delayed task run on the calling thread (no thread anywhere): the chain of delayed tasks it
// posts runs in a loop of the outermost call, not by recursion.
struct InlineDelayed {
  uint64_t deadlineNs;
  std::function<void()> task;
};
thread_local int tInlineDepth = 0;
thread_local std::vector<InlineDelayed>* tInlinePending = nullptr;
void runDelayedOnCaller(uint64_t deadlineNs, std::function<void()> task) {
  if (tInlineDepth > 0) { // inside a delayed task run here: the outer loop runs it
    if (!tInlinePending)
      tInlinePending = new std::vector<InlineDelayed>; // per thread, leaked with it
    tInlinePending->push_back(InlineDelayed{deadlineNs, std::move(task)});
    return;
  }
  ++tInlineDepth;
  InlineDelayed next{deadlineNs, std::move(task)};
  for (;;) {
    // A caller that may block waits until the deadline (a back-off it is waiting for anyway); one
    // that may not (a client callback thread) runs the task at once: the delays are back-offs.
    const uint64_t now = nowNs();
    if (next.deadlineNs > now && callerMayBlock())
      std::this_thread::sleep_for(std::chrono::nanoseconds(next.deadlineNs - now));
    next.task(); // tasks must not throw (no exceptions cross the ABI)
    if (!tInlinePending || tInlinePending->empty())
      break;
    auto it = std::min_element(tInlinePending->begin(), tInlinePending->end(),
                               [](const InlineDelayed& a, const InlineDelayed& b) {
                                 return a.deadlineNs < b.deadlineNs;
                               });
    next = std::move(*it);
    tInlinePending->erase(it);
  }
  --tInlineDepth;
}
// A task that could not be queued for want of memory: run on the calling thread, said once.
std::atomic<bool> g_noMemSaid{false};
void sayNoMemory();
void noteNoMemory() {
  g_threadFailures.fetch_add(1, std::memory_order_relaxed);
  if (Stats* s = g_failStats.load(std::memory_order_acquire))
    s->poolThreadFailures.fetch_add(1, std::memory_order_relaxed);
  sayNoMemory();
}
void sayNoMemory() {
  if (g_noMemSaid.exchange(true))
    return;
  try {
    UCACHE_WARN("a task of uCache's pools could not be queued for want of memory (an "
                "address-space limit?): it ran on the thread that asked (counted in "
                "pool_thread_failures)");
  } catch (...) {
  }
}
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
// either, a delayed task goes to another pool's timer, or, with no thread anywhere, runs on the
// calling thread (after its delay when that thread may block, at once when it may not; a chain of
// them as a loop, runDelayedOnCaller), except housekeeping, which waits in the queue and runs at the
// pool's next post: nobody waits for it. A task that cannot be queued for want of memory runs on
// the calling thread too.
struct Executor::Core {
  // A pool starts one worker, and starts another at a post that finds none idle, up to `threads`
  // (each thread takes address space: its stack, and a malloc arena once it runs a task; a process
  // under a limit on it must not lose it to threads it never needs). Its timer thread starts at its
  // first delayed task: most pools never have one.
  Core(unsigned threads, uint64_t gen, const std::atomic<Stats*>* stats, const char* name,
       const char* timerName)
      : gen(gen), stats(stats), size(threads), name(name), timerName(timerName) {
    std::string err;
    if (threads > 0) {
      starting = spawned = 1; // no thread runs yet: no lock needed
      if (startThread([this] { loop(); }, err, name)) {
        workers.fetch_add(1, std::memory_order_release);
      } else {
        starting = spawned = 0;
        sayThreadFailure(threads, threads, "worker threads", err); // a post tries again later
      }
    }
    try {
      std::lock_guard<std::mutex> g(coresMu());
      coresLocked().push_back(this);
    } catch (...) {
      // not a fallback for others then; it runs its own tasks as above
    }
  }

  // Whether this pool is not the current process's current generation's: one a forked child
  // inherited (built in the parent, before the child started its pools over, or with no start
  // over at all). Such a pool starts no thread and runs nothing: its tasks capture the parent's
  // state, and its counts (workers, idle) are the parent's, whose threads the child does not
  // have. Asked only where a thread would be started, or a task run on the caller.
  bool foreign() const {
    return ::getpid() != pid || gen != g_gen.load(std::memory_order_acquire);
  }
  // A pool with no worker: try again to start one (at most once a second).
  bool retryWorker() {
    if (foreign())
      return false;
    const uint64_t now = nowNs();
    uint64_t last = lastTryNs.load(std::memory_order_relaxed);
    if (now - last < 1000000000ull || !lastTryNs.compare_exchange_strong(last, now))
      return workers.load(std::memory_order_acquire) > 0;
    std::string err;
    {
      std::lock_guard<std::mutex> g(mu);
      ++starting;
      ++spawned;
    }
    if (startThread([this] { loop(); }, err, name)) {
      workers.fetch_add(1, std::memory_order_release);
    } else {
      std::lock_guard<std::mutex> g(mu);
      --starting;
      --spawned;
    }
    return workers.load(std::memory_order_acquire) > 0;
  }
  // The timer thread, started at the first delayed task (tried again at most once a second). On
  // failure the workers serve the delayed tasks (loop).
  void ensureTimer() {
    if (timerOk.load(std::memory_order_acquire) || foreign())
      return;
    const uint64_t now = nowNs();
    uint64_t last = lastTimerTryNs.load(std::memory_order_relaxed);
    if ((last && now - last < 1000000000ull) || !lastTimerTryNs.compare_exchange_strong(last, now))
      return;
    std::string err;
    if (startThread([this] { timerLoop(); }, err, timerName))
      timerOk.store(true, std::memory_order_release);
    else
      sayThreadFailure(1, 1, "timer threads", err);
  }
  // Whether the tasks waiting outnumber the workers idle or on their way, and the pool may start
  // one more (below its size; not within a second of a failure to start one): then it is counted
  // as on its way, and the caller starts it (grow) once it has let go of mu. Under mu.
  bool wantMoreLocked() {
    if (queue.size() <= idle + starting || spawned >= size ||
        nowNs() - lastGrowFailNs.load(std::memory_order_relaxed) <= 1000000000ull || foreign())
      return false;
    ++starting;
    ++spawned;
    return true;
  }
  // One more worker (wantMoreLocked said so).
  void grow() {
    std::string err;
    const bool ok = startThread([this] { loop(); }, err, name);
    if (!ok) {
      std::lock_guard<std::mutex> g(mu);
      --starting;
      --spawned;
    }
    if (ok) {
      workers.fetch_add(1, std::memory_order_release);
      return;
    }
    lastGrowFailNs.store(nowNs(), std::memory_order_relaxed);
    sayThreadFailure(size - workers.load(std::memory_order_acquire), size, "worker threads", err);
  }
  // Another pool of this generation with a worker, for a pool that has none; null for none (also
  // when the list cannot be read for want of memory: the task then runs on the caller).
  Core* fallback() noexcept {
    try {
      std::lock_guard<std::mutex> g(coresMu());
      for (void* p : coresLocked()) {
        Core* c = static_cast<Core*>(p);
        if (c && c != this && c->gen == gen && !c->foreign() &&
            c->workers.load(std::memory_order_acquire) > 0)
          return c;
      }
    } catch (...) {
    }
    return nullptr;
  }

  void post(std::function<void()> task) {
    if (workers.load(std::memory_order_acquire) == 0 && !retryWorker() && !foreign()) {
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
    Task t{std::move(task), s ? nowNs() : 0};
    bool queued = false;
    bool more = false;
    {
      std::lock_guard<std::mutex> g(mu);
      try {
        queue.push_back(std::move(t)); // on failure (no memory) `t` is left as it was
        queued = true;
      } catch (...) {
      }
      // More tasks waiting than workers idle or on their way: one more, up to the pool's size.
      more = queued && wantMoreLocked();
      if (queued && s) {
        const uint64_t n = queue.size();
        uint64_t hw = s->poolQueueHighWater.load(std::memory_order_relaxed);
        while (n > hw && !s->poolQueueHighWater.compare_exchange_weak(hw, n, std::memory_order_relaxed)) {
        }
      }
    }
    if (!queued) { // no memory to queue it (an address-space limit): it runs here, said once
      noteNoMemory();
      t.fn();
      return;
    }
    cv.notify_one();
    if (more)
      grow();
  }

  void postAfter(uint64_t delayMs, std::function<void()> task, bool housekeeping) {
    const uint64_t deadline = nowNs() + delayMs * 1000000ull;
    if (workers.load(std::memory_order_acquire) == 0)
      (void)retryWorker();
    ensureTimer();
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
    if (foreign()) { // kept, never run (see foreign)
      std::lock_guard<std::mutex> t(tmu);
      timers.push(Timed{deadline, std::move(task)});
      return;
    }
    if (Core* other = fallback()) {
      other->postAfter(delayMs, std::move(task), housekeeping);
      return;
    }
    // No thread anywhere. A back-off a caller is waiting for (an open retried, a move's next
    // attempt) runs on the calling thread, after its delay when the caller may block, at once when
    // it may not: kept for a later post, it would never run when nothing else posts (the reader
    // blocked in its open). Housekeeping (the checkpoint, a parked read's insurance) is kept, and
    // runs at a later post of this pool once due (runDueTimersHere): nobody waits for it, and run
    // here it would hold up the caller (the checkpoint re-arms itself for good).
    if (!housekeeping) {
      runDelayedOnCaller(deadline, std::move(task));
      return;
    }
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
  // With no thread anywhere: the delayed tasks due now, run on the calling thread one at a time
  // (nothing allocated: this path must work when nothing else does). Those a task adds are due
  // later than `now`, so the loop ends.
  void runDueTimersHere() {
    const uint64_t now = nowNs();
    for (;;) {
      std::function<void()> f;
      {
        std::lock_guard<std::mutex> t(tmu);
        if (timers.empty() || timers.top().deadlineNs > now)
          return;
        f = std::move(const_cast<Timed&>(timers.top()).task);
        timers.pop();
      }
      if (f)
        f();
    }
  }

  void loop() {
    {
      std::lock_guard<std::mutex> g(mu);
      if (starting > 0)
        --starting; // this worker is in its loop: counted among the idle from now on
    }
    for (;;) {
      Task task;
      bool more = false;
      {
        std::unique_lock<std::mutex> lk(mu);
        for (;;) {
          if (!timerOk.load(std::memory_order_acquire)) {
            // No timer thread: the workers serve the delayed tasks.
            const uint64_t next = takeDueTimersLocked();
            if (!queue.empty())
              break;
            ++idle;
            if (next) {
              const uint64_t now = nowNs();
              cv.wait_for(lk, std::chrono::nanoseconds(next > now ? next - now : 0));
            } else {
              cv.wait(lk);
            }
            --idle;
            continue;
          }
          if (!queue.empty())
            break;
          ++idle;
          cv.wait(lk);
          --idle;
        }
        task = std::move(queue.front());
        queue.pop_front();
        // Tasks still waiting with no one free for them (a burst, or tasks that wait for one
        // another): the next worker is started here, so growth goes on without a further post.
        more = wantMoreLocked();
      }
      if (more)
        grow();
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
  const pid_t pid = ::getpid();           // the process that built it (foreign)
  const std::atomic<Stats*>* const stats; // the Executor's; it is never destroyed
  const unsigned size;                    // the most workers it starts
  const char* const name;                 // its threads' name
  const char* const timerName;            // its timer thread's
  unsigned idle = 0;                      // workers waiting for a task; under mu
  unsigned starting = 0;                  // workers started and not yet in their loop; under mu
  unsigned spawned = 0;                   // workers started or being started; under mu
  std::atomic<uint64_t> lastGrowFailNs{0}; // the last failure to start one more
  std::atomic<uint64_t> lastTimerTryNs{0}; // the last try to start the timer thread
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
Executor::Executor(unsigned threads, const char* name, const char* timerName)
    : threads_(threads), name_(name), timerName_(timerName) {}

void Executor::nameThisThread(const char* name) {
  if (!name || !*name)
    return;
  char buf[16];
  std::snprintf(buf, sizeof buf, "%s", name); // at most 15 characters
#if defined(__APPLE__)
  pthread_setname_np(buf);
#else
  pthread_setname_np(pthread_self(), buf);
#endif
}

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
  c = new Core(threads_, gen, &stats_, name_, timerName_);
  core_.store(c, std::memory_order_release);
  return c;
}

void Executor::post(std::function<void()> task) {
  Core* c = nullptr;
  try {
    c = core();
  } catch (...) { // no memory for the pool (an address-space limit): the task runs here
    noteNoMemory();
    task();
    return;
  }
  c->post(std::move(task));
}

// Never builds the pool: a question about a pool that has no threads yet starts none (each pool's
// threads take address space, which a process under a limit on it needs for other work).
bool Executor::runsThisThread() {
  const Core* c = core_.load(std::memory_order_acquire);
  return tCore != nullptr && c != nullptr && tCore == c &&
         c->gen == g_gen.load(std::memory_order_acquire);
}

void Executor::postAfter(uint64_t delayMs, std::function<void()> task, bool housekeeping) {
  if (delayMs == 0) {
    post(std::move(task));
    return;
  }
  Core* c = nullptr;
  try {
    c = core();
  } catch (...) { // no memory for the pool: as with no thread anywhere
    noteNoMemory();
    if (!housekeeping)
      runDelayedOnCaller(nowNs() + delayMs * 1000000ull, std::move(task));
    return;
  }
  c->postAfter(delayMs, std::move(task), housekeeping);
}

void Executor::setMayBlockProbe(bool (*probe)()) { g_mayBlock.store(probe, std::memory_order_release); }

Executor& Executor::instance(unsigned threads) {
  // One thread per read in flight, up to the CPUs this process may use: a
  // reader thread waits for its read, so these threads take no CPU from one
  // that is working, and the work posted here (slot-store reads and decodes,
  // first-pass requests) blocks.
  static Executor* exec =
      new Executor(threads ? threads : std::max(1u, usableCpus()), "uc-pool", "uc-pool-timer");
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
  g_failAfter.store(n < 0 ? int64_t(-1) : n, std::memory_order_relaxed);
}

unsigned Executor::startedWorkers() {
  Core* c = core();
  return c->workers.load(std::memory_order_acquire);
}

void Executor::afterForkChild() {
  g_buildMu.store(new std::mutex, std::memory_order_release); // the old one: leaked
  g_coresMu.store(new std::mutex, std::memory_order_release); // the old one: leaked
  g_cores = nullptr; // the parent's list: leaked; the child's is made at its first pool
  g_gen.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace ucache
