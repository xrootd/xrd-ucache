// The sweep of one file: what the byte cache holds of it, converted into the
// file's slot store -- the same store, the same layout and the same records a
// first pass with `recompress = on` makes as it reads -- and the next map made
// from it. Run by `ucache recompress`, one file per worker.
//
// Nothing is read from the origin. The layout is computed from the cached
// metadata when the file has no store yet (a file whose metadata is not cached
// is left for later), and every slot whose original is cached and not yet in
// the store is converted, in slot order, and committed in blocks under the
// store's lock (a slot another process commits meanwhile is not written
// twice). With `recompress_keep_originals = off` the byte cache then lets go of
// every converted original, first passes' included; the file's first 128 KiB
// stay.
//
// Thread-safety: sweepFile may run on several threads at once for different
// files. Two sweeps of the same file at once are safe (the store's lock) but
// waste the work of one.
#pragma once

#include "CacheStore.h"
#include "Config.h"
#include "IOBackend.h"
#include "UrlKey.h"

#include <cstdint>
#include <string>

namespace ucache::transpose {

struct SweepResult {
  enum class Outcome {
    kConverted,  // records were committed
    kAlready,    // every cached slot was in the store already
    kNothing,    // no slot's original is cached
    kIncomplete, // the metadata the layout needs is not cached
    kDeclined,   // the file's layout declines it (`note`; `declinedCodecs` for a codec)
    kNewer,      // a newer uCache's store: left alone, the file served as stored
    kNoSpace,    // stopped at the free-space floor; what was committed stays
    kFailed,     // `note` says why
  };
  Outcome outcome = Outcome::kFailed;
  std::string note;
  bool codecDecline = false;  // kDeclined for the codec the file is stored in
  std::string declinedCodecs; // ... the codecs `recompress_codecs` does not name
  bool storeCreated = false;
  bool mapMade = false;
  bool droppedEarlier = false; // a replica an earlier release made was removed
  uint64_t converted = 0;     // slots this sweep committed converted ...
  uint64_t kept = 0;          // ... and kept as stored (no record)
  uint64_t inBytes = 0;       // the originals of the converted slots
  uint64_t outBytes = 0;      // their records
  uint64_t convertNs = 0;     // time spent converting them
  uint64_t storedBytes = 0;   // written to the store: records and maps
  uint64_t releasedBytes = 0; // byte-cache bytes let go of
  uint64_t uncached = 0;      // slots neither in the store nor cached (not read yet)
  uint64_t unreadable = 0;    // cached originals that failed their checksum
  uint64_t undecodable = 0;   // originals that do not decode (the file's own pages)
};

// Sweep the file `key` names. `cs` is the cache it is in; `cfg` gives the
// settings (codecs for a new store, keep originals, maps, the window).
SweepResult sweepFile(CacheStore& cs, const Config& cfg, IOBackend& io, const UrlKey& key);

} // namespace ucache::transpose
