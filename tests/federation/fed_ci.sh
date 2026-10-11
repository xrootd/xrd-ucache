#!/usr/bin/env bash
# A federation on the loopback interface, and what uCache does in it when a server
# refuses a file: one regional redirector and three data servers, run with the xrootd
# and cmsd programs of the XRootD packages (EPEL's xrootd-server ships both). Needs
# nothing else: no host name, no network address of the machine.
#
#   fed_ci.sh BUILD_DIR      BUILD_DIR holds src/plugin/libXrdClUCache.so and
#                            tests/ucache-plugin-fuzz (and, if built, src/cli/ucache)
#
# Environment: FED_CI_PORT_BASE (default 21100; seven ports from it are used),
# FED_CI_XROOTD_BIN (the directory of xrootd, cmsd and xrdfs; default /usr/bin),
# FED_CI_USER (the account the servers run as when this is run as root; xrootd
# refuses to run as root; default xrdsrv). The servers run only for the length of
# the run; the directory it works in is printed and left.
#
# The three servers answer from the same address, so a redirector cannot be told to
# avoid one of them by name (tried= masks full host names). That is the case the
# regional redirector's locate is for: an open that failed at a server and that the
# redirector sends back to it is made at a server the regional lists for the file.
#
#   sent back              the file is on b and c, and c refuses it; a new file each
#                          time, one open, until the regional sends the retry back to
#                          c. With adaptive_federation on every open succeeds, the
#                          client sends a locate, the regional logs it,
#                          failover_locates counts it, and the open that was sent
#                          back is made at b: an attempt of route candidate
#   empty copy             the file is a ROOT file named *.root, two servers hold it and the third
#                          holds it empty (a, b and c in turn); a new file each time, one open,
#                          until an open landed on the empty copy first.
#                          With adaptive_federation on that copy is refused (a ROOT file is never
#                          empty), the open goes on to a copy with bytes, the reader gets the
#                          file's own bytes, and the records show the empty copy's size 0, a
#                          failed attempt of op open, class data, and the copy that was taken
#   cut short              the file is named *.root, a holds its first bytes (the header, which says
#                          where the file ends, is intact), b holds all of it and c is empty. With
#                          adaptive_federation on, the open that lands on a reads the first 300
#                          bytes of the copy, finds the size it reports smaller than the end the
#                          header states, and goes on to a copy that is whole; the reader is shown
#                          the whole size; the records show the copy's size, a failed attempt of op
#                          open, class data, and the copy that was taken. When only the cut copy
#                          exists, the open fails with one error that names the copy, its size and
#                          the header's end, and uCache has closed the copy. The copy of a file not
#                          named *.root is not looked at;
#                          the name is checked in any case (*.ROOT)
#   header read fails      the header read of a copy fails (UCACHE_TEST_HEADER_FAIL_N, with a
#                          connection error, or _KIND=short, answered short): at every copy the
#                          open fails naming each, at the first copy only the open goes on to
#                          another; either way the reader's open gets exactly one completion,
#                          and uCache closes each copy it opened. A ReadRecovery the reader sets
#                          just as the checks begin is on the File before they turn it off: it
#                          reads false during their reads, and the reader's value after them. A Stat
#                          the reader makes while the checks run waits for the open's answer before
#                          the handle is set up (with every copy cut, it is never answered the cut
#                          size); what the checks read is stored in the entry. A Stat and a Read
#                          made then from another file's callback return at once and are answered
#                          after the open
#   zeros at the size      one server holds as many zero bytes as the file has (a, b and c in
#                          turn), the other two the file: the copy of zeros does not begin as a
#                          ROOT file does and is refused; the open goes on to a whole copy
#   damaged header or list the file is named *.root; one server holds it with its header's first-record
#                          position set past the end of its data, or with its list of top-level objects
#                          zeroed (a, b and c in turn), the other two the file: the open that lands on the
#                          damaged copy reads it (its first 300 bytes, then the list) and refuses it as
#                          ROOT's own reading would (op open, class data, the reason in the record), and
#                          ends at a whole copy; when the zeroed list is the only copy, the open fails with
#                          one error naming it. When the read of the list fails (UCACHE_TEST_HEADER_FAIL_N
#                          with _STAGE=list), that copy is a failed one and the open goes on, with one
#                          completion
#   longer than its header every copy is the file with bytes appended, so its header states an end
#                          before its size (as for a file updated in place, which ROOT reads
#                          normally): the open stands at the copy it landed on, nothing is refused,
#                          the reader gets the file's own bytes and the size is not confirmed
#   all copies empty       the file is named *.root and every copy is empty: the open fails with
#                          one error of class data that names each copy tried with its server and
#                          its size, each attempt recorded as a failure of op open, class data, a
#                          copy the regional lists is not tried when it was refused already, and
#                          uCache closes each copy it opened (the test client never closes its File)
#   all refused            a, b and c refuse the file: Open() returns OK, one
#                          completion, a failure that names all three servers
#   switch off             the default: a refuses the file. On a alone, every open lands
#                          on a and fails; on a, b and c, the opens that landed on a fail
#                          there and none is moved. The client sends no locate and the
#                          regional logs none for the file; the positive controls are the
#                          first leg's client log and an xrdfs locate of the leg's own
#   reads                  a read whose origin read is made to fail (UCACHE_TEST_READ_FAIL_N,
#                          and _KIND for where and how) is read again at another site,
#                          with adaptive_federation on, and answered with the file's own
#                          bytes, once: misses (a Read, then a VectorRead), relayed reads
#                          (a PgRead, a VectorRead into one buffer), reads the client
#                          refuses at once, eight failures of threads at one handle (also
#                          failing late), and a failure no other site would change, which
#                          stays at its server. A read that fails at every copy each move takes
#                          ends when its own attempts are spent (four) with the error that names
#                          every copy it was read from, with its size, and the code of its last
#                          failure. Each case checks the records: the failed
#                          reads, each followed by an open with route read at another
#                          server, and open_failovers equal to those opens. A handle whose
#                          reads are all relayed (the name it was opened by, localhost, denied
#                          the cache) moves too, to a copy of the size its reader's open was
#                          shown, never back to the server its read failed at, and once every
#                          server has failed, to the oldest failure (servers named as IPv6
#                          addresses where localhost is IPv6 first); so does one whose host no allow
#                          list names (the lists, which say which files uCache caches, do not narrow
#                          where the reads of a file they leave out move). With the
#                          switch off the failed reads are relayed on the same file and
#                          the client's read recovery stays on
#   cut after the open     the file is named *.root, its copies are whole, and the copy the handle
#                          was opened at is cut short by the test: a read inside the file's size
#                          past the cut is answered short, which is held against the copy (its size
#                          was confirmed by the header at the open): the read is made again at
#                          another copy and is exact, for a Read the cache serves and for a PgRead it
#                          passes straight to the origin, across the cut or wholly past it (the copy
#                          answers the latter with no bytes, which the client itself fails with an
#                          invalid response; inside the file's size uCache holds that against the copy
#                          too). When every copy is cut the read fails with the
#                          data error, whose text says no copy could serve the read and names each
#                          copy tried with its size; a second read made right after it, within a
#                          second, is not moved again: it gets that move's error at once, which says
#                          that a move made for an earlier read of the handle found no copy and that
#                          the offsets it names are that read's. A read that reaches past the end of an intact
#                          file is answered short as it always was, and the handle does not move.
#                          The handle's cache entry, which holds what the checks at the open read of
#                          the cut copy, is dropped when the copy is found bad (bad_entries_dropped,
#                          no sidecar left), for each of the three reads
#   zeros                  a copy that serves a basket as zeros (the fixture of the key-position
#                          check, tests/data/unnamed_codec_fixture.root): with the switch on the
#                          read fails its check and is made again at another site, for a Read and
#                          for a VectorRead, exactly, once; when the first move lands on a copy
#                          that is damaged too, that answer is refused as well; when every copy
#                          is damaged the reader gets errDataError, after at most four reads and
#                          never the zeros; when the only copy is damaged it is never opened again
#                          for the handle (one failed read, no move takes it), and the error names
#                          it; with the switch off (the check on) it gets the error and no read is
#                          moved
#   another length         the file map (the check on): a copy whose basket's key states another length than
#                          the tree's record gives it (one ROOT would read as stored uncompressed: wrong values,
#                          no error), or whose tree's record fails its own check (one byte of its compressed
#                          data changed: ROOT finds no tree), damaged on the server the handle landed on after
#                          the open: with the switch on the read is refused there and made again at another
#                          site, exactly, once, for a Read and a VectorRead; with it off the read fails its
#                          check and is not moved; each failure's text says which. A tree's record that
#                          fails its own check is held against the reads of that tree's own parts only: in a
#                          file of two trees, a read of the other tree's baskets at that copy is exact and
#                          stays there, a read of the damaged record moves
#   block checksum         a basket whose compressed data fails its own checksum (one byte changed inside a
#                          ZLIB basket's data, its key intact), on the server the handle landed on after the
#                          open: with the switch on the read is refused there and made again at another site,
#                          exactly, once, for a Read, a VectorRead and a PgRead (which the cache relays to the
#                          origin), and the entry holding the copy's bytes is dropped; with it off the read
#                          fails its check and is not moved
#   lost trust, open fails a handle opened trusting its cache entry (a freshness window) whose entry is
#                          gone by its first read, and whose own open then fails at every copy (each
#                          refuses the file): a Read and a PgRead each fail with that open's error,
#                          which names the copies, and neither is given to the File that never
#                          opened (neither gets the client's own answer there, "Invalid operation")
#                          With the switch off (the default) each read gets that open's own error, the
#                          server's refusal, naming no copies, and never "Invalid operation"; once the
#                          copies are readable again, a later read (after the open's cool-down) is exact.
#                          With the switch on, such a handle's own open makes the checks made at open
#                          (every copy cut: a Stat and a Read fail naming the copies; every copy whole:
#                          both are right), and an Fcntl or a Visa before the first read sets the handle
#                          up first, so that its own open finds the entry's size (another version at
#                          every copy: a Read the entry does not hold fails, never that version's bytes)
#   left copy              a read's data failure (a short answer inside the confirmed size) that
#                          arrives after the handle moved, on the File it left: that copy is one the
#                          handle never opens again, as for a failure on the File it reads from
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD=$(cd "${1:?usage: fed_ci.sh BUILD_DIR}" && pwd)
LIB="$BUILD/src/plugin/libXrdClUCache.so"; FUZZ="$BUILD/tests/ucache-plugin-fuzz"
[ -f "$LIB" ] && [ -x "$FUZZ" ] || { echo "fed_ci: $BUILD has no plugin or no ucache-plugin-fuzz" >&2; exit 2; }
XBIN=${FED_CI_XROOTD_BIN:-/usr/bin}
for p in xrootd cmsd xrdfs xrdcp; do
  [ -x "$XBIN/$p" ] || { echo "fed_ci: no $XBIN/$p (EPEL's xrootd-server and xrootd-client)" >&2; exit 2; }
done
SRVUSER=${FED_CI_USER:-xrdsrv}
as_srv() { if [ "$(id -u)" = 0 ]; then runuser -u "$SRVUSER" -- "$@"; else "$@"; fi; }
# The XRootD programs of the packages, without this shell's library path or plugins.
host() { env -u LD_LIBRARY_PATH -u XRD_PLUGINCONFDIR -u XRD_PLUGIN "$@"; }

BASE=${FED_CI_PORT_BASE:-21100}
RX=$BASE RC=$((BASE + 1)) AX=$((BASE + 21)) BX=$((BASE + 22)) CX=$((BASE + 23))
W=$(mktemp -d "${TMPDIR:-/tmp}/fed-ci-XXXXXX"); chmod 755 "$W"
cd "$W" || exit 2 # the servers' runtime directories stay here, not in the caller's
echo "fed_ci: working in $W (ports $BASE..$((BASE + 23)))"
FAILS=0
verdict() { if [ "$2" = 0 ]; then echo "PASS: $1"; else echo "FAIL: $1"; FAILS=$((FAILS + 1)); fi; }
# A label with a command substitution in it is expanded before the $? that follows it, so that
# verdict would test the substitution, not the check: the caller captures r=$? first. The script
# reads its own source for the mistake, by its full path (the working directory has changed), and
# fails when it cannot read it.
SELF="$HERE/$(basename "${BASH_SOURCE[0]}")"
if [ -r "$SELF" ]; then
  lint=$(grep -nE '(^|[ ;])verdict ".*\$\([^(].*" \$\?[[:space:]]*$' "$SELF" | grep -v 'lint=')
else
  lint="cannot read $SELF"
fi
[ -z "$lint" ] || echo "$lint"
[ -z "$lint" ]; verdict "lint: no verdict reads \$? after a command substitution ($SELF)" $?

cfg() { # name role port [cms-port]
  local n=$1 role=$2 port=$3 cport=${4:-}
  install -d -m 777 "$W/$n" "$W/$n/admin" "$W/data/$n"
  {
    echo "all.role $role"
    echo "all.manager 127.0.0.1:$RC"
    echo "xrd.port $port if exec xrootd"
    [ -z "$cport" ] || echo "xrd.port $cport if exec cmsd"
    echo "all.export /store r/o nolock"
    echo "oss.localroot $W/data/$n"
    echo "oss.defaults nomig nodread nolock r/o"
    echo "all.adminpath $W/$n/admin"
    echo "all.pidpath $W/$n"
    echo "xrd.allow host localhost"
    echo "cms.allow host localhost"
    echo "cms.delay startup 2 lookup 1"
    echo "xrootd.trace emsg login redirect fs" # fs: the regional's log shows every locate
    echo "xrootd.async nosf" # no sendfile: a copy cut under an open file then answers short
  } > "$W/$n.cfg"
}
cfg regional manager $RX $RC; cfg a server $AX; cfg b server $BX; cfg c server $CX
start() { # node prog
  as_srv env -u LD_LIBRARY_PATH -u XRD_PLUGINCONFDIR -u XRD_PLUGIN "$XBIN/$2" -b -n "fedci-$1" \
    -l "$W/$1/$2.log" -s "$W/$1/$2.pid" -c "$W/$1.cfg" >/dev/null 2>&1
}
stop_all() { # only the processes whose pidfiles are ours
  local f p
  for f in "$W"/*/xrootd.pid "$W"/*/cmsd.pid; do
    [ -s "$f" ] || continue
    p=$(tr -dc 0-9 < "$f"); [ -n "$p" ] && kill "$p" 2>/dev/null
  done
}
trap stop_all EXIT
trap 'exit 130' INT; trap 'exit 143' TERM

# the data: a file of random bytes, on the servers named
head -c 1048576 /dev/urandom > "$W/f.bin"; chmod 644 "$W/f.bin"
place() { # LFN server... (a copy the server's xrootd and cmsd both see)
  local lfn=$1 s; shift
  for s in "$@"; do
    install -d -m 777 "$W/data/$s/$(dirname "$lfn" | sed 's|^/||')"
    cp "$W/f.bin" "$W/data/$s$lfn"; chmod 666 "$W/data/$s$lfn"
  done
}
place /store/alive a b c
for n in regional a b c; do start $n xrootd; start $n cmsd; done
xfs() { host timeout 30 "$XBIN/xrdfs" "$@"; }
port_of() { case $1 in a) echo $AX ;; b) echo $BX ;; c) echo $CX ;; esac; }
# locate at the regional until it lists the three servers for the sentinel
up=0
for i in $(seq 60); do
  out=$(xfs 127.0.0.1:$RX locate -r /store/alive 2>&1)
  if echo "$out" | grep -q ":$AX " && echo "$out" | grep -q ":$BX " && echo "$out" | grep -q ":$CX "; then up=1; break; fi
  sleep 0.5
done
[ "$up" = 1 ]; verdict "the regional lists a, b and c" $?
if [ "$up" != 1 ]; then echo "$out"; tail -20 "$W/regional/cmsd.log"; exit 1; fi
REGLOG=$(ls "$W"/regional/*/xrootd.log "$W"/regional/xrootd.log 2>/dev/null | head -1)
[ -s "$REGLOG" ]; verdict "the regional's log exists ($REGLOG)" $?

mkdir -p "$W/conf"
# localhost:$RX too: the regional under another name than the address its servers answer from
printf 'url = 127.0.0.1:%s;127.0.0.1:%s;127.0.0.1:%s;127.0.0.1:%s;localhost:%s\nlib = %s\nenable = true\n' \
  $RX $AX $BX $CX $RX "$LIB" > "$W/conf/u.conf"
direct_read() { # server lfn -> 0 when a read at the server's own port succeeds
  host timeout 30 "$XBIN/xrdcp" -f -s "root://127.0.0.1:$(port_of "$1")/$2" /dev/null >/dev/null 2>&1
}
# the plugin's records: counters and lines of one cache directory
counter() { cat "$1"/stats/*.jsonl 2>/dev/null | grep -v '"key"' | grep -o "\"$2\":[0-9]*" | grep -o '[0-9]*$' | sort -n | tail -1; }
records() { cat "$1"/stats/*.files.jsonl 2>/dev/null | grep '"kind":"open"'; }
# fuzz CACHE LOG ARGS... : the plugin's test client, with the plugin bound to the federation
fuzz() {
  local cache=$1 log=$2; shift 2
  mkdir -p "$cache"
  env XRD_PLUGINCONFDIR="$W/conf" UCACHE_DIR="$cache" UCACHE_REVALIDATE_S=0 UCACHE_THREADS=8 \
      XRD_LOGLEVEL=Debug XRD_LOGFILE="$log" "${FEDENV[@]}" timeout 300 "$FUZZ" "$@" 2>&1
}
regional_locates() { grep -c "Xrootd_Protocol: locate .*$1\$" "$REGLOG"; }

echo; echo "=== an open sent back to the server that refused it"
# The file is on b and c, and c refuses it. A new file each time, one open: when the
# regional's first answer is c, it sends the retry to c again (a name that cannot be
# masked), and the open goes on to a server the regional lists. Up to 120 files (about 7 are
# needed on average, and the leg stops at the first).
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on)
got=0 allok=1
for i in $(seq 120); do
  L=/store/ci/f5-$i.bin
  place $L b c
  chmod 000 "$W/data/c$L"
  res=$(fuzz "$W/c5-$i" "$W/f5-$i.log" "root://127.0.0.1:$RX/$L" "$W/f.bin" 1 1 1 open-failover)
  echo "$res" | grep -q '^open-failover: clean$' || { allok=0; echo "$res" | sed 's/^/    /'; break; }
  if [ "$(records "$W/c5-$i" | grep -c '"route":"candidate","status":"ok"')" -ge 1 ]; then got=$i; break; fi
done
[ "$allok" = 1 ]; verdict "every open succeeded, one completion, right bytes" $?
[ "$got" != 0 ]; verdict "an open was sent back to c and went to a listed server (file $got of at most 120)" $?
if [ "$got" != 0 ]; then
  records "$W/c5-$got" | sed 's/^/    rec: /' | cut -c1-230
  direct_read c $L; r=$?; [ "$r" != 0 ]; verdict "c refuses the file (a direct read fails)" $?
  NL=$(grep 'MsgHandler created' "$W/f5-$got.log" | grep -c 'kXR_locate ')
  [ "$NL" -ge 1 ]; verdict "the client's log shows kXR_locate ($NL)" $?
  [ "$(regional_locates $L)" -ge "$NL" ]; r=$?; verdict "the regional's log shows the locate ($(regional_locates $L) requests)" $r
  FL=$(counter "$W/c5-$got" failover_locates)
  [ "${FL:-0}" = "$NL" ]; verdict "failover_locates is ${FL:-0}, the locates the client sent ($NL)" $?
  records "$W/c5-$got" | tail -1 | grep -q '"outcome":"ok"'; verdict "the last record line says outcome ok" $?
fi
L5=$L

# A ROOT file for the header check's cases: its header states its size.
FIXW="$HERE/../data/unnamed_codec_wide_fixture.root" # 106379 bytes; its header says so
FIXW_SIZE=$(stat -c %s "$FIXW"); CUT=70000
[ "$(head -c 4 "$FIXW")" = root ] && [ "$FIXW_SIZE" -gt "$CUT" ]; verdict "the fixture is a ROOT file of $FIXW_SIZE bytes, longer than the cut ($CUT)" $?
copyw() { # SRC LFN server... (SRC on each)
  local src=$1 lfn=$2 s; shift 2
  for s in "$@"; do install -d -m 777 "$W/data/$s/$(dirname "$lfn" | sed 's|^/||')"; cp "$src" "$W/data/$s$lfn"; chmod 666 "$W/data/$s$lfn"; done
}
placew() { local lfn=$1; shift; copyw "$FIXW" "$lfn" "$@"; } # LFN server... (the whole fixture)
head -c $CUT "$FIXW" > "$W/fixw-cut.bin"
cutw() { local lfn=$1; shift; copyw "$W/fixw-cut.bin" "$lfn" "$@"; } # LFN server... (its first $CUT bytes)
emptyw() { # LFN server... (an empty copy)
  local lfn=$1 s; shift
  for s in "$@"; do install -d -m 777 "$W/data/$s/$(dirname "$lfn" | sed 's|^/||')"; : > "$W/data/$s$lfn"; chmod 666 "$W/data/$s$lfn"; done
}
# the header check's reads at 0 in a client's log: its first 300 bytes; for a file named *.root that the cache
# caches, the check reads the first page whole (4096 bytes) and the cache keeps it for the reader. (The key-position
# check's parse reads that page too, but from the page kept, so in a log of such a file every read of 4096 bytes at
# 0 is the header check's.)
nheader() {
  if grep -qiE 'kXR_open \(file: [^,?]*\.root[,?]' "$1"; then
    grep -cE 'MsgHandler created.*kXR_read \(handle: 0x[0-9a-f]*, offset: 0, size: (300|4096)\)' "$1"
  else
    grep -c 'MsgHandler created.*kXR_read (handle: 0x[0-9a-f]*, offset: 0, size: 300)' "$1"
  fi
}
# bit 2 of a sidecar's flags (offset 12): the size confirmed; "none" without a sidecar
confirmed() {
  local m; m=$(find "$1" -name '*.meta' | head -1)
  if [ -z "$m" ]; then echo none; else echo $(( ($(od -An -tu4 -j12 -N4 "$m") >> 2) & 1 )); fi
}
# In a client log, the Files that opened a copy (each by the name XrdCl's log gives it, its
# address and URL) and how many of them sent their own close: "OPENED CLOSED". The test client's
# open-denied mode never closes or destroys its File, so every close there is uCache's.
copies_closed() {
  local o c
  o=$(sed -n 's/.*\[File *\] \[\(0x[0-9a-f]*@.*\)\] successfully opened at .*/\1/p' "$1" | sort -u)
  c=$(sed -n 's/.*\[File *\] \[\(0x[0-9a-f]*@.*\)\] Sending a close command for handle .*/\1/p' "$1" | sort -u)
  echo "$(echo "$o" | grep -c .) $(comm -12 <(echo "$o") <(echo "$c") | grep -c .)"
}

echo; echo "=== an empty copy of a file named *.root is refused"
# Up to 60 files, the empty copy on a, b and c in turn (the regional may prefer a server when all
# three hold the file), until an open landed on it first.
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on)
others() { case $1 in a) echo b c ;; b) echo a c ;; c) echo a b ;; esac; }
got=0 allok=1
for i in $(seq 60); do
  L=/store/ci/ec-$i.root
  e=$(echo a b c | cut -d' ' -f$((i % 3 + 1)))
  placew $L $(others $e); emptyw $L $e
  res=$(fuzz "$W/c11-$i" "$W/f11-$i.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-failover)
  echo "$res" | grep -q '^open-failover: clean$' || { allok=0; echo "$res" | sed 's/^/    /'; break; }
  if records "$W/c11-$i" | head -1 | grep -q '"size":0,'; then got=$i; break; fi
done
[ "$allok" = 1 ]; verdict "every open succeeded, one completion, right bytes" $?
[ "$got" != 0 ]; verdict "an open landed on the empty copy first (file $got of at most 60)" $?
if [ "$got" != 0 ]; then
  records "$W/c11-$got" | sed 's/^/    rec: /' | cut -c1-230
  records "$W/c11-$got" | tail -1 | grep -q "\"status\":\"ok\".*\"size\":$FIXW_SIZE,"; verdict "the open ended at a copy with $FIXW_SIZE bytes" $?
  cat "$W/c11-$got"/stats/*.files.jsonl | grep '"kind":"origin_failure"' | grep -q '"op":"open".*"class":"data"'
  verdict "the empty copy is recorded as a failure of op open, class data" $?
  [ "$(counter "$W/c11-$got" origin_failures_data)" -ge 1 ]; verdict "origin_failures_data counts it" $?
fi

echo; echo "=== a copy of a file named *.root that is cut short of its header's end is refused"
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on)
got=0 allok=1
for i in $(seq 60); do
  L=/store/ci/cut-$i.root
  cutw $L a; placew $L b; emptyw $L c
  res=$(fuzz "$W/c13-$i" "$W/f13-$i.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-failover)
  echo "$res" | grep -q '^open-failover: clean$' || { allok=0; echo "$res" | sed 's/^/    /'; break; }
  if records "$W/c13-$i" | head -1 | grep -q "\"size\":$CUT,"; then got=$i; break; fi
done
[ "$allok" = 1 ]; verdict "every open succeeded, one completion, right bytes" $?
[ "$got" != 0 ]; verdict "an open landed on the cut copy first (file $got of at most 60)" $?
if [ "$got" != 0 ]; then
  records "$W/c13-$got" | sed 's/^/    rec: /' | cut -c1-230
  records "$W/c13-$got" | tail -1 | grep -q "\"status\":\"ok\".*\"size\":$FIXW_SIZE,"; verdict "the open ended at a copy with $FIXW_SIZE bytes" $?
  cat "$W/c13-$got"/stats/*.files.jsonl | grep '"kind":"origin_failure"' | grep -q '"op":"open".*"class":"data".*its header says the file is '"$FIXW_SIZE"' bytes'
  verdict "the cut copy is recorded as a failure of op open, class data, with the sizes" $?
  [ "$(nheader "$W/f13-$got.log")" -ge 2 ]; verdict "the first 300 bytes were read at each copy that had bytes" $?
fi
# only the cut copy: the open fails, naming it
L=/store/ci/cut-only.root
cutw $L a
res=$(fuzz "$W/c14" "$W/f14.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-denied)
echo "$res" | sed 's/^/    /' | cut -c1-700
echo "$res" | grep -q '^open-denied: clean$'; verdict "only the cut copy exists: Open() returned OK and the reader got one completion, a failure" $?
echo "$res" | grep -q "(127.0.0.1:$AX): size $CUT: its header says the file is $FIXW_SIZE bytes"; verdict "the error names the copy, its size $CUT and the header's $FIXW_SIZE" $?
echo "$res" | grep -q 'Received corrupted data'; verdict "the error is a data error" $?
[ -z "$(find "$W/c14" \( -name '*.meta' -o -name '*.data' \) -print -quit)" ]; verdict "nothing of the file is cached" $?
CC=$(copies_closed "$W/f14.log")
[ "${CC% *}" -ge 1 ] && [ "${CC% *}" = "${CC#* }" ]; verdict "the cut copy that was opened was closed by uCache (opened, closed: $CC)" $?
# a file not named *.root is not looked at: its copy is served as it is
L=/store/ci/cut-only.dat
cutw $L a
res=$(fuzz "$W/c15" "$W/f15.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-failover)
echo "$res" | grep -q '^open-failover: clean$'; verdict "the cut copy of a file not named *.root opens as it is (no header check)" $?
[ "$(nheader "$W/f15.log")" = 0 ]; verdict "no 300-byte read was made for it" $?
# the name in upper case is a ROOT file's name too
L=/store/ci/cut-only-upper.ROOT
cutw $L a
res=$(fuzz "$W/c16" "$W/f16.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-denied)
echo "$res" | sed 's/^/    /' | cut -c1-700
echo "$res" | grep -q '^open-denied: clean$' && echo "$res" | grep -q "(127.0.0.1:$AX): size $CUT: its header says the file is $FIXW_SIZE bytes"
verdict "the cut copy of a file named *.ROOT is refused too, and the error names it" $?

echo; echo "=== the header read fails: the reader's open gets exactly one completion"
# UCACHE_TEST_HEADER_FAIL_N fails the header read of the first N opens with a connection error
# (_KIND=short: the read is sent one byte short, so the copy answers it short). Each such copy is a
# failed copy, and the open goes on to another; when no copy is left the open fails. Either way the
# reader's open gets one completion, through the handler's HandleResponseWithHosts, with a host list.
L=/store/ci/hf-all.root
placew $L a b c
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_TEST_HEADER_FAIL_N=99)
res=$(fuzz "$W/c19" "$W/f19.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-denied)
echo "$res" | sed 's/^/    /' | cut -c1-700
records "$W/c19" | sed 's/^/    rec: /' | cut -c1-230
NH=$(nheader "$W/f19.log"); NATT=$(records "$W/c19" | wc -l)
[ "$NATT" -ge 3 ] && [ "$(records "$W/c19" | grep -c '"status":"ok"')" = 0 ] && [ "$NH" = 0 ]
verdict "header read failing at every copy: each attempt failed ($NATT attempts; the fault struck: no read of 300 bytes was sent, $NH)" $?
echo "$res" | grep -q '^open-denied: clean$'; verdict "header read failing at every copy: Open() returned OK and the reader got exactly one completion, a failure" $?
n=0; for s in a b c; do echo "$res" | grep -q "(127.0.0.1:$(port_of $s)): \[ERROR\] Connection error" && n=$((n + 1)); done
[ "$n" = 3 ] && echo "$res" | grep -q 'no site could open it'; verdict "header read failing at every copy: the error names each copy and its failure ($n of 3)" $?
CC=$(copies_closed "$W/f19.log")
[ "${CC% *}" = "$NATT" ] && [ "${CC#* }" = "$NATT" ]; verdict "header read failing at every copy: each copy that was opened was closed by uCache (opened, closed: $CC; $NATT attempts)" $?
for kind in error short; do
  L=/store/ci/hf-one-$kind.root
  placew $L a b c
  FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_TEST_HEADER_FAIL_N=1 UCACHE_TEST_HEADER_FAIL_KIND=$kind)
  res=$(fuzz "$W/c20-$kind" "$W/f20-$kind.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-failover)
  echo "$res" | grep -v '^PASS' | sed 's/^/    /' | cut -c1-300
  records "$W/c20-$kind" | sed 's/^/    rec: /' | cut -c1-230
  case $kind in
    error) records "$W/c20-$kind" | head -1 | grep -q '"code":108,'; r=$? ;;
    short) records "$W/c20-$kind" | head -1 | grep -q '"status":"ok".*"class":"none"' &&
             cat "$W/c20-$kind"/stats/*.files.jsonl | grep '"kind":"origin_failure"' | head -1 | grep -q '"op":"open".*"class":"data".*a read of its first 300 bytes answered 299'; r=$? ;;
  esac
  [ "$r" = 0 ] && [ "$(records "$W/c20-$kind" | wc -l)" -ge 2 ]
  verdict "header read failing ($kind) at the first copy: the fault struck there and the open went on to another attempt" $?
  echo "$res" | grep -q '^open-failover: clean$'; verdict "header read failing ($kind) at the first copy: exactly one completion, a success, the file's own bytes" $?
  records "$W/c20-$kind" | tail -1 | grep -q "\"status\":\"ok\".*\"size\":$FIXW_SIZE,.*\"outcome\":\"ok\""; verdict "header read failing ($kind) at the first copy: the last attempt is ok, at a whole copy" $?
  # Each copy opened was closed: the ones refused by uCache, the one taken by the reader's own close (open-failover
  # closes its File after its read).
  NATT=$(records "$W/c20-$kind" | wc -l); CC=$(copies_closed "$W/f20-$kind.log")
  [ "${CC% *}" = "$NATT" ] && [ "${CC#* }" = "$NATT" ]; verdict "header read failing ($kind) at the first copy: each copy that was opened was closed (opened, closed: $CC; $NATT attempts)" $?
done
# A ReadRecovery the reader sets just as the checks begin (UCACHE_TEST_PROP_DELAY_MS holds it between its
# recording and its setting on the File; the checks' first read is sent 5 s late): it is on the File before the
# checks turn the recovery off, so it reads false while their reads are in flight, and the reader's true after.
L=/store/ci/prop-race.root
placew $L a b c
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_TEST_PROP_DELAY_MS=2000 UCACHE_TEST_HEADER_FAIL_N=1 UCACHE_TEST_HEADER_FAIL_KIND=delay)
res=$(fuzz "$W/c24" "$W/f24.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-property-race)
echo "$res" | sed 's/^/    /' | cut -c1-300
echo "$res" | grep -q '^open-property-race: clean$'; verdict "a setting made as the checks at open begin: ReadRecovery false during their reads, the reader's true after, one completion" $?
[ "$(nheader "$W/f24.log")" = 1 ] && records "$W/c24" | grep -q '"status":"ok"'; verdict "the checks' first read was sent once, late, and the open succeeded (the fault was in place)" $?
# A Stat the reader makes while the checks run (their first read sent 5 s late) waits for the open's answer before
# the handle is set up: until then the File may be open at a copy the checks have yet to judge. It answers the
# file's size no earlier than the checks' late read, and what the checks read of the copy the open takes goes into
# the entry (open_check_bytes_kept), never left waiting in memory.
L=/store/ci/stat-during.root
placew $L a b c
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_TEST_HEADER_FAIL_N=1 UCACHE_TEST_HEADER_FAIL_KIND=delay)
res=$(fuzz "$W/c25" "$W/f25.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-stat-during-checks)
echo "$res" | sed 's/^/    /' | cut -c1-300
echo "$res" | grep -q '^open-stat-during-checks: clean$'; verdict "a Stat made while the checks at open run waits for the open's answer: the file's size, no earlier than 4.5 s after the open was issued; one completion; the read of the header exact" $?
# (The delayed read is a test's fault, made as asked, 300 bytes, and not kept; the list's pages are kept.)
grep -qE 'MsgHandler created.*kXR_read \(handle: 0x[0-9a-f]*, offset: 0, size: 300\)' "$W/f25.log" &&
  [ "$(records "$W/c25" | wc -l)" = 1 ] && records "$W/c25" | grep -q '"status":"ok"'
verdict "the checks' first read was the late one (300 bytes at 0), and the open succeeded at its first attempt (the fault was in place)" $?
K=$(counter "$W/c25" open_check_bytes_kept)
[ "${K:-0}" -gt 0 ]; verdict "what the checks read went into the entry the Stat had set up (open_check_bytes_kept ${K:-none})" $?
# Every copy cut short (its header intact): a Stat made while the checks run (their first read sent 5 s late) is
# never given a cut copy's size, for any copy the redirector picks: it waits for the open, which refuses every copy
# and fails once; the Stat then fails too (the client's own answer for a File that is not open).
L=/store/ci/stat-during-cut.root
cutw $L a b c
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_TEST_HEADER_FAIL_N=1 UCACHE_TEST_HEADER_FAIL_KIND=delay)
res=$(fuzz "$W/c25c" "$W/f25c.log" "root://127.0.0.1:$RX/$L" "$W/fixw-cut.bin" 1 1 1 open-stat-during-checks cut)
echo "$res" | sed 's/^/    /' | cut -c1-300
echo "$res" | grep -q '^open-stat-during-checks: clean$'
verdict "every copy cut: a Stat made while the checks run waits for the open, is never answered the cut size ($CUT), and the open fails once" $?
NC=$(records "$W/c25c" | grep -c '"size":'"$CUT")
grep -qE 'MsgHandler created.*kXR_read \(handle: 0x[0-9a-f]*, offset: 0, size: 300\)' "$W/f25c.log" && [ "$NC" -ge 1 ] &&
  records "$W/c25c" | tail -1 | grep -q '"outcome":"error"'
verdict "every copy cut: the checks' first read was the late one, the open's copies reported the cut size ($NC records), and the open ended in error (the fault was in place)" $?
# A Stat and a Read made, asynchronously, from inside the callback of another file's request (on the client's own
# thread) while the checks at open run (their first read sent 5 s late): each call returns at once, never holding the
# client's thread, which the open's own checks may need, and each is answered once, after the open answered: the
# file's size, and the file's first 300 bytes.
L=/store/ci/park-from-callback.root
placew $L a b c
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_TEST_HEADER_FAIL_N=1 UCACHE_TEST_HEADER_FAIL_KIND=delay)
res=$(fuzz "$W/c25p" "$W/f25p.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-park-from-callback)
echo "$res" | sed 's/^/    /' | cut -c1-600
echo "$res" | grep -q '^CALLS at-once$'
verdict "a Stat and a Read made from another file's callback while the checks at open run return at once (the client's thread is not held)" $?
echo "$res" | grep -q '^open-park-from-callback: clean$'
verdict "the Stat and the Read made from the callback are answered once each, after the open: the file's size and its first 300 bytes" $?
grep -qE 'MsgHandler created.*kXR_read \(handle: 0x[0-9a-f]*, offset: 0, size: 300\)' "$W/f25p.log" &&
  [ "$(records "$W/c25p" | wc -l)" = 1 ] && records "$W/c25p" | grep -q '"status":"ok"'
verdict "the checks' first read was the late one, and the open succeeded at its first attempt (the fault was in place)" $?
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on) # the cases below have no header fault

echo; echo "=== a copy that is zeros at the file's size is refused"
head -c "$FIXW_SIZE" /dev/zero > "$W/fixw-zero.bin"
got=0 allok=1
for i in $(seq 60); do
  L=/store/ci/zero-$i.root
  z=$(echo a b c | cut -d' ' -f$((i % 3 + 1)))
  copyw "$W/fixw-zero.bin" $L $z; placew $L $(others $z)
  res=$(fuzz "$W/c17-$i" "$W/f17-$i.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-failover)
  echo "$res" | grep -q '^open-failover: clean$' || { allok=0; echo "$res" | sed 's/^/    /'; break; }
  if records "$W/c17-$i" | head -1 | grep -q "\"data_server\":\"127.0.0.1:$(port_of $z)\""; then got=$i; break; fi
done
[ "$allok" = 1 ]; verdict "every open succeeded, one completion, right bytes" $?
[ "$got" != 0 ]; verdict "an open landed on the copy of zeros (file $got of at most 60)" $?
if [ "$got" != 0 ]; then
  records "$W/c17-$got" | sed 's/^/    rec: /' | cut -c1-230
  cat "$W/c17-$got"/stats/*.files.jsonl | grep '"kind":"origin_failure"' | grep -q "\"op\":\"open\".*\"class\":\"data\".*size $FIXW_SIZE: it does not begin with"
  verdict "the copy of zeros is recorded as a failure of op open, class data: it does not begin as a ROOT file does" $?
  records "$W/c17-$got" | tail -1 | grep -q "\"status\":\"ok\".*\"size\":$FIXW_SIZE,"; verdict "the open ended at a whole copy" $?
fi

echo; echo "=== a copy whose header or list of top-level objects is damaged is refused"
# The fixture's header and its top directory's record (32-bit positions): the first record at 8, the
# end at 12, the name record's length at 28; the directory's record at begin + that length gives the
# list's length at 10 and its position at 26.
be32() { od -An -tu1 -j "$2" -N4 "$1" | awk '{print (($1 * 256 + $2) * 256 + $3) * 256 + $4}'; }
put32() { # FILE OFF VALUE: four bytes, big-endian, in place
  printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $(($3 >> 24 & 255)) $(($3 >> 16 & 255)) $(($3 >> 8 & 255)) $(($3 & 255)))" |
    dd of="$1" bs=1 seek="$2" conv=notrunc status=none
}
W_BEGIN=$(be32 "$FIXW" 8); W_END=$(be32 "$FIXW" 12); W_DIR=$((W_BEGIN + $(be32 "$FIXW" 28)))
W_NK=$(be32 "$FIXW" $((W_DIR + 10))); W_SK=$(be32 "$FIXW" $((W_DIR + 26)))
echo "    fixture: begin $W_BEGIN, end $W_END, directory record at $W_DIR, list at $W_SK ($W_NK bytes)"
cp "$FIXW" "$W/fixw-begin.bin"; put32 "$W/fixw-begin.bin" 8 $((W_END + 1))
cp "$FIXW" "$W/fixw-list.bin"; dd if=/dev/zero of="$W/fixw-list.bin" bs=1 seek="$W_SK" count="$W_NK" conv=notrunc status=none
[ "$W_END" = "$FIXW_SIZE" ] && [ "$W_SK" -gt "$W_BEGIN" ] && [ $((W_SK + W_NK)) -le "$W_END" ] && [ "$(be32 "$W/fixw-begin.bin" 8)" = $((W_END + 1)) ] &&
  od -v -An -tu1 -j "$W_SK" -N "$W_NK" "$W/fixw-list.bin" | awk '{ for (i = 1; i <= NF; i++) if ($i != 0) bad = 1 } END { exit bad }' &&
  [ "$(stat -c %s "$W/fixw-begin.bin")" = "$FIXW_SIZE" ] && [ "$(stat -c %s "$W/fixw-list.bin")" = "$FIXW_SIZE" ]
verdict "the damaged copies: the header's first record past its end ($((W_END + 1))), and the list's $W_NK bytes at $W_SK zeroed, both at the fixture's size" $?
# reads of the list in a client's log: as the check needs it, or its whole pages (4096 bytes each, to the copy's
# end), which the cache keeps for the reader
W_PO=$((W_SK / 4096 * 4096)); W_PE=$(( (W_SK + W_NK + 4095) / 4096 * 4096 )); [ "$W_PE" -gt "$FIXW_SIZE" ] && W_PE=$FIXW_SIZE
nlist() {
  echo $(( $(grep -c "MsgHandler created.*kXR_read (handle: 0x[0-9a-f]*, offset: $W_SK, size: $W_NK)" "$1") +
           $(grep -c "MsgHandler created.*kXR_read (handle: 0x[0-9a-f]*, offset: $W_PO, size: $((W_PE - W_PO)))" "$1") ))
}
for dmg in begin list; do
  case $dmg in
    begin) why="size $FIXW_SIZE: its header says its first record begins at $((W_END + 1)), past the end of its data at $W_END" ;;
    list) why="size $FIXW_SIZE: its list of top-level objects at $W_SK is labelled as lying at 0" ;;
  esac
  got=0 allok=1
  for i in $(seq 60); do
    L=/store/ci/dmg-$dmg-$i.root
    z=$(echo a b c | cut -d' ' -f$((i % 3 + 1)))
    copyw "$W/fixw-$dmg.bin" $L $z; placew $L $(others $z)
    res=$(fuzz "$W/c21-$dmg-$i" "$W/f21-$dmg-$i.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-failover)
    echo "$res" | grep -q '^open-failover: clean$' || { allok=0; echo "$res" | sed 's/^/    /'; break; }
    if records "$W/c21-$dmg-$i" | head -1 | grep -q "\"data_server\":\"127.0.0.1:$(port_of $z)\""; then got=$i; break; fi
  done
  [ "$allok" = 1 ]; verdict "$dmg: every open succeeded, one completion, right bytes" $?
  [ "$got" != 0 ]; verdict "$dmg: an open landed on the damaged copy first (file $got of at most 60)" $?
  if [ "$got" != 0 ]; then
    records "$W/c21-$dmg-$got" | sed 's/^/    rec: /' | cut -c1-230
    cat "$W/c21-$dmg-$got"/stats/*.files.jsonl | grep '"kind":"origin_failure"' | head -1 | grep -q "\"op\":\"open\".*\"class\":\"data\".*$why"
    verdict "$dmg: the damaged copy is recorded as a failure of op open, class data: $why" $?
    records "$W/c21-$dmg-$got" | tail -1 | grep -q "\"status\":\"ok\".*\"size\":$FIXW_SIZE,.*\"outcome\":\"ok\""; verdict "$dmg: the open ended at a whole copy" $?
    # (a and b answer from 127.0.0.1, which tried= cannot name: the open may be sent back to a damaged
    # copy there, which is then read and refused again)
    NL=$(nlist "$W/f21-$dmg-$got.log")
    if [ $dmg = list ]; then [ "$NL" -ge 2 ]; else [ "$NL" = 1 ]; fi; r=$? # (read before the label's expansions)
    verdict "$dmg: the list of top-level objects was read $NL times ($( [ $dmg = list ] && echo "at the damaged copy, and " )at the copy taken)" $r
    [ "$(confirmed "$W/c21-$dmg-$got")" = 1 ]; verdict "$dmg: the copy taken confirmed the size" $?
  fi
done
# the zeroed list is the only copy: the open fails, naming it
L=/store/ci/dmg-list-only.root
copyw "$W/fixw-list.bin" $L a
res=$(fuzz "$W/c22" "$W/f22.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-denied)
echo "$res" | sed 's/^/    /' | cut -c1-700
echo "$res" | grep -q '^open-denied: clean$'; verdict "only the copy with the zeroed list: Open() returned OK and the reader got one completion, a failure" $?
echo "$res" | grep -q "(127.0.0.1:$AX): size $FIXW_SIZE: its list of top-level objects at $W_SK is labelled as lying at 0" && echo "$res" | grep -q 'Received corrupted data'
verdict "the error is a data error that names the copy and why" $?
[ -z "$(find "$W/c22" \( -name '*.meta' -o -name '*.data' \) -print -quit)" ]; verdict "nothing of the file is cached" $?
CC=$(copies_closed "$W/f22.log")
[ "${CC% *}" -ge 1 ] && [ "${CC% *}" = "${CC#* }" ]; verdict "the copy that was opened was closed by uCache (opened, closed: $CC)" $?
# the read of the list fails at the first copy: a failed copy, the open goes on
L=/store/ci/dmg-listread.root
placew $L a b c
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_TEST_HEADER_FAIL_N=1 UCACHE_TEST_HEADER_FAIL_STAGE=list)
res=$(fuzz "$W/c23" "$W/f23.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-failover)
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on)
echo "$res" | grep -v '^PASS' | sed 's/^/    /' | cut -c1-300
records "$W/c23" | sed 's/^/    rec: /' | cut -c1-230
records "$W/c23" | head -1 | grep -q '"code":108,' && [ "$(nheader "$W/f23.log")" = 2 ]
verdict "the list read failing at the first copy: that attempt failed (108) after its 300 bytes were read (two 300-byte reads in all: the fault struck the list read)" $?
echo "$res" | grep -q '^open-failover: clean$'; verdict "the list read failing at the first copy: exactly one completion, a success, the file's own bytes" $?
records "$W/c23" | tail -1 | grep -q "\"status\":\"ok\".*\"size\":$FIXW_SIZE,.*\"outcome\":\"ok\""; verdict "the list read failing at the first copy: the last attempt is ok, at a whole copy" $?

echo; echo "=== every copy is longer than its header says: the open stands"
cat "$FIXW" > "$W/fixw-long.bin"; head -c 4096 /dev/urandom >> "$W/fixw-long.bin"
LONG_SIZE=$(stat -c %s "$W/fixw-long.bin")
L=/store/ci/long.root
copyw "$W/fixw-long.bin" $L a b c
res=$(fuzz "$W/c18" "$W/f18.log" "root://127.0.0.1:$RX/$L" "$W/fixw-long.bin" 1 1 1 open-failover)
echo "$res" | grep -v '^PASS' | sed 's/^/    /' | cut -c1-300
records "$W/c18" | sed 's/^/    rec: /' | cut -c1-230
echo "$res" | grep -q '^open-failover: clean$'; verdict "the open succeeded, one completion, the file's own bytes" $?
[ "$(records "$W/c18" | wc -l)" = 1 ] && records "$W/c18" | grep -q "\"status\":\"ok\".*\"size\":$LONG_SIZE,"
verdict "one attempt, ok, at a copy of $LONG_SIZE bytes (its header says $FIXW_SIZE): it stands" $?
[ -z "$(cat "$W/c18"/stats/*.files.jsonl 2>/dev/null | grep '"kind":"origin_failure"')" ]; verdict "no copy was refused" $?
[ "$(nheader "$W/f18.log")" = 1 ]; verdict "its first 300 bytes were read" $?
[ "$(confirmed "$W/c18")" = 0 ]; verdict "the size is not confirmed (bit 2 of the sidecar's flags is clear)" $?

echo; echo "=== every copy of a file named *.root is empty: one error naming the copies"
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on)
L12=/store/ci/ee.root
place $L12 a b c
for s in a b c; do : > "$W/data/$s$L12"; done
res=$(fuzz "$W/c12" "$W/f12.log" "root://127.0.0.1:$RX/$L12" "$W/f.bin" 1 1 1 open-denied)
echo "$res" | sed 's/^/    /' | cut -c1-900
echo "$res" | grep -q '^open-denied: clean$'; verdict "Open() returned OK and the reader got one completion, a failure" $?
n=0; for s in a b c; do echo "$res" | grep -q "(127.0.0.1:$(port_of $s)): size 0:" && n=$((n + 1)); done
[ "$n" = 3 ]; verdict "the error names all three copies, each with its size 0 ($n of 3)" $?
echo "$res" | grep -q 'no site could open it'; verdict "the error says no site could open it" $?
echo "$res" | grep -q 'Received corrupted data'; verdict "the error is a data error" $?
# The three servers answer from one address, so a retry through the redirector may be sent back to a copy
# that was refused (four attempts, not three). A copy the regional lists is never one that was refused.
NATT=$(records "$W/c12" | wc -l)
[ "$NATT" -ge 3 ] && [ "$(records "$W/c12" | grep -c '"size":0,')" = "$NATT" ]
verdict "at least three attempts ($NATT), each at a copy of size 0" $?
seen="" dup=0
while read -r line; do
  srv=$(echo "$line" | sed -n 's/.*"data_server":"\([^"]*\)".*/\1/p')
  case "$line" in *'"route":"candidate"'*) case " $seen " in *" $srv "*) dup=1 ;; esac ;; esac
  seen="$seen $srv"
done < <(records "$W/c12")
[ "$dup" = 0 ]; verdict "an attempt at a listed copy was never at a copy that was refused already" $?
records "$W/c12" | tail -1 | grep -q '"outcome":"error"'; verdict "the last record line says outcome error" $?
# A copy the regional sends back after it was refused at this open is refused again as it is, with no new record
# and no new count (no read is made of it, and its refusal is not a new failure): one record and one count a copy,
# whatever the number of attempts.
NCOPY=$(records "$W/c12" | sed -n 's/.*"data_server":"[^"]*:\([0-9]*\)".*/\1/p' | sort -u | wc -l)
FSRV=$(cat "$W/c12"/stats/*.files.jsonl | grep '"kind":"origin_failure"' | grep '"op":"open".*"class":"data"' | sed -n 's/.*"data_server":"[^"]*:\([0-9]*\)".*/\1/p')
[ "$NCOPY" = 3 ] && [ "$(echo "$FSRV" | grep -c .)" = 3 ] && [ "$(echo "$FSRV" | sort -u | grep -c .)" = 3 ]; r=$?
verdict "failures of op open, class data are recorded, one for each copy ($(echo $FSRV); $NCOPY copies in $NATT attempts)" $r
[ "$(counter "$W/c12" origin_failures_data)" = "$NCOPY" ]; verdict "origin_failures_data counts them, once a copy ($NCOPY)" $?
CC=$(copies_closed "$W/f12.log")
[ "${CC% *}" = "$NATT" ] && [ "${CC#* }" = "$NATT" ]; verdict "each copy that was opened was closed by uCache (opened, closed: $CC; $NATT attempts)" $?
[ -z "$(find "$W/c12" \( -name '*.meta' -o -name '*.data' \) -print -quit)" ]; verdict "nothing of the file is cached" $?

echo; echo "=== all refused: a, b and c refuse the file"
L9=/store/ci/f9.bin
place $L9 a b c
chmod 000 "$W/data/a$L9" "$W/data/b$L9" "$W/data/c$L9"
ok=0; for s in a b c; do direct_read $s $L9 || ok=$((ok + 1)); done
[ "$ok" = 3 ]; verdict "each of the three refuses the file ($ok of 3)" $?
res=$(fuzz "$W/c9" "$W/f9.log" "root://127.0.0.1:$RX/$L9" "$W/f.bin" 1 1 1 open-denied)
echo "$res" | sed 's/^/    /' | cut -c1-900
echo "$res" | grep -q '^open-denied: clean$'; verdict "Open() returned OK and the reader got one completion, a failure" $?
n=0; for s in a b c; do echo "$res" | grep -q "(127.0.0.1:$(port_of $s))" && n=$((n + 1)); done
echo "$res" | grep -q 'no site could open it'; verdict "the error says no site could open it" $?
[ "$n" = 3 ]; verdict "it names all three servers ($n of 3)" $?
records "$W/c9" | tail -1 | grep -q '"outcome":"error"'; verdict "the last record line says outcome error" $?

echo; echo "=== the switch off, the default: a refuses the file"
DEF=on
[ -x "$BUILD/src/cli/ucache" ] && DEF=$(env -u UCACHE_ADAPTIVE_FEDERATION UCACHE_DIR="$W/c10s" "$BUILD/src/cli/ucache" settings 2>&1 | grep -i 'adaptive_federation' | head -1 | awk '{print $2}')
echo "    the switch's default: $DEF"
if [ "$DEF" = off ]; then
  # First the file on a alone, which refuses it: every open lands on a (whatever the regional would choose among
  # copies), is refused and fails. Then the file on a, b and c, a refusing: the regional chooses among them as it
  # will (it went 30 opens without a in some runs), and each open that landed on a fails there, as it would without
  # the cache, and no other is moved anywhere: the opens that failed are the refusals at a, one landing each.
  FEDENV=()
  L10A=/store/ci/f10a.bin
  place $L10A a
  chmod 000 "$W/data/a$L10A"
  RL0A=$(regional_locates $L10A)
  res=$(fuzz "$W/c10a" "$W/f10a.log" "root://127.0.0.1:$RX/$L10A" "$W/f.bin" 5 1 1 open-failover)
  echo "$res" | tail -2 | sed 's/^/    /'
  NRA=$(grep -c "\[127.0.0.1:$AX\] Handling error while processing kXR_open (file: $L10A,.*permission denied" "$W/f10a.log")
  echo "$res" | grep -q '^open-failover: 5 open(s), 5 failed, 0 without exactly one completion$' && [ "$NRA" = 5 ]
  verdict "the copy on a alone: each of 5 opens was refused at a and failed, one completion each: the refusal was felt and nothing failed over ($NRA refusals)" $?
  [ "$(grep 'MsgHandler created' "$W/f10a.log" | grep -c 'kXR_locate ')" = 0 ] && [ "$(regional_locates $L10A)" = "$RL0A" ]
  verdict "the copy on a alone: no kXR_locate in the client's log, none of the file in the regional's" $?
  L10=/store/ci/f10.bin
  place $L10 a b c
  chmod 000 "$W/data/a$L10"
  direct_read a $L10; r=$?; [ "$r" != 0 ]; verdict "a refuses the file" $?
  RL0=$(regional_locates $L10)
  res=$(fuzz "$W/c10" "$W/f10.log" "root://127.0.0.1:$RX/$L10" "$W/f.bin" 30 1 1 open-failover)
  echo "$res" | tail -2 | sed 's/^/    /'
  NF=$(echo "$res" | sed -n 's/^open-failover: 30 open(s), \([0-9]*\) failed, 0 without exactly one completion$/\1/p')
  NR=$(grep -c "\[127.0.0.1:$AX\] Handling error while processing kXR_open (file: $L10,.*permission denied" "$W/f10.log")
  NL=$(grep -c "Redirected from: root://127.0.0.1:$RX/*$L10 to: " "$W/f10.log")
  [ -n "$NF" ] && [ "$NF" = "$NR" ] && [ "$NL" = 30 ]
  verdict "the file on a, b and c: each open that landed on a failed there and none was moved ($NF failed, $NR refused at a, $NL landings for 30 opens)" $?
  [ "$(grep 'MsgHandler created' "$W/f10.log" | grep -c 'kXR_locate ')" = 0 ]; verdict "no kXR_locate in the client's log" $?
  [ "$(regional_locates $L10)" = "$RL0" ]; verdict "no locate of the file in the regional's log" $?
  xfs 127.0.0.1:$RX locate $L10 >/dev/null 2>&1
  [ "$(regional_locates $L10)" -gt "$RL0" ]; verdict "positive control: an xrdfs locate of the leg's own shows in the regional's log" $?
else
  echo "    the default is on: this leg's expectations follow the switch-on legs"
fi
echo; echo "=== reads: a failed origin read is read again at another site"
head -c 8388608 /dev/urandom > "$W/f8.bin"; chmod 644 "$W/f8.bin"
place8() { # LFN server... (8 MiB)
  local lfn=$1 s; shift
  for s in "$@"; do
    install -d -m 777 "$W/data/$s/$(dirname "$lfn" | sed 's|^/||')"
    cp "$W/f8.bin" "$W/data/$s$lfn"; chmod 666 "$W/data/$s$lfn"
  done
}
# CACHE -> the lines of one kind of its per-file records
recs() { cat "$1"/stats/*.files.jsonl 2>/dev/null | grep "\"kind\":\"$2\""; }
field() { sed -n "s/.*\"$1\":\"\{0,1\}\([^\",}]*\).*/\1/p"; } # a field of each line
# A server as its port: the three data servers share one address, and the form of that address in a record or a
# property depends on the address family the client reached the regional over (localhost may be 127.0.0.1 or
# ::1, which names them 127.0.0.1:P, [::1]:P or [::ffff:127.0.0.1]:P), so the port is what tells them apart.
pnorm() { sed -E 's/(^| )[^ ]*:([0-9]+)/\1\2/g'; }
# rf CASE N SWITCH [KIND [TAG]]: the case on a file of its own (TAG tells two runs of one case apart);
# sets RES, RFC (its cache), SV (the servers it read from). RFX: more of the client's environment;
# RFHOST: the name the reader opens the regional by.
RFX=() RFHOST=127.0.0.1
rf() {
  local c=$1 n=$2 sw=$3 kind=${4:-} tag=${5:-}
  local l=/store/ci/rf-$c-${kind:-x}$tag.bin
  place8 $l a b c
  RFC="$W/c-rf-$c-${kind:-x}$tag"
  FEDENV=(UCACHE_ADAPTIVE_FEDERATION=$sw UCACHE_TEST_READ_FAIL_N=$n ${kind:+UCACHE_TEST_READ_FAIL_KIND=$kind} "${RFX[@]}")
  RES=$(fuzz "$RFC" "$W/rf-$c-${kind:-x}$tag.log" "root://$RFHOST:$RX/$l" "$W/f8.bin" 1 1 1 read-failover $c)
  echo "$RES" | grep -v '^PASS' | sed 's/^/    /' | cut -c1-220
  SV=($(echo "$RES" | sed -n 's/^SERVERS //p' | pnorm))       # as ports (pnorm), as FL and MV
  FL=($(recs "$RFC" origin_failure | field data_server | pnorm))   # the server of each failed read
  MV=($(recs "$RFC" open | grep '"route":"read"' | grep '"outcome":"ok"' | field data_server | pnorm)) # where each move landed
  NRA=$(recs "$RFC" open | grep -c '"route":"read"')       # open attempts of moves
}
clean() { echo "$RES" | grep -q "^read-failover $1: clean\$"; }

rf miss 2 on
clean miss; verdict "miss: a Read and a VectorRead whose origin read failed are exact, once, and move the handle" $?
[ "${#FL[@]}" = 2 ] && [ "${FL[0]}" = "${SV[0]}" ] && [ "${FL[1]}" = "${SV[1]}" ]; verdict "miss: the failed reads name the servers the handle was at (${FL[*]})" $?
[ "${#MV[@]}" = 2 ] && [ "${MV[0]}" = "${SV[1]}" ] && [ "${MV[1]}" = "${SV[2]}" ]; verdict "miss: a move of route read after each landed at the next server (${MV[*]})" $?
[ "$(counter "$RFC" open_failovers)" = "$NRA" ]; r=$?; verdict "miss: open_failovers $(counter "$RFC" open_failovers) equals the attempts of route read ($NRA)" $r

rf relay 2 on relay
clean relay; verdict "relay: a PgRead and a VectorRead into one buffer that failed are exact, once, and move the handle" $?
[ "$(recs "$RFC" origin_failure | field op | tr '\n' ' ')" = "relay relay " ] && [ "${#MV[@]}" = 2 ]; verdict "relay: two failed relayed reads, two moves (${MV[*]})" $?

# The same on a handle the cache does not serve (its host is denied the cache): it has no entry and no
# stat of its own, and moves to a copy of the size its reader's open was shown. Every read of it is
# relayed, the first one (the Read that arms a cached handle) too, so three fail. The reader opens
# the regional as localhost and that name is denied; the servers the regional lists answer from
# 127.0.0.1. (The lists say which files uCache caches; for a file they leave out they do not narrow
# the copies a move may take: see "no host allowed" below.)
RFX=(UCACHE_DENY=localhost) RFHOST=localhost
rf relay 3 on relay -denied
RFX=() RFHOST=127.0.0.1
clean relay; verdict "relay, host denied: a Read, a PgRead and a VectorRead that failed are exact, once, and move the handle" $?
[ "$(find "$RFC" -name '*.meta' | wc -l)" = 0 ]; verdict "relay, host denied: nothing is cached" $?
[ "$(recs "$RFC" origin_failure | field op | tr '\n' ' ')" = "relay relay relay " ] && [ "${#MV[@]}" = 3 ] &&
  [ "$(recs "$RFC" open | grep '"route":"read"' | grep '"outcome":"ok"' | grep -c '"size":8388608,')" = 3 ]
verdict "relay, host denied: three failed relayed reads, three moves, each to a copy of 8388608 bytes (${MV[*]})" $?
# Each move lands at a server other than the one its read failed at, also when the regional sends the move's
# open back there (it cannot be told to avoid an address): the move then takes another copy the regional
# lists. By the third every server has failed, so the regional's answer is always sent back, and the list
# gives the failed servers the oldest failure first: the third move lands where the first read failed.
# The reader reaches the servers as the regional names them, [::1] where localhost is IPv6 first: the checks
# compare servers by their ports (pnorm), so they hold whichever address localhost gives. The deny list names
# localhost, the host of the job's URL, never a server's address, so the file is not cached either way.
away=1; for i in 0 1 2; do [ -n "${MV[$i]:-}" ] && [ "${MV[$i]}" != "${FL[$i]:-}" ] || away=0; done
SB=$(recs "$RFC" open | grep '"route":"read"' | grep -vc '"attempt":1,')
[ "$away" = 1 ]; verdict "relay, host denied: each move landed away from the server its read failed at (failed ${FL[*]}; landed ${MV[*]}; $SB later attempts of moves)" $?
[ "${#FL[@]}" = 3 ] && [ "$(printf '%s\n' "${FL[@]}" | sort -u | wc -l)" = 3 ] && [ "${MV[2]:-}" = "${FL[0]}" ] && [ "$SB" -ge 1 ]
verdict "relay, host denied: with every server failed, the third move was sent back and then took the oldest failure, ${FL[0]} (landed ${MV[2]:-none})" $?
# The same with an allow list that names no host at all, so it leaves out the regional's name and every copy's
# address too: the file is not cached, and the lists, which say which files uCache caches, do not narrow where
# its reads move: each move takes a copy the regional lists, as above. (Were the lists to narrow them, every
# copy would be refused, and a move the regional sends back to the server it left would take that one.)
RFX=(UCACHE_ALLOW=nohost.invalid) RFHOST=localhost
rf relay 3 on relay -allownone
RFX=() RFHOST=127.0.0.1
clean relay; verdict "relay, no host allowed: a Read, a PgRead and a VectorRead that failed are exact, once, and move the handle" $?
[ "$(find "$RFC" -name '*.meta' | wc -l)" = 0 ]; verdict "relay, no host allowed: nothing is cached" $?
[ "$(recs "$RFC" origin_failure | field op | tr '\n' ' ')" = "relay relay relay " ] && [ "${#MV[@]}" = 3 ] &&
  [ "$(recs "$RFC" open | grep '"route":"read"' | grep '"outcome":"ok"' | grep -c '"size":8388608,')" = 3 ]
verdict "relay, no host allowed: three failed relayed reads, three moves, each to a copy of 8388608 bytes (${MV[*]})" $?
away=1; for i in 0 1 2; do [ -n "${MV[$i]:-}" ] && [ "${MV[$i]}" != "${FL[$i]:-}" ] || away=0; done
SB=$(recs "$RFC" open | grep '"route":"read"' | grep -vc '"attempt":1,')
[ "$away" = 1 ]; verdict "relay, no host allowed: each move landed away from the server its read failed at (failed ${FL[*]}; landed ${MV[*]}; $SB later attempts of moves)" $?
[ "${#FL[@]}" = 3 ] && [ "$(printf '%s\n' "${FL[@]}" | sort -u | wc -l)" = 3 ] && [ "${MV[2]:-}" = "${FL[0]}" ] && [ "$SB" -ge 1 ]
verdict "relay, no host allowed: with every server failed, the third move was sent back and then took the oldest failure from the regional's list, ${FL[0]} (landed ${MV[2]:-none})" $?

rf refuse 3 on refuse
clean refuse; verdict "refuse: reads the client refused at once are exact, once" $?
[ "$(recs "$RFC" origin_failure | grep -c '"code":109')" = 3 ] && [ "${#MV[@]}" -ge 1 ]; verdict "refuse: three refusals recorded with 109, each followed by a move (${#MV[@]} moves)" $?

rf final 2 on final
clean final; verdict "final: reads that failed with an error no other site would change are exact, once, and stay put" $?
[ "$(recs "$RFC" origin_failure | grep -c '"code":303')" = 2 ] && [ "$NRA" = 0 ]; verdict "final: two failures recorded with 303, no open of route read" $?

for kind in "" late; do
  rf many 8 on $kind
  clean many; verdict "many${kind:+ ($kind)}: 32 reads at once at one handle, up to 8 of them failing, are exact, once" $?
  # The first failure is the arming read's, alone. The others (how many depends on how many of the reads missed: at most 7)
  # were issued at the server it moved to and share a move. An injected failure carries no host, so one that is
  # handled after a move names the server of that move's landing. How many moves there are depends on the timing:
  # fewer than the failures is what shows that a move serves several.
  same=1
  for ((i = 1; i < ${#FL[@]}; i++)); do
    [[ " ${MV[*]} " == *" ${FL[$i]} "* ]] || same=0
  done
  [ "${#FL[@]}" -ge 3 ] && [ "$same" = 1 ] && [ "${#MV[@]}" -ge 1 ] && [ "${#MV[@]}" -lt "${#FL[@]}" ]; verdict "many${kind:+ ($kind)}: of ${#FL[@]} failures the ones after the first name the server of a move, and the moves (${#MV[@]}) are fewer than the failures" $?
done

# A read that fails at every copy a move takes (UCACHE_TEST_READ_FAIL_KIND=again: the faults strike the relayed
# read's first issue and each issue made again after a move): after its fourth attempt it ends, with the error that
# names every copy it was read from, each with its size, and its last failure's code (108, a connection error).
rf exhaust 4 on again
echo "$RES" | grep '^MSG' | sed 's/^/    /' | cut -c1-600
clean exhaust; verdict "exhaust: a PgRead that failed at every copy it was moved to fails once, with its last failure's code and the error naming the copies; the handle reads on" $?
[ "$(recs "$RFC" origin_failure | grep '"op":"relay"' | grep -c '"code":108,')" = 4 ] && [ "${#MV[@]}" = 3 ]
verdict "exhaust: four failed relayed reads of code 108 (the fault struck each attempt), three moves between them (${MV[*]})" $?
NC=0; for p in $AX $BX $CX; do echo "$RES" | grep '^MSG' | grep -q "(127.0.0.1:$p, 8388608 bytes): \[ERROR\] Connection error" && NC=$((NC + 1)); done
echo "$RES" | grep '^MSG' | grep -q 'no copy could serve the read' && [ "$NC" = 3 ]
verdict "exhaust: the error says no copy could serve the read and names each of the three copies with its size 8388608 and its connection error ($NC of 3)" $?

rf off 2 off
clean off; verdict "switch off: failed reads are relayed on the same file, exact, once; the client's read recovery is on" $?
[ "$NRA" = 0 ] && [ "${#FL[@]}" = 2 ]; verdict "switch off: two failures recorded, no open of route read" $?

echo; echo "=== a basket a server serves as zeros: the read is made again at another site"
FIX="$HERE/../data/unnamed_codec_fixture.root" # Jet_pt basket 1: 2481 bytes at 4915 (ci.yml's key-position step)
placefix() { # LFN server... (the fixture)
  local lfn=$1 s; shift
  for s in "$@"; do
    install -d -m 777 "$W/data/$s/$(dirname "$lfn" | sed 's|^/||')"
    cp "$FIX" "$W/data/$s$lfn"; chmod 666 "$W/data/$s$lfn"
  done
}
zero_at() { # LFN server... : the basket zeroed in the copies on those servers
  local lfn=$1 s; shift
  for s in "$@"; do
    dd if=/dev/zero of="$W/data/$s$lfn" bs=64K seek=4915 count=2481 oflag=seek_bytes iflag=count_bytes conv=notrunc status=none
  done
}
# What the test client runs, with the port of the server its handle landed on, to zero the basket there.
cat > "$W/zero.sh" <<ZERO
#!/bin/sh
case \$1 in $AX) s=a ;; $BX) s=b ;; $CX) s=c ;; *) exit 1 ;; esac
dd if=/dev/zero of="$W/data/\$s\$ZERO_LFN" bs=64K seek=4915 count=2481 oflag=seek_bytes iflag=count_bytes conv=notrunc status=none
ZERO
chmod 755 "$W/zero.sh"
# zr CASE LFN SWITCH [land] [CMD]: plugin_fuzz's read-failover CASE on LFN (the check on); `land`: the basket is
# zeroed on the server the handle lands on (by CMD instead of zero.sh when given: the copy damaged otherwise, with
# ZREXTRA's settings for the case's check of the fault). Sets RES, RFC (its cache), SV (landed, final), F14 (failed
# reads with code 14), FL (their servers), MV (where each move landed) and NRA (attempts of route read).
ZREXTRA=()
zr() {
  local c=$1 l=$2 sw=$3 land=${4:-} cmd=${5:-$W/zero.sh} n
  n=$(echo "$l" | tr / _)
  RFC="$W/c-zr$n"
  ZRLOG="$W/zr$n.log"
  FEDENV=(UCACHE_ADAPTIVE_FEDERATION=$sw UCACHE_CHECK_ORIGIN_INTEGRITY=on ${land:+UCACHE_ZERO_CMD=$cmd ZERO_LFN=$l} "${ZREXTRA[@]}")
  [ "$land" = landref ] && FEDENV+=(UCACHE_TEST_LOCATE_REFRESH=1)
  RES=$(fuzz "$RFC" "$W/zr$n.log" "root://127.0.0.1:$RX/$l" "$FIX" 1 1 1 read-failover $c)
  echo "$RES" | grep -v '^PASS' | sed 's/^/    /' | cut -c1-220
  SV=($(echo "$RES" | sed -n 's/^SERVERS //p' | pnorm))
  F14=$(recs "$RFC" origin_failure | grep -c '"code":14,')
  FL=($(recs "$RFC" origin_failure | grep '"code":14,' | field data_server | pnorm))
  MV=($(recs "$RFC" open | grep '"route":"read"' | grep '"outcome":"ok"' | field data_server | pnorm))
  NRA=$(recs "$RFC" open | grep -c '"route":"read"')
}
clean_zr() { echo "$RES" | grep -q "^read-failover $1: clean\$"; }
# (a) the copy the handle landed on serves zeros: a Read, a VectorRead
for c in zero zerov; do
  L=/store/ci/zr-$c.root; placefix $L a b c
  zr $c $L on land
  clean_zr $c; verdict "$c: the zeroed basket is refused and read exactly at another server, once (${SV[*]})" $?
  [ "$F14" = 1 ] && [ "${FL[0]:-x}" = "${SV[0]}" ]; verdict "$c: one failed read with code 14, at the server the handle landed on (${FL[*]})" $?
  [ "${#MV[@]}" -ge 1 ] && [ "${MV[-1]}" = "${SV[1]}" ]; verdict "$c: a move of route read ended at the server the bytes came from (${MV[*]})" $?
done
# (b) the first move may land on a copy that is damaged too: that answer is refused as well. Made certain (it
# was a loop of up to 30 files, which missed now and then): the file is placed on a alone and the regional asked
# where it is (it keeps that answer, a alone, and sends every open there); then on b, whole, and c, damaged. The
# handle lands on a, whose copy the hook zeroes. The read fails its check at a; the move's open is sent back to a,
# which a read found bad (refused as it is), so the regional is asked for its list, with refresh
# (UCACHE_TEST_LOCATE_REFRESH: uCache asks with refresh only after a "not found", which a server that still has
# the file open never answers): it lists c and b, in that order (as it lists its servers, c's port first). The move
# lands on c, damaged, whose answer is refused too, and then on b, which is exact.
L=/store/ci/zr-b.root; placefix $L a
RL=$(xfs 127.0.0.1:$RX locate "$L" 2>&1 | tr '\n' ' ')
placefix $L b c; zero_at $L c
zr zero $L on landref
echo "    the regional listed before the open: $RL"
clean_zr zero; verdict "two damaged copies: every read was exact, once, from the one that is not" $?
[ "$F14" = 2 ] && [ "${FL[0]:-}" = "$AX" ] && [ "${FL[1]:-}" = "$CX" ] && [ "${SV[1]:-}" = "$BX" ]
verdict "two damaged copies: the read failed its check at a, the move landed on c, damaged, and its answer was refused too, and the bytes came from b ($F14 failed reads at ${FL[*]}; ended at ${SV[1]:-none})" $?
case "$RL" in *":$AX "*) case "$RL" in *":$BX "*|*":$CX "*) r=1 ;; *) r=0 ;; esac ;; *) r=1 ;; esac
[ "$r" = 0 ] && [ "${SV[0]:-}" = "$AX" ]
verdict "two damaged copies: the regional listed a alone before the open, and the handle landed on a (the fault was in place: $RL; landed ${SV[0]:-none})" $?
# (c) every copy is damaged: the reader gets the error, after at most four reads, never the zeros
L=/store/ci/zr-all.root; placefix $L a b c; zero_at $L a b c
zr zeroerr $L on
clean_zr zeroerr; verdict "every copy damaged: the read fails its check, once, and never serves the zeros" $?
[ "$F14" -ge 1 ] && [ "$F14" -le 4 ]; verdict "every copy damaged: $F14 failed reads (one to four)" $?
# (e) the only copy is damaged: a copy a read found bad is never opened again for the handle, wherever
# the redirector or the list of copies sends the open; the error names it and says no copy could serve
L=/store/ci/zr-only.root; placefix $L a
zr zeroerr $L on land
clean_zr zeroerr; verdict "only copy damaged: the read fails its check, once, and never serves the zeros" $?
NOK=$(recs "$RFC" open | grep '"route":"read"' | grep -c '"outcome":"ok"')
[ "$F14" = 1 ] && [ "$NOK" = 0 ]; verdict "only copy damaged: one failed read, and no move took the copy again ($F14 failed reads, $NOK moves ended ok, $NRA attempts)" $?
echo "$RES" | grep -q "no copy could serve the read: 127.0.0.1 (127.0.0.1:$AX, $(stat -c %s "$FIX") bytes): .*failed its check"
verdict "only copy damaged: the error says no copy could serve the read, and names the copy with its size" $?
# (d) the switch off, the check on: the same read fails its check and is not moved
L=/store/ci/zr-off.root; placefix $L a b c; zero_at $L a b c
zr zeroerr $L off
clean_zr zeroerr; verdict "switch off: the read fails its check, once, and never serves the zeros" $?
[ "$F14" = 1 ] && [ "$NRA" = 0 ]; verdict "switch off: one failed read, no open of route read ($F14, $NRA)" $?

echo; echo "=== a basket whose key states another length, a tree's record that fails its own check: the read moves"
# The file map (check_origin_integrity): a basket fetched must state the length the tree's record gives it, and a
# tree's record that fails its own check shows its copy bad. The fixture's Jet_pt basket at 4915 (2481 bytes, its key
# 75 bytes, its object 12008) made to state 12083 bytes, the length of a basket stored as it is (ROOT then takes its
# compressed bytes for its entries: wrong values, no error); the fixture's tree record at 11210 (717 bytes, zlib, its
# label 46 bytes) with one byte of its compressed data changed, at 11296 (ROOT finds no tree). Each by a script the
# test client runs with the port of the server its handle landed on, after the open.
cat > "$W/len.sh" <<LENSH
#!/bin/sh
case \$1 in $AX) s=a ;; $BX) s=b ;; $CX) s=c ;; *) exit 1 ;; esac
printf '\000\000\057\063' | dd of="$W/data/\$s\$ZERO_LFN" bs=1 seek=4915 conv=notrunc status=none
LENSH
rec_at() { # LFN server... : one byte of the tree's record's compressed data changed in the copies on those servers
  local lfn=$1 s; shift
  for s in "$@"; do printf '\060' | dd of="$W/data/$s$lfn" bs=1 seek=11296 conv=notrunc status=none; done
}
chmod 755 "$W/len.sh"
# failtext WHY: the failed reads whose warning, in the test client's log, gives WHY as the reason
failtext() { grep -c "failed its check ($1); not served" "$ZRLOG"; }
# (a) a basket whose key states another length, on the server the handle landed on, made after the open
ZREXTRA=(UCACHE_FAULT_AT=4915)
why="key length differs from the tree's record"
for c in len lenv; do
  L=/store/ci/zr-$c.root; placefix $L a b c
  zr $c $L on land "$W/len.sh"
  clean_zr $c; verdict "$c: the basket of another length is refused and read exactly at another server, once (${SV[*]})" $?
  [ "$F14" = 1 ] && [ "${FL[0]:-x}" = "${SV[0]}" ] && [ "$(failtext "$why")" = 1 ]
  verdict "$c: one failed read with code 14, at the server the handle landed on, its warning saying \"$why\" (${FL[*]})" $?
  [ "${#MV[@]}" -ge 1 ] && [ "${MV[-1]}" = "${SV[1]}" ]; verdict "$c: a move of route read ended at the server the bytes came from (${MV[*]})" $?
  ND=$(counter "$RFC" bad_entries_dropped); [ "$ND" = 1 ] && [ -z "$(find "$RFC/objects" -name '*.meta' 2>/dev/null)" ]
  verdict "$c: the entry, which holds bytes of the damaged copy (the checks' at open), is dropped (bad_entries_dropped $ND, no sidecar left)" $?
done
L=/store/ci/zr-len-off.root; placefix $L a b c
zr lenerr $L off land "$W/len.sh"
clean_zr lenerr; verdict "len, switch off: the read fails its check, once, and is never served" $?
[ "$F14" = 1 ] && [ "$NRA" = 0 ] && [ "$(failtext "$why")" = 1 ]
verdict "len, switch off: one failed read whose warning says \"$why\", no open of route read ($F14, $NRA)" $?
# (b) a tree's record that fails its own check. It must be damaged before the open: the checks made at open keep the
# record's pages (the reader reads it first), and the map is made from them. Made certain as (b) of the zeros above:
# the file placed on a alone and the regional asked where it is (it keeps that answer, a alone), a's copy damaged, then
# whole copies on b and c; the handle lands on a. The map made from a's record shows a bad: the read fails its check
# there, the move's open (sent back to a, which a read found bad) asks the regional for its list with refresh, and the
# read is exact at b or c.
# The read is of the record itself (11210, 717 bytes): a map that shows its copy bad refuses only the reads of
# the damaged tree's own parts (a read of a basket of the tree, which ROOT reaches only through the record, or
# of another tree, makes no claim; below).
ZREXTRA=(UCACHE_FAULT_AT=11296 UCACHE_FAULT_LEN=1 UCACHE_READ_AT=11210 UCACHE_READ_LEN=717)
why="the tree's record fails its own check"
for c in rec recv; do
  L=/store/ci/zr-$c.root; placefix $L a
  RL=$(xfs 127.0.0.1:$RX locate "$L" 2>&1 | tr '\n' ' ')
  rec_at $L a; placefix $L b c
  zr $c $L on landref /bin/true
  clean_zr $c; verdict "$c: a's tree's record fails its own check: the read is refused there and read exactly at another server, once (${SV[*]})" $?
  [ "$F14" = 1 ] && [ "${FL[0]:-x}" = "$AX" ] && [ "${SV[0]:-}" = "$AX" ] && [ "$(failtext "$why")" = 1 ]
  verdict "$c: the handle landed on a (the regional listed $RL), one failed read with code 14 at a, its warning saying \"$why\" (${FL[*]})" $?
  [ "${#MV[@]}" -ge 1 ] && [ "${MV[-1]}" = "${SV[1]}" ] && [ "${SV[1]}" != "$AX" ]; verdict "$c: a move of route read ended at the server the bytes came from, not a (${MV[*]})" $?
  ND=$(counter "$RFC" bad_entries_dropped); [ "$ND" = 1 ] && [ -z "$(find "$RFC/objects" -name '*.meta' 2>/dev/null)" ]
  verdict "$c: the entry, which holds a's record, is dropped (bad_entries_dropped $ND, no sidecar left)" $?
done
L=/store/ci/zr-rec-off.root; placefix $L a b c; rec_at $L a b c
zr recerr $L off
clean_zr recerr; verdict "rec, switch off: the read fails its check, once, and is never served" $?
[ "$F14" = 1 ] && [ "$NRA" = 0 ] && [ "$(failtext "$why")" = 1 ]
verdict "rec, switch off: one failed read whose warning says \"$why\", no open of route read ($F14, $NRA)" $?
ZREXTRA=()

echo; echo "=== a basket whose compressed data fails its own checksum: the read moves"
# The block checksum (the check on): the fixture's Jet_pt basket at 4915 (2481 bytes, ZLIB, its key 75 bytes) with
# one byte of its compressed data changed, at 5999 (157 made 141), its key intact, on the server the handle
# landed on, after the open (ROOT fails to decompress it: its adler32 does not hold).
cat > "$W/blk.sh" <<BLKSH
#!/bin/sh
case \$1 in $AX) s=a ;; $BX) s=b ;; $CX) s=c ;; *) exit 1 ;; esac
printf '\215' | dd of="$W/data/\$s\$ZERO_LFN" bs=1 seek=5999 conv=notrunc status=none
BLKSH
chmod 755 "$W/blk.sh"
ZREXTRA=(UCACHE_FAULT_AT=5999 UCACHE_FAULT_LEN=1)
why="its data fails its own check: zlib block failed its check or did not decode"
for c in blk blkv blkpg; do
  L=/store/ci/zr-$c.root; placefix $L a b c
  zr $c $L on land "$W/blk.sh"
  clean_zr $c; verdict "$c: the basket whose data fails its checksum is refused and read exactly at another server, once (${SV[*]})" $?
  [ "$F14" = 1 ] && [ "${FL[0]:-x}" = "${SV[0]}" ] && [ "$(failtext "$why")" = 1 ]
  verdict "$c: one failed read with code 14, at the server the handle landed on, its warning saying \"$why\" (${FL[*]})" $?
  [ "${#MV[@]}" -ge 1 ] && [ "${MV[-1]}" = "${SV[1]}" ]; verdict "$c: a move of route read ended at the server the bytes came from (${MV[*]})" $?
  ND=$(counter "$RFC" bad_entries_dropped); [ "$ND" = 1 ] && [ -z "$(find "$RFC/objects" -name '*.meta' 2>/dev/null)" ]
  verdict "$c: the entry, which holds bytes of the damaged copy (the checks' at open), is dropped (bad_entries_dropped $ND, no sidecar left)" $?
  NP=$(counter "$RFC" origin_checks_passed); [ "${NP:-0}" -ge 1 ]
  verdict "$c: baskets whose checksum held are counted (origin_checks_passed ${NP:-0})" $?
done
L=/store/ci/zr-blk-off.root; placefix $L a b c
zr blkerr $L off land "$W/blk.sh"
clean_zr blkerr; verdict "blk, switch off: the read fails its check, once, and is never served" $?
[ "$F14" = 1 ] && [ "$NRA" = 0 ] && [ "$(failtext "$why")" = 1 ]
verdict "blk, switch off: one failed read whose warning says \"$why\", no open of route read ($F14, $NRA)" $?
ZREXTRA=()

echo; echo "=== a tree's record that fails its own check concerns that tree only: a read of another tree stays"
# tests/data/two_trees_fixture.root: Events (its record at 9094, 694 bytes, ZLIB, its label 46 bytes) and Runs (its
# baskets at 4627, 82 and 104 bytes, on a page the checks at open do not keep). Events' record with one byte of its
# compressed data changed, at 9449 (156 made 140), on a, before the open; whole copies on b and c; the handle lands
# on a (as the rec cases above). A read of Runs' baskets is exact at a, with no failed read and no move (ROOT reads
# Runs from that copy); a read of Events' record is refused there and read exactly at another server.
FIXONE=$FIX
FIX="$HERE/../data/two_trees_fixture.root"
for c in stay rec2; do
  L=/store/ci/zr-two-$c.root; placefix $L a
  RL=$(xfs 127.0.0.1:$RX locate "$L" 2>&1 | tr '\n' ' ')
  printf '\214' | dd of="$W/data/a$L" bs=1 seek=9449 conv=notrunc status=none
  placefix $L b c
  if [ $c = stay ]; then
    ZREXTRA=(UCACHE_FAULT_AT=9449 UCACHE_FAULT_LEN=1 UCACHE_READ_AT=4627 UCACHE_READ_LEN=186)
    zr stay $L on landref /bin/true
    clean_zr stay; verdict "two trees: a read of Runs' baskets at a, whose Events record fails its check, is exact and stays at a (${SV[*]})" $?
    [ "$F14" = 0 ] && [ "${SV[0]:-}" = "$AX" ] && [ "${#MV[@]}" = 0 ]
    verdict "two trees: the handle landed on a (the regional listed $RL), no failed read, no move ($F14 failed, ${#MV[@]} moves)" $?
  else
    ZREXTRA=(UCACHE_FAULT_AT=9449 UCACHE_FAULT_LEN=1 UCACHE_READ_AT=9094 UCACHE_READ_LEN=694)
    zr rec $L on landref /bin/true
    clean_zr rec; verdict "two trees: a read of Events' record at a is refused and read exactly at another server (${SV[*]})" $?
    [ "$F14" = 1 ] && [ "${FL[0]:-x}" = "$AX" ] && [ "$(failtext "the tree's record fails its own check")" = 1 ]
    verdict "two trees: one failed read with code 14 at a, the record's failure (${FL[*]})" $?
  fi
done
FIX=$FIXONE
ZREXTRA=()

echo; echo "=== a copy cut short after the open: a read inside the file's size is answered short"
CUTAT=30000
for v in cut cutall; do
  cat > "$W/$v.sh" <<CUTSH
#!/bin/sh
case \$1 in $AX) s=a ;; $BX) s=b ;; $CX) s=c ;; *) exit 1 ;; esac
CUTTARGETS=\$s; [ $v = cutall ] && CUTTARGETS="a b c"
for t in \$CUTTARGETS; do truncate -s $CUTAT "$W/data/\$t\$CUT_LFN"; done
CUTSH
  chmod 755 "$W/$v.sh"
done
# ct CASE SCRIPT LFN : plugin_fuzz's read-failover CASE on LFN (three whole copies), the script run after the open
ct() {
  local c=$1 script=$2 l=$3 n
  n=$(echo "$l" | tr / _)
  placew $l a b c
  RFC="$W/c-ct$n"
  FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on ${script:+UCACHE_CUT_CMD=$W/$script.sh CUT_LFN=$l})
  RES=$(fuzz "$RFC" "$W/ct$n.log" "root://127.0.0.1:$RX/$l" "$FIXW" 1 1 1 read-failover $c)
  echo "$RES" | grep -v '^PASS' | sed 's/^/    /' | cut -c1-300
  SV=($(echo "$RES" | sed -n 's/^SERVERS //p' | pnorm))
  F14=$(recs "$RFC" origin_failure | grep -c '"op":"read".*"code":14,\|"op":"relay".*"code":14,')
  F14OPS=$(recs "$RFC" origin_failure | grep '"code":14,' | field op | tr '\n' ' ')
  NRA=$(recs "$RFC" open | grep -c '"route":"read"')
}
ct cut cut /store/ci/ct-cut.root
echo "$RES" | grep -q '^read-failover cut: clean$'; verdict "cut: the read past the cut is the file's own bytes, from another server, once (${SV[*]})" $?
[ "$F14" = 1 ] && [ "$F14OPS" = "read " ]; verdict "cut: the short answer is one failed read of class data, code 14, op read: a miss of the cache ($F14, $F14OPS)" $?
recs "$RFC" open | grep '"route":"read"' | grep '"outcome":"ok"' | tail -1 | grep -q "\"size\":$FIXW_SIZE,"
verdict "cut: a move of route read ended at a copy of $FIXW_SIZE bytes" $?
[ "$(counter "$RFC" origin_failures_data)" -ge 1 ]; verdict "cut: origin_failures_data counts it" $?
# The handle's entry holds what the checks at open read of the copy it was opened at: the copy found bad, the
# entry is dropped (nothing of a bad copy stays cached)
dropped() { [ "$(counter "$RFC" bad_entries_dropped)" = 1 ] && [ -z "$(find "$RFC/objects" -name '*.meta' 2>/dev/null)" ]; }
dropped; r=$?; verdict "cut: the entry, which holds bytes of the cut copy, is dropped (bad_entries_dropped $(counter "$RFC" bad_entries_dropped), no sidecar left)" $r
# the same with a PgRead across the cut (from 20000), which the cache passes straight to the origin: a page read's short answer
ct cutpg cut /store/ci/ct-cutpg.root
echo "$RES" | grep -q '^read-failover cutpg: clean$'; verdict "cutpg: a PgRead across the cut is the file's own bytes, from another server, once (${SV[*]})" $?
[ "$F14" = 1 ] && [ "$F14OPS" = "relay " ]; verdict "cutpg: the short answer is one failed read of class data, code 14, op relay: the passed-through PgRead ($F14, $F14OPS)" $?
recs "$RFC" open | grep '"route":"read"' | grep '"outcome":"ok"' | tail -1 | grep -q "\"size\":$FIXW_SIZE,"
verdict "cutpg: a move of route read ended at a copy of $FIXW_SIZE bytes" $?
dropped; r=$?; verdict "cutpg: the entry, which holds bytes of the cut copy, is dropped (bad_entries_dropped $(counter "$RFC" bad_entries_dropped), no sidecar left)" $r
# the same with a PgRead wholly past the cut (from 40000): the copy answers it with no bytes, which the client
# fails itself with an invalid response (303) and leaves its File unusable; inside the file's confirmed size that
# is a data failure of the copy, and the read moves
ct cutpgpast cut /store/ci/ct-cutpgpast.root
echo "$RES" | grep -q '^read-failover cutpgpast: clean$'; verdict "cutpgpast: a PgRead wholly past the cut is the file's own bytes, from another server, once (${SV[*]})" $?
[ "$F14" = 1 ] && [ "$F14OPS" = "relay " ]; verdict "cutpgpast: the empty answer is one failed read of class data, code 14, op relay ($F14, $F14OPS)" $?
# the fault: the client itself failed the page read for the copy's empty answer (when a client takes such an
# answer, it is a short one, held against the copy as cutpg's is)
grep -q 'kXR_pgread (handle: [^,]*, offset: 40000, size: 65536): user supplied buffer is too small' "$W/ct_store_ci_ct-cutpgpast.root.log"
verdict "cutpgpast: the client failed the page read at 40000 itself, for an empty answer (its log): the fault was in place" $?
recs "$RFC" open | grep '"route":"read"' | grep '"outcome":"ok"' | tail -1 | grep -q "\"size\":$FIXW_SIZE,"
verdict "cutpgpast: a move of route read ended at a copy of $FIXW_SIZE bytes" $?
dropped; r=$?; verdict "cutpgpast: the entry, which holds bytes of the cut copy, is dropped for the empty answer (bad_entries_dropped $(counter "$RFC" bad_entries_dropped), no sidecar left)" $r
ct cutonly cutall /store/ci/ct-all.root
echo "$RES" | grep -q '^read-failover cutonly: clean$'; verdict "cutonly: every copy is cut, the read fails with the data error and says why" $?
[ "$F14" -ge 1 ] && [ "$F14" -le 4 ]; verdict "cutonly: $F14 failed reads (one to four)" $?
echo "$RES" | grep -v '^MSG2' | grep -q "no copy could serve the read: .*, $FIXW_SIZE bytes): answered .*inside the file's size of $FIXW_SIZE bytes.*, $CUTAT bytes): it serves $CUTAT bytes, the cached file has $FIXW_SIZE"
verdict "cutonly: the error says no copy could serve the read, and names the copies tried with their sizes" $?
# a second read made at once, while no move is made again: the first read's move's error, said to be that
echo "$RES" | grep '^MSG2' | grep -q "no copy could serve the read: a move made [0-9]* ms ago for an earlier read of the handle found none.*: .*answered .* asked at [0-9]*"
verdict "cutonly: a second read made at once gets the error of the move made for the first, which says whose offsets it names" $?
ct end "" /store/ci/ct-end.root
echo "$RES" | grep -q '^read-failover end: clean$'; verdict "end: reads that reach past the end of an intact file are answered short, exactly (a PgRead wholly past it as without uCache), and the handle stays (${SV[*]})" $?
[ "$F14" = 0 ] && [ "$NRA" = 0 ]; verdict "end: no failed read, no move ($F14, $NRA)" $?

# A read's data failure that arrives after the handle moved, on the File it left (UCACHE_TEST_READ_FAIL_KIND=leftdata;
# the fixture on a and b only, the copy the handle opened at cut after the open): the copy is marked as one the
# handle never opens again, as for a data failure on the File it reads from. Two Reads issued 400 ms apart at X: the
# first's miss fails (a connection error, 1 s late) and moves the handle to Y; the second, past the cut, is answered
# short by X 2.5 s late, after that move. X is then made whole again (UCACHE_RESTORE_CMD), so that only its mark keeps
# a move from it. A third Read fails at Y: its move stays at Y (an X marked only as a failure is taken again, as the
# oldest failure: the redirector's sending the move to X, which an address cannot keep it from, is refused as a copy
# whose read answered with bad bytes).
L=/store/ci/ct-leftdata.root
placew $L a b
RFC="$W/c-ct-leftdata"
cat > "$W/restore.sh" <<RESTSH
#!/bin/sh
case \$1 in $AX) s=a ;; $BX) s=b ;; $CX) s=c ;; *) exit 1 ;; esac
cp "$FIXW" "$W/data/\$s$L"
RESTSH
chmod 755 "$W/restore.sh"
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_TEST_READ_FAIL_N=3 UCACHE_TEST_READ_FAIL_KIND=leftdata UCACHE_CUT_CMD=$W/cut.sh CUT_LFN=$L UCACHE_RESTORE_CMD=$W/restore.sh)
RES=$(fuzz "$RFC" "$W/ct-leftdata.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 read-failover leftdata)
echo "$RES" | grep -v '^PASS' | sed 's/^/    /' | cut -c1-300
LD=($(echo "$RES" | sed -n 's/^LEFTDATA //p' | pnorm))
FL=($(recs "$RFC" origin_failure | field code)); FO=($(recs "$RFC" origin_failure | field off))
MV=($(recs "$RFC" open | grep '"route":"read"' | grep '"outcome":"ok"' | field data_server | pnorm))
recs "$RFC" origin_failure | sed 's/^/    fail: /' | cut -c1-240
echo "$RES" | grep -q '^read-failover leftdata: clean$'
verdict "leftdata: three Reads exact, once each; the third does not move the handle back to the copy that answered short (${LD[*]})" $?
[ "${#LD[@]}" = 3 ] && [ "${LD[0]}" != "${LD[1]}" ] && [ "${#FL[@]}" = 3 ] && [ "${FL[0]} ${FO[0]}" = "108 8192" ] &&
  [ "${FL[1]} ${FO[1]}" = "14 36864" ] && [ "${FL[2]} ${FO[2]}" = "108 16384" ]
verdict "leftdata: the faults struck as meant: a connection error at 8192, then the short answer past the cut (code 14, the page at 36864), then a connection error at 16384 (${FL[*]} at ${FO[*]})" $?
[ "${#MV[@]}" -ge 2 ] && [ "${MV[0]}" = "${LD[1]:-}" ] && ! printf '%s\n' "${MV[@]}" | grep -qx "${LD[0]:-none}" &&
  [ "$(stat -c %s "$W/data/a$L") $(stat -c %s "$W/data/b$L")" = "$FIXW_SIZE $FIXW_SIZE" ]
verdict "leftdata: the moves landed at Y, never at X, though X was whole again (${MV[*]}; X ${LD[0]:-}, Y ${LD[1]:-})" $?

echo; echo "=== a handle that lost its trust, whose own open then fails at every copy"
# The first run fills the cache with a freshness window, so that the second opens trusting the entry (no request
# at open). After that open the test removes the entry and has every copy refuse the file: the first read finds
# no entry and opens the file itself, which fails at every copy. Each read then gets that open's error.
L=/store/ci/degraded.root
placew $L a b c
DC="$W/c-degraded"
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_REVALIDATE_S=3600)
res=$(fuzz "$DC" "$W/degraded-fill.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-failover)
echo "$res" | grep -q '^open-failover: clean$' && [ -n "$(find "$DC" -name '*.meta' -print -quit)" ] && [ -n "$(find "$DC" -name '*.val' -print -quit)" ]
verdict "lost trust: the first run read the file and left its entry and its freshness marker" $?
N0=$(recs "$DC" open | wc -l) # the first run's open records; the second run's follow them
cat > "$W/degraded.sh" <<DEGSH
#!/bin/sh
find "$DC" -name '*.meta' -delete
for s in a b c; do chmod 000 "$W/data/\$s$L"; done
DEGSH
chmod 755 "$W/degraded.sh"
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_REVALIDATE_S=3600 UCACHE_CUT_CMD=$W/degraded.sh)
RES=$(fuzz "$DC" "$W/degraded.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 read-failover degraded)
echo "$RES" | grep -v '^PASS' | sed 's/^/    /' | cut -c1-600
recs "$DC" open | tail -n +$((N0 + 1)) | sed 's/^/    rec: /' | cut -c1-230
echo "$RES" | grep -q '^read-failover degraded: clean$'; verdict "lost trust: a Read and a PgRead each fail once with the open's error, which names the copies (never the client's error for a File that never opened)" $?
# the fault, from the client's log (which a client that dies still leaves): the handle was trusted at its open, and
# its own open was then refused at each of the three copies
NREF=0
for p in $AX $BX $CX; do
  grep -q "\[127.0.0.1:$p\] Handling error while processing kXR_open (file: $L,.*permission denied" "$W/degraded.log" && NREF=$((NREF + 1))
done
grep -q 'trusted recent cache for ' "$W/degraded.log" && [ "$NREF" = 3 ] && [ -z "$(find "$DC" -name '*.meta' -print -quit)" ]
verdict "lost trust: the handle was trusted at its open, its entry was gone, and its own open was refused at each copy ($NREF of 3): the fault was in place" $?
NDG=$(recs "$DC" open | tail -n +$((N0 + 1)) | grep -c '"route":"degraded"'); NOK=$(recs "$DC" open | tail -n +$((N0 + 1)) | grep -c '"status":"ok"')
[ "$NDG" -ge 1 ] && [ "$NOK" = 0 ]
verdict "lost trust: the records show the handle's own open (route degraded, then its failovers), none ok ($NDG degraded, $NOK ok)" $?
n=0; for p in $AX $BX $CX; do echo "$RES" | grep '^MSG read' | grep -q "(127.0.0.1:$p)" && n=$((n + 1)); done
[ "$n" = 3 ]; verdict "lost trust: the Read's error names each of the three copies ($n of 3)" $?
# No read is given to the File that never opened: the client answers such a read itself with "Invalid operation"
# and sends nothing (so its log cannot show it), and a PgRead there brought the test client down. That answer is
# what a read given to it would get.
! echo "$RES" | grep -q 'Invalid operation'
verdict "lost trust: no read got the client's answer for a File that never opened (Invalid operation): none was given to it" $?

echo; echo "=== the same with the switch off (the default)"
# The same fault with adaptive_federation off: each request (a Read, a VectorRead, a Stat, an Fcntl, a Visa, then a
# PgRead) gets the handle's own open's error, the server's refusal,
# which is what the reader's open would have got without the cache; no copy is named and no other is tried. Then
# the test makes the copies readable again (UCACHE_RESTORE_CMD), and after the open's cool-down (a second) a Read
# and a PgRead are exact: the open is made again and stands.
L=/store/ci/degraded-off.root
placew $L a b c
DO="$W/c-degraded-off"
FEDENV=(UCACHE_REVALIDATE_S=3600)
res=$(fuzz "$DO" "$W/degraded-off-fill.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-failover)
echo "$res" | grep -q '^open-failover: clean$' && [ -n "$(find "$DO" -name '*.meta' -print -quit)" ] && [ -n "$(find "$DO" -name '*.val' -print -quit)" ]
verdict "lost trust, switch off: the first run read the file and left its entry and its freshness marker" $?
N0=$(recs "$DO" open | wc -l)
cat > "$W/degraded-off.sh" <<DEGSH
#!/bin/sh
find "$DO" -name '*.meta' -delete
for s in a b c; do chmod 000 "$W/data/\$s$L"; done
DEGSH
cat > "$W/degraded-off-restore.sh" <<DEGSH
#!/bin/sh
for s in a b c; do chmod 666 "$W/data/\$s$L"; done
DEGSH
chmod 755 "$W/degraded-off.sh" "$W/degraded-off-restore.sh"
FEDENV=(UCACHE_REVALIDATE_S=3600 UCACHE_CUT_CMD=$W/degraded-off.sh UCACHE_RESTORE_CMD=$W/degraded-off-restore.sh)
RES=$(fuzz "$DO" "$W/degraded-off.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 read-failover degradedoff)
echo "$RES" | grep -v '^PASS' | sed 's/^/    /' | cut -c1-600
recs "$DO" open | tail -n +$((N0 + 1)) | sed 's/^/    rec: /' | cut -c1-230
echo "$RES" | grep -q '^read-failover degradedoff: clean$'
verdict "lost trust, switch off: a Read, a VectorRead, a Stat, an Fcntl, a Visa and a PgRead each fail once with the open's own error (the server's refusal), and once the copies are readable again a Read and a PgRead are exact" $?
NPD=$(echo "$RES" | grep '^MSG ' | grep -c 'permission denied')
[ "$NPD" = 6 ] && ! echo "$RES" | grep '^MSG ' | grep -q 'no copy could serve'
verdict "lost trust, switch off: the errors of the Read, VectorRead, Stat, Fcntl, Visa and PgRead are the server's refusal, permission denied ($NPD of 6), naming no copies" $?
# A read given to the File that never opened gets the client's own answer, "Invalid operation" (it sends nothing,
# so the client's log cannot show it), and a PgRead there brings the test client down: the line it prints after
# the PgRead, and its last line, are the client still running.
! echo "$RES" | grep -q 'Invalid operation' && echo "$RES" | grep -q '^MSG pgread ' && echo "$RES" | grep -q '^read-failover degradedoff: '
verdict "lost trust, switch off: no request got the client's answer for a File that never opened (Invalid operation), and the client ran on after the PgRead" $?
# No read in the client's log until the open made again stood (the reads after it are the exact ones).
FIRSTOK=$(grep -n 'successfully opened at' "$W/degraded-off.log" | head -1 | cut -d: -f1)
FIRSTRD=$(grep -nE 'MsgHandler created.*kXR_(pg)?read ' "$W/degraded-off.log" | head -1 | cut -d: -f1)
[ -n "$FIRSTOK" ] && [ -n "$FIRSTRD" ] && [ "$FIRSTRD" -gt "$FIRSTOK" ]
verdict "lost trust, switch off: no read in the client's log before the open made again stood (first read at line ${FIRSTRD:-none}, the open at ${FIRSTOK:-none})" $?
# the fault: trusted at its open, the entry gone, the origin refusing; with the switch off no locate is sent
NREF=$(grep -c "Handling error while processing kXR_open (file: $L,.*permission denied" "$W/degraded-off.log")
grep -q 'trusted recent cache for ' "$W/degraded-off.log" && [ "$NREF" -ge 1 ] && [ -z "$(find "$DO" -name '*.meta' -print -quit)" ] &&
  [ "$(regional_locates $L)" = 0 ]
verdict "lost trust, switch off: the handle was trusted at its open, its entry was gone, its own open was refused ($NREF), and no locate was sent (the fault was in place)" $?
DREC=$(recs "$DO" open | tail -n +$((N0 + 1)))
echo "$DREC" | head -1 | grep -q '"route":"degraded","status":"error","code":400,"errno":3010,' &&
  echo "$DREC" | tail -1 | grep -q '"route":"degraded","status":"ok"' && [ "$(echo "$DREC" | grep -c '"route":"failover"\|"route":"candidate"')" = 0 ]
verdict "lost trust, switch off: the records show the handle's own open refused (route degraded, 400/3010), made again once the copies were readable (ok), and no failover" $?
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on)

echo; echo "=== a trusted handle's own open: an Fcntl or a Visa first, another version at every copy"
# A handle opened trusting its entry (a freshness window) is given an Fcntl (a Visa) before its first read: the
# cache's own open of the origin, which that needs, must find the size the entry holds. The test puts another
# version of the file at every copy after the open: 4 KiB longer, with other bytes at 85000 (inside a basket, whose
# key stays as it is, so that the key-position check finds nothing wrong there). Every copy is refused for its
# size, and a Read at 85000, which the entry does not hold, fails with the error naming that size, never the other
# version's bytes. Certain for any copy the redirector picks.
{ head -c 85000 "$FIXW"; head -c 4096 /dev/zero | tr '\0' 'Z'; tail -c +$((85000 + 4096 + 1)) "$FIXW"; head -c 4096 /dev/zero; } > "$W/fixw-other.bin"
OTHER_SIZE=$(stat -c %s "$W/fixw-other.bin")
[ "$OTHER_SIZE" = $((FIXW_SIZE + 4096)) ]; verdict "the other version is $OTHER_SIZE bytes, 4 KiB more than the file" $?
for kind in fcntl visa; do
  L=/store/ci/own-$kind.root
  placew $L a b c
  OC="$W/c-own-$kind"
  FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_REVALIDATE_S=3600)
  res=$(fuzz "$OC" "$W/own-$kind-fill.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-failover)
  echo "$res" | grep -q '^open-failover: clean$' && [ -n "$(find "$OC" -name '*.val' -print -quit)" ]
  verdict "own $kind: the first run read the file and left its entry and its freshness marker" $?
  N0=$(recs "$OC" open | wc -l)
  printf '#!/bin/sh\nfor s in a b c; do cp "%s" "%s/data/$s%s"; chmod 666 "%s/data/$s%s"; done\n' "$W/fixw-other.bin" "$W" "$L" "$W" "$L" > "$W/own-$kind.sh"
  chmod 755 "$W/own-$kind.sh"
  FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_REVALIDATE_S=3600 UCACHE_CUT_CMD=$W/own-$kind.sh)
  RES=$(fuzz "$OC" "$W/own-$kind.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 read-failover own$kind)
  echo "$RES" | grep -v '^PASS' | sed 's/^/    /' | cut -c1-400
  echo "$RES" | grep -q "^read-failover own$kind: clean$"
  verdict "own $kind: answered once, and a Read the entry does not hold fails naming the copies' other size, never the other version's bytes" $?
  NO=$(recs "$OC" open | tail -n +$((N0 + 1)) | grep -c '"size":'"$OTHER_SIZE")
  grep -q 'trusted recent cache for ' "$W/own-$kind.log" && [ "$NO" -ge 1 ] && [ "$(stat -c %s "$W/data/a$L")" = "$OTHER_SIZE" ]
  verdict "own $kind: the handle was trusted at its open, and its own open met the other version (records of size $OTHER_SIZE: $NO; the fault was in place)" $?
done

echo; echo "=== a trusted handle whose entry is gone: its own open makes the checks made at open"
# The entry is removed after the trusted open, so the handle's first request opens the file itself, with no size
# it must find. Every copy cut short (its header intact): that open makes the checks made at open and refuses each,
# so a Stat and a Read past the cut fail with the error naming the copies; never the cut size, nor an empty answer
# past it. Certain for any copy the redirector picks. Then every copy whole: the checks pass, the Stat answers the
# file's size and a Read is exact.
for kind in cut whole; do
  L=/store/ci/degraded-$kind.root
  placew $L a b c
  DK="$W/c-degraded-$kind"
  FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_REVALIDATE_S=3600)
  res=$(fuzz "$DK" "$W/degraded-$kind-fill.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 open-failover)
  echo "$res" | grep -q '^open-failover: clean$' && [ -n "$(find "$DK" -name '*.val' -print -quit)" ]
  verdict "degraded $kind: the first run read the file and left its entry and its freshness marker" $?
  N0=$(recs "$DK" open | wc -l)
  {
    echo '#!/bin/sh'
    echo "find \"$DK\" -name '*.meta' -delete"
    [ "$kind" = cut ] && echo "for s in a b c; do cp \"$W/fixw-cut.bin\" \"$W/data/\$s$L\"; done"
  } > "$W/degraded-$kind.sh"
  chmod 755 "$W/degraded-$kind.sh"
  FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on UCACHE_REVALIDATE_S=3600 UCACHE_CUT_CMD=$W/degraded-$kind.sh)
  RES=$(fuzz "$DK" "$W/degraded-$kind.log" "root://127.0.0.1:$RX/$L" "$FIXW" 1 1 1 read-failover degraded$kind)
  echo "$RES" | grep -v '^PASS' | sed 's/^/    /' | cut -c1-400
  recs "$DK" open | tail -n +$((N0 + 1)) | sed 's/^/    rec: /' | cut -c1-230
  echo "$RES" | grep -q "^read-failover degraded$kind: clean$"; r=$?
  if [ "$kind" = cut ]; then
    verdict "degraded, every copy cut: a Stat and a Read past the cut fail with the error naming the copies and their header's end; never the cut size" $r
  else
    verdict "degraded, every copy whole: the Stat answers the file's size and a Read is exact (the checks refuse no whole copy)" $r
  fi
  NDG=$(recs "$DK" open | tail -n +$((N0 + 1)) | grep -c '"route":"degraded"')
  SZ=$(for x in a b c; do stat -c %s "$W/data/$x$L"; done | sort -u | tr '\n' ' ')
  WANT=$([ "$kind" = cut ] && echo "$CUT " || echo "$FIXW_SIZE ")
  grep -q 'trusted recent cache for ' "$W/degraded-$kind.log" && [ "$NDG" -ge 1 ] && [ "$SZ" = "$WANT" ]
  verdict "degraded $kind: trusted at its open, its entry gone so that its own open was made (route degraded: $NDG), every copy $SZ bytes (the fault was in place)" $?
  NH=$(grep -cE 'MsgHandler created.*kXR_read \(handle: 0x[0-9a-f]*, offset: 0, size: 300\)' "$W/degraded-$kind.log")
  [ "$NH" -ge 1 ]; verdict "degraded $kind: the handle's own open read the copy's first 300 bytes, the checks made at open ($NH)" $?
done
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on)

echo; echo "fed_ci: $FAILS failure(s)"
exit $((FAILS > 0))
