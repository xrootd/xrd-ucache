#include "TreeMeta.h"

#include <algorithm>

#include <cstring>
#include <fcntl.h>
#include <type_traits>
#include <unistd.h>

#include "vendor/xxh3.h"

#include <lzma.h>
#include <utility>
#include <zlib.h>
#include <zstd.h>
#ifdef UCACHE_HAVE_LZ4
#include <lz4.h>
#endif

// Layout sources of record: uproot 5.6 models (TTree v20 _ttree20_format1,
// TBranch v13 _tbranch13_format{1,2}, TLeaf v2 _tleaf2_format0, TIOFeatures,
// TObjArray) — the same independent implementation that verified the spike.
namespace ucache::transpose {
namespace {

constexpr uint32_t kByteCountMask = 0x40000000;
constexpr uint32_t kNewClassTag = 0xFFFFFFFF;
constexpr uint32_t kClassMask = 0x80000000;
constexpr uint32_t kIsReferenced = 1u << 4;

template <typename T> T beGet(const uint8_t* p) {
  using U = std::make_unsigned_t<T>;
  U v{};
  for (size_t i = 0; i < sizeof(T); ++i)
    v = static_cast<U>((v << 8) | p[i]);
  T out;
  std::memcpy(&out, &v, sizeof(T));
  return out;
}

// Big-endian cursor over the decompressed metadata blob. All read methods
// set fail_ instead of throwing (no exceptions cross the plugin ABI).
struct Cur {
  const uint8_t* p;
  size_t n;
  size_t at = 0;
  bool fail_ = false;
  std::string why;
  // Back-reference bias is EXACT, not learned: ROOT's TBufferFile maps
  // streamed objects at (position within the full key record + kMapOffset=2),
  // and this blob excludes the key header — so an object first streamed at
  // blob offset P is referenced as P + keylen + 2, and a class tag written at
  // blob offset T is referenced as T + keylen + 2 (we record the class NAME
  // position, tag field + 4, hence the -4 below).
  uint16_t keylen = 0;

  bool need(size_t k) {
    if (fail_ || at + k > n) {
      if (!fail_) {
        fail_ = true;
        why = "truncated at " + std::to_string(at);
      }
      return false;
    }
    return true;
  }
  template <typename T> T get() {
    if (!need(sizeof(T)))
      return T{};
    T v = beGet<T>(p + at);
    at += sizeof(T);
    return v;
  }
  void skip(size_t k) { need(k) ? void(at += k) : void(); }
  std::string tstring() {
    uint32_t len = get<uint8_t>();
    if (len == 255)
      len = get<uint32_t>();
    if (!need(len))
      return {};
    std::string s(reinterpret_cast<const char*>(p + at), len);
    at += len;
    return s;
  }
  // [bytecount|kByteCountMask][version]; returns end-of-object position.
  size_t objHeader(uint16_t& ver, const char* what) {
    uint32_t bc = get<uint32_t>();
    if (!(bc & kByteCountMask)) {
      fail_ = true;
      why = std::string(what) + ": missing byte count";
      return 0;
    }
    size_t end = at + (bc & ~kByteCountMask);
    ver = get<uint16_t>();
    return end;
  }
  void skipObj(const char* what) {
    uint16_t v;
    size_t end = objHeader(v, what);
    if (!fail_)
      need(end - at) ? void(at = end) : void();
  }
  void tobject() {
    get<uint16_t>(); // version
    get<uint32_t>(); // fUniqueID
    uint32_t bits = get<uint32_t>();
    if (bits & kIsReferenced)
      get<uint16_t>();
  }
};

// One entry of a TObjArray: either null, a back-reference (`ref` set), or an
// inline object (`cls` + body up to `end`; cursor left at the body start).
struct AnyObj {
  bool null = false;
  uint32_t ref = 0;   // nonzero => back-reference to an earlier object
  std::string cls;    // resolved class name for inline objects
  size_t end = 0;     // one past the object (skip target)
  size_t tagPos = 0;  // blob offset of the first u32 (registration anchor)
};

// read_object_any per TBuffer conventions; classRefs maps the blob offset of
// a kNewClassTag (+ kMapOffset bias handled by the caller's registry) to its
// class name for tag back-references.
AnyObj readAny(Cur& c, std::vector<std::pair<uint32_t, std::string>>& classRefs) {
  AnyObj o;
  o.tagPos = c.at;
  uint32_t first = c.get<uint32_t>();
  if (first == 0) {
    o.null = true;
    return o;
  }
  if (!(first & kByteCountMask)) {
    o.ref = first; // back-reference to an already-streamed object
    return o;
  }
  o.end = c.at + (first & ~kByteCountMask);
  uint32_t tag = c.get<uint32_t>();
  if (tag == kNewClassTag) {
    size_t namePos = c.at;
    std::string cls;
    while (!c.fail_ && c.at < c.n && c.p[c.at] != 0)
      cls.push_back(static_cast<char>(c.p[c.at++]));
    c.skip(1); // NUL
    o.cls = cls;
    // ROOT registers the class at (tag position + kMapOffset); empirically
    // the reference value equals namePos - 4 + 2... calibrated below via the
    // registry the caller maintains keyed on tagPos-relative offsets.
    classRefs.emplace_back(static_cast<uint32_t>(namePos), cls);
  } else if (tag & kClassMask) {
    const int64_t off = static_cast<int64_t>(tag & ~kClassMask);
    for (auto& [pos, cls] : classRefs)
      if (static_cast<int64_t>(pos) - 4 + c.keylen + 2 == off) {
        o.cls = cls;
        break;
      }
    if (o.cls.empty())
      o.cls = "?ref";
  } else {
    c.fail_ = true;
    c.why = "object tag without class at " + std::to_string(o.tagPos);
  }
  return o;
}

} // namespace

std::optional<KeyInfo> parseKey(const uint8_t* p, size_t n, uint64_t off) {
  if (off + 34 > n)
    return std::nullopt;
  const uint8_t* k = p + off;
  KeyInfo o;
  o.nbytes = beGet<int32_t>(k);
  // TKey::ReadKeyBuffer: the version is a signed Version_t, and above 1000
  // marks 64-bit positions; a 32-bit fSeekKey is read unsigned (UInt_t).
  o.ver = beGet<int16_t>(k + 4);
  o.objlen = beGet<int32_t>(k + 6);
  o.keylen = beGet<uint16_t>(k + 14);
  o.cycle = beGet<uint16_t>(k + 16);
  size_t q;
  if (o.ver > 1000) {
    o.seekkey = beGet<int64_t>(k + 18);
    q = 34;
  } else {
    o.seekkey = beGet<uint32_t>(k + 18);
    q = 26;
  }
  auto str = [&](std::string& s) {
    if (off + q >= n)
      return false;
    uint8_t len = k[q++];
    if (off + q + len > n)
      return false;
    s.assign(reinterpret_cast<const char*>(k + q), len);
    q += len;
    return true;
  };
  if (!str(o.cls) || !str(o.name) || !str(o.title))
    return std::nullopt;
  o.keyPartLen = static_cast<uint16_t>(q);
  return o;
}

std::optional<BasketHeader> parseBasketHeader(const uint8_t* p, size_t n, uint64_t off,
                                              const KeyInfo& k) {
  // TBasket::Streamer, after the TKey part: version (2), fBufferSize,
  // fNevBufSize, fNevBuf, fLast (4 each), flag (1).
  constexpr size_t kFields = 2 + 4 * 4 + 1;
  const uint64_t at = off + k.keyPartLen;
  if (k.keyPartLen == 0 || at > n || n - at < kFields)
    return std::nullopt;
  const uint8_t* b = p + at;
  BasketHeader h;
  h.version = beGet<int16_t>(b);
  h.bufferSize = beGet<int32_t>(b + 2);
  h.nevBufSize = beGet<int32_t>(b + 6);
  h.nevBuf = beGet<int32_t>(b + 10);
  h.last = beGet<int32_t>(b + 14);
  h.flag = b[18];
  return h;
}

BasketKeyCheck checkBasketKey(const KeyInfo& k, const BasketHeader& b, const BranchInfo& br,
                              size_t basket, const std::string& treeName) {
  BasketKeyCheck c;
  auto fail = [&](std::string why) {
    c.fail = true;
    c.problems.push_back(std::move(why));
  };
  if (basket >= br.basketSeek.size() || basket >= br.basketBytes.size()) {
    fail("basket index past the branch's record");
    return c;
  }
  // ROOT itself refuses a basket whose key does not say it sits where the
  // branch says it does.
  if (k.seekkey != br.basketSeek[basket])
    fail("key position differs from the branch's record");
  // The basket fields sit inside the key header, so it must at least hold
  // them, and it cannot be longer than the record.
  if (k.keylen < k.keyPartLen + 19 || k.keylen > k.nbytes)
    fail("key header length inconsistent");
  if (k.objlen < 0)
    fail("negative object length");
  // ROOT reads the entry data up to fLast and the entry offsets after it.
  else if (b.last < k.keylen || static_cast<int64_t>(b.last) > int64_t{k.keylen} + k.objlen)
    fail("end of entry data outside the basket");
  if (basket + 1 < br.basketEntry.size() &&
      int64_t{b.nevBuf} != br.basketEntry[basket + 1] - br.basketEntry[basket])
    fail("entry count differs from the branch's record");
  // ROOT reads fBasketBytes[i] bytes, whatever the key says.
  if (k.nbytes != br.basketBytes[basket])
    c.anomalies.push_back("record length differs from the branch's record");
  if (k.cls != "TBasket")
    c.anomalies.push_back("key class is not TBasket");
  if (k.name != br.name)
    c.anomalies.push_back("key name is not the branch's");
  if (!treeName.empty() && k.title != treeName)
    c.anomalies.push_back("key title is not the tree's");
  return c;
}

void basketMap(const FileMeta& fm, std::vector<uint64_t>& seeks, std::vector<int32_t>& bytes) {
  std::vector<std::pair<uint64_t, int32_t>> all;
  for (const auto& b : fm.branches)
    if (!b.externalFile)
      for (size_t i = 0; i < b.basketSeek.size(); ++i)
        if (b.basketSeek[i] > 0)
          all.emplace_back(static_cast<uint64_t>(b.basketSeek[i]),
                           i < b.basketBytes.size() && b.basketBytes[i] > 0 ? b.basketBytes[i] : 0);
  std::sort(all.begin(), all.end());
  seeks.clear();
  bytes.clear();
  for (const auto& [s, n] : all) {
    if (!seeks.empty() && seeks.back() == s) {
      if (bytes.back() != n)
        bytes.back() = 0; // two records of one position disagree: no length to hold it to
      continue;
    }
    seeks.push_back(s);
    bytes.push_back(n);
  }
}

std::string basketMapBad(const FileMeta& fm, const std::vector<uint64_t>& seeks,
                         const std::vector<int32_t>& bytes, uint64_t fileSize) {
  if (!fm.error.empty())
    return fm.recordFailed ? "the tree's record fails its own check" : "";
  for (size_t i = 0; i < seeks.size() && i < bytes.size(); ++i)
    if (bytes[i] > 0 && (seeks[i] > fileSize || uint64_t(bytes[i]) > fileSize - seeks[i]))
      return "the tree's record places a basket at " + std::to_string(seeks[i]) + " of " +
             std::to_string(bytes[i]) + " bytes past the copy's end at " +
             std::to_string(fileSize);
  return "";
}

std::vector<uint64_t> basketSeeks(const FileMeta& fm) {
  std::vector<uint64_t> s;
  for (const auto& b : fm.branches)
    if (!b.externalFile)
      for (int64_t seek : b.basketSeek)
        if (seek > 0)
          s.push_back(static_cast<uint64_t>(seek));
  std::sort(s.begin(), s.end());
  s.erase(std::unique(s.begin(), s.end()), s.end());
  return s;
}

MisplacedKeys firstMisplacedKey(const std::vector<uint64_t>& seeks, uint64_t spanOff,
                                const void* bytes, uint64_t n, uint64_t lo, uint64_t hi) {
  return firstMisplacedKey(seeks, std::vector<int32_t>(), spanOff, bytes, n, lo, hi);
}

MisplacedKeys firstMisplacedKey(const std::vector<uint64_t>& seeks,
                                const std::vector<int32_t>& lengths, uint64_t spanOff,
                                const void* bytes, uint64_t n, uint64_t lo, uint64_t hi) {
  const auto* p = static_cast<const uint8_t*>(bytes);
  const bool withLengths = lengths.size() == seeks.size();
  MisplacedKeys m;
  for (auto it = std::lower_bound(seeks.begin(), seeks.end(), spanOff);
       it != seeks.end() && *it - spanOff < n; ++it) {
    // The key's version (bytes 4-5) says how wide fSeekKey (from byte 18) is,
    // read as TKey reads it: a signed version above 1000 is 64-bit, and a
    // 32-bit fSeekKey is unsigned. A version whose top bit is set is negative,
    // so ROOT reads that key as 32-bit, and so does this check.
    // Seeks only grow from here, so once a header no longer fits none will.
    const uint64_t at = *it - spanOff;
    if (n - at < 6)
      break;
    const bool wide = beGet<int16_t>(p + at + 4) > 1000;
    if (n - at < (wide ? 26u : 22u))
      break;
    const int64_t key = wide ? beGet<int64_t>(p + at + 18) : int64_t{beGet<uint32_t>(p + at + 18)};
    bool fails = key != static_cast<int64_t>(*it);
    bool byLength = false;
    if (!fails && withLengths) {
      // The key's own fields, read as TKey::ReadKeyBuffer reads them (fNbytes and fObjlen signed
      // 32-bit, fKeylen signed 16-bit), against the map's length of the basket.
      const int32_t want = lengths[static_cast<size_t>(it - seeks.begin())];
      const int32_t nbytes = beGet<int32_t>(p + at);
      const int32_t objlen = beGet<int32_t>(p + at + 6);
      const int16_t keylen = beGet<int16_t>(p + at + 14);
      byLength = (want > 0 && nbytes != want) || keylen <= 0 || keylen > nbytes || objlen < 0;
      fails = byLength;
    }
    if (!fails)
      continue;
    if (*it >= lo && *it < hi) {
      if (m.count++ == 0) {
        m.first = *it;
        m.length = byLength;
      }
    } else {
      m.outside = true;
    }
  }
  return m;
}

std::string blockCodec(const uint8_t* p, size_t n) {
  if (n < 9)
    return "";
  if (p[0] == 'X' && p[1] == 'Z')
    return "lzma";
  if (p[0] == 'Z' && p[1] == 'L')
    return "zlib";
  if (p[0] == 'Z' && p[1] == 'S')
    return "zstd";
  if (p[0] == 'L' && p[1] == '4')
    return "lz4";
  return "";
}

DecodedFrames decodeFrames(const uint8_t* payload, size_t n, uint64_t objlen) {
  DecodedFrames d;
  auto failed = [&](const char* why) {
    d.check = FrameCheck::kFailed;
    d.why = why;
    d.out.clear();
    return std::move(d);
  };
  auto unsupported = [&](const char* why) {
    d.check = FrameCheck::kNotCheckable;
    d.why = why;
    d.out.clear();
    return std::move(d);
  };
  // objlen is attacker-influenceable: cap the upfront reservation (growth past
  // it amortizes per block) so a hostile value cannot force a giant allocation.
  d.out.reserve(std::min<uint64_t>(objlen, 64ull << 20));
  bool everyBlockChecked = true;
  size_t p = 0;
  while (d.out.size() < objlen) {
    if (p > n || n - p < 9)
      return failed("compression block header past the payload");
    const std::string codec = blockCodec(payload + p, n - p);
    if (codec.empty()) {
      // ROOT's old algorithm (CS) is legitimate and not decoded here; any
      // other tag is one ROOT itself does not know, so the bytes are damaged.
      if (payload[p] == 'C' && payload[p + 1] == 'S')
        return unsupported("compression block of ROOT's old algorithm");
      return failed("unknown compression block");
    }
    uint64_t csize = payload[p + 3] | payload[p + 4] << 8 | payload[p + 5] << 16;
    uint64_t usize = payload[p + 6] | payload[p + 7] << 8 | payload[p + 8] << 16;
    if (usize == 0)
      return failed("empty compression block");
    const uint64_t availIn = n - p - 9;
    const uint64_t wantOut = objlen - d.out.size();
    // ROOT decodes each block with the sizes its header states and stops once
    // the object is complete, so a LAST block whose header claims more input or
    // more output than there is still reads correctly in ROOT; its own check
    // decides. Smaller values, or larger ones in a block that is not the last,
    // fail in ROOT too. LZ4 is the exception: its hash covers exactly the
    // compressed size the header states.
    if (csize > availIn || usize > wantOut) {
      if (usize < wantOut)
        return failed("compression block runs past the payload");
      if (csize > availIn && codec == "lz4")
        return failed("lz4 block runs past the payload");
      csize = std::min(csize, availIn);
      usize = wantOut;
      d.headerTolerated = true;
    }
    const uint8_t* blob = payload + p + 9;
    const size_t pre = d.out.size();
    d.out.resize(pre + usize);
    uint8_t* dst = d.out.data() + pre;
    if (codec == "lzma") {
      // An xz stream names its check in the second byte of its stream flags;
      // ROOT writes CRC32, and liblzma verifies whatever is named.
      if (csize < 8)
        return failed("lzma block too short");
      if ((blob[7] & 0x0F) == 0)
        everyBlockChecked = false;
      uint64_t memlim = UINT64_MAX;
      size_t inPos = 0, outPos = 0;
      if (lzma_stream_buffer_decode(&memlim, 0, nullptr, blob, &inPos, csize, dst, &outPos,
                                    usize) != LZMA_OK ||
          outPos != usize)
        return failed("lzma block failed its check or did not decode");
    } else if (codec == "zlib") {
      // The zlib wrapper ends with an adler32 that uncompress() verifies.
      uLongf dl = usize;
      if (::uncompress(dst, &dl, blob, csize) != Z_OK || dl != usize)
        return failed("zlib block failed its check or did not decode");
    } else if (codec == "zstd") {
      // Frame header: 4-byte magic, then the descriptor whose bit 2 says a
      // content checksum follows the frame (ZSTD verifies it when present).
      if (csize < 5)
        return failed("zstd block too short");
      if ((blob[4] & 0x04) == 0)
        everyBlockChecked = false;
      if (ZSTD_decompress(dst, usize, blob, csize) != usize)
        return failed("zstd block did not decode");
    } else { // lz4
#ifdef UCACHE_HAVE_LZ4
      // ROOT's LZ4 block: an 8-byte XXH64 (big-endian) of the compressed bytes
      // that follow it, then the LZ4 data.
      if (csize < 8)
        return failed("lz4 block too short");
      uint64_t want = 0;
      for (int b = 0; b < 8; ++b)
        want = (want << 8) | blob[b];
      if (xxh64(blob + 8, csize - 8, 0) != want)
        return failed("lz4 block failed its check");
      if (LZ4_decompress_safe(reinterpret_cast<const char*>(blob + 8), reinterpret_cast<char*>(dst),
                              static_cast<int>(csize - 8),
                              static_cast<int>(usize)) != static_cast<int>(usize))
        return failed("lz4 block did not decode");
#else
      return unsupported("lz4 block, and this build has no lz4");
#endif
    }
    p += 9 + csize;
  }
  d.check = everyBlockChecked ? FrameCheck::kPassed : FrameCheck::kNotCheckable;
  if (!everyBlockChecked)
    d.why = "a block carries no check";
  return d;
}

std::vector<uint8_t> decompressFrames(const uint8_t* payload, size_t n, uint64_t objlen) {
  DecodedFrames d = decodeFrames(payload, n, objlen);
  if (d.check == FrameCheck::kFailed)
    return {};
  return std::move(d.out); // empty for a block this build cannot decode
}

namespace {

// Streamed-TObjArray walk calling `element` for each non-null inline object;
// the callback must leave the cursor at o.end.
template <typename F>
bool objArray(Cur& c, std::vector<std::pair<uint32_t, std::string>>& classRefs,
              const char* what, F element) {
  uint16_t ver;
  size_t end = c.objHeader(ver, what);
  if (c.fail_)
    return false;
  if (ver != 3) {
    c.fail_ = true;
    c.why = std::string(what) + ": TObjArray v" + std::to_string(ver) + " unsupported";
    return false;
  }
  c.tobject();
  c.tstring(); // fName
  int32_t size = c.get<int32_t>();
  c.get<int32_t>(); // fLowerBound
  for (int32_t i = 0; i < size && !c.fail_; ++i) {
    AnyObj o = readAny(c, classRefs);
    if (o.null || o.ref)
      continue;
    if (!element(o))
      return false;
    if (!c.fail_)
      c.at = o.end;
  }
  c.at = end;
  return !c.fail_;
}

// TLeaf v2 base + name/title + counter back-ref; consumed via outer bytecount.
bool parseLeaf(Cur& c, const AnyObj& o, BranchInfo& b,
               std::vector<std::pair<uint32_t, std::string>>& classRefs,
               std::vector<std::pair<uint32_t, std::string>>& leafRegistry) {
  b.leafClass = o.cls;
  uint16_t lver;
  c.objHeader(lver, "TLeafX"); // subclass header (v1)
  uint16_t bver;
  size_t bend = c.objHeader(bver, "TLeaf");
  if (c.fail_)
    return false;
  if (bver != 2) {
    c.fail_ = true;
    c.why = "TLeaf v" + std::to_string(bver) + " unsupported";
    return false;
  }
  { // TNamed: name + title
    uint16_t nv;
    c.objHeader(nv, "TNamed");
    c.tobject();
    std::string name = c.tstring();
    b.leafTitle = c.tstring();
    // Register this leaf for counter back-references: ROOT maps objects at
    // (buffer position of the object's first u32) + keylen + kMapOffset;
    // we store tagPos and resolve with the calibrated bias below.
    leafRegistry.emplace_back(static_cast<uint32_t>(o.tagPos), name);
  }
  c.skip(4 * 3 + 2); // fLen, fLenType, fOffset (i4 each), fIsRange, fIsUnsigned
  AnyObj cnt = readAny(c, classRefs);
  if (cnt.ref) {
    // Exact resolution (header comment): referenced object was first
    // streamed at blob offset `pos`, so its reference is pos + keylen + 2.
    for (auto& [pos, name] : leafRegistry)
      if (static_cast<int64_t>(pos) + c.keylen + 2 == static_cast<int64_t>(cnt.ref)) {
        b.leafCount = name;
        break;
      }
  } else if (!cnt.null && !c.fail_) {
    c.at = cnt.end; // inline counter object (first reference) — not expected
  }
  (void)bend;
  return !c.fail_;
}

// TBranch v13; the caller (objArray) repositions to the outer bytecount end.
bool parseBranch(Cur& c, FileMeta& fm,
                 std::vector<std::pair<uint32_t, std::string>>& classRefs,
                 std::vector<std::pair<uint32_t, std::string>>& leafRegistry) {
  uint16_t ver;
  size_t end = c.objHeader(ver, "TBranch");
  if (c.fail_)
    return false;
  if (ver != 13) {
    c.fail_ = true;
    c.why = "TBranch v" + std::to_string(ver) + " unsupported";
    return false;
  }
  BranchInfo b;
  { // TNamed (need fName)
    uint16_t nv;
    c.objHeader(nv, "TNamed");
    c.tobject();
    b.name = c.tstring();
    c.tstring(); // title
  }
  c.skipObj("TAttFill");
  c.skip(4 * 4 + 8);         // fCompress..fWriteBasket (i4 x4), fEntryNumber (q)
  if (c.fail_)
    return false;
  b.compress = beGet<int32_t>(c.p + c.at - 24);
  b.writeBasket = beGet<int32_t>(c.p + c.at - 12);
  c.skipObj("TIOFeatures");
  c.skip(4);                 // fOffset
  b.maxBaskets = c.get<uint32_t>();
  c.skip(4);                 // fSplitLevel
  b.entries = c.get<int64_t>();
  c.skip(8 * 3);             // fFirstEntry, fTotBytes, fZipBytes
  if (c.fail_)
    return false;
  b.zipBytesOff = c.at - 8;
  b.zipBytes = beGet<int64_t>(c.p + c.at - 8);
  b.totBytes = beGet<int64_t>(c.p + c.at - 16);
  if (!objArray(c, classRefs, "fBranches(sub)", [&](const AnyObj&) {
        c.fail_ = true;
        c.why = "sub-branches unsupported (flat trees only)";
        return false;
      }))
    return false;
  if (!objArray(c, classRefs, "fLeaves", [&](const AnyObj& lo) {
        return parseLeaf(c, lo, b, classRefs, leafRegistry);
      }))
    return false;
  { // fBaskets: skip whole array via byte count
    c.skipObj("fBaskets");
  }
  if (b.maxBaskets > (1u << 24) || c.fail_) {
    c.fail_ = true;
    if (c.why.empty())
      c.why = "implausible fMaxBaskets";
    return false;
  }
  // fWriteBasket indexes the three arrays below, each sized fMaxBaskets: a
  // negative or oversized value would size an allocation from hostile bytes
  // and read past the arrays. This walk runs on metadata that arrived over
  // the network, so it is checked here, once, before any of the three loops.
  if (b.writeBasket < 0 || static_cast<uint32_t>(b.writeBasket) > b.maxBaskets) {
    c.fail_ = true;
    c.why = "implausible fWriteBasket";
    return false;
  }
  const size_t nb = static_cast<size_t>(b.writeBasket);
  c.skip(1);
  b.bytesArrayOff = c.at;
  if (!c.need(4ull * b.maxBaskets))
    return false;
  b.basketBytes.resize(nb);
  for (size_t i = 0; i < nb; ++i)
    b.basketBytes[i] = beGet<int32_t>(c.p + c.at + 4 * i);
  c.skip(4ull * b.maxBaskets);
  c.skip(1);
  if (!c.need(8ull * b.maxBaskets)) // fBasketEntry: the entry axis
    return false;
  const size_t ne = std::min<size_t>(nb + 1, b.maxBaskets);
  b.basketEntry.resize(nb + 1, b.entries);
  for (size_t i = 0; i < ne; ++i)
    b.basketEntry[i] = beGet<int64_t>(c.p + c.at + 8 * i);
  if (nb && b.basketEntry[nb] < b.basketEntry[nb - 1])
    b.basketEntry[nb] = b.entries; // an unflushed last basket ends at the branch's end
  c.skip(8ull * b.maxBaskets);
  c.skip(1);
  b.seekArrayOff = c.at;
  if (!c.need(8ull * b.maxBaskets))
    return false;
  b.basketSeek.resize(nb);
  for (size_t i = 0; i < nb; ++i)
    b.basketSeek[i] = beGet<int64_t>(c.p + c.at + 8 * i);
  { // fFileName: a TString right after the seek array, empty unless the
    // branch's baskets were written to ANOTHER file. Its first byte is the
    // length (255 escapes to a longer one), so non-zero means "set".
    const size_t fn = c.at + 8ull * b.maxBaskets;
    if (fn < end && fn < c.n)
      b.externalFile = c.p[fn] != 0;
  }
  c.at = end; // fFileName + any tail via the outer byte count
  fm.branches.push_back(std::move(b));
  return true;
}

} // namespace

namespace {

// Where the container walk reads from: a descriptor, or a caller's Source.
// Reads are all-or-nothing; a short read is a failed read, and a Source that
// cannot vouch for a range (`has` false) fails it too — the walk never guesses.
struct Reader {
  virtual ~Reader() = default;
  virtual bool read(void* dst, size_t n, int64_t off) = 0;
  int64_t size = 0;
};
struct FdReader : Reader {
  int fd;
  explicit FdReader(int f) : fd(f) { size = ::lseek(fd, 0, SEEK_END); }
  bool read(void* dst, size_t n, int64_t off) override {
    return off >= 0 && ::pread(fd, dst, n, off) == static_cast<ssize_t>(n);
  }
};
struct SourceReader : Reader {
  Source& src;
  SourceReader(Source& s, int64_t sz) : src(s) { size = sz; }
  bool read(void* dst, size_t n, int64_t off) override {
    if (off < 0)
      return false;
    const uint64_t o = static_cast<uint64_t>(off);
    return src.has(o, n) && src.read(dst, n, o);
  }
};

ContainerMeta parseContainerFrom(Reader& r) {
  ContainerMeta fm;
  auto fail = [&](std::string why) {
    fm.error = std::move(why);
    fm.keys.clear();
    return fm;
  };

  const int64_t fsz = r.size;
  fm.fileSize = fsz;
  std::vector<uint8_t> head(static_cast<size_t>(std::clamp<int64_t>(fsz, 0, 512)));
  // 20, not 4: the version and fEND below are read at +4 and +12. Sizing the
  // buffer to the file (right, for a Source that must not read past the end)
  // removed the slack that a fixed 512-byte buffer used to provide, and a
  // file of a few bytes starting with "root" then read past the allocation.
  if (head.size() < 20 || !r.read(head.data(), head.size(), 0) ||
      std::memcmp(head.data(), "root", 4) != 0)
    return fail("not a ROOT file");
  // TFile::Init: a signed version below 1000000 has 32-bit fields.
  fm.large = beGet<int32_t>(head.data() + 4) >= 1000000;
  fm.headerSeekWidth = fm.large ? 8 : 4;
  fm.fend = fm.large ? beGet<int64_t>(head.data() + 12) : beGet<int32_t>(head.data() + 12);

  // Root directory record at fBEGIN=100: key, TNamed payload, TDirectory
  // (fVersion, 2x datime, fNbytesKeys, fNbytesName, 3 seeks by width).
  auto dirKey = parseKey(head.data(), head.size(), 100);
  if (!dirKey)
    return fail("no root directory key");
  {
    // The whole walk is bounds-checked against the header buffer: keylen and
    // the TString lengths are hostile bytes (a bad keylen once walked the
    // cursor 64 KiB past `head` — found by transpose-fuzz).
    size_t q = 100 + dirKey->keylen;
    bool oob = false;
    // TNamed inline (no byte count here): two TStrings.
    auto pstr = [&](size_t& at) {
      if (at >= head.size()) {
        oob = true;
        return;
      }
      uint32_t len = head[at++];
      if (len == 255) {
        if (at + 4 > head.size()) {
          oob = true;
          return;
        }
        len = beGet<uint32_t>(head.data() + at);
        at += 4;
      }
      if (len > head.size() - at) {
        oob = true;
        return;
      }
      at += len;
    };
    pstr(q);
    pstr(q);
    if (oob || q + 2 > head.size())
      return fail("directory record out of bounds");
    // A signed Version_t, as TFile::Init and TDirectoryFile::ReadKeys read it.
    const int16_t dver = beGet<int16_t>(head.data() + q);
    q += 2 + 4 + 4 + 4 + 4; // version, datimeC, datimeM, fNbytesKeys, fNbytesName
    bool wide = dver > 1000;
    q += (wide ? 8 : 4) * 2; // fSeekDir, fSeekParent
    if (q + (wide ? 8u : 4u) > head.size())
      return fail("directory record out of bounds");
    fm.keyslistSeek = wide ? beGet<int64_t>(head.data() + q) : beGet<int32_t>(head.data() + q);
    fm.dirSeekKeysOff = q;
    // A narrow directory record under a 64-bit header is a valid layout, not a
    // corrupt file: each width is used only where it applies (see FileMeta).
    fm.dirSeekWidth = wide ? 8 : 4;
  }

  // Keys list.
  {
    auto kl = [&]() -> std::optional<KeyInfo> {
      if (fm.keyslistSeek < 0 || fm.keyslistSeek >= fsz)
        return std::nullopt;
      std::vector<uint8_t> buf(static_cast<size_t>(std::min<int64_t>(4096, fsz - fm.keyslistSeek)));
      if (buf.size() < 34 || !r.read(buf.data(), buf.size(), fm.keyslistSeek))
        return std::nullopt;
      return parseKey(buf.data(), buf.size(), 0);
    }();
    if (!kl)
      return fail("cannot read keys list");
    // Geometry must hold before it sizes an allocation or indexes the record
    // (hostile nbytes/keylen: negative/oversized alloc, OOB nkeys read).
    if (kl->nbytes <= 0 || kl->keylen + 4u > static_cast<uint32_t>(kl->nbytes) ||
        fm.keyslistSeek < 0 || kl->nbytes > fsz - fm.keyslistSeek)
      return fail("keys-list key geometry implausible");
    std::vector<uint8_t> buf(kl->nbytes);
    if (!r.read(buf.data(), buf.size(), fm.keyslistSeek))
      return fail("short keys list read");
    size_t at = kl->keylen;
    int32_t nkeys = beGet<int32_t>(buf.data() + at);
    at += 4;
    for (int32_t i = 0; i < nkeys; ++i) {
      auto e = parseKey(buf.data(), buf.size(), at);
      if (!e)
        break;
      fm.keys.push_back(*e);
      if (e->keylen < 29) // smaller than a minimal key record: cannot advance
        break;
      at += e->keylen;
    }
  }
  return fm;
}

std::vector<uint8_t> readKeyPayloadFrom(Reader& r, const KeyInfo& k, bool* unreadable = nullptr,
                                        bool* checkFailed = nullptr) {
  if (unreadable)
    *unreadable = false;
  if (checkFailed)
    *checkFailed = false;
  if (k.nbytes <= 0 || k.keylen > k.nbytes || k.objlen < 0 || k.seekkey < 0)
    return {};
  std::vector<uint8_t> rec(k.nbytes);
  if (!r.read(rec.data(), rec.size(), k.seekkey)) {
    if (unreadable) // the source could not vouch for the bytes: try again later
      *unreadable = true;
    return {};
  }
  std::vector<uint8_t> out;
  if (k.objlen == k.nbytes - k.keylen) {
    out.assign(rec.begin() + k.keylen, rec.end());
  } else {
    DecodedFrames d = decodeFrames(rec.data() + k.keylen, rec.size() - k.keylen, k.objlen);
    if (d.check == FrameCheck::kFailed) {
      if (checkFailed)
        *checkFailed = true;
      return {};
    }
    out = std::move(d.out);
  }
  if (out.size() != static_cast<size_t>(k.objlen))
    return {};
  return out;
}

FileMeta parseTreeFrom(Reader& r, const std::string& tree) {
  FileMeta fm;
  auto fail = [&](std::string why) {
    fm.error = std::move(why);
    fm.branches.clear();
    return fm;
  };
  ContainerMeta cm = parseContainerFrom(r);
  // Geometry is carried over even when the walk failed later on: the detected
  // seek widths stay observable on the error path, which is what makes a
  // width-detection bug diagnosable from a file that does not fully parse.
  const int64_t fsz = cm.fileSize;
  fm.large = cm.large;
  fm.headerSeekWidth = cm.headerSeekWidth;
  fm.fend = cm.fend;
  fm.keyslistSeek = cm.keyslistSeek;
  fm.dirSeekKeysOff = cm.dirSeekKeysOff;
  fm.dirSeekWidth = cm.dirSeekWidth;
  if (!cm.error.empty())
    return fail(cm.error);

  // The live (highest-cycle) key named `tree`.
  for (const auto& e : cm.keys)
    if (e.cls == "TTree" && e.name == tree && e.cycle >= fm.treeKey.cycle)
      fm.treeKey = e;
  if (fm.treeKey.nbytes == 0)
    return fail("tree '" + tree + "' not found in keys list");
  if (fm.treeKey.nbytes < 0 || fm.treeKey.keylen > fm.treeKey.nbytes ||
      fm.treeKey.objlen < 0 || fm.treeKey.seekkey < 0 ||
      fm.treeKey.nbytes > fsz - fm.treeKey.seekkey)
    return fail("tree key geometry implausible");
  // One read, not two. Distinguishing "the source cannot vouch for these
  // bytes" (retryable: the reader has not staged them yet) from "they are not
  // valid tree metadata" (final) used to cost a whole second read of the
  // record, which on a large LZMA file is multiple megabytes off the cache
  // disk, CRC-verified, on every parse and on each of its retries.
  bool unreadable = false, checkFailed = false;
  fm.treeBlob = readKeyPayloadFrom(r, fm.treeKey, &unreadable, &checkFailed);
  if (unreadable)
    return fail("tree record not readable");
  if (checkFailed) {
    fail("tree metadata failed its own check");
    fm.recordFailed = true; // ROOT cannot read the tree from these bytes either
    return fm;
  }
  if (fm.treeBlob.size() != static_cast<size_t>(fm.treeKey.objlen))
    return fail("tree metadata decompression failed");
  if (!parseTreeBlob(fm.treeBlob.data(), fm.treeBlob.size(), fm.treeKey.keylen, fm))
    return fail(fm.error);
  return fm;
}

} // namespace

ContainerMeta parseContainer(int fd) {
  FdReader r(fd);
  return parseContainerFrom(r);
}
ContainerMeta parseContainer(Source& src, int64_t fileSize) {
  SourceReader r(src, fileSize);
  return parseContainerFrom(r);
}
std::vector<uint8_t> readKeyPayload(int fd, const KeyInfo& k) {
  FdReader r(fd);
  return readKeyPayloadFrom(r, k);
}
std::vector<uint8_t> readKeyPayload(Source& src, const KeyInfo& k) {
  SourceReader r(src, 0); // the size is not consulted for a single key record
  return readKeyPayloadFrom(r, k);
}

bool parseTreeBlob(const uint8_t* blob, size_t n, uint16_t keylen, FileMeta& fm) {
  // Streamed TTree v20 walk.
  fm.branches.clear();
  fm.clusterRangeEnd.clear();
  fm.clusterSize.clear();
  auto fail = [&](std::string why) {
    fm.error = std::move(why);
    fm.branches.clear();
    return false;
  };
  Cur c;
  c.p = blob;
  c.n = n;
  c.keylen = keylen;
  std::vector<std::pair<uint32_t, std::string>> classRefs;
  std::vector<std::pair<uint32_t, std::string>> leafRegistry;
  uint16_t tver;
  c.objHeader(tver, "TTree");
  if (c.fail_ || tver != 20)
    return fail(c.fail_ ? c.why : "TTree v" + std::to_string(tver) + " unsupported");
  c.skipObj("TNamed");
  c.skipObj("TAttLine");
  c.skipObj("TAttFill");
  c.skipObj("TAttMarker");
  fm.entries = c.get<int64_t>();
  if (c.need(16)) { // fTotBytes, then fZipBytes
    fm.zipBytesOff = c.at + 8;
    fm.zipBytes = beGet<int64_t>(c.p + c.at + 8);
  }
  c.skip(4 * 8 + 8 + 4 * 4); // fTotBytes..fFlushedBytes (q x4), fWeight (d), 4x i4
  uint32_t nClusterRange = c.get<uint32_t>();
  c.skip(4 * 8);             // fMaxEntries, fMaxEntryLoop, fMaxVirtualSize, fAutoSave
  fm.autoFlushOff = c.at;
  fm.autoFlush = c.get<int64_t>();
  c.skip(8);                 // fEstimate
  if (nClusterRange > (1u << 20))
    return fail("implausible fNClusterRange");
  auto clusterArray = [&](std::vector<int64_t>& out) {
    c.skip(1); // array marker
    if (!c.need(8ull * nClusterRange))
      return;
    out.resize(nClusterRange);
    for (uint32_t i = 0; i < nClusterRange; ++i)
      out[i] = beGet<int64_t>(c.p + c.at + 8ull * i);
    c.skip(8ull * nClusterRange);
  };
  clusterArray(fm.clusterRangeEnd); // fClusterRangeEnd
  clusterArray(fm.clusterSize);     // fClusterSize
  if (c.fail_)
    return fail(c.why);
  c.skipObj("TIOFeatures");
  if (!objArray(c, classRefs, "fBranches", [&](const AnyObj& o) {
        return o.cls == "TBranch" ? parseBranch(c, fm, classRefs, leafRegistry)
                                  : (c.fail_ = true,
                                     c.why = "branch class " + o.cls + " unsupported", false);
      }))
    return fail(c.why.empty() ? "branch walk failed" : c.why);
  if (c.fail_)
    return fail(c.why);
  return true;
}

FileMeta parseFile(const std::string& path, const std::string& tree) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    FileMeta fm;
    fm.error = "cannot open " + path;
    return fm;
  }
  FdReader r(fd);
  FileMeta fm = parseTreeFrom(r, tree);
  ::close(fd);
  return fm;
}

FileMeta parseFile(Source& src, int64_t fileSize, const std::string& tree) {
  SourceReader r(src, fileSize);
  return parseTreeFrom(r, tree);
}

} // namespace ucache::transpose
