#include "EntryNames.h"

#include "IOBackend.h"
#include "MetaFile.h"
#include "vendor/sha256.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <mutex>
#include <pthread.h>
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
bool entryExists(const std::string& dir, const std::string& hash) {
  return exists(dir + "/objects/" + shard(hash) + "/" + hash + ".meta");
}
bool isHash(const std::string& h) {
  return h.size() == 64 && std::all_of(h.begin(), h.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

struct Target {
  std::string hash, key;
};

// The alias of the name `nameHash`, when there is one and it parses.
bool readAlias(const std::string& dir, const std::string& nameHash, Target& out) {
  std::ifstream in(aliasPath(dir, nameHash));
  std::string line;
  if (!in || !std::getline(in, line) || in.bad())
    return false;
  const std::string head = "ucache-alias 1 ";
  if (line.compare(0, head.size(), head) != 0 || line.size() < head.size() + 66)
    return false;
  const std::string hash = line.substr(head.size(), 64);
  if (!isHash(hash) || line[head.size() + 64] != ' ')
    return false;
  out.hash = hash;
  out.key = line.substr(head.size() + 65);
  return !out.key.empty();
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

// Makes the alias of `nameHash` to `t`, whole or not at all: a file written apart and linked in,
// which fails when another process linked one first. An alias whose entry is gone (a dangling one:
// left by a removal that ended before its aliases went) is replaced. True when this one is now the
// alias.
bool createAlias(const std::string& dir, const std::string& nameHash, const Target& t) {
  const std::string path = aliasPath(dir, nameHash);
  if (!mkdirs(path.substr(0, path.rfind('/'))))
    return false;
  const std::string tmp = tmpName(path);
  const std::string body = "ucache-alias 1 " + t.hash + " " + t.key + "\n";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0)
    return false;
  const bool wrote = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
  ::close(fd);
  bool linked = wrote && ::link(tmp.c_str(), path.c_str()) == 0;
  if (wrote && !linked && errno == EEXIST) {
    Target there;
    if (!readAlias(dir, nameHash, there) || !entryExists(dir, there.hash)) {
      ::unlink(path.c_str()); // dangling, or not an alias at all
      linked = ::link(tmp.c_str(), path.c_str()) == 0;
    }
  }
  ::unlink(tmp.c_str());
  return linked;
}

// Adds `kind key` to the names file of `entryHash` unless it is there. One line, one write with
// O_APPEND (whole, or not at all for a line this short). A file grown past twice the names it keeps
// is written again compacted, apart and renamed over it (a line another process appends meanwhile
// may be lost from the list: the list is for reading, the aliases hold the names).
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
      if (!out)
        return;
    }
    ::rename(tmp.c_str(), path.c_str());
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
UrlKey resolve(const UrlKey& k, const std::string& dir, bool writable) noexcept try {
  if (dir.empty() || k.hashHex.size() != 64)
    return k;
  const bool named = k.logical() && !k.urlHash.empty();
  const std::string memoKey = dir + "\n" + k.key + "\n" + (named ? k.urlKey : std::string());
  {
    Memory m = memory();
    std::lock_guard<std::mutex> g(*m.mu);
    const auto it = m.done->find(memoKey);
    // A remembered entry under another name's hash holds only while it exists: once it is gone,
    // the name is resolved again (a new entry is made only under the name's own hash).
    if (it != m.done->end() && (it->second.hash == k.hashHex || entryExists(dir, it->second.hash)))
      return at(k, it->second);
    if (it != m.done->end())
      m.done->erase(it);
  }
  Target t;
  bool decided = false;
  bool joined = false; // this URL is a name of another URL's entry from now on (its size unseen)
  if (!named) {
    // A key with no logical name: its own entry; else, in a cache that has names, its alias.
    // Not remembered: the alias is followed only while the entry it names exists (removed, the
    // URL's key is its own again, so that no file is cached under another's name without names).
    if (entryExists(dir, k.hashHex))
      return k;
    if (!exists(entryNamesDir(dir)) || !readAlias(dir, k.hashHex, t) || !entryExists(dir, t.hash))
      return k;
    return at(k, t);
  } else {
    Target via;
    // A URL kept on its own entry (keepOwnEntry: its copy is not the name's size) stays its own
    // while that entry exists.
    if (readAlias(dir, k.urlHash, via) && via.hash == k.urlHash && entryExists(dir, k.urlHash))
      return at(k, via);
    if (entryExists(dir, k.hashHex)) {
      t = {k.hashHex, k.key};
    } else if (readAlias(dir, k.hashHex, via) && entryExists(dir, via.hash)) {
      t = via; // a live alias only: a dangling one is as none (never an entry under its hash)
    } else if (entryExists(dir, k.urlHash)) {
      // The URL's entry, cached before the name was used, becomes the name's.
      t = {k.urlHash, k.urlKey};
      Target won;
      if (writable && !createAlias(dir, k.hashHex, t) && readAlias(dir, k.hashHex, won) &&
          entryExists(dir, won.hash))
        t = won; // another process named an entry first: that one
    } else {
      t = {k.hashHex, k.key}; // a new entry, under the name
    }
    decided = true;
    if (writable) {
      if (t.hash != k.urlHash) {
        Target own;
        if (!readAlias(dir, k.urlHash, own) || own.hash != t.hash) {
          if (createAlias(dir, k.urlHash, t))
            joined = entryExists(dir, t.hash) && !entryExists(dir, k.urlHash);
        }
      }
      addName(dir, t.hash, "lfn", k.key);
      addName(dir, t.hash, "url", k.urlKey);
    }
  }
  if (decided) {
    Memory m = memory();
    std::lock_guard<std::mutex> g(*m.mu);
    if (m.done->size() >= kMemoryMax)
      m.done->clear();
    (*m.done)[memoKey] = t;
  }
  UrlKey r = at(k, t);
  r.joined = joined;
  return r;
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
  const std::string path = aliasPath(cacheDir, k.urlHash);
  if (!mkdirs(path.substr(0, path.rfind('/'))))
    return;
  const std::string tmp = tmpName(path);
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << "ucache-alias 1 " << k.urlHash << " " << k.urlKey << "\n";
    if (!out)
      return;
  }
  ::rename(tmp.c_str(), path.c_str()); // over the alias to the name's entry
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
  if (!exists(nd))
    return 0;
  static std::atomic<unsigned> next{0};
  size_t seen = 0, removed = 0;
  for (unsigned n = 0; n < 256 && seen < budget; ++n) {
    char sh[3];
    std::snprintf(sh, sizeof sh, "%02x", (next.fetch_add(1, std::memory_order_relaxed)) & 0xff);
    const std::string d = nd + "/" + sh;
    std::vector<std::string> files;
    if (RealIO::instance().listDir(d, files) < 0)
      continue;
    for (const auto& f : files) {
      if (++seen > budget)
        break;
      if (f.size() == 67 && f.compare(64, 3, ".to") == 0) {
        Target t;
        if (!readAlias(cacheDir, f.substr(0, 64), t) || !entryExists(cacheDir, t.hash))
          removed += ::unlink((d + "/" + f).c_str()) == 0;
      } else if (f.size() == 70 && f.compare(64, 6, ".names") == 0) {
        if (!entryExists(cacheDir, f.substr(0, 64)))
          removed += ::unlink((d + "/" + f).c_str()) == 0;
      }
    }
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
    uint64_t atime, size;
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
          all.push_back({m->key, f.substr(0, 64), m->atime, m->fileSize});
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
    // A name already given to an entry of another size: this one is another copy, and keeps its URL
    // as its own (no alias), as an open would find (resolveEntry's caller compares the sizes).
    Target first;
    if (readAlias(dir, k.hashHex, first) && first.hash != c.hash && entryExists(dir, first.hash)) {
      const auto a = MetaFile::loadSummary(io, dir + "/objects/" + shard(first.hash) + "/" + first.hash + ".meta");
      if (!a || a->fileSize != c.size) {
        keepOwnEntry(k, dir);
        continue;
      }
    }
    resolve(k, dir, /*writable=*/true);
    ++named;
  }
  std::ofstream(marker) << named << "\n";
  ::close(lock);
  return named;
} catch (...) {
  return -1;
}

} // namespace ucache
