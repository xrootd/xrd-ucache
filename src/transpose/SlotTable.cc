#include "SlotTable.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>

namespace ucache::transpose {

namespace {

// LEB128 and zigzag, as the layout blob is written.
struct Rd {
  const uint8_t* p;
  size_t n, at;
  bool ok = true;
  uint64_t varint() {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (at >= n) {
        ok = false;
        return 0;
      }
      const uint8_t c = p[at++];
      v |= static_cast<uint64_t>(c & 0x7f) << shift;
      if (!(c & 0x80))
        return v;
    }
    ok = false;
    return 0;
  }
  int64_t svarint() {
    const uint64_t u = varint();
    return static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
  }
};

uint32_t plainLen(uint32_t origLen, uint32_t k100) {
  const uint64_t v = static_cast<uint64_t>(origLen) * k100 / 100;
  return v > INT32_MAX ? static_cast<uint32_t>(INT32_MAX) : static_cast<uint32_t>(v);
}

// One record, against the state before it (which it advances).
struct State {
  uint32_t prevBranch = 0, prevBasket = 0;
  uint64_t prevOrig = 0, nextV = 0;
};
bool readSlot(Rd& r, State& st, uint32_t k100, FillSlot& s) {
  s.branch = static_cast<uint32_t>(st.prevBranch + r.svarint());
  s.basket = static_cast<uint32_t>(st.prevBasket + 1 + r.svarint());
  s.origSeek = st.prevOrig + static_cast<uint64_t>(r.svarint());
  s.origLen = static_cast<uint32_t>(r.varint());
  const uint64_t vl = r.varint();
  s.vLen = vl ? static_cast<uint32_t>(vl - 1) : plainLen(s.origLen, k100);
  s.vSeek = st.nextV + static_cast<uint64_t>(r.svarint());
  st.prevBranch = s.branch;
  st.prevBasket = s.basket;
  st.prevOrig = s.origSeek;
  st.nextV = s.vSeek + s.vLen;
  return r.ok;
}

} // namespace

void SlotTable::assign(std::vector<FillSlot> all) {
  std::lock_guard<std::mutex> g(mu_);
  all_ = std::move(all);
  n_ = static_cast<uint32_t>(all_.size());
  src_ = nullptr;
  owned_.clear();
  byOrig_.clear();
  byOrigFresh_ = false;
  std::vector<uint8_t>().swap(raw_);
  std::vector<std::pair<uint32_t, uint32_t>> rs; // first, count
  for (uint32_t i = 0; i < n_; ++i)
    if (rs.empty() || all_[i].branch != all_[rs.back().first].branch)
      rs.emplace_back(i, 1);
    else
      ++rs.back().second;
  nRuns_ = rs.size();
  runs_.reset(new Run[nRuns_]);
  for (size_t k = 0; k < nRuns_; ++k) {
    Run& r = runs_[k];
    r.first = rs[k].first;
    r.count = rs[k].second;
    r.vStart = all_[r.first].vSeek;
    r.vEnd = all_[r.first + r.count - 1].vSeek + all_[r.first + r.count - 1].vLen;
    r.slots.store(all_.data() + r.first, std::memory_order_release);
  }
  decodedSlots_.store(n_, std::memory_order_relaxed);
}

bool SlotTable::index(const std::vector<uint8_t>& raw, size_t& off, uint32_t n, const Geometry& g,
                      Source src) {
  std::lock_guard<std::mutex> lk(mu_);
  all_.clear();
  owned_.clear();
  byOrig_.clear();
  byOrigFresh_ = false;
  std::vector<uint8_t>().swap(raw_);
  quiet_.store(0, std::memory_order_relaxed);
  runs_.reset();
  nRuns_ = 0;
  n_ = 0;
  decodedSlots_.store(0, std::memory_order_relaxed);
  if (static_cast<uint64_t>(n) * 6 > raw.size())
    return false;
  struct Tmp {
    uint32_t first, count;
    uint64_t vStart, vEnd;
    size_t rawOff;
    State st;
  };
  std::vector<Tmp> rs;
  Rd r{raw.data(), raw.size(), off};
  State st;
  st.nextV = g.slotsBegin;
  uint64_t prev = g.slotsBegin;
  uint32_t branch = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const size_t at = r.at;
    const State before = st;
    FillSlot s;
    if (!readSlot(r, st, g.slotFactor100, s))
      return false;
    // Checked, not trusted, exactly as a full decode checks it.
    if (s.vSeek < prev || s.vSeek + s.vLen > g.virtualSize || (!g.rntuple && s.vLen < s.origLen) ||
        s.origSeek + s.origLen > g.originSize)
      return false;
    prev = s.vSeek + s.vLen;
    if (rs.empty() || s.branch != branch) {
      rs.push_back({i, 0, s.vSeek, 0, at, before});
      branch = s.branch;
    }
    ++rs.back().count;
    rs.back().vEnd = s.vSeek + s.vLen;
  }
  off = r.at;
  nRuns_ = rs.size();
  runs_.reset(new Run[nRuns_]);
  for (size_t k = 0; k < nRuns_; ++k) {
    Run& x = runs_[k];
    x.first = rs[k].first;
    x.count = rs[k].count;
    x.vStart = rs[k].vStart;
    x.vEnd = rs[k].vEnd;
    x.rawOff = rs[k].rawOff;
    x.prevBranch = rs[k].st.prevBranch;
    x.prevBasket = rs[k].st.prevBasket;
    x.prevOrig = rs[k].st.prevOrig;
    x.nextV = rs[k].st.nextV;
  }
  g_ = g;
  src_ = std::move(src);
  n_ = n;
  return true;
}

size_t SlotTable::runOf(uint32_t i) const {
  size_t lo = 0, hi = nRuns_;
  while (hi - lo > 1) {
    const size_t mid = (lo + hi) / 2;
    if (runs_[mid].first <= i)
      lo = mid;
    else
      hi = mid;
  }
  return lo;
}

bool SlotTable::decodeRun(const std::vector<uint8_t>& raw, Run& r,
                          std::unique_ptr<FillSlot[]>& out) const {
  out.reset(new FillSlot[r.count]);
  Rd rd{raw.data(), raw.size(), r.rawOff};
  State st{r.prevBranch, r.prevBasket, r.prevOrig, r.nextV};
  for (uint32_t k = 0; k < r.count; ++k) {
    FillSlot& s = out[k];
    if (!readSlot(rd, st, g_.slotFactor100, s))
      return false;
    if ((k == 0 && s.vSeek != r.vStart) || (k > 0 && s.branch != out[0].branch))
      return false; // not the bytes the run was indexed from
  }
  return st.nextV == r.vEnd;
}

bool SlotTable::decodeRuns(const std::vector<size_t>& want) {
  std::vector<size_t> todo;
  for (size_t k : want)
    if (!runs_[k].slots.load(std::memory_order_acquire))
      todo.push_back(k);
  if (todo.empty())
    return true;
  quiet_.store(0, std::memory_order_relaxed);
  if (raw_.empty() && (!src_ || !src_(raw_))) {
    std::vector<uint8_t>().swap(raw_);
    return false;
  }
  for (size_t k : todo) {
    Run& r = runs_[k];
    std::unique_ptr<FillSlot[]> buf;
    if (!decodeRun(raw_, r, buf)) {
      std::vector<uint8_t>().swap(raw_); // read again next time, not these bytes
      return false;
    }
    r.slots.store(buf.get(), std::memory_order_release);
    owned_.push_back(std::move(buf));
    decodedSlots_.fetch_add(r.count, std::memory_order_relaxed);
  }
  byOrigFresh_ = false;
  return true;
}

void SlotTable::runsIn(uint64_t a, uint64_t b, std::vector<size_t>& out) const {
  if (!nRuns_ || b <= a)
    return;
  // The first run ending past a; runs ascend and do not overlap.
  size_t lo = 0, hi = nRuns_;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (runs_[mid].vEnd <= a)
      lo = mid + 1;
    else
      hi = mid;
  }
  for (size_t k = lo; k < nRuns_ && runs_[k].vStart < b; ++k)
    if (!runs_[k].slots.load(std::memory_order_acquire))
      out.push_back(k);
}

void SlotTable::noteQuiet() {
  if (quiet_.fetch_add(1, std::memory_order_relaxed) + 1 != kKeepQuiet)
    return;
  std::lock_guard<std::mutex> g(mu_);
  if (quiet_.load(std::memory_order_relaxed) >= kKeepQuiet)
    std::vector<uint8_t>().swap(raw_);
}

bool SlotTable::holdsRaw() {
  std::lock_guard<std::mutex> g(mu_);
  return !raw_.empty();
}

bool SlotTable::prepare(uint64_t a, uint64_t b) {
  std::vector<size_t> want;
  runsIn(a, b, want);
  if (want.empty()) {
    noteQuiet();
    return true;
  }
  std::lock_guard<std::mutex> g(mu_);
  return decodeRuns(want);
}

bool SlotTable::prepareRanges(const std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
  std::vector<size_t> want;
  for (const auto& [off, len] : ranges)
    if (off + len > g_.slotsBegin)
      runsIn(std::max(off, g_.slotsBegin), off + len, want);
  if (want.empty()) {
    noteQuiet();
    return true;
  }
  std::sort(want.begin(), want.end());
  want.erase(std::unique(want.begin(), want.end()), want.end());
  std::lock_guard<std::mutex> g(mu_);
  return decodeRuns(want);
}

bool SlotTable::prepareSlots(const std::vector<uint32_t>& idx) {
  std::vector<size_t> want;
  for (uint32_t i : idx)
    if (i < n_) {
      const size_t k = runOf(i);
      if (!runs_[k].slots.load(std::memory_order_acquire))
        want.push_back(k);
    }
  if (want.empty()) {
    noteQuiet();
    return true;
  }
  std::sort(want.begin(), want.end());
  want.erase(std::unique(want.begin(), want.end()), want.end());
  std::lock_guard<std::mutex> g(mu_);
  return decodeRuns(want);
}

const FillSlot& SlotTable::at(uint32_t i) {
  if (i >= n_)
    throw std::runtime_error("slot out of range");
  Run& r = runs_[runOf(i)];
  const FillSlot* p = r.slots.load(std::memory_order_acquire);
  if (!p) {
    std::lock_guard<std::mutex> g(mu_);
    if (!decodeRuns({static_cast<size_t>(&r - runs_.get())}))
      throw std::runtime_error("slot table cannot be read");
    p = r.slots.load(std::memory_order_acquire);
  }
  return p[i - r.first];
}

bool SlotTable::find(uint64_t pos, uint32_t& i) {
  if (!nRuns_ || pos < g_.slotsBegin || (g_.virtualSize && pos >= g_.virtualSize))
    return false;
  // The last run starting at or before pos.
  size_t lo = 0, hi = nRuns_;
  while (hi - lo > 1) {
    const size_t mid = (lo + hi) / 2;
    if (runs_[mid].vStart <= pos)
      lo = mid;
    else
      hi = mid;
  }
  Run& r = runs_[lo];
  if (r.vStart > pos)
    return false;
  const FillSlot* p = r.slots.load(std::memory_order_acquire);
  if (!p) {
    std::lock_guard<std::mutex> g(mu_);
    if (!decodeRuns({lo}))
      return false;
    p = r.slots.load(std::memory_order_acquire);
  }
  uint32_t a = 0, b = r.count;
  while (b - a > 1) {
    const uint32_t mid = (a + b) / 2;
    if (p[mid].vSeek <= pos)
      a = mid;
    else
      b = mid;
  }
  i = r.first + a;
  return true;
}

bool SlotTable::forEach(uint64_t a, uint64_t b,
                        const std::function<bool(uint32_t, const FillSlot&)>& f) {
  if (!nRuns_ || b <= a)
    return true;
  if (!prepare(a, b))
    return false;
  size_t lo = 0, hi = nRuns_;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (runs_[mid].vEnd <= a)
      lo = mid + 1;
    else
      hi = mid;
  }
  for (size_t k = lo; k < nRuns_ && runs_[k].vStart < b; ++k) {
    const Run& r = runs_[k];
    const FillSlot* p = r.slots.load(std::memory_order_acquire);
    if (!p)
      return false;
    // The first slot of the run ending past a.
    uint32_t x = 0, y = r.count;
    while (x < y) {
      const uint32_t mid = (x + y) / 2;
      if (p[mid].vSeek + p[mid].vLen <= a)
        x = mid + 1;
      else
        y = mid;
    }
    for (uint32_t j = x; j < r.count && p[j].vSeek < b; ++j)
      if (!f(r.first + j, p[j]))
        return true;
  }
  return true;
}

bool SlotTable::forEachAll(const std::function<void(uint32_t, const FillSlot&)>& f) {
  std::lock_guard<std::mutex> g(mu_);
  std::vector<uint8_t> raw;
  bool haveRaw = false;
  for (size_t k = 0; k < nRuns_; ++k) {
    Run& r = runs_[k];
    const FillSlot* p = r.slots.load(std::memory_order_acquire);
    std::unique_ptr<FillSlot[]> tmp;
    if (!p) {
      if (!haveRaw && (!src_ || !(haveRaw = src_(raw))))
        return false;
      if (!decodeRun(raw, r, tmp))
        return false;
      p = tmp.get();
    }
    for (uint32_t j = 0; j < r.count; ++j)
      f(r.first + j, p[j]);
  }
  return true;
}

void SlotTable::decodedOverlappingOrig(uint64_t a, uint64_t b, std::vector<uint32_t>& out) {
  std::lock_guard<std::mutex> g(mu_);
  auto slot = [this](uint32_t i) -> const FillSlot& {
    const Run& r = runs_[runOf(i)];
    return r.slots.load(std::memory_order_acquire)[i - r.first];
  };
  if (!byOrigFresh_) {
    byOrig_.clear();
    for (size_t k = 0; k < nRuns_; ++k)
      if (runs_[k].slots.load(std::memory_order_acquire))
        for (uint32_t j = 0; j < runs_[k].count; ++j)
          byOrig_.push_back(runs_[k].first + j);
    std::sort(byOrig_.begin(), byOrig_.end(), [&](uint32_t x, uint32_t y) {
      return slot(x).origSeek < slot(y).origSeek;
    });
    byOrigFresh_ = true;
  }
  // Baskets do not overlap in the original file, so their ends ascend too.
  auto it = std::lower_bound(byOrig_.begin(), byOrig_.end(), a, [&](uint32_t k, uint64_t v) {
    return slot(k).origSeek + slot(k).origLen <= v;
  });
  for (; it != byOrig_.end() && slot(*it).origSeek < b; ++it)
    out.push_back(*it);
}

} // namespace ucache::transpose
