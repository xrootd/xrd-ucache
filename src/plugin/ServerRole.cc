#include "ServerRole.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace ucache {

std::string roleOfAnswer(const char* data, size_t len) {
  if (!data)
    return std::string();
  size_t end = 0;
  while (end < len && data[end] != '\n' && data[end] != '\0')
    ++end;
  size_t b = 0;
  while (b < end && (data[b] == ' ' || data[b] == '\t' || data[b] == '\r'))
    ++b;
  while (end > b && (data[end - 1] == ' ' || data[end - 1] == '\t' || data[end - 1] == '\r'))
    --end;
  std::string role(data + b, end - b);
  if (role.empty() || role.size() > kRoleMaxLen)
    return std::string();
  for (char& c : role) {
    if (c < ' ' || c > '~')
      return std::string();
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return role == "role" ? std::string() : role;
}

bool roleIsRedirector(const std::string& role) {
  return role.find("manager") != std::string::npos || role.find("supervisor") != std::string::npos;
}

uint64_t ServerRoles::nowUs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

bool flagsSayRedirector(uint32_t flags) {
  return (flags & kLoginIsManager) && !(flags & kLoginAttrProxy);
}

std::string roleOfFlags(uint32_t flags) {
  char hex[32];
  std::snprintf(hex, sizeof hex, "flags 0x%x", flags);
  if (flags & kLoginAttrProxy)
    return (flags & kLoginIsManager) ? "proxy manager" : "proxy server";
  if (flags & kLoginIsManager)
    return (flags & kLoginAttrMeta)    ? "meta manager"
           : (flags & kLoginAttrSuper) ? "supervisor"
                                       : "manager";
  if (flags & kLoginIsServer)
    return "server";
  return hex;
}

RoleVerdict roleVerdict(uint32_t flags, bool roleAnswered, const std::string& role,
                        const std::string& why) {
  RoleVerdict v;
  if (!flagsSayRedirector(flags)) {
    v.role = roleOfFlags(flags) + " (login)";
    return v;
  }
  if (roleAnswered && !role.empty()) {
    v.redirector = roleIsRedirector(role);
    v.role = role + " (login: " + roleOfFlags(flags) + ")";
    return v;
  }
  v.redirector = true;
  v.role = roleOfFlags(flags) + " (login; no configured role: " +
           (roleAnswered ? std::string("none it could name") : why) + ")";
  return v;
}

bool ServerRoles::answer(const std::shared_ptr<Op>& op, bool answered, bool redirector,
                         const std::string& role) noexcept try {
  if (!op)
    return false;
  std::lock_guard<std::mutex> g(op->mu);
  if (op->kind != Kind::kPending)
    return false;
  if (nowUs() > op->untilUs) { // came after the bound, which no open had seen run out yet
    op->kind = Kind::kNoAnswer;
    op->role = "no answer within " + std::to_string((op->boundMs + 999) / 1000) + " s";
  } else {
    op->kind = !answered ? Kind::kNoAnswer : redirector ? Kind::kRedirector : Kind::kOther;
    op->role = role;
  }
  op->cv.notify_all();
  return true;
} catch (...) {
  return false;
}

ServerRoles::Decision ServerRoles::decide(
    const std::string& server, uint64_t boundMs, uint64_t waitMs, bool mayWait,
    const std::function<bool(const std::shared_ptr<Op>&)>& start) noexcept {
  Decision d;
  try {
    std::shared_ptr<Op> op;
    {
      std::lock_guard<std::mutex> g(mu_);
      const auto it = ops_.find(server);
      if (it != ops_.end()) {
        op = it->second;
      } else {
        if (server.empty() || ops_.size() >= kRolesMax)
          return d; // not asked: as with the switch off
        op = std::make_shared<Op>();
        op->untilUs = nowUs() + boundMs * 1000;
        op->boundMs = boundMs;
        ops_[server] = op;
        d.asked = true;
      }
    }
    if (d.asked) {
      bool started = false;
      try {
        started = start && start(op);
      } catch (...) {
      }
      if (!started)
        answer(op, false, false, "the query could not be made");
    }
    std::unique_lock<std::mutex> lk(op->mu);
    if (op->kind == Kind::kPending && mayWait) {
      const uint64_t now = nowUs();
      const uint64_t until = std::min(op->untilUs, now + waitMs * 1000);
      if (until > now)
        op->cv.wait_for(lk, std::chrono::microseconds(until - now),
                        [&op] { return op->kind != Kind::kPending; });
    }
    // The query's own bound is over with no answer: that is its outcome, whoever sees it first.
    if (op->kind == Kind::kPending && nowUs() >= op->untilUs) {
      op->kind = Kind::kNoAnswer;
      op->role = "no answer within " + std::to_string((op->boundMs + 999) / 1000) + " s";
      op->cv.notify_all();
      d.ended = true;
    }
    d.kind = op->kind;
    d.role = op->role;
    d.op = op;
    d.federation = op->kind == Kind::kRedirector;
  } catch (...) {
    d.federation = false;
  }
  return d;
}

ServerRoles::Decision ServerRoles::known(const std::string& server) const noexcept {
  Decision d;
  try {
    std::shared_ptr<Op> op;
    {
      std::lock_guard<std::mutex> g(mu_);
      const auto it = ops_.find(server);
      if (it == ops_.end())
        return d;
      op = it->second;
    }
    std::lock_guard<std::mutex> g(op->mu);
    d.kind = op->kind;
    d.role = op->role;
    d.federation = op->kind == Kind::kRedirector;
    d.op = op;
  } catch (...) {
    d = Decision();
  }
  return d;
}

size_t ServerRoles::size() const {
  std::lock_guard<std::mutex> g(mu_);
  return ops_.size();
}

} // namespace ucache
