#!/usr/bin/env bash
#
# bench-client.sh - client side of the Limen measurement suite.
# Start bench-server.sh first, in another terminal, then run this.
#
# Owns CPU tuning and restores it on exit, including on Ctrl-C.
# perftest reference runs execute first, then limen_bench.

set -euo pipefail

DEV=${DEV:-rocep4s0f0}
PEER=${PEER:-192.168.100.1}
GID=${GID:-3}
PORT=${PORT:-18515}
CPU=${CPU:-2}
PEER_CPU=${PEER_CPU:-1}
BENCH=${BENCH:-./build/limen_bench}
NS=${NS:-limen-b}
OUT=${OUT:-docs/results}
TUNE=${TUNE:-1}

RUN="sudo $NS taskset -c $CPU"

die()  { printf '%s\n' "$*" >&2; exit 1; }
note() { printf '\n=== %s\n' "$*" >&2; }

core_of() {
    local f=/sys/devices/system/cpu/cpu$1/topology/core_id
    [[ -r $f ]] || die "cannot read $f"
    cat "$f"
}

[[ $CPU != "$PEER_CPU" ]] || die "CPU and PEER_CPU are the same CPU"
[[ $(core_of "$CPU") != "$(core_of "$PEER_CPU")" ]] || die \
"cpu$CPU and cpu$PEER_CPU are SMT siblings on physical core $(core_of "$CPU").
Pick two CPUs with different CORE values:
$(lscpu -e=CPU,CORE,SOCKET)
Then: CPU=<a> ./scripts/bench-server.sh   and   CPU=<b> $0"

[[ -x $BENCH ]] || die "$BENCH not found or not executable"

# perftest exchanges setup over a real TCP socket, so ss can see it.
wait_tcp() {
    for _ in $(seq 1 200); do
        ss -ltnH "sport = :$PORT" 2>/dev/null | grep -q . && return 0
        sleep 0.05
    done
    die "no TCP listener on :$PORT after 10s. Is bench-server.sh running?"
}

# rdma_listen creates no socket, so ss cannot see limen_bench. Ask the RDMA stack.
wait_rdma() {
    for _ in $(seq 1 200); do
        rdma resource show cm_id 2>/dev/null | grep -q 'state LISTEN' && return 0
        sleep 0.05
    done
    die "no rdma_cm listener after 10s.
Check the bench-server.sh terminal. If it printed
  serve_forever: PendingConnection::listen:rdma_bind_addr: Cannot assign requested address
then rdma_cm has no usable device in this namespace, and the fabric needs bringing up:
  rdma system show      # must say 'netns exclusive'
  rdma link show
  ip -br addr show      # 192.168.100.1 must be present
  systemctl status limen-fabric.service"
}

tune_down() {
    note "restoring: governor=powersave, C-states enabled"
    sudo cpupower idle-set -E    >/dev/null 2>&1 || true
    sudo cpupower frequency-set -g powersave >/dev/null 2>&1 || true
}

mkdir -p "$OUT"

if [[ $TUNE == 1 ]]; then
    note "tuning: governor=performance, C-states disabled"
    sudo cpupower frequency-set -g performance >/dev/null
    sudo cpupower idle-set -D 0 >/dev/null
    trap tune_down EXIT INT TERM
fi

printf 'clocksource: %s\n' \
    "$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)" >&2
lscpu -e=CPU,CORE,SOCKET >&2

note "client 1/10: perftest latency reference"
wait_tcp
$RUN ib_send_lat -d "$DEV" -x "$GID" -s 2 -n 100000 -F "$PEER" \
    | tee "$OUT/perftest-lat-2b.txt"

note "client 2/10: perftest bandwidth reference"
wait_tcp
$RUN ib_send_bw -d "$DEV" -x "$GID" -s 65536 -n 100000 -t 64 -F "$PEER" \
    | tee "$OUT/perftest-bw-64k.txt"

note "client 3/10: conditions"
$RUN "$BENCH" -d "$DEV" --clock-floor --report-config "$PEER" \
    | tee "$OUT/conditions.txt"

note "client 4/10: warmup ramp (R2)"
wait_rdma
$RUN "$BENCH" -d "$DEV" --mode latency -s 2 --inline \
    -n 5000 --warmup 0 --runs 1 --json "$OUT/warmup-ramp.json" "$PEER"

note "client 5/10: latency 2 B (R8)"
$RUN "$BENCH" -d "$DEV" --mode latency -s 2 --inline \
    -n 100000 --warmup 5000 --runs 9 --json "$OUT/latency-2b.json" "$PEER"

note "client 6/10: response 140k msg/s (R4a)"
$RUN "$BENCH" -d "$DEV" --mode response -s 2 --inline \
    --rate 140000 -n 100000 --warmup 1000 --runs 9 \
    --json "$OUT/response-140k.json" "$PEER"

note "client 7/10: response 400k msg/s (R4b)"
$RUN "$BENCH" -d "$DEV" --mode response -s 2 --inline \
    --rate 400000 -n 100000 --warmup 1000 --runs 9 \
    --json "$OUT/response-400k.json" "$PEER"

note "client 8/10: bandwidth 64 KiB (R5)"
$RUN "$BENCH" -d "$DEV" --mode bandwidth -s 65536 --pipeline 64 \
    -n 100000 --warmup 5000 --runs 9 \
    --json "$OUT/bandwidth-64k.json" "$PEER"

note "client 9/10: option sweep 64 B (R6a)"
$RUN "$BENCH" -d "$DEV" --mode sweep -s 64 \
    -n 100000 --warmup 5000 --runs 9 \
    --json "$OUT/sweep-64b.json" "$PEER" | tee "$OUT/sweep-64b.txt"

note "client 10/10: option sweep 4096 B (R6b)"
$RUN "$BENCH" -d "$DEV" --mode sweep -s 4096 \
    -n 100000 --warmup 5000 --runs 9 \
    --json "$OUT/sweep-4096b.json" "$PEER" | tee "$OUT/sweep-4096b.txt"

note "done. Ctrl-C the bench-server.sh terminal."
note "ib_send_lat reports half the round trip (rtt_factor=2), so double t_typical"
note "in $OUT/perftest-lat-2b.txt before comparing it to latency-2b.json."