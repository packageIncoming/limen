#!/usr/bin/env bash
# trd-07-tests.sh — Limen RDMA Transport Project
# TRD-07: Completion Efficiency
#
#   ./scripts/trd-07-tests.sh                                            build + config
#   ./scripts/trd-07-tests.sh --peer 10.0.0.1 --gid 3                    full gate
#   ./scripts/trd-07-tests.sh --peer 10.0.0.1 --gid 3 --ssh you@10.0.0.1 automatic
#
# Exit 0 only when every test passes and none was skipped.

set -uo pipefail

BIN="./build/limen_pingpong"
DEV=""; GID=""; PEER=""; SSH_PEER=""
TCP_BASE=18900
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
      "pkill -f limen_pingpong" >/dev/null 2>&1 || true
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

echo -e "${B}=== Limen TRD-07: Completion Efficiency ===${N}"

run_pair() {
  local sargs="$1" cargs="$2" tmo="${3:-90}" port
  port=$(next_port)
  if [[ -n "$SSH_PEER" ]]; then
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH_PEER" \
      "cd '$(pwd)' && nohup $BIN -d $DEV -t $port $sargs >/dev/null 2>&1 & disown" \
      >/dev/null 2>&1 || true
    sleep 2
  else
    echo "      on the peer:  $BIN -d $DEV -t $port $sargs" >&2
    read -rp "      press enter once it is running... " >&2
  fi
  timeout "$tmo" "$BIN" -d "$DEV" -t "$port" $cargs "$PEER" 2>&1
  local rc=$?
  [[ -n "$SSH_PEER" ]] && ssh -o BatchMode=yes "$SSH_PEER" \
      "pkill -f 'limen_pingpong.*-t $port'" >/dev/null 2>&1 || true
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
  [[ -x "$BIN" ]] || { no "$BIN missing"; return; }
  local w; w=$(grep -ci 'warning:' "$log" || true)
  [[ "$w" -gt 0 ]] && { no "emitted $w warning(s) — see $log"; return; }
  BUILD_OK=1; ok "built with no warnings"
}

# ══ Config, no peer needed ════════════════════════════════════════════
CONF=""
capture_config() {
  [[ -n "$CONF" ]] && return 0
  CONF=$("$BIN" -d "$DEV" --report-config 2>&1 || true)
}

test_reports_config() {                                               # R1
  hdr "R1: --report-config prints all five settings and exits 0"
  local out rc
  out=$("$BIN" -d "$DEV" --report-config 2>&1); rc=$?
  [[ "$rc" -ne 0 ]] && { no "exited $rc"; return; }
  local problems="" f
  for f in inline= signal_every= pipeline= reap= moderation=; do
    grep -q "$f" <<<"$out" || problems="$problems ${f%=}"
  done
  [[ -z "$problems" ]] && ok "$(grep -m1 '^config:' <<<"$out")" \
                       || no "config line missing:$problems"
}

test_inline_limit_reported() {                                        # R2
  hdr "R2: the granted max_inline_data is reported"
  capture_config
  if grep -qE 'inline=(on|off)\(max=[0-9]+\)' <<<"$CONF"; then
    ok "$(grep -oE 'inline=[a-z]+\(max=[0-9]+\)' <<<"$CONF")"
  else
    no "no granted inline limit in the config line"
  fi
}

test_signal_period_validated() {                                      # R3
  hdr "R3: a signalling period exceeding the send-queue depth is rejected"
  local rc
  "$BIN" -d "$DEV" --signal-every 999999 --report-config >/dev/null 2>&1; rc=$?
  [[ "$rc" -eq 1 ]] && ok "exit 1 at parse time" \
                    || no "expected exit 1, got $rc — an over-long period fills the send queue"
}

test_moderation_graceful() {                                          # R9
  hdr "R9: unsupported moderation reports unavailable and exits 0"
  local out rc
  out=$("$BIN" -d "$DEV" --moderate 16:8 --report-config 2>&1); rc=$?
  if [[ "$rc" -ne 0 ]]; then
    no "exited $rc — an unsupported optimisation must not be fatal"
  elif grep -qE 'moderation=(unavailable|[0-9]+:[0-9]+)' <<<"$out"; then
    ok "$(grep -oE 'moderation=[^ ]+' <<<"$out")"
  else
    no "moderation state not reported distinctly"
  fi
}

# ══ Runtime ═══════════════════════════════════════════════════════════
test_inline_correct() {                                               # R2
  hdr "R2: a run with --inline completes with zero mismatches"
  local out; out=$(run_pair "-n 500 -s 64" "--inline -n 500 -s 64")
  if grep -qE '^result:.*mismatches=0' <<<"$out"; then
    ok "500 iterations inline, payloads verified"
  else
    no "$(grep -m1 '^result:' <<<"$out" || echo 'no result line')"
  fi
}

test_signal_every_reduces() {                                         # R3
  hdr "R3: --signal-every 16 reaps roughly a sixteenth of the sends"
  local out; out=$(run_pair "-n 1000" "--signal-every 16 -n 1000")
  local sc
  sc=$(grep -m1 '^result:' <<<"$out" | grep -oE 'send_completions=[0-9]+' | cut -d= -f2)
  if [[ -z "$sc" ]]; then
    no "result line carries no send_completions field"
  elif [[ "$sc" -gt 0 && "$sc" -lt 200 ]]; then
    ok "send_completions=$sc for 1000 sends (expected ~63)"
  else
    no "send_completions=$sc — expected far fewer than 1000 and more than 0"
  fi
}

test_buffer_reuse_correct() {                                         # R4
  hdr "R4: payloads verify at several signalling periods"
  local bad="" p
  for p in 1 4 16; do
    local out; out=$(run_pair "-n 500" "--signal-every $p --pipeline 8 -n 500")
    grep -qE '^result:.*mismatches=0' <<<"$out" || bad="$bad period=$p"
  done
  [[ -z "$bad" ]] && ok "zero mismatches at periods 1, 4, 16" \
                  || no "corruption at:$bad — covered-sequence tracking is wrong"
}

test_pipeline_clamped() {                                             # R5
  hdr "R5: pipeline depth is clamped to the granted send-queue depth"
  local out
  out=$("$BIN" -d "$DEV" --pipeline 999999 --report-config 2>&1 || true)
  local d
  d=$(grep -oE 'pipeline=[0-9]+' <<<"$out" | cut -d= -f2)
  if [[ -z "$d" ]]; then
    no "no pipeline value reported"
  elif [[ "$d" -lt 999999 && "$d" -gt 0 ]]; then
    ok "clamped to $d"
  else
    no "pipeline=$d — an unclamped request will fail at post time"
  fi
}

test_pipeline_correct() {                                             # R5
  hdr "R5: a pipelined run completes with zero mismatches"
  local out; out=$(run_pair "-n 1000" "--pipeline 8 --signal-every 4 -n 1000")
  grep -qE '^result:.*mismatches=0' <<<"$out" \
    && ok "depth 8, 1000 iterations, verified" \
    || no "$(grep -m1 '^result:' <<<"$out" || echo 'no result line')"
}

test_event_mode_completes() {                                         # R6
  hdr "R6: event-driven reaping completes a run without hanging"
  local out rc
  out=$(run_pair "-n 500 --reap event" "--reap event -n 500")
  rc=$(exit_of "$out")
  if [[ "$rc" -eq 124 ]]; then
    no "hung — check the four rules in Hints H6 (arm, re-arm, drain, race-poll)"
  elif [[ "$rc" -eq 8 ]]; then
    no "completion-channel timeout — an event was expected and never arrived"
  elif [[ "$rc" -ne 0 ]]; then
    no "exited $rc"
  elif grep -qE '^result:.*mismatches=0' <<<"$out"; then
    ok "event mode completed, payloads verified"
  else
    no "$(grep -m1 '^result:' <<<"$out" || echo 'no result line')"
  fi
}

test_empty_events_tolerated() {                                       # R6
  hdr "R6: an event carrying no completion is not treated as an error"
  local out; out=$(run_pair "-n 500 --reap event" "--reap event -n 500")
  if ! grep -qE '^events:' <<<"$out"; then
    no "no events: line — empty_events must be counted and reported"
    return
  fi
  local rc; rc=$(exit_of "$out")
  if [[ "$rc" -eq 0 ]]; then
    ok "$(grep -m1 '^events:' <<<"$out")"
  else
    no "run exited $rc — an empty event must not be fatal"
  fi
}

test_events_all_acked() {                                             # R7
  hdr "R7: events received and acknowledged are equal at exit"
  local out; out=$(run_pair "-n 500 --reap event" "--reap event -n 500")
  local line; line=$(grep -m1 '^events:' <<<"$out" || true)
  [[ -n "$line" ]] || { no "no events: line"; return; }
  local r a
  r=$(grep -oE 'received=[0-9]+' <<<"$line" | cut -d= -f2)
  a=$(grep -oE 'acked=[0-9]+'    <<<"$line" | cut -d= -f2)
  if [[ -z "$r" || -z "$a" ]]; then
    no "events line missing received or acked"
  elif [[ "$r" -eq "$a" ]]; then
    ok "received=$r acked=$a"
  else
    no "received=$r but acked=$a — unacknowledged events block CQ destruction"
  fi
}

test_race_guard_reported() {                                          # R8
  hdr "R8: race_polls_hit is reported and the notes record what it establishes"
  local out; out=$(run_pair "-n 2000 --reap event" "--reap event --pipeline 8 -n 2000" 120)
  if ! grep -qE 'race_polls_hit=[0-9]+' <<<"$out"; then
    no "no race_polls_hit counter on the events line"
    return
  fi
  local n; n=$(grep -oE 'race_polls_hit=[0-9]+' <<<"$out" | cut -d= -f2)
  if [[ ! -s docs/efficiency-notes.md ]]; then
    no "counter reported ($n) but docs/efficiency-notes.md is missing"
  elif grep -qiE 'race' docs/efficiency-notes.md; then
    ok "race_polls_hit=$n, recorded in the notes"
  else
    no "counter reported ($n) but the notes do not discuss it"
  fi
}

test_broken_arming_recorded() {                                       # R8
  hdr "R8: --broken-arming is implemented and its outcome recorded"
  local out rc
  out=$(run_pair "-n 20000 --reap event" "--reap event --pipeline 32 --broken-arming -n 20000" 100)
  rc=$(exit_of "$out")
  if [[ ! -s docs/efficiency-notes.md ]]; then
    no "docs/efficiency-notes.md missing"
    return
  fi
  if ! grep -qiE 'broken.arming|unguarded' docs/efficiency-notes.md; then
    no "the notes do not record the --broken-arming outcome"
    return
  fi
  case "$rc" in
    124|8) ok "unguarded variant stalled (exit $rc) — the race is real here" ;;
    0)     ok "unguarded variant completed; outcome recorded honestly in the notes" ;;
    *)     ok "unguarded variant exited $rc; outcome recorded" ;;
  esac
}

test_option_matrix() {                                                # R10
  hdr "R10: every cell of the option matrix completes with zero mismatches"
  local bad="" inl sig pipe reap
  for inl in "" "--inline"; do
    for sig in 1 16; do
      for pipe in 1 8; do
        for reap in poll event; do
          local out
          out=$(run_pair "-n 200 -s 64 --reap $reap" \
                         "$inl --signal-every $sig --pipeline $pipe --reap $reap -n 200 -s 64" 60)
          grep -qE '^result:.*mismatches=0' <<<"$out" \
            || bad="$bad [${inl:-noinline},sig=$sig,pipe=$pipe,$reap]"
        done
      done
    done
  done
  [[ -z "$bad" ]] && ok "all 16 cells verified" || no "failing cells:$bad"
}

# ══ Regressions ═══════════════════════════════════════════════════════
run_regression() {
  local n="$1"; shift
  local s="scripts/trd-0${n}-tests.sh"
  [[ -x "$s" ]] || { echo "MISSING"; return; }
  if "$s" "$@" >"$TMPDIR/reg-${n}.log" 2>&1; then echo "PASS"; else echo "FAIL"; fi
}

test_trd03_regression() {                                             # R10
  hdr "R10: the TRD-03 suite passes with the defaults"
  local r; r=$(run_regression 3 --dev "$DEV" --gid "$GID" --peer "$PEER" ${SSH_PEER:+--ssh "$SSH_PEER"})
  case "$r" in
    PASS)    ok "defaults preserve TRD-06 behaviour" ;;
    MISSING) sk "scripts/trd-03-tests.sh not present" ;;
    *)       no "TRD-03 failed — a default changed; options must be opt-in"
             grep -m3 'FAIL' "$TMPDIR/reg-3.log" | sed 's/^/        /' || true ;;
  esac
}

test_trd05_regression() {                                             # R10
  hdr "R10: the TRD-05 suite passes with the defaults"
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
  for spec in "poll:-n 100:-n 100" \
              "event:-n 100 --reap event:--reap event -n 100" \
              "timeout:-n 5:--reap event -n 100000" ; do
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
    timeout 180 valgrind --leak-check=full --show-leak-kinds=definite \
      "$BIN" -d "$DEV" -t "$port" $cargs "$PEER" >/dev/null 2>"$log" || true
    lost=$(grep -oE 'definitely lost: [0-9,]+ bytes' "$log" | grep -oE '[0-9,]+' | tr -d ',' || echo "")
    [[ -z "$lost" ]] && { no "could not parse valgrind output for ${name} — see $log"; return; }
    [[ "$lost" -gt "$worst" ]] && worst="$lost"
  done
  [[ "$worst" -eq 0 ]] && ok "0 bytes definitely lost on poll, event, and timeout paths" \
                       || no "${worst} bytes definitely lost — see $TMPDIR/vg-*.log"
}

# ══ Run ═══════════════════════════════════════════════════════════════
test_builds
if [[ "$BUILD_OK" -ne 1 ]]; then
  echo -e "\n${R}Gate closed.${N} Nothing else can run until the build succeeds."
  exit 1
fi

if [[ -z "$DEV" ]] && command -v ibv_devices >/dev/null 2>&1; then
  DEV=$(ibv_devices 2>/dev/null | awk 'NR>2 && NF {print $1; exit}' || true)
fi

if [[ -z "$DEV" ]]; then
  hdr "R1-R9: device-dependent configuration checks"
  sk "no RDMA device found"
  sk "  (test_reports_config)"
  sk "  (test_inline_limit_reported)"
  sk "  (test_signal_period_validated)"
  sk "  (test_pipeline_clamped)"
  sk "  (test_moderation_graceful)"
else
  test_reports_config
  test_inline_limit_reported
  test_signal_period_validated
  test_pipeline_clamped
  test_moderation_graceful
fi

if [[ -z "$PEER" || -z "$DEV" ]]; then
  hdr "R2-R10: two-node checks"
  sk "requires --peer and a device"
  sk "  (test_inline_correct)"
  sk "  (test_signal_every_reduces)"
  sk "  (test_buffer_reuse_correct)"
  sk "  (test_pipeline_correct)"
  sk "  (test_event_mode_completes)"
  sk "  (test_empty_events_tolerated)"
  sk "  (test_events_all_acked)"
  sk "  (test_race_guard_reported)"
  sk "  (test_broken_arming_recorded)"
  sk "  (test_option_matrix)"
  sk "  (test_trd03_regression)"
  sk "  (test_trd05_regression)"
  sk "  (test_no_leaks)"
else
  echo -e "\n${B}Device ${DEV}, peer ${PEER}${N}"
  test_inline_correct
  test_signal_every_reduces
  test_buffer_reuse_correct
  test_pipeline_correct
  test_event_mode_completes
  test_empty_events_tolerated
  test_events_all_acked
  test_race_guard_reported
  test_broken_arming_recorded
  test_option_matrix
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
echo -e "\n${G}Gate open.${N} Finish docs/efficiency-notes.md, answer the comprehension"
echo -e "questions, then begin TRD-08."
