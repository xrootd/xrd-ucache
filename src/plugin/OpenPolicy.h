// What the answer to an origin open says, in plain terms: the hops of its
// redirect chain, the server that answered last, the site that server belongs
// to, and a provisional class for a failure. The records of origin opens and
// failures (docs/FORMAT.md) are written from these.
//
// The XRootD client hands every open's completion the list of hosts it went
// through (its HostList): one entry per hop, each with the login flags that host
// answered with (zero for a host that never answered), and a mark on the one the
// client uses as its load balancer. UCacheFile.cc turns that list into Hops; the
// functions here read it. They are kept free of the client library, so the unit
// tests build Hops by hand.
//
// Thread-safety: pure functions of their arguments; no state.
#pragma once

#include "RootFormat.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ucache {

// The XRootD status codes read here, as plain integers so that this file
// compiles without the client's headers. UCacheFile.cc checks every one of
// them against those headers at compile time: a client that changed one fails
// the build, never a classification in the field.
namespace xrdwire {
// Client status codes (XrdClStatus.hh).
constexpr int kErrInternal = 7;
constexpr int kErrDataError = 14;
constexpr int kErrSocketError = 102;
constexpr int kErrSocketTimeout = 103;
constexpr int kErrSocketDisconnected = 104;
constexpr int kErrPollerError = 105;
constexpr int kErrSocketOptError = 106;
constexpr int kErrStreamDisconnect = 107;
constexpr int kErrConnectionError = 108;
constexpr int kErrInvalidSession = 109;
constexpr int kErrTlsError = 110;
constexpr int kErrAuthFailed = 204;
constexpr int kErrOperationExpired = 206;
constexpr int kErrOperationInterrupted = 207;
constexpr int kErrNotFound = 304;
constexpr int kErrRedirectLimit = 306;
constexpr int kErrErrorResponse = 400; // the server answered with an error: errNo holds its code
constexpr int kErrRedirect = 401;      // a redirect the client was told not to follow
// Server error codes (XProtocol.hh), in errNo with kErrErrorResponse.
constexpr int kXrFileLocked = 3003;
constexpr int kXrFileNotOpen = 3004;
constexpr int kXrFSError = 3005;
constexpr int kXrIOError = 3007;
constexpr int kXrNoMemory = 3008;
constexpr int kXrNotAuthorized = 3010;
constexpr int kXrNotFound = 3011;
constexpr int kXrServerError = 3012;
constexpr int kXrNoServer = 3014;
constexpr int kXrCancelled = 3017;
constexpr int kXrItExists = 3018;
constexpr int kXrOverloaded = 3024;
constexpr int kXrReqTimedOut = 3034;
// Login flags of a host (XProtocol.hh), and the stat flag of a file on tape.
constexpr uint32_t kIsServer = 0x1;
constexpr uint32_t kIsManager = 0x2;  // a redirector
constexpr uint32_t kAttrMeta = 0x100; // a redirector over redirectors (a global one)
constexpr uint32_t kStatOffline = 8;
} // namespace xrdwire

// One host an open went through.
struct Hop {
  std::string host; // as the client names it
  int port = 0;
  uint32_t flags = 0;        // its login flags; 0 when it never answered
  bool loadBalancer = false; // the client uses it as its load balancer
  // Its URL carries EOS's capability (cap.msg or cap.sym): a data server behind
  // an EOS front end, reached with the front end's signed permission.
  bool eosFst = false;
};

// A Hop from the parts of one HostList entry. `query` is the parameters of the
// entry's URL, as "a=1&b=2" (a leading '?' is allowed); only their names are
// read, so no value (a token, a capability) is kept.
Hop makeHop(const std::string& host, int port, uint32_t flags, bool loadBalancer,
            const std::string& query);

// What a chain of hops says.
struct ChainView {
  int last = -1; // the host that answered last (the client's DataServer); -1 for none
  // The server the client itself names when it retries (in tried=): the last
  // data server that sent the open on to another host, else the last host;
  // -1 when the last host is a redirector.
  int effective = -1;
  int regional = -1;          // the first redirector that is not a global one; -1 for none
  bool viaRedirector = false; // a redirector is in the chain
  size_t hops = 0;
};
ChainView readChain(const std::vector<Hop>& hops);

// The chain as one line: "host:port/0x<flags>" per hop, comma separated, with
// '*' after the load balancer and '^' after an EOS data server.
std::string chainText(const std::vector<Hop>& hops);

// The site a host belongs to, from its name alone (no DNS): the name without
// its first label, lower-cased ("cmsio10.rc.ufl.edu" -> "rc.ufl.edu"). A name
// without a dot, and an IPv4 or IPv6 address, is its own site.
std::string siteOf(const std::string& host);

// A provisional class for a failed origin request, from its status code and
// errNo alone. It is a first reading, not a verdict: what a failure means often
// depends on its context (whether other sites progressed, whose timeout
// expired), so every record also keeps the raw code and errNo.
//   client   -- this client could not reach or talk to the server: socket,
//               connection and TLS errors, failed authentication, a refusal
//               to authorize (a token or proxy the server does not accept)
//   expired  -- the request outlived its timeout (whose, it does not say)
//   site     -- the server or its storage failed the request, or the
//               redirectors bounced it past the client's redirect limit
//   notfound -- no server holds the file
//   data     -- the bytes were refused: a checksum that failed
//   other    -- anything else; among them a server's own report that it could
//               not reach a server behind it (kXR_noserver), which can be that
//               site's network as well as this client's
enum class FailureClass : uint8_t { kNone, kClient, kSite, kNotFound, kData, kExpired, kOther };
FailureClass classifyFailure(int code, int errNo); // kNone for code 0 (success)
const char* failureClassName(FailureClass c);      // "none", "client", ...

// The kind of origin open an attempt belongs to.
enum class OpenRoute : uint8_t {
  kRelay,    // the reader's own open of a handle, relayed to the origin
  kLazy,     // a trusted handle's open at its first miss
  kDegraded, // a trusted handle whose entry was gone at its first read
  kFork,     // a forked child's open afresh of a handle it inherited
  kFailover, // with adaptive_federation on, an open of any of these at another site
  kCandidate, // ... at a server a regional redirector listed as holding the file
  kRead,     // ... made because reads of the handle stopped (every attempt of such an open)
};
// "relay", "lazy", "degraded", "fork", "failover", "candidate", "read"
const char* openRouteName(OpenRoute r);

// With adaptive_federation on, the deadline of every read open of a handle
// the cache serves: the reader's own open, and the cache's own opens of the
// origin (a trusted handle's first miss, a handle whose entry was gone, a
// forked child's open afresh). Without one, an open waits for as long as the
// XRootD client's request timeout (30 minutes by default) when a server
// accepts the connection and never answers, or a copy is being staged from
// tape. With one, the open fails with the client's "operation expired" error,
// and the job can go on to whatever it does with a failed open. With the
// switch off, the reader's own open passes the reader's own timeout unchanged,
// and the cache's own opens carry none (the client's request timeout), as
// before there was a deadline: there is no failover then, and the client's own recovery from a server that never
// accepts the connection (going on to another data server at a redirector)
// can take longer than any deadline.
//
// 60 s is a first value, to be revisited with measurements; it must stay at
// least 30 s, so that a healthy open on a loaded server never expires.
inline constexpr int kOpenDeadlineS = 60;
static_assert(kOpenDeadlineS >= 30, "an open deadline below 30 s expires healthy opens");

// The per-request timeout an open is given, in seconds. A positive timeout of
// the reader's own below the deadline is the reader's choice and passes
// unchanged; anything else (0, or longer) gets the deadline. For a reader that
// gives none, the caller passes the client's RequestTimeout, which is what
// that reader would wait uncached, so a shorter one is kept too. `deadlineS`
// replaces kOpenDeadlineS when positive: a test's value, which may be below
// the 30 s floor.
int openDeadlineS(int readerTimeoutS, int deadlineS = 0);

// ---- Opening a file at another site (adaptive_federation on) --------------
// When an open through a redirector fails at one site in a way another site
// may not share, the open is made again through the redirector, with the
// failed servers named in tried= so that it sends the open elsewhere; a
// success is taken wherever it lands. Each such sequence of attempts has one
// absolute time budget and a bound on the sites it tries.

// What follows one attempt of an open.
enum class OpenStep : uint8_t {
  kFinal,    // the outcome is the open's (a success, or an error no site would change)
  kFailOver, // try another site
  kLocate,   // a file not found where a copy may yet exist: ask the regional
             // redirector where the copies are (orderLocations)
};
// The step after an attempt whose chain is `v`, which failed with (code, errNo)
// -- code 0 for a success -- and, on success, served a file of another size
// than the one the cache holds (`sizeDiffers`, the cache's own opens only) or
// one on tape (`offline`). `excluded`: the attempt named servers of ours in
// tried=, so a copy may have been hidden by them. Only a chain through a
// redirector can go elsewhere; a failure of the first host itself (the
// redirector down, stalled or refusing) is final, as there is no other
// redirector to ask. `repeats`: a success that landed on a server or site where
// reads of the file stopped (the open of a read that moves): the redirector did
// not keep it from there, so the regional redirector is asked for the copies.
// `regionalKnown`: a regional redirector can be asked. A server that did not
// answer within the deadline then sends the open to that list, not back through
// the redirector, which would steer it to the other server of the same site,
// and that one stalls as well.
OpenStep stepAfter(const ChainView& v, int code, int errNo, bool sizeDiffers, bool offline,
                   bool excluded, bool repeats = false, bool regionalKnown = false);
// The step after an open whose header read (rootHeaderVerdict) failed with (code, errNo) at
// a copy reached through `v`: a failure of that copy, whatever the code, so another copy is
// tried when the open went through a redirector; how (another site, or the regional's list
// after a stall) is stepAfter's. A direct URL has no other copy: kFinal.
OpenStep stepAfterHeaderRead(const ChainView& v, int code, int errNo, bool excluded,
                             bool regionalKnown);
// Whether the client would have recovered a read that failed with `code` by opening the file
// again, had its read recovery and its following of redirects been on (its own list of the
// failures it recovers: a lost or timed-out connection, a lost session, TLS, an interruption,
// an internal error; and a server's redirect). The header read has both off, so that no other
// copy answers it; a direct URL's open is made once more for such a failure, as the client
// would have made it.
bool clientRecoversRead(int code);

// Whether a copy of `path` that reports `size` bytes is a bad copy for that file: no
// bytes in a file named *.root (a ROOT file is never empty). For any other name a size of
// zero is an ordinary size.
bool emptyRootCopy(const std::string& path, long long size);
// Whether `path` names a ROOT file: the part before the first '?' (the path without its
// query) ends in ".root", in upper or lower case.
bool rootNamed(const std::string& path);

// The header check of a ROOT file's copy, made inside the open of a file named *.root: its
// first kRootHeaderBytes bytes (what ROOT itself reads at open), or the whole copy when it is
// smaller (rootHeaderBytes). A ROOT file begins with "root", a version (a 32-bit integer,
// big-endian, at 4), where its first record begins (at 8) and at 12 the end of its data: a
// 32-bit integer, or a 64-bit one when the version is 1000000 or more.
inline constexpr size_t kRootHeaderBytes = 300;
// The bytes the header check reads of a copy that reports `size` bytes (0 for none).
size_t rootHeaderBytes(long long size);
// The end of the data the first `n` bytes of `b` state, or -1 when they state none: they do
// not begin with "root", or end before the end's own field (16 bytes, 20 for a 64-bit end).
long long rootFileEnd(const unsigned char* b, size_t n);
// What the checks made at open say of a copy that reports `size` (> 0) bytes. They read what ROOT
// reads when it opens a file (TFile::Init, TDirectoryFile::ReadKeys): the copy's first bytes, which
// hold the file's header and, unless long names push it further, the top directory's record; and
// then the list of top-level objects. rootHeaderVerdict judges the first read, and asks for each
// further read it needs in `next`; rootCheckRead judges each answer. The verdict is final when
// `next` is kNone:
//  - kBad, refused (`why` says what is wrong, in words):
//     - a read answered fewer bytes than it asked for, inside the copy's size (the copy ends before
//       its reported size);
//     - the copy does not begin with "root" (no ROOT file is without it: ROOT and uproot refuse
//       such a file; a copy of zeros at the right size is one), or is too short to hold its own
//       header;
//     - its size is smaller than the end its header states (a cut copy: ROOT reports it truncated);
//     - the header contradicts itself where ROOT refuses a file for it: the first record begins
//       inside the header's own fields or past the end of the data; the top directory's name
//       record is shorter than 10 or longer than 10000 bytes, in the header or in the directory's
//       record; the directory's record ends past the end of the data;
//     - in a completed header (one that places the list of top-level objects past the header), a
//       record it places lies outside the file's records (from the header's begin to the end of
//       the data): the list of top-level objects, the class descriptions, the list of free space
//       (a position of 0 for the last two means there is none);
//     - the list of top-level objects does not carry its own label (its position, its length, and
//       the top directory as its directory), or an object it lists lies outside the file's records
//       (its position from the begin to before the end, and its end at or before the end), or has
//       another directory than the top one, or the list ends inside its entries.
//  - kConfirmed: its size equals the end its header states, which confirms the file's size;
//  - kNoClaim: its size is larger than that end (ROOT reads such a file normally and ignores the
//    bytes past the end: a file updated in place, bytes appended, a writer that stopped after a
//    save), or the header was never completed: it places no list of top-level objects past the
//    header (the writer never saved the file; every copy is the same, and ROOT recovers what it
//    can), which makes no claim on what follows. The copy stands, and nothing is confirmed.
// The top directory is the record at the header's begin, where ROOT writes it. A list of top-level
// objects longer than kRootKeysListMaxBytes is not read: the verdict stands as the header gives it.
struct RootHeaderVerdict {
  enum Kind : uint8_t { kNoClaim, kConfirmed, kBad };
  Kind kind = kNoClaim;
  long long end = -1; // the end the header states; -1 for none
  std::string why;    // kBad: what is wrong with the copy
  // The read the check needs next, before the verdict is final: the top directory's record (when
  // it lies past the first bytes) or the list of top-level objects; `nextLen` bytes at `nextAt`.
  enum Next : uint8_t { kNone, kDirRecord, kKeysList };
  Next next = kNone;
  long long nextAt = 0;
  uint32_t nextLen = 0;
  long long size = 0;    // the size the copy reported
  RootFileHeader header; // what the header says, once read
  RootDirRecord dir;     // what the top directory's record says, once read
};
inline constexpr uint32_t kRootKeysListMaxBytes = 16u << 20;
RootHeaderVerdict rootHeaderVerdict(const unsigned char* b, size_t n, long long size);
// Judges the answer to the read `v.next` asked for: the `n` bytes of `b`, which a read of
// `v.nextLen` bytes at `v.nextAt` answered with (fewer than asked: the copy ends there, kBad).
// Sets the next read the check needs, or kNone. Does nothing when `v` asks for no read.
void rootCheckRead(RootHeaderVerdict& v, const unsigned char* b, size_t n);
// The time limit of the header read, in seconds: what is left of the open attempt's own limit
// (`attemptLimitS`, the attempt issued `elapsedMs` ago), and at least kHeaderReadFloorS. The
// floor gives a copy that answered its open at the very end of that limit a moment to answer
// the read (one round trip to a server that has just answered). It is the margin that budgetS
// keeps between an open's budget and the client's stream error window, so a read that starts
// at the end of the budget still ends inside that window. The checks' further reads share it
// (checkReadTimeoutS): the floor is given once, to all of them.
inline constexpr int kHeaderReadFloorS = 5;
static_assert(kHeaderReadFloorS >= 1 && kHeaderReadFloorS < kOpenDeadlineS,
              "the header read's floor is a moment, never a fresh open deadline");
int headerReadTimeoutS(int attemptLimitS, int64_t elapsedMs);
// The time limit, in seconds, of a further read of the checks made at open: what is left of the
// time the checks were given when their first read was sent (headerReadTimeoutS), `leftMs`,
// rounded up; 0 when none is left (the copy has run out of time, as for a read that expired).
int checkReadTimeoutS(int64_t leftMs);

// The host name a retry's tried= may carry for hop `i` of a chain, or "" when
// it may not: never a redirector, nor the first host, nor an address or a
// name without a dot (neither masks a server at a redirector), nor a data
// server reached with an EOS capability (behind an EOS front end, the front
// end is the site's entry). Lower-cased.
std::string exclusionName(const std::vector<Hop>& hops, int i);

// The open's budget, in seconds, read at its start: at most kFailoverMaxS, at
// most the reader's own open timeout when it gave one, and short enough to end
// before the client's stream error window (`sewS`), less its timeout
// resolution (`trS`) and 5 s, runs out: a reader that waits that window for
// a reopen after a failed read gets an answer before it gives up. `maxS`
// replaces kFailoverMaxS when positive (a test's value). At least 1.
inline constexpr int kFailoverMaxS = 180; // a first value, to be revisited with measurements
inline constexpr int kFailoverMaxSites = 4;
int budgetS(int readerTimeoutS, int sewS, int trS, int maxS = 0);
// True when nothing more may be issued with `remainingMs` of the budget left:
// less than one attempt's deadline, capped at 30 s, and for a reader with a
// timeout of its own, at half that timeout (a reader that gives 30 s would
// otherwise never have a second attempt).
bool budgetSpent(int64_t remainingMs, int attemptDeadlineS, int readerTimeoutS = 0);

// How long a copy the regional redirector lists is given to answer the question of its
// state before an open of it: a copy at a site that stalls would otherwise take the
// open's whole deadline, one after the other, before any copy is opened.
inline constexpr int kCandidateStatS = 10;
static_assert(kCandidateStatS < kOpenDeadlineS, "a copy's answer must not take a whole open deadline");

// One site an open failed at, as the final error and the next attempt see it.
struct SiteFailure {
  std::string server; // host:port
  std::string site;
  std::string name;  // what tried= may carry for it (exclusionName), "" for none
  std::string error; // the error's text
  // The copy is refused for what it is: its open succeeded and it is an empty or cut ROOT file,
  // or a read from it answered with bad bytes (a data error). It is not opened again by this
  // sequence, nor by the handle whose read found it, and the error that ends it is of class data.
  bool bad = false;
  long long size = -1; // the size the copy reported at its open; -1 when it is not known
};
// Whether `failed` lists `server` (host:port, in any case) as a copy refused for what it is.
bool refusedBefore(const std::vector<SiteFailure>& failed, const std::string& server);
// A read of a handle failed at the copy `f` (its server, site and size; `f.bad` when the read
// answered with bad bytes): it is put on the handle's list of copies its reads failed at,
// `failed`, as its newest entry. A server listed already keeps one entry, the newest, and stays
// bad once it was found bad. The list keeps at most `keep` entries, the oldest that is not bad
// dropped first; one found bad is never dropped, so it is never opened again for the handle.
void noteReadFailure(std::vector<SiteFailure>& failed, SiteFailure f, size_t keep);
// What the sequence does after a fail-over outcome, given the sites that
// failed before (`failed`, to which `f` is added unless its server is there):
//   kStop    the bound of `maxSites` distinct sites is reached, or the budget is
//            spent (`spent`)
//   kRepeat  the attempt landed on a server or site that had failed already:
//            the names did not keep the redirector from it, or it could not be
//            named. The caller asks the regional redirector for the copies.
//   kRetry   open again through the redirector, naming the failed servers
enum class AfterFailure : uint8_t { kRetry, kStop, kRepeat };
AfterFailure afterFailure(std::vector<SiteFailure>& failed, const SiteFailure& f, int maxSites,
                          bool spent);
// A copy a regional redirector's answer to a locate lists.
struct Location {
  std::string host;
  int port = 0;
  bool manager = false; // a redirector of its own, not a data server
  bool pending = false; // the file is being brought online there
};
// A Location from "host:port" as a locate's answer gives it: a bracketed IPv6 host
// loses its brackets, an IPv4 address in its IPv6 form ("::127.0.0.1",
// "::ffff:127.0.0.1") is the IPv4 one, and no port is 1094.
Location locationOf(const std::string& address, bool manager, bool pending);
// Whether the copy at `server` (host:port) is on tape: its stat flags say so, or a
// test's list (`testOffline`, ",host:port,host:port,"; null for none) names it.
bool copyOffline(uint32_t statFlags, const std::string& server, const std::string* testOffline);
// Which of `listed` to open, in order. Dropped: servers the reader's own tried=
// names (`readerTried`; never used), and the servers and the sites of `failed`
// (a site is its domain; an address stands for its server alone). Of what is
// left, online copies come before pending ones, each in the answer's order.
// When nothing is left, the failed ones are given, the oldest failure first,
// except a server whose copy was refused for what it is (`bad`): that one is final.
// `failed` names an IPv6 server in brackets ("[2001:db8::1]:1094"), as the client does;
// a Location holds the bare address.
std::vector<Location> orderLocations(const std::vector<Location>& listed,
                                     const std::vector<std::string>& readerTried,
                                     const std::vector<SiteFailure>& failed);
// The names of `failed` a retry's tried= carries, oldest first.
std::vector<std::string> exclusionNames(const std::vector<SiteFailure>& failed);
// Whether `f` is a server `failed` lists, or a site it lists. Servers behind one
// address are not one site: an address is its own server alone.
bool failedBefore(const std::vector<SiteFailure>& failed, const SiteFailure& f);
// The error an open that failed at every site it tried ends with: "uCache:
// <file>: no site could open it: <site> (<server>): <error>; ...", each error
// cut to 160 bytes and without URL queries, the whole at most 1024 bytes.
std::string allSitesFailedText(const std::string& file, const std::vector<SiteFailure>& failed);
// Its list alone: "<site> (<server>): <error>; ...", each error cut likewise.
std::string sitesFailedList(const std::vector<SiteFailure>& failed);
// The error a read ends with when no copy of the size its reader was shown could serve it:
// "uCache: <file>: no copy could serve the read: <site> (<server>, <size> bytes): <error>; ...",
// a copy whose size is not known without it; each error cut to 160 bytes and without URL
// queries, the whole at most 1024 bytes.
std::string noCopyServedText(const std::string& file, const std::vector<SiteFailure>& failed);

// ---- Reading a file from another site (adaptive_federation on) --------------
// A handle is armed to move its reads when the cache serves it, an open of it
// succeeded through a redirector, it was not shown a slot layout (whose
// addresses no other site has), and the size its reader was shown is known. The
// origin reads of an armed handle have a deadline, and one that fails in a way
// another site may not share is issued again, in full, at another site of that
// size: the reader gets the bytes, never the error, within a bound on the
// attempts and on the time. `slotLayout`: the handle was shown one. `sizeKnown`:
// the size its reader was shown is known; without it no copy can be told to be
// the reader's file, nothing could move, and the client's own recovery of a read
// stays on.
bool armedForReads(bool adaptiveFederation, bool cacheServes, bool viaRedirector, bool slotLayout,
                   bool sizeKnown);

// The time limit of one origin read of an armed handle: a backstop, 5 minutes. A
// server that stops answering is found by silence, not by this limit: the client's
// own check of a quiet connection (its stream timeout, 60 s, and its timeout
// resolution, 15 s, at the defaults) ends every request on that connection at once
// with an error, and each is made again at another site. The limit is on the whole
// call, from the moment it is issued, queueing and delivery included, and it is set
// so large that a read still running at it moves data at a rate nothing useful
// could be done at. It is the end of a read on a server that answers other requests
// on the connection and never this one. It must stay above the stream timeout plus
// its resolution, or it fires first; a reader that sets a longer stream timeout
// moves each file after this limit. Never below 30 s.
inline constexpr int kReadDeadlineS = 300;
static_assert(kReadDeadlineS >= 30, "a read deadline below 30 s expires healthy reads");
// A test's deadline (the text of UCACHE_TEST_READ_DEADLINE_S), which may be below
// that floor; unset, empty, not a number or not positive gives kReadDeadlineS.
int readDeadlineS(const char* text);

// How long a read may take in all, in seconds: the reader's own timeout when it
// gave one, else the client's request timeout (`requestTimeoutS`), which is
// what the read would be allowed without the cache.
int readBudgetS(int readerTimeoutS, int requestTimeoutS);
// The timeout of one attempt of a read, in seconds, `elapsedS` into its budget:
// the deadline `deadlineS`, or what is left of the budget when that is less. A
// reader's own shorter timeout passes unchanged, and its expiry ends the budget.
// 0: no attempt may be made. That is only for `again`, an attempt after a failed
// one, and when what is left is below the deadline capped at 30 s and, for a
// reader with a timeout of its own, below half of it (a reader that gives 30 s
// would otherwise never be moved).
int readAttemptTimeoutS(int readerTimeoutS, int requestTimeoutS, int deadlineS, int elapsedS,
                        bool again);
// Attempts of one read, at as many servers.
inline constexpr int kReadMaxAttempts = 4;

// Whether a read answered `got` bytes of the `len` asked at `off` is a short answer that counts
// against the copy: the file's size was confirmed by the header check (`confirmed`, 0 for none)
// and the request lies wholly inside it, so the copy ends before the file does. A short answer is
// normal at the end of a file, because readers, and the cache itself, ask for more than remains:
// a request that reaches past the confirmed size, and any request when no size was confirmed, never
// counts.
bool shortInsideConfirmed(uint64_t confirmed, uint64_t off, uint64_t len, uint64_t got);

// What follows a failed origin read of an armed handle. This is every status the
// client's own recovery of a read handles, which is off on such a handle, and
// more: the stream and connection errors, an expired request, and a server's
// report of a fault of its own or of its storage. Final: a request no other site
// would answer differently, and a corrupted answer (14), which is the integrity
// check's to decide.
enum class ReadStep : uint8_t { kFinal, kMove };
ReadStep readStep(int code, int errNo);

// A JSON string's contents: quote and backslash escaped, control bytes as
// \u00XX, so a server's text can never break a record line.
std::string escapeForJson(const std::string& s);

// An error's text as a record carries it: every URL query removed (from a '?'
// to the next blank, where its URL ends; a query can carry an access token),
// then escaped as escapeForJson does and cut to at most `max` bytes, never inside
// an escape.
std::string recordText(const std::string& text, size_t max = 200);

} // namespace ucache
