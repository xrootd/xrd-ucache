// The origin-integrity checks on compressed objects and basket key headers,
// pinned against blocks ROOT itself wrote: a check that is wrong but
// self-consistent would pass our own round trip and still fail on real data.
#include "FillLayout.h"
#include "TreeMeta.h"
#include "vendor/xxh3.h"

#include <gtest/gtest.h>
#include <lzma.h>
#include <zstd.h>

#include <cstring>
#include <tuple>
#include <vector>

using namespace ucache::transpose;

namespace {

// Written by ROOT 6.36.02 with PyROOT: a one-branch TTree per codec (compression
// 207, 101, 505, 404), the first basket of branch x, x[i] = (i % 7) * 0.5.
const uint64_t kLzmaObjlen = 8000;
const uint8_t kLzmaPayload[] = {
    0x58, 0x5a, 0x00, 0x74, 0x00, 0x00, 0x40, 0x1f, 0x00, 0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00, 0x00,
    0x01, 0x69, 0x22, 0xde, 0x36, 0x02, 0x00, 0x21, 0x01, 0x00, 0x00, 0x00, 0x00, 0x37, 0x27, 0x97,
    0xd6, 0xe0, 0x1f, 0x3f, 0x00, 0x38, 0x5d, 0x00, 0x00, 0x68, 0x8e, 0x3f, 0xcd, 0x79, 0x6e, 0xd0,
    0x48, 0x32, 0x56, 0xad, 0x6a, 0xbf, 0x3d, 0xbe, 0x1b, 0xf3, 0x3c, 0x06, 0x97, 0xe1, 0x55, 0x62,
    0x50, 0xad, 0xbe, 0xc8, 0x7c, 0x95, 0x0b, 0xc0, 0x23, 0x2a, 0xec, 0x11, 0x89, 0xc3, 0x43, 0xfc,
    0x34, 0xc1, 0xb0, 0x1f, 0xe1, 0x4d, 0xdb, 0xc8, 0x3e, 0x91, 0xb4, 0x69, 0x6b, 0xae, 0x93, 0x00,
    0x00, 0x67, 0xa4, 0x31, 0x01, 0x00, 0x01, 0x50, 0xc0, 0x3e, 0x00, 0x00, 0x00, 0x63, 0xbb, 0x3a,
    0x36, 0x3e, 0x30, 0x0d, 0x8b, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x59, 0x5a,
};
const uint64_t kZlibObjlen = 8000;
const uint8_t kZlibPayload[] = {
    0x5a, 0x4c, 0x08, 0x73, 0x00, 0x00, 0x40, 0x1f, 0x00, 0x78, 0x01, 0xed, 0xd9, 0xd1, 0x09, 0xc0,
    0x20, 0x0c, 0x05, 0xc0, 0x37, 0x4a, 0x37, 0x69, 0x46, 0x73, 0xb4, 0x8e, 0xd6, 0x97, 0x35, 0xe4,
    0x84, 0x80, 0xe0, 0x5f, 0x30, 0x47, 0x34, 0x49, 0xf2, 0x6e, 0x9c, 0xc6, 0x97, 0x4c, 0xf7, 0xf3,
    0x34, 0x76, 0xd3, 0xe5, 0x4c, 0x5e, 0xa6, 0xf7, 0xc0, 0x9d, 0xd8, 0x6a, 0x50, 0x0f, 0x9c, 0x60,
    0x24, 0x0b, 0x58, 0xa0, 0x2f, 0xd0, 0x17, 0x4c, 0xcb, 0x80, 0x05, 0x2c, 0x60, 0x01, 0x0b, 0x58,
    0xe0, 0xcd, 0xbc, 0x12, 0xb2, 0x80, 0x05, 0x2c, 0x60, 0x01, 0x0b, 0xea, 0xc0, 0x61, 0x01, 0x0b,
    0x58, 0xc0, 0x02, 0x16, 0x98, 0x2d, 0xf9, 0x2b, 0x31, 0x53, 0xac, 0x03, 0xc5, 0x50, 0x5f, 0xd0,
    0x5a, 0x68, 0x1e, 0x6e, 0xf8, 0x3b, 0xfc, 0x01, 0x60, 0xef, 0x79, 0xb4,
};
const uint64_t kZstdObjlen = 8000;
const uint8_t kZstdPayload[] = {
    0x5a, 0x53, 0x01, 0x2b, 0x00, 0x00, 0x40, 0x1f, 0x00, 0x28, 0xb5, 0x2f, 0xfd, 0x60, 0x40, 0x1e,
    0x0d, 0x01, 0x00, 0xa8, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00, 0x3f, 0xc0, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x40, 0x20, 0x00, 0x00, 0x40, 0x03, 0x00, 0x20, 0xff, 0xb3, 0x01, 0x16,
    0x5e, 0xaf, 0x4e, 0x0b,
};
const uint64_t kLz4Objlen = 8000;
const uint8_t kLz4Payload[] = {
    0x4c, 0x34, 0x01, 0x4d, 0x00, 0x00, 0x40, 0x1f, 0x00, 0x2b, 0x5f, 0x17, 0xc3, 0xa2, 0x5d, 0x09,
    0x8b, 0x50, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x04, 0x00, 0xf0, 0x01, 0x80, 0x00, 0x00, 0x3f, 0xc0,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x20, 0x00, 0x00, 0x40, 0x09, 0x00, 0x0f, 0x1c, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x2a,
    0x50, 0x00, 0x40, 0x00, 0x00, 0x00,
};

// The whole first basket record of x in the LZMA file (key header + payload),
// written by ROOT 6.36.02 at file offset 300.
const uint8_t kLzmaRecord[] = {
    0x00, 0x00, 0x00, 0xc3, 0x03, 0xec, 0x00, 0x00, 0x1f, 0x40, 0x7e, 0x89, 0x63, 0x06, 0x00, 0x46,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x64, 0x07, 0x54, 0x42, 0x61, 0x73, 0x6b, 0x65, 0x74, 0x01, 0x78, 0x06, 0x45, 0x76, 0x65,
    0x6e, 0x74, 0x73, 0x00, 0x03, 0x00, 0x00, 0xfa, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x07,
    0xd0, 0x00, 0x00, 0x1f, 0x86, 0x00, 0x58, 0x5a, 0x00, 0x74, 0x00, 0x00, 0x40, 0x1f, 0x00, 0xfd,
    0x37, 0x7a, 0x58, 0x5a, 0x00, 0x00, 0x01, 0x69, 0x22, 0xde, 0x36, 0x02, 0x00, 0x21, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x37, 0x27, 0x97, 0xd6, 0xe0, 0x1f, 0x3f, 0x00, 0x38, 0x5d, 0x00, 0x00, 0x68,
    0x8e, 0x3f, 0xcd, 0x79, 0x6e, 0xd0, 0x48, 0x32, 0x56, 0xad, 0x6a, 0xbf, 0x3d, 0xbe, 0x1b, 0xf3,
    0x3c, 0x06, 0x97, 0xe1, 0x55, 0x62, 0x50, 0xad, 0xbe, 0xc8, 0x7c, 0x95, 0x0b, 0xc0, 0x23, 0x2a,
    0xec, 0x11, 0x89, 0xc3, 0x43, 0xfc, 0x34, 0xc1, 0xb0, 0x1f, 0xe1, 0x4d, 0xdb, 0xc8, 0x3e, 0x91,
    0xb4, 0x69, 0x6b, 0xae, 0x93, 0x00, 0x00, 0x67, 0xa4, 0x31, 0x01, 0x00, 0x01, 0x50, 0xc0, 0x3e,
    0x00, 0x00, 0x00, 0x63, 0xbb, 0x3a, 0x36, 0x3e, 0x30, 0x0d, 0x8b, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x59, 0x5a,
};
const int64_t kLzmaRecordSeek = 300;

// The x values ROOT wrote: x[i] = (i % 7) * 0.5, stored as big-endian floats.
std::vector<uint8_t> expectedX() {
  std::vector<uint8_t> v;
  for (int i = 0; i < 2000; ++i) {
    const float f = static_cast<float>(i % 7) * 0.5f;
    uint32_t u;
    std::memcpy(&u, &f, 4);
    for (int b = 3; b >= 0; --b)
      v.push_back(static_cast<uint8_t>(u >> (8 * b)));
  }
  return v;
}

std::vector<uint8_t> bytes(const uint8_t* p, size_t n) { return std::vector<uint8_t>(p, p + n); }

// The 3-byte little-endian sizes of a ROOT block header (csize at 3, usize at 6).
void setSize(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  b[at] = v & 0xFF;
  b[at + 1] = (v >> 8) & 0xFF;
  b[at + 2] = (v >> 16) & 0xFF;
}
uint32_t getSize(const std::vector<uint8_t>& b, size_t at) {
  return b[at] | b[at + 1] << 8 | b[at + 2] << 16;
}

DecodedFrames decode(const std::vector<uint8_t>& b, uint64_t objlen) {
  return decodeFrames(b.data(), b.size(), objlen);
}

// A ROOT block around `blob`: tag, a method byte, then the two sizes.
std::vector<uint8_t> rootBlock(char t0, char t1, const std::vector<uint8_t>& blob, uint32_t usize) {
  std::vector<uint8_t> b = {static_cast<uint8_t>(t0), static_cast<uint8_t>(t1), 1, 0, 0, 0, 0, 0, 0};
  setSize(b, 3, static_cast<uint32_t>(blob.size()));
  setSize(b, 6, usize);
  b.insert(b.end(), blob.begin(), blob.end());
  return b;
}

bool haveLz4() {
  // A build without lz4 says so for an LZ4 block it cannot decode.
  const DecodedFrames d = decode(bytes(kLz4Payload, sizeof(kLz4Payload)), kLz4Objlen);
  return !(d.check == FrameCheck::kNotCheckable && d.out.empty());
}

} // namespace

TEST(FrameCheck, RootWrittenBlocksPassTheirChecks) {
  const auto want = expectedX();
  DecodedFrames d = decode(bytes(kLzmaPayload, sizeof(kLzmaPayload)), kLzmaObjlen);
  EXPECT_EQ(d.check, FrameCheck::kPassed) << d.why;
  EXPECT_EQ(d.out, want);
  EXPECT_FALSE(d.headerTolerated);
  d = decode(bytes(kZlibPayload, sizeof(kZlibPayload)), kZlibObjlen);
  EXPECT_EQ(d.check, FrameCheck::kPassed) << d.why;
  EXPECT_EQ(d.out, want);
  // ROOT writes ZSTD frames without a content checksum: the bytes decode, but
  // there is nothing to check them against.
  d = decode(bytes(kZstdPayload, sizeof(kZstdPayload)), kZstdObjlen);
  EXPECT_EQ(d.check, FrameCheck::kNotCheckable);
  EXPECT_EQ(d.out, want);
}

TEST(FrameCheck, Lz4ChecksumIsTheXxh64RootWrote) {
  // ROOT's LZ4 block: 9-byte header, 8-byte big-endian XXH64 of the
  // compressed bytes after it, then those bytes. Pins our XXH64 against a
  // value ROOT computed, with or without lz4 in this build.
  const uint8_t* blob = kLz4Payload + 9;
  const uint32_t csize = kLz4Payload[3] | kLz4Payload[4] << 8 | kLz4Payload[5] << 16;
  ASSERT_EQ(9u + csize, sizeof(kLz4Payload));
  uint64_t stored = 0;
  for (int b = 0; b < 8; ++b)
    stored = (stored << 8) | blob[b];
  EXPECT_EQ(ucache::xxh64(blob + 8, csize - 8, 0), stored);
}

TEST(FrameCheck, Lz4BlockDependsOnTheBuild) {
  auto b = bytes(kLz4Payload, sizeof(kLz4Payload));
  DecodedFrames d = decode(b, kLz4Objlen);
  if (!haveLz4()) {
    EXPECT_EQ(d.check, FrameCheck::kNotCheckable);
    EXPECT_TRUE(d.out.empty());
    return;
  }
  EXPECT_EQ(d.check, FrameCheck::kPassed) << d.why;
  EXPECT_EQ(d.out, expectedX());
  b[b.size() - 5] ^= 0x01; // damage the LZ4 data: the hash no longer matches
  EXPECT_EQ(decode(b, kLz4Objlen).check, FrameCheck::kFailed);
}

TEST(FrameCheck, DamagedCompressedBytesFail) {
  // One byte changed in the middle of the compressed data, as on a bad copy.
  for (auto [p, n, objlen] : {std::tuple{kLzmaPayload, sizeof(kLzmaPayload), kLzmaObjlen},
                              std::tuple{kZlibPayload, sizeof(kZlibPayload), kZlibObjlen}}) {
    auto b = bytes(p, n);
    b[9 + (b.size() - 9) / 2] ^= 0x10;
    const DecodedFrames d = decode(b, objlen);
    EXPECT_EQ(d.check, FrameCheck::kFailed) << "tag " << b[0] << b[1];
    EXPECT_TRUE(d.out.empty());
    EXPECT_TRUE(decompressFrames(b.data(), b.size(), objlen).empty());
  }
}

TEST(FrameCheck, LastBlockSizesLargerThanThereAreAreTolerated) {
  // ROOT decodes a block with the sizes its header states and stops once the
  // object is complete, so larger sizes in the last block still read right.
  auto b = bytes(kLzmaPayload, sizeof(kLzmaPayload));
  auto in = b;
  setSize(in, 3, getSize(b, 3) + 1);
  DecodedFrames d = decode(in, kLzmaObjlen);
  EXPECT_EQ(d.check, FrameCheck::kPassed) << d.why;
  EXPECT_TRUE(d.headerTolerated);
  EXPECT_EQ(d.out, expectedX());
  auto out = b;
  setSize(out, 6, getSize(b, 6) + 1);
  d = decode(out, kLzmaObjlen);
  EXPECT_EQ(d.check, FrameCheck::kPassed) << d.why;
  EXPECT_TRUE(d.headerTolerated);
  // Smaller sizes fail, as they do in ROOT.
  auto lessIn = b;
  setSize(lessIn, 3, getSize(b, 3) - 1);
  EXPECT_EQ(decode(lessIn, kLzmaObjlen).check, FrameCheck::kFailed);
  auto lessOut = b;
  setSize(lessOut, 6, getSize(b, 6) - 1);
  EXPECT_EQ(decode(lessOut, kLzmaObjlen).check, FrameCheck::kFailed);
  // LZ4's hash covers exactly the compressed size the header states.
  auto lz4 = bytes(kLz4Payload, sizeof(kLz4Payload));
  setSize(lz4, 3, getSize(lz4, 3) + 1);
  EXPECT_EQ(decode(lz4, kLz4Objlen).check, FrameCheck::kFailed);
}

TEST(FrameCheck, ObjectLengthMustAddUp) {
  const auto b = bytes(kLzmaPayload, sizeof(kLzmaPayload));
  EXPECT_EQ(decode(b, kLzmaObjlen + 1).check, FrameCheck::kFailed); // wants another block
  EXPECT_EQ(decode(b, kLzmaObjlen - 1).check, FrameCheck::kFailed); // decodes longer
}

TEST(FrameCheck, CodecTags) {
  auto b = bytes(kLzmaPayload, sizeof(kLzmaPayload));
  b[0] = 'Q'; // a tag ROOT does not know: damage
  EXPECT_EQ(decode(b, kLzmaObjlen).check, FrameCheck::kFailed);
  b[0] = 'C';
  b[1] = 'S'; // ROOT's old algorithm: legitimate, not decoded here
  const DecodedFrames d = decode(b, kLzmaObjlen);
  EXPECT_EQ(d.check, FrameCheck::kNotCheckable);
  EXPECT_TRUE(d.out.empty());
}

TEST(FrameCheck, ZstdWithAChecksumIsChecked) {
  const auto want = expectedX();
  std::vector<uint8_t> z(ZSTD_compressBound(want.size()));
  ZSTD_CCtx* cx = ZSTD_createCCtx();
  ASSERT_NE(cx, nullptr);
  ZSTD_CCtx_setParameter(cx, ZSTD_c_compressionLevel, 1);
  ZSTD_CCtx_setParameter(cx, ZSTD_c_checksumFlag, 1);
  const size_t n = ZSTD_compress2(cx, z.data(), z.size(), want.data(), want.size());
  ZSTD_freeCCtx(cx);
  ASSERT_FALSE(ZSTD_isError(n));
  z.resize(n);
  auto b = rootBlock('Z', 'S', z, static_cast<uint32_t>(want.size()));
  DecodedFrames d = decode(b, want.size());
  EXPECT_EQ(d.check, FrameCheck::kPassed) << d.why;
  EXPECT_EQ(d.out, want);
  b[9 + z.size() / 2] ^= 0x10;
  EXPECT_NE(decode(b, want.size()).check, FrameCheck::kPassed);
}

TEST(FrameCheck, LzmaStreamWithoutACheckIsNotCheckable) {
  const auto want = expectedX();
  std::vector<uint8_t> x(lzma_stream_buffer_bound(want.size()));
  size_t pos = 0;
  ASSERT_EQ(lzma_easy_buffer_encode(1, LZMA_CHECK_NONE, nullptr, want.data(), want.size(),
                                    x.data(), &pos, x.size()),
            LZMA_OK);
  x.resize(pos);
  const auto b = rootBlock('X', 'Z', x, static_cast<uint32_t>(want.size()));
  const DecodedFrames d = decode(b, want.size());
  EXPECT_EQ(d.check, FrameCheck::kNotCheckable);
  EXPECT_EQ(d.out, want);
}

TEST(FrameCheck, TwoBlocksBothChecked) {
  // An object longer than one block's 16 MiB limit is written as several
  // blocks; here two, split by hand from the same data.
  const auto want = expectedX();
  const size_t half = want.size() / 2;
  std::vector<uint8_t> all;
  for (size_t part = 0; part < 2; ++part) {
    const std::vector<uint8_t> src(want.begin() + part * half,
                                   part ? want.end() : want.begin() + half);
    std::vector<uint8_t> x(lzma_stream_buffer_bound(src.size()));
    size_t pos = 0;
    ASSERT_EQ(lzma_easy_buffer_encode(1, LZMA_CHECK_CRC32, nullptr, src.data(), src.size(),
                                      x.data(), &pos, x.size()),
              LZMA_OK);
    x.resize(pos);
    const auto blk = rootBlock('X', 'Z', x, static_cast<uint32_t>(src.size()));
    all.insert(all.end(), blk.begin(), blk.end());
  }
  DecodedFrames d = decode(all, want.size());
  EXPECT_EQ(d.check, FrameCheck::kPassed) << d.why;
  EXPECT_EQ(d.out, want);
  // An oversized first block is not the last one: it fails.
  setSize(all, 6, getSize(all, 6) + 1);
  EXPECT_EQ(decode(all, want.size()).check, FrameCheck::kFailed);
}

TEST(BasketKey, ParsesTheFieldsRootWrote) {
  const auto k = parseKey(kLzmaRecord, sizeof(kLzmaRecord), 0);
  ASSERT_TRUE(k);
  EXPECT_EQ(k->cls, "TBasket");
  EXPECT_EQ(k->name, "x");
  EXPECT_EQ(k->title, "Events");
  EXPECT_EQ(k->seekkey, kLzmaRecordSeek);
  EXPECT_EQ(k->keylen, 70);
  EXPECT_EQ(k->keyPartLen, 51);
  const auto b = parseBasketHeader(kLzmaRecord, sizeof(kLzmaRecord), 0, *k);
  ASSERT_TRUE(b);
  EXPECT_EQ(b->nevBuf, 2000);
  EXPECT_EQ(b->nevBufSize, 4);
  EXPECT_EQ(b->last, 70 + 8000);
  EXPECT_EQ(b->bufferSize, 64000);
}

namespace {

struct Basket {
  KeyInfo k;
  BasketHeader b;
  BranchInfo br;
};

Basket rootBasket() {
  Basket x;
  x.k = *parseKey(kLzmaRecord, sizeof(kLzmaRecord), 0);
  x.b = *parseBasketHeader(kLzmaRecord, sizeof(kLzmaRecord), 0, x.k);
  x.br.name = "x";
  x.br.basketSeek = {kLzmaRecordSeek};
  x.br.basketBytes = {static_cast<int32_t>(sizeof(kLzmaRecord))};
  x.br.basketEntry = {0, 2000};
  return x;
}

} // namespace

TEST(BasketKey, MatchesTheTreesRecord) {
  const Basket x = rootBasket();
  const BasketKeyCheck c = checkBasketKey(x.k, x.b, x.br, 0, "Events");
  EXPECT_FALSE(c.fail);
  EXPECT_TRUE(c.problems.empty());
  EXPECT_TRUE(c.anomalies.empty());
}

TEST(BasketKey, FieldsRootReliesOnFail) {
  Basket x = rootBasket();
  x.k.seekkey += 1; // not where the branch says it is: ROOT refuses it too
  EXPECT_TRUE(checkBasketKey(x.k, x.b, x.br, 0, "Events").fail);
  x = rootBasket();
  x.b.nevBuf = 1999; // a different entry count
  EXPECT_TRUE(checkBasketKey(x.k, x.b, x.br, 0, "Events").fail);
  x = rootBasket();
  x.b.last = x.k.keylen + x.k.objlen + 1; // entry data ends past the basket
  EXPECT_TRUE(checkBasketKey(x.k, x.b, x.br, 0, "Events").fail);
  x = rootBasket();
  x.k.keylen = x.k.keyPartLen + 10; // too short to hold the basket fields
  EXPECT_TRUE(checkBasketKey(x.k, x.b, x.br, 0, "Events").fail);
  x = rootBasket();
  EXPECT_TRUE(checkBasketKey(x.k, x.b, x.br, 1, "Events").fail); // no such basket
}

TEST(ConvertBasket, ReportsWhatItsDecodeSaid) {
  // A basket ROOT wrote converts, and says its check passed.
  const std::vector<std::string> lzma = {"lzma"};
  ConvertedBasket ok = convertBasket(kLzmaRecord, sizeof(kLzmaRecord), 3 * sizeof(kLzmaRecord), lzma);
  EXPECT_NE(ok.kind, ConvertedBasket::kOriginal);
  EXPECT_EQ(ok.check, ConvertedBasket::kPassed);
  // One byte changed in its compressed data: kept as stored, and failed.
  std::vector<uint8_t> bad(kLzmaRecord, kLzmaRecord + sizeof(kLzmaRecord));
  bad[70 + 9 + 60] ^= 0x10;
  ConvertedBasket b = convertBasket(bad.data(), bad.size(), 3 * bad.size(), lzma);
  EXPECT_EQ(b.kind, ConvertedBasket::kOriginal);
  EXPECT_EQ(b.check, ConvertedBasket::kFailed);
  // A codec not converted is not decoded, so not examined.
  ConvertedBasket n = convertBasket(kLzmaRecord, sizeof(kLzmaRecord), 3 * sizeof(kLzmaRecord),
                                    std::vector<std::string>{"zlib"});
  EXPECT_EQ(n.kind, ConvertedBasket::kOriginal);
  EXPECT_EQ(n.check, ConvertedBasket::kNotExamined);
}

TEST(BasketKey, FieldsRootIgnoresAreOnlyNoted) {
  Basket x = rootBasket();
  x.br.basketBytes = {static_cast<int32_t>(sizeof(kLzmaRecord)) + 1};
  BasketKeyCheck c = checkBasketKey(x.k, x.b, x.br, 0, "Events");
  EXPECT_FALSE(c.fail);
  EXPECT_EQ(c.anomalies.size(), 1u);
  x = rootBasket();
  x.k.name = "y";
  x.k.title = "Other";
  c = checkBasketKey(x.k, x.b, x.br, 0, "Events");
  EXPECT_FALSE(c.fail);
  EXPECT_EQ(c.anomalies.size(), 2u);
}

// The key-position check of fetched bytes (firstMisplacedKey). Each failing
// case uses the same bytes as a passing one with one change; the passing cases
// are the control that the check does not fail everything.
namespace {

// The record ROOT wrote (a version 1004 key: 64-bit fSeekKey at byte 18), as
// it would read had ROOT written it at `seek`.
std::vector<uint8_t> wideAt(uint64_t seek) {
  std::vector<uint8_t> w = bytes(kLzmaRecord, sizeof(kLzmaRecord));
  for (int i = 0; i < 8; ++i)
    w[18 + i] = static_cast<uint8_t>(seek >> (8 * (7 - i)));
  return w;
}

// The same record as a version 4 key: fSeekKey and fSeekPdir 4 bytes each, as
// ROOT writes a key below 2 GiB.
std::vector<uint8_t> narrowAt(uint32_t seek) {
  const std::vector<uint8_t> w = wideAt(seek);
  std::vector<uint8_t> n(w.begin(), w.begin() + 18);
  n[3] = static_cast<uint8_t>(n[3] - 8); // fNbytes
  n[4] = 0;
  n[5] = 4;                              // fVersion
  n[15] = static_cast<uint8_t>(n[15] - 8); // fKeylen
  n.insert(n.end(), w.begin() + 22, w.begin() + 26); // fSeekKey, low 4 bytes
  n.insert(n.end(), w.begin() + 30, w.begin() + 34); // fSeekPdir, low 4 bytes
  n.insert(n.end(), w.begin() + 34, w.end());
  return n;
}

// `rec` placed at file offset `at` in a span of `len` bytes from `spanOff`.
struct Span {
  uint64_t off;
  std::vector<uint8_t> b;
};
Span spanWith(const std::vector<uint8_t>& rec, uint64_t spanOff, uint64_t at, size_t len) {
  Span s{spanOff, std::vector<uint8_t>(len, 0xAB)};
  std::copy(rec.begin(), rec.end(), s.b.begin() + (at - spanOff));
  return s;
}
MisplacedKeys check(const std::vector<uint64_t>& seeks, const Span& s, uint64_t lo, uint64_t hi) {
  return firstMisplacedKey(seeks, s.off, s.b.data(), s.b.size(), lo, hi);
}

} // namespace

TEST(KeyPosition, RootWrittenKeysPass) {
  // The record as ROOT wrote it, a 64-bit key at 300.
  const uint64_t S = kLzmaRecordSeek;
  MisplacedKeys m = check({S}, spanWith(wideAt(S), 0, S, 4096), S, S + sizeof(kLzmaRecord));
  EXPECT_EQ(m.count, 0u);
  EXPECT_FALSE(m.outside);
  // The same as a 32-bit key: the shape parseKey reads, at the same place.
  const std::vector<uint8_t> n = narrowAt(S);
  const auto k = parseKey(n.data(), n.size(), 0);
  ASSERT_TRUE(k);
  EXPECT_EQ(k->ver, 4);
  EXPECT_EQ(k->seekkey, kLzmaRecordSeek);
  EXPECT_EQ(k->name, "x");
  EXPECT_EQ(check({S}, spanWith(n, 0, S, 4096), S, S + n.size()).count, 0u);
  // 64-bit keys at 27,130,216 (2408 bytes into a page) and past 2^31.
  for (uint64_t at : {uint64_t{27130216}, (uint64_t{1} << 31) + 4096, uint64_t{5} << 32}) {
    m = check({at}, spanWith(wideAt(at), at - 2408, at, 4096), at, at + sizeof(kLzmaRecord));
    EXPECT_EQ(m.count, 0u) << at;
    EXPECT_FALSE(m.outside) << at;
  }
}

TEST(KeyPosition, VersionsReadAsRootReadsThem) {
  // TKey reads a key's version as a signed 16-bit Version_t and takes 64-bit
  // positions only above 1000, and reads a 32-bit fSeekKey unsigned. A 32-bit
  // basket key whose version has its top bit set (4 as 0x8004) is negative to
  // ROOT, which reads it as 32-bit and reads the basket: it must pass.
  const uint64_t S = kLzmaRecordSeek;
  std::vector<uint8_t> n = narrowAt(S);
  n[4] |= 0x80;
  auto k = parseKey(n.data(), n.size(), 0);
  ASSERT_TRUE(k);
  EXPECT_EQ(k->ver, -32764);
  EXPECT_EQ(k->seekkey, static_cast<int64_t>(S));
  EXPECT_EQ(k->cls, "TBasket");
  EXPECT_EQ(k->name, "x");
  EXPECT_EQ(k->title, "Events");
  EXPECT_EQ(check({S}, spanWith(n, 0, S, 4096), S, S + n.size()).count, 0u);
  // And through the conversion's own check of the basket (fLast moves with the
  // 8 bytes the narrow key header is shorter by).
  {
    std::vector<uint8_t> r = n;
    const size_t lastAt = k->keyPartLen + 14u;
    const uint32_t last = (uint32_t{r[lastAt]} << 24) | (uint32_t{r[lastAt + 1]} << 16) |
                          (uint32_t{r[lastAt + 2]} << 8) | r[lastAt + 3];
    const uint32_t moved = last - 8;
    for (int i = 0; i < 4; ++i)
      r[lastAt + i] = static_cast<uint8_t>(moved >> (24 - 8 * i));
    const auto rk = parseKey(r.data(), r.size(), 0);
    ASSERT_TRUE(rk);
    const auto b = parseBasketHeader(r.data(), r.size(), 0, *rk);
    ASSERT_TRUE(b);
    BranchInfo br;
    br.name = "x";
    br.basketSeek = {static_cast<int64_t>(S)};
    br.basketBytes = {static_cast<int32_t>(r.size())};
    br.basketEntry = {0, 2000};
    const BasketKeyCheck c = checkBasketKey(*rk, *b, br, 0, "Events");
    EXPECT_FALSE(c.fail) << (c.problems.empty() ? "" : c.problems.front());
  }
  // The same bit on a 64-bit key's 1004: ROOT reads it as 32-bit, finds the
  // high half of the position (0 here) at byte 18, and refuses the basket; so
  // does the check.
  std::vector<uint8_t> w = wideAt(S);
  w[4] |= 0x80;
  EXPECT_EQ(check({S}, spanWith(w, 0, S, 4096), S, S + w.size()).count, 1u);
  // A version of 1001 is 64-bit, 1000 is 32-bit (ROOT's "> 1000").
  std::vector<uint8_t> v1000 = narrowAt(S);
  v1000[4] = 0x03;
  v1000[5] = 0xE8;
  EXPECT_EQ(check({S}, spanWith(v1000, 0, S, 4096), S, S + v1000.size()).count, 0u);
  std::vector<uint8_t> v1001 = wideAt(S);
  v1001[4] = 0x03;
  v1001[5] = 0xE9;
  EXPECT_EQ(check({S}, spanWith(v1001, 0, S, 4096), S, S + v1001.size()).count, 0u);
  // A 32-bit fSeekKey is unsigned: a position with its top bit set, as ROOT
  // reads it.
  const uint64_t T = (uint64_t{1} << 31) + 4096;
  const std::vector<uint8_t> high = narrowAt(static_cast<uint32_t>(T));
  k = parseKey(high.data(), high.size(), 0);
  ASSERT_TRUE(k);
  EXPECT_EQ(k->seekkey, static_cast<int64_t>(T));
  EXPECT_EQ(check({T}, spanWith(high, T - 100, T, 4096), T, T + high.size()).count, 0u);
}

TEST(KeyPosition, ZerosWhereTheTreeSaysABasketIsFail) {
  // A basket of 1,647 bytes at 27,130,216, 2408 bytes into its page, read
  // back as zeros: the shape seen at a site.
  const uint64_t S = 27130216;
  Span s = spanWith(wideAt(S), S - 2408, S, 4096);
  ASSERT_EQ(check({S}, s, S, S + 1647).count, 0u);
  std::fill(s.b.begin() + 2408, s.b.begin() + 2408 + 1647, 0);
  MisplacedKeys m = check({S}, s, S, S + 1647);
  EXPECT_EQ(m.count, 1u);
  EXPECT_EQ(m.first, S);
  EXPECT_FALSE(m.outside);
  // The same past 2^31: zeros read as a version 0 key whose position is 0.
  const uint64_t far = (uint64_t{1} << 31) + 1000;
  Span z = spanWith(wideAt(far), far - 1000, far, 4096);
  ASSERT_EQ(check({far}, z, far, far + 1647).count, 0u);
  std::fill(z.b.begin() + 1000, z.b.begin() + 1000 + 1647, 0);
  m = check({far}, z, far, far + 1647);
  EXPECT_EQ(m.count, 1u);
  EXPECT_EQ(m.first, far);
  // And a 32-bit key's header zeroed.
  Span n = spanWith(narrowAt(kLzmaRecordSeek), 0, kLzmaRecordSeek, 4096);
  ASSERT_EQ(check({uint64_t(kLzmaRecordSeek)}, n, 0, 4096).count, 0u);
  std::fill(n.b.begin() + kLzmaRecordSeek, n.b.begin() + kLzmaRecordSeek + 40, 0);
  EXPECT_EQ(check({uint64_t(kLzmaRecordSeek)}, n, 0, 4096).count, 1u);
}

TEST(KeyPosition, ASeekOffByOneFails) {
  const uint64_t S = 27130216;
  ASSERT_EQ(check({S}, spanWith(wideAt(S), S - 100, S, 4096), S, S + 100).count, 0u);
  const MisplacedKeys m = check({S}, spanWith(wideAt(S + 1), S - 100, S, 4096), S, S + 100);
  EXPECT_EQ(m.count, 1u);
  EXPECT_EQ(m.first, S);
  const uint64_t T = kLzmaRecordSeek;
  ASSERT_EQ(check({T}, spanWith(narrowAt(T), 0, T, 4096), 0, 4096).count, 0u);
  EXPECT_EQ(check({T}, spanWith(narrowAt(T + 1), 0, T, 4096), 0, 4096).count, 1u);
}

TEST(KeyPosition, WhatTheSpanDoesNotHoldWhollyIsNotJudged) {
  const uint64_t S = 1000;
  const std::vector<uint8_t> zeros(4096, 0);
  // A header the span's end cuts: zeros read as version 0, whose header is 22
  // bytes, and a misplaced 64-bit key, whose header is 26.
  EXPECT_EQ(firstMisplacedKey({S}, 0, zeros.data(), S + 21, 0, S + 21).count, 0u);
  EXPECT_EQ(firstMisplacedKey({S}, 0, zeros.data(), S + 22, 0, S + 22).count, 1u);
  const Span cut = spanWith(wideAt(S + 1), 0, S, 4096);
  EXPECT_EQ(firstMisplacedKey({S}, 0, cut.b.data(), S + 25, 0, S + 25).count, 0u);
  EXPECT_EQ(firstMisplacedKey({S}, 0, cut.b.data(), S + 26, 0, S + 26).count, 1u);
  // A span that starts inside a basket: that basket is not judged.
  EXPECT_EQ(firstMisplacedKey({S}, S + 1, zeros.data(), 4096, S + 1, S + 4097).count, 0u);
  // Zeros with no basket of the table inside them.
  EXPECT_EQ(firstMisplacedKey({S, 9000}, 2000, zeros.data(), 4096, 2000, 6096).count, 0u);
  EXPECT_EQ(firstMisplacedKey({}, 0, zeros.data(), 4096, 0, 4096).count, 0u);
  EXPECT_EQ(firstMisplacedKey({S}, 0, nullptr, 0, 0, 0).count, 0u);
}

TEST(KeyPosition, OnlyTheReadersBasketsFailTheRead) {
  // Two baskets in one fetch: the reader's at A is good; its neighbour at B is
  // zeros and lies in the rounded rest of the fetch only.
  const uint64_t A = 300, B = 3000;
  Span s = spanWith(wideAt(A), 0, A, 4096);
  std::fill(s.b.begin() + B, s.b.end(), 0);
  MisplacedKeys m = check({A, B}, s, A, A + sizeof(kLzmaRecord));
  EXPECT_EQ(m.count, 0u);
  EXPECT_TRUE(m.outside);
  m = check({A, B}, s, A, B + 1); // the reader's bytes reach B
  EXPECT_EQ(m.count, 1u);
  EXPECT_EQ(m.first, B);
  EXPECT_FALSE(m.outside);
}

TEST(KeyPosition, BasketSeeksAreTheTreesOwn) {
  FileMeta fm;
  BranchInfo a, b, ext;
  a.basketSeek = {500, 100, 0};
  b.basketSeek = {300, 100};
  ext.basketSeek = {700};
  ext.externalFile = true;
  fm.branches = {a, b, ext};
  EXPECT_EQ(basketSeeks(fm), (std::vector<uint64_t>{100, 300, 500}));
}

TEST(KeyPosition, ZerosFailTheKeyHeaderCheck) {
  // A key header of zeros parses (every length is 0) and fails on its position.
  const std::vector<uint8_t> zeros(1647, 0);
  const auto k = parseKey(zeros.data(), zeros.size(), 0);
  ASSERT_TRUE(k);
  const auto b = parseBasketHeader(zeros.data(), zeros.size(), 0, *k);
  ASSERT_TRUE(b);
  BranchInfo br;
  br.name = "nElectron";
  br.basketSeek = {27130216};
  br.basketBytes = {1647};
  br.basketEntry = {0, 100};
  const BasketKeyCheck c = checkBasketKey(*k, *b, br, 0, "Events");
  EXPECT_TRUE(c.fail);
  ASSERT_FALSE(c.problems.empty());
  EXPECT_EQ(c.problems.front(), "key position differs from the branch's record");
}

TEST(KeyPosition, CodecCheckOfZerosPassesToday) {
  // A characterization of today's gap, not the behaviour wanted: an object of
  // length 0 has no block to check, so the codec check passes all-zero bytes
  // and only the key-position check catches them. Once the byte path checks
  // codecs, a record that is not a basket where the tree says a basket is
  // must count as failed, and this expectation is to be inverted.
  const std::vector<uint8_t> zeros(1647, 0);
  EXPECT_EQ(decode(zeros, 0).check, FrameCheck::kPassed);
}

// The file map (basketMap): each basket's position and the length the tree's record gives it, and
// the key check of fetched bytes against both (firstMisplacedKey with lengths).
namespace {
void putBe32(std::vector<uint8_t>& r, size_t at, int32_t v) {
  for (int i = 0; i < 4; ++i)
    r[at + i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * (3 - i)));
}
MisplacedKeys checkLen(const std::vector<uint64_t>& seeks, const std::vector<int32_t>& lengths,
                       const Span& s, uint64_t lo, uint64_t hi) {
  return firstMisplacedKey(seeks, lengths, s.off, s.b.data(), s.b.size(), lo, hi);
}
} // namespace

TEST(FileMap, LengthsAreTheTreesOwn) {
  FileMeta fm;
  BranchInfo a, b, ext;
  a.basketSeek = {500, 100, 0};
  a.basketBytes = {50, 40, 0};
  b.basketSeek = {300, 100, 900};
  b.basketBytes = {60, 41, -5};
  ext.basketSeek = {700};
  ext.basketBytes = {70};
  ext.externalFile = true;
  fm.branches = {a, b, ext};
  std::vector<uint64_t> seeks;
  std::vector<int32_t> bytes;
  basketMap(fm, seeks, bytes);
  EXPECT_EQ(seeks, (std::vector<uint64_t>{100, 300, 500, 900}));
  EXPECT_EQ(bytes, (std::vector<int32_t>{0, 60, 50, 0}))
      << "100: two records disagree; 900: no length above 0";
  EXPECT_EQ(seeks, basketSeeks(fm)) << "the same positions as the key-position check";
}

TEST(FileMap, KeyLengthAndFieldsAgainstTheMap) {
  const uint64_t S = kLzmaRecordSeek;
  const std::vector<uint8_t> rec = wideAt(S);
  const int32_t nb = 195, kl = 70, ol = 8000; // the record's fNbytes, fKeylen, fObjlen: compressed
  const uint64_t hi = S + rec.size();
  // As ROOT wrote it: passes with its length, with no length known, with no lengths at all.
  EXPECT_EQ(checkLen({S}, {nb}, spanWith(rec, 0, S, 4096), S, hi).count, 0u);
  EXPECT_EQ(checkLen({S}, {0}, spanWith(rec, 0, S, 4096), S, hi).count, 0u);
  EXPECT_EQ(checkLen({S}, {}, spanWith(rec, 0, S, 4096), S, hi).count, 0u);
  // Lengths that differ where ROOT still reads the basket correctly (it reads the map's length and
  // decompresses by the blocks' own headers; compressed by either length): no claim.
  EXPECT_EQ(checkLen({S}, {nb + 1}, spanWith(rec, 0, S, 4096), S, hi).count, 0u) << "map longer";
  EXPECT_EQ(checkLen({S}, {nb - 8}, spanWith(rec, 0, S, 4096), S, hi).count, 0u) << "map shorter";
  std::vector<uint8_t> r = rec;
  putBe32(r, 0, nb - 8);
  EXPECT_EQ(checkLen({S}, {nb}, spanWith(r, 0, S, 4096), S, hi).count, 0u) << "key shorter";
  putBe32(r, 0, kl + ol - 1);
  EXPECT_EQ(checkLen({S}, {nb}, spanWith(r, 0, S, 4096), S, hi).count, 0u)
      << "key longer, still compressed by it";
  // The key's length makes ROOT read the basket as stored uncompressed (its compressed bytes taken
  // for the entries: wrong values, no error): refused, by length.
  putBe32(r, 0, kl + ol);
  MisplacedKeys m = checkLen({S}, {nb}, spanWith(r, 0, S, 4096), S, hi);
  EXPECT_EQ(m.count, 1u);
  EXPECT_EQ(m.first, S);
  EXPECT_TRUE(m.length);
  putBe32(r, 0, kl + ol + 500);
  EXPECT_EQ(checkLen({S}, {nb}, spanWith(r, 0, S, 4096), S, hi).count, 1u);
  EXPECT_EQ(checkLen({S}, {}, spanWith(r, 0, S, 4096), S, hi).count, 0u)
      << "without the map's lengths, only the position and the key's own fields";
  // The map's length says stored uncompressed, the key's says compressed (ROOT would try to
  // decompress stored bytes and fail): refused.
  m = checkLen({S}, {kl + ol}, spanWith(rec, 0, S, 4096), S, hi);
  EXPECT_EQ(m.count, 1u);
  EXPECT_TRUE(m.length);
  // The key's own fields, where ROOT fails too: a key length of 0, one past its record, an object
  // length below 0.
  r = rec;
  r[14] = 0; // fKeylen, after fNbytes, fVersion, fObjlen and fDatime
  r[15] = 0;
  EXPECT_EQ(checkLen({S}, {0}, spanWith(r, 0, S, 4096), S, hi).count, 1u) << "key length 0";
  r = rec;
  r[14] = 0x10; // 4096 + ...: past the record's 195 bytes
  EXPECT_EQ(checkLen({S}, {nb}, spanWith(r, 0, S, 4096), S, hi).count, 1u) << "key past its record";
  r = rec;
  putBe32(r, 6, -1);
  EXPECT_EQ(checkLen({S}, {nb}, spanWith(r, 0, S, 4096), S, hi).count, 1u) << "object length < 0";
  // A 32-bit key (8 bytes shorter): with the map's own length, or one 8 bytes longer (a key
  // rewritten shorter in place, the tree's record left as it was), ROOT reads it: no claim.
  const std::vector<uint8_t> n = narrowAt(S);
  EXPECT_EQ(checkLen({S}, {nb - 8}, spanWith(n, 0, S, 4096), S, S + n.size()).count, 0u);
  EXPECT_EQ(checkLen({S}, {nb}, spanWith(n, 0, S, 4096), S, S + n.size()).count, 0u);
  std::vector<uint8_t> nf = n;
  putBe32(nf, 0, (kl - 8) + ol); // its length makes it a stored basket: refused
  m = checkLen({S}, {nb - 8}, spanWith(nf, 0, S, 4096), S, S + n.size());
  EXPECT_EQ(m.count, 1u);
  EXPECT_TRUE(m.length);
  // A position that fails is reported as one, not as a length.
  m = checkLen({S}, {nb}, spanWith(wideAt(S + 1), 0, S, 4096), S, hi);
  EXPECT_EQ(m.count, 1u);
  EXPECT_FALSE(m.length);
  // Outside the reader's bytes: noted, not counted.
  m = checkLen({S}, {kl + ol}, spanWith(rec, 0, S, 4096), S + 1, S + 2);
  EXPECT_EQ(m.count, 0u);
  EXPECT_TRUE(m.outside);
  // A header the span cuts is not judged (the length lies in its first 18 bytes, the position
  // past them: judged only once the position fits).
  Span cut = spanWith(rec, 0, S, 4096);
  cut.b.resize(S + 25);
  EXPECT_EQ(checkLen({S}, {kl + ol}, cut, S, S + 25).count, 0u);
}

TEST(FileMap, AMapThatShowsTheCopyBad) {
  FileMeta fm;
  std::vector<uint64_t> seeks{100, 300};
  std::vector<int32_t> bytes{200, 50};
  EXPECT_EQ(basketMapBad(fm, seeks, bytes, 350), "");
  EXPECT_EQ(basketMapBad(fm, seeks, bytes, 349),
            "the tree's record places a basket at 300 of 50 bytes past the copy's end at 349");
  EXPECT_EQ(basketMapBad(fm, {100, 400}, {0, 0}, 300), "") << "no length known: no claim";
  EXPECT_EQ(basketMapBad(fm, {1000}, {10}, 300),
            "the tree's record places a basket at 1000 of 10 bytes past the copy's end at 300");
  fm.error = "TTree v19 unsupported";
  EXPECT_EQ(basketMapBad(fm, {}, {}, 300), "") << "a parse this walk does not support";
  fm.error = "tree metadata failed its own check";
  fm.recordFailed = true;
  EXPECT_EQ(basketMapBad(fm, {}, {}, 300), "the tree's record fails its own check");
}

#ifdef UCACHE_TEST_DATA_DIR
namespace {
struct BytesSource : Source {
  std::vector<uint8_t> b;
  bool read(void* dst, uint64_t n, uint64_t off) override {
    if (off > b.size() || n > b.size() - off)
      return false;
    std::memcpy(dst, b.data() + off, n);
    return true;
  }
  bool has(uint64_t off, uint64_t n) override { return off <= b.size() && n <= b.size() - off; }
};
} // namespace

// A tree record whose compressed data fails its own check: ROOT cannot read the tree, and the parse
// says so (recordFailed), unlike a parse it does not support.
TEST(FileMap, ATreeRecordThatFailsItsOwnCheck) {
  int checked = 0;
  for (const char* f : {"unnamed_codec_fixture.root", "unnamed_codec_wide_fixture.root"}) {
    BytesSource src;
    FILE* fp = std::fopen((std::string(UCACHE_TEST_DATA_DIR) + "/" + f).c_str(), "rb");
    ASSERT_NE(fp, nullptr) << f;
    uint8_t buf[65536];
    size_t r;
    while ((r = std::fread(buf, 1, sizeof buf, fp)) > 0)
      src.b.insert(src.b.end(), buf, buf + r);
    std::fclose(fp);
    const int64_t size = static_cast<int64_t>(src.b.size());
    FileMeta good = parseFile(src, size, "Events");
    if (!good.error.empty())
      good = parseFile(src, size, "t");
    if (!good.error.empty())
      continue; // no tree this walk reads in it
    EXPECT_FALSE(good.recordFailed);
    const KeyInfo& k = good.treeKey;
    ASSERT_GT(k.objlen, k.nbytes - k.keylen) << f << ": a compressed tree record";
    const std::string codec = blockCodec(src.b.data() + k.seekkey + k.keylen, 9);
    if (codec != "lzma" && codec != "zlib")
      continue; // a codec with no check of its own
    src.b[static_cast<size_t>(k.seekkey + k.keylen + 40)] ^= 0x10; // inside the compressed data
    const FileMeta bad = parseFile(src, size, good.treeKey.name);
    EXPECT_FALSE(bad.error.empty()) << f;
    EXPECT_TRUE(bad.recordFailed) << f << ": " << bad.error;
    EXPECT_EQ(basketMapBad(bad, {}, {}, static_cast<uint64_t>(size)),
              "the tree's record fails its own check");
    ++checked;
  }
  EXPECT_GT(checked, 0) << "a fixture with a checked tree record";
}
#endif
