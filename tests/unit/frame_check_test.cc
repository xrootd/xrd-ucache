// The origin-integrity checks on compressed objects and basket key headers,
// pinned against blocks ROOT itself wrote: a check that is wrong but
// self-consistent would pass our own round trip and still fail on real data.
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
