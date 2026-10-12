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
// open-refused checks how an open the client refuses at once is answered: from
// Open itself with the switch off, as without uCache; through the reader's
// handler with the switch on (see runOpenRefused).
//
// Two more cover an open made again, with adaptive_federation on:
//   open-property      a property set while the open is retried reaches the
//                      File that replaces the first (see runOpenProperty)
//   open-property-race a ReadRecovery set just as the checks made at open
//                      begin does not land on the File during their reads
//                      (see runOpenPropertyRace)
//   open-stat-during-checks [cut] a Stat made while the checks made at open
//                      run waits for the open's answer: it is never given a
//                      copy the checks refuse, and what they read of the copy
//                      the open takes is stored in the entry (see
//                      runOpenStatDuringChecks)
//   open-park-from-callback an asynchronous Stat and Read made from another
//                      file's callback while the checks made at open run
//                      return at once and are answered after the open (see
//                      runOpenParkFromCallback)
//   open-failover [N]  N opens through a redirector where a server refuses
//                      the file: exactly one completion each (see runOpenFailover)
//   open-denied        one open of a file that every server refuses: Open()
//                      returns OK and the reader gets one completion, a failure
//                      whose text is printed (see runOpenDenied)
//
// And one checks check_origin_integrity's key-position check of byte-cache
// misses against a copy with one basket zeroed (see runKeyCheck):
//   key-check <off> <len>   <root-url> serves that copy, <local-path> is the
//                      undamaged file; ops, threads and seed are ignored
//
// read-failover <case> checks that a read an origin failure was injected into
// (UCACHE_TEST_READ_FAIL_N, UCACHE_TEST_READ_FAIL_KIND) is answered with the
// right bytes, once, with adaptive_federation on and the file's copies behind a
// redirector (see runReadFailover). The caller checks the records. The cases
// zero, zerov and zeroerr do the same for a read of a basket that a server serves
// as zeros (see runZero), len, lenv and lenerr for one whose key states another length, rec,
// recv and recerr for one whose tree's record fails its own check, blk, blkv and blkerr for one
// whose compressed data fails its own checksum, stay for a read no check refuses, and cut, cutpg,
// cutpgpast, cutonly and end for a copy that is cut short after the open (see runCut), and degraded
// and degradedoff for a handle whose own open of the origin fails.
#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClURL.hh>
#ifdef UCACHE_FUZZ_HAVE_TRANSPOSE
#include "TreeMeta.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include <sys/stat.h>
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
  std::string message;      // and its message
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
    const std::string text = status ? status->GetErrorMessage() : std::string();
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
        message = text;
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

// open-refused: an open the client refuses at once. Within the client's error
// window after a server failed, a new request to that host:port fails inside
// Open, before anything is sent. So: the test open fault sends every open to
// port 1 (never bound, so refused); the first open fails through its handler,
// as any connection failure does; the second, of a second handle in the same
// process, is the one refused at once.
//
// With adaptive_federation off (the default) a handle the cache serves answers
// as the client does without uCache: the first open's failure comes through its
// handler (once, with a host list, on another thread than the one in Open), and
// the second Open returns the error itself, with no completion ever after. With
// UCACHE_OPEN_RETRIES=1 and the fault on both attempts (UCACHE_TEST_OPEN_FAIL_N=2)
// the opens go through the retries of open_retries: both Open calls return OK,
// and each handler gets one failure, with an empty host list, off the opening
// thread (the second handle's first attempt is refused inside Open, and its
// retry is scheduled, not made there).
//
// With UCACHE_ADAPTIVE_FEDERATION=on, Open returns OK for both, and each handler
// gets exactly one failure, with a host list, on another thread than the one in
// Open. With UCACHE_OPEN_RETRIES=1 the first handle's failure carries the host
// list of the attempt that reached the client (naming 127.0.0.1:1).
int runOpenRefused(const char* url) {
  const char* mode = "open-refused";
  const int retries = envInt("UCACHE_OPEN_RETRIES", 0);
  const char* sw = ::getenv("UCACHE_ADAPTIVE_FEDERATION");
  const bool fed = sw && (std::string(sw) == "on" || std::string(sw) == "1" ||
                          std::string(sw) == "true" || std::string(sw) == "yes");
  if ((retries != 0 && retries != 1) || envInt("UCACHE_TEST_OPEN_FAIL_N", 0) < retries + 1 ||
      envInt("UCACHE_REVALIDATE_S", -1) != 0 || envInt("XRD_CONNECTIONRETRY", 0) != 1) {
    std::printf("%s: SETUP: needs UCACHE_OPEN_RETRIES=0 with UCACHE_TEST_OPEN_FAIL_N>=1, or "
                "UCACHE_OPEN_RETRIES=1 with UCACHE_TEST_OPEN_FAIL_N>=2; UCACHE_REVALIDATE_S=0 "
                "and XRD_CONNECTIONRETRY=1\n",
                mode);
    return 2;
  }
  std::printf("%s: adaptive_federation %s, open_retries %d\n", mode, fed ? "on" : "off", retries);
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
    // Switch off, no retries: the second open, refused at once, is answered by Open itself.
    const bool fromOpen = !fed && retries == 0 && i == 2;
    if (fromOpen) {
      if (st.IsOK()) {
        std::printf("%s: open 2: FAIL: Open() returned OK; the client's own refusal is an error "
                    "from Open\n",
                    mode);
        ++bad;
      }
      std::this_thread::sleep_for(std::chrono::seconds(2)); // room for a completion, which must not come
      std::lock_guard<std::mutex> g(h->mu);
      std::printf("%s: open 2: %d completion(s) after the error from Open()\n", mode,
                  h->completions);
      if (h->completions != 0) {
        std::printf("%s: open 2: FAIL: a completion came after Open() %s\n", mode,
                    st.IsOK() ? "returned OK" : "returned the error");
        ++bad;
      }
      continue;
    }
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
    if (fed && retries == 1 && i == 1 && h->hosts.find("127.0.0.1:1") == std::string::npos) {
      std::printf("%s: open 1: FAIL: the failure does not carry the hosts of the attempt that "
                  "reached the client\n",
                  mode);
      ++bad;
    }
    if (!fed && retries == 1 && !h->hosts.empty()) {
      std::printf("%s: open %d: FAIL: the failure of the retries carries hosts; open_retries "
                  "gives an empty host list\n",
                  mode, i);
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

// open-property-race: ReadRecovery set by the reader just as the checks made at open begin (a file
// named *.root, adaptive_federation on). The checks turn the File's recovery off for their reads
// and give the reader's setting back after them; a setting recorded just before they begin must
// be on the File before they turn it off, never after. UCACHE_TEST_PROP_DELAY_MS (2000 or more)
// holds the setting between its recording and its setting on the File, and the checks' first read
// is sent late (UCACHE_TEST_HEADER_FAIL_N=1, _KIND=delay: 5 s), so that their reads are in flight
// when the reader looks: 3.5 s after the open is issued, ReadRecovery must read false; after the
// open, the reader's true.
int runOpenPropertyRace(const char* url) {
  const char* mode = "open-property-race";
  if (envInt("UCACHE_TEST_PROP_DELAY_MS", 0) < 2000 ||
      envInt("UCACHE_TEST_HEADER_FAIL_N", 0) != 1) {
    std::printf("%s: SETUP: needs UCACHE_TEST_PROP_DELAY_MS>=2000 and UCACHE_TEST_HEADER_FAIL_N=1 "
                "(_KIND=delay)\n",
                mode);
    return 2;
  }
  auto* f = new XrdCl::File(); // leaked with its handler: a late completion lands safely
  auto* h = new StrictOpenHandler();
  const auto t0 = std::chrono::steady_clock::now();
  const XrdCl::XRootDStatus st = f->Open(url, XrdCl::OpenFlags::Read, XrdCl::Access::None, h);
  bool set = false;
  std::thread setter([&] { set = f->SetProperty("ReadRecovery", "true"); });
  std::this_thread::sleep_until(t0 + std::chrono::milliseconds(3500));
  std::string during;
  const bool gotDuring = f->GetProperty("ReadRecovery", during);
  const bool doneEarly = h->count() > 0; // the checks' reads were still in flight
  setter.join();
  if (!st.IsOK() || !h->succeededWithin(std::chrono::seconds(60))) {
    std::printf("%s: FAIL: the open (Open() %s, %d completion(s))\n", mode, st.ToString().c_str(),
                h->count());
    return 1;
  }
  std::string after;
  const bool gotAfter = f->GetProperty("ReadRecovery", after);
  std::this_thread::sleep_for(std::chrono::seconds(1)); // room for a second completion
  std::printf("%s: SetProperty %s; ReadRecovery during the checks' reads: %s%s, after the open: "
              "%s; %d completion(s)\n",
              mode, set ? "ok" : "refused", gotDuring ? during.c_str() : "(none)",
              doneEarly ? " (the open had already completed)" : "",
              gotAfter ? after.c_str() : "(none)", h->count());
  const bool good = set && !doneEarly && gotDuring && during == "false" && gotAfter &&
                    after == "true" && h->exactlyOne();
  std::printf(good ? "%s: clean\n" : "%s: FAIL\n", mode);
  return good ? 0 : 1;
}

// open-stat-during-checks [cut]: a Stat made by the reader while the checks made at open run (a
// file named *.root that the cache caches, adaptive_federation on; the checks' first read sent 5 s
// late, UCACHE_TEST_HEADER_FAIL_N=1, _KIND=delay). Until the open answers, its File may be open at
// a copy the checks have yet to judge: the Stat waits for the open's answer before the handle is
// set up, so neither the entry nor the reader is given the size of a copy the checks then refuse.
// 1.5 s after the open is issued, a Stat; it must answer no earlier than 4.5 s after the open was
// issued (the checks' late read). Without `cut`, the copies are whole: the Stat answers the file's
// size, the open succeeds, and after it (and a second) a Read of the first 300 bytes is the file's;
// the gate checks that what the checks read went into the entry. With `cut`, <local-path> is the
// cut copy every server holds (its header states a larger end): the open fails, once, and the Stat
// never answers the cut copy's size.
int runOpenStatDuringChecks(const char* url, const char* local, bool cut) {
  const char* mode = "open-stat-during-checks";
  if (envInt("UCACHE_TEST_HEADER_FAIL_N", 0) != 1) {
    std::printf("%s: SETUP: needs UCACHE_TEST_HEADER_FAIL_N=1 (_KIND=delay)\n", mode);
    return 2;
  }
  const int fd = ::open(local, O_RDONLY);
  struct stat sb{};
  std::vector<char> ref(300), got(300);
  if (fd < 0 || ::fstat(fd, &sb) != 0 || ::pread(fd, ref.data(), ref.size(), 0) != 300) {
    std::printf("%s: SETUP: cannot read 300 bytes of %s\n", mode, local);
    if (fd >= 0)
      ::close(fd);
    return 2;
  }
  ::close(fd);
  auto* f = new XrdCl::File(); // leaked with its handler: a late completion lands safely
  auto* h = new StrictOpenHandler();
  const auto t0 = std::chrono::steady_clock::now();
  const XrdCl::XRootDStatus st = f->Open(url, XrdCl::OpenFlags::Read, XrdCl::Access::None, h);
  std::this_thread::sleep_until(t0 + std::chrono::milliseconds(1500));
  XrdCl::StatInfo* si = nullptr;
  const XrdCl::XRootDStatus ss = f->Stat(false, si, 60);
  const long long statMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
  const uint64_t statSize = si ? si->GetSize() : 0;
  delete si;
  const bool doneEarly = h->count() > 0; // the open had answered when the Stat returned
  const bool openOk = h->succeededWithin(std::chrono::seconds(120));
  if (!st.IsOK() || openOk == cut) {
    std::printf("%s: FAIL: the open (Open() %s, %s, %d completion(s))\n", mode,
                st.ToString().c_str(), openOk ? "succeeded" : "failed", h->count());
    return 1;
  }
  bool exact = true;
  std::string readText = "not made";
  if (!cut) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    uint32_t bytes = 0;
    const XrdCl::XRootDStatus rs = f->Read(0, 300, got.data(), bytes);
    exact = rs.IsOK() && bytes == 300 && got == ref;
    readText = exact ? "exact" : rs.ToString();
  }
  std::this_thread::sleep_for(std::chrono::seconds(1)); // room for a second completion
  std::printf("%s: Stat during the checks: %s, size %llu, answered %lld ms after the open was "
              "issued%s; the open: %d completion(s), %s; the read of 300 bytes at 0: %s\n",
              mode, ss.ToString().c_str(), static_cast<unsigned long long>(statSize), statMs,
              doneEarly ? " (the open had already answered)" : "", h->count(),
              openOk ? "succeeded" : "failed", readText.c_str());
  if (!cut) {
    const XrdCl::XRootDStatus cs = f->Close();
    (void)cs;
  }
  const bool waited = statMs >= 4500;
  const bool statRight = cut ? !(ss.IsOK() && statSize == static_cast<uint64_t>(sb.st_size))
                             : ss.IsOK() && statSize == static_cast<uint64_t>(sb.st_size);
  const bool good = waited && statRight && exact && h->exactlyOne();
  std::printf("STATWAIT %s\n", waited ? "yes" : "no");
  std::printf(good ? "%s: clean\n" : "%s: FAIL\n", mode);
  return good ? 0 : 1;
}

// One request's completions, timed: the first one's status, its size (a Stat) or length (a read),
// the time it came, and how many came (the contract: exactly one, with a non-null HostList).
struct TimedAnswer : XrdCl::ResponseHandler {
  std::mutex mu;
  std::condition_variable cv;
  int completions = 0;
  bool ok = false;
  bool contract = true;
  uint64_t size = 0;
  std::chrono::steady_clock::time_point at;
  void HandleResponse(XrdCl::XRootDStatus* status, XrdCl::AnyObject* response) override {
    {
      std::lock_guard<std::mutex> g(mu);
      contract = false; // a completion through HandleResponse
    }
    HandleResponseWithHosts(status, response, nullptr);
  }
  void HandleResponseWithHosts(XrdCl::XRootDStatus* status, XrdCl::AnyObject* response,
                               XrdCl::HostList* hostList) override {
    const bool good = status && status->IsOK();
    uint64_t n = 0;
    if (good && response) {
      XrdCl::StatInfo* si = nullptr;
      XrdCl::ChunkInfo* ci = nullptr;
      response->Get(si); // null unless the object is of that type
      response->Get(ci);
      n = si ? si->GetSize() : ci ? ci->length : 0;
    }
    std::lock_guard<std::mutex> g(mu);
    if (!hostList)
      contract = false;
    if (++completions == 1) {
      ok = good;
      size = n;
      at = std::chrono::steady_clock::now();
    }
    delete status;
    delete response;
    delete hostList;
    cv.notify_all();
  }
  bool waitFor(std::chrono::seconds max) {
    std::unique_lock<std::mutex> l(mu);
    return cv.wait_for(l, max, [&] { return completions > 0; });
  }
};

// open-park-from-callback: an asynchronous Stat and Read made on a file whose reader's open is in
// flight (adaptive_federation on, a file named *.root the cache caches; the checks' first read sent
// 5 s late, UCACHE_TEST_HEADER_FAIL_N=1, _KIND=delay), from inside the completion of another file's
// request, on the client's own callback thread. Each call must return at once (within 1 s): the
// thread is the client's, and the open's own checks may need it to deliver their reads. Each
// request must then be answered once, after the open answered: the Stat with the file's size, the
// Read with the file's first 300 bytes. The other file is opened without plug-ins, so its answer
// comes on a client thread.
int runOpenParkFromCallback(const char* url, const char* local) {
  const char* mode = "open-park-from-callback";
  if (envInt("UCACHE_TEST_HEADER_FAIL_N", 0) != 1) {
    std::printf("%s: SETUP: needs UCACHE_TEST_HEADER_FAIL_N=1 (_KIND=delay)\n", mode);
    return 2;
  }
  const int fd = ::open(local, O_RDONLY);
  struct stat sb{};
  std::vector<char> ref(300);
  if (fd < 0 || ::fstat(fd, &sb) != 0 || ::pread(fd, ref.data(), ref.size(), 0) != 300) {
    std::printf("%s: SETUP: cannot read 300 bytes of %s\n", mode, local);
    if (fd >= 0)
      ::close(fd);
    return 2;
  }
  ::close(fd);
  auto* other = new XrdCl::File(false); // no plug-ins: its answers come on the client's threads
  if (!other->Open(url, XrdCl::OpenFlags::Read).IsOK()) {
    std::printf("%s: SETUP: the other file did not open\n", mode);
    return 2;
  }
  // Leaked with their handlers: a late completion lands safely.
  auto* f = new XrdCl::File();
  auto* h = new StrictOpenHandler();
  auto* hs = new TimedAnswer();
  auto* hr = new TimedAnswer();
  auto* buf = new std::vector<char>(300);
  struct FromCallback : XrdCl::ResponseHandler {
    XrdCl::File* f;
    TimedAnswer* hs;
    TimedAnswer* hr;
    std::vector<char>* buf;
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool mainThread = false;
    std::thread::id main;
    long long statCallMs = -1, readCallMs = -1;
    std::string statRet, readRet;
    void HandleResponseWithHosts(XrdCl::XRootDStatus* status, XrdCl::AnyObject* response,
                                 XrdCl::HostList* hostList) override {
      delete status;
      delete response;
      delete hostList;
      const auto t1 = std::chrono::steady_clock::now();
      const XrdCl::XRootDStatus ss = f->Stat(false, hs, 60);
      const auto t2 = std::chrono::steady_clock::now();
      const XrdCl::XRootDStatus rs = f->Read(0, 300, buf->data(), hr, 60);
      const auto t3 = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> g(mu);
      mainThread = std::this_thread::get_id() == main;
      statCallMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
      readCallMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
      statRet = ss.ToString();
      readRet = rs.ToString();
      if (!ss.IsOK())
        hs->HandleResponseWithHosts(new XrdCl::XRootDStatus(ss), nullptr, new XrdCl::HostList());
      if (!rs.IsOK())
        hr->HandleResponseWithHosts(new XrdCl::XRootDStatus(rs), nullptr, new XrdCl::HostList());
      done = true;
      cv.notify_all();
    }
  };
  auto* cb = new FromCallback();
  cb->f = f;
  cb->hs = hs;
  cb->hr = hr;
  cb->buf = buf;
  cb->main = std::this_thread::get_id();
  const auto t0 = std::chrono::steady_clock::now();
  const XrdCl::XRootDStatus st = f->Open(url, XrdCl::OpenFlags::Read, XrdCl::Access::None, h);
  std::this_thread::sleep_until(t0 + std::chrono::milliseconds(1500));
  const XrdCl::XRootDStatus os = other->Stat(true, cb, 60);
  bool cbDone = false;
  {
    std::unique_lock<std::mutex> l(cb->mu);
    cbDone = os.IsOK() && cb->cv.wait_for(l, std::chrono::seconds(200), [&] { return cb->done; });
  }
  const bool openOk = h->succeededWithin(std::chrono::seconds(120));
  const auto openAt = std::chrono::steady_clock::now(); // no earlier than the open's answer
  hs->waitFor(std::chrono::seconds(120));
  hr->waitFor(std::chrono::seconds(120));
  std::this_thread::sleep_for(std::chrono::seconds(1)); // room for a second completion
  auto ms = [&](std::chrono::steady_clock::time_point t) {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t - t0).count());
  };
  std::lock_guard<std::mutex> gs(hs->mu);
  std::lock_guard<std::mutex> gr(hr->mu);
  std::lock_guard<std::mutex> gc(cb->mu);
  const long long statAt = hs->completions ? ms(hs->at) : -1;
  const long long readAt = hr->completions ? ms(hr->at) : -1;
  const bool exact = hr->ok && hr->size == 300 && *buf == ref;
  std::printf("%s: the open: Open() %s, %s, %d completion(s), waited for until %lld ms; from the "
              "other file's callback (%s thread): Stat() returned %s in %lld ms, Read() returned "
              "%s in %lld ms; the Stat answered %s at %lld ms (%d completion(s), size %llu); the "
              "Read answered %s at %lld ms (%d completion(s), %s)\n",
              mode, st.ToString().c_str(), openOk ? "succeeded" : "failed", h->count(),
              ms(openAt), cb->mainThread ? "the main" : "a client", cb->statRet.c_str(),
              cb->statCallMs, cb->readRet.c_str(), cb->readCallMs, hs->ok ? "ok" : "in error",
              statAt, hs->completions, static_cast<unsigned long long>(hs->size),
              hr->ok ? "ok" : "in error", readAt, hr->completions,
              exact ? "the file's 300 bytes" : "not the file's bytes");
  const bool returnedAtOnce =
      cbDone && cb->statCallMs >= 0 && cb->statCallMs < 1000 && cb->readCallMs < 1000;
  std::printf("CALLS %s\n", returnedAtOnce ? "at-once" : "held");
  const bool good = st.IsOK() && openOk && h->exactlyOne() && cbDone && !cb->mainThread &&
                    returnedAtOnce && hs->completions == 1 && hs->contract && hs->ok &&
                    hs->size == static_cast<uint64_t>(sb.st_size) && statAt >= 4500 &&
                    hr->completions == 1 && hr->contract && exact && readAt >= 4500;
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

// open-denied: one open of a file that every server refuses. Open() returns OK
// and the handler gets exactly one completion, a failure, with a host list; the
// failure's text is printed for the caller to check.
int runOpenDenied(const char* url) {
  const char* mode = "open-denied";
  auto* f = new XrdCl::File(); // leaked with its handler: a late completion lands safely
  auto* h = new StrictOpenHandler();
  const XrdCl::XRootDStatus st = f->Open(url, XrdCl::OpenFlags::Read, XrdCl::Access::None, h);
  const bool succeeded = h->succeededWithin(std::chrono::seconds(200));
  std::this_thread::sleep_for(std::chrono::seconds(1)); // room for a second completion
  std::printf("%s: Open() %s, %d completion(s), %s: %s | %s\n", mode,
              st.IsOK() ? "ok" : st.ToString().c_str(), h->count(),
              succeeded ? "succeeded" : "failed", h->answer.c_str(), h->message.c_str());
  const bool good = st.IsOK() && !succeeded && h->count() == 1 && h->exactlyOne();
  std::printf(good ? "%s: clean\n" : "%s: FAIL\n", mode);
  return good ? 0 : 1;
}

// read-failover <case>: every read below is asynchronous, with its own handler,
// and must be answered exactly once (through HandleResponseWithHosts, with a
// non-null HostList) with the file's own bytes, whatever was injected into it.
// Each line printed is a verdict; the data servers a handle read from are
// printed as "SERVERS", for the caller to check against the records.
//   miss      two misses fail (UCACHE_TEST_READ_FAIL_N=2): a Read, then a
//             VectorRead. The handle is read from another server after each; its
//             ReadRecovery property is "false" and a SetProperty of "true" does
//             not change that
//   relay     (KIND=relay, N=2) a PgRead and a VectorRead into one buffer, which
//             are relayed to the origin, fail and are answered
//   refuse    (KIND=refuse, N=3) a Read, a PgRead and a VectorRead are refused
//             at once by the client (the handle may be read from the same server
//             after, which a refusal does not exclude)
//   final     (KIND=final, N=2) a Read and a VectorRead fail with an error no
//             other site would answer differently: the handle stays where it is
//   off       (N=2, the switch off) a Read and a VectorRead fail: the handle stays
//             where it is, and the client's own read recovery is on
//   many      (N=8, KIND unset or late) 8 threads read 4 pages each at once,
//             8 of them failing: all are exact
//   exhaust   (KIND=again, N=4) a PgRead, relayed, fails at the copy the handle
//             is at and at every copy each move takes, until its attempts (4)
//             are spent: it fails, once, with the connection error of its last
//             attempt and the error that names every copy it was read from
//             ("no copy could serve the read"), printed as "MSG"; a Read after
//             it is exact
struct ReadCase {
  const Source& src;
  XrdCl::File* file;
  std::vector<ReadWait*>& handlers;
  int& fails;
  std::string prop(const char* name) const {
    std::string v;
    return file->GetProperty(name, v) ? v : "(none)";
  }
  void verdict(bool ok, const std::string& what) const { // called by the threads of `many`
    static std::mutex mu;
    std::lock_guard<std::mutex> g(mu);
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    fails += ok ? 0 : 1;
  }
  // kind: 'r' Read, 'p' PgRead, 'v' VectorRead of two chunks (a part of one page
  // each, two pages apart), 'b' VectorRead into one buffer
  bool read(char kind, uint64_t off, const char* what) const {
    auto* w = new ReadWait();
    {
      static std::mutex mu;
      std::lock_guard<std::mutex> g(mu);
      handlers.push_back(w);
    }
    const uint32_t n = 65536;
    std::vector<std::vector<char>> bufs(2, std::vector<char>(n, 0x5A));
    XrdCl::ChunkList chunks{XrdCl::ChunkInfo(off, n, bufs[0].data()),
                            XrdCl::ChunkInfo(off + 3 * n, n, bufs[1].data())};
    std::vector<char> both(2 * n, 0x5A);
    XrdCl::XRootDStatus s;
    switch (kind) {
    case 'r':
      s = file->Read(off, n, bufs[0].data(), w);
      break;
    case 'p':
      s = file->PgRead(off, n, bufs[0].data(), w);
      break;
    case 'v':
      s = file->VectorRead(chunks, nullptr, w);
      break;
    default:
      s = file->VectorRead(chunks, both.data(), w);
      break;
    }
    bool ok = s.IsOK() && w->wait();
    {
      std::lock_guard<std::mutex> g(w->mu);
      ok = ok && w->status.IsOK();
    }
    const char* ref = src.bytes.data();
    if (kind == 'r' || kind == 'p')
      ok = ok && std::memcmp(bufs[0].data(), ref + off, n) == 0;
    else if (kind == 'v')
      ok = ok && std::memcmp(bufs[0].data(), ref + off, n) == 0 &&
           std::memcmp(bufs[1].data(), ref + off + 3 * n, n) == 0;
    else
      ok = ok && std::memcmp(both.data(), ref + off, n) == 0 &&
           std::memcmp(both.data() + n, ref + off + 3 * n, n) == 0;
    verdict(ok, std::string(what) +
                    (ok ? "" : " (" + (s.IsOK() ? w->status.ToStr() : s.ToString()) + ")"));
    return ok;
  }
};

// read-failover zero | zerov | zeroerr: <local-path> is the fixture of key-check (a ROOT
// file whose basket at 4915, 2481 bytes, lies wholly in the last page of the basket before
// it) and <root-url> serves it from the servers of a federation, with that basket zeroed on
// some of them; check_origin_integrity is on (adaptive_federation forces it). The handle is
// opened, then UCACHE_ZERO_CMD, when set, is run with the port of the server the handle
// landed on and zeroes the basket there. Then the basket is read:
//   zero     a Read: the bytes are the fixture's, once, from another server than the one
//            that served zeros
//   zerov    the same with a VectorRead
//   zeroerr  the read fails with errDataError and "failed its check" in the message, once:
//            no copy serves it (or the handle is not armed to move); never the zeros
// len, lenv, lenerr: the same, with the copy damaged elsewhere by UCACHE_ZERO_CMD: the basket's
// key states another length; rec, recv, recerr: the tree's record fails its own check; blk, blkv,
// blkpg, blkerr: a byte inside the basket's compressed data changed (its key intact), so that its
// data fails its own checksum (blkpg: read by a PgRead, which the cache relays to the origin);
// stay: the read is exact and made where the handle landed (the copy's damage concerns other
// bytes). The fault is then checked as the bytes the copy serves at
// UCACHE_FAULT_AT (UCACHE_FAULT_LEN of them, default 4) differing from the fixture's, read with no
// plugin. UCACHE_READ_AT and UCACHE_READ_LEN, when set, give the bytes read in place of the basket
// at 4915 (another basket, a tree's record).
// "SERVERS landed final" is printed for the caller to check against the records.
int runZero(const char* url, const char* local, const std::string& which) {
  const char* mode = "read-failover";
  const char* ra = ::getenv("UCACHE_READ_AT");
  const char* rl = ::getenv("UCACHE_READ_LEN");
  const uint64_t kOff = ra ? std::strtoull(ra, nullptr, 10) : 4915;
  const uint32_t kLen = rl ? static_cast<uint32_t>(std::strtoul(rl, nullptr, 10)) : 2481;
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  Source src(local);
  if (src.bytes.size() < kOff + kLen ||
      std::count(src.bytes.begin() + kOff, src.bytes.begin() + kOff + kLen, 0) ==
          static_cast<std::ptrdiff_t>(kLen)) {
    std::printf("%s: SETUP: %s has no non-zero bytes at %llu+%u\n", mode, local,
                static_cast<unsigned long long>(kOff), kLen);
    return 2;
  }
  int fails = 0;
  const auto verdict = [&](bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    fails += ok ? 0 : 1;
  };
  auto file = std::make_unique<XrdCl::File>(); // plugins enabled
  std::string landed;
  if (!strictOpen(*file, url) || !file->GetProperty("DataServer", landed)) {
    std::printf("%s: SETUP: open failed\n", mode);
    return 2;
  }
  if (const char* cmd = ::getenv("UCACHE_ZERO_CMD"))
    if (std::system((std::string(cmd) + " " + landed.substr(landed.rfind(':') + 1)).c_str()) != 0) {
      std::printf("%s: SETUP: %s failed\n", mode, cmd);
      return 2;
    }
  const bool zeros = which.rfind("zero", 0) == 0;
  const char* fa = ::getenv("UCACHE_FAULT_AT");
  const char* fl = ::getenv("UCACHE_FAULT_LEN");
  const uint64_t faultAt = zeros || !fa ? kOff : std::strtoull(fa, nullptr, 10);
  const uint32_t faultLen = zeros ? kLen : fl ? static_cast<uint32_t>(std::atoi(fl)) : 4;
  { // the fault: the server the handle landed on serves zeros there, or other bytes than the
    // fixture's (read with no plugin)
    XrdCl::File plain(false);
    std::vector<char> z(faultLen, 1);
    uint32_t got = 0;
    const std::string at = "root://" + landed + "/" + XrdCl::URL(url).GetPath();
    const bool read = faultAt + faultLen <= src.bytes.size() &&
                      plain.Open(at, XrdCl::OpenFlags::Read).IsOK() &&
                      plain.Read(faultAt, faultLen, z.data(), got).IsOK() && got == faultLen;
    const bool fault = read && (zeros ? std::count(z.begin(), z.end(), 0) ==
                                            static_cast<std::ptrdiff_t>(faultLen)
                                      : std::memcmp(z.data(), src.bytes.data() + faultAt,
                                                    faultLen) != 0);
    if (!fault) {
      std::printf("%s: SETUP: %s does not serve %s at %llu+%u\n", mode, landed.c_str(),
                  zeros ? "zeros" : "other bytes than the fixture's",
                  static_cast<unsigned long long>(faultAt), faultLen);
      return 2;
    }
    const XrdCl::XRootDStatus closed = plain.Close();
    (void)closed;
  }
  auto* w = new ReadWait(); // leaked with its buffer: a late completion lands safely
  auto* buf = new std::vector<char>(kLen, 0x5A);
  XrdCl::ChunkList chunks{XrdCl::ChunkInfo(kOff, kLen, buf->data())};
  const bool vec = which == "zerov" || which == "lenv" || which == "recv" || which == "blkv";
  const XrdCl::XRootDStatus s = vec                ? file->VectorRead(chunks, nullptr, w)
                                : which == "blkpg" ? file->PgRead(kOff, kLen, buf->data(), w)
                                                   : file->Read(kOff, kLen, buf->data(), w);
  const bool answered = s.IsOK() && w->wait();
  std::this_thread::sleep_for(std::chrono::seconds(1)); // room for a second completion
  std::string now;
  file->GetProperty("DataServer", now);
  {
    std::lock_guard<std::mutex> g(w->mu);
    const std::string said = w->status.ToStr();
    if (which == "zeroerr" || which == "lenerr" || which == "recerr" || which == "blkerr") {
      verdict(answered && w->status.code == XrdCl::errDataError &&
                  said.find("failed its check") != std::string::npos,
              "the read of the zeroed basket fails its check: " + said);
    } else if (which == "stay") {
      verdict(answered && w->status.IsOK() &&
                  std::memcmp(buf->data(), src.bytes.data() + kOff, kLen) == 0,
              "the bytes are read exactly (" + (answered ? said : "no answer") + ")");
      verdict(now == landed,
              "they were read where the handle landed, " + landed + " (now " + now + ")");
    } else {
      verdict(answered && w->status.IsOK() &&
                  std::memcmp(buf->data(), src.bytes.data() + kOff, kLen) == 0,
              "the zeroed basket is read exactly (" + (answered ? said : "no answer") + ")");
      verdict(now != landed, "it was read from " + now + " after " + landed);
    }
  }
  verdict(w->contractKept(), "one completion, through HandleResponseWithHosts, with a non-null HostList");
  const XrdCl::XRootDStatus cs = file->Close();
  if (!cs.IsOK())
    std::printf("INFO  close: %s\n", cs.ToString().c_str());
  verdict(openHandlersBad() == 0, "every open had exactly one completion");
  std::printf("SERVERS %s %s\n", landed.c_str(), now.c_str());
  std::printf("%s %s: %s\n", mode, which.c_str(), fails ? "FAILED" : "clean");
  return fails ? 1 : 0;
}

// read-failover cut | cutpg | cutpgpast | cutonly | end: <local-path> is a ROOT file of at least
// 64 KiB, whose header says where it ends, and <root-url> serves it from the servers of a
// federation that do not use sendfile (a copy cut under an open file then answers short, where
// sendfile would break the connection). The handle is opened (adaptive_federation on: the open
// reads the header, so the file's size is confirmed), then UCACHE_CUT_CMD, when set, is run with
// the port of the server the handle landed on and cuts that copy short. Then 64 KiB are read
// inside the file's size, past the cut:
//   cut      the read is the file's own bytes, from another server than the cut one
//   cutpg    the same with a PgRead, which the cache passes straight to the origin, from 20000, so
//            that the caller's cut falls inside it (fed_ci.sh cuts at 30000) and the copy answers
//            it with the bytes before the cut: the short answer of a page read is held against the
//            copy as a plain read's is
//   cutpgpast the same with a PgRead from 40000, wholly past the cut: the copy answers it with no
//            bytes, which the client fails itself with an invalid response (it cannot take an
//            empty page answer); inside the file's size that is held against the copy too, and
//            the read is made again at another copy (a client that took the empty answer would
//            give a short one, held against the copy as well)
//   cutonly  every copy was cut: the read fails with errDataError, and its message says the
//            answer was short inside the file's size. A second Read, at 50000, made at once
//            (within the second in which no move is made again), fails with the same error,
//            whose message says it is that of a move made for an earlier read (the offsets it
//            names are the first read's), printed as "MSG2"
//   degraded the handle opened trusting its cache entry (the caller's window), and UCACHE_CUT_CMD
//            removes the entry and makes every copy refuse the file: the handle's own open at its
//            first read fails at every copy. A Read and then a PgRead inside the file each fail,
//            once (at the call or through the handler), with the open's error, which names the
//            copies ("no copy could serve the read"); never the client's error for a File that
//            never opened, and the PgRead is never sent on that File (the client crashes there)
//   degradedoff the same with adaptive_federation off: a Read, a VectorRead, a Stat, an Fcntl, a
//            Visa and a PgRead (last) each fail, once, with the open's own error, the one the
//            reader's open would have got without the cache (here a refusal to authorize,
//            kXR_NotAuthorized), naming no copies; never the client's error for a File that never
//            opened. Then UCACHE_RESTORE_CMD, when set, makes
//            the copies readable again, and after the open's cool-down (a second) a Read and a
//            PgRead at the same place are exact: the handle's own open is made again and stands
//   ownfcntl, ownvisa the handle opened trusting its cache entry, and UCACHE_CUT_CMD puts another
//            version of the file, of another size, at every copy: an Fcntl (a Visa) before the
//            first read is answered once, and a Read at 85000, which the entry does not hold,
//            fails with the error naming the copies' other size; never the other version's bytes
//   degradedcut the handle opened trusting its cache entry, and UCACHE_CUT_CMD removes the entry
//            and cuts every copy short (its header intact): a Stat and then a Read at 80000 (past
//            the cut) each fail, once, with the error naming the copies, each refused by its
//            header's end: the handle's own open makes the checks made at open
//   degradedwhole the same with every copy whole: a Stat answers the file's size and a Read at
//            80000 is exact
//   end      nothing is cut. A Read and a PgRead that reach past the end of the file are answered
//            with the bytes that exist, short, as they always were, and the handle stays put; a
//            PgRead wholly past the end is answered as without uCache (no bytes, or the client's
//            own invalid response for that empty answer), and does not move the handle
int runCut(const char* url, const char* local, const std::string& which) {
  const char* mode = "read-failover";
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  Source src(local);
  const uint64_t size = src.bytes.size();
  if (size < 65536 + 40000) {
    std::printf("%s: SETUP: %s is shorter than 104 KB\n", mode, local);
    return 2;
  }
  std::atomic<int> fails{0}; // the leftdata case gives verdicts from two threads
  const auto verdict = [&](bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    fails += ok ? 0 : 1;
  };
  auto file = std::make_unique<XrdCl::File>(); // plugins enabled
  std::string landed;
  if (!strictOpen(*file, url) || !file->GetProperty("DataServer", landed)) {
    std::printf("%s: SETUP: open failed\n", mode);
    return 2;
  }
  if (const char* cmd = ::getenv("UCACHE_CUT_CMD"))
    if (std::system((std::string(cmd) + " " + landed.substr(landed.rfind(':') + 1)).c_str()) != 0) {
      std::printf("%s: SETUP: %s failed\n", mode, cmd);
      return 2;
    }
  std::string now = landed;
  std::vector<char> buf(65536, 0x5A);
  // A read of `n` bytes at `off`, exact, once; `what` says which.
  const auto readExact = [&](uint64_t off, uint32_t n, const std::string& what) {
    auto* w = new ReadWait(); // leaked with its buffer: a late completion lands safely
    auto* b = new std::vector<char>(n, 0x5A);
    const XrdCl::XRootDStatus s = file->Read(off, n, b->data(), w);
    const bool answered = s.IsOK() && w->wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // room for a second completion
    const bool once = w->contractKept();
    std::lock_guard<std::mutex> g(w->mu);
    const bool ok = answered && once && w->status.IsOK() &&
                    std::memcmp(b->data(), src.bytes.data() + off, n) == 0;
    verdict(ok, what + (ok ? "" : " (" + (s.IsOK() ? w->status.ToStr() : s.ToString()) + ")"));
  };
  const auto server = [&] {
    std::string v;
    return file->GetProperty("DataServer", v) ? v : std::string("(none)");
  };
  if (which == "leftdata") {
    // UCACHE_TEST_READ_FAIL_N=3, _KIND=leftdata; the file on two servers; the copy X the handle
    // opened at cut after the open (UCACHE_CUT_CMD). Two Reads, issued 400 ms apart, both meet X:
    // the first's miss fails (a connection error, 1 s late) and moves the handle to Y; the second's
    // (past the cut) is answered short by X, 2.5 s late, after that move, on the File the handle
    // left: X is then a copy the handle never opens again. Both are exact (the second is issued
    // again at Y). Then UCACHE_RESTORE_CMD (with X's port) makes X whole again, so that only its
    // mark can keep a move from it. A third Read fails at Y (a connection error): the move it makes
    // stays at Y (an X marked only as a failure is taken again, as the oldest failure).
    std::thread t1([&] {
      readExact(8192, 4096, "a Read whose miss fails at X (a connection error) is exact");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    std::thread t2([&] {
      readExact(40000, 4096, "a Read past the cut, answered short by X after the move, is exact");
    });
    t1.join();
    t2.join();
    const std::string y = server();
    verdict(y != landed, "after the two Reads the handle reads from " + y + ", was " + landed);
    if (const char* cmd = ::getenv("UCACHE_RESTORE_CMD"))
      if (std::system((std::string(cmd) + " " + landed.substr(landed.rfind(':') + 1)).c_str()) !=
          0) {
        std::printf("%s: SETUP: %s failed\n", mode, cmd);
        return 2;
      }
    readExact(20000, 4096, "a third Read, whose miss fails at Y, is exact");
    now = server();
    verdict(now == y, "after the third Read the handle still reads from " + now +
                          " (X, whose read answered short, is not taken again though whole)");
    std::printf("LEFTDATA %s %s %s\n", landed.c_str(), y.c_str(), now.c_str());
  } else if (which == "degraded" || which == "degradedoff") { // each request gets the open's error
    const bool on = which == "degraded";
    // r Read, v VectorRead, s Stat, f Fcntl, z Visa, p PgRead (last: on a File that never opened it
    // crashes the client)
    const std::string kinds = on ? "rp" : "rvsfzp";
    for (const char k : kinds) {
      const char* name = k == 'r'   ? "read"
                         : k == 'v' ? "vectorread"
                         : k == 's' ? "stat"
                         : k == 'f' ? "fcntl"
                         : k == 'z' ? "visa"
                                    : "pgread";
      auto* w = new ReadWait(); // leaked with its buffer: a late completion lands safely
      auto* b = new std::vector<char>(65536, 0x5A);
      XrdCl::XRootDStatus s;
      switch (k) {
      case 'r':
        s = file->Read(40000, 65536, b->data(), w);
        break;
      case 'v':
        s = file->VectorRead(XrdCl::ChunkList{XrdCl::ChunkInfo(40000, 4096, b->data()),
                                              XrdCl::ChunkInfo(50000, 4096, b->data() + 4096)},
                             nullptr, w);
        break;
      case 's':
        s = file->Stat(false, w);
        break;
      case 'f':
        s = file->Fcntl(XrdCl::Buffer(), w);
        break;
      case 'z':
        s = file->Visa(w);
        break;
      default:
        s = file->PgRead(40000, 65536, b->data(), w);
        break;
      }
      XrdCl::XRootDStatus got = s;
      bool once = true;
      if (s.IsOK()) {
        const bool answered = w->wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // room for a second completion
        std::lock_guard<std::mutex> g(w->mu);
        got =
            answered ? w->status : XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOperationExpired);
        once = answered;
      }
      const std::string msg = got.GetErrorMessage();
      std::printf("MSG %s %s\n", name, msg.c_str());
      const char* what = k == 'r'   ? "a Read"
                         : k == 'v' ? "a VectorRead"
                         : k == 's' ? "a Stat"
                         : k == 'f' ? "an Fcntl"
                         : k == 'z' ? "a Visa"
                                    : "a PgRead";
      if (on)
        verdict(!got.IsOK() && got.code != XrdCl::errInvalidOp &&
                    msg.find("no copy could serve the read") != std::string::npos,
                std::string(what) +
                    " fails with the open's error, which names "
                    "the copies (" +
                    got.ToStr() + (s.IsOK() ? ", through the handler" : ", at the call") + ")");
      else
        verdict(!got.IsOK() && got.code == XrdCl::errErrorResponse &&
                    got.errNo == 3010 && // kXR_NotAuthorized
                    msg.find("no copy could serve the read") == std::string::npos,
                std::string(what) + " fails with the open's own error, the server's refusal (" +
                    got.ToStr() +
                    (s.IsOK() ? ", through the handler" : ", at the call") + ")");
      verdict(once && (!s.IsOK() || w->contractKept()),
              std::string("it is answered once (") + what + ")");
    }
    if (!on)
      if (const char* cmd = ::getenv("UCACHE_RESTORE_CMD")) {
        if (std::system(cmd) != 0) {
          std::printf("%s: SETUP: %s failed\n", mode, cmd);
          return 2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // past the open's cool-down
        readExact(40000, 65536, "after the copies are readable again, a Read is exact");
        uint32_t got = 0;
        std::vector<uint32_t> cksums;
        const XrdCl::XRootDStatus ps = file->PgRead(40000, 65536, buf.data(), cksums, got);
        verdict(ps.IsOK() && got == 65536 &&
                    std::memcmp(buf.data(), src.bytes.data() + 40000, 65536) == 0,
                "and a PgRead (" + ps.ToString() + ", " + std::to_string(got) + " bytes)");
      }
  } else if (which == "ownfcntl" || which == "ownvisa") {
    // Trusted at Open (no request at open); UCACHE_CUT_CMD put another version of the file, of
    // another size and with other bytes at 85000 (where no basket begins), at every copy. An Fcntl
    // (or a Visa) before the handle's first read needs the cache's own open of the origin, which
    // must find the size the entry holds: every copy is refused for its size, and a Read at 85000,
    // which the entry does not hold, then fails with that error; never the other version's bytes.
    const bool fc = which == "ownfcntl";
    const char* what = fc ? "an Fcntl" : "a Visa";
    auto* w = new ReadWait(); // leaked: a late completion lands safely
    const XrdCl::XRootDStatus s = fc ? file->Fcntl(XrdCl::Buffer(), w) : file->Visa(w);
    XrdCl::XRootDStatus got = s;
    bool once = true;
    if (s.IsOK()) {
      const bool answered = w->wait();
      std::this_thread::sleep_for(std::chrono::milliseconds(300)); // room for a second completion
      once = answered && w->contractKept();
      std::lock_guard<std::mutex> g(w->mu);
      got = answered ? w->status : XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOperationExpired);
    }
    std::printf("MSG %s %s\n", fc ? "fcntl" : "visa", got.ToString().c_str());
    verdict(once, std::string(what) + " before the first read is answered once");
    auto* rw = new ReadWait();
    auto* b = new std::vector<char>(4096, 0x5A);
    const XrdCl::XRootDStatus rs = file->Read(85000, 4096, b->data(), rw);
    XrdCl::XRootDStatus rgot = rs;
    bool ronce = true;
    if (rs.IsOK()) {
      const bool answered = rw->wait();
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      ronce = answered && rw->contractKept();
      std::lock_guard<std::mutex> g(rw->mu);
      rgot = answered ? rw->status
                      : XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOperationExpired);
    }
    const bool same = rgot.IsOK() && std::memcmp(b->data(), src.bytes.data() + 85000, 4096) == 0;
    const std::string m = rgot.GetErrorMessage();
    std::printf("OWNREAD %s %s\n", !rgot.IsOK() ? "error" : same ? "exact" : "other",
                rgot.ToString().c_str());
    verdict(ronce && !rgot.IsOK() && rgot.code == XrdCl::errDataError &&
                m.find("the cached file has " + std::to_string(size)) != std::string::npos,
            "then a Read at 85000, which the entry does not hold, fails with the error that names "
            "the copies' other size, never the other version's bytes (" +
                rgot.ToString().substr(0, 200) + ")");
  } else if (which == "degradedcut") {
    // Trusted at Open; UCACHE_CUT_CMD removed the entry and cut every copy short (its header
    // intact, so it states the whole file's end). The handle's own open at its first request
    // makes the checks made at open, as the reader's open does, and refuses every copy: a Stat
    // and a Read past the cut each fail with the error that names the copies and the end their
    // header states; never the cut copy's size, nor an empty answer past it.
    auto* w = new ReadWait();
    XrdCl::XRootDStatus s = file->Stat(false, w);
    XrdCl::XRootDStatus got = s;
    bool once = true;
    uint64_t shown = 0;
    if (s.IsOK()) {
      const bool answered = w->wait();
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      once = answered && w->contractKept();
      std::lock_guard<std::mutex> g(w->mu);
      got = answered ? w->status : XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOperationExpired);
    }
    if (got.IsOK()) { // what size the Stat showed, from a synchronous Stat (answered the same way)
      XrdCl::StatInfo* si = nullptr;
      if (file->Stat(false, si, 30).IsOK() && si)
        shown = si->GetSize();
      delete si;
    }
    const std::string sm = got.GetErrorMessage();
    std::printf("MSG stat %s%s\n", got.ToString().c_str(),
                got.IsOK() ? (" size " + std::to_string(shown)).c_str() : "");
    verdict(once && !got.IsOK() && sm.find("no copy could serve the read") != std::string::npos &&
                sm.find("its header says the file is " + std::to_string(size)) != std::string::npos,
            "a Stat fails with the error that names the copies, each refused by its header's "
            "end (" +
                got.ToString().substr(0, 200) + ")");
    auto* rw = new ReadWait();
    auto* b = new std::vector<char>(4096, 0x5A);
    s = file->Read(80000, 4096, b->data(), rw);
    got = s;
    once = true;
    if (s.IsOK()) {
      const bool answered = rw->wait();
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      once = answered && rw->contractKept();
      std::lock_guard<std::mutex> g(rw->mu);
      got = answered ? rw->status : XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOperationExpired);
    }
    std::printf("MSG read %s\n", got.ToString().c_str());
    verdict(once && !got.IsOK() && got.GetErrorMessage().find("no copy could serve the read") !=
                                       std::string::npos,
            "a Read at 80000, past the cut, fails with that error (" +
                got.ToString().substr(0, 200) + ")");
  } else if (which == "degradedwhole") {
    // Trusted at Open; UCACHE_CUT_CMD removed the entry, and every copy is whole: the handle's own
    // open makes the checks made at open and takes the copy it lands on. A Stat answers the file's
    // size and a Read at 80000 is exact.
    XrdCl::StatInfo* si = nullptr;
    const XrdCl::XRootDStatus ss = file->Stat(false, si, 60);
    const uint64_t shown = si ? si->GetSize() : 0;
    delete si;
    verdict(ss.IsOK() && shown == size,
            "a Stat answers the file's size (" + ss.ToString() + ", " + std::to_string(shown) +
                ")");
    readExact(80000, 4096, "a Read at 80000 is exact");
  } else if (which == "end") { // past the end: short, exact, no move
    uint32_t got = 0;
    XrdCl::XRootDStatus s = file->Read(size - 1000, 65536, buf.data(), got);
    verdict(s.IsOK() && got == 1000 && std::memcmp(buf.data(), src.bytes.data() + size - 1000, 1000) == 0,
            "a Read across the end of the file is answered with the 1000 bytes there are (" +
                s.ToString() + ", " + std::to_string(got) + " bytes)");
    std::vector<uint32_t> cksums;
    s = file->PgRead(size - 1000, 65536, buf.data(), cksums, got);
    verdict(s.IsOK() && got == 1000 && std::memcmp(buf.data(), src.bytes.data() + size - 1000, 1000) == 0,
            "a PgRead across the end is answered with the 1000 bytes there are (" + s.ToString() +
                ", " + std::to_string(got) + " bytes)");
    s = file->Read(size + 5000, 65536, buf.data(), got);
    verdict(s.IsOK() && got == 0, "a Read past the end is answered with no bytes (" + s.ToString() + ")");
    file->GetProperty("DataServer", now);
    verdict(now == landed, "the handle is still read from " + landed + " (now " + now + ")");
    // Last: a client that fails an empty page answer leaves its File unusable.
    s = file->PgRead(size + 5000, 65536, buf.data(), cksums, got);
    verdict((s.IsOK() && got == 0) || s.code == XrdCl::errInvalidResponse,
            "a PgRead past the end is answered as without uCache: no bytes, or the client's "
            "invalid response (" + s.ToString() + ")");
    std::this_thread::sleep_for(std::chrono::seconds(1)); // room for a move, were one made
    file->GetProperty("DataServer", now);
    verdict(now == landed,
            "the handle is still read from " + landed + " after it (now " + now + ")");
  } else {
    auto* w = new ReadWait(); // leaked with its buffer: a late completion lands safely
    const bool pg = which == "cutpg" || which == "cutpgpast";
    const uint64_t off = which == "cutpg" ? 20000 : 40000;
    const XrdCl::XRootDStatus s =
        pg ? file->PgRead(off, 65536, buf.data(), w) : file->Read(off, 65536, buf.data(), w);
    const bool answered = s.IsOK() && w->wait();
    // cutonly: a second read at once, inside the second after the first's move found no copy
    auto* w2 = new ReadWait(); // leaked with its buffer: a late completion lands safely
    auto* b2 = new std::vector<char>(65536, 0x5A);
    const XrdCl::XRootDStatus s2 =
        which == "cutonly" ? file->Read(50000, 65536, b2->data(), w2) : XrdCl::XRootDStatus();
    std::this_thread::sleep_for(std::chrono::seconds(1)); // room for a second completion
    file->GetProperty("DataServer", now);
    {
      std::lock_guard<std::mutex> g(w->mu);
      const std::string said = w->status.ToStr();
      if (which == "cutonly") {
        verdict(answered && w->status.code == XrdCl::errDataError &&
                    w->status.GetErrorMessage().find("inside the file's size of") != std::string::npos,
                "the read inside the file's size past the cut fails with the data error: " + said +
                    " " + w->status.GetErrorMessage());
        const bool answered2 = s2.IsOK() && w2->wait();
        std::lock_guard<std::mutex> g2(w2->mu);
        const std::string m2 = w2->status.GetErrorMessage();
        std::printf("MSG2 %s\n", m2.c_str());
        verdict(answered2 && w2->status.code == XrdCl::errDataError &&
                    m2.find("for an earlier read of the handle") != std::string::npos,
                "a second read at once fails with the error of the move made for the first, which "
                "says so: " +
                    (answered2 ? w2->status.ToStr() : s2.ToString()));
      } else {
        verdict(answered && w->status.IsOK() && std::memcmp(buf.data(), src.bytes.data() + off, 65536) == 0,
                "the read past the cut is the file's own bytes (" + (answered ? said : "no answer") + ")");
        verdict(now != landed, "it was read from " + now + " after " + landed);
      }
    }
    verdict(w->contractKept(), "one completion, through HandleResponseWithHosts, with a non-null HostList");
  }
  const XrdCl::XRootDStatus cs = file->Close();
  if (!cs.IsOK())
    std::printf("INFO  close: %s\n", cs.ToString().c_str());
  verdict(openHandlersBad() == 0, "every open had exactly one completion");
  std::printf("SERVERS %s %s\n", landed.c_str(), now.c_str());
  std::printf("%s %s: %s\n", mode, which.c_str(), fails ? "FAILED" : "clean");
  return fails ? 1 : 0;
}

int runReadFailover(const char* url, const char* local, const std::string& which) {
  const char* mode = "read-failover";
  if (which.rfind("zero", 0) == 0 || which.rfind("len", 0) == 0 || which.rfind("rec", 0) == 0 ||
      which.rfind("blk", 0) == 0 || which == "stay")
    return runZero(url, local, which);
  if (which == "cut" || which == "cutpg" || which == "cutpgpast" || which == "cutonly" ||
      which == "end" || which == "degraded" || which == "degradedoff" || which == "leftdata" ||
      which == "ownfcntl" || which == "ownvisa" || which == "degradedcut" ||
      which == "degradedwhole")
    return runCut(url, local, which);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  Source src(local);
  if (src.bytes.size() < 8 * 1024 * 1024) {
    std::printf("%s: SETUP: %s is shorter than 8 MiB\n", mode, local);
    return 2;
  }
  int fails = 0;
  std::vector<ReadWait*> handlers;
  auto file = std::make_unique<XrdCl::File>(); // plugins enabled
  if (!strictOpen(*file, url)) {
    std::printf("%s: SETUP: open failed\n", mode);
    return 2;
  }
  ReadCase rc{src, file.get(), handlers, fails};
  std::vector<std::string> servers{rc.prop("DataServer")};
  const auto moved = [&](const char* what) {
    servers.push_back(rc.prop("DataServer"));
    rc.verdict(servers.back() != servers[servers.size() - 2], std::string(what) + ": read from " +
                                                                  servers.back() + " now, was " +
                                                                  servers[servers.size() - 2]);
  };
  const auto stayed = [&](const char* what) {
    servers.push_back(rc.prop("DataServer"));
    rc.verdict(servers.back() == servers[servers.size() - 2],
               std::string(what) + ": still read from " + servers.back());
  };
  constexpr uint64_t M = 1 << 20;
  if (which == "miss") {
    rc.read('r', 1 * M, "a Read whose origin read failed is exact");
    moved("after the Read");
    rc.verdict(rc.prop("ReadRecovery") == "false",
               "ReadRecovery is false: " + rc.prop("ReadRecovery"));
    rc.read('v', 2 * M, "a VectorRead whose origin read failed is exact");
    moved("after the VectorRead");
    rc.read('r', 3 * M, "a Read after the moves is exact");
    stayed("after it");
    const bool set = file->SetProperty("ReadRecovery", "true");
    rc.verdict(set && rc.prop("ReadRecovery") == "false",
               "SetProperty(ReadRecovery, true) is accepted and ReadRecovery stays false: " +
                   rc.prop("ReadRecovery"));
    rc.read('r', 3 * M + 262144, "a Read after it is exact");
  } else if (which == "relay") {
    rc.read('r', 1 * M, "(first, to arm the handle) a Read is exact");
    rc.read('p', 2 * M, "a PgRead that failed is exact");
    moved("after the PgRead");
    rc.read('b', 3 * M, "a VectorRead into one buffer that failed is exact");
    moved("after the VectorRead");
  } else if (which == "refuse") {
    // A refusal at once says nothing of the server (a session lost on a healthy one), so the
    // move may land on it again: that it moved is for the caller to check in the records.
    rc.read('r', 1 * M, "a Read the client refused at once is exact");
    servers.push_back(rc.prop("DataServer"));
    rc.read('p', 2 * M, "a PgRead the client refused is exact");
    rc.read('v', 3 * M, "a VectorRead the client refused is exact");
  } else if (which == "final" || which == "off") {
    rc.read('r', 1 * M, "a Read whose origin read failed is exact");
    stayed("after the Read");
    rc.read('v', 2 * M, "a VectorRead whose origin read failed is exact");
    stayed("after the VectorRead");
    if (which == "off")
      rc.verdict(rc.prop("ReadRecovery") == "true",
                 "ReadRecovery is true: " + rc.prop("ReadRecovery"));
  } else if (which == "many") {
    rc.read('r', 0, "(first, to arm the handle) a Read is exact");
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
      ts.emplace_back([&, t] {
        for (int k = 0; k < 4; ++k) // page-distinct: no two of them share a fetch
          rc.read('r', (2 + t * 4 + k) * 131072ull, "a concurrent Read is exact");
      });
    for (auto& t : ts)
      t.join();
    moved("after the 32 reads");
  } else if (which == "exhaust") {
    rc.read('r', 1 * M, "(first, to arm the handle) a Read is exact");
    auto* w = new ReadWait(); // leaked with its buffer: a late completion lands safely
    handlers.push_back(w);
    auto* buf = new std::vector<char>(65536, 0x5A);
    const XrdCl::XRootDStatus s = file->PgRead(2 * M, 65536, buf->data(), w);
    const bool answered = s.IsOK() && w->wait();
    {
      std::lock_guard<std::mutex> g(w->mu);
      const std::string msg = w->status.GetErrorMessage();
      std::printf("MSG %s\n", msg.c_str());
      rc.verdict(answered && w->status.code == XrdCl::errConnectionError &&
                     msg.find("no copy could serve the read") != std::string::npos,
                 "a PgRead that failed at every copy it was moved to fails with its last "
                 "failure's code (" +
                     (answered ? w->status.ToStr() : s.ToString()) +
                     ") and the error that names the copies");
    }
    servers.push_back(rc.prop("DataServer"));
    rc.read('r', 3 * M, "a Read after it is exact");
  } else {
    std::printf("%s: unknown case %s\n", mode, which.c_str());
    return 2;
  }
  const XrdCl::XRootDStatus cs = file->Close();
  if (!cs.IsOK())
    std::printf("INFO  close: %s\n", cs.ToString().c_str());
  std::this_thread::sleep_for(std::chrono::seconds(1)); // room for a late second completion
  bool kept = true;
  for (auto* w : handlers)
    kept = kept && w->contractKept();
  rc.verdict(kept, "every read had exactly one completion, through HandleResponseWithHosts, "
                   "with a non-null HostList");
  rc.verdict(openHandlersBad() == 0, "every open had exactly one completion");
  std::string all;
  for (const auto& sv : servers)
    all += (all.empty() ? "" : " ") + sv;
  std::printf("SERVERS %s\n", all.c_str());
  std::printf("%s %s: %s\n", mode, which.c_str(), fails ? "FAILED" : "clean");
  return fails ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <root-url> <local-path> [ops] [threads] [seed] "
                 "[fuzz|inner-race|inner-race-relay|open-refused|open-property|open-property-race|"
                 "open-stat-during-checks [cut]|open-park-from-callback|"
                 "open-failover|open-denied|"
                 "key-check <off> <len>|read-failover <case>]\n",
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
  if (mode == "open-property-race")
    return runOpenPropertyRace(url);
  if (mode == "open-stat-during-checks")
    return runOpenStatDuringChecks(url, argv[2], argc > 7 && std::string(argv[7]) == "cut");
  if (mode == "open-park-from-callback")
    return runOpenParkFromCallback(url, argv[2]);
  if (mode == "open-failover")
    return runOpenFailover(url, argv[2], argc > 3 ? ::atoi(argv[3]) : 12);
  if (mode == "open-denied")
    return runOpenDenied(url);
  if (mode == "read-failover")
    return argc > 7 ? runReadFailover(url, argv[2], argv[7]) : 2;
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
