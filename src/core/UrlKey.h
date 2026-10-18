// URL keying and normalization.
//
// Key = scheme://host:port/path after normalization: scheme/host lowercased,
// default port made explicit (root/xroot -> 1094), consecutive slashes
// collapsed, "."/".." resolved. ALL CGI/opaque parameters are stripped by
// default — authz=/xrd.*/tokens must never reach disk or keys; UCACHE_KEEP_CGI
// whitelists semantically meaningful keys (kept sorted, canonical).
//
// Thread-safety: immutable value type; parse() is pure.
//
// Keys by logical name: with `logical_name_prefixes` set, a URL whose normalized
// path holds one of them is keyed "lfn:<logical name>" instead,
// so one file reached through any site or redirector is one cache entry
// (KeyRules below). An entry's identity is the hash of its key, and is never
// derived again from the key's text (UrlKey::ofEntry).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ucache {

struct UrlKey {
  std::string key;     // full normalized key (stored in meta for ls/debug)
  std::string hashHex; // sha256(key), lowercase hex
  std::string scheme;  // "lfn" for a key by logical name
  std::string host;    // lowercased, no port (for UCACHE_ALLOW/DENY globs): the URL's,
                       // also for a key by logical name; "" for one given as "lfn:/..."
  // For a key by logical name made from a URL: that URL's own key, as it is with
  // logical names off, and its hash (KeyRules::key). Empty otherwise.
  std::string urlKey, urlHash;
  // Set by resolveEntry (EntryNames.h): this URL is a name of an entry another URL filled, and
  // its copy's size has not been compared with that entry's yet, so the caller is to compare them
  // (at the origin) before it trusts that entry for it.
  bool joined = false;
  // Set by resolveEntry when the entry was reached through a name of it (an alias) and is stored
  // under another key's hash: the generation of that entry the name points to (MetaData::gen).
  // Such an entry is only ever taken as it is, in that generation: never started, nor started
  // again, under that hash for this key (FileEntry::open). 0 otherwise. A key of this kind
  // also carries the URL's own key and hash in urlKey/urlHash, with or without a logical name.
  uint32_t entryGen = 0;

  bool logical() const { return scheme == "lfn"; }

  // The key of `url` by its URL. A key by logical name, "lfn:/<path>", is
  // accepted as it is (its path normalized as a URL's is): what `ucache ls`
  // prints can be given back to any command.
  static std::optional<UrlKey> parse(const std::string& url,
                                     const std::vector<std::string>& keepCgi = {});
  // The identity of an entry that exists, from its stored key and its object
  // name: never derived again from the key's text, whatever the settings say.
  static UrlKey ofEntry(const std::string& key, const std::string& hashHex);

  // objects/<hh>/<hash>.data|.meta under cacheDir.
  std::string dataPath(const std::string& cacheDir) const;
  std::string metaPath(const std::string& cacheDir) const;
  std::string objectDir(const std::string& cacheDir) const;
};

// Local files. A URL whose host is `local` -- root://local//abs/path, with any
// of the root schemes and any port -- names the file /abs/path of this
// machine's file system. The plug-in reads that file itself, through the
// client's own local-file access, and no server is ever contacted: `local` is
// no server's address, so the URL cannot mean anything else to a program
// without uCache, where `localhost` would be a real server's. The path is the
// URL's normalized path, the same one its key holds, so the file read is
// always the file the key names, and the key is always the URL's: never a
// logical name (KeyRules). Which of these URLs reach the plug-in is the
// client's plug-in configuration's choice (`url = *`, or `local` for
// root://local on the default port).
inline constexpr const char* kLocalFileHost = "local";
// The absolute, normalized path a local-file URL names; "" for any other URL. Pure.
std::string localFilePath(const std::string& url);
inline bool isLocalFileUrl(const std::string& url) { return !localFilePath(url).empty(); }
// The client's own URL of that file, "file://localhost/abs/path"; "" for any other URL. Pure.
std::string localFileClientUrl(const std::string& url);
// A path given to a request on the local host itself (a stat or a listing), as
// a URL's path is: made absolute and normalized. Pure.
std::string localPathOf(const std::string& path);

// Logical names are off unless the cache sets its prefixes. CMS's official namespaces, as the
// documentation gives them for an example: "/store/data/ /store/mc/ /store/hidata/".
inline constexpr const char* kDefaultLogicalNamePrefixes = "off";
// At most this many prefixes, each at most this long.
inline constexpr size_t kMaxLogicalNamePrefixes = 32;
inline constexpr size_t kMaxLogicalNamePrefixBytes = 256;

// The rules every caller keys a file by: the CGI kept (keep_cgi) and, when
// `logical` and valid prefixes are given, the logical names.
// Built once, in Config. The prefixes are looked for in the normalized path
// only (no scheme, host, port or query): the logical name runs from the first
// (leftmost) place in the path where any of them begins to the end of the
// path. Each prefix begins and ends with "/", so it only ever matches whole
// path components. Kept CGI follows the name as it follows a URL key's path.
//
// Thread-safety: immutable after construction.
class KeyRules {
 public:
  KeyRules() = default;
  // `prefixes`: the space-separated list, "off" or "" for none. A list that is
  // not valid (logicalNamePrefixesValid) leaves logical names off;
  // prefixesError() says why.
  KeyRules(std::vector<std::string> keepCgi, bool logical, const std::string& prefixes);

  // The key a file read through `url` is cached under: by logical name when
  // these rules have one for its path, else by its URL (UrlKey::parse). An
  // "lfn:" key is accepted as it is.
  std::optional<UrlKey> key(const std::string& url) const;
  // The key by URL alone, whatever the rules say of logical names.
  std::optional<UrlKey> urlKey(const std::string& url) const;
  // A key stored in a record, as these rules key it now: a URL key whose path
  // has a logical name becomes "lfn:<name>"; any other key is returned as it is.
  std::string mapStored(const std::string& storedKey) const;
  bool logical() const { return logical_; }
  // These rules with logical names on whatever `logical` said (the readouts of records that were
  // keyed by name); as they are when the prefixes are off or not valid.
  KeyRules withNames() const {
    KeyRules k = *this;
    k.logical_ = !prefixes_.empty();
    k.id_ = nextId();
    return k;
  }
  // The prefixes are valid (or off); false for a list that is not.
  bool prefixesValid() const { return error_.empty(); }
  const std::string& prefixesError() const { return error_; }
  const std::vector<std::string>& keepCgi() const { return keepCgi_; }
  // The prefixes in use (empty: none).
  const std::vector<std::string>& prefixes() const { return prefixes_; }
  // The first 8 hex digits of the SHA-256 of the normalized list (normalizedPrefixes; "" when
  // off): what run_settings record as logical_names=on/<hash>.
  const std::string& prefixesHash() const { return prefixesHash_; }

 private:
  std::vector<std::string> keepCgi_;
  std::vector<std::string> prefixes_; // empty: none
  bool logical_ = false;               // keys by logical name
  std::string error_;
  std::string prefixesHash_;
  // Tells rules apart for the cache of the last key each thread derived (key()): copies of one set
  // of rules share it, withNames() makes a new one.
  uint64_t id_ = nextId();
  static uint64_t nextId();
  std::optional<UrlKey> derive(const std::string& url) const;
};

// The logical name of a normalized path: from the leftmost place where any of `prefixes` begins to
// the end of the path; "" when none is in it. Pure.
std::string logicalNameOf(const std::vector<std::string>& prefixes, const std::string& path);
// Whether `value` is a valid list of prefixes: "off" (or ""), or 1 to kMaxLogicalNamePrefixes
// entries separated by spaces, each beginning and ending with "/", with at least one byte between,
// printable (no space or control byte), with no empty, "." or ".." component (a normalized path
// never holds one), and at most kMaxLogicalNamePrefixBytes long. `why` gets the reason when it is
// not. Pure.
bool logicalNamePrefixesValid(const std::string& value, std::string* why = nullptr);
// The list as the rules hold it: its entries separated by one space ("off" for none). Pure.
std::string normalizedPrefixes(const std::string& value);


// fnmatch-style glob matching for UCACHE_ALLOW/DENY host lists.
bool hostMatchesAny(const std::vector<std::string>& globs, const std::string& host);
// Whether `host` passes the cache's allow and deny lists: on the allow list (or the list is
// empty), and not on the deny list. Matched against the host of a job's URL, it says whether
// uCache caches the file.
bool hostPassesLists(const std::vector<std::string>& allow, const std::vector<std::string>& deny,
                     const std::string& host);

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
// The same, but of the hosts a reselection request names, those in
// `failedHosts` are kept (host names, compared without a port and ignoring
// case), followed by triedrc=ioerr: a reader that asks for another server
// beside the ones it uses names both the servers it reads from without
// trouble and the ones it gave up on, and the URL does not say which is
// which. The ones this process saw fail for the file are the reader's own
// reason for the request, so the cache's own open avoids them too; the others
// are not passed on (with every holder of the file named, a redirector finds
// no server at all). Used with failover on.
std::string ownOpenUrl(const std::string& url, const std::vector<std::string>& failedHosts);

// The reader's URL as the base of a retry of its own open at another site:
// every tried= host merged into one tried= list as ownOpenUrl does, a
// reselection request's hosts included (they are servers the reader is
// already reading from), and every triedrc= removed. Pure.
std::string mergeTried(const std::string& url);

// `base` (a URL with at most one tried=, as mergeTried and ownOpenUrl make it)
// with the host names `ours` added to its tried= list, after the hosts already
// there, which are never dropped, and followed by triedrc=`triedrc` in place
// of any triedrc= (a redirector reads only one): `enoent`, or `ioerr` for
// any other value (never a reselection request). Of ours, only full host
// names are added: an address or a name without a dot keeps the redirector
// from nothing. Duplicates (ignoring case) are dropped, and of ours at most
// the 4 newest are kept, fewer when the list would pass 1024 bytes. Without
// any host to list, `base` is returned unchanged. Pure.
std::string withExclusions(const std::string& base, const std::vector<std::string>& ours,
                           const std::string& triedrc);

// The hosts a URL's tried= lists (all of them, in order, duplicates dropped, as
// written). Pure.
std::vector<std::string> triedHosts(const std::string& url);

// ---- Hosts to avoid, by pattern, in tried= ----------------------------------
// A tried= entry that holds a '*' is a host pattern, not a host name: a glob on the full host
// name, '*' standing for any run of characters (dots included), compared ignoring case. A
// redirector compares tried= entries with whole host names only, so a pattern is applied by
// uCache and never sent: where uCache's federation behaviour is on for the file, an open that
// lands on a data server whose name matches is refused before anything is read there, and the
// open is asked again with that server's own name in tried= (OpenPolicy.h avoidedHop).
//
// What a URL's tried= patterns are: `patterns`, in order of appearance, case-insensitive
// duplicates dropped; `malformed`, the entries that hold a '*' and are not a host pattern
// (hostPatternValid), which are ignored; and `url`, the URL with every such entry removed from
// its tried= lists (a tried= left without an entry is removed; every other parameter is kept
// byte for byte, in order). A URL without a pattern entry gives no pattern and itself. Pure.
struct TriedPatterns {
  std::vector<std::string> patterns;
  std::vector<std::string> malformed;
  std::string url;
  bool any() const { return !patterns.empty() || !malformed.empty(); }
};
TriedPatterns splitTriedPatterns(const std::string& url);
// At most this many patterns are taken from one URL (the rest are malformed), each at most
// kMaxHostPatternBytes long.
inline constexpr size_t kMaxHostPatterns = 32;
inline constexpr size_t kMaxHostPatternBytes = 253;
// Whether `p` is a host pattern: 1 to kMaxHostPatternBytes bytes of letters, digits, '.', '-',
// '_', ':' (an IPv6 address) and '*', optionally in square brackets, holding at least one '*',
// and not a host and a port ("*.x.org:1094": patterns are on the host only). `why` gets the reason
// when it is not. Pure.
bool hostPatternValid(const std::string& p, std::string* why = nullptr);
// Whether `host` (as the client names it: a name, an IPv4 address, an IPv6 address with or without
// brackets) matches the pattern `p`: '*' stands for any run of characters, everything else for
// itself, ignoring case; the brackets of an IPv6 address and a final '.' of a name are not
// compared. Pure.
bool hostMatchesPattern(const std::string& p, const std::string& host);
// The first of `patterns` that `host` matches, "" for none. Pure.
std::string matchingPattern(const std::vector<std::string>& patterns, const std::string& host);
// `url` with the host names `hosts` added to its tried= list after the hosts already there,
// which are never dropped: the servers uCache refused for a pattern, which every later attempt
// keeps naming. Duplicates (ignoring case) are dropped; of `hosts`, at most kMaxAvoidedNames are
// kept, the newest, fewer when the list would pass 1024 bytes. A triedrc= stays as it is. Without
// a host to add, `url` is returned unchanged. Pure.
inline constexpr size_t kMaxAvoidedNames = 16;
std::string withTriedHosts(const std::string& url, const std::vector<std::string>& hosts);

// `url` opened at the server `hostPort` instead of its own: the URL's scheme,
// user and path are kept, the host and port replaced. Every tried= and triedrc=
// is dropped unless `keepTried`; other parameters (tokens, xrd.* options) are
// kept byte for byte. Pure.
std::string urlAtServer(const std::string& url, const std::string& hostPort, bool keepTried);

} // namespace ucache
