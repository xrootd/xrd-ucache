// How long the threads that read through the cache wait on it.
//
// Per calling thread: the time during which it has at least one request
// outstanding in the cache -- from the moment one is issued while none is, to
// the moment the last one outstanding is answered. A plain sum of request
// latencies would count a reader that issues several requests and waits for
// all of them (ROOT's vector reads for one fill) several times over. Each such
// interval is charged to the costliest tier any of its requests needed: the
// origin over the slot store over the byte cache.
//
// Who the callers are decides what the sum means. For TTree they are the
// analysis threads, so the sum over threads, divided by threads x wall, is
// their share of time spent waiting on the cache. For RNTuple the reads come
// from ROOT's own I/O threads, which wait by design while the analysis
// computes: there it is request latency, never a share of the analysis.
//
// Thread-safety: every function is thread-safe. A Thread record is used by its
// own thread (begin) and by whichever thread answers its requests (end,
// cancel); its mutex serializes the two.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

namespace ucache {

class ReaderWait {
 public:
  enum Tier : uint8_t { kByte = 0, kReplica = 1, kSlots = 2, kOrigin = 3, kTiers = 4 };

  struct Thread {
    std::mutex mu;
    int outstanding = 0;
    uint64_t startUs = 0;
    uint8_t tier = kByte; // the costliest tier of the interval so far
  };

  // The calling thread's record, made on its first read (`created` true then).
  // A record made in another fork generation is replaced: requests the parent
  // had outstanding are never answered in the child.
  static std::shared_ptr<Thread> current(uint64_t generation, bool& created);

  // A request issued on `t` at `nowUs`.
  static void begin(Thread& t, uint64_t nowUs);
  // A request of `t` answered at `nowUs`, after needing `tier`. When it was the
  // last one outstanding, returns the interval that ends here (us) and sets
  // `intervalTier`; else 0.
  static uint64_t end(Thread& t, uint64_t nowUs, Tier tier, Tier& intervalTier);
  // A request issued and refused at once (no answer will come): forgotten.
  // Returns as end() does, without charging the request's own tier.
  static uint64_t cancel(Thread& t, uint64_t nowUs, Tier& intervalTier);
};

} // namespace ucache
