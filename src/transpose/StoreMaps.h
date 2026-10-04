// The maps of a file's slot store. A map is one way of showing the file to a
// reader: map 1 is the slot layout itself (StoreLayout.h); a MIXED map states
// the converted baskets at their real length at their slots; a COMPACT map
// states them back to back in a range of its own past map 1's. This is their
// record in the store, where a new one may go, which ones may still be read,
// and how one is made. Shared by the plugin, which makes maps as files are
// read, and the command line, which makes one after converting what the byte
// cache holds.
//
// Thread-safety: StoreMaps' state is guarded by `mu` unless said otherwise; a
// map is made by one thread at a time (the plugin holds its own lock around
// makeMapNow). ColdMap is immutable once made.
#pragma once

#include "FileEntry.h"
#include "FillLayout.h"
#include "InUse.h"
#include "SlotStore.h"
#include "StoreLayout.h"
#include "TreeMeta.h"
#include "UrlKey.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ucache::transpose {

// ---- mixed maps (TTree; transpose/FillLayout.h)
//
// A map is worth making when this much of what the store holds is not in the
// newest one: a twentieth of everything converted, and after the first map
// 8 MiB too (on a smaller file, as much again as the newest map states).
constexpr uint64_t kMapMinBytes = 8ull << 20;
constexpr uint64_t kMapShare = 20;
// A compact map's range lies past map 1's slots and past every range handed
// out, behind a guard longer than any read a reader merges (RNTuple's
// fMaxKeySize is 1 GiB), so no read spans two maps. At most this many are in
// use at once; then the next open is shown map 1.
constexpr uint64_t kCompactGuard = 1ull << 30;
constexpr size_t kMaxCompactMaps = 4;

// Only a few maps fit the space the layout keeps for them (about four for a
// NanoAOD file), and a map another may still use keeps its place while a
// handle holds it and for `in_use_seconds` after its last use (InUse.h). So
// maps are made rarely: not
// while anyone is still converting the file (nothing committed for this long),
// and after the first, at most one an hour.
// The last handle of a file closed: its map is made a moment after (2 s),
// unless the file is opened again meanwhile (a reader that works through a
// file in several tasks closes and reopens it between them).
struct MapTiming {
  uint64_t quietS = 120, intervalS = 3600, closeDelayMs = 2000;
};
const MapTiming& mapTiming();

uint64_t wallSeconds(); // seconds, wall clock

// A map a handle is given at open: its own keys-list window, and its own
// relocated tree record (transpose/FillLayout.h).
// * A MIXED map states the converted baskets at their real length, at their
//   slots: every other byte a handle reads is map 1's.
// * A COMPACT map states them back to back in a range of its own, past map 1's
//   slots: [rangeLo, rangeLo + rangeLen), each address translated to the exact
//   record it names (`recs`). It has its own header window too (fEND = its
//   end); the baskets not converted when it was made stay at their slots, and
//   those kept as stored at their original place.
// Immutable once made.
struct ColdMap {
  uint64_t seq = 0;              // its entry's place in the store: the order maps were made in
  uint64_t madeS = 0;            // when it was made (wall clock, seconds)
  uint64_t metaSeek = 0;         // its tree record, as served
  std::vector<uint8_t> meta;
  uint64_t keysListOff = 0;      // its keys-list window
  std::vector<uint8_t> keysList;
  uint64_t rangeLo = 0, rangeLen = 0; // a compact map's range (0: a mixed map)
  // Where its records start, from rangeLo: an RNTuple map's own page list and
  // footer come first (served from `meta`, metaSeek = rangeLo); a TTree map's
  // tree record lies in the tables area instead (0 here).
  uint64_t firstOff = 0;
  bool sums = false; // an RNTuple map: each record followed by its checksum
  uint64_t headerOff = 0;             // a compact map's header window
  std::vector<uint8_t> header;
  struct Rec {
    uint64_t off = 0;      // from rangeLo
    uint32_t slot = 0;
    uint32_t len = 0, crc = 0; // the record it names (store offset `storeOff`)
    uint8_t kind = 0;
    uint64_t storeOff = 0;
  };
  std::vector<Rec> recs; // ascending, back to back from 0 to rangeLen
  bool compact() const { return rangeLen != 0; }
  uint64_t end() const { return rangeLo + rangeLen; }
  bool metaInRange() const { return compact() && metaSeek == rangeLo; }
  uint64_t extent(const Rec& r) const { return r.len + (sums ? 8u : 0u); }
};

// A map's record in the store: this head, then its tree record as served,
// then its keys-list window -- and, for a compact map, its header window and
// the records its range names (kMapRecBytes each: slot, length, CRC, kind,
// store offset), in address order. Made once, never rebuilt: another build
// could compress the tree record into other bytes. A build that knows only
// mixed maps passes over a compact one (another magic), and shows map 1.
constexpr char kMapMagic[8] = {'U', 'C', 'S', 'M', 'A', 'P', '0', '1'};
constexpr char kMapMagic2[8] = {'U', 'C', 'S', 'M', 'A', 'P', '0', '2'};
constexpr size_t kMapHead = 64, kMapHead2 = 96, kMapRecBytes = 24;
struct MapHead {
  uint32_t metaLen = 0, keysListLen = 0;
  uint64_t madeS = 0;    // wall clock, seconds
  uint64_t metaSeek = 0; // where its tree record is served
  uint64_t keysListOff = 0;
  uint64_t covered = 0;  // real bytes of the slots it states at their real length
  // A compact map:
  uint64_t rangeLo = 0, rangeLen = 0;
  uint64_t headerOff = 0;
  uint32_t headerLen = 0, nRecs = 0;
  uint64_t firstOff = 0; // where its records start, from rangeLo
  uint8_t flags = 0;     // 1: each record followed by its 8-byte checksum
  bool compact() const { return rangeLen != 0; }
  uint64_t recordLen() const {
    return (compact() ? kMapHead2 : kMapHead) + uint64_t(metaLen) + keysListLen + headerLen +
           uint64_t(nRecs) * kMapRecBytes;
  }
};
std::vector<uint8_t> encodeMapRecord(const MapHead& h, const std::vector<uint8_t>& meta,
                                     const std::vector<uint8_t>& keysList,
                                     const ColdMap* compact = nullptr);
bool decodeMapHead(const uint8_t* p, size_t n, MapHead& h);

// The file's bytes the byte cache holds, for parsing its metadata again.
struct CachedSource : Source {
  FileEntry& e;
  explicit CachedSource(FileEntry& f) : e(f) {}
  bool has(uint64_t off, uint64_t n) override { return e.hasRange(off, n); }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    return e.readCached(off, n, dst, /*account=*/false);
  }
};

// A file's slot store as a book of maps: what is committed, which maps exist,
// and making the next one. The layout (StoredLayout) and store are set up by
// the caller.
class StoreMaps : public StoredLayout {
 public:
  virtual ~StoreMaps() = default;

  UrlKey key;
  std::string cacheDir;
  std::shared_ptr<SlotStore> store;
  std::shared_ptr<FileEntry> entry;
  std::atomic<bool> storeGone{false}; // dropped or replaced: nothing more is committed
  uint64_t inUseSeconds = 86400;      // the window (InUse.h)
  bool fsync = true;                  // commits are flushed to the disk

  std::mutex mu; // guards what follows (and a subclass's own state)
  int handles = 0; // the plugin's open handles on the file (0 for the command line)
  struct MapPlace {
    uint64_t seq = 0;   // its entry's offset in the store: the order maps were made in
    MapHead head;       // where it lies, when it was made, what it covers
    SlotEntry e;
    std::shared_ptr<const ColdMap> map; // read in full once asked for
  };
  bool mapsOff = true;          // no map for this file (set at set-up)
  bool mapsImpossible = false;  // a map was tried and cannot be made for this file
  uint64_t mapsFullUntilS = 0;  // no place for a map before then
  uint64_t seekLimit = 0;       // the largest place a map may lie at
  MapTables tables;
  std::vector<MapPlace> maps;   // every map the store holds, by seq
  uint64_t convertedBytes = 0;  // real bytes of the committed slots (records, not kept originals)
  // Per committed slot, its FIRST record's length (0: kept as stored), for
  // good -- what every mixed map states it at. Never forgotten with the slot.
  std::unordered_map<uint32_t, uint32_t> firstLens;
  // Wall seconds of the last slot committed after this run was set up: by
  // this process, and by another one (0: none).
  uint64_t lastOwnS = 0, lastForeignS = 0;
  bool loaded = false;          // the entries found at set-up are in
  std::shared_ptr<const FileMeta> mapFields; // where the tree record's fields are (under the map lock)

  // A committed slot entry, counted once. `foreign`: another process's.
  void noteSlotLocked(const SlotEntry& e, bool foreign);
  // A map entry, verified against the layout: false when it is not one.
  bool noteMapLocked(const SlotEntry& e, const MapHead& h);
  // Seconds until a map may be made (0: now), or -1 when none is wanted.
  // `atExit`: this process is done with the file whatever its handles say.
  int64_t mapDueLocked(uint64_t now, bool atExit) const;
  // An explicit sweep's rule: a map whenever the newest one does not hold
  // what the store does, unless another process converted the file within
  // the quiet period (the map is left to it) or no map is to be made.
  bool sweepMapWantedLocked(uint64_t now) const;
  // Where a map record of `len` bytes can go at `now` (0: nowhere, and
  // `retryS` = when a place may free): clear of every map a reader may still
  // hold -- this store's, and those of a store this one replaced (the in-use
  // record keeps where they were handed out). Before them all, else after them
  // all, else between: a newer map states more and is a little longer, so it
  // fits where an older one lay only together with the room next to it.
  uint64_t freePlaceLocked(uint64_t len, uint64_t now, const InUseRecord& inUse,
                           uint64_t& retryS) const;
  // The compact map to make of the slots `real` states: its range, header and
  // records in `m`, the addresses its tree record states in `seeks`. Null when
  // one is made; else why not ("full": as many compact maps are in use as may
  // be -- the caller shows map 1 instead; anything else: a mixed map is made).
  const char* compactPlan(const std::vector<std::pair<uint32_t, uint32_t>>& real,
                          const InUseRecord& inUse, uint64_t now, ColdMap& m,
                          std::vector<std::pair<uint32_t, uint64_t>>& seeks);
  // Is the compact map at maps[k] still to be served: the newest, held by a
  // handle of this process, or in use in the in-use record?
  bool compactValidLocked(size_t k, const InUseRecord& inUse, uint64_t now) const;
  // makeMapNow for an RNTuple file: a compact map of every column range whose
  // pages are all converted, or nothing.
  bool makeRNTupleMap(uint64_t now, uint64_t covered);
  // What a read at `pos`, past map 1's slots, reads -- by address, whichever
  // map the handle was shown: the compact map whose range holds it, when it
  // may still be read; else `refused`, when pos lies in a range readers may
  // hold that is no longer valid (a map replaced and out of use, or a range of
  // a store this one replaced); else neither: unassigned, zeros up to `upto`.
  std::shared_ptr<const ColdMap> compactAt(uint64_t pos, bool& refused, uint64_t& upto);
  // A read at `pos` in the tables area, in the tree record of a map of another
  // store -- one this store replaced, whose readers may still hold positions
  // there: `refused` (no bytes there are this store's to give). Else `upto` =
  // where the next such record starts (UINT64_MAX: none).
  void tablesForeign(uint64_t pos, bool& refused, uint64_t& upto);
  // The in-use record as last read, again at most every few seconds.
  InUseRecord inUseSnapshot(uint64_t now);
  std::mutex inUseMu;
  InUseRecord inUseCache;
  uint64_t inUseCacheS = 0;
  std::atomic<uint64_t> refusedLogS{0};
  // The map whose entry lies at `seq`, read once. Null when it cannot be read.
  std::shared_ptr<const ColdMap> mapBySeq(uint64_t seq);
  // The tables area as any handle reads it: the newest map lying at `pos`,
  // or null (zeros); and how many bytes from `pos` (at most `n`) are zeros.
  // A reader handed a tree record's place by another process reads the same
  // bytes there, as long as the map is live.
  std::shared_ptr<const ColdMap> mapCovering(uint64_t pos);
  uint64_t zerosFrom(uint64_t pos, uint64_t n);
  std::shared_ptr<const FileMeta> fields(); // under the map lock
  // Make a map if one is due (`sweep`: by the sweep's rule), from what the
  // store holds now. The caller makes one map of a file at a time. Returns
  // whether one was made.
  bool makeMapNow(bool atExit, bool sweep = false);

 protected:
  // What only the caller knows. All but heldHere are called with mu held.
  // A handle of this process holds the map whose range is [lo, hi).
  virtual bool heldHere(uint64_t lo, uint64_t hi) const {
    (void)lo;
    (void)hi;
    return false;
  }
  // Slot i's committed entry (anyone's), if it has one.
  virtual bool storedEntry(uint32_t i, SlotEntry& e) const = 0;
  // Every committed entry known.
  virtual void forEachStored(const std::function<void(uint32_t, const SlotEntry&)>& f) const = 0;
  // Entries another process committed, met while storing a map (mu not held).
  virtual void applyOthers(const std::vector<SlotEntry>& es) = 0;
  virtual void onMapFull() {}
  virtual void onMapMade(uint64_t storedBytes) { (void)storedBytes; }
};

} // namespace ucache::transpose
