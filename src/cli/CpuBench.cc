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


} // namespace

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
