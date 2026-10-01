// The cold-run layout: every basket of a relocatable branch gets a slot k times
// its stored length past fEND, and the metadata the reader sees points at the
// slots. These tests pin the geometry, every metadata patch, the header and
// keys-list rewrites, the declines, and how a basket is prepared for its slot.
// ROOT's own verdict on a materialized file is the integration check; these
// catch a wrong offset or width without it.
#include "FillLayout.h"
#include "Transposer.h"
#include "TreeMeta.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ucache::transpose;

namespace {

void bePut16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
void bePut32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
void bePut64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
}
uint64_t beGet64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v = (v << 8) | p[i];
  return v;
}
uint32_t beGet32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i)
    v = (v << 8) | p[i];
  return v;
}

std::vector<uint8_t> keyHeader(bool wide, const std::string& cls, const std::string& name,
                              int32_t nbytes, int32_t objlen, int64_t seekkey) {
  const size_t fixed = wide ? 34u : 26u;
  const size_t keylen = fixed + 1 + cls.size() + 1 + name.size() + 1;
  std::vector<uint8_t> k(keylen, 0);
  bePut32(k.data(), static_cast<uint32_t>(nbytes));
  bePut16(k.data() + 4, static_cast<uint16_t>(wide ? 1004 : 4));
  bePut32(k.data() + 6, static_cast<uint32_t>(objlen));
  bePut16(k.data() + 14, static_cast<uint16_t>(keylen));
  bePut16(k.data() + 16, 1);
  if (wide)
    bePut64(k.data() + 18, static_cast<uint64_t>(seekkey));
  else
    bePut32(k.data() + 18, static_cast<uint32_t>(seekkey));
  size_t q = fixed;
  k[q++] = static_cast<uint8_t>(cls.size());
  std::memcpy(k.data() + q, cls.data(), cls.size());
  q += cls.size();
  k[q++] = static_cast<uint8_t>(name.size());
  std::memcpy(k.data() + q, name.data(), name.size());
  return k;
}

// Offsets of the fields inside the crafted tree record.
constexpr uint64_t kTreeZipOff = 10, kAutoFlushOff = 20;
constexpr uint64_t kB0Seek = 100, kB0Bytes = 130, kB0Zip = 140;
constexpr uint64_t kB1Seek = 200, kB1Bytes = 230, kB1Zip = 240;
constexpr uint64_t kB2Seek = 300, kB2Bytes = 330, kB2Zip = 340;
constexpr int64_t kTreeKeySeek = 50000, kKeysListSeek = 60000;

struct Fx {
  FileMeta fm;
  std::vector<uint8_t> header, treeKeyHeader, keysList;
  uint64_t fileSize = 0;
  size_t entryOff = 0; // the tree entry inside keysList
};

// Three branches: b0 LZMA (two baskets), b1 ZSTD, b2 inheriting the file's
// setting. `fend` sets the file size, `largeHeader` the header layout,
// `wideEntry` the width of the keys-list entry for the tree.
Fx fixture(int64_t fend = 70000, bool largeHeader = false, bool wideEntry = true,
           int32_t fileCompress = 207, int64_t autoFlush = -30000000) {
  Fx fx;
  FileMeta& fm = fx.fm;
  fm.fend = fend;
  fm.large = largeHeader;
  fm.keyslistSeek = kKeysListSeek;
  fm.treeBlob.assign(400, 0);
  fm.zipBytes = 10000;
  fm.zipBytesOff = kTreeZipOff;
  fm.autoFlush = autoFlush;
  fm.autoFlushOff = kAutoFlushOff;
  auto branch = [&](const char* name, int32_t compress, std::vector<int64_t> seeks,
                    std::vector<int32_t> bytes, uint64_t so, uint64_t bo, uint64_t zo) {
    BranchInfo b;
    b.name = name;
    b.compress = compress;
    b.writeBasket = static_cast<int32_t>(seeks.size());
    b.maxBaskets = static_cast<uint32_t>(seeks.size());
    b.basketSeek = seeks;
    b.basketBytes = bytes;
    b.seekArrayOff = so;
    b.bytesArrayOff = bo;
    b.zipBytesOff = zo;
    for (int32_t x : bytes) b.zipBytes += x;
    fm.branches.push_back(b);
  };
  branch("Muon_pt", 207, {1000, 3000}, {1500, 2500}, kB0Seek, kB0Bytes, kB0Zip);
  branch("Muon_eta", 505, {6000}, {800}, kB1Seek, kB1Bytes, kB1Zip);
  branch("Muon_phi", -1, {7000}, {900}, kB2Seek, kB2Bytes, kB2Zip);
  // The record carries the totals, as a real one does.
  bePut64(&fm.treeBlob[kTreeZipOff], static_cast<uint64_t>(fm.zipBytes));
  bePut64(&fm.treeBlob[kAutoFlushOff], static_cast<uint64_t>(autoFlush));
  for (const auto& b : fm.branches)
    bePut64(&fm.treeBlob[b.zipBytesOff], static_cast<uint64_t>(b.zipBytes));

  fx.header.assign(100, 0);
  std::memcpy(fx.header.data(), "root", 4);
  bePut32(&fx.header[8], 100);
  if (largeHeader) {
    bePut32(&fx.header[4], 1062604);
    bePut64(&fx.header[12], static_cast<uint64_t>(fend));
    bePut64(&fx.header[20], 1234);          // fSeekFree
    bePut32(&fx.header[28], 56);            // fNbytesFree
    bePut32(&fx.header[41], static_cast<uint32_t>(fileCompress));
    bePut64(&fx.header[45], 4321);          // fSeekInfo
  } else {
    bePut32(&fx.header[4], 62604);
    bePut32(&fx.header[12], static_cast<uint32_t>(fend));
    bePut32(&fx.header[16], 1234);          // fSeekFree
    bePut32(&fx.header[20], 56);            // fNbytesFree
    bePut32(&fx.header[24], 1);             // nfree
    bePut32(&fx.header[28], 77);            // fNbytesName
    fx.header[32] = 4;                      // fUnits
    bePut32(&fx.header[33], static_cast<uint32_t>(fileCompress));
    bePut32(&fx.header[37], 4321);          // fSeekInfo
    bePut32(&fx.header[41], 99);            // fNbytesInfo
    for (int i = 0; i < 18; ++i) fx.header[45 + i] = static_cast<uint8_t>(0xC0 + i); // UUID
  }

  fx.treeKeyHeader = keyHeader(true, "TTree", "Events", 2000, 400, kTreeKeySeek);
  fm.treeKey.nbytes = 2000;
  fm.treeKey.objlen = 400;
  fm.treeKey.keylen = static_cast<uint16_t>(fx.treeKeyHeader.size());
  fm.treeKey.ver = 1004;
  fm.treeKey.cycle = 1;
  fm.treeKey.seekkey = kTreeKeySeek;
  fm.treeKey.cls = "TTree";
  fm.treeKey.name = "Events";

  auto own = keyHeader(false, "TFile", "f.root", 0, 0, kKeysListSeek);
  auto other = keyHeader(false, "TNamed", "note", 50, 20, 900);
  auto entry = keyHeader(wideEntry, "TTree", "Events", 2000, 400, kTreeKeySeek);
  fx.keysList = own;
  fx.keysList.resize(own.size() + 4);
  bePut32(fx.keysList.data() + own.size(), 2);
  fx.keysList.insert(fx.keysList.end(), other.begin(), other.end());
  fx.entryOff = fx.keysList.size();
  fx.keysList.insert(fx.keysList.end(), entry.begin(), entry.end());
  bePut32(fx.keysList.data(), static_cast<uint32_t>(fx.keysList.size()));
  fx.fileSize = static_cast<uint64_t>(fend);
  return fx;
}

FillLayout layout(const Fx& fx, std::vector<std::string> codecs = {"lzma", "zlib"},
                  uint32_t k100 = 400) {
  return layoutForFill(fx.fm, fx.fileSize, fx.header, fx.treeKeyHeader, fx.keysList, codecs, k100);
}

std::vector<uint8_t> metaBlob(const FillLayout& L, const Fx& fx) {
  const uint8_t* pay = L.metaRecord.data() + fx.fm.treeKey.keylen;
  const size_t n = L.metaRecord.size() - fx.fm.treeKey.keylen;
  if (n == fx.fm.treeBlob.size())
    return std::vector<uint8_t>(pay, pay + n); // stored raw
  return decompressFrames(pay, n, fx.fm.treeBlob.size());
}

// A basket record: key header + payload. `payload` is written as given.
std::vector<uint8_t> basketRecord(int32_t objlen, const std::vector<uint8_t>& payload, int64_t seek = 1000) {
  auto k = keyHeader(true, "TBasket", "Muon_pt", 0, objlen, seek);
  std::vector<uint8_t> r = k;
  r.insert(r.end(), payload.begin(), payload.end());
  bePut32(r.data(), static_cast<uint32_t>(r.size()));
  return r;
}

// Two bits of entropy per byte: every ZSTD level lands near 4:1, like physics
// data does between codecs.
std::vector<uint8_t> compressible(size_t n) {
  std::vector<uint8_t> v(n);
  uint64_t x = 0x2545F4914F6CDD1Dull;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    v[i] = static_cast<uint8_t>(x & 3);
  }
  return v;
}
// A short period: ZSTD-19 finds it and ZSTD-1 mostly does not.
std::vector<uint8_t> periodic(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i / 7) % 13);
  return v;
}
std::vector<uint8_t> incompressible(size_t n) {
  std::vector<uint8_t> v(n);
  uint64_t x = 0x9E3779B97F4A7C15ull;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    v[i] = static_cast<uint8_t>(x);
  }
  return v;
}

} // namespace

TEST(FillLayout, CodecOfSetting) {
  EXPECT_EQ(codecOfSetting(207, 0), "lzma");
  EXPECT_EQ(codecOfSetting(101, 0), "zlib");
  EXPECT_EQ(codecOfSetting(404, 0), "lz4");
  EXPECT_EQ(codecOfSetting(505, 0), "zstd");
  EXPECT_EQ(codecOfSetting(-1, 209), "lzma"); // inherits the file's
  EXPECT_EQ(codecOfSetting(-1, 0), "");
  EXPECT_EQ(codecOfSetting(200, 0), "");      // level 0 = stored
  EXPECT_EQ(codecOfSetting(0, 207), "");      // explicit 0 does NOT inherit
  EXPECT_EQ(codecOfSetting(301, 0), "");      // old ROOT algorithm: not transcoded
  EXPECT_EQ(codecOfSetting(1, 0), "");        // algorithm 0 = global default, unknown here
}

TEST(FillLayout, SlotsAreContiguousAndSized) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  // b0 (lzma) and b2 (inherits 207); b1 is zstd, not listed.
  ASSERT_EQ(L.relocated, (std::vector<uint32_t>{0, 2}));
  EXPECT_EQ(L.originSize, 70000u);
  EXPECT_EQ(L.metaSeek, 73728u); // fEND rounded up to 4 KiB
  EXPECT_EQ(L.slotsBegin, L.metaSeek + fx.fm.treeKey.keylen + fx.fm.treeBlob.size());
  ASSERT_EQ(L.slots.size(), 3u);
  uint64_t at = L.slotsBegin;
  const uint32_t orig[] = {1500, 2500, 900};
  const uint64_t oseek[] = {1000, 3000, 7000};
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(L.slots[i].vSeek, at);
    EXPECT_EQ(L.slots[i].origLen, orig[i]);
    EXPECT_EQ(L.slots[i].origSeek, oseek[i]);
    EXPECT_EQ(L.slots[i].vLen, 4 * orig[i]);
    at += L.slots[i].vLen;
  }
  EXPECT_EQ(L.slots[2].branch, 2u);
  EXPECT_EQ(L.slots[1].basket, 1u);
  EXPECT_EQ(L.virtualSize, at);
  EXPECT_GE(L.metaSeek + L.metaRecord.size(), L.metaSeek);
  EXPECT_LE(L.metaSeek + L.metaRecord.size(), L.slotsBegin);
}

// The original bytes a read of the layout carried, for counts that must be
// right or absent. A reader reads a slot whole (fBasketBytes is the slot) and
// the relocated tree key whole (its fNbytes): each is its whole original
// record. A read into part of a slot is not a count of anything in the
// original file; the padding between the pieces is nothing.
TEST(FillLayout, ExactOriginRangesTakeWholePiecesOnly) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  const std::vector<std::pair<uint64_t, uint64_t>> metaOrigin{
      {static_cast<uint64_t>(kTreeKeySeek), 2000}};
  std::vector<std::pair<uint64_t, uint64_t>> out;
  // What the reader reads, whole: header, keys list, the tree key, each slot.
  ASSERT_TRUE(exactOriginRanges(L, metaOrigin, 0, 100, out));
  ASSERT_TRUE(exactOriginRanges(L, metaOrigin, kKeysListSeek, fx.keysList.size(), out));
  ASSERT_TRUE(exactOriginRanges(L, metaOrigin, L.metaSeek, L.metaRecord.size(), out));
  for (const auto& s : L.slots)
    ASSERT_TRUE(exactOriginRanges(L, metaOrigin, s.vSeek, s.vLen, out));
  const std::vector<std::pair<uint64_t, uint64_t>> want{{0, 100},
                                                        {kKeysListSeek, fx.keysList.size()},
                                                        {kTreeKeySeek, 2000},
                                                        {1000, 1500},
                                                        {3000, 2500},
                                                        {7000, 900}};
  EXPECT_EQ(out, want);

  // One read over the padding after the tree key and two whole slots.
  out.clear();
  const uint64_t from = L.metaSeek + L.metaRecord.size();
  ASSERT_TRUE(
      exactOriginRanges(L, metaOrigin, from, L.slots[1].vSeek + L.slots[1].vLen - from, out));
  EXPECT_EQ(out, (std::vector<std::pair<uint64_t, uint64_t>>{{1000, 1500}, {3000, 2500}}));
  // Past the original end and before the tree key: alignment, nothing.
  out.clear();
  ASSERT_TRUE(exactOriginRanges(L, metaOrigin, L.originSize, L.metaSeek - L.originSize, out));
  EXPECT_TRUE(out.empty());

  // Parts: the record in a slot without its padding, a slot's tail, the tree
  // key's header alone, a read that ends inside the next slot.
  const auto& s0 = L.slots[0];
  out.clear();
  EXPECT_FALSE(exactOriginRanges(L, metaOrigin, s0.vSeek, s0.origLen, out));
  out.clear();
  EXPECT_FALSE(exactOriginRanges(L, metaOrigin, s0.vSeek + 1, s0.vLen - 1, out));
  out.clear();
  EXPECT_FALSE(exactOriginRanges(L, metaOrigin, L.metaSeek, fx.fm.treeKey.keylen, out));
  out.clear();
  EXPECT_FALSE(exactOriginRanges(L, metaOrigin, s0.vSeek, s0.vLen + 1, out));
  // Nothing to read, and a request no layout holds, carry nothing.
  out.clear();
  EXPECT_TRUE(exactOriginRanges(L, metaOrigin, s0.vSeek, 0, out));
  EXPECT_TRUE(exactOriginRanges(L, metaOrigin, ~uint64_t(0) - 10, 100, out));
  EXPECT_TRUE(out.empty());
  // Past the last slot.
  EXPECT_TRUE(exactOriginRanges(L, metaOrigin, L.virtualSize, 4096, out));
  EXPECT_TRUE(out.empty());
}

// The factor is in hundredths: 2.5x, and a factor whose slots must be rounded
// down to a byte (the plugin fixes it at 3; the layout takes any). The tree's
// totals are left as the file states them, whatever the factor.
TEST(FillLayout, FractionalSlotFactors) {
  Fx fx = fixture();
  const uint32_t orig[] = {1500, 2500, 900};
  for (uint32_t k100 : {250u, 233u}) {
    FillLayout L = layout(fx, {"lzma", "zlib"}, k100);
    ASSERT_TRUE(L.error.empty()) << L.error;
    ASSERT_EQ(L.slots.size(), 3u);
    uint64_t at = L.slotsBegin;
    for (size_t i = 0; i < 3; ++i) {
      const uint32_t want = orig[i] * k100 / 100; // 3750 6250 2250; 3495 5825 2097
      EXPECT_EQ(L.slots[i].vLen, want) << k100;
      EXPECT_EQ(L.slots[i].vSeek, at);
      at += L.slots[i].vLen;
    }
    EXPECT_EQ(L.virtualSize, at);
    auto blob = metaBlob(L, fx);
    for (uint64_t off : {kB0Zip, kB2Zip, kTreeZipOff, kAutoFlushOff})
      EXPECT_EQ(beGet64(&blob[off]), beGet64(&fx.fm.treeBlob[off])) << "offset " << off;
  }
  EXPECT_EQ(layout(fx, {"lzma", "zlib"}, 100).slots[0].vLen, 1500u) << "1x: the original fits";
  EXPECT_NE(layout(fx, {"lzma", "zlib"}, 99).error.find("at least 1"), std::string::npos)
      << "below 1x the original would not fit its own slot";
}

TEST(FillLayout, MetadataPointsAtSlotsAndKeepsTheRealTotal) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  auto blob = metaBlob(L, fx);
  ASSERT_EQ(blob.size(), fx.fm.treeBlob.size());
  EXPECT_EQ(beGet64(&blob[kB0Seek]), L.slots[0].vSeek);
  EXPECT_EQ(beGet64(&blob[kB0Seek + 8]), L.slots[1].vSeek);
  EXPECT_EQ(beGet32(&blob[kB0Bytes]), L.slots[0].vLen);
  EXPECT_EQ(beGet32(&blob[kB0Bytes + 4]), L.slots[1].vLen);
  EXPECT_EQ(beGet64(&blob[kB2Seek]), L.slots[2].vSeek);
  // The unlisted branch is untouched.
  EXPECT_EQ(beGet64(&blob[kB1Seek]), 0u);
  EXPECT_EQ(beGet64(&blob[kB1Zip]), 800u);
  // fZipBytes (per branch, for the tree) and fAutoFlush stay what the file
  // says: ROOT's read cache keeps the size it has for the original file,
  // however padded the slots are.
  for (uint64_t off : {kB0Zip, kB2Zip, kTreeZipOff, kAutoFlushOff})
    EXPECT_EQ(beGet64(&blob[off]), beGet64(&fx.fm.treeBlob[off])) << "offset " << off;
}

TEST(FillLayout, AutoFlushByEntriesIsLeftAlone) {
  Fx fx = fixture(70000, false, true, 207, 1000);
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  auto blob = metaBlob(L, fx);
  EXPECT_EQ(beGet64(&blob[kAutoFlushOff]), 1000u); // never written
}

TEST(FillLayout, MetadataRecordKeepsItsKeyHeaderLength) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  const size_t kl = fx.fm.treeKey.keylen;
  ASSERT_GT(L.metaRecord.size(), kl);
  EXPECT_EQ(beGet32(L.metaRecord.data()), L.metaRecord.size());           // fNbytes
  EXPECT_EQ(beGet64(L.metaRecord.data() + 18), L.metaSeek);               // fSeekKey
  EXPECT_EQ(beGet32(L.metaRecord.data() + 6), fx.fm.treeBlob.size());     // fObjlen unchanged
  EXPECT_EQ((L.metaRecord[14] << 8) | L.metaRecord[15], static_cast<int>(kl)); // fKeylen unchanged
  // Everything else in the key header is the original's.
  EXPECT_EQ(0, std::memcmp(L.metaRecord.data() + 4, fx.treeKeyHeader.data() + 4, 14));
  EXPECT_EQ(0, std::memcmp(L.metaRecord.data() + 26, fx.treeKeyHeader.data() + 26, kl - 26));
}

TEST(FillLayout, KeysListEntryPointsAtTheMetadataKey) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  ASSERT_EQ(L.windows.size(), 2u);
  const auto& w = L.windows[1];
  EXPECT_EQ(w.off, static_cast<uint64_t>(kKeysListSeek));
  ASSERT_EQ(w.bytes.size(), fx.keysList.size()); // patched in place: same length
  EXPECT_EQ(beGet32(&w.bytes[fx.entryOff]), L.metaRecord.size());
  EXPECT_EQ(beGet64(&w.bytes[fx.entryOff + 18]), L.metaSeek);
  // Nothing outside the live entry's two fields changed.
  std::vector<uint8_t> a = fx.keysList, b = w.bytes;
  std::memset(&a[fx.entryOff], 0, 4);
  std::memset(&b[fx.entryOff], 0, 4);
  std::memset(&a[fx.entryOff + 18], 0, 8);
  std::memset(&b[fx.entryOff + 18], 0, 8);
  EXPECT_EQ(a, b);
}

TEST(FillLayout, NarrowKeysListEntryBelow2GiB) {
  Fx fx = fixture(70000, false, /*wideEntry=*/false);
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  EXPECT_EQ(beGet32(&L.windows[1].bytes[fx.entryOff + 18]), L.metaSeek);
}

TEST(FillLayout, SmallHeaderKeepsItsWidthBelow2GiB) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  EXPECT_EQ(L.windows[0].off, 12u);
  ASSERT_EQ(L.windows[0].bytes.size(), 4u);
  EXPECT_EQ(beGet32(L.windows[0].bytes.data()), L.virtualSize);
}

TEST(FillLayout, LargeHeaderWritesFEndAt64Bits) {
  Fx fx = fixture(70000, /*largeHeader=*/true);
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  EXPECT_EQ(L.windows[0].off, 12u);
  ASSERT_EQ(L.windows[0].bytes.size(), 8u);
  EXPECT_EQ(beGet64(L.windows[0].bytes.data()), L.virtualSize);
}

TEST(FillLayout, SmallHeaderIsPromotedPast2GiB) {
  // A 600 MB basket padded 4x ends past 2^31: the 32-bit header cannot hold fEND.
  Fx fx = fixture();
  fx.fm.branches[0].basketBytes = {1500, 600000000};
  fx.fm.fend = fx.fileSize = 700000000;
  bePut32(&fx.header[12], 700000000);
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  ASSERT_GE(L.virtualSize, 1ull << 31);
  const auto& w = L.windows[0].bytes;
  ASSERT_EQ(L.windows[0].off, 0u);
  ASSERT_EQ(w.size(), 75u);
  EXPECT_EQ(0, std::memcmp(w.data(), "root", 4));
  EXPECT_EQ(beGet32(&w[4]), 1062604u);
  EXPECT_EQ(beGet32(&w[8]), 100u);
  EXPECT_EQ(beGet64(&w[12]), L.virtualSize);
  EXPECT_EQ(beGet64(&w[20]), 1234u);  // fSeekFree
  EXPECT_EQ(beGet32(&w[28]), 56u);    // fNbytesFree
  EXPECT_EQ(beGet32(&w[32]), 1u);     // nfree
  EXPECT_EQ(beGet32(&w[36]), 77u);    // fNbytesName
  EXPECT_EQ(w[40], 8);                // fUnits
  EXPECT_EQ(beGet32(&w[41]), 207u);   // fCompress
  EXPECT_EQ(beGet64(&w[45]), 4321u);  // fSeekInfo
  EXPECT_EQ(beGet32(&w[53]), 99u);    // fNbytesInfo
  EXPECT_EQ(0, std::memcmp(&w[57], &fx.header[45], 18)); // UUID
}

TEST(FillLayout, SlotLengthIsCappedAtInt32Max) {
  Fx fx = fixture();
  fx.fm.branches[0].basketBytes = {1500, 600000000};
  fx.fm.fend = fx.fileSize = 700000000;
  bePut32(&fx.header[12], 700000000);
  FillLayout L = layout(fx, {"lzma"}, 400);
  ASSERT_TRUE(L.error.empty()) << L.error;
  EXPECT_EQ(L.slots[1].vLen, static_cast<uint32_t>(INT32_MAX));
}

TEST(FillLayout, Declines) {
  {
    Fx fx = fixture();
    fx.fileSize += 1; // bytes past fEND
    EXPECT_NE(layout(fx).error.find("differs from fEND"), std::string::npos);
  }
  {
    Fx fx = fixture();
    FillLayout L = layout(fx, {"lz4"});
    EXPECT_NE(L.error.find("no relocatable branch: its baskets are lzma, zstd"), std::string::npos)
        << L.error;
    EXPECT_TRUE(L.codecDecline);
  }
  {
    Fx fx = fixture(); // a branch whose baskets are in another file is left alone
    fx.fm.branches[0].externalFile = true;
    FillLayout L = layout(fx);
    ASSERT_TRUE(L.error.empty());
    EXPECT_EQ(L.relocated, (std::vector<uint32_t>{2}));
  }
  {
    Fx fx = fixture(); // a basket embedded in the tree record (seek 0)
    fx.fm.branches[0].basketSeek[1] = 0;
    EXPECT_EQ(layout(fx).relocated, (std::vector<uint32_t>{2}));
  }
  {
    Fx fx = fixture(); // a basket past EOF
    fx.fm.branches[2].basketSeek[0] = 69500;
    EXPECT_EQ(layout(fx).relocated, (std::vector<uint32_t>{0}));
  }
  {
    Fx fx = fixture(); // header and parse disagree
    bePut32(&fx.header[12], 12345);
    EXPECT_FALSE(layout(fx).error.empty());
  }
  {
    Fx fx = fixture(); // the keys list has no entry for the live tree
    bePut64(&fx.keysList[fx.entryOff + 18], 1);
    EXPECT_NE(layout(fx).error.find("live tree entry"), std::string::npos);
  }
  {
    // A 32-bit tree key cannot address a metadata key past 2 GiB.
    Fx big = fixture(2200000000ll, true);
    big.treeKeyHeader = keyHeader(false, "TTree", "Events", 2000, 400, kTreeKeySeek);
    big.fm.treeKey.keylen = static_cast<uint16_t>(big.treeKeyHeader.size());
    big.fm.treeKey.ver = 4;
    FillLayout L = layout(big);
    EXPECT_NE(L.error.find("32-bit"), std::string::npos) << L.error;
  }
  {
    // A 32-bit keys-list entry, same.
    Fx big = fixture(2200000000ll, true, /*wideEntry=*/false);
    FillLayout L = layout(big);
    EXPECT_NE(L.error.find("keys-list entry is 32-bit"), std::string::npos) << L.error;
  }
  {
    Fx fx = fixture(); // declined layouts carry nothing
    fx.fileSize += 1;
    FillLayout L = layout(fx);
    EXPECT_TRUE(L.slots.empty());
    EXPECT_TRUE(L.windows.empty());
    EXPECT_TRUE(L.metaRecord.empty());
  }
}

TEST(FillLayout, ConvertsToZstdWhenItFits) {
  auto raw = compressible(20000);
  auto zs = encodeZstdFrames(raw.data(), raw.size(), 19);
  auto rec = basketRecord(static_cast<int32_t>(raw.size()), zs);
  ConvertedBasket c = convertBasket(rec.data(), rec.size(), 4 * rec.size(), {"zstd"});
  ASSERT_TRUE(c.error.empty()) << c.error;
  EXPECT_EQ(c.kind, ConvertedBasket::kZstd);
  EXPECT_EQ(c.codec, "zstd");
  const size_t kl = (rec[14] << 8) | rec[15];
  EXPECT_EQ(beGet32(c.record.data()), c.record.size());
  EXPECT_EQ(0, std::memcmp(c.record.data() + 4, rec.data() + 4, kl - 4)); // header otherwise the original's
  auto back = decompressFrames(c.record.data() + kl, c.record.size() - kl, raw.size());
  EXPECT_EQ(back, raw);
  EXPECT_EQ(c.record[kl], 'Z');
  EXPECT_EQ(c.record[kl + 1], 'S');
}

TEST(FillLayout, IncompressibleBasketIsServedRaw) {
  // ZSTD cannot shrink it; uncompressed fits the slot and saves the reader a decode.
  auto raw = incompressible(4000);
  // The source is "compressed" in name only: frame header claims zstd, payload
  // is zstd of random bytes (larger than raw), which ROOT's writer would have
  // stored raw; construct it anyway to reach the branch.
  auto zs = encodeZstdFrames(raw.data(), raw.size(), 1);
  auto rec = basketRecord(static_cast<int32_t>(raw.size()), zs);
  ConvertedBasket c = convertBasket(rec.data(), rec.size(), 4 * rec.size(), {"zstd"});
  ASSERT_TRUE(c.error.empty()) << c.error;
  EXPECT_EQ(c.kind, ConvertedBasket::kRaw);
  const size_t kl = (rec[14] << 8) | rec[15];
  EXPECT_EQ(c.record.size(), kl + raw.size()); // fObjlen == fNbytes - fKeylen: raw to ROOT
  EXPECT_EQ(0, std::memcmp(c.record.data() + kl, raw.data(), raw.size()));
}

TEST(FillLayout, KeepsTheOriginalWhenNothingElseFits) {
  auto raw = periodic(200000);
  auto zs = encodeZstdFrames(raw.data(), raw.size(), 19);
  auto rec = basketRecord(static_cast<int32_t>(raw.size()), zs);
  // A slot exactly the original's length: the level-1 re-encode is larger.
  auto l1 = encodeZstdFrames(raw.data(), raw.size(), 1);
  ASSERT_GT(l1.size(), zs.size());
  ConvertedBasket c = convertBasket(rec.data(), rec.size(), static_cast<uint32_t>(rec.size()), {"zstd"});
  EXPECT_EQ(c.kind, ConvertedBasket::kOriginal);
  EXPECT_EQ(c.record, rec);
}

TEST(FillLayout, OriginalForUnlistedStoredOrUndecodable) {
  auto raw = compressible(5000);
  auto zs = encodeZstdFrames(raw.data(), raw.size(), 3);
  auto rec = basketRecord(static_cast<int32_t>(raw.size()), zs);
  {
    ConvertedBasket c = convertBasket(rec.data(), rec.size(), 4 * rec.size(), {"lzma"});
    EXPECT_EQ(c.kind, ConvertedBasket::kOriginal);
    EXPECT_EQ(c.codec, "zstd");
    EXPECT_EQ(c.record, rec);
  }
  {
    auto stored = basketRecord(static_cast<int32_t>(raw.size()), raw); // fObjlen == payload
    ConvertedBasket c = convertBasket(stored.data(), stored.size(), 4 * stored.size(), {"zstd"});
    EXPECT_EQ(c.kind, ConvertedBasket::kOriginal);
    EXPECT_EQ(c.record, stored);
  }
  {
    auto bad = rec;
    const size_t kl = (bad[14] << 8) | bad[15];
    for (size_t i = kl + 9; i < bad.size(); ++i) bad[i] ^= 0x5A; // frame body garbage
    ConvertedBasket c = convertBasket(bad.data(), bad.size(), 4 * bad.size(), {"zstd"});
    EXPECT_TRUE(c.error.empty());
    EXPECT_EQ(c.kind, ConvertedBasket::kOriginal);
    EXPECT_EQ(c.record, bad); // the reader gets exactly the origin's bytes
  }
  {
    auto notBasket = rec;
    notBasket[35] = 'X'; // class name "TBasket" -> "XBasket"
    EXPECT_FALSE(convertBasket(notBasket.data(), notBasket.size(), 4 * rec.size(), {"zstd"}).error.empty());
    EXPECT_FALSE(convertBasket(rec.data(), rec.size(), static_cast<uint32_t>(rec.size() - 1), {"zstd"})
                     .error.empty());
  }
}

TEST(FillLayout, PlaceInSlotPatchesSeekAndZeroFills) {
  auto raw = compressible(3000);
  auto zs = encodeZstdFrames(raw.data(), raw.size(), 19);
  auto rec = basketRecord(static_cast<int32_t>(raw.size()), zs);
  ConvertedBasket c = convertBasket(rec.data(), rec.size(), 4 * rec.size(), {"zstd"});
  ASSERT_EQ(c.kind, ConvertedBasket::kZstd);
  FillSlot s;
  s.vSeek = 5000000000ull; // past 4 GiB: a 64-bit basket key reaches it
  s.vLen = static_cast<uint32_t>(4 * rec.size());
  std::vector<uint8_t> out(s.vLen, 0xAA);
  std::string err;
  ASSERT_TRUE(placeInSlot(c, s, out.data(), err)) << err;
  EXPECT_EQ(beGet64(&out[18]), s.vSeek);
  EXPECT_EQ(0, std::memcmp(out.data(), c.record.data(), 18));
  EXPECT_EQ(0, std::memcmp(out.data() + 26, c.record.data() + 26, c.record.size() - 26));
  for (size_t i = c.record.size(); i < out.size(); ++i)
    ASSERT_EQ(out[i], 0) << "at " << i;
  // The stored record is position-independent: placing it elsewhere changes only fSeekKey.
  FillSlot s2 = s;
  s2.vSeek = 12345;
  std::vector<uint8_t> out2(s2.vLen, 0xAA);
  ASSERT_TRUE(placeInSlot(c, s2, out2.data(), err));
  EXPECT_EQ(beGet64(&out2[18]), 12345u);
}

TEST(FillLayout, PlaceInSlotRefuses) {
  auto raw = compressible(3000);
  auto rec = basketRecord(static_cast<int32_t>(raw.size()), raw);
  ConvertedBasket c = convertBasket(rec.data(), rec.size(), 4 * rec.size(), {"zstd"});
  std::string err;
  FillSlot s;
  s.vSeek = 100;
  s.vLen = static_cast<uint32_t>(rec.size() - 1);
  std::vector<uint8_t> out(rec.size(), 0);
  EXPECT_FALSE(placeInSlot(c, s, out.data(), err));
  // A 32-bit basket key cannot address a slot past 2 GiB.
  auto narrow = keyHeader(false, "TBasket", "Muon_pt", 0, static_cast<int32_t>(raw.size()), 1000);
  ConvertedBasket n;
  n.record = narrow;
  n.record.insert(n.record.end(), raw.begin(), raw.end());
  bePut32(n.record.data(), static_cast<uint32_t>(n.record.size()));
  s.vLen = static_cast<uint32_t>(n.record.size());
  s.vSeek = 3000000000ull;
  out.assign(s.vLen, 0);
  EXPECT_FALSE(placeInSlot(n, s, out.data(), err));
  s.vSeek = 2000;
  EXPECT_TRUE(placeInSlot(n, s, out.data(), err)) << err;
  EXPECT_EQ(beGet32(&out[18]), 2000u);
}

// ---------------------------------------------------------------------------
// A compression setting that names no codec. Files written by older ROOT
// versions (and `hadd -f1`) record setting 1 on the file and every branch:
// algorithm 0, "the global default", level 1 -- and the baskets are ZLIB. The
// committed fixture has exactly that property, and its last basket in each
// branch was fast-copied from an LZMA tree, so a branch also holds a basket in
// another codec than the one the layout takes for it.
// tests/data/make_unnamed_codec_fixture.C regenerates it.
namespace {

std::string unnamedFixture() {
  return std::string(UCACHE_TEST_DATA_DIR) + "/unnamed_codec_fixture.root";
}

std::vector<uint8_t> slurpFile(const std::string& path) {
  std::vector<uint8_t> out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f)
    return out;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
    out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

// A file held in memory, counting the reads made through it.
struct CountingSource : Source {
  std::vector<uint8_t> b;
  int reads = 0;
  bool failReads = false;
  bool has(uint64_t off, uint64_t n) override { return off + n >= off && off + n <= b.size(); }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    ++reads;
    if (failReads || off + n > b.size())
      return false;
    std::memcpy(dst, b.data() + off, n);
    return true;
  }
};

struct UnnamedFx {
  FileMeta fm;
  std::vector<uint8_t> file, header, treeKeyHeader, keysList;
};

UnnamedFx unnamedFx() {
  UnnamedFx u;
  u.fm = parseFile(unnamedFixture(), "Events");
  u.file = slurpFile(unnamedFixture());
  if (!u.fm.error.empty() || u.file.size() < 100)
    return u;
  u.header.assign(u.file.begin(), u.file.begin() + 100);
  const uint64_t tk = static_cast<uint64_t>(u.fm.treeKey.seekkey);
  u.treeKeyHeader.assign(u.file.begin() + tk, u.file.begin() + tk + u.fm.treeKey.keylen);
  const uint64_t kl = static_cast<uint64_t>(u.fm.keyslistSeek);
  const uint32_t n = beGet32(u.file.data() + kl);
  u.keysList.assign(u.file.begin() + kl, u.file.begin() + kl + n);
  return u;
}

FillLayout unnamedLayout(const UnnamedFx& u, std::vector<std::string> codecs,
                         const std::string& unnamed) {
  return layoutForFill(u.fm, u.file.size(), u.header, u.treeKeyHeader, u.keysList, codecs, 300,
                       unnamed);
}

// The payload of the basket record at `seek`, decoded by its own blocks.
std::vector<uint8_t> decodedBasket(const uint8_t* rec, size_t n) {
  auto k = parseKey(rec, n, 0);
  if (!k || k->keylen > n)
    return {};
  if (k->objlen == k->nbytes - static_cast<int32_t>(k->keylen))
    return std::vector<uint8_t>(rec + k->keylen, rec + n);
  return decompressFrames(rec + k->keylen, n - k->keylen, static_cast<uint64_t>(k->objlen));
}

} // namespace

TEST(FillLayoutUnnamed, FixtureHasTheProperty) {
  UnnamedFx u = unnamedFx();
  ASSERT_TRUE(u.fm.error.empty()) << u.fm.error;
  ASSERT_EQ(u.fm.branches.size(), 4u);
  for (const auto& b : u.fm.branches) {
    EXPECT_EQ(b.compress, 1) << b.name;
    EXPECT_EQ(codecOfSetting(b.compress, 1), "") << b.name; // the setting names no codec
    EXPECT_LT(b.zipBytes, b.totBytes) << b.name;            // and yet it is compressed
    ASSERT_EQ(b.writeBasket, 3) << b.name;
    const std::string want[] = {"zlib", "zlib", "lzma"};
    for (int i = 0; i < 3; ++i) {
      const uint8_t* rec = u.file.data() + b.basketSeek[i];
      auto k = parseKey(rec, static_cast<size_t>(b.basketBytes[i]), 0);
      ASSERT_TRUE(k);
      EXPECT_EQ(blockCodec(rec + k->keylen, static_cast<size_t>(b.basketBytes[i]) - k->keylen),
                want[i])
          << b.name << " basket " << i;
    }
  }
}

// The codec comes from ONE basket read, and the layout then relocates every
// such branch -- where before this file was declined whole.
TEST(FillLayoutUnnamed, TakesTheCodecOfTheBaskets) {
  UnnamedFx u = unnamedFx();
  ASSERT_TRUE(u.fm.error.empty()) << u.fm.error;
  CountingSource src;
  src.b = u.file;
  std::string codec;
  ASSERT_TRUE(unnamedSettingCodec(u.fm, u.header, src, codec));
  EXPECT_EQ(codec, "zlib");
  EXPECT_EQ(src.reads, 1);

  FillLayout L = unnamedLayout(u, {"lzma", "zlib"}, codec);
  ASSERT_TRUE(L.error.empty()) << L.error;
  EXPECT_EQ(L.relocated, (std::vector<uint32_t>{0, 1, 2, 3}));
  EXPECT_EQ(L.slots.size(), 12u);

  // Without the baskets' codec the file is declined, for its codec.
  FillLayout none = unnamedLayout(u, {"lzma", "zlib"}, "");
  EXPECT_NE(none.error.find("not converted"), std::string::npos) << none.error;
  EXPECT_TRUE(none.codecDecline);
}

// recompress_codecs keeps its meaning: the baskets' codec must be listed.
TEST(FillLayoutUnnamed, AnUnlistedCodecIsStillDeclined) {
  UnnamedFx u = unnamedFx();
  ASSERT_TRUE(u.fm.error.empty()) << u.fm.error;
  FillLayout L = unnamedLayout(u, {"lzma"}, "zlib");
  EXPECT_NE(L.error.find("zlib"), std::string::npos) << L.error;
  EXPECT_NE(L.error.find("recompress_codecs (lzma)"), std::string::npos) << L.error;
  EXPECT_TRUE(L.codecDecline);
  EXPECT_TRUE(L.slots.empty());
  // A codec decline does not point at `ucache recompress`: a sweep declines it too.
  EXPECT_EQ(declineNote(L), L.error);
  // A layout decline does: the sweep builds that file's replica from the byte cache.
  FillLayout past = layoutForFill(u.fm, u.file.size() + 1, u.header, u.treeKeyHeader, u.keysList,
                                  {"lzma", "zlib"}, 300, "zlib");
  EXPECT_FALSE(past.codecDecline);
  EXPECT_NE(declineNote(past).find("`ucache recompress`"), std::string::npos) << declineNote(past);
  EXPECT_EQ(declineNote(FillLayout()), ""); // nothing declined, nothing to say
}

// Every basket of the fixture, converted for its slot: each is decided by its
// own blocks. The LZMA basket in a branch taken as ZLIB is kept as stored when
// only zlib is listed and converted when lzma is, and whatever is converted
// decodes to exactly the original's bytes.
TEST(FillLayoutUnnamed, BasketsInAnotherCodecAreDecidedByTheirOwnBlocks) {
  UnnamedFx u = unnamedFx();
  ASSERT_TRUE(u.fm.error.empty()) << u.fm.error;
  FillLayout L = unnamedLayout(u, {"lzma", "zlib"}, "zlib");
  ASSERT_TRUE(L.error.empty()) << L.error;
  for (const auto& codecs :
       {std::vector<std::string>{"zlib"}, std::vector<std::string>{"lzma", "zlib"}}) {
    int converted = 0, kept = 0;
    for (const auto& s : L.slots) {
      const uint8_t* rec = u.file.data() + s.origSeek;
      ConvertedBasket c = convertBasket(rec, s.origLen, s.vLen, codecs);
      ASSERT_TRUE(c.error.empty()) << c.error;
      const bool lzmaBasket = s.basket == 2;
      EXPECT_EQ(c.codec, lzmaBasket ? "lzma" : "zlib");
      if (lzmaBasket && codecs.size() == 1) {
        EXPECT_EQ(c.kind, ConvertedBasket::kOriginal);
        EXPECT_EQ(c.record, std::vector<uint8_t>(rec, rec + s.origLen));
        ++kept;
        continue;
      }
      ASSERT_NE(c.kind, ConvertedBasket::kOriginal) << "slot " << s.vSeek;
      EXPECT_EQ(decodedBasket(c.record.data(), c.record.size()), decodedBasket(rec, s.origLen));
      // The key header is the original's but for fNbytes.
      EXPECT_EQ(0, std::memcmp(c.record.data() + 4, rec + 4, 14));
      ++converted;
    }
    EXPECT_EQ(kept, codecs.size() == 1 ? 4 : 0);
    EXPECT_EQ(converted, codecs.size() == 1 ? 8 : 12);
  }
}

// A basket whose blocks disagree on their codec, or whose block names none
// this converts, is kept exactly as stored -- never converted from a codec
// the policy does not name, never served as anything but the origin's bytes.
TEST(FillLayoutUnnamed, MismatchedBlocksAreKeptAsStored) {
  UnnamedFx u = unnamedFx();
  ASSERT_TRUE(u.fm.error.empty()) << u.fm.error;
  const BranchInfo& b = u.fm.branches[2]; // Jet_pt: the largest baskets
  auto payloadOf = [&](int i, int32_t& objlen) {
    const uint8_t* rec = u.file.data() + b.basketSeek[i];
    auto k = parseKey(rec, static_cast<size_t>(b.basketBytes[i]), 0);
    objlen = k->objlen;
    return std::vector<uint8_t>(rec + k->keylen, rec + b.basketBytes[i]);
  };
  int32_t o0 = 0, o2 = 0;
  const auto zl = payloadOf(0, o0), xz = payloadOf(2, o2);
  // One basket of two blocks: ZLIB, then LZMA. Each decodes, so a decoder
  // alone would convert it.
  std::vector<uint8_t> both = zl;
  both.insert(both.end(), xz.begin(), xz.end());
  auto mixed = basketRecord(o0 + o2, both);
  ASSERT_EQ(decompressFrames(both.data(), both.size(), static_cast<uint64_t>(o0 + o2)).size(),
            static_cast<size_t>(o0 + o2));
  ConvertedBasket c = convertBasket(mixed.data(), mixed.size(), 4 * mixed.size(), {"lzma", "zlib"});
  EXPECT_EQ(c.kind, ConvertedBasket::kOriginal);
  EXPECT_EQ(c.record, mixed);
  // The same blocks, one codec: converted.
  std::vector<uint8_t> twice = zl;
  twice.insert(twice.end(), zl.begin(), zl.end());
  auto same = basketRecord(2 * o0, twice);
  EXPECT_EQ(convertBasket(same.data(), same.size(), 4 * same.size(), {"zlib"}).kind,
            ConvertedBasket::kZstd);
  // The old ROOT algorithm's header: not decoded here, kept as stored.
  auto cs = basketRecord(o0, zl);
  const size_t kl = (cs[14] << 8) | cs[15];
  cs[kl] = 'C';
  cs[kl + 1] = 'S';
  ConvertedBasket old = convertBasket(cs.data(), cs.size(), 4 * cs.size(), {"lzma", "zlib"});
  EXPECT_EQ(old.kind, ConvertedBasket::kOriginal);
  EXPECT_EQ(old.record, cs);
  // A block chain that runs past the payload: kept as stored.
  auto cut = basketRecord(o0, std::vector<uint8_t>(zl.begin(), zl.end() - 1));
  EXPECT_EQ(convertBasket(cut.data(), cut.size(), 4 * cut.size(), {"zlib"}).kind,
            ConvertedBasket::kOriginal);
}

// A file whose settings name their codecs reads nothing; a basket stored
// uncompressed sends the question to the next branch; a failed read decides
// nothing.
TEST(FillLayoutUnnamed, ReadsOnlyWhatItMust) {
  {
    Fx fx = fixture(); // every setting names a codec
    CountingSource src;
    src.b.assign(static_cast<size_t>(fx.fileSize), 0);
    std::string codec = "x";
    ASSERT_TRUE(unnamedSettingCodec(fx.fm, fx.header, src, codec));
    EXPECT_EQ(codec, "");
    EXPECT_EQ(src.reads, 0);
  }
  {
    Fx fx = fixture(); // an unnamed branch whose metadata shows no compression
    fx.fm.branches[1].compress = 0;
    fx.fm.branches[1].totBytes = fx.fm.branches[1].zipBytes;
    CountingSource src;
    src.b.assign(static_cast<size_t>(fx.fileSize), 0);
    std::string codec;
    ASSERT_TRUE(unnamedSettingCodec(fx.fm, fx.header, src, codec));
    EXPECT_EQ(src.reads, 0);
    FillLayout L = layout(fx, {"lzma", "zlib"});
    ASSERT_TRUE(L.error.empty()) << L.error;
    EXPECT_EQ(L.relocated, (std::vector<uint32_t>{0, 2})); // never relocated: nothing to convert
  }
  {
    // Two unnamed branches: the more compressed one's first basket was stored
    // uncompressed, so the next one is asked.
    Fx fx = fixture();
    for (auto* b : {&fx.fm.branches[0], &fx.fm.branches[1]}) {
      b->compress = 1;
      b->totBytes = b->zipBytes * 4;
    }
    fx.fm.branches[0].totBytes = fx.fm.branches[0].zipBytes * 10; // asked first
    CountingSource src;
    src.b.assign(static_cast<size_t>(fx.fileSize), 0);
    auto raw = compressible(1000);
    auto stored = basketRecord(static_cast<int32_t>(raw.size()), raw);
    std::memcpy(src.b.data() + fx.fm.branches[0].basketSeek[0], stored.data(), stored.size());
    auto z = encodeZstdFrames(raw.data(), raw.size(), 1);
    auto zs = basketRecord(static_cast<int32_t>(raw.size()), z);
    std::memcpy(src.b.data() + fx.fm.branches[1].basketSeek[0], zs.data(), zs.size());
    std::string codec;
    ASSERT_TRUE(unnamedSettingCodec(fx.fm, fx.header, src, codec));
    EXPECT_EQ(codec, "zstd");
    EXPECT_EQ(src.reads, 2);
    // Both unnamed branches take it: zstd, which only the second list names.
    FillLayout L = layoutForFill(fx.fm, fx.fileSize, fx.header, fx.treeKeyHeader, fx.keysList,
                                 {"lzma"}, 400, codec);
    ASSERT_TRUE(L.error.empty()) << L.error;
    EXPECT_EQ(L.relocated, (std::vector<uint32_t>{2}));
    L = layoutForFill(fx.fm, fx.fileSize, fx.header, fx.treeKeyHeader, fx.keysList,
                      {"lzma", "zstd"}, 400, codec);
    ASSERT_TRUE(L.error.empty()) << L.error;
    EXPECT_EQ(L.relocated, (std::vector<uint32_t>{0, 1, 2}));

    src.failReads = true;
    EXPECT_FALSE(unnamedSettingCodec(fx.fm, fx.header, src, codec)); // nothing decided
  }
}

// ---- Mixed maps: the same address space, converted baskets stated at their
// real length. A map is its own tree record (in the reservation past map 0's)
// and its own keys-list entry; nothing else differs.

namespace {
// Map 0's tree record with slots 0 and 2 stated at a real length.
std::vector<uint8_t> mixedBlob(const FillLayout& L, const Fx& fx,
                               const std::vector<std::pair<uint32_t, uint32_t>>& real) {
  std::vector<uint8_t> blob;
  uint16_t keylen = 0;
  EXPECT_TRUE(decodeMetaRecord(L, blob, keylen));
  EXPECT_EQ(keylen, fx.fm.treeKey.keylen);
  std::string err;
  EXPECT_TRUE(stateRealLengths(blob, fx.fm, L, real, err)) << err;
  return blob;
}
} // namespace

TEST(FillLayoutMaps, TablesLieInTheReservationPastMapZero) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  MapTables t = mapTables(L);
  ASSERT_FALSE(t.empty());
  EXPECT_GE(t.first, L.metaSeek + L.metaRecord.size());
  EXPECT_LT(t.first, L.metaSeek + L.metaRecord.size() + 8);
  EXPECT_EQ(t.first % 8, 0u);
  EXPECT_EQ(t.end, L.slotsBegin);
  EXPECT_TRUE(mapTables(FillLayout()).empty()) << "a declined layout has no tables";
}

TEST(FillLayoutMaps, RealLengthsAreStatedWithTheTotals) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  ASSERT_EQ(L.slots.size(), 3u);
  const std::vector<std::pair<uint32_t, uint32_t>> real{{0, 1234}, {2, 555}};
  auto blob = mixedBlob(L, fx, real);
  EXPECT_EQ(beGet32(&blob[kB0Bytes]), 1234u);
  EXPECT_EQ(beGet32(&blob[kB0Bytes + 4]), L.slots[1].vLen) << "not converted: the slot's length";
  EXPECT_EQ(beGet32(&blob[kB2Bytes]), 555u);
  // Seeks never move.
  EXPECT_EQ(beGet64(&blob[kB0Seek]), L.slots[0].vSeek);
  EXPECT_EQ(beGet64(&blob[kB2Seek]), L.slots[2].vSeek);
  // fZipBytes: what each basket really is -- the record, or the original.
  EXPECT_EQ(beGet64(&blob[kB0Zip]), 1234u + 2500u);
  EXPECT_EQ(beGet64(&blob[kB2Zip]), 555u);
  EXPECT_EQ(beGet64(&blob[kB1Zip]), 800u);
  EXPECT_EQ(beGet64(&blob[kTreeZipOff]), 10000u - (1500 - 1234) - (900 - 555));
  // And read back from the record.
  std::vector<std::pair<uint32_t, uint32_t>> got;
  ASSERT_TRUE(statedRealLengths(blob, fx.fm, L, got));
  EXPECT_EQ(got, real);
  auto zero = mixedBlob(L, fx, {});
  ASSERT_TRUE(statedRealLengths(zero, fx.fm, L, got));
  EXPECT_TRUE(got.empty()) << "map 0 states every slot at its length";
}

TEST(FillLayoutMaps, StatingRefusesWhatTheLayoutDoesNotHold) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  std::vector<uint8_t> blob;
  uint16_t keylen = 0;
  ASSERT_TRUE(decodeMetaRecord(L, blob, keylen));
  std::string err;
  for (const auto& bad : std::vector<std::pair<uint32_t, uint32_t>>{
           {1, 0}, {1, L.slots[1].vLen + 1}, {3, 10}}) {
    auto b = blob;
    EXPECT_FALSE(stateRealLengths(b, fx.fm, L, {bad}, err)) << bad.first << " " << bad.second;
  }
  // The original record: its baskets are not where the layout put them.
  auto orig = fx.fm.treeBlob;
  EXPECT_FALSE(stateRealLengths(orig, fx.fm, L, {{0, 100}}, err));
  EXPECT_NE(err.find("not where"), std::string::npos) << err;
}

TEST(FillLayoutMaps, AMapsRecordIsMapZerosKeyAtItsPlace) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  auto blob = mixedBlob(L, fx, {{0, 1234}});
  const uint64_t seek = mapTables(L).first;
  std::vector<uint8_t> rec;
  std::string err;
  ASSERT_TRUE(mapMetaRecord(L, blob, seek, rec, err)) << err;
  const size_t kl = fx.fm.treeKey.keylen;
  ASSERT_GT(rec.size(), kl);
  EXPECT_EQ(beGet32(rec.data()), rec.size());                      // fNbytes
  EXPECT_EQ(beGet64(rec.data() + 18), seek);                       // fSeekKey
  EXPECT_EQ(beGet32(rec.data() + 6), fx.fm.treeBlob.size());       // fObjlen
  EXPECT_EQ(0, std::memcmp(rec.data() + 26, L.metaRecord.data() + 26, kl - 26));
  EXPECT_LT(rec.size() - kl, blob.size()) << "compressed, never raw";
  std::vector<uint8_t> back;
  uint16_t k2 = 0;
  ASSERT_TRUE(decodeKeyRecord(rec, back, k2));
  EXPECT_EQ(back, blob);
  // A record that does not compress is not stored raw: ROOT tells the two
  // apart by the length a keys-list entry states, and a reader holding
  // another map's entry must find the form it expects.
  EXPECT_FALSE(mapMetaRecord(L, incompressible(blob.size()), seek, rec, err));
}

TEST(FillLayoutMaps, AMapsKeysListPointsAtItsRecordAlone) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  const uint64_t seek = mapTables(L).first;
  FillLayout::Window w;
  std::string err;
  ASSERT_TRUE(keysListForMap(L, seek, 321, w, err)) << err;
  EXPECT_EQ(w.off, static_cast<uint64_t>(kKeysListSeek));
  ASSERT_EQ(w.bytes.size(), L.windows[1].bytes.size());
  EXPECT_EQ(beGet32(&w.bytes[fx.entryOff]), 321u);
  EXPECT_EQ(beGet64(&w.bytes[fx.entryOff + 18]), seek);
  std::vector<uint8_t> a = L.windows[1].bytes, b = w.bytes;
  std::memset(&a[fx.entryOff], 0, 4);
  std::memset(&b[fx.entryOff], 0, 4);
  std::memset(&a[fx.entryOff + 18], 0, 8);
  std::memset(&b[fx.entryOff + 18], 0, 8);
  EXPECT_EQ(a, b);
  // A 32-bit entry cannot point past 2 GiB.
  Fx nx = fixture(70000, false, /*wideEntry=*/false);
  FillLayout N = layout(nx);
  ASSERT_TRUE(N.error.empty()) << N.error;
  EXPECT_TRUE(keysListForMap(N, mapTables(N).first, 321, w, err));
  EXPECT_FALSE(keysListForMap(N, 3000000000ull, 321, w, err));
}

TEST(FillLayoutMaps, AReadToTheRecordsEndIsTheWholeBasket) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  const std::vector<std::pair<uint64_t, uint64_t>> metaOrigin{
      {static_cast<uint64_t>(kTreeKeySeek), 2000}};
  const std::function<uint32_t(uint32_t)> real = [](uint32_t i) { return i == 0 ? 1234u : 0u; };
  const auto& s0 = L.slots[0];
  const auto& s1 = L.slots[1];
  std::vector<std::pair<uint64_t, uint64_t>> out;
  ASSERT_TRUE(exactOriginRanges(L, metaOrigin, s0.vSeek, 1234, out, &real));
  EXPECT_EQ(out, (std::vector<std::pair<uint64_t, uint64_t>>{{1000, 1500}}));
  out.clear();
  EXPECT_FALSE(exactOriginRanges(L, metaOrigin, s0.vSeek, 1233, out, &real)) << "cut short";
  out.clear();
  EXPECT_FALSE(exactOriginRanges(L, metaOrigin, s0.vSeek, 1234, out)) << "no map: a part";
  out.clear();
  EXPECT_FALSE(exactOriginRanges(L, metaOrigin, s1.vSeek, 1234, out, &real)) << "not converted";
  // The whole slot is the whole basket either way; and a read over slot 0's
  // record into slot 1 is a part.
  out.clear();
  ASSERT_TRUE(exactOriginRanges(L, metaOrigin, s0.vSeek, s0.vLen, out, &real));
  out.clear();
  EXPECT_FALSE(exactOriginRanges(L, metaOrigin, s0.vSeek, s0.vLen + 10, out, &real));
  // The map's own tree record, whole, is the original tree key.
  const uint64_t seek = mapTables(L).first;
  out.clear();
  ASSERT_TRUE(exactOriginRanges(L, metaOrigin, seek, 300, out, nullptr, {seek, 300}));
  EXPECT_EQ(out, metaOrigin);
  out.clear();
  EXPECT_FALSE(exactOriginRanges(L, metaOrigin, seek, 299, out, nullptr, {seek, 300}));
}
