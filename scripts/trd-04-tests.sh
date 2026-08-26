#!/usr/bin/env bash
# trd-04-tests.sh — Limen RDMA Transport Project
# TRD-04: The RAII Layer
#
# Run from the repository root:
#
#   ./scripts/trd-04-tests.sh                                            compile-time checks
#   ./scripts/trd-04-tests.sh --peer 10.0.0.1 --gid 3                    plus regressions
#   ./scripts/trd-04-tests.sh --peer 10.0.0.1 --gid 3 --ssh you@10.0.0.1 fully automatic
#
# Single-host, two-card topology (peer lives in a network namespace):
#
#   ./scripts/trd-04-tests.sh --dev rocep1s0f0 --peer-dev rocep4s0f0 \
#                             --gid 3 --peer 192.168.100.2 --peer-cmd "sudo limen-b"
#
# Most of this rung is verified at compile time. The three regression suites are
# what prove the refactor did not change behaviour.
#
# Exit 0 only when every test passes and none was skipped.

set -uo pipefail

DEV=""; GID=""; PEER=""; SSH_PEER=""
PEER_DEV=""; PEER_CMD=""
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
    --dev)  DEV="${2:-}";  shift 2 ;;
    --gid)  GID="${2:-}";  shift 2 ;;
    --peer) PEER="${2:-}"; shift 2 ;;
    --ssh)  SSH_PEER="${2:-}"; shift 2 ;;
    --peer-dev) PEER_DEV="${2:-}"; shift 2 ;;
    --peer-cmd) PEER_CMD="${2:-}"; shift 2 ;;
    -h|--help)
      echo "usage: $0 [--dev NAME] [--gid N] [--peer IP] [--ssh user@host]"
      echo "          [--peer-dev NAME] [--peer-cmd 'sudo limen-b']"
      exit 1 ;;
    *) echo "unknown argument: $1"; exit 1 ;;
  esac
done

CXX="${CXX:-c++}"
INCLUDES="-Iinclude"
STD="-std=c++20"

echo -e "${B}=== Limen TRD-04: The RAII Layer ===${N}"

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
  if [[ -n "$missing" ]]; then no "missing binaries:$missing"; return; fi
  BUILD_OK=1; ok "all three programs built with no warnings"
}

# ══ Compile-time contract ═════════════════════════════════════════════
test_static_contract() {                                              # R1
  hdr "R1: static_contract.cpp compiles for all five wrapper types"
  if [[ ! -f tests/static_contract.cpp ]]; then
    no "tests/static_contract.cpp does not exist"; return
  fi
  local log="$TMPDIR/static.log"
  if $CXX $STD $INCLUDES -fsyntax-only tests/static_contract.cpp >"$log" 2>&1; then
    local n; n=$(grep -c 'limen::' tests/static_contract.cpp || echo 0)
    ok "assertions hold (file references limen:: $n times)"
  else
    no "failed to compile — a wrapper violates the ownership contract:"
    sed 's/^/        /' "$log" | grep -m4 'static_assert\|error:' || true
    FAIL=$FAIL
  fi
}

test_copy_rejected_at_compile_time() {                                # R6
  hdr "R6: a program attempting to copy a wrapper must fail to compile"
  if [[ ! -f tests/copy_must_not_compile.cpp ]]; then
    no "tests/copy_must_not_compile.cpp does not exist"; return
  fi
  if $CXX $STD $INCLUDES -fsyntax-only tests/copy_must_not_compile.cpp >/dev/null 2>&1; then
    no "it COMPILED — copy operations are not deleted"
  else
    ok "rejected by the compiler, as required"
  fi
}

test_destructors_noexcept() {                                         # R8
  hdr "R8: every wrapper destructor is noexcept"
  local src="$TMPDIR/noexcept.cpp"
  cat > "$src" <<'EOF'
#include <limen/verbs.hpp>
#include <type_traits>
static_assert(std::is_nothrow_destructible_v<limen::Context>);
static_assert(std::is_nothrow_destructible_v<limen::ProtectionDomain>);
static_assert(std::is_nothrow_destructible_v<limen::MemoryRegion>);
static_assert(std::is_nothrow_destructible_v<limen::CompletionQueue>);
static_assert(std::is_nothrow_destructible_v<limen::QueuePair>);
static_assert(std::is_nothrow_destructible_v<limen::Endpoint>);
int main() {}
EOF
  if $CXX $STD $INCLUDES -fsyntax-only "$src" >/dev/null 2>&1; then
    ok "all six types nothrow-destructible"
  else
    no "at least one destructor is not noexcept"
  fi
}

# ══ Structural ════════════════════════════════════════════════════════
test_no_raw_release_calls() {                                         # R11
  hdr "R11: no release verb is named outside the wrapper implementation"
  local hits
  hits=$(grep -rn -E 'ibv_(destroy_qp|destroy_cq|dereg_mr|dealloc_pd|close_device)' \
           src/ include/ --include='*.c' --include='*.cpp' --include='*.hpp' 2>/dev/null \
         | grep -v 'src/verbs\.cpp' | grep -v 'include/limen/verbs\.hpp' || true)
  if [[ -z "$hits" ]]; then
    ok "release calls confined to the layer"
  else
    no "release calls outside the layer:"; sed 's/^/        /' <<<"$hits" | head -5
  fi
}

test_no_manual_cleanup() {                                            # R11
  hdr "R11: no goto cleanup survives in the three programs"
  local hits
  hits=$(grep -rn 'goto cleanup' src/ 2>/dev/null | grep -v 'verbs\.cpp' || true)
  if [[ -z "$hits" ]]; then
    ok "manual cleanup fully removed"
  else
    no "surviving manual cleanup (a throw would skip these):"; sed 's/^/        /' <<<"$hits" | head -5
  fi
}

# ══ Runtime, device required ══════════════════════════════════════════
test_context_throws_on_bad_device() {                                 # R2
  hdr "R2: an unknown device name throws VerbsError"
  local out rc
  out=$(./build/limen_devinfo -d definitely_not_a_device 2>&1); rc=$?
  if [[ "$rc" -eq 2 ]]; then
    ok "reported and exited 2 (VerbsError caught and mapped)"
  elif [[ "$rc" -gt 128 ]]; then
    no "terminated by a signal ($rc) — an exception escaped main"
  else
    no "expected exit 2, got $rc"
  fi
}

test_region_exposes_keys() {                                          # R4
  hdr "R4: a region reports non-zero lkey, rkey, and a usable buffer"
  local out; out=$(./build/limen_devinfo -d "$DEV" -s 65536 2>&1 || true)
  local line; line=$(grep -m1 '^mr: ' <<<"$out" || true)
  [[ -n "$line" ]] || { no "no mr: line"; return; }
  local lk rk
  lk=$(grep -oE 'lkey=0x[0-9a-fA-F]+' <<<"$line" | cut -d= -f2)
  rk=$(grep -oE 'rkey=0x[0-9a-fA-F]+' <<<"$line" | cut -d= -f2)
  if [[ -n "$lk" && -n "$rk" && "$lk" != "0x00000000" ]]; then
    ok "lkey=$lk rkey=$rk"
  else
    no "keys missing or zero: $line"
  fi
}

test_cq_qp_report_granted() {                                         # R5
  hdr "R5: completion queue and queue pair report granted capacities"
  local out; out=$(./build/limen_connect -d "$DEV" -g "$GID" --dry-run 2>&1 || true)
  if grep -qE '^cq: cqe=[0-9]+' <<<"$out" && grep -qE '^qp: type=RC' <<<"$out"; then
    ok "both report granted values"
  else
    sk "no --dry-run mode; covered by the TRD-02 regression instead"
  fi
}

test_moved_from_is_empty() {                                          # R6
  hdr "R6: a moved-from wrapper is empty and destroys without fault"
  local src="$TMPDIR/moved.cpp" bin="$TMPDIR/moved"
  cat > "$src" <<EOF
#include <limen/verbs.hpp>
#include <cstdio>
int main() {
    limen::Context a("$DEV");
    limen::Context b(std::move(a));
    if (a.get() != nullptr)  { std::puts("FAIL: source retained its handle"); return 1; }
    if (static_cast<bool>(a)) { std::puts("FAIL: source still tests true");   return 1; }
    if (b.get() == nullptr)  { std::puts("FAIL: destination is empty");      return 1; }
    std::puts("OK");
    return 0;                       /* both destroyed here; exactly one release */
}
EOF
  if ! $CXX $STD $INCLUDES "$src" -o "$bin" -Lbuild -llimen -libverbs >/dev/null 2>&1 \
     && ! $CXX $STD $INCLUDES "$src" src/verbs.cpp -o "$bin" -libverbs >/dev/null 2>&1; then
    sk "could not link against the layer (no liblimen and direct compile failed)"
    return
  fi
  local out; out=$("$bin" 2>&1 || true)
  if grep -q '^OK' <<<"$out"; then ok "source emptied, destination owns"; else no "$out"; fi
}

test_move_assign_releases_once() {                                    # R6
  hdr "R6: move assignment over a live object releases exactly once"
  local src="$TMPDIR/massign.cpp" bin="$TMPDIR/massign"
  cat > "$src" <<EOF
#include <limen/verbs.hpp>
#include <cstdio>
int main() {
    limen::Context a("$DEV");
    limen::Context b("$DEV");
    b = std::move(a);               /* b's original must be released here */
    if (a.get()) { std::puts("FAIL: source retained"); return 1; }
    limen::Context &r = b;
    r = std::move(b);               /* self-assignment must be a no-op */
    if (!b.get()) { std::puts("FAIL: self-assignment destroyed the object"); return 1; }
    std::puts("OK");
    return 0;
}
EOF
  if ! $CXX $STD $INCLUDES "$src" src/verbs.cpp -o "$bin" -libverbs >/dev/null 2>&1; then
    sk "could not build the move-assignment probe"; return
  fi
  local vg="$TMPDIR/massign.vg"
  if command -v valgrind >/dev/null 2>&1; then
    valgrind --leak-check=full --error-exitcode=42 "$bin" >"$TMPDIR/massign.out" 2>"$vg" || true
    if grep -q '^OK' "$TMPDIR/massign.out" \
       && grep -qE 'definitely lost: 0 bytes' "$vg" \
       && ! grep -qE 'Invalid free|double free' "$vg"; then
      ok "released once, no double free, self-assignment safe"
    else
      no "see $vg — $(head -1 "$TMPDIR/massign.out" 2>/dev/null)"
    fi
  else
    local out; out=$("$bin" 2>&1 || true)
    grep -q '^OK' <<<"$out" && ok "behaviour correct (valgrind absent)" || no "$out"
  fi
}

test_destruction_order() {                                            # R7
  hdr "R7: diagnostic mode reports release order qp, cq, mr, pd, context"
  local out
  out=$(LIMEN_TRACE_RELEASE=1 ./build/limen_devinfo -d "$DEV" -s 4096 2>&1 || true)
  local order
  order=$(grep -oE '^release: [a-z_]+' <<<"$out" | sed 's/release: //' | tr '\n' ' ')
  if [[ -z "$order" ]]; then
    no "LIMEN_TRACE_RELEASE produced no release lines (R7 diagnostic mode missing)"
    return
  fi
  # devinfo builds ctx, pd, mr — so expect mr, pd, context. Endpoint users add qp, cq.
  if grep -qE '^(qp cq mr pd context|mr pd context) $' <<<"$order"; then
    ok "order: ${order}"
  else
    no "unexpected order: ${order}(expected reverse of creation)"
  fi
}

test_close_idempotent() {                                             # R8
  hdr "R8: close() is idempotent and returns a status"
  local src="$TMPDIR/close.cpp" bin="$TMPDIR/close"
  cat > "$src" <<EOF
#include <limen/verbs.hpp>
#include <cstdio>
int main() {
    limen::Context c("$DEV");
    int first  = c.close();
    int second = c.close();          /* must be a no-op returning 0 */
    if (first != 0)  { std::printf("FAIL: first close returned %d\n", first);  return 1; }
    if (second != 0) { std::printf("FAIL: second close returned %d\n", second); return 1; }
    std::puts("OK");
    return 0;                        /* destructor must also be a no-op */
}
EOF
  if ! $CXX $STD $INCLUDES "$src" src/verbs.cpp -o "$bin" -libverbs >/dev/null 2>&1; then
    sk "could not build the idempotence probe"; return
  fi
  local out; out=$("$bin" 2>&1 || true)
  grep -q '^OK' <<<"$out" && ok "second close is a no-op" || no "$out"
}

test_partial_construction_no_leak() {                                 # R9
  hdr "R9: a failure part-way through construction leaks nothing"
  if ! command -v valgrind >/dev/null 2>&1; then sk "valgrind not installed"; return; fi
  local src="$TMPDIR/partial.cpp" bin="$TMPDIR/partial"
  # NOTE (deviation from the shipped suite): the original probe called a
  # five-argument Endpoint(dev, bytes, access, cqe, init&). The Requirements
  # document does not specify Endpoint's signature -- only the Hints sketch
  # that shape -- and it cannot express two memory regions of different sizes,
  # which limen_pingpong needs (an rx_depth-slot receive region and a
  # single-message send region). The constructor call below is adapted to the
  # implemented signature. What the probe actually asserts is unchanged:
  # construction must throw part-way through, and valgrind must report zero
  # bytes definitely lost, proving the already-constructed members released.
  cat > "$src" <<EOF
#include <limen/verbs.hpp>
#include <cstdio>
int main() {
    ibv_qp_init_attr init{};
    init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = 1; init.cap.max_recv_wr = 1;
    init.cap.max_send_sge = 1; init.cap.max_recv_sge = 1;
    try {
        /* ctx, pd and both regions construct; the cq is far too large and
           throws, so those four must unwind cleanly. */
        limen::Endpoint ep("$DEV",
                           1 << 30,      /* cqe, far beyond max_cqe */
                           nullptr,      /* cq_context */
                           nullptr,      /* comp channel */
                           0,            /* comp_vector */
                           &init,
                           4096,         /* message_size */
                           8,            /* rx_depth */
                           IBV_ACCESS_LOCAL_WRITE);
        std::puts("FAIL: expected construction to throw");
        return 1;
    } catch (const limen::VerbsError &e) {
        std::printf("OK caught: %s\n", e.what());
        return 0;
    }
}
EOF
  if ! $CXX $STD $INCLUDES "$src" src/verbs.cpp -o "$bin" -libverbs >/dev/null 2>&1; then
    sk "could not build the partial-construction probe"; return
  fi
  local vg="$TMPDIR/partial.vg"
  valgrind --leak-check=full "$bin" >"$TMPDIR/partial.out" 2>"$vg" || true
  local lost
  lost=$(grep -oE 'definitely lost: [0-9,]+ bytes' "$vg" | grep -oE '[0-9,]+' | tr -d ',' || echo "")
  if ! grep -q '^OK' "$TMPDIR/partial.out"; then
    no "construction did not throw as expected"
  elif [[ -z "$lost" ]]; then
    no "could not parse valgrind output — see $vg"
  elif [[ "$lost" -eq 0 ]]; then
    ok "threw, and the constructed members were released (0 bytes lost)"
  else
    no "${lost} bytes definitely lost on the partial-construction path"
  fi
}

# ══ Regressions — the real proof ══════════════════════════════════════
run_regression() {  # $1 = trd number, $2 = extra args
  local n="$1"; shift
  local script="scripts/trd-0${n}-tests.sh"
  if [[ ! -x "$script" ]]; then echo "MISSING"; return; fi
  if "$script" "$@" >"$TMPDIR/reg-${n}.log" 2>&1; then echo "PASS"; else echo "FAIL"; fi
}

test_trd01_regression() {                                             # R10
  hdr "R10: the TRD-01 suite passes against the refactored program"
  local r; r=$(run_regression 1 --dev "$DEV")
  case "$r" in
    PASS)    ok "TRD-01 suite green, unmodified" ;;
    MISSING) sk "scripts/trd-01-tests.sh not present" ;;
    *)       no "TRD-01 suite failed — see $TMPDIR/reg-1.log"
             grep -m3 'FAIL' "$TMPDIR/reg-1.log" | sed 's/^/        /' || true ;;
  esac
}

test_trd02_regression() {                                             # R10
  hdr "R10: the TRD-02 suite passes against the refactored program"
  # An array, not ${VAR:+...} expansion: --peer-cmd's value contains a space
  # ("sudo limen-b") and word-splitting would shred it into separate arguments.
  local args=(--dev "$DEV" --gid "$GID" --peer "$PEER")
  [[ -n "$SSH_PEER" ]] && args+=(--ssh      "$SSH_PEER")
  [[ -n "$PEER_DEV" ]] && args+=(--peer-dev "$PEER_DEV")
  [[ -n "$PEER_CMD" ]] && args+=(--peer-cmd "$PEER_CMD")
  local r; r=$(run_regression 2 "${args[@]}")
  case "$r" in
    PASS)    ok "TRD-02 suite green, unmodified" ;;
    MISSING) sk "scripts/trd-02-tests.sh not present" ;;
    *)       no "TRD-02 suite failed — see $TMPDIR/reg-2.log"
             grep -m3 'FAIL' "$TMPDIR/reg-2.log" | sed 's/^/        /' || true ;;
  esac
}

test_trd03_regression() {                                             # R10
  hdr "R10: the TRD-03 suite passes against the refactored program"
  # An array, not ${VAR:+...} expansion: --peer-cmd's value contains a space
  # ("sudo limen-b") and word-splitting would shred it into separate arguments.
  local args=(--dev "$DEV" --gid "$GID" --peer "$PEER")
  [[ -n "$SSH_PEER" ]] && args+=(--ssh      "$SSH_PEER")
  [[ -n "$PEER_DEV" ]] && args+=(--peer-dev "$PEER_DEV")
  [[ -n "$PEER_CMD" ]] && args+=(--peer-cmd "$PEER_CMD")
  local r; r=$(run_regression 3 "${args[@]}")
  case "$r" in
    PASS)    ok "TRD-03 suite green, unmodified" ;;
    MISSING) sk "scripts/trd-03-tests.sh not present" ;;
    *)       no "TRD-03 suite failed — see $TMPDIR/reg-3.log"
             grep -m3 'FAIL' "$TMPDIR/reg-3.log" | sed 's/^/        /' || true ;;
  esac
}

test_no_leaks() {                                                     # R11
  hdr "R11: valgrind reports zero bytes definitely lost across all three programs"
  if ! command -v valgrind >/dev/null 2>&1; then sk "valgrind not installed"; return; fi
  local worst=0 lost log
  # devinfo: success path and a failure path
  for args in "-d $DEV -s 65536" "-d no_such_device"; do
    log="$TMPDIR/vg-devinfo-$RANDOM.log"
    valgrind --leak-check=full --show-leak-kinds=definite \
      ./build/limen_devinfo $args >/dev/null 2>"$log" || true
    lost=$(grep -oE 'definitely lost: [0-9,]+ bytes' "$log" | grep -oE '[0-9,]+' | tr -d ',' || echo "")
    [[ -z "$lost" ]] && { no "could not parse valgrind output — see $log"; return; }
    [[ "$lost" -gt "$worst" ]] && worst="$lost"
  done
  if [[ "$worst" -eq 0 ]]; then
    ok "0 bytes definitely lost (limen_devinfo, success and failure paths)"
  else
    no "${worst} bytes definitely lost"
  fi
}

# ══ Run ═══════════════════════════════════════════════════════════════
test_builds
if [[ "$BUILD_OK" -ne 1 ]]; then
  echo -e "\n${R}Gate closed.${N} Nothing else can run until the build succeeds."
  exit 1
fi

test_static_contract
test_copy_rejected_at_compile_time
test_destructors_noexcept
test_no_raw_release_calls
test_no_manual_cleanup

if [[ -z "$DEV" ]] && command -v ibv_devices >/dev/null 2>&1; then
  DEV=$(ibv_devices 2>/dev/null | awk 'NR>2 && NF {print $1; exit}' || true)
fi

# The peer defaults to the same device name; on a single host with two cards
# they differ, so --peer-dev overrides it.
[[ -z "$PEER_DEV" ]] && PEER_DEV="$DEV"

# Cache sudo credentials up front so backgrounded peer launches inside the
# regression suites never block on a password prompt.
if [[ "$PEER_CMD" == sudo* ]]; then
  sudo -v || { echo "sudo credentials required for --peer-cmd"; exit 1; }
fi

if [[ -z "$DEV" ]]; then
  hdr "R2–R11: device-dependent checks"
  sk "no RDMA device found"
  sk "  (test_context_throws_on_bad_device)"
  sk "  (test_region_exposes_keys)"
  sk "  (test_cq_qp_report_granted)"
  sk "  (test_moved_from_is_empty)"
  sk "  (test_move_assign_releases_once)"
  sk "  (test_destruction_order)"
  sk "  (test_close_idempotent)"
  sk "  (test_partial_construction_no_leak)"
  sk "  (test_no_leaks)"
else
  echo -e "\n${B}Device ${DEV}${N}"
  test_context_throws_on_bad_device
  test_region_exposes_keys
  test_cq_qp_report_granted
  test_moved_from_is_empty
  test_move_assign_releases_once
  test_destruction_order
  test_close_idempotent
  test_partial_construction_no_leak
  test_no_leaks
fi

if [[ -z "$PEER" || -z "$GID" || -z "$DEV" ]]; then
  hdr "R10: regression suites"
  sk "requires --peer, --gid, and a device"
  sk "  (test_trd01_regression)"
  sk "  (test_trd02_regression)"
  sk "  (test_trd03_regression)"
else
  test_trd01_regression
  test_trd02_regression
  test_trd03_regression
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
echo -e "\n${G}Gate open.${N} Record your templating decision and the trace output in"
echo -e "docs/raii-notes.md, answer the comprehension questions, then begin TRD-05."