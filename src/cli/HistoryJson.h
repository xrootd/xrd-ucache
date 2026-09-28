// The run history as JSON, for `ucache history --json` and for the history
// block `ucache publish` sends.
//
// ONE emitter for both on purpose: the report service reads the keys the local
// command prints, and a second copy would drift. The two forms differ only in
// what may leave the machine -- see historyJson.
//
// Thread-safety: pure functions of their arguments.
#pragma once

#include "RunLog.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ucache {

// What the run label should say. Deliberately not a percentage: "warm" and
// "fill" are the two states a user acts on differently.
const char* runKind(const Run& r);

// `runs` newest first, as loadRuns returns them. `shown` is the newest-N
// window; `oldestFirst` reverses it, which is how the service charts it.
// `redacted` is the published form: no process id, a blank host, the start
// time reduced to a date, and the origins the run read from reduced to their
// registrable domains (`origins`, and the host of each `origins_rt` entry),
// derived from per-file records that never leave. The local form keeps the
// host, the process id, the epoch start and the origin hosts.
std::string historyJson(const std::vector<Run>& runs, size_t shown, bool redacted,
                        bool oldestFirst);

} // namespace ucache
