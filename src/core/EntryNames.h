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
//   <cacheDir>/names/<hh>/<namehash>.to     an alias: "ucache-alias 2 <entryhash> <gen> <c|u>
//                                           <entry key>\n"; made once, by link(2) of a whole
//                                           file: the first process to name an entry wins, and
//                                           every other reads the winner's
//   <cacheDir>/names/<hh>/<entryhash>.names the entry's names, appended one line each
//                                           ("lfn <key>" or "url <key>"), read as the logical
//                                           name and the newest kEntryNamesMax distinct URLs;
//                                           a torn last line is skipped
//   <cacheDir>/names/generation             the last generation id given (nextGen)
//
// <namehash> is the SHA-256 of the name's key (lfn:/... or a URL key), <hh> its first two hex
// digits. An alias names one generation of an entry: <gen> (6 hex digits) is the id the entry's
// sidecar carries (MetaData::gen), given when the first alias to it is made; an entry made, or
// started again, has none, and ids are not given twice. An alias is followed only while the entry
// it names exists in that generation (its data file there too), so an alias left behind (by a
// names list that lost its line, a crash, a removal racing a naming) is never followed again,
// whatever is cached under that hash later: it is as none, replaced when the name is given again,
// and swept. A self alias (the URL's
// own entry: its copy is not the size of the name's entry) holds while that entry exists. <c|u>:
// whether the URL's copy's size was compared with the entry's ("c") or not yet ("u": the caller
// compares it at the origin before it trusts the entry, see UrlKey::joined). When an entry goes,
// the aliases its names file lists go with it (dropEntryNames).
//
// Resolution of a key made from a URL with a logical name (K = lfn:<name>, P = the URL's key):
//   0. a self alias of P whose entry exists: P's own entry;
//   1. an entry under K's hash: that one;
//   2. an alias of K: the entry it names;
//   3. an entry under P's hash (cached before the prefixes were set): it becomes K's entry (an
//      alias of K is made to it), so the cache stays warm;
//   4. none: a new entry, under K's hash.
// When the entry exists and is not P's (cases 1-2), P's alias to it is made (the plug-in only):
// compared at once when P has an entry of its own (another size: P keeps it, a self alias), else
// "u". An entry reached through an alias under another key's hash is only ever taken as it is, in
// that generation (UrlKey::entryGen): never made or started again under that hash for this key.
// Two entries of one name cached before (two URLs of one
// file): the first one named becomes the name's entry (nameCachedEntries names the most recently
// used first); the other URL is then an alias of it (that
// URL's own entry is not read while the prefixes are set and ages out; with them off it is that
// URL's entry again while it lasts). Bytes of two entries are never mixed. A key with no logical
// name (the prefixes off, or a path that holds none): its own entry, or, when it has none and the
// cache has names, the entry its alias names while that entry exists in that generation (once it
// is removed, the key is its own again: no file is cached under another's name with the prefixes
// off).
//
// Thread-safety: every function may be called from any thread and any process; a forked child
// starts with an empty memory of resolutions.
#pragma once

#include "UrlKey.h"

#include <functional>
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
// The names of `entryHash` (as entryNames) that lead to it now: the key it is stored under, and
// each name whose alias names it in the generation it holds. A name whose alias is gone, names
// another entry, or names a generation the entry no longer holds (started afresh, or its sidecar
// rewritten by a release that knows no names) is left out. For the readouts.
EntryNameList liveEntryNames(const std::string& cacheDir, const std::string& entryHash);
// The entry `entryHash` is gone (removed, evicted, dropped): the aliases its list names that point
// to it, and the list. An alias left (a list past its bound, a removal that ended between) names a
// generation that is gone, and is as none: never followed (also once the hash holds an entry
// again), replaced when the name is given again, swept (sweepEntryNames).
void dropEntryNames(const std::string& cacheDir, const std::string& entryHash);
// The URL of `k` (resolved: its entry is another URL's, reached by the logical name) met that
// entry with a copy of another size: from now on the URL is its own entry (a self alias, which
// resolveEntry takes before the logical name while that entry exists). Removes the URL from the
// process's memory of resolutions.
void keepOwnEntry(const UrlKey& k, const std::string& cacheDir);
// The URL of `k` (resolved: its entry is another key's) was opened at its origin, and its copy is
// the size of that entry, which it has just opened: the URL's alias to the entry is made (the
// plug-in only), or marked compared. A URL kept apart meanwhile stays so. Never throws.
void confirmEntryName(const UrlKey& k, const std::string& cacheDir) noexcept;
// `ucache clear`: every name of the cache goes with its entries.
void clearEntryNames(const std::string& cacheDir);
// Removes aliases that name nothing now (dangling, or of an earlier generation), lists of entries
// that are gone, and temporary files older than an hour, looking at most `budget` files of
// <cacheDir>/names, from where the last call of this process stopped (in shard and file name
// order, so that every file is reached in turn). Returns the files removed.
size_t sweepEntryNames(const std::string& cacheDir, size_t budget) noexcept;

// Names the entries already cached: every entry stored under a URL key whose path has a logical
// name under `rules` gets that name (resolveEntry's case 3, as if a job opened it now, whatever
// setEntryNamesWritable says), the most recently used first, so that another URL of the file finds
// it as soon as the prefixes are set. Once per cache and list of prefixes: done when the marker
// <cacheDir>/names/named-<prefixes hash> exists (made at the end); one process at a time (a flock of
// <cacheDir>/names/naming.lock; another that finds it held returns at once). Reads every sidecar
// once. Returns the entries named, -1 when it was done before, held, or `rules` has no names.
// `stop`, when given, is asked between entries: true ends the naming where it is, without the
// marker (the next process with the prefixes names the rest).
long nameCachedEntries(const std::string& cacheDir, const KeyRules& rules,
                       const std::function<bool()>& stop = nullptr) noexcept;
// The cache's prefixes are changed or turned off (`ucache set`, `ucache unset`): every marker of a
// naming done is removed, so that the entries cached by URL from now on are named when a list of
// prefixes is set again, the same list too. Names given stay. Returns the markers removed.
size_t forgetNamings(const std::string& cacheDir) noexcept;

// Paths (for tests and the CLI).
std::string entryNamesDir(const std::string& cacheDir);
std::string aliasPath(const std::string& cacheDir, const std::string& nameHash);
std::string namesPath(const std::string& cacheDir, const std::string& entryHash);

} // namespace ucache
