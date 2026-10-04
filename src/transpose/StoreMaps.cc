#include "StoreMaps.h"

#include "IOBackend.h"
#include "Log.h"
#include "RNTupleMeta.h"
#include "RNTupleRewrite.h"
#include "Transposer.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ucache::transpose {

namespace tp = ::ucache::transpose;

// UCACHE_TEST_MAP_TIMING=quiet_s,interval_s,close_delay_ms: for gates, which
// cannot wait an hour. Read once.
const MapTiming& mapTiming() {
  static const MapTiming t = [] {
    MapTiming m;
    unsigned long long q = 0, i = 0, d = 0;
    if (const char* v = std::getenv("UCACHE_TEST_MAP_TIMING"))
      if (std::sscanf(v, "%llu,%llu,%llu", &q, &i, &d) == 3) {
        m.quietS = q;
        m.intervalS = i;
        m.closeDelayMs = d;
      }
    return m;
  }();
  return t;
}

uint64_t wallSeconds() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

std::vector<uint8_t> encodeMapRecord(const MapHead& h, const std::vector<uint8_t>& meta,
                                     const std::vector<uint8_t>& keysList,
                                     const ColdMap* compact) {
  const size_t head = compact ? kMapHead2 : kMapHead;
  const size_t total = head + meta.size() + keysList.size() +
                       (compact ? compact->header.size() + compact->recs.size() * kMapRecBytes : 0);
  // Sized once and filled in place (growing it piece by piece trips a false
  // -Wstringop-overread in gcc 11).
  std::vector<uint8_t> b(total, 0);
  auto put = [&](size_t off, auto v) { std::memcpy(b.data() + off, &v, sizeof v); };
  std::memcpy(b.data(), compact ? kMapMagic2 : kMapMagic, 8);
  put(8, static_cast<uint32_t>(meta.size()));
  put(12, static_cast<uint32_t>(keysList.size()));
  put(16, h.madeS);
  put(24, h.metaSeek);
  put(32, h.keysListOff);
  put(40, h.covered);
  if (compact) {
    put(48, compact->rangeLo);
    put(56, compact->rangeLen);
    put(64, compact->headerOff);
    put(72, static_cast<uint32_t>(compact->header.size()));
    put(76, static_cast<uint32_t>(compact->recs.size()));
    put(80, compact->firstOff);
    put(88, static_cast<uint8_t>(compact->sums ? 1 : 0));
  }
  size_t at = head;
  auto append = [&](const uint8_t* p, size_t n) {
    if (n)
      std::memcpy(b.data() + at, p, n);
    at += n;
  };
  append(meta.data(), meta.size());
  append(keysList.data(), keysList.size());
  if (compact) {
    append(compact->header.data(), compact->header.size());
    for (const auto& r : compact->recs) {
      uint8_t* e = b.data() + at;
      std::memcpy(e, &r.slot, 4);
      std::memcpy(e + 4, &r.len, 4);
      std::memcpy(e + 8, &r.crc, 4);
      e[12] = r.kind;
      std::memcpy(e + 16, &r.storeOff, 8);
      at += kMapRecBytes;
    }
  }
  return b;
}
bool decodeMapHead(const uint8_t* p, size_t n, MapHead& h) {
  const bool v2 = n >= kMapHead2 && std::memcmp(p, kMapMagic2, 8) == 0;
  if (!v2 && (n < kMapHead || std::memcmp(p, kMapMagic, 8) != 0))
    return false;
  auto get = [&](size_t off, auto& v) { std::memcpy(&v, p + off, sizeof v); };
  get(8, h.metaLen);
  get(12, h.keysListLen);
  get(16, h.madeS);
  get(24, h.metaSeek);
  get(32, h.keysListOff);
  get(40, h.covered);
  if (v2) {
    get(48, h.rangeLo);
    get(56, h.rangeLen);
    get(64, h.headerOff);
    get(72, h.headerLen);
    get(76, h.nRecs);
    get(80, h.firstOff);
    get(88, h.flags);
    if (!h.rangeLen) // a compact map states something
      return false;
  }
  return true;
}

void StoreMaps::noteSlotLocked(const SlotEntry& e, bool foreign) {
  if (e.slot >= nSlots())
    return;
  if (loaded)
    (foreign ? lastForeignS : lastOwnS) = wallSeconds();
  // A slot's real length is its FIRST record's, for good: what every map
  // states. A slot committed again (its record failed its check and was
  // converted anew) is not counted twice, and serving refuses a record of
  // another length to a reader holding a map (copySlot). A kept original is
  // its own length (looked up where its slot is at hand) and is not counted
  // in convertedBytes: nothing here decodes a branch.
  const bool kept = e.kind == SlotEntry::kKept;
  if (!firstLens.emplace(e.slot, kept ? 0 : e.len).second)
    return;
  if (!kept)
    convertedBytes += e.len;
}

namespace {
// Where a compact map's header may state its end: in one of map 1's windows,
// or -- when map 1's header is 32-bit and the map ends past 2 GiB -- the whole
// header in the 64-bit form at offset 0, as ROOT rewrites it once a file
// passes 2 GB. Each handle is served its own map's header there.
bool headerWindowFits(const tp::FillLayout& L, uint64_t off, size_t len) {
  for (const auto& w : L.windows)
    if (w.off == off && w.bytes.size() == len)
      return true;
  return off == 0 && len == tp::kLargeHeaderBytes;
}
} // namespace

bool StoreMaps::noteMapLocked(const SlotEntry& e, const MapHead& h) {
  const bool metaInRange = h.compact() && h.metaSeek == h.rangeLo;
  if (mapsOff || h.metaLen < 26 || h.recordLen() != e.len ||
      (metaInRange ? h.metaLen > h.firstOff || h.firstOff > h.rangeLen
                   : h.metaSeek < tables.first || h.metaSeek + h.metaLen > tables.end))
    return false;
  bool window = false;
  const bool header = !h.compact() || headerWindowFits(L, h.headerOff, h.headerLen);
  for (const auto& w : L.windows)
    window = window || (w.off && w.off == h.keysListOff && w.bytes.size() == h.keysListLen);
  // A compact map's range lies past map 1's slots; its header replaces map 1's.
  if (!window || !header || (h.compact() && (h.rangeLo < L.virtualSize || !h.nRecs ||
                                             h.rangeLo + h.rangeLen < h.rangeLo)))
    return false;
  auto it = std::lower_bound(maps.begin(), maps.end(), e.off,
                             [](const MapPlace& m, uint64_t v) { return m.seq < v; });
  if (it != maps.end() && it->seq == e.off)
    return true;
  MapPlace m;
  m.seq = e.off;
  m.head = h;
  m.e = e;
  maps.insert(it, std::move(m));
  return true;
}

int64_t StoreMaps::mapDueLocked(uint64_t now, bool atExit) const {
  if (mapsOff || mapsImpossible || storeGone.load())
    return -1;
  const uint64_t covered = maps.empty() ? 0 : maps.back().head.covered;
  const uint64_t uncovered = convertedBytes > covered ? convertedBytes - covered : 0;
  const uint64_t need = std::max(convertedBytes / kMapShare, std::min(kMapMinBytes, covered));
  if (!uncovered || uncovered < need)
    return -1;
  uint64_t at = std::max(now, mapsFullUntilS);
  if (!maps.empty())
    at = std::max(at, maps.back().head.madeS + mapTiming().intervalS);
  if (lastForeignS)
    at = std::max(at, lastForeignS + mapTiming().quietS);
  if (!atExit && handles > 0 && lastOwnS)
    at = std::max(at, lastOwnS + mapTiming().quietS);
  return static_cast<int64_t>(at - now);
}

uint64_t StoreMaps::freePlaceLocked(uint64_t len, uint64_t now, const InUseRecord& inUse,
                                   uint64_t& retryS) const {
  // A map replaced by a newer one keeps its place while it is in use: handed
  // out or released within `in_use_seconds` by any process (the in-use
  // record), or held by a handle of this one. A map nobody was handed frees
  // at once; a place a newer map took over went with it.
  const uint64_t window = inUseSeconds;
  const uint64_t id = store ? store->header().storeId : 0;
  std::vector<std::pair<uint64_t, uint64_t>> live, newer;
  retryS = UINT64_MAX;
  for (size_t k = maps.size(); k-- > 0;) {
    const MapHead& h = maps[k].head;
    const uint64_t a = h.metaSeek, b = h.metaSeek + h.metaLen;
    // What identifies the map in the in-use record: a compact map's addresses.
    const uint64_t ia = h.compact() ? h.rangeLo : a, ib = h.compact() ? h.rangeLo + h.rangeLen : b;
    bool gone = false;
    if (k + 1 < maps.size()) { // replaced
      const bool held = heldHere(ia, ib);
      gone = !held && !inUse.inUse(id, ia, ib, now, window);
      if (!gone) {
        const uint64_t at = inUse.freeAtS(id, ia, ib, window);
        retryS = std::min(retryS, held || at <= now ? now + InUseRecord::kHoldRefreshS : at);
      }
    }
    for (const auto& [x, y] : newer)
      gone = gone || (a < y && x < b);
    if (!gone)
      live.emplace_back(a, b);
    newer.emplace_back(a, b);
  }
  // A map of another store -- one this store replaced -- keeps its place too
  // while it is in use: its readers may hold positions in its tree record, and
  // this store's map there would hand them other bytes.
  for (const auto& r : inUse.ranges) {
    if (r.storeId == id || r.hi <= tables.first || r.lo >= tables.end ||
        !inUse.inUse(r.storeId, r.lo, r.hi, now, window))
      continue;
    live.emplace_back(r.lo, r.hi);
    const uint64_t at = inUse.freeAtS(r.storeId, r.lo, r.hi, window);
    retryS = std::min(retryS, at <= now ? now + InUseRecord::kHoldRefreshS : at);
  }
  const uint64_t end = std::min(tables.end, seekLimit == UINT64_MAX ? UINT64_MAX : seekLimit + len);
  if (end <= tables.first || end - tables.first < len)
    return 0;
  std::sort(live.begin(), live.end());
  if (live.empty() || live.front().first - tables.first >= len)
    return tables.first;
  const uint64_t last = (live.back().second + 7) / 8 * 8;
  const uint64_t tail = (end - len) / 8 * 8;
  if (tail >= last && tail >= tables.first)
    return tail;
  uint64_t at = tables.first;
  for (const auto& [a, b] : live) {
    if (a >= at && a - at >= len)
      return at;
    at = std::max(at, (b + 7) / 8 * 8);
  }
  return 0;
}

std::shared_ptr<const ColdMap> StoreMaps::mapBySeq(uint64_t seq) {
  SlotEntry e;
  MapHead h;
  {
    std::lock_guard<std::mutex> g(mu);
    auto it = std::lower_bound(maps.begin(), maps.end(), seq,
                               [](const MapPlace& m, uint64_t v) { return m.seq < v; });
    if (it == maps.end() || it->seq != seq)
      return nullptr;
    if (it->map)
      return it->map;
    e = it->e;
    h = it->head;
  }
  std::vector<uint8_t> rec;
  if (!store || !store->readRecord(e, rec) || rec.size() != h.recordLen()) {
    UCACHE_WARN("map for %s failed its check; its handles are shown the slot layout",
                key.key.c_str());
    return nullptr;
  }
  auto m = std::make_shared<ColdMap>();
  m->seq = seq;
  m->madeS = h.madeS;
  m->metaSeek = h.metaSeek;
  size_t at = h.compact() ? kMapHead2 : kMapHead;
  m->meta.assign(rec.begin() + at, rec.begin() + at + h.metaLen);
  at += h.metaLen;
  m->keysListOff = h.keysListOff;
  m->keysList.assign(rec.begin() + at, rec.begin() + at + h.keysListLen);
  at += h.keysListLen;
  if (h.compact()) {
    m->rangeLo = h.rangeLo;
    m->rangeLen = h.rangeLen;
    m->headerOff = h.headerOff;
    m->header.assign(rec.begin() + at, rec.begin() + at + h.headerLen);
    at += h.headerLen;
    m->firstOff = h.firstOff;
    m->sums = (h.flags & 1) != 0;
    m->recs.resize(h.nRecs);
    uint64_t off = h.firstOff;
    for (auto& r : m->recs) {
      const uint8_t* p = rec.data() + at;
      std::memcpy(&r.slot, p, 4);
      std::memcpy(&r.len, p + 4, 4);
      std::memcpy(&r.crc, p + 8, 4);
      r.kind = p[12];
      std::memcpy(&r.storeOff, p + 16, 8);
      r.off = off;
      off += m->extent(r);
      at += kMapRecBytes;
    }
    if (off != h.rangeLen) {
      UCACHE_WARN("compact map for %s does not add up; its handles are shown the slot layout",
                  key.key.c_str());
      return nullptr;
    }
  }
  std::lock_guard<std::mutex> g(mu);
  auto it = std::lower_bound(maps.begin(), maps.end(), seq,
                             [](const MapPlace& p, uint64_t v) { return p.seq < v; });
  if (it == maps.end() || it->seq != seq)
    return m;
  if (!it->map)
    it->map = m;
  return it->map;
}

std::shared_ptr<const tp::FileMeta> StoreMaps::fields() {
  if (mapFields)
    return mapFields;
  std::vector<uint8_t> blob;
  uint16_t keylen = 0;
  if (!tp::decodeMetaRecord(L, blob, keylen))
    return nullptr;
  auto fm = std::make_shared<tp::FileMeta>();
  if (!tp::parseTreeBlob(blob.data(), blob.size(), keylen, *fm) || !fm->error.empty())
    return nullptr;
  for (auto& b : fm->branches) { // the offsets are all a map needs
    std::vector<int64_t>().swap(b.basketSeek);
    std::vector<int32_t>().swap(b.basketBytes);
    std::vector<int64_t>().swap(b.basketEntry);
  }
  mapFields = fm;
  return fm;
}

std::shared_ptr<const ColdMap> StoreMaps::mapCovering(uint64_t pos) {
  uint64_t seq = 0;
  {
    std::lock_guard<std::mutex> g(mu);
    for (size_t k = maps.size(); k-- > 0 && !seq;)
      if (pos >= maps[k].head.metaSeek && pos < maps[k].head.metaSeek + maps[k].head.metaLen)
        seq = maps[k].seq;
  }
  return seq ? mapBySeq(seq) : nullptr;
}

uint64_t StoreMaps::zerosFrom(uint64_t pos, uint64_t n) {
  std::lock_guard<std::mutex> g(mu);
  for (const auto& m : maps)
    if (m.head.metaSeek > pos)
      n = std::min<uint64_t>(n, m.head.metaSeek - pos);
  return n;
}


bool StoreMaps::compactValidLocked(size_t k, const InUseRecord& inUse, uint64_t now) const {
  const MapHead& h = maps[k].head;
  if (!h.compact())
    return false;
  if (k + 1 == maps.size())
    return true; // the newest map is what new opens are shown
  const uint64_t lo = h.rangeLo, hi = h.rangeLo + h.rangeLen;
  return heldHere(lo, hi) ||
         inUse.inUse(store ? store->header().storeId : 0, lo, hi, now,
                     inUseSeconds);
}

InUseRecord StoreMaps::inUseSnapshot(uint64_t now) {
  std::lock_guard<std::mutex> g(inUseMu);
  if (!inUseCacheS || now >= inUseCacheS + 5) {
    InUseRecord r;
    if (InUseRecord::load(RealIO::instance(), InUseRecord::path(cacheDir, key.hashHex), r))
        inUseCache = std::move(r);
    inUseCacheS = now;
  }
  return inUseCache;
}

void StoreMaps::tablesForeign(uint64_t pos, bool& refused, uint64_t& upto) {
  refused = false;
  upto = UINT64_MAX;
  const InUseRecord rec = inUseSnapshot(wallSeconds());
  const uint64_t id = store ? store->header().storeId : 0;
  for (const auto& r : rec.ranges) {
    if (r.storeId == id || r.hi <= tables.first || r.lo >= tables.end)
      continue;
    if (pos >= r.lo && pos < r.hi)
      refused = true;
    else if (r.lo > pos)
      upto = std::min(upto, r.lo);
  }
}

std::shared_ptr<const ColdMap> StoreMaps::compactAt(uint64_t pos, bool& refused, uint64_t& upto) {
  refused = false;
  upto = UINT64_MAX;
  const uint64_t now = wallSeconds();
  const InUseRecord rec = inUseSnapshot(now);
  uint64_t seq = 0;
  {
    std::lock_guard<std::mutex> g(mu);
    for (size_t k = 0; k < maps.size() && !seq && !refused; ++k) {
      const MapHead& h = maps[k].head;
      if (!h.compact())
        continue;
      if (pos >= h.rangeLo && pos < h.rangeLo + h.rangeLen) {
        if (compactValidLocked(k, rec, now))
          seq = maps[k].seq;
        else
          refused = true;
      } else if (h.rangeLo > pos) {
        upto = std::min(upto, h.rangeLo);
      }
    }
  }
  if (seq) {
    auto m = mapBySeq(seq);
    refused = !m; // valid, and its record unreadable: no other bytes are its
    return m;
  }
  if (!refused)
    for (const auto& r : rec.ranges) {
      if (r.lo < L.virtualSize)
        continue; // map 1's, or a mixed map's tree record
      if (pos >= r.lo && pos < r.hi)
        refused = true; // handed out, of a store or a map this process does not know
      else if (r.lo > pos)
        upto = std::min(upto, r.lo);
    }
  return nullptr;
}

const char* StoreMaps::compactPlan(const std::vector<std::pair<uint32_t, uint32_t>>& real,
                                  const InUseRecord& inUse, uint64_t now, ColdMap& m,
                                  std::vector<std::pair<uint32_t, uint64_t>>& seeks) {
  if (rnt || !store || !entry)
    return "not a TTree run";
  uint64_t base = std::max(L.virtualSize, inUse.highWater);
  size_t valid = 0;
  {
    std::lock_guard<std::mutex> g(mu);
    for (size_t k = 0; k < maps.size(); ++k) {
      const MapHead& h = maps[k].head;
      if (!h.compact())
        continue;
      base = std::max(base, h.rangeLo + h.rangeLen);
      valid += compactValidLocked(k, inUse, now) ? 1 : 0;
    }
    for (const auto& [i, n] : real) {
      SlotEntry e;
      if (!storedEntry(i, e))
        continue; // stated at its real length at its slot, as a mixed map does
      if (e.kind == SlotEntry::kKept) {
        seeks.emplace_back(i, slot(i).origSeek); // the original, where the file has it
        continue;
      }
      if ((e.kind != SlotEntry::kZstd && e.kind != SlotEntry::kRaw) || e.len != n)
        continue;
      ColdMap::Rec r;
      r.slot = i;
      r.len = e.len;
      r.crc = e.crc;
      r.kind = e.kind;
      r.storeOff = e.off;
      m.recs.push_back(r);
    }
  }
  if (valid >= kMaxCompactMaps)
    return "full";
  if (m.recs.empty())
    return "nothing converted";
  { // Basket keys are 64-bit in every file ROOT writes today; a 32-bit one
    // cannot state an address past 2 GiB, where the range lies.
    SlotEntry e;
    e.slot = m.recs[0].slot;
    e.kind = m.recs[0].kind;
    e.off = m.recs[0].storeOff;
    e.len = m.recs[0].len;
    e.crc = m.recs[0].crc;
    uint8_t h[6];
    if (!store->readHead(e, h, sizeof h) || ((h[4] << 8) | h[5]) <= 1000)
      return "32-bit basket keys";
  }
  const uint64_t lo = (base + kCompactGuard + 4095) / 4096 * 4096;
  uint64_t off = 0;
  for (auto& r : m.recs) {
    r.off = off;
    seeks.emplace_back(r.slot, lo + off);
    off += r.len;
  }
  std::sort(seeks.begin(), seeks.end());
  m.rangeLo = lo;
  m.rangeLen = off;
  // Its header states its end, in the window map 1's header has.
  std::vector<uint8_t> hdr(static_cast<size_t>(std::min<uint64_t>(L.originSize, 4096)));
  if (!entry->hasRange(0, hdr.size()) ||
      !entry->readCached(0, hdr.size(), reinterpret_cast<char*>(hdr.data()), /*account=*/false))
    return "the file header is not cached";
  std::string err;
  if (!tp::headerWindowForEnd(hdr, m.end(), m.headerOff, m.header, err))
    return "the file header is unreadable";
  if (!headerWindowFits(L, m.headerOff, m.header.size()))
    return "the header cannot state the map's end";
  return nullptr;
}


bool StoreMaps::makeRNTupleMap(uint64_t now, uint64_t covered) {
  if (!entry || !store)
    return false;
  auto impossible = [&](const std::string& why) {
    std::lock_guard<std::mutex> g(mu);
    mapsImpossible = true;
    UCACHE_INFO("no compact map for %s (%s); it is read in the slot layout", key.key.c_str(),
                why.c_str());
    return false;
  };
  // The original metadata: the file's own records, which the byte cache keeps.
  CachedSource src(*entry);
  tp::RNTupleMeta rm = tp::parseRNTuple(src, static_cast<int64_t>(L.originSize), "");
  if (!rm.error.empty()) {
    UCACHE_INFO("no compact map for %s yet: its metadata is not all cached (%s)", key.key.c_str(),
                rm.error.c_str());
    return false;
  }
  // The layout's slots and relocated ranges, as the map is computed from them.
  // The ranges are the layout's own: one whose pages all repeat a page another
  // range has (RNTuple's same-page merging) has no slot of its own.
  tp::FillLayout Lc;
  Lc.slots.reserve(nSlots());
  for (uint32_t i = 0; i < nSlots(); ++i)
    Lc.slots.push_back(slot(i));
  Lc.relocated = L.relocated;
  Lc.virtualSize = L.virtualSize;
  InUseRecord inUseNow;
  InUseRecord::load(RealIO::instance(), InUseRecord::path(cacheDir, key.hashHex),
                    inUseNow);
  std::vector<uint32_t> lens(nSlots(), 0);
  std::vector<SlotEntry> ents(nSlots());
  uint64_t base = std::max(L.virtualSize, inUseNow.highWater);
  size_t valid = 0;
  {
    std::lock_guard<std::mutex> g(mu);
    for (size_t k = 0; k < maps.size(); ++k)
      if (maps[k].head.compact()) {
        base = std::max(base, maps[k].head.rangeLo + maps[k].head.rangeLen);
        valid += compactValidLocked(k, inUseNow, now) ? 1 : 0;
      }
    forEachStored([&](uint32_t i, const SlotEntry& e) {
      if (i < nSlots() && (e.kind == SlotEntry::kZstd || e.kind == SlotEntry::kRaw)) {
        lens[i] = e.len;
        ents[i] = e;
      }
    });
    if (valid >= kMaxCompactMaps) {
      mapsFullUntilS = now + InUseRecord::kHoldRefreshS;
      onMapFull();
      UCACHE_INFO("no room for another compact map of %s: %zu in use; new opens are shown the "
                  "slot layout",
                  key.key.c_str(), kMaxCompactMaps);
      return false;
    }
  }
  const uint64_t lo = (base + kCompactGuard + 4095) / 4096 * 4096;
  tp::RNTupleCompactMap cmap =
      tp::rnTupleCompactMap(rm, Lc, lo, [&lens](uint32_t i) { return i < lens.size() ? lens[i] : 0u; });
  if (!cmap.error.empty())
    return false; // nothing wholly converted yet
  auto m = std::make_shared<ColdMap>();
  m->rangeLo = lo;
  m->rangeLen = cmap.end - lo;
  m->firstOff = cmap.metaReserve;
  m->sums = true;
  m->metaSeek = lo;
  m->meta = std::move(cmap.meta);
  m->keysListOff = cmap.anchor.off; // an RNTuple map's anchor, where the keys list is a TTree's
  m->keysList = std::move(cmap.anchor.bytes);
  for (const auto& [i, at] : cmap.pieces) {
    ColdMap::Rec r;
    r.off = at - lo;
    r.slot = i;
    r.len = lens[i];
    r.crc = ents[i].crc;
    r.kind = ents[i].kind;
    r.storeOff = ents[i].off;
    m->recs.push_back(r);
  }
  // Its header states its end, in the window the slot layout's header uses.
  std::vector<uint8_t> hdr(static_cast<size_t>(std::min<uint64_t>(L.originSize, 4096)));
  std::string err;
  if (!entry->hasRange(0, hdr.size()) ||
      !entry->readCached(0, hdr.size(), reinterpret_cast<char*>(hdr.data()), /*account=*/false))
    return false;
  if (!tp::headerWindowForEnd(hdr, m->end(), m->headerOff, m->header, err))
    return impossible("the file header is unreadable");
  if (!headerWindowFits(L, m->headerOff, m->header.size()))
    return impossible("the header cannot state a map's end");
  MapHead head;
  bool busy = false;
  SlotEntry made;
  const int64_t n = store->commitBuilt(
      [this, &busy](const std::vector<SlotEntry>& es) {
        for (const auto& e : es)
          busy = busy || e.kind != SlotEntry::kMap;
        applyOthers(es);
      },
      [&](SlotRecord& r) {
        if (busy && mapTiming().quietS > 0)
          return false;
        InUseRecord inUse;
        InUseRecord::load(RealIO::instance(),
                          InUseRecord::path(cacheDir, key.hashHex), inUse);
        std::lock_guard<std::mutex> g(mu);
        if (!maps.empty() && maps.back().head.covered >= covered)
          return false;
        bool clear = inUse.highWater + kCompactGuard <= m->rangeLo;
        for (const auto& p : maps)
          clear = clear && (!p.head.compact() ||
                            p.head.rangeLo + p.head.rangeLen + kCompactGuard <= m->rangeLo);
        if (!clear)
          return false;
        head.metaLen = static_cast<uint32_t>(m->meta.size());
        head.keysListLen = static_cast<uint32_t>(m->keysList.size());
        head.madeS = now;
        head.metaSeek = m->metaSeek;
        head.keysListOff = m->keysListOff;
        head.covered = covered;
        head.rangeLo = m->rangeLo;
        head.rangeLen = m->rangeLen;
        head.headerOff = m->headerOff;
        head.headerLen = static_cast<uint32_t>(m->header.size());
        head.nRecs = static_cast<uint32_t>(m->recs.size());
        head.firstOff = m->firstOff;
        head.flags = 1;
        m->madeS = now;
        r.slot = SlotEntry::kMapBit;
        r.kind = SlotEntry::kMap;
        r.bytes = encodeMapRecord(head, m->meta, m->keysList, m.get());
        return true;
      },
      fsync, made);
  if (n == -ESTALE) {
    storeGone.store(true);
    return false;
  }
  if (n < 0) {
    UCACHE_WARN("compact map for %s not stored (%s)", key.key.c_str(),
                std::strerror(static_cast<int>(-n)));
    return false;
  }
  if (made.kind != SlotEntry::kMap)
    return false;
  m->seq = made.off;
  {
    std::lock_guard<std::mutex> g(mu);
    if (noteMapLocked(made, head)) {
      auto it = std::lower_bound(maps.begin(), maps.end(), made.off,
                                 [](const MapPlace& p, uint64_t v) { return p.seq < v; });
      if (it != maps.end() && it->seq == made.off)
        it->map = m;
    }
  }
  onMapMade(static_cast<uint64_t>(n));
  UCACHE_INFO("compact map for %s: %zu pages of %zu column ranges in [%llu, %llu) (%.1f MiB)",
              key.key.c_str(), m->recs.size(), cmap.rangesCompact,
              static_cast<unsigned long long>(m->rangeLo),
              static_cast<unsigned long long>(m->end()), m->rangeLen / 1048576.0);
  return true;
}


bool StoreMaps::sweepMapWantedLocked(uint64_t now) const {
  if (mapsOff || mapsImpossible || storeGone.load())
    return false;
  const uint64_t covered = maps.empty() ? 0 : maps.back().head.covered;
  if (convertedBytes <= covered)
    return false;
  return !lastForeignS || now >= lastForeignS + mapTiming().quietS;
}

bool StoreMaps::makeMapNow(bool atExit, bool sweep) try {
  if (!store || storeGone.load())
    return false;
  const uint64_t now = wallSeconds();
  std::vector<std::pair<uint32_t, uint32_t>> real;
  uint64_t covered = 0;
  {
    std::lock_guard<std::mutex> g(mu);
    if (sweep ? !sweepMapWantedLocked(now) : mapDueLocked(now, atExit) != 0)
      return false;
    real.assign(firstLens.begin(), firstLens.end());
  }
  std::sort(real.begin(), real.end());
  {
    // Every slot it states, decoded here in one read, outside the lock; kept
    // originals are stated at their own length, from their slots.
    std::vector<uint32_t> idx;
    idx.reserve(real.size());
    for (const auto& [i, n] : real)
      idx.push_back(i);
    if (!slots.prepareSlots(idx))
      return false;
    for (auto& [i, n] : real)
      if (!n)
        n = slot(i).origLen;
      else
        covered += n; // as convertedBytes counts: records, not kept originals
  }
  if (rnt)
    return makeRNTupleMap(now, covered);
  auto impossible = [&](const std::string& why) {
    std::lock_guard<std::mutex> g(mu);
    mapsImpossible = true;
    UCACHE_INFO("no mixed map for %s (%s); it is read in the slot layout", key.key.c_str(),
                why.c_str());
    return false;
  };
  auto f = fields();
  std::vector<uint8_t> blob;
  uint16_t keylen = 0;
  if (!f || !tp::decodeMetaRecord(L, blob, keylen))
    return impossible("the relocated tree record does not decode");
  std::string err;
  std::vector<uint8_t> meta;
  const std::function<const tp::FillSlot&(uint32_t)> slotOf =
      [this](uint32_t i) -> const tp::FillSlot& { return slot(i); };
  // A compact map when the file allows one: the converted baskets back to back
  // in a range of its own, each the exact record committed now; the baskets
  // kept as stored at their original place; the rest where map 1 has them.
  auto m = std::make_shared<ColdMap>();
  std::vector<std::pair<uint32_t, uint64_t>> seeks;
  InUseRecord inUseNow;
  InUseRecord::load(RealIO::instance(), InUseRecord::path(cacheDir, key.hashHex),
                      inUseNow);
  if (const char* why = compactPlan(real, inUseNow, now, *m, seeks)) {
    m = std::make_shared<ColdMap>(); // a mixed map instead
    seeks.clear();
    if (std::strcmp(why, "full") == 0) {
      std::lock_guard<std::mutex> g(mu);
      mapsFullUntilS = now + InUseRecord::kHoldRefreshS;
      onMapFull();
      UCACHE_INFO("no room for another compact map of %s: %zu in use; new opens are shown the "
                  "slot layout",
                  key.key.c_str(), kMaxCompactMaps);
      return false;
    }
  }
  if (!tp::stateRealLengths(blob, *f, nSlots(), slotOf, real, err) ||
      (!seeks.empty() && !tp::stateSeeks(blob, *f, nSlots(), slotOf, seeks, err)) ||
      !tp::mapMetaRecord(L, blob, tables.first, meta, err))
    return impossible(err);
  std::vector<uint8_t>().swap(blob);

  MapHead head;
  bool busy = false, full = false;
  SlotEntry made;
  const int64_t n = store->commitBuilt(
      [this, &busy](const std::vector<SlotEntry>& es) {
        for (const auto& e : es)
          busy = busy || e.kind != SlotEntry::kMap;
        applyOthers(es);
      },
      [&](SlotRecord& r) {
        // Someone converted meanwhile: not quiet after all, and the next map
        // would come too soon. Or someone made one already.
        if (busy && mapTiming().quietS > 0)
          return false;
        // Which places readers may still use: read under the store's exclusive
        // lock, which every placement holds (the store's lock, then the
        // record's). A record that cannot be read leaves only this process's
        // own knowledge.
        InUseRecord inUse;
        InUseRecord::load(RealIO::instance(),
                            InUseRecord::path(cacheDir, key.hashHex), inUse);
        std::lock_guard<std::mutex> g(mu);
        if (!maps.empty() && maps.back().head.covered >= covered)
          return false;
        // The range was chosen before this lock: still past every range
        // handed out, and every map's (another process may have made one)?
        if (m->compact()) {
          bool clear = inUse.highWater + kCompactGuard <= m->rangeLo;
          for (const auto& p : maps)
            clear = clear && (!p.head.compact() ||
                              p.head.rangeLo + p.head.rangeLen + kCompactGuard <= m->rangeLo);
          if (!clear)
            return false;
        }
        uint64_t retryS = 0;
        const uint64_t seek = freePlaceLocked(meta.size(), now, inUse, retryS);
        if (!seek) {
          full = true;
          // Nothing may free before then; if nothing ever will (the newest
          // alone leaves no room), try again when a newer map would be due.
          mapsFullUntilS = retryS != UINT64_MAX ? retryS : now + mapTiming().intervalS;
          return false;
        }
        tp::FillLayout::Window kl;
        if (!tp::patchKeySeek(meta.data(), meta.size(), seek) ||
            !tp::keysListForMap(L, seek, static_cast<uint32_t>(meta.size()), kl, err)) {
          if (err.empty())
            err = "the tree key cannot address the map's place";
          return false;
        }
        head.metaLen = static_cast<uint32_t>(meta.size());
        head.keysListLen = static_cast<uint32_t>(kl.bytes.size());
        head.madeS = now;
        head.metaSeek = seek;
        head.keysListOff = kl.off;
        head.covered = covered;
        if (m->compact()) {
          head.rangeLo = m->rangeLo;
          head.rangeLen = m->rangeLen;
          head.headerOff = m->headerOff;
          head.headerLen = static_cast<uint32_t>(m->header.size());
          head.nRecs = static_cast<uint32_t>(m->recs.size());
        }
        m->madeS = now;
        m->metaSeek = seek;
        m->meta = std::move(meta);
        m->keysListOff = kl.off;
        m->keysList = std::move(kl.bytes);
        r.slot = SlotEntry::kMapBit;
        r.kind = SlotEntry::kMap;
        r.bytes = encodeMapRecord(head, m->meta, m->keysList, m->compact() ? m.get() : nullptr);
        return true;
      },
      fsync, made);
  if (n == -ESTALE) {
    storeGone.store(true);
    return false;
  }
  if (n < 0) {
    UCACHE_WARN("mixed map for %s not stored (%s)", key.key.c_str(),
                std::strerror(static_cast<int>(-n)));
    return false;
  }
  if (made.kind != SlotEntry::kMap) {
    if (!err.empty())
      return impossible(err);
    if (full)
      onMapFull();
    if (full)
      UCACHE_INFO("no room for another mixed map of %s until an older one expires",
                  key.key.c_str());
    return false;
  }
  m->seq = made.off;
  {
    std::lock_guard<std::mutex> g(mu);
    if (noteMapLocked(made, head)) {
      auto it = std::lower_bound(maps.begin(), maps.end(), made.off,
                                 [](const MapPlace& p, uint64_t v) { return p.seq < v; });
      if (it != maps.end() && it->seq == made.off)
        it->map = m;
    }
  }
  onMapMade(static_cast<uint64_t>(n));
  if (m->compact())
    UCACHE_INFO("compact map for %s: %zu baskets in [%llu, %llu) (%.1f MiB)", key.key.c_str(),
                m->recs.size(), static_cast<unsigned long long>(m->rangeLo),
                static_cast<unsigned long long>(m->end()), m->rangeLen / 1048576.0);
  else
    UCACHE_INFO("mixed map for %s: %zu baskets at their real length (%.1f MiB)", key.key.c_str(),
                real.size(), covered / 1048576.0);
  return true;
} catch (const std::exception& e) {
  UCACHE_WARN("mixed map for %s not made (%s)", key.key.c_str(), e.what());
  return false;
}

} // namespace ucache::transpose
