#include "UCacheFile.h"
#ifdef UCACHE_HAVE_PREFETCH
#include "Prefetch.h"
#endif
#ifdef UCACHE_HAVE_COLDRUN
#include "ColdRun.h"
#endif

#include "CopyDetect.h"
#include "Executor.h"
#include "Log.h"
#include "OpenPolicy.h"
#include "OpenRetry.h"
#ifdef UCACHE_HAVE_COLDRUN
#include "OriginSource.h"
#include "ReadRule.h"
#endif
#include "OriginInFlight.h"
#include "PluginSupport.h"
#include "ReaderWait.h"
#include "ReadRounding.h"
#include "Trace.h"
#include "vendor/crc32c.h"

#include <functional>
#include <map>
#include <pthread.h>
#include <XProtocol/XProtocol.hh> // kXR_* wire codes for the OpenRetry drift asserts
#include <XrdCl/XrdClConstants.hh>
#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClFileSystem.hh>

#include <cstdio>
#include <optional>
#include <sys/resource.h>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <algorithm>
#include <new>
#include <thread>
#include <unordered_set>

namespace ucache {

using XrdCl::AnyObject;
using XrdCl::ChunkInfo;
using XrdCl::ChunkList;
using XrdCl::HostList;
using XrdCl::ResponseHandler;
using XrdCl::XRootDStatus;

// OpenRetry.cc hardcodes these XRootD 5.8.3 ABI/wire values to stay XrdCl-free
// (the raw-errNo comparison is deliberate); assert they
// still match the headers so ABI drift breaks the build, not the field.
static_assert(XrdCl::errConnectionError == 108 && XrdCl::errErrorResponse == 400,
              "XrdCl status codes drifted from OpenRetry.cc");
static_assert(kXR_ItExists == 3018 && kXR_Overloaded == 3024,
              "kXR wire codes drifted from OpenRetry.cc");
// The same for OpenPolicy.h's failure classes.
static_assert(XrdCl::errInternal == xrdwire::kErrInternal &&
                  XrdCl::errDataError == xrdwire::kErrDataError &&
                  XrdCl::errSocketError == xrdwire::kErrSocketError &&
                  XrdCl::errSocketTimeout == xrdwire::kErrSocketTimeout &&
                  XrdCl::errSocketDisconnected == xrdwire::kErrSocketDisconnected &&
                  XrdCl::errPollerError == xrdwire::kErrPollerError &&
                  XrdCl::errSocketOptError == xrdwire::kErrSocketOptError &&
                  XrdCl::errOperationInterrupted == xrdwire::kErrOperationInterrupted &&
                  XrdCl::errStreamDisconnect == xrdwire::kErrStreamDisconnect &&
                  XrdCl::errConnectionError == xrdwire::kErrConnectionError &&
                  XrdCl::errInvalidSession == xrdwire::kErrInvalidSession &&
                  XrdCl::errTlsError == xrdwire::kErrTlsError &&
                  XrdCl::errAuthFailed == xrdwire::kErrAuthFailed &&
                  XrdCl::errOperationExpired == xrdwire::kErrOperationExpired &&
                  XrdCl::errNotFound == xrdwire::kErrNotFound &&
                  XrdCl::errRedirectLimit == xrdwire::kErrRedirectLimit &&
                  XrdCl::errErrorResponse == xrdwire::kErrErrorResponse,
              "XrdCl status codes drifted from OpenPolicy.h");
static_assert(kXR_FileLocked == xrdwire::kXrFileLocked && kXR_FSError == xrdwire::kXrFSError &&
                  kXR_FileNotOpen == xrdwire::kXrFileNotOpen &&
                  kXR_NoMemory == xrdwire::kXrNoMemory &&
                  kXR_ReqTimedOut == xrdwire::kXrReqTimedOut &&
                  kXR_IOError == xrdwire::kXrIOError &&
                  kXR_NotAuthorized == xrdwire::kXrNotAuthorized &&
                  kXR_NotFound == xrdwire::kXrNotFound &&
                  kXR_ServerError == xrdwire::kXrServerError &&
                  kXR_noserver == xrdwire::kXrNoServer && kXR_Cancelled == xrdwire::kXrCancelled &&
                  kXR_ItExists == xrdwire::kXrItExists && kXR_Overloaded == xrdwire::kXrOverloaded,
              "kXR wire codes drifted from OpenPolicy.h");
static_assert(kXR_isServer == xrdwire::kIsServer && kXR_isManager == xrdwire::kIsManager &&
                  kXR_attrMeta == xrdwire::kAttrMeta && kXR_offline == xrdwire::kStatOffline &&
                  XrdCl::StatInfo::Offline == xrdwire::kStatOffline,
              "XRootD login or stat flags drifted from OpenPolicy.h");
// A silent connection must be found by the client's own check before a read's limit.
static_assert(XrdCl::DefaultStreamTimeout + XrdCl::DefaultTimeoutResolution < kReadDeadlineS,
              "the client's stream timeout and its resolution must end before a read's limit");

// Test-only wire-read fault (PluginSupport.h): the origin dying mid-stream
// after a healthy open, so the fail-open contract -- the reader still gets the
// exact bytes -- can be tested deterministically. The relay pass-through and
// every other inner operation are untouched. Sibling of UCACHE_TEST_OPEN_FAIL_N.
bool readFaultFire() {
  static std::atomic<int> budget{[] {
    const char* v = ::getenv("UCACHE_TEST_READ_FAIL_N");
    return v ? ::atoi(v) : 0;
  }()};
  if (budget.load(std::memory_order_relaxed) <= 0)
    return false; // production fast path: one relaxed load
  return budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}

// UCACHE_TEST_READ_FAIL_KIND (inert unless set, read at load) says where the
// faults of UCACHE_TEST_READ_FAIL_N strike and how. Unset: the issues of the
// byte cache's misses, each failing with a connection error posted to its
// handler, as if the client had answered. relay: the issues of relayed reads
// instead. refuse: both, as an invalid-session error the issue call itself
// returns, which nothing was sent for. final: the misses, with an error no
// other site would answer differently. late: the misses, the error posted 500 ms
// late (after the open of a move, on the loopback).
enum class FaultKind : uint8_t { kMiss, kRelay, kRefuse, kFinal, kLate };
static const FaultKind gFaultKind = [] {
  const char* v = ::getenv("UCACHE_TEST_READ_FAIL_KIND");
  const std::string k = v ? v : "";
  return k == "relay"    ? FaultKind::kRelay
         : k == "refuse" ? FaultKind::kRefuse
         : k == "final"  ? FaultKind::kFinal
         : k == "late"   ? FaultKind::kLate
                         : FaultKind::kMiss;
}();
// True when the fault fires at this issue site: `s` is then what the issue call
// returns (OK when the failure is posted to `h`, which gets exactly that).
static bool injectReadFault(bool relaySite, ResponseHandler* h, XRootDStatus& s) {
  const FaultKind k = gFaultKind;
  const bool here =
      relaySite ? k == FaultKind::kRelay || k == FaultKind::kRefuse : k != FaultKind::kRelay;
  if (!here || !readFaultFire())
    return false;
  s = XRootDStatus();
  if (k == FaultKind::kRefuse) {
    s = XRootDStatus(XrdCl::stError, XrdCl::errInvalidSession);
    return true;
  }
  const XRootDStatus err(XrdCl::stError, k == FaultKind::kFinal ? XrdCl::errInvalidResponse
                                                                : XrdCl::errConnectionError);
  Executor::instance().postAfter(k == FaultKind::kLate ? 500 : 0, [h, err] {
    h->HandleResponseWithHosts(new XRootDStatus(err), nullptr, new HostList());
  });
  return true;
}

namespace {
// The reader's handler, wrapped by the read's entry point: the reading
// thread's wait (ReaderWait.h) is closed just before the reader is answered,
// charged to the costliest tier the request needed. Forwards exactly the one
// completion it receives, through the same call, then deletes itself.
class WaitHandler : public ResponseHandler {
 public:
  WaitHandler(ResponseHandler* real, std::shared_ptr<ReaderWait::Thread> t, Stats* s)
      : real_(real), t_(std::move(t)), stats_(s) {
    ReaderWait::begin(*t_, nowUs());
  }
  void note(uint8_t tier) {
    uint8_t cur = tier_.load(std::memory_order_relaxed);
    while (cur < tier && !tier_.compare_exchange_weak(cur, tier, std::memory_order_relaxed)) {
    }
  }
  void HandleResponseWithHosts(XRootDStatus* status, AnyObject* response,
                               HostList* hostList) override {
    ResponseHandler* real = finish(false);
    real->HandleResponseWithHosts(status, response, hostList);
  }
  void HandleResponse(XRootDStatus* status, AnyObject* response) override {
    ResponseHandler* real = finish(false);
    real->HandleResponse(status, response);
  }
  // The read was refused at once: no answer will come through this handler.
  void cancel() { finish(true); }

 private:
  ResponseHandler* finish(bool refused) {
    ReaderWait::Tier tier = ReaderWait::kByte;
    const uint64_t now = nowUs();
    const uint64_t us =
        refused ? ReaderWait::cancel(*t_, now, tier)
                : ReaderWait::end(*t_, now,
                                  static_cast<ReaderWait::Tier>(tier_.load(std::memory_order_relaxed)),
                                  tier);
    if (us)
      stats_->readerWaitUs[tier].fetch_add(us, std::memory_order_relaxed);
    ResponseHandler* real = real_;
    delete this;
    return real;
  }
  ResponseHandler* real_;
  std::shared_ptr<ReaderWait::Thread> t_;
  Stats* stats_;
  std::atomic<uint8_t> tier_{ReaderWait::kByte};
};

// Serve one read with the reader's handler wrapped, when this handle caches
// (a pass-through handle, and so a switched-off run, is left as it is).
template <class Serve>
XRootDStatus waited(const std::shared_ptr<HandleState>& st, bool passthrough,
                    ResponseHandler* handler, Serve serve) {
  if (passthrough || !st->store || !handler)
    return serve(handler);
  bool created = false;
  auto t = ReaderWait::current(Executor::forkGeneration(), created);
  Stats& stats = st->store->stats();
  if (created)
    stats.readerThreads.fetch_add(1, std::memory_order_relaxed);
  auto* w = new WaitHandler(handler, std::move(t), &stats);
  XRootDStatus s = serve(w);
  if (!s.IsOK())
    w->cancel();
  return s;
}
} // namespace

void noteWaitTier(ResponseHandler* h, uint8_t tier) {
  if (auto* w = dynamic_cast<WaitHandler*>(h))
    w->note(tier);
}

namespace {

// ---- Test-only open fault injection (inert unless armed) -------------------
// UCACHE_TEST_OPEN_FAIL_N=N rewrites the target port of a handle's first N inner
// open ATTEMPTS to port 1 (privileged, never bound), so the real open fails
// with errConnectionError and the inner XrdCl::File lands in the terminal Error
// state — the exact condition the fresh-file retry must survive. Ships
// compiled-in and inert (deterministic CI beats a build-flag matrix); the only
// test-only hook in shipped plugin code (a deliberate precedent).
int openFaultFirstN() {
  static const int n = [] {
    const char* v = ::getenv("UCACHE_TEST_OPEN_FAIL_N");
    return v ? ::atoi(v) : 0;
  }();
  return n;
}
std::string rewritePortTo1(const std::string& url) {
  auto sch = url.find("://");
  if (sch == std::string::npos)
    return url;
  size_t authStart = sch + 3;
  size_t authEnd = url.find('/', authStart);
  if (authEnd == std::string::npos)
    authEnd = url.size();
  std::string auth = url.substr(authStart, authEnd - authStart);
  auto colon = auth.rfind(':');
  std::string newAuth =
      (colon != std::string::npos) ? auth.substr(0, colon) + ":1" : auth + ":1";
  return url.substr(0, authStart) + newAuth + url.substr(authEnd);
}
// attempt is 1-based; returns url unchanged unless the fault is armed and this
// attempt is within the first N.
std::string faultOpenUrl(const std::string& url, int attempt) {
  int n = openFaultFirstN();
  return (n > 0 && attempt <= n) ? rewritePortTo1(url) : url;
}

// UCACHE_TEST_OPEN_DEADLINE_S replaces the open deadline, for tests that
// cannot wait a minute. Read once, when the library loads: a value set then
// has no first-use guard that a fork could catch half taken, and the cache's
// opens in a forked child read it too.
const int gTestOpenDeadlineS = [] {
  const char* v = ::getenv("UCACHE_TEST_OPEN_DEADLINE_S");
  return v ? ::atoi(v) : 0;
}();
// UCACHE_TEST_READ_DEADLINE_S replaces the deadline of an origin read of an
// armed handle (OpenPolicy.h), read at load for the same reason.
const int gReadDeadlineS = readDeadlineS(::getenv("UCACHE_TEST_READ_DEADLINE_S"));

// The per-request timeout of a read open of a cached handle (OpenPolicy.h).
// The deadline is part of adaptive_federation: with the switch off, the
// reader's own open passes the reader's own timeout unchanged (0 = the
// client's RequestTimeout), and the cache's own opens pass none (0), as they
// did before there was a deadline (openAndWait). Without the switch there is no other
// site to go on to, and a deadline would cut off the client's own recovery
// from a server that never accepts the connection (at a redirector, it goes on
// to another data server only before the request expires), so a job that
// works uncached could fail. With the switch on, a reader that gives none (0)
// waits, without the cache, for the client's RequestTimeout, so that is the
// reader's own timeout the rule keeps when it is the shorter: no open waits
// longer than it would uncached.
ucache::XrdTimeout openDeadline(int readerTimeoutS) {
  if (!globalConfig().adaptiveFederation)
    return static_cast<ucache::XrdTimeout>(readerTimeoutS);
  const int own = readerTimeoutS > 0 ? readerTimeoutS : defaultRequestTimeoutS();
  return static_cast<ucache::XrdTimeout>(openDeadlineS(own, gTestOpenDeadlineS));
}

// ---- Opening a file at another site (adaptive_federation on) -------------
// UCACHE_TEST_FAILOVER_MAX_S and UCACHE_TEST_FAILOVER_MAX_SITES replace the
// time and site bounds of a sequence of attempts, for tests; read at load,
// like the deadline's.
int envAtLoad(const char* name) {
  const char* v = ::getenv(name);
  return v ? ::atoi(v) : 0;
}
const int gTestFailoverMaxS = envAtLoad("UCACHE_TEST_FAILOVER_MAX_S");
const int gTestFailoverMaxSites = envAtLoad("UCACHE_TEST_FAILOVER_MAX_SITES");
int failoverMaxSites() {
  return gTestFailoverMaxSites > 0 ? gTestFailoverMaxSites : kFailoverMaxSites;
}
// The pool for the work that waits for another site: the moves of a handle's reads
// and the re-opens of a reader's open. A stalled site holds one of its threads for as
// long as the attempts last (up to kFailoverMaxS), so that work is not given to the
// pool that serves cache hits and persists (Executor::instance). Built at first use, as
// that one is: a fork finds no thread.
Executor& moveExecutor() {
  static Executor* exec = new Executor(4);
  return *exec;
}
// The pool that gives a read its answer once its move is over: the read issued again, or the
// failure handed to the reader's handler, which may block. Apart from the moves, so that an
// answer never waits behind a move that is stalled.
Executor& deliverExecutor() {
  static Executor* exec = new Executor(2);
  return *exec;
}
// UCACHE_TEST_OFFLINE_HOSTS=host:port,... (inert unless set): the stat an open
// at one of those servers answers carries the Offline flag, for tests of the
// copy-on-tape step without a tape system. Read at load; kept for the life of
// the process, since detached threads read it.
const std::string* const gTestOfflineHosts = [] {
  const char* v = ::getenv("UCACHE_TEST_OFFLINE_HOSTS");
  return v && *v ? new std::string(std::string(",") + v + ",") : nullptr;
}();

// ---- Records of the origin's answers (docs/FORMAT.md) ---------------------
// Lines are built where the answer arrives, often on XrdCl's callback thread,
// and only queued there (CacheStore::enqueueRecord): the periodic checkpoint
// and the final dump write them. Nothing here blocks or touches the disk.

// A status's text for a record, escaped (OpenPolicy.h recordText).
std::string recordMsg(const XRootDStatus& s) {
  return s.IsOK() ? std::string() : recordText(s.ToString());
}

// The file a record names: its cache key, which carries no query.
std::string recordFile(const std::string& url) {
  const auto key = UrlKey::parse(url, globalConfig().keepCgi);
  return key ? key->key : url.substr(0, url.find('?'));
}

uint64_t wallMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

std::string hostPortOf(const XrdCl::URL& u) {
  return u.GetHostName() + ":" + std::to_string(u.GetPort());
}

// A failed status's provisional class; never kNone, since it failed.
FailureClass failureClassOf(const XRootDStatus& s) {
  const FailureClass c = classifyFailure(s.code, static_cast<int>(s.errNo));
  return c == FailureClass::kNone ? FailureClass::kOther : c;
}

// Stats::originFailures is client, site, notfound, data, expired, other (its
// published names are checked in the unit tests): the classes must stay in
// that order, from 1.
static_assert(static_cast<int>(FailureClass::kClient) == 1 &&
                  static_cast<int>(FailureClass::kSite) == 2 &&
                  static_cast<int>(FailureClass::kNotFound) == 3 &&
                  static_cast<int>(FailureClass::kData) == 4 &&
                  static_cast<int>(FailureClass::kExpired) == 5 &&
                  static_cast<int>(FailureClass::kOther) == 6,
              "FailureClass order differs from Stats::originFailures");
void countFailure(Stats& stats, FailureClass c) {
  const int i = static_cast<int>(c) - 1; // Stats::originFailures starts at kClient
  if (i >= 0 && i < 6)
    stats.originFailures[i].fetch_add(1, std::memory_order_relaxed);
}

// The number of an origin open and its retries, unique in the process.
std::atomic<uint64_t> gOpenSeq{0};

// One origin open attempt, as the code that issued it knows it.
struct OpenAttempt {
  HandleState* st = nullptr;
  OpenRoute route = OpenRoute::kRelay;
  uint64_t seq = 0;
  int attempt = 1;
  uint64_t t0 = 0;  // issued, steady µs
  uint64_t endUs = 0; // answered, when the record is written after something else was waited for
  int timeoutS = 0; // the request's own timeout; 0 = XrdCl's default
  // With adaptive_federation on, set on the last attempt of a sequence: how
  // the open ended ("ok" or "error") and the sequence's budget.
  const char* outcome = nullptr;
  int budgetS = 0;
};

// What one attempt's answer says: its hops and, on success, the file's stat
// as the open answered it (-1 when unknown).
struct AttemptView {
  std::vector<Hop> hops;
  ChainView v;
  long long statFlags = -1, size = -1;
};
// `f` is the File the attempt opened, or null for the handle's current one;
// Stat(false) answers from the stat the open stored and sends no request, so
// this may run on a callback thread. The reader may be destroying its handle
// while the open completes, so the current File is held as the read paths
// hold it (the handle's shutdown waits for innerOps) and skipped once the
// handle has shut it down. Throws nothing: for want of memory the view is
// empty, as for an answer with no hosts.
AttemptView viewOf(HandleState& st, const XRootDStatus& s, const HostList* hosts,
                   XrdCl::File* f) noexcept try {
  AttemptView w;
  if (hosts)
    for (const auto& h : *hosts)
      w.hops.push_back(makeHop(h.url.GetHostName(), h.url.GetPort(), h.flags, h.loadBalancer,
                               h.url.GetParamsAsString()));
  w.v = readChain(w.hops);
  if (!s.IsOK())
    return w;
  struct Held {
    HandleState* st = nullptr;
    ~Held() {
      if (st)
        st->releaseInner();
    }
  } held;
  if (!f) {
    std::lock_guard<std::mutex> g(st.mu);
    if (st.innerValid && st.inner.load()) {
      f = st.inner.load();
      ++st.innerOps;
      held.st = &st;
    }
  }
  if (f) {
    XrdCl::StatInfo* si = nullptr;
    if (f->Stat(false, si).IsOK() && si) {
      w.statFlags = static_cast<long long>(si->GetFlags());
      w.size = static_cast<long long>(si->GetSize());
      if (gTestOfflineHosts && w.v.last >= 0) {
        const Hop& h = w.hops[static_cast<size_t>(w.v.last)];
        if (gTestOfflineHosts->find("," + h.host + ":" + std::to_string(h.port) + ",") !=
            std::string::npos)
          w.statFlags |= static_cast<long long>(xrdwire::kStatOffline);
      }
    }
    delete si;
  }
  return w;
} catch (...) {
  return AttemptView();
}

SiteFailure siteFailure(const AttemptView& w, const XRootDStatus& s, const std::string& sentTo);

// Each origin open attempt the plugin watches ends here with XrdCl's status
// and the hosts the open went through (null when none came with the answer):
// it is counted and recorded, and a successful one tells the handle which
// server it reached. `f` is the File the attempt opened, or null for the
// handle's current one; on success its stat is the one the open answered with
// (no request is sent). The reader's open handlers call it on XrdCl's callback
// thread, with null; openAndWait calls it on the waiting thread, which holds
// innerOpenMu, with its own File. It never blocks in either. An open XrdCl
// refuses at once (Open returns the error, and no answer will come) is
// recorded the same way, by the code that issued it, with no hosts and
// `refusedAt`: the host:port of the URL it was asked to open, since no server
// answered. Nothing may leave it: a record lost for want of memory is a lost
// record, never a job. `view`, when given, is viewOf's reading of this answer.
void noteOriginOpenOrThrow(const OpenAttempt& a, const XRootDStatus* status, const HostList* hosts,
                           XrdCl::File* f, const std::string& refusedAt,
                           const AttemptView* view) {
  HandleState& st = *a.st;
  if (!st.store)
    return;
  Stats& stats = st.store->stats();
  const uint64_t us = (a.endUs ? a.endUs : nowUs()) - a.t0;
  stats.originOpenAttempts.fetch_add(1, std::memory_order_relaxed);
  stats.originOpenUs.add(us);
  const XRootDStatus s = status ? *status : XRootDStatus(XrdCl::stError, XrdCl::errInternal);
  const bool ok = s.IsOK();
  const FailureClass cls = ok ? FailureClass::kNone : failureClassOf(s);
  if (!ok) {
    stats.originOpenFailures.fetch_add(1, std::memory_order_relaxed);
    countFailure(stats, cls);
  }
  const AttemptView w = view ? *view : viewOf(st, s, hosts, f);
  const std::vector<Hop>& hops = w.hops;
  const ChainView& v = w.v;
  std::string server, host;
  if (v.last >= 0) {
    host = hops[static_cast<size_t>(v.last)].host;
    server = host + ":" + std::to_string(hops[static_cast<size_t>(v.last)].port);
  } else if (!ok && !refusedAt.empty()) {
    server = refusedAt;
    host = server.substr(0, server.rfind(':'));
  }
  if (ok && !server.empty() && a.route != OpenRoute::kRead) { // a move's is adopted, or not
    std::lock_guard<std::mutex> g(st.mu);
    st.dataServer = server;
    if (globalConfig().adaptiveFederation) {
      st.viaRedirector = st.viaRedirector || v.viaRedirector;
      st.source = siteFailure(w, s, server);
    }
  }
  const long long statFlags = w.statFlags, size = w.size;
  const int timeoutS = a.timeoutS > 0 ? a.timeoutS : defaultRequestTimeoutS();
  if (s.code == XrdCl::errOperationExpired) {
    if (globalConfig().adaptiveFederation) // the open deadline is the switch's
      stats.openDeadlineExpiries.fetch_add(1, std::memory_order_relaxed);
    // The server that did not answer is the last hop the open reached.
    const std::string at = !hops.empty() ? hops.back().host + ":" + std::to_string(hops.back().port)
                           : !server.empty() ? server
                                             : std::string("the origin");
    UCACHE_WARN("%s: the open at %s did not complete within %d s (a server that never accepted "
                "the connection, one that accepted it and did not answer, or a copy being staged "
                "from tape); the open fails",
                recordFile(st.url).c_str(), at.c_str(), timeoutS);
  }
  std::string line;
  line.reserve(512);
  line += "{\"kind\":\"open\",\"v\":1,\"ts_ms\":" + std::to_string(wallMs());
  line += ",\"file\":\"" + escapeForJson(recordFile(st.url)) + '"';
  line += ",\"seq\":" + std::to_string(a.seq) + ",\"attempt\":" + std::to_string(a.attempt);
  line += std::string(",\"route\":\"") + openRouteName(a.route) + '"';
  line += std::string(",\"status\":\"") + (ok ? "ok" : "error") + '"';
  line += ",\"code\":" + std::to_string(s.code) + ",\"errno\":" + std::to_string(s.errNo);
  line += std::string(",\"class\":\"") + failureClassName(cls) + '"';
  line += ",\"chain\":\"" + escapeForJson(chainText(hops)) + '"';
  line += ",\"data_server\":\"" + escapeForJson(server) + '"';
  line += ",\"site\":\"" + escapeForJson(siteOf(host)) + "\",\"site_src\":\"domain\"";
  line += ",\"stat_flags\":" + std::to_string(statFlags) + ",\"size\":" + std::to_string(size);
  line += ",\"us\":" + std::to_string(us) + ",\"timeout_s\":" + std::to_string(timeoutS);
  line += ",\"msg\":\"" + recordMsg(s) + '"';
  if (a.outcome)
    line += std::string(",\"outcome\":\"") + a.outcome + "\",\"budget_s\":" +
            std::to_string(a.budgetS);
  line += "}\n";
  st.store->enqueueRecord(std::move(line));
}
void noteOriginOpen(const OpenAttempt& a, const XRootDStatus* status, const HostList* hosts,
                    XrdCl::File* f, const std::string& refusedAt = std::string(),
                    const AttemptView* view = nullptr) {
  try {
    noteOriginOpenOrThrow(a, status, hosts, f, refusedAt, view);
  } catch (...) {
  }
}
// The site an open failed at, as a sequence of attempts sees it: the server
// the client itself would name (the chain's effective server, else the last
// host), its site (its domain; a server that never answered is its own site,
// host:port), the name a retry may carry for it, and the error. `sentTo`: the
// host:port the attempt was sent to, named when no host answered.
SiteFailure siteFailure(const AttemptView& w, const XRootDStatus& s, const std::string& sentTo) {
  SiteFailure f;
  const int i = w.v.effective >= 0 ? w.v.effective : w.v.last;
  if (i >= 0) {
    const Hop& h = w.hops[static_cast<size_t>(i)];
    f.server = h.host + ":" + std::to_string(h.port);
    f.site = h.flags ? siteOf(h.host) : f.server;
    f.name = exclusionName(w.hops, i);
  } else {
    f.server = f.site = sentTo.empty() ? std::string("the origin") : sentTo;
  }
  f.error = s.ToString();
  return f;
}
std::string hostPortOfUrl(const std::string& url) {
  const XrdCl::URL u(url);
  return u.IsValid() ? hostPortOf(u) : std::string();
}
// The host:port of the last host of `w`; "" for none.
std::string lastServer(const AttemptView& w) {
  if (w.v.last < 0)
    return std::string();
  const Hop& h = w.hops[static_cast<size_t>(w.v.last)];
  return h.host + ":" + std::to_string(h.port);
}

// A file of another size than the cached one, recorded (docs/FORMAT.md) and
// counted as a failure of its server in the file's entry, where an open the
// cache makes of the file for a reader's reselection request looks for the
// servers that failed it (UrlKey.h ownOpenUrl).
void noteSizeDisagreement(HandleState& st, const std::string& server, uint64_t served,
                          uint64_t cached) try {
  if (!st.store)
    return;
  std::shared_ptr<FileEntry> e;
  {
    std::lock_guard<std::mutex> g(st.mu);
    e = st.entry;
  }
  if (e)
    e->obs().servers.fail(server);
  std::string line = "{\"kind\":\"size_disagreement\",\"v\":1,\"ts_ms\":" + std::to_string(wallMs());
  line += ",\"file\":\"" + escapeForJson(recordFile(st.url)) + "\",\"site\":\"" +
          escapeForJson(siteOf(server.substr(0, server.rfind(':')))) + "\",\"server\":\"" +
          escapeForJson(server) + "\",\"sizes\":\"" + std::to_string(served) + "/" +
          std::to_string(cached) + "\"}\n";
  st.store->enqueueRecord(std::move(line));
} catch (...) {
}

// A copy refused for what it is, though its open succeeded: counted as a data failure of
// `server`, recorded as an `origin_failure` line of op `open`, and put on the file's account
// of its servers, as noteSizeDisagreement does.
void noteBadCopy(HandleState& st, const std::string& server, const std::string& why) try {
  std::shared_ptr<FileEntry> e;
  {
    std::lock_guard<std::mutex> g(st.mu);
    e = st.entry;
  }
  noteOriginFailure(st, e ? &e->obs().servers : nullptr, "open", 0, 0,
                    XRootDStatus(XrdCl::stError, XrdCl::errDataError, 0, why), server);
} catch (...) {
}

// One open's attempts, with adaptive_federation on, as one sequence
// (OpenPolicy.h): its time budget, read at its start, the sites that failed,
// and what the next attempt names in tried=. Used by one driver at a time: the
// reader's open handler, or the thread that runs the cache's own open.
struct FailoverSeq {
  FailoverSeq(int readerTimeoutS, ucache::XrdTimeout deadline)
      : deadlineS(static_cast<int>(deadline)), readerT(readerTimeoutS), t0(nowUs()) {
    int sew = XrdCl::DefaultStreamErrorWindow;
    trS = XrdCl::DefaultTimeoutResolution;
    if (XrdCl::Env* env = XrdCl::DefaultEnv::GetEnv()) {
      env->GetInt("StreamErrorWindow", sew);
      env->GetInt("TimeoutResolution", trS);
    }
    budget = budgetS(readerTimeoutS, sew, trS, gTestFailoverMaxS);
  }
  int64_t remainingMs() const {
    return int64_t(budget) * 1000 - static_cast<int64_t>((nowUs() - t0) / 1000);
  }
  // The next request's timeout: the open deadline, or what is left of the
  // budget when that is less (at least 1 s).
  ucache::XrdTimeout timeout() const {
    const int64_t leftS = (remainingMs() + 999) / 1000;
    return static_cast<ucache::XrdTimeout>(std::max<int64_t>(1, std::min<int64_t>(deadlineS, leftS)));
  }
  // Nothing more may be issued: the process is exiting, or the stop rule of
  // the budget holds (OpenPolicy.h budgetSpent).
  bool stopped() const {
    return processExiting() || budgetSpent(remainingMs(), deadlineS, readerT);
  }
  // The URL of the next attempt, at another site: `base` with the failed
  // servers named in tried=. Counted.
  std::string next(HandleState& st, const std::string& base) {
    if (lastResort) { // the reader's own URL, nothing excluded, taken as it answers
      excluded = false;
      return base;
    }
    const std::vector<std::string> names = exclusionNames(failed);
    excluded = !names.empty();
    ++failovers;
    if (st.store)
      st.store->stats().openFailovers.fetch_add(1, std::memory_order_relaxed);
    return withExclusions(base, names, "ioerr");
  }
  // The error the open ends with after `last`: once it has failed over, or a
  // bound stopped it, one naming every site that failed, counted and warned;
  // else `last` (also for want of memory).
  XRootDStatus finalError(HandleState& st, const std::string& file,
                          const XRootDStatus& last) const noexcept try {
    if (failed.empty())
      return last;
    // A refused copy is why the open failed, whatever the last attempt said: class data.
    const bool refused = std::any_of(failed.begin(), failed.end(),
                                     [](const SiteFailure& x) { return x.bad; });
    XRootDStatus out(refused ? XrdCl::stError : last.status,
                     refused ? XrdCl::errDataError : last.code, refused ? 0 : last.errNo,
                     allSitesFailedText(file, failed));
    if (st.store)
      st.store->stats().opensFailedEverywhere.fetch_add(1, std::memory_order_relaxed);
    UCACHE_WARN("%s: no site could open it: %s", file.c_str(), sitesFailedList(failed).c_str());
    return out;
  } catch (...) {
    return last;
  }
  // The open succeeded at `server` (host:port), `attempts` attempts in.
  void recovered(HandleState& st, const std::string& server, int attempts) const noexcept try {
    if (!failovers)
      return;
    if (st.store)
      st.store->stats().opensRecovered.fetch_add(1, std::memory_order_relaxed);
    if (!read) // a move says where it went when it ends
      UCACHE_INFO("%s: opened at %s (%s) after %d failed attempt(s)", recordFile(st.url).c_str(),
                  server.c_str(), siteOf(server.substr(0, server.rfind(':'))).c_str(), attempts - 1);
  } catch (...) {
  }

  // What the attempts so far say of where to ask: the regional redirector
  // (host:port) of the latest attempt that went through one, and whether the
  // latest failure was NotFound.
  void noteChain(const AttemptView& w, const XRootDStatus& s) {
    if (w.v.regional >= 0) {
      const Hop& h = w.hops[static_cast<size_t>(w.v.regional)];
      regional = h.host + ":" + std::to_string(h.port);
    }
    lastNotFound = s.code == XrdCl::errErrorResponse ? s.errNo == kXR_NotFound
                                                      : s.code == XrdCl::errNotFound;
  }

  int budget = 0;    // s
  int deadlineS = 0; // one attempt's open deadline
  int readerT = 0;   // the reader's own open timeout; 0 for none
  int trS = 0;       // the client's timeout resolution
  uint64_t t0 = 0;
  std::vector<SiteFailure> failed;
  bool excluded = false; // the current attempt names servers of ours
  int failovers = 0;
  // A copy on tape was opened and passed over. When no other site then opens
  // the file, one last open of the reader's own URL is made (`lastResort`) and
  // taken as it answers, tape or not: a job that works without the cache must
  // not fail because another site was looked for.
  bool sawOffline = false;
  bool lastResort = false;
  // The open of a move (reads of the handle stopped): `failed` starts with
  // the servers they stopped at, and a success there is not taken.
  bool read = false;
  // The copies the regional redirector listed, asked for once (`located`), in
  // the order to try them (`ci` is the next); `onCandidate`: the attempt in
  // flight is at one of them.
  std::string regional;
  bool lastNotFound = false, located = false, onCandidate = false;
  std::vector<Location> cands;
  size_t ci = 0;
};

// What follows one attempt of an open.
enum class Next : uint8_t {
  kDone,
  kSameSource,
  kElsewhere,
  kLocate,
  // The open succeeded and the copy is refused for what it is (no bytes in a ROOT file), and no
  // other copy is left to try: the open ends with one error that names every copy tried.
  kRefuse,
};
// The attempt answered `s` (`w`: its reading), the `sourceAttempt`-th at its
// source, sent to `sentUrl`; `want`: the size the cache holds (0: none). A
// transient failure is retried at the same source first (open_retries), while
// the sequence `seq` may issue more (null: adaptive_federation off); then,
// with `seq`, an outcome another site may not share -- a success of another
// size, an empty ROOT file or one on tape, included -- goes elsewhere. A copy of
// another size, or on tape, that no other site replaces is kept: that is how an open ends
// when a bound stops the sequence. An empty ROOT file is not: with no other copy to try
// the open ends with kRefuse, and its error names every copy (FailoverSeq::finalError).
// When no other site opens a file whose copy on tape was passed over, one last open of
// the reader's own URL is made and its answer stands, unless the copy is empty. Counts
// the retries.
// Throws nothing: for want of memory the open ends with this answer.
Next afterAttempt(HandleState& st, const Config& cfg, FailoverSeq* seq, const XRootDStatus& s,
                  const AttemptView& w, const std::string& sentUrl, int sourceAttempt,
                  uint64_t want) noexcept {
  const bool ok = s.IsOK();
  // With a sequence, an expiry at a server behind a redirector is not retried at the same URL,
  // which meets the server that stalled or its neighbour: it goes on to another site. An open
  // that has no other site to go to (a direct URL, or the redirector itself) is retried.
  if (!ok && sourceAttempt <= cfg.openRetries && isRetryableOpen(s.code, s.errNo) &&
      !(seq && s.code == xrdwire::kErrOperationExpired && w.v.viaRedirector && w.v.hops > 1)) {
    if (st.store)
      st.store->stats().openRetries.fetch_add(1, std::memory_order_relaxed);
    return Next::kSameSource;
  }
  if (!ok && sourceAttempt > 1 && st.store) // retried at this source and still failed
    st.store->stats().openRetriesExhausted.fetch_add(1, std::memory_order_relaxed);
  if (!seq)
    return Next::kDone;
  try {
    // A move takes no copy whose size it cannot read: the size the reader was shown is
    // what tells a copy from a damaged one.
    const bool sizeDiffers =
        ok && want && (w.size >= 0 ? static_cast<uint64_t>(w.size) != want : seq->read);
    // A copy with no bytes of a file named *.root is a bad copy, refused like one of another size.
    const bool emptyRoot = ok && !sizeDiffers && emptyRootCopy(st.url, w.size);
    const bool bad = sizeDiffers || emptyRoot;
    SiteFailure f = siteFailure(w, s, hostPortOfUrl(sentUrl));
    const bool offline =
        ok && w.statFlags >= 0 && (static_cast<uint64_t>(w.statFlags) & xrdwire::kStatOffline);
    const bool repeats = ok && seq->read && failedBefore(seq->failed, f);
    if (sizeDiffers)
      f.error = w.size < 0 ? std::string("its size could not be read")
                           : "it serves " + std::to_string(w.size) + " bytes, the cached file has " +
                                 std::to_string(want);
    else if (emptyRoot)
      f.error = "size 0: the copy is empty, and a ROOT file never is";
    else if (offline)
      f.error = "the copy there is on tape (offline)";
    else if (repeats)
      f.error = "reads from it stopped, and the redirector sent the open back";
    f.bad = emptyRoot;
    // The copy is refused and no other is tried: it goes on the list the error names, and is counted.
    const auto refuse = [&] {
      const bool listed = std::any_of(seq->failed.begin(), seq->failed.end(),
                                      [&](const SiteFailure& x) { return x.server == f.server; });
      if (!listed)
        seq->failed.push_back(f);
      noteBadCopy(st, f.server, f.error);
      return Next::kRefuse;
    };
    if (seq->lastResort) // the last open: whatever it answers stands, but an empty ROOT file
      return emptyRoot ? refuse() : Next::kDone;
    const bool onCandidate = seq->onCandidate;
    seq->onCandidate = false;
    seq->noteChain(w, s);
    // The open goes on to the regional redirector's list of copies when it was
    // not found though a copy may exist, when the redirector handed back a
    // server or site that failed already, when a server did not answer within
    // the deadline (a retry through the redirector would meet the site's other
    // server), and after an attempt at a listed copy that failed (it has no
    // redirector to send it elsewhere). Without a regional redirector to ask,
    // it ends, or tries elsewhere through the redirector after a stall.
    const bool canAsk = !seq->regional.empty() || seq->located;
    const OpenStep step =
        onCandidate ? (ok && !bad && !offline ? OpenStep::kFinal : OpenStep::kLocate)
                    : stepAfter(w.v, ok ? 0 : s.code, ok ? 0 : static_cast<int>(s.errNo), bad,
                                offline, seq->excluded, repeats, canAsk);
    AfterFailure af = AfterFailure::kStop;
    if (step != OpenStep::kFinal)
      af = afterFailure(seq->failed, f, failoverMaxSites(), seq->stopped());
    const bool more = seq->ci < seq->cands.size() || (seq->sawOffline && !seq->lastResort);
    const bool locate = af != AfterFailure::kStop &&
                        (step == OpenStep::kLocate || af == AfterFailure::kRepeat) && canAsk &&
                        (!onCandidate || more);
    const bool goOn = locate || (af == AfterFailure::kRetry && step == OpenStep::kFailOver);
    {
      std::lock_guard<std::mutex> g(st.mu);
      if (st.closed)
        return Next::kDone;
    }
    if (!goOn) {
      if (emptyRoot)
        return refuse();
      if (ok || !seq->sawOffline || processExiting())
        return Next::kDone;
      seq->lastResort = true; // see FailoverSeq::sawOffline
      return Next::kElsewhere;
    }
    if (offline)
      seq->sawOffline = true;
    if (sizeDiffers && w.size >= 0) {
      if (st.store)
        st.store->stats().sizeDisagreements.fetch_add(1, std::memory_order_relaxed);
      noteSizeDisagreement(st, f.server, static_cast<uint64_t>(w.size), want);
    } else if (emptyRoot) {
      noteBadCopy(st, f.server, f.error);
    }
    UCACHE_WARN("%s: %s (%s) could not open it (%s); trying another site",
                recordFile(st.url).c_str(), f.server.c_str(), f.site.c_str(),
                recordText(f.error, 300).c_str());
    return locate ? Next::kLocate : Next::kElsewhere;
  } catch (...) {
    return Next::kDone;
  }
}

// ---- Asking a regional redirector where a file is --------------------------
// One call of a FileSystem on `server` (host:port), waited for: the answer's
// status and response, or false when none came within the call's timeout (plus
// two timeout resolutions). The FileSystem is made and destroyed here, on the
// waiting thread, never on a callback thread; one whose call has not answered
// is left undestroyed. The handler and the waiter share the result.
struct FsReply {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  XRootDStatus status;
  std::unique_ptr<AnyObject> response;
};
class FsHandler : public ResponseHandler {
 public:
  explicit FsHandler(std::shared_ptr<FsReply> r) : r_(std::move(r)) {}
  void HandleResponse(XRootDStatus* status, AnyObject* response) override {
    std::unique_ptr<XRootDStatus> s(status);
    std::shared_ptr<FsReply> r = std::move(r_);
    delete this;
    std::lock_guard<std::mutex> g(r->mu);
    r->status = s ? *s : XRootDStatus(XrdCl::stError, XrdCl::errInternal);
    r->response.reset(response);
    r->done = true;
    r->cv.notify_all();
  }

 private:
  std::shared_ptr<FsReply> r_;
};
bool fsCall(const std::string& server, int timeoutS, int graceS,
            const std::function<XRootDStatus(XrdCl::FileSystem&, ResponseHandler*)>& issue,
            XRootDStatus& status, std::unique_ptr<AnyObject>& response) {
  auto* fs = new XrdCl::FileSystem(XrdCl::URL("root://" + server), false);
  auto r = std::make_shared<FsReply>();
  auto* h = new FsHandler(r);
  status = issue(*fs, h);
  if (!status.IsOK()) {
    delete h;
    delete fs;
    return true;
  }
  std::unique_lock<std::mutex> lk(r->mu);
  if (!r->cv.wait_for(lk, std::chrono::seconds(timeoutS + graceS), [&r] { return r->done; })) {
    status = XRootDStatus(XrdCl::stError, XrdCl::errOperationExpired, 0, "no answer in time");
    return false;
  }
  lk.unlock();
  status = r->status;
  response = std::move(r->response);
  delete fs;
  return true;
}
// The asks in flight, by (regional, path, refresh), and the lock over them. Never
// destroyed: detached threads may use them. Built by locateInit, before any thread
// exists, and made anew in a forked child, where a parent's thread may have held the
// lock; nothing is registered with fork here, so a fork cannot meet it half made.
struct LocateOp {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  std::vector<Location> out;
};
struct LocateTable {
  std::mutex* mu = new std::mutex;
  std::map<std::string, std::shared_ptr<LocateOp>>* ops =
      new std::map<std::string, std::shared_ptr<LocateOp>>;
};
LocateTable& locateTable() {
  static LocateTable* t = new LocateTable;
  return *t;
}
// The copies `q.regional` lists for `path`, with Refresh after a NotFound. Asks
// are coalesced: an ask for the same (regional, path, refresh) that is in
// flight is waited for instead of repeated, and counted once. Empty when the
// answer fails or does not come.
std::vector<Location> locateCopies(HandleState& st, FailoverSeq& q, const std::string& path) {
  const LocateTable table = locateTable();
  const std::string key = q.regional + ' ' + path + (q.lastNotFound ? " refresh" : "");
  std::shared_ptr<LocateOp> op;
  bool mine = false;
  {
    std::lock_guard<std::mutex> g(*table.mu);
    auto& slot = (*table.ops)[key];
    mine = !slot;
    if (mine)
      slot = std::make_shared<LocateOp>();
    op = slot;
  }
  const int t = q.timeout();
  if (mine) {
    // However this ends, the slot is finished and removed: one left unfinished would
    // make every later ask for the same file wait out its timeout.
    struct Finish {
      const LocateTable& table;
      const std::string& key;
      LocateOp& op;
      std::vector<Location> out;
      ~Finish() {
        {
          std::lock_guard<std::mutex> g(op.mu);
          op.out = std::move(out);
          op.done = true;
          op.cv.notify_all();
        }
        std::lock_guard<std::mutex> g(*table.mu);
        table.ops->erase(key);
      }
    } fin{table, key, *op, {}};
    if (st.store)
      st.store->stats().failoverLocates.fetch_add(1, std::memory_order_relaxed);
    XRootDStatus s;
    std::unique_ptr<AnyObject> resp;
    const XrdCl::OpenFlags::Flags flags =
        XrdCl::OpenFlags::PrefName | (q.lastNotFound ? XrdCl::OpenFlags::Refresh : XrdCl::OpenFlags::None);
    const bool answered = fsCall(
        q.regional, t, 2 * q.trS,
        [&](XrdCl::FileSystem& fs, ResponseHandler* h) {
          return fs.Locate(path, flags, h, static_cast<uint16_t>(t));
        },
        s, resp);
    XrdCl::LocationInfo* info = nullptr;
    if (answered && s.IsOK() && resp)
      resp->Get(info);
    if (info)
      for (auto l = info->Begin(); l != info->End(); ++l)
        fin.out.push_back(locationOf(l->GetAddress(), l->IsManager(),
                                     l->GetType() == XrdCl::LocationInfo::ManagerPending ||
                                         l->GetType() == XrdCl::LocationInfo::ServerPending));
  }
  std::unique_lock<std::mutex> lk(op->mu);
  op->cv.wait_for(lk, std::chrono::seconds(t + 3 * q.trS), [&op] { return op->done; });
  return op->out;
}
// The URL of the next listed copy to open, or "" when there is none (or a bound
// stops the sequence). `base`: the URL the sequence's retries start from. The
// first call asks the regional redirector. Each copy is asked for its state
// first: one on tape is passed over (counted), one that does not answer fails
// (an entry of the sequence's failures). A copy that is a redirector is opened
// with the failed servers named in tried=. When nothing is left and a copy on
// tape was passed over, the reader's own URL is given once. Blocks; never called
// on a client callback thread. Throws nothing.
std::string locateNext(HandleState& st, FailoverSeq& q, const std::string& base) noexcept try {
  const Config& cfg = globalConfig();
  if (!q.located) {
    q.located = true;
    std::vector<Location> listed;
    for (const Location& l : locateCopies(st, q, XrdCl::URL(base).GetPath()))
      if ((cfg.allowHosts.empty() || hostMatchesAny(cfg.allowHosts, l.host)) &&
          !hostMatchesAny(cfg.denyHosts, l.host))
        listed.push_back(l);
    q.cands = orderLocations(listed, triedHosts(base), q.failed);
  }
  const std::string file = recordFile(st.url);
  while (q.ci < q.cands.size() && !q.stopped()) {
    const Location c = q.cands[q.ci++];
    const std::string server = (c.host.find(':') == std::string::npos ? c.host : "[" + c.host + "]") +
                               ":" + std::to_string(c.port);
    const std::string where = urlAtServer(base, server, false);
    XRootDStatus s;
    std::unique_ptr<AnyObject> resp;
    const int t = std::min<int>(q.timeout(), kCandidateStatS);
    (void)fsCall(
        server, t, 2 * q.trS,
        [&](XrdCl::FileSystem& fs, ResponseHandler* h) {
          return fs.Stat(XrdCl::URL(where).GetPathWithParams(), h, static_cast<uint16_t>(t));
        },
        s, resp);
    XrdCl::StatInfo* si = nullptr;
    if (s.IsOK() && resp)
      resp->Get(si);
    if (si) {
      if (copyOffline(si->GetFlags(), server, gTestOfflineHosts)) {
        if (st.store)
          st.store->stats().offlineCopiesSkipped.fetch_add(1, std::memory_order_relaxed);
        q.sawOffline = true;
        UCACHE_INFO("%s: the copy at %s is on tape (offline); not opened", file.c_str(),
                    server.c_str());
        continue;
      }
      q.onCandidate = true;
      ++q.failovers;
      if (st.store)
        st.store->stats().openFailovers.fetch_add(1, std::memory_order_relaxed);
      q.excluded = c.manager && !exclusionNames(q.failed).empty();
      return c.manager ? withExclusions(urlAtServer(base, server, true), exclusionNames(q.failed),
                                        "ioerr")
                       : where;
    }
    SiteFailure f;
    f.server = server;
    f.site = server;
    f.error = s.ToString();
    if (afterFailure(q.failed, f, failoverMaxSites(), q.stopped()) == AfterFailure::kStop)
      break;
  }
  if (q.sawOffline && !q.lastResort && !processExiting()) {
    q.lastResort = true;
    q.excluded = false;
    return base;
  }
  return std::string();
} catch (...) {
  return std::string();
}
// Whether an open about to be issued on `f` goes to the client's request path:
// false when an earlier open on `f` was sent. The client then answers from that
// File's own state (in progress, already open, or its stored error) and asks no
// server. An open the client refused at once leaves its File closed, so the
// next open on it is a new attempt. Read before the open; sentOpenOn after one
// that returned OK.
bool firstOpenOn(const HandleState& st, XrdCl::File* f) {
  return st.openSentOn.load() != f;
}
void sentOpenOn(HandleState& st, XrdCl::File* f) {
  st.openSentOn.store(f);
}
// An open XrdCl refused at once, recorded against the URL's server; only when
// firstOpenOn said it was a new attempt.
void noteRefusedOpen(const OpenAttempt& a, const XRootDStatus& status, const std::string& url) {
  try {
    const XrdCl::URL u(url);
    noteOriginOpen(a, &status, nullptr, nullptr, u.IsValid() ? hostPortOf(u) : std::string());
  } catch (...) {
  }
}

// The reader's open of a cached handle: XrdCl's own completion is passed to
// the reader unchanged -- status, response and hosts -- through the same call,
// on the thread XrdCl called back on, once the attempt is recorded. Nothing is
// created or destroyed there but this object.
class ReaderOpenHandler : public ResponseHandler {
 public:
  ReaderOpenHandler(ResponseHandler* real, std::shared_ptr<HandleState> st, int timeoutS)
      : real_(real), st_(std::move(st)) {
    a_.st = st_.get();
    a_.seq = gOpenSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    a_.t0 = nowUs();
    a_.timeoutS = timeoutS;
  }
  void HandleResponseWithHosts(XRootDStatus* status, AnyObject* response,
                               HostList* hostList) override {
    noteOriginOpen(a_, status, hostList, nullptr);
    ResponseHandler* real = real_;
    delete this;
    real->HandleResponseWithHosts(status, response, hostList);
  }
  void HandleResponse(XRootDStatus* status, AnyObject* response) override {
    noteOriginOpen(a_, status, nullptr, nullptr);
    ResponseHandler* real = real_;
    delete this;
    real->HandleResponse(status, response);
  }
  // XrdCl refused the open of `url` at once (within its error window after a
  // server failed, a new request to it fails before it is sent): no completion
  // will come, so the attempt is recorded here (when `fresh`: the first open
  // issued on its File), and the failure goes to the reader through its
  // handler, as every other open failure does. Open then returns OK, so a
  // reader handles one kind of failure only. The answer is posted, never given
  // inside Open: a reader may hold a lock across Open that its handler takes.
  // It carries an empty host list, since no host answered. The task holds only
  // the reader's handler and a status (never the plugin object) and makes or
  // destroys no client object; the work pool rebuilds itself in a forked child.
  // What it costs: the reader's handler runs on uCache's work pool, behind the
  // tasks queued there, some of which block (a cache open of the origin waits
  // up to its deadline, or with adaptive_federation off up to the client's
  // request timeout, a first-pass request up to its timeout). So a refusal
  // that used to come back at once can reach the reader as late as one such
  // wait when every worker is busy -- a one-worker pool on a one-core slot is
  // the plain case. And a handler that itself opens, synchronously, another
  // file the cache serves, refused too, waits for a free worker: on a
  // one-worker pool it holds the only one and never returns. A pool kept for
  // the delivery of reader opens alone removes the first; the second needs it
  // never to wait on itself.
  void refused(const XRootDStatus& status, const std::string& url, bool fresh) {
    if (fresh)
      noteRefusedOpen(a_, status, url);
    ResponseHandler* real = real_;
    delete this;
    Executor::instance().post([real, status] {
      real->HandleResponseWithHosts(new XRootDStatus(status), nullptr, new HostList());
    });
  }

 private:
  ResponseHandler* real_;
  std::shared_ptr<HandleState> st_;
  OpenAttempt a_;
};

// The cache's own origin open, waited for. The asynchronous open is used
// because its answer carries the hosts the open went through, which the
// synchronous form drops. The handler and the waiting thread share the result,
// and the handler deletes itself, so neither can outlive what the other uses.
struct OpenResult {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  XRootDStatus status;
  std::unique_ptr<HostList> hosts;
};
class OpenWaitHandler : public ResponseHandler {
 public:
  explicit OpenWaitHandler(std::shared_ptr<OpenResult> r) : r_(std::move(r)) {}
  void HandleResponseWithHosts(XRootDStatus* status, AnyObject* response,
                               HostList* hostList) override {
    finish(status, response, hostList);
  }
  void HandleResponse(XRootDStatus* status, AnyObject* response) override {
    finish(status, response, nullptr);
  }

 private:
  void finish(XRootDStatus* status, AnyObject* response, HostList* hostList) {
    std::unique_ptr<XRootDStatus> s(status);
    delete response;
    std::shared_ptr<OpenResult> r = std::move(r_);
    delete this;
    std::lock_guard<std::mutex> g(r->mu);
    r->status = s ? *s : XRootDStatus(XrdCl::stError, XrdCl::errInternal);
    r->hosts.reset(hostList);
    r->done = true;
    r->cv.notify_all();
  }
  std::shared_ptr<OpenResult> r_;
};
// One answer to the cache's own origin open.
struct OpenAnswer {
  XRootDStatus status;
  std::unique_ptr<HostList> hosts; // null when none came
  bool sent = false;  // the open was sent; else the client refused it at once
  bool fresh = false; // refused at once on a File whose earlier open was not sent
  // The wait ended before any answer came, so the File may still have its open
  // in flight: it is never destroyed (left behind, as a parent's File is).
  bool unfinished = false;
};
// Opens `f` for reading with `timeout` (0: the client's request timeout, as
// with adaptive_federation off) and waits for the answer, as the synchronous
// open did; an error XrdCl returns at once, when no answer will come, is
// returned as it is. `netS` > 0 bounds the wait: an answer that has not come
// that long after the request's own timeout would have ended it is taken as an
// expiry (a late one lands in the shared result, which nobody reads). `a` is
// the attempt; its issue time and timeout are set here. Records nothing.
OpenAnswer openAndWait(XrdCl::File* f, const std::string& url, OpenAttempt& a,
                       ucache::XrdTimeout timeout, int netS) {
  OpenAnswer ans;
  auto r = std::make_shared<OpenResult>();
  auto* h = new OpenWaitHandler(r);
  a.t0 = nowUs();
  a.endUs = 0;
  a.timeoutS = static_cast<int>(timeout);
  ans.fresh = firstOpenOn(*a.st, f);
  ans.status = f->Open(url, XrdCl::OpenFlags::Read, XrdCl::Access::None, h, timeout);
  if (!ans.status.IsOK()) {
    delete h;
    return ans;
  }
  ans.sent = true;
  sentOpenOn(*a.st, f);
  std::unique_lock<std::mutex> lk(r->mu);
  if (netS > 0) {
    if (!r->cv.wait_for(lk, std::chrono::seconds(timeout + netS), [&r] { return r->done; })) {
      ans.status = XRootDStatus(XrdCl::stError, XrdCl::errOperationExpired, 0,
                                "no answer to the open after its timeout had passed");
      ans.unfinished = true;
      return ans;
    }
  } else {
    r->cv.wait(lk, [&r] { return r->done; });
  }
  lk.unlock(); // the handler writes nothing more once done is set
  ans.status = r->status;
  ans.hosts = std::move(r->hosts);
  return ans;
}
// Records an answer of openAndWait's (`w`: its reading).
void noteAnswer(const OpenAttempt& a, const OpenAnswer& ans, const std::string& url,
                XrdCl::File* f, const AttemptView& w) {
  if (ans.sent)
    noteOriginOpen(a, &ans.status, ans.hosts.get(), f, std::string(), &w);
  else if (ans.fresh)
    noteRefusedOpen(a, ans.status, url);
}

// ---- The reader's open, retried --------------------------------------------
// Wraps the reader's open handler to retry an open that failed, each time on a
// FRESH inner file (a failed open is terminal on the object):
//  - open_retries: a TRANSIENT failure is tried again at the same source, with
//    full-jitter backoff;
//  - with adaptive_federation on, an open through a redirector that failed at
//    one site in a way another site may not share is made again through the
//    redirector, with the servers that failed named in tried= (OpenPolicy.h),
//    within one time budget and a bound on the sites; a success is taken
//    wherever it lands, and one on tape is passed over only when another site
//    is tried. When the redirector sends it back to a server or site that
//    failed already, the open ends: asking the regional redirector for the
//    other copies is not done here.
// What follows a failure -- a new File, the next attempt -- is posted, never
// done on the client's callback thread, where making or destroying a File can
// hang a fork. Forwards EXACTLY ONE final completion to the real handler:
// XrdCl's own on success; on failure the last attempt's error, or once it has
// failed over, an error naming every site that failed. Then deletes itself.
// Captures only st_ (never the plugin object): a posted step may run after
// Open has returned.
class RetryingOpenHandler : public ResponseHandler {
 public:
  RetryingOpenHandler(std::shared_ptr<HandleState> st, std::string url,
                      XrdCl::OpenFlags::Flags flags, XrdCl::Access::Mode mode,
                      ucache::XrdTimeout timeout, ResponseHandler* real, const Config& cfg,
                      bool record, int readerTimeoutS)
      : st_(std::move(st)), url_(std::move(url)), flags_(flags), mode_(mode),
        timeout_(timeout), real_(real), cfg_(cfg), record_(record) {
    a_.st = st_.get();
    a_.seq = gOpenSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    a_.timeoutS = static_cast<int>(timeout);
    source_ = url_;
    if (record && cfg.adaptiveFederation) {
      seq_ = std::make_unique<FailoverSeq>(readerTimeoutS, timeout);
      base_ = mergeTried(url_); // the reader's excluded servers stay excluded
    }
  }

  // Issue the first attempt on the ctor-created inner file. Always returns stOK:
  // `real_` will receive exactly one completion (after a retry, or a forwarded
  // failure), so the XrdCl sync wrapper waits for our handler instead of
  // short-circuiting on a sync error. For a handle the cache serves, what
  // follows a sync error is posted, so that the reader is never answered from
  // inside Open (see ReaderOpenHandler::refused, which also says what posting
  // to the work pool costs).
  XRootDStatus start() {
    bool fresh = false;
    const XRootDStatus s = issueOn(st_->inner.load(), fresh);
    if (!s.IsOK()) { // sync issue error: no callback will come
      RetryingOpenHandler* self = this;
      if (record_)
        Executor::instance().post([self, s, fresh] { self->refusedAtOnce(s, fresh); });
      else
        refusedAtOnce(s, fresh);
    }
    return XRootDStatus();
  }

 private:
  XRootDStatus issueOn(XrdCl::File* f, bool& fresh) {
    ++attempt_;
    ++sourceAttempt_;
    a_.attempt = attempt_;
    a_.t0 = nowUs();
    a_.endUs = 0;
    const ucache::XrdTimeout t = seq_ ? seq_->timeout() : timeout_;
    a_.timeoutS = static_cast<int>(t);
    sentUrl_ = faultOpenUrl(source_, attempt_);
    const std::string url = sentUrl_; // once sent, `this` may be gone at any time
    fresh = firstOpenOn(*st_, f);
    const std::shared_ptr<HandleState> st = st_;
    XRootDStatus s = f->Open(url, flags_, mode_, this, t);
    if (s.IsOK())
      sentOpenOn(*st, f);
    return s;
  }

  void HandleResponse(XRootDStatus* status, AnyObject* response) override {
    HandleResponseWithHosts(status, response, nullptr); // a chained plugin's completion
  }
  void HandleResponseWithHosts(XRootDStatus* status, AnyObject* response,
                               HostList* hostList) override {
    std::unique_ptr<XRootDStatus> s(status ? status
                                           : new XRootDStatus(XrdCl::stError, XrdCl::errInternal));
    std::unique_ptr<AnyObject> r(response);
    std::unique_ptr<HostList> h(hostList);
    const AttemptView w = record_ ? viewOf(*st_, *s, h.get(), nullptr) : AttemptView();
    if (ended(*s, h, w, /*sent=*/true, /*fresh=*/false))
      return;
    // Transparent on success: forward exactly what we got.
    real_->HandleResponseWithHosts(s.release(), r.release(), h ? h.release() : new HostList());
    delete this;
  }

  // The client refused the attempt at once: no completion will come. Runs off
  // the opening thread for a handle the cache serves.
  void refusedAtOnce(const XRootDStatus& s, bool fresh) {
    std::unique_ptr<HostList> none;
    ended(s, none, AttemptView(), /*sent=*/false, fresh);
  }

  // One attempt ended with `s` (`sent`: it reached the client's request path,
  // else the client refused it at once, recorded when `fresh`). False for a
  // success the reader gets as it is. Otherwise the next attempt is scheduled
  // -- after a success too, when its copy is on tape and another site is
  // tried -- or the failure forwarded, and `this` may be gone.
  bool ended(const XRootDStatus& s, std::unique_ptr<HostList>& h, const AttemptView& w, bool sent,
             bool fresh) {
    const Next n = afterAttempt(*st_, cfg_, seq_.get(), s, w, sentUrl_, sourceAttempt_, 0);
    const bool ok = s.IsOK();
    a_.endUs = nowUs();
    if (seq_ && (n == Next::kDone || n == Next::kRefuse)) {
      a_.outcome = ok && n == Next::kDone ? "ok" : "error";
      a_.budgetS = seq_->budget;
    }
    if (n == Next::kLocate) // recorded by doRetry, which asks where the copies are first
      held_ = Held{s, w, sent, fresh};
    else
      note(s, w, sent, fresh);
    if (ok && n == Next::kDone) {
      if (seq_)
        seq_->recovered(*st_, lastServer(w), attempt_);
      return false;
    }
    lastStatus_ = n == Next::kRefuse
                      ? XRootDStatus(XrdCl::stError, XrdCl::errDataError, 0, "the copy is refused")
                  : ok ? XRootDStatus(XrdCl::stError, XrdCl::errErrorResponse, kXR_FSError,
                                      "the copy there is on tape (offline)")
                       : s;
    if (record_ && h)
      lastHosts_ = std::move(h); // the reader's failure carries the last hosts there were
    RetryingOpenHandler* self = this;
    switch (n) {
    case Next::kSameSource:
      moveExecutor().postAfter(backoffMs(sourceAttempt_, cfg_),
                               [self] { self->doRetry(false, false, false); });
      break;
    case Next::kElsewhere:
    case Next::kLocate:
      moveExecutor().post([self, ok, n] { self->doRetry(true, ok, n == Next::kLocate); });
      break;
    case Next::kRefuse: // the refused copy is open: closed in the executor, not on this thread
      moveExecutor().post([self] { self->doRefuse(); });
      break;
    case Next::kDone:
      forwardFailure();
      break;
    }
    return true;
  }

  // The record of an attempt.
  void note(const XRootDStatus& s, const AttemptView& w, bool sent, bool fresh) {
    if (!record_)
      return;
    if (sent)
      noteOriginOpen(a_, &s, nullptr, nullptr, std::string(), &w);
    else if (fresh)
      noteRefusedOpen(a_, s, sentUrl_);
  }

  // Runs on the move executor (off the XrdCl callback thread): swap in a fresh file
  // and re-issue, at the same source or, `elsewhere`, through the redirector with
  // the failed servers named. `closeOld`: the replaced file is open (refused
  // for its copy) and is closed without waiting. If the handle shut down,
  // forward the last failure.
  void doRetry(bool elsewhere, bool closeOld, bool locate) {
    const bool exiting = processExiting(); // the exit handler has begun: nothing more is issued
    const std::shared_ptr<HandleState> st = st_; // once sent, `this` may be gone
    std::string listed; // the next listed copy; asking for it waits, here
    if (locate) {
      if (!exiting)
        listed = locateNext(*st, *seq_, base_);
      if (exiting || listed.empty()) { // the last line of the sequence says how it ended
        a_.outcome = "error";
        a_.budgetS = seq_->budget;
      }
      note(held_->s, held_->w, held_->sent, held_->fresh);
      held_.reset();
    }
    if (exiting || (locate && listed.empty())) {
      if (closeOld && !exiting) // a copy refused for what it is, and no other to try
        st->resetInner(/*leakOld=*/false, /*onlyIfIdle=*/false, /*closeOld=*/true);
      forwardFailure();
      return;
    }
    XrdCl::File* fresh = st->resetInner(/*leakOld=*/false, /*onlyIfIdle=*/false, closeOld);
    if (!fresh) {
      forwardFailure();
      return;
    }
    if (elsewhere) {
      source_ = locate ? listed : seq_->next(*st, base_);
      a_.route = seq_->onCandidate ? OpenRoute::kCandidate : OpenRoute::kFailover;
      sourceAttempt_ = 0;
    }
    bool isFresh = false;
    const XRootDStatus s = issueOn(fresh, isFresh);
    if (!s.IsOK()) // sync re-issue error: classify again, never drop a completion
      refusedAtOnce(s, isFresh);
  }

  // Runs on the move executor: the open ends with the error that names the copies. The copy
  // that was refused is open, and is closed without waiting.
  void doRefuse() {
    const std::shared_ptr<HandleState> st = st_; // once forwarded, `this` is gone
    if (!processExiting())
      st->resetInner(/*leakOld=*/false, /*onlyIfIdle=*/false, /*closeOld=*/true);
    forwardFailure();
  }

  void forwardFailure() {
    // Once it has failed over, an error naming every site that failed.
    const XRootDStatus out =
        seq_ ? seq_->finalError(*st_, recordFile(st_->url), lastStatus_) : lastStatus_;
    // The hosts of the last attempt that went through any, else a non-null
    // empty HostList: the native completion contract (see complete()).
    HostList* hosts = lastHosts_ ? lastHosts_.release() : new HostList();
    real_->HandleResponseWithHosts(new XRootDStatus(out), nullptr, hosts);
    delete this;
  }

  std::shared_ptr<HandleState> st_;
  std::string url_;
  XrdCl::OpenFlags::Flags flags_;
  XrdCl::Access::Mode mode_;
  ucache::XrdTimeout timeout_;
  ResponseHandler* real_;
  const Config& cfg_;
  const bool record_; // a handle the cache serves: its attempts are recorded
  int attempt_ = 0;
  int sourceAttempt_ = 0; // attempts at the current source
  XRootDStatus lastStatus_;
  std::unique_ptr<HostList> lastHosts_; // of the last failed attempt that had any
  OpenAttempt a_; // the attempt in flight, as recorded
  // An attempt whose record waits for the locate that follows it (doRetry).
  struct Held {
    XRootDStatus s;
    AttemptView w;
    bool sent, fresh;
  };
  std::optional<Held> held_;
  std::string source_;  // the URL of the current source's attempts
  std::string sentUrl_; // the URL the last attempt was sent with
  // With adaptive_federation on: the sequence, and the reader's URL as every
  // retry elsewhere starts from it.
  std::unique_ptr<FailoverSeq> seq_;
  std::string base_;
};

} // namespace

void locateInit() {
  (void)locateTable();
  ::pthread_atfork(nullptr, nullptr, [] { locateTable() = LocateTable(); });
}

uint64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Every completion the plugin synthesizes (trusted cache-only opens, locally
// served reads, no-op closes) must honor the NATIVE completion contract:
// XrdCl always hands HandleResponseWithHosts a non-null HostList, and real
// consumers dereference it unconditionally — CMSSW's XrdAdaptor SEGVs on
// null (tracerouteRedirections iterates *hostList; reproduced with cmsRun on
// a warm cache). An EMPTY list is the honest value for a locally-served
// completion (no host was contacted) and is safe in every consumer we
// audited (traceroute prints an empty trace without touching per-host
// transport queries; determineHostExcludeString is size-guarded). The
// receiving handler owns and deletes it, exactly as with native XrdCl.
void complete(ResponseHandler* h, XRootDStatus* status, AnyObject* response) {
  h->HandleResponseWithHosts(status, response, new HostList());
}

XRootDStatus* okStatus() { return new XRootDStatus(); }

AnyObject* chunkResponse(uint64_t off, uint32_t len, void* buf) {
  auto* obj = new AnyObject();
  obj->Set(new ChunkInfo(off, len, buf));
  return obj;
}

AnyObject* vreadResponse(const ChunkList& chunks) {
  auto* info = new XrdCl::VectorReadInfo();
  uint32_t total = 0;
  for (const auto& c : chunks)
    total += c.length;
  info->GetChunks() = chunks;
  info->SetSize(total);
  auto* obj = new AnyObject();
  obj->Set(info);
  return obj;
}

std::string answeredBy(HandleState& st, const HostList* hosts) noexcept {
  // Evaluated in completion handlers as an argument, outside any callee's
  // catch: nothing may leave it, and an unnamed server is a lost count.
  try {
    if (hosts && !hosts->empty())
      return hostPortOf(hosts->back().url);
    std::lock_guard<std::mutex> g(st.mu);
    return st.dataServer;
  } catch (...) {
    return std::string();
  }
}

std::string issueServer(HandleState& st, XrdCl::File* f) noexcept {
  try {
    std::string server = answeredBy(st, nullptr);
    if (server.empty() && f && f->GetProperty("DataServer", server))
      server = serverOfHostId(std::move(server));
    return server;
  } catch (...) {
    return std::string();
  }
}

std::string wireTraceFields(const std::string& ds, uint64_t t0, uint64_t nc) noexcept {
  try {
    return "\"ds\":\"" + escapeForJson(ds) + "\",\"t0\":" + std::to_string(t0) +
           ",\"nc\":" + std::to_string(nc);
  } catch (...) {
    return std::string(); // the record is written without them
  }
}

void noteOriginFailure(HandleState& st, ServerTally* tally, const char* op, uint64_t off,
                       uint64_t len, const XRootDStatus& s, const std::string& server,
                       const std::string& codec) try {
  if (!st.store)
    return;
  const FailureClass cls = failureClassOf(s);
  countFailure(st.store->stats(), cls);
  if (tally)
    tally->fail(server);
  const std::string file = recordFile(st.url);
  std::string line;
  line.reserve(384);
  line += "{\"kind\":\"origin_failure\",\"v\":1,\"ts_ms\":" + std::to_string(wallMs());
  line += ",\"file\":\"" + escapeForJson(file) + "\",\"op\":\"" + op + '"';
  line += ",\"off\":" + std::to_string(off) + ",\"len\":" + std::to_string(len);
  line += ",\"code\":" + std::to_string(s.code) + ",\"errno\":" + std::to_string(s.errNo);
  line += std::string(",\"class\":\"") + failureClassName(cls) + '"';
  line += ",\"data_server\":\"" + escapeForJson(server) + '"';
  if (!codec.empty())
    line += ",\"codec\":\"" + escapeForJson(codec) + '"';
  line += ",\"msg\":\"" + recordMsg(s) + "\"}\n";
  // At most kRecordsPerCapKey lines per file, server and class: a server that
  // fails every request must not fill the queue (the per-file record counts
  // every failure).
  st.store->enqueueRecord(std::move(line),
                          file + '\n' + server + '\n' + failureClassName(cls));
} catch (...) {
  // a completion handler of the client library: nothing may leave it
}

namespace {

// Cache-freshness marker (UCACHE_REVALIDATE_S): objects/<aa>/<hash>.val, whose
// mtime records the last successful remote validation. touchVal() stamps it
// after a real origin stat; cacheFresh() trusts the entry while it is younger
// than the window AND a sidecar is present.
std::string valPath(const UrlKey& key, const std::string& cacheDir) {
  return key.objectDir(cacheDir) + "/" + key.hashHex + ".val";
}
void touchVal(const UrlKey& key, const std::string& cacheDir) {
  int fd = RealIO::instance().open(valPath(key, cacheDir), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0)
    RealIO::instance().close(fd);
}
bool cacheFresh(const UrlKey& key, const std::string& cacheDir, int revalidateSeconds) {
  struct ::stat st;
  if (RealIO::instance().stat(valPath(key, cacheDir), &st) != 0)
    return false;
  uint64_t now = static_cast<uint64_t>(::time(nullptr));
  if (now > static_cast<uint64_t>(st.st_mtime) + static_cast<uint64_t>(revalidateSeconds))
    return false; // stale — revalidate against the origin
  struct ::stat ms; // a sidecar must exist for the entry to be usable
  return RealIO::instance().stat(key.metaPath(cacheDir), &ms) == 0;
}

} // namespace

//------------------------------------------------------------------ state --

// Lazy origin open (cache-freshness path only): a trusted-cache handle
// deferred the remote open; a genuine miss needs it now. Serialized by
// innerOpenMu, off the fast path; runs on a caller/executor thread (never an
// XrdCl callback thread), so waiting here for the open is allowed.
// Failed attempts re-arm after a cool-down instead of latching (a transient
// dead-server reselection must not kill the handle for the rest of the job).
constexpr uint64_t kLazyOpenRetryCooldownUs = 1'000'000;

// The hosts this process has seen fail a request of the file `st` reads, or
// serve it at another size than the cache holds (its entry's account of its
// servers, shared by every handle of the file).
static std::vector<std::string> failedHostsOf(HandleState& st) {
  std::shared_ptr<FileEntry> e;
  {
    std::lock_guard<std::mutex> g(st.mu);
    e = st.entry;
  }
  return e ? e->obs().servers.failedHosts() : std::vector<std::string>();
}

bool HandleState::ensureInnerOpen() {
  std::lock_guard<std::mutex> g(innerOpenMu);
  if (innerOpened)
    return innerOpenOk;
  // The reopen is over when this is, however it ends: a caller that asks is told so
  // without waiting for the lock (reopenPending).
  struct Reopened {
    std::atomic<bool>& stale;
    ~Reopened() { stale.store(false, std::memory_order_release); }
  } reopened{innerStale};
  // Which of the cache's own opens this is, for its records: a forked child's
  // open afresh, a trusted handle's at its first miss, or one whose trusted
  // entry was gone (ensureEntry clears cacheOnly before it asks).
  OpenAttempt a;
  a.st = this;
  {
    std::lock_guard<std::mutex> m(mu);
    a.route = innerStale ? OpenRoute::kFork : cacheOnly ? OpenRoute::kLazy : OpenRoute::kDegraded;
  }
  if (innerStale) { // see startOverAfterFork; innerOps is 0 in the child
    if (!resetInner(/*leakOld=*/true))
      return false; // the handle is shut down
    lazyOpenFailUs = 0;
  }
  if (lazyOpenFailUs && nowUs() - lazyOpenFailUs < kLazyOpenRetryCooldownUs)
    return false; // recent failure: rate-limit re-attempts, keep the error
  const Config& cfg = globalConfig();
  const bool fo = cfg.adaptiveFederation;
  // This open is the cache's own (a trusted handle's first miss, a degraded
  // handle, or a forked child starting over), so it gets the reader's URL with
  // one change. Servers the reader asked to avoid (tried=) stay avoided. A
  // reselection request (triedrc ending in "resel", which a client sends to get
  // a second server beside the ones it uses) is not passed on: inheriting it
  // pushed these opens onto servers this host could not reach, on every
  // re-attempt. With adaptive_federation on, the servers of such a request
  // that this process saw fail for the file stay avoided (UrlKey.h). Auth
  // tokens and xrd.* options pass through untouched. The relayed open (an
  // untrusted handle) sends the reader's URL verbatim, since there the inner
  // connection is the reader's own.
  const std::string fillUrl = fo ? ownOpenUrl(url, failedHostsOf(*this)) : ownOpenUrl(url);
  XrdCl::File* f = inner.load();
  a.seq = gOpenSeq.fetch_add(1, std::memory_order_relaxed) + 1;
  // The reader of a trusted handle holds positions in the size it was shown.
  // A source of another size is another copy of the file -- a damaged or
  // truncated one, or the file rewritten -- and its bytes must not complete
  // this entry. The open already returns the source's stat.
  const uint64_t want = expectedOriginSize.load(std::memory_order_relaxed);
  // With adaptive_federation on, the attempts are one sequence (FailoverSeq):
  // each has the open deadline, or what is left of the budget when that is
  // less; an answer that never comes is waited for a few seconds past its
  // timeout; and an outcome another site may not share is tried again through
  // the redirector with the failed servers named. With it off, the open waits
  // for the client's request timeout, as before.
  const int readerT = readerOpenTimeoutS.load();
  std::optional<FailoverSeq> seq;
  if (fo)
    seq.emplace(readerT, openDeadline(readerT));
  std::string source = fillUrl, reached;
  OpenAnswer ans;
  // The handle shut down while this sequence ran (shutdownInner does not wait
  // for it): nobody will use what it opened, so an open File is closed, without
  // waiting, and left undestroyed, as shutdownInner left it.
  auto abandoned = [this](XrdCl::File* file, bool isOpen) {
    std::lock_guard<std::mutex> g(mu);
    if (!seqAbandoned)
      return false;
    if (isOpen) {
      const XRootDStatus cs = file->Close(nullptr, static_cast<ucache::XrdTimeout>(0));
      (void)cs;
    }
    innerOpened = true;
    innerOpenOk = false;
    return true;
  };
  // Case B: bounded synchronous retry of a TRANSIENT lazy open on a FRESH
  // inner file (a failed open is terminal). The sleeps run on this
  // caller/executor thread under innerOpenMu -- the deliberate asymmetry with
  // the reader's off-thread backoff: acceptable because this is the degraded/
  // miss path only and innerOpened latches, so a handle runs at most one
  // sequence. open_retries == 0 (default) skips the retries.
  Next n = Next::kDone;
  for (int sourceAttempt = 1;; ++sourceAttempt) {
    const std::string sent = faultOpenUrl(source, ++lazyOpenAttempt);
    ans = openAndWait(f, sent, a, seq ? seq->timeout() : static_cast<ucache::XrdTimeout>(0),
                      seq ? 2 * seq->trS : 0);
    reached = ans.hosts && !ans.hosts->empty() ? hostPortOf(ans.hosts->back().url) : std::string();
    const AttemptView w = viewOf(*this, ans.status, ans.hosts.get(), f);
    n = afterAttempt(*this, cfg, seq ? &*seq : nullptr, ans.status, w, sent, sourceAttempt, want);
    a.endUs = nowUs();
    // Whether this attempt ends the sequence, known before it is recorded: its last line
    // says how the open ended. Asking for the next listed copy waits, here.
    std::string listed;
    const bool end = n == Next::kDone || n == Next::kRefuse || processExiting() ||
                     (n == Next::kLocate && (listed = locateNext(*this, *seq, fillUrl)).empty());
    if (seq && end) {
      a.outcome = ans.status.IsOK() && n != Next::kRefuse ? "ok" : "error";
      a.budgetS = seq->budget;
    }
    noteAnswer(a, ans, sent, f, w);
    if (abandoned(f, ans.status.IsOK()))
      return false;
    if (end)
      break; // done, or the exit handler has begun: nothing more is issued
    if (n == Next::kSameSource) {
      std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs(sourceAttempt, cfg)));
      if (processExiting())
        break;
    }
    // A copy refused after it opened (on tape) is closed without waiting, in
    // the swap.
    XrdCl::File* fresh = resetInner(/*leakOld=*/ans.unfinished, /*onlyIfIdle=*/false,
                                    /*closeOld=*/ans.status.IsOK());
    if (!fresh)
      break; // the handle is shut down: the last answer stands
    f = fresh;
    ++a.attempt;
    if (n == Next::kElsewhere || n == Next::kLocate) {
      source = n == Next::kLocate ? listed : seq->next(*this, fillUrl);
      a.route = seq->onCandidate ? OpenRoute::kCandidate : OpenRoute::kFailover;
      sourceAttempt = 0;
    }
  }
  XRootDStatus s = ans.status;
  if (abandoned(f, s.IsOK()))
    return false;
  if (n == Next::kRefuse) // refused for what the copy is, and no other to try: the open fails
    s = XRootDStatus(XrdCl::stError, XrdCl::errDataError, 0, "the copy is refused");
  if (s.IsOK() && want) {
    XrdCl::StatInfo* si = nullptr;
    const XRootDStatus ss = f->Stat(false, si);
    std::unique_ptr<XrdCl::StatInfo> sip(si);
    if (ss.IsOK() && si && si->GetSize() != want) {
      std::string server;
      f->GetProperty("DataServer", server);
      // The message goes to the log and back to the reader as its error, so it
      // names the file without the URL's query, which can carry an access token.
      const std::string shown = url.substr(0, url.find('?'));
      char msg[512];
      std::snprintf(msg, sizeof msg,
                    "%s: %s serves %llu bytes, the cached file has %llu; not mixing them",
                    shown.c_str(), server.empty() ? "the origin" : server.c_str(),
                    static_cast<unsigned long long>(si->GetSize()),
                    static_cast<unsigned long long>(want));
      UCACHE_WARN("%s (the read fails; the next open checks the origin again)", msg);
      if (store)
        store->stats().sizeDisagreements.fetch_add(1, std::memory_order_relaxed);
      if (fo)
        noteSizeDisagreement(*this, reached.empty() ? server : reached, si->GetSize(), want);
      if (!trustMarker.empty())
        RealIO::instance().unlink(trustMarker);
      lazyOpenError = XRootDStatus(XrdCl::stError, XrdCl::errDataError, 0, msg);
      lazyOpenFailUs = nowUs();
      innerOpenOk = false;
      sizeRefused = true; // before the latch: acquireInner reads them in this order
      innerOpened = true; // latch: the same source would answer the same
      // Let go of the refused source's open file: closed now without waiting,
      // the asynchronous close its destructor made, but kept until the handle
      // closes (a call may still be in it).
      if (resetInner(/*leakOld=*/false, /*onlyIfIdle=*/true)) {
        const XRootDStatus cs = f->Close(nullptr, static_cast<ucache::XrdTimeout>(0));
        (void)cs;
      }
      return false;
    }
  }
  innerOpenOk = s.IsOK();
  if (innerOpenOk) {
    innerOpened = true; // success latches; failure only arms the cool-down
    lazyOpenFailUs = 0;
    applyReadPolicy();
    if (seq)
      seq->recovered(*this, reached, a.attempt);
  } else {
    lazyOpenError = s;
    lazyOpenFailUs = nowUs();
    // A failed open is TERMINAL on the object: install a fresh file
    // now so the post-cool-down re-attempt doesn't replay the cached error.
    resetInner(/*leakOld=*/ans.unfinished, /*onlyIfIdle=*/true, /*closeOld=*/n == Next::kRefuse);
    // Named by its cache key, which carries no query (a query can carry an
    // access token). Other log lines that name the reader's URL print it whole.
    const auto key = UrlKey::parse(url, cfg.keepCgi);
    const std::string file = key ? key->key : url.substr(0, url.find('?'));
    if (seq && !seq->failed.empty()) // it failed over, or a bound stopped it
      lazyOpenError = seq->finalError(*this, file, s);
    else
      UCACHE_WARN("%s: opening it at %s failed (%s); reads that need the origin fail with that "
                  "error, and the next one tries again in a second",
                  file.c_str(), reached.empty() ? "the origin" : reached.c_str(),
                  s.ToString().c_str());
  }
  return innerOpenOk;
}

XrdCl::XRootDStatus HandleState::missError() {
  {
    std::lock_guard<std::mutex> g(mu);
    if (moveInFlight) // a move holds innerOpenMu for its whole budget, and has no open's error to give
      return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errInvalidOp);
  }
  std::lock_guard<std::mutex> g(innerOpenMu);
  if (lazyOpenFailUs && !lazyOpenError.IsOK())
    return lazyOpenError; // the origin's own error: retryable by smart clients
  return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errInvalidOp);
}

XrdCl::File* HandleState::acquireInner() {
  // Trusted-cache handle: open the origin on first real need (fail-open —
  // nullptr here makes the miss path return an error, never bad data).
  if (cacheOnly && !innerOpened && !ensureInnerOpen())
    return nullptr;
  if (sizeRefused.load(std::memory_order_acquire))
    return nullptr; // the origin serves another size of this file: never read it here
  std::lock_guard<std::mutex> g(mu);
  if (!innerValid)
    return nullptr;
  ++innerOps;
  return inner;
}

XrdCl::File* HandleState::acquireInnerIfOpen() {
  if (cacheOnly && !innerOpened)
    return nullptr; // would have to open the origin; the caller is speculative
  if (sizeRefused.load(std::memory_order_acquire))
    return nullptr;
  std::lock_guard<std::mutex> g(mu);
  if (!innerValid || !inner.load()->IsOpen())
    return nullptr;
  ++innerOps;
  return inner;
}

void HandleState::releaseInner() {
  std::lock_guard<std::mutex> g(mu);
  if (innerOps <= 0) { // a request is given back once, by the code that took it: counted, never an abort
    if (store)
      store->stats().failopenEvents.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (--innerOps == 0)
    innerCv.notify_all();
}

// The client's own recovery of a request, off: the failures it would recover are
// moved instead (moveOn). A File opened with no flags (what pyxrootd passes when it
// is given none) is not read-only to the client, which then recovers by its write
// setting, so both are turned off.
static void recoveryOff(XrdCl::File& f) {
  f.SetProperty("ReadRecovery", "false");
  f.SetProperty("WriteRecovery", "false");
}

// Called where the handle is set up, and where one of the cache's own opens of
// the origin has succeeded (either may come first). Arms the handle for moving
// its reads (OpenPolicy.h armedForReads) and, from then on, the client's own
// recovery is off on every File of it. Never disarms. On a non-callback thread:
// SetProperty takes the File's own lock.
void HandleState::applyReadPolicy() {
  if (!globalConfig().adaptiveFederation)
    return;
  XrdCl::File* f = nullptr;
  {
    std::lock_guard<std::mutex> g(mu);
    // A copy's one read of the whole file would be repeated, in full, at the next site, and its
    // deadline is not a read's.
    if (moveOnReads || !innerValid || !setupDone || wholeCopy ||
        !armedForReads(true, reopenInChild, viaRedirector, cold != nullptr))
      return;
    moveOnReads = true;
    f = inner.load(); // a File that is replaced meanwhile is kept, and the new one is made armed
  }
  recoveryOff(*f);
}

void HandleState::unpark() {
  std::lock_guard<std::mutex> g(mu);
  if (--parkedReads == 0)
    innerCv.notify_all();
}

ucache::XrdTimeout HandleState::originTimeout(int readerTimeoutS, uint64_t startUs, bool again) {
  if (!moveOnReads.load(std::memory_order_relaxed))
    return static_cast<ucache::XrdTimeout>(readerTimeoutS);
  const int elapsedS = startUs ? static_cast<int>((nowUs() - startUs) / 1000000) : 0;
  return static_cast<ucache::XrdTimeout>(readAttemptTimeoutS(
      readerTimeoutS, defaultRequestTimeoutS(), gReadDeadlineS, elapsedS, again));
}

// A forked child's first cache work -- a setup, a relayed byte, an open --
// gives it a record of its own and re-arms its periodic checkpoint, together;
// a child that only destroys a handle it inherited does neither. One atomic
// load each in a process that never forked.
static void childWork(const std::shared_ptr<HandleState>& st) {
  resumeAfterFork();
  if (st->store)
    st->store->ensureOwnIdentity();
}

// Pass-through relay to the origin for ops the cache never serves (PgRead,
// legacy/beyond-EOF geometries): acquire the inner FIRST — on a trusted-cache
// handle (UCACHE_REVALIDATE_S) that lazy-opens the origin. A never-opened
// XrdCl::File must never receive I/O: 5.8.3's PgRead SEGVs probing the
// transport of a channel with no URL (found by plugin_fuzz the moment
// the freshness window became the default). The inner is released when the op
// completes; if no usable inner exists (lazy open failed, teardown) the op
// fails cleanly with errInvalidOp instead.
// Bytes served by pure pass-through — the cache never touched them.
// Counted at issue time (a failed relay overcounts a little; the counter is
// a tier-share indicator, not an accounting invariant).
static void noteRelayBytes(const std::shared_ptr<HandleState>& st, uint64_t n,
                           uint64_t off = 0, uint64_t len = 0) {
  (void)off;
  (void)len; // the footprint is recorded once, at the entry point
  if (st->store && n) {
    childWork(st);
    st->store->stats().relayBytes.fetch_add(n, std::memory_order_relaxed);
    st->relayedBytes.fetch_add(n, std::memory_order_relaxed);
    const uint64_t t = nowUs();
    uint64_t expected = 0;
    st->relayFirstUs.compare_exchange_strong(expected, t, std::memory_order_relaxed);
    st->relayLastUs.store(t, std::memory_order_relaxed);
  }
}

// What the APPLICATION asked for, in origin coordinates -- recorded once, at
// the entry point, for every route. Recording it deeper would record what each
// route CHOSE to move instead: a fill rounds and coalesces its fetches, the
// byte tier reads exactly the request, and a replica addresses a rewritten
// container. Those are three different byte sets for one analysis, and hashing
// them would say the same work was different work.
// Bytes a file read directly (max_read_fraction) fetched and did not keep:
// relayed, as pass-through bytes are, and counted as read directly.
static void noteDirectBytes(const std::shared_ptr<HandleState>& st,
                            const std::shared_ptr<FileEntry>& entry, uint64_t n) {
  if (!n)
    return;
  noteRelayBytes(st, n);
  if (st->store)
    st->store->stats().directReadBytes.fetch_add(n, std::memory_order_relaxed);
  if (entry)
    entry->obs().directBytes.fetch_add(n, std::memory_order_relaxed);
}

// max_read_fraction, on the plain path: a request that needs the origin asks
// the file's rule, which decides once, at the first such request that reads
// two or more branches (ReadRule.h). The rule of a file read directly, else
// null.
// (A build without the tree parser has no rule.)
#ifdef UCACHE_HAVE_COLDRUN
static std::shared_ptr<ReadRule> directRule(const std::shared_ptr<HandleState>& st,
                                            const std::shared_ptr<FileEntry>& entry,
                                            const XrdCl::ChunkList& chunks) {
  std::shared_ptr<ReadRule> rule;
  {
    std::lock_guard<std::mutex> g(st->mu);
    rule = st->rule;
  }
  if (!rule)
    return nullptr;
  if (rule->state() == ReadRule::kUndecided) {
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    ranges.reserve(chunks.size());
    for (const auto& c : chunks)
      ranges.emplace_back(c.offset, c.length);
    OriginSource src;
    src.st = st;
    src.entry = entry;
    rule->observe(ranges, src);
  }
  return rule->direct() ? rule : nullptr;
}
// Does [off, off + len) read the data of a file read directly?
static bool ruleReadsData(const std::shared_ptr<ReadRule>& r, uint64_t off, uint64_t len) {
  return r && r->readsData(off, len);
}
// check_origin_integrity on a byte-cache miss: each basket that starts in the
// fetched bytes must say in its key that it sits where the tree's record puts
// it (transpose::firstMisplacedKey). The handle's table of basket positions is
// set on the thread that issues the miss, before the miss, by a parse whose
// reads carry the miss's own timeout; the miss's completion reads it (null:
// not checked). A file whose tree does not parse has an empty one. (A build
// without the tree parser checks nothing.)
static std::shared_ptr<const std::vector<uint64_t>> keySeeksOf(HandleState& st) {
  std::lock_guard<std::mutex> g(st.mu);
  return st.keySeeks;
}
static void armKeyCheck(const std::shared_ptr<HandleState>& st,
                        const std::shared_ptr<FileEntry>& entry, XrdTimeout timeout) {
  if (!globalConfig().checkOriginIntegrity || keySeeksOf(*st))
    return;
  auto seeks = keySeeksFor(st, entry, timeout);
  std::lock_guard<std::mutex> g(st->mu);
  st->keySeeks = std::move(seeks);
}
#else
static std::shared_ptr<ReadRule> directRule(const std::shared_ptr<HandleState>&,
                                            const std::shared_ptr<FileEntry>&,
                                            const XrdCl::ChunkList&) {
  return nullptr;
}
static bool ruleReadsData(const std::shared_ptr<ReadRule>&, uint64_t, uint64_t) { return false; }
static void armKeyCheck(const std::shared_ptr<HandleState>&, const std::shared_ptr<FileEntry>&,
                        XrdTimeout) {}
#endif

static void noteAppRead(const std::shared_ptr<HandleState>& st,
                        const std::shared_ptr<FileEntry>& entry, uint64_t off,
                        uint64_t len, const std::shared_ptr<ColdFill>& cold = nullptr) {
  if (!len)
    return;
#ifdef UCACHE_HAVE_COLDRUN
  if (entry && cold) {
    // The cold run's layout maps back through its slot table: a slot is its
    // basket, the rest is the original file or the original tree record. The
    // buckets take every one the read touched, whole; the byte counts only
    // what it read of them exactly, or nothing, for good, when it read part
    // of a slot (ReadFootprint::noteMapped).
    widthSampler().sample();
    std::vector<std::pair<uint64_t, uint64_t>> units, exact;
    coldOriginRanges(*cold, off, len, units, st->coldMap.get());
    const bool known = coldExactOriginRanges(*cold, off, len, exact, st->coldMap.get());
    entry->noteMappedRead(units, known ? &exact : nullptr);
    return;
  }
#else
  (void)cold;
#endif
  // Reads happen for the whole run, so this is where the parallelism ceiling
  // gets sampled; internally it is a load and a compare all but once a second.
  widthSampler().sample();
  if (!entry) {
    // Keyed by url, not by handle: the same file opened twice must accumulate
    // into one footprint or neither open matches anything.
    if (st->store)
      st->store->relayFootprint(st->url).note(off, len);
    return;
  }
  entry->noteRead(off, len);
}

// The chunks of one vector read. On a rewritten layout (the slot store),
// chunks that follow one another end to end are noted as the one span they
// make (forEachEndToEndRun): a basket a reader read in pieces is still read
// whole. The buckets come out the same either way; the byte counts stay exact
// where each piece alone would read part of the basket. Elsewhere every chunk
// is noted as it is.
static void noteAppChunks(const std::shared_ptr<HandleState>& st,
                          const std::shared_ptr<FileEntry>& entry,
                          const XrdCl::ChunkList& chunks, const std::shared_ptr<ColdFill>& cold) {
  if (!entry || !cold) {
    for (const auto& c : chunks)
      noteAppRead(st, entry, c.offset, c.length, cold);
    return;
  }
  forEachEndToEndRun(chunks,
                     [&](uint64_t off, uint64_t len) { noteAppRead(st, entry, off, len, cold); });
}

// A vector read's first offset and its bytes, for a failure's record.
static uint64_t chunksFirst(const XrdCl::ChunkList& chunks) {
  return chunks.empty() ? 0 : chunks.front().offset;
}
static uint64_t chunksBytes(const XrdCl::ChunkList& chunks) {
  uint64_t n = 0;
  for (const auto& c : chunks)
    n += c.length;
  return n;
}

// Vector relay: the byte total and the footprint come from the same walk.
static void noteRelayChunks(const std::shared_ptr<HandleState>& st,
                            const XrdCl::ChunkList& chunks) {
  uint64_t n = 0;
  for (const auto& c : chunks)
    n += c.length;
  noteRelayBytes(st, n);
}

// First of Close/dtor decides: a handle that relayed and never had an entry
// gets a per-file record now (FileEntry emits one only for entries).
// `entry` is the handle's entry as swapped out by the caller — non-null means
// FileEntry's own record covers this file and this one must not duplicate it.
static void emitRelayObs(const std::shared_ptr<HandleState>& st,
                         const std::shared_ptr<FileEntry>& entry) {
  if (st->relayObsDone.exchange(true))
    return;
  // A handle with an entry relays too (a file read directly, a fail-open): its
  // answer times are this file's, and go in the entry's record, which is
  // written only once the entry's last handle has let go of it.
  if (entry) {
    entry->obs().originRtUs.addFrom(st->relayRtUs);
    entry->obs().servers.addFrom(st->relayServers);
  }
  const uint64_t n = st->relayedBytes.load(std::memory_order_relaxed);
  if (!entry && st->store && n) {
    const uint64_t a = st->relayFirstUs.load(std::memory_order_relaxed);
    const uint64_t b = nowUs(); // the handle is closing: this IS its last activity
    // The file's size at the origin, which is the one figure that means the
    // same thing on both routes. force=false answers from the open response,
    // so this costs no round trip in the common case.
    //
    // This DOES run from the destructor, and a blocking call on a dead
    // connection stalls teardown once per relayed file. Suppressing it there
    // was tried and reverted: the size does not only weight originShare(), it
    // GATES the read signature -- CacheStore's relay record emits read_sig and
    // read_buckets only when the size is known -- and a relayed handle is what
    // a cache-disabled BASELINE leaves behind. Losing it makes every later
    // comparison against that baseline unverified, and with no signed pairs at
    // all the agreement rule is skipped entirely, so a genuinely
    // different-work comparison stops being refused. That is a worse failure
    // than a slow exit.
    //
    // It also bought less than it looked: shutdownInner() below destroys the
    // inner File, whose destructor runs a synchronous Close, so this teardown
    // blocks on a dead origin either way. Removing the Stat removed one of two
    // blocking calls and paid for it with the evidence.
    // Prefer the copy taken at open: it costs nothing and, unlike the inner
    // file, it cannot be swapped out from under this by a retry. Asking the
    // inner file is the fallback, and it goes through acquire/release like
    // every other use of it -- reading `inner` raw here was a race with
    // resetInner, and this runs from the DESTRUCTOR, where a blocking call on
    // a dead connection stalls teardown once per relayed file.
    uint64_t originSize = 0;
    if (st->statInfo) {
      originSize = st->statInfo->GetSize();
    } else if (XrdCl::File* f = st->acquireInner()) {
      if (f->IsOpen()) {
        XrdCl::StatInfo* si = nullptr;
        if (f->Stat(false, si).IsOK() && si)
          originSize = si->GetSize();
        delete si;
      }
      st->releaseInner();
    }
    st->store->recordRelayObs(st->url, n, "relay", b > a ? b - a : 0, originSize,
                              &st->relayRtUs, &st->relayServers);
  }
}

// Request shape as the CLIENT asked for it, before the cache decides how to
// serve it: one sample per Read and per vector-read chunk. Read against the
// physical read-size histograms, this is what shows whether reads arrive
// basket-sized or page-sized.
static void noteRequestBytes(const std::shared_ptr<HandleState>& st, uint64_t n) {
  if (st->store)
    st->store->stats().reqReadBytes.add(n);
}

// One cache-handled vector read: its width, its chunks, and their shapes.
static void noteVectorRequest(const std::shared_ptr<HandleState>& st,
                              const XrdCl::ChunkList& chunks) {
  if (!st->store)
    return;
  auto& s = st->store->stats();
  s.readvCalls.fetch_add(1, std::memory_order_relaxed);
  s.readvChunks.fetch_add(chunks.size(), std::memory_order_relaxed);
  for (const auto& c : chunks)
    s.reqReadBytes.add(c.length);
}

// ---- Moving the reads of a handle to another site --------------------------
// (adaptive_federation on, an armed handle: OpenPolicy.h armedForReads.) A read
// that fails in a way another site may not share (readStep) waits, with its
// reader's handler held, for the handle's origin file to be opened at another
// site, and is issued again there in full. One move serves every read that fails
// while it is made.

// How a request is issued on a File of the handle: the call it was made with.
using ReadAgain = std::function<XRootDStatus(XrdCl::File*, ResponseHandler*, ucache::XrdTimeout)>;

// Opens the origin file of the handle at another site and, when that succeeds,
// puts it in place of the one reads failed on (the old one is kept until the
// handle closes: a call may still be in it). The cache's own open sequence
// (FailoverSeq, as ensureInnerOpen runs it) on Files that no reader can reach: a
// File that is not open yet answers a read with an error nothing recovers. The
// servers reads failed on (`failed`) are named in tried= and taken last; the
// source must have the size the reader was shown (`want`); `capS`: what was left of
// the time of the request that failed, `failUs` when it failed (the move's time
// starts there, not when it gets a thread). `to`: where it was opened. Blocks, with
// innerOpenMu held by the caller. Nothing is changed when the handle closed
// meanwhile.
static bool moveInner(HandleState& st, std::vector<SiteFailure> failed, uint64_t want, int capS,
                      uint64_t failUs, std::string& to) {
  Config cfg = globalConfig();
  cfg.openRetries = 0; // a failed attempt goes on to another site, within the sequence's bounds
  const int readerT = st.readerOpenTimeoutS.load();
  FailoverSeq seq(readerT, openDeadline(readerT));
  seq.t0 = failUs;
  seq.read = true;
  seq.failed = std::move(failed);
  seq.budget = std::min(seq.budget, std::max(capS, 1));
  const std::string base =
      st.innerOpened ? ownOpenUrl(st.url, failedHostsOf(st)) : mergeTried(st.url);
  // A File is made outside mu (its constructor takes the client's fork lock, which a
  // thread that forks holds while it waits for threads that wait for mu). The reader's
  // properties set after the copy taken here are given to it where it is adopted.
  size_t props = 0;
  const auto fresh = [&st, &props] {
    std::vector<std::pair<std::string, std::string>> mine;
    {
      std::lock_guard<std::mutex> g(st.mu);
      mine = st.readerProps;
    }
    props = mine.size();
    auto f = std::make_unique<XrdCl::File>(false); // plugins disabled (no recursion)
    for (const auto& p : mine)
      f->SetProperty(p.first, p.second);
    recoveryOff(*f); // after the reader's: it cannot turn it back on
    return f;
  };
  OpenAttempt a;
  a.st = &st;
  a.route = OpenRoute::kRead;
  a.seq = gOpenSeq.fetch_add(1, std::memory_order_relaxed) + 1;
  std::string source = seq.next(st, base), listed;
  std::unique_ptr<XrdCl::File> f = fresh();
  OpenAnswer ans;
  AttemptView w;
  Next n = Next::kDone;
  bool exiting = false;
  for (;;) {
    ans = openAndWait(f.get(), source, a, seq.timeout(), 2 * seq.trS);
    w = viewOf(st, ans.status, ans.hosts.get(), f.get());
    n = afterAttempt(st, cfg, &seq, ans.status, w, source, 1, want);
    a.endUs = nowUs();
    const bool end = n == Next::kDone || n == Next::kRefuse || (exiting = processExiting()) ||
                     (n == Next::kLocate && (listed = locateNext(st, seq, base)).empty());
    if (end) { // the last line of the sequence says how it ended
      a.outcome = ans.status.IsOK() ? "ok" : "error";
      a.budgetS = seq.budget;
    }
    noteAnswer(a, ans, source, f.get(), w);
    if (end)
      break;
    if (ans.unfinished)
      (void)f.release(); // its open may be in flight still: left behind, undestroyed
    else
      f.reset();
    f = fresh();
    ++a.attempt;
    source = n == Next::kLocate ? listed : seq.next(st, base);
  }
  // A size the copy does not give is not the one the reader was shown. Nothing is
  // adopted at an exit: the answer is only the last one there was.
  const bool sizeOk = w.size >= 0 && static_cast<uint64_t>(w.size) == want;
  if (exiting || !ans.status.IsOK() || !sizeOk || n == Next::kRefuse) {
    if (ans.unfinished)
      (void)f.release();
    (void)seq.finalError(st, recordFile(st.url), ans.status); // counted, and said
    return false;
  }
  const SiteFailure at = siteFailure(w, ans.status, hostPortOfUrl(source));
  seq.recovered(st, at.server, a.attempt);
  std::lock_guard<std::mutex> g(st.mu);
  if (!st.innerValid || st.closed || st.seqAbandoned)
    return false;
  for (; props < st.readerProps.size(); ++props)
    f->SetProperty(st.readerProps[props].first, st.readerProps[props].second);
  recoveryOff(*f);
  st.retired.push_back(std::move(st.innerOwned));
  st.innerOwned = std::move(f);
  st.inner = st.innerOwned.get();
  st.dataServer = to = at.server;
  st.source = at;
  st.viaRedirector = st.viaRedirector || w.v.viaRedirector;
  return true;
}

// Runs on the move executor: the one move of the handle, for every read that waits
// for it. `why`: the failure that started it, `failUs` when it was and `capS` what was
// left of the time of its request then.
static void runMove(const std::shared_ptr<HandleState>& st, int capS, uint64_t failUs,
                    const std::string& why) {
  std::string from, to;
  bool adopted = false;
  {
    std::lock_guard<std::mutex> open(st->innerOpenMu);
    std::vector<SiteFailure> failed;
    uint64_t want = st->expectedOriginSize.load();
    bool live;
    {
      std::lock_guard<std::mutex> g(st->mu);
      live = st->innerValid && !st->closed; // the handle may have closed while this waited
      from = st->source.server;
      failed = st->readFailed;
      if (!want && st->entry)
        want = st->entry->fileSize();
      if (!want && st->statInfo)
        want = st->statInfo->GetSize();
    }
    // The size the reader was shown is what tells a source from a damaged one,
    // and a move that failed a moment ago is not made again at once.
    const bool cooling =
        st->lazyOpenFailUs && nowUs() - st->lazyOpenFailUs < kLazyOpenRetryCooldownUs;
    if (live && want && !cooling && !processExiting()) {
      try {
        adopted = moveInner(*st, std::move(failed), want, capS, failUs, to);
      } catch (...) {
      }
      st->lazyOpenFailUs = adopted ? 0 : nowUs();
    }
  }
  const std::string file = recordFile(st->url);
  if (adopted)
    UCACHE_WARN("%s: reads from %s stopped (%s); reading it from %s from now on", file.c_str(),
                from.c_str(), why.c_str(), to.c_str());
  else
    UCACHE_WARN("%s: reads from %s stopped (%s) and no other site could serve it", file.c_str(),
                from.c_str(), why.c_str());
  std::vector<HandleState::MoveWaiter> waiters;
  {
    std::lock_guard<std::mutex> g(st->mu);
    st->moveInFlight = false;
    waiters.swap(st->moveWaiters);
  }
  // Every read of the batch takes the File and leaves the parked count before any goes on:
  // a reader's handler that closes the handle then finds the others past it. Each then goes
  // on as a task of its own, so that a handler that blocks holds up no other read.
  std::vector<std::function<void()>> goOn;
  for (auto& w : waiters)
    goOn.push_back(w(adopted));
  for (auto& g : goOn)
    deliverExecutor().post(std::move(g));
}

// A read that was issued on `on` failed with `s`. Calls `cont(true)` when the
// handle's File is another by then (that read is to be issued again), or has
// become another, or `cont(false)` when no other site could serve it or the
// handle is going away. Starts the move when none is under way, and names the
// server as one that failed -- except for a session the client lost (109), which
// a healthy server loses at a restart. Never blocks and makes no File, so it may
// run on a client callback thread; `cont` runs on the deliver executor. `capS`: what is
// left of the failed request's time.
static void moveOn(const std::shared_ptr<HandleState>& st, XrdCl::File* on, const XRootDStatus& s,
                   int capS, HandleState::MoveWaiter cont) {
  std::unique_lock<std::mutex> lk(st->mu);
  ++st->parkedReads;
  const bool gone = !st->innerValid || st->closed;
  if (!gone && st->moveInFlight) {
    st->moveWaiters.push_back(std::move(cont));
    return;
  }
  if (gone || st->inner.load() != on) {
    lk.unlock();
    deliverExecutor().post([cont = std::move(cont), gone] { cont(!gone)(); });
    return;
  }
  if (s.code != XrdCl::errInvalidSession && !st->source.server.empty()) {
    st->readFailed.push_back(st->source);
    st->readFailed.back().error = s.ToString();
    if (st->readFailed.size() > kFailoverMaxSites - 1) // room for the move's own sites in its bound
      st->readFailed.erase(st->readFailed.begin());
  }
  st->moveInFlight = true;
  st->moveWaiters.push_back(std::move(cont));
  lk.unlock();
  moveExecutor().post([st, capS, failUs = nowUs(), why = recordText(s.ToString())] {
    runMove(st, capS, failUs, why);
  });
}

// Whether a request whose `attempt`-th attempt failed with `s`, begun `startUs`
// ago (`readerT`: its reader's timeout), moves. `checkFailed`: its bytes failed
// the integrity check, which moves it whatever the status says.
static bool movesRead(HandleState& st, const XRootDStatus& s, bool checkFailed, int attempt,
                      int readerT, uint64_t startUs) {
  return st.moveOnReads &&
         (checkFailed || readStep(s.code, static_cast<int>(s.errNo)) == ReadStep::kMove) &&
         attempt < kReadMaxAttempts && st.originTimeout(readerT, startUs, true) > 0;
}

// An issue call that returned `s` for the request handler `h`: when the client
// refused the request at once (a session it lost while the File was idle) and the
// request moves, the failure is posted to `h`, which treats it as one the client
// delivered, and OK is returned. Otherwise `s`, as it is.
static XRootDStatus refusedLater(const std::shared_ptr<HandleState>& st, XRootDStatus s,
                                 ResponseHandler* h) {
  if (s.IsOK() || !st->moveOnReads ||
      readStep(s.code, static_cast<int>(s.errNo)) != ReadStep::kMove)
    return s;
  Executor::instance().post(
      [h, s] { h->HandleResponseWithHosts(new XRootDStatus(s), nullptr, new HostList()); });
  return XRootDStatus();
}

namespace {
// check_origin_integrity's failed check, defined with the miss handlers below.
[[maybe_unused]] XRootDStatus* originCheckFailed(HandleState& st, ServerTally* tally,
                                                 const char* op, uint64_t pos, uint32_t n,
                                                 uint64_t spanOff, uint64_t spanLen,
                                                 const std::string& server);
} // namespace

// The one handler that hands a pass-through completion back to the caller.
// Every fail-open route uses it. It exists as one type because it did not:
// three routes carried their own copy of these fifteen lines, and only one of
// the copies ever acquired the in-flight guard, so an origin read issued while
// failing open was invisible to the counter that exists to see it.
//
// It also times the read, from issue to completion: the origin's answer time on
// the route that bypasses the cache (hist_relay_rt_us, and the handle's own
// share for its per-file record). A successful answer only, as the cache's own
// fetches are timed; a clock reading on each side is all it adds, so what is
// relayed and when is unchanged.
//
// A relayed plain read of a file with no entry also teaches the file's
// footprint its size when the answer is SHORT: the origin gives fewer bytes
// than asked only at the end of the file, so the file ends there. The
// footprint then clips the requests that follow at once, where it could
// otherwise follow only kTail of those that run past the end. The answer is
// only looked at, never changed.
struct RelayHandler : XrdCl::ResponseHandler {
  std::shared_ptr<HandleState> st;
  XrdCl::ResponseHandler* user = nullptr;
  OriginInFlight inflight;
  uint64_t t0 = 0;      // when the read was issued
  uint64_t readLen = 0; // a plain read's length (Read, PgRead), else 0
  uint64_t off = 0, len = 0; // what was asked, for a failure's record
  // On an armed handle (moveOn): `again` issues the request on a File of the
  // handle, `on` is the File this attempt went to, and a failure that another
  // site may not share is not the reader's to see: the handler waits for the
  // handle to move and goes on with the next attempt (resume). The reader's
  // handler is called once, by the attempt that succeeds or, when no other site
  // serves the request, with what the last attempt answered (`last`, `lastHosts`).
  ReadAgain again;
  XrdCl::File* on = nullptr;
  int attempt = 1, readerT = 0; // readerT: the reader's own timeout, 0 for none
  uint64_t startUs = 0;         // when the request was first issued
  XRootDStatus last;
  std::unique_ptr<HostList> lastHosts;
  // This attempt goes to `f`: its read is counted in flight, and timed from now.
  void issueOn(XrdCl::File* f) {
    on = f;
    if (st->store) {
      inflight = OriginInFlight(&st->store->stats(), issueServer(*st, f));
      t0 = nowUs();
    }
  }
  // The attempt failed with `s` and the request moves: the handler waits for the
  // handle to move, and then is run again by resume and goOn. Nothing of it is touched
  // after moveOn, which may run them at once on another thread.
  void park(const XRootDStatus& s, HostList* h) {
    last = s;
    lastHosts.reset(h);
    const int capS = readBudgetS(readerT, defaultRequestTimeoutS()) -
                     static_cast<int>((nowUs() - startUs) / 1000000);
    moveOn(st, on, s, capS, [this](bool moved) { return resume(moved); });
  }
  // The handle moved, or did not, and this read takes the File of the handle (when it is
  // to be issued again) and leaves the parked count, together, so that a Close never finds
  // neither of them held. Returns the rest: it issues the read again, or answers it.
  std::function<void()> resume(bool moved) {
    const ucache::XrdTimeout t =
        moved && !processExiting() ? st->originTimeout(readerT, startUs, true) : 0;
    XrdCl::File* f = t ? st->acquireInner() : nullptr;
    st->unpark();
    return [this, f, t] { goOn(f, t); };
  }
  void goOn(XrdCl::File* f, ucache::XrdTimeout t) {
    if (f) {
      issueOn(f);
      ++attempt;
      const ReadAgain issue = again; // once issued, this handler may be gone at any time
      const std::shared_ptr<HandleState> keep = st;
      const XRootDStatus s = refusedLater(keep, issue(f, this, t), this);
      if (s.IsOK())
        return;
      st->releaseInner(); // never issued: the failure is the answer
      inflight.release();
      last = s;
      lastHosts.reset();
    }
    XrdCl::HostList* h = lastHosts ? lastHosts.release() : new HostList();
    if (user)
      user->HandleResponseWithHosts(new XRootDStatus(last), nullptr, h);
    else
      delete h;
    delete this;
  }
  // The bytes an answer carries: a plain read's, a page read's or a vector
  // read's; 0 for anything else.
  static uint64_t answerBytes(XrdCl::AnyObject* r) {
    if (!r)
      return 0;
    XrdCl::ChunkInfo* ci = nullptr;
    r->Get(ci); // checked against the type held: null for any other answer
    if (ci)
      return ci->length;
    XrdCl::PageInfo* pi = nullptr;
    r->Get(pi);
    if (pi)
      return pi->GetLength();
    XrdCl::VectorReadInfo* vi = nullptr;
    r->Get(vi);
    return vi ? vi->GetSize() : 0;
  }
  void learnSizeFrom(XrdCl::AnyObject* r) {
    uint64_t off = 0, got = 0;
    XrdCl::ChunkInfo* ci = nullptr;
    r->Get(ci); // checked against the type held: null for any other answer
    if (ci) {
      off = ci->offset;
      got = ci->length;
    } else {
      XrdCl::PageInfo* pi = nullptr;
      r->Get(pi);
      if (!pi)
        return;
      off = pi->GetOffset();
      got = pi->GetLength();
    }
    if (got >= readLen)
      return; // not short: the end of the file is further on
    try {
      if (ReadFootprint* fp = st->store->footprintFor(st->url))
        fp->learnSize(off + got);
    } catch (...) {
      // a callback of the client library: nothing may leave it
    }
  }
#ifdef UCACHE_HAVE_COLDRUN
  // The answer of a request that was moved, when its bytes fail the key-position
  // check (check_origin_integrity), as a miss's do: the status the reader would
  // get, null when they pass. Only moved requests are checked here, with the
  // handle's table of basket positions when it holds one: a second damaged copy
  // must not reach the reader in the case where damage is known.
  XRootDStatus* misplaced(XrdCl::AnyObject* r, const HostList* h) {
    const auto seeks = keySeeksOf(*st);
    if (!seeks || !r)
      return nullptr;
    transpose::MisplacedKeys bad;
    const auto look = [&](uint64_t o, const void* p, uint64_t n) {
      const auto m = transpose::firstMisplacedKey(*seeks, o, p, n, o, o + n);
      bad.first = bad.count ? bad.first : m.first;
      bad.count += m.count;
    };
    XrdCl::ChunkInfo* ci = nullptr;
    r->Get(ci);
    if (ci)
      look(ci->offset, ci->buffer, ci->length);
    XrdCl::VectorReadInfo* vi = nullptr;
    r->Get(vi);
    if (vi)
      for (const auto& c : vi->GetChunks())
        look(c.offset, c.buffer, c.length);
    return bad.count ? originCheckFailed(*st, &st->relayServers, vi ? "readv" : "read", bad.first,
                                         bad.count, off, len, answeredBy(*st, h))
                     : nullptr;
  }
#endif
  void HandleResponseWithHosts(XrdCl::XRootDStatus* s, XrdCl::AnyObject* r,
                               XrdCl::HostList* h) override {
    bool checkFailed = false;
#ifdef UCACHE_HAVE_COLDRUN
    if (attempt > 1 && s && s->IsOK())
      if (XRootDStatus* refused = misplaced(r, h)) {
        delete s;
        delete r;
        s = refused;
        r = nullptr;
        checkFailed = true;
      }
#endif
    // Timed before the inner file is given back, so a Close that waits for
    // this read finds its time already on the handle.
    if (st->store && t0 && s && s->IsOK()) {
      const uint64_t rt = nowUs() - t0;
      st->store->stats().relayRtUs.add(rt);
      st->relayRtUs.add(rt);
      st->relayServers.add(answeredBy(*st, h), answerBytes(r), rt, inflight.nc);
      if (readLen && r)
        learnSizeFrom(r);
    } else if (st->store && s && !s->IsOK() && !checkFailed) {
      // A pass-through handle's open is not watched, so it knows no server
      // of its own: then the server its reads went to is named.
      std::string server = answeredBy(*st, h);
      if (server.empty())
        server = st->relayServers.topServer();
      noteOriginFailure(*st, &st->relayServers, "relay", off, len, *s, server);
    }
    st->releaseInner();
    inflight.release();
    if (again && s && !s->IsOK() && movesRead(*st, *s, checkFailed, attempt, readerT, startUs)) {
      park(*s, h);
      delete s;
      delete r;
      return;
    }
    if (user)
      user->HandleResponseWithHosts(s, r, h);
    else {
      delete s;
      delete r;
      delete h;
    }
    delete this;
  }
};

// Every relay is issued the same way: take the guard as the request goes out,
// so it spans the read rather than the handler's lifetime. `f`: the file the
// request goes out on, whose server it is counted against.
static RelayHandler* newRelay(const std::shared_ptr<HandleState>& st, XrdCl::File* f,
                              XrdCl::ResponseHandler* user, uint64_t off = 0, uint64_t len = 0) {
  auto* relay = new RelayHandler;
  relay->st = st;
  relay->user = user;
  relay->off = off;
  relay->len = len;
  relay->issueOn(f);
  return relay;
}

// The request of a miss whose wire read failed with `s`, as a relay that moves
// it (`again` issues it on a File of the handle): true when it does, and the
// relay owns `h` and what is left to do. False, with nothing taken, when the
// request does not move: the caller fails open as before. `on`: the File the
// miss was issued on, `startUs` when it was.
static bool moveRead(const std::shared_ptr<HandleState>& st, XrdCl::File* on, ResponseHandler* user,
                     uint64_t off, uint64_t len, int readerT, uint64_t startUs, ReadAgain again,
                     const XRootDStatus& s, std::unique_ptr<HostList>& h, bool checkFailed) {
  if (!movesRead(*st, s, checkFailed, 1, readerT, startUs))
    return false;
  auto* relay = new RelayHandler;
  relay->st = st;
  relay->user = user;
  relay->off = off;
  relay->len = len;
  relay->on = on;
  relay->again = std::move(again);
  relay->readerT = readerT;
  relay->startUs = startUs;
  relay->park(s, h.release());
  return true;
}

// A vector read relayed on an armed handle may be issued again after the call
// has returned, so its chunk list is kept (the reader's goes with the call);
// null on any other, which is issued once, in the call.
static std::shared_ptr<const ChunkList> keepChunks(const HandleState& st, const ChunkList& chunks) {
  return st.moveOnReads ? std::make_shared<const ChunkList>(chunks) : nullptr;
}

// `readLen`: the length of a plain read (Read, PgRead) of a handle with no
// entry, whose short answer tells its footprint where the file ends; 0 for
// anything else. `off` and `len`: what is asked, for a failure's record.
// `issue`: the call, given a File, the handler and the timeout. `reissuable`: it
// may be made again after this call returned (a vector read's chunk list is kept
// for that: keepChunks).
template <typename Issue>
static XrdCl::XRootDStatus relayToInner(const std::shared_ptr<HandleState>& st,
                                        XrdCl::ResponseHandler* user, int readerT, Issue issue,
                                        uint64_t readLen = 0, uint64_t off = 0, uint64_t len = 0,
                                        bool reissuable = true) {
  noteWaitTier(user, ReaderWait::kOrigin);
  if (XrdCl::File* f = st->acquireInner()) {
    auto* relay = newRelay(st, f, user, off, len);
    relay->readLen = readLen;
    if (reissuable && st->moveOnReads) {
      relay->again = issue;
      relay->readerT = readerT;
      relay->startUs = nowUs();
    }
    XrdCl::XRootDStatus s;
    if (!injectReadFault(true, relay, s))
      s = issue(f, static_cast<XrdCl::ResponseHandler*>(relay), st->originTimeout(readerT));
    s = refusedLater(st, s, relay);
    if (!s.IsOK()) {
      delete relay;
      st->releaseInner();
    }
    return s;
  }
  return st->missError(); // the origin's own error when the lazy open failed
}

void HandleState::startOverAfterFork(uint64_t gen) {
  uint64_t claim = forkClaim.load(std::memory_order_acquire);
  if (claim == gen || !forkClaim.compare_exchange_strong(claim, gen, std::memory_order_acq_rel)) {
    while (forkGen.load(std::memory_order_acquire) != gen) // another thread is at it
      std::this_thread::yield();
    return;
  }
  // Only this thread touches the state now: every other entry point waits above.
  new (&mu) std::mutex;
  new (&setupMu) std::mutex;
  new (&innerOpenMu) std::mutex;
  new (&persistMu) std::mutex;
  new (&innerCv) std::condition_variable;
  new (&persistCv) std::condition_variable;
  copyGuard.afterForkChild();
  innerOps = 0;        // requests XrdCl dropped in this child
  pendingPersists = 0; // tasks queued on the parent's pool, which never run here
  moveInFlight = false; // a move is the parent's: its reads wait there, and are left behind
  parkedReads = 0;
  new (&moveWaiters) std::vector<MoveWaiter>;
  new (&source) SiteFailure; // written under mu by callback threads: a fork may have caught them mid-write
  new (&readFailed) std::vector<SiteFailure>;
  // The layout its reader was shown, unless the handle has not been set up
  // since an earlier fork (a grandchild forked while a child was at it): then
  // that one is still the one to keep, and so is a layout lost for good.
  if (forkLayout == ForkLayout::kNone && setupDone && !closed && !tripped && !layoutLost)
    forkLayout = cold ? ForkLayout::kCold : ForkLayout::kOriginal;
  // The inner file is the parent's open. XrdCl recovers such a file at its
  // first use in the child, and that recovery does not survive vector reads
  // issued at once -- the reader's threads and the cache's own. A handle the
  // cache serves opens the file afresh instead, once, at its first need of the
  // origin (ensureInnerOpen, under innerOpenMu), and the parent's File is left
  // behind undestroyed; a lazy open in flight at the fork is replaced the same
  // way. A handle that only relays (write, disabled, a copy tool's) keeps
  // XrdCl's own behaviour.
  if ((reopenInChild || (cacheOnly && !innerOpened)) && !closed) {
    innerStale = true;
    innerOpened = false;
    cacheOnly = true; // opened on the first miss, as a trusted handle is
  }
  struct LeftBehind {
    std::shared_ptr<FileEntry> entry;
    std::shared_ptr<ColdFill> cold;
    std::shared_ptr<ReadRule> rule;
    std::unique_ptr<XrdCl::StatInfo> statInfo;
    std::vector<std::unique_ptr<XrdCl::File>> retired;
  };
  new LeftBehind{std::move(entry), std::move(cold), std::move(rule), std::move(statInfo),
                 std::move(retired)}; // leaked on purpose: the parent's
  setupDone = closed || tripped || layoutLost; // nothing to set up again
  prefetchSeen.store(false, std::memory_order_relaxed);
  // This process's own account of the handle starts here.
  relayedBytes.store(0, std::memory_order_relaxed);
  relayFirstUs.store(0, std::memory_order_relaxed);
  relayLastUs.store(0, std::memory_order_relaxed);
  relayRtUs.clear();
  new (&relayServers) ServerTally; // its lock may be held by a thread left in the parent
  new (&dataServer) std::string;
  cpu0Us = 0;
  cpuBlended = true; // a span that began in the parent cannot be attributed here
  forkGen.store(gen, std::memory_order_release);
  // No record is claimed here: this may be the child's destructor of a handle
  // it never used. The first real work claims one (childWork).
}

bool HandleState::waitParked(std::chrono::milliseconds max) {
  std::unique_lock<std::mutex> lk(mu);
  return innerCv.wait_for(lk, max, [this] { return parkedReads == 0; });
}

bool HandleState::waitInnerIdle(std::chrono::milliseconds max) {
  std::unique_lock<std::mutex> lk(mu);
  return innerCv.wait_for(lk, max, [this] { return innerOps == 0 && parkedReads == 0; });
}

void HandleState::shutdownInner(bool leak) {
  // Runs only from ~UCacheFile: no call of the reader's into this handle can
  // still be live, so this is where the replaced Files can go too. (A lazy
  // open the cache itself is running on another thread is not waited for
  // here; that holds for the current File as much as for the replaced ones.
  // With adaptive_federation on, the current File is then left undestroyed:
  // that open is waiting for its answer.)
  std::unique_ptr<XrdCl::File> dead;
  std::vector<std::unique_ptr<XrdCl::File>> old;
  std::unique_lock<std::mutex> opening(innerOpenMu, std::defer_lock);
  const bool openLive = globalConfig().adaptiveFederation && !opening.try_lock();
  if (opening.owns_lock())
    opening.unlock();
  {
    std::unique_lock<std::mutex> lk(mu);
    innerValid = false;
    seqAbandoned = openLive;
    innerCv.wait(lk, [this] { return innerOps == 0; });
    inner = nullptr;
    dead = std::move(innerOwned);
    old.swap(retired);
  }
  if (leak || openLive)
    (void)dead.release();
  // The File dtors (an asynchronous Close of an open one) run here with mu
  // released; the replaced ones failed or were closed already.
}

XrdCl::File* HandleState::resetInner(bool leakOld, bool onlyIfIdle, bool closeOld) {
  // A failed open is terminal on the object, so a retry needs a new File. The
  // old one is kept, not destroyed: a call that read `inner` without the lock
  // may still be inside it. The validity check sits in the same section as the
  // swap, so a handle shut down meanwhile gets no File that nothing destroys.
  std::lock_guard<std::mutex> g(mu);
  if (!innerValid || (onlyIfIdle && innerOps != 0))
    return nullptr;
  if (leakOld) {
    (void)innerOwned.release(); // the parent's file, which calls without the lock may still use
  } else {
    if (closeOld) {
      const XRootDStatus cs = innerOwned->Close(nullptr, static_cast<ucache::XrdTimeout>(0));
      (void)cs; // only queued: the close is not waited for
    }
    retired.push_back(std::move(innerOwned));
  }
  innerOwned = makeInner();
  inner = innerOwned.get();
  return innerOwned.get();
}

std::unique_ptr<XrdCl::File> HandleState::makeInner() {
  auto f = std::make_unique<XrdCl::File>(false); // plugins disabled (no recursion)
  for (const auto& p : readerProps)              // the reader's settings, in the order it made them
    f->SetProperty(p.first, p.second);
  if (moveOnReads)
    recoveryOff(*f); // after the reader's: it cannot turn it back on
  return f;
}

bool HandleState::reopenPending() {
  // With adaptive_federation on this does not wait: innerOpenMu is held by a move, or
  // by an open of the origin that can take its whole budget, and neither is waited for
  // by a call that only asks, nor by a Close or a handle's destruction (a handle closed
  // as its process exits must not hold it up for an origin that does not answer; that
  // open's File is then left undestroyed). A caller that needs the open waits in
  // ensureInnerOpen. With it off, an open of the origin another thread is making is
  // waited for, as it always was.
  if (globalConfig().adaptiveFederation)
    return innerStale.load(std::memory_order_acquire);
  std::lock_guard<std::mutex> g(innerOpenMu);
  return innerStale;
}

void HandleState::noteCacheError(const Config& cfg) {
  std::lock_guard<std::mutex> g(mu);
  if (store)
    store->stats().failopenEvents.fetch_add(1, std::memory_order_relaxed);
  if (++errors >= cfg.maxErrors && !tripped) {
    tripped = true;
    if (store)
      store->stats().disabledHandles.fetch_add(1, std::memory_order_relaxed);
    UCACHE_WARN("handle for %s tripped after %d consecutive cache errors; "
                "pass-through from now on",
                url.c_str(), errors);
  }
}

void HandleState::noteCacheOk() {
  std::lock_guard<std::mutex> g(mu);
  errors = 0;
}

void HandleState::beginPersist() {
  std::lock_guard<std::mutex> g(persistMu);
  ++pendingPersists;
}

void HandleState::endPersist() {
  std::lock_guard<std::mutex> g(persistMu);
  if (--pendingPersists == 0)
    persistCv.notify_all();
}

void HandleState::drainPersists() {
  std::unique_lock<std::mutex> lk(persistMu);
  persistCv.wait(lk, [this] { return pendingPersists == 0; });
}

//--------------------------------------------------------------- handlers --

namespace {

// (Cache entry setup is lazy + synchronous on the first read — see
// UCacheFile::ensureEntry. Reads issued before an async setup completed used
// to fall through uncached; lazy sync setup on the caller thread closes that
// race by construction.)

void persistPagesReserved(const std::shared_ptr<HandleState>& st,
                          const std::shared_ptr<FileEntry>& entry, uint64_t off,
                          std::shared_ptr<std::vector<char>> buf, uint64_t len,
                          uint64_t gateLen = 0) {
  Executor::instance().post([st, entry, off, buf, len, gateLen] {
    entry->writePages(off, len, buf->data());
    entry->flushMeta(false);
    if (gateLen) // waiters re-dispatch only AFTER the pages are staged
      entry->endFetch(off, gateLen);
    st->endPersist(); // wakes a draining Close/dtor when the last one lands
  });
}

// A miss whose bytes failed the key-position check: reported with the server
// that sent them, counted, and the read fails (or moves, on an armed handle).
// `tally`: the file's account of its servers. Nothing of the fetch is kept
// or served, and it is not relayed to the origin either (that would serve the
// same bytes). The record carries the fetch's span, as the other fetch
// failures do; the message names the basket. Returns the status the reader
// gets. The fetch's bytes are not counted among the origin's.
[[maybe_unused]] XRootDStatus* originCheckFailed(HandleState& st, ServerTally* tally,
                                                 const char* op, uint64_t pos, uint32_t n,
                                                 uint64_t spanOff, uint64_t spanLen,
                                                 const std::string& server) {
  char msg[600];
  std::snprintf(msg, sizeof msg,
                "%s: a basket at %llu from %s failed its check (key position differs from the "
                "tree's record); not served",
                recordFile(st.url).c_str(), static_cast<unsigned long long>(pos),
                server.empty() ? "the origin" : server.c_str());
  UCACHE_WARN("%s", msg);
  if (st.store)
    st.store->stats().originChecksFailed.fetch_add(n, std::memory_order_relaxed);
  auto* refused = new XRootDStatus(XrdCl::stError, XrdCl::errDataError, 0, msg);
  noteOriginFailure(st, tally, op, spanOff, spanLen, *refused, server);
  return refused;
}

// Single-Read miss: rounded wire read, user completion first, async persist.
class MissReadHandler : public ResponseHandler {
 public:
  // gateLen != 0: this fetch owns the (wireOff, gateLen) fetch-gate slot
  // (fetch dedup) and must release it on every exit — after staging on success,
  // immediately on failure (waiters then fetch for themselves).
  // `f`: the file the read is issued on, whose server it is counted against.
  // `readerT`: the reader's own timeout of the request, 0 for none or unknown.
  MissReadHandler(std::shared_ptr<HandleState> st, XrdCl::File* f,
                  std::shared_ptr<FileEntry> entry, uint64_t userOff, uint32_t userLen,
                  void* userBuf,
                  ResponseHandler* user, uint64_t wireOff,
                  std::shared_ptr<std::vector<char>> wireBuf, uint64_t t0,
                  uint64_t gateLen = 0, int readerT = 0)
      : st_(std::move(st)), entry_(std::move(entry)), userOff_(userOff), userLen_(userLen),
        userBuf_(userBuf), user_(user), wireOff_(wireOff), wireBuf_(std::move(wireBuf)),
        t0_(t0), gateLen_(gateLen), on_(f), readerT_(readerT),
        inflight_(st_->store ? &st_->store->stats() : nullptr, issueServer(*st_, f)) {
    noteWaitTier(user_, ReaderWait::kOrigin);
  }

  void HandleResponseWithHosts(XRootDStatus* status, AnyObject* response,
                               HostList* hostList) override {
    st_->releaseInner();
    // The origin read this handler was waiting for has landed. Give the guard
    // back HERE and not at destruction: a fail-open retry below issues a fresh
    // origin read with its own guard, and holding this one across it would
    // count one read as two -- while a caller that starts its next read from
    // inside its completion handler would do the same.
    inflight_.release();
    std::unique_ptr<XRootDStatus> s(status);
    std::unique_ptr<AnyObject> r(response);
    std::unique_ptr<HostList> h(hostList);

    ChunkInfo* ci = nullptr;
    if (s && s->IsOK() && r)
      r->Get(ci);
    uint64_t got = ci ? ci->length : 0;
    bool covers = ci && wireOff_ <= userOff_ && wireOff_ + got >= userOff_ + userLen_;

    if (!covers) {
      // Wire failure or short read on the rounded span: fail open — retry
      // the exact user request as pure pass-through, completing the user
      // from that operation faithfully (§5.2 step 5).
      if (!s || !s->IsOK())
        noteOriginFailure(*st_, &entry_->obs().servers, "read", wireOff_, wireBuf_->size(),
                          s ? *s : XRootDStatus(XrdCl::stError, XrdCl::errInternal),
                          answeredBy(*st_, h.get()));
      if (gateLen_)
        entry_->endFetch(wireOff_, gateLen_); // waiters refetch on their own
      st_->noteCacheError(globalConfig());
      // On an armed handle a failure another site may not share is not relayed
      // on the File that failed it: the request moves (moveRead).
      if (s && !s->IsOK() && handOff(*s, h, false)) {
        delete this;
        return;
      }
      if (XrdCl::File* f = st_->acquireInner()) {
        auto* relay = newRelay(st_, f, user_, userOff_, userLen_);
        XRootDStatus s2 = f->Read(userOff_, userLen_, userBuf_, relay, 0);
        if (s2.IsOK()) {
          delete this;
          return;
        }
        delete relay;
        st_->releaseInner();
        complete(user_, new XRootDStatus(s2), nullptr);
      } else {
        complete(user_,
                 s && !s->IsOK() ? new XRootDStatus(*s)
                                 : new XRootDStatus(XrdCl::stError, XrdCl::errInternal),
                 nullptr);
      }
      delete this;
      return;
    }

    // check_origin_integrity: a basket the reader asked for that fails the key
    // check fails the read. One only in the rounded rest of the fetch (a
    // neighbour sharing an edge page) does not concern this reader: it is
    // served, and nothing of the fetch is kept, so the neighbour's own read
    // misses and is checked. Readers parked on this fetch then fetch, one after
    // another, for themselves.
    bool keepNone = false;
#ifdef UCACHE_HAVE_COLDRUN
    if (auto seeks = keySeeksOf(*st_)) {
      const auto m = transpose::firstMisplacedKey(*seeks, wireOff_, wireBuf_->data(), got,
                                                  userOff_, userOff_ + userLen_);
      if (m.count) {
        if (gateLen_)
          entry_->endFetch(wireOff_, gateLen_); // waiters fetch, and are checked, on their own
        std::unique_ptr<XRootDStatus> refused(originCheckFailed(*st_, &entry_->obs().servers,
                                                                "read", m.first, m.count, wireOff_,
                                                                got, answeredBy(*st_, h.get())));
        if (!handOff(*refused, h, true))
          complete(user_, refused.release(), nullptr);
        delete this;
        return;
      }
      keepNone = m.outside;
    }
#endif
    // Serve the user first (memcpy only — no disk on this thread)...
    std::memcpy(userBuf_, wireBuf_->data() + (userOff_ - wireOff_), userLen_);
    if (st_->store) {
      auto& stats = st_->store->stats();
      stats.missBytes.fetch_add(userLen_, std::memory_order_relaxed);
      stats.servedBytes.fetch_add(userLen_, std::memory_order_relaxed);
      stats.originBytes.fetch_add(got, std::memory_order_relaxed);
      stats.originReads.fetch_add(1, std::memory_order_relaxed);
      const uint64_t rt = nowUs() - t0_;
      stats.originRtUs.add(rt);
      entry_->obs().originRtUs.add(rt);
      const std::string server = answeredBy(*st_, h.get());
      entry_->obs().servers.add(server, got, rt, inflight_.nc);
      if (stats.tracer)
        stats.tracer->rec("wire", entry_->key().key, wireOff_, got, nowUs() - t0_, true,
                          wireTraceFields(server, inflight_.t0, inflight_.nc));
    }
    st_->noteCacheOk();
    // Reserve the persist slot BEFORE completing the user, so a Close on the
    // just-woken app thread cannot race past a not-yet-counted persist.
    st_->beginPersist();
    complete(user_, okStatus(), chunkResponse(userOff_, userLen_, userBuf_));
    // ...then persist full pages asynchronously (§5.2 step 4). Ownership of
    // the wire buffer MOVES to the task: single owner, no cross-thread
    // refcount teardown (keeps TSan exact and the lifetime obvious). Nothing
    // of it is kept when its edge failed the key check (parked readers then
    // fetch for themselves).
    persistPagesReserved(st_, entry_, wireOff_, std::move(wireBuf_), keepNone ? 0 : got,
                         gateLen_);
    delete this;
  }

 private:
   // The request moves to another site, as a relay, when the handle is armed and
   // `s` says so (moveRead); `checkFailed`: its bytes failed the integrity check.
   bool handOff(const XRootDStatus& s, std::unique_ptr<HostList>& h, bool checkFailed) {
     return st_->moveOnReads && moveRead(
                                    st_, on_, user_, userOff_, userLen_, readerT_, t0_,
                                    [off = userOff_, len = userLen_, buf = userBuf_](
                                        XrdCl::File* f, ResponseHandler* rh, ucache::XrdTimeout t) {
                                      return f->Read(off, len, buf, rh, t);
                                    },
                                    s, h, checkFailed);
   }
  std::shared_ptr<HandleState> st_;
  std::shared_ptr<FileEntry> entry_;
  uint64_t userOff_;
  uint32_t userLen_;
  void* userBuf_;
  ResponseHandler* user_;
  uint64_t wireOff_;
  std::shared_ptr<std::vector<char>> wireBuf_;
  uint64_t t0_;
  uint64_t gateLen_;
  XrdCl::File* on_; // the File the read was issued on
  int readerT_;
  OriginInFlight inflight_; // the origin read this handler waits for
};

// Issue the rounded wire read for a single-Read miss, honoring the test-only
// wire-read fault: the injected completion is posted to the executor, standing
// in for the XrdCl callback thread, so the handler runs its real error path.
XRootDStatus issueWireRead(const std::shared_ptr<HandleState>& st, XrdCl::File* f, uint64_t off,
                           uint64_t len, void* buf, MissReadHandler* mh, int readerT) {
  XRootDStatus s;
  if (!injectReadFault(false, mh, s))
    s = f->Read(off, len, buf, mh, st->originTimeout(readerT));
  return refusedLater(st, s, mh);
}

std::pair<uint64_t, uint64_t> roundChunk(const FileEntry& e, uint64_t off, uint64_t len);

// Parked-read re-dispatch: runs on the executor after the owning fetch
// released the gate. Usually a RAM hit against the just-staged pages; if the
// owner failed, fetches for itself (joining any newer in-flight fetch).
// Free function on (st, entry) by design — the UCacheFile object may be gone
// (executor tasks must never capture the plugin object).
void redispatchRead(std::shared_ptr<HandleState> st, std::shared_ptr<FileEntry> entry,
                    uint64_t offset, uint32_t size, void* buffer,
                    ResponseHandler* handler) {
  uint64_t t0 = nowUs();
  if (entry->readCached(offset, size, buffer)) {
    if (st->store) {
      auto& stats = st->store->stats();
      stats.servedBytes.fetch_add(size, std::memory_order_relaxed);
      stats.hitReadUs.add(nowUs() - t0);
      if (stats.tracer)
        stats.tracer->rec("ram", entry->key().key, offset, size, nowUs() - t0);
    }
    st->noteCacheOk();
    complete(handler, okStatus(), chunkResponse(offset, size, buffer));
    return;
  }
  auto [ws, we] = roundChunk(*entry, offset, size);
  auto fired = std::make_shared<std::atomic<bool>>(false);
  auto again = [st, entry, offset, size, buffer, handler, fired] {
    if (fired->exchange(true))
      return;
    Executor::instance().post([st, entry, offset, size, buffer, handler] {
      redispatchRead(st, entry, offset, size, buffer, handler);
    });
  };
  if (!entry->beginFetch(ws, we - ws, again)) {
    Executor::instance().postAfter(10000, again); // missed-endFetch insurance
    return;
  }
  auto wireBuf = std::make_shared<std::vector<char>>(we - ws);
  if (XrdCl::File* f = st->acquireInner()) {
    auto* mh = new MissReadHandler(st, f, entry, offset, size, buffer, handler, ws, wireBuf,
                                   nowUs(), we - ws);
    XRootDStatus s = issueWireRead(st, f, ws, we - ws, wireBuf->data(), mh, 0);
    if (!s.IsOK()) {
      delete mh;
      st->releaseInner();
      entry->endFetch(ws, we - ws);
      complete(handler, new XRootDStatus(s), nullptr);
    }
    return;
  }
  entry->endFetch(ws, we - ws);
  complete(handler, new XRootDStatus(st->missError()), nullptr);
}

struct WireElem {
  uint64_t off = 0;
  uint64_t len = 0;
  std::shared_ptr<std::vector<char>> buf;
};

// Vector-read miss stage: one wire VectorRead of rounded+coalesced ranges.
class MissVReadHandler : public ResponseHandler {
 public:
  // `direct`: the file's rule when it is read directly -- its data is served
  // and not kept, only pages holding none of it are.
  MissVReadHandler(std::shared_ptr<HandleState> st, XrdCl::File* f,
                   std::shared_ptr<FileEntry> entry, ChunkList userChunks,
                   std::vector<size_t> missIdx,
                   std::vector<WireElem> wire, ResponseHandler* user, uint64_t t0,
                   std::shared_ptr<ReadRule> direct = nullptr)
      : st_(std::move(st)), entry_(std::move(entry)), userChunks_(std::move(userChunks)),
        missIdx_(std::move(missIdx)), wire_(std::move(wire)), user_(user), t0_(t0),
        direct_(std::move(direct)), on_(f),
        inflight_(st_->store ? &st_->store->stats() : nullptr, issueServer(*st_, f)) {
    noteWaitTier(user_, ReaderWait::kOrigin);
  }

  void HandleResponseWithHosts(XRootDStatus* status, AnyObject* response,
                               HostList* hostList) override {
    st_->releaseInner();
    // The origin read this handler was waiting for has landed. Give the guard
    // back HERE and not at destruction: a fail-open retry below issues a fresh
    // origin read with its own guard, and holding this one across it would
    // count one read as two -- while a caller that starts its next read from
    // inside its completion handler would do the same.
    inflight_.release();
    std::unique_ptr<XRootDStatus> s(status);
    std::unique_ptr<AnyObject> r(response);
    std::unique_ptr<HostList> h(hostList);
    uint64_t wireBytes = 0;
    for (const auto& w : wire_)
      wireBytes += w.len;

    if (!s || !s->IsOK()) {
      // Wire failure: fail open — reissue the FULL original chunk list as
      // pure pass-through (hits included; wasteful but simple and faithful).
      noteOriginFailure(*st_, &entry_->obs().servers, "readv", wire_.front().off, wireBytes,
                        s ? *s : XRootDStatus(XrdCl::stError, XrdCl::errInternal),
                        answeredBy(*st_, h.get()));
      st_->noteCacheError(globalConfig());
      if (s && handOff(*s, h, false)) {
        delete this;
        return;
      }
      if (XrdCl::File* f = st_->acquireInner()) {
        auto* relay =
            newRelay(st_, f, user_, chunksFirst(userChunks_), chunksBytes(userChunks_));
        XRootDStatus s2 = f->VectorRead(userChunks_, nullptr, relay, 0);
        if (s2.IsOK()) {
          delete this;
          return;
        }
        delete relay;
        st_->releaseInner();
        complete(user_, new XRootDStatus(s2), nullptr);
      } else {
        complete(user_, new XRootDStatus(*s), nullptr);
      }
      delete this;
      return;
    }

    // check_origin_integrity, as for a single read: a failing basket inside a
    // miss chunk fails the whole request (one damaged basket fails the
    // reader's whole fill); one only in an element's rounded rest is served
    // and nothing of this answer is kept. Each element is checked on its own,
    // so a key header that an element cut divides is not judged.
    bool keepNone = false;
#ifdef UCACHE_HAVE_COLDRUN
    if (auto seeks = keySeeksOf(*st_)) {
      // One walk of each element finds whether any key in it fails; only an
      // element that holds one is walked again for each miss chunk it covers.
      // Every element overlaps a miss chunk (it is one's rounded run), so a
      // failing key in no chunk is "outside" for each chunk of its element.
      transpose::MisplacedKeys bad;
      for (const auto& w : wire_) {
        if (!transpose::firstMisplacedKey(*seeks, w.off, w.buf->data(), w.len, w.off, w.off)
                 .outside)
          continue;
        for (size_t idx : missIdx_) {
          const auto& c = userChunks_[idx];
          if (c.offset >= w.off + w.len || c.offset + c.length <= w.off)
            continue;
          const auto m = transpose::firstMisplacedKey(*seeks, w.off, w.buf->data(), w.len,
                                                      c.offset, c.offset + c.length);
          bad.first = bad.count ? bad.first : m.first;
          bad.count += m.count;
          keepNone = keepNone || m.outside;
        }
      }
      if (bad.count) {
        std::unique_ptr<XRootDStatus> refused(
            originCheckFailed(*st_, &entry_->obs().servers, "readv", bad.first, bad.count,
                              wire_.front().off, wireBytes, answeredBy(*st_, h.get())));
        if (!handOff(*refused, h, true))
          complete(user_, refused.release(), nullptr);
        delete this;
        return;
      }
    }
#endif

    // Scatter wire bytes into the user's miss chunks (memcpy only). A chunk can
    // span several elements: an oversized run is cut into protocol-legal pieces,
    // and the pieces of one run are adjacent and in offset order.
    uint64_t missBytes = 0, directBytes = 0;
    const WireElem* const wend = wire_.data() + wire_.size();
    for (size_t idx : missIdx_) {
      auto& c = userChunks_[idx];
      char* dst = static_cast<char*>(c.buffer);
      uint64_t at = c.offset, left = c.length;
      for (const WireElem* w = findWire(at); left && w < wend; ++w) {
        if (at < w->off || at >= w->off + w->len)
          break; // gap: coverage is broken, do not read outside the element
        const uint64_t n = std::min<uint64_t>(left, w->off + w->len - at);
        std::memcpy(dst, w->buf->data() + (at - w->off), n);
        dst += n;
        at += n;
        left -= n;
      }
      if (left) {
        // Unreachable by construction (the runs cover every rounded miss
        // range); serving a partially filled buffer would be a wrong answer.
        complete(user_, new XRootDStatus(XrdCl::stError, XrdCl::errInternal), nullptr);
        delete this;
        return;
      }
      if (ruleReadsData(direct_, c.offset, c.length))
        directBytes += c.length;
      else
        missBytes += c.length;
    }
    if (st_->store) {
      auto& stats = st_->store->stats();
      stats.missBytes.fetch_add(missBytes, std::memory_order_relaxed);
      // What a file read directly fetched for its data is relayed, not a fill.
      stats.originBytes.fetch_add(wireBytes - std::min(wireBytes, directBytes),
                                  std::memory_order_relaxed);
      stats.originReadvs.fetch_add(1, std::memory_order_relaxed);
      uint64_t total = 0;
      for (const auto& c : userChunks_)
        total += c.length;
      stats.servedBytes.fetch_add(total - directBytes, std::memory_order_relaxed);
      const uint64_t rt = nowUs() - t0_;
      stats.originRtUs.add(rt);
      entry_->obs().originRtUs.add(rt);
      const std::string server = answeredBy(*st_, h.get());
      entry_->obs().servers.add(server, wireBytes, rt, inflight_.nc);
      if (stats.tracer)
        stats.tracer->rec("wire", entry_->key().key, wire_.front().off, wireBytes, nowUs() - t0_,
                          true, wireTraceFields(server, inflight_.t0, inflight_.nc));
    }
    st_->noteCacheOk();
    if (keepNone)
      wire_.clear(); // served, and nothing of this answer is kept
    if (direct_) {
      noteDirectBytes(st_, entry_, directBytes);
      persistWithoutData();
      delete this;
      return;
    }
    // Reserve all persist slots before completing the user (see MissRead).
    for (size_t i = 0; i < wire_.size(); ++i)
      st_->beginPersist();
    complete(user_, okStatus(), vreadResponse(userChunks_));
    // Persist each wire element's full pages asynchronously; buffer
    // ownership moves to each task (single owner — see MissReadHandler).
    for (auto& w : wire_)
      persistPagesReserved(st_, entry_, w.off, std::move(w.buf), w.len);
    delete this;
  }

 private:
   // As MissReadHandler's: the whole request moves, issued again as it was made.
   bool handOff(const XRootDStatus& s, std::unique_ptr<HostList>& h, bool checkFailed) {
     return st_->moveOnReads &&
            moveRead(
                st_, on_, user_, chunksFirst(userChunks_), chunksBytes(userChunks_), 0, t0_,
                [chunks = userChunks_](XrdCl::File* f, ResponseHandler* rh, ucache::XrdTimeout t) {
                  return f->VectorRead(chunks, nullptr, rh, t);
                },
                s, h, checkFailed);
   }

  // A file read directly: complete the user, then keep only the whole pages of
  // each element that hold none of the file's data (its own records).
  void persistWithoutData() {
    const uint64_t ps = entry_->pageSize(), fs = entry_->fileSize();
    struct Run {
      uint64_t a, b;
      std::shared_ptr<std::vector<char>> buf;
      uint64_t base;
    };
    std::vector<Run> keep;
    for (auto& w : wire_) {
      const uint64_t we = w.off + w.len;
      uint64_t p = (w.off + ps - 1) / ps * ps; // whole pages only
      for (; p < we; p += ps) {
        const uint64_t pe = std::min(p + ps, fs);
        if (pe > we || ruleReadsData(direct_, p, pe - p))
          continue;
        if (!keep.empty() && keep.back().buf == w.buf && keep.back().b == p)
          keep.back().b = pe;
        else
          keep.push_back({p, pe, w.buf, w.off});
      }
    }
    for (size_t i = 0; i < keep.size(); ++i)
      st_->beginPersist();
    complete(user_, okStatus(), vreadResponse(userChunks_));
    for (auto& r : keep) {
      auto st = st_;
      auto entry = entry_;
      Executor::instance().post([st, entry, r] {
        entry->writePages(r.a, r.b - r.a, r.buf->data() + (r.a - r.base));
        entry->flushMeta(false);
        st->endPersist();
      });
    }
  }

  const WireElem* findWire(uint64_t off) const {
    // wire_ is sorted by offset and covers every miss chunk's rounded range.
    const WireElem* best = &wire_.front();
    for (const auto& w : wire_)
      if (w.off <= off)
        best = &w;
      else
        break;
    return best;
  }

  std::shared_ptr<HandleState> st_;
  std::shared_ptr<FileEntry> entry_;
  ChunkList userChunks_;
  std::vector<size_t> missIdx_;
  std::vector<WireElem> wire_;
  ResponseHandler* user_;
  uint64_t t0_;
  std::shared_ptr<ReadRule> direct_;
  XrdCl::File* on_;         // the File the read was issued on
  OriginInFlight inflight_; // the origin read this handler waits for
};

// Rounded [start,end) for a chunk; the rule and why it is not optional are in
// ReadRounding.h. Vector reads cut the result to legal elements afterwards.
std::pair<uint64_t, uint64_t> roundChunk(const FileEntry& e, uint64_t off, uint64_t len) {
  return roundSpan(e.pageSize(), e.fileSize(), off, len);
}

// Issues the wire VectorRead for the miss stage. Runs on the caller's thread
// or an executor thread; touches inner only via acquire/release.
void issueMissVRead(const std::shared_ptr<HandleState>& st,
                    const std::shared_ptr<FileEntry>& entry, ChunkList userChunks,
                    std::vector<size_t> missIdx, ResponseHandler* user,
                    std::shared_ptr<ReadRule> direct = nullptr) {
  // Build rounded intervals in offset order, coalescing overlaps so the
  // origin never sees overlapping reads (§5.2 step 3). A file read directly
  // fetches its data exactly as asked: rounding is for pages to keep.
  std::vector<std::pair<uint64_t, uint64_t>> ivals;
  ivals.reserve(missIdx.size());
  for (size_t idx : missIdx) {
    const auto& c = userChunks[idx];
    if (ruleReadsData(direct, c.offset, c.length))
      ivals.emplace_back(c.offset, c.offset + c.length);
    else
      ivals.push_back(roundChunk(*entry, c.offset, c.length));
  }
  std::sort(ivals.begin(), ivals.end());
  std::vector<std::pair<uint64_t, uint64_t>> runs;
  for (auto [s, e] : ivals) {
    if (!runs.empty() && s <= runs.back().second)
      runs.back().second = std::max(runs.back().second, e);
    else
      runs.emplace_back(s, e);
  }
  // Cut each run into protocol-legal elements. An element above the ceiling is
  // REFUSED and fails the whole request, so this is correctness, not tuning;
  // cutting on page boundaries keeps every piece storable whole. Runs stay
  // sorted and their pieces adjacent, which is what the scatter below relies on.
  std::vector<WireElem> wire;
  wire.reserve(runs.size());
  for (auto [s, e] : runs)
    for (uint64_t at = s; at < e;) {
      const uint64_t cut = readvElemEnd(at, e, entry->pageSize());
      wire.push_back({at, cut - at, nullptr});
      at = cut;
    }
  ChunkList wireChunks;
  wireChunks.reserve(wire.size());
  for (auto& w : wire) {
    w.buf = std::make_shared<std::vector<char>>(w.len);
    wireChunks.emplace_back(w.off, static_cast<uint32_t>(w.len), w.buf->data());
  }

  XrdCl::File* f = st->acquireInner();
  if (!f) {
    complete(user, new XRootDStatus(st->missError()), nullptr);
    return;
  }
  auto* mh = new MissVReadHandler(st, f, entry, std::move(userChunks), std::move(missIdx),
                                  std::move(wire), user, nowUs(), std::move(direct));
  XRootDStatus s;
  if (!injectReadFault(false, mh, s)) // test hook: wire vector read dies mid-stream
    s = f->VectorRead(wireChunks, nullptr, mh, st->originTimeout(0));
  s = refusedLater(st, s, mh);
  if (!s.IsOK()) {
    delete mh;
    st->releaseInner();
    complete(user, new XRootDStatus(s), nullptr);
  }
}

} // namespace

//---------------------------------------------------------------- plugin ---

UCacheFile::UCacheFile() : st_(std::make_shared<HandleState>()) {
  st_->store = globalStore();
  {
    std::lock_guard<std::mutex> g(st_->mu);
    // The inner file is owned by HandleState (not as a plugin member): a posted
    // retry task swaps it and may run after Open returns, so it must reach the
    // file through st_ without capturing the plugin object.
    // enablePlugIns=false — prevents recursion.
    st_->innerOwned = std::make_unique<XrdCl::File>(false);
    st_->inner = st_->innerOwned.get();
    st_->innerValid = true;
  }
}

UCacheFile::~UCacheFile() {
  st_->syncFork(); // a handle a forked child inherited starts over first
  // Drain outstanding page persists so a short-lived process still leaves a
  // fully populated cache (Close normally does this; the dtor covers the
  // no-explicit-Close path). Not on any read's latency path.
  st_->drainPersists();
  std::shared_ptr<FileEntry> e;
  std::shared_ptr<ColdFill> cold;
  {
    std::lock_guard<std::mutex> g(st_->mu);
    st_->closed = true;
    e.swap(st_->entry);
    cold.swap(st_->cold);
  }
#ifdef UCACHE_HAVE_COLDRUN
  coldDetach(cold);
#endif
#ifdef UCACHE_HAVE_PREFETCH
  // Only touch the prefetcher if this process uses it: reaching the singleton
  // constructs it and starts its thread, which `prefetch = off` should not do.
  if (globalConfig().prefetch && st_->prefetchSeen.load(std::memory_order_acquire))
    Prefetcher::instance().onClose(st_, e); // read-ahead never used is dropped here too
#endif
  if (e)
    e->flushAll(); // synchronous (covers the no-explicit-Close path)
  emitRelayObs(st_, e);
  // A forked child that never opened afresh the file it inherited leaves it
  // be: it is the parent's, and closing it is not this process's to do.
  st_->shutdownInner(/*leak=*/st_->reopenPending());
}

// CPU-span evidence: process user+sys CPU in µs, and a counter of
// concurrently-open ucache handles (attribution is per-file only when the
// access pattern is sequential — overlap sets the `blended` flag).
uint64_t processCpuUs() {
  struct rusage ru;
  ::getrusage(RUSAGE_SELF, &ru);
  return static_cast<uint64_t>(ru.ru_utime.tv_sec) * 1000000u + ru.ru_utime.tv_usec +
         static_cast<uint64_t>(ru.ru_stime.tv_sec) * 1000000u + ru.ru_stime.tv_usec;
}
std::atomic<int> gOpenUCacheHandles{0};

// Write the entry's .cost sidecar (tiny, advisory: tmp+rename).
void writeCostSidecar(const std::string& url, const Config& cfg, uint64_t cpuUs, bool blended) {
  auto key = UrlKey::parse(url, cfg.keepCgi);
  if (!key || cpuUs == 0)
    return;
  const std::string base = key->objectDir(cfg.cacheDir) + "/" + key->hashHex;
  struct ::stat st;
  if (::stat((base + ".meta").c_str(), &st) != 0)
    return; // nothing cached
  char buf[96];
  int n = std::snprintf(buf, sizeof buf, "v1 cpu_us=%llu blended=%d\n",
                        static_cast<unsigned long long>(cpuUs), blended ? 1 : 0);
  const std::string tmp = base + ".cost.tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
    return;
  ssize_t r = ::write(fd, buf, static_cast<size_t>(n));
  ::close(fd);
  if (r == n)
    ::rename(tmp.c_str(), (base + ".cost").c_str());
  else
    ::unlink(tmp.c_str());
}

namespace {
// Once per process at INFO (silent at the default level), then at DEBUG: which
// signal made a handle a copy. The URL is logged without its query, which can
// carry a token.
void noteCopier(CopySignal sig, const std::string& url) {
  static std::atomic<bool> said{false};
  const std::string where = url.substr(0, url.find('?'));
  const std::string program = hostScript().empty() ? hostExecutable() : hostScript();
  const std::string what = sig == CopySignal::kExecutable || sig == CopySignal::kRootTool
                               ? std::string(copySignalName(sig)) + " (" + program + ")"
                               : std::string(copySignalName(sig));
  if (!said.exchange(true, std::memory_order_relaxed))
    UCACHE_INFO("%s is opened by %s: copies are read straight from the origin, not through the "
                "cache, so a copy is the origin's bytes (copy_detect = off to cache them)",
                where.c_str(), what.c_str());
  else
    UCACHE_DEBUG("%s opened by %s: read straight from the origin", where.c_str(), what.c_str());
}
} // namespace

XrdCl::XRootDStatus UCacheFile::Open(const std::string& url, XrdCl::OpenFlags::Flags flags,
                                     XrdCl::Access::Mode mode, ResponseHandler* handler,
                                     ucache::XrdTimeout timeout) {
  st_->syncFork();
  childWork(st_); // a forked child's first open: its own record
  st_->cpu0Us = processCpuUs(); // CPU-span start
  const int nowOpen = gOpenUCacheHandles.fetch_add(1) + 1;
  if (nowOpen > 1)
    st_->cpuBlended = true; // another handle already open: spans overlap
  // nowOpen can be <= 0: the counter has one increment here and one decrement
  // in Close, so a Close without a matching Open (a handle XrdCl closes after
  // a failed open) drives it negative. Casting that to uint64_t stored 2^64-3
  // as the "peak" and silently disabled every consumer of it.
  if (st_->store && nowOpen > 0) {
    // Monotone max. Relaxed CAS loop: contention here is bounded by how often
    // a NEW peak happens, which is rare after the first seconds of a job.
    auto& hw = st_->store->stats().handlesHighWater;
    uint64_t seen = hw.load(std::memory_order_relaxed);
    while (static_cast<uint64_t>(nowOpen) > seen &&
           !hw.compare_exchange_weak(seen, static_cast<uint64_t>(nowOpen),
                                     std::memory_order_relaxed))
      ; // seen is refreshed by the failed exchange
  }

  const Config& cfg = globalConfig();
  st_->url = url;
  st_->readerOpenTimeoutS.store(static_cast<int>(timeout));
  using OF = XrdCl::OpenFlags;
  bool writey = flags & (OF::Update | OF::Write | OF::New | OF::Delete);
  passthroughOnly_ = writey || cfg.disable || !st_->store;
  // A handle opened for a copy reads straight from the origin, for this handle
  // only: a copy is the origin's bytes, never the layout the cache would show
  // a reader (CopyDetect.h). Asked only of a read open that could be cached,
  // so a copy's destination never pays for the stack walk.
  if (!passthroughOnly_ && cfg.copyDetect) {
    const CopySignal sig = copierSignal();
    if (sig != CopySignal::kNone) {
      passthroughOnly_ = true;
      st_->store->stats().copierHandles.fetch_add(1, std::memory_order_relaxed);
      noteCopier(sig, url);
    }
  }
  st_->reopenInChild = !passthroughOnly_; // before any fork can find it unset
  if (passthroughOnly_ && st_->store) {
    // Start a relayed handle's span at open for the same reason: its cost
    // includes reaching the origin, and a one-read file must still have a span.
    const uint64_t t0 = nowUs();
    uint64_t expected = 0;
    st_->relayFirstUs.compare_exchange_strong(expected, t0, std::memory_order_relaxed);
    st_->relayLastUs.store(t0, std::memory_order_relaxed);
  }
  if (writey && st_->store) {
    // §4.4: write access invalidates any cached entry for this URL.
    auto store = st_->store;
    auto key = UrlKey::parse(url, cfg.keepCgi);
    if (key)
      Executor::instance().post([store, key] { store->invalidate(*key); });
  }
  // Cache-freshness (UCACHE_REVALIDATE_S, default 7 days): if a
  // recent local entry is trusted, skip the remote open+stat entirely and
  // serve from cache — the origin is touched only if a genuine miss forces a
  // lazy open (fail-open). This is the reliability control: warm reads don't
  // depend on a flaky/loaded/WAN remote. UCACHE_REVALIDATE_S=0 (revalidate
  // every open) never enters here.
  if (!passthroughOnly_ && cfg.revalidateSeconds > 0) {
    auto key = UrlKey::parse(url, cfg.keepCgi);
    if (key && cacheFresh(*key, cfg.cacheDir, cfg.revalidateSeconds)) {
      {
        std::lock_guard<std::mutex> g(st_->mu);
        st_->cacheOnly = true;
      }
      cacheOpen_ = true;
      UCACHE_INFO("trusted recent cache for %s; serving without remote open "
                  "(UCACHE_REVALIDATE_S=%d)", url.c_str(), cfg.revalidateSeconds);
      if (handler) {
        auto h = handler;
        Executor::instance().post([h] { complete(h, okStatus(), nullptr); });
      }
      return XRootDStatus();
    }
  }
  // The cache entry is created lazily on the first read (ensureEntry). With
  // open-retry enabled (opt-in) wrap the app handler so a TRANSIENT open failure
  // is retried on a fresh inner file (Case A); write-opens and
  // the null-handler case are never wrapped.
  // With adaptive_federation on, a read open of a handle the cache serves has
  // a deadline (OpenPolicy.h); every other open keeps the reader's timeout.
  const ucache::XrdTimeout openTimeout =
      passthroughOnly_ ? timeout : openDeadline(static_cast<int>(timeout));
  // With adaptive_federation on, a cached handle's read open goes through it
  // too: it opens the file at another site when the open fails at one.
  if ((cfg.openRetries > 0 || (cfg.adaptiveFederation && !passthroughOnly_)) && !writey &&
      handler) {
    // Recorded for a handle the cache serves only, as without retries.
    auto* rh = new RetryingOpenHandler(st_, url, flags, mode, openTimeout, handler, cfg,
                                       !passthroughOnly_, static_cast<int>(timeout));
    return rh->start();
  }
  // Otherwise a cached handle's read open passes through ReaderOpenHandler,
  // which hands the reader XrdCl's own completion; every other open forwards
  // the reader's handler unchanged. An error XrdCl returns at once (no
  // completion will come) is, for a watched open, recorded as a failed attempt
  // and given to the reader through its handler, and Open returns OK; for any
  // other open it is the reader's answer, as without the plugin.
  ResponseHandler* h = handler;
  ReaderOpenHandler* watched = nullptr;
  if (!passthroughOnly_ && handler)
    h = watched = new ReaderOpenHandler(handler, st_, static_cast<int>(openTimeout));
  XrdCl::File* f = st_->inner.load();
  const std::string openUrl = faultOpenUrl(url, 1); // test hook only; `url` in production
  const bool fresh = firstOpenOn(*st_, f);
  const XRootDStatus s = f->Open(openUrl, flags, mode, h, openTimeout);
  if (s.IsOK()) {
    sentOpenOn(*st_, f);
  } else if (watched) {
    watched->refused(s, openUrl, fresh);
    return XRootDStatus();
  }
  return s;
}

// Runs on the caller's (app/IMT) thread — never an XrdCl callback thread —
// so the synchronous inner Stat here is allowed (§5.3). Serialized by
// setupMu so concurrent first-reads set up exactly once.
#ifdef UCACHE_HAVE_COLDRUN
namespace {
// Which layout of the file this handle is shown. A process never shows a file
// in a layout it has not shown it in before -- a reader may hold offsets from
// any of its opens -- except that the original may become a slot layout, whose
// original region reads the same. Otherwise: the file's store, else a new one
// when recompression is on, else the file as stored.
// A handle opened for a copy never gets here: Open makes it pass-through, so it
// is neither recorded nor constrained by this. That is safe because a copy's
// file handle is its own and holds no offset any reader uses.
// `inherited`: the handle is one a forked child is setting up again, and says
// itself what happens when its layout is gone.
void chooseLayout(const std::shared_ptr<HandleState>& st, const std::shared_ptr<FileEntry>& entry,
                  const UrlKey& key, const Config& cfg, uint64_t mtime, uint8_t cksumKind,
                  uint32_t cksum, std::shared_ptr<ColdFill>& cold, bool inherited) {
  // Each pass either settles on a layout this process has not contradicted, or
  // learns which layout another handle settled on meanwhile and follows it.
  for (int pass = 0;; ++pass) {
    uint64_t hash = 0;
    const ShownLayout shown = shownLayout(key.key, hash);
    if (shown == ShownLayout::kSlot) {
      cold = coldAttach(st, entry, key, mtime, cksumKind, cksum, AttachMode::kMatch, hash);
      if (!cold && !inherited)
        UCACHE_WARN("%s was shown in a slot layout earlier in this process and that layout is "
                    "gone; it is served as stored, and offsets read from the layout fail",
                    key.key.c_str());
      return;
    }
    if (pass >= 3) // cannot happen: a shown layout only ever moves away from original
      return;
    std::shared_ptr<ColdFill> c =
        coldAttach(st, entry, key, mtime, cksumKind, cksum, AttachMode::kExisting);
    // Not for a file this process already reads directly: nothing it
    // converted would be kept.
    if (!c && cfg.recompress && !readRuleDirect(key.key))
      c = coldAttach(st, entry, key, mtime, cksumKind, cksum, AttachMode::kCreate);
    if (c) {
      const uint64_t mine = coldLayoutHash(*c);
      uint64_t won = 0;
      if (noteShownLayout(key.key, ShownLayout::kSlot, mine, &won, c.get()) ==
              ShownLayout::kSlot &&
          won == mine) {
        cold = std::move(c);
        return;
      }
      coldDetach(c);
      continue;
    }
    if (noteShownLayout(key.key, ShownLayout::kOriginal) == ShownLayout::kOriginal)
      return;
  }
}
} // namespace
#endif

namespace {
// What a handle's first request says about it, when that request reads the
// whole file at once (fsspec's open().read(), cat_file(path, 0, size)): only a
// copy asks that as its first request, but so does a reader that fetches in
// blocks larger than a small file (ROOT's raw-file layer reads 128 KiB).
enum class WholeFirstRead : uint8_t {
  kNo,       // not a whole-file first read, or nothing to decide
  kCopy,     // a slot store exists: the handle is a copy,
             // and reads the origin's bytes straight from the origin
  kAsStored, // recompress = on and nothing converted yet: shown as stored and
             // cached, and no layout of its own is made for this handle
};

// Either answer is the origin's bytes, which is right for a copy and for a
// reader alike; a copy of a layout the cache made would not be.
WholeFirstRead wholeFirstRead(const std::pair<uint64_t, uint64_t>* firstRead,
                              uint64_t originSize, const UrlKey& key, const Config& cfg,
                              ForkLayout forkLayout) {
  if (!firstRead || !cfg.copyDetect || !cfg.transpose || forkLayout != ForkLayout::kNone)
    return WholeFirstRead::kNo;
  if (originSize == 0 || firstRead->first != 0 || firstRead->second < originSize)
    return WholeFirstRead::kNo;
  const std::string base = key.objectDir(cfg.cacheDir) + "/" + key.hashHex;
  struct ::stat sb;
  if (::stat((base + ".slots").c_str(), &sb) == 0)
    return WholeFirstRead::kCopy;
  return cfg.recompress ? WholeFirstRead::kAsStored : WholeFirstRead::kNo;
}

// A replica an earlier release made beside the byte cache (<hash>.tdata, .tmeta,
// .tok) is not served: the file's slot store takes its place. Removed by the
// first open that finds it (and by `ucache recompress` and the eviction pass);
// said once per process at WARN, later ones at INFO. A reader that held
// positions in it fails, as after any replica is removed.
void dropEarlierReplica(const UrlKey& key, const Config& cfg) {
  if (!CacheStore::dropEarlierReplica(RealIO::instance(), key.objectDir(cfg.cacheDir), key.hashHex))
    return;
  static std::atomic<bool> said{false};
  if (!said.exchange(true, std::memory_order_relaxed))
    UCACHE_WARN("%s had a replica an earlier uCache made (.tdata), which this one does not serve; "
                "removed. `ucache recompress` converts what is cached into the current form "
                "(later files are logged at INFO)",
                key.key.c_str());
  else
    UCACHE_INFO("%s had a replica an earlier uCache made (.tdata); removed", key.key.c_str());
}

#ifdef UCACHE_HAVE_COLDRUN
// A request the truncated-copy guard refused (CopyGuard.h): the copy fails
// here, loudly, instead of coming out corrupt. Not a cache error -- never
// counted as a fail-open, never trips the handle. (Only a file shown in
// another layout has a copy to guard.)
XrdCl::XRootDStatus refuseCopy(const std::shared_ptr<HandleState>& st) {
  static std::atomic<bool> said{false};
  if (st->store)
    st->store->stats().copiesRefused.fetch_add(1, std::memory_order_relaxed);
  const std::string where = st->url.substr(0, st->url.find('?')); // no token in the log
  const std::string msg = "uCache: this read would complete a copy of " + where +
                          " as the cache lays the file out, which is not the origin's file; "
                          "copy it with the cache off (UCACHE_DISABLE=1)";
  if (!said.exchange(true, std::memory_order_relaxed))
    UCACHE_WARN("%s", msg.c_str());
  return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported, 0, msg);
}
#endif

// A read through an inherited handle whose layout is gone (ensureEntry).
XrdCl::XRootDStatus lostLayoutError(const std::shared_ptr<HandleState>& st) {
  const std::string where = st->url.substr(0, st->url.find('?'));
  return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errInvalidOp, 0,
                             "uCache: " + where +
                                 " was shown in a layout that is gone since this process was "
                                 "forked; open the file again");
}

#ifdef UCACHE_HAVE_COLDRUN
bool guardAllowsChunks(const std::shared_ptr<HandleState>& st, uint64_t originSize,
                       uint64_t shownSize, const XrdCl::ChunkList& chunks) {
  std::vector<CopyGuard::Range> r;
  r.reserve(chunks.size());
  for (const auto& c : chunks)
    r.emplace_back(c.offset, c.length);
  return st->copyGuard.allowRanges(originSize, shownSize, r.data(), r.size());
}
#endif
} // namespace

std::shared_ptr<FileEntry> UCacheFile::ensureEntry(const std::pair<uint64_t, uint64_t>* firstRead) {
  // Pass-through handles -- write opens, UCACHE_DISABLE, no store, and copies
  // (CopyDetect.h) -- never get an entry: nothing is created, stored or
  // shown in another layout for them.
  if (passthroughOnly_ || !st_->store)
    return nullptr;
  {
    std::lock_guard<std::mutex> g(st_->mu);
    if (st_->setupDone)
      return (st_->closed || st_->tripped) ? nullptr : st_->entry;
  }
  std::lock_guard<std::mutex> setup(st_->setupMu);
  {
    std::lock_guard<std::mutex> g(st_->mu);
    if (st_->setupDone)
      return (st_->closed || st_->tripped) ? nullptr : st_->entry;
  }
  childWork(st_); // a handle a forked child inherited sets up again here
  const uint64_t setupT0 = nowUs(); // entry-setup span (sidecar load,
                                    // validation, layout choice) — the per-open
                                    // cost 65k opens multiply on a slow disk
  const Config& cfg = globalConfig();
  auto key = UrlKey::parse(st_->url, cfg.keepCgi);
  bool allowed = key &&
                 (cfg.allowHosts.empty() || hostMatchesAny(cfg.allowHosts, key->host)) &&
                 !hostMatchesAny(cfg.denyHosts, key->host);
  std::shared_ptr<FileEntry> entry;
  std::shared_ptr<ColdFill> cold;
  std::unique_ptr<XrdCl::StatInfo> statClone;

  bool cacheOnly;
  ForkLayout forkLayout; // set when a forked child inherited this handle (syncFork)
  {
    std::lock_guard<std::mutex> g(st_->mu);
    cacheOnly = st_->cacheOnly;
    forkLayout = st_->forkLayout;
  }
  // The layout such a handle was set up in before the fork is the one it keeps:
  // its reader holds offsets into it. The original stays the original; a
  // slot layout is matched again.
  // A handle whose first request read the whole file is shown the file as
  // stored too (wholeFirstRead), or is a copy: no entry, the origin's bytes.
  bool keepOriginal = forkLayout == ForkLayout::kOriginal;
  bool copy = false;

  // Trusted recent cache (UCACHE_REVALIDATE_S): build from the local sidecar,
  // NO remote stat. If the entry is gone (evicted since Open), degrade to the
  // origin path below.
  bool degraded = false;
  if (cacheOnly && allowed) {
    if (auto meta = MetaFile::load(RealIO::instance(), key->metaPath(cfg.cacheDir));
        meta && meta->key == key->key) {
      // Set before anything here can open the origin lazily (the layout choice
      // reads the file's records): that open must serve this size or nothing.
      st_->trustMarker = valPath(*key, cfg.cacheDir);
      st_->expectedOriginSize.store(meta->fileSize, std::memory_order_relaxed);
      const WholeFirstRead whole = wholeFirstRead(firstRead, meta->fileSize, *key, cfg, forkLayout);
      keepOriginal = keepOriginal || whole == WholeFirstRead::kAsStored;
      if (whole == WholeFirstRead::kCopy) {
        copy = true; // read from the origin; the Stat answer is the origin's
        statClone = std::make_unique<XrdCl::StatInfo>("0", meta->fileSize,
                                                      XrdCl::StatInfo::IsReadable,
                                                      meta->originMtime);
      } else {
        entry = st_->store->open(*key, meta->fileSize, meta->originMtime, meta->cksumKind,
                                 meta->originCksum);
      }
      if (entry) {
        statClone = std::make_unique<XrdCl::StatInfo>("0", meta->fileSize,
                                                      XrdCl::StatInfo::IsReadable,
                                                      meta->originMtime);
        dropEarlierReplica(*key, cfg);
#ifdef UCACHE_HAVE_COLDRUN
        if (cfg.transpose && !keepOriginal)
          chooseLayout(st_, entry, *key, cfg, meta->originMtime, meta->cksumKind,
                       meta->originCksum, cold, forkLayout != ForkLayout::kNone);
        if (cold)
          statClone->SetSize(coldVirtualSize(*cold));
#endif
      }
    }
    if (!entry && !copy) {
      std::lock_guard<std::mutex> g(st_->mu);
      st_->cacheOnly = false; // no usable trusted cache — hit the origin once
      degraded = true;
    }
  } else if (cacheOnly) {
    // Trusted at Open but the key is no longer allowed: degrade so the lazy
    // origin open below gives the handle a real pass-through target.
    std::lock_guard<std::mutex> g(st_->mu);
    st_->cacheOnly = false;
    degraded = true;
  }

  // Origin path: the default (revalidate every open), or a degraded trusted
  // handle. When degraded, the inner file was never opened at Open — open it
  // lazily now (best-effort; a failed open just yields pass-through).
  // `degraded` must re-enable this block: `cacheOnly` is the pre-degrade
  // snapshot, and gating on it alone left the handle entry-less on an
  // unopened inner file — every read failed errInvalidOp (found by the
  // revalidate degrade-race gate).
  if (!entry && !copy && (!cacheOnly || degraded)) {
    if (degraded)
      st_->ensureInnerOpen();
    XrdCl::StatInfo* si = nullptr;
    XRootDStatus s = st_->inner.load()->Stat(false, si); // synchronous — caller thread
    std::unique_ptr<XrdCl::StatInfo> sip(si);
    if (s.IsOK() && si && allowed) {
      statClone = std::make_unique<XrdCl::StatInfo>(*si);
      bool declinedForSpace = false, declinedSizeDiffers = false;
      const WholeFirstRead whole = wholeFirstRead(firstRead, si->GetSize(), *key, cfg, forkLayout);
      keepOriginal = keepOriginal || whole == WholeFirstRead::kAsStored;
      if (whole == WholeFirstRead::kCopy)
        copy = true;
      else
        entry = st_->store->open(*key, si->GetSize(), si->GetModTime(),
                                 MetaData::kCksumNone, 0, &declinedForSpace,
                                 &declinedSizeDiffers);
      // A capacity DECISION is not a fail-open. failopenEvents means something
      // went wrong and every benchmark requires it to be zero, so counting a
      // deliberate decline there would make a cache that is merely full look
      // broken. The read proceeds uncached either way; only the reason differs.
      // Nor is refusing to mix a source of another size into the entry this
      // process already holds (size_disagreements counts that).
      if (!entry && !declinedForSpace && !declinedSizeDiffers && !copy)
        st_->store->stats().failopenEvents.fetch_add(1, std::memory_order_relaxed);
      // The file's store, when there is one or recompression makes one: the
      // reported size becomes the layout's, so ROOT's fBEGIN <= fEND <= size
      // check holds.
      if (entry)
        dropEarlierReplica(*key, cfg);
#ifdef UCACHE_HAVE_COLDRUN
      if (entry && cfg.transpose && !keepOriginal)
        chooseLayout(st_, entry, *key, cfg, si->GetModTime(), MetaData::kCksumNone, 0, cold,
                     forkLayout != ForkLayout::kNone);
      if (entry && cold)
        statClone->SetSize(coldVirtualSize(*cold));
#endif
      // Record the validation time so a later open within the window can trust
      // the cache and skip the origin (UCACHE_REVALIDATE_S).
      if (entry && cfg.revalidateSeconds > 0)
        touchVal(*key, cfg.cacheDir);
    }
  }
  if (copy) {
    st_->store->stats().copierHandles.fetch_add(1, std::memory_order_relaxed);
    noteCopier(CopySignal::kWholeFile, st_->url);
  }
  // A handle a forked child inherited whose layout cannot be had again (the
  // slot store was removed, or rebuilt differently, and the cache cannot take
  // the file back): its reader holds offsets into that layout, and the file as
  // stored would give other bytes at some of them. Every read fails instead.
  const bool lost = forkLayout == ForkLayout::kCold && !cold;
  if (lost) {
    UCACHE_WARN("%s was shown in a layout of the cache's before this process was forked, and "
                "that layout is gone: reads through the handle the process inherited fail; "
                "open the file again",
                key ? key->key.c_str() : st_->url.c_str());
    entry.reset();
  }
#ifdef UCACHE_HAVE_COLDRUN
  // The mixed map the handle is shown, chosen once for its life: a handle a
  // forked child set up again keeps the one it had (null: the slot layout).
  std::shared_ptr<const ColdMap> coldMap;
  bool pickMap = false;
  if (cold && !lost) {
    std::lock_guard<std::mutex> g(st_->mu);
    pickMap = !st_->coldMapChosen;
  }
  if (pickMap) {
    coldMap = coldPickMap(cold);
    // The size it is shown is its map's: ROOT takes a header whose fEND is
    // past the file's size for a truncated file.
    if (coldMap && coldMap->compact() && statClone)
      statClone->SetSize(coldShownSize(*cold, coldMap.get()));
  }
#endif
  if (st_->store) {
    auto& stats = st_->store->stats();
    stats.openUs.add(nowUs() - setupT0);
    if (stats.tracer && entry)
      stats.tracer->rec("open", entry->key().key, 0, 0, nowUs() - setupT0,
                        /*sampled=*/false);
  }
  bool attached = false;
  {
    std::lock_guard<std::mutex> g(st_->mu);
    st_->setupDone = true;
    st_->statInfo = std::move(statClone);
    st_->forkLayout = ForkLayout::kNone; // set up in this process now
    if (lost)
      st_->layoutLost.store(true, std::memory_order_release);
    if (!st_->closed && !st_->tripped) {
      st_->entry = entry;
#ifdef UCACHE_HAVE_COLDRUN
      // A slot run keeps its own rule (ColdRun).
      if (entry && !cold)
        st_->rule = ReadRule::forFile(entry->key().key, entry->fileSize());
#endif
      st_->cold = cold;
      st_->wholeCopy = copy;
#ifdef UCACHE_HAVE_COLDRUN
      if (cold && !st_->coldMapChosen) {
        st_->coldMap = std::move(coldMap);
        st_->coldMapChosen = true;
      }
#endif
      attached = true;
    }
  }
#ifdef UCACHE_HAVE_COLDRUN
  if (cold && !attached)
    coldDetach(cold); // closed while setting up
#else
  (void)attached;
#endif
  st_->applyReadPolicy();
  std::lock_guard<std::mutex> g(st_->mu);
  return st_->entry;
}

std::shared_ptr<ColdFill> UCacheFile::currentCold() const {
  std::lock_guard<std::mutex> g(st_->mu);
  return st_->cold;
}

uint64_t UCacheFile::shownSize() const {
  std::lock_guard<std::mutex> g(st_->mu);
#ifdef UCACHE_HAVE_COLDRUN
  if (st_->cold)
    return coldShownSize(*st_->cold, st_->coldMap.get());
#endif
  return 0;
}


XrdCl::XRootDStatus UCacheFile::Close(ResponseHandler* handler, ucache::XrdTimeout timeout) {
  st_->syncFork();
  // A read waiting for a move is a request in flight, which the client's own Close refuses: waited
  // for, within the bound, before anything of the handle is changed, so that a Close that is
  // refused leaves the handle as it was and the read goes on.
  if (!st_->waitParked(std::chrono::seconds(10)))
    return XRootDStatus(XrdCl::stError, XrdCl::errInvalidOp);
  // Wait for queued page persists before flushing meta, so the cache is
  // complete on disk when the process (which may exit right after Close)
  // goes away. This blocks Close, never a read (§5.2 step 4).
  st_->drainPersists();
  std::shared_ptr<FileEntry> e;
  std::shared_ptr<ColdFill> cold;
  {
    std::lock_guard<std::mutex> g(st_->mu);
    st_->closed = true;
    e.swap(st_->entry);
    cold.swap(st_->cold);
  }
#ifdef UCACHE_HAVE_COLDRUN
  coldDetach(cold); // the process's last handle on the file commits what it converted
#endif
#ifdef UCACHE_HAVE_PREFETCH
  if (globalConfig().prefetch && st_->prefetchSeen.load(std::memory_order_acquire))
    Prefetcher::instance().onClose(st_, e); // drops what was read ahead and never used
#endif
  if (e)
    e->flushAll(); // synchronous: staged pages + bitmap must hit disk before we may exit
  emitRelayObs(st_, e);
  // CPU-span evidence for the closed entry (the .cost sidecar). Off the Close
  // path (executor task); captures only values, never the plugin object.
  if (gOpenUCacheHandles.fetch_sub(1) > 1)
    st_->cpuBlended = true; // handles still open: the tail of this span overlaps
  if (e && st_->cpu0Us) {
    uint64_t cpu = processCpuUs() - st_->cpu0Us;
    std::string url = st_->url;
    bool blended = st_->cpuBlended;
    Executor::instance().post(
        [url, cpu, blended] { writeCostSidecar(url, globalConfig(), cpu, blended); });
  }
  // Read-ahead's own vector reads may still be on the origin. XrdCl answers a
  // Close with requests in flight errInvalidOp, and the application then
  // reports a failed close of a file it read perfectly well. onClose above
  // has already stopped new ones being issued for this handle, so this is a
  // bounded wait for what was already sent.
  if (!st_->waitInnerIdle(std::chrono::seconds(10)))
    return XRootDStatus(XrdCl::stError, XrdCl::errInvalidOp); // requests are left: not closed, as the client's own Close
  // A trusted cache-only handle that never hit a miss never opened the origin;
  // there is nothing to close remotely (UCACHE_REVALIDATE_S). Nor has a forked
  // child that never opened afresh the handle it inherited: that file is the
  // parent's.
  const bool pending = st_->reopenPending();
  XrdCl::File* in = st_->inner.load(); // one File for the check and the close
  if (pending || !in->IsOpen()) {
    if (handler)
      complete(handler, okStatus(), nullptr);
    return XRootDStatus();
  }
  return in->Close(handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::Stat(bool force, ResponseHandler* handler,
                                     ucache::XrdTimeout timeout) {
  st_->syncFork();
  st_->opsSeen.store(true, std::memory_order_relaxed); // a reader that stats first is no copy by read
  struct SizeShown { // every answer below gives a relocated handle's shown size
    UCacheFile* f;
    ~SizeShown() {
      if (f->shownSize())
        f->st_->copyGuard.sizeShown();
    }
  } sizeShown{this};
  // A trusted cache-only handle (UCACHE_REVALIDATE_S) answers Stat from local
  // metadata for BOTH force values — the whole point is to not touch the
  // origin. If setup degrades (entry gone), it opens the origin and we fall
  // through to the normal path.
  if (cacheOpen_) {
    ensureEntry();
    std::lock_guard<std::mutex> g(st_->mu);
    if (st_->cacheOnly && st_->statInfo && !st_->closed) {
      auto* obj = new AnyObject();
      obj->Set(new XrdCl::StatInfo(*st_->statInfo));
      complete(handler, okStatus(), obj);
      return XRootDStatus();
    }
  }
  // Set up eagerly (caller thread, like the first read): ROOT may Stat before
  // reading, and a handle shown a slot layout must report its size from the
  // very first answer -- forced or not, and for a handle a forked child is
  // setting up again as much as for a new one.
  if (st_->inner.load()->IsOpen() || cacheOpen_ || st_->reopenPending())
    ensureEntry();
  if (st_->layoutLost.load(std::memory_order_acquire))
    return lostLayoutError(st_);
  if (!force) {
    std::lock_guard<std::mutex> g(st_->mu);
    // Serve from validated metadata: only once the entry is
    // attached (validation done) and a StatInfo clone is on hand.
    if (st_->entry && st_->statInfo && !st_->closed) {
      auto* obj = new AnyObject();
      obj->Set(new XrdCl::StatInfo(*st_->statInfo));
      complete(handler, okStatus(), obj);
      return XRootDStatus();
    }
  } else if (uint64_t vsize = shownSize()) {
    // force=true forwards, but the size a handle shown a slot layout reports
    // must stay the layout's — the origin doesn't know about the extension.
    struct SizeRelay : ResponseHandler {
      ResponseHandler* user;
      uint64_t vsize;
      void HandleResponseWithHosts(XRootDStatus* s, AnyObject* r, HostList* h) override {
        if (s && s->IsOK() && r) {
          XrdCl::StatInfo* si = nullptr;
          r->Get(si);
          if (si)
            si->SetSize(vsize);
        }
        if (user)
          user->HandleResponseWithHosts(s, r, h);
        else {
          delete s;
          delete r;
          delete h;
        }
        delete this;
      }
    };
    if (st_->reopenPending() && !st_->ensureInnerOpen())
      return st_->missError(); // an inherited handle opens the origin afresh first
    auto* relay = new SizeRelay;
    relay->user = handler;
    relay->vsize = vsize;
    XRootDStatus s = st_->inner.load()->Stat(force, relay, timeout);
    if (!s.IsOK())
      delete relay;
    return s;
  }
  if (st_->reopenPending() && !st_->ensureInnerOpen())
    return st_->missError();
  return st_->inner.load()->Stat(force, handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::Read(uint64_t offset, uint32_t size, void* buffer,
                                     ResponseHandler* handler, ucache::XrdTimeout timeout) {
  return waited(st_, passthroughOnly_, handler, [&](ResponseHandler* h) {
    return readServe(offset, size, buffer, h, timeout);
  });
}

XrdCl::XRootDStatus UCacheFile::readServe(uint64_t offset, uint32_t size, void* buffer,
                                          ResponseHandler* handler, ucache::XrdTimeout timeout) {
  st_->syncFork();
  const std::pair<uint64_t, uint64_t> req{offset, size};
  auto entry = ensureEntry(st_->opsSeen.exchange(true) ? nullptr : &req);
  if (!entry && st_->layoutLost.load(std::memory_order_acquire))
    return lostLayoutError(st_);
  if (entry)
    noteRequestBytes(st_, size);
  // ONE sample of the layout, used for both. Taking it twice is not merely
  // wasteful: a layout set up between the two calls has the footprint recorded
  // in the original file's coordinates and the read served in the layout's,
  // which produces a WRONG signature rather than an absent one -- and a wrong
  // signature is evidence, so it is worse than none.
  auto cold = currentCold();
#ifdef UCACHE_HAVE_COLDRUN
  if (cold && offset + size >= offset) // the branches it touches, decoded in one read
    coldPrepare(*cold, offset, size);
#endif
  noteAppRead(st_, entry, offset, size, cold);
#ifdef UCACHE_HAVE_COLDRUN
  if (entry && cold) {
    // A slot layout's handle: served on the executor, as its layout exists
    // only here.
    if (offset + size < offset) // overflow: the origin has no such layout to answer from
      return XRootDStatus(XrdCl::stError, XrdCl::errInvalidArgs);
    // A read running past EOF is ORDINARY, not an error: readers that fetch in
    // fixed-size blocks (ROOT's raw-file layer uses 128 KiB) always overrun on
    // the last block, and the correct answer is a SHORT read. Relaying it to
    // the origin cannot give that answer — the layout is LARGER than the file
    // the origin has, so the origin returns nothing and the caller sees
    // nread == 0. Clamp to the shown size and serve what exists.
    uint64_t vsize = shownSize();
    // A read that starts past the handle's own end reads a position another
    // handle was shown (a compact map's range): bounded by every address
    // served. One that starts inside it ends at its end, as above.
    if (offset >= vsize)
      vsize = std::max(vsize, coldAddressEnd(*cold));
    const uint32_t served =
        offset >= vsize ? 0u : static_cast<uint32_t>(std::min<uint64_t>(size, vsize - offset));
    if (served == 0) { // at or past EOF, or a zero-length request: 0 bytes, not an error
      complete(handler, okStatus(), chunkResponse(offset, 0, buffer));
      return XRootDStatus();
    }
    if (!st_->copyGuard.allow(entry->fileSize(), shownSize(), offset, size))
      return refuseCopy(st_);
    auto st = st_;
    ChunkList one;
    one.emplace_back(offset, served, buffer);
    Executor::instance().post([st, entry, cold, one = std::move(one), handler]() mutable {
      coldServe(st, entry, cold, std::move(one), /*isVRead=*/false, handler);
    });
    return XRootDStatus();
  }
#endif
  if (!entry || offset + size < offset) {
    noteRelayBytes(st_, size, offset, size);
    return relayToInner(
        st_, handler, timeout,
        [=](XrdCl::File* f, ResponseHandler* rh, ucache::XrdTimeout t) {
          return f->Read(offset, size, buffer, rh, t);
        },
        entry ? 0 : size, offset, size);
  }
  // Past EOF is a SHORT read, and it must still be cached. Relaying it here
  // was correct in what it returned — the origin has the same file — but relay
  // bypasses the cache, so the last block of every file stayed uncached. Any
  // reader that fetches in fixed-size blocks overruns on its final block, so
  // that is the tail of EVERY file; for a container whose metadata lives at the
  // end (RNTuple keeps its page list and footer there) the effect is that the
  // metadata is never cached and no replica can ever be built from the entry.
  if (offset >= entry->fileSize() || size == 0) {
    complete(handler, okStatus(), chunkResponse(offset, 0, buffer));
    return XRootDStatus();
  }
  if (offset + size > entry->fileSize())
    size = static_cast<uint32_t>(entry->fileSize() - offset);

  if (entry->hasRange(offset, size)) {
    // HIT: disk read + verification on the executor (§5.3).
    auto st = st_;
    Executor::instance().post([st, entry, offset, size, buffer, handler] {
      uint64_t t0 = nowUs();
      if (entry->readCached(offset, size, buffer)) {
        if (st->store) {
          auto& stats = st->store->stats();
          stats.servedBytes.fetch_add(size, std::memory_order_relaxed);
          stats.hitReadUs.add(nowUs() - t0);
          if (stats.tracer)
            stats.tracer->rec("hit", entry->key().key, offset, size, nowUs() - t0);
        }
        st->noteCacheOk();
        complete(handler, okStatus(), chunkResponse(offset, size, buffer));
        return;
      }
      // CRC quarantine: demote to wire miss (counted by core).
      auto [ws, we] = roundChunk(*entry, offset, size);
      auto wireBuf = std::make_shared<std::vector<char>>(we - ws);
      if (XrdCl::File* f = st->acquireInner()) {
        auto* mh = new MissReadHandler(st, f, entry, offset, size, buffer, handler, ws,
                                       wireBuf, nowUs());
        XRootDStatus s = issueWireRead(st, f, ws, we - ws, wireBuf->data(), mh, 0);
        if (!s.IsOK()) {
          delete mh;
          st->releaseInner();
          complete(handler, new XRootDStatus(s), nullptr);
        }
      } else {
        complete(handler, new XRootDStatus(st->missError()), nullptr);
      }
    });
    return XRootDStatus();
  }

  // A file read directly (max_read_fraction): its data is relayed as asked,
  // not kept; its own records are fetched and kept below, as for any file.
  {
    ChunkList one;
    one.emplace_back(offset, size, buffer);
    if (auto rule = directRule(st_, entry, one); ruleReadsData(rule, offset, size)) {
      noteDirectBytes(st_, entry, size);
      return relayToInner(
          st_, handler, timeout,
          [=](XrdCl::File* f, ResponseHandler* rh, ucache::XrdTimeout t) {
            return f->Read(offset, size, buffer, rh, t);
          },
          0, offset, size);
    }
  }
  armKeyCheck(st_, entry, timeout);

  // MISS from the caller's thread: rounded wire read. Dedup: an identical
  // rounded miss already in flight (another handle/thread — RDF-IMT re-reads
  // the same baskets constantly) parks this read instead of duplicating the
  // fetch; the owner's staged pages then serve it from RAM.
  noteWaitTier(handler, ReaderWait::kOrigin);
  auto [ws, we] = roundChunk(*entry, offset, size);
  {
    auto st = st_;
    auto fired = std::make_shared<std::atomic<bool>>(false);
    auto redispatch = [st, entry, offset, size, buffer, handler, fired] {
      if (fired->exchange(true))
        return;
      Executor::instance().post([st, entry, offset, size, buffer, handler] {
        redispatchRead(st, entry, offset, size, buffer, handler);
      });
    };
    if (!entry->beginFetch(ws, we - ws, redispatch)) {
      Executor::instance().postAfter(10000, redispatch); // missed-endFetch insurance
      return XRootDStatus();
    }
  }
  auto wireBuf = std::make_shared<std::vector<char>>(we - ws);
  if (XrdCl::File* f = st_->acquireInner()) {
    auto* mh = new MissReadHandler(st_, f, entry, offset, size, buffer, handler, ws, wireBuf,
                                   nowUs(), we - ws, timeout);
    XRootDStatus s = issueWireRead(st_, f, ws, we - ws, wireBuf->data(), mh, timeout);
    if (!s.IsOK()) {
      delete mh;
      st_->releaseInner();
      entry->endFetch(ws, we - ws);
      return s;
    }
    return XRootDStatus();
  }
  // No usable inner (teardown, or a trusted handle whose lazy origin open
  // failed): fail with the ORIGIN's error when there is one — smart clients
  // (CMSSW XrdAdaptor) classify and retry it; errInvalidOp is terminal there.
  entry->endFetch(ws, we - ws);
  return st_->missError();
}

// §4.7: PgRead is pure pass-through, never cached, never served from cache —
// ROOT's remote-read path is Read/VectorRead (F5); xrdcp's pgread traffic is
// deliberately uncached in v1, and a copy's handle is pass-through anyway
// (CopyDetect.h). On a trusted-cache handle (UCACHE_REVALIDATE_S,
// default-on) the relay lazy-opens the origin first
// (relayToInner). EXCEPTION: a handle shown a slot layout must never pass
// PgRead through — the origin lacks the layout's bytes and has other bytes in
// its patched windows — so it is served from the layout with locally computed
// kXR page checksums (crc32c, same as the wire protocol).
XrdCl::XRootDStatus UCacheFile::PgRead(uint64_t offset, uint32_t size, void* buffer,
                                       ResponseHandler* handler, ucache::XrdTimeout timeout) {
  return waited(st_, passthroughOnly_, handler, [&](ResponseHandler* h) {
    return pgReadServe(offset, size, buffer, h, timeout);
  });
}

XrdCl::XRootDStatus UCacheFile::pgReadServe(uint64_t offset, uint32_t size, void* buffer,
                                            ResponseHandler* handler, ucache::XrdTimeout timeout) {
  st_->syncFork();
  const std::pair<uint64_t, uint64_t> req{offset, size};
  auto entry = ensureEntry(st_->opsSeen.exchange(true) ? nullptr : &req);
  if (!entry && st_->layoutLost.load(std::memory_order_acquire))
    return lostLayoutError(st_);
  if (entry)
    noteRequestBytes(st_, size); // a PgRead served here drives the same physical
                                 // reads as Read, so it must be sampled too
  // A PgRead is an application read like any other. Leaving it out of the
  // footprint meant a client that uses PgRead -- which some copy tools do by
  // default -- signed only whatever it happened to fetch through Read, so the
  // same work looked different depending on which call the reader chose. The
  // in-flight count and the width sample were missing for the same reason.
  // ONE sample of the layout, used for both (as in Read).
  auto cold = currentCold();
#ifdef UCACHE_HAVE_COLDRUN
  if (cold && offset + size >= offset) // the branches it touches, decoded in one read
    coldPrepare(*cold, offset, size);
#endif
  noteAppRead(st_, entry, offset, size, cold);
#ifdef UCACHE_HAVE_COLDRUN
  if (entry && cold) {
    if (offset + size < offset) // overflow
      return XRootDStatus(XrdCl::stError, XrdCl::errInvalidArgs);
    // Same clamp as Read: past EOF is a short read, and the origin cannot
    // produce one for a file that is larger here than it is there.
    const uint64_t pgVsize = shownSize();
    if (offset >= pgVsize || size == 0) {
      // An empty page set, as the origin answers: the caller reads the
      // response's PageInfo unconditionally.
      auto* obj = new AnyObject();
      obj->Set(new XrdCl::PageInfo(offset, 0, buffer, {}));
      complete(handler, okStatus(), obj);
      return XRootDStatus();
    }
    if (!st_->copyGuard.allow(entry->fileSize(), pgVsize, offset, size))
      return refuseCopy(st_);
    if (offset + size > pgVsize)
      size = static_cast<uint32_t>(pgVsize - offset);
    struct PgAdapter : ResponseHandler {
      ResponseHandler* user;
      uint64_t off;
      uint32_t size;
      void* buf;
      void HandleResponseWithHosts(XRootDStatus* s, AnyObject* r, HostList* h) override {
        std::unique_ptr<AnyObject> rr(r); // ChunkInfo response; rebuilt below
        std::unique_ptr<HostList> hh(h);
        if (s && s->IsOK()) {
          std::vector<uint32_t> cks;
          uint64_t pos = off;
          const auto* p = static_cast<const uint8_t*>(buf);
          while (pos < off + size) {
            uint64_t pageEnd = std::min<uint64_t>((pos / 4096 + 1) * 4096, off + size);
            cks.push_back(crc32c(p + (pos - off), pageEnd - pos));
            pos = pageEnd;
          }
          auto* obj = new AnyObject();
          obj->Set(new XrdCl::PageInfo(off, size, buf, std::move(cks)));
          complete(user, s, obj);
        } else {
          complete(user, s ? s : new XRootDStatus(XrdCl::stError, XrdCl::errInternal),
                   nullptr);
        }
        delete this;
      }
    };
    auto* pg = new PgAdapter;
    pg->user = handler;
    pg->off = offset;
    pg->size = size;
    pg->buf = buffer;
    auto st = st_;
    ChunkList one;
    one.emplace_back(offset, size, buffer);
    Executor::instance().post([st, entry, cold, one = std::move(one), pg]() mutable {
      coldServe(st, entry, cold, std::move(one), /*isVRead=*/false, pg);
    });
    return XRootDStatus();
  }
#endif
  noteRelayBytes(st_, size, offset, size);
  return relayToInner(
      st_, handler, timeout,
      [=](XrdCl::File* f, ResponseHandler* rh, ucache::XrdTimeout t) {
        return f->PgRead(offset, size, buffer, rh, t);
      },
      entry ? 0 : size, offset, size);
}

// The plain byte-cache vector-read serve, as a free function so a request that
// parks behind a fetch already in flight can re-enter it. `mayPark` is false on
// that second pass: it has already been counted as a fill, and one wait is
// enough -- whatever is still missing then goes to the origin.
static void servePlainVectorRead(const std::shared_ptr<HandleState>& st,
                                 const std::shared_ptr<FileEntry>& entry, ChunkList chunks,
                                 ResponseHandler* handler, bool mayPark) {
  // Atomic per-chunk classification (§5.2 step 1).
  std::vector<size_t> missIdx;
  for (size_t i = 0; i < chunks.size(); ++i)
    if (!entry->hasRange(chunks[i].offset, chunks[i].length))
      missIdx.push_back(i);
  if (!missIdx.empty()) // fetched, or waited for while another fetch brings it
    noteWaitTier(handler, ReaderWait::kOrigin);
  // Once per request, not once per pass: a read that parked and came back is
  // the same request, and counting it again inflated the chunk total by the
  // number of parked reads (6.5M -> 9.1M on a full pass).
  if (mayPark)
    noteVectorRequest(st, chunks);
  // max_read_fraction: a file read directly is neither read ahead nor waited
  // for; what it misses is fetched for this request only.
  std::shared_ptr<ReadRule> direct = missIdx.empty() ? nullptr : directRule(st, entry, chunks);
  if (!missIdx.empty())
    armKeyCheck(st, entry, 0); // as the wire vector read below
#ifdef UCACHE_HAVE_PREFETCH
  // Read-ahead learns from every fill and, once confirmed, fetches the next
  // one while the reader computes; posted to its own thread, never blocking.
  // Only on the first pass: a re-dispatched request is the same fill, and
  // counting it twice would move the frontier past baskets nobody asked for.
  if (mayPark && globalConfig().prefetch && !direct)
    Prefetcher::instance().onFill(st, entry, chunks, !missIdx.empty());
#endif

  // Are the bytes we are missing already coming? Read-ahead issues page-
  // rounded runs that a demand read would otherwise request again, and both
  // copies then cross the network. Wait for the one in flight instead. Only
  // ever parks once per request, and only when EVERY absent page is covered,
  // so a read can never wait on a fetch that is not happening.
  if (mayPark && !missIdx.empty() && globalConfig().prefetchJoin && !direct) {
    std::vector<std::pair<uint64_t, uint64_t>> want;
    want.reserve(missIdx.size());
    for (size_t i : missIdx)
      want.emplace_back(chunks[i].offset, chunks[i].length);
    ChunkList again = chunks;
    auto fired = std::make_shared<std::atomic<bool>>(false);
    // How long the reader waited for the copy already in flight. This is the
    // join's whole cost and nothing else records it: the thread sampler sees a
    // futex either way, the wire trace sees only requests this reader sent.
    // Recorded unsampled as trace op "join" (off/len = the request's first
    // missing chunk and its total missing bytes) so every wait is in the trace.
    const uint64_t parkedAt = nowUs();
    uint64_t missOff = chunks[missIdx.front()].offset, missLen = 0;
    for (size_t i : missIdx)
      missLen += chunks[i].length;
    auto redispatch = [st, entry, again = std::move(again), handler, fired, parkedAt, missOff,
                       missLen]() mutable {
      if (fired->exchange(true))
        return;
      if (st->store && st->store->stats().tracer)
        st->store->stats().tracer->rec("join", entry->key().key, missOff, missLen,
                                       nowUs() - parkedAt, /*sampled=*/false);
      Executor::instance().post([st, entry, again = std::move(again), handler]() mutable {
        servePlainVectorRead(st, entry, std::move(again), handler, /*mayPark=*/false);
      });
    };
    if (entry->waitForInFlight(want, redispatch)) {
      Executor::instance().postAfter(10000, redispatch); // the same insurance the single-read path keeps
      return;
    }
  }
  // Guarded like noteVectorRequest and onFill above, and for the same reason:
  // a request that parked and came back is the SAME request. Without this it
  // was counted once per pass.
  if (mayPark && st->store && !missIdx.empty() && missIdx.size() != chunks.size())
    st->store->stats().readvMixed.fetch_add(1,
                                             std::memory_order_relaxed); // serial hit+wire shape

  if (missIdx.size() == chunks.size()) {
    // All-miss: no disk stage; issue the wire read from this thread.
    issueMissVRead(st, entry, chunks, std::move(missIdx), handler, std::move(direct));
    return;
  }

  // Hit stage on the executor; misses (incl. CRC demotions) follow as one
  // wire vector read.
  ChunkList userChunks = chunks;
  Executor::instance().post(
      [st, entry, userChunks = std::move(userChunks), missIdx = std::move(missIdx), handler,
       direct = std::move(direct)]() mutable {
        uint64_t t0 = nowUs();
        uint64_t hitBytes = 0;
        std::vector<size_t> misses = std::move(missIdx);
        std::vector<bool> isMiss(userChunks.size(), false);
        for (size_t i : misses)
          isMiss[i] = true;
        for (size_t i = 0; i < userChunks.size(); ++i) {
          if (isMiss[i])
            continue;
          const auto& c = userChunks[i];
          if (entry->readCached(c.offset, c.length, c.buffer)) {
            hitBytes += c.length;
          } else {
            misses.push_back(i); // CRC demotion
          }
        }
        if (st->store && hitBytes)
          st->store->stats().hitReadUs.add(nowUs() - t0);
        if (misses.empty()) {
          if (st->store) {
            uint64_t total = 0;
            for (const auto& c : userChunks)
              total += c.length;
            st->store->stats().servedBytes.fetch_add(total, std::memory_order_relaxed);
          }
          st->noteCacheOk();
          complete(handler, okStatus(), vreadResponse(userChunks));
          return;
        }
        std::sort(misses.begin(), misses.end());
        issueMissVRead(st, entry, std::move(userChunks), std::move(misses), handler,
                       std::move(direct));
      });
  return;
}

XrdCl::XRootDStatus UCacheFile::VectorRead(const ChunkList& chunks, void* buffer,
                                           ResponseHandler* handler, ucache::XrdTimeout timeout) {
  return waited(st_, passthroughOnly_, handler, [&](ResponseHandler* h) {
    return vectorReadServe(chunks, buffer, h, timeout);
  });
}

XrdCl::XRootDStatus UCacheFile::vectorReadServe(const ChunkList& chunks, void* buffer,
                                                ResponseHandler* handler,
                                                ucache::XrdTimeout timeout) {
  st_->syncFork();
  st_->opsSeen.store(true, std::memory_order_relaxed);
  auto entry = ensureEntry();
  if (!entry && st_->layoutLost.load(std::memory_order_acquire))
    return lostLayoutError(st_);
  // ONE sample, for the footprint AND the serving decision below. The loop had
  // its own and the decision took another, so a replica published between them
  // recorded the chunks in the original file's coordinates and served them in
  // the replica's -- a WRONG signature rather than an absent one.
  auto cold = currentCold();
  // The combined-buffer variant, where this handle shows a layout the origin
  // does not have (relaying it would send offsets the origin lacks): served as
  // the per-chunk form, each chunk given its place in the buffer.
  if (entry && buffer && !chunks.empty() && cold) {
    ChunkList placed = chunks;
    auto* at = static_cast<char*>(buffer);
    for (auto& c : placed) {
      c.buffer = at;
      at += c.length;
    }
    return vectorReadServe(placed, nullptr, handler, timeout);
  }
#ifdef UCACHE_HAVE_COLDRUN
  if (cold) // the branches it touches, decoded in one read
    coldPrepare(*cold, chunks);
#endif
  noteAppChunks(st_, entry, chunks, cold);
  // The combined-buffer variant is legacy and rare: pass through unchanged.
  if (!entry || buffer || chunks.empty()) {
    noteRelayChunks(st_, chunks);
    const auto kept = keepChunks(*st_, chunks);
    return relayToInner(
        st_, handler, timeout,
        [kept, &chunks, buffer](XrdCl::File* f, ResponseHandler* rh, ucache::XrdTimeout t) {
          return f->VectorRead(kept ? *kept : chunks, buffer, rh, t);
        },
        0, chunksFirst(chunks), chunksBytes(chunks), kept != nullptr);
  }

#ifdef UCACHE_HAVE_COLDRUN
  if (cold) {
    // Cold-run entry: whole vector served on the executor. A chunk outside the
    // layout cannot be relayed -- the origin does not have this layout.
    const uint64_t vsize = coldAddressEnd(*cold);
    for (const auto& c : chunks)
      if (c.offset + c.length < c.offset || c.offset + c.length > vsize)
        return XRootDStatus(XrdCl::stError, XrdCl::errInvalidArgs);
    if (!guardAllowsChunks(st_, entry->fileSize(), shownSize(), chunks))
      return refuseCopy(st_);
    auto st = st_;
    noteVectorRequest(st_, chunks);
    ChunkList userChunks = chunks;
    Executor::instance().post(
        [st, entry, cold, userChunks = std::move(userChunks), handler]() mutable {
          coldServe(st, entry, cold, std::move(userChunks), /*isVRead=*/true, handler);
        });
    return XRootDStatus();
  }
#endif

  for (const auto& c : chunks)
    if (c.length == 0 || c.offset + c.length > entry->fileSize()) {
      noteRelayChunks(st_, chunks);
      const auto kept = keepChunks(*st_, chunks);
      return relayToInner(
          st_, handler, timeout,
          [kept, &chunks, buffer](XrdCl::File* f, ResponseHandler* rh, ucache::XrdTimeout t) {
            return f->VectorRead(kept ? *kept : chunks, buffer, rh, t);
          },
          0, chunksFirst(chunks), chunksBytes(chunks), kept != nullptr);
    }

  servePlainVectorRead(st_, entry, chunks, handler, /*mayPark=*/true);
  return XRootDStatus();
}

void UCacheFile::invalidateOnWrite() {
  std::shared_ptr<FileEntry> e;
  std::shared_ptr<ColdFill> cold;
  {
    std::lock_guard<std::mutex> g(st_->mu);
    e.swap(st_->entry);
    cold.swap(st_->cold);
    st_->tripped = true; // no caching on this handle after a write
  }
#ifdef UCACHE_HAVE_COLDRUN
  coldDetach(cold);
#endif
  if (st_->store) {
    auto store = st_->store;
    auto key = UrlKey::parse(st_->url, globalConfig().keepCgi);
    if (key)
      Executor::instance().post([store, key] { store->invalidate(*key); });
  }
}

XrdCl::XRootDStatus UCacheFile::Write(uint64_t offset, uint32_t size, const void* buffer,
                                      ResponseHandler* handler, ucache::XrdTimeout timeout) {
  st_->syncFork();
  invalidateOnWrite();
  return st_->inner.load()->Write(offset, size, buffer, handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::Write(uint64_t offset, XrdCl::Buffer&& buffer,
                                      ResponseHandler* handler, ucache::XrdTimeout timeout) {
  st_->syncFork();
  invalidateOnWrite();
  return st_->inner.load()->Write(offset, std::move(buffer), handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::VectorWrite(const ChunkList& chunks,
                                            ResponseHandler* handler, ucache::XrdTimeout timeout) {
  st_->syncFork();
  invalidateOnWrite();
  return st_->inner.load()->VectorWrite(chunks, handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::WriteV(uint64_t offset, const struct iovec* iov, int iovcnt,
                                       ResponseHandler* handler, ucache::XrdTimeout timeout) {
  st_->syncFork();
  invalidateOnWrite();
  return st_->inner.load()->WriteV(offset, iov, iovcnt, handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::Sync(ResponseHandler* handler, ucache::XrdTimeout timeout) {
  st_->syncFork();
  return st_->inner.load()->Sync(handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::Truncate(uint64_t size, ResponseHandler* handler,
                                         ucache::XrdTimeout timeout) {
  st_->syncFork();
  invalidateOnWrite();
  return st_->inner.load()->Truncate(size, handler, timeout);
}

// An armed handle's File a forked child inherited is in the client's Error state, where a
// call returns the File's old status (OK) and never completes its handler: such a handle
// opens the origin afresh first (Stat's way).
XrdCl::XRootDStatus UCacheFile::Fcntl(const XrdCl::Buffer& arg, ResponseHandler* handler,
                                      ucache::XrdTimeout timeout) {
  st_->syncFork();
  if (st_->moveOnReads && st_->reopenPending() && !st_->ensureInnerOpen())
    return st_->missError();
  return st_->inner.load()->Fcntl(arg, handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::Visa(ResponseHandler* handler, ucache::XrdTimeout timeout) {
  st_->syncFork();
  if (st_->moveOnReads && st_->reopenPending() && !st_->ensureInnerOpen())
    return st_->missError();
  return st_->inner.load()->Visa(handler, timeout);
}

bool UCacheFile::IsOpen() const {
  // The client puts a File inherited by a forked child, whose read recovery is
  // off, in its Error state, where it is not open and answers a call with its old
  // status without completing it: an armed handle's reopen is the plugin's.
  if (st_->moveOnReads) {
    st_->syncFork();
    if (st_->reopenPending())
      return true;
  }
  return cacheOpen_ || st_->inner.load()->IsOpen();
}

bool UCacheFile::SetProperty(const std::string& name, const std::string& value) {
  st_->syncFork();
  if (globalConfig().adaptiveFederation) {
    // Kept, so that a File that replaces this one -- the open retried or made
    // at another site -- is given it too (resetInner). Recorded before it is
    // set: a File swapped in after this is either given it there or is the
    // one loaded below.
    std::lock_guard<std::mutex> g(st_->mu);
    st_->readerProps.emplace_back(name, value);
  }
  // On an armed handle the reader cannot turn the client's read recovery back
  // on: the handle's own reads depend on it being off.
  const bool recovery = name == "ReadRecovery" || name == "WriteRecovery";
  if (recovery && st_->moveOnReads)
    return true;
  const bool ok = st_->inner.load()->SetProperty(name, value);
  if (recovery && st_->moveOnReads) // armed while this was set
    recoveryOff(*st_->inner.load());
  return ok;
}

bool UCacheFile::GetProperty(const std::string& name, std::string& value) const {
  st_->syncFork();
  if (st_->inner.load()->GetProperty(name, value) && !value.empty())
    return true;
  // Trusted cache-only handles never opened a transport, so the properties
  // clients act on are unset. Native XrdCl has a contract for exactly this
  // situation — the LOCAL REDIRECT: LastURL = file://localhost/<path> makes
  // both ROOT (TNetXNGFile::GetVectorReadLimits) and uproot
  // (get_server_config) take their local-file branch: parseable URL (uproot
  // dies on None), sane readv defaults, and NO follow-up server query — a
  // warm open must never touch the network (an offline-warm leg gates this;
  // synthesizing a real host here hung offline warm reads on the clients'
  // FileSystem config query). The path is the URL's (never the cache's .data
  // file: a sparse page file read directly would be silent corruption; this
  // path fails loudly if anything opens it).
  //
  // DataServer is answered with an EMPTY value, never a real host (the same
  // origin-may-be-down reason) and never "no such property". ROOT and uproot's
  // own XRootD source take the LastURL branch above and never ask for it;
  // fsspec-xrootd -- uproot 5's default transport -- asks for nothing else,
  // and in Python an unset property arrives as None: it takes its no-server
  // defaults on "" and died in client.URL(None) on the second open of every
  // cached file. Fail-open means answering what each client can act on.
  //
  // A copy's handle never takes this path: it is never trusted cache-only, so
  // the lines above return the origin's own values, and a checksum asked for at
  // the end of a copy (xrdcp --cksum, gfal-copy -K) is asked of the origin.
  if (name == "LastURL") {
    std::lock_guard<std::mutex> g(st_->mu);
    if (!st_->url.empty()) {
      value = "file://localhost" + XrdCl::URL(st_->url).GetPath();
      return true;
    }
  }
  if (name == "DataServer") {
    value.clear();
    return true;
  }
  return false;
}

} // namespace ucache
