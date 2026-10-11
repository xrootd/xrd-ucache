#include "ServerTally.h"

#include <algorithm>
#include <vector>

namespace ucache {

// add and fail are called from the client library's completion handlers, so
// nothing may leave them: a server not counted for want of memory is a lost
// count, never a job.
void ServerTally::add(const std::string& server, uint64_t bytes, uint64_t rtUs,
                      uint64_t inFlight) try {
  if (server.empty())
    return;
  std::lock_guard<std::mutex> g(mu_);
  Agg& a = m_[server];
  a.bytes += bytes;
  ++a.reqs;
  a.rtSum += rtUs;
  a.rtMax = std::max(a.rtMax, rtUs);
  a.ncMax = std::max(a.ncMax, inFlight);
  if (bytes)
    took_.insert(server);
} catch (...) {
}

void ServerTally::noteTook(const std::string& server) try {
  if (server.empty())
    return;
  std::lock_guard<std::mutex> g(mu_);
  took_.insert(server);
} catch (...) {
}

bool ServerTally::tookFrom(const std::string& server) const {
  std::lock_guard<std::mutex> g(mu_);
  return took_.count(server) != 0;
}

void ServerTally::fail(const std::string& server) try {
  if (server.empty())
    return;
  std::lock_guard<std::mutex> g(mu_);
  ++m_[server].fails;
} catch (...) {
}

void ServerTally::addFrom(const ServerTally& o) {
  if (&o == this)
    return;
  std::map<std::string, Agg> other;
  {
    std::lock_guard<std::mutex> g(o.mu_);
    other = o.m_;
  }
  std::lock_guard<std::mutex> g(mu_);
  for (const auto& [s, b] : other) {
    Agg& a = m_[s];
    a.bytes += b.bytes;
    a.reqs += b.reqs;
    a.rtSum += b.rtSum;
    a.rtMax = std::max(a.rtMax, b.rtMax);
    a.ncMax = std::max(a.ncMax, b.ncMax);
    a.fails += b.fails;
  }
}

bool ServerTally::empty() const {
  std::lock_guard<std::mutex> g(mu_);
  return m_.empty();
}

std::string ServerTally::topServer() const noexcept try {
  std::string best;
  uint64_t most = 0;
  std::lock_guard<std::mutex> g(mu_);
  for (const auto& [s, a] : m_)
    if (a.bytes > most) { // ties go to the first name, so the answer is stable
      most = a.bytes;
      best = s;
    }
  return best;
} catch (...) {
  return std::string(); // called from completion handlers too: nothing may leave it
}

std::string ServerTally::top() const {
  std::string best = topServer();
  // "host:port", or "[v6]:port": the port is what follows the last colon.
  const size_t colon = best.rfind(':');
  if (colon != std::string::npos && best.find(']', colon) == std::string::npos)
    best.resize(colon);
  return best;
}

std::vector<std::string> ServerTally::failedHosts() const {
  std::vector<std::string> out;
  std::lock_guard<std::mutex> g(mu_);
  for (const auto& [server, a] : m_)
    if (a.fails)
      out.push_back(server.substr(0, server.rfind(':')));
  return out;
}

std::string ServerTally::text() const {
  std::vector<std::pair<std::string, Agg>> v;
  {
    std::lock_guard<std::mutex> g(mu_);
    v.assign(m_.begin(), m_.end());
  }
  std::stable_sort(v.begin(), v.end(),
                   [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
  std::string out;
  for (size_t i = 0; i < v.size() && i < kMaxNamed; ++i) {
    const Agg& a = v[i].second;
    if (!out.empty())
      out += ';';
    out += v[i].first + '=' + std::to_string(a.bytes) + ':' + std::to_string(a.reqs) + ':' +
           std::to_string(a.rtSum) + ':' + std::to_string(a.rtMax) + ':' +
           std::to_string(a.ncMax) + ':' + std::to_string(a.fails);
  }
  if (v.size() > kMaxNamed)
    out += ";+" + std::to_string(v.size() - kMaxNamed);
  return out;
}

} // namespace ucache
