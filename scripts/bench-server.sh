#!/usr/bin/env bash
#
# bench-server.sh - server side of the Limen measurement suite.
# Run this first, in its own terminal, then run bench-client.sh in another.
#
# Three phases, in order. Each perftest server exits when its client finishes,
# so the script advances on its own. limen_bench stays up until you Ctrl-C.

set -euo pipefail

DEV=${DEV:-rocep1s0f0}
GID=${GID:-3}
CPU=${CPU:-1}
PEER_CPU=${PEER_CPU:-2}
BENCH=${BENCH:-./build/limen_bench}

RUN="taskset -c $CPU"

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
Then: CPU=<a> $0   and   CPU=<b> ./scripts/bench-client.sh"

[[ -x $BENCH ]] || die "$BENCH not found or not executable"

note "server 1/3: ib_send_lat"
$RUN ib_send_lat -d "$DEV" -x "$GID" -s 2 -n 100000 -F

note "server 2/3: ib_send_bw"
$RUN ib_send_bw -d "$DEV" -x "$GID" -s 65536 -n 100000 -t 64 -F

note "server 3/3: limen_bench, stays up for client steps 4 through 10"
exec $RUN "$BENCH" -d "$DEV" -s 1048576 --pipeline 64 -n 1000000000