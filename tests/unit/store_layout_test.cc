// The stored layout (StoreLayout.h): computed from a real file, stored and read
// back with every byte and the hash unchanged, for both containers; and the
// rule that decides whether a store is served.
#include "StoreLayout.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ucache;
using namespace ucache::transpose;

namespace {

std::vector<uint8_t> slurp(const std::string& path) {
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

struct BytesSource : Source {
  std::vector<uint8_t> b;
  bool has(uint64_t off, uint64_t n) override { return off + n >= off && off + n <= b.size(); }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    if (!has(off, n))
      return false;
    std::memcpy(dst, b.data() + off, n);
    return true;
  }
};

const std::vector<std::string> kEveryCodec = {"lzma", "zlib", "zstd", "lz4"};

void roundTrip(const std::string& file, bool wantRnt) {
  BytesSource src;
  src.b = slurp(std::string(UCACHE_TEST_DATA_DIR) + "/" + file);
  ASSERT_FALSE(src.b.empty()) << file;
  StoredLayout a;
  a.codecs = kEveryCodec;
  a.slotFactor100 = kSlotFactor100;
  bool declined = false;
  std::string why;
  ASSERT_TRUE(computeLayout(a, src, nullptr, src.b.size(), kSlotFactor100, declined, why))
      << file << ": " << why << " " << a.L.error;
  EXPECT_EQ(a.rnt, wantRnt);
  ASSERT_FALSE(a.L.slots.empty());
  if (a.rnt) {
    EXPECT_EQ(a.pages.size(), a.L.slots.size());
  }
  const std::vector<uint8_t> blob = encodeLayout(a);
  StoredLayout b;
  b.slotFactor100 = a.slotFactor100; // a store's header carries it beside the blob
  ASSERT_TRUE(decodeLayout(blob, b)) << file;
  EXPECT_EQ(b.rnt, a.rnt);
  EXPECT_EQ(layoutHash(b.L, b.rnt), layoutHash(a.L, a.rnt)) << file;
  EXPECT_EQ(encodeLayout(b), blob) << file;
  EXPECT_EQ(b.metaOrigin, a.metaOrigin);
  // A damaged blob is refused, never half-decoded.
  std::vector<uint8_t> bad = blob;
  bad.resize(bad.size() / 2);
  StoredLayout c;
  c.slotFactor100 = a.slotFactor100;
  EXPECT_FALSE(decodeLayout(bad, c));
}

} // namespace

TEST(StoreLayout, ATTreeLayoutIsStoredAndReadBackUnchanged) {
  roundTrip("unnamed_codec_fixture.root", false);
}

TEST(StoreLayout, AnRNTupleLayoutIsStoredAndReadBackUnchanged) {
  roundTrip("rntuple_fixture.root", true);
}

TEST(StoreLayout, AFileWithNeitherContainerIsDeclinedForGood) {
  BytesSource src;
  src.b.assign(4096, 0);
  std::memcpy(src.b.data(), "root", 4);
  StoredLayout a;
  a.codecs = kEveryCodec;
  bool declined = false;
  std::string why;
  EXPECT_FALSE(computeLayout(a, src, nullptr, src.b.size(), kSlotFactor100, declined, why));
}

TEST(StoreLayout, TheStoreSideOfAdoption) {
  SlotStoreHeader h;
  h.layoutVersion = kLayoutVersion;
  h.originSize = 1000;
  h.originMtime = 7;
  EXPECT_TRUE(adoptable(h, 1000, 7, 0, 0, ValidateMode::kSize));
  EXPECT_FALSE(adoptable(h, 999, 7, 0, 0, ValidateMode::kSize));          // another file
  EXPECT_TRUE(adoptable(h, 1000, 8, 0, 0, ValidateMode::kSize));          // mtime not asked
  EXPECT_FALSE(adoptable(h, 1000, 8, 0, 0, ValidateMode::kSizeMtime));    // mtime asked
  h.layoutVersion = kLayoutVersion + 1;
  EXPECT_FALSE(adoptable(h, 1000, 7, 0, 0, ValidateMode::kSize));         // a newer uCache's
  h.layoutVersion = kTTreeLayoutVersion;
  EXPECT_TRUE(adoptable(h, 1000, 7, 0, 0, ValidateMode::kSize));
  h.layoutVersion = kOldestServedLayout;
  EXPECT_TRUE(adoptable(h, 1000, 7, 0, 0, ValidateMode::kSize));          // still served
  h.declined = true;
  EXPECT_FALSE(adoptable(h, 1000, 7, 0, 0, ValidateMode::kSize));         // decided again
  h.layoutVersion = kOldestDecision;
  EXPECT_TRUE(adoptable(h, 1000, 7, 0, 0, ValidateMode::kSize));
  h.declined = false;
  h.layoutVersion = kOldestServedLayout - 1;
  EXPECT_FALSE(adoptable(h, 1000, 7, 0, 0, ValidateMode::kSize));         // too old
}
