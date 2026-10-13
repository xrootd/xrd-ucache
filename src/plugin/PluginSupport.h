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
// Test-only (UCACHE_TEST_READ_FAIL_KIND=convert, with UCACHE_TEST_READ_FAIL_N): true for the
// process's first N conversions of the slot run, each of which then throws as for want of memory,
// instead of the reads failing. slotFetchFaultFire: readFaultFire for the slot run's fetches,
// false with that kind.
bool convertFaultFire();
bool slotFetchFaultFire();
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
