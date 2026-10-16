// adaptive_federation = auto: what each server was found to be (a redirector or not), kept in the
// cache directory and shared by every process of the cache, so that a later process decides a
// file it opens from a trusted entry -- with no network -- as the process that asked the server
// did.
//
// One small text file per cache, <cacheDir>/roles, written whole to <cacheDir>/roles.new and
// renamed over it under an exclusive flock of <cacheDir>/roles.lock, read without a lock (the
// rename replaces it whole), never fsynced, as <cacheDir>/sources is: a process that dies leaves
// the old version or the new one, and a verdict lost costs one more question.
//
//   ucache-roles 1
//   role <host:port> <redirector|other> <saved at> <role>
//
// The time is wall-clock seconds. The role is what the server said (as the log says it),
// percent-encoded where it holds a space or a byte that is not printable ("-" for none). Only an
// answer is kept, never "no answer". A line that does not parse is skipped; a file whose first
// line is not the header reads as no verdicts; a file that cannot be read reads as none. A verdict
// saved more than kRoleSavedForgetS ago is dropped at the next write; at most kRolesSavedMax
// servers are kept, the newest first.
//
// Thread-safety: the functions may be called from any thread and any process; RoleSaved values
// are plain data.
#pragma once

#include "IOBackend.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ucache {

inline constexpr uint64_t kRoleSavedForgetS = 30ull * 86400;
inline constexpr size_t kRolesSavedMax = 4096;

struct RoleSaved {
  std::string server; // host:port, lower-cased
  bool redirector = false;
  uint64_t savedS = 0;
  std::string role; // what it said ("" for none)
};

std::string rolesPath(const std::string& cacheDir);
std::vector<RoleSaved> parseRoles(const std::string& text);
std::string serializeRoles(const std::vector<RoleSaved>& roles);
// The verdicts at `path`; no file, or one that cannot be read, reads as none.
std::vector<RoleSaved> loadRoles(IOBackend& io, const std::string& path);
// The verdict on `server` saved at `path`, if any.
bool findRole(IOBackend& io, const std::string& path, const std::string& server, RoleSaved& out);
// Saves the verdict on `server` (replacing an older one) under the file's exclusive lock, at
// `nowS`. False on an I/O failure or when the lock could not be had within about 100 ms.
bool saveRole(IOBackend& io, const std::string& path, const std::string& server, bool redirector,
              const std::string& role, uint64_t nowS);

} // namespace ucache
