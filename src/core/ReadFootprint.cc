#include "ReadFootprint.h"

#include <algorithm>
#include <cstdio>
#include <new>

namespace ucache {

namespace {

// Every footprint's ranges, in bytes, against the process bound. A forked
// child starts a new generation of it (afterForkChild): a footprint counted in
// an earlier one is counted afresh the next time its ranges change.
std::atomic<uint64_t> g_rangeBytes{0};
std::atomic<uint64_t> g_rangeGen{0};
std::atomic<uint64_t> g_rangeCap{ReadFootprint::kMaxRangeBytes};
constexpr uint64_t kPairBytes = sizeof(std::pair<uint64_t, uint64_t>);
// A range set grows by doubling from this many, and each growth is claimed
// against the process total before it is made: the shared counter is touched
// once per doubling, not once per range, so the reads of every file do not
// meet on one cache line.
constexpr size_t kFirstRanges = 16;
// Out-of-order ranges wait in pend_ until there are this many, or a quarter of
// the merged set: a sort of the few and one pass over the many.
constexpr size_t kCompactMin = 256;

void putVarint(std::vector<uint8_t>& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<uint8_t>(v | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<uint8_t>(v));
}

bool getVarint(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
  v = 0;
  for (int shift = 0; p < end && shift < 64; shift += 7) {
    const uint8_t b = *p++;
    v |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (!(b & 0x80))
      return true;
  }
  return false;
}

// Merge two sorted range lists into `out`, joining ranges that touch.
void mergeSorted(const std::vector<std::pair<uint64_t, uint64_t>>& a,
                 const std::vector<std::pair<uint64_t, uint64_t>>& b,
                 std::vector<std::pair<uint64_t, uint64_t>>& out) {
  out.clear();
  out.reserve(a.size() + b.size());
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    const auto& r = (j == b.size() || (i < a.size() && a[i].first <= b[j].first)) ? a[i++] : b[j++];
    if (!out.empty() && r.first <= out.back().second)
      out.back().second = std::max(out.back().second, r.second);
    else
      out.push_back(r);
  }
}

} // namespace

ReadFootprint::~ReadFootprint() {
  if (held_ && heldGen_ == g_rangeGen.load(std::memory_order_relaxed))
    g_rangeBytes.fetch_sub(held_, std::memory_order_relaxed);
}

uint64_t ReadFootprint::rangeBytesTotal() { return g_rangeBytes.load(std::memory_order_relaxed); }

void ReadFootprint::afterForkChild() {
  g_rangeGen.fetch_add(1, std::memory_order_relaxed);
  g_rangeBytes.store(0, std::memory_order_relaxed);
}

void ReadFootprint::setRangeByteCapForTests(uint64_t bytes) {
  g_rangeCap.store(bytes ? bytes : kMaxRangeBytes, std::memory_order_relaxed);
}

bool ReadFootprint::overCap() const {
  return g_rangeBytes.load(std::memory_order_relaxed) > g_rangeCap.load(std::memory_order_relaxed);
}

bool ReadFootprint::bucketSpan(uint64_t off, uint64_t len, uint64_t& first, uint64_t& last) {
  if (!len)
    return false;
  first = off / kBucket;
  last = (off + len - 1) / kBucket;
  // A hostile or corrupt range must not turn into an enormous allocation:
  // this is a diagnostic, and refusing it costs only the signature. The bound
  // has to be a plausible FILE, not a plausible integer -- at one bucket per
  // MiB, 2^32 buckets is four petabytes and would have allocated half a
  // gigabyte here, under the lock, from a single absurd offset. 2^24 is
  // sixteen terabytes, past anything this reads and cheap to refuse. It also
  // keeps off + len from wrapping.
  constexpr uint64_t kMaxBucket = 1ull << 24;
  return last >= first && last <= kMaxBucket;
}

bool ReadFootprint::noteBucketsLocked(uint64_t first, uint64_t last) {
  const size_t need = static_cast<size_t>(last / 64) + 1;
  if (bits_.size() < need) {
    try {
      bits_.resize(need, 0);
    } catch (...) {
      // This runs on the application's read: no memory for the bucket set
      // makes the footprint unknown, never the read fail.
      poisoned_ = true;
      dropRangesLocked();
      return false;
    }
  }
  for (uint64_t b = first; b <= last; ++b)
    bits_[b / 64] |= (1ull << (b % 64));
  return true;
}

void ReadFootprint::note(uint64_t off, uint64_t len, uint64_t sizeBytes) {
  uint64_t first = 0, last = 0;
  if (!bucketSpan(off, len, first, last))
    return;
  std::lock_guard<std::mutex> g(mu_);
  if (noteBucketsLocked(first, last))
    noteRangeLocked(off, off + len, sizeBytes);
}

void ReadFootprint::noteMapped(const std::vector<Span>& units, const std::vector<Span>* exact,
                               uint64_t sizeBytes) {
  std::lock_guard<std::mutex> g(mu_);
  uint64_t first = 0, last = 0;
  for (const auto& [off, len] : units)
    if (bucketSpan(off, len, first, last) && !noteBucketsLocked(first, last))
      return; // poisoned: nothing more to record
  if (!exact) {
    bytesUnknownLocked();
    return;
  }
  // Taken exactly as note() takes a range, and refused where note() refuses it.
  for (const auto& [off, len] : *exact)
    if (bucketSpan(off, len, first, last))
      noteRangeLocked(off, off + len, sizeBytes);
}

void ReadFootprint::bytesUnknown() {
  std::lock_guard<std::mutex> g(mu_);
  bytesUnknownLocked();
}

void ReadFootprint::bytesUnknownLocked() {
  if (bytesLost_)
    return;
  bytesLost_ = true;
  dropRangesLocked(); // nothing derived from them will be reported now
}

void ReadFootprint::learnSize(uint64_t sizeBytes) {
  if (!sizeBytes)
    return;
  std::lock_guard<std::mutex> g(mu_);
  knownSize_ = sizeBytes;
}

void ReadFootprint::noteRangeLocked(uint64_t a, uint64_t b, uint64_t size) {
  if (bytesLost_)
    return; // neither total is reported again: nothing to keep for them
  if (!size)
    size = knownSize_;
  if (size) {
    if (clipSize_ && clipSize_ != size)
      clipConflict_ = true;
    clipSize_ = size;
    if (a < size)
      origClipped_ += std::min(b, size) - a;
  } else {
    origRaw_ += b - a;
    noteTailLocked(a, b);
  }
  if (poisoned_ || overflow_)
    return;
  try {
    addRangeLocked(a, b);
  } catch (...) {
    overflowLocked(); // unknown beats a failed read
  }
}

void ReadFootprint::addRangeLocked(uint64_t a, uint64_t b) {
  if (frozenCount_) {
    thawLocked();
    if (overflow_)
      return;
  }
  // The common shapes first: a reader moving forward appends a range or
  // extends the last one, and a vector read's adjacent chunks join as they come.
  if (pend_.empty()) {
    if (!iv_.empty() && a <= iv_.back().second && a >= iv_.back().first) {
      iv_.back().second = std::max(iv_.back().second, b);
      return;
    }
    if (iv_.empty() || a > iv_.back().second) {
      if (iv_.size() >= kMaxRanges) // one more disjoint range than a file keeps
        overflowLocked();
      else if (roomLocked(iv_))
        iv_.emplace_back(a, b); // room made: cannot throw
      return;
    }
  } else if (a <= pend_.back().second && b >= pend_.back().first) {
    pend_.back().first = std::min(pend_.back().first, a);
    pend_.back().second = std::max(pend_.back().second, b);
    return;
  }
  if (!roomLocked(pend_))
    return;
  pend_.emplace_back(a, b);
  if (pend_.size() >= std::max(kCompactMin, iv_.size() / 4))
    compactLocked();
}

void ReadFootprint::noteTailLocked(uint64_t a, uint64_t b) {
  if (origLost_)
    return;
  if (!tail_) {
    tail_.reset(new (std::nothrow) Tail());
    if (!tail_) {
      origLost_ = true; // cannot follow the requests past the end any more
      return;
    }
  }
  Tail& t = *tail_;
  auto minAt = [&t] {
    size_t m = 0;
    for (size_t i = 1; i < kTail; ++i)
      if (t.r[i].end < t.r[m].end)
        m = i;
    return m;
  };
  if (t.n < kTail) {
    t.r[t.n++] = {a, b};
    if (t.n == kTail)
      t.minAt = minAt();
    return;
  }
  if (b <= t.r[t.minAt].end) {
    tailLost_ = std::max(tailLost_, b);
    return;
  }
  tailLost_ = std::max(tailLost_, t.r[t.minAt].end);
  t.r[t.minAt] = {a, b};
  t.minAt = minAt();
}

uint64_t ReadFootprint::occupiedLocked() const {
  return static_cast<uint64_t>(iv_.capacity() + pend_.capacity()) * kPairBytes + frozen_.capacity();
}

bool ReadFootprint::roomLocked(std::vector<Range>& v) {
  if (heldGen_ != g_rangeGen.load(std::memory_order_relaxed))
    settleLocked(); // counted in the process that forked this one: count it here
  if (v.size() < v.capacity())
    return true;
  const size_t cap = std::max(kFirstRanges, v.capacity() * 2);
  const uint64_t add = static_cast<uint64_t>(cap - v.capacity()) * kPairBytes;
  // Claimed before it is made, so two files growing at once cannot both slip
  // under the bound.
  if (g_rangeBytes.fetch_add(add, std::memory_order_relaxed) + add >
      g_rangeCap.load(std::memory_order_relaxed)) {
    g_rangeBytes.fetch_sub(add, std::memory_order_relaxed);
    overflowLocked();
    return false;
  }
  held_ += add;
  try {
    v.reserve(cap);
  } catch (...) {
    overflowLocked(); // gives back the claim with everything else held
    return false;
  }
  settleLocked(); // the allocator may have given more than was asked
  return true;
}

void ReadFootprint::rehold(uint64_t bytes) {
  const uint64_t gen = g_rangeGen.load(std::memory_order_relaxed);
  if (heldGen_ != gen) { // held_ was counted in another process's total
    heldGen_ = gen;
    held_ = 0;
  }
  if (bytes >= held_)
    g_rangeBytes.fetch_add(bytes - held_, std::memory_order_relaxed);
  else
    g_rangeBytes.fetch_sub(held_ - bytes, std::memory_order_relaxed);
  held_ = bytes;
}

void ReadFootprint::settleLocked() { rehold(occupiedLocked()); }

void ReadFootprint::overflowLocked() {
  overflow_ = true;
  dropRangesLocked();
}

void ReadFootprint::dropRangesLocked() {
  std::vector<Range>().swap(iv_);
  std::vector<Range>().swap(pend_);
  std::vector<uint8_t>().swap(frozen_);
  frozenCount_ = 0;
  rehold(0);
}

void ReadFootprint::compactLocked() {
  if (pend_.empty())
    return;
  std::sort(pend_.begin(), pend_.end());
  std::vector<Range> out;
  mergeSorted(iv_, pend_, out);
  iv_.swap(out);
  pend_.clear(); // its capacity stays, and is counted: it fills again
  if (iv_.size() > kMaxRanges) {
    overflowLocked();
    return;
  }
  settleLocked();
  if (overCap())
    overflowLocked();
}

void ReadFootprint::thawLocked() {
  std::vector<Range> out;
  out.reserve(frozenCount_);
  const uint8_t* p = frozen_.data();
  const uint8_t* end = p + frozen_.size();
  uint64_t at = 0;
  for (size_t i = 0; i < frozenCount_; ++i) {
    uint64_t gap = 0, len = 0;
    if (!getVarint(p, end, gap) || !getVarint(p, end, len)) {
      overflowLocked(); // never written that way; unknown beats wrong
      return;
    }
    out.emplace_back(at + gap, at + gap + len);
    at += gap + len;
  }
  iv_.swap(out);
  std::vector<uint8_t>().swap(frozen_);
  frozenCount_ = 0;
  settleLocked();
  if (overCap())
    overflowLocked();
}

void ReadFootprint::freeze() {
  std::lock_guard<std::mutex> g(mu_);
  if (poisoned_ || overflow_ || bytesLost_ || frozenCount_)
    return;
  try {
    compactLocked();
    if (overflow_)
      return;
    if (iv_.empty()) { // nothing to keep; the spare room goes back all the same
      std::vector<Range>().swap(iv_);
      std::vector<Range>().swap(pend_);
      settleLocked();
      return;
    }
    std::vector<uint8_t> enc;
    enc.reserve(iv_.size() * 5);
    uint64_t at = 0;
    for (const auto& [a, b] : iv_) {
      putVarint(enc, a - at);
      putVarint(enc, b - a);
      at = b;
    }
    enc.shrink_to_fit();
    frozenCount_ = iv_.size();
    frozen_.swap(enc);
    std::vector<Range>().swap(iv_);
    std::vector<Range>().swap(pend_);
    settleLocked();
  } catch (...) {
    overflowLocked(); // no memory to store them compactly: they go
  }
}

template <typename F> bool ReadFootprint::forEachMergedLocked(F f) const {
  if (frozenCount_) { // decoded as they are walked: no copy
    const uint8_t* p = frozen_.data();
    const uint8_t* end = p + frozen_.size();
    uint64_t at = 0;
    for (size_t i = 0; i < frozenCount_; ++i) {
      uint64_t gap = 0, len = 0;
      if (!getVarint(p, end, gap) || !getVarint(p, end, len))
        return false;
      f(at + gap, at + gap + len);
      at += gap + len;
    }
    return true;
  }
  if (pend_.empty()) {
    for (const auto& [a, b] : iv_)
      f(a, b);
    return true;
  }
  std::vector<Range> p;
  try {
    p = pend_;
  } catch (...) {
    return false;
  }
  std::sort(p.begin(), p.end());
  // The two sorted lists, merged as they are walked, touching ranges joined.
  size_t i = 0, j = 0;
  bool open = false;
  uint64_t ca = 0, cb = 0;
  while (i < iv_.size() || j < p.size()) {
    const Range& r =
        (j == p.size() || (i < iv_.size() && iv_[i].first <= p[j].first)) ? iv_[i++] : p[j++];
    if (open && r.first <= cb) {
      cb = std::max(cb, r.second);
      continue;
    }
    if (open)
      f(ca, cb);
    open = true;
    ca = r.first;
    cb = r.second;
  }
  if (open)
    f(ca, cb);
  return true;
}

ReadFootprint::Totals ReadFootprint::totals(uint64_t limitBytes) const {
  std::lock_guard<std::mutex> g(mu_);
  Totals t;
  if (poisoned_ || bytesLost_)
    return t;
  const uint64_t lim = limitBytes ? limitBytes : ~uint64_t(0);
  // What ran past the end is not file content. Every request of unknown size
  // that did is among the kTail followed, unless one not followed ends past
  // the limit; the others were clipped as they came, at the size they gave.
  const bool clippedHere = !clipSize_ || (!clipConflict_ && clipSize_ == lim);
  if (!origLost_ && tailLost_ <= lim && clippedHere) {
    uint64_t over = 0;
    for (size_t i = 0; tail_ && i < tail_->n; ++i)
      if (tail_->r[i].end > lim)
        over += tail_->r[i].end - std::max(tail_->r[i].off, lim);
    t.origKnown = true;
    t.origBytes = origClipped_ + origRaw_ - over;
  }
  if (overflow_)
    return t;
  uint64_t n = 0;
  const bool ok = forEachMergedLocked([&n, lim](uint64_t a, uint64_t b) {
    if (a < lim)
      n += std::min(b, lim) - a;
  });
  if (!ok)
    return t; // no memory to merge them: the distinct bytes are unknown
  t.uniqueKnown = true;
  t.uniqueBytes = n;
  return t;
}

bool ReadFootprint::ranges(uint64_t limitBytes,
                           std::vector<std::pair<uint64_t, uint64_t>>& out) const {
  std::lock_guard<std::mutex> g(mu_);
  if (poisoned_ || overflow_ || bytesLost_)
    return false;
  const uint64_t lim = limitBytes ? limitBytes : ~uint64_t(0);
  std::vector<Range> r;
  try {
    const bool ok = forEachMergedLocked([&r, lim](uint64_t a, uint64_t b) {
      if (a < lim)
        r.emplace_back(a, std::min(b, lim) - a);
    });
    if (!ok)
      return false;
  } catch (...) {
    return false; // no memory for the copy
  }
  out.swap(r);
  return true;
}

bool ReadFootprint::rangesOverflowed() const {
  std::lock_guard<std::mutex> g(mu_);
  return overflow_;
}

bool ReadFootprint::empty() const {
  std::lock_guard<std::mutex> g(mu_);
  for (uint64_t w : bits_)
    if (w)
      return false;
  return true;
}

namespace {
uint64_t lastBucket(uint64_t limitBytes) {
  return limitBytes ? (limitBytes - 1) / ReadFootprint::kBucket
                    : ~uint64_t(0);
}
} // namespace

void ReadFootprint::poison() {
  std::lock_guard<std::mutex> g(mu_);
  poisoned_ = true;
  dropRangesLocked(); // nothing derived from them will be reported now
}

bool ReadFootprint::poisoned() const {
  std::lock_guard<std::mutex> g(mu_);
  return poisoned_;
}

uint64_t ReadFootprint::count(uint64_t limitBytes) const {
  std::lock_guard<std::mutex> g(mu_);
  if (poisoned_)
    return 0;
  return countLocked(limitBytes);
}

// Callers hold mu_. Split out so sigAndCount() computes exactly what the
// single-value accessors do rather than a second copy of it.
uint64_t ReadFootprint::countLocked(uint64_t limitBytes) const {
  const uint64_t last = lastBucket(limitBytes);
  uint64_t n = 0;
  for (size_t i = 0; i < bits_.size(); ++i) {
    uint64_t w = bits_[i];
    while (w) {
      const uint64_t b = i * 64 + static_cast<uint64_t>(__builtin_ctzll(w));
      w &= w - 1;
      if (b <= last)
        ++n;
    }
  }
  return n;
}

std::vector<uint64_t> ReadFootprint::buckets(uint64_t limitBytes) const {
  std::lock_guard<std::mutex> g(mu_);
  const uint64_t last = lastBucket(limitBytes);
  std::vector<uint64_t> out;
  for (size_t i = 0; i < bits_.size(); ++i) {
    uint64_t w = bits_[i];
    while (w) {
      const uint64_t b = i * 64 + static_cast<uint64_t>(__builtin_ctzll(w));
      w &= w - 1;
      if (b <= last)
        out.push_back(b);
    }
  }
  return out;
}

bool ReadFootprint::sigAndCount(uint64_t limitBytes, std::string& sigOut,
                                uint64_t& countOut) const {
  std::lock_guard<std::mutex> g(mu_);
  if (poisoned_)
    return false;
  const std::string sg = sigLocked(limitBytes);
  if (sg.empty())
    return false;
  sigOut = sg;
  countOut = countLocked(limitBytes);
  return true;
}

std::string ReadFootprint::sig(uint64_t limitBytes) const {
  std::lock_guard<std::mutex> g(mu_);
  if (poisoned_)
    return {};
  return sigLocked(limitBytes);
}

// Callers hold mu_.
std::string ReadFootprint::sigLocked(uint64_t limitBytes) const {
  const uint64_t last = lastBucket(limitBytes);
  uint64_t h = 1469598103934665603ull;
  bool any = false;
  for (size_t i = 0; i < bits_.size(); ++i) {
    uint64_t w = bits_[i];
    while (w) {
      const uint64_t b = i * 64 + static_cast<uint64_t>(__builtin_ctzll(w));
      w &= w - 1;
      if (b > last)
        continue;
      any = true;
      for (int s = 0; s < 8; ++s) {
        h ^= static_cast<unsigned char>((b >> (8 * s)) & 0xff);
        h *= 1099511628211ull;
      }
    }
  }
  if (!any)
    return std::string();
  char buf[24];
  std::snprintf(buf, sizeof buf, "%012llx",
                static_cast<unsigned long long>(h & 0xffffffffffffull));
  return std::string(buf);
}

} // namespace ucache
