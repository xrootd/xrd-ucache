#include "UrlKey.h"

#include <gtest/gtest.h>

using namespace ucache;

TEST(UrlKey, NormalizesCaseSlashesAndPort) {
  auto k = UrlKey::parse("root://EOSpublic.CERN.ch//eos//opendata/./cms/file.root");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://eospublic.cern.ch:1094/eos/opendata/cms/file.root");
  EXPECT_EQ(k->host, "eospublic.cern.ch");
  EXPECT_EQ(k->scheme, "root");
  EXPECT_EQ(k->hashHex.size(), 64u);
}

TEST(UrlKey, ExplicitPortPreserved) {
  auto k = UrlKey::parse("root://h.example:2094//a/b");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://h.example:2094/a/b");
}

TEST(UrlKey, DotDotResolution) {
  auto k = UrlKey::parse("root://h//a/b/../c/../../d");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://h:1094/d");
  k = UrlKey::parse("root://h//../..//x");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://h:1094/x");
}

TEST(UrlKey, StripsAllCgiByDefault) {
  auto k = UrlKey::parse("root://h//f.root?authz=SECRET&xrd.wantprot=unix");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://h:1094/f.root");
  EXPECT_EQ(k->key.find("SECRET"), std::string::npos);
  // Same file with different tokens => same cache object.
  auto k2 = UrlKey::parse("root://h//f.root?authz=OTHER");
  ASSERT_TRUE(k2);
  EXPECT_EQ(k->hashHex, k2->hashHex);
}

TEST(UrlKey, KeepCgiWhitelistSortedCanonical) {
  std::vector<std::string> keep = {"zkey", "akey"};
  auto k = UrlKey::parse("root://h//f?zkey=1&authz=SECRET&akey=2", keep);
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://h:1094/f?akey=2&zkey=1");
  // Order-insensitive: same params, different order => same key.
  auto k2 = UrlKey::parse("root://h//f?akey=2&zkey=1&authz=X", keep);
  ASSERT_TRUE(k2);
  EXPECT_EQ(k->hashHex, k2->hashHex);
}

TEST(UrlKey, UserinfoDropped) {
  auto k = UrlKey::parse("root://user@h//f");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://h:1094/f");
}

TEST(UrlKey, Ipv6) {
  auto k = UrlKey::parse("root://[::1]:2094//f");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://[::1]:2094/f");
  EXPECT_EQ(k->host, "[::1]");
}

TEST(UrlKey, ParseFailures) {
  EXPECT_FALSE(UrlKey::parse("not a url"));
  EXPECT_FALSE(UrlKey::parse("root://"));
  EXPECT_FALSE(UrlKey::parse("root://hostonly"));
  EXPECT_FALSE(UrlKey::parse("://h//f"));
  EXPECT_FALSE(UrlKey::parse("root://[::1//f"));
}

TEST(UrlKey, ObjectPaths) {
  auto k = UrlKey::parse("root://h//f");
  ASSERT_TRUE(k);
  std::string shard = k->hashHex.substr(0, 2);
  EXPECT_EQ(k->objectDir("/c"), "/c/objects/" + shard);
  EXPECT_EQ(k->dataPath("/c"), "/c/objects/" + shard + "/" + k->hashHex + ".data");
  EXPECT_EQ(k->metaPath("/c"), "/c/objects/" + shard + "/" + k->hashHex + ".meta");
}

TEST(UrlKey, HostGlobs) {
  EXPECT_TRUE(hostMatchesAny({"*.cern.ch"}, "eospublic.cern.ch"));
  EXPECT_TRUE(hostMatchesAny({"exact.host"}, "EXACT.host"));
  EXPECT_FALSE(hostMatchesAny({"*.cern.ch"}, "cern.ch"));
  EXPECT_FALSE(hostMatchesAny({}, "anything"));
  EXPECT_TRUE(hostMatchesAny({"a", "b", "eos*"}, "eospublic.cern.ch"));
}

// Removing named parameters leaves everything else byte for byte (auth tokens
// in particular).
TEST(UrlKey, StripCgiParams) {
  const std::vector<std::string> kRouting = {"tried", "triedrc"};
  EXPECT_EQ(stripCgiParams("root://h//f.root", kRouting), "root://h//f.root");
  EXPECT_EQ(stripCgiParams("root://h//f.root?tried=a,b&triedrc=resel", kRouting),
            "root://h//f.root");
  EXPECT_EQ(stripCgiParams("root://h//f.root?authz=TOK&tried=a&xrd.wantprot=unix", kRouting),
            "root://h//f.root?authz=TOK&xrd.wantprot=unix");
  EXPECT_EQ(stripCgiParams("root://h//f.root?tried=a&authz=TOK", kRouting),
            "root://h//f.root?authz=TOK");
  // 'triedx' is NOT 'tried': exact key match only.
  EXPECT_EQ(stripCgiParams("root://h//f.root?triedx=1", kRouting),
            "root://h//f.root?triedx=1");
}

// The cache's own origin opens keep the servers a reader asked to avoid
// (tried=) and drop only a reselection request (a triedrc= whose last reason
// is resel). Repeated tried= lists are merged into one, since only one reaches
// the server.
TEST(UrlKey, OwnOpenUrl) {
  const std::string base = "root://h//f.root";
  const auto own = [&base](const std::string& query) { return ownOpenUrl(base + "?" + query); };
  EXPECT_EQ(ownOpenUrl(base), base);
  // A user's list, with no reason: kept.
  EXPECT_EQ(own("tried=a,b"), base + "?tried=a,b");
  // A reselection request: removed, whatever came before its last reason.
  EXPECT_EQ(own("tried=a,b&triedrc=resel"), base);
  EXPECT_EQ(own("tried=a,b&triedrc=resel,resel"), base);
  EXPECT_EQ(own("tried=a&triedrc=enoent,resel"), base);
  // The last reason is the one a redirector reads: not a reselection.
  EXPECT_EQ(own("tried=a&triedrc=resel,ioerr"), base + "?tried=a&triedrc=resel,ioerr");
  // A user's list ahead of a reselection request survives it.
  EXPECT_EQ(own("tried=u&tried=a,b&triedrc=resel"), base + "?tried=u");
  // Repeated lists are merged, in order.
  EXPECT_EQ(own("tried=u&tried=a,b"), base + "?tried=u,a,b");
  // A list the client grew after a failure: kept with its reason.
  EXPECT_EQ(own("tried=u,f&triedrc=ioerr"), base + "?tried=u,f&triedrc=ioerr");
  // Empty items (a source with no known server) go.
  EXPECT_EQ(own("tried=,a"), base + "?tried=a");
  // Every other parameter passes byte for byte, in order.
  EXPECT_EQ(own("authz=TOK&tried=a&xrd.wantprot=unix"),
            base + "?authz=TOK&tried=a&xrd.wantprot=unix");
  // 'triedx' is not 'tried': exact key match only.
  EXPECT_EQ(own("triedx=1"), base + "?triedx=1");
  // Host names compare without case; the first spelling is kept.
  EXPECT_EQ(own("tried=A,a"), base + "?tried=A");
  // The merged list sits where the first tried= was, its reason right after it.
  EXPECT_EQ(own("tried=u&authz=TOK&tried=f&triedrc=ioerr&xrd.k=v"),
            base + "?tried=u,f&triedrc=ioerr&authz=TOK&xrd.k=v");
  // A user's list, then a client's reselection request behind it.
  EXPECT_EQ(own("authz=TOK&tried=bad.example.org&tried=s1,s2&triedrc=resel"),
            base + "?authz=TOK&tried=bad.example.org");
  // Nothing left to avoid: the query goes with it.
  EXPECT_EQ(own("tried=,"), base);
  // A reselection request with no list: removed.
  EXPECT_EQ(own("triedrc=resel"), base);
  // Any other reason with no host to follow stays, as the reader wrote it.
  EXPECT_EQ(own("triedrc=ioerr"), base + "?triedrc=ioerr");
  EXPECT_EQ(own("tried=,&triedrc=ioerr"), base + "?triedrc=ioerr");
  EXPECT_EQ(own("authz=TOK&triedrc=enoent&xrd.k=v"), base + "?authz=TOK&triedrc=enoent&xrd.k=v");
}
