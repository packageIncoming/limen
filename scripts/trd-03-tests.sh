#!/usr/bin/env bash
# trd-03-tests.sh — Limen RDMA Transport Project
# TRD-03: Two-Sided Transfer
#
# Run from the repository root:
#
#   ./scripts/trd-03-tests.sh                                             build checks only
#   ./scripts/trd-03-tests.sh --peer 10.0.0.2 --gid 1 --dev rxe0          two-node, manual
#   ./scripts/trd-03-tests.sh --peer 10.0.0.2 --gid 1 --dev rxe0 \
#                             --ssh main@10.0.0.2                         fully automatic
#
# Options:
#   --dev NAME          local RDMA device (auto-detected when omitted)
#   --gid N             GID index, both ends
#   --peer IP           address the client dials
#   --ssh user@host     start the far side automatically over ssh
#   --keep-logs         always keep captured logs, not just on failure
#
# With --ssh, the freshly built local binary is copied to /tmp/limen-peer on
# the peer before every run. Nothing needs to be installed or checked out
# there, and the two sides can never be running different builds.
#
# Exit 0 only when every test passes and none was skipped.

set -uo pipefail

BIN_REL="build/limen_pingpong"
BIN="./$BIN_REL"
DEV=""; GID=""; PEER=""; SSH_PEER=""
REMOTE_DIR="/tmp/limen-peer"
REMOTE_BIN="$REMOTE_DIR/limen_pingpong"
KEEP_LOGS=0
TCP_BASE=18600
PASS=0; FAIL=0; SKIP=0

R=$'\033[0;31m'; G=$'\033[0;32m'; Y=$'\033[0;33m'; B=$'\033[1m'; N=$'\033[0m'

# CRITICAL: VAR=$((VAR + 1)). A bare ((VAR++)) returns the pre-increment value
# as its exit status, which is 1 when the counter is 0.
ok()  { echo -e "  ${G}PASS${N}  $1"; PASS=$((PASS + 1)); }
no()  { echo -e "  ${R}FAIL${N}  $1"; FAIL=$((FAIL + 1)); }
sk()  { echo -e "  ${Y}SKIP${N}  $1"; SKIP=$((SKIP + 1)); }
hdr() { echo -e "\n${B}$1${N}"; }

# Preflight reporting is deliberately outside the PASS/FAIL counters. Preflight
# checks verify the harness and the environment, not the requirements.
pre_ok() { echo -e "  ${G} ok ${N}  $1"; }
pre_no() { echo -e "  ${R}stop${N}  $1"; }
pre_wn() { echo -e "  ${Y}warn${N}  $1"; }

TMPDIR="$(mktemp -d)"
PORT_SEQ=0
LOGDIR="./.trd03-logs"

# ── SSH plumbing ──────────────────────────────────────────────────────
# ControlMaster makes the dozens of short control-plane calls below cost one
# TCP handshake in total instead of one each. -n keeps ssh from consuming the
# script's stdin, which otherwise eats the manual-mode `read` prompts.
SSH_OPTS=(
  -o BatchMode=yes
  -o ConnectTimeout=10
  -o StrictHostKeyChecking=accept-new
  -o ControlMaster=auto
  -o ControlPath="$TMPDIR/cm-%C"
  -o ControlPersist=180
)
# rsh: control-plane calls, stdin closed so ssh cannot eat the script's stdin.
# rsh_pipe: same host, stdin left open, for streaming a tarball across.
rsh()      { ssh -n "${SSH_OPTS[@]}" "$SSH_PEER" "$@"; }
rsh_pipe() { ssh    "${SSH_OPTS[@]}" "$SSH_PEER" "$@"; }

cleanup() {
  if [[ -n "$SSH_PEER" ]]; then
    # The bracket keeps the pattern from matching the remote shell that is
    # running pkill, whose own command line contains this very string.
    rsh "pkill -f '[l]imen_pingpong'" >/dev/null 2>&1 || true
    ssh "${SSH_OPTS[@]}" -O exit "$SSH_PEER" >/dev/null 2>&1 || true
  fi
  if [[ "$KEEP_LOGS" -eq 1 || "$FAIL" -ne 0 ]] && compgen -G "$TMPDIR/*" >/dev/null; then
    rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"
    cp -a "$TMPDIR"/. "$LOGDIR"/ 2>/dev/null || true
    echo -e "\nLogs kept in ${B}${LOGDIR}${N}"
  fi
  rm -rf "$TMPDIR"
}
trap cleanup EXIT INT TERM

next_port() { PORT_SEQ=$((PORT_SEQ + 1)); echo $((TCP_BASE + PORT_SEQ)); }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dev)          DEV="${2:-}";          shift 2 ;;
    --gid)          GID="${2:-}";          shift 2 ;;
    --peer)         PEER="${2:-}";         shift 2 ;;
    --ssh)          SSH_PEER="${2:-}";     shift 2 ;;
    --keep-logs)    KEEP_LOGS=1;           shift   ;;
    -h|--help)
      sed -n '3,25p' "$0" | sed 's/^# \{0,1\}//'
      exit 1 ;;
    *) echo "unknown argument: $1"; exit 1 ;;
  esac
done



remote_log_local() { echo "$TMPDIR/server-$1.log"; }

remote_fetch_log() {
  local port="$1"
  rsh "cat /tmp/limen-srv-${port}.log 2>/dev/null" \
      >"$(remote_log_local "$port")" 2>/dev/null || true
}

dump_server_log() {
  local port="$1" f
  f="$(remote_log_local "$port")"
  if [[ -s "$f" ]]; then
    echo "      ---- peer server output (port $port) ----" >&2
    sed 's/^/      | /' "$f" >&2
    echo "      ----------------------------------------" >&2
  else
    echo "      (peer server produced no output on port $port)" >&2
  fi
}

remote_stop() {
  local port="$1"
  rsh "pkill -f '[l]imen_pingpong.*-t ${port}([[:space:]]|\$)'" >/dev/null 2>&1 || true
}

# Start the far side and do not return until it is actually listening, it has
# died, or the wait expires. A fixed `sleep 2` is the difference between this
# suite working and every test failing with a connect timeout.
remote_start() {
  local port="$1" sargs="$2"
  local rlog="/tmp/limen-srv-${port}.log" cmd

  printf -v cmd 'cd %q && rm -f %q && { setsid env LD_LIBRARY_PATH=%q nohup %q -d %q -g %q -t %q %s >%q 2>&1 </dev/null & } && sleep 0.3' \
    "$REMOTE_DIR" "$rlog" "$REMOTE_DIR" "$REMOTE_BIN" "$DEV" "$GID" "$port" "$sargs" "$rlog"

  if ! rsh "$cmd" >/dev/null 2>"$TMPDIR/ssh-start-$port.err"; then
    echo "      ssh could not launch the peer server on port $port" >&2
    sed 's/^/      | /' "$TMPDIR/ssh-start-$port.err" >&2
    return 1
  fi

  # Readiness is polled on the peer in a single round trip. /proc/net/tcp is
  # the primary probe because it needs no iproute2, no net-tools, and no
  # cooperation from the program under test; state 0A is LISTEN.
  local hexport; hexport=$(printf '%04X' "$port")
  printf -v cmd 'for i in $(seq 1 80); do
      if awk -v p=%q '"'"'$2 ~ ":"p"$" && $4 == "0A" {f=1} END {exit !f}'"'"' /proc/net/tcp* 2>/dev/null; then exit 0; fi
      if { ss -ltnH 2>/dev/null || netstat -ltn 2>/dev/null; } | grep -q ":%s "; then exit 0; fi
      if ! pgrep -f "[l]imen_pingpong.*-t %s([[:space:]]|$)" >/dev/null 2>&1; then exit 2; fi
      sleep 0.25
    done
    exit 1' "$hexport" "$port" "$port"

  rsh "$cmd" >/dev/null 2>&1
  local rc=$?
  case "$rc" in
    0) return 0 ;;
    2) echo "      the peer server exited before it began listening on $port" >&2 ;;
    *) echo "      the peer server never reached LISTEN on $port within 20s" >&2 ;;
  esac
  remote_fetch_log "$port"
  dump_server_log "$port"
  return 1
}

# ── Binary push ───────────────────────────────────────────────────────
# The peer runs the exact bytes this machine just built. Streaming the binary
# over the already-open ssh connection means the peer needs no checkout, no
# toolchain, and no cmake, and the two sides cannot drift apart.
push_binary() {
  local err="$TMPDIR/push.err"
  if ! rsh_pipe "mkdir -p ${REMOTE_DIR@Q} && cat > ${REMOTE_BIN@Q}.new && chmod +x ${REMOTE_BIN@Q}.new && mv -f ${REMOTE_BIN@Q}.new ${REMOTE_BIN@Q}" \
       <"$BIN" 2>"$err"; then
    pre_no "could not copy the binary to $REMOTE_BIN"
    sed 's/^/        /' "$err" >&2
    return 1
  fi
  pre_ok "pushed $(stat -c %s "$BIN") bytes to $REMOTE_BIN"

  # The project's own shared libraries ride along. Nothing to install on the
  # peer: they sit beside the binary and LD_LIBRARY_PATH points at that dir.
  local libs lib
  libs=$(ldd "$BIN" 2>/dev/null | awk '$1 ~ /^lib/ && $1 !~ /^(libc|libm|libgcc|libstdc|libpthread|libdl|librt|libibverbs|librdmacm|libnl)/ && $3 ~ /^\// {print $3}')
  [[ -z "$libs" ]] && libs=$(find build -maxdepth 3 -name 'lib*.so*' -type f 2>/dev/null)
  for lib in $libs; do
    if ! rsh_pipe "cat > ${REMOTE_DIR@Q}/$(basename "$lib")" <"$lib" 2>/dev/null; then
      pre_no "could not copy $lib to the peer"; return 1
    fi
    pre_ok "pushed $(basename "$lib")"
  done

  local missing
  missing=$(rsh "LD_LIBRARY_PATH=${REMOTE_DIR@Q} ldd ${REMOTE_BIN@Q} 2>&1 | grep 'not found'" || true)
  if [[ -n "$missing" ]]; then
    pre_no "the binary will not load on the peer, shared libraries are missing"
    sed 's/^/        /' <<<"$missing" >&2
    echo "        If these are system libraries, install rdma-core / libibverbs there." >&2
    return 1
  fi
  return 0
}

# ── Preflight (ssh mode only) ─────────────────────────────────────────
ssh_preflight() {
  hdr "Preflight: peer $SSH_PEER"
  local out

  if ! out=$(rsh "echo ok" 2>&1); then
    pre_no "cannot ssh to $SSH_PEER non-interactively"
    echo "        $out" >&2
    echo "        BatchMode=yes means no password prompt is possible. Fix with:" >&2
    echo "          ssh-keygen -t ed25519       # only if you have no key yet" >&2
    echo "          ssh-copy-id $SSH_PEER" >&2
    return 1
  fi
  pre_ok "ssh reachable, key auth working"

  if ! push_binary; then return 1; fi

  if ! rsh "test -d /sys/class/infiniband/${DEV@Q}" 2>/dev/null; then
    pre_no "device $DEV not present on the peer"
    rsh "ls /sys/class/infiniband 2>/dev/null" | sed 's/^/        available: /' >&2
    return 1
  fi
  pre_ok "device $DEV present on the peer"

  local mlock
  mlock=$(rsh "ulimit -l" 2>/dev/null | tr -d '[:space:]')
  if [[ "$mlock" =~ ^[0-9]+$ && "$mlock" -lt 1024 ]]; then
    pre_wn "peer ulimit -l is ${mlock}kB; memory registration may fail"
  fi


  rsh "pkill -f '[l]imen_pingpong'" >/dev/null 2>&1 || true
  return 0
}

# ── Helper: run one client/server pair, echo the client's output ──────
# usage: run_pair "<server extra args>" "<client extra args>" <timeout>
# Exit code 199 is reserved for "the harness could not start the peer server",
# which is a setup failure and not a requirement failure.
run_pair() {
  local sargs="$1" cargs="$2" tmo="${3:-60}" port out rc
  port=$(next_port)

  if [[ -n "$SSH_PEER" ]]; then
    if ! remote_start "$port" "$sargs"; then
      echo "__EXIT__199"
      return
    fi
  else
    echo "      on the peer:  $BIN -d $DEV -g $GID -t $port $sargs" >&2
    read -rp "      press enter once it is running... " >&2
  fi

  out=$(timeout "$tmo" "$BIN" -d "$DEV" -g "$GID" -t "$port" $cargs "$PEER" 2>&1)
  rc=$?

  if [[ -n "$SSH_PEER" ]]; then
    remote_fetch_log "$port"
    if [[ "$rc" -ne 0 ]]; then
      echo "      client exited $rc on port $port" >&2
      dump_server_log "$port"
    fi
    remote_stop "$port"
  fi

  printf '%s\n' "$out"
  echo "__EXIT__${rc}"
}
exit_of() { grep -oE '__EXIT__[0-9]+' <<<"$1" | grep -oE '[0-9]+$'; }
body_of() { grep -v '__EXIT__' <<<"$1"; }

# Any test that consumed a run where the harness itself failed reports that
# fact instead of blaming the implementation. Returns 0 when it handled it.
setup_failed() {
  local out="$1"
  if [[ "$(exit_of "$out")" == "199" ]]; then
    no "peer server could not be started — this is a harness/setup failure, not a requirement failure"
    return 0
  fi
  return 1
}

# ── Build ─────────────────────────────────────────────────────────────
BUILD_OK=0
test_builds() {                                                      # R1
  hdr "R1: limen_pingpong builds clean under -Wall -Wextra"
  local log="$TMPDIR/build.log"
  if ! cmake -S . -B build >"$log" 2>&1;      then no "cmake configure failed — see $log"; return; fi
  if ! cmake --build build >>"$log" 2>&1;     then no "compilation failed — see $log";     return; fi
  if [[ ! -x "$BIN" ]];                       then no "$BIN missing or not executable";    return; fi
  local warns; warns=$(grep -ci 'warning:' "$log" || true)
  if [[ "$warns" -gt 0 ]];                    then no "emitted $warns warning(s) — see $log"; return; fi
  BUILD_OK=1; ok "built with no warnings"
}

# ── Baseline run, reused by several tests ─────────────────────────────
BASE=""
capture_base() {
  [[ -n "$BASE" ]] && return 0
  BASE=$(run_pair "-n 100 -s 4096" "-n 100 -s 4096" 90)
}

test_recv_buffers_reported() {                                       # R1
  hdr "R1: recv line reports slots, depth, and size"
  capture_base
  setup_failed "$BASE" && return
  if grep -qE '^recv: posted=[0-9]+ depth=[0-9]+ size=[0-9]+' <<<"$BASE"; then
    ok "$(grep -m1 '^recv:' <<<"$BASE")"
  else
    no "no conforming recv: line"
  fi
}

test_recv_posted_first() {                                           # R2
  hdr "R2: receives are posted before the first send"
  capture_base
  setup_failed "$BASE" && return
  local recv_ln send_ln
  recv_ln=$(grep -n '^recv: posted=' <<<"$BASE" | head -1 | cut -d: -f1)
  send_ln=$(grep -n '^completion:.*opcode=SEND' <<<"$BASE" | head -1 | cut -d: -f1)
  if [[ -z "$recv_ln" ]]; then
    no "no recv: line to order against"
  elif [[ -z "$send_ln" ]]; then
    no "no send completion to order against"
  elif [[ "$recv_ln" -lt "$send_ln" ]]; then
    ok "recv posting precedes the first send completion"
  else
    no "first send completion appears before receives were posted"
  fi
}

test_send_completion_appears() {                                     # R3
  hdr "R3: a signalled send produces a send completion"
  capture_base
  setup_failed "$BASE" && return
  if grep -qE '^completion:.*opcode=SEND.*status=SUCCESS' <<<"$BASE"; then
    ok "send completion present"
  else
    no "no successful SEND completion — check IBV_SEND_SIGNALED"
  fi
}

test_poll_tolerates_empty() {                                        # R4
  hdr "R4: an empty poll does not terminate the loop"
  capture_base
  setup_failed "$BASE" && return
  # A run that completes all iterations necessarily survived many empty polls.
  local rc; rc=$(exit_of "$BASE")
  if [[ "$rc" -eq 0 ]] && grep -qE '^result: iterations=100' <<<"$BASE"; then
    ok "100 iterations completed, so empty polls were handled"
  else
    no "run did not complete — exit $rc"
  fi
}

test_success_fields_reported() {                                     # R5
  hdr "R5: success completions report opcode, and byte_len for receives"
  capture_base
  setup_failed "$BASE" && return
  local problems=""
  grep -qE '^completion:.*opcode=RECV.*status=SUCCESS.*byte_len=[0-9]+' <<<"$BASE" \
    || problems="$problems recv-byte_len"
  grep -qE '^completion:.*opcode=SEND.*status=SUCCESS' <<<"$BASE" \
    || problems="$problems send-opcode"
  if [[ -z "$problems" ]]; then ok "success fields present"; else no "missing:$problems"; fi
}

test_pingpong_completes() {                                          # R6
  hdr "R6: full ping-pong completes with matching counts"
  capture_base
  setup_failed "$BASE" && return
  local line; line=$(grep -m1 '^result:' <<<"$BASE" || true)
  [[ -n "$line" ]] || { no "no result: line"; return; }
  local it se re
  it=$(grep -oE 'iterations=[0-9]+' <<<"$line" | grep -oE '[0-9]+')
  se=$(grep -oE 'sent=[0-9]+'       <<<"$line" | grep -oE '[0-9]+')
  re=$(grep -oE 'received=[0-9]+'   <<<"$line" | grep -oE '[0-9]+')
  if [[ "$it" -eq 100 && "$re" -eq 100 && "$se" -ge 99 ]]; then
    ok "iterations=$it sent=$se received=$re"
  else
    no "counts do not match: $line"
  fi
}

test_iteration_count_honoured() {                                    # R6
  hdr "R6: a non-default iteration count is honoured"
  local out; out=$(run_pair "-n 7" "-n 7" 60)
  setup_failed "$out" && return
  if grep -qE '^result: iterations=7 ' <<<"$out" && grep -qE 'received=7' <<<"$out"; then
    ok "7 iterations requested and performed"
  else
    no "$(grep -m1 '^result:' <<<"$out" || echo 'no result line')"
  fi
}

test_payload_verified() {                                            # R7
  hdr "R7: payloads verify with zero mismatches"
  capture_base
  setup_failed "$BASE" && return
  if grep -qE '^result:.*mismatches=0' <<<"$BASE"; then
    ok "mismatches=0"
  else
    no "$(grep -m1 '^result:' <<<"$BASE" || echo 'no result line')"
  fi
}

test_rnr_infinite_times_out() {                                      # R8
  hdr "R8: --no-recv with infinite retry times out rather than hanging"
  local out rc
  out=$(run_pair "--no-recv" "-n 5" 45)
  setup_failed "$out" && return
  rc=$(exit_of "$out")
  if [[ "$rc" -eq 124 ]]; then
    no "the client hung — R4's timeout is missing or too long"
  elif [[ "$rc" -eq 5 ]] && grep -qiE 'timeout|timed out' <<<"$out"; then
    ok "timed out and said so (exit 5)"
  else
    no "expected a reported timeout with exit 5, got exit $rc"
  fi
}

RNR=""
capture_rnr() {
  [[ -n "$RNR" ]] && return 0
  RNR=$(run_pair "--no-recv --rnr-retry 1" "--rnr-retry 1 -n 5" 60)
}

test_rnr_finite_reports_error() {                                    # R8
  hdr "R8: --no-recv --rnr-retry 1 yields an RNR retry-exceeded completion"
  capture_rnr
  setup_failed "$RNR" && return
  local rc; rc=$(exit_of "$RNR")
  if grep -qE 'status=RNR_RETRY_EXC_ERR' <<<"$RNR"; then
    ok "RNR_RETRY_EXC_ERR reported (exit $rc)"
  else
    no "no RNR_RETRY_EXC_ERR completion — got: $(grep -m1 '^completion:.*status=' <<<"$RNR" || echo '<none>')"
  fi
}

test_error_field_validity() {                                        # R5
  hdr "R5: error completions omit opcode/byte_len and carry the validity note"
  capture_rnr
  setup_failed "$RNR" && return
  local errline
  errline=$(grep -m1 -E '^completion:.*status=(RNR_RETRY_EXC_ERR|RETRY_EXC_ERR|WR_FLUSH_ERR)' <<<"$RNR" || true)
  [[ -n "$errline" ]] || { sk "no error completion produced on this fabric"; return; }
  local problems=""
  grep -q 'opcode='   <<<"$errline" && problems="$problems opcode-printed-anyway"
  grep -q 'byte_len=' <<<"$errline" && problems="$problems byte_len-printed-anyway"
  grep -qE 'vendor_err=' <<<"$errline" || problems="$problems no-vendor_err"
  grep -qiE 'note:.*not valid on an error completion' <<<"$RNR" || problems="$problems no-note"
  if [[ -z "$problems" ]]; then
    ok "error line restricted to the four valid fields, note present"
  else
    no "problems:$problems"
  fi
}

test_qp_error_after_failure() {                                      # R10
  hdr "R10: the queue pair reports ERR after an error completion"
  capture_rnr
  setup_failed "$RNR" && return
  if grep -qE '^qp_state_after_error: ERR' <<<"$RNR"; then
    ok "qp_state_after_error: ERR"
  else
    no "$(grep -m1 '^qp_state_after_error:' <<<"$RNR" || echo 'no qp_state_after_error line')"
  fi
}

test_first_error_reported() {                                        # R10
  hdr "R10: the first non-success status is reported, not the last"
  capture_rnr
  setup_failed "$RNR" && return
  local first
  first=$(grep -oE '^completion:.*status=[A-Z_]+' <<<"$RNR" \
          | grep -v 'status=SUCCESS' | head -1 | grep -oE 'status=[A-Z_]+' | cut -d= -f2)
  local reported
  reported=$(grep -m1 '^result:' <<<"$RNR" | grep -oE 'first_error=[A-Z_]+' | cut -d= -f2)
  if [[ -z "$reported" ]]; then
    no "result line carries no first_error field"
  elif [[ "$reported" == "$first" ]]; then
    ok "first_error=$reported matches the first error completion"
  else
    no "first_error=$reported but the first error completion was $first"
  fi
}

test_unsignaled_no_completion() {                                    # R9
  hdr "R9: --unsignaled transfers data, reports zero send completions, exits 0"
  local out rc
  out=$(run_pair "-n 20" "--unsignaled -n 20" 60)
  setup_failed "$out" && return
  rc=$(exit_of "$out")
  if [[ "$rc" -eq 124 ]]; then
    no "hung waiting for a send completion that was never coming"
    return
  fi
  local sc
  sc=$(grep -m1 '^result:' <<<"$out" | grep -oE 'send_completions=[0-9]+' | cut -d= -f2)
  if [[ -z "$sc" ]]; then
    no "result line carries no send_completions field"
  elif [[ "$sc" -ne 0 ]]; then
    no "expected send_completions=0 with --unsignaled, got $sc"
  elif [[ "$rc" -ne 0 ]]; then
    no "send_completions=0 as expected but exit was $rc, should be 0"
  else
    ok "data moved, send_completions=0, exit 0"
  fi
}

test_teardown_reports_all() {                                        # R11
  hdr "R11: teardown line reports all stages"
  capture_base
  setup_failed "$BASE" && return
  local line; line=$(grep -m1 '^teardown: ' <<<"$BASE" || true)
  [[ -n "$line" ]] || { no "no teardown: line"; return; }
  local problems="" f
  for f in qp cq pd context; do
    grep -qE "${f}=ok" <<<"$line" || problems="$problems ${f}"
  done
  grep -qE '(rx_mr|mr)=ok' <<<"$line" || problems="$problems rx_mr"
  grep -qE '(tx_mr|mr)=ok' <<<"$line" || problems="$problems tx_mr"
  if [[ -z "$problems" ]]; then ok "all stages ok"; else no "not ok:$problems — in: $line"; fi
}

test_no_leaks() {                                                    # R11
  hdr "R11: valgrind reports zero bytes definitely lost, success and RNR paths"
  if ! command -v valgrind >/dev/null 2>&1; then sk "valgrind not installed"; return; fi
  local worst=0
  for spec in "success::-n 20" "rnr:--no-recv --rnr-retry 1:--rnr-retry 1 -n 5"; do
    local name="${spec%%:*}" rest="${spec#*:}"
    local sargs="${rest%%:*}" cargs="${rest#*:}"
    local port; port=$(next_port)

    if [[ -n "$SSH_PEER" ]]; then
      if ! remote_start "$port" "$sargs"; then
        no "peer server could not be started for the ${name} path — harness/setup failure"
        return
      fi
    else
      echo "      on the peer:  $BIN -d $DEV -g $GID -t $port $sargs"
      read -rp "      press enter once it is running... "
    fi

    local log="$TMPDIR/vg-${name}.log" cout="$TMPDIR/vg-${name}.out"
    timeout 120 valgrind --leak-check=full --show-leak-kinds=definite \
      "$BIN" -d "$DEV" -g "$GID" -t "$port" $cargs "$PEER" >"$cout" 2>"$log"
    local crc=$?
    [[ -n "$SSH_PEER" ]] && { remote_fetch_log "$port"; remote_stop "$port"; }

    # A client that died at connect time leaks nothing and proves nothing.
    # Without this guard the test passes vacuously on a broken fabric.
    if [[ "$name" == "success" ]] && ! grep -qh '^result:' "$cout" "$log" 2>/dev/null; then
      no "the success-path client under valgrind never produced a result: line (exit $crc) — the leak count would be meaningless"
      [[ -n "$SSH_PEER" ]] && dump_server_log "$port"
      return
    fi

    local lost
    lost=$(grep -oE 'definitely lost: [0-9,]+ bytes' "$log" \
           | grep -oE '[0-9,]+' | tr -d ',' || echo "")
    if [[ -z "$lost" ]]; then
      no "could not parse valgrind output for the ${name} path — see $log"; return
    fi
    [[ "$lost" -gt "$worst" ]] && worst="$lost"
  done
  if [[ "$worst" -eq 0 ]]; then
    ok "0 bytes definitely lost on both paths"
  else
    no "${worst} bytes definitely lost on at least one path — see $TMPDIR/vg-*.log"
  fi
}

# ── Run ───────────────────────────────────────────────────────────────
echo -e "${B}=== Limen TRD-03: Two-Sided Transfer ===${N}"

test_builds
if [[ "$BUILD_OK" -ne 1 ]]; then
  echo -e "\n${R}Gate closed.${N} Nothing else can run until the build succeeds."
  exit 1
fi

if [[ -z "$DEV" ]] && command -v ibv_devices >/dev/null 2>&1; then
  DEV=$(ibv_devices 2>/dev/null | awk 'NR>2 && NF {print $1; exit}' || true)
fi

if [[ -n "$SSH_PEER" ]]; then
  if [[ -z "$PEER" || -z "$GID" || -z "$DEV" ]]; then
    hdr "Preflight: peer $SSH_PEER"
    pre_no "--ssh also needs --peer, --gid, and a device"
    echo -e "\n${R}Gate closed.${N}"
    exit 1
  fi
  if ! ssh_preflight; then
    echo -e "\n${R}Gate closed.${N} The peer is not usable, so no requirement was tested."
    exit 1
  fi
fi

if [[ -z "$PEER" || -z "$GID" || -z "$DEV" ]]; then
  hdr "R1–R11: two-node checks"
  sk "requires --peer, --gid, and a device"
  sk "  (test_recv_buffers_reported)"
  sk "  (test_recv_posted_first)"
  sk "  (test_send_completion_appears)"
  sk "  (test_poll_tolerates_empty)"
  sk "  (test_success_fields_reported)"
  sk "  (test_pingpong_completes)"
  sk "  (test_iteration_count_honoured)"
  sk "  (test_payload_verified)"
  sk "  (test_rnr_infinite_times_out)"
  sk "  (test_rnr_finite_reports_error)"
  sk "  (test_error_field_validity)"
  sk "  (test_qp_error_after_failure)"
  sk "  (test_first_error_reported)"
  sk "  (test_unsignaled_no_completion)"
  sk "  (test_teardown_reports_all)"
  sk "  (test_no_leaks)"
else
  echo -e "\n${B}Local device ${DEV}, GID index ${GID}, peer ${PEER}${N}"
  [[ -n "$SSH_PEER" ]] && echo -e "${B}Peer ${SSH_PEER}, binary staged at ${REMOTE_BIN}${N}"
  test_recv_buffers_reported
  test_recv_posted_first
  test_send_completion_appears
  test_poll_tolerates_empty
  test_success_fields_reported
  test_pingpong_completes
  test_iteration_count_honoured
  test_payload_verified
  test_rnr_infinite_times_out
  test_rnr_finite_reports_error
  test_error_field_validity
  test_qp_error_after_failure
  test_first_error_reported
  test_unsignaled_no_completion
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
echo -e "\n${G}Gate open.${N} Record all four captures in docs/transfer-log.md, answer the"
echo -e "comprehension questions, then begin TRD-04."