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
//
// open-refused checks that an open the client refuses at once still reaches
// the reader through its handler (see runOpenRefused).
//
// Two more cover an open made again, with adaptive_federation on:
//   open-property      a property set while the open is retried reaches the
//                      File that replaces the first (see runOpenProperty)
//   open-failover [N]  N opens through a redirector where a server refuses
//                      the file: exactly one completion each (see runOpenFailover)
//
// And one checks check_origin_integrity's key-position check of byte-cache
// misses against a copy with one basket zeroed (see runKeyCheck):
//   key-check <off> <len>   <root-url> serves that copy, <local-path> is the
//                      undamaged file; ops, threads and seed are ignored
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
#include <tuple>
#include <unistd.h>
#include <vector>

namespace {

std::atomic<uint64_t> gFailures{0};

// XrdAdaptor-strict open handler: native XrdCl always delivers a NON-NULL
// HostList to HandleResponseWithHosts, and real consumers (CMSSW's
// tracerouteRedirections) iterate it unconditionally — a null list from a
// synthesized cache-served completion SEGVs cmsRun on warm opens. Every
// fuzz open goes through this handler so the contract is enforced on both
// cold (relayed) and warm (locally completed) paths. It also counts the
// completions, of which there must be exactly one, and flags one that arrives
// through plain HandleResponse: a client that reads the HostList never sees it.
struct StrictOpenHandler : XrdCl::ResponseHandler {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;     // of the first completion
  int completions = 0; // the contract: exactly one
  bool viaPlain = false;
  bool openRefused = false; // Open() returned an error: no completion may come
  std::string answer;       // the first completion's status
  void HandleResponse(XrdCl::XRootDStatus* status, XrdCl::AnyObject* response) override {
    std::fprintf(stderr, "OPEN CONTRACT VIOLATION: completion through HandleResponse\n");
    {
      std::lock_guard<std::mutex> g(mu);
      viaPlain = true;
    }
    HandleResponseWithHosts(status, response, nullptr);
  }
  void HandleResponseWithHosts(XrdCl::XRootDStatus* status, XrdCl::AnyObject* response,
                               XrdCl::HostList* hostList) override {
    bool good = status && status->IsOK();
    const std::string said = status ? status->ToString() : "(no status)";
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
    // Handlers live for the process, so a late completion is counted rather
    // than written into freed memory. Notifying under the lock keeps done, the
    // count and the first completion's fields consistent for the waiter.
    {
      std::lock_guard<std::mutex> g(mu);
      if (++completions == 1) {
        ok = good;
        answer = said;
      }
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
  bool exactlyOne() {
    std::lock_guard<std::mutex> g(mu);
    return completions == (openRefused ? 0 : 1) && !viaPlain;
  }
};

// Every handler strictOpen used, kept for the process's life, so a second
// completion is counted rather than landing in freed memory; the run checks
// them all at its end.
std::mutex gOpenHandlersMu;
std::vector<StrictOpenHandler*>* gOpenHandlers = new std::vector<StrictOpenHandler*>();
int openHandlersBad() {
  std::lock_guard<std::mutex> g(gOpenHandlersMu);
  int bad = 0;
  for (auto* h : *gOpenHandlers)
    bad += h->exactlyOne() ? 0 : 1;
  return bad;
}

// Async open through the strict handler; returns false on any failure --
// including a property-contract violation. After a successful open, cold or
// cache-served, every property a client routes on must be a STRING: LastURL
// is what ROOT and uproot's own source branch on; DataServer is what
// fsspec-xrootd (uproot 5's default transport) asks for, and in Python an
// unset property arrives as None. A cache-served open used to decline
// DataServer, and uproot died in client.URL(None) on the second open of every
// cached file.
bool strictOpen(XrdCl::File& file, const char* url) {
  auto* oh = new StrictOpenHandler();
  {
    std::lock_guard<std::mutex> g(gOpenHandlersMu);
    gOpenHandlers->push_back(oh);
  }
  // What Open() returned, and what the handler was answered: after an error
  // from Open() no completion may come, so the handler is waited for only
  // after an OK.
  const XrdCl::XRootDStatus st = file.Open(url, XrdCl::OpenFlags::Read, XrdCl::Access::None, oh);
  if (!st.IsOK()) {
    std::fprintf(stderr, "open: Open() returned %s\n", st.ToString().c_str());
    std::lock_guard<std::mutex> g(oh->mu);
    oh->openRefused = true;
    return false;
  }
  if (!oh->wait()) {
    std::lock_guard<std::mutex> g(oh->mu);
    std::fprintf(stderr, "open: the handler was answered %s\n", oh->answer.c_str());
    return false;
  }
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
    bad += h->exactlyOne() ? 0 : 1;
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

// open-refused: an open the client refuses at once reaches the reader through
// its handler. Within the client's error window after a server failed, a new
// request to that host:port fails inside Open, before anything is sent. So:
// the test open fault sends every open to port 1 (never bound, so refused);
// the first open fails through its handler, as any connection failure does;
// the second, of a second handle in the same process, is the one refused at
// once. For a handle the cache serves, Open must return OK for both, and each
// handler get exactly one failure, with a host list, on another thread than
// the one in Open (a completion given inside Open would run there). A plugin
// without this returns the second failure from Open itself, which is also
// what shows that the second open was refused at once.
//
// With UCACHE_OPEN_RETRIES=1 and the fault on both attempts
// (UCACHE_TEST_OPEN_FAIL_N=2), the opens go through the retrying handler: the
// first handle's retry is the one refused at once, and its failure must carry
// the host list of the attempt that reached the client (naming 127.0.0.1:1);
// the second handle's first attempt is refused at once, inside Open, and what
// follows must still be answered off the opening thread.
int runOpenRefused(const char* url) {
  const char* mode = "open-refused";
  const int retries = envInt("UCACHE_OPEN_RETRIES", 0);
  if ((retries != 0 && retries != 1) || envInt("UCACHE_TEST_OPEN_FAIL_N", 0) < retries + 1 ||
      envInt("UCACHE_REVALIDATE_S", -1) != 0 || envInt("XRD_CONNECTIONRETRY", 0) != 1) {
    std::printf("%s: SETUP: needs UCACHE_OPEN_RETRIES=0 with UCACHE_TEST_OPEN_FAIL_N>=1, or "
                "UCACHE_OPEN_RETRIES=1 with UCACHE_TEST_OPEN_FAIL_N>=2; UCACHE_REVALIDATE_S=0 "
                "and XRD_CONNECTIONRETRY=1\n",
                mode);
    return 2;
  }
  struct Handler : StrictOpenHandler {
    std::thread::id on;
    bool nullHosts = false;
    std::string hosts; // the first completion's, host:port joined by spaces
    void HandleResponseWithHosts(XrdCl::XRootDStatus* status, XrdCl::AnyObject* response,
                                 XrdCl::HostList* hostList) override {
      {
        std::lock_guard<std::mutex> g(mu);
        if (completions == 0) {
          on = std::this_thread::get_id();
          if (hostList)
            for (const auto& h : *hostList)
              hosts += (hosts.empty() ? "" : " ") + h.url.GetHostId();
        }
        nullHosts = nullHosts || hostList == nullptr;
      }
      StrictOpenHandler::HandleResponseWithHosts(status, response, hostList);
    }
  };
  int bad = 0;
  for (int i = 1; i <= 2; ++i) {
    auto* f = new XrdCl::File(); // leaked with its handler: a late completion lands safely
    auto* h = new Handler();
    const XrdCl::XRootDStatus st = f->Open(url, XrdCl::OpenFlags::Read, XrdCl::Access::None, h);
    std::printf("%s: open %d: Open() returned %s\n", mode, i, st.ToString().c_str());
    if (!st.IsOK()) {
      ++bad;
      continue;
    }
    if (h->succeededWithin(std::chrono::seconds(60)) || h->count() == 0) {
      std::printf("%s: open %d: FAIL: %s\n", mode, i,
                  h->count() ? "the open succeeded (the fault did not reach it)"
                             : "no completion within 60 s");
      ++bad;
      continue;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1)); // room for a second completion
    std::lock_guard<std::mutex> g(h->mu);
    std::printf("%s: open %d: the handler was answered %s (%d completion(s)), hosts [%s]\n", mode,
                i, h->answer.c_str(), h->completions, h->hosts.c_str());
    if (h->completions != 1 || h->viaPlain) {
      std::printf("%s: open %d: FAIL: not exactly one completion\n", mode, i);
      ++bad;
    }
    if (h->on == std::this_thread::get_id()) {
      std::printf("%s: open %d: FAIL: answered inside Open, on the opening thread\n", mode, i);
      ++bad;
    }
    if (h->nullHosts) {
      std::printf("%s: open %d: FAIL: the failure came with a null host list\n", mode, i);
      ++bad;
    }
    if (retries == 1 && i == 1 && h->hosts.find("127.0.0.1:1") == std::string::npos) {
      std::printf("%s: open 1: FAIL: the failure does not carry the hosts of the attempt that "
                  "reached the client\n",
                  mode);
      ++bad;
    }
  }
  std::printf(bad ? "%s: FAIL\n" : "%s: clean\n", mode);
  return bad ? 1 : 0;
}

// A read's completion, with the contract every completion must keep: exactly
// one, through HandleResponseWithHosts, with a non-null HostList. Handlers live
// for the process, so a late second completion is counted.
struct ReadWait : XrdCl::ResponseHandler {
  std::mutex mu;
  std::condition_variable cv;
  int completions = 0;
  bool viaPlain = false, nullHosts = false;
  XrdCl::XRootDStatus status; // the first completion's
  void HandleResponse(XrdCl::XRootDStatus* s, XrdCl::AnyObject* r) override {
    {
      std::lock_guard<std::mutex> g(mu);
      viaPlain = true;
    }
    HandleResponseWithHosts(s, r, nullptr);
  }
  void HandleResponseWithHosts(XrdCl::XRootDStatus* s, XrdCl::AnyObject* r,
                               XrdCl::HostList* h) override {
    std::lock_guard<std::mutex> g(mu);
    if (!h)
      nullHosts = true;
    if (++completions == 1 && s)
      status = *s;
    delete s;
    delete r;
    delete h;
    cv.notify_all();
  }
  bool wait() {
    std::unique_lock<std::mutex> l(mu);
    return cv.wait_for(l, std::chrono::seconds(120), [&] { return completions > 0; });
  }
  bool contractKept() {
    std::lock_guard<std::mutex> g(mu);
    return completions == 1 && !viaPlain && !nullHosts;
  }
};

#ifdef UCACHE_FUZZ_HAVE_TRANSPOSE
// The uCache plugin this process mapped, from /proc/self/maps ("none" if none).
std::string mappedPlugin() {
  FILE* m = std::fopen("/proc/self/maps", "r");
  std::string found = "none";
  char line[4096];
  while (m && std::fgets(line, sizeof line, m))
    if (const char* at = std::strstr(line, "libXrdClUCache"))
      if (const char* path = std::strchr(line, '/'); path && path < at)
        found.assign(path, std::strcspn(path, "\n"));
  if (m)
    std::fclose(m);
  return found;
}
#endif

// key-check: check_origin_integrity's key-position check of byte-cache misses.
// <root-url> serves a copy of <local-path> whose basket at <off> (<len> bytes)
// is zeros. Run with UCACHE_CHECK_ORIGIN_INTEGRITY=on and a fresh cache. The
// zeroed basket lies wholly in the last page of the basket before it, which
// ends where it starts. On new handles, one after another:
//   0. (with UCACHE_KEYCHECK_STEP0, see below) the parse of the file's records
//      that a miss runs: no other handle waits for it, a held origin stops it
//      within the read's own timeout, and a later miss parses again;
//   1. a Read of the basket before it is a miss whose rounded edge page holds
//      the zeroed basket: its bytes are exact;
//   2. a Read of the zeroed basket fails with errDataError, "failed its check"
//      in the message (it was not served from the edge page of 1: nothing of
//      that fetch was kept);
//   3. a VectorRead of it fails the same way;
//   4. a VectorRead of the basket before it reads exactly;
//   5. a Read of the zeroed basket still fails its check (nothing of 4 kept);
//   6. Reads of the zeroed basket on two fresh handles at once (one waits for
//      the other's fetch) both fail their check, within seconds;
//   7. seven Reads of it in a row on one handle all fail their check (a failed
//      check never trips the handle to pass-through).
// Every completion is exactly one, through HandleResponseWithHosts, with a
// non-null HostList.
int runKeyCheck(const char* url, const char* local, uint64_t off, uint32_t len) {
  const char* mode = "key-check";
  std::setvbuf(stdout, nullptr, _IOLBF, 0); // a run cut off by a timeout keeps what it printed
#ifdef UCACHE_FUZZ_HAVE_TRANSPOSE
  const char* on = ::getenv("UCACHE_CHECK_ORIGIN_INTEGRITY");
  if (!on || std::strcmp(on, "on") != 0) {
    std::printf("%s: SETUP: run with UCACHE_CHECK_ORIGIN_INTEGRITY=on\n", mode);
    return 2;
  }
  const ucache::transpose::FileMeta fm = ucache::transpose::parseFile(local);
  std::vector<std::pair<uint64_t, uint32_t>> baskets;
  for (const auto& b : fm.branches)
    for (size_t i = 0; i < b.basketSeek.size() && i < b.basketBytes.size(); ++i)
      if (b.basketSeek[i] > 0)
        baskets.emplace_back(static_cast<uint64_t>(b.basketSeek[i]),
                             static_cast<uint32_t>(b.basketBytes[i]));
  std::sort(baskets.begin(), baskets.end());
  auto it = std::lower_bound(baskets.begin(), baskets.end(), std::make_pair(off, uint32_t{0}));
  if (!fm.error.empty() || it == baskets.end() || it->first != off || it->second != len ||
      it == baskets.begin()) {
    std::printf("%s: SETUP: no basket of %u bytes at %llu with one before it in %s\n", mode, len,
                (unsigned long long)off, local);
    return 2;
  }
  const auto prev = *(it - 1);
  const char* ps = ::getenv("UCACHE_PAGE_SIZE");
  const uint64_t page = ps ? ::strtoull(ps, nullptr, 10) : 4096;
  const uint64_t prevEnd = prev.first + prev.second;
  const uint64_t edgeEnd = (prevEnd + page - 1) / page * page;
  if (prevEnd != off || off % page == 0 || off + len > edgeEnd) {
    std::printf("%s: SETUP: the zeroed basket is not wholly in the last page of the basket "
                "before it (%llu+%u, page %llu)\n",
                mode, (unsigned long long)prev.first, prev.second, (unsigned long long)page);
    return 2;
  }
  // Commands that stop the origin answering and let it answer again.
  const char* hold = ::getenv("UCACHE_KEYCHECK_HOLD");
  const char* release = ::getenv("UCACHE_KEYCHECK_RELEASE");
  // Step 0 reads a basket X that shares no page with the two above, so what it
  // keeps changes nothing for the steps after it.
  const bool step0 = ::getenv("UCACHE_KEYCHECK_STEP0") != nullptr;
  const auto x = std::find_if(baskets.begin(), baskets.end(), [&](const auto& b) {
    return b.first + b.second <= prev.first / page * page || b.first >= edgeEnd;
  });
  const char* reval = ::getenv("UCACHE_REVALIDATE_S");
  if (step0 && (!hold || !release || !reval || std::strcmp(reval, "0") != 0 ||
                x == baskets.end())) {
    std::printf("%s: SETUP: step 0 needs UCACHE_KEYCHECK_HOLD and _RELEASE, "
                "UCACHE_REVALIDATE_S=0 (a trusted handle would open the origin at its first "
                "miss, and wait there), and a basket that shares no page with those two\n",
                mode);
    return 2;
  }
  // The fault: the origin serves zeros there (read with no plugin).
  {
    XrdCl::File plain(false);
    std::vector<char> z(len, 1);
    uint32_t got = 0;
    if (!plain.Open(url, XrdCl::OpenFlags::Read).IsOK() ||
        !plain.Read(off, len, z.data(), got).IsOK() || got != len ||
        std::count(z.begin(), z.end(), 0) != static_cast<std::ptrdiff_t>(len)) {
      std::printf("%s: SETUP: the origin does not serve zeros at %llu+%u\n", mode,
                  (unsigned long long)off, len);
      return 2;
    }
    const XrdCl::XRootDStatus closed = plain.Close();
    (void)closed;
  }
  std::printf("%s: zeroed basket %llu+%u, the one before %llu+%u\n", mode,
              (unsigned long long)off, len, (unsigned long long)prev.first, prev.second);
  Source src(local);
  int fails = 0;
  auto verdict = [&](bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    fails += ok ? 0 : 1;
  };
  // Handlers and buffers live for the process: a late completion lands in them.
  std::vector<ReadWait*> handlers;
  auto buffer = [](size_t n) { return (new std::vector<char>(n, 0x5A))->data(); };
  std::unique_ptr<XrdCl::File> file;
  auto reopen = [&]() { // a close waits for the pages a read keeps
    if (file && !file->Close().IsOK())
      return false;
    file = std::make_unique<XrdCl::File>(); // plugins enabled
    return strictOpen(*file, url);
  };
  auto failsTheCheck = [&](ReadWait* w, int seconds) {
    std::unique_lock<std::mutex> l(w->mu);
    const bool done = w->cv.wait_for(l, std::chrono::seconds(seconds),
                                     [&] { return w->completions > 0; });
    const std::string said = done ? w->status.ToStr() : "no answer";
    return std::make_pair(done && w->status.code == XrdCl::errDataError &&
                              said.find("failed its check") != std::string::npos,
                          said);
  };
  auto readZeroed = [&](XrdCl::File& f, const char* what) {
    auto* w = new ReadWait();
    handlers.push_back(w);
    const XrdCl::XRootDStatus s = f.Read(off, len, buffer(len), w);
    const auto r = s.IsOK() ? failsTheCheck(w, 120) : std::make_pair(false, s.ToString());
    verdict(r.first, std::string(what) + ": " + r.second);
    return r.first;
  };
  auto prevExact = [&](bool vector, const char* what, XrdCl::File* on = nullptr) {
    auto* w = new ReadWait();
    handlers.push_back(w);
    char* buf = buffer(prev.second);
    XrdCl::ChunkList chunks{XrdCl::ChunkInfo(prev.first, prev.second, buf)};
    XrdCl::File& f = on ? *on : *file;
    const XrdCl::XRootDStatus s = vector ? f.VectorRead(chunks, nullptr, w)
                                         : f.Read(prev.first, prev.second, buf, w);
    bool ok = s.IsOK() && w->wait();
    std::lock_guard<std::mutex> g(w->mu);
    ok = ok && w->status.IsOK() &&
         std::memcmp(buf, src.bytes.data() + prev.first, prev.second) == 0;
    verdict(ok, what);
  };
  // 0. Before anything of the file is read, so its records are not cached:
  // with the origin held, handle 1's Read of X runs the parse of the records
  // and waits in it, and handle 2's Read() call returns at once (it does not
  // wait for another handle's parse; its miss goes unchecked). The parse gives
  // up within handle 1's read timeout (3 s). Once the origin answers again and
  // a second has passed, a miss parses again: the zeroed basket fails its check.
  // Its two handles are the file's only ones; they are closed after it, and a
  // read it parked keeps the entry for the 10 s of its missed-wakeup timer, so
  // the steps after it start 11 s later, on a new entry of the file, as they
  // do without it.
  if (step0) {
    auto h1 = std::make_unique<XrdCl::File>(), h2 = std::make_unique<XrdCl::File>();
    if (!strictOpen(*h1, url) || !strictOpen(*h2, url) || std::system(hold) != 0) {
      std::printf("%s: SETUP: step 0's opens or the hold command failed\n", mode);
      return 2;
    }
    auto* w1 = new ReadWait();
    auto* w2 = new ReadWait();
    handlers.push_back(w1);
    handlers.push_back(w2);
    char* b1 = buffer(x->second);
    char* b2 = buffer(x->second);
    XrdCl::XRootDStatus s1;
    std::atomic<bool> back{false};
    std::thread t([&] {
      s1 = h1->Read(x->first, x->second, b1, w1, 3);
      back = true;
    });
    std::this_thread::sleep_for(std::chrono::seconds(1));
    verdict(!back, "0. handle 1's Read() call of X (" + std::to_string(x->first) +
                       ") waits in its parse of the file's records (the origin is held)");
    const auto t0 = std::chrono::steady_clock::now();
    const XrdCl::XRootDStatus s2 = h2->Read(x->first, x->second, b2, w2);
    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const bool parsing = !back;
    verdict(took < 1.0 && parsing, "0. handle 2's Read() call returns in " +
                                       std::to_string(took) + " s, before handle 1's (" +
                                       (parsing ? "it did" : "it did not") + ")");
    for (int i = 0; i < 600 && !back; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    verdict(back, "0. handle 1's Read() call returns while the origin is held (the parse's "
                  "reads carry the read's own timeout)");
    const bool released = std::system(release) == 0;
    t.join();
    if (!released) {
      std::printf("%s: SETUP: the release command failed: %s\n", mode, release);
      return 2;
    }
    bool exact = true;
    for (auto [st, w, b] : {std::make_tuple(s1, w1, b1), std::make_tuple(s2, w2, b2)}) {
      const bool done = st.IsOK() && w->wait();
      std::lock_guard<std::mutex> g(w->mu);
      exact = exact && done && w->status.IsOK() &&
              std::memcmp(b, src.bytes.data() + x->first, x->second) == 0;
    }
    verdict(exact, "0. both Reads of X are exact once the origin answers");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    readZeroed(*h1, "0. a Read of the zeroed basket on handle 1 then fails its check (the "
                    "parse a failed read stopped ran again)");
    const XrdCl::XRootDStatus c1 = h1->Close(), c2 = h2->Close();
    (void)c1;
    (void)c2;
    std::this_thread::sleep_for(std::chrono::seconds(11));
  }
  if (!reopen()) {
    std::printf("%s: SETUP: open failed\n", mode);
    return 2;
  }
  std::printf("PLUGIN %s\n", mappedPlugin().c_str());
  prevExact(false, "1. a Read of the basket before the zeroed one is exact (its edge page holds "
                   "the zeroed basket)");
  if (!reopen()) {
    std::printf("%s: SETUP: close and reopen failed\n", mode);
    return 2;
  }
  readZeroed(*file, "2. a Read of the zeroed basket fails its check (a miss, not the edge page)");
  {
    auto* w = new ReadWait();
    handlers.push_back(w);
    XrdCl::ChunkList chunks{XrdCl::ChunkInfo(off, len, buffer(len))};
    const XrdCl::XRootDStatus s = file->VectorRead(chunks, nullptr, w);
    const auto r = s.IsOK() ? failsTheCheck(w, 120) : std::make_pair(false, s.ToString());
    verdict(r.first, "3. a VectorRead of the zeroed basket fails its check: " + r.second);
  }
  if (!reopen()) {
    std::printf("%s: SETUP: close and reopen failed\n", mode);
    return 2;
  }
  prevExact(true, "4. a VectorRead of the basket before the zeroed one is exact");
  if (!reopen()) {
    std::printf("%s: SETUP: close and reopen failed\n", mode);
    return 2;
  }
  readZeroed(*file, "5. a Read of the zeroed basket after 4 fails its check (a miss again)");
  // 6. Two handles of a fresh entry at once: the second read finds the first's
  // fetch in flight and waits for it; when that fetch fails its check, it must
  // fetch, and fail, for itself, and not wait out a timeout. Both handles first
  // read the basket before (which arms the check). With UCACHE_KEYCHECK_HOLD
  // and UCACHE_KEYCHECK_RELEASE (commands that stop the origin answering and
  // let it answer again), both reads are issued while the origin is held, so
  // the second surely waits for the first's fetch; without them it may not.
  {
    if (!reopen()) {
      std::printf("%s: SETUP: close and reopen failed\n", mode);
      return 2;
    }
    auto other = std::make_unique<XrdCl::File>();
    if (!strictOpen(*other, url)) {
      std::printf("%s: SETUP: second open failed\n", mode);
      return 2;
    }
    prevExact(false, "6. (setup) the basket before the zeroed one is exact on the first handle");
    prevExact(false, "6. (setup) ... and on the second", other.get());
    const bool held = hold && release;
    if (held && std::system(hold) != 0) {
      std::printf("%s: SETUP: the hold command failed: %s\n", mode, hold);
      return 2;
    }
    auto* w1 = new ReadWait();
    auto* w2 = new ReadWait();
    handlers.push_back(w1);
    handlers.push_back(w2);
    const XrdCl::XRootDStatus s1 = file->Read(off, len, buffer(len), w1);
    const XrdCl::XRootDStatus s2 = other->Read(off, len, buffer(len), w2);
    if (held) {
      int early = 0;
      for (auto* w : {w1, w2}) {
        std::lock_guard<std::mutex> g(w->mu);
        early += w->completions;
      }
      verdict(early == 0, "6. neither read was answered while the origin was held (" +
                              std::to_string(early) + ")");
      if (std::system(release) != 0) {
        std::printf("%s: SETUP: the release command failed: %s\n", mode, release);
        return 2;
      }
    }
    const auto r1 = s1.IsOK() ? failsTheCheck(w1, 5) : std::make_pair(false, s1.ToString());
    const auto r2 = s2.IsOK() ? failsTheCheck(w2, 5) : std::make_pair(false, s2.ToString());
    verdict(r1.first, std::string("6. the first of two Reads at once fails its check within 5 s") +
                          (held ? " (both issued while the origin was held)" : "") + ": " +
                          r1.second);
    verdict(r2.first, "6. the second, which waited for the first's fetch, fails its check within "
                      "5 s: " + r2.second);
    const XrdCl::XRootDStatus closed = other->Close();
    (void)closed;
  }
  // 7. A failed check is not a cache error: a handle that fails it again and
  // again never trips to passing reads straight to the origin, which would
  // serve the zeros.
  if (!reopen()) {
    std::printf("%s: SETUP: close and reopen failed\n", mode);
    return 2;
  }
  for (int i = 1; i <= 7; ++i)
    if (!readZeroed(*file, ("7. failing Read " + std::to_string(i) + " of 7 on one handle fails "
                            "its check").c_str()))
      break; // one failure says it; the rest would only wait
  const XrdCl::XRootDStatus cs = file->Close();
  if (!cs.IsOK())
    std::fprintf(stderr, "close: %s\n", cs.ToString().c_str());
  std::this_thread::sleep_for(std::chrono::seconds(1)); // room for a late second completion
  bool kept = true;
  for (auto* w : handlers)
    kept = kept && w->contractKept();
  verdict(kept, "every read had exactly one completion, through HandleResponseWithHosts, with a "
                "non-null HostList");
  verdict(openHandlersBad() == 0, "every open had exactly one completion");
  std::printf("%s: %s\n", mode, fails ? "FAILED" : "clean");
  return fails ? 1 : 0;
#else
  (void)url;
  (void)local;
  (void)off;
  (void)len;
  std::printf("%s: SETUP: built without the tree parser\n", mode);
  return 2;
#endif
}

// open-property: a property the reader sets while its open is under way
// reaches the File that a retry puts in place of the first. The first
// attempt is faulted (UCACHE_TEST_OPEN_FAIL_N=1) and retried after a backoff
// (UCACHE_OPEN_RETRIES=1, UCACHE_OPEN_RETRY_BASE_MS of 1000 or more, so the
// property is set before the File is replaced), with adaptive_federation on.
// ReadRecovery is set to false right after Open returns; after the open it
// must read false. A plugin that sets it on the first File only reads the
// new File's default, true.
int runOpenProperty(const char* url) {
  const char* mode = "open-property";
  if (envInt("UCACHE_OPEN_RETRIES", 0) != 1 || envInt("UCACHE_TEST_OPEN_FAIL_N", 0) != 1 ||
      envInt("UCACHE_OPEN_RETRY_BASE_MS", 0) < 1000 || envInt("UCACHE_REVALIDATE_S", -1) != 0) {
    std::printf("%s: SETUP: needs UCACHE_OPEN_RETRIES=1, UCACHE_TEST_OPEN_FAIL_N=1, "
                "UCACHE_OPEN_RETRY_BASE_MS>=1000 and UCACHE_REVALIDATE_S=0\n",
                mode);
    return 2;
  }
  auto* f = new XrdCl::File(); // leaked with its handler: a late completion lands safely
  auto* h = new StrictOpenHandler();
  const XrdCl::XRootDStatus st = f->Open(url, XrdCl::OpenFlags::Read, XrdCl::Access::None, h);
  const bool set = f->SetProperty("ReadRecovery", "false");
  if (!st.IsOK() || !h->succeededWithin(std::chrono::seconds(60))) {
    std::printf("%s: FAIL: the open (Open() %s, %d completion(s))\n", mode, st.ToString().c_str(),
                h->count());
    return 1;
  }
  std::string v;
  const bool got = f->GetProperty("ReadRecovery", v);
  std::this_thread::sleep_for(std::chrono::seconds(1)); // room for a second completion
  std::printf("%s: SetProperty %s, ReadRecovery after the open: %s, %d completion(s)\n", mode,
              set ? "ok" : "refused", got ? v.c_str() : "(none)", h->count());
  const bool good = set && got && v == "false" && h->exactlyOne();
  std::printf(good ? "%s: clean\n" : "%s: FAIL\n", mode);
  return good ? 0 : 1;
}

// open-failover N: N opens, one after another, each with its own handler,
// through a redirector where a server refuses the file (the gate's fault):
// every open must get exactly one completion, a success, with a host list,
// and read the file's first 64 KiB right. Which opens failed over is in the
// records of the cache directory.
int runOpenFailover(const char* url, const char* local, int n) {
  const char* mode = "open-failover";
  const int fd = ::open(local, O_RDONLY);
  if (fd < 0) {
    std::printf("%s: SETUP: cannot read %s\n", mode, local);
    return 2;
  }
  const uint32_t len = 65536;
  std::vector<char> ref(len), got(len);
  if (::pread(fd, ref.data(), len, 0) != static_cast<ssize_t>(len)) {
    std::printf("%s: SETUP: %s is shorter than 64 KiB\n", mode, local);
    return 2;
  }
  ::close(fd);
  std::vector<StrictOpenHandler*> hs;
  int bad = 0;
  for (int i = 0; i < n; ++i) {
    auto* f = new XrdCl::File(); // leaked with its handler: a late completion lands safely
    auto* h = new StrictOpenHandler();
    hs.push_back(h);
    const XrdCl::XRootDStatus st = f->Open(url, XrdCl::OpenFlags::Read, XrdCl::Access::None, h);
    if (!st.IsOK() || !h->succeededWithin(std::chrono::seconds(200))) {
      std::printf("%s: open %d: FAIL: Open() %s, answer '%s'\n", mode, i + 1,
                  st.ToString().c_str(), h->answer.c_str());
      ++bad;
      continue;
    }
    uint32_t bytes = 0;
    const XrdCl::XRootDStatus rs = f->Read(0, len, got.data(), bytes);
    if (!rs.IsOK() || bytes != len || std::memcmp(got.data(), ref.data(), len) != 0) {
      std::printf("%s: open %d: FAIL: the read (%s, %u bytes)\n", mode, i + 1,
                  rs.ToString().c_str(), bytes);
      ++bad;
    }
    const XrdCl::XRootDStatus cs = f->Close();
    (void)cs;
  }
  std::this_thread::sleep_for(std::chrono::seconds(2)); // room for a late second completion
  int notOne = 0;
  for (auto* h : hs)
    notOne += h->exactlyOne() ? 0 : 1;
  std::printf("%s: %d open(s), %d failed, %d without exactly one completion\n", mode, n, bad,
              notOne);
  std::printf(bad || notOne ? "%s: FAIL\n" : "%s: clean\n", mode);
  return bad || notOne ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <root-url> <local-path> [ops] [threads] [seed] "
                 "[fuzz|inner-race|inner-race-relay|open-refused|open-property|open-failover|"
                 "key-check <off> <len>]\n",
                 argv[0]);
    return 2;
  }
  const char* url = argv[1];
  const std::string mode = argc > 6 ? argv[6] : "fuzz";
  if (mode == "key-check")
    return argc > 8 ? runKeyCheck(url, argv[2], ::strtoull(argv[7], nullptr, 10),
                                  static_cast<uint32_t>(::strtoul(argv[8], nullptr, 10)))
                    : 2;
  if (mode == "inner-race" || mode == "inner-race-relay")
    return runInnerRace(url, argv[2], argc > 3 ? ::strtoull(argv[3], nullptr, 10) : 200,
                        argc > 4 ? ::atoi(argv[4]) : 4, mode == "inner-race-relay");
  if (mode == "open-refused")
    return runOpenRefused(url);
  if (mode == "open-property")
    return runOpenProperty(url);
  if (mode == "open-failover")
    return runOpenFailover(url, argv[2], argc > 3 ? ::atoi(argv[3]) : 12);
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
  std::this_thread::sleep_for(std::chrono::seconds(1)); // room for a late second completion
  if (const int bad = openHandlersBad()) {
    std::fprintf(stderr, "FAIL: %d open(s) without exactly one completion\n", bad);
    ++gFailures;
  }
  if (gFailures) {
    std::fprintf(stderr, "FAIL seed=%llu failures=%llu\n", (unsigned long long)seed,
                 (unsigned long long)gFailures.load());
    return 1;
  }
  std::printf("plugin_fuzz: clean (%llu ops x %d threads)\n", (unsigned long long)opsPer,
              threads);
  return 0;
}
