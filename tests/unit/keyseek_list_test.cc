// The list of file maps for check_origin_integrity (KeySeekList.h): a map stays while its file
// entry is alive, however many files are read at once, and is let go once the entry is gone,
// unless a parse fills it or a request waits for it.
#include "KeySeekList.h"

#include <gtest/gtest.h>

#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace {

struct Entry {};
struct Table {
  std::weak_ptr<Entry> entry;
  bool parsing = false;
  std::vector<std::function<void()>> waiters;
};

std::shared_ptr<Table> tableOf(const std::shared_ptr<Entry>& e) {
  auto t = std::make_shared<Table>();
  t->entry = e;
  return t;
}

} // namespace

TEST(KeySeekList, MapsOfLiveEntriesStayHoweverMany) {
  // More files read at once than the 32 the list once held: none of their maps may go.
  std::vector<std::shared_ptr<Entry>> live;
  std::deque<std::shared_ptr<Table>> d;
  for (int i = 0; i < 100; ++i) {
    live.push_back(std::make_shared<Entry>());
    ucache::dropUnusedKeySeekTables(d);
    d.push_back(tableOf(live.back()));
  }
  EXPECT_EQ(ucache::dropUnusedKeySeekTables(d), 0u);
  ASSERT_EQ(d.size(), 100u);
  for (size_t i = 0; i < d.size(); ++i)
    EXPECT_EQ(d[i]->entry.lock(), live[i]) << i; // each one found, in its order
}

TEST(KeySeekList, MapsOfGoneEntriesGo) {
  auto a = std::make_shared<Entry>(), b = std::make_shared<Entry>(), c = std::make_shared<Entry>();
  std::deque<std::shared_ptr<Table>> d{tableOf(a), tableOf(b), tableOf(c)};
  b.reset();
  EXPECT_EQ(ucache::dropUnusedKeySeekTables(d), 1u);
  ASSERT_EQ(d.size(), 2u);
  EXPECT_EQ(d[0]->entry.lock(), a);
  EXPECT_EQ(d[1]->entry.lock(), c);
  a.reset();
  c.reset();
  EXPECT_EQ(ucache::dropUnusedKeySeekTables(d), 2u);
  EXPECT_TRUE(d.empty());
}

TEST(KeySeekList, ParsingOrWaitedForStaysAfterItsEntry) {
  // A parse holds its entry, as do the requests that wait for it; should either outlive it, the
  // table they fill or wait on still stays until they are done.
  auto a = std::make_shared<Entry>(), b = std::make_shared<Entry>();
  auto ta = tableOf(a), tb = tableOf(b);
  ta->parsing = true;
  tb->waiters.push_back([] {});
  std::deque<std::shared_ptr<Table>> d{ta, tb};
  a.reset();
  b.reset();
  EXPECT_EQ(ucache::dropUnusedKeySeekTables(d), 0u);
  EXPECT_EQ(d.size(), 2u);
  ta->parsing = false;
  tb->waiters.clear();
  EXPECT_EQ(ucache::dropUnusedKeySeekTables(d), 2u);
  EXPECT_TRUE(d.empty());
}

TEST(KeySeekList, NullTablesGo) {
  auto a = std::make_shared<Entry>();
  std::deque<std::shared_ptr<Table>> d{nullptr, tableOf(a)};
  EXPECT_EQ(ucache::dropUnusedKeySeekTables(d), 1u);
  EXPECT_EQ(d.size(), 1u);
}
