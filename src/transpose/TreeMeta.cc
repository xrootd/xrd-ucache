#include "TreeMeta.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

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

namespace {
// One basket's key header at `at` of the fetched bytes `p` (`n` of them), for the basket the map
// places at `seek` with length `want` (0: none known; `withLength`: the map gives lengths).
enum class KeyVerdict : uint8_t { kCut, kOk, kPosition, kLength };
KeyVerdict judgeKey(const uint8_t* p, uint64_t n, uint64_t at, uint64_t seek, int32_t want,
                    bool withLength) {
  // The key's version (bytes 4-5) says how wide fSeekKey (from byte 18) is,
  // read as TKey reads it: a signed version above 1000 is 64-bit, and a
  // 32-bit fSeekKey is unsigned. A version whose top bit is set is negative,
  // so ROOT reads that key as 32-bit, and so does this check.
  if (n - at < 6)
    return KeyVerdict::kCut;
  const bool wide = beGet<int16_t>(p + at + 4) > 1000;
  if (n - at < (wide ? 26u : 22u))
    return KeyVerdict::kCut;
  const int64_t key = wide ? beGet<int64_t>(p + at + 18) : int64_t{beGet<uint32_t>(p + at + 18)};
  if (key != static_cast<int64_t>(seek))
    return KeyVerdict::kPosition;
  if (!withLength)
    return KeyVerdict::kOk;
  // The key's own fields, read as TKey::ReadKeyBuffer reads them (fNbytes and fObjlen signed
  // 32-bit, fKeylen signed 16-bit), against the map's length of the basket.
  const int32_t nbytes = beGet<int32_t>(p + at);
  const int32_t objlen = beGet<int32_t>(p + at + 6);
  const int16_t keylen = beGet<int16_t>(p + at + 14);
  // ROOT reads the basket by the map's length and decides from the key's own fields whether
  // it is compressed (TBasket::ReadBasketBuffers: fObjlen > fNbytes - fKeylen). A key length
  // that differs from the map's is held against the copy only where that decision differs
  // from the one the map's length gives: there ROOT takes compressed bytes for the entries
  // (wrong values, no error) or fails to decompress stored ones. Elsewhere ROOT reads the
  // basket correctly, and no claim is made.
  // The key's own fields: ROOT makes a zombie of a key whose length, key length or object
  // length is negative, and fails to read the basket. A key length of 0 makes it take the
  // key's own bytes for the data, and one past the map's length makes it read past the bytes
  // it fetched. A length below the key length, with that length alone damaged, is read
  // correctly (ROOT reads by the map's length) and makes no claim of its own.
  const bool byKey = int64_t{objlen} > int64_t{nbytes} - keylen;
  const bool byMap = int64_t{objlen} > int64_t{want} - keylen;
  if ((want > 0 && nbytes != want && byKey != byMap) || keylen <= 0 ||
      (want > 0 && keylen > want) || nbytes < 0 || objlen < 0)
    return KeyVerdict::kLength;
  return KeyVerdict::kOk;
}

bool overlaps(uint64_t a, uint64_t b, uint64_t lo, uint64_t hi) {
  return a < hi && lo < b;
}
} // namespace

uint64_t basketExtent(int32_t length) {
  // At least a 64-bit key's header (34 bytes), which the key check reads whatever the length says.
  return std::max<uint64_t>(length > 0 ? static_cast<uint64_t>(length) : 0, 34);
}

MisplacedKeys firstMisplacedKey(const std::vector<uint64_t>& seeks,
                                const std::vector<int32_t>& lengths, uint64_t spanOff,
                                const void* bytes, uint64_t n, uint64_t lo, uint64_t hi) {
  const auto* p = static_cast<const uint8_t*>(bytes);
  const bool withLengths = lengths.size() == seeks.size();
  MisplacedKeys m;
  for (auto it = std::lower_bound(seeks.begin(), seeks.end(), spanOff);
       it != seeks.end() && *it - spanOff < n; ++it) {
    // Seeks only grow from here, so once a header no longer fits none will.
    const size_t i = static_cast<size_t>(it - seeks.begin());
    const KeyVerdict v =
        judgeKey(p, n, *it - spanOff, *it, withLengths ? lengths[i] : 0, withLengths);
    if (v == KeyVerdict::kCut)
      break;
    if (v == KeyVerdict::kOk)
      continue;
    if (*it >= lo && *it < hi) {
      if (m.count++ == 0) {
        m.first = *it;
        m.length = v == KeyVerdict::kLength;
      }
    } else {
      m.outside = true;
    }
  }
  return m;
}

namespace {
// Whether a basket's data, `pay` (`n` bytes, its first block's header first), opens a block whose
// own checksum can be verified: zlib (adler32), LZMA (the xz stream's check), LZ4 (when this build
// has lz4; its XXH64), ZSTD only with a content checksum. A tag ROOT does not know is judged too
// (ROOT fails on it, and the decoder says so without decoding). Stored data, ZSTD without a
// checksum, an xz stream that declares no check and ROOT's old algorithm carry none: none of them
// is decompressed for the check (it would verify nothing).
enum class DataCheck : uint8_t { kNone, kDecode };
// The xz stream header that follows ROOT's 9-byte block header (`p`, `n` bytes from the block's
// start) declares no check (its check type, the low four bits of its second flag byte, is None),
// and its own CRC32 over the two flag bytes holds, so the declaration is the writer's, not damage:
// decoding it would verify nothing. ROOT writes CRC32 (check type 1); this is for other writers.
bool xzDeclaresNoCheck(const uint8_t* p, uint64_t n) {
  static const uint8_t kMagic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};
  if (n < 9 + 12 || std::memcmp(p + 9, kMagic, 6) != 0 || p[15] != 0 || (p[16] & 0x0F) != 0)
    return false;
  uint32_t crc = 0xFFFFFFFFu; // CRC-32 (IEEE), as xz computes it
  for (int i = 15; i <= 16; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  crc ^= 0xFFFFFFFFu;
  const uint32_t stored = uint32_t{p[17]} | uint32_t{p[18]} << 8 | uint32_t{p[19]} << 16 |
                          uint32_t{p[20]} << 24;
  return crc == stored;
}
DataCheck dataCheckOf(const uint8_t* pay, uint64_t n) {
  if (n < 9)
    return DataCheck::kDecode; // no block header: the decoder fails it at once
  const uint8_t a = pay[0], b = pay[1], m = pay[2];
  if (a == 'X' && b == 'Z') // judged unless its stream declares no check
    return xzDeclaresNoCheck(pay, n) ? DataCheck::kNone : DataCheck::kDecode;
  // LZ4: judged by its hash, in any build (verifyFrames).
  if ((a == 'Z' && b == 'L') || (a == 'L' && b == '4'))
    return DataCheck::kDecode;
  if (a == 'Z' && b == 'S') // judged when it carries a content checksum, or ROOT refuses its header
    return m != 1 || (n >= 14 && (pay[13] & 0x04)) ? DataCheck::kDecode : DataCheck::kNone;
  if (a == 'C' && b == 'S')
    return m == 8 ? DataCheck::kNone : DataCheck::kDecode;
  return DataCheck::kDecode;
}

} // namespace

bool dataCarriesNoCheck(const uint8_t* pay, uint64_t n) {
  return dataCheckOf(pay, n) == DataCheck::kNone;
}

namespace {
// The basket at `at` (map length `want`, its key passed) wholly in the fetched bytes: whether ROOT
// decompresses it (fObjlen > fNbytes - fKeylen, from the key, as TBasket::ReadBasketBuffers
// decides), and where its data starts. False when it is stored as it is.
bool compressedData(const uint8_t* p, uint64_t at, int32_t want, const uint8_t*& pay,
                    uint64_t& payLen, uint64_t& objlen) {
  const int32_t nbytes = beGet<int32_t>(p + at);
  const int32_t obj = beGet<int32_t>(p + at + 6);
  const int16_t keylen = beGet<int16_t>(p + at + 14);
  if (int64_t{obj} <= int64_t{nbytes} - keylen)
    return false;
  pay = p + at + static_cast<uint64_t>(keylen);
  payLen = static_cast<uint64_t>(want) - static_cast<uint64_t>(keylen);
  objlen = static_cast<uint64_t>(obj);
  return true;
}
} // namespace

FetchVerdict checkFetched(const std::vector<uint64_t>& seeks, const std::vector<int32_t>& lengths,
                          uint64_t spanOff, const void* bytes, uint64_t n, bool data,
                          const FetchJudge& judge) {
  const auto* p = static_cast<const uint8_t*>(bytes);
  const bool withLengths = lengths.size() == seeks.size();
  FetchVerdict v;
  for (auto it = std::lower_bound(seeks.begin(), seeks.end(), std::max(spanOff, judge.lo));
       it != seeks.end() && *it - spanOff < n && *it < judge.hi; ++it) {
    const size_t i = static_cast<size_t>(it - seeks.begin());
    const uint64_t at = *it - spanOff;
    const int32_t want = withLengths ? lengths[i] : 0;
    // A basket the bytes do not hold to its end (its length, or a key header's most) is left to
    // the job that holds it whole (checkJobs).
    if (judge.skipPastEnd && basketExtent(want) > n - at)
      continue;
    const KeyVerdict k = judgeKey(p, n, at, *it, want, withLengths);
    if (k == KeyVerdict::kCut)
      break;
    if (k != KeyVerdict::kOk) {
      FetchFault f;
      f.at = *it;
      f.end = *it + 1;
      f.kind = k == KeyVerdict::kLength ? FetchFault::kLength : FetchFault::kPosition;
      v.faults.push_back(std::move(f));
      continue;
    }
    // The basket's data: only one whose length the map gives and that lies wholly in the bytes,
    // and, with `reach`, that the given ranges reach.
    if (!data || want <= 0 || static_cast<uint64_t>(want) > n - at)
      continue;
    if (judge.reach) {
      bool reached = false;
      for (const auto& [lo, hi] : *judge.reach)
        if (overlaps(*it, *it + static_cast<uint64_t>(want), lo, hi)) {
          reached = true;
          break;
        }
      if (!reached)
        continue;
    }
    const uint8_t* pay = nullptr;
    uint64_t payLen = 0, objlen = 0;
    if (!compressedData(p, at, want, pay, payLen, objlen) ||
        dataCheckOf(pay, payLen) == DataCheck::kNone) {
      ++v.none;
      continue;
    }
    const DecodedFrames d = verifyFrames(pay, payLen, objlen);
    if (d.check == FrameCheck::kPassed) {
      ++v.passed;
    } else if (d.check == FrameCheck::kFailed) {
      FetchFault f;
      f.at = *it;
      f.end = *it + static_cast<uint64_t>(want);
      f.kind = FetchFault::kData;
      f.why = d.why;
      v.faults.push_back(std::move(f));
    } else {
      ++v.none;
    }
  }
  return v;
}

bool fetchedDataToCheck(const std::vector<uint64_t>& seeks, const std::vector<int32_t>& lengths,
                        uint64_t spanOff, const void* bytes, uint64_t n) {
  if (lengths.size() != seeks.size())
    return false;
  const auto* p = static_cast<const uint8_t*>(bytes);
  for (auto it = std::lower_bound(seeks.begin(), seeks.end(), spanOff);
       it != seeks.end() && *it - spanOff < n; ++it) {
    const size_t i = static_cast<size_t>(it - seeks.begin());
    const uint64_t at = *it - spanOff;
    const int32_t want = lengths[i];
    const KeyVerdict k = judgeKey(p, n, at, *it, want, true);
    if (k == KeyVerdict::kCut)
      break;
    if (k != KeyVerdict::kOk || want <= 0 || static_cast<uint64_t>(want) > n - at)
      continue;
    const uint8_t* pay = nullptr;
    uint64_t payLen = 0, objlen = 0;
    if (compressedData(p, at, want, pay, payLen, objlen) &&
        dataCheckOf(pay, payLen) == DataCheck::kDecode)
      return true;
  }
  return false;
}

namespace {
// `pieces` in offset order, cut into runs of pieces that follow each other without a gap.
std::vector<std::vector<BytePiece>> pieceRuns(std::vector<BytePiece> pieces) {
  std::sort(pieces.begin(), pieces.end(),
            [](const BytePiece& a, const BytePiece& b) { return a.off < b.off; });
  std::vector<std::vector<BytePiece>> runs;
  for (const auto& x : pieces) {
    if (!x.p || !x.n)
      continue;
    if (!runs.empty() && runs.back().back().off + runs.back().back().n == x.off)
      runs.back().push_back(x);
    else
      runs.push_back({x});
  }
  return runs;
}
} // namespace

std::vector<CheckJob> checkJobs(const std::vector<uint64_t>& seeks,
                                const std::vector<int32_t>& lengths, std::vector<BytePiece> pieces,
                                uint64_t sliceBytes,
                                std::vector<std::pair<uint64_t, uint64_t>>* runs) {
  const bool withLengths = lengths.size() == seeks.size();
  if (sliceBytes == 0)
    sliceBytes = 1;
  std::vector<CheckJob> jobs;
  for (const auto& run : pieceRuns(std::move(pieces))) {
    const uint64_t runOff = run.front().off;
    const uint64_t runEnd = run.back().off + run.back().n;
    if (runs)
      runs->emplace_back(runOff, runEnd - runOff);
    for (size_t k = 0; k < run.size(); ++k) {
      const BytePiece& x = run[k];
      const bool last = k + 1 == run.size();
      // The piece's baskets in slices of `sliceBytes` by where they start, each slice judging
      // with the piece's bytes to its end.
      for (uint64_t a = x.off; a < x.off + x.n;) {
        CheckJob j;
        j.off = x.off;
        j.p = x.p;
        j.n = x.n;
        j.lo = a;
        j.hi = x.off + x.n - a > sliceBytes ? a + sliceBytes : x.off + x.n;
        j.skipPastEnd = !last;
        a = j.hi;
        jobs.push_back(std::move(j));
      }
      if (last)
        continue;
      // A basket that starts in this piece and runs past its end into the next ones: its bytes
      // joined (to the run's end at most), judged on their own.
      const uint64_t end = x.off + x.n;
      for (auto it = std::lower_bound(seeks.begin(), seeks.end(), x.off);
           it != seeks.end() && *it < end; ++it) {
        const size_t i = static_cast<size_t>(it - seeks.begin());
        const uint64_t ext = basketExtent(withLengths ? lengths[i] : 0);
        if (ext <= end - *it)
          continue;
        const uint64_t to = runEnd - *it > ext ? *it + ext : runEnd;
        auto own = std::make_shared<std::vector<char>>();
        own->reserve(to - *it);
        for (size_t q = k; q < run.size() && run[q].off < to; ++q) {
          const uint64_t from = std::max(*it, run[q].off);
          const uint64_t upto = std::min(to, run[q].off + run[q].n);
          own->insert(own->end(), run[q].p + (from - run[q].off), run[q].p + (upto - run[q].off));
        }
        CheckJob j;
        j.off = *it;
        j.p = own->data();
        j.n = own->size();
        j.lo = *it;
        j.hi = *it + 1;
        j.own = std::move(own);
        jobs.push_back(std::move(j));
      }
    }
  }
  return jobs;
}

FetchVerdict runCheckJob(const std::vector<uint64_t>& seeks, const std::vector<int32_t>& lengths,
                         const CheckJob& j, bool data,
                         const std::vector<std::pair<uint64_t, uint64_t>>* reach) {
  FetchJudge judge;
  judge.lo = j.lo;
  judge.hi = j.hi;
  judge.skipPastEnd = j.skipPastEnd;
  judge.reach = reach;
  return checkFetched(seeks, lengths, j.off, j.p, j.n, data, judge);
}

void FetchVerdict::sortFaults() {
  std::stable_sort(faults.begin(), faults.end(),
                   [](const FetchFault& a, const FetchFault& b) { return a.at < b.at; });
}

FetchVerdict checkPieces(const std::vector<uint64_t>& seeks, const std::vector<int32_t>& lengths,
                         std::vector<BytePiece> pieces, bool data,
                         const std::vector<std::pair<uint64_t, uint64_t>>* damaged,
                         const std::string& why,
                         const std::vector<std::pair<uint64_t, uint64_t>>* reach) {
  FetchVerdict v;
  std::vector<std::pair<uint64_t, uint64_t>> runs;
  const std::vector<CheckJob> jobs =
      checkJobs(seeks, lengths, std::move(pieces), ~uint64_t{0}, &runs);
  if (damaged)
    for (const auto& [off, n] : runs)
      v.damaged(*damaged, off, n, why);
  for (const auto& j : jobs)
    v.merge(runCheckJob(seeks, lengths, j, data, reach));
  v.sortFaults();
  return v;
}

bool piecesNeedWork(const std::vector<uint64_t>& seeks, const std::vector<int32_t>& lengths,
                    std::vector<BytePiece> pieces) {
  for (const auto& run : pieceRuns(std::move(pieces)))
    if (run.size() > 1 ||
        fetchedDataToCheck(seeks, lengths, run.front().off, run.front().p, run.front().n))
      return true;
  return false;
}

void FetchVerdict::damaged(const std::vector<std::pair<uint64_t, uint64_t>>& spans,
                           uint64_t spanOff, uint64_t n, const std::string& why) {
  auto it = std::upper_bound(spans.begin(), spans.end(),
                             std::pair<uint64_t, uint64_t>(spanOff, ~uint64_t{0}));
  if (it != spans.begin() && std::prev(it)->second > spanOff)
    --it;
  for (; it != spans.end() && it->first < spanOff + n; ++it) {
    FetchFault f;
    f.at = std::max(it->first, spanOff);
    f.end = std::min(it->second, spanOff + n);
    f.kind = FetchFault::kDamaged;
    f.why = why;
    faults.push_back(std::move(f));
  }
}

void FetchVerdict::merge(FetchVerdict&& o) {
  for (auto& f : o.faults)
    faults.push_back(std::move(f));
  passed += o.passed;
  none += o.none;
  unjudged += o.unjudged;
}

MisplacedKeys FetchVerdict::against(const std::vector<std::pair<uint64_t, uint64_t>>& reader,
                                    std::string* why) const {
  MisplacedKeys m;
  for (const auto& f : faults) {
    bool mine = false;
    uint64_t first = f.at;
    for (const auto& [lo, hi] : reader) {
      if (f.kind == FetchFault::kPosition || f.kind == FetchFault::kLength) {
        mine = f.at >= lo && f.at < hi; // a key that starts in the reader's bytes
      } else if (overlaps(f.at, f.end, lo, hi)) {
        mine = true; // a basket's data, or a damaged part, the reader's bytes reach into
        first = f.kind == FetchFault::kDamaged ? std::max(f.at, lo) : f.at;
      }
      if (mine)
        break;
    }
    if (!mine) {
      m.outside = true;
      continue;
    }
    if (m.count++ == 0) {
      m.first = first;
      m.length = f.kind == FetchFault::kLength;
      if (why)
        *why = f.kind == FetchFault::kLength    ? "key length differs from the tree's record"
               : f.kind == FetchFault::kData    ? "its data fails its own check: " + f.why
               : f.kind == FetchFault::kDamaged ? f.why
                                                : std::string();
    }
  }
  return m;
}

MisplacedKeys FetchVerdict::against(uint64_t lo, uint64_t hi, std::string* why) const {
  return against(std::vector<std::pair<uint64_t, uint64_t>>{{lo, hi}}, why);
}

std::vector<std::pair<uint64_t, uint64_t>> treeSpans(const FileMeta& fm,
                                                     const std::vector<uint64_t>& seeks,
                                                     const std::vector<int32_t>& bytes) {
  std::vector<std::pair<uint64_t, uint64_t>> all;
  if (fm.treeKey.nbytes > 0 && fm.treeKey.seekkey > 0)
    all.emplace_back(static_cast<uint64_t>(fm.treeKey.seekkey),
                     static_cast<uint64_t>(fm.treeKey.seekkey) +
                         static_cast<uint64_t>(fm.treeKey.nbytes));
  for (size_t i = 0; i < seeks.size(); ++i)
    all.emplace_back(seeks[i],
                     seeks[i] + (i < bytes.size() && bytes[i] > 0 ? static_cast<uint64_t>(bytes[i])
                                                                  : uint64_t{1}));
  std::sort(all.begin(), all.end());
  std::vector<std::pair<uint64_t, uint64_t>> out;
  for (const auto& r : all)
    if (!out.empty() && r.first <= out.back().second)
      out.back().second = std::max(out.back().second, r.second);
    else
      out.push_back(r);
  return out;
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

namespace {
// Decoder state kept per thread and reused from one block to the next (ROOT's own decoders are
// set up per block; their acceptance is the same): an LZMA stream decoder, whose dictionary
// liblzma keeps while the next stream asks for one of the same size; a zlib inflate state, reset
// per block; a ZSTD context; and a scratch buffer for a block whose output is not kept. Every
// allocation the decoders make goes through a counting allocator (decoderAllocations).
std::atomic<uint64_t> g_decoderAllocs{0};
std::atomic<uint64_t> g_decoderLargeAllocs{
    0}; // 1 MiB or more: an LZMA dictionary, a zlib window is 32 KiB
std::atomic<int64_t> g_decoderLargeLive{0}; // large allocations not yet freed
// Each allocation carries its size in front, so a free knows whether it was a large one.
constexpr size_t kAllocHead = 16;
void* countedAlloc(void*, size_t nmemb, size_t size) {
  const size_t n = nmemb * size;
  g_decoderAllocs.fetch_add(1, std::memory_order_relaxed);
  if (n >= (1u << 20)) {
    g_decoderLargeAllocs.fetch_add(1, std::memory_order_relaxed);
    g_decoderLargeLive.fetch_add(1, std::memory_order_relaxed);
  }
  auto* base = static_cast<unsigned char*>(std::malloc(n + kAllocHead));
  if (!base)
    return nullptr;
  std::memcpy(base, &n, sizeof n);
  return base + kAllocHead;
}
void countedFree(void*, void* ptr) {
  if (!ptr)
    return;
  auto* base = static_cast<unsigned char*>(ptr) - kAllocHead;
  size_t n = 0;
  std::memcpy(&n, base, sizeof n);
  if (n >= (1u << 20))
    g_decoderLargeLive.fetch_sub(1, std::memory_order_relaxed);
  std::free(base);
}
void* zAlloc(void* o, uInt items, uInt size) {
  return countedAlloc(o, items, size);
}
void zFree(void* o, void* ptr) {
  countedFree(o, ptr);
}
const lzma_allocator kLzmaAlloc{countedAlloc, countedFree, nullptr};
struct Decoders {
  lzma_stream xz = LZMA_STREAM_INIT;
  z_stream z{};
  bool zInit = false;
  ZSTD_DCtx* zs = nullptr;
  std::vector<uint8_t> scratch;
  Decoders() { xz.allocator = &kLzmaAlloc; }
  ~Decoders() {
    lzma_end(&xz);
    if (zInit)
      inflateEnd(&z);
    if (zs)
      ZSTD_freeDCtx(zs);
  }
};
Decoders& decoders() {
  thread_local Decoders d;
  return d;
}
thread_local bool t_keepDecoders = false;
} // namespace

uint64_t decoderAllocations() {
  return g_decoderAllocs.load(std::memory_order_relaxed);
}
uint64_t decoderLargeAllocations() {
  return g_decoderLargeAllocs.load(std::memory_order_relaxed);
}
int64_t decoderLargeLive() {
  return g_decoderLargeLive.load(std::memory_order_relaxed);
}
void keepDecodersOnThisThread() {
  t_keepDecoders = true;
}

namespace {
// The blocks of a compressed object, each decoded as ROOT decodes it (R__unzip's ZLIB, LZMA,
// ZSTD and LZ4 paths): a block may give fewer bytes than its header's size (ROOT goes on from
// what it gave: TBasket::ReadBasketBuffers advances by R__unzip's output), never more, and never
// none; the blocks together must give exactly `objlen`. `keep`: the object is kept in d.out;
// otherwise each block is decoded into the thread's scratch and only judged, and an LZ4 block is
// judged by its XXH64 alone (ROOT verifies that hash over the compressed bytes before it
// decodes them; a block whose hash holds is the one its writer wrote).
void decodeInto(const uint8_t* payload, size_t n, uint64_t objlen, bool keep, DecodedFrames& d) {
  auto failed = [&](const char* why) {
    d.check = FrameCheck::kFailed;
    d.why = why;
    d.out.clear();
  };
  auto unsupported = [&](const char* why) {
    d.check = FrameCheck::kNotCheckable;
    d.why = why;
    d.out.clear();
  };
  // objlen is attacker-influenceable: cap the upfront reservation (growth past
  // it amortizes per block) so a hostile value cannot force a giant allocation.
  if (keep)
    d.out.reserve(std::min<uint64_t>(objlen, 64ull << 20));
  // Decoders kept for the thread's life only where the thread asked for it (the decode pool's);
  // elsewhere set up for this object and freed with it, as liblzma's one-shot decoder did.
  Decoders local;
  Decoders& dec = t_keepDecoders ? decoders() : local;
  bool everyBlockChecked = true;
  uint64_t total = 0; // what the blocks gave so far
  size_t p = 0;
  while (total < objlen) {
    if (p > n || n - p < 9)
      return failed("compression block header past the payload");
    const std::string codec = blockCodec(payload + p, n - p);
    if (codec.empty()) {
      // ROOT's old algorithm (CS, its third byte 8) is legitimate and not decoded here; any
      // other tag is one ROOT itself does not know, so the bytes are damaged.
      if (payload[p] == 'C' && payload[p + 1] == 'S' && payload[p + 2] == 8)
        return unsupported("compression block of ROOT's old algorithm");
      return failed("unknown compression block");
    }
    // The third byte, as ROOT holds it (R__unzip's is_valid_header: ZL 8, XZ 0, ZS 1;
    // R__unzipLZ4: the LZ4 major version, 1): ROOT decodes nothing from a block whose third byte
    // is another.
    if (payload[p + 2] != (codec == "zlib" ? 8 : codec == "lzma" ? 0 : 1))
      return failed("compression block header ROOT does not decode");
    uint64_t csize = payload[p + 3] | payload[p + 4] << 8 | payload[p + 5] << 16;
    uint64_t usize = payload[p + 6] | payload[p + 7] << 8 | payload[p + 8] << 16;
    if (usize == 0)
      return failed("empty compression block");
    const uint64_t availIn = n - p - 9;
    const uint64_t wantOut = objlen - total;
    // ROOT decodes each block with the sizes its header states and stops once
    // the object is complete, so a LAST block whose header claims more input or
    // more output than there is still reads correctly in ROOT; its own check
    // decides. A block whose header claims more output than it gives reads
    // correctly too (ROOT goes on from what it gave, below); one that does not
    // fit the output its header states fails in ROOT, as here. A block that
    // is not the last and claims more input than there is fails as well. LZ4
    // is the exception: its hash covers exactly the compressed size the header
    // states.
    if (csize > availIn || usize > wantOut) {
      if (csize > availIn && usize < wantOut)
        return failed("compression block runs past the payload");
      if (csize > availIn && codec == "lz4")
        return failed("lz4 block runs past the payload");
      csize = std::min(csize, availIn);
      usize = std::min(usize, wantOut);
      d.headerTolerated = true;
    }
    const uint8_t* blob = payload + p + 9;
    uint8_t* dst = nullptr;
    size_t pre = 0;
    if (keep) {
      pre = d.out.size();
      d.out.resize(pre + usize);
      dst = d.out.data() + pre;
    } else if (codec != "lz4") {
      if (dec.scratch.size() < usize)
        dec.scratch.resize(usize);
      dst = dec.scratch.data();
    }
    uint64_t gave = 0;
    if (codec == "lzma") {
      // An xz stream names its check in the second byte of its stream flags;
      // ROOT writes CRC32, and liblzma verifies whatever is named. As ROOT:
      // a stream decoder, one lzma_code with LZMA_FINISH, the stream's end.
      if (csize < 8)
        return failed("lzma block too short");
      if ((blob[7] & 0x0F) == 0)
        everyBlockChecked = false;
      if (lzma_stream_decoder(&dec.xz, UINT64_MAX, 0) != LZMA_OK)
        return failed("lzma block failed its check or did not decode");
      dec.xz.next_in = blob;
      dec.xz.avail_in = csize;
      dec.xz.next_out = dst;
      dec.xz.avail_out = usize;
      if (lzma_code(&dec.xz, LZMA_FINISH) != LZMA_STREAM_END)
        return failed("lzma block failed its check or did not decode");
      gave = usize - dec.xz.avail_out;
    } else if (codec == "zlib") {
      // The zlib wrapper ends with an adler32 that inflate verifies. As ROOT:
      // inflate with Z_FINISH until the stream's end.
      if (!dec.zInit) {
        dec.z.zalloc = zAlloc;
        dec.z.zfree = zFree;
        dec.z.opaque = nullptr;
        if (inflateInit(&dec.z) != Z_OK)
          return failed("zlib block failed its check or did not decode");
        dec.zInit = true;
      } else if (inflateReset(&dec.z) != Z_OK) {
        return failed("zlib block failed its check or did not decode");
      }
      dec.z.next_in = const_cast<Bytef*>(blob);
      dec.z.avail_in = static_cast<uInt>(csize);
      dec.z.next_out = dst;
      dec.z.avail_out = static_cast<uInt>(usize);
      int err;
      while ((err = inflate(&dec.z, Z_FINISH)) != Z_STREAM_END)
        if (err != Z_OK)
          return failed("zlib block failed its check or did not decode");
      gave = usize - dec.z.avail_out;
    } else if (codec == "zstd") {
      // Frame header: 4-byte magic, then the descriptor whose bit 2 says a
      // content checksum follows the frame (ZSTD verifies it when present).
      if (csize < 5)
        return failed("zstd block too short");
      if ((blob[4] & 0x04) == 0)
        everyBlockChecked = false;
      if (!dec.zs) {
        dec.zs = ZSTD_createDCtx();
        g_decoderAllocs.fetch_add(1,
                                  std::memory_order_relaxed); // its own allocations, counted once
        if (!dec.zs)
          return failed("zstd block did not decode");
      }
      const size_t r = ZSTD_decompressDCtx(dec.zs, dst, usize, blob, csize);
      if (ZSTD_isError(r))
        return failed("zstd block did not decode");
      gave = r;
    } else { // lz4
      // ROOT's LZ4 block: an 8-byte XXH64 (big-endian) of the compressed bytes
      // that follow it, then the LZ4 data.
      if (csize < 8)
        return failed("lz4 block too short");
      uint64_t want = 0;
      for (int b = 0; b < 8; ++b)
        want = (want << 8) | blob[b];
      if (xxh64(blob + 8, csize - 8, 0) != want)
        return failed("lz4 block failed its check");
      if (!keep) {
        gave = usize; // judged by its hash; its size as its header states it
      } else {
#ifdef UCACHE_HAVE_LZ4
        const int r = LZ4_decompress_safe(reinterpret_cast<const char*>(blob + 8),
                                          reinterpret_cast<char*>(dst), static_cast<int>(csize - 8),
                                          static_cast<int>(usize));
        if (r <= 0)
          return failed("lz4 block did not decode");
        gave = static_cast<uint64_t>(r);
#else
        return unsupported("lz4 block, and this build has no lz4");
#endif
      }
    }
    if (gave == 0)
      return failed("compression block gave nothing");
    if (keep)
      d.out.resize(pre + gave);
    total += gave;
    p += 9 + csize;
  }
  d.check = everyBlockChecked ? FrameCheck::kPassed : FrameCheck::kNotCheckable;
  if (!everyBlockChecked)
    d.why = "a block carries no check";
}
} // namespace

DecodedFrames decodeFrames(const uint8_t* payload, size_t n, uint64_t objlen) {
  DecodedFrames d;
  decodeInto(payload, n, objlen, true, d);
  return d;
}

DecodedFrames verifyFrames(const uint8_t* payload, size_t n, uint64_t objlen) {
  DecodedFrames d;
  decodeInto(payload, n, objlen, false, d);
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
