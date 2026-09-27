// The truncated-copy guard (CopyGuard.h): a handle that reads all of
// [0, origin size) of a relocated layout, without first reading the part past
// it, is a copy sized from the origin, and the request that completes it is
// refused.
#include "CopyGuard.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace ucache;

namespace {
constexpr uint64_t kSize = 961844;  // any size that is not a multiple of a chunk
constexpr uint64_t kShown = 1884200; // the layout the handle is shown, larger
} // namespace

TEST(CopyGuard, ASequentialCopyIsRefusedAtTheRequestThatCompletesIt) {
  CopyGuard g;
  uint64_t off = 0;
  const uint64_t chunk = 1 << 18;
  while (off + chunk < kSize) {
    EXPECT_TRUE(g.allow(kSize, kShown, off, chunk)) << off;
    off += chunk;
  }
  EXPECT_FALSE(g.allow(kSize, kShown, off, kSize - off)); // the last piece
  EXPECT_FALSE(g.allow(kSize, kShown, 0, 4096));          // and it stays refused
}

TEST(CopyGuard, ACopyWhoseLastPieceRunsPastTheOriginsSizeIsRefused) {
  // fsspec's block cache, or a loop that does not clamp its last piece: the
  // last block starts below the origin's size and ends past it.
  for (const uint64_t chunk : {uint64_t(1) << 17, uint64_t(1) << 18, uint64_t(5) << 20}) {
    CopyGuard g;
    uint64_t off = 0;
    while (off + chunk < kSize) {
      EXPECT_TRUE(g.allow(kSize, kShown, off, chunk)) << chunk << " " << off;
      off += chunk;
    }
    if (off == 0)
      continue; // one block holds the whole file: a first request, decided elsewhere
    EXPECT_FALSE(g.allow(kSize, kShown, off, chunk)) << chunk;
  }
}

TEST(CopyGuard, OneOriginSizedReadIsRefusedAtAnyTime) {
  CopyGuard g;
  EXPECT_FALSE(g.allow(kSize, kShown, 0, kSize));
  CopyGuard reader; // even on a handle a reader already used
  EXPECT_TRUE(reader.allow(kSize, kShown, kSize + 100, 900));
  EXPECT_FALSE(reader.allow(kSize, kShown, 0, kSize));
  EXPECT_TRUE(reader.allow(kSize, kShown, 0, 4096)); // and the reader goes on
}

TEST(CopyGuard, TheWholeLayoutInOneRequestIsAllowed) {
  CopyGuard g; // a valid file, if not the origin's
  EXPECT_TRUE(g.allow(kSize, kShown, 0, 300));
  EXPECT_TRUE(g.allow(kSize, kShown, 0, kShown));
  EXPECT_TRUE(g.allow(kSize, kShown, 300, kSize - 300));
}

TEST(CopyGuard, OutOfOrderAndOverlappingPiecesCompleteTheCopyToo) {
  CopyGuard g;
  EXPECT_TRUE(g.allow(kSize, kShown, 500000, 300000));
  EXPECT_TRUE(g.allow(kSize, kShown, 0, 200000));
  EXPECT_TRUE(g.allow(kSize, kShown, 150000, 400000)); // overlaps both
  EXPECT_FALSE(g.allow(kSize, kShown, 790000, kSize - 790000));
}

TEST(CopyGuard, AReaderOfThePartPastTheOriginsSizeIsNeverRefusedAPiece) {
  CopyGuard g;
  EXPECT_TRUE(g.allow(kSize, kShown, 0, 300));          // the header, as ROOT and uproot read it
  EXPECT_TRUE(g.allow(kSize, kShown, kSize + 100, 900)); // the relocated tree record
  for (uint64_t off = 0; off < kSize; off += 65536)     // then all of the original region
    EXPECT_TRUE(g.allow(kSize, kShown, off, 65536));
}

TEST(CopyGuard, ABlockIntoThePartPastTheEndThatCompletesNothingStopsTheTracking) {
  CopyGuard g; // ROOT's raw-file layer: 128 KiB blocks, one holding the relocated metadata
  EXPECT_TRUE(g.allow(kSize, kShown, 0, 131072));
  EXPECT_TRUE(g.allow(kSize, kShown, kSize - 1000, 131072));
  for (uint64_t off = 131072; off < kSize; off += 65536)
    EXPECT_TRUE(g.allow(kSize, kShown, off, 65536));
}

TEST(CopyGuard, AChunkStartingPastTheEndInAVectorStopsTheTracking) {
  CopyGuard g;
  const CopyGuard::Range v[] = {{0, 4096}, {kSize, 64}};
  EXPECT_TRUE(g.allowRanges(kSize, kShown, v, 2));
  EXPECT_TRUE(g.allow(kSize, kShown, 4096, kSize - 4096));
}

TEST(CopyGuard, AVectorThatCompletesTheCopyIsRefused) {
  CopyGuard g;
  EXPECT_TRUE(g.allow(kSize, kShown, 0, 400000));
  const CopyGuard::Range v[] = {{400000, 300000}, {700000, kSize - 700000}};
  EXPECT_FALSE(g.allowRanges(kSize, kShown, v, 2));
}

TEST(CopyGuard, ScatteredReadsStopTheTracking) {
  CopyGuard g;
  // More disjoint ranges than a copy would ever leave: a random-access reader.
  for (size_t i = 0; i <= CopyGuard::kMaxRuns; ++i)
    EXPECT_TRUE(g.allow(kSize, kShown, i * 8192, 100));
  EXPECT_TRUE(g.allow(kSize, kShown, 0, kSize - 1)); // tracking is off for good
}

TEST(CopyGuard, ZeroLengthPiecesCountForNothing) {
  CopyGuard g;
  EXPECT_TRUE(g.allow(kSize, kShown, 0, 0));
  EXPECT_TRUE(g.allow(kSize, kShown, kSize + 10, 0)); // not a read past the end either
  EXPECT_TRUE(g.allow(kSize, kShown, 0, kSize - 10));
  EXPECT_FALSE(g.allow(kSize, kShown, kSize - 10, 10));
}

TEST(CopyGuard, AnIncompleteCopyIsAllowed) {
  CopyGuard g; // a copy stopped short, or a reader of the first part only
  EXPECT_TRUE(g.allow(kSize, kShown, 0, kSize - 1));
  EXPECT_TRUE(g.allow(kSize, kShown, 0, kSize - 1));
}

TEST(CopyGuard, ALoopSizedByTheHandlesOwnStatReadsOnPastTheOriginsSize) {
  CopyGuard g; // it was told the layout's size: its last piece below it is not the last
  g.sizeShown();
  uint64_t off = 0;
  const uint64_t chunk = 1 << 18;
  for (; off < kShown; off += chunk)
    EXPECT_TRUE(g.allow(kSize, kShown, off, std::min(chunk, kShown - off))) << off;
  CopyGuard clamped; // but pieces that stop at the origin's size are still that copy
  clamped.sizeShown();
  for (off = 0; off + chunk < kSize; off += chunk)
    EXPECT_TRUE(clamped.allow(kSize, kShown, off, chunk));
  EXPECT_FALSE(clamped.allow(kSize, kShown, off, kSize - off));
}
