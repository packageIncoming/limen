#!/usr/bin/env bash
# trd-05-tests.sh — Limen RDMA Transport Project
# TRD-05: Connection Management
#
# Run from the repository root:
#
#   ./scripts/trd-05-tests.sh                                            compile-time checks
#   ./scripts/trd-05-tests.sh --peer 10.0.0.1 --gid 3                    plus runtime
#   ./scripts/trd-05-tests.sh --peer 10.0.0.1 --gid 3 --ssh you@10.0.0.1 fully automatic
#
# Single-host, two-card topology (peer lives in a network namespace):
#
#   ./scripts/trd-05-tests.sh --dev rocep1s0f0 --peer-dev rocep4s0f0 \
#                             --gid 3 --peer 192.168.100.2 --peer-cmd "sudo limen-b"
#
# Exit 0 only when every test passes and none was skipped.

set -uo pipefail

BIN="./build/limen_pingpong"
DEV=""; GID=""; PEER=""; SSH_PEER=""
PEER_DEV=""; PEER_CMD=""
TCP_BASE=18700
PASS=0; FAIL=0; SKIP=0

R=$'\033[0;31m'; G=$'\033[0;32m'; Y=$'\033[0;33m'; B=$'\033[1m'; N=$'\033[0m'

# CRITICAL: VAR=$((VAR + 1)). A bare ((VAR++)) returns the pre-increment value
# as its exit status, which is 1 when the counter is 0.
ok()  { echo -e "  ${G}PASS${N}  $1"; PASS=$((PASS + 1)); }
no()  { echo -e "  ${R}FAIL${N}  $1"; FAIL=$((FAIL + 1)); }
sk()  { echo -e "  ${Y}SKIP${N}  $1"; SKIP=$((SKIP + 1)); }
hdr() { echo -e "\n${B}$1${N}"; }

TMPDIR="$(mktemp -d)"
PORT_SEQ=0
cleanup() {
  if [[ -n "$PEER_CMD" ]]; then
    $PEER_CMD pkill -f limen_pingpong >/dev/null 2>&1 || true
    sudo pkill -f limen_pingpong      >/dev/null 2>&1 || true
  elif [[ -n "$SSH_PEER" ]]; then
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH_PEER" \
        "pkill -f limen_pingpong" >/dev/null 2>&1 || true
  fi
  rm -rf "$TMPDIR"
}
trap cleanup EXIT INT TERM
next_port() { PORT_SEQ=$((PORT_SEQ + 1)); echo $((TCP_BASE + PORT_SEQ)); }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dev) DEV="${2:-}"; shift 2 ;;
    --gid) GID="${2:-}"; shift 2 ;;
    --peer) PEER="${2:-}"; shift 2 ;;
    --ssh) SSH_PEER="${2:-}"; shift 2 ;;
    --peer-dev) PEER_DEV="${2:-}"; shift 2 ;;
    --peer-cmd) PEER_CMD="${2:-}"; shift 2 ;;
    -h|--help)
      echo "usage: $0 [--dev NAME] [--gid N] [--peer IP] [--ssh user@host]"
      echo "          [--peer-dev NAME] [--peer-cmd 'sudo limen-b']"
      exit 1 ;;
    *) echo "unknown argument: $1"; exit 1 ;;
  esac
done

CXX="${CXX:-c++}"; STD="-std=c++20"; INC="-Iinclude"

echo -e "${B}=== Limen TRD-05: Connection Management ===${N}"

# ── Peer control ──────────────────────────────────────────────────────
# Three modes, in priority order:
#   --peer-cmd  the peer is on this host behind a wrapper (namespace, container)
#   --ssh       the peer is a separate machine
#   neither     prompt the operator to start it by hand
#
# $3, when given, is a file the peer's stdout and stderr are captured into,
# which is how R6 inspects the server side.
start_peer() {
  local port="$1" extra="${2:-}" logfile="${3:-/dev/null}"
  if [[ -n "$PEER_CMD" ]]; then
    $PEER_CMD "$BIN" -d "$PEER_DEV" -g "$GID" -t "$port" $extra >"$logfile" 2>&1 &
    sleep 2
  elif [[ -n "$SSH_PEER" ]]; then
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH_PEER" \
      "cd '$(pwd)' && nohup $BIN -d $PEER_DEV -g $GID -t $port $extra >/tmp/limen-srv.log 2>&1 & disown" \
      >/dev/null 2>&1 || true
    sleep 2
  else
    echo "      on the peer:  $BIN -d $PEER_DEV -g $GID -t $port $extra" >&2
    read -rp "      press enter once it is running... " >&2
  fi
}

kill_peer() {
  local port="$1"
  if [[ -n "$PEER_CMD" ]]; then
    $PEER_CMD pkill -f "limen_pingpong.*-t $port" >/dev/null 2>&1 || true
    sudo pkill -f "limen_pingpong.*-t $port"      >/dev/null 2>&1 || true
  elif [[ -n "$SSH_PEER" ]]; then
    ssh -o BatchMode=yes "$SSH_PEER" \
        "pkill -f 'limen_pingpong.*-t $port'" >/dev/null 2>&1 || true
  fi
}

# Echo whatever the peer wrote, whichever transport started it.
peer_log() {
  local logfile="${1:-/dev/null}"
  if [[ -n "$PEER_CMD" ]]; then
    cat "$logfile" 2>/dev/null || true
  elif [[ -n "$SSH_PEER" ]]; then
    ssh -o BatchMode=yes "$SSH_PEER" "cat /tmp/limen-srv.log" 2>/dev/null || true
  fi
}

# run_pair "<server args>" "<client args>" <timeout>
run_pair() {
  local sargs="$1" cargs="$2" tmo="${3:-60}" port
  port=$(next_port)
  start_peer "$port" "$sargs"
  timeout "$tmo" "$BIN" -d "$DEV" -g "$GID" -t "$port" $cargs "$PEER" 2>&1
  local rc=$?
  kill_peer "$port"
  echo "__EXIT__${rc}"
}
exit_of() { grep -oE '__EXIT__[0-9]+' <<<"$1" | grep -oE '[0-9]+$'; }

# ══ Build ═════════════════════════════════════════════════════════════
BUILD_OK=0
test_builds() {                                                       # R1
  hdr "R1: everything builds clean under -Wall -Wextra"
  local log="$TMPDIR/build.log"
  if ! cmake -S . -B build >"$log" 2>&1;  then no "cmake configure failed — see $log"; return; fi
  if ! cmake --build build >>"$log" 2>&1; then no "compilation failed — see $log";     return; fi
  local warns; warns=$(grep -ci 'warning:' "$log" || true)
  if [[ "$warns" -gt 0 ]]; then no "emitted $warns warning(s) — see $log"; return; fi
  local missing="" p
  for p in limen_devinfo limen_connect limen_pingpong; do
    [[ -x "./build/$p" ]] || missing="$missing $p"
  done
  [[ -n "$missing" ]] && { no "missing binaries:$missing"; return; }
  BUILD_OK=1; ok "all three programs built"
}

# ══ Compile-time ══════════════════════════════════════════════════════
test_cm_static_contract() {                                           # R1
  hdr "R1: the three CM wrappers satisfy the ownership contract"
  [[ -f tests/cm_static_contract.cpp ]] || { no "tests/cm_static_contract.cpp missing"; return; }
  if $CXX $STD $INC -fsyntax-only tests/cm_static_contract.cpp >"$TMPDIR/cm.log" 2>&1; then
    ok "EventChannel, ConnectionId, and Event all conform"
  else
    no "assertions failed:"; grep -m3 'error:\|static_assert' "$TMPDIR/cm.log" | sed 's/^/        /' || true
  fi
}

test_adopt_exists() {                                                 # R2
  hdr "R2: ConnectionId::adopt exists and is noexcept"
  local src="$TMPDIR/adopt.cpp"
  cat > "$src" <<'EOF'
#include <limen/cm.hpp>
#include <type_traits>
static_assert(std::is_same_v<decltype(limen::ConnectionId::adopt(nullptr)),
                             limen::ConnectionId>, "adopt must return by value");
static_assert(noexcept(limen::ConnectionId::adopt(nullptr)), "adopt must be noexcept");
int main() {}
EOF
  if $CXX $STD $INC -fsyntax-only "$src" >/dev/null 2>&1; then
    ok "adoption interface present and noexcept"
  else
    no "adopt() missing, not static, or not noexcept"
  fi
}

test_no_raw_private_data() {                                          # R4
  hdr "R4: Event exposes no raw private-data pointer"
  [[ -f tests/no_raw_private_data.cpp ]] || { no "tests/no_raw_private_data.cpp missing"; return; }
  if $CXX $STD $INC -fsyntax-only tests/no_raw_private_data.cpp >/dev/null 2>&1; then
    no "it COMPILED — the raw pointer is reachable, so a use-after-ack is expressible"
  else
    ok "rejected by the compiler, as required"
  fi
}

test_conninfo_fits() {                                                # R7
  hdr "R7: sizeof(ConnInfo) <= 56 is asserted at compile time"
  if ! grep -rqE 'static_assert.*sizeof\(ConnInfo\).*56' include/ src/ 2>/dev/null; then
    no "no static_assert bounding ConnInfo against the 56-byte connect limit"
    return
  fi
  local src="$TMPDIR/size.cpp"
  cat > "$src" <<'EOF'
#include <limen/cm.hpp>
static_assert(sizeof(limen::ConnInfo) <= 56, "exceeds rdma_connect private data limit");
int main() {}
EOF
  $CXX $STD $INC -fsyntax-only "$src" >/dev/null 2>&1 \
    && ok "assertion present and holds" || no "ConnInfo exceeds 56 bytes"
}

# ══ Structural ════════════════════════════════════════════════════════
test_side_channel_removed() {                                         # R10
  hdr "R10: no socket call or ibv_modify_qp survives in the ported program"
  local f problems=""
  f=$(ls src/limen_pingpong.c* 2>/dev/null | head -1)
  [[ -n "$f" ]] || { no "cannot locate limen_pingpong source"; return; }
  grep -qE '\b(socket|bind|listen|accept|connect)[[:space:]]*\(' "$f" \
    && ! grep -qE 'rdma_(bind_addr|listen|accept|connect)' <<<"$(grep -E '\b(socket|bind|listen|accept|connect)[[:space:]]*\(' "$f")" \
    && problems="$problems raw-sockets"
  if grep -E '\b(socket|bind|listen|accept|connect)[[:space:]]*\(' "$f" | grep -qv 'rdma_'; then
    problems="$problems socket-calls"
  fi
  grep -q 'ibv_modify_qp' "$f" && problems="$problems ibv_modify_qp"
  if [[ -z "$problems" ]]; then
    ok "side channel and manual state machine fully removed"
  else
    no "surviving:$problems in $f"
  fi
}

# ══ Runtime ═══════════════════════════════════════════════════════════
BASE=""
capture_base() { [[ -n "$BASE" ]] && return 0; BASE=$(run_pair "-n 50" "-n 50" 90); }

test_client_event_sequence() {                                        # R5
  hdr "R5: client reports ADDR_RESOLVED, ROUTE_RESOLVED, ESTABLISHED in order"
  capture_base
  local seq
  seq=$(grep -oE '^cm: event [A-Z_]+' <<<"$BASE" | sed 's/cm: event //' | tr '\n' ' ')
  if grep -qE 'ADDR_RESOLVED .*ROUTE_RESOLVED .*ESTABLISHED' <<<"$seq"; then
    ok "sequence: ${seq}"
  else
    no "unexpected sequence: ${seq:-<none>}"
  fi
}

test_server_adopts_new_id() {                                         # R6
  hdr "R6: server reports CONNECT_REQUEST and adopts the new id"
  # The client's own output cannot show this; the server's must be captured.
  if [[ -z "$SSH_PEER" && -z "$PEER_CMD" ]]; then
    sk "requires --ssh or --peer-cmd to capture the server's output"
    return
  fi
  local port; port=$(next_port)
  local srvlog="$TMPDIR/srv-${port}.log"
  start_peer "$port" "-n 5" "$srvlog"
  timeout 60 "$BIN" -d "$DEV" -g "$GID" -t "$port" -n 5 "$PEER" >/dev/null 2>&1 || true
  sleep 1
  local srv; srv=$(peer_log "$srvlog")
  kill_peer "$port"
  if grep -qE 'CONNECT_REQUEST.*adopted' <<<"$srv"; then
    ok "server adopted the delivered id"
  else
    no "no adoption line — got: $(grep -m1 'CONNECT_REQUEST' <<<"$srv" || echo '<none>')"
  fi
}

test_payload_exchanged() {                                            # R7
  hdr "R7: the peer: line carries a non-zero address, rkey, and length"
  capture_base
  local line; line=$(grep -m1 '^peer: ' <<<"$BASE" || true)
  [[ -n "$line" ]] || { no "no peer: line"; return; }
  local a k l problems=""
  a=$(grep -oE 'addr=0x[0-9a-fA-F]+'   <<<"$line" | cut -d= -f2)
  k=$(grep -oE 'rkey=0x[0-9a-fA-F]+'   <<<"$line" | cut -d= -f2)
  l=$(grep -oE 'length=[0-9]+'         <<<"$line" | cut -d= -f2)
  [[ -n "$a" && "$a" != "0x0" && "$a" != "0x0000000000000000" ]] || problems="$problems addr"
  [[ -n "$k" && "$k" != "0x00000000" ]] || problems="$problems rkey"
  [[ -n "$l" && "$l" -gt 0 ]] || problems="$problems length"
  [[ -z "$problems" ]] && ok "$line" || no "zero or missing:$problems — in: $line"
}

test_no_teardown_hang() {                                             # R3
  hdr "R3: teardown completes — no unacknowledged event blocking destroy"
  local out rc
  out=$(run_pair "-n 10" "-n 10" 45)
  rc=$(exit_of "$out")
  if [[ "$rc" -eq 124 ]]; then
    no "the program hung — an event was not acknowledged (see Hints H3)"
  elif [[ "$rc" -ne 0 ]]; then
    no "exited $rc rather than completing"
  elif grep -qE '^teardown: ' <<<"$out"; then
    ok "ran to completion and printed teardown"
  else
    no "exited 0 but never reached teardown"
  fi
}

test_clean_disconnect() {                                             # R9
  hdr "R9: both sides report DISCONNECTED and the client exits 0"
  capture_base
  local rc; rc=$(exit_of "$BASE")
  if grep -qE '^cm: event DISCONNECTED' <<<"$BASE" && [[ "$rc" -eq 0 ]]; then
    ok "DISCONNECTED reported, exit 0"
  else
    no "$(grep -m1 '^cm: event DISCONNECTED' <<<"$BASE" || echo 'no DISCONNECTED event') (exit $rc)"
  fi
}

test_unreachable_times_out() {                                        # R5
  hdr "R5: an unreachable peer times out and reports, rather than hanging"
  local rc
  timeout 45 "$BIN" -d "$DEV" -g "$GID" -t "$(next_port)" 203.0.113.1 >/dev/null 2>&1; rc=$?
  if [[ "$rc" -eq 124 ]]; then
    no "hung — rdma_get_cm_event blocks by default and needs a timeout above it"
  elif [[ "$rc" -eq 7 || "$rc" -eq 4 ]]; then
    ok "reported and exited $rc"
  else
    no "expected exit 7 (unexpected/absent event), got $rc"
  fi
}

test_rnr_retry_plumbed() {                                            # R8
  hdr "R8: --rnr-retry still reaches the connection parameters"
  local out; out=$(run_pair "-n 5" "--rnr-retry 3 -n 5" 60)
  if grep -qE 'rnr_retry=3' <<<"$out"; then
    ok "value reported as 3"
  else
    no "$(grep -m1 '^pingpong:' <<<"$out" || echo 'no pingpong: line')"
  fi
}

test_teardown_reports_cm() {                                          # R11
  hdr "R11: teardown reports the id and channel stages"
  capture_base
  local line; line=$(grep -m1 '^teardown: ' <<<"$BASE" || true)
  [[ -n "$line" ]] || { no "no teardown: line"; return; }
  local problems="" f
  for f in id channel; do grep -qE "${f}=ok" <<<"$line" || problems="$problems ${f}"; done
  [[ -z "$problems" ]] && ok "$line" || no "missing or not ok:$problems — in: $line"
}

# ══ Regressions ═══════════════════════════════════════════════════════
run_regression() {
  local n="$1"; shift
  local s="scripts/trd-0${n}-tests.sh"
  [[ -x "$s" ]] || { echo "MISSING"; return; }
  if "$s" "$@" >"$TMPDIR/reg-${n}.log" 2>&1; then echo "PASS"; else echo "FAIL"; fi
}

test_trd02_regression() {                                             # R11
  hdr "R11: the TRD-02 suite passes against the untouched limen_connect"
  # An array, not ${VAR:+...} expansion: --peer-cmd's value contains a space
  # ("sudo limen-b") and word-splitting would shred it into separate arguments.
  local args=(--dev "$DEV" --gid "$GID" --peer "$PEER")
  [[ -n "$SSH_PEER" ]] && args+=(--ssh      "$SSH_PEER")
  [[ -n "$PEER_DEV" ]] && args+=(--peer-dev "$PEER_DEV")
  [[ -n "$PEER_CMD" ]] && args+=(--peer-cmd "$PEER_CMD")
  local r; r=$(run_regression 2 "${args[@]}")
  case "$r" in
    PASS)    ok "manual bringup still green — the reference is intact" ;;
    MISSING) sk "scripts/trd-02-tests.sh not present" ;;
    *)       no "TRD-02 suite failed — limen_connect should not have changed"
             grep -m3 'FAIL' "$TMPDIR/reg-2.log" | sed 's/^/        /' || true ;;
  esac
}

test_trd03_regression() {                                             # R11
  hdr "R11: the TRD-03 suite passes against the ported limen_pingpong"
  # An array, not ${VAR:+...} expansion: --peer-cmd's value contains a space
  # ("sudo limen-b") and word-splitting would shred it into separate arguments.
  local args=(--dev "$DEV" --gid "$GID" --peer "$PEER")
  [[ -n "$SSH_PEER" ]] && args+=(--ssh      "$SSH_PEER")
  [[ -n "$PEER_DEV" ]] && args+=(--peer-dev "$PEER_DEV")
  [[ -n "$PEER_CMD" ]] && args+=(--peer-cmd "$PEER_CMD")
  local r; r=$(run_regression 3 "${args[@]}")
  case "$r" in
    PASS)    ok "transfer behaviour unchanged by the port" ;;
    MISSING) sk "scripts/trd-03-tests.sh not present" ;;
    *)       no "TRD-03 suite failed — see $TMPDIR/reg-3.log"
             grep -m3 'FAIL' "$TMPDIR/reg-3.log" | sed 's/^/        /' || true ;;
  esac
}

test_no_leaks() {                                                     # R11
  hdr "R11: valgrind reports zero bytes definitely lost on three paths"
  if ! command -v valgrind >/dev/null 2>&1; then sk "valgrind not installed"; return; fi
  local worst=0 lost log port

  # Path 1: no peer at all — needs no server
  log="$TMPDIR/vg-nopeer.log"
  timeout 90 valgrind --leak-check=full --show-leak-kinds=definite \
    "$BIN" -d "$DEV" -g "$GID" -t "$(next_port)" 203.0.113.1 >/dev/null 2>"$log" || true
  lost=$(grep -oE 'definitely lost: [0-9,]+ bytes' "$log" | grep -oE '[0-9,]+' | tr -d ',' || echo "")
  [[ -z "$lost" ]] && { no "could not parse valgrind output — see $log"; return; }
  [[ "$lost" -gt "$worst" ]] && worst="$lost"

  # Paths 2 and 3: with a peer
  for spec in "success::-n 20" "rnr:--no-recv --rnr-retry 1:--rnr-retry 1 -n 5"; do
    local name="${spec%%:*}" rest="${spec#*:}"
    local sargs="${rest%%:*}" cargs="${rest#*:}"
    port=$(next_port)
    start_peer "$port" "$sargs"
    log="$TMPDIR/vg-${name}.log"
    timeout 150 valgrind --leak-check=full --show-leak-kinds=definite \
      "$BIN" -d "$DEV" -g "$GID" -t "$port" $cargs "$PEER" >/dev/null 2>"$log" || true
    lost=$(grep -oE 'definitely lost: [0-9,]+ bytes' "$log" | grep -oE '[0-9,]+' | tr -d ',' || echo "")
    [[ -z "$lost" ]] && { no "could not parse valgrind output for ${name} — see $log"; return; }
    [[ "$lost" -gt "$worst" ]] && worst="$lost"
  done

  [[ "$worst" -eq 0 ]] && ok "0 bytes definitely lost on all three paths" \
                       || no "${worst} bytes definitely lost — see $TMPDIR/vg-*.log"
}

# ══ Run ═══════════════════════════════════════════════════════════════
test_builds
if [[ "$BUILD_OK" -ne 1 ]]; then
  echo -e "\n${R}Gate closed.${N} Nothing else can run until the build succeeds."
  exit 1
fi

test_cm_static_contract
test_adopt_exists
test_no_raw_private_data
test_conninfo_fits
test_side_channel_removed

if [[ -z "$DEV" ]] && command -v ibv_devices >/dev/null 2>&1; then
  DEV=$(ibv_devices 2>/dev/null | awk 'NR>2 && NF {print $1; exit}' || true)
fi

# The peer defaults to the same device name; on a single host with two cards
# they differ, so --peer-dev overrides it.
[[ -z "$PEER_DEV" ]] && PEER_DEV="$DEV"

# Cache sudo credentials up front so backgrounded peer launches never block
# on a password prompt in the middle of the suite.
if [[ "$PEER_CMD" == sudo* ]]; then
  sudo -v || { echo "sudo credentials required for --peer-cmd"; exit 1; }
fi

if [[ -z "$PEER" || -z "$GID" || -z "$DEV" ]]; then
  hdr "R3–R11: runtime checks"
  sk "requires --peer, --gid, and a device"
  sk "  (test_client_event_sequence)"
  sk "  (test_server_adopts_new_id)"
  sk "  (test_payload_exchanged)"
  sk "  (test_no_teardown_hang)"
  sk "  (test_clean_disconnect)"
  sk "  (test_unreachable_times_out)"
  sk "  (test_rnr_retry_plumbed)"
  sk "  (test_teardown_reports_cm)"
  sk "  (test_trd02_regression)"
  sk "  (test_trd03_regression)"
  sk "  (test_no_leaks)"
else
  echo -e "\n${B}Device ${DEV}, GID index ${GID}, peer ${PEER}${N}"
  test_client_event_sequence
  test_server_adopts_new_id
  test_payload_exchanged
  test_no_teardown_hang
  test_clean_disconnect
  test_unreachable_times_out
  test_rnr_retry_plumbed
  test_teardown_reports_cm
  test_trd02_regression
  test_trd03_regression
  test_no_leaks
fi

# ══ Summary ═══════════════════════════════════════════════════════════
echo ""
echo -e "${B}=========================================${N}"
echo -e "  ${G}${PASS} passed${N} / ${R}${FAIL} failed${N} / ${Y}${SKIP} skipped${N}"
echo -e "${B}=========================================${N}"

if [[ "$FAIL" -ne 0 ]]; then
  echo -e "\n${R}Gate closed.${N} Consult the Hints section named beside the first failure."
  exit 1
fi
if [[ "$SKIP" -ne 0 ]]; then
  echo -e "\n${Y}Gate incomplete.${N} Everything that ran passed, but skipped checks must run."
  exit 1
fi
echo -e "\n${G}Gate open.${N} Record the event traces and the RNR-timer note in docs/cm-notes.md,"
echo -e "answer the comprehension questions, then begin TRD-06."