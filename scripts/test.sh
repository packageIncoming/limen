#!/usr/bin/env bash
# scripts/test.sh — Limen acceptance suite
#
# Run from the repository root.
#
#   ./scripts/test.sh                                   build + static + local
#   ./scripts/test.sh --dev rocep1s0f0 --gid 3 \
#                     --peer 192.168.100.1 --peer-dev rocep4s0f0 \
#                     --peer-cmd "sudo limen-b"         full suite, one host
#   ./scripts/test.sh --dev mlx5_0 --gid 3 \
#                     --peer 10.0.0.2 --ssh you@10.0.0.2   full suite, two hosts
#
#   --group build,static,local,transport,onesided,efficiency,bench,leaks
#   --only  <substring>     run only matching tests
#   --list                  print test names and exit
#
# Exit 0 only when every test that ran passed and none was skipped.

set -uo pipefail

DEV=""; GID=""; PEER=""; PEER_DEV=""; PEER_CMD=""; SSH_PEER=""
TCP_BASE=18800
SUITES="build,static,local,transport,onesided,efficiency,bench,leaks"
ONLY=""; LIST=0

DEVINFO_BIN="./build/limen_devinfo"
CONNECT_BIN="./build/limen_connect"
PINGPONG_BIN="./build/limen_pingpong"
ONESIDED_BIN="./build/limen_onesided"
BENCH_BIN="./build/limen_bench"

PASS=0; FAIL=0; SKIP=0

R=$'\033[0;31m'; G=$'\033[0;32m'; Y=$'\033[0;33m'; B=$'\033[1m'; N=$'\033[0m'

#  VAR=$((VAR + 1)). A bare ((VAR++)) returns the pre-increment value as its
#  exit status, which is 1 when the counter is 0 and trips set -e.
ok()  { echo -e "  ${G}PASS${N}  $1"; PASS=$((PASS + 1)); }
no()  { echo -e "  ${R}FAIL${N}  $1"; FAIL=$((FAIL + 1)); peer_ctx; }
sk()  { echo -e "  ${Y}SKIP${N}  $1"; SKIP=$((SKIP + 1)); }
hdr() { echo -e "\n${B}$1${N}"; }
sec() { echo -e "\n${B}── $1 ${N}"; }

TMPDIR="$(mktemp -d)"
SRV_LOG=""
PORT_SEQ=0

cleanup() {
  local p
  for p in limen_connect limen_pingpong limen_onesided limen_bench; do
    if [[ -n "$PEER_CMD" ]]; then
      $PEER_CMD pkill -f "$p" >/dev/null 2>&1 || true
      sudo pkill -f "$p"      >/dev/null 2>&1 || true
    elif [[ -n "$SSH_PEER" ]]; then
      ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH_PEER" "pkill -f $p" >/dev/null 2>&1 || true
    fi
  done
  rm -rf "$TMPDIR"
}
trap cleanup EXIT INT TERM
SRV_LOG="$TMPDIR/server.log"

next_port() { PORT_SEQ=$((PORT_SEQ + 1)); echo $((TCP_BASE + PORT_SEQ)); }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dev)       DEV="${2:-}";       shift 2 ;;
    --gid)       GID="${2:-}";       shift 2 ;;
    --peer)      PEER="${2:-}";      shift 2 ;;
    --peer-dev)  PEER_DEV="${2:-}";  shift 2 ;;
    --peer-cmd)  PEER_CMD="${2:-}";  shift 2 ;;
    --ssh)       SSH_PEER="${2:-}";  shift 2 ;;
    --base-port) TCP_BASE="${2:-}";  shift 2 ;;
    --group)     SUITES="${2:-}";    shift 2 ;;
    --only)      ONLY="${2:-}";      shift 2 ;;
    --list)      LIST=1;             shift   ;;
    -h|--help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 1 ;;
    *) echo "unknown argument: $1"; exit 1 ;;
  esac
done

want_group() { [[ ",$SUITES," == *",$1,"* ]]; }
want_test()  { [[ -z "$ONLY" || "$1" == *"$ONLY"* ]]; }

#  run <group> <name> — dispatch guard used by every test
run() {
  local grp="$1" name="$2"
  want_group "$grp" || return 1
  want_test  "$name" || return 1
  if [[ "$LIST" -eq 1 ]]; then echo "  $grp  $name"; return 1; fi
  return 0
}

# ══════════════════════════════════════════════════════════════════════
#  Peer control
# ══════════════════════════════════════════════════════════════════════
#  --peer-cmd  peer is on this host behind a wrapper (network namespace)
#  --ssh       peer is a second machine
#  neither     prompt the operator

start_peer() {
  local bin="$1" port="$2" extra="${3:-}" ready="${4:-role: server}"
  : >"$SRV_LOG"
  if [[ -n "$PEER_CMD" ]]; then
    # shellcheck disable=SC2086
    $PEER_CMD "$bin" -d "$PEER_DEV" ${GID:+-g $GID} -t "$port" $extra >"$SRV_LOG" 2>&1 &
    local i=0
    while [[ $i -lt 80 ]]; do
      grep -q "$ready" "$SRV_LOG" 2>/dev/null && break
      sleep 0.05; i=$((i + 1))
    done
    sleep 0.3
  elif [[ -n "$SSH_PEER" ]]; then
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH_PEER" \
      "cd '$(pwd)' && nohup $bin -d $PEER_DEV ${GID:+-g $GID} -t $port $extra >/tmp/limen-srv.log 2>&1 & disown" \
      >/dev/null 2>&1 || true
    sleep 2
  else
    echo "      on the peer:  $bin -d $PEER_DEV ${GID:+-g $GID} -t $port $extra" >&2
    read -rp "      press enter once it is running... " >&2
  fi
}

kill_peer() {
  local bin="$1" port="$2" base
  base=$(basename "$bin")
  if [[ -n "$PEER_CMD" ]]; then
    $PEER_CMD pkill -f "${base}.*-t $port" >/dev/null 2>&1 || true
    sudo pkill -f "${base}.*-t $port"      >/dev/null 2>&1 || true
  elif [[ -n "$SSH_PEER" ]]; then
    ssh -o BatchMode=yes "$SSH_PEER" "pkill -f '${base}.*-t $port'" >/dev/null 2>&1 || true
  fi
}

server_log() {
  if   [[ -n "$PEER_CMD" ]]; then cat "$SRV_LOG" 2>/dev/null || true
  elif [[ -n "$SSH_PEER" ]]; then ssh -o BatchMode=yes "$SSH_PEER" "cat /tmp/limen-srv.log" 2>/dev/null || true
  fi
}

#  printed after a failure so a peer-side death is visible instead of silent
peer_ctx() {
  [[ -n "$PEER_CMD" ]] || return 0
  [[ -s "$SRV_LOG" ]]  || return 0
  grep -iE 'error|failed|status=[A-Z_]*ERR|timeout|mismatch' "$SRV_LOG" 2>/dev/null \
    | head -3 | sed 's/^/          peer: /' || true
}

#  pair <bin> "<server args>" "<client args>" [timeout]
#  echoes the client's output followed by __EXIT__<rc>
pair() {
  local bin="$1" sargs="$2" cargs="$3" tmo="${4:-90}" port
  port=$(next_port)
  start_peer "$bin" "$port" "$sargs"
  # shellcheck disable=SC2086
  timeout "$tmo" "$bin" -d "$DEV" ${GID:+-g $GID} -t "$port" $cargs "$PEER" 2>&1
  local rc=$?
  [[ -n "$PEER_CMD" ]] && sleep 0.5
  kill_peer "$bin" "$port"
  echo "__EXIT__${rc}"
}
exit_of() { grep -oE '__EXIT__[0-9]+' <<<"$1" | grep -oE '[0-9]+$'; }
body_of() { grep -v '__EXIT__' <<<"$1"; }

num() { grep -oE '[0-9]+' <<<"$1" | head -1; }

# ══════════════════════════════════════════════════════════════════════
#  build
# ══════════════════════════════════════════════════════════════════════

BUILD_OK=0

t_builds_clean() {
  hdr "build: configures and compiles with no warnings"
  local log="$TMPDIR/build.log"
  if ! cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo >"$log" 2>&1; then
    no "cmake configure failed — see $log"; return
  fi
  if ! cmake --build build >>"$log" 2>&1; then
    no "compilation failed — see $log"; return
  fi
  local warns; warns=$(grep -ci 'warning:' "$log" || true)
  [[ "$warns" -gt 0 ]] && { no "built with $warns warning(s) — see $log"; return; }
  BUILD_OK=1
  ok "clean build"
}

t_binaries_present() {
  hdr "build: every binary is present and executable"
  local missing="" b
  for b in "$DEVINFO_BIN" "$CONNECT_BIN" "$PINGPONG_BIN" "$ONESIDED_BIN" "$BENCH_BIN"; do
    [[ -x "$b" ]] || missing="$missing $(basename "$b")"
  done
  [[ -z "$missing" ]] && ok "all five present" || no "missing:$missing"
}

t_libs_located() {
  hdr "build: cmake reports paths for libibverbs and librdmacm"
  local log="$TMPDIR/cfg.log"
  cmake -S . -B build >"$log" 2>&1 || { no "configure failed"; return; }
  if grep -q 'libibverbs:.*/' "$log" && grep -q 'librdmacm:.*/' "$log"; then
    ok "both located"
  else
    no "status messages did not report both library paths"
  fi
}

# ══════════════════════════════════════════════════════════════════════
#  static — compile-time contracts, no device required
# ══════════════════════════════════════════════════════════════════════

#  0 compiled, 1 rejected, 2 file absent, 3 RDMA headers not installed
compiles() {
  local src="$1"
  [[ -f "$src" ]] || return 2
  g++ -std=c++20 -Iinclude -fsyntax-only "$src" >"$TMPDIR/sc.log" 2>&1 && return 0
  grep -qE 'infiniband/verbs\.h|rdma/rdma_cma\.h' "$TMPDIR/sc.log" && return 3
  return 1
}

t_ownership_contract() {
  hdr "static: verbs wrappers satisfy the ownership contract"
  compiles tests/static_contract.cpp
  case $? in
    0) ok "static_assert set holds for every wrapper type" ;;
    2) sk "tests/static_contract.cpp not present" ;;
    3) sk "libibverbs headers not installed" ;;
    *) no "failed to compile — see $TMPDIR/sc.log" ;;
  esac
}

t_cm_contract() {
  hdr "static: connection-manager wrappers satisfy the same contract"
  compiles tests/cm_static_contract.cpp
  case $? in
    0) ok "EventChannel, ConnectionId, Event hold" ;;
    2) sk "tests/cm_static_contract.cpp not present" ;;
    3) sk "libibverbs headers not installed" ;;
    *) no "failed to compile — see $TMPDIR/sc.log" ;;
  esac
}

t_copy_rejected() {
  hdr "static: copying a wrapper must not compile"
  [[ -f tests/copy_must_not_compile.cpp ]] || { sk "tests/copy_must_not_compile.cpp not present"; return; }
  local log="$TMPDIR/copy.log"
  if g++ -std=c++20 -Iinclude -fsyntax-only tests/copy_must_not_compile.cpp >"$log" 2>&1; then
    no "it compiled — the copy constructor is not deleted"
  elif grep -qE 'infiniband/verbs\.h|rdma/rdma_cma\.h' "$log"; then
    sk "libibverbs headers not installed"
  elif grep -qiE 'deleted function|call to deleted|use of deleted' "$log"; then
    ok "rejected: copy constructor is deleted"
  else
    no "rejected, but not because the copy is deleted — see $log"
  fi
}

t_no_raw_private_data() {
  hdr "static: Event exposes no raw private-data pointer"
  compiles tests/no_raw_private_data.cpp
  case $? in
    0) ok "accessor returns a checked copy" ;;
    2) sk "tests/no_raw_private_data.cpp not present" ;;
    3) sk "libibverbs headers not installed" ;;
    *) no "failed to compile — see $TMPDIR/sc.log" ;;
  esac
}

t_no_raw_release() {
  hdr "static: no release verb is called outside the wrapper implementations"
  local hits
  hits=$(grep -rnE 'ibv_(dealloc_pd|dereg_mr|destroy_cq|destroy_qp|destroy_comp_channel|close_device)|rdma_(destroy_id|destroy_event_channel|destroy_qp)' \
         src/ --include='*.cpp' 2>/dev/null \
         | grep -vE 'src/(verbs|cm|session)\.cpp' || true)
  if [[ -z "$hits" ]]; then
    ok "release verbs confined to verbs.cpp, cm.cpp, session.cpp"
  else
    no "raw release outside the wrappers:"; sed 's/^/          /' <<<"$hits" | head -5
  fi
}

t_no_goto_cleanup() {
  hdr "static: no goto-based cleanup survives"
  local hits
  hits=$(grep -rn 'goto[[:space:]]\+cleanup' src/ --include='*.cpp' 2>/dev/null || true)
  [[ -z "$hits" ]] && ok "none" || { no "found:"; sed 's/^/          /' <<<"$hits" | head -5; }
}

t_destructors_noexcept() {
  hdr "static: every wrapper destructor is noexcept"
  local bad="" f
  for f in include/limen/verbs.hpp include/limen/cm.hpp include/limen/session.hpp; do
    [[ -f "$f" ]] || continue
    while IFS= read -r line; do
      grep -q 'noexcept' <<<"$line" || bad="$bad $(basename "$f"):$(cut -d: -f1 <<<"$line")"
    done < <(grep -nE '^[[:space:]]*~[A-Za-z_]+\(' "$f" || true)
  done
  [[ -z "$bad" ]] && ok "all noexcept" || no "not noexcept:$bad"
}

t_side_channel_removed() {
  hdr "static: the ported programs use rdma_cm, not sockets or manual transitions"
  local hits
  #  the leading class excludes qualified calls such as PendingConnection::listen
  #  and prefixed verbs such as rdma_connect
  hits=$(grep -rnE '(^|[^[:alnum:]_:.>])(socket|connect|bind|listen|accept)[[:space:]]*\(|ibv_modify_qp' \
         src/limen_pingpong.cpp src/limen_onesided.cpp src/harness.cpp 2>/dev/null \
         | grep -v 'ibv_modify_cq' || true)
  [[ -z "$hits" ]] && ok "no socket call or ibv_modify_qp" \
                   || { no "found:"; sed 's/^/          /' <<<"$hits" | head -5; }
}

# ══════════════════════════════════════════════════════════════════════
#  local — single node, requires a device
# ══════════════════════════════════════════════════════════════════════

t_usage_exit_zero() {
  hdr "local: -h prints usage to stdout and exits 0"
  local out rc
  out=$("$DEVINFO_BIN" -h 2>/dev/null); rc=$?
  if   [[ "$rc" -ne 0 ]]; then no "expected exit 0, got $rc"
  elif [[ -z "$out"   ]]; then no "exited 0 but printed nothing"
  else ok "usage on stdout, exit 0"; fi
}

t_bad_flag_exit_one() {
  hdr "local: an unknown argument exits 1"
  local rc
  "$DEVINFO_BIN" --not-a-real-flag >/dev/null 2>&1; rc=$?
  [[ "$rc" -eq 1 ]] && ok "exit 1" || no "expected exit 1, got $rc"
}

t_enumerates_devices() {
  hdr "local: no arguments enumerates devices"
  local out rc
  out=$("$DEVINFO_BIN" 2>&1); rc=$?
  if [[ "$rc" -ne 0 ]]; then no "expected exit 0 with a device present, got $rc"; return; fi
  if grep -q '^device: ' <<<"$out" && grep -qE '^devices: [0-9]+' <<<"$out"; then
    ok "device: and devices: lines present"
  else
    no "missing device:/devices: lines"
  fi
}

t_unknown_device_exit_two() {
  hdr "local: an unknown device name exits 2 and lists what is available"
  local out rc
  out=$("$DEVINFO_BIN" -d definitely_not_a_device 2>&1); rc=$?
  if [[ "$rc" -ne 2 ]]; then no "expected exit 2, got $rc"; return; fi
  grep -q "$DEV" <<<"$out" && ok "exit 2, message names $DEV" \
                           || no "exit 2 but available devices not listed"
}

REPORT=""
capture_report() { [[ -n "$REPORT" ]] || REPORT=$("$DEVINFO_BIN" -d "$DEV" 2>&1 || true); }

t_device_attrs() {
  hdr "local: all eleven device attributes are reported"
  capture_report
  local missing="" f
  for f in guid fw_ver phys_port_cnt max_qp max_qp_wr max_cq max_cqe \
           max_mr max_mr_size max_sge max_qp_rd_atom; do
    grep -qE "^[[:space:]]*${f}:" <<<"$REPORT" || missing="$missing $f"
  done
  [[ -z "$missing" ]] && ok "all eleven present" || no "missing:$missing"
}

t_port_attrs() {
  hdr "local: port block is symbolic and reports MTU in bytes"
  capture_report
  local problems="" mtu
  grep -qE '^port [0-9]+:' <<<"$REPORT" || problems="$problems port-header"
  grep -qE '^[[:space:]]*state:[[:space:]]*PORT_[A-Z]+' <<<"$REPORT" || problems="$problems state"
  grep -qE '^[[:space:]]*link_layer:[[:space:]]*(Ethernet|InfiniBand|Unspecified)' <<<"$REPORT" \
    || problems="$problems link_layer"
  mtu=$(grep -oE '^[[:space:]]*active_mtu:[[:space:]]*[0-9]+' <<<"$REPORT" | grep -oE '[0-9]+$' || true)
  if   [[ -z "$mtu"     ]]; then problems="$problems active_mtu"
  elif [[ "$mtu" -lt 256 ]]; then problems="$problems active_mtu=${mtu}-is-an-enum-ordinal"; fi
  grep -qE '^[[:space:]]*max_msg_sz:'  <<<"$REPORT" || problems="$problems max_msg_sz"
  grep -qE '^[[:space:]]*gid_tbl_len:' <<<"$REPORT" || problems="$problems gid_tbl_len"
  [[ -z "$problems" ]] && ok "complete and symbolic (active_mtu=${mtu})" || no "problems:$problems"
}

t_bad_port_exit_one() {
  hdr "local: a port above phys_port_cnt exits 1"
  local rc
  "$DEVINFO_BIN" -d "$DEV" -p 99 >/dev/null 2>&1; rc=$?
  [[ "$rc" -eq 1 ]] && ok "exit 1" || no "expected exit 1, got $rc"
}

MROUT=""
capture_mr() { [[ -n "$MROUT" ]] || MROUT=$("$DEVINFO_BIN" -d "$DEV" -s 1048576 2>&1 || true); }

t_pd_allocated() {
  hdr "local: protection domain is allocated"
  capture_report
  grep -qE '^pd:[[:space:]]*allocated' <<<"$REPORT" && ok "pd: allocated" || no "no 'pd: allocated' line"
}

t_mr_registered() {
  hdr "local: -s registers a region and reports address, length, and flags"
  capture_mr
  local line problems=""
  line=$(grep -m1 '^mr: ' <<<"$MROUT" || true)
  [[ -n "$line" ]] || { no "no 'mr:' line"; return; }
  grep -qE 'addr=0x[0-9a-fA-F]+' <<<"$line" || problems="$problems addr"
  grep -qE 'length=1048576'      <<<"$line" || problems="$problems length"
  grep -q  'LOCAL_WRITE'         <<<"$line" || problems="$problems LOCAL_WRITE"
  grep -q  'REMOTE_READ'         <<<"$line" || problems="$problems REMOTE_READ"
  grep -q  'REMOTE_WRITE'        <<<"$line" || problems="$problems REMOTE_WRITE"
  grep -qE 'lkey=0x[0-9a-fA-F]+' <<<"$line" || problems="$problems lkey"
  grep -qE 'rkey=0x[0-9a-fA-F]+' <<<"$line" || problems="$problems rkey"
  [[ -z "$problems" ]] && ok "complete" || no "missing:$problems"
}

t_access_flag_check() {
  hdr "local: REMOTE_WRITE without LOCAL_WRITE is rejected with EINVAL"
  local out rc
  out=$("$DEVINFO_BIN" -d "$DEV" --check-access 2>&1); rc=$?
  if [[ "$rc" -ne 0 ]]; then no "exited $rc — an unexpected success is reported as 3"; return; fi
  grep -qE '^access-check:.*rejected:.*EINVAL' <<<"$out" \
    && ok "rejected, reported symbolically" \
    || no "no access-check line naming EINVAL"
}

t_oversized_mr_graceful() {
  hdr "local: an oversized registration fails with a complete diagnostic"
  local lim; lim=$(ulimit -l 2>/dev/null || echo unlimited)
  [[ "$lim" == "unlimited" ]] && { sk "RLIMIT_MEMLOCK is unlimited here"; return; }
  local bytes=$(( lim * 1024 * 4 )) out rc
  out=$("$DEVINFO_BIN" -d "$DEV" -s "$bytes" 2>&1); rc=$?
  [[ "$rc" -eq 0 ]] && { sk "${bytes} bytes registered — the limit did not bind"; return; }
  [[ "$rc" -ne 3 ]] && { no "expected exit 3, got $rc"; return; }
  local problems=""
  grep -qE 'registration failed:.*(ENOMEM|EINVAL|EPERM)' <<<"$out" || problems="$problems errno"
  grep -qE 'requested:[[:space:]]*[0-9]+'                <<<"$out" || problems="$problems size"
  grep -qiE 'RLIMIT_MEMLOCK'                             <<<"$out" || problems="$problems limit"
  grep -qiE 'hint:'                                      <<<"$out" || problems="$problems hint"
  [[ -z "$problems" ]] && ok "graceful" || no "diagnostic missing:$problems"
}

t_teardown_stages() {
  hdr "local: teardown reports every stage"
  capture_mr
  local line; line=$(grep -m1 '^teardown: ' <<<"$MROUT" || true)
  [[ -n "$line" ]] || { no "no teardown: line"; return; }
  local problems="" f
  for f in mr pd context; do grep -qE "${f}=ok" <<<"$line" || problems="$problems $f"; done
  [[ -z "$problems" ]] && ok "mr pd context all ok" || no "not ok:$problems"
}

t_missing_gid_exit_one() {
  hdr "local: limen_connect requires -g rather than defaulting"
  local rc
  "$CONNECT_BIN" -d "$DEV" >/dev/null 2>&1; rc=$?
  [[ "$rc" -eq 1 ]] && ok "exit 1" || no "expected exit 1, got $rc"
}

t_bad_gid_exit_one() {
  hdr "local: a GID index above gid_tbl_len exits 1"
  local rc
  "$CONNECT_BIN" -d "$DEV" -g 9999 >/dev/null 2>&1; rc=$?
  [[ "$rc" -eq 1 ]] && ok "exit 1" || no "expected exit 1, got $rc"
}

t_zero_gid_exit_three() {
  hdr "local: an unpopulated GID index is rejected with exit 3"
  local tbl idx="" i g
  tbl=$(ibv_devinfo -d "$DEV" 2>/dev/null | grep -oE 'gid_tbl_len:[[:space:]]*[0-9]+' \
        | grep -oE '[0-9]+$' || echo 0)
  for ((i = 0; i < tbl; i++)); do
    g=$(cat "/sys/class/infiniband/${DEV}/ports/1/gids/${i}" 2>/dev/null || echo "")
    [[ "$g" == "0000:0000:0000:0000:0000:0000:0000:0000" ]] && { idx="$i"; break; }
  done
  [[ -z "$idx" ]] && { sk "every GID table entry is populated"; return; }
  local rc
  "$CONNECT_BIN" -d "$DEV" -g "$idx" >/dev/null 2>&1; rc=$?
  [[ "$rc" -eq 3 ]] && ok "empty index $idx rejected with exit 3" || no "expected exit 3, got $rc"
}

t_bench_report_config() {
  hdr "local: limen_bench --report-config prints the conditions block and exits 0"
  local out rc
  out=$("$BENCH_BIN" -d "$DEV" --report-config 2>&1); rc=$?
  [[ "$rc" -ne 0 ]] && { no "expected exit 0, got $rc"; return; }
  local problems="" f
  for f in kernel governor clocksource device clock_floor_ns message_size warmup runs; do
    grep -qE "^[[:space:]]*${f}" <<<"$out" || problems="$problems $f"
  done
  [[ -z "$problems" ]] && ok "conditions complete" || no "missing:$problems"
}

t_bench_clock_floor() {
  hdr "local: --clock-floor reports a per-call cost"
  local out floor
  out=$("$BENCH_BIN" -d "$DEV" --clock-floor --report-config 2>&1 || true)
  floor=$(grep -m1 -oE 'clock_floor_ns[[:space:]]+[0-9]+' <<<"$out" | grep -oE '[0-9]+$' || true)
  if [[ -z "$floor" ]]; then
    no "no clock floor reported"
  elif [[ "$floor" -le 0 || "$floor" -gt 5000 ]]; then
    no "implausible floor: ${floor} ns"
  else
    ok "${floor} ns per clock_gettime call"
  fi
}

t_signal_period_validated() {
  hdr "local: a signalling period above max_qp_wr is rejected"
  local rc
  "$BENCH_BIN" -d "$DEV" --signal-every 99999999 --report-config >/dev/null 2>&1; rc=$?
  [[ "$rc" -ne 0 ]] && ok "rejected (exit $rc)" || no "accepted a period larger than the queue"
}

# ══════════════════════════════════════════════════════════════════════
#  transport — two nodes, connection manager and two-sided transfer
# ══════════════════════════════════════════════════════════════════════

t_unreachable_exit_four() {
  hdr "transport: an unreachable peer fails distinctly instead of hanging"
  local rc port; port=$(next_port)
  timeout 25 "$CONNECT_BIN" -d "$DEV" -g "$GID" -t "$port" 203.0.113.1 >/dev/null 2>&1; rc=$?
  if   [[ "$rc" -eq 124 ]]; then no "hung — the side channel needs a connect timeout"
  elif [[ "$rc" -eq 4   ]]; then ok "exit 4"
  else no "expected exit 4, got $rc"; fi
}

CONNRUN=""
capture_connect() {
  [[ -n "$CONNRUN" ]] && return 0
  CONNRUN=$(pair "$CONNECT_BIN" "" "" 40)
}

t_qp_bringup() {
  hdr "transport: queue pair reaches RTS through every transition"
  capture_connect
  local out; out=$(body_of "$CONNRUN")
  local problems=""
  grep -qE '^cq: cqe=[0-9]+ \(requested [0-9]+\)' <<<"$out" || problems="$problems cq-capacity"
  grep -q  'type=RC'                              <<<"$out" || problems="$problems qp-type"
  grep -qE 'max_send_wr=[0-9]+'                   <<<"$out" || problems="$problems granted-caps"
  grep -qE '^state: RESET -> INIT ok'             <<<"$out" || problems="$problems RESET->INIT"
  grep -qE '^state: INIT -> RTR ok'               <<<"$out" || problems="$problems INIT->RTR"
  grep -qE '^state: RTR -> RTS ok'                <<<"$out" || problems="$problems RTR->RTS"
  grep -qE '^verify: qp_state=RTS'                <<<"$out" || problems="$problems verify-RTS"
  [[ "$(exit_of "$CONNRUN")" -ne 0 ]]                       && problems="$problems nonzero-exit"
  [[ -z "$problems" ]] && ok "RESET, INIT, RTR, RTS, verified" || no "problems:$problems"
}

t_identity_exchanged() {
  hdr "transport: the peer's identity is received, not read back from self"
  capture_connect
  local out l r
  out=$(body_of "$CONNRUN")
  l=$(grep -m1 '^local:'  <<<"$out" | grep -oE 'qpn=0x[0-9a-fA-F]+' || true)
  r=$(grep -m1 '^remote:' <<<"$out" | grep -oE 'qpn=0x[0-9a-fA-F]+' || true)
  local problems=""
  grep -m1 '^local:' <<<"$out" | grep -qE 'psn=0x[0-9a-fA-F]+' || problems="$problems psn"
  grep -m1 '^local:' <<<"$out" | grep -qE 'gid=([0-9a-fA-F]{4}:){7}[0-9a-fA-F]{4}' || problems="$problems gid"
  if   [[ -z "$r"       ]]; then no "no remote: line — the exchange did not complete"
  elif [[ "$l" == "$r"  ]]; then no "remote qpn equals local qpn ($l)"
  elif [[ -n "$problems" ]]; then no "local identity missing:$problems"
  else ok "local $l, remote $r"; fi
}

t_connect_teardown() {
  hdr "transport: teardown reports all five stages"
  capture_connect
  local line problems="" f
  line=$(grep -m1 '^teardown: ' <<<"$(body_of "$CONNRUN")" || true)
  [[ -n "$line" ]] || { no "no teardown: line"; return; }
  for f in qp cq mr pd context; do grep -qE "${f}=ok" <<<"$line" || problems="$problems $f"; done
  [[ -z "$problems" ]] && ok "qp cq mr pd context all ok" || no "not ok:$problems"
}

PPRUN=""
capture_pingpong() {
  [[ -n "$PPRUN" ]] && return 0
  PPRUN=$(pair "$PINGPONG_BIN" "-n 100 -s 4096" "-n 100 -s 4096" 60)
}

t_pingpong_completes() {
  hdr "transport: a full ping-pong completes with matching counts"
  capture_pingpong
  local out; out=$(body_of "$PPRUN")
  [[ "$(exit_of "$PPRUN")" -ne 0 ]] && { no "exited $(exit_of "$PPRUN")"; return; }
  local s r
  s=$(num "$(grep -m1 -oE 'send_completions=[0-9]+' <<<"$out" || echo 0)")
  r=$(num "$(grep -m1 -oE 'recv_count=[0-9]+'       <<<"$out" || echo 0)")
  if [[ "${r:-0}" -eq 100 ]]; then
    ok "100 receives, ${s:-?} send completions"
  else
    no "expected 100 receives, got ${r:-none}"
  fi
}

t_payload_verified() {
  hdr "transport: payloads verify with zero mismatches"
  capture_pingpong
  local m
  m=$(grep -m1 -oE 'mismatches=[0-9]+' <<<"$(body_of "$PPRUN")" | grep -oE '[0-9]+$' || true)
  if   [[ -z "$m"      ]]; then no "no mismatches= counter in the output"
  elif [[ "$m" -eq 0   ]]; then ok "0 mismatches"
  else no "$m mismatches"; fi
}

t_recv_posted_first() {
  hdr "transport: receives are posted before the first send"
  capture_pingpong
  local out; out=$(body_of "$PPRUN")
  local rl sl
  rl=$(grep -n '^recv: ' <<<"$out" | head -1 | cut -d: -f1)
  sl=$(grep -nE '^(send|posted): ' <<<"$out" | head -1 | cut -d: -f1)
  if   [[ -z "$rl"                   ]]; then no "no recv: line reporting slots, depth, and size"
  elif [[ -z "$sl" || "$rl" -lt "$sl" ]]; then ok "receive queue filled first"
  else no "a send was reported before the receives"; fi
}

t_iteration_count_honoured() {
  hdr "transport: a non-default iteration count is honoured exactly"
  local out r
  out=$(pair "$PINGPONG_BIN" "-n 7" "-n 7" 40)
  r=$(grep -m1 -oE 'recv_count=[0-9]+' <<<"$(body_of "$out")" | grep -oE '[0-9]+$' || true)
  [[ "${r:-0}" -eq 7 ]] && ok "7 requested, 7 completed" || no "expected 7, got ${r:-none}"
}

t_rnr_infinite_times_out() {
  hdr "transport: infinite RNR retry times out rather than hanging forever"
  local out rc
  out=$(pair "$PINGPONG_BIN" "--no-recv" "-n 5" 45)
  rc=$(exit_of "$out")
  if   [[ "$rc" -eq 124 ]]; then no "never returned — the client has no timeout"
  elif [[ "$rc" -eq 0   ]]; then no "reported success although the peer posted no receives"
  else ok "gave up and exited $rc"; fi
}

t_rnr_finite_reports_error() {
  hdr "transport: finite RNR retry yields a retry-exceeded completion"
  local out body
  out=$(pair "$PINGPONG_BIN" "--no-recv --rnr-retry 1" "--rnr-retry 1 -n 5" 45)
  body=$(body_of "$out")
  grep -qE 'RNR_RETRY_EXC' <<<"$body" \
    && ok "IBV_WC_RNR_RETRY_EXC_ERR reported" \
    || no "no RNR retry-exceeded status in the output"
}

t_error_completion_fields() {
  hdr "transport: error completions omit opcode and byte_len"
  local out body
  out=$(pair "$PINGPONG_BIN" "--no-recv --rnr-retry 1" "--rnr-retry 1 -n 5" 45)
  body=$(body_of "$out")
  local errline
  errline=$(grep -m1 -E 'status=[A-Z_]*ERR' <<<"$body" || true)
  [[ -n "$errline" ]] || { no "no error completion line"; return; }
  if grep -qE 'opcode=|byte_len=' <<<"$errline"; then
    no "error completion reports opcode or byte_len, which are not valid"
  else
    grep -qi 'not valid' <<<"$body" && ok "fields omitted, validity noted" \
                                    || ok "fields omitted"
  fi
}

t_qp_error_state() {
  hdr "transport: the queue pair reports ERR after an error completion"
  local out
  out=$(pair "$PINGPONG_BIN" "--no-recv --rnr-retry 1" "--rnr-retry 1 -n 5" 45)
  grep -qE 'qp_state[^=]*=[[:space:]]*ERR' <<<"$(body_of "$out")" \
    && ok "ERR reported" \
    || no "queue-pair state after the error was not reported as ERR"
}

t_clean_disconnect() {
  hdr "transport: both sides reach DISCONNECTED and the client exits 0"
  local out
  out=$(pair "$PINGPONG_BIN" "-n 50" "-n 50" 45)
  [[ "$(exit_of "$out")" -ne 0 ]] && { no "client exited $(exit_of "$out")"; return; }
  local log; log=$(server_log)
  if [[ -z "$log" ]]; then
    ok "client exited 0 (peer log unavailable)"
  elif grep -qi 'DISCONNECT' <<<"$log"; then
    ok "both sides disconnected cleanly"
  else
    no "the peer did not report a disconnect"
  fi
}

t_no_teardown_hang() {
  hdr "transport: teardown completes, no unacknowledged event blocks destroy"
  local out rc
  out=$(pair "$PINGPONG_BIN" "-n 10" "-n 10" 30)
  rc=$(exit_of "$out")
  [[ "$rc" -eq 124 ]] && no "hung in teardown" || ok "returned in time (exit $rc)"
}

# ══════════════════════════════════════════════════════════════════════
#  onesided — RDMA write and read
# ══════════════════════════════════════════════════════════════════════

t_descriptor_validated() {
  hdr "onesided: a zero remote address or key is rejected before posting"
  local out rc
  out=$(pair "$ONESIDED_BIN" "--mode write -n 5" "--mode write --bad-rkey -n 5" 40)
  rc=$(exit_of "$out")
  if [[ "$rc" -eq 5 ]] || grep -qE 'REM_ACCESS_ERR' <<<"$(body_of "$out")"; then
    ok "invalid descriptor rejected (exit $rc)"
  else
    no "expected exit 5 or REM_ACCESS_ERR, got exit $rc"
  fi
}

t_rdma_write() {
  hdr "onesided: RDMA_WRITE completes and the peer's buffer matches"
  local out m
  out=$(pair "$ONESIDED_BIN" "--mode write -n 100" "--mode write -n 100" 60)
  [[ "$(exit_of "$out")" -ne 0 ]] && { no "exited $(exit_of "$out")"; return; }
  m=$(grep -m1 -oE 'mismatches=[0-9]+' <<<"$(body_of "$out")" | grep -oE '[0-9]+$' || echo 0)
  [[ "${m:-0}" -eq 0 ]] && ok "0 mismatches" || no "$m mismatches"
}

t_receiver_passive() {
  hdr "onesided: the peer reports zero completions during a plain write"
  local out log c
  out=$(pair "$ONESIDED_BIN" "--mode write -n 100" "--mode write -n 100" 60)
  log=$(server_log)
  [[ -z "$log" ]] && { sk "peer log unavailable"; return; }
  c=$(grep -m1 -oE 'recv_count=[0-9]+' <<<"$log" | grep -oE '[0-9]+$' || echo 0)
  [[ "${c:-0}" -eq 0 ]] && ok "peer saw no completions" \
                        || no "peer reported ${c} completions for a one-sided write"
}

t_rdma_read() {
  hdr "onesided: RDMA_READ retrieves the peer's contents correctly"
  local out m
  out=$(pair "$ONESIDED_BIN" "--mode read -n 50" "--mode read -n 50" 60)
  [[ "$(exit_of "$out")" -ne 0 ]] && { no "exited $(exit_of "$out")"; return; }
  m=$(grep -m1 -oE 'mismatches=[0-9]+' <<<"$(body_of "$out")" | grep -oE '[0-9]+$' || echo 0)
  [[ "${m:-0}" -eq 0 ]] && ok "0 mismatches" || no "$m mismatches"
}

t_flag_notification() {
  hdr "onesided: flag-write notification completes with zero mismatches"
  local out m
  out=$(pair "$ONESIDED_BIN" "--mode flag -n 100" "--mode flag -n 100" 60)
  [[ "$(exit_of "$out")" -ne 0 ]] && { no "exited $(exit_of "$out")"; return; }
  m=$(grep -m1 -oE 'mismatches=[0-9]+' <<<"$(body_of "$out")" | grep -oE '[0-9]+$' || echo 0)
  [[ "${m:-0}" -eq 0 ]] && ok "0 mismatches" || no "$m mismatches"
}

t_write_with_imm() {
  hdr "onesided: immediate mode delivers the iteration number and is two-sided"
  local out log c
  out=$(pair "$ONESIDED_BIN" "--mode imm -n 50" "--mode imm -n 50" 60)
  [[ "$(exit_of "$out")" -ne 0 ]] && { no "exited $(exit_of "$out")"; return; }
  log=$(server_log)
  if [[ -z "$log" ]]; then sk "peer log unavailable"; return; fi
  c=$(grep -m1 -oE 'recv_count=[0-9]+' <<<"$log" | grep -oE '[0-9]+$' || echo 0)
  [[ "${c:-0}" -gt 0 ]] && ok "peer consumed ${c} receives" \
                        || no "peer saw no completions — immediate should consume a receive"
}

t_read_depth_reported() {
  hdr "onesided: the effective outstanding-read limit is reported"
  local out
  out=$(pair "$ONESIDED_BIN" "--mode read -n 5" "--mode read -n 5" 40)
  grep -qE '(initiator_depth|max_rd_atomic|read_depth)[^0-9]*[0-9]+' <<<"$(body_of "$out")" \
    && ok "reported" \
    || no "no outstanding-read limit in the output"
}

# ══════════════════════════════════════════════════════════════════════
#  efficiency — the five tunable mechanisms
# ══════════════════════════════════════════════════════════════════════

t_pp_report_config() {
  hdr "efficiency: --report-config prints all five settings and exits 0"
  local out rc problems="" f
  out=$("$PINGPONG_BIN" -d "$DEV" --report-config 2>&1); rc=$?
  [[ "$rc" -ne 0 ]] && { no "expected exit 0, got $rc"; return; }
  for f in inline signal_every pipeline reap moderation; do
    grep -q "$f" <<<"$out" || problems="$problems $f"
  done
  [[ -z "$problems" ]] && ok "all five reported" || no "missing:$problems"
}

t_inline_limit_reported() {
  hdr "efficiency: the granted max_inline_data is reported"
  local out v
  out=$("$PINGPONG_BIN" -d "$DEV" --report-config 2>&1 || true)
  v=$(grep -m1 -oE 'max=[0-9]+' <<<"$out" | grep -oE '[0-9]+$' || true)
  [[ -n "$v" ]] && ok "max_inline_data=${v}" || no "not reported"
}

t_inline_correct() {
  hdr "efficiency: --inline transfers correctly"
  local out m
  out=$(pair "$PINGPONG_BIN" "-n 500 -s 64" "--inline -n 500 -s 64" 60)
  [[ "$(exit_of "$out")" -ne 0 ]] && { no "exited $(exit_of "$out")"; return; }
  m=$(grep -m1 -oE 'mismatches=[0-9]+' <<<"$(body_of "$out")" | grep -oE '[0-9]+$' || echo 0)
  [[ "${m:-0}" -eq 0 ]] && ok "0 mismatches" || no "$m mismatches"
}

t_signal_every_reduces() {
  hdr "efficiency: --signal-every 16 reaps roughly a sixteenth of the sends"
  local out c
  out=$(pair "$PINGPONG_BIN" "-n 1000" "--signal-every 16 -n 1000" 90)
  c=$(grep -m1 -oE 'send_completions=[0-9]+' <<<"$(body_of "$out")" | grep -oE '[0-9]+$' || true)
  if   [[ -z "$c"           ]]; then no "no send_completions counter"
  elif [[ "$c" -gt 200      ]]; then no "${c} send completions for 1000 sends — signalling had no effect"
  elif [[ "$c" -lt 40       ]]; then no "${c} send completions — fewer than the final flush should leave"
  else ok "${c} completions for 1000 sends"; fi
}

t_buffer_reuse_correct() {
  hdr "efficiency: payloads verify across several signalling periods"
  local p out m bad=""
  for p in 1 4 16; do
    out=$(pair "$PINGPONG_BIN" "-n 500" "--signal-every $p --pipeline 8 -n 500" 90)
    m=$(grep -m1 -oE 'mismatches=[0-9]+' <<<"$(body_of "$out")" | grep -oE '[0-9]+$' || echo 1)
    [[ "${m:-1}" -eq 0 ]] || bad="$bad period=$p"
  done
  [[ -z "$bad" ]] && ok "periods 1, 4, 16 all clean" || no "mismatches at:$bad"
}

t_pipeline_clamped() {
  hdr "efficiency: pipeline depth is clamped to the granted send-queue depth"
  local out d
  out=$(pair "$PINGPONG_BIN" "-n 10" "--pipeline 999999 -n 10" 40)
  d=$(grep -m1 -oE 'pipeline=[0-9]+' <<<"$(body_of "$out")" | grep -oE '[0-9]+$' || true)
  if   [[ -z "$d"          ]]; then no "effective pipeline depth not reported"
  elif [[ "$d" -ge 999999  ]]; then no "requested depth accepted without clamping"
  else ok "clamped to ${d}"; fi
}

t_pipeline_correct() {
  hdr "efficiency: a pipelined run completes with zero mismatches"
  local out m
  out=$(pair "$PINGPONG_BIN" "-n 1000" "--pipeline 8 --signal-every 4 -n 1000" 90)
  [[ "$(exit_of "$out")" -ne 0 ]] && { no "exited $(exit_of "$out")"; return; }
  m=$(grep -m1 -oE 'mismatches=[0-9]+' <<<"$(body_of "$out")" | grep -oE '[0-9]+$' || echo 0)
  [[ "${m:-0}" -eq 0 ]] && ok "0 mismatches" || no "$m mismatches"
}

t_event_mode_completes() {
  hdr "efficiency: event-driven reaping completes without hanging"
  local out rc m
  out=$(pair "$PINGPONG_BIN" "-n 500 --reap event" "--reap event -n 500" 90)
  rc=$(exit_of "$out")
  [[ "$rc" -eq 124 ]] && { no "hung — check the arm, re-drain, block sequence"; return; }
  [[ "$rc" -ne 0   ]] && { no "exited $rc"; return; }
  m=$(grep -m1 -oE 'mismatches=[0-9]+' <<<"$(body_of "$out")" | grep -oE '[0-9]+$' || echo 0)
  [[ "${m:-0}" -eq 0 ]] && ok "completed, 0 mismatches" || no "$m mismatches"
}

t_events_all_acked() {
  hdr "efficiency: events received and acknowledged are equal at exit"
  local out body rcv ack
  out=$(pair "$PINGPONG_BIN" "-n 500 --reap event" "--reap event -n 500" 90)
  body=$(body_of "$out")
  rcv=$(grep -m1 -oE 'events_received=[0-9]+' <<<"$body" | grep -oE '[0-9]+$' || true)
  ack=$(grep -m1 -oE 'events_acked=[0-9]+'    <<<"$body" | grep -oE '[0-9]+$' || true)
  if   [[ -z "$rcv" || -z "$ack" ]]; then no "event counters not reported"
  elif [[ "$rcv" -ne "$ack"      ]]; then no "received ${rcv}, acknowledged ${ack} — destroy would block"
  else ok "${rcv} received, ${ack} acknowledged"; fi
}

t_empty_events_tolerated() {
  hdr "efficiency: an event carrying no completion is not treated as an error"
  local out body
  out=$(pair "$PINGPONG_BIN" "-n 2000 --reap event" "--reap event --pipeline 8 -n 2000" 120)
  body=$(body_of "$out")
  [[ "$(exit_of "$out")" -ne 0 ]] && { no "exited $(exit_of "$out")"; return; }
  grep -qE 'empty_events=[0-9]+' <<<"$body" \
    && ok "$(grep -m1 -oE 'empty_events=[0-9]+' <<<"$body") and the run still completed" \
    || ok "completed (no empty_events counter to check)"
}

t_race_guard_reported() {
  hdr "efficiency: the arm/completion race guard is instrumented"
  local out
  out=$(pair "$PINGPONG_BIN" "-n 500 --reap event" "--reap event -n 500" 90)
  grep -qE 'race_polls_hit=[0-9]+' <<<"$(body_of "$out")" \
    && ok "$(grep -m1 -oE 'race_polls_hit=[0-9]+' <<<"$(body_of "$out")")" \
    || no "race_polls_hit not reported"
}

t_broken_arming() {
  hdr "efficiency: --broken-arming is implemented and removes the guard"
  local out rc
  out=$(pair "$PINGPONG_BIN" "-n 20000 --reap event" "--reap event --pipeline 32 --broken-arming -n 20000" 180)
  rc=$(exit_of "$out")
  #  either outcome is informative: a hang demonstrates the race, a completion
  #  shows it did not fire on this run. Silent non-implementation is not.
  if grep -qiE 'broken-arming|unknown option' <<<"$(body_of "$out")" \
     && grep -qi 'unknown' <<<"$(body_of "$out")"; then
    no "--broken-arming is not implemented"
  elif [[ "$rc" -eq 124 ]]; then
    ok "hung with the guard removed — the race is real"
  else
    ok "completed with the guard removed (exit $rc) — the race did not fire this run"
  fi
}

t_moderation_graceful() {
  hdr "efficiency: unsupported CQ moderation reports unavailable and exits 0"
  local out rc
  out=$("$PINGPONG_BIN" -d "$DEV" --moderate 16:8 --report-config 2>&1); rc=$?
  [[ "$rc" -ne 0 ]] && { no "expected exit 0, got $rc"; return; }
  grep -qE 'moderation=(16:8|unavailable|off)' <<<"$out" \
    && ok "$(grep -m1 -oE 'moderation=[^ ]+' <<<"$out")" \
    || no "moderation state not reported"
}

t_option_matrix() {
  hdr "efficiency: every cell of the option matrix completes cleanly"
  local cells=(
    "--inline -s 64"
    "--signal-every 16"
    "--pipeline 8"
    "--reap event"
    "--inline --signal-every 4 --pipeline 8 -s 64"
  )
  local c out m bad=""
  for c in "${cells[@]}"; do
    out=$(pair "$PINGPONG_BIN" "-n 300 ${c##*-s }" "$c -n 300" 90)
    m=$(grep -m1 -oE 'mismatches=[0-9]+' <<<"$(body_of "$out")" | grep -oE '[0-9]+$' || echo 1)
    if [[ "$(exit_of "$out")" -ne 0 || "${m:-1}" -ne 0 ]]; then bad="$bad [$c]"; fi
  done
  [[ -z "$bad" ]] && ok "${#cells[@]} cells clean" || no "failed:$bad"
}

# ══════════════════════════════════════════════════════════════════════
#  bench — the measurement harness
# ══════════════════════════════════════════════════════════════════════

bench_pair() {
  local sargs="$1" cargs="$2" tmo="${3:-120}" port
  port=$(next_port)
  : >"$SRV_LOG"
  if [[ -n "$PEER_CMD" ]]; then
    # shellcheck disable=SC2086
    $PEER_CMD "$BENCH_BIN" -d "$PEER_DEV" -t "$port" $sargs >"$SRV_LOG" 2>&1 &
    local i=0
    while [[ $i -lt 80 ]]; do
      grep -q 'role: server' "$SRV_LOG" 2>/dev/null && break
      sleep 0.05; i=$((i + 1))
    done
    sleep 0.3
  elif [[ -n "$SSH_PEER" ]]; then
    ssh -o BatchMode=yes "$SSH_PEER" \
      "cd '$(pwd)' && nohup $BENCH_BIN -d $PEER_DEV -t $port $sargs >/tmp/limen-srv.log 2>&1 & disown" \
      >/dev/null 2>&1 || true
    sleep 2
  else
    echo "      on the peer:  $BENCH_BIN -d $PEER_DEV -t $port $sargs" >&2
    read -rp "      press enter once it is running... " >&2
  fi
  # shellcheck disable=SC2086
  timeout "$tmo" "$BENCH_BIN" -d "$DEV" -t "$port" $cargs "$PEER" 2>&1
  local rc=$?
  kill_peer "$BENCH_BIN" "$port"
  echo "__EXIT__${rc}"
}

BENCHRUN=""
capture_bench() {
  [[ -n "$BENCHRUN" ]] && return 0
  BENCHRUN=$(bench_pair "-s 1024 --pipeline 8 -n 100000" \
                        "--mode latency -s 1024 -n 2000 --warmup 200 --runs 3 --json $TMPDIR/b.json" 150)
}

t_bench_distribution() {
  hdr "bench: all eight distribution statistics are reported"
  capture_bench
  local out problems="" f
  out=$(body_of "$BENCHRUN")
  [[ "$(exit_of "$BENCHRUN")" -ne 0 ]] && { no "exited $(exit_of "$BENCHRUN")"; return; }
  for f in min 50 90 99 99.9 mean max stddev; do
    grep -q "$f" <<<"$out" || problems="$problems $f"
  done
  [[ -z "$problems" ]] && ok "min p50 p90 p99 p99.9 mean max stddev" || no "missing:$problems"
}

t_bench_percentiles_ordered() {
  hdr "bench: percentiles are non-decreasing"
  capture_bench
  local line vals
  line=$(grep -m1 -E '^latency \(us\):' <<<"$(body_of "$BENCHRUN")" || true)
  [[ -n "$line" ]] || { no "no latency percentile line"; return; }
  vals=$(grep -oE '[0-9]+\.[0-9]+' <<<"$line")
  local prev="" v bad=0
  for v in $vals; do
    [[ -n "$prev" ]] && awk "BEGIN{exit !($v < $prev)}" && bad=1
    prev="$v"
  done
  [[ "$bad" -eq 0 ]] && ok "ordered: $(tr '\n' ' ' <<<"$vals")" || no "out of order: $(tr '\n' ' ' <<<"$vals")"
}

t_bench_json_samples() {
  hdr "bench: --json emits raw samples and the conditions block"
  capture_bench
  [[ -s "$TMPDIR/b.json" ]] || { no "no JSON written"; return; }
  local problems="" f
  for f in conditions config noise_pct samples_ns; do
    grep -q "\"$f\"" "$TMPDIR/b.json" || problems="$problems $f"
  done
  local n
  n=$(grep -oE '"samples_ns": \[[0-9,]+' "$TMPDIR/b.json" | head -1 | tr -cd ',' | wc -c)
  [[ "${n:-0}" -lt 100 ]] && problems="$problems too-few-samples"
  [[ -z "$problems" ]] && ok "conditions, config, floor, ${n} samples" || no "missing:$problems"
}

t_bench_warmup_applied() {
  hdr "bench: warm-up iterations are excluded from the recorded samples"
  local out n
  out=$(bench_pair "-s 1024 --pipeline 8 -n 100000" \
                   "--mode latency -s 1024 -n 500 --warmup 100 --runs 1 --json $TMPDIR/w.json" 90)
  [[ "$(exit_of "$out")" -ne 0 ]] && { no "exited $(exit_of "$out")"; return; }
  n=$(grep -oE '"ops": [0-9]+' "$TMPDIR/w.json" | head -1 | grep -oE '[0-9]+$' || true)
  if   [[ -z "$n"        ]]; then no "sample count not present in the JSON"
  elif [[ "$n" -eq 500   ]]; then ok "500 recorded, 100 warm-up excluded"
  else no "expected 500 recorded samples, got ${n}"; fi
}

t_bench_modes_labelled() {
  hdr "bench: latency and response figures are labelled distinctly"
  capture_bench
  grep -q 'quantity=service_time' <<<"$(body_of "$BENCHRUN")" \
    || { no "latency mode did not label its output service_time"; return; }
  local out
  out=$(bench_pair "-s 1024 --pipeline 8 -n 100000" \
                   "--mode response -s 1024 --rate 20000 -n 2000 --warmup 200 --runs 1" 90)
  grep -q 'quantity=response_time' <<<"$(body_of "$out")" \
    && ok "service_time and response_time both labelled" \
    || no "response mode did not label its output response_time"
}

t_bench_response_schedule() {
  hdr "bench: response mode measures from the intended time, not from issue"
  local out body med late
  #  at a rate well beyond capacity, lateness must accumulate. a harness timing
  #  from issue would report roughly the service time instead.
  out=$(bench_pair "-s 1024 --pipeline 8 -n 100000" \
                   "--mode response -s 1024 --rate 2000000 -n 5000 --warmup 500 --runs 1" 120)
  body=$(body_of "$out")
  med=$(grep -m1 -E '^latency \(us\):' <<<"$body" | grep -oE '[0-9]+\.[0-9]+' | sed -n 2p || true)
  late=$(grep -m1 -oE 'max_late_us=[0-9.]+' <<<"$body" | grep -oE '[0-9.]+$' || true)
  if [[ -z "$med" ]]; then
    no "no percentile line in response mode"
  elif awk "BEGIN{exit !($med > 50)}"; then
    ok "median ${med} us at a saturating rate, max lateness ${late:-?} us"
  else
    no "median ${med} us at 2M ops/s — lateness is not being accumulated"
  fi
}

t_bench_bandwidth_units() {
  hdr "bench: bandwidth is reported in both units with a message rate"
  local out body
  out=$(bench_pair "-s 65536 --pipeline 64 -n 100000" \
                   "--mode bandwidth -s 65536 --pipeline 64 -n 5000 --warmup 500 --runs 1" 120)
  body=$(body_of "$out")
  local problems=""
  grep -q 'MiB/s'  <<<"$body" || problems="$problems MiB/s"
  grep -q 'Gbit/s' <<<"$body" || problems="$problems Gbit/s"
  grep -q 'msg/s'  <<<"$body" || problems="$problems msg/s"
  grep -qE '[0-9.]+ s' <<<"$body" || problems="$problems elapsed"
  [[ -z "$problems" ]] && ok "$(grep -m1 '^bandwidth:' <<<"$body")" || no "missing:$problems"
}

t_bench_variance() {
  hdr "bench: --runs reports a spread across runs"
  capture_bench
  local line pct
  line=$(grep -m1 '^noise floor:' <<<"$(body_of "$BENCHRUN")" || true)
  [[ -n "$line" ]] || { no "no noise floor line"; return; }
  pct=$(grep -oE '[0-9]+\.[0-9]+%' <<<"$line" | head -1 | tr -d '%')
  local runs; runs=$(grep -oE 'over [0-9]+ runs' <<<"$line" | grep -oE '[0-9]+' || echo 0)
  if   [[ "${runs:-0}" -lt 2 ]]; then no "only ${runs} run contributed to the floor"
  elif [[ -z "$pct"          ]]; then no "floor not expressed as a percentage"
  else ok "${pct}% over ${runs} runs"; fi
}

t_bench_size_sweep() {
  hdr "bench: the size sweep covers 64 B to 1 MB"
  local out body
  out=$(bench_pair "-s 1048576 --pipeline 64 -n 100000000" \
                   "--mode sweep -s 4096 -n 400 --warmup 100 --runs 2" 900)
  body=$(body_of "$out")
  local problems=""
  grep -qE '^[[:space:]]*64[[:space:]]'      <<<"$body" || problems="$problems 64B"
  grep -qE '^[[:space:]]*1048576[[:space:]]' <<<"$body" || problems="$problems 1MB"
  local rows; rows=$(grep -cE '^[[:space:]]*[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9]+\.[0-9]+' <<<"$body")
  [[ "${rows:-0}" -lt 15 ]] && problems="$problems only-${rows}-rows"
  [[ -z "$problems" ]] && ok "${rows} sizes measured" || no "problems:$problems"
  BENCH_SWEEP_OUT="$body"
}

BENCH_SWEEP_OUT=""
t_bench_option_sweep() {
  hdr "bench: the option sweep covers every switch and marks the noise floor"
  [[ -n "$BENCH_SWEEP_OUT" ]] || { sk "size sweep did not run"; return; }
  local problems="" f
  for f in baseline inline signal_every pipeline reap; do
    grep -q "$f" <<<"$BENCH_SWEEP_OUT" || problems="$problems $f"
  done
  grep -qiE 'within noise|SIGNIFICANT' <<<"$BENCH_SWEEP_OUT" || problems="$problems verdict-column"
  [[ -z "$problems" ]] && ok "all switches with verdicts" || no "missing:$problems"
}

# ══════════════════════════════════════════════════════════════════════
#  leaks
# ══════════════════════════════════════════════════════════════════════

vg_check() {
  local label="$1"; shift
  local log="$TMPDIR/vg-$$.log"
  timeout 240 valgrind --leak-check=full --show-leak-kinds=definite \
    "$@" >/dev/null 2>"$log" || true
  local lost
  lost=$(grep -oE 'definitely lost: [0-9,]+ bytes' "$log" | grep -oE '[0-9,]+' | tr -d ',' || echo "")
  if   [[ -z "$lost"    ]]; then no "${label}: could not parse valgrind output"
  elif [[ "$lost" -eq 0 ]]; then ok "${label}: 0 bytes definitely lost"
  else no "${label}: ${lost} bytes definitely lost — see $log"; fi
}

t_leaks_local() {
  hdr "leaks: limen_devinfo"
  command -v valgrind >/dev/null 2>&1 || { sk "valgrind not installed"; return; }
  vg_check "devinfo" "$DEVINFO_BIN" -d "$DEV" -s 1048576
}

t_leaks_transport() {
  hdr "leaks: limen_pingpong, poll and event paths"
  command -v valgrind >/dev/null 2>&1 || { sk "valgrind not installed"; return; }
  local port
  port=$(next_port); start_peer "$PINGPONG_BIN" "$port" "-n 50"
  vg_check "pingpong/poll" "$PINGPONG_BIN" -d "$DEV" ${GID:+-g $GID} -t "$port" -n 50 "$PEER"
  kill_peer "$PINGPONG_BIN" "$port"

  port=$(next_port); start_peer "$PINGPONG_BIN" "$port" "-n 50 --reap event"
  vg_check "pingpong/event" "$PINGPONG_BIN" -d "$DEV" ${GID:+-g $GID} -t "$port" --reap event -n 50 "$PEER"
  kill_peer "$PINGPONG_BIN" "$port"
}

t_leaks_onesided() {
  hdr "leaks: limen_onesided"
  command -v valgrind >/dev/null 2>&1 || { sk "valgrind not installed"; return; }
  local port; port=$(next_port)
  start_peer "$ONESIDED_BIN" "$port" "--mode write -n 20"
  vg_check "onesided" "$ONESIDED_BIN" -d "$DEV" ${GID:+-g $GID} -t "$port" --mode write -n 20 "$PEER"
  kill_peer "$ONESIDED_BIN" "$port"
}

t_leaks_bench() {
  hdr "leaks: limen_bench"
  command -v valgrind >/dev/null 2>&1 || { sk "valgrind not installed"; return; }
  local port; port=$(next_port)
  if [[ -n "$PEER_CMD" ]]; then
    $PEER_CMD "$BENCH_BIN" -d "$PEER_DEV" -t "$port" -s 1024 -n 100000 >"$SRV_LOG" 2>&1 &
    sleep 1
  fi
  vg_check "bench" "$BENCH_BIN" -d "$DEV" -t "$port" --mode latency -s 1024 -n 200 --warmup 20 "$PEER"
  kill_peer "$BENCH_BIN" "$port"
}

# ══════════════════════════════════════════════════════════════════════
#  Dispatch
# ══════════════════════════════════════════════════════════════════════

[[ "$LIST" -eq 1 ]] || echo -e "${B}=== Limen acceptance suite ===${N}"

if want_group build; then
  [[ "$LIST" -eq 0 ]] && sec "build"
  run build builds_clean && t_builds_clean
  if [[ "$LIST" -eq 0 && "$BUILD_OK" -ne 1 ]]; then
    echo -e "\n${R}Build failed. Nothing else can run.${N}"
    exit 1
  fi
  run build binaries_present && t_binaries_present
  run build libs_located     && t_libs_located
fi

if want_group static; then
  [[ "$LIST" -eq 0 ]] && sec "static"
  run static ownership_contract   && t_ownership_contract
  run static cm_contract          && t_cm_contract
  run static copy_rejected        && t_copy_rejected
  run static no_raw_private_data  && t_no_raw_private_data
  run static no_raw_release       && t_no_raw_release
  run static no_goto_cleanup      && t_no_goto_cleanup
  run static destructors_noexcept && t_destructors_noexcept
  run static side_channel_removed && t_side_channel_removed
fi

#  device auto-detection
if [[ -z "$DEV" ]] && command -v ibv_devices >/dev/null 2>&1; then
  DEV=$(ibv_devices 2>/dev/null | awk 'NR>2 && NF {print $1; exit}' || true)
fi
[[ -z "$PEER_DEV" ]] && PEER_DEV="$DEV"
[[ "$PEER_CMD" == sudo* ]] && { sudo -v || { echo "sudo credentials required for --peer-cmd"; exit 1; }; }

HAVE_DEV=0;  [[ -n "$DEV" ]] && HAVE_DEV=1
HAVE_PEER=0; [[ -n "$DEV" && -n "$PEER" ]] && HAVE_PEER=1

if want_group local; then
  [[ "$LIST" -eq 0 ]] && sec "local (device ${DEV:-none})"
  if [[ "$HAVE_DEV" -eq 0 && "$LIST" -eq 0 ]]; then
    sk "no RDMA device found — pass --dev"
  else
    run local usage_exit_zero       && t_usage_exit_zero
    run local bad_flag_exit_one     && t_bad_flag_exit_one
    run local enumerates_devices    && t_enumerates_devices
    run local unknown_device        && t_unknown_device_exit_two
    run local device_attrs          && t_device_attrs
    run local port_attrs            && t_port_attrs
    run local bad_port_exit_one     && t_bad_port_exit_one
    run local pd_allocated          && t_pd_allocated
    run local mr_registered         && t_mr_registered
    run local access_flag_check     && t_access_flag_check
    run local oversized_mr          && t_oversized_mr_graceful
    run local teardown_stages       && t_teardown_stages
    run local missing_gid           && t_missing_gid_exit_one
    run local bad_gid               && t_bad_gid_exit_one
    run local zero_gid              && t_zero_gid_exit_three
    run local bench_report_config   && t_bench_report_config
    run local bench_clock_floor     && t_bench_clock_floor
    run local signal_period         && t_signal_period_validated
  fi
fi

need_peer() {
  [[ "$HAVE_PEER" -eq 1 ]] && return 0
  [[ "$LIST" -eq 0 ]] && sk "requires --peer and a device"
  return 1
}

if want_group transport; then
  [[ "$LIST" -eq 0 ]] && sec "transport (peer ${PEER:-none})"
  if [[ "$LIST" -eq 1 ]] || need_peer; then
    run transport unreachable_exit_four  && t_unreachable_exit_four
    run transport qp_bringup             && t_qp_bringup
    run transport identity_exchanged     && t_identity_exchanged
    run transport connect_teardown       && t_connect_teardown
    run transport pingpong_completes     && t_pingpong_completes
    run transport payload_verified       && t_payload_verified
    run transport recv_posted_first      && t_recv_posted_first
    run transport iteration_count        && t_iteration_count_honoured
    run transport rnr_infinite           && t_rnr_infinite_times_out
    run transport rnr_finite             && t_rnr_finite_reports_error
    run transport error_completion       && t_error_completion_fields
    run transport qp_error_state         && t_qp_error_state
    run transport clean_disconnect       && t_clean_disconnect
    run transport no_teardown_hang       && t_no_teardown_hang
  fi
fi

if want_group onesided; then
  [[ "$LIST" -eq 0 ]] && sec "onesided"
  if [[ "$LIST" -eq 1 ]] || need_peer; then
    run onesided descriptor_validated && t_descriptor_validated
    run onesided rdma_write           && t_rdma_write
    run onesided receiver_passive     && t_receiver_passive
    run onesided rdma_read            && t_rdma_read
    run onesided flag_notification    && t_flag_notification
    run onesided write_with_imm       && t_write_with_imm
    run onesided read_depth           && t_read_depth_reported
  fi
fi

if want_group efficiency; then
  [[ "$LIST" -eq 0 ]] && sec "efficiency"
  if [[ "$LIST" -eq 1 ]] || [[ "$HAVE_DEV" -eq 1 ]]; then
    run efficiency pp_report_config   && t_pp_report_config
    run efficiency inline_limit       && t_inline_limit_reported
    run efficiency moderation         && t_moderation_graceful
  fi
  if [[ "$LIST" -eq 1 ]] || need_peer; then
    run efficiency inline_correct     && t_inline_correct
    run efficiency signal_every       && t_signal_every_reduces
    run efficiency buffer_reuse       && t_buffer_reuse_correct
    run efficiency pipeline_clamped   && t_pipeline_clamped
    run efficiency pipeline_correct   && t_pipeline_correct
    run efficiency event_mode         && t_event_mode_completes
    run efficiency events_acked       && t_events_all_acked
    run efficiency empty_events       && t_empty_events_tolerated
    run efficiency race_guard         && t_race_guard_reported
    run efficiency broken_arming      && t_broken_arming
    run efficiency option_matrix      && t_option_matrix
  fi
fi

if want_group bench; then
  [[ "$LIST" -eq 0 ]] && sec "bench"
  if [[ "$LIST" -eq 1 ]] || need_peer; then
    run bench distribution        && t_bench_distribution
    run bench percentiles_ordered && t_bench_percentiles_ordered
    run bench json_samples        && t_bench_json_samples
    run bench warmup_applied      && t_bench_warmup_applied
    run bench modes_labelled      && t_bench_modes_labelled
    run bench response_schedule   && t_bench_response_schedule
    run bench bandwidth_units     && t_bench_bandwidth_units
    run bench variance            && t_bench_variance
    run bench size_sweep          && t_bench_size_sweep
    run bench option_sweep        && t_bench_option_sweep
  fi
fi

if want_group leaks; then
  [[ "$LIST" -eq 0 ]] && sec "leaks"
  if [[ "$LIST" -eq 1 ]] || [[ "$HAVE_DEV" -eq 1 ]]; then
    run leaks local && t_leaks_local
  fi
  if [[ "$LIST" -eq 1 ]] || need_peer; then
    run leaks transport && t_leaks_transport
    run leaks onesided  && t_leaks_onesided
    run leaks bench     && t_leaks_bench
  fi
fi

[[ "$LIST" -eq 1 ]] && exit 0

# ── Summary ───────────────────────────────────────────────────────────
echo ""
echo -e "${B}=========================================${N}"
echo -e "  ${G}${PASS} passed${N} / ${R}${FAIL} failed${N} / ${Y}${SKIP} skipped${N}"
echo -e "${B}=========================================${N}"

[[ "$FAIL" -ne 0 ]] && exit 1
[[ "$SKIP" -ne 0 ]] && exit 1
exit 0