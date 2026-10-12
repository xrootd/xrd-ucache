#include "FetchCheck.h"

#include "Executor.h"
#include "UsableCpus.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
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
bool decodeThrows() {
  static const bool on = std::getenv("UCACHE_TEST_DECODE_THROW") != nullptr;
  return on;
}
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

// One job, on whichever thread: never throws. A job that throws (out of memory, or a test's hook)
// leaves its part not judged (no claim; `unjudged`), and the count goes down in any case, so the
// continuation runs once whatever happened.
void runJob(const std::shared_ptr<Join>& j, size_t i) noexcept {
  try {
    if (j->onJob)
      j->onJob();
    if (const long ms = decodeDelayMs(); ms > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    if (decodeThrows())
      throw std::bad_alloc(); // test hook: a job out of memory
    j->parts[i] = tp::runCheckJob(*j->seeks, *j->lengths, j->jobs[i], j->data, j->reach.get());
  } catch (...) {
    j->parts[i] = tp::FetchVerdict();
    j->parts[i].unjudged = 1;
  }
  if (j->left.fetch_sub(1, std::memory_order_acq_rel) != 1)
    return;
  tp::FetchVerdict v;
  try {
    v = std::move(j->base);
    for (auto& part : j->parts)
      v.merge(std::move(part));
    v.sortFaults();
  } catch (...) {
    v = tp::FetchVerdict(); // the merge could not be made: no claim
    v.unjudged = static_cast<uint32_t>(j->parts.size());
  }
  auto done = std::move(j->done);
  try {
    done(std::move(v));
  } catch (...) {
    // the caller's continuation must not throw; nothing may leave a pool thread
  }
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
      pool.post([j, i] {
        tp::keepDecodersOnThisThread(); // the pool's threads keep their decoders (TreeMeta.h)
        runJob(j, i);
      });
    } catch (...) {
      runJob(j, i); // could not be posted: judged here
    }
  }
}

} // namespace ucache
