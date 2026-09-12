#!/usr/bin/env bash
#
# bench-run.sh - run the full Limen measurement suite with fixed conditions.
#
#   ./scripts/bench-run.sh --all       one host, both sides, recommended
#   ./scripts/bench-run.sh --server    server side only  (terminal A)
#   ./scripts/bench-run.sh --client    client side only  (terminal B)
#
# perftest reference runs execute first, then limen_bench.
# The client owns CPU tuning and restores it on exit.

set -euo pipefail

SERVER_DEV=${SERVER_DEV:-rocep1s0f0}
CLIENT_DEV=${CLIENT_DEV:-rocep4s0f0}
PEER=${PEER:-192.168.100.1}
GID=${GID:-3}
PORT=${PORT:-18515}
SERVER_CPU=${SERVER_CPU:-1}
CLIENT_CPU=${CLIENT_CPU:-2}
BENCH=${BENCH:-./build/limen_bench}
NS=${NS:-limen-b}
OUT=${OUT:-docs/results}
TUNE=${TUNE:-1}

SRV="taskset -c $SERVER_CPU"
CLI="sudo $NS taskset -c $CLIENT_CPU"

die() { printf '%s\n' "$*" >&2; exit 1; }
note() { printf '\n=== %s\n' "$*" >&2; }

core_of() {
    local f=/sys/devices/system/cpu/cpu$1/topology/core_id
    [[ -r $f ]] || die "cannot read $f"
    cat "$f"
}

check_cpus() {
    local sc cc
    sc=$(core_of "$SERVER_CPU")
    cc=$(core_of "$CLIENT_CPU")
    [[ $SERVER_CPU != "$CLIENT_CPU" ]] || die "SERVER_CPU and CLIENT_CPU are the same CPU"
    [[ $sc != "$cc" ]] && return 0
    die "cpu$SERVER_CPU and cpu$CLIENT_CPU are SMT siblings on physical core $sc.
Pick two CPUs with different CORE values:
$(lscpu -e=CPU,CORE,SOCKET)
Then: SERVER_CPU=<a> CLIENT_CPU=<b> $0 $*"
}

wait_port() {
    for _ in $(seq 1 200); do
        ss -ltnH "sport = :$PORT" 2>/dev/null | grep -q . && return 0
        sleep 0.05
    done
    die "nothing listening on :$PORT after 10s"
}

wait_port_clear() {
    for _ in $(seq 1 200); do
        ss -ltnH "sport = :$PORT" 2>/dev/null | grep -q . || return 0
        sleep 0.05
    done
    return 0
}

tune_up() {
    [[ $TUNE == 1 ]] || return 0
    note "tuning: governor=performance, C-states disabled"
    sudo cpupower frequency-set -g performance >/dev/null
    sudo cpupower idle-set -D 0 >/dev/null
    trap tune_down EXIT INT TERM
}

tune_down() {
    note "restoring: governor=powersave, C-states enabled"
    sudo cpupower idle-set -E >/dev/null 2>&1 || true
    sudo cpupower frequency-set -g powersave >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------- server side

server_phases() {
    check_cpus

    note "server 1/3: ib_send_lat"
    $SRV ib_send_lat -d "$SERVER_DEV" -x "$GID" -s 2 -n 100000 -F >/dev/null
    wait_port_clear

    note "server 2/3: ib_send_bw"
    $SRV ib_send_bw -d "$SERVER_DEV" -x "$GID" -s 65536 -n 100000 -t 64 -F >/dev/null
    wait_port_clear

    note "server 3/3: limen_bench (stays up; Ctrl-C when the client finishes)"
    exec $SRV "$BENCH" -d "$SERVER_DEV" -s 1048576 --pipeline 64 -n 1000000000
}

# ---------------------------------------------------------------- client side

client_phases() {
    check_cpus
    mkdir -p "$OUT"
    tune_up

    printf 'clocksource: %s\n' \
        "$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)" >&2
    lscpu -e=CPU,CORE,SOCKET >&2

    note "client 1/10: perftest latency reference"
    wait_port
    $CLI ib_send_lat -d "$CLIENT_DEV" -x "$GID" -s 2 -n 100000 -F "$PEER" \
        | tee "$OUT/perftest-lat-2b.txt"

    note "client 2/10: perftest bandwidth reference"
    wait_port
    $CLI ib_send_bw -d "$CLIENT_DEV" -x "$GID" -s 65536 -n 100000 -t 64 -F "$PEER" \
        | tee "$OUT/perftest-bw-64k.txt"

    note "client 3/10: conditions"
    $CLI "$BENCH" -d "$CLIENT_DEV" --clock-floor --report-config "$PEER" \
        | tee "$OUT/conditions.txt"

    note "client 4/10: warmup ramp (R2)"
    wait_port
    $CLI "$BENCH" -d "$CLIENT_DEV" --mode latency -s 2 --inline \
        -n 5000 --warmup 0 --runs 1 --json "$OUT/warmup-ramp.json" "$PEER"

    note "client 5/10: latency 2 B (R8)"
    $CLI "$BENCH" -d "$CLIENT_DEV" --mode latency -s 2 --inline \
        -n 100000 --warmup 1000 --runs 9 --json "$OUT/latency-2b.json" "$PEER"

    note "client 6/10: response 140k msg/s (R4a)"
    $CLI "$BENCH" -d "$CLIENT_DEV" --mode response -s 2 --inline \
        --rate 140000 -n 100000 --warmup 1000 --runs 9 \
        --json "$OUT/response-140k.json" "$PEER"

    note "client 7/10: response 400k msg/s (R4b)"
    $CLI "$BENCH" -d "$CLIENT_DEV" --mode response -s 2 --inline \
        --rate 400000 -n 100000 --warmup 1000 --runs 9 \
        --json "$OUT/response-400k.json" "$PEER"

    note "client 8/10: bandwidth 64 KiB (R5)"
    $CLI "$BENCH" -d "$CLIENT_DEV" --mode bandwidth -s 65536 --pipeline 64 \
        -n 100000 --warmup 1000 --runs 9 \
        --json "$OUT/bandwidth-64k.json" "$PEER"

    note "client 9/10: option sweep 64 B (R6a)"
    $CLI "$BENCH" -d "$CLIENT_DEV" --mode sweep -s 64 \
        -n 20000 --warmup 2000 --runs 9 \
        --json "$OUT/sweep-64b.json" "$PEER" | tee "$OUT/sweep-64b.txt"

    note "client 10/10: option sweep 4096 B (R6b)"
    $CLI "$BENCH" -d "$CLIENT_DEV" --mode sweep -s 4096 \
        -n 20000 --warmup 2000 --runs 9 \
        --json "$OUT/sweep-4096b.json" "$PEER" | tee "$OUT/sweep-4096b.txt"

    note "done. ib_send_lat reports half the round trip (rtt_factor=2);"
    note "double t_typical in $OUT/perftest-lat-2b.txt before comparing to latency-2b.json."
}

# ---------------------------------------------------------------- both sides

run_all() {
    check_cpus
    local srv_pid
    TUNE=0 "$0" --server &
    srv_pid=$!
    trap 'kill -- -'"$srv_pid"' 2>/dev/null || true; tune_down' EXIT INT TERM
    client_phases
    note "stopping server"
    pkill -P "$srv_pid" 2>/dev/null || true
    kill "$srv_pid" 2>/dev/null || true
    wait "$srv_pid" 2>/dev/null || true
}

case "${1:-}" in
    --server) server_phases ;;
    --client) client_phases ;;
    --all)    run_all ;;
    *) die "usage: $0 --all | --server | --client
env overrides: SERVER_CPU CLIENT_CPU SERVER_DEV CLIENT_DEV PEER GID PORT BENCH NS OUT TUNE" ;;
esac