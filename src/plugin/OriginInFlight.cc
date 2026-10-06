#include "OriginInFlight.h"

#include "Executor.h"

#include <mutex>
#include <unordered_map>

namespace ucache {
namespace {

// One fork generation's servers, one counter each. Leaked, counters included:
// a guard may give its read back after anything that could own them is gone,
// and a forked child must never take a lock a parent thread held at the fork.
struct ServerCounts {
  explicit ServerCounts(uint64_t g) : gen(g) {}
  const uint64_t gen;
  std::mutex mu; // find or insert only; a counter is used without it
  std::unordered_map<std::string, std::atomic<uint64_t>*> counts;
};
// Constant-initialized and built at first use, as the executor's core is: the
// plugin can be called before this library's dynamic initializers have run.
std::atomic<ServerCounts*> g_counts{nullptr};

ServerCounts* current() {
  const uint64_t gen = Executor::forkGeneration();
  ServerCounts* c = g_counts.load(std::memory_order_acquire);
  while (!c || c->gen != gen) {
    auto* fresh = new ServerCounts(gen);
    if (g_counts.compare_exchange_strong(c, fresh, std::memory_order_acq_rel))
      return fresh; // the previous generation's is left as it is
    delete fresh;   // another thread installed one first; `c` is now that one
  }
  return c;
}

} // namespace

std::atomic<uint64_t>* serverInFlight(const std::string& server) noexcept {
  if (server.empty())
    return nullptr;
  try {
    ServerCounts* c = current();
    std::lock_guard<std::mutex> g(c->mu);
    std::atomic<uint64_t>*& n = c->counts[server];
    if (!n)
      n = new std::atomic<uint64_t>(0);
    return n;
  } catch (...) {
    return nullptr; // not counted for this server, as for an unknown one
  }
}

} // namespace ucache
