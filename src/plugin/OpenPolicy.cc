#include "OpenPolicy.h"

#include "ReadRounding.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ucache {

namespace {
using namespace xrdwire;

bool isIpv4(const std::string& h) {
  int dots = 0, digits = 0;
  for (char c : h) {
    if (c == '.') {
      if (digits == 0)
        return false;
      ++dots;
      digits = 0;
    } else if (c >= '0' && c <= '9') {
      if (++digits > 3)
        return false;
    } else {
      return false;
    }
  }
  return dots == 3 && digits > 0;
}

// An IPv6 address, bracketed as in a URL or bare: no host name has a colon.
bool isIpv6(const std::string& h) {
  return h.find(':') != std::string::npos || (!h.empty() && h[0] == '[');
}
} // namespace

Hop makeHop(const std::string& host, int port, uint32_t flags, bool loadBalancer,
            const std::string& query) {
  Hop h;
  h.host = host;
  h.port = port;
  h.flags = flags;
  h.loadBalancer = loadBalancer;
  // Parameter names only: "cap.msg=...&cap.sym=...&mgm.id=..." on the hop to an
  // EOS data server.
  size_t at = (!query.empty() && query[0] == '?') ? 1 : 0;
  while (at < query.size()) {
    size_t end = query.find('&', at);
    if (end == std::string::npos)
      end = query.size();
    const size_t eq = query.find('=', at);
    const size_t nameEnd = (eq == std::string::npos || eq > end) ? end : eq;
    const std::string name = query.substr(at, nameEnd - at);
    if (name == "cap.msg" || name == "cap.sym")
      h.eosFst = true;
    at = end + 1;
  }
  return h;
}

ChainView readChain(const std::vector<Hop>& hops) {
  ChainView v;
  v.hops = hops.size();
  v.last = static_cast<int>(hops.size()) - 1;
  for (size_t i = 0; i < hops.size(); ++i) {
    const uint32_t f = hops[i].flags;
    if (f & kIsManager) {
      v.viaRedirector = true;
      if (v.regional < 0 && !(f & kAttrMeta))
        v.regional = static_cast<int>(i);
    } else if ((f & kIsServer) && i + 1 < hops.size()) {
      v.effective = static_cast<int>(i); // a data server that sent the open on
    }
  }
  if (v.effective < 0 && v.last >= 0 && !(hops[static_cast<size_t>(v.last)].flags & kIsManager))
    v.effective = v.last;
  return v;
}

std::string chainText(const std::vector<Hop>& hops) {
  std::string out;
  char flags[16];
  for (const Hop& h : hops) {
    if (!out.empty())
      out += ',';
    std::snprintf(flags, sizeof flags, "/0x%x", h.flags);
    out += h.host;
    out += ':';
    out += std::to_string(h.port);
    out += flags;
    if (h.loadBalancer)
      out += '*';
    if (h.eosFst)
      out += '^';
  }
  return out;
}

std::string siteOf(const std::string& host) {
  if (host.empty() || isIpv4(host) || isIpv6(host))
    return host;
  std::string h = host;
  for (char& c : h)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  while (!h.empty() && h.back() == '.') // a fully qualified name's root dot
    h.pop_back();
  const size_t dot = h.find('.');
  if (dot == std::string::npos || dot + 1 >= h.size())
    return h;
  return h.substr(dot + 1);
}

FailureClass classifyFailure(int code, int errNo) {
  switch (code) {
  case 0:
    return FailureClass::kNone;
  case kErrSocketError:
  case kErrSocketTimeout:
  case kErrSocketDisconnected:
  case kErrStreamDisconnect:
  case kErrConnectionError:
  case kErrInvalidSession:
  case kErrTlsError:
  case kErrAuthFailed:
    return FailureClass::kClient;
  case kErrOperationExpired:
    return FailureClass::kExpired;
  case kErrRedirectLimit:
    return FailureClass::kSite;
  case kErrNotFound:
    return FailureClass::kNotFound;
  case kErrDataError:
    return FailureClass::kData;
  case kErrErrorResponse:
    switch (errNo) {
    case kXrNotAuthorized:
      return FailureClass::kClient;
    case kXrNotFound:
      return FailureClass::kNotFound;
    case kXrFileLocked:
    case kXrFSError:
    case kXrIOError:
    case kXrServerError:
    case kXrCancelled:
    case kXrItExists:
    case kXrOverloaded:
      return FailureClass::kSite;
    default: // kXrNoServer among them: see the header
      return FailureClass::kOther;
    }
  default:
    return FailureClass::kOther;
  }
}

const char* failureClassName(FailureClass c) {
  switch (c) {
  case FailureClass::kNone:
    return "none";
  case FailureClass::kClient:
    return "client";
  case FailureClass::kSite:
    return "site";
  case FailureClass::kNotFound:
    return "notfound";
  case FailureClass::kData:
    return "data";
  case FailureClass::kExpired:
    return "expired";
  case FailureClass::kOther:
    break;
  }
  return "other";
}

const char* openRouteName(OpenRoute r) {
  switch (r) {
  case OpenRoute::kRelay:
    return "relay";
  case OpenRoute::kLazy:
    return "lazy";
  case OpenRoute::kDegraded:
    return "degraded";
  case OpenRoute::kFork:
    return "fork";
  case OpenRoute::kCandidate:
    return "candidate";
  case OpenRoute::kRead:
    return "read";
  case OpenRoute::kFailover:
    break;
  }
  return "failover";
}

namespace {
// Appends c, escaped, to out unless that would pass `max` bytes; false then.
bool appendEscaped(std::string& out, char c, size_t max) {
  char b[8];
  size_t n = 1;
  if (c == '"' || c == '\\') {
    b[0] = '\\';
    b[1] = c;
    n = 2;
  } else if (static_cast<unsigned char>(c) < 0x20) {
    std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
    n = 6;
  } else {
    b[0] = c;
  }
  if (out.size() + n > max)
    return false;
  out.append(b, n);
  return true;
}
} // namespace

int openDeadlineS(int readerTimeoutS, int deadlineS) {
  const int d = deadlineS > 0 ? deadlineS : kOpenDeadlineS;
  return readerTimeoutS > 0 && readerTimeoutS < d ? readerTimeoutS : d;
}

OpenStep stepAfter(const ChainView& v, int code, int errNo, bool sizeDiffers, bool offline,
                   bool excluded, bool repeats, bool regionalKnown) {
  if (!v.viaRedirector) // a direct open has nowhere else to go
    return OpenStep::kFinal;
  if (code == 0)
    return sizeDiffers || offline ? OpenStep::kFailOver
           : repeats              ? OpenStep::kLocate
                                  : OpenStep::kFinal;
  const bool first = v.hops <= 1; // the redirector answered (or failed) itself
  const OpenStep elsewhere = first ? OpenStep::kFinal : OpenStep::kFailOver;
  // Not found by the redirector itself, with nothing of ours hiding a copy: no
  // site has it. Otherwise the client's own retry at its load balancer found
  // none either, though a listed copy may be missing or hidden.
  const OpenStep notFound = first && !excluded ? OpenStep::kFinal : OpenStep::kLocate;
  switch (code) {
  case kErrRedirectLimit:
    return OpenStep::kFailOver;
  case kErrNotFound:
    return notFound;
  case kErrOperationExpired: // the server stalled: the retry would meet its site's other server
    return regionalKnown && !first ? OpenStep::kLocate : elsewhere;
  case kErrSocketTimeout:
  case kErrConnectionError:
    return elsewhere;
  case kErrErrorResponse:
    switch (errNo) {
    case kXrNotFound:
      return notFound;
    case kXrNotAuthorized: // the client retries it only at a global redirector
    case kXrFileLocked:
    case kXrFSError:
    case kXrIOError:
    case kXrServerError:
    case kXrNoServer:
    case kXrCancelled:
    case kXrItExists:
    case kXrOverloaded:
      return elsewhere;
    default:
      return OpenStep::kFinal;
    }
  default:
    return OpenStep::kFinal;
  }
}

OpenStep stepAfterHeaderRead(const ChainView& v, int code, int errNo, bool excluded,
                             bool regionalKnown) {
  const OpenStep s = stepAfter(v, code, errNo, false, false, excluded, false, regionalKnown);
  return s == OpenStep::kFinal && v.viaRedirector && v.hops > 1 ? OpenStep::kFailOver : s;
}

bool clientRecoversRead(int code) {
  switch (code) {
  case kErrInternal:
  case kErrSocketError:
  case kErrSocketTimeout:
  case kErrInvalidSession:
  case kErrTlsError:
  case kErrOperationInterrupted:
  case kErrRedirect:
    return true;
  default:
    return false;
  }
}

bool openOnceMoreAfterCheckRead(bool headerFailed, int sourceAttempt, int code) {
  return headerFailed && sourceAttempt == 1 && clientRecoversRead(code);
}

bool rootNamed(const std::string& path) {
  const size_t end = path.find('?');
  const size_t n = end == std::string::npos ? path.size() : end;
  if (n < 5)
    return false;
  static const char kExt[] = ".root";
  for (size_t i = 0; i < 5; ++i)
    if (std::tolower(static_cast<unsigned char>(path[n - 5 + i])) != kExt[i])
      return false;
  return true;
}

bool emptyRootCopy(const std::string& path, long long size) {
  return size == 0 && rootNamed(path);
}

size_t rootHeaderBytes(long long size) {
  return size <= 0 ? 0 : static_cast<size_t>(std::min<long long>(size, kRootHeaderBytes));
}

namespace {
uint64_t bigEndian(const unsigned char* b, size_t at, int bytes) {
  uint64_t v = 0;
  for (int i = 0; i < bytes; ++i)
    v = v << 8 | b[at + static_cast<size_t>(i)];
  return v;
}
// Where the end's field of a header with these first bytes ends: 16, or 20 for a 64-bit end;
// 0 when the version is not there yet. The version is a signed 32-bit integer, as TFile::Init
// reads it (`fVersion < 1000000`: 32-bit fields), so one whose top bit is set is negative and
// marks 32-bit fields, as for ROOT.
size_t endFieldEnd(const unsigned char* b, size_t n) {
  if (n < 8)
    return 0;
  const int32_t version = static_cast<int32_t>(static_cast<uint32_t>(bigEndian(b, 4, 4)));
  return version >= 1000000 ? 20 : 16;
}
} // namespace

long long rootFileEnd(const unsigned char* b, size_t n) {
  const size_t need = endFieldEnd(b, n);
  if (need == 0 || n < need || std::memcmp(b, "root", 4) != 0)
    return -1;
  const uint64_t end = need == 20 ? bigEndian(b, 12, 8) : bigEndian(b, 12, 4);
  return end > static_cast<uint64_t>(LLONG_MAX) ? LLONG_MAX : static_cast<long long>(end);
}

namespace {
std::string num(long long v) {
  return std::to_string(v);
}
void refuse(RootHeaderVerdict& v, const std::string& why) {
  v.kind = RootHeaderVerdict::kBad;
  v.why = "size " + num(v.size) + ": " + why;
  v.next = RootHeaderVerdict::kNone;
}
// Whether the record of `len` bytes at `at` lies from `from` to `to`; never overflows.
bool within(int64_t from, int64_t to, int64_t at, int64_t len) {
  return at >= from && len >= 0 && at <= to && len <= to - at;
}
std::string span(int64_t from, int64_t to) {
  return "outside the file's records (" + num(from) + " to " + num(to) + ")";
}
// Where the list of top-level objects and the objects it lists may end: the copy's size, which is
// the header's end when the two are equal (an end past the size is refused before). The list
// and the directory's record that places it are rewritten by ROOT without the header
// (TFile::Save and TDirectory::SaveSelf write the list and that record, not the header): a file
// saved so and then written on, or whose writer stopped there, holds its list and its newest
// objects past the end its header states. ROOT reads such a file normally (TFile::Init takes
// it as complete, and TDirectoryFile::ReadKeys bounds each object by the file's size). The class
// descriptions are bounded the same way: ROOT reads them (TFile::GetStreamerInfoListImpl) within
// the file's size, whatever the header's end says.
int64_t listBound(const RootHeaderVerdict& v) {
  return std::max<int64_t>(v.header.end, v.size);
}
// The least position TDirectoryFile::ReadKeys takes for an object it lists, and for its directory:
// it stops reading the list at an entry below it ("reading illegal key").
constexpr int64_t kRootKeyMinPos = 64;
// Whether `at` is the top directory: where its record lies by the header (fBEGIN, where TFile::Init
// reads it and where ROOT writes it), or by its own record (fSeekDir, which TFile::Init takes as
// the directory's position once it has read the record, and which ROOT writes as the directory of
// the objects it adds to the file). The two agree in every file ROOT writes; ROOT itself compares
// an object's directory with neither, so a copy whose record names another position than fBEGIN,
// which ROOT reads, is not refused for it.
bool topDirectory(const RootHeaderVerdict& v, int64_t at) {
  return at == v.header.begin || at == v.dir.seekDir;
}
// The top directory's record, read: ROOT's own refusal of a name record out of range, then whether
// the header was completed, then where the records it places lie (TFile::Init).
void judgeDirRecord(RootHeaderVerdict& v, const RootDirRecord& d) {
  const RootFileHeader& h = v.header;
  v.dir = d;
  if (d.nbytesName < 10 || d.nbytesName > 10000)
    return refuse(v, "its top directory's record gives its name record " + num(d.nbytesName) +
                         " bytes; ROOT reads 10 to 10000");
  if (d.seekKeys <= h.begin) { // never completed: no claim on what follows, nothing confirmed
    v.kind = RootHeaderVerdict::kNoClaim;
    v.next = RootHeaderVerdict::kNone;
    return;
  }
  const int64_t to = listBound(v);
  if (!within(h.begin, to, d.seekKeys, d.nbytesKeys) || d.nbytesKeys <= 0)
    return refuse(v, "its list of top-level objects lies at " + num(d.seekKeys) + ", " +
                         num(d.nbytesKeys) + " bytes, " + span(h.begin, to));
  // The class descriptions, which TFile::Init reads when their position is past the first record
  // (one at or before it is not read, and makes no claim), within the copy's size: a copy ROOT
  // cannot read them from is one it cannot open. The list of free space is not looked at: ROOT
  // reads it only for a file opened to be written, and ignores it when it is out of bounds.
  if (h.seekInfo > h.begin && !within(h.begin, to, h.seekInfo, h.nbytesInfo))
    return refuse(v, "its class descriptions lie at " + num(h.seekInfo) + ", " + num(h.nbytesInfo) +
                         " bytes, " + span(h.begin, to));
  if (static_cast<uint32_t>(d.nbytesKeys) > kRootKeysListMaxBytes) { // not read: the verdict stands
    v.next = RootHeaderVerdict::kNone;
    return;
  }
  v.next = RootHeaderVerdict::kKeysList;
  v.nextAt = d.seekKeys;
  v.nextLen = static_cast<uint32_t>(d.nbytesKeys);
}
// The list of top-level objects, read whole (TDirectoryFile::ReadKeys): its own label (its
// position and its directory), the count, and each object's label. The label's length is not
// compared with the directory's record: ROOT reads the record's length of bytes and never the
// label's, and a writer stopped inside a save (TDirectory::SaveSelf: the new list written, the
// directory's record not yet) leaves the old list, which ROOT reads normally, with ROOT's mark
// of a freed record in that field.
void judgeKeysList(RootHeaderVerdict& v, const unsigned char* b, size_t n) {
  const RootFileHeader& h = v.header;
  const std::string list = "its list of top-level objects at " + num(v.dir.seekKeys);
  size_t at = 0;
  RootKeyLabel own;
  if (!parseRootKeyLabel(b, n, at, own))
    return refuse(v, list + " does not hold its own label");
  if (own.seekKey != v.dir.seekKeys)
    return refuse(v, list + " is labelled as lying at " + num(own.seekKey));
  if (!topDirectory(v, own.seekPdir))
    return refuse(v, list + " is labelled with the directory at " + num(own.seekPdir) +
                         ", not the top directory at " + num(h.begin));
  if (n < 4 || at > n - 4)
    return refuse(v, list + " ends before its count of objects");
  const int32_t count = static_cast<int32_t>(static_cast<uint32_t>(b[at]) << 24 |
                                             static_cast<uint32_t>(b[at + 1]) << 16 |
                                             static_cast<uint32_t>(b[at + 2]) << 8 | b[at + 3]);
  at += 4;
  if (count < 0)
    return refuse(v, list + " gives a count of " + num(count) + " objects");
  const int64_t to = listBound(v);
  for (int32_t i = 0; i < count; ++i) {
    RootKeyLabel k;
    if (!parseRootKeyLabel(b, n, at, k))
      return refuse(v, list + " ends inside its entry " + num(i + 1) + " of " + num(count));
    // An object ROOT can read: inside the copy, from the first record on and never below
    // ReadKeys' own least position, and of a length (TKey::ReadFile reads that many bytes; ROOT
    // writes the key's own header and its data in it, never less than one byte).
    const int64_t from = std::max(h.begin, kRootKeyMinPos);
    if (k.nbytes <= 0 || !within(from, to, k.seekKey, k.nbytes))
      return refuse(v, "its list of top-level objects names an object at " + num(k.seekKey) + ", " +
                           num(k.nbytes) + " bytes, " + span(from, to));
    if (!topDirectory(v, k.seekPdir))
      return refuse(v, "its list of top-level objects names an object at " + num(k.seekKey) +
                           " in the directory at " + num(k.seekPdir) +
                           ", not the top directory at " + num(h.begin));
    if (k.seekPdir < kRootKeyMinPos) // the top directory, but ROOT stops reading the list there
      return refuse(v, "its list of top-level objects names an object at " + num(k.seekKey) +
                           " in the directory at " + num(k.seekPdir) + ", below the " +
                           num(kRootKeyMinPos) + " ROOT reads a list from");
  }
  v.next = RootHeaderVerdict::kNone;
}
} // namespace

RootHeaderVerdict rootHeaderVerdict(const unsigned char* b, size_t n, long long size) {
  RootHeaderVerdict v;
  v.size = size;
  const size_t want = rootHeaderBytes(size);
  if (n < want) {
    refuse(v,
           "a read of its first " + std::to_string(want) + " bytes answered " + std::to_string(n));
    return v;
  }
  if (std::memcmp(b, "root", std::min<size_t>(n, 4)) != 0) {
    refuse(v, "it does not begin with \"root\", as every ROOT file does");
    return v;
  }
  v.end = rootFileEnd(b, n);
  if (v.end < 0) {
    refuse(v, "too short to hold a ROOT file's header");
    return v;
  }
  if (v.end > size) {
    refuse(v, "its header says the file is " + std::to_string(v.end) + " bytes");
    return v;
  }
  v.kind = v.end == size ? RootHeaderVerdict::kConfirmed : RootHeaderVerdict::kNoClaim;
  // The header's own fields, as TFile::Init reads and refuses them.
  RootFileHeader& h = v.header;
  if (!parseRootFileHeader(b, n, h)) {
    refuse(v, "too short to hold a ROOT file's header");
    return v;
  }
  // ROOT's own bound (TFile::Init): a first record before the start of the file. One inside the
  // header's own fields is no file ROOT writes (it places it at 100), but nothing ROOT reads needs
  // it past them: TFile::Init reads the directory's record at the first record and its name
  // record's length, and a header damaged there misplaces that record, which then fails its own
  // checks.
  if (h.begin < 0) {
    refuse(v, "its header says its first record begins at " + num(h.begin) +
                  ", before the start of the file");
    return v;
  }
  if (h.begin > h.end) {
    refuse(v, "its header says its first record begins at " + num(h.begin) +
                  ", past the end of its data at " + num(h.end));
    return v;
  }
  if (h.nbytesName < 10 || h.nbytesName > 10000) {
    refuse(v, "its header gives the top directory's name record " + num(h.nbytesName) +
                  " bytes; ROOT reads 10 to 10000");
    return v;
  }
  const int64_t dirAt = h.begin + h.nbytesName; // both bounded: no overflow
  const int64_t dirLen = static_cast<int64_t>(rootDirRecordBytes(h.version));
  if (dirLen > h.end - dirAt) {
    refuse(v, "its top directory's record ends at " + num(dirAt + dirLen) +
                  ", past the end of its data at " + num(h.end));
    return v;
  }
  if (dirAt + dirLen <= static_cast<int64_t>(n)) { // in the first bytes, as for most files
    RootDirRecord d;
    if (!parseRootDirRecord(b + dirAt, static_cast<size_t>(dirLen), d)) {
      refuse(v, "its top directory's record cannot be read");
      return v;
    }
    judgeDirRecord(v, d);
    return v;
  }
  v.next = RootHeaderVerdict::kDirRecord; // long names place it further: one more read
  v.nextAt = dirAt;
  v.nextLen = static_cast<uint32_t>(dirLen);
  return v;
}

void rootCheckRead(RootHeaderVerdict& v, const unsigned char* b, size_t n) {
  if (v.next == RootHeaderVerdict::kNone || v.kind == RootHeaderVerdict::kBad)
    return;
  if (!b || n < v.nextLen)
    return refuse(v, "a read of " + num(v.nextLen) + " bytes at " + num(v.nextAt) + " answered " +
                         num(static_cast<long long>(b ? n : 0)));
  if (v.next == RootHeaderVerdict::kDirRecord) {
    RootDirRecord d;
    if (!parseRootDirRecord(b, v.nextLen, d))
      return refuse(v, "its top directory's record cannot be read");
    return judgeDirRecord(v, d);
  }
  judgeKeysList(v, b, v.nextLen);
}

int headerReadTimeoutS(int attemptLimitS, int64_t elapsedMs) {
  const int64_t limitMs = int64_t(attemptLimitS > 0 ? attemptLimitS : kOpenDeadlineS) * 1000;
  const int64_t leftS = (limitMs - std::max<int64_t>(elapsedMs, 0) + 999) / 1000;
  return static_cast<int>(std::max<int64_t>(kHeaderReadFloorS, std::min<int64_t>(leftS, limitMs / 1000)));
}

int checkReadTimeoutS(uint32_t len, int64_t leftMs, int64_t openLeftMs) {
  if (openLeftMs <= 0)
    return 0;
  const int64_t floorS =
      kHeaderReadFloorS + (int64_t(len) + kCheckReadMinBytesPerS - 1) / kCheckReadMinBytesPerS;
  const int64_t leftS = leftMs > 0 ? (leftMs + 999) / 1000 : 0;
  const int64_t capS = openLeftMs / 1000 + (openLeftMs % 1000 ? 1 : 0);
  return static_cast<int>(std::min<int64_t>({std::max(leftS, floorS), capS, INT_MAX}));
}

ByteSpan checkReadSpan(bool keep, uint32_t pageSize, uint64_t size, uint64_t at, uint64_t len,
                       uint64_t room) {
  const ByteSpan asked{at, len, false};
  if (!keep || pageSize == 0 || len == 0 || at >= size || len > size - at)
    return asked;
  const std::pair<uint64_t, uint64_t> r = roundSpan(pageSize, size, at, len);
  if (r.first > at || r.second < at + len || r.second - r.first > room)
    return asked;
  return ByteSpan{r.first, r.second - r.first, true};
}

std::string ageText(uint64_t s) {
  struct Unit {
    uint64_t secs;
    const char* name;
  };
  static constexpr Unit kUnits[] = {{86400, "day"}, {3600, "hour"}, {60, "minute"}, {1, "second"}};
  for (const Unit& u : kUnits)
    if (s >= u.secs || u.secs == 1) {
      const uint64_t n = s / u.secs;
      return std::to_string(n) + " " + u.name + (n == 1 ? "" : "s");
    }
  return std::string(); // not reached: the last unit takes any value
}

std::string bytesText(uint64_t n) {
  if (n < 1000)
    return std::to_string(n) + (n == 1 ? " byte" : " bytes");
  static constexpr const char* kUnits[] = {"kB", "MB", "GB", "TB", "PB"};
  double v = static_cast<double>(n) / 1000.0;
  size_t k = 0;
  while (v >= 999.95 && k + 1 < sizeof kUnits / sizeof kUnits[0]) {
    v /= 1000.0;
    ++k;
  }
  char b[48];
  std::snprintf(b, sizeof b, "%.1f %s", v, kUnits[k]);
  return b;
}

uint64_t suggestedWindowS(uint64_t ageS) {
  constexpr uint64_t kDay = 86400;
  constexpr uint64_t kMax = uint64_t(INT_MAX) / kDay * kDay;
  if (ageS >= kMax)
    return kMax;
  return std::min(kMax, (ageS + kDay + kDay - 1) / kDay * kDay);
}

std::string staleEntryAdvice(uint64_t ageS, uint64_t held, uint64_t size) {
  if (held == 0 && size > 0)
    return std::string(); // nothing of the file to read from the cache
  const bool whole = held >= size;
  std::string out = "uCache holds an earlier copy of this file, last confirmed at an origin " +
                    ageText(ageS) + " ago, holding ";
  if (whole) {
    out += "the whole file (" + bytesText(size) + ")";
  } else {
    const uint64_t pct = size ? static_cast<uint64_t>((static_cast<long double>(held) * 100) / size) : 0;
    out += bytesText(held) + " of its " + bytesText(size) + " (" +
           (pct == 0 ? std::string("less than 1%") : std::to_string(pct) + "%") + ")";
  }
  out += ": a job run with a freshness window longer than that (UCACHE_REVALIDATE_S=" +
         std::to_string(suggestedWindowS(ageS)) +
         ", or revalidate_seconds in ucache.conf) reads ";
  out += whole ? "it" : "the parts it holds";
  out += " from the cache without asking an origin";
  if (!whole)
    out += "; reads of the parts it does not hold will still fail";
  return out;
}

std::string withAdvice(const std::string& text, const std::string& advice, size_t max) {
  if (advice.empty())
    return text;
  if (text.empty())
    return advice.size() <= max ? advice : text;
  const std::string join = text.back() == '.' ? " " : ". ";
  if (advice.size() + join.size() > max)
    return text;
  const size_t room = max - advice.size() - join.size();
  return (text.size() > room ? text.substr(0, room) : text) + join + advice;
}

std::string exclusionName(const std::vector<Hop>& hops, int i) {
  if (i <= 0 || static_cast<size_t>(i) >= hops.size())
    return std::string();
  const Hop& h = hops[static_cast<size_t>(i)];
  if ((h.flags & kIsManager) || h.eosFst || isIpv4(h.host) || isIpv6(h.host))
    return std::string();
  std::string name = h.host;
  for (char& c : name)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  while (!name.empty() && name.back() == '.')
    name.pop_back();
  return name.find('.') == std::string::npos ? std::string() : name;
}

int budgetS(int readerTimeoutS, int sewS, int trS, int maxS) {
  int b = maxS > 0 ? maxS : kFailoverMaxS;
  if (readerTimeoutS > 0)
    b = std::min(b, readerTimeoutS);
  return std::max(std::min(b, sewS - trS - 5), 1);
}

bool budgetSpent(int64_t remainingMs, int attemptDeadlineS, int readerTimeoutS) {
  int64_t floorMs = std::min<int64_t>(attemptDeadlineS, 30) * 1000;
  if (readerTimeoutS > 0)
    floorMs = std::min<int64_t>(floorMs, int64_t(readerTimeoutS) * 500);
  return remainingMs < floorMs;
}

AfterFailure afterFailure(std::vector<SiteFailure>& failed, const SiteFailure& f, int maxSites,
                          bool spent) {
  bool seen = false, sameSite = false;
  for (const SiteFailure& x : failed) {
    seen = seen || x.server == f.server;
    sameSite = sameSite || x.site == f.site;
  }
  if (!seen)
    failed.push_back(f);
  std::vector<std::string> sites;
  for (const SiteFailure& x : failed)
    if (std::find(sites.begin(), sites.end(), x.site) == sites.end())
      sites.push_back(x.site);
  if (static_cast<int>(sites.size()) >= maxSites || spent)
    return AfterFailure::kStop;
  return seen || sameSite ? AfterFailure::kRepeat : AfterFailure::kRetry;
}

Location locationOf(const std::string& address, bool manager, bool pending) {
  Location l;
  const auto colon = address.rfind(':');
  l.host = address.substr(0, colon);
  if (l.host.size() > 1 && l.host.front() == '[' && l.host.back() == ']')
    l.host = l.host.substr(1, l.host.size() - 2);
  if (l.host.compare(0, 7, "::ffff:") == 0)
    l.host.erase(0, 7);
  else if (l.host.compare(0, 2, "::") == 0 && l.host.find('.') != std::string::npos)
    l.host.erase(0, 2);
  l.port = colon == std::string::npos ? 1094 : std::atoi(address.c_str() + colon + 1);
  l.manager = manager;
  l.pending = pending;
  return l;
}

bool copyOffline(uint32_t statFlags, const std::string& server, const std::string* testOffline) {
  return (statFlags & kStatOffline) ||
         (testOffline && testOffline->find("," + server + ",") != std::string::npos);
}

std::vector<Location> orderLocations(const std::vector<Location>& listed,
                                     const std::vector<std::string>& readerTried,
                                     const std::vector<SiteFailure>& failed) {
  const auto lower = [](std::string s) {
    for (char& c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  std::vector<std::string> tried;
  for (const std::string& t : readerTried)
    tried.push_back(lower(t.substr(0, t.find(':'))));
  std::vector<Location> online, pending;
  std::vector<std::pair<size_t, Location>> excluded; // with the position of its failure
  for (const Location& l : listed) {
    // A failed server is named as the client names it, an IPv6 address in brackets.
    const std::string host = lower(l.host),
                      server = (host.find(':') == std::string::npos ? host : "[" + host + "]") + ":" +
                               std::to_string(l.port);
    if (std::find(tried.begin(), tried.end(), host) != tried.end())
      continue;
    const bool literal = isIpv4(host) || isIpv6(host);
    size_t age = failed.size();
    const std::string site = siteOf(host);
    for (size_t i = 0; i < failed.size() && age == failed.size(); ++i) {
      // A server that never answered is its own site (host:port), but the
      // domain of its name is where its siblings are.
      const std::string& fs = failed[i].server;
      if (lower(fs) == server ||
          (!literal && (failed[i].site == site || siteOf(fs.substr(0, fs.rfind(':'))) == site)))
        age = i;
    }
    const bool refused = std::any_of(failed.begin(), failed.end(), [&](const SiteFailure& x) {
      return x.bad && lower(x.server) == server;
    });
    if (age < failed.size()) {
      if (!refused)
        excluded.emplace_back(age, l);
    } else
      (l.pending ? pending : online).push_back(l);
  }
  online.insert(online.end(), pending.begin(), pending.end());
  if (online.empty()) {
    std::stable_sort(excluded.begin(), excluded.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& e : excluded)
      online.push_back(e.second);
  }
  return online;
}

std::vector<std::string> exclusionNames(const std::vector<SiteFailure>& failed) {
  std::vector<std::string> out;
  for (const SiteFailure& x : failed)
    if (!x.name.empty() && std::find(out.begin(), out.end(), x.name) == out.end())
      out.push_back(x.name);
  return out;
}

bool refusedBefore(const std::vector<SiteFailure>& failed, const std::string& server) {
  const auto lower = [](std::string s) {
    for (char& c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string want = lower(server);
  return std::any_of(failed.begin(), failed.end(),
                     [&](const SiteFailure& x) { return x.bad && lower(x.server) == want; });
}

void noteReadFailure(std::vector<SiteFailure>& failed, SiteFailure f, size_t keep) {
  for (auto it = failed.begin(); it != failed.end(); ++it)
    if (it->server == f.server) {
      f.bad = f.bad || it->bad; // found bad once, bad for good
      failed.erase(it);
      break;
    }
  failed.push_back(std::move(f));
  while (failed.size() > keep) {
    const auto newest = failed.end() - 1; // never dropped: the copy the read just failed at
    const auto old =
        std::find_if(failed.begin(), newest, [](const SiteFailure& x) { return !x.bad; });
    if (old == newest)
      break; // every older one was found bad: none is dropped
    failed.erase(old);
  }
}

bool failedBefore(const std::vector<SiteFailure>& failed, const SiteFailure& f) {
  const std::string host = f.server.substr(0, f.server.rfind(':'));
  for (const SiteFailure& x : failed)
    if (x.server == f.server || (x.site == f.site && f.site != host))
      return true;
  return false;
}

bool armedForReads(bool adaptiveFederation, bool cacheServes, bool viaRedirector, bool slotLayout,
                   bool sizeKnown) {
  return adaptiveFederation && cacheServes && viaRedirector && !slotLayout && sizeKnown;
}

bool shortInsideConfirmed(uint64_t confirmed, uint64_t off, uint64_t len, uint64_t got) {
  return confirmed && len && got < len && off + len >= off && off + len <= confirmed;
}

bool emptyPageReadInsideConfirmed(bool armed, bool pageRead, int code, uint64_t confirmed,
                                  uint64_t off, uint64_t len) {
  return armed && pageRead && code == kErrInvalidResponse &&
         shortInsideConfirmed(confirmed, off, len, 0);
}

int readDeadlineS(const char* text) {
  char* end = nullptr;
  const long v = text ? std::strtol(text, &end, 10) : 0;
  return text && end != text && *end == '\0' && v > 0 && v < INT_MAX ? static_cast<int>(v)
                                                                     : kReadDeadlineS;
}

int readBudgetS(int readerTimeoutS, int requestTimeoutS) {
  if (readerTimeoutS > 0)
    return readerTimeoutS;
  return requestTimeoutS > 0 ? requestTimeoutS : kReadDeadlineS;
}

int readAttemptTimeoutS(int readerTimeoutS, int requestTimeoutS, int deadlineS, int elapsedS,
                        bool again) {
  const int left = readBudgetS(readerTimeoutS, requestTimeoutS) - elapsedS;
  int floorS = std::min(deadlineS, 30);
  if (readerTimeoutS > 0)
    floorS = std::min(floorS, readerTimeoutS / 2);
  if (again && left < std::max(floorS, 1))
    return 0;
  return std::max(std::min(deadlineS, left), 1);
}

ReadStep readStep(int code, int errNo) {
  static constexpr int kCodes[] = {
      kErrInternal,       kErrSocketError,    kErrSocketTimeout,    kErrSocketDisconnected,
      kErrPollerError,    kErrSocketOptError, kErrStreamDisconnect, kErrConnectionError,
      kErrInvalidSession, kErrTlsError,       kErrOperationExpired, kErrOperationInterrupted};
  static constexpr int kServer[] = {kXrFileNotOpen,   kXrFSError,    kXrIOError,     kXrNoMemory,
                                    kXrNotAuthorized, kXrNotFound,   kXrServerError, kXrNoServer,
                                    kXrOverloaded,    kXrReqTimedOut};
  const auto in = [](const auto& list, int v) {
    return std::find(std::begin(list), std::end(list), v) != std::end(list);
  };
  const bool moves = in(kCodes, code) || (code == kErrErrorResponse && in(kServer, errNo));
  return moves ? ReadStep::kMove : ReadStep::kFinal;
}

namespace {
// `t` with every URL query removed (a query runs to the next blank, where its
// URL ends) and cut to at most `max` bytes.
std::string withoutQueries(const std::string& t, size_t max) {
  std::string out;
  for (size_t i = 0; i < t.size() && out.size() < max; ++i) {
    if (t[i] == '?') {
      while (i + 1 < t.size() && t[i + 1] != ' ' && t[i + 1] != '\t' && t[i + 1] != '\n')
        ++i;
      continue;
    }
    out += t[i];
  }
  return out;
}
} // namespace

std::string sitesFailedList(const std::vector<SiteFailure>& failed) {
  std::string out;
  for (size_t i = 0; i < failed.size(); ++i) {
    if (i)
      out += "; ";
    out += failed[i].site + " (" + failed[i].server + "): " + withoutQueries(failed[i].error, 160);
  }
  return out;
}

std::string allSitesFailedText(const std::string& file, const std::vector<SiteFailure>& failed) {
  const std::string out =
      "uCache: " + withoutQueries(file, 300) + ": no site could open it: " + sitesFailedList(failed);
  return out.size() > kErrorTextMax ? out.substr(0, kErrorTextMax) : out;
}

std::string noCopyServedText(const std::string& file, const std::vector<SiteFailure>& failed) {
  std::string out = "uCache: " + withoutQueries(file, 300) + ": no copy could serve the read: ";
  for (size_t i = 0; i < failed.size(); ++i) {
    if (i)
      out += "; ";
    out += failed[i].site + " (" + failed[i].server;
    if (failed[i].size >= 0)
      out += ", " + std::to_string(failed[i].size) + " bytes";
    out += "): " + withoutQueries(failed[i].error, 160);
  }
  return out.size() > kErrorTextMax ? out.substr(0, kErrorTextMax) : out;
}

std::string earlierMoveText(const std::string& moveText, uint64_t agoMs) {
  static const std::string kMark = ": no copy could serve the read: ";
  const size_t at = moveText.find(kMark);
  if (at == std::string::npos)
    return moveText;
  const std::string ago = agoMs < 1000 ? std::to_string(agoMs) + " ms" : ageText(agoMs / 1000);
  std::string out = moveText.substr(0, at + kMark.size()) + "a move made " + ago +
                    " ago for an earlier read of the handle found none, and none is made again "
                    "within a second; the copies it tried, as they answered that read: " +
                    moveText.substr(at + kMark.size());
  return out.size() > kErrorTextMax ? out.substr(0, kErrorTextMax) : out;
}

std::string escapeForJson(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s)
    appendEscaped(out, c, std::string::npos);
  return out;
}

std::string recordText(const std::string& t, size_t max) {
  std::string out;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] == '?') { // the query runs to the next blank, where its URL ends
      while (i + 1 < t.size() && t[i + 1] != ' ' && t[i + 1] != '\t' && t[i + 1] != '\n')
        ++i;
      continue;
    }
    if (!appendEscaped(out, t[i], max))
      break;
  }
  return out;
}

} // namespace ucache
