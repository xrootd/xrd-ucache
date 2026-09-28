// The run history as `history --json` prints it and `publish` sends it: the
// raw fields the report service interprets, in both forms. The published form
// is the one that must hold to "domains, never hosts".
#include "HistoryJson.h"

#include "Publish.h"
#include "TestUtil.h"
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace ucache;

namespace {

void writeLines(const std::string& dir, const std::string& stem,
                const std::vector<std::string>& counters, const std::vector<std::string>& files) {
  {
    std::ofstream o(dir + "/" + stem + ".jsonl");
    for (const auto& l : counters)
      o << l << "\n";
  }
  std::ofstream o(dir + "/" + stem + ".files.jsonl");
  for (const auto& l : files)
    o << l << "\n";
}

std::string fileLine(const std::string& key, uint64_t originSize, const std::string& extra) {
  return "{\"ts\":1099,\"key\":\"" + key +
         "\",\"opens\":1,\"served_bytes\":1000,\"ram_bytes\":0,\"replica_bytes\":0,"
         "\"wire_bytes\":0,\"span_us\":5,\"origin_size\":" +
         std::to_string(originSize) +
         ",\"read_sig\":\"ab\",\"read_buckets\":1,\"mode\":\"cached\"" +
         (extra.empty() ? "" : "," + extra) + "}";
}

// A run in the current shape over three files on two domains.
void currentRun(const std::string& dir) {
  writeLines(dir, "node-7.example-4242-1000-0",
             {"{\"ts\":1100,\"pid\":4242,\"ts_ms\":1100500,\"start_ms\":1000250,\"opens\":3,"
              "\"files_opened\":3,\"hit_bytes\":3000,\"served_bytes\":3000,\"origin_bytes\":0,"
              "\"counter_source\":\"perf\",\"pmu_duty\":0.8123,\"instructions\":99,"
              "\"hist_relay_rt_us\":[0,1,2]}"},
             {fileLine("root://eos1.cern.ch:1094//store/a.root", 5000,
                       "\"ts_ms\":1099100,\"orig_bytes\":1500,\"unique_bytes\":1200,"
                       "\"origin_rt_us\":[1,1]"),
              fileLine("root://eos2.cern.ch//store/b.root", 6000,
                       "\"ts_ms\":1099200,\"orig_bytes\":1600,\"unique_bytes\":1300,"
                       "\"origin_rt_us\":[0,2]"),
              fileLine("root://xrootd.example.org//data/c.root", 7000,
                       "\"ts_ms\":1099300,\"orig_bytes\":1700,\"origin_rt_us\":[3]")});
}

Json history(const std::string& dir, bool redacted) {
  const auto runs = loadRuns(dir);
  Json j;
  std::string err;
  EXPECT_TRUE(Json::parse(historyJson(runs, 10, redacted, /*oldestFirst=*/true), j, &err)) << err;
  return j;
}

const Json& firstRun(const Json& h) {
  static const Json none;
  const Json* runs = h.get("runs");
  if (!runs || !runs->isArray() || runs->arr.empty()) {
    ADD_FAILURE() << "no runs";
    return none;
  }
  return runs->arr.front();
}

std::string num(const Json& o, const char* key) {
  const Json* v = o.get(key);
  return v && v->isNumber() ? v->s : "<absent>";
}

std::vector<std::string> nums(const Json& arr) {
  std::vector<std::string> out;
  for (const auto& v : arr.arr)
    out.push_back(v.s);
  return out;
}

} // namespace

TEST(HistoryJson, CarriesTheRawFieldsOfTheCurrentRecords) {
  test::TempDir td;
  currentRun(td.path());
  for (bool redacted : {false, true}) {
    const Json h = history(td.path(), redacted);
    const Json& r = firstRun(h);
    EXPECT_EQ(num(r, "duration_ms"), "100250");
    EXPECT_EQ(num(r, "duration_s"), "100") << "kept as it was";
    EXPECT_EQ(num(r, "orig_read_bytes"), "4800");
    EXPECT_EQ(num(r, "orig_files"), "3");
    EXPECT_EQ(num(r, "unique_bytes"), "2500");
    EXPECT_EQ(num(r, "unique_files"), "2") << "the third file's range set said nothing";
    EXPECT_EQ(num(r, "files_bytes"), "18000");
    EXPECT_EQ(r.str("counter_source"), "perf");
    EXPECT_EQ(num(r, "pmu_duty"), "0.8123");
    ASSERT_TRUE(r.get("relay_rt_us") && r.get("relay_rt_us")->isArray());
    EXPECT_EQ(nums(*r.get("relay_rt_us")), (std::vector<std::string>{"0", "1", "2"}));
  }
}

TEST(HistoryJson, EachByteSumSaysHowManyFilesItIsOver) {
  // A file whose byte counts are unknown (a replica read the map could not
  // name, say) is in neither sum, and the counts of files say so: without
  // them the read total over two files would sit beside the size of three.
  test::TempDir td;
  writeLines(td.path(), "h-1-1000-0", {"{\"ts\":1100,\"hit_bytes\":3000,\"served_bytes\":3000}"},
             {fileLine("root://o.cern.ch//a", 5000,
                       "\"ts_ms\":1099100,\"orig_bytes\":1500,\"unique_bytes\":1200"),
              fileLine("root://o.cern.ch//b", 6000, "\"ts_ms\":1099200,\"orig_bytes\":1600"),
              fileLine("root://o.cern.ch//c", 7000, "\"ts_ms\":1099300")});
  for (bool redacted : {false, true}) {
    const Json h = history(td.path(), redacted);
    const Json& r = firstRun(h);
    EXPECT_EQ(num(r, "orig_read_bytes"), "3100");
    EXPECT_EQ(num(r, "orig_files"), "2");
    EXPECT_EQ(num(r, "unique_bytes"), "1200");
    EXPECT_EQ(num(r, "unique_files"), "1");
    EXPECT_EQ(num(r, "files_bytes"), "18000");
  }
}

TEST(HistoryJson, OriginTimingsGoByDomainWhenPublishedAndByHostLocally) {
  test::TempDir td;
  currentRun(td.path());
  auto byOrigin = [](const Json& r) {
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    const Json* o = r.get("origins_rt");
    if (!o || !o->isArray())
      return out;
    for (const auto& e : o->arr)
      out.emplace_back(e.str("origin"),
                       e.get("hist") ? nums(*e.get("hist")) : std::vector<std::string>{});
    return out;
  };
  const Json pub = firstRun(history(td.path(), /*redacted=*/true));
  using V = std::vector<std::string>;
  EXPECT_EQ(byOrigin(pub), (std::vector<std::pair<std::string, V>>{
                               {"root://cern.ch", V{"1", "3"}}, // two hosts, one domain, summed
                               {"root://example.org", V{"3"}}}));
  const Json local = firstRun(history(td.path(), /*redacted=*/false));
  EXPECT_EQ(byOrigin(local),
            (std::vector<std::pair<std::string, V>>{{"root://eos1.cern.ch", V{"1", "1"}},
                                                    {"root://eos2.cern.ch", V{"0", "2"}},
                                                    {"root://xrootd.example.org", V{"3"}}}));
}

TEST(HistoryJson, ThePublishedFormNamesNoHostNoPathAndNoProcess) {
  test::TempDir td;
  currentRun(td.path());
  const std::string pub = historyJson(loadRuns(td.path()), 10, /*redacted=*/true, true);
  for (const char* leak : {"eos1", "eos2", "xrootd.example", "/store/", "/data/", "node-7", "4242"})
    EXPECT_EQ(pub.find(leak), std::string::npos) << leak << " in " << pub;
  // ... while the local form keeps what the machine's owner can use.
  const std::string local = historyJson(loadRuns(td.path()), 10, /*redacted=*/false, false);
  EXPECT_NE(local.find("root://eos1.cern.ch"), std::string::npos);
  EXPECT_NE(local.find("\"pid\":4242"), std::string::npos);
}

TEST(HistoryJson, AnOldRecordAddsOnlyWhatItCanSay) {
  // Written before the raw fields existed: every new key is absent rather than
  // zero, except the input size, which old records always carried.
  test::TempDir td;
  writeLines(td.path(), "h-1-1000-0", {"{\"ts\":1100,\"hit_bytes\":1000,\"served_bytes\":1000}"},
             {fileLine("root://o.cern.ch//a", 5000, "")});
  for (bool redacted : {false, true}) {
    const Json h = history(td.path(), redacted);
    const Json& r = firstRun(h);
    EXPECT_EQ(num(r, "files_bytes"), "5000");
    for (const char* k :
         {"duration_ms", "orig_read_bytes", "orig_files", "unique_bytes", "unique_files",
          "counter_source", "pmu_duty", "relay_rt_us", "origins_rt"})
      EXPECT_FALSE(r.has(k)) << k << (redacted ? " (published)" : " (local)");
    EXPECT_EQ(num(r, "duration_s"), "100");
    EXPECT_EQ(r.str("kind"), "warm");
  }
}
