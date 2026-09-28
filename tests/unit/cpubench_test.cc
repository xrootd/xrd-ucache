// The fixed CPU workload `ucache publish` times. What matters is that it is
// the SAME work on every run and every thread -- a calibration that varied
// would put two machines on two different scales -- and that what it reports
// is shaped like a measurement. How fast this machine is is not tested.
#include "CpuBench.h"

#include "Publish.h"
#include <gtest/gtest.h>
#include <map>
#include <string>

using namespace ucache;

#if defined(UCACHE_CPUBENCH_CODECS)

TEST(CpuBench, TheInputIsTheSameEveryTime) {
  const CpuBenchInput a = prepareCpuBench();
  const CpuBenchInput b = prepareCpuBench();
  ASSERT_TRUE(a.ok);
  ASSERT_TRUE(b.ok);
  EXPECT_EQ(a.events, b.events);
  EXPECT_EQ(a.lzma, b.lzma);
  EXPECT_EQ(a.zlib, b.zlib);
  // Both codecs actually compress it: quantized floats, not noise.
  const size_t raw = a.events.size() * sizeof(float);
  EXPECT_LT(a.lzma.size(), raw);
  EXPECT_LT(a.zlib.size(), raw);
  EXPECT_LT(a.lzma.size(), a.zlib.size()); // LZMA is the heavier codec
}

TEST(CpuBench, TheWorkIsDeterministic) {
  const CpuBenchInput in = prepareCpuBench();
  ASSERT_TRUE(in.ok);
  const uint64_t once = cpuBenchWork(in, 1);
  EXPECT_NE(once, 0u);
  EXPECT_EQ(once, cpuBenchWork(in, 1));
  // ... and depends on how much of it was done, so a short pass cannot pass
  // for a full one.
  const uint64_t twice = cpuBenchWork(in, 2);
  EXPECT_NE(twice, 0u);
  EXPECT_NE(twice, once);
  EXPECT_EQ(twice, cpuBenchWork(in, 2));
}

TEST(CpuBench, ADamagedStreamIsNotTimed) {
  CpuBenchInput in = prepareCpuBench();
  ASSERT_TRUE(in.ok);
  CpuBenchInput lz = in;
  lz.lzma[lz.lzma.size() / 2] ^= 0x5a;
  EXPECT_EQ(cpuBenchWork(lz, 1), 0u);
  CpuBenchInput z = in;
  z.zlib[z.zlib.size() / 2] ^= 0x5a;
  EXPECT_EQ(cpuBenchWork(z, 1), 0u);
  in.ok = false;
  EXPECT_EQ(cpuBenchWork(in, 1), 0u);
}

TEST(CpuBench, EveryThreadDoesTheSameWorkAndTheResultIsSane) {
  CpuBenchOptions opt;
  opt.reps = 1;
  opt.threads = 3;
  const CpuBenchResult r = runCpuBench(opt);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.checksum, cpuBenchWork(prepareCpuBench(), 1));
  EXPECT_EQ(r.tnThreads, 3u);
  EXPECT_GT(r.t1CpuS, 0.0);
  EXPECT_GT(r.t1WallS, 0.0);
  EXPECT_GT(r.tnCpuS, 0.0);
  EXPECT_GT(r.tnWallS, 0.0);
  // CPU time on one thread cannot exceed its wall by more than clock grain;
  // three threads' CPU cannot exceed three walls.
  EXPECT_LE(r.t1CpuS, r.t1WallS * 1.05 + 0.01);
  EXPECT_LE(r.tnCpuS, 3.0 * r.tnWallS * 1.05 + 0.01);
  // Three times the work took at least about twice the CPU of once.
  EXPECT_GT(r.tnCpuS, 2.0 * r.t1CpuS);
  if (r.counterSource.empty()) {
    EXPECT_EQ(r.t1Instructions, 0u);
    EXPECT_EQ(r.tnInstructions, 0u);
  } else {
    EXPECT_TRUE(r.counterSource == "perf" || r.counterSource == "rusage");
    EXPECT_GT(r.t1Instructions, 0u);
    // Instructions are the work, not the time: three copies of it retire
    // about three times as many, whatever the machine was doing meanwhile.
    const double ratio =
        static_cast<double>(r.tnInstructions) / static_cast<double>(r.t1Instructions);
    EXPECT_GT(ratio, 2.7);
    EXPECT_LT(ratio, 3.3);
  }
}

#else

TEST(CpuBench, WithoutCodecsThereIsNoWorkload) {
  EXPECT_FALSE(prepareCpuBench().ok);
  EXPECT_FALSE(runCpuBench().ok);
}

#endif

TEST(CpuBench, TheJsonCarriesTheFieldsAndInstructionsOnlyWhereCounted) {
  CpuBenchResult r;
  r.ok = true;
  r.t1CpuS = 0.30123;
  r.t1WallS = 0.30456;
  r.tnThreads = 64;
  r.tnCpuS = 19.5;
  r.tnWallS = 0.41;
  const Json plain = cpuBenchJson(r);
  EXPECT_EQ(plain.dump(), "{\"version\":1,\"t1_cpu_s\":0.3012,\"t1_wall_s\":0.3046,"
                          "\"tn_threads\":64,\"tn_cpu_s\":19.5000,\"tn_wall_s\":0.4100}");
  r.counterSource = "perf";
  r.t1Instructions = 1234567890123ull;
  r.tnInstructions = 79012345678901ull;
  const Json counted = cpuBenchJson(r);
  EXPECT_EQ(counted.str("counter_source"), "perf");
  ASSERT_NE(counted.get("t1_instructions"), nullptr);
  EXPECT_EQ(counted.get("t1_instructions")->s, "1234567890123");
  EXPECT_EQ(counted.get("tn_instructions")->s, "79012345678901");
}

// ---- the CPUs a cgroup quota gives ------------------------------------------
//
// A container's CPU limit is a quota, not an affinity mask: the mask still
// names every CPU of the host. The n-thread pass takes the quota, rounded up,
// when it is tighter than the mask.

namespace {
// A made-up /proc and /sys: path -> text.
struct Tree {
  std::map<std::string, std::string> files;
  ReadTextFn fn() const {
    return [this](const std::string& path, std::string& text) {
      auto it = files.find(path);
      if (it == files.end())
        return false;
      text = it->second;
      return true;
    };
  }
};
const char* kV2Mount =
    "25 30 0:22 / /sys/fs/cgroup rw,nosuid,nodev,noexec,relatime shared:4 - cgroup2 cgroup2 "
    "rw,nsdelegate,memory_recursiveprot\n";
} // namespace

TEST(CpuQuota, CpuMaxParses) {
  uint64_t q = 0, p = 0;
  EXPECT_FALSE(parseCpuMax("max 100000\n", q, p)) << "no quota";
  ASSERT_TRUE(parseCpuMax("250000 100000\n", q, p));
  EXPECT_EQ(q, 250000u);
  EXPECT_EQ(p, 100000u);
  EXPECT_FALSE(parseCpuMax("", q, p));
  EXPECT_FALSE(parseCpuMax("250000", q, p));
  EXPECT_FALSE(parseCpuMax("0 100000", q, p));
  EXPECT_FALSE(parseCpuMax("250000 0", q, p));
  EXPECT_FALSE(parseCpuMax("25x 100000", q, p));
  EXPECT_EQ(cpusForQuota(250000, 100000), 3u) << "2.5 CPUs of time: 3 threads";
  EXPECT_EQ(cpusForQuota(200000, 100000), 2u);
  EXPECT_EQ(cpusForQuota(50000, 100000), 1u) << "half a CPU is still one thread";
  EXPECT_EQ(cpusForQuota(1, 0), 0u);
}

TEST(CpuQuota, V2TheTightestQuotaOnTheWayUpWins) {
  Tree t;
  t.files["/proc/self/cgroup"] = "0::/kubepods/pod1/c1\n";
  t.files["/proc/self/mountinfo"] = kV2Mount;
  t.files["/sys/fs/cgroup/kubepods/pod1/c1/cpu.max"] = "max 100000\n";
  t.files["/sys/fs/cgroup/kubepods/pod1/cpu.max"] = "250000 100000\n";
  t.files["/sys/fs/cgroup/kubepods/cpu.max"] = "800000 100000\n";
  EXPECT_EQ(cgroupCpuQuota(t.fn()), 3u);
  t.files["/sys/fs/cgroup/kubepods/pod1/c1/cpu.max"] = "100000 100000\n";
  EXPECT_EQ(cgroupCpuQuota(t.fn()), 1u);
}

TEST(CpuQuota, V2InsideItsOwnNamespace) {
  // A container with its own cgroup namespace sees its cgroup as the root.
  Tree t;
  t.files["/proc/self/cgroup"] = "0::/\n";
  t.files["/proc/self/mountinfo"] = kV2Mount;
  t.files["/sys/fs/cgroup/cpu.max"] = "200000 100000\n";
  EXPECT_EQ(cgroupCpuQuota(t.fn()), 2u);
  // Without mountinfo the usual mount point is assumed.
  t.files.erase("/proc/self/mountinfo");
  EXPECT_EQ(cgroupCpuQuota(t.fn()), 2u);
}

TEST(CpuQuota, V2MountedBelowItsRoot) {
  // The hierarchy mounted from the process's own cgroup: the path it is in
  // is the mount's root, and the files are at the mount point.
  Tree t;
  t.files["/proc/self/cgroup"] = "0::/docker/abc\n";
  t.files["/proc/self/mountinfo"] =
      "40 35 0:22 /docker/abc /sys/fs/cgroup ro,nosuid - cgroup2 cgroup rw\n";
  t.files["/sys/fs/cgroup/cpu.max"] = "400000 100000\n";
  t.files["/sys/fs/cgroup/docker/abc/cpu.max"] = "100000 100000\n"; // not where it is
  EXPECT_EQ(cgroupCpuQuota(t.fn()), 4u);
}

TEST(CpuQuota, V1TheCpuControllersHierarchy) {
  Tree t;
  t.files["/proc/self/cgroup"] =
      "7:cpuset:/docker/abc\n4:cpu,cpuacct:/docker/abc\n1:name=systemd:/x\n";
  t.files["/proc/self/mountinfo"] =
      "30 25 0:26 / /sys/fs/cgroup/cpuset rw - cgroup cgroup rw,cpuset\n"
      "31 25 0:27 /docker/abc /sys/fs/cgroup/cpu,cpuacct rw - cgroup cgroup rw,cpu,cpuacct\n";
  t.files["/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us"] = "150000\n";
  t.files["/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_period_us"] = "100000\n";
  t.files["/sys/fs/cgroup/cpuset/cpu.cfs_quota_us"] = "10000\n"; // cpuset is not cpu
  t.files["/sys/fs/cgroup/cpuset/cpu.cfs_period_us"] = "100000\n";
  EXPECT_EQ(cgroupCpuQuota(t.fn()), 2u);
  t.files["/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us"] = "-1\n"; // no quota
  EXPECT_EQ(cgroupCpuQuota(t.fn()), 0u);
}

TEST(CpuQuota, NoQuotaIsZero) {
  Tree t;
  EXPECT_EQ(cgroupCpuQuota(t.fn()), 0u) << "nothing readable";
  t.files["/proc/self/cgroup"] = "0::/user.slice/user-1000.slice/session-1.scope\n";
  t.files["/proc/self/mountinfo"] = kV2Mount;
  t.files["/sys/fs/cgroup/user.slice/cpu.max"] = "max 100000\n";
  EXPECT_EQ(cgroupCpuQuota(t.fn()), 0u);
  t.files["/proc/self/cgroup"] = "garbage\n";
  EXPECT_EQ(cgroupCpuQuota(t.fn()), 0u);
}

TEST(CpuQuota, TheThreadCountIsAtMostTheQuota) {
  // On this machine: whatever the quota says, the pass never takes more
  // threads than it allows, nor more than the affinity mask.
  const unsigned n = usableCpus();
  EXPECT_GE(n, 1u);
  const unsigned q = cgroupCpuQuota();
  if (q) {
    EXPECT_LE(n, q);
  }
}
