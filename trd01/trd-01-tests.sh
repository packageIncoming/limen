#!/usr/bin/env bash
# trd-01-tests.sh — Limen RDMA Transport Project
# TRD-01: Context, Protection Domain, Memory Registration
#
# Run from the repository root:
#
#   ./scripts/trd-01-tests.sh                  auto-detect the device
#   ./scripts/trd-01-tests.sh --dev rxe0       name it explicitly
#
# Tests that need a live RDMA device SKIP rather than FAIL when none is present,
# so the build and argument-handling checks remain useful anywhere.
#
# Exit 0 only when every test passes and none was skipped.

set -uo pipefail

BIN="./build/limen_devinfo"
DEV=""
PASS=0; FAIL=0; SKIP=0

R=$'\033[0;31m'; G=$'\033[0;32m'; Y=$'\033[0;33m'; B=$'\033[1m'; N=$'\033[0m'

# CRITICAL: VAR=$((VAR + 1)). A bare ((VAR++)) returns the pre-increment value
# as its exit status, which is 1 when the counter is 0.
ok()  { echo -e "  ${G}PASS${N}  $1"; PASS=$((PASS + 1)); }
no()  { echo -e "  ${R}FAIL${N}  $1"; FAIL=$((FAIL + 1)); }
sk()  { echo -e "  ${Y}SKIP${N}  $1"; SKIP=$((SKIP + 1)); }
hdr() { echo -e "\n${B}$1${N}"; }

TMPDIR="$(mktemp -d)"
cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT INT TERM

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dev) DEV="${2:-}"; shift 2 ;;
    -h|--help) echo "usage: $0 [--dev NAME]"; exit 1 ;;
    *) echo "unknown argument: $1"; exit 1 ;;
  esac
done

echo -e "${B}=== Limen TRD-01: Context, PD, Memory Registration ===${N}"

# ── Build ─────────────────────────────────────────────────────────────
BUILD_OK=0
test_builds() {                                                    # R1
  hdr "R1: limen_devinfo builds clean under -Wall -Wextra"
  local log="$TMPDIR/build.log"
  if ! cmake -S . -B build >"$log" 2>&1; then
    no "cmake configure failed — see $log"
    return
  fi
  if ! cmake --build build >>"$log" 2>&1; then
    no "compilation failed — see $log"
    return
  fi
  if [[ ! -x "$BIN" ]]; then
    no "built, but $BIN is missing or not executable"
    return
  fi
  local warns
  warns=$(grep -ci 'warning:' "$log" || true)
  if [[ "$warns" -gt 0 ]]; then
    no "built, but emitted $warns warning(s) — see $log"
    return
  fi
  BUILD_OK=1
  ok "built with no warnings"
}

# ── Argument handling (no device needed) ──────────────────────────────
test_usage_exits_zero() {                                          # R1
  hdr "R1: -h prints usage to stdout and exits 0"
  local out rc
  out=$("$BIN" -h 2>/dev/null); rc=$?
  if [[ "$rc" -ne 0 ]]; then
    no "expected exit 0, got $rc"
  elif [[ -z "$out" ]]; then
    no "exited 0 but printed nothing to stdout"
  else
    ok "usage on stdout, exit 0"
  fi
}

test_bad_flag_exits_one() {                                        # R1
  hdr "R1: an unknown argument exits 1"
  local rc
  "$BIN" --not-a-real-flag >/dev/null 2>&1; rc=$?
  if [[ "$rc" -eq 1 ]]; then
    ok "exit 1"
  else
    no "expected exit 1, got $rc"
  fi
}

test_lists_devices() {                                             # R2
  hdr "R2: no arguments enumerates devices"
  local out rc
  out=$("$BIN" 2>&1); rc=$?
  if [[ "$rc" -eq 2 ]] && [[ -z "$DEV" ]]; then
    ok "exit 2 with no devices present — correct per R2"
    return
  fi
  if [[ "$rc" -ne 0 ]]; then
    no "expected exit 0 with a device present, got $rc"
    return
  fi
  if grep -q '^device: ' <<<"$out" && grep -qE '^devices: [0-9]+' <<<"$out"; then
    ok "device: and devices: lines present"
  else
    no "missing device:/devices: lines — got: $(head -2 <<<"$out")"
  fi
}

test_unknown_device_exits_two() {                                  # R3
  hdr "R3: an unknown device name exits 2 and lists what is available"
  local out rc
  out=$("$BIN" -d definitely_not_a_device 2>&1); rc=$?
  if [[ "$rc" -ne 2 ]]; then
    no "expected exit 2, got $rc"
    return
  fi
  if [[ -z "$DEV" ]]; then
    ok "exit 2 (no devices present to list)"
  elif grep -q "$DEV" <<<"$out"; then
    ok "exit 2 and the message names $DEV"
  else
    no "exit 2 but the message does not list available devices"
  fi
}

# ── Device-dependent ──────────────────────────────────────────────────
REPORT=""
capture_report() {
  [[ -n "$REPORT" ]] && return 0
  REPORT=$("$BIN" -d "$DEV" 2>&1 || true)
}

test_reports_device_attrs() {                                      # R4
  hdr "R4: all eleven device attribute labels are reported"
  capture_report
  local missing="" f
  for f in guid fw_ver phys_port_cnt max_qp max_qp_wr max_cq max_cqe \
           max_mr max_mr_size max_sge max_qp_rd_atom; do
    grep -qE "^[[:space:]]*${f}:" <<<"$REPORT" || missing="$missing $f"
  done
  if [[ -z "$missing" ]]; then
    ok "all eleven present"
  else
    no "missing:$missing"
  fi
}

test_reports_port_attrs() {                                        # R5
  hdr "R5: port block reports symbolic state, link layer, and MTU in bytes"
  capture_report
  local problems=""
  grep -qE '^port [0-9]+:' <<<"$REPORT" || problems="$problems no-port-header"
  grep -qE '^[[:space:]]*state:[[:space:]]*PORT_[A-Z]+' <<<"$REPORT" \
    || problems="$problems state-not-symbolic"
  grep -qE '^[[:space:]]*link_layer:[[:space:]]*(Ethernet|InfiniBand|Unspecified)' <<<"$REPORT" \
    || problems="$problems link_layer-not-symbolic"
  # MTU must be a byte count (>= 256), not an enum ordinal (1..5)
  local mtu
  mtu=$(grep -oE '^[[:space:]]*active_mtu:[[:space:]]*[0-9]+' <<<"$REPORT" | grep -oE '[0-9]+$' || true)
  if [[ -z "$mtu" ]]; then
    problems="$problems no-active_mtu"
  elif [[ "$mtu" -lt 256 ]]; then
    problems="$problems active_mtu=${mtu}-looks-like-an-enum-ordinal"
  fi
  grep -qE '^[[:space:]]*max_msg_sz:' <<<"$REPORT" || problems="$problems no-max_msg_sz"
  grep -qE '^[[:space:]]*gid_tbl_len:' <<<"$REPORT" || problems="$problems no-gid_tbl_len"

  if [[ -z "$problems" ]]; then
    ok "port block complete and symbolic (active_mtu=${mtu})"
  else
    no "problems:$problems"
  fi
}

test_bad_port_exits_one() {                                        # R5
  hdr "R5: a port number above phys_port_cnt exits 1"
  local rc
  "$BIN" -d "$DEV" -p 99 >/dev/null 2>&1; rc=$?
  if [[ "$rc" -eq 1 ]]; then
    ok "exit 1"
  else
    no "expected exit 1, got $rc"
  fi
}

test_allocates_pd() {                                              # R6
  hdr "R6: protection domain is allocated"
  capture_report
  if grep -qE '^pd:[[:space:]]*allocated' <<<"$REPORT"; then
    ok "pd: allocated"
  else
    no "no 'pd: allocated' line in report output"
  fi
}

MROUT=""
capture_mr() {
  [[ -n "$MROUT" ]] && return 0
  MROUT=$("$BIN" -d "$DEV" -s 1048576 2>&1 || true)
}

test_registers_mr() {                                              # R7
  hdr "R7: -s registers a region and reports address, length, and flags"
  capture_mr
  local line
  line=$(grep -m1 '^mr: ' <<<"$MROUT" || true)
  if [[ -z "$line" ]]; then
    no "no 'mr:' line — output was: $(tail -3 <<<"$MROUT")"
    return
  fi
  local problems=""
  grep -qE 'addr=0x[0-9a-fA-F]+' <<<"$line" || problems="$problems addr"
  grep -qE 'length=1048576' <<<"$line"      || problems="$problems length"
  grep -q  'LOCAL_WRITE'      <<<"$line"    || problems="$problems LOCAL_WRITE"
  grep -q  'REMOTE_READ'      <<<"$line"    || problems="$problems REMOTE_READ"
  grep -q  'REMOTE_WRITE'     <<<"$line"    || problems="$problems REMOTE_WRITE"
  if [[ -z "$problems" ]]; then
    ok "mr line complete"
  else
    no "mr line missing:$problems"
  fi
}

test_reports_both_keys() {                                         # R7
  hdr "R7: mr line carries lkey and rkey in hexadecimal"
  capture_mr
  local line
  line=$(grep -m1 '^mr: ' <<<"$MROUT" || true)
  if grep -qE 'lkey=0x[0-9a-fA-F]+' <<<"$line" \
     && grep -qE 'rkey=0x[0-9a-fA-F]+' <<<"$line"; then
    ok "both keys present"
  else
    no "lkey and/or rkey missing or not hexadecimal"
  fi
}

test_access_check_rejected() {                                     # R8
  hdr "R8: REMOTE_WRITE without LOCAL_WRITE is rejected"
  local out rc
  out=$("$BIN" -d "$DEV" --check-access 2>&1); rc=$?
  if [[ "$rc" -ne 0 ]]; then
    no "exited $rc — an unexpected registration success is reported as 3 (see R8)"
    return
  fi
  if grep -qE '^access-check:.*rejected:.*EINVAL' <<<"$out"; then
    ok "rejected with EINVAL, reported symbolically"
  else
    no "expected an access-check line naming EINVAL — got: $(grep -m1 'access-check' <<<"$out" || echo '<none>')"
  fi
}

test_oversized_registration_graceful() {                           # R9
  hdr "R9: an oversized registration fails gracefully"
  local lim
  lim=$(ulimit -l 2>/dev/null || echo unlimited)
  if [[ "$lim" == "unlimited" ]]; then
    sk "RLIMIT_MEMLOCK is unlimited on this host — cannot provoke the failure"
    return
  fi
  # ulimit -l reports kilobytes; request roughly four times the limit
  local bytes=$(( lim * 1024 * 4 ))
  local out rc
  out=$("$BIN" -d "$DEV" -s "$bytes" 2>&1); rc=$?
  if [[ "$rc" -eq 0 ]]; then
    sk "registration of ${bytes} bytes succeeded — the limit did not bind here"
    return
  fi
  if [[ "$rc" -ne 3 ]]; then
    no "expected exit 3 on registration failure, got $rc"
    return
  fi
  local problems=""
  grep -qE 'registration failed:.*(ENOMEM|EINVAL|EPERM)' <<<"$out" || problems="$problems symbolic-errno"
  grep -qE 'requested:[[:space:]]*[0-9]+'                <<<"$out" || problems="$problems requested-size"
  grep -qiE 'RLIMIT_MEMLOCK'                             <<<"$out" || problems="$problems memlock-limit"
  grep -qiE 'hint:'                                      <<<"$out" || problems="$problems hint"
  if [[ -z "$problems" ]]; then
    ok "failed gracefully with a complete diagnostic"
  else
    no "diagnostic missing:$problems"
  fi
}

test_teardown_reports_ok() {                                       # R10
  hdr "R10: teardown line reports every stage"
  capture_mr
  local line
  line=$(grep -m1 '^teardown: ' <<<"$MROUT" || true)
  if [[ -z "$line" ]]; then
    no "no 'teardown:' line"
    return
  fi
  if grep -qE 'mr=ok' <<<"$line" && grep -qE 'pd=ok' <<<"$line" \
     && grep -qE 'context=ok' <<<"$line"; then
    ok "mr=ok pd=ok context=ok"
  else
    no "teardown reported a failure or is incomplete: $line"
  fi
}

test_no_leaks() {                                                  # R10
  hdr "R10: valgrind reports zero bytes definitely lost"
  if ! command -v valgrind >/dev/null 2>&1; then
    sk "valgrind not installed"
    return
  fi
  local log="$TMPDIR/valgrind.log"
  valgrind --leak-check=full --show-leak-kinds=definite \
           "$BIN" -d "$DEV" -s 1048576 >/dev/null 2>"$log" || true
  local lost
  lost=$(grep -oE 'definitely lost: [0-9,]+ bytes' "$log" | grep -oE '[0-9,]+' | tr -d ',' || echo "")
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

# Auto-detect a device if one was not named.
if [[ -z "$DEV" ]] && command -v ibv_devices >/dev/null 2>&1; then
  DEV=$(ibv_devices 2>/dev/null | awk 'NR>2 && NF {print $1; exit}' || true)
fi

test_usage_exits_zero
test_bad_flag_exits_one
test_lists_devices
test_unknown_device_exits_two

if [[ -z "$DEV" ]]; then
  hdr "R4–R10: device-dependent checks"
  sk "no RDMA device found — revisit TRD-00 R3 and R4, or pass --dev"
  sk "  (test_reports_device_attrs)"
  sk "  (test_reports_port_attrs)"
  sk "  (test_bad_port_exits_one)"
  sk "  (test_allocates_pd)"
  sk "  (test_registers_mr)"
  sk "  (test_reports_both_keys)"
  sk "  (test_access_check_rejected)"
  sk "  (test_oversized_registration_graceful)"
  sk "  (test_teardown_reports_ok)"
  sk "  (test_no_leaks)"
else
  echo -e "\n${B}Using device: ${DEV}${N}"
  test_reports_device_attrs
  test_reports_port_attrs
  test_bad_port_exits_one
  test_allocates_pd
  test_registers_mr
  test_reports_both_keys
  test_access_check_rejected
  test_oversized_registration_graceful
  test_teardown_reports_ok
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

echo -e "\n${G}Gate open.${N} Record the report output in docs/device-attrs.md, answer the"
echo -e "comprehension questions, then begin TRD-02."
