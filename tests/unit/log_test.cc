#include "Log.h"

#include "TestUtil.h"
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace ucache;
using test::TempDir;

TEST(Log, LevelsAndFileOutput) {
  TempDir td;
  std::string path = td.path() + "/log.txt";
  Log::configure("info:" + path);
  EXPECT_TRUE(Log::enabled(LogLevel::kError));
  EXPECT_TRUE(Log::enabled(LogLevel::kWarn));
  EXPECT_TRUE(Log::enabled(LogLevel::kInfo));
  EXPECT_FALSE(Log::enabled(LogLevel::kDebug));
  UCACHE_ERROR("err %d", 1);
  UCACHE_WARN("warn %s", "two");
  UCACHE_INFO("info");
  UCACHE_DEBUG("suppressed");

  Log::configure("debug:" + path);
  EXPECT_TRUE(Log::enabled(LogLevel::kDebug));
  UCACHE_DEBUG("now visible");
  Log::configure("error:" + path);
  EXPECT_FALSE(Log::enabled(LogLevel::kWarn));
  // Oversized message is clamped, not overflowed.
  std::string big(5000, 'x');
  UCACHE_ERROR("%s", big.c_str());

  std::ifstream in(path);
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(all.find("ERROR] err 1"), std::string::npos);
  EXPECT_NE(all.find("WARN] warn two"), std::string::npos);
  EXPECT_NE(all.find("INFO] info"), std::string::npos);
  EXPECT_EQ(all.find("suppressed"), std::string::npos);
  EXPECT_NE(all.find("now visible"), std::string::npos);
  // Reset for the rest of the test binary: a spec without a file is stderr again.
  Log::configure("warn");
}

// The logger is process-wide state: each of these runs in a child process.
namespace {
int inChild(int (*body)(const std::string& path), const std::string& path) {
  pid_t pid = ::fork();
  if (pid == 0)
    ::_exit(body(path));
  int st = 0;
  ::waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}
std::string slurp(const std::string& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
size_t count(const std::string& all, const std::string& what) {
  size_t n = 0;
  for (size_t p = all.find(what); p != std::string::npos; p = all.find(what, p + 1))
    ++n;
  return n;
}
std::vector<std::string>* gSunk = nullptr;
bool sink(LogLevel, const char* msg) {
  gSunk->emplace_back(msg);
  return true;
}
} // namespace

// A flood of one warning (200,000 CRC lines in one job's log) is cut to the
// first 20 and summaries; another kind still prints.
TEST(Log, RepeatedWarningsAreHeldBackAndCounted) {
  TempDir td;
  const std::string path = td.path() + "/log.txt";
  EXPECT_EQ(0, inChild(
                   [](const std::string& p) {
                     Log::configure("warn:" + p);
                     for (int i = 0; i < 50; ++i)
                       UCACHE_WARN("the same kind %d", i);
                     UCACHE_WARN("another kind");
                     Log::flushHeldBack();
                     return 0;
                   },
                   path));
  const std::string all = slurp(path);
  EXPECT_EQ(count(all, "] the same kind"), 20u) << all;
  EXPECT_NE(all.find("30 more like this were held back, the latest: the same kind 49"),
            std::string::npos)
      << all;
  EXPECT_EQ(count(all, "another kind"), 1u);
}

TEST(Log, AtInfoEveryWarningIsPrinted) {
  TempDir td;
  const std::string path = td.path() + "/log.txt";
  EXPECT_EQ(0, inChild(
                   [](const std::string& p) {
                     Log::configure("info:" + p);
                     for (int i = 0; i < 50; ++i)
                       UCACHE_WARN("every one %d", i);
                     return 0;
                   },
                   path));
  EXPECT_EQ(count(slurp(path), "] every one"), 50u);
}

// What is running, once, before the process's first warning.
TEST(Log, ContextPrecedesTheFirstWarningOnly) {
  TempDir td;
  const std::string path = td.path() + "/log.txt";
  EXPECT_EQ(0, inChild(
                   [](const std::string& p) {
                     Log::setContext([] { return std::string("CONTEXT LINE"); });
                     Log::configure("info:" + p);
                     UCACHE_INFO("an info line");
                     UCACHE_WARN("first warning");
                     UCACHE_WARN("second warning");
                     return 0;
                   },
                   path));
  const std::string all = slurp(path);
  EXPECT_EQ(count(all, "CONTEXT LINE"), 1u) << all;
  EXPECT_LT(all.find("an info line"), all.find("CONTEXT LINE"));
  EXPECT_LT(all.find("CONTEXT LINE"), all.find("first warning"));
}

// The plugin hands messages to the XRootD client's log; a file named in
// UCACHE_LOG takes them instead. The client's level raises uCache's and a
// later configure() does not lower it.
TEST(Log, SinkRaisedLevelAndSpec) {
  TempDir td;
  const std::string path = td.path() + "/log.txt";
  EXPECT_EQ(0, inChild(
                   [](const std::string& p) {
                     std::vector<std::string> got;
                     gSunk = &got;
                     Log::setSink(&sink);
                     Log::raiseLevel(LogLevel::kDebug);
                     Log::configure("warn");
                     if (!Log::enabled(LogLevel::kDebug) || Log::spec() != "warn")
                       return 1;
                     UCACHE_DEBUG("to the client log");
                     if (got.size() != 1 || got[0] != "to the client log")
                       return 2;
                     Log::configure("warn:" + p);
                     UCACHE_WARN("to the file");
                     return got.size() == 1 ? 0 : 3;
                   },
                   path));
  EXPECT_NE(slurp(path).find("] to the file"), std::string::npos);
}
