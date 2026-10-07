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
constexpr int kErrDataError = 14;
constexpr int kErrSocketError = 102;
constexpr int kErrSocketTimeout = 103;
constexpr int kErrSocketDisconnected = 104;
constexpr int kErrStreamDisconnect = 107;
constexpr int kErrConnectionError = 108;
constexpr int kErrInvalidSession = 109;
constexpr int kErrTlsError = 110;
constexpr int kErrAuthFailed = 204;
constexpr int kErrOperationExpired = 206;
constexpr int kErrNotFound = 304;
constexpr int kErrRedirectLimit = 306;
constexpr int kErrErrorResponse = 400; // the server answered with an error: errNo holds its code
// Server error codes (XProtocol.hh), in errNo with kErrErrorResponse.
constexpr int kXrFileLocked = 3003;
constexpr int kXrFSError = 3005;
constexpr int kXrIOError = 3007;
constexpr int kXrNotAuthorized = 3010;
constexpr int kXrNotFound = 3011;
constexpr int kXrServerError = 3012;
constexpr int kXrNoServer = 3014;
constexpr int kXrCancelled = 3017;
constexpr int kXrItExists = 3018;
constexpr int kXrOverloaded = 3024;
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

// What a chain of hops says. Which hop is a redirector, and which server the
// client itself would name on a retry, is left to the reading of the records.
struct ChainView {
  int last = -1; // the host that answered last (the client's DataServer); -1 for none
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
};
const char* openRouteName(OpenRoute r); // "relay", "lazy", "degraded", "fork"

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

// A JSON string's contents: quote and backslash escaped, control bytes as
// \u00XX, so a server's text can never break a record line.
std::string escapeForJson(const std::string& s);

// An error's text as a record carries it: every URL query removed (from a '?'
// to the next blank, where its URL ends; a query can carry an access token),
// then escaped as escapeForJson does and cut to at most `max` bytes, never inside
// an escape.
std::string recordText(const std::string& text, size_t max = 200);

} // namespace ucache
