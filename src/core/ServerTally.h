// Per data server account of one file's origin requests in one process: the
// bytes each server delivered, how many requests it answered, their answer
// times (sum and maximum), the most origin reads in flight when one of them was
// issued, and how many failed. The per-file records carry it as two flat
// fields (docs/FORMAT.md): `data_server`, the host that delivered the most
// bytes, and `data_servers`, the account itself.
//
// Servers are named "host:port" as the client reached them. Fed from the
// origin requests' completion handlers, so every call is arithmetic on a small
// map under its own mutex, and never blocks on anything else.
//
// Thread-safety: every member is safe to call from any thread.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ucache {

class ServerTally {
 public:
  // At most this many servers are named in text(); the rest are counted.
  static constexpr size_t kMaxNamed = 8;

  // One answered request: `bytes` delivered in `rtUs` (issue to completion),
  // issued while `inFlight` origin reads were outstanding (itself included).
  void add(const std::string& server, uint64_t bytes, uint64_t rtUs, uint64_t inFlight);
  // One failed request (every one: the failure lines are capped, this is not).
  void fail(const std::string& server);
  // Adds another account to this one (a relayed handle's into its entry's). What
  // the other account's own requests took (tookFrom) is not added.
  void addFrom(const ServerTally& o);
  // A server that delivered bytes to a request made outside add (read-ahead, which
  // keeps no account of its servers): counted by tookFrom only.
  void noteTook(const std::string& server);
  // Whether this account's own requests (add with bytes, noteTook) took bytes from
  // `server`: for a file's entry, whether the entry was given bytes that server sent.
  bool tookFrom(const std::string& server) const;
  bool empty() const;
  // The host:port that delivered the most bytes; "" when none did.
  std::string topServer() const noexcept;
  // The same host without its port.
  std::string top() const;
  // "host:port=bytes:reqs:rt_sum_us:rt_max_us:nc_max:fails;..." by bytes, most
  // first, at most kMaxNamed servers, then "+N" for the N not named; "" when
  // empty.
  std::string text() const;
  // The hosts (without their port) with at least one failed request.
  std::vector<std::string> failedHosts() const;

 private:
  struct Agg {
    uint64_t bytes = 0, reqs = 0, rtSum = 0, rtMax = 0, ncMax = 0, fails = 0;
  };
  mutable std::mutex mu_;
  std::map<std::string, Agg> m_;
  std::set<std::string> took_; // tookFrom's servers
};

} // namespace ucache
