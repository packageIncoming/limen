#!/usr/bin/env bash
# trd-00-tests.sh — Limen RDMA Transport Project, TRD-00: Fabric Up
#
# Run from the repository root:
#
#   ./scripts/trd-00-tests.sh                                       setup check + local
#   ./scripts/trd-00-tests.sh --peer 10.0.0.1 --gid 3               full gate
#   ./scripts/trd-00-tests.sh --peer 10.0.0.1 --gid 3 --ssh you@10.0.0.1
#
# Setup (S1-S9) is verified as a PRECONDITION and reported separately.
# Only requirements (R1-R8) are graded.
#
# Exit 0 only when every requirement passes and none was skipped.

set -uo pipefail

DEV="rxe0"; IFACE=""; GID=""; PEER=""; SSH_PEER=""
PASS=0; FAIL=0; SKIP=0; SETUP_BAD=0

R=$'\033[0;31m'; G=$'\033[0;32m'; Y=$'\033[0;33m'; B=$'\033[1m'; N=$'\033[0m'

# CRITICAL: VAR=$((VAR + 1)). A bare ((VAR++)) returns the pre-increment value
# as its exit status, which is 1 when the counter is 0.
ok()    { echo -e "  ${G}PASS${N}  $1"; PASS=$((PASS + 1)); }
no()    { echo -e "  ${R}FAIL${N}  $1"; FAIL=$((FAIL + 1)); }
sk()    { echo -e "  ${Y}SKIP${N}  $1"; SKIP=$((SKIP + 1)); }
sok()   { echo -e "  ${G} ok ${N}  $1"; }
sbad()  { echo -e "  ${R}SETUP${N} $1"; SETUP_BAD=$((SETUP_BAD + 1)); }
hdr()   { echo -e "\n${B}$1${N}"; }

TMPDIR="$(mktemp -d)"
cleanup() {
  [[ -n "$SSH_PEER" ]] && ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH_PEER" \
      "pkill -f 'ibv_rc_pingpong|rping|ib_send_bw|ib_send_lat|iperf3'" >/dev/null 2>&1 || true
  rm -rf "$TMPDIR"
}
trap cleanup EXIT INT TERM

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dev)   DEV="${2:-}";   shift 2 ;;
    --iface) IFACE="${2:-}"; shift 2 ;;
    --gid)   GID="${2:-}";   shift 2 ;;
    --peer)  PEER="${2:-}";  shift 2 ;;
    --ssh)   SSH_PEER="${2:-}"; shift 2 ;;
    -h|--help)
      echo "usage: $0 [--dev rxe0] [--iface NAME] [--gid N] [--peer IP] [--ssh user@host]"
      exit 1 ;;
    *) echo "unknown argument: $1"; exit 1 ;;
  esac
done

srv() {   # start a server-side command on the peer, or prompt for it
  if [[ -n "$SSH_PEER" ]]; then
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH_PEER" \
        "nohup $1 >/dev/null 2>&1 & disown" >/dev/null 2>&1 || true
    sleep 2
  else
    echo "      on the peer:  $1" >&2
    read -rp "      press enter once it is running... " >&2
  fi
}
kill_srv() {
  [[ -n "$SSH_PEER" ]] && ssh -o BatchMode=yes "$SSH_PEER" "pkill -f '$1'" >/dev/null 2>&1 || true
}

echo -e "${B}=== Limen TRD-00: Fabric Up ===${N}"

# ═══════════════════════════════════════════════════════════════════════
#  SETUP PRECONDITIONS  (S1–S9) — verified, not graded
# ═══════════════════════════════════════════════════════════════════════
hdr "Setup preconditions (S1-S9) — not graded"

lsmod 2>/dev/null | grep -q '^rdma_rxe' \
  && sok "S6  rdma_rxe loaded" \
  || sbad "S6  rdma_rxe not loaded — sudo modprobe rdma_rxe"

lsmod 2>/dev/null | grep -q '^rdma_ucm' \
  && sok "S6  rdma_ucm loaded" \
  || sbad "S6  rdma_ucm not loaded — needed by R3"

rdma link show 2>/dev/null | grep -q "${DEV}/1" \
  && sok "S8  ${DEV}/1 exists" \
  || sbad "S8  ${DEV} missing — sudo rdma link add ${DEV} type rxe netdev <iface>"

DEVINFO=$(ibv_devinfo -d "$DEV" 2>/dev/null || true)
grep -q 'PORT_ACTIVE' <<<"$DEVINFO" \
  && sok "S9  PORT_ACTIVE" \
  || sbad "S9  port not active — wrong interface bound, or interface down"

grep -q 'link_layer:.*Ethernet' <<<"$DEVINFO" \
  && sok "S9  link_layer Ethernet" \
  || sbad "S9  unexpected link layer"

MISSING=""
for t in ibv_devinfo ibv_rc_pingpong rping ib_send_bw ib_send_lat rdma cmake git tcpdump iperf3; do
  command -v "$t" >/dev/null 2>&1 || MISSING="$MISSING $t"
done
[[ -z "$MISSING" ]] && sok "S5  all tools present" || sbad "S5  missing:$MISSING"

if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q 'Status: active' \
   && ! ufw status 2>/dev/null | grep -q '4791'; then
  sbad "S7  ufw active and 4791/udp not permitted"
else
  sok "S7  UDP 4791 not blocked locally"
fi

if [[ -n "$PEER" ]]; then
  ping -c2 -W2 "$PEER" >/dev/null 2>&1 \
    && sok "S4  peer $PEER reachable" \
    || sbad "S4  cannot reach $PEER — check the S1 adapter and the S3 address"
fi

if [[ "$SETUP_BAD" -ne 0 ]]; then
  echo ""
  echo -e "${R}Setup incomplete.${N} ${SETUP_BAD} precondition(s) unmet."
  echo "Fix these using the Setup section and its Troubleshooting table in the"
  echo "Requirements document. No requirement can be assessed until they pass."
  exit 1
fi
echo -e "  ${G}Setup complete.${N} Assessing requirements."

# ═══════════════════════════════════════════════════════════════════════
#  REQUIREMENTS  (R1–R8) — graded
# ═══════════════════════════════════════════════════════════════════════

test_gid_index_recorded() {                                          # R1
  hdr "R1: docs/fabric.md records a GID index with its type and reasoning"
  [[ -s docs/fabric.md ]] || { no "docs/fabric.md missing or empty"; return; }
  local problems=""
  grep -qiE 'gid[ _]?index|gid:' docs/fabric.md    || problems="$problems index"
  grep -qiE 'roce[ _]?v2'        docs/fabric.md    || problems="$problems type"
  # reasoning: some prose beyond the bare value
  local words; words=$(grep -icE 'roce v1|not routable|ipv6|because|reject' docs/fabric.md || echo 0)
  [[ "$words" -ge 1 ]] || problems="$problems reasoning"
  [[ -z "$problems" ]] && ok "index, type, and reasoning recorded" \
                       || no "missing:$problems"
}

test_raw_verbs_transfer() {                                          # R2
  hdr "R2: ibv_rc_pingpong completes against the peer"
  srv "ibv_rc_pingpong -d $DEV -g $GID"
  if timeout 30 ibv_rc_pingpong -d "$DEV" -g "$GID" "$PEER" >/dev/null 2>&1; then
    ok "raw verbs exchange completed (gid index $GID)"
  else
    no "failed — re-check the R1 index; a wrong index fails here without saying so"
  fi
  kill_srv "ibv_rc_pingpong"
}

test_cm_transfer() {                                                 # R3
  hdr "R3: rping completes 10 verified iterations"
  srv "rping -s -a $PEER -C 10"
  if timeout 40 rping -c -a "$PEER" -v -C 10 >/dev/null 2>&1; then
    ok "connection-manager exchange completed"
  else
    no "failed — check rdma_ucm is loaded and /dev/infiniband/rdma_cm exists"
  fi
  kill_srv "rping -s"
}

test_cm_difference_noted() {                                         # R3
  hdr "R3: docs/fabric.md notes what the CM path did not require"
  [[ -s docs/fabric.md ]] || { no "docs/fabric.md missing"; return; }
  if grep -qiE '\-g|gid index' docs/fabric.md \
     && grep -qiE 'rping|connection manager|resolv' docs/fabric.md; then
    ok "comparison paragraph present"
  else
    no "no note comparing the two invocations (R3 asks which argument was absent, and why)"
  fi
}

test_wire_capture() {                                                # R4
  hdr "R4: a capture confirms UDP 4791 traffic during a transfer"
  local ifc="$IFACE"
  if [[ -z "$ifc" ]]; then
    ifc=$(rdma link show 2>/dev/null | grep -oE 'netdev [a-z0-9]+' | awk '{print $2}' | head -1)
  fi
  [[ -n "$ifc" ]] || { no "could not determine the RDMA interface — pass --iface"; return; }
  local cap="$TMPDIR/cap.txt"
  ( sudo -n timeout 25 tcpdump -i "$ifc" -n udp port 4791 -c 10 >"$cap" 2>/dev/null ) &
  local tpid=$!
  sleep 2
  srv "ib_send_bw -d $DEV"
  timeout 20 ib_send_bw -d "$DEV" "$PEER" >/dev/null 2>&1 || true
  kill_srv "ib_send_bw"
  wait "$tpid" 2>/dev/null || true
  local n; n=$(grep -c '4791' "$cap" 2>/dev/null || echo 0)
  if [[ "$n" -gt 0 ]]; then
    ok "$n packet(s) captured on $ifc, UDP port 4791"
  elif [[ ! -s "$cap" ]] && ! sudo -n true 2>/dev/null; then
    sk "tcpdump needs sudo without a password prompt; run the R4 capture by hand"
  else
    no "no 4791 traffic captured on $ifc during a transfer"
  fi
}

test_capture_explained() {                                           # R4
  hdr "R4: docs/fabric.md explains why this capture fails on hardware"
  [[ -s docs/fabric.md ]] || { no "docs/fabric.md missing"; return; }
  if grep -qiE 'hardware|offload|bypass|ibdump|mirror' docs/fabric.md; then
    ok "explanation present"
  else
    no "no note on why a hardware adapter's traffic would not appear in this capture"
  fi
}

test_bandwidth_measured() {                                          # R5
  hdr "R5: ib_send_bw returns a positive average bandwidth"
  srv "ib_send_bw -d $DEV"
  local out bw
  out=$(timeout 120 ib_send_bw -d "$DEV" "$PEER" 2>/dev/null || true)
  bw=$(awk '/^[[:space:]]*[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9.]+[[:space:]]+[0-9.]+/ {print $4; exit}' <<<"$out")
  kill_srv "ib_send_bw"
  if [[ -n "$bw" ]] && awk "BEGIN{exit !($bw > 0)}" 2>/dev/null; then
    ok "average bandwidth ${bw} MB/sec"
  else
    no "no bandwidth figure parsed"
  fi
}

test_baseline_recorded() {                                           # R5
  hdr "R5: docs/baseline-rxe.md records a bandwidth and a latency figure"
  [[ -s docs/baseline-rxe.md ]] || { no "docs/baseline-rxe.md missing or empty"; return; }
  local bw=0 lat=0
  grep -qiE '(bw|bandwidth).*[0-9]'              docs/baseline-rxe.md && bw=1
  grep -qiE '(lat|latency|median|t_med|p99).*[0-9]' docs/baseline-rxe.md && lat=1
  [[ "$bw" -eq 1 && "$lat" -eq 1 ]] && ok "both figures present" \
                                    || no "bandwidth:$bw latency:$lat — both required"
}

test_tcp_comparison_recorded() {                                     # R6
  hdr "R6: a TCP measurement is recorded beside the RDMA figures"
  [[ -s docs/baseline-rxe.md ]] || { no "docs/baseline-rxe.md missing"; return; }
  if grep -qiE 'iperf|tcp' docs/baseline-rxe.md \
     && grep -qiE '(iperf|tcp).*[0-9]' docs/baseline-rxe.md; then
    ok "TCP figure recorded"
  else
    no "no iperf3/TCP measurement recorded"
  fi
}

test_comparison_explained() {                                        # R6
  hdr "R6: the comparison carries a written explanation"
  [[ -s docs/baseline-rxe.md ]] || { no "docs/baseline-rxe.md missing"; return; }
  local sec; sec=$(grep -iA8 -E 'comparison|vs\.? tcp|versus tcp' docs/baseline-rxe.md || true)
  local words; words=$(wc -w <<<"$sec")
  if [[ "$words" -ge 25 ]]; then
    ok "explanation present (${words} words)"
  else
    no "explanation missing or too thin (${words} words; R6 asks for 2-4 sentences)"
  fi
}

test_device_attrs_recorded() {                                       # R7
  hdr "R7: all nine named device attributes appear in the baseline"
  [[ -s docs/baseline-rxe.md ]] || { no "docs/baseline-rxe.md missing"; return; }
  local missing="" f
  for f in max_qp max_qp_wr max_cq max_cqe max_mr max_sge max_qp_rd_atom active_mtu gid_tbl_len; do
    grep -q "$f" docs/baseline-rxe.md || missing="$missing $f"
  done
  [[ -z "$missing" ]] && ok "all nine present" || no "missing:$missing"
}

test_fabric_documented() {                                           # R7
  hdr "R7: docs/fabric.md records the interface, addresses, and recreation commands"
  [[ -s docs/fabric.md ]] || { no "docs/fabric.md missing"; return; }
  local problems=""
  grep -qE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' docs/fabric.md || problems="$problems addresses"
  grep -qE 'enp|eth|ens'                    docs/fabric.md || problems="$problems interface"
  grep -qE 'rdma link add'                  docs/fabric.md || problems="$problems recreate-cmds"
  grep -qiE 'ufw|firewall'                  docs/fabric.md || problems="$problems firewall-note"
  [[ -z "$problems" ]] && ok "fabric fully documented" || no "missing:$problems"
}

test_repo_layout() {                                                 # R8
  hdr "R8: repository layout and initial commit"
  local missing="" p
  for p in CMakeLists.txt README.md .gitignore docs docs/fabric.md docs/baseline-rxe.md \
           include/limen src scripts tests; do
    [[ -e "$p" ]] || missing="$missing $p"
  done
  [[ -n "$missing" ]] && { no "missing:$missing"; return; }
  git rev-parse --git-dir >/dev/null 2>&1 || { no "not a git repository"; return; }
  git rev-parse HEAD >/dev/null 2>&1 \
    && ok "layout complete, repository has a commit" \
    || no "repository has no commits yet"
}

test_cmake_configures() {                                            # R8
  hdr "R8: cmake locates libibverbs and librdmacm"
  local log="$TMPDIR/cmake.log"
  if ! cmake -S . -B build >"$log" 2>&1; then
    no "configure failed — see $log"; return
  fi
  if grep -q 'libibverbs:.*/' "$log" && grep -q 'librdmacm:.*/' "$log"; then
    ok "configured; both libraries located"
  else
    no "configured, but the status messages did not report both library paths"
  fi
}

# ── Run ───────────────────────────────────────────────────────────────
test_gid_index_recorded
test_cm_difference_noted
test_capture_explained
test_baseline_recorded
test_tcp_comparison_recorded
test_comparison_explained
test_device_attrs_recorded
test_fabric_documented
test_repo_layout
test_cmake_configures

if [[ -z "$PEER" || -z "$GID" ]]; then
  hdr "R2-R5: two-node checks"
  sk "requires --peer and --gid"
  sk "  (test_raw_verbs_transfer)"
  sk "  (test_cm_transfer)"
  sk "  (test_wire_capture)"
  sk "  (test_bandwidth_measured)"
else
  test_raw_verbs_transfer
  test_cm_transfer
  test_wire_capture
  test_bandwidth_measured
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
echo -e "\n${G}Gate open.${N} Answer the comprehension questions, then begin TRD-01."
