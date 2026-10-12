#include "FetchCheck.h"

#include "Executor.h"
#include "UsableCpus.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace ucache {

namespace tp = transpose;

Executor& decodePool() {
  // One thread per usable CPU, at most kMaxDecodeThreads: each thread keeps its decoders' state,
  // an LZMA dictionary of 64 MiB of address space among them (ROOT's LZMA level 9), and a reader
  // run under a limit on its address space must not lose it to them.
  static Executor* pool = new Executor(
      std::max(1u, std::min(usableCpus(), kMaxDecodeThreads))); // leaked, as the executor
  return *pool;
}

namespace {
long decodeDelayMs() {
  static const long ms = [] {
    const char* v = std::getenv("UCACHE_TEST_DECODE_DELAY_MS");
    return v ? std::strtol(v, nullptr, 10) : 0L;
  }();
  return ms;
}

struct Join {
  std::shared_ptr<const void> keep;
  const std::vector<uint64_t>* seeks = nullptr;
  const std::vector<int32_t>* lengths = nullptr;
  bool data = true;
  std::shared_ptr<const Ranges> reach;
  std::vector<tp::CheckJob> jobs;
  std::vector<tp::FetchVerdict> parts;
  tp::FetchVerdict base;
  std::function<void()> onJob;
  std::function<void(tp::FetchVerdict)> done;
  std::atomic<size_t> left{0};
};

void runJob(const std::shared_ptr<Join>& j, size_t i) {
  if (j->onJob)
    j->onJob();
  if (const long ms = decodeDelayMs(); ms > 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  j->parts[i] = tp::runCheckJob(*j->seeks, *j->lengths, j->jobs[i], j->data, j->reach.get());
  if (j->left.fetch_sub(1, std::memory_order_acq_rel) != 1)
    return;
  tp::FetchVerdict v = std::move(j->base);
  for (auto& part : j->parts)
    v.merge(std::move(part));
  v.sortFaults();
  auto done = std::move(j->done);
  done(std::move(v));
}
} // namespace

void checkPiecesOn(Executor& pool, std::shared_ptr<const void> keep,
                   const std::vector<uint64_t>& seeks, const std::vector<int32_t>& lengths,
                   std::vector<tp::BytePiece> pieces, bool data, const Ranges* damaged,
                   const std::string& why, std::shared_ptr<const Ranges> reach,
                   std::function<void(tp::FetchVerdict)> done, const FetchCheckOptions& options) {
  auto j = std::make_shared<Join>();
  j->keep = std::move(keep);
  j->seeks = &seeks;
  j->lengths = &lengths;
  j->data = data;
  j->reach = std::move(reach);
  j->onJob = options.onJob;
  Ranges runs;
  j->jobs = tp::checkJobs(seeks, lengths, std::move(pieces), options.sliceBytes, &runs);
  if (damaged)
    for (const auto& [off, n] : runs)
      j->base.damaged(*damaged, off, n, why);
  if (j->jobs.empty()) {
    done(std::move(j->base));
    return;
  }
  j->parts.resize(j->jobs.size());
  j->done = std::move(done);
  j->left.store(j->jobs.size(), std::memory_order_release);
  const size_t n = j->jobs.size();
  for (size_t i = 0; i < n; ++i) {
    try {
      pool.post([j, i] { runJob(j, i); });
    } catch (...) {
      runJob(j, i); // could not be posted: judged here
    }
  }
}

} // namespace ucache
