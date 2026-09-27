#include "CpuCounters.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <sys/resource.h>
#include <unistd.h>

#if defined(__linux__)
#include <cstring>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#endif

namespace ucache {

#if defined(__linux__)
namespace {
// `inherit` follows the whole task tree, and a task tree includes anything the
// process SPAWNS. This one spawns a recompression helper that decodes and
// re-encodes gigabytes, so its instructions landed in the run's own count --
// and the same work with recompression off then looked like different work,
// which is the one comparison these counters exist to support. Asking the
// kernel to drop the event when a task execs removes the helper without
// touching the threads that do the analysis, since those never exec.
//
// Kernels before 5.13 have no such bit and reject the attribute outright, so
// the open is retried without it rather than losing the counters entirely.
int openCounter(uint64_t config, bool dropOnExec, int pid) {
  struct perf_event_attr attr;
  ::memset(&attr, 0, sizeof attr);
  attr.type = PERF_TYPE_HARDWARE;
  attr.size = sizeof attr;
  attr.config = config;
  attr.disabled = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.inherit = 1; // count every thread this process spawns
#if defined(PERF_ATTR_SIZE_VER7)
  if (dropOnExec) {
    attr.remove_on_exec = 1;
    // Threads only, not forked processes: a forked worker counts its own work
    // in its own record (CpuCounters::reopen), and a parent's count must not
    // grow by its children's when they exit. Same kernel generation as the bit
    // above; older kernels take the retry below and count children too.
    attr.inherit_thread = 1;
  }
#else
  (void)dropOnExec;
#endif
  // CLOEXEC as well: the helper has no use for the descriptor either.
  const int fd = static_cast<int>(::syscall(SYS_perf_event_open, &attr, pid, /*cpu=*/-1,
                                            /*group=*/-1, PERF_FLAG_FD_CLOEXEC));
  return fd; // < 0 simply means "not permitted here"
}

// pid 0: the calling thread and every thread started after; a thread id: that
// thread and every thread it starts after.
int openCounter(uint64_t config, int pid = 0) {
  const int fd = openCounter(config, /*dropOnExec=*/true, pid);
  const int any = fd >= 0 ? fd : openCounter(config, /*dropOnExec=*/false, pid);
  if (any >= 0)
    ::ioctl(any, PERF_EVENT_IOC_ENABLE, 0);
  return any;
}

// Calls f(tid) for each thread of this process but the calling one. No memory
// is allocated: this runs in a fork child handler.
template <typename F> void forOtherThreads(F f) {
  const int dir = ::open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0)
    return;
  const long self = ::syscall(SYS_gettid);
  alignas(8) char buf[4096];
  for (;;) {
    const long n = ::syscall(SYS_getdents64, dir, buf, sizeof buf);
    if (n <= 0)
      break;
    for (long at = 0; at < n;) {
      const auto* d = reinterpret_cast<const struct dirent64*>(buf + at);
      at += d->d_reclen;
      long tid = 0;
      const char* c = d->d_name;
      for (; *c >= '0' && *c <= '9'; ++c)
        tid = tid * 10 + (*c - '0');
      if (*c == 0 && tid > 0 && tid != self)
        f(static_cast<int>(tid));
    }
  }
  ::close(dir);
}

uint64_t readCounter(int fd) {
  if (fd < 0)
    return 0;
  uint64_t v = 0;
  if (::read(fd, &v, sizeof v) != static_cast<ssize_t>(sizeof v))
    return 0;
  return v;
}
} // namespace

CpuCounters::CpuCounters() {
  insFd_ = openCounter(PERF_COUNT_HW_INSTRUCTIONS);
  cycFd_ = openCounter(PERF_COUNT_HW_CPU_CYCLES);
}

void CpuCounters::closeAll() {
  if (insFd_ >= 0)
    ::close(insFd_);
  if (cycFd_ >= 0)
    ::close(cycFd_);
  insFd_ = cycFd_ = -1;
  for (int i = 0; i < nOther_; ++i) {
    if (otherIns_[i] >= 0)
      ::close(otherIns_[i]);
    if (otherCyc_[i] >= 0)
      ::close(otherCyc_[i]);
  }
  nOther_ = 0;
}

CpuCounters::~CpuCounters() { closeAll(); }

uint64_t CpuCounters::instructions() const {
  uint64_t v = readCounter(insFd_);
  for (int i = 0; i < nOther_; ++i)
    v += readCounter(otherIns_[i]);
  return v;
}
uint64_t CpuCounters::cycles() const {
  uint64_t v = readCounter(cycFd_);
  for (int i = 0; i < nOther_; ++i)
    v += readCounter(otherCyc_[i]);
  return v;
}

void CpuCounters::reopen() {
  closeAll();
  insFd_ = openCounter(PERF_COUNT_HW_INSTRUCTIONS);
  cycFd_ = openCounter(PERF_COUNT_HW_CPU_CYCLES);
  if (insFd_ < 0 || cycFd_ < 0)
    return;
  // Threads the client library's own fork handler started before this one
  // ran (its I/O threads): each is counted on its own, as in a process that
  // never forked, where they start after the counters opened.
  forOtherThreads([this](int tid) {
    if (nOther_ < kMaxOther) {
      otherIns_[nOther_] = openCounter(PERF_COUNT_HW_INSTRUCTIONS, tid);
      otherCyc_[nOther_] = openCounter(PERF_COUNT_HW_CPU_CYCLES, tid);
      ++nOther_;
    }
  });
}

#else  // not Linux: no perf_event_open, rusage still works

CpuCounters::CpuCounters() = default;
CpuCounters::~CpuCounters() = default;
uint64_t CpuCounters::instructions() const { return 0; }
uint64_t CpuCounters::cycles() const { return 0; }
void CpuCounters::reopen() {}

#endif

uint64_t CpuCounters::liveThreads() {
#if defined(__linux__)
  FILE* f = ::fopen("/proc/self/status", "re");
  if (!f)
    return 0;
  char buf[256];
  uint64_t n = 0;
  while (::fgets(buf, sizeof buf, f))
    if (::strncmp(buf, "Threads:", 8) == 0) {
      n = ::strtoull(buf + 8, nullptr, 10);
      break;
    }
  ::fclose(f);
  return n;
#else
  return 0;
#endif
}

uint64_t CpuCounters::processCpuUs() {
  struct rusage ru;
  if (::getrusage(RUSAGE_SELF, &ru) != 0)
    return 0;
  const uint64_t u = static_cast<uint64_t>(ru.ru_utime.tv_sec) * 1000000ull +
                     static_cast<uint64_t>(ru.ru_utime.tv_usec);
  const uint64_t s = static_cast<uint64_t>(ru.ru_stime.tv_sec) * 1000000ull +
                     static_cast<uint64_t>(ru.ru_stime.tv_usec);
  return u + s;
}

void WidthSampler::sample() {
  struct timespec ts;
  if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return;
  const uint64_t wall = static_cast<uint64_t>(ts.tv_sec) * 1000000ull +
                        static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
  // Lock-free gate: this is called from the read path, which runs millions of
  // times, and all but one call a second must cost a load and a compare.
  uint64_t due = nextSampleUs_.load(std::memory_order_relaxed);
  if (wall < due)
    return;
  if (!nextSampleUs_.compare_exchange_strong(due, wall + 1000000ull,
                                             std::memory_order_relaxed))
    return; // another thread is taking this second's sample
  const uint64_t cpu = CpuCounters::processCpuUs();
  std::lock_guard<std::mutex> g(mu_);
  if (lastWallUs_ == 0) {
    lastWallUs_ = wall;
    lastCpuUs_ = cpu;
    return;
  }
  const uint64_t dw = wall - lastWallUs_;
  if (dw == 0)
    return;
  const double cores = static_cast<double>(cpu - lastCpuUs_) / static_cast<double>(dw);
  lastWallUs_ = wall;
  lastCpuUs_ = cpu;
  if (cores > peakCores_)
    peakCores_ = cores;
}

uint64_t WidthSampler::width() const {
  std::lock_guard<std::mutex> g(mu_);
  return static_cast<uint64_t>(peakCores_ + 0.5);
}

void WidthSampler::afterForkChild() {
  new (&mu_) std::mutex; // whatever held it is not in this process
  nextSampleUs_.store(0, std::memory_order_relaxed);
  lastCpuUs_ = 0;
  lastWallUs_ = 0;
  peakCores_ = 0.0;
}

WidthSampler& widthSampler() {
  static WidthSampler* s = new WidthSampler(); // never destroyed, on purpose
  return *s;
}

} // namespace ucache
