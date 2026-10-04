#include "StoreSweep.h"

#include "FileEntry.h"
#include "InUse.h"
#include "Log.h"
#include "MetaFile.h"
#include "RNTupleRewrite.h"
#include "SlotStore.h"
#include "StoreLayout.h"
#include "StoreMaps.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace ucache::transpose {

namespace {

// Records are committed in blocks of about this much: one block per commit,
// the records of neighbouring slots back to back in the store.
constexpr uint64_t kBlockBytes = 64ull << 20;
// The file's head is never let go of: ROOT reads it as one block at every open.
constexpr uint64_t kKeepHead = 128 * 1024;

uint64_t steadyNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

// A file's store as the sweep sees it: every committed entry, by slot.
class SweepMaps : public StoreMaps {
 public:
  std::vector<SlotEntry> entries;
  std::vector<uint8_t> have;
  uint64_t mapBytes = 0; // written to the store by the maps made

  // Entries committed by anyone: maps noted, slots marked stored.
  void ingest(const std::vector<SlotEntry>& es, bool foreign) {
    std::vector<std::pair<const SlotEntry*, MapHead>> heads;
    for (const auto& e : es) {
      uint8_t b[kMapHead2];
      MapHead h;
      const size_t n = std::min<size_t>(kMapHead2, e.len);
      if (e.kind == SlotEntry::kMap && store && store->readHead(e, b, n) &&
          decodeMapHead(b, n, h))
        heads.emplace_back(&e, h);
    }
    std::lock_guard<std::mutex> g(mu);
    for (const auto& [e, h] : heads)
      noteMapLocked(*e, h);
    for (const auto& e : es) {
      if (e.kind == SlotEntry::kMap || e.slot >= nSlots() || have[e.slot])
        continue;
      noteSlotLocked(e, foreign);
      have[e.slot] = 1;
      entries[e.slot] = e;
    }
  }

 protected:
  bool storedEntry(uint32_t i, SlotEntry& e) const override {
    if (i >= have.size() || !have[i])
      return false;
    e = entries[i];
    return true;
  }
  void forEachStored(const std::function<void(uint32_t, const SlotEntry&)>& f) const override {
    for (uint32_t i = 0; i < have.size(); ++i)
      if (have[i])
        f(i, entries[i]);
  }
  void applyOthers(const std::vector<SlotEntry>& es) override { ingest(es, true); }
  void onMapMade(uint64_t storedBytes) override { mapBytes += storedBytes; }
};

// The converted slots' originals, let go of by the byte cache: the whole pages
// they cover, a page shared with anything else kept.
uint64_t releaseConverted(SweepMaps& sm, FileEntry& entry) {
  struct R {
    uint64_t a, b;
  };
  std::vector<R> conv;
  for (uint32_t i = 0; i < sm.nSlots(); ++i)
    if (sm.have[i] && sm.entries[i].kind != SlotEntry::kKept) {
      const FillSlot& s = sm.slot(i);
      conv.push_back({s.origSeek, s.origSeek + s.origLen});
    }
  std::sort(conv.begin(), conv.end(), [](const R& x, const R& y) { return x.a < y.a; });
  // Converted originals that touch are one run, so a page they share goes too.
  std::vector<std::pair<uint64_t, uint64_t>> runs;
  for (const auto& r : conv) {
    if (!runs.empty() && r.a <= runs.back().second)
      runs.back().second = std::max(runs.back().second, r.b);
    else
      runs.emplace_back(r.a, r.b);
  }
  std::vector<std::pair<uint64_t, uint64_t>> punch; // (offset, length)
  for (auto [a, b] : runs) {
    a = std::max(a, kKeepHead);
    if (a < b)
      punch.emplace_back(a, b - a);
  }
  return punch.empty() ? 0 : entry.releaseRanges(punch);
}

} // namespace

SweepResult sweepFile(CacheStore& cs, const Config& cfg, IOBackend& io, const UrlKey& key) {
  using O = SweepResult::Outcome;
  SweepResult res;
  auto fail = [&res](O o, std::string why) {
    res.outcome = o;
    res.note = std::move(why);
    return res;
  };
  auto cm = MetaFile::loadSummary(io, key.metaPath(cfg.cacheDir));
  if (!cm)
    return fail(O::kFailed, "no readable cache sidecar (evicted, or never cached)");
  const uint64_t size = cm->fileSize;
  if (size < 100)
    return fail(O::kNothing, "");
  auto entry = cs.open(key, size, cm->originMtime, cm->cksumKind, cm->originCksum);
  if (!entry)
    return fail(O::kFailed, "the cache entry cannot be opened");
  const std::string dir = key.objectDir(cfg.cacheDir);

  SweepMaps sm;
  sm.key = key;
  sm.cacheDir = cfg.cacheDir;
  sm.entry = entry;
  sm.inUseSeconds = cfg.inUseSeconds;
  sm.fsync = cfg.fsync != FsyncMode::kOff;
  CachedSource src(*entry);

  auto store = SlotStore::open(io, dir, key.hashHex);
  if ((store && store->header().layoutVersion > kLayoutVersion) ||
      (!store && SlotStore::newer(io, dir, key.hashHex)))
    return fail(O::kNewer, "its store was made by a newer uCache");
  // The replica an earlier uCache made beside the byte cache is not served any
  // more: the store replaces it.
  res.droppedEarlier = CacheStore::dropEarlierReplica(io, dir, key.hashHex);
  if (store && !adoptable(store->header(), size, cm->originMtime, cm->cksumKind, cm->originCksum,
                          cfg.validate)) {
    // Made by an earlier uCache, or for another version of the file.
    if (!store->dropIfCurrent())
      return fail(O::kIncomplete, "its store is being replaced");
    store.reset();
  }
  if (store && store->header().declined) {
    // Served as stored, as decided when the store was made -- unless the codec
    // list it was decided with has changed, which can change the decision.
    if (store->header().codecs == joinCodecs(cfg.recompressCodecs)) {
      StoredLayout probe;
      probe.codecs = cfg.recompressCodecs;
      bool declined = false;
      std::string why;
      computeLayout(probe, src, nullptr, size, store->header().slotFactor100, declined, why,
                    /*anyCachedProbe=*/true);
      res.codecDecline = probe.L.codecDecline;
      res.declinedCodecs = probe.L.declinedCodecs;
      return fail(O::kDeclined, probe.L.error.empty() ? "declined when it was first read"
                                                      : probe.L.error);
    }
    if (!store->dropIfCurrent())
      return fail(O::kIncomplete, "its store is being replaced");
    store.reset();
  }
  if (!store) {
    // From the cached metadata alone, with the settings of the layout readers
    // were last shown within the window, if any (their positions stay right).
    // Then the codec of branches whose setting names none comes from the
    // basket every process asks, as the layout they were shown did; with none
    // shown, from any cached one (a job need not have read that basket).
    StoredLayout lay;
    lay.codecs = cfg.recompressCodecs;
    lay.slotFactor100 = kSlotFactor100;
    InUseRecord rec;
    bool shown = false;
    if (InUseRecord::load(io, InUseRecord::path(cfg.cacheDir, key.hashHex), rec) &&
        rec.hasSettings &&
        (rec.settings.layoutVersion == kLayoutVersion ||
         rec.settings.layoutVersion == kTTreeLayoutVersion) &&
        !rec.expired(wallSeconds(), cfg.inUseSeconds)) {
      lay.codecs = splitCodecs(rec.settings.codecs);
      lay.slotFactor100 = rec.settings.slotFactor100;
      shown = true;
    }
    bool declined = false;
    std::string why;
    if (!computeLayout(lay, src, nullptr, size, lay.slotFactor100, declined, why,
                       /*anyCachedProbe=*/!shown)) {
      if (!lay.L.error.empty()) { // the file's own content: a store is not made
        res.codecDecline = lay.L.codecDecline;
        res.declinedCodecs = lay.L.declinedCodecs;
        return fail(O::kDeclined, lay.L.error);
      }
      // Neither container in a file whose every byte is cached; else perhaps
      // only not cached yet.
      if (entry->hasRange(0, size))
        return fail(why.find("not found") != std::string::npos ? O::kNothing : O::kFailed,
                    why.empty() ? "parse failed" : "parse: " + why);
      return fail(O::kIncomplete, "what decides its layout is not cached");
    }
    SlotStoreHeader want;
    want.layoutVersion = layoutVersionOf(lay.rnt);
    want.slotFactor100 = static_cast<uint16_t>(lay.slotFactor100);
    want.codecs = joinCodecs(lay.codecs);
    want.originSize = size;
    want.originMtime = cm->originMtime;
    want.cksumKind = cm->cksumKind;
    want.originCksum = cm->originCksum;
    want.container = lay.rnt ? 1 : 0;
    want.virtualSize = lay.L.virtualSize;
    want.nSlots = static_cast<uint32_t>(lay.L.slots.size());
    want.layoutHash = layoutHash(lay.L, lay.rnt);
    const std::vector<uint8_t> blob = encodeLayout(lay);
    { // everyone after us serves what this blob decodes to: make sure it does
      StoredLayout check;
      check.slotFactor100 = lay.slotFactor100;
      if (!decodeLayout(blob, check) || layoutHash(check.L, check.rnt) != want.layoutHash)
        return fail(O::kFailed, "its layout does not survive storing");
    }
    std::string err;
    store = SlotStore::openOrCreate(io, dir, key.hashHex, want, blob, res.storeCreated, err);
    if (!store)
      return fail(O::kFailed, err);
    if (!res.storeCreated) { // someone else made it first: theirs is the layout
      if (store->header().declined)
        return fail(O::kDeclined, "declined when it was first read");
      if (!adoptable(store->header(), size, cm->originMtime, cm->cksumKind, cm->originCksum,
                     cfg.validate))
        return fail(O::kIncomplete, "its store is being replaced");
    }
  }

  // The stored layout, in full, in place of anything computed above.
  sm.slotFactor100 = store->header().slotFactor100;
  if (!decodeLayout(store->layoutBlob(), sm) || sm.L.slots.size() != store->header().nSlots ||
      sm.L.virtualSize != store->header().virtualSize)
    return fail(O::kFailed, "its store has an unreadable layout");
  sm.slots.assign(std::move(sm.L.slots));
  store->releaseBlob();
  sm.codecs = splitCodecs(store->header().codecs);
  sm.store = store;
  const uint32_t n = sm.nSlots();
  sm.entries.resize(n);
  sm.have.assign(n, 0);
  if (cfg.mixedMaps) {
    if (!sm.rnt) {
      sm.tables = mapTables(sm.L);
      sm.seekLimit = mapSeekLimit(sm.L);
      sm.mapsOff = sm.tables.empty() || sm.tables.first > sm.seekLimit;
    } else {
      sm.mapsOff = false; // compact maps only: their metadata in their own range
    }
  }
  sm.ingest(store->refresh(/*wait=*/true), /*foreign=*/false);
  {
    std::lock_guard<std::mutex> g(sm.mu);
    sm.loaded = true; // what comes in from now on is another process at work
  }

  // Convert, in slot order, what is cached and not stored. (The outcome is
  // decided at the end; until then only a stop sets it.)
  res.outcome = O::kAlready;
  std::vector<SlotRecord> recs;
  uint64_t recBytes = 0;
  bool stop = false;
  auto commitBlock = [&]() {
    if (recs.empty())
      return;
    // Never written past the free-space floor: the sweep stops there, and
    // what it committed stays.
    if (recBytes > CacheStore::headroomToFloor(cfg, io)) {
      res.outcome = O::kNoSpace;
      stop = true;
      recs.clear();
      recBytes = 0;
      return;
    }
    std::vector<SlotEntry> committed;
    const int64_t rc = store->commit(
        recs, [&sm](const std::vector<SlotEntry>& es) { sm.ingest(es, true); },
        [&sm](uint32_t slot) { return sm.have[slot] != 0; }, sm.fsync, committed);
    if (rc < 0) {
      res.outcome = O::kFailed;
      res.note = rc == -ESTALE ? "its store was removed while it was swept"
                               : std::string("the store refused the records: ") +
                                     std::strerror(static_cast<int>(-rc));
      stop = true;
    } else {
      res.storedBytes += static_cast<uint64_t>(rc);
      sm.ingest(committed, /*foreign=*/false);
      for (const auto& e : committed) {
        if (e.kind == SlotEntry::kKept) {
          ++res.kept;
        } else {
          ++res.converted;
          res.outBytes += e.len;
          res.inBytes += sm.slot(e.slot).origLen;
        }
      }
    }
    recs.clear();
    recBytes = 0;
  };
  std::vector<uint8_t> orig;
  std::vector<uint32_t> badReads;
  for (uint32_t i = 0; i < n && !stop; ++i) {
    if (sm.have[i])
      continue;
    const FillSlot& s = sm.slot(i);
    if (!entry->hasRange(s.origSeek, s.origLen)) {
      ++res.uncached;
      continue;
    }
    orig.resize(s.origLen);
    if (!entry->readCached(s.origSeek, s.origLen, orig.data(), /*account=*/false)) {
      badReads.push_back(i); // rotted, or let go of meanwhile by a first pass that converted it
      continue;
    }
    const uint64_t t0 = steadyNs();
    SlotRecord r;
    r.slot = i;
    if (sm.rnt) {
      const StoredLayout::Page& pg = sm.pages[i];
      ConvertedPage p =
          convertPage(orig.data(), orig.size(), pg.nbytes, pg.hasChecksum, s.vLen - kSlotChecksumBytes);
      if (!p.error.empty()) { // never served decoded: the reader could not detect it
        ++res.undecodable;
        if (res.note.empty())
          res.note = "page at " + std::to_string(s.origSeek) + ": " + p.error;
        continue;
      }
      r.kind = p.enc.size() == p.raw.size() ? SlotEntry::kRaw : SlotEntry::kZstd;
      r.bytes = std::move(p.enc);
    } else {
      ConvertedBasket c = convertBasket(orig.data(), orig.size(), s.vLen, sm.codecs);
      if (!c.error.empty())
        c.kind = ConvertedBasket::kOriginal; // the reader gets the origin's bytes, as it would have
      if (c.kind == ConvertedBasket::kOriginal) {
        r.kind = SlotEntry::kKept; // served from the byte cache, as stored
      } else {
        if (!patchKeySeek(c.record.data(), c.record.size(), s.vSeek)) {
          ++res.undecodable;
          if (res.note.empty())
            res.note = "basket at " + std::to_string(s.origSeek) + ": its key cannot state its slot";
          continue;
        }
        r.kind = c.kind == ConvertedBasket::kZstd ? SlotEntry::kZstd : SlotEntry::kRaw;
        r.bytes = std::move(c.record);
      }
    }
    res.convertNs += steadyNs() - t0;
    recBytes += r.bytes.size();
    recs.push_back(std::move(r));
    if (recBytes >= kBlockBytes)
      commitBlock();
  }
  if (!stop)
    commitBlock();
  // A cached original that failed its checksum may have been let go of by a
  // first pass that converted it meanwhile: then it is in the store now. (The
  // read marked the page absent either way, so the store is what tells.)
  if (!badReads.empty()) {
    sm.ingest(store->refresh(/*wait=*/true), /*foreign=*/true);
    for (uint32_t i : badReads)
      if (!sm.have[i])
        ++res.unreadable;
  }
  if (!cfg.recompressKeepOriginals && res.outcome != O::kFailed)
    res.releasedBytes = releaseConverted(sm, *entry);
  if (res.outcome != O::kFailed) {
    res.mapMade = sm.makeMapNow(/*atExit=*/false, /*sweep=*/true);
    res.storedBytes += sm.mapBytes;
  }
  if (res.outcome == O::kFailed || res.outcome == O::kNoSpace)
    return res;
  if (res.converted || res.kept)
    res.outcome = O::kConverted;
  else if (res.unreadable || res.undecodable)
    res.outcome = O::kFailed;
  else if (std::any_of(sm.have.begin(), sm.have.end(), [](uint8_t h) { return h != 0; }))
    res.outcome = O::kAlready;
  else
    res.outcome = O::kNothing;
  if (res.unreadable && res.note.empty())
    res.note = std::to_string(res.unreadable) + " cached original" +
               (res.unreadable == 1 ? "" : "s") + " failed the checksum";
  return res;
}

} // namespace ucache::transpose
