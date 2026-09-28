#include "CpuBench.h"

#include "CpuCounters.h"
#include "Publish.h"

#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#endif
#if defined(UCACHE_CPUBENCH_CODECS)
#include <lzma.h>
#include <zlib.h>
#endif

namespace ucache {
namespace {

// The rest of the workload's definition (see kCpuBenchVersion).
//
// The events: two muons each, three columns per muon, stored column after
// column as a TTree basket or an RNTuple page holds them. Every value is an
// integer drawn from a fixed-seed generator, scaled by one multiplication, so
// the input is the same bits on every machine, and quantized to twelve bits
// the way analysis formats truncate their floats -- which is what gives the
// codecs something to do.
constexpr size_t kEvents = size_t(1) << 15;
constexpr size_t kColumns = 6; // pt, eta, phi of each muon
constexpr size_t kValues = kEvents * kColumns;
constexpr uint64_t kSeed = 0x7563616368652d31ull;

uint64_t splitmix(uint64_t& s) {
  uint64_t z = (s += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

std::vector<float> makeEvents() {
  std::vector<float> v(kValues);
  uint64_t s = kSeed;
  const float kPhiStep = 0x1.921fb6p-10f; // pi / 2048
  for (size_t c = 0; c < kColumns; ++c)
    for (size_t i = 0; i < kEvents; ++i) {
      const int k = static_cast<int>(splitmix(s) >> 52); // 0..4095
      float x;
      if (c % 3 == 0) // pt: 5 to 261 GeV, falling
        x = static_cast<float>(80 + ((k * k) >> 12)) * 0.0625f;
      else if (c % 3 == 1) // eta: -2.5 to 2.5
        x = static_cast<float>(k - 2048) * 0.001220703125f;
      else // phi: -pi to pi
        x = static_cast<float>(k - 2048) * kPhiStep;
      v[c * kEvents + i] = x;
    }
  return v;
}

#if defined(UCACHE_CPUBENCH_CODECS)
constexpr size_t kBytes = kValues * sizeof(float);
// ZLIB at level 1, as analysis files commonly are. LZMA at preset 1 rather
// than the 9 heavily compressed files use: on this input the higher presets
// shrink the stream by 5% and decode no more slowly, while they take three
// times as long to encode -- and the encoding is the untimed part a publish
// waits for.
constexpr uint32_t kLzmaPreset = 1;
constexpr int kZlibLevel = 1;
// The mass spectrum: 1 GeV bins to 200 GeV, and one overflow bin.
constexpr size_t kMassBins = 200;

// The per-event loop: select two central muons above 10 GeV, build each
// one's four-vector from pt, eta, phi and the muon mass, and histogram the
// pair's invariant mass -- what ROOT::VecOps::InvariantMass does per event.
void massHistogram(const float* col, uint64_t* hist) {
  constexpr double kMuonMass = 0.1056583755; // GeV
  const float* pt1 = col;
  const float* eta1 = col + kEvents;
  const float* phi1 = col + 2 * kEvents;
  const float* pt2 = col + 3 * kEvents;
  const float* eta2 = col + 4 * kEvents;
  const float* phi2 = col + 5 * kEvents;
  for (size_t i = 0; i < kEvents; ++i) {
    if (pt1[i] < 10.0f || pt2[i] < 10.0f || std::fabs(eta1[i]) > 2.4f || std::fabs(eta2[i]) > 2.4f)
      continue;
    const double p1 = pt1[i], p2 = pt2[i];
    const double x1 = p1 * std::cos(static_cast<double>(phi1[i]));
    const double y1 = p1 * std::sin(static_cast<double>(phi1[i]));
    const double z1 = p1 * std::sinh(static_cast<double>(eta1[i]));
    const double x2 = p2 * std::cos(static_cast<double>(phi2[i]));
    const double y2 = p2 * std::sin(static_cast<double>(phi2[i]));
    const double z2 = p2 * std::sinh(static_cast<double>(eta2[i]));
    const double e1 = std::sqrt(x1 * x1 + y1 * y1 + z1 * z1 + kMuonMass * kMuonMass);
    const double e2 = std::sqrt(x2 * x2 + y2 * y2 + z2 * z2 + kMuonMass * kMuonMass);
    const double e = e1 + e2, x = x1 + x2, y = y1 + y2, z = z1 + z2;
    const double m2 = e * e - x * x - y * y - z * z;
    const double m = m2 > 0.0 ? std::sqrt(m2) : 0.0;
    ++hist[m < static_cast<double>(kMassBins) ? static_cast<size_t>(m) : kMassBins];
  }
}

uint64_t mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h;
}

// Every decoded bit, folded: a decoder that produced anything else changes it.
uint64_t bitsOf(const float* v) {
  uint64_t h = 0;
  for (size_t i = 0; i < kValues; i += 2) {
    uint64_t w;
    std::memcpy(&w, v + i, sizeof w);
    h = (h ^ w) * 0x100000001b3ull;
  }
  return h;
}

bool decodeLzma(const std::string& in, float* out) {
  uint64_t memlim = UINT64_MAX;
  size_t inPos = 0, outPos = 0;
  return lzma_stream_buffer_decode(&memlim, 0, nullptr, reinterpret_cast<const uint8_t*>(in.data()),
                                   &inPos, in.size(), reinterpret_cast<uint8_t*>(out), &outPos,
                                   kBytes) == LZMA_OK &&
         outPos == kBytes;
}

bool decodeZlib(const std::string& in, float* out) {
  uLongf len = kBytes;
  return uncompress(reinterpret_cast<Bytef*>(out), &len, reinterpret_cast<const Bytef*>(in.data()),
                    in.size()) == Z_OK &&
         len == kBytes;
}
#endif

double threadCpuS() {
  struct timespec ts;
  if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
    return 0.0;
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

double wallS() {
  struct timespec ts;
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

// Instructions retired so far, or 0 when there are no counters here.
uint64_t instructionsNow(const CpuCounters& c) {
  return c.available() ? c.sample().instructions : 0;
}

// Instructions retired between two readings, or 0 when there are no counters
// here. A difference, so it holds for counters that start at zero when opened
// (perf) and for ones that count the process from its start (rusage) alike.
// `source` is set to where they came from whenever there were counters.
uint64_t counted(const CpuCounters& c, uint64_t before, std::string& source) {
  if (!c.available())
    return 0;
  const CpuCounters::Sample s = c.sample();
  if (!s.source)
    return 0;
  source = s.source;
  return s.instructions > before ? s.instructions - before : 0;
}

// ---- cgroup CPU quota ------------------------------------------------------

std::vector<std::string> splitOn(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t at = 0;
  for (;;) {
    const size_t e = s.find(sep, at);
    out.push_back(s.substr(at, e == std::string::npos ? std::string::npos : e - at));
    if (e == std::string::npos)
      return out;
    at = e + 1;
  }
}

bool parseU64(const std::string& s, uint64_t& v) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
    ++i;
  if (i == s.size() || s[i] < '0' || s[i] > '9')
    return false;
  v = 0;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
    if (v > (UINT64_MAX - 9) / 10)
      return false;
    v = v * 10 + static_cast<uint64_t>(s[i] - '0');
  }
  for (; i < s.size(); ++i)
    if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n')
      return false;
  return true;
}

// A cgroup hierarchy as this process sees it: where it is mounted, which of
// its cgroups the mount's root is, and which the process is in.
struct Hierarchy {
  std::string point, root = "/", path;
};

// The mount of the cgroup v2 hierarchy (`v2`), or of the v1 hierarchy that
// carries the cpu controller, from /proc/self/mountinfo: "<id> <parent>
// <dev> <root> <point> <opts> [optional...] - <type> <source> <super opts>".
bool findMount(const std::string& mountinfo, bool v2, Hierarchy& h) {
  for (const std::string& line : splitOn(mountinfo, '\n')) {
    const std::vector<std::string> f = splitOn(line, ' ');
    size_t dash = 0;
    while (dash < f.size() && f[dash] != "-")
      ++dash;
    if (dash < 5 || dash + 3 >= f.size())
      continue;
    const std::string& type = f[dash + 1];
    bool match = v2 && type == "cgroup2";
    if (!v2 && type == "cgroup")
      for (const std::string& o : splitOn(f[dash + 3], ','))
        match = match || o == "cpu";
    if (match) {
      h.root = f[3];
      h.point = f[4];
      return true;
    }
  }
  return false;
}

// The tightest quota on the way from the process's cgroup up to the mount's
// root; 0 when none. `quotaIn(dir)` reads one cgroup's quota (0: none).
template <typename QuotaIn> unsigned walkUp(const Hierarchy& h, QuotaIn quotaIn) {
  if (h.point.empty() || h.point[0] != '/')
    return 0; // not a path: nothing to read
  // The process's cgroup, relative to the mount's root. A cgroup outside
  // what the mount shows (another namespace's) leaves only the mount's own.
  std::string rel;
  const std::string root = h.root == "/" ? "" : h.root;
  if (h.path.compare(0, root.size(), root) == 0 &&
      (h.path.size() == root.size() || h.path[root.size()] == '/'))
    rel = h.path.substr(root.size());
  std::string point = h.point;
  while (point.size() > 1 && point.back() == '/')
    point.pop_back();
  if (point.size() < 2)
    return 0; // a hierarchy mounted at / is not one this reads
  std::string dir = point;
  for (const std::string& c : splitOn(rel, '/'))
    if (!c.empty() && c != "." && c != "..")
      dir += "/" + c;
  unsigned best = 0;
  for (;;) {
    const unsigned q = quotaIn(dir);
    if (q && (!best || q < best))
      best = q;
    if (dir.size() <= point.size())
      return best;
    dir.resize(dir.rfind('/'));
  }
}

} // namespace

bool parseCpuMax(const std::string& text, uint64_t& quota, uint64_t& period) {
  std::string t = text;
  while (!t.empty() && (t.back() == '\n' || t.back() == ' '))
    t.pop_back();
  const size_t sp = t.find(' ');
  if (sp == std::string::npos)
    return false;
  uint64_t q = 0, p = 0;
  if (!parseU64(t.substr(0, sp), q) || !parseU64(t.substr(sp + 1), p) || !q || !p)
    return false; // "max <period>" is no quota; anything else is not one either
  quota = q;
  period = p;
  return true;
}

unsigned cpusForQuota(uint64_t quota, uint64_t period) {
  if (!period)
    return 0;
  const uint64_t n = quota / period + (quota % period ? 1 : 0);
  if (n > UINT32_MAX)
    return UINT32_MAX;
  return n ? static_cast<unsigned>(n) : 1u;
}

unsigned cgroupCpuQuota(const ReadTextFn& read) {
  std::string cgroups, mountinfo;
  if (!read("/proc/self/cgroup", cgroups))
    return 0;
  const bool haveMounts = read("/proc/self/mountinfo", mountinfo);
  // "<id>:<controllers>:<path>": id 0 with no controllers is the v2
  // hierarchy; a v1 line naming the cpu controller is the one a quota lives in.
  Hierarchy v2, v1;
  bool inV2 = false, inV1 = false;
  for (const std::string& line : splitOn(cgroups, '\n')) {
    const size_t a = line.find(':');
    const size_t b = a == std::string::npos ? a : line.find(':', a + 1);
    if (b == std::string::npos)
      continue;
    const std::string id = line.substr(0, a), ctl = line.substr(a + 1, b - a - 1);
    const std::string path = line.substr(b + 1);
    if (id == "0" && ctl.empty()) {
      v2.path = path;
      inV2 = true;
    } else {
      for (const std::string& c : splitOn(ctl, ','))
        if (c == "cpu") {
          v1.path = path;
          inV1 = true;
        }
    }
  }
  unsigned best = 0;
  auto take = [&best](unsigned q) {
    if (q && (!best || q < best))
      best = q;
  };
  if (inV2) {
    if (!haveMounts || !findMount(mountinfo, true, v2))
      v2.point = "/sys/fs/cgroup";
    take(walkUp(v2, [&read](const std::string& dir) {
      std::string t;
      uint64_t q = 0, p = 0;
      return read(dir + "/cpu.max", t) && parseCpuMax(t, q, p) ? cpusForQuota(q, p) : 0u;
    }));
  }
  if (inV1) {
    if (!haveMounts || !findMount(mountinfo, false, v1))
      v1.point = "/sys/fs/cgroup/cpu";
    take(walkUp(v1, [&read](const std::string& dir) {
      std::string qt, pt;
      uint64_t q = 0, p = 0;
      // A quota of -1 is none; it does not parse as a number, so it is none here.
      if (!read(dir + "/cpu.cfs_quota_us", qt) || !read(dir + "/cpu.cfs_period_us", pt) ||
          !parseU64(qt, q) || !parseU64(pt, p) || !q)
        return 0u;
      return cpusForQuota(q, p);
    }));
  }
  return best;
}

unsigned cgroupCpuQuota() {
#if defined(__linux__)
  return cgroupCpuQuota([](const std::string& path, std::string& text) {
    std::ifstream in(path);
    if (!in)
      return false;
    std::ostringstream os;
    os << in.rdbuf();
    text = os.str();
    return !in.bad();
  });
#else
  return 0;
#endif
}

unsigned usableCpus() {
  unsigned n = 0;
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  if (::sched_getaffinity(0, sizeof set, &set) == 0 && CPU_COUNT(&set) > 0)
    n = static_cast<unsigned>(CPU_COUNT(&set));
#endif
  if (!n) {
    const long c = ::sysconf(_SC_NPROCESSORS_ONLN);
    n = c > 0 ? static_cast<unsigned>(c) : 1u;
  }
  const unsigned q = cgroupCpuQuota();
  return q && q < n ? q : n;
}

CpuBenchInput prepareCpuBench() {
  CpuBenchInput in;
  in.events = makeEvents();
#if defined(UCACHE_CPUBENCH_CODECS)
  const auto* raw = reinterpret_cast<const uint8_t*>(in.events.data());
  std::string lz(lzma_stream_buffer_bound(kBytes), '\0');
  size_t lzLen = 0;
  if (lzma_easy_buffer_encode(kLzmaPreset, LZMA_CHECK_CRC32, nullptr, raw, kBytes,
                              reinterpret_cast<uint8_t*>(&lz[0]), &lzLen, lz.size()) != LZMA_OK)
    return in;
  lz.resize(lzLen);
  uLongf zLen = compressBound(kBytes);
  std::string z(zLen, '\0');
  if (compress2(reinterpret_cast<Bytef*>(&z[0]), &zLen, raw, kBytes, kZlibLevel) != Z_OK)
    return in;
  z.resize(zLen);
  // Both round trips checked once, here, so the timed work never has to
  // compare against the original: a codec that decodes to other bits would
  // time a different workload.
  std::vector<float> back(kValues);
  if (!decodeLzma(lz, back.data()) || std::memcmp(back.data(), in.events.data(), kBytes) != 0)
    return in;
  if (!decodeZlib(z, back.data()) || std::memcmp(back.data(), in.events.data(), kBytes) != 0)
    return in;
  in.lzma = std::move(lz);
  in.zlib = std::move(z);
  in.ok = true;
#endif
  return in;
}

uint64_t cpuBenchWork(const CpuBenchInput& in, unsigned reps) {
#if defined(UCACHE_CPUBENCH_CODECS)
  if (!in.ok)
    return 0;
  std::vector<float> out(kValues);
  uint64_t hist[kMassBins + 1] = {0};
  uint64_t h = 1469598103934665603ull;
  for (unsigned rep = 0; rep < reps; ++rep) {
    if (!decodeLzma(in.lzma, out.data()))
      return 0;
    h = mix(h, bitsOf(out.data()));
    massHistogram(out.data(), hist);
    if (!decodeZlib(in.zlib, out.data()))
      return 0;
    h = mix(h, bitsOf(out.data()));
    massHistogram(out.data(), hist);
  }
  for (uint64_t n : hist)
    h = mix(h, n);
  return h ? h : 1; // 0 is reserved for "the work could not be done"
#else
  (void)in;
  (void)reps;
  return 0;
#endif
}

CpuBenchResult runCpuBench(const CpuBenchOptions& opt) {
  CpuBenchResult r;
  const CpuBenchInput in = prepareCpuBench();
  if (!in.ok)
    return r;
  const unsigned reps = opt.reps ? opt.reps : kCpuBenchReps;
  bool haveCounts = true;
  std::string source;

  // Once, on this thread.
  {
    CpuCounters c;
    const uint64_t i0 = instructionsNow(c);
    const double w0 = wallS(), c0 = threadCpuS();
    r.checksum = cpuBenchWork(in, reps);
    r.t1CpuS = threadCpuS() - c0;
    r.t1WallS = wallS() - w0;
    r.t1Instructions = counted(c, i0, source);
    haveCounts = r.t1Instructions > 0;
  }
  if (!r.checksum)
    return r;

  // The same work on every CPU it may use at once, all released together.
  // The counters are opened before the threads start, which is what makes
  // them follow each thread (perf counts a thread started after the counter
  // opened).
  const unsigned n = opt.threads ? opt.threads : usableCpus();
  std::vector<uint64_t> sums(n, 0);
  std::vector<double> cpu(n, 0.0);
  {
    std::mutex mu;
    std::condition_variable cv;
    unsigned ready = 0;
    bool go = false;
    CpuCounters c;
    const uint64_t i0 = instructionsNow(c);
    std::vector<std::thread> threads;
    bool spawned = true;
    try {
      threads.reserve(n);
      for (unsigned t = 0; t < n; ++t)
        threads.emplace_back([&, t] {
          {
            std::unique_lock<std::mutex> l(mu);
            ++ready;
            cv.notify_all();
            cv.wait(l, [&] { return go; });
          }
          const double c0 = threadCpuS();
          sums[t] = cpuBenchWork(in, reps);
          cpu[t] = threadCpuS() - c0;
        });
    } catch (...) {
      spawned = false; // release and join the ones that did start, then give up
    }
    double w0;
    {
      std::unique_lock<std::mutex> l(mu);
      if (spawned)
        cv.wait(l, [&] { return ready == n; });
      go = true;
      w0 = wallS();
    }
    cv.notify_all();
    for (auto& th : threads)
      th.join();
    if (!spawned)
      return r; // no calibration rather than one taken on fewer CPUs than it says
    r.tnWallS = wallS() - w0;
    r.tnInstructions = counted(c, i0, source);
    haveCounts = haveCounts && r.tnInstructions > 0;
  }
  for (unsigned t = 0; t < n; ++t) {
    if (sums[t] != r.checksum)
      return r; // a thread did different work: nothing here is a measurement
    r.tnCpuS += cpu[t];
  }
  r.tnThreads = n;
  if (haveCounts && !source.empty()) {
    r.counterSource = source;
  } else {
    r.t1Instructions = r.tnInstructions = 0;
  }
  r.ok = true;
  return r;
}

Json cpuBenchJson(const CpuBenchResult& r) {
  Json j = Json::object();
  j.set("version", Json::integer(kCpuBenchVersion));
  j.set("t1_cpu_s", Json::real(r.t1CpuS, 4));
  j.set("t1_wall_s", Json::real(r.t1WallS, 4));
  j.set("tn_threads", Json::integer(r.tnThreads));
  j.set("tn_cpu_s", Json::real(r.tnCpuS, 4));
  j.set("tn_wall_s", Json::real(r.tnWallS, 4));
  if (!r.counterSource.empty()) {
    j.set("counter_source", Json::string(r.counterSource));
    j.set("t1_instructions", Json::numberText(std::to_string(r.t1Instructions)));
    j.set("tn_instructions", Json::numberText(std::to_string(r.tnInstructions)));
  }
  return j;
}

} // namespace ucache
