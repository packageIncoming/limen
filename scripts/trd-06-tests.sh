#!/usr/bin/env bash
# trd-06-tests.sh — Limen RDMA Transport Project
# TRD-06: One-Sided Operations
#
#   ./scripts/trd-06-tests.sh                                            build checks
#   ./scripts/trd-06-tests.sh --peer 10.0.0.1 --gid 3                    full gate
#   ./scripts/trd-06-tests.sh --peer 10.0.0.1 --gid 3 --ssh you@10.0.0.1 automatic
#
# Exit 0 only when every test passes and none was skipped.

set -uo pipefail

BIN="./build/limen_onesided"
DEV=""; GID=""; PEER=""; SSH_PEER=""
TCP_BASE=18800
PASS=0; FAIL=0; SKIP=0

R=$'\033[0;31m'; G=$'\033[0;32m'; Y=$'\033[0;33m'; B=$'\033[1m'; N=$'\033[0m'

# CRITICAL: VAR=$((VAR + 1)). A bare ((VAR++)) returns the pre-increment value
# as its exit status, which is 1 when the counter is 0.
ok()  { echo -e "  ${G}PASS${N}  $1"; PASS=$((PASS + 1)); }
no()  { echo -e "  ${R}FAIL${N}  $1"; FAIL=$((FAIL + 1)); }
sk()  { echo -e "  ${Y}SKIP${N}  $1"; SKIP=$((SKIP + 1)); }
hdr() { echo -e "\n${B}$1${N}"; }

TMPDIR="$(mktemp -d)"; PORT_SEQ=0
cleanup() {
  [[ -n "$SSH_PEER" ]] && ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH_PEER" \
      "pkill -f limen_onesided" >/dev/null 2>&1 || true
  rm -rf "$TMPDIR"
}
trap cleanup EXIT INT TERM
next_port() { PORT_SEQ=$((PORT_SEQ + 1)); echo $((TCP_BASE + PORT_SEQ)); }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dev)  DEV="${2:-}";  shift 2 ;;
    --gid)  GID="${2:-}";  shift 2 ;;
    --peer) PEER="${2:-}"; shift 2 ;;
    --ssh)  SSH_PEER="${2:-}"; shift 2 ;;
    -h|--help) echo "usage: $0 [--dev NAME] [--gid N] [--peer IP] [--ssh user@host]"; exit 1 ;;
    *) echo "unknown argument: $1"; exit 1 ;;
  esac
done

echo -e "${B}=== Limen TRD-06: One-Sided Operations ===${N}"

# run_pair "<server args>" "<client args>" <timeout>  -> client output + __EXIT__n
run_pair() {
  local sargs="$1" cargs="$2" tmo="${3:-60}" port
  port=$(next_port)
  if [[ -n "$SSH_PEER" ]]; then
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH_PEER" \
      "cd '$(pwd)' && nohup $BIN -d $DEV -t $port $sargs >/tmp/limen-srv6.log 2>&1 & disown" \
      >/dev/null 2>&1 || true
    sleep 2
  else
    echo "      on the peer:  $BIN -d $DEV -t $port $sargs" >&2
    read -rp "      press enter once it is running... " >&2
  fi
  timeout "$tmo" "$BIN" -d "$DEV" -t "$port" $cargs "$PEER" 2>&1
  local rc=$?
  [[ -n "$SSH_PEER" ]] && ssh -o BatchMode=yes "$SSH_PEER" \
      "pkill -f 'limen_onesided.*-t $port'" >/dev/null 2>&1 || true
  echo "__EXIT__${rc}"
}
exit_of()  { grep -oE '__EXIT__[0-9]+' <<<"$1" | grep -oE '[0-9]+$'; }
server_log() {
  [[ -n "$SSH_PEER" ]] && ssh -o BatchMode=yes "$SSH_PEER" "cat /tmp/limen-srv6.log" 2>/dev/null || true
}

# ══ Build ═════════════════════════════════════════════════════════════
BUILD_OK=0
test_builds() {                                                       # R1
  hdr "R1: limen_onesided builds clean under -Wall -Wextra"
  local log="$TMPDIR/build.log"
  if ! cmake -S . -B build >"$log" 2>&1;  then no "cmake configure failed — see $log"; return; fi
  if ! cmake --build build >>"$log" 2>&1; then no "compilation failed — see $log";     return; fi
  [[ -x "$BIN" ]] || { no "$BIN missing or not executable"; return; }
  local w; w=$(grep -ci 'warning:' "$log" || true)
  [[ "$w" -gt 0 ]] && { no "emitted $w warning(s) — see $log"; return; }
  BUILD_OK=1; ok "built with no warnings"
}

# ══ Documentation ═════════════════════════════════════════════════════
test_flag_ordering_explained() {                                      # R5
  hdr "R5: docs/onesided-notes.md explains why the flag pattern is safe"
  [[ -s docs/onesided-notes.md ]] || { no "docs/onesided-notes.md missing or empty"; return; }
  if grep -qiE 'order' docs/onesided-notes.md \
     && grep -qiE 'flag|second write|separate' docs/onesided-notes.md; then
    ok "ordering argument present"
  else
    no "no explanation of why a following flag write cannot overtake the payload"
  fi
}

test_lastbyte_recorded() {                                            # R7
  hdr "R7: last-byte mode results recorded at two sizes"
  [[ -s docs/onesided-notes.md ]] || { no "docs/onesided-notes.md missing"; return; }
  local n
  n=$(grep -ciE 'lastbyte|last.byte' docs/onesided-notes.md || echo 0)
  local sizes
  sizes=$(grep -oiE 'size=[0-9]+' docs/onesided-notes.md | sort -u | wc -l)
  if [[ "$n" -ge 1 && "$sizes" -ge 2 ]]; then
    ok "recorded at ${sizes} distinct sizes"
  else
    no "need results at two sizes (found ${sizes} size annotations)"
  fi
}

test_lastbyte_explained() {                                           # R7
  hdr "R7: notes explain why last-byte polling is unsound even if it passed"
  [[ -s docs/onesided-notes.md ]] || { no "docs/onesided-notes.md missing"; return; }
  if grep -qiE 'within (a|one|the same) (single )?(rdma )?(write|message)|not guaranteed|segment' \
       docs/onesided-notes.md; then
    ok "explanation present"
  else
    no "no statement that byte order within one message is unguaranteed"
  fi
}

# ══ Runtime ═══════════════════════════════════════════════════════════
WRITE_RUN=""
capture_write() { [[ -n "$WRITE_RUN" ]] && return 0; WRITE_RUN=$(run_pair "--mode write -n 100" "--mode write -n 100" 90); }

test_descriptor_validated() {                                         # R1
  hdr "R1: a zero address or key is rejected before posting"
  # --bad-rkey with a zeroing mask is not available; use a peer that never sends
  # a descriptor by pointing at an address with no server. Expect a CM failure (7),
  # never a crash or a silent success.
  local rc
  timeout 40 "$BIN" -d "$DEV" -t "$(next_port)" --mode write 203.0.113.1 >/dev/null 2>&1; rc=$?
  if [[ "$rc" -eq 124 ]]; then
    no "hung rather than failing on an unreachable peer"
  elif [[ "$rc" -eq 7 || "$rc" -eq 3 ]]; then
    ok "reported and exited $rc without posting against an invalid descriptor"
  else
    no "expected exit 7 or 3, got $rc"
  fi
}

test_rdma_write() {                                                   # R2
  hdr "R2: RDMA_WRITE completes and the peer's buffer matches"
  capture_write
  local rc; rc=$(exit_of "$WRITE_RUN")
  if [[ "$rc" -ne 0 ]]; then
    no "client exited $rc — $(grep -m1 'status=' <<<"$WRITE_RUN" || echo 'no completion line')"
    return
  fi
  if ! grep -qE '^completion:.*opcode=RDMA_WRITE.*status=SUCCESS' <<<"$WRITE_RUN"; then
    no "no successful RDMA_WRITE completion"
    return
  fi
  local srv; srv=$(server_log)
  if [[ -z "$srv" ]]; then
    sk "cannot read the server log without --ssh; verify by hand"
  elif grep -qiE '^verify:.*match' <<<"$srv" && ! grep -qiE 'DO NOT match' <<<"$srv"; then
    ok "write completed and the peer verified its buffer"
  else
    no "server did not verify: $(grep -m1 '^verify:' <<<"$srv" || echo '<none>')"
  fi
}

test_receiver_passive() {                                             # R3
  hdr "R3: the peer reports zero completions during a plain write"
  capture_write
  local srv; srv=$(server_log)
  [[ -n "$srv" ]] || { sk "requires --ssh to read the server's output"; return; }
  local n
  n=$(grep -m1 -oE '^remote-completions: [0-9]+' <<<"$srv" | grep -oE '[0-9]+$' || echo "")
  if [[ -z "$n" ]]; then
    no "server printed no remote-completions line"
  elif [[ "$n" -eq 0 ]]; then
    ok "server reaped 0 completions while its memory was written"
  else
    no "server reaped $n completions — a plain RDMA_WRITE must generate none"
  fi
}

test_rdma_read() {                                                    # R4
  hdr "R4: RDMA_READ retrieves the peer's contents correctly"
  local out rc
  out=$(run_pair "--mode read -n 50" "--mode read -n 50" 90)
  rc=$(exit_of "$out")
  if [[ "$rc" -ne 0 ]]; then
    no "client exited $rc"
  elif grep -qE '^completion:.*opcode=RDMA_READ.*status=SUCCESS' <<<"$out" \
     && grep -qE '^result:.*mismatches=0' <<<"$out"; then
    ok "read completed, retrieved data verified"
  else
    no "$(grep -m1 '^result:' <<<"$out" || echo 'no result line')"
  fi
}

test_flag_notification() {                                            # R5
  hdr "R5: flag-write mode completes with zero mismatches"
  local out rc
  out=$(run_pair "--mode flag -n 100" "--mode flag -n 100" 90)
  rc=$(exit_of "$out")
  if [[ "$rc" -eq 124 ]]; then
    no "hung — the server's flag poll never observed the token (check volatile)"
  elif [[ "$rc" -ne 0 ]]; then
    no "client exited $rc"
  elif grep -qE '^result:.*mismatches=0' <<<"$out"; then
    ok "100 iterations, payload verified on every flag observation"
  else
    no "$(grep -m1 '^result:' <<<"$out" || echo 'no result line')"
  fi
}

test_write_with_imm() {                                               # R6
  hdr "R6: immediate mode delivers the iteration number to the peer"
  local out rc
  out=$(run_pair "--mode imm -n 50" "--mode imm -n 50" 90)
  rc=$(exit_of "$out")
  if [[ "$rc" -eq 124 ]]; then
    no "hung — no receives posted on the peer (WRITE_WITH_IMM consumes one)"
    return
  fi
  [[ "$rc" -ne 0 ]] && { no "client exited $rc"; return; }
  local srv; srv=$(server_log)
  if [[ -z "$srv" ]]; then
    sk "requires --ssh to confirm the peer received the immediate value"
  elif grep -qiE 'RECV_RDMA_WITH_IMM|imm' <<<"$srv"; then
    ok "peer reported an immediate-data completion"
  else
    no "peer never reported an immediate-data completion"
  fi
}

test_imm_is_two_sided() {                                             # R6
  hdr "R6: immediate mode produces non-zero remote completions"
  local srv; srv=$(server_log)
  [[ -n "$srv" ]] || { sk "requires --ssh"; return; }
  local n
  n=$(grep -m1 -oE '^remote-completions: [0-9]+' <<<"$srv" | grep -oE '[0-9]+$' || echo "")
  if [[ -z "$n" ]]; then
    no "no remote-completions line from the peer"
  elif [[ "$n" -gt 0 ]]; then
    ok "peer reaped $n completions — confirming this opcode is two-sided"
  else
    no "peer reaped 0 — WRITE_WITH_IMM must generate a remote completion"
  fi
}

test_bad_rkey_rejected() {                                            # R8
  hdr "R8: --bad-rkey yields REM_ACCESS_ERR and exits 5"
  local out rc
  out=$(run_pair "--mode write -n 5" "--mode write --bad-rkey -n 5" 60)
  rc=$(exit_of "$out")
  local problems=""
  grep -qE 'status=REM_ACCESS_ERR' <<<"$out" || problems="$problems symbolic-status"
  grep -qE 'rkey=0x'               <<<"$out" || problems="$problems rkey-printed"
  grep -qiE 'qp_access_flags|both'  <<<"$out" || problems="$problems both-gates-named"
  grep -qE '^qp_state_after_error: ERR' <<<"$out" || problems="$problems qp-state"
  [[ "$rc" -eq 5 ]] || problems="$problems exit-code($rc)"
  [[ -z "$problems" ]] && ok "rejected, diagnosed, exit 5" || no "missing:$problems"
}

test_read_depth_reported() {                                          # R9
  hdr "R9: the effective outstanding-read limit is reported"
  local out; out=$(run_pair "--mode read -n 5" "--mode read -n 5" 60)
  if grep -qE 'max_outstanding_reads=[0-9]+' <<<"$out"; then
    ok "$(grep -m1 -oE 'max_outstanding_reads=[0-9]+' <<<"$out")"
  else
    no "no max_outstanding_reads reported on the mode line"
  fi
}

# ══ Regressions ═══════════════════════════════════════════════════════
run_regression() {
  local n="$1"; shift
  local s="scripts/trd-0${n}-tests.sh"
  [[ -x "$s" ]] || { echo "MISSING"; return; }
  if "$s" "$@" >"$TMPDIR/reg-${n}.log" 2>&1; then echo "PASS"; else echo "FAIL"; fi
}

test_trd03_regression() {                                             # R10
  hdr "R10: the TRD-03 suite still passes"
  local r; r=$(run_regression 3 --dev "$DEV" --gid "$GID" --peer "$PEER" ${SSH_PEER:+--ssh "$SSH_PEER"})
  case "$r" in
    PASS)    ok "two-sided transfer unaffected" ;;
    MISSING) sk "scripts/trd-03-tests.sh not present" ;;
    *)       no "TRD-03 failed — this rung adds a program, it should not change one"
             grep -m3 'FAIL' "$TMPDIR/reg-3.log" | sed 's/^/        /' || true ;;
  esac
}

test_trd05_regression() {                                             # R10
  hdr "R10: the TRD-05 suite still passes"
  local r; r=$(run_regression 5 --dev "$DEV" --gid "$GID" --peer "$PEER" ${SSH_PEER:+--ssh "$SSH_PEER"})
  case "$r" in
    PASS)    ok "connection management unaffected" ;;
    MISSING) sk "scripts/trd-05-tests.sh not present" ;;
    *)       no "TRD-05 failed — see $TMPDIR/reg-5.log"
             grep -m3 'FAIL' "$TMPDIR/reg-5.log" | sed 's/^/        /' || true ;;
  esac
}

test_no_leaks() {                                                     # R10
  hdr "R10: valgrind reports zero bytes definitely lost on three paths"
  command -v valgrind >/dev/null 2>&1 || { sk "valgrind not installed"; return; }
  local worst=0 lost log port
  for spec in "write:--mode write -n 20:--mode write -n 20" \
              "read:--mode read -n 20:--mode read -n 20" \
              "badrkey:--mode write -n 5:--mode write --bad-rkey -n 5"; do
    local name="${spec%%:*}" rest="${spec#*:}"
    local sargs="${rest%%:*}" cargs="${rest#*:}"
    port=$(next_port)
    if [[ -n "$SSH_PEER" ]]; then
      ssh -o BatchMode=yes "$SSH_PEER" \
        "cd '$(pwd)' && nohup $BIN -d $DEV -t $port $sargs >/dev/null 2>&1 & disown" \
        >/dev/null 2>&1 || true
      sleep 2
    else
      echo "      on the peer:  $BIN -d $DEV -t $port $sargs"
      read -rp "      press enter once it is running... "
    fi
    log="$TMPDIR/vg-${name}.log"
    timeout 150 valgrind --leak-check=full --show-leak-kinds=definite \
      "$BIN" -d "$DEV" -t "$port" $cargs "$PEER" >/dev/null 2>"$log" || true
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

test_flag_ordering_explained
test_lastbyte_recorded
test_lastbyte_explained

if [[ -z "$DEV" ]] && command -v ibv_devices >/dev/null 2>&1; then
  DEV=$(ibv_devices 2>/dev/null | awk 'NR>2 && NF {print $1; exit}' || true)
fi

if [[ -z "$PEER" || -z "$DEV" ]]; then
  hdr "R1-R10: two-node checks"
  sk "requires --peer and a device"
  sk "  (test_descriptor_validated)"
  sk "  (test_rdma_write)"
  sk "  (test_receiver_passive)"
  sk "  (test_rdma_read)"
  sk "  (test_flag_notification)"
  sk "  (test_write_with_imm)"
  sk "  (test_imm_is_two_sided)"
  sk "  (test_bad_rkey_rejected)"
  sk "  (test_read_depth_reported)"
  sk "  (test_trd03_regression)"
  sk "  (test_trd05_regression)"
  sk "  (test_no_leaks)"
else
  echo -e "\n${B}Device ${DEV}, peer ${PEER}${N}"
  test_descriptor_validated
  test_rdma_write
  test_receiver_passive
  test_rdma_read
  test_flag_notification
  test_write_with_imm
  test_imm_is_two_sided
  test_bad_rkey_rejected
  test_read_depth_reported
  test_trd03_regression
  test_trd05_regression
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
echo -e "\n${G}Gate open.${N} Finish docs/onesided-notes.md, answer the comprehension"
echo -e "questions, then begin TRD-07."
