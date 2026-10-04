// The sweep of one file (StoreSweep.h): what the byte cache holds, converted
// into the file's slot store exactly as a first pass converts it, once.
#include "StoreSweep.h"

#include "CacheStore.h"
#include "FillLayout.h"
#include "RNTupleRewrite.h"
#include "SlotStore.h"
#include "StoreLayout.h"
#include "TestUtil.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace ucache;
using namespace ucache::transpose;
using test::TempDir;

namespace {

std::vector<uint8_t> slurp(const std::string& name) {
  std::vector<uint8_t> out;
  FILE* f = std::fopen((std::string(UCACHE_TEST_DATA_DIR) + "/" + name).c_str(), "rb");
  if (!f)
    return out;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
    out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

struct Fixture {
  TempDir td;
  RealIO io;
  Config cfg;
  UrlKey key = *UrlKey::parse("root://h//data/sweep.root");
  std::vector<uint8_t> file;

  Fixture(const std::string& name, std::vector<std::string> codecs) {
    cfg.cacheDir = td.path();
    cfg.recompressCodecs = std::move(codecs);
    file = slurp(name);
  }
  // The file's first `n` bytes in the byte cache (whole pages; the last one
  // short at the end of the file).
  void cache(uint64_t n) {
    CacheStore cs(io, cfg);
    auto e = cs.open(key, file.size());
    ASSERT_TRUE(e);
    e->writePages(0, n, file.data());
    e->flushMeta(true);
  }
  SweepResult sweep() {
    CacheStore cs(io, cfg);
    return sweepFile(cs, cfg, io, key);
  }
  std::shared_ptr<SlotStore> store() {
    return SlotStore::open(io, key.objectDir(cfg.cacheDir), key.hashHex);
  }
};

// Every slot's committed entry, and the layout the store keeps.
std::vector<SlotEntry> entriesOf(SlotStore& s, StoredLayout& lay) {
  lay.slotFactor100 = s.header().slotFactor100;
  EXPECT_TRUE(decodeLayout(s.layoutBlob(), lay));
  std::vector<SlotEntry> out(lay.L.slots.size());
  for (const auto& e : s.refresh(true))
    if (e.kind != SlotEntry::kMap && e.slot < out.size())
      out[e.slot] = e;
  return out;
}

} // namespace

TEST(StoreSweep, ATTreeFileIsConvertedIntoItsStoreOnce) {
  Fixture f("unnamed_codec_fixture.root", {"lzma", "zlib"});
  ASSERT_FALSE(f.file.empty());
  f.cache(f.file.size());
  SweepResult r = f.sweep();
  ASSERT_EQ(r.outcome, SweepResult::Outcome::kConverted) << r.note;
  EXPECT_TRUE(r.storeCreated);
  EXPECT_GT(r.converted, 0u);
  EXPECT_EQ(r.uncached, 0u);
  EXPECT_EQ(r.unreadable + r.undecodable, 0u);
  auto s = f.store();
  ASSERT_TRUE(s);
  StoredLayout lay;
  auto es = entriesOf(*s, lay);
  EXPECT_EQ(r.converted + r.kept, lay.L.slots.size());
  // Each record is the basket converted as a first pass converts it, stating
  // its slot.
  for (size_t i = 0; i < es.size(); ++i) {
    const FillSlot& sl = lay.L.slots[i];
    ConvertedBasket c =
        convertBasket(f.file.data() + sl.origSeek, sl.origLen, sl.vLen, splitCodecs(s->header().codecs));
    if (es[i].kind == SlotEntry::kKept) {
      EXPECT_TRUE(c.kind == ConvertedBasket::kOriginal || !c.error.empty()) << i;
      continue;
    }
    ASSERT_TRUE(es[i].kind == SlotEntry::kZstd || es[i].kind == SlotEntry::kRaw) << i;
    ASSERT_TRUE(patchKeySeek(c.record.data(), c.record.size(), sl.vSeek));
    std::vector<uint8_t> rec;
    ASSERT_TRUE(s->readRecord(es[i], rec)) << i;
    EXPECT_EQ(rec, c.record) << i;
  }
  // Again: nothing left to convert, and nothing written.
  SweepResult again = f.sweep();
  EXPECT_EQ(again.outcome, SweepResult::Outcome::kAlready) << again.note;
  EXPECT_EQ(again.converted + again.kept, 0u);
  EXPECT_FALSE(again.storeCreated);
  EXPECT_EQ(again.storedBytes, 0u);
}

TEST(StoreSweep, AnRNTupleFileIsConvertedAndGivenACompactMap) {
  Fixture f("rntuple_fixture.root", {"lzma", "zlib", "zstd"});
  ASSERT_FALSE(f.file.empty());
  f.cache(f.file.size());
  SweepResult r = f.sweep();
  ASSERT_EQ(r.outcome, SweepResult::Outcome::kConverted) << r.note;
  EXPECT_EQ(r.kept, 0u); // an RNTuple page is always converted: decoded
  EXPECT_TRUE(r.mapMade);
  auto s = f.store();
  ASSERT_TRUE(s);
  StoredLayout lay;
  auto es = entriesOf(*s, lay);
  ASSERT_TRUE(lay.rnt);
  EXPECT_EQ(r.converted, lay.L.slots.size());
  for (size_t i = 0; i < es.size(); ++i) {
    const FillSlot& sl = lay.L.slots[i];
    ConvertedPage p = convertPage(f.file.data() + sl.origSeek, sl.origLen, lay.pages[i].nbytes,
                                  lay.pages[i].hasChecksum, sl.vLen - kSlotChecksumBytes);
    ASSERT_TRUE(p.error.empty()) << i;
    std::vector<uint8_t> rec;
    ASSERT_TRUE(s->readRecord(es[i], rec)) << i;
    EXPECT_EQ(rec, p.enc) << i;
  }
  // The map states what the store holds: the next sweep makes none.
  SweepResult again = f.sweep();
  EXPECT_EQ(again.outcome, SweepResult::Outcome::kAlready);
  EXPECT_FALSE(again.mapMade);
}

TEST(StoreSweep, ACodecTheSettingDoesNotNameIsDeclinedAndNamed) {
  Fixture f("rntuple_fixture.root", {"lzma", "zlib"}); // its pages are ZSTD
  f.cache(f.file.size());
  SweepResult r = f.sweep();
  EXPECT_EQ(r.outcome, SweepResult::Outcome::kDeclined);
  EXPECT_TRUE(r.codecDecline);
  EXPECT_EQ(r.declinedCodecs, "zstd");
  EXPECT_FALSE(f.store()); // nothing decided for good: a store is not made
}

TEST(StoreSweep, AFileWhoseMetadataIsNotCachedIsLeftForLater) {
  Fixture f("unnamed_codec_fixture.root", {"lzma", "zlib"});
  f.cache(4096); // the header alone
  SweepResult r = f.sweep();
  EXPECT_EQ(r.outcome, SweepResult::Outcome::kIncomplete) << r.note;
  EXPECT_FALSE(f.store());
}

TEST(StoreSweep, AnEarlierReplicaFormIsRemoved) {
  Fixture f("unnamed_codec_fixture.root", {"lzma", "zlib"});
  f.cache(f.file.size());
  const std::string base = f.key.objectDir(f.cfg.cacheDir) + "/" + f.key.hashHex;
  for (const char* suffix : {".tdata", ".tmeta", ".tok"})
    std::ofstream(base + suffix) << "x";
  SweepResult r = f.sweep();
  EXPECT_EQ(r.outcome, SweepResult::Outcome::kConverted) << r.note;
  EXPECT_TRUE(r.droppedEarlier);
  struct ::stat st;
  for (const char* suffix : {".tdata", ".tmeta", ".tok"})
    EXPECT_NE(::stat((base + suffix).c_str(), &st), 0) << suffix;
}

TEST(StoreSweep, ACachedOriginalThatRottedIsCaughtAndCounted) {
  // The layout first, to know where the baskets lie.
  Fixture ref("unnamed_codec_fixture.root", {"lzma", "zlib"});
  ref.cache(ref.file.size());
  ASSERT_EQ(ref.sweep().outcome, SweepResult::Outcome::kConverted);
  StoredLayout lay;
  auto s = ref.store();
  ASSERT_TRUE(s);
  entriesOf(*s, lay);
  // A byte flipped in a basket whose pages the layout does not need: that
  // basket is not converted, and the sweep says so (and why).
  bool seen = false;
  for (size_t k = lay.L.slots.size(); k-- > 0 && !seen;) {
    const FillSlot& sl = lay.L.slots[k];
    Fixture f("unnamed_codec_fixture.root", {"lzma", "zlib"});
    f.cache(f.file.size());
    {
      std::fstream d(f.key.dataPath(f.cfg.cacheDir), std::ios::in | std::ios::out | std::ios::binary);
      d.seekg(static_cast<std::streamoff>(sl.origSeek + sl.origLen / 2));
      char b = 0;
      d.read(&b, 1);
      b = static_cast<char>(b ^ 0x5A);
      d.seekp(static_cast<std::streamoff>(sl.origSeek + sl.origLen / 2));
      d.write(&b, 1);
    }
    SweepResult r = f.sweep();
    if (r.outcome != SweepResult::Outcome::kConverted)
      continue; // that page also holds what the layout reads
    seen = r.unreadable > 0;
    if (seen) {
      StoredLayout l2;
      auto s2 = f.store();
      ASSERT_TRUE(s2);
      auto es = entriesOf(*s2, l2);
      EXPECT_EQ(es[k].kind, 0) << "the rotted basket was converted"; // no entry
    }
  }
  EXPECT_TRUE(seen) << "no basket's rot was caught";
}
