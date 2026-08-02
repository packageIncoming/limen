#!/usr/bin/env bash
# trd-08-tests.sh — Limen RDMA Transport Project
# TRD-08: Benchmark Harness  (final rung)
#
#   ./scripts/trd-08-tests.sh                                            build + docs
#   ./scripts/trd-08-tests.sh --peer 10.0.0.1                            full gate
#   ./scripts/trd-08-tests.sh --peer 10.0.0.1 --ssh you@10.0.0.1         automatic
#
# Exit 0 only when every test passes and none was skipped.

set -uo pipefail

BIN="./build/limen_bench"
DEV=""; PEER=""; SSH_PEER=""
TCP_BASE=19000
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
      "pkill -f 'limen_bench|ib_send_lat'" >/dev/null 2>&1 || true
  rm -rf "$TMPDIR"
}
trap cleanup EXIT INT TERM
next_port() { PORT_SEQ=$((PORT_SEQ + 1)); echo $((TCP_BASE + PORT_SEQ)); }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dev)  DEV="${2:-}";  shift 2 ;;
    --peer) PEER="${2:-}"; shift 2 ;;
    --ssh)  SSH_PEER="${2:-}"; shift 2 ;;
    -h|--help) echo "usage: $0 [--dev NAME] [--peer IP] [--ssh user@host]"; exit 1 ;;
    *) echo "unknown argument: $1"; exit 1 ;;
  esac
done

echo -e "${B}=== Limen TRD-08: Benchmark Harness ===${N}"

run_bench() {   # "<server args>" "<client args>" <timeout>
  local sargs="$1" cargs="$2" tmo="${3:-120}" port
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
      "pkill -f 'limen_bench.*-t $port'" >/dev/null 2>&1 || true
  echo "__EXIT__${rc}"
}
exit_of() { grep -oE '__EXIT__[0-9]+' <<<"$1" | grep -oE '[0-9]+$'; }
REPORT="docs/benchmark-report.md"

# ══ Build ═════════════════════════════════════════════════════════════
BUILD_OK=0
test_builds() {                                                       # R1
  hdr "R1: limen_bench builds clean under -Wall -Wextra"
  local log="$TMPDIR/build.log"
  if ! cmake -S . -B build >"$log" 2>&1;  then no "cmake configure failed — see $log"; return; fi
  if ! cmake --build build >>"$log" 2>&1; then no "compilation failed — see $log";     return; fi
  [[ -x "$BIN" ]] || { no "$BIN missing"; return; }
  local w; w=$(grep -ci 'warning:' "$log" || true)
  [[ "$w" -gt 0 ]] && { no "emitted $w warning(s) — see $log"; return; }
  BUILD_OK=1; ok "built with no warnings"
}

# ══ Local, no peer ════════════════════════════════════════════════════
test_clock_floor_reported() {                                         # R1
  hdr "R1: --clock-floor reports a per-call cost"
  local out rc
  out=$("$BIN" -d "$DEV" --clock-floor 2>&1); rc=$?
  [[ "$rc" -ne 0 ]] && { no "exited $rc"; return; }
  local ns
  ns=$(grep -oE 'median=[0-9]+ ?ns' <<<"$out" | grep -oE '[0-9]+' | head -1)
  if [[ -z "$ns" ]]; then
    no "no median clock cost reported"
  elif [[ "$ns" -gt 0 && "$ns" -lt 100000 ]]; then
    ok "clock floor ${ns} ns per call"
  else
    no "implausible clock floor: ${ns} ns"
  fi
}

# ══ Report / documentation ════════════════════════════════════════════
test_report_sections() {                                              # R10
  hdr "R10: the report contains all eight required sections"
  [[ -s "$REPORT" ]] || { no "$REPORT missing or empty"; return; }
  local missing="" s
  for s in "methodolog" "condition" "validat" "noise" "result" "tcp" "finding" "does not show"; do
    grep -qi "$s" "$REPORT" || missing="$missing ${s}"
  done
  [[ -z "$missing" ]] && ok "all eight sections present" || no "missing:$missing"
}

test_report_states_noise_floor() {                                    # R10
  hdr "R10: the report states the noise floor before any comparison"
  [[ -s "$REPORT" ]] || { no "$REPORT missing"; return; }
  local nl cl
  nl=$(grep -niE 'noise floor|run-to-run|spread' "$REPORT" | head -1 | cut -d: -f1)
  cl=$(grep -niE 'SIGNIFICANT|within noise|delta|improvement' "$REPORT" | head -1 | cut -d: -f1)
  if [[ -z "$nl" ]]; then
    no "no noise floor stated"
  elif [[ -z "$cl" ]]; then
    no "no comparisons found — the option sweep results are missing"
  elif [[ "$nl" -lt "$cl" ]]; then
    ok "noise floor stated at line $nl, first comparison at line $cl"
  else
    no "first comparison (line $cl) precedes the noise floor (line $nl)"
  fi
}

test_report_states_limits() {                                         # R10
  hdr "R10: the report contains a 'what this does not show' section"
  [[ -s "$REPORT" ]] || { no "$REPORT missing"; return; }
  local sec
  sec=$(grep -iA10 'does not show\|limitations' "$REPORT" || true)
  local w; w=$(wc -w <<<"$sec")
  if [[ "$w" -ge 40 ]]; then
    ok "limits section present (${w} words)"
  else
    no "limits section missing or too thin (${w} words)"
  fi
}

test_reference_convention() {                                         # R8
  hdr "R8: the reference's reporting convention is determined and recorded"
  [[ -s "$REPORT" ]] || { no "$REPORT missing"; return; }
  if grep -qiE 'round.trip|one.way|half' "$REPORT" \
     && grep -qiE 'ratio|t_typical' "$REPORT"; then
    ok "convention recorded with its supporting comparison"
  else
    no "the report does not establish whether the reference reports full or half RTT"
  fi
}

# ══ Runtime ═══════════════════════════════════════════════════════════
LAT=""
capture_lat() {
  [[ -n "$LAT" ]] && return 0
  LAT=$(run_bench "--mode latency -s 64 -n 20000" "--mode latency -s 64 -n 20000 --runs 3")
}

test_warmup_applied() {                                               # R2
  hdr "R2: warm-up iterations are excluded and the count reported"
  capture_lat
  if grep -qiE 'warmup[=: ]+[0-9]+' <<<"$LAT"; then
    ok "$(grep -oiE 'warmup[=: ]+[0-9]+' <<<"$LAT" | head -1)"
  else
    no "no warmup count in the output"
  fi
}

test_distribution_reported() {                                        # R3
  hdr "R3: all eight distribution statistics are reported"
  capture_lat
  local missing="" s
  for s in min p50 p90 p99 p99.9 max mean stdev; do
    grep -qiE "(^|[^a-z])${s//./\\.}" <<<"$LAT" || missing="$missing $s"
  done
  [[ -z "$missing" ]] && ok "full distribution reported" || no "missing:$missing"
}

test_percentiles_consistent() {                                       # R3
  hdr "R3: percentiles are ordered and internally consistent"
  capture_lat
  local p50 p99
  p50=$(grep -oiE 'p50[=: ]+[0-9.]+' <<<"$LAT" | grep -oE '[0-9.]+' | head -1)
  p99=$(grep -oiE 'p99[=: ]+[0-9.]+' <<<"$LAT" | grep -oE '[0-9.]+' | head -1)
  if [[ -z "$p50" || -z "$p99" ]]; then
    no "could not parse p50/p99"
  elif awk "BEGIN{exit !($p99 >= $p50)}"; then
    ok "p99 ($p99) >= p50 ($p50)"
  else
    no "p99 ($p99) < p50 ($p50) — percentile indexing is wrong"
  fi
}

test_json_samples_emitted() {                                         # R3
  hdr "R3: --json emits raw samples"
  local jf="$TMPDIR/out.json"
  run_bench "--mode latency -s 64 -n 5000" \
            "--mode latency -s 64 -n 5000 --json $jf" >/dev/null
  if [[ ! -s "$jf" ]]; then
    no "no JSON written to $jf"
  elif grep -qE 'samples' "$jf" && grep -qE '[0-9]+,[0-9]+' "$jf"; then
    ok "raw samples present ($(wc -c <"$jf") bytes)"
  else
    no "JSON written but contains no raw sample array"
  fi
}

test_modes_labelled() {                                               # R4
  hdr "R4: latency and response figures are labelled distinctly"
  capture_lat
  local resp
  resp=$(run_bench "--mode response -s 64 -n 20000" \
                   "--mode response -s 64 -n 20000 --rate 5000")
  if grep -qiE 'service.time|closed.loop' <<<"$LAT" \
     && grep -qiE 'response.time|open.loop' <<<"$resp"; then
    ok "the two quantities are named distinctly"
  else
    no "modes not labelled — a figure called 'latency' must say which quantity"
  fi
}

test_response_mode_schedule() {                                       # R4
  hdr "R4: response mode measures from intended time, not issue time"
  local out
  out=$(run_bench "--mode response -s 64 -n 20000" \
                  "--mode response -s 64 -n 20000 --rate 1000000")   # deliberately unachievable
  if grep -qiE 'behind[=: ]+[0-9]+' <<<"$out"; then
    local b; b=$(grep -oiE 'behind[=: ]+[0-9]+' <<<"$out" | grep -oE '[0-9]+' | head -1)
    if [[ "$b" -gt 0 ]]; then
      ok "reported behind=$b at an unachievable rate — schedule is being tracked"
    else
      no "behind=0 at 1M ops/s — the harness is not measuring against a schedule"
    fi
  else
    no "no 'behind' counter — cannot tell whether the schedule is enforced"
  fi
}

test_bandwidth_units() {                                              # R5
  hdr "R5: bandwidth is reported in both units with a message rate"
  local out
  out=$(run_bench "--mode bandwidth -s 65536 -n 20000" \
                  "--mode bandwidth -s 65536 -n 20000")
  local problems=""
  grep -qiE 'MiB/s|MB/s'   <<<"$out" || problems="$problems MiB/s"
  grep -qiE 'Gbit/s|Gb/s'  <<<"$out" || problems="$problems Gbit/s"
  grep -qiE 'msg/s|msg_rate|ops/s' <<<"$out" || problems="$problems msg-rate"
  [[ -z "$problems" ]] && ok "$(grep -m1 -iE 'bandwidth:' <<<"$out")" \
                       || no "missing:$problems"
}

test_size_sweep() {                                                   # R6
  hdr "R6: the size sweep covers at least 64 B to 1 MB"
  local out
  out=$(run_bench "--mode sweep" "--mode sweep" 600)
  local have64 have1m
  grep -qE '(^|[^0-9])64([^0-9]|$)'      <<<"$out" && have64=1
  grep -qE '1048576|1 ?MB|1 ?MiB'        <<<"$out" && have1m=1
  if [[ -n "${have64:-}" && -n "${have1m:-}" ]]; then
    ok "sweep spans 64 B to 1 MB"
  else
    no "sweep does not span the required range"
  fi
}

test_option_sweep() {                                                 # R6
  hdr "R6: the option sweep covers all five TRD-07 switches"
  [[ -s "$REPORT" ]] || { no "$REPORT missing"; return; }
  local missing="" s
  for s in inline signal pipeline reap "write\|one-sided"; do
    grep -qiE "$s" "$REPORT" || missing="$missing ${s%%\\*}"
  done
  [[ -z "$missing" ]] && ok "all five switches appear in the report" || no "missing:$missing"
}

test_variance_reported() {                                            # R7
  hdr "R7: --runs reports a spread across runs"
  capture_lat
  if grep -qiE 'spread|variance|run-to-run' <<<"$LAT"; then
    ok "$(grep -m1 -iE 'spread|variance' <<<"$LAT")"
  else
    no "no across-run spread reported — R6's comparisons have no threshold"
  fi
}

test_validated_against_perftest() {                                   # R8
  hdr "R8: validation against ib_send_lat is within the declared tolerance"
  command -v ib_send_lat >/dev/null 2>&1 || { sk "ib_send_lat not installed"; return; }
  # reference
  if [[ -n "$SSH_PEER" ]]; then
    ssh -o BatchMode=yes "$SSH_PEER" \
      "nohup ib_send_lat -d $DEV -s 64 -n 10000 >/dev/null 2>&1 & disown" >/dev/null 2>&1 || true
    sleep 2
  else
    echo "      on the peer:  ib_send_lat -d $DEV -s 64 -n 10000"
    read -rp "      press enter once it is running... "
  fi
  local ref
  ref=$(timeout 120 ib_send_lat -d "$DEV" -s 64 -n 10000 "$PEER" 2>/dev/null || true)
  local rt
  rt=$(awk '/^[[:space:]]*64[[:space:]]/ {print $5; exit}' <<<"$ref")   # t_typical
  if [[ -z "$rt" ]]; then
    sk "could not parse ib_send_lat t_typical"
    return
  fi
  local mine
  mine=$(run_bench "--mode latency -s 64 -n 10000" "--mode latency -s 64 -n 10000")
  local mt
  mt=$(grep -oiE 'p50[=: ]+[0-9.]+' <<<"$mine" | grep -oE '[0-9.]+' | head -1)
  [[ -z "$mt" ]] && { no "could not parse our own p50"; return; }
  local within
  within=$(awk -v a="$rt" -v b="$mt" 'BEGIN{
      if (a<=0) {print "na"; exit}
      r=b/a; if (r<1) r=1/r;
      print (r <= 1.25) ? "yes" : ((r>1.8 && r<2.2) ? "factor2" : "no")}')
  case "$within" in
    yes)     ok "reference t_typical=${rt} us, ours p50=${mt} us — within 25%" ;;
    factor2) no "ratio near 2.0 — round-trip vs one-way convention mismatch (see R8)" ;;
    *)       no "reference=${rt} us vs ours=${mt} us — outside tolerance; suspect the harness" ;;
  esac
}

test_conditions_captured() {                                          # R9
  hdr "R9: every result carries the full conditions block"
  capture_lat
  local missing="" s
  for s in kernel device mtu clock warmup iterations; do
    grep -qi "$s" <<<"$LAT" || missing="$missing $s"
  done
  [[ -z "$missing" ]] && ok "conditions block complete" || no "missing:$missing"
}

test_no_leaks() {                                                     # R10
  hdr "R10: valgrind reports zero bytes definitely lost"
  command -v valgrind >/dev/null 2>&1 || { sk "valgrind not installed"; return; }
  local port; port=$(next_port)
  if [[ -n "$SSH_PEER" ]]; then
    ssh -o BatchMode=yes "$SSH_PEER" \
      "cd '$(pwd)' && nohup $BIN -d $DEV -t $port --mode latency -s 64 -n 2000 >/dev/null 2>&1 & disown" \
      >/dev/null 2>&1 || true
    sleep 2
  else
    echo "      on the peer:  $BIN -d $DEV -t $port --mode latency -s 64 -n 2000"
    read -rp "      press enter once it is running... "
  fi
  local log="$TMPDIR/vg.log"
  timeout 200 valgrind --leak-check=full --show-leak-kinds=definite \
    "$BIN" -d "$DEV" -t "$port" --mode latency -s 64 -n 2000 "$PEER" >/dev/null 2>"$log" || true
  local lost
  lost=$(grep -oE 'definitely lost: [0-9,]+ bytes' "$log" | grep -oE '[0-9,]+' | tr -d ',' || echo "")
  if [[ -z "$lost" ]]; then no "could not parse valgrind output — see $log"
  elif [[ "$lost" -eq 0 ]]; then ok "0 bytes definitely lost"
  else no "${lost} bytes definitely lost — see $log"; fi
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

test_report_sections
test_report_states_noise_floor
test_report_states_limits
test_reference_convention
test_option_sweep

if [[ -z "$DEV" ]]; then
  hdr "R1: device-dependent local checks"
  sk "no RDMA device found"
  sk "  (test_clock_floor_reported)"
else
  test_clock_floor_reported
fi

if [[ -z "$PEER" || -z "$DEV" ]]; then
  hdr "R2-R10: two-node checks"
  sk "requires --peer and a device"
  sk "  (test_warmup_applied)"
  sk "  (test_distribution_reported)"
  sk "  (test_percentiles_consistent)"
  sk "  (test_json_samples_emitted)"
  sk "  (test_modes_labelled)"
  sk "  (test_response_mode_schedule)"
  sk "  (test_bandwidth_units)"
  sk "  (test_size_sweep)"
  sk "  (test_variance_reported)"
  sk "  (test_validated_against_perftest)"
  sk "  (test_conditions_captured)"
  sk "  (test_no_leaks)"
else
  echo -e "\n${B}Device ${DEV}, peer ${PEER}${N}"
  test_warmup_applied
  test_distribution_reported
  test_percentiles_consistent
  test_json_samples_emitted
  test_modes_labelled
  test_response_mode_schedule
  test_bandwidth_units
  test_size_sweep
  test_variance_reported
  test_validated_against_perftest
  test_conditions_captured
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
echo -e "\n${G}Gate open — and the ladder is complete.${N}"
echo -e "Answer the comprehension questions. docs/benchmark-report.md is the artifact."
