<p align="center">
  <img src="assets/limen-logo-dark-trim.png" alt="Limen" width="600">
</p>

<p align="center">
  <b>An RDMA transport over RoCEv2, and a harness that measures it honestly.</b>
</p>

<p align="center">
  C++20 &middot; libibverbs &middot; librdmacm &middot; CMake
</p>

---

**3.87 µs round trip at 2 B. 20.4 Gbit/s at 1 MB, 81.6% of a 25 Gbit link.
Measured against a 0.21% run-to-run noise floor on ConnectX-4 Lx.**

Every figure below carries its conditions and its noise floor. Raw samples for every run
cited here are in [`docs/results/`](docs/results/). Full methodology is in
[`docs/benchmark-report.md`](docs/benchmark-report.md).

---

## Contents

- [What this is](#what-this-is)
- [Build](#build)
- [Quick start](#quick-start)
- [Architecture](#architecture)
- [Benchmark harness](#benchmark-harness)
- [Conditions](#conditions)
- [Results](#results)
- [Validation](#validation)
- [Findings](#findings)
- [What this does not show](#what-this-does-not-show)
- [Repository layout](#repository-layout)

---

## What this is

Remote Direct Memory Access lets one machine read and write another machine's memory
without involving either kernel on the data path. The application registers memory with
the adapter, posts work requests to a queue pair, and reads completions from a completion
queue. No syscall, no copy, no interrupt in the steady state.

Limen implements that path from the verbs layer up:

- Reliable connected queue pairs with a `librdmacm` connection manager
- Two-sided `send`/`recv` and one-sided `write`/`read`
- An RAII ownership layer (`libLimen`) separating verbs objects from connection lifecycle
- Five tunable mechanisms: inline data, send signalling period, pipeline depth,
  poll versus event-driven completion reaping, and CQ moderation
- `limen_bench`, a measurement harness built to the discipline in
  [*How NOT to Measure Latency*](#references)

It is a learning artifact, not a production library. The
[limits](#what-this-does-not-show) are stated.

<!-- TODO(mert): one paragraph on why you built it and what "limen" means. -->

### The ladder

The project was built as nine gated milestones, TRD-00 through TRD-08, each with prior
knowledge, testable requirements, and comprehension questions. TRD-08 is the harness and
the report; it is the artifact the rest was building toward.

<!-- TODO(mert): table of the nine rungs, one line each. -->

---

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

AddressSanitizer build: `-DLIMEN_ASAN=ON`.

Requires `libibverbs`, `librdmacm`, and a RoCEv2-capable adapter or `rdma_rxe`.

| binary | purpose |
|---|---|
| `limen_devinfo` | device and port attribute dump |
| `limen_connect` | connection manager bringup, no data path |
| `limen_pingpong` | two-sided send/recv with payload verification |
| `limen_onesided` | RDMA write and read |
| `limen_bench` | measurement harness |

---

## Quick start

Two endpoints. Here they are two network namespaces on one host; two machines work the
same way.

```sh
# server
./build/limen_bench -d rocep1s0f0 -s 2 --inline -n 100000

# client
./build/limen_bench -d rocep4s0f0 -s 2 --inline \
    --mode latency -n 100000 --warmup 1000 --runs 9 192.168.100.1
```

```
report: quantity=service_time
percentile:   min    50     90     99     99.9
latency (us): 2.88   3.87   3.96   5.60   8.97
mean (us):    3.93   max (us): 504.27 stddev (us): 1.64
median/mean:  0.985
```

<!-- TODO(mert): note the limen-b namespace wrapper and limen-fabric.service, or
     drop the single-host detail entirely and describe a two-machine setup. -->

---

## Architecture

```
libLimen
├── verbs.hpp/cpp     Context, ProtectionDomain, MemoryRegion,
│                     CompletionQueue, CompletionChannel. 1:N capable.
├── cm.hpp/cpp        EventChannel, ConnectionId, Event. 1:N capable.
└── session.hpp/cpp   PendingConnection -> Session. 1:1 convenience type.

app layer (not part of libLimen)
├── harness.cpp       can_post, post_one, handle_wc, drain, reap
└── limen_bench.cpp   modes, sweeps, statistics, reporting
```

### The connection state machine

`rdma_cm` forces resource creation into a specific window: after `id->verbs` becomes
valid, before `rdma_connect` or `rdma_accept`. `PendingConnection` is that window made
into a type, with its own set of legal operations. `finish() &&` consumes it and moves
every resource into a `Session`.

<!-- TODO(mert): why the window exists, and what goes wrong if you create the PD
     or MRs outside it. -->

### Memory

Send and receive regions are slot rings sized at session creation. `wr_id` carries the
operation sequence number plus a tag bit distinguishing send from receive completions.

Registered memory is pinned and the adapter holds its address, so the backing allocation
must never move.

<!-- TODO(mert): the concrete consequence. why std::vector is prohibited here. -->

### Flow control

`can_post` gates on two independent quantities:

```
posted - covered      < eff_pipeline       // send ring slot safety
posted - recv_count   < max_outstanding    // requests offered to the peer
```

The first protects the send ring: a slot cannot be reused until the adapter has read it,
and the send completion is the only signal that this has happened. The second bounds how
much work is outstanding at the peer. Bandwidth mode sets `max_outstanding = 0` to
disable it.

These are different resources, and conflating them is a real defect this project shipped
and then found by measuring. See [Findings](#findings).

<!-- TODO(mert): what corruption looks like if the first clause is removed.
     Name the buffer and the sequence. -->

### Completion reaping

`--reap poll` drains the CQ in a loop. `--reap event` arms the CQ with
`ibv_req_notify_cq`, drains again to close the arm/completion race, then blocks on the
completion channel fd. Events are acked in batches of 64, with the remainder acked before
teardown because `ibv_destroy_cq` blocks on unacked events.

<!-- TODO(mert): the race, and why the second drain closes it. The --broken-arming
     flag exists to demonstrate this; say what it does. -->

---

## Benchmark harness

```
./build/limen_bench -d <device> [-t <port>]
        --mode <latency|response|bandwidth|sweep>
        [-s <bytes>] [-n <iters>] [--warmup <n>] [--runs <n>]
        [--rate <ops_per_sec>] [--op <send|write|read>]
        [--inline] [--signal-every <n>] [--pipeline <depth>]
        [--reap <poll|event>] [--moderate <count:usec>]
        [--json <path>] [--clock-floor] <peer>
```

| mode | measures |
|---|---|
| `latency` | closed loop, one operation outstanding, timed from issue. **Service time.** |
| `response` | open loop at `--rate`, timed from the **intended** send time. **Response time.** |
| `bandwidth` | sustained bytes over elapsed time with operations in flight. |
| `sweep` | size sweep 64 B to 1 MB, plus the five-switch option matrix. |

Exit codes follow TRD-07, plus **9** for a validation discrepancy beyond the declared
tolerance.

### Service time versus response time

A closed loop with one operation outstanding measures how long the system takes to serve a
request when nothing else is in flight. It is a legitimate number and it is what the
reference tools report. It is not the latency a client experiences under load, because the
benchmark stops issuing work exactly when the system is least able to handle it. Every
stall is recorded as one bad sample instead of the thousands of delayed requests it
actually caused.

`--mode response` fixes each operation's intended send time before the loop starts, as
`t_start + i × interval`, and measures from that instant rather than from when the send
actually happened. When the harness falls behind it does not sleep. The lateness is the
measurement.

Both modes are reported and labelled distinctly. The difference between them at the same
message size is in [Results](#coordinated-omission).

### Method

- `clock_gettime(CLOCK_MONOTONIC)`. `CLOCK_REALTIME` can step and produce negative
  durations. The floor is measured before every run as the median of 10,000 consecutive
  reads after 1,000 discarded.
- Every sample is retained and sorted once at the end. Nothing is summarised
  incrementally.
- `--warmup` operations are posted and completed but not recorded. The measured window
  opens when the warmup-th operation **retires**, not at connect.
- `--runs n` repeats the entire measurement, reconnect included. The noise floor is the
  range of the per-run medians divided by their mean.
- Under pipelining, a single reap can return several completions. Operation *i*'s start
  time is stored at post and retired in post order, valid because RC preserves ordering on
  one queue pair.

### Reproducing

```sh
lscpu -e=CPU,CORE                       # pick CPUs with different CORE values
sudo cpupower frequency-set -g performance
sudo cpupower idle-set -D 0
```

```sh
# server: launch at the largest size in the sweep, at matching depth
taskset -c 1 ./build/limen_bench -d rocep1s0f0 -s 1048576 --pipeline 64 -n 1000000000

# client
taskset -c 2 ./build/limen_bench -d rocep4s0f0 --mode sweep -s 4096 \
    -n 20000 --warmup 2000 --runs 9 --json docs/results/sweep-4096b.json 192.168.100.1
```

The server reflects `wc.byte_len` rather than a fixed size, so one invocation serves every
cell of a size sweep.

<!-- TODO(mert): the full command list for every run cited below, or point at
     scripts/trd-08-tests.sh. -->

---

## Conditions

| | |
|---|---|
| date | 2026-09-12 |
| OS | Ubuntu 24.04.4 LTS |
| kernel | Linux 7.0.0-30-generic |
| CPUs | 8, governor `performance`, C-states disabled |
| clocksource | `tsc`, floor 16 to 29 ns |
| device | ConnectX-4 Lx, fw 14.27.6122 |
| `max_qp_wr` / `max_cqe` | 8192 / 4194303 |
| granted `max_inline_data` | 316 B |
| link | 25G SFP28 DAC, active MTU 1024 |
| topology | one host, two namespaces, endpoints pinned to separate physical cores |

The clock floor is 0.6 to 1.0% of the smallest quantity measured (2.88 µs), inside the 2%
threshold at which it would need a stated error bar.

---

## Results

### Latency, 2 B inline, depth 1, poll

```
min 2.88   p50 3.87   p90 3.96   p99 5.60   p99.9 8.97   (µs)
mean 3.93  max 504.27  stddev 1.64
median/mean 0.985
```

Noise floor **1.13%** over 9 runs.

The median and mean agree to 1.5%, so the body of the distribution is flat. The tail is
thin through p99 (1.45x the median) and widens at p99.9 (2.3x). One sample in roughly
900,000 landed at 504 µs.

<!-- TODO(mert): what produces the p99.9, and what produces the 504 µs outlier.
     Same cause or different? -->

### Size sweep

Latency at depth 1, bandwidth at depth 64, all other parameters held constant.

| size | med (µs) | p90 (µs) | p99 (µs) | p99.9 (µs) | Gbit/s |
|---:|---:|---:|---:|---:|---:|
| 64 B | 4.32 | 5.55 | 7.79 | 12.00 | 0.26 |
| 128 B | 4.47 | 5.69 | 7.75 | 12.04 | 0.52 |
| 256 B | 5.74 | 5.91 | 8.03 | 13.07 | 0.99 |
| 512 B | 6.02 | 6.28 | 8.71 | 14.00 | 1.88 |
| 1 KB | 6.80 | 7.81 | 10.25 | 14.09 | 3.30 |
| 2 KB | 7.63 | 8.71 | 11.74 | 16.82 | 5.27 |
| 4 KB | 9.40 | 9.67 | 16.09 | 23.67 | 7.66 |
| 8 KB | 12.31 | 12.78 | 22.67 | 30.29 | 10.94 |
| 16 KB | 17.63 | 19.01 | 32.61 | 43.47 | 13.80 |
| 32 KB | 28.90 | 31.63 | 47.61 | 62.15 | 16.64 |
| 64 KB | 52.44 | 62.50 | 87.93 | 109.27 | 17.90 |
| 128 KB | 97.28 | 115.29 | 131.00 | 152.86 | 18.81 |
| 256 KB | 188.86 | 209.71 | 236.76 | 282.59 | 20.32 |
| 512 KB | 374.88 | 394.29 | 430.62 | 776.47 | 20.16 |
| 1 MB | 749.98 | 755.21 | 760.41 | 791.03 | **20.41** |

Noise floor **0.21%** over 9 runs. Reproduced within 1% at every size across two
independent sweeps.

Taking the two ends of the curve:

| quantity | value | from |
|---|---|---|
| fixed per-operation cost | 4.32 µs | 64 B median |
| marginal byte cost | 0.715 ns/B | (749.98 − 374.88) / 524288 |
| marginal rate at depth 1 | 11.2 Gbit/s | 1 / 0.715 ns/B |
| crossover | ~6 KB | 4.32 µs / 0.715 ns/B |

Below 6 KB the fixed cost dominates and payload size is nearly free. Above it the wire
dominates and the curve is linear in bytes.

<!-- TODO(mert): where the 4.32 µs goes. Name components with estimates:
     doorbell, PCIe, wire, peer turnaround, CQE DMA, poll detection. -->

<!-- TODO(mert): 128 B to 256 B jumps 28% while its neighbours jump 3% and 5%.
     Reproduced in both sweeps. Active MTU is 1024. Hypothesis and the test. -->

### Option sweep

Baseline: depth 16, signalled every send, inline off, poll reaping. One switch changed per
row.

| option | 64 B (floor 0.28%) | 4 KB (floor 0.21%) |
|---|---:|---:|
| `--inline` | +0.4% | +0.1% <sup>1</sup> |
| `--signal-every 16` | +0.0% | −3.0% |
| `--pipeline 1` | −86.1% | −86.8% <sup>2</sup> |
| `--reap event` | −1.7% | −13.4% |
| `--op write` | not implemented | not implemented |

<sup>1</sup> 4096 B exceeds the granted `max_inline_data` of 316 B, so inline silently did
not apply. Only the 64 B column measures inlining.

<sup>2</sup> Not an 86% improvement. Depth 16 at 4 KB: 71.43 µs, 224k ops/s. Depth 1:
9.40 µs, 106k ops/s. Sixteen times the depth buys 2.1x the throughput for 7.6x the
latency.

<!-- TODO(mert): inline applied at 64 B and produced nothing measurable, against a
     0.28% floor, on hardware. State it as a finding or explain it. -->

<!-- TODO(mert): event beat poll, and the margin grows with message size.
     Both endpoints were pinned so CPU contention is not the explanation.
     Hypothesis, competing explanation, and the experiment that separates them. -->

### Coordinated omission

Capacity at 2 B is 1 / 3.87 µs ≈ 258,000 ops/s.

| offered rate | p50 | p90 | p99.9 | max lateness |
|---|---:|---:|---:|---:|
| 140k/s (54% of capacity) | 3.98 µs | 4.21 µs | 18.11 µs | 28 µs |
| 400k/s (155% of capacity) | 80,145 µs | 142,795 µs | 158,350 µs | 158,498 µs |

Closed-loop latency mode reports 3.87 µs regardless of which of these rates a client would
have been offering, because it never offers more than one operation at a time.

Both figures are correct measurements of different quantities.

<!-- TODO(mert): explain to a skeptic why both are correct. -->

Completion batch size was 1.00 mean and 1 max at both rates, so the one-timestamp-per-reap
bias noted in [limits](#what-this-does-not-show) never fired in these runs.

### Against TCP

Carried forward from TRD-00.

<!-- TODO(mert): the TCP round trip from TRD-00 R6, rerun with a distribution
     beside it rather than a single figure. Table it against the 2 B RDMA row. -->

---

## Validation

Measured against `ib_send_lat` and `ib_send_bw` from `linux-rdma/perftest` in a matched
configuration: same device, same size, same iteration count, RC transport.

**The reference reports half the round trip.** `print_report_lat` in
`perftest_parameters.c` sets `rtt_factor = 2` for SEND and divides every printed column by
it. Determined from source rather than assumed; assuming wrongly produces a clean factor
of two error across an entire report.

Declared tolerance: **25%**.

| | perftest (×2) | Limen | delta |
|---|---:|---:|---:|
| latency p50, 2 B | 2.48 µs | 3.87 µs | **+56.0%** |
| latency min | 2.24 µs | 2.88 µs | +28.6% |
| latency p99 | 4.80 µs | 5.60 µs | +16.7% |
| latency p99.9 | 8.34 µs | 8.97 µs | +7.6% |
| bandwidth, 64 KB, depth 64 | 22.19 Gbit/s | 18.67 Gbit/s | −15.9% |

**Bandwidth passes. Latency fails and the harness exits 9.**

The latency case is not like-for-like, and this is recorded rather than explained away.
`ib_send_lat` at depth 1 posts sends **unsignaled** and uses a **separate receive
completion queue**. Limen signals sends because `can_post` uses send completions for
send-ring flow control, and shares one CQ between send and receive. Both are consequences
of supporting pipelining, one-sided operations, and event-driven reaping, none of which
the reference latency binary implements.

<!-- TODO(mert): estimate how much each of the two contributes, or name the
     experiment that separates them. -->

<!-- TODO(mert): the gap is 56% at the median, 29% at the min, 8% at p99.9.
     A fixed per-operation cost would shift all three by the same absolute
     amount. What does that shape actually mean? -->

---

## Findings

Each traceable to a table above.

**1. Cost is fixed below ~6 KB and linear above it.**
4.32 µs per operation regardless of size, plus 0.715 ns per byte. The crossover falls at
6 KB and the measured curve turns where the arithmetic says it should.

**2. Peak throughput is 20.4 Gbit/s, 81.6% of line rate, and it requires depth.**
At depth 1 the marginal rate is 11.2 Gbit/s. Keeping operations in flight is worth 1.8x.

**3. Pipelining trades latency for throughput at a poor exchange rate here.**
Sixteen in flight buys 2.1x the operations per second and costs 7.6x the response time.

**4. `--pipeline` bounded outstanding sends, not outstanding requests.**
A send completes when the adapter has read the buffer, not when the reply arrives. Setting
depth to 16 therefore let the client offer thousands of requests at once and produced an
18 ms median, until `max_outstanding` was added as a separate bound. Found by measuring,
not by reading the code.

**5. Inline data produced no measurable effect where it applied.**
+0.4% at 64 B against a 0.28% floor, on hardware, at the message size where inlining is
supposed to matter most.

**6. Event-driven reaping beat polling, and the margin grows with message size.**
−1.7% at 64 B, −13.4% at 4 KB, with both endpoints pinned to separate physical cores.

<!-- TODO(mert): 5 and 6 both need a mechanism or an explicit "unexplained".
     Do not leave them as bare numbers in the finished report. -->

**7. The noise floor is 0.21% to 1.57% depending on the measurement.**
No difference smaller than the floor for its own measurement is claimed anywhere above.

---

## What this does not show

- **`--op write` and `--op read` are parsed and ignored.** `post_one` emits `IBV_WR_SEND`
  only, so the two-sided versus one-sided row of the option sweep is absent. One-sided
  operations are what RDMA is principally for, and they are not measured here.
- **Both endpoints share one host.** No switch, no cross-host PCIe path, no fabric
  congestion. A two-machine rerun of this harness with no code changes is the single
  highest-value follow-up.
- **One timestamp per completion batch.** Under pipelining, every completion reaped in one
  call shares the timestamp taken after that call returned, which biases the earliest
  member upward. Mean batch size was 1.00 in every run cited, so it never fired, but it
  would under heavier load. True per-completion arrival requires
  `IBV_WC_EX_WITH_COMPLETION_TIMESTAMP`, which the transport does not use.
- **`behind` in response mode is not usable.** It counts every operation posted later than
  its exact intended nanosecond, which is nearly all of them. `max_late_us` is the figure
  to read.
- **`verdict()` has no margin.** Any delta above the noise floor is marked significant, so
  a result at 1.4x the floor over 9 samples is reported the same way as one at 60x.
- **The `--warmup` default of 1000 follows the reference tools' convention** and has not
  yet been justified from the per-iteration ramp captured in
  `docs/results/warmup-ramp.json`.
- **Single connection, single queue pair.** No shared receive queue, no atomics, no memory
  windows, no multi-connection event loop.
- **No cycle counters, cache-miss profiling, or flame graphs.** This establishes *whether*,
  not *why*. Several findings above are therefore stated as hypotheses.

<!-- TODO(mert): one paragraph on what would change on a two-machine fabric and
     why, labelled clearly as expectation rather than data. -->

---

## Repository layout

```
include/limen/          libLimen public headers
include/limen/app/      application layer headers
src/                    library and binary sources
tests/                  static contract and compile-fail tests
scripts/                per-TRD acceptance tests
docs/
├── benchmark-report.md full methodology and findings
└── results/            JSON for every run cited here
```

---

## References

1. Tene, Gil. *How NOT to Measure Latency.* The coordinated omission problem `--mode
   response` exists to avoid.
2. `clock_gettime(2)`, Linux manual page section 2.
3. `ib_send_lat(1)`, `ib_send_bw(1)` — [linux-rdma/perftest](https://github.com/linux-rdma/perftest).
   The reference implementation validated against.
4. NVIDIA, *RDMA Aware Networks Programming User Manual.*

<!-- TODO(mert): license, and a contact line if you want one. -->