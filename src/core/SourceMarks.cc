#include "SourceMarks.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <fcntl.h>
#include <sstream>
#include <sys/file.h>

namespace ucache {

namespace {

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

std::string lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Under the file's exclusive lock: `edit` changes the marks read, which are then written back,
// marks long ended dropped and at most kSourceMarksMax kept.
template <typename Edit>
bool update(IOBackend& io, const std::string& path, uint64_t nowS, Edit edit) {
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
    std::vector<SourceMark> marks = parseSources(text);
    edit(marks);
    marks.erase(std::remove_if(marks.begin(), marks.end(),
                               [nowS](const SourceMark& m) {
                                 return m.count == 0 || m.untilS + kSourceMarkForgetS < nowS;
                               }),
                marks.end());
    if (marks.size() > kSourceMarksMax) {
      std::sort(marks.begin(), marks.end(),
                [](const SourceMark& a, const SourceMark& b) { return a.lastS > b.lastS; });
      marks.resize(kSourceMarksMax);
    }
    const std::string out = serializeSources(marks);
    ok = io.pwriteFull(fd, out.data(), out.size(), 0) == static_cast<int64_t>(out.size()) &&
         io.ftruncate(fd, out.size()) == 0;
  }
  io.flock(fd, LOCK_UN);
  io.close(fd);
  return ok;
}

} // namespace

uint64_t siteMarkSeconds(uint32_t count) {
  uint64_t s = kSiteMarkBaseS;
  for (uint32_t i = 1; i < count && s < kSiteMarkMaxS; ++i)
    s *= 2;
  return std::min(s, kSiteMarkMaxS);
}

std::string sourcesPath(const std::string& cacheDir) { return cacheDir + "/sources"; }

std::string encodeSite(const std::string& site) {
  std::string out;
  char buf[4];
  for (const char ch : site) {
    const auto c = static_cast<unsigned char>(ch);
    if (c <= ' ' || c > '~' || c == '%') {
      std::snprintf(buf, sizeof buf, "%%%02X", c);
      out += buf;
    } else {
      out += ch;
    }
  }
  return out.empty() ? std::string("-") : out;
}

std::string decodeSite(const std::string& text) {
  if (text == "-")
    return std::string();
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

std::vector<SourceMark> parseSources(const std::string& text) {
  std::vector<SourceMark> out;
  std::istringstream in(text);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (first) {
      first = false;
      if (line != "ucache-sources 1")
        return {}; // not a file this build knows
      continue;
    }
    std::istringstream ls(line);
    std::string tag, name, domain;
    SourceMark m;
    ls >> tag;
    if (tag != "server" ||
        !(ls >> m.server >> name >> domain >> m.count >> m.firstS >> m.lastS >> m.untilS) ||
        m.server.empty() || m.count == 0)
      continue;
    m.server = lower(m.server);
    m.siteName = decodeSite(name);
    m.domain = decodeSite(domain);
    auto same = std::find_if(out.begin(), out.end(),
                             [&](const SourceMark& x) { return x.server == m.server; });
    if (same == out.end())
      out.push_back(std::move(m));
    else if (m.lastS > same->lastS)
      *same = std::move(m);
  }
  return out;
}

std::string serializeSources(const std::vector<SourceMark>& marks) {
  std::ostringstream os;
  os << "ucache-sources 1\n";
  char buf[96];
  for (const auto& m : marks) {
    std::snprintf(buf, sizeof buf, " %u %" PRIu64 " %" PRIu64 " %" PRIu64 "\n", m.count, m.firstS,
                  m.lastS, m.untilS);
    os << "server " << m.server << ' ' << encodeSite(m.siteName) << ' ' << encodeSite(m.domain)
       << buf;
  }
  return os.str();
}

bool loadSources(IOBackend& io, const std::string& path, std::vector<SourceMark>& out) {
  out.clear();
  const int fd = io.open(path, O_RDONLY | O_CLOEXEC, 0);
  if (fd < 0)
    return errno == ENOENT;
  io.flock(fd, LOCK_SH); // a writer rewrites in place: read a whole version
  std::string text;
  const bool ok = readAll(io, fd, text);
  io.flock(fd, LOCK_UN);
  io.close(fd);
  if (ok)
    out = parseSources(text);
  return ok;
}

bool sameSiteIds(const std::string& nameA, const std::string& domainA, const std::string& nameB,
                 const std::string& domainB) {
  if (!nameA.empty() && !nameB.empty())
    return nameA == nameB;
  return !domainA.empty() && domainA == domainB;
}

bool markSource(IOBackend& io, const std::string& path, const std::string& server,
                const std::string& siteName, const std::string& domain, uint64_t nowS,
                SourceMark* out) {
  const std::string key = lower(server);
  return update(io, path, nowS, [&](std::vector<SourceMark>& marks) {
    auto it = std::find_if(marks.begin(), marks.end(),
                           [&](const SourceMark& m) { return m.server == key; });
    if (it == marks.end()) {
      marks.push_back(SourceMark{key, siteName, domain, 0, nowS, nowS, 0});
      it = marks.end() - 1;
    }
    if (!siteName.empty())
      it->siteName = siteName;
    if (!domain.empty())
      it->domain = domain;
    it->count = it->count + 1;
    if (it->count == 1)
      it->firstS = nowS;
    it->lastS = nowS;
    it->untilS = nowS + siteMarkSeconds(it->count);
    if (out)
      *out = *it;
  });
}

bool clearSource(IOBackend& io, const std::string& path, const std::string& server,
                 uint64_t nowS, bool* cleared) {
  const std::string key = lower(server);
  bool was = false;
  const bool ok = update(io, path, nowS, [&](std::vector<SourceMark>& marks) {
    for (auto& m : marks)
      if (m.server == key) {
        was = markLive(m, nowS);
        m.count = 0; // dropped on writing
      }
  });
  if (cleared)
    *cleared = was;
  return ok;
}

bool markLive(const SourceMark& m, uint64_t nowS) { return m.count > 0 && m.untilS > nowS; }

bool siteMarked(const std::vector<SourceMark>& marks, const std::string& siteName,
                const std::string& domain, uint64_t nowS, uint64_t* newestS) {
  int n = 0;
  uint64_t t = 0;
  for (const auto& m : marks)
    if (markLive(m, nowS) && sameSiteIds(m.siteName, m.domain, siteName, domain)) {
      ++n;
      t = std::max(t, m.lastS);
    }
  if (newestS)
    *newestS = n >= 2 ? t : 0;
  return n >= 2;
}

uint64_t markedAtS(const std::vector<SourceMark>& marks, const std::string& server,
                   const std::string& siteName, const std::string& domain, uint64_t nowS) {
  const std::string key = lower(server);
  uint64_t t = 0;
  for (const auto& m : marks)
    if (m.server == key && markLive(m, nowS))
      t = std::max(t, m.lastS);
  uint64_t site = 0;
  if (siteMarked(marks, siteName, domain, nowS, &site))
    t = std::max(t, site);
  return t;
}

} // namespace ucache
