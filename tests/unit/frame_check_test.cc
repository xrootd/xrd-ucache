// GCC 11 reports a read of zero bytes in std::vector::insert of this file's constant test bytes
// (-Wstringop-overread, a false report that later GCCs do not make).
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 11
#pragma GCC diagnostic ignored "-Wstringop-overread"
#endif
// The origin-integrity checks on compressed objects and basket key headers,
// pinned against blocks ROOT itself wrote: a check that is wrong but
// self-consistent would pass our own round trip and still fail on real data.
#include "FillLayout.h"
#include "TreeMeta.h"
#include "vendor/xxh3.h"

#include <gtest/gtest.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include <cstring>
#include <set>
#include <thread>
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
} // namespace

// A decoder that cannot get memory is a local failure (std::bad_alloc, which the callers take as
// no claim on the copy), never a failed check, for each codec that allocates; once memory is back
// the same block decodes and passes. LZ4 allocates nothing and is judged as ever.
TEST(FrameCheck, ADecoderOutOfMemoryIsNoFailedCheck) {
  const struct {
    const char* codec;
    const uint8_t* p;
    size_t n;
    uint64_t objlen;
  } blocks[] = {{"lzma", kLzmaPayload, sizeof kLzmaPayload, kLzmaObjlen},
                {"zlib", kZlibPayload, sizeof kZlibPayload, kZlibObjlen},
                {"zstd", kZstdPayload, sizeof kZstdPayload, kZstdObjlen}};
  for (const auto& b : blocks) {
    {
      DecoderNoMemory none;
      EXPECT_THROW(decodeFrames(b.p, b.n, b.objlen), std::bad_alloc) << b.codec;
      EXPECT_THROW(verifyFrames(b.p, b.n, b.objlen), std::bad_alloc) << b.codec;
      // decompressFrames' callers read {} as "not here" (a stored page decoded for a request is
      // fetched again): it never throws
      std::vector<uint8_t> got;
      EXPECT_NO_THROW(got = decompressFrames(b.p, b.n, b.objlen)) << b.codec;
      EXPECT_TRUE(got.empty()) << b.codec;
    }
    // ROOT's ZSTD block carries no content checksum: not checkable, but decoded
    const DecodedFrames d = decodeFrames(b.p, b.n, b.objlen);
    EXPECT_NE(d.check, FrameCheck::kFailed) << b.codec;
    EXPECT_EQ(d.out.size(), b.objlen) << b.codec;
    EXPECT_EQ(verifyFrames(b.p, b.n, b.objlen).check, d.check) << b.codec;
  }
  DecoderNoMemory none;
  EXPECT_EQ(verifyFrames(kLz4Payload, sizeof kLz4Payload, kLz4Objlen).check, FrameCheck::kPassed);
}

namespace {
// ROOT's LZMA block with its xz block header's LZMA2 dictionary raised to `code` (the filter's
// one property byte: 40 is 4 GiB - 1), the header's CRC32 made again: a valid stream naming a
// dictionary no ROOT writer makes.
std::vector<uint8_t> lzmaWithDictionary(uint8_t code) {
  std::vector<uint8_t> b(kLzmaPayload, kLzmaPayload + sizeof kLzmaPayload);
  const size_t xz = 9;                 // ROOT's block header, then the xz stream
  const size_t bh = xz + 12;           // the xz stream header, then the first block's header
  const size_t size = (b[bh] + 1) * 4; // its size byte
  const uint8_t flags = b[bh + 1];
  size_t p = bh + 2;
  auto varint = [&] {
    while (b[p++] & 0x80) {
    }
  };
  if (flags & 0x40)
    varint(); // compressed size
  if (flags & 0x80)
    varint(); // uncompressed size
  EXPECT_EQ(b[p], 0x21) << "the LZMA2 filter";
  EXPECT_EQ(b[p + 1], 1) << "one property byte";
  b[p + 2] = code;
  const uint32_t crc = static_cast<uint32_t>(crc32(0, b.data() + bh, static_cast<uInt>(size - 4)));
  for (int i = 0; i < 4; ++i)
    b[bh + size - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
  return b;
}
} // namespace

// An xz stream that names a dictionary larger than any ROOT writer makes (its decoder would need
// more than 1 GiB, liblzma's memlimit) fails its check: never a want of memory of this process,
// whatever the process's limits; the dictionary ROOT wrote decodes as ever.
TEST(FrameCheck, AnLzmaDictionaryNoRootWriterMakesFailsItsCheck) {
  EXPECT_EQ(decode(lzmaWithDictionary(40), kLzmaObjlen).check, FrameCheck::kFailed);
  EXPECT_EQ(verifyFrames(lzmaWithDictionary(40).data(), lzmaWithDictionary(40).size(), kLzmaObjlen).check,
            FrameCheck::kFailed);
  const std::vector<uint8_t> asWritten(kLzmaPayload, kLzmaPayload + sizeof kLzmaPayload);
  EXPECT_EQ(decode(asWritten, kLzmaObjlen).check, FrameCheck::kPassed);
}

namespace {

// A ROOT block around `blob`: tag, a method byte, then the two sizes.
// Its third byte as ROOT writes it: ZL and CS 8 (Z_DEFLATED), XZ 0, ZS 1, L4 the LZ4 major
// version 1.
std::vector<uint8_t> rootBlock(char t0, char t1, const std::vector<uint8_t>& blob, uint32_t usize) {
  const uint8_t m = (t0 == 'Z' && t1 == 'L') || (t0 == 'C' && t1 == 'S') ? 8
                    : t0 == 'X' && t1 == 'Z'                             ? 0
                                                                         : 1;
  std::vector<uint8_t> b = {
      static_cast<uint8_t>(t0), static_cast<uint8_t>(t1), m, 0, 0, 0, 0, 0, 0};
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
  b[1] = 'S'; // ROOT's old algorithm, its third byte not 8: ROOT does not decode it
  EXPECT_EQ(decode(b, kLzmaObjlen).check, FrameCheck::kFailed);
  b[2] = 8; // ROOT's old algorithm: legitimate, not decoded here
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
  // A first block whose header states more output than it gives: ROOT goes on from what it gave
  // (TBasket::ReadBasketBuffers advances by R__unzip's output), and reads the object exactly.
  for (uint32_t more : {1u, 5000u, 0xffffffu - getSize(all, 6)}) {
    std::vector<uint8_t> raised = all;
    setSize(raised, 6, getSize(all, 6) + more);
    d = decode(raised, want.size());
    EXPECT_EQ(d.check, FrameCheck::kPassed) << more << " " << d.why;
    EXPECT_EQ(d.out, want) << more;
    EXPECT_EQ(verifyFrames(raised.data(), raised.size(), want.size()).check, FrameCheck::kPassed);
  }
  // One that states less than it gives does not fit its output, in ROOT too: it fails.
  std::vector<uint8_t> lowered = all;
  setSize(lowered, 6, getSize(all, 6) - 1);
  EXPECT_EQ(decode(lowered, want.size()).check, FrameCheck::kFailed);
  // Blocks that give less than the object: fails (the second header runs past the payload).
  EXPECT_EQ(decode(all, want.size() + 1).check, FrameCheck::kFailed);
}

TEST(FrameCheck, ZlibBlockGivingLessThanItsHeaderStates) {
  const auto want = expectedX();
  const size_t half = want.size() / 2;
  std::vector<uint8_t> all;
  for (size_t part = 0; part < 2; ++part) {
    const std::vector<uint8_t> src(want.begin() + part * half,
                                   part ? want.end() : want.begin() + half);
    uLongf zl = compressBound(src.size());
    std::vector<uint8_t> z(zl);
    ASSERT_EQ(compress2(z.data(), &zl, src.data(), src.size(), 6), Z_OK);
    z.resize(zl);
    std::vector<uint8_t> blk = rootBlock('Z', 'L', z, static_cast<uint32_t>(src.size()));
    blk[2] = 8;
    all.insert(all.end(), blk.begin(), blk.end());
  }
  EXPECT_EQ(decode(all, want.size()).check, FrameCheck::kPassed);
  std::vector<uint8_t> raised = all;
  setSize(raised, 6, getSize(all, 6) + 3000);
  const DecodedFrames d = decode(raised, want.size());
  EXPECT_EQ(d.check, FrameCheck::kPassed) << d.why;
  EXPECT_EQ(d.out, want);
  std::vector<uint8_t> lowered = all;
  setSize(lowered, 6, getSize(all, 6) - 1);
  EXPECT_EQ(decode(lowered, want.size()).check, FrameCheck::kFailed);
}

// verifyFrames judges as decodeFrames does, keeping nothing; LZ4 by its hash, in any build.
TEST(FrameCheck, VerifyJudgesAsDecodeDoes) {
  struct P {
    const uint8_t* p;
    size_t n;
    uint64_t objlen;
  };
  for (const P& x : {P{kLzmaPayload, sizeof(kLzmaPayload), kLzmaObjlen},
                     P{kZlibPayload, sizeof(kZlibPayload), kZlibObjlen},
                     P{kZstdPayload, sizeof(kZstdPayload), kZstdObjlen}}) {
    auto b = bytes(x.p, x.n);
    for (int damage = 0; damage < 2; ++damage) {
      if (damage)
        b[9 + (b.size() - 9) / 2] ^= 0x10;
      const DecodedFrames v = verifyFrames(b.data(), b.size(), x.objlen);
      EXPECT_EQ(v.check, decode(b, x.objlen).check) << b[0] << b[1] << " damage " << damage;
      EXPECT_TRUE(v.out.empty());
    }
  }
  auto l = bytes(kLz4Payload, sizeof(kLz4Payload));
  EXPECT_EQ(verifyFrames(l.data(), l.size(), kLz4Objlen).check, FrameCheck::kPassed);
  l[l.size() - 5] ^= 0x01;
  EXPECT_EQ(verifyFrames(l.data(), l.size(), kLz4Objlen).check, FrameCheck::kFailed);
}

// The decoders' state is kept per thread and reused where the thread keeps them (the decode
// pool's): once such a thread has decoded an LZMA, a ZLIB and a ZSTD block, decoding many more (a
// damaged one among them) makes no large allocation (an LZMA dictionary) and gives the same bytes
// each time; zlib and ZSTD allocate nothing more.
TEST(FrameCheck, DecodersAreReusedAndGiveTheSameBytes) {
  // On threads that keep their decoders, as the decode pool's do.
  std::thread([] {
    keepDecodersOnThisThread();
    const auto want = expectedX();
    auto lz = bytes(kLzmaPayload, sizeof(kLzmaPayload));
    auto zl = bytes(kZlibPayload, sizeof(kZlibPayload));
    auto zs = bytes(kZstdPayload, sizeof(kZstdPayload));
    ASSERT_EQ(decode(lz, kLzmaObjlen).out, want);
    ASSERT_EQ(decode(zl, kZlibObjlen).out, want);
    ASSERT_EQ(decode(zs, kZstdObjlen).out, want);
    ASSERT_EQ(verifyFrames(lz.data(), lz.size(), kLzmaObjlen).check, FrameCheck::kPassed);
    // ROOT's LZMA level 9 is xz's preset 9: a 64 MiB dictionary, which liblzma allocates at each
    // set-up of its decoder.
    std::vector<uint8_t> x9(lzma_stream_buffer_bound(want.size()));
    size_t pos = 0;
    ASSERT_EQ(lzma_easy_buffer_encode(9, LZMA_CHECK_CRC32, nullptr, want.data(), want.size(),
                                      x9.data(), &pos, x9.size()),
              LZMA_OK);
    x9.resize(pos);
    const auto l9 = rootBlock('X', 'Z', x9, static_cast<uint32_t>(want.size()));
    ASSERT_EQ(decode(l9, want.size()).out, want);
    const uint64_t large = decoderLargeAllocations();
    auto bad = l9;
    bad[9 + (bad.size() - 9) / 2] ^= 0x10;
    for (int i = 0; i < 200; ++i) {
      EXPECT_EQ(decode(l9, want.size()).out, want);
      EXPECT_EQ(verifyFrames(l9.data(), l9.size(), want.size()).check, FrameCheck::kPassed);
      if (i % 50 == 0) {
        EXPECT_EQ(decode(bad, want.size()).check, FrameCheck::kFailed);
      }
    }
    EXPECT_EQ(decoderLargeAllocations(), large) << "an LZMA dictionary allocated again";
    const uint64_t all = decoderAllocations();
    for (int i = 0; i < 200; ++i) {
      EXPECT_EQ(decode(zl, kZlibObjlen).out, want);
      EXPECT_EQ(decode(zs, kZstdObjlen).out, want);
      EXPECT_EQ(verifyFrames(zl.data(), zl.size(), kZlibObjlen).check, FrameCheck::kPassed);
    }
    EXPECT_EQ(decoderAllocations(), all) << "zlib or ZSTD set up again";
    // Another thread sets up its own dictionary, once.
    uint64_t other = 0;
    std::thread([&] {
      keepDecodersOnThisThread();
      const uint64_t a = decoderLargeAllocations();
      for (int i = 0; i < 20; ++i)
        EXPECT_EQ(decode(l9, want.size()).out, want);
      other = decoderLargeAllocations() - a;
    }).join();
    EXPECT_EQ(other, 1u) << "one dictionary for the thread, not one a block";
  }).join();
}

// A thread that does not keep its decoders (a conversion thread, the executor's) holds no LZMA
// dictionary once a decode has returned, as liblzma's one-shot decoder did; one that keeps them
// holds one.
TEST(FrameCheck, AThreadThatDoesNotKeepDecodersHoldsNoDictionary) {
  const auto want = expectedX();
  std::vector<uint8_t> x9(lzma_stream_buffer_bound(want.size()));
  size_t pos = 0;
  ASSERT_EQ(lzma_easy_buffer_encode(9, LZMA_CHECK_CRC32, nullptr, want.data(), want.size(),
                                    x9.data(), &pos, x9.size()),
            LZMA_OK);
  x9.resize(pos);
  const auto l9 = rootBlock('X', 'Z', x9, static_cast<uint32_t>(want.size()));
  std::thread([&] {
    const int64_t live = decoderLargeLive();
    const uint64_t made = decoderLargeAllocations();
    for (int i = 0; i < 5; ++i) {
      EXPECT_EQ(decodeFrames(l9.data(), l9.size(), want.size()).out, want);
      EXPECT_EQ(decompressFrames(l9.data(), l9.size(), want.size()), want);
      EXPECT_EQ(verifyFrames(l9.data(), l9.size(), want.size()).check, FrameCheck::kPassed);
      EXPECT_EQ(decoderLargeLive(), live) << "a dictionary kept after the decode returned";
    }
    EXPECT_EQ(decoderLargeAllocations() - made, 15u) << "one dictionary for each object";
  }).join();
  std::thread([&] {
    keepDecodersOnThisThread();
    const int64_t live = decoderLargeLive();
    EXPECT_EQ(verifyFrames(l9.data(), l9.size(), want.size()).check, FrameCheck::kPassed);
    EXPECT_EQ(decoderLargeLive(), live + 1) << "a keeping thread holds its dictionary";
  }).join();
}

// The third byte of a block's header, as ROOT holds it (ZL 8, XZ 0, ZS 1, L4 1, CS 8): ROOT decodes
// nothing from a block whose third byte is another, and neither check passes it.
TEST(FrameCheck, HeaderThirdByteAsRootHoldsIt) {
  struct P {
    const uint8_t* p;
    size_t n;
    uint64_t objlen;
  };
  for (const P& x : {P{kLzmaPayload, sizeof(kLzmaPayload), kLzmaObjlen},
                     P{kZlibPayload, sizeof(kZlibPayload), kZlibObjlen},
                     P{kZstdPayload, sizeof(kZstdPayload), kZstdObjlen},
                     P{kLz4Payload, sizeof(kLz4Payload), kLz4Objlen}}) {
    auto b = bytes(x.p, x.n);
    EXPECT_NE(verifyFrames(b.data(), b.size(), x.objlen).check, FrameCheck::kFailed)
        << b[0] << b[1];
    for (int delta : {1, -1, 7}) {
      auto c = b;
      c[2] = static_cast<uint8_t>(c[2] + delta);
      EXPECT_EQ(verifyFrames(c.data(), c.size(), x.objlen).check, FrameCheck::kFailed)
          << b[0] << b[1] << " byte 2 " << int(c[2]);
      EXPECT_EQ(decode(c, x.objlen).check, FrameCheck::kFailed) << b[0] << b[1];
    }
  }
  auto cs = rootBlock('C', 'S', std::vector<uint8_t>(20, 0), 100);
  EXPECT_EQ(decode(cs, 100).check, FrameCheck::kNotCheckable) << "ROOT's old algorithm";
  cs[2] = 9;
  EXPECT_EQ(decode(cs, 100).check, FrameCheck::kFailed);
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
  // A basket stored uncompressed carries no check: not one left unexamined.
  std::vector<uint8_t> stored(kLzmaRecord, kLzmaRecord + 70);
  stored.insert(stored.end(), 40, 0x2a);
  const uint32_t nbytes = static_cast<uint32_t>(stored.size()), objlen = nbytes - 70;
  for (int i = 0; i < 4; ++i) {
    stored[i] = static_cast<uint8_t>(nbytes >> (24 - 8 * i));
    stored[6 + i] = static_cast<uint8_t>(objlen >> (24 - 8 * i));
  }
  ConvertedBasket r = convertBasket(stored.data(), stored.size(), 3 * stored.size(), lzma);
  ASSERT_TRUE(r.error.empty()) << r.error;
  EXPECT_EQ(r.kind, ConvertedBasket::kOriginal);
  EXPECT_EQ(r.check, ConvertedBasket::kNotCheckable);
  // An object no longer than its data (fObjlen <= fNbytes - fKeylen; 0 here): ROOT reads it as
  // stored, so it carries no check either, as the byte path's check counts it.
  std::vector<uint8_t> zero(kLzmaRecord, kLzmaRecord + sizeof(kLzmaRecord));
  for (int i = 6; i < 10; ++i)
    zero[i] = 0;
  ConvertedBasket z = convertBasket(zero.data(), zero.size(), 3 * zero.size(), lzma);
  EXPECT_EQ(z.kind, ConvertedBasket::kOriginal);
  EXPECT_EQ(z.check, ConvertedBasket::kNotCheckable);
  // ROOT's old algorithm ("CS", method 8) carries no check: not checkable, as the byte path says.
  std::vector<uint8_t> cs(kLzmaRecord, kLzmaRecord + sizeof(kLzmaRecord));
  cs[70] = 'C';
  cs[71] = 'S';
  cs[72] = 8;
  ConvertedBasket o = convertBasket(cs.data(), cs.size(), 3 * cs.size(), lzma);
  EXPECT_EQ(o.kind, ConvertedBasket::kOriginal);
  EXPECT_EQ(o.check, ConvertedBasket::kNotCheckable);
  // A tag ROOT does not know is not decoded: not examined (a decoder would judge it).
  std::vector<uint8_t> unk = cs;
  unk[70] = 'Q';
  unk[71] = 'Q';
  ConvertedBasket u = convertBasket(unk.data(), unk.size(), 3 * unk.size(), lzma);
  EXPECT_EQ(u.kind, ConvertedBasket::kOriginal);
  EXPECT_EQ(u.check, ConvertedBasket::kNotExamined);
  // Not a basket record at all: not examined.
  std::vector<uint8_t> notb(kLzmaRecord, kLzmaRecord + sizeof(kLzmaRecord));
  notb[35] = 'X'; // TBasket -> XBasket
  EXPECT_EQ(convertBasket(notb.data(), notb.size(), 3 * notb.size(), lzma).check,
            ConvertedBasket::kNotExamined);
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
  // The key's length alone damaged to a value below its key length (0 included): ROOT reads the
  // basket by the map's length and still takes it as compressed, so it reads it correctly: no
  // claim. A negative one makes the key a zombie: refused.
  for (int32_t v : {0, 1, kl - 1}) {
    r = rec;
    putBe32(r, 0, v);
    EXPECT_EQ(checkLen({S}, {nb}, spanWith(r, 0, S, 4096), S, hi).count, 0u) << "fNbytes " << v;
  }
  r = rec;
  putBe32(r, 0, -1);
  m = checkLen({S}, {nb}, spanWith(r, 0, S, 4096), S, hi);
  EXPECT_EQ(m.count, 1u) << "fNbytes < 0";
  EXPECT_TRUE(m.length);
  // A key length past the map's length: refused; with no length known, no claim on it.
  r = rec;
  r[14] = 0;
  r[15] = static_cast<uint8_t>(nb + 1);
  EXPECT_EQ(checkLen({S}, {nb}, spanWith(r, 0, S, 4096), S, hi).count, 1u) << "key past the map";
  EXPECT_EQ(checkLen({S}, {0}, spanWith(r, 0, S, 4096), S, hi).count, 0u) << "no length known";
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

// The block checksum on the byte path (checkFetched with data): every basket whose key passes,
// whose length the map gives and that lies wholly in the fetched bytes has its data decoded as ROOT
// decodes it; a block that fails its own checksum is a fault of that basket. ROOT's ZSTD carries
// none, and stored data none: no claim.
namespace {
// A basket record of `payload` at `seek`: the LZMA record's 64-bit key header (70 bytes, object
// length 8000) with its length set to the payload's.
std::vector<uint8_t> basketOf(const uint8_t* payload, size_t n, uint64_t seek) {
  std::vector<uint8_t> r = wideAt(seek);
  r.resize(70);
  putBe32(r, 0, static_cast<int32_t>(70 + n));
  r.insert(r.end(), payload, payload + n);
  return r;
}
FetchVerdict verdictOf(const std::vector<uint64_t>& seeks, const std::vector<int32_t>& lengths,
                       const Span& s, bool data = true) {
  return checkFetched(seeks, lengths, s.off, s.b.data(), s.b.size(), data);
}
struct Codec {
  const char* name;
  const uint8_t* p;
  size_t n;
  bool checked; // carries a checksum this build verifies
};
std::vector<Codec> codecs() {
  return {{"lzma", kLzmaPayload, sizeof(kLzmaPayload), true},
          {"zlib", kZlibPayload, sizeof(kZlibPayload), true},
          {"lz4", kLz4Payload, sizeof(kLz4Payload), true}, // its hash, in any build
          {"zstd", kZstdPayload, sizeof(kZstdPayload), false}};
}
} // namespace

TEST(BlockChecksum, RootWrittenBasketsOfEachCodec) {
  const uint64_t S = 300;
  for (const Codec& c : codecs()) {
    const std::vector<uint8_t> rec = basketOf(c.p, c.n, S);
    const int32_t nb = static_cast<int32_t>(rec.size());
    const Span sp = spanWith(rec, 0, S, 4096);
    // As ROOT wrote it: passes; checked where the codec carries a checksum.
    FetchVerdict v = verdictOf({S}, {nb}, sp);
    EXPECT_TRUE(v.faults.empty()) << c.name;
    EXPECT_EQ(v.passed, c.checked ? 1u : 0u) << c.name;
    EXPECT_EQ(v.none, c.checked ? 0u : 1u) << c.name;
    EXPECT_EQ(fetchedDataToCheck({S}, {nb}, sp.off, sp.b.data(), sp.b.size()), c.checked) << c.name;
    // One byte changed inside the compressed data, the key intact (its position and length
    // pass): refused where the codec carries a checksum, as the basket's own fault.
    std::vector<uint8_t> bad = rec;
    bad[70 + 9 + (c.n - 9) / 2] ^= 0x10;
    const Span bsp = spanWith(bad, 0, S, 4096);
    EXPECT_EQ(firstMisplacedKey({S}, {nb}, bsp.off, bsp.b.data(), bsp.b.size(), S, S + nb).count,
              0u)
        << c.name << ": the key passes";
    v = verdictOf({S}, {nb}, bsp);
    if (!c.checked) {
      EXPECT_TRUE(v.faults.empty()) << c.name << ": nothing to check it against";
      continue;
    }
    ASSERT_EQ(v.faults.size(), 1u) << c.name;
    EXPECT_EQ(v.faults[0].kind, FetchFault::kData);
    EXPECT_EQ(v.faults[0].at, S);
    EXPECT_EQ(v.faults[0].end, S + nb);
    std::string why;
    MisplacedKeys m = v.against(S, S + nb, &why);
    EXPECT_EQ(m.count, 1u);
    EXPECT_EQ(m.first, S);
    EXPECT_FALSE(m.length);
    EXPECT_EQ(why.rfind("its data fails its own check: ", 0), 0u) << why;
    // A reader whose bytes reach into the basket's data, not its key: still its fault.
    EXPECT_EQ(v.against(S + nb - 1, S + nb + 10).count, 1u) << c.name;
    // Bytes the reader did not ask for: outside (nothing of the fetch kept), not counted.
    m = v.against(S + nb, S + nb + 100);
    EXPECT_EQ(m.count, 0u);
    EXPECT_TRUE(m.outside);
    m = v.against(S, S);
    EXPECT_EQ(m.count, 0u);
    EXPECT_TRUE(m.outside);
    // Without the data check (as before), only the key: no fault.
    EXPECT_TRUE(verdictOf({S}, {nb}, bsp, false).faults.empty());
  }
}

TEST(BlockChecksum, WhatIsNotJudged) {
  const uint64_t S = 300;
  const std::vector<uint8_t> rec = basketOf(kZlibPayload, sizeof(kZlibPayload), S);
  const int32_t nb = static_cast<int32_t>(rec.size());
  std::vector<uint8_t> bad = rec;
  bad[70 + 40] ^= 0x10;
  // No length known (two records of one position disagree): ROOT's read length is not known.
  FetchVerdict v = verdictOf({S}, {0}, spanWith(bad, 0, S, 4096));
  EXPECT_TRUE(v.faults.empty());
  EXPECT_EQ(v.passed + v.none, 0u);
  // No map lengths at all.
  EXPECT_TRUE(verdictOf({S}, {}, spanWith(bad, 0, S, 4096)).faults.empty());
  // The basket not wholly in the fetched bytes (its key is): not judged.
  Span cut = spanWith(bad, 0, S, 4096);
  cut.b.resize(S + nb - 1);
  v = verdictOf({S}, {nb}, cut);
  EXPECT_TRUE(v.faults.empty());
  EXPECT_EQ(v.passed + v.none, 0u);
  EXPECT_FALSE(fetchedDataToCheck({S}, {nb}, cut.off, cut.b.data(), cut.b.size()));
  // Stored as it is (fObjlen == fNbytes - fKeylen): no checksum, counted none, never decoded.
  std::vector<uint8_t> stored = wideAt(S);
  stored.resize(70);
  std::vector<uint8_t> data(200, 0x11);
  putBe32(stored, 0, 270);
  putBe32(stored, 6, 200);
  stored.insert(stored.end(), data.begin(), data.end());
  const Span ssp = spanWith(stored, 0, S, 4096);
  v = verdictOf({S}, {270}, ssp);
  EXPECT_TRUE(v.faults.empty());
  EXPECT_EQ(v.none, 1u);
  EXPECT_FALSE(fetchedDataToCheck({S}, {270}, ssp.off, ssp.b.data(), ssp.b.size()));
  // ROOT's old algorithm (CS): no claim. A tag ROOT does not know: ROOT fails, and so does this.
  std::vector<uint8_t> cs = rec;
  cs[70] = 'C';
  cs[71] = 'S';
  v = verdictOf({S}, {nb}, spanWith(cs, 0, S, 4096));
  EXPECT_TRUE(v.faults.empty());
  EXPECT_EQ(v.none, 1u);
  std::vector<uint8_t> unk = rec;
  unk[70] = 'Q';
  v = verdictOf({S}, {nb}, spanWith(unk, 0, S, 4096));
  ASSERT_EQ(v.faults.size(), 1u);
  EXPECT_EQ(v.faults[0].why, "unknown compression block");
  // A key that fails is reported as the key's fault, and its data is not decoded.
  v = verdictOf({S}, {nb},
                spanWith(basketOf(kZlibPayload, sizeof(kZlibPayload), S + 1), 0, S, 4096));
  ASSERT_EQ(v.faults.size(), 1u);
  EXPECT_EQ(v.faults[0].kind, FetchFault::kPosition);
  EXPECT_EQ(v.passed + v.none, 0u);
}

TEST(BlockChecksum, MapLengthDecidesTheBytesDecoded) {
  // ROOT reads a basket by the map's length: a key whose own length alone differs (ROOT's
  // compressed decision unchanged) is decoded from the key's length to the map's.
  const uint64_t S = 300;
  std::vector<uint8_t> rec = basketOf(kLzmaPayload, sizeof(kLzmaPayload), S);
  const int32_t nb = static_cast<int32_t>(rec.size());
  putBe32(rec, 0, nb + 7); // the key says 7 bytes more: still compressed by both
  Span sp = spanWith(rec, 0, S, 4096);
  FetchVerdict v = verdictOf({S}, {nb}, sp);
  EXPECT_TRUE(v.faults.empty());
  EXPECT_EQ(v.passed, 1u);
}

TEST(BlockChecksum, PiecesThatFollowEachOtherAreJudgedWhole) {
  // A basket (its key header, or its data) that a cut between two pieces of one fetch divides is
  // judged as if fetched in one piece; apart, neither piece holds it whole.
  const uint64_t S = 300;
  const std::vector<uint8_t> rec = basketOf(kZlibPayload, sizeof(kZlibPayload), S);
  const int32_t nb = static_cast<int32_t>(rec.size());
  std::vector<uint8_t> bad = rec;
  bad[70 + 9 + 30] ^= 0x10;
  std::vector<uint8_t> zero = rec;
  std::fill(zero.begin(), zero.begin() + 26, 0); // the key header zeroed
  for (const std::vector<uint8_t>* r :
       std::vector<const std::vector<uint8_t>*>{&rec, &bad, &zero}) {
    const Span sp = spanWith(*r, 0, S, 4096);
    for (uint64_t cut : {S + 10, S + 20, S + 25, S + 70, S + 100, S + nb - 1}) {
      const std::vector<BytePiece> two{
          {0, reinterpret_cast<const char*>(sp.b.data()), cut},
          {cut, reinterpret_cast<const char*>(sp.b.data()) + cut, sp.b.size() - cut}};
      const FetchVerdict whole = verdictOf({S}, {nb}, sp);
      const FetchVerdict joined = checkPieces({S}, {nb}, two, true);
      EXPECT_EQ(joined.faults.size(), whole.faults.size()) << "cut " << cut;
      EXPECT_EQ(joined.passed, whole.passed) << "cut " << cut;
      EXPECT_TRUE(piecesNeedWork({S}, {nb}, two)) << "pieces to join";
      // Judged apart, the divided basket is not judged at all.
      FetchVerdict apart = checkFetched({S}, {nb}, 0, sp.b.data(), cut, true);
      apart.merge(checkFetched({S}, {nb}, cut, sp.b.data() + cut, sp.b.size() - cut, true));
      // A header the first piece holds whole is judged there (a zeroed one is read 32-bit: 22
      // bytes).
      if (r != &zero || cut < S + 22) {
        EXPECT_TRUE(apart.faults.empty()) << "cut " << cut;
      }
    }
    // In reverse order: joined all the same. With a gap: not joined.
    const std::vector<BytePiece> rev{
        {S + 50, reinterpret_cast<const char*>(sp.b.data()) + S + 50, sp.b.size() - S - 50},
        {0, reinterpret_cast<const char*>(sp.b.data()), S + 50}};
    EXPECT_EQ(checkPieces({S}, {nb}, rev, true).faults.size(),
              verdictOf({S}, {nb}, sp).faults.size());
    const std::vector<BytePiece> gap{
        {0, reinterpret_cast<const char*>(sp.b.data()), S + 50},
        {S + 51, reinterpret_cast<const char*>(sp.b.data()) + S + 51, sp.b.size() - S - 51}};
    EXPECT_EQ(checkPieces({S}, {nb}, gap, true).faults.size(), r == &zero ? 1u : 0u)
        << "only a header the first piece holds whole";
  }
  // One piece whose basket needs no decoding: no work to hand off.
  const Span plain = spanWith(rec, 0, S, 4096);
  const std::vector<BytePiece> one{{0, reinterpret_cast<const char*>(plain.b.data()), 200}};
  EXPECT_FALSE(piecesNeedWork({S}, {nb}, one));
}

// A map that shows its copy bad concerns only the damaged tree: its record and, where the record
// could be read, its baskets. Reads of another tree of the copy make no claim.
TEST(FileMap, ADamagedTreeConcernsOnlyItsOwnParts) {
  FileMeta fm;
  fm.treeKey.seekkey = 1000;
  fm.treeKey.nbytes = 200;
  EXPECT_EQ(treeSpans(fm, {}, {}), (std::vector<std::pair<uint64_t, uint64_t>>{{1000, 1200}}));
  EXPECT_EQ(treeSpans(fm, {100, 150, 300}, {50, 20, 0}),
            (std::vector<std::pair<uint64_t, uint64_t>>{{100, 170}, {300, 301}, {1000, 1200}}));
  fm.treeKey.nbytes = 0;
  EXPECT_TRUE(treeSpans(fm, {}, {}).empty()) << "no record found: nothing to hold against";
  const std::vector<std::pair<uint64_t, uint64_t>> spans{{1000, 1200}};
  std::vector<char> b(4096, 0);
  const std::string why = "the tree's record fails its own check";
  const auto at = [&](uint64_t off, uint64_t n) {
    return checkPieces({}, {}, {BytePiece{off, b.data(), n}}, true, &spans, why);
  };
  // Another tree's bytes, before and after the record: no claim.
  EXPECT_TRUE(at(0, 1000).faults.empty());
  EXPECT_TRUE(at(1200, 2000).faults.empty());
  // The record: refused, at the first byte the reader asked for inside it.
  std::string w;
  MisplacedKeys m = at(900, 400).against(1100, 1150, &w);
  EXPECT_EQ(m.count, 1u);
  EXPECT_EQ(m.first, 1100u);
  EXPECT_EQ(w, why);
  m = at(900, 400).against(950, 1010);
  EXPECT_EQ(m.count, 1u);
  EXPECT_EQ(m.first, 1000u);
  // The fetch holds part of the record, the reader's bytes do not: outside (nothing kept).
  m = at(900, 400).against(900, 950);
  EXPECT_EQ(m.count, 0u);
  EXPECT_TRUE(m.outside);
}

#ifdef UCACHE_TEST_DATA_DIR
// The fixture's baskets as ROOT wrote them (zlib and LZMA, ROOT reads every one): every basket the
// map places passes its data's own check; one byte changed inside any basket's compressed data,
// its key intact, is that basket's fault and no other's.
TEST(BlockChecksum, EveryBasketOfARootFile) {
  BytesSource src;
  FILE* fp =
      std::fopen((std::string(UCACHE_TEST_DATA_DIR) + "/unnamed_codec_fixture.root").c_str(), "rb");
  ASSERT_NE(fp, nullptr);
  uint8_t buf[65536];
  size_t r;
  while ((r = std::fread(buf, 1, sizeof buf, fp)) > 0)
    src.b.insert(src.b.end(), buf, buf + r);
  std::fclose(fp);
  const FileMeta fm = parseFile(src, static_cast<int64_t>(src.b.size()), "Events");
  ASSERT_TRUE(fm.error.empty()) << fm.error;
  std::vector<uint64_t> seeks;
  std::vector<int32_t> lengths;
  basketMap(fm, seeks, lengths);
  ASSERT_EQ(seeks.size(), 12u);
  FetchVerdict v = checkFetched(seeks, lengths, 0, src.b.data(), src.b.size(), true);
  EXPECT_TRUE(v.faults.empty());
  EXPECT_EQ(v.passed, 12u) << "every basket compressed, by zlib or LZMA";
  std::set<std::string> seen;
  for (size_t i = 0; i < seeks.size(); ++i) {
    const uint8_t* k = src.b.data() + seeks[i];
    const int keylen = k[14] << 8 | k[15];
    seen.insert(blockCodec(k + keylen, 9));
    std::vector<uint8_t> bad = src.b;
    bad[seeks[i] + keylen + 9 + (lengths[i] - keylen - 9) / 2] ^= 0x04;
    v = checkFetched(seeks, lengths, 0, bad.data(), bad.size(), true);
    ASSERT_EQ(v.faults.size(), 1u) << "basket at " << seeks[i];
    EXPECT_EQ(v.faults[0].at, seeks[i]);
    EXPECT_EQ(v.faults[0].kind, FetchFault::kData);
    EXPECT_EQ(v.passed, 11u);
    EXPECT_EQ(firstMisplacedKey(seeks, lengths, 0, bad.data(), bad.size(), 0, bad.size()).count, 0u)
        << "its key passes";
  }
  EXPECT_EQ(seen, (std::set<std::string>{"lzma", "zlib"}));
}
#endif

// An xz stream that declares no check (check type None, its header's own CRC32 holding) carries
// nothing to verify: it is not decompressed for the check, and counts as carrying none, even with
// its data damaged. The same block with its flag byte damaged (the header's CRC32 no longer
// holding) is decoded, and fails.
TEST(BlockChecksum, XzStreamDeclaringNoCheckIsNotDecoded) {
  const uint64_t S = 300;
  std::vector<uint8_t> pay(kLzmaPayload, kLzmaPayload + sizeof(kLzmaPayload));
  ASSERT_EQ(pay[9], 0xFD);
  ASSERT_EQ(pay[16], 0x01) << "ROOT writes CRC32";
  EXPECT_FALSE(dataCarriesNoCheck(pay.data(), pay.size()));
  std::vector<uint8_t> none = pay;
  none[16] = 0x00; // check type None, and the header's CRC32 of {00, 00}
  none[17] = 0xff;
  none[18] = 0x12;
  none[19] = 0xd9;
  none[20] = 0x41;
  EXPECT_TRUE(dataCarriesNoCheck(none.data(), none.size()));
  none[9 + 12 + (none.size() - 21) / 2] ^= 0x10; // damage inside its data: nothing to judge it by
  const std::vector<uint8_t> rec = basketOf(none.data(), none.size(), S);
  const int32_t nb = static_cast<int32_t>(rec.size());
  const FetchVerdict v = verdictOf({S}, {nb}, spanWith(rec, 0, S, 4096));
  EXPECT_TRUE(v.faults.empty());
  EXPECT_EQ(v.none, 1u);
  EXPECT_EQ(v.passed, 0u);
  // The flag byte itself damaged to None: the header's CRC32 does not hold, so it is decoded (and
  // the decoder fails it), not taken for a writer's choice.
  std::vector<uint8_t> flag = pay;
  flag[16] = 0x00;
  EXPECT_FALSE(dataCarriesNoCheck(flag.data(), flag.size()));
  const std::vector<uint8_t> frec = basketOf(flag.data(), flag.size(), S);
  const FetchVerdict fv =
      verdictOf({S}, {static_cast<int32_t>(frec.size())}, spanWith(frec, 0, S, 4096));
  EXPECT_EQ(fv.faults.size(), 1u);
}
