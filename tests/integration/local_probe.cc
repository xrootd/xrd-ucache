// Local files: for a URL whose host is `local`, root://local//abs/path, the
// plug-in reads the file /abs/path of this machine's file system, caches it,
// and checks the cached entry against the file at every open.
//
//   local_probe <plugin library>
//
// Self-contained, with no server: the probe makes a directory of its own
// (under TMPDIR, else /tmp) holding a plug-in conf that binds the host `local`,
// an empty cache and one data file, and points the client at them. Each phase
// runs in a process of its own, as separate jobs would: the probe runs itself
// again with that environment (the client reads its environment when its
// library loads, before main), and the phase ends with a normal exit, which
// is when a job's cache writes are complete. The parent never uses the client
// and changes the file between phases:
//   A. cold: plain and vector reads of the file through root://local are its
//      bytes; the handle's LastURL is file://localhost/<path> and its
//      DataServer ""; a stat and a directory listing on the host (plain, and
//      with the locate ROOT asks for) answer from the file system, and a stat
//      of a missing path says "not found".
//   B. the file is rewritten in place with other bytes of the same size, and
//      its modification time put back: the reads give the FIRST bytes, which
//      only the cache holds.
//   C. a new modification time: the reads give the new bytes.
//   D. a new size: the open's stat gives it, and the reads give the new bytes.
//   E. in one process: a handle reads the file and stays open, the file is
//      rewritten at the same size with a new time, and a second handle reads
//      the new bytes (the process's open entry is not reused for them).
//   F. with uCache disabled (UCACHE_DISABLE=1) the file is still read.
//   G. an open of a file that does not exist fails.
// In every phase the client's own log (Debug level, captured in the child)
// must hold the local-file access's own lines (the log was captured) and show no
// connection, no address lookup and no channel: nothing is ever sent to a
// server. The trust window and the freshness check are left at their
// defaults (revalidate_seconds of 7 days, validate = size), because a local
// file is checked at every open whatever they say.
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
#include <climits>
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

// Deterministic contents: `seed` tells two versions of the file apart at every byte.
std::string contents(size_t n, unsigned seed) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i)
    s[i] = static_cast<char>(((i + seed) * 2654435761u >> 13) ^ seed);
  return s;
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

struct Range {
  uint64_t off;
  uint32_t len;
};

// One job: open the URL, read the ranges (the first ones plainly, the rest in
// one vector read), compare them with `want`, close.
void readJob(const std::string& url, const std::string& path, const std::string& want,
             const char* phase, bool fileSystem) {
  XrdCl::File f;
  const XrdCl::XRootDStatus os = f.Open(url, XrdCl::OpenFlags::Read);
  check(os.IsOK(), std::string(phase) + ": open of " + url + (os.IsOK() ? "" : ": " + os.ToStr()));
  if (!os.IsOK())
    return;
  XrdCl::StatInfo* si = nullptr;
  const XrdCl::XRootDStatus ss = f.Stat(false, si);
  check(ss.IsOK() && si && si->GetSize() == want.size(),
        std::string(phase) + ": the open's size is the file's (" + std::to_string(want.size()) +
            ")");
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
  check(same, std::string(phase) + ": plain reads give the expected bytes");

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
  check(same, std::string(phase) + ": a vector read gives the expected bytes");

  if (fileSystem) {
    std::string lastUrl, dataServer;
    const bool haveLast = f.GetProperty("LastURL", lastUrl);
    const bool haveServer = f.GetProperty("DataServer", dataServer);
    check(haveLast && lastUrl == "file://localhost" + path,
          std::string(phase) + ": LastURL is file://localhost" + path + " (got '" + lastUrl +
              "')");
    check(haveServer && dataServer.empty(),
          std::string(phase) + ": DataServer is empty (got '" + dataServer + "')");
  }
  const XrdCl::XRootDStatus cs = f.Close();
  check(cs.IsOK(), std::string(phase) + ": close");

  if (!fileSystem)
    return;
  XrdCl::FileSystem fs(XrdCl::URL("root://local:1094"));
  XrdCl::StatInfo* fsi = nullptr;
  const XrdCl::XRootDStatus fss = fs.Stat(path, fsi);
  check(fss.IsOK() && fsi && fsi->GetSize() == want.size() &&
            fsi->TestFlags(XrdCl::StatInfo::IsReadable) && !fsi->TestFlags(XrdCl::StatInfo::IsDir),
        std::string(phase) + ": a stat on the host gives the file's size" +
            (fss.IsOK() ? "" : ": " + fss.ToStr()));
  delete fsi;
  XrdCl::StatInfo* none = nullptr;
  const XrdCl::XRootDStatus ns = fs.Stat(path + ".missing", none);
  check(!ns.IsOK() && ns.code == XrdCl::errErrorResponse &&
            ns.errNo == static_cast<uint32_t>(kXR_NotFound),
        std::string(phase) + ": a stat of a missing path says not found (" + ns.ToStr() + ")");
  delete none;
  const std::string dir = path.substr(0, path.rfind('/'));
  const std::string name = path.substr(path.rfind('/') + 1);
  // Plainly, and as ROOT lists a directory (for a wildcard in a TChain): with a
  // locate first, which the client turns into a listing where it was found.
  for (const auto flags : {XrdCl::DirListFlags::Stat,
                           XrdCl::DirListFlags::Stat | XrdCl::DirListFlags::Locate}) {
    XrdCl::DirectoryList* list = nullptr;
    const XrdCl::XRootDStatus ls = fs.DirList(dir, flags, list);
    bool listed = false;
    if (ls.IsOK() && list)
      for (auto it = list->Begin(); it != list->End(); ++it)
        if ((*it)->GetName() == name && (*it)->GetStatInfo() &&
            (*it)->GetStatInfo()->GetSize() == want.size())
          listed = true;
    check(listed, std::string(phase) + ": a listing of the directory" +
                      (flags & XrdCl::DirListFlags::Locate ? " (with a locate)" : "") +
                      " holds the file, with its size" + (ls.IsOK() ? "" : ": " + ls.ToStr()));
    delete list;
  }
}

const size_t kSize = (3u << 20) + 4321;

// The file's contents in each version: 1 and 2 differ at (almost) every byte
// and have one size; 3 is 2 with 4 KiB more; 4 is another file of 3's size.
std::string version(int v) {
  if (v == 3)
    return contents(kSize, 2) + contents(4096, 3);
  if (v == 4)
    return contents(kSize + 4096, 4);
  return contents(kSize, static_cast<unsigned>(v));
}

struct ::timespec mtimeOf(const struct ::stat& sb) {
#if defined(__APPLE__)
  return sb.st_mtimespec;
#else
  return sb.st_mtim;
#endif
}

// Phase E's job: one handle reads version `v` and stays open while the file is
// rewritten as version `next` at the same size with a later time; a second
// handle in this process then reads `next`.
void sameProcessJob(const std::string& url, const std::string& path, int v, int next,
                    const char* phase) {
  XrdCl::File first;
  const XrdCl::XRootDStatus os = first.Open(url, XrdCl::OpenFlags::Read);
  const std::string want = version(v);
  std::vector<char> buf(256 * 1024);
  uint32_t got = 0;
  const bool read = os.IsOK() && first.Read(0, static_cast<uint32_t>(buf.size()), buf.data(), got)
                                     .IsOK() &&
                    got == buf.size() && std::memcmp(buf.data(), want.data(), got) == 0;
  check(read, std::string(phase) + ": the first handle reads the file");
  struct ::stat sb{};
  struct ::timespec later{};
  const bool rewritten = ::stat(path.c_str(), &sb) == 0 &&
                         writeFile(path, version(next), O_TRUNC) &&
                         (later = mtimeOf(sb), later.tv_sec += 100, setMtime(path, later));
  check(rewritten, std::string(phase) + ": the file rewritten at its size, with a later time");
  readJob(url, path, version(next), phase, false);
  if (os.IsOK()) {
    const XrdCl::XRootDStatus cs = first.Close();
    (void)cs;
  }
}

// A phase, in the process the probe started for it: set up the client, run
// the job of its kind, check that nothing reached a server, exit normally.
int runPhase(const char* name, const std::string& url, const std::string& path, int v,
             const std::string& kind) {
  XrdCl::Log* log = XrdCl::DefaultEnv::GetLog();
  log->SetOutput(new Capture);
  log->SetLevel(XrdCl::Log::DebugMsg);
  if (kind == "missing") {
    XrdCl::File f;
    const XrdCl::XRootDStatus os = f.Open(url + ".missing", XrdCl::OpenFlags::Read);
    check(!os.IsOK(), std::string(name) + ": the open fails (" + os.ToStr() + ")");
  } else if (kind == "same-process") {
    sameProcessJob(url, path, v, v + 1, name);
  } else {
    readJob(url, path, version(v), name, kind == "fs");
  }
  // The plug-in's binding is logged before the capture starts (the client
  // reads its plug-in configuration when it sets up its log); the inner
  // File's own lines come after, and say the local-file access was used.
  check(logged("@file://localhost/"),
        std::string(name) + ": the client's log was captured (it holds the local-file access)");
  const std::string net = networkLine();
  check(net.empty(), std::string(name) + ": nothing was sent to a server" +
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
// Kinds: "fs" (reads, then the file-system requests), "plain" (reads),
// "same-process" (phase E), "disabled" (reads with UCACHE_DISABLE=1),
// "missing" (an open of a file that does not exist).
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
  const std::string top = real, conf = top + "/conf", cache = top + "/cache",
                    data = top + "/data", path = data + "/file.bin";
  if (::mkdir(conf.c_str(), 0755) != 0 || ::mkdir(cache.c_str(), 0755) != 0 ||
      ::mkdir(data.c_str(), 0755) != 0 ||
      !writeFile(conf + "/ucache.conf",
                 "url = local\nlib = " + std::string(argv[1]) + "\nenable = true\ndir = " + cache +
                     "\n",
                 O_TRUNC)) {
    std::fprintf(stderr, "local_probe: SETUP, cannot write under %s\n", top.c_str());
    return 3;
  }
  // The settings under test keep their defaults, whatever the environment has.
  ::setenv("XRD_PLUGINCONFDIR", conf.c_str(), 1);
  ::setenv("UCACHE_DIR", cache.c_str(), 1);
  for (const char* v : {"XRD_PLUGIN", "XRD_LOGFILE", "UCACHE_DISABLE", "UCACHE_REVALIDATE_S",
                        "UCACHE_VALIDATE"})
    ::unsetenv(v);

  if (!writeFile(path, version(1), O_TRUNC)) {
    std::fprintf(stderr, "local_probe: SETUP, cannot write %s\n", path.c_str());
    return 3;
  }
  struct ::stat sb{};
  if (::stat(path.c_str(), &sb) != 0) {
    std::fprintf(stderr, "local_probe: SETUP, cannot stat %s\n", path.c_str());
    return 3;
  }
  const struct ::timespec firstMtime = mtimeOf(sb);
  const std::string url = "root://local/" + path; // the double slash of an absolute path

  bool ok = phase(self, "A cold", url, path, 1, "fs");
  check(cacheHasEntry(cache), "A: the file has a cache entry");
  ok = gFails == 0 && ok;

  ok = writeFile(path, version(2), 0) && setMtime(path, firstMtime) && ok; // same size and time
  ok = phase(self, "B same size and time", url, path, 1, "plain") && ok;

  struct ::timespec later = firstMtime;
  later.tv_sec += 100;
  ok = setMtime(path, later) && ok;
  ok = phase(self, "C new time", url, path, 2, "plain") && ok;

  ok = writeFile(path, version(3), O_TRUNC) && ok;
  ok = phase(self, "D new size", url, path, 3, "plain") && ok;

  ok = phase(self, "E one process", url, path, 3, "same-process") && ok; // leaves version 4
  ok = phase(self, "F disabled", url, path, 4, "disabled") && ok;
  ok = phase(self, "G missing", url, path, 4, "missing") && ok;

  ok = ok && gFails == 0;
  if (ok)
    ::nftw(top.c_str(), removeOne, 16, FTW_DEPTH | FTW_PHYS);
  else
    std::printf("local_probe: kept %s\n", top.c_str());
  std::printf("local_probe: %s\n", ok ? "ALL PASS" : "FAILED");
  return ok ? 0 : 1;
}
