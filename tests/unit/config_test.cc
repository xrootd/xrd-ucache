#include "Config.h"
#include "Log.h"

#include "TestUtil.h"
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <map>
#include <sys/stat.h>

using namespace ucache;

namespace {
struct EnvGuard {
  // Tests are hermetic: the legacy config.toml layer is retired, and the state
  // layer only loads from an explicitly-set cache dir. Every UCACHE_* twin is
  // cleared on entry -- a caller's own UCACHE_DIR would otherwise reach the
  // test -- and again on teardown.
  EnvGuard() { clear(); }
  ~EnvGuard() { clear(); }
  static void clear() {
    for (const auto& k : Config::knownKeys())
      if (k.envName)
        ::unsetenv(k.envName);
    for (const auto& r : Config::retiredKeys())
      if (r.envName)
        ::unsetenv(r.envName);
  }
};
} // namespace

TEST(Config, Defaults) {
  EnvGuard g;
  Config c = Config::fromEnv();
  EXPECT_EQ(c.pageSize, Config::kDefaultPageSize); // measured default
#if defined(__APPLE__)
  EXPECT_EQ(Config::kDefaultPageSize, 16384u); // what one memory page takes on disk
#else
  EXPECT_EQ(Config::kDefaultPageSize, 4096u);
#endif
  EXPECT_EQ(c.maxBytes, 0u);
  EXPECT_DOUBLE_EQ(c.highWater, 0.90);
  EXPECT_DOUBLE_EQ(c.lowWater, 0.75);
  EXPECT_EQ(c.validate, ValidateMode::kSize);
  EXPECT_EQ(c.fsync, FsyncMode::kOff);
  EXPECT_EQ(c.maxErrors, 5);
  EXPECT_EQ(c.metaFlushSeconds, 30);
  EXPECT_EQ(c.revalidateSeconds, 604800); // 7-day freshness window
  EXPECT_FALSE(c.recompress); // recompression is a single opt-in switch
  EXPECT_EQ(c.recompressCodecs, (std::vector<std::string>{"lzma", "zlib"}));
  EXPECT_TRUE(c.transpose);
  EXPECT_TRUE(c.announce); // servers are told uCache is in the path, by default
  EXPECT_TRUE(c.copyDetect); // a copy is the origin's bytes, by default
  EXPECT_EQ(c.valueOf("copy_detect"), "on");
  EXPECT_FALSE(c.disable);
  EXPECT_FALSE(c.recompressKeepOriginals); // a converted basket is not also kept
  EXPECT_TRUE(c.cacheDir.empty()); // deliberately NO default cache dir
  EXPECT_TRUE(c.sources.empty());  // nothing explicitly set anywhere
  // failover is on by default: it applies to the files of a redirector, decided per file; the
  // check is not forced for the process.
  EXPECT_TRUE(c.failover);
  EXPECT_EQ(c.valueOf("failover"), "on");
  EXPECT_FALSE(c.checkOriginIntegrity);
}

TEST(Config, ParsesEverything) {
  EnvGuard g;
  ::setenv("UCACHE_DIR", "/x/y", 1);
  ::setenv("UCACHE_PAGE_SIZE", "16k", 1);
  ::setenv("UCACHE_MAX_BYTES", "1000000", 1);
  ::setenv("UCACHE_HIGH_WATER", "0.8", 1);
  ::setenv("UCACHE_LOW_WATER", "0.5", 1);
  ::setenv("UCACHE_VALIDATE", "size+mtime", 1);
  ::setenv("UCACHE_FSYNC", "data", 1);
  ::setenv("UCACHE_THREADS", "4", 1);
  ::setenv("UCACHE_MAX_ERRORS", "9", 1);
  ::setenv("UCACHE_META_FLUSH_S", "7", 1);
  ::setenv("UCACHE_REVALIDATE_S", "0", 1); // 0 must override the 7-day default
  ::setenv("UCACHE_DISABLE", "1", 1);
  ::setenv("UCACHE_TRANSPOSE", "off", 1);
  ::setenv("UCACHE_ANNOUNCE", "off", 1);
  ::setenv("UCACHE_COPY_DETECT", "off", 1);
  ::setenv("UCACHE_RECOMPRESS", "on", 1);
  ::setenv("UCACHE_RECOMPRESS_KEEP_ORIGINALS", "on", 1);
  ::setenv("UCACHE_KEEP_CGI", "a,b", 1);
  ::setenv("UCACHE_ALLOW", "*.cern.ch", 1);
  ::setenv("UCACHE_DENY", "bad.host", 1);
  Config c = Config::fromEnv();
  EXPECT_EQ(c.cacheDir, "/x/y");
  EXPECT_EQ(c.pageSize, 16384u);
  EXPECT_EQ(c.maxBytes, 1000000u);
  EXPECT_DOUBLE_EQ(c.highWater, 0.8);
  EXPECT_DOUBLE_EQ(c.lowWater, 0.5);
  EXPECT_EQ(c.validate, ValidateMode::kSizeMtime);
  EXPECT_EQ(c.fsync, FsyncMode::kData);
  EXPECT_EQ(c.threads, 4);
  EXPECT_EQ(c.maxErrors, 9);
  EXPECT_EQ(c.metaFlushSeconds, 7);
  EXPECT_EQ(c.revalidateSeconds, 0);
  EXPECT_TRUE(c.disable);
  EXPECT_FALSE(c.transpose);
  EXPECT_FALSE(c.announce);
  EXPECT_EQ(c.valueOf("announce"), "off"); // reported by `ucache settings`
  EXPECT_FALSE(c.copyDetect);
  EXPECT_EQ(c.valueOf("copy_detect"), "off");
  EXPECT_EQ(c.sources.at("copy_detect"), "env");
  EXPECT_TRUE(c.recompress);
  EXPECT_TRUE(c.recompressKeepOriginals);
  EXPECT_EQ(c.valueOf("recompress_keep_originals"), "on");
  EXPECT_EQ(c.keepCgi, (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(c.allowHosts, (std::vector<std::string>{"*.cern.ch"}));
  EXPECT_EQ(c.denyHosts, (std::vector<std::string>{"bad.host"}));
  EXPECT_EQ(c.sources.at("revalidate_seconds"), "env");
}

// max_read_fraction: a percentage, 25 by default; 0 (every file read straight
// from the origin) is taken only as a plain zero; anything outside 0..100, or
// not a number, is refused and the value below it stands.
TEST(Config, MaxReadFraction) {
  EnvGuard g;
  {
    Config c = Config::fromEnv();
    EXPECT_EQ(c.maxReadFraction, 25);
    EXPECT_EQ(c.valueOf("max_read_fraction"), "25");
  }
  std::map<std::string, std::string> conf = {{"max_read_fraction", "60"}};
  {
    Config c = Config::fromEnv(&conf);
    EXPECT_EQ(c.maxReadFraction, 60);
    EXPECT_EQ(c.sources.at("max_read_fraction"), "conf");
  }
  ::setenv("UCACHE_MAX_READ_FRACTION", "100", 1);
  {
    Config c = Config::fromEnv(&conf);
    EXPECT_EQ(c.maxReadFraction, 100) << "the per-job override wins";
  }
  for (const char* bad : {"101", "-5", "abc", "", "-0", " 0", "0x", "0.5"}) {
    ::setenv("UCACHE_MAX_READ_FRACTION", bad, 1);
    Config c = Config::fromEnv(&conf);
    EXPECT_EQ(c.maxReadFraction, 60) << "'" << bad << "'";
    EXPECT_EQ(c.valueOf("max_read_fraction"), "60") << "'" << bad << "'";
  }
  for (const char* zero : {"0", "00"}) {
    ::setenv("UCACHE_MAX_READ_FRACTION", zero, 1);
    Config c = Config::fromEnv(&conf);
    EXPECT_EQ(c.maxReadFraction, 0) << zero;
    EXPECT_EQ(c.valueOf("max_read_fraction"), "0") << zero;
    EXPECT_EQ(c.sources.at("max_read_fraction"), "env") << zero;
  }
  ::unsetenv("UCACHE_MAX_READ_FRACTION");
  std::map<std::string, std::string> zconf = {{"max_read_fraction", "0"}};
  {
    Config c = Config::fromEnv(&zconf);
    EXPECT_EQ(c.maxReadFraction, 0) << "0 from the conf";
    EXPECT_EQ(c.sources.at("max_read_fraction"), "conf");
  }
  ::setenv("UCACHE_MAX_READ_FRACTION", "25", 1);
  {
    Config c = Config::fromEnv(&zconf);
    EXPECT_EQ(c.maxReadFraction, 25) << "the per-job override wins over a conf's 0";
  }
  // 1..100 read as before: a leading number
  ::setenv("UCACHE_MAX_READ_FRACTION", "40%", 1);
  {
    Config c = Config::fromEnv(&conf);
    EXPECT_EQ(c.maxReadFraction, 40);
  }
}

// `ucache set max_read_fraction 0` says what 0 does; other values say nothing.
TEST(Config, MaxReadFractionZeroNote) {
  EnvGuard g;
  const Config c = Config::fromEnv();
  const std::string note = c.setNote("max_read_fraction", "0");
  EXPECT_NE(note.find("read straight from the origin"), std::string::npos) << note;
  EXPECT_NE(note.find("other files are cached"), std::string::npos) << note;
  // Only the main tree's (or the RNTuple's) data is read directly: the note says what is kept.
  EXPECT_NE(note.find("its records and its other trees are"), std::string::npos) << note;
  EXPECT_EQ(c.setNote("max_read_fraction", "00"), note);
  EXPECT_EQ(c.setNote("max_read_fraction", "25"), "");
  EXPECT_EQ(c.setNote("max_read_fraction", "100"), "");
  EXPECT_EQ(c.setNote("max_read_fraction", "abc"), "");
  EXPECT_EQ(c.setNote("max_read_fraction", ""), "");
}

// check_origin_integrity: off by default, its own setting. recompress = on does not
// force it (recompression checks every block it converts by its own checksum
// whatever the setting says): it stays as the layers set it.
TEST(Config, CheckOriginIntegrityNotForcedByRecompress) {
  EnvGuard g;
  {
    Config c = Config::fromEnv();
    EXPECT_FALSE(c.checkOriginIntegrity);
    EXPECT_EQ(c.valueOf("check_origin_integrity"), "off");
  }
  std::map<std::string, std::string> conf = {{"check_origin_integrity", "on"}};
  {
    Config c = Config::fromEnv(&conf);
    EXPECT_TRUE(c.checkOriginIntegrity);
    EXPECT_EQ(c.sources.at("check_origin_integrity"), "conf");
  }
  conf = {{"recompress", "on"}};
  {
    Config c = Config::fromEnv(&conf);
    EXPECT_TRUE(c.recompress);
    EXPECT_FALSE(c.checkOriginIntegrity) << "recompress alone leaves it off";
    EXPECT_EQ(c.valueOf("check_origin_integrity"), "off");
    EXPECT_EQ(c.sources.count("check_origin_integrity"), 0u) << "nothing forced it";
  }
  conf = {{"check_origin_integrity", "off"}, {"recompress", "on"}};
  {
    Config c = Config::fromEnv(&conf);
    EXPECT_FALSE(c.checkOriginIntegrity);
    EXPECT_EQ(c.sources.at("check_origin_integrity"), "conf");
  }
  conf = {{"check_origin_integrity", "on"}, {"recompress", "on"}};
  ::setenv("UCACHE_CHECK_ORIGIN_INTEGRITY", "off", 1);
  {
    Config c = Config::fromEnv(&conf);
    EXPECT_FALSE(c.checkOriginIntegrity) << "the environment wins, as for any setting";
    EXPECT_EQ(c.sources.at("check_origin_integrity"), "env");
  }
}

// failover: on by default, set from the conf or its env twin; it never forces
// check_origin_integrity for the process (that is per file, in the plugin), and
// neither does recompress.
TEST(Config, FailoverForcesNothingForTheProcess) {
  EnvGuard g;
  {
    Config c = Config::fromEnv();
    EXPECT_TRUE(c.failover) << "on by default";
    EXPECT_EQ(c.valueOf("failover"), "on");
    EXPECT_EQ(c.sources.count("failover"), 0u);
    EXPECT_FALSE(c.checkOriginIntegrity);
  }
  std::map<std::string, std::string> conf = {{"failover", "on"}};
  {
    Config c = Config::fromEnv(&conf);
    EXPECT_TRUE(c.failover);
    EXPECT_EQ(c.sources.at("failover"), "conf");
    EXPECT_FALSE(c.checkOriginIntegrity) << "forced per file, not for the process";
    EXPECT_EQ(c.sources.count("check_origin_integrity"), 0u);
  }
  conf = {{"failover", "on"}, {"check_origin_integrity", "on"}};
  {
    Config c = Config::fromEnv(&conf);
    EXPECT_TRUE(c.checkOriginIntegrity);
    EXPECT_EQ(c.sources.at("check_origin_integrity"), "conf");
  }
  conf = {{"recompress", "on"}};
  {
    Config c = Config::fromEnv(&conf);
    EXPECT_FALSE(c.checkOriginIntegrity) << "recompress = on forces nothing";
  }
  conf = {{"failover", "on"}};
  ::setenv("UCACHE_FAILOVER", "off", 1);
  {
    Config c = Config::fromEnv(&conf);
    EXPECT_FALSE(c.failover) << "the environment wins";
    EXPECT_EQ(c.valueOf("failover"), "off");
    EXPECT_EQ(c.sources.at("failover"), "env");
  }
}

// failover takes on/off in any case and the words the other switches take (1/true, 0/false);
// any other value -- `auto` among them, a value of development builds -- is ignored (the layer
// below governs, the default when none sets it) and recorded for doctor. The old name is not a
// setting: the parser calls it unknown, as any key it does not know.
TEST(Config, FailoverValues) {
  EnvGuard g;
  for (const char* v : {"ON", "On", "on", "True", "1"}) {
    ::setenv("UCACHE_FAILOVER", v, 1);
    Config c = Config::fromEnv();
    EXPECT_TRUE(c.failover) << v;
    EXPECT_TRUE(c.badFailover.empty()) << v;
  }
  for (const char* v : {"OFF", "Off", "False", "0"}) {
    ::setenv("UCACHE_FAILOVER", v, 1);
    Config c = Config::fromEnv();
    EXPECT_FALSE(c.failover) << v;
    EXPECT_TRUE(c.badFailover.empty()) << v;
  }
  for (const char* v : {"bogus", "yes", "auto", "enabled"}) {
    std::map<std::string, std::string> offConf = {{"failover", "off"}};
    ::setenv("UCACHE_FAILOVER", v, 1);
    Config d = Config::fromEnv();
    EXPECT_TRUE(d.failover) << v << ": ignored, the default holds";
    EXPECT_EQ(d.sources.count("failover"), 0u) << v;
    EXPECT_NE(d.badFailover.find(v), std::string::npos) << d.badFailover;
    Config c = Config::fromEnv(&offConf);
    EXPECT_FALSE(c.failover) << v << ": the layer below (the conf) governs";
    EXPECT_EQ(c.sources.at("failover"), "conf");
    EXPECT_FALSE(c.badFailover.empty());
  }
  ::unsetenv("UCACHE_FAILOVER");
  {
    std::map<std::string, std::string> old = {{"adaptive_federation", "off"}};
    ::setenv("UCACHE_ADAPTIVE_FEDERATION", "off", 1);
    Config c = Config::fromEnv(&old);
    EXPECT_TRUE(c.failover) << "the old name sets nothing";
    EXPECT_EQ(c.sources.count("adaptive_federation"), 0u);
    EXPECT_EQ(c.valueOf("adaptive_federation"), "?");
    EXPECT_FALSE(Config::stateSettable("adaptive_federation"));
    EXPECT_EQ(Config::retiredReason("adaptive_federation"), nullptr) << "never released: no alias";
    ::unsetenv("UCACHE_ADAPTIVE_FEDERATION");
  }
}

// The notes `ucache set` prints: turning the check off while failover is on says
// it stays on for the files failover applies to; setting failover says what it
// does, and never names client connection settings (they are left alone).
TEST(Config, SetNotes) {
  EnvGuard g;
  std::map<std::string, std::string> offConf = {{"failover", "off"}};
  Config plain = Config::fromEnv(&offConf);
  EXPECT_EQ(plain.setNote("check_origin_integrity", "off"), "");
  EXPECT_EQ(plain.setNote("check_origin_integrity", "on"), "");
  const std::string swOff = plain.setNote("failover", "off");
  EXPECT_NE(swOff.find("as in uCache 1.4.0"), std::string::npos) << swOff;
  EXPECT_EQ(plain.setNote("failover", "OFF"), swOff);
  const std::string on = plain.setNote("failover", "on");
  EXPECT_NE(on.find("query config role"), std::string::npos) << on;
  EXPECT_NE(on.find("a site's own redirector included"), std::string::npos) << on;
  EXPECT_NE(on.find("nothing to fail over to"), std::string::npos) << on;
  EXPECT_EQ(on.find("connection window 31 s"), std::string::npos) << on;
  EXPECT_EQ(on.find("federation"), std::string::npos) << on;
  EXPECT_EQ(plain.setNote("failover", "1"), on);
  EXPECT_EQ(plain.setNote("failover", "On"), on);
  EXPECT_EQ(plain.setNote("recompress", "off"), "");

  Config fed = Config::fromEnv();
  const std::string off = fed.setNote("check_origin_integrity", "off");
  EXPECT_NE(off.find("failover forces it on for them"), std::string::npos) << off;
  EXPECT_EQ(fed.setNote("check_origin_integrity", "on"), "");

  std::map<std::string, std::string> conf = {{"recompress", "on"}, {"failover", "off"}};
  Config rec = Config::fromEnv(&conf);
  EXPECT_EQ(rec.setNote("check_origin_integrity", "false"), "")
      << "recompress = on does not force the check: turning it off needs no note";

  // Turning logical names on keeps the cache warm: the note says so, never that files are
  // fetched again.
  const std::string names = plain.setNote("logical_name_prefixes", "/store/mc/");
  EXPECT_NE(names.find("nothing is fetched again"), std::string::npos) << names;
  EXPECT_EQ(names.find("age out"), std::string::npos) << names;
  EXPECT_EQ(plain.setNote("logical_name_prefixes", "off"), "  files are keyed by their URL");
}

// mixed_maps: on by default. in_use_seconds: 1 day, seconds >= 0 only (0 = no
// protection); it replaces evict_protect_seconds and map_expiry_seconds.
TEST(Config, MixedMapsAndTheInUseWindow) {
  EnvGuard g;
  {
    Config c = Config::fromEnv();
    EXPECT_TRUE(c.mixedMaps);
    EXPECT_EQ(c.valueOf("mixed_maps"), "on");
    EXPECT_EQ(c.inUseSeconds, 86400u);
    EXPECT_EQ(c.valueOf("in_use_seconds"), "86400");
  }
  std::map<std::string, std::string> conf = {{"mixed_maps", "off"}, {"in_use_seconds", "3600"}};
  {
    Config c = Config::fromEnv(&conf);
    EXPECT_FALSE(c.mixedMaps);
    EXPECT_EQ(c.inUseSeconds, 3600u);
    EXPECT_EQ(c.sources.at("mixed_maps"), "conf");
    EXPECT_EQ(c.sources.at("in_use_seconds"), "conf");
  }
  ::setenv("UCACHE_MIXED_MAPS", "on", 1);
  ::setenv("UCACHE_IN_USE_S", "0", 1);
  {
    Config c = Config::fromEnv(&conf);
    EXPECT_TRUE(c.mixedMaps) << "the per-job override wins";
    EXPECT_EQ(c.inUseSeconds, 0u);
  }
  for (const char* bad : {"-1", "abc", "", "10x", "99999999999"}) {
    ::setenv("UCACHE_IN_USE_S", bad, 1);
    Config c = Config::fromEnv(&conf);
    EXPECT_EQ(c.inUseSeconds, 3600u) << "'" << bad << "' is refused";
  }
}

// The slot factor is not a setting: where every slot sits follows from it, so
// it is fixed. A conf line naming it is an unknown key: ignored, no source.
TEST(Config, SlotFactorIsNotASetting) {
  EnvGuard g;
  for (const auto& k : Config::knownKeys())
    EXPECT_STRNE(k.key, "recompress_slot_factor");
  std::map<std::string, std::string> conf = {{"recompress_slot_factor", "2.5"}};
  Config c = Config::fromEnv(&conf);
  EXPECT_EQ(c.sources.count("recompress_slot_factor"), 0u);
}

// ucache settings live in the XrdCl plugin conf. Layering:
// defaults < plugin conf < <cacheDir>/state < UCACHE_* env.
TEST(Config, PluginConfMapLayer) {
  EnvGuard g;
  std::map<std::string, std::string> conf = {
      {"url", "*"}, // XrdCl's keys: silently ignored
      {"lib", "/x/libXrdClUCache.so"},
      {"enable", "true"},
      {"dir", "/conf/cache"}, // ours
      {"revalidate_seconds", "42"},
      {"max_bytes", "1000000"},
      {"recompress", "on"},
      {"recompress_codecs", "lzma,zlib"},
      {"recompress_min_share", "25"}, // retired key: warned + ignored
      {"recompress_reclaim", "full"},       // retired key: named + ignored
  };
  Config c = Config::fromEnv(&conf);
  EXPECT_EQ(c.cacheDir, "/conf/cache");
  EXPECT_EQ(c.revalidateSeconds, 42);
  EXPECT_EQ(c.maxBytes, 1000000u);
  EXPECT_FALSE(c.budgetAuto); // explicit positive cap in the conf
  EXPECT_TRUE(c.recompress);
  EXPECT_EQ(c.recompressCodecs, (std::vector<std::string>{"lzma", "zlib"}));
  EXPECT_EQ(c.sources.at("dir"), "conf");
  EXPECT_EQ(c.sources.count("recompress_min_share"), 0u); // unknown keys get no source
  EXPECT_EQ(c.sources.count("recompress_reclaim"), 0u);
  EXPECT_NE(Config::retiredReason("recompress_reclaim"), nullptr);
  EXPECT_NE(Config::retiredReason("UCACHE_RECOMPRESS_RECLAIM"), nullptr);
  EXPECT_FALSE(Config::stateSettable("recompress_reclaim"));

  // env overrides the plugin conf
  ::setenv("UCACHE_DIR", "/env/cache", 1);
  ::setenv("UCACHE_REVALIDATE_S", "7", 1);
  c = Config::fromEnv(&conf);
  EXPECT_EQ(c.cacheDir, "/env/cache");
  EXPECT_EQ(c.revalidateSeconds, 7);
  EXPECT_EQ(c.sources.at("dir"), "env");
}

// The state file (CLI-written CURRENT values) sits between the conf
// (the user's defaults) and the env (per-job override).
TEST(Config, StateLayerBetweenConfAndEnv) {
  EnvGuard g;
  test::TempDir td;
  const std::string cache = td.path() + "/cache";
  ::mkdir(cache.c_str(), 0755);
  const std::string conf = td.path() + "/ucache.conf";
  std::ofstream(conf) << "url = *\nlib = /x/libXrdClUCache.so\nenable = true\n"
                      << "dir = " << cache << "\nrevalidate_seconds = 1\nrecompress = off\n";
  std::ofstream(Config::statePath(cache)) << "# managed\nrevalidate_seconds = 2\n"
                                          << "recompress = on\n";
  Config c = Config::fromEnv(conf);
  EXPECT_EQ(c.revalidateSeconds, 2); // state wins over conf
  EXPECT_TRUE(c.recompress);
  EXPECT_EQ(c.sources.at("revalidate_seconds"), "state");
  EXPECT_EQ(c.sources.at("recompress"), "state");
  EXPECT_EQ(c.sources.at("dir"), "conf");

  ::setenv("UCACHE_REVALIDATE_S", "3", 1); // env wins over state
  c = Config::fromEnv(conf);
  EXPECT_EQ(c.revalidateSeconds, 3);
  EXPECT_EQ(c.sources.at("revalidate_seconds"), "env");
  EXPECT_TRUE(c.recompress); // untouched state key still applies
}

// `dir` in the state file would be circular (the file lives inside the cache
// dir) — it must be refused, loudly ignored.
TEST(Config, StateCannotRelocateCacheDir) {
  EnvGuard g;
  test::TempDir td;
  const std::string cache = td.path() + "/cache";
  ::mkdir(cache.c_str(), 0755);
  const std::string conf = td.path() + "/ucache.conf";
  std::ofstream(conf) << "dir = " << cache << "\n";
  std::ofstream(Config::statePath(cache)) << "dir = /evil/elsewhere\nthreads = 3\n";
  Config c = Config::fromEnv(conf);
  EXPECT_EQ(c.cacheDir, cache); // unmoved
  EXPECT_EQ(c.threads, 3);      // the rest of the state still applies
}

// Bootstrap order: UCACHE_DIR alone (no conf) must still locate the state
// file, and env stays the highest layer for every other key.
TEST(Config, StateDiscoveredViaEnvDir) {
  EnvGuard g;
  test::TempDir td;
  std::ofstream(Config::statePath(td.path())) << "recompress = on\nthreads = 5\n";
  ::setenv("UCACHE_DIR", td.path().c_str(), 1);
  ::setenv("UCACHE_THREADS", "7", 1);
  Config c = Config::fromEnv();
  EXPECT_EQ(c.cacheDir, td.path());
  EXPECT_TRUE(c.recompress);            // from state, found via env dir
  EXPECT_EQ(c.threads, 7);              // env beats state
  EXPECT_EQ(c.sources.at("recompress"), "state");
  EXPECT_EQ(c.sources.at("threads"), "env");
}

TEST(Config, RejectsBadValues) {
  EnvGuard g;
  ::setenv("UCACHE_PAGE_SIZE", "12345", 1); // not a power of two
  ::setenv("UCACHE_HIGH_WATER", "0.5", 1);
  ::setenv("UCACHE_LOW_WATER", "0.9", 1); // low > high
  Config c = Config::fromEnv();
  EXPECT_EQ(c.pageSize, Config::kDefaultPageSize);
  EXPECT_DOUBLE_EQ(c.highWater, 0.90);
  EXPECT_DOUBLE_EQ(c.lowWater, 0.75);
  ::setenv("UCACHE_PAGE_SIZE", "2048", 1); // below 4 KiB floor
  c = Config::fromEnv();
  EXPECT_EQ(c.pageSize, Config::kDefaultPageSize);
  ::setenv("UCACHE_PAGE_SIZE", "2m", 1); // above 1 MiB ceiling
  c = Config::fromEnv();
  EXPECT_EQ(c.pageSize, Config::kDefaultPageSize);
  ::setenv("UCACHE_VALIDATE", "bogus", 1);
  c = Config::fromEnv();
  EXPECT_EQ(c.validate, ValidateMode::kSize);
}

TEST(Config, ConfFileLayeredUnderEnv) {
  EnvGuard g;
  test::TempDir td;
  std::string path = td.path() + "/ucache.conf";
  {
    std::ofstream f(path);
    f << "# ucache conf\n"
      << "dir = \"/from/file\"\n"
      << "page_size = 16k\n"
      << "max_bytes = 5000000\n"
      << "min_free_bytes = 2000000\n"
      << "validate = size+mtime\n";
  }
  // File values applied; an explicit max_bytes turns budgetAuto OFF.
  Config c = Config::fromEnv(path);
  EXPECT_EQ(c.cacheDir, "/from/file");
  EXPECT_EQ(c.pageSize, 16384u);
  EXPECT_EQ(c.maxBytes, 5000000u);
  EXPECT_EQ(c.minFreeBytes, 2000000u);
  EXPECT_EQ(c.validate, ValidateMode::kSizeMtime);
  EXPECT_FALSE(c.budgetAuto);

  // Env overrides the file for the keys it sets; the rest stay from the file.
  ::setenv("UCACHE_DIR", "/from/env", 1);
  c = Config::fromEnv(path);
  EXPECT_EQ(c.cacheDir, "/from/env"); // env wins
  EXPECT_EQ(c.maxBytes, 5000000u);    // still from file
}

TEST(Config, BudgetAutoWhenNoExplicitCap) {
  EnvGuard g; // no conf, no UCACHE_MAX_BYTES
  Config c = Config::fromEnv();
  EXPECT_EQ(c.maxBytes, 0u);
  EXPECT_TRUE(c.budgetAuto); // floor-governed default
}

TEST(Config, ByteSuffixesZeroAndGarbage) {
  EnvGuard g;
  ::setenv("UCACHE_MAX_BYTES", "2m", 1);
  ::setenv("UCACHE_MIN_FREE_BYTES", "3g", 1);
  Config c = Config::fromEnv();
  EXPECT_EQ(c.maxBytes, 2u << 20);
  EXPECT_EQ(c.minFreeBytes, 3ull << 30);
  EXPECT_FALSE(c.budgetAuto); // positive cap => hard-cap mode
  // Explicit 0 => no byte cap; floor still governs (budgetAuto stays on).
  ::setenv("UCACHE_MAX_BYTES", "0", 1);
  c = Config::fromEnv();
  EXPECT_EQ(c.maxBytes, 0u);
  EXPECT_TRUE(c.budgetAuto);
  // Garbage => ignored; must NOT silently disable the auto floor.
  ::setenv("UCACHE_MAX_BYTES", "notanumber", 1);
  c = Config::fromEnv();
  EXPECT_TRUE(c.budgetAuto);
}

TEST(Config, FileDisableAndHashInsideQuotedValue) {
  EnvGuard g;
  test::TempDir td;
  std::string path = td.path() + "/ucache.conf";
  {
    std::ofstream f(path);
    f << "disable = true\n";
    f << "dir = \"/data/run#42/cache\"\n"; // '#' inside quotes must survive
  }
  Config c = Config::fromEnv(path);
  EXPECT_TRUE(c.disable);
  EXPECT_EQ(c.cacheDir, "/data/run#42/cache");
}

TEST(Config, StateSettableVocabulary) {
  EXPECT_TRUE(Config::stateSettable("recompress"));
  EXPECT_TRUE(Config::stateSettable("revalidate_seconds"));
  EXPECT_FALSE(Config::stateSettable("dir"));           // lives outside the state file
  EXPECT_FALSE(Config::stateSettable("no_such_key"));   // typo protection
  EXPECT_FALSE(Config::stateSettable("recompress_min_share")); // retired key
}

namespace {
std::string slurp(const std::string& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
size_t countOf(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1))
    ++n;
  return n;
}
} // namespace

// The two settings the window replaced are named, with the new one, wherever
// they are still set -- and never applied.
TEST(Config, TheOldWindowKeysAreNamed) {
  EnvGuard g;
  test::TempDir td;
  const std::string log = td.path() + "/log.txt";
  Log::configure("warn:" + log);
  std::map<std::string, std::string> conf = {{"evict_protect_seconds", "60"},
                                             {"map_expiry_seconds", "60"}};
  ::setenv("UCACHE_EVICT_PROTECT_S", "60", 1);
  ::setenv("UCACHE_MAP_EXPIRY_S", "60", 1);
  Config c = Config::fromEnv(&conf);
  Log::configure("warn");
  const std::string all = slurp(log);
  for (const char* k : {"evict_protect_seconds", "map_expiry_seconds"}) {
    EXPECT_NE(all.find(std::string("'") + k + "' is no longer a setting"), std::string::npos) << all;
    EXPECT_NE(Config::retiredReason(k), nullptr);
    EXPECT_EQ(c.sources.count(k), 0u);
    for (const auto& known : Config::knownKeys())
      EXPECT_STRNE(known.key, k);
  }
  EXPECT_EQ(countOf(all, "UCACHE_EVICT_PROTECT_S is no longer used"), 1u);
  EXPECT_EQ(countOf(all, "UCACHE_MAP_EXPIRY_S is no longer used"), 1u);
  EXPECT_NE(all.find("in_use_seconds"), std::string::npos); // the reason names the new one
  EXPECT_EQ(c.inUseSeconds, 86400u);                        // neither was applied
}

// A setting that no longer exists is named, with its reason, wherever it is
// still set — never reported as a typo, never recorded as a source, never
// offered by `ucache set`.
TEST(Config, RetiredDrainJobsKeyIsNamed) {
  EnvGuard g;
  test::TempDir td;
  const std::string log = td.path() + "/log.txt";
  Log::configure("warn:" + log);
  const std::string cache = td.path() + "/cache";
  ::mkdir(cache.c_str(), 0755);
  std::ofstream(Config::statePath(cache)) << "recompress_drain_jobs = 4\n";
  std::map<std::string, std::string> conf = {{"dir", cache}, {"recompress_drain_jobs", "4"}};
  ::setenv("UCACHE_RECOMPRESS_DRAIN_JOBS", "4", 1);
  Config c = Config::fromEnv(&conf);
  Log::configure("warn");
  const std::string all = slurp(log);
  EXPECT_NE(all.find("ucache.conf: 'recompress_drain_jobs' is no longer a setting"),
            std::string::npos)
      << all;
  EXPECT_NE(all.find("state: 'recompress_drain_jobs' is no longer a setting"), std::string::npos);
  EXPECT_NE(all.find("`ucache unset recompress_drain_jobs` removes it"), std::string::npos);
  EXPECT_NE(all.find("background recompression worker"), std::string::npos); // the reason
  EXPECT_EQ(countOf(all, "UCACHE_RECOMPRESS_DRAIN_JOBS is no longer used"), 1u);
  EXPECT_EQ(all.find("unknown key"), std::string::npos); // named, not a typo
  EXPECT_EQ(c.sources.count("recompress_drain_jobs"), 0u);
  EXPECT_FALSE(Config::stateSettable("recompress_drain_jobs"));
  for (const auto& k : Config::knownKeys())
    EXPECT_STRNE(k.key, "recompress_drain_jobs");
  EXPECT_NE(Config::retiredReason("recompress_drain_jobs"), nullptr);
  EXPECT_EQ(Config::retiredReason("recompress"), nullptr); // a live key is not retired
}

// The helper override has no key at all: only the environment can carry it, so
// the environment layer names it — once per process, however often the
// settings are loaded.
TEST(Config, RetiredHelperEnvIsNamedOnce) {
  EnvGuard g;
  test::TempDir td;
  const std::string log = td.path() + "/log.txt";
  Log::configure("warn:" + log);
  ::setenv("UCACHE_RECOMPRESS_HELPER", "/x/ucache", 1);
  Config::fromEnv();
  Config::fromEnv();
  Log::configure("warn");
  const std::string all = slurp(log);
  EXPECT_EQ(countOf(all, "UCACHE_RECOMPRESS_HELPER is no longer used"), 1u) << all;
  EXPECT_NE(all.find("background recompression worker"), std::string::npos);
  EXPECT_NE(Config::retiredReason("UCACHE_RECOMPRESS_HELPER"), nullptr);
}

TEST(Config, ValidPageSize) {
  EXPECT_TRUE(Config::validPageSize(4096));
  EXPECT_TRUE(Config::validPageSize(1u << 20));
  EXPECT_FALSE(Config::validPageSize(0));
  EXPECT_FALSE(Config::validPageSize(4095));
  EXPECT_FALSE(Config::validPageSize(2048));
  EXPECT_FALSE(Config::validPageSize(2u << 20));
}
