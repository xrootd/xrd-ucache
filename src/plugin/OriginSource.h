// A metadata parser's byte source over one cache entry: the byte cache when it
// holds the range, otherwise ONE synchronous page-rounded read from the origin,
// kept in the byte cache. What the parsers read this way is the file's own
// records -- header, directory, keys list, tree record, RNTuple anchor and
// envelopes -- which is exactly what the byte cache holds for any reader.
//
// Thread-safety: one instance per parse; the entry and the handle state it
// reads through are thread-safe.
#pragma once

#include "CacheStore.h"
#include "FileEntry.h"
#include "OriginInFlight.h"
#include "PluginSupport.h"
#include "ReadRounding.h"
#include "Trace.h"
#include "TreeMeta.h"
#include "UCacheFile.h"
#include "XrdClTimeout.h"

#include <XrdCl/XrdClFile.hh>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace ucache {

struct OriginSource : transpose::Source {
  static constexpr uint64_t kHeadBlock = 128 * 1024;

  std::shared_ptr<HandleState> st;
  std::shared_ptr<FileEntry> entry;
  bool readFailed = false; // a read from the origin failed (which says nothing of the file)
  std::string fetchedFrom; // the server of its last read from the origin ("" for none)
  XrdTimeout timeout = 0;  // of each read from the origin (0: the client's default)

  bool has(uint64_t off, uint64_t n) override {
    return off + n >= off && off + n <= entry->fileSize();
  }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    if (n == 0)
      return true;
    if (entry->hasRange(off, n) && entry->readCached(off, n, dst, /*account=*/false))
      return true;
    auto [ws, we] = roundSpan(entry->pageSize(), entry->fileSize(), off, n);
    return fetch(ws, we, dst, off, n);
  }
  // The file head as ONE block of the size ROOT's RNTuple reader asks for it
  // in (128 KiB at offset 0): piecemeal reads would otherwise leave that read
  // partly missing, and cost one more request. A TTree reader reads only the
  // first few hundred bytes, so this is for RNTuple files only.
  bool fetchHead() {
    const uint64_t n = std::min<uint64_t>(kHeadBlock, entry->fileSize());
    if (entry->hasRange(0, n))
      return true;
    std::vector<char> tmp(1);
    return fetch(0, n, tmp.data(), 0, 0);
  }

 private:
  bool fetch(uint64_t ws, uint64_t we, void* dst, uint64_t off, uint64_t n) {
    if (we - ws > UINT32_MAX)
      return false;
    std::vector<char> buf(we - ws);
    XrdCl::File* f = st->acquireInner();
    if (!f) {
      readFailed = true;
      return false;
    }
    uint32_t got = 0;
    const uint64_t t0 = nowUs();
    // Counted among the reads in flight to its server for as long as it is
    // out, as the asynchronous ones are (inFlight: those once it was counted,
    // itself included), but not among all of the process's origin reads,
    // which have only ever counted the asynchronous ones. Its answer names no
    // host, so it is put down to the server it was sent to.
    const std::string server = issueServer(*st, f);
    std::atomic<uint64_t>* srv = st->store ? serverInFlight(server) : nullptr;
    const uint64_t inFlight = srv ? srv->fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    XrdCl::XRootDStatus s =
        f->Read(ws, static_cast<uint32_t>(we - ws), buf.data(), got, st->originTimeout(timeout));
    if (srv)
      srv->fetch_sub(1, std::memory_order_relaxed);
    st->releaseInner();
    if (!s.IsOK())
      noteOriginFailure(*st, &entry->obs().servers, "records", ws, we - ws, s, server);
    if (!s.IsOK() || got < off + n - ws) {
      readFailed = true;
      return false;
    }
    if (st->store) {
      auto& stats = st->store->stats();
      stats.originBytes.fetch_add(got, std::memory_order_relaxed);
      stats.originReads.fetch_add(1, std::memory_order_relaxed);
      const uint64_t rt = nowUs() - t0;
      stats.originRtUs.add(rt);
      entry->obs().originRtUs.add(rt);
      entry->obs().servers.add(server, got, rt, inFlight);
      if (stats.tracer)
        stats.tracer->rec("wire", entry->key().key, ws, got, rt, true,
                          wireTraceFields(server, t0, inFlight));
    }
    entry->writePages(ws, got, buf.data());
    if (n)
      std::memcpy(dst, buf.data() + (off - ws), n);
    fetchedFrom = server;
    return true;
  }
};

} // namespace ucache
