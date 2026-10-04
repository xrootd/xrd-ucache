// Seek-width contract tests: the file header and the root directory record
// carry INDEPENDENT widths, and a 64-bit header over a 32-bit directory record
// is a valid in-the-wild layout (a file that grew past 2 GB after its keys list
// had been written keeps a narrow directory record pointing at an early keys
// list). The parser must accept it.
#include "CacheStore.h"
#include "IOBackend.h"
#include "ReadFootprint.h"
#include "Transposer.h"
#include "TreeMeta.h"

#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>

#include "TestUtil.h"

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

// A TKey header. `wide` = version + 1000 => 64-bit fSeekKey/fSeekPdir (ROOT
// widens a key by its own placement, independently of the directory record).
std::vector<uint8_t> keyHeader(bool wide, const std::string& cls, const std::string& name,
                              int32_t nbytes, int32_t objlen, int64_t seekkey) {
  const size_t fixed = wide ? 34u : 26u;
  const size_t keylen = fixed + 1 + cls.size() + 1 + name.size() + 1; // + empty title
  std::vector<uint8_t> k(keylen, 0);
  bePut32(k.data(), static_cast<uint32_t>(nbytes));
  bePut16(k.data() + 4, static_cast<uint16_t>(wide ? 1004 : 4));
  bePut32(k.data() + 6, static_cast<uint32_t>(objlen));
  bePut16(k.data() + 14, static_cast<uint16_t>(keylen));
  bePut16(k.data() + 16, 1); // cycle
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
  q += name.size();
  k[q] = 0; // title ""
  return k;
}

// 512-byte crafted TFile header + root directory record. `large` sets the
// 64-bit HEADER layout, `wideDir` the 64-bit DIRECTORY record — deliberately
// independent, which is the whole point of these tests.
std::vector<uint8_t> craftHeader(bool large, bool wideDir, int64_t fend, int64_t keyslistSeek) {
  std::vector<uint8_t> h(512, 0);
  std::memcpy(h.data(), "root", 4);
  bePut32(h.data() + 4, large ? 1062604u : 62604u);
  if (large)
    bePut64(h.data() + 12, static_cast<uint64_t>(fend));
  else
    bePut32(h.data() + 12, static_cast<uint32_t>(fend));
  // Root directory key at fBEGIN = 100 (a narrow key; only its keylen is read).
  auto dk = keyHeader(false, "TFile", "f.root", 100, 0, 100);
  std::memcpy(h.data() + 100, dk.data(), dk.size());
  size_t q = 100 + dk.size();
  h[q++] = 0; // TNamed: fName  ""
  h[q++] = 0; // TNamed: fTitle ""
  bePut16(h.data() + q, static_cast<uint16_t>(wideDir ? 1005 : 5));
  q += 2 + 4 + 4 + 4 + 4;              // version, 2x datime, fNbytesKeys, fNbytesName
  const size_t dw = wideDir ? 8u : 4u; // fSeekDir, fSeekParent, then fSeekKeys
  q += 2 * dw;
  if (wideDir)
    bePut64(h.data() + q, static_cast<uint64_t>(keyslistSeek));
  else
    bePut32(h.data() + q, static_cast<uint32_t>(keyslistSeek));
  return h;
}

std::string writeTemp(const ucache::test::TempDir& td, const std::vector<uint8_t>& bytes) {
  std::string path = td.path() + "/crafted.root";
  FILE* f = ::fopen(path.c_str(), "wb");
  ::fwrite(bytes.data(), 1, bytes.size(), f);
  ::fclose(f);
  return path;
}

// Geometry of the mixed-width layout: a 3 GB file (64-bit header) whose keys
// list was written at 512 MB and whose directory record therefore stayed
// 32-bit.
constexpr int64_t kFend = 3000000000ll;
constexpr int64_t kKeysListSeek = 511823714ll;


} // namespace

// ---------------------------------------------------------------- parser

TEST(TransposeWidth, WideHeaderOverNarrowDirectoryIsAccepted) {
  ucache::test::TempDir td;
  auto path = writeTemp(td, craftHeader(/*large=*/true, /*wideDir=*/false, kFend, kKeysListSeek));
  FileMeta fm = parseFile(path);
  // The mismatch itself must no longer be a verdict: parsing gets past the
  // header/directory walk and only stops for want of the (absent) keys list.
  EXPECT_EQ(fm.error, "cannot read keys list");
  EXPECT_TRUE(fm.large);
  EXPECT_EQ(fm.headerSeekWidth, 8);
  EXPECT_EQ(fm.dirSeekWidth, 4);
  EXPECT_EQ(fm.fend, kFend);
  EXPECT_EQ(fm.keyslistSeek, kKeysListSeek); // read at the DIRECTORY's width
}

TEST(TransposeWidth, MatchingWidthsStillParse) {
  ucache::test::TempDir td;
  { // both wide
    auto p = writeTemp(td, craftHeader(true, true, kFend, kKeysListSeek));
    FileMeta fm = parseFile(p);
    EXPECT_EQ(fm.error, "cannot read keys list");
    EXPECT_EQ(fm.headerSeekWidth, 8);
    EXPECT_EQ(fm.dirSeekWidth, 8);
    EXPECT_EQ(fm.fend, kFend);
    EXPECT_EQ(fm.keyslistSeek, kKeysListSeek);
  }
  { // both narrow (the small-file layout)
    auto p = writeTemp(td, craftHeader(false, false, 1000000, 900000));
    FileMeta fm = parseFile(p);
    EXPECT_EQ(fm.error, "cannot read keys list");
    EXPECT_EQ(fm.headerSeekWidth, 4);
    EXPECT_EQ(fm.dirSeekWidth, 4);
    EXPECT_EQ(fm.fend, 1000000);
    EXPECT_EQ(fm.keyslistSeek, 900000);
  }
}

TEST(TransposeWidth, NarrowHeaderOverWideDirectoryIsAccepted) {
  // The converse mismatch (a 32-bit header with a 64-bit directory record) is
  // just as much a per-site fact; neither is a reason to refuse the file.
  ucache::test::TempDir td;
  auto p = writeTemp(td, craftHeader(/*large=*/false, /*wideDir=*/true, 1000000, 900000));
  FileMeta fm = parseFile(p);
  EXPECT_EQ(fm.error, "cannot read keys list");
  EXPECT_EQ(fm.headerSeekWidth, 4);
  EXPECT_EQ(fm.dirSeekWidth, 8);
  EXPECT_EQ(fm.keyslistSeek, 900000);
}

// A Source over the crafted bytes: the walk the plugin would run on bytes it
// holds itself (a staged fill) must reach the same verdict and geometry as the
// path-based walk, and must stop where the Source cannot vouch for a range.
struct BytesSource : Source {
  std::vector<uint8_t> bytes;
  explicit BytesSource(std::vector<uint8_t> b) : bytes(std::move(b)) {}
  bool has(uint64_t off, uint64_t n) override { return off + n <= bytes.size(); }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    if (!has(off, n))
      return false;
    std::memcpy(dst, bytes.data() + off, n);
    return true;
  }
};

TEST(TransposeWidth, SourceWalkMatchesPathWalk) {
  ucache::test::TempDir td;
  auto bytes = craftHeader(/*large=*/true, /*wideDir=*/false, kFend, kKeysListSeek);
  FileMeta viaPath = parseFile(writeTemp(td, bytes));
  BytesSource src(bytes);
  FileMeta viaSource = parseFile(src, static_cast<int64_t>(bytes.size()));
  EXPECT_EQ(viaSource.error, viaPath.error);
  EXPECT_EQ(viaSource.error, "cannot read keys list");
  EXPECT_EQ(viaSource.large, viaPath.large);
  EXPECT_EQ(viaSource.headerSeekWidth, 8);
  EXPECT_EQ(viaSource.dirSeekWidth, 4);
  EXPECT_EQ(viaSource.fend, kFend);
  EXPECT_EQ(viaSource.keyslistSeek, kKeysListSeek);
  ContainerMeta cm = parseContainer(src, static_cast<int64_t>(bytes.size()));
  EXPECT_EQ(cm.error, "cannot read keys list");
  EXPECT_EQ(cm.keyslistSeek, kKeysListSeek);
}

TEST(TransposeWidth, SourceThatCannotVouchFailsNotGuesses) {
  // Presence false for every range: the header itself is unavailable, and the
  // walk must refuse rather than read whatever `read` might hand back.
  struct Absent : Source {
    bool has(uint64_t, uint64_t) override { return false; }
    bool read(void* dst, uint64_t n, uint64_t) override {
      std::memset(dst, 0, n);
      return true;
    }
  } absent;
  FileMeta fm = parseFile(absent, 4096);
  EXPECT_EQ(fm.error, "not a ROOT file");
  EXPECT_TRUE(fm.branches.empty());
  // And a blob no tree walk can accept fails the same way, without throwing.
  std::vector<uint8_t> junk(64, 0xEE);
  FileMeta blob;
  EXPECT_FALSE(parseTreeBlob(junk.data(), junk.size(), 40, blob));
  EXPECT_FALSE(blob.error.empty());
  EXPECT_TRUE(blob.branches.empty());
}
