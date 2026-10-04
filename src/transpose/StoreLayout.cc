#include "StoreLayout.h"

#include "RNTupleMeta.h"
#include "ReadMap.h"
#include "RNTupleRewrite.h"
#include "Transposer.h"
#include "vendor/xxh3.h"

#include <cstring>

namespace ucache::transpose {

uint64_t layoutHash(const FillLayout& L, bool rnt) {
  std::vector<uint8_t> b;
  auto put64 = [&](uint64_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + 8);
  };
  put64(rnt ? 1 : 0);
  put64(L.originSize);
  put64(L.virtualSize);
  put64(L.metaSeek);
  put64(L.slotsBegin);
  put64(L.windows.size());
  for (const auto& w : L.windows) {
    put64(w.off);
    put64(w.bytes.size());
    b.insert(b.end(), w.bytes.begin(), w.bytes.end());
  }
  put64(L.metaRecord.size());
  b.insert(b.end(), L.metaRecord.begin(), L.metaRecord.end());
  put64(L.slots.size());
  for (const auto& s : L.slots) {
    put64(s.origSeek);
    put64(s.vSeek);
    put64((static_cast<uint64_t>(s.origLen) << 32) | s.vLen);
  }
  return xxh3_64(b.data(), b.size());
}

std::string joinCodecs(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& c : v)
    s += (s.empty() ? "" : ",") + c;
  return s;
}
std::vector<std::string> splitCodecs(const std::string& s) {
  std::vector<std::string> v;
  std::string cur;
  for (char c : s + ",")
    if (c == ',') {
      if (!cur.empty())
        v.push_back(cur);
      cur.clear();
    } else
      cur += c;
  return v;
}

namespace {

// ---- the stored layout: what the store's creator computed, as everyone serves it

constexpr char kLayoutMagic[8] = {'U', 'C', 'L', 'A', 'Y', 'T', '0', '2'};

struct Writer {
  std::vector<uint8_t> b;
  template <typename T> void put(T v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof v);
  }
  void bytes(const std::vector<uint8_t>& v) {
    put<uint64_t>(v.size());
    b.insert(b.end(), v.begin(), v.end());
  }
  void varint(uint64_t v) {
    while (v >= 0x80) {
      b.push_back(static_cast<uint8_t>(v | 0x80));
      v >>= 7;
    }
    b.push_back(static_cast<uint8_t>(v));
  }
  void svarint(int64_t v) { varint((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
};
struct Reader {
  const uint8_t* p;
  size_t n, at = 0;
  bool ok = true;
  template <typename T> T get() {
    T v{};
    if (at + sizeof v > n) {
      ok = false;
      return v;
    }
    std::memcpy(&v, p + at, sizeof v);
    at += sizeof v;
    return v;
  }
  bool bytes(std::vector<uint8_t>& v) {
    const uint64_t len = get<uint64_t>();
    if (!ok || len > n - at)
      return ok = false;
    v.assign(p + at, p + at + len);
    at += len;
    return true;
  }
  uint64_t varint() {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (at >= n) {
        ok = false;
        return 0;
      }
      const uint8_t c = p[at++];
      v |= static_cast<uint64_t>(c & 0x7f) << shift;
      if (!(c & 0x80))
        return v;
    }
    ok = false;
    return 0;
  }
  int64_t svarint() {
    const uint64_t u = varint();
    return static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
  }
};

} // namespace

// A TTree slot's length when nothing capped it: k (in hundredths) times the stored basket.
uint32_t plainSlotLen(uint32_t origLen, uint32_t k100) {
  const uint64_t v = static_cast<uint64_t>(origLen) * k100 / 100; // as layoutForFill sizes it
  return v > INT32_MAX ? static_cast<uint32_t>(INT32_MAX) : static_cast<uint32_t>(v);
}

std::vector<uint8_t> encodeLayout(const StoredLayout& cf) {
  Writer w;
  w.b.insert(w.b.end(), kLayoutMagic, kLayoutMagic + 8);
  const FillLayout& L = cf.L;
  w.put<uint8_t>(cf.rnt ? 1 : 0);
  w.put<uint64_t>(L.originSize);
  w.put<uint64_t>(L.virtualSize);
  w.put<uint64_t>(L.metaSeek);
  w.put<uint64_t>(L.slotsBegin);
  w.put<uint32_t>(static_cast<uint32_t>(L.windows.size()));
  for (const auto& win : L.windows) {
    w.put<uint64_t>(win.off);
    w.bytes(win.bytes);
  }
  w.bytes(L.metaRecord);
  w.put<uint32_t>(static_cast<uint32_t>(cf.metaOrigin.size()));
  for (const auto& [o, n] : cf.metaOrigin) {
    w.put<uint64_t>(o);
    w.put<uint64_t>(n);
  }
  w.put<uint32_t>(static_cast<uint32_t>(L.relocated.size()));
  for (uint32_t r : L.relocated)
    w.put<uint32_t>(r);
  // The slot table, delta-coded: slots follow one another from slotsBegin, a
  // branch's baskets are consecutive, and a TTree slot is k times its basket
  // unless capped -- so most of a slot is implied by the one before it.
  w.put<uint32_t>(static_cast<uint32_t>(L.slots.size()));
  uint64_t prevOrig = 0, nextV = L.slotsBegin;
  uint32_t prevBranch = 0, prevBasket = 0;
  for (const auto& s : L.slots) {
    w.svarint(static_cast<int64_t>(s.branch) - static_cast<int64_t>(prevBranch));
    w.svarint(static_cast<int64_t>(s.basket) - static_cast<int64_t>(prevBasket) - 1);
    w.svarint(static_cast<int64_t>(s.origSeek - prevOrig));
    w.varint(s.origLen);
    w.varint(!cf.rnt && s.vLen == plainSlotLen(s.origLen, cf.slotFactor100) ? 0 : uint64_t(s.vLen) + 1);
    w.svarint(static_cast<int64_t>(s.vSeek - nextV)); // 0 when contiguous
    prevBranch = s.branch;
    prevBasket = s.basket;
    prevOrig = s.origSeek;
    nextV = s.vSeek + s.vLen;
  }
  if (cf.rnt)
    for (const auto& pg : cf.pages) {
      w.put<uint32_t>(pg.nbytes);
      w.put<uint8_t>(pg.hasChecksum ? 1 : 0);
    }
  // Stored compressed: the slot table of a large tree is tens of MB raw.
  std::vector<uint8_t> out(8);
  const uint64_t raw = w.b.size();
  std::memcpy(out.data(), &raw, 8);
  auto z = encodeZstdFrames(w.b.data(), w.b.size(), 3);
  if (z.empty())
    out.insert(out.end(), w.b.begin(), w.b.end()); // stored raw (flagged by length)
  else
    out.insert(out.end(), z.begin(), z.end());
  return out;
}

bool layoutRaw(const std::vector<uint8_t>& blob, std::vector<uint8_t>& b) {
  if (blob.size() < 8)
    return false;
  uint64_t raw = 0;
  std::memcpy(&raw, blob.data(), 8);
  if (blob.size() - 8 == raw)
    b.assign(blob.begin() + 8, blob.end());
  else
    b = decompressFrames(blob.data() + 8, blob.size() - 8, raw);
  return b.size() == raw && raw >= 8 && std::memcmp(b.data(), kLayoutMagic, 8) == 0;
}

bool decodeLayout(const std::vector<uint8_t>& blob, StoredLayout& cf, SlotTable::Source lazy) {
  std::vector<uint8_t> b;
  if (!layoutRaw(blob, b))
    return false;
  Reader r{b.data(), b.size(), 8};
  FillLayout& L = cf.L;
  cf.rnt = r.get<uint8_t>() != 0;
  L.originSize = r.get<uint64_t>();
  L.virtualSize = r.get<uint64_t>();
  L.metaSeek = r.get<uint64_t>();
  L.slotsBegin = r.get<uint64_t>();
  const uint32_t nw = r.get<uint32_t>();
  for (uint32_t i = 0; r.ok && i < nw; ++i) {
    FillLayout::Window win;
    win.off = r.get<uint64_t>();
    r.bytes(win.bytes);
    L.windows.push_back(std::move(win));
  }
  r.bytes(L.metaRecord);
  const uint32_t nm = r.get<uint32_t>();
  for (uint32_t i = 0; r.ok && i < nm; ++i) {
    const uint64_t o = r.get<uint64_t>();
    cf.metaOrigin.emplace_back(o, r.get<uint64_t>());
  }
  const uint32_t nr = r.get<uint32_t>();
  if (!r.ok || nr > b.size())
    return false;
  L.relocated.resize(nr);
  for (uint32_t i = 0; r.ok && i < nr; ++i)
    L.relocated[i] = r.get<uint32_t>();
  const uint32_t ns = r.get<uint32_t>();
  if (!r.ok || static_cast<uint64_t>(ns) * 6 > b.size())
    return false;
  if (lazy && !cf.rnt) {
    if (L.virtualSize < L.slotsBegin || L.slotsBegin < L.metaSeek)
      return false;
    SlotTable::Geometry g;
    g.slotsBegin = L.slotsBegin;
    g.virtualSize = L.virtualSize;
    g.originSize = L.originSize;
    g.slotFactor100 = cf.slotFactor100;
    size_t at = r.at;
    if (!cf.slots.index(b, at, ns, g, std::move(lazy)) || at != b.size())
      return false;
    for (const auto& win : L.windows)
      if (win.off + win.bytes.size() > L.originSize)
        return false;
    return true;
  }
  L.slots.resize(ns);
  uint64_t prevOrig = 0, nextV = L.slotsBegin;
  uint32_t prevBranch = 0, prevBasket = 0;
  for (uint32_t i = 0; r.ok && i < ns; ++i) {
    FillSlot& s = L.slots[i];
    s.branch = static_cast<uint32_t>(prevBranch + r.svarint());
    s.basket = static_cast<uint32_t>(prevBasket + 1 + r.svarint());
    s.origSeek = prevOrig + static_cast<uint64_t>(r.svarint());
    s.origLen = static_cast<uint32_t>(r.varint());
    const uint64_t vl = r.varint();
    s.vLen = vl ? static_cast<uint32_t>(vl - 1) : plainSlotLen(s.origLen, cf.slotFactor100);
    s.vSeek = nextV + static_cast<uint64_t>(r.svarint());
    prevBranch = s.branch;
    prevBasket = s.basket;
    prevOrig = s.origSeek;
    nextV = s.vSeek + s.vLen;
  }
  if (cf.rnt) {
    cf.pages.resize(ns);
    for (uint32_t i = 0; r.ok && i < ns; ++i) {
      cf.pages[i].nbytes = r.get<uint32_t>();
      cf.pages[i].hasChecksum = r.get<uint8_t>() != 0;
    }
  }
  // Checked, not trusted: every slot lies in the layout, in order.
  if (!r.ok || r.at != b.size() || L.virtualSize < L.slotsBegin || L.slotsBegin < L.metaSeek)
    return false;
  uint64_t prev = L.slotsBegin;
  for (const auto& s : L.slots) {
    // (An RNTuple slot is the page's DECODED size plus its checksum, which may
    // be shorter than the page as stored; a TTree slot holds the record.)
    if (s.vSeek < prev || s.vSeek + s.vLen > L.virtualSize ||
        (!cf.rnt && s.vLen < s.origLen) || s.origSeek + s.origLen > L.originSize)
      return false;
    prev = s.vSeek + s.vLen;
  }
  for (const auto& win : L.windows)
    if (win.off + win.bytes.size() > L.originSize)
      return false;
  return true;
}

bool computeLayout(StoredLayout& cf, Source& src, const std::function<bool()>& fetchHead,
                   uint64_t size, uint32_t k100, bool& declined, std::string& why,
                   bool anyCachedProbe) {
  declined = false;
  why.clear();
  std::vector<uint8_t> header(100);
  cf.rnt = false;
  FileMeta fm = parseReaderTree(src, static_cast<int64_t>(size));
  if (!fm.error.empty() && fm.error.find("not found") != std::string::npos) {
    // No TTree: perhaps an RNTuple.
    if (fetchHead && !fetchHead())
      return false;
    RNTupleMeta rm = parseRNTuple(src, static_cast<int64_t>(size), "");
    if (!rm.error.empty() || !src.read(header.data(), header.size(), 0)) {
      why = fm.error;
      // Remembered only when the file has neither container: any other
      // parse failure may be a read that failed, and is tried again next time.
      declined = rm.error.find("not found") != std::string::npos;
      return false;
    }
    cf.rnt = true;
    std::string unnamed; // the codec of ranges whose setting names none: one page header
    if (!unnamedRNTupleCodec(rm, src, unnamed, anyCachedProbe))
      return false; // a failed read: tried again next time, not remembered
    cf.L = layoutForRNTupleFill(rm, size, header, cf.codecs, unnamed);
    cf.metaOrigin = {{rm.pageListOffset, rm.pageListNbytes},
                     {rm.anchor.seekFooter, rm.anchor.nbytesFooter}};
    if (cf.L.error.empty()) {
      cf.pages.resize(cf.L.slots.size());
      for (size_t i = 0; i < cf.L.slots.size(); ++i) {
        const auto& pg = rm.ranges[cf.L.slots[i].branch].pages[cf.L.slots[i].basket];
        cf.pages[i].nbytes = static_cast<uint32_t>(pg.nbytes);
        cf.pages[i].hasChecksum = pg.hasChecksum;
      }
    }
  } else {
    if (!fm.error.empty()) {
      why = fm.error;
      return false; // perhaps a failed read: tried again next time, not remembered
    }
    std::vector<uint8_t> treeKeyHeader(fm.treeKey.keylen), keysList;
    uint8_t klLen[4];
    if (!src.read(header.data(), header.size(), 0) ||
        !src.read(treeKeyHeader.data(), treeKeyHeader.size(),
                  static_cast<uint64_t>(fm.treeKey.seekkey)) ||
        !src.read(klLen, 4, static_cast<uint64_t>(fm.keyslistSeek)))
      return false;
    const int32_t kn = static_cast<int32_t>(static_cast<uint32_t>(klLen[0]) << 24 |
                                            static_cast<uint32_t>(klLen[1]) << 16 |
                                            static_cast<uint32_t>(klLen[2]) << 8 | klLen[3]);
    if (kn <= 0 || static_cast<uint64_t>(fm.keyslistSeek) + static_cast<uint64_t>(kn) > size)
      return false;
    keysList.resize(static_cast<size_t>(kn));
    if (!src.read(keysList.data(), keysList.size(), static_cast<uint64_t>(fm.keyslistSeek)))
      return false;
    std::string unnamed; // the codec of branches whose setting names none: one basket header
    if (!unnamedSettingCodec(fm, header, src, unnamed, anyCachedProbe))
      return false; // a failed read: tried again next time, not remembered
    cf.L = layoutForFill(fm, size, header, treeKeyHeader, keysList, cf.codecs, k100, unnamed);
    cf.metaOrigin = {{static_cast<uint64_t>(fm.treeKey.seekkey),
                      static_cast<uint64_t>(fm.treeKey.nbytes)}};
  }
  if (!cf.L.error.empty()) {
    declined = true; // decided by the file's own content: build() says so
    return false;
  }
  return true;
}

bool adoptable(const SlotStoreHeader& h, uint64_t size, uint64_t originMtime, uint8_t cksumKind,
               uint32_t originCksum, ValidateMode validate) {
  if (h.layoutVersion > kLayoutVersion || h.originSize != size ||
      h.layoutVersion < (h.declined ? kOldestDecision : kOldestServedLayout))
    return false;
  if (validate == ValidateMode::kSizeMtime && h.originMtime != originMtime)
    return false;
  if (validate == ValidateMode::kCksum && cksumKind != 0 &&
      (h.cksumKind != cksumKind || h.originCksum != originCksum))
    return false;
  return true;
}

} // namespace ucache::transpose
