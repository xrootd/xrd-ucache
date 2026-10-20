// complete_chunks' choice of baskets (ChunkComplete.h), apart from any I/O so that it can be tested
// on its own. Pure: thread-safe as any function of its arguments.
#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ucache {

// One branch's baskets, from the tree's record: basket k at [seek[k], seek[k] + bytes[k]), holding
// entries [entry[k], entry[k + 1]) (entry has one more element than there are baskets).
struct BranchBaskets {
  std::vector<int64_t> seek, entry;
  std::vector<int32_t> bytes;
};

// For the baskets that start inside the `asked` ranges ([off, end)), found in `bySeek` (a basket's
// position -> its branch in `branches` and its index there), the ranges of their branches' other
// baskets in the clusters they lie in (`clusterStart`: the tree's cluster starts, its entries
// last), appended to `out`, `n` counting them. A basket that spans clusters takes the baskets of
// every cluster it reaches. Unsorted, and a basket may appear twice.
void clusterSiblings(const std::vector<int64_t>& clusterStart,
                     const std::unordered_map<uint32_t, BranchBaskets>& branches,
                     const std::map<uint64_t, std::pair<uint32_t, uint32_t>>& bySeek,
                     const std::vector<std::pair<uint64_t, uint64_t>>& asked,
                     std::vector<std::pair<uint64_t, uint64_t>>& out, uint64_t& n);

} // namespace ucache
