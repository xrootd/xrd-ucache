#include "FillLayout.h"

#include "Transposer.h"

#include <algorithm>
#include <climits>
#include <string>
#include <cmath>
#include <cstring>

namespace ucache::transpose {
namespace {

// The metadata key is compressed on the path of the reader's first read, so
// speed decides the level here; on a large NanoAOD tree record ZSTD-1 came out
// no bigger than ZSTD-9 as well. The published replica keeps its own level.
constexpr int kFillMetaZstdLevel = 1;
constexpr uint64_t kAlign = 4096; // the extension starts on a page boundary

template <typename T> void bePut(uint8_t* p, T v) {
  using U = std::make_unsigned_t<T>;
  U u;
  std::memcpy(&u, &v, sizeof(T));
  for (size_t i = sizeof(T); i-- > 0;) {
    p[i] = static_cast<uint8_t>(u & 0xFF);
    u >>= 8;
  }
}
template <typename T> T beGet(const uint8_t* p) {
  using U = std::make_unsigned_t<T>;
  U u = 0;
  for (size_t i = 0; i < sizeof(T); ++i)
    u = static_cast<U>((u << 8) | p[i]);
  T v;
  std::memcpy(&v, &u, sizeof(T));
  return v;
}

// fSeekKey of a key header at `rec`, written at the key's own width: 64-bit
// for a version above 1000, 32-bit otherwise. False when a 32-bit key cannot
// hold `seek`.
bool putKeySeek(uint8_t* rec, uint16_t ver, int64_t seek) {
  if (ver > 1000) {
    bePut<int64_t>(rec + 18, seek);
    return true;
  }
  if (seek < 0 || seek >= (1ll << 31))
    return false;
  bePut<int32_t>(rec + 18, static_cast<int32_t>(seek));
  return true;
}

// The codec EVERY compression block of a basket's payload names, walked as
// decompressFrames walks them (until they hold `objlen` bytes); "" when two
// blocks disagree, one names no codec, or the chain runs out. ROOT writes one
// codec per basket; a payload that says otherwise is kept as stored rather
// than converted from a codec the policy does not name.
std::string payloadCodec(const uint8_t* p, size_t n, uint64_t objlen) {
  std::string codec;
  size_t at = 0;
  uint64_t decoded = 0;
  while (decoded < objlen) {
    if (n - at < 9)
      return "";
    const std::string c = blockCodec(p + at, n - at);
    if (c.empty() || (!codec.empty() && c != codec))
      return "";
    codec = c;
    const uint64_t csize = p[at + 3] | p[at + 4] << 8 | p[at + 5] << 16;
    const uint64_t usize = p[at + 6] | p[at + 7] << 8 | p[at + 8] << 16;
    if (usize == 0 || csize > n - at - 9)
      return "";
    at += 9 + csize;
    decoded += usize;
  }
  return codec;
}

bool listed(const std::vector<std::string>& codecs, const std::string& c) {
  return !c.empty() && std::find(codecs.begin(), codecs.end(), c) != codecs.end();
}

// The file header, in either layout. Only what a rewrite needs.
struct Header {
  int32_t version = 0, begin = 0;
  int64_t end = 0, seekFree = 0, seekInfo = 0;
  int32_t nbytesFree = 0, nfree = 0, nbytesName = 0, compress = 0, nbytesInfo = 0;
  uint8_t units = 0;
  const uint8_t* uuid = nullptr; // 18 bytes
  bool large = false;
};

bool parseHeader(const std::vector<uint8_t>& h, Header& out) {
  if (h.size() < 75 || std::memcmp(h.data(), "root", 4) != 0)
    return false;
  out.version = beGet<int32_t>(&h[4]);
  out.begin = beGet<int32_t>(&h[8]);
  out.large = out.version >= 1000000;
  if (out.large) {
    out.end = beGet<int64_t>(&h[12]);
    out.seekFree = beGet<int64_t>(&h[20]);
    out.nbytesFree = beGet<int32_t>(&h[28]);
    out.nfree = beGet<int32_t>(&h[32]);
    out.nbytesName = beGet<int32_t>(&h[36]);
    out.units = h[40];
    out.compress = beGet<int32_t>(&h[41]);
    out.seekInfo = beGet<int64_t>(&h[45]);
    out.nbytesInfo = beGet<int32_t>(&h[53]);
    out.uuid = &h[57];
  } else {
    out.end = beGet<int32_t>(&h[12]);
    out.seekFree = beGet<int32_t>(&h[16]);
    out.nbytesFree = beGet<int32_t>(&h[20]);
    out.nfree = beGet<int32_t>(&h[24]);
    out.nbytesName = beGet<int32_t>(&h[28]);
    out.units = h[32];
    out.compress = beGet<int32_t>(&h[33]);
    out.seekInfo = beGet<int32_t>(&h[37]);
    out.nbytesInfo = beGet<int32_t>(&h[41]);
    out.uuid = &h[45];
  }
  return out.begin >= 75;
}

// The 64-bit header layout — what ROOT itself writes once a file passes 2 GB
// (TFile::WriteHeader) — for a header that was written 32-bit. 75 bytes,
// which fit before fBEGIN in any file ROOT writes.
std::vector<uint8_t> largeHeader(const Header& h, int64_t newEnd) {
  std::vector<uint8_t> w(kLargeHeaderBytes, 0);
  std::memcpy(&w[0], "root", 4);
  bePut<int32_t>(&w[4], h.large ? h.version : h.version + 1000000);
  bePut<int32_t>(&w[8], h.begin);
  bePut<int64_t>(&w[12], newEnd);
  bePut<int64_t>(&w[20], h.seekFree);
  bePut<int32_t>(&w[28], h.nbytesFree);
  bePut<int32_t>(&w[32], h.nfree);
  bePut<int32_t>(&w[36], h.nbytesName);
  w[40] = 8; // fUnits: pointer width
  bePut<int32_t>(&w[41], h.compress);
  bePut<int64_t>(&w[45], h.seekInfo);
  bePut<int32_t>(&w[53], h.nbytesInfo);
  std::memcpy(&w[57], h.uuid, 18);
  return w;
}

} // namespace

bool headerWindowForEnd(const std::vector<uint8_t>& header, uint64_t newEnd, uint64_t& windowOff,
                        std::vector<uint8_t>& window, std::string& err) {
  Header H;
  if (!parseHeader(header, H)) {
    err = "file header unreadable or fBEGIN below 75";
    return false;
  }
  if (H.large) {
    window.assign(8, 0);
    bePut<int64_t>(window.data(), static_cast<int64_t>(newEnd));
    windowOff = 12;
  } else if (newEnd < (1ull << 31)) {
    window.assign(4, 0);
    bePut<int32_t>(window.data(), static_cast<int32_t>(newEnd));
    windowOff = 12;
  } else {
    window = largeHeader(H, static_cast<int64_t>(newEnd));
    windowOff = 0;
  }
  return true;
}

std::string codecOfSetting(int32_t compress, int32_t fileCompress) {
  int32_t c = compress < 0 ? fileCompress : compress;
  if (c <= 0 || c % 100 == 0) // unset, or level 0: stored uncompressed
    return "";
  switch (c / 100) {
  case 1: return "zlib";
  case 2: return "lzma";
  case 4: return "lz4";
  case 5: return "zstd";
  default: return ""; // 0 (global default, meaning unknown here), 3 (the old
                      // ROOT algorithm): not transcoded
  }
}

bool unnamedSettingCodec(const FileMeta& fm, const std::vector<uint8_t>& header, Source& src,
                         std::string& codec) {
  codec.clear();
  Header H;
  if (!fm.error.empty() || !parseHeader(header, H))
    return true; // nothing to decide: the layout declines on these itself
  std::vector<const BranchInfo*> unnamed;
  for (const auto& br : fm.branches)
    if (br.writeBasket > 0 && !br.externalFile && !br.basketSeek.empty() && br.basketSeek[0] > 0 &&
        br.basketBytes[0] > 0 && br.zipBytes < br.totBytes &&
        codecOfSetting(br.compress, H.compress).empty())
      unnamed.push_back(&br);
  // The most compressed first: its first basket is the least likely to be one
  // that did not shrink and was stored uncompressed.
  std::stable_sort(unnamed.begin(), unnamed.end(), [](const BranchInfo* a, const BranchInfo* b) {
    return a->totBytes - a->zipBytes > b->totBytes - b->zipBytes;
  });
  constexpr size_t kTries = 3;
  // A key header is at most ~560 bytes (three names of up to 255); the
  // compression header follows it.
  constexpr uint64_t kHead = 1024;
  for (size_t i = 0; i < unnamed.size() && i < kTries; ++i) {
    const BranchInfo& br = *unnamed[i];
    const uint64_t off = static_cast<uint64_t>(br.basketSeek[0]);
    const uint64_t want = std::min<uint64_t>(static_cast<uint64_t>(br.basketBytes[0]), kHead);
    if (off + want > static_cast<uint64_t>(fm.fend))
      continue; // outside the file: the layout never relocates it
    uint8_t head[kHead];
    if (!src.has(off, want) || !src.read(head, want, off))
      return false;
    auto k = parseKey(head, static_cast<size_t>(want), 0);
    if (!k || k->cls != "TBasket" || k->nbytes <= 0 || k->keylen >= want)
      continue; // not a basket record where the metadata says one is
    if (k->objlen == k->nbytes - static_cast<int32_t>(k->keylen))
      continue; // this basket did not shrink and was stored as it is: ask the next
    codec = blockCodec(head + k->keylen, static_cast<size_t>(want - k->keylen));
    return true;
  }
  return true;
}

std::string declineNote(const FillLayout& L) {
  if (L.error.empty())
    return "";
  if (L.codecDecline)
    return L.error;
  return L.error + "; `ucache recompress` can build its replica after the run, from what the run "
                   "caches";
}

FillLayout layoutForFill(const FileMeta& fm, uint64_t fileSize, const std::vector<uint8_t>& header,
                         const std::vector<uint8_t>& treeKeyHeader,
                         const std::vector<uint8_t>& keysList,
                         const std::vector<std::string>& codecs, uint32_t slotFactor100,
                         const std::string& unnamedCodec) {
  FillLayout L;
  auto decline = [&](std::string why, bool forCodec = false) {
    L.error = std::move(why);
    L.codecDecline = forCodec;
    L.windows.clear();
    L.slots.clear();
    L.relocated.clear();
    L.metaRecord.clear();
    return L;
  };
  if (!fm.error.empty())
    return decline("parse: " + fm.error);
  if (slotFactor100 < 100) // the original must fit its slot: it is what a basket falls back to
    return decline("slot factor must be at least 1");
  Header H;
  if (!parseHeader(header, H))
    return decline("file header unreadable or fBEGIN below 75");
  if (H.end != fm.fend)
    return decline("file header disagrees with the parse on fEND");
  if (treeKeyHeader.size() != fm.treeKey.keylen || fm.treeKey.keylen < 26)
    return decline("tree key header missing");
  if (fm.zipBytesOff == 0 || fm.autoFlushOff == 0 || fm.zipBytes <= 0)
    return decline("tree record fields not located");

  // Which branches, in which order. Branch-major and in tree order, so a
  // column's slots are adjacent in the virtual file.
  std::vector<std::string> unlisted; // codecs seen and not listed, for the decline
  bool unconverted = false;          // compressed in a codec named nowhere
  bool stored = false;               // uncompressed: nothing to convert
  bool anyListed = false;            // else a decline is for the codec alone
  for (uint32_t b = 0; b < fm.branches.size(); ++b) {
    const BranchInfo& br = fm.branches[b];
    if (br.writeBasket <= 0 || br.externalFile || br.zipBytesOff == 0)
      continue;
    std::string codec = codecOfSetting(br.compress, H.compress);
    if (codec.empty() && br.zipBytes < br.totBytes) { // compressed all the same
      codec = unnamedCodec;
      unconverted = unconverted || codec.empty();
    } else if (codec.empty()) {
      stored = true;
      continue;
    }
    if (!listed(codecs, codec)) {
      if (!codec.empty() && std::find(unlisted.begin(), unlisted.end(), codec) == unlisted.end())
        unlisted.push_back(codec);
      continue;
    }
    anyListed = true;
    bool inside = true;
    for (int32_t i = 0; i < br.writeBasket && inside; ++i)
      inside = br.basketSeek[i] > 0 && br.basketBytes[i] > 0 &&
               static_cast<uint64_t>(br.basketSeek[i]) + static_cast<uint64_t>(br.basketBytes[i]) <=
                   static_cast<uint64_t>(fm.fend);
    if (inside) // a basket with no seek lives inside the tree record itself;
                // one past EOF is a parse nothing should act on
      L.relocated.push_back(b);
  }
  if (L.relocated.empty() && !anyListed) {
    if (!unlisted.empty()) {
      std::string seen, list;
      for (const auto& c : unlisted)
        seen += (seen.empty() ? "" : ", ") + c;
      for (const auto& c : codecs)
        list += (list.empty() ? "" : ",") + c;
      return decline("no relocatable branch: its baskets are " + seen +
                         ", which recompress_codecs (" + (list.empty() ? "empty" : list) +
                         ") does not name",
                     true);
    }
    if (unconverted)
      return decline("no relocatable branch: its baskets are compressed in a codec that is not "
                     "converted",
                     true);
    if (stored)
      return decline("no relocatable branch: its baskets are stored uncompressed", true);
  }
  if (L.relocated.empty())
    return decline("no relocatable branch");

  // Geometry: [fEND, metaSeek) nothing, [metaSeek, slotsBegin) the metadata
  // key's reservation, then the slots. The reservation is the key header plus
  // the RAW tree record: the patched record has the same length (every patched
  // field is fixed-width), and its compressed size is not known until the
  // slot positions it contains are.
  if (fileSize != static_cast<uint64_t>(fm.fend)) // bytes past fEND would sit under the extension
    return decline("file size " + std::to_string(fileSize) + " differs from fEND " +
                   std::to_string(fm.fend));
  L.originSize = fileSize;
  L.metaSeek = (L.originSize + kAlign - 1) / kAlign * kAlign;
  const uint64_t reserve = fm.treeKey.keylen + fm.treeBlob.size();
  L.slotsBegin = L.metaSeek + reserve;

  std::vector<uint8_t> blob = fm.treeBlob;
  uint64_t at = L.slotsBegin;
  for (uint32_t b : L.relocated) {
    const BranchInfo& br = fm.branches[b];
    for (int32_t i = 0; i < br.writeBasket; ++i) {
      FillSlot s;
      s.branch = b;
      s.basket = static_cast<uint32_t>(i);
      s.origSeek = static_cast<uint64_t>(br.basketSeek[i]);
      s.origLen = static_cast<uint32_t>(br.basketBytes[i]);
      const uint64_t want = static_cast<uint64_t>(s.origLen) * slotFactor100 / 100;
      s.vLen = static_cast<uint32_t>(std::min<uint64_t>(want, INT32_MAX));
      s.vSeek = at;
      at += s.vLen;
      bePut<int64_t>(blob.data() + br.seekArrayOff + 8ull * i, static_cast<int64_t>(s.vSeek));
      bePut<int32_t>(blob.data() + br.bytesArrayOff + 4ull * i, static_cast<int32_t>(s.vLen));
      L.slots.push_back(s);
    }
  }
  L.virtualSize = at;
  // fZipBytes (tree and branch) and fAutoFlush are left as the file states
  // them: ROOT sizes its read cache from them (TTree::GetCacheAutoSize), and
  // the real total keeps that cache the size it has for the original file.
  // Scaled with the slots it was three times that, for every branch of the
  // tree, read or not. The price is that a fill holding padded slots splits
  // into more requests (measured: no cost in time).

  // The relocated metadata key: the original header bytes (same length),
  // fNbytes = the record's length, fSeekKey = metaSeek; fObjlen is untouched,
  // so ROOT still inflates it to the same size. Stored raw if compressing does
  // not shrink it.
  std::vector<uint8_t> payload = encodeZstdFrames(blob.data(), blob.size(), kFillMetaZstdLevel);
  if (payload.empty() || payload.size() >= blob.size())
    payload = blob;
  L.metaRecord = treeKeyHeader;
  L.metaRecord.insert(L.metaRecord.end(), payload.begin(), payload.end());
  if (L.metaRecord.size() > reserve)
    return decline("metadata key outgrew its reservation");
  bePut<int32_t>(L.metaRecord.data(), static_cast<int32_t>(L.metaRecord.size()));
  if (!putKeySeek(L.metaRecord.data(), fm.treeKey.ver, static_cast<int64_t>(L.metaSeek)))
    return decline("tree key is 32-bit and the extension starts past 2 GiB");

  // The keys list, patched in place: its size never changes, so neither does
  // anything that points at it (see Transposer.cc for why it is never moved).
  auto kl = parseKey(keysList.data(), keysList.size(), 0);
  if (!kl || kl->nbytes <= 0 || static_cast<size_t>(kl->nbytes) != keysList.size() ||
      kl->keylen + 4u > keysList.size())
    return decline("keys-list record malformed");
  if (fm.keyslistSeek < 100 ||
      static_cast<uint64_t>(fm.keyslistSeek) + keysList.size() > L.originSize)
    return decline("keys-list record does not lie inside the original file");
  std::vector<uint8_t> klist = keysList;
  const int32_t nkeys = beGet<int32_t>(klist.data() + kl->keylen);
  size_t pos = kl->keylen + 4;
  bool patched = false;
  for (int32_t i = 0; i < nkeys; ++i) {
    auto e = parseKey(klist.data(), klist.size(), pos);
    if (!e)
      return decline("keys-list entry malformed");
    if (e->cls == "TTree" && e->name == fm.treeKey.name && e->seekkey == fm.treeKey.seekkey) {
      bePut<int32_t>(klist.data() + pos, static_cast<int32_t>(L.metaRecord.size()));
      if (!putKeySeek(klist.data() + pos, e->ver, static_cast<int64_t>(L.metaSeek)))
        return decline("keys-list entry is 32-bit and the metadata key lies past 2 GiB");
      patched = true;
    }
    pos += e->keylen;
  }
  if (!patched)
    return decline("live tree entry not found in the keys list");

  // The header: fEND at its own width, or the whole header rewritten in the
  // 64-bit layout when a 32-bit header cannot hold the virtual end.
  {
    FillLayout::Window w;
    std::string err;
    if (!headerWindowForEnd(header, L.virtualSize, w.off, w.bytes, err))
      return decline(err);
    L.windows.push_back(std::move(w));
  }
  L.windows.push_back({static_cast<uint64_t>(fm.keyslistSeek), std::move(klist)});
  return L;
}

ConvertedBasket convertBasket(const uint8_t* rec, size_t n, uint32_t slotLen,
                              const std::vector<std::string>& codecs) {
  ConvertedBasket c;
  auto k = parseKey(rec, n, 0);
  if (!k || k->cls != "TBasket" || k->nbytes <= 0 || static_cast<size_t>(k->nbytes) != n ||
      k->keylen > n) {
    c.error = "not a basket record";
    return c;
  }
  auto original = [&] {
    c.kind = ConvertedBasket::kOriginal;
    c.record.assign(rec, rec + n);
    return c;
  };
  if (n > slotLen) {
    c.error = "record longer than its slot";
    return c;
  }
  const uint8_t* pay = rec + k->keylen;
  const size_t payLen = n - k->keylen;
  const bool storedRaw = k->objlen == k->nbytes - static_cast<int32_t>(k->keylen);
  if (storedRaw)
    return original(); // nothing for the reader to decode already
  if (k->objlen <= 0)
    return original();
  // The basket's own blocks name what it holds, never the branch's setting.
  c.codec = blockCodec(pay, payLen);
  if (!listed(codecs, c.codec) ||
      payloadCodec(pay, payLen, static_cast<uint64_t>(k->objlen)) != c.codec)
    return original();
  std::vector<uint8_t> raw = decompressFrames(pay, payLen, static_cast<uint64_t>(k->objlen));
  if (raw.empty())
    return original();
  std::vector<uint8_t> z = encodeZstdFrames(raw.data(), raw.size(), 1);
  const std::vector<uint8_t>* use = nullptr;
  if (!z.empty() && z.size() < raw.size()) {
    if (k->keylen + z.size() <= slotLen) {
      use = &z;
      c.kind = ConvertedBasket::kZstd;
    }
  } else if (k->keylen + raw.size() <= slotLen) { // ZSTD cannot shrink it: raw
    use = &raw;
    c.kind = ConvertedBasket::kRaw;
  }
  if (!use)
    return original();
  c.record.assign(rec, rec + k->keylen); // the key header keeps its length
  c.record.insert(c.record.end(), use->begin(), use->end());
  bePut<int32_t>(c.record.data(), static_cast<int32_t>(c.record.size())); // fNbytes
  return c;
}

bool patchKeySeek(uint8_t* record, size_t n, uint64_t seek) {
  if (n < 26 || seek > static_cast<uint64_t>(INT64_MAX))
    return false;
  const uint16_t ver = beGet<uint16_t>(record + 4);
  if (ver > 1000 && n < 34)
    return false;
  return putKeySeek(record, ver, static_cast<int64_t>(seek));
}

bool placeInSlot(const ConvertedBasket& c, const FillSlot& slot, uint8_t* out, std::string& err) {
  if (!c.error.empty() || c.record.size() < 26) {
    err = c.error.empty() ? "empty record" : c.error;
    return false;
  }
  if (c.record.size() > slot.vLen) {
    err = "record longer than its slot";
    return false;
  }
  std::memcpy(out, c.record.data(), c.record.size());
  const uint16_t ver = beGet<uint16_t>(out + 4);
  if (!putKeySeek(out, ver, static_cast<int64_t>(slot.vSeek))) {
    err = "32-bit basket key cannot address a slot past 2 GiB";
    return false;
  }
  std::memset(out + c.record.size(), 0, slot.vLen - c.record.size());
  return true;
}

bool exactOriginRanges(const FillLayout& L,
                       const std::vector<std::pair<uint64_t, uint64_t>>& metaOrigin, uint64_t off,
                       uint64_t len, std::vector<std::pair<uint64_t, uint64_t>>& out,
                       const std::function<uint32_t(uint32_t)>* realLen,
                       std::pair<uint64_t, uint64_t> mapMeta) {
  const SlotsIn in = [&L](uint64_t a, uint64_t b,
                          const std::function<bool(uint32_t, const FillSlot&)>& f) {
    // The first slot that ends past a: slots ascend and do not overlap.
    auto it = std::lower_bound(L.slots.begin(), L.slots.end(), a,
                               [](const FillSlot& s, uint64_t o) { return s.vSeek + s.vLen <= o; });
    for (; it != L.slots.end() && it->vSeek < b; ++it)
      if (!f(static_cast<uint32_t>(it - L.slots.begin()), *it))
        break;
    return true;
  };
  return exactOriginRanges(L, in, metaOrigin, off, len, out, realLen, mapMeta);
}

bool exactOriginRanges(const FillLayout& L, const SlotsIn& slotsIn,
                       const std::vector<std::pair<uint64_t, uint64_t>>& metaOrigin, uint64_t off,
                       uint64_t len, std::vector<std::pair<uint64_t, uint64_t>>& out,
                       const std::function<uint32_t(uint32_t)>* realLen,
                       std::pair<uint64_t, uint64_t> mapMeta) {
  const uint64_t end = off + len;
  if (!len || end < off)
    return true; // a request with no bytes, or one no layout can hold: nothing read
  if (off < L.originSize)
    out.emplace_back(off, std::min(end, L.originSize) - off);
  // A metadata record -- map 0's, or the handle's own map's -- read whole is
  // the original tree record; a part of one is no count of anything.
  auto meta = [&](uint64_t seek, uint64_t n) {
    const uint64_t e = seek + n;
    if (!n || off >= e || end <= seek)
      return true;
    if (off > seek || end < e)
      return false;
    out.insert(out.end(), metaOrigin.begin(), metaOrigin.end());
    return true;
  };
  if (!meta(L.metaSeek, L.metaRecord.size()) || !meta(mapMeta.first, mapMeta.second))
    return false;
  if (end <= L.slotsBegin)
    return true;
  bool whole = true;
  const bool had = slotsIn(off, end, [&](uint32_t i, const FillSlot& s) {
    if (off > s.vSeek)
      return whole = false;
    if (end < s.vSeek + s.vLen) {
      // Short of the slot's end: whole only when it reaches the basket's real
      // length (a mixed map states that), and this is the last slot it reads.
      const uint32_t real = realLen ? (*realLen)(i) : 0;
      if (!real || end < s.vSeek + real)
        return whole = false;
    }
    out.emplace_back(s.origSeek, s.origLen);
    return true;
  });
  return had && whole;
}

MapTables mapTables(const FillLayout& L) {
  MapTables t;
  if (L.metaRecord.empty() || L.slotsBegin <= L.metaSeek)
    return t;
  t.first = (L.metaSeek + L.metaRecord.size() + 7) / 8 * 8;
  t.end = L.slotsBegin;
  return t;
}

uint64_t mapSeekLimit(const FillLayout& L) {
  constexpr uint64_t kNarrow = INT32_MAX;
  if (L.metaRecord.size() < 6 || beGet<uint16_t>(L.metaRecord.data() + 4) <= 1000)
    return kNarrow;
  for (const auto& w : L.windows) {
    auto kl = parseKey(w.bytes.data(), w.bytes.size(), 0);
    if (!kl || kl->nbytes <= 0 || static_cast<size_t>(kl->nbytes) != w.bytes.size() ||
        kl->keylen + 4u > w.bytes.size())
      continue;
    const int32_t nkeys = beGet<int32_t>(w.bytes.data() + kl->keylen);
    size_t pos = kl->keylen + 4;
    for (int32_t i = 0; i < nkeys; ++i) {
      auto e = parseKey(w.bytes.data(), w.bytes.size(), pos);
      if (!e)
        break;
      if (e->cls == "TTree" && e->seekkey == static_cast<int64_t>(L.metaSeek))
        return e->ver > 1000 ? UINT64_MAX : kNarrow;
      pos += e->keylen;
    }
  }
  return kNarrow;
}

bool statedRealLengths(const std::vector<uint8_t>& blob, const FileMeta& fields,
                       const FillLayout& L, std::vector<std::pair<uint32_t, uint32_t>>& real) {
  real.clear();
  for (uint32_t i = 0; i < L.slots.size(); ++i) {
    const FillSlot& s = L.slots[i];
    if (s.branch >= fields.branches.size())
      return false;
    const uint64_t bo = fields.branches[s.branch].bytesArrayOff + 4ull * s.basket;
    if (bo + 4 > blob.size())
      return false;
    const int32_t n = beGet<int32_t>(blob.data() + bo);
    if (n <= 0 || static_cast<uint32_t>(n) > s.vLen)
      return false;
    if (static_cast<uint32_t>(n) != s.vLen)
      real.emplace_back(i, static_cast<uint32_t>(n));
  }
  return true;
}

bool decodeKeyRecord(const std::vector<uint8_t>& rec, std::vector<uint8_t>& blob, uint16_t& keylen) {
  auto k = parseKey(rec.data(), rec.size(), 0);
  if (!k || k->nbytes <= 0 || static_cast<size_t>(k->nbytes) != rec.size() || k->keylen >= rec.size() ||
      k->objlen <= 0)
    return false;
  keylen = k->keylen;
  const uint8_t* pay = rec.data() + k->keylen;
  const size_t n = rec.size() - k->keylen;
  if (n == static_cast<size_t>(k->objlen))
    blob.assign(pay, pay + n); // stored raw
  else
    blob = decompressFrames(pay, n, static_cast<uint64_t>(k->objlen));
  return blob.size() == static_cast<size_t>(k->objlen);
}

bool stateRealLengths(std::vector<uint8_t>& blob, const FileMeta& fields, const FillLayout& L,
                      const std::vector<std::pair<uint32_t, uint32_t>>& real, std::string& err) {
  return stateRealLengths(
      blob, fields, static_cast<uint32_t>(L.slots.size()),
      [&L](uint32_t i) -> const FillSlot& { return L.slots[i]; }, real, err);
}

bool stateRealLengths(std::vector<uint8_t>& blob, const FileMeta& fields, uint32_t nSlots,
                      const std::function<const FillSlot&(uint32_t)>& slot,
                      const std::vector<std::pair<uint32_t, uint32_t>>& real, std::string& err) {
  std::vector<int64_t> delta(fields.branches.size(), 0);
  int64_t total = 0;
  for (const auto& [i, len] : real) {
    if (i >= nSlots) {
      err = "slot out of range";
      return false;
    }
    const FillSlot& s = slot(i);
    if (s.branch >= fields.branches.size()) {
      err = "branch out of range";
      return false;
    }
    const BranchInfo& br = fields.branches[s.branch];
    const uint64_t so = br.seekArrayOff + 8ull * s.basket, bo = br.bytesArrayOff + 4ull * s.basket;
    if (static_cast<int32_t>(s.basket) >= br.writeBasket || so + 8 > blob.size() ||
        bo + 4 > blob.size() || beGet<int64_t>(blob.data() + so) != static_cast<int64_t>(s.vSeek) ||
        beGet<int32_t>(blob.data() + bo) != static_cast<int32_t>(s.vLen)) {
      err = "basket not where the layout put it";
      return false;
    }
    if (len == 0 || len > s.vLen) {
      err = "a real length must be in (0, slot length]";
      return false;
    }
    bePut<int32_t>(blob.data() + bo, static_cast<int32_t>(len));
    const int64_t d = static_cast<int64_t>(len) - static_cast<int64_t>(s.origLen);
    delta[s.branch] += d;
    total += d;
  }
  for (size_t b = 0; b < delta.size(); ++b) {
    if (!delta[b])
      continue;
    const uint64_t zo = fields.branches[b].zipBytesOff;
    if (!zo || zo + 8 > blob.size()) {
      err = "branch fZipBytes not located";
      return false;
    }
    bePut<int64_t>(blob.data() + zo, beGet<int64_t>(blob.data() + zo) + delta[b]);
  }
  if (total) {
    if (!fields.zipBytesOff || fields.zipBytesOff + 8 > blob.size()) {
      err = "tree fZipBytes not located";
      return false;
    }
    bePut<int64_t>(blob.data() + fields.zipBytesOff,
                   beGet<int64_t>(blob.data() + fields.zipBytesOff) + total);
  }
  return true;
}

bool stateSeeks(std::vector<uint8_t>& blob, const FileMeta& fields, uint32_t nSlots,
                const std::function<const FillSlot&(uint32_t)>& slot,
                const std::vector<std::pair<uint32_t, uint64_t>>& seeks, std::string& err) {
  for (const auto& [i, seek] : seeks) {
    if (i >= nSlots) {
      err = "slot out of range";
      return false;
    }
    const FillSlot& s = slot(i);
    if (s.branch >= fields.branches.size()) {
      err = "branch out of range";
      return false;
    }
    const BranchInfo& br = fields.branches[s.branch];
    const uint64_t so = br.seekArrayOff + 8ull * s.basket;
    if (static_cast<int32_t>(s.basket) >= br.writeBasket || so + 8 > blob.size() ||
        beGet<int64_t>(blob.data() + so) != static_cast<int64_t>(s.vSeek)) {
      err = "basket not where the layout put it";
      return false;
    }
    if (seek > static_cast<uint64_t>(INT64_MAX)) {
      err = "address out of range";
      return false;
    }
    bePut<int64_t>(blob.data() + so, static_cast<int64_t>(seek));
  }
  return true;
}

bool mapMetaRecord(const FillLayout& L, const std::vector<uint8_t>& blob, uint64_t seek,
                   std::vector<uint8_t>& out, std::string& err) {
  auto k = parseKey(L.metaRecord.data(), L.metaRecord.size(), 0);
  if (!k || k->keylen >= L.metaRecord.size()) {
    err = "map 0's metadata key unreadable";
    return false;
  }
  std::vector<uint8_t> payload = encodeZstdFrames(blob.data(), blob.size(), kFillMetaZstdLevel);
  if (payload.empty() || payload.size() >= blob.size()) {
    err = "tree record does not compress";
    return false;
  }
  out.assign(L.metaRecord.begin(), L.metaRecord.begin() + k->keylen);
  out.insert(out.end(), payload.begin(), payload.end());
  bePut<int32_t>(out.data(), static_cast<int32_t>(out.size()));
  if (!putKeySeek(out.data(), k->ver, static_cast<int64_t>(seek))) {
    err = "tree key is 32-bit and the map lies past 2 GiB";
    return false;
  }
  return true;
}

bool keysListForMap(const FillLayout& L, uint64_t seek, uint32_t nbytes, FillLayout::Window& out,
                    std::string& err) {
  for (const auto& w : L.windows) {
    auto kl = parseKey(w.bytes.data(), w.bytes.size(), 0);
    if (!kl || kl->nbytes <= 0 || static_cast<size_t>(kl->nbytes) != w.bytes.size() ||
        kl->keylen + 4u > w.bytes.size())
      continue; // not the keys list (the header's window)
    std::vector<uint8_t> klist = w.bytes;
    const int32_t nkeys = beGet<int32_t>(klist.data() + kl->keylen);
    size_t pos = kl->keylen + 4;
    for (int32_t i = 0; i < nkeys; ++i) {
      auto e = parseKey(klist.data(), klist.size(), pos);
      if (!e)
        break;
      if (e->cls == "TTree" && e->seekkey == static_cast<int64_t>(L.metaSeek) &&
          e->nbytes == static_cast<int32_t>(L.metaRecord.size())) {
        bePut<int32_t>(klist.data() + pos, static_cast<int32_t>(nbytes));
        if (!putKeySeek(klist.data() + pos, e->ver, static_cast<int64_t>(seek))) {
          err = "keys-list entry is 32-bit and the map lies past 2 GiB";
          return false;
        }
        out.off = w.off;
        out.bytes = std::move(klist);
        return true;
      }
      pos += e->keylen;
    }
  }
  err = "live tree entry not found in the keys list";
  return false;
}

} // namespace ucache::transpose
