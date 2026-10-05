// The cache's own origin open keeps the servers a reader asked to avoid.
//
//   tried_probe <root-url>
//
// Needs XRD_PLUGINCONFDIR binding the URL's host to the plugin, and UCACHE_DIR
// naming a cache directory that holds no entries yet. The default trust window
// (revalidate_seconds) is assumed: a file cached a moment ago is trusted.
//
// A reader names a server to avoid by adding ?tried=<host> to the URL. The
// probe adds ?tried=excluded.example.org and makes two opens in one process:
//   1. On the empty cache the open is relayed: the reader's URL goes to the
//      origin as written. It reads 64 KiB, which the cache keeps, and closes.
//   2. The second open is answered from the fresh entry, with no request to
//      the origin. It reads a range the cache does not hold, so the cache
//      opens the origin itself, and closes.
// The XRootD client logs every request it creates at Debug level; the probe
// captures that log in its own process and looks at the open requests.
//
// Exit 0: exactly two open requests went to the origin and the second, the
// cache's own, carries tried=excluded.example.org. Exit 1: it does not (or the
// second open's read failed). Exit 2: the second open was not answered from
// the cache, so the cache's own open never happened and nothing was tested;
// that fails too. Exit 3: usage or setup error (bad arguments, a cache that
// already holds entries, an open that fails, a first read that fails, a file
// too small for the probe), printed as "tried_probe: SETUP".
#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClLog.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <mutex>
#include <string>
#include <vector>

namespace {

const char* const kTried = "tried=excluded.example.org";

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

// The file argument of every open request created so far, in order.
std::vector<std::string> openRequests() {
  std::vector<std::string> out;
  std::lock_guard<std::mutex> g(lines().mu);
  for (const auto& l : lines().all) {
    if (l.find("MsgHandler created") == std::string::npos)
      continue;
    const std::string tag = "kXR_open (file: ";
    const auto at = l.find(tag);
    if (at == std::string::npos)
      continue;
    const auto from = at + tag.size();
    const auto end = l.find_first_of(", )", from);
    out.push_back(l.substr(from, end == std::string::npos ? std::string::npos : end - from));
  }
  return out;
}

bool hasEntries(const std::string& cache) {
  DIR* top = ::opendir((cache + "/objects").c_str());
  if (!top)
    return false;
  bool found = false;
  while (dirent* e = ::readdir(top)) {
    if (e->d_name[0] == '.')
      continue;
    if (DIR* sub = ::opendir((cache + "/objects/" + e->d_name).c_str())) {
      while (dirent* f = ::readdir(sub))
        if (f->d_name[0] != '.')
          found = true;
      ::closedir(sub);
    }
  }
  ::closedir(top);
  return found;
}

enum class Outcome { Ok, OpenFailed, TooSmall, ReadFailed };

// Open, read `len` bytes at `off`, close. When off < 0 the read is from the
// middle of the file, well away from the first read.
Outcome openRead(const std::string& url, long long off, uint32_t len, size_t* requestsAtOpen) {
  XrdCl::File f;
  const XrdCl::XRootDStatus os = f.Open(url, XrdCl::OpenFlags::Read);
  if (!os.IsOK()) {
    std::printf("open failed: %s\n", os.ToString().c_str());
    return Outcome::OpenFailed;
  }
  if (requestsAtOpen)
    *requestsAtOpen = openRequests().size();
  if (off < 0) {
    XrdCl::StatInfo* si = nullptr;
    const XrdCl::XRootDStatus ss = f.Stat(false, si);
    const uint64_t sz = ss.IsOK() && si ? si->GetSize() : 0;
    delete si;
    if (sz < 2ull * len + (1u << 20)) {
      std::printf("the file is too small for the probe (%llu bytes)\n",
                  static_cast<unsigned long long>(sz));
      const XrdCl::XRootDStatus cs = f.Close();
      (void)cs;
      return Outcome::TooSmall;
    }
    off = static_cast<long long>((sz / 2) / len * len);
  }
  std::vector<char> buf(len);
  uint32_t got = 0;
  const XrdCl::XRootDStatus rs = f.Read(static_cast<uint64_t>(off), len, buf.data(), got);
  const XrdCl::XRootDStatus cs = f.Close();
  (void)cs;
  if (!rs.IsOK() || got != len) {
    std::printf("read of %u bytes at %lld failed: %s (got %u)\n", len, off,
                rs.ToString().c_str(), got);
    return Outcome::ReadFailed;
  }
  return Outcome::Ok;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <root-url>\n", argv[0]);
    return 3;
  }
  const char* cacheEnv = ::getenv("UCACHE_DIR");
  if (!cacheEnv || !*cacheEnv) {
    std::fprintf(stderr, "tried_probe: SETUP, set UCACHE_DIR to an empty cache directory\n");
    return 3;
  }
  if (hasEntries(cacheEnv)) {
    std::fprintf(stderr, "tried_probe: SETUP, %s already holds entries; give it an empty one\n",
                 cacheEnv);
    return 3;
  }
  // Before the first request: the client logs each request as it creates it.
  XrdCl::Log* log = XrdCl::DefaultEnv::GetLog();
  log->SetOutput(new Capture);
  log->SetLevel(XrdCl::Log::DebugMsg);

  std::string url = argv[1];
  url += (url.find('?') == std::string::npos ? "?" : "&");
  url += kTried;

  const uint32_t kLen = 64 * 1024;
  // The first open is the relayed one, which every build sends as written, so
  // a failure there says the setup is wrong (origin down, plugin not bound).
  if (openRead(url, 0, kLen, nullptr) != Outcome::Ok) {
    std::printf("tried_probe: SETUP, the first open or its read failed\n");
    return 3;
  }
  size_t atOpen2 = 0;
  const Outcome second = openRead(url, -1, kLen, &atOpen2);
  // An answer from the cache does not fail; an open that failed went to the
  // origin, like the first, so it is a setup failure too.
  if (second == Outcome::OpenFailed || second == Outcome::TooSmall) {
    const char* why =
        second == Outcome::TooSmall ? "give it a larger file" : "the second open failed";
    std::printf("tried_probe: SETUP, %s\n", why);
    return 3;
  }
  const std::vector<std::string> opens = openRequests();

  std::printf("open requests sent to the origin:\n");
  for (size_t i = 0; i < opens.size(); ++i)
    std::printf("  %zu: %s\n", i + 1, opens[i].c_str());

  // The relayed first open sends the reader's URL; the second open must not
  // have sent anything by the time it returned. (The request count decides
  // this, not the plugin's own log line: the plugin settles its log level when
  // it loads, which can be before the level above is set.)
  if (atOpen2 != 1) {
    std::printf("tried_probe: FAIL, the second open was not answered from the cache "
                "(%zu open requests when it returned, want 1)\n",
                atOpen2);
    return 2;
  }
  int fails = 0;
  const auto check = [&fails](bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
      ++fails;
  };
  check(second == Outcome::Ok, "the second open read the uncached range");
  check(opens.size() == 2, "exactly two open requests reached the origin");
  check(!opens.empty() && opens[0].find(kTried) != std::string::npos,
        "the relayed open carries the reader's tried=");
  check(opens.size() >= 2 && opens[1].find(kTried) != std::string::npos,
        "the cache's own open carries the reader's tried=");
  std::printf(fails ? "tried_probe: %d FAILED\n" : "tried_probe: ALL PASS\n", fails);
  return fails ? 1 : 0;
}
