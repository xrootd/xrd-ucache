#include "UrlKey.h"

#include "vendor/sha256.h"

#include <algorithm>
#include <cctype>
#include <fnmatch.h>
#include <map>
#include <sstream>

namespace ucache {
namespace {

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

int defaultPort(const std::string& scheme) {
  if (scheme == "root" || scheme == "xroot" || scheme == "roots" || scheme == "xroots")
    return 1094;
  if (scheme == "http")
    return 80;
  if (scheme == "https")
    return 443;
  return 0;
}

// Collapse duplicate slashes and resolve "." / ".." without escaping root.
std::string normalizePath(const std::string& in) {
  std::vector<std::string> segs;
  std::string seg;
  std::istringstream is(in);
  while (std::getline(is, seg, '/')) {
    if (seg.empty() || seg == ".")
      continue;
    if (seg == "..") {
      if (!segs.empty())
        segs.pop_back();
      continue;
    }
    segs.push_back(seg);
  }
  std::string out;
  for (const auto& s : segs)
    out += "/" + s;
  return out.empty() ? "/" : out;
}

} // namespace

std::optional<UrlKey> UrlKey::parse(const std::string& url,
                                    const std::vector<std::string>& keepCgi) {
  auto schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos || schemeEnd == 0)
    return std::nullopt;
  std::string scheme = toLower(url.substr(0, schemeEnd));

  size_t authStart = schemeEnd + 3;
  size_t pathStart = url.find('/', authStart);
  if (pathStart == std::string::npos)
    return std::nullopt;
  std::string auth = url.substr(authStart, pathStart - authStart);
  if (auth.empty())
    return std::nullopt;
  // Drop userinfo if present (never part of the key).
  if (auto at = auth.rfind('@'); at != std::string::npos)
    auth = auth.substr(at + 1);

  std::string host = auth;
  int port = defaultPort(scheme);
  if (!auth.empty() && auth.front() == '[') {
    // [ipv6] or [ipv6]:port — brackets are mandatory for v6 literals.
    auto close = auth.find(']');
    if (close == std::string::npos)
      return std::nullopt;
    host = auth.substr(0, close + 1);
    if (close + 1 < auth.size() && auth[close + 1] == ':')
      port = std::atoi(auth.c_str() + close + 2);
  } else if (auto colon = auth.rfind(':'); colon != std::string::npos) {
    host = auth.substr(0, colon);
    port = std::atoi(auth.c_str() + colon + 1);
  }
  host = toLower(host);
  if (host.empty() || port <= 0)
    return std::nullopt;

  std::string rest = url.substr(pathStart);
  std::string cgi;
  if (auto q = rest.find('?'); q != std::string::npos) {
    cgi = rest.substr(q + 1);
    rest = rest.substr(0, q);
  }
  std::string path = normalizePath(rest);

  // Keep only whitelisted CGI keys, sorted for canonical form.
  std::string kept;
  if (!keepCgi.empty() && !cgi.empty()) {
    std::map<std::string, std::string> keptKv; // sorted by key
    std::istringstream is(cgi);
    std::string kv;
    while (std::getline(is, kv, '&')) {
      auto eq = kv.find('=');
      std::string k = eq == std::string::npos ? kv : kv.substr(0, eq);
      if (std::find(keepCgi.begin(), keepCgi.end(), k) != keepCgi.end())
        keptKv[k] = eq == std::string::npos ? "" : kv.substr(eq + 1);
    }
    for (const auto& [k, v] : keptKv)
      kept += (kept.empty() ? "?" : "&") + k + "=" + v;
  }

  UrlKey out;
  out.scheme = scheme;
  out.host = host;
  out.key = scheme + "://" + host + ":" + std::to_string(port) + path + kept;
  out.hashHex = sha256Hex(out.key);
  return out;
}

std::string UrlKey::objectDir(const std::string& cacheDir) const {
  return cacheDir + "/objects/" + hashHex.substr(0, 2);
}
std::string UrlKey::dataPath(const std::string& cacheDir) const {
  return objectDir(cacheDir) + "/" + hashHex + ".data";
}
std::string UrlKey::metaPath(const std::string& cacheDir) const {
  return objectDir(cacheDir) + "/" + hashHex + ".meta";
}

bool hostMatchesAny(const std::vector<std::string>& globs, const std::string& host) {
  for (const auto& g : globs)
    if (::fnmatch(g.c_str(), host.c_str(), FNM_CASEFOLD) == 0)
      return true;
  return false;
}

std::string stripCgiParams(const std::string& url, const std::vector<std::string>& names) {
  auto q = url.find('?');
  if (q == std::string::npos)
    return url;
  std::string kept;
  std::istringstream in(url.substr(q + 1));
  std::string param;
  while (std::getline(in, param, '&')) {
    if (param.empty())
      continue;
    std::string k = param.substr(0, param.find('='));
    if (std::find(names.begin(), names.end(), k) != names.end())
      continue;
    kept += (kept.empty() ? "" : "&") + param;
  }
  return kept.empty() ? url.substr(0, q) : url.substr(0, q) + "?" + kept;
}

namespace {

std::string cgiKey(const std::string& param) {
  return param.substr(0, param.find('='));
}
std::string cgiValue(const std::string& param) {
  const auto eq = param.find('=');
  return eq == std::string::npos ? std::string() : param.substr(eq + 1);
}

// Rewrites the tried=/triedrc= parameters of `url` and keeps every other
// parameter byte for byte, in order. Every tried= host is merged into one
// tried= at the position of the first, in order of appearance, with empty
// items and case-insensitive duplicates dropped.
//   dropReselection: a triedrc= whose last reason is "resel" removes itself,
//     every other triedrc=, and the last tried= before it.
//   keepReason: the last remaining triedrc= follows the merged tried=, or
//     stays in its own place when no host remains; otherwise every triedrc=
//     is removed.
// A URL with neither key comes back unchanged.
std::string mergeTriedParams(const std::string& url, bool dropReselection, bool keepReason) {
  const auto q = url.find('?');
  if (q == std::string::npos)
    return url;
  std::vector<std::string> params;
  {
    std::istringstream in(url.substr(q + 1));
    std::string param;
    while (std::getline(in, param, '&'))
      if (!param.empty())
        params.push_back(param);
  }
  constexpr size_t kNone = static_cast<size_t>(-1);
  size_t lastReason = kNone;
  bool anyTried = false;
  for (size_t i = 0; i < params.size(); ++i) {
    const std::string k = cgiKey(params[i]);
    if (k == "triedrc")
      lastReason = i;
    else if (k == "tried")
      anyTried = true;
  }
  if (!anyTried && lastReason == kNone)
    return url;

  // A reselection request is recognised by its last reason, the only one a
  // redirector reads.
  bool reselection = false;
  size_t droppedTried = kNone;
  if (dropReselection && lastReason != kNone) {
    const std::string v = cgiValue(params[lastReason]);
    const auto comma = v.rfind(',');
    reselection = (comma == std::string::npos ? v : v.substr(comma + 1)) == "resel";
    if (reselection)
      for (size_t i = 0; i < lastReason; ++i)
        if (cgiKey(params[i]) == "tried")
          droppedTried = i;
  }

  std::vector<std::string> hosts, seen;
  for (size_t i = 0; i < params.size(); ++i) {
    if (i == droppedTried || cgiKey(params[i]) != "tried")
      continue;
    std::istringstream in(cgiValue(params[i]));
    std::string host;
    while (std::getline(in, host, ',')) {
      if (host.empty())
        continue;
      std::string lower = toLower(host);
      if (std::find(seen.begin(), seen.end(), lower) != seen.end())
        continue;
      seen.push_back(std::move(lower));
      hosts.push_back(host);
    }
  }
  const bool keptReason = keepReason && !reselection && lastReason != kNone;

  std::string kept;
  const auto add = [&kept](const std::string& p) { kept += (kept.empty() ? "" : "&") + p; };
  bool merged = false;
  for (size_t i = 0; i < params.size(); ++i) {
    const std::string k = cgiKey(params[i]);
    if (k == "tried") {
      if (merged)
        continue;
      merged = true;
      if (hosts.empty())
        continue;
      std::string list;
      for (const auto& h : hosts)
        list += (list.empty() ? "" : ",") + h;
      add("tried=" + list);
      if (keptReason)
        add(params[lastReason]);
    } else if (k == "triedrc") {
      // Placed after the merged list; kept in place only with no host to follow.
      if (keptReason && i == lastReason && hosts.empty())
        add(params[i]);
    } else {
      add(params[i]);
    }
  }
  return kept.empty() ? url.substr(0, q) : url.substr(0, q) + "?" + kept;
}

} // namespace

std::string ownOpenUrl(const std::string& url) {
  return mergeTriedParams(url, /*dropReselection=*/true, /*keepReason=*/true);
}

} // namespace ucache
