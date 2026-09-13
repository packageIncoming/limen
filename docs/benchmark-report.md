# Limen benchmark report

Measurements of the Limen RDMA transport on ConnectX-4 Lx over a 25G link, alongside
`ib_send_lat` and `ib_send_bw` from `linux-rdma/perftest` taken in the same session.

All figures are from one suite run on 2026-09-12 at 18:57. Raw JSON is in
[`results/`](results/). Every JSON file carries its own conditions block.

---

## Conditions

| | |
|---|---|
| OS / kernel | Ubuntu 24.04.4 LTS, Linux 7.0.0-31-generic |
| CPUs | 8 (4 physical cores, SMT on) |
| pinning | server `cpu1`, client `cpu2`, different physical cores |
| governor | `performance` |
| C-states | `POLL`, `C1`, `C1E`, `C3`, `C6`, `C7s`, `C8` disabled |
| clocksource | `tsc`, measured floor 15 to 29 ns |
| device | ConnectX-4 Lx, fw 14.27.6122 |
| `max_qp_wr` / `max_cqe` | 8192 / 4194303 |
| granted `max_inline_data` | 316 B |
| link | 25G SFP28 DAC, `active_mtu` 4096 |
| echo server | `send_slots=64 send_wr=256 recv_wr=64 slot_size=1048576 cqe=336` |
| topology | one host, two cards, two network namespaces |

RoCE v2 framing is roughly 82 B per packet, so at MTU 4096 about 2% of the wire is
overhead before anything else. That applies to the reference tool equally.

---

## Method

**Binary.** `limen_bench` is server or client depending on whether a peer address is given.

```
./build/limen_bench -d <device> [-t <port>]
        --mode <latency|response|bandwidth|sweep>
        [-s <bytes>] [-n <iters>] [--warmup <n>] [--runs <n>]
        [--rate <ops_per_sec>] [--op <send|write|read>]
        [--inline] [--signal-every <n>] [--pipeline <depth>]
        [--reap <poll|event>] [--moderate <count:usec>]
        [--json <path>] [--clock-floor] [--report-config] <peer>
```

**Modes.**

| mode | shape | reported |
|---|---|---|
| `latency` | closed loop, depth 1, timed from issue to reply | service time |
| `response` | open loop at `--rate`, timed from the intended send time | response time |
| `bandwidth` | operations in flight, bytes over elapsed time | throughput |
| `sweep` | 15 sizes at depth 1 and depth 64, plus a 5-cell option matrix at depth 16 | both |

**Clock.** `clock_gettime(CLOCK_MONOTONIC)`. The floor is measured before each run as the
median of 10,000 reads after 1,000 discarded, and printed in the conditions block. At 15 to
29 ns against a smallest measured quantity of 2.86 µs it is about 1% of the measurement.

**Warmup.** `--warmup` operations are posted and completed but not recorded. The measured
window opens when the warmup-th operation retires, not at connect.

**Service time vs response time.** A closed loop at depth 1 measures how long a request
takes when nothing else is in flight. That is what the reference tools report. `--mode
response` fixes each operation's intended send time as `sched_base + i × interval` before
the loop starts and measures from that instant. When the harness falls behind it does not
sleep; the lateness is recorded.

**Lateness.** The post loop spins at roughly 200 ns granularity, so it overshoots the
intended instant by nanoseconds on nearly every slot and a raw late count reads as
near-total failure at any load. Both the lateness distribution and the count of operations
late by more than one full interval are given.

**Retirement.** Operation *i*'s start time is stored at post and retired in post order.
Valid because RC preserves ordering on a single queue pair.

**Noise floor.** The range of the per-run medians divided by their mean, across `--runs`
complete measurements including reconnect. No difference smaller than the floor is marked
significant. Failed runs are counted, excluded, and printed next to the floor. Every run in
this suite completed: 9 of 9 at every step.

**Echo server.** One invocation serves every cell of a sweep; it reflects `wc.byte_len`
rather than a fixed size. It is half of every two-sided figure here and its reply depth
bounds what any client-side option can show, so it prints its resolved configuration at
startup (the `echo:` line in Conditions above).

**perftest.** `print_report_lat` in `perftest_parameters.c` sets `rtt_factor = 2` for SEND
and divides every printed column by it, so every perftest latency figure below is doubled.
`ib_send_bw` without `-b` is one way: the server posts receives and never replies. Limen's
server replies to every message, so its throughput figures are per direction with an equal
volume crossing back. The two throughput numbers are listed but not divided.

---

## Results

### Latency, 2 B inline, depth 1, poll

100,000 iterations, 5,000 warmup, 9 runs.

```
min 2.86   p50 3.85   p90 4.04   p99 5.85   p99.9 9.45   max 19.62   (µs)
mean 3.92  stddev 0.42  median/mean 0.981
```

Noise floor **0.18%** over 9 of 9 runs (medians 3.847 to 3.854 µs).

### Throughput, 64 KiB, depth 64, poll

100,000 iterations, 5,000 warmup, 9 runs. Per direction of a two-sided echo.

| | |
|---|---|
| range over 9 runs | 17.74 to 19.66 Gbit/s |
| median run | 19.56 Gbit/s (2.680 s, 2332 MiB/s, 37,313 msg/s) |
| run-to-run spread | 10.52% |

Elapsed times were 2.667, 2.673, 2.675, 2.678, 2.680, 2.684, 2.743, 2.900, 2.955 s. Two
runs sit outside the other seven. `print_bandwidth` reports the last run rather than a
central estimate, which here was 2.743 s (19.11 Gbit/s). The figure is given as a range.

### Alongside perftest

Same device, sizes, iteration counts, pinning, and session. perftest latency doubled per
`rtt_factor = 2`.

| quantity | perftest | Limen | ratio | gap |
|---|---:|---:|---:|---:|
| latency min, 2 B | 2.24 µs | 2.86 µs | 1.28x | 0.62 µs |
| latency p50, 2 B | 2.46 µs | 3.85 µs | 1.57x | 1.39 µs |
| latency p99, 2 B | 4.74 µs | 5.85 µs | 1.23x | 1.11 µs |
| latency p99.9, 2 B | 8.66 µs | 9.45 µs | 1.09x | 0.79 µs |

| quantity | perftest | Limen |
|---|---:|---:|
| throughput, 64 KiB | 22.55 Gbit/s, one direction, no reply | 19.56 Gbit/s per direction, echo |

The latency figures are not like for like. `ib_send_lat` at depth 1 posts sends unsignaled
and uses a separate receive completion queue. Limen signals sends because `can_post` uses
send completions for send-ring flow control, and shares one CQ between send and receive.
perftest uses the `ibv_wr_*` API; Limen uses `ibv_post_send`.

### Size sweep, latency

Depth 1, poll. The sweep was run twice, once with a 64 B baseline (A) and once with a
4096 B baseline (B). Both median columns are given. Percentile columns are from A.

| size | A med (µs) | B med (µs) | spread | p90 (µs) | p99 (µs) | p99.9 (µs) |
|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 4.29 | 4.29 | 0.0% | 5.48 | 7.63 | 11.61 |
| 128 B | 4.45 | 4.43 | 0.5% | 5.70 | 7.75 | 11.06 |
| 256 B | 5.72 | 5.72 | 0.0% | 5.90 | 8.47 | 17.75 |
| 512 B | 6.03 | 6.02 | 0.2% | 6.26 | 8.80 | 11.94 |
| 1 KB | 6.84 | 6.84 | 0.0% | 7.84 | 10.95 | 15.73 |
| 2 KB | 8.35 | 8.35 | 0.0% | 8.64 | 13.38 | 19.52 |
| 4 KB | 11.60 | 11.66 | 0.5% | 11.88 | 19.58 | 24.64 |
| 8 KB | 15.86 | 15.89 | 0.2% | 16.08 | 26.32 | 33.52 |
| 16 KB | 24.75 | 24.74 | 0.0% | 25.18 | 40.83 | 45.01 |
| 32 KB | 39.39 | 39.42 | 0.1% | 40.05 | 57.72 | 60.80 |
| 64 KB | 61.27 | 60.88 | 0.6% | 77.41 | 96.44 | 109.94 |
| 128 KB | 103.64 | 103.73 | 0.1% | 119.31 | 125.61 | 127.52 |
| 256 KB | 191.06 | 190.80 | 0.1% | 211.95 | 229.06 | 238.83 |
| 512 KB | 368.58 | 369.29 | 0.2% | 389.91 | 394.22 | 398.15 |
| 1 MB | 736.80 | 736.59 | 0.0% | 743.95 | 752.68 | 756.98 |

Latency noise floor 0.28% in both sweeps. Medians agree within 0.6% at all 15 sizes.

Slope between the two endpoints of A: (736.80 − 368.58) µs over 524,288 B is 0.702 ns/B,
or 11.4 Gbit/s marginal. Dividing the 64 B median by that slope puts the crossover near
6 KB. Two-point arithmetic, not a fit over the column.

### Size sweep, throughput

Depth 64. Each cell runs once, so these are single measurements rather than repeated ones.
Both sweeps are given so the spread is visible.

| size | A Gbit/s | B Gbit/s | spread | A msg/s | B msg/s |
|---:|---:|---:|---:|---:|---:|
| 64 B | 1.503 | 1.496 | 0.5% | 2,935,466 | 2,921,743 |
| 128 B | 3.000 | 0.048 | 62x | 2,930,103 | 46,657 |
| 256 B | 5.940 | 5.896 | 0.7% | 2,900,581 | 2,879,054 |
| 512 B | 11.424 | 11.567 | 1.2% | 2,788,989 | 2,823,998 |
| 1 KB | 17.511 | 17.072 | 2.5% | 2,137,562 | 2,083,972 |
| 2 KB | 13.304 | 18.793 | 34% | 811,991 | 1,147,047 |
| 4 KB | 19.779 | 19.519 | 1.3% | 603,611 | 595,680 |
| 8 KB | 17.023 | 14.629 | 15% | 259,749 | 223,219 |
| 16 KB | 19.441 | 18.893 | 2.9% | 148,323 | 144,140 |
| 32 KB | 19.747 | 16.504 | 18% | 75,327 | 62,958 |
| 64 KB | 19.985 | 19.965 | 0.1% | 38,119 | 38,081 |
| 128 KB | 20.253 | 20.071 | 0.9% | 19,315 | 19,141 |
| 256 KB | 19.772 | 20.383 | 3.0% | 9,428 | 9,719 |
| 512 KB | 19.427 | 19.436 | 0.1% | 4,632 | 4,634 |
| 1 MB | 19.222 | 18.750 | 2.5% | 2,291 | 2,235 |

Eleven of fifteen sizes agree within 3.1%. Four do not: 2 KB, 8 KB, and 32 KB by 15% to
34%, and 128 B by a factor of 62. Those four should not be quoted from either column. At
64 KiB the dedicated throughput run above is 9 repeated measurements and is the figure to
use.

Throughput is flat at 2.79 to 2.94 Mpps from 64 B to 512 B regardless of payload size. The
first size where it falls below that is 1 KB.

### Option sweep

Baseline is depth 16, signalled every send, inline off, poll reaping. One switch changed
per row, 9 runs per cell. The gate for each verdict is the larger of the baseline's floor
and the cell's own.

**64 B** (baseline 7.43 µs, floor 1.25%)

| option | median | delta | p99 | cell noise | verdict |
|---|---:|---:|---:|---:|---|
| `inline=on` | 6.29 µs | −15.4% | 10.30 µs | 1.80% | significant |
| `signal_every=16` | 8.12 µs | +9.2% | 12.99 µs | 0.74% | significant, worse |
| `pipeline=1` | 4.29 µs | −42.3% | 7.58 µs | 0.26% | significant |
| `reap=event` | 10.33 µs | +38.9% | 14.60 µs | 0.44% | significant, worse |

**4096 B** (baseline 27.16 µs, floor 2.10%)

| option | median | delta | p99 | cell noise | verdict |
|---|---:|---:|---:|---:|---|
| `inline=on` | 27.15 µs | — | 57.51 µs | 2.06% | not applied, granted 316 B < 4096 B |
| `signal_every=16` | 28.98 µs | +6.7% | 60.05 µs | 7.51% | within noise |
| `pipeline=1` | 11.63 µs | −57.2% | 21.78 µs | 0.43% | significant |
| `reap=event` | 26.91 µs | −0.9% | 60.09 µs | 2.22% | within noise |

`pipeline=1` takes the closed-loop branch in `run_once` while the other cells take the
open-loop branch, so depth and code path are confounded in that row. Its delta is queueing
delay from 16 in flight rather than a property of the send path.

`inline=on` at 4096 B exceeds the granted `max_inline_data`. `run_once` clears the flag, so
the cell runs the baseline configuration and is reported as not applied rather than as a
delta.

At 4096 B only `pipeline=1` clears its gate. `signal_every=16` and `reap=event` do not, and
neither is claimed as a result at that size.

### Response time under offered load

Capacity at 2 B implied by the closed-loop median is 1 / 3.85 µs, about 260,000 ops/s.

| offered rate | p50 | p90 | p99 | p99.9 | floor |
|---|---:|---:|---:|---:|---:|
| 140k/s (54% of capacity) | 3.92 µs | 4.16 µs | 7.44 µs | 16.93 µs | 0.33% |
| 400k/s (154% of capacity) | 75,233 µs | 135,029 µs | 148,517 µs | 149,866 µs | 1.22% |

| offered rate | interval | ops late | lateness p50 | lateness p99 | lateness max | late by > 1 slot |
|---|---:|---:|---:|---:|---:|---:|
| 140k/s | 7.14 µs | 99,190 / 100,000 | 62 ns | 1.17 µs | 36.08 µs | 224 (0.22%) |
| 400k/s | 2.50 µs | 99,999 / 100,000 | 75,229 µs | 148,513 µs | 150,012 µs | 99,997 (100%) |

The 400k row is past saturation. Service time is 3.92 µs against a 2.50 µs slot, a deficit
of 1.42 µs per operation; over 100,000 operations that is 142 ms of accumulated delay
against an observed maximum of 150 ms. The harness prints this inside a percentile table
formatted the same as the 140k row.

Closed-loop latency mode reports 3.85 µs at either offered rate, because it never offers
more than one operation at a time.

Completion batch size was 1.00 mean and 1 max at both rates. `drain` calls
`ibv_poll_cq(cq, 1, &wc)`, one completion per call, so that is a property of the reap loop
rather than of the workload.

### Warmup

5,000 iterations with `--warmup 0`, one run, against the 9-run steady state above.

| | warmup 0 | warmup 5000 |
|---|---:|---:|
| p50 | 3.84 µs | 3.85 µs |
| p90 | 4.13 µs | 4.04 µs |
| p99 | 6.02 µs | 5.85 µs |
| p99.9 | 15.19 µs | 9.45 µs |
| max | 35.19 µs | 19.62 µs |

---

## Not measured

- `--op write` and `--op read` are parsed and ignored. `post_one` emits `IBV_WR_SEND`
  only, so the two-sided versus one-sided row of the option sweep is absent. One-sided
  operations are implemented and exercised by `limen_onesided`.
- Both endpoints are on one host. Two cards, two namespaces, a DAC cable between them. No
  switch, no cross-host PCIe path, no fabric congestion.
- Single connection, single queue pair. No shared receive queue, no atomics, no memory
  windows, no multi-connection event loop.
- CQ moderation (`--moderate`) is implemented but is not a cell in the option matrix.
- No cycle counters, cache profiling, or flame graphs. The tables establish what, not why.
  Observations are left without a mechanism rather than given a guess.
- `verdict()` has no margin beyond the floor, so a delta at 1.4x the floor is marked the
  same way as one at 60x.
- Throughput cells in the size sweep run once each. Four of fifteen do not reproduce
  between the two sweeps and are flagged in place.

## Provenance

Everything here is from the 2026-09-12 18:57 run recorded in `results/`. Earlier runs the
same day are superseded and no figure from them appears above. They predate four harness
fixes: the server's `--pipeline` was parsed and discarded, so its reply depth was pinned at
1; the listening `rdma_cm_id` was destroyed on every accept, so the server was unbound
between sessions; the completion-error report was split across stdout and stderr and only
stdout was captured; and a non-blocking CM check sat in the server's busy-wait loop. All
four are fixed in the tree these numbers came from.

---

## Reproducing

```sh
# terminal A
./scripts/bench-server.sh

# terminal B
./scripts/bench-client.sh
```

About three minutes. Check the server printed `send_slots=64` before reading anything the
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
set to 4200 on both ends:

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

The clock floor is reported, not assumed. It varied between 15 and 29 ns across this suite
with pinning and C-states off. That variance is in the floor probe itself.