// The sweep of one file (StoreSweep.h): what the byte cache holds, converted
// into the file's slot store exactly as a first pass converts it, once.
#include "StoreSweep.h"

#include "CacheStore.h"
#include "FillLayout.h"
#include "InUse.h"
#include "RNTupleRewrite.h"
#include "ReadMap.h"
#include "SlotStore.h"
#include "StoreLayout.h"
#include "TestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
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
  void cache(uint64_t n) { cacheAllBut(n, 0, 0); }
  // Bytes [0, n) in the byte cache except the pages that touch [lo, hi).
  void cacheAllBut(uint64_t n, uint64_t lo, uint64_t hi) {
    CacheStore cs(io, cfg);
    auto e = cs.open(key, file.size());
    ASSERT_TRUE(e);
    const uint64_t pg = e->pageSize();
    if (lo >= hi) {
      e->writePages(0, n, file.data());
    } else {
      const uint64_t a = lo / pg * pg, b = std::min<uint64_t>(n, (hi + pg - 1) / pg * pg);
      e->writePages(0, a, file.data());
      e->writePages(b, n - b, file.data() + b);
    }
    e->flushMeta(true);
  }
  // A layout shown to readers within the window, as a first pass notes it.
  void noteShown(uint32_t layoutVersion) {
    InUseSettings s;
    s.layoutVersion = layoutVersion;
    s.slotFactor100 = static_cast<uint16_t>(kSlotFactor100);
    s.codecs = joinCodecs(cfg.recompressCodecs);
    InUseRange r;
    r.storeId = 1;
    r.lo = 1;
    r.hi = 2;
    r.lastS = static_cast<uint64_t>(std::time(nullptr));
    ASSERT_TRUE(InUseRecord::note(io, InUseRecord::path(cfg.cacheDir, key.hashHex), r, &s,
                                  cfg.inUseSeconds));
  }
  SweepResult sweep() {
    CacheStore cs(io, cfg);
    return sweepFile(cs, cfg, io, key);
  }
  std::shared_ptr<SlotStore> store() {
    return SlotStore::open(io, key.objectDir(cfg.cacheDir), key.hashHex);
  }
};

// The file in memory, noting where it was read.
struct MemSource : Source {
  const std::vector<uint8_t>& b;
  std::vector<uint64_t> at;
  explicit MemSource(const std::vector<uint8_t>& f) : b(f) {}
  bool has(uint64_t off, uint64_t n) override { return off + n >= off && off + n <= b.size(); }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    if (!has(off, n))
      return false;
    at.push_back(off);
    std::memcpy(dst, b.data() + off, n);
    return true;
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

// A file whose compression setting names no codec has the codec named by one
// basket every process asks for. A job that never read it leaves it out of the
// byte cache: with no layout shown to readers, any cached basket of those
// branches names the codec and the file is converted; with one shown within
// the window, the sweep asks the same basket the layout was decided by, and
// leaves the file for later rather than lay it out another way. (The same
// choice for an RNTuple file's pages: rntuple_rewrite_test.)
TEST(StoreSweep, AnUnnamedCodecIsNamedByACachedBasketUnlessALayoutWasShown) {
  const char* name = "unnamed_codec_wide_fixture.root";
  std::vector<uint8_t> file = slurp(name);
  ASSERT_FALSE(file.empty());
  MemSource m(file);
  FileMeta fm = parseReaderTree(m, static_cast<int64_t>(file.size()));
  ASSERT_TRUE(fm.error.empty()) << fm.error;
  const std::vector<uint8_t> header(file.begin(), file.begin() + 100);
  m.at.clear();
  std::string codec;
  ASSERT_TRUE(unnamedSettingCodec(fm, header, m, codec));
  ASSERT_EQ(codec, "zlib");
  ASSERT_EQ(m.at.size(), 1u);
  const uint64_t probe = m.at[0];
  ASSERT_GE(probe, 4096u); // past the file's first page

  Fixture f(name, {"lzma", "zlib"});
  f.cacheAllBut(f.file.size(), probe, probe + 1);
  SweepResult r = f.sweep();
  ASSERT_EQ(r.outcome, SweepResult::Outcome::kConverted) << r.note;
  EXPECT_GT(r.converted, 0u);
  EXPECT_GT(r.uncached, 0u); // the basket on the page left out
  auto s = f.store();
  ASSERT_TRUE(s);
  StoredLayout lay;
  entriesOf(*s, lay);
  EXPECT_EQ(lay.L.relocated.size(), 4u); // the unnamed branches with the named one

  Fixture g(name, {"lzma", "zlib"});
  g.cacheAllBut(g.file.size(), probe, probe + 1);
  g.noteShown(kTTreeLayoutVersion);
  SweepResult held = g.sweep();
  EXPECT_EQ(held.outcome, SweepResult::Outcome::kIncomplete) << held.note;
  EXPECT_FALSE(g.store());
  // Once that basket is cached, the same layout is made.
  g.cache(g.file.size());
  held = g.sweep();
  ASSERT_EQ(held.outcome, SweepResult::Outcome::kConverted) << held.note;
  ASSERT_TRUE(g.store());
  EXPECT_EQ(g.store()->header().layoutHash, s->header().layoutHash);
}
