<p align="center">
  <img src="assets/limen-logo-dark-trim.png" alt="Limen" width="600">
</p>

<p align="center">
  <b>An RDMA transport over RoCE v2, built from the verbs layer up.</b>
</p>

<p align="center">
  C++20 &middot; libibverbs &middot; librdmacm &middot; CMake
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
- Five tunable mechanisms on the send path: inline data, send signalling period, pipeline
  depth, reaping mode, and CQ moderation

It runs on ConnectX-4 Lx over a 25G link. It is a learning artifact rather than a
production library, and [what it leaves out](#what-is-left-out) is stated below.

---

## Components

```
libLimen
├── verbs.hpp/cpp     Context, ProtectionDomain, MemoryRegion,
│                     CompletionQueue, CompletionChannel
├── cm.hpp/cpp        EventChannel, ConnectionId, Event
└── session.hpp/cpp   PendingConnection -> Session

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
are different resources and conflating them is a defect this project shipped and then
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
| `sweep` | size sweep 64 B to 1 MB, plus an option matrix over the five send-path switches. |

Every figure it reports carries its conditions and a measured run-to-run noise floor, and
no difference smaller than that floor is claimed as a result. Headline numbers:

| | |
|---|---|
| median round trip, 2 B | 3.88 µs |
| bandwidth, 64 KB | 18.6 Gbit/s |
| versus `perftest` on the same card | 85% of its bandwidth, 1.6x its median latency |

**Full methodology, every result table, and the conditions they were taken under are in
[`docs/benchmark-report.md`](docs/benchmark-report.md).** Raw JSON for every run cited
there is in [`docs/results/`](docs/results/).

> The transport is hand-written. The measurement harness (`src/limen_bench.cpp`) was
> written with AI assistance against a measurement design I specified: which quantities to
> measure, how runs are repeated, how the noise floor gates a result, and what counts as a
> finding. Every number in the report came off my hardware and I can defend how it was
> produced.

---

## What is left out

- **`--op write` and `--op read` are parsed and ignored by the harness.** `post_one`
  emits `IBV_WR_SEND` only. One-sided operations are implemented and exercised by
  `limen_onesided`, but they are not benchmarked, so the two-sided versus one-sided row of
  the option sweep is absent.
- **Both endpoints share one host.** Two cards, two network namespaces, no switch, no
  cross-host PCIe path, no fabric congestion. A two-machine rerun with no code changes is
  the highest-value follow-up.
- **Active MTU is 1024, not 4096.** That caps payload efficiency at about 92.6% of the
  wire before anything else, and it applies equally to the reference tool.
- **One timestamp per completion batch.** Under pipelining, every completion reaped in one
  call shares the timestamp taken after that call returned, which biases the earliest
  member upward. Mean batch size was 1.00 in every run cited, so it never fired. True
  per-completion arrival needs `IBV_WC_EX_WITH_COMPLETION_TIMESTAMP`, which this transport
  does not use.
- **Single connection, single queue pair.** No shared receive queue, no atomics, no memory
  windows, no multi-connection event loop.
- **No cycle counters, cache-miss profiling, or flame graphs.** The report establishes
  *whether*, not *why*. Several of its observations are left without a mechanism rather
  than given a guess.
- **`verdict()` has no margin beyond the floor.** Any delta larger than the measured noise
  floor is marked significant, so a result at 1.4x the floor is reported the same way as
  one at 60x.

---

## License

MIT. Do whatever you want with it.
