#include "Stats.h"

#include "IOBackend.h"
#include "TestUtil.h"
#include "Trace.h"
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace ucache;

TEST(Histogram, Log2Bucketing) {
  Histogram h;
  h.add(0);
  h.add(1); // both -> bucket 0
  h.add(2);
  h.add(3); // -> bucket 1
  h.add(4); // -> bucket 2
  h.add(1024); // -> bucket 10
  h.add(~0ull); // clamps to last bucket
  EXPECT_EQ(h.b[0].load(), 2u);
  EXPECT_EQ(h.b[1].load(), 2u);
  EXPECT_EQ(h.b[2].load(), 1u);
  EXPECT_EQ(h.b[10].load(), 1u);
  EXPECT_EQ(h.b[Histogram::kBuckets - 1].load(), 1u);
}

TEST(Histogram, JsonTrimsTrailingZeros) {
  Histogram h;
  EXPECT_EQ(h.toJson(), "[]");
  h.add(1);
  h.add(4);
  EXPECT_EQ(h.toJson(), "[1,0,1]");
}

TEST(Histogram, EmptyClearAndAddFrom) {
  Histogram a, b;
  EXPECT_TRUE(a.empty());
  a.add(4);
  b.add(4);
  b.add(1000);
  EXPECT_FALSE(a.empty());
  a.addFrom(b); // bucket by bucket
  EXPECT_EQ(a.toJson(), "[0,0,2,0,0,0,0,0,0,1]");
  EXPECT_EQ(b.toJson(), "[0,0,1,0,0,0,0,0,0,1]"); // the source is untouched
  a.clear();
  EXPECT_TRUE(a.empty());
  EXPECT_EQ(a.toJson(), "[]");
}

TEST(Stats, JsonBodyHasAllCounters) {
  Stats s;
  s.opens = 3;
  s.hitBytes = 12345;
  s.crcFailures = 1;
  s.hitReadUs.add(7);
  s.replicaReadBytes = 65536;
  s.readvCalls = 9;
  s.copierHandles = 2;
  s.coldReplicaSkipped = 4;
  s.slotMapsMade = 5;
  s.slotMapOpens = 6;
  s.reqReadBytes.add(4096);
  s.hitReadSize.add(4096);
  s.slotCrcFailures = 2;
  s.readerWaitUs[3] = 7;
  s.readerThreads = 8;
  s.coldOriginFailures = 6;
  s.slotReadFallbacks = 4;
  s.slotPagesDecoded = 5;
  s.keptReads = 3;
  s.poolTasks = 11;
  s.poolQueueHighWater = 7;
  s.coldRequestUs.add(1000); // bucket 9
  std::string j = "{" + s.toJsonBody() + "}";
  // Spot-check names and values (schema is docs/STATS.md, and is relied on).
  EXPECT_NE(j.find("\"opens\":3"), std::string::npos);
  EXPECT_NE(j.find("\"hit_bytes\":12345"), std::string::npos);
  EXPECT_NE(j.find("\"crc_failures\":1"), std::string::npos);
  EXPECT_NE(j.find("\"origin_bytes\":0"), std::string::npos);
  EXPECT_NE(j.find("\"failopen_events\":0"), std::string::npos);
  EXPECT_NE(j.find("\"disabled_handles\":0"), std::string::npos);
  EXPECT_NE(j.find("\"copier_handles\":2"), std::string::npos);
  EXPECT_NE(j.find("\"cold_replica_skipped\":4"), std::string::npos);
  EXPECT_NE(j.find("\"cold_replica_declined\":0"), std::string::npos); // its own counter
  EXPECT_NE(j.find("\"slot_maps_made\":5"), std::string::npos);
  EXPECT_NE(j.find("\"slot_map_opens\":6"), std::string::npos);
  EXPECT_NE(j.find("\"slot_map_full\":0"), std::string::npos);
  EXPECT_NE(j.find("\"slot_crc_failures\":2"), std::string::npos);
  EXPECT_NE(j.find("\"slot_read_fallbacks\":4"), std::string::npos);
  EXPECT_NE(j.find("\"reader_wait_origin_us\":7"), std::string::npos);
  EXPECT_NE(j.find("\"reader_threads\":8"), std::string::npos);
  EXPECT_NE(j.find("\"cold_origin_failures\":6"), std::string::npos);
  EXPECT_NE(j.find("\"slot_pages_decoded\":5"), std::string::npos);
  EXPECT_NE(j.find("\"kept_reads\":3"), std::string::npos);
  EXPECT_NE(j.find("\"kept_read_bytes\":0"), std::string::npos);
  EXPECT_NE(j.find("\"pool_tasks\":11"), std::string::npos);
  EXPECT_NE(j.find("\"pool_busy_us\":0"), std::string::npos);
  EXPECT_NE(j.find("\"pool_queue_high_water\":7"), std::string::npos);
  EXPECT_NE(j.find("\"hist_cold_request_us\":[0,0,0,0,0,0,0,0,0,1]"), std::string::npos);
  EXPECT_NE(j.find("\"hist_slot_read_us\":[]"), std::string::npos);
  EXPECT_NE(j.find("\"hist_pool_queue_us\":[]"), std::string::npos);
  EXPECT_NE(j.find("\"hist_hit_read_us\":[0,0,1]"), std::string::npos);
  // Read-shape surface: sizes are histogrammed in log2 BYTES (4096 => bucket 12).
  EXPECT_NE(j.find("\"replica_read_bytes\":65536"), std::string::npos);
  EXPECT_NE(j.find("\"readv_calls\":9"), std::string::npos);
  EXPECT_NE(j.find("\"hist_req_read_bytes\":[0,0,0,0,0,0,0,0,0,0,0,0,1]"), std::string::npos);
  EXPECT_NE(j.find("\"hist_hit_read_bytes\":[0,0,0,0,0,0,0,0,0,0,0,0,1]"), std::string::npos);
  EXPECT_NE(j.find("\"hist_replica_read_bytes\":[]"), std::string::npos);
  // Relayed answer times are their own histogram, never the cache's fetches'.
  s.relayRtUs.add(1000);
  j = "{" + s.toJsonBody() + "}";
  EXPECT_NE(j.find("\"hist_relay_rt_us\":[0,0,0,0,0,0,0,0,0,1]"), std::string::npos);
  EXPECT_NE(j.find("\"hist_origin_rt_us\":[]"), std::string::npos);
  // Braces balanced, no trailing comma before '}'.
  EXPECT_EQ(j.find(",}"), std::string::npos);
}

TEST(StatsAggregate, LastLinePerFileSummedAcrossProcesses) {
  test::TempDir td;
  std::string dir = td.path() + "/stats";
  ::mkdir(dir.c_str(), 0700);
  // Process A: two cumulative snapshots — the LAST supersedes.
  {
    std::ofstream f(dir + "/hostA-100-1.jsonl");
    f << "{\"ts\":1,\"pid\":100,\"opens\":5,\"hit_bytes\":100,\"origin_bytes\":40,"
         "\"origin_reads\":2,\"hist_hit_read_us\":[1,2,3]}\n";
    f << "{\"ts\":2,\"pid\":100,\"opens\":10,\"hit_bytes\":200,\"origin_bytes\":50,"
         "\"origin_reads\":4,\"hist_hit_read_us\":[1,2,3,4]}\n"; // final for A
  }
  // Process B: single line.
  {
    std::ofstream f(dir + "/hostB-200-1.jsonl");
    f << "{\"ts\":9,\"pid\":200,\"opens\":3,\"hit_bytes\":30,\"served_bytes\":7,"
         "\"failopen_events\":1,\"copier_handles\":2,\"cold_replica_skipped\":3,"
         "\"slot_maps_made\":2,\"slot_map_opens\":7,\"slot_map_full\":1}\n";
  }
  // Noise that must be ignored: wrong suffix, and an empty file.
  { std::ofstream(dir + "/notes.txt") << "ignore me\n"; }
  { std::ofstream(dir + "/hostC-300-1.jsonl"); }

  StatsTotals t = aggregateStats(dir);
  EXPECT_EQ(t.files, 2);          // empty + .txt skipped
  EXPECT_EQ(t.opens, 13u);        // A's last (10) + B (3)
  EXPECT_EQ(t.hitBytes, 230u);    // 200 + 30
  EXPECT_EQ(t.originBytes, 50u);  // A's last only (not 40+50)
  EXPECT_EQ(t.originReads, 4u);   // A's last
  EXPECT_EQ(t.servedBytes, 7u);   // B
  EXPECT_EQ(t.failopenEvents, 1u);
  EXPECT_EQ(t.copierHandles, 2u); // B only; A's lines predate the counter and read as 0
  EXPECT_EQ(t.coldReplicaSkipped, 3u);
  EXPECT_EQ(t.slotMapsMade, 2u);
  EXPECT_EQ(t.slotMapOpens, 7u);
  EXPECT_EQ(t.slotMapFull, 1u);
}

// Stats files written on either side of the coalescing change use the same
// counter NAMES with different meanings (hit_disk_reads counted pages, now
// counts runs), so summing them yields a number that means nothing. The
// aggregate must say so, and the CLI then withholds the derived per-read
// figures rather than printing a plausible-looking average of the two.
TEST(StatsAggregate, MixedSchemaIsFlagged) {
  test::TempDir td;
  std::string dir = td.path() + "/stats";
  ::mkdir(dir.c_str(), 0700);
  const char* oldLine = "{\"ts\":1,\"opens\":1,\"hit_disk_reads\":100,\"hit_disk_bytes\":409600,"
                        "\"replica_reads\":10,\"replica_bytes_served\":655360}\n";
  const char* newLine = "{\"ts\":2,\"opens\":1,\"hit_disk_reads\":10,\"hit_disk_bytes\":409600,"
                        "\"replica_reads\":2,\"replica_bytes_served\":131072,"
                        "\"replica_read_bytes\":140000,\"readv_calls\":3}\n";
  { std::ofstream(dir + "/a.jsonl") << newLine; }
  { std::ofstream(dir + "/b.jsonl") << newLine; }
  EXPECT_FALSE(aggregateStats(dir).schemaMixed); // both new: comparable
  { std::ofstream(dir + "/c.jsonl") << oldLine; }
  EXPECT_TRUE(aggregateStats(dir).schemaMixed); // one pre-change file spoils it

  // With only pre-change files the replica mean falls back to served bytes,
  // which is the right basis for a file that has no bytes-moved counter.
  test::TempDir td2;
  std::string dir2 = td2.path() + "/stats";
  ::mkdir(dir2.c_str(), 0700);
  { std::ofstream(dir2 + "/only-old.jsonl") << oldLine; }
  auto t = aggregateStats(dir2);
  EXPECT_FALSE(t.schemaMixed);
  EXPECT_EQ(t.replicaReadBytes, 655360u);
}

// The origin open and failure counters: written by toJsonBody under their
// names, summed back by aggregateStats, and absent (zero) in older files.
TEST(StatsAggregate, OriginOpenAndFailureCountersRoundTrip) {
  Stats s;
  s.originOpenAttempts = 7;
  s.originOpenFailures = 2;
  for (int i = 0; i < 6; ++i)
    s.originFailures[i] = static_cast<uint64_t>(10 + i);
  s.recordsDropped = 3;
  s.originOpenUs.add(1000); // bucket 9
  s.openFailovers = 5;
  s.opensRecovered = 4;
  s.opensFailedEverywhere = 1;
  s.openDeadlineExpiries = 2;
  s.failoverLocates = 6;
  s.offlineCopiesSkipped = 7;
  s.openCheckBytesKept = 8192;
  const std::string body = s.toJsonBody();
  for (const char* f :
       {"\"origin_open_attempts\":7", "\"origin_open_failures\":2",
        "\"origin_failures_client\":10", "\"origin_failures_site\":11",
        "\"origin_failures_notfound\":12", "\"origin_failures_data\":13",
        "\"origin_failures_expired\":14", "\"origin_failures_other\":15",
        "\"records_dropped\":3", "\"hist_origin_open_us\":[0,0,0,0,0,0,0,0,0,1]",
        "\"open_failovers\":5", "\"opens_recovered\":4", "\"opens_failed_everywhere\":1",
        "\"open_deadline_expiries\":2", "\"failover_locates\":6",
        "\"offline_copies_skipped\":7", "\"open_check_bytes_kept\":8192"})
    EXPECT_NE(body.find(f), std::string::npos) << f;
  EXPECT_EQ(("{" + body + "}").find(",}"), std::string::npos);
  test::TempDir td;
  std::string dir = td.path() + "/stats";
  ::mkdir(dir.c_str(), 0700);
  { std::ofstream(dir + "/a.jsonl") << "{\"ts\":1," << body << "}\n"; }
  { std::ofstream(dir + "/b.jsonl") << "{\"ts\":1," << body << "}\n"; }
  { std::ofstream(dir + "/old.jsonl") << "{\"ts\":1,\"opens\":4}\n"; } // before the counters
  const StatsTotals t = aggregateStats(dir);
  EXPECT_EQ(t.files, 3);
  EXPECT_EQ(t.originOpenAttempts, 14u);
  EXPECT_EQ(t.originOpenFailures, 4u);
  for (int i = 0; i < 6; ++i)
    EXPECT_EQ(t.originFailures[i], static_cast<uint64_t>(2 * (10 + i))) << i;
  EXPECT_EQ(t.recordsDropped, 6u);
  EXPECT_EQ(t.openFailovers, 10u);
  EXPECT_EQ(t.opensRecovered, 8u);
  EXPECT_EQ(t.opensFailedEverywhere, 2u);
  EXPECT_EQ(t.openDeadlineExpiries, 4u);
  EXPECT_EQ(t.failoverLocates, 12u);
  EXPECT_EQ(t.offlineCopiesSkipped, 14u);
  EXPECT_EQ(t.openCheckBytesKept, 16384u);
  EXPECT_EQ(t.histOriginOpen, (std::vector<uint64_t>{0, 0, 0, 0, 0, 0, 0, 0, 0, 2}));
  EXPECT_EQ(t.opens, 4u) << "no new counter collides with an older one's name";
}

TEST(StatsAggregate, MissingDirIsZero) {
  StatsTotals t = aggregateStats("/nonexistent/ucache/stats");
  EXPECT_EQ(t.files, 0);
  EXPECT_EQ(t.opens, 0u);
}

// A trace record with further members keeps every common member as it was and
// in the same order, so a reader of the older records reads it unchanged; one
// too long for its line is not written at all, never cut.
TEST(Tracer, FurtherMembersFollowTheCommonOnes) {
  test::TempDir d;
  const std::string path = d.path() + "/t.trace.jsonl";
  {
    RealIO io;
    Tracer t(io, path, 1);
    t.rec("hit", "root://h//f", 1, 2, 3);
    t.rec("wire", "root://h//f", 4, 5, 6, true, "\"ds\":\"h:1094\",\"t0\":7,\"nc\":2");
    t.rec("wire", "root://h//f", 8, 9, 10, true, "\"ds\":\"" + std::string(600, 'x') + "\"");
  }
  std::ifstream in(path);
  std::vector<std::string> lines;
  for (std::string l; std::getline(in, l);)
    lines.push_back(l);
  ASSERT_EQ(lines.size(), 4u) << "meta, the key's legend, two records; the long one is dropped";
  EXPECT_EQ(lines[0], "{\"op\":\"meta\",\"sample\":1}");
  auto endsWith = [](const std::string& l, const std::string& tail) {
    return l.size() >= tail.size() && l.compare(l.size() - tail.size(), tail.size(), tail) == 0;
  };
  EXPECT_EQ(lines[2].rfind("{\"t\":", 0), 0u);
  EXPECT_NE(lines[2].find(",\"op\":\"hit\",\"k\":\""), std::string::npos);
  EXPECT_TRUE(endsWith(lines[2], ",\"off\":1,\"len\":2,\"us\":3}")) << lines[2];
  EXPECT_EQ(lines[3].rfind("{\"t\":", 0), 0u);
  EXPECT_NE(lines[3].find(",\"op\":\"wire\",\"k\":\""), std::string::npos);
  EXPECT_TRUE(endsWith(lines[3],
                       ",\"off\":4,\"len\":5,\"us\":6,\"ds\":\"h:1094\",\"t0\":7,\"nc\":2}"))
      << lines[3];
}
