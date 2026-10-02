// The in-use record: which of a file's maps were handed to readers, and when.
//
// A map is one way of showing a file to a reader (the original, the slot
// layout, a mixed map): a range of addresses and the metadata that states
// them. A reader learns a map's positions only by reading that metadata, and
// may keep using them long after -- a later open in the same process, a worker
// a position was handed to, a process forked after the read. So every hand-out
// is written down here, per file, and three things are decided from it:
//   * a map replaced by a newer one keeps its place while it was handed out
//     within the window (`in_use_seconds`), and a place never handed out frees
//     at once;
//   * a file whose maps were used within the window is not evicted
//     automatically (serving a replica does not move the byte cache's read
//     time, so the read time alone cannot tell);
//   * a file whose store is rebuilt within the window is laid out with the
//     settings it was shown with, whatever the rebuilding process's
//     `recompress_codecs`, so the positions readers hold stay right.
//
// One small text file per entry, <cacheDir>/inuse/<hh>/<hash>, outside
// objects/: removing a file's cached data (`ucache rm`, `clear`, an
// invalidation, eviction) leaves the record, which expires on its own once
// nothing in it was used within the window. It also keeps a high-water mark:
// the highest address ever handed out while the record exists.
//
//   ucache-inuse 1
//   settings <layout version> <slot factor x100> <codecs, or ->
//   high <highest address handed out>
//   range <store id, hex> <first address> <end address> <last use> <last held> <pid>
//
// "Last use" (pid 0) is the newest hand-out, or close of a process that held
// the map: the window counts from it. A line with a pid is that process's
// hold: noted when one of its handles is handed the map, refreshed while one
// holds it, removed at its last close -- so a held map stays in use whatever
// the window (0 included), and a process that ends without closing lets go
// once it stops refreshing.
//
// Written in place under an exclusive flock, never fsynced: it sits on the
// open path, and a record lost in a crash costs only the protection it gave.
// A line that does not parse is skipped.
//
// Thread-safety: the static functions may be called from any thread and any
// process; an InUseRecord value is plain data.
#pragma once

#include "IOBackend.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ucache {

struct InUseSettings {
  uint32_t layoutVersion = 0;
  uint16_t slotFactor100 = 0;
  std::string codecs;
};

struct InUseRange {
  uint64_t storeId = 0; // the store the map's addresses belong to (0: none)
  uint64_t lo = 0, hi = 0;
  uint64_t lastS = 0; // last hand-out, or close of a holder (wall clock, seconds)
  uint64_t holdS = 0; // last time process `pid` was known to hold it
  uint64_t pid = 0;   // the holder (0: a use, not a hold)
};

class InUseRecord {
 public:
  // A map held by an open handle is noted as held at least this often; it
  // counts as in use for twice this after the last such note.
  static constexpr uint64_t kHoldRefreshS = 600;

  std::vector<InUseRange> ranges;
  uint64_t highWater = 0;
  bool hasSettings = false;
  InUseSettings settings;

  static std::string path(const std::string& cacheDir, const std::string& hashHex);

  // The record at `path`; no record reads as an empty one. False only when one
  // exists and cannot be read.
  static bool load(IOBackend& io, const std::string& path, InUseRecord& out);

  // A map's range used (r.lastS) and/or held by process r.pid (r.holdS), or
  // -- `release` -- that process's hold removed and r.lastS noted: merged in
  // under the record's exclusive lock, an entry for the same store, range and
  // holder keeping the newer time, entries no longer in use dropped, the
  // high-water mark raised, and `s` stored when the record has no settings
  // yet. A record with nothing in use starts over (settings and high-water
  // mark included). False on an I/O failure (the reader is served anyway).
  static bool note(IOBackend& io, const std::string& path, const InUseRange& r,
                   const InUseSettings* s, uint64_t windowS, bool release = false);

  // The newest use of anything in the record; 0 if none.
  uint64_t lastUseS() const;

  // Is any part of [lo, hi) of store `storeId` in use at nowS: handed out or
  // released within the window, or held by an open handle?
  bool inUse(uint64_t storeId, uint64_t lo, uint64_t hi, uint64_t nowS, uint64_t windowS) const;

  // When [lo, hi) of store `storeId` stops being in use if nothing notes it
  // again: the latest end of a window or hold over the entries overlapping it;
  // 0 when none does.
  uint64_t freeAtS(uint64_t storeId, uint64_t lo, uint64_t hi, uint64_t windowS) const;

  // Is the whole record past its window at nowS (nothing in it in use)?
  bool expired(uint64_t nowS, uint64_t windowS) const;

  // Remove the records under <cacheDir>/inuse that have expired at nowS.
  // Returns how many were removed.
  static int sweep(IOBackend& io, const std::string& cacheDir, uint64_t nowS, uint64_t windowS);

  std::string serialize() const;
  static InUseRecord parse(const std::string& text);
};

} // namespace ucache
