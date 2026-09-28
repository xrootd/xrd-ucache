// Per-process CPU accounting for the run record: how much work a run did, and
// how much of its wall the machine actually spent doing it.
//
// Two sources, deliberately both:
//
// * `getrusage` — CPU time for the whole process, always available. Complete,
//   but coarse: it cannot separate work from spin, and it says nothing about
//   how efficiently the work executed.
// * `perf_event_open` — retired instructions and cycles. Precise, and their
//   ratio is the only way to tell "the same job ran slower" from "a different
//   job ran". BUT the counters can only start when this object is built, and
//   the plugin is loaded on the first client call — so a process that spends
//   time before touching a file has that time in rusage and NOT in the
//   counters. Consumers must treat instruction totals as "since the cache
//   engaged", never as "for the process".
//
// Instruction totals are the work fingerprint: two runs that executed the same
// number of instructions did the same computation, which is what makes their
// walls comparable. Measured on this project, a cached and a cache-disabled run
// of one job agreed to 2.5% on instructions and 0.6% on cycles while their
// walls differed 2.5x.
//
// perf_event_open is unavailable under some kernel settings and in some
// containers. That is not an error: the counters simply report zero and every
// consumer falls back to rusage.
//
// A hardware counter can be SHARED: when more events want the PMU than it has
// registers -- another job's counters on a batch node, a profiler -- the kernel
// time-slices them, and each event counts only while it holds a register. Read
// as a bare value, such a counter under-reports by exactly the share of time it
// was switched out, silently (a pool of shared batch nodes read 56% of the true
// count this way). So every read also takes the kernel's two times, enabled and
// running, the totals are scaled up by enabled/running as `perf stat` does, and
// the ratio itself -- the duty cycle -- is reported beside them, so a consumer
// can tell a measured count from an extrapolated one.
//
// macOS has no perf_event_open, but its kernel keeps per-process instruction
// and cycle totals (proc_pid_rusage, RUSAGE_INFO_V4). Those cover the WHOLE
// process from its start, user and kernel, rather than user space since the
// cache engaged, so the two sources are named in the record and are not
// comparable with each other.
//
// Thread-safety: open/read from any thread; the counters aggregate all threads
// of this process (inherit=1). Reads are independent.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ucache {

// One read of a perf counter: its value and the kernel's two times, in ns --
// how long the event was enabled and how long it actually counted.
struct CounterReading {
  uint64_t value = 0;
  uint64_t enabled = 0;
  uint64_t running = 0;
};

// Several readings of ONE event (one per descriptor that counts part of the
// process) summed, each scaled up by enabled/running for the time it was
// switched out, and the event's duty cycle: running over enabled, summed over
// the readings. A reading that was never enabled adds nothing to either; one
// that was enabled and never ran adds nothing to the value (there is nothing
// to scale) and pulls the duty down. With nothing enabled at all the duty is
// 1: nothing was lost. Pure, so the arithmetic is tested on its own.
struct ScaledCount {
  uint64_t value = 0;
  double duty = 1.0;
};
ScaledCount scaleCounter(const CounterReading* readings, int n);

// A duty cycle as the record writes it: four decimals, never in exponent form
// and independent of the process locale. Values are clamped to [0, 1].
std::string dutyText(double duty);

class CpuCounters {
 public:
  CpuCounters();  // opens and enables the counters if the kernel permits
  ~CpuCounters();
  CpuCounters(const CpuCounters&) = delete;
  CpuCounters& operator=(const CpuCounters&) = delete;

  // Retired instructions and CPU cycles, read together. On Linux they count
  // user space since construction and are scaled for multiplexing (`duty` is
  // the smaller of the two events' duty cycles); on macOS they are the
  // process's own totals and `duty` stays 1. `source` names where they came
  // from -- "perf" or "rusage" -- and is null when there are no counters, in
  // which case the values are 0.
  struct Sample {
    uint64_t instructions = 0;
    uint64_t cycles = 0;
    double duty = 1.0;
    const char* source = nullptr;
  };
  Sample sample() const;
  // True when this platform gave us the counters.
  bool available() const;
  // Close and open the counters again, counting from now. A forked child's
  // descriptors are copies of the parent's and read the PARENT's event, so a
  // child that keeps a record of its own opens its own -- before it starts any
  // thread, since a counter follows only threads started after it opened; the
  // threads already running (the client library's, started by its own fork
  // handler) get a counter each. System calls only, no lock and no
  // allocation: safe in a fork child handler.
  void reopen();

  // Process CPU time (user+system) in microseconds, from getrusage. Always
  // available, and covers the whole process rather than only our lifetime.
  static uint64_t processCpuUs();

  // Threads alive in this process right now (/proc/self/status). 0 where
  // unavailable. Sampled while the run is active rather than at exit, because
  // by exit the pool has wound down and the peak is what a wall must be
  // divided by to mean anything.
  static uint64_t liveThreads();

#if defined(__linux__) // perf_event descriptors; other platforms keep no state
 private:
  void closeAll();

  static constexpr int kMaxOther = 64;
  int insFd_ = -1;
  int cycFd_ = -1;
  int otherIns_[kMaxOther];
  int otherCyc_[kMaxOther];
  int nOther_ = 0; // written only by reopen (a fork child, one thread)
#endif
};

// How wide a run COULD go, measured rather than declared.
//
// The job's own thread setting is invisible from here and must stay that way,
// so the question is turned around: what is the most parallelism this run ever
// actually reached? CPU time sampled once a second gives cores-busy per
// interval, and the MAXIMUM over the run is the answer. A job limited to eight
// threads can never exceed eight cores, not even for one second; a job given
// thirty-two still bursts to thirty-two whenever data is there, however
// starved it is on average. So the peak reflects the ceiling the job was
// given, while the mean reflects how well it was fed -- and the two together
// say whether a run used the resources it had.
//
// Two measures that were tried and are worse: threads ALIVE counts every
// thread the process owns (33 for an eight-core job, 640 for an RNTuple one,
// and it differs by route for the same job), and threads that ever used CPU,
// even weighted by how much, returns the same 33 because ROOT spreads work
// across its whole arena.
//
// Sampling must be driven by something that happens THROUGHOUT the run. Driven
// by file opens it measured the opening burst and nothing else, and reported a
// peak of 1 core for a 32-thread job -- opens all land before the compute
// starts. Reads run for the whole run, so that is where it is called from, and
// the once-a-second check is a lock-free load so a hot path can afford it.
class WidthSampler {
 public:
  void sample();
  // Peak cores busy over any sampled interval; 0 before the second sample.
  uint64_t width() const;
  // A forked child starts sampling afresh: its CPU time restarts at zero, and
  // the lock may have been held by a thread that stayed in the parent. Called
  // from the plugin's fork child handler only (single-threaded).
  void afterForkChild();

 private:
  std::atomic<uint64_t> nextSampleUs_{0}; // lock-free gate for the hot path
  mutable std::mutex mu_;
  uint64_t lastCpuUs_ = 0;
  uint64_t lastWallUs_ = 0;
  double peakCores_ = 0.0;
};

WidthSampler& widthSampler();

} // namespace ucache
