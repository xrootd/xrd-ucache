#include "UsableCpus.h"

#include <fstream>
#include <sstream>
#include <unistd.h>
#include <vector>
#if defined(__linux__)
#include <sched.h>
#endif

namespace ucache {
namespace {

// ---- cgroup CPU quota ------------------------------------------------------

std::vector<std::string> splitOn(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t at = 0;
  for (;;) {
    const size_t e = s.find(sep, at);
    out.push_back(s.substr(at, e == std::string::npos ? std::string::npos : e - at));
    if (e == std::string::npos)
      return out;
    at = e + 1;
  }
}

bool parseU64(const std::string& s, uint64_t& v) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
    ++i;
  if (i == s.size() || s[i] < '0' || s[i] > '9')
    return false;
  v = 0;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
    if (v > (UINT64_MAX - 9) / 10)
      return false;
    v = v * 10 + static_cast<uint64_t>(s[i] - '0');
  }
  for (; i < s.size(); ++i)
    if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n')
      return false;
  return true;
}

// A cgroup hierarchy as this process sees it: where it is mounted, which of
// its cgroups the mount's root is, and which the process is in.
struct Hierarchy {
  std::string point, root = "/", path;
};

// The mount of the cgroup v2 hierarchy (`v2`), or of the v1 hierarchy that
// carries the cpu controller, from /proc/self/mountinfo: "<id> <parent>
// <dev> <root> <point> <opts> [optional...] - <type> <source> <super opts>".
bool findMount(const std::string& mountinfo, bool v2, Hierarchy& h) {
  for (const std::string& line : splitOn(mountinfo, '\n')) {
    const std::vector<std::string> f = splitOn(line, ' ');
    size_t dash = 0;
    while (dash < f.size() && f[dash] != "-")
      ++dash;
    if (dash < 5 || dash + 3 >= f.size())
      continue;
    const std::string& type = f[dash + 1];
    bool match = v2 && type == "cgroup2";
    if (!v2 && type == "cgroup")
      for (const std::string& o : splitOn(f[dash + 3], ','))
        match = match || o == "cpu";
    if (match) {
      h.root = f[3];
      h.point = f[4];
      return true;
    }
  }
  return false;
}

// The tightest quota on the way from the process's cgroup up to the mount's
// root; 0 when none. `quotaIn(dir)` reads one cgroup's quota (0: none).
template <typename QuotaIn> unsigned walkUp(const Hierarchy& h, QuotaIn quotaIn) {
  if (h.point.empty() || h.point[0] != '/')
    return 0; // not a path: nothing to read
  // The process's cgroup, relative to the mount's root. A cgroup outside
  // what the mount shows (another namespace's) leaves only the mount's own.
  std::string rel;
  const std::string root = h.root == "/" ? "" : h.root;
  if (h.path.compare(0, root.size(), root) == 0 &&
      (h.path.size() == root.size() || h.path[root.size()] == '/'))
    rel = h.path.substr(root.size());
  std::string point = h.point;
  while (point.size() > 1 && point.back() == '/')
    point.pop_back();
  if (point.size() < 2)
    return 0; // a hierarchy mounted at / is not one this reads
  std::string dir = point;
  for (const std::string& c : splitOn(rel, '/'))
    if (!c.empty() && c != "." && c != "..")
      dir += "/" + c;
  unsigned best = 0;
  for (;;) {
    const unsigned q = quotaIn(dir);
    if (q && (!best || q < best))
      best = q;
    if (dir.size() <= point.size())
      return best;
    dir.resize(dir.rfind('/'));
  }
}

} // namespace

bool parseCpuMax(const std::string& text, uint64_t& quota, uint64_t& period) {
  std::string t = text;
  while (!t.empty() && (t.back() == '\n' || t.back() == ' '))
    t.pop_back();
  const size_t sp = t.find(' ');
  if (sp == std::string::npos)
    return false;
  uint64_t q = 0, p = 0;
  if (!parseU64(t.substr(0, sp), q) || !parseU64(t.substr(sp + 1), p) || !q || !p)
    return false; // "max <period>" is no quota; anything else is not one either
  quota = q;
  period = p;
  return true;
}

unsigned cpusForQuota(uint64_t quota, uint64_t period) {
  if (!period)
    return 0;
  const uint64_t n = quota / period + (quota % period ? 1 : 0);
  if (n > UINT32_MAX)
    return UINT32_MAX;
  return n ? static_cast<unsigned>(n) : 1u;
}

unsigned cgroupCpuQuota(const ReadTextFn& read) {
  std::string cgroups, mountinfo;
  if (!read("/proc/self/cgroup", cgroups))
    return 0;
  const bool haveMounts = read("/proc/self/mountinfo", mountinfo);
  // "<id>:<controllers>:<path>": id 0 with no controllers is the v2
  // hierarchy; a v1 line naming the cpu controller is the one a quota lives in.
  Hierarchy v2, v1;
  bool inV2 = false, inV1 = false;
  for (const std::string& line : splitOn(cgroups, '\n')) {
    const size_t a = line.find(':');
    const size_t b = a == std::string::npos ? a : line.find(':', a + 1);
    if (b == std::string::npos)
      continue;
    const std::string id = line.substr(0, a), ctl = line.substr(a + 1, b - a - 1);
    const std::string path = line.substr(b + 1);
    if (id == "0" && ctl.empty()) {
      v2.path = path;
      inV2 = true;
    } else {
      for (const std::string& c : splitOn(ctl, ','))
        if (c == "cpu") {
          v1.path = path;
          inV1 = true;
        }
    }
  }
  unsigned best = 0;
  auto take = [&best](unsigned q) {
    if (q && (!best || q < best))
      best = q;
  };
  if (inV2) {
    if (!haveMounts || !findMount(mountinfo, true, v2))
      v2.point = "/sys/fs/cgroup";
    take(walkUp(v2, [&read](const std::string& dir) {
      std::string t;
      uint64_t q = 0, p = 0;
      return read(dir + "/cpu.max", t) && parseCpuMax(t, q, p) ? cpusForQuota(q, p) : 0u;
    }));
  }
  if (inV1) {
    if (!haveMounts || !findMount(mountinfo, false, v1))
      v1.point = "/sys/fs/cgroup/cpu";
    take(walkUp(v1, [&read](const std::string& dir) {
      std::string qt, pt;
      uint64_t q = 0, p = 0;
      // A quota of -1 is none; it does not parse as a number, so it is none here.
      if (!read(dir + "/cpu.cfs_quota_us", qt) || !read(dir + "/cpu.cfs_period_us", pt) ||
          !parseU64(qt, q) || !parseU64(pt, p) || !q)
        return 0u;
      return cpusForQuota(q, p);
    }));
  }
  return best;
}

unsigned cgroupCpuQuota() {
#if defined(__linux__)
  return cgroupCpuQuota([](const std::string& path, std::string& text) {
    std::ifstream in(path);
    if (!in)
      return false;
    std::ostringstream os;
    os << in.rdbuf();
    text = os.str();
    return !in.bad();
  });
#else
  return 0;
#endif
}

unsigned usableCpus() {
  unsigned n = 0;
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  if (::sched_getaffinity(0, sizeof set, &set) == 0 && CPU_COUNT(&set) > 0)
    n = static_cast<unsigned>(CPU_COUNT(&set));
#endif
  if (!n) {
    const long c = ::sysconf(_SC_NPROCESSORS_ONLN);
    n = c > 0 ? static_cast<unsigned>(c) : 1u;
  }
  const unsigned q = cgroupCpuQuota();
  return q && q < n ? q : n;
}

} // namespace ucache
