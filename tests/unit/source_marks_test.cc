#include "SourceMarks.h"

#include "TestUtil.h"
#include <fstream>
#include <gtest/gtest.h>

using namespace ucache;
using test::TempDir;

namespace {
// A process that dies right after a write's data reached the disk: the step that would finish
// the write (an ftruncate of a file rewritten in place, or the rename of a new one) never comes.
class DiesAfterDataIO : public RealIO {
 public:
  bool armed = false;
  int ftruncate(int fd, uint64_t length) override {
    return armed ? -1 : RealIO::ftruncate(fd, length);
  }
  int rename(const std::string& from, const std::string& to) override {
    return armed ? -1 : RealIO::rename(from, to);
  }
};
std::string bytesOf(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

// A write that dies before it is finished leaves the file as it was or as it was to be, byte for
// byte: never the new version with the old one's tail (where a cleared mark could come back).
TEST(SourceMarks, AWriteThatDiesLeavesOneWholeVersion) {
  TempDir t;
  DiesAfterDataIO io;
  const std::string p = sourcesPath(t.path());
  ASSERT_TRUE(markSource(io, p, "a.one.org:1094", "a site with a long name", "one.org", 1000));
  ASSERT_TRUE(markSource(io, p, "b.two.org:1094", "B", "two.org", 1000));
  ASSERT_TRUE(markSource(io, p, "c.three.org:1094", "C", "three.org", 1000));
  const std::string before = bytesOf(p);
  std::vector<SourceMark> m;
  ASSERT_TRUE(loadSources(io, p, m));
  std::vector<SourceMark> rest;
  for (const auto& x : m)
    if (x.server != "a.one.org:1094")
      rest.push_back(x);
  const std::string after = serializeSources(rest);
  io.armed = true;
  clearSource(io, p, "a.one.org:1094", 1100); // dies after its data
  io.armed = false;
  const std::string now = bytesOf(p);
  EXPECT_TRUE(now == before || now == after) << "a mix of two versions:\n" << now;
  ASSERT_TRUE(clearSource(io, p, "a.one.org:1094", 1100)); // made again, it holds
  ASSERT_TRUE(loadSources(io, p, m));
  EXPECT_EQ(m.size(), 2u);
}

// A clock that goes back never lengthens a mark: one whose end lies further ahead than an hour
// (and a minute) is taken as ended.
TEST(SourceMarks, AClockGoneBackNeverLengthensAMark) {
  SourceMark m{"a:1", "A", "one.org", 4, 10000, 10000, 10000 + 3600};
  EXPECT_TRUE(markLive(m, 10000)) << "an hour left: as long as a mark lasts";
  EXPECT_TRUE(markLive(m, 10000 - 60)) << "within the slack";
  EXPECT_FALSE(markLive(m, 10000 - 61)) << "the clock went back past it: ended";
  EXPECT_FALSE(markLive(m, 10000 - 7200));
  EXPECT_FALSE(markLive(m, 13600)) << "its end reached";
  std::vector<SourceMark> two = {m, SourceMark{"b:1", "A", "one.org", 1, 10000, 10000, 10600}};
  EXPECT_TRUE(siteMarked(two, "A", "one.org", 10000));
  EXPECT_FALSE(siteMarked(two, "A", "one.org", 10000 - 7200)) << "the site's marks too";
  EXPECT_EQ(markedAtS(two, "a:1", "A", "one.org", 10000 - 7200), 0u);
}

// 10 minutes, doubled at each repeat, at most an hour.
TEST(SourceMarks, AMarkLastsTenMinutesDoubledToAnHour) {
  EXPECT_EQ(siteMarkSeconds(1), 600u);
  EXPECT_EQ(siteMarkSeconds(2), 1200u);
  EXPECT_EQ(siteMarkSeconds(3), 2400u);
  EXPECT_EQ(siteMarkSeconds(4), 3600u);
  EXPECT_EQ(siteMarkSeconds(9), 3600u);
  EXPECT_EQ(siteMarkSeconds(0), 600u);
}

TEST(SourceMarks, MarksAreWrittenRenewedAndClearedInTheFile) {
  TempDir t;
  IOBackend& io = RealIO::instance();
  const std::string p = sourcesPath(t.path());
  EXPECT_EQ(p, t.path() + "/sources");
  std::vector<SourceMark> m;
  ASSERT_TRUE(loadSources(io, p, m)); // no file: no marks
  EXPECT_TRUE(m.empty());
  SourceMark w;
  ASSERT_TRUE(markSource(io, p, "CMS01.Site.ORG:1094", "T2 X%Y", "site.org", 1000, &w));
  EXPECT_EQ(w.server, "cms01.site.org:1094");
  EXPECT_EQ(w.count, 1u);
  EXPECT_EQ(w.untilS, 1600u);
  ASSERT_TRUE(markSource(io, p, "cms01.site.org:1094", "", "site.org", 2000, &w)); // a repeat
  EXPECT_EQ(w.count, 2u);
  EXPECT_EQ(w.firstS, 1000u);
  EXPECT_EQ(w.untilS, 2000u + 1200u) << "doubled";
  EXPECT_EQ(w.siteName, "T2 X%Y") << "a name known stays when a later failure has none";
  ASSERT_TRUE(markSource(io, p, "b.site.org:1094", "", "site.org", 2100));
  ASSERT_TRUE(loadSources(io, p, m));
  ASSERT_EQ(m.size(), 2u);
  EXPECT_EQ(m[0].siteName, "T2 X%Y") << "a site with a space and a '%' survives the file";
  EXPECT_EQ(m[1].siteName, "");
  EXPECT_EQ(markSite(m[1]), "site.org");
  std::ifstream raw(p);
  std::string text((std::istreambuf_iterator<char>(raw)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("server cms01.site.org:1094 T2%20X%25Y site.org 2 1000 2000 3200"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("server b.site.org:1094 - site.org 1"), std::string::npos) << text;
  EXPECT_TRUE(markLive(m[0], 3199));
  EXPECT_FALSE(markLive(m[0], 3200));
  bool was = false;
  ASSERT_TRUE(clearSource(io, p, "cms01.site.org:1094", 2500, &was));
  EXPECT_TRUE(was);
  ASSERT_TRUE(loadSources(io, p, m));
  ASSERT_EQ(m.size(), 1u) << "a success clears the mark and its row";
  ASSERT_TRUE(markSource(io, p, "cms01.site.org:1094", "T2", "site.org", 2600, &w));
  EXPECT_EQ(w.count, 1u) << "after a success the row starts again";
  // A mark long ended is forgotten at the next write.
  ASSERT_TRUE(markSource(io, p, "c.site.org:1094", "", "site.org", 2100 + 600 + kSourceMarkForgetS + 1));
  ASSERT_TRUE(loadSources(io, p, m));
  for (const auto& x : m)
    EXPECT_NE(x.server, "b.site.org:1094");
}

TEST(SourceMarks, AFileThisBuildDoesNotKnowReadsAsNoMarks) {
  EXPECT_TRUE(parseSources("ucache-sources 2\nserver a:1 s d 1 1 1 9\n").empty());
  const auto m = parseSources("ucache-sources 1\nserver a:1 s d 1 1 1 9\nnonsense\nserver b:1\n"
                              "server c:1 - d 0 1 1 9\nserver e:1 s 1 1 1 9\n");
  ASSERT_EQ(m.size(), 1u) << "a line that does not parse, or a count of 0, is skipped";
  EXPECT_EQ(m[0].server, "a:1");
  EXPECT_EQ(decodeSite("-"), "");
  EXPECT_EQ(encodeSite(""), "-");
}

// A site is marked once two or more of its servers are, its servers compared by names when both
// are known, else by domains; a copy is marked by its server's own mark, or by its site's, at the
// newest failure among them.
TEST(SourceMarks, ASiteIsMarkedAtTwoServers) {
  std::vector<SourceMark> m = {{"a:1", "T1", "one.org", 1, 100, 100, 700},
                               {"b:1", "", "one.org", 1, 200, 200, 800},
                               {"c:1", "T2", "two.org", 1, 300, 300, 900},
                               {"d:1", "T3", "three.org", 1, 50, 50, 60},
                               {"e:1", "T4", "two.org", 1, 400, 400, 900}};
  EXPECT_TRUE(siteMarked(m, "T1", "one.org", 500)) << "a named, b not yet, one domain";
  EXPECT_TRUE(siteMarked(m, "", "one.org", 500));
  EXPECT_FALSE(siteMarked(m, "T2", "two.org", 500)) << "c and e: two names in one domain";
  EXPECT_FALSE(siteMarked(m, "T1", "one.org", 750)) << "a's mark ended: one server left";
  EXPECT_EQ(markedAtS(m, "a:1", "T1", "one.org", 500), 200u) << "the site's newest failure";
  EXPECT_EQ(markedAtS(m, "x:1", "", "one.org", 500), 200u) << "another server of a marked site";
  EXPECT_EQ(markedAtS(m, "c:1", "T2", "two.org", 500), 300u) << "its own mark";
  EXPECT_EQ(markedAtS(m, "f:1", "T2", "two.org", 500), 0u) << "its site has one marked server";
  EXPECT_EQ(markedAtS(m, "d:1", "T3", "three.org", 500), 0u) << "ended";
  EXPECT_EQ(markedAtS(m, "C:1", "T2", "two.org", 500), 300u) << "a server in any case";
  EXPECT_TRUE(sameSiteIds("T1", "one.org", "", "one.org"));
  EXPECT_FALSE(sameSiteIds("T1", "one.org", "T5", "one.org"));
  EXPECT_TRUE(sameSiteIds("T1", "one.org", "T1", "other.org"));
  EXPECT_FALSE(sameSiteIds("", "", "", ""));
}
