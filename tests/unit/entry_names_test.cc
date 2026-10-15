// The names of a cache entry (EntryNames.h): a logical name and the URLs that reached one entry,
// so that turning logical names on keeps the cache warm and turning them off again loses nothing.
#include "CacheStore.h"
#include "Config.h"
#include "EntryNames.h"
#include "MetaFile.h"
#include "UrlKey.h"

#include "TestUtil.h"
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <atomic>
#include <random>
#include <thread>
#include <ctime>

using namespace ucache;
using test::TempDir;

namespace {
constexpr const char* kCms = "/store/data/ /store/mc/ /store/hidata/";
const KeyRules kOn({}, true, kCms);
const KeyRules kOff({}, true, "off");

bool exists(const std::string& p) {
  struct ::stat st;
  return ::stat(p.c_str(), &st) == 0;
}
// An entry under `k`'s own hash, as the store makes one (its sidecar), with one page cached.
void makeEntry(const std::string& dir, const UrlKey& k, bool pin = false, uint64_t size = 50000) {
  RealIO io;
  Config cfg;
  cfg.cacheDir = dir;
  CacheStore store(io, cfg);
  store.disableStatsDump();
  auto e = store.open(k, size);
  ASSERT_TRUE(e);
  ASSERT_TRUE(exists(k.metaPath(dir)));
  if (pin) {
    ASSERT_TRUE(store.setPinnedByKey(k, true));
  }
}
struct Writable {
  Writable() { setEntryNamesWritable(true); }
  ~Writable() { setEntryNamesWritable(false); }
};
// As the plug-in fills an entry it resolved: the entry made (or opened) with the URL's copy, then
// the URL's name of it confirmed (its copy is that entry's size).
void fill(const std::string& dir, const UrlKey& r, uint64_t size = 50000) {
  makeEntry(dir, r, false, size);
  confirmEntryName(r, dir);
}
} // namespace

// The upgrade: a URL's entry cached with the prefixes off becomes the name's entry when they are
// set (no new entry, its pin kept); another URL of the file joins it; with the prefixes off again,
// both URLs still reach it.
TEST(EntryNames, TurningNamesOnKeepsTheCacheWarm) {
  TempDir td;
  const std::string dir = td.path();
  const std::string u1 = "root://site-a.example//store/mc/a.root";
  const std::string u2 = "root://site-b.example//eos/cms/store/mc/a.root";
  const UrlKey p1 = *kOff.key(u1);
  makeEntry(dir, p1, /*pin=*/true);
  EXPECT_FALSE(exists(entryNamesDir(dir))); // the prefixes off: nothing of names on disk
  Writable w;
  const UrlKey n1 = *kOn.key(u1);
  ASSERT_NE(n1.hashHex, p1.hashHex);
  const UrlKey e1 = resolveEntry(n1, dir);
  EXPECT_EQ(e1.hashHex, p1.hashHex); // the URL's entry is the name's: nothing to fetch again
  EXPECT_EQ(e1.key, p1.key);         // and keeps its stored key
  EXPECT_TRUE(e1.logical());
  EXPECT_FALSE(exists(n1.metaPath(dir))); // no second entry
  const UrlKey e2 = resolveEntry(*kOn.key(u2), dir);
  EXPECT_EQ(e2.hashHex, p1.hashHex); // another URL of the file joins it
  {
    RealIO io;
    Config cfg;
    cfg.cacheDir = dir;
    CacheStore store(io, cfg);
    store.disableStatsDump();
    auto e = store.open(e2, 50000);
    ASSERT_TRUE(e);
    EXPECT_TRUE(e->pinned()); // the entry's pin, whichever name opened it
  }
  const EntryNameList names = entryNames(dir, p1.hashHex);
  EXPECT_EQ(names.lfn, "lfn:/store/mc/a.root");
  ASSERT_EQ(names.urls.size(), 2u);
  EXPECT_EQ(names.urls[0], p1.key);
  EXPECT_EQ(names.urls[1], kOff.key(u2)->key);
  // The prefixes off again: both URLs reach the entry.
  EXPECT_EQ(resolveEntry(*kOff.key(u1), dir).hashHex, p1.hashHex);
  const UrlKey off2 = resolveEntry(*kOff.key(u2), dir);
  EXPECT_EQ(off2.hashHex, p1.hashHex);
  EXPECT_EQ(off2.key, p1.key);
  // A key given by its name, as `ls` prints it, too.
  EXPECT_EQ(resolveEntry(*kOn.key("lfn:/store/mc/a.root"), dir).hashHex, p1.hashHex);
}

// A file met first with the prefixes set: a new entry under the name, which each URL reaches, also
// with the prefixes off.
TEST(EntryNames, ANewEntryIsTheNamesAndEveryUrlReachesIt) {
  TempDir td;
  const std::string dir = td.path();
  Writable w;
  const UrlKey n = *kOn.key("root://x.example//store/data/R/b.root");
  const UrlKey e = resolveEntry(n, dir);
  EXPECT_EQ(e.hashHex, n.hashHex);
  EXPECT_EQ(e.key, "lfn:/store/data/R/b.root");
  fill(dir, e);
  EXPECT_EQ(resolveEntry(*kOff.key("root://x.example//store/data/R/b.root"), dir).hashHex, n.hashHex);
  EXPECT_EQ(resolveEntry(*kOn.key("root://y.example//store/data/R/b.root"), dir).hashHex, n.hashHex);
  EXPECT_EQ(resolveEntry(*kOff.key("root://y.example//store/data/R/b.root"), dir).hashHex, n.hashHex);
  // The entry removed: with the prefixes off each URL's key is its own again; with them set, the
  // name still gives its entry's hash (made again there at the next fill).
  ASSERT_EQ(::unlink(n.metaPath(dir).c_str()), 0);
  const UrlKey y = *kOff.key("root://y.example//store/data/R/b.root");
  EXPECT_EQ(resolveEntry(y, dir).hashHex, y.hashHex);
  EXPECT_EQ(resolveEntry(*kOn.key("root://y.example//store/data/R/b.root"), dir).hashHex, n.hashHex);
}

// Two entries of one name cached before the prefixes (two URLs of one file): the first one met is
// the name's; the other URL joins it, and its own entry is that URL's again with the prefixes off.
TEST(EntryNames, OfTwoEntriesTheFirstMetIsTheNames) {
  TempDir td;
  const std::string dir = td.path();
  const std::string u1 = "root://a.example//store/mc/c.root", u2 = "root://b.example//store/mc/c.root";
  const UrlKey p1 = *kOff.key(u1), p2 = *kOff.key(u2);
  makeEntry(dir, p1);
  makeEntry(dir, p2);
  Writable w;
  EXPECT_EQ(resolveEntry(*kOn.key(u2), dir).hashHex, p2.hashHex); // met first
  EXPECT_EQ(resolveEntry(*kOn.key(u1), dir).hashHex, p2.hashHex); // joins it
  EXPECT_EQ(resolveEntry(p1, dir).hashHex, p1.hashHex);           // off: its own entry again
  EXPECT_EQ(resolveEntry(p2, dir).hashHex, p2.hashHex);
}

// The prefixes off and a cache that never had names: every key is its own, and nothing of names is
// written or read; the CLI (not writable) reads names but gives none.
TEST(EntryNames, WithoutNamesNothingChanges) {
  TempDir td;
  const std::string dir = td.path();
  Writable w;
  const UrlKey k = *kOff.key("root://h//store/mc/a.root");
  const UrlKey r = resolveEntry(k, dir);
  EXPECT_EQ(r.hashHex, k.hashHex);
  EXPECT_EQ(r.key, k.key);
  EXPECT_FALSE(exists(entryNamesDir(dir)));
  setEntryNamesWritable(false);
  const UrlKey n = *kOn.key("root://h//store/mc/z.root");
  EXPECT_EQ(resolveEntry(n, dir).hashHex, n.hashHex);
  EXPECT_FALSE(exists(entryNamesDir(dir))); // read only: no alias, no list
  EXPECT_EQ(resolveEntry(k, "").hashHex, k.hashHex); // no cache: the key itself
}

// An alias is made once and never changed; one that does not parse (an earlier format too) is no
// alias, and one of another generation of its entry is none either.
TEST(EntryNames, AnAliasIsMadeOnceAndAMalformedOneIsIgnored) {
  TempDir td;
  const std::string dir = td.path();
  Writable w;
  const UrlKey n = *kOn.key("root://h//store/mc/d.root");
  const UrlKey other = UrlKey::ofEntry("root://e.example:1094/elsewhere", std::string(64, 'e'));
  makeEntry(dir, other); // the entry another process named
  {
    RealIO io;
    auto m = MetaFile::load(io, other.metaPath(dir));
    ASSERT_TRUE(m);
    m->gen = 0x12ab;
    ASSERT_EQ(MetaFile::store(io, other.metaPath(dir), *m, false), 0);
    EXPECT_EQ(MetaFile::loadHead(io, other.metaPath(dir))->gen, 0x12abu);
  }
  auto put = [&dir](const UrlKey& name, const std::string& body) {
    ::mkdir(entryNamesDir(dir).c_str(), 0755);
    ::mkdir((entryNamesDir(dir) + "/" + name.hashHex.substr(0, 2)).c_str(), 0755);
    std::ofstream(aliasPath(dir, name.hashHex)) << body;
  };
  const UrlKey n1 = *kOn.key("root://h//store/mc/d1.root");
  const UrlKey n2 = *kOn.key("root://h//store/mc/d2.root");
  put(n1, "ucache-alias 1 " + other.hashHex + " " + other.key + "\n");
  EXPECT_EQ(resolveEntry(n1, dir).hashHex, n1.hashHex); // the earlier format: no alias
  put(n2, "ucache-alias 2 " + other.hashHex + " 0012ac c " + other.key + "\n");
  EXPECT_EQ(resolveEntry(n2, dir).hashHex, n2.hashHex); // another generation: no alias
  put(n, "ucache-alias 2 " + other.hashHex + " 0012ab c " + other.key + "\n");
  const UrlKey e = resolveEntry(n, dir);
  EXPECT_EQ(e.hashHex, other.hashHex); // the alias another process made first
  EXPECT_EQ(e.key, other.key);
  EXPECT_EQ(e.entryGen, 0x12abu);
  const UrlKey m = *kOn.key("root://h//store/mc/f.root");
  ::mkdir((entryNamesDir(dir) + "/" + m.hashHex.substr(0, 2)).c_str(), 0755);
  std::ofstream(aliasPath(dir, m.hashHex)) << "ucache-alias 1 not-a-hash x\n";
  EXPECT_EQ(resolveEntry(m, dir).hashHex, m.hashHex);
}

// The list of names keeps the logical name and the newest kEntryNamesMax URLs (every URL stays an
// alias); a torn last line is skipped; rm drops the list.
TEST(EntryNames, TheListIsBounded) {
  TempDir td;
  const std::string dir = td.path();
  Writable w;
  std::string entry;
  for (int i = 0; i < 12; ++i) {
    const UrlKey e = resolveEntry(*kOn.key("root://s" + std::to_string(i) + ".example//store/mc/g.root"), dir);
    if (i == 0) {
      entry = e.hashHex;
      fill(dir, e); // the entry is filled (the names hold also while it is not)
    }
    EXPECT_EQ(e.hashHex, entry) << i;
  }
  EntryNameList names = entryNames(dir, entry);
  EXPECT_EQ(names.lfn, "lfn:/store/mc/g.root");
  ASSERT_EQ(names.urls.size(), kEntryNamesMax);
  EXPECT_EQ(names.urls.back(), "root://s11.example:1094/store/mc/g.root");
  EXPECT_EQ(names.urls.front(), "root://s4.example:1094/store/mc/g.root");
  for (int i = 0; i < 12; ++i) // every URL still reaches the entry with the prefixes off
    EXPECT_EQ(resolveEntry(*kOff.key("root://s" + std::to_string(i) + ".example//store/mc/g.root"), dir).hashHex,
              entry) << i;
  std::ofstream(namesPath(dir, entry), std::ios::app) << "url root://torn";
  EXPECT_EQ(entryNames(dir, entry).urls.back(), "root://s11.example:1094/store/mc/g.root");
  dropEntryNames(dir, entry);
  EXPECT_TRUE(entryNames(dir, entry).urls.empty());
}

// The entries already cached are named once when the prefixes are set (`ucache set`, or the
// plug-in's first process): another URL of the file then finds its entry at once; of two entries of
// one file, the most recently used is the name's; a cache named for these prefixes is not scanned
// again.
TEST(EntryNames, CachedEntriesAreNamedOnce) {
  TempDir td;
  const std::string dir = td.path();
  const UrlKey p1 = *kOff.key("root://a.example//store/mc/h.root");
  const UrlKey p2 = *kOff.key("root://b.example//store/mc/h.root");
  const UrlKey other = *kOff.key("root://a.example//data/x.root");
  makeEntry(dir, p1);
  makeEntry(dir, other);
  makeEntry(dir, p2);
  EXPECT_EQ(nameCachedEntries(dir, kOff), -1); // no names to give
  setEntryNamesWritable(false);                // the CLI's case: the scan names all the same
  EXPECT_EQ(nameCachedEntries(dir, kOn), 2);
  EXPECT_EQ(nameCachedEntries(dir, kOn), -1); // done for these prefixes
  const UrlKey third = resolveEntry(*kOn.key("root://c.example//store/mc/h.root"), dir);
  EXPECT_TRUE(third.hashHex == p2.hashHex || third.hashHex == p1.hashHex);
  EXPECT_EQ(resolveEntry(*kOn.key("root://a.example//store/mc/h.root"), dir).hashHex, third.hashHex);
  EXPECT_EQ(resolveEntry(*kOn.key("root://b.example//store/mc/h.root"), dir).hashHex, third.hashHex);
  EXPECT_EQ(resolveEntry(other, dir).hashHex, other.hashHex);
  EXPECT_FALSE(entryNames(dir, third.hashHex).lfn.empty());
}

// The prefixes turned off and set again (the same list too): the entries cached by URL in between
// are named then, once the namings done were forgotten (`ucache set` to another list or off, and
// `unset`, call forgetNamings). Without that, a cache named once for a list was never scanned again.
// A naming asked to stop leaves no marker: the next one names the rest.
TEST(EntryNames, NamedAgainAfterTheNamingsAreForgotten) {
  TempDir td;
  const std::string dir = td.path();
  const UrlKey p1 = *kOff.key("root://a.example//store/mc/i.root");
  makeEntry(dir, p1);
  EXPECT_EQ(nameCachedEntries(dir, kOn), 1);
  EXPECT_EQ(nameCachedEntries(dir, kOn), -1); // done for these prefixes
  // the prefixes off: a file cached by URL
  const UrlKey p2 = *kOff.key("root://a.example//store/mc/j.root");
  makeEntry(dir, p2);
  EXPECT_EQ(forgetNamings(dir), 1u);
  EXPECT_EQ(forgetNamings(dir), 0u);
  // set again: the scan runs, and the file cached meanwhile is found by another URL
  EXPECT_GE(nameCachedEntries(dir, kOn), 1);
  EXPECT_EQ(resolveEntry(*kOn.key("root://b.example//store/mc/j.root"), dir).hashHex, p2.hashHex);
  EXPECT_EQ(nameCachedEntries(dir, kOn), -1);
  // stopped at once: nothing named, no marker, so the next naming runs
  const UrlKey p3 = *kOff.key("root://a.example//store/mc/k.root");
  makeEntry(dir, p3);
  EXPECT_EQ(forgetNamings(dir), 1u);
  EXPECT_EQ(nameCachedEntries(dir, kOn, [] { return true; }), 0);
  EXPECT_GE(nameCachedEntries(dir, kOn), 1);
  EXPECT_EQ(resolveEntry(*kOn.key("root://b.example//store/mc/k.root"), dir).hashHex, p3.hashHex);
}

// An alias is followed only while its entry exists: once the entry is gone (removed, with its
// aliases or, after a crash between, without them), no name ever makes an entry under its hash. The
// case: a user's copy keyed by the official name, removed; the official file's next fill must not
// land under the user URL's key.
TEST(EntryNames, ADanglingAliasIsNeverFollowed) {
  TempDir td;
  const std::string dir = td.path();
  const std::string user = "root://u.example//user/me/store/mc/test/a.root";
  const std::string official = "root://o.example//store/mc/test/a.root";
  const UrlKey u = *kOff.key(user);
  makeEntry(dir, u);
  Writable w;
  EXPECT_EQ(resolveEntry(*kOn.key(user), dir).hashHex, u.hashHex);     // the user's entry named
  EXPECT_EQ(resolveEntry(*kOn.key(official), dir).hashHex, u.hashHex); // the documented risk
  // The entry goes as a crash would leave it: its files gone, its aliases not.
  ASSERT_EQ(::unlink(u.metaPath(dir).c_str()), 0);
  const UrlKey lfn = *kOn.key(official);
  const UrlKey again = resolveEntry(lfn, dir); // remembered, but its entry is gone: resolved again
  EXPECT_EQ(again.hashHex, lfn.hashHex);       // a new entry under the name's own hash
  EXPECT_EQ(again.key, "lfn:/store/mc/test/a.root");
  fill(dir, again);
  EXPECT_EQ(resolveEntry(u, dir).hashHex, u.hashHex); // prefixes off: the user's URL is its own
  const UrlKey o = *kOff.key(official);
  EXPECT_EQ(resolveEntry(o, dir).hashHex, lfn.hashHex); // reaches the name's entry
}

// Removing an entry removes the aliases its list names and the list; `clear` removes every name;
// the sweep removes what is left dangling.
TEST(EntryNames, NamesGoWithTheirEntry) {
  TempDir td;
  const std::string dir = td.path();
  Writable w;
  const UrlKey n = *kOn.key("root://a.example//store/mc/r.root");
  const UrlKey e = resolveEntry(n, dir);
  makeEntry(dir, e);
  const UrlKey p2 = *kOn.key("root://b.example//store/mc/r.root");
  EXPECT_EQ(resolveEntry(p2, dir).hashHex, e.hashHex);
  const std::string a2 = aliasPath(dir, p2.urlHash);
  ASSERT_TRUE(exists(a2));
  {
    RealIO io;
    Config cfg;
    cfg.cacheDir = dir;
    CacheStore store(io, cfg);
    store.disableStatsDump();
    ASSERT_TRUE(store.removeEntry(e));
  }
  EXPECT_FALSE(exists(a2));
  EXPECT_FALSE(exists(namesPath(dir, e.hashHex)));
  // A drop (a bad copy, a write) takes the names too.
  const UrlKey e2 = resolveEntry(*kOn.key("root://a.example//store/mc/r2.root"), dir);
  makeEntry(dir, e2);
  const UrlKey j2 = *kOn.key("root://b.example//store/mc/r2.root");
  EXPECT_EQ(resolveEntry(j2, dir).hashHex, e2.hashHex);
  ASSERT_TRUE(exists(aliasPath(dir, j2.urlHash)));
  {
    RealIO io;
    Config cfg;
    cfg.cacheDir = dir;
    CacheStore store(io, cfg);
    store.disableStatsDump();
    store.dropEntry(e2);
  }
  EXPECT_FALSE(exists(aliasPath(dir, j2.urlHash)));
  EXPECT_FALSE(exists(e2.metaPath(dir)));
  // A dangling alias (a list past its bound, or a crash) is swept.
  ::mkdir((entryNamesDir(dir) + "/" + std::string(64, 'c').substr(0, 2)).c_str(), 0755);
  std::ofstream(aliasPath(dir, std::string(64, 'c'))) << "ucache-alias 1 " << std::string(64, 'd') << " x\n";
  std::ofstream(namesPath(dir, std::string(64, 'd'))) << "url x\n";
  size_t swept = 0;
  for (int i = 0; i < 256 && exists(aliasPath(dir, std::string(64, 'c'))); ++i)
    swept += sweepEntryNames(dir, 64);
  EXPECT_FALSE(exists(aliasPath(dir, std::string(64, 'c'))));
  EXPECT_FALSE(exists(namesPath(dir, std::string(64, 'd'))));
  EXPECT_GE(swept, 1u);
  resolveEntry(*kOn.key("root://a.example//store/mc/s.root"), dir);
  ASSERT_TRUE(exists(entryNamesDir(dir)));
  clearEntryNames(dir);
  EXPECT_FALSE(exists(entryNamesDir(dir)));
}

// A URL whose copy is not the name's entry's size keeps an entry of its own; and the naming of the
// cached entries does not join two entries of different sizes.
TEST(EntryNames, ACopyOfAnotherSizeKeepsItsOwnEntry) {
  TempDir td;
  const std::string dir = td.path();
  Writable w;
  const UrlKey e = resolveEntry(*kOn.key("root://a.example//store/mc/t.root"), dir);
  fill(dir, e);
  const UrlKey j = resolveEntry(*kOn.key("root://b.example//store/mc/t.root"), dir);
  EXPECT_EQ(j.hashHex, e.hashHex);
  EXPECT_TRUE(j.joined); // the caller compares the sizes before it trusts the entry
  keepOwnEntry(j, dir);   // it found another size
  const UrlKey own = UrlKey::ofEntry(j.urlKey, j.urlHash);
  makeEntry(dir, own, false, 60000);
  EXPECT_EQ(resolveEntry(*kOn.key("root://b.example//store/mc/t.root"), dir).hashHex, j.urlHash);
  EXPECT_FALSE(resolveEntry(*kOn.key("root://a.example//store/mc/t.root"), dir).joined);
  // The scan: two old entries of one name, of two sizes.
  TempDir td2;
  const std::string d2 = td2.path();
  const UrlKey q1 = *kOff.key("root://a.example//store/mc/v.root");
  const UrlKey q2 = *kOff.key("root://b.example//store/mc/v.root");
  makeEntry(d2, q1, false, 50000);
  makeEntry(d2, q2, false, 70000);
  EXPECT_EQ(nameCachedEntries(d2, kOn), 1); // one named; the other kept on its own
  const UrlKey r1 = resolveEntry(*kOn.key("root://a.example//store/mc/v.root"), d2);
  const UrlKey r2 = resolveEntry(*kOn.key("root://b.example//store/mc/v.root"), d2);
  EXPECT_NE(r1.hashHex, r2.hashHex);
  EXPECT_EQ(r1.hashHex, q1.hashHex); // each URL its own entry, whichever was named
  EXPECT_EQ(r2.hashHex, q2.hashHex);
}

// ---- The rows below also build on the change before generations, where they fail. ----

namespace {
// `n` URLs of the logical name `lfn` whose own keys' hashes begin with `hh` (so that their aliases
// share one shard of names/).
std::vector<std::string> urlsInShard(const std::string& lfn, const std::string& hh, int n) {
  std::vector<std::string> out;
  for (int i = 0; static_cast<int>(out.size()) < n; ++i) {
    const std::string u = "root://s" + std::to_string(i) + ".example/" + lfn;
    if (kOff.key(u)->hashHex.compare(0, 2, hh) == 0)
      out.push_back(u);
  }
  return out;
}
size_t removeWithStore(const std::string& dir, const UrlKey& k) {
  RealIO io;
  Config cfg;
  cfg.cacheDir = dir;
  CacheStore store(io, cfg);
  store.disableStatsDump();
  return store.removeEntry(k) ? 1 : 0;
}
} // namespace

// An alias the entry's list does not hold (the list keeps the newest 8 URLs) is left when the entry
// goes; once the same hash holds an entry again (the user's URL read again with the prefixes off),
// that alias is never followed: the official URL is its own key, not the user's copy.
TEST(EntryNames, AnAliasLostFromTheListIsNotRevived) {
  TempDir td;
  const std::string dir = td.path();
  const std::string user = "root://u.example//user/me/store/mc/test/a.root";
  const std::string official = "root://o.example//store/mc/test/a.root";
  const UrlKey u = *kOff.key(user);
  makeEntry(dir, u);
  Writable w;
  EXPECT_EQ(resolveEntry(*kOn.key(user), dir).hashHex, u.hashHex);
  EXPECT_EQ(resolveEntry(*kOn.key(official), dir).hashHex, u.hashHex); // the documented risk
  for (int i = 0; i < 17; ++i) {
    const std::string s = "root://s" + std::to_string(i) + ".example//store/mc/test/a.root";
    EXPECT_EQ(resolveEntry(*kOn.key(s), dir).hashHex, u.hashHex);
  }
  const UrlKey o = *kOff.key(official);
  bool listed = false;
  for (const auto& x : entryNames(dir, u.hashHex).urls)
    listed = listed || x == o.key;
  ASSERT_FALSE(listed); // the official URL's line is gone from the bounded list
  ASSERT_EQ(removeWithStore(dir, u), 1u); // the documented remedy: rm (the prefixes off)
  ASSERT_TRUE(exists(aliasPath(dir, o.hashHex))); // the official URL's alias is left behind
  makeEntry(dir, u); // the user's URL read again with the prefixes off: the user's copy
  setEntryNamesWritable(false);
  EXPECT_EQ(resolveEntry(o, dir).hashHex, o.hashHex) << "the official URL reached the user's copy";
  setEntryNamesWritable(true);
  EXPECT_EQ(resolveEntry(*kOn.key(official), dir).hashHex, kOn.key(official)->hashHex)
      << "the name reached the user's copy";
}

// The same with the alias left by a crash between a removal's unlinks and its names' removal.
TEST(EntryNames, AnAliasLeftByACrashIsNotRevived) {
  TempDir td;
  const std::string dir = td.path();
  const std::string user = "root://u.example//user/me/store/mc/test/b.root";
  const std::string official = "root://o.example//store/mc/test/b.root";
  const UrlKey u = *kOff.key(user);
  makeEntry(dir, u);
  {
    Writable w;
    EXPECT_EQ(resolveEntry(*kOn.key(user), dir).hashHex, u.hashHex);
    EXPECT_EQ(resolveEntry(*kOn.key(official), dir).hashHex, u.hashHex);
  }
  ASSERT_EQ(::unlink(u.metaPath(dir).c_str()), 0); // the crash: files gone, aliases not
  makeEntry(dir, u);                               // the user's URL, prefixes off, read again
  const UrlKey o = *kOff.key(official);
  EXPECT_EQ(resolveEntry(o, dir).hashHex, o.hashHex) << "the official URL reached the user's copy";
  EXPECT_EQ(resolveEntry(*kOn.key(official), dir).hashHex, kOn.key(official)->hashHex);
}

// A URL whose own entry (cached with the prefixes off) is another size than the name's entry keeps
// its own entry; of the same size, it joins the name's, compared already (not to be opened again).
TEST(EntryNames, AUrlWithItsOwnEntryOfAnotherSizeKeepsIt) {
  TempDir td;
  const std::string dir = td.path();
  Writable w;
  const UrlKey e = resolveEntry(*kOn.key("root://a.example//store/mc/w.root"), dir);
  fill(dir, e, 50000);
  const UrlKey ob = *kOff.key("root://b.example//store/mc/w.root");
  makeEntry(dir, ob, false, 60000); // b's own copy, another size, cached with the prefixes off
  const UrlKey j = resolveEntry(*kOn.key("root://b.example//store/mc/w.root"), dir);
  EXPECT_EQ(j.hashHex, ob.hashHex) << "b's URL reached the name's entry of another size";
  EXPECT_EQ(resolveEntry(*kOn.key("root://b.example//store/mc/w.root"), dir).hashHex, ob.hashHex);
  const UrlKey oc = *kOff.key("root://c.example//store/mc/w.root");
  makeEntry(dir, oc, false, 50000); // c's own copy, the name's size
  const UrlKey jc = resolveEntry(*kOn.key("root://c.example//store/mc/w.root"), dir);
  EXPECT_EQ(jc.hashHex, e.hashHex);
  EXPECT_FALSE(jc.joined);
}

// The naming of the cached entries compares a URL entry's size with an entry under the name's own
// hash too: a URL entry of another size keeps its own entry.
TEST(EntryNames, TheNamingComparesWithAnEntryUnderTheName) {
  TempDir td;
  const std::string dir = td.path();
  {
    Writable w;
    const UrlKey e = resolveEntry(*kOn.key("root://a.example//store/mc/x.root"), dir);
    fill(dir, e, 50000);
  }
  const UrlKey ob = *kOff.key("root://b.example//store/mc/x.root");
  makeEntry(dir, ob, false, 70000);
  EXPECT_GE(nameCachedEntries(dir, kOn), 0);
  setEntryNamesWritable(false); // the CLI's view of what the naming did
  EXPECT_EQ(resolveEntry(*kOn.key("root://b.example//store/mc/x.root"), dir).hashHex, ob.hashHex)
      << "b (70000 bytes) is a name of the 50000-byte entry";
}

// The eviction pass's sweep goes on, call after call, from where it stopped: in a shard of more
// live files than its budget, every dangling alias is reached.
TEST(EntryNames, TheSweepReachesEveryFileOfAShard) {
  TempDir td;
  const std::string dir = td.path();
  Writable w;
  const UrlKey dead = resolveEntry(*kOn.key("root://first.example//store/mc/dead.root"), dir);
  fill(dir, dead);
  const UrlKey live = resolveEntry(*kOn.key("root://first.example//store/mc/live.root"), dir);
  fill(dir, live);
  const auto deadUrls = urlsInShard("/store/mc/dead.root", "cc", 300);
  const auto liveUrls = urlsInShard("/store/mc/live.root", "cc", 300);
  for (size_t i = 0; i < 300; ++i) { // made in turn, whatever order a directory lists them in
    ASSERT_EQ(resolveEntry(*kOn.key(liveUrls[i]), dir).hashHex, live.hashHex);
    ASSERT_EQ(resolveEntry(*kOn.key(deadUrls[i]), dir).hashHex, dead.hashHex);
  }
  ASSERT_EQ(::unlink(dead.metaPath(dir).c_str()), 0); // its aliases left (a crash)
  size_t swept = 0;
  for (int i = 0; i < 50; ++i)
    swept += sweepEntryNames(dir, 256);
  size_t deadLeft = 0, liveLeft = 0;
  for (const auto& u : deadUrls)
    deadLeft += exists(aliasPath(dir, kOff.key(u)->hashHex));
  for (const auto& u : liveUrls)
    liveLeft += exists(aliasPath(dir, kOff.key(u)->hashHex));
  EXPECT_EQ(deadLeft, 0u) << "dangling aliases the sweep never reached (" << swept << " removed)";
  EXPECT_EQ(liveLeft, 300u);
}

// A URL that joined another URL's entry and whose copy was not compared yet is "joined" (to be
// compared at its origin before the entry is trusted) every time it is resolved, in this process
// and in another, until it is compared.
TEST(EntryNames, AnUncomparedUrlIsJoinedInEveryProcess) {
  TempDir td;
  const std::string dir = td.path();
  Writable w;
  const UrlKey e = resolveEntry(*kOn.key("root://a.example//store/mc/y.root"), dir);
  fill(dir, e);
  const UrlKey b = *kOn.key("root://b.example//store/mc/y.root");
  EXPECT_TRUE(resolveEntry(b, dir).joined);
  EXPECT_TRUE(resolveEntry(b, dir).joined) << "the second resolution trusts it unchecked";
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0)
    ::_exit(resolveEntry(b, dir).joined ? 0 : 1);
  int st = 0;
  ASSERT_EQ(::waitpid(pid, &st, 0), pid);
  EXPECT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0) << "another process trusts it unchecked";
}

// A process that resolved a URL to the name's entry sees it kept apart once another process found
// its copy another size.
TEST(EntryNames, AUrlKeptApartByAnotherProcessIsKeptApartHere) {
  TempDir td;
  const std::string dir = td.path();
  Writable w;
  const UrlKey e = resolveEntry(*kOn.key("root://a.example//store/mc/z.root"), dir);
  fill(dir, e);
  const UrlKey b = *kOn.key("root://b.example//store/mc/z.root");
  const UrlKey j = resolveEntry(b, dir);
  ASSERT_EQ(j.hashHex, e.hashHex);
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) { // the other process: b's copy is another size
    keepOwnEntry(j, dir);
    ::_exit(0);
  }
  int st = 0;
  ASSERT_EQ(::waitpid(pid, &st, 0), pid);
  makeEntry(dir, UrlKey::ofEntry(j.urlKey, j.urlHash), false, 60000); // its own entry, made there
  EXPECT_EQ(resolveEntry(b, dir).hashHex, j.urlHash)
      << "this process still gives b the name's entry";
}

// An entry reached through an alias, under another key's hash, is never made, nor started again,
// for that key: an alias resolved just before its entry went (or was made again) opens nothing.
TEST(EntryNames, AnEntryReachedThroughANameIsNeverStartedUnderItsHash) {
  TempDir td;
  const std::string dir = td.path();
  const std::string user = "root://u.example//user/me/store/mc/test/c.root";
  const std::string official = "root://o.example//store/mc/test/c.root";
  const UrlKey u = *kOff.key(user);
  makeEntry(dir, u);
  {
    Writable w;
    EXPECT_EQ(resolveEntry(*kOn.key(user), dir).hashHex, u.hashHex);
    EXPECT_EQ(resolveEntry(*kOn.key(official), dir).hashHex, u.hashHex);
  }
  const UrlKey r = resolveEntry(*kOff.key(official), dir); // the prefixes off: the alias
  ASSERT_EQ(r.hashHex, u.hashHex);
  RealIO io;
  Config cfg;
  cfg.cacheDir = dir;
  {
    ASSERT_EQ(removeWithStore(dir, u), 1u); // the entry goes between the resolution and the open
    CacheStore store(io, cfg);
    store.disableStatsDump();
    EXPECT_FALSE(store.open(r, 50000)) << "an entry was made under the user's URL's hash";
    EXPECT_FALSE(exists(u.metaPath(dir)));
  }
  makeEntry(dir, u); // the user's URL read again: another generation under that hash
  {
    CacheStore store(io, cfg);
    store.disableStatsDump();
    EXPECT_FALSE(store.open(r, 50000)) << "the new generation was taken through the old name";
  }
}

// ---- The generation of an entry and the comparison of a URL's copy (no counterpart before). ----

// Once a URL's copy was found the entry's size (the plug-in, after its open at the origin), the
// URL is no longer "joined", here or in another process.
TEST(EntryNames, AUrlIsComparedOnce) {
  TempDir td;
  const std::string dir = td.path();
  Writable w;
  const UrlKey e = resolveEntry(*kOn.key("root://a.example//store/mc/q.root"), dir);
  fill(dir, e);
  const UrlKey b = *kOn.key("root://b.example//store/mc/q.root");
  const UrlKey j = resolveEntry(b, dir);
  ASSERT_TRUE(j.joined);
  confirmEntryName(j, dir);
  EXPECT_FALSE(resolveEntry(b, dir).joined);
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0)
    ::_exit(resolveEntry(b, dir).joined ? 1 : 0);
  int st = 0;
  ASSERT_EQ(::waitpid(pid, &st, 0), pid);
  EXPECT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  // The URL that filled the entry is never joined to it.
  EXPECT_FALSE(resolveEntry(*kOn.key("root://a.example//store/mc/q.root"), dir).joined);
}

// An entry's generation is given when a name of another key is made to it, is kept by every write
// of the entry's sidecar (a pin, the entry's own commits), and is gone when the entry starts
// afresh: the names made to it are then names of nothing.
TEST(EntryNames, AGenerationLastsAsLongAsTheEntrysBytes) {
  TempDir td;
  const std::string dir = td.path();
  const UrlKey u = *kOff.key("root://u.example//store/mc/g1.root");
  makeEntry(dir, u);
  RealIO io;
  EXPECT_EQ(MetaFile::loadHead(io, u.metaPath(dir))->gen, 0u); // no name of it: none
  Writable w;
  const UrlKey o = *kOn.key("root://o.example//store/mc/g1.root");
  EXPECT_EQ(resolveEntry(*kOn.key("root://u.example//store/mc/g1.root"), dir).hashHex, u.hashHex);
  const uint32_t gen = MetaFile::loadHead(io, u.metaPath(dir))->gen;
  EXPECT_NE(gen, 0u);
  const UrlKey r = resolveEntry(o, dir);
  EXPECT_EQ(r.hashHex, u.hashHex);
  EXPECT_EQ(r.entryGen, gen);
  Config cfg;
  cfg.cacheDir = dir;
  {
    CacheStore store(io, cfg);
    store.disableStatsDump();
    ASSERT_TRUE(store.setPinnedByKey(u, true));
    auto e = store.open(r, 50000); // through the name, as it is
    ASSERT_TRUE(e);
    e->setPinned(false); // the entry's own commit
  }
  EXPECT_EQ(MetaFile::loadHead(io, u.metaPath(dir))->gen, gen);
  setEntryNamesWritable(false);
  EXPECT_EQ(resolveEntry(*kOff.key("root://o.example//store/mc/g1.root"), dir).hashHex, u.hashHex);
  {
    CacheStore store(io, cfg);
    store.disableStatsDump();
    ASSERT_TRUE(store.open(u, 60000)); // the user's file changed: the entry starts afresh
  }
  EXPECT_EQ(MetaFile::loadHead(io, u.metaPath(dir))->gen, 0u);
  const UrlKey off = *kOff.key("root://o.example//store/mc/g1.root");
  EXPECT_EQ(resolveEntry(off, dir).hashHex, off.hashHex); // its names name nothing now
  // Generations are not given twice: the next one differs.
  setEntryNamesWritable(true);
  EXPECT_EQ(resolveEntry(*kOn.key("root://u.example//store/mc/g1.root"), dir).hashHex, u.hashHex);
  const uint32_t gen2 = MetaFile::loadHead(io, u.metaPath(dir))->gen;
  EXPECT_NE(gen2, 0u);
  EXPECT_NE(gen2, gen);
}

// The sweep removes a temporary file of names/ left by a process that ended while writing it, once
// it is an hour old, and not before.
TEST(EntryNames, TheSweepRemovesOldTemporaryFiles) {
  TempDir td;
  const std::string dir = td.path();
  const std::string sh = entryNamesDir(dir) + "/ab";
  ASSERT_EQ(::mkdir(entryNamesDir(dir).c_str(), 0755), 0);
  ASSERT_EQ(::mkdir(sh.c_str(), 0755), 0);
  const std::string old = sh + "/" + std::string(64, 'a') + ".to.tmp.123.0";
  const std::string young = sh + "/" + std::string(64, 'b') + ".to.tmp.123.1";
  std::ofstream(old) << "x";
  std::ofstream(young) << "x";
  struct ::timespec ts[2] = {{::time(nullptr) - 7200, 0}, {::time(nullptr) - 7200, 0}};
  ASSERT_EQ(::utimensat(AT_FDCWD, old.c_str(), ts, 0), 0);
  for (int i = 0; i < 300; ++i)
    sweepEntryNames(dir, 64);
  EXPECT_FALSE(exists(old));
  EXPECT_TRUE(exists(young));
}

// ---- A sidecar without its data, and the generation through every write of a sidecar. ----

// A removal (eviction, rm) takes no lock: it unlinks the data file, then the sidecar. Naming the
// entry meanwhile (its generation written into the sidecar) never puts the sidecar back once the
// removal took it, and no name of the file leads to what is left.
TEST(EntryNames, ANamingNeverPutsBackTheSidecarOfARemovedEntry) {
  TempDir td;
  const std::string dir = td.path();
  Writable w;
  int orphans = 0, nameToOrphan = 0;
  std::mt19937 rng(1);
  for (int i = 0; i < 4000; ++i) {
    const std::string tail = "//store/mc/race/f" + std::to_string(i) + ".root";
    const UrlKey q = *kOff.key("root://q.example" + tail);
    makeEntry(dir, q);
    std::atomic<bool> go{false};
    const int spin = static_cast<int>(rng() % 400);
    std::thread rm([&] {
      while (!go.load()) {
      }
      for (volatile int s = 0; s < spin * 20; s = s + 1) {
      }
      ::unlink(q.dataPath(dir).c_str()); // as eviction: the data first, then the sidecar
      ::unlink(q.metaPath(dir).c_str());
      dropEntryNames(dir, q.hashHex);
    });
    go = true;
    (void)resolveEntry(*kOn.key("root://q.example" + tail), dir); // names it: its generation
    rm.join();
    if (exists(q.metaPath(dir)) && !exists(q.dataPath(dir))) {
      ++orphans;
      setEntryNamesWritable(false);
      const UrlKey r = resolveEntry(*kOn.key("root://o.example" + tail), dir);
      setEntryNamesWritable(true);
      nameToOrphan += r.hashHex == q.hashHex;
    }
  }
  EXPECT_EQ(orphans, 0) << "sidecars with no data file: " << orphans << ", the name leads to "
                        << nameToOrphan;
}

// A sidecar left without its data file all the same (a crash between a removal's two unlinks) is
// no entry: no name of the file leads to it, and the eviction pass removes it, with its names,
// once it is an hour old (not before: an entry is made with its data file first).
TEST(EntryNames, ASidecarWithoutItsDataIsNoEntry) {
  TempDir td;
  const std::string dir = td.path();
  const UrlKey u = *kOff.key("root://u.example//store/mc/half/a.root");
  makeEntry(dir, u);
  Writable w;
  ASSERT_EQ(resolveEntry(*kOn.key("root://u.example//store/mc/half/a.root"), dir).hashHex,
            u.hashHex);
  const UrlKey on = *kOn.key("root://o.example//store/mc/half/a.root");
  ASSERT_EQ(resolveEntry(on, dir).hashHex, u.hashHex);
  ASSERT_EQ(::unlink(u.dataPath(dir).c_str()), 0); // the removal ended between its unlinks
  EXPECT_NE(resolveEntry(on, dir).hashHex, u.hashHex) << "the name leads to a sidecar alone";
  setEntryNamesWritable(false);
  const UrlKey off = *kOff.key("root://o.example//store/mc/half/a.root");
  EXPECT_EQ(resolveEntry(off, dir).hashHex, off.hashHex) << "the URL leads to a sidecar alone";
  setEntryNamesWritable(true);
  RealIO io;
  Config cfg;
  cfg.cacheDir = dir;
  {
    CacheStore store(io, cfg);
    store.disableStatsDump();
    store.evictNow();
  }
  EXPECT_TRUE(exists(u.metaPath(dir))) << "a young sidecar is left: it may be an entry being made";
  struct ::timespec ts[2] = {{::time(nullptr) - 7200, 0}, {::time(nullptr) - 7200, 0}};
  ASSERT_EQ(::utimensat(AT_FDCWD, u.metaPath(dir).c_str(), ts, 0), 0);
  {
    CacheStore store(io, cfg);
    store.disableStatsDump();
    store.evictNow();
  }
  EXPECT_FALSE(exists(u.metaPath(dir))) << "the eviction pass keeps a sidecar with no data";
  EXPECT_FALSE(exists(namesPath(dir, u.hashHex)));
}

// A handle opened before its entry was named keeps the generation the naming gave: its commit
// takes it from the sidecar on disk, so the names made meanwhile still lead to the entry.
TEST(EntryNames, ACommitKeepsTheGenerationGivenMeanwhile) {
  TempDir td;
  const std::string dir = td.path();
  const UrlKey u = *kOff.key("root://u.example//store/mc/hold/a.root");
  RealIO io;
  Config cfg;
  cfg.cacheDir = dir;
  CacheStore store(io, cfg);
  store.disableStatsDump();
  auto e = store.open(u, 50000);
  ASSERT_TRUE(e);
  Writable w;
  ASSERT_EQ(resolveEntry(*kOn.key("root://u.example//store/mc/hold/a.root"), dir).hashHex,
            u.hashHex);
  const uint32_t gen = MetaFile::loadHead(io, u.metaPath(dir))->gen;
  ASSERT_NE(gen, 0u);
  e->setPinned(true); // the handle's commit, its own view of generation 0
  EXPECT_EQ(MetaFile::loadHead(io, u.metaPath(dir))->gen, gen);
  EXPECT_EQ(resolveEntry(*kOn.key("root://o.example//store/mc/hold/a.root"), dir).hashHex,
            u.hashHex);
  setEntryNamesWritable(false);
  const UrlKey off = *kOff.key("root://o.example//store/mc/hold/a.root");
  EXPECT_EQ(resolveEntry(off, dir).hashHex, u.hashHex);
}

// A handle that wrote its view back over an entry another process started afresh (the whole view,
// since the sidecar on disk is another file's) gives it no generation: the names made to the
// generation it held are never followed again.
TEST(EntryNames, AViewWrittenBackOverAFreshStartCarriesNoName) {
  TempDir td;
  const std::string dir = td.path();
  const UrlKey u = *kOff.key("root://u.example//store/mc/back/a.root");
  makeEntry(dir, u);
  const UrlKey off = *kOff.key("root://o.example//store/mc/back/a.root");
  UrlKey r;
  {
    Writable w;
    ASSERT_EQ(resolveEntry(*kOn.key("root://u.example//store/mc/back/a.root"), dir).hashHex,
              u.hashHex);
    r = resolveEntry(*kOn.key("root://o.example//store/mc/back/a.root"), dir);
    ASSERT_EQ(r.hashHex, u.hashHex);
  }
  ASSERT_EQ(resolveEntry(off, dir).hashHex, u.hashHex);
  RealIO io;
  Config cfg;
  cfg.cacheDir = dir;
  CacheStore one(io, cfg); // a process holding the entry through the name
  one.disableStatsDump();
  auto held = one.open(r, 50000);
  ASSERT_TRUE(held);
  {
    CacheStore two(io, cfg); // another, whose copy of the user's file changed: a fresh start
    two.disableStatsDump();
    ASSERT_TRUE(two.open(u, 60000));
  }
  ASSERT_EQ(MetaFile::loadHead(io, u.metaPath(dir))->gen, 0u);
  held->setPinned(true); // the first one's commit: its view, whole
  EXPECT_EQ(MetaFile::loadHead(io, u.metaPath(dir))->fileSize, 50000u);
  EXPECT_EQ(MetaFile::loadHead(io, u.metaPath(dir))->gen, 0u)
      << "the generation the names were made to came back";
  EXPECT_EQ(resolveEntry(off, dir).hashHex, off.hashHex);
}

// The readouts (`ucache ls -l`) list only the names that lead to the entry now: once the entry
// started afresh, the names made to its earlier generation are no longer listed under it, and the
// key it is stored under still is.
TEST(EntryNames, TheReadoutsListOnlyNamesThatLeadToTheEntry) {
  TempDir td;
  const std::string dir = td.path();
  const UrlKey u = *kOff.key("root://u.example//store/mc/list/a.root");
  makeEntry(dir, u);
  {
    Writable w;
    ASSERT_EQ(resolveEntry(*kOn.key("root://u.example//store/mc/list/a.root"), dir).hashHex,
              u.hashHex);
    ASSERT_EQ(resolveEntry(*kOn.key("root://o.example//store/mc/list/a.root"), dir).hashHex,
              u.hashHex);
  }
  const UrlKey o = *kOff.key("root://o.example//store/mc/list/a.root");
  EntryNameList names = liveEntryNames(dir, u.hashHex);
  EXPECT_EQ(names.lfn, "lfn:/store/mc/list/a.root");
  EXPECT_EQ(names.urls.size(), 2u);
  RealIO io;
  Config cfg;
  cfg.cacheDir = dir;
  {
    CacheStore store(io, cfg);
    store.disableStatsDump();
    ASSERT_TRUE(store.open(u, 60000)); // the user's file changed: the entry starts afresh
  }
  EXPECT_EQ(entryNames(dir, u.hashHex).urls.size(), 2u); // the list itself is not rewritten
  names = liveEntryNames(dir, u.hashHex);
  EXPECT_TRUE(names.lfn.empty()) << names.lfn;
  ASSERT_EQ(names.urls.size(), 1u);
  EXPECT_EQ(names.urls[0], u.key);
  EXPECT_NE(names.urls[0], o.key);
}
