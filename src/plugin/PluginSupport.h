// Small helpers every serving route shares: the steady clock the stats use, and
// the completion calls that honor XrdCl's native contract (a non-null, empty
// HostList on every synthesized completion — see complete() in UCacheFile.cc).
//
// Thread-safety: stateless.
#pragma once

#include <XrdCl/XrdClXRootDResponses.hh>

#include <cstdint>

namespace ucache {

uint64_t nowUs();
void complete(XrdCl::ResponseHandler* h, XrdCl::XRootDStatus* status, XrdCl::AnyObject* response);
XrdCl::XRootDStatus* okStatus();
XrdCl::AnyObject* chunkResponse(uint64_t off, uint32_t len, void* buf);
XrdCl::AnyObject* vreadResponse(const XrdCl::ChunkList& chunks);

// Test-only wire-read fault (UCACHE_TEST_READ_FAIL_N, inert unless set): true
// for the process's first N origin reads the cache issues -- byte-cache misses
// and first-pass fetches alike -- which then complete with errConnectionError
// instead of being sent. One relaxed load when unset.
bool readFaultFire();

// A handler the XRootD client calls marks its thread while it runs (a ClientCallback for the call's
// length), so that code a reader runs from inside its own completion (a read issued from its
// handler) can tell that it is on a client callback thread, where nothing may wait for a move:
// the client's few worker threads deliver every answer, the move's own among them.
struct ClientCallback {
  ClientCallback();
  ~ClientCallback();
  ClientCallback(const ClientCallback&) = delete;
  ClientCallback& operator=(const ClientCallback&) = delete;
};
bool onClientCallback();

// Whether a vector read's answer holds every byte asked, chunk by chunk (`asked`: the elements
// sent, in order, each with its length in `len`, or a ChunkList). A server's answer always does
// (the client refuses one that does not); the client's own access to local files (file://, a
// root://local file) answers a chunk past the file's end with fewer bytes, or none, and still OK --
// a file truncated or rewritten while it is read. What it did not return must be neither served nor
// kept: the wire buffers' rest is zeros.
inline constexpr const char* kShortVectorRead =
    "uCache: a vector read was answered with fewer bytes than asked (the file changed while it "
    "was read)";
inline uint64_t askedLength(const XrdCl::ChunkInfo& c) {
  return c.length;
}
template <typename Elem> auto askedLength(const Elem& e) -> decltype(uint64_t(e.len)) {
  return e.len;
}
template <typename Elems> bool vreadComplete(XrdCl::AnyObject* response, const Elems& asked) {
  XrdCl::VectorReadInfo* info = nullptr;
  if (response)
    response->Get(info);
  if (!info)
    return false;
  uint64_t want = 0;
  for (const auto& e : asked)
    want += askedLength(e);
  if (info->GetSize() != want)
    return false;
  const XrdCl::ChunkList& got = info->GetChunks();
  if (got.size() == asked.size()) {
    size_t i = 0;
    for (const auto& e : asked)
      if (got[i++].length != askedLength(e))
        return false;
  }
  return true;
}

// Test-only (UCACHE_TEST_READ_FAIL_KIND=convert, with UCACHE_TEST_READ_FAIL_N): true for the
// process's first N conversions of the slot run, each of which then throws as for want of memory,
// instead of the reads failing. slotFetchFaultFire: readFaultFire for the slot run's fetches,
// false with that kind.
bool convertFaultFire();
// Test hook (UCACHE_TEST_READ_FAIL_KIND=decodenomem): the slot run's conversion about to start is
// made with its decoders' allocations failing (transpose::DecoderNoMemory). UCacheFile.cc.
bool decodeNoMemFire();
// The same for the decode of a stored RNTuple page a request copies (UCACHE_TEST_READ_FAIL_KIND=
// pagenomem).
bool pageNoMemFire();
bool slotFetchFaultFire();
// Test hook (UCACHE_TEST_READ_FAIL_KIND=extra, with UCACHE_TEST_READ_FAIL_N): true for the
// process's first N fetches of what complete_chunks takes along on their own, each of which then
// fails with a connection error. UCacheFile.cc.
bool extraFetchFaultFire();
// Test-only (UCACHE_TEST_READ_FAIL_KIND=splitmove, with UCACHE_TEST_READ_FAIL_N=1): the slot run
// cuts its fetches into parts of at most 8 baskets; between the first and the second part of the
// first request that has more than one, the handle is moved to another copy, as another request's
// failure would move it (splitMoveFire, once), and the next part issued then fails with a
// connection error (splitMoveFailNext), on the File it was issued on.
bool splitMoveKind();
bool splitMoveFire();
bool splitMoveFailNext();

// The tier a request needed (ReaderWait::Tier), noted on the reader's handler
// when the read's entry point wrapped it to measure the reader's wait; any
// other handler is left alone. The costliest tier noted wins.
void noteWaitTier(XrdCl::ResponseHandler* h, uint8_t tier);

} // namespace ucache
