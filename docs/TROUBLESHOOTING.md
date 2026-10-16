# uCache — Troubleshooting

Start with `ucache doctor` — it checks the install, the cache filesystem, and
activation (whichever way you configured it), and prints `[ OK ]` / `[WARN]` /
`[FAIL]` per item. Then `ucache test <url>` on one of your data files proves
the whole chain dynamically (cold + warm pass; warm must be origin-free).
Most issues below map to one of their lines.

In this document "with `adaptive_federation = on`" means: for a file the
switch is on for -- every file with `on`; with `auto`, the default, a file
opened at a server that reports a redirector's role (USER_GUIDE.md, "Which
files the switch is on for").

To see what uCache does, turn on debug messages the way you would for the
XRootD client: `XRD_LOGLEVEL=Debug XRD_LOGFILE=/tmp/xrd.log <your job>` puts
uCache's messages (topic `UCache`) beside the client's. Or
`ucache set log debug` for every job until `ucache unset log`. A log with a
warning starts with one line naming the uCache version, the program, the
thread count and the cache directory with its filesystem: send that line with
a report.

## The cache doesn't seem to be used (warm pass still hits the network)

`ucache stats` after two identical runs should show `origin_bytes == 0` on the
second. If not:

1. **No conf where XrdCl looks.** `doctor` shows `[FAIL] no plugin conf`.
   Write `~/.xrootd/client.plugins.d/ucache.conf` (USER_GUIDE §2) or run
   `ucache setup`. Note XrdCl resolves that `~` from the passwd database, not
   `$HOME` — they can differ on batch systems.
2. **Using the `XRD_PLUGINCONFDIR` route in a shell or job that doesn't set
   it.** The default-directory conf needs no environment at all; but if you
   keep the conf elsewhere via `XRD_PLUGINCONFDIR`, every shell and batch job
   must have that variable set (the job still runs — fail-open — just
   uncached). Easiest fix: move the conf to `~/.xrootd/client.plugins.d/`.
3. **The `*` slot is taken.** If a system plugin conf under
   `/etc/xrootd/client.plugins.d/` already binds `url = *`, your `url = *` user
   conf is shadowed and skipped (`doctor` and `setup` warn about this). Re-run
   `ucache setup --host <host:port>` (e.g. `eospublic.cern.ch:1094`) to bind your
   data host explicitly.
4. **Host mismatch.** The cache key includes host:port. If you read via a
   federation redirector on the cold pass and a direct site URL on the warm pass
   (or vice versa), they are different keys, unless the path has a logical name
   and `logical_name_prefixes` is set: then every URL of the file is one key,
   `lfn:/store/...` (USER_GUIDE.md, "Keys by logical name"). Otherwise read via
   a consistent URL.
5. **Caching disabled.** `UCACHE_DISABLE=1`, `disable = true` in the conf, or
   `ucache disable` was run. Re-enable with `ucache enable`.
6. **You tested with `xrdcp`.** A copy never uses the cache, by design: `xrdcp`,
   `xrdfs`, `xrdadler32`, XRootD's copy engine from any program (Python's
   `CopyProcess`, `gfal-copy`, `rucio download`), ROOT's `TFile::Cp`, `hadd`
   and ROOT's command-line tools (`rootcp`, `rootls`, ...) are
   recognised and read straight from the origin, because a copy must be the
   origin's bytes and uCache may show readers a file in a layout of its own.
   `stats` then shows `copier_handles` and direct (relayed) bytes, not hits —
   which also proves the plugin is loaded. Analysis reads (ROOT, RDataFrame,
   uproot) cache normally. To smoke-test, use `ucache test <url>`; to push one
   copy through the byte cache, `UCACHE_COPY_DETECT=off
   UCACHE_MAX_READ_FRACTION=100 XRD_CPUSEPGWRTRD=0 xrdcp …` (a copy reads all
   of a file, see item 8; recent `xrdcp` otherwise transfers with PgRead,
   which the plugin relays without caching).
7. **The job's XrdCl refuses the plugin (version or glibc).** Two variants,
   both **silently fail-open** (job correct, nothing cached); diagnose either
   with `XRD_LOGLEVEL=Debug <your job> … 2>&1 | grep -i plug` in the same
   environment:
   - *Version handshake:* XrdCl accepts a plugin only when it was built for
     the client's own major version, with a minor no newer than the client's.
     The release packages therefore carry two builds, each against its
     major's oldest release: `libXrdClUCache-5.so` (`@V:XrdClUCache v5.6.x`,
     loads in every 5.6+ client, CMSSW externals included — verified: xrootd
     5.6.4/5.8.4/5.9.6) and `libXrdClUCache-6.so` (`v6.0.x`, every 6.x
     client). A conf naming `libXrdClUCache.so` makes each client open its
     own. Two ways to lose that: naming one build in the conf (`lib = …-5.so`
     pins every client to it, and an XRootD 6 client — ROOT 6.40 in LCG_110,
     for one — then refuses it), or an install with no build for your
     client's major (a source build has only the one it was built against).
     If you build from source against newer headers, older clients of the
     same major (e.g. CMSSW's) log `Plugin version client v5.6.4 is
     incompatible …` and run uncached — rebuild against the oldest headers
     you need to serve. **`ucache doctor` checks all of this for you**: it
     works out which file the `xrootd` client on your PATH opens for your
     conf, and FAILs when that file is missing, built for another major, or
     newer than the client, or when the client is 4.x (a different ABI —
     `libXrdCl.so.3` is absent, so a 5.x plugin cannot load at all).
   - *Which framework release do I need?* Find your client version with
     `xrdcp --version` (or, in a CMSSW area, `scram tool info xrootd`). The
     prebuilt plugin needs xrootd **≥ 5.6**, or any 6.x. CMSSW ships modern xrootd in the
     latest patch release of **actively-maintained** cycles — e.g.
     `CMSSW_10_6_50` (2026) carries xrootd 5.7.2, though early patches of the
     same cycle (`10_6_26`) shipped 4.8.5 — so *upgrading to the newest patch
     in your cycle* usually resolves it. **Frozen** cycles never got the bump
     (`CMSSW_10_2_29` stays 4.8.3, `CMSSW_11_3_4` stays 4.12.3) and cannot run
     the plugin at any patch level; move to a maintained cycle.
   - *Older containers* (`cmssw-el8`/`cmssw-el7`): the el9-built `.so` needs
     GLIBC_2.34 and cannot load in an el8/el7 apptainer (glibc ≤ 2.28). el9
     CMSSW releases work as-is; an el8-flavor artifact is future work.
8. **The job reads most of each file.** A job whose reads of a ROOT file (a
   TTree or RNTuple of 64 MiB or more) cover branches holding more than
   `max_read_fraction` of the file's data (default 25%) reads that file
   straight from the origin and does not cache it: one `WARN` per job names
   the first such file and the share it read, and `ucache stats` counts them in
   `direct_read_files`. That is deliberate — such a job would fill the cache
   with the whole dataset. To cache it anyway, `UCACHE_MAX_READ_FRACTION=100
   <your job>`, or `ucache set max_read_fraction 100` for every job. With
   `max_read_fraction = 0` (`ucache settings` shows it) every ROOT file with a
   TTree or RNTuple, of any size, is read this way, with no warning: that
   setting caches none of the data of such a file's main tree or RNTuple (its
   records and other trees are cached). On a build without the codec libraries no file is
   read this way, whatever the setting (`ucache doctor` warns when it is `0`).
9. **`XRD_PLUGIN` is set.** While it is, XrdCl loads the library it names for
   every URL and reads no plugin config file at all, so a `ucache.conf` does
   nothing — and if it names some other plugin, uCache is not loaded.
   `ucache doctor` says which. Unset it, or point it at uCache on purpose
   (USER_GUIDE §2, "no file at all").

## `[Error][File] Plug-in factory failed to produce a plug-in … continuing without one`

Harmless despite the Error tag. uCache deliberately provides no *FileSystem*
plugin (locate/stat/query are pure pass-through), and XrdCl logs this line
whenever a client — CMSSW's XrdAdaptor and `edmFileUtil` do — creates an
`XrdCl::FileSystem` for a bound host. Your *file* reads still engage the
cache: check `ucache stats` (opens/hit/miss moving), not this message.

## cmsRun warm runs: `failed to determine data server name` / `server (unknown)`

Harmless warnings, and actually good news: a warm open inside the freshness
window is served **entirely from the local cache — no server is contacted**,
so CMSSW's XrdAdaptor has no data-server name to report and says "(unknown)".
The job runs normally, reads come from disk (`ucache stats`: warm
`origin_bytes ≈ 0`). Two more lines in the same family:
`[Error][PostMaster] Unable to get transport handler for file protocol` and
`[Error][XRootD] [localhost] Unable to send the message kXR_query …` —
XrdAdaptor probing the cache's local-redirect URL for readv limits (it lacks
the guard ROOT and uproot have); the probe fails fast and sane defaults
apply.

## A site's monitoring lists my jobs as `ucache`, not as ROOT or cmsRun

Expected, and it is one setting. uCache names itself as the application in the
login of every session it caches, so that sites can tell traffic that comes
through a cache from traffic that does not; your program's name goes in the
information string beside it (`ucache/1.0.0 (root.exe)`). If a dashboard,
accounting rule or site policy needs your program in the application field
instead, either turn the announcement off:

```sh
ucache set announce off      # or `announce = off` in ucache.conf
```

or set the name yourself, which uCache never overrides:

```sh
export XRD_APPNAME=my-analysis
```

Nothing else changes with it either way — the announcement is two strings in
the login request and is not involved in reading or caching a single byte.

## The file changed at the origin but uCache serves the old bytes

Expected inside the freshness window (default 7 days, `revalidate_seconds`):
a validated entry is served without contacting the origin, so a file
*replaced under the same name* keeps serving the cached version until the
window expires. Physics data is normally write-once, which is why the default
trusts the cache. When you know a file changed:

- `ucache rm <url>` — drop that entry; the next open re-fetches. With
  `logical_name_prefixes` set and a path with a logical name, the entry is the
  file's for every URL that names it, and `rm` drops it (and the URL's own
  entry, if one is left from before).
- `UCACHE_REVALIDATE_S=0 <your job>` — force origin re-checks for one run.
- `revalidate_seconds = 0` in your `ucache.conf` — always re-check (the
  pre-0.9.1 behavior; adds a remote open+stat to every open).

## A copy of a file does not match the origin (size or checksum)

A copy that uCache recognises is read straight from the origin and is the
origin's bytes: `xrdcp`, `xrdfs`, `xrdadler32`, `edmCopyUtil`, XRootD's copy
engine from any program (Python's `CopyProcess`, `gfal-copy`, `rucio
download`), ROOT's `TFile::Cp`, `hadd`, ROOT's command-line tools (`rootcp`,
`rootmv`, `rooteventselector`, `rootslimtree`, `rootls`, `rootprint`, ...) and
ROOT's file merger (`TFileMerger`, given the file's name). `ucache stats`
counts each such handle in `copier_handles`.

For a file with a replica, a handle whose first request reads the whole file
at once (fsspec's `open().read()`, `cat_file(path, 0, size)`) is a copy too and
gets the origin's bytes. A copy in pieces up to the origin's size (fsspec
reading block by block, a loop sized by the file system's `stat`, and fsspec's
`get`) fails with "uCache: this read would complete a copy of ... copy it with
the cache off (UCACHE_DISABLE=1)" before it completes; `copies_refused` counts
them.

A copy made any other way reads through the cache like an analysis job, and a
file with a replica is then shown in the replica's layout — the same data to
ROOT, but a larger file with different bytes. Read whole (in one request, such
as `cat_file(path, None, None)`, or by a loop sized by the handle's own `stat`)
that is a valid file, not the origin's; in pieces spread over several handles
or processes (parallel segments, workers each copying a part) it comes out
corrupt. The same holds for fast cloning inside a program of your own
(`CloneTree(-1, "fast")` on a tree it opened) and copies through an XRootD proxy
or a FUSE mount with uCache inside it. Make those copies with the cache switched
off, and inspect a file's real sizes and codecs the same way (`TTree::Print`,
`TFile::Map`, the `RNTupleInspector` report what they are shown):

```sh
UCACHE_DISABLE=1 python3 my_copy.py
```

A copy tool that uses XRootD's copy engine and still is not counted in
`copier_handles` is worth reporting, with its name and the client version.

## `doctor` reports a problem

- **`[FAIL] logical_name_prefixes ... is not valid`** — the list in the conf or
  the state file has an entry that does not begin and end with `/`, holds a space
  or a byte that is not printable, or an empty, `.` or `..` part, or is too long
  (USER_GUIDE.md, "Keys by logical name"). Files are keyed by their URL until it
  is fixed: `ucache set logical_name_prefixes '/store/data/ /store/mc/'` (which
  refuses a bad list), or `off`. Every job says it once in its log.

- **`[FAIL] plugin not loadable`** — the `.so` can't be `dlopen`ed. Usually a
  missing/incompatible `libXrdCl` (wrong XRootD version) or a moved install.
  Reinstall against your host's `xrootd-client-devel` (≥ 5.6); the build fails
  the version gate if it's too old.
- **`[WARN] sparse files`** — the cache filesystem doesn't report sparse
  allocation (some network filesystems). uCache still works but uses more space
  than the logical cached bytes; prefer a local ext4/xfs/btrfs `UCACHE_DIR`.
- **`[WARN] advisory locks (flock)`** — cross-process eviction coordination needs
  `flock`. On filesystems without coherent locking (some NFS setups) eviction may
  be less precise; use a local `UCACHE_DIR`.

## Fail-open events

`ucache stats` shows `failopen_events`. A non-zero count means some reads
degraded to uncached pass-through (e.g. transient ENOSPC, an I/O error, a torn
sidecar). **Your results are unaffected** — fail-open never serves wrong bytes;
it just skips caching for the affected reads. A steadily climbing count points at
a sick cache filesystem (full, read-only, failing disk).

## A job stops making progress when the storage server goes away

When the server a job reads from dies and stays down, the XRootD client keeps
trying to reconnect before it reports an error. With its default settings that
takes about 8 minutes (measured: 480 s), and meanwhile reads neither fail nor
progress. With the default settings a job through uCache behaves exactly like
one without it, with caching on or off: the error arrives at the same moment.
(With `adaptive_federation = on` a read that fails at its server is made again
at another site; see below.)

To fail sooner, shorten the client's reconnect policy for the job, for example
`XRD_CONNECTIONWINDOW=15 XRD_CONNECTIONRETRY=2`: the read error then comes after
about 15 seconds. Files already in the cache and inside the freshness window
need no server at all, so a warm pass over them is not affected.

With `adaptive_federation = on`, uCache sets that policy itself where no
`XRD_*` variable or `client.conf` sets it: a connection is tried for 31 s, twice,
so a server that is down is given up after about a minute, and the failure is
remembered for 180 s (`ucache doctor` and the run's `run_settings` show the
values in force).

A server that stays up but stops answering (it keeps the connection and never
replies) is different: a read then fails when its time runs out, with
`operation expired`: the job's own timeout for the read, or the client's request
timeout (`XRD_REQUESTTIMEOUT`, 1800 s by default). Through uCache, with
`adaptive_federation = off`, a read the cache does not hold fails at the same
moment, with the same error, single and vector reads alike: the cache's fetch
from the server is sent with the read's own timeout (the client's request
timeout when the job gives none), and when it runs out of time (or the
client's stream timeout ends it on a connection gone quiet) its read is not
sent to that server again, which would wait as long once more. Single reads of
other threads or handles of the job that need the same bytes (the same span of
the file) wait for that one fetch rather than sending their own. When it runs out
of time at the server their handle reads, a waiting read whose own time had run
out by the moment that failure is seen (its own timeout, or the client's request
timeout, counted in whole seconds from when it was made, as the client counts its
requests' time) gets the same error at that moment, as its own read to that
server would have at that same check of the client's timer; one with time left,
or whose handle has not opened the origin yet, sends its own read with what it has
(at least 1 s), and a server that answers again within it serves it. A fetch that fails otherwise (a lost
connection, a server's error) is followed by the job's own read, sent to the
origin as it is, within the time that read has left (counted against the
client's request timeout as the client holds it at that moment when the job
gives none); a read that was waiting for it makes its own fetch, within its
own time left, or fails with `operation expired` when none is left. An answer
short of the bytes asked for is followed by the job's own read too, with at
least 30 s (half the job's own timeout when that is shorter). (With
`adaptive_federation = on` such a read is made again at another site; see
below.)

## An open fails after a minute (`did not complete within 60 s`)

With `adaptive_federation = on`, every read open of a file uCache serves has a
deadline of 60 s: the job's own open, and the opens uCache makes of the origin
when it needs bytes it does not hold. Without a deadline, an open waits for the
XRootD client's request timeout (30 minutes by default) when a server never
accepts the connection, accepts it and never answers, or is staging the copy
from tape. With one, the open fails after 60 s with the client's `operation expired`
error, and uCache warns
`<file>: the open at <host>:<port> did not complete within 60 s (...)`. When
uCache's own open fails this way, the job sees it as a read error, and the
next read that needs the origin tries again. A shorter open timeout is kept as
it is: one the program gives its open, or the client's `RequestTimeout`
(`XRD_REQUESTTIMEOUT`, or a `client.conf`) when the program gives none.

A server that never accepts the connection (its address drops every packet)
is given up by the client after its connection window times its retries, 31 s
twice with the switch on, which is past the deadline: such an open fails at the
deadline, 60 s, with the same `operation expired` error and the same warning.

Through a redirector, an open that passes the deadline at one site is made
again at another (next entry); the warning is then followed by
`trying another site`.

With the switch off for the file (`adaptive_federation = off`, or with the
default, `auto`, a file whose URL names a server that is not a redirector)
there is no deadline: the job's own open
passes the job's own timeout, or none, exactly as without uCache, and the opens
uCache makes of the origin carry none (they wait for the client's
`RequestTimeout`), so the client's own
recovery (at a redirector, going on to another server after a connection
fails) has all the time it has without uCache.

## Is the switch on for my files? (`adaptive_federation = auto`)

With the default, `auto`, the switch is on for a file whose URL names a
redirector and off for every other (USER_GUIDE.md, "Which files the switch is
on for"). `log = info` (or `XRD_LOGLEVEL=Info`) names what each server said,
once per job: `adaptive_federation = auto: <host:port> reports the role
'manager (login: manager)', a redirector's: the files opened there are read
with the switch on`, or `... not a redirector's ...`, or `... gave no role (no
answer within 5 s) ...`. `ucache stats` counts them (`role_queries`,
`role_redirectors`, `role_unanswered`).

- **A site's own redirector turns it on.** It says `manager` as a federation's
  redirectors do: it too sends opens to several servers. Set
  `adaptive_federation = off` to read every file as uCache 1.4.0 did.
- **An EOS instance leaves it off.** EOS says `manager` at login but `none` as
  its configured role, and uCache takes the configured role.
- **The first open of a job waited about 5 s.** The server the URL names did
  not answer the question in time (it was stopped or overloaded); the switch is off for its files for the rest of the job, and
  later opens there do not wait again. An open the cache answers from a trusted
  entry never asks.
- **A warm job asks no server at its opens.** An open answered from a trusted
  entry takes the verdict this job or an earlier one found (`<dir>/roles`);
  with none, the file is decided at its first read the cache does not hold
  (the log says `decided at its first access to the origin`).
- **Opens through a redirector were refused for a long time after it came
  back.** With `auto` the XRootD client keeps its own connection settings: a
  server that failed is answered with that failure for 30 minutes (after
  trying it for up to 10). `adaptive_federation = on`, or
  `XRD_CONNECTIONWINDOW=31 XRD_CONNECTIONRETRY=2 XRD_STREAMERRORWINDOW=180`,
  gives up sooner and tries again after 3 minutes.
- **`ucache doctor` says adaptive_federation has a value it does not know.**
  The value (a typo, `yes`) is ignored and the jobs run with the layer below,
  or the default `auto`; set `off`, `on` or `auto`.

## One server refuses or fails and stops the job

Reading through a federation (a redirector that picks one of several servers
holding the file), one bad server can be enough to stop a job: it refuses
opens (for example `permission denied`), or serves a damaged copy. Through
uCache this often shows as a **read** error rather than an open error, because
the file was opened from the cache and uCache opened the origin only when it
needed bytes it did not hold; the redirector then picked the bad server for
that open.

Find the server's full host name in the error, in uCache's warning when it is
uCache's own open that failed with `adaptive_federation = on` (`<file>: opening
it at <host>:<port> failed (...)`; with the switch off the warning is `lazy
origin open failed for <URL> (...)`, which names no server), or in the
client's log (`XRD_LOGLEVEL=Debug`, the host the open was redirected to), and
add it to the file's URL as `?tried=<full host name>`
(comma separated for several; `&tried=` if the URL already has a query without
a `tried=`). If the URL already carries
a `tried=`, add the name to that list with a comma instead: XRootD keeps only
the last of several `tried=` parameters, so a second one would undo the first
for the open that goes straight to the origin. The redirector then chooses
another server, and uCache's own origin opens for that file carry the same
list, so they avoid it too (releases up to 1.4.0 dropped `tried=` from those
opens). If every copy is on the servers you name, the redirector answers that
no server has the file.

With `adaptive_federation = on`, uCache does this itself for each open (next
entry).

## uCache opened the file at another site

With `adaptive_federation = on`, an open through a redirector that fails at
one site (the server refused this client, stalled past the open deadline, could
not be reached, or failed with a server or storage error) is made again through
the redirector, with the servers that failed named in `tried=`. The log shows
each step:

- `<file>: <host>:<port> (<site>) could not open it (<error>); trying another site`
- `<file>: opened at <host>:<port> (<site>) after <n> failed attempt(s)`, when
  another site opened it;
- `<file>: no site could open it: <site> (<host>:<port>): <error>; ...`, when
  none did. The job gets that error, with the code of the last one.

An open stops after 4 sites or after 180 s in all (160 s with the switch's
error window, less with a shorter open timeout of the job's own). A server the
redirector names by its address, or by a name without a dot, cannot be named
in `tried=` (a redirector masks servers by full host name), so the redirector
may send the next attempt to it again; uCache then asks the regional
redirector for the other copies of the file and opens one directly (the
per-file record file has route `candidate` for these attempts). It does the
same after a server that stalled past the open deadline, or whose connection
timed out (the client's stream timeout), was lost or could not be made, so
that the next attempt does not meet the same site again (the other server of
the site, which stalls too); the copies at that server's site are tried
last: a site is the name its servers give for it (`all.sitename`), else their
domain (USER_GUIDE.md). This holds for a failure of the checks' reads at open as well. A copy listed
as on tape is skipped, with the log line `<file>: the copy at <host>:<port> is
on tape (offline); not opened`. A copy of a file named `*.root` that holds no
bytes is a bad copy: the open is made again at another site, with the log line
`... could not open it (size 0: the copy is empty, and a ROOT file never is); trying
another site`, and the empty copy is counted in `origin_failures_data` and recorded
as an `origin_failure` line of op `open` (FORMAT.md). So is a copy of a file named
`*.root` (`.root` in upper or lower case) that the checks made at open find is not a whole
ROOT file. Inside the job's open uCache reads the first 300 bytes of the copy (and, when
long names place the top directory's record past them, 60 bytes more), then the file's
list of top-level objects (the first read within what is left of the attempt's time, at
least 5 s; each later one within what is left of that, or at least 5 s and 1 s for each MB
it reads), and refuses:

- a copy whose size is smaller than the end its own header states (`size 10161194: its
  header says the file is 16935324 bytes`);
- one that does not begin with `root` (`size 16935324: it does not begin with "root", as
  every ROOT file does`, a copy of zeros among them), or too short to hold the header
  (`size 14: too short to hold a ROOT file's header`);
- one whose header contradicts itself where ROOT refuses a file for it (`its header says
  its first record begins at 1756918, past the end of its data at 1756917`, `... begins at
  -1, before the start of the file`, `its header gives the top directory's name record 20000
  bytes; ROOT reads 10 to 10000`, `its top directory's record ends at ..., past the end of
  its data at ...`, the same of the directory's own record);
- one whose top directory's record places the list of top-level objects, or whose header
  places the class descriptions, outside the copy (`its list of top-level objects lies at
  1757917, 159 bytes, outside the file's records (100 to 1756917)`, `its class descriptions
  lie at ..., ... bytes, outside the file's records (100 to ...)`), the last number being the
  copy's size (class descriptions placed at or before the first record are not looked at, as
  ROOT does not read them; nor is the list of free space, which ROOT reads only for a file
  opened to be written);
- one whose list of top-level objects is damaged: zeroed or overwritten (`its list of
  top-level objects at 1750173 is labelled as lying at 0`, `... does not hold its own
  label`, `... is labelled with the directory at 0, not the top directory at 100`), or
  naming an object outside the copy, in another directory, or at a position below 64
  (ROOT opens such a copy with an error and reads only the objects listed before the
  damaged entry; an object in another directory ROOT does not hold against the copy
  at all) (`its list of top-level objects names an object at ..., ... bytes,
  outside the file's records (100 to ...)`, the last
  number being the copy's size; `... in the directory at ..., not the top directory at
  100`), or ending inside its own entries;
- one whose tree description, RNTuple, other listed object or class descriptions were zeroed
  or overwritten while the list survived, read in one vector read of the labels of at most 64
  of those records (`the label at 1749103 (TTree t) states position 0, and no block follows
  it`, `the label at 1750332 (class descriptions) states position ... and class "..."`): only
  where ROOT cannot read the record from those bytes either; a label whose own fields alone
  were damaged, and an object stored uncompressed, are not held against the copy;
- one whose server answers any of those reads short (`size 16935324: a read of its first
  300 bytes answered 120`, `size 1756917: a read of 159 bytes at 1750173 answered 158`).

A read of them that fails or runs out of time is a failure of the copy, as an open's would
be, and another copy is tried; the client's own recovery of those reads and its following
of a redirect are off for them, so no other copy answers them. Where no other copy is left to
try (or the URL names a server directly, or it is the last open of the job's own URL after a
copy on tape was passed over), the open is made once more at the same place when the client
would have recovered the read (a lost connection or session): two open records at that
server, the first an error of class `client`. A copy whose size is larger
than the end its header states stands, and confirms nothing: ROOT reads such a file
normally (a file updated in place, or with bytes appended, or saved with `TFile::Save`,
which writes the list of top-level objects and not the header, and then written on; its
list and newest objects may lie past that end, inside the copy). So does a copy whose header
places no list of top-level objects (its writer never saved the file): ROOT recovers what
it can of it, and every copy is the same. A copy that passes the checks and whose size
equals that end confirms the file's size, which the cache entry keeps for its freshness window (`revalidate_seconds`):
within it no copy of another size is taken for the entry, and a job's open that the entry
answers (no open at the origin) takes the confirmed size from it and reads no header. The
first open after the window checks the origin and reads the header again; a copy that passes
at another size replaces the entry. The job is never shown a refused copy or
its size. When no
other copy is left (every copy is refused, or the URL names a server directly), the job's open fails with one
error that names each copy tried, its server and its site, its size and why it was
refused (`no site could open it: <site> (<server>): size 0: the copy is empty, ...`),
of class `data`; the copy that was opened is closed and nothing of it is cached.
When the redirector sends a later attempt of the same open back to a copy it refused
already, that copy is refused again as it is (`refused already at this open; it is not taken
again`), with nothing read of it, and it is neither recorded nor counted a second time: the
error names each copy once, and `origin_failures_data` counts each refused copy once.
The counters `open_failovers`, `opens_recovered`, `opens_failed_everywhere`,
`failover_locates` and `offline_copies_skipped` (STATS.md) count these, and the
per-file record file has a line per attempt (FORMAT.md, routes `failover` and
`candidate`).

## The open fails, and the error says uCache holds an earlier copy

With `adaptive_federation = on`, once the freshness window of the cache's entry for a file
has passed (`revalidate_seconds`, 7 days by default), the next open checks the file at an
origin. When no copy passes (each is refused by the checks made at open, or none can be
reached), the open fails with the one error that names every copy tried, and uCache does
not serve the entry it holds on its own judgment: it cannot tell a bad copy from a file
that changed. When it holds one, the error ends with, for example:

```
uCache holds an earlier copy of this file, last confirmed at an origin 8 days ago, holding
the whole file (1.8 MB): a job run with a freshness window longer than that
(UCACHE_REVALIDATE_S=864000, or revalidate_seconds in ucache.conf) reads it from the cache
without asking an origin
```

The age is the time since an open last validated the entry at an origin; the window
suggested is that age and a day, rounded up to whole days. The copies named come first: a
site that is down may come back, and a copy refused for what it is is worth reporting to its
site. To go on meanwhile with what uCache holds, run the job with the window suggested
(`UCACHE_REVALIDATE_S=864000 ./my_job`, or `revalidate_seconds` in ucache.conf): its opens
of files whose entries were confirmed within that window are answered from the cache, with
no request to any origin. That window applies to every file the job opens, not only this
one, so a file rewritten at its origin within it is read as cached: set it for that job
only. What the entry holds counts the bytes of the file the cache keeps as they are and, for
a file with a replica (`recompress`), the parts of the file the replica holds converted
(read without waiting: while another job is writing to the replica at that moment, its parts
are left out of the count). When the entry holds only part of the file, the error says how much (`holding 131.1 kB
of its 1.8 MB (7%)`) and that reads of the parts it does not hold will still fail: they need
an origin, and no copy passes. When the copies such a read then finds are of another size
than the entry's, the read fails with the error that names them and the entry's freshness
marker is removed, so later opens check an origin again (and the advice is not given again
for that entry). The entry itself is kept: this failure does not drop it, an open that later
finds a copy that passes validates it again (a copy that passes at another size replaces
it), and eviction removes it as any other. An entry made only by jobs run with
`revalidate_seconds = 0` has no record of when it was confirmed (no marker is written), and
the error gives no advice for it.

## A server or a copy is tried last (`ucache sources`)

With `adaptive_federation = on`, the log says when a server is marked or a
copy is proven missing (a server's fault of its own is marked from a read, a
listed copy's stat, or an open made at that server; an open made through a
redirector does not see it: the XRootD client itself makes such an open again
at the redirector, which sends it elsewhere):

- `<file>: the server <host:port> (<site>) stalled while other sites answered:
  its copies are tried after the others for <n> min (failure <k> in a row)`, or
  `... answered with a fault of its own: ...`;
- `<file>: the server <host:port> (<site>) answered: its mark is cleared`;
- `<file>: the copy at <host:port> (<site>) is missing though the regional
  redirector lists it: it is tried after the others for 7 days`.

`ucache sources` lists the servers marked (their site, `marked <n>m` or
`ended`, the failures in a row, the last one's age), the sites marked (two or
more servers), and the copies proven missing (by in-use record). A mark only
puts copies last in the regional redirector's list: a job is never refused a
copy for it. To forget every server mark, remove `<dir>/sources`; a copy's mark
goes with its file's in-use record (`<dir>/inuse/...`, FORMAT.md).
A mark is written off the job's threads; a process that ends while one is being
written waits for it, at most a second, so that it is kept and counted. A write
that cannot take `<dir>/sources.lock` within about a tenth of a second (another
process holds it, stopped) is dropped: a lost mark costs one more failover.

## uCache read the file at another site

With `adaptive_federation = on`, a read that fails at the server a file was
opened at, in a way another site may not share (the server stopped answering,
the connection was lost, the request ran past its time limit of 5 minutes, or
the server reported a fault of its own or of its storage), is made again at
another site, and the job gets the bytes. A file shown in a layout of uCache's own
(`recompress = on`) moves too: its request is served again from the start at the other
copy, its fetches made at the origin's offsets, and the job's addresses stay as they were.
The log says so, once for every move:

- `<file>: reads from <host>:<port> stopped (<error>); reading it from
  <host>:<port> from now on`, when another site served it;
- `<file>: reads from <host>:<port> stopped (<error>) and no other site could
  serve it`, when none did: the job then gets one error, with the code of the
  read that failed, that names each copy tried, its size where it is known and
  what it said (`uCache: <file>: no copy could serve the read: <site>
  (<host>:<port>, <size> bytes): <error>; ...`). This holds whatever made the read
  move: a server's failure, a short answer (below), a basket that failed the
  check of `check_origin_integrity` (`failed its check (key position differs from the
  tree's record)`, `(key length differs from the tree's record)`, or, for a basket whose
  compressed data fails its own checksum, `(its data fails its own check: lzma block failed
  its check or did not decode)`, with `zlib` or `lz4` as the codec may be), or a read of the
  parts of a tree whose record showed the copy bad (`a read at ... failed its check (the
  tree's record fails its own check)`, `(the tree's record places a basket at ... of ... bytes
  past the copy's end at ...)`; a read of another tree of that copy is served). A read that
  fails within a second
  after such a move gets that move's error at once, without another attempt, saying so
  (`no copy could serve the read: a move made <time> ago for an earlier read of the
  handle found none, ...`: the offsets in the list that follows are that earlier
  read's); the next one after that tries again.

A read moves only to a copy of the size the job was shown at its open, also on a
file whose reads uCache passes straight to the origin (its host is left out by the
cache's allow and deny lists). For a file uCache caches, the lists also narrow the copies a
read moves to, as they do an open's: the copies taken from the regional redirector's list.
Where a redirector itself sends the read's open is not filtered: the server it names is
taken whatever the lists say of its host, and when it sends the open back to a server that
failed and the lists refuse every other copy the regional redirector lists, that server is
taken again. For a file whose reads uCache passes straight to the origin
(its host left out by the lists) they do not: its open's failover and its reads' moves may
take any copy the regional redirector lists.
A file whose size uCache does not know is not moved: the client's own recovery of its
reads stays on.

uCache's own reads of the file's records, made to know where each basket is for
`check_origin_integrity` (and to find a direct file's baskets under `max_read_fraction`),
move as a read does: one that stalls, whose connection is lost, that the server fails with an
error of its own, or that a copy answers short inside the file's confirmed size, is a failure
of that copy; the file is opened at another site by the same rules, and the records are read
there, on the job's own thread, so the reads that wait for them are checked once they are
read. So do the reads with which a file shown in uCache's own layout (`recompress = on`) is set
up at its first use: the layout is then made from the new copy. A failure of uCache's own (not
the copy's) does not move. The time those reads take, and a move of them, counts against the
request that made them, a vector read or a read of uCache's own layout as a plain read: its
fetch from the origin is then given what is left of its time.

A request the job makes from inside its own completion handler (a read issued from the handler
of another), before the file is set up or its records read, is not served on that thread: the
thread is the XRootD client's, which must never wait for a move (the move's own open is answered
on such a thread), so the request is made again on a thread of uCache's own, where the records are
read and a move waited for (`requests_parked_for_parse`, STATS.md). A job that makes a
synchronous call from inside a completion handler holds a client thread itself; with
`XRD_WORKERTHREADS=1` nothing, uCache's move included, can be answered until that call times out.

What does not move: the reads of the records made on uCache's own threads that serve hits and
read ahead (a request issued again there after read-ahead waited), and those a file in uCache's
own layout makes for `max_read_fraction`; neither changes the bytes the job gets (the second only
whether the file is cached or read directly), and such a thread never waits for a move. While
the first request that decides `max_read_fraction` reads the records (and waits for a move of
those reads), the file's other first requests wait for its decision, as they did for the reads. On a file whose reads
move, the client's own recovery is off, so a connection lost during those reads fails them: the
file's reads are then served unchecked, and nothing of them kept, until a read a second later
or more reads the records again.

A file shown in uCache's own layout (`recompress = on`) bounds its fetches by the read
deadline (5 minutes, within the client's `RequestTimeout`) and by the job's own timeout of the
read when that is shorter, as a plain read is: a fetch that ran out of time at its server is
the job's answer at once, and one that failed otherwise is made again only with what is left of
the read's time.

On a file shown in uCache's own layout, these requests do not move, and with the client's own
recovery off on such a file a connection lost under them fails them: a `Stat` the job forces
(`force = true`), read-ahead fetches (`prefetch = on`), and the reads of the records made for
`max_read_fraction` on uCache's own threads. The job's reads move.

A job's open that the cache entry answered within its freshness window opens the file at an
origin only when a read needs it. When the entry is gone by the job's first read (evicted
meanwhile), that read opens the file at an origin, with the failover of an open. If that open
fails at every copy, each read and `Stat` of the file gets that open's error, which names the
copies (`no copy could serve the read: ...`), and nothing is sent to an origin on the file
until an open succeeds: the first read after a second opens it again. With
the switch off for the file (`off`, or a file `auto` leaves off) the same holds, with no other copy tried: each read
and `Stat` gets the error the origin gave that open (for example `permission denied`), the
one a job without uCache would have got at its open. (In uCache 1.4.0 and earlier, such
reads got the XRootD client's `[ERROR] Invalid operation`, and a page read there could
crash the client.)

A read that a copy answers short, wholly inside the file's confirmed size (confirmed by
the header at this open, or kept by the cache entry within its freshness window for an
open the entry answered: see "uCache opened the file at another site" above), is such a
failure too: the log
says `<file>: <host>:<port> answered <n> of the <m> bytes asked at <offset>, inside the
file's size of <size> bytes; not served`, the line in the record file is of class `data`
(code 14), and the read is made again at a copy of that size. So is a page read (`PgRead`,
which uCache passes straight to the origin) that the copy answers with no bytes there: the
XRootD client fails such an answer itself (`[ERROR] Invalid response`, "user supplied buffer
is too small for the received data", and its File is unusable after it); uCache logs
`answered a page read of <m> bytes at <offset> with no bytes, inside the file's size of
<size> bytes` and moves the read in full. When none is left the job
gets one error naming each copy tried and what it said. A copy whose read answered with
bad bytes (such a short answer, or a basket that failed the key-position check of
`check_origin_integrity`) is never taken again for that file handle: the regional
redirector's list of copies no longer offers it, and an open the redirector sends back
to it is refused. A read that reaches past the end
of the file, and any short answer of a file whose size was not confirmed (a file not named
`*.root`, or one whose header made no claim), is answered as it would be without uCache.
The per-file record file has the `origin_failure` line of each read that
failed, then a line per open attempt with route `read` (FORMAT.md). The
counters of an open at another site (`open_failovers`, `opens_recovered`,
`opens_failed_everywhere`) count these attempts, and `origin_failures_*` the
failed reads. A read stops after 4 attempts or the time it was allowed. When each
move found a copy and the read failed there too until then, the job gets the same
kind of error, with the code of the read's last failure, naming each copy the read
was made at, its size and what it said (`uCache: <file>: no copy could serve the read:
...`), and the log says `<file>: a read moved <n> time(s) and its bounds are spent:
<copies>`. A copy that answered with bad bytes is marked so also when its read
fails while a move of the handle is under way, or after the handle has left that
copy. A
server that stalls is found by silence: the client's own check of a quiet
connection (`StreamTimeout`, 60 s, and its `TimeoutResolution`, 15 s, by
default) ends every read of that connection at once with an error, and each is
made again at another site. The 5-minute limit is only a backstop, for a server
that answers other requests on the connection and never this one. A job that
sets `StreamTimeout` close to or above 300 s sees each of its files move after
its own 300 s.

The 300 s covers a whole call, a read split into several requests included, so
a read of a large range that the connection cannot deliver in 300 s fails at the
limit, and is made again at another site with the same result, though the
server works. A job whose own timeout for a read is 30 s or less (uproot passes
30 s) is not moved when a server stalls: its timeout ends the read before the
time a move needs is left, and it gets that timeout's error, as it would
without uCache. A file opened through an EOS front end moves through that front
end, which names the next server: no other site is asked.

A session the client lost while a file was idle (a server restarted) moves the
file at its next read without naming the server it left. A file with the client's
read recovery turned off by uCache fails a forced `Stat`, `Fcntl` or `Visa` on
such a lost session at once (the next read moves the file); a plain `Stat` is
answered from what the open returned.

## A cache entry was dropped (`bad_entries_dropped`)

uCache drops a file's cache entry when what it holds came from a bad copy, and the next open
of the file fills a new one. The first case below needs `adaptive_federation = on`; the second
needs `check_origin_integrity = on`, which the switch forces on. The log says why, once per
entry:

- `<file>: the cache entry holds <M> bytes and the header it holds says the file is <N>
  bytes: it was filled from a cut copy, and is dropped; the file is opened at an origin`:
  an entry made before the checks at open existed (by uCache 1.4.0 or earlier, or with the
  switch off) from a copy that holds only the first M bytes of the file. Such an entry
  answered every read past M with nothing; the open now goes to an origin and takes a whole
  copy (see "uCache opened the file at another site" above).
- `<file>: the cache entry holds bytes from <host>:<port>, whose copy failed a check: the
  entry is dropped, and the file is read from the origin until it is opened again`: a read
  found that copy bad (a basket that failed the key-position check or its own checksum, or
  a short answer inside the file's confirmed size; see "uCache read the file at another
  site" above), the file map read from that copy showed one of its trees damaged (dropped
  as soon as the map is read, whatever the job reads next), or an open refused it for what
  it is (empty, cut short of its header's end,
  or its header or list of top-level objects not those of a whole ROOT file), after the
  entry had taken bytes from it. Those bytes may be damaged in ways
  no check sees (inside a ZSTD or uncompressed block), so none of them are kept. Until the
  job opens the file again, its reads of it go to the origin. With `recompress = on` the same
  holds for the records of uCache's own layout: a request whose fetch failed at its copy keeps
  nothing it converted from it, and once the entry is dropped every record the process holds of
  the file is forgotten and fetched again. A read answered before the copy was found bad (a
  request of its own, all of whose bytes passed their checks or carry none) got that copy's
  bytes; so does a request already under way at that moment. For a handle opened at that copy
  this is what a read without uCache gets. Another handle of the same process, opened at a good
  copy, can also be served that copy's bytes from the cache until the entry is dropped, which
  without uCache it would not be.

`bad_entries_dropped` (STATS.md) counts both. Only the process that found the copy bad
knows it: another job that has the file open at that moment goes on reading what its view
of the entry holds until it closes the file, and an entry filled from the same copy by an
earlier job is dropped only when a read finds the copy bad again. The files of a dropped entry
are removed at once, or when the job ends; a job killed before that (or ending with `_exit`,
as a worker of a Python process pool does) can leave them, and the next job reads them. To be
sure, remove the file's entry (`ucache rm <url>`) or clear the cache.

## The cache is filling my disk

By default uCache uses the disk and evicts LRU to keep a free-space floor
(`min(50 GiB, 10% of the volume)` free). If that's too aggressive for your disk:

- Set a hard cap: `UCACHE_MAX_BYTES=50g` (or `max_bytes = 50g` in the conf).
- Raise the floor: `UCACHE_MIN_FREE_BYTES=100g`.
- Check current usage/budget: `ucache status`; force a pass: `ucache evict`.

Note the byte cap is best-effort under extreme concurrent write rates; the
free-disk floor is the hard guard against actually filling the volume.

## Jobs are killed for memory with `recompress = on`

With recompression on, the conversion uses one thread per CPU the process may use,
and each thread that converts reserves a block of address space of its own for
its allocations (the C library's arena, 64 MiB). Under a limit on the job's
address space (`ulimit -v`, or a batch system's virtual-memory limit) a reader on
a machine with many CPUs can reach that limit: the job then ends with
`std::system_error` ("Resource temporarily unavailable") or fails to allocate,
though it holds little memory. Raise the limit, run the job on fewer CPUs (the pool
follows the CPUs the process may use, as `taskset` or a batch slot sets them), or
turn recompression off for such jobs. A stored page of a replica that uCache cannot
decode for want of memory is kept, and the read that needs it is made again; one
that still cannot have it fails with
`[ERROR] OS Error: cannot allocate memory; uCache: a stored page could not be
decoded for want of memory (this process's, not the copy's)`, never as damaged data.

With recompression on, a job holds more memory while it reads a recompressed
file: ROOT sizes its read buffers from the layout the file is shown in and does
not shrink them to fit, and uCache keeps a table for each such file while it is
open (a few MB for a large NanoAOD file). On the analyses measured, warm passes
needed about 1.4x (TTree) and 1.3x (RNTuple) the memory of the same job with no
cache, whether the file was recompressed as it was read or by
`ucache recompress`. A batch system or the kernel then stops the job without a
word from uCache (`ucache doctor` says so while recompression is on).

- Use the byte cache alone for these jobs: turn recompression off
  (`UCACHE_RECOMPRESS=off`, or `ucache set recompress off`) and do not run
  `ucache recompress`; the byte cache needs about what a job needs with no
  cache. Files that already have a replica keep it whatever the setting:
  `UCACHE_TRANSPOSE=0` serves every file as stored meanwhile, fetching again
  what was converted, and `ucache untranspose <url>` removes a file's replica
  while no job is reading it.

## `recompress = on` but no replicas ever appear

With `recompress = on`, a file your job reads gets its replica while it is read
(`ucache ls` shows its size under RECOMP as soon as records are committed:
every few seconds, and when the job closes the file). If a file stays at 0,
the plugin says why: the first such file of each process gets a warning,

```
[ucache WARN] slot run declined for root://…/f.root: no relocatable branch: its baskets are zstd, which recompress_codecs (lzma,zlib) does not name; the file is served as stored (`ucache stats` counts such files as cold_replica_skipped; later ones are logged at INFO)
```

every later one an INFO line (`UCACHE_LOG=info`). `ucache stats` counts them
on its `not converted` line, and `ucache status` on its `declined` line. The
reasons:

- **its baskets (pages) are in a codec `recompress_codecs` does not name** —
  see cause 1 below. The file is decided again at the next read once the list
  names that codec.
- **its baskets are stored uncompressed**, or **compressed in a codec that is
  not converted** (the old ROOT algorithm): there is nothing to gain, and a
  sweep declines such a file as well.
- anything else is a file structure the layout cannot hold, which the warning
  names: the file is served from the byte cache, and `ucache recompress`
  declines it as well.

A file whose compression setting names no codec — setting 1, "the global
default", which files from older ROOT versions and `hadd -f1` carry — is
judged by the codec its baskets are actually stored in, read from one basket.
`ucache recompress` takes that basket from what your jobs cached: the first
basket of any of those branches will do. The exception is a file whose replica
readers were shown within `in_use_seconds`: then it asks for the same basket the
replica was laid out by, and the file stays `incomplete` until that basket is
cached.

Data cached before recompression was switched on and not read since gets a
replica only from `ucache recompress`. The rest of this section covers these
causes.

**Ask the tools first — they diagnose this for you:**

```sh
ucache doctor      # names the cause, and exits nonzero on it
ucache status      # the reason, cheaply
```

`doctor` reports `recompress = on but no replicas exist` together with the
specific cause, when it can identify one; a cache whose data simply predates the
switch has no replica yet and nothing wrong with it. The causes, in order of
likelihood:

1. **The source codec is not in `recompress_codecs`.** The default is
   `lzma,zlib`, the two codecs that are expensive to decode. Data already
   stored in ZSTD or LZ4 is declined in full, because transcoding it to ZSTD-1
   would spend disk to buy nothing. A sweep says so in as many words, naming
   the codec it actually found and the fix:

   ```
   recompress: 0 recompressed, 9 declined (source codec zstd, not in recompress_codecs = lzma,zlib), 0 nothing to do (nothing cached), 0 failed
     nothing was recompressed: this cache holds zstd content, but recompress_codecs = lzma,zlib.
     to transcode it:  ucache set recompress_codecs zstd
   ```

   `ucache branches <url>` shows the codec per branch if you want to look first.

2. **`transpose = off`.** A replica is created on the first pass only where
   replicas are served, so with the replica tier switched off a job's first pass
   neither creates nor serves one. `ucache set transpose on` (or
   `ucache unset transpose`) lets recompression work again.

3. **There is no headroom above the eviction floor.** Converted records that
   still do not fit above the floor after an eviction pass are not kept
   (`ucache stats` counts them as `cold_replica_declined`), and a sweep defers
   each file that would not fit rather than pushing your cache into eviction,
   saying so in its summary. Free some space. See "The cache is filling my
   disk".

4. **Nothing qualifies yet.** Only what your jobs actually read is cached, and
   so converted by a sweep. A file whose metadata is not cached yet (a job
   opened it and read little else) is reported as `incomplete` and retried on a
   later pass.

If replicas exist but you see no speedup, coverage is probably partial — see
the recompression section of the User Guide: unreplicated files pace the loop,
so finish with an explicit `ucache recompress`.

**Files an older uCache left in the cache directory.** `recompress.pending`,
`recompress.working` and `recompress.log` belonged to the background worker that
uCache 1.2.0 and earlier ran; nothing reads them now. `ucache recompress` removes
the first two (it scans the whole cache, so it covers every file they listed);
`recompress.log` is safe to delete.

## `failed` vs `incomplete` vs `deferred` in `ucache recompress` output

Only `failed` means something is wrong. The others are ordinary states with
their own words, because conflating them made healthy runs look broken:

- **`incomplete (bytes not cached)`** — what the file's layout is computed from
  (its metadata, and for a file whose compression setting names no codec, a
  basket that names it) is not in the byte cache yet. It retries for free on
  the next pass and needs no action. The message reads `not built yet, will
  retry`.
- **`deferred (no space)`** — no headroom above the eviction floor; the file is
  left for a later `ucache recompress` once there is room. See cause 3 above.
- **`declined (source codec …)`** — working as configured; see cause 1 above.
- **`declined (layout, said above)`** — the file's own structure cannot be
  converted (32-bit keys that cannot point past 2 GiB, an RNTuple anchor stored
  compressed, and the like); a line above names the file and the reason. It
  stays in the byte cache and reads correctly, and a first pass declines it the
  same way.
- **`failed`** — a real failure: cached bytes that no longer match their
  checksum (the message points at `ucache verify`; the rest of the file is
  converted, those bytes are not), a page that does not decode, or an entry that
  has gone missing. A failure cannot produce wrong data: what does not verify is
  never converted, and every record is checked against its checksum when it is
  served. Confirm serving is clean with
  `ucache stats` — `crc_failures 0`, `failopen_events 0`, `validations_failed 0`.
  If failures persist for the same file and coverage never completes, report it
  with the branch name from the sweep's output.

## `a read ... in the range of a map that is no longer valid ... fails`

With `recompress = on`, a TTree or RNTuple file read again is shown a *map*
that states its converted baskets or pages in an address range of its own. A map replaced by a
newer one stays readable while any process's handle holds it (an open handle
keeps it, however long it is idle) and for `in_use_seconds` (default 1 day)
after it was last used. A reader that comes back later with positions it
learned from it -- kept after every handle that held the map was closed, or
handed to another process -- gets this error instead of another basket's
bytes. Open the file again: the new handle is shown the current map. If your
jobs reuse positions longer than a day after closing the file, raise
`in_use_seconds`; `ucache stats` counts these reads as `slot_map_refused`.
The same holds when a file's replica is removed (`ucache rm`,
`ucache untranspose`) and made again while a reader still holds positions in
it: the new replica's maps keep clear of the old ones' places while those are
in use, and a read there fails (`... in the tree record of a map of a store
that was replaced`).

## Corruption / CRC

Every cached page carries a CRC32C, verified on read; a mismatch quarantines that
page (it's re-fetched next time) and increments `crc_failures` — wrong bytes are
never served. To scrub an entry on demand: `ucache verify <url>` (it reports
`checked`/`bad`; bad pages are quarantined, not the whole entry). To drop and
re-fetch an entry entirely, remove it and let the next read repopulate.

Many `CRC mismatch on page N ... marked absent` lines from a job that reads each
file with several threads at once (say 384 threads over 100 files) came from
releases 1.1.1 to 1.3.2: two threads saving a file's page index at the same
moment could store a wrong checksum over correct bytes. Results were not
affected; each page was fetched again. Later releases do not do this, and they
treat such a page as absent when they open the entry, so the cache heals
without `ucache clear`.

## Replicas keep disappearing when an older uCache shares the cache directory

This release does not serve the compact replicas earlier releases made beside
the byte cache (`<hash>.tdata`, `.tmeta`): it removes each one at the file's
next open, in `ucache recompress` and in the eviction pass, and recompresses
into the file's slot store instead. An older release reading the same cache
directory keeps building them again, and this one keeps removing them, so the
transcoding is paid for over and over.

Point one version at a cache directory at a time. A cache directory is
version-coupled state, not a shared scratch area; separate directories cost
only disk. `ucache status` (`earlier` line) and `ucache doctor` say how many
such replicas are left.

## A file is cached twice under two names

Two jobs whose confs set `logical_name_prefixes` differently (one with, one
without) each key the file their way; a file the other job cached first is cached
again. Set the prefixes for the cache with `ucache set logical_name_prefixes ...`
(`ucache doctor` notes a value that comes from a conf). Two URLs of one file cached
as two entries before the prefixes were set are one entry from then on: the second
URL's old entry ages out (USER_GUIDE.md, "Keys by logical name").

## A job was served another file's bytes under a `/store/` path

With `logical_name_prefixes = /store/data/ /store/mc/ /store/hidata/`, any path
that contains `/store/data/`, `/store/mc/` or `/store/hidata/` is keyed by the name
from there on, a user's copy that keeps CMS's layout included (`/eos/user/x/me/store/mc/...`):
it and the official file are one entry, and inside the freshness window either is
served the bytes of whichever was cached first, when the two are the same size (a
copy of another size is cached apart, under its own URL: `name_size_mismatches`). Set `logical_name_prefixes = off`
for such a cache (or a list that none of its users' paths holds), and `ucache rm`
the entry.

## Pinned data got evicted / a pin didn't take

`ucache pin <url>` sets a flag in the entry's sidecar; `ucache ls` shows a `yes`
in the PIN column. If a URL won't pin, it isn't cached yet (pin after a first
read). Pinned entries are skipped by eviction; if the pinned set alone exceeds
the budget, eviction can't shrink below it (that's intended — unpin something).

## Clean slate

`ucache clear` is the supported wipe: it prompts, removes cached data and
settings state, and keeps your conf (`--keep-pinned` spares pinned entries).
Everything uCache stores lives under the configured cache dir (`dir =` /
`UCACHE_DIR`), so removing that directory by hand is equally safe — the next
run repopulates. The only file elsewhere is the plugin conf itself
(`~/.xrootd/client.plugins.d/ucache.conf`, or under `~/.config/ucache/plugins/`
if you use the `XRD_PLUGINCONFDIR` route). `ucache disable` turns caching off
without removing anything; deleting the conf removes all trace.
