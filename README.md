<img src="assets/limen-social-preview.png" alt="Limen">

# Limen — RDMA Transport Project  

A nine-rung self-teaching ladder that builds an RDMA transport library in C++20
over RoCEv2, from an empty machine to a measured, validated benchmark report.

Every rung is five documents:

| Document | Purpose |
|---|---|
| `prior-knowledge.html` | The chapter. Read first, completely. |
| `requirements.html` | The contract. What must exist when you are done. |
| `hints.html` | Two-tier guidance, one section per requirement. Not read straight through. |
| `tests.sh` | The gate. Green means done. |
| `comprehension.html` | Questions, then a full answer key. Answer before turning the page. |

## The ladder

| TRD | Title | Builds | Difficulty | Est. |
|---|---|---|---|---|
| 00 | Fabric Up | Verified soft-RoCE fabric, baseline, TCP control, wire capture | Low | 1–2 |
| 01 | Context, Domain, Memory | Device opened, memory registered, first verbs code | Medium | 3–4 |
| 02 | Queue Pairs | A connected queue pair, driven by hand through its state machine | High | 4–6 |
| 03 | Two-Sided Transfer | SEND and RECV moving real bytes | Medium-High | 3–5 |
| 04 | The RAII Layer | C++ ownership over every verbs resource | Medium | 3–5 |
| 05 | Connection Management | Connection setup by address, via the RDMA CM | Medium-High | 4–5 |
| 06 | One-Sided Operations | RDMA_WRITE and RDMA_READ against remote memory | Medium | 3–4 |
| 07 | Completion Efficiency | Inline, unsignaled, moderated, event-driven | High | 4–6 |
| 08 | Benchmark Harness | Defensible latency and bandwidth measurement | Medium | 3–4 |

A session is a focused block of roughly two to three hours.

## Setup

TRD-00's Requirements document opens with a **Setup** section — nine numbered
steps with exact commands, covering hypervisor adapters through to a live
`rxe0` device, plus a troubleshooting table. Setup is a prerequisite, not an
assessed requirement.

## Working through a rung

1. Read Prior Knowledge start to finish before writing code.
2. Read Requirements. Note which ones you cannot yet picture implementing.
3. Implement. Consult Hints at Level 1 only after 15 minutes on a specific blocker.
4. Run the test suite until green.
5. Answer the comprehension questions in writing, then check the key.
6. Commit. Continue.

## Running the suites

From the repository root on node A:

```
./scripts/trd-0N-tests.sh --peer 10.0.0.2 --gid <idx> --ssh you@10.0.0.2
```

The `--ssh` flag lets node A start the server side on node B automatically.
Without it, the suite prompts you to start each server command by hand.

## Artifacts the ladder produces

- `docs/fabric.md` — GID index and its justification, transfer results, wire capture
- `docs/baseline-rxe.md` — soft-RoCE measurements, TCP comparison, device attributes
- `docs/bringup-log.md` — a full queue-pair bringup from both nodes
- `docs/transfer-log.md` — four captures including both RNR failure modes
- `docs/raii-notes.md` — the templating decision and destruction-order trace
- `docs/cm-notes.md` — event traces and the imposed RNR timer note
- `docs/onesided-notes.md` — the three notification mechanisms compared
- `docs/efficiency-notes.md` — the arming-race counter and what it establishes
- `docs/benchmark-report.md` — **the final artifact**
