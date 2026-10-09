# FORMAT.md — on-disk cache format (entry sidecars v1, slot stores v2)

Bump `MetaData::kFormatVersion` on ANY layout change; readers reject other
versions (entry treated as absent and rebuilt).

```
$UCACHE_DIR/                         (0700)
  objects/<hh>/<sha256(key)>.data    # sparse data file, logical size = origin size
  objects/<hh>/<sha256(key)>.meta    # sidecar, layout below
  objects/<hh>/<sha256(key)>.meta.tmp  # transient during atomic rewrite
  objects/<hh>/<sha256(key)>.cost    # CPU-span evidence sidecar (recompression;
                                     #  tmp+rename at close, best-effort)
  objects/<hh>/<sha256(key)>.slots   # slot store: a replica built as the file is read
  objects/<hh>/<sha256(key)>.slots.tmp.<pid>.<n>  # transient during its creation
  stats/<host>-<pid>-<start_ts>-<seq>.jsonl  # per-process stats dumps (docs/STATS.md)
  stats/<stem>.files.jsonl           # per-file lifetime records (docs/STATS.md), and
                                     #  origin open and failure lines (below)
  stats/<stem>.trace.jsonl           # sampled IO trace, `trace = io` (docs/STATS.md)
  LOCK                               # cache-wide flock for eviction
  objects.cleared.<pid>-<ms>/        # an objects/ tree `ucache clear` set aside,
                                     #  deleted in the background
  cleared.lock                       # flock held while such a tree is deleted
```

`<hh>` = first two hex chars of the sha256. The key is the normalized URL;
CGI/opaque parameters are stripped before hashing so tokens never
reach disk, in keys or file names.

## .meta layout (all integers little-endian; no implicit struct packing)

| offset | size | field | notes |
|---|---|---|---|
| 0 | 4 | magic | `"UCAC"` |
| 4 | 4 | format_version u32 | = 1; mismatch → treat as absent |
| 8 | 4 | page_size u32 | power of two, 4 KiB–1 MiB; fixed at entry creation |
| 12 | 4 | flags u32 | bit0 = pinned, bit1 = complete, bit2 = size confirmed (below); other bits are kept as read |
| 16 | 8 | file_size u64 | origin size validator (always checked) |
| 24 | 8 | atime u64 | unix seconds, coarse (≤1 update/min) — eviction LRU key |
| 32 | 8 | origin_mtime u64 | 0 unless `UCACHE_VALIDATE=size+mtime` |
| 40 | 1 | cksum_kind u8 | 0 none, 1 adler32, 2 crc32c (`UCACHE_VALIDATE=cksum`) |
| 41 | 3 | reserved | zero |
| 44 | 4 | origin_cksum u32 | valid when cksum_kind ≠ 0 |
| 48 | 4 | key_len u32 | ≤ 64 KiB |
| 52 | 4 | meta_crc u32 | crc32c of the whole image with this field zeroed |
| 56 | key_len | key | UTF-8, not NUL-terminated |
| — | pad | | zero pad to 8-byte alignment |
| — | ⌈npages/8⌉ | bitmap | LSB-first within each byte; bit i = page i present |
| — | npages×4 | crc32c[u32] | per-page CRC of the page's real bytes; 0 when absent |

`npages = ceil(file_size / page_size)`. The tail page covers only
`file_size − (npages−1)·page_size` bytes; its CRC covers exactly those bytes.

Flag bit2, *size confirmed*: with `adaptive_federation = on`, an open of a file
named `*.root` reads the first 64 bytes of the copy it landed on. A ROOT file's header
states where the file ends; when that equals the size the copy reported, the size is
confirmed and the entry carries the bit, and a copy of another size is not accepted
for the entry or for a handle that holds it. The bit is only ever set, and goes with
the entry when the origin size changes (a new entry starts unconfirmed). It is not a
format change: a build that does not know the bit ignores it and keeps it when it
rewrites the sidecar (`format_version` stays 1, since any other version makes a reader
treat the entry as absent), and a sidecar written before the bit reads as not confirmed.

## Integrity rules

- A page is served only if its bitmap bit is set **and** its stored CRC
  matches the bytes read. Any mismatch → page treated as absent (bit
  cleared, CRC zeroed), refetched from origin, `crc_failures` counted.
- A set bit whose stored CRC is 0 is read as absent when the entry is opened,
  and the next sidecar commit clears it.
- Write ordering: page data is written (and optionally fdatasync'd per
  `UCACHE_FSYNC`) **before** its bit is set. The bitmap+CRCs are flushed on
  close and every `UCACHE_META_FLUSH_S` (default 30 s) — by time, not only
  when a write arrives, so a process that stops writing and exits without
  running destructors leaves at most one interval of fill behind.
- Sidecar rewrites are atomic: serialize → `<path>.meta.tmp` → rename, under
  `flock(LOCK_EX)` on the entry's `.data` fd (stable inode; the sidecar's
  inode changes on every rewrite). Readers take `LOCK_SH` on `.data` for
  full-meta loads at open.
- A rewrite is a **commit, not a replacement.** Several processes may hold
  one entry open and fill different ranges of it (one job per chunk, or
  several jobs on one node). Under the lock the writer re-reads the sidecar
  and applies only the pages **it** set or cleared since its last store, plus
  a `pinned` flag it changed; every other bit and CRC is taken from the image
  on disk. So a sibling's pages survive, and a page one process cleared
  before punching is not put back by another process's stale view. The writer
  then adopts the committed image, so it serves its siblings' pages as well.
- A fresh entry's empty sidecar is stored at open, under the same lock, so a
  second opener adopts it instead of truncating `.data` under a fill that has
  not published its pages yet.
- The whole-image `meta_crc` makes torn sidecar writes detectable: a corrupt
  or truncated sidecar simply fails to load and the entry restarts empty
  (`meta_corrupt` counted). A crash can therefore only lose cached pages —
  never serve wrong bytes.
- Page writes are idempotent (same origin bytes at the same offsets), so
  concurrent populators — including other processes — never conflict on
  `.data`; their sidecar changes are merged by the commit rule above.
- An entry unlinked while open (eviction) keeps serving through its open fd;
  the owner detects `st_nlink == 0` before sidecar flushes and skips them so
  the entry is not resurrected.

## Replicas of earlier releases: `<hash>.tdata`, `.tmeta`, `.tok`

Earlier releases kept a compact replica beside the byte cache: `.tdata` (its
bytes), `.tmeta` (its map) and `.tok` (a verify-once marker). This release
neither makes nor serves one: the file's slot store (below) takes its place.
Each is removed at the file's next open, by `ucache recompress`, and by the
eviction pass (whether or not its entry still exists); until then
`ucache status` and `ucache doctor` say how many are left.

## Slot store: `<hash>.slots`, format_version 2

The replica of a file: recompressed as it is read (`recompress = on`), and by
`ucache recompress` from what the byte cache holds. The file
is served to readers in its SLOT layout: every convertible basket (TTree) or
page (RNTuple) of the original sits in a slot past the original end, sized so
its converted record fits, followed by zeros. An RNTuple slot is the page
DECODED, followed by 8 bytes: the XXH3-64 of the decoded page, little-endian,
the page's checksum in the format's own convention (the page list flags every
such page as checksummed), so a reader verifies each page it is served.
Slots lie branch by branch (TTree: each branch's baskets in order) or column
by column (RNTuple: each column's clusters in order), so the gaps a reader's
request may span between the pages it wants are pages of the same branches or
columns, not of ones it never reads. That layout is fixed when the
store is created and never changes, so a reader may reopen the file at any
time and find its offsets unchanged. All integers little-endian.

```
[0, 4096)            header (written once)
[4096, 4096+blob)    layout blob (written once): the layout, as its creator computed it
then, each at a 4096-aligned offset: commit blocks
```

Header:

| offset | size | field | notes |
|---|---|---|---|
| 0 | 8 | magic | `"UCSLOTS1"` |
| 8 | 4 | format_version u32 | = 2. Below 2: not used, and replaced when a new store is made. Above 2 (a newer uCache's store): left in place — never replaced, unlinked or served; the file is served as stored |
| 12 | 4 | layout_version u32 | the layout algorithm: 4 for a TTree file, 5 for an RNTuple file (5 lays an RNTuple file's slots out column by column; 4 laid them out cluster by cluster). Lower: store replaced — except that a version 3 or 4 store is served as it is unless it is a version 3 DECLINED one (version 4 changed only which files and branches are converted, never how a layout is laid out); higher: left in place, as a newer format is |
| 16 | 1 | container u8 | 0 = TTree, 1 = RNTuple |
| 18 | 1 | declined u8 | 1 = the file is not served this way (nothing else follows) |
| 20 | 4 | n_slots u32 | |
| 24 | 8 | origin_size u64 | validators, compared like a replica's (`validate`) |
| 32 | 8 | origin_mtime u64 | |
| 40 | 1 | cksum_kind u8 | |
| 42 | 2 | slot_factor u16 | in hundredths: a TTree slot is ⌊stored basket length × slot_factor / 100⌋ bytes. 300 (3×), fixed; a store an earlier build made with another factor is served with the one it records |
| 44 | 4 | origin_cksum u32 | |
| 48 | 8 | virtual_size u64 | the file size readers are shown |
| 56 | 8 | layout_hash u64 | XXH3-64 of the layout |
| 64 | 8 | store_id u64 | random, chosen at creation |
| 72 | 8 | blob_len u64 | |
| 80 | 4 | blob_crc u32 | CRC32C of the blob |
| 84 | 2 | codecs_len u16 | then the codecs list the store converts (ASCII, ≤ 256) |
| 4092 | 4 | header_crc u32 | CRC32C of bytes [0, 4092) |

The layout blob is `raw_len u64` followed by the raw layout compressed as
ROOT-style ZSTD chunks — each a 9-byte header (`'Z' 'S' 1`, compressed size
u24, uncompressed size u24, little-endian) then one zstd frame, the raw bytes
cut into chunks under 16 MiB — or followed by the raw bytes themselves, which
is recognised by `raw_len` equalling what follows. (A plain zstd decoder does
not read the chunked form.) All integers are little-endian. The raw layout:

| field | encoding |
|---|---|
| magic | `"UCLAYT02"` |
| container | u8 (0 TTree, 1 RNTuple) |
| origin size, virtual size, metadata seek, slots begin | u64 each |
| windows | u32 count, then per window: offset u64, bytes (u64 length + bytes) |
| relocated metadata record | u64 length + bytes |
| read-footprint ranges | u32 count, then per range: offset u64, length u64 |
| relocated branches or column ranges | u32 count, then u32 each |
| slot table | u32 count, then per slot the six varints below |
| RNTuple pages | per slot: stored size u32, checksum flag u8 |

Per slot, in this order, against the slot before (the first against zeros
and against `slots begin`): the branch index delta and the basket index delta
minus one, as zigzag varints; the original seek delta, a zigzag varint; the
original length, a plain varint; the slot length, a varint that is 0 for the
plain factor × original length (TTree only) and length + 1 otherwise; and the
slot's virtual seek minus the previous slot's END, a zigzag varint (0 when
slots follow one another). The layout is decoded and checked, never trusted:
the slots must lie inside the virtual size, in order and without overlap.

Commit block, at a 4096-aligned offset:

| offset | size | field |
|---|---|---|
| 0 | 8 | magic `"USBLOCK1"` |
| 8 | 8 | store_id u64 (must match the header) |
| 16 | 8 | block_len u64 (header + entries + records) |
| 24 | 4 | n_entries u32 |
| 28 | 4 | entries_crc u32 (CRC32C of the entries) |
| 60 | 4 | block_header_crc u32 (CRC32C of bytes [0, 60)) |
| 64 | 24 × n | entries: slot u32, kind u8 (1 ZSTD, 2 raw, 3 kept as stored, 4 map), 3 pad, record offset u64, record length u32, record crc u32 |
| … | | the records |

A record's CRC32C is seeded with (store_id u64, slot u32, length u32), so it
validates only as the record it was written as, in the store it was written
to. A kept-as-stored entry has no record: the slot is served from the byte
cache's copy of the original basket, with its `fSeekKey` pointing at the slot.
The last valid entry for a slot wins.

**Maps (TTree).** A kind-4 entry is a *mixed map*, with the slot field
`0x80000000` (an entry whose kind and slot field disagree is not read). Builds
that do not know the kind skip the entry and serve the file in the slot layout
alone, which every map shares. A map is the slot layout with the slots
committed when it was made stated at their record's length (a kept original
at its own length; a slot's length is its first record's) instead of the
slot's, and the tree's and those branches' `fZipBytes` changed by the
difference. A handle is shown, at its open, the newest map (the one whose
entry lies furthest into the store) and keeps it for its life; every byte of
the file but its tree record and its keys list is the same in every map. The
record:

| offset | size | field |
|---|---|---|
| 0 | 8 | magic `"UCSMAP01"` |
| 8 | 4 | tree record length u32 |
| 12 | 4 | keys-list window length u32 |
| 16 | 8 | made u64 (seconds since the epoch) |
| 24 | 8 | where the tree record is served u64 |
| 32 | 8 | where the keys-list window is served u64 (the original keys list's offset) |
| 40 | 8 | covered u64: the bytes of the slots it states at their record's length |
| 48 | 16 | zero |
| 64 | | the tree record as served: the slot layout's tree key header, its `fNbytes` and `fSeekKey` set, then the tree record as ROOT-style ZSTD-1 chunks (never stored uncompressed: ROOT tells the two forms apart by the length the keys list states) |
| … | | the keys-list window: the original keys list, its live tree entry pointing at this map's tree record |

**Compact maps (magic `"UCSMAP02"`).** A map made by this release states the
baskets converted when it was made back to back, at their record's length, in
a range of its own, `[rangeLo, rangeLo + rangeLen)`. The range lies past the
slot layout's end and past every range the file's in-use record has seen,
behind a guard of 1 GiB that nothing is served at but zeros. A basket kept as
stored is stated at its original place; one not converted when the map was
made stays at its slot. Its header window states its end (`fEND`), the size a
handle shown it is given: in the slot layout's header window, or -- when the
file's header is 32-bit and the map ends past 2 GiB -- as the whole header
rewritten in the 64-bit layout at offset 0, as ROOT does when a file passes
2 GB. A read in the range is translated to the record the map names and served
with that basket's `fSeekKey` set to the address read. A read in the range of a
map no longer live is refused, logged and counted (`slot_map_refused`). Builds
that know only `"UCSMAP01"` skip the entry. At most four are live at once; then
new opens are shown the slot layout.

An RNTuple file's compact map starts its range with its own page list and
footer, an RBlob key each, in a reservation of their raw size; its records
start `firstOff` into the range. Each column range whose pages all have records
is stated there page by page as its record -- ZSTD-1, or the page uncompressed
where that is no smaller -- followed by the record's 8-byte checksum (XXH3-64,
little-endian), with compression setting 501; ROOT decodes them. A page several
column ranges share (same-page merging) is stated once. Every other range stays
at its slots, decoded. The anchor window, where a TTree map has its keys-list
window, points at the map's footer; the RNTuple header is not rewritten.

Its record is the same as above to offset 48, then:

| offset | size | field |
|---|---|---|
| 48 | 8 | rangeLo u64 |
| 56 | 8 | rangeLen u64 |
| 64 | 8 | where the header window is served u64 |
| 72 | 4 | header window length u32 |
| 76 | 4 | records u32 |
| 80 | 8 | where its records start, from rangeLo u64 (RNTuple: past its page list and footer; TTree: 0) |
| 88 | 1 | flags u8: 1 = each record is followed by its 8-byte checksum (RNTuple) |
| 89 | 7 | zero |
| 96 | | the tree record (RNTuple: the page list and footer served at rangeLo), then the keys-list window (RNTuple: the anchor window), as above |
| … | | the header window |
| … | 24 each | the records its range names, in address order: slot u32, length u32, CRC u32, kind u8, 3 zero bytes, store offset u64 |

A map's tree record lies between the slot layout's relocated tree record and
the first slot — the room the slot layout keeps there, the length of the tree
record uncompressed — in a place no live map holds. A map is live while it is
the newest, while a handle in any process holds it, and for `in_use_seconds`
(default 1 day) after it was last handed out or released (the file's in-use
record, below); then its place may be taken. A map nobody was handed frees at
once. A map is never rewritten. A store made after the file's store was
removed lays out the same place: there, a map of the removed store keeps its
place while the in-use record says it is in use, and a read in it is refused
(`slot_map_refused`).

- **Releases sharing a cache:** every format keeps the magic at offset 0 and
  format_version at offset 8, so any build can tell a newer store from debris.
  A newer store still counts as the file's store: it is left in place, neither
  replaced nor served, and the file is served as stored.
- **Creation:** header and blob are written to `.slots.tmp.<pid>.<n>`, then
  `link()`ed into place. That fails if the store already exists, so a store is
  never visible without its layout.
- **Commit:** under an exclusive `flock` on the file:
  1. read the blocks written since;
  2. skip slots already committed;
  3. write one block at the 4096-aligned offset past the file's end, every
     block read, and the furthest end any block header CLAIMS — valid or not.
     A block cut short by a crash still owns its claimed extent: once the file
     grows past it, its header passes its checks and a reader steps over
     everything inside it, so nothing may be written there.

  A commit whose open file is no longer the one at the path (the store was
  dropped or replaced) writes nothing.
- **Reading new blocks** takes the shared lock without waiting. A block that
  fails its checks is debris of a commit that died, and is stepped over.

## In-use record: `<dir>/inuse/<hh>/<hash>`

Which of a file's maps were handed to readers, and when: one small text file
per entry, beside `objects/` rather than in it, so removing a file's cached
data (`ucache rm`, `clear`, an invalidation, eviction) leaves it. It expires on
its own: removed once nothing in it has been in use for `in_use_seconds`.

```
ucache-inuse 1
settings <layout version> <slot factor x100> <codecs, or ->
high <highest address handed out>
range <store id, 16 hex digits> <first address> <end address> <last use> <last held> <pid>
```

- A `range` line names a map by its addresses: a mixed map's tree record, a
  compact map's range -- and a TTree compact map's tree record too, which lies
  apart from its range. With pid `0` it is a use: the newest time a map's
  metadata was handed out, or a process that held the map closed the file. The
  window counts from it.
- A line with a pid is that process's hold: noted when one of its handles is
  handed the map, refreshed while one holds it, removed at its last close. A
  held map stays in use whatever the window; a process that ends without
  closing lets go 20 minutes after its last refresh.
- `settings`: what the slot layout readers were shown was computed with. A
  store rebuilt while the record is in use is laid out with them, whatever the
  rebuilding process's `recompress_codecs`, so the positions readers hold stay
  right.
- Written in place under an exclusive `flock`, never fsynced; a line that does
  not parse is skipped, and a first line other than `ucache-inuse 1` makes the
  file read as no record.

## Cache-freshness marker (optional): `<hash>.val`

An empty per-entry marker whose mtime records the last time the entry was
validated against the origin (a real remote stat). Written only when
`UCACHE_REVALIDATE_S > 0`; evicted / invalidated / orphan-swept with the
entry like the other sidecars. At open, if the marker is younger than
`UCACHE_REVALIDATE_S` and a `.meta` is present, the plugin trusts the local
entry and skips the remote open+stat entirely — serving from the
byte cache / replica, touching the origin only on a genuine miss (fail-open).
Advisory: a missing/stale marker just triggers a normal revalidation; the
per-page CRC guarantees still hold regardless.

## Record lines of other kinds in `stats/<stem>.files.jsonl`

Beside the per-file records ([STATS.md](STATS.md)), the per-file record file
holds lines of other kinds, one complete JSON object per line. Each carries
`"kind"` and a version `"v"`, and never `"key"`: readers of per-file records
take only lines with a `"key"` string, so they skip these, and a reader of
these takes only the kinds and versions it knows. A newer field is added at
the end of a line under a name no other field uses; a different meaning gets a
new `"v"`. No other companion file is used, because older readers count every
other `*.jsonl` in `stats/` as a counter file. The lines are queued in memory
(at most 4096) and written at the periodic checkpoint and at exit; a process
that ends without either loses those queued since its last checkpoint, as it
loses its counters. The last line can be torn by a crash; readers skip a line
that does not end in `}`. A message never carries a URL's query (`?…`), which
can hold an access token, and `file` is the cache key, which has none.

**`"kind":"open"`, v1** — one line per origin open attempt the plugin watched:

```json
{"kind":"open","v":1,"ts_ms":0,"file":"root://h:1094/p","seq":7,"attempt":1,
 "route":"relay","status":"ok","code":0,"errno":0,"class":"none",
 "chain":"g:1094/0x102*,r:1094/0x2,d:1094/0x1","data_server":"d:1094",
 "site":"dom.ain","site_src":"domain","stat_flags":16,"size":123,"us":812,
 "timeout_s":60,"msg":""}
```

- `seq` numbers one open of one handle, its retries included (unique in the
  process); `attempt` counts from 1 within it.
- `route`: `relay` (the reader's own open of a file the cache serves),
  `lazy` (a trusted handle's open at its first miss), `degraded` (a trusted
  handle whose cached entry was gone at its first read), `fork` (a forked
  child's open afresh of a handle it inherited), `failover` (with
  `adaptive_federation = on`, an attempt of any of these at another site after
  the open failed at one; its `seq` is the first attempt's), `candidate` (an
  attempt at a server a regional redirector listed as holding the file, opened
  directly or through its own redirector), `read` (with `adaptive_federation =
  on`, every attempt of an open made because reads of the file stopped at the
  server it was open at; the `origin_failure` line of the read that stopped it
  comes first, and names that server in `data_server`).
- `status` `ok` or `error`, with the client's `code` and `errno` (the server's
  error code when `code` is 400) and the provisional `class` of a failure
  (`none` on success; the classes are the `origin_failures_*` counters'
  in [STATS.md](STATS.md)).
- `chain`: the hosts the open went through, as the client lists them, each
  `host:port/0x<login flags>` (flags 0 for a host that never answered), `*`
  after the host the client uses as its load balancer, `^` after a data server
  reached with an EOS capability. `data_server` is the last of them, the
  server that answered; `site` its name without the first label (no DNS: an
  address, or a name without a dot, is its own site), `site_src` how the site
  was found (`domain`).
- `stat_flags` and `size`: the file's stat as the open answered it; -1 on
  failure.
- `us`: issue to answer; `timeout_s`: the timeout the request carried, the
  client's `RequestTimeout` when it carried none. With `adaptive_federation =
  on` that is the open deadline, 60 s, or a shorter open timeout of the
  reader's own, or the client's `RequestTimeout` when the reader gave none and
  that is shorter. With it off (and in records of builds without the
  deadline), a `relay` line has the reader's own open timeout, or the
  `RequestTimeout` when it gave none, and a `lazy`, `degraded` or `fork` line
  has the `RequestTimeout`: uCache's own opens carry no timeout then.
- `msg`: the error's text, at most 200 bytes as written (escapes included);
  `""` on success.
- With `adaptive_federation = on`, the line of the attempt that decided how
  the open ended has, after `msg`, `"outcome"` (`ok` or `error`) and
  `"budget_s"`, the time the open's attempts were allowed in all, in seconds.
  An open that ended because its handle was closed before a next attempt was
  issued has no such line.

An open the client refuses at once, before it reaches any server (the open
call itself returns the error, and no answer comes), has a line as well:
`status` `error` with the client's code, an empty `chain`, and as
`data_server` the host:port of the URL it was asked to open, since no server
answered; `site` is that host's. Such a refusal leaves the file closed, so a
later open of it is a new attempt with a line of its own. An open issued again
on a file whose earlier open was sent (in progress, open, or failed in its
answer), which the client answers from that file's own state, asks nothing of
any server and has no line.

**`"kind":"origin_failure"`, v1** — one line per failed origin request:

```json
{"kind":"origin_failure","v":1,"ts_ms":0,"file":"root://h:1094/p","op":"read",
 "off":0,"len":0,"code":108,"errno":0,"class":"client","data_server":"d:1094",
 "msg":""}
```

- `op`: `read` and `readv` (the byte cache's fetches, and a basket there
  whose key is not where the tree's record puts it), `first_pass` (a
  replica's first pass, and a basket or page there that failed its checksum),
  `records` (the file's own records, read to set up its layout), `relay` (a
  request passed straight to the origin), `open` (with `adaptive_federation =
  on`, a copy refused after its open succeeded: an empty copy of a file named
  `*.root`, or one whose size is not the end its own header states, `msg` saying which;
  class `data`, code 14, `off` and `len` 0). When no other copy is left, the
  open's last line has `outcome` `error` although its own status is `ok`: the
  job's open failed (the copy was refused).
- `off`, `len`: the span asked for (a vector read's first offset and its
  bytes); `data_server`: the last host of the failed answer, else the server
  the file's open reached (for a pass-through handle, whose open is not
  watched, the server that delivered most of its bytes; `""` when none is
  known); `codec`, after `data_server`, on a checksum failure
  whose compression is known.
- At most 16 lines per file, server and class in a process; the per-file
  record's `data_servers` counts every failed read. Failed opens are in the
  `open` lines only; a copy refused after its open succeeded is there too, as
  a line of op `open`.

**`"kind":"size_disagreement"`, v1** — with `adaptive_federation = on`, one
line each time an open uCache made of a file it holds landed on a copy of
another size through a redirector, whether it was then made again at another
site or refused:

```json
{"kind":"size_disagreement","v":1,"ts_ms":0,"file":"root://h:1094/p",
 "site":"dom.ain","server":"d:1094","sizes":"1000000/4247184"}
```

- `site` and `server`: where that copy is (`server` host:port, `site` its
  domain); `sizes`: the copy's size, then the size of the file uCache holds.
  The `size_disagreements` counter (STATS.md) counts these too.
