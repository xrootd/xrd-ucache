// The names of a cache entry: with logical_name_prefixes set, one entry holds one file and has
// several names, its logical name (the core name, which groups the others) and each URL key that
// reached it. A URL is an alias of the entry.
//
// An entry's data stays where it was made: under the hash of the key it was made under (a URL's
// key, for an entry cached before the prefixes were set; the logical name's, for one made after).
// Every other name points to it with an alias, so turning logical names on keeps the cache warm
// (the URL's entry becomes the name's, nothing is fetched again or copied), and turning them off
// again keeps every entry reachable by its URLs.
//
// On disk, only once a name was given (a cache that never had logical names has none of it):
//
//   <cacheDir>/names/<hh>/<namehash>.to     an alias: "ucache-alias 1 <entryhash> <entry key>\n";
//                                           made once, by link(2) of a whole file, never
//                                           changed: the first process to name an entry wins,
//                                           and every other reads the winner's
//   <cacheDir>/names/<hh>/<entryhash>.names the entry's names, appended one line each
//                                           ("lfn <key>" or "url <key>"), read as the logical
//                                           name and the newest kEntryNamesMax distinct URLs;
//                                           a torn last line is skipped
//
// <namehash> is the SHA-256 of the name's key (lfn:/... or a URL key), <hh> its first two hex
// digits. An alias outlives its entry's eviction: the next fill through any of its names makes
// the entry again under the same hash. `ucache rm` removes the entry's names file.
//
// Resolution of a key made from a URL with a logical name (K = lfn:<name>, P = the URL's key):
//   1. an entry under K's hash: that one;
//   2. an alias of K: the entry it names;
//   3. an entry under P's hash (cached before the prefixes were set): it becomes K's entry (an
//      alias of K is made to it), so the cache stays warm;
//   4. none: a new entry, under K's hash.
// In each case (the plug-in only) an alias of P is made to the entry when P is not that entry, and
// both names are added to its names file. Two entries of one name cached before (two URLs of one
// file): the first one named becomes the name's entry (nameCachedEntries names the most recently
// used first); the other URL is then an alias of it (that
// URL's own entry is not read while the prefixes are set and ages out; with them off it is that
// URL's entry again while it lasts). Bytes of two entries are never mixed. A key with no logical
// name (the prefixes off, or a path that holds none): its own entry, or, when it has none and the
// cache has names, the entry its alias names while that entry exists (once it is removed, the key
// is its own again: no file is cached under another's name with the prefixes off).
//
// Thread-safety: every function may be called from any thread and any process; a forked child
// starts with an empty memory of resolutions.
#pragma once

#include "UrlKey.h"

#include <string>
#include <vector>

namespace ucache {

inline constexpr size_t kEntryNamesMax = 8; // URL names an entry lists (all stay its aliases)

// Whether this process gives names (makes aliases, adds to names files): the plug-in does, the CLI
// only reads them. Off by default.
void setEntryNamesWritable(bool writable);

// The entry `k` (as KeyRules::key made it) is held in, as a key: its hash and stored key are the
// entry's, everything else `k`'s. `k` itself when `cacheDir` is empty or `k` names its own entry.
// Blocks on a few small local file reads (and, writable, writes). Never throws.
UrlKey resolveEntry(const UrlKey& k, const std::string& cacheDir) noexcept;

struct EntryNameList {
  std::string lfn;               // the logical name, "" for none recorded
  std::vector<std::string> urls; // the newest distinct URL keys, oldest first, at most kEntryNamesMax
};
// The names recorded for the entry `entryHash`; empty when none are.
EntryNameList entryNames(const std::string& cacheDir, const std::string& entryHash);
// Removes the names file of the entry `entryHash` (its aliases stay: they are names, not data).
void dropEntryNames(const std::string& cacheDir, const std::string& entryHash);

// Names the entries already cached: every entry stored under a URL key whose path has a logical
// name under `rules` gets that name (resolveEntry's case 3, as if a job opened it now, whatever
// setEntryNamesWritable says), the most recently used first, so that another URL of the file finds
// it as soon as the prefixes are set. Once per cache and list of prefixes: done when the marker
// <cacheDir>/names/named-<prefixes hash> exists (made at the end); one process at a time (a flock of
// <cacheDir>/names/naming.lock; another that finds it held returns at once). Reads every sidecar
// once. Returns the entries named, -1 when it was done before, held, or `rules` has no names.
long nameCachedEntries(const std::string& cacheDir, const KeyRules& rules) noexcept;

// Paths (for tests and the CLI).
std::string entryNamesDir(const std::string& cacheDir);
std::string aliasPath(const std::string& cacheDir, const std::string& nameHash);
std::string namesPath(const std::string& cacheDir, const std::string& entryHash);

} // namespace ucache
