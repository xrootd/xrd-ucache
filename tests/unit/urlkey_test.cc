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
  // Both lists: on the allow list (or none given) and not on the deny list.
  EXPECT_TRUE(hostPassesLists({}, {}, "eospublic.cern.ch")) << "no lists: every host";
  EXPECT_TRUE(hostPassesLists({"*.cern.ch"}, {}, "eospublic.cern.ch"));
  EXPECT_FALSE(hostPassesLists({"*.cern.ch"}, {}, "xrootd.example.org")) << "not on the allow list";
  EXPECT_FALSE(hostPassesLists({}, {"eos*"}, "eospublic.cern.ch")) << "on the deny list";
  EXPECT_FALSE(hostPassesLists({"*.cern.ch"}, {"eos*"}, "eospublic.cern.ch")) << "deny wins";
  EXPECT_TRUE(hostPassesLists({"*.cern.ch"}, {"eos*"}, "xrootd.cern.ch"));
  EXPECT_TRUE(hostPassesLists({}, {"127.0.0.1"}, "::1")) << "an address is a host like any other";
  EXPECT_FALSE(hostPassesLists({"nohost.invalid"}, {}, "127.0.0.1"));
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

// With adaptive_federation on, of the hosts in a reselection request the cache's
// own open keeps exactly those this process saw fail for the file, and sends
// them with triedrc=ioerr; the others (servers the reader reads from without
// trouble) and the resel go.
TEST(UrlKey, OwnOpenUrlKeepsFailedServersOfAReselection) {
  const std::string base = "root://h//f.root";
  const auto own = [&base](const std::string& query, const std::vector<std::string>& failed) {
    return ownOpenUrl(base + "?" + query, failed);
  };
  EXPECT_EQ(own("tried=a.x.org,b.x.org&triedrc=resel", {"a.x.org"}),
            base + "?tried=a.x.org&triedrc=ioerr");
  EXPECT_EQ(own("tried=a.x.org,b.x.org&triedrc=resel", {"a.x.org", "b.x.org"}),
            base + "?tried=a.x.org,b.x.org&triedrc=ioerr");
  // None of them failed here: dropped whole, as without the switch.
  EXPECT_EQ(own("tried=a.x.org,b.x.org&triedrc=resel", {}), base);
  EXPECT_EQ(own("tried=a.x.org,b.x.org&triedrc=resel", {"c.x.org"}), base);
  // Compared without a port and without case; the reader's spelling is kept.
  EXPECT_EQ(own("tried=A.X.org:1094,b.x.org&triedrc=resel", {"a.x.org"}),
            base + "?tried=A.X.org:1094&triedrc=ioerr");
  // A user's list ahead of the request is kept as it is, the failed host after it.
  EXPECT_EQ(own("tried=u.x.org&tried=a.x.org,b.x.org&triedrc=resel", {"b.x.org"}),
            base + "?tried=u.x.org,b.x.org&triedrc=ioerr");
  EXPECT_EQ(own("tried=u.x.org&tried=a.x.org&triedrc=resel", {}), base + "?tried=u.x.org");
  // Every other parameter passes byte for byte.
  EXPECT_EQ(own("authz=TOK&tried=a.x.org,b.x.org&triedrc=resel&xrd.k=v", {"a.x.org"}),
            base + "?authz=TOK&tried=a.x.org&triedrc=ioerr&xrd.k=v");
  // Not a reselection request: the list does not change.
  EXPECT_EQ(own("tried=a.x.org,b.x.org&triedrc=enoent", {"c.x.org"}),
            base + "?tried=a.x.org,b.x.org&triedrc=enoent");
  EXPECT_EQ(own("tried=a.x.org", {"a.x.org"}), base + "?tried=a.x.org");
}

// The base of a retry of the reader's own open: every host kept, the
// reselection request's included, and no triedrc.
TEST(UrlKey, MergeTriedKeepsEveryReaderHost) {
  const std::string base = "root://h//f.root";
  EXPECT_EQ(mergeTried(base), base);
  EXPECT_EQ(mergeTried(base + "?tried=a,b&triedrc=resel"), base + "?tried=a,b");
  EXPECT_EQ(mergeTried(base + "?tried=u&tried=a,b&triedrc=resel"), base + "?tried=u,a,b");
  EXPECT_EQ(mergeTried(base + "?authz=T&tried=a&triedrc=ioerr&xrd.k=v"),
            base + "?authz=T&tried=a&xrd.k=v");
  EXPECT_EQ(mergeTried(base + "?triedrc=enoent&x=1"), base + "?x=1");
}

TEST(UrlKey, WithExclusions) {
  const std::string base = "root://h:1094//store/f.root";
  // Ours after the reader's, with the reason; a URL without a query gets one.
  EXPECT_EQ(withExclusions(base, {"c.site.org"}, "ioerr"),
            base + "?tried=c.site.org&triedrc=ioerr");
  EXPECT_EQ(withExclusions(base + "?tried=a,b", {"c.site.org"}, "ioerr"),
            base + "?tried=a,b,c.site.org&triedrc=ioerr");
  // The reader's reason is replaced (a redirector reads only one); never resel.
  EXPECT_EQ(withExclusions(base + "?tried=a&triedrc=resel", {"c.site.org"}, "enoent"),
            base + "?tried=a,c.site.org&triedrc=enoent");
  EXPECT_EQ(withExclusions(base + "?tried=a&triedrc=resel", {}, "ioerr"),
            base + "?tried=a&triedrc=ioerr");
  // Only the two reasons of a failure: anything else, a reselection included,
  // is sent as ioerr. (Managers, the first hop and EOS data servers are left
  // out where the names are chosen: OpenPolicy's exclusionNames, covered in
  // open_policy_test.)
  EXPECT_EQ(withExclusions(base, {"c.site.org"}, "resel"), base + "?tried=c.site.org&triedrc=ioerr");
  EXPECT_EQ(withExclusions(base + "?tried=a", {}, "resel"), base + "?tried=a&triedrc=ioerr");
  // Lower-cased, the root dot gone; duplicates (of the reader's too) dropped.
  EXPECT_EQ(withExclusions(base + "?tried=C.Site.Org", {"c.site.org", "D.Site.Org."}, "ioerr"),
            base + "?tried=C.Site.Org,d.site.org&triedrc=ioerr");
  EXPECT_EQ(withExclusions(base, {"c.site.org", "c.site.org"}, "ioerr"),
            base + "?tried=c.site.org&triedrc=ioerr");
  // Addresses and names without a dot mask nothing: never added.
  EXPECT_EQ(withExclusions(base, {"127.0.0.1", "[::1]", "2001:db8::7", "localhost"}, "ioerr"),
            base);
  EXPECT_EQ(withExclusions(base + "?tried=a", {"127.0.0.1", "localhost"}, "ioerr"),
            base + "?tried=a&triedrc=ioerr");
  // At most 4 of ours, the newest; the reader's are never dropped.
  EXPECT_EQ(withExclusions(base + "?tried=r",
                           {"s1.x.org", "s2.x.org", "s3.x.org", "s4.x.org", "s5.x.org"}, "ioerr"),
            base + "?tried=r,s2.x.org,s3.x.org,s4.x.org,s5.x.org&triedrc=ioerr");
  // At most 1024 bytes for the list: our oldest go first, the reader's stay.
  const std::string longReader(1000, 'r');
  EXPECT_EQ(withExclusions(base + "?tried=" + longReader, {"aaaaaaaaaa.x.org", "b.x.org"}, "ioerr"),
            base + "?tried=" + longReader + ",b.x.org&triedrc=ioerr");
  const std::string huge(1100, 'h');
  EXPECT_EQ(withExclusions(base + "?tried=" + huge, {"b.x.org"}, "ioerr"),
            base + "?tried=" + huge + "&triedrc=ioerr");
  // Every other parameter is kept in order; the list sits where tried= was.
  EXPECT_EQ(withExclusions(base + "?authz=TOK&tried=a&triedrc=ioerr&xrd.k=v", {"c.x.org"}, "ioerr"),
            base + "?authz=TOK&tried=a,c.x.org&triedrc=ioerr&xrd.k=v");
  EXPECT_EQ(withExclusions(base + "?authz=TOK", {"c.x.org"}, "ioerr"),
            base + "?authz=TOK&tried=c.x.org&triedrc=ioerr");
}

// A URL opened at another server, and the hosts it names to avoid.
TEST(UrlKey, UrlAtServerAndTriedHosts) {
  const std::string path = "//store/f.root";
  EXPECT_EQ(urlAtServer("root://r.x.org:1094/" + path, "a.y.org:1095", false),
            "root://a.y.org:1095/" + path);
  EXPECT_EQ(urlAtServer("root://u@r.x.org/" + path + "?authz=T&tried=a&triedrc=ioerr&xrd.k=v",
                        "a.y.org:1095", false),
            "root://u@a.y.org:1095/" + path + "?authz=T&xrd.k=v");
  EXPECT_EQ(urlAtServer("root://r.x.org/" + path + "?tried=a,b&triedrc=ioerr", "a.y.org:1", true),
            "root://a.y.org:1/" + path + "?tried=a,b&triedrc=ioerr");
  EXPECT_EQ(urlAtServer("root://r.x.org", "a.y.org:1", false), "root://a.y.org:1");
  EXPECT_EQ(urlAtServer("not a url", "a.y.org:1", false), "not a url");
  EXPECT_TRUE(triedHosts("root://r/f").empty());
  EXPECT_EQ(triedHosts("root://r/f?tried=A.x.org,b&tried=a.X.org,c&triedrc=resel"),
            (std::vector<std::string>{"A.x.org", "b", "c"}));
}

// Host patterns in tried=: a '*' entry is a glob on the full host name, applied by uCache.
TEST(UrlKey, HostPatternMatching) {
  EXPECT_TRUE(hostMatchesPattern("*.cern.ch", "eoscms-fst1.CERN.ch"));
  EXPECT_TRUE(hostMatchesPattern("*.CERN.CH", "eoscms.cern.ch."));      // case, the root dot
  EXPECT_FALSE(hostMatchesPattern("*.cern.ch", "cern.ch"));             // the dot is literal
  EXPECT_FALSE(hostMatchesPattern("*.cern.ch", "cmsxrootd.fnal.gov"));
  EXPECT_FALSE(hostMatchesPattern("*.cern.ch", "x.cern.ch.example.org")); // the whole name
  EXPECT_TRUE(hostMatchesPattern("cms*.fnal.gov", "cmsxrootd.fnal.gov"));
  EXPECT_TRUE(hostMatchesPattern("*xrootd*", "cmsxrootd.fnal.gov"));
  EXPECT_TRUE(hostMatchesPattern("*", "anything.at.all"));
  EXPECT_TRUE(hostMatchesPattern("a*b*c", "abc"));
  EXPECT_TRUE(hostMatchesPattern("a*b*c", "aXbYbZc"));
  EXPECT_FALSE(hostMatchesPattern("a*b*c", "aXbYcZ"));
  // Addresses match as text; an IPv6 address with or without its brackets.
  EXPECT_TRUE(hostMatchesPattern("127.0.0.*", "127.0.0.1"));
  EXPECT_FALSE(hostMatchesPattern("127.0.0.*", "127.0.1.1"));
  EXPECT_TRUE(hostMatchesPattern("2001:db8:*", "[2001:db8::7]"));
  EXPECT_TRUE(hostMatchesPattern("[2001:db8:*]", "2001:db8::7"));
  // Only '*' is special.
  EXPECT_FALSE(hostMatchesPattern("a?c.org*", "abc.org"));
  EXPECT_TRUE(hostMatchesPattern("a?c.org*", "a?c.org"));
  EXPECT_FALSE(hostMatchesPattern("*.x.org", ""));
  EXPECT_EQ(matchingPattern({"*.fnal.gov", "*.cern.ch"}, "a.cern.ch"), "*.cern.ch");
  EXPECT_EQ(matchingPattern({"*.fnal.gov", "*.cern.ch"}, "a.infn.it"), "");
  EXPECT_EQ(matchingPattern({}, "a.cern.ch"), "");
}

TEST(UrlKey, HostPatternValidity) {
  std::string why;
  EXPECT_TRUE(hostPatternValid("*.cern.ch"));
  EXPECT_TRUE(hostPatternValid("*"));
  EXPECT_TRUE(hostPatternValid("127.0.0.*"));
  EXPECT_TRUE(hostPatternValid("[2001:db8:*]"));
  EXPECT_TRUE(hostPatternValid("2001:db8::*"));
  EXPECT_TRUE(hostPatternValid("x_y-*.org"));
  EXPECT_FALSE(hostPatternValid("", &why));
  EXPECT_FALSE(hostPatternValid("a.cern.ch", &why)) << "no '*': a host name";
  EXPECT_FALSE(hostPatternValid("*.cern.ch:1094", &why));
  EXPECT_NE(why.find("port"), std::string::npos) << why;
  EXPECT_FALSE(hostPatternValid("*.cern.ch/x", &why));
  EXPECT_FALSE(hostPatternValid("* .cern.ch", &why));
  EXPECT_FALSE(hostPatternValid("[*.cern.ch", &why));
  EXPECT_FALSE(hostPatternValid("*%2E", &why));
  EXPECT_FALSE(hostPatternValid(std::string(254, '*'), &why));
}

TEST(UrlKey, SplitTriedPatterns) {
  const std::string base = "root://g.x.org:1094//store/f.root";
  // No pattern: the URL as it is.
  auto tp = splitTriedPatterns(base + "?tried=a.x.org&triedrc=ioerr");
  EXPECT_FALSE(tp.any());
  EXPECT_EQ(tp.url, base + "?tried=a.x.org&triedrc=ioerr");
  EXPECT_EQ(splitTriedPatterns(base).url, base);
  EXPECT_FALSE(splitTriedPatterns(base + "?authz=a*b").any()) << "a '*' outside tried=";
  EXPECT_EQ(splitTriedPatterns(base + "?authz=a*b").url, base + "?authz=a*b");
  // Patterns only: tried= goes; every other parameter stays byte for byte, in order.
  tp = splitTriedPatterns(base + "?authz=T&tried=*.cern.ch,*.fnal.gov&xrd.k=v");
  EXPECT_EQ(tp.patterns, (std::vector<std::string>{"*.cern.ch", "*.fnal.gov"}));
  EXPECT_TRUE(tp.malformed.empty());
  EXPECT_EQ(tp.url, base + "?authz=T&xrd.k=v");
  tp = splitTriedPatterns(base + "?tried=*.cern.ch");
  EXPECT_EQ(tp.url, base);
  // Mixed with exact names: the names stay where they were, in order; triedrc stays.
  tp = splitTriedPatterns(base + "?tried=a.x.org,*.cern.ch,b.y.org&triedrc=enoent");
  EXPECT_EQ(tp.patterns, (std::vector<std::string>{"*.cern.ch"}));
  EXPECT_EQ(tp.url, base + "?tried=a.x.org,b.y.org&triedrc=enoent");
  // Several tried= parameters, duplicates (ignoring case) once; one without patterns untouched.
  tp = splitTriedPatterns(base + "?tried=*.CERN.ch&tried=c.z.org,,d&tried=*.cern.ch,e.z.org");
  EXPECT_EQ(tp.patterns, (std::vector<std::string>{"*.CERN.ch"}));
  EXPECT_EQ(tp.url, base + "?tried=c.z.org,,d&tried=e.z.org");
  // A malformed entry is ignored and removed too.
  tp = splitTriedPatterns(base + "?tried=*.cern.ch:1094,*.fnal.gov,x*/y");
  EXPECT_EQ(tp.patterns, (std::vector<std::string>{"*.fnal.gov"}));
  EXPECT_EQ(tp.malformed, (std::vector<std::string>{"*.cern.ch:1094", "x*/y"}));
  EXPECT_EQ(tp.url, base);
  EXPECT_TRUE(tp.any());
  // At most kMaxHostPatterns patterns; the rest are malformed (ignored).
  std::string many;
  for (size_t i = 0; i < kMaxHostPatterns + 2; ++i)
    many += (many.empty() ? "" : ",") + std::string("*.s") + std::to_string(i) + ".org";
  tp = splitTriedPatterns(base + "?tried=" + many);
  EXPECT_EQ(tp.patterns.size(), kMaxHostPatterns);
  EXPECT_EQ(tp.malformed.size(), 2u);
  // The cache key is the same with and without patterns (no CGI is part of a key).
  const auto k1 = UrlKey::parse(base + "?tried=*.cern.ch,a.x.org&triedrc=ioerr");
  const auto k2 = UrlKey::parse(base);
  ASSERT_TRUE(k1 && k2);
  EXPECT_EQ(k1->key, k2->key);
  EXPECT_EQ(k1->hashHex, k2->hashHex);
}

TEST(UrlKey, WithTriedHosts) {
  const std::string base = "root://g.x.org:1094//store/f.root";
  EXPECT_EQ(withTriedHosts(base, {}), base);
  EXPECT_EQ(withTriedHosts(base, {"A.cern.CH."}), base + "?tried=a.cern.ch");
  // After the reader's hosts, which stay; the reason stays where it is.
  EXPECT_EQ(withTriedHosts(base + "?tried=r.x.org&triedrc=enoent", {"a.cern.ch", "b.cern.ch"}),
            base + "?tried=r.x.org,a.cern.ch,b.cern.ch&triedrc=enoent");
  EXPECT_EQ(withTriedHosts(base + "?authz=T&tried=a.cern.ch", {"a.cern.ch"}),
            base + "?authz=T&tried=a.cern.ch");
  // Accumulated names count as the reader's for withExclusions: never dropped for newer ones.
  std::vector<std::string> avoided;
  for (int i = 1; i <= 6; ++i)
    avoided.push_back("f" + std::to_string(i) + ".cern.ch");
  const std::string acc = withTriedHosts(base, avoided);
  EXPECT_EQ(withExclusions(acc, {"s1.x.org"}, "ioerr"),
            base + "?tried=f1.cern.ch,f2.cern.ch,f3.cern.ch,f4.cern.ch,f5.cern.ch,f6.cern.ch,"
                   "s1.x.org&triedrc=ioerr");
  // At most kMaxAvoidedNames, the newest; within 1024 bytes.
  avoided.clear();
  for (size_t i = 0; i < kMaxAvoidedNames + 3; ++i)
    avoided.push_back("f" + std::to_string(i) + ".cern.ch");
  EXPECT_EQ(triedHosts(withTriedHosts(base, avoided)).size(), kMaxAvoidedNames);
  EXPECT_EQ(triedHosts(withTriedHosts(base, avoided)).front(), "f3.cern.ch");
  const std::string longReader(1000, 'r');
  EXPECT_EQ(withTriedHosts(base + "?tried=" + longReader, {"aaaaaaaaaaaaaaa.x.org", "b.x.org"}),
            base + "?tried=" + longReader + ",b.x.org");
}
