// XrdCl FilePlugIn gluing ucache-core to the client.
//
// Design invariants:
//  - The inner XrdCl::File is constructed with plugins DISABLED — no
//    recursion (asserted in tests).
//  - Classification (bitmap checks) happens on the caller's thread; disk
//    reads/writes and buffer assembly happen on the Executor; wire misses
//    are issued as ONE VectorRead of page-rounded, order-preserving,
//    coalesced ranges (§5.2 step 3); the user's completion is never delayed
//    by disk persistence (§5.2 step 4).
//  - Exactly one completion per request, statuses propagated faithfully;
//    any cache-side failure degrades to pure pass-through (§5.2 step 5);
//    after UCACHE_MAX_ERRORS consecutive cache errors the handle trips to
//    permanent pass-through (disabled_handles).
//  - No exceptions cross this ABI (§5.3).
//
// Lifetime: executor tasks and wire handlers capture only the shared
// HandleState (never `this`). Tasks that must touch the inner file go
// through HandleState::acquireInner/releaseInner; the plugin destructor
// invalidates and drains. PgRead is pass-through and never cached (§4.7:
// ROOT's path is Read/VectorRead; xrdcp's pgread traffic stays uncached) —
// EXCEPT on a handle shown a slot layout, where pass-through would return
// other bytes: there it is served from the layout with locally computed
// crc32c page checksums.
//
// A handle opened for a copy -- by a copy tool, XRootD's copy engine, ROOT's
// TFile::Cp or gfal2's xrootd plugin -- is pure pass-through: it reads the
// origin's bytes and never enters the cache (CopyDetect.h).
//
// Thread-safety: fully thread-safe; see HandleState.
#pragma once

#include "CacheStore.h"
#include "Config.h"
#include "CopyGuard.h"
#include "Executor.h"
#include "OpenPolicy.h"
#include "XrdClTimeout.h"

#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClPlugInInterface.hh>

#include "ReadFootprint.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace ucache {

class ColdFill; // ColdRun.h
namespace transpose {
struct ColdMap; // StoreMaps.h
}
using ColdMap = transpose::ColdMap;
class ReadRule; // ReadRule.h
struct KeyMap;  // ReadRule.h

class UCacheFile;

// The layout a handle a forked child inherited had been set up in: its next
// setup in the child serves exactly that one (HandleState::syncFork).
enum class ForkLayout : uint8_t { kNone, kOriginal, kCold };

// State shared between the plugin object, executor tasks, and wire handlers.
struct HandleState {
  // fork(): a handle a child inherited may be mid-request in a thread that
  // stayed in the parent -- its locks held, its counts waiting for requests
  // XrdCl drops in the child without completing them, its entry the parent's.
  // Every entry point calls syncFork() first; in a new fork generation the
  // first caller starts the handle over (the others wait for it): the locks
  // are made anew, the counts forgotten, the entry and slot run left behind
  // undestroyed, and the handle is set up again at its next read in the
  // layout it had -- the original, or the same slot layout; if that layout
  // cannot be had again, every read on the handle fails. A handle the cache serves opens
  // the origin file afresh at its first need of it (the parent's is left
  // behind); one that only relays keeps it, and XrdCl recovers it. Nothing is
  // held across fork, and nothing is done in the fork handler: a parent that
  // never forks pays one atomic load.
  void syncFork() {
    const uint64_t g = Executor::forkGeneration();
    if (forkGen.load(std::memory_order_acquire) != g)
      startOverAfterFork(g);
  }
  std::atomic<uint64_t> forkGen{Executor::forkGeneration()};
  std::atomic<uint64_t> forkClaim{Executor::forkGeneration()};
  ForkLayout forkLayout = ForkLayout::kNone;  // under setupMu, then mu
  // The inner file is to be opened afresh: set where a forked child starts the handle
  // over, cleared when the open ends (ensureInnerOpen). Atomic so that a caller that only
  // asks never waits for innerOpenMu, which a move holds for its whole budget.
  std::atomic<bool> innerStale{false};
  bool reopenInChild = false; // set at Open: the cache serves this handle
  // The layout this handle's reader was shown before a fork could not be set
  // up again in the child: reads fail rather than give other bytes.
  std::atomic<bool> layoutLost{false};

  // Set by the handle's first read or Stat. A handle whose FIRST request reads
  // the whole file at once is a copy (UCacheFile::ensureEntry).
  std::atomic<bool> opsSeen{false};
  // The truncated-copy guard of a handle shown another layout (CopyGuard.h).
  CopyGuard copyGuard;
  // Read-ahead (Prefetch.h) keeps its per-handle state on its own thread, keyed
  // by this object; the one bit it shares is whether this handle has ever
  // missed (a handle that never misses is never looked at).
  std::atomic<bool> prefetchSeen{false};
  // CPU-span evidence for the recompression estimator: rusage
  // user+sys µs snapshotted when the handle opens; the delta at Close is the
  // CPU attributable to this file for sequential access patterns. `blended`
  // is set when other ucache handles were open concurrently (attribution
  // uncertain — the estimator treats it as an upper bound).
  uint64_t cpu0Us = 0;
  bool cpuBlended = false;
  std::shared_ptr<CacheStore> store; // process-wide; null => never cache
  std::string url;
  // The timeout the reader gave its Open (0 = the client's default): with
  // adaptive_federation on, the cache's own opens of this handle honour a
  // shorter one (OpenPolicy.h openDeadlineS); with it off it is unused, and
  // those opens pass none (the client's RequestTimeout), as before.
  std::atomic<int> readerOpenTimeoutS{0};
  // Pass-through bytes this handle relayed, and the once-latch for its
  // per-file record. A handle with no entry (UCACHE_DISABLE, write-opened)
  // leaves no lifetime record from FileEntry, so the first of Close/dtor
  // writes one from these — that is what makes a cache-disabled BASELINE run
  // matchable, file by file, against the cached runs that follow it.
  std::atomic<uint64_t> relayedBytes{0};
  std::atomic<bool> relayObsDone{false};
  // Wall span of a handle the cache never served, in µs of a steady clock —
  // the same quantity FileEntry tracks for cached files, so a relayed file and
  // a cached file are directly comparable.
  std::atomic<uint64_t> relayFirstUs{0};
  std::atomic<uint64_t> relayLastUs{0};
  // Answer times of the reads this handle relayed (RelayHandler), for its own
  // per-file record: only this handle's, so a file's records sum.
  Histogram relayRtUs;
  // The data servers of those reads, for the same record.
  ServerTally relayServers;

  std::mutex mu;
  // The server (host:port) the handle's last successful origin open reached,
  // from that open's answer; "" before one. Under mu. A read whose answer
  // names no host is put down to it.
  std::string dataServer;
  std::shared_ptr<FileEntry> entry;            // null until setup completes
  // Slot run (ColdRun.h): the layout this handle was shown at setup,
  // HANDLE-STABLE thereafter: its addresses do not exist at the origin, so a
  // handle keeps the layout it opened with.
  std::shared_ptr<ColdFill> cold;
  // With `cold`: the mixed map this handle was shown at setup (null: the slot
  // layout itself). HANDLE-STABLE like the layout.
  std::shared_ptr<const ColdMap> coldMap;
  bool coldMapChosen = false; // guarded by mu
  // max_read_fraction (ReadRule.h), set with the entry: null when no rule
  // applies. Shared by every handle of the file in the process.
  std::shared_ptr<ReadRule> rule;
  // check_origin_integrity: the file map (keySeeksFor), for the key check of
  // the handle's byte-cache misses. Under mu; null until a miss with the check
  // on finds the file's map built (each miss asks until then); empty for a file
  // that is not checked.
  std::shared_ptr<const KeyMap> keySeeks; // KeyMap: ReadRule.h
  std::unique_ptr<XrdCl::StatInfo> statInfo;   // clone source for Stat(false)
  std::mutex setupMu;                          // serializes lazy entry setup
  bool setupDone = false;                      // entry setup attempted (ok or not)
  bool closed = false;
  int errors = 0; // consecutive cache-side errors (UCACHE_MAX_ERRORS trip)
  bool tripped = false;

  // The inner XrdCl::File lives HERE (owned by HandleState, not the plugin
  // object) so a posted retry task can swap it while capturing only `st_`.
  // `inner` points into `innerOwned` and is repointed by resetInner(). It is
  // atomic because some calls (GetProperty, IsOpen, Stat, Close, the write
  // calls) read it without `mu`; such a call may still be inside the File it
  // loaded when a retry replaces it, so a replaced File is never destroyed
  // while the handle lives: it moves to `retired` (under `mu`), and
  // shutdownInner destroys those once no call can be in them. A File whose
  // open failed (it was never open) goes to `retiredFailed` instead, with the
  // time it was replaced, and a later resetInner destroys it once
  // kRetiredFailedKeepUs have passed: such a File answers every call at once,
  // from its own state, without the network, so no call can still be in it
  // by then. Otherwise a handle whose opens keep failing while its reader
  // keeps asking would keep one File (about 1.3 kB) per lazy-open cool-down
  // (1 s) until it closes. Under mu.
  std::unique_ptr<XrdCl::File> innerOwned;
  std::atomic<XrdCl::File*> inner{nullptr};
  std::vector<std::unique_ptr<XrdCl::File>> retired;
  static constexpr uint64_t kRetiredFailedKeepUs = 600ull * 1000000;
  std::vector<std::pair<std::unique_ptr<XrdCl::File>, uint64_t>> retiredFailed;
  // With adaptive_federation on, the properties the reader set (SetProperty),
  // in order, given again to every File resetInner makes: an open made again
  // at another site would otherwise lose them. Under mu.
  std::vector<std::pair<std::string, std::string>> readerProps;
  // With adaptive_federation on: held across the recording of a reader's property and its setting
  // on the File (UCacheFile::SetProperty), and by the checks made at open while they turn the
  // File's recovery and redirects off and give them back (checkHeader, restoreProps), so that a
  // setting of the reader's never lands on the File between the two. Taken before mu, never under
  // it.
  std::mutex propMu;
  // adaptive_federation: a handle that moves its reads to another site (OpenPolicy.h
  // armedForReads) is armed once it is set up and an open of it went through a
  // redirector (`viaRedirector`, under mu). The client's own recovery of a read is
  // off on every File of an armed handle, and a read that fails in a way another
  // site may not share is issued again at another site (UCacheFile.cc moveOn).
  std::atomic<bool> moveOnReads{false};
  bool viaRedirector = false;
  // Under mu: the server and site the current File was opened at; where reads
  // of the handle failed, the 3 newest (the next open names them in tried= and
  // takes them last), and every one whose read answered with bad bytes, which no
  // open of the handle takes again (OpenPolicy.h noteReadFailure); a move in flight,
  // which holds innerOpenMu, the reads waiting for it, and the reads waiting
  // between attempts, which Close waits for.
  SiteFailure source;
  std::vector<SiteFailure> readFailed;
  // Under mu: the source of each File a move put aside (moveInner), so that a read that fails on
  // one of them later is put down to the copy it was read from.
  std::vector<std::pair<XrdCl::File*, SiteFailure>> leftSources;
  // Under mu: the header read of the reader's open is in flight (UCacheFile.cc checkHeader), with
  // the client's recovery and redirects off on its File; the reader's settings of those made
  // meanwhile are kept in readerProps, from `headerProps` on, and given to the File at its end.
  bool headerReading = false;
  size_t headerProps = 0;
  // Under mu: with adaptive_federation on, the reader's open of a handle the cache serves is in
  // flight (RetryingOpenHandler), from its first attempt to the completion the reader gets. Its
  // File may meanwhile be open at a copy the checks made at open have yet to judge, or have
  // refused: a request made on the handle then is parked until the answer (openWaiters), or, made
  // without a handler, waits for it (openCv), before the handle is set up, so that neither its entry
  // nor its reader is given that copy's size (ensureEntry).
  bool readerOpenPending = false;
  std::condition_variable openCv;
  // Under mu, with adaptive_federation on: the requests made with a handler while the reader's open
  // is in flight, parked here instead of waiting on the thread that made them (it may be a client
  // callback thread that the open's own checks need: UCacheFile::parkForOpen). Taken whole by the
  // first of: the open's answer, Close, the plugin object's end (under mu). Each is then given its
  // turn on a pool thread, exactly once: made again through the plugin object (`redo`, with the
  // handler to answer) once the open succeeded, or its `handler` answered with the open's error,
  // or a closed handle's.
  struct OpenWaiter {
    XrdCl::ResponseHandler* handler = nullptr;
    std::function<XrdCl::XRootDStatus(UCacheFile* owner, XrdCl::ResponseHandler* h)> redo;
  };
  std::vector<OpenWaiter> openWaiters;
  // Under mu: the plugin object while it lives, through which a parked request is made again (the
  // tasks hold this state, never the plugin object), and how many such requests are inside it now;
  // the plugin's destructor clears `owner` and waits (openCv) until ownerUses is 0. A request made
  // again is answered only after it has left the plugin object, so a handler that deletes its file
  // never waits for itself.
  UCacheFile* owner = nullptr;
  int ownerUses = 0;
  // With adaptive_federation on, for a file the cache caches: what the checks made at the reader's
  // open read of the copy the open took, as whole pages of the file (OpenPolicy.h checkReadSpan),
  // kept here from the open's answer until the handle's entry is set up (ensureEntry), which stores
  // them as it stores any bytes fetched from the origin, when the entry is of the size the copy
  // reported and the handle's File is still the one they were read on; then, or at Close, they go.
  // A handle set up before its open answered (a request made on it while the checks ran) never
  // holds them here: they are stored in its entry at the answer (RetryingOpenHandler::
  // keepOpenBytes). Under mu.
  struct OpenBytes {
    XrdCl::File* file = nullptr; // the File the checks read on
    uint64_t size = 0;           // the size the copy reported at its open
    std::string server;          // the copy's server, host:port ("" when not known)
    struct Span {
      uint64_t off = 0;
      std::vector<char> bytes;
      uint64_t t0Us = 0; // the read was sent (steady µs)
      uint64_t rtUs = 0; // its answer time
    };
    std::vector<Span> spans;
  };
  std::unique_ptr<OpenBytes> openBytes;
  // Under the move's locks (innerOpenMu, then mu): the text of the error that names every copy
  // the last move tried, none of which could serve the reads (OpenPolicy.h noCopyServedText); ""
  // once a move succeeded. Given to the reads of a move not made again at once (its cool-down).
  std::string moveFailure;
  bool moveInFlight = false;
  // A read waiting for a move: given whether the handle moved and, when it did not, the text of
  // the error that names every copy the move tried ("" for none), it takes the handle's File and
  // leaves the parked count, and returns what then issues the read again or answers it (moveOn).
  using MoveWaiter = std::function<std::function<void()>(bool, const std::string&)>;
  std::vector<MoveWaiter> moveWaiters;
  int parkedReads = 0;
  bool wholeCopy = false; // under mu: set up as a copy (the first read took the whole file)
  void applyReadPolicy();
  void unpark();
  // The timeout of an origin read of this handle (OpenPolicy.h readAttemptTimeoutS),
  // `startUs` after the request began (`again`: this is a later attempt); the
  // reader's own, as given, when the handle is not armed.
  ucache::XrdTimeout originTimeout(int readerTimeoutS, uint64_t startUs = 0, bool again = false);
  // The last File an origin open was sent on (the open call returned OK). An
  // open issued again on that File (a failed one that could not be replaced
  // while a call was inside it, or a reader opening one File twice) is answered
  // at once from the File's own state (in progress, open, or failed in its
  // answer) and reaches no server, so it is not recorded as refused. An open
  // the client refused at once leaves its File closed and is not stored here:
  // the next open on that File is a new attempt and is recorded.
  std::atomic<XrdCl::File*> openSentOn{nullptr};
  int innerOps = 0;
  bool innerValid = false;
  // Under mu: the handle shut down while the cache's own open of the origin was
  // in flight (shutdownInner does not wait for it). An open of that sequence that
  // succeeds afterwards is closed, not adopted.
  bool seqAbandoned = false;
  std::condition_variable innerCv;

  // Cache-freshness (UCACHE_REVALIDATE_S): when a recent local entry is
  // trusted, Open skips the remote open+stat and sets cacheOnly — the inner
  // file is then opened LAZILY only if a genuine miss needs the origin
  // (fail-open). Opt-in; when the knob is 0 these stay false and the path is
  // unchanged. innerOpenMu serializes the one-shot lazy open off the fast path.
  // Written under mu; atomic, because requests read it without (ownOpenPending, acquireInner).
  std::atomic<bool> cacheOnly{false};
  // Set at Open when the handle is trusted (cacheOnly): its File is opened only by the cache's own
  // open (ensureInnerOpen), never by the reader's. With adaptive_federation on, no request goes to
  // that File until that open has succeeded. With it off, the same holds once the handle has lost
  // its trust (its entry gone by its first read): a request on a File that never opened fails in
  // the client, or crashes it. A handle that still trusts its entry gives an Fcntl, a Visa or a
  // write to that File as it is, as 1.4.0 did (the client answers "invalid operation" and sends
  // nothing); its Stat is answered from the entry, and a read that misses waits for the open.
  std::atomic<bool> trustedAtOpen{false};
  // Read without innerOpenMu on the fast path (acquireInner): set under it, and
  // atomic so a reader that sees it set also sees the open that preceded it.
  std::atomic<bool> innerOpened{false};
  bool innerOpenOk = false;
  std::mutex innerOpenMu;
  // Failed lazy opens do NOT latch (a user's cmsRun died to one transient
  // "no route to host" reselection): the failure and its time are kept so
  // the next miss after a cool-down re-attempts, and reads surface the
  // ORIGIN's error, never a generic errInvalidOp. Guarded by innerOpenMu.
  XrdCl::XRootDStatus lazyOpenError;
  uint64_t lazyOpenFailUs = 0;
  int lazyOpenAttempt = 0; // cumulative across cool-down re-attempts (fault hook numbering)
  // The file size a trusted handle showed its reader (from the sidecar), 0 when
  // none: the lazy open refuses a source of another size, because the reader
  // holds positions in this one. A refusal latches for the handle and drops
  // the entry's trust, so the next open checks the origin again.
  std::atomic<uint64_t> expectedOriginSize{0};
  // With adaptive_federation on, the size the reader's open was answered with, by the copy that
  // open took (0: none, or not known): the size its reader was shown, which a move of its reads
  // must find again (sizeShownLocked).
  std::atomic<uint64_t> readerOpenSize{0};
  // Under mu: the size the handle's reader was shown, which every copy its reads move to must
  // have: the trusted entry's, else the entry's, else the stat's, else the reader's open's; 0
  // when none is known (the handle is then not armed: applyReadPolicy).
  uint64_t sizeShownLocked() const;
  // The origin size that passed the header check of a ROOT file (0: none), the copy's own header
  // saying where the file ends at the size it reported (OpenPolicy.h rootFileEnd): set by the
  // reader's open of a file named *.root with adaptive_federation on, and by a handle whose
  // entry carries the size confirmed (MetaData::kFlagSizeConfirmed).
  std::atomic<uint64_t> confirmedSize{0};
  std::string trustMarker; // under mu: the entry's .val path, removed on such a refusal
  std::atomic<bool> sizeRefused{false}; // set: no origin read of this handle reaches that source
  bool ensureInnerOpen(); // lazy origin open on first miss; the caller waits for it
  // The status a cache miss should fail with when the inner file is
  // unavailable: the recorded lazy-open failure if there is one, else
  // errInvalidOp (genuinely invalid handle states).
  XrdCl::XRootDStatus missError();

  XrdCl::File* acquireInner();
  // Like acquireInner, but never OPENS the origin: a trusted cache-only handle
  // whose inner file is not open yet returns null instead of running the
  // synchronous lazy open. For work the application did not ask for -- read
  // ahead -- where blocking a shared thread on a remote open, consuming the
  // handle's one open attempt, and latching an open failure that the reader's
  // next real miss would then inherit are all the wrong trade.
  XrdCl::File* acquireInnerIfOpen();
  void releaseInner();
  void shutdownInner(bool leak = false); // leak: the file is not destroyed
  // Wait, up to `max`, for the cache's own requests on the inner file to
  // finish. Close must do this: XrdCl refuses to close a file with requests
  // in flight, and read-ahead's are not the application's to lose a close
  // over. Bounded, because a hung origin request must not hang a close. False
  // when requests or parked reads are left.
  bool waitInnerIdle(std::chrono::milliseconds max);
  // Wait, up to `max`, for the reads waiting for a move to be issued again or answered. False when
  // some are left.
  bool waitParked(std::chrono::milliseconds max);
  // Install a fresh inner file in place of a terminally-failed (or refused)
  // one, which moves to `retired`; returns the fresh one. Returns nullptr,
  // changing nothing, once the handle is shut down, or with `onlyIfIdle` while
  // the cache's own requests are on the inner file (after a failed or refused
  // open; between the attempts of an open none can be, as no request of the
  // cache's goes to a File whose open has not succeeded). `leakOld`: the old
  // file is left alone instead (a forked child's inherited one, the parent's).
  // `closeOld`: the old one is open (an open refused its copy) and is closed
  // without waiting, as it moves to `retired`.
  XrdCl::File* resetInner(bool leakOld = false, bool onlyIfIdle = false, bool closeOld = false);
  // A File of this handle's: plugins off, the reader's properties given again in
  // order and, on an armed handle, the client's read recovery off. Under mu.
  std::unique_ptr<XrdCl::File> makeInner();
  // A forked child has yet to open this handle's origin file afresh, or is doing so.
  // With adaptive_federation on it never waits: a move holds innerOpenMu for its whole
  // budget, and a caller that needs the open calls ensureInnerOpen, which waits for it.
  bool reopenPending();

  // Async page-persist accounting. Persists are posted off the read path
  // (§5.2 step 4), but Close/destruction must wait for them so short-lived
  // processes still leave a fully populated cache. begin/end bracket each
  // posted task; drainPersists blocks (Close/dtor only, never a read).
  std::mutex persistMu;
  std::condition_variable persistCv;
  int pendingPersists = 0;
  void beginPersist();
  void endPersist();
  void drainPersists();

  void noteCacheError(const Config& cfg);
  void noteCacheOk();

 private:
  void startOverAfterFork(uint64_t gen);
};

class UCacheFile : public XrdCl::FilePlugIn {
 public:
  UCacheFile();
  ~UCacheFile() override;

  XrdCl::XRootDStatus Open(const std::string& url, XrdCl::OpenFlags::Flags flags,
                           XrdCl::Access::Mode mode, XrdCl::ResponseHandler* handler,
                           ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Close(XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Stat(bool force, XrdCl::ResponseHandler* handler,
                           ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Read(uint64_t offset, uint32_t size, void* buffer,
                           XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus PgRead(uint64_t offset, uint32_t size, void* buffer,
                             XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus VectorRead(const XrdCl::ChunkList& chunks, void* buffer,
                                 XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Write(uint64_t offset, uint32_t size, const void* buffer,
                            XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Write(uint64_t offset, XrdCl::Buffer&& buffer,
                            XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus VectorWrite(const XrdCl::ChunkList& chunks,
                                  XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus WriteV(uint64_t offset, const struct iovec* iov, int iovcnt,
                             XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Sync(XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Truncate(uint64_t size, XrdCl::ResponseHandler* handler,
                               ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Fcntl(const XrdCl::Buffer& arg, XrdCl::ResponseHandler* handler,
                            ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Visa(XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  bool IsOpen() const override;
  bool SetProperty(const std::string& name, const std::string& value) override;
  bool GetProperty(const std::string& name, std::string& value) const override;

 private:
  // The reads themselves; the public calls wrap the reader's handler to
  // measure how long the reading thread waits (ReaderWait.h).
  XrdCl::XRootDStatus readServe(uint64_t offset, uint32_t size, void* buffer,
                                XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout);
  XrdCl::XRootDStatus pgReadServe(uint64_t offset, uint32_t size, void* buffer,
                                  XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout);
  XrdCl::XRootDStatus vectorReadServe(const XrdCl::ChunkList& chunks, void* buffer,
                                      XrdCl::ResponseHandler* handler,
                                      ucache::XrdTimeout timeout);
  void invalidateOnWrite();
  // Sets up a handle trusted at Open before a request that would make the cache's own open of the
  // origin (adaptive_federation on only): that open must find the entry's size.
  void setUpBeforeOwnOpen();
  // With adaptive_federation on, a request made with a handler while the reader's open is in flight
  // is parked (HandleState::openWaiters) and true is returned: `redo` then makes it again, or
  // `handler` is answered, exactly once, after the open's answer. False (nothing parked) otherwise.
  bool parkForOpen(XrdCl::ResponseHandler* handler,
                   std::function<XrdCl::XRootDStatus(UCacheFile*, XrdCl::ResponseHandler*)> redo);
  // Lazily create the cache entry on the caller's thread (first read), doing
  // a synchronous inner Stat. Returns the entry, or nullptr for
  // pass-through (write-open, disabled, denied host, Stat failure, closed).
  // Also sets up the file's slot layout when it has a store.
  // `firstRead` is the handle's first request when this is its first operation:
  // a whole-file read there makes the handle a copy (no entry, the origin's
  // bytes) where a slot store exists, and shows it the file as stored where
  // recompression could make one.
  std::shared_ptr<FileEntry> ensureEntry(const std::pair<uint64_t, uint64_t>* firstRead = nullptr);
  std::shared_ptr<ColdFill> currentCold() const;
  // The size a slot-layout handle shows its reader; 0 for a plain one.
  uint64_t shownSize() const;

  std::shared_ptr<HandleState> st_;
  bool passthroughOnly_ = false; // write-open / UCACHE_DISABLE / no store / a copy
  bool cacheOpen_ = false;       // trusted-cache Open succeeded without the remote
};

// Records of the origin's answers (docs/FORMAT.md). Both take only what a
// completion handler already holds and queue their line (CacheStore::
// enqueueRecord), so they may run on a client callback thread.
//
// The server an answer came from, as host:port: the last host of its HostList
// (right even when the client recovered the connection elsewhere), else the
// server the handle's origin open reached; "" when neither is known (or the
// name could not be built).
std::string answeredBy(HandleState& st, const XrdCl::HostList* hosts) noexcept;
// The server an origin read is sent to, as host:port, for its in-flight count
// (OriginInFlight), by one rule at every issue: the server the handle's origin
// open reached when its answer named one, and otherwise `f`'s current data
// server as the client reports it (`f`: the file the read goes out on; a
// pass-through handle, whose open is not watched, and an open answered with no
// host list both take this). "" when neither is known: the read is then not
// counted for any server. The client chooses the connection as it sends, so
// this is the server the read goes to unless the client redirects or recovers
// it.
std::string issueServer(HandleState& st, XrdCl::File* f) noexcept;
// A trace record's members for one origin request: its server ("ds",
// host:port), when it was issued ("t0", steady clock µs) and the requests in
// flight to the server it was sent to then, itself included ("nc"). "" if
// they cannot be built: the record is then written without them.
std::string wireTraceFields(const std::string& ds, uint64_t t0, uint64_t nc) noexcept;
// One failed origin request: counted by its provisional class, put on
// `tally` (the file's account of its servers, when there is one), and recorded
// as an `origin_failure` line, at most 16 per file, server and class in a
// process. `op` names the request: read, readv, first_pass, records or relay.
// `codec`, when not empty, is the compression of a basket that failed its
// checksum.
void noteOriginFailure(HandleState& st, ServerTally* tally, const char* op, uint64_t off,
                       uint64_t len, const XrdCl::XRootDStatus& s, const std::string& server,
                       const std::string& codec = std::string());
// A check found the copy at `server` bad while a handle read the file (a basket that failed the
// key-position check or its own checksum, a short answer inside the confirmed size, a page read
// answered with no bytes inside it). When `entry` (the handle's) holds bytes this process fetched
// from that copy (ServerTally::tookFrom on its account of its servers), the entry is dropped: no
// handle of this process is served from it again, their reads go to the origin (and move as any
// read does), and its files are removed off this thread (CacheStore::removeDropped), so that the
// next open of the file starts a new entry. Only with check_origin_integrity on, which
// adaptive_federation forces. No I/O: it may run on a client callback thread.
void dropEntryOfBadCopy(HandleState& st, const std::shared_ptr<FileEntry>& entry,
                        const std::string& server) noexcept;
// The same, with the handle's own entry.
void dropEntryOfBadCopy(HandleState& st, const std::string& server) noexcept;

// Process-wide config/store accessors (UCacheFactory.cc).
const Config& globalConfig();
std::shared_ptr<CacheStore> globalStore();
// XrdCl's RequestTimeout as the plugin found it at load: what an origin
// request given no timeout of its own is allowed.
int defaultRequestTimeoutS();
// The client's request timeout as it holds it now (its Env, read at each call; the value a request
// given timeout 0 gets): what a failure path that sends a request again counts the time left by.
int currentRequestTimeoutS();
// True once the process has begun to exit (the plugin's exit handler has
// started): an open trying other sites issues nothing more and ends with its
// last error.
bool processExiting();
// A forked child's first cache work: re-arms what lived only in the parent's
// timer queue (the periodic checkpoint). Cheap when there is nothing to do.
void resumeAfterFork();
// Builds the table of asks to the regional redirectors and has a forked child start
// it anew (UCacheFile.cc); called once, where the plugin starts, before any thread.
void locateInit();

} // namespace ucache
