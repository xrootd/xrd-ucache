#pragma once
#include "Stats.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace ucache {

// The origin reads this process has in flight to one data server ("host:port"),
// for the guard below. The client library keeps one channel per host:port in a
// process, so this is also the count on that server's connection. Taking it
// finds or inserts the server under one small mutex; the counter it returns is
// never freed, so giving a read back is a plain atomic decrement with no lock.
// A forked child starts from an empty set (the parent's is left behind, as the
// executor's core is), so it counts only its own reads. An empty name gives
// null: nothing is counted. So does a failure to take the lock or to make room
// (it runs on the client's callback threads, where nothing may leave it): a
// read not counted for want of memory is a lost count, never a lost job.
std::atomic<uint64_t>* serverInFlight(const std::string& server) noexcept;

// The host:port of a server as the client names it in a file's "DataServer"
// property, which can carry a user name ("user@host:port").
inline std::string serverOfHostId(std::string id) {
  const size_t at = id.rfind('@');
  if (at != std::string::npos)
    id.erase(0, at + 1);
  return id;
}

// One read outstanding at the ORIGIN, for as long as this object lives. It
// goes INSIDE the handler that owns a wire read, because that handler is
// constructed at the issue and destroyed exactly once -- on completion or on a
// failed issue -- which is the same span the origin sees. Counting on entry to
// Read and releasing on return measured the handoff instead: every route here
// dispatches and returns while the caller waits above this library, so 32
// threads with a read each reported a handful.
//
// It counts the read twice, and gives both back together: among all of the
// process's origin reads (Stats::originReadsInFlight and its high-water mark),
// and among those to the read's own server (serverInFlight).
struct OriginInFlight {
  Stats* s = nullptr;
  std::atomic<uint64_t>* srv = nullptr; // the server's count, until released
  // The origin reads in flight to this read's server once it was counted
  // (itself included), kept after release for the per-file record's
  // per-server maximum and the trace; 0 without stats, or when the server was
  // not known at the issue.
  uint64_t nc = 0;
  uint64_t t0 = 0; // when the read was issued, steady clock µs
  OriginInFlight() = default;
  // `server`: the server the read is sent to. The client picks the connection
  // as it sends, so callers name the server the file's handle uses (the one its
  // open reached); a read the client then has answered elsewhere is still
  // counted against the server it was sent to.
  explicit OriginInFlight(Stats* st, const std::string& server = std::string())
      : s(st), t0(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count())) {
    if (!s)
      return;
    const uint64_t n = s->originReadsInFlight.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t hw = s->originReadsInFlightHighWater.load(std::memory_order_relaxed);
    while (n > hw && !s->originReadsInFlightHighWater.compare_exchange_weak(
                         hw, n, std::memory_order_relaxed))
      ;
    srv = serverInFlight(server);
    if (srv)
      nc = srv->fetch_add(1, std::memory_order_relaxed) + 1;
  }
  OriginInFlight(const OriginInFlight&) = delete;
  OriginInFlight& operator=(const OriginInFlight&) = delete;
  OriginInFlight(OriginInFlight&& o) noexcept : s(o.s), srv(o.srv), nc(o.nc), t0(o.t0) {
    o.s = nullptr;
    o.srv = nullptr;
  }
  OriginInFlight& operator=(OriginInFlight&& o) noexcept {
    if (this != &o) {
      release();
      s = o.s;
      srv = o.srv;
      nc = o.nc;
      t0 = o.t0;
      o.s = nullptr;
      o.srv = nullptr;
    }
    return *this;
  }
  // Give the read back as soon as it has landed. Waiting for the destructor
  // means holding it across the caller's own completion handler, and a caller
  // that issues its next read from in there -- which is the shape that creates
  // real concurrency -- takes the next guard before this one is released. The
  // count would then be an artefact of when handlers happen to be destroyed
  // rather than a measure of what the origin was being asked for.
  void release() {
    if (s)
      s->originReadsInFlight.fetch_sub(1, std::memory_order_relaxed);
    if (srv)
      srv->fetch_sub(1, std::memory_order_relaxed);
    s = nullptr;
    srv = nullptr;
  }
  ~OriginInFlight() { release(); }
};

} // namespace ucache
