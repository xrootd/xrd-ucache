#include "Transposer.h"

#include <algorithm>
#include <cstring>

#include <zstd.h>

namespace ucache::transpose {
namespace {

constexpr uint64_t kMaxFrame = 0xFFFFFF; // 16 MiB - 1, ROOT per-frame limit

// raw bytes -> ROOT multi-frame ZSTD container ('ZS\x01' framing). The method
// byte stays 1 at every level (ROOT keys decompression on the 'ZS' magic, not
// the method byte).
std::vector<uint8_t> zstdFrames(const uint8_t* raw, size_t n, int level) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < n; i += kMaxFrame) {
    size_t chunk = std::min<size_t>(kMaxFrame, n - i);
    size_t cap = ZSTD_compressBound(chunk);
    size_t pre = out.size();
    out.resize(pre + 9 + cap);
    size_t got = ZSTD_compress(out.data() + pre + 9, cap, raw + i, chunk, level);
    if (ZSTD_isError(got))
      return {};
    out.resize(pre + 9 + got);
    uint8_t* h = out.data() + pre;
    h[0] = 'Z';
    h[1] = 'S';
    h[2] = 1;
    h[3] = got & 0xFF;
    h[4] = (got >> 8) & 0xFF;
    h[5] = (got >> 16) & 0xFF;
    h[6] = chunk & 0xFF;
    h[7] = (chunk >> 8) & 0xFF;
    h[8] = (chunk >> 16) & 0xFF;
  }
  return out;
}

} // namespace

// Every basket of `b` fully present in `src`?
bool fullyCached(const BranchInfo& b, Source& src) {
  if (b.writeBasket <= 0)
    return false;
  for (int32_t i = 0; i < b.writeBasket; ++i)
    if (!src.has(static_cast<uint64_t>(b.basketSeek[i]),
                 static_cast<uint64_t>(b.basketBytes[i])))
      return false;
  return true;
}

std::string branchCodec(const FileMeta& fm, const BranchInfo& b, Source& src) {
  (void)fm;
  if (b.writeBasket <= 0)
    return "";
  const uint64_t off = static_cast<uint64_t>(b.basketSeek[0]);
  const uint64_t nb = static_cast<uint64_t>(b.basketBytes[0]);
  uint8_t head[512];
  const uint64_t want = std::min<uint64_t>(nb, sizeof head);
  if (!src.has(off, want) || !src.read(head, want, off))
    return "";
  auto k = parseKey(head, static_cast<size_t>(want), 0); // head[0] IS the key start
  if (!k || k->keylen + 2u > want)
    return "";
  // Uncompressed basket: payload == object bytes.
  if (static_cast<uint64_t>(k->objlen) + k->keylen == static_cast<uint64_t>(k->nbytes))
    return "none";
  return blockCodec(head + k->keylen, static_cast<size_t>(want - k->keylen));
}

std::vector<uint8_t> encodeZstdFrames(const uint8_t* raw, size_t n, int level) {
  return zstdFrames(raw, n, level);
}

} // namespace ucache::transpose
