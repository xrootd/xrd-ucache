#include "ColdRun.h"

#include "CacheStore.h"
#include "Executor.h"
#include "FillLayout.h"
#include "RNTupleRewrite.h"
#include "Log.h"
#include "OriginInFlight.h"
#include "OriginSource.h"
#ifdef UCACHE_HAVE_PREFETCH
#include "Prefetch.h"
#include "UsableCpus.h"
#endif
#include "PluginSupport.h"
#include "InUse.h"
#include "ReaderWait.h"
#include "ReadRounding.h"
#include "ReadRule.h"
#include "SlotStore.h"
#include "SlotTable.h"
#include "StoreLayout.h"
#include "Transposer.h"
#include "UCacheFile.h"
#include "XrdClTimeout.h"
#include "vendor/xxh3.h"

#include <XrdCl/XrdClFile.hh>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <new>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace ucache {

namespace tp = transpose;
using XrdCl::AnyObject;
using XrdCl::ChunkList;
using XrdCl::HostList;
using XrdCl::ResponseHandler;
using XrdCl::XRootDStatus;

namespace {

// The layout constants and versions (StoreLayout.h).
using tp::kLayoutVersion;
using tp::kOldestDecision;
using tp::kOldestServedLayout;
using tp::kSlotFactor100;
using tp::kTTreeLayoutVersion;
using tp::layoutVersionOf;
// The maps (StoreMaps.h).
using tp::CachedSource;
using tp::decodeMapHead;
using tp::encodeMapRecord;
using tp::kCompactGuard;
using tp::kMapHead;
using tp::kMapHead2;
using tp::kMaxCompactMaps;
using tp::MapHead;
using tp::mapTiming;
using tp::wallSeconds;

// Converted records wait in memory until this many bytes, the periodic
// checkpoint, or the process's last close of the file, then go to the store in
// one commit.
constexpr uint64_t kCommitBytes = 8ull << 20;

// A reader's fill is fetched as several vector reads of about this many bytes
// each, so conversion starts when the first of them lands rather than when the
// whole fill has. Smaller fills go as one request, exactly as they would
// without the cache. A part never carries more items than a vector read may.
constexpr uint64_t kPartBytes = 4ull << 20;
constexpr size_t kPartItems = 512;

// Conversions run here, never on the executor that serves hits: they are
// CPU-bound and as many as the reader has threads, and a hit must not queue
// behind them. Leaked like the executor (threads end with the process).
Executor& convertPool() {
  static Executor* pool = new Executor(std::max(2u, usableCpus()));
  return *pool;
}

// Commits posted and not yet run. A process that exits normally waits for them
// (bounded); a hard _exit() skips this, and loses at most what the periodic
// checkpoint had not yet committed.
std::mutex g_cmtMu;
std::condition_variable g_cmtCv;
int g_cmtPending = 0;


// UCACHE_TEST_COMMIT_FAIL_N=N (inert unless set): the process's first N
// slot-store commits fail with EIO, as a write the cache disk refused, so a
// gate can show what a failed commit costs: nothing a reader sees, and the
// records are converted again by a later pass.
bool commitFaultFire() {
  static std::atomic<int> budget{[] {
    const char* v = std::getenv("UCACHE_TEST_COMMIT_FAIL_N");
    return v ? std::atoi(v) : 0;
  }()};
  if (budget.load(std::memory_order_relaxed) <= 0)
    return false;
  return budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}


// The store's kind for a converted basket, and back.
uint8_t entryKind(uint8_t basketKind) {
  return basketKind == tp::ConvertedBasket::kZstd  ? SlotEntry::kZstd
         : basketKind == tp::ConvertedBasket::kRaw ? SlotEntry::kRaw
                                                   : SlotEntry::kKept;
}
uint8_t basketKind(uint8_t entryKind) {
  return entryKind == SlotEntry::kZstd  ? tp::ConvertedBasket::kZstd
         : entryKind == SlotEntry::kRaw ? tp::ConvertedBasket::kRaw
                                        : tp::ConvertedBasket::kOriginal;
}

} // namespace

class ColdFill : public tp::StoreMaps, public std::enable_shared_from_this<ColdFill> {
 public:
  enum : uint8_t { kAbsent = 0, kFetching = 1, kReady = 2 };
  // What is known of a slot someone has touched. Guarded by ColdFill::mu.
  struct Info {
    uint8_t kind = 0;       // tp::ConvertedBasket::Kind, valid once kReady
    bool inStore = false;   // `e` is a committed entry (anyone's)
    bool persist = false;   // `mem` is to be committed
    bool fromCache = false; // converted from the byte cache's copy of the original
    bool transient = false; // `mem` is served, not to be committed (counted in transientBytes)
    // Staged by a request that has not finished: neither pending nor transient
    // yet, counted in no budget (the request holds the record anyway). When the
    // request finishes, its records join `pending` together, in address order,
    // so a later reader finds what one request read side by side in the store.
    bool held = false;
    uint64_t token = 0;    // which staging of the slot this is (it may be staged again)
    uint64_t stagedUs = 0; // when, for a checkpoint that commits a stalled request's records
    SlotEntry e;
    std::shared_ptr<const std::vector<uint8_t>> mem; // the record, until committed
  };
  using Rec = std::shared_ptr<const std::vector<uint8_t>>;
  // Told when a slot it waited on is ready (with the record, when the one who
  // converted it has it in hand) or was given up.
  using Waiter = std::function<void(bool, Rec)>;

  // The layout and its settings (rnt, L, pages, metaOrigin, codecs,
  // slotFactor100, the slot table): StoreLayout.h; key, cacheDir, store,
  // entry, storeGone, mu, handles and the maps: StoreMaps.h.
  bool keepOriginals = false;
  // max_read_fraction: null when no rule applies. Once it says direct, what
  // this process converts is served and never kept, and nothing it fetches
  // goes to the byte cache but the file's own records.
  std::shared_ptr<ReadRule> rule;
  bool direct() const { return rule && rule->direct(); }
  std::unique_ptr<std::atomic<uint8_t>[]> state; // per slot: kAbsent / kFetching / kReady
  // Slot i's first committed record's length (a kept original's own length;
  // 0 when nothing is committed): what a mixed map states it at.
  uint32_t committedLenLocked(uint32_t i, const tp::FillSlot& s) const {
    auto it = firstLens.find(i);
    return it == firstLens.end() ? 0 : it->second ? it->second : s.origLen;
  }
  uint32_t committedLen(uint32_t i, const tp::FillSlot& s) {
    std::lock_guard<std::mutex> g(mu);
    return committedLenLocked(i, s);
  }

  std::unordered_map<uint32_t, Info> info;
  std::unordered_map<uint32_t, std::vector<Waiter>> waiters;
  std::vector<uint32_t> pending;
  uint64_t pendingBytes = 0;
  uint64_t tokenSeq = 0;
  // Slots held for requests (slot, token, staged at), oldest first; entries no
  // longer held are dropped when met.
  std::deque<std::tuple<uint32_t, uint64_t, uint64_t>> heldList;
  // Records served but not to be committed (the store is gone, or the
  // process's pending cap is reached): kept for the requests that need them,
  // oldest dropped first past kTransientBytes.
  std::deque<uint32_t> transient;
  uint64_t transientBytes = 0;
  // Original ranges of relocated baskets, merged and sorted: bytes the reader
  // never reads in this layout, so never worth keeping. Built before the first
  // fetch from the origin (ensureRelocatedOrig), on a thread that may read.
  std::shared_ptr<const std::vector<std::pair<uint64_t, uint64_t>>> relocatedOrig;
  bool relocatedTried = false;
  void ensureRelocatedOrig();
  // Is page `pg` wholly inside baskets that are committed and converted -- so
  // the byte cache's copy of it serves nothing? Caller holds mu.
  bool pageDeadLocked(uint64_t pg, uint64_t ps);

  std::mutex commitMu; // one commit of this file at a time in this process
  std::atomic<bool> commitQueued{false};
  std::atomic<bool> commitAgain{false}; // asked for while one was running

  std::atomic<uint64_t> nZstd{0}, nRaw{0}, nOrig{0}, inBytes{0}, outBytes{0}, convertUs{0},
      wireBytes{0};

  // A snapshot of where slot i's bytes are, if it is ready.
  struct Where {
    uint8_t kind = 0;
    bool inStore = false;
    SlotEntry e;
    std::shared_ptr<const std::vector<uint8_t>> mem;
  };
  bool where(uint32_t i, Where& w) {
    std::lock_guard<std::mutex> g(mu);
    if (state[i].load(std::memory_order_acquire) != kReady)
      return false;
    auto it = info.find(i);
    if (it == info.end())
      return false;
    w.kind = it->second.kind;
    w.inStore = it->second.inStore;
    w.e = it->second.e;
    w.mem = it->second.mem;
    return true;
  }

  // A committed kept-as-stored slot is served from the byte cache's copy of
  // its original; if that copy is gone (evicted), the slot must be fetched.
  bool keptOriginalGone(uint32_t i) {
    if (rnt || state[i].load(std::memory_order_acquire) != kReady)
      return false;
    std::lock_guard<std::mutex> g(mu);
    auto it = info.find(i);
    if (it == info.end() || it->second.kind != tp::ConvertedBasket::kOriginal || it->second.mem)
      return false;
    const tp::FillSlot& fs = slot(i); // decoded: the request found it
    if (entry->hasRange(fs.origSeek, fs.origLen))
      return false;
    forgetLocked(i);
    return true;
  }

  // Slot i turned out unreadable (a record failing its CRC, an original gone):
  // absent again, so the next request fetches and converts it anew.
  void forget(uint32_t i) {
    std::lock_guard<std::mutex> g(mu);
    forgetLocked(i);
  }

  // Entries committed by anyone: the slots they name are ready from the store.
  // `foreign`: by another process (or not known to be this one's).
  void apply(const std::vector<SlotEntry>& es, bool foreign = true) {
    std::vector<Waiter> wake;
    // A map's head is read before taking the lock: no I/O under it.
    std::vector<std::pair<const SlotEntry*, MapHead>> heads;
    for (const auto& e : es) {
      uint8_t b[kMapHead2];
      MapHead h;
      const size_t n = std::min<size_t>(kMapHead2, e.len);
      if (e.kind == SlotEntry::kMap && store && store->readHead(e, b, n) &&
          decodeMapHead(b, n, h))
        heads.emplace_back(&e, h); // an unreadable one is not a map anyone can be given
    }
    {
      std::lock_guard<std::mutex> g(mu);
      for (const auto& [e, h] : heads)
        noteMapLocked(*e, h);
      for (const auto& e : es) {
        if (e.kind == SlotEntry::kMap || e.slot >= nSlots())
          continue;
        Info& s = info[e.slot];
        if (!s.inStore)
          noteSlotLocked(e, foreign);
        s.inStore = true;
        s.e = e;
        s.kind = basketKind(e.kind);
        // Committed by someone: the store's copy serves, ours is not kept.
        releaseMemLocked(s);
        s.persist = false;
        if (state[e.slot].load(std::memory_order_acquire) != kReady) {
          state[e.slot].store(kReady, std::memory_order_release);
          takeWaitersLocked(e.slot, wake);
        }
      }
    }
    for (auto& w : wake)
      w(true, nullptr);
  }

  // Pick up what other processes committed since we last looked -- unless a
  // commit is under way, in which case this is skipped, not waited for.
  void sync(bool wait = false) {
    if (store) {
      auto es = store->refresh(wait);
      if (!es.empty())
        apply(es);
    }
  }

  // A converted record for slot i: served from memory until it is committed.
  // `keepIt` false (a file this process reads directly): served to whoever asked, never kept.
  // `group`: the caller is a request that will hand the record back with
  // appendGroup() when it finishes; set to the staging's token (0: not held).
  void stage(uint32_t i, uint8_t kind, Rec rec, bool fromCache, bool keepIt = true,
             uint64_t* group = nullptr);
  // A finished request's records (slot, token): to be committed together, in
  // slot order; a slot staged again since, or committed meanwhile, is skipped.
  void appendGroup(std::vector<std::pair<uint32_t, uint64_t>> group);
  // Records held for requests that have not finished for `ageUs`: committed as
  // their own group (a stalled request must not keep them from the store).
  void appendHeldOlderThan(uint64_t ageUs);

  // A fetch that claimed slot i failed: give the claim back and tell whoever
  // waited on it, so they can fetch it themselves.
  void abandon(uint32_t i) {
    std::vector<Waiter> wake;
    {
      std::lock_guard<std::mutex> g(mu);
      uint8_t expect = kFetching;
      state[i].compare_exchange_strong(expect, kAbsent, std::memory_order_acq_rel);
      takeWaitersLocked(i, wake);
    }
    for (auto& w : wake)
      w(false, nullptr);
  }

  // Claim slot i for fetching (true), or register `cb` to hear when whoever
  // holds it is done (false). A slot found ready returns false with `ready`
  // set and registers nothing. Both looks are compare-and-swaps: a slot
  // released between them is claimed by exactly one caller.
  bool claimOrWait(uint32_t i, Waiter cb, bool& ready) {
    ready = false;
    uint8_t expect = kAbsent;
    if (state[i].compare_exchange_strong(expect, kFetching, std::memory_order_acq_rel))
      return true;
    std::lock_guard<std::mutex> g(mu);
    expect = kAbsent;
    if (state[i].compare_exchange_strong(expect, kFetching, std::memory_order_acq_rel))
      return true;
    if (expect == kReady) {
      ready = true;
      return false;
    }
    waiters[i].push_back(std::move(cb));
    return false;
  }

  // Original bytes worth keeping in the byte cache, of the page-aligned range
  // [rs, re): every page not wholly inside a relocated basket.
  std::vector<std::pair<uint64_t, uint64_t>> storableRuns(uint64_t rs, uint64_t re, uint64_t ps);

  void queueCommit();
  void commit();
  ~ColdFill();

  // ---- maps (StoreMaps.h), made here in the background (queueMap).
  // When this process last noted a range of this file in its in-use record
  // (InUse.h): first address -> (hand-out, hold), wall seconds.
  std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> inUseNoted;
  std::atomic<bool> mapQueued{false};
  std::mutex mapMu; // one map made at a time in this process; guards mapFields
  void queueMap(uint64_t delayMs);
  // Commit what this process holds, then make a map if one is due. Returns
  // whether one was made.
  bool makeMap(bool atExit);

 protected:
  bool heldHere(uint64_t lo, uint64_t hi) const override;
  bool storedEntry(uint32_t i, SlotEntry& e) const override;
  void forEachStored(const std::function<void(uint32_t, const SlotEntry&)>& f) const override;
  void applyOthers(const std::vector<SlotEntry>& es) override { apply(es); }
  void onMapFull() override;
  void onMapMade(uint64_t storedBytes) override;

 private:
  // Send a staged record to `pending`, or -- when not allowed (the store is
  // gone, or the process holds its cap of records waiting) -- to the transient
  // budget. False when it is not kept at all.
  bool placeLocked(uint32_t i, Info& s, const Rec& rec, uint64_t b, bool inCache, bool persist,
                   uint64_t& lost, bool& kick);
  void appendGroupLocked(std::vector<std::pair<uint32_t, uint64_t>>& group, uint64_t& lost,
                         bool& kick);
  void forgetLocked(uint32_t i) {
    uint8_t expect = kReady;
    state[i].compare_exchange_strong(expect, kAbsent, std::memory_order_acq_rel);
    if (auto it = info.find(i); it != info.end()) {
      releaseMemLocked(it->second);
      info.erase(it);
    }
  }
  // Drop this process's copy of a record, and its share of the transient budget.
  void releaseMemLocked(Info& s);
  void takeWaitersLocked(uint32_t i, std::vector<Waiter>& out) {
    auto it = waiters.find(i);
    if (it != waiters.end()) {
      out.swap(it->second);
      waiters.erase(it);
    }
  }
  void dropRecords(const std::vector<SlotRecord>& recs, const std::string& why);
  // Records collected for a commit that will not happen: this process's copy
  // goes (a request that still needs one holds its own), counted as not kept.
  void releaseCollected(const std::vector<SlotRecord>& recs);
};

namespace {

std::mutex g_regMu;
// Leaked: executor tasks may detach during teardown.
std::unordered_map<std::string, std::shared_ptr<ColdFill>>& registry() {
  static auto* r = new std::unordered_map<std::string, std::shared_ptr<ColdFill>>();
  return *r;
}

// Which layout this process has shown each file in, for the process's whole
// life: a reader may hold offsets from any open it made, and must never be
// shown a different address space later. For a slot layout, also what it was
// computed with: a store removed meanwhile (evicted, `ucache rm`) is rebuilt
// with the SAME slot factor and codecs, whatever the settings say now, or the
// rebuilt layout could not be the one the reader holds offsets into. Leaked.
struct Shown {
  ShownLayout layout = ShownLayout::kNone;
  uint64_t hash = 0;
  uint32_t slotFactor100 = 0; // a slot layout's
  std::string codecs;         // a slot layout's, joined
  // The mixed maps this process showed the file in, oldest first: where each
  // one's tree record lies, and the map while a handle still holds it. A
  // position a reader learned from one is served from it by every handle --
  // also after the store was replaced -- and fails once no handle holds it.
  struct Map {
    uint64_t seek = 0, len = 0;   // its tree record
    uint64_t lo = 0, hi = 0;      // what identifies it in the in-use record
    std::weak_ptr<const ColdMap> map;
  };
  std::vector<Map> maps;
};
using ShownMap = std::unordered_map<std::string, Shown>;
std::mutex g_shownMu;
// Made at first use (constant-initialized: see Executor.cc), under g_shownMu by
// every caller; replaced only in a forked child (below).
ShownMap* g_shown = nullptr;
ShownMap& shownMap() {
  if (!g_shown)
    g_shown = new ShownMap();
  return *g_shown;
}
// Odd while a change to the map is under way (under g_shownMu): a child forked
// at that instant cannot trust the map it inherited, and takes the copy made
// just before the fork instead (coldForkPrepare). No lock is held ACROSS fork:
// XrdCl's own prepare handler runs after ours and waits for its callback
// threads, and one of those may be setting up a handle, which needs this lock.
std::atomic<uint64_t> g_shownVersion{0};
ShownMap* g_shownCopy = nullptr; // the copy prepare made, for the child
void noteShownMap(const std::string& key, const std::shared_ptr<const ColdMap>& m) {
  std::lock_guard<std::mutex> g(g_shownMu);
  g_shownVersion.fetch_add(1, std::memory_order_acq_rel); // odd: changing
  auto& v = shownMap()[key].maps;
  bool have = false;
  for (const auto& x : v)
    have = have || x.map.lock() == m;
  if (!have)
    v.push_back({m->metaSeek, m->meta.size(), m->compact() ? m->rangeLo : m->metaSeek,
                 m->compact() ? m->end() : m->metaSeek + m->meta.size(), m});
  g_shownVersion.fetch_add(1, std::memory_order_acq_rel); // even: whole again
}
// The map this process showed `key` in whose tree record lies at `pos`, while a
// handle holds it; `gone` when one lay there and none does any more.
std::shared_ptr<const ColdMap> shownMapAt(const std::string& key, uint64_t pos, bool& gone) {
  gone = false;
  std::lock_guard<std::mutex> g(g_shownMu);
  auto it = shownMap().find(key);
  if (it == shownMap().end())
    return nullptr;
  const auto& v = it->second.maps;
  for (size_t k = v.size(); k-- > 0;)
    if (pos >= v[k].seek && pos < v[k].seek + v[k].len) {
      if (auto m = v[k].map.lock())
        return m;
      gone = true;
    }
  return nullptr;
}
// Does a handle of this process hold a map of `key` whose identifying range
// (a compact map's addresses, a mixed map's tree record) overlaps [a, b)?
bool shownMapHeld(const std::string& key, uint64_t a, uint64_t b) {
  std::lock_guard<std::mutex> g(g_shownMu);
  auto it = shownMap().find(key);
  if (it == shownMap().end())
    return false;
  for (const auto& m : it->second.maps)
    if (m.lo < b && a < m.hi && !m.map.expired())
      return true;
  return false;
}
// The identifying ranges of the maps this process showed `key` in -- and a
// TTree compact map's tree record, in the tables area apart from its range;
// `held`: only those a handle still holds.
std::vector<std::pair<uint64_t, uint64_t>> shownMapRanges(const std::string& key, bool held) {
  std::vector<std::pair<uint64_t, uint64_t>> out;
  std::lock_guard<std::mutex> g(g_shownMu);
  auto it = shownMap().find(key);
  if (it != shownMap().end())
    for (const auto& m : it->second.maps)
      if (!held || !m.map.expired()) {
        out.emplace_back(m.lo, m.hi);
        if (m.seek != m.lo)
          out.emplace_back(m.seek, m.seek + m.len);
      }
  return out;
}
// The parameters of the slot layout this process showed `key` in, if any.
bool shownSlotParams(const std::string& key, uint32_t& slotFactor100, std::string& codecs) {
  std::lock_guard<std::mutex> g(g_shownMu);
  auto it = shownMap().find(key);
  if (it == shownMap().end() || it->second.layout != ShownLayout::kSlot ||
      it->second.slotFactor100 == 0)
    return false;
  slotFactor100 = it->second.slotFactor100;
  codecs = it->second.codecs;
  return true;
}

// The parser's byte source at setup (shared with the read rule).
using SetupSource = OriginSource;

using tp::decodeLayout;
using tp::encodeLayout;
using tp::joinCodecs;
using tp::layoutRaw;
using tp::layoutHash;
using tp::splitCodecs;

// Parse the file and compute its layout with the given parameters into cf
// (StoreLayout.h), reading the file's head as one block for an RNTuple file.
bool computeLayout(ColdFill& cf, SetupSource& src, uint64_t size, uint32_t k100, bool& declined) {
  std::string why;
  const bool ok = tp::computeLayout(
      cf, src, [&src] { return src.fetchHead(); }, size, k100, declined, why);
  if (!ok && !why.empty())
    UCACHE_DEBUG("slot run declined for %s: %s", cf.key.key.c_str(), why.c_str());
  return ok;
}

// A first pass the file's own layout declined (L.error): counted, and said
// once per process at WARN -- recompression that quietly does nothing for a
// file is the one outcome a user cannot see otherwise. Every other such file
// is logged at INFO. `recorded` = this process made the store that remembers
// the decision, so each file counts once. A file with no TTree or RNTuple at
// all is not a candidate, and says nothing.
void noteDeclined(const std::shared_ptr<HandleState>& st, const UrlKey& key,
                  const tp::FillLayout& L, bool recorded) {
  if (L.error.empty())
    return;
  static std::atomic<bool> warned{false};
  if (recorded && st->store)
    st->store->stats().coldReplicaSkipped.fetch_add(1, std::memory_order_relaxed);
  const std::string note = tp::declineNote(L);
  if (recorded && !warned.exchange(true))
    UCACHE_WARN("slot run declined for %s: %s; the file is served as stored (`ucache stats` "
                "counts such files as cold_replica_skipped; later ones are logged at INFO)",
                key.key.c_str(), note.c_str());
  else
    UCACHE_INFO("slot run declined for %s: %s; the file is served as stored", key.key.c_str(),
                note.c_str());
}

// The replica tier's adoption rule (StoreLayout.h), under this process's
// `validate` setting.
bool adoptable(const SlotStoreHeader& h, uint64_t size, uint64_t originMtime, uint8_t cksumKind,
               uint32_t originCksum) {
  return tp::adoptable(h, size, originMtime, cksumKind, originCksum, globalConfig().validate);
}

// Set up the slot run of a file.
//
// An existing store is served as it is: its layout was computed once, by
// whoever created it, and is never recomputed. With kCreate and no store, the
// layout is computed and a store made (a file whose layout declines gets a
// store marked DECLINED, so the next open need not parse it to find that
// out). kMatch is for a process that has already shown this file's slot
// layout: only a layout with that hash will do.
std::shared_ptr<ColdFill> build(const std::shared_ptr<HandleState>& st,
                                const std::shared_ptr<FileEntry>& entry, const UrlKey& key,
                                uint64_t originMtime, uint8_t cksumKind, uint32_t originCksum,
                                AttachMode mode, uint64_t matchHash) {
  const Config& cfg = globalConfig();
  const uint64_t size = entry->fileSize();
  if (size < 100)
    return nullptr;
  const uint64_t tSetup = nowUs();
  IOBackend& io = RealIO::instance();
  const std::string dir = key.objectDir(cfg.cacheDir);

  auto cf = std::make_shared<ColdFill>();
  cf->key = key;
  cf->cacheDir = cfg.cacheDir;
  cf->entry = entry;
  cf->keepOriginals = cfg.recompressKeepOriginals;
  cf->inUseSeconds = cfg.inUseSeconds;
  cf->fsync = cfg.fsync != FsyncMode::kOff;
  cf->rule = ReadRule::forFile(key.key, size);

  auto store = SlotStore::open(io, dir, key.hashHex);
  // Several releases may share a cache: a newer uCache's store is left alone
  // and the file served as stored, never replaced (it would replace ours back).
  if (store && store->header().layoutVersion > kLayoutVersion)
    return nullptr;
  if (store && !adoptable(store->header(), size, originMtime, cksumKind, originCksum)) {
    const SlotStoreHeader& oh = store->header();
    if (oh.layoutVersion < (oh.declined ? kOldestDecision : kOldestServedLayout))
      UCACHE_INFO("slot store for %s was made by an earlier uCache; decided again",
                  key.key.c_str());
    else
      UCACHE_INFO("slot store for %s was made for another version of the file; replaced",
                  key.key.c_str());
    store->dropIfCurrent(); // busy: the stale store is found again below, and declined
    store.reset();
  }
  if (store && store->header().declined) {
    // Served as stored, as decided when the store was made -- unless the codec
    // list it was decided with has changed, which can change the decision.
    if (mode != AttachMode::kCreate || store->header().codecs == joinCodecs(cfg.recompressCodecs))
      return nullptr;
    store->dropIfCurrent(); // busy: found again below, and served as stored this time
    store.reset();
  }
  if (store && mode == AttachMode::kMatch && store->header().layoutHash != matchHash)
    return nullptr; // not the layout this process has shown: never mix two
  bool created = false;
  if (!store) {
    if (mode == AttachMode::kExisting || SlotStore::newer(io, dir, key.hashHex))
      return nullptr;
    SetupSource src;
    src.st = st;
    src.entry = entry;
    cf->codecs = cfg.recompressCodecs;
    cf->slotFactor100 = kSlotFactor100;
    // Rebuilding the layout this process already showed (its store is gone):
    // with what it was computed with, not with today's settings. Likewise a
    // layout another process showed readers within `in_use_seconds` (its
    // in-use record says how), so the positions they hold stay right.
    InUseRecord rec;
    if (std::string shownCodecs; mode == AttachMode::kMatch &&
                                 shownSlotParams(key.key, cf->slotFactor100, shownCodecs))
      cf->codecs = splitCodecs(shownCodecs);
    else if (InUseRecord::load(io, InUseRecord::path(cfg.cacheDir, key.hashHex), rec) &&
             rec.hasSettings &&
             (rec.settings.layoutVersion == kLayoutVersion ||
              rec.settings.layoutVersion == kTTreeLayoutVersion) &&
             !rec.expired(wallSeconds(), cfg.inUseSeconds)) {
      cf->codecs = splitCodecs(rec.settings.codecs);
      cf->slotFactor100 = rec.settings.slotFactor100;
    }
    bool declined = false;
    const bool ok = computeLayout(*cf, src, size, cf->slotFactor100, declined);
    SlotStoreHeader want;
    want.layoutVersion = layoutVersionOf(cf->rnt);
    want.slotFactor100 = static_cast<uint16_t>(cf->slotFactor100);
    want.codecs = joinCodecs(cf->codecs);
    want.originSize = size;
    want.originMtime = originMtime;
    want.cksumKind = cksumKind;
    want.originCksum = originCksum;
    std::vector<uint8_t> blob;
    if (!ok) {
      if (!declined)
        return nullptr;
      if (mode != AttachMode::kCreate) {
        noteDeclined(st, key, cf->L, /*recorded=*/false);
        return nullptr;
      }
      want.declined = true;
    } else {
      want.container = cf->rnt ? 1 : 0;
      want.virtualSize = cf->L.virtualSize;
      want.nSlots = static_cast<uint32_t>(cf->L.slots.size());
      want.layoutHash = layoutHash(cf->L, cf->rnt);
      if (mode == AttachMode::kMatch && want.layoutHash != matchHash)
        return nullptr; // this build cannot reproduce what was shown
      blob = encodeLayout(*cf);
      // Everyone after us serves what this blob decodes to: make sure it does.
      ColdFill check;
      check.slotFactor100 = cf->slotFactor100;
      if (!decodeLayout(blob, check) || layoutHash(check.L, check.rnt) != want.layoutHash) {
        UCACHE_WARN("slot run declined for %s: its layout does not survive storing",
                    key.key.c_str());
        return nullptr;
      }
    }
    std::string err;
    store = SlotStore::openOrCreate(io, dir, key.hashHex, want, blob, created, err);
    if (!store) {
      UCACHE_INFO("slot run declined for %s: %s", key.key.c_str(), err.c_str());
      return nullptr;
    }
    if (store->header().declined) {
      if (created || want.declined) // the decision is this process's: say why
        noteDeclined(st, key, cf->L, created);
      return nullptr;
    }
    if (!created) { // someone else made it first: theirs is the layout
      if (!adoptable(store->header(), size, originMtime, cksumKind, originCksum) ||
          (mode == AttachMode::kMatch && store->header().layoutHash != matchHash))
        return nullptr;
    }
  }
  if (created && cf->rnt) {
    cf->slots.assign(std::move(cf->L.slots)); // RNTuple: every slot, held
  } else {
    // The stored layout, in place of anything computed above -- for a TTree
    // file also when this process just made the store: what the run holds is
    // the index of the slot table, each branch's entries decoded from the store
    // when a read first touches it.
    const bool rnt = cf->rnt;
    cf->L = tp::FillLayout();
    cf->slotFactor100 = store->header().slotFactor100;
    cf->metaOrigin.clear();
    cf->pages.clear();
    tp::SlotTable::Source src = [store](std::vector<uint8_t>& raw) {
      std::vector<uint8_t> blob;
      return store->readBlob(blob) && layoutRaw(blob, raw);
    };
    const bool ok = decodeLayout(store->layoutBlob(), *cf, rnt ? nullptr : std::move(src));
    if (ok && cf->rnt)
      cf->slots.assign(std::move(cf->L.slots));
    if (!ok || cf->nSlots() != store->header().nSlots ||
        cf->L.virtualSize != store->header().virtualSize) {
      UCACHE_WARN("slot store for %s has an unreadable layout; the file is served as stored",
                  key.key.c_str());
      return nullptr;
    }
    cf->codecs = splitCodecs(store->header().codecs);
  }
  store->releaseBlob(); // read again from the file if the run needs more of it
  cf->store = store;
  cf->state.reset(new std::atomic<uint8_t>[cf->nSlots()]());
  if (cfg.mixedMaps) {
    if (!cf->rnt) {
      cf->tables = tp::mapTables(cf->L);
      cf->seekLimit = tp::mapSeekLimit(cf->L);
      cf->mapsOff = cf->tables.empty() || cf->tables.first > cf->seekLimit;
    } else {
      cf->mapsOff = false; // compact maps only: their metadata in their own range
    }
  }
  // Every record anyone has committed so far -- without waiting, so a commit
  // held up in another process never holds up an open (what is missed now is
  // read at the next request, or converted again and deduplicated).
  cf->sync();
  {
    std::lock_guard<std::mutex> g(cf->mu);
    cf->loaded = true; // what comes in from now on is activity
  }
  if (created && st->store)
    st->store->stats().coldReplicaFiles.fetch_add(1, std::memory_order_relaxed);
  char factor[32] = "";
  if (!cf->rnt)
    std::snprintf(factor, sizeof factor, ", slot factor %u.%02u", cf->slotFactor100 / 100,
                  cf->slotFactor100 % 100);
  UCACHE_INFO("slot run for %s: %zu %s of %zu %s in slots (%s%s), virtual %llu bytes, "
              "set up in %.1f ms",
              key.key.c_str(), static_cast<size_t>(cf->nSlots()), cf->rnt ? "pages" : "baskets",
              cf->L.relocated.size(), cf->rnt ? "column ranges" : "branches",
              created ? "new store" : "existing store", factor,
              static_cast<unsigned long long>(cf->L.virtualSize), (nowUs() - tSetup) / 1e3);
  return cf;
}

// ------------------------------------------------------------------ serving

struct ColdRequest;
void serveRequest(const std::shared_ptr<ColdRequest>& req);

struct ColdRequest : std::enable_shared_from_this<ColdRequest> {
  std::shared_ptr<HandleState> st;
  std::shared_ptr<FileEntry> entry;
  std::shared_ptr<ColdFill> cf;
  ChunkList chunks;
  bool isVRead = false;
  ResponseHandler* user = nullptr;
  int attempt = 0;
  bool mayPark = true; // may wait once for read-ahead already on the wire
  uint64_t t0 = 0;

  // Filled in by classify(), consumed when every fetch it started is done.
  struct SlotPiece {
    uint32_t slot;
    uint64_t from, len; // within the slot
    char* dest;
    // RNTuple: served by the conversion from the page it just decoded, so
    // finish() does not decode the record again. Written before the
    // conversion's done(), read in finish() after the last one.
    bool served = false;
    // Read at a compact map's address: the record there starts at `seekAs`
    // (its fSeekKey, as served), and must be the one the map names.
    uint64_t seekAs = 0;
    uint32_t expectLen = 0, expectCrc = 0;
  };
  std::vector<SlotPiece> slotPieces;
  struct OrigPiece {
    uint64_t off, len; // original bytes the byte cache did not have
    char* dest;
  };
  std::vector<OrigPiece> origPieces;
  std::vector<uint32_t> claimed; // slots this request fetches
  // Slots whose records reach this reader from the origin, not from the cache:
  // fetched by this request, or by another one it waited on. Their bytes are
  // the fill; everything else in a slot is the replica tier.
  std::vector<uint32_t> fetched;
  // The records this request copies from, held from the moment they were
  // converted (or found ready) until it has copied them: the file's own copy
  // may be dropped meanwhile -- a commit refused at the free-space floor, the
  // transient budget -- and a request must not lose what it already has.
  std::unordered_map<uint32_t, ColdFill::Where> held; // guarded by emu
  std::vector<uint32_t> converted; // claimed slots this request staged; guarded by emu
  // ... and the stagings held for it (slot, token): handed back together at
  // finish(), on every path, so they are committed side by side. Guarded by emu.
  std::vector<std::pair<uint32_t, uint64_t>> staged;
  void hold(uint32_t i, ColdFill::Rec r) {
    ColdFill::Where w;
    w.mem = std::move(r);
    std::lock_guard<std::mutex> g(emu);
    held[i] = std::move(w);
  }
  // Slot i is ready now: keep where its bytes are -- the record in memory, or
  // the store entry (whose file stays readable), or the kept original.
  void holdReady(uint32_t i) {
    ColdFill::Where w;
    if (!cf->where(i, w))
      return;
    std::lock_guard<std::mutex> g(emu);
    held.emplace(i, std::move(w)); // a record already held is at least as good
  }

  std::atomic<int> outstanding{1}; // the issuing stage holds one
  std::atomic<bool> failed{false};
  std::atomic<bool> retry{false}; // a slot someone else was fetching fell through
  std::atomic<bool> refused{false}; // read in an invalid map's range: asking again cannot help
  std::mutex emu;
  XRootDStatus err;

  void fail(const XRootDStatus& s) {
    std::lock_guard<std::mutex> g(emu);
    if (!failed.exchange(true))
      err = s;
  }
  // The last hold released finishes the request -- on the executor, whatever
  // thread let go (an XrdCl callback thread must not do disk reads).
  void done() {
    if (outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      auto self = shared_from_this();
      Executor::instance().post([self] { self->finish(); });
    }
  }
  void finish();
  // The stored records this request's pieces need, read in as few reads as
  // the store allows (neighbours together), into `mine`'s copies in memory;
  // a record that fails its check is left for copySlot, which drops it.
  void readStoredTogether(std::unordered_map<uint32_t, ColdFill::Where>& mine);
  // Serve the same chunks again from the start (a slot fell through).
  void again(bool mayParkAgain);
};

// Bytes [from, from+len) of an RNTuple slot: the decoded page, then the decoded
// page's checksum.
void servePage(const uint8_t* page, uint32_t pageLen, uint64_t from, uint64_t len, char* dest) {
  uint8_t sum[tp::kSlotChecksumBytes];
  if (from + len > pageLen)
    tp::sealDecodedPage(page, pageLen, sum);
  if (from < pageLen) {
    const uint64_t n = std::min<uint64_t>(len, pageLen - from);
    std::memcpy(dest, page + from, n);
  }
  for (uint64_t p = std::max<uint64_t>(from, pageLen); p < from + len; ++p)
    dest[p - from] = static_cast<char>(sum[p - pageLen]);
}

// Copy bytes [from, from+len) of slot i -- its record, then zeros -- to dest.
// False when the slot's bytes are not to be had after all: the slot is then
// absent again, and serving the request again fetches it -- unless `fatal`
// is set, and the request fails.
bool copySlot(ColdFill& cf, uint32_t i, uint64_t from, uint64_t len, char* dest,
              uint64_t& recBytes, const ColdFill::Where* held, bool mapped, bool& fatal, uint64_t seekAs = 0, uint32_t expectLen = 0,
              uint32_t expectCrc = 0) {
  recBytes = 0;
  ColdFill::Where w;
  if (held) {
    w = *held; // what the request took when the slot was ready: nothing else is consulted
  } else if (!cf.where(i, w)) {
    return false;
  }
  const tp::FillSlot& fs = cf.slot(i);
  if (from + len > fs.vLen)
    return false;
  std::shared_ptr<const std::vector<uint8_t>> rec = w.mem;
  if (!rec && w.kind == tp::ConvertedBasket::kOriginal && !cf.rnt) {
    // Kept as stored: the original record, from the byte cache, with its
    // fSeekKey pointing at the slot.
    auto buf = std::make_shared<std::vector<uint8_t>>(fs.origLen);
    if (!cf.entry->hasRange(fs.origSeek, fs.origLen) ||
        !cf.entry->readCached(fs.origSeek, fs.origLen, buf->data(), /*account=*/false) ||
        !tp::patchKeySeek(buf->data(), buf->size(), fs.vSeek)) {
      cf.forget(i);
      return false;
    }
    if (auto cs = globalStore()) { // not counted by readCached (account=false)
      cs->stats().keptReads.fetch_add(1, std::memory_order_relaxed);
      cs->stats().keptReadBytes.fetch_add(fs.origLen, std::memory_order_relaxed);
    }
    rec = buf;
  } else if (!rec) {
    auto buf = std::make_shared<std::vector<uint8_t>>();
    const uint64_t t0 = nowUs();
    const bool ok = w.inStore && cf.store->readRecord(w.e, *buf);
    if (auto cs = globalStore()) {
      if (w.inStore)
        cs->stats().slotReadUs.add(nowUs() - t0);
      if (!ok)
        cs->stats().slotCrcFailures.fetch_add(1, std::memory_order_relaxed);
    }
    if (!ok) {
      UCACHE_WARN("slot record for %s (slot %u) failed its check; converting it again",
                  cf.key.key.c_str(), i);
      cf.forget(i);
      return false;
    }
    if (auto cs = globalStore()) { // the replica tier's disk reads
      auto& stats = cs->stats();
      stats.replicaReads.fetch_add(1, std::memory_order_relaxed);
      stats.replicaReadBytes.fetch_add(buf->size(), std::memory_order_relaxed);
      stats.replicaReadSize.add(buf->size());
    }
    rec = buf;
  }
  if (seekAs) {
    // At a compact map's address: exactly the record the map names, with its
    // fSeekKey stating the address read, and nothing after it. A record
    // converted again since (this one failed its check) serves only if it is
    // the same record; otherwise the read fails rather than serve another.
    const uint64_t recLen = rec->size();
    const bool same =
        recLen == expectLen &&
        (w.inStore && !w.mem ? w.e.crc == expectCrc
                             : SlotStore::recordCrc(cf.store->header().storeId, i, rec->data(),
                                                    recLen) == expectCrc);
    const uint64_t extent = recLen + (cf.rnt ? tp::kSlotChecksumBytes : 0);
    if (!same || from + len > extent) {
      UCACHE_ERROR("slot %u of %s: a compact map names a record of %u bytes; the slot's is %llu "
                   "bytes, another record",
                   i, cf.key.key.c_str(), expectLen, static_cast<unsigned long long>(recLen));
      fatal = true;
      return false;
    }
    if (cf.rnt) { // the page as stored (ROOT decodes it), then its checksum
      if (from < recLen)
        std::memcpy(dest, rec->data() + from, std::min<uint64_t>(len, recLen - from));
      if (from + len > recLen) {
        uint8_t sum[tp::kSlotChecksumBytes];
        tp::sealDecodedPage(rec->data(), recLen, sum); // XXH3-64 of the bytes, as the format seals
        for (uint64_t q = std::max<uint64_t>(from, recLen); q < from + len; ++q)
          dest[q - from] = static_cast<char>(sum[q - recLen]);
      }
      recBytes = len;
      return true;
    }
    std::memcpy(dest, rec->data() + from, len);
    uint8_t head[64];
    const size_t hn = static_cast<size_t>(std::min<uint64_t>(recLen, sizeof head));
    if (from < hn) {
      std::memcpy(head, rec->data(), hn);
      if (!tp::patchKeySeek(head, hn, seekAs)) {
        UCACHE_ERROR("slot %u of %s: its basket key cannot state the address %llu", i,
                     cf.key.key.c_str(), static_cast<unsigned long long>(seekAs));
        fatal = true;
        return false;
      }
      const uint64_t b = std::min<uint64_t>(from + len, hn);
      std::memcpy(dest, head + from, b - from);
    }
    recBytes = len;
    return true;
  }
  if (cf.rnt) {
    // The store keeps the block; the reader is served the page decoded, then
    // the decoded page's checksum: the slot's length.
    const uint32_t pageLen = fs.vLen - tp::kSlotChecksumBytes;
    std::vector<uint8_t> raw;
    const uint8_t* page = rec->data();
    if (rec->size() != pageLen) {
      if (auto cs = globalStore())
        cs->stats().slotPagesDecoded.fetch_add(1, std::memory_order_relaxed);
      raw = tp::decompressFrames(rec->data(), rec->size(), pageLen);
      if (raw.size() != pageLen) {
        cf.forget(i);
        return false;
      }
      page = raw.data();
    }
    servePage(page, pageLen, from, len, dest);
    recBytes = len;
    return true;
  }
  const uint64_t recLen = rec->size();
  // A mixed map states a committed slot at its record's length: a reader
  // given one that stops short of the slot's end must be served that record.
  // By construction it is (a slot is committed once per store); if not, the
  // read fails rather than hand the reader a basket cut short.
  if (const uint32_t c = mapped ? cf.committedLen(i, fs) : 0; c && c != recLen && from + len < fs.vLen) {
    UCACHE_ERROR("slot %u of %s: a record of %llu bytes where %u are committed", i,
                 cf.key.key.c_str(), static_cast<unsigned long long>(recLen), c);
    fatal = true;
    return false;
  }
  uint64_t n = 0;
  if (from < recLen) {
    n = std::min<uint64_t>(len, recLen - from);
    std::memcpy(dest, rec->data() + from, n);
  }
  if (n < len)
    std::memset(dest + n, 0, len - n); // the slot's padding
  recBytes = n;
  return true;
}

void ColdRequest::again(bool mayParkAgain) {
  auto r2 = std::make_shared<ColdRequest>();
  r2->st = st;
  r2->entry = entry;
  r2->cf = cf;
  r2->chunks = chunks;
  r2->isVRead = isVRead;
  r2->user = user;
  r2->attempt = attempt + 1;
  r2->mayPark = mayParkAgain;
  r2->t0 = t0;
  Executor::instance().post([r2] { serveRequest(r2); });
}

// Merging limits for a request's stored records: a read of at most 1 MiB (the
// limit the byte cache's reads have too), across gaps of at most 64 KiB (a commit block's header
// and alignment between two runs of records; ~3% more bytes on NanoAOD).
constexpr uint64_t kStoredRunBytes = 1ull << 20;
constexpr uint64_t kStoredRunGap = 64ull << 10;

void ColdRequest::readStoredTogether(std::unordered_map<uint32_t, ColdFill::Where>& mine) {
  std::vector<uint32_t> slots;
  std::vector<SlotEntry> es;
  std::vector<ColdFill::Where> ws;
  std::unordered_set<uint32_t> seen; // a slot in several pieces: read once
  for (const auto& p : slotPieces) {
    if (p.served || !seen.insert(p.slot).second)
      continue;
    ColdFill::Where w;
    auto h = mine.find(p.slot);
    if (h != mine.end())
      w = h->second;
    else if (!cf->where(p.slot, w))
      continue;
    if (w.mem || !w.inStore || w.e.kind == SlotEntry::kKept || !w.e.len)
      continue; // in memory, kept as stored, or not in the store: as before
    slots.push_back(p.slot);
    es.push_back(w.e);
    ws.push_back(std::move(w));
  }
  if (es.size() < 2)
    return; // one record: copySlot reads it as it always has
  Stats* stats = st->store ? &st->store->stats() : nullptr;
  std::vector<std::vector<uint8_t>> got;
  std::vector<char> ok;
  const uint64_t fellBack = cf->store->readRecords(es, got, ok, kStoredRunBytes, kStoredRunGap,
                         [stats](uint64_t us, uint64_t bytes) {
                           if (!stats)
                             return;
                           stats->replicaReads.fetch_add(1, std::memory_order_relaxed);
                           stats->replicaReadBytes.fetch_add(bytes, std::memory_order_relaxed);
                           stats->replicaReadSize.add(bytes);
                           stats->slotReadUs.add(us);
                         });
  if (stats && fellBack)
    stats->slotReadFallbacks.fetch_add(fellBack, std::memory_order_relaxed);
  for (size_t i = 0; i < es.size(); ++i) {
    if (!ok[i])
      continue; // copySlot reads it alone, and drops it if it fails again
    ColdFill::Where w = std::move(ws[i]);
    w.mem = std::make_shared<const std::vector<uint8_t>>(std::move(got[i]));
    mine[slots[i]] = std::move(w);
  }
}

void ColdRequest::finish() {
  // What this request converted goes to the store together, before anything
  // else and on every path (served, failed, retried): a retry is a new request
  // that stages nothing it finds ready, so records left here would never be
  // committed. Before the reader is answered, so a reader that closes on its
  // answer cannot outrun it (an append with no handle open queues a commit).
  {
    std::vector<std::pair<uint32_t, uint64_t>> group;
    {
      std::lock_guard<std::mutex> g(emu);
      group.swap(staged);
    }
    if (!group.empty())
      cf->appendGroup(std::move(group));
  }
  // Arrival (t0, kept across retries) to the answer handed to the reader.
  auto answered = [this] {
    if (st->store && t0)
      st->store->stats().coldRequestUs.add(nowUs() - t0);
  };
  if (failed.load()) {
    // Give back the claims this request still holds -- those it never staged.
    // A slot it staged may have been dropped and claimed by another request
    // since: that claim is not this request's to give back.
    std::vector<uint32_t> done;
    {
      std::lock_guard<std::mutex> g(emu);
      done = converted;
    }
    std::sort(done.begin(), done.end());
    for (uint32_t i : claimed)
      if (!std::binary_search(done.begin(), done.end(), i))
        cf->abandon(i);
    // Served again from the start, as a request whose slot fell through is
    // (requests that waited on this fetch do the same): a read the origin
    // failed once is asked for once more, as the plain miss path relays it.
    if (attempt < 2 && !refused.load()) {
      again(false);
      return;
    }
    XRootDStatus s;
    {
      std::lock_guard<std::mutex> g(emu);
      s = err;
    }
    // NOT noteCacheError: that trips a handle to pass-through after a few
    // errors, and a pass-through handle would send the origin offsets that
    // exist only in the layout this reader was shown. The origin's error goes
    // to the reader, as it would have without the cache.
    answered();
    complete(user, new XRootDStatus(s), nullptr);
    return;
  }
  bool lost = retry.load(); // another request's fetch failed
  uint64_t fillRec = 0, replicaRec = 0;
  std::sort(fetched.begin(), fetched.end());
  std::unordered_map<uint32_t, ColdFill::Where> mine;
  {
    std::lock_guard<std::mutex> g(emu);
    mine.swap(held);
  }
  if (!lost && cf->store)
    readStoredTogether(mine);
  // Every piece is tried, not only up to the first that fails: each failing
  // slot is forgotten (and a kept original's bad pages demoted), so the retry
  // fetches them all at once. Stopping at the first let a request with several
  // damaged slots use up its retries one slot at a time and fail the read.
  bool fatal = false;
  if (!lost)
    for (const auto& p : slotPieces) {
      uint64_t rec = 0;
      if (p.served) { // converted by this request: its page is in place already
        (std::binary_search(fetched.begin(), fetched.end(), p.slot) ? fillRec : replicaRec) += p.len;
        continue;
      }
      auto h = mine.find(p.slot);
      if (!copySlot(*cf, p.slot, p.from, p.len, p.dest, rec,
                    h == mine.end() ? nullptr : &h->second, st->coldMap != nullptr, fatal,
                    p.seekAs, p.expectLen, p.expectCrc)) {
        lost = true; // a record failed its check, or a kept original is gone
        continue;
      }
      (std::binary_search(fetched.begin(), fetched.end(), p.slot) ? fillRec : replicaRec) += rec;
    }
  if (lost) {
    if (attempt < 2 && !fatal) {
      again(false);
      return;
    }
    answered();
    complete(user, new XRootDStatus(XrdCl::stError, XrdCl::errDataError), nullptr);
    return;
  }
  if (st->store) {
    uint64_t total = 0, origFetched = 0;
    for (const auto& c : chunks)
      total += c.length;
    for (const auto& p : origPieces)
      origFetched += p.len;
    auto& stats = st->store->stats();
    // Every byte handed to the reader is served, as on every other path. Of
    // them, records already in the store (or converted from the byte cache)
    // are the replica tier's; records converted from what the origin just
    // sent, and original bytes fetched, are the fill. A slot's padding is
    // neither: zeros made here, which no tier holds.
    //
    // A file read directly (max_read_fraction) keeps none of what it fetched:
    // those bytes are relayed, as pass-through is, not a fill -- except the
    // file's own records, which are cached whatever the reader.
    uint64_t directBytes = 0;
    if (cf->direct()) {
      directBytes = fillRec;
      for (const auto& p : origPieces)
        if (cf->rule->readsData(p.off, p.len))
          directBytes += p.len;
    }
    stats.servedBytes.fetch_add(total - directBytes, std::memory_order_relaxed);
    stats.missBytes.fetch_add(fillRec + origFetched - directBytes, std::memory_order_relaxed);
    stats.replicaBytesServed.fetch_add(replicaRec, std::memory_order_relaxed);
    entry->obs().replicaBytes.fetch_add(replicaRec, std::memory_order_relaxed);
    if (directBytes) {
      stats.relayBytes.fetch_add(directBytes, std::memory_order_relaxed);
      stats.directReadBytes.fetch_add(directBytes, std::memory_order_relaxed);
      entry->obs().directBytes.fetch_add(directBytes, std::memory_order_relaxed);
    }
  }
  entry->noteActivity();
  st->noteCacheOk();
  answered();
  if (isVRead)
    complete(user, okStatus(), vreadResponse(chunks));
  else
    complete(user, okStatus(), chunkResponse(chunks[0].offset, chunks[0].length, chunks[0].buffer));
}

// Convert one claimed basket and stage it. `rounded` holds the page-rounded
// original range starting at `rStart`; the basket is [recOff, recOff+recLen)
// inside it. `fromCache` = the original came from the byte cache (its copy
// there is freed once the record is committed). Runs on the conversion pool.
void convertOne(const std::shared_ptr<ColdRequest>& req, uint32_t i,
                std::shared_ptr<std::vector<char>> rounded, uint64_t rStart, uint64_t recOff,
                uint32_t recLen, bool fromCache) try {
  ColdFill& cf = *req->cf;
  const tp::FillSlot& slot = cf.slot(i);
  const auto* rec = reinterpret_cast<const uint8_t*>(rounded->data() + recOff);
  const uint64_t t0 = nowUs();
  if (cf.rnt) {
    const ColdFill::Page& pg = cf.pages[i];
    tp::ConvertedPage p =
        tp::convertPage(rec, recLen, pg.nbytes, pg.hasChecksum, slot.vLen - tp::kSlotChecksumBytes);
    const uint64_t us = nowUs() - t0;
    if (!p.error.empty()) {
      // A page whose checksum or decode fails must not be served decoded: the
      // reader could no longer detect it. Fail the read, as ROOT would.
      UCACHE_WARN("slot run for %s: page at %llu: %s", cf.key.key.c_str(),
                  static_cast<unsigned long long>(slot.origSeek), p.error.c_str());
      req->fail(XRootDStatus(XrdCl::stError, XrdCl::errDataError));
      req->done();
      return;
    }
    const uint8_t kind = p.enc.size() == p.raw.size() ? tp::ConvertedBasket::kRaw
                                                      : tp::ConvertedBasket::kZstd;
    cf.convertUs.fetch_add(us, std::memory_order_relaxed);
    cf.inBytes.fetch_add(recLen, std::memory_order_relaxed);
    cf.outBytes.fetch_add(p.enc.size(), std::memory_order_relaxed);
    (kind == tp::ConvertedBasket::kZstd ? cf.nZstd : cf.nRaw).fetch_add(1, std::memory_order_relaxed);
    if (req->st->store) {
      auto& stats = req->st->store->stats();
      stats.coldReplicaConvertUs.fetch_add(us, std::memory_order_relaxed);
      stats.coldReplicaBaskets.fetch_add(1, std::memory_order_relaxed);
      stats.coldReplicaInBytes.fetch_add(recLen, std::memory_order_relaxed);
      stats.coldReplicaOutBytes.fetch_add(p.enc.size(), std::memory_order_relaxed);
    }
    const bool direct = cf.direct();
    if (cf.keepOriginals && !direct) {
      req->entry->writePages(rStart, rounded->size(), rounded->data());
      req->entry->flushMeta(false);
    } else if (!fromCache && !direct) {
      // Fetched for this file, kept only as its record: the file's origin tier
      // (staging counts what goes to the byte cache).
      req->entry->obs().wireBytes.fetch_add(recLen, std::memory_order_relaxed);
    }
    // This request's own pieces of the page, from the bytes just decoded: each
    // page is decoded once per pass, not again in finish().
    const uint32_t pageLen = slot.vLen - tp::kSlotChecksumBytes;
    if (p.raw.size() == pageLen)
      for (auto& piece : req->slotPieces)
        if (piece.slot == i && !piece.served) {
          servePage(p.raw.data(), pageLen, piece.from, piece.len, piece.dest);
          piece.served = true;
        }
    auto rec = std::make_shared<const std::vector<uint8_t>>(std::move(p.enc));
    req->hold(i, rec);
    uint64_t token = 0;
    cf.stage(i, kind, std::move(rec), fromCache, /*keepIt=*/!direct, &token);
    { // staged: the claim is no longer this request's to give back
      std::lock_guard<std::mutex> g(req->emu);
      req->converted.push_back(i);
      if (token)
        req->staged.emplace_back(i, token);
    }
    req->done();
    return;
  }
  tp::ConvertedBasket c = tp::convertBasket(rec, recLen, slot.vLen, cf.codecs);
  if (!c.error.empty()) { // not a basket: the reader gets the origin's bytes, as it would have
    c.kind = tp::ConvertedBasket::kOriginal;
    c.record.assign(rec, rec + recLen);
  }
  const uint64_t us = nowUs() - t0;
  cf.convertUs.fetch_add(us, std::memory_order_relaxed);
  cf.inBytes.fetch_add(recLen, std::memory_order_relaxed);
  cf.outBytes.fetch_add(c.record.size(), std::memory_order_relaxed);
  (c.kind == tp::ConvertedBasket::kZstd  ? cf.nZstd
   : c.kind == tp::ConvertedBasket::kRaw ? cf.nRaw
                                         : cf.nOrig)
      .fetch_add(1, std::memory_order_relaxed);
  if (req->st->store) {
    auto& stats = req->st->store->stats();
    stats.coldReplicaConvertUs.fetch_add(us, std::memory_order_relaxed);
    if (c.kind == tp::ConvertedBasket::kOriginal) {
      stats.coldReplicaBasketsKept.fetch_add(1, std::memory_order_relaxed);
    } else {
      stats.coldReplicaBaskets.fetch_add(1, std::memory_order_relaxed);
      stats.coldReplicaInBytes.fetch_add(recLen, std::memory_order_relaxed);
      stats.coldReplicaOutBytes.fetch_add(c.record.size(), std::memory_order_relaxed);
    }
  }
  if (!tp::patchKeySeek(c.record.data(), c.record.size(), slot.vSeek)) {
    req->fail(XRootDStatus(XrdCl::stError, XrdCl::errDataError));
    req->done();
    return;
  }
  // A kept basket is served from the byte cache, so it is written there even
  // when it came from it: taken from read-ahead's stage, its pages left the
  // stage when used, or are still only speculative.
  // A file read directly keeps neither: the request holds the record it serves.
  const bool direct = cf.direct();
  if ((c.kind == tp::ConvertedBasket::kOriginal || cf.keepOriginals) && !direct) {
    // Cannot be converted: the one kind of basket the byte cache keeps --
    // unless keeping every original was asked for, to compare the tiers.
    req->entry->writePages(rStart, rounded->size(), rounded->data());
    req->entry->flushMeta(false);
  } else if (!fromCache && !direct) {
    // Fetched for this file, kept only as its record: the file's origin tier
    // (staging counts what goes to the byte cache).
    req->entry->obs().wireBytes.fetch_add(recLen, std::memory_order_relaxed);
  }
  auto held = std::make_shared<const std::vector<uint8_t>>(std::move(c.record));
  req->hold(i, held);
  // A kept original is not punched from the byte cache: it is the only copy.
  uint64_t token = 0;
  cf.stage(i, static_cast<uint8_t>(c.kind), std::move(held),
           fromCache && c.kind != tp::ConvertedBasket::kOriginal, /*keepIt=*/!direct, &token);
  { // staged: the claim is no longer this request's to give back
    std::lock_guard<std::mutex> g(req->emu);
    req->converted.push_back(i);
    if (token)
      req->staged.emplace_back(i, token);
  }
  req->done();
} catch (const std::exception& e) {
  UCACHE_WARN("slot run for %s: conversion failed (%s)", req->cf->key.key.c_str(), e.what());
  req->fail(XRootDStatus(XrdCl::stError, XrdCl::errInternal));
  req->done();
}

// One part of a request's fetch: a vector read of the page-rounded original
// ranges of some claimed baskets and of original bytes the byte cache lacked.
struct PartItem {
  bool slot;          // a claimed basket, else an original-bytes piece
  uint32_t index;     // slot index, or index into origPieces
  uint64_t off, len;  // the exact original range
  uint64_t rs, re;    // page-rounded
};
struct WireElem {
  uint64_t off, len;
  std::shared_ptr<std::vector<char>> buf;
};

// Of page runs a file read directly would keep, the pages that hold none of its
// data: the file's own records are cached whatever the reader.
std::vector<std::pair<uint64_t, uint64_t>>
withoutData(const std::vector<std::pair<uint64_t, uint64_t>>& runs, const ReadRule& rule,
            uint64_t ps) {
  std::vector<std::pair<uint64_t, uint64_t>> out;
  for (const auto& [a, b] : runs)
    for (uint64_t p = a; p < b; p += ps) {
      const uint64_t pe = std::min(b, p + ps);
      if (rule.readsData(p, pe - p))
        continue;
      if (!out.empty() && out.back().second == p)
        out.back().second = pe;
      else
        out.emplace_back(p, pe);
    }
  return out;
}

class PartHandler : public ResponseHandler {
 public:
  PartHandler(std::shared_ptr<ColdRequest> req, std::vector<PartItem> items,
              std::vector<WireElem> wire)
      : req_(std::move(req)), items_(std::move(items)), wire_(std::move(wire)),
        t0_(nowUs()), inflight_(req_->st->store ? &req_->st->store->stats() : nullptr) {}

  void HandleResponseWithHosts(XRootDStatus* status, AnyObject* response,
                               HostList* hostList) override {
    req_->st->releaseInner();
    inflight_.release();
    std::unique_ptr<XRootDStatus> s(status);
    std::unique_ptr<AnyObject> r(response);
    std::unique_ptr<HostList> h(hostList);
    if (!s || !s->IsOK()) {
      if (req_->st->store)
        req_->st->store->stats().coldOriginFailures.fetch_add(1, std::memory_order_relaxed);
      req_->fail(s ? *s : XRootDStatus(XrdCl::stError, XrdCl::errInternal));
      req_->done();
      delete this;
      return;
    }
    uint64_t bytes = 0;
    for (const auto& w : wire_)
      bytes += w.len;
    auto& st = req_->st;
    if (st->store) {
      auto& stats = st->store->stats();
      stats.originBytes.fetch_add(bytes, std::memory_order_relaxed);
      stats.originReadvs.fetch_add(1, std::memory_order_relaxed);
      const uint64_t rt = nowUs() - t0_;
      stats.originRtUs.add(rt);
      req_->entry->obs().originRtUs.add(rt);
    }
    req_->cf->wireBytes.fetch_add(bytes, std::memory_order_relaxed);
    for (const auto& it : items_) {
      auto buf = std::make_shared<std::vector<char>>(it.re - it.rs);
      if (!gather(it.rs, it.re - it.rs, buf->data())) {
        req_->fail(XRootDStatus(XrdCl::stError, XrdCl::errInternal));
        continue;
      }
      if (it.slot) {
        req_->outstanding.fetch_add(1, std::memory_order_relaxed);
        auto req = req_;
        const uint64_t recOff = it.off - it.rs;
        const uint32_t recLen = static_cast<uint32_t>(it.len);
        const uint32_t idx = it.index;
        const uint64_t rs = it.rs;
        convertPool().post(
            [req, idx, buf, rs, recOff, recLen] {
              convertOne(req, idx, buf, rs, recOff, recLen, /*fromCache=*/false);
            });
      } else {
        const auto& p = req_->origPieces[it.index];
        std::memcpy(p.dest, buf->data() + (p.off - it.rs), p.len);
        // Original bytes the reader asked for: the byte cache keeps them --
        // except pages wholly inside a relocated basket, which a reader of
        // this layout never needs (a copy tool reading everything would
        // otherwise store every basket a second time).
        auto runs = req_->cf->storableRuns(it.rs, it.re, req_->entry->pageSize());
        if (req_->cf->direct())
          runs = withoutData(runs, *req_->cf->rule, req_->entry->pageSize());
        if (!runs.empty()) {
          st->beginPersist();
          auto entry = req_->entry;
          const uint64_t rs = it.rs;
          Executor::instance().post([st, entry, buf, rs, runs] {
            for (const auto& [a, b] : runs)
              entry->writePages(a, b - a, buf->data() + (a - rs));
            entry->flushMeta(false);
            st->endPersist();
          });
        }
      }
    }
    req_->done();
    delete this;
  }

 private:
  // Copy [off, off+len) out of the wire elements (sorted, covering it).
  bool gather(uint64_t off, uint64_t len, char* dst) const {
    uint64_t at = off, left = len;
    for (const auto& w : wire_) {
      if (!left)
        break;
      if (at < w.off || at >= w.off + w.len)
        continue;
      const uint64_t n = std::min<uint64_t>(left, w.off + w.len - at);
      std::memcpy(dst + (at - off), w.buf->data() + (at - w.off), n);
      at += n;
      left -= n;
    }
    return left == 0;
  }

  std::shared_ptr<ColdRequest> req_;
  std::vector<PartItem> items_;
  std::vector<WireElem> wire_;
  uint64_t t0_;
  OriginInFlight inflight_;
};

// Issue one part: coalesce its rounded ranges, cut them into legal elements,
// one vector read.
void issuePart(const std::shared_ptr<ColdRequest>& req, std::vector<PartItem> items) {
  std::vector<std::pair<uint64_t, uint64_t>> runs;
  for (const auto& it : items) { // items arrive sorted by rs
    if (!runs.empty() && it.rs <= runs.back().second)
      runs.back().second = std::max(runs.back().second, it.re);
    else
      runs.emplace_back(it.rs, it.re);
  }
  std::vector<WireElem> wire;
  ChunkList chunks;
  const uint64_t ps = req->entry->pageSize();
  for (auto [s, e] : runs)
    for (uint64_t at = s; at < e;) {
      const uint64_t cut = readvElemEnd(at, e, ps);
      WireElem w{at, cut - at, std::make_shared<std::vector<char>>(cut - at)};
      chunks.emplace_back(w.off, static_cast<uint32_t>(w.len), w.buf->data());
      wire.push_back(std::move(w));
      at = cut;
    }
  req->outstanding.fetch_add(1, std::memory_order_relaxed);
  XrdCl::File* f = req->st->acquireInner();
  if (!f) {
    req->fail(req->st->missError());
    req->done();
    return;
  }
  auto* h = new PartHandler(req, std::move(items), std::move(wire));
  XRootDStatus s;
  if (readFaultFire()) // test hook: the origin's vector read dies mid-stream
    Executor::instance().post([h] {
      h->HandleResponseWithHosts(new XRootDStatus(XrdCl::stError, XrdCl::errConnectionError),
                                 nullptr, nullptr);
    });
  else
    s = f->VectorRead(chunks, nullptr, h, 0);
  if (!s.IsOK()) {
    delete h;
    req->st->releaseInner();
    if (req->st->store)
      req->st->store->stats().coldOriginFailures.fetch_add(1, std::memory_order_relaxed);
    req->fail(s);
    req->done();
  }
}

// A range of the file's addresses noted in its in-use record (InUse.h), off
// the reader's path: a map's metadata handed out to the handle shown it
// (`kHandOut`, which the handle then holds); a read of a map's addresses or
// metadata by a handle not shown it (`kUse`: positions learned elsewhere,
// counted from now but not held, since only the maps a process shows are
// released at its close); a map an open handle of this process still holds
// (`kHold`); or this process's last close of the file (`kRelease`: the window
// counts from it). Hand-outs, uses and holds are rate-limited per range;
// nothing here can fail a read.
enum class Use { kHandOut, kUse, kHold, kRelease };
constexpr uint64_t kHandOutNoteS = 60;
void noteUse(ColdFill& cf, uint64_t lo, uint64_t hi, Use u) {
  auto cs = globalStore();
  if (!cs || !cf.store || cf.store->header().declined)
    return;
  const uint64_t now = wallSeconds();
  {
    std::lock_guard<std::mutex> g(cf.mu);
    auto& t = cf.inUseNoted[lo];
    if (u == Use::kHandOut || u == Use::kUse) {
      if (t.first && now < t.first + kHandOutNoteS)
        return;
      t.first = now;
    } else if (u == Use::kHold) {
      if (t.second && now < t.second + InUseRecord::kHoldRefreshS)
        return;
      t.second = now;
    }
  }
  const SlotStoreHeader& h = cf.store->header();
  InUseRange r;
  r.storeId = h.storeId;
  r.lo = lo;
  r.hi = hi;
  // A hand-out is also this process's hold: the handle that read the metadata
  // has the map now, and other processes must see that before the first
  // refresh. A release removes this process's hold.
  r.pid = static_cast<uint64_t>(::getpid());
  if (u == Use::kHandOut || u == Use::kHold)
    r.holdS = now;
  if (u != Use::kHold)
    r.lastS = now;
  InUseSettings s{h.layoutVersion, h.slotFactor100, h.codecs};
  const std::string path = InUseRecord::path(cs->config().cacheDir, cf.key.hashHex);
  const uint64_t window = globalConfig().inUseSeconds;
  if (u == Use::kRelease) { // at once: a map made right after (at exit) must see it
    InUseRecord::note(RealIO::instance(), path, r, &s, window, /*release=*/true);
    return;
  }
  Executor::instance().post(
      [path, r, s, window] { InUseRecord::note(RealIO::instance(), path, r, &s, window); });
}
// Map 1, the slot layout itself: its slots.
void noteSlotLayout(ColdFill& cf, Use u) { noteUse(cf, cf.L.slotsBegin, cf.L.virtualSize, u); }

// Walk every chunk: answer what is here now, note what must be fetched.
void classify(const std::shared_ptr<ColdRequest>& req, std::vector<uint32_t>& need) {
  ColdFill& cf = *req->cf;
  const tp::FillLayout& L = cf.L;
  const uint64_t metaEnd = L.metaSeek + L.metaRecord.size();
  // The handle's mixed map, if it was shown one: its keys-list window in place
  // of the layout's, its tree record in the tables area.
  const ColdMap* map = req->st->coldMap.get();
  coldPrepare(cf, req->chunks); // every branch the request touches, in one read
  // Did the reader learn positions here: the layout's windows or tree record
  // (map 1), or a mixed map's tree record (its range)? Noted after the walk.
  bool layoutMeta = false;
  uint64_t mapLo = 0, mapHi = 0;
  bool mapOwn = false; // the map read is the one the handle was shown
  // A TTree compact map's tree record, in the tables area: noted beside the
  // map's range, so a store made after this one is replaced keeps clear of it.
  uint64_t tabLo = 0, tabHi = 0;
  auto noteTables = [&tabLo, &tabHi](const ColdMap& m) {
    const bool apart = m.compact() && !m.metaInRange();
    tabLo = apart ? m.metaSeek : 0;
    tabHi = apart ? m.metaSeek + m.meta.size() : 0;
  };
  std::vector<size_t> compactIdx; // pieces read at a compact map's address
  for (const auto& c : req->chunks) {
    char* base = static_cast<char*>(c.buffer);
    uint64_t pos = c.offset;
    const uint64_t end = c.offset + c.length;
    while (pos < end) {
      char* d = base + (pos - c.offset);
      if (pos < L.originSize) {
        const uint64_t segEnd = std::min(end, L.originSize);
        // A compact map's header, stating its end: the handle's own, in a
        // window of map 1's or as the whole 64-bit header at offset 0.
        if (map && map->compact() && pos >= map->headerOff &&
            pos < map->headerOff + map->header.size()) {
          const uint64_t n = std::min(segEnd, map->headerOff + map->header.size()) - pos;
          std::memcpy(d, map->header.data() + (pos - map->headerOff), n);
          pos += n;
          layoutMeta = true;
          continue;
        }
        // A window, if one covers pos; else the original bytes up to the next one.
        uint64_t next = segEnd;
        bool inWindow = false;
        for (const auto& w : L.windows) {
          const uint64_t we = w.off + w.bytes.size();
          if (pos >= w.off && pos < we) {
            const uint64_t n = std::min(segEnd, we) - pos;
            // The handle's map has its own keys-list window, at the offset
            // and size of map 1's.
            const uint8_t* src =
                map && w.off == map->keysListOff ? map->keysList.data() : w.bytes.data();
            std::memcpy(d, src + (pos - w.off), n);
            pos += n;
            inWindow = true;
            layoutMeta = true;
            break;
          }
          if (w.off > pos)
            next = std::min(next, w.off);
        }
        if (inWindow)
          continue;
        const uint64_t n = next - pos;
        if (!(req->entry->hasRange(pos, n) && req->entry->readCached(pos, n, d)))
          req->origPieces.push_back({pos, n, d});
        pos += n;
      } else if (pos < L.metaSeek) {
        const uint64_t n = std::min(end, L.metaSeek) - pos;
        std::memset(d, 0, n);
        pos += n;
      } else if (pos < metaEnd) {
        const uint64_t n = std::min(end, metaEnd) - pos;
        std::memcpy(d, L.metaRecord.data() + (pos - L.metaSeek), n);
        pos += n;
        layoutMeta = true;
      } else if (pos < L.slotsBegin) {
        // The tables area: a map's tree record where one lies -- the handle's
        // own; else one this process showed, while a handle holds it (a read
        // there of one no handle holds fails); else one the store holds --
        // else zeros.
        uint64_t n = std::min(end, L.slotsBegin) - pos;
        std::shared_ptr<const ColdMap> other;
        const ColdMap* at = map && pos >= map->metaSeek && pos < map->metaSeek + map->meta.size()
                                ? map
                                : nullptr;
        bool gone = false;
        if (!at && (other = shownMapAt(cf.key.key, pos, gone)))
          at = other.get();
        if (!at && gone) {
          req->fail(XRootDStatus(XrdCl::stError, XrdCl::errDataError, 0,
                                 "a mixed map's tree record no handle holds any more"));
          UCACHE_WARN("%s: a read at %llu, in the tree record of a map no handle holds any more, "
                      "fails; open the file again",
                      cf.key.key.c_str(), static_cast<unsigned long long>(pos));
        }
        if (!at && !gone && (other = cf.mapCovering(pos)))
          at = other.get();
        bool foreign = false;
        uint64_t foreignAt = UINT64_MAX;
        if (!at && !gone)
          cf.tablesForeign(pos, foreign, foreignAt);
        if (foreign) {
          // A map's tree record of the store this one replaced, handed out and
          // perhaps still held: no bytes here are this store's to give.
          req->refused.store(true);
          req->fail(XRootDStatus(XrdCl::stError, XrdCl::errDataError, 0,
                                 "a read in the tree record of a map of a replaced store"));
          if (auto cs = globalStore())
            cs->stats().slotMapRefused.fetch_add(1, std::memory_order_relaxed);
          const uint64_t nowS = wallSeconds();
          if (uint64_t last = cf.refusedLogS.load(); nowS != last &&
                                                     cf.refusedLogS.compare_exchange_strong(last, nowS))
            UCACHE_ERROR("%s: a read at %llu, in the tree record of a map of a store that was "
                         "replaced, fails; open the file again",
                         cf.key.key.c_str(), static_cast<unsigned long long>(pos));
        } else if (foreignAt != UINT64_MAX) {
          n = std::min<uint64_t>(n, foreignAt - pos);
        }
        if (at) {
          n = std::min<uint64_t>(n, at->metaSeek + at->meta.size() - pos);
          std::memcpy(d, at->meta.data() + (pos - at->metaSeek), n);
          mapLo = at->compact() ? at->rangeLo : at->metaSeek;
          mapHi = at->compact() ? at->end() : at->metaSeek + at->meta.size();
          mapOwn = at == map;
          noteTables(*at);
        } else {
          if (map && pos < map->metaSeek)
            n = std::min<uint64_t>(n, map->metaSeek - pos);
          n = cf.zerosFrom(pos, n);
          std::memset(d, 0, n);
        }
        pos += n;
      } else if (pos >= L.virtualSize) {
        // Past map 1's slots: a compact map's range (any handle's, by
        // address), or a guard -- zeros.
        bool refused = false;
        uint64_t upto = UINT64_MAX;
        auto cm = cf.compactAt(pos, refused, upto);
        if (cm && pos < cm->rangeLo + cm->firstOff) {
          // An RNTuple map's own page list and footer, then zeros to its records.
          const uint64_t rel = pos - cm->rangeLo;
          uint64_t n = std::min<uint64_t>(end, cm->rangeLo + cm->firstOff) - pos;
          if (rel < cm->meta.size()) {
            n = std::min<uint64_t>(n, cm->meta.size() - rel);
            std::memcpy(d, cm->meta.data() + rel, n);
          } else {
            std::memset(d, 0, n);
          }
          mapLo = cm->rangeLo;
          mapHi = cm->end();
          mapOwn = cm.get() == map;
          noteTables(*cm);
          pos += n;
        } else if (cm) {
          const auto& R = cm->recs;
          auto it = std::upper_bound(R.begin(), R.end(), pos - cm->rangeLo,
                                     [](uint64_t v, const ColdMap::Rec& r) { return v < r.off; });
          const ColdMap::Rec& r = *(it - 1); // recs[0].off is firstOff
          const uint64_t base = cm->rangeLo + r.off;
          const uint64_t n = std::min<uint64_t>(end, base + cm->extent(r)) - pos;
          req->slotPieces.push_back({r.slot, pos - base, n, d});
          auto& p = req->slotPieces.back();
          p.seekAs = base;
          p.expectLen = r.len;
          p.expectCrc = r.crc;
          compactIdx.push_back(req->slotPieces.size() - 1);
          mapLo = cm->rangeLo; // a use of the map: its positions are being read
          mapHi = cm->end();
          mapOwn = cm.get() == map;
          noteTables(*cm);
          pos += n;
        } else if (refused) {
          req->refused.store(true);
          req->fail(XRootDStatus(XrdCl::stError, XrdCl::errDataError, 0,
                                 "a read in the range of a map that is no longer valid"));
          if (auto cs = globalStore())
            cs->stats().slotMapRefused.fetch_add(1, std::memory_order_relaxed);
          const uint64_t nowS = wallSeconds();
          if (uint64_t last = cf.refusedLogS.load(); nowS != last &&
                                                     cf.refusedLogS.compare_exchange_strong(last, nowS))
            UCACHE_ERROR("%s: a read at %llu, in the range of a map that is no longer valid "
                         "(replaced and unused for in_use_seconds, or its store was replaced), "
                         "fails; open the file again",
                         cf.key.key.c_str(), static_cast<unsigned long long>(pos));
          std::memset(d, 0, end - pos);
          pos = end;
        } else {
          const uint64_t n = std::min(end, upto) - pos;
          std::memset(d, 0, n);
          pos += n;
        }
      } else {
        uint32_t i = 0;
        if (!cf.slots.find(pos, i)) {
          // The slot table cannot be read again (the store file failed its
          // check): this request cannot be served in the layout it was shown.
          req->fail(XRootDStatus(XrdCl::stError, XrdCl::errDataError, 0,
                                 "slot table unreadable"));
          std::memset(d, 0, end - pos);
          pos = end;
          continue;
        }
        const tp::FillSlot& s = cf.slot(i);
        const uint64_t from = pos - s.vSeek;
        const uint64_t n = std::min<uint64_t>(end, s.vSeek + s.vLen) - pos;
        if (!n) { // past the slot: no layout has a gap here
          req->fail(XRootDStatus(XrdCl::stError, XrdCl::errDataError, 0, "no slot at offset"));
          std::memset(d, 0, end - pos);
          pos = end;
          continue;
        }
        req->slotPieces.push_back({i, from, n, d});
        if (cf.state[i].load(std::memory_order_acquire) != ColdFill::kReady ||
            cf.keptOriginalGone(i)) {
          need.push_back(i);
        } else {
          req->holdReady(i); // may be dropped from the file's copy before it is copied
        }
        pos += n;
      }
    }
  }
  if (!compactIdx.empty()) {
    // Their slots, decoded in one read; then as any slot read: ready, or to
    // be fetched and converted again (its record failed its check).
    std::vector<uint32_t> idx;
    idx.reserve(compactIdx.size());
    for (size_t k : compactIdx)
      idx.push_back(req->slotPieces[k].slot);
    if (!cf.slots.prepareSlots(idx)) {
      req->fail(XRootDStatus(XrdCl::stError, XrdCl::errDataError, 0, "slot table unreadable"));
    } else {
      for (uint32_t i : idx)
        if (cf.state[i].load(std::memory_order_acquire) != ColdFill::kReady)
          need.push_back(i);
        else
          req->holdReady(i);
    }
  }
  if (layoutMeta)
    noteSlotLayout(cf, Use::kHandOut);
  if (mapHi)
    noteUse(cf, mapLo, mapHi, mapOwn ? Use::kHandOut : Use::kUse);
  if (tabHi)
    noteUse(cf, tabLo, tabHi, mapOwn ? Use::kHandOut : Use::kUse);
}

void serveRequest(const std::shared_ptr<ColdRequest>& req) {
  ColdFill& cf = *req->cf;
  std::vector<uint32_t> need;
  classify(req, need);
  std::sort(need.begin(), need.end());
  need.erase(std::unique(need.begin(), need.end()), need.end());
  if (!need.empty()) {
    // Another process may have converted them since: use its records rather
    // than fetch and convert the same baskets again.
    cf.sync();
    need.erase(std::remove_if(need.begin(), need.end(),
                              [&](uint32_t i) {
                                if (cf.state[i].load(std::memory_order_acquire) !=
                                    ColdFill::kReady)
                                  return false;
                                req->holdReady(i);
                                return true;
                              }),
               need.end());
  }
  if (!need.empty() || !req->origPieces.empty()) // fetched, or waited for
    noteWaitTier(req->user, ReaderWait::kOrigin);
  // max_read_fraction is decided at the first request that needs the origin,
  // from the ORIGINAL ranges the request carries (a slot is its basket).
  if (cf.rule && cf.rule->state() == ReadRule::kUndecided &&
      (!need.empty() || !req->origPieces.empty())) {
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (const auto& c : req->chunks)
      coldOriginRanges(cf, c.offset, c.length, ranges, req->st->coldMap.get());
    OriginSource src;
    src.st = req->st;
    src.entry = req->entry;
    cf.rule->observe(ranges, src);
  }

#ifdef UCACHE_HAVE_PREFETCH
  const Config& cfg = globalConfig();
  // A file read directly is not read ahead: nothing it fetches is kept.
  const bool direct = cf.direct();
  if (cfg.prefetch && req->mayPark && !need.empty() && !direct) {
    // Read-ahead learns from this fill in the ORIGINAL file's coordinates --
    // a slot is its basket -- and predicts and fetches the next baskets into
    // the byte cache's speculative stage, from where the conversion below
    // takes them. Only the slots still to be converted: predicting from the
    // ones in the store would fetch originals nobody needs.
    XrdCl::ChunkList orig;
    for (uint32_t i : need)
      orig.emplace_back(cf.slot(i).origSeek, cf.slot(i).origLen, nullptr);
    Prefetcher::instance().onFill(req->st, req->entry, orig, true);
  }
  if (cfg.prefetch && cfg.prefetchJoin && req->mayPark && !need.empty() && !direct) {
    // A basket read ahead may be on the wire right now: wait for that copy
    // (once) instead of fetching it a second time.
    std::vector<std::pair<uint64_t, uint64_t>> want;
    for (uint32_t i : need) {
      const tp::FillSlot& s = cf.slot(i);
      if (!req->entry->hasRange(s.origSeek, s.origLen))
        want.emplace_back(s.origSeek, s.origLen);
    }
    if (!want.empty()) {
      auto fired = std::make_shared<std::atomic<bool>>(false);
      auto again = [req, fired] {
        if (fired->exchange(true))
          return;
        auto r2 = std::make_shared<ColdRequest>();
        r2->st = req->st;
        r2->entry = req->entry;
        r2->cf = req->cf;
        r2->chunks = req->chunks;
        r2->isVRead = req->isVRead;
        r2->user = req->user;
        r2->attempt = req->attempt;
        r2->mayPark = false;
        r2->t0 = req->t0;
        Executor::instance().post([r2] { serveRequest(r2); });
      };
      if (req->entry->waitForInFlight(want, again)) {
        Executor::instance().postAfter(10000, again); // a lost wakeup must not hang the read
        return;
      }
    }
  }
#endif

  // Claim what nobody is fetching; wait for what somebody is.
  for (uint32_t i : need) {
    bool ready = false;
    req->outstanding.fetch_add(1, std::memory_order_relaxed);
    auto cb = [req, i](bool ok, ColdFill::Rec r) {
      if (!ok)
        req->retry.store(true);
      else if (r)
        req->hold(i, std::move(r));
      req->done();
    };
    if (cf.claimOrWait(i, cb, ready)) {
      req->claimed.push_back(i);
      req->outstanding.fetch_sub(1, std::memory_order_relaxed); // counted per conversion instead
    } else if (ready) {
      req->holdReady(i); // converted meanwhile, as in classify
      req->outstanding.fetch_sub(1, std::memory_order_relaxed);
    } else {
      req->fetched.push_back(i); // converting elsewhere, from wherever that request got it
    }
  }

  // Which original pages are worth keeping needs every relocated basket's
  // range: known before any of what is fetched arrives, on this thread.
  if (!req->claimed.empty() || !req->origPieces.empty())
    cf.ensureRelocatedOrig();

  // What to fetch: claimed baskets the byte cache does not hold, and original
  // bytes it did not have. A claimed basket already in the byte cache (a
  // file read before recompression was switched on) converts from there.
  std::vector<PartItem> items;
  const uint64_t ps = req->entry->pageSize(), fs = req->entry->fileSize();
  for (uint32_t i : req->claimed) {
    const tp::FillSlot& s = cf.slot(i);
    auto [rs, re] = roundSpan(ps, fs, s.origSeek, s.origLen);
    if (req->entry->hasRange(s.origSeek, s.origLen)) {
      // Its whole pages: a basket kept as stored is written back from them, so
      // none of its bytes stay only in read-ahead's stage.
      auto buf = std::make_shared<std::vector<char>>(re - rs);
      if (req->entry->readCached(rs, re - rs, buf->data(), /*account=*/false)) {
        // Read ahead into the stage: used now, so it leaves the stage as served
        // rather than being written or dropped as never used.
        req->entry->consumeSpeculative(s.origSeek, s.origLen);
        req->outstanding.fetch_add(1, std::memory_order_relaxed);
        const uint64_t rStart = rs;
        convertPool().post([req, i, buf, s, rStart] {
          convertOne(req, i, buf, rStart, s.origSeek - rStart, s.origLen, /*fromCache=*/true);
        });
        continue;
      }
    }
    req->fetched.push_back(i);
    items.push_back({true, i, s.origSeek, s.origLen, rs, re});
  }
  for (uint32_t k = 0; k < req->origPieces.size(); ++k) {
    const auto& p = req->origPieces[k];
    auto [rs, re] = roundSpan(ps, fs, p.off, p.len);
    items.push_back({false, k, p.off, p.len, rs, re});
  }
  std::sort(items.begin(), items.end(),
            [](const PartItem& a, const PartItem& b) { return a.rs < b.rs; });
  // Parts: consecutive items until a part holds kPartBytes, cut only where the
  // next item does not share a page with the part so far.
  std::vector<PartItem> part;
  uint64_t partBytes = 0, partEnd = 0;
  for (const auto& it : items) {
    // Cut by bytes, and by element count: a vector read carries at most 1024.
    if (!part.empty() && (partBytes >= kPartBytes || part.size() >= kPartItems) &&
        it.rs >= partEnd) {
      issuePart(req, std::move(part));
      part.clear();
      partBytes = 0;
    }
    partBytes += it.re - std::max(it.rs, std::min(partEnd, it.re));
    partEnd = std::max(partEnd, it.re);
    part.push_back(it);
  }
  if (!part.empty())
    issuePart(req, std::move(part));
  req->done(); // the issuing stage's own hold
}

} // namespace

// --------------------------------------------------------------- committing

namespace {

// Commits run here: they take the store's exclusive lock, which may wait for
// another process's commit, and nothing that serves reads may wait on that.
// Leaked like the other pools.
Executor& commitPool() {
  static Executor* pool = new Executor(2);
  return *pool;
}

// Maps are made on a pool of their own: making one (an RNTuple file's page
// list and footer are parsed and rebuilt) must never hold up commits, or the
// records waiting pass their cap and conversions are served without being
// kept -- read from the origin again by the next pass.
Executor& mapPool() {
  static Executor* pool = new Executor(2);
  return *pool;
}

// Records converted in this process and not yet committed, all files. Past
// the cap the thread that converts commits itself -- back-pressure instead of
// memory growth.
std::atomic<uint64_t> g_pendingTotal{0};
constexpr uint64_t kPendingCap = 512ull << 20;
// Records served but not committed (see ColdFill::transient): a cache for a
// reader that reads a slot again, nothing more -- every request holds the
// records it needs itself. Per file, and for the whole process.
constexpr uint64_t kTransientBytes = 16ull << 20;
std::atomic<uint64_t> g_transientTotal{0};
constexpr uint64_t kTransientCap = 256ull << 20;

// The file's first 128 KiB is never punched: ROOT reads it as one block, and
// a hole there sends every open to the origin.
constexpr uint64_t kKeepHead = 128 * 1024;

} // namespace

ColdFill::~ColdFill() {
  g_pendingTotal.fetch_sub(std::min(pendingBytes, g_pendingTotal.load()), std::memory_order_relaxed);
  g_transientTotal.fetch_sub(std::min(transientBytes, g_transientTotal.load()),
                             std::memory_order_relaxed);
  // Served, never committed (the store was gone): not kept.
  uint64_t lost = 0;
  for (const auto& [slot, s] : info)
    lost += s.transient && s.mem && !s.inStore;
  if (lost)
    if (auto store = globalStore())
      store->stats().coldReplicaDeclined.fetch_add(lost, std::memory_order_relaxed);
}

void ColdFill::releaseMemLocked(Info& s) {
  if (s.transient && s.mem) {
    const uint64_t b = std::min<uint64_t>(transientBytes, s.mem->size());
    transientBytes -= b;
    g_transientTotal.fetch_sub(std::min(b, g_transientTotal.load()), std::memory_order_relaxed);
  }
  s.transient = false;
  s.mem.reset();
}

void ColdFill::stage(uint32_t i, uint8_t kind, Rec rec, bool fromCache, bool keepIt,
                     uint64_t* group) {
  std::vector<Waiter> wake;
  bool kick = false;
  uint64_t lost = 0; // records no longer held anywhere: converted again if read
  if (group)
    *group = 0;
  {
    std::lock_guard<std::mutex> g(mu);
    Info& s = info[i];
    bool keep = true;
    if (!keepIt && !s.inStore) {
      releaseMemLocked(s); // a file read directly: nothing of it is kept
      keep = false;
    } else if (!s.inStore) { // someone committed it meanwhile: theirs serves
      releaseMemLocked(s); // staged twice: the newer record replaces the older
      s.kind = kind;
      s.held = false;
      // A basket kept as stored is served from the byte cache's copy of its
      // original, just written; memory keeps it only if that write did not
      // land.
      const tp::FillSlot& fs = slot(i);
      const bool inCache = !rnt && kind == tp::ConvertedBasket::kOriginal &&
                           entry->hasRange(fs.origSeek, fs.origLen);
      const uint64_t b = inCache ? 0 : rec->size();
      s.fromCache = fromCache;
      if (group && !storeGone.load(std::memory_order_relaxed)) {
        // Held for its request, which commits it with the rest of what it read.
        s.mem = inCache ? nullptr : rec;
        s.held = true;
        s.token = ++tokenSeq;
        s.stagedUs = nowUs();
        heldList.emplace_back(i, s.token, s.stagedUs);
        *group = s.token;
      } else {
        // Committed, unless the store is gone or the process already holds its
        // cap of records waiting: then it is served to the requests that asked
        // for it (they hold it) and kept only while the transient budget
        // allows -- memory never grows to cope, and nothing that serves reads
        // waits for a commit.
        const bool persist = !storeGone.load(std::memory_order_relaxed) &&
                             g_pendingTotal.load(std::memory_order_relaxed) + b <= kPendingCap;
        keep = placeLocked(i, s, rec, b, inCache, persist, lost, kick);
      }
    }
    takeWaitersLocked(i, wake);
    if (keep) {
      state[i].store(kReady, std::memory_order_release);
    } else { // served to whoever asked, and not kept: absent again
      info.erase(i);
      state[i].store(kAbsent, std::memory_order_release);
    }
  }
  for (auto& w : wake)
    w(true, rec);
  if (lost)
    if (auto store = globalStore())
      store->stats().coldReplicaDeclined.fetch_add(lost, std::memory_order_relaxed);
  if (kick)
    queueCommit();
}

bool ColdFill::placeLocked(uint32_t i, Info& s, const Rec& rec, uint64_t b, bool inCache,
                           bool persist, uint64_t& lost, bool& kick) {
  s.persist = persist;
  if (persist) {
    s.mem = inCache ? nullptr : rec;
    pending.push_back(i);
    pendingBytes += b;
    g_pendingTotal.fetch_add(b, std::memory_order_relaxed);
    kick = kick || pendingBytes >= kCommitBytes || handles == 0;
    return true;
  }
  if (inCache)
    return true; // served from the byte cache: committed the next time it is staged with room
  // Oldest first, never this record, until both budgets fit.
  while ((transientBytes + b > kTransientBytes ||
          g_transientTotal.load(std::memory_order_relaxed) + b > kTransientCap) &&
         !transient.empty()) {
    const uint32_t j = transient.front();
    transient.pop_front();
    if (j == i)
      continue;
    auto it = info.find(j);
    if (it != info.end() && it->second.transient && !it->second.inStore) {
      forgetLocked(j);
      ++lost;
    }
  }
  const bool keep = transientBytes + b <= kTransientBytes &&
                    g_transientTotal.load(std::memory_order_relaxed) + b <= kTransientCap;
  if (!keep) {
    ++lost;
    return false;
  }
  s.mem = rec;
  s.transient = true;
  transient.push_back(i);
  transientBytes += b;
  g_transientTotal.fetch_add(b, std::memory_order_relaxed);
  kick = kick || !storeGone.load(std::memory_order_relaxed); // drain what waits
  return true;
}

void ColdFill::appendGroupLocked(std::vector<std::pair<uint32_t, uint64_t>>& group,
                                 uint64_t& lost, bool& kick) {
  std::sort(group.begin(), group.end());
  // The whole group goes one way: committed if the process is under its cap of
  // records waiting (it may pass it by one group), else served and transient.
  const bool persist = !storeGone.load(std::memory_order_relaxed) &&
                       g_pendingTotal.load(std::memory_order_relaxed) <= kPendingCap;
  for (const auto& [i, token] : group) {
    auto it = info.find(i);
    if (it == info.end())
      continue; // forgotten meanwhile
    Info& s = it->second;
    if (!s.held || s.token != token || s.inStore) {
      if (s.token == token)
        s.held = false;
      continue; // staged again by another request, or committed by someone
    }
    s.held = false;
    const bool inCache = !s.mem;
    Rec rec = s.mem;
    const uint64_t b = rec ? rec->size() : 0;
    if (!placeLocked(i, s, rec, b, inCache, persist, lost, kick))
      forgetLocked(i);
  }
  // Requests mostly finish in the order they staged: drop what is handed back.
  while (!heldList.empty()) {
    const auto& [i, token, at] = heldList.front();
    (void)at;
    auto it = info.find(i);
    if (it != info.end() && it->second.held && it->second.token == token)
      break;
    heldList.pop_front();
  }
}

void ColdFill::appendGroup(std::vector<std::pair<uint32_t, uint64_t>> group) {
  uint64_t lost = 0;
  bool kick = false;
  {
    std::lock_guard<std::mutex> g(mu);
    appendGroupLocked(group, lost, kick);
  }
  if (lost)
    if (auto store = globalStore())
      store->stats().coldReplicaDeclined.fetch_add(lost, std::memory_order_relaxed);
  if (kick)
    queueCommit();
}

void ColdFill::appendHeldOlderThan(uint64_t ageUs) {
  uint64_t lost = 0;
  bool kick = false;
  {
    std::lock_guard<std::mutex> g(mu);
    const uint64_t now = nowUs();
    std::vector<std::pair<uint32_t, uint64_t>> group;
    std::deque<std::tuple<uint32_t, uint64_t, uint64_t>> keep;
    for (const auto& [i, token, at] : heldList) {
      auto it = info.find(i);
      if (it == info.end() || !it->second.held || it->second.token != token)
        continue; // already handed back, or staged again
      if (now - at >= ageUs)
        group.emplace_back(i, token);
      else
        keep.emplace_back(i, token, at);
    }
    heldList.swap(keep);
    if (!group.empty())
      appendGroupLocked(group, lost, kick);
  }
  if (lost)
    if (auto store = globalStore())
      store->stats().coldReplicaDeclined.fetch_add(lost, std::memory_order_relaxed);
  if (kick)
    queueCommit();
}

bool ColdFill::pageDeadLocked(uint64_t pg, uint64_t ps) {
  const uint64_t a = pg * ps, b = std::min(a + ps, L.originSize);
  if (a < kKeepHead || a >= b)
    return false;
  // Every byte must be covered, with no gap, by committed converted baskets.
  // Only the branches this process has decoded are known: a page also covered
  // by one it has not is kept, which is never wrong (it only frees less).
  std::vector<uint32_t> over;
  slots.decodedOverlappingOrig(a, b, over);
  uint64_t covered = a;
  for (uint32_t k : over) {
    const tp::FillSlot& fs = slot(k);
    if (fs.origSeek > covered)
      return false; // bytes in no relocated basket we know: the file's own records, or unknown
    auto in = info.find(k);
    if (in == info.end() || !in->second.inStore || in->second.kind == tp::ConvertedBasket::kOriginal)
      return false; // not converted yet, or kept as stored: its original serves
    covered = std::max<uint64_t>(covered, fs.origSeek + fs.origLen);
    if (covered >= b)
      return true;
  }
  return covered >= b;
}

void ColdFill::ensureRelocatedOrig() {
  {
    std::lock_guard<std::mutex> g(mu);
    if (relocatedOrig || relocatedTried)
      return;
    relocatedTried = true;
  }
  // Every relocated basket's original range: one pass over the whole table,
  // decoded for it and not kept.
  std::vector<std::pair<uint64_t, uint64_t>> r;
  r.reserve(nSlots());
  if (!slots.forEachAll([&r](uint32_t, const tp::FillSlot& s) {
        r.emplace_back(s.origSeek, s.origSeek + s.origLen);
      }))
    return;
  std::sort(r.begin(), r.end());
  // As merged runs: a page covered by two adjacent baskets is as dead as one
  // inside a single basket.
  auto merged = std::make_shared<std::vector<std::pair<uint64_t, uint64_t>>>();
  for (const auto& [a, b] : r) {
    if (!merged->empty() && a <= merged->back().second)
      merged->back().second = std::max(merged->back().second, b);
    else
      merged->emplace_back(a, b);
  }
  merged->shrink_to_fit();
  std::lock_guard<std::mutex> g(mu);
  relocatedOrig = std::move(merged);
}

std::vector<std::pair<uint64_t, uint64_t>> ColdFill::storableRuns(uint64_t rs, uint64_t re,
                                                                  uint64_t ps) {
  // Built before the fetch was issued (ensureRelocatedOrig: this may be an
  // XrdCl callback thread, which must not read the disk); if it could not be,
  // nothing is dead and every page is kept, which is never wrong.
  std::shared_ptr<const std::vector<std::pair<uint64_t, uint64_t>>> rel;
  {
    std::lock_guard<std::mutex> g(mu);
    rel = relocatedOrig;
  }
  static const std::vector<std::pair<uint64_t, uint64_t>> none;
  const auto& relocated = rel ? *rel : none;
  std::vector<std::pair<uint64_t, uint64_t>> out;
  for (uint64_t p = rs; p < re; p += ps) {
    const uint64_t pe = std::min(re, p + ps);
    // Is [p, pe) inside the relocated baskets? (The head is always kept: ROOT
    // reads it as one block, and a hole there sends every open to the origin.)
    auto it = std::upper_bound(relocated.begin(), relocated.end(), std::make_pair(p, UINT64_MAX));
    const bool dead = p >= kKeepHead && it != relocated.begin() && std::prev(it)->second >= pe;
    if (dead)
      continue;
    if (!out.empty() && out.back().second == p)
      out.back().second = pe;
    else
      out.emplace_back(p, pe);
  }
  return out;
}

void ColdFill::queueCommit() {
  if (commitQueued.exchange(true))
    return;
  {
    std::lock_guard<std::mutex> g(g_cmtMu);
    ++g_cmtPending;
  }
  auto self = shared_from_this();
  commitPool().post([self] {
    self->commit();
    std::lock_guard<std::mutex> g(g_cmtMu);
    if (--g_cmtPending == 0)
      g_cmtCv.notify_all();
  });
}

void ColdFill::dropRecords(const std::vector<SlotRecord>& recs, const std::string& why) {
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true))
    UCACHE_WARN("slot records for %s not kept (%s); they are converted again when read "
                "(further such cases are not reported)",
                key.key.c_str(), why.c_str());
  releaseCollected(recs);
}

void ColdFill::releaseCollected(const std::vector<SlotRecord>& recs) {
  {
    std::lock_guard<std::mutex> g(mu);
    for (const auto& r : recs) {
      auto it = info.find(r.slot);
      // Not if someone committed it meanwhile, or it was staged again since.
      if (it != info.end() && !it->second.inStore && !it->second.persist)
        forgetLocked(r.slot);
    }
  }
  if (auto store = globalStore())
    store->stats().coldReplicaDeclined.fetch_add(recs.size(), std::memory_order_relaxed);
}

// Commit what waits in memory. Under the store's lock, entries other processes
// committed meanwhile are applied first and their slots skipped -- so no slot
// is written twice -- then the rest goes to the store as one block.
void ColdFill::commit() try {
  // One commit of a file at a time, and a second never waits for the first on
  // a pool thread: the one running goes round again instead.
  // (Flag, then try again: a holder that unlocks after the flag is set sees
  // it; one that unlocked before leaves the lock to this try.)
  std::unique_lock<std::mutex> cg(commitMu, std::defer_lock);
  commitQueued.store(false);
  if (!cg.try_lock()) {
    commitAgain.store(true);
    if (!cg.try_lock())
      return;
  }
  struct Again {
    ColdFill* self;
    std::unique_lock<std::mutex>& lk;
    ~Again() {
      lk.unlock();
      if (self->commitAgain.exchange(false))
        self->queueCommit();
    }
  } again{this, cg};
  std::vector<SlotRecord> recs;
  {
    std::lock_guard<std::mutex> g(mu);
    for (uint32_t i : pending) {
      auto it = info.find(i);
      if (it == info.end())
        continue; // forgotten meanwhile
      Info& s = it->second;
      if (s.inStore) { // committed by someone meanwhile: ours is not kept
        s.persist = false;
        releaseMemLocked(s);
        continue;
      }
      if (!s.persist || state[i].load(std::memory_order_acquire) != kReady ||
          (!s.mem && s.kind != tp::ConvertedBasket::kOriginal))
        continue;
      s.persist = false; // collected once, even if the slot was staged twice
      SlotRecord r;
      r.slot = i;
      r.kind = entryKind(s.kind);
      if (r.kind != SlotEntry::kKept)
        r.bytes = *s.mem;
      recs.push_back(std::move(r));
    }
    pending.clear();
    g_pendingTotal.fetch_sub(pendingBytes, std::memory_order_relaxed);
    pendingBytes = 0;
    // Records served while there was no room to commit them go with this
    // batch, now that it is being written: out of the transient budget, kept
    // in memory until committed.
    if (!storeGone.load()) {
      for (uint32_t j : transient) {
        auto it = info.find(j);
        if (it == info.end() || !it->second.transient || !it->second.mem || it->second.inStore)
          continue;
        Info& s = it->second;
        SlotRecord r;
        r.slot = j;
        r.kind = entryKind(s.kind);
        r.bytes = *s.mem;
        auto keep = s.mem;
        releaseMemLocked(s); // out of the budget ...
        s.mem = std::move(keep); // ... still served until the commit lands
        recs.push_back(std::move(r));
      }
      transient.clear();
    }
  }
  if (recs.empty())
    return;
  if (!store || storeGone.load()) {
    releaseCollected(recs);
    return;
  }
  uint64_t bytes = 0;
  for (const auto& r : recs)
    bytes += r.bytes.size();
  auto cs = globalStore();
  if (cs && bytes > CacheStore::headroomToFloor(cs->config(), RealIO::instance())) {
    // Records are cached like any fill: make room the way a fill does, by
    // eviction (at most every 30 s per process: it scans the cache), and keep
    // them only if that found the room.
    static std::atomic<uint64_t> lastEvictUs{0};
    const uint64_t now = nowUs();
    uint64_t last = lastEvictUs.load(std::memory_order_relaxed);
    if (now - last > 30'000'000 && lastEvictUs.compare_exchange_strong(last, now))
      cs->evictNow();
    if (bytes > CacheStore::headroomToFloor(cs->config(), RealIO::instance())) {
      dropRecords(recs, "no room above the free-space floor");
      return;
    }
  }
  std::vector<SlotEntry> committed;
  int64_t n = -EIO; // test hook: the commit's write refused
  if (!commitFaultFire())
    n = store->commit(
        recs, [this](const std::vector<SlotEntry>& es) { apply(es); },
        [this](uint32_t slot) {
          std::lock_guard<std::mutex> g(mu);
          auto it = info.find(slot);
          return it != info.end() && it->second.inStore;
        },
        globalConfig().fsync != FsyncMode::kOff, committed);
  if (n == -ESTALE) {
    // Dropped or replaced (evicted, removed, another build's): this process
    // keeps serving what it holds, and commits nothing more to it.
    if (!storeGone.exchange(true))
      UCACHE_INFO("slot store for %s was removed; nothing more is committed to it",
                  key.key.c_str());
    releaseCollected(recs);
    return;
  }
  if (n < 0) {
    dropRecords(recs, std::strerror(static_cast<int>(-n)));
    return;
  }
  std::vector<std::pair<uint64_t, uint64_t>> punch;
  std::vector<SlotEntry> forgotten;
  {
    std::lock_guard<std::mutex> g(mu);
    for (const auto& e : committed) {
      auto it = info.find(e.slot);
      if (it == info.end()) { // forgotten meanwhile: its new entry makes it ready
        forgotten.push_back(e);
        continue;
      }
      Info& s = it->second;
      if (!s.inStore)
        noteSlotLocked(e, /*foreign=*/false);
      s.inStore = true;
      s.e = e;
      s.kind = basketKind(e.kind);
      // A kept basket's original is the only copy: never punched.
      if (s.fromCache && !keepOriginals && e.kind != SlotEntry::kKept)
        punch.emplace_back(slot(e.slot).origSeek, slot(e.slot).origSeek + slot(e.slot).origLen);
      s.fromCache = false;
      s.persist = false;
      releaseMemLocked(s);
    }
    for (const auto& r : recs) { // skipped: another process's record serves
      auto it = info.find(r.slot);
      if (it != info.end() && it->second.inStore) {
        it->second.persist = false;
        releaseMemLocked(it->second);
      }
    }
  }
  if (!forgotten.empty())
    apply(forgotten, /*foreign=*/false);
  // Converted baskets that touch are released as one range, so a page they
  // share goes too -- and so does a page one of them shares with a basket
  // committed earlier. The file's first 128 KiB never does.
  std::sort(punch.begin(), punch.end());
  std::vector<std::pair<uint64_t, uint64_t>> runs;
  for (const auto& [a, b] : punch) {
    if (!runs.empty() && a <= runs.back().second)
      runs.back().second = std::max(runs.back().second, b);
    else
      runs.emplace_back(a, b);
  }
  punch.clear();
  if (!runs.empty() && entry) {
    const uint64_t ps = entry->pageSize();
    std::lock_guard<std::mutex> g(mu);
    for (auto& [a, b] : runs) {
      if (a % ps && pageDeadLocked(a / ps, ps))
        a -= a % ps;
      if (b % ps && b < L.originSize && pageDeadLocked(b / ps, ps))
        b = std::min<uint64_t>(b + (ps - b % ps), L.originSize);
    }
  }
  for (auto [a, b] : runs) {
    a = std::max<uint64_t>(a, kKeepHead);
    if (a < b)
      punch.emplace_back(a, b - a);
  }
  // Outside the store's lock: accounting may evict, punching takes the byte
  // cache's own lock.
  if (cs && n > 0)
    cs->noteStoredBytes(static_cast<uint64_t>(n));
  // The converted record is the copy now: the original's whole pages go.
  if (!punch.empty() && entry)
    entry->releaseRanges(punch);
} catch (const std::exception& e) {
  UCACHE_WARN("slot store commit for %s failed (%s)", key.key.c_str(), e.what());
}


// ------------------------------------------------------------ mixed maps

namespace {
// Runs whose last handle closed with a map perhaps to be made: made at exit
// instead if the process ends first. Leaked, like the registry.
std::mutex g_mapMu;
std::condition_variable g_mapCv;
int g_mapRunning = 0; // map tasks under way: exit waits for them
std::vector<std::shared_ptr<ColdFill>>& mapWanted() {
  static auto* v = new std::vector<std::shared_ptr<ColdFill>>();
  return *v;
}
} // namespace

void ColdFill::queueMap(uint64_t delayMs) {
  if (mapQueued.exchange(true))
    return;
  auto self = shared_from_this();
  {
    std::lock_guard<std::mutex> g(g_mapMu);
    mapWanted().push_back(self);
  }
  auto task = [self] {
    {
      std::lock_guard<std::mutex> g(g_mapMu);
      auto& v = mapWanted();
      auto it = std::find(v.begin(), v.end(), self);
      if (it != v.end())
        v.erase(it);
      ++g_mapRunning;
    }
    self->mapQueued.store(false);
    self->makeMap(false);
    std::lock_guard<std::mutex> g(g_mapMu);
    if (--g_mapRunning == 0)
      g_mapCv.notify_all();
  };
  if (delayMs)
    mapPool().postAfter(delayMs, std::move(task));
  else
    mapPool().post(std::move(task));
}

bool ColdFill::makeMap(bool atExit) try {
  std::lock_guard<std::mutex> mg(mapMu);
  // This process's records first, so the map covers them.
  commit();
  std::unique_lock<std::mutex> cg(commitMu);
  struct Again { // a commit that found the lock taken left its turn to us
    ColdFill* self;
    std::unique_lock<std::mutex>& lk;
    ~Again() {
      lk.unlock();
      if (self->commitAgain.exchange(false))
        self->queueCommit();
    }
  } again{this, cg};
  return makeMapNow(atExit);
} catch (const std::exception& e) {
  UCACHE_WARN("mixed map for %s not made (%s)", key.key.c_str(), e.what());
  return false;
}

bool ColdFill::heldHere(uint64_t lo, uint64_t hi) const { return shownMapHeld(key.key, lo, hi); }

bool ColdFill::storedEntry(uint32_t i, SlotEntry& e) const {
  auto it = info.find(i);
  if (it == info.end() || !it->second.inStore)
    return false;
  e = it->second.e;
  return true;
}

void ColdFill::forEachStored(const std::function<void(uint32_t, const SlotEntry&)>& f) const {
  for (const auto& [i, s] : info)
    if (s.inStore)
      f(i, s.e);
}

void ColdFill::onMapFull() {
  if (auto cs = globalStore())
    cs->stats().slotMapFull.fetch_add(1, std::memory_order_relaxed);
}

void ColdFill::onMapMade(uint64_t storedBytes) {
  if (auto cs = globalStore()) {
    cs->stats().slotMapsMade.fetch_add(1, std::memory_order_relaxed);
    cs->noteStoredBytes(storedBytes);
  }
}

namespace {

// At exit: every record still in memory is committed, with a bounded wait. A
// hard _exit() skips this, and loses at most what the periodic checkpoint had
// not yet committed.
void commitAtExit() {
  { // at exit nothing more finishes: every record still held goes now
    std::vector<std::shared_ptr<ColdFill>> held;
    {
      std::lock_guard<std::mutex> g(g_regMu);
      for (const auto& [k, cf] : registry())
        held.push_back(cf);
    }
    for (const auto& cf : held)
      cf->appendHeldOlderThan(0);
  }
  coldCheckpoint();
  {
    std::unique_lock<std::mutex> lk(g_cmtMu);
    if (!g_cmtCv.wait_for(lk, std::chrono::minutes(2), [] { return g_cmtPending == 0; })) {
      UCACHE_WARN("exiting with %d slot-store commit(s) unfinished after 2 min", g_cmtPending);
      return;
    }
  }
  // The maps due for files this process read: it is done with them, and they
  // would otherwise wait for a later open to be made. At most 30 s, in
  // parallel, after those already being made.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  {
    std::unique_lock<std::mutex> lk(g_mapMu);
    g_mapCv.wait_until(lk, deadline, [] { return g_mapRunning == 0; });
  }
  std::vector<std::shared_ptr<ColdFill>> fills;
  {
    std::lock_guard<std::mutex> g(g_regMu);
    for (const auto& [k, cf] : registry())
      fills.push_back(cf);
  }
  {
    std::lock_guard<std::mutex> g(g_mapMu);
    fills.insert(fills.end(), mapWanted().begin(), mapWanted().end());
  }
  std::sort(fills.begin(), fills.end());
  fills.erase(std::unique(fills.begin(), fills.end()), fills.end());
  const uint64_t now = wallSeconds();
  fills.erase(std::remove_if(fills.begin(), fills.end(),
                             [now](const std::shared_ptr<ColdFill>& cf) {
                               std::lock_guard<std::mutex> g(cf->mu);
                               return cf->mapDueLocked(now, true) != 0;
                             }),
              fills.end());
  if (fills.empty())
    return;
  struct Left {
    std::mutex mu;
    std::condition_variable cv;
    size_t n = 0;
  };
  auto left = std::make_shared<Left>();
  left->n = fills.size();
  for (const auto& cf : fills)
    convertPool().post([cf, left] {
      cf->makeMap(true);
      std::lock_guard<std::mutex> g(left->mu);
      if (--left->n == 0)
        left->cv.notify_all();
    });
  std::unique_lock<std::mutex> lk(left->mu);
  if (!left->cv.wait_until(lk, deadline, [&] { return left->n == 0; }))
    UCACHE_WARN("exiting with %zu mixed map(s) not made after 30 s", left->n);
}

} // namespace

ShownLayout shownLayout(const std::string& key, uint64_t& hash) {
  std::lock_guard<std::mutex> g(g_shownMu);
  auto it = shownMap().find(key);
  if (it == shownMap().end())
    return ShownLayout::kNone;
  hash = it->second.hash;
  return it->second.layout;
}

ShownLayout noteShownLayout(const std::string& key, ShownLayout s, uint64_t hash,
                            uint64_t* winnerHash, const ColdFill* run) {
  std::lock_guard<std::mutex> g(g_shownMu);
  g_shownVersion.fetch_add(1, std::memory_order_acq_rel); // odd: changing
  auto& v = shownMap()[key];
  // Original may later become a slot layout (the original region reads the
  // same); nothing else changes once shown -- not even to another slot layout
  // of the same file. The caller learns which layout won, and must serve that
  // one.
  if (v.layout == ShownLayout::kNone || v.layout == ShownLayout::kOriginal ||
      (v.layout == s && v.hash == hash)) {
    const bool first = v.layout != s || v.hash != hash;
    v.layout = s;
    v.hash = hash;
    if (first && s == ShownLayout::kSlot && run) {
      v.slotFactor100 = run->slotFactor100;
      v.codecs = joinCodecs(run->codecs);
    }
  }
  if (winnerHash)
    *winnerHash = v.hash;
  const ShownLayout won = v.layout;
  g_shownVersion.fetch_add(1, std::memory_order_acq_rel); // even: whole again
  return won;
}

// -------------------------------------------------------------------- API

std::shared_ptr<ColdFill> coldAttach(const std::shared_ptr<HandleState>& st,
                                     const std::shared_ptr<FileEntry>& entry, const UrlKey& key,
                                     uint64_t originMtime, uint8_t cksumKind, uint32_t originCksum,
                                     AttachMode mode, uint64_t matchHash) {
  {
    std::lock_guard<std::mutex> g(g_regMu);
    auto it = registry().find(key.key);
    // Joined only if it is the same file: an entry replaced at the origin
    // meanwhile gets a run of its own.
    if (it != registry().end() && it->second->L.originSize == entry->fileSize() &&
        it->second->entry == entry &&
        (mode != AttachMode::kMatch || it->second->store->header().layoutHash == matchHash)) {
      std::lock_guard<std::mutex> g2(it->second->mu);
      ++it->second->handles;
      return it->second;
    }
  }
  std::shared_ptr<ColdFill> cf;
  try { // network reads: outside the registry lock
    cf = build(st, entry, key, originMtime, cksumKind, originCksum, mode, matchHash);
  } catch (const std::exception& e) {
    UCACHE_WARN("slot run for %s not set up (%s); the file is served as stored", key.key.c_str(),
                e.what());
    return nullptr;
  }
  if (!cf)
    return nullptr;
  // Registered after the store exists, so it runs before the store is torn down.
  static std::once_flag once;
  std::call_once(once, [] { std::atexit(commitAtExit); });
  std::lock_guard<std::mutex> g(g_regMu);
  auto [it, inserted] = registry().emplace(key.key, cf);
  if (!inserted && (it->second->L.originSize != entry->fileSize() || it->second->entry != entry ||
                    (mode == AttachMode::kMatch &&
                     it->second->store->header().layoutHash != matchHash)))
    it->second = cf; // the old run keeps serving its own handles
  std::lock_guard<std::mutex> g2(it->second->mu);
  ++it->second->handles; // a racing handle built it first: join that one, drop ours
  return it->second;
}

void coldDetach(const std::shared_ptr<ColdFill>& cf) {
  if (!cf)
    return;
  bool last = false;
  {
    std::lock_guard<std::mutex> g(g_regMu);
    std::lock_guard<std::mutex> g2(cf->mu);
    if (--cf->handles == 0) {
      last = true;
      auto it = registry().find(cf->key.key);
      if (it != registry().end() && it->second == cf)
        registry().erase(it);
    }
  }
  if (last) {
    // The window of what this process used counts from its last close.
    noteSlotLayout(*cf, Use::kRelease);
    for (const auto& [a, b] : shownMapRanges(cf->key.key, /*held=*/false))
      noteUse(*cf, a, b, Use::kRelease);
    cf->queueCommit(); // what this process converted goes to the store now
    bool maps;
    {
      std::lock_guard<std::mutex> g(cf->mu);
      maps = !cf->mapsOff && !cf->mapsImpossible;
    }
    if (maps) // decided once that commit is in, unless the file is opened again
      cf->queueMap(mapTiming().closeDelayMs);
  }
}

void coldCheckpoint() {
  std::vector<std::shared_ptr<ColdFill>> all;
  {
    std::lock_guard<std::mutex> g(g_regMu);
    for (const auto& [k, cf] : registry())
      all.push_back(cf);
  }
  const uint64_t now = wallSeconds();
  const uint64_t stalledUs = static_cast<uint64_t>(std::max(1, globalConfig().metaFlushSeconds)) * 1000000;
  for (const auto& cf : all) {
    cf->appendHeldOlderThan(stalledUs); // a request stalled this long gives up its records
    // Still open here: the layout, and the maps a handle holds, are in use.
    noteSlotLayout(*cf, Use::kHold);
    for (const auto& [a, b] : shownMapRanges(cf->key.key, /*held=*/true))
      noteUse(*cf, a, b, Use::kHold);
    cf->queueCommit();
    bool due;
    {
      std::lock_guard<std::mutex> g(cf->mu);
      due = cf->mapDueLocked(now, false) == 0;
    }
    if (due)
      cf->queueMap(0);
  }
}

void coldForkPrepare() {
  // Copied under the lock, which is released before returning: see g_shownVersion.
  auto* copy = [] {
    std::lock_guard<std::mutex> g(g_shownMu);
    return new ShownMap(shownMap());
  }();
  g_shownCopy = copy;
  // A child's handler may use these; a first use in progress in another thread
  // at the fork would leave the child waiting on it for good.
  (void)registry();
}

void coldForkParent() {
  delete g_shownCopy; // only a child could need it, and has its own
  g_shownCopy = nullptr;
}

void coldAfterForkChild() {
  new (&g_shownMu) std::mutex; // a parent thread may have held it
  if (g_shownVersion.load(std::memory_order_acquire) % 2 != 0 && g_shownCopy) {
    g_shown = g_shownCopy; // the inherited map was mid-change: use prepare's copy
    g_shownCopy = nullptr;
    g_shownVersion.fetch_add(1, std::memory_order_acq_rel);
  }
  delete g_shownCopy; // not needed: the inherited map is whole
  g_shownCopy = nullptr;
  // Left behind untouched: a ColdFill's destructor would commit, flock and
  // write a store the parent is still using.
  auto* left = new std::unordered_map<std::string, std::shared_ptr<ColdFill>>(); // leaked
  left->swap(registry());
  new (&g_regMu) std::mutex;
  auto* leftMaps = new std::vector<std::shared_ptr<ColdFill>>(); // leaked, as above
  leftMaps->swap(mapWanted());
  new (&g_mapMu) std::mutex;
  new (&g_mapCv) std::condition_variable;
  g_mapRunning = 0; // the parent's to wait for
  new (&g_cmtMu) std::mutex;
  new (&g_cmtCv) std::condition_variable;
  g_cmtPending = 0; // the parent's commits are the parent's to wait for at exit
  g_pendingTotal.store(0, std::memory_order_relaxed);
  g_transientTotal.store(0, std::memory_order_relaxed);
}

uint64_t coldVirtualSize(const ColdFill& cf) { return cf.L.virtualSize; }
uint64_t coldShownSize(const ColdFill& cf, const ColdMap* map) {
  return map && map->compact() ? map->end() : cf.L.virtualSize;
}
uint64_t coldAddressEnd(ColdFill& cf) {
  uint64_t end = cf.L.virtualSize;
  std::lock_guard<std::mutex> g(cf.mu);
  for (const auto& m : cf.maps)
    if (m.head.compact())
      end = std::max(end, m.head.rangeLo + m.head.rangeLen);
  return end;
}
uint64_t coldLayoutHash(const ColdFill& cf) { return cf.store->header().layoutHash; }

std::shared_ptr<const ColdMap> coldPickMap(const std::shared_ptr<ColdFill>& cf) {
  if (!cf || cf->mapsOff)
    return nullptr;
  cf->sync(); // maps made since the run was set up (never waits)
  uint64_t seq = 0;
  bool due, outOfMaps;
  {
    std::lock_guard<std::mutex> g(cf->mu);
    if (!cf->maps.empty())
      seq = cf->maps.back().seq;
    const uint64_t now = wallSeconds();
    const int64_t at = cf->mapDueLocked(now, false);
    due = at == 0;
    // What was converted since the newest map is worth a map, and none can
    // be made (as many compact maps are in use as may be): the slot layout,
    // which states everything converted, rather than an outdated map.
    outOfMaps = at > 0 && cf->mapsFullUntilS > now && !cf->maps.empty() &&
                cf->maps.back().head.compact();
  }
  if (due) // for later opens: this one is shown what exists now
    cf->queueMap(0);
  if (outOfMaps)
    return nullptr;
  auto m = seq ? cf->mapBySeq(seq) : nullptr;
  if (m) {
    noteShownMap(cf->key.key, m);
    if (auto cs = globalStore())
      cs->stats().slotMapOpens.fetch_add(1, std::memory_order_relaxed);
  }
  return m;
}

bool coldPrepare(ColdFill& cf, const ChunkList& chunks) {
  std::vector<std::pair<uint64_t, uint64_t>> r;
  r.reserve(chunks.size());
  for (const auto& c : chunks)
    r.emplace_back(c.offset, c.length);
  return cf.slots.prepareRanges(r);
}

bool coldPrepare(ColdFill& cf, uint64_t off, uint64_t len) {
  return cf.slots.prepareRanges({{off, len}});
}

namespace {
// The records compact maps state in [a, b), in address order: f(record's
// address, record, its slot) until f returns false. False when a map there
// cannot be read or its slots decoded.
bool forCompactRecords(ColdFill& cf, uint64_t a, uint64_t b,
                       const std::function<bool(uint64_t, uint64_t, const ColdMap::Rec&,
                                                const tp::FillSlot&)>& f,
                       bool* metaWhole = nullptr, bool* metaTouched = nullptr) {
  std::vector<uint64_t> seqs;
  {
    std::lock_guard<std::mutex> g(cf.mu);
    for (const auto& m : cf.maps)
      if (m.head.compact() && m.head.rangeLo < b && a < m.head.rangeLo + m.head.rangeLen)
        seqs.push_back(m.seq);
  }
  for (uint64_t seq : seqs) {
    auto m = cf.mapBySeq(seq);
    if (!m)
      return false;
    if (m->metaInRange() && a < m->rangeLo + m->meta.size() && m->rangeLo < b) {
      // its own page list and footer: they stand for the original's
      if (metaTouched)
        *metaTouched = true;
      if (metaWhole && (a > m->rangeLo || b < m->rangeLo + m->meta.size()))
        *metaWhole = false;
    }
    const auto& R = m->recs;
    auto it = std::upper_bound(R.begin(), R.end(), a > m->rangeLo ? a - m->rangeLo : 0,
                               [](uint64_t v, const ColdMap::Rec& r) { return v < r.off; });
    if (it != R.begin())
      --it;
    std::vector<uint32_t> idx;
    for (auto j = it; j != R.end() && m->rangeLo + j->off < b; ++j)
      idx.push_back(j->slot);
    if (!cf.slots.prepareSlots(idx))
      return false;
    for (; it != R.end() && m->rangeLo + it->off < b; ++it)
      if (m->rangeLo + it->off + m->extent(*it) > a &&
          !f(m->rangeLo + it->off, m->extent(*it), *it, cf.slot(it->slot)))
        return true;
  }
  return true;
}
} // namespace

void coldOriginRanges(ColdFill& cf, uint64_t off, uint64_t len,
                      std::vector<std::pair<uint64_t, uint64_t>>& out, const ColdMap* map) {
  const tp::FillLayout& L = cf.L;
  const uint64_t end = off + len;
  if (end > L.virtualSize) { // a compact map's records are their baskets
    bool meta = false;
    forCompactRecords(
        cf, std::max(off, L.virtualSize), end,
        [&out](uint64_t, uint64_t, const ColdMap::Rec&, const tp::FillSlot& s) {
          out.emplace_back(s.origSeek, s.origLen);
          return true;
        },
        nullptr, &meta);
    if (meta)
      for (const auto& r : cf.metaOrigin)
        out.push_back(r);
    if (off >= L.virtualSize)
      return;
  }
  if (off < L.originSize)
    out.emplace_back(off, std::min(end, L.originSize) - off);
  const uint64_t metaEnd = L.metaSeek + L.metaRecord.size();
  const bool inMap = map && off < map->metaSeek + map->meta.size() && end > map->metaSeek;
  if ((off < metaEnd && end > L.metaSeek) || inMap)
    for (const auto& r : cf.metaOrigin)
      out.push_back(r);
  if (end > L.slotsBegin)
    cf.slots.forEach(std::max(off, L.slotsBegin), end, [&out](uint32_t, const tp::FillSlot& s) {
      out.emplace_back(s.origSeek, s.origLen);
      return true;
    });
}

bool coldExactOriginRanges(ColdFill& cf, uint64_t off, uint64_t len,
                           std::vector<std::pair<uint64_t, uint64_t>>& out, const ColdMap* map) {
  if (off + len > cf.L.virtualSize) {
    // Past map 1's slots: a compact map's record read whole is its basket; a
    // guard carries nothing; part of a record has no original offset.
    const uint64_t a = std::max(off, cf.L.virtualSize), b = off + len;
    bool whole = true, metaWhole = true, meta = false;
    if (!forCompactRecords(
            cf, a, b,
            [&](uint64_t at, uint64_t ext, const ColdMap::Rec&, const tp::FillSlot& s) {
              if (at < a || at + ext > b)
                return whole = false;
              out.emplace_back(s.origSeek, s.origLen);
              return true;
            },
            &metaWhole, &meta) ||
        !whole || (meta && !metaWhole))
      return false;
    if (meta)
      for (const auto& r : cf.metaOrigin)
        out.push_back(r);
    if (off >= cf.L.virtualSize)
      return true;
    len = cf.L.virtualSize - off;
  }
  const tp::SlotsIn in = [&cf](uint64_t a, uint64_t b,
                              const std::function<bool(uint32_t, const tp::FillSlot&)>& f) {
    return cf.slots.forEach(a, b, f);
  };
  if (cf.mapsOff)
    return tp::exactOriginRanges(cf.L, in, cf.metaOrigin, off, len, out);
  // A slot read to its record's end is its whole basket: what a mixed map
  // states, and what a reader given one (here, or in another process) reads.
  // Decoded first, so that nothing is read from disk under the lock.
  cf.slots.prepare(std::max(off, cf.L.slotsBegin), off + len);
  std::lock_guard<std::mutex> g(cf.mu);
  const std::function<uint32_t(uint32_t)> real = [&cf](uint32_t i) {
    return cf.committedLenLocked(i, cf.slot(i));
  };
  return tp::exactOriginRanges(cf.L, in, cf.metaOrigin, off, len, out, &real,
                               map ? std::make_pair(map->metaSeek, uint64_t(map->meta.size()))
                                   : std::pair<uint64_t, uint64_t>{0, 0});
}

void coldServe(std::shared_ptr<HandleState> st, std::shared_ptr<FileEntry> entry,
               std::shared_ptr<ColdFill> cf, ChunkList chunks, bool isVRead,
               ResponseHandler* user) {
  noteWaitTier(user, ReaderWait::kSlots);
  auto req = std::make_shared<ColdRequest>();
  req->st = std::move(st);
  req->entry = std::move(entry);
  req->cf = std::move(cf);
  req->chunks = std::move(chunks);
  req->isVRead = isVRead;
  req->user = user;
  req->t0 = nowUs();
  serveRequest(req);
}

} // namespace ucache
