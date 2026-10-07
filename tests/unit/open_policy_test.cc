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
