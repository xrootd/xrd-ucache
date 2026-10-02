// A fixed CPU workload, timed on this machine, for `ucache publish`.
//
// A run's wall and its instruction count say what one job cost on one
// machine. Putting two machines on one scale needs a measurement that is the
// SAME work everywhere, and that is all this is: a small, deterministic piece
// of what an analysis does -- decode a buffer compressed with LZMA and one
// compressed with ZLIB, then select dimuon events in them and compute each
// pair's invariant mass -- timed once on one thread and once on every CPU the
// process may use, at once. The report service does the interpreting; the
// client only measures and sends.
//
// The data is generated from a fixed seed, bit for bit the same on every
// machine, and compressed once, untimed, so the decoders see the same stream
// wherever the codec library encodes it the same way. The codecs
// are the ones the transposer already links; a build without them has no
// workload to offer and reports `ok == false`, and the publish then carries
// no calibration rather than a different one.
//
// About 0.3 s of CPU on one thread of a 2019 server core (less on anything
// newer), then as long again on every CPU the process may use at once -- twice
// that where two hardware threads share a core -- plus a seventh of a second
// to prepare: a publish waits about a second.
//
// Thread-safety: runCpuBench() starts and joins its own threads and shares
// nothing between calls; usableCpus() and cgroupCpuQuota() only read files;
// everything else is a pure function of its arguments.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "UsableCpus.h"

namespace ucache {

struct Json;

// Repetitions of the decode-and-compute pass that make up the workload. Part
// of its definition, like the seed and the sizes: changing any of them makes a
// different workload, and `kCpuBenchVersion` then has to change with it.
inline constexpr unsigned kCpuBenchReps = 9;
inline constexpr int kCpuBenchVersion = 1;

// The workload's input, prepared once.
struct CpuBenchInput {
  std::vector<float> events; // the uncompressed columns, for checking a decode
  std::string lzma, zlib;    // the same bytes compressed with each codec
  bool ok = false;           // false: no codecs in this build, or a codec failed
};
CpuBenchInput prepareCpuBench();

// One pass of the work on the calling thread, `reps` times over: returns the
// work's own result -- a hash of the mass histogram and the decoded bits -- so
// two runs can be checked to have done the same thing. 0 when a decode failed.
uint64_t cpuBenchWork(const CpuBenchInput& in, unsigned reps);

struct CpuBenchResult {
  bool ok = false;
  double t1CpuS = 0.0, t1WallS = 0.0; // the work once, on one thread
  unsigned tnThreads = 0;             // the work on every core at once
  double tnCpuS = 0.0, tnWallS = 0.0;
  // "perf" (Linux perf_event) or "rusage" (macOS per-process counters); empty
  // when the machine gives no instruction counts, and the counts are then 0.
  std::string counterSource;
  uint64_t t1Instructions = 0, tnInstructions = 0;
  uint64_t checksum = 0; // cpuBenchWork's result, the same on every thread
};

struct CpuBenchOptions {
  unsigned reps = kCpuBenchReps;
  unsigned threads = 0; // 0: one per CPU this process may use (usableCpus)
};
CpuBenchResult runCpuBench(const CpuBenchOptions& opt = {});


// `machine.calib` for the publish payload. Instructions only where counted.
Json cpuBenchJson(const CpuBenchResult& r);

} // namespace ucache
