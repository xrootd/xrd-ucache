// How many CPUs this process may use: the plugin sizes its thread pools by it,
// and `ucache publish`'s CPU calibration runs one thread per CPU it counts.
//
// Thread-safety: these only read files and call sched_getaffinity; every
// other function here is a pure function of its arguments.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ucache {

// The CPUs this process may use: its affinity mask (a batch slot is often
// given fewer CPUs than the machine has), capped by a cgroup CPU quota when
// one is set (cgroupCpuQuota). Else every CPU on line.
unsigned usableCpus();

// How many CPUs' worth of time a cgroup CPU QUOTA gives this process, rounded
// up -- a limit of 2.5 CPUs is 3 -- or 0 when no quota applies. A quota is what
// container and Kubernetes CPU limits set, and it leaves the affinity mask at
// every CPU of the host: counting the mask alone runs one thread per host CPU
// through a few CPUs' worth of time. Read from cgroup v2 `cpu.max` and cgroup
// v1 `cpu.cfs_quota_us` / `cpu.cfs_period_us`, in the process's own cgroup and
// every one above it up to the root of its hierarchy, and the tightest wins.
// `read(path, text)` returns a file's text, false when there is none; the
// second form reads this process's own (/proc/self/cgroup and mountinfo, and
// the files under the mount points they name).
using ReadTextFn = std::function<bool(const std::string& path, std::string& text)>;
unsigned cgroupCpuQuota(const ReadTextFn& read);
unsigned cgroupCpuQuota();

// Parts of it, for tests. A cgroup v2 `cpu.max` ("<quota> <period>", or "max
// <period>" for none): false when there is no quota. The CPUs a quota gives,
// rounded up (at least 1); 0 for a period of 0.
bool parseCpuMax(const std::string& text, uint64_t& quota, uint64_t& period);
unsigned cpusForQuota(uint64_t quota, uint64_t period);

} // namespace ucache
