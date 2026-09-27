#include "CopyGuard.h"

#include <algorithm>

namespace ucache {

void CopyGuard::stop() {
  off_ = true;
  runs_.clear();
  runs_.shrink_to_fit();
}

void CopyGuard::sizeShown() {
  std::lock_guard<std::mutex> g(mu_);
  sizeShown_ = true;
}

bool CopyGuard::allowRanges(uint64_t originSize, uint64_t shownSize, const Range* ranges,
                            size_t n) {
  if (!originSize)
    return true;
  if (n == 1 && ranges[0].first == 0 && ranges[0].second == originSize)
    return false; // the origin's file by its size, from a larger layout
  std::lock_guard<std::mutex> g(mu_);
  if (refused_)
    return false;
  if (off_)
    return true;
  if (n == 1 && ranges[0].first == 0 && ranges[0].second == shownSize) {
    stop(); // the whole layout: a valid file
    return true;
  }
  bool crosses = false;
  for (size_t i = 0; i < n; ++i) {
    const uint64_t begin = ranges[i].first;
    const uint64_t end = begin + ranges[i].second;
    if (!ranges[i].second)
      continue;
    if (end < begin || begin >= originSize) {
      stop(); // a reader of the part past the origin's size
      return true;
    }
    if (end > originSize)
      crosses = true;
  }
  for (size_t i = 0; i < n; ++i) {
    if (!ranges[i].second)
      continue;
    Range r{ranges[i].first, std::min(ranges[i].first + ranges[i].second, originSize)};
    auto it = std::lower_bound(runs_.begin(), runs_.end(), r,
                               [](const Range& a, const Range& b) { return a.second < b.first; });
    while (it != runs_.end() && it->first <= r.second) { // overlaps or touches: merge
      r.first = std::min(r.first, it->first);
      r.second = std::max(r.second, it->second);
      it = runs_.erase(it);
    }
    runs_.insert(it, r);
  }
  if (runs_.size() == 1 && runs_[0].first == 0 && runs_[0].second >= originSize &&
      !(crosses && sizeShown_)) {
    refused_ = true;
    return false;
  }
  if (crosses || runs_.size() > kMaxRuns)
    stop(); // read into the part past the origin's size without completing it; or scattered
  return true;
}

} // namespace ucache
