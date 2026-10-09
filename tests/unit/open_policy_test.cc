// What an origin open's answer says (OpenPolicy.h): the redirect chain read
// from the client's HostList, the site of a host, and the provisional class of
// a failure. Pure and XrdCl-free; the numeric codes are the client's, checked
// against its headers where the plugin is built.
#include "OpenPolicy.h"

#include <gtest/gtest.h>

#include <vector>

using namespace ucache;
using namespace ucache::xrdwire;

namespace {
// Login flags as the client reports them (XProtocol.hh): a data server, a
// redirector, and a redirector that is a meta manager (a global redirector).
constexpr uint32_t kServer = 0x1;
constexpr uint32_t kManager = 0x2;
constexpr uint32_t kMeta = 0x2 | 0x100;
Hop hop(const char* host, uint32_t flags, bool lb = false, const char* query = "") {
  return makeHop(host, 1094, flags, lb, query);
}
} // namespace

TEST(OpenPolicy, GlobalRegionalServerChain) {
  // The client's list through a federation: the global redirector (a meta
  // manager, the load balancer), the regional one, then the data server.
  const std::vector<Hop> hops = {hop("global.example.org", kMeta, true),
                                 hop("regional.example.org", kManager),
                                 hop("data.site.example.org", kServer)};
  const ChainView v = readChain(hops);
  EXPECT_EQ(v.hops, 3u);
  EXPECT_EQ(v.last, 2); // the data server is the host that answered last
  EXPECT_EQ(hops[static_cast<size_t>(v.last)].host, "data.site.example.org");
  EXPECT_EQ(chainText(hops),
            "global.example.org:1094/0x102*,regional.example.org:1094/0x2,"
            "data.site.example.org:1094/0x1");
}

TEST(OpenPolicy, DirectOpenIsOneHop) {
  const std::vector<Hop> hops = {hop("data.site.example.org", kServer)};
  const ChainView v = readChain(hops);
  EXPECT_EQ(v.hops, 1u);
  EXPECT_EQ(v.last, 0);
  EXPECT_EQ(chainText(hops), "data.site.example.org:1094/0x1");
}

TEST(OpenPolicy, EmptyListGivesNoChain) {
  const ChainView v = readChain({});
  EXPECT_EQ(v.hops, 0u);
  EXPECT_EQ(v.last, -1);
  EXPECT_EQ(chainText({}), "");
}

TEST(OpenPolicy, HostThatNeverAnsweredHasNoFlags) {
  // A refused connection: the client lists the host it tried, with no flags.
  const std::vector<Hop> hops = {makeHop("127.0.0.1", 1, 0, false, "")};
  EXPECT_EQ(readChain(hops).last, 0);
  EXPECT_EQ(chainText(hops), "127.0.0.1:1/0x0");
}

TEST(OpenPolicy, EosCapabilityMarksAnEosDataServer) {
  const Hop fst = hop("fst12.site.example.org", kServer, false,
                      "?cap.msg=AbC&cap.sym=XyZ&mgm.id=12&mgm.replicaindex=0");
  EXPECT_TRUE(fst.eosFst);
  EXPECT_TRUE(hop("fst.example.org", kServer, false, "mgm.id=1&cap.sym=x").eosFst);
  // Other parameters, and names that only begin like one, do not.
  EXPECT_FALSE(hop("data.example.org", kServer, false, "?authz=TOKEN&tried=a").eosFst);
  EXPECT_FALSE(hop("data.example.org", kServer, false, "cap.msgx=1&xcap.sym=2").eosFst);
  EXPECT_FALSE(hop("data.example.org", kServer).eosFst);
  // The chain names the mark, never the capability itself.
  EXPECT_EQ(chainText({fst}), "fst12.site.example.org:1094/0x1^");
}

TEST(OpenPolicy, SiteOfAHost) {
  EXPECT_EQ(siteOf("cmsio10.rc.ufl.edu"), "rc.ufl.edu");
  EXPECT_EQ(siteOf("CMSIO10.RC.UFL.EDU"), "rc.ufl.edu");
  EXPECT_EQ(siteOf("eospublic.cern.ch."), "cern.ch");
  EXPECT_EQ(siteOf("localhost"), "localhost"); // a dotless name is its own site
  EXPECT_EQ(siteOf("127.0.0.1"), "127.0.0.1"); // so is an address
  EXPECT_EQ(siteOf("[2001:db8::7]"), "[2001:db8::7]");
  EXPECT_EQ(siteOf("2001:db8::7"), "2001:db8::7");
  EXPECT_EQ(siteOf("10.1.2"), "1.2"); // three numbers are a name, not an address
  EXPECT_EQ(siteOf(""), "");
}

TEST(OpenPolicy, FailureClasses) {
  struct Row {
    int code;
    int errNo;
    FailureClass want;
  };
  const Row rows[] = {
      {0, 0, FailureClass::kNone},
      {kErrSocketError, 0, FailureClass::kClient},                  // 102
      {kErrSocketTimeout, 0, FailureClass::kClient},                // 103
      {kErrSocketDisconnected, 0, FailureClass::kClient},           // 104
      {kErrStreamDisconnect, 0, FailureClass::kClient},             // 107
      {kErrConnectionError, 111, FailureClass::kClient},            // 108
      {kErrInvalidSession, 0, FailureClass::kClient},               // 109
      {kErrTlsError, 0, FailureClass::kClient},                     // 110
      {kErrAuthFailed, 0, FailureClass::kClient},                   // 204
      {kErrOperationExpired, 0, FailureClass::kExpired},            // 206
      {kErrRedirectLimit, 0, FailureClass::kSite},                  // 306
      {kErrErrorResponse, kXrNotAuthorized, FailureClass::kClient}, // 3010
      {kErrErrorResponse, kXrNotFound, FailureClass::kNotFound},    // 3011
      {kErrNotFound, 0, FailureClass::kNotFound},                   // 304
      {kErrErrorResponse, kXrNoServer, FailureClass::kOther},       // 3014
      {kErrErrorResponse, kXrFileLocked, FailureClass::kSite},      // 3003
      {kErrErrorResponse, kXrFSError, FailureClass::kSite},         // 3005
      {kErrErrorResponse, kXrIOError, FailureClass::kSite},         // 3007
      {kErrErrorResponse, kXrServerError, FailureClass::kSite},     // 3012
      {kErrErrorResponse, kXrCancelled, FailureClass::kSite},       // 3017
      {kErrErrorResponse, kXrItExists, FailureClass::kSite},        // 3018
      {kErrErrorResponse, kXrOverloaded, FailureClass::kSite},      // 3024
      {kErrErrorResponse, 3013, FailureClass::kOther},              // unsupported
      {kErrDataError, 0, FailureClass::kData},                      // 14
      {101, 0, FailureClass::kOther}, // an invalid address is no fault of the server's
      {7, 0, FailureClass::kOther},   // errInternal
  };
  for (const Row& r : rows)
    EXPECT_EQ(classifyFailure(r.code, r.errNo), r.want)
        << "code " << r.code << " errNo " << r.errNo;
}

TEST(OpenPolicy, Names) {
  EXPECT_STREQ(failureClassName(FailureClass::kNone), "none");
  EXPECT_STREQ(failureClassName(FailureClass::kClient), "client");
  EXPECT_STREQ(failureClassName(FailureClass::kSite), "site");
  EXPECT_STREQ(failureClassName(FailureClass::kNotFound), "notfound");
  EXPECT_STREQ(failureClassName(FailureClass::kData), "data");
  EXPECT_STREQ(failureClassName(FailureClass::kExpired), "expired");
  EXPECT_STREQ(failureClassName(FailureClass::kOther), "other");
  EXPECT_STREQ(openRouteName(OpenRoute::kRelay), "relay");
  EXPECT_STREQ(openRouteName(OpenRoute::kLazy), "lazy");
  EXPECT_STREQ(openRouteName(OpenRoute::kDegraded), "degraded");
  EXPECT_STREQ(openRouteName(OpenRoute::kFork), "fork");
  EXPECT_STREQ(openRouteName(OpenRoute::kFailover), "failover");
  EXPECT_STREQ(openRouteName(OpenRoute::kCandidate), "candidate");
  EXPECT_STREQ(openRouteName(OpenRoute::kRead), "read");
}

TEST(OpenPolicy, JsonEscapeKeepsTheLineWhole) {
  EXPECT_EQ(escapeForJson("plain"), "plain");
  EXPECT_EQ(escapeForJson("a\"b\\c"), "a\\\"b\\\\c");
  EXPECT_EQ(escapeForJson(std::string("x\ny\x01", 4)), "x\\u000ay\\u0001");
}

// A record's error text: no URL query survives (a query can carry a token),
// and the text as written, escapes included, is at most the bound.
TEST(OpenPolicy, RecordTextDropsQueriesAndIsBounded) {
  EXPECT_EQ(recordText("[ERROR] at root://h:1094//p?authz=SECRET&x=1 retry"),
            "[ERROR] at root://h:1094//p retry");
  EXPECT_EQ(recordText("ends root://h//p?authz=SECRET"), "ends root://h//p");
  EXPECT_EQ(recordText("a root://h//p?t=S1 and root://g//q?t=S2\tthen"),
            "a root://h//p and root://g//q\\u0009then");
  EXPECT_EQ(recordText("why? because"), "why because");
  EXPECT_EQ(recordText("say \"no\""), "say \\\"no\\\"");
  const std::string q = recordText(std::string(300, '"'));
  EXPECT_EQ(q.size(), 200u);
  for (size_t i = 0; i < q.size(); i += 2)
    EXPECT_EQ(q.substr(i, 2), "\\\"") << i; // never cut inside an escape
  const std::string ctl = recordText(std::string(100, '\x01'));
  EXPECT_EQ(ctl.size(), 198u) << "33 whole \\u00XX escapes, never a cut one";
  EXPECT_EQ(recordText(std::string(250, 'a')).size(), 200u);
  EXPECT_EQ(recordText(std::string(250, 'a'), 10), std::string(10, 'a'));
}

// The open deadline: a reader's own shorter timeout passes unchanged, anything
// else gets the deadline; a test's deadline replaces it, below the floor too.
TEST(OpenPolicy, OpenDeadline) {
  EXPECT_EQ(kOpenDeadlineS, 60);
  EXPECT_EQ(openDeadlineS(0), 60);
  EXPECT_EQ(openDeadlineS(10), 10);
  EXPECT_EQ(openDeadlineS(45), 45);
  EXPECT_EQ(openDeadlineS(120), 60);
  EXPECT_EQ(openDeadlineS(60), 60);
  EXPECT_EQ(openDeadlineS(-1), 60);
  // A reader that gives none is called with the client's RequestTimeout: the
  // default 1800 gets the deadline, a RequestTimeout of 40 is kept.
  EXPECT_EQ(openDeadlineS(1800), 60);
  EXPECT_EQ(openDeadlineS(40), 40);
  EXPECT_EQ(openDeadlineS(0, 5), 5);
  EXPECT_EQ(openDeadlineS(10, 5), 5);
  EXPECT_EQ(openDeadlineS(3, 5), 3);
  EXPECT_EQ(openDeadlineS(0, -5), 60) << "only a positive value replaces the deadline";
}

// ---- Opening a file at another site --------------------------------------

namespace {
// A regional redirector, then a data server: the federation's usual chain.
ChainView regionalThen(uint32_t serverFlags) {
  return readChain({hop("regional.example.org", kManager), hop("d.site.org", serverFlags)});
}
const ChainView kDirect = readChain({hop("d.site.org", kServer)});
// The redirector answered itself (or never answered).
const ChainView kRedirectorOnly = readChain({hop("regional.example.org", kManager)});
OpenStep step(const ChainView& v, int code, int errNo = 0, bool excluded = false) {
  return stepAfter(v, code, errNo, false, false, excluded);
}
} // namespace

TEST(OpenPolicy, ChainReadsTheEffectiveServerAndTheRegional) {
  const std::vector<Hop> fed = {hop("global.example.org", kMeta, true),
                                hop("regional.example.org", kManager),
                                hop("data.site.example.org", kServer)};
  ChainView v = readChain(fed);
  EXPECT_TRUE(v.viaRedirector);
  EXPECT_EQ(v.regional, 1) << "the first redirector that is not a global one";
  EXPECT_EQ(v.effective, 2);
  // A server that never answered (flags 0) is still the one to name.
  v = readChain({hop("regional.example.org", kManager), makeHop("a.site.org", 1094, 0, false, "")});
  EXPECT_EQ(v.effective, 1);
  // A data server that sent the open on to another is the client's choice.
  v = readChain({hop("regional.example.org", kManager), hop("pool.site.org", kServer),
                 hop("disk.site.org", kServer)});
  EXPECT_EQ(v.effective, 1);
  EXPECT_EQ(v.last, 2);
  // The last host a redirector: no server to name.
  EXPECT_EQ(kRedirectorOnly.effective, -1);
  EXPECT_TRUE(kRedirectorOnly.viaRedirector);
  EXPECT_EQ(kRedirectorOnly.regional, 0);
  // Only a global redirector: no regional.
  EXPECT_EQ(readChain({hop("global.example.org", kMeta, true), hop("d.site.org", kServer)}).regional,
            -1);
  EXPECT_FALSE(kDirect.viaRedirector);
  EXPECT_EQ(kDirect.effective, 0);
  EXPECT_EQ(readChain({}).effective, -1);
}

// Every row of the table of what follows an attempt.
TEST(OpenPolicy, StepAfterAnAttempt) {
  const ChainView v = regionalThen(kServer);
  // Any failure with no redirector in the chain is final: a direct URL has no
  // other copy to go to, and a redirector that never answered shows no flags.
  for (int code : {kErrConnectionError, kErrOperationExpired, kErrRedirectLimit, kErrNotFound})
    EXPECT_EQ(step(kDirect, code), OpenStep::kFinal) << code;
  for (int errNo : {kXrNotAuthorized, kXrNotFound, kXrServerError})
    EXPECT_EQ(step(kDirect, kErrErrorResponse, errNo), OpenStep::kFinal) << errNo;
  const ChainView unanswered = readChain({makeHop("regional.example.org", 1094, 0, false, "")});
  EXPECT_EQ(step(unanswered, kErrConnectionError), OpenStep::kFinal);
  EXPECT_EQ(step(unanswered, kErrOperationExpired), OpenStep::kFinal);
  // The redirector itself refused, stalled or failed: no other redirector to ask.
  for (int code : {kErrConnectionError, kErrSocketTimeout, kErrOperationExpired, kErrSocketError,
                   kErrTlsError, kErrStreamDisconnect})
    EXPECT_EQ(step(kRedirectorOnly, code), OpenStep::kFinal) << code;
  EXPECT_EQ(step(kRedirectorOnly, kErrErrorResponse, kXrNotAuthorized), OpenStep::kFinal);
  EXPECT_EQ(step(kRedirectorOnly, kErrErrorResponse, kXrServerError), OpenStep::kFinal);
  // A refusal at a site: another site may take this client.
  EXPECT_EQ(step(v, kErrErrorResponse, kXrNotAuthorized), OpenStep::kFailOver);
  // Not found by the redirector itself, nothing of ours named: no site has it.
  EXPECT_EQ(step(kRedirectorOnly, kErrErrorResponse, kXrNotFound), OpenStep::kFinal);
  EXPECT_EQ(step(kRedirectorOnly, kErrNotFound), OpenStep::kFinal);
  // Not found with servers of ours named, or at a site: asking where the copies
  // are (not done in this version).
  EXPECT_EQ(step(kRedirectorOnly, kErrErrorResponse, kXrNotFound, true), OpenStep::kLocate);
  EXPECT_EQ(step(v, kErrErrorResponse, kXrNotFound), OpenStep::kLocate);
  EXPECT_EQ(step(v, kErrNotFound), OpenStep::kLocate);
  // Bounced past the redirect limit: anywhere in a federation.
  EXPECT_EQ(step(v, kErrRedirectLimit), OpenStep::kFailOver);
  EXPECT_EQ(step(kRedirectorOnly, kErrRedirectLimit), OpenStep::kFailOver);
  // A site that stalled or could not be reached.
  for (int code : {kErrOperationExpired, kErrSocketTimeout, kErrConnectionError})
    EXPECT_EQ(step(v, code), OpenStep::kFailOver) << code;
  // A server's or its storage's failure.
  for (int errNo : {kXrFileLocked, kXrFSError, kXrIOError, kXrServerError, kXrNoServer,
                    kXrCancelled, kXrItExists, kXrOverloaded})
    EXPECT_EQ(step(v, kErrErrorResponse, errNo), OpenStep::kFailOver) << errNo;
  // Anything else is final.
  for (int code : {kErrSocketError, kErrSocketDisconnected, kErrStreamDisconnect, kErrTlsError,
                   kErrAuthFailed, kErrInvalidSession, kErrDataError, 101, 7})
    EXPECT_EQ(step(v, code), OpenStep::kFinal) << code;
  EXPECT_EQ(step(v, kErrErrorResponse, 3013), OpenStep::kFinal); // unsupported
  // A success is final where it lands, unless it serves another size than the
  // cache holds or its copy is on tape -- and only through a redirector.
  EXPECT_EQ(step(v, 0), OpenStep::kFinal);
  EXPECT_EQ(stepAfter(v, 0, 0, true, false, false), OpenStep::kFailOver);
  EXPECT_EQ(stepAfter(v, 0, 0, false, true, false), OpenStep::kFailOver);
  EXPECT_EQ(stepAfter(kDirect, 0, 0, true, false, false), OpenStep::kFinal);
  EXPECT_EQ(stepAfter(kDirect, 0, 0, false, true, false), OpenStep::kFinal);
  // The open of a read that moves: a success that landed where reads stopped
  // asks the regional redirector for the copies, through a redirector only; a
  // size or tape failure goes to another site as before.
  EXPECT_EQ(stepAfter(v, 0, 0, false, false, false, true), OpenStep::kLocate);
  EXPECT_EQ(stepAfter(v, 0, 0, true, false, false, true), OpenStep::kFailOver);
  EXPECT_EQ(stepAfter(v, 0, 0, false, true, false, true), OpenStep::kFailOver);
  EXPECT_EQ(stepAfter(kDirect, 0, 0, false, false, false, true), OpenStep::kFinal);
  // A failure is judged as without it.
  EXPECT_EQ(stepAfter(v, kErrOperationExpired, 0, false, false, false, true), OpenStep::kFailOver);
  EXPECT_EQ(stepAfter(v, kErrSocketError, 0, false, false, false, true), OpenStep::kFinal);
  // A server that did not answer within the deadline: with a regional redirector
  // to ask, the copies are asked for, as a retry through the redirector would
  // meet the site's other server; without one, another site is tried as before.
  // The redirector's own stall and a direct open stay final, and no other
  // failure changes.
  const auto withRegional = [](const ChainView& c, int code, int errNo = 0) {
    return stepAfter(c, code, errNo, false, false, false, false, true);
  };
  EXPECT_EQ(withRegional(v, kErrOperationExpired), OpenStep::kLocate);
  EXPECT_EQ(step(v, kErrOperationExpired), OpenStep::kFailOver);
  EXPECT_EQ(withRegional(kRedirectorOnly, kErrOperationExpired), OpenStep::kFinal);
  EXPECT_EQ(withRegional(kDirect, kErrOperationExpired), OpenStep::kFinal);
  for (int code : {kErrSocketTimeout, kErrConnectionError, kErrRedirectLimit})
    EXPECT_EQ(withRegional(v, code), OpenStep::kFailOver) << code;
  for (int errNo : {kXrNotAuthorized, kXrServerError, kXrIOError})
    EXPECT_EQ(withRegional(v, kErrErrorResponse, errNo), OpenStep::kFailOver) << errNo;
  EXPECT_EQ(withRegional(v, kErrErrorResponse, kXrNotFound), OpenStep::kLocate);
  EXPECT_EQ(withRegional(v, kErrSocketError), OpenStep::kFinal);
}

// A server or site that failed before, as an attempt that succeeded sees it:
// by server, or by the domain of a named host; never by an address, which the
// servers of one machine share.
TEST(OpenPolicy, FailedBefore) {
  const SiteFailure a{"a.one.org:1094", "one.org", "a.one.org", "stopped"};
  const SiteFailure lo1{"127.0.0.1:1095", "127.0.0.1", "", "stopped"};
  const SiteFailure silent{"d.four.org:1094", "d.four.org:1094", "d.four.org", "never answered"};
  const SiteFailure v6{"::1:1093", "::1", "", "stopped"};
  const std::vector<SiteFailure> failed = {a, lo1, silent, v6};
  EXPECT_TRUE(failedBefore(failed, a));
  EXPECT_TRUE(failedBefore(failed, {"a2.one.org:1094", "one.org", "a2.one.org", ""}))
      << "another server of a site that failed";
  EXPECT_FALSE(failedBefore(failed, {"b.two.org:1094", "two.org", "b.two.org", ""}));
  EXPECT_TRUE(failedBefore(failed, lo1));
  EXPECT_FALSE(failedBefore(failed, {"127.0.0.1:1096", "127.0.0.1", "", ""}))
      << "an address is its own server alone";
  EXPECT_TRUE(failedBefore(failed, v6));
  EXPECT_FALSE(failedBefore(failed, {"::1:1094", "::1", "", ""}));
  EXPECT_FALSE(failedBefore(failed, {"d.four.org:1095", "d.four.org:1095", "d.four.org", ""}))
      << "a server that never answered is its own site";
  EXPECT_TRUE(failedBefore(failed, silent));
  EXPECT_FALSE(failedBefore({}, a));
}

// ---- Reading a file from another site --------------------------------------

// The four conditions of a handle that moves its reads.
TEST(OpenPolicy, ArmedForReads) {
  EXPECT_TRUE(armedForReads(true, true, true, false));
  EXPECT_FALSE(armedForReads(false, true, true, false)) << "the switch is off";
  EXPECT_FALSE(armedForReads(true, false, true, false)) << "a pass-through handle";
  EXPECT_FALSE(armedForReads(true, true, false, false)) << "no redirector in any open";
  EXPECT_FALSE(armedForReads(true, true, true, true)) << "shown a slot layout";
}

// The deadline of a test's choosing: unset, empty, not a number, not positive
// and out of range all give the built-in one.
TEST(OpenPolicy, ReadDeadlineOverride) {
  EXPECT_EQ(kReadDeadlineS, 300);
  EXPECT_EQ(readDeadlineS(nullptr), 300);
  EXPECT_EQ(readDeadlineS(""), 300);
  EXPECT_EQ(readDeadlineS("abc"), 300);
  EXPECT_EQ(readDeadlineS("8x"), 300);
  EXPECT_EQ(readDeadlineS("0"), 300);
  EXPECT_EQ(readDeadlineS("-5"), 300);
  EXPECT_EQ(readDeadlineS("99999999999"), 300);
  EXPECT_EQ(readDeadlineS("8"), 8) << "below the floor of 30: a test's";
  EXPECT_EQ(readDeadlineS("120"), 120) << "a test's value that is the old built-in one";
}

// The timeout of an attempt of a read, with the built-in limit of 300 s and
// the client's default request timeout of 1800 s.
namespace {
constexpr int K = 300, RT = 1800;
int first(int reader, int rt = RT, int k = K) {
  return readAttemptTimeoutS(reader, rt, k, 0, false);
}
int again(int reader, int elapsed, int rt = RT, int k = K) {
  return readAttemptTimeoutS(reader, rt, k, elapsed, true);
}
} // namespace
TEST(OpenPolicy, ReadAttemptTimeout) {
  EXPECT_EQ(readBudgetS(0, RT), 1800);
  EXPECT_EQ(readBudgetS(45, RT), 45);
  EXPECT_EQ(readBudgetS(0, 0), kReadDeadlineS) << "no budget at all falls back to the deadline";
  // The first attempt: the deadline, or the reader's shorter timeout unchanged.
  EXPECT_EQ(first(0), 300);
  EXPECT_EQ(first(10), 10);
  EXPECT_EQ(first(30), 30);
  EXPECT_EQ(first(45), 45);
  EXPECT_EQ(first(600), 300);
  EXPECT_EQ(first(300), 300);
  EXPECT_EQ(first(0, 40), 40) << "a request timeout of 40 s is kept";
  EXPECT_EQ(first(0, 20), 20) << "a first attempt is never refused for the time left";
  // After a failed attempt: what is left of the budget, nothing once it is below
  // the floor (30 s, and for a reader with a timeout of its own half of it).
  EXPECT_EQ(again(0, 1790), 0) << "10 s left";
  EXPECT_EQ(again(0, 1769), 31);
  EXPECT_EQ(again(0, 1770), 30) << "30 s left: the floor itself is allowed";
  EXPECT_EQ(again(0, 1771), 0) << "29 s left: below the floor";
  EXPECT_EQ(again(100, 70), 30);
  EXPECT_EQ(again(100, 71), 0) << "a reader with 100 s: the floor is 30, 29 left";
  EXPECT_EQ(again(0, 1000), 300);
  EXPECT_EQ(again(100, 60), 40);
  EXPECT_EQ(again(30, 0), 30) << "a refusal at once, from a reader that gives 30 s";
  EXPECT_EQ(again(30, 15), 15);
  EXPECT_EQ(again(30, 16), 0) << "14 s left, below half of the reader's 30";
  EXPECT_EQ(again(10, 0), 10);
  EXPECT_EQ(again(10, 6), 0);
  EXPECT_EQ(again(1, 0), 1);
  EXPECT_EQ(again(0, 1800), 0);
  EXPECT_EQ(again(0, 2000), 0);
  // A reader's own expiry ends the budget: nothing after it.
  EXPECT_EQ(again(45, 45), 0);
  // A test's deadline of 5 s.
  EXPECT_EQ(first(0, RT, 5), 5);
  EXPECT_EQ(first(10, RT, 5), 5);
  EXPECT_EQ(again(0, 100, RT, 5), 5);
  EXPECT_EQ(again(0, 1795, RT, 5), 5);
  EXPECT_EQ(again(0, 1796, RT, 5), 0) << "the floor is 5 here: 4 left is below it";
}

// Every status of a failed read and what follows it.
TEST(OpenPolicy, ReadStepClasses) {
  EXPECT_EQ(kReadMaxAttempts, 4);
  // The statuses the client's own read recovery handles (102, 103, 109, 110, 7
  // and 207 among them), the stream errors and an expired request.
  for (int code : {7, kErrSocketError, kErrSocketTimeout, kErrSocketDisconnected, 105, 106,
                   kErrStreamDisconnect, kErrConnectionError, kErrInvalidSession, kErrTlsError,
                   kErrOperationExpired, 207})
    EXPECT_EQ(readStep(code, 0), ReadStep::kMove) << code;
  // A server's report of a fault of its own, its storage or its credentials.
  for (int errNo : {3004, kXrFSError, kXrIOError, 3008, kXrNotAuthorized, kXrNotFound,
                    kXrServerError, kXrNoServer, kXrOverloaded, 3034})
    EXPECT_EQ(readStep(kErrErrorResponse, errNo), ReadStep::kMove) << errNo;
  // A corrupted answer is the integrity check's, a bad request no site's fault.
  EXPECT_EQ(readStep(kErrDataError, 0), ReadStep::kFinal);
  for (int errNo : {3000, 3001, 3002, 3006, 3013, 3015, 3016, 3019, kXrCancelled, kXrItExists,
                    kXrFileLocked, 0})
    EXPECT_EQ(readStep(kErrErrorResponse, errNo), ReadStep::kFinal) << errNo;
  for (int code : {0, 3 /* errInvalidOp */, 9 /* errInvalidArgs */, 13 /* errNotSupported */,
                   kErrAuthFailed, 303, kErrNotFound, kErrRedirectLimit, 101, 402})
    EXPECT_EQ(readStep(code, 0), ReadStep::kFinal) << code;
}

// What a retry's tried= may carry for a hop.
TEST(OpenPolicy, ExclusionNames) {
  const std::vector<Hop> hops = {
      hop("regional.example.org", kManager),       // 0: the first host
      hop("Site-Redirector.Example.ORG", kManager), // 1: a redirector
      hop("Data1.Site.Org.", kServer),             // 2: a full name
      hop("127.0.0.1", kServer),                   // 3: an address
      hop("localhost", kServer),                   // 4: no dot
      hop("[2001:db8::7]", kServer),               // 5: an address
      hop("fst.site.org", kServer, false, "cap.msg=x&cap.sym=y"), // 6: behind an EOS front end
      makeHop("stalled.site.org", 1094, 0, false, ""),            // 7: never answered
  };
  EXPECT_EQ(exclusionName(hops, 0), "");
  EXPECT_EQ(exclusionName(hops, 1), "");
  EXPECT_EQ(exclusionName(hops, 2), "data1.site.org");
  EXPECT_EQ(exclusionName(hops, 3), "");
  EXPECT_EQ(exclusionName(hops, 4), "");
  EXPECT_EQ(exclusionName(hops, 5), "");
  EXPECT_EQ(exclusionName(hops, 6), "");
  EXPECT_EQ(exclusionName(hops, 7), "stalled.site.org");
  EXPECT_EQ(exclusionName(hops, -1), "");
  EXPECT_EQ(exclusionName(hops, 8), "");
  // The first host is never named, even a data server reached directly.
  EXPECT_EQ(exclusionName({hop("d.site.org", kServer)}, 0), "");
}

// An EOS site: the global, the regional, the EOS front end (a redirector) and a
// data server reached with the front end's capability. Its failure names
// nothing of ours, so the next landing on that site is a repeat; with the
// front end logged in as a data server, the front end is the site's entry.
TEST(OpenPolicy, EosChain) {
  std::vector<Hop> hops = {hop("global.example.org", kMeta, true),
                           hop("regional.example.org", kManager),
                           hop("eos.site.org", kManager),
                           hop("fst.site.org", kServer, false, "cap.msg=x&cap.sym=y")};
  ChainView v = readChain(hops);
  EXPECT_EQ(step(v, kErrErrorResponse, kXrIOError), OpenStep::kFailOver);
  EXPECT_EQ(v.effective, 3);
  EXPECT_EQ(exclusionName(hops, v.effective), "");
  std::vector<SiteFailure> failed;
  EXPECT_EQ(afterFailure(failed, {"fst.site.org:1094", "site.org", "", "e"}, 4, false),
            AfterFailure::kRetry);
  EXPECT_TRUE(exclusionNames(failed).empty());
  // The next attempt lands on another data server of that site.
  EXPECT_EQ(afterFailure(failed, {"fst2.site.org:1094", "site.org", "", "e"}, 4, false),
            AfterFailure::kRepeat);
  hops[2].flags = kServer;
  v = readChain(hops);
  EXPECT_EQ(v.effective, 2);
  EXPECT_EQ(exclusionName(hops, v.effective), "eos.site.org");
}

// The budget of one open's attempts, and when nothing more may be issued.
TEST(OpenPolicy, BudgetAndStopRule) {
  EXPECT_EQ(kFailoverMaxS, 180);
  EXPECT_EQ(kFailoverMaxSites, 4);
  EXPECT_EQ(budgetS(0, 1800, 15), 180); // the client's defaults
  EXPECT_EQ(budgetS(0, 180, 15), 160);  // the switch's error window: ends before it
  EXPECT_EQ(budgetS(10, 1800, 15), 10); // a reader's own timeout
  EXPECT_EQ(budgetS(30, 1800, 15), 30);
  EXPECT_EQ(budgetS(10, 180, 15), 10);
  EXPECT_EQ(budgetS(0, 180, 1, 8), 8); // a test's bound
  EXPECT_EQ(budgetS(0, 10, 15), 1) << "never below 1";
  // Nothing new once less than one attempt's deadline (capped at 30 s) is left.
  EXPECT_FALSE(budgetSpent(30000, 60, 0));
  EXPECT_TRUE(budgetSpent(29999, 60, 0));
  EXPECT_FALSE(budgetSpent(10000, 10, 0));
  EXPECT_TRUE(budgetSpent(9999, 10, 0));
  EXPECT_TRUE(budgetSpent(4000, 5, 0));
  // A reader with a short timeout of its own: half of it, so that a reader
  // that gives every open 30 s still gets a second attempt.
  EXPECT_FALSE(budgetSpent(15000, openDeadlineS(30), 30));
  EXPECT_TRUE(budgetSpent(14999, openDeadlineS(30), 30));
  EXPECT_FALSE(budgetSpent(5000, openDeadlineS(10), 10));
  EXPECT_TRUE(budgetSpent(4999, openDeadlineS(10), 10));
  EXPECT_TRUE(budgetSpent(-1, 60, 0));
}

// What follows a fail-over outcome.
TEST(OpenPolicy, AfterFailure) {
  std::vector<SiteFailure> failed;
  const SiteFailure a{"a.one.org:1094", "one.org", "a.one.org", "[ERROR] refused"};
  const SiteFailure a2{"a2.one.org:1094", "one.org", "a2.one.org", "[ERROR] refused"};
  const SiteFailure b{"b.two.org:1094", "two.org", "b.two.org", "[ERROR] refused"};
  const SiteFailure c{"127.0.0.1:1095", "127.0.0.1", "", "[ERROR] refused"};
  EXPECT_EQ(afterFailure(failed, a, 4, false), AfterFailure::kRetry);
  EXPECT_EQ(afterFailure(failed, b, 4, false), AfterFailure::kRetry);
  EXPECT_EQ(failed.size(), 2u);
  // Landed on a server that failed already: the names did not keep it away.
  EXPECT_EQ(afterFailure(failed, a, 4, false), AfterFailure::kRepeat);
  EXPECT_EQ(failed.size(), 2u) << "a server is listed once";
  // Or on another server of a site that failed: added, and a repeat.
  EXPECT_EQ(afterFailure(failed, a2, 4, false), AfterFailure::kRepeat);
  EXPECT_EQ(failed.size(), 3u);
  EXPECT_EQ(exclusionNames(failed), (std::vector<std::string>{"a.one.org", "b.two.org", "a2.one.org"}));
  // A server that cannot be named adds no name.
  EXPECT_EQ(afterFailure(failed, c, 4, false), AfterFailure::kRetry);
  EXPECT_EQ(exclusionNames(failed).size(), 3u);
  // The bound of sites: three distinct ones here.
  std::vector<SiteFailure> f3 = failed;
  EXPECT_EQ(afterFailure(f3, {"d.four.org:1094", "four.org", "d.four.org", "e"}, 4, false),
            AfterFailure::kStop);
  std::vector<SiteFailure> one;
  EXPECT_EQ(afterFailure(one, a, 1, false), AfterFailure::kStop) << "one site allowed";
  std::vector<SiteFailure> two;
  EXPECT_EQ(afterFailure(two, a, 2, false), AfterFailure::kRetry);
  EXPECT_EQ(afterFailure(two, b, 2, false), AfterFailure::kStop);
  // The budget spent: stop, whatever else holds.
  std::vector<SiteFailure> late;
  EXPECT_EQ(afterFailure(late, a, 4, true), AfterFailure::kStop);
  EXPECT_EQ(late.size(), 1u) << "the failure is still named";
}

// The error of an open that failed at every site it tried.
TEST(OpenPolicy, AllSitesFailedText) {
  const std::vector<SiteFailure> failed = {
      {"a.one.org:1094", "one.org", "a.one.org", "[ERROR] Server responded with an error: [3010] no"},
      {"b.two.org:1094", "two.org", "", "[ERROR] at root://b.two.org:1094//f?authz=SECRET denied"}};
  const std::string t = allSitesFailedText("root://r:1094//store/f.root", failed);
  EXPECT_EQ(t, "uCache: root://r:1094//store/f.root: no site could open it: one.org "
               "(a.one.org:1094): [ERROR] Server responded with an error: [3010] no; two.org "
               "(b.two.org:1094): [ERROR] at root://b.two.org:1094//f denied");
  EXPECT_EQ(t.find("SECRET"), std::string::npos);
  EXPECT_EQ(t.substr(t.find("one.org (")), sitesFailedList(failed)) << "the list alone, for the log";
  // Each error at most 160 bytes, the whole at most 1024.
  std::vector<SiteFailure> many(20, {"s.x.org:1094", "x.org", "s.x.org", std::string(500, 'e')});
  const std::string m = allSitesFailedText("root://r//f", many);
  EXPECT_LE(m.size(), 1024u);
  EXPECT_NE(m.find(std::string(160, 'e') + ";"), std::string::npos);
  EXPECT_EQ(m.find(std::string(161, 'e')), std::string::npos);
  EXPECT_STREQ(openRouteName(OpenRoute::kFailover), "failover");
}

// The copies a locate lists, in the order they are tried.
TEST(OpenPolicy, OrderLocations) {
  const auto loc = [](const char* host, int port, bool manager = false, bool pending = false) {
    Location l;
    l.host = host;
    l.port = port;
    l.manager = manager;
    l.pending = pending;
    return l;
  };
  const auto names = [](const std::vector<Location>& v) {
    std::string s;
    for (const Location& l : v)
      s += l.host + ":" + std::to_string(l.port) + " ";
    return s;
  };
  // Online before pending, each in the answer's order.
  const std::vector<Location> listed = {loc("p.one.org", 1094, false, true), loc("a.two.org", 1094),
                                        loc("m.three.org", 1094, true), loc("b.four.org", 1094)};
  EXPECT_EQ(names(orderLocations(listed, {}, {})),
            "a.two.org:1094 m.three.org:1094 b.four.org:1094 p.one.org:1094 ");
  // The reader's own tried= hosts are never used, with a port or without, in any case.
  EXPECT_EQ(names(orderLocations(listed, {"A.TWO.org", "b.four.org:1094"}, {})),
            "m.three.org:1094 p.one.org:1094 ");
  // A server that failed, and the other servers of its site, are left out while
  // others remain; an address stands for its own server alone.
  const std::vector<SiteFailure> failed = {{"x.two.org:1094", "two.org", "x.two.org", "e"},
                                           {"127.0.0.1:12122", "127.0.0.1", "", "e"}};
  const std::vector<Location> mixed = {loc("a.two.org", 1094), loc("127.0.0.1", 12122),
                                       loc("127.0.0.1", 12121), loc("b.four.org", 1094)};
  EXPECT_EQ(names(orderLocations(mixed, {}, failed)), "127.0.0.1:12121 b.four.org:1094 ");
  // Nothing else left: the failed ones, the oldest failure first; the reader's
  // never.
  const std::vector<Location> only = {loc("127.0.0.1", 12122), loc("a.two.org", 1094),
                                      loc("r.five.org", 1094)};
  EXPECT_EQ(names(orderLocations(only, {"r.five.org"}, failed)), "a.two.org:1094 127.0.0.1:12122 ");
  // An EOS site's front end is listed beside a data server that failed: it is
  // of the same site, so it is tried when nothing else is left.
  EXPECT_EQ(names(orderLocations({loc("eos.site.org", 1094, true)}, {},
                                 {{"fst.site.org:1094", "site.org", "", "e"}})),
            "eos.site.org:1094 ");
  EXPECT_TRUE(orderLocations({}, {}, failed).empty());
  // A server that never answered is its own site (host:port), yet the other
  // servers in the domain of its name are left out with it: they stall as well.
  const std::vector<SiteFailure> stalled = {
      {"red2.site.org:1094", "red2.site.org:1094", "red2.site.org", "no answer"}};
  const std::vector<Location> sibling = {loc("red1.site.org", 1094), loc("red2.site.org", 1094),
                                         loc("a.two.org", 1094)};
  EXPECT_EQ(names(orderLocations(sibling, {}, stalled)), "a.two.org:1094 ");
  const std::vector<Location> siteOnly = {loc("red1.site.org", 1094), loc("red2.site.org", 1094)};
  EXPECT_EQ(names(orderLocations(siteOnly, {}, stalled)), "red1.site.org:1094 red2.site.org:1094 ")
      << "nothing else left: the site's servers, in the answer's order";
  EXPECT_STREQ(openRouteName(OpenRoute::kCandidate), "candidate");
}

// The address of a copy in a locate's answer.
TEST(OpenPolicy, LocationOfAnAddress) {
  const struct {
    const char* address;
    const char* host;
    int port;
  } forms[] = {
      {"cms-xrd-global.cern.ch:1094", "cms-xrd-global.cern.ch", 1094},
      {"Data1.Site.ORG:2094", "Data1.Site.ORG", 2094}, // the case is kept: matching lowercases
      {"10.1.2.3:1095", "10.1.2.3", 1095},
      {"[2001:db8::7]:1094", "2001:db8::7", 1094},
      {"[::1]:1094", "::1", 1094},
      {"[::127.0.0.1]:12122", "127.0.0.1", 12122}, // an IPv4 address in its IPv6 form
      {"[::ffff:127.0.0.1]:12123", "127.0.0.1", 12123},
      {"::ffff:10.0.0.9:1096", "10.0.0.9", 1096}, // unbracketed: the last colon is the port's
      {"host.example.org", "host.example.org", 1094}, // no port: the default one
  };
  for (const auto& f : forms) {
    const Location l = locationOf(f.address, false, false);
    EXPECT_EQ(l.host, f.host) << f.address;
    EXPECT_EQ(l.port, f.port) << f.address;
  }
  const Location m = locationOf("a.two.org:1094", true, true);
  EXPECT_TRUE(m.manager);
  EXPECT_TRUE(m.pending);
  EXPECT_FALSE(locationOf("a.two.org:1094", false, false).manager);
}

// A copy on tape: its stat flags say so, or a test's list names its server.
TEST(OpenPolicy, CopyOffline) {
  const std::string list = ",127.0.0.1:12122,[::1]:1094,";
  EXPECT_FALSE(copyOffline(0, "a.two.org:1094", nullptr));
  EXPECT_TRUE(copyOffline(kStatOffline, "a.two.org:1094", nullptr));
  EXPECT_TRUE(copyOffline(kStatOffline | 1, "a.two.org:1094", &list)) << "the flag with other flags";
  EXPECT_FALSE(copyOffline(1 | 4, "a.two.org:1094", &list)) << "other flags are not the tape's";
  EXPECT_TRUE(copyOffline(0, "127.0.0.1:12122", &list));
  EXPECT_TRUE(copyOffline(0, "[::1]:1094", &list));
  EXPECT_FALSE(copyOffline(0, "127.0.0.1:1212", &list)) << "a server is named whole: host and port";
  EXPECT_FALSE(copyOffline(0, "0.0.1:12122", &list));
}

// Two rules say that a server or site failed already: failedBefore, for the copy an open
// landed on, and the list of copies (orderLocations), for the ones still to try. They are
// two implementations (one lowercases): each row says what each answers, and the rows
// where they differ do so on purpose. `list`: the list of copies leaves the copy out.
TEST(OpenPolicy, FailedBeforeAndTheListOfCopiesAgree) {
  const std::vector<SiteFailure> failed = {
      {"a.one.org:1094", "one.org", "a.one.org", "e"},
      {"127.0.0.1:1095", "127.0.0.1", "", "e"},
      {"::1:1093", "::1", "", "e"},
      {"d.four.org:1094", "d.four.org:1094", "d.four.org", "never answered"}};
  const auto excludedByList = [&](const std::string& host, int port) {
    Location l, other;
    l.host = host;
    l.port = port;
    other.host = "z.nine.org";
    other.port = 1094;
    const std::vector<Location> r = orderLocations({l, other}, {}, failed);
    return r.size() == 1 && r[0].host == "z.nine.org";
  };
  const struct {
    const char* host;
    int port;
    bool before; // failedBefore
    bool list;   // excludedByList
    const char* why;
  } rows[] = {
      {"a.one.org", 1094, true, true, "the server that failed"},
      {"A.One.ORG", 1094, true, true, "the same, in another case"},
      {"a2.one.org", 1094, true, true, "another server of the site"},
      {"a.one.org", 1095, true, true, "another port of the server: the site failed"},
      {"b.two.org", 1094, false, false, "another site"},
      {"127.0.0.1", 1095, true, true, "an address: the server that failed"},
      {"127.0.0.1", 1096, false, false, "an address is its own server alone"},
      {"::1", 1093, true, true, "an IPv6 address: the server that failed"},
      {"::1", 1094, false, false, "another port of the address"},
      {"d.four.org", 1094, true, true, "a server that never answered"},
      // The list also leaves out the other servers of the domain of a server that never
      // answered, which stall as it did; an open that landed on one is not "failed before".
      {"d.four.org", 1095, false, true, "a server that never answered is its own site"},
      {"e.four.org", 1094, false, true, "a sibling in its domain"},
  };
  for (const auto& r : rows) {
    SiteFailure f;
    f.server = std::string(r.host) + ":" + std::to_string(r.port);
    f.site = siteOf(r.host);
    EXPECT_EQ(failedBefore(failed, f), r.before) << r.host << ":" << r.port << " " << r.why;
    EXPECT_EQ(excludedByList(r.host, r.port), r.list) << r.host << ":" << r.port << " " << r.why;
  }
}

// A copy with no bytes of a file named *.root is a bad copy; a size of zero is an
// ordinary size for any other name.
TEST(OpenPolicy, EmptyRootCopy) {
  EXPECT_TRUE(emptyRootCopy("/store/mc/a/b.root", 0));
  EXPECT_TRUE(emptyRootCopy("root://h:1094//store/a.root", 0)) << "a URL";
  EXPECT_TRUE(emptyRootCopy("root://h:1094//store/a.root?tried=x.org&triedrc=ioerr", 0))
      << "the query is not part of the name";
  EXPECT_TRUE(emptyRootCopy("a.root", 0));
  EXPECT_TRUE(emptyRootCopy("/store/a.root.root", 0));
  // Not empty, or not known: no claim.
  EXPECT_FALSE(emptyRootCopy("/store/a.root", 1));
  EXPECT_FALSE(emptyRootCopy("/store/a.root", 4247184));
  EXPECT_FALSE(emptyRootCopy("/store/a.root", -1)) << "a size that could not be read";
  // Not the name of a ROOT file: an empty file is an ordinary file.
  EXPECT_FALSE(emptyRootCopy("/store/a.dat", 0));
  EXPECT_FALSE(emptyRootCopy("/store/a.root.gz", 0));
  EXPECT_FALSE(emptyRootCopy("/store/a.root/", 0)) << "a trailing slash";
  EXPECT_FALSE(emptyRootCopy("/store/aroot", 0)) << "no dot";
  EXPECT_FALSE(emptyRootCopy("/store/a.dat?name=b.root", 0)) << "only the query ends in .root";
  EXPECT_FALSE(emptyRootCopy("/store/a.ROOT", 0)) << "only the lower-case .root counts";
  EXPECT_FALSE(emptyRootCopy("", 0));
  EXPECT_FALSE(emptyRootCopy("root", 0));
  EXPECT_FALSE(emptyRootCopy(".roo", 0));
  EXPECT_FALSE(emptyRootCopy("?a.root", 0)) << "nothing before the query";
}
