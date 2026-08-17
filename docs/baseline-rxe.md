# Baseline: soft-RoCE (rxe) — TRD-00

**Status: superseded for all benchmark purposes. Retained as the "before" half of a
before/after comparison and as the record of how TRD-00 was originally satisfied.**

Recorded July 2026 on two VirtualBox VMs. Superseded 2026-08-16 by the hardware fabric
documented in `fabric.md`. Do not quote latency or bandwidth numbers from this file
without the caveats in §7.

---

## 1. Environment

| | |
|---|---|
| Host | Laptop, Windows + VirtualBox |
| Guests | `limen-node-0`, `limen-node-1`, Ubuntu |
| RDMA device | `rxe0` (software emulation, `rdma_rxe`) |
| Bound netdev | `enp0s8` (host-only adapter) |
| Addresses | node-0 `10.0.0.1`, node-1 `10.0.0.2` |
| NAT adapter | `enp0s3`, `10.0.2.15` (internet only, not the fabric) |

The rxe device does not survive reboot and must be recreated on both nodes:

```bash
sudo rdma link add rxe0 type rxe netdev enp0s8
```

`sudo ufw disable` was required (S7). Note for contrast: on hardware this step is
irrelevant, because RDMA traffic bypasses the kernel network stack entirely and netfilter
never sees it. The firewall mattered here *because* rxe is software running over the
kernel's own UDP path.

`show_gids` is not available. It ships with MLNX_OFED, not with `perftest` or inbox
`rdma-core`. GID enumeration was done by reading sysfs directly.

---

## 2. GID selection (R1)

**Chose index 1 on both nodes.**

node-0:
```
0  RoCE v2  fe80:0000:0000:0000:0a00:27ff:fe58:ef89
1  RoCE v2  0000:0000:0000:0000:0000:ffff:0a00:0001
2  RoCE v2  fe80:0000:0000:0000:dedb:4745:0a22:4f86
```

node-1:
```
0  RoCE v2  fe80:0000:0000:0000:0a00:27ff:fe28:3db0
1  RoCE v2  0000:0000:0000:0000:0000:ffff:0a00:0002
2  RoCE v2  fe80:0000:0000:0000:e09c:aa29:e231:faad
```

Reasoning: the last four bytes of index 1 are `0a:00:00:01` and `0a:00:00:02`, the
IPv4-mapped forms of 10.0.0.1 and 10.0.0.2. Indices 0 and 2 are link-local IPv6.

**Note for later:** rxe reported every GID as RoCE v2. Real hardware does not — mlx5
exposes both RoCE v1 and RoCE v2 entries for the same address, and picking a v1 index
silently selects a different encapsulation. The selection step is trivial here and is a
real decision on hardware. See `fabric.md` §5.

---

## 3. R2 — cross-node RC transfer (`ibv_rc_pingpong`)

node-1 (server):
```
ibv_rc_pingpong -d rxe0 -g 1 -n 10
  local address:  LID 0x0000, QPN 0x000016, PSN 0x5f2537, GID ::ffff:10.0.0.2
  remote address: LID 0x0000, QPN 0x000016, PSN 0x466180, GID ::ffff:10.0.0.1
81920 bytes in 0.02 seconds = 30.79 Mbit/sec
10 iters in 0.02 seconds = 2128.80 usec/iter
```

node-0 (client):
```
ibv_rc_pingpong -d rxe0 -g 1 10.0.0.2 -n 10
  local address:  LID 0x0000, QPN 0x000016, PSN 0x466180, GID ::ffff:10.0.0.1
  remote address: LID 0x0000, QPN 0x000016, PSN 0x5f2537, GID ::ffff:10.0.0.2
81920 bytes in 0.02 seconds = 34.74 Mbit/sec
10 iters in 0.02 seconds = 1886.70 usec/iter
```

**R2 was removed from the automated gate.** Raw-verbs RC data transfer across the fabric
is already verified by R5 (`ib_send_bw`), which passes. `ibv_rc_pingpong` completes when
both endpoints run in interactive shells but fails when the server is launched via
non-interactive SSH (the harness's `srv()` path), while `ib_send_bw` and `rping` survive
the same launch. That isolates the failure to this tool's interaction with the
non-interactive session environment, not to the fabric or to any project code. Retained
as a manual sanity check.

---

## 4. R3 — connection manager path (`rping`)

node-0 server, node-1 client.

**What rping asked for:** server-or-client role, and an address — to bind to as server,
to connect to as client.

**What differed from `ibv_rc_pingpong`:** pingpong required the device name and the GID
index; rping required neither. The connection manager resolves the address to a device
and selects the GID itself. That resolution is what TRD-05 reimplements.

---

## 5. R4 — wire capture (`tcpdump`)

```
22:26:55.331888 IP 10.0.0.2.49577 > limen-node-0.4791: UDP, length 1040
22:26:55.332215 IP 10.0.0.2.49577 > limen-node-0.4791: UDP, length 1040
22:26:55.332597 IP 10.0.0.2.49577 > limen-node-0.4791: UDP, length 1040
22:26:55.332914 IP 10.0.0.2.49577 > limen-node-0.4791: UDP, length 1040
22:26:55.333227 IP 10.0.0.2.49577 > limen-node-0.4791: UDP, length 1040
22:26:55.333592 IP 10.0.0.2.49577 > limen-node-0.4791: UDP, length 1040
^C
16145 packets captured
67868 packets received by filter
51723 packets dropped by kernel
```

Destination port 4791 confirms RoCE v2 encapsulation in UDP.

**Answer given at the time:** this is capturable because rxe runs the traffic through the
host network stack. On real hardware it would not be, because the frames never traverse
the kernel.

**Confirmed 2026-08-16.** That prediction was correct. `tcpdump` on the hardware fabric
captures nothing. See `fabric.md` §5 for what replaces it.

---

## 6. R5 — measurements

### `ib_send_bw`, 64 KB, 1000 iterations

node-0 (server):
```
 Device: rxe0   Connection type: RC   Mtu: 1024[B]   GID index: 1
 #bytes     #iterations    BW peak[MB/sec]    BW average[MB/sec]   MsgRate[Mpps]
 65536      1000             0.00               3.06                 0.000049
```

node-1 (client):
```
 Device: rxe0   Connection type: RC   Mtu: 1024[B]   GID index: 1
 #bytes     #iterations    BW peak[MB/sec]    BW average[MB/sec]   MsgRate[Mpps]
 65536      1000             3.75               3.06                 0.000049
```

### `ib_send_lat`, 2 bytes, 1000 iterations

node-0:
```
 #bytes #iters  t_min    t_max      t_typical  t_avg    t_stdev   99%       99.9%
 2      1000    453.06   37330.73   692.41     928.05   2465.90   1216.65   37330.73
```

node-1:
```
 #bytes #iters  t_min    t_max      t_typical  t_avg    t_stdev   99%       99.9%
 2      1000    461.98   37858.01   724.36     927.05   2486.36   1084.09   37858.01
```

### Summary

| Metric | Value |
|---|---|
| Bandwidth | 3.06 MB/s = **24.5 Mbit/s** |
| Latency, typical | **692–724 µs** |
| Latency, p99.9 | 37,330–37,858 µs (37 ms) |
| stdev | ~2470 µs |

The tail is catastrophic — p99.9 is roughly 50x the median, and the standard deviation is
three times the median. That is a software emulation contending with a hypervisor
scheduler, not a network.

---

## 7. R6 — comparison against TCP

`iperf3` over the same `enp0s8` host-only link:

```
[  5]   0.00-10.01  sec  1.51 GBytes  1.29 Gbits/sec  1409   sender
[  5]   0.00-10.01  sec  1.50 GBytes  1.29 Gbits/sec         receiver
```

| | Bandwidth |
|---|---|
| TCP (`iperf3`) | 1.29 Gbit/s = 161 MB/s |
| soft-RoCE (`ib_send_bw`) | 0.0245 Gbit/s = 3.06 MB/s |
| **TCP advantage** | **~53x** |

**Conclusion:** TCP beats soft-RoCE by roughly 53x on identical hardware. This is the
expected result and the reason soft-RoCE is a development convenience rather than a
transport. rxe does not bypass anything: it emulates RDMA semantics in the kernel and then
sends the result over UDP, so it performs every unit of work TCP performs and adds
emulation on top. It is strictly more work by construction.

*(Unit correction: the original note compared 1.2 Gbit/s against 3 MB/s, which are
different units and understate the gap by a factor of eight. Normalised above.)*

---

## 8. R7 — device attributes (`ibv_devinfo`)

Key fields, node-0. node-1 is identical except for `node_guid` (`0a00:27ff:fe28:3db0`)
and its GID table.

```
hca_id: rxe0
    transport:        InfiniBand (0)
    fw_ver:           0.0.0
    node_guid:        0a00:27ff:fe58:ef89
    vendor_id:        0xffffff
    vendor_part_id:   0
    phys_port_cnt:    1
    max_mr_size:      0xffffffffffffffff
    max_qp:           1048560
    max_qp_wr:        1048576
    max_sge:          32
    max_cq:           1048576
    max_cqe:          32767
    max_mr:           524287
    max_pd:           1048576
    max_qp_rd_atom:   128
    atomic_cap:       ATOMIC_HCA (1)
    num_comp_vectors: 2
  port 1:
    state:            PORT_ACTIVE (4)
    max_mtu:          4096 (5)
    active_mtu:       1024 (3)
    link_layer:       Ethernet
    max_msg_sz:       0x80000000   (2 GiB)
    gid_tbl_len:      1024
    active_width:     1X (1)
    active_speed:     2.5 Gbps (1)
    phys_state:       LINK_UP (5)
    GID[0]: fe80::a00:27ff:fe58:ef89, RoCE v2
    GID[1]: ::ffff:10.0.0.1, RoCE v2
    GID[2]: fe80::dedb:4745:a22:4f86, RoCE v2
```

`fw_ver: 0.0.0`, `vendor_id: 0xffffff`, and `vendor_part_id: 0` are the tells that no
hardware exists. `active_speed: 2.5 Gbps, 1X` is fabricated and bears no relationship to
the measured 24.5 Mbit/s.

**Important:** several of these limits are *higher* than the real adapter's. See
`fabric.md` §7 — code that sized queues against rxe's advertised maxima will fail on mlx5.

---

## 9. What this baseline is and is not good for

**Good for:** developing and debugging verbs code without hardware access; verifying
protocol logic, state machine transitions, completion handling, and teardown; wire-format
inspection via `tcpdump`, which hardware cannot provide.

**Not good for:** any latency or bandwidth claim; any statement about tail behaviour; any
sizing decision based on advertised device limits; anything presented as a performance
result.

**Known to differ from hardware** (all three discovered the hard way):

1. Device creation is manual (`rdma link add`) rather than automatic at driver probe.
2. Every GID is RoCE v2. Hardware exposes RoCE v1 entries that must be avoided.
3. `tcpdump` works. On hardware it captures nothing.

The original TRD-00 note read *"Only difference compared to hardware is the name change."*
That was wrong in all three of the above respects, plus the device-limit differences in §8.
Corrected here rather than deleted, because the specific ways it was wrong are the useful
part.