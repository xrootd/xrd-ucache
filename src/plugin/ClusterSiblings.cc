#include "ClusterSiblings.h"

#include <algorithm>

namespace ucache {

void clusterSiblings(const std::vector<int64_t>& cs,
                     const std::unordered_map<uint32_t, BranchBaskets>& branches,
                     const std::map<uint64_t, std::pair<uint32_t, uint32_t>>& bySeek,
                     const std::vector<std::pair<uint64_t, uint64_t>>& asked,
                     std::vector<std::pair<uint64_t, uint64_t>>& out, uint64_t& n) {
  for (const auto& [a, e] : asked)
    for (auto it = bySeek.lower_bound(a); it != bySeek.end() && it->first < e; ++it) {
      auto b = branches.find(it->second.first);
      if (b == branches.end())
        continue;
      const BranchBaskets& kb = b->second;
      const uint32_t k = it->second.second;
      if (k + 1 >= kb.entry.size())
        continue;
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
      for (;
           j + 1 < kb.entry.size() && j < kb.seek.size() && j < kb.bytes.size() && kb.entry[j] < cb;
           ++j) {
        if (j == k || kb.entry[j + 1] <= ca || kb.seek[j] <= 0 || kb.bytes[j] <= 0)
          continue;
        out.emplace_back(static_cast<uint64_t>(kb.seek[j]),
                         static_cast<uint64_t>(kb.seek[j]) + static_cast<uint64_t>(kb.bytes[j]));
        ++n;
      }
    }
}

} // namespace ucache
