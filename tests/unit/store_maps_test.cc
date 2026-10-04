// A map's record in the store (StoreMaps.h): both forms read back as written,
// and a record that is not one is refused; and where a map may go.
#include "StoreMaps.h"

#include "InUse.h"
#include "TestUtil.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

using namespace ucache;
using namespace ucache::transpose;

TEST(StoreMaps, AMixedMapRecordReadsBack) {
  MapHead h;
  h.madeS = 1790000000;
  h.metaSeek = 4096;
  h.keysListOff = 1234;
  h.covered = 77;
  const std::vector<uint8_t> meta(40, 0xAB), keys(12, 0xCD);
  h.metaLen = static_cast<uint32_t>(meta.size());
  h.keysListLen = static_cast<uint32_t>(keys.size());
  const auto rec = encodeMapRecord(h, meta, keys);
  ASSERT_EQ(rec.size(), h.recordLen());
  MapHead back;
  ASSERT_TRUE(decodeMapHead(rec.data(), rec.size(), back));
  EXPECT_FALSE(back.compact());
  EXPECT_EQ(back.madeS, h.madeS);
  EXPECT_EQ(back.metaSeek, h.metaSeek);
  EXPECT_EQ(back.keysListOff, h.keysListOff);
  EXPECT_EQ(back.covered, h.covered);
  EXPECT_EQ(back.recordLen(), rec.size());
  EXPECT_EQ(0, std::memcmp(rec.data() + kMapHead, meta.data(), meta.size()));
}

TEST(StoreMaps, ACompactMapRecordReadsBackWithItsRecords) {
  ColdMap m;
  m.rangeLo = 3ull << 30;
  m.headerOff = 0;
  m.header.assign(kLargeHeaderBytes, 0x11);
  m.firstOff = 8192;
  m.sums = true;
  for (uint32_t i = 0; i < 3; ++i) {
    ColdMap::Rec r;
    r.slot = 10 + i;
    r.len = 100 + i;
    r.crc = 0xC0DE0000 + i;
    r.kind = 1;
    r.storeOff = 65536 * (i + 1);
    m.recs.push_back(r);
  }
  m.rangeLen = m.firstOff;
  for (const auto& r : m.recs)
    m.rangeLen += m.extent(r);
  MapHead h;
  h.madeS = 5;
  h.metaSeek = m.rangeLo;
  h.rangeLo = m.rangeLo;
  h.rangeLen = m.rangeLen;
  h.headerOff = m.headerOff;
  h.headerLen = static_cast<uint32_t>(m.header.size());
  h.nRecs = static_cast<uint32_t>(m.recs.size());
  h.firstOff = m.firstOff;
  h.flags = 1;
  const std::vector<uint8_t> meta(64, 0x22), anchor(16, 0x33);
  h.metaLen = static_cast<uint32_t>(meta.size());
  h.keysListLen = static_cast<uint32_t>(anchor.size());
  const auto rec = encodeMapRecord(h, meta, anchor, &m);
  ASSERT_EQ(rec.size(), h.recordLen());
  MapHead back;
  ASSERT_TRUE(decodeMapHead(rec.data(), rec.size(), back));
  ASSERT_TRUE(back.compact());
  EXPECT_EQ(back.rangeLo, h.rangeLo);
  EXPECT_EQ(back.rangeLen, h.rangeLen);
  EXPECT_EQ(back.nRecs, 3u);
  EXPECT_EQ(back.firstOff, h.firstOff);
  EXPECT_EQ(back.flags, 1u);
  EXPECT_EQ(back.headerLen, kLargeHeaderBytes);
  // The last record's entry: slot, length, CRC, kind, store offset.
  const uint8_t* e = rec.data() + rec.size() - kMapRecBytes;
  uint32_t slot = 0, len = 0, crc = 0;
  uint64_t off = 0;
  std::memcpy(&slot, e, 4);
  std::memcpy(&len, e + 4, 4);
  std::memcpy(&crc, e + 8, 4);
  std::memcpy(&off, e + 16, 8);
  EXPECT_EQ(slot, 12u);
  EXPECT_EQ(len, 102u);
  EXPECT_EQ(crc, 0xC0DE0002u);
  EXPECT_EQ(e[12], 1);
  EXPECT_EQ(off, 3u * 65536);
}

TEST(StoreMaps, ARecordThatIsNotAMapIsRefused) {
  std::vector<uint8_t> junk(kMapHead2, 0x5A);
  MapHead h;
  EXPECT_FALSE(decodeMapHead(junk.data(), junk.size(), h));
  // A compact head that states nothing (an empty range) is not a map either.
  MapHead c;
  c.rangeLo = 1;
  c.rangeLen = 0;
  auto rec = encodeMapRecord(c, {}, {});
  std::memcpy(rec.data(), "UCSMAP02", 8);
  rec.resize(kMapHead2, 0);
  EXPECT_FALSE(decodeMapHead(rec.data(), rec.size(), h));
  // Too short for its own head.
  EXPECT_FALSE(decodeMapHead(junk.data(), kMapHead - 1, h));
}

namespace {
// A book of maps with no store and no maps of its own: only the in-use
// record speaks.
struct BareMaps : StoreMaps {
  bool storedEntry(uint32_t, SlotEntry&) const override { return false; }
  void forEachStored(const std::function<void(uint32_t, const SlotEntry&)>&) const override {}
  void applyOthers(const std::vector<SlotEntry>&) override {}
};
} // namespace

// A store made after another was removed lays out the same tables area. A map
// of the removed store handed out within the window keeps its place there:
// the new store's map goes elsewhere, and a read there is refused rather than
// answered with this store's bytes.
TEST(StoreMaps, AReplacedStoresMapKeepsItsPlaceWhileInUse) {
  test::TempDir td;
  RealIO io;
  BareMaps sm;
  sm.cacheDir = td.path();
  sm.key = *UrlKey::parse("root://h//data/replaced.root");
  sm.tables.first = 1000;
  sm.tables.end = 100000;
  sm.seekLimit = UINT64_MAX;
  sm.inUseSeconds = 86400;
  const uint64_t now = wallSeconds();
  InUseRecord rec;
  InUseRange r;
  r.storeId = 7; // not this store
  r.lo = 1000;
  r.hi = 3000;
  r.lastS = now;
  rec.ranges.push_back(r);
  uint64_t retry = 0;
  uint64_t at = 0;
  {
    std::lock_guard<std::mutex> g(sm.mu);
    at = sm.freePlaceLocked(2000, now, rec, retry);
  }
  EXPECT_NE(at, 0u);
  EXPECT_TRUE(at >= r.hi || at + 2000 <= r.lo) << at;
  // Out of use: the place is free again.
  rec.ranges[0].lastS = now - 2 * sm.inUseSeconds;
  {
    std::lock_guard<std::mutex> g(sm.mu);
    at = sm.freePlaceLocked(2000, now, rec, retry);
  }
  EXPECT_EQ(at, sm.tables.first);
  // Reads: inside the replaced store's map refused, before it bounded by it.
  InUseSettings s;
  ASSERT_TRUE(InUseRecord::note(io, InUseRecord::path(sm.cacheDir, sm.key.hashHex), r, &s,
                                sm.inUseSeconds));
  bool refused = false;
  uint64_t upto = 0;
  sm.tablesForeign(1500, refused, upto);
  EXPECT_TRUE(refused);
  sm.tablesForeign(500, refused, upto);
  EXPECT_FALSE(refused);
  EXPECT_EQ(upto, 1000u);
  sm.tablesForeign(5000, refused, upto);
  EXPECT_FALSE(refused);
  EXPECT_EQ(upto, UINT64_MAX);
}
