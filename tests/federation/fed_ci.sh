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
#   all refused            a, b and c refuse the file: Open() returns OK, one
#                          completion, a failure that names all three servers
#   switch off             the default: a refused at a: some opens fail, the client
#                          sends no locate and the regional logs none for the file; the
#                          positive controls are the first leg's client log and an
#                          xrdfs locate of the leg's own
#   reads                  a read whose origin read is made to fail (UCACHE_TEST_READ_FAIL_N,
#                          and _KIND for where and how) is read again at another site,
#                          with adaptive_federation on, and answered with the file's own
#                          bytes, once: misses (a Read, then a VectorRead), relayed reads
#                          (a PgRead, a VectorRead into one buffer), reads the client
#                          refuses at once, eight failures of threads at one handle (also
#                          failing late), and a failure no other site would change, which
#                          stays at its server. Each case checks the records: the failed
#                          reads, each followed by an open with route read at another
#                          server, and open_failovers equal to those opens. With the
#                          switch off the failed reads are relayed on the same file and
#                          the client's read recovery stays on
#   zeros                  a copy that serves a basket as zeros (the fixture of the key-position
#                          check, tests/data/unnamed_codec_fixture.root): with the switch on the
#                          read fails its check and is made again at another site, for a Read and
#                          for a VectorRead, exactly, once; when the first move lands on a copy
#                          that is damaged too, that answer is refused as well; when every copy
#                          is damaged the reader gets errDataError, after at most four reads and
#                          never the zeros; with the switch off (the check on) it gets the error
#                          and no read is moved
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
printf 'url = 127.0.0.1:%s;127.0.0.1:%s;127.0.0.1:%s;127.0.0.1:%s\nlib = %s\nenable = true\n' \
  $RX $AX $BX $CX "$LIB" > "$W/conf/u.conf"
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
# masked), and the open goes on to a server the regional lists. Up to 40 files.
FEDENV=(UCACHE_ADAPTIVE_FEDERATION=on)
got=0 allok=1
for i in $(seq 40); do
  L=/store/ci/f5-$i.bin
  place $L b c
  chmod 000 "$W/data/c$L"
  res=$(fuzz "$W/c5-$i" "$W/f5-$i.log" "root://127.0.0.1:$RX/$L" "$W/f.bin" 1 1 1 open-failover)
  echo "$res" | grep -q '^open-failover: clean$' || { allok=0; echo "$res" | sed 's/^/    /'; break; }
  if [ "$(records "$W/c5-$i" | grep -c '"route":"candidate","status":"ok"')" -ge 1 ]; then got=$i; break; fi
done
[ "$allok" = 1 ]; verdict "every open succeeded, one completion, right bytes" $?
[ "$got" != 0 ]; verdict "an open was sent back to c and went to a listed server (file $got of at most 40)" $?
if [ "$got" != 0 ]; then
  records "$W/c5-$got" | sed 's/^/    rec: /' | cut -c1-230
  direct_read c $L; r=$?; [ "$r" != 0 ]; verdict "c refuses the file (a direct read fails)" $?
  NL=$(grep 'MsgHandler created' "$W/f5-$got.log" | grep -c 'kXR_locate ')
  [ "$NL" -ge 1 ]; verdict "the client's log shows kXR_locate ($NL)" $?
  [ "$(regional_locates $L)" -ge "$NL" ]; verdict "the regional's log shows the locate ($(regional_locates $L) requests)" $?
  FL=$(counter "$W/c5-$got" failover_locates)
  [ "${FL:-0}" = "$NL" ]; verdict "failover_locates is ${FL:-0}, the locates the client sent ($NL)" $?
  records "$W/c5-$got" | tail -1 | grep -q '"outcome":"ok"'; verdict "the last record line says outcome ok" $?
fi
L5=$L

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
  L10=/store/ci/f10.bin
  place $L10 a b c
  chmod 000 "$W/data/a$L10"
  direct_read a $L10; r=$?; [ "$r" != 0 ]; verdict "a refuses the file" $?
  RL0=$(regional_locates $L10)
  FEDENV=()
  res=$(fuzz "$W/c10" "$W/f10.log" "root://127.0.0.1:$RX/$L10" "$W/f.bin" 30 1 1 open-failover)
  echo "$res" | tail -2 | sed 's/^/    /'
  echo "$res" | grep -q '^open-failover: FAIL$'; verdict "some opens failed: the refusal was felt and nothing failed over" $?
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
# rf CASE N SWITCH [KIND]: the case on a file of its own; sets RES, RFC (its cache), SV (the servers it read from)
rf() {
  local c=$1 n=$2 sw=$3 kind=${4:-}
  local l=/store/ci/rf-$c-${kind:-x}.bin
  place8 $l a b c
  RFC="$W/c-rf-$c-${kind:-x}"
  FEDENV=(UCACHE_ADAPTIVE_FEDERATION=$sw UCACHE_TEST_READ_FAIL_N=$n ${kind:+UCACHE_TEST_READ_FAIL_KIND=$kind})
  RES=$(fuzz "$RFC" "$W/rf-$c-${kind:-x}.log" "root://127.0.0.1:$RX/$l" "$W/f8.bin" 1 1 1 read-failover $c)
  echo "$RES" | grep -v '^PASS' | sed 's/^/    /' | cut -c1-220
  SV=($(echo "$RES" | sed -n 's/^SERVERS //p'))
  FL=($(recs "$RFC" origin_failure | field data_server))   # the server of each failed read
  MV=($(recs "$RFC" open | grep '"route":"read"' | grep '"outcome":"ok"' | field data_server)) # where each move landed
  NRA=$(recs "$RFC" open | grep -c '"route":"read"')       # open attempts of moves
}
clean() { echo "$RES" | grep -q "^read-failover $1: clean\$"; }

rf miss 2 on
clean miss; verdict "miss: a Read and a VectorRead whose origin read failed are exact, once, and move the handle" $?
[ "${#FL[@]}" = 2 ] && [ "${FL[0]}" = "${SV[0]}" ] && [ "${FL[1]}" = "${SV[1]}" ]; verdict "miss: the failed reads name the servers the handle was at (${FL[*]})" $?
[ "${#MV[@]}" = 2 ] && [ "${MV[0]}" = "${SV[1]}" ] && [ "${MV[1]}" = "${SV[2]}" ]; verdict "miss: a move of route read after each landed at the next server (${MV[*]})" $?
[ "$(counter "$RFC" open_failovers)" = "$NRA" ]; verdict "miss: open_failovers $(counter "$RFC" open_failovers) equals the attempts of route read ($NRA)" $?

rf relay 2 on relay
clean relay; verdict "relay: a PgRead and a VectorRead into one buffer that failed are exact, once, and move the handle" $?
[ "$(recs "$RFC" origin_failure | field op | tr '\n' ' ')" = "relay relay " ] && [ "${#MV[@]}" = 2 ]; verdict "relay: two failed relayed reads, two moves (${MV[*]})" $?

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
# zr CASE LFN SWITCH [land]: plugin_fuzz's read-failover CASE on LFN (the check on); `land`: the basket is
# zeroed on the server the handle lands on. Sets RES, RFC (its cache), SV (landed, final), F14 (failed
# reads with code 14), FL (their servers), MV (where each move landed) and NRA (attempts of route read).
zr() {
  local c=$1 l=$2 sw=$3 land=${4:-} n
  n=$(echo "$l" | tr / _)
  RFC="$W/c-zr$n"
  FEDENV=(UCACHE_ADAPTIVE_FEDERATION=$sw UCACHE_CHECK_ORIGIN_INTEGRITY=on ${land:+UCACHE_ZERO_CMD=$W/zero.sh ZERO_LFN=$l})
  RES=$(fuzz "$RFC" "$W/zr$n.log" "root://127.0.0.1:$RX/$l" "$FIX" 1 1 1 read-failover $c)
  echo "$RES" | grep -v '^PASS' | sed 's/^/    /' | cut -c1-220
  SV=($(echo "$RES" | sed -n 's/^SERVERS //p'))
  F14=$(recs "$RFC" origin_failure | grep -c '"code":14,')
  FL=($(recs "$RFC" origin_failure | grep '"code":14,' | field data_server))
  MV=($(recs "$RFC" open | grep '"route":"read"' | grep '"outcome":"ok"' | field data_server))
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
# (b) the first move may land on a copy that is damaged too: that answer is refused as well. A new file
# each time, until a first move landed on one (up to 30).
hit=0 allok=1
for i in $(seq 30); do
  L=/store/ci/zr-b$i.root; placefix $L a b c; zero_at $L b
  zr zero $L on land
  clean_zr zero || { allok=0; break; }
  [ "$F14" -ge 2 ] && { hit=$i; break; }
done
[ "$allok" = 1 ]; verdict "two damaged copies: every read was exact, once, from the one that is not" $?
[ "$hit" != 0 ]; verdict "two damaged copies: a move landed on the damaged one and its answer was refused too (file $hit of at most 30; $F14 failed reads, ${#MV[@]} moves)" $?
# (c) every copy is damaged: the reader gets the error, after at most four reads, never the zeros
L=/store/ci/zr-all.root; placefix $L a b c; zero_at $L a b c
zr zeroerr $L on
clean_zr zeroerr; verdict "every copy damaged: the read fails its check, once, and never serves the zeros" $?
[ "$F14" -ge 1 ] && [ "$F14" -le 4 ]; verdict "every copy damaged: $F14 failed reads (one to four)" $?
# (d) the switch off, the check on: the same read fails its check and is not moved
L=/store/ci/zr-off.root; placefix $L a b c; zero_at $L a b c
zr zeroerr $L off
clean_zr zeroerr; verdict "switch off: the read fails its check, once, and never serves the zeros" $?
[ "$F14" = 1 ] && [ "$NRA" = 0 ]; verdict "switch off: one failed read, no open of route read ($F14, $NRA)" $?

echo; echo "fed_ci: $FAILS failure(s)"
exit $((FAILS > 0))
