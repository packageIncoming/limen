# Limen benchmark report

Measurements of the Limen RDMA transport on ConnectX-4 Lx over a 25G link, alongside
`ib_send_lat` and `ib_send_bw` from `linux-rdma/perftest` taken in the same session.

All suite figures are from one run on 2026-09-28 at 22:08, on the tree that includes the
latency changes described [below](#where-the-latency-went). Raw JSON is in
[`results/`](results/). Every JSON file carries its own conditions block. Where a result is
compared with the previous build, the previous build was run in the same session with the
same arguments and pinning, and its logs are in [`results/same-session/`](results/same-session/).

---

## Conditions

| | |
|---|---|
| OS / kernel | Ubuntu 24.04.4 LTS, Linux 7.0.0-31-generic |
| CPUs | 8 (4 physical cores, SMT on) |
| pinning | server `cpu1`, client `cpu2`, different physical cores |
| governor | `performance` |
| C-states | `POLL`, `C1`, `C1E`, `C3`, `C6`, `C7s`, `C8` disabled |
| clocksource | `tsc`, measured floor 19 to 30 ns |
| server card | ConnectX-4 Lx `rocep1s0f0`, fw 14.28.4512, PCIe 01:00.0 on a CPU root port, x8 |
| client card | ConnectX-4 Lx `rocep4s0f0`, fw 14.27.6122, PCIe 04:00.0 on a chipset (PCH) root port, x4 |
| `max_qp_wr` / `max_cqe` | 8192 / 4194303 |
| granted `max_inline_data` | 316 B |
| link | 25G SFP28 DAC, `active_mtu` 4096 (interface MTU 4200) |
| echo server | `send_slots=64 send_wr=256 recv_wr=64 slot_size=1048576 cqe=80 send_cqe=64` |
| topology | one host, two cards, two network namespaces |

RoCE v2 framing is roughly 82 B per packet, so at MTU 4096 about 2% of the wire is
overhead before anything else. That applies to the reference tool equally.

Which slot each card sits in turned out to matter more than anything else in this report.
The client card is behind the chipset, and the latency results below depend on that.

---

## Method

#### Binary

`limen_bench` is server or client depending on whether a peer address is given.

```
./build/limen_bench -d <device> [-t <port>]
        --mode <latency|response|bandwidth|sweep>
        [-s <bytes>] [-n <iters>] [--warmup <n>] [--runs <n>]
        [--rate <ops_per_sec>] [--op <send|write|read>]
        [--inline] [--signal-every <n>] [--pipeline <depth>]
        [--reap <poll|event>] [--moderate <count:usec>]
        [--json <path>] [--clock-floor] [--report-config] <peer>
```

#### Modes

| mode | shape | reported |
|---|---|---|
| `latency` | closed loop, depth 1, timed from one post to the next | service time |
| `response` | open loop at `--rate`, timed from the intended send time | response time |
| `bandwidth` | operations in flight, bytes over elapsed time | throughput |
| `sweep` | 15 sizes at depth 1 and depth 64, plus a 5-cell option matrix at depth 16 | both |

#### Latency samples

In poll mode the latency loop posts the next request the moment it sees the reply, and reads
the clock after posting. A sample is the time between two consecutive posts, which is one
round trip plus the few nanoseconds between seeing a reply and posting. `ib_send_lat`
measures the same way. A timestamp taken late shortens the next sample, so the minimum of
single samples no longer means anything. Medians, means, and percentiles are unaffected.

#### Clock

`clock_gettime(CLOCK_MONOTONIC)`. The floor is measured before each run as the median of
10,000 reads after 1,000 discarded, and printed in the conditions block. At 19 to 30 ns
against a 2.37 µs median it is about 1% of the measurement.

#### Warmup

`--warmup` operations are posted and completed but not recorded. The measured window opens
when the warmup-th operation retires, not at connect.

#### Service time vs response time

A closed loop at depth 1 measures how long a request takes when nothing else is in flight.
That is what the reference tools report. `--mode response` fixes each operation's intended
send time as `sched_base + i × interval` before the loop starts and measures from that
instant. When the harness falls behind it does not sleep, and the lateness is recorded.

#### Lateness

The post loop spins at roughly 200 ns granularity, so it overshoots the intended instant by
nanoseconds on nearly every slot and a raw late count reads as near-total failure at any
load. Both the lateness distribution and the count of operations late by more than one
full interval are given.

#### Retirement

Operation *i*'s start time is stored at post and retired in post order. Valid because RC
preserves ordering on a single queue pair.

#### Noise floor

The range of the per-run medians divided by their mean, across `--runs` complete
measurements including reconnect. No difference smaller than the floor is marked
significant. Failed runs are counted, excluded, and printed next to the floor. Every run in
this suite completed, 9 of 9 at every step.

#### Echo server

One invocation serves every cell of a sweep. It reflects `wc.byte_len` rather than a fixed
size, and sends every reply that fits the granted inline size (316 B) inline. It is half of
every two-sided figure here and its reply depth bounds what any client-side option can
show, so it prints its resolved configuration at startup (the `echo:` line in Conditions
above).

#### perftest

`print_report_lat` in `perftest_parameters.c` sets `rtt_factor = 2` for SEND and divides
every printed column by it, so every perftest latency figure below is doubled. `ib_send_bw`
without `-b` is one way. The server posts receives and never replies. Limen's server
replies to every message, so its throughput figures are per direction with an equal volume
crossing back. The two throughput numbers are listed but not divided.

---

## Results

### Latency, 2 B inline, depth 1, poll

100,000 iterations, 5,000 warmup, 9 runs.

```
min 0.53   p50 2.37   p90 3.54   p99 4.79   p99.9 12.05   max 25.53   (µs)
mean 2.63  stddev 0.79  median/mean 0.899
```

Noise floor 0.42% over 9 of 9 runs (medians 2.369 to 2.379 µs). The minimum is a timing
artifact, see [Latency samples](#latency-samples).

The same measurement on the previous build, same session, gave a median of 3.85 µs with a
floor of 0.18% (min 2.88, p90 4.02, p99 6.30, p99.9 16.01). That matches the 2026-09-12
report's 3.85 µs, so the conditions reproduce and the change is the build.

The samples fall into two groups. Across all 900,000:

| round trip | this build | previous build (2026-09-12 run) |
|---|---:|---:|
| under 2.8 µs | 77.2% | 0.0% |
| 2.8 to 3.2 µs | 2.0% | 0.2% |
| 3.2 to 4.2 µs | 19.3% | 97.1% |
| over 4.2 µs | 1.4% | 2.7% |

About one round trip in five still takes the slow path, which is why p90 sits at 3.54 µs.
[Where the latency went](#where-the-latency-went) covers both groups.

### Throughput, 64 KiB, depth 64, poll

100,000 iterations, 5,000 warmup, 9 runs. Per direction of a two-sided echo.

| | |
|---|---|
| range over 9 runs | 17.60 to 18.91 Gbit/s |
| median run | 18.83 Gbit/s (2.785 s, 2244 MiB/s, 35,907 msg/s) |
| run-to-run spread | 7.34% |

Elapsed times were 2.772, 2.773, 2.779, 2.782, 2.785, 2.814, 2.848, 2.915, 2.979 s.
`print_bandwidth` reports the last run, 2.773 s (18.90 Gbit/s).

Same session, previous build against this build, two sets of 9 runs each, alternating:

| median of 9 | set 1 | set 2 |
|---|---:|---:|
| previous build | 18.47 Gbit/s (2.838 s) | 19.04 Gbit/s (2.754 s) |
| this build | 18.15 Gbit/s (2.889 s) | 19.05 Gbit/s (2.752 s) |

The two builds are level. Each build moves more between its own two sets than the builds
differ from each other. The 2026-09-12 report measured 19.56 Gbit/s for the previous
build, so that day ran faster than this one for reasons outside the code.

### Alongside perftest

Same devices, sizes, iteration counts, pinning, and session. perftest latency doubled per
`rtt_factor = 2`.

| latency, 2 B | perftest | this build | previous build |
|---|---:|---:|---:|
| min | 2.24 µs | not meaningful | 2.88 µs |
| p50 | 2.48 µs | 2.37 µs | 3.85 µs |
| p99 | 4.92 µs | 4.79 µs | 6.30 µs |
| p99.9 | 9.98 µs | 12.05 µs | 16.01 µs |

| throughput, 64 KiB | perftest | Limen |
|---|---:|---:|
| | 22.51 Gbit/s, one direction, no reply | 18.83 Gbit/s per direction, echo |

At the median Limen is level with perftest or slightly under it, by 0.02 to 0.11 µs across
the runs in this report. perftest's own median moved from 2.42 to 2.48 µs between sessions,
which is as large as that margin, so this report does not claim Limen is faster. Limen's
p99.9 is higher.

Both programs now use a send CQ sized to the one outstanding send at depth 1 and wait for
the send completion before looking for the reply. perftest uses the `ibv_wr_*` API; Limen
uses `ibv_post_send`.

### By size, inline where it fits

Median round trip, depth 1, poll, 100,000 iterations. Limen 3 runs per cell. All three
programs inline sizes up to 316 B (perftest with `-I 316`, since its default is 236 B) and
none inline above that. The previous build's server was given `--inline` where the size
fits, its best configuration. Same session. Logs are in
[`results/same-session/latency-by-size.txt`](results/same-session/latency-by-size.txt).

| size | inline | A, A perftest | A, A previous | A, A this build | A, B perftest | A, B previous | A, B this build |
|---:|---|---:|---:|---:|---:|---:|---:|
| 2 B | yes | 1.70 µs | 1.85 µs | 1.60 µs | 2.48 µs | 3.53 µs | 2.38 µs |
| 64 B | yes | 2.00 µs | 2.18 µs | 1.93 µs | 3.22 µs | 3.25 µs | 3.23 µs |
| 128 B | yes | 2.02 µs | 2.25 µs | 1.99 µs | 3.24 µs | 3.26 µs | 3.25 µs |
| 256 B | yes | 3.40 µs | 3.13 µs | 2.88 µs | 6.62 µs | 5.90 µs | 4.69 µs |
| 512 B | no | 2.76 µs | 3.08 µs | 2.83 µs | 4.74 µs | 6.04 µs | 4.83 µs |
| 1 KB | no | 3.18 µs | 3.52 µs | 3.25 µs | 6.64 µs | 6.76 µs | 6.72 µs |
| 4 KB | no | 5.72 µs | 6.12 µs | 5.88 µs | 11.22 µs | 11.65 µs | 11.37 µs |

A is the CPU-attached card and B the chipset-attached one, server card first. With both
ends on A this build is 0.24 to 0.27 µs faster than the previous one at every size, inline
or not, and within 0.16 µs of perftest or under it. With the client on B it is within
0.15 µs of perftest at every size and 1.93 µs under it at 256 B.

The short round trip described [below](#where-the-latency-went) exists only at 2 B here.
At 64 B and up perftest does not get it either. Received data is delivered inside the
completion entry only up to 32 B with 64 B entries, and that is one of the conditions for
the short case. perftest is slower at 256 B inline than at 512 B without inline on both
placements; that was not investigated.

---

## Where the latency went

The 2026-09-12 report had Limen at 3.85 µs against perftest's 2.46 µs in the same session,
a gap of 1.39 µs. About 1.1 µs of it was a timing window on the chipset-attached card that
perftest happens to hit and Limen missed. About 0.3 µs was the echo server not sending its
replies inline. A minimal `rdma_cm` ping-pong written from scratch for this was nearly as
slow as Limen wherever the chipset card was involved (3.34 µs, against 3.52 µs for Limen with
an inline server) until it copied one pattern from perftest.

#### The two cards are not equal

The server card sits on a CPU root port at x8. The client card sits on a chipset root port
and runs at x4. With the server and client both on the CPU-attached card, the previous
build was 1.85 µs against perftest's 1.62, a gap of 0.23 µs. The large gap appeared only
when the chipset-attached card was one of the ends.

#### A timing window on the chipset card

With the chipset card in the path, a round trip takes either about 2.4 µs or about 3.5 µs,
and nothing in between. The ping-pong program with a fixed delay inserted on the server
between seeing a request and posting the reply:

| server delay | 0 ns | 4 ns | 8 to 200 ns | 500 ns | 1000 ns |
|---|---:|---:|---:|---:|---:|
| round trip | 2.45 µs | 3.39 µs | about 3.58 µs | 3.88 µs | 4.38 µs |

Four nanoseconds of extra work before replying costs about 0.9 µs. Past that the penalty
is flat until the delay itself dominates. A delay on the client before sending changed
nothing (0 to 400 ns, all 2.45 µs).

#### What gets a program through

All of these were needed, each tested by turning it off alone in the ping-pong program.

- The send CQ on the chipset-card side is created with one entry (two slots). Any size from
  2 to 256 entries is slow.
- Every send is signaled.
- The sender waits for its own send completion before polling for the reply.
- Nothing happens between seeing a message and posting the next one. No drain, no clock
  read, no bookkeeping.
- Received data is delivered inside the completion entry, which is the mlx5 default.
  Turning that off (`MLX5_SCATTER_TO_CQE=0`) gave 3.10 µs.

`ib_send_lat` does all of this because its latency test sizes the send CQ to `tx_depth`,
which is 1. A patched perftest 6.20 with a larger send CQ measured 2.42 µs by default,
3.30 µs with a send CQ of 2 and 3.32 µs with 32 when the larger CQ was on the chipset-card
side, and 2.42 µs when it was only on the CPU-attached side.

The previous build shared one 336-entry CQ between sends and receives and did its
bookkeeping between seeing a reply and posting the next request.

#### Per placement

Median round trip, 2 B, measured during the investigation with separate scripts at
`active_mtu` 1024. A 2 B message is one packet at either MTU. In this table the previous
build's server was started with `--inline`, which is why server A, client B reads 3.52 µs
here and 3.85 µs in the suite above, where the server script does not pass it.

| server card, client card | perftest | previous build | this build, server depth 64 | this build, server `--pipeline 1` |
|---|---:|---:|---:|---:|
| A (CPU), A | 1.62 µs | 1.85 µs | 1.60 µs | 1.57 µs |
| A, B (chipset) | 2.42 µs | 3.52 µs | 2.37 µs | 2.37 µs |
| B, A | 2.42 µs | 3.52 µs | 3.30 µs | 2.38 µs |
| B, B | 4.28 µs | 3.24 µs | 3.64 µs | 4.21 µs |

A server on the chipset card needs `--pipeline 1` so its own send CQ is the small one.
With both ends on the chipset card every configuration is slow, and this build is slower
there than the previous one.

#### What is not established

The mechanism below the driver. The results are consistent with an idle timer on the
chipset path that takes about 1 µs to exit, but software cannot observe it and it is not
proven. ASPM is disabled on both slot links; the chipset uplink's power settings are BIOS
controlled and were not read.

The fast path is also narrow. A server that does any real work per request, more than a
few nanoseconds, pays the penalty on this card, and so would perftest. On two machines with
both cards in CPU-attached slots the window probably does not exist at all. That rerun has
not been done.

#### The changes

1. In poll mode sends complete on their own CQ, sized to the signaled sends that can be
   outstanding. That is one for the latency client, which the driver rounds to two slots.
   Event mode keeps one shared CQ because it blocks on a single completion channel.
2. The latency loop waits for its send completion on the send CQ, then polls only the
   receive CQ, and posts the next request the moment the reply is seen. Bookkeeping and the
   clock read come after the post.
3. The echo server posts each reply before it reposts the receive and counts it.
4. The echo server sends every reply that fits the granted inline size inline, whether or
   not `--inline` was given. Worth about 0.3 µs at 2 B.
5. Send completions are collected up to 16 per poll, by the echo server whenever it is idle
   or holding a reply back, and by the client's drain on every pass. The first version of
   changes 1 to 3 collected one at a time only when the other CQ was empty, and under load
   that capped small messages at about 2.1M msg/s.
6. Every connection sets `min_rnr_timer` to 1 (0.01 ms) once it is established. `rdma_cm`
   leaves it at 0, which the wire reads as 655.36 ms, the longest wait there is. The echo
   server posts 64 receives against a client pipeline of 64, so any hiccup leaves a request
   with no receive posted, and the previous build then stalled for 1 to 2 s. Forced with a
   client pipeline of 256, the previous build moved 7.8k to 9.3k msg/s and this build 2.07M
   to 2.19M. The 62x collapse at 128 B in the 2026-09-12 sweep has the same signature.

---

### Size sweep, latency

Depth 1, poll. The sweep was run twice, once with a 64 B baseline (A) and once with a
4096 B baseline (B). Percentile columns are from A. The last column is the previous build,
same session, from a sweep with a 64 B baseline.

| size | A med (µs) | B med (µs) | spread | p90 (µs) | p99 (µs) | p99.9 (µs) | previous med (µs) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 4.22 | 4.22 | 0.0% | 4.67 | 7.18 | 20.02 | 4.29 |
| 128 B | 4.30 | 4.30 | 0.0% | 4.76 | 6.99 | 19.91 | 4.44 |
| 256 B | 4.64 | 4.56 | 1.7% | 6.15 | 10.54 | 21.02 | 5.70 |
| 512 B | 4.84 | 4.82 | 0.4% | 6.09 | 8.96 | 20.66 | 6.03 |
| 1 KB | 6.73 | 6.73 | 0.0% | 7.85 | 11.90 | 22.16 | 6.84 |
| 2 KB | 8.13 | 8.13 | 0.0% | 8.60 | 14.65 | 25.38 | 8.37 |
| 4 KB | 11.48 | 11.38 | 0.9% | 13.21 | 25.04 | 36.34 | 11.61 |
| 8 KB | 15.66 | 15.67 | 0.1% | 16.25 | 29.30 | 38.13 | 15.90 |
| 16 KB | 24.93 | 24.93 | 0.0% | 25.59 | 41.93 | 52.26 | 24.77 |
| 32 KB | 39.24 | 39.37 | 0.3% | 40.41 | 57.05 | 62.22 | 39.44 |
| 64 KB | 60.54 | 60.64 | 0.2% | 68.64 | 87.35 | 101.14 | 60.77 |
| 128 KB | 103.90 | 104.22 | 0.3% | 122.12 | 142.56 | 148.55 | 103.84 |
| 256 KB | 190.60 | 191.58 | 0.5% | 213.71 | 245.45 | 258.42 | 190.82 |
| 512 KB | 373.67 | 384.03 | 2.7% | 401.71 | 450.19 | 461.84 | 374.92 |
| 1 MB | 740.58 | 741.33 | 0.1% | 784.35 | 877.78 | 923.87 | 740.13 |

Latency noise floor 0.17% (A) and 0.45% (B).

The sweep does not pass `--inline`, so the client's requests are not inline and the 2 B
fast path above does not apply. The medians improve by 0.1 to 0.2 µs up to 128 B and by
1.1 to 1.2 µs at 256 B and 512 B. 256 B replies now go inline and 512 B replies do not, so
inline is not the whole of the 512 B change, and what is was not isolated. From 16 KB up the
two builds agree within 0.7%.

### Size sweep, throughput

Depth 64. Each cell runs once, so these are single measurements rather than repeated ones.
Both sweeps are given so the spread is visible, with the previous build from the same
session last.

| size | A Gbit/s | B Gbit/s | spread | A msg/s | B msg/s | previous msg/s |
|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 1.305 | 1.338 | 2.5% | 2,548,902 | 2,613,976 | 11,990 |
| 128 B | 2.645 | 2.667 | 0.8% | 2,583,308 | 2,604,701 | 2,824,162 |
| 256 B | 5.010 | 5.070 | 1.2% | 2,446,319 | 2,475,468 | 2,791,735 |
| 512 B | 9.587 | 10.152 | 5.7% | 2,340,456 | 2,478,577 | 2,630,842 |
| 1 KB | 13.961 | 15.930 | 13.2% | 1,704,255 | 1,944,627 | 2,092,939 |
| 2 KB | 16.204 | 17.110 | 5.4% | 989,020 | 1,044,292 | 1,117,859 |
| 4 KB | 15.723 | 17.909 | 13.0% | 479,831 | 546,543 | 572,972 |
| 8 KB | 19.474 | 17.680 | 9.7% | 297,148 | 269,772 | 292,925 |
| 16 KB | 18.512 | 18.129 | 2.1% | 141,233 | 138,311 | 138,860 |
| 32 KB | 18.385 | 17.533 | 4.7% | 70,134 | 66,885 | 71,733 |
| 64 KB | 18.470 | 18.271 | 1.1% | 35,228 | 34,849 | 37,053 |
| 128 KB | 18.798 | 17.568 | 6.8% | 17,928 | 16,754 | 18,731 |
| 256 KB | 18.590 | 17.566 | 5.7% | 8,864 | 8,376 | 9,340 |
| 512 KB | 17.959 | 16.499 | 8.5% | 4,282 | 3,934 | 4,675 |
| 1 MB | 17.515 | 18.468 | 5.3% | 2,088 | 2,202 | 2,334 |

No cell of either sweep collapsed. The previous build's 64 B cell collapsed in this
session, as its 128 B cell did in the 2026-09-12 sweep. The spread between A and B is up to
13%, so single cells should not be quoted to better than that.

The large sizes read lower here than the previous build's single cells. Repeated runs,
5 per set, two sets per build, alternating, put them level. At 256 KB the previous build
took 0.210 to 0.228 s per run and this build 0.211 to 0.230 s. At 1 MB the previous build
took 0.858 to 1.070 s and this build 0.880 to 0.923 s.

Small messages are a trade. Repeated runs at depth 64, 10 per build:

| size | previous build, runs without a stall | previous build stalls | this build | this build stalls |
|---|---:|---:|---:|---:|
| 64 B | 2.78M to 2.86M msg/s | 7 of 10 | 2.63M to 2.70M msg/s | 0 of 10 |
| 512 B | 2.63M to 2.70M msg/s | 1 of 10 | 2.44M to 2.50M msg/s | 0 of 10 |

This build gives up roughly 5 to 9% of peak small-message rate against the previous
build's runs that did not stall, and no longer stalls.

### Option sweep

Baseline is depth 16, signalled every send, inline off, poll reaping. One switch changed
per row, 9 runs per cell. The gate for each verdict is the larger of the baseline's floor
and the cell's own.

64 B (baseline 6.49 µs, floor 5.20%)

| option | median | delta | p99 | cell noise | verdict |
|---|---:|---:|---:|---:|---|
| `inline=on` | 5.25 µs | −19.0% | 11.98 µs | 1.28% | significant |
| `signal_every=16` | 6.43 µs | −0.9% | 12.19 µs | 1.13% | within noise |
| `pipeline=1` | 4.22 µs | −34.9% | 7.05 µs | 0.17% | significant |
| `reap=event` | 10.44 µs | +60.8% | 15.80 µs | 1.47% | significant, worse |

The previous build in the same session had a 7.51 µs baseline with a 0.57% floor, and
`signal_every=16` was 8.3% worse. On this build it is within noise. That change was not
isolated.

4096 B (baseline 27.41 µs, floor 2.38%)

| option | median | delta | p99 | cell noise | verdict |
|---|---:|---:|---:|---:|---|
| `inline=on` | 27.40 µs | n/a | 61.82 µs | 3.40% | not applied, granted 316 B < 4096 B |
| `signal_every=16` | 30.08 µs | +9.7% | 62.00 µs | 4.34% | significant, worse |
| `pipeline=1` | 11.37 µs | −58.5% | 21.37 µs | 0.10% | significant |
| `reap=event` | 27.38 µs | −0.1% | 64.22 µs | 2.34% | within noise |

`pipeline=1` takes the closed-loop branch in `run_once` while the other cells take the
open-loop branch, so depth and code path are confounded in that row. Its delta is queueing
delay from 16 in flight rather than a property of the send path.

`inline=on` at 4096 B exceeds the granted `max_inline_data`. `run_once` clears the flag, so
the cell runs the baseline configuration and is reported as not applied rather than as a
delta.

### Response time under offered load

Capacity at 2 B implied by the closed-loop median is 1 / 2.37 µs, about 420,000 ops/s.

| offered rate | p50 | p90 | p99 | p99.9 | floor |
|---|---:|---:|---:|---:|---:|
| 140k/s | 2.78 µs | 3.98 µs | 9.75 µs | 19.89 µs | 29.92% |
| 400k/s | 26,987 µs | 48,146 µs | 52,949 µs | 53,361 µs | 10.38% |

| offered rate | interval | ops late | lateness p50 | lateness p99 | lateness max | late by > 1 slot |
|---|---:|---:|---:|---:|---:|---:|
| 140k/s | 7.14 µs | 99,394 / 100,000 | 80 ns | 3.79 µs | 39.05 µs | 595 (0.60%) |
| 400k/s | 2.50 µs | 99,999 / 100,000 | 26,984 µs | 52,947 µs | 53,404 µs | 99,998 (100%) |

At 140k/s eight of the nine run medians were 2.756 to 2.786 µs and one was 3.613 µs, which
is where the 29.92% floor comes from. The 2026-09-12 report had a p50 of 3.92 µs here.

The 400k row is past saturation even though 400k/s is under the closed-loop figure. The
open-loop path costs more per operation than the closed loop (2.78 µs at 140k/s against a
2.50 µs slot), and the backlog reached 53.4 ms over 100,000 operations, about 0.53 µs per
operation. The 2026-09-12 report had a p50 of 75,233 µs at this rate. The harness prints
both rows in the same percentile table format.

Replies are still polled one per call, so the completion batch size is 1.00 mean and 1 max
at both rates, a property of the reap loop rather than of the workload.

### Warmup

5,000 iterations with `--warmup 0`, one run, against the 9-run steady state above.

| | warmup 0 | warmup 5000 |
|---:|---:|---:|
| p50 | 2.38 µs | 2.37 µs |
| p90 | 3.55 µs | 3.54 µs |
| p99 | 4.93 µs | 4.79 µs |
| p99.9 | 10.46 µs | 12.05 µs |
| max | 19.64 µs | 25.53 µs |

The warmup-0 run has 5,000 samples against 900,000, so its tail is not comparable.

---

## Not measured

- `--op write` and `--op read` are parsed and ignored. `post_one` emits `IBV_WR_SEND`
  only, so the two-sided versus one-sided row of the option sweep is absent. One-sided
  operations are implemented and exercised by `limen_onesided`.
- Both endpoints are on one host. Two cards, two namespaces, a DAC cable between them. No
  switch, no cross-host PCIe path, no fabric congestion. The client card is behind the
  chipset, and the latency results above depend on it.
- Single connection, single queue pair. No shared receive queue, no atomics, no memory
  windows, no multi-connection event loop.
- CQ moderation (`--moderate`) is implemented but is not a cell in the option matrix.
- Apart from the latency investigation above, no cycle counters, cache profiling, or flame
  graphs. The tables establish what, not why.
- `verdict()` has no margin beyond the floor, so a delta at 1.4x the floor is marked the
  same way as one at 60x.
- Throughput cells in the size sweep run once each. The two sweeps differ by up to 13%.
- The server on the chipset card with the default depth, and both ends on the chipset card,
  are covered only by the placement table.

## Provenance

The suite figures are from the 2026-09-28 22:08 run recorded in `results/`. The same-session
previous-build runs, in `results/same-session/`, were taken right after it with the same
arguments and pinning. The delay sweep, the patched perftest, the per-placement table, and
the forced receiver-not-ready case come from the investigation and were measured with
separate scripts; their raw logs are not in this repository.

The 2026-09-12 report and its results are superseded and are in the git history.

---

## Reproducing

```sh
# terminal A
./scripts/bench-server.sh

# terminal B
./scripts/bench-client.sh
```

About two minutes. Check the server printed `send_slots=64` before reading anything the
client produces.

The client script sets the governor to `performance`, disables C-states, runs all ten
phases, and restores both on exit including on Ctrl-C. Every phase captures stderr into
its log. Defaults are `CPU=1` for the server and `CPU=2` for the client; both scripts read
`/sys/devices/system/cpu/cpuN/topology/core_id` and refuse to start if those are SMT
siblings.

`DEV`, `PEER`, `GID`, `PORT`, `BENCH`, `NS`, `OUT`, and `TUNE` are overridable, plus
`MAXMSG` and `DEPTH` on the server.

By hand instead:

```sh
lscpu -e=CPU,CORE                       # pick CPUs with different CORE values
sudo cpupower frequency-set -g performance
sudo cpupower idle-set -D 0
```

```sh
# server: largest size in the sweep, reply depth matching the client's deepest pipeline
taskset -c 1 ./build/limen_bench -d rocep1s0f0 -s 1048576 --pipeline 64 -n 1000000000

# client
sudo limen-b taskset -c 2 ./build/limen_bench -d rocep4s0f0 --mode sweep -s 4096 \
    -n 100000 --warmup 5000 --runs 9 --json docs/results/sweep-4096b.json 192.168.100.1
```

The server pins about 128 MiB per session at that configuration, so `RLIMIT_MEMLOCK` has
to allow it. The script raises it or fails with a message.

`active_mtu` follows the netdev MTU. These runs used 4096, which needs the interface MTU
set to 4200 on both ends. The interfaces come up at 1500, which gives 1024.

```sh
sudo ip link set dev <server iface> mtu 4200
sudo limen-b ip link set dev <client iface> mtu 4200
```

Restore afterwards:

```sh
sudo cpupower idle-set -E
sudo cpupower frequency-set -g powersave
```

A sweep that loses a cell prints `LATENCY CELL FAILED` or `BANDWIDTH CELL FAILED` with the
exit code on that row, prints a count at the end, and exits non-zero. A run that fails
inside a repeated measurement is excluded and the exclusion is printed next to the noise
floor.

---

## Notes

The scripts encode one development environment. Device names (`rocep1s0f0`,
`rocep4s0f0`), the peer address `192.168.100.1`, GID index 3, and the namespace wrapper
`limen-b` are defaults matching the machine this was built on. They are environment
variables for a reason.

`rdma system show` must report `netns exclusive` for the namespace split to work. If it
reports `shared`, `rdma_bind_addr` fails with `EADDRNOTAVAIL` and only `limen_bench`
breaks, while perftest keeps working, because its output says `rdma_cm QPs : OFF` and it
never touches that path.

The clock floor is reported, not assumed. It varied between 19 and 30 ns across this suite
with pinning and C-states off. That variance is in the floor probe itself.
