// The byte path's check of fetched bytes (check_origin_integrity), done in parallel on a pool of
// its own: a fetch's baskets are judged (their keys, and their data decoded to verify its own
// checksum) as jobs of about a megabyte each (transpose::checkJobs), spread over the decode pool,
// and the caller's continuation runs once, on the thread that finished the last job.
//
// The decode pool is not the executor that serves hits and writes pages: a fetch's decoding never
// holds back another handle's hit. It has one thread per CPU this process may use, at most 16:
// with 64 threads, a reader limited to 8 GiB of address space (a gate's) ran out of it. Memory in
// flight: what each decode thread keeps (its decoders' state, an LZMA dictionary among them, and
// a scratch buffer of at most one block's 16 MiB; transpose::verifyFrames), and the bytes joined
// for baskets that a cut between two pieces of one fetch divides (one basket per cut); the
// fetched bytes themselves are the caller's.
//
// Thread-safety: thread-safe; the caller keeps the pieces' bytes alive until `done` has run.
#pragma once

#include "TreeMeta.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ucache {

class Executor;

// The decode pool (process-wide, built at first use): one thread per usable CPU, at most
// kMaxDecodeThreads.
constexpr unsigned kMaxDecodeThreads = 16;
Executor& decodePool();

struct FetchCheckOptions {
  uint64_t sliceBytes = 1u << 20; // the bytes a job judges, by where its baskets start
  std::function<void()> onJob;    // tests: run at the start of each job, on its thread
};

using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;

// transpose::checkPieces' work, on `pool`: `done` is called exactly once, with the merged verdict
// (faults by position), on the pool thread that finished the last job, or at once on this thread
// when there is no job. `keep` holds `seeks`, `lengths` and `damaged` alive (the file map);
// `reach`, when set, limits decoding to the baskets it reaches (FetchJudge). A job that cannot be
// posted runs on this thread. Test hooks: UCACHE_TEST_DECODE_DELAY_MS holds each job that long;
// UCACHE_TEST_DECODE_THROW makes each job throw, as one out of memory (its part not judged).
void checkPiecesOn(Executor& pool, std::shared_ptr<const void> keep,
                   const std::vector<uint64_t>& seeks, const std::vector<int32_t>& lengths,
                   std::vector<transpose::BytePiece> pieces, bool data, const Ranges* damaged,
                   const std::string& why, std::shared_ptr<const Ranges> reach,
                   std::function<void(transpose::FetchVerdict)> done,
                   const FetchCheckOptions& options = FetchCheckOptions());

} // namespace ucache
