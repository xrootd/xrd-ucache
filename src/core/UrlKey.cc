#include "UrlKey.h"

#include "vendor/sha256.h"

#include <algorithm>
#include <cctype>
#include <cstring>
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

// Keep only whitelisted CGI keys, sorted for canonical form: "" or "?k=v&...".
std::string keptCgi(const std::string& cgi, const std::vector<std::string>& keepCgi) {
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
  return kept;
}

// The parts of a URL a key is made of.
struct UrlParts {
  std::string scheme, host, path, kept;
  int port = 0;
};
std::optional<UrlParts> urlParts(const std::string& url, const std::vector<std::string>& keepCgi);

constexpr const char* kLfnPrefix = "lfn:";

// "lfn:/<path>[?query]": the key as it is, its path normalized. nullopt for
// anything else.
std::optional<UrlKey> parseLogical(const std::string& s, const std::vector<std::string>& keepCgi) {
  if (s.compare(0, 4, kLfnPrefix) != 0 || s.size() < 5 || s[4] != '/')
    return std::nullopt;
  std::string rest = s.substr(4), cgi;
  if (auto q = rest.find('?'); q != std::string::npos) {
    cgi = rest.substr(q + 1);
    rest = rest.substr(0, q);
  }
  UrlKey out;
  out.scheme = "lfn";
  out.key = std::string(kLfnPrefix) + normalizePath(rest) + keptCgi(cgi, keepCgi);
  out.hashHex = sha256Hex(out.key);
  return out;
}

} // namespace

std::optional<UrlKey> UrlKey::parse(const std::string& url,
                                    const std::vector<std::string>& keepCgi) {
  if (auto lfn = parseLogical(url, keepCgi))
    return lfn;
  const auto p = urlParts(url, keepCgi);
  if (!p)
    return std::nullopt;
  UrlKey out;
  out.scheme = p->scheme;
  out.host = p->host;
  out.key = p->scheme + "://" + p->host + ":" + std::to_string(p->port) + p->path + p->kept;
  out.hashHex = sha256Hex(out.key);
  return out;
}

UrlKey UrlKey::ofEntry(const std::string& key, const std::string& hashHex) {
  UrlKey out;
  if (auto k = parse(key)) // scheme and host, as far as the text gives them
    out = *k;
  out.key = key;
  out.hashHex = hashHex; // the identity: never the hash of the text again
  out.urlKey.clear();
  out.urlHash.clear();
  if (key.compare(0, 4, kLfnPrefix) == 0)
    out.scheme = "lfn";
  return out;
}

namespace {
std::optional<UrlParts> urlParts(const std::string& url, const std::vector<std::string>& keepCgi) {
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
  UrlParts out;
  out.scheme = scheme;
  out.host = host;
  out.port = port;
  out.path = normalizePath(rest);
  out.kept = keptCgi(cgi, keepCgi);
  return out;
}
} // namespace

// ---- Keys by logical name ---------------------------------------------------

namespace {
bool prefixesOff(const std::string& value) {
  return value.find_first_not_of(" \t") == std::string::npos || value == "off";
}
std::vector<std::string> splitPrefixes(const std::string& value) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < value.size()) {
    while (i < value.size() && (value[i] == ' ' || value[i] == '\t'))
      ++i;
    size_t j = i;
    while (j < value.size() && value[j] != ' ' && value[j] != '\t')
      ++j;
    if (j > i)
      out.push_back(value.substr(i, j - i));
    i = j;
  }
  return out;
}
// Why `p` cannot be a prefix, "" when it can.
std::string prefixFault(const std::string& p) {
  if (p.size() > kMaxLogicalNamePrefixBytes)
    return "'" + p.substr(0, 40) + "...' is longer than " +
           std::to_string(kMaxLogicalNamePrefixBytes) + " bytes";
  if (p.size() < 3 || p.front() != '/' || p.back() != '/')
    return "'" + p + "' does not begin and end with \"/\" with a name between";
  for (unsigned char c : p)
    if (c < 0x21 || c > 0x7e)
      return "'" + p + "' holds a byte that is not printable";
  if (p.find("//") != std::string::npos || p.find("/./") != std::string::npos ||
      p.find("/../") != std::string::npos)
    return "'" + p + "' holds an empty, \".\" or \"..\" component, which a path never does";
  return std::string();
}
} // namespace

std::string logicalNameOf(const std::vector<std::string>& prefixes, const std::string& path) {
  size_t from = std::string::npos;
  for (const std::string& p : prefixes) {
    const size_t at = path.find(p);
    if (at < from)
      from = at;
  }
  return from == std::string::npos ? std::string() : path.substr(from);
}

bool logicalNamePrefixesValid(const std::string& value, std::string* why) {
  if (prefixesOff(value))
    return true;
  const std::vector<std::string> list = splitPrefixes(value);
  std::string fault;
  if (list.size() > kMaxLogicalNamePrefixes)
    fault = std::to_string(list.size()) + " prefixes, more than " +
            std::to_string(kMaxLogicalNamePrefixes);
  for (const std::string& p : list)
    if (fault.empty())
      fault = prefixFault(p);
  if (!fault.empty() && why)
    *why = fault;
  return fault.empty();
}

std::string normalizedPrefixes(const std::string& value) {
  if (prefixesOff(value))
    return "off";
  std::string out;
  for (const std::string& p : splitPrefixes(value))
    out += (out.empty() ? "" : " ") + p;
  return out;
}

KeyRules::KeyRules(std::vector<std::string> keepCgi, bool logical, const std::string& prefixes)
    : keepCgi_(std::move(keepCgi)) {
  if (prefixesOff(prefixes))
    return;
  std::string why;
  if (!logicalNamePrefixesValid(prefixes, &why)) {
    error_ = why;
    return;
  }
  prefixes_ = splitPrefixes(prefixes);
  prefixesHash_ = sha256Hex(normalizedPrefixes(prefixes)).substr(0, 8);
  logical_ = logical;
}

std::optional<UrlKey> KeyRules::urlKey(const std::string& url) const {
  return UrlKey::parse(url, keepCgi_);
}

uint64_t KeyRules::nextId() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

namespace {
// KeyRules::key's last answer, per thread. Freed when the thread ends, by an owner whose destructor
// also marks it gone: a call made after that on the same thread (the main thread's at process
// exit, while the final records are written) derives the key without keeping it, and never meets
// a destroyed object. The pointer and the mark are trivially destructible, so they can be read
// until the thread itself is gone.
struct LastKey {
  uint64_t id = 0;
  std::string url;
  std::optional<UrlKey> key;
};
thread_local LastKey* tLastKey = nullptr;
thread_local bool tLastKeyGone = false;
struct LastKeyOwner {
  ~LastKeyOwner() {
    delete tLastKey;
    tLastKey = nullptr;
    tLastKeyGone = true;
  }
};
} // namespace

std::optional<UrlKey> KeyRules::key(const std::string& url) const {
  // The same URL is keyed again and again by one thread (a handle's records and reads): the last
  // answer is kept per thread, for these rules (a regular expression is not free).
  if (tLastKeyGone)
    return derive(url); // the thread is ending: nothing kept
  if (!tLastKey) {
    static thread_local LastKeyOwner owner; // frees it when the thread ends
    (void)owner;
    tLastKey = new LastKey;
  }
  LastKey* last = tLastKey;
  if (last->id == id_ && last->url == url)
    return last->key;
  std::optional<UrlKey> k = derive(url);
  last->id = id_;
  last->url = url;
  last->key = k;
  return k;
}

std::optional<UrlKey> KeyRules::derive(const std::string& url) const {
  if (auto lfn = parseLogical(url, keepCgi_))
    return lfn;
  const auto p = urlParts(url, keepCgi_);
  if (!p)
    return std::nullopt;
  UrlKey out;
  out.scheme = p->scheme;
  out.host = p->host;
  out.key = p->scheme + "://" + p->host + ":" + std::to_string(p->port) + p->path + p->kept;
  out.hashHex = sha256Hex(out.key);
  if (!logical_)
    return out;
  const std::string name = logicalNameOf(prefixes_, p->path);
  if (name.empty())
    return out;
  out.urlKey = std::move(out.key);
  out.urlHash = std::move(out.hashHex);
  out.scheme = "lfn";
  out.key = std::string(kLfnPrefix) + name + p->kept;
  out.hashHex = sha256Hex(out.key);
  return out;
}

std::string KeyRules::mapStored(const std::string& storedKey) const {
  if (!logical_ || storedKey.compare(0, 4, kLfnPrefix) == 0)
    return storedKey;
  // A stored key is a URL key already (normalized, its kept CGI sorted): its
  // own kept query goes on as it is.
  const auto p = urlParts(storedKey, {});
  if (!p)
    return storedKey;
  const std::string name = logicalNameOf(prefixes_, p->path);
  if (name.empty())
    return storedKey;
  const auto q = storedKey.find('?');
  return std::string(kLfnPrefix) + name + (q == std::string::npos ? "" : storedKey.substr(q));
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

bool hostPassesLists(const std::vector<std::string>& allow, const std::vector<std::string>& deny,
                     const std::string& host) {
  return (allow.empty() || hostMatchesAny(allow, host)) && !hostMatchesAny(deny, host);
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
//   failed: with dropReselection, the hosts of the dropped tried= that are in
//     this list are kept after all (compared without a port, ignoring case),
//     and when any is, triedrc=ioerr follows the merged tried=.
//   hostsOut: gets the merged list's hosts.
// A URL with neither key comes back unchanged.
std::string mergeTriedParams(const std::string& url, bool dropReselection, bool keepReason,
                             const std::vector<std::string>* failed = nullptr,
                             std::vector<std::string>* hostsOut = nullptr) {
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

  const auto isFailed = [failed](const std::string& host) {
    const std::string h = toLower(host.substr(0, host.find(':')));
    for (const auto& f : *failed)
      if (toLower(f.substr(0, f.find(':'))) == h)
        return true;
    return false;
  };
  bool keptFailed = false;
  std::vector<std::string> hosts, seen;
  for (size_t i = 0; i < params.size(); ++i) {
    const bool resel = i == droppedTried;
    if ((resel && !failed) || cgiKey(params[i]) != "tried")
      continue;
    std::istringstream in(cgiValue(params[i]));
    std::string host;
    while (std::getline(in, host, ',')) {
      if (host.empty() || (resel && !isFailed(host)))
        continue;
      keptFailed = keptFailed || resel;
      std::string lower = toLower(host);
      if (std::find(seen.begin(), seen.end(), lower) != seen.end())
        continue;
      seen.push_back(std::move(lower));
      hosts.push_back(host);
    }
  }
  if (hostsOut)
    *hostsOut = hosts;
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
      else if (keptFailed)
        add("triedrc=ioerr");
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

std::string ownOpenUrl(const std::string& url, const std::vector<std::string>& failedHosts) {
  return mergeTriedParams(url, /*dropReselection=*/true, /*keepReason=*/true, &failedHosts);
}

std::string mergeTried(const std::string& url) {
  return mergeTriedParams(url, /*dropReselection=*/false, /*keepReason=*/false);
}

std::vector<std::string> triedHosts(const std::string& url) {
  std::vector<std::string> hosts;
  (void)mergeTriedParams(url, /*dropReselection=*/false, /*keepReason=*/false, nullptr, &hosts);
  return hosts;
}

namespace {
// `s` lower-cased, without the square brackets around it and without a final '.'.
std::string hostForMatch(std::string s) {
  s = toLower(std::move(s));
  if (s.size() >= 2 && s.front() == '[' && s.back() == ']')
    s = s.substr(1, s.size() - 2);
  while (!s.empty() && s.back() == '.')
    s.pop_back();
  return s;
}
// Glob matching where only '*' is special (any run, empty included). Both lower-cased already.
bool globMatch(const std::string& p, const std::string& t) {
  size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
  while (ti < t.size()) {
    if (pi < p.size() && p[pi] == '*') {
      star = pi++;
      mark = ti;
    } else if (pi < p.size() && p[pi] == t[ti]) {
      ++pi;
      ++ti;
    } else if (star != std::string::npos) {
      pi = star + 1;
      ti = ++mark;
    } else {
      return false;
    }
  }
  while (pi < p.size() && p[pi] == '*')
    ++pi;
  return pi == p.size();
}
} // namespace

bool hostPatternValid(const std::string& p, std::string* why) {
  const auto no = [why](const char* w) {
    if (why)
      *why = w;
    return false;
  };
  if (p.empty())
    return no("it is empty");
  if (p.size() > kMaxHostPatternBytes)
    return no("it is longer than a host name can be");
  std::string body = p;
  if (body.size() >= 2 && body.front() == '[' && body.back() == ']')
    body = body.substr(1, body.size() - 2);
  if (body.find('*') == std::string::npos)
    return no("it holds no '*'");
  for (const char c : body) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!(std::isalnum(u) || c == '.' || c == '-' || c == '_' || c == ':' || c == '*'))
      return no("it holds a character no host name has");
  }
  const size_t colon = body.find(':');
  if (colon != std::string::npos && body.find(':', colon + 1) == std::string::npos &&
      colon + 1 < body.size() &&
      body.find_first_not_of("0123456789", colon + 1) == std::string::npos)
    return no("it names a port; a pattern is on the host name only");
  return true;
}

bool hostMatchesPattern(const std::string& p, const std::string& host) {
  const std::string h = hostForMatch(host);
  return !h.empty() && globMatch(hostForMatch(p), h);
}

std::string matchingPattern(const std::vector<std::string>& patterns, const std::string& host) {
  for (const auto& p : patterns)
    if (hostMatchesPattern(p, host))
      return p;
  return std::string();
}

TriedPatterns splitTriedPatterns(const std::string& url) {
  TriedPatterns out;
  out.url = url;
  const auto q = url.find('?');
  if (q == std::string::npos || url.find('*', q) == std::string::npos)
    return out;
  std::vector<std::string> params;
  {
    std::istringstream in(url.substr(q + 1));
    std::string param;
    while (std::getline(in, param, '&'))
      params.push_back(param);
  }
  std::vector<std::string> seen;
  bool changed = false;
  std::string kept;
  const auto add = [&kept](const std::string& p) { kept += (kept.empty() ? "" : "&") + p; };
  for (const auto& param : params) {
    if (param.empty())
      continue;
    if (cgiKey(param) != "tried" || cgiValue(param).find('*') == std::string::npos) {
      add(param);
      continue;
    }
    changed = true;
    std::istringstream in(cgiValue(param));
    std::string item, list;
    while (std::getline(in, item, ',')) {
      if (item.find('*') == std::string::npos) {
        if (!item.empty())
          list += (list.empty() ? "" : ",") + item;
        continue;
      }
      const std::string lower = toLower(item);
      if (std::find(seen.begin(), seen.end(), lower) != seen.end())
        continue;
      seen.push_back(lower);
      if (hostPatternValid(item) && out.patterns.size() < kMaxHostPatterns)
        out.patterns.push_back(item);
      else
        out.malformed.push_back(item);
    }
    if (!list.empty())
      add("tried=" + list);
  }
  if (changed)
    out.url = kept.empty() ? url.substr(0, q) : url.substr(0, q) + "?" + kept;
  return out;
}

std::string withTriedHosts(const std::string& url, const std::vector<std::string>& hosts) {
  std::vector<std::string> theirs;
  (void)mergeTriedParams(url, /*dropReselection=*/false, /*keepReason=*/false, nullptr, &theirs);
  std::vector<std::string> seen, mine;
  for (const auto& h : theirs)
    seen.push_back(toLower(h));
  for (const auto& h : hosts) {
    std::string name = toLower(h);
    while (!name.empty() && name.back() == '.')
      name.pop_back();
    if (name.empty() || std::find(seen.begin(), seen.end(), name) != seen.end())
      continue;
    seen.push_back(name);
    mine.push_back(name);
  }
  if (mine.size() > kMaxAvoidedNames) // the oldest go first
    mine.erase(mine.begin(), mine.end() - kMaxAvoidedNames);
  constexpr size_t kMaxBytes = 1024;
  size_t bytes = 0; // the list's length, with a comma after each host
  for (const auto& h : theirs)
    bytes += h.size() + 1;
  for (const auto& h : mine)
    bytes += h.size() + 1;
  while (!mine.empty() && bytes - 1 > kMaxBytes) {
    bytes -= mine.front().size() + 1;
    mine.erase(mine.begin());
  }
  if (mine.empty())
    return url;
  std::string list;
  for (const auto& h : mine)
    list += (list.empty() ? "" : ",") + h;
  return mergeTriedParams(url + (url.find('?') == std::string::npos ? "?" : "&") + "tried=" + list,
                          /*dropReselection=*/false, /*keepReason=*/true);
}

std::string urlAtServer(const std::string& url, const std::string& hostPort, bool keepTried) {
  const std::string u = keepTried ? url : stripCgiParams(url, {"tried", "triedrc"});
  const auto scheme = u.find("://");
  if (scheme == std::string::npos)
    return u;
  size_t a = scheme + 3;
  const size_t e = std::min(u.find_first_of("/?", a), u.size());
  const size_t at = u.find('@', a);
  if (at < e)
    a = at + 1; // keep the user
  return u.substr(0, a) + hostPort + u.substr(e);
}

namespace {
// An address (IPv4, IPv6, bracketed or not) rather than a host name.
bool isAddress(const std::string& h) {
  if (h.find(':') != std::string::npos || (!h.empty() && h[0] == '['))
    return true;
  return !h.empty() && h.find_first_not_of("0123456789.") == std::string::npos;
}
} // namespace

std::string withExclusions(const std::string& base, const std::vector<std::string>& ours,
                           const std::string& triedrc) {
  std::vector<std::string> theirs; // the reader's, never dropped
  (void)mergeTriedParams(base, /*dropReselection=*/false, /*keepReason=*/false, nullptr, &theirs);
  std::vector<std::string> seen, mine;
  for (const auto& h : theirs)
    seen.push_back(toLower(h));
  for (const auto& o : ours) {
    std::string name = toLower(o);
    while (!name.empty() && name.back() == '.')
      name.pop_back();
    if (name.find('.') == std::string::npos || isAddress(name) ||
        std::find(seen.begin(), seen.end(), name) != seen.end())
      continue; // masks nothing, or listed already
    seen.push_back(name);
    mine.push_back(name);
  }
  constexpr size_t kMaxOurs = 4, kMaxBytes = 1024;
  if (mine.size() > kMaxOurs) // the oldest go first
    mine.erase(mine.begin(), mine.end() - kMaxOurs);
  size_t bytes = 0; // the list's length, with a comma after each host
  for (const auto& h : theirs)
    bytes += h.size() + 1;
  for (const auto& h : mine)
    bytes += h.size() + 1;
  while (!mine.empty() && bytes - 1 > kMaxBytes) {
    bytes -= mine.front().size() + 1;
    mine.erase(mine.begin());
  }
  if (theirs.empty() && mine.empty())
    return base;
  // Appended last: ours merge after the reader's hosts, and ours is the reason kept.
  // Only the two reasons of a failure; a reselection request is never made.
  std::string tail = std::string("triedrc=") + (triedrc == "enoent" ? "enoent" : "ioerr");
  if (!mine.empty()) {
    std::string list;
    for (const auto& h : mine)
      list += (list.empty() ? "" : ",") + h;
    tail = "tried=" + list + "&" + tail;
  }
  return mergeTriedParams(base + (base.find('?') == std::string::npos ? "?" : "&") + tail,
                          /*dropReselection=*/false, /*keepReason=*/true);
}

} // namespace ucache
