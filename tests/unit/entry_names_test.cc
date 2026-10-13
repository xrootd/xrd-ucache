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
#include <sys/stat.h>

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
void makeEntry(const std::string& dir, const UrlKey& k, bool pin = false) {
  RealIO io;
  Config cfg;
  cfg.cacheDir = dir;
  CacheStore store(io, cfg);
  store.disableStatsDump();
  auto e = store.open(k, 50000);
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
  makeEntry(dir, e);
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

// An alias is made once and never changed; one that does not parse is no alias.
TEST(EntryNames, AnAliasIsMadeOnceAndAMalformedOneIsIgnored) {
  TempDir td;
  const std::string dir = td.path();
  Writable w;
  const UrlKey n = *kOn.key("root://h//store/mc/d.root");
  const std::string other(64, 'e');
  ::mkdir(entryNamesDir(dir).c_str(), 0755);
  ::mkdir((entryNamesDir(dir) + "/" + n.hashHex.substr(0, 2)).c_str(), 0755);
  std::ofstream(aliasPath(dir, n.hashHex)) << "ucache-alias 1 " << other << " lfn:/elsewhere\n";
  const UrlKey e = resolveEntry(n, dir);
  EXPECT_EQ(e.hashHex, other); // the alias another process made first
  EXPECT_EQ(e.key, "lfn:/elsewhere");
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
      makeEntry(dir, e); // the entry is filled (the names hold also while it is not)
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
