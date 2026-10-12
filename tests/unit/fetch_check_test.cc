// The byte path's check made in parallel on the decode pool (FetchCheck.h), and the end of a
// parse that requests wait for (ParseEnd.h).
#include "Executor.h"
#include "FetchCheck.h"
#include "ParseEnd.h"
#include "TreeMeta.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace ucache;
using namespace ucache::transpose;

namespace {

struct Fixture {
  std::vector<uint8_t> b;
  std::vector<uint64_t> seeks;
  std::vector<int32_t> lengths;
  bool load() {
    FILE* fp = std::fopen(UCACHE_TEST_DATA_DIR "/unnamed_codec_fixture.root", "rb");
    if (!fp)
      return false;
    uint8_t buf[65536];
    size_t r;
    while ((r = std::fread(buf, 1, sizeof buf, fp)) > 0)
      b.insert(b.end(), buf, buf + r);
    std::fclose(fp);
    struct Src : Source {
      std::vector<uint8_t>* b;
      bool read(void* d, uint64_t n, uint64_t off) override {
        if (off > b->size() || n > b->size() - off)
          return false;
        std::copy(b->begin() + off, b->begin() + off + n, static_cast<uint8_t*>(d));
        return true;
      }
      bool has(uint64_t off, uint64_t n) override {
        return off <= b->size() && n <= b->size() - off;
      }
    } src;
    src.b = &b;
    const FileMeta fm = parseFile(src, static_cast<int64_t>(b.size()), "Events");
    if (!fm.error.empty())
      return false;
    basketMap(fm, seeks, lengths);
    return seeks.size() == 12;
  }
  // The whole file as three pieces, cut at `c1` and `c2`.
  std::vector<BytePiece> pieces(const std::vector<uint8_t>& bytes, uint64_t c1, uint64_t c2) const {
    const char* p = reinterpret_cast<const char*>(bytes.data());
    return {{0, p, c1}, {c1, p + c1, c2 - c1}, {c2, p + c2, bytes.size() - c2}};
  }
};

bool same(const FetchVerdict& a, const FetchVerdict& b) {
  if (a.passed != b.passed || a.none != b.none || a.faults.size() != b.faults.size())
    return false;
  for (size_t i = 0; i < a.faults.size(); ++i)
    if (a.faults[i].at != b.faults[i].at || a.faults[i].kind != b.faults[i].kind)
      return false;
  return true;
}

// Runs checkPiecesOn and waits for its one completion; `calls` counts completions.
FetchVerdict onPool(Executor& pool, const Fixture& f, std::vector<BytePiece> pieces,
                    const FetchCheckOptions& o, int& calls, std::thread::id& doneOn,
                    std::shared_ptr<const Ranges> reach = nullptr) {
  std::mutex mu;
  std::condition_variable cv;
  FetchVerdict out;
  calls = 0;
  checkPiecesOn(
      pool, nullptr, f.seeks, f.lengths, std::move(pieces), true, nullptr, "", reach,
      [&](FetchVerdict v) {
        std::lock_guard<std::mutex> g(mu);
        out = std::move(v);
        doneOn = std::this_thread::get_id();
        ++calls;
        cv.notify_all();
      },
      o);
  std::unique_lock<std::mutex> l(mu);
  cv.wait_for(l, std::chrono::seconds(30), [&] { return calls > 0; });
  l.unlock();
  std::this_thread::sleep_for(std::chrono::milliseconds(50)); // room for a second completion
  return out;
}

} // namespace

// The pool's verdict is the one a single walk over the bytes gives, whatever the cuts between the
// pieces and the jobs: every basket passes as ROOT wrote it, and a changed byte inside one basket's
// data, or one of its key's position bytes, is that basket's fault only. One completion.
TEST(FetchCheck, JobsGiveTheVerdictOfOneWalk) {
  Fixture f;
  ASSERT_TRUE(f.load());
  Executor pool(4);
  for (int variant = 0; variant < 3; ++variant) {
    std::vector<uint8_t> bytes = f.b;
    if (variant == 1)
      bytes[5999] ^= 0x10; // inside the data of the ZLIB basket at 4915
    if (variant == 2)
      bytes[4915 + 25] ^= 0x01; // its key's position (a 64-bit key: bytes 18-25)
    const FetchVerdict one = checkFetched(f.seeks, f.lengths, 0, bytes.data(), bytes.size(), true);
    EXPECT_EQ(one.faults.size(), variant ? 1u : 0u);
    for (uint64_t c1 : {100u, 4920u, 5000u, 7000u})
      for (uint64_t slice : {64u, 1000u, 1u << 20}) {
        const uint64_t c2 = c1 + 2481; // the second cut inside the next basket, or past it
        int calls = 0;
        std::thread::id on;
        FetchCheckOptions o;
        o.sliceBytes = slice;
        const FetchVerdict v = onPool(pool, f, f.pieces(bytes, c1, c2), o, calls, on);
        EXPECT_EQ(calls, 1) << c1 << " " << slice;
        EXPECT_TRUE(same(v, one)) << "variant " << variant << " cut " << c1 << " slice " << slice
                                  << ": " << v.passed << "/" << v.faults.size() << " against "
                                  << one.passed << "/" << one.faults.size();
      }
  }
}

// A large fetch's jobs run on the pool's threads, several at once, never on the caller's.
TEST(FetchCheck, JobsRunInParallelOnThePool) {
  Fixture f;
  ASSERT_TRUE(f.load());
  Executor pool(4);
  std::mutex mu;
  std::condition_variable cv;
  int inside = 0, most = 0;
  std::set<std::thread::id> threads;
  FetchCheckOptions o;
  o.sliceBytes = 512; // about 34 jobs over the file
  o.onJob = [&] {
    std::unique_lock<std::mutex> l(mu);
    threads.insert(std::this_thread::get_id());
    ++inside;
    most = std::max(most, inside);
    cv.notify_all();
    // Hold the job until a second one is inside too (or a while has passed): two at once.
    cv.wait_for(l, std::chrono::milliseconds(500), [&] { return inside >= 2; });
    --inside;
  };
  int calls = 0;
  std::thread::id on;
  const FetchVerdict v = onPool(pool, f, f.pieces(f.b, 4000, 9000), o, calls, on);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(v.passed, 12u);
  EXPECT_GE(most, 2) << "jobs ran one at a time";
  EXPECT_GE(threads.size(), 2u);
  EXPECT_EQ(threads.count(std::this_thread::get_id()), 0u) << "a job ran on the caller's thread";
  EXPECT_NE(on, std::this_thread::get_id()) << "the completion ran on the caller's thread";
}

// The decode pool is not the executor that serves hits: with every decode thread held, a task
// posted to the executor runs at once.
TEST(FetchCheck, AHitIsNotQueuedBehindDecoding) {
  Fixture f;
  ASSERT_TRUE(f.load());
  EXPECT_NE(&decodePool(), &Executor::instance());
  std::mutex mu;
  std::condition_variable cv;
  bool release = false;
  FetchCheckOptions o;
  o.sliceBytes = 256; // more jobs than the pool has threads
  o.onJob = [&] {
    std::unique_lock<std::mutex> l(mu);
    cv.wait_for(l, std::chrono::seconds(20), [&] { return release; });
  };
  std::atomic<int> done{0};
  checkPiecesOn(
      decodePool(), nullptr, f.seeks, f.lengths,
      {{0, reinterpret_cast<const char*>(f.b.data()), f.b.size()}}, true, nullptr, "", nullptr,
      [&](FetchVerdict) { done.fetch_add(1); }, o);
  std::atomic<bool> hit{false};
  Executor::instance().post([&] { hit = true; });
  for (int i = 0; i < 200 && !hit; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_TRUE(hit.load()) << "the executor's task waited for the decode pool";
  EXPECT_EQ(done.load(), 0) << "the decoding was held";
  {
    std::lock_guard<std::mutex> g(mu);
    release = true;
  }
  cv.notify_all();
  for (int i = 0; i < 500 && done.load() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(done.load(), 1);
}

// With `reach`, only the baskets the ranges reach are decoded: a damaged one outside them makes no
// fault and is not counted; one inside them does.
TEST(FetchCheck, ReachLimitsDecoding) {
  Fixture f;
  ASSERT_TRUE(f.load());
  std::vector<uint8_t> bytes = f.b;
  bytes[5999] ^= 0x10; // the basket at 4915 (2481 bytes)
  const std::vector<BytePiece> one{{0, reinterpret_cast<const char*>(bytes.data()), bytes.size()}};
  const Ranges away{{0, 4000}, {8000, 9000}};
  FetchVerdict v = checkPieces(f.seeks, f.lengths, one, true, nullptr, "", &away);
  EXPECT_TRUE(v.faults.empty());
  EXPECT_LT(v.passed + v.none, 12u) << "every basket decoded";
  const Ranges at{{5000, 5100}};
  v = checkPieces(f.seeks, f.lengths, one, true, nullptr, "", &at);
  ASSERT_EQ(v.faults.size(), 1u);
  EXPECT_EQ(v.faults[0].at, 4915u);
  EXPECT_EQ(v.passed + v.none, 0u) << "only the basket reached";
}

// A parse that requests wait for ends however it leaves: on an exception (with the table's lock
// let go, or held), the table no longer says parsing and every waiter is answered, once.
namespace {
struct Table {
  bool parsing = false;
  std::vector<std::function<void()>> waiters;
};
int g_posted = 0;
void postNow(std::function<void()>& f) {
  ++g_posted;
  f();
}
} // namespace
TEST(ParseEnd, WaitersAreAnsweredWhenTheParseThrows) {
  for (bool lockHeld : {false, true}) {
    std::mutex mu;
    Table t;
    int answered = 0;
    g_posted = 0;
    try {
      std::unique_lock<std::mutex> g(mu);
      t.parsing = true;
      ParseEnd<Table> end(t, g, &postNow);
      g.unlock();
      t.waiters.push_back([&] { ++answered; });
      t.waiters.push_back([&] { ++answered; });
      if (lockHeld)
        g.lock();
      throw std::runtime_error("the parse ran out of memory");
    } catch (const std::runtime_error&) {
    }
    EXPECT_FALSE(t.parsing) << lockHeld;
    EXPECT_TRUE(t.waiters.empty());
    EXPECT_EQ(answered, 2);
    EXPECT_EQ(g_posted, 2);
    EXPECT_TRUE(mu.try_lock()) << "the lock was left held";
    mu.unlock();
  }
  // Finished by hand: the destructor does nothing more.
  std::mutex mu;
  Table t;
  t.parsing = true;
  int answered = 0;
  t.waiters.push_back([&] { ++answered; });
  {
    std::unique_lock<std::mutex> g(mu);
    ParseEnd<Table> end(t, g, &postNow);
    end.finish();
  }
  EXPECT_EQ(answered, 1);
}
