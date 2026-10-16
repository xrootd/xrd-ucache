// What an origin open's answer says (OpenPolicy.h): the redirect chain read
// from the client's HostList, the site of a host, and the provisional class of
// a failure. Pure and XrdCl-free; the numeric codes are the client's, checked
// against its headers where the plugin is built.
#include "OpenPolicy.h"

#include <gtest/gtest.h>

#include <functional>
#include <map>

#include <climits>
#include <cstring>
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

// What a server answers when asked for its "sitename": the name, on its first line.
TEST(OpenPolicy, SiteNameOfAnAnswer) {
  const auto name = [](const std::string& a) { return siteNameOfAnswer(a.data(), a.size()); };
  EXPECT_EQ(name("T2_S1\n\n"), "T2_S1"); // as an XRootD server answers it
  EXPECT_EQ(name("T1_US_FNAL_Disk"), "T1_US_FNAL_Disk");
  EXPECT_EQ(name("  T2_CH_CERN \r\n"), "T2_CH_CERN");
  EXPECT_EQ(name(std::string("T3_X\0junk", 9)), "T3_X");
  EXPECT_EQ(name("sitename\n"), "") << "a server with no all.sitename answers the key itself";
  EXPECT_EQ(name(""), "");
  EXPECT_EQ(name("\n"), "");
  EXPECT_EQ(name("two words\n"), "") << "not a name";
  EXPECT_EQ(name("bad\x01name"), "");
  EXPECT_EQ(name(std::string(kSiteNameMaxLen, 'a')), std::string(kSiteNameMaxLen, 'a'));
  EXPECT_EQ(name(std::string(kSiteNameMaxLen + 1, 'a')), "") << "too long";
  EXPECT_EQ(siteNameOfAnswer(nullptr, 5), "");
}

// A server's site: the name it gave, else its domain when it answered, else itself.
TEST(OpenPolicy, SiteIdentityPrefersTheName) {
  EXPECT_EQ(siteIdentity("cmsxrootd1.fnal.gov:1094", true, "T1_US_FNAL"), "T1_US_FNAL");
  EXPECT_EQ(siteIdentity("cmsxrootd1.fnal.gov:1094", false, "T1_US_FNAL"), "T1_US_FNAL")
      << "a server that gave its name keeps it when it stalls later";
  EXPECT_EQ(siteIdentity("cmsxrootd1.fnal.gov:1094", true, ""), "fnal.gov");
  EXPECT_EQ(siteIdentity("cmsxrootd1.fnal.gov:1094", false, ""), "cmsxrootd1.fnal.gov:1094");
  EXPECT_EQ(siteIdentity("127.0.0.1:1094", false, "T2_S1"), "T2_S1");
}

// Servers that gave one site name count as one site: four managers of one site stalling use one
// of the bound of sites, not four, and a listed copy of that site is left to the end, an address
// too.
TEST(OpenPolicy, ServersOfOneSiteNameAreOneSite) {
  const std::map<std::string, std::string> names = {{"127.0.0.1:1101", "T1_ONE"},
                                                    {"127.0.0.1:1102", "T1_ONE"},
                                                    {"127.0.0.2:1103", "T1_ONE"},
                                                    {"m4.one.org:1094", "T1_ONE"},
                                                    {"127.0.0.1:1105", "T2_TWO"}};
  const std::function<std::string(const std::string&)> nameOf = [&](const std::string& s) {
    const auto it = names.find(s);
    return it == names.end() ? std::string() : it->second;
  };
  // Four servers of T1_ONE that did not answer (each its own host:port until named).
  std::vector<SiteFailure> failed;
  const auto stalled = [](const std::string& server) {
    SiteFailure f;
    f.server = server;
    f.site = server;
    f.error = "expired";
    return f;
  };
  AfterFailure last = AfterFailure::kRetry;
  for (const char* s : {"127.0.0.1:1101", "127.0.0.1:1102", "127.0.0.2:1103"}) {
    SiteFailure f = stalled(s);
    f.siteName = nameOf(f.server);
    f.domain = f.server;
    f.site = siteIdentity(f.server, false, f.siteName);
    setSiteNames(failed, nameOf);
    last = afterFailure(failed, f, 2, false);
  }
  EXPECT_NE(last, AfterFailure::kStop) << "three servers of one named site are one of 2 sites";
  // Unnamed (the names unknown), they are three sites, and the bound of 2 stops the sequence.
  std::vector<SiteFailure> unnamed;
  last = AfterFailure::kRetry;
  for (const char* s : {"127.0.0.1:1101", "127.0.0.1:1102", "127.0.0.2:1103"})
    last = afterFailure(unnamed, stalled(s), 2, false);
  EXPECT_EQ(last, AfterFailure::kStop);

  // The regional lists m4 (T1_ONE, a name), 127.0.0.1:1105 (T2_TWO), 127.0.0.2:1103 (T1_ONE, an
  // address): the copies of T1_ONE are left out while another site's is listed.
  Location m4, two, three;
  m4.host = "m4.one.org";
  m4.port = 1094;
  two.host = "127.0.0.1";
  two.port = 1105;
  three.host = "127.0.0.2";
  three.port = 1103;
  std::vector<SiteFailure> one = {stalled("127.0.0.1:1101")};
  setSiteNames(one, nameOf);
  EXPECT_EQ(one.front().site, "T1_ONE");
  auto order = orderLocations({m4, three, two}, {}, one, nameOf);
  ASSERT_EQ(order.size(), 1u);
  EXPECT_EQ(serverOf(order.front()), "127.0.0.1:1105");
  // Without names, the address 127.0.0.2:1103 and m4 (another domain than the failed address)
  // are taken first, in the list's order.
  order = orderLocations({m4, three, two}, {}, {stalled("127.0.0.1:1101")});
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order.front().host, "m4.one.org");
  // A site that failed and is all that is listed: its copies are given, as before.
  order = orderLocations({m4, three}, {}, one, nameOf);
  EXPECT_EQ(order.size(), 2u);
  // A failed server whose name became known after it failed is renamed: its site is the name.
  std::vector<SiteFailure> late = {stalled("127.0.0.1:1102")};
  EXPECT_EQ(late.front().site, "127.0.0.1:1102");
  setSiteNames(late, nameOf);
  EXPECT_EQ(late.front().site, "T1_ONE");
  SiteFailure other = stalled("127.0.0.1:1101");
  other.siteName = "T1_ONE";
  EXPECT_TRUE(failedBefore(late, other));
  EXPECT_EQ(serverOf(Location{"2001:DB8::7", 1094, false, false}), "[2001:db8::7]:1094");
}

// Mixed identities: a server whose name is known and one whose name is not known yet are compared by
// their domains (as before site names); two named servers by their names, also within one domain;
// two unnamed by their domains.
TEST(OpenPolicy, ASiteByNamesWhenBothAreKnownElseByDomains) {
  const auto at = [](const std::string& server, bool answered, const std::string& name) {
    SiteFailure f;
    f.server = server;
    f.siteName = name;
    f.domain = siteIdentity(server, answered, "");
    f.site = name.empty() ? f.domain : name;
    f.error = "e";
    return f;
  };
  const SiteFailure aNamed = at("cmsxrd1.fnal.gov:1094", true, "T1_US_FNAL");
  const SiteFailure bUnnamed = at("cmsxrd2.fnal.gov:1094", true, "");
  const SiteFailure bNamed = at("cmsxrd2.fnal.gov:1094", true, "T1_US_FNAL");
  const SiteFailure cOther = at("t3.fnal.gov:1094", true, "T3_US_FNALLPC");
  const SiteFailure aUnnamed = at("cmsxrd1.fnal.gov:1094", true, "");
  EXPECT_TRUE(sameSite(aNamed, bUnnamed)) << "named and not yet named, one domain: one site";
  EXPECT_TRUE(sameSite(aNamed, bNamed));
  EXPECT_FALSE(sameSite(aNamed, cOther)) << "two names in one domain: two sites";
  EXPECT_TRUE(sameSite(aUnnamed, bUnnamed)) << "neither named: by domain";
  EXPECT_FALSE(sameSite(aNamed, at("x.cern.ch:1094", true, ""))) << "another domain";
  // afterFailure: the second server of the domain is a repeat of the site (the regional's list
  // next), and the sites counted are one: the bound of 2 is not reached.
  std::vector<SiteFailure> failed;
  EXPECT_EQ(afterFailure(failed, aNamed, 2, false), AfterFailure::kRetry);
  EXPECT_EQ(afterFailure(failed, bUnnamed, 2, false), AfterFailure::kRepeat);
  std::vector<SiteFailure> two;
  EXPECT_EQ(afterFailure(two, aNamed, 2, false), AfterFailure::kRetry);
  EXPECT_EQ(afterFailure(two, cOther, 2, false), AfterFailure::kStop) << "two named sites";
  // failedBefore: the same.
  EXPECT_TRUE(failedBefore({aNamed}, bUnnamed));
  EXPECT_TRUE(failedBefore({bUnnamed}, aNamed));
  EXPECT_FALSE(failedBefore({aNamed}, cOther));
  // An address: by domain its own server alone; by names, of the site of its name.
  const SiteFailure l1 = at("127.0.0.1:1101", true, ""), l2 = at("127.0.0.1:1102", true, "");
  EXPECT_FALSE(failedBefore({l1}, l2));
  EXPECT_TRUE(failedBefore({at("127.0.0.1:1101", true, "T2_S1")}, at("127.0.0.1:1102", true, "T2_S1")));
  // A listed copy: named, against a failure whose name is not known yet, by domain; an address
  // only by names.
  EXPECT_TRUE(atFailedSite(bUnnamed, "cmsxrd1.fnal.gov", "T1_US_FNAL"));
  EXPECT_TRUE(atFailedSite(aNamed, "cmsxrd2.fnal.gov", ""));
  EXPECT_FALSE(atFailedSite(aNamed, "t3.fnal.gov", "T3_US_FNALLPC"));
  EXPECT_FALSE(atFailedSite(l1, "127.0.0.1", ""));
  EXPECT_TRUE(atFailedSite(at("127.0.0.1:1101", true, "T2_S1"), "127.0.0.1", "T2_S1"));
  // A stalled server (never answered) of a named site's domain: its host's domain is where its
  // siblings are, as before.
  EXPECT_TRUE(atFailedSite(at("cmsxrd3.fnal.gov:1094", false, ""),
                           "cmsxrd1.fnal.gov", "T1_US_FNAL"));
  Location l;
  l.host = "cmsxrd1.fnal.gov";
  l.port = 1094;
  const std::function<std::string(const std::string&)> named = [](const std::string& s) {
    return s == "cmsxrd1.fnal.gov:1094" ? std::string("T1_US_FNAL") : std::string();
  };
  Location t3 = l;
  t3.host = "t3.cern.ch";
  const auto order = orderLocations({l, t3}, {}, {bUnnamed}, named);
  ASSERT_EQ(order.size(), 1u);
  EXPECT_EQ(order.front().host, "t3.cern.ch") << "the named copy of the failed domain is left out";
}

// What a failure says of the server: a stall at uCache's own time or a quiet connection, a fault
// of its own; never authorization, TLS, resolution, connections, a missing file, bad bytes or the
// job's own timeout.
// The site-fault memory marks the last host an attempt reached, never a redirector, and not the
// earlier server whose own answer sent the client on (the one tried= names).
TEST(OpenPolicy, TheLastHostIsMarked) {
  const std::vector<Hop> notFoundThenStall = {hop("regional.example.org", kManager),
                                              hop("b.site.org", kServer),
                                              makeHop("c.other.org", 1094, 0, false, "")};
  ChainView v = readChain(notFoundThenStall);
  EXPECT_EQ(v.effective, 1) << "tried= names b";
  EXPECT_EQ(markedHop(v, notFoundThenStall), 2) << "c did not answer: c is marked";
  const std::vector<Hop> one = {hop("regional.example.org", kManager), hop("a.site.org", kServer)};
  EXPECT_EQ(markedHop(readChain(one), one), 1);
  const std::vector<Hop> regionalOnly = {hop("global.example.org", kMeta, true),
                                         hop("regional.example.org", kManager)};
  EXPECT_EQ(markedHop(readChain(regionalOnly), regionalOnly), -1) << "a redirector is never marked";
  EXPECT_EQ(markedHop(readChain({}), {}), -1);
}

// A replica layout's request that could not have its slots fails for want of memory only when no
// piece failed as data: a data failure is never hidden behind want of memory.
TEST(OpenPolicy, ADataFailureComesBeforeWantOfMemory) {
  EXPECT_TRUE(slotLossIsWantOfMemory(false, true));
  EXPECT_FALSE(slotLossIsWantOfMemory(true, true)) << "one piece short of memory, another corrupt";
  EXPECT_FALSE(slotLossIsWantOfMemory(true, false));
  EXPECT_FALSE(slotLossIsWantOfMemory(false, false));
}

TEST(OpenPolicy, SiteFaultOfAFailure) {
  EXPECT_EQ(siteFaultOf(kErrOperationExpired, 0, true), SiteFault::kStall);
  EXPECT_EQ(siteFaultOf(kErrOperationExpired, 0, false), SiteFault::kNone) << "the job's own time";
  EXPECT_EQ(siteFaultOf(kErrSocketTimeout, 0, false), SiteFault::kStall);
  EXPECT_EQ(siteFaultOf(kErrSocketTimeout, 0, false, /*jobStream=*/true), SiteFault::kNone)
      << "a stream timeout the job lowered is the job's own";
  EXPECT_TRUE(streamTimeoutIsJobs(5));
  EXPECT_FALSE(streamTimeoutIsJobs(60)) << "the client's default";
  EXPECT_FALSE(streamTimeoutIsJobs(120)) << "raised: a quiet connection still says the server stalled";
  EXPECT_FALSE(streamTimeoutIsJobs(0));
  for (int e : {kXrFSError, kXrIOError, kXrServerError, kXrNoMemory, kXrOverloaded})
    EXPECT_EQ(siteFaultOf(kErrErrorResponse, e, false), SiteFault::kServer) << e;
  for (int e : {kXrNotAuthorized, kXrNotFound, kXrNoServer, kXrFileLocked})
    EXPECT_EQ(siteFaultOf(kErrErrorResponse, e, true), SiteFault::kNone) << e;
  for (int c : {kErrTlsError, kErrAuthFailed, kErrConnectionError, kErrSocketError,
                kErrSocketDisconnected, kErrInvalidSession, kErrNotFound, kErrDataError, 101, 0})
    EXPECT_EQ(siteFaultOf(c, 0, true), SiteFault::kNone) << c;
}

// Marked copies come after every other copy left, the oldest failure first; when every copy is
// marked all are given, oldest first; a copy failed in this sequence still comes only when nothing
// else is left.
TEST(OpenPolicy, MarkedCopiesAreTriedLast) {
  const auto loc = [](const char* h, bool pending = false) {
    Location l;
    l.host = h;
    l.port = 1094;
    l.pending = pending;
    return l;
  };
  const std::map<std::string, uint64_t> at = {{"a.one.org:1094", 500}, {"b.two.org:1094", 300}};
  const std::function<uint64_t(const std::string&)> markedAt = [&](const std::string& s) {
    const auto it = at.find(s);
    return it == at.end() ? uint64_t(0) : it->second;
  };
  const auto hosts = [](const std::vector<Location>& v) {
    std::string out;
    for (const auto& l : v)
      out += l.host + " ";
    return out;
  };
  const std::vector<Location> listed = {loc("a.one.org"), loc("b.two.org"), loc("c.three.org"),
                                        loc("d.four.org", true)};
  EXPECT_EQ(hosts(orderLocations(listed, {}, {}, nullptr, markedAt)),
            "c.three.org d.four.org b.two.org a.one.org ")
      << "unmarked online, unmarked pending, then marked, the oldest failure first";
  EXPECT_EQ(hosts(orderLocations(listed, {}, {})), "a.one.org b.two.org c.three.org d.four.org ")
      << "without marks, the list's order";
  EXPECT_EQ(hosts(orderLocations({loc("a.one.org"), loc("b.two.org")}, {}, {}, nullptr, markedAt)),
            "b.two.org a.one.org ")
      << "every copy marked: all tried, the oldest failure first";
  SiteFailure c;
  c.server = "c.three.org:1094";
  c.site = "three.org";
  EXPECT_EQ(hosts(orderLocations({loc("a.one.org"), loc("c.three.org")}, {}, {c}, nullptr, markedAt)),
            "a.one.org ")
      << "a marked copy before one that failed in this sequence (given only when nothing is left)";
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
  // A site that stalled, could not be reached, or whose connection was lost.
  for (int code : {kErrOperationExpired, kErrSocketTimeout, kErrConnectionError, kErrSocketError,
                   kErrSocketDisconnected, kErrStreamDisconnect, kErrInvalidSession})
    EXPECT_EQ(step(v, code), OpenStep::kFailOver) << code;
  // A server's or its storage's failure.
  for (int errNo : {kXrFileLocked, kXrFSError, kXrIOError, kXrServerError, kXrNoServer,
                    kXrCancelled, kXrItExists, kXrOverloaded})
    EXPECT_EQ(step(v, kErrErrorResponse, errNo), OpenStep::kFailOver) << errNo;
  // Anything else is final: a name that does not resolve, TLS, the handshake, authentication,
  // the client's own local faults.
  for (int code : {kErrTlsError, kErrAuthFailed, kErrHandShakeFailed, kErrDataError, 101, 105, 106,
                   7})
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
  EXPECT_EQ(stepAfter(v, kErrSocketError, 0, false, false, false, true), OpenStep::kFailOver);
  EXPECT_EQ(stepAfter(v, kErrTlsError, 0, false, false, false, true), OpenStep::kFinal);
  // A server that did not answer within the deadline, or whose connection timed
  // out, was lost or could not be made: with a regional redirector to ask, the
  // copies are asked for, as a retry through the redirector may meet the same
  // site again; without one, another site is tried as before. The redirector's
  // own stall and a direct open stay final, and no other failure changes.
  const auto withRegional = [](const ChainView& c, int code, int errNo = 0) {
    return stepAfter(c, code, errNo, false, false, false, false, true);
  };
  EXPECT_EQ(withRegional(v, kErrOperationExpired), OpenStep::kLocate);
  EXPECT_EQ(step(v, kErrOperationExpired), OpenStep::kFailOver);
  EXPECT_EQ(withRegional(kRedirectorOnly, kErrOperationExpired), OpenStep::kFinal);
  EXPECT_EQ(withRegional(kDirect, kErrOperationExpired), OpenStep::kFinal);
  for (int code : {kErrSocketTimeout, kErrConnectionError, kErrSocketError, kErrSocketDisconnected,
                   kErrStreamDisconnect, kErrInvalidSession}) {
    EXPECT_EQ(withRegional(v, code), OpenStep::kLocate) << code;
    EXPECT_EQ(withRegional(kRedirectorOnly, code), OpenStep::kFinal) << code;
    EXPECT_EQ(withRegional(kDirect, code), OpenStep::kFinal) << code;
  }
  for (int code : {kErrRedirectLimit})
    EXPECT_EQ(withRegional(v, code), OpenStep::kFailOver) << code;
  for (int code : {kErrTlsError, kErrAuthFailed, kErrHandShakeFailed, kErrInvalidAddr, 105, 106})
    EXPECT_EQ(withRegional(v, code), OpenStep::kFinal) << code;
  for (int errNo : {kXrNotAuthorized, kXrServerError, kXrIOError})
    EXPECT_EQ(withRegional(v, kErrErrorResponse, errNo), OpenStep::kFailOver) << errNo;
  EXPECT_EQ(withRegional(v, kErrErrorResponse, kXrNotFound), OpenStep::kLocate);
}

// The failures that say a server stopped answering or its connection failed: the client's
// status codes (XrdClStatus.hh), each as the client gives it.
TEST(OpenPolicy, StalledOrLost) {
  for (int code : {kErrOperationExpired, kErrSocketTimeout, kErrConnectionError, kErrSocketError,
                   kErrSocketDisconnected, kErrStreamDisconnect, kErrInvalidSession})
    EXPECT_TRUE(stalledOrLost(code)) << code;
  for (int code : {0, kErrInternal, kErrDataError, kErrInvalidAddr, 105, 106, kErrTlsError,
                   kErrHandShakeFailed, 203, kErrAuthFailed, kErrOperationInterrupted,
                   kErrInvalidResponse, kErrNotFound, kErrRedirectLimit, kErrErrorResponse,
                   kErrRedirect})
    EXPECT_FALSE(stalledOrLost(code)) << code;
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

// The five conditions of a handle that moves its reads.
TEST(OpenPolicy, ArmedForReads) {
  EXPECT_TRUE(armedForReads(true, true, true, false, true));
  EXPECT_FALSE(armedForReads(false, true, true, false, true)) << "the switch is off";
  EXPECT_FALSE(armedForReads(true, false, true, false, true)) << "a pass-through handle";
  EXPECT_FALSE(armedForReads(true, true, false, false, true)) << "no redirector in any open";
  EXPECT_FALSE(armedForReads(true, true, true, true, true)) << "shown a slot layout";
  EXPECT_FALSE(armedForReads(true, true, true, false, false))
      << "no size its reader was shown is known: no copy could be told to be its file";
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

// A miss whose fetch did not bring the reader's bytes: the reader's request is sent again on the
// same File with what is left of its time, unless the fetch ran out of time at its server.
TEST(OpenPolicy, MissRelayTimeout) {
  // The fetch ran out of time at its server: its error is the reader's, whatever time is left
  // (a request sent again would wait for the same server on the same connection as long again).
  for (int code : {kErrOperationExpired, kErrSocketTimeout})
    for (int readerT : {0, 10, 3600})
      for (int elapsed : {0, 9, 10, 60, 1800, 4000})
        EXPECT_EQ(missRelayTimeoutS(code, readerT, RT, elapsed), -1)
            << code << " " << readerT << " " << elapsed;
  // Any other failure, and an answer without the reader's bytes: sent again with what is left of
  // the reader's own timeout, or of the client's request timeout when it gave none.
  for (int code : {kErrConnectionError, kErrSocketError, kErrSocketDisconnected,
                   kErrStreamDisconnect, kErrInvalidSession, kErrErrorResponse, kErrInternal,
                   kErrDataError}) {
    EXPECT_EQ(missRelayTimeoutS(code, 0, RT, 0), 1800) << code;
    EXPECT_EQ(missRelayTimeoutS(code, 0, RT, 30), 1770) << code;
    EXPECT_EQ(missRelayTimeoutS(code, 45, RT, 0), 45) << code;
    EXPECT_EQ(missRelayTimeoutS(code, 45, RT, 44), 1) << code;
    EXPECT_EQ(missRelayTimeoutS(code, 0, 10, 3), 7) << code;
    EXPECT_EQ(missRelayTimeoutS(code, 0, 0, 0), kReadDeadlineS) << code << ": no request timeout";
    EXPECT_EQ(missRelayTimeoutS(code, 30, RT, -5), 30) << code << ": a clock that went back";
  }
  // Nothing left of the reader's time: a failure is the reader's answer; an answer without its
  // bytes is still sent again (it has no error to give), with the floor of a later attempt: 30 s,
  // or half the reader's own timeout when shorter, at least 1 s.
  for (int code : {kErrConnectionError, kErrSocketError, kErrErrorResponse}) {
    EXPECT_EQ(missRelayTimeoutS(code, 45, RT, 45), -1) << code;
    EXPECT_EQ(missRelayTimeoutS(code, 0, RT, 1800), -1) << code;
    EXPECT_EQ(missRelayTimeoutS(code, 0, RT, 5000), -1) << code;
  }
  EXPECT_EQ(missRelayTimeoutS(0, 45, RT, 45), 22);
  EXPECT_EQ(missRelayTimeoutS(0, 45, RT, 40), 22) << "5 s left, the floor is more";
  EXPECT_EQ(missRelayTimeoutS(0, 0, RT, 5000), 30);
  EXPECT_EQ(missRelayTimeoutS(0, 0, RT, 1790), 30);
  EXPECT_EQ(missRelayTimeoutS(0, 1, RT, 5), 1);
  EXPECT_EQ(missRelayTimeoutS(0, 0, RT, 100), 1700) << "more left than the floor: what is left";
  EXPECT_EQ(missRelayTimeoutS(0, 0, RT, 0), 1800);
  EXPECT_EQ(missRelayTimeoutS(0, 45, RT, 0), 45);
  EXPECT_EQ(missRelayTimeoutS(0, 0, 10, 3), 30) << "7 s left, the floor of 30 s is more";
}

TEST(OpenPolicy, FetchRanOutOfTime) {
  EXPECT_TRUE(fetchRanOutOfTime(kErrOperationExpired));
  EXPECT_TRUE(fetchRanOutOfTime(kErrSocketTimeout));
  for (int code : {0, kErrConnectionError, kErrSocketError, kErrSocketDisconnected,
                   kErrInvalidSession, kErrErrorResponse, kErrDataError, kErrTlsError})
    EXPECT_FALSE(fetchRanOutOfTime(code)) << code;
}

// A read that waited, parked, for another read's fetch of its span makes its own fetch within its
// own time left; with nothing left it fails with an expiry.
TEST(OpenPolicy, ParkedRefetchTimeout) {
  constexpr int64_t S = 1000000; // 1 s in microseconds
  EXPECT_EQ(parkedRefetchTimeoutS(0, RT, 0), 1800);
  EXPECT_EQ(parkedRefetchTimeoutS(0, RT, 10 * S), 1790);
  EXPECT_EQ(parkedRefetchTimeoutS(0, 10, 9 * S), 1);
  EXPECT_EQ(parkedRefetchTimeoutS(0, 10, 9 * S + S / 2), 1) << "half a second left: 1 s";
  EXPECT_EQ(parkedRefetchTimeoutS(0, 10, 10 * S - 1), 1) << "any time left: 1 s, not an expiry";
  EXPECT_EQ(parkedRefetchTimeoutS(0, 6, 5 * S + S / 10), 1) << "the slow server's case: 0.9 s left";
  EXPECT_EQ(parkedRefetchTimeoutS(0, 10, 3 * S + S / 2), 7) << "6.5 s left, rounded up";
  EXPECT_EQ(parkedRefetchTimeoutS(0, 10, 10 * S), -1) << "the client's request timeout spent";
  EXPECT_EQ(parkedRefetchTimeoutS(0, 10, 25 * S), -1);
  EXPECT_EQ(parkedRefetchTimeoutS(45, RT, 20 * S), 25) << "the reader's own timeout, not the client's";
  EXPECT_EQ(parkedRefetchTimeoutS(45, RT, 45 * S), -1);
  EXPECT_EQ(parkedRefetchTimeoutS(30, RT, -3 * S), 30) << "a clock that went back";
  EXPECT_EQ(parkedRefetchTimeoutS(0, 0, 0), kReadDeadlineS) << "no request timeout known";
}

// The insurance timer of a parked read: 10 s, or just past the end of its own time when sooner.
TEST(OpenPolicy, ParkedInsurance) {
  constexpr int64_t S = 1000000;
  EXPECT_EQ(parkedInsuranceMs(0, RT, 0), 10000u) << "the client's 1800 s: the 10 s insurance";
  EXPECT_EQ(parkedInsuranceMs(5, RT, 0), 4020u) << "5 s of its own: 1 s before their end";
  EXPECT_EQ(parkedInsuranceMs(5, RT, 2 * S), 2020u);
  EXPECT_EQ(parkedInsuranceMs(5, RT, 4 * S), 20u) << "in its last second: at once, after 20 ms";
  EXPECT_EQ(parkedInsuranceMs(5, RT, 9 * S), 20u);
  EXPECT_EQ(parkedInsuranceMs(30, RT, 25 * S), 4020u);
  EXPECT_EQ(parkedInsuranceMs(0, 8, 0), 7020u) << "a client's request timeout of 8 s";
  EXPECT_EQ(parkedInsuranceMs(0, 12, 0), 10000u);
}

// A read parked behind a fetch that ran out of time at the server its handle reads, on a handle
// whose reads do not move, takes that error when its own time had run out by the moment the
// failure was seen (the client's own rule: a request's expiry is time(0) plus its timeout, in
// whole seconds, and the client ends it at the first check at or past it); anything else fetches
// for itself.
TEST(OpenPolicy, ParkedGetsOwnersError) {
  const std::string A = "a.example.org:1094", B = "b.example.org:1094";
  const auto gets = [&](int code, bool moves, const std::string& fs, const std::string& own,
                        int64_t failedAtS, int64_t ownExpiryS) {
    return parkedGetsOwnersError(code, moves, fs, own, failedAtS, ownExpiryS);
  };
  constexpr int64_t F = 1000000; // the fetch's own expiry (wall seconds); the client's timer checks
  constexpr int64_t R = 15;      // every R s (its timeout resolution, 15 by default)
  for (int code : {kErrOperationExpired, kErrSocketTimeout}) {
    // Seen at the fetch's own expiry: a read with the same expiry, or an earlier one, takes it.
    EXPECT_TRUE(gets(code, false, A, A, F, F));
    EXPECT_TRUE(gets(code, false, A, A, F, F - 5)) << "its own time ran out first";
    EXPECT_TRUE(gets(code, false, A, A, F + 3, F + 3)) << "seen 3 s late, its expiry then";
    EXPECT_TRUE(gets(code, false, "A.Example.org:1094", A, F, F)) << "in any case";
    EXPECT_TRUE(gets(code, false, "", A, F, F)) << "the fetch's server not known: the same";
    // Its own time had not run out when the failure was seen: it fetches for itself, whatever the
    // fetch's own expiry and the timer's period.
    EXPECT_FALSE(gets(code, false, A, A, F, F + 1));
    EXPECT_FALSE(gets(code, false, A, A, F, F + R - 1))
        << "its expiry the fetch's plus the resolution less 1 s, the failure seen at the fetch's";
    EXPECT_FALSE(gets(code, false, A, A, F + 3, F + 4))
        << "the fetch had 2 s, seen at 5 s; the read had 6 s (the slow server's case)";
    EXPECT_FALSE(gets(code, false, A, A, F, F + 7)) << "the fetch had 3 s, the read 10";
    // Its own server not known (a handle that has not opened its origin): it fetches for itself.
    EXPECT_FALSE(gets(code, false, A, "", F, F));
    EXPECT_FALSE(gets(code, false, "", "", F, F));
    // When the failure was seen not known: never handed over.
    EXPECT_FALSE(gets(code, false, A, A, 0, F));
    EXPECT_FALSE(gets(code, true, A, A, F, F)) << "its reads move";
    EXPECT_FALSE(gets(code, false, A, B, F, F)) << "its handle reads another server";
  }
  for (int code : {0, kErrConnectionError, kErrSocketError, kErrErrorResponse, kErrDataError})
    EXPECT_FALSE(gets(code, false, A, A, F, F)) << code;
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

// A server listed for an earlier failure of the open whose copy is then refused for what it is:
// its entry says so (bad, the refusal's text and the copy's size), so that it is not taken again
// and the open's error names the refusal; a refusal is never undone by a later plain failure.
TEST(OpenPolicy, ListOpenFailureRefusedLater) {
  std::vector<SiteFailure> failed;
  SiteFailure first{"a.one.org:1094", "one.org", "a.one.org", "[ERROR] Operation expired"};
  EXPECT_FALSE(listOpenFailure(failed, first));
  EXPECT_FALSE(refusedBefore(failed, "a.one.org:1094"));
  SiteFailure cut{"a.one.org:1094", "one.org", "a.one.org",
                  "its header says the file is 2000 bytes, and the copy has 1000"};
  cut.bad = true;
  cut.size = 1000;
  EXPECT_TRUE(listOpenFailure(failed, cut)) << "listed already";
  ASSERT_EQ(failed.size(), 1u) << "a server is listed once";
  EXPECT_TRUE(failed[0].bad);
  EXPECT_EQ(failed[0].error, cut.error);
  EXPECT_EQ(failed[0].size, 1000);
  EXPECT_TRUE(refusedBefore(failed, "a.one.org:1094"));
  EXPECT_EQ(afterFailure(failed, first, 4, false), AfterFailure::kRepeat);
  EXPECT_TRUE(failed[0].bad) << "a later plain failure does not undo the refusal";
  EXPECT_EQ(failed[0].error, cut.error);
  // afterFailure lists a refusal the same way.
  std::vector<SiteFailure> f2{first};
  EXPECT_EQ(afterFailure(f2, cut, 4, false), AfterFailure::kRepeat);
  EXPECT_TRUE(f2[0].bad);
  EXPECT_EQ(f2[0].error, cut.error);
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

// The error of a read that no copy of the size its reader was shown could serve: every copy
// tried, with its size where it is known.
TEST(OpenPolicy, NoCopyServedText) {
  SiteFailure a{"127.0.0.1:14121", "127.0.0.1", "",
                "answered 0 of the 65536 bytes asked at 10161194, inside the file's size of "
                "16935324 bytes"};
  a.size = 16935324;
  a.bad = true;
  SiteFailure b{"b.two.org:1094", "two.org", "b.two.org",
                "it serves 10161194 bytes, the cached file has 16935324"};
  b.size = 10161194;
  SiteFailure c{"c.three.org:1094", "three.org", "c.three.org", "size 0"};
  c.size = 0;
  const SiteFailure d{"d.four.org:1094", "four.org", "d.four.org",
                      "[ERROR] at root://d.four.org:1094//f?authz=SECRET: not found"};
  const std::string t = noCopyServedText("/store/mc/f.root", {a, b, c, d});
  EXPECT_EQ(t, "uCache: /store/mc/f.root: no copy could serve the read: 127.0.0.1 "
               "(127.0.0.1:14121, 16935324 bytes): answered 0 of the 65536 bytes asked at "
               "10161194, inside the file's size of 16935324 bytes; two.org (b.two.org:1094, "
               "10161194 bytes): it serves 10161194 bytes, the cached file has 16935324; three.org "
               "(c.three.org:1094, 0 bytes): size 0; four.org (d.four.org:1094): [ERROR] at "
               "root://d.four.org:1094//f not found");
  EXPECT_EQ(t.find("SECRET"), std::string::npos);
  EXPECT_EQ(t.find("no site could open it"), std::string::npos) << "a read, not an open";
  EXPECT_EQ(noCopyServedText("/f", {}), "uCache: /f: no copy could serve the read: ");
  // Each error at most 160 bytes, the whole at most 1024.
  std::vector<SiteFailure> many(20, {"s.x.org:1094", "x.org", "s.x.org", std::string(500, 'e')});
  many[0].size = 7;
  const std::string m = noCopyServedText("/f", many);
  EXPECT_LE(m.size(), 1024u);
  EXPECT_NE(m.find("(s.x.org:1094, 7 bytes): " + std::string(160, 'e') + ";"), std::string::npos);
  EXPECT_EQ(m.find(std::string(161, 'e')), std::string::npos);
}

// The copies a handle's reads failed at: one entry per server, the newest last, at most
// `keep` of them; one that answered with bad bytes stays bad, and is never dropped.
TEST(OpenPolicy, NoteReadFailure) {
  const auto fail = [](const char* server, const char* error, bool bad) {
    SiteFailure f{server, server, "", error};
    f.bad = bad;
    f.size = 100;
    return f;
  };
  const auto servers = [](const std::vector<SiteFailure>& v) {
    std::string s;
    for (const SiteFailure& x : v)
      s += x.server + (x.bad ? "!" : "") + " ";
    return s;
  };
  std::vector<SiteFailure> l;
  noteReadFailure(l, fail("a:1", "stall", false), 3);
  noteReadFailure(l, fail("b:1", "short", true), 3);
  EXPECT_EQ(servers(l), "a:1 b:1! ");
  // The same server again: one entry, the newest, with the newest error.
  noteReadFailure(l, fail("a:1", "socket", false), 3);
  EXPECT_EQ(servers(l), "b:1! a:1 ");
  EXPECT_EQ(l.back().error, "socket");
  // Found bad once, bad for good, though its later failure is of another kind.
  noteReadFailure(l, fail("b:1", "stall", false), 3);
  EXPECT_EQ(servers(l), "a:1 b:1! ");
  EXPECT_EQ(l.back().error, "stall");
  // At most three: the oldest that is not bad goes first.
  noteReadFailure(l, fail("c:1", "stall", false), 3);
  noteReadFailure(l, fail("d:1", "stall", false), 3);
  EXPECT_EQ(servers(l), "b:1! c:1 d:1 ") << "a went, the bad b stays though it is older";
  noteReadFailure(l, fail("e:1", "zeros", true), 3);
  EXPECT_EQ(servers(l), "b:1! d:1 e:1! ");
  // Every one bad: none is dropped, so none is ever opened again.
  noteReadFailure(l, fail("f:1", "zeros", true), 3);
  noteReadFailure(l, fail("g:1", "zeros", true), 3);
  EXPECT_EQ(servers(l), "b:1! e:1! f:1! g:1! ");
  // A failure that is not bad, with every older one bad: it is kept, the newest, though the list
  // then holds one more than three, so that a move never goes straight back to the copy it leaves.
  noteReadFailure(l, fail("h:1", "stall", false), 3);
  EXPECT_EQ(servers(l), "b:1! e:1! f:1! g:1! h:1 ");
  EXPECT_EQ(l.back().error, "stall");
  // The next one replaces it: the oldest that is not bad goes, never the newest.
  noteReadFailure(l, fail("i:1", "socket", false), 3);
  EXPECT_EQ(servers(l), "b:1! e:1! f:1! g:1! i:1 ");
  noteReadFailure(l, fail("i:1", "stall", false), 3);
  EXPECT_EQ(servers(l), "b:1! e:1! f:1! g:1! i:1 ") << "the same server: one entry";
  // A list of three bad ones and one more that is not: four, the newest kept.
  std::vector<SiteFailure> three;
  for (const char* s : {"p:1", "q:1", "r:1"})
    noteReadFailure(three, fail(s, "zeros", true), 3);
  noteReadFailure(three, fail("s:1", "stall", false), 3);
  EXPECT_EQ(servers(three), "p:1! q:1! r:1! s:1 ");
  Location ls;
  ls.host = "s";
  ls.port = 1;
  Location lt = ls;
  lt.host = "t";
  EXPECT_EQ(orderLocations({ls, lt}, {}, three).front().host, "t")
      << "the copy just left is taken last";
  // What the list says of a server, in any case of its name.
  EXPECT_TRUE(refusedBefore(l, "B:1"));
  EXPECT_TRUE(refusedBefore(l, "g:1"));
  EXPECT_FALSE(refusedBefore(l, "d:1")) << "dropped";
  EXPECT_FALSE(refusedBefore({fail("a:1", "stall", false)}, "a:1")) << "failed, not bad";
  EXPECT_FALSE(refusedBefore({}, "a:1"));
  // A copy a read found bad is not given again by the list of copies, even as the last resort;
  // one whose read merely failed still is.
  Location la;
  la.host = "a.one.org";
  la.port = 1094;
  std::vector<SiteFailure> reads;
  noteReadFailure(reads, fail("a.one.org:1094", "zeros", true), 3);
  EXPECT_TRUE(orderLocations({la}, {}, reads).empty());
  std::vector<SiteFailure> stalls;
  noteReadFailure(stalls, fail("a.one.org:1094", "stall", false), 3);
  EXPECT_EQ(orderLocations({la}, {}, stalls).size(), 1u);
}

// The error a read is given in the second after a move made for an earlier read found no copy.
TEST(OpenPolicy, EarlierMoveText) {
  SiteFailure a{"127.0.0.1:14121", "127.0.0.1", "",
                "answered 0 of the 65536 bytes asked at 10161194, inside the file's size of "
                "16935324 bytes"};
  a.size = 16935324;
  const std::string move = noCopyServedText("/store/mc/f.root", {a});
  EXPECT_EQ(earlierMoveText(move, 250),
            "uCache: /store/mc/f.root: no copy could serve the read: a move made 250 ms ago for an "
            "earlier read of the handle found none, and none is made again within a second; the "
            "copies it tried, as they answered that read: 127.0.0.1 (127.0.0.1:14121, 16935324 "
            "bytes): answered 0 of the 65536 bytes asked at 10161194, inside the file's size of "
            "16935324 bytes");
  EXPECT_NE(earlierMoveText(move, 999).find("999 ms ago"), std::string::npos);
  EXPECT_NE(earlierMoveText(move, 1000).find("1 second ago"), std::string::npos);
  EXPECT_NE(earlierMoveText(move, 0).find("0 ms ago"), std::string::npos);
  // Not a move's text (an open's, or none): as it is.
  EXPECT_EQ(earlierMoveText("uCache: /f: no site could open it: x", 5),
            "uCache: /f: no site could open it: x");
  EXPECT_EQ(earlierMoveText("", 5), "");
  // At most 1024 bytes.
  std::vector<SiteFailure> many(20, {"s.x.org:1094", "x.org", "s.x.org", std::string(150, 'e')});
  const std::string m = earlierMoveText(noCopyServedText("/f", many), 10);
  EXPECT_EQ(m.size(), 1024u);
  EXPECT_EQ(m.find("uCache: /f: no copy could serve the read: a move made 10 ms ago"), 0u);
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
  // A copy refused for what it is (`bad`) is final: when nothing else is left it is not given
  // again, though a server that merely failed is. Its siblings are not bad because it is.
  SiteFailure refused{"a.two.org:1094", "two.org", "a.two.org", "size 0"};
  refused.bad = true;
  const std::vector<Location> pair = {loc("a.two.org", 1094), loc("b.two.org", 1094)};
  EXPECT_EQ(names(orderLocations(pair, {}, {refused})), "b.two.org:1094 ")
      << "the sibling of a refused copy is a failed server as before, not a refused one";
  EXPECT_EQ(names(orderLocations({loc("a.two.org", 1094)}, {}, {refused})), "")
      << "the only copy was refused: nothing is left to open";
  const SiteFailure failedOnly{"a.two.org:1094", "two.org", "a.two.org", "refused"};
  EXPECT_EQ(names(orderLocations({loc("a.two.org", 1094)}, {}, {failedOnly})), "a.two.org:1094 ")
      << "a server that merely failed is still the last resort";
  // IPv6 servers: the client names a failed one in brackets, the locate's answer gives the bare
  // address. A server that failed is left to the end, the oldest failure first, and one refused for
  // what it is is never given again.
  SiteFailure v6old{"[::1]:21121", "[::1]", "", "stall"}, v6new{"[::1]:21123", "[::1]", "", "stall"};
  SiteFailure v6bad{"[2001:DB8::7]:1094", "[2001:db8::7]", "", "zeros"};
  v6bad.bad = true;
  const std::vector<Location> v6 = {loc("::1", 21123), loc("::1", 21121), loc("2001:db8::7", 1094),
                                    loc("::1", 21122)};
  EXPECT_EQ(names(orderLocations(v6, {}, {v6old, v6new, v6bad})), "::1:21122 ")
      << "the one IPv6 server that did not fail";
  EXPECT_EQ(names(orderLocations({loc("::1", 21123), loc("::1", 21121), loc("2001:db8::7", 1094)}, {},
                                 {v6old, v6new, v6bad})),
            "::1:21121 ::1:21123 ")
      << "only failed ones left: the oldest failure first; the refused one never";
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
      {"[::1]:1093", "[::1]", "", "e"}, // an IPv6 server as the client names it
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
    SiteFailure f; // as siteFailure makes it from a hop: an IPv6 host in brackets
    const std::string h = std::strchr(r.host, ':') ? "[" + std::string(r.host) + "]" : r.host;
    f.server = h + ":" + std::to_string(r.port);
    f.site = siteOf(h);
    EXPECT_EQ(failedBefore(failed, f), r.before) << r.host << ":" << r.port << " " << r.why;
    EXPECT_EQ(excludedByList(r.host, r.port), r.list) << r.host << ":" << r.port << " " << r.why;
  }
}

// A copy with no bytes of a file named *.root is a bad copy; a size of zero is an
// ordinary size for any other name. The name is the path without its query, in any case.
TEST(OpenPolicy, EmptyRootCopy) {
  struct Row {
    const char* path;
    long long size;
    bool bad;
    const char* why;
  };
  const Row rows[] = {
      {"/store/mc/a/b.root", 0, true, "a path"},
      {"root://h:1094//store/a.root", 0, true, "a URL"},
      {"root://h:1094//store/a.root?tried=x.org&triedrc=ioerr", 0, true, "the query is not part of the name"},
      {"a.root", 0, true, ""},
      {"/store/a.root.root", 0, true, ""},
      {"/store/a.ROOT", 0, true, "upper case"},
      {"/store/a.Root", 0, true, "mixed case"},
      {"root://h:1094//store/A.RoOt?x=y.dat", 0, true, "mixed case with a query"},
      // Not empty, or not known: no claim.
      {"/store/a.root", 1, false, ""},
      {"/store/a.root", 4247184, false, ""},
      {"/store/a.root", -1, false, "a size that could not be read"},
      {"/store/a.ROOT", 1, false, "upper case, not empty"},
      // Not the name of a ROOT file: an empty file is an ordinary file.
      {"/store/a.dat", 0, false, ""},
      {"/store/a.root.gz", 0, false, ""},
      {"/store/a.ROOT.gz", 0, false, ""},
      {"/store/a.root/", 0, false, "a trailing slash"},
      {"/store/aroot", 0, false, "no dot"},
      {"/store/aROOT", 0, false, "no dot, upper case"},
      {"/store/a.dat?name=b.root", 0, false, "only the query ends in .root"},
      {"/store/a.dat?name=b.ROOT", 0, false, "only the query ends in .ROOT"},
      {"/store/a.r00t", 0, false, "zeros for the o's"},
      {"", 0, false, ""},
      {"root", 0, false, ""},
      {"ROOT", 0, false, ""},
      {".roo", 0, false, ""},
      {"?a.root", 0, false, "nothing before the query"},
  };
  for (const Row& r : rows)
    EXPECT_EQ(emptyRootCopy(r.path, r.size), r.bad) << r.path << " " << r.size << " " << r.why;
}

TEST(OpenPolicy, RootNamed) {
  struct Row {
    const char* path;
    bool root;
  };
  const Row rows[] = {
      {"/store/mc/a/b.root", true},
      {"root://h:1094//store/a.root?tried=x.org", true},
      {".root", true},
      {".ROOT", true},
      {"/store/a.ROOT", true},
      {"/store/a.Root", true},
      {"/store/a.rOOT?x=1", true},
      {"root://h:1094//store/a.ROOT?tried=x.org&triedrc=ioerr", true},
      {"/store/a.root.gz", false},
      {"/store/a.ROOT.GZ", false},
      {"/store/a.dat?name=b.root", false},
      {"/store/a.dat?name=b.ROOT", false},
      {"/store/aroot", false},
      {"/store/a_root", false},
      {"/store/a.roo", false},
      {"root", false},
      {"", false},
      {"?.root", false},
  };
  for (const Row& r : rows)
    EXPECT_EQ(rootNamed(r.path), r.root) << r.path;
}

namespace {
// The first 300 bytes of a ROOT file whose header states `end`: a completed header (the first
// record at 100, a name record of 58 bytes, no class descriptions or free list placed), and the
// top directory's record at 158, which places a list of top-level objects of 30 bytes at 218. The
// rest is filler. (root_check_test.cc builds whole files; these rows are about the first read.)
std::vector<unsigned char> header(const char* magic, uint32_t version, uint64_t end, bool wide,
                                  size_t n = kRootHeaderBytes) {
  std::vector<unsigned char> b(magic, magic + 4);
  const auto put = [&b](uint64_t v, int k) {
    for (int i = k - 1; i >= 0; --i)
      b.push_back(static_cast<unsigned char>(v >> (8 * i)));
  };
  put(version, 4);
  put(100, 4); // where the first record begins
  put(end, wide ? 8 : 4);
  put(0, wide ? 8 : 4); // no list of free space
  put(0, 4);
  put(1, 4);
  put(58, 4); // the top directory's name record
  put(wide ? 8 : 4, 1);
  put(505, 4);
  put(0, wide ? 8 : 4); // no class descriptions
  put(0, 4);
  while (b.size() < 158)
    b.push_back(0x5a);
  put(wide ? 1005 : 5, 2); // the top directory's record
  put(0x12345678, 4);
  put(0x12345678, 4);
  put(30, 4); // the list's length
  put(58, 4);
  put(100, wide ? 8 : 4);
  put(0, wide ? 8 : 4);
  put(218, wide ? 8 : 4); // the list's position
  while (b.size() < kRootHeaderBytes)
    b.push_back(0x5a);
  b.resize(n);
  return b;
}
} // namespace

// What a copy's first bytes say about where the file ends: the end, or none.
// A warm entry made before the checks existed: the header it holds against the size it holds.
TEST(OpenPolicy, StoredEntryCut) {
  auto b = header("root", 62002, 1000000, false);
  EXPECT_TRUE(storedEntryCut(b.data(), b.size(), 600000)) << "the end past the stored size: cut";
  EXPECT_TRUE(storedEntryCut(b.data(), b.size(), 999999));
  EXPECT_FALSE(storedEntryCut(b.data(), b.size(), 1000000)) << "the end at the stored size";
  EXPECT_FALSE(storedEntryCut(b.data(), b.size(), 1004096))
      << "the end before the stored size (bytes past it, a file updated in place): no claim";
  EXPECT_TRUE(storedEntryCut(b.data(), 16, 600000)) << "a 32-bit end is whole at 16 bytes";
  EXPECT_FALSE(storedEntryCut(b.data(), 15, 600000)) << "the end's field not held: no claim";
  b = header("root", 1062002, 5000000000ull, true);
  EXPECT_TRUE(storedEntryCut(b.data(), b.size(), 3000000000ull)) << "a 64-bit end";
  EXPECT_FALSE(storedEntryCut(b.data(), b.size(), 5000000000ull));
  EXPECT_FALSE(storedEntryCut(b.data(), 19, 3000000000ull)) << "a 64-bit end's field cut";
  b = header("ROOT", 62002, 1000000, false);
  EXPECT_FALSE(storedEntryCut(b.data(), b.size(), 600000)) << "not a ROOT file's header: no claim";
  b = header("root", 1062002, ~0ull, true);
  EXPECT_TRUE(storedEntryCut(b.data(), b.size(), 1000000)) << "an end no file has (LLONG_MAX)";
  EXPECT_FALSE(storedEntryCut(nullptr, 0, 0));
}

TEST(OpenPolicy, RootFileEnd) {
  // The first bytes of a real file (6559 bytes): the end it states is its size.
  const unsigned char real[] = {0x72, 0x6f, 0x6f, 0x74, 0x00, 0x00, 0xf8, 0x72, 0x00, 0x00,
                                0x00, 0x64, 0x00, 0x00, 0x19, 0x9f, 0x00, 0x00, 0x19, 0x64};
  EXPECT_EQ(rootFileEnd(real, sizeof real), 6559);
  EXPECT_EQ(rootFileEnd(real, 16), 6559) << "a 32-bit end is whole at 16 bytes";
  EXPECT_EQ(rootFileEnd(real, 15), -1) << "the end's field is cut";
  EXPECT_EQ(rootFileEnd(real, 8), -1);
  EXPECT_EQ(rootFileEnd(real, 7), -1) << "the version is cut";
  EXPECT_EQ(rootFileEnd(real, 0), -1);
  // A 32-bit end, a 64-bit one for a version of a million or more, the end one past 2 GiB.
  auto b = header("root", 62002, 123456789, false);
  EXPECT_EQ(rootFileEnd(b.data(), b.size()), 123456789);
  b = header("root", 1062002, 5000000000ull, true);
  EXPECT_EQ(rootFileEnd(b.data(), b.size()), 5000000000ll);
  EXPECT_EQ(rootFileEnd(b.data(), 20), 5000000000ll) << "a 64-bit end is whole at 20 bytes";
  EXPECT_EQ(rootFileEnd(b.data(), 19), -1) << "a 64-bit end's field is cut";
  EXPECT_EQ(rootFileEnd(b.data(), 16), -1) << "16 bytes do not hold a 64-bit end";
  b = header("root", 1000000, 7, true);
  EXPECT_EQ(rootFileEnd(b.data(), b.size()), 7) << "version 1000000 is the first with a 64-bit end";
  b = header("root", 999999, 7, false);
  EXPECT_EQ(rootFileEnd(b.data(), b.size()), 7);
  b = header("root", 62002, 0x80000000u, false);
  EXPECT_EQ(rootFileEnd(b.data(), b.size()), 2147483648ll) << "never negative";
  b = header("root", 1062002, ~0ull, true);
  EXPECT_EQ(rootFileEnd(b.data(), b.size()), LLONG_MAX) << "an end no file has: it equals no size";
  // The version is a signed integer, as ROOT reads it: one whose top bit is set is negative,
  // below a million, and marks a 32-bit end (ROOT opens such a file as a small one).
  b = header("root", 0x80000000u | 62002, 123456789, false);
  EXPECT_EQ(rootFileEnd(b.data(), b.size()), 123456789) << "the top bit of a small file's version";
  EXPECT_EQ(rootFileEnd(b.data(), 16), 123456789) << "its 32-bit end is whole at 16 bytes";
  b = header("root", 0x80000000u, 4242, false);
  EXPECT_EQ(rootFileEnd(b.data(), b.size()), 4242) << "the top bit alone";
  b = header("root", 0xFFFFFFFFu, 4242, false);
  EXPECT_EQ(rootFileEnd(b.data(), b.size()), 4242) << "a version of -1";
  // A 64-bit header with the top bit set in its version is read with 32-bit fields, as ROOT reads
  // it: the end is the top half of the 64-bit end (and ROOT refuses the file, its first record
  // lying past that end).
  b = header("root", 0x80000000u | 1062002, 5000000000ull, true);
  EXPECT_EQ(rootFileEnd(b.data(), b.size()), 1) << "the top 32 bits of 5000000000";
  // Not a ROOT file's first bytes, whatever follows the first four.
  for (const char* magic : {"Root", "ROOT", "toor", "roo\0", "\0\0\0\0"}) {
    b = header(magic, 62002, 4242, false);
    EXPECT_EQ(rootFileEnd(b.data(), b.size()), -1) << magic;
  }
  const std::vector<unsigned char> zeros(kRootHeaderBytes, 0);
  EXPECT_EQ(rootFileEnd(zeros.data(), zeros.size()), -1) << "a block of zeros";
}

// The bytes the header check reads: ROOT's 300, or the whole copy when it is smaller.
TEST(OpenPolicy, RootHeaderBytes) {
  EXPECT_EQ(kRootHeaderBytes, 300u) << "what ROOT reads at open";
  const std::pair<long long, size_t> rows[] = {{-1, 0},   {0, 0},     {1, 1},     {19, 19},
                                               {20, 20},  {299, 299}, {300, 300}, {301, 300},
                                               {6559, 300}, {LLONG_MAX, 300}};
  for (const auto& r : rows)
    EXPECT_EQ(rootHeaderBytes(r.first), r.second) << r.first;
}

// What the header check says of a copy, given what the read of its first bytes answered.
TEST(OpenPolicy, RootHeaderVerdict) {
  using K = RootHeaderVerdict::Kind;
  const auto verdict = [](const std::vector<unsigned char>& b, long long size) {
    return rootHeaderVerdict(b.data(), b.size(), size);
  };
  const auto first = [](const std::vector<unsigned char>& b, long long size) {
    return std::vector<unsigned char>(b.begin(), b.begin() + static_cast<long>(rootHeaderBytes(size)));
  };
  // The end against the size, for a 32-bit and a 64-bit header, the end both below and above 2^32.
  struct EndRow {
    uint32_t version;
    bool wide;
    long long end, size;
    K kind;
    const char* why;
  };
  const EndRow ends[] = {
      {62002, false, 6559, 6559, K::kConfirmed, "a whole copy"},
      {62002, false, 6559, 6558, K::kBad, "one byte short of its header's end"},
      {62002, false, 6559, 3000, K::kBad, "cut short"},
      {62002, false, 6559, 6560, K::kNoClaim, "one byte past it"},
      {62002, false, 6559, 10561, K::kNoClaim, "bytes appended, or a file updated in place"},
      {1062002, true, 5000000000ll, 5000000000ll, K::kConfirmed, "a 64-bit end"},
      {1062002, true, 5000000000ll, 4999999999ll, K::kBad, "a 64-bit end, cut"},
      {1062002, true, 5000000000ll, 5000004096ll, K::kNoClaim, "a 64-bit end, longer"},
      {1062002, true, 6559, 6559, K::kConfirmed, "a small file with a 64-bit header"},
      {1062002, true, 6559, 400, K::kBad, "a small file with a 64-bit header, cut"},
      {62002, false, 300, 300, K::kConfirmed, "a copy of exactly 300 bytes"},
      {62002, false, 300, 299, K::kBad, "299 bytes of a file of 300"},
      {62002, false, 250, 250, K::kConfirmed, "a copy shorter than 300 bytes, whole"},
      {62002, false, 0, 6559, K::kBad, "an end of 0, before the first record: ROOT refuses it"},
      {0x80000000u | 62002, false, 6559, 6559, K::kConfirmed,
       "the top bit of the version set: a small file to ROOT, which opens it"},
      {0x80000000u | 62002, false, 6559, 10561, K::kNoClaim, "the same, longer"},
      {0x80000000u | 62002, false, 6559, 3000, K::kBad, "the same, cut"},
  };
  for (const EndRow& r : ends) {
    const auto b = first(header("root", r.version, static_cast<uint64_t>(r.end), r.wide), r.size);
    const RootHeaderVerdict v = verdict(b, r.size);
    EXPECT_EQ(v.kind, r.kind) << r.why;
    EXPECT_EQ(v.end, r.end) << r.why;
    EXPECT_EQ(v.kind == K::kBad, !v.why.empty()) << r.why;
    // A kind that stands asks for the list of top-level objects next (the verdict is not final).
    if (v.kind != K::kBad) {
      EXPECT_EQ(v.next, RootHeaderVerdict::kKeysList) << r.why;
      EXPECT_EQ(v.nextAt, 218) << r.why;
      EXPECT_EQ(v.nextLen, 30u) << r.why;
    } else {
      EXPECT_EQ(v.next, RootHeaderVerdict::kNone) << r.why;
    }
  }
  // The cut copy's text names both sizes, as the records and the error say it.
  EXPECT_EQ(verdict(header("root", 62002, 16935324, false), 10161194).why,
            "size 10161194: its header says the file is 16935324 bytes");
  // Not beginning with "root": a bad copy, whatever its size; zeros at the right size included.
  for (const char* magic : {"Root", "ROOT", "toor", "roo\0", "\0\0\0\0", "NOTR"}) {
    const RootHeaderVerdict v = verdict(header(magic, 62002, 6559, false), 6559);
    EXPECT_EQ(v.kind, K::kBad) << magic;
    EXPECT_EQ(v.why, "size 6559: it does not begin with \"root\", as every ROOT file does") << magic;
  }
  EXPECT_EQ(verdict(std::vector<unsigned char>(300, 0), 16935324).kind, K::kBad) << "all zeros";
  // Every size from 1 to 300 bytes of a cut copy of a 32-bit and of a 64-bit file: too short to
  // hold the header up to the end's field (16 bytes, or 20), else cut short of the end (6559).
  for (bool wide : {false, true}) {
    const auto full = header("root", wide ? 1062002 : 62002, 6559, wide);
    const long long field = wide ? 20 : 16;
    for (long long size = 1; size <= 300; ++size) {
      const RootHeaderVerdict v = verdict(first(full, size), size);
      EXPECT_EQ(v.kind, K::kBad) << size << " wide " << wide;
      if (size < field)
        EXPECT_EQ(v.why, "size " + std::to_string(size) + ": too short to hold a ROOT file's header")
            << size;
      else
        EXPECT_EQ(v.end, 6559) << size;
    }
  }
  // The first bytes of a copy that does not begin as a ROOT file does, at any size below 4.
  EXPECT_EQ(verdict({'x'}, 1).why, "size 1: it does not begin with \"root\", as every ROOT file does");
  EXPECT_EQ(verdict({'r', 'o'}, 2).why, "size 2: too short to hold a ROOT file's header");
  // A read answered short inside the copy's size: the copy ends before its size, refused.
  const auto whole = header("root", 62002, 6559, false);
  for (size_t n : {size_t(0), size_t(1), size_t(20), size_t(64), size_t(299)}) {
    const std::vector<unsigned char> part(whole.begin(), whole.begin() + static_cast<long>(n));
    const RootHeaderVerdict v = verdict(part, 6559);
    EXPECT_EQ(v.kind, K::kBad) << n;
    EXPECT_EQ(v.why, "size 6559: a read of its first 300 bytes answered " + std::to_string(n)) << n;
  }
  const std::vector<unsigned char> part(whole.begin(), whole.begin() + 99);
  EXPECT_EQ(verdict(part, 100).why, "size 100: a read of its first 100 bytes answered 99");
  EXPECT_EQ(verdict(std::vector<unsigned char>(whole.begin(), whole.begin() + 100), 100).kind, K::kBad)
      << "a whole copy of 100 bytes whose header says 6559";
}

// The header read's limit: what is left of the attempt's own open limit, at least the floor.
TEST(OpenPolicy, HeaderReadTimeout) {
  EXPECT_EQ(headerReadTimeoutS(60, 0), 60) << "an open that answered at once";
  EXPECT_EQ(headerReadTimeoutS(60, 1), 60) << "rounded up";
  EXPECT_EQ(headerReadTimeoutS(60, 20000), 40);
  EXPECT_EQ(headerReadTimeoutS(60, 20001), 40) << "a part of a second left counts as a second";
  EXPECT_EQ(headerReadTimeoutS(60, 54000), 6);
  EXPECT_EQ(headerReadTimeoutS(60, 55000), kHeaderReadFloorS);
  EXPECT_EQ(headerReadTimeoutS(60, 59999), kHeaderReadFloorS) << "never the open's limit afresh";
  EXPECT_EQ(headerReadTimeoutS(60, 600000), kHeaderReadFloorS) << "past the limit: the floor";
  EXPECT_EQ(headerReadTimeoutS(30, 1000), 29) << "an attempt given less than the deadline";
  EXPECT_EQ(headerReadTimeoutS(3, 0), kHeaderReadFloorS) << "a test's short limit";
  EXPECT_EQ(headerReadTimeoutS(60, -5), 60) << "a clock that went back counts as no time";
  EXPECT_EQ(headerReadTimeoutS(0, 1000), kOpenDeadlineS - 1) << "no limit given: the deadline's";
  for (int64_t ms = 0; ms <= 120000; ms += 7) {
    const int t = headerReadTimeoutS(60, ms);
    ASSERT_GE(t, kHeaderReadFloorS) << ms;
    ASSERT_LE(t, 60) << ms;
  }
}

// A header read that failed is a failure of the copy: another copy when the open went through a
// redirector, whatever the code; a direct URL has none.
TEST(OpenPolicy, StepAfterHeaderRead) {
  const ChainView v = regionalThen(kServer);
  for (int code : {kErrSocketError, kErrSocketDisconnected, kErrStreamDisconnect, kErrTlsError,
                   kErrInvalidSession, kErrOperationInterrupted, kErrInternal, kErrDataError,
                   kErrRedirect, kErrConnectionError, kErrSocketTimeout, 101})
    EXPECT_EQ(stepAfterHeaderRead(v, code, 0, false, false), OpenStep::kFailOver) << code;
  for (int errNo : {kXrIOError, kXrServerError, 3013, kXrNotAuthorized})
    EXPECT_EQ(stepAfterHeaderRead(v, kErrErrorResponse, errNo, false, false), OpenStep::kFailOver)
        << errNo;
  // How another copy is found is the open's rule: a stall, or a connection that timed out, was
  // lost or could not be made, asks the regional for its list; any other failure of the read goes
  // to another site through the redirector.
  EXPECT_EQ(stepAfterHeaderRead(v, kErrOperationExpired, 0, false, true), OpenStep::kLocate);
  for (int code : {kErrSocketTimeout, kErrConnectionError, kErrSocketError, kErrSocketDisconnected,
                   kErrStreamDisconnect, kErrInvalidSession})
    EXPECT_EQ(stepAfterHeaderRead(v, code, 0, false, true), OpenStep::kLocate) << code;
  for (int code : {kErrTlsError, kErrOperationInterrupted, kErrInternal, kErrDataError, kErrRedirect,
                   101})
    EXPECT_EQ(stepAfterHeaderRead(v, code, 0, false, true), OpenStep::kFailOver) << code;
  EXPECT_EQ(stepAfterHeaderRead(v, kErrOperationExpired, 0, false, false), OpenStep::kFailOver);
  EXPECT_EQ(stepAfterHeaderRead(v, kErrErrorResponse, kXrNotFound, false, false), OpenStep::kLocate);
  // No other copy: a direct URL, or a chain with no data server after the redirector.
  for (int code : {kErrSocketError, kErrRedirect, kErrOperationExpired, kErrConnectionError})
    EXPECT_EQ(stepAfterHeaderRead(kDirect, code, 0, false, true), OpenStep::kFinal) << code;
  EXPECT_EQ(stepAfterHeaderRead(kRedirectorOnly, kErrSocketError, 0, false, false), OpenStep::kFinal);
}

// The failures the client would have recovered by opening the file again (its read recovery and
// its following of redirects), which the header read turns off.
TEST(OpenPolicy, ClientRecoversRead) {
  for (int code : {kErrSocketError, kErrSocketTimeout, kErrInvalidSession, kErrTlsError,
                   kErrOperationInterrupted, kErrInternal, kErrRedirect})
    EXPECT_TRUE(clientRecoversRead(code)) << code;
  for (int code : {0, kErrConnectionError, kErrOperationExpired, kErrStreamDisconnect,
                   kErrSocketDisconnected, kErrDataError, kErrErrorResponse, kErrNotFound,
                   kErrRedirectLimit, kErrAuthFailed})
    EXPECT_FALSE(clientRecoversRead(code)) << code;
}

// Where an open would end with a failed read of its checks, the open is made once more at the same
// source when the client would have recovered that read: once (the first attempt there), and only
// for a failed read of the checks.
TEST(OpenPolicy, OpenOnceMoreAfterCheckRead) {
  for (int code : {kErrSocketError, kErrSocketTimeout, kErrInvalidSession, kErrTlsError,
                   kErrOperationInterrupted, kErrInternal, kErrRedirect}) {
    EXPECT_TRUE(openOnceMoreAfterCheckRead(true, 1, code)) << code;
    EXPECT_FALSE(openOnceMoreAfterCheckRead(true, 2, code)) << code << ": once at a source";
    EXPECT_FALSE(openOnceMoreAfterCheckRead(true, 3, code)) << code;
    EXPECT_FALSE(openOnceMoreAfterCheckRead(false, 1, code)) << code << ": an open's own failure";
  }
  // A failure the client would not have recovered either: the open ends as it would without uCache.
  for (int code : {kErrConnectionError, kErrOperationExpired, kErrStreamDisconnect, kErrDataError,
                   kErrErrorResponse, kErrNotFound, 0})
    EXPECT_FALSE(openOnceMoreAfterCheckRead(true, 1, code)) << code;
}

// The once-more open is made only while the open's budget has time for it, and its checks' first
// read is held to what is left and the margin: the job's open never runs past its budget by more
// than kHeaderReadFloorS.
TEST(OpenPolicy, OnceMoreOpenWithinTheBudget) {
  EXPECT_EQ(kOnceMoreMinMs, 1000);
  EXPECT_TRUE(openOnceMoreInTime(1000, 0)) << "a second left, no wait";
  EXPECT_FALSE(openOnceMoreInTime(999, 0)) << "less than a second";
  EXPECT_TRUE(openOnceMoreInTime(1500, 500)) << "the wait and a second";
  EXPECT_FALSE(openOnceMoreInTime(1499, 500));
  EXPECT_FALSE(openOnceMoreInTime(0, 0)) << "the budget spent";
  EXPECT_FALSE(openOnceMoreInTime(-20000, 0)) << "the budget overrun";
  EXPECT_TRUE(openOnceMoreInTime(170000, 0)) << "most of it left";
  EXPECT_FALSE(openOnceMoreInTime(170000, 169001));
  EXPECT_FALSE(openOnceMoreInTime(INT64_MAX, UINT64_MAX)) << "no wraps";
  EXPECT_TRUE(openOnceMoreInTime(INT64_MAX, 0));
  // Its first read: no more than what is left of the open's time, rounded up; 0 for none.
  const int64_t m = int64_t(kHeaderReadFloorS) * 1000; // the margin
  EXPECT_EQ(headerReadTimeoutS(60, 0, 100000 + m), 60) << "time enough: as before";
  EXPECT_EQ(headerReadTimeoutS(60, 0, 20000 + m), 25) << "the budget's rest and the margin";
  EXPECT_EQ(headerReadTimeoutS(60, 0, 20001 + m), 26) << "rounded up";
  EXPECT_EQ(headerReadTimeoutS(1, 0, 1000 + m), kHeaderReadFloorS) << "the floor, inside the cap";
  EXPECT_EQ(headerReadTimeoutS(1, 0, 300 + m), kHeaderReadFloorS);
  EXPECT_EQ(headerReadTimeoutS(1, 900, 100 + m), kHeaderReadFloorS);
  EXPECT_EQ(headerReadTimeoutS(1, 0, 4200), kHeaderReadFloorS) << "the budget overrun by 0.8 s";
  EXPECT_EQ(headerReadTimeoutS(1, 0, 1), 1) << "a moment left: a second";
  EXPECT_EQ(headerReadTimeoutS(1, 0, -3000 + m), 2) << "overrun by 3 s: 2 s of the margin";
  EXPECT_EQ(headerReadTimeoutS(1, 0, 0), 0) << "none left";
  EXPECT_EQ(headerReadTimeoutS(1, 0, -1), 0);
  EXPECT_EQ(headerReadTimeoutS(60, 0, INT64_MIN), 0);
  // Without the cap (the default), as before.
  EXPECT_EQ(headerReadTimeoutS(60, 20000), headerReadTimeoutS(60, 20000, INT64_MAX));
  EXPECT_EQ(headerReadTimeoutS(3, 0), kHeaderReadFloorS);
  // The bound: an attempt that starts with `rem` of the budget left, given at most that for its
  // open, ends its first read no later than the budget's end plus the margin.
  for (int64_t rem = kOnceMoreMinMs; rem <= 60000; rem += 333)
    for (int64_t openMs = 0; openMs <= rem; openMs += 997) {
      const int t =
          headerReadTimeoutS(static_cast<int>((rem + 999) / 1000), openMs, rem - openMs + m);
      ASSERT_LE(openMs + int64_t(t) * 1000, rem + m + 999) << rem << " " << openMs;
    }
}

// A page read the client failed with an invalid response (its answer to a copy's empty answer) is
// a failure of the copy only on an armed handle, inside the confirmed size; anything else is as it
// was.
TEST(OpenPolicy, EmptyPageReadInsideConfirmed) {
  constexpr uint64_t N = 106379; // the confirmed size
  EXPECT_EQ(kErrInvalidResponse, 303);
  EXPECT_TRUE(emptyPageReadInsideConfirmed(true, true, 303, N, 40000, 65536)) << "past a cut";
  EXPECT_TRUE(emptyPageReadInsideConfirmed(true, true, 303, N, 0, 4096));
  EXPECT_TRUE(emptyPageReadInsideConfirmed(true, true, 303, N, N - 4096, 4096)) << "to the end";
  // Not an armed handle, not a page read, no confirmed size: as without uCache.
  EXPECT_FALSE(emptyPageReadInsideConfirmed(false, true, 303, N, 40000, 65536)) << "not armed";
  EXPECT_FALSE(emptyPageReadInsideConfirmed(true, false, 303, N, 40000, 65536)) << "a plain read";
  EXPECT_FALSE(emptyPageReadInsideConfirmed(true, true, 303, 0, 40000, 65536)) << "no size";
  // Another failure: as it was (a data error, a short answer, are judged elsewhere).
  for (int code : {0, kErrDataError, kErrSocketError, kErrOperationExpired, kErrNotFound, 302, 305,
                   kErrErrorResponse})
    EXPECT_FALSE(emptyPageReadInsideConfirmed(true, true, code, N, 40000, 65536)) << code;
  // A page read that reaches past the confirmed size: every copy answers it with no bytes past the
  // end of the file, so it says nothing of the copy.
  EXPECT_FALSE(emptyPageReadInsideConfirmed(true, true, 303, N, N - 1000, 65536)) << "across it";
  EXPECT_FALSE(emptyPageReadInsideConfirmed(true, true, 303, N, N + 5000, 65536)) << "past the end";
  EXPECT_FALSE(emptyPageReadInsideConfirmed(true, true, 303, N, N, 1)) << "at the end";
  EXPECT_FALSE(emptyPageReadInsideConfirmed(true, true, 303, N, 40000, 0)) << "nothing asked";
  EXPECT_FALSE(emptyPageReadInsideConfirmed(true, true, 303, N, UINT64_MAX - 10, 65536)) << "wraps";
}

// A short answer counts against a copy only when the request lies wholly inside the confirmed size.
TEST(OpenPolicy, ShortInsideConfirmed) {
  constexpr uint64_t N = 1000000; // the confirmed size
  EXPECT_TRUE(shortInsideConfirmed(N, 500000, 65536, 0)) << "no bytes at all, inside the file";
  EXPECT_TRUE(shortInsideConfirmed(N, 500000, 65536, 100)) << "a part of them";
  EXPECT_TRUE(shortInsideConfirmed(N, N - 65536, 65536, 65535)) << "a request that ends at the end";
  // Exact answers, and answers of more than was asked, are not short.
  EXPECT_FALSE(shortInsideConfirmed(N, 500000, 65536, 65536));
  EXPECT_FALSE(shortInsideConfirmed(N, 500000, 65536, 70000));
  // A request that reaches past the end of the file is answered short, as it should be.
  EXPECT_FALSE(shortInsideConfirmed(N, N - 1000, 65536, 1000)) << "across the end";
  EXPECT_FALSE(shortInsideConfirmed(N, N - 65535, 65536, 100)) << "one byte past the end";
  EXPECT_FALSE(shortInsideConfirmed(N, N + 10, 65536, 0)) << "wholly past the end";
  EXPECT_FALSE(shortInsideConfirmed(N, N, 65536, 0)) << "at the end";
  // No confirmed size: never held against a copy, whatever the answer.
  EXPECT_FALSE(shortInsideConfirmed(0, 500000, 65536, 0));
  EXPECT_FALSE(shortInsideConfirmed(0, 0, 1, 0));
  // Nothing asked, or arguments that wrap.
  EXPECT_FALSE(shortInsideConfirmed(N, 500000, 0, 0));
  EXPECT_FALSE(shortInsideConfirmed(N, ~0ull - 10, 100, 0)) << "off + len wraps";
}

// The span a read of the checks made at open asks for: whole pages when its bytes are to be kept,
// within the copy's size and the room left; exactly what the checks need otherwise.
TEST(OpenPolicy, CheckReadSpan) {
  constexpr uint64_t N = 1756917; // a copy's size
  const auto span = [](bool keep, uint32_t page, uint64_t size, uint64_t at, uint64_t len,
                       uint64_t room) {
    const ByteSpan s = checkReadSpan(keep, page, size, at, len, room);
    return std::to_string(s.off) + "+" + std::to_string(s.len) + (s.kept ? " kept" : "");
  };
  // The header: the first page.
  EXPECT_EQ(span(true, 4096, N, 0, 300, kOpenCheckKeepBytes), "0+4096 kept");
  EXPECT_EQ(span(true, 16384, N, 0, 300, kOpenCheckKeepBytes), "0+16384 kept") << "another page size";
  // The list of top-level objects: both ends widened to pages, the last one to the copy's end.
  EXPECT_EQ(span(true, 4096, N, 1755000, 1600, kOpenCheckKeepBytes), "1753088+3829 kept");
  EXPECT_EQ(span(true, 4096, N, 1700000, 5000, kOpenCheckKeepBytes), "1699840+8192 kept");
  EXPECT_EQ(span(true, 4096, N, 4096, 4096, kOpenCheckKeepBytes), "4096+4096 kept") << "pages already";
  // A copy smaller than a page: all of it; one smaller than the first read, too (its tail page).
  EXPECT_EQ(span(true, 4096, 1000, 0, 300, kOpenCheckKeepBytes), "0+1000 kept");
  EXPECT_EQ(span(true, 4096, 300, 0, 300, kOpenCheckKeepBytes), "0+300 kept");
  EXPECT_EQ(span(true, 4096, 200, 0, 200, kOpenCheckKeepBytes), "0+200 kept");
  // As asked: the bytes are not kept, the widened span takes more than the room left, no page size,
  // nothing asked, or the span does not lie inside the copy.
  EXPECT_EQ(span(false, 4096, N, 0, 300, kOpenCheckKeepBytes), "0+300");
  EXPECT_EQ(span(true, 4096, N, 0, 300, 4095), "0+300") << "no room for the page";
  EXPECT_EQ(span(true, 4096, N, 0, 300, 4096), "0+4096 kept") << "just room for it";
  EXPECT_EQ(span(true, 4096, 30000000, 1000000, 2400000, kOpenCheckKeepBytes), "1000000+2400000")
      << "a list longer than the bytes kept for a copy";
  EXPECT_EQ(span(true, 0, N, 0, 300, kOpenCheckKeepBytes), "0+300");
  EXPECT_EQ(span(true, 4096, N, 100, 0, kOpenCheckKeepBytes), "100+0");
  EXPECT_EQ(span(true, 4096, N, N - 10, 300, kOpenCheckKeepBytes), std::to_string(N - 10) + "+300")
      << "past the copy's end";
  EXPECT_EQ(span(true, 4096, N, N, 1, kOpenCheckKeepBytes), std::to_string(N) + "+1");
  EXPECT_EQ(span(true, 4096, 0, 0, 300, kOpenCheckKeepBytes), "0+300") << "no size";
  EXPECT_EQ(span(true, 4096, N, UINT64_MAX - 5, 300, kOpenCheckKeepBytes),
            std::to_string(UINT64_MAX - 5) + "+300")
      << "wraps";
  EXPECT_EQ(kOpenCheckKeepBytes, 1u << 20);
}

// The age, the size and the window the advice of a failed open gives, in plain words.
TEST(OpenPolicy, AgeBytesAndWindowText) {
  EXPECT_EQ(ageText(0), "0 seconds");
  EXPECT_EQ(ageText(1), "1 second");
  EXPECT_EQ(ageText(59), "59 seconds");
  EXPECT_EQ(ageText(60), "1 minute");
  EXPECT_EQ(ageText(119), "1 minute");
  EXPECT_EQ(ageText(120), "2 minutes");
  EXPECT_EQ(ageText(3599), "59 minutes");
  EXPECT_EQ(ageText(3600), "1 hour");
  EXPECT_EQ(ageText(86399), "23 hours");
  EXPECT_EQ(ageText(86400), "1 day");
  EXPECT_EQ(ageText(8 * 86400 + 5000), "8 days");
  EXPECT_EQ(ageText(400 * 86400), "400 days");
  EXPECT_EQ(bytesText(0), "0 bytes");
  EXPECT_EQ(bytesText(1), "1 byte");
  EXPECT_EQ(bytesText(999), "999 bytes");
  EXPECT_EQ(bytesText(1000), "1.0 kB");
  EXPECT_EQ(bytesText(131072), "131.1 kB");
  EXPECT_EQ(bytesText(999949), "999.9 kB");
  EXPECT_EQ(bytesText(999950), "1.0 MB") << "never 1000.0 kB";
  EXPECT_EQ(bytesText(1756917), "1.8 MB");
  EXPECT_EQ(bytesText(16935324), "16.9 MB");
  EXPECT_EQ(bytesText(4200000000ull), "4.2 GB");
  EXPECT_EQ(bytesText(3000000000000ull), "3.0 TB");
  EXPECT_EQ(bytesText(UINT64_MAX), "18446.7 PB") << "the largest unit stays";
  // The window: the age and a day, rounded up to whole days.
  EXPECT_EQ(suggestedWindowS(0), 86400u);
  EXPECT_EQ(suggestedWindowS(5), 172800u);
  EXPECT_EQ(suggestedWindowS(86399), 172800u);
  EXPECT_EQ(suggestedWindowS(86400), 172800u);
  EXPECT_EQ(suggestedWindowS(86401), 259200u);
  EXPECT_EQ(suggestedWindowS(8 * 86400 + 5000), 10u * 86400);
  for (uint64_t a : {0ull, 1ull, 59ull, 86399ull, 86400ull, 700000ull, 9ull * 86400 + 1}) {
    EXPECT_GT(suggestedWindowS(a), a + 86399) << a << ": at least a day longer than the age";
    EXPECT_EQ(suggestedWindowS(a) % 86400, 0u) << a;
  }
  constexpr uint64_t kMax = uint64_t(INT_MAX) / 86400 * 86400;
  EXPECT_EQ(suggestedWindowS(kMax - 86400), kMax);
  EXPECT_EQ(suggestedWindowS(kMax), kMax) << "the most the setting holds";
  EXPECT_EQ(suggestedWindowS(UINT64_MAX), kMax);
}

// The advice of an open that no copy passed, when the cache holds an earlier copy of the file.
TEST(OpenPolicy, StaleEntryAdvice) {
  EXPECT_EQ(staleEntryAdvice(5, 1756917, 1756917),
            "uCache holds an earlier copy of this file, last confirmed at an origin 5 seconds ago, "
            "holding the whole file (1.8 MB): a job run with a freshness window longer than that "
            "(UCACHE_REVALIDATE_S=172800, or revalidate_seconds in ucache.conf) reads it from the "
            "cache without asking an origin");
  EXPECT_EQ(staleEntryAdvice(8 * 86400 + 5000, 131072, 1756917),
            "uCache holds an earlier copy of this file, last confirmed at an origin 8 days ago, "
            "holding 131.1 kB of its 1.8 MB (7%): a job run with a freshness window longer than "
            "that (UCACHE_REVALIDATE_S=864000, or revalidate_seconds in ucache.conf) reads the "
            "parts it holds from the cache without asking an origin; reads of the parts it does "
            "not hold will still fail");
  // A part rounds down, never to the whole file; a sliver says so.
  EXPECT_NE(staleEntryAdvice(1, 999999, 1000000).find("(99%)"), std::string::npos);
  EXPECT_NE(staleEntryAdvice(1, 4096, 4000000000ull).find("(less than 1%)"), std::string::npos);
  EXPECT_NE(staleEntryAdvice(1, 999999, 1000000).find("will still fail"), std::string::npos);
  // An entry that holds nothing gives no advice (nothing could be read from it); an empty file is
  // held whole.
  EXPECT_EQ(staleEntryAdvice(5, 0, 1756917), "");
  EXPECT_NE(staleEntryAdvice(5, 0, 0).find("holding the whole file (0 bytes)"), std::string::npos);
  // Short enough to fit beside the copies (kErrorTextMax).
  EXPECT_LE(staleEntryAdvice(UINT64_MAX, UINT64_MAX - 1, UINT64_MAX).size(), 400u);
}

// The advice of an earlier copy is given only when every copy was refused by a check or could not
// be reached: never for a file that is not there, a refusal to authorize, or a request a server
// rejects.
TEST(OpenPolicy, StaleAdviceAfter) {
  using namespace xrdwire;
  for (bool first : {true, false}) {
    for (int code :
         {0, kErrDataError, kErrInvalidAddr, kErrSocketError, kErrSocketTimeout,
          kErrSocketDisconnected, kErrPollerError, kErrSocketOptError, kErrStreamDisconnect,
          kErrConnectionError, kErrInvalidSession, kErrTlsError, kErrHandShakeFailed,
          kErrOperationExpired, kErrInvalidResponse, kErrRedirectLimit})
      EXPECT_EQ(staleAdviceAfter(code, 0, first), StaleAdvice::kAllows) << code << " " << first;
    for (int errNo : {kXrIOError, kXrFSError, kXrServerError, kXrOverloaded, kXrNoServer,
                      kXrReqTimedOut, kXrNoMemory})
      EXPECT_EQ(staleAdviceAfter(kErrErrorResponse, errNo, first), StaleAdvice::kAllows) << errNo;
    // A refusal to authorize or authenticate, a request rejected for what it asks, anything else.
    for (int errNo : {kXrNotAuthorized, kXrFileLocked, kXrFileNotOpen, kXrCancelled, kXrItExists,
                      3000, 3001, 3002, 3006, 3009, 3013, 3016, 3019, 0})
      EXPECT_EQ(staleAdviceAfter(kErrErrorResponse, errNo, first), StaleAdvice::kBlocks) << errNo;
    for (int code : {kErrAuthFailed, 203, kErrInternal, kErrOperationInterrupted, kErrRedirect, 3,
                     9, 13, 1, 2, 201, 305, 999})
      EXPECT_EQ(staleAdviceAfter(code, 0, first), StaleAdvice::kBlocks) << code;
  }
  // Not found: at the first attempt the federation has no copy; later, no other copy is left.
  EXPECT_EQ(staleAdviceAfter(kErrErrorResponse, kXrNotFound, true), StaleAdvice::kBlocks);
  EXPECT_EQ(staleAdviceAfter(kErrNotFound, 0, true), StaleAdvice::kBlocks);
  EXPECT_EQ(staleAdviceAfter(kErrErrorResponse, kXrNotFound, false), StaleAdvice::kNeutral);
  EXPECT_EQ(staleAdviceAfter(kErrNotFound, 0, false), StaleAdvice::kNeutral);
}

// The advice goes after the error's text, whole: the text is cut first, within the bound.
TEST(OpenPolicy, WithAdvice) {
  const std::string advice = staleEntryAdvice(5, 131072, 1756917);
  EXPECT_EQ(withAdvice("uCache: /f.root: no site could open it: a (a:1): refused", advice),
            "uCache: /f.root: no site could open it: a (a:1): refused. " + advice);
  EXPECT_EQ(withAdvice("ends with a period.", "Advice"), "ends with a period. Advice");
  EXPECT_EQ(withAdvice("text", ""), "text") << "no advice";
  EXPECT_EQ(withAdvice("", advice), advice) << "no text";
  // A text of every copy at the bound: cut, so the whole stays at the bound and the advice is whole.
  std::vector<SiteFailure> many(20, {"s.x.org:1094", "x.org", "s.x.org", std::string(500, 'e')});
  const std::string list = allSitesFailedText("root://r//store/f.root", many);
  ASSERT_EQ(list.size(), kErrorTextMax);
  const std::string t = withAdvice(list, advice);
  EXPECT_EQ(t.size(), kErrorTextMax);
  EXPECT_EQ(t.substr(t.size() - advice.size()), advice) << "the advice is never cut";
  EXPECT_EQ(t.rfind("uCache: root://r//store/f.root: no site could open it: x.org (s.x.org:1094): ", 0), 0u)
      << "the text's start, which names the file and the first copy, stays";
  EXPECT_EQ(withAdvice(std::string(5000, 't'), advice, 2048).size(), 2048u);
  // An advice that does not fit the bound with its join is not added.
  EXPECT_EQ(withAdvice("text", std::string(1023, 'a')), "text");
  EXPECT_EQ(withAdvice("text", std::string(1022, 'a')).size(), 1024u);
  EXPECT_EQ(withAdvice("", std::string(1025, 'a')), "");
}

// A handle trusted at Open whose own open has not succeeded gives no request to its File, which
// never opened: with adaptive_federation off once it has lost its trust (its entry gone by its
// first read), with it on also before. A handle not trusted at Open, or whose open succeeded, is
// never held.
TEST(OpenPolicy, OwnOpenPending) {
  for (bool federation : {false, true}) {
    EXPECT_TRUE(ownOpenPending(true, false, federation, false)) << "lost its trust: " << federation;
    for (bool trusted : {false, true}) {
      EXPECT_FALSE(ownOpenPending(true, true, federation, trusted)) << "its open succeeded";
      EXPECT_FALSE(ownOpenPending(false, false, federation, trusted)) << "not trusted at Open";
      EXPECT_FALSE(ownOpenPending(false, true, federation, trusted));
    }
  }
  // A handle that still trusts its entry: held with the switch on only (the switch off serves it
  // as it always was).
  EXPECT_TRUE(ownOpenPending(true, false, true, true));
  EXPECT_FALSE(ownOpenPending(true, false, false, true));
}

// Hosts to avoid by tried= pattern: which hop of a chain is one, and how a sequence counts them.
TEST(OpenPolicy, AvoidedHop) {
  const std::vector<std::string> cern = {"*.cern.ch"};
  std::string p;
  // global -> regional -> data server: the data server matches.
  std::vector<Hop> chain = {hop("g.cern.ch", kMeta, true), hop("r.infn.it", kManager),
                            hop("fst1.cern.ch", kServer)};
  EXPECT_EQ(avoidedHop(chain, cern, &p), 2);
  EXPECT_EQ(p, "*.cern.ch");
  // A redirector is never avoided, whatever its name.
  chain = {hop("g.cern.ch", kMeta, true), hop("r.cern.ch", kManager), hop("d.fnal.gov", kServer)};
  EXPECT_EQ(avoidedHop(chain, cern), -1);
  // A data server that sent the open on (an EOS front end) matches too: the later one first.
  chain = {hop("g.x.org", kMeta, true), hop("eoscms.cern.ch", kServer),
           hop("p0612.cern.ch", kServer, false, "cap.sym=x&cap.msg=y")};
  EXPECT_EQ(avoidedHop(chain, cern), 2);
  EXPECT_EQ(avoidedHop(chain, {"eoscms*"}), 1);
  // No pattern, or none matching: none.
  EXPECT_EQ(avoidedHop(chain, {}), -1);
  EXPECT_EQ(avoidedHop(chain, {"*.fnal.gov"}), -1);
  // A direct URL at a data server that matches: that host (no other copy to go to).
  EXPECT_EQ(avoidedHop({hop("d.cern.ch", kServer)}, cern), 0);
  // Addresses match as text.
  EXPECT_EQ(avoidedHop({hop("r.x.org", kManager), hop("127.0.0.1", kServer)}, {"127.0.0.*"}), 1);
  EXPECT_EQ(exclusionNameOf("D.Cern.Ch."), "d.cern.ch");
  EXPECT_EQ(exclusionNameOf("127.0.0.1"), "");
  EXPECT_EQ(exclusionNameOf("2001:db8::1"), "");
  EXPECT_EQ(exclusionNameOf("localhost"), "");
}

TEST(OpenPolicy, AvoidedCopiesInASequence) {
  const auto avoidedAt = [](const char* server, const char* name) {
    SiteFailure f;
    f.server = server;
    f.site = f.domain = siteOf(std::string(server).substr(0, std::string(server).rfind(':')));
    f.name = name;
    f.avoided = "*.cern.ch";
    f.error = "its host matches tried=*.cern.ch";
    return f;
  };
  std::vector<SiteFailure> failed;
  // Each avoided server of one site is a new server, never a repeat of its site, and counts as
  // no site: the open is asked again, the names accumulating.
  for (int i = 1; i <= 6; ++i) {
    const std::string s = "fst" + std::to_string(i) + ".cern.ch:1094";
    EXPECT_EQ(afterFailure(failed, avoidedAt(s.c_str(), s.substr(0, s.find(':')).c_str()), 2, false),
              AfterFailure::kRetry)
        << i;
  }
  EXPECT_EQ(avoidedNames(failed).size(), 6u);
  EXPECT_EQ(avoidedNames(failed).front(), "fst1.cern.ch");
  EXPECT_EQ(exclusionNames(failed).size(), 6u);
  // The same server again (an address masks nothing at a redirector): a repeat, the list asked.
  EXPECT_EQ(afterFailure(failed, avoidedAt("fst1.cern.ch:1094", "fst1.cern.ch"), 2, false),
            AfterFailure::kRepeat);
  // A real failure at another server of that site is not a repeat of the avoided ones.
  SiteFailure real;
  real.server = "fst9.cern.ch:1094";
  real.site = real.domain = "cern.ch";
  real.error = "stalled";
  EXPECT_EQ(afterFailure(failed, real, 2, false), AfterFailure::kRetry);
  // ... and counts as a site: the bound of 2 sites is met with the next.
  SiteFailure other = real;
  other.server = "d.fnal.gov:1094";
  other.site = other.domain = "fnal.gov";
  EXPECT_EQ(afterFailure(failed, other, 2, false), AfterFailure::kStop);
  // Nothing avoided: no names.
  EXPECT_TRUE(avoidedNames({real, other}).empty());

  // orderLocations: another server of an avoided server's site is an ordinary candidate.
  Location l1;
  l1.host = "fst2.cern.ch";
  l1.port = 1094;
  Location l2 = l1;
  l2.host = "d.infn.it";
  const auto order = orderLocations({l1, l2}, {}, {avoidedAt("fst1.cern.ch:1094", "fst1.cern.ch")});
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0].host, "fst2.cern.ch");
  // failedBefore: an avoided entry stands for its server alone.
  SiteFailure same = real;
  same.server = "fst2.cern.ch:1094";
  EXPECT_FALSE(failedBefore({avoidedAt("fst1.cern.ch:1094", "fst1.cern.ch")}, same));
  same.server = "fst1.cern.ch:1094";
  EXPECT_TRUE(failedBefore({avoidedAt("fst1.cern.ch:1094", "fst1.cern.ch")}, same));

  // allAvoided: at least one avoided copy, and every other failure a "not found".
  SiteFailure nf;
  nf.server = "r.infn.it:1094";
  nf.site = nf.domain = "infn.it";
  nf.notFound = true;
  nf.error = "[ERROR] Server responded with an error: [3011] No servers are available";
  EXPECT_TRUE(allAvoided({avoidedAt("a.cern.ch:1094", "a.cern.ch")}));
  EXPECT_TRUE(allAvoided({avoidedAt("a.cern.ch:1094", "a.cern.ch"), nf,
                          avoidedAt("b.cern.ch:1094", "b.cern.ch")}));
  EXPECT_FALSE(allAvoided({nf}));
  EXPECT_FALSE(allAvoided({}));
  EXPECT_FALSE(allAvoided({avoidedAt("a.cern.ch:1094", "a.cern.ch"), real}));
  const std::string text = avoidedText(
      "root://g:1094//store/f.root", {avoidedAt("a.cern.ch:1094", "a.cern.ch"), nf,
                                      avoidedAt("[2001:db8::1]:1094", "")});
  EXPECT_EQ(text, "uCache: root://g:1094//store/f.root: every copy found is at a host the URL's "
                  "tried= asks to avoid, and nothing was read there: a.cern.ch:1094 "
                  "(tried=*.cern.ch); [2001:db8::1]:1094 (tried=*.cern.ch)");
  std::vector<SiteFailure> lots(200, avoidedAt("a-long-name-of-a-server.cern.ch:1094", "x"));
  EXPECT_LE(avoidedText("f", lots).size(), kErrorTextMax);
}
