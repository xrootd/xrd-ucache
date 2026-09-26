#include "Executor.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace ucache {

namespace {
uint64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::atomic<uint64_t> g_gen{0};
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
} // namespace

// One fork generation's pool. Its threads capture the core, never the
// Executor, so a core outlives nothing it depends on.
struct Executor::Core {
  Core(unsigned threads, uint64_t gen) : gen(gen) {
    for (unsigned i = 0; i < threads; ++i)
      std::thread([this] { loop(); }).detach(); // leaked with the core; see header
    std::thread([this] { timerLoop(); }).detach(); // one dedicated timer thread
  }

  void post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> g(mu);
      queue.push_back(std::move(task));
    }
    cv.notify_one();
  }

  void postAfter(uint64_t delayMs, std::function<void()> task) {
    const uint64_t deadline = nowNs() + delayMs * 1000000ull;
    {
      std::lock_guard<std::mutex> g(tmu);
      timers.push(Timed{deadline, std::move(task)});
    }
    tcv.notify_one();
  }

  void loop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return !queue.empty(); });
        task = std::move(queue.front());
        queue.pop_front();
      }
      task(); // tasks must not throw (no exceptions cross the ABI)
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

  const uint64_t gen;
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> queue;

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
  c = new Core(threads_, gen);
  core_.store(c, std::memory_order_release);
  return c;
}

void Executor::post(std::function<void()> task) { core()->post(std::move(task)); }

void Executor::postAfter(uint64_t delayMs, std::function<void()> task) {
  if (delayMs == 0) {
    post(std::move(task));
    return;
  }
  core()->postAfter(delayMs, std::move(task));
}

Executor& Executor::instance(unsigned threads) {
  static Executor* exec = new Executor(
      threads ? threads
              : std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
  return *exec;
}

uint64_t Executor::forkGeneration() { return g_gen.load(std::memory_order_acquire); }

void Executor::afterForkChild() {
  g_buildMu.store(new std::mutex, std::memory_order_release); // the old one: leaked
  g_gen.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace ucache
