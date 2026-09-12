# Limen benchmark report

Measurements of the Limen RDMA transport on ConnectX-4 Lx over a 25G link, validated
against `ib_send_lat` and `ib_send_bw` from `linux-rdma/perftest` in a matched
configuration.

All figures below come from a single suite run on 2026-09-12. Raw JSON for every run is in
[`results/`](results/).

---

## Contents

- [Benchmark components](#benchmark-components)
- [How the benchmark measures](#how-the-benchmark-measures)
- [Conditions](#conditions)
- [Results](#results)
- [Significant observations](#significant-observations)
- [Reproducing](#reproducing)
- [Important notes](#important-notes)

---

## Benchmark components

`limen_bench` is a single binary that acts as server or client depending on whether a peer
address is given.

```
./build/limen_bench -d <device> [-t <port>]
        --mode <latency|response|bandwidth|sweep>
        [-s <bytes>] [-n <iters>] [--warmup <n>] [--runs <n>]
        [--rate <ops_per_sec>] [--op <send|write|read>]
        [--inline] [--signal-every <n>] [--pipeline <depth>]
        [--reap <poll|event>] [--moderate <count:usec>]
        [--json <path>] [--clock-floor] [--report-config] <peer>
```

### Modes

| mode | shape | quantity reported |
|---|---|---|
| `latency` | closed loop, depth 1, timed from issue to reply | service time |
| `response` | open loop at `--rate`, timed from the *intended* send time | response time |
| `bandwidth` | operations in flight, bytes over elapsed time | throughput |
| `sweep` | 15 sizes at depth 1 and depth 64, plus a 5-cell option matrix at depth 16 | both |

### The option matrix

One switch changed per row against a depth-16 baseline:

| cell | changed from baseline |
|---|---|
| `inline=on` | inline data enabled for sends under the granted `max_inline_data` |
| `signal_every=16` | one signalled send per 16 instead of every send |
| `pipeline=1` | one operation in flight instead of 16 |
| `reap=event` | completion channel instead of CQ polling |
| `op=write` | not implemented; `post_one` emits `IBV_WR_SEND` only |

### Statistics

Every sample is retained and sorted once at the end. Nothing is summarised incrementally.
`compute()` returns min, p50, p90, p99, p99.9, max, mean, and standard deviation.

The **noise floor** is the range of the per-run medians divided by their mean, across
`--runs` complete measurements including reconnect. It is the resolution limit of the
measurement, and no difference smaller than it is claimed as a result anywhere below.

---

## How the benchmark measures

### Clock

`clock_gettime(CLOCK_MONOTONIC)`. `CLOCK_REALTIME` can step and produce negative
durations. The clock floor is measured before every run as the median of 10,000
consecutive reads after 1,000 discarded, and reported in the conditions block.

At 18 to 29 ns against a smallest measured quantity of 2.90 µs, the floor is 0.6% to 1.0%
of the measurement, inside the 2% threshold at which it would need a stated error bar.

### Warmup

`--warmup` operations are posted and completed but not recorded. The measured window opens
when the warmup-th operation **retires**, not at connect, so connection setup cannot leak
into the first sample.

### Service time versus response time

A closed loop with one operation outstanding measures how long the system takes to serve a
request when nothing else is in flight. It is what the reference tools report. It is not
the latency a client experiences under load, because the benchmark stops issuing work
exactly when the system is least able to handle it: every stall is recorded as one bad
sample instead of the thousands of delayed requests it would actually have caused.

`--mode response` fixes each operation's intended send time before the loop starts, as
`sched_base + i × interval`, and measures from that instant rather than from when the send
actually happened. When the harness falls behind it does not sleep. The lateness is the
measurement.

Both quantities are reported and labelled distinctly.

### Lateness

In scheduled mode, an operation posted after its intended nanosecond is recorded with the
*magnitude* of its lateness, not merely counted. The counter alone is not usable: the post
loop spins at roughly 200 ns granularity, so it overshoots the intended instant by
nanoseconds on nearly every slot and a raw count reads as near-total failure regardless of
load. The report gives the p50, p99, and max of the lateness distribution, plus the count
of operations late by more than one full interval.

### Retirement under pipelining

Operation *i*'s start time is stored at post and retired in post order. This is valid
because RC preserves ordering on a single queue pair. A single reap can return several
completions; each is matched to its own start timestamp.

### Validation

Measured against `perftest` on the same device, same size, same iteration count, RC
transport, same pinning.

**The reference reports half the round trip.** `print_report_lat` in
`perftest_parameters.c` sets `rtt_factor = 2` for SEND and divides every printed column by
it. Determined from source rather than assumed; assuming wrongly produces a clean factor
of two error across an entire report. Every perftest latency figure below is doubled.

---

## Conditions

| | |
|---|---|
| date | 2026-09-12 |
| OS | Ubuntu 24.04.4 LTS |
| kernel | Linux 7.0.0-31-generic |
| CPUs | 8 (4 physical cores, SMT on) |
| pinning | server `cpu1`, client `cpu2`, different physical cores |
| governor | `performance` |
| C-states | `POLL`, `C1`, `C1E`, `C3`, `C6`, `C7s`, `C8` all disabled |
| clocksource | `tsc`, floor 18 to 29 ns |
| device | ConnectX-4 Lx, fw 14.27.6122 |
| `max_qp_wr` / `max_cqe` | 8192 / 4194303 |
| granted `max_inline_data` | 316 B |
| link | 25G SFP28 DAC, active MTU 1024 |
| topology | one host, two cards, two network namespaces |

At MTU 1024 the RoCE v2 wire overhead is roughly 82 B per packet, so the payload ceiling
is about 23.2 Gbit/s rather than 25. That applies to the reference tool equally.

---

## Results

### Latency, 2 B inline, depth 1, poll

100,000 iterations, 5,000 warmup, 9 runs.

```
min 2.90   p50 3.88   p90 3.95   p99 6.12   p99.9 10.86   max 24.00   (µs)
mean 3.94  stddev 0.51  median/mean 0.984
```

Noise floor **0.54%** over 9 runs (medians 3.869 to 3.890 µs).

### Bandwidth, 64 KiB, depth 64, poll

100,000 iterations, 5,000 warmup, 9 runs.

```
2219.92 MiB/s   18.622 Gbit/s   35,519 msg/s
```

Noise floor **0.25%** over 9 runs (elapsed 2.814 to 2.821 s).

### Against perftest

Same device, same sizes, same iteration counts, same pinning, same session.

| quantity | perftest | Limen | ratio |
|---|---:|---:|---:|
| latency min, 2 B | 2.22 µs | 2.90 µs | 1.31x |
| latency p50, 2 B | 2.46 µs | 3.88 µs | 1.58x |
| latency p99, 2 B | 4.80 µs | 6.12 µs | 1.28x |
| latency p99.9, 2 B | 9.44 µs | 10.86 µs | 1.15x |
| bandwidth, 64 KiB | 21.85 Gbit/s | 18.62 Gbit/s | 0.85x |

The latency comparison is not like-for-like. `ib_send_lat` at depth 1 posts sends
**unsignaled** and uses a **separate receive completion queue**. Limen signals sends
because `can_post` uses send completions for send-ring flow control, and shares one CQ
between send and receive. Both are consequences of supporting pipelining, one-sided
operations, and event-driven reaping, none of which the reference latency binary
implements. perftest also uses the `ibv_wr_*` API; Limen uses `ibv_post_send`.

### Size sweep

Latency at depth 1, bandwidth at depth 64. Run twice, once with a 64 B baseline and once
with a 4096 B baseline, to test reproducibility.

| size | med (µs) | p90 (µs) | p99 (µs) | p99.9 (µs) | Gbit/s @ d64 |
|---:|---:|---:|---:|---:|---:|
| 64 B | 4.33 | 5.55 | 7.77 | 13.05 | 0.269 |
| 128 B | 4.51 | 5.71 | 7.84 | 11.63 | 0.526 |
| 256 B | 5.74 | 5.92 | 8.16 | 12.74 | 1.015 |
| 512 B | 6.03 | 6.30 | 8.93 | 14.64 | 1.906 |
| 1 KB | 6.82 | 7.82 | 10.48 | 14.57 | 3.343 |
| 2 KB | 7.63 | 8.72 | 11.91 | 16.76 | 5.501 |
| 4 KB | 9.40 | 9.70 | 15.96 | 23.20 | 8.305 |
| 8 KB | 12.34 | 12.80 | 22.20 | 32.07 | 10.644 |
| 16 KB | 17.63 | 19.14 | 33.85 | 47.51 | 13.468 |
| 32 KB | 28.96 | 33.01 | 50.10 | 63.17 | 16.308 |
| 64 KB | 52.02 | 60.52 | 77.85 | 93.96 | 18.369 |
| 128 KB | 97.49 | 114.70 | 125.69 | 138.88 | 20.023 |
| 256 KB | 189.24 | 209.58 | 233.93 | 258.86 | 20.435 |
| 512 KB | 372.90 | 391.94 | 438.27 | 472.55 | 19.165 |
| 1 MB | 747.68 | 756.26 | 760.33 | 765.11 | 18.972 |

Latency noise floor **0.21%** (64 B baseline) and **0.18%** (4096 B baseline).

**Median latency reproduced within 0.5% at every one of the 15 sizes across the two
sweeps.** The bandwidth column did not: the 64 B cell differs by 11x between them
(0.024 versus 0.269 Gbit/s), and 512 KB and 1 MB differ by 6% and 5%. Each bandwidth cell
runs once, unlike the dedicated `--mode bandwidth` figure above, which is 9 runs at a
0.25% floor. **Do not quote a single figure from this column.**

Taking the two ends of the latency curve:

| quantity | value | from |
|---|---|---|
| fixed per-operation cost | 4.33 µs | 64 B median |
| marginal byte cost | 0.715 ns/B | (747.68 − 372.90) / 524288 |
| marginal rate at depth 1 | 11.2 Gbit/s | 1 / 0.715 ns/B |
| crossover | ~6 KB | 4.33 µs / 0.715 ns/B |

### Option sweep

Baseline: depth 16, signalled every send, inline off, poll reaping. One switch changed per
row. Every cell run 9 times; the floor for each verdict is the larger of the baseline's
and the cell's own per-run spread.

**64 B** (baseline 31.16 µs, floor 0.07%)

| option | median | delta | p99 | cell noise | verdict |
|---|---:|---:|---:|---:|---|
| `inline=on` | 31.13 µs | −0.1% | 43.28 µs | 0.14% | within noise |
| `signal_every=16` | 31.14 µs | −0.1% | 43.94 µs | 0.12% | within noise |
| `pipeline=1` | 4.33 µs | −86.1% | 7.74 µs | 0.21% | significant |
| `reap=event` | 30.66 µs | −1.6% | 44.46 µs | 0.70% | significant |

**4096 B** (baseline 71.36 µs, floor 0.26%)

| option | median | delta | p99 | cell noise | verdict |
|---|---:|---:|---:|---:|---|
| `inline=on` | 71.33 µs | −0.0% | 74.44 µs | 0.27% | within noise |
| `signal_every=16` | 68.92 µs | −3.4% | 72.57 µs | 0.79% | significant |
| `pipeline=1` | 9.41 µs | −86.8% | 11.21 µs | 0.13% | significant |
| `reap=event` | 61.45 µs | −13.9% | 68.74 µs | 1.05% | significant |

4096 B exceeds the granted `max_inline_data` of 316 B, so inline could not apply in that
column. Only the 64 B column measures inlining.

`pipeline=1` also takes a different branch in `run_once`: depth 1 runs the closed-loop
path, the other four cells run the open-loop path. Depth and code path are confounded in
that row.

### Coordinated omission

Capacity at 2 B implied by the closed-loop median is 1 / 3.88 µs ≈ 258,000 ops/s.

| offered rate | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| 140k/s (54% of capacity) | 4.00 µs | 4.25 µs | 8.82 µs | 18.07 µs |
| 400k/s (155% of capacity) | 81,357 µs | 144,819 µs | 159,008 µs | 160,416 µs |

Noise floors **0.23%** and **2.52%** over 9 runs.

Lateness distribution at the same two rates:

| offered rate | interval | ops late | lateness p50 | lateness p99 | lateness max | late by > 1 slot |
|---|---:|---:|---:|---:|---:|---:|
| 140k/s | 7.14 µs | 99,134 / 100,000 | 59 ns | 2.26 µs | 38.29 µs | 268 (0.27%) |
| 400k/s | 2.50 µs | 99,999 / 100,000 | 81,353 µs | 159,004 µs | 160,572 µs | 99,997 (100%) |

Closed-loop latency mode reports 3.88 µs regardless of which of these rates a client would
have been offering, because it never offers more than one operation at a time.

Completion batch size was 1.00 mean and 1 max at both rates.

### Warmup

5,000 iterations with `--warmup 0`, one run, against the steady-state figures above:

| | warmup 0 | warmup 5000 |
|---|---:|---:|
| p50 | 3.88 µs | 3.88 µs |
| p90 | 4.06 µs | 3.95 µs |
| p99 | 6.05 µs | 6.12 µs |
| p99.9 | 15.80 µs | 10.86 µs |

---

## Significant observations

Stated as measured, without mechanism. Each is traceable to a table above.

1. **Per-operation cost is fixed below roughly 6 KB and linear in bytes above it.**
   4.33 µs regardless of size, plus 0.715 ns per byte. The measured curve turns where the
   arithmetic says it should.

2. **Inline data produced no measurable effect at 64 B, against a 0.07% noise floor.**
   −0.1% at the message size where inlining is supposed to matter most, on hardware, over
   9 runs.

3. **Event-driven reaping beat polling at both sizes, and the margin grows with message
   size.** −1.6% at 64 B and −13.9% at 4096 B, with both endpoints pinned to separate
   physical cores.

4. **Signalling every 16th send did nothing at 64 B and helped 3.4% at 4096 B.** Same
   switch, opposite verdicts at two sizes.

5. **The gap to perftest is roughly constant in absolute terms and shrinks as a ratio into
   the tail.** 1.42 µs at the median, 1.32 µs at p99, 1.42 µs at p99.9; 1.58x, 1.28x,
   1.15x.

6. **99.1% of operations at 140k/s were late, by a median of 59 ns.** Only 0.27% were late
   by more than one full 7.14 µs interval. At 400k/s, 100% were late by more than a full
   interval and the median lateness was 81 ms.

7. **Median latency reproduced within 0.5% at all 15 sizes across two independent sweeps;
   the single-run bandwidth column in the same sweeps did not.** 11x disagreement at 64 B,
   5-6% at 512 KB and 1 MB.

8. **Removing warmup left the median unchanged and roughly halved the p99.9.** 3.88 µs
   both ways; 15.80 µs versus 10.86 µs at p99.9.

9. **`--pipeline` originally bounded outstanding sends rather than outstanding requests.**
   A send completes when the adapter has read the buffer, not when the reply arrives, so
   depth 16 allowed thousands of requests in flight and produced an 18 ms median until
   `max_outstanding` was added as a separate bound. Found by measuring, not by reading the
   code.

10. **Noise floors ranged from 0.07% to 2.52% across the suite.** The option sweep's floor
    was 0.07% at 64 B; the 400k/s response run's was 2.52%.

---

## Reproducing

Both endpoints run on one host. `scripts/bench-server.sh` and `scripts/bench-client.sh`
run the whole suite, perftest references first, then Limen.

```sh
# terminal A
./scripts/bench-server.sh

# terminal B
./scripts/bench-client.sh
```

The client script sets the governor to `performance`, disables C-states, runs all ten
phases, and restores both settings on exit including on Ctrl-C. Defaults are `CPU=1` for
the server and `CPU=2` for the client; both scripts read
`/sys/devices/system/cpu/cpuN/topology/core_id` and refuse to start if the two CPUs are
SMT siblings.

Override with environment variables:

```sh
CPU=3 PEER_CPU=1 ./scripts/bench-client.sh
```

`SERVER_DEV`, `CLIENT_DEV`, `PEER`, `GID`, `PORT`, `BENCH`, `NS`, `OUT`, and `TUNE` are
also overridable. Running by hand instead:

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
    -n 100000 --warmup 5000 --runs 9 --json docs/results/sweep-4096b.json 192.168.100.1
```

The server reflects `wc.byte_len` rather than a fixed size, so one invocation serves every
cell of a size sweep.

Restore afterwards:

```sh
sudo cpupower idle-set -E
sudo cpupower frequency-set -g powersave
```

---

## Important notes

**The scripts encode one specific development environment.** Device names
(`rocep1s0f0`, `rocep4s0f0`), the peer address `192.168.100.1`, GID index 3, and the
namespace wrapper `limen-b` are all defaults matching the machine this was built on. They
are environment variables for a reason; nothing about the harness depends on them.

**Both endpoints are on one host.** Two ConnectX-4 Lx ports on the same machine, the
second moved into a network namespace so the two sides have distinct addresses and
distinct RDMA devices. The link between them is a physical 25G SFP28 DAC cable, not
loopback, so the data path is real: PCIe out, wire, PCIe in. What is *not* real is a
switch, a cross-host PCIe topology, and any fabric congestion. The two sides also contend
for one machine's memory bandwidth and cache, which is why both are pinned to separate
physical cores with C-states disabled.

**`rdma system show` must report `netns exclusive`** for the namespace split to work. If
it reports `shared`, `rdma_bind_addr` fails with `EADDRNOTAVAIL` and only `limen_bench`
breaks, while perftest keeps working, because its output says `rdma_cm QPs : OFF` and it never
touches that path.

**The clock floor is reported, not assumed.** It varies between 18 and 29 ns across runs
in this suite even with pinning and C-states off. That variance is in the floor probe
itself.

**Every JSON file carries its own conditions block**, including affinity, SMT siblings,
governor, and per-C-state enable flags. A figure from `results/` can be checked against
the environment it was taken in without reference to this document.

**Figures from different suite runs are not mixed.** Everything here is from the
2026-09-12 run recorded in `results/`. Earlier numbers taken without CPU pinning or with
C-states enabled are superseded and are not reproduced anywhere in this report.