// Crash suite: fork a populator child that caches pages from a
// source buffer as fast as it can (with periodic sidecar flushes), SIGKILL
// it at a random point, then reopen the entry in the parent and prove ZERO
// servable-but-wrong pages: every page the cache claims to have must be
// (a) CRC-clean and (b) byte-identical to the source. Repeats
// UCACHE_CRASH_ITERS times (default 200; set lower for quick runs).
//
// Exit code 0 = all iterations clean; 1 = violation (seed and iter printed).
//
// `ucache-crash-harness records`: the same for the per-file record file. The
// child queues record lines of the newer kinds (an origin open) and writes
// per-file records of the older shape (a relayed file) through the store, with
// checkpoints between, and is SIGKILLed at a random point. The parent checks
// that every line in the file is complete except at most a torn last one, that
// no line written whole is missing, that the run reader takes exactly the
// older-shape lines as files (the torn tail skipped), and that it reads them
// the same as from a copy without the newer kinds.
#include "CacheStore.h"
#include "RunLog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <ftw.h>
#include <functional>
#include <random>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace ucache;

namespace {

int rmCb(const char* p, const struct stat*, int, struct FTW*) { return ::remove(p); }
void rmTree(const std::string& p) { ::nftw(p.c_str(), rmCb, 16, FTW_DEPTH | FTW_PHYS); }

std::vector<uint8_t> randomBytes(size_t n, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<uint8_t> out(n);
  for (size_t i = 0; i < n; ++i)
    out[i] = static_cast<uint8_t>(rng());
  return out;
}

uint64_t envU64(const char* name, uint64_t dflt) {
  const char* v = ::getenv(name);
  return v ? ::strtoull(v, nullptr, 10) : dflt;
}

// The child populates forever until killed.
[[noreturn]] void childLoop(const std::string& dir, const std::vector<uint8_t>& src,
                            uint64_t seed) {
  Config cfg;
  cfg.cacheDir = dir;
  cfg.pageSize = 4096;
  cfg.metaFlushSeconds = 0; // flush aggressively: maximize meta/data races
  RealIO io;
  Stats stats;
  UrlKey key = *UrlKey::parse("root://crash//f");
  auto e = FileEntry::open(io, cfg, stats, key, src.size(), 0, MetaData::kCksumNone, 0);
  if (!e)
    ::_exit(2);
  std::mt19937_64 rng(seed);
  for (;;) {
    uint64_t len = 1 + rng() % (128 * 1024);
    len = std::min<uint64_t>(len, src.size());
    uint64_t off = rng() % (src.size() - len + 1);
    e->writePages(off, len, src.data() + off);
    if (rng() % 16 == 0)
      e->flushMeta(true);
  }
}

// Returns number of wrong pages (0 = clean).
uint64_t verifyAfterCrash(const std::string& dir, const std::vector<uint8_t>& src) {
  Config cfg;
  cfg.cacheDir = dir;
  cfg.pageSize = 4096;
  RealIO io;
  Stats stats;
  UrlKey key = *UrlKey::parse("root://crash//f");
  auto e = FileEntry::open(io, cfg, stats, key, src.size(), 0, MetaData::kCksumNone, 0);
  if (!e) {
    std::fprintf(stderr, "verify: reopen failed\n");
    return 1;
  }
  // (a) CRC scrub.
  auto scrub = e->verifyAll();
  // (b) byte-truth: every page still claimed present must equal the source.
  uint64_t wrong = 0;
  const uint32_t P = e->pageSize();
  std::vector<uint8_t> buf(P);
  uint64_t npages = (src.size() + P - 1) / P;
  uint64_t present = 0;
  for (uint64_t i = 0; i < npages; ++i) {
    uint64_t off = i * P;
    uint64_t len = std::min<uint64_t>(P, src.size() - off);
    if (!e->hasRange(off, len))
      continue;
    ++present;
    if (!e->readCached(off, len, buf.data())) {
      ++wrong; // claimed present but unreadable-clean => counts as wrong
      continue;
    }
    if (std::memcmp(buf.data(), src.data() + off, len) != 0)
      ++wrong;
  }
  std::printf("  scrubbed=%llu bad_crc=%llu present=%llu wrong=%llu\n",
              static_cast<unsigned long long>(scrub.checked),
              static_cast<unsigned long long>(scrub.bad),
              static_cast<unsigned long long>(present),
              static_cast<unsigned long long>(wrong));
  // scrub.bad pages were quarantined (lost cache, acceptable); `wrong` pages
  // would have been SERVED wrong — the invariant that must never break.
  return wrong;
}

// ---- records mode -----------------------------------------------------------

// The child writes record lines until killed: newer-kind lines through the
// store's queue (numbered by "n"), older-shape per-file records of relayed
// files (named r0, r1, ...), and a checkpoint every few lines, which writes
// the queued lines and a counter line.
[[noreturn]] void recordsChild(const std::string& dir, uint64_t seed) {
  Config cfg;
  cfg.cacheDir = dir;
  RealIO io;
  CacheStore store(io, cfg);
  std::mt19937_64 rng(seed);
  const std::string pad(64 + seed % 160, 'x'); // longer lines, more ways to tear
  for (uint64_t n = 0, j = 0;; ++n) {
    store.enqueueRecord("{\"kind\":\"open\",\"v\":1,\"ts_ms\":1,\"file\":\"root://crash//r\","
                        "\"n\":" +
                        std::to_string(n) + ",\"msg\":\"" + pad + "\"}\n");
    if (rng() % 3 == 0) {
      store.recordRelayObs("root://crash//r" + std::to_string(j), 1000 + j, "relay", 5, 0);
      ++j;
    }
    if (rng() % 4 == 0)
      store.checkpoint();
  }
}

// The number after `"<name>":` in a line, or -1.
long long numberAfter(const std::string& line, const char* name) {
  const std::string k = std::string("\"") + name + "\":";
  const size_t p = line.find(k);
  return p == std::string::npos ? -1 : std::atoll(line.c_str() + p + k.size());
}

// The relayed file a per-file record names (r<j>, its key "root://crash:1094/r<j>"),
// or -1.
long long relayIndex(const std::string& line) {
  const std::string k = "\"key\":\"root://crash:1094/r";
  const size_t p = line.find(k);
  return p == std::string::npos ? -1 : std::atoll(line.c_str() + p + k.size());
}

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream os;
  os << in.rdbuf();
  return os.str();
}

// The record file and the counter file a store left in `statsDir` ("" for none).
void statsPaths(const std::string& statsDir, std::string& recPath, std::string& counterPath) {
  recPath.clear();
  counterPath.clear();
  if (DIR* d = ::opendir(statsDir.c_str())) {
    while (dirent* e = ::readdir(d)) {
      const std::string n = e->d_name;
      if (n.size() > 12 && n.compare(n.size() - 12, 12, ".files.jsonl") == 0)
        recPath = statsDir + "/" + n;
      else if (n.size() > 6 && n.compare(n.size() - 6, 6, ".jsonl") == 0)
        counterPath = statsDir + "/" + n;
    }
    ::closedir(d);
  }
}

// `dir`/`sub`/stats: the counter file as it is, and the record file as `edit`
// makes it from the original's text. Returns `dir`/`sub`.
std::string editedCopy(const std::string& dir, const std::string& sub,
                       const std::function<std::string(const std::string&)>& edit) {
  std::string recPath, counterPath;
  statsPaths(dir + "/stats", recPath, counterPath);
  const std::string to = dir + "/" + sub;
  ::mkdir(to.c_str(), 0700);
  ::mkdir((to + "/stats").c_str(), 0700);
  if (!counterPath.empty())
    std::ofstream(to + "/stats" + counterPath.substr(counterPath.rfind('/')), std::ios::binary)
        << slurp(counterPath);
  if (!recPath.empty())
    std::ofstream(to + "/stats" + recPath.substr(recPath.rfind('/')), std::ios::binary)
        << edit(slurp(recPath));
  return to;
}

// Whether `l` is a newer-kind line (the queue's), not a per-file record.
bool isKindLine(const std::string& l) { return l.find("\"kind\":") != std::string::npos; }

// Returns the number of violations (0 = clean). `quiet`: report none of them
// (the control, which must have some). `drained`: the files are as the child
// left them, so a checkpoint's counter line means the queue was written before
// it (the checkpoint drains first, and the first line is queued before any
// checkpoint): kind lines must be there.
uint64_t verifyRecords(const std::string& dir, bool quiet = false, bool drained = false) {
  FILE* const err = quiet ? nullptr : stderr;
  const auto say = [err](const char* fmt, auto... a) {
    if (!err)
      return;
    if constexpr (sizeof...(a) == 0)
      std::fputs(fmt, err); // a message with no arguments is no format
    else
      std::fprintf(err, fmt, a...);
  };
  const std::string statsDir = dir + "/stats";
  std::string recPath, counterPath;
  statsPaths(statsDir, recPath, counterPath);
  const std::string text = recPath.empty() ? std::string() : slurp(recPath);
  uint64_t bad = 0;
  long long nextKind = 0, nextRelay = 0;
  std::string kept; // the file without the newer kinds
  size_t at = 0;
  const auto check = [&](const std::string& l, bool whole) {
    if (l.empty() || l.front() != '{' || l.back() != '}') {
      if (whole) {
        say("  incomplete line before the end: %s\n", l.c_str());
        ++bad;
      }
      return; // a torn tail: the readers skip it
    }
    if (isKindLine(l)) {
      if (l.find("\"key\"") != std::string::npos || numberAfter(l, "n") != nextKind) {
        say("  kind line out of order or keyed (want n=%lld): %s\n",
                     nextKind, l.c_str());
        ++bad;
      }
      ++nextKind;
      return;
    }
    if (relayIndex(l) != nextRelay) {
      say("  per-file record missing or out of order (want r%lld): %s\n",
                   nextRelay, l.c_str());
      ++bad;
    }
    ++nextRelay;
    kept += l + "\n";
  };
  for (size_t nl; (nl = text.find('\n', at)) != std::string::npos; at = nl + 1)
    check(text.substr(at, nl - at), true);
  check(text.substr(at), false);
  // The run reader: exactly the older-shape records are files, read the same
  // from a copy without the newer kinds.
  const auto runs = loadRuns(statsDir);
  size_t files = 0;
  for (const auto& r : runs)
    files += r.files.size();
  if (files != static_cast<size_t>(nextRelay)) {
    say("  the run reader found %zu files, the record file has %lld\n", files,
                 nextRelay);
    ++bad;
  }
  if (drained && nextKind == 0 && !counterPath.empty() &&
      slurp(counterPath).find("}\n") != std::string::npos) {
    say("  a checkpoint wrote its counter line, but no queued line reached the file\n");
    ++bad;
  }
  const auto plain =
      loadRuns(editedCopy(dir, "copy", [&](const std::string&) { return kept; }) + "/stats");
  if (plain.size() != runs.size()) {
    say("  %zu runs, %zu without the newer kinds\n", runs.size(), plain.size());
    ++bad;
  }
  for (size_t i = 0; i < runs.size() && i < plain.size(); ++i) {
    const auto& a = runs[i].files;
    const auto& b = plain[i].files;
    bool same = a.size() == b.size() && runs[i].sig == plain[i].sig;
    for (auto ia = a.begin(), ib = b.begin(); same && ia != a.end(); ++ia, ++ib)
      same = ia->first == ib->first && ia->second.wireBytes == ib->second.wireBytes &&
             ia->second.spanUs == ib->second.spanUs && ia->second.mode == ib->second.mode;
    if (!same) {
      say("  the newer kinds changed what the run reader reads\n");
      ++bad;
    }
  }
  std::printf("  kind lines=%lld per-file records=%lld torn tail=%s\n", nextKind, nextRelay,
              at < text.size() ? "yes" : "no");
  return bad;
}

int recordsMain() {
  const uint64_t iters = envU64("UCACHE_CRASH_ITERS", 200);
  const uint64_t seed0 = envU64("UCACHE_CRASH_SEED", std::random_device{}());
  std::printf("crash suite (records): %llu iterations, seed=%llu\n",
              static_cast<unsigned long long>(iters), static_cast<unsigned long long>(seed0));
  std::mt19937_64 rng(seed0);
  const char* tbase = ::getenv("TMPDIR");
  const std::string base = std::string(tbase ? tbase : "/tmp");
  for (uint64_t it = 0; it < iters; ++it) {
    const uint64_t seed = rng();
    std::string tpl = base + "/ucache-crash-XXXXXX";
    char* dir = ::mkdtemp(tpl.data());
    if (!dir) {
      std::perror("mkdtemp");
      return 1;
    }
    const pid_t pid = ::fork();
    if (pid == 0)
      recordsChild(dir, seed); // never returns
    if (pid < 0) {
      std::perror("fork");
      return 1;
    }
    ::usleep(200 + seed % 60000);
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    std::printf("iter %llu seed=%llu\n", static_cast<unsigned long long>(it),
                static_cast<unsigned long long>(seed));
    uint64_t bad = verifyRecords(dir, /*quiet=*/false, /*drained=*/true);
    // A kill landing inside a write is rare, so a torn last line is also made
    // by hand, at a random byte of the file's last line: the readers must skip
    // it and lose nothing before it.
    std::mt19937_64 cut(seed);
    const std::string torn = editedCopy(dir, "torn", [&](const std::string& t) {
      if (t.size() < 2)
        return t;
      const size_t last = t.rfind('\n', t.size() - 2); // the newline before the last line
      const size_t from = last == std::string::npos ? 0 : last + 1;
      return t.substr(0, from + 1 + cut() % (t.size() - 1 - from));
    });
    bad += verifyRecords(torn);
    // The control: with a whole line taken out, the check must fail;
    // otherwise it could not have seen a lost line. The line taken out has a
    // later whole line of its own type, so that its loss leaves a gap in that
    // type's numbering; a file with no such line has no control this time.
    bool removed = false;
    const std::string holed = editedCopy(dir, "holed", [&](const std::string& t) {
      std::vector<std::pair<size_t, size_t>> whole; // [begin, end) of each line ending in \n
      for (size_t at = 0, nl; (nl = t.find('\n', at)) != std::string::npos; at = nl + 1)
        whole.emplace_back(at, nl + 1);
      for (size_t i = 0; i < whole.size(); ++i)
        for (size_t j = i + 1; j < whole.size(); ++j) {
          const auto line = [&](size_t k) {
            return t.substr(whole[k].first, whole[k].second - whole[k].first);
          };
          if (isKindLine(line(i)) == isKindLine(line(j))) {
            removed = true;
            return t.substr(0, whole[i].first) + t.substr(whole[i].second);
          }
        }
      return t;
    });
    if (removed && verifyRecords(holed, /*quiet=*/true) == 0) {
      std::fprintf(stderr, "  the control (a line taken out) was not caught\n");
      ++bad;
    }
    rmTree(dir);
    if (bad) {
      std::fprintf(stderr, "FAIL: iter=%llu seed=%llu violations=%llu\n",
                   static_cast<unsigned long long>(it), static_cast<unsigned long long>(seed),
                   static_cast<unsigned long long>(bad));
      return 1;
    }
  }
  std::printf("crash suite (records): all %llu iterations clean\n",
              static_cast<unsigned long long>(iters));
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "records") == 0)
    return recordsMain();
  const uint64_t iters = envU64("UCACHE_CRASH_ITERS", 200);
  const uint64_t seed0 = envU64("UCACHE_CRASH_SEED", std::random_device{}());
  std::printf("crash suite: %llu iterations, seed=%llu\n",
              static_cast<unsigned long long>(iters),
              static_cast<unsigned long long>(seed0));
  std::mt19937_64 rng(seed0);

  const char* tbase = ::getenv("TMPDIR");
  std::string base = std::string(tbase ? tbase : "/tmp");

  for (uint64_t it = 0; it < iters; ++it) {
    uint64_t seed = rng();
    std::string tpl = base + "/ucache-crash-XXXXXX";
    char* dir = ::mkdtemp(tpl.data());
    if (!dir) {
      std::perror("mkdtemp");
      return 1;
    }
    size_t fileSize = 512 * 1024 + seed % (3 * 1024 * 1024);
    auto src = randomBytes(fileSize, seed);

    pid_t pid = ::fork();
    if (pid == 0)
      childLoop(dir, src, seed); // never returns
    if (pid < 0) {
      std::perror("fork");
      return 1;
    }
    // Kill at a random point: 200us .. 60ms of populating.
    ::usleep(200 + seed % 60000);
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);

    std::printf("iter %llu seed=%llu size=%zu\n", static_cast<unsigned long long>(it),
                static_cast<unsigned long long>(seed), fileSize);
    uint64_t wrong = verifyAfterCrash(dir, src);
    rmTree(dir);
    if (wrong != 0) {
      std::fprintf(stderr, "FAIL: iter=%llu seed=%llu wrong_pages=%llu\n",
                   static_cast<unsigned long long>(it),
                   static_cast<unsigned long long>(seed),
                   static_cast<unsigned long long>(wrong));
      return 1;
    }
  }
  std::printf("crash suite: all %llu iterations clean\n",
              static_cast<unsigned long long>(iters));
  return 0;
}
