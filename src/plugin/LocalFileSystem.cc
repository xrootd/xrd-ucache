#include "LocalFileSystem.h"

#include "Executor.h"
#include "PluginSupport.h"
#include "UrlKey.h"

#include <XProtocol/XProtocol.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace ucache {

namespace {

// What a server answers for a path the file system refuses: the error code a
// server sends (the one clients test for a path that does not exist), with the
// system's reason.
XrdCl::XRootDStatus* failure(int err) {
  return new XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errErrorResponse,
                                 static_cast<uint32_t>(XProtocol::mapError(err)),
                                 std::strerror(err));
}

XrdCl::StatInfo* statInfoOf(const std::string& path, const struct ::stat& sb) {
  uint32_t flags = 0;
  if (S_ISDIR(sb.st_mode))
    flags |= XrdCl::StatInfo::IsDir;
  else if (!S_ISREG(sb.st_mode))
    flags |= XrdCl::StatInfo::Other;
  if (::access(path.c_str(), R_OK) == 0)
    flags |= XrdCl::StatInfo::IsReadable;
  if (::access(path.c_str(), W_OK) == 0)
    flags |= XrdCl::StatInfo::IsWritable;
  if (sb.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))
    flags |= XrdCl::StatInfo::XBitSet;
  return new XrdCl::StatInfo(std::to_string(static_cast<unsigned long long>(sb.st_ino)),
                             static_cast<uint64_t>(sb.st_size), flags,
                             static_cast<uint64_t>(sb.st_mtime));
}

void deliver(XrdCl::ResponseHandler* h, XrdCl::XRootDStatus* status,
             XrdCl::AnyObject* response) {
  if (!h) {
    delete status;
    delete response;
    return;
  }
  Executor::instance().post([h, status, response] { complete(h, status, response); });
}

template <typename T> XrdCl::AnyObject* objectOf(T* value) {
  auto* o = new XrdCl::AnyObject();
  o->Set(value);
  return o;
}

} // namespace

LocalFileSystem::LocalFileSystem(const std::string& url) {
  const XrdCl::URL u(url);
  hostId_ = std::string(kLocalFileHost) + ":" + std::to_string(u.IsValid() ? u.GetPort() : 1094);
}

// Each request answers or fails as a server would, and never throws into the
// client: one it cannot even set up (no memory) is refused at once.
XrdCl::XRootDStatus LocalFileSystem::Locate(const std::string& path, XrdCl::OpenFlags::Flags,
                                            XrdCl::ResponseHandler* handler,
                                            ucache::XrdTimeout) try {
  // A leading '*' asks a server to locate the path everywhere it may be (the
  // client's deep locate sends one); it is not part of the path.
  const std::string p = localPathOf(!path.empty() && path[0] == '*' ? path.substr(1) : path);
  struct ::stat sb{};
  if (::stat(p.c_str(), &sb) != 0) {
    deliver(handler, failure(errno), nullptr);
    return XrdCl::XRootDStatus();
  }
  auto info = std::make_unique<XrdCl::LocationInfo>();
  info->Add(XrdCl::LocationInfo::Location(hostId_, XrdCl::LocationInfo::ServerOnline,
                                          XrdCl::LocationInfo::ReadWrite));
  deliver(handler, okStatus(), objectOf(info.release()));
  return XrdCl::XRootDStatus();
} catch (...) {
  return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errInternal);
}

XrdCl::XRootDStatus LocalFileSystem::Stat(const std::string& path,
                                          XrdCl::ResponseHandler* handler,
                                          ucache::XrdTimeout) try {
  const std::string p = localPathOf(path);
  struct ::stat sb{};
  if (::stat(p.c_str(), &sb) != 0)
    deliver(handler, failure(errno), nullptr);
  else
    deliver(handler, okStatus(), objectOf(statInfoOf(p, sb)));
  return XrdCl::XRootDStatus();
} catch (...) {
  return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errInternal);
}

XrdCl::XRootDStatus LocalFileSystem::DirList(const std::string& path,
                                             XrdCl::DirListFlags::Flags flags,
                                             XrdCl::ResponseHandler* handler,
                                             ucache::XrdTimeout) try {
  if (flags & XrdCl::DirListFlags::Recursive)
    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported, 0,
                               "a recursive listing of a local directory is not supported");
  const std::string p = localPathOf(path);
  std::unique_ptr<DIR, int (*)(DIR*)> d(::opendir(p.c_str()), &::closedir);
  if (!d) {
    deliver(handler, failure(errno), nullptr);
    return XrdCl::XRootDStatus();
  }
  auto list = std::make_unique<XrdCl::DirectoryList>();
  list->SetParentName(p);
  const std::string dir = p == "/" ? p : p + "/";
  for (;;) {
    errno = 0;
    const struct ::dirent* e = ::readdir(d.get());
    if (!e) {
      if (errno != 0) { // not the end: a listing cut short is no listing
        deliver(handler, failure(errno), nullptr);
        return XrdCl::XRootDStatus();
      }
      break;
    }
    const std::string name = e->d_name;
    if (name == "." || name == "..")
      continue;
    XrdCl::StatInfo* si = nullptr;
    if (flags & XrdCl::DirListFlags::Stat) {
      // As a server lists: every entry with its stat. A link to nothing is
      // listed as the link; an entry gone since the directory was read is
      // left out; any other failure (a directory that may be read but not
      // searched, an I/O error) fails the listing.
      struct ::stat sb{};
      const std::string full = dir + name;
      if (::stat(full.c_str(), &sb) != 0 && ::lstat(full.c_str(), &sb) != 0) {
        if (errno == ENOENT)
          continue;
        deliver(handler, failure(errno), nullptr);
        return XrdCl::XRootDStatus();
      }
      si = statInfoOf(full, sb);
    }
    // The entry's host is the host itself, never a server's address.
    list->Add(new XrdCl::DirectoryList::ListEntry(hostId_, name, si));
  }
  deliver(handler, okStatus(), objectOf(list.release()));
  return XrdCl::XRootDStatus();
} catch (...) {
  return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errInternal);
}

} // namespace ucache
