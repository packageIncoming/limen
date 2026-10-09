#!/usr/bin/env bash
#
# run.sh - bidirectional ib_send_bw next to limen_bench --mode bandwidth, same session.
#
# Mirrors scripts/bench-server.sh and scripts/bench-client.sh: same devices, GID index,
# pinning, 64 KiB messages, depth 64, 100,000 iterations, 9 runs per measurement. The
# perftest reference gains -b (both ends send) and --report-both (print each direction).
# The one-way reference from the suite is repeated so all three come from one session.
#
# Needs no password. Card B is reached through the NOPASSWD helper limen-b-user, which
# enters limen-b and drops back to the calling user. CPU tuning goes through limen-tune
# and is restored on exit, including on Ctrl-C.
#
# Does not touch the MTU. Logs go under mtu<active_mtu>/ next to this script.

set -u

HERE=$(dirname "$(readlink -f "$0")")
cd "$HERE/../../.." || exit 1

SRV_DEV=${SRV_DEV:-rocep1s0f0}
CLI_DEV=${CLI_DEV:-rocep4s0f0}
PEER=${PEER:-192.168.100.1}
GID=${GID:-3}
PORT=${PORT:-18515}
SRV_CPU=${SRV_CPU:-1}
CLI_CPU=${CLI_CPU:-2}
BENCH=${BENCH:-./build/limen_bench}
SETS=${SETS:-2}

SIZE=65536
ITERS=100000
WARMUP=5000
DEPTH=64
RUNS=9

NSX="sudo -n /usr/local/sbin/limen-b-user"
TUNE="sudo -n /usr/local/sbin/limen-tune"
S="taskset -c $SRV_CPU"
C="$NSX taskset -c $CLI_CPU"

die() { printf '%s\n' "$*" >&2; exit 1; }

[[ -x $BENCH ]] || die "$BENCH not found or not executable"
$NSX true      || die "cannot enter limen-b without a password (limen-b-user)"

core_of() { cat "/sys/devices/system/cpu/cpu$1/topology/core_id"; }
[[ $(core_of "$SRV_CPU") != "$(core_of "$CLI_CPU")" ]] || die "cpu$SRV_CPU and cpu$CLI_CPU are SMT siblings"

mtu_of() { awk '/active_mtu/ { print $2 }'; }
MTU=$(ibv_devinfo -d "$SRV_DEV" | mtu_of)
MTU_B=$($NSX ibv_devinfo -d "$CLI_DEV" | mtu_of)
[[ -n $MTU && $MTU == "$MTU_B" ]] || die "active_mtu differs: $SRV_DEV=$MTU $CLI_DEV=$MTU_B"

OUT=$HERE/mtu$MTU
[[ ! -e $OUT/summary.txt || ${FORCE:-0} == 1 ]] || die "$OUT already holds a run. FORCE=1 to overwrite."
mkdir -p "$OUT"
SUM=$OUT/summary.txt
: > "$SUM"
: > "$OUT/counters.txt"

log() { printf '%s  %s\n' "$(date +%T)" "$*" | tee -a "$SUM"; }

SRV_PID=
restore() {
    [[ -n $SRV_PID ]] && kill -TERM "$SRV_PID" 2>/dev/null
    $TUNE off || true
    log "restored: governor=$(cat /sys/devices/system/cpu/cpu$SRV_CPU/cpufreq/scaling_governor), C-states enabled"
}
trap restore EXIT
trap 'exit 130' INT TERM

$TUNE on || die "limen-tune on failed"

# perftest exchanges setup over a real TCP socket, so ss can see it.
wait_tcp() {
    for _ in $(seq 1 200); do
        ss -ltnH "sport = :$PORT" 2>/dev/null | grep -q . && return 0
        sleep 0.05
    done
    return 1
}

# rdma_listen creates no socket, so ss cannot see limen_bench. Ask the RDMA stack.
wait_rdma() {
    for _ in $(seq 1 200); do
        rdma resource show cm_id 2>/dev/null | grep -q 'state LISTEN' && return 0
        sleep 0.05
    done
    return 1
}

# Port and hw counters of both cards, so traffic volume per direction and any
# RNR or retransmit activity can be read back per phase.
ctrs() {
    local a=/sys/class/infiniband/$SRV_DEV/ports/1 b=/sys/class/infiniband/$CLI_DEV/ports/1
    {
        printf '### %s\n' "$1"
        grep -H . $a/counters/port_*_data $a/counters/port_*_packets $a/hw_counters/* 2>/dev/null
        $NSX sh -c "grep -H . $b/counters/port_*_data $b/counters/port_*_packets $b/hw_counters/* 2>/dev/null"
    } >> "$OUT/counters.txt"
}

# RUNS fresh ib_send_bw pairs. Usage: pt <tag> [extra ib_send_bw args...]
pt() {
    local tag=$1 i rc src; shift
    ctrs "$tag before"
    for i in $(seq 1 $RUNS); do
        timeout 120 $S ib_send_bw -d "$SRV_DEV" -x "$GID" -s $SIZE -n $ITERS -t $DEPTH -F "$@" \
            > "$OUT/$tag-run$i.srv" 2>&1 &
        SRV_PID=$!
        wait_tcp || log "$tag run $i: no TCP listener on :$PORT"
        timeout 120 $C ib_send_bw -d "$CLI_DEV" -x "$GID" -s $SIZE -n $ITERS -t $DEPTH -F "$@" "$PEER" \
            > "$OUT/$tag-run$i.cli" 2>&1
        rc=$?
        wait "$SRV_PID"; src=$?
        SRV_PID=
        log "$tag run $i: client rc=$rc server rc=$src BW average MiB/s:$(awk -v s=$SIZE -v n=$ITERS \
            '$1 == s && $2 == n { printf " %s", $4 }' "$OUT/$tag-run$i.cli")"
    done
    ctrs "$tag after"
}

# One limen_bench bandwidth measurement, RUNS runs with reconnect, against a fresh echo server.
limen() {
    local tag=$1 rc
    ctrs "$tag before"
    $S "$BENCH" -d "$SRV_DEV" -s 1048576 --pipeline $DEPTH -n 1000000000 > "$OUT/$tag.srv" 2>&1 &
    SRV_PID=$!
    wait_rdma || log "$tag: no rdma_cm listener"
    timeout 300 $C "$BENCH" -d "$CLI_DEV" --mode bandwidth -s $SIZE --pipeline $DEPTH \
        -n $ITERS --warmup $WARMUP --runs $RUNS --json "$OUT/$tag.json" "$PEER" \
        > "$OUT/$tag.cli" 2>&1
    rc=$?
    kill -TERM "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null
    SRV_PID=
    log "$tag: client rc=$rc $(grep -E '^noise floor' "$OUT/$tag.cli")"
    ctrs "$tag after"
}

{
    printf 'date            %s\n' "$(date +%FT%T%z)"
    printf 'os / kernel     %s, Linux %s\n' "$(lsb_release -ds 2>/dev/null)" "$(uname -r)"
    printf 'perftest        %s\n' "$(ib_send_bw --version 2>&1 | head -1)"
    printf 'limen           %s\n' "$(git log -1 --format='%h %ci')"
    printf 'pinning         server cpu%s (core %s), client cpu%s (core %s)\n' \
        "$SRV_CPU" "$(core_of "$SRV_CPU")" "$CLI_CPU" "$(core_of "$CLI_CPU")"
    printf 'governor        %s\n' "$(cat /sys/devices/system/cpu/cpu$CLI_CPU/cpufreq/scaling_governor)"
    printf 'cstates         %s\n' "$(for s in /sys/devices/system/cpu/cpu$CLI_CPU/cpuidle/state*; do
        printf '%s:%s ' "$(cat "$s/name")" "$([[ $(cat "$s/disable") == 1 ]] && echo off || echo on)"; done)"
    printf 'clocksource     %s\n' "$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)"
    printf 'server card     %s, active_mtu %s, netdev mtu %s\n' "$SRV_DEV" "$MTU" \
        "$(cat /sys/class/infiniband/$SRV_DEV/device/net/*/mtu 2>/dev/null | head -1)"
    printf 'client card     %s, active_mtu %s, netdev mtu %s (netns limen-b)\n' "$CLI_DEV" "$MTU_B" \
        "$($NSX sh -c "cat /sys/class/infiniband/$CLI_DEV/device/net/*/mtu 2>/dev/null | head -1")"
    printf 'loadavg         %s\n' "$(cat /proc/loadavg)"
    printf 'message / depth %s B, tx depth %s, %s iterations, %s runs, %s sets\n' \
        "$SIZE" "$DEPTH" "$ITERS" "$RUNS" "$SETS"
} > "$OUT/conditions.txt"
cat "$OUT/conditions.txt"

log "start, output in $OUT"
for set in $(seq 1 "$SETS"); do
    pt    "perftest-bidir-set$set" -b --report-both
    limen "limen-bw-set$set"
    pt    "perftest-oneway-set$set"
done
log "done"
