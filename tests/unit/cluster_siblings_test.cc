// complete_chunks' choice of baskets (ClusterSiblings.h): which other baskets of a branch lie in
// the clusters the asked baskets are in.
#include "ClusterSiblings.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace ucache;

namespace {

using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;

// One branch of `nb` baskets of `per` entries each, basket k at 1000 + 100 k, 50 bytes long.
struct Fixture {
  std::unordered_map<uint32_t, BranchBaskets> branches;
  std::map<uint64_t, std::pair<uint32_t, uint32_t>> bySeek;
  explicit Fixture(uint32_t nb, int64_t per) {
    BranchBaskets& b = branches[0];
    for (uint32_t k = 0; k < nb; ++k) {
      b.seek.push_back(1000 + 100 * k);
      b.bytes.push_back(50);
      b.entry.push_back(per * k);
      bySeek[1000 + 100 * k] = {0, k};
    }
    b.entry.push_back(per * nb);
  }
  static std::pair<uint64_t, uint64_t> basket(uint32_t k) {
    return {1000 + 100 * k, 1050 + 100 * k};
  }
  Ranges pick(const std::vector<int64_t>& clusters, const Ranges& asked, uint64_t& n) const {
    Ranges out;
    n = 0;
    clusterSiblings(clusters, branches, bySeek, asked, out, n);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }
};

} // namespace

TEST(ClusterSiblings, TheRestOfTheAskedBasketsCluster) {
  Fixture f(20, 1000); // two clusters of ten baskets
  uint64_t n = 0;
  const Ranges got = f.pick({0, 10000, 20000}, {Fixture::basket(3)}, n);
  Ranges want;
  for (uint32_t k = 0; k < 10; ++k)
    if (k != 3)
      want.push_back(Fixture::basket(k));
  EXPECT_EQ(got, want);
  EXPECT_EQ(n, 9u);
  // The second cluster's last basket: its own cluster only, the tree's end included.
  const Ranges last = f.pick({0, 10000, 20000}, {Fixture::basket(19)}, n);
  EXPECT_EQ(last.size(), 9u);
  EXPECT_EQ(last.front(), Fixture::basket(10));
  EXPECT_EQ(last.back(), Fixture::basket(18));
}

TEST(ClusterSiblings, ABasketSpanningClustersTakesBoth) {
  Fixture f(4, 1500); // baskets [0,1500) [1500,3000) [3000,4500) [4500,6000)
  uint64_t n = 0;
  // Clusters [0,2000) [2000,4000) [4000,6000): basket 1 spans the first two.
  const Ranges got = f.pick({0, 2000, 4000, 6000}, {Fixture::basket(1)}, n);
  EXPECT_EQ(got, (Ranges{Fixture::basket(0), Fixture::basket(2)}));
}

TEST(ClusterSiblings, NothingForWhatIsNotABasketStartOrNotKept) {
  Fixture f(20, 1000);
  uint64_t n = 0;
  // A range that starts no basket (the file's own records, say).
  EXPECT_TRUE(f.pick({0, 10000, 20000}, {{1010, 1040}}, n).empty());
  EXPECT_TRUE(f.pick({0, 10000, 20000}, {{0, 900}}, n).empty());
  // A basket of a branch whose baskets are not kept.
  Fixture g(20, 1000);
  g.bySeek[1000] = {7, 0};
  EXPECT_TRUE(g.pick({0, 10000, 20000}, {Fixture::basket(0)}, n).empty());
  // No clusters stated, or entries outside them.
  EXPECT_TRUE(f.pick({}, {Fixture::basket(3)}, n).empty());
  EXPECT_TRUE(f.pick({0, 5000}, {Fixture::basket(7)}, n).empty());
}

TEST(ClusterSiblings, ARecordThatDoesNotAddUpIsSkippedNotRead) {
  Fixture f(20, 1000);
  f.branches[0].seek[5] = 0;   // a basket with no position
  f.branches[0].bytes[6] = -1; // ... or no length
  uint64_t n = 0;
  const Ranges got = f.pick({0, 10000, 20000}, {Fixture::basket(3)}, n);
  EXPECT_EQ(got.size(), 7u);
  EXPECT_EQ(std::count(got.begin(), got.end(), Fixture::basket(5)), 0);
  // An index past the branch's lists, and lists of unequal lengths: nothing out of bounds.
  f.bySeek[1000 + 100 * 3] = {0, 40};
  EXPECT_TRUE(f.pick({0, 10000, 20000}, {Fixture::basket(3)}, n).empty());
  Fixture g(20, 1000);
  g.branches[0].seek.resize(4);
  g.branches[0].bytes.resize(4);
  const Ranges cut = g.pick({0, 10000, 20000}, {Fixture::basket(2)}, n);
  EXPECT_EQ(cut, (Ranges{Fixture::basket(0), Fixture::basket(1), Fixture::basket(3)}));
}
