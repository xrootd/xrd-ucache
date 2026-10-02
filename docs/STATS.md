# Metrics

The numbers uCache records about your jobs: what each one means, and the format
of the JSON files it writes.

**To simply see how your cache is doing, run `ucache stats`** — it prints a
human-readable summary of these same numbers. This file is for reading the raw
JSON, or building tooling on it. The user guide's monitoring section explains
how to read the printed summary.

## Which number answers which question

| question | look at |
|---|---|
| Is the cache being used at all? | `opens`, `files_opened`, `hit_bytes` |
| Did a warm pass avoid the network? | `origin_bytes` — 0 means everything came from local disk |
| Is anything wrong? | `crc_failures`, `failopen_events`, `validations_failed` — **all three should be 0** |
| Is the cache big enough? | `admissions_bypassed`, `evicted_bytes`, `evicted_entries` |
| Where did the bytes come from? | `ram_hit_bytes`, `hit_bytes`, `replica_bytes_served`, `miss_bytes`, `relay_bytes` |
| Is the job re-reading the same data? | `opens` / `files_opened`, `first_touch_bytes` against `hit_bytes` |
| What is the cache disk being asked to do? | `hit_disk_reads`, `hit_disk_bytes`, `replica_reads`, `replica_read_bytes`, `hist_*_read_bytes` |
| Is the cache disk keeping up? | `buffer_stalls`, `buffer_stall_us`, `hist_hit_read_us`, `hist_flush_write_us` |
| Did the replica tier get used? | `replica_opens`, `replica_published`, `replica_bytes_served` |
| Were replicas created as files were read? | `cold_replica_files`, `cold_replica_in_bytes`, `cold_replica_baskets_kept`, `cold_replica_declined`, `cold_replica_skipped` |
| Were later passes shown the converted baskets at their real size? | `slot_maps_made`, `slot_map_opens`, `slot_map_full` |

## Where the numbers are written

These records are also what `ucache summary` and `ucache history` read: the
file name carries the store's start time and the last line carries its end, so
one process's work is bracketed without any periodic sampling, and the
`.files.jsonl` companion says which entries the bytes belonged to. Consumers
should take the file name's `<start_ts>` as the start of the run.

One JSON line is appended to `$UCACHE_DIR/stats/<host>-<pid>-<start_ts>-<seq>.jsonl`
at every `CacheStore::dumpStats()`: explicit calls, store destruction, the
plugin's `atexit`, and **every `meta_flush_seconds` (default 30 s) while the
process runs**. That periodic line is what keeps a run visible when its
process exits without running destructors — Python's `multiprocessing` and
`concurrent.futures` workers `_exit()`, which skips `atexit` too — so such a
run is at most one period short instead of absent. Counters are cumulative,
so consumers take the last complete line. External tooling consumes these —
the schema is a correctness surface; changes require updating this file and
the consumers together.

## Line schema

```json
{"ts": 1783300000, "pid": 12345, "ts_ms": 1783300000123, "start_ms": 1783299400456,
 "opens": 0, "validations_failed": 0,
 "hit_bytes": 0, "miss_bytes": 0, "origin_bytes": 0, "served_bytes": 0,
 "origin_reads": 0, "fetches_joined": 0, "origin_readvs": 0, "page_writes": 0,
 "prefetch_issued_bytes": 0, "prefetch_served_bytes": 0, "prefetch_refetched_bytes": 0,
 "prefetch_dropped_unread": 0, "prefetch_late_bytes": 0, "prefetch_parses": 0,
 "prefetch_bridge_bytes": 0,
 "prefetch_disabled": 0, "prefetch_fetch_errors": 0,
 "crc_failures": 0, "meta_corrupt": 0,
 "evicted_entries": 0, "evicted_bytes": 0,
 "failopen_events": 0, "admissions_bypassed": 0,
 "open_retries": 0, "open_retries_exhausted": 0,
 "disabled_handles": 0, "copier_handles": 0, "copies_refused": 0,
 "direct_read_files": 0, "direct_read_bytes": 0,
 "replica_opens": 0, "replica_published": 0, "replica_invalid": 0,
 "replica_crc_failures": 0, "replica_punched_bytes": 0, "replica_orphans_swept": 0,
 "files_opened": 0, "ram_hit_bytes": 0, "first_touch_bytes": 0,
 "hit_disk_reads": 0, "hit_disk_bytes": 0, "hit_disk_seq": 0,
 "replica_bytes_served": 0,
 "cold_replica_files": 0, "cold_replica_in_bytes": 0, "cold_replica_out_bytes": 0,
 "cold_replica_baskets": 0, "cold_replica_baskets_kept": 0, "cold_replica_convert_us": 0,
 "cold_replica_declined": 0, "cold_replica_skipped": 0, "cold_origin_failures": 0,
 "slot_maps_made": 0, "slot_map_opens": 0, "slot_map_full": 0,
 "slot_crc_failures": 0, "slot_read_fallbacks": 0, "slot_pages_decoded": 0,
 "kept_reads": 0, "kept_read_bytes": 0,
 "pool_tasks": 0, "pool_busy_us": 0, "pool_queue_high_water": 0,
 "reader_wait_byte_us": 0, "reader_wait_replica_us": 0, "reader_wait_slots_us": 0,
 "reader_wait_origin_us": 0, "reader_threads": 0,
 "replica_reads": 0, "replica_read_bytes": 0,
 "relay_bytes": 0,
 "readv_chunks": 0, "readv_calls": 0, "readv_mixed": 0,
 "flush_runs": 0, "flush_run_bytes": 0,
 "buffer_stalls": 0, "buffer_stall_us": 0,
 "cpu_us": 0, "instructions": 0, "cycles": 0, "counter_source": "perf", "pmu_duty": 1.0000,
 "hist_hit_read_us": [..], "hist_miss_read_us": [..], "hist_origin_rt_us": [..],
 "hist_relay_rt_us": [..], "hist_open_us": [..], "hist_replica_read_us": [..],
 "hist_flush_write_us": [..], "hist_meta_flush_us": [..],
 "hist_req_read_bytes": [..], "hist_hit_read_bytes": [..],
 "hist_replica_read_bytes": [..],
 "hist_slot_read_us": [..], "hist_cold_request_us": [..], "hist_pool_queue_us": [..],
 "entries": [{"key": "root://host:1094/path", "file_size": 0,
              "cached_bytes": 0, "page_size": 4096}]}
```

## Field notes

What each counter means, and the reasoning behind the ones that are easy to
misread. Three words recur. The **byte tier** is the page-granular copy of the
original file — the ordinary cache. The **replica tier** is a recompressed
overlay built by `ucache recompress`, which serves the same data with less
decompression work; a cache with recompression off has none. A **sidecar** is
the small file beside each cached entry holding its page bitmap and checksums.
[How it works](USER_GUIDE.md#how-it-works-briefly) covers the tiers, and
[FORMAT.md](FORMAT.md) the files.

### Bytes and where they came from

Every read is served from somewhere, and the counters name the source. Bytes
served locally are `hit_bytes` (byte tier) and `replica_bytes_served` (replica
tier); bytes that had to be fetched are `miss_bytes`; bytes the cache never
touched at all are `relay_bytes`. On a warm pass the fetched figure should be
zero.

- `origin_bytes` — bytes requested from the origin **after page rounding**, so
  it is the numerator of read amplification: divide by the bytes the job asked
  for to see how much extra the cache pulled to satisfy them.
- Which layer maintains which counter, when tracing an unexpected value back to
  its source: `miss_bytes`/`served_bytes`/`origin_reads`/`origin_readvs` are
  incremented by the plugin layer; the core owns `opens`, `hit_bytes`,
  `page_writes`, `crc_failures`, `meta_corrupt`, `validations_failed`,
  `evicted_*`, `failopen_events`.

### Capacity, and things going wrong

These are the counters to read first when something looks off. `crc_failures`,
`failopen_events` and `validations_failed` should all be zero. By contrast
`admissions_bypassed` is not a fault at all — it records a deliberate decision
not to cache, and says the cache is under pressure rather than broken.

- `admissions_bypassed` — files NOT cached because every resident entry was
  still inside the eviction protection window (`in_use_seconds`), so the
  cache had stopped growing rather than evict data a running job still needs.
  These reads SUCCEED, uncached. Deliberately separate from `failopen_events`:
  that counter means something went wrong, while this is a capacity decision. A
  non-zero value here means the cache is too small for the working set — see
  `ucache status`, which names the remedy.
- `validations_failed` — cached entries discarded because they no longer
  matched the file at the origin: a different size, mtime or checksum, or a
  changed page size. The entry is refetched from scratch, so a read still
  succeeds; a steadily rising count means the data is being rewritten
  underneath the cache.
- `crc_failures` — pages that failed CRC on read and were quarantined
  (refetched later). `meta_corrupt` — sidecars that failed to load (torn or
  damaged) and were rebuilt.
- `failopen_events` — cache-side faults that degraded to pass-through
  (failed page writes, failed sidecar persists). `disabled_handles` —
  handles that tripped `UCACHE_MAX_ERRORS` (plugin layer).
- `copier_handles` (plugin layer) — file handles opened for a copy: by a copy
  tool (`xrdcp`, `xrdfs`, `xrdadler32`, `edmCopyUtil`), by `hadd` or one of
  ROOT's command-line tools, from inside XRootD's copy engine, from ROOT's
  `TFile::Cp` or `TFileMerger`, or from gfal2's xrootd plugin. They are
  served as pure pass-through, so a copy is the origin's bytes: their reads
  are in `relay_bytes`, and they add nothing to the cache. Also counted: a
  handle whose first request read the whole file at once, for a file with a
  compact replica or a slot store. Zero with `copy_detect = off`.
- `copies_refused` (plugin layer) — requests refused because they would have
  completed a copy, in pieces up to the origin's size, of a file shown in a
  replica's layout (the handle read all of [0, origin size) without reading
  any of the replica's own part first): the copy fails with an error naming
  `UCACHE_DISABLE=1` instead of coming out corrupt. Not a cache error: never
  in `failopen_events`.
- `direct_read_files` / `direct_read_bytes` (plugin layer) — ROOT files this
  process decided to read straight from the origin because its reads covered
  branches holding more than `max_read_fraction` of the file's data, and the
  bytes it fetched for their data and did not keep (also in `relay_bytes`).
  A file is counted once per process. Zero with `max_read_fraction = 100`.
- `open_retries` / `open_retries_exhausted` (plugin layer) —
  transient inner-open failures re-attempted (`UCACHE_OPEN_RETRIES`), and opens
  that ultimately gave up after retrying. Uncounted when caching is off (retry
  still works, just unrecorded — no store to hold the counters).

### Replica tier

Only meaningful once `ucache recompress` has built overlays; all zero
otherwise. They track the life of an overlay: built, adopted for serving,
rejected, or cleaned up. See the replica section of [FORMAT.md](FORMAT.md).

- `replica_opens` — overlays adopted for serving, after the full verification
  done when the entry is opened.
- `replica_published` — overlays successfully built and made available.
- `replica_invalid` — overlays rejected at open as torn, stale or mismatched;
  each rejection also drops the overlay.
- `replica_crc_failures` — overlay pages whose checksum did not match, at open
  or while serving.
- `replica_punched_bytes` — original bytes reclaimed once an overlay covered
  them, freeing the space the byte-tier copy held.
- `replica_orphans_swept` — leftover overlay files removed by eviction, after a
  crash or a version change.

### Replicas created as files are read

With `recompress = on`, a file that has no replica gets a slot store on its
first open, and every basket a job reads that the store does not hold yet is
converted as it arrives -- on the first pass, and later for branches read for
the first time. All zero otherwise. `ucache stats` prints them on one
`converted on read` line, and `cold_replica_skipped` on a `not converted` line
of its own. For an RNTuple file, read "page" wherever these say "basket".

- `cold_replica_files` — slot stores created.
- `cold_replica_in_bytes` / `cold_replica_out_bytes` — original basket bytes
  converted, and the converted records they became (ZSTD-1, or uncompressed
  where ZSTD could not shrink a basket).
- `cold_replica_baskets` — baskets converted.
- `cold_replica_baskets_kept` — baskets kept exactly as stored because their
  converted form did not fit their slot, or they could not be decoded. Their
  original bytes are the byte cache's share of such a file. (Branches whose
  codec is not in `recompress_codecs` are not counted: they are never
  converted.)
- `cold_replica_convert_us` — time spent converting, summed over the threads that did
  it; it is CPU a pass spends that a pass without recompression does not.
- `cold_replica_declined` — converted records not kept: they still did not fit
  above the free-space floor after an eviction pass, writing them failed (the
  log says which), the store was removed meanwhile, or the process already held
  its limit of records waiting to be written. They were served to the job that
  read them; a later read converts them again.
- `cold_origin_failures` — origin reads sent on the first pass that failed. The
  request is served again from the start, twice at most, so a read the origin
  fails once or twice is answered; a third failure in a row reaches the reader,
  as it would without the cache. Nonzero means origin trouble during the pass.
- `cold_replica_skipped` — files whose first pass was declined by what the file
  itself holds: baskets in a codec `recompress_codecs` does not name, baskets
  stored uncompressed, or a structure the first pass cannot lay out. Such a
  file is served as stored. Each file is counted once, by the process that
  decided it, when that decision is recorded in the cache; later reads of the
  file do not count it again. The plugin logs the first such file of each
  process as a warning, with the reason (see TROUBLESHOOTING.md,
  "`recompress = on` but no replicas ever appear").
- `slot_maps_made` — maps this process made (`mixed_maps = on`): a later open
  is shown the baskets or RNTuple pages converted by then at their real size. One is
  made a moment after a process is done with a file, when enough has been
  converted since the newest (a twentieth of what the file's store holds, and
  after the first map 8 MiB) and nobody else is converting it; after the
  first, at most one an hour.
- `slot_map_opens` — opens shown a map. An open of a file with a slot store
  that is not counted here is shown the slot layout alone: no map yet, or
  `mixed_maps = off`.
- `slot_map_refused` — reads refused because they lay in the range of a compact
  map no longer live: replaced and, for `in_use_seconds`, neither held nor
  used; or a range of a store that was replaced since. The reader gets an
  error and the plugin logs one; reopening the file shows the current map.
- `slot_map_full` — maps not made because there was no place for one: the
  baskets converted since the newest map are read at their slot's size until
  a place frees: when no handle holds a replaced map and it has not been used
  for `in_use_seconds`.
- `slot_crc_failures` — records read from a slot store that failed their check
  (damage on the cache disk, or a torn write): each is dropped and converted
  again from the origin, and the read that met it is served again.
- `slot_read_fallbacks` — reads of neighbouring stored records (joined into one
  read of at most 1 MiB) that failed or came back short, and were read again one
  record at a time. Nonzero means a cache disk returning errors.
- `slot_pages_decoded` — RNTuple pages decoded from their stored record to be
  served. A page converted for the request that asked for it is served from the
  conversion's own decoded copy and is not counted, so a first pass counts here
  only pages it finds already stored. A later pass shown a map counts only the
  pages it reads in the slot layout -- column ranges not yet wholly converted,
  and the few an open reads past the file's original end; the pages of the
  map's range are served as stored and ROOT decodes them.
- `kept_reads` / `kept_read_bytes` — reads of baskets a slot store keeps as
  stored, served from the byte cache's copy of the original. They are not in
  `hit_disk_reads` (that counts the byte tier's own serving).
- `hist_slot_read_us` — time to read and check one stored slot record.
- `hist_cold_request_us` — a request served in the first-pass layout, from its
  arrival to the answer handed to the reader, retries included: what a reader
  waits for on the first pass.
- `pool_tasks`, `pool_busy_us`, `pool_queue_high_water`, `hist_pool_queue_us` —
  the serving pool, the threads every read is posted to: tasks it ran, time
  its threads spent running them, the most tasks waiting at once, and how long
  each waited before a thread took it. A long queue wait with the threads busy
  the whole time means the pool, not the disk or the origin, sets the pace.
- `reader_wait_byte_us`, `reader_wait_replica_us`, `reader_wait_slots_us`,
  `reader_wait_origin_us`, `reader_threads` — how long the threads that read
  through the cache waited on it. For each reading thread, the time during
  which it had at least one read outstanding in the cache, from the moment it
  sent one while none was outstanding until the last one was answered (reads
  a thread sends together and waits for together count once). Each such wait
  is charged to the costliest place any of its reads needed: the origin (a
  fetch, or waiting for one already on the wire), then a slot store (the
  first-pass layout), then a compact replica, then the byte cache.
  `reader_threads` counts the threads that read. For TTree the readers are the
  analysis threads, so the sum divided by threads x wall is the share of
  their time spent waiting on the cache; for RNTuple they are ROOT's own I/O
  threads, which wait by design while the analysis computes, so there the sum
  is request latency only. Handles that do not cache (`UCACHE_DISABLE=1`, a
  copy) record nothing here.

Records a job reads from a slot store count in `replica_bytes_served` (and
their reads from the cache disk in `replica_reads` / `replica_read_bytes`);
records it converts from what the origin sends count in `miss_bytes`, as any
first read does, so the pass that makes a store reads as a fill. The zeros
that pad each slot count in neither.

`ucache summary` never takes a run that converted baskets this way as its
baseline: its time includes the cache's own conversion work. A plain first pass
that only filled the byte cache is unaffected and still qualifies under the
usual rules.

### What the printed summary is derived from

`ucache stats` does the divisions in its `workflow:` block from the counters
below — how often each file was opened, which tier served what, how much of the
reading was re-reading, and what the cache disk was asked to do.

- Workflow counters: `files_opened` — distinct keys opened by this process
  (`opens/files_opened` ≫ 1 = a reopen loop); `ram_hit_bytes` — hit bytes
  served from the fill buffer's staged RAM (subset of `hit_bytes`);
  `first_touch_bytes` — bytes served for the first time in the entry's
  in-process lifetime (the rest are re-reads).
- Byte-tier disk reads: `hit_disk_reads` /
  `hit_disk_bytes` / `hit_disk_seq` — byte-tier disk reads, their bytes, and
  how many continued sequentially from the previous read. **One
  `hit_disk_reads` is one pread covering a whole contiguous run of cached
  pages, not one per page** — a request spanning ten adjacent pages costs one
  read op — so `hit_disk_bytes / hit_disk_reads` is the mean size the device
  sees and `hit_disk_reads` divided by wall time is the op rate it is charged
  for (the currency on IOPS-quota storage). Bytes are counted at whole-page
  granularity, since each page is still checksummed in full. `hit_disk_bytes`
  is directly comparable to versions before reads were coalesced for every
  request that SUCCEEDS (verified byte-identical on the same workload). It
  differs only for a request that fails part way: the older per-page loop had
  already read some pages before discovering an absent one, whereas the planning
  pass now finds that page first and reads nothing — so a run with a non-zero
  acceptance triple can show slightly fewer bytes than an older one.

- Replica-tier reads: `replica_bytes_served` / `replica_reads` /
  `replica_read_bytes` — bytes served from replica overlays, the physical `.tdata` preads that delivered
  them, and the bytes those preads moved (the replica-tier analogue of the
  byte-tier trio, coalesced the same way; `replica_read_bytes` ≥
  `replica_bytes_served` because whole overlay pages are read and verified,
  and a page read for two different user reads counts twice).
- Pass-through and vector reads: `relay_bytes` — pure pass-through, the cache
  never touched them (copies included: see `copier_handles`; files read directly
  too: see `direct_read_files`); `readv_chunks` /
  `readv_calls` / `readv_mixed` — vector-read chunks seen, vector reads the
  cache handled (`readv_chunks / readv_calls` = mean batch width), and
  vectors mixing hits with misses.
- Fill path: `flush_runs` / `flush_run_bytes` — coalesced fill-buffer pwrite
  runs and their bytes; `buffer_stalls` / `buffer_stall_us` — fills that had
  to drain synchronously at the buffer cap, and the wall time lost;
  `fetches_joined` — misses that joined an identical in-flight fetch instead
  of fetching again.
- Read-ahead (`prefetch`, OFF by default; when switched on the plugin predicts a TTree reader's
  next fill from the file's own basket map and fetches it while the reader
  computes, into RAM only): `prefetch_issued_bytes` — asked of the origin ahead
  of demand; `prefetch_served_bytes` — of those, bytes the reader then
  demanded (they also count as hits); `prefetch_refetched_bytes` — served
  bytes a demand read had fetched again in the meantime; `prefetch_dropped_unread`
  — bytes read ahead that the reader never asked for, dropped from RAM and
  never written to the cache; `prefetch_late_bytes` — arrived after the demand
  read had already brought them; `prefetch_parses` — basket maps parsed
  (successes; a map is retried while the reader's header pages are still being
  staged, and those attempts do not count);
  `prefetch_bridge_bytes` — padding between two predicted ranges, fetched to
  save a request element and discarded on arrival (`prefetch_bridge_kb`):
  bandwidth only, never staged, never cached, and never counted against
  read-ahead's own accuracy; `prefetch_disabled` — 1 once a
  process switched read-ahead off, so a total over several processes is a
  count of how many did; `prefetch_fetch_errors` — read-ahead
  wire reads that failed (dropped; never a fail-open event). `fetches_joined` counts the reads that waited for a fetch already on the
  wire rather than sending their own, which is how read-ahead stops paying
  for the same bytes twice (`prefetch_join`). Read-ahead's wire
  traffic is counted in `origin_bytes` and `origin_readvs`, as any other origin
  read, but NOT in `hist_origin_rt_us`: that histogram is time a reader waited,
  and nobody waited for these. A run that read ahead at all is never taken as a
  no-cache baseline by `summary`, whether or not the prediction was used.
  Per file, `prefetch_issued` / `prefetch_served` / `prefetch_dropped` in the
  `.files.jsonl` record.
- `hist_relay_rt_us` — answer times of the requests the plugin passed straight
  through to the origin (the ones whose bytes count in `relay_bytes`, a
  `UCACHE_DISABLE` run's included), process-wide. Kept apart from
  `hist_origin_rt_us`, which is the cache's own fetches and keeps meaning only
  that.
- Histograms are log2 buckets: bucket *i* counts samples with
  `floor(log2(v)) == i` (bucket 0 = ≤1); arrays are trimmed of trailing
  zeros, 40 buckets max. The `_us` ones bucket microseconds; the
  `_bytes` ones bucket bytes (bucket 12 = 4 KiB, 16 = 64 KiB).
- Read-shape histograms answer "what is the storage actually asked for":
  `hist_req_read_bytes` — sizes the client requested (one sample per read
  and per vector-read chunk, before the cache decides how to serve them);
  `hist_hit_read_bytes` / `hist_replica_read_bytes` — sizes of the preads
  each tier issued. Issued sizes *smaller* than requested are normal on a
  partially cached entry, because a read stops at any page that is absent or
  still staged in RAM, and at the coalescing cap; issued sizes *larger* than
  requested come from rounding out to whole pages. What the pair is for is the
  shape of the tail: many issued reads far smaller than the requests they serve
  means the entry is fragmented, and on op-priced storage that is what costs.
- `entries` — per-entry page coverage of entries live in the process at dump
  time (`cached_bytes / file_size` = the coverage signal gating replica
  materialization). Closed entries' coverage persists in their
  sidecars and is aggregated by `ucache stats` from there.
- `handles_high_water` — the most cache-engaged handles open at once in this
  process. The origin's delivery is a non-monotone function of concurrency, so
  measurements at different widths are not interchangeable; consumers pool runs
  across time only when this agrees. Aggregated across processes it is a MAX,
  never a sum.
- `disabled` — present (as `1`) when the process ran with `UCACHE_DISABLE`:
  the cache was out of the loop and every byte was relayed. Such a run is a
  measured NO-CACHE BASELINE; `ucache summary`/`history` compare cached runs
  against it, matched by key through the per-file records.
- `threads_high_water` — the most threads this process had alive at once,
  every thread it owns, not the ones doing the work. Recorded, not a width:
  a runtime's worker pool inflates it freely.
- `peak_cores` — the most cores busy in any one second, from CPU time sampled
  once a second on the read path. This IS a width: CPU seconds per wall second
  cannot exceed the cores actually executing. Sampled where reads happen, not
  where files open, because opens all land before the work starts and would
  report one core for a job running on thirty-two.
- `cpu_us`, `instructions`, `cycles` — work done, as opposed to time taken.
  Instructions are the stable one across routes: the same analysis over the
  same data executes very nearly the same number however the bytes reached it,
  which is what makes two runs comparable at all. Requires performance counters
  to be permitted; absent (zero) where they are not.
- `counter_source` — where `instructions` and `cycles` came from: `perf`
  (Linux performance counters: user space only, counted from when the cache
  engaged in the process) or `rusage` (macOS per-process counters: the whole
  process from its start, user and kernel). The two do not count the same
  thing, so compare counts only between runs with the same source. Absent when
  there are no counters.
- `pmu_duty` — `perf` only, four decimals: the smaller, over the two counters,
  of the share of the time a counter was enabled that it was actually counting.
  Below 1 the processor's counters were shared (with other jobs on the node, or
  other counters in the process) and the kernel multiplexed them; the
  `instructions` and `cycles` written are already SCALED by enabled/running
  time, so they estimate the full count, and a low duty is how much of it was
  estimated. Like the counts, it covers the process from when the counters
  opened, so each line's includes the lines before it; `ucache history --json`
  reports the last line's, which is the whole run's.
- `ts_ms`, `start_ms` — the same clock as `ts` and the file name's start, in
  milliseconds: when this line was written, and when this process's store
  started. Their difference is the run's wall to a millisecond, where the
  file name and `ts` give it to a second. The final line's time, and its
  `cpu_us`, `instructions` and `cycles`, are read as the final dump begins,
  before the dump writes the records of files still open: that work is not the
  job's, and is in neither.
- `origin_reads_in_flight_high_water` — the most reads the ORIGIN was being
  asked for at once. Counted where a read is issued to the origin and given
  back at the top of its completion handler, so it spans the READ. This is the
  width that sets the origin's delivery rate, and it is the one to use. A warm
  run reads 0, correctly: nothing was outstanding at the origin because nothing
  went there. It is recorded and displayed; nothing is decided from it.
- `reads_in_flight_high_water` — SUPERSEDED by the counter above, and kept only
  so the old name is not silently reused for the new meaning. It counted reads
  being SET UP at once: every route dispatches and returns while a synchronous
  caller waits above this library, so it spans the handoff, not the read, and a
  job with thirty-two reads genuinely outstanding can report a handful. It is
  emitted as a permanent 0. Do not read anything into it.
- Counters are process-lifetime monotonic; successive lines from one process
  supersede earlier ones (consumers take the last line per file).

## Companion files (same stem)

- `<stem>.files.jsonl` — one record per entry per process lifetime, emitted
  once (at last release, or at the final dump). A handle the cache never
  served — pass-through under `UCACHE_DISABLE`, a copy, or a write-opened file — also
  leaves one at close, with the relayed bytes under `wire_bytes` and the
  cache-side fields zero; that is what makes a baseline run matchable file by
  file. Consumed by
  `ucache stats --files`:

  ```json
  {"ts": 0, "ts_ms": 0, "key": "root://…", "opens": 0, "served_bytes": 0, "ram_bytes": 0,
   "replica_bytes": 0, "disk_reads": 0, "disk_seq": 0, "disk_bytes": 0,
   "first_touch_bytes": 0, "wire_bytes": 0, "direct_bytes": 0, "span_us": 0,
   "origin_size": 0, "read_sig": "", "read_buckets": 0, "mode": "cached",
   "orig_bytes": 0, "unique_bytes": 0, "origin_rt_us": [..]}
  ```

  The fields from `orig_bytes` on are recorded for the report service
  (`ucache publish`); `summary` and `history` do not interpret them. Recording
  them never changes what is read, fetched, cached or served: whenever one
  cannot be had cheaply, it is left out.

  - `orig_bytes` — the bytes the application asked this file for, in the
    ORIGINAL file's coordinates, re-reads counted, clipped to `origin_size`.
  - `unique_bytes` — the distinct bytes among them: the union of the same
    ranges.
  - `origin_rt_us` — answer times of this file's requests to the origin, a
    histogram like `hist_origin_rt_us`: the cache's own fetches on the cached
    route, the relayed requests on the pass-through route. A cached entry's
    record also holds the requests its handles relayed past the cache (a file
    read directly under `max_read_fraction`, a fail-open), so its sum is
    `hist_origin_rt_us` plus that share of `hist_relay_rt_us`. Read-ahead
    fetches are left out, as they are from `hist_origin_rt_us`. Absent when
    there were none.
  - `ts_ms` — when the record was written, in milliseconds; for a record the
    final dump writes (a file still open at exit), the moment the dump began,
    like the counter line it goes with.

  **`orig_bytes` and `unique_bytes` are exact or absent**, never estimated:
  absent means unknown, and a file opened and not read has 0. How each route
  gets them:

  - Byte cache, pass-through, cache disabled: the application's requests
    address the original file, and are counted as they are.
  - A replica (a recompressed copy, or the slot store of `recompress = on`)
    has its own layout, and a read of it is mapped back through the replica's
    map of what each piece holds. Bytes still where the original has them count
    as themselves. A relocated basket or page read WHOLE counts as its whole
    original record. A read that covers only PART of a relocated basket or page
    has no place in the original file -- its bytes are recompressed, or
    decoded -- so from that read on the file records neither field, for the
    rest of the process. ROOT reads a TTree's baskets and its tree record
    whole, so a TTree file that ROOT reads through a replica keeps them. Other
    readers mostly do not: measured, uproot's reads of a TTree through a
    replica leave neither, and so do ROOT's reads of an RNTuple, whose
    buffered reads near the footer take part of the pages beside it.
  - Either is absent too when the footprint was discarded; `orig_bytes` also
    when the file's size was not known well enough to clip the reads to it;
    `unique_bytes` also when the file's range set outgrew its cap (100000
    ranges per file, 128 MiB of ranges per process).

  They count what the reader asked of the layout it was shown, and a reader
  does not ask a replica for the same bytes as the original: one that reads
  ahead in blocks counts the extra bytes of the original on the byte route,
  and on a replica either whole pieces or, once it takes part of one, no
  count at all. Compare them between runs of one route; `read_sig` (below) is
  the field made to compare across routes.

  **One key can have several records in a process** (one per relayed handle,
  or an entry dropped and opened again), and the fields combine differently.
  `orig_bytes` and `unique_bytes` describe the process's whole footprint on the
  URL, so later records hold what earlier ones did: take them, as one unit,
  from the record with the largest `orig_bytes` (on a tie, the later one) —
  never add them. A record with `ts_ms` and no `orig_bytes` says the footprint
  became unknown after the records before it (discarded, a replica read that
  could not be mapped exactly, or asked past the file's end more often than it
  could follow): the unit is then unknown, until a later record carries
  `orig_bytes` again.
  `origin_rt_us` covers only the requests of the record's own handle or
  entry: add those bucket by bucket, as with `wire_bytes`.

  `direct_bytes` are the bytes a file read directly (`max_read_fraction`)
  fetched and did not keep. `mode` is `fill` when the process mostly fetched
  the file, `relay` when it mostly read it straight from the origin (a file
  read directly), and `cached` otherwise.

  `origin_size` is the file's size at the origin — the one measure of a file
  that means the same thing whichever route served it, and therefore the only
  sound weight when combining files.

  `read_sig` and `read_buckets` say WHICH parts of the file were read, and how
  many, in the ORIGINAL file's coordinates. The file is divided into 1 MiB
  blocks and the signature is a hash of the set that were touched; a read
  served from a replica is mapped back through the sidecar's origin map first,
  so the two routes are expressed in the same coordinates.

  That is not the same as agreeing, and MEASURED THEY DO NOT ALWAYS AGREE. A
  replica is a rewritten container, so the reader asks it for different spans
  than it asks the original -- not merely coalesced differently, requested
  differently. On a recorded file set, 26 files of 439 (94.1% agreement) had a
  whole block fall inside a gap the replica route never asks for: the byte
  route marked it, the replica route did not. Both are honest about what was
  read; they answer slightly different questions.

  So equality is evidence of the same work, while inequality is NOT proof of
  different work -- which is why runs are compared on the SHARE of files that
  agree rather than on a single hash over the whole run, and why that share is
  required to be high rather than total.

  An empty `read_sig` means UNKNOWN, never "nothing was read" — a replica
  written before origin maps existed cannot answer, and so says nothing rather
  than something wrong. Treat it as no evidence.

  **A consumer reading these must take the record with the most
  `read_buckets`, not the last one.** A file can be reported more than once in
  a process, and the footprint grows for as long as the process runs, so
  earlier records hold subsets of later ones. Taking the last record is right
  for every other field here and wrong for this one.

  `span_us` is the wall this file was live and working, from open to last
  activity. Where one worker handles one file at a time that is the worker's
  full cost for it — waits and the route's own CPU together — which is what
  makes files served by different routes comparable. `mode` is `cached`,
  `fill` (this process mostly fetched it) or `relay` (passed through, including
  a `UCACHE_DISABLE` run).

- `<stem>.trace.jsonl` — sampled per-operation IO trace, written only with
  `trace = io` (every `trace_sample`-th operation per class). Records are
  `{"t": <wall µs>, "w": <worker slot>, "op": "<operation>", "k": "<16-hex key
  hash>", "off": 0, "len": 0, "us": 0}`; `w` is a small per-thread slot id,
  assigned on that thread's first record and stable for its life, so a consumer
  can reconstruct each worker's timeline — the gap between one worker's
  completion and its next issue is that worker's compute. The first record for
  each key is a legend line
  `{"op": "key", "k": "<hash>", "url": "<full url>"}` mapping the hash back
  to the URL.
