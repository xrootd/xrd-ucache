#ifdef UCACHE_HAVE_COLDRUN
#include "ColdRun.h"
#endif
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
// Plugin factory + entry point. XrdCl resolves XrdClGetPlugIn
// from the library named in the plugin conf and calls it with the conf's
// key/value map; the returned factory produces per-file plugins.
// CreateFileSystem returns nullptr: XrdCl logs and continues without a
// plugin for filesystem objects (verified in XrdClFileSystem.cc) — all FS
// ops are naturally pass-through, and a broken cache can never break a job.
#include "Announce.h"
#include "CopyDetect.h"
#include "CpuCounters.h"
#include "EntryNames.h"
#include "Executor.h"
#include "PluginSupport.h"
#include "Log.h"
#include "UCacheFile.h"
#ifdef UCACHE_HAVE_PREFETCH
#include "Prefetch.h"
#endif
#ifdef UCACHE_HAVE_COLDRUN
#include "ReadRule.h"
#endif

#include <XrdCl/XrdClConstants.hh>
#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClLog.hh>
#include <XrdVersion.hh>

#include <atomic>
#include <dlfcn.h>
#include <mutex>
#include <pthread.h>
#include <thread>
#if defined(__APPLE__)
#include <sys/mount.h> // statfs, and f_fstypename instead of a magic number
#include <sys/param.h>
#else
#include <sys/vfs.h>
#endif
#include <unistd.h>

namespace ucache {

namespace {
std::once_flag gInitFlag;
Config* gConfig = nullptr;
std::shared_ptr<CacheStore>* gStore = nullptr;
std::atomic<int> gRequestTimeoutS{XrdCl::DefaultRequestTimeout};
std::atomic<bool> gExiting{false}; // the process has begun to exit (processExiting)

// ---- the XRootD client's log ----------------------------------------------
// uCache is part of the client a job runs, so it logs where the client does:
// XRD_LOGLEVEL=Debug shows uCache's debug lines too, and XRD_LOGFILE takes
// them beside the client's own, under this topic. Far above the client's own
// topics (5.x and 6.x end below 0x10000).
constexpr uint64_t kClientLogTopic = 0x0000000100000000ULL;

XrdCl::Log::LogLevel clientLevelOf(LogLevel l) {
  switch (l) {
  case LogLevel::kError:
    return XrdCl::Log::ErrorMsg;
  case LogLevel::kWarn:
    return XrdCl::Log::WarningMsg;
  case LogLevel::kInfo:
    return XrdCl::Log::InfoMsg;
  default:
    return XrdCl::Log::DebugMsg;
  }
}

// A message the client's level admits goes through the client's log.
bool toClientLog(LogLevel lvl, const char* msg) {
  XrdCl::Log* xl = XrdCl::DefaultEnv::GetLog();
  if (!xl || xl->GetLevel() < clientLevelOf(lvl))
    return false;
  switch (lvl) {
  case LogLevel::kError:
    xl->Error(kClientLogTopic, "%s", msg);
    break;
  case LogLevel::kWarn:
    xl->Warning(kClientLogTopic, "%s", msg);
    break;
  case LogLevel::kInfo:
    xl->Info(kClientLogTopic, "%s", msg);
    break;
  default:
    xl->Debug(kClientLogTopic, "%s", msg);
  }
  return true;
}

void followClientLog() {
  XrdCl::Log* xl = XrdCl::DefaultEnv::GetLog();
  if (!xl)
    return;
  xl->SetTopicName(kClientLogTopic, "UCache");
  const auto cl = xl->GetLevel();
  if (cl >= XrdCl::Log::DebugMsg)
    Log::raiseLevel(LogLevel::kDebug);
  else if (cl >= XrdCl::Log::InfoMsg)
    Log::raiseLevel(LogLevel::kInfo);
  Log::setSink(&toClientLog);
}

std::string fsNameOf(const std::string& dir) {
  struct ::statfs sf{};
  if (dir.empty() || ::statfs(dir.c_str(), &sf) != 0)
    return "?";
#if defined(__APPLE__)
  // Darwin names the filesystem outright, so no magic-number table is needed.
  return sf.f_fstypename[0] ? std::string(sf.f_fstypename) : std::string("?");
#else
  switch (static_cast<unsigned long>(sf.f_type)) {
  case 0x58465342: return "xfs";
  case 0xEF53: return "ext4";
  case 0x9123683E: return "btrfs";
  case 0x01021994: return "tmpfs";
  case 0x6969: return "nfs";
  case 0x65735546: return "fuse";
  case 0x00C36400: return "ceph";
  case 0x5346414F: return "afs";
  case 0x0BD00BD0: return "lustre";
  case 0x47504653: return "gpfs";
  case 0x2FC12FC1: return "zfs";
  default: return "other";
  }
#endif
}

// Printed once, before a process's first warning: what a report from the
// field needs and a warning line alone does not say.
std::string runContext() {
  char line[600];
  const Config* c = gConfig;
  ::snprintf(line, sizeof line,
             "uCache %s in %s (pid %d, %llu threads), cache %s on %s, recompress %s, "
             "prefetch %s",
             UCACHE_VERSION, hostExecutable().c_str(), static_cast<int>(::getpid()),
             static_cast<unsigned long long>(CpuCounters::liveThreads()),
             c && !c->cacheDir.empty() ? c->cacheDir.c_str() : "(none)",
             c ? fsNameOf(c->cacheDir).c_str() : "?", c && c->recompress ? "on" : "off",
             c && c->prefetch ? "on" : "off");
  return line;
}
// The plugin conf's key/value map: XrdCl hands it to XrdClGetPlugIn,
// but only for the duration of the call — copied here (and leaked, like the
// other globals) before the config is first built. First conf wins if two
// conf files name this library.
std::map<std::string, std::string>* gPluginConf = nullptr;

// Keep this library mapped for the life of the process. XrdCl's plugin manager
// dlcloses plugin libraries at teardown, while our detached executor threads and
// the atexit stats dump still execute code from this image — unmapping it is an
// exit-time crash, not a leak. Where the link step could mark the library
// non-unloadable it already has (UCACHE_LINK_NODELETE) and there is nothing to
// do here; otherwise the loader is asked for the same guarantee by re-opening
// this image with RTLD_NODELETE and never closing the handle.
void pinSelfInMemory() {
#ifndef UCACHE_LINK_NODELETE
  ::Dl_info info{};
  // dladdr reports success as NON-zero, unlike most of what surrounds it.
  if (::dladdr(reinterpret_cast<const void*>(&pinSelfInMemory), &info) == 0 || !info.dli_fname) {
    UCACHE_WARN("could not identify this plugin's own library file; it stays unloadable only "
                "if the loader chooses to keep it");
    return;
  }
  // Leaked deliberately: closing this handle is exactly what it exists to
  // prevent. One extra reference on an already-loaded image costs nothing.
  if (::dlopen(info.dli_fname, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE) == nullptr)
    UCACHE_WARN("could not pin %s in memory (%s); a plugin unload during teardown could crash "
                "at exit", info.dli_fname, ::dlerror());
#endif
}

// Tell servers that uCache, not the host program, is the one asking.
//
// The two strings travel in the login request that opens a session, so this
// has to happen before the first connection -- it does: the client loads its
// plugins from inside its own environment setup, which is where this runs, and
// the strings it reads at login are the ones left here. They are also the only
// use the client makes of them, so nothing about the requests uCache issues
// changes.
//
// Silent when uCache is not actually in the data path. A disabled cache, or
// one with nowhere to store anything, relays the program's own reads
// untouched, and claiming those would misreport them -- a baseline run must
// look like what it is. So does a copy tool: every read it makes is a copy,
// and copies go straight to the origin (CopyDetect.h).
void announceIdentity(const Config& cfg) {
  if (!cfg.announce) {
    UCACHE_DEBUG("announce = off: leaving the application name as the host program's");
    return;
  }
  if (cfg.disable || cfg.cacheDir.empty()) {
    UCACHE_DEBUG("not announcing: uCache is passing through (%s), so the reads a server "
                 "sees are the program's own",
                 cfg.disable ? "disabled" : "no cache dir");
    return;
  }
  if (cfg.copyDetect && copyProgramSignal() != CopySignal::kNone) {
    UCACHE_DEBUG("not announcing: %s only copies, merges or inspects, and those read straight "
                 "from the origin, so the reads a server sees are the program's own",
                 hostScript().empty() ? hostExecutable().c_str() : hostScript().c_str());
    return;
  }
  XrdCl::Env* env = XrdCl::DefaultEnv::GetEnv();
  if (!env) { // never seen: the client builds its environment before loading us
    UCACHE_DEBUG("not announcing: no client environment yet");
    return;
  }
  std::string hostApp;
  env->GetString("AppName", hostApp); // the host program, or the user's own name
  const Announcement a = buildAnnouncement(hostApp, UCACHE_VERSION);
  // A name exported in the environment is the user's decision and is kept:
  // these writes are declined in that case, which is why they are checked
  // rather than assumed.
  const bool named = env->PutString("AppName", a.appName);
  const bool informed = env->PutString("MonInfo", a.monInfo);
  if (named && informed)
    UCACHE_INFO("servers will see this session as '%s' (%s)", a.appName.c_str(),
                a.monInfo.c_str());
  else
    UCACHE_INFO("announcing partially: application name %s, information string %s "
                "(a name set in the environment wins over uCache's)",
                named ? "= 'ucache'" : "kept from the environment",
                informed ? "= 'ucache/...'" : "kept from the environment");
}

// The run line's `run_settings` (docs/STATS.md): the settings that decide how
// the origin is reached, with where each value came from. The XRootD client's
// connection and timeout settings are read as the plugin finds them at load;
// an application that changes one later is not seen. Its source is "env" when
// the XRD_<NAME> variable is set, "default" when it is the client's built-in
// value, and "client" otherwise (a client.conf, or the application). uCache
// changes none of them.
std::string runSettings(const Config& cfg) {
  struct Knob {
    const char* name;
    int dflt;
  };
  const Knob knobs[] = {{"ConnectionWindow", XrdCl::DefaultConnectionWindow},
                        {"ConnectionRetry", XrdCl::DefaultConnectionRetry},
                        {"StreamErrorWindow", XrdCl::DefaultStreamErrorWindow},
                        {"RequestTimeout", XrdCl::DefaultRequestTimeout},
                        {"StreamTimeout", XrdCl::DefaultStreamTimeout},
                        {"TimeoutResolution", XrdCl::DefaultTimeoutResolution}};
  std::string out = "v1 check_origin_integrity=";
  out += cfg.checkOriginIntegrity ? "on" : "off";
  XrdCl::Env* env = XrdCl::DefaultEnv::GetEnv();
  for (const Knob& k : knobs) {
    int v = k.dflt;
    if (env)
      env->GetInt(k.name, v);
    std::string var = "XRD_";
    for (const char* c = k.name; *c; ++c)
      var += static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    const char* src = ::getenv(var.c_str()) ? "env" : v == k.dflt ? "default" : "client";
    out += std::string(" xrdcl.") + k.name + "=" + std::to_string(v) + "/" + src;
    if (std::strcmp(k.name, "RequestTimeout") == 0)
      gRequestTimeoutS.store(v, std::memory_order_relaxed);
  }
  out += cfg.failover ? " failover=on" : " failover=off";
  // With logical_name_prefixes set: files are keyed by logical name, and a hash of the prefixes that
  // name them (two runs keyed by different prefixes key one file two ways). No token when unset.
  if (cfg.keys.logical())
    out += " logical_names=on/" + cfg.keys.prefixesHash();
  return out;
}

// Re-armed from itself every meta_flush_seconds. A process that _exit()s -- a
// multiprocessing worker, which is how Python's process pools end -- runs
// neither destructors nor the atexit dump below, and used to leave the pages
// it had staged and its whole run record behind in RAM. Captures nothing but
// the leaked globals, so it is safe for as long as the process lives.
void scheduleCheckpoint() {
  const uint64_t periodMs = static_cast<uint64_t>(gConfig->metaFlushSeconds) * 1000;
  Executor::instance().postAfter(periodMs, [] {
    if (gStore && *gStore)
      (*gStore)->checkpoint();
#ifdef UCACHE_HAVE_COLDRUN
    coldCheckpoint(); // slot records still in memory go to their stores
#endif
    scheduleCheckpoint();
  }, /*housekeeping=*/true); // nobody waits for it: never run on a caller (Executor.h)
}

// fork(). A child has only the thread that forked: every pool, its queue and
// whatever the parent's threads were doing stays behind. The child starts
// over -- each pool and registry rebuilt on its first use there, the store's
// entries and counters its own, every inherited handle set up again at its
// next use (HandleState::syncFork) -- and keeps one thing, the layouts it has
// shown each file in, because a reader in the child may hold offsets it
// learned before the fork. Nothing here does I/O or starts a thread: a child
// that only execs pays nothing. NO lock is held across fork: XrdCl's prepare
// handler runs after ours and waits for its callback threads, and one of
// those may be in plugin code that needs any lock we held.
std::atomic<bool> gResume{false};

void forkPrepare() {
  // The child's handler reaches these through function-local statics: any
  // first use another thread has under way is finished before the fork.
  (void)widthSampler();
#ifdef UCACHE_HAVE_COLDRUN
  coldForkPrepare(); // copies the shown layouts, lock released on return
  readRuleBeforeFork();
#endif
}

void forkParent() {
#ifdef UCACHE_HAVE_COLDRUN
  coldForkParent();
#endif
}

void forkChild() {
  Executor::afterForkChild();
  if (gStore && *gStore)
    (*gStore)->afterForkChild();
  FileEntry::afterForkChild();
  widthSampler().afterForkChild();
#ifdef UCACHE_HAVE_COLDRUN
  coldAfterForkChild();
  readRuleAfterForkChild();
#endif
#ifdef UCACHE_HAVE_PREFETCH
  Prefetcher::afterForkChild();
#endif
  gResume.store(true, std::memory_order_release);
}

void initGlobals() {
  followClientLog(); // before anything here can log
  Log::setContext(&runContext);
  pinSelfInMemory(); // before any thread exists that could outlive an unload
  // Before any plugin thread exists, so that no fork can find one unprepared.
  ::pthread_atfork(forkPrepare, forkParent, forkChild);
  locateInit();
  setEntryNamesWritable(true); // the plug-in gives names to the entries it opens (EntryNames.h)
  // Leaked intentionally: destruction order against XrdCl teardown and the
  // executor threads is unknowable; the final stats dump happens via atexit.
  gConfig = new Config(Config::fromEnv(gPluginConf));
  // No cache dir configured — deliberately no default. Null store =>
  // every handle is passthroughOnly_: the job runs correctly, just uncached.
  if (gConfig->cacheDir.empty() && !gConfig->disable)
    UCACHE_WARN("no cache dir configured — set `dir =` in ucache.conf (USER_GUIDE §2) "
                "or UCACHE_DIR; running uncached (pass-through)");
  // Copy detection needs the libXrdCl this plugin is bound to -- only that
  // library can route an open here -- and any function in it names it.
  if (gConfig->copyDetect)
    copyDetectInit(reinterpret_cast<const void*>(&XrdCl::DefaultEnv::GetEnv));
  announceIdentity(*gConfig);
  gStore = new std::shared_ptr<CacheStore>(
      gConfig->cacheDir.empty()
          ? nullptr
          : std::make_shared<CacheStore>(RealIO::instance(), *gConfig));
  {
    const std::string settings = runSettings(*gConfig); // before any thread can dump stats
    if (*gStore)
      (*gStore)->setRunSettings(settings);
  }
  Executor::instance(static_cast<unsigned>(
      gConfig->threads > 0 ? gConfig->threads : 0));
  if (*gStore) {
    Executor::instance().setStats(&(*gStore)->stats());
    Executor::setFailureStats(&(*gStore)->stats()); // threads that could not start, from now on
  }
  {
    // A delayed task run on the calling thread (no pool thread anywhere) never sleeps on a client
    // callback thread: the client's few workers deliver every answer.
    Executor::setMayBlockProbe([] { return !onClientCallback(); });
  }
  if (*gStore && gConfig->metaFlushSeconds > 0)
    scheduleCheckpoint();
  // With logical names set, the entries cached before get their names once per cache and list of
  // prefixes, in the background (an open meanwhile names its own URL's entry as it goes), on a
  // thread of its own: the scan reads every sidecar of the cache and may take minutes, and on the
  // executor it would hold up every hit, page write and completion queued behind it. Detached; it
  // touches only the cache's files and objects that are never destroyed, and stops at the
  // process's end (processExiting) without its marker. A forked child does not inherit it: the
  // naming is left to the next process (the parent's goes on; naming.lock keeps it one at a time).
  if (*gStore && gConfig->keys.logical()) {
    const std::string dir = gConfig->cacheDir;
    const KeyRules rules = gConfig->keys;
    try {
      std::thread([dir, rules] {
        Executor::nameThisThread("uc-names");
        (void)nameCachedEntries(dir, rules, [] { return processExiting(); });
      }).detach();
    } catch (...) {
      // no thread: the entries are named as jobs open them (resolveEntry's case 3)
    }
  }
  ::atexit([] {
    gExiting.store(true, std::memory_order_release); // first: no open is issued from here on
    waitForSourceWrites(1000); // a mark made just before the end is in the last stats
    if (gStore && *gStore)
      (*gStore)->dumpStats(/*finalDump=*/true); // + Layer-2 records of live entries
    Log::flushHeldBack();
  });
  UCACHE_INFO("xrd-ucache plugin initialized (dir=%s page=%u disable=%d)",
              gConfig->cacheDir.c_str(), gConfig->pageSize, gConfig->disable ? 1 : 0);
#ifndef UCACHE_HAVE_COLDRUN
  if (gConfig->checkOriginIntegrity || gConfig->failover)
    UCACHE_INFO("check_origin_integrity is on%s, but this build has no codec libraries: nothing "
                "fetched is checked, and what is fetched is kept as without the setting",
                gConfig->checkOriginIntegrity ? ""
                                              : " (forced by failover, for the files from a "
                                                "redirector)");
  if (gConfig->maxReadFraction == 0)
    UCACHE_WARN("max_read_fraction = 0, but this build has no codec libraries: it cannot read a "
                "file's records, so no file is read straight from the origin and every file's "
                "data is cached");
#endif
}
} // namespace

const Config& globalConfig() {
  std::call_once(gInitFlag, initGlobals);
  return *gConfig;
}

std::shared_ptr<CacheStore> globalStore() {
  std::call_once(gInitFlag, initGlobals);
  return *gStore;
}

int defaultRequestTimeoutS() { return gRequestTimeoutS.load(std::memory_order_relaxed); }

int currentRequestTimeoutS() {
  // At the process's end the client's environment is deleted (DefaultEnv::Finalize), maybe between
  // a load of it and its use here: the value read at load is taken instead.
  if (gExiting.load(std::memory_order_acquire))
    return defaultRequestTimeoutS();
  int v = XrdCl::DefaultRequestTimeout;
  if (XrdCl::Env* env = XrdCl::DefaultEnv::GetEnv())
    env->GetInt("RequestTimeout", v);
  return v;
}

bool processExiting() { return gExiting.load(std::memory_order_acquire); }

void resumeAfterFork() {
  if (!gResume.load(std::memory_order_acquire) || !gResume.exchange(false))
    return;
  // The checkpoint re-armed itself on the parent's timer, which is not here.
  if (gStore && *gStore && gConfig->metaFlushSeconds > 0)
    scheduleCheckpoint();
}

class UCacheFactory : public XrdCl::PlugInFactory {
 public:
  XrdCl::FilePlugIn* CreateFile(const std::string& /*url*/) override { return new UCacheFile(); }
  XrdCl::FileSystemPlugIn* CreateFileSystem(const std::string& /*url*/) override {
    return nullptr; // default FileSystem; see header comment
  }
};

} // namespace ucache

XrdVERSIONINFO(XrdClGetPlugIn, XrdClUCache)

extern "C" {
void* XrdClGetPlugIn(const void* config) {
  // `config` is the conf file's key/value map (std::map<std::string,
  // std::string>*, verified in 5.8.3 PlugInManager::LoadFactory) — valid only
  // during this call. ucache settings may live right in that file (one
  // config file); copy the map before the first globalConfig() build.
  if (config && !ucache::gPluginConf)
    ucache::gPluginConf = new std::map<std::string, std::string>(
        *static_cast<const std::map<std::string, std::string>*>(config));
  ucache::globalConfig(); // initialize early, on the loader's thread
  // Heap-allocated: XrdCl::PlugInManager takes ownership and deletes the
  // factory at teardown (FactoryHelper dtor) — a static here crashes exit.
  return new ucache::UCacheFactory();
}
}
