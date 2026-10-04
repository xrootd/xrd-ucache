// Reading a hole must not look like a corrupt file.
//
// CacheSource reads a cache entry's bytes for the command line (the codec a
// branch is stored in, which branches are cached) from a bitmap snapshot: the
// sidecar is loaded once, then pages are read. Converting a page frees its
// original -- the bits are cleared and the sidecar stored, and only then the
// bytes punched -- so a reader holding an older snapshot can be pointed at
// pages that have since become holes. They read back as zeros, and are caught
// by the per-page checksum the serving path relies on too. Pinned here.
#include "CacheSource.h"
#include "MetaFile.h"
#include "TreeMeta.h"
#include "Transposer.h"
#include "vendor/crc32c.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "TestUtil.h"

using namespace ucache;
using namespace ucache::transpose;

namespace {

constexpr uint32_t kPageSize = 4096;
constexpr uint64_t kBasketOff = 8192; // page-aligned, two pages in
constexpr uint32_t kBasketLen = 4096;

// An entry on disk: `fileSize` bytes of content, a sidecar bitmap marking
// every page present, and honest per-page checksums.
struct Entry {
  std::string path;
  MetaData meta;
  int fd = -1;
  ~Entry() {
    if (fd >= 0)
      ::close(fd);
  }
};

void fillEntry(Entry& e, const ucache::test::TempDir& td, const std::vector<uint8_t>& content) {
  e.path = td.path() + "/entry.data";
  FILE* f = ::fopen(e.path.c_str(), "wb");
  ::fwrite(content.data(), 1, content.size(), f);
  ::fclose(f);
  e.meta = MetaData::fresh("root://h//f.root", content.size(), kPageSize);
  for (uint64_t i = 0; i < e.meta.npages(); ++i) {
    e.meta.bitmap.set(i);
    e.meta.pageCrcs[i] = crc32c(content.data() + i * kPageSize, e.meta.pageBytes(i));
  }
  e.fd = ::open(e.path.c_str(), O_RDONLY | O_CLOEXEC);
}

// Punch a page the way reclaim does, but leave the sidecar alone: bit still
// set, checksum still the pre-punch one. This is precisely the state a build
// holding an older snapshot sees.
void zeroPageOnDisk(const std::string& path, uint64_t page) {
  int fd = ::open(path.c_str(), O_WRONLY);
  ASSERT_GE(fd, 0);
  std::vector<uint8_t> zeros(kPageSize, 0);
  ASSERT_EQ(::pwrite(fd, zeros.data(), zeros.size(), static_cast<off_t>(page * kPageSize)),
            static_cast<ssize_t>(zeros.size()));
  ::close(fd);
}

} // namespace

// ------------------------------------------------- CacheSource verification

TEST(TransposeHole, IntactPagesReadBack) {
  ucache::test::TempDir td;
  Entry e;
  fillEntry(e, td, ucache::test::randomBytes(16384, 7));
  CacheSource src;
  src.fd = e.fd;
  src.meta = &e.meta;

  std::vector<uint8_t> got(kBasketLen);
  EXPECT_TRUE(src.has(kBasketOff, kBasketLen));
  ASSERT_TRUE(src.read(got.data(), kBasketLen, kBasketOff));
  std::vector<uint8_t> want(kBasketLen);
  ASSERT_EQ(::pread(e.fd, want.data(), want.size(), static_cast<off_t>(kBasketOff)),
            static_cast<ssize_t>(want.size()));
  EXPECT_EQ(got, want);
}

TEST(TransposeHole, UnalignedReadsSpanningPagesAreVerified) {
  ucache::test::TempDir td;
  Entry e;
  fillEntry(e, td, ucache::test::randomBytes(16384, 11));
  CacheSource src;
  src.fd = e.fd;
  src.meta = &e.meta;
  // Straddles three pages and starts mid-page: the checksum is per page, the
  // read is not, so the buffer arithmetic has to be right.
  const uint64_t off = kPageSize + 100, n = 2 * kPageSize + 55;
  std::vector<uint8_t> got(n), want(n);
  ASSERT_TRUE(src.read(got.data(), n, off));
  ASSERT_EQ(::pread(e.fd, want.data(), want.size(), static_cast<off_t>(off)),
            static_cast<ssize_t>(want.size()));
  EXPECT_EQ(got, want);
}

TEST(TransposeHole, PunchedPageIsRefusedNotReturnedAsZeros) {
  ucache::test::TempDir td;
  Entry e;
  fillEntry(e, td, ucache::test::randomBytes(16384, 13));
  zeroPageOnDisk(e.path, kBasketOff / kPageSize);
  CacheSource src;
  src.fd = e.fd;
  src.meta = &e.meta;

  // The stale snapshot still claims the page — that is the whole problem.
  EXPECT_TRUE(src.has(kBasketOff, kBasketLen));
  std::vector<uint8_t> got(kBasketLen, 0xEE);
  EXPECT_FALSE(src.read(got.data(), kBasketLen, kBasketOff));
  // A neighbouring page is untouched and must still be readable.
  EXPECT_TRUE(src.read(got.data(), kBasketLen, 0));
}

TEST(TransposeHole, TailPageAndShortSidecarsAreHandled) {
  ucache::test::TempDir td;
  Entry e;
  fillEntry(e, td, ucache::test::randomBytes(kPageSize + 100, 17)); // short tail page
  CacheSource src;
  src.fd = e.fd;
  src.meta = &e.meta;
  std::vector<uint8_t> got(100);
  EXPECT_TRUE(src.read(got.data(), 100, kPageSize)); // tail page verifies on its real length
  EXPECT_FALSE(src.read(got.data(), 100, kPageSize + 50));  // past EOF
  // A summary-loaded sidecar carries no checksum table: nothing to verify
  // against, so a read must refuse rather than pass bytes through unchecked.
  MetaData summary = e.meta;
  summary.pageCrcs.clear();
  src.meta = &summary;
  EXPECT_FALSE(src.read(got.data(), 100, 0));
}
