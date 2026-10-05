// URL keying and normalization.
//
// Key = scheme://host:port/path after normalization: scheme/host lowercased,
// default port made explicit (root/xroot -> 1094), consecutive slashes
// collapsed, "."/".." resolved. ALL CGI/opaque parameters are stripped by
// default — authz=/xrd.*/tokens must never reach disk or keys; UCACHE_KEEP_CGI
// whitelists semantically meaningful keys (kept sorted, canonical).
//
// Thread-safety: immutable value type; parse() is pure.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ucache {

struct UrlKey {
  std::string key;     // full normalized key (stored in meta for ls/debug)
  std::string hashHex; // sha256(key), lowercase hex
  std::string scheme;
  std::string host;    // lowercased, no port (for UCACHE_ALLOW/DENY globs)

  static std::optional<UrlKey> parse(const std::string& url,
                                     const std::vector<std::string>& keepCgi = {});

  // objects/<hh>/<hash>.data|.meta under cacheDir.
  std::string dataPath(const std::string& cacheDir) const;
  std::string metaPath(const std::string& cacheDir) const;
  std::string objectDir(const std::string& cacheDir) const;
};

// fnmatch-style glob matching for UCACHE_ALLOW/DENY host lists.
bool hostMatchesAny(const std::vector<std::string>& globs, const std::string& host);

// Remove the named CGI parameters from a URL, preserving everything else
// (auth tokens, xrd.* options) byte-for-byte.
std::string stripCgiParams(const std::string& url, const std::vector<std::string>& names);

// The URL for an origin open the cache makes on its own behalf (a trusted
// handle's open on its first miss, a degraded handle's open, the open a forked
// child makes afresh), derived from the reader's URL.
//
// A reader names servers to avoid with tried=<host>[,<host>...], optionally
// followed by triedrc=<reason>; the redirector skips those hosts. That list is
// kept, so the cache's own opens avoid the same servers. One form is dropped:
// a reselection request, i.e. a triedrc= whose last reason is "resel" (the
// redirector reads only the last one). A client such as CMSSW's XrdAdaptor
// sends it to ask for a second server BESIDE the ones it already reads from;
// it says nothing about those servers being bad, and an open that inherited
// it was pushed onto servers this host could not reach. For that form every
// triedrc= is removed, with the last tried= before it.
//
// The remaining tried= hosts are merged into ONE tried= parameter, because a
// repeated key reaches the server only once (the client keeps the last copy):
// in order of appearance, empty items and case-insensitive duplicates dropped,
// at the position of the first tried=, followed by the last remaining
// triedrc=. When no host remains, that triedrc= stays where it was, as the
// reader wrote it (unless it is a reselection request). Every other parameter
// (auth tokens, xrd.* options) is kept byte for byte, in order. A URL without
// tried=/triedrc= is returned unchanged.
// Pure.
std::string ownOpenUrl(const std::string& url);

} // namespace ucache
