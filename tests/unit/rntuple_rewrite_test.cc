// The RNTuple recompressor, checked against a committed fixture.
//
// ROOT is the arbiter for whether a rewritten file is READABLE, and that check
// lives in an environment-bound gate because it needs ROOT and real files.
// What can be proved here, hermetically and with no ROOT at all, is the
// property that actually matters for correctness: **every page must decode to
// exactly the same bytes after recompression as before.** A codec change that
// preserves that cannot alter physics; one that breaks it cannot be saved by
// anything downstream.
//
// The fixture is 6.5 KB and deliberately covers what a NanoAOD file does not
// fit into that size: three clusters, a bit-packed boolean column (one bit per
// element, where the byte count is a CEILING), a constant column whose
// identical pages are shared between records, and a variable-length column.
// tests/data/make_rntuple_fixture.C regenerates it.
#include "CacheStore.h"
#include "IOBackend.h"
#include "RNTupleMeta.h"
#include "RNTupleRewrite.h"
#include "ReadFootprint.h"
#include "ReplicaStore.h"
#include "TreeMeta.h"

#include <gtest/gtest.h>
#include <set>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "TestUtil.h"

using namespace ucache::transpose;

namespace {

std::string fixture() { return std::string(UCACHE_TEST_DATA_DIR) + "/rntuple_fixture.root"; }

class FileSource : public Source {
public:
  explicit FileSource(int fd, uint64_t size) : fd_(fd), size_(size) {}
  bool read(void* dst, uint64_t n, uint64_t off) override {
    return ::pread(fd_, dst, n, (off_t)off) == (ssize_t)n;
  }
  bool has(uint64_t off, uint64_t n) override { return off + n >= off && off + n <= size_; }

private:
  int fd_;
  uint64_t size_;
};

// Decode every page of `m`, reading through `src`, into one blob per page in
// page-table order. Two files with the same decoded sequence hold the same
// data whatever codec they are written in.
std::vector<std::vector<uint8_t>> decodeAllPages(const RNTupleMeta& m, Source& src) {
  std::vector<std::vector<uint8_t>> out;
  for (const auto& range : m.ranges) {
    for (const auto& pg : range.pages) {
      std::vector<uint8_t> raw(pg.nbytes);
      if (pg.nbytes && !src.read(raw.data(), pg.nbytes, pg.offset)) return {};
      if ((uint64_t)pg.nbytes == pg.uncompressedBytes) {
        out.push_back(std::move(raw)); // stored uncompressed
      } else {
        auto d = decompressFrames(raw.data(), raw.size(), pg.uncompressedBytes);
        if (d.size() != pg.uncompressedBytes) return {};
        out.push_back(std::move(d));
      }
    }
  }
  return out;
}

struct Rewritten {
  std::string path;
  ~Rewritten() {
    if (!path.empty()) ::unlink(path.c_str());
  }
};

} // namespace

TEST(RNTupleFixture, ParsesWithTheStructureTheGateReliesOn) {
  auto m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  EXPECT_EQ(m.ntupleName, "Events");
  EXPECT_EQ(m.nEntries, 2000u);
  // If any of these drift the fixture stopped covering what it was built for.
  EXPECT_EQ(m.nClusters, 3u) << "fixture must span several clusters";
  EXPECT_EQ(m.columns.size(), 6u);
  size_t pages = 0, bitPacked = 0;
  for (const auto& r : m.ranges) pages += r.pages.size();
  for (const auto& c : m.columns)
    if (c.bitsOnStorage == 1) ++bitPacked;
  EXPECT_GT(pages, 20u);
  EXPECT_GE(bitPacked, 1u) << "fixture must contain a bit-packed column";
}

TEST(RNTupleFixture, SharedPagesAreRealInTheFixture) {
  // Several page records pointing at the same bytes is a format feature, not a
  // curiosity: the writer must reuse the transcode and must not punch a page
  // another record still needs.
  auto m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  size_t pages = 0;
  std::vector<uint64_t> offs;
  for (const auto& r : m.ranges)
    for (const auto& p : r.pages) {
      ++pages;
      offs.push_back(p.offset);
    }
  std::sort(offs.begin(), offs.end());
  const size_t distinct = std::unique(offs.begin(), offs.end()) - offs.begin();
  EXPECT_LT(distinct, pages) << "fixture must contain at least one shared page";
}

TEST(RNTupleRewrite, EveryPageDecodesToTheSameBytesAfterRecompression) {
  auto m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  int fd = ::open(fixture().c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  FileSource src(fd, m.fileSize);

  const auto before = decodeAllPages(m, src);
  ASSERT_FALSE(before.empty());

  auto rw = buildRNTupleRewrite(m, src, m.fileSize, 1);
  ::close(fd);
  ASSERT_TRUE(rw.error.empty()) << rw.error;
  EXPECT_EQ(rw.rangesRelocated, m.ranges.size());
  EXPECT_GE(rw.sharedPages, 1u) << "a shared page must be reused, not re-encoded";

  Rewritten out{std::string(::testing::TempDir()) + "/ucache_rntuple_rw.root"};
  std::string err;
  ASSERT_TRUE(writeRewrittenRNTuple(fixture(), out.path, rw, err)) << err;

  auto m2 = parseRNTuple(out.path, "");
  ASSERT_TRUE(m2.error.empty()) << m2.error;
  EXPECT_EQ(m2.nEntries, m.nEntries);
  EXPECT_EQ(m2.nClusters, m.nClusters);
  EXPECT_EQ(m2.columns.size(), m.columns.size());
  for (const auto& r : m2.ranges)
    EXPECT_EQ(r.compressionSettings, 501) << "pages must now be ZSTD-1";

  int fd2 = ::open(out.path.c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd2, 0);
  FileSource src2(fd2, m2.fileSize);
  const auto after = decodeAllPages(m2, src2);
  ::close(fd2);

  // The whole point: same decoded bytes, different codec.
  ASSERT_EQ(after.size(), before.size());
  for (size_t i = 0; i < before.size(); ++i)
    EXPECT_EQ(after[i], before[i]) << "page " << i << " changed content";
}

TEST(RNTupleRewrite, OverlayStitchesToTheRewrittenFile) {
  // The replica serves the overlay, not the standalone file. If the two ever
  // disagree, a check on one says nothing about the other.
  auto m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  int fd = ::open(fixture().c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  FileSource src(fd, m.fileSize);
  auto rw = buildRNTupleRewrite(m, src, m.fileSize, 1);
  ASSERT_TRUE(rw.error.empty()) << rw.error;

  Rewritten out{std::string(::testing::TempDir()) + "/ucache_rntuple_stitch.root"};
  std::string err;
  ASSERT_TRUE(writeRewrittenRNTuple(fixture(), out.path, rw, err)) << err;

  auto ov = rnTupleOverlay(m, rw);
  ASSERT_TRUE(ov.error.empty()) << ov.error;
  std::vector<uint8_t> stitched(ov.meta.virtualSize, 0);
  ASSERT_EQ(::pread(fd, stitched.data(), m.fileSize, 0), (ssize_t)m.fileSize);
  ::close(fd);
  for (const auto& e : ov.meta.extents)
    std::memcpy(stitched.data() + e.virtOff, ov.tdata.data() + e.tdataOff, e.len);

  std::vector<uint8_t> written(ov.meta.virtualSize, 0);
  int fd2 = ::open(out.path.c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd2, 0);
  ASSERT_EQ(::pread(fd2, written.data(), written.size(), 0), (ssize_t)written.size());
  ::close(fd2);
  EXPECT_EQ(stitched, written);
}

TEST(RNTupleRewrite, CorruptPageIsRefusedRatherThanRecompressed) {
  // Negative control. A page whose bytes are damaged must stop the build, not
  // be re-encoded into a replica that serves wrong data for ever.
  auto m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;

  // Copy the fixture and flip a byte inside the LAST page's payload — the
  // first page of a column is not a safe target, since a decoder can be handed
  // a plausible-looking block and produce something.
  const std::string copy = std::string(::testing::TempDir()) + "/ucache_rntuple_rot.root";
  {
    std::vector<uint8_t> all(m.fileSize);
    int f = ::open(fixture().c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_GE(f, 0);
    ASSERT_EQ(::pread(f, all.data(), all.size(), 0), (ssize_t)all.size());
    ::close(f);
    const auto& last = m.ranges.back().pages.back();
    ASSERT_GT(last.nbytes, 4u);
    all[last.offset + last.nbytes / 2] ^= 0xFF;
    int g = ::open(copy.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    ASSERT_GE(g, 0);
    ASSERT_EQ(::write(g, all.data(), all.size()), (ssize_t)all.size());
    ::close(g);
  }
  Rewritten cleanup{copy};

  auto m2 = parseRNTuple(copy, "");
  ASSERT_TRUE(m2.error.empty()) << m2.error; // the page table is still intact
  int fd = ::open(copy.c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  FileSource src(fd, m2.fileSize);
  auto rw = buildRNTupleRewrite(m2, src, m2.fileSize, 1);
  ::close(fd);
  EXPECT_FALSE(rw.error.empty()) << "a corrupt page must not recompress silently";
}

TEST(RNTupleRewrite, OriginMapCoversTheWholePageIncludingItsChecksum) {
  // A replica run is comparable with the baseline that measured it only
  // because every stitched read maps back to the original bytes it carried.
  // A page's on-disk object is the block PLUS the 8-byte checksum that sits
  // past the locator's size, so a mapping that records the block alone leaves
  // that tail with no original address: the same read reports fewer bytes
  // touched when it is served from the replica than when it is served from
  // the byte cache. Nothing downstream can notice — the tail is 8 bytes, and
  // it only changes the answer when it falls in a region nothing else reads —
  // so the invariant has to be pinned right here, at the builder.
  auto m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  int fd = ::open(fixture().c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  FileSource src(fd, m.fileSize);
  auto rw = buildRNTupleRewrite(m, src, m.fileSize, 1);
  ::close(fd);
  ASSERT_TRUE(rw.error.empty()) << rw.error;
  ASSERT_GT(rw.pages, 0u);

  size_t checksummed = 0, pages = 0;
  for (const auto& range : m.ranges) {
    for (const auto& pg : range.pages) {
      const uint64_t onDisk = (uint64_t)pg.nbytes + (pg.hasChecksum ? 8 : 0);
      if (pg.hasChecksum) ++checksummed;
      ++pages;
      const ucache::ReplicaMeta::OrigRange* found = nullptr;
      for (const auto& r : rw.origMap)
        if (r.origOff == pg.offset) found = &r;
      ASSERT_NE(found, nullptr) << "page at " << pg.offset << " has no mapping";
      EXPECT_EQ(found->origLen, onDisk)
          << "page at " << pg.offset << ": mapping covers " << found->origLen
          << " of " << onDisk << " bytes on disk";
    }
  }
  EXPECT_GT(pages, 0u);
  // If the fixture ever stops carrying checksummed pages this test still
  // passes while proving nothing — say so rather than go quietly green.
  EXPECT_GT(checksummed, 0u) << "fixture no longer exercises checksummed pages";

  // The superseded list and the map are two views of the same bytes and must
  // agree on how much of the file each relocated page occupied.
  uint64_t supTotal = 0, mapTotal = 0;
  for (const auto& r : rw.superseded) supTotal += r.len;
  for (const auto& r : rw.origMap)
    for (const auto& s : rw.superseded)
      if (r.origOff == s.off) mapTotal += r.origLen;
  EXPECT_EQ(mapTotal, supTotal);
}

// ---------------------------------------------------------------------------
// The cold replica run's RNTuple layout: pages served DECODED in slots of their
// uncompressed size. The file it describes must parse back -- envelopes,
// anchor checksum and all -- with every relocated record pointing at its slot
// as an uncompressed, checksum-less page, and every page must decode to the
// same bytes as in the original.
namespace {

struct BytesSource : Source {
  std::vector<uint8_t> b;
  bool has(uint64_t off, uint64_t n) override { return off + n >= off && off + n <= b.size(); }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    if (!has(off, n)) return false;
    std::memcpy(dst, b.data() + off, n);
    return true;
  }
};

std::vector<uint8_t> slurp(const std::string& path) {
  std::vector<uint8_t> out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

} // namespace

TEST(RNTupleFill, LayoutParsesBackAndServesTheSamePages) {
  RNTupleMeta m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  const auto orig = slurp(fixture());
  ASSERT_EQ(orig.size(), m.fileSize);
  const std::vector<uint8_t> header(orig.begin(), orig.begin() + 100);
  const std::string codec = rnTupleCodecName(m.ranges[0].compressionSettings);
  FillLayout L = layoutForRNTupleFill(m, m.fileSize, header, {codec});
  ASSERT_TRUE(L.error.empty()) << L.error;
  ASSERT_FALSE(L.slots.empty());
  EXPECT_EQ(L.originSize, m.fileSize);
  EXPECT_LE(L.metaSeek + L.metaRecord.size(), L.slotsBegin);

  // The file the reader is shown: original + windows, zeros, metadata, slots.
  BytesSource v;
  v.b = orig;
  for (const auto& w : L.windows) std::memcpy(v.b.data() + w.off, w.bytes.data(), w.bytes.size());
  v.b.resize(L.virtualSize, 0);
  std::memcpy(v.b.data() + L.metaSeek, L.metaRecord.data(), L.metaRecord.size());
  for (const auto& s : L.slots) {
    const auto& pg = m.ranges[s.branch].pages[s.basket];
    ConvertedPage c = convertPage(orig.data() + s.origSeek, s.origLen, pg.nbytes, pg.hasChecksum,
                                  pg.uncompressedBytes);
    ASSERT_TRUE(c.error.empty()) << c.error;
    ASSERT_EQ(c.raw.size() + kSlotChecksumBytes, s.vLen);
    std::memcpy(v.b.data() + s.vSeek, c.raw.data(), c.raw.size());
    sealDecodedPage(c.raw.data(), c.raw.size(), v.b.data() + s.vSeek + c.raw.size());
  }

  RNTupleMeta t = parseRNTuple(v, static_cast<int64_t>(v.b.size()), "");
  ASSERT_TRUE(t.error.empty()) << t.error; // envelope and anchor checksums hold
  ASSERT_EQ(t.ranges.size(), m.ranges.size());
  size_t relocatedPages = 0;
  for (uint32_t ri : L.relocated)
    for (const auto& pg : t.ranges[ri].pages) {
      EXPECT_EQ(pg.nbytes, pg.uncompressedBytes);
      EXPECT_TRUE(pg.hasChecksum); // every page a reader is served can be verified
      EXPECT_GE(pg.offset, L.slotsBegin);
      EXPECT_LE(pg.offset + pg.nbytes + kSlotChecksumBytes, L.virtualSize);
      // The checksum after the page is the decoded page's, as a reader checks it.
      EXPECT_EQ(convertPage(v.b.data() + pg.offset, pg.nbytes + kSlotChecksumBytes, pg.nbytes, true,
                            pg.uncompressedBytes).error, "");
      ++relocatedPages;
    }
  EXPECT_GT(relocatedPages, L.slots.size() - 1); // shared pages: records >= slots
  for (uint32_t ri : L.relocated) EXPECT_EQ(t.ranges[ri].compressionSettings, 0);

  FileSource src(::open(fixture().c_str(), O_RDONLY), m.fileSize);
  EXPECT_EQ(decodeAllPages(t, v), decodeAllPages(m, src));
}

// The first-pass layout lies column by column, each column's clusters in
// order: a reader's request for one cluster then spans pages of the columns it
// reads, with other clusters of those columns between them, not pages of
// columns it never reads.
TEST(RNTupleFill, SlotsLieColumnByColumn) {
  RNTupleMeta m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  const auto orig = slurp(fixture());
  const std::vector<uint8_t> header(orig.begin(), orig.begin() + 100);
  const std::string codec = rnTupleCodecName(m.ranges[0].compressionSettings);
  FillLayout L = layoutForRNTupleFill(m, m.fileSize, header, {codec});
  ASSERT_TRUE(L.error.empty()) << L.error;
  std::set<uint32_t> clusters, columns;
  for (uint32_t ri : L.relocated) {
    clusters.insert(m.ranges[ri].clusterId);
    columns.insert(m.ranges[ri].columnId);
  }
  ASSERT_GE(clusters.size(), 2u) << "the fixture needs two clusters";
  ASSERT_GE(columns.size(), 2u) << "and two columns";
  auto key = [&m](uint32_t ri) {
    return std::make_pair(m.ranges[ri].columnId, m.ranges[ri].clusterId);
  };
  for (size_t i = 1; i < L.relocated.size(); ++i)
    EXPECT_LT(key(L.relocated[i - 1]), key(L.relocated[i])) << i;
  for (size_t i = 1; i < L.slots.size(); ++i) {
    EXPECT_LT(L.slots[i - 1].vSeek, L.slots[i].vSeek) << i;
    EXPECT_LE(key(L.slots[i - 1].branch), key(L.slots[i].branch)) << i;
  }
}

// A compact map of the first-pass layout: its own page list and footer first
// in a range of its own, then every page of a wholly converted range as its
// record (ZSTD-1, or uncompressed) with the record's checksum; a range not
// wholly converted stays at its slots, decoded. The file it describes parses
// back -- anchor and envelope checksums included -- and every page decodes to
// the original's bytes.
TEST(RNTupleFill, ACompactMapParsesBackAndServesTheSamePages) {
  RNTupleMeta m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  const auto orig = slurp(fixture());
  const std::vector<uint8_t> header(orig.begin(), orig.begin() + 100);
  const std::string codec = rnTupleCodecName(m.ranges[0].compressionSettings);
  FillLayout L = layoutForRNTupleFill(m, m.fileSize, header, {codec});
  ASSERT_TRUE(L.error.empty()) << L.error;
  ASSERT_GE(L.relocated.size(), 2u) << "the fixture needs two relocated ranges";
  std::vector<std::vector<uint8_t>> enc(L.slots.size()), raw(L.slots.size());
  for (uint32_t i = 0; i < L.slots.size(); ++i) {
    const auto& s = L.slots[i];
    const auto& pg = m.ranges[s.branch].pages[s.basket];
    ConvertedPage c = convertPage(orig.data() + s.origSeek, s.origLen, pg.nbytes, pg.hasChecksum,
                                  pg.uncompressedBytes);
    ASSERT_TRUE(c.error.empty()) << c.error;
    enc[i] = c.enc;
    raw[i] = c.raw;
  }
  for (const bool partial : {false, true}) {
    // partial: the first relocated range has a page with no record yet.
    const uint32_t held = L.relocated[0];
    auto recLen = [&](uint32_t i) -> uint32_t {
      return partial && L.slots[i].branch == held && L.slots[i].basket == 0
                 ? 0
                 : static_cast<uint32_t>(enc[i].size());
    };
    const uint64_t base = (L.virtualSize + 8191) / 4096 * 4096; // a guard of a page here
    RNTupleCompactMap cm = rnTupleCompactMap(m, L, base, recLen);
    ASSERT_TRUE(cm.error.empty()) << cm.error;
    ASSERT_LE(cm.meta.size(), cm.metaReserve);
    // The file a handle shown it reads: windows (its anchor, a header for its
    // end), map 1's slots decoded, its range.
    BytesSource v;
    v.b = orig;
    std::vector<uint8_t> hw;
    uint64_t hwOff = 0;
    std::string err;
    ASSERT_TRUE(headerWindowForEnd(header, cm.end, hwOff, hw, err)) << err;
    for (const auto& w : L.windows)
      std::memcpy(v.b.data() + w.off, w.bytes.data(), w.bytes.size());
    std::memcpy(v.b.data() + hwOff, hw.data(), hw.size());
    std::memcpy(v.b.data() + cm.anchor.off, cm.anchor.bytes.data(), cm.anchor.bytes.size());
    v.b.resize(cm.end, 0);
    for (uint32_t i = 0; i < L.slots.size(); ++i) {
      std::memcpy(v.b.data() + L.slots[i].vSeek, raw[i].data(), raw[i].size());
      sealDecodedPage(raw[i].data(), raw[i].size(), v.b.data() + L.slots[i].vSeek + raw[i].size());
    }
    std::memcpy(v.b.data() + base, cm.meta.data(), cm.meta.size());
    std::set<uint32_t> compactRanges;
    for (const auto& [i, at] : cm.pieces) {
      std::memcpy(v.b.data() + at, enc[i].data(), enc[i].size());
      sealDecodedPage(enc[i].data(), enc[i].size(), v.b.data() + at + enc[i].size());
      compactRanges.insert(L.slots[i].branch);
    }
    EXPECT_EQ(compactRanges.count(held), partial ? 0u : 1u);
    EXPECT_EQ(cm.rangesCompact, compactRanges.size());
    RNTupleMeta t = parseRNTuple(v, static_cast<int64_t>(v.b.size()), "");
    ASSERT_TRUE(t.error.empty()) << t.error;
    for (uint32_t ri : L.relocated) {
      const bool compact = compactRanges.count(ri) > 0;
      EXPECT_EQ(t.ranges[ri].compressionSettings, compact ? 501 : 0) << ri;
      for (const auto& pg : t.ranges[ri].pages) {
        EXPECT_TRUE(pg.hasChecksum);
        if (compact)
          EXPECT_GE(pg.offset, base + cm.metaReserve);
        else
          EXPECT_LT(pg.offset, L.virtualSize); // its slot
        EXPECT_LE(pg.offset + pg.nbytes + kSlotChecksumBytes, cm.end);
      }
    }
    FileSource src(::open(fixture().c_str(), O_RDONLY), m.fileSize);
    EXPECT_EQ(decodeAllPages(t, v), decodeAllPages(m, src)) << (partial ? "partial" : "whole");
  }
}

TEST(RNTupleFill, PublishFromConvertedPagesMatchesTheRewrite) {
  RNTupleMeta m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  const auto orig = slurp(fixture());
  int fd = ::open(fixture().c_str(), O_RDONLY);
  FileSource src(fd, m.fileSize);
  RNTupleRewrite ref = buildRNTupleRewrite(m, src, m.fileSize, 1, {});
  ::close(fd);
  ASSERT_TRUE(ref.error.empty()) << ref.error;
  RNTupleRewrite got = buildRNTupleRewriteFromPages(
      m, m.fileSize, 1, [](size_t, size_t) { return true; },
      [&](size_t ri, size_t pi, std::vector<uint8_t>& enc) {
        const auto& pg = m.ranges[ri].pages[pi];
        ConvertedPage c = convertPage(orig.data() + pg.offset, pg.nbytes + (pg.hasChecksum ? 8 : 0),
                                      pg.nbytes, pg.hasChecksum, pg.uncompressedBytes);
        enc = c.enc;
        return c.error.empty();
      });
  ASSERT_TRUE(got.error.empty()) << got.error;
  EXPECT_EQ(got.extension, ref.extension);
  ASSERT_EQ(got.patches.size(), ref.patches.size());
  for (size_t i = 0; i < got.patches.size(); ++i) {
    EXPECT_EQ(got.patches[i].offset, ref.patches[i].offset);
    EXPECT_EQ(got.patches[i].bytes, ref.patches[i].bytes);
  }
  EXPECT_EQ(got.superseded.size(), ref.superseded.size());
  EXPECT_EQ(got.rangesRelocated, ref.rangesRelocated);

  // A range with a page the stage does not hold stays where it was.
  RNTupleRewrite part = buildRNTupleRewriteFromPages(
      m, m.fileSize, 1, [](size_t ri, size_t) { return ri != 0; },
      [&](size_t ri, size_t pi, std::vector<uint8_t>& enc) {
        const auto& pg = m.ranges[ri].pages[pi];
        enc = convertPage(orig.data() + pg.offset, pg.nbytes + (pg.hasChecksum ? 8 : 0), pg.nbytes,
                          pg.hasChecksum, pg.uncompressedBytes)
                  .enc;
        return true;
      });
  ASSERT_TRUE(part.error.empty()) << part.error;
  EXPECT_EQ(part.rangesRelocated, ref.rangesRelocated - 1);
  EXPECT_EQ(part.rangesUncached, 1u);
}

TEST(RNTupleFill, DamagedPageIsNeverServed) {
  RNTupleMeta m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  auto orig = slurp(fixture());
  const PageInfo* pg = nullptr;
  for (const auto& r : m.ranges)
    for (const auto& p : r.pages)
      if (p.hasChecksum && p.nbytes > 4 && !pg) pg = &p;
  ASSERT_NE(pg, nullptr) << "the fixture's pages carry checksums";
  ConvertedPage ok = convertPage(orig.data() + pg->offset, pg->nbytes + 8, pg->nbytes, true,
                                 pg->uncompressedBytes);
  EXPECT_TRUE(ok.error.empty()) << ok.error;
  orig[pg->offset + 1] ^= 0x40;
  ConvertedPage bad = convertPage(orig.data() + pg->offset, pg->nbytes + 8, pg->nbytes, true,
                                  pg->uncompressedBytes);
  EXPECT_FALSE(bad.error.empty());
  EXPECT_TRUE(bad.raw.empty());
}

TEST(RNTupleFill, Declines) {
  RNTupleMeta m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  const auto orig = slurp(fixture());
  const std::vector<uint8_t> header(orig.begin(), orig.begin() + 100);
  FillLayout unlisted = layoutForRNTupleFill(m, m.fileSize, header, {"lz4"});
  EXPECT_NE(unlisted.error.find("its pages are zstd"), std::string::npos) << unlisted.error;
  EXPECT_TRUE(unlisted.codecDecline);
  FillLayout past = layoutForRNTupleFill(m, m.fileSize + 1, header,
                                         {rnTupleCodecName(m.ranges[0].compressionSettings)});
  EXPECT_FALSE(past.error.empty());
  EXPECT_FALSE(past.codecDecline);
}

// A compression setting that names no codec: `hadd -f1` records setting 1 --
// algorithm 0, "the global default" -- on every column range of the merged
// file, and the pages are ZLIB. The committed fixture is exactly that: two
// copies of make_rntuple_fixture.C's tree written with 101, merged with
// `hadd -f1 rntuple_unnamed_codec_fixture.root in.root in.root`.
namespace {
std::string unnamedFixture() {
  return std::string(UCACHE_TEST_DATA_DIR) + "/rntuple_unnamed_codec_fixture.root";
}
struct CountingFileSource : FileSource {
  using FileSource::FileSource;
  int reads = 0;
  bool read(void* dst, uint64_t n, uint64_t off) override {
    ++reads;
    return FileSource::read(dst, n, off);
  }
};
} // namespace

// The one read that names an unnamed setting's codec fails: that is not a
// verdict for any range -- the sweep leaves every such range for next time
// rather than declining the later ones for an empty codec.
TEST(RNTupleFill, AFailedCodecReadDeclinesNoRange) {
  struct FailingSource : FileSource {
    using FileSource::FileSource;
    bool read(void*, uint64_t, uint64_t) override { return false; }
  };
  RNTupleMeta m = parseRNTuple(unnamedFixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  ASSERT_GT(m.ranges.size(), 1u) << "the case needs more than one unnamed range";
  int fd = ::open(unnamedFixture().c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  FailingSource src(fd, m.fileSize);
  RNTupleRewrite rw = buildRNTupleRewrite(m, src, m.fileSize, 1, {"lzma", "zlib"});
  ::close(fd);
  EXPECT_EQ(rw.rangesRelocated, 0u);
  EXPECT_TRUE(rw.declinedCodec.empty()) << rw.declinedCodec;
  EXPECT_TRUE(rw.transient);
}

TEST(RNTupleFill, UnnamedSettingTakesThePagesCodec) {
  EXPECT_EQ(rnTupleCodecName(0), "none");
  EXPECT_EQ(rnTupleCodecName(1), ""); // the global default: named by the pages
  EXPECT_EQ(rnTupleCodecName(505), "zstd");

  RNTupleMeta m = parseRNTuple(unnamedFixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  ASSERT_FALSE(m.ranges.empty());
  size_t compressed = 0;
  for (const auto& r : m.ranges) {
    EXPECT_EQ(r.compressionSettings, 1);
    for (const auto& pg : r.pages) compressed += pg.nbytes < pg.uncompressedBytes;
  }
  ASSERT_GT(compressed, 0u) << "the fixture's pages are compressed";
  const auto orig = slurp(unnamedFixture());
  const std::vector<uint8_t> header(orig.begin(), orig.begin() + 100);

  int fd = ::open(unnamedFixture().c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  CountingFileSource src(fd, m.fileSize);
  std::string codec;
  ASSERT_TRUE(unnamedRNTupleCodec(m, src, codec));
  EXPECT_EQ(codec, "zlib");
  EXPECT_EQ(src.reads, 1);

  // The first pass converts the file...
  FillLayout L = layoutForRNTupleFill(m, m.fileSize, header, {"lzma", "zlib"}, codec);
  ASSERT_TRUE(L.error.empty()) << L.error;
  EXPECT_EQ(L.relocated.size(), m.ranges.size());
  for (const auto& s : L.slots) { // every page decodes into its slot
    const auto& pg = m.ranges[s.branch].pages[s.basket];
    ConvertedPage c = convertPage(orig.data() + s.origSeek, s.origLen, pg.nbytes, pg.hasChecksum,
                                  pg.uncompressedBytes);
    ASSERT_TRUE(c.error.empty()) << c.error;
  }
  // ...keeps recompress_codecs' meaning...
  FillLayout unlisted = layoutForRNTupleFill(m, m.fileSize, header, {"lzma"}, codec);
  EXPECT_NE(unlisted.error.find("its pages are zlib"), std::string::npos) << unlisted.error;
  EXPECT_TRUE(unlisted.codecDecline);
  // ...and without the pages' codec it would decline the file whole.
  FillLayout blind = layoutForRNTupleFill(m, m.fileSize, header, {"lzma", "zlib"});
  EXPECT_NE(blind.error.find("not converted"), std::string::npos) << blind.error;

  // A sweep decides the same.
  RNTupleRewrite rw = buildRNTupleRewrite(m, src, m.fileSize, 1, {"lzma", "zlib"});
  ASSERT_TRUE(rw.error.empty()) << rw.error;
  EXPECT_EQ(rw.rangesRelocated, m.ranges.size());
  RNTupleRewrite no = buildRNTupleRewrite(m, src, m.fileSize, 1, {"lzma"});
  EXPECT_EQ(no.rangesRelocated, 0u);
  EXPECT_EQ(no.declinedCodec, "zlib");
  ::close(fd);

  // A range whose pages are all stored as they are is "none", whatever codec
  // the file's other pages are in.
  ColumnRange raw;
  raw.compressionSettings = 1;
  PageInfo pg;
  pg.nbytes = 64;
  pg.uncompressedBytes = 64;
  raw.pages.push_back(pg);
  EXPECT_EQ(rangeCodec(raw, "zlib"), "none");
  raw.pages[0].nbytes = 40;
  EXPECT_EQ(rangeCodec(raw, "zlib"), "zlib");
  raw.compressionSettings = 207;
  EXPECT_EQ(rangeCodec(raw, "zlib"), "lzma"); // a named setting is taken as it is
}

// A file whose settings name their codecs reads nothing for this.
TEST(RNTupleFill, NamedSettingsReadNothing) {
  RNTupleMeta m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  int fd = ::open(fixture().c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  CountingFileSource src(fd, m.fileSize);
  std::string codec = "x";
  ASSERT_TRUE(unnamedRNTupleCodec(m, src, codec));
  EXPECT_EQ(codec, "");
  EXPECT_EQ(src.reads, 0);
  ::close(fd);
}

// ---------------------------------------------------------------------------
// Read back in the ORIGINAL file's coordinates. A run records how many bytes
// of each original file it read (orig_bytes, unique_bytes), and those counts
// must be right or absent. A page read whole is its original page. A read that
// takes PART of a relocated page -- a reader's buffered read of the footer runs
// on into the pages beside it -- has no original bytes to give: counting the
// page whole is what made these counts 2.3x too high on compact replicas. The
// 1 MiB buckets of the read signature still take every page touched, whole,
// exactly as before.
namespace {

using Spans = std::vector<std::pair<uint64_t, uint64_t>>;

// The reader's reads of a file as it sees it: header block, footer, page list,
// then every page of every column range, whole (block + checksum).
Spans readerReads(const RNTupleMeta& t) {
  Spans r;
  r.emplace_back(0, std::min<uint64_t>(t.fileSize, 4096));
  r.emplace_back(t.anchor.seekFooter, t.anchor.nbytesFooter);
  r.emplace_back(t.pageListOffset, t.pageListNbytes);
  for (const auto& range : t.ranges)
    for (const auto& pg : range.pages)
      r.emplace_back(pg.offset, (uint64_t)pg.nbytes + (pg.hasChecksum ? 8 : 0));
  return r;
}

struct CompactReplica {
  RNTupleMeta m, seen; // the original, and the file as the replica shows it
  ucache::test::TempDir td;
  ucache::RealIO io;
  ucache::Config cfg;
  ucache::Stats stats;
  std::unique_ptr<ucache::CacheStore> store;
  std::unique_ptr<ucache::ReplicaStore> rs;
  std::shared_ptr<ucache::ReplicaView> view;
  std::string error;

  CompactReplica() {
    m = parseRNTuple(fixture(), "");
    if (!m.error.empty()) {
      error = m.error;
      return;
    }
    int fd = ::open(fixture().c_str(), O_RDONLY | O_CLOEXEC);
    FileSource src(fd, m.fileSize);
    auto rw = buildRNTupleRewrite(m, src, m.fileSize, 1);
    auto ov = rnTupleOverlay(m, rw);
    BytesSource v;
    v.b.assign(ov.meta.virtualSize, 0);
    const bool readOk = ::pread(fd, v.b.data(), m.fileSize, 0) == (ssize_t)m.fileSize;
    ::close(fd);
    if (!ov.error.empty() || !readOk) {
      error = ov.error.empty() ? "cannot read the fixture" : ov.error;
      return;
    }
    for (const auto& e : ov.meta.extents)
      std::memcpy(v.b.data() + e.virtOff, ov.tdata.data() + e.tdataOff, e.len);
    seen = parseRNTuple(v, static_cast<int64_t>(v.b.size()), "");
    cfg.cacheDir = td.path();
    store = std::make_unique<ucache::CacheStore>(io, cfg);
    store->disableStatsDump();
    rs = std::make_unique<ucache::ReplicaStore>(io, cfg, stats);
    const auto key = *ucache::UrlKey::parse("root://origin//data/rntuple.root");
    if (rs->publish(key, ov.meta, ov.tdata.data(), ov.tdata.size()) != 0) {
      error = "publish failed";
      return;
    }
    view = rs->openView(key, m.fileSize);
    if (!view || !view->hasOriginMap())
      error = "no view with an origin map";
  }

  // A read of the replica, into `f` as the plugin notes it, and into `whole`
  // as it was noted before: every unit touched, counted whole.
  bool read(uint64_t off, uint64_t len, ucache::ReadFootprint& f, ucache::ReadFootprint& whole) {
    Spans units, exact;
    const bool known = view->mapToOrigin(off, len, units, exact);
    f.noteMapped(units, known ? &exact : nullptr, m.fileSize);
    std::vector<ucache::ReplicaMeta::Range> old;
    view->originRanges(off, len, old);
    EXPECT_EQ(old.size(), units.size()) << "the units are what the signature always took";
    for (size_t i = 0; i < old.size() && i < units.size(); ++i) {
      EXPECT_EQ(units[i].first, old[i].off);
      EXPECT_EQ(units[i].second, old[i].len);
    }
    for (const auto& r : old)
      whole.note(r.off, r.len, m.fileSize);
    return known;
  }
};

} // namespace

TEST(RNTupleReplicaBytes, WholeReadsCountTheOriginalBytesTheByteRouteCounts) {
  CompactReplica c;
  ASSERT_TRUE(c.error.empty()) << c.error;
  ASSERT_TRUE(c.seen.error.empty()) << c.seen.error;
  const Spans onReplica = readerReads(c.seen);
  const Spans onOriginal = readerReads(c.m);
  ASSERT_EQ(onReplica.size(), onOriginal.size());

  ucache::ReadFootprint replica, whole, byteRoute;
  size_t relocated = 0;
  for (size_t i = 0; i < onReplica.size(); ++i) {
    EXPECT_TRUE(c.read(onReplica[i].first, onReplica[i].second, replica, whole))
        << "read " << i << " at " << onReplica[i].first << " is whole, so it maps exactly";
    byteRoute.note(onOriginal[i].first, onOriginal[i].second, c.m.fileSize);
    relocated += onReplica[i].first != onOriginal[i].first;
  }
  ASSERT_GT(relocated, 0u) << "the fixture's pages must have been relocated";
  const auto t = replica.totals(c.m.fileSize);
  const auto b = byteRoute.totals(c.m.fileSize);
  ASSERT_TRUE(t.origKnown);
  ASSERT_TRUE(t.uniqueKnown);
  ASSERT_TRUE(b.origKnown);
  EXPECT_EQ(t.origBytes, b.origBytes);
  EXPECT_EQ(t.uniqueBytes, b.uniqueBytes);
  EXPECT_EQ(replica.sig(c.m.fileSize), whole.sig(c.m.fileSize));
  EXPECT_EQ(replica.sig(c.m.fileSize), byteRoute.sig(c.m.fileSize));
}

TEST(RNTupleReplicaBytes, AReadIntoPartOfARelocatedPageLeavesTheCountsUnknown) {
  CompactReplica c;
  ASSERT_TRUE(c.error.empty()) << c.error;
  // The page the extension holds last, before the page list and footer: a
  // buffered read of the footer region that starts inside it.
  const ucache::ReplicaMeta::OrigRange* last = nullptr;
  for (const auto& r : c.view->meta().origMap)
    if (r.virtOff != r.origOff && r.len > 16 && r.origOff != c.m.anchor.seekFooter &&
        r.origOff != c.m.pageListOffset && (!last || r.virtOff > last->virtOff))
      last = &r;
  ASSERT_NE(last, nullptr);
  const uint64_t from = last->virtOff + last->len / 2;
  const uint64_t len = c.view->virtualSize() - from;

  ucache::ReadFootprint f, whole;
  f.note(0, 4096, c.m.fileSize); // what came before is known ...
  whole.note(0, 4096, c.m.fileSize);
  ASSERT_TRUE(f.totals(c.m.fileSize).origKnown);
  EXPECT_FALSE(c.read(from, len, f, whole));
  // ... and is not the answer any more: the counts are unknown, not a guess.
  const auto t = f.totals(c.m.fileSize);
  EXPECT_FALSE(t.origKnown);
  EXPECT_FALSE(t.uniqueKnown);
  // What the whole-unit count would have said: the whole page, for a part of it.
  const auto w = whole.totals(c.m.fileSize);
  ASSERT_TRUE(w.origKnown);
  EXPECT_GE(w.origBytes, 4096 + last->origLen);
  // The signature is the one it always was.
  EXPECT_FALSE(f.poisoned());
  EXPECT_EQ(f.sig(c.m.fileSize), whole.sig(c.m.fileSize));
  EXPECT_EQ(f.count(c.m.fileSize), whole.count(c.m.fileSize));
}

TEST(RNTupleReplicaBytes, InPlaceBytesMapByteForByte) {
  // The anchor is patched in place: any part of it is the same part of the
  // original. Bytes the overlay does not cover are the original's own.
  CompactReplica c;
  ASSERT_TRUE(c.error.empty()) << c.error;
  const ucache::ReplicaMeta::OrigRange* win = nullptr;
  for (const auto& r : c.view->meta().origMap)
    if (r.virtOff == r.origOff && r.len == r.origLen && r.len > 2)
      win = &r;
  ASSERT_NE(win, nullptr) << "the fixture has a window patched in place";
  Spans units, exact;
  ASSERT_TRUE(c.view->mapToOrigin(win->virtOff + 1, win->len - 2, units, exact));
  ASSERT_EQ(exact.size(), 1u);
  EXPECT_EQ(exact[0], std::make_pair(win->virtOff + 1, win->len - 2));
  units.clear();
  exact.clear();
  ASSERT_TRUE(c.view->mapToOrigin(0, 64, units, exact)); // the file header
  ASSERT_FALSE(exact.empty());
  uint64_t n = 0;
  for (const auto& e : exact)
    n += e.second;
  EXPECT_EQ(n, 64u);
}

// The slot store's layout for RNTuple: a page's slot read whole is its page; a
// read of the page list or footer alone is part of the relocated metadata
// record, which stands for both, and a read into part of a slot has no
// original bytes either.
TEST(RNTupleReplicaBytes, SlotLayoutMapsWholeSlotsAndRefusesParts) {
  RNTupleMeta m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  const auto orig = slurp(fixture());
  const std::vector<uint8_t> header(orig.begin(), orig.begin() + 100);
  FillLayout L = layoutForRNTupleFill(m, m.fileSize, header,
                                      {rnTupleCodecName(m.ranges[0].compressionSettings)});
  ASSERT_TRUE(L.error.empty()) << L.error;
  ASSERT_FALSE(L.slots.empty());
  const Spans metaOrigin{{m.pageListOffset, m.pageListNbytes},
                         {m.anchor.seekFooter, m.anchor.nbytesFooter}};
  Spans out;
  for (const auto& s : L.slots) {
    out.clear();
    ASSERT_TRUE(exactOriginRanges(L, metaOrigin, s.vSeek, s.vLen, out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], std::make_pair(s.origSeek, (uint64_t)s.origLen));
    out.clear();
    EXPECT_FALSE(exactOriginRanges(L, metaOrigin, s.vSeek, s.vLen - 1, out));
  }
  // Two whole slots in one read, and the zeros before them.
  if (L.slots.size() >= 2) {
    out.clear();
    const auto& a = L.slots[0];
    const auto& b = L.slots[1];
    ASSERT_TRUE(exactOriginRanges(L, metaOrigin, L.metaSeek + L.metaRecord.size(),
                                  b.vSeek + b.vLen - (L.metaSeek + L.metaRecord.size()), out));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].first, a.origSeek);
    EXPECT_EQ(out[1].first, b.origSeek);
  }
  out.clear();
  ASSERT_TRUE(exactOriginRanges(L, metaOrigin, L.metaSeek, L.metaRecord.size(), out));
  EXPECT_EQ(out, metaOrigin) << "the metadata record read whole stands for both";
  out.clear();
  EXPECT_FALSE(exactOriginRanges(L, metaOrigin, L.metaSeek + 1, 16, out));
  out.clear();
  ASSERT_TRUE(exactOriginRanges(L, metaOrigin, 0, 100, out)); // the header, in place
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], std::make_pair(uint64_t(0), uint64_t(100)));
}
