#include "EntryNames.h"

#include "IOBackend.h"
#include "MetaFile.h"
#include "vendor/sha256.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <optional>
#include <mutex>
#include <pthread.h>
#include <random>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ucache {
namespace {

std::atomic<bool> gWritable{false};

bool exists(const std::string& path) {
  struct ::stat st;
  return ::stat(path.c_str(), &st) == 0;
}
std::string shard(const std::string& hash) { return hash.substr(0, 2); }
std::string objectBase(const std::string& dir, const std::string& hash) {
  return dir + "/objects/" + shard(hash) + "/" + hash;
}
bool entryExists(const std::string& dir, const std::string& hash) {
  return exists(objectBase(dir, hash) + ".meta");
}
bool isHash(const std::string& h) {
  return h.size() == 64 && std::all_of(h.begin(), h.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
// The header and key of the entry `hash` (its size, key and generation), when it exists.
std::optional<MetaData> entryHead(const std::string& dir, const std::string& hash) {
  return MetaFile::loadHead(RealIO::instance(), objectBase(dir, hash) + ".meta");
}

struct Target {
  std::string hash, key;
  uint32_t gen = 0;    // the generation of the entry named (MetaData::gen); 0 in a self alias
  bool checked = true; // a URL's alias: the URL's copy's size was compared with the entry's
};

// The alias of the name `nameHash`, when there is one and it parses (this format only: an alias
// of any other shape is no alias, and is replaced when the name is given again).
bool readAlias(const std::string& dir, const std::string& nameHash, Target& out) {
  std::ifstream in(aliasPath(dir, nameHash));
  std::string line;
  if (!in || !std::getline(in, line) || in.bad() || in.eof())
    return false; // none, or torn (no newline)
  const std::string head = "ucache-alias 2 ";
  // head, the entry's hash, " ", 6 hex digits of its generation, " ", c|u, " ", its key
  const size_t h = head.size(), g = h + 65, c = g + 7, k = c + 2;
  if (line.compare(0, h, head) != 0 || line.size() <= k)
    return false;
  const std::string hash = line.substr(h, 64);
  if (!isHash(hash) || line[h + 64] != ' ' || line[g + 6] != ' ' || line[c + 1] != ' ' ||
      (line[c] != 'c' && line[c] != 'u'))
    return false;
  uint32_t gen = 0;
  for (size_t i = g; i < g + 6; ++i) {
    const char x = line[i];
    const int v = (x >= '0' && x <= '9') ? x - '0' : (x >= 'a' && x <= 'f') ? x - 'a' + 10 : -1;
    if (v < 0)
      return false;
    gen = gen << 4 | static_cast<uint32_t>(v);
  }
  out.hash = hash;
  out.gen = gen;
  out.checked = line[c] == 'c';
  out.key = line.substr(k);
  return !out.key.empty();
}

// Whether the alias `t` of the name `nameHash` names an entry now: a self alias (the name's own
// entry), while that entry exists; any other, while the entry it names exists in the generation
// it names (its key too). A generation is given only once (nextGen), and a fresh start has none,
// so an alias left behind by any path (a list that lost its line, a crash, a race with a
// removal) is never followed again, whatever is cached under that hash later.
bool live(const std::string& dir, const std::string& nameHash, const Target& t) {
  if (t.hash == nameHash)
    return entryExists(dir, t.hash);
  if (t.gen == 0)
    return false;
  const auto h = entryHead(dir, t.hash);
  return h && h->gen == t.gen && h->key == t.key;
}

bool mkdirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); ++i)
    if (i == path.size() || path[i] == '/')
      if (::mkdir(path.substr(0, i).c_str(), 0755) != 0 && errno != EEXIST)
        return false;
  return true;
}

// A temporary name beside `path` that no other process or thread uses.
std::string tmpName(const std::string& path) {
  static std::atomic<uint64_t> seq{0};
  return path + ".tmp." + std::to_string(::getpid()) + "." +
         std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
}

uint32_t randomGen() {
  uint32_t v = 0;
  try {
    std::random_device rd;
    v = rd();
  } catch (...) {
  }
  v ^= static_cast<uint32_t>(::time(nullptr)) * 2654435761u ^ static_cast<uint32_t>(::getpid());
  v &= MetaData::kGenMask;
  return v ? v : 1;
}

// The next generation id of the cache: a counter in <dir>/names/generation (6 hex digits), moved
// on under a flock of that file, 1 to 0xffffff and round again; it starts at a random value. Two
// generations of one cache get one id only 16.7 million ids apart.
uint32_t nextGen(const std::string& dir) {
  const std::string nd = entryNamesDir(dir);
  uint32_t v = 0;
  const int fd =
      mkdirs(nd) ? ::open((nd + "/generation").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644) : -1;
  if (fd >= 0 && ::flock(fd, LOCK_EX) == 0) {
    char b[8] = {};
    uint32_t last = 0;
    bool ok = ::pread(fd, b, 7, 0) == 7 && b[6] == '\n';
    for (int i = 0; ok && i < 6; ++i) {
      const char x = b[i];
      const int d = (x >= '0' && x <= '9') ? x - '0' : (x >= 'a' && x <= 'f') ? x - 'a' + 10 : -1;
      ok = d >= 0;
      last = last << 4 | static_cast<uint32_t>(d < 0 ? 0 : d);
    }
    v = (ok ? last : randomGen()) % MetaData::kGenMask + 1;
    char out[8];
    std::snprintf(out, sizeof out, "%06x\n", v);
    (void)!::pwrite(fd, out, 7, 0);
    ::flock(fd, LOCK_UN);
  }
  if (fd >= 0)
    ::close(fd);
  return v ? v : randomGen();
}

// The generation of the entry `hash`, given one if it has none (its sidecar rewritten under the
// entry's lock, as the store writes it). 0 when the entry is not there or cannot be given one.
uint32_t stampEntry(const std::string& dir, const std::string& hash) {
  const auto h = entryHead(dir, hash);
  if (!h)
    return 0;
  if (h->gen)
    return h->gen;
  const std::string base = objectBase(dir, hash);
  const int fd = ::open((base + ".data").c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return 0;
  uint32_t gen = 0;
  if (::flock(fd, LOCK_EX) == 0) {
    IOBackend& io = RealIO::instance();
    auto m = MetaFile::load(io, base + ".meta");
    if (m && m->gen == 0) {
      m->gen = nextGen(dir);
      if (MetaFile::store(io, base + ".meta", *m, /*fsyncMeta=*/false) != 0)
        m->gen = 0;
    }
    if (m)
      gen = m->gen;
    ::flock(fd, LOCK_UN);
  }
  ::close(fd);
  return gen;
}

std::string aliasBody(const Target& t) {
  char g[8];
  std::snprintf(g, sizeof g, "%06x", t.gen & MetaData::kGenMask);
  return "ucache-alias 2 " + t.hash + " " + g + " " + (t.checked ? "c" : "u") + " " + t.key + "\n";
}

// Writes `t` apart, beside the alias of `nameHash`; "" when it could not.
std::string writeApart(const std::string& dir, const std::string& nameHash, const Target& t) {
  const std::string path = aliasPath(dir, nameHash);
  if (!mkdirs(path.substr(0, path.rfind('/'))))
    return "";
  const std::string tmp = tmpName(path);
  const std::string body = aliasBody(t);
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0)
    return "";
  const bool wrote = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
  ::close(fd);
  if (!wrote) {
    ::unlink(tmp.c_str());
    return "";
  }
  return tmp;
}

// Makes the alias of `nameHash` to `t`, whole or not at all: a file written apart and linked in,
// which fails when another process linked one first. An alias that names nothing now (dangling,
// of another generation, or not an alias at all) is replaced. True when this one is now the alias.
bool createAlias(const std::string& dir, const std::string& nameHash, const Target& t) {
  const std::string path = aliasPath(dir, nameHash);
  const std::string tmp = writeApart(dir, nameHash, t);
  if (tmp.empty())
    return false;
  bool linked = ::link(tmp.c_str(), path.c_str()) == 0;
  if (!linked && errno == EEXIST) {
    Target there;
    if (!readAlias(dir, nameHash, there) || !live(dir, nameHash, there)) {
      ::unlink(path.c_str());
      linked = ::link(tmp.c_str(), path.c_str()) == 0;
    }
  }
  ::unlink(tmp.c_str());
  return linked;
}

// Puts `t` in place of the alias of `nameHash`, whatever is there (a rename over it).
void replaceAlias(const std::string& dir, const std::string& nameHash, const Target& t) {
  const std::string tmp = writeApart(dir, nameHash, t);
  if (!tmp.empty() && ::rename(tmp.c_str(), aliasPath(dir, nameHash).c_str()) != 0)
    ::unlink(tmp.c_str());
}

// Adds `kind key` to the names file of `entryHash` unless it is there. One line, one write with
// O_APPEND (whole, or not at all for a line this short). A file grown past twice the names it keeps
// is written again compacted, apart and renamed over it (a line another process appends meanwhile
// may be lost from the list: the list is for reading and for removing aliases with their entry;
// an alias it lost is never followed once that entry goes, see live()).
void addName(const std::string& dir, const std::string& entryHash, const std::string& kind,
             const std::string& key) {
  if (key.empty())
    return;
  const std::string path = namesPath(dir, entryHash);
  const std::string line = kind + " " + key;
  size_t n = 0;
  {
    std::ifstream in(path);
    std::string l;
    while (std::getline(in, l) && !in.eof()) {
      if (l == line)
        return;
      ++n;
    }
  }
  if (!mkdirs(path.substr(0, path.rfind('/'))))
    return;
  if (n + 1 > 2 * kEntryNamesMax + 2) {
    EntryNameList now = entryNames(dir, entryHash);
    if (kind == "lfn")
      now.lfn = key;
    else
      now.urls.push_back(key);
    if (now.urls.size() > kEntryNamesMax)
      now.urls.erase(now.urls.begin(), now.urls.end() - static_cast<long>(kEntryNamesMax));
    std::string body = now.lfn.empty() ? std::string() : "lfn " + now.lfn + "\n";
    for (const auto& u : now.urls)
      body += "url " + u + "\n";
    const std::string tmp = tmpName(path);
    {
      std::ofstream out(tmp, std::ios::trunc);
      out << body;
      if (!out) {
        out.close();
        ::unlink(tmp.c_str());
        return;
      }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0)
      ::unlink(tmp.c_str());
    return;
  }
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0)
    return;
  const std::string out = line + "\n";
  (void)!::write(fd, out.data(), out.size());
  ::close(fd);
}

// The process's memory of resolutions made (a name's entry never changes once given), by the
// name's key and the URL's: never destroyed, made anew in a forked child (a parent's thread may
// have held its lock).
struct Memory {
  std::mutex* mu = new std::mutex;
  std::map<std::string, Target>* done = new std::map<std::string, Target>;
};
Memory& memory() {
  static Memory* m = [] {
    auto* p = new Memory;
    ::pthread_atfork(nullptr, nullptr, [] { memory() = Memory(); });
    return p;
  }();
  return *m;
}
constexpr size_t kMemoryMax = 4096;

UrlKey at(const UrlKey& k, const Target& t) {
  UrlKey r = k;
  r.hashHex = t.hash;
  r.key = t.key;
  return r;
}

} // namespace

void setEntryNamesWritable(bool writable) { gWritable.store(writable, std::memory_order_relaxed); }

std::string entryNamesDir(const std::string& cacheDir) { return cacheDir + "/names"; }
std::string aliasPath(const std::string& cacheDir, const std::string& nameHash) {
  return entryNamesDir(cacheDir) + "/" + shard(nameHash) + "/" + nameHash + ".to";
}
std::string namesPath(const std::string& cacheDir, const std::string& entryHash) {
  return entryNamesDir(cacheDir) + "/" + shard(entryHash) + "/" + entryHash + ".names";
}

namespace {
UrlKey resolve(const UrlKey& k, const std::string& dir, bool writable) noexcept;
} // namespace

UrlKey resolveEntry(const UrlKey& k, const std::string& dir) noexcept {
  return resolve(k, dir, gWritable.load(std::memory_order_relaxed));
}

namespace {
// What the alias of the URL of `k` (a key with a logical name) says of the URL, given that the
// name's entry is `t`, an entry that exists under another hash than the URL's.
enum class UrlState {
  kNone,      // no alias of the URL names `t` (the CLI's case, or one could not be made)
  kChecked,   // a name of `t`, its copy's size the entry's
  kUnchecked, // a name of `t`, its copy's size not compared yet
  kOwn        // the URL is its own entry (its copy is another size: keepOwnEntry)
};
UrlState urlAlias(const UrlKey& k, const Target& t, const std::string& dir, bool writable) {
  Target a;
  const bool has = readAlias(dir, k.urlHash, a);
  if (has && a.hash == k.urlHash && entryExists(dir, k.urlHash))
    return UrlState::kOwn; // kept apart while its own entry exists
  if (has && a.hash == t.hash && live(dir, k.urlHash, a))
    return a.checked ? UrlState::kChecked : UrlState::kUnchecked;
  if (!writable)
    return UrlState::kNone;
  Target to = t;
  to.gen = stampEntry(dir, t.hash);
  if (!to.gen)
    return UrlState::kNone;
  to.checked = false;
  // The URL's own entry, cached before (with the prefixes off, or by a process with other ones):
  // its size is a size of the URL's copy, compared here with no origin to ask.
  if (const auto own = entryHead(dir, k.urlHash)) {
    const auto there = entryHead(dir, t.hash);
    if (!there)
      return UrlState::kNone;
    if (own->fileSize != there->fileSize) {
      keepOwnEntry(k, dir);
      return UrlState::kOwn;
    }
    to.checked = true;
  }
  if (createAlias(dir, k.urlHash, to))
    return to.checked ? UrlState::kChecked : UrlState::kUnchecked;
  if (readAlias(dir, k.urlHash, a)) { // another process's, made meanwhile
    if (a.hash == k.urlHash && entryExists(dir, k.urlHash))
      return UrlState::kOwn;
    if (a.hash == t.hash && live(dir, k.urlHash, a))
      return a.checked ? UrlState::kChecked : UrlState::kUnchecked;
  }
  return UrlState::kNone;
}

// Whether the URL of `k` is kept apart (a self alias) and its own entry exists.
bool keptApart(const UrlKey& k, const std::string& dir) {
  Target a;
  return readAlias(dir, k.urlHash, a) && a.hash == k.urlHash && entryExists(dir, k.urlHash);
}

UrlKey ownOf(const UrlKey& k) {
  UrlKey r = k;
  r.hashHex = k.urlHash;
  r.key = k.urlKey;
  return r;
}

// The resolved key of `k` at `t` (an entry of the name of `k`), with what the caller needs to know.
UrlKey answer(const UrlKey& k, const Target& t, const std::string& dir, UrlState s) {
  UrlKey r = at(k, t);
  const bool other = t.hash != k.hashHex && t.hash != k.urlHash; // under another key's hash
  r.entryGen = other ? t.gen : 0;
  // A URL that is not the entry's own and whose copy's size was not compared with it is to be
  // compared before the entry is trusted for it (an entry that is not there yet is made from the
  // URL's own copy: nothing to compare).
  r.joined = t.hash != k.urlHash && s != UrlState::kChecked && entryExists(dir, t.hash);
  return r;
}

UrlKey resolve(const UrlKey& k, const std::string& dir, bool writable) noexcept try {
  if (dir.empty() || k.hashHex.size() != 64)
    return k;
  const bool named = k.logical() && !k.urlHash.empty();
  if (!named) {
    // A key with no logical name: its own entry; else, in a cache that has names, the entry its
    // alias names, in that entry's generation. Not remembered (the alias is looked at each time).
    Target t;
    if (entryExists(dir, k.hashHex))
      return k;
    if (!exists(entryNamesDir(dir)) || !readAlias(dir, k.hashHex, t) || t.hash == k.hashHex ||
        !live(dir, k.hashHex, t))
      return k;
    UrlKey r = at(k, t);
    r.entryGen = t.gen;
    r.urlKey = k.key; // the key's own, for a copy that turns out not to be the entry's
    r.urlHash = k.hashHex;
    r.joined = !t.checked;
    return r;
  }
  const std::string memoKey = dir + "\n" + k.key + "\n" + k.urlKey;
  Target t;
  {
    Memory m = memory();
    std::lock_guard<std::mutex> g(*m.mu);
    const auto it = m.done->find(memoKey);
    if (it != m.done->end())
      t = it->second;
  }
  if (!t.hash.empty()) {
    // A remembered entry holds while it is there: the name's own (always made there again), the
    // URL's own while it exists (and, in a process that gives names, while the name's alias names
    // it: an entry started afresh is named again), any other while it exists in the generation
    // remembered.
    Target via;
    const bool holds =
        t.hash == k.hashHex ||
        (t.hash == k.urlHash
             ? entryExists(dir, t.hash) &&
                   (!writable || (readAlias(dir, k.hashHex, via) && via.hash == t.hash &&
                                  live(dir, k.hashHex, via)))
             : live(dir, k.hashHex, t));
    if (!holds) {
      Memory m = memory();
      std::lock_guard<std::mutex> g(*m.mu);
      m.done->erase(memoKey);
      t = Target();
    } else if (t.hash == k.urlHash) {
      return answer(k, t, dir, UrlState::kNone);
    } else if (!entryExists(dir, t.hash)) {
      return keptApart(k, dir) ? ownOf(k) : answer(k, t, dir, UrlState::kNone);
    }
    // else: the URL's alias is looked at below (another process may have kept the URL apart,
    // or compared its size, since)
  }
  if (t.hash.empty()) {
    Target via;
    if (keptApart(k, dir)) {
      return ownOf(k); // its copy is not the name's entry's size: its own entry, while it exists
    } else if (entryExists(dir, k.hashHex)) {
      t = {k.hashHex, k.key};
    } else if (readAlias(dir, k.hashHex, via) && live(dir, k.hashHex, via)) {
      t = via; // a live alias only: a dangling one, or one of an earlier generation, is as none
    } else if (entryExists(dir, k.urlHash)) {
      // The URL's entry, cached before the name was used, becomes the name's.
      t = {k.urlHash, k.urlKey};
      Target won;
      if (writable && (t.gen = stampEntry(dir, t.hash)) != 0 && !createAlias(dir, k.hashHex, t) &&
          readAlias(dir, k.hashHex, won) && live(dir, k.hashHex, won))
        t = won; // another process named an entry first: that one
    } else {
      t = {k.hashHex, k.key}; // a new entry, under the name
    }
  }
  UrlState s = UrlState::kNone;
  if (t.hash != k.urlHash && entryExists(dir, t.hash)) {
    // What the URL's alias says (made now when it is missing): its own entry when its copy is
    // another size, kept apart (a self alias, also when another process made it meanwhile).
    s = urlAlias(k, t, dir, writable);
    if (s == UrlState::kOwn) {
      Memory m = memory();
      std::lock_guard<std::mutex> g(*m.mu);
      m.done->erase(memoKey);
      return ownOf(k);
    }
  }
  if (writable) {
    addName(dir, t.hash, "lfn", k.key);
    addName(dir, t.hash, "url", k.urlKey);
  }
  {
    Memory m = memory();
    std::lock_guard<std::mutex> g(*m.mu);
    if (m.done->size() >= kMemoryMax)
      m.done->clear();
    (*m.done)[memoKey] = t;
  }
  return answer(k, t, dir, s);
} catch (...) {
  return k;
}
} // namespace

EntryNameList entryNames(const std::string& cacheDir, const std::string& entryHash) {
  EntryNameList out;
  std::ifstream in(namesPath(cacheDir, entryHash));
  std::string l;
  std::vector<std::string> urls;
  while (std::getline(in, l)) {
    if (in.eof())
      break; // a last line with no newline: torn
    if (l.compare(0, 4, "lfn ") == 0)
      out.lfn = l.substr(4);
    else if (l.compare(0, 4, "url ") == 0) {
      const std::string u = l.substr(4);
      urls.erase(std::remove(urls.begin(), urls.end(), u), urls.end());
      urls.push_back(u);
    }
  }
  if (urls.size() > kEntryNamesMax)
    urls.erase(urls.begin(), urls.end() - static_cast<long>(kEntryNamesMax));
  out.urls = std::move(urls);
  return out;
}

void dropEntryNames(const std::string& cacheDir, const std::string& entryHash) {
  const std::string list = namesPath(cacheDir, entryHash);
  std::ifstream in(list);
  if (!in)
    return; // no names (and, in a cache that never had them, no names/ at all)
  std::string l;
  while (std::getline(in, l)) {
    if (l.size() < 5 || (l.compare(0, 4, "lfn ") != 0 && l.compare(0, 4, "url ") != 0))
      continue;
    const std::string nameHash = sha256Hex(l.substr(4));
    Target t;
    if (readAlias(cacheDir, nameHash, t) && t.hash == entryHash)
      ::unlink(aliasPath(cacheDir, nameHash).c_str());
  }
  ::unlink(list.c_str());
}

void keepOwnEntry(const UrlKey& k, const std::string& cacheDir) {
  if (k.urlHash.empty() || k.urlKey.empty())
    return;
  Target self{k.urlHash, k.urlKey};
  replaceAlias(cacheDir, k.urlHash, self); // over the alias to the name's entry
  Memory m = memory();
  std::lock_guard<std::mutex> g(*m.mu);
  const std::string tail = "\n" + k.urlKey;
  for (auto it = m.done->begin(); it != m.done->end();)
    if (it->first.size() >= tail.size() &&
        it->first.compare(it->first.size() - tail.size(), tail.size(), tail) == 0)
      it = m.done->erase(it);
    else
      ++it;
}

void confirmEntryName(const UrlKey& k, const std::string& cacheDir) noexcept try {
  if (cacheDir.empty() || k.urlHash.empty() || k.urlKey.empty() || k.urlHash == k.hashHex)
    return;
  Target a;
  const bool has = readAlias(cacheDir, k.urlHash, a);
  if (has && a.hash == k.urlHash && entryExists(cacheDir, k.urlHash))
    return; // kept apart (by another process meanwhile): left so
  Target t{k.hashHex, k.key};
  if (has && a.hash == t.hash && live(cacheDir, k.urlHash, a)) {
    if (!a.checked) {
      a.checked = true;
      replaceAlias(cacheDir, k.urlHash, a);
    }
    return;
  }
  if (!gWritable.load(std::memory_order_relaxed))
    return;
  t.gen = stampEntry(cacheDir, t.hash);
  if (t.gen)
    createAlias(cacheDir, k.urlHash, t); // checked: this URL's copy is the entry's size
} catch (...) {
}

void clearEntryNames(const std::string& cacheDir) {
  const std::string nd = entryNamesDir(cacheDir);
  std::vector<std::string> shards;
  if (RealIO::instance().listDir(nd, shards) < 0)
    return;
  for (const auto& sh : shards) {
    const std::string d = nd + "/" + sh;
    std::vector<std::string> files;
    if (RealIO::instance().listDir(d, files) >= 0) {
      for (const auto& f : files)
        ::unlink((d + "/" + f).c_str());
      ::rmdir(d.c_str());
    } else {
      ::unlink(d.c_str()); // a marker or the lock file
    }
  }
  ::rmdir(nd.c_str());
  Memory m = memory();
  std::lock_guard<std::mutex> g(*m.mu);
  m.done->clear();
}

size_t sweepEntryNames(const std::string& cacheDir, size_t budget) noexcept try {
  const std::string nd = entryNamesDir(cacheDir);
  if (budget == 0 || !exists(nd))
    return 0;
  // Where the last call of this process stopped: a shard and the last file looked at in it (the
  // files of a shard are taken in name order), so that every file is reached in turn however many
  // live ones a shard holds. Never destroyed; a call that finds another one running returns.
  struct Pos {
    std::mutex mu;
    unsigned shard = 0;
    std::string after;
  };
  static Pos* pos = new Pos;
  std::unique_lock<std::mutex> g(pos->mu, std::try_to_lock);
  if (!g.owns_lock())
    return 0;
  const time_t now = ::time(nullptr);
  size_t seen = 0, removed = 0;
  for (unsigned n = 0; n <= 256 && seen < budget; ++n) {
    char sh[3];
    std::snprintf(sh, sizeof sh, "%02x", pos->shard & 0xff);
    const std::string d = nd + "/" + sh;
    std::vector<std::string> files;
    if (RealIO::instance().listDir(d, files) >= 0) {
      std::sort(files.begin(), files.end());
      auto f = std::upper_bound(files.begin(), files.end(), pos->after);
      for (; f != files.end() && seen < budget; ++f) {
        ++seen;
        pos->after = *f;
        const std::string path = d + "/" + *f;
        if (f->size() == 67 && f->compare(64, 3, ".to") == 0) {
          Target t;
          if (!readAlias(cacheDir, f->substr(0, 64), t) || !live(cacheDir, f->substr(0, 64), t))
            removed += ::unlink(path.c_str()) == 0;
        } else if (f->size() == 70 && f->compare(64, 6, ".names") == 0) {
          if (!entryExists(cacheDir, f->substr(0, 64)))
            removed += ::unlink(path.c_str()) == 0;
        } else if (f->find(".tmp.") != std::string::npos) {
          // a temporary file left by a process that ended while writing it (an hour on)
          struct ::stat st;
          if (::stat(path.c_str(), &st) == 0 && st.st_mtime + 3600 < now)
            removed += ::unlink(path.c_str()) == 0;
        }
      }
      if (f != files.end())
        break; // the budget ran out inside this shard: the next call goes on from here
    }
    pos->shard = (pos->shard + 1) & 0xff;
    pos->after.clear();
  }
  return removed;
} catch (...) {
  return 0;
}

} // namespace ucache

namespace ucache {

long nameCachedEntries(const std::string& dir, const KeyRules& rules) noexcept try {
  if (dir.empty() || !rules.logical())
    return -1;
  const std::string nd = entryNamesDir(dir);
  const std::string marker = nd + "/named-" + rules.prefixesHash();
  if (exists(marker) || !mkdirs(nd))
    return -1;
  const int lock = ::open((nd + "/naming.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock < 0)
    return -1;
  if (::flock(lock, LOCK_EX | LOCK_NB) != 0 || exists(marker)) {
    ::close(lock);
    return -1;
  }
  struct Cached {
    std::string key, hash;
    uint64_t atime;
  };
  std::vector<Cached> all;
  IOBackend& io = RealIO::instance();
  const std::string objRoot = dir + "/objects";
  std::vector<std::string> shards;
  if (io.listDir(objRoot, shards) >= 0)
    for (const auto& sh : shards) {
      std::vector<std::string> files;
      if (io.listDir(objRoot + "/" + sh, files) < 0)
        continue;
      for (const auto& f : files) {
        if (f.size() != 69 || f.compare(64, 5, ".meta") != 0)
          continue;
        const auto m = MetaFile::loadSummary(io, objRoot + "/" + sh + "/" + f);
        if (m && m->key.compare(0, 4, "lfn:") != 0)
          all.push_back({m->key, f.substr(0, 64), m->atime});
      }
    }
  std::sort(all.begin(), all.end(), [](const Cached& a, const Cached& b) { return a.atime > b.atime; });
  long named = 0;
  for (const auto& c : all) {
    const std::string name = rules.mapStored(c.key);
    if (name.compare(0, 4, "lfn:") != 0)
      continue;
    UrlKey k;
    k.scheme = "lfn";
    k.key = name;
    k.hashHex = sha256Hex(name);
    k.urlKey = c.key;
    k.urlHash = c.hash;
    // An entry of another size than the name's (under the name's own hash, or the one its alias
    // names) is another copy: the resolution keeps its URL as its own (a self alias), as an open
    // would find, and it is not counted.
    resolve(k, dir, /*writable=*/true);
    if (!keptApart(k, dir))
      ++named;
  }
  std::ofstream(marker) << named << "\n";
  ::close(lock);
  return named;
} catch (...) {
  return -1;
}

} // namespace ucache
