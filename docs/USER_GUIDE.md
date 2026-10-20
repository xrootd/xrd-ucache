# uCache — User Guide

`ucache` is a transparent, per-user read cache for remote ROOT/XRootD data. You
install it once; thereafter every `root://…` file your jobs read is cached on
local disk, so the **second and later** passes over the same data read from your
NVMe instead of the network. Nothing in your analysis code changes.

It is safe by construction: if anything goes wrong (bad cache dir, disk full,
version mismatch, corruption) it **fails open** — your job still runs correctly,
just without the speedup.

## 1. Install (no root — like everything else here)

Unpack the release tarball into your home:

```sh
mkdir -p ~/.local
tar -C ~/.local --strip-components=1 -xf xrd-ucache-<version>-el9-x86_64.tar.gz
ucache doctor      # expect FAILs for cache dir + activation — §2 fixes both;
                   # the `plugin loads` line is what confirms the install
```

That's the whole install: `bin/ucache`, `bin/ucache-netbench`, and the plugin
built twice, `lib64/libXrdClUCache-5.so` and `lib64/libXrdClUCache-6.so`, under
`~/.local`. On EL9 `~/.local/bin` is on `PATH` by default; if `ucache` is
not found, add `export PATH="$HOME/.local/bin:$PATH"` to your shell startup
file (`~/.bashrc`, `~/.zshrc`, …). Any other prefix works too — nothing
cares where the files live. Continue with §2 (activation — one config
file). The tarball and an EL9 RPM are attached to every release on the
[Releases page](https://github.com/xrootd/xrd-ucache/releases).

No XRootD needs to be installed for it: the plugin runs inside a process that
already has an XRootD client (CMSSW, LCG/CVMFS ROOT, EPEL's `xrootd-client`, …)
and uses that one. A compiled plugin serves one XRootD major, so there are two:
`libXrdClUCache-5.so` for XRootD **5.6** and later 5.x clients,
`libXrdClUCache-6.so` for XRootD **6**. Your configuration names
`libXrdClUCache.so`, and each client opens the build for its own major beside
it. The plugin never links ROOT, and where it can't load (CentOS 7-era releases
under apptainer) it fails open — the job just runs uncached.

### Get the source

All three routes below build the same tree, and their `cmake -S .` means this
directory:

```sh
git clone https://github.com/xrootd/xrd-ucache.git
cd xrd-ucache
git checkout v<version>     # optional — a release rather than the tip of main
```

A source tarball from the [Releases
page](https://github.com/xrootd/xrd-ucache/releases) works the same way:
unpack it and `cd` into it instead. Downloading either with `git` or `curl`
matters on macOS — see that section.

### Build from source — AlmaLinux 9 / RHEL 9

Install the toolchain and the XRootD **client** headers. `xrootd*` comes from
EPEL, so enable it first. None of this needs to be the same machine that later
runs your jobs — it just needs the headers to build against.

```sh
sudo dnf -y install epel-release
sudo dnf -y install gcc-c++ cmake make \
     xrootd-client xrootd-client-devel \
     xz-devel zlib-devel libzstd-devel lz4-devel     # codec libs (see note)
```

- `xrootd-client-devel` provides `libXrdCl` + headers (must be ≥ 5.6; EPEL 9
  ships 5.6+). Its headers live under `/usr/include/xrootd`, which is why the
  build needs `-DUCACHE_XROOTD_ROOT=/usr` below.
- The four `*-devel` codec packages let uCache read a ROOT file's own records,
  which the **recompression** feature (`ucache recompress`, see
  §Configuration), read-ahead (`prefetch`), `max_read_fraction` and the checks
  of what the origin sends (`check_origin_integrity`, which
  `failover` turns on for the files it applies to) all need. Omit them and everything else
  still builds — the page cache works fully — but those are skipped (fail-open
  at build time): no replica is made, nothing is read ahead or checked, and
  every file is cached whatever `max_read_fraction` says (`ucache doctor` warns
  when it is `0`). `lz4-devel` is optional even among those (it only lets the
  transposer *read* LZ4-compressed inputs).

Build against the **host** XRootD (point the version pin at `/usr`) and install
to a user prefix — no root needed for the build/install itself:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DUCACHE_BUILD_BENCH=OFF -DUCACHE_BUILD_TESTS=OFF \
      -DUCACHE_XROOTD_ROOT=/usr
cmake --build build -j"$(nproc)"
cmake --install build --prefix ~/.local        # or a system prefix (needs write access)
```

`configure` prints `ucache: XRootD client <version> (>= 5.6 required) — OK`; if
it instead can't find `XrdCl/XrdClPlugInInterface.hh`, `xrootd-client-devel`
isn't installed or `-DUCACHE_XROOTD_ROOT` doesn't point at its prefix. This
installs the plugin as `lib64/libXrdClUCache-<major>.so`, named after the
XRootD major it was built against (`-5` for EPEL's client), and the `ucache`
CLI to `bin/`. Put the prefix's `bin` on your `PATH` if it isn't already. To
build for both majors, as the release packages are, add
`-DUCACHE_XROOTD_EXTRA_ROOT=<prefix>` naming a client install of the other
major.

> **Note:** `-DUCACHE_XROOTD_ROOT=/usr` is required on a stock EL9 box — the
> default points at the CERN CVMFS/LCG build used for development, which a
> normal host does not have. Set it to wherever `xrootd-client-devel` landed
> (`$(dirname "$(dirname "$(command -v xrdcp)")")` resolves it).

### Other distributions

The only distro-specific part is the XRootD client package; the rest is the same
`cmake` invocation with `-DUCACHE_XROOTD_ROOT` pointed at its prefix (usually
`/usr`). On Debian/Ubuntu: `apt-get install g++ cmake make xrootd-dev
liblzma-dev zlib1g-dev libzstd-dev liblz4-dev` (verify `xrootd-dev` ≥ 5.6). Any
Linux with a C++17 compiler, CMake ≥ 3.20, and XRootD client ≥ 5.6 works.

### Build from source — macOS

There is no prebuilt macOS package yet. Tested on Apple silicon, macOS 14, with
MacPorts.

**1. Prerequisites.** Command Line Tools are enough — Xcode is not required.

```sh
xcode-select --install                     # clang, linker, SDK
sudo port install cmake xrootd root6 +xrootd
sudo port install xz zstd lz4              # codec libs — optional, see the note above
```

**2. Check which XRootD client your ROOT loads**, and build against that one —
this matters more on macOS than anywhere else, because a second XRootD
(Homebrew's, or a ROOT framework build) makes the plugin load and never cache:

```sh
root-config --has-xrootd                                       # must say yes
otool -L "$(root-config --libdir)/libNetxNG.so" | grep -i XrdCl
```

The prefix flags below assume that path is under `/opt/local`; point them
elsewhere if it is not.

> **MacPorts installs ROOT's tools with a `6` suffix** — `root6`,
> `root-config6`. Substitute `root-config6` in the two commands above.
> `sudo port select --set root root6` is meant to create the unsuffixed names
> and fails on ROOT 6.40, because its list still names the removed `proofserv`.

> uCache does not need ROOT to build — `root-config` comes with ROOT, and this
> step only identifies the client that will load the plugin at run time. Skip
> it if ROOT is not installed yet or you read with uproot; with only MacPorts'
> `xrootd` present, `/opt/local` is the answer. Run it later against whatever
> ends up loading the plugin, and rebuild if it names a different XrdCl.

**3. Get the source** — the `git clone` under "Get the source" above. `-S .`
below is that directory.

**4. Build and install.**

```sh
export TMPDIR=/tmp          # do this FIRST — see the note below
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DUCACHE_BUILD_BENCH=OFF -DUCACHE_BUILD_TESTS=OFF \
      -DUCACHE_XROOTD_ROOT=/opt/local -DCMAKE_PREFIX_PATH=/opt/local
cmake --build build -j"$(sysctl -n hw.ncpu)"
cmake --install build --prefix ~/.local
```

**5. Check what you built.**

```sh
~/.local/bin/ucache --version
otool -L ~/.local/lib/libXrdClUCache-*.so | grep -i XrdCl   # same path as step 2
```

The **second** line of that output is the one to read — the `libXrdCl` the
plugin will load, which must be the path step 2 printed. The first line is the
library naming itself, and its `compatibility version 0.0.0` is not a version
of anything: the plugin is `dlopen`ed by absolute path, so it carries no
Mach-O dylib version. The version XRootD checks at load is separate, and is
the client release the plugin was compiled against:

```sh
strings ~/.local/lib/libXrdClUCache-*.so | grep '@V:'   # e.g. @V:XrdClUCache v5.9.1
```

It must be the same major as the client that loads the plugin and no newer
than it; building against that same client, as above, satisfies it by
construction. The file is named after that major: `libXrdClUCache-5.so` with
MacPorts' XRootD 5, `libXrdClUCache-6.so` with Homebrew's or conda's XRootD 6.

> **Set `TMPDIR` before building.** With it unset, the compiler is handed a
> per-session `/var/folders/...` path it may not own, and AppleClang then fails to
> compile anything at all with "unable to make temporary file". Any writable
> directory works.

> **If both MacPorts and Homebrew are present**, CMake searches `/opt/homebrew`
> first on Apple silicon. The two prefix flags above are what keep the build on
> the same XrdCl your ROOT loads — confirm with `otool -L` rather than assuming.

> **On quarantine.** Gatekeeper's `com.apple.quarantine` attribute is applied by
> the application that downloads a file — a browser, Mail — and a quarantined
> library cannot be `dlopen`ed, so the XRootD client would silently decline to
> load a plugin obtained that way. `git`, `curl` and `tar` do not set it, so
> fetching the source as above is unaffected.

Everything after this is identical to Linux: activate with the same plugin
configuration file (§2), and point the cache at an ordinary directory on a
volume with room to spare.

### For site administrators (optional)

Each release also ships `xrd-ucache-<version>-1.el9.x86_64.rpm` for a system-wide
install (`dnf install`, lands in `/usr/lib64` + `/usr/bin`). It requires no
XRootD package: the plugin uses whichever client a job runs, from the system
or from CVMFS. Nothing about uCache *needs* this — it exists for admins
who want one shared copy; users still activate per-user (§2). Maintainers:
`scripts/package-el9.sh` builds both artifacts into `dist/` with the host
toolchain (never an LCG shell), so they run on stock EL9 and inside CMSSW/LCG
environments alike.

## 2. Activate (one file, no root)

uCache is activated **and** configured by a single file: an XRootD plugin
config that tells XrdCl to load the plugin and carries all ucache settings —
starting with where the cache lives. Write it yourself, or let `ucache setup`
write the same file; nothing else is touched either way.

### Write the file

XrdCl scans `~/.xrootd/client.plugins.d/` automatically in every `root://`
process — no environment variable, no shell startup file to edit, no shell
to reload, and batch jobs pick it up too. The package ships the recommended
file, which activates nothing until you copy it into your own account (uCache
is a per-user cache; nothing it installs switches it on for anyone else):

```sh
mkdir -p ~/.xrootd/client.plugins.d
cp /usr/share/xrd-ucache/ucache.conf ~/.xrootd/client.plugins.d/   # the RPM
# a tarball or source install: <prefix>/share/xrd-ucache/ucache.conf
# then edit it: set `dir` (required); unless you installed the RPM, also `lib`
ucache doctor      # verify install, filesystem, activation; exits 0 when good
```

The file, without its longer comments:

```ini
url = *
lib = /usr/lib64/libXrdClUCache.so
enable = true

# REQUIRED: the cache directory, on a LOCAL disk (not AFS, NFS or EOS).
dir = /path/on/a/local/disk/ucache

# Recompression is OFF by default: the cache keeps the bytes the origin sent.
# With it on, a ROOT file read for the first time is converted, as it is read,
# into a form that is faster to decode -- no separate step.
#
# MEMORY: a job reading a recompressed file holds more memory while it reads.
# On the analyses measured, warm passes needed about 1.4x (TTree) and 1.3x
# (RNTuple) the memory the same job needs with no cache, however the file was
# recompressed. If your jobs run short of memory, leave it off and do not run
# `ucache recompress`: the byte cache alone needs about what a job needs with
# no cache. Turning it off later does not change files already recompressed:
# remove those first, while no job is reading them (`ucache untranspose <url>`,
# or `ucache clear`; a job still reading one would fail).
# recompress = on
```

- `dir` — where the cached data lives. **Required, on purpose**: there is no
  default location (a default would silently land in your home directory,
  which on CERN machines is AFS — caching network data onto a network
  filesystem defeats the purpose). Use a **local** disk. Until it is set,
  `doctor` FAILs and the plugin runs uncached (fail-open).
- `lib` — the absolute path to wherever §1 put the plugin:
  `/usr/lib64/libXrdClUCache.so` from the RPM (as shipped), or
  `$HOME/.local/lib64/libXrdClUCache.so` for the no-root tarball/source
  install — written out in full: the conf needs a literal absolute path, no
  `~` and no `$HOME`. No file has exactly that name: an XRootD client adds its
  own major and opens `libXrdClUCache-5.so` or `libXrdClUCache-6.so` beside
  it, so one line serves both. Name the plain file, not one of the builds —
  naming one makes every client load it, and a client of the other major then
  refuses it. `ucache doctor` prints the file your client opens on its
  `plugin loads` line, and `ucache setup` (below) fills the path in for you.
- `url = *` intercepts every `root://` URL. If a system config in
  `/etc/xrootd/client.plugins.d/` already claims the `*` slot (`doctor`
  warns), list your data hosts explicitly instead:
  `url = eospublic.cern.ch:1094;xrootd.example.org:1094` (your experiment's
  data servers or redirectors). Add `local` to such a list to cache files of
  mounted or slow file systems too (`root://local`, §Configuration).
- XrdCl resolves the `~` in `~/.xrootd/client.plugins.d` from the passwd
  database, not `$HOME` — they differ on some batch systems.
- Prefer the file elsewhere? XrdCl also processes `$XRD_PLUGINCONFDIR`
  (after `/etc/xrootd/client.plugins.d` and the default dir — last wins for
  the same `url`), so you can keep the conf in any directory and set that
  variable in your shell startup file; that route only works in shells (and
  jobs) where the variable is set.

Deactivation is equally boring: delete the file (or set `enable = false`).

However you configured things — this file, `XRD_PLUGINCONFDIR`, `XRD_PLUGIN`,
environment variables, `setup` — **`ucache doctor` is the one verification
step**: it finds the governing conf exactly as XrdCl would, dlopens the very
file your client would open for the library that conf names, checks the cache
filesystem, and exits nonzero if anything would stop caching (no conf,
`enable = false`, a shadowed `url = *`, a stale `lib =` path, no build for
your client's XRootD major, an unset or unsuitable cache dir).

### Or: no file at all (`XRD_PLUGIN`)

Two environment variables switch uCache on with every default:

```sh
export XRD_PLUGIN=/usr/lib64/libXrdClUCache.so   # the RPM; else <prefix>/lib64/libXrdClUCache.so
export UCACHE_DIR=/path/on/a/local/disk/ucache   # required: there is no default
ucache doctor
```

`XRD_PLUGIN` is XRootD's own variable: every client loads the library it
names, for every URL, and reads no plugin configuration at all. Settings then
come from `UCACHE_*` variables and `ucache set` (§Configuration). Three things
follow from how it works:

- **No plugin config file is read while it is set** — not yours, not the
  system's. Any other client plugin configured on the machine is off in that
  process, and a `ucache.conf`'s settings, `dir` included, do not apply
  (`doctor` says so if you have one).
- **It applies to every URL on every host.** A config file can limit uCache to
  some hosts; this cannot.
- **Every process that reads data needs both variables**, batch jobs
  included: export them in the job script.

To switch it off, unset `XRD_PLUGIN`.

### Or: `ucache setup` (writes the same file)

```sh
ucache setup --host eospublic.cern.ch:1094 --dir /data/$USER/ucache-cache
ucache doctor
```

`setup` writes exactly the file above — the same text, with this install's
plugin path, your `--host` and your `--dir` filled in — to
`~/.xrootd/client.plugins.d/ucache.conf`, and touches nothing
else: no shell startup files, no environment variables, no second config.
`--dir` is required unless `UCACHE_DIR` or an existing conf already provides
a location (there is deliberately no default). Activation is immediate for
every `root://` process, batch jobs included.

## 3. Verify it really works, then run your analysis

**The standard self-test** — point it at any file your conf intercepts
(pick one of your usual data files; the whole file is transferred, so not
your biggest):

```sh
ucache test root://<your-host>//path/to/file.root
```

It uses your setup exactly as configured — no changes, no environment — and
runs a cold pass then a warm pass through a real XRootD client, with
progress shown. It PASSes only if the warm pass is served entirely from
cache with **zero origin contact**, then removes the entry it created (a
file that was already cached is verified warm-only and kept). Exit 0 = the
whole chain works: conf → plugin loaded → interception → caching → warm
serving. It tests the byte cache: its copies run with copy detection off,
because uCache otherwise reads a copy straight from the origin (below) and the
test would see no cache traffic at all, with `max_read_fraction` at 100, since
a copy reads all of a file, and with recompression's layout
switched off, because a whole-file copy of a file in that layout would read
baskets the layout never keeps.

Then run your normal ROOT / RDataFrame / uproot job **twice**:

```sh
python my_analysis.py     # cold: fetches the working set from the origin
python my_analysis.py     # warm: served from local cache
ucache stats              # warm pass shows origin_bytes == 0
```

(Don't smoke-test with plain `xrdcp`: a copy never uses the cache — it is
read straight from the origin, so `stats` shows only `copier_handles` and
direct bytes. Use `ucache test`, or, to push one copy through the byte cache,
`UCACHE_COPY_DETECT=off UCACHE_MAX_READ_FRACTION=100 XRD_CPUSEPGWRTRD=0 xrdcp …`
— the second setting because a copy reads all of a file, which a ROOT file is
otherwise not cached for (see `max_read_fraction`), the third because xrdcp
otherwise transfers with PgRead, which the cache passes through.)

`ucache status` shows where the cache lives, the disk budget, how much is cached,
and aggregate counters.

## Monitoring — start with `ucache summary`

`ucache summary` answers the question you actually have: **is this cache worth
having, and what has it saved me?** It reads records the plugin already wrote —
nothing needs to be running, and there is no sampling daemon to start.

```text
$ ucache summary
overall    : 12 run(s) recorded — 336.8 GB read, 86.3 GB from the origin
  saved    : 10m43s — 4 run(s) took 4m49s, and would have taken about 15m32s with
             no cache (3.2x, vs a measured baseline)
  health   : OK — no faults in any recorded run

by dataset
  3fe77e    439 files      623.9 GB at origin   47 dir(s)  eospublic.cern.ch
           4 run(s), 2 measured (71% of GB)   108.5 GB read   gain  byte 1.96x  repl 5.07x
  34a731    255 files      630.8 GB at origin   10 dir(s)  eospublic.cern.ch
           4 run(s), 2 measured (67% of GB)   105.8 GB read   gain  byte 3.67x  repl 4.56x

cache      : /scratch/ucache — 694 entries, 202.8 GB on disk (61.4 byte + 141.4 replica),
             1.3 TB headroom

notes
  8 of 12 runs not measured:  3 baseline(s) — they are the reference  3 filled the cache  2 under 30s
next       : `ucache summary --detail` for the last run; `ucache history` for the trend
publish    : `ucache publish` — a report with recommendations from this history;
             paths, hostnames and file names never leave the machine (docs/PUBLISH.md)
```

Runs are grouped by the set of files they read, so a cache used for more than
one analysis does not average them into a single meaningless number. The
`notes` block accounts for every run that produced no gain, by reason — a run
that IS the reference, one that filled the cache, one too short to time — so
"not measured" is never left looking like "measured and disappointing".

**The headline is time, and it is measured against a baseline — the same work
run once with the cache out of the loop.** Record one by running your job with
`UCACHE_DISABLE=1` set; the plugin passes everything through untouched and
writes the run down like any other. From then on, cached runs over the same
files are compared against it — and the comparison is symmetric: **a cache that
is slower than your origin shows up as a cost, not a gain**:

```text
overall    : 7 run(s) recorded — 429.4 GiB served from cache, 222.8 GiB from the origin
  COST     : the cache is costing you time on this workload — 4 run(s) took 15m26s
             where no cache would have taken about 11m40s (0.8x, vs a measured baseline)
```

That is a real result from a real workload (a fast LAN origin and cheaply
compressed data): the honest answer there is to not use a cache, and the tool
says so.

### Why a baseline, and not an estimate

Nothing recorded during cached operation can stand in for the origin alone —
this was measured, twice, before being accepted. The cold fill's wall is set
partly by the cache's own writes (on the workload above, using it as the
reference reported a 1.7x *gain* where the measured truth was a 0.70x *loss*),
and service-time histograms record blocked waits, which overlap compute and are
not wall time (same test: 2.8x, same truth). A measured baseline reproduces the
true ratio to about 1% either way. One extra run of your job is what
truthfulness costs.

**Two kinds of run can be the reference.** One with the cache switched off, and
a first pass over data the cache does not have yet — a *fill* — provided the
cache added little to it: the origin has to have delivered at least 90% of the
bytes (80%, when a correction is possible, below), and the cache's own writing
must have cost no more than a tenth of the time spent waiting on the origin.

Even a first pass over an empty cache is served some of its data from it: every
piece of a file a thread opens re-reads the file's header and table of
contents, and after the first time those come from what the pass has just
fetched — 5-10% of the bytes on a multi-threaded job. That makes a fill a
little faster than a run with the cache off, so gains measured against it read
a little low. When a warm run over the same files, served from the byte cache,
exists, the fill's time is corrected for it: a fill that took T with a share f
from the cache, beside a warm run that took T_warm, counts as
(T − f·T_warm) / (1 − f). `ucache summary --detail` says when a reference was
corrected, and by what share. That second figure is a coarse estimate, not
a percentage of your job: it compares time blocked writing against time blocked
fetching, and both are summed across threads, so it can be off by a factor of
two or three in either direction. It is used only to tell a 1-2% effect from a
20%+ one, which is all the admission decision needs. That second kind of
reference means an ordinary first run can supply a
measurement without anyone having known to record one. A switched-off run still
wins over a fill however recent the fill is, because a fill carries
its own writing in its wall.

Rules the comparison enforces, so a number is never printed where it would
mislead:

- **Same files.** The baseline must cover ≥70% of the same files in both
  directions, matched by key: by URL, or, with `logical_name_prefixes` set in
  the settings the command reads, by logical name for a path that has one
  (records written by URL before are read as that name's, so a baseline taken
  before the prefixes were set, or through another site or redirector, still
  matches).
- **Same work.** Covering the same files is not enough — two analyses can read
  the same files and touch quite different parts of them. Each run records
  which 1 MiB blocks of each file it read, in the original file's coordinates,
  and at least **90% of the files compared must agree**. It is a share and not
  a single hash over the whole run because the two routes genuinely differ a
  little: a replica is a rewritten container and the reader asks it for
  different spans, which on a measured file set left 26 files of 439 disagreeing
  on otherwise identical work.
- **Enough evidence to check.** At least half the compared files must carry
  that record on both sides for the comparison to count as VERIFIED. One
  agreeing file cannot vouch for four hundred. Below that line the walls are
  still compared — absence of evidence is not evidence of different work, and
  refusing here made the answer depend on how much evidence happened to exist,
  so that adding one signature to one file could delete a measurement that
  worked with none. What changes is that the result says so:
  `ucache summary --detail` prints `same work: NOT verified` with the count it
  rests on, and both JSON readouts carry `work_verified`.
- **Big enough, long enough, clean.** Runs and baselines under 30 s or 64 MiB
  are refused (too short to time at one-second resolution), and a run that hit
  faults is not used as one.
- A run that mostly *filled* the cache is never given a gain of its own.

Recording a baseline *after* you have been running cached for a while works
fine — either side in time qualifies. When the answer cannot be sound,
`summary` says why, and which of these it was, instead of printing a number.

`ucache summary --detail` adds the last run underneath: tier split, delivered
rate, per-tier read counts and sizes, replica coverage, and that run's own
comparison with its baseline.

## Trend across runs — `ucache history`

One row per run, newest first. This is where you see whether the numbers hold up
over time, across versions, or after you change the cache disk.

```text
$ ucache history --top 8
12 run(s) recorded, newest first (showing 8)
WHEN         DUR     SIG     RSIG    PEAK   RIF  FILES    READ  DIR/FILL/BYTE/REPL   RATE    INS    IPS   CPU   OVH   GAIN
                                      cor  orig             GB  percent              GB/s   1e12    1e9   cor     %      x
ALL 12 runs  22m40s                               1133   336.8   23/  26/  24/  28                                    3.22
-----------  ------  ------  ------  ----  ----  -----  ------  ------------------  -----  -----  -----  ----  ----  -----
08-29 12:52  27s     b95890  9ffb8d    33     -    439    11.1    0/   0/   1/  99  0.412   1.22   45.3  12.3     -      -
08-29 12:50  26s     b95890  3bdf9b    33     -    439    11.8    0/   0/ 100/   0  0.452   1.25   48.1  12.7     -      -
08-29 12:50  51s     b95890  3bdf9b    25     -    439    11.9    0/ 100/   0/   0  0.233   1.27   24.9   7.0   4.1   base
08-29 12:49  36s     b95890  3bdf9b    22     -    439    11.9  100/   0/   0/   0  0.331   1.25   34.8   8.6     -   base
08-29 12:48  55s     3fe77e  c59be7    32     -    439    45.9    0/   0/   0/ 100  0.835   5.02   91.2  22.4     -   5.07
08-29 12:42  2m22s   3fe77e  c59be7    32     -    439    31.3    0/   0/ 100/   0  0.220  15.02  105.8  27.8     -   1.96
08-29 12:37  4m34s   3fe77e  c59be7    23     -    439    37.7    0/  98/   2/   0  0.138  15.08   55.0  13.9   1.1   base
08-29 12:32  4m39s   3fe77e  c59be7    23     -    439    31.4  100/   0/   0/   0  0.112  15.15   54.3  13.9     -   base

4 of 12 runs measured vs baseline: took 4m49s, no cache would have taken about 15m32s — saved 10m43s
(4 older run(s) not shown — `--top 12` for more)

publish    : `ucache publish` — a report with recommendations from this history;
             paths, hostnames and file names never leave the machine (docs/PUBLISH.md)
```

Reading it: two datasets, kept apart. In the lower group the two `base` rows are
the references — the bottom one ran with the cache switched off, the one above
it is a fill that qualified — and the same work served warm reads 1.96x from the
byte tier and 5.07x from recompressed replicas. The upper group has references
too, but both of its warm runs are under the 30-second floor, so they report `-`
rather than a number from too short a wall.

- `SIG` is the set of files; `RSIG` is which parts of them were read. Rows
  sharing both did the same work. They are not a gate on their own — whether
  two runs may be compared is settled file by file, at 90% — but a row whose
  `RSIG` differs from its baseline's is worth looking at before trusting the
  number beside it.
- `DIR/FILL/BYTE/REPL` is where the bytes came from, as percentages: straight
  from the origin without caching, fetched and kept, served from the byte
  cache, served from a recompressed replica.
- `PEAK cor` is the width actually measured — CPU-seconds per wall-second,
  sampled on the read path — not a thread count. `RIF orig` is the most reads
  the origin was being asked for at once. Both read `-` on records written
  before they existed, as above; a run recorded by a current build shows a
  number, and for `RIF` on a warm run that number is 0, correctly, because
  nothing went to the origin.
- `INS`/`IPS` are instructions retired and per second: the same analysis
  executes very nearly the same instruction count however the bytes reached it,
  so a row far off its neighbours did different work whatever else agrees.
  `OVH` is what the cache's own writing cost as a share of the origin wait, and
  is what decides whether a fill can be a reference.
- `GAIN` reads `base` for a reference run and `-` where the comparison was
  refused; `ucache summary` says why.

`RATE` is what the job consumed from all sources combined — paced by the
application, not a cache capability (that is `ucache bench`). A process that
opened no file and read nothing is left out — of this table and of what
`summary` calls the last run: a `ucache` command, or a parent process that only
started the workers that did the reading. Every process that read is listed,
however short (one under 30 s gets no gain: it is too short to time), and so is
a worker that ended without closing its files. `--top N` shows more rows;
`--json` on either command emits the same figures for scripting.

Records appear when a job that used the cache **exits**; CLI invocations do not
write them. `ucache stats --reset` starts a fresh counter window but keeps the
run history (moved under `stats/history/`, where both commands still read it) —
baselines survive a reset.

## A report with recommendations — `ucache publish`

`summary` and `history` tell you what happened; the report service tells you
what it means: how this disk grades against reference thresholds, how it
compares with your own origin measurement, and what to change. One command,
foreground, nothing automatic:

```sh
ucache publish              # asks for confirmation, prints the report URL
ucache publish --dry-run    # shows the exact payload and sends nothing
```

What goes out is the run history, the disk's benchmark records and this
machine's origin measurements — as numbers — and the time this machine takes
for a fixed CPU workload, measured at every publish (it takes about a second).
Paths, mount points and the hostname are replaced by salted hashes whose salt
never leaves the machine; file names never leave and origin URLs are reduced to
domains; run timestamps to dates. Every
field is listed in **`docs/PUBLISH.md`**, and the report URLs are unlisted:
whoever has one can read that page and nobody else can. `ucache bench
--publish` and `ucache netbench --publish` send a single record the same way.

## Monitoring — reading `ucache stats`

`ucache stats` aggregates the counters every process wrote and prints four
blocks. Run it after a job to see what the cache actually did. An example from
a warm pass:

```text
stats across 3 process file(s):
  opens              1574
  hit_bytes          58720256000 (54.7 GiB)
  miss_bytes         0 (0 B)
  origin_bytes       0 (0 B)
  ...
  crc_failures       0
  failopen_events    0
  admissions_bypassed 0
workflow:
  files opened       787 distinct (2.0 opens/file)
  served by tier     direct 0 B | fill 0 B | ram 1.2 GiB | disk 53.5 GiB | replica 0 B
  hit disk reads     2380995 (mean 41.8 KiB, 88% sequential)
  re-read factor     1.8x (first touch 30.4 GiB of 54.7 GiB byte-tier serves)
  fill flushes       4210 runs (mean 7.9 MiB)
read size p50/p95/p99 (log2 buckets, floor):
  requested          4.0 KiB / 64.0 KiB / 256.0 KiB
  byte-tier pread    32.0 KiB / 128.0 KiB / 512.0 KiB
latency p50/p95/p99:
  hit read           48us / 210us / 1.4ms
  origin rt          -
```

The first block is raw totals. `workflow:` is the arithmetic you would
otherwise do by hand — which tier served the bytes, how often each file was
opened, how much of the reading was re-reading. The last two blocks describe
what the storage was asked for and how long it took; `-` means there were no
samples, so `origin rt -` above is the point of a warm pass, not a gap.

### The four questions worth asking

1. **Did the cache engage at all?** On a warm pass `origin_bytes` should be
   **0** — every byte came from local disk. If it is large, or `relay_bytes`
   dominates, the cache is being bypassed rather than used; `ucache doctor`
   and the activation section above are where to look.
2. **Is anything wrong?** `crc_failures`, `failopen_events` and
   `validations_failed` should **all be 0**. They are separate on purpose:
   a CRC failure quarantines a page, a fail-open event means the cache
   degraded to pass-through, and a validation failure means an entry was
   discarded as stale.
3. **Is the cache big enough?** A non-zero `admissions_bypassed` means files
   were left uncached because everything resident was still in use — reads
   succeeded, uncached. Together with a large `evicted_bytes` it says the
   working set does not fit; `ucache status` names the remedy.
4. **Is the cache disk keeping up?** `fill stalls` is time a job spent waiting
   on the cache disk, and the `hit read` latency percentiles show what reads
   cost. If those are slow, measure the disk itself with
   `ucache bench <dir>` — a cache on the wrong device can be slower than the
   origin.

### Other forms

```sh
ucache stats --files --top 20   # per-file records, costliest first
ucache stats --reset            # start a fresh measurement window (records are kept)
```

`--reset` warns if a job looks like it is still running, since its counters
would vanish with the files.

Every field, and the JSON schema tooling reads, is in [Metrics](STATS.md).

## How it works (briefly)

**Read-ahead (off by default; `ucache set prefetch on`).** A ROOT reader
fetches a batch of baskets, works on it, then fetches the next; on a cold pass
it waits for the origin every time. uCache reads the tree's own metadata — where every basket of every
branch is and which entry it starts at — identifies the branches your job
uses from what it asks for, and fetches the next stretch while your code is
still computing on the current one. The prediction is checked before anything
is fetched (the first prediction is only compared with the next real request),
what is fetched is bounded by what your job has been drawing per request, and
a process that finds a quarter of its read-ahead unused switches it off. A
page fetched ahead reaches the cache only once your job has actually asked
for it; anything never asked for is dropped from RAM. Read-ahead never opens a
connection of its own: if your job is being served entirely from the cache, it
stays that way. Readers that fetch once per file open (one task per chunk)
never confirm a prediction, so they cost no origin traffic. `UCACHE_PREFETCH=on`
turns it on for one job; the `prefetch_*` counters in `ucache stats` show what
it did.

**When it is worth switching on.** It returns time your job spends waiting for
the origin, so it helps exactly where that wait is a large share of the run
and nowhere else. On the workload it is best measured on — a full analysis
over 1453 TTree files from a CERN-LAN origin at 32 threads, first pass — it
reads **about 1.2x faster for roughly 1% more origin traffic**. On a warm pass
it does nothing by design. It is largest when the time your job spends
fetching and the time it spends decompressing are about equal, and it falls
away in both directions: on a much slower origin you are bandwidth-bound and
the cache itself is what helps, and on a much faster one there was little wait
to remove. It is off by default because that gain is real but narrow, and
nobody should acquire a new moving part in their read path without choosing
it.

- **Page cache, not file cache.** uCache stores the pages your analysis
  actually reads (4 KiB each, 16 KiB on macOS), typically a small fraction of
  each file, each protected by a CRC32C. It never stores data it wasn't asked
  for beyond page rounding.
- **Validation.** A cached entry is trusted for a freshness window (default
  7 days): inside it, opens don't contact the origin at all — warm passes work
  even when the remote is down or flaky. When the window expires, the next
  open re-checks the origin file's size (and, with
  `UCACHE_VALIDATE=size+mtime`, its mtime, except for a file keyed by its
  logical name, below, whose modification time belongs to one site's copy);
  a changed file is re-cached. Set
  `revalidate_seconds = 0` to re-check on every open. With
  failover on, when that re-check finds no copy that passes
  (each refused by the checks at open, or none reachable), the open fails and
  the old entry is not served; the error then says that uCache holds an earlier
  copy, how long ago it was last confirmed at an origin, how much of the file it
  holds, and the window with which a job reads it from the cache without asking
  an origin (`UCACHE_REVALIDATE_S=<seconds>`; TROUBLESHOOTING.md). The entry is
  kept.
- **Fail-open.** Every failure path degrades to a normal uncached read. A
  read whose fetch from the origin ran out of time at its server (the server
  stopped answering) gets that error, as the read would without uCache, and so
  does every read of the job that was waiting for the same bytes from the same
  server and whose own time had run out by then; a waiting read with time
  left sends its own read with what it has (TROUBLESHOOTING.md).
- **Servers to avoid (`?tried=`).** A reader can keep a redirector from
  sending it to a given server by adding `?tried=<full host name>` (a comma
  separated list for several) to the file's URL. An entry with a `*` is a
  host pattern that uCache applies itself, with failover on
  for the file (Configuration, "Keeping reads away from some hosts"). uCache honours it on every
  origin open it makes for that file, its own included: when a file opened
  from the cache needs bytes the cache does not hold, uCache opens the origin
  itself, and that open carries the same `tried=`. Write one `tried=` and
  extend it with commas; do not add a second one. The open that goes straight
  through to the origin (no fresh cache entry) sends your URL exactly as
  written, and XRootD keeps only the last of several `tried=` parameters, so
  that open avoids only the servers in the last one. uCache's own opens merge
  them into one list. One form is not passed on to uCache's own opens: a
  reselection request, a `triedrc=` whose last reason is `resel`, which a
  client sends to get a second server beside the ones it already reads from
  (CMSSW does this). uCache's own open does not need a second server, and
  inheriting that request could send it to one this host cannot reach. With
  failover on, the servers in such a request that this process
  has seen fail a request for the file are kept (with `triedrc=ioerr`): they
  are the reason the client asked, and the others are servers it reads from
  without trouble.
- **Many processes, one cache.** Processes started separately, and workers a
  process forks after it has read through the cache (Python's `multiprocessing`
  and `concurrent.futures` process pools, ROOT's `TProcessExecutor`), fill and
  read the same cache; each keeps its own counters in `ucache stats`. A worker
  reading through a file its parent opened sees the file as the parent was
  shown it, or, if that layout was removed since and cannot be had again, gets
  an error on every read of that file handle (opening the file again works).

## Configuration

Three levels, lowest to highest, each with its own job:

1. **Your defaults** — `key = value` lines in the same `ucache.conf` that
   activates the plugin (§2), next to `url`/`lib`/`enable`. Edited by hand,
   by you; no `ucache` command ever writes this file.
2. **Current values** — set from the CLI without touching your defaults:
   `ucache set <key> <value>` / `ucache unset <key>` (kept in a small state
   file inside the cache dir; applies to processes started from then on,
   persists until unset).
3. **Per-job override** — the `UCACHE_*` environment variable twins, for one
   run: `UCACHE_REVALIDATE_S=0 ./my_job`.

`ucache settings` prints every knob with its effective value **and where it
came from** (default | conf | state | env); `doctor` flags any CLI-set values
overriding your defaults. Common keys:

| ucache.conf         | env                     | meaning |
|---------------------|-------------------------|---------|
| `dir = …`           | `UCACHE_DIR`            | cache location — **required** (no default; use a local disk). Unset ⇒ doctor FAILs, plugin runs uncached |
| `max_bytes = 50g`   | `UCACHE_MAX_BYTES`      | hard cache-size cap; unset ⇒ no cap (use the disk, evict at the floor) |
| `min_free_bytes = …`| `UCACHE_MIN_FREE_BYTES` | keep this much disk free (the default limit) |
| `in_use_seconds = 86400` | `UCACHE_IN_USE_S` | how long after a reader last used a file it stays in use: not evicted (read, or its maps handed out, this recently), and a replaced map keeps its place for readers that still hold it (default 1 day; 0 = plain LRU, and a replaced map stays valid only while an open handle holds it). Replaces `evict_protect_seconds` and `map_expiry_seconds` |
| `validate = size`   | `UCACHE_VALIDATE`       | `none` / `size` / `size+mtime` / `cksum`. **Caveat:** the plugin has no origin-checksum source yet, so `cksum` currently degrades to size-only (weaker than `size+mtime`) — prefer `size+mtime` until a checksum query lands. A local file (`root://local`) not keyed by a logical name is checked on `size+mtime`, unless this is `none` |
| `revalidate_seconds = 604800` | `UCACHE_REVALIDATE_S` | freshness window (TTL): an entry validated against the origin within this many seconds is served with **no remote contact at all**. Default 7 days — right for write-once physics data. `0` = re-check on every open; `ucache rm <url>` forces a re-check anytime. A local file (`root://local`) is re-checked on every open whatever this says |
| `open_retries = 0`  | `UCACHE_OPEN_RETRIES`   | retry a transient open failure this many times (0 = off); backoff via `open_retry_base_ms`/`open_retry_max_ms`. With failover on, an open through a redirector that passed its deadline at a server, or whose connection to the server timed out, was lost or could not be made, is not retried at the same URL: it goes to another site (below) |
| `recompress = off`  | `UCACHE_RECOMPRESS`     | `on` = the files your jobs read get fast-to-decode replicas **automatically**, created on their first pass (default off — opt-in CPU/disk). Flip it with `ucache set recompress on`. The conversion runs on one thread per CPU the process may use, and each thread that converts reserves 64 MiB of address space for its allocations: under a limit on the job's address space (`ulimit -v`) a reader on a machine with many CPUs can reach it (TROUBLESHOOTING.md) |
| `check_origin_integrity = off` | `UCACHE_CHECK_ORIGIN_INTEGRITY` | a setting for tests and diagnosis. `on` = check what arrives from the origin before it is cached or served: a block that fails a check is not served, and the read fails with an error naming the copy and the reason. **Forced on by `failover` for the files it applies to** (`ucache settings` says so). `recompress = on` does not need it: recompression checks every block it converts, and only those, each by its own checksum (zlib's adler32, LZMA's CRC32, LZ4's XXH64; ZSTD and uncompressed blocks carry none), and every RNTuple page it converts by the page's own checksum (XXH3-64, which ROOT writes after each page unless the file was written without page checksums); a block that fails is never converted, since the converted block would carry a new, valid checksum, and on a first pass it is not kept or served either: the read fails with "failed its checksum ... not served", naming the server. A job run with `recompress = off` that reads a file whose replica was made earlier still converts what it reads of it for the first time; with this setting off, a block there that fails its own checksum is kept as stored and served, and the reader's own decoder reports the damage, as it would without uCache. What this setting checks is what the cache fetches from the origin, against the file map: the position and length of each basket of a TTree, as the tree's own record gives them. Each basket that starts in the fetched bytes, its key header wholly in them, must say in its key that it sits where the tree's record puts it (ROOT itself refuses a basket whose key does not; bytes that are not the basket, zeros among them, fail it), with a key length above 0 and within the length the tree's record gives it, and a length and an object length of 0 or more (ROOT fails, or reads the wrong bytes, on a key that does not; a key whose own length alone is damaged, which ROOT does not read the basket by, is not held against the copy). Its length is held to the one the tree's record gives only where it matters to ROOT: ROOT reads a basket by the tree's record's length and decides from the key whether it is compressed, so a key whose length differs counts against the copy only where that decision changes (ROOT would take compressed bytes for the entries, wrong values with no error, or fail to decompress stored ones); a key whose length alone differs and that ROOT reads correctly is not held against the copy. Each basket that lies wholly in the fetched bytes, its key passing and its length known, is also checked by its own checksum where ROOT decompresses it: its data is decompressed as ROOT decompresses it, and a ZLIB (adler32) or LZMA (the xz stream's check) block that fails its checksum or does not decode, or an LZ4 block whose XXH64 (which ROOT verifies over the compressed bytes before it decodes them) does not hold, fails the read ("failed its check (its data fails its own check: ...)"); ZSTD blocks written without a content checksum (as ROOT writes them), ROOT's old algorithm and baskets stored uncompressed carry none and are not judged (nothing, ROOT included, can find damage inside them). A decoder that cannot do its work for reasons of the job's own process (no memory, for one, under a limit on its address space) fails no check and holds nothing against the copy: the fetch it judged is served unjudged and not kept, and a conversion that fails so is made again at the same copy; a stored page of uCache's own layout that it cannot decode so is kept and decoded again, and a read that still cannot have it fails with an error that says so (not "corrupted data"). An LZMA block whose decoder would need more than 1 GiB (it names a dictionary no ROOT writer makes; ROOT's largest is 64 MiB) fails its check. This costs CPU: one more decompression of each ZLIB or LZMA basket fetched from the origin (an LZ4 one costs a hash), about what ROOT spends decompressing it again, with each thread's decoders kept from one basket to the next. It runs on a pool of uCache's own, of at most one thread per CPU the process may use and at most 16 (`UCACHE_THREADS` does not set it), started one at a time as the work needs them (like every pool of uCache's), apart from the threads that serve the cache's hits and write its pages, and never on the XRootD client's: a fetch's baskets are spread over the pool in parts of about a megabyte, so a cold fetch is answered later by about its decompression time divided by the threads free, and no other request waits for it. Each decode thread keeps its decoders' state (an LZMA dictionary: 64 MiB of address space for ROOT's LZMA level 9, in memory only as far as a basket fills it), a scratch buffer of at most 16 MiB, the C library's own reserve for the thread's allocations (64 MiB of address space) and its stack (8 MiB): up to about 2.4 GiB of address space for 16 threads, which matters to a job run under a limit on its address space (`ulimit -v`). Other threads (recompression's among them) keep no decoder between uses. A basket recompression converts is decompressed once, by the conversion's own check. A copy whose tree's record fails its own checksum (ROOT cannot read the tree from it either), or whose record places a basket past the copy's end, is bad for that tree: the reads it answers of that tree's record, and of its baskets when the record could be read, fail the same way, while a read of another tree of the copy (a job reading only `Runs`) is served; what the cache entry took from that copy is dropped as soon as the map shows it. A read that holds such a basket, or such a part of a copy, fails with "failed its check (the reason) ... not served", and nothing of that fetch is kept (with failover on the read is first made again at another copy, and the map made again from it; below). A file whose tree's record uses a layout uCache's reader does not support, and a file with no tree (RNTuple), make no claim: their baskets are not checked. A tree's record that carries no checksum (ZSTD without one, or stored uncompressed) cannot show its copy bad, but the map is made from it and the baskets are checked. The map costs reading the tree's record once per file and process (tens to hundreds of KB, usually already in the cache). Nothing is decompressed for the check that carries no checksum: ZSTD blocks written without a content checksum, an xz stream that declares no check, ROOT's old algorithm and baskets stored uncompressed are not decoded (they count as carrying no check), and an LZ4 block is judged by its hash alone. Checked: byte-cache misses, single and vector (the pieces of one vector read that follow each other are judged as one run, so a key header or a basket that the cut between two pieces divides is judged whole); in a file that has a replica (`recompress = on`), whatever `recompress` says, every basket wholly in the original bytes a read fetches outside the converted baskets (a branch whose codec is not in `recompress_codecs` among them), the key of every basket placed in the replica (where the tree's record puts it, as for a miss: the replica rewrites a basket's own position, so ROOT could no longer catch one fetched from the wrong place), and the data of a converted branch's basket the conversion leaves as stored; a basket the conversion decodes has its data checked once, by its conversion; the reads the cache relays to the origin for a handle that holds the file's map (page reads, a read relayed after a cache error, the single reads of a file read straight from the origin under `max_read_fraction`, and a read moved to another site); read-ahead (nothing of a read-ahead fetch in which a basket fails, or made while the file's map is not known, is staged); and the bytes the checks at a job's open keep, when they hold the tree's record themselves (a small file's do; the map is then made from them). A request of another handle made while the file's records are read waits for that read and is checked then. A basket that fails fails the read ("failed its check ... not served"), nothing of those bytes is kept, and the entry is dropped; with failover on the read is first made again at another copy (below). A read waiting for another handle's reading of the file's records is bounded by that reading (the waiting read's own timeout does not shorten it), and when that reading fails the waiting read is served unchecked, and nothing of it kept, as on the byte path. Not checked: bytes already in the cache (among them the pages of the file's own records, which uCache reads to make the map and which are judged only by the record's own checksum, and the pages the checks at a job's open keep when they do not hold the tree's record: they are stored before the map exists); the reads of a handle that holds no map (a copy tool's, whose reads are passed straight to the origin, and a handle that has made no miss: making the map for it would cost a read of the file's records for bytes uCache does not keep); a basket that the fetched bytes do not hold whole (cut by the fetch's edge), or whose length the tree's records do not agree on; and, in a file with a replica, the bytes around a basket the conversion leaves as stored, up to the page on each side, which are cached with it (a small basket of another branch wholly among them is not judged). A build without the codec libraries makes no map and checks nothing. To find the baskets, the first read of a file that misses waits, before its own fetch, for uCache's own reads of its records, each with that read's own timeout (a stalled origin can hold it for several of them); a miss of another handle made meanwhile is answered only once those reads end, and is checked then. When one of those reads fails, misses are served unchecked, and nothing of them kept (`origin_checks_skipped`, and a warning naming the file, once per file and process), until one a second later or more reads the records again; with failover on, such a read that fails at its copy (a stall, a lost connection, a server's own error, a short answer inside the file's confirmed size) first moves the file to another copy, as any read does, and the records are read there (TROUBLESHOOTING.md). See STATS.md for the counters |
| `failover = on` | `UCACHE_FAILOVER` | when a file has more than one copy behind a redirector (a server that sends each open to one of several servers holding the file), an open or a read that fails at one copy is made again at another, and each copy is checked before it is used (see "Failover between copies" below). It helps only where there is something to fail over to: a file with one copy, or a URL that names one server directly, is read on the plain path, as with `off`. `on` (the default) = decided at each file's open: failover applies to a file whose URL names a server that reports a redirector's role, or whose URL has a host pattern in `tried=`, and `check_origin_integrity` is forced on for those files (`ucache settings` says so); every other file is read as with `off`. `off` = every file is read as in uCache 1.4.0. uCache never changes the XRootD client's connection settings. The values in force are in `run_settings` (STATS.md) and in `ucache doctor` |
| `logical_name_prefixes = off` | (none) | a space-separated list of path prefixes (CMS's official namespaces: `/store/data/ /store/mc/ /store/hidata/`); set, a file whose URL's path holds one of these prefixes is cached under its logical name, `lfn:/store/...` (the path from the first place a prefix begins), so one file read through any site or redirector is one cache entry (see "Keys by logical name" below). Only the cache's key changes: where an open or a read goes, and what it does when it fails, is as for any other file. Each prefix begins and ends with `/`. `off` (the default) = every file keyed by its URL, as before. Independent of `failover`. A property of the cache, not of a job: no environment variable sets it, so that two jobs never key one URL two ways; set it in the conf or with `ucache set` (which refuses a list that is not valid) |
| `recompress_keep_originals = off` | `UCACHE_RECOMPRESS_KEEP_ORIGINALS` | `on` = when baskets are converted into a replica, keep their original bytes in the byte cache too (by default they are not kept, and a copy held from before is released: the cache would hold the same data twice) |
| `complete_chunks = on` | `UCACHE_COMPLETE_CHUNKS` | when a read fetches baskets of a TTree branch from the origin, that branch's other baskets in the same clusters are fetched too, so that a later pass finds everything ROOT's read cache may read of those clusters and needs no origin, in whatever layout it is shown (see "Recompression"). With `recompress = on` they are converted with the rest; with it off they go to the byte cache (only branches in a codec of `recompress_codecs`, which a sweep would convert): with the miss of a vector read, and in the background after a single read's. Only the branches and clusters the job touched; the counters are `cold_replica_completed`, `fill_completed` and their `_bytes` (STATS.md). `off` = only the baskets the job asks for: a job that reads a few events per cluster keeps a smaller cache |
| `mixed_maps = on` | `UCACHE_MIXED_MAPS` | with `recompress = on`: each open of a TTree or RNTuple file is shown its newest map, the baskets or pages converted by then back to back at their real size (see "Recompression"); `off` = every open sees the first-pass layout, every basket at the size of its room and every RNTuple page decoded — for jobs that hand basket positions to workers on other machines with caches of their own |
| `recompress_codecs = lzma,zlib` | `UCACHE_RECOMPRESS_CODECS` | which **source** codecs are worth recompressing (comma list); branches in other codecs are served as-is |
| `page_size = 4k` | `UCACHE_PAGE_SIZE` | size of the pages the cache stores: 4 KiB, and 16 KiB on macOS, where a smaller page written into a hole of the cache file takes 16 KiB of disk anyway. Applies to files cached from then on. `ucache doctor` checks it against the cache disk |
| `log = warn` | `UCACHE_LOG` | `error` / `warn` / `info` / `debug`, optionally `:/path/to/file`. The XRootD client's own switch works too: `XRD_LOGLEVEL=Debug` shows uCache's debug messages, and with `XRD_LOGFILE` they go into the client's log file under the topic `UCache`. `ucache set log debug` turns debug on for every job; `ucache unset log` turns it off |
| `trace = off` | `UCACHE_TRACE` | `io` = write a sampled per-operation JSON trace next to the process's stats file (deep-dive forensics; zero cost when off). Best set per job: `UCACHE_TRACE=io python3 my_analysis.py` |
| `trace_sample = 64` | `UCACHE_TRACE_SAMPLE` | record every Nth read-class trace op (`1` = everything; opens/flushes are always recorded) |
| `prefetch = off`    | `UCACHE_PREFETCH`       | read ahead for TTree readers: the next batch of baskets is predicted from the file's own metadata and fetched while your code computes (default **off**; see below). `on` = try to stay a batch ahead of your code |
| `prefetch_window_mb = 32` | `UCACHE_PREFETCH_WINDOW_MB` | how far ahead one file handle may read (one fill's worth per branch, up to this) |
| `prefetch_ram_mb = 1024` | `UCACHE_PREFETCH_RAM_MB` | RAM the whole process may hold in read-ahead pages not yet asked for, counting both what has arrived and what is on the wire; the window shrinks as it is approached and reaches zero at the limit. With many reader threads this, not the window, is what sizes how far ahead each one gets |
| `prefetch_depth = 1` | `UCACHE_PREFETCH_DEPTH` | how many batches ahead to keep fetching. Measured and left at one: a second batch queued behind the first does not arrive any sooner (an origin answers one open file's requests one at a time) and guessing further ahead is less accurate, so on a full pass depth 2 fetched 6.6% more and read 6% slower. Raise it only for an origin that overlaps one file's requests |
| `prefetch_bridge_kb = 0` | `UCACHE_PREFETCH_BRIDGE_KB` | join predicted ranges separated by less than this into one request element. Off, and measured to be a bad trade on a busy origin: it fetches more bytes to save request pieces, and once several readers share a link the bytes cost far more than the pieces save. The extra bytes are thrown away on arrival and never enter the cache |
| `prefetch_map_cache_mb = 256` | `UCACHE_PREFETCH_MAP_CACHE_MB` | RAM for parsed basket maps, shared by every reader thread. One map of a 1500-branch file is about 21 MB, and a map dropped before it is used has to be parsed again |
| `prefetch_join = on` | `UCACHE_PREFETCH_JOIN` | when your job asks for bytes read-ahead is already fetching, wait for that copy instead of asking the origin a second time. `off` restores the older behaviour, which fetched them twice |
| `prefetch_threads = 4` | `UCACHE_PREFETCH_THREADS` | threads predicting and reading basket maps. One cannot both parse a new file's map and keep up with the batches of many reader threads |
| `announce = on`     | `UCACHE_ANNOUNCE`       | name uCache as the application in what the client tells servers at login, so a site can see traffic that comes through a cache (default on; see below). `off` = send your program's own name, as without uCache |
| `copy_detect = on`  | `UCACHE_COPY_DETECT`    | copy tools and copy engines read straight from the origin, so a copy is the origin's bytes (default on; see below). `off` = they are ordinary readers and use the cache |
| `max_read_fraction = 25` | `UCACHE_MAX_READ_FRACTION` | a job whose reads of a ROOT file (a TTree or RNTuple of 64 MiB or more) cover branches holding more than this percentage of the file's data reads that file straight from the origin and caches none of its data (see below). `100` = cache every file. `0` = read every ROOT file with a TTree or RNTuple, of any size, straight from the origin, caching none of its main tree's or RNTuple's data (its records and other trees are cached; see below). Needs a build with the codec libraries: without them every file is cached, whatever the value |
| `disable = true`    | `UCACHE_DISABLE`        | turn caching off (pure pass-through) |

Sizes accept `k`/`m`/`g`/`t` suffixes.

**Failover between copies (`failover`, on by default).** First, when it cannot
help: failover needs another copy to go to. A file stored in one place, or a URL
that names one server directly (a site's data server, or an EOS instance such as
`root://eoscms.cern.ch//eos/...`), has nothing to fail over to: uCache reads it on
the plain path, as with `failover = off` and as uCache 1.4.0 read every file, and a
failure there is the job's, as it would be without uCache. Failover applies to a
file opened through a redirector (CMS's global or regional redirectors, or a site's
own redirector, each of which sends an open to one of several servers holding the
file), and to a file whose URL has a host pattern in `tried=` ("Keeping reads
away from some hosts", below). For those files, an open or a read that fails at
one copy is made again at another, copies are checked as they are opened and
read, and servers that failed are remembered (all below).

**Which files failover applies to.**
A file whose URL has a host pattern in `tried=` has it, at its open, and no
server is asked ("Keeping reads away from some hosts", below). For any other
file, uCache asks the server a file's URL names what it is, at the
first open of a file there, once per server (host and port) and job, and waits
for the answers at most 5 s in all (or the job's own open timeout, when
shorter). It asks first for the flags every XRootD server gives at login
(`kXR_protocol`): a server that does not say `manager` there (a data server,
a proxy) is not a redirector, and nothing more is asked. One that says
`manager` is asked for its configured role (the configuration query `role`, as
`xrdfs <host> query config role` asks it): `manager`, `meta manager` or
`supervisor` confirm it; another role it names makes it not a redirector --
an EOS instance says `manager` at login (it sends each open to a storage
node) and `none` as its role, and failover does not apply to its files; a
server that names no role (an error, a key it does not know) is taken at its
login flags. Failover applies to the files opened at a redirector, everything
below included, `check_origin_integrity` too.
Anything else, an error, or no answer in time leaves the files of that server
exactly as with failover off; an answer that comes after the 5 s changes
nothing for the opens that went on without it. A server that gave no answer is
asked again at an open made a minute later or more, and a verdict, an answer
of this job's or a saved one, is renewed (asked again) at an open of the origin
made 10 minutes or more after it. Only a new answer replaces a verdict: a
renewal that gets none (the redirector slow or down just then) leaves the
earlier answer standing, and is tried again a minute later. The top redirector
of a set of sites (CMS's global one says `meta manager`) and a regional one
(`manager`) have it; so does **a site's own redirector**, which also says
`manager`: it too sends opens to several servers. To read every file as uCache
1.4.0 did, set `failover = off`.

The questions cost one round trip to each server a job opens files at (two to
a redirector), once per job (and once per 10 minutes at most), on the
connection the open then uses. What a server was found to be is saved in the
cache directory (`<dir>/roles`, FORMAT.md) for the other jobs of the cache.

An open the cache answers from a trusted entry (within its freshness window)
asks nothing, so a warm open never reaches the network: it takes what this job
already knows of the server, else the verdict saved in `<dir>/roles` (whatever
its age), and failover applies to the file or not as it did for the job that
asked. When neither knows the server (an entry filled by uCache 1.4.0, or with
failover off), the file is decided at its first access to the origin -- its
first read the cache does not hold, or the cache's own open of the origin:
the server is asked then (the same 5 s at most), before that read is made, and
the reads the cache holds never wait for it. A read made where uCache may not
wait (from inside the completion of another read) while the answer is not in
yet leaves the file undecided: that read is served, nothing it fetches is kept,
and a later read decides. So the second pass of a job, which
reads other parts of files a first pass cached, has the protections for what it
fetches. A handle that only relays (a write, a copy, the cache disabled) is not
asked for: failover does not apply to it. A handle a forked child inherits keeps the
decision its parent made. uCache never changes the XRootD client's connection
settings, which are the whole job's, so that a file failover does not apply to
is read exactly as without uCache. The protections of a file failover applies
to do not depend on them (the open deadline, the moves, the client's own read
recovery turned off and the checks are the file's), but the time the client
takes to give up on a server it cannot reach does: with the client's own values
(a connection tried for 120 s, 5 times, a server that failed answered with that
failure for 30 minutes) a redirector that was down for more than about 10
minutes can have the job's opens through it refused for up to 30 minutes after
it is back. `XRD_CONNECTIONWINDOW=31 XRD_CONNECTIONRETRY=2
XRD_STREAMERRORWINDOW=180` in the job's environment recovers within about 3
minutes. `ucache stats` counts the
servers asked (`role_queries`, renewals included), those that reported a
redirector's role (`role_redirectors`) and those that gave none
(`role_unanswered`); `log = info` names each server's answer. The setting takes
`on` or `off` in any case (and `1`/`true`, `0`/`false`); any other value
is ignored with a warning, the layer below (or the default, `on`) holds, and
`ucache doctor` names it.

Below, "with failover on" means: for a file failover applies to.

**The open deadline (part of failover).** With
failover on, every read open of a file uCache serves, the job's
own and the opens uCache makes of the origin, has a deadline of 60 s, or the
job's own open timeout, or the client's `RequestTimeout`, when that is shorter.
An open that passes it fails with the client's `operation expired` error
(TROUBLESHOOTING.md). With failover off, the job's own open waits as long as
it would without uCache (the job's own open timeout, or the client's
`RequestTimeout`), and the opens uCache makes of the origin wait for the
client's `RequestTimeout`.

**Opening a file at another site (part of failover).** With
failover on, when an open through a redirector fails at the
site it was sent to, in a way another site may not share -- the server refused
this client (`[3010]`), stalled past the deadline, could not be reached, or
failed the request with a server or storage error -- uCache opens the file again
through the redirector, naming the servers that failed in `tried=` so that the
redirector sends it elsewhere. This holds for the job's own open and for the
opens uCache makes of the origin. A success is taken wherever it lands; for the
opens uCache makes of a file it holds, only a copy of the size it holds; a
copy with no bytes of a file named `*.root` (a ROOT file is never empty; a size
of zero is an ordinary size for any other name; the name is the path without its
query, `.root` in upper or lower case), a copy of such a file whose first bytes show
it is not a whole ROOT file, and a copy on tape (the server's answer to the open says
`Offline`) are passed over for another site's. Inside the job's open uCache checks the
copy as ROOT itself does when it opens a file, before the job is shown it; a request the
job makes on the file while its open is being checked waits for the open's answer (one made
with a completion handler is held and made once the open has answered, so the call returns
at once and never holds the thread that made it). uCache's
own open of such a file makes the same checks when it knows no size the copy must have (an
entry trusted at the job's open that is gone by its first request). It reads the
first 300 bytes of the copy (what ROOT reads at open; the whole copy when it is smaller),
which hold the file's header and, unless long names push it further, the record of the
file's top directory (one more read of 60 bytes fetches it then); then it reads the list
of the file's top-level objects (the record, near the end of the file, that says where each
tree, RNTuple or histogram lies; a list over 16 MiB is not read); then, in one vector read,
the label at the head of each record that list names and of the class descriptions (below).
It refuses a copy:

- whose size is smaller than the end its own header states (a transfer that was cut);
- that does not begin with `root` as every ROOT file does (a copy of zeros at the right
  size is one), or is too short to hold its own header;
- whose header contradicts itself where ROOT refuses a file for it: its first record
  begins before the start of the file or past the end of the data, the top directory's
  name record is shorter than 10 or longer than 10000 bytes, or the directory's record runs
  past the end of the data;
- whose top directory's record places the list of top-level objects, or whose header places
  the class descriptions, outside the copy (from the first record to the copy's size; class
  descriptions placed at or before the first record are not read by ROOT, and not looked
  at). The list of free space is not looked at: ROOT reads it only for a file opened to be
  written;
- whose list of top-level objects does not carry its own label (every record of a ROOT
  file begins with one, giving its position and its directory), or names an object outside
  the copy or in another directory than the top one (where the header places the top
  directory's record, or where that record says it is), or in one below 64, where ROOT stops
  reading a list: a list zeroed, or overwritten with
  other bytes, is refused here (without uCache, ROOT would scan the copy to rebuild such a
  list, and keep what it finds);
- whose tree description, RNTuple or other object the list names, or whose class
  descriptions, were zeroed or overwritten while the list survived: the label at the head
  of that record does not give its own position and the top directory, and ROOT cannot read
  the record from those bytes either. ROOT itself does not compare a record's label with
  where it read it (it reads an object by the list's account of it, and the class
  descriptions by their own label), so a label that does not give its place counts against
  the copy only where ROOT would fail too: the object is stored compressed and its data, where
  the list says it starts, does not begin a block ROOT decompresses, or the class
  descriptions' own label names no list of classes. A label whose own fields alone were
  damaged, and an object stored uncompressed (an RNTuple's anchor), make no claim. Damage
  inside those records, past their first bytes, is not looked at (ROOT's own check of the
  block's checksum finds it when the job reads the record, and the job fails);
- whose server answers any of those reads short.

The labels' read takes, for each record, its label's fields and the first three bytes of its
data (about a hundred bytes; 160 for the class descriptions), at most 64 records: the class
descriptions, then the trees and RNTuples, then the other objects, in the list's order. In a
file whose top directory lists more objects, the labels of the rest are not read, so a
directory of thousands of histograms costs no more than 64. A label that lies in the pages
the checks already read is judged from them, and for a file uCache caches the labels of the
records the job reads right after its open (the class descriptions, trees, RNTuples) are read
as their whole pages and kept, as the checks' other reads are. A list that carries ROOT's
mark of a freed record (a writer stopped in the middle of a save, below) makes no claim on any
label: the records it names may have been deleted and their space reused since, and ROOT reads
such a file. These checks at open run only with failover on, where a copy they
refuse can be replaced by another; `check_origin_integrity` alone does not turn them on.

The first read gets what is left of that attempt's time (at least 5 s); each later one gets
what is left of that, or its own floor when that is more (5 s and 1 s for each MB it reads),
within the open's own time; and one of them that fails or runs out of time is a failure of
the copy, as an open's would be. For them the client's own recovery of a read and its
following of a server's redirect are off, so that no other copy answers them. Where the open
would end with such a failure (a URL that names a server directly, the last open of the
job's own URL after a copy on tape was passed over, no other copy left to try, or the open's
bounds spent), the open is made once more at the same place when the client would have
recovered the read itself (a lost connection or session), as it would have. A copy whose size is larger
than the end its header states is not refused: ROOT reads such a file normally and ignores
the bytes past that end (a file updated in place, bytes appended). The list of top-level
objects and the objects it names are held to the copy's size, not to that end, since ROOT
writes them without the header when a file is saved with `TFile::Save` or
`TDirectory::SaveSelf`: a file saved so, and then written on or left by its writer, has its
list and its newest objects past the end its header states, and ROOT reads it normally.
Nor is the length a list's own label gives compared (ROOT reads the length the directory's
record gives): a writer stopped in the middle of such a save leaves the earlier list with
its length overwritten, and ROOT reads that list. Nor is a copy whose
header places no list of top-level objects (its writer stopped before it ever saved the
file): every copy of such a file is the same, ROOT recovers what it can of it, and the
check makes no claim on it. A file not named `*.root` is not looked at. For a file uCache
caches, what the checks read of the copy the open takes is kept in the cache, so the job's
own reads of the header and of the list of top-level objects right after its open (ROOT
reads both when it opens a file) are served from it instead of from the origin again: each
read of the checks then asks for the whole pages it touches (the cache keeps whole pages
only; 4 KiB each by default), within the copy's size and at most 1 MiB of a copy, and the
bytes go into the file's entry when the job first reads it (or as its open answers, when a
request of the job's came first), if the entry is of that copy's size
(`open_check_bytes_kept`, STATS.md). Stored as the entry is set up, these pages are checked
(`check_origin_integrity`) only when they hold the tree's record themselves, from which the
file map is then made (a small file's do); otherwise uCache has not yet read where the file's
baskets lie, and they are stored unchecked, as the pages of the file's own records are. Nothing a refused copy's checks read is kept, nor
anything of a file the cache does not cache, whose checks read only what they need. A copy that passes
the checks and whose size equals the end its header states confirms the file's size (a
header that places no list of top-level objects confirms nothing), which the cache entry keeps
(FORMAT.md) for its freshness window (`revalidate_seconds`, 7 days by default). Within
the window a copy of another size is not accepted for the entry or for a handle that
holds it: a job's open that the entry answers, with no open at the origin, takes the
confirmed size from the entry and reads nothing of a copy, and a read that then needs
the origin and finds only copies of another size fails with one error naming them (the
next open checks the origin again). After the window the next open checks the origin
and reads the header of the copy it lands on: a copy that fails the check is refused, and
one that passes at another size (the file was rewritten) replaces the entry and becomes
its confirmed size. With `revalidate_seconds = 0` every open does so. An entry made before
these checks existed (by an earlier release, or with failover off) carries no confirmed
size: within its window, a job's open that such an entry of a `*.root` file would answer
first compares the end its header states, as the cache holds it, with the size the entry
holds, reading nothing but the cache. An entry whose header states an end past its size was
filled from a cut copy: it is dropped (`bad_entries_dropped`, STATS.md), and the open goes
to the origin as one with no entry, with the checks above. An entry that does not hold the
file's first page, or whose header makes no claim, answers the open as before; one that
carries a confirmed size is not looked at again. When no copy passes at
that open, the job's open fails with the one error that names the copies, and the entry is
not served; when uCache holds one, the error says so and how to read it from the cache
(TROUBLESHOOTING.md, "The open fails, and the error says uCache holds an earlier copy").
When no other site opens a file whose copy on tape was passed over, it is opened once
more as the job asked for it and that answer stands, so a file that is readable
without uCache stays readable. When every copy is refused for what it is (empty, or its header or its list of top-level objects not those of a whole ROOT file;
or the URL names a server directly and that copy is), the job's open fails with one error of class `data`
that names each copy tried, its size and why it was refused. Each open tries
at most 4 sites, within 180 s in all (less when the job's own open timeout is
shorter, or the client's error window is set short: 20 s less than it, 160 s for
an error window of 180 s), and an open that
fails at every site it tries ends with an error naming each site and its
error. A file the redirector finds nowhere, a
redirector that is down or refuses the client itself, and a direct URL to a
server are final, as without uCache. The servers named are full host names
only: an address or a name without a dot keeps a redirector from nothing, so
a server known by one is not named. When the redirector sends the open
back to a server or site that failed already, or finds no copy after servers
were left out, or when a server stalled past the open deadline or its
connection failed (the client's stream timeout on a connection that went quiet,
a connection lost, or one that could not be made: a retry through the
redirector may meet the same site again, the other server of which stalls as
well; a host name that does not resolve, and TLS and authentication failures,
are not counted as such), uCache asks the regional redirector where the file is (a
`locate`), asks each listed copy that the job did not exclude for its state
(a copy on tape is not opened; a copy that does not answer within 10 s has
failed), and opens the first that answers directly,
or through its own redirector when it is one, naming the servers that failed
there. For a file uCache caches, the allow and deny lists narrow the copies taken
from the regional redirector's list: a listed copy whose host the lists refuse is
not tried. Where a redirector itself sends an open is not filtered, for a cached
file too: the server it sends the open to is tried whatever the lists say of its
host. For a file whose reads uCache passes straight to the origin (the lists leave
the host of the job's URL out) the lists say nothing of where it is read, and every
copy listed may be tried. The copies listed are tried within the same bounds. A server that failed, and the
other servers of its site, are left to the end: when only copies
at such places are left, they are tried too, the oldest failure first. A site is
the name its servers give for it (their `all.sitename`, which a server tells when
asked for its configuration value `sitename`, as CMSSW's XRootD adaptor asks
it): uCache asks each server that answered an open, and each copy the regional
redirector lists when its turn comes (beside the question of its state, so a
copy never asked its state is never asked its name), once per process and off
the client's own threads (a few hundred bytes each), and nothing waits for the
answer but a listed copy's turn, which waits for its name at most as long as its
stat took. Two servers are of one site when both names are known and agree; while
either name is not known (not asked yet, none configured, no answer), when their
domains agree, as before site names (a server known by an address, or one that
never answered, being its own site by domain). Two servers of one domain that give
different names are two sites. So the servers of one site count as one of the 4
sites wherever they failed, and a copy at a site that failed is left to the end
though it is named by an address (`site_name_queries`, `site_names_missing`,
STATS.md).
A job's own `tried=` list is kept ahead of uCache's names. An open
that uCache makes of the origin holds one of its worker threads while it tries
sites, so with a stalled site and every worker so held, the next step of a
job's own open waits for a free one. With
failover off nothing of this happens: an open's failure is the job's, as without
uCache.

**Reading a file from another site (part of failover).** With
failover on, a file that was opened through a redirector, whether
uCache serves it or passes its reads straight to the origin (its host left out by the
cache's allow and deny lists), or shows it in a layout of its own (`recompress = on`),
has the reads it makes of the origin bounded: one that has not been answered
after 5 minutes (a backstop: a server that goes quiet is found sooner, by the
client's own check of a quiet connection, 60 s plus its timeout resolution of
15 s), or the job's own timeout of that read when that is shorter (the
client's `RequestTimeout` when the job gave none), is given up. A read that
fails in a way another site may not share, because the server stopped
answering, the connection was lost, the request expired, or the server
reported a fault of its own or of its storage, is not the job's to see: uCache
opens the file again at another site as it does for an open (above), takes only
a copy of the size the job was shown, and issues the read again in full, so the
job gets the bytes. The server it left is tried last from then on. For a file
uCache caches, its allow and deny lists narrow the copies a read moves to as they do
an open's: the copies taken from the regional redirector's list, never where a
redirector itself sends the move's open. So when the redirector sends the open back to a
server that failed and the lists refuse every other copy the regional redirector lists,
that server is taken; for
a file whose reads it passes straight to the origin they do not, and a read moves to
any copy listed. A read that
failed because of what it asked for (an unsupported or invalid request) fails
as it does without uCache. One failed read serves every read that fails while
the file is being opened elsewhere; a read is tried at up to 4 servers, within
the time it was allowed. The bytes of a read that was served elsewhere are not
kept in the cache. When a basket of a read fails the check of `check_origin_integrity`
(its key's position or length, or its data's own checksum), the read is made again at
another site too, and its answer is checked as well; the job never gets the damaged bytes. So is a
read that a copy answers short, inside a file whose size is confirmed (a file named
`*.root` whose copy's header said where it ends, at the size the copy reported: at
this open, or, for an open the cache entry answered within its freshness window, at
an earlier one): the copy ends before the file does, which is a data failure of that
copy. So is a page read (`PgRead`, passed straight to the origin) that the copy answers
with no bytes inside that size, which the XRootD client fails itself as an invalid
response: the read is made again in full at another copy. A short answer is normal when a read reaches past the end of a file, and without
a confirmed size it is never held against a copy: the read ends as it would without
uCache. When no other copy of the size the job was shown serves a read that moves, for
any of these reasons, the job gets one error, with the code of the read's own failure,
that names each copy tried, its size where it is known, and what it said; a read that
fails within a second after that gets the same error at once, without another attempt.
A copy that answered a read with bad bytes (a basket that failed the check, a short
answer inside the confirmed size) is never taken again for that file handle: the
regional redirector's list of copies no longer offers it, and an open the redirector
sends back to it is refused. Nor does anything it sent stay cached: when the file's cache
entry holds bytes this process fetched from that copy (or from a copy an open refuses for what
it is, above), the whole entry is dropped
(`bad_entries_dropped`, STATS.md). No handle of the process is served from it again (their
reads go to the origin, and move as above), and its files are removed, so the next open of
the file starts a new entry (at once, or when the job ends: a job killed first, or ending with
`_exit`, can leave them). The same holds when a block fails its own checksum where
`recompress` converts it. This needs `check_origin_integrity`, which failover forces on.
Only this process's own fetches are known: an entry another process filled from that copy,
or this process before it opened the file this time, is not dropped, and another process
that holds the entry open at the same time goes on serving what it holds until it closes the
file (what it adds is no longer kept). A handle shown uCache's own layout keeps the blocks
that layout holds converted until it closes (each passed its own checksum when converted);
the layout's file is removed with the entry. Because the client's own
recovery of a read would otherwise run first, such a file has it turned off
(`ReadRecovery` and `WriteRecovery` read `false`, and a program cannot turn them
back on): a call that the client used to recover on a connection that was lost
while the file was idle, such as a forced `Stat`, fails at once instead, and
the next read moves the file. A file shown in uCache's own layout (`recompress = on`)
moves the same way: the request that failed is served again from the start once the
file is read from another copy, its fetches made there at the origin's offsets (the
baskets it converts and the bytes it keeps as they are), checked as from any copy;
the addresses the job was shown never change. Each fetch is judged on its own: nothing
of a fetch that failed is kept, while what the request's other fetches brought from the
copy it left and that passed their checks stays (unless that copy is found bad: then the
entry is dropped, as above). A failure made in uCache itself (a conversion that ran out of
memory, say) is no copy's: the request is served again at the same copy, as with
failover off. A file read from a direct URL and a
file the job copies are not changed. With failover off nothing of this happens.

**Keys by logical name (`logical_name_prefixes`).** CMS names each
file by its logical name (`/store/...`), and the same file is reached through
many URLs: a regional or the global redirector, a site's server, a site's own
prefix (`root://eoscms.cern.ch//eos/cms/store/mc/...`). With
`logical_name_prefixes` set (it is off by default, and does not depend on
`failover`), a file whose URL's path holds one of the prefixes has a
logical name (`lfn:/store/mc/...`), whatever server the URL names, and its cache entry
has several names: the logical name, which is the entry's core name and groups the
others, and each URL that reached it. One entry, read once from the origin, then
served to every URL of it. Only the cache's names change: where an open or
a read goes, and what it does when a server fails, is as for any other file of
that URL (a direct URL stays a direct URL; no redirector is added to it). The
prefixes are looked for in the path
alone (no scheme, server, port or query; repeated slashes, `.` and `..` resolved
first), anywhere in it, and the name runs from the first (leftmost) place where any
of them begins to the end of the path: with CMS's `/store/data/ /store/mc/
/store/hidata/` (`ucache set logical_name_prefixes '/store/data/ /store/mc/
/store/hidata/'`), `/eos/cms/store/data/x/y.root` is named `/store/data/x/y.root`.
Each prefix begins and ends with `/`, so it only matches whole parts of a path
(`/store/mcextra/` is not `/store/mc/`). A path that holds none has no logical name.
The list is space-separated, at most 32 prefixes of at most 256 bytes, each
printable, with no empty, `.` or `..` part; `ucache set` refuses a list that is
not, and one in the conf or the state file keys every file by its URL, said once in
each job's log, and fails `ucache doctor`. Parameters
kept by `keep_cgi` follow the name as they follow a URL's path. `ucache ls` prints
an entry by its logical name, `ucache ls -l` lists its URLs under it (of the newest 8
it recorded, those that still lead to it; every URL that reached it stays a name of it), and every
command takes any of its names (`ucache pin lfn:/store/mc/...`, or a URL).

- **The assumption.** A path that holds a prefix names the same unchanging bytes
  on every server. CMS's official data is written once under a name that is never
  reused, which is why it holds there. Inside the freshness window a cached file is
  served without asking any server, whatever URL the job gives.
- **The risk.** A prefix matches anywhere in a path, further down
  too: a user's copy that keeps CMS's layout (`/eos/user/x/me/store/mc/...`) is
  then keyed by the official name and, inside the window, served the official
  file's bytes (or serves its own to a later job that asks for the official one).
  A cache that holds such copies sets `logical_name_prefixes = off` (every file
  keyed by its URL), or a list that none of its users' paths holds, and `ucache rm`s
  the entry: with the prefixes off a URL still reaches the entry it was a name of
  while that entry exists, and its own key is its own again once it is removed (a
  name points to one generation of an entry: a file cached again later under the
  same key is not reached through it). In the same way, one site's copy of the
  file's size whose bytes are damaged reaches the readers of every URL of the file
  once it is cached, a healthy site's direct URL included, unless
  `check_origin_integrity` (which failover turns on) catches it as it
  is fetched.
- **Copies of another size.** One name reached through several sites can meet a
  copy that is not the file: cut short, or another version. A URL whose copy is not
  the size of the entry it reaches by the name is cached apart under its own URL
  (`name_size_mismatches`), and the name's entry is left as it is. The sizes are
  compared before the entry is ever served to that URL: at once when the URL has an
  entry of its own cached before (with the prefixes off, or by a job with other
  settings), else by opening that URL's copy at its origin (inside the freshness
  window too) the first time a job reaches the entry through it, and again by every
  job that reaches the origin through it. Otherwise the rules that keep a file's
  bytes from two copies apart hold as they do for one URL: an entry is never filled
  from a copy whose size differs from it (the read is made at a copy of its size,
  through the redirector the file was opened through, or fails naming both sizes),
  and a copy whose header contradicts its size is refused at the open.
- **Freshness.** With `validate = size+mtime` a file keyed by its logical name is
  checked on its size alone: a modification time belongs to one site's copy, and a
  change of site would start the entry again.
- **Turning it on keeps the cache warm.** When the prefixes are set, the entries
  already cached get their logical names: at once with `ucache set` (it says how
  many), else once, in the background, by the first job with them. An entry cached by
  a URL keeps its data, its pin, its replica and its records; nothing is fetched again
  or copied, and every other URL of the file then reaches that entry, once its copy
  was found that entry's size ("Copies of another size" above): a copy of another size
  is not the file, and that URL is cached apart, under its own key, from then on
  (`name_size_mismatches`, STATS.md). A job that opens a file before its entry was
  named names it itself. When two URLs of one file were cached as two entries before,
  the one named first (by the naming, the most recently used) becomes the file's
  entry, and the other URL becomes a name of it; that URL's own old entry is not read
  while the prefixes are set and ages out like any entry nobody reads (a pinned one
  stays until `ucache unpin <its key as ls prints it>`). Bytes of two entries are
  never mixed. Turned off (or set to another list) and set again with `ucache set`,
  the same list too, the entries cached by URL in between are named the same way;
  a cache whose prefixes are changed only in a job's conf is named once per list.
  A job that holds an entry trusted at its open and finds, at its first read, that
  the name now leads to another entry (the first was removed meanwhile) opens the
  file at its origin, where the sizes are compared, before it is served from it.
- **Turning it off keeps every entry reachable.** With the prefixes off again, each
  entry is still found through each URL that reached it, while it exists (and is
  not started afresh); only the logical name is no longer used. With `validate =
  size+mtime` the modification time counts again once the prefixes are off: a URL
  whose site's copy has another modification time than the one the entry was cached
  from is not served the entry, and is cached apart under its own key from then on.
- **One setting for the cache.** Set the prefixes for the cache with `ucache set`
  (its state file, which every job of the cache reads); never from a job's
  environment. A value set in one job's conf only makes that job key files by name
  while others do not: each still finds an entry through its URL, but a file the
  other job caches first is cached twice. `ucache doctor` says so when the value
  comes from the conf. Readers that hand positions in a file to workers in other
  processes (uproot.dask) rely on one setting.
- **A cache shared with an older release.** A release without logical names reads
  and writes a cache that has them, safely, but an entry whose index it rewrites (a
  job of it reading the file, or its `ucache pin`) loses its names: the file's other
  URLs no longer reach that entry and may cache the file again, and `ucache ls -l`
  stops listing them under it. Nothing wrong is served. An older `ucache` lists an
  entry stored under its logical name by its `lfn:` key, cannot act on it through
  a URL (`pin`, `rm`, `verify`, `branches`), and its `recompress` counts each such
  entry as failed ("not a usable cache key") and leaves it as it is. With
  `validate = size+mtime`, a job that checks a file's modification time and finds it
  changed starts the entry again; a job that holds the entry from before never adds
  its pages of the earlier copy to it.
- **Removing, pinning, verifying.** `ucache rm`, `pin`, `unpin`, `verify`,
  `branches` and `untranspose` act on the file's entry whichever of its names they
  are given; `rm <url>` also removes that URL's own old entry if one is left beside
  it. The entry's names go with it (`rm`, eviction, `clear`): the next job to read the
  file through any URL caches it again, under the name.
- **The readouts.** With the prefixes set in the settings a command reads (`ucache
  history`, `stats --files`, `summary`, `publish`), or when any run of the records
  keyed files by the cache's prefixes (its `run_settings` say `logical_names=on/`
  and the list's hash), records written by URL are read as the file's name's: a
  baseline matches across the change, and a run's `sig` is computed on the names
  (STATS.md, PUBLISH.md). Such a record keeps the origin its URL named. The records
  of a run keyed by other prefixes are read as written (the command says so once).

**Remembering sources that failed (part of failover).** With
failover on, the processes of one cache remember the servers and
copies that failed, so that the next open does not meet them first:

- **A server** is marked when it stalled (an open that ran out of uCache's own
  deadline there, a connection gone quiet past the client's stream timeout
  unless the job set it below the client's 60 s, a listed copy that did not
  answer its state) while another site answered
  normally at the same time or within a minute, or when it answered with a fault
  of its own (its storage, its file system, an internal error, no memory,
  overload). Never for authorization, TLS, a name that does not resolve, a
  connection that could not be made or was lost, a file not found, bad bytes, or
  a timeout the job gave (a read's own expiry is not marked: its time may be the
  job's). A mark lasts 10 minutes, doubled at each further failure in a row, at
  most an hour, and is cleared at the server's first successful open (one that
  the process sees: a process reads the marks of the others again at most every
  30 s, so a mark another process made just before may outlast a success). A site is
  marked while two or more of its servers are (sites as above: by the names
  their servers give, else by domain).
- **A copy** is marked for 7 days when it is proven missing: its server said the
  file is not there although the regional redirector, asked again, still listed
  it there. It is noted in the file's in-use record.

Marks only order: a copy at a marked server or site, or a marked copy, is tried
after every copy the regional redirector lists that is not, and when every copy
is marked all are tried, the oldest failure first. Nothing is left out for a
mark, the bounds of an open are unchanged, and a redirector's own choice is not
overruled (the marks are not sent to it). Server marks live in `<dir>/sources`
(FORMAT.md), shared by every process of the cache; `ucache sources` shows them
and the copies marked (`site_faults_marked`, `site_marks_cleared`,
`copy_faults_marked`, `marked_copies_last`, STATS.md). With failover off
nothing is marked or read.

**Keeping reads away from some hosts (`tried=` patterns, part of failover).**
A redirector compares the entries of `tried=` with whole host names only, so a
site's many servers cannot be named there in one entry. uCache can: an entry of
`tried=` that holds a `*` is a pattern on the full host name, `*` standing for
any run of characters (dots included), compared ignoring case. To read through
a global redirector while keeping every read away from the servers of one site,
and of a second one:

```
root://global-redirector.example//store/data/Run2024A/file.root?tried=*.site-a.example,*.site-b.example
```

With failover on (the default), a URL with a host pattern has failover for its
file, at its open, whatever server the URL names and without asking it its role:
the reader asked for hosts to be avoided, and only failover's behaviour avoids
them. uCache then applies the
patterns itself and never sends them: no URL that reaches a server carries one. An open that lands on a data server whose name matches is refused
there before anything is read of it, and asked again through the redirector with
that server's own full name added to `tried=` (where a front end handed the open
to the server, as EOS does, the front end's name, which is the one the
redirector knows); the names added in one open accumulate, each kept for every
later attempt of that open (at most 16 of them, the list at most 1024 bytes). The
copies the regional redirector lists are passed over when their server matches,
and a read that moves to another copy never moves to one that matches. The
client's own recovery of a read is off on every file with a pattern (it would
open the file again through the redirector, which may send it to a matching
host): a read that fails moves as above, or fails. For the same reason the
client follows no server's redirect of a read once the file is open: such a
read moves to another copy, judged as an open's landing is, or fails. A
redirector is never refused for a pattern, whatever its name: it holds no copy.
A storage front end that hands opens to its own servers (EOS) is a redirector:
its servers are the data servers, so a pattern that matches only the front
end's name (`eoscms*`) blocks nothing; one that matches its servers' names
(`*.cern.ch` for EOS at CERN) blocks it, and the front end's name is the one
added to `tried=`.
When every copy found is at a matching host, the open fails with an error that
says so and names each of those servers with the pattern it matched
(TROUBLESHOOTING.md); nothing is ever read at a matching host instead. A
redirector that keeps sending the open to matching hosts ends it the same way
after 8 refusals. Patterns and exact names mix in one list: the exact names are
sent to the redirector as they are, the patterns are uCache's. An address is
matched as it is written (`127.0.0.*`, `2001:db8:*`), and a host as the
redirector names it in its redirect. A pattern is on the host only: an entry with a
`*` that is not a host pattern (a port, `/`, `%` or a blank in it; longer than a
host name can be) is ignored with one warning, and not sent either. Each refusal
is recorded (a `host_avoided` line in the per-file records, FORMAT.md; the open's
own line carries `avoided`) and counted (`hosts_avoided`, STATS.md), so a run can
show that no byte came from a matching host (`data_servers` in the per-file
records). A warm read is served from the cache as before and asks no server, and
`tried=` is never part of the file's cache key, so one file read with and without
patterns is one entry. With `failover = off`, and for a file uCache
passes straight through (a copy made with a copy tool such as `xrdcp`, a write,
`UCACHE_DISABLE`), the URL is sent exactly as written, the patterns included, as
without uCache: a redirector then avoids nothing for them. So a run that must
keep away from some hosts neither sets `off` nor copies its inputs through uCache
(or sets `copy_detect = off`, so that a copy is read as any job's read is).

**Files on a mounted or slow file system (`root://local`).** uCache also
caches files that no server stands in front of: a network or FUSE mount
(CephFS, NFS, EOS through `eosxd`, AFS) or any other file system that is slow
to read. Name the file with the host `local` and its absolute path:

```sh
root -l -b -q 'analysis.C("root://local//mnt/cephfs/data/file.root")'
python3 -c 'import uproot; uproot.open("root://local//mnt/cephfs/data/file.root")'
```

The plugin reads `/mnt/cephfs/data/file.root` itself, through the XRootD
client's own access to local files: nothing is sent to any server, and no
server named `local` is needed or contacted. Think of it as the operating
system's page cache made complete: the bytes your jobs read are kept on the
cache disk, outlive the process and a reboot, are not limited by memory, and
are evicted by uCache's own rules (`max_bytes`, `min_free_bytes`,
`in_use_seconds`, `ucache pin`) rather than by memory pressure. Every open
opens the file (with your permissions) and compares the cached entry with it
as it is now, whatever `revalidate_seconds` says: by its size and
modification time (to the second), or by its size alone with
`validate = none` or when the file is keyed by a logical name (below). So a
file rewritten in place is read afresh by every new open, also in a job that
still has it open. As without uCache, a file must not change while a job reads
it: a handle open across the change can read bytes from before it and from
after it. A vector read past the new end of a file truncated under it fails,
where the file system would answer it short (ROOT checks only that a vector
read succeeded). Only the `root://local` name
goes through uCache: a program that opens the plain path (`/mnt/...`) reads
around it.

- The host is `local`, never `localhost`: `root://localhost` is the address
  of a real server on your machine, and a job run without uCache must not
  find that a URL means something else. Without uCache, `root://local` fails
  at once wherever no machine is named `local`, rather than reaching some
  other server. So a job that names `root://local` needs uCache loaded
  wherever it runs: where it is not, the open fails (a server's URL would be
  read uncached). With the plugin loaded but disabled (`UCACHE_DISABLE=1`, or
  no `dir`) the file is still read, uncached.
- The shipped conf binds every host (`url = *`), `local` included. A conf
  that lists its hosts needs `local` in the list
  (`url = eospublic.cern.ch:1094;local`); that binds `root://local` on the
  default port, and `url = *` binds every scheme and port.
- `root://local//abs/path` and `root://local/abs/path` name the same file, and
  repeated slashes and `..` are resolved in the text of the URL, as for any
  URL, not by the file system: after a symbolic link, `..` can name another
  file than a program would open, so give a path without `..`. A directory, a
  pipe or a device is refused at its open.
- With `logical_name_prefixes`, a local file is keyed as any URL is:
  `root://local//eos/cms/store/mc/...` and
  `root://eoscms.cern.ch//eos/cms/store/mc/...` are one entry, checked on its
  size alone as every file keyed by name is ("Freshness" under "Keys by logical
  name" above): a rewrite in place at the same size is not noticed
  (`ucache rm` the file after one). Without logical names the two are two
  URLs and two entries: a file read both ways is cached twice. When the
  prefixes are turned off again, a local URL whose entry another URL filled
  with another modification time (an EOS mount and EOS's servers report one
  file's time about 2 s apart) is read under its own URL while they stay off,
  and is one entry with the others again once they are set.
- Failover never applies to a local file (there is one copy): no server is
  asked anything, and `tried=` in its URL is ignored.
- A stat, a directory listing and a locate on the host (`xrdfs root://local
  ls /dir`, a wildcard in a `TChain`, uproot's own size and listing calls) are
  answered from the file system too; a recursive listing is not supported, and
  every other request on the host (`xrdfs ... rm`, `mkdir`, `query checksum`)
  is refused. So `xrdcp --cksum` gets no checksum for a `root://local` source:
  check a copy against the plain path (`xrdadler32 /abs/path`).
- Everything else is as for any file: copies (`xrdcp`, `hadd`, `rootcp`) read
  the file directly and do not fill the cache ("Copies are the origin's bytes"
  below), a job that reads most of a file reads it directly
  (`max_read_fraction`), and `ucache ls`, `ucache rm` and `ucache pin` take the
  URL as written. A write open writes the file itself, with nothing cached.
- uCache is a per-user cache: keep the cache directory your own. The bytes it
  holds are readable by whoever can read that directory, whatever the
  permissions of the files they came from.

**Put the cache on local disk.** `dir` has **no default** — an unset cache
location is a loud condition (`doctor` FAILs, the plugin passes through
uncached), never a silent cache in your home directory, which on CERN
machines is AFS/NFS. Point it at a local filesystem (`/data/$USER/…`, local
scratch, `/tmp/$USER/…`); `doctor`'s sparse-files WARN flags network
filesystems that slipped through.

**Reliable reads under a flaky/loaded/WAN remote.** This is the default: once
an entry has been validated, reads within the 7-day freshness window are
served **entirely from local disk — the origin is never opened or statted**,
and is contacted only if a genuine cache miss needs new bytes. Repeated
analysis passes are independent of remote state (the origin can be down and
warm reads still complete). Right for write-once physics data; a file changed
in place at the origin isn't noticed until the window lapses — set
`revalidate_seconds = 0` (re-check every open) if that ever matters for yours.

**Reliable opens under a flaky/loaded remote.** Some servers intermittently fail
individual file *opens* under load or over the WAN (a transient error — the same
file opens on retry), and ROOT aborts the whole job on any single open failure, so
a many-file job's success probability collapses with scale. Set
`UCACHE_OPEN_RETRIES` (e.g. `3`) and the plugin retries a *transient* open failure
with exponential full-jitter backoff (`UCACHE_OPEN_RETRY_BASE_MS`=200,
`UCACHE_OPEN_RETRY_MAX_MS`=5000 cap) instead of letting it abort — genuine errors
(missing or forbidden files) still fail fast. This rescues the initial (cold)
fill; `UCACHE_REVALIDATE_S` then carries later warm passes with no remote contact.
Off by default; for a large job over a flaky remote, 2–3 retries makes a transient
per-open failure negligible.

**What a server learns about you.** An XRootD client names itself when it
opens a session: the program doing the reading (`root.exe`, `python3`,
`cmsRun`) and a free-form information string, both carried inside the login
request. With a cache in front, the requests a server actually sees are the
cache's — page-aligned, never asking twice for the same bytes, and absent
altogether on a warm pass — so uCache puts its own name there and keeps your
program beside it:

```
application   ucache
information   ucache/1.0.0 (root.exe)
```

Sites use this to see how much of their traffic already comes through a cache,
which is worth their knowing and costs you nothing: the two strings travel in
the login that opens a session, so nothing is added per file and no extra
request is made, and they are the only thing that changes — what uCache asks
for and how your job behaves are untouched. Nothing about your data, your
paths or your identity is in them; your username and host reach the server
anyway, as they do without uCache.

Turn it off with `announce = off` and your program is named as before. A name
you set yourself always wins: export `XRD_APPNAME` or `XRD_MONINFO` and uCache
leaves that field alone. And uCache stays quiet whenever it is not actually in
the data path — `disable = true`, or no `dir`, or a copy tool (`xrdcp`,
`xrdfs`, `xrdadler32`, `edmCopyUtil`), whose reads are all copies — because
those reads really are your program's own, which also keeps a
`UCACHE_DISABLE=1` baseline run looking like exactly what it is.

**Copies are the origin's bytes.** uCache can show a reader a file in a layout
of its own (a replica, see below): the same data to ROOT, but a different file.
A copy is meant to be the file, so a copy never goes through the cache. These
are recognised and read straight from the origin, and never enter or use the
cache: `xrdcp` (and `xrdcopy`), `xrdfs cat`/`tail`, `xrdadler32`,
`edmCopyUtil`, XRootD's copy engine from any program or language (Python's
`XRootD.client.CopyProcess`, `gfal-copy`, `rucio download`), ROOT's
`TFile::Cp`, and ROOT's own tools that merge, copy or report on files: `hadd`,
the command-line tools (`rootcp`, `rootmv`, `rooteventselector`,
`rootslimtree`, `rootls`, `rootprint`, ...) and `TFileMerger` given a file's
name. Only the copy's own file handle is affected: a program that reads
a file and also copies one keeps its reads cached. A checksum asked for at the
end of a copy (`xrdcp --cksum`, `gfal-copy -K`) is the origin's. Because a copy
reads the origin, it needs the origin to be reachable, even for a file that is
cached.

A file with a replica is also recognised by how it is read. A handle whose
first request reads the whole file at once (fsspec's `open().read()`,
`cat_file(path, 0, size)`) is a copy, and gets the origin's bytes. A handle that
reads the file in pieces up to the size the origin reports, without first
reading any of the replica's own part, is stopped with an error at the piece
that would complete it, naming the remedy: fsspec reading block by block, a loop
sized by the file system's `stat`, whose result would be the first part of the
replica's layout -- not a valid file -- and also fsspec's `get`, which reads
pieces until the end and cannot be told from them at that piece.
`copies_refused` in `ucache stats` counts these.

A copy made any other way reads through the cache like any reader, and for a
file with a replica it copies what it is shown. The whole layout, read in one
request (`cat_file(path, None, None)`, `read_bytes`) or by a loop sized by the
handle's own `stat`, is a valid file but not the origin's. Pieces of one copy
spread over several handles or processes (a download in parallel segments,
workers each copying a part, pieces on a handle an earlier reader in the same
process used) are not seen together, and come out corrupt. The same goes for
fast cloning inside a program of your own (`CloneTree(-1, "fast")` on a tree it
opened) and copies through an XRootD proxy or a FUSE mount with uCache inside
it. Make those copies with the cache switched off: `UCACHE_DISABLE=1 …`. The same holds for
inspecting a file's real form from your own session: `TTree::Print`,
`TFile::Map` or the `RNTupleInspector` report the sizes and codecs of the layout
they are shown. Each handle recognised as a copy is counted in
`copier_handles` (`ucache stats`); `copy_detect = off` turns the recognition
off.

**Jobs that read most of every file are read, not cached.** uCache is built for
analyses that read part of each file, again and again. A job that reads most
of every file — a skim that writes out nearly every branch, a format
conversion — would fill the cache with the whole dataset and push out what
your analyses use, for little gain; if you have the space for the whole
dataset, a local copy serves such a job better. So when a job's reads of a ROOT
file (a TTree or RNTuple of 64 MiB or more) cover branches holding more than
`max_read_fraction` of the file's data (default 25%), the job reads that file
straight from the origin:

- what it fetches for the file's data is not kept. Bytes already in the cache
  are still served, and the file's own records (header, keys list, tree
  metadata) are cached as usual;
- the decision is taken once per file per job, early — as soon as the branches
  read so far hold more than the limit, or once the job comes back to branches
  it has already read — and is not revisited;
- one warning per job names the first such file. `ucache stats` counts them
  (`direct_read_files`, `direct_read_bytes`), and their bytes show as direct,
  like pass-through.

To cache such a job anyway — decompression is what slows it and a replica
would help, or you want its second pass warm — raise the limit for that job,
`UCACHE_MAX_READ_FRACTION=100 ./my_job`, or for every job with
`ucache set max_read_fraction 100`. Files that are not a TTree or RNTuple are
always cached, and so are files under 64 MiB unless the limit is `0`.

**`max_read_fraction = 0`: read through uCache, cache no data.** With `0`,
every ROOT file with a TTree or RNTuple, whatever its size and whatever the
job reads, is read straight from the origin. uCache stays in the data path, so
everything it does for the reads themselves still applies — with
failover on, the move of a failed open or read to another site,
the checks of what the origin sends, and the memory of sites that failed — but
none of the file's data is kept. Use it to have those for jobs whose data you
do not want on the local disk; `UCACHE_MAX_READ_FRACTION=0 ./my_job` does it
for one job.

- It needs a build with the codec libraries (Install): uCache reads the file's
  records with them. A build without them caches every file, whatever the
  value; `ucache doctor` warns, and so does each job.
- The decision is made once uCache has read the file's own records, at the
  job's first read of the file that needs the origin; until then what the job
  fetches is not kept either. When a read of those records from the origin
  fails, the job's reads go on directly, nothing kept, and the records are
  read again by a later read, a second later at the earliest. A file that is
  not a ROOT file, or a ROOT file with neither a TTree nor an RNTuple, is
  cached as usual. A ROOT file whose records uCache cannot map — its tree's record fails its own
  check, as on a damaged copy (ROOT cannot read the tree from it either), or
  uses a layout uCache's reader does not support — is read directly whole:
  nothing the job fetches of it is kept (only what uCache read of its records
  to judge it), from that copy or, after a move with failover on, from any
  other. The decision holds for the file for the life of the
  process.
- What is read directly is the data of the file's main tree (the one named
  `Events`, else its first TTree) or of its RNTuple. The file's other trees
  (in a NanoAOD file, for example, `Runs` and `ParameterSets`) are cached as
  its records are.
- What is still written to the cache directory: the pages that hold the file's
  own records (header, keys list, tree metadata, the other trees; for an
  RNTuple file its first 128 KiB too) and the bytes failover's
  checks at open read, so that the next job does not fetch them again; the
  entry's small files beside them (`.meta`, `.cost`, FORMAT.md); the run
  records under `stats/` that `ucache stats` reads; and, with
  failover on, the marks of servers that failed (`sources`).
- Bytes already cached are still served: a file cached before you set `0`
  is read from the cache as before (`ucache rm` removes it). A replica made
  earlier is served in its layout, and no file gets a new one (`recompress =
  on` makes none).
- The checks of what the origin sends judge a basket only when the bytes
  fetched hold it whole. A single read of a file read directly is passed to
  the origin exactly as the job asks it, not rounded out to the cache's pages,
  so a read that starts inside a basket is served as the origin sends it,
  unchecked — as a plain client would get it. ROOT and uproot read whole
  baskets, so their reads are checked.
- `ucache stats` counts each such file in `direct_read_files` and its bytes in
  `direct_read_bytes` and as direct, like pass-through. There is no warning per
  job: you asked for it.
- A process keeps its decisions for up to 200000 files; a file it opens past
  that is cached as usual (a long-lived process such as a proxy can reach it).

**Eviction is on by default.** With no `max_bytes` set, the cache uses the disk
freely and evicts least-recently-used entries only to keep a free-space floor
(`min(50 GiB, 10% of the volume)` free) — so a heavy session fills most of the
disk and reclaims under pressure. Set `max_bytes` for a fixed-size cache instead.
Protect a hot dataset from eviction with `ucache pin <url>`. To reclaim space
yourself — by age, by size, per file, or all at once — see the cleanup commands
below and the dedicated guide in `docs/CACHE_MANAGEMENT.md`.

## CLI reference

| command            | what it does |
|--------------------|--------------|
| `ucache --version` (`-V`) | print the version and the build id, then exit. The build id identifies the revision this binary was built from (`v0.18.3`, or `v0.18.3-4-g1a2b3c-dirty` for a local build with edits); it also appears in every benchmark record, so a measurement can be traced back to a binary. A build id equal to the bare version means the build could not read its own revision |
| `ucache setup [--host H] [--dir PATH]` | write the single conf file (activation + settings, cache dir explicit) to `~/.xrootd/client.plugins.d` |
| `ucache doctor`    | check install, filesystem (sparse/flock), and activation |
| `ucache test <url>` | end-to-end self-test: cold + warm whole-file read via xrdcp against your setup as-is (its copies run with copy detection and recompression off and `max_read_fraction` at 100, so they exercise the byte cache); warm must be origin-free; cleans up the entry it created (pre-existing entries kept) |
| `ucache enable` / `disable` | turn caching on/off (flips the conf) |
| `ucache summary [--detail] [--json]` | **overall performance across every recorded run, and the time the cache has saved (or cost) you, measured against a no-cache baseline** — record one by running your job once with `UCACHE_DISABLE=1`. Refused with a reason rather than qualified when the comparison would not be sound. `--detail` adds the last run: tier split, delivered rate, per-tier read counts and sizes, replica coverage |
| `ucache history [--top N] [--json]` | an **ALL** row aggregating every recorded run, then one row per run, newest first — whether the numbers are holding up across runs, versions and machines |
| `ucache status`    | cache location, budget, usage, aggregate stats |
| `ucache ls [--sort age\|size] [-l]` | list cached entries (size, cached, coverage, last-used age, replica, pinned); `-l` lists each entry's other names (the URLs that reached an entry with a logical name) |
| `ucache stats`     | aggregate `stats/*.jsonl` across all processes, plus the derived **workflow picture**: opens per distinct file, bytes served per tier (direct / fill / RAM / disk / replica), disk-read count + mean size + sequential share, re-read factor, fill flush shape, and p50/p95/p99 latencies |
| `ucache stats --files [--top N]` | per-file records (one per file per process), costliest first: which files were re-opened, re-read, served from which tier; under a file with a logical name, the URLs its entry was reached through |
| `ucache stats --reset` | start a fresh counter window for measuring **one** run against a specific cache state (run it between jobs — it warns if a file looks live). The counter and per-file records MOVE to `stats/history`, where `summary` and `history` keep reading them: deleting them would delete any measured no-cache baseline, which is the one thing later runs are compared against. Traces are deleted (bulky, per-op, no run-level meaning). The cache contents are untouched |
| `ucache bench --threads N [PATH …] [--size SZ] [--measurement-duration S] [--block KB] [--fill writers=N,block=SZ] [--sweep] [--cache-path [--cache-sample SZ]] [--log FILE\|--no-log]` | storage self-test of the cache dir (or of candidate dirs, to pick one). Three groups: **standard** measures at pinned block sizes and queue depths 1/16/32, comparable to a datasheet; **pattern** measures at your job's concurrency — each serving tier's read shape, and random reads under writeback; and, with `--cache-path`, the same storage **through uCache's own fill and read code** rather than an imitation of it. Plus fsync, create/unlink. `--threads` is **required and never guessed** — it is what your analyses run at, not the core count. `--measurement-duration` is the window of ONE measurement (the build stage gets 3×; the plan and estimated total print up front). Appends every run, with the machine, load and block device behind the path, to `./ucache-bench.txt`. `--publish` also sends the record — path and mount point replaced by salted hashes, hostname blanked — and prints the report URL (`docs/PUBLISH.md`). **Full guide: `docs/BENCH.md`** |
| `ucache netbench <root://…> [--streams N,…] [--block KB] [--seconds S] [--publish]` | origin random-read baseline at 1/16/64 streams: what the network side delivers, for a fair cache-vs-origin comparison. `--publish` sends the record (hostname blanked, URL reduced to its domain) for a report |
| `ucache publish [--label TEXT] [--dry-run] [--yes] [--url URL] [--runs N]` | send this cache's run history (the newest 200 runs, or `--runs N`), its disk's benchmark records (and any taken on the same volume) and this machine's origin measurements to the report service and print the report URL with its recommendations. **Paths, hostnames and file names never leave the machine** — `docs/PUBLISH.md` lists every field. `--dry-run` prints the exact payload instead; `--yes` for scripts (a non-terminal without it is refused). `bench --publish` and `netbench --publish` send one record the same way |
| `ucache identity [--set STRING \| --new \| --path]` | the identity string that groups everything you publish under one owner page: an owner id plus a salt that never leaves the machine. Created by the first publish; `--set` installs it on another machine, `--new` starts over |
| `ucache evict [--older-than DUR \| --newer-than DUR \| --to-size SIZE] [--dry-run]` | reclaim space: no flags = one pass to the configured budget; `--older-than 30d` drops entries unused that long; `--newer-than 1h` drops entries used within the window (undo a polluting run); `--to-size 20g` LRU-evicts down to a total size; `--dry-run` previews |
| `ucache rm <url> [url…]` | remove specific entries (byte cache + replica) |
| `ucache clear [--yes] [--keep-pinned] [--wait]` | empty the whole cache (prompts unless `--yes`). The cache is empty at once; the disk space is freed in the background, or before the command returns with `--wait` |
| `ucache pin <url>` / `unpin <url>` | protect / unprotect an entry from eviction |
| `ucache verify <url>` | CRC-scrub an entry; quarantine (not wipe) bad pages |
| `ucache settings`  | every setting: effective value + where it comes from (default \| conf \| state \| env) |
| `ucache set <key> <value>` / `unset <key>` | change / drop a **current** value without touching your defaults in the conf |
| `ucache recompress [--jobs N] [--yes]` | convert what is cached of the files whose source codec is in `recompress_codecs` into their replicas — the same form a first pass with `recompress = on` makes — in the foreground with live progress (`--jobs` default: every core the process may use). Estimates the disk growth first; a file that would not fit above the free-space floor is deferred, never evicted around |
| `ucache branches <url>` | which branches your analysis read: branches whose every basket is cached or converted into the file's store (`stored`), with bytes + source codec, and the summary share |
| `ucache untranspose <url>` | drop an entry's replica; the byte cache is kept |
| `ucache sources` | with failover on: the servers and sites this cache remembers as failed (their marks and how long each holds) and the copies proven missing, which are tried after the others (TROUBLESHOOTING.md) |

**Recompression (decompress-once replicas).** Tightly compressed (LZMA)
ntuples spend most of a *warm* analysis just decompressing. uCache can rebuild
the branches you actually read into replicas re-encoded once as ZSTD-1 — an
order of magnitude cheaper to decode — and serve them transparently (same
results, bit-identical). One switch controls it:

```sh
ucache set recompress on   # or `recompress = on` in ucache.conf as your default
```

With it on, a file that has no replica yet gets one **on its first pass**,
TTree and RNTuple alike. uCache fetches what your job asks for, as it would
anyway, converts each basket or page to ZSTD-1 as it arrives, and keeps the
converted records in the file's replica as it goes; the next run reads them. No
command is needed. For a TTree it also fetches, with them, the rest of each
branch's baskets in the clusters your job touched (`complete_chunks = on`):
ROOT's read cache reads a cluster's baskets together, and in the replica's
layout it can read a few baskets ahead that it skipped on the first pass, which
a later pass would otherwise fetch from the origin (with no network, such a read
would wait for the connection and then fail). It fetches only baskets of the
branches and clusters your job touched, none of them twice: one your job reads
later is already there. That first pass costs
CPU — everything it reads is converted — so on a machine short of cores it can
be slower than a pass with recompression off. It also goes no faster than the
converted records can be written to the cache disk: when writing falls behind
(a slow disk, or a system that limits how much unwritten data the kernel may
hold, `vm.dirty_bytes`), the reads that would convert more wait for it, for at
most a minute each, so that everything converted is kept and the next pass
reads it from the replica. When the cache has no room for what was converted
(the free-space floor) or a write fails, nothing waits: what is converted is
served and not kept, and a later pass converts it again, until a write keeps
its records again (as soon as there is room).

**It also costs memory, on every pass that reads such a file.** ROOT sizes its
read buffers from the layout described below and does not shrink them to fit
the memory there is, and uCache keeps a table for each such file while it is
open (a few MB for a large NanoAOD file). On the analyses measured (64
threads), warm passes needed about 1.4x the memory on TTree and 1.3x on RNTuple
of the same job with no cache, however the file was recompressed. A job that
runs short of memory is killed by the system, not warned: if yours are near
their memory limit, leave recompression off and use the byte cache alone.
Turning it off later does not change files already recompressed this way; `docs/TROUBLESHOOTING.md` ("Jobs are killed for memory") says how to get
them back. `ucache doctor` repeats this note whenever recompression is on.

While a file is recompressed this way your jobs see it in a layout of its own,
in which every basket has room to be converted: the same data and the same
results, in a larger file. That layout is the file's for good — every process,
every open — so a job can close and reopen a file at any point, several
processes can read different parts of one file at once and build one replica
between them, and a job that ends abruptly loses at most the last few seconds
of conversion. Branches a later job reads for the first time are converted as
it reads them. Two consequences:

- A copy is still the origin's file: `xrdcp`, `xrdfs`, `xrdadler32`, XRootD's
  copy engine from any language (`gfal-copy`, `rucio download`, Python's
  `CopyProcess`), ROOT's `TFile::Cp`, `hadd` and ROOT's command-line tools are
  recognised and read straight from the origin (see "Copies are the origin's
  bytes" above). A copy made any other way — a read loop, fsspec's `get`, fast
  cloning in your own program — gets the larger layout: make it with
  `UCACHE_DISABLE=1`.
- Do not share a cache directory with uCache 1.2.0 or older. It does not know
  these replicas: it serves such files from the byte cache and the origin
  (correctly, but slowly) and can leave the replicas behind when it evicts.

**From the next pass on, a file is read at its real size.** Each open is
shown the file's newest *map*: everything converted by then stated back to
back at the size of its converted record, in an address range of the map's
own past the first-pass layout, so ROOT reads and holds just the records; the
rest stay where the first-pass layout has them. A TTree basket is stated as
converted; an RNTuple page as stored (ZSTD-1, or uncompressed where that is no
smaller), which ROOT decodes itself, and a column range only once all its
pages are converted. A map is made a moment after a process is
done with a file, once enough has been converted since the last one (a
twentieth of what the file's replica holds, and after the first map 8 MiB) and
nobody else is converting the file; after the first, at most one an hour. Baskets converted
after the newest map are read at the size of their room until the next one.
A handle keeps the map it was shown for its life, whatever is made after it,
and a replaced map keeps its range while any process's handle holds it and for
`in_use_seconds` (1 day) after it was last used, for readers still holding its
positions. After that a read in its range fails, with an error that says so:
reopening the file shows the current map. At most four maps are in use at once;
when a fifth would be due, new opens are shown the first-pass layout, which
states everything converted (`ucache stats` counts it). `mixed_maps = off` shows every open
the plain layout: use it when a job hands basket positions to workers on
other machines, each with a cache of its own (`uproot.dask` with remote
workers). The memory figures above were measured before maps existed; they
are expected to come down, and have not been measured again yet.

Only branches whose source codec is in `recompress_codecs` (default
`lzma,zlib`) are converted; recompressing already-fast codecs would waste CPU
and disk. Everything else — other codecs, the file's own records, and the rare
basket whose converted form does not fit — is kept in the byte cache as usual.
What was converted is not ALSO kept in the byte cache, and a copy the byte cache
held from before is released once its basket is converted; set
`recompress_keep_originals = on` if you want both, for instance to compare the
two tiers on one cache. Converted records are held to the cache's limits like
any other cached data, so keeping them can evict least-recently-used entries.
What still does not fit above the free-space floor is not kept (a later read
converts it again), and `ucache stats` counts it as not kept.

`recompress = off` only stops files that have no replica from getting one. A
file that has one keeps being served from it, and what a job reads of it for
the first time is still converted.

A file the first pass cannot recompress is served from the byte cache. The
plugin warns about the first such file of each process, naming the reason;
`ucache stats` counts them as `cold_replica_skipped`, and `ucache status` on
its `declined` line. The commonest reason is the codec: a file stored in a
codec that is not in `recompress_codecs` is left as it is, and changing the
list makes the next read decide again. A file whose compression setting names
no codec (setting 1, "the global default", from older ROOT versions or
`hadd -f1`) is judged by the codec its baskets are actually stored in. For
other reasons (an unusual structure), the warning says so; `ucache recompress`
declines the same files, and they stay in the byte cache.

`ucache recompress` runs one foreground sweep, with live progress, over what is
already in the byte cache: you need it only for data cached before
recompression was switched on (or with it off) and never read since, and for
what a first pass did not keep. It converts into the same replica a first pass
makes — every basket or page whose original is cached and not yet converted —
and makes the file's next map; nothing is read from the origin. A replica an
earlier release made beside the byte cache (`.tdata`/`.tmeta`) is not served
by this one: the sweep removes it and converts what is cached in its place
(the file's next open removes it too, and `ucache status` counts what is
left). Check where you stand with `ucache status` (the `recompressed:`
line) or per file with `ucache ls` (the `RECOMP` column). The sweep's own
summary gives each outcome its own words —
`recompressed`, `declined` (with the codec it found and the one-line fix),
`already recompressed`, `incomplete`, `failed`, and `deferred (no space)` for
files that would not fit above the free-space floor (run it again once there
is room).
`ucache doctor` will tell you why nothing is being built if that is what you are
seeing (see Troubleshooting).

What the sweep converts leaves the byte cache, as on a first pass, unless
`recompress_keep_originals = on`; a basket or page kept as stored stays there,
since it is the only copy, and so do the file's first 128 KiB. The setting
`recompress_reclaim` of earlier releases is retired: the sweep frees every
converted original, which is what its `superseded` did, and its `full` freed
originals the replica still needs.

Set expectations honestly: the replica removes *decompression* time only. A
warm analysis that is 80% LZMA decode gets several× faster; one dominated by
its own compute may gain 10%. One instrumented run with ROOT's
`TTreePerfStats` tells you your ceiling before you spend the disk (measured
1.04–1.22× of the cached bytes on LZMA-9 sources, by container — see
`docs/CACHE_MANAGEMENT.md` §3; `ucache status` totals it, and the originals a
replica converted leave the byte cache, so the data is not stored twice). `ucache
branches <url>` shows exactly which branches you read and their codecs.
Escape hatches: `ucache set transpose off` stops serving replicas; `ucache
untranspose <url>` drops one (a replica made on a first pass is rebuilt from the
origin the next time the file is read).

Stuck? See **`docs/TROUBLESHOOTING.md`**.

---

uCache is MIT-licensed (see `LICENSE`); copyright (c) 2026 Massachusetts
Institute of Technology.
