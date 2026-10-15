#include "ReadRule.h"

#include "Executor.h"
#include "FetchCheck.h"
#include "Log.h"
#include "OriginSource.h"
#include "ParseEnd.h"
#include "RNTupleMeta.h"
#include "TreeMeta.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <new>
#include <optional>
#include <thread>
#include <unordered_map>

namespace ucache {

namespace tp = transpose;

namespace {

// Every file this process has asked the question of, for the process's life:
// a decision is never taken twice. Bounded like the other per-key maps; past
// the bound a new file simply has no rule.
constexpr size_t kMaxRules = 200000;

std::mutex& regMu() {
  static auto* m = new std::mutex;
  return *m;
}
std::unordered_map<std::string, std::shared_ptr<ReadRule>>& registry() {
  static auto* r = new std::unordered_map<std::string, std::shared_ptr<ReadRule>>();
  return *r;
}

// Parsed maps held by files still undecided: a reader that only ever reads one
// branch at a time never decides, and must not keep every file's map for the
// life of the process. Past the bound the oldest undecided file is cached as
// usual, for good, and its map freed.
constexpr size_t kMaxMaps = 64;
std::mutex& mapsMu() {
  static auto* m = new std::mutex;
  return *m;
}
std::deque<std::weak_ptr<ReadRule>>& maps() {
  static auto* d = new std::deque<std::weak_ptr<ReadRule>>();
  return *d;
}

std::atomic<bool> g_warned{false};

// The key-seek tables (keySeeksFor), oldest first, one per file entry: a file
// changed at the origin has a new entry, so a table never outlives the bytes it
// was parsed from. The lock is never held across a parse, so no handle waits
// for another's. Lock order: a rule's lock, then this one.
constexpr size_t kMaxKeySeekTables = 32;
// After a parse that a failed origin read stopped, no other for this long: a
// failing origin costs one parse attempt a second, not one a miss. The same
// for the rule's own parse with a limit of 0 (ReadRule::observe).
constexpr uint64_t kKeySeekRetryUs = 1'000'000;
struct KeySeekTable {
  std::weak_ptr<FileEntry> entry; // compared by identity, never locked
  bool parsing = false;
  uint64_t failedUs = 0; // when a failed origin read last stopped a parse
  uint64_t badUs = 0;    // when a parse last made a map that shows its copy bad
  std::shared_ptr<const KeyMap> seeks; // null until built
  // Requests of other handles answered while a parse runs (keySeeksBuilt): posted to the
  // executor when it ends, whatever it found.
  std::vector<std::function<void()>> waiters;
};
// The waiters of a parse that ended, each posted to the executor (none runs on the parsing
// thread, which may be a client callback thread). A waiter that cannot be posted runs here.
void postWaiter(std::function<void()>& f) {
  try {
    Executor::instance().post(f);
  } catch (...) {
    f();
  }
}
void postWaiters(std::vector<std::function<void()>>& w) {
  for (auto& f : w)
    postWaiter(f);
  w.clear();
}
// The parse of a file's records for its map. Test hook: UCACHE_TEST_KEYMAP_THROW makes it throw,
// as a parse that runs out of memory would, so that a test sees its waiters still answered.
tp::FileMeta parseRecords(OriginSource& src, int64_t size) {
  static const bool throwIt = std::getenv("UCACHE_TEST_KEYMAP_THROW") != nullptr;
  if (throwIt)
    throw std::bad_alloc();
  return tp::parseReaderTree(src, size);
}
std::mutex& tablesMu() {
  static auto* m = new std::mutex;
  return *m;
}
std::deque<std::shared_ptr<KeySeekTable>>& tables() {
  static auto* d = new std::deque<std::shared_ptr<KeySeekTable>>();
  return *d;
}

} // namespace

// The one place a table is built: from `fm` when the caller parsed the tree
// already (ReadRule::observe), else by one parse through OriginSource, if no
// parse of the file is running. A parse that failed on the file's content
// (not ROOT, no tree: RNTuple) leaves the table empty for good; one that a
// failed origin read stopped leaves it unbuilt, for a miss after the retry
// interval to try again.
KeyMap keyMapOf(const tp::FileMeta& fm, uint64_t fileSize, const std::string& server) {
  KeyMap m;
  if (fm.error.empty())
    tp::basketMap(fm, m.seeks, m.bytes);
  m.bad = tp::basketMapBad(fm, m.seeks, m.bytes, fileSize);
  if (!m.bad.empty()) {
    m.server = server;
    m.badSpans = tp::treeSpans(fm, m.seeks, m.bytes);
    if (m.badSpans.empty()) // nothing to hold against: no claim
      m.bad.clear();
  }
  return m;
}

std::shared_ptr<const KeyMap>
keySeeksFor(const std::shared_ptr<HandleState>& st, const std::shared_ptr<FileEntry>& entry,
            XrdTimeout timeout, const tp::FileMeta* fm, const std::string& fmServer,
            const std::string* current, uint64_t startUs) {
  std::unique_lock<std::mutex> g(tablesMu());
  std::shared_ptr<KeySeekTable> t;
  for (const auto& x : tables())
    if (!x->entry.owner_before(entry) && !entry.owner_before(x->entry))
      t = x;
  if (!t) {
    t = std::make_shared<KeySeekTable>();
    t->entry = entry;
    tables().push_back(t);
    if (tables().size() > kMaxKeySeekTables)
      tables().pop_front();
  }
  // A map that shows its copy bad is parsed again for another copy, at most once a second.
  const bool again = t->seeks && !t->seeks->bad.empty() && current &&
                     *current != t->seeks->server &&
                     (!t->badUs || nowUs() - t->badUs >= kKeySeekRetryUs);
  if (!fm && ((t->seeks && !again) || t->parsing ||
              (t->failedUs && nowUs() - t->failedUs < kKeySeekRetryUs)))
    return t->seeks;
  OriginSource src;
  tp::FileMeta own;
  // A parse this call makes ends, however it leaves (an exception included), with the table no
  // longer parsing and the requests that waited for it posted (ParseEnd).
  std::optional<ParseEnd<KeySeekTable>> end;
  if (!fm) {
    t->parsing = true;
    end.emplace(*t, g, &postWaiter);
    g.unlock();
    // Test hook: the parse held this long before its first read, so that another handle's miss
    // is answered while it runs (that answer waits for the map, keySeeksBuilt).
    static const long holdMs = [] {
      const char* v = std::getenv("UCACHE_TEST_KEYMAP_DELAY_MS");
      return v ? std::strtol(v, nullptr, 10) : 0L;
    }();
    if (holdMs > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    src.st = st;
    src.entry = entry;
    src.timeout = timeout;
    src.mayMove = st != nullptr; // the parse runs on the reader's thread (armKeyCheck)
    src.startUs = startUs;       // the request's time, the parse's included
    own = parseRecords(src, static_cast<int64_t>(entry->fileSize()));
    g.lock();
    if (src.readFailed) {
      t->failedUs = nowUs();
      auto seeks = t->seeks;
      end->finish();
      return seeks;
    }
    fm = &own;
  }
  // The copy the records came from: the server of the parse's reads from the origin. When the
  // cache answered any of them, none: the cache's records may be another copy's (an entry by
  // logical name serves every URL of the file, and a process that checked nothing may have filled
  // it), and a map made of them makes no claim against a server. Its caller drops the entry, when
  // the map shows the records bad, and the map is made again from the origin at once (no wait
  // between parses).
  std::string from = fm == &own ? (src.fromCache ? std::string() : src.fetchedFrom) : fmServer;
  if (from.empty() && current && fm != &own)
    from = *current;
  auto m = std::make_shared<const KeyMap>(keyMapOf(*fm, entry->fileSize(), from));
  t->badUs = m->bad.empty() || m->server.empty() ? 0 : nowUs();
  t->seeks = m;
  if (end) {
    end->finish();
    return m;
  }
  std::vector<std::function<void()>> w;
  w.swap(t->waiters);
  g.unlock();
  postWaiters(w);
  return m;
}

tp::FetchVerdict fetchVerdict(const KeyMap& m, const std::string& server,
                              std::vector<FetchPiece> pieces, const Ranges* reach) {
  if (!m.bad.empty() && (m.server.empty() || server != m.server))
    return tp::FetchVerdict();
  return tp::checkPieces(m.seeks, m.bytes, std::move(pieces), /*data=*/true,
                         m.bad.empty() ? nullptr : &m.badSpans, m.bad, reach);
}

void fetchVerdictAsync(const std::shared_ptr<const KeyMap>& m, const std::string& server,
                       std::vector<FetchPiece> pieces, std::shared_ptr<const Ranges> reach,
                       std::function<void(tp::FetchVerdict)> done) {
  if (!m->bad.empty() && (m->server.empty() || server != m->server)) {
    done(tp::FetchVerdict());
    return;
  }
  checkPiecesOn(decodePool(), m, m->seeks, m->bytes, std::move(pieces), /*data=*/true,
                m->bad.empty() ? nullptr : &m->badSpans, m->bad, std::move(reach), std::move(done));
}

bool fetchNeedsWork(const KeyMap& m, const std::string& server, std::vector<FetchPiece> pieces) {
  if (!m.bad.empty() && (m.server.empty() || server != m.server))
    return false;
  return tp::piecesNeedWork(m.seeks, m.bytes, std::move(pieces));
}

KeySeeksBuilt keySeeksBuilt(const std::shared_ptr<FileEntry>& entry, std::function<void()>* then) {
  KeySeeksBuilt b;
  if (!entry)
    return b;
  std::lock_guard<std::mutex> g(tablesMu());
  for (const auto& x : tables())
    if (!x->entry.owner_before(entry) && !entry.owner_before(x->entry)) {
      b.map = x->seeks;
      if (!b.map && x->parsing && then && *then) {
        x->waiters.push_back(std::move(*then));
        *then = nullptr;
        b.parked = true;
      }
      break;
    }
  return b;
}

void readRuleBeforeFork() {
  // The child's handler uses these: a first use in progress in another thread
  // at the fork would leave the child waiting on it for good.
  (void)regMu();
  (void)registry();
  (void)mapsMu();
  (void)maps();
  (void)tablesMu();
  (void)tables();
}

void readRuleAfterForkChild() {
  // Moved aside untouched: a parent thread may have been changing them.
  auto* rules = new std::unordered_map<std::string, std::shared_ptr<ReadRule>>(); // leaked
  rules->swap(registry());
  auto* held = new std::deque<std::weak_ptr<ReadRule>>(); // leaked
  held->swap(maps());
  auto* kept = new std::deque<std::shared_ptr<KeySeekTable>>(); // leaked
  kept->swap(tables());
  new (&regMu()) std::mutex;
  new (&mapsMu()) std::mutex;
  new (&tablesMu()) std::mutex;
}

ReadRule::~ReadRule() = default;

void ReadRule::dropMap() {
  std::unique_lock<std::mutex> g(mu_, std::try_to_lock);
  if (!g.owns_lock())
    return; // in use: freed at its decision
  if (map_ && state() == kUndecided) {
    UCACHE_DEBUG("read rule: %s undecided while %zu other files were read; cached as usual",
                 key_.c_str(), kMaxMaps);
    state_.store(kCache, std::memory_order_release);
  }
  map_.reset();
  seen_.clear();
  seen_.shrink_to_fit();
}

std::shared_ptr<ReadRule> ReadRule::forFile(const std::string& key, uint64_t originSize) {
  const int limit = globalConfig().maxReadFraction;
  if (limit >= 100 || (limit > 0 && originSize < kMinBytes)) // 0: files of any size
    return nullptr;
  std::lock_guard<std::mutex> g(regMu());
  auto& r = registry();
  auto it = r.find(key);
  if (it != r.end() && it->second->size_ == originSize)
    return it->second;
  if (it == r.end() && r.size() >= kMaxRules)
    return nullptr;
  auto rule = std::make_shared<ReadRule>(key, originSize, limit);
  r[key] = rule; // a file of another size (it changed at the origin) decides again
  return rule;
}

bool readRuleDirect(const std::string& key) {
  if (globalConfig().maxReadFraction == 0) // nothing a file converts would be kept
    return true;
  std::lock_guard<std::mutex> g(regMu());
  auto it = registry().find(key);
  return it != registry().end() && it->second->direct();
}

ReadRule::State ReadRule::observe(const std::vector<std::pair<uint64_t, uint64_t>>& ranges,
                                  OriginSource& src) {
  State s = state();
  if (s != kUndecided)
    return s;
  // Waited for, not skipped: the requests of one fill arrive together, and a
  // request not seen would leave its branches out of the share. The holder
  // parses once (the file's records, normally in the byte cache already) and
  // is otherwise a few lookups. With adaptive_federation on, a read of the
  // parse that fails at its copy may move the handle (parseReadMoves) while
  // mu_ is held: the file's other first requests wait for that too, as for the
  // parse's reads; the move takes no lock of the rule's. A request on the pool
  // that serves hits never waits for it: when another thread holds mu_ it is
  // not seen (served as the rule stands, undecided: cached as usual), so a
  // parse waiting for a move never holds those threads.
  std::unique_lock<std::mutex> g(mu_, std::defer_lock);
  if (Executor::instance().runsThisThread()) {
    if (!g.try_lock())
      return kUndecided;
  } else {
    g.lock();
  }
  s = state();
  if (s != kUndecided)
    return s;
  if (!parsed_) {
    // With 0, a parse that a failed origin read stopped is made again a second later at the
    // earliest: until then a request is served as undecided (read directly, nothing kept) without
    // first waiting for another attempt, so a failing origin does not double each request's wait.
    if (limit_ == 0 && failedUs_ && nowUs() - failedUs_ < kKeySeekRetryUs)
      return kUndecided;
    parsed_ = true;
    tp::FileMeta fm = tp::parseReaderTree(src, static_cast<int64_t>(size_));
    if (limit_ == 0 && src.readFailed && !fm.error.empty()) {
      // Stopped by a failed origin read, not by what the file is: undecided (nothing kept), and
      // parsed again at a later request that needs the origin.
      parsed_ = false;
      failedUs_ = nowUs();
      UCACHE_DEBUG("read rule: %s: a read of its records failed; read directly, its records read "
                   "again in a second at the earliest",
                   key_.c_str());
      return kUndecided;
    }
    // The server of the parse's reads from the origin; none when the cache answered any of them
    // (they may be another copy's: no claim against a server, see keySeeksFor).
    const std::string from = src.fromCache ? std::string() : src.fetchedFrom;
    if (fm.recordFailed && globalConfig().checkOriginIntegrity) // the copy shown bad, for the check
      keySeeksFor(src.st, src.entry, 0, &fm, from);
    if (fm.error.empty()) {
      map_ = std::make_unique<tp::ReadMap>(tp::ReadMap::fromTree(fm));
      if (globalConfig().checkOriginIntegrity) // the key-position check's table, from this parse
        keySeeksFor(src.st, src.entry, 0, &fm, from);
    }
    // A ROOT file that holds neither a TTree nor an RNTuple (no tree, and the RNTuple's parse found
    // none, or could not look): no rule, whatever the limit.
    const bool noTree = fm.error.find("not found") != std::string::npos;
    bool neither = noTree;
    std::string why = fm.error;
    if (noTree && src.fetchHead()) {
      tp::RNTupleMeta rm = tp::parseRNTuple(src, static_cast<int64_t>(size_), "");
      if (rm.error.empty())
        map_ = std::make_unique<tp::ReadMap>(tp::ReadMap::fromRNTuple(rm));
      neither = rm.error.find("not found") != std::string::npos;
      if (!rm.error.empty() && !neither)
        why = "RNTuple: " + rm.error;
    }
    if ((!map_ || map_->empty()) && limit_ == 0 && src.readFailed) {
      map_.reset(); // the RNTuple's parse stopped by a failed read: as above
      parsed_ = false;
      failedUs_ = nowUs();
      return kUndecided;
    }
    if ((!map_ || map_->empty()) && limit_ == 0 && !fm.error.empty() &&
        fm.error != "not a ROOT file" && !neither) {
      // A ROOT file whose records cannot be mapped: its tree's record failed its own check (a
      // damaged copy: ROOT cannot read the tree from it either), or uses a layout the walk does
      // not support. With 0 it is read directly WHOLE: where its data lies is not known, so
      // nothing it reads is kept (what this parse read is), from every copy it is read from.
      map_.reset();
      runs_.assign(1, {0, size_});
      state_.store(kDirect, std::memory_order_release);
      if (src.st && src.st->store)
        src.st->store->stats().directReadFiles.fetch_add(1, std::memory_order_relaxed);
      UCACHE_INFO("%s: max_read_fraction = 0: its records cannot be mapped (%s); read straight "
                  "from the origin, nothing it reads cached",
                  key_.c_str(), why.c_str());
      return kDirect;
    }
    if (!map_ || map_->empty()) {
      UCACHE_DEBUG("read rule: %s has no basket or page map (%s); cached as usual", key_.c_str(),
                   fm.error.empty() ? "no units" : why.c_str());
      map_.reset();
      state_.store(kNoRule, std::memory_order_release);
      return kNoRule;
    }
    if (limit_ == 0) { // every such file is read directly: decided now, whatever is asked
      runs_ = map_->dataRuns();
      const bool rnt = map_->isRNTuple();
      map_.reset();
      state_.store(kDirect, std::memory_order_release);
      if (src.st && src.st->store)
        src.st->store->stats().directReadFiles.fetch_add(1, std::memory_order_relaxed);
      UCACHE_INFO("%s: max_read_fraction = 0: read straight from the origin, its data (%s) "
                  "not cached",
                  key_.c_str(), rnt ? "RNTuple pages" : "TTree baskets");
      return kDirect;
    }
    holdMap();
  }
  // The branches read so far, weighed (ReadMap::step): direct as soon as they
  // hold more than the limit, cached once the reader comes back to them.
  tp::ReadMap::Share sh;
  const int d = map_->step(seen_, ranges, limit_, sh);
  if (d < 0)
    return kUndecided;
  seen_.clear();
  seen_.shrink_to_fit();
  const double pct = sh.total ? 100.0 * static_cast<double>(sh.asked) / sh.total : 0.0;
  if (d == 0) {
    UCACHE_DEBUG("read rule: %s: read %u %s holding %.1f%% of its data; cached", key_.c_str(),
                 sh.groups, map_->isRNTuple() ? "columns" : "branches", pct);
    map_.reset();
    state_.store(kCache, std::memory_order_release);
    return kCache;
  }
  runs_ = map_->dataRuns();
  const bool rnt = map_->isRNTuple();
  map_.reset();
  state_.store(kDirect, std::memory_order_release);
  if (src.st && src.st->store)
    src.st->store->stats().directReadFiles.fetch_add(1, std::memory_order_relaxed);
  if (!g_warned.exchange(true))
    UCACHE_WARN("%s: this job reads %u %s holding %.1f%% of the file's data, more than "
                "max_read_fraction = %d%%; it is read straight from the origin and what it "
                "fetches is not cached (raise max_read_fraction to cache such files; further "
                "files are counted in `ucache stats`, not reported)",
                key_.c_str(), sh.groups, rnt ? "columns" : "branches", pct, limit_);
  else
    UCACHE_INFO("%s: read %u %s holding %.1f%% of its data; read straight from the origin",
                key_.c_str(), sh.groups, rnt ? "columns" : "branches", pct);
  return kDirect;
}

void ReadRule::holdMap() {
  std::shared_ptr<ReadRule> oldest;
  {
    std::lock_guard<std::mutex> g(mapsMu());
    auto& d = maps();
    d.push_back(weak_from_this());
    if (d.size() > kMaxMaps) {
      oldest = d.front().lock();
      d.pop_front();
    }
  }
  if (oldest && oldest.get() != this)
    oldest->dropMap(); // never waits: another rule's lock is only tried
}

bool ReadRule::readsData(uint64_t off, uint64_t len) const {
  if (len == 0)
    return false;
  const State s = state();
  if (s == kUndecided && limit_ == 0) // no map yet: every byte counts as data
    return true;
  if (s != kDirect)
    return false;
  const uint64_t end = off + len;
  auto it = std::upper_bound(runs_.begin(), runs_.end(), std::pair<uint64_t, uint64_t>(off, ~uint64_t{0}));
  if (it != runs_.begin() && std::prev(it)->second > off)
    return true;
  return it != runs_.end() && it->first < end;
}

} // namespace ucache
