[![build](https://github.com/packageIncoming/limen/actions/workflows/build.yml/badge.svg)](https://github.com/packageIncoming/limen/actions/workflows/build.yml)

<p align="center">
  <img src="assets/limen-logo-dark-trim.png" alt="Limen" width="600">
</p>

<p align="center">
  <b>An RDMA transport over RoCE v2, built from the verbs layer up.</b>
</p>

<p align="center">
  C++20 &middot; libibverbs &middot; librdmacm &middot; CMake
</p>

<p align="center">
  Mert Isik &middot; <a href="https://github.com/packageIncoming">github.com/packageIncoming</a> &middot; <a href="https://linkedin.com/in/mert-c-isik">linkedin.com/in/mert-c-isik</a> &middot; mertisik329@gmail.com
</p>

---

## What Limen is

Remote Direct Memory Access lets one machine read and write another machine's memory
without involving either kernel on the data path. The application registers memory with
the adapter, posts work requests to a queue pair, and reads completions from a completion
queue. No syscall, no copy, no interrupt in the steady state.

Limen implements that path directly against `libibverbs` and `librdmacm`:

- Reliable connected queue pairs, driven through the full `INIT` → `RTR` → `RTS`
  transition by an `rdma_cm` connection manager
- Two-sided `send`/`recv` and one-sided `write`, `write_with_imm`, and `read`
- Protection domains and registered memory regions with explicit slot rings, because
  registered memory is pinned and the adapter holds its address
- Poll-based and event-driven completion reaping, including the race that event-driven
  reaping has to close
- Tunable send path: inline data, send signalling period, pipeline depth, reaping mode,
  and CQ moderation

It runs on ConnectX-4 Lx over a 25G link. It is a learning artifact rather than a
production library, and [what it leaves out](#what-is-left-out) is stated below.

---

## Components

```
libLimen
├── verbs.hpp/cpp     Context, ProtectionDomain, MemoryRegion,
│                     CompletionQueue, CompletionChannel
├── cm.hpp/cpp        EventChannel, ConnectionId, Event
└── session.hpp/cpp   Listener, PendingConnection -> Session

app layer
├── harness.cpp       can_post, post_one, handle_wc, drain, reap
└── limen_bench.cpp   modes, sweeps, statistics, reporting
```

Every verbs object is wrapped in a move-only RAII handle with copy deleted. `tests/`
holds `static_assert` contract checks and compile-fail tests that break the build if a
handle becomes copyable or a raw pointer escapes.

### The connection window

`rdma_cm` forces resource creation into a specific window: after `id->verbs` becomes
valid, before `rdma_connect` or `rdma_accept`. `PendingConnection` is that window made
into a type, with its own set of legal operations. `finish() &&` consumes it and moves
every resource into a `Session`.

### The listener

`Listener` owns the bind and is separate from `Session`, because a server that accepts
more than one connection has to stay bound between them. `accept_on()` takes a connection
request off the listener and `rdma_migrate_id`s the accepted id onto its own event
channel, so the `Session` still destroys everything it uses without taking the bind with
it.

### Teardown order

`Session::close()` destroys in a fixed order: QP, CQ, completion channel, MRs, PD, CM id,
event channel. Member declaration order is not sufficient, because a memory region cannot
outlive the protection domain it was registered against and `ibv_destroy_cq` blocks on
unacknowledged completion channel events.

### Flow control

`can_post` gates on two independent quantities:

```
posted - covered      < eff_pipeline       // send ring slot safety
posted - recv_count   < max_outstanding    // requests offered to the peer
```

A send completion means the adapter has read the buffer, not that the peer replied. Those
are different resources, and conflating them is a defect this project shipped and then
found by measuring.

### Completion reaping

`--reap poll` drains the CQ in a loop. `--reap event` arms the CQ with
`ibv_req_notify_cq`, drains a second time, then blocks on the completion channel fd. The
second drain exists because a completion that lands between the arm and the block
generates no event, and a naive arm-then-block never wakes for it. `--broken-arming`
removes that drain so the stall can be reproduced on demand.

| binary | purpose |
|---|---|
| `limen_devinfo` | device and port attribute dump |
| `limen_connect` | connection manager bringup, no data path |
| `limen_pingpong` | two-sided send/recv with payload verification |
| `limen_onesided` | RDMA write, write-with-immediate, and read |
| `limen_bench` | measurement harness |

---

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

AddressSanitizer build: `-DLIMEN_ASAN=ON`.

Requires `libibverbs`, `librdmacm`, and a RoCE v2 capable adapter. Soft-RoCE (`rdma_rxe`)
works for correctness but not for any number you would quote.

Quick check that the fabric is alive:

```sh
./build/limen_devinfo -d <device>
```

---

## The benchmark harness

`limen_bench` measures the transport in four modes.

| mode | measures |
|---|---|
| `latency` | closed loop, one operation outstanding, timed from issue. Service time. |
| `response` | open loop at `--rate`, timed from the intended send time. Response time. |
| `bandwidth` | sustained bytes over elapsed time with operations in flight. |
| `sweep` | size sweep 64 B to 1 MB, plus an option matrix over the send-path switches. |

Every figure carries its conditions and a measured run-to-run noise floor, and no
difference smaller than that floor is marked significant.

| | |
|---|---|
| median round trip, 2 B | 3.85 µs, noise floor 0.18% over 9 runs |
| 64 KiB throughput, per direction | 17.7 to 19.7 Gbit/s over 9 runs, median 19.6 |
| `ib_send_lat` median in the same session, doubled for `rtt_factor` | 2.46 µs |

The throughput figure is per direction of a two-sided echo: every message counted also
crossed the wire back as a reply of equal size. `ib_send_bw` is one way and reports
22.55 Gbit/s in the same session. Those are not the same quantity and the report does not
divide them.

**Methods, every table, and the conditions everything was taken under are in
[`docs/benchmark-report.md`](docs/benchmark-report.md).** Raw JSON for every run is in
[`docs/results/`](docs/results/).

> The transport is hand-written. The measurement harness (`src/limen_bench.cpp`), parts of
> the debugging that followed, and this report were done with AI assistance against a
> measurement design I specified: which quantities to measure, how runs are repeated, how
> the noise floor gates a result, and what counts as a finding. Every number came off my
> hardware and I can defend how it was produced, including the ones that turned out to be
> wrong.

---

## What is left out

- **`--op write` and `--op read` are parsed and ignored by the harness.** `post_one`
  emits `IBV_WR_SEND` only, so the two-sided versus one-sided row of the option sweep is
  absent. One-sided operations are implemented and exercised by `limen_onesided`.
- **Both endpoints share one host.** Two cards, two network namespaces, a DAC cable
  between them. No switch, no cross-host PCIe path, no fabric congestion. A two-machine
  rerun with no code changes is the highest-value follow-up.
- **Single connection, single queue pair.** No shared receive queue, no atomics, no memory
  windows, no multi-connection event loop.
- **Throughput cells in the size sweep run once each.** Four of fifteen sizes do not
  reproduce between the two sweeps and are flagged in place in the report. The 64 KiB
  figure above is a separate 9-run measurement.
- **The harness cannot batch completions.** `drain` calls `ibv_poll_cq(cq, 1, &wc)`, one
  per call, so the reported mean batch size of 1.00 is a property of the reap loop rather
  than an observation about the workload.
- **CQ moderation is implemented but not swept.** It is a flag, not a cell in the option
  matrix.
- **No cycle counters, cache profiling, or flame graphs.** The report establishes what,
  not why. Observations are left without a mechanism rather than given a guess.
- **`verdict()` has no margin beyond the floor.** A delta at 1.4x the floor is marked the
  same way as one at 60x.
- **`--mode response` does not flag saturation.** Past capacity it prints a 75 ms median
  inside a percentile table formatted the same as a 3.9 µs one. The divergence is expected;
  the presentation does not say so.

---

## License

MIT. Do whatever you want with it.