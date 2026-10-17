// The servers that failed in a way of their own (failover): site-fault memory, shared by
// every process of a cache.
//
// A server is marked when it stalled while other sites answered normally at the same time, or when
// it answered with an error of its own (its storage, its file system, an internal fault, overload);
// never for authorization, TLS, a name that does not resolve, or a timeout the job chose. A mark
// lasts kSiteMarkBaseS, doubled at each repeat up to kSiteMarkMaxS, and is cleared at the server's
// first success. A site counts as marked while two or more of its servers are. A mark only orders:
// a copy at a marked server or site is tried after every copy that is not, and when every copy is
// marked all are tried, the oldest failure first; nothing is ever left out for a mark.
//
// One small text file per cache, <cacheDir>/sources, written whole to <cacheDir>/sources.new and
// renamed over it under an exclusive flock of <cacheDir>/sources.lock, read without a lock (the
// rename replaces it whole), never fsynced: a process that dies leaves the old version or the new
// one, and a mark lost in a crash of the machine costs one more failover, which marks again.
//
//   ucache-sources 1
//   server <host:port> <site name> <domain> <count> <first failure> <last failure> <marked until>
//
// Times are wall-clock seconds. The site name is the name the server gave ("-" while none is
// known); the domain, its domain as before site names (the server itself for one that never
// answered). Two servers are of one site when both names are known and agree, else when their
// domains agree (sameSiteIds). A name or domain holding a space or a byte that is not printable is
// percent-encoded. A line that does not parse is skipped; a file whose first line is not the
// header reads as no marks. A server whose mark ended more than kSourceMarkForgetS ago is
// dropped at the next write (its count with it); at most kSourceMarksMax servers are kept, the
// oldest failure dropped first.
//
// Thread-safety: the static functions may be called from any thread and any process; SourceMark
// values are plain data.
#pragma once

#include "IOBackend.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ucache {

inline constexpr uint64_t kSiteMarkBaseS = 600;   // 10 minutes
inline constexpr uint64_t kSiteMarkMaxS = 3600;   // 1 hour
inline constexpr uint64_t kSourceMarkForgetS = 86400;
inline constexpr size_t kSourceMarksMax = 4096;
// A clock that went back: a mark whose end lies further ahead than any mark lasts
// (kSiteMarkMaxS), by more than this slack, is taken as ended (markLive), and a copy fault whose
// proof lies ahead by more than it is no longer live (InUse). Never lengthened by the clock.
inline constexpr uint64_t kMarkClockSlackS = 60;

struct SourceMark {
  std::string server;   // host:port, lower-cased
  std::string siteName; // the name the server gave, "" while none is known
  std::string domain;   // its domain as before site names, the server itself if it never answered
  uint32_t count = 0; // marks since the last success
  uint64_t firstS = 0, lastS = 0, untilS = 0;
};

// How long the `count`-th mark in a row lasts: kSiteMarkBaseS, doubled at each repeat, at most
// kSiteMarkMaxS.
uint64_t siteMarkSeconds(uint32_t count);

std::string sourcesPath(const std::string& cacheDir);
std::vector<SourceMark> parseSources(const std::string& text);
std::string serializeSources(const std::vector<SourceMark>& marks);

// The marks at `path`; no file reads as none. False only when one exists and cannot be read.
bool loadSources(IOBackend& io, const std::string& path, std::vector<SourceMark>& out);

// Two servers are of one site: by names when both are known, else by domains.
bool sameSiteIds(const std::string& nameA, const std::string& domainA, const std::string& nameB,
                 const std::string& domainB);

// `server` (its site name, "" for none known, and its domain) failed at `nowS`: its mark is
// renewed under the file's exclusive lock, one more in its row (until = now +
// siteMarkSeconds(count)). `out`, when given, gets the mark as written. False on an I/O failure.
bool markSource(IOBackend& io, const std::string& path, const std::string& server,
                const std::string& siteName, const std::string& domain, uint64_t nowS,
                SourceMark* out = nullptr);
// `server` succeeded: its mark and its row are cleared. `cleared` says whether it had a live
// mark. False on an I/O failure.
bool clearSource(IOBackend& io, const std::string& path, const std::string& server,
                 uint64_t nowS, bool* cleared = nullptr);

// Live: failed since its last success, its end not reached, and that end no further ahead than
// a mark lasts (kSiteMarkMaxS, with kMarkClockSlackS): a clock that went back never lengthens it.
bool markLive(const SourceMark& m, uint64_t nowS);
// The site of a server (its site name, "" for none known, and its domain) is marked: two or more
// of the servers of that site hold live marks. `newestS`, when given, gets their newest failure.
bool siteMarked(const std::vector<SourceMark>& marks, const std::string& siteName,
                const std::string& domain, uint64_t nowS, uint64_t* newestS = nullptr);
// When the copy at `server` (its site name and domain) was last failed for a mark that still
// holds: its own server's live mark, or, when its site is marked, the newest failure of the site's
// marked servers; 0 when neither holds.
uint64_t markedAtS(const std::vector<SourceMark>& marks, const std::string& server,
                   const std::string& siteName, const std::string& domain, uint64_t nowS);
// The site a mark is said as: its name, else its domain.
inline const std::string& markSite(const SourceMark& m) {
  return m.siteName.empty() ? m.domain : m.siteName;
}

// A site name as the file holds it: percent-encoded where it has a space, '%' or a byte that is
// not printable, and back.
std::string encodeSite(const std::string& site);
std::string decodeSite(const std::string& text);

} // namespace ucache
