// What the recompressing code shares about a TTree file's branches: whether a
// branch's baskets are all cached, the codec they are stored in, and ROOT's
// ZSTD block framing (one encoder for baskets and RNTuple pages alike, so the
// two cannot drift).
//
// Thread-safety: pure functions over caller-owned data; no shared state.
#pragma once
#include "TreeMeta.h"
#include <cstdint>
#include <string>
#include <vector>
namespace ucache::transpose {
// `Source` (byte source with presence semantics) is declared in TreeMeta.h.
// Source codec of a branch's baskets, from basket 0's compression frame:
// "lzma" | "zlib" | "zstd" | "lz4" | "none" (uncompressed) | "" (unreadable).
std::string branchCodec(const FileMeta& fm, const BranchInfo& b, Source& src);
// Every basket of `b` fully present in `src`?
bool fullyCached(const BranchInfo& b, Source& src);
// raw bytes -> a ROOT multi-frame ZSTD container.
std::vector<uint8_t> encodeZstdFrames(const uint8_t* raw, size_t n, int level);
} // namespace ucache::transpose
