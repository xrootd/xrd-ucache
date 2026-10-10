#include "OpenPolicy.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ucache {

namespace {
using namespace xrdwire;

bool isIpv4(const std::string& h) {
  int dots = 0, digits = 0;
  for (char c : h) {
    if (c == '.') {
      if (digits == 0)
        return false;
      ++dots;
      digits = 0;
    } else if (c >= '0' && c <= '9') {
      if (++digits > 3)
        return false;
    } else {
      return false;
    }
  }
  return dots == 3 && digits > 0;
}

// An IPv6 address, bracketed as in a URL or bare: no host name has a colon.
bool isIpv6(const std::string& h) {
  return h.find(':') != std::string::npos || (!h.empty() && h[0] == '[');
}
} // namespace

Hop makeHop(const std::string& host, int port, uint32_t flags, bool loadBalancer,
            const std::string& query) {
  Hop h;
  h.host = host;
  h.port = port;
  h.flags = flags;
  h.loadBalancer = loadBalancer;
  // Parameter names only: "cap.msg=...&cap.sym=...&mgm.id=..." on the hop to an
  // EOS data server.
  size_t at = (!query.empty() && query[0] == '?') ? 1 : 0;
  while (at < query.size()) {
    size_t end = query.find('&', at);
    if (end == std::string::npos)
      end = query.size();
    const size_t eq = query.find('=', at);
    const size_t nameEnd = (eq == std::string::npos || eq > end) ? end : eq;
    const std::string name = query.substr(at, nameEnd - at);
    if (name == "cap.msg" || name == "cap.sym")
      h.eosFst = true;
    at = end + 1;
  }
  return h;
}

ChainView readChain(const std::vector<Hop>& hops) {
  ChainView v;
  v.hops = hops.size();
  v.last = static_cast<int>(hops.size()) - 1;
  for (size_t i = 0; i < hops.size(); ++i) {
    const uint32_t f = hops[i].flags;
    if (f & kIsManager) {
      v.viaRedirector = true;
      if (v.regional < 0 && !(f & kAttrMeta))
        v.regional = static_cast<int>(i);
    } else if ((f & kIsServer) && i + 1 < hops.size()) {
      v.effective = static_cast<int>(i); // a data server that sent the open on
    }
  }
  if (v.effective < 0 && v.last >= 0 && !(hops[static_cast<size_t>(v.last)].flags & kIsManager))
    v.effective = v.last;
  return v;
}

std::string chainText(const std::vector<Hop>& hops) {
  std::string out;
  char flags[16];
  for (const Hop& h : hops) {
    if (!out.empty())
      out += ',';
    std::snprintf(flags, sizeof flags, "/0x%x", h.flags);
    out += h.host;
    out += ':';
    out += std::to_string(h.port);
    out += flags;
    if (h.loadBalancer)
      out += '*';
    if (h.eosFst)
      out += '^';
  }
  return out;
}

std::string siteOf(const std::string& host) {
  if (host.empty() || isIpv4(host) || isIpv6(host))
    return host;
  std::string h = host;
  for (char& c : h)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  while (!h.empty() && h.back() == '.') // a fully qualified name's root dot
    h.pop_back();
  const size_t dot = h.find('.');
  if (dot == std::string::npos || dot + 1 >= h.size())
    return h;
  return h.substr(dot + 1);
}

FailureClass classifyFailure(int code, int errNo) {
  switch (code) {
  case 0:
    return FailureClass::kNone;
  case kErrSocketError:
  case kErrSocketTimeout:
  case kErrSocketDisconnected:
  case kErrStreamDisconnect:
  case kErrConnectionError:
  case kErrInvalidSession:
  case kErrTlsError:
  case kErrAuthFailed:
    return FailureClass::kClient;
  case kErrOperationExpired:
    return FailureClass::kExpired;
  case kErrRedirectLimit:
    return FailureClass::kSite;
  case kErrNotFound:
    return FailureClass::kNotFound;
  case kErrDataError:
    return FailureClass::kData;
  case kErrErrorResponse:
    switch (errNo) {
    case kXrNotAuthorized:
      return FailureClass::kClient;
    case kXrNotFound:
      return FailureClass::kNotFound;
    case kXrFileLocked:
    case kXrFSError:
    case kXrIOError:
    case kXrServerError:
    case kXrCancelled:
    case kXrItExists:
    case kXrOverloaded:
      return FailureClass::kSite;
    default: // kXrNoServer among them: see the header
      return FailureClass::kOther;
    }
  default:
    return FailureClass::kOther;
  }
}

const char* failureClassName(FailureClass c) {
  switch (c) {
  case FailureClass::kNone:
    return "none";
  case FailureClass::kClient:
    return "client";
  case FailureClass::kSite:
    return "site";
  case FailureClass::kNotFound:
    return "notfound";
  case FailureClass::kData:
    return "data";
  case FailureClass::kExpired:
    return "expired";
  case FailureClass::kOther:
    break;
  }
  return "other";
}

const char* openRouteName(OpenRoute r) {
  switch (r) {
  case OpenRoute::kRelay:
    return "relay";
  case OpenRoute::kLazy:
    return "lazy";
  case OpenRoute::kDegraded:
    return "degraded";
  case OpenRoute::kFork:
    return "fork";
  case OpenRoute::kCandidate:
    return "candidate";
  case OpenRoute::kRead:
    return "read";
  case OpenRoute::kFailover:
    break;
  }
  return "failover";
}

namespace {
// Appends c, escaped, to out unless that would pass `max` bytes; false then.
bool appendEscaped(std::string& out, char c, size_t max) {
  char b[8];
  size_t n = 1;
  if (c == '"' || c == '\\') {
    b[0] = '\\';
    b[1] = c;
    n = 2;
  } else if (static_cast<unsigned char>(c) < 0x20) {
    std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
    n = 6;
  } else {
    b[0] = c;
  }
  if (out.size() + n > max)
    return false;
  out.append(b, n);
  return true;
}
} // namespace

int openDeadlineS(int readerTimeoutS, int deadlineS) {
  const int d = deadlineS > 0 ? deadlineS : kOpenDeadlineS;
  return readerTimeoutS > 0 && readerTimeoutS < d ? readerTimeoutS : d;
}

OpenStep stepAfter(const ChainView& v, int code, int errNo, bool sizeDiffers, bool offline,
                   bool excluded, bool repeats, bool regionalKnown) {
  if (!v.viaRedirector) // a direct open has nowhere else to go
    return OpenStep::kFinal;
  if (code == 0)
    return sizeDiffers || offline ? OpenStep::kFailOver
           : repeats              ? OpenStep::kLocate
                                  : OpenStep::kFinal;
  const bool first = v.hops <= 1; // the redirector answered (or failed) itself
  const OpenStep elsewhere = first ? OpenStep::kFinal : OpenStep::kFailOver;
  // Not found by the redirector itself, with nothing of ours hiding a copy: no
  // site has it. Otherwise the client's own retry at its load balancer found
  // none either, though a listed copy may be missing or hidden.
  const OpenStep notFound = first && !excluded ? OpenStep::kFinal : OpenStep::kLocate;
  switch (code) {
  case kErrRedirectLimit:
    return OpenStep::kFailOver;
  case kErrNotFound:
    return notFound;
  case kErrOperationExpired: // the server stalled: the retry would meet its site's other server
    return regionalKnown && !first ? OpenStep::kLocate : elsewhere;
  case kErrSocketTimeout:
  case kErrConnectionError:
    return elsewhere;
  case kErrErrorResponse:
    switch (errNo) {
    case kXrNotFound:
      return notFound;
    case kXrNotAuthorized: // the client retries it only at a global redirector
    case kXrFileLocked:
    case kXrFSError:
    case kXrIOError:
    case kXrServerError:
    case kXrNoServer:
    case kXrCancelled:
    case kXrItExists:
    case kXrOverloaded:
      return elsewhere;
    default:
      return OpenStep::kFinal;
    }
  default:
    return OpenStep::kFinal;
  }
}

OpenStep stepAfterHeaderRead(const ChainView& v, int code, int errNo, bool excluded,
                             bool regionalKnown) {
  const OpenStep s = stepAfter(v, code, errNo, false, false, excluded, false, regionalKnown);
  return s == OpenStep::kFinal && v.viaRedirector && v.hops > 1 ? OpenStep::kFailOver : s;
}

bool clientRecoversRead(int code) {
  switch (code) {
  case kErrInternal:
  case kErrSocketError:
  case kErrSocketTimeout:
  case kErrInvalidSession:
  case kErrTlsError:
  case kErrOperationInterrupted:
  case kErrRedirect:
    return true;
  default:
    return false;
  }
}

bool rootNamed(const std::string& path) {
  const size_t end = path.find('?');
  const size_t n = end == std::string::npos ? path.size() : end;
  if (n < 5)
    return false;
  static const char kExt[] = ".root";
  for (size_t i = 0; i < 5; ++i)
    if (std::tolower(static_cast<unsigned char>(path[n - 5 + i])) != kExt[i])
      return false;
  return true;
}

bool emptyRootCopy(const std::string& path, long long size) {
  return size == 0 && rootNamed(path);
}

size_t rootHeaderBytes(long long size) {
  return size <= 0 ? 0 : static_cast<size_t>(std::min<long long>(size, kRootHeaderBytes));
}

namespace {
uint64_t bigEndian(const unsigned char* b, size_t at, int bytes) {
  uint64_t v = 0;
  for (int i = 0; i < bytes; ++i)
    v = v << 8 | b[at + static_cast<size_t>(i)];
  return v;
}
// Where the end's field of a header with these first bytes ends: 16, or 20 for a 64-bit end;
// 0 when the version is not there yet.
size_t endFieldEnd(const unsigned char* b, size_t n) {
  if (n < 8)
    return 0;
  return bigEndian(b, 4, 4) >= 1000000 ? 20 : 16;
}
} // namespace

long long rootFileEnd(const unsigned char* b, size_t n) {
  const size_t need = endFieldEnd(b, n);
  if (need == 0 || n < need || std::memcmp(b, "root", 4) != 0)
    return -1;
  const uint64_t end = need == 20 ? bigEndian(b, 12, 8) : bigEndian(b, 12, 4);
  return end > static_cast<uint64_t>(LLONG_MAX) ? LLONG_MAX : static_cast<long long>(end);
}

RootHeaderVerdict rootHeaderVerdict(const unsigned char* b, size_t n, long long size) {
  RootHeaderVerdict v;
  const std::string sz = "size " + std::to_string(size) + ": ";
  const size_t want = rootHeaderBytes(size);
  if (n < want) {
    v.kind = RootHeaderVerdict::kBad;
    v.why = sz + "a read of its first " + std::to_string(want) + " bytes answered " +
            std::to_string(n);
    return v;
  }
  if (std::memcmp(b, "root", std::min<size_t>(n, 4)) != 0) {
    v.kind = RootHeaderVerdict::kBad;
    v.why = sz + "it does not begin with \"root\", as every ROOT file does";
    return v;
  }
  v.end = rootFileEnd(b, n);
  if (v.end < 0) {
    v.kind = RootHeaderVerdict::kBad;
    v.why = sz + "too short to hold a ROOT file's header";
    return v;
  }
  if (v.end > size) {
    v.kind = RootHeaderVerdict::kBad;
    v.why = sz + "its header says the file is " + std::to_string(v.end) + " bytes";
    return v;
  }
  v.kind = v.end == size ? RootHeaderVerdict::kConfirmed : RootHeaderVerdict::kNoClaim;
  return v;
}

int headerReadTimeoutS(int attemptLimitS, int64_t elapsedMs) {
  const int64_t limitMs = int64_t(attemptLimitS > 0 ? attemptLimitS : kOpenDeadlineS) * 1000;
  const int64_t leftS = (limitMs - std::max<int64_t>(elapsedMs, 0) + 999) / 1000;
  return static_cast<int>(std::max<int64_t>(kHeaderReadFloorS, std::min<int64_t>(leftS, limitMs / 1000)));
}

std::string exclusionName(const std::vector<Hop>& hops, int i) {
  if (i <= 0 || static_cast<size_t>(i) >= hops.size())
    return std::string();
  const Hop& h = hops[static_cast<size_t>(i)];
  if ((h.flags & kIsManager) || h.eosFst || isIpv4(h.host) || isIpv6(h.host))
    return std::string();
  std::string name = h.host;
  for (char& c : name)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  while (!name.empty() && name.back() == '.')
    name.pop_back();
  return name.find('.') == std::string::npos ? std::string() : name;
}

int budgetS(int readerTimeoutS, int sewS, int trS, int maxS) {
  int b = maxS > 0 ? maxS : kFailoverMaxS;
  if (readerTimeoutS > 0)
    b = std::min(b, readerTimeoutS);
  return std::max(std::min(b, sewS - trS - 5), 1);
}

bool budgetSpent(int64_t remainingMs, int attemptDeadlineS, int readerTimeoutS) {
  int64_t floorMs = std::min<int64_t>(attemptDeadlineS, 30) * 1000;
  if (readerTimeoutS > 0)
    floorMs = std::min<int64_t>(floorMs, int64_t(readerTimeoutS) * 500);
  return remainingMs < floorMs;
}

AfterFailure afterFailure(std::vector<SiteFailure>& failed, const SiteFailure& f, int maxSites,
                          bool spent) {
  bool seen = false, sameSite = false;
  for (const SiteFailure& x : failed) {
    seen = seen || x.server == f.server;
    sameSite = sameSite || x.site == f.site;
  }
  if (!seen)
    failed.push_back(f);
  std::vector<std::string> sites;
  for (const SiteFailure& x : failed)
    if (std::find(sites.begin(), sites.end(), x.site) == sites.end())
      sites.push_back(x.site);
  if (static_cast<int>(sites.size()) >= maxSites || spent)
    return AfterFailure::kStop;
  return seen || sameSite ? AfterFailure::kRepeat : AfterFailure::kRetry;
}

Location locationOf(const std::string& address, bool manager, bool pending) {
  Location l;
  const auto colon = address.rfind(':');
  l.host = address.substr(0, colon);
  if (l.host.size() > 1 && l.host.front() == '[' && l.host.back() == ']')
    l.host = l.host.substr(1, l.host.size() - 2);
  if (l.host.compare(0, 7, "::ffff:") == 0)
    l.host.erase(0, 7);
  else if (l.host.compare(0, 2, "::") == 0 && l.host.find('.') != std::string::npos)
    l.host.erase(0, 2);
  l.port = colon == std::string::npos ? 1094 : std::atoi(address.c_str() + colon + 1);
  l.manager = manager;
  l.pending = pending;
  return l;
}

bool copyOffline(uint32_t statFlags, const std::string& server, const std::string* testOffline) {
  return (statFlags & kStatOffline) ||
         (testOffline && testOffline->find("," + server + ",") != std::string::npos);
}

std::vector<Location> orderLocations(const std::vector<Location>& listed,
                                     const std::vector<std::string>& readerTried,
                                     const std::vector<SiteFailure>& failed) {
  const auto lower = [](std::string s) {
    for (char& c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  std::vector<std::string> tried;
  for (const std::string& t : readerTried)
    tried.push_back(lower(t.substr(0, t.find(':'))));
  std::vector<Location> online, pending;
  std::vector<std::pair<size_t, Location>> excluded; // with the position of its failure
  for (const Location& l : listed) {
    const std::string host = lower(l.host), server = host + ":" + std::to_string(l.port);
    if (std::find(tried.begin(), tried.end(), host) != tried.end())
      continue;
    const bool literal = isIpv4(host) || isIpv6(host);
    size_t age = failed.size();
    const std::string site = siteOf(host);
    for (size_t i = 0; i < failed.size() && age == failed.size(); ++i) {
      // A server that never answered is its own site (host:port), but the
      // domain of its name is where its siblings are.
      const std::string& fs = failed[i].server;
      if (lower(fs) == server ||
          (!literal && (failed[i].site == site || siteOf(fs.substr(0, fs.rfind(':'))) == site)))
        age = i;
    }
    const bool refused = std::any_of(failed.begin(), failed.end(), [&](const SiteFailure& x) {
      return x.bad && lower(x.server) == server;
    });
    if (age < failed.size()) {
      if (!refused)
        excluded.emplace_back(age, l);
    } else
      (l.pending ? pending : online).push_back(l);
  }
  online.insert(online.end(), pending.begin(), pending.end());
  if (online.empty()) {
    std::stable_sort(excluded.begin(), excluded.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& e : excluded)
      online.push_back(e.second);
  }
  return online;
}

std::vector<std::string> exclusionNames(const std::vector<SiteFailure>& failed) {
  std::vector<std::string> out;
  for (const SiteFailure& x : failed)
    if (!x.name.empty() && std::find(out.begin(), out.end(), x.name) == out.end())
      out.push_back(x.name);
  return out;
}

bool failedBefore(const std::vector<SiteFailure>& failed, const SiteFailure& f) {
  const std::string host = f.server.substr(0, f.server.rfind(':'));
  for (const SiteFailure& x : failed)
    if (x.server == f.server || (x.site == f.site && f.site != host))
      return true;
  return false;
}

bool armedForReads(bool adaptiveFederation, bool cacheServes, bool viaRedirector, bool slotLayout) {
  return adaptiveFederation && cacheServes && viaRedirector && !slotLayout;
}

bool shortInsideConfirmed(uint64_t confirmed, uint64_t off, uint64_t len, uint64_t got) {
  return confirmed && len && got < len && off + len >= off && off + len <= confirmed;
}

int readDeadlineS(const char* text) {
  char* end = nullptr;
  const long v = text ? std::strtol(text, &end, 10) : 0;
  return text && end != text && *end == '\0' && v > 0 && v < INT_MAX ? static_cast<int>(v)
                                                                     : kReadDeadlineS;
}

int readBudgetS(int readerTimeoutS, int requestTimeoutS) {
  if (readerTimeoutS > 0)
    return readerTimeoutS;
  return requestTimeoutS > 0 ? requestTimeoutS : kReadDeadlineS;
}

int readAttemptTimeoutS(int readerTimeoutS, int requestTimeoutS, int deadlineS, int elapsedS,
                        bool again) {
  const int left = readBudgetS(readerTimeoutS, requestTimeoutS) - elapsedS;
  int floorS = std::min(deadlineS, 30);
  if (readerTimeoutS > 0)
    floorS = std::min(floorS, readerTimeoutS / 2);
  if (again && left < std::max(floorS, 1))
    return 0;
  return std::max(std::min(deadlineS, left), 1);
}

ReadStep readStep(int code, int errNo) {
  static constexpr int kCodes[] = {
      kErrInternal,       kErrSocketError,    kErrSocketTimeout,    kErrSocketDisconnected,
      kErrPollerError,    kErrSocketOptError, kErrStreamDisconnect, kErrConnectionError,
      kErrInvalidSession, kErrTlsError,       kErrOperationExpired, kErrOperationInterrupted};
  static constexpr int kServer[] = {kXrFileNotOpen,   kXrFSError,    kXrIOError,     kXrNoMemory,
                                    kXrNotAuthorized, kXrNotFound,   kXrServerError, kXrNoServer,
                                    kXrOverloaded,    kXrReqTimedOut};
  const auto in = [](const auto& list, int v) {
    return std::find(std::begin(list), std::end(list), v) != std::end(list);
  };
  const bool moves = in(kCodes, code) || (code == kErrErrorResponse && in(kServer, errNo));
  return moves ? ReadStep::kMove : ReadStep::kFinal;
}

namespace {
// `t` with every URL query removed (a query runs to the next blank, where its
// URL ends) and cut to at most `max` bytes.
std::string withoutQueries(const std::string& t, size_t max) {
  std::string out;
  for (size_t i = 0; i < t.size() && out.size() < max; ++i) {
    if (t[i] == '?') {
      while (i + 1 < t.size() && t[i + 1] != ' ' && t[i + 1] != '\t' && t[i + 1] != '\n')
        ++i;
      continue;
    }
    out += t[i];
  }
  return out;
}
} // namespace

std::string sitesFailedList(const std::vector<SiteFailure>& failed) {
  std::string out;
  for (size_t i = 0; i < failed.size(); ++i) {
    if (i)
      out += "; ";
    out += failed[i].site + " (" + failed[i].server + "): " + withoutQueries(failed[i].error, 160);
  }
  return out;
}

std::string allSitesFailedText(const std::string& file, const std::vector<SiteFailure>& failed) {
  const std::string out =
      "uCache: " + withoutQueries(file, 300) + ": no site could open it: " + sitesFailedList(failed);
  return out.size() > 1024 ? out.substr(0, 1024) : out;
}

std::string escapeForJson(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s)
    appendEscaped(out, c, std::string::npos);
  return out;
}

std::string recordText(const std::string& t, size_t max) {
  std::string out;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] == '?') { // the query runs to the next blank, where its URL ends
      while (i + 1 < t.size() && t[i + 1] != ' ' && t[i + 1] != '\t' && t[i + 1] != '\n')
        ++i;
      continue;
    }
    if (!appendEscaped(out, t[i], max))
      break;
  }
  return out;
}

} // namespace ucache
