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
├── harness.cpp       can_post, post_one, handle_wc, reply_first, drain, reap
└── limen_bench.cpp   modes, sweeps, statistics, reporting
```

Every verbs object is wrapped in a move-only RAII handle with copy deleted. `tests/`
holds `static_assert` contract checks, which break the build if a handle becomes copyable,
and compile-fail tests that `scripts/test.sh` checks for the right error.

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

`Session::close()` destroys in a fixed order: QP, send CQ, CQ, completion channel, MRs, PD,
CM id, event channel. Member declaration order is not sufficient, because a memory region
cannot outlive the protection domain it was registered against and `ibv_destroy_cq` blocks
on unacknowledged completion channel events.

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

`--reap poll` drains in a loop. Sends complete on their own CQ, sized to the signaled sends
that can be outstanding, and the other CQ holds only receives. `--reap event` keeps one
shared CQ, arms it with `ibv_req_notify_cq`, drains a second time, then blocks on the
completion channel fd. The second drain exists because a completion that lands between the
arm and the block generates no event, and a naive arm-then-block never wakes for it.
`--broken-arming` removes that drain so the stall can be reproduced on demand.

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

## Testing

```sh
./scripts/test.sh
```

With no arguments it builds and runs the static and single-device checks. With a peer it
also runs the transport, one-sided, send-path, harness, and valgrind leak checks:

```sh
./scripts/test.sh --dev rocep1s0f0 --gid 3 --peer 192.168.100.2 \
                  --peer-dev rocep4s0f0 --peer-cmd "sudo limen-b"
```

`--list` prints every test and `--only <substring>` runs a subset.

---

## The benchmark harness

`limen_bench` measures the transport in four modes.

| mode | measures |
|---|---|
| `latency` | closed loop, one operation outstanding, timed from one post to the next. Service time. |
| `response` | open loop at `--rate`, timed from the intended send time. Response time. |
| `bandwidth` | sustained bytes over elapsed time with operations in flight. |
| `sweep` | size sweep 64 B to 1 MB, plus an option matrix over the send-path switches. |

Every figure carries its conditions and a measured run-to-run noise floor, and no
difference smaller than that floor is marked significant.

| | |
|---|---|
| median round trip, 2 B | 2.37 µs, noise floor 0.42% over 9 runs |
| `ib_send_lat` median in the same session, doubled for `rtt_factor` | 2.48 µs |
| 64 KiB throughput, per direction | 17.6 to 18.9 Gbit/s over 9 runs, median 18.8 |
| previous build, same session | 3.85 µs round trip, 18.5 and 19.1 Gbit/s in two 9-run sets |

The throughput figure is per direction of a two-sided echo: every message counted also
crossed the wire back as a reply of equal size. `ib_send_bw` is one way and reports
22.51 Gbit/s in the same session. Those are not the same quantity and the report does not
divide them.

Methods, every table, and the conditions everything was taken under are in
[`docs/benchmark-report.md`](docs/benchmark-report.md). Raw JSON for every run is in
[`docs/results/`](docs/results/).

### The latency gap

The first report had Limen at 3.85 µs against perftest's 2.46 µs. Most of that gap was not
in Limen's send path. The client card sits in a slot wired through the chipset, and on
that card a round trip takes either about 2.4 µs or about 3.5 µs. A reply posted within a
few nanoseconds of the request arriving gets the short one, and 4 ns of extra work costs
about 0.9 µs. perftest's latency test lands in the short case because it gives sends a
one-entry CQ, waits for its send completion before polling for the reply, and posts the
next request with nothing in between. Limen shared one large CQ and did its bookkeeping
first.

Limen now does the same in poll mode. Sends complete on their own CQ, the latency loop and
the echo server post before any bookkeeping, and the echo server sends every reply that
fits inline. The median round trip went from 3.85 µs to 2.37 µs, level with perftest's
2.48 µs in the same session. From 2 B to 4 KB the previous build was behind perftest at
every size, and this one is level with it or faster at every size.

That result belongs to this machine. About one round trip in five still takes the slow
path, a server on the chipset card needs `--pipeline 1` to get the short one, both ends on
the chipset card got slower, and on two machines with CPU-attached slots the window
probably does not exist. The mechanism below the driver is not established. The
[report](docs/benchmark-report.md#where-the-latency-went) has the measurements.

The same work turned up a bug. `rdma_cm` leaves `min_rnr_timer` at 0, which the wire reads
as 655 ms. With the echo server's 64 receives against a 64-deep client, one late repost
stalled a run for 1 to 2 s, which matches the 62x collapse at 128 B in the first report.
Every connection now sets it to 0.01 ms.

> The transport is hand-written. The measurement harness (`src/limen_bench.cpp`), parts of
> the debugging that followed, and this report were done with AI assistance against a
> measurement design I specified: which quantities to measure, how runs are repeated, how
> the noise floor gates a result, and what counts as a finding. Every number came off my
> hardware and I can defend how it was produced, including the ones that turned out to be
> wrong.

---

## What is left out

- `--op write` and `--op read` are parsed and ignored by the harness. `post_one` emits
  `IBV_WR_SEND` only, so the two-sided versus one-sided row of the option sweep is absent.
  One-sided operations are implemented and exercised by `limen_onesided`.
- Both endpoints share one host. Two cards, two network namespaces, a DAC cable between
  them, and the client card behind the chipset. No switch, no cross-host PCIe path, no
  fabric congestion. The latency result depends on that slot. A two-machine rerun with no
  code changes is the highest-value follow-up.
- Single connection, single queue pair. No shared receive queue, no atomics, no memory
  windows, no multi-connection event loop.
- Throughput cells in the size sweep run once each. The two sweeps differ by up to 13% at
  some sizes. The 64 KiB figure above is a separate 9-run measurement.
- Peak small-message rate is 5 to 9% below the previous build's runs that did not stall.
  The previous build stalled in 8 of 20 such runs and this one in none.
- The minimum of single latency samples means nothing. A sample runs from one post to the
  next, so a late timestamp shortens the sample after it.
- Replies are polled one per call. Send completions are taken up to 16 per call, but the
  reported batch size counts replies, so its 1.00 is a property of the reap loop rather
  than an observation about the workload.
- CQ moderation is implemented but not swept. It is a flag, not a cell in the option
  matrix.
- Apart from the latency investigation, no cycle counters, cache profiling, or flame
  graphs.
- `verdict()` has no margin beyond the floor. A delta at 1.4x the floor is marked the
  same way as one at 60x.
- `--mode response` does not flag saturation. Past capacity it prints a 27 ms median
  inside a percentile table formatted the same as a 2.8 µs one. The divergence is expected;
  the presentation does not say so.

---

## License

MIT. Do whatever you want with it.
