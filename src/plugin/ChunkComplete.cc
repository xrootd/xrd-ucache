#include "ChunkComplete.h"

#include "Config.h"
#include "Executor.h"
#include "FileEntry.h"
#include "Log.h"
#include "OriginSource.h"
#include "PluginSupport.h"
#include "ReadMap.h"
#include "ReadRounding.h"
#include "ReadRule.h"
#include "StoreMaps.h"
#include "UCacheFile.h"

#include <XrdCl/XrdClFile.hh>

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ucache {

namespace tp = transpose;

namespace {

constexpr size_t kMaxTables = 64; // files whose tables are kept; the oldest go first
constexpr int kMaxParses = 8;     // parses of one file's records: the misses that start
                                  // nowhere known look again, this often at most ...
constexpr uint64_t kParseIntervalUs = 2'000'000; // ... and no more often than this
constexpr int kMaxFails = 32; // looks that found no records or no new branch, tried
                              // again at the next misses
constexpr uint64_t kMaxCompleteBytes = 64ull << 20; // fetched beyond one miss at most
constexpr size_t kMaxReadvElems = 1024;             // the vector read's element cap

uint64_t nowMicros() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

// One file's table: the baskets of the branches its readers have asked for, by position, and the
// tree's cluster starts. Guarded by mu.
struct FileTable {
  std::weak_ptr<FileEntry> entry;
  std::mutex mu;
  bool none = false; // no tree, or no cluster boundaries: never looked at again
  int parses = 0, fails = 0;
  uint64_t lastParseUs = 0; // of the last parse that found the records
  std::vector<int64_t> clusterStart;
  struct Branch {
    std::vector<int64_t> seek, entry; // per basket; entry: [writeBasket + 1]
    std::vector<int32_t> bytes;
  };
  std::unordered_map<uint32_t, Branch> kept;                // by the record's branch index
  std::map<uint64_t, std::pair<uint32_t, uint32_t>> bySeek; // kept baskets: branch, basket
};

std::mutex& tablesMu() {
  static auto* m = new std::mutex; // leaked: executor tasks may run during teardown
  return *m;
}
std::deque<std::shared_ptr<FileTable>>& tables() {
  static auto* t = new std::deque<std::shared_ptr<FileTable>>();
  return *t;
}

std::shared_ptr<FileTable> tableFor(const std::shared_ptr<FileEntry>& entry) {
  std::lock_guard<std::mutex> g(tablesMu());
  auto& ts = tables();
  ts.erase(std::remove_if(ts.begin(), ts.end(), [](const auto& t) { return t->entry.expired(); }),
           ts.end());
  for (auto it = ts.begin(); it != ts.end(); ++it) {
    auto e = (*it)->entry.lock();
    if (e == entry) {
      auto t = *it;
      ts.erase(it);
      ts.push_front(t);
      return t;
    }
  }
  auto t = std::make_shared<FileTable>();
  t->entry = entry;
  ts.push_front(t);
  while (ts.size() > kMaxTables)
    ts.pop_back();
  return t;
}

// The branches the record says are in a codec the sweep converts (a setting that names none counts
// as one: the sweep decides from the baskets themselves).
bool converted(const tp::BranchInfo& b) {
  const std::string c = tp::codecOfSetting(b.compress, -1);
  if (c.empty())
    return true;
  const auto& codecs = globalConfig().recompressCodecs;
  return std::find(codecs.begin(), codecs.end(), c) != codecs.end();
}

// Parse the file's records -- from the byte cache, or from the origin through `st` for what it
// does not hold yet (a miss's pages are written after it is answered), kept -- and learn the
// branches whose baskets start at `starts`. Under t.mu.
void learn(FileTable& t, const std::shared_ptr<HandleState>& st,
           const std::shared_ptr<FileEntry>& entry, const std::vector<uint64_t>& starts) {
  OriginSource src;
  src.st = st;
  src.entry = entry;
  src.timeout = st->originTimeout(0);
  src.mayMove = false; // never waits for a move: it may run on the executor
  tp::FileMeta fm = tp::parseReaderTree(src, static_cast<int64_t>(entry->fileSize()));
  if (!fm.error.empty()) {
    ++t.fails;
    if (fm.error.find("not found") != std::string::npos)
      t.none = true; // no tree: nothing to complete (an RNTuple reads its clusters whole)
    UCACHE_DEBUG("complete_chunks: %s: no basket map yet (%s)%s", entry->key().key.c_str(),
                 fm.error.c_str(), t.none ? "; not looked at again" : "");
    return; // else the records may not all be cached yet: a later miss looks again
  }
  if (t.clusterStart.empty()) {
    t.clusterStart = tp::clusterStarts(fm);
    if (t.clusterStart.size() < 2) {
      t.none = true;
      UCACHE_DEBUG("complete_chunks: %s: its tree states no clusters; not completed",
                   entry->key().key.c_str());
      return;
    }
  }
  // Every basket by position, for this look only.
  std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> all;
  for (uint32_t b = 0; b < fm.branches.size(); ++b) {
    const auto& br = fm.branches[b];
    if (t.kept.count(b) || br.externalFile || !converted(br) ||
        br.basketEntry.size() != static_cast<size_t>(br.writeBasket) + 1)
      continue;
    for (int32_t k = 0; k < br.writeBasket; ++k)
      if (br.basketSeek[k] > 0 && br.basketBytes[k] > 0)
        all.push_back({static_cast<uint64_t>(br.basketSeek[k]), {b, static_cast<uint32_t>(k)}});
  }
  std::sort(all.begin(), all.end());
  const size_t had = t.kept.size();
  for (uint64_t s : starts) {
    auto it = std::lower_bound(all.begin(), all.end(), std::make_pair(s, std::make_pair(0u, 0u)));
    if (it == all.end() || it->first != s)
      continue;
    const uint32_t b = it->second.first;
    if (t.kept.count(b))
      continue;
    auto& br = fm.branches[b];
    FileTable::Branch& kb = t.kept[b];
    kb.seek = br.basketSeek;
    kb.bytes = br.basketBytes;
    kb.entry = br.basketEntry;
    for (int32_t k = 0; k < br.writeBasket; ++k)
      if (kb.seek[k] > 0 && kb.bytes[k] > 0)
        t.bySeek[static_cast<uint64_t>(kb.seek[k])] = {b, static_cast<uint32_t>(k)};
  }
  // A look that found no new branch (the misses were the file's own records: the first look is
  // made at the header's, and it keeps the records it read, so the reader's own reads of them do
  // not miss again) is cheap to make again and starts no interval.
  if (t.kept.size() == had) {
    ++t.fails;
    return;
  }
  t.lastParseUs = nowMicros();
  ++t.parses;
}

// The ranges of the baskets to complete for `asked`, under t.mu: the kept branches' baskets in
// the clusters the asked baskets lie in, other than those asked.
void siblings(const FileTable& t, const std::vector<std::pair<uint64_t, uint64_t>>& asked,
              std::vector<std::pair<uint64_t, uint64_t>>& out, uint64_t& n) {
  const auto& cs = t.clusterStart;
  for (const auto& [a, e] : asked)
    for (auto it = t.bySeek.lower_bound(a); it != t.bySeek.end() && it->first < e; ++it) {
      const auto& kb = t.kept.at(it->second.first);
      const uint32_t k = it->second.second;
      const int64_t e0 = kb.entry[k], e1 = kb.entry[k + 1];
      if (e1 <= e0)
        continue;
      auto lo = std::upper_bound(cs.begin(), cs.end(), e0);
      auto hi = std::lower_bound(cs.begin(), cs.end(), e1);
      if (lo == cs.begin() || hi == cs.end())
        continue;
      const int64_t ca = *(lo - 1), cb = *hi;
      size_t j = static_cast<size_t>(std::upper_bound(kb.entry.begin(), kb.entry.end(), ca) -
                                     kb.entry.begin());
      j = j ? j - 1 : 0;
      for (; j + 1 < kb.entry.size() && kb.entry[j] < cb; ++j) {
        if (j == k || kb.entry[j + 1] <= ca || kb.seek[j] <= 0 || kb.bytes[j] <= 0)
          continue;
        out.emplace_back(static_cast<uint64_t>(kb.seek[j]),
                         static_cast<uint64_t>(kb.seek[j]) + static_cast<uint64_t>(kb.bytes[j]));
        ++n;
      }
    }
}

// The completing read: its pages are kept as a miss's are, checked first where the handle's
// fetches are (check_origin_integrity). The ranges it registered as in flight are withdrawn on
// every exit.
class CompleteHandler : public XrdCl::ResponseHandler {
 public:
  struct Elem {
    uint64_t off = 0, len = 0;
    std::shared_ptr<std::vector<char>> buf;
  };
  CompleteHandler(std::shared_ptr<HandleState> st, std::shared_ptr<FileEntry> entry,
                  std::vector<Elem> elems, std::vector<std::pair<uint64_t, uint64_t>> runs,
                  uint64_t baskets)
      : st_(std::move(st)), entry_(std::move(entry)), elems_(std::move(elems)),
        runs_(std::move(runs)), baskets_(baskets) {}
  ~CompleteHandler() override { entry_->clearFetchInFlight(runs_); }

  void HandleResponseWithHosts(XrdCl::XRootDStatus* status, XrdCl::AnyObject* response,
                               XrdCl::HostList* hosts) override {
    ClientCallback inClientCallback; // PluginSupport.h
    std::unique_ptr<XrdCl::XRootDStatus> s(status);
    std::unique_ptr<XrdCl::AnyObject> r(response);
    std::unique_ptr<XrdCl::HostList> hl(hosts);
    const bool check = checksOrigin(st_.get());
    const std::string server = check ? answeredBy(*st_, hosts) : std::string();
    std::shared_ptr<const KeyMap> map;
    if (check) {
      {
        std::lock_guard<std::mutex> g(st_->mu);
        map = st_->keySeeks;
      }
      if (!map)
        map = keySeeksBuilt(entry_, nullptr).map;
    }
    st_->releaseInner();
    // An answer short of what was asked is no answer (PluginSupport.h vreadComplete).
    if (!s || !s->IsOK() || !vreadComplete(r.get(), elems_)) {
      delete this;
      return;
    }
    if (check) { // as read-ahead's: nothing kept unless every basket passed
      if (!map) {
        delete this;
        return;
      }
      std::vector<FetchPiece> pieces;
      for (const auto& e : elems_)
        pieces.push_back(FetchPiece{e.off, e.buf->data(), e.len});
      auto then = [this](transpose::FetchVerdict v) {
        if (v.faults.empty() && !v.unjudged)
          keep();
        delete this;
      };
      if (fetchNeedsWork(*map, server, pieces))
        fetchVerdictAsync(map, server, std::move(pieces), nullptr, then);
      else
        then(fetchVerdict(*map, server, std::move(pieces), nullptr));
      return;
    }
    keep();
    delete this;
  }

 private:
  void keep() {
    uint64_t bytes = 0;
    for (auto& e : elems_) {
      bytes += e.len;
      auto entry = entry_;
      const uint64_t off = e.off, len = e.len;
      HandleState::postPersist(st_, [entry, off, len, buf = std::move(e.buf)] {
        entry->writePages(off, len, buf->data());
        entry->flushMeta(false);
      });
    }
    if (st_->store) {
      auto& stats = st_->store->stats();
      stats.fillCompleted.fetch_add(baskets_, std::memory_order_relaxed);
      stats.fillCompletedBytes.fetch_add(bytes, std::memory_order_relaxed);
      stats.originBytes.fetch_add(bytes, std::memory_order_relaxed);
      stats.originReadvs.fetch_add(1, std::memory_order_relaxed);
    }
  }

  std::shared_ptr<HandleState> st_;
  std::shared_ptr<FileEntry> entry_;
  std::vector<Elem> elems_;
  std::vector<std::pair<uint64_t, uint64_t>> runs_;
  uint64_t baskets_;
};

} // namespace

std::vector<std::pair<uint64_t, uint64_t>>
completionFor(const std::shared_ptr<HandleState>& st, const std::shared_ptr<FileEntry>& entry,
              const std::vector<std::pair<uint64_t, uint64_t>>& asked, uint64_t& baskets) {
  baskets = 0;
  std::vector<std::pair<uint64_t, uint64_t>> out;
  const Config& cfg = globalConfig();
  if (!st || !entry || asked.empty() || !cfg.completeChunks || !cfg.transpose ||
      readRuleDirect(entry->key().key))
    return out;
  auto t = tableFor(entry);
  std::vector<std::pair<uint64_t, uint64_t>> want;
  uint64_t n = 0;
  {
    std::lock_guard<std::mutex> g(t->mu);
    if (t->none)
      return out;
    // Misses that start where no kept basket does: a branch not learned yet (or the file's own
    // records, which no look finds; the parse limits bound those).
    std::vector<uint64_t> unknown;
    for (const auto& [a, e] : asked)
      if (!t->bySeek.count(a))
        unknown.push_back(a);
    if (!unknown.empty() && t->fails < kMaxFails && t->parses < kMaxParses &&
        (!t->parses || nowMicros() - t->lastParseUs >= kParseIntervalUs))
      learn(*t, st, entry, unknown);
    if (t->none || t->clusterStart.empty())
      return out;
    siblings(*t, asked, want, n);
  }
  // Not what the miss itself fetches, nor what the cache holds; at most kMaxCompleteBytes.
  std::sort(want.begin(), want.end());
  want.erase(std::unique(want.begin(), want.end()), want.end());
  uint64_t total = 0;
  for (const auto& [a, e] : want) {
    bool overlaps = false;
    for (const auto& [x, y] : asked)
      overlaps = overlaps || (a < y && x < e);
    if (overlaps || entry->hasRange(a, e - a))
      continue;
    if (total + (e - a) > kMaxCompleteBytes)
      break;
    out.emplace_back(a, e);
    total += e - a;
    ++baskets;
  }
  return out;
}

namespace {

// The background fetch of completeAfterMiss: the ranges in pages, as a miss keeps them.
void fetchCompletion(const std::shared_ptr<HandleState>& st,
                     const std::shared_ptr<FileEntry>& entry,
                     const std::vector<std::pair<uint64_t, uint64_t>>& asked) {
  uint64_t baskets = 0;
  const auto want = completionFor(st, entry, asked, baskets);
  if (want.empty())
    return;
  const uint64_t ps = entry->pageSize(), fs = entry->fileSize();
  std::vector<std::pair<uint64_t, uint64_t>> runs;
  for (const auto& [a, e] : want) {
    auto [rs, re] = roundSpan(ps, fs, a, e - a);
    if (!runs.empty() && rs <= runs.back().second)
      runs.back().second = std::max(runs.back().second, re);
    else
      runs.emplace_back(rs, re);
  }
  std::vector<CompleteHandler::Elem> elems;
  XrdCl::ChunkList wire;
  for (const auto& [s, e] : runs)
    for (uint64_t at = s; at < e && elems.size() < kMaxReadvElems;) {
      const uint64_t cut = readvElemEnd(at, e, ps);
      CompleteHandler::Elem el;
      el.off = at;
      el.len = cut - at;
      el.buf = std::make_shared<std::vector<char>>(el.len);
      wire.emplace_back(el.off, static_cast<uint32_t>(el.len), el.buf->data());
      elems.push_back(std::move(el));
      at = cut;
    }
  if (elems.size() >= kMaxReadvElems) { // what did not fit is not announced or fetched
    runs.clear();
    for (const auto& el : elems)
      if (!runs.empty() && runs.back().second == el.off)
        runs.back().second = el.off + el.len;
      else
        runs.emplace_back(el.off, el.off + el.len);
  }
  XrdCl::File* f = st->acquireInnerIfOpen();
  if (!f)
    return; // no origin open: nothing is opened for this
  // Announced before the read goes out, so a reader's miss inside them waits for this copy.
  std::vector<std::pair<uint64_t, uint64_t>> reg;
  for (const auto& [s, e] : runs) {
    entry->noteFetchInFlight(s, e - s);
    reg.emplace_back(s, e - s);
  }
  auto* h = new CompleteHandler(st, entry, std::move(elems), std::move(reg), baskets);
  XrdCl::XRootDStatus s = f->VectorRead(wire, nullptr, h, st->originTimeout(0));
  if (!s.IsOK()) {
    delete h; // withdraws the ranges
    st->releaseInner();
  }
}

} // namespace

void completeAfterMiss(const std::shared_ptr<HandleState>& st,
                       const std::shared_ptr<FileEntry>& entry,
                       std::vector<std::pair<uint64_t, uint64_t>> asked) {
  const Config& cfg = globalConfig();
  if (!st || !entry || asked.empty() || !cfg.completeChunks || !cfg.transpose)
    return;
  Executor::instance().post([st, entry, asked = std::move(asked)] {
    try {
      fetchCompletion(st, entry, asked);
    } catch (const std::exception& e) {
      UCACHE_DEBUG("complete_chunks for %s: %s", entry->key().key.c_str(), e.what());
    }
  });
}

void chunkCompleteBeforeFork() {
  (void)tablesMu();
  (void)tables();
}

void chunkCompleteAfterForkChild() {
  auto* kept = new std::deque<std::shared_ptr<FileTable>>(); // leaked: moved aside untouched
  kept->swap(tables());
  new (&tablesMu()) std::mutex;
}

} // namespace ucache
