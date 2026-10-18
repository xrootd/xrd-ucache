#include "LocalFileSystem.h"

#include "Executor.h"
#include "PluginSupport.h"
#include "UrlKey.h"

#include <XProtocol/XProtocol.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <cerrno>
#include <cstring>
#include <dirent.h>
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

XrdCl::XRootDStatus LocalFileSystem::Locate(const std::string& path, XrdCl::OpenFlags::Flags,
                                            XrdCl::ResponseHandler* handler, ucache::XrdTimeout) {
  // A leading '*' asks a server to locate the path everywhere it may be (the
  // client's deep locate sends one); it is not part of the path.
  const std::string p = localPathOf(!path.empty() && path[0] == '*' ? path.substr(1) : path);
  struct ::stat sb{};
  if (::stat(p.c_str(), &sb) != 0) {
    deliver(handler, failure(errno), nullptr);
    return XrdCl::XRootDStatus();
  }
  auto* info = new XrdCl::LocationInfo();
  info->Add(XrdCl::LocationInfo::Location(hostId_, XrdCl::LocationInfo::ServerOnline,
                                          XrdCl::LocationInfo::ReadWrite));
  deliver(handler, okStatus(), objectOf(info));
  return XrdCl::XRootDStatus();
}

XrdCl::XRootDStatus LocalFileSystem::Stat(const std::string& path,
                                          XrdCl::ResponseHandler* handler, ucache::XrdTimeout) {
  const std::string p = localPathOf(path);
  struct ::stat sb{};
  if (::stat(p.c_str(), &sb) != 0)
    deliver(handler, failure(errno), nullptr);
  else
    deliver(handler, okStatus(), objectOf(statInfoOf(p, sb)));
  return XrdCl::XRootDStatus();
}

XrdCl::XRootDStatus LocalFileSystem::DirList(const std::string& path,
                                             XrdCl::DirListFlags::Flags flags,
                                             XrdCl::ResponseHandler* handler, ucache::XrdTimeout) {
  if (flags & XrdCl::DirListFlags::Recursive)
    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported, 0,
                               "a recursive listing of a local directory is not supported");
  const std::string p = localPathOf(path);
  DIR* d = ::opendir(p.c_str());
  if (!d) {
    deliver(handler, failure(errno), nullptr);
    return XrdCl::XRootDStatus();
  }
  auto* list = new XrdCl::DirectoryList();
  list->SetParentName(p);
  const std::string dir = p == "/" ? p : p + "/";
  while (const struct ::dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..")
      continue;
    XrdCl::StatInfo* si = nullptr;
    if (flags & XrdCl::DirListFlags::Stat) {
      struct ::stat sb{};
      if (::stat((dir + name).c_str(), &sb) == 0)
        si = statInfoOf(dir + name, sb);
    }
    list->Add(new XrdCl::DirectoryList::ListEntry("localhost", name, si));
  }
  ::closedir(d);
  deliver(handler, okStatus(), objectOf(list));
  return XrdCl::XRootDStatus();
}

} // namespace ucache
