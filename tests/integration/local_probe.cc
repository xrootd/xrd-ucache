// Local files: for a URL whose host is `local`, root://local//abs/path, the
// plug-in reads the file /abs/path of this machine's file system, caches it,
// and checks the cached entry against the file at every open.
//
//   local_probe <plugin library>
//
// Self-contained, with no server: the probe makes a directory of its own
// (under TMPDIR, else /tmp) holding a plug-in conf that binds the host `local`,
// an empty cache and the data files, and points the client at them. Each phase
// runs in a process of its own, as separate jobs would: the probe runs itself
// again with that environment (the client reads its environment when its
// library loads, before main), and the phase ends with a normal exit, which
// is when a job's cache writes are complete. The parent never uses the client
// and changes the files between phases:
//   A. cold: plain and vector reads through root://local are the file's bytes;
//      LastURL is file://localhost/<path> and DataServer ""; on the host a stat
//      (with and without a query on the path), a stat of a missing path ("not
//      found"), and a listing (plain, and with the locate ROOT asks for) answer
//      from the file system: every entry with its stat (a link to nothing too)
//      and the host itself as its host; a directory that may be read but not
//      searched fails its listing.
//   B. rewritten in place, other bytes of the same size, its time put back:
//      the reads give the FIRST bytes, which only the cache holds.
//   C. a new modification time: the new bytes.
//   D. a new size (other bytes from the first): the new bytes.
//   E. in one process: a handle reads and stays open, the file is rewritten at
//      its size with a later time, a second handle reads the new bytes.
//   F. a new size with the modification time kept: the new bytes.
//   G. disabled (UCACHE_DISABLE=1): the file is still read.
//   H. a handle open while the file is truncated: a vector read past the new end
//      fails, as a server's answer does, and I (the file restored, its time put
//      back) reads the file's bytes there: nothing the read did not return was
//      kept.
//   O. a process forks right after reading uncached ranges (the client's own
//      local single read is POSIX AIO, whose idle helper threads a child
//      inherits as present): the child's read of an uncached range through the
//      handle it inherited completes, within a watchdog.
//   J. a missing file: the open fails, and an asynchronous open's answer carries
//      a host list (an empty one: never none).
//   K. the file removed after it was cached: the open fails.
//   L. a directory: the open is refused.
//   M. logical names (/store/), two local files of one name, of one size and
//      other bytes and times: (1) the first is read; (2) the second is served
//      the first's entry (one entry by name); (3) names off: the second reads
//      its own bytes under its own URL; (4) names on again: one entry again.
//   N. the negative control: a URL the conf does not bind (another port) goes
//      to the network, and the log check sees it.
// In every phase but N the client's own log (Debug level, captured in the
// child) must hold the local-file access's own lines (the log was captured)
// and no connection, address lookup or channel: nothing is ever sent to a
// server. The trust window and the freshness check keep their defaults; every
// UCACHE_* and XRD_* variable of the caller's environment is cleared.
//
// Exit 0: every check passed. 1: a check failed (each printed as "FAIL: ...";
// the directory is kept for inspection). 3: setup error.
#include <XProtocol/XProtocol.hh>
#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClFileSystem.hh>
#include <XrdCl/XrdClLog.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

// Every line the client logs. Leaked on purpose: the client deletes its log
// output at exit, and its threads may still be logging then.
struct Lines {
  std::mutex mu;
  std::vector<std::string> all;
};
Lines& lines() {
  static Lines* l = new Lines;
  return *l;
}

class Capture : public XrdCl::LogOut {
 public:
  void Write(const std::string& message) override {
    std::lock_guard<std::mutex> g(lines().mu);
    lines().all.push_back(message);
  }
};

int gFails = 0;
void check(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok)
    ++gFails;
}

// The client logged a line holding `text`.
bool logged(const char* text) {
  std::lock_guard<std::mutex> g(lines().mu);
  for (const auto& l : lines().all)
    if (l.find(text) != std::string::npos)
      return true;
  return false;
}

// The first line of the client's log that shows a request on its way to a
// server: a connection, an address lookup or a channel. "" for none.
std::string networkLine() {
  static const char* const kSigns[] = {"AsyncSock", "Creating new channel", "Unable to resolve",
                                       "[local:"};
  std::lock_guard<std::mutex> g(lines().mu);
  for (const auto& l : lines().all)
    for (const char* s : kSigns)
      if (l.find(s) != std::string::npos)
        return l;
  return "";
}

// Deterministic contents: `seed` tells two versions of a file apart at every byte.
std::string contents(size_t n, unsigned seed) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i)
    s[i] = static_cast<char>(((i + seed) * 2654435761u >> 13) ^ seed);
  return s;
}

const size_t kSize = (3u << 20) + 4321;

// The files' contents by version: 1 and 2 of one size; 3 and 4 of another (3
// differs from 2 from its first byte); 5 of a third; 11 and 12 the two files
// of one logical name (phase M), of one size.
std::string version(int v) {
  switch (v) {
  case 3:
  case 4:
    return contents(kSize + 4096, static_cast<unsigned>(v));
  case 5:
    return contents(kSize + 8192, 5);
  default:
    return contents(kSize, static_cast<unsigned>(v));
  }
}

bool writeFile(const std::string& path, const std::string& data, int flags) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | flags, 0644);
  if (fd < 0)
    return false;
  size_t done = 0;
  while (done < data.size()) {
    const ssize_t w = ::write(fd, data.data() + done, data.size() - done);
    if (w <= 0)
      break;
    done += static_cast<size_t>(w);
  }
  return ::close(fd) == 0 && done == data.size();
}

struct ::timespec mtimeOf(const struct ::stat& sb) {
#if defined(__APPLE__)
  return sb.st_mtimespec;
#else
  return sb.st_mtim;
#endif
}

bool mtimeOfPath(const std::string& path, struct ::timespec& out) {
  struct ::stat sb{};
  if (::stat(path.c_str(), &sb) != 0)
    return false;
  out = mtimeOf(sb);
  return true;
}

bool setMtime(const std::string& path, const struct ::timespec& mtime) {
  struct ::timespec ts[2];
  ts[0].tv_sec = 0;
  ts[0].tv_nsec = UTIME_OMIT; // the access time is left as it is
  ts[1] = mtime;
  return ::utimensat(AT_FDCWD, path.c_str(), ts, 0) == 0;
}

bool cacheHasEntry(const std::string& cache) {
  DIR* top = ::opendir((cache + "/objects").c_str());
  if (!top)
    return false;
  bool found = false;
  while (const dirent* e = ::readdir(top)) {
    if (e->d_name[0] == '.')
      continue;
    if (DIR* sub = ::opendir((cache + "/objects/" + e->d_name).c_str())) {
      while (const dirent* f = ::readdir(sub))
        if (std::strstr(f->d_name, ".meta"))
          found = true;
      ::closedir(sub);
    }
  }
  ::closedir(top);
  return found;
}

int removeOne(const char* path, const struct ::stat*, int, struct FTW*) { return ::remove(path); }

std::string dirOf(const std::string& path) {
  return path.substr(0, path.rfind('/'));
}

struct Range {
  uint64_t off;
  uint32_t len;
};

// The requests on the host of phase A: stats, listings.
void fileSystemJob(const std::string& path, const std::string& want, const std::string& phase) {
  XrdCl::FileSystem fs(XrdCl::URL("root://local:1094"));
  XrdCl::StatInfo* fsi = nullptr;
  const XrdCl::XRootDStatus fss = fs.Stat(path, fsi);
  check(fss.IsOK() && fsi && fsi->GetSize() == want.size() &&
            fsi->TestFlags(XrdCl::StatInfo::IsReadable) && !fsi->TestFlags(XrdCl::StatInfo::IsDir),
        phase + ": a stat on the host gives the file's size" +
            (fss.IsOK() ? "" : ": " + fss.ToStr()));
  delete fsi;
  XrdCl::StatInfo* qsi = nullptr;
  const XrdCl::XRootDStatus qs = fs.Stat(path + "?xrd.wantprot=unix", qsi);
  check(qs.IsOK() && qsi && qsi->GetSize() == want.size(),
        phase + ": a stat with a query on the path names the file" +
            (qs.IsOK() ? "" : ": " + qs.ToStr()));
  delete qsi;
  XrdCl::StatInfo* none = nullptr;
  const XrdCl::XRootDStatus ns = fs.Stat(path + ".missing", none);
  check(!ns.IsOK() && ns.code == XrdCl::errErrorResponse &&
            ns.errNo == static_cast<uint32_t>(kXR_NotFound),
        phase + ": a stat of a missing path says not found (" + ns.ToStr() + ")");
  delete none;
  const std::string dir = dirOf(path);
  const std::string name = path.substr(path.rfind('/') + 1);
  // Plainly, and as ROOT lists a directory (for a wildcard in a TChain): with a
  // locate first, which the client turns into a listing where it was found.
  for (const auto flags :
       {XrdCl::DirListFlags::Stat, XrdCl::DirListFlags::Stat | XrdCl::DirListFlags::Locate}) {
    XrdCl::DirectoryList* list = nullptr;
    const XrdCl::XRootDStatus ls = fs.DirList(dir, flags, list);
    bool listed = false, link = false, allStat = ls.IsOK() && list, ownHost = allStat;
    if (ls.IsOK() && list)
      for (auto it = list->Begin(); it != list->End(); ++it) {
        const XrdCl::StatInfo* si = (*it)->GetStatInfo();
        allStat = allStat && si;
        ownHost = ownHost && (*it)->GetHostAddress() == "local:1094";
        if ((*it)->GetName() == name && si && si->GetSize() == want.size())
          listed = true;
        if ((*it)->GetName() == "dangling")
          link = true;
      }
    const std::string how = flags & XrdCl::DirListFlags::Locate ? " (with a locate)" : "";
    check(listed, phase + ": a listing" + how + " holds the file, with its size" +
                      (ls.IsOK() ? "" : ": " + ls.ToStr()));
    check(link && allStat,
          phase + ": a listing" + how + " gives every entry its stat, a link to nothing included");
    check(ownHost,
          phase + ": a listing" + how + " names the host itself (local:1094) for each entry");
    delete list;
  }
  if (::geteuid() != 0) { // root searches any directory
    XrdCl::DirectoryList* list = nullptr;
    const XrdCl::XRootDStatus ls =
        fs.DirList(dirOf(dir) + "/nosearch", XrdCl::DirListFlags::Stat, list);
    check(!ls.IsOK(), phase +
                          ": a directory that may be read but not searched fails its listing (" +
                          ls.ToStr() + ")");
    delete list;
  }
}

// One job: open the URL, read the ranges (the first ones plainly, the rest in
// one vector read), compare them with `want`, close.
void readJob(const std::string& url, const std::string& path, const std::string& want,
             const std::string& phase, bool fileSystem) {
  XrdCl::File f;
  const XrdCl::XRootDStatus os = f.Open(url, XrdCl::OpenFlags::Read);
  check(os.IsOK(), phase + ": open of " + url + (os.IsOK() ? "" : ": " + os.ToStr()));
  if (!os.IsOK())
    return;
  XrdCl::StatInfo* si = nullptr;
  const XrdCl::XRootDStatus ss = f.Stat(false, si);
  check(ss.IsOK() && si && si->GetSize() == want.size(),
        phase + ": the open's size is the file's (" + std::to_string(want.size()) + ")");
  delete si;

  const std::vector<Range> plain = {{0, 256 * 1024}, {256 * 1024, 256 * 1024}, {1 << 20, 4000}};
  bool same = true;
  for (const Range& r : plain) {
    std::vector<char> buf(r.len);
    uint32_t got = 0;
    const XrdCl::XRootDStatus rs = f.Read(r.off, r.len, buf.data(), got);
    same = same && rs.IsOK() && got == r.len &&
           std::memcmp(buf.data(), want.data() + r.off, r.len) == 0;
  }
  check(same, phase + ": plain reads give the expected bytes");

  const uint64_t size = want.size();
  const std::vector<Range> vec = {{(3u << 19) + 17, 65536}, {2u << 20, 4096}, {size - 1000, 1000}};
  std::vector<std::vector<char>> bufs;
  bufs.reserve(vec.size());
  XrdCl::ChunkList chunks;
  for (const Range& r : vec) {
    bufs.emplace_back(r.len);
    chunks.emplace_back(r.off, r.len, bufs.back().data());
  }
  XrdCl::VectorReadInfo* vi = nullptr;
  const XrdCl::XRootDStatus vs = f.VectorRead(chunks, nullptr, vi);
  same = vs.IsOK() && vi && vi->GetSize() == 65536 + 4096 + 1000;
  for (size_t i = 0; same && i < vec.size(); ++i)
    same = std::memcmp(bufs[i].data(), want.data() + vec[i].off, vec[i].len) == 0;
  delete vi;
  check(same, phase + ": a vector read gives the expected bytes");

  if (fileSystem) {
    std::string lastUrl, dataServer;
    const bool haveLast = f.GetProperty("LastURL", lastUrl);
    const bool haveServer = f.GetProperty("DataServer", dataServer);
    check(haveLast && lastUrl == "file://localhost" + path,
          phase + ": LastURL is file://localhost" + path + " (got '" + lastUrl + "')");
    check(haveServer && dataServer.empty(),
          phase + ": DataServer is empty (got '" + dataServer + "')");
  }
  const XrdCl::XRootDStatus cs = f.Close();
  check(cs.IsOK(), phase + ": close");
  if (fileSystem)
    fileSystemJob(path, want, phase);
}

// Phase E: one handle reads version `v` and stays open while the file is
// rewritten as version `next` at the same size with a later time; a second
// handle in this process then reads `next`.
void sameProcessJob(const std::string& url, const std::string& path, int v, int next,
                    const std::string& phase) {
  XrdCl::File first;
  const XrdCl::XRootDStatus os = first.Open(url, XrdCl::OpenFlags::Read);
  const std::string want = version(v);
  std::vector<char> buf(256 * 1024);
  uint32_t got = 0;
  const bool read = os.IsOK() && first.Read(0, static_cast<uint32_t>(buf.size()), buf.data(), got)
                                     .IsOK() &&
                    got == buf.size() && std::memcmp(buf.data(), want.data(), got) == 0;
  check(read, phase + ": the first handle reads the file");
  struct ::timespec later{};
  const bool rewritten = mtimeOfPath(path, later) && writeFile(path, version(next), O_TRUNC) &&
                         (later.tv_sec += 100, setMtime(path, later));
  check(rewritten, phase + ": the file rewritten at its size, with a later time");
  readJob(url, path, version(next), phase, false);
  if (os.IsOK()) {
    const XrdCl::XRootDStatus cs = first.Close();
    (void)cs;
  }
}

// The ranges of phases H and I: past 1 MiB, and read by no earlier phase.
const std::vector<Range> kShortRanges = {{(9u << 18) + 4096, 4096}, {(11u << 18) + 123, 4096}};

// Phase H: a handle open while the file is truncated under it; its vector read
// past the new end is not answered as complete. Phase I (`check`): the file
// restored, the same ranges are its bytes (nothing H did not get was kept).
void shortReadJob(const std::string& url, const std::string& path, const std::string& phase,
                  bool truncateFirst) {
  XrdCl::File f;
  const XrdCl::XRootDStatus os = f.Open(url, XrdCl::OpenFlags::Read);
  check(os.IsOK(), phase + ": open");
  if (!os.IsOK())
    return;
  if (truncateFirst)
    check(::truncate(path.c_str(), 1 << 20) == 0, phase + ": the file truncated to 1 MiB");
  std::vector<std::vector<char>> bufs;
  bufs.reserve(kShortRanges.size());
  XrdCl::ChunkList chunks;
  uint64_t asked = 0;
  for (const Range& r : kShortRanges) {
    bufs.emplace_back(r.len, 'x');
    chunks.emplace_back(r.off, r.len, bufs.back().data());
    asked += r.len;
  }
  XrdCl::VectorReadInfo* vi = nullptr;
  const XrdCl::XRootDStatus vs = f.VectorRead(chunks, nullptr, vi);
  const bool full = vs.IsOK() && vi && vi->GetSize() == asked;
  if (truncateFirst) {
    // An error, as a server answers: ROOT checks only the status of a vector read, so an answer
    // short of what was asked would be read as whole.
    check(!vs.IsOK() && !full, phase + ": a vector read past the new end fails (" + vs.ToStr() +
                                   ", " + std::to_string(vi ? vi->GetSize() : 0) + " bytes)");
  } else {
    const std::string want = version(5);
    bool same = full;
    for (size_t i = 0; same && i < kShortRanges.size(); ++i)
      same =
          std::memcmp(bufs[i].data(), want.data() + kShortRanges[i].off, kShortRanges[i].len) == 0;
    check(same, phase + ": the ranges read while the file was truncated are the file's bytes");
  }
  delete vi;
  const XrdCl::XRootDStatus cs = f.Close();
  (void)cs;
}

// Phase O: a process that has just read uncached ranges of the file (single
// reads, which the client's local-file access makes asynchronously) forks at
// once, as a Python pool or a parallel reader does; the child, through the
// handle it inherited, reads another uncached range within a watchdog.
void forkJob(const std::string& url, const std::string& phase) {
  XrdCl::File f;
  const XrdCl::XRootDStatus os = f.Open(url, XrdCl::OpenFlags::Read);
  check(os.IsOK(), phase + ": open");
  if (!os.IsOK())
    return;
  const std::string want = version(5);
  std::vector<char> buf(4096);
  uint32_t got = 0;
  bool read = true;
  for (const uint64_t off : {uint64_t(768) << 10, uint64_t(1280) << 10})
    read = read && f.Read(off, 4096, buf.data(), got).IsOK() && got == 4096 &&
           std::memcmp(buf.data(), want.data() + off, 4096) == 0;
  check(read, phase + ": the parent's reads");
  std::fflush(stdout);
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::alarm(20); // a read that never completes ends the child here
    const uint64_t off = uint64_t(2560) << 10;
    uint32_t n = 0;
    const bool ok = f.Read(off, 4096, buf.data(), n).IsOK() && n == 4096 &&
                    std::memcmp(buf.data(), want.data() + off, 4096) == 0;
    ::_exit(ok ? 0 : 1);
  }
  int st = 0;
  ::waitpid(pid, &st, 0);
  check(pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
        phase + ": the child's read of an uncached range completes, with the file's bytes (" +
            (WIFSIGNALED(st) ? "killed by signal " + std::to_string(WTERMSIG(st))
                             : "exit " + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -1)) +
            ")");
  const XrdCl::XRootDStatus cs = f.Close();
  (void)cs;
}

// What an asynchronous open's answer carries.
class OpenWait : public XrdCl::ResponseHandler {
 public:
  void HandleResponseWithHosts(XrdCl::XRootDStatus* s, XrdCl::AnyObject* r,
                               XrdCl::HostList* h) override {
    std::lock_guard<std::mutex> g(mu);
    ok = s && s->IsOK();
    hosts = h != nullptr;
    delete s;
    delete r;
    delete h;
    done = true;
    cv.notify_all();
  }
  bool wait() {
    std::unique_lock<std::mutex> g(mu);
    return cv.wait_for(g, std::chrono::seconds(30), [this] { return done; });
  }
  std::mutex mu;
  std::condition_variable cv;
  bool done = false, ok = false, hosts = false;
};

void missingJob(const std::string& url, const std::string& phase) {
  XrdCl::File f;
  const XrdCl::XRootDStatus os = f.Open(url, XrdCl::OpenFlags::Read);
  check(!os.IsOK(), phase + ": the open fails (" + os.ToStr() + ")");
  auto* w = new OpenWait; // leaked: never destroyed before its answer
  XrdCl::File g;
  const XrdCl::XRootDStatus as = g.Open(url, XrdCl::OpenFlags::Read, XrdCl::Access::None, w);
  const bool answered = as.IsOK() && w->wait();
  check(!as.IsOK() || (answered && !w->ok && w->hosts),
        phase + ": an asynchronous open's failure carries a host list");
}

// A phase, in the process the probe started for it: set up the client, run
// the job of its kind, check what reached a server, exit normally.
int runPhase(const std::string& name, const std::string& url, const std::string& path, int v,
             const std::string& kind) {
  XrdCl::Log* log = XrdCl::DefaultEnv::GetLog();
  log->SetOutput(new Capture);
  log->SetLevel(XrdCl::Log::DebugMsg);
  if (kind == "missing") {
    missingJob(url, name);
  } else if (kind == "refused") { // a removed file, a directory
    XrdCl::File f;
    const XrdCl::XRootDStatus os = f.Open(url, XrdCl::OpenFlags::Read);
    check(!os.IsOK(), name + ": the open is refused (" + os.ToStr() + ")");
  } else if (kind == "same-process") {
    sameProcessJob(url, path, v, v + 1, name);
  } else if (kind == "short" || kind == "after-short") {
    shortReadJob(url, path, name, kind == "short");
  } else if (kind == "fork") {
    forkJob(url, name);
  } else if (kind == "unbound") {
    XrdCl::File f;
    const XrdCl::XRootDStatus os = f.Open(url, XrdCl::OpenFlags::Read);
    check(!os.IsOK(), name + ": the open fails (" + os.ToStr() + ")");
    check(!networkLine().empty(),
          name + ": the client logged its attempt to reach a server (the log check sees one)");
    std::fflush(stdout);
    return gFails ? 1 : 0;
  } else {
    readJob(url, path, version(v), name, kind == "fs");
  }
  // The plug-in's binding is logged before the capture starts (the client
  // reads its plug-in configuration when it sets up its log); the inner
  // File's own lines come after, and say the local-file access was used.
  if (kind != "refused")
    check(logged("@file://localhost/"),
          name + ": the client's log was captured (it holds the local-file access)");
  const std::string net = networkLine();
  check(net.empty(), name + ": nothing was sent to a server" +
                         (net.empty() ? "" : " (the client logged: " + net + ")"));
  if (gFails) { // what the plug-in and the client's plug-in manager said, for the diagnosis
    std::lock_guard<std::mutex> g(lines().mu);
    for (const auto& l : lines().all)
      if (l.find("[UCache") != std::string::npos || l.find("[PlugInMgr") != std::string::npos)
        std::printf("  log: %s", l.c_str());
  }
  std::fflush(stdout);
  return gFails ? 1 : 0;
}

// Runs a phase in a new process of this program, with the environment set up.
// Kinds: "fs" (reads, then the requests on the host), "plain" (reads),
// "same-process" (E), "disabled" (reads with UCACHE_DISABLE=1), "short" (H),
// "after-short" (I), "fork" (O), "missing" (J), "refused" (K, L), "unbound" (N).
bool phase(const std::string& self, const char* name, const std::string& url,
           const std::string& path, int v, const char* kind) {
  std::fflush(stdout);
  const pid_t pid = ::fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    const std::string vs = std::to_string(v);
    if (std::strcmp(kind, "disabled") == 0)
      ::setenv("UCACHE_DISABLE", "1", 1);
    if (std::strcmp(kind, "unbound") ==
        0) { // a host that resolves somewhere is not waited for long
      ::setenv("XRD_CONNECTIONWINDOW", "3", 1);
      ::setenv("XRD_CONNECTIONRETRY", "1", 1);
      ::setenv("XRD_REQUESTTIMEOUT", "10", 1);
    }
    ::execl(self.c_str(), self.c_str(), "--phase", name, url.c_str(), path.c_str(), vs.c_str(),
            kind, static_cast<char*>(nullptr));
    std::printf("FAIL: %s: cannot run %s again: %s\n", name, self.c_str(), std::strerror(errno));
    std::fflush(stdout);
    ::_exit(1);
  }
  int st = 0;
  ::waitpid(pid, &st, 0);
  return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

// The plug-in conf, with or without logical names.
bool writeConf(const std::string& conf, const std::string& lib, const std::string& cache,
               bool names) {
  return writeFile(conf + "/ucache.conf",
                   "url = local\nlib = " + lib + "\nenable = true\ndir = " + cache + "\n" +
                       (names ? "logical_name_prefixes = /store/\n" : ""),
                   O_TRUNC);
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 7 && std::strcmp(argv[1], "--phase") == 0)
    return runPhase(argv[2], argv[3], argv[4], std::atoi(argv[5]), argv[6]);
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <plugin library>\n", argv[0]);
    return 3;
  }
  char self[PATH_MAX];
  if (!::realpath(argv[0], self)) {
    std::fprintf(stderr, "local_probe: SETUP, cannot resolve %s\n", argv[0]);
    return 3;
  }
  const char* tmp = ::getenv("TMPDIR");
  std::string base = std::string(tmp && *tmp ? tmp : "/tmp") + "/ucache-local-probe-XXXXXX";
  std::vector<char> tmpl(base.begin(), base.end());
  tmpl.push_back('\0');
  if (!::mkdtemp(tmpl.data())) {
    std::fprintf(stderr, "local_probe: SETUP, cannot make a directory under %s\n", base.c_str());
    return 3;
  }
  // The client and the cache see this path as the URL names it: made absolute
  // and without links, so that the URL's path is the file's own.
  char real[PATH_MAX];
  if (!::realpath(tmpl.data(), real)) {
    std::fprintf(stderr, "local_probe: SETUP, cannot resolve %s\n", tmpl.data());
    return 3;
  }
  const std::string top = real, conf = top + "/conf", cache = top + "/cache", data = top + "/data",
                    path = data + "/file.bin", nosearch = top + "/nosearch",
                    nameA = top + "/a/store/x/f.bin", nameB = top + "/b/store/x/f.bin";
  bool made = true;
  for (const std::string& d :
       {conf, cache, data, nosearch, top + "/a", top + "/a/store", top + "/a/store/x", top + "/b",
        top + "/b/store", top + "/b/store/x"})
    made = made && ::mkdir(d.c_str(), 0755) == 0;
  if (!made || !writeConf(conf, argv[1], cache, false) ||
      !writeFile(nosearch + "/f", "x", O_TRUNC) || ::chmod(nosearch.c_str(), 0644) != 0 ||
      ::symlink((data + "/nothing-there").c_str(), (data + "/dangling").c_str()) != 0) {
    std::fprintf(stderr, "local_probe: SETUP, cannot prepare %s\n", top.c_str());
    return 3;
  }
  // The settings under test keep their defaults, whatever the environment has:
  // no UCACHE_* or XRD_* variable of the caller's reaches a phase.
  std::vector<std::string> inherited;
  for (char** e = environ; *e; ++e) {
    const std::string kv = *e;
    if (kv.compare(0, 7, "UCACHE_") == 0 || kv.compare(0, 4, "XRD_") == 0)
      inherited.push_back(kv.substr(0, kv.find('=')));
  }
  for (const auto& n : inherited)
    ::unsetenv(n.c_str());
  ::setenv("XRD_PLUGINCONFDIR", conf.c_str(), 1);
  ::setenv("UCACHE_DIR", cache.c_str(), 1);

  struct ::timespec t1{};
  if (!writeFile(path, version(1), O_TRUNC) || !mtimeOfPath(path, t1)) {
    std::fprintf(stderr, "local_probe: SETUP, cannot write %s\n", path.c_str());
    return 3;
  }
  const std::string url = "root://local/" + path; // the double slash of an absolute path

  bool ok = phase(self, "A cold", url, path, 1, "fs");
  check(cacheHasEntry(cache), "A: the file has a cache entry");
  ok = gFails == 0 && ok;

  ok = writeFile(path, version(2), 0) && setMtime(path, t1) && ok; // same size and time
  ok = phase(self, "B same size and time", url, path, 1, "plain") && ok;

  struct ::timespec later = t1;
  later.tv_sec += 100;
  ok = setMtime(path, later) && ok;
  ok = phase(self, "C new time", url, path, 2, "plain") && ok;

  ok = writeFile(path, version(3), O_TRUNC) && ok;
  ok = phase(self, "D new size", url, path, 3, "plain") && ok;

  ok = phase(self, "E one process", url, path, 3, "same-process") && ok; // leaves version 4
  struct ::timespec t4{};
  ok = mtimeOfPath(path, t4) && writeFile(path, version(5), O_TRUNC) && setMtime(path, t4) && ok;
  ok = phase(self, "F new size, same time", url, path, 5, "plain") && ok;
  ok = phase(self, "G disabled", url, path, 5, "disabled") && ok;

  ok = phase(self, "H truncated while read", url, path, 5, "short") && ok;
  ok = writeFile(path, version(5), O_TRUNC) && setMtime(path, t4) && ok; // as it was, time too
  ok = phase(self, "I after the truncation", url, path, 5, "after-short") && ok;
  ok = phase(self, "O a fork right after reads", url, path, 5, "fork") && ok;

  ok = phase(self, "J missing", url + ".missing", path, 5, "missing") && ok;
  ok = ::unlink(path.c_str()) == 0 && ok;
  ok = phase(self, "K removed after it was cached", url, path, 5, "refused") && ok;
  ok = phase(self, "L a directory", "root://local/" + data, data, 0, "refused") && ok;

  struct ::timespec ta{};
  ok = writeFile(nameA, version(11), O_TRUNC) && writeFile(nameB, version(12), O_TRUNC) &&
       mtimeOfPath(nameA, ta) && (ta.tv_sec += 100, setMtime(nameB, ta)) &&
       writeConf(conf, argv[1], cache, true) && ok;
  const std::string urlA = "root://local/" + nameA, urlB = "root://local/" + nameB;
  ok = phase(self, "M1 names: the first file", urlA, nameA, 11, "plain") && ok;
  ok = phase(self, "M2 names: the second served the first's entry", urlB, nameB, 11, "plain") && ok;
  ok = writeConf(conf, argv[1], cache, false) && ok;
  ok = phase(self, "M3 names off: the second's own bytes", urlB, nameB, 12, "plain") && ok;
  ok = writeConf(conf, argv[1], cache, true) && ok;
  ok = phase(self, "M4 names on again: one entry again", urlB, nameB, 11, "plain") && ok;

  ok = phase(self, "N an unbound URL (the control)", "root://local:1095/" + nameA, nameA, 11,
             "unbound") &&
       ok;

  ok = ok && gFails == 0;
  ::chmod(nosearch.c_str(), 0755); // for the removal below, or a look at what failed
  if (ok)
    ::nftw(top.c_str(), removeOne, 16, FTW_DEPTH | FTW_PHYS);
  else
    std::printf("local_probe: kept %s\n", top.c_str());
  std::printf("local_probe: %s\n", ok ? "ALL PASS" : "FAILED");
  return ok ? 0 : 1;
}
