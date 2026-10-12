// The max_read_fraction rule. A process that asks for more than the limit of
// a ROOT file's data gains nothing from caching it -- it would fill the cache
// with the file -- so it reads that file straight from the origin: what it
// fetches is not kept, while bytes already cached are still served.
//
// Decided once per file per process, early, and not checked again. The share
// is the bytes the branches (RNTuple: columns) read so far hold in the file
// over the bytes of every branch -- the part of each entry's data the reader
// asks for, from the file's own metadata (ReadMap). Read directly as soon as
// it exceeds the limit; cached for good as soon as a request of two or more
// branches adds none (the reader has come back to branches it read before).
// Until then the file is cached as usual.
//
// The map is parsed only when a request needs the origin, so a warm pass
// never pays for it. The file's own records (header, keys list, tree record,
// RNTuple envelopes) are cached as usual whatever the decision. Files smaller
// than kMinBytes, files that are not TTree or RNTuple, and a limit of 100 have
// no rule: they are cached as before.
//
// Thread-safety: all methods are thread-safe. A rule is shared by every handle
// of the file in the process and lives for the process.
#pragma once

#include "ReadMap.h"
#include "TreeMeta.h"
#include "XrdClTimeout.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ucache {

struct OriginSource;
struct HandleState;
class FileEntry;

// check_origin_integrity: the file map of a file's tree, for the key check of byte-cache misses
// (keySeeksFor). Immutable once made.
struct KeyMap {
  std::vector<uint64_t> seeks; // where each basket starts, sorted
  std::vector<int32_t> bytes;  // beside seeks: its length, 0 for none known
  // Non-empty: the map shows the copy it was read from bad (why); `server` is that copy's server
  // ("" when not known), and `badSpans` the parts of the copy the damaged tree concerns (its
  // record, and its baskets when the record could be read; transpose::treeSpans): only reads
  // of those, answered by that copy, are refused. A map whose tree's record failed its own check
  // holds no basket; one that places a basket past the copy's end keeps its baskets.
  std::string bad;
  std::string server;
  std::vector<std::pair<uint64_t, uint64_t>> badSpans;
};

class ReadRule : public std::enable_shared_from_this<ReadRule> {
 public:
  enum State : uint8_t { kUndecided = 0, kCache = 1, kDirect = 2, kNoRule = 3 };
  static constexpr uint64_t kMinBytes = 64ull << 20;

  // The rule for a file, created on first use, or null when none applies.
  static std::shared_ptr<ReadRule> forFile(const std::string& key, uint64_t originSize);

  State state() const { return static_cast<State>(state_.load(std::memory_order_acquire)); }
  bool direct() const { return state() == kDirect; }

  // A request that needs the origin, as ranges of the ORIGINAL file. Parses
  // the file's map on the first call (reading through `src`: byte cache first,
  // origin otherwise, the metadata kept), then takes the request into the
  // share (ReadMap::step). Concurrent calls wait for each other.
  State observe(const std::vector<std::pair<uint64_t, uint64_t>>& ranges, OriginSource& src);

  // After a direct decision: does [off, off + len) read the file's data? The
  // file's own records are fetched and kept as usual.
  bool readsData(uint64_t off, uint64_t len) const;

  ReadRule(std::string key, uint64_t size, int limit)
      : key_(std::move(key)), size_(size), limit_(limit) {}
  ~ReadRule();

 private:
  void holdMap();  // counts this parse against the bound on maps held (caller holds mu_)
  void dropMap();  // the bound's victim: parsed again if it is needed again
  std::string key_;
  uint64_t size_;
  int limit_;
  std::atomic<uint8_t> state_{kUndecided};
  std::mutex mu_;              // one parse or step at a time
  bool parsed_ = false;        // guarded by mu_
  std::unique_ptr<transpose::ReadMap> map_; // until the decision; guarded by mu_
  std::vector<uint32_t> seen_; // branches (columns) requests touched so far; guarded by mu_
  // Written once, before state_ becomes kDirect (release), read after (acquire).
  std::vector<std::pair<uint64_t, uint64_t>> runs_;
};

// Whether this process decided to read `key` directly: such a file is never
// given a new slot store.
bool readRuleDirect(const std::string& key);

// check_origin_integrity: where every basket of the file's tree starts
// (transpose::basketSeeks), 8 bytes a basket, for the key-position check of
// byte-cache misses. Built once per file entry per process: from the rule's
// own parse when the file has a rule, otherwise by one parse through
// OriginSource. Empty for a file whose tree does not parse (RNTuple, not
// ROOT): it is not checked. Null while the table is not built: while another
// handle's parse runs (no handle waits for another's), and after a parse that
// a failed origin read stopped; the caller's miss then goes unchecked, and a
// later miss asks again. At most one parse of a file runs at a time. At most
// 32 tables are held; the oldest is parsed again if it is needed again.
// Thread-safe. The parse BLOCKS the thread that issues the miss (the reader's
// own, or an XrdCl worker when a reader issues a read from inside a
// completion); each of its synchronous reads carries `timeout`, the miss's own
// (0: the client's default). After a parse that a failed read stopped, none
// runs for a second. It keeps the pages of the records it reads in the byte
// cache, and their other bytes are not checked. `fm`: the tree the caller
// parsed already, which builds the table instead (ReadRule::observe).
//
// The table is the file map (KeyMap): each basket's position and
// the length the tree's record gives it. A parse that shows the copy it read
// from bad makes a map that says why (KeyMap::bad), with that copy's server
// and the parts of the copy the damaged tree concerns: the tree's record
// failed its own check (ROOT fails to read the tree too), or it places a
// basket past the copy's end. A parse this
// walk does not support makes no claim (an empty map), as before. A bad map
// is parsed again when `current` (the server the handle reads from now) is
// another (the handle moved), at most once a second. `fmServer`: with `fm`,
// the server its records were read from.
std::shared_ptr<const KeyMap>
keySeeksFor(const std::shared_ptr<HandleState>& st, const std::shared_ptr<FileEntry>& entry,
            XrdTimeout timeout, const transpose::FileMeta* fm = nullptr,
            const std::string& fmServer = std::string(), const std::string* current = nullptr);
// check_origin_integrity: a piece of bytes fetched from the origin (an answer's buffer, an element
// of a vector read), at `off` in the file.
using FetchPiece = transpose::BytePiece;
// The verdict on fetched `pieces` that `server` answered, with the map `m`: each basket's key
// (position, length) and the block checksum of each basket's data (transpose::checkFetched: one
// decompression of each basket wholly in the bytes, CPU), and, where `m` shows that copy bad, the
// parts of the bytes its damaged tree concerns. A map that shows another copy bad makes no claim
// on them. Pieces that follow each other without a gap (in offset order) are judged as one run, so
// a key header or a basket a cut between two pieces divides is judged whole (such a run is copied
// into one buffer first). Pieces that overlap are judged apart. Thread-safe; reads only the pieces.
transpose::FetchVerdict
fetchVerdict(const KeyMap& m, const std::string& server, std::vector<FetchPiece> pieces,
             const std::vector<std::pair<uint64_t, uint64_t>>* reach = nullptr);
// The same, made in parallel on the decode pool (FetchCheck.h): `done` is called once, with the
// verdict, on the pool thread that finished it (at once, on this thread, when there is nothing
// to judge). `m` is kept alive until then; the pieces' bytes are the caller's to keep. `reach`:
// decode only the baskets these ranges reach (transpose::FetchJudge), for bytes of which nothing
// else is kept (read-ahead's padding).
void fetchVerdictAsync(const std::shared_ptr<const KeyMap>& m, const std::string& server,
                       std::vector<FetchPiece> pieces,
                       std::shared_ptr<const std::vector<std::pair<uint64_t, uint64_t>>> reach,
                       std::function<void(transpose::FetchVerdict)> done);
// Whether fetchVerdict would decode anything, or join pieces: work kept off the client's callback
// threads. A few bytes read per basket.
bool fetchNeedsWork(const KeyMap& m, const std::string& server, std::vector<FetchPiece> pieces);
// The map of `entry` when a parse has built it, without parsing (null: none yet). While a parse
// of the file runs and has built none yet, `then` (when given and set) is kept and posted to the
// executor once that parse ends, whatever it found (`parked`; *then is taken). For the answer of a
// request a handle made while another handle's parse ran: it is checked once the map is known
// rather than not at all.
struct KeySeeksBuilt {
  std::shared_ptr<const KeyMap> map;
  bool parked = false;
};
KeySeeksBuilt keySeeksBuilt(const std::shared_ptr<FileEntry>& entry, std::function<void()>* then);
// The file map of the parse `fm` of a copy of `fileSize` bytes read from `server`.
KeyMap keyMapOf(const transpose::FileMeta& fm, uint64_t fileSize, const std::string& server);

// fork(): a child decides afresh for every file (a decision costs at most a
// few early fills, and the bytes read are the same). Before is called from the
// plugin's fork prepare handler (it finishes any first use of the registry
// under way), after from its child handler (single-threaded, no I/O, no
// thread).
void readRuleBeforeFork();
void readRuleAfterForkChild();

} // namespace ucache
