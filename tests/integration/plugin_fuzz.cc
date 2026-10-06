// Plugin-level differential fuzz: random Read/VectorRead/PgRead
// mixes through an XrdCl::File (plugin enabled via XRD_PLUGINCONFDIR in the
// environment) against an origin-served file, byte-compared on every
// operation against the same file read directly from disk.
//
//   plugin_fuzz <root-url> <local-path> [ops=100000] [threads=1] [seed=random] [mode=fuzz]
//
// exit 0 = clean; 1 = mismatch/error (seed printed). Multi-thread mode
// shares one File across threads (the IMT pattern) — run under TSan for the
// §13 TSan requirement. Single-thread mode also cycles the handle (~0.5% of
// ops close + reopen a FRESH File), fuzzing the whole plugin open/setup/
// drain/close lifecycle, not just steady-state reads.
//
// Two more modes race the calls that use a handle's origin File without
// waiting for anything against the retries of an origin open, which replace
// that File (see runInnerRace for what they need and check):
//   inner-race         retries of the cache's own open (a trusted handle's
//                      first miss); `ops` = handle cycles, `threads` = racers
//   inner-race-relay   retries of the reader's own open (an untrusted handle),
//                      raced from the moment Open() returns until it completes
#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClURL.hh>
#ifdef UCACHE_FUZZ_HAVE_TRANSPOSE
#include "TreeMeta.h"
#endif

#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::atomic<uint64_t> gFailures{0};

// XrdAdaptor-strict open handler: native XrdCl always delivers a NON-NULL
// HostList to HandleResponseWithHosts, and real consumers (CMSSW's
// tracerouteRedirections) iterate it unconditionally — a null list from a
// synthesized cache-served completion SEGVs cmsRun on warm opens. Every
// fuzz open goes through this handler so the contract is enforced on both
// cold (relayed) and warm (locally completed) paths.
struct StrictOpenHandler : XrdCl::ResponseHandler {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;     // of the first completion
  int completions = 0; // the contract: exactly one
  void HandleResponseWithHosts(XrdCl::XRootDStatus* status, XrdCl::AnyObject* response,
                               XrdCl::HostList* hostList) override {
    bool good = status && status->IsOK();
    if (hostList == nullptr) {
      std::fprintf(stderr, "OPEN CONTRACT VIOLATION: null HostList in completion\n");
      good = false;
    } else {
      for (const auto& h : *hostList) // dereference exactly like XrdAdaptor
        (void)h;
    }
    delete status;
    delete response;
    delete hostList;
    // Notify UNDER the lock: the waiter destroys this stack-allocated handler
    // as soon as it observes done, so the signal must complete before the
    // mutex is released (condvar-destruction race, TSan-caught).
    {
      std::lock_guard<std::mutex> g(mu);
      if (++completions == 1)
        ok = good;
      done = true;
      cv.notify_all();
    }
  }
  bool wait() {
    std::unique_lock<std::mutex> l(mu);
    cv.wait(l, [&] { return done; });
    return ok;
  }
  // The first completion's verdict, or false if none came within `max`.
  bool succeededWithin(std::chrono::seconds max) {
    std::unique_lock<std::mutex> l(mu);
    return cv.wait_for(l, max, [&] { return done; }) && ok;
  }
  int count() {
    std::lock_guard<std::mutex> g(mu);
    return completions;
  }
};

// Async open through the strict handler; returns false on any failure --
// including a property-contract violation. After a successful open, cold or
// cache-served, every property a client routes on must be a STRING: LastURL
// is what ROOT and uproot's own source branch on; DataServer is what
// fsspec-xrootd (uproot 5's default transport) asks for, and in Python an
// unset property arrives as None. A cache-served open used to decline
// DataServer, and uproot died in client.URL(None) on the second open of every
// cached file.
bool strictOpen(XrdCl::File& file, const char* url) {
  StrictOpenHandler oh;
  auto st = file.Open(url, XrdCl::OpenFlags::Read, XrdCl::Access::None, &oh);
  if (!st.IsOK())
    return false;
  if (!oh.wait())
    return false;
  std::string v;
  if (!file.GetProperty("LastURL", v) || v.empty()) {
    std::fprintf(stderr, "LastURL unset after open\n");
    return false;
  }
  if (!file.GetProperty("DataServer", v)) {
    std::fprintf(stderr, "DataServer unset after open (a Python client sees None)\n");
    return false;
  }
  return true;
}

struct Source {
  std::vector<char> bytes;
  explicit Source(const char* path) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
      std::perror("open source");
      std::exit(2);
    }
    off_t size = ::lseek(fd, 0, SEEK_END);
    bytes.resize(size);
    // Loop: a single pread caps at ~2 GiB (MAX_RW_COUNT); ground truths in a
    // layout of the cache's (a file written out in it) can exceed it.
    off_t done = 0;
    while (done < size) {
      ssize_t r = ::pread(fd, bytes.data() + done, size - done, done);
      if (r <= 0) {
        std::fprintf(stderr, "short source read\n");
        std::exit(2);
      }
      done += r;
    }
    ::close(fd);
  }
};

struct Ctx {
  XrdCl::File* file;
  const char* url;
  bool reopen; // single-thread only: cycle the handle through close/open
};

// A TTree reader's fill stream, from the seed's own basket map: a dozen
// branches, the next eight baskets of each per fill, in entry order. Random
// reads never look like this, and read-ahead only engages on it -- without it
// the plugin's read-ahead path would sit under the sanitizers unexercised.
struct Walk {
  std::vector<std::vector<std::pair<uint64_t, uint32_t>>> branches; // (seek, bytes) per basket
  // What a reader fetches before its first fill: header, keys list, the tree
  // record. The plugin's read-ahead parses the map out of exactly these bytes.
  std::vector<std::pair<uint64_t, uint32_t>> meta;
};
#ifdef UCACHE_FUZZ_HAVE_TRANSPOSE
std::unique_ptr<Walk> buildWalk(const char* path) {
  ucache::transpose::FileMeta fm = ucache::transpose::parseFile(path);
  if (!fm.error.empty() || fm.branches.size() < 12)
    return nullptr;
  auto w = std::make_unique<Walk>();
  w->meta.emplace_back(0, 4096);
  {
    int fd = ::open(path, O_RDONLY);
    if (fd >= 0) {
      std::vector<uint8_t> probe(512);
      if (::pread(fd, probe.data(), probe.size(), fm.keyslistSeek) > 34)
        if (auto k = ucache::transpose::parseKey(probe.data(), probe.size(), 0))
          w->meta.emplace_back(static_cast<uint64_t>(fm.keyslistSeek), static_cast<uint32_t>(k->nbytes));
      ::close(fd);
    }
  }
  w->meta.emplace_back(static_cast<uint64_t>(fm.treeKey.seekkey), static_cast<uint32_t>(fm.treeKey.nbytes));
  const size_t step = fm.branches.size() / 12;
  for (size_t b = 0; b < fm.branches.size() && w->branches.size() < 12; b += step) {
    const auto& br = fm.branches[b];
    if (br.basketSeek.size() < 16)
      continue;
    std::vector<std::pair<uint64_t, uint32_t>> bk;
    for (size_t i = 0; i < br.basketSeek.size(); ++i)
      bk.emplace_back(static_cast<uint64_t>(br.basketSeek[i]), static_cast<uint32_t>(br.basketBytes[i]));
    w->branches.push_back(std::move(bk));
  }
  return w->branches.empty() ? nullptr : std::move(w);
}
#else
std::unique_ptr<Walk> buildWalk(const char*) { return nullptr; }
#endif

void worker(Ctx* ctx, const Source& src, const Walk* walk, uint64_t seed, uint64_t ops, int tid) {
  std::mt19937_64 rng(seed + tid);
  const uint64_t fsize = src.bytes.size();
  std::vector<char> buf(4 * 1024 * 1024);
  std::vector<std::vector<char>> vbufs(16);
  std::vector<size_t> cursor(walk ? walk->branches.size() : 0, 0);

  for (uint64_t i = 0; i < ops; ++i) {
    XrdCl::File* file = ctx->file;
    if (ctx->reopen && rng() % 200 == 0) {
      // Fresh File = fresh plugin instance: exercises Close (drainPersists),
      // destruction, and the whole lazy-setup path again on a warm cache.
      auto cs = file->Close();
      (void)cs; // reads already verified; a close error alone is not a defect
      delete file;
      ctx->file = file = new XrdCl::File();
      if (!strictOpen(*file, ctx->url)) { // warm reopen: the cache-served path
        std::fprintf(stderr, "REOPEN FAILED op=%llu\n", (unsigned long long)i);
        ++gFailures;
        return;
      }
    }
    if (rng() % 100 < 55) { // single read
      // Mostly small, but one in sixteen is large enough to cross the readv
      // element ceiling. That size class rounds its fetched span differently,
      // so the served bytes come out of the wire buffer at a shifted offset --
      // the arithmetic this exercises is not reachable with 256 KiB reads.
      uint32_t len = (rng() % 16 == 0) ? 2 * 1024 * 1024 + rng() % (2 * 1024 * 1024)
                                       : 1 + rng() % (256 * 1024);
      if (len > fsize)
        len = fsize;
      // One in 32 reads deliberately runs PAST EOF, and must come back SHORT.
      // Every read here used to be placed in bounds by construction, so the
      // last-block overrun that any fixed-block reader performs was never
      // exercised — and an entry the cache lays out answering it with zero bytes went
      // unnoticed until a real client aborted on it.
      const bool overrun = (rng() % 32 == 0) && len > 1;
      const uint64_t off = overrun ? fsize - len / 2 : rng() % (fsize - len + 1);
      const uint32_t want =
          static_cast<uint32_t>(fsize - off < len ? fsize - off : len);
      uint32_t got = 0;
      auto st = file->Read(off, len, buf.data(), got);
      if (!st.IsOK() || got != want ||
          std::memcmp(buf.data(), src.bytes.data() + off, want) != 0) {
        std::fprintf(stderr,
                     "READ MISMATCH tid=%d op=%llu off=%llu len=%u want=%u got=%u ok=%d\n",
                     tid, (unsigned long long)i, (unsigned long long)off, len, want, got,
                     st.IsOK());
        ++gFailures;
        return;
      }
    } else if (walk && tid == 0 && rng() % 100 < 50) { // a TTree reader's fill (one coherent reader; the other threads race it)
      XrdCl::ChunkList chunks;
      size_t n = 0;
      for (size_t b = 0; b < walk->branches.size(); ++b) {
        const auto& bk = walk->branches[b];
        for (int k = 0; k < 8 && cursor[b] < bk.size(); ++k, ++cursor[b]) {
          const auto [off, len] = bk[cursor[b]];
          if (vbufs.size() <= n)
            vbufs.resize(n + 1);
          vbufs[n].resize(len);
          chunks.emplace_back(off, len, vbufs[n].data());
          ++n;
        }
      }
      if (chunks.empty()) { // the reader reached the end: start the file over
        std::fill(cursor.begin(), cursor.end(), 0);
        continue;
      }
      if (cursor[0] <= 8) { // a pass begins: fetch the metadata a real reader fetches first
        for (const auto& [moff, mlen] : walk->meta) {
          const uint32_t want = static_cast<uint32_t>(fsize - moff < mlen ? fsize - moff : mlen);
          uint32_t got = 0;
          auto ms = file->Read(moff, mlen, buf.data(), got);
          if (!ms.IsOK() || got != want ||
              std::memcmp(buf.data(), src.bytes.data() + moff, want) != 0) {
            std::fprintf(stderr, "META READ MISMATCH tid=%d op=%llu off=%llu\n", tid,
                         (unsigned long long)i, (unsigned long long)moff);
            ++gFailures;
            return;
          }
        }
        // A reader decodes what it fetched before its first fill (ROOT: ~30 ms
        // on the AGC files); the fetched pages are staged asynchronously
        // behind the plugin's other work, and a fill in the same microsecond
        // is a pattern no reader has.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
      }
      std::sort(chunks.begin(), chunks.end(),
                [](const XrdCl::ChunkInfo& a, const XrdCl::ChunkInfo& b) { return a.offset < b.offset; });
      XrdCl::VectorReadInfo* info = nullptr;
      auto st = file->VectorRead(chunks, nullptr, info);
      bool ok = st.IsOK() && info;
      if (ok)
        for (const auto& c : chunks)
          if (std::memcmp(c.buffer, src.bytes.data() + c.offset, c.length) != 0)
            ok = false;
      delete info;
      if (!ok) {
        std::fprintf(stderr, "FILL MISMATCH tid=%d op=%llu chunks=%zu ok=%d\n", tid,
                     (unsigned long long)i, chunks.size(), st.IsOK());
        ++gFailures;
        return;
      }
    } else if (rng() % 100 < 67 || fsize < 4096) { // vector read
      int n = 1 + rng() % 12;
      XrdCl::ChunkList chunks;
      for (int c = 0; c < n; ++c) {
        // Now and then a zero-length chunk: ROOT sends them, and one must be
        // served empty, never make the plugin relay a vector in a cache's layout (whose
        // other chunks are offsets the origin does not have).
        uint32_t len = rng() % 16 == 0 ? 0 : 1 + rng() % (64 * 1024);
        if (len > fsize)
          len = fsize;
        uint64_t off = rng() % (fsize - len + 1);
        vbufs[c].resize(std::max<uint32_t>(len, 1)); // a real buffer, as ROOT's
        chunks.emplace_back(off, len, vbufs[c].data());
      }
      XrdCl::VectorReadInfo* info = nullptr;
      auto st = file->VectorRead(chunks, nullptr, info);
      bool ok = st.IsOK() && info;
      if (ok)
        for (int c = 0; c < n; ++c)
          if (std::memcmp(vbufs[c].data(), src.bytes.data() + chunks[c].offset,
                          chunks[c].length) != 0)
            ok = false;
      delete info;
      if (!ok) {
        std::fprintf(stderr, "VREAD MISMATCH tid=%d op=%llu n=%d ok=%d\n", tid,
                     (unsigned long long)i, n, st.IsOK());
        ++gFailures;
        return;
      }
    } else { // pgread (>= 1 page by API contract; slot-layout handles serve locally)
      uint32_t len = 4096 + rng() % (256 * 1024);
      if (len > fsize)
        len = static_cast<uint32_t>(fsize);
      uint64_t off = (rng() % (fsize - len + 1)) & ~4095ull; // page-aligned
      std::vector<uint32_t> cksums;
      uint32_t got = 0;
      auto st = file->PgRead(off, len, buf.data(), cksums, got);
      if (!st.IsOK() || got != len ||
          std::memcmp(buf.data(), src.bytes.data() + off, len) != 0) {
        std::fprintf(stderr, "PGREAD MISMATCH tid=%d op=%llu off=%llu len=%u got=%u ok=%d\n",
                     tid, (unsigned long long)i, (unsigned long long)off, len, got,
                     st.IsOK());
        ++gFailures;
        return;
      }
    }
  }
}

// A File destroyed under a call that is still in it mostly shows as a call
// that never returns (the thread waits on a mutex in freed memory), so the
// race modes run a watchdog: no progress for kStuckS seconds ends the run
// with exit 3 and a line naming the cycle.
constexpr int kStuckS = 120;
constexpr uint64_t kInSetup = ~uint64_t(0);
struct Watchdog {
  // Leaked: the watchdog thread may still be reading them when main returns.
  std::atomic<uint64_t>* progress = new std::atomic<uint64_t>{0};
  std::atomic<uint64_t>* where = new std::atomic<uint64_t>{kInSetup}; // the cycle
  explicit Watchdog(const char* mode) {
    auto* p = progress;
    auto* w = where;
    std::thread([p, w, mode] {
      uint64_t last = p->load(), still = 0;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const uint64_t now = p->load();
        if (now != last) {
          last = now;
          still = 0;
        } else if (++still >= kStuckS) {
          const uint64_t at = w->load();
          const std::string place = at == kInSetup ? "setup" : "cycle " + std::to_string(at);
          std::printf("%s: FAIL: stuck: %s made no progress for %d s (step %llu): "
                      "a call into the handle never returned\n",
                      mode, place.c_str(), kStuckS, (unsigned long long)now);
          std::fflush(stdout);
          std::_Exit(3);
        }
      }
    }).detach();
  }
  void step() { ++*progress; }
};

// `n` threads call GetProperty("DataServer"), IsOpen() and Stat(false) on `f`
// in a loop -- calls that use the handle's origin File without waiting for
// anything -- from the moment the constructor returns (every racer running)
// until stop(). `inFlight`, when given, says whether the open raced against is
// still under way; the rounds begun while it said so are counted apart.
class Racers {
 public:
  Racers(XrdCl::File* f, int n, std::function<bool()> inFlight = nullptr)
      : f_(f), inFlight_(std::move(inFlight)) {
    for (int t = 0; t < n; ++t)
      ts_.emplace_back([this] { run(); });
    std::unique_lock<std::mutex> l(mu_);
    cv_.wait(l, [&] { return running_ == static_cast<int>(ts_.size()); });
  }
  // Joins them; the rounds they made, and of those the ones begun in flight.
  std::pair<uint64_t, uint64_t> stop() {
    stop_.store(true);
    for (auto& t : ts_)
      t.join();
    return {made_.load(), madeInFlight_.load()};
  }

 private:
  void run() {
    {
      std::lock_guard<std::mutex> g(mu_);
      ++running_;
      cv_.notify_all();
    }
    std::string v;
    uint64_t k = 0, kf = 0;
    do {
      const bool fl = inFlight_ && inFlight_();
      f_->GetProperty("DataServer", v);
      (void)f_->IsOpen();
      XrdCl::StatInfo* si = nullptr;
      const XrdCl::XRootDStatus ss = f_->Stat(false, si);
      (void)ss;
      delete si;
      ++k;
      kf += fl ? 1 : 0;
    } while (!stop_.load());
    made_ += k;
    madeInFlight_ += kf;
  }
  XrdCl::File* f_;
  std::function<bool()> inFlight_;
  std::vector<std::thread> ts_;
  std::mutex mu_;
  std::condition_variable cv_;
  int running_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> made_{0}, madeInFlight_{0};
};

int envInt(const char* name, int dflt) {
  const char* v = ::getenv(name);
  return v ? ::atoi(v) : dflt;
}

// Modes inner-race and inner-race-relay: `racers` threads (Racers) call into
// a handle while retries of an origin open replace its origin File. Run it
// under a sanitizer: a File destroyed under a call still in it is a use after
// free, or a hang the watchdog stops. Needs UCACHE_TEST_OPEN_FAIL_N=N and
// UCACHE_OPEN_RETRIES>=N (each open's first N attempts fail, and each failure
// puts in a fresh File), a cache directory without this file, and
// <local-path> the file the URL names.
//  - inner-race, the cache's own open, with the default trust window: a
//    relayed open reads the first 64 KiB, so the file's entry exists and is
//    trusted; then `cycles` times: open (answered from the entry, without the
//    origin), start the racers, read 64 KiB the entry does not hold -- the miss
//    opens the origin, through its retries -- and compare it with the local
//    file, stop the racers, close.
//  - inner-race-relay, the reader's own open: UCACHE_REVALIDATE_S=0, so that
//    no open is answered from the entry. `cycles` times: open, run the racers
//    until the open's completion arrives, read 64 KiB and compare it, close.
// Each cycle checks that its racers ran, that the open was answered the way
// the mode needs, and after its read that the handle names the origin's
// server (its port, never port 1, where the test fault sends the failed
// attempts). Every open must get exactly one completion.
int runInnerRace(const char* url, const char* local, uint64_t cycles, int racers, bool relay) {
  const char* mode = relay ? "inner-race-relay" : "inner-race";
  const int failN = envInt("UCACHE_TEST_OPEN_FAIL_N", 0);
  if (failN < 1 || envInt("UCACHE_OPEN_RETRIES", 0) < failN) {
    std::printf("%s: SETUP: needs UCACHE_TEST_OPEN_FAIL_N=N and UCACHE_OPEN_RETRIES>=N\n", mode);
    return 2;
  }
  if (relay && envInt("UCACHE_REVALIDATE_S", -1) != 0) {
    std::printf("%s: SETUP: needs UCACHE_REVALIDATE_S=0 (no open answered from the entry)\n",
                mode);
    return 2;
  }
  const int fd = ::open(local, O_RDONLY);
  if (fd < 0) {
    std::printf("%s: SETUP: cannot open the local file\n", mode);
    return 2;
  }
  const uint64_t size = static_cast<uint64_t>(::lseek(fd, 0, SEEK_END));
  const uint32_t len = 65536;
  if (cycles == 0 || racers < 1 || size < (cycles + 2) * 2ull * len) {
    std::printf("%s: SETUP: the file is too small for %llu cycles\n", mode,
                (unsigned long long)cycles);
    return 2;
  }
  const std::string originPort = ":" + std::to_string(XrdCl::URL(url).GetPort());
  // Leaked, with their Files' completions: a late second completion is then
  // counted rather than landing in freed memory.
  std::vector<StrictOpenHandler*> handlers;
  auto startOpen = [&](XrdCl::File*& f, StrictOpenHandler*& oh) {
    f = new XrdCl::File();
    oh = new StrictOpenHandler();
    handlers.push_back(oh);
    return f->Open(url, XrdCl::OpenFlags::Read, XrdCl::Access::None, oh).IsOK();
  };
  auto readCheck = [&](XrdCl::File* f, uint64_t off) {
    std::vector<char> got(len), ref(len);
    uint32_t n = 0;
    const XrdCl::XRootDStatus st = f->Read(off, len, got.data(), n);
    return st.IsOK() && n == len &&
           ::pread(fd, ref.data(), len, static_cast<off_t>(off)) == static_cast<ssize_t>(len) &&
           std::memcmp(got.data(), ref.data(), len) == 0;
  };
  auto closeOne = [](XrdCl::File*& f) {
    const XrdCl::XRootDStatus cs = f->Close();
    (void)cs; // the reads were checked; a close error alone is not a defect here
    delete f;
    f = nullptr;
  };
  // Longer than the watchdog's limit, so an open whose completion never comes
  // always ends as "stuck", never as a failed open.
  const std::chrono::seconds openMax(2 * kStuckS);
  Watchdog dog(mode);

  XrdCl::File* f = nullptr;
  StrictOpenHandler* oh = nullptr;
  if (!relay) {
    if (!startOpen(f, oh) || !oh->succeededWithin(openMax) || !readCheck(f, 0)) {
      std::printf("%s: FAIL: the relayed open and first read of the setup\n", mode);
      return 1;
    }
    closeOne(f);
    dog.step();
  }

  const uint64_t stride = ((size - len) / (cycles + 1)) & ~uint64_t(0xFFFF);
  uint64_t calls = 0, callsInFlight = 0;
  for (uint64_t c = 0; c < cycles; ++c) {
    dog.where->store(c);
    const auto fail = [&](const std::string& what) {
      std::printf("%s: FAIL: cycle %llu: %s\n", mode, (unsigned long long)c, what.c_str());
      return 1;
    };
    if (!startOpen(f, oh))
      return fail("Open() returned an error");
    dog.step();
    const uint64_t off = stride * (c + 1);
    bool readOk = false, fromEntry = false;
    std::pair<uint64_t, uint64_t> made;
    // Asked once the open has its answer, before any read: an open answered
    // from the entry names a local file until a miss opens the origin.
    auto fromTheEntry = [&] {
      std::string lastUrl;
      return f->GetProperty("LastURL", lastUrl) && lastUrl.rfind("file://", 0) == 0;
    };
    if (relay) {
      StrictOpenHandler* h = oh;
      Racers r(f, racers, [h] { return h->count() == 0; });
      const bool opened = oh->succeededWithin(openMax);
      dog.step();
      made = r.stop();
      dog.step();
      if (!opened)
        return fail("the open");
      fromEntry = fromTheEntry();
      readOk = readCheck(f, off);
    } else {
      if (!oh->succeededWithin(openMax))
        return fail("the open");
      fromEntry = fromTheEntry();
      Racers r(f, racers);
      readOk = readCheck(f, off); // the miss: the cache opens the origin, with its retries
      dog.step();
      made = r.stop();
      dog.step();
    }
    std::string server;
    f->GetProperty("DataServer", server);
    calls += made.first;
    callsInFlight += made.second;
    closeOne(f);
    dog.step();
    if (!readOk)
      return fail("the read at " + std::to_string(off) + " failed or read other bytes");
    if (made.first == 0)
      return fail("the racers made no call");
    if (server.size() < originPort.size() ||
        server.compare(server.size() - originPort.size(), originPort.size(), originPort) != 0)
      return fail("after the read the handle names \"" + server +
                  "\" as its server, not the origin: the read did not reach it through the "
                  "retried open");
    // The cache's own open needs one answered from the entry; the reader's
    // open needs one that was not.
    if (fromEntry == relay)
      return fail(std::string("the open was ") + (fromEntry ? "" : "not ") +
                  "answered from the entry, so the open under test was not the one raced");
  }
  ::close(fd);
  std::this_thread::sleep_for(std::chrono::seconds(2)); // room for a late second completion
  int bad = 0;
  for (auto* h : handlers)
    bad += h->count() == 1 ? 0 : 1;
  std::printf("%s: %llu cycles, %llu concurrent calls (%llu while the open was under way), "
              "%d open(s) without exactly one completion\n",
              mode, (unsigned long long)cycles, (unsigned long long)calls,
              (unsigned long long)callsInFlight, bad);
  if (relay && callsInFlight == 0) {
    std::printf("%s: FAIL: no call was made while an open was under way\n", mode);
    return 1;
  }
  if (bad) {
    std::printf("%s: FAIL: the open completion contract\n", mode);
    return 1;
  }
  std::printf("%s: clean\n", mode);
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <root-url> <local-path> [ops] [threads] [seed] "
                 "[fuzz|inner-race|inner-race-relay]\n",
                 argv[0]);
    return 2;
  }
  const char* url = argv[1];
  const std::string mode = argc > 6 ? argv[6] : "fuzz";
  if (mode == "inner-race" || mode == "inner-race-relay")
    return runInnerRace(url, argv[2], argc > 3 ? ::strtoull(argv[3], nullptr, 10) : 200,
                        argc > 4 ? ::atoi(argv[4]) : 4, mode == "inner-race-relay");
  if (mode != "fuzz") {
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
  }
  Source src(argv[2]);
  std::unique_ptr<Walk> walk = buildWalk(argv[2]);
  if (walk)
    std::fprintf(stderr, "plugin-fuzz: TTree fill stream over %zu branches\n", walk->branches.size());
  uint64_t ops = argc > 3 ? ::strtoull(argv[3], nullptr, 10) : 100000;
  int threads = argc > 4 ? ::atoi(argv[4]) : 1;
  uint64_t seed = argc > 5 ? ::strtoull(argv[5], nullptr, 10) : std::random_device{}();
  std::printf("plugin_fuzz url=%s size=%zu ops=%llu threads=%d seed=%llu\n", url,
              src.bytes.size(), (unsigned long long)ops, threads,
              (unsigned long long)seed);

  Ctx ctx{new XrdCl::File(), url, threads == 1}; // plugins enabled — the point
  if (!strictOpen(*ctx.file, url)) { // warm second invocations hit the cache-served path
    std::fprintf(stderr, "open failed (or completion contract violated)\n");
    return 2;
  }

  std::vector<std::thread> ts;
  uint64_t opsPer = ops / threads;
  for (int t = 0; t < threads; ++t)
    ts.emplace_back(worker, &ctx, std::cref(src), walk.get(), seed, opsPer, t);
  for (auto& t : ts)
    t.join();

  auto cs = ctx.file->Close();
  delete ctx.file;
  if (!cs.IsOK())
    std::fprintf(stderr, "close: %s\n", cs.ToString().c_str());
  if (gFailures) {
    std::fprintf(stderr, "FAIL seed=%llu failures=%llu\n", (unsigned long long)seed,
                 (unsigned long long)gFailures.load());
    return 1;
  }
  std::printf("plugin_fuzz: clean (%llu ops x %d threads)\n", (unsigned long long)opsPer,
              threads);
  return 0;
}
