// adaptive_federation = auto (ServerRole.h): a server's login flags and the role
// in its answer to `query config role`, which are a redirector's, and the per-server table:
// one query per server, waits bounded, the first outcome standing. Pure; the
// query itself is made where the plugin runs.
#include "ServerRole.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace ucache;

namespace {
std::string roleOf(const char* s) { return roleOfAnswer(s, std::strlen(s)); }
uint64_t msSince(uint64_t t0Us) { return (ServerRoles::nowUs() - t0Us) / 1000; }
} // namespace

// The answers servers give, as measured, and what is made of them.
TEST(ServerRole, RoleOfAnswer) {
  EXPECT_EQ(roleOf("meta manager"), "meta manager");
  EXPECT_EQ(roleOf("manager\n"), "manager");
  EXPECT_EQ(roleOf("  server \r\n"), "server");
  EXPECT_EQ(roleOf("none"), "none");
  EXPECT_EQ(roleOf("Proxy Server"), "proxy server");
  EXPECT_EQ(roleOf("supervisor\nsecond line"), "supervisor");
  // A key the server does not know comes back as itself: no role.
  EXPECT_EQ(roleOf("role"), "");
  EXPECT_EQ(roleOf(""), "");
  EXPECT_EQ(roleOf("   \n"), "");
  EXPECT_EQ(roleOfAnswer(nullptr, 4), "");
  EXPECT_EQ(roleOf("man\x01" "ager"), "");
  EXPECT_EQ(roleOf(std::string(kRoleMaxLen + 1, 'm').c_str()), "");
  // The buffer need not be terminated: only `len` bytes are read.
  const char raw[] = {'m', 'a', 'n', 'a', 'g', 'e', 'r', 'X'};
  EXPECT_EQ(roleOfAnswer(raw, 7), "manager");
  const char nul[] = {'s', 'e', 'r', 'v', 'e', 'r', '\0', 'x'};
  EXPECT_EQ(roleOfAnswer(nul, sizeof nul), "server");
}

TEST(ServerRole, RedirectorRoles) {
  for (const char* r : {"manager", "meta manager", "supervisor", "proxy manager"})
    EXPECT_TRUE(roleIsRedirector(r)) << r;
  for (const char* r : {"server", "none", "proxy server", "", "peer"})
    EXPECT_FALSE(roleIsRedirector(r)) << r;
}

// The login flags, as servers give them (measured: a federation's top
// redirector, a regional, an EOS instance -- isManager at login, "none" as its
// configured role -- and a data server).
TEST(ServerRole, LoginFlags) {
  EXPECT_TRUE(flagsSayRedirector(kLoginIsManager));
  EXPECT_TRUE(flagsSayRedirector(kLoginIsManager | kLoginAttrMeta));
  EXPECT_TRUE(flagsSayRedirector(kLoginIsManager | kLoginAttrSuper));
  EXPECT_FALSE(flagsSayRedirector(kLoginIsManager | kLoginAttrProxy));
  EXPECT_FALSE(flagsSayRedirector(kLoginIsServer));
  EXPECT_FALSE(flagsSayRedirector(kLoginIsServer | kLoginAttrProxy));
  EXPECT_FALSE(flagsSayRedirector(0));
  EXPECT_EQ(roleOfFlags(kLoginIsManager | kLoginAttrMeta), "meta manager");
  EXPECT_EQ(roleOfFlags(kLoginIsManager | kLoginAttrSuper), "supervisor");
  EXPECT_EQ(roleOfFlags(kLoginIsManager), "manager");
  EXPECT_EQ(roleOfFlags(kLoginIsServer), "server");
  EXPECT_EQ(roleOfFlags(kLoginIsServer | kLoginAttrProxy), "proxy server");
  EXPECT_EQ(roleOfFlags(0), "flags 0x0");
}

TEST(ServerRole, Verdict) {
  // Not a redirector at login: decided there, the configured role never asked.
  auto v = roleVerdict(kLoginIsServer, false, "", "");
  EXPECT_FALSE(v.redirector);
  EXPECT_EQ(v.role, "server (login)");
  EXPECT_FALSE(roleVerdict(kLoginIsManager | kLoginAttrProxy, true, "manager", "").redirector);
  // A redirector at login whose configured role confirms it.
  v = roleVerdict(kLoginIsManager | kLoginAttrMeta, true, "meta manager", "");
  EXPECT_TRUE(v.redirector);
  EXPECT_EQ(v.role, "meta manager (login: meta manager)");
  EXPECT_TRUE(roleVerdict(kLoginIsManager, true, "manager", "").redirector);
  EXPECT_TRUE(roleVerdict(kLoginIsManager | kLoginAttrSuper, true, "supervisor", "").redirector);
  // An EOS instance: isManager at login, "none" as its role: not a redirector.
  v = roleVerdict(kLoginIsManager, true, "none", "");
  EXPECT_FALSE(v.redirector);
  EXPECT_EQ(v.role, "none (login: manager)");
  // No role it could name, or an error: its login flags decide.
  v = roleVerdict(kLoginIsManager, true, "", "");
  EXPECT_TRUE(v.redirector);
  EXPECT_EQ(v.role, "manager (login; no configured role: none it could name)");
  v = roleVerdict(kLoginIsManager, false, "", "[ERROR] Server responded with an error");
  EXPECT_TRUE(v.redirector);
  EXPECT_NE(v.role.find("[ERROR] Server responded"), std::string::npos);
}

// The first open of a server asks it once and waits for its answer; later
// opens take the answer without asking.
TEST(ServerRole, AskedOnceAndCached) {
  ServerRoles roles;
  int started = 0;
  auto answers = [&](const char* role) {
    return [&started, role](const std::shared_ptr<ServerRoles::Op>& op) {
      ++started;
      std::thread([op, role] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ServerRoles::answer(op, true, roleIsRedirector(roleOf(role)), roleOf(role));
      }).detach();
      return true;
    };
  };
  auto d = roles.decide("global.example:1094", 5000, 5000, true, answers("meta manager"));
  EXPECT_TRUE(d.asked);
  EXPECT_TRUE(d.federation);
  EXPECT_EQ(d.kind, ServerRoles::Kind::kRedirector);
  EXPECT_EQ(d.role, "meta manager");
  d = roles.decide("global.example:1094", 5000, 5000, true, answers("server"));
  EXPECT_FALSE(d.asked);
  EXPECT_TRUE(d.federation);
  EXPECT_EQ(started, 1);
  d = roles.decide("disk.example:1094", 5000, 5000, true, answers("server"));
  EXPECT_TRUE(d.asked);
  EXPECT_FALSE(d.federation);
  EXPECT_EQ(d.kind, ServerRoles::Kind::kOther);
  EXPECT_EQ(started, 2);
  EXPECT_EQ(roles.size(), 2u);
}

// Many first opens of one server at once: one query, and every one of them
// waits for its answer.
TEST(ServerRole, ConcurrentFirstOpensShareOneQuery) {
  ServerRoles roles;
  std::atomic<int> started{0};
  auto start = [&started](const std::shared_ptr<ServerRoles::Op>& op) {
    ++started;
    std::thread([op] {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      ServerRoles::answer(op, true, roleIsRedirector("manager"), "manager");
    }).detach();
    return true;
  };
  std::atomic<int> on{0};
  std::vector<std::thread> ts;
  for (int i = 0; i < 16; ++i)
    ts.emplace_back([&] {
      if (roles.decide("regional.example:1094", 5000, 5000, true, start).federation)
        ++on;
    });
  for (auto& t : ts)
    t.join();
  EXPECT_EQ(started.load(), 1);
  EXPECT_EQ(on.load(), 16);
}

// A server that does not answer: the open waits for the bound, no longer, and
// goes on as with the switch off; the outcome stands, and a late answer is not
// taken.
TEST(ServerRole, NoAnswerWithinTheBound) {
  ServerRoles roles;
  std::shared_ptr<ServerRoles::Op> held;
  auto never = [&held](const std::shared_ptr<ServerRoles::Op>& op) {
    held = op;
    return true;
  };
  const uint64_t t0 = ServerRoles::nowUs();
  auto d = roles.decide("stopped.example:1094", 300, 5000, true, never);
  const uint64_t took = msSince(t0);
  EXPECT_GE(took, 290u);
  EXPECT_LT(took, 2000u);
  EXPECT_TRUE(d.asked);
  EXPECT_TRUE(d.ended);
  EXPECT_FALSE(d.federation);
  EXPECT_EQ(d.kind, ServerRoles::Kind::kNoAnswer);
  EXPECT_EQ(d.role, "no answer within 1 s");
  EXPECT_FALSE(ServerRoles::answer(held, true, roleIsRedirector("manager"), "manager")); // late: ended already
  d = roles.decide("stopped.example:1094", 300, 5000, true, never);
  EXPECT_FALSE(d.asked);
  EXPECT_FALSE(d.federation);
  EXPECT_EQ(d.kind, ServerRoles::Kind::kNoAnswer);
}

// An answer that arrives after the bound, before any open saw the bound run
// out, still counts as none.
TEST(ServerRole, LateAnswerIsNoAnswer) {
  ServerRoles roles;
  std::shared_ptr<ServerRoles::Op> held;
  auto d = roles.decide("slow.example:1094", 50, 5000, /*mayWait=*/false,
                        [&held](const std::shared_ptr<ServerRoles::Op>& op) {
                          held = op;
                          return true;
                        });
  EXPECT_TRUE(d.asked);
  EXPECT_FALSE(d.federation);
  EXPECT_EQ(d.kind, ServerRoles::Kind::kPending);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  EXPECT_TRUE(ServerRoles::answer(held, true, roleIsRedirector("manager"), "manager"));
  d = roles.decide("slow.example:1094", 50, 5000, true, nullptr);
  EXPECT_EQ(d.kind, ServerRoles::Kind::kNoAnswer);
  EXPECT_FALSE(d.federation);
}

// A caller that may not wait takes what is known (nothing yet: off) and does
// not hold the query up; one that may waits for it.
TEST(ServerRole, MayNotWait) {
  ServerRoles roles;
  std::shared_ptr<ServerRoles::Op> held;
  auto d = roles.decide("r.example:1094", 5000, 5000, false,
                        [&held](const std::shared_ptr<ServerRoles::Op>& op) {
                          held = op;
                          return true;
                        });
  EXPECT_TRUE(d.asked);
  EXPECT_FALSE(d.federation);
  EXPECT_EQ(d.kind, ServerRoles::Kind::kPending);
  EXPECT_TRUE(ServerRoles::answer(held, true, roleIsRedirector("manager"), "manager"));
  d = roles.decide("r.example:1094", 5000, 5000, false, nullptr);
  EXPECT_TRUE(d.federation);
}

// A reader's own shorter open timeout bounds its wait; it takes off without
// ending the query, which a later open can still find answered.
TEST(ServerRole, ShorterWaitDoesNotEndTheQuery) {
  ServerRoles roles;
  std::shared_ptr<ServerRoles::Op> held;
  const uint64_t t0 = ServerRoles::nowUs();
  auto d = roles.decide("r.example:1094", 5000, 100, true,
                        [&held](const std::shared_ptr<ServerRoles::Op>& op) {
                          held = op;
                          return true;
                        });
  EXPECT_LT(msSince(t0), 2000u);
  EXPECT_FALSE(d.federation);
  EXPECT_FALSE(d.ended);
  EXPECT_EQ(d.kind, ServerRoles::Kind::kPending);
  EXPECT_TRUE(ServerRoles::answer(held, true, roleIsRedirector("manager"), "manager"));
  EXPECT_TRUE(roles.decide("r.example:1094", 5000, 100, true, nullptr).federation);
}

// A query that could not be made, or an error answer: off, and not asked again.
TEST(ServerRole, ErrorsAreOff) {
  ServerRoles roles;
  int started = 0;
  auto refuse = [&started](const std::shared_ptr<ServerRoles::Op>&) {
    ++started;
    return false;
  };
  auto d = roles.decide("a.example:1094", 5000, 5000, true, refuse);
  EXPECT_FALSE(d.federation);
  EXPECT_EQ(d.kind, ServerRoles::Kind::kNoAnswer);
  d = roles.decide("a.example:1094", 5000, 5000, true, refuse);
  EXPECT_FALSE(d.asked);
  EXPECT_EQ(started, 1);
  auto throws = [](const std::shared_ptr<ServerRoles::Op>&) -> bool { throw 1; };
  EXPECT_FALSE(roles.decide("b.example:1094", 5000, 5000, true, throws).federation);
  auto err = [](const std::shared_ptr<ServerRoles::Op>& op) {
    ServerRoles::answer(op, false, false, "[ERROR] Server responded with an error");
    return true;
  };
  d = roles.decide("c.example:1094", 5000, 5000, true, err);
  EXPECT_FALSE(d.federation);
  EXPECT_EQ(d.kind, ServerRoles::Kind::kNoAnswer);
  // No server: not asked.
  EXPECT_FALSE(roles.decide("", 5000, 5000, true, refuse).asked);
}
