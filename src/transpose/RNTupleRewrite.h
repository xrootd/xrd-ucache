// Recompress an RNTuple's pages by patch-and-append.
//
// Every page is decoded and re-encoded into a new codec, appended as a fresh
// RBlob key, and its locator repointed. The rebuilt page list and footer are
// appended too, and the anchor is patched where it lies to point at them. The
// original pages become dead space; reclaiming it is a separate concern, and a
// from-scratch container writer would be far more code for no extra confidence.
//
// The result is deliberately expressed as "an extension plus a few in-place
// patch windows" rather than as a finished file, because that is exactly the
// shape the replica tier stores: the extension becomes the replica's data and
// the patches are applied over the original when serving. A standalone
// rewritten file is then just one way of consuming the same result, which is
// what makes it possible to hand the output to ROOT and ask whether it reads.
//
// Thread-safety: pure functions over caller-owned data; no shared state.
#pragma once

#include "FillLayout.h"
#include "RNTupleMeta.h"
#include "Transposer.h" // Source

#include <functional>

#include <cstdint>
#include <string>
#include <vector>

namespace ucache::transpose {

// A window of bytes to overwrite in place. In-place means the record is never
// relocated and its size never changes, so no enclosing key needs adjusting.
struct RNTuplePatch {
  uint64_t offset = 0;
  std::vector<uint8_t> bytes;
};

struct RNTupleRewrite {
  uint64_t extBase = 0;             // where `extension` is appended (old file size)
  std::vector<uint8_t> extension;   // relocated pages + page list + footer
  std::vector<RNTuplePatch> patches; // anchor payload, and fEND
  // Original ranges the overlay replaced, and therefore the only ones safe to
  // punch. A range left un-relocated still serves from the original bytes, and
  // because pages are SHARED a page kept alive by one such range must survive
  // even when another, relocated range also pointed at it.
  std::vector<ReplicaMeta::Range> superseded;
  // Where each relocated piece came from in the ORIGINAL file, at page
  // granularity. Serving never reads this; it is what lets a replica read be
  // named in origin coordinates.
  std::vector<ReplicaMeta::OrigRange> origMap;

  uint64_t pages = 0, storedRaw = 0;
  // Page records whose bytes were already transcoded for an earlier record —
  // RNTuple writes an identical page once and points several records at it.
  uint64_t sharedPages = 0;
  // Work is counted in (cluster, column) RANGES, not pages: the compression
  // setting is recorded per range, so a range can only be relocated whole.
  uint64_t rangesRelocated = 0, rangesUncached = 0, rangesDeclined = 0;
  // A source codec actually observed on a declined range, so the caller can
  // tell the user which codec to add to the policy rather than guessing.
  std::string declinedCodec;
  uint64_t oldPageBytes = 0, newPageBytes = 0;
  uint64_t decodeBytes = 0, decodeNs = 0; // calibration data, as for baskets

  std::string error;
  // As for a basket build: set when the SOURCE could not vouch for bytes it
  // was asked for. Retryable, and says nothing about the file being malformed.
  bool transient = false;
};

// Recompress `m`'s pages to ZSTD at `level`, reading original page bytes
// through `src`. `fileSize` is the source file's size, which is where the
// extension begins.
//
// Works per (cluster, column) RANGE and relocates a range only when every one
// of its pages is available AND its source codec is listed in `codecs` (empty
// = no filter). A range that is not relocated is left pointing at the original
// bytes, so a partially cached entry yields a partial replica instead of no
// replica — the same behaviour the basket path has for hot branches. Doing
// this per page would be wrong: the compression setting is recorded once per
// range and could not describe a range that was only half relocated.
RNTupleRewrite buildRNTupleRewrite(const RNTupleMeta& m, Source& src, uint64_t fileSize, int level,
                                   const std::vector<std::string>& codecs = {});

// The same build from pages converted elsewhere -- the cold replica run
// converts them as the reader asks. A range is relocated when `has` answers
// true for every one of its pages (its codec was already judged when the page
// was converted); `page(ri, pi, enc)` then fills `enc` with page pi of range ri
// as its new block, ZSTD-1 or uncompressed, WITHOUT a checksum (one is added
// here where the page carries one). Output identical in form to the above.
RNTupleRewrite buildRNTupleRewriteFromPages(
    const RNTupleMeta& m, uint64_t fileSize, int level,
    const std::function<bool(size_t ri, size_t pi)>& has,
    const std::function<bool(size_t ri, size_t pi, std::vector<uint8_t>& enc)>& page);

// The layout an RNTuple file is presented in while its replica is created on
// the first read, computed from metadata alone (see FillLayout.h for the TTree
// counterpart and the shape of the result). A page's uncompressed size IS in
// the metadata, so every page of a convertible column range gets a slot of
// that size plus its checksum and is served DECODED: its record's locator
// points at the slot with size = uncompressed size (ROOT copies a page whose
// stored and uncompressed sizes agree), its checksum flag is SET, and the
// range's compression setting becomes 0. The 8 bytes after the page are the
// checksum of the decoded bytes (sealDecodedPage), so a reader verifies every
// page it is served exactly as it would the original's. The rebuilt page list and footer come first in
// the extension, the anchor is patched in place, and the header says the new
// end. `header` = the file's first fBEGIN bytes. Slots lie column by column,
// each column's clusters in order (`relocated` is in that order).
// `slots[i].branch` is the range index and `.basket` the page index; a page
// several records share gets one slot. Declines (error set) rather than guess.
// The checksum a slot serves after a decoded page: XXH3-64 of the page's
// bytes, little-endian, as the format stores a page checksum.
inline constexpr uint32_t kSlotChecksumBytes = 8;
void sealDecodedPage(const uint8_t* page, size_t n, uint8_t out[kSlotChecksumBytes]);

//
// A column range is relocated when its codec (rangeCodec, with `unnamedCodec`
// from unnamedRNTupleCodec) is in `codecs`.
FillLayout layoutForRNTupleFill(const RNTupleMeta& m, uint64_t fileSize,
                                const std::vector<uint8_t>& header,
                                const std::vector<std::string>& codecs,
                                const std::string& unnamedCodec = "");

// A compact map of the first-pass layout `L` (layoutForRNTupleFill of `m`): a
// range of addresses from `base` holding, first, the map's own page list and
// footer (an RBlob key each, in a reservation of their raw size), then every
// page of each column range whose pages ALL have records (`recLen(slot)` > 0)
// back to back, each as its record -- ZSTD-1, or the page uncompressed when that
// did not shrink it -- followed by the record's checksum (XXH3-64,
// little-endian). Those ranges state ZSTD-1 (501); ROOT decides per page from
// its stored and uncompressed sizes. Every other range L relocates stays at its
// slots, decoded, as L states it; the rest as the original. The RNTuple header
// is not rewritten. A page several records share has one slot, and one place.
struct RNTupleCompactMap {
  std::vector<uint8_t> meta;          // the bytes at `base`: the page list's and footer's keys
  uint64_t metaReserve = 0;           // the pages start at base + this
  FillLayout::Window anchor;          // the anchor, pointing at this map's footer
  std::vector<std::pair<uint32_t, uint64_t>> pieces; // (slot, address), ascending addresses
  uint64_t end = 0;                   // past the last page's checksum
  size_t rangesCompact = 0;
  std::string error;
};
RNTupleCompactMap rnTupleCompactMap(const RNTupleMeta& m, const FillLayout& L, uint64_t base,
                                    const std::function<uint32_t(uint32_t slot)>& recLen);

// One page prepared for a cold run from its ORIGINAL on-disk bytes (block, plus
// the 8-byte checksum when it carries one): `raw` = the decoded page, what the
// reader is served; `enc` = the block the replica keeps (ZSTD-1, or `raw` when
// that does not shrink it). A checksum that does not match, or a page that does
// not decode to `uncompressed` bytes, sets `error`: the bytes must not be served.
struct ConvertedPage {
  std::vector<uint8_t> raw, enc;
  std::string error;
};
ConvertedPage convertPage(const uint8_t* onDisk, size_t n, uint32_t nbytes, bool hasChecksum,
                          uint64_t uncompressed);

// Codec name for a page-list compression setting ("lzma"/"zlib"/"zstd"/"lz4"/
// "none"), the same vocabulary the recompress policy is written in. "" for a
// setting that names no codec: an unknown algorithm, or algorithm 0 above
// level 0 -- "the global default", which `hadd -f1` and SetCompression(1)
// record while the pages are compressed with whatever that default was.
std::string rnTupleCodecName(int32_t compressionSettings);

// The codec the pages of a file's UNNAMED column ranges are stored in: ranges
// whose setting names no codec (rnTupleCodecName gives "") while the page list
// shows compressed pages (stored smaller than they decode). Named by the block
// header of ONE such page read through `src` -- the first one `src` has -- so a
// file whose settings name their codecs reads nothing here. `codec` = "" when
// there is no such page, or it names no codec converted here. False only when
// a read failed: nothing is decided then.
bool unnamedRNTupleCodec(const RNTupleMeta& m, Source& src, std::string& codec);

// A range's codec: the one its setting names; for an unnamed range with
// compressed pages, `unnamedCodec`; "none" for one whose pages are stored
// uncompressed.
std::string rangeCodec(const ColumnRange& r, const std::string& unnamedCodec);

// Apply a rewrite to a copy of `srcPath`, producing a standalone file. This is
// the verification path: the arbiter for any change here is whether ROOT reads
// the result and reports the same physics.
bool writeRewrittenRNTuple(const std::string& srcPath, const std::string& dstPath,
                           const RNTupleRewrite& rw, std::string& error);

// Package a rewrite as a replica overlay — the same artifact the basket
// transposer emits, so it publishes and serves through the existing path
// unchanged. The patch windows and the extension become extents over .tdata;
// the pages they replace become superseded ranges, which is what makes the
// original bytes reclaimable.
Overlay rnTupleOverlay(const RNTupleMeta& m, const RNTupleRewrite& rw);

} // namespace ucache::transpose
