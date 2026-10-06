#include "OpenPolicy.h"

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
    break;
  }
  return "fork";
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
