// adaptive_federation = auto: whether the server a file's URL names is a
// redirector, from what the server says of itself.
//
// Two answers, both asked once per server (host:port) and process, before the
// first open there:
//  - its login flags (the kXR_protocol answer every XRootD server gives at
//    login, as XrdCl::FileSystem::Protocol asks it): kXR_isManager for a
//    redirector (with kXR_attrMeta for a federation's top one, kXR_attrSuper
//    for a supervisor), kXR_isServer for a data server, kXR_attrProxy for a
//    proxy. A server that does not say isManager, or says proxy, is not a
//    redirector, and nothing more is asked of it;
//  - for one that says isManager, its configured role (the configuration query
//    "role", `xrdfs <host> query config role`): "manager", "meta manager" or
//    "supervisor" confirm it; any other role it names ("none", "server") makes
//    it not a redirector. An EOS instance says isManager at login (it sends
//    each open to a storage node) and "none" as its role, and is not taken for
//    one. A server that names no role (an error, a key it does not know, no
//    answer in time) is taken at its login flags.
// A file opened at a redirector may be found at several data servers, which is
// what adaptive_federation is for. With `auto` the switch is on for it and off
// for every other.
//
// An open waits for the answers at most kRoleWaitS in all (or the reader's
// own open timeout when shorter); one that has none by then goes the way of a
// file whose server is not a redirector. An answer that comes after the bound
// is not taken. An outcome (an answer, no answer, or a verdict saved by another
// process of the cache, ServerVerdicts.h) is renewed -- asked again -- at an
// open that needs the server once it is older than the renewal age; a caller
// that may not wait takes the old outcome meanwhile. (The flags the open's own login brings come only with the open's
// answer, too late for that open's failover.)
//
// This header is pure (no XRootD, no I/O): the plugin asks (UCacheFile.cc) and
// hands the answers here.
//
// Thread-safety: the free functions are pure. ServerRoles is fully
// thread-safe; an Op's fields are read and written under its own mutex.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ucache {

// The most an open waits for its server's role, in seconds: a server that
// answers at all answers within a round trip, and an open must not wait longer
// for the question than it would for its own answer.
inline constexpr int kRoleWaitS = 5;
// The most servers whose role is kept; a server beyond it is not asked, and
// its files are read as with the switch off.
inline constexpr size_t kRolesMax = 4096;
// The longest role kept from an answer.
inline constexpr size_t kRoleMaxLen = 64;

// The role in an answer's bytes (`data`, `len`): its first line, without
// surrounding blanks, lower-cased; "" when there is none: empty, the word
// "role" itself (what a server answers for a key it does not know), longer
// than kRoleMaxLen, or holding a byte that is not printable.
std::string roleOfAnswer(const char* data, size_t len);
// Whether a role (as roleOfAnswer gives it) is a redirector's: it holds
// "manager" ("manager", "meta manager") or "supervisor".
bool roleIsRedirector(const std::string& role);
// Login flags (XProtocol.hh).
inline constexpr uint32_t kLoginIsServer = 0x1;
inline constexpr uint32_t kLoginIsManager = 0x2;
inline constexpr uint32_t kLoginAttrMeta = 0x100;
inline constexpr uint32_t kLoginAttrProxy = 0x200;
inline constexpr uint32_t kLoginAttrSuper = 0x400;
// Whether a server's login flags say redirector: isManager, and not a proxy.
bool flagsSayRedirector(uint32_t flags);
// The flags as a role's words: "meta manager", "supervisor", "manager",
// "proxy ...", "server", or "flags 0x..." for none of those.
std::string roleOfFlags(uint32_t flags);
// The verdict on one server from its login flags and, for one whose flags say
// redirector, its configured role: `roleAnswered` with `role` (roleOfAnswer's;
// "" for none it could name), or not answered (`why`).
struct RoleVerdict {
  bool redirector = false;
  std::string role; // what the log and the counters say of it
};
RoleVerdict roleVerdict(uint32_t flags, bool roleAnswered, const std::string& role,
                        const std::string& why);

// The roles of the servers this process has asked, and the queries in flight.
class ServerRoles {
 public:
  enum class Kind : uint8_t {
    kPending,    // asked, no answer yet
    kRedirector, // its role is a redirector's
    kOther,      // it gave another role (or none it could name)
    kNoAnswer,   // an error, or no answer within the wait
  };
  // One server's query and its outcome.
  struct Op {
    std::mutex mu;
    std::condition_variable cv;
    Kind kind = Kind::kPending;
    std::string role;  // the role it gave; for kNoAnswer, why there was none
    uint64_t untilUs = 0; // an answer later than this (steady clock) is not taken
    uint64_t boundMs = 0; // the query's bound, from its start
    uint64_t doneUs = 0;  // when it ended (steady clock); for a saved verdict, when it was saved
    bool saved = false;   // the verdict was read from the cache directory, not asked here
  };
  // What decide() found for one open.
  struct Decision {
    bool federation = false; // the switch is on for the file
    Kind kind = Kind::kPending; // the server's outcome when the open went on; kPending: none yet
    std::string role;
    bool asked = false; // this call made the server's query (start was called)
    bool ended = false; // this call ended the query as kNoAnswer (its wait ran out, or the
                        // query could not be made)
    std::shared_ptr<Op> op; // the server's query (null when it was not asked)
  };

  // The decision for an open at `server` (host:port, lower-cased). A server
  // not asked yet is asked: `start(op)` is called once, outside every lock, and
  // must arrange for answer(op, ...) to be called later (or return false when
  // it could not: the server then counts as kNoAnswer). The query has `boundMs`
  // to answer, counted from its start. With `mayWait`, the call waits for the
  // answer at most `waitMs` (and never past the query's own bound); without,
  // it takes what is known now. Never throws for want of memory to the caller
  // that may not: a failure is an open read as with the switch off.
  // `renewMs` > 0: an outcome older than that (an answer, a saved verdict, or no
  // answer) is asked again; a caller that may not wait meanwhile takes the old
  // outcome.
  Decision decide(const std::string& server, uint64_t boundMs, uint64_t waitMs, bool mayWait,
                  const std::function<bool(const std::shared_ptr<Op>&)>& start,
                  uint64_t renewMs = 0) noexcept;
  // A verdict saved in the cache directory `ageS` seconds ago: taken as the
  // server's outcome when this process has none, or only no answer. True when
  // it was taken.
  bool seed(const std::string& server, bool redirector, const std::string& role,
            uint64_t ageS) noexcept;
  // The answer to `op`'s query: `answered` with the verdict (`redirector`, and
  // `role` to say), or no answer (`role` then says why). True when this call
  // ends the query; false when it had ended already. An answer that comes
  // after the query's bound ends it as kNoAnswer: what it says is not taken.
  static bool answer(const std::shared_ptr<Op>& op, bool answered, bool redirector,
                     const std::string& role) noexcept;
  // What is known of `server` now, without asking it or waiting: federation
  // only when it has reported a redirector's role (kPending with no op when it
  // was never asked).
  Decision known(const std::string& server) const noexcept;
  // The servers asked so far.
  size_t size() const;
  // Now on the steady clock, in microseconds.
  static uint64_t nowUs();

 private:
  mutable std::mutex mu_;
  std::map<std::string, std::shared_ptr<Op>> ops_;
};

} // namespace ucache
