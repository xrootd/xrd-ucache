// The XRootD client's connection settings that `adaptive_federation = on`
// changes, and the rule for when it changes one.
//
// The client's own defaults suit a single server that is worth waiting for: a
// connection is tried for 120 s, 5 times, and a server that failed is answered
// with that failure for 1800 s. Through a federation another site usually holds
// the file, and waiting ten minutes on one that cannot be reached is time a job
// loses for nothing. So with the switch on, uCache sets at load:
//
//     ConnectionWindow   31 s   (one connection attempt)
//     ConnectionRetry     2     (attempts before the server counts as down)
//     StreamErrorWindow 180 s   (how long that failure is remembered)
//
// A value someone chose is never replaced: one set by an XRD_<NAME> variable
// (the client refuses the write anyway), or one that already differs from the
// client's built-in default (a client.conf, or the application). Nothing else
// is ever set; in particular the client's fork handling is left alone.
//
// This header is pure -- no XRootD, no I/O, no globals -- so the rule is
// testable on its own; the factory reads the client's values and applies the
// plan.
//
// Thread-safety: pure functions of their arguments and constant tables; safe to
// call concurrently.
#pragma once

#include <cstdint>

namespace ucache {

struct ClientSetting {
  const char* name;    // the client's name for it, as Env::PutInt takes it
  const char* envName; // the variable that sets it in the shell
  int federationValue; // what adaptive_federation = on sets it to
};

inline constexpr int kClientSettingCount = 3;
// ConnectionWindow, ConnectionRetry, StreamErrorWindow -- in that order in
// every array indexed by setting below.
extern const ClientSetting kClientSettings[kClientSettingCount];

enum class ClientSettingStep : uint8_t {
  kLeave,      // the switch is off: the client's value stands, untouched
  kApply,      // set it to federationValue
  kKeepEnv,    // an XRD_* variable sets it: that value is kept
  kKeepClient, // it already differs from the client's default: kept
};

struct ClientSettingsPlan {
  ClientSettingStep step[kClientSettingCount];
};

// The plan for the three settings. envSet[i]: kClientSettings[i].envName is set
// in the environment. currentEqualsDefault[i]: the client's value is its
// built-in default.
ClientSettingsPlan planClientSettings(bool switchOn, const bool envSet[kClientSettingCount],
                                      const bool currentEqualsDefault[kClientSettingCount]);

// Where the value in force came from, as the run line's `run_settings` names it
// (docs/STATS.md): "ucache" when uCache set it, "kept" when uCache would have
// and kept an XRD_* value instead (or the client refused the write), "client"
// for a value from a client.conf or the application, and with the switch off
// "env", "default" or "client" as for every other client setting. `applied`:
// the client accepted the write of a kApply step.
const char* clientSettingSource(ClientSettingStep step, bool applied, bool envSet,
                                bool currentEqualsDefault);

} // namespace ucache
