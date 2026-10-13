// Keys by logical name (logical_name_prefixes set): how a key
// is derived from a URL, what the switch off leaves alone, the prefixes' checks,
// the readouts' mapping of stored keys and the freshness check of such an entry (the entry's
// names: entry_names_test.cc).
#include "CacheStore.h"
#include "Config.h"
#include "MetaFile.h"
#include "RunLog.h"
#include "UrlKey.h"

#include "TestUtil.h"
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>

#include <string>

using namespace ucache;
using test::TempDir;

namespace {
// CMS's official namespaces, the example value the documentation gives (the default is off).
constexpr const char* kCms = "/store/data/ /store/mc/ /store/hidata/";
KeyRules cms(bool on = true) { return KeyRules({}, on, kCms); }

// The settings with nothing set but `kv` (no environment, no state file).
Config configOf(std::map<std::string, std::string> kv) {
  for (const auto& k : Config::knownKeys())
    if (k.envName)
      ::unsetenv(k.envName);
  return Config::fromEnv(&kv);
}
} // namespace

// One file reached through a redirector, a regional, a site with a prefix and a
// site URL is one key, "lfn:" and the path from /store on; the URL's own key and
// host are kept beside it.
TEST(LogicalNames, OneFileThroughAnyServerIsOneKey) {
  const KeyRules r = cms();
  const char* urls[] = {
      "root://cms-xrd-global.cern.ch//store/mc/Run2/X/NANO/a.root",
      "root://cmsxrootd.fnal.gov:1094//store/mc/Run2/X/NANO/a.root?authz=SECRET&tried=h",
      "root://eoscms.cern.ch//eos/cms/store/mc/Run2/X/NANO/a.root",
      "root://T2.Site.edu:1095//data//./store/mc/Run2/X/NANO/a.root",
  };
  std::string hash;
  for (const char* u : urls) {
    const auto k = r.key(u);
    ASSERT_TRUE(k) << u;
    EXPECT_TRUE(k->logical()) << u;
    EXPECT_EQ(k->key, "lfn:/store/mc/Run2/X/NANO/a.root") << u;
    EXPECT_EQ(k->scheme, "lfn");
    if (hash.empty())
      hash = k->hashHex;
    EXPECT_EQ(k->hashHex, hash) << u;
    // The URL's own key, as with logical names off, and its host (allow/deny match it).
    const auto byUrl = UrlKey::parse(u);
    ASSERT_TRUE(byUrl);
    EXPECT_EQ(k->urlKey, byUrl->key) << u;
    EXPECT_EQ(k->urlHash, byUrl->hashHex) << u;
    EXPECT_EQ(k->host, byUrl->host) << u;
  }
  EXPECT_EQ(r.key(urls[2])->host, "eoscms.cern.ch");
  // Every official namespace of the default; never the query (a token) in the key.
  EXPECT_EQ(r.key("root://h//store/data/Run2025G/a.root")->key, "lfn:/store/data/Run2025G/a.root");
  EXPECT_EQ(r.key("root://h//store/hidata/HIRun/a.root")->key, "lfn:/store/hidata/HIRun/a.root");
  EXPECT_EQ(r.key(urls[1])->key.find("SECRET"), std::string::npos);
}

// With the switch off (and with the prefixes off, or a path that holds none) the
// key is the URL's, byte for byte as UrlKey::parse makes it: nothing changes.
TEST(LogicalNames, SwitchOffKeysByUrlAsBefore) {
  const char* urls[] = {"root://cms-xrd-global.cern.ch//store/mc/Run2/a.root",
                        "root://h:2094//eos/opendata/cms/x.root?authz=t",
                        "root://h//store/user/someone/a.root", "root://h//store/mcx/a.root",
                        "https://web.example//store/data/a.root"};
  for (const char* u : urls) {
    const auto plain = UrlKey::parse(u);
    ASSERT_TRUE(plain) << u;
    for (const KeyRules& r : {cms(false), KeyRules({}, true, "off"), KeyRules({}, false, "")}) {
      const auto k = r.key(u);
      ASSERT_TRUE(k) << u;
      EXPECT_EQ(k->key, plain->key) << u;
      EXPECT_EQ(k->hashHex, plain->hashHex) << u;
      EXPECT_FALSE(k->logical());
      EXPECT_TRUE(k->urlKey.empty());
    }
  }
  // Paths the default does not name keep their URL keys with it on too.
  for (const char* u : {urls[1], urls[2], urls[3]})
    EXPECT_EQ(cms().key(u)->key, UrlKey::parse(u)->key) << u;
}

// The default prefixes: the name runs from the first place a prefix begins to the end of the path,
// for these exactly "/store/" + g1 + "/" + g2 of /store/(data|mc|hidata)/(.*)$: at a site that
// serves /store/ as it is, at one with a prefix before it, on a path that is normalized first, on
// one with /store/mc/ twice (the first), and never with the query. A path with none keeps its URL
// key.
TEST(LogicalNames, TheDefaultPrefixesNameTheRestOfThePath) {
  ASSERT_STREQ(kDefaultLogicalNamePrefixes, "off");
  const KeyRules d({}, true, "/store/data/ /store/mc/ /store/hidata/");
  const struct {
    const char* url;
    const char* g1; // the user's /store/(data|mc|hidata)/(.*)$ on the normalized path
    const char* g2;
  } rows[] = {
      {"root://cmsxrootd.fnal.gov//store/mc/RunX/a.root", "mc", "RunX/a.root"},
      {"root://eoscms.cern.ch//eos/cms/store/data/x/y.root", "data", "x/y.root"},
      {"root://h//store//mc//a.root", "mc", "a.root"},
      {"root://h//site/store/mc/b/store/mc/c.root", "mc", "b/store/mc/c.root"},
      {"root://h//store/hidata/R/b.root?authz=T&x=1", "hidata", "R/b.root"},
  };
  for (const auto& r : rows) {
    const std::string name = std::string("/store/") + r.g1 + "/" + r.g2;
    EXPECT_EQ(d.key(r.url)->key, "lfn:" + name) << r.url;
  }
  for (const char* u : {"root://h//store/user/me/c.root", "root://h//x/store/mcextra/a.root",
                        "root://h//x/mystore/mc/a.root", "root://h//store/mc"}) {
    const auto k = d.key(u);
    EXPECT_FALSE(k->logical()) << u;
    EXPECT_EQ(k->key, UrlKey::parse(u)->key) << u;
  }
  // The leftmost place any prefix begins, whichever prefix it is and wherever it is in the list.
  const KeyRules two({}, true, "/store/mc/ /store/test/");
  EXPECT_EQ(two.key("root://h//store/test/xrootd/T2_X/store/mc/a.root")->key,
            "lfn:/store/test/xrootd/T2_X/store/mc/a.root");
  EXPECT_EQ(two.key("root://h//eos/store/mc/x/store/test/a.root")->key,
            "lfn:/store/mc/x/store/test/a.root");
  // A long path is named as any other.
  std::string tail;
  while (tail.size() < 8192)
    tail += "/abcdefghijklmnopqrstuvwxyz0123456789";
  EXPECT_EQ(d.key("root://h//store/mc" + tail + "/a.root")->key, "lfn:/store/mc" + tail + "/a.root");
}

// The default is not anchored: a user's copy that keeps CMS's layout further down its path is keyed
// by the official name (the risk the docs state).
TEST(LogicalNames, APrefixFurtherDownStillNames) {
  EXPECT_EQ(cms().key("root://users.example//home/me/store/mc/A/a.root")->key,
            "lfn:/store/mc/A/a.root");
  EXPECT_EQ(cms().key("root://user.example//home/x/me/store/data/B/b.root")->key,
            "lfn:/store/data/B/b.root");
}

// Kept CGI follows the name as it follows a URL key's path; an "lfn:" key is
// accepted as it is (what `ls` prints can be given back), its path normalized.
TEST(LogicalNames, KeptCgiAndKeysGivenAsTheyArePrinted) {
  const KeyRules r({"v"}, true, kCms);
  EXPECT_EQ(r.key("root://h//store/mc/a.root?z=1&v=2&authz=x")->key, "lfn:/store/mc/a.root?v=2");
  const auto given = r.key("lfn:/store//mc/./a.root");
  ASSERT_TRUE(given);
  EXPECT_EQ(given->key, "lfn:/store/mc/a.root");
  EXPECT_EQ(given->hashHex, cms().key("root://other//store/mc/a.root")->hashHex);
  EXPECT_TRUE(given->urlKey.empty());
  // UrlKey::parse accepts one too, whatever the switch says.
  EXPECT_EQ(UrlKey::parse("lfn:/store/mc/a.root")->hashHex, given->hashHex);
  EXPECT_FALSE(UrlKey::parse("lfn:store/mc/a.root")); // a name always begins with "/"
}

// An entry's identity is its object name: never the hash of its text again.
TEST(LogicalNames, AnEntrysIdentityIsItsHash) {
  const std::string hash(64, 'a');
  const UrlKey e = UrlKey::ofEntry("root://h:1094/store/mc/a.root", hash);
  EXPECT_EQ(e.hashHex, hash);
  EXPECT_EQ(e.key, "root://h:1094/store/mc/a.root");
  EXPECT_EQ(e.host, "h");
  EXPECT_FALSE(e.logical());
  const UrlKey l = UrlKey::ofEntry("lfn:/store/mc/a.root", hash);
  EXPECT_TRUE(l.logical());
  EXPECT_EQ(l.hashHex, hash);
  EXPECT_EQ(UrlKey::ofEntry("not a key at all", hash).hashHex, hash);
}

// A list that is not valid leaves keys by URL and says why; "off" (or nothing) is valid, and the
// list is held with one space between its entries.
TEST(LogicalNames, BadPrefixesKeyByUrl) {
  for (const char* v : {"store/mc/", "/store/mc", "/", "//", "/store/mc/ x", "/store//mc/",
                        "/store/./mc/", "/store/../mc/", "/st\x01re/"}) {
    std::string why;
    EXPECT_FALSE(logicalNamePrefixesValid(v, &why)) << v;
    EXPECT_FALSE(why.empty()) << v;
    const KeyRules bad({}, true, v);
    EXPECT_FALSE(bad.prefixesValid()) << v;
    EXPECT_FALSE(bad.prefixesError().empty()) << v;
    EXPECT_FALSE(bad.logical()) << v;
    EXPECT_EQ(bad.key("root://h//store/mc/a.root")->key, "root://h:1094/store/mc/a.root") << v;
  }
  EXPECT_FALSE(logicalNamePrefixesValid("/" + std::string(kMaxLogicalNamePrefixBytes, 'a') + "/"));
  std::string many;
  for (size_t i = 0; i <= kMaxLogicalNamePrefixes; ++i)
    many += "/p" + std::to_string(i) + "/ ";
  EXPECT_FALSE(logicalNamePrefixesValid(many));
  for (const char* v : {"off", "", "  ", kCms, "/a/", "  /a/\t/b/c/  "})
    EXPECT_TRUE(logicalNamePrefixesValid(v)) << v;
  EXPECT_EQ(normalizedPrefixes("  /a/\t/b/c/  "), "/a/ /b/c/");
  EXPECT_EQ(normalizedPrefixes(""), "off");
  // off names nothing; the hash is of the list as held.
  const KeyRules off({}, true, "off");
  EXPECT_TRUE(off.prefixesValid());
  EXPECT_FALSE(off.logical());
  EXPECT_TRUE(off.prefixesHash().empty());
  EXPECT_EQ(KeyRules({}, true, "  /a/\t/b/c/ ").prefixesHash(), KeyRules({}, true, "/a/ /b/c/").prefixesHash());
  EXPECT_NE(KeyRules({}, true, "/a/").prefixesHash(), KeyRules({}, true, "/b/").prefixesHash());
}

// The readouts read a stored URL key as the rules key the file now; a key by
// name, and any key with logical names off, is read as it is.
TEST(LogicalNames, StoredKeysMapThroughTheRules) {
  EXPECT_EQ(cms().mapStored("root://cmsxrootd.fnal.gov:1094/store/mc/a.root"),
            "lfn:/store/mc/a.root");
  EXPECT_EQ(cms().mapStored("lfn:/store/mc/a.root"), "lfn:/store/mc/a.root");
  EXPECT_EQ(cms().mapStored("root://h:1094/eos/opendata/x.root"), "root://h:1094/eos/opendata/x.root");
  EXPECT_EQ(cms(false).mapStored("root://h:1094/store/mc/a.root"), "root://h:1094/store/mc/a.root");
  EXPECT_EQ(KeyRules({"v"}, true, kCms)
                .mapStored("root://h:1094/store/mc/a.root?v=2"),
            "lfn:/store/mc/a.root?v=2");
}

// The settings: the prefixes have no environment variable (a property of the cache); off by
// default; set, they key by name whatever adaptive_federation says, and the switch alone keys
// nothing by name; the list is shown as held.
TEST(LogicalNames, Settings) {
  for (const auto& k : Config::knownKeys()) {
    if (std::string(k.key) == "logical_name_prefixes") {
      EXPECT_EQ(k.envName, nullptr);
    }
  }
  Config c = configOf({});
  EXPECT_EQ(c.logicalNamePrefixes, "off");
  EXPECT_EQ(c.valueOf("logical_name_prefixes"), "off");
  EXPECT_FALSE(c.keys.logical());
  EXPECT_EQ(c.keys.key("root://h//store/mc/a.root")->key, UrlKey::parse("root://h//store/mc/a.root")->key);
  c = configOf({{"adaptive_federation", "on"}}); // the switch alone: keys by URL, as before
  EXPECT_FALSE(c.keys.logical());
  EXPECT_EQ(c.keys.key("root://h//store/mc/a.root")->key, UrlKey::parse("root://h//store/mc/a.root")->key);
  for (const char* sw : {"off", "on"}) { // set, the prefixes name files with the switch off or on
    c = configOf({{"adaptive_federation", sw}, {"logical_name_prefixes", kCms}});
    EXPECT_TRUE(c.keys.logical()) << sw;
    EXPECT_EQ(c.keys.key("root://h//store/mc/a.root")->key, "lfn:/store/mc/a.root") << sw;
  }
  c = configOf({{"logical_name_prefixes", " /store/mc/   /x/ "}});
  EXPECT_EQ(c.valueOf("logical_name_prefixes"), "/store/mc/ /x/");
  EXPECT_EQ(c.keys.key("root://h//a/x/b.root")->key, "lfn:/x/b.root");
  EXPECT_FALSE(c.keys.key("root://h//store/data/b.root")->logical());
  c = configOf({{"logical_name_prefixes", "store/x"}});
  EXPECT_FALSE(c.keys.logical());
  EXPECT_FALSE(c.keys.prefixesValid());
  // keep_cgi reaches the rules.
  c = configOf({{"keep_cgi", "v"}, {"logical_name_prefixes", kCms}});
  EXPECT_EQ(c.keys.key("root://h//store/mc/a.root?v=1")->key, "lfn:/store/mc/a.root?v=1");
}

// validate = size+mtime checks an entry by name on its size alone (a modification
// time belongs to one site's copy); every other mode and key is unchanged.
TEST(LogicalNames, FreshnessOfAnEntryByName) {
  Config c;
  c.validate = ValidateMode::kSizeMtime;
  const auto byName = *cms().key("root://h//store/mc/a.root");
  const auto byUrl = *UrlKey::parse("root://h//store/mc/a.root");
  EXPECT_EQ(c.validateFor(byName), ValidateMode::kSize);
  EXPECT_EQ(c.validateFor(byUrl), ValidateMode::kSizeMtime);
  c.validate = ValidateMode::kCksum;
  EXPECT_EQ(c.validateFor(byName), ValidateMode::kCksum);
  // An entry by name opened again with another mtime is adopted, not started again.
  TempDir td;
  RealIO io;
  Config sc;
  sc.cacheDir = td.path();
  sc.validate = ValidateMode::kSizeMtime;
  {
    CacheStore store(io, sc);
    store.disableStatsDump();
    auto e = store.open(byName, 100000, /*mtime=*/111);
    ASSERT_TRUE(e);
    std::vector<uint8_t> page(4096, 7);
    e->writePages(0, page.size(), page.data());
    e->flushAll();
  }
  {
    CacheStore store(io, sc);
    store.disableStatsDump();
    auto e = store.open(byName, 100000, /*mtime=*/222);
    ASSERT_TRUE(e);
    EXPECT_GT(e->cachedBytes(), 0u); // kept
    auto u = store.open(byUrl, 100000, 111);
    ASSERT_TRUE(u);
  }
  {
    CacheStore store(io, sc);
    store.disableStatsDump();
    auto u = store.open(byUrl, 100000, 222); // a URL key still checks its mtime
    ASSERT_TRUE(u);
    EXPECT_EQ(u->cachedBytes(), 0u);
  }
}

// The readouts: a baseline recorded by URL and a run keyed by name are one file
// once the stored keys are mapped, and the run's signature is the same.
TEST(LogicalNames, ReadoutsMatchABaselineKeptByUrl) {
  TempDir td;
  const auto write = [&](uint64_t pid, const std::string& key) {
    const std::string stem = td.path() + "/host-" + std::to_string(pid) + "-1000-0";
    std::ofstream(stem + ".jsonl") << "{\"ts\":2000,\"pid\":" << pid << ",\"opens\":1}\n";
    std::ofstream(stem + ".files.jsonl")
        << "{\"ts\":2000,\"key\":\"" << key << "\",\"opens\":1,\"wire_bytes\":10,\"origin_size\":10}\n";
  };
  write(1, "root://cmsxrootd.fnal.gov:1094/store/mc/a.root");
  write(2, "lfn:/store/mc/a.root");
  const KeyRules on = cms();
  const auto mapped = loadRuns(td.path(), "", &on);
  ASSERT_EQ(mapped.size(), 2u);
  EXPECT_EQ(mapped[0].files.count("lfn:/store/mc/a.root"), 1u);
  EXPECT_EQ(mapped[1].files.count("lfn:/store/mc/a.root"), 1u);
  EXPECT_EQ(mapped[0].sig, mapped[1].sig);
  const auto plain = loadRuns(td.path());
  ASSERT_EQ(plain.size(), 2u);
  EXPECT_NE(plain[0].sig, plain[1].sig); // unmapped, as with the switch off
}

// A switch-off run's record keeps its origin when the readout reads its key as a logical name;
// and the readout maps the keys when any run of the records keyed files by name, even when the
// command's own settings have the switch off. With neither, nothing is mapped.
TEST(LogicalNames, ReadoutsFollowTheRecordsAndKeepOrigins) {
  const auto write = [](const std::string& dir, uint64_t pid, const std::string& key,
                        const std::string& settings) {
    const std::string stem = dir + "/host-" + std::to_string(pid) + "-1000-0";
    std::ofstream(stem + ".jsonl") << "{\"ts\":2000,\"pid\":" << pid
                                   << ",\"opens\":1,\"run_settings\":\"" << settings << "\"}\n";
    std::ofstream(stem + ".files.jsonl")
        << "{\"ts\":2000,\"key\":\"" << key << "\",\"opens\":1,\"wire_bytes\":10,\"origin_size\":10}\n";
  };
  TempDir td;
  write(td.path(), 1, "root://cmsxrootd.fnal.gov:1094/store/mc/a.root",
        "v1 check_origin_integrity=off adaptive_federation=off");
  const KeyRules off = cms(false);
  ASSERT_EQ(off.prefixesHash().size(), 8u);
  write(td.path(), 2, "lfn:/store/mc/a.root",
        "v1 check_origin_integrity=on adaptive_federation=on logical_names=on/" + off.prefixesHash());
  EXPECT_TRUE(recordsKeyedByName(td.path(), off.prefixesHash()));
  EXPECT_FALSE(recordsKeyedByName(td.path(), "00000000"));
  const auto runs = loadRuns(td.path(), "", &off);
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].sig, runs[1].sig); // mapped: the records keyed files by name
  for (const auto& r : runs)
    EXPECT_EQ(r.files.count("lfn:/store/mc/a.root"), 1u);
  const ucache::Run& byUrl = runs[0].pid == 1 ? runs[0] : runs[1];
  EXPECT_EQ(byUrl.originHosts.count("cmsxrootd.fnal.gov"), 1u); // its origin is kept
  TempDir td2;
  write(td2.path(), 1, "root://cmsxrootd.fnal.gov:1094/store/mc/a.root", "v1 adaptive_federation=off");
  write(td2.path(), 2, "root://eoscms.cern.ch:1094/eos/cms/store/mc/a.root", "v1 adaptive_federation=off");
  EXPECT_FALSE(recordsKeyedByName(td2.path(), off.prefixesHash()));
  const auto plain = loadRuns(td2.path(), "", &off);
  ASSERT_EQ(plain.size(), 2u);
  EXPECT_NE(plain[0].sig, plain[1].sig); // nothing keyed by name, the switch off: as written
  EXPECT_TRUE(keyedByName("v1 adaptive_federation=on logical_names=on/abcdef01"));
  EXPECT_FALSE(keyedByName("v1 adaptive_federation=on logical_names=off"));
  EXPECT_EQ(nameHashOf("v1 adaptive_federation=on logical_names=on/abcdef01"), "abcdef01");
  // Records of a run keyed by other prefixes are read as written: they do not join these
  // prefixes' names, and alone they do not make the readout map anything.
  TempDir td3;
  write(td3.path(), 1, "root://cmsxrootd.fnal.gov:1094/store/mc/a.root", "v1 adaptive_federation=off");
  write(td3.path(), 2, "root://eoscms.cern.ch:1094/eos/cms/store/mc/a.root",
        "v1 adaptive_federation=on logical_names=on/deadbeef");
  const auto other = loadRuns(td3.path(), "", &off);
  ASSERT_EQ(other.size(), 2u);
  EXPECT_NE(other[0].sig, other[1].sig);
  const KeyRules on = cms();
  const auto mixed = loadRuns(td3.path(), "", &on); // the command's switch on: its own prefixes
  ASSERT_EQ(mixed.size(), 2u);
  const ucache::Run& byOther = mixed[0].pid == 2 ? mixed[0] : mixed[1];
  EXPECT_EQ(byOther.files.count("root://eoscms.cern.ch:1094/eos/cms/store/mc/a.root"), 1u);
  // A record keyed by name has no host in its key: its origin is the data server it names, with
  // adaptive_federation off too (logical names do not depend on it).
  TempDir td4;
  {
    const std::string stem = td4.path() + "/host-7-1000-0";
    std::ofstream(stem + ".jsonl") << "{\"ts\":2000,\"pid\":7,\"opens\":1,\"run_settings\":\"v1 "
                                      "adaptive_federation=off logical_names=on/"
                                   << on.prefixesHash() << "\"}\n";
    std::ofstream(stem + ".files.jsonl")
        << "{\"ts\":2000,\"key\":\"lfn:/store/mc/a.root\",\"opens\":1,\"wire_bytes\":10,"
           "\"origin_size\":10,\"data_server\":\"disk1.example:1094\"}\n";
  }
  const auto named = loadRuns(td4.path(), "", &on);
  ASSERT_EQ(named.size(), 1u);
  ASSERT_EQ(named[0].originHosts.size(), 1u);
  EXPECT_NE(named[0].originHosts.begin()->first.find("disk1.example"), std::string::npos);
}

// The last key a thread derived is kept per set of rules: rules that key one URL two ways never
// give each other's answer.
TEST(LogicalNames, TheKeptKeyIsPerRules) {
  const KeyRules on = cms(), off = cms(false);
  const KeyRules named = off.withNames();
  const std::string u = "root://h//store/mc/a.root";
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(on.key(u)->logical());
    EXPECT_FALSE(off.key(u)->logical());
    EXPECT_TRUE(named.key(u)->logical());
    EXPECT_TRUE(on.key(u)->logical());
  }
  const KeyRules copy = off;
  EXPECT_FALSE(copy.key(u)->logical());
  EXPECT_EQ(on.key("root://h//store/mc/b.root")->key, "lfn:/store/mc/b.root");
}
