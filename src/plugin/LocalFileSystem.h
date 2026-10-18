// The file-system requests of the local host, root://local (UrlKey.h): a stat,
// a directory listing and a locate, answered from this machine's file system.
// A program may ask one before or beside its open -- uproot's default
// transport takes a file's size from a stat, and ROOT lists a directory for a
// wildcard in a TChain, which the client does by locating the directory first
// and then listing it where it was found -- and without this object the
// client would send it to a server named `local`. Every other request is
// refused at once (not implemented), and none of them ever reaches a server.
//
// Answers are delivered from the executor, never inside the call, as the
// plug-in's other synthesized completions are, with the empty host list every
// one of them carries (PluginSupport.h).
//
// Thread-safety: stateless; any number of requests from any threads.
#pragma once

#include "XrdClTimeout.h"

#include <XrdCl/XrdClPlugInInterface.hh>

#include <string>

namespace ucache {

class LocalFileSystem : public XrdCl::FileSystemPlugIn {
 public:
  // `url`: the URL the client made this object for (root://local:1094/...).
  explicit LocalFileSystem(const std::string& url);

  XrdCl::XRootDStatus Stat(const std::string& path, XrdCl::ResponseHandler* handler,
                           ucache::XrdTimeout timeout) override;
  // A path that exists is located on the host itself, as a data server that
  // holds it: whatever the client asks there next comes back to this object's
  // kind. A path that does not exist is not found.
  XrdCl::XRootDStatus Locate(const std::string& path, XrdCl::OpenFlags::Flags flags,
                             XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  // Flags: Stat is honoured; Recursive is refused (not supported); the others
  // (Locate, Merge, Chunked, Zip, Cksm) concern servers and change nothing here.
  XrdCl::XRootDStatus DirList(const std::string& path, XrdCl::DirListFlags::Flags flags,
                              XrdCl::ResponseHandler* handler,
                              ucache::XrdTimeout timeout) override;

 private:
  std::string hostId_; // "local:<port>", the address a locate answers with
};

} // namespace ucache
