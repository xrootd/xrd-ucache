#include "InUse.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <fcntl.h>
#include <sstream>
#include <sys/file.h>

namespace ucache {

namespace {

bool live(const InUseRange& r, uint64_t nowS, uint64_t windowS) {
  return (r.lastS && r.lastS + windowS > nowS) ||
         (r.holdS && r.holdS + 2 * InUseRecord::kHoldRefreshS > nowS);
}

bool readAll(IOBackend& io, int fd, std::string& out) {
  struct ::stat st {};
  if (io.fstat(fd, &st) != 0)
    return false;
  out.resize(static_cast<size_t>(st.st_size));
  if (out.empty())
    return true;
  const int64_t n = io.preadFull(fd, out.data(), out.size(), 0);
  if (n < 0)
    return false;
  out.resize(static_cast<size_t>(n));
  return true;
}

} // namespace

std::string InUseRecord::path(const std::string& cacheDir, const std::string& hashHex) {
  return cacheDir + "/inuse/" + hashHex.substr(0, 2) + "/" + hashHex;
}

std::string InUseRecord::serialize() const {
  std::ostringstream os;
  os << "ucache-inuse 1\n";
  if (hasSettings)
    os << "settings " << settings.layoutVersion << ' ' << settings.slotFactor100 << ' '
       << (settings.codecs.empty() ? std::string("-") : settings.codecs) << '\n';
  os << "high " << highWater << '\n';
  char buf[96];
  for (const auto& r : ranges) {
    std::snprintf(buf, sizeof buf,
                  "range %016" PRIx64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                  "\n",
                  r.storeId, r.lo, r.hi, r.lastS, r.holdS, r.pid);
    os << buf;
  }
  return os.str();
}

InUseRecord InUseRecord::parse(const std::string& text) {
  InUseRecord rec;
  std::istringstream in(text);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (first) {
      first = false;
      if (line != "ucache-inuse 1")
        return InUseRecord{}; // not a record this build knows
      continue;
    }
    std::istringstream ls(line);
    std::string tag;
    ls >> tag;
    if (tag == "settings") {
      InUseSettings s;
      uint32_t f = 0;
      std::string c;
      if (ls >> s.layoutVersion >> f >> c && f <= 0xffff) {
        s.slotFactor100 = static_cast<uint16_t>(f);
        s.codecs = c == "-" ? std::string() : c;
        rec.settings = s;
        rec.hasSettings = true;
      }
    } else if (tag == "high") {
      uint64_t h = 0;
      if (ls >> h)
        rec.highWater = std::max(rec.highWater, h);
    } else if (tag == "range") {
      std::string id;
      InUseRange r;
      if (!(ls >> id >> r.lo >> r.hi >> r.lastS >> r.holdS >> r.pid) || r.hi < r.lo ||
          id.size() > 16)
        continue;
      char* end = nullptr;
      errno = 0;
      r.storeId = std::strtoull(id.c_str(), &end, 16);
      if (errno || !end || *end)
        continue;
      rec.ranges.push_back(r);
    }
  }
  return rec;
}

bool InUseRecord::load(IOBackend& io, const std::string& path, InUseRecord& out) {
  out = InUseRecord{};
  const int fd = io.open(path, O_RDONLY | O_CLOEXEC, 0);
  if (fd < 0)
    return errno == ENOENT;
  io.flock(fd, LOCK_SH); // a writer rewrites in place: read a whole version
  std::string text;
  const bool ok = readAll(io, fd, text);
  io.flock(fd, LOCK_UN);
  io.close(fd);
  if (ok)
    out = parse(text);
  return ok;
}

bool InUseRecord::note(IOBackend& io, const std::string& path, const InUseRange& r,
                       const InUseSettings* s, uint64_t windowS, bool release) {
  const auto slash = path.rfind('/');
  if (slash != std::string::npos)
    io.mkdirs(path.substr(0, slash), 0700);
  const int fd = io.open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0)
    return false;
  bool ok = io.flock(fd, LOCK_EX) == 0;
  std::string text;
  ok = ok && readAll(io, fd, text);
  if (ok) {
    InUseRecord rec = parse(text);
    const uint64_t now = std::max(r.lastS, r.holdS);
    if (rec.expired(now, windowS))
      rec = InUseRecord{}; // nothing in it is in use: as good as no record
    std::vector<InUseRange> kept;
    bool usedIn = !r.lastS, heldIn = !(r.holdS && r.pid) || release;
    for (auto e : rec.ranges) {
      const bool same = e.storeId == r.storeId && e.lo == r.lo && e.hi == r.hi;
      if (same && !e.pid && r.lastS) {
        e.lastS = std::max(e.lastS, r.lastS);
        usedIn = true;
      }
      if (same && e.pid && e.pid == r.pid) {
        if (release)
          continue; // its last close: no longer held by it
        e.holdS = std::max(e.holdS, r.holdS);
        heldIn = true;
      }
      if (live(e, now, windowS))
        kept.push_back(e);
    }
    if (!usedIn) {
      InUseRange u = r;
      u.holdS = 0;
      u.pid = 0;
      kept.push_back(u);
    }
    if (!heldIn) {
      InUseRange hld = r;
      hld.lastS = 0;
      kept.push_back(hld);
    }
    rec.ranges = std::move(kept);
    rec.highWater = std::max(rec.highWater, r.hi);
    if (s && !rec.hasSettings) {
      rec.settings = *s;
      rec.hasSettings = true;
    }
    const std::string out = rec.serialize();
    ok = io.pwriteFull(fd, out.data(), out.size(), 0) == static_cast<int64_t>(out.size()) &&
         io.ftruncate(fd, out.size()) == 0;
  }
  io.flock(fd, LOCK_UN);
  io.close(fd);
  return ok;
}

uint64_t InUseRecord::lastUseS() const {
  uint64_t t = 0;
  for (const auto& r : ranges)
    t = std::max({t, r.lastS, r.holdS});
  return t;
}

bool InUseRecord::inUse(uint64_t storeId, uint64_t lo, uint64_t hi, uint64_t nowS,
                        uint64_t windowS) const {
  for (const auto& r : ranges)
    if (r.storeId == storeId && r.lo < hi && lo < r.hi && live(r, nowS, windowS))
      return true;
  return false;
}

uint64_t InUseRecord::freeAtS(uint64_t storeId, uint64_t lo, uint64_t hi,
                              uint64_t windowS) const {
  uint64_t t = 0;
  for (const auto& r : ranges)
    if (r.storeId == storeId && r.lo < hi && lo < r.hi) {
      if (r.lastS)
        t = std::max(t, r.lastS + windowS);
      if (r.holdS)
        t = std::max(t, r.holdS + 2 * kHoldRefreshS);
    }
  return t;
}

bool InUseRecord::expired(uint64_t nowS, uint64_t windowS) const {
  for (const auto& r : ranges)
    if (live(r, nowS, windowS))
      return false;
  return true;
}

int InUseRecord::sweep(IOBackend& io, const std::string& cacheDir, uint64_t nowS,
                       uint64_t windowS) {
  const std::string root = cacheDir + "/inuse";
  std::vector<std::string> shards;
  if (io.listDir(root, shards) != 0)
    return 0;
  int removed = 0;
  for (const auto& sh : shards) {
    std::vector<std::string> names;
    if (sh.size() != 2 || io.listDir(root + "/" + sh, names) != 0)
      continue;
    for (const auto& n : names) {
      const std::string p = root + "/" + sh + "/" + n;
      // Under the record's own lock, so a hand-out noted meanwhile is never lost.
      const int fd = io.open(p, O_RDWR | O_CLOEXEC, 0);
      if (fd < 0)
        continue;
      if (io.flock(fd, LOCK_EX | LOCK_NB) == 0) {
        std::string text;
        if (readAll(io, fd, text) && parse(text).expired(nowS, windowS) &&
            io.unlink(p) == 0)
          ++removed;
        io.flock(fd, LOCK_UN);
      }
      io.close(fd);
    }
  }
  return removed;
}

} // namespace ucache
