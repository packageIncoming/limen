#!/usr/bin/env bash
# trd-02-tests.sh — Limen RDMA Transport Project
# TRD-02: Queue Pairs and the State Machine
#
# Run from the repository root:
#
#   ./scripts/trd-02-tests.sh                                        build + argument checks
#   ./scripts/trd-02-tests.sh --peer 10.0.0.1 --gid 3                two-node, manual server start
#   ./scripts/trd-02-tests.sh --peer 10.0.0.1 --gid 3 --ssh you@10.0.0.1   fully automatic
#
# Single-host, two-card topology (peer lives in a network namespace):
#
#   ./scripts/trd-02-tests.sh --dev rocep1s0f0 --peer-dev rocep4s0f0 \
#                             --gid 3 --peer 192.168.100.2 --peer-cmd "sudo limen-b"
#
# Exit 0 only when every test passes and none was skipped.

set -uo pipefail

BIN="./build/limen_connect"
DEV=""
GID=""
PEER=""
SSH_PEER=""
PEER_DEV=""
PEER_CMD=""
TCP_PORT=18515
PASS=0; FAIL=0; SKIP=0

R=$'\033[0;31m'; G=$'\033[0;32m'; Y=$'\033[0;33m'; B=$'\033[1m'; N=$'\033[0m'

# CRITICAL: VAR=$((VAR + 1)). A bare ((VAR++)) returns the pre-increment value
# as its exit status, which is 1 when the counter is 0.
ok()  { echo -e "  ${G}PASS${N}  $1"; PASS=$((PASS + 1)); }
no()  { echo -e "  ${R}FAIL${N}  $1"; FAIL=$((FAIL + 1)); }
sk()  { echo -e "  ${Y}SKIP${N}  $1"; SKIP=$((SKIP + 1)); }
hdr() { echo -e "\n${B}$1${N}"; }

TMPDIR="$(mktemp -d)"
SERVER_STARTED=""
cleanup() {
  if [[ -n "$SERVER_STARTED" ]]; then
    if [[ -n "$PEER_CMD" ]]; then
      $PEER_CMD pkill -f limen_connect >/dev/null 2>&1 || true
      sudo pkill -f limen_connect     >/dev/null 2>&1 || true
    elif [[ -n "$SSH_PEER" ]]; then
      ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH_PEER" \
          "pkill -f limen_connect" >/dev/null 2>&1 || true
    fi
  fi
  rm -rf "$TMPDIR"
}
trap cleanup EXIT INT TERM

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dev)  DEV="${2:-}"; shift 2 ;;
    --gid)  GID="${2:-}"; shift 2 ;;
    --peer) PEER="${2:-}"; shift 2 ;;
    --ssh)  SSH_PEER="${2:-}"; shift 2 ;;
    --peer-dev) PEER_DEV="${2:-}"; shift 2 ;;
    --peer-cmd) PEER_CMD="${2:-}"; shift 2 ;;
    --tcp)  TCP_PORT="${2:-}"; shift 2 ;;
    -h|--help)
      echo "usage: $0 [--dev NAME] [--gid N] [--peer IP] [--ssh user@host]"
      echo "          [--peer-dev NAME] [--peer-cmd 'sudo limen-b'] [--tcp PORT]"
      exit 1 ;;
    *) echo "unknown argument: $1"; exit 1 ;;
  esac
done

echo -e "${B}=== Limen TRD-02: Queue Pairs and the State Machine ===${N}"

# ── Build ─────────────────────────────────────────────────────────────
BUILD_OK=0
test_builds() {                                                     # R1
  hdr "R1: limen_connect builds clean under -Wall -Wextra"
  local log="$TMPDIR/build.log"
  if ! cmake -S . -B build >"$log" 2>&1; then
    no "cmake configure failed — see $log"; return
  fi
  if ! cmake --build build >>"$log" 2>&1; then
    no "compilation failed — see $log"; return
  fi
  if [[ ! -x "$BIN" ]]; then
    no "built, but $BIN is missing or not executable"; return
  fi
  local warns
  warns=$(grep -ci 'warning:' "$log" || true)
  if [[ "$warns" -gt 0 ]]; then
    no "built, but emitted $warns warning(s) — see $log"; return
  fi
  BUILD_OK=1
  ok "built with no warnings"
}

# ── Argument handling (no fabric needed) ──────────────────────────────
test_missing_gid_exits_one() {                                      # R9
  hdr "R9: a missing -g exits 1 rather than defaulting"
  local rc
  "$BIN" -d "${DEV:-rxe0}" >/dev/null 2>&1; rc=$?
  if [[ "$rc" -eq 1 ]]; then
    ok "exit 1"
  else
    no "expected exit 1, got $rc — -g must be required, not defaulted"
  fi
}

test_bad_gid_index_exits_one() {                                    # R9
  hdr "R9: a GID index above gid_tbl_len exits 1"
  local rc
  "$BIN" -d "$DEV" -g 9999 >/dev/null 2>&1; rc=$?
  if [[ "$rc" -eq 1 ]]; then
    ok "exit 1"
  else
    no "expected exit 1, got $rc"
  fi
}

test_zero_gid_rejected() {                                          # R3
  hdr "R3: an unpopulated GID index is rejected with exit 3"
  # find an index inside the table whose GID reads all zeroes
  local tbl idx="" i g
  tbl=$(ibv_devinfo -d "$DEV" 2>/dev/null | grep -oE 'gid_tbl_len:[[:space:]]*[0-9]+' \
        | grep -oE '[0-9]+$' || echo 0)
  for ((i = 0; i < tbl; i++)); do
    g=$(cat "/sys/class/infiniband/${DEV}/ports/1/gids/${i}" 2>/dev/null || echo "")
    if [[ "$g" == "0000:0000:0000:0000:0000:0000:0000:0000" ]]; then idx="$i"; break; fi
  done
  if [[ -z "$idx" ]]; then
    sk "every entry in this GID table is populated — cannot exercise the check"
    return
  fi
  local rc
  "$BIN" -d "$DEV" -g "$idx" >/dev/null 2>&1; rc=$?
  if [[ "$rc" -eq 3 ]]; then
    ok "empty index $idx rejected with exit 3"
  else
    no "expected exit 3 for empty gid index $idx, got $rc"
  fi
}

test_side_channel_failure_exits_four() {                            # R4
  hdr "R4: an unreachable peer exits 4, distinctly from a verbs failure"
  local rc
  timeout 20 "$BIN" -d "$DEV" -g "$GID" -t "$TCP_PORT" 203.0.113.1 >/dev/null 2>&1; rc=$?
  if [[ "$rc" -eq 4 ]]; then
    ok "exit 4"
  elif [[ "$rc" -eq 124 ]]; then
    no "hung rather than failing — add a connect timeout to the side channel"
  else
    no "expected exit 4, got $rc — verbs and side-channel failures must be distinguishable"
  fi
}

# ── Peer control ──────────────────────────────────────────────────────
# Three modes, in priority order:
#   --peer-cmd  the peer is on this host behind a wrapper (namespace, container)
#   --ssh       the peer is a separate machine
#   neither     prompt the operator to start it by hand
start_peer() {
  local port="$1" extra="${2:-}"
  if [[ -n "$PEER_CMD" ]]; then
    $PEER_CMD "$BIN" -d "$PEER_DEV" -g "$GID" -t "$port" $extra >/dev/null 2>&1 &
    SERVER_STARTED=1
    sleep 2
  elif [[ -n "$SSH_PEER" ]]; then
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH_PEER" \
      "cd '$(pwd)' && nohup $BIN -d $PEER_DEV -g $GID -t $port $extra >/dev/null 2>&1 & disown" \
      >/dev/null 2>&1 || true
    SERVER_STARTED=1
    sleep 2
  else
    echo "      start on the peer:  $BIN -d $PEER_DEV -g $GID -t $port $extra"
    read -rp "      press enter once it is listening... "
  fi
}

# ── Two-node run ──────────────────────────────────────────────────────
RUN=""
RUN_RC=""
do_connect_run() {
  [[ -n "$RUN" ]] && return 0
  start_peer "$TCP_PORT"
  RUN=$(timeout 40 "$BIN" -d "$DEV" -g "$GID" -t "$TCP_PORT" "$PEER" 2>&1)
  RUN_RC=$?
}

test_cq_reports_capacity() {                                        # R1
  hdr "R1: cq line reports granted and requested capacity"
  do_connect_run
  if grep -qE '^cq: cqe=[0-9]+ \(requested [0-9]+\)' <<<"$RUN"; then
    ok "$(grep -m1 '^cq:' <<<"$RUN")"
  else
    no "no conforming cq: line — got: $(grep -m1 '^cq' <<<"$RUN" || echo '<none>')"
  fi
}

test_qp_reports_caps() {                                            # R2
  hdr "R2: qp line reports RC and all four granted capabilities"
  do_connect_run
  local line problems=""
  line=$(grep -m1 '^qp: ' <<<"$RUN" || true)
  [[ -n "$line" ]] || { no "no qp: line"; return; }
  grep -q 'type=RC'                  <<<"$line" || problems="$problems type"
  grep -qE 'max_send_wr=[0-9]+'      <<<"$line" || problems="$problems max_send_wr"
  grep -qE 'max_recv_wr=[0-9]+'      <<<"$line" || problems="$problems max_recv_wr"
  grep -qE 'max_send_sge=[0-9]+'     <<<"$line" || problems="$problems max_send_sge"
  grep -qE 'max_recv_sge=[0-9]+'     <<<"$line" || problems="$problems max_recv_sge"
  if [[ -z "$problems" ]]; then ok "$line"; else no "qp line missing:$problems"; fi
}

test_local_identity() {                                             # R3
  hdr "R3: local line carries qpn, psn, gid, and lid"
  do_connect_run
  local line problems=""
  line=$(grep -m1 '^local:' <<<"$RUN" || true)
  [[ -n "$line" ]] || { no "no local: line"; return; }
  grep -qE 'qpn=0x[0-9a-fA-F]+'                       <<<"$line" || problems="$problems qpn"
  grep -qE 'psn=0x[0-9a-fA-F]+'                       <<<"$line" || problems="$problems psn"
  grep -qE 'gid=([0-9a-fA-F]{4}:){7}[0-9a-fA-F]{4}'   <<<"$line" || problems="$problems gid"
  grep -qE 'lid=0x[0-9a-fA-F]+'                       <<<"$line" || problems="$problems lid"
  if [[ -z "$problems" ]]; then ok "local identity complete"; else no "missing:$problems"; fi
}

test_exchange_completed() {                                         # R4
  hdr "R4: remote line appears and its qpn differs from the local one"
  do_connect_run
  local l r
  l=$(grep -m1 '^local:'  <<<"$RUN" | grep -oE 'qpn=0x[0-9a-fA-F]+' || true)
  r=$(grep -m1 '^remote:' <<<"$RUN" | grep -oE 'qpn=0x[0-9a-fA-F]+' || true)
  if [[ -z "$r" ]]; then
    no "no remote: line — the side-channel exchange did not complete"
  elif [[ "$l" == "$r" ]]; then
    no "remote qpn equals local qpn ($l) — the program is reading back its own values"
  else
    ok "exchanged (local $l, remote $r)"
  fi
}

test_init_transition() {                                            # R5
  hdr "R5: RESET -> INIT"
  do_connect_run
  grep -qE '^state: RESET -> INIT ok' <<<"$RUN" \
    && ok "transition reported ok" \
    || no "$(grep -m1 '^state: RESET' <<<"$RUN" || echo 'no RESET -> INIT line')"
}

test_rtr_transition() {                                             # R6
  hdr "R6: INIT -> RTR"
  do_connect_run
  if grep -qE '^state: INIT -> RTR ok' <<<"$RUN"; then
    ok "transition reported ok"
  else
    no "$(grep -m1 '^state: INIT -> RTR' <<<"$RUN" || echo 'no INIT -> RTR line') — check is_global and the GID index first (Hints H6)"
  fi
}

test_rts_transition() {                                             # R7
  hdr "R7: RTR -> RTS"
  do_connect_run
  grep -qE '^state: RTR -> RTS ok' <<<"$RUN" \
    && ok "transition reported ok" \
    || no "$(grep -m1 '^state: RTR' <<<"$RUN" || echo 'no RTR -> RTS line')"
}

test_verify_rts() {                                                 # R8
  hdr "R8: queried state is RTS and the process exits 0"
  do_connect_run
  if ! grep -qE '^verify: qp_state=RTS' <<<"$RUN"; then
    no "$(grep -m1 '^verify:' <<<"$RUN" || echo 'no verify: line')"
    return
  fi
  if [[ "$RUN_RC" -ne 0 ]]; then
    no "reported RTS but exited $RUN_RC"
  else
    ok "qp_state=RTS, exit 0"
  fi
}

test_failure_diagnostic() {                                         # R10
  hdr "R10: a forced transition failure prints mask and attribute values"
  local port=$((TCP_PORT + 1))
  start_peer "$port"
  local out rc
  out=$(timeout 30 "$BIN" -d "$DEV" -g "$GID" -t "$port" \
        --force-rtr-fail "$PEER" 2>&1); rc=$?
  if [[ "$rc" -eq 0 ]]; then
    sk "--force-rtr-fail not implemented, or it did not fail (see Hints H10)"
    return
  fi
  local problems=""
  grep -qE 'INIT -> RTR FAILED: E[A-Z]+' <<<"$out" || problems="$problems symbolic-errno"
  grep -qE 'attr_mask:.*IBV_QP_AV'       <<<"$out" || problems="$problems attr_mask"
  grep -qE 'ah_attr:.*is_global='        <<<"$out" || problems="$problems ah_attr"
  grep -qE 'dgid:'                       <<<"$out" || problems="$problems dgid"
  grep -qiE 'hint:'                      <<<"$out" || problems="$problems hint"
  if [[ -z "$problems" ]]; then
    ok "diagnostic complete"
  else
    no "diagnostic missing:$problems"
  fi
}

test_teardown_reports_all() {                                       # R11
  hdr "R11: teardown line reports all five stages"
  do_connect_run
  local line
  line=$(grep -m1 '^teardown: ' <<<"$RUN" || true)
  [[ -n "$line" ]] || { no "no teardown: line"; return; }
  local problems="" f
  for f in qp cq mr pd context; do
    grep -qE "${f}=ok" <<<"$line" || problems="$problems ${f}"
  done
  if [[ -z "$problems" ]]; then
    ok "qp cq mr pd context all ok"
  else
    no "not ok (or absent):$problems — in: $line"
  fi
}

test_no_leaks() {                                                   # R11
  hdr "R11: valgrind reports zero bytes definitely lost"
  if ! command -v valgrind >/dev/null 2>&1; then
    sk "valgrind not installed"; return
  fi
  start_peer "$((TCP_PORT + 2))"
  local log="$TMPDIR/valgrind.log"
  timeout 90 valgrind --leak-check=full --show-leak-kinds=definite \
    "$BIN" -d "$DEV" -g "$GID" -t "$((TCP_PORT + 2))" "$PEER" >/dev/null 2>"$log" || true
  local lost
  lost=$(grep -oE 'definitely lost: [0-9,]+ bytes' "$log" \
         | grep -oE '[0-9,]+' | tr -d ',' || echo "")
  if [[ -z "$lost" ]]; then
    no "could not parse valgrind output — see $log"
  elif [[ "$lost" -eq 0 ]]; then
    ok "0 bytes definitely lost"
  else
    no "${lost} bytes definitely lost — see $log"
  fi
}

# ── Run ───────────────────────────────────────────────────────────────
test_builds
if [[ "$BUILD_OK" -ne 1 ]]; then
  echo -e "\n${R}Gate closed.${N} Nothing else can run until the build succeeds."
  exit 1
fi

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

test_missing_gid_exits_one

if [[ -z "$DEV" || -z "$GID" ]]; then
  hdr "R3, R9: fabric-dependent argument checks"
  sk "no device and/or --gid given"
  sk "  (test_bad_gid_index_exits_one)"
  sk "  (test_zero_gid_rejected)"
else
  test_bad_gid_index_exits_one
  test_zero_gid_rejected
fi

if [[ -z "$PEER" || -z "$GID" || -z "$DEV" ]]; then
  hdr "R1–R11: two-node checks"
  sk "requires --peer, --gid, and a device"
  sk "  (test_side_channel_failure_exits_four)"
  sk "  (test_cq_reports_capacity)"
  sk "  (test_qp_reports_caps)"
  sk "  (test_local_identity)"
  sk "  (test_exchange_completed)"
  sk "  (test_init_transition)"
  sk "  (test_rtr_transition)"
  sk "  (test_rts_transition)"
  sk "  (test_verify_rts)"
  sk "  (test_failure_diagnostic)"
  sk "  (test_teardown_reports_all)"
  sk "  (test_no_leaks)"
else
  echo -e "\n${B}Device ${DEV}, GID index ${GID}, peer ${PEER}${N}"
  test_side_channel_failure_exits_four
  test_cq_reports_capacity
  test_qp_reports_caps
  test_local_identity
  test_exchange_completed
  test_init_transition
  test_rtr_transition
  test_rts_transition
  test_verify_rts
  test_failure_diagnostic
  test_teardown_reports_all
  test_no_leaks
fi

# ── Summary ───────────────────────────────────────────────────────────
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
echo -e "\n${G}Gate open.${N} Record a full run in docs/bringup-log.md, answer the"
echo -e "comprehension questions, then begin TRD-03."