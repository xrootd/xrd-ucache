#include "OpenPolicy.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

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
                   bool excluded) {
  if (!v.viaRedirector) // a direct open has nowhere else to go
    return OpenStep::kFinal;
  if (code == 0)
    return sizeDiffers || offline ? OpenStep::kFailOver : OpenStep::kFinal;
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
  case kErrOperationExpired:
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

std::vector<std::string> exclusionNames(const std::vector<SiteFailure>& failed) {
  std::vector<std::string> out;
  for (const SiteFailure& x : failed)
    if (!x.name.empty() && std::find(out.begin(), out.end(), x.name) == out.end())
      out.push_back(x.name);
  return out;
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
