#include "ServerVerdicts.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <fcntl.h>
#include <sstream>
#include <sys/file.h>
#include <unistd.h>

namespace ucache {

namespace {

std::string lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string encode(const std::string& s) {
  std::string out;
  char buf[4];
  for (const char ch : s) {
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

std::string decode(const std::string& text) {
  if (text == "-")
    return std::string();
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() &&
        std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

bool readText(IOBackend& io, const std::string& path, std::string& text) {
  text.clear();
  const int fd = io.open(path, O_RDONLY | O_CLOEXEC, 0);
  if (fd < 0)
    return errno == ENOENT;
  struct ::stat st {};
  bool ok = io.fstat(fd, &st) == 0;
  if (ok && st.st_size > 0) {
    text.resize(static_cast<size_t>(std::min<off_t>(st.st_size, 1 << 20)));
    const int64_t n = io.preadFull(fd, text.data(), text.size(), 0);
    ok = n >= 0;
    text.resize(ok ? static_cast<size_t>(n) : 0);
  }
  io.close(fd);
  return ok;
}

} // namespace

std::string rolesPath(const std::string& cacheDir) { return cacheDir + "/roles"; }

std::vector<RoleSaved> parseRoles(const std::string& text) {
  std::vector<RoleSaved> out;
  std::istringstream in(text);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (first) {
      first = false;
      if (line != "ucache-roles 1")
        return {}; // not a file this build knows
      continue;
    }
    std::istringstream ls(line);
    std::string tag, kind, role;
    RoleSaved r;
    if (!(ls >> tag >> r.server >> kind >> r.savedS >> role) || tag != "role" || r.server.empty() ||
        (kind != "redirector" && kind != "other"))
      continue;
    r.server = lower(r.server);
    r.redirector = kind == "redirector";
    r.role = decode(role);
    auto same = std::find_if(out.begin(), out.end(),
                             [&](const RoleSaved& x) { return x.server == r.server; });
    if (same == out.end())
      out.push_back(std::move(r));
    else if (r.savedS > same->savedS)
      *same = std::move(r);
  }
  return out;
}

std::string serializeRoles(const std::vector<RoleSaved>& roles) {
  std::ostringstream os;
  os << "ucache-roles 1\n";
  for (const auto& r : roles)
    os << "role " << r.server << ' ' << (r.redirector ? "redirector" : "other") << ' ' << r.savedS
       << ' ' << encode(r.role) << '\n';
  return os.str();
}

std::vector<RoleSaved> loadRoles(IOBackend& io, const std::string& path) {
  std::string text;
  if (!readText(io, path, text))
    return {};
  return parseRoles(text);
}

bool findRole(IOBackend& io, const std::string& path, const std::string& server, RoleSaved& out) {
  const std::string key = lower(server);
  for (auto& r : loadRoles(io, path))
    if (r.server == key) {
      out = std::move(r);
      return true;
    }
  return false;
}

bool saveRole(IOBackend& io, const std::string& path, const std::string& server, bool redirector,
              const std::string& role, uint64_t nowS) {
  const auto slash = path.rfind('/');
  if (slash != std::string::npos)
    io.mkdirs(path.substr(0, slash), 0700);
  const int lfd = io.open(path + ".lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lfd < 0)
    return false;
  // Never blocked for long, as for <dir>/sources: a lost verdict costs one more question.
  bool ok = false;
  for (int i = 0; i < 20 && !ok; ++i) {
    ok = io.flock(lfd, LOCK_EX | LOCK_NB) == 0;
    if (!ok)
      ::usleep(5000);
  }
  std::string text;
  ok = ok && readText(io, path, text);
  if (ok) {
    std::vector<RoleSaved> roles = parseRoles(text);
    const std::string key = lower(server);
    roles.erase(std::remove_if(roles.begin(), roles.end(),
                               [&](const RoleSaved& r) {
                                 return r.server == key || r.savedS + kRoleSavedForgetS < nowS;
                               }),
                roles.end());
    roles.push_back(RoleSaved{key, redirector, nowS, role});
    if (roles.size() > kRolesSavedMax) {
      std::sort(roles.begin(), roles.end(),
                [](const RoleSaved& a, const RoleSaved& b) { return a.savedS > b.savedS; });
      roles.resize(kRolesSavedMax);
    }
    const std::string body = serializeRoles(roles);
    const std::string fresh = path + ".new";
    const int fd = io.open(fresh, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    ok = fd >= 0;
    if (ok) {
      ok = io.pwriteFull(fd, body.data(), body.size(), 0) == static_cast<int64_t>(body.size());
      io.close(fd);
      ok = ok && io.rename(fresh, path) == 0;
    }
  }
  io.flock(lfd, LOCK_UN);
  io.close(lfd);
  return ok;
}

} // namespace ucache
