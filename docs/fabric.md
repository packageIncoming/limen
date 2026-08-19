# Limen Fabric Reference

Host: `mert-OptiPlex-7050` (Dell OptiPlex 7050 MT, Ubuntu 24.04 Desktop)
Last verified: 2026-08-18

Single-host, two-card RoCE v2 fabric. Two Mellanox ConnectX-4 Lx (CX4121C) adapters
cross-connected by a 10Gtek 25G SFP28 passive DAC. Both endpoints run on the same
machine, split across network namespaces.

---

## 1. Topology

```
         root namespace                      namespace  limen-b
    +--------------------------+        +--------------------------+
    |  rocep1s0f0              |        |  rocep4s0f0              |
    |  enp1s0f0np0             |        |  enp4s0f0np0             |
    |  192.168.100.1/24        |        |  192.168.100.2/24        |
    |  04:3f:72:e8:46:5a       |        |  04:3f:72:d5:46:c4       |
    +-----------+--------------+        +--------------+-----------+
                |                                      |
                +--------  25G SFP28 passive DAC  -----+
```

Server side conventionally runs in the root namespace on `.1`.
Client side runs in `limen-b` on `.2`.

### Why namespaces are required

Both fabric addresses are in `192.168.100.0/24` on the same host. With both interfaces
in the root namespace the kernel resolves the peer address as **local**, routes it via
`lo`, and the neighbor entry never populates (`ip neigh` shows `FAILED`). RoCE resolves
the destination MAC from the destination GID through the ordinary route and neighbor
path, so with no neighbor entry there is no dmac, and the queue pair transition to RTR
fails:

```
Failed to modify QP to RTR
Couldn't connect to remote QP
```

That error is misleading. It reads like a peer or cabling problem and is actually a
local routing problem. Moving one card into its own network namespace makes the peer
address non-local, which restores ARP, which restores dmac resolution.

Side effect worth knowing: with the namespace split, the TCP side channel between two
processes now genuinely traverses the DAC rather than loopback, so
`tcpdump -i enp1s0f0np0` sees both the side channel and the RoCE traffic.

---

## 2. GID selection

Both devices: **GID index 3**, RoCE v2, IPv4-mapped.

```
index 3  ->  ::ffff:192.168.100.1   (rocep1s0f0)
index 3  ->  ::ffff:192.168.100.2   (rocep4s0f0)
```

Rejected entries: the lower indices are RoCE v1, which encapsulate directly in Ethernet
with their own EtherType and cannot be routed; the IPv6 link-local entries address the
`fe80::` scope rather than the fabric subnet. Index 0 is RoCE v1 on these cards, which
is why defaulting to it fails with an error that never mentions GIDs.

The GID table is rebuilt when a device moves between namespaces. Re-derive the index
after any namespace change rather than assuming it carried over. `limen-verify` prints
both.

Flag inconsistency across tools: `ibv_rc_pingpong` takes `-g <index>`, perftest tools
(`ib_send_lat`, `ib_send_bw`) take `-x <index>`.

---

## 3. Link parameters

| Property | Value | Note |
|---|---|---|
| Link layer | Ethernet | |
| Port state | PORT_ACTIVE | |
| Line rate | 25 Gb/s | SFP28 |
| Netdev MTU | 1500 | |
| Path MTU | 1024 B | capped by the 1500 B netdev MTU |
| Max inline | 236 B (latency test), 0 B (bandwidth test) | |

**Open item.** ConnectX-4 Lx supports a 4096 B path MTU. Reaching it requires raising
the netdev MTU to 9000 on both interfaces. That would measurably change the bandwidth
figures and invalidates section 4. Either do it and re-measure once, or record the
decision not to. Do not discover the gap later.

---

## 4. Baseline measurements

Taken 2026-08-18 over the namespace-split topology above, endpoints pinned to separate
cores so they do not contend for one.

### 4.1 Bandwidth, `ib_send_bw`

```bash
# server, root namespace
sudo taskset -c 2 ib_send_bw -d rocep1s0f0 -x 3 -F --report_gbits

# client, limen-b
sudo limen-b taskset -c 6 ib_send_bw -d rocep4s0f0 -x 3 -F --report_gbits 192.168.100.1
```

65536 byte messages, 1000 iterations, TX depth 128, CQ moderation 1.
Ten consecutive runs across two batches, client-side average column, sorted:

```
22.18  22.34  22.48  22.48  22.51  22.54  22.59  22.64  22.71  22.72
```

**Median 22.53 Gb/s, range 22.18 to 22.72, spread 2.4%.**
That is **90.1% of 25G line rate** at the median.

Report the median and the range together. A single run is a sample, not a measurement.

Column identity confirmed against the unit-independent message rate:
`0.042602 Mpps x 65536 B x 8 = 22.34 Gb/s`, matching that run's reported average
exactly. The reported average is a true average, not a peak.

### 4.2 Startup transients, and the peak-versus-average check

Two earlier single runs the same day reported **19.66** and **19.68 Gb/s**, roughly 13%
below the block above. Resolved: in the 19.66 run the **peak column read 22.54**,
identical to the steady-state average of every clean run. The hardware reached full
speed during that run and the average was dragged down by a slow portion of it.

That is a startup transient, not a fabric limit. Most likely CPU frequency ramp, since
that run launched immediately after a `sudo` password prompt on an otherwise idle core.
Clean runs show peak and average within 0.15 Gb/s of each other.

**Standing sanity check: compare peak against average on every run.** A large gap means
the run was contaminated and should be discarded. A small gap means the number is real.
Do not average contaminated runs into a baseline, and do not report a depressed average
as a fabric characteristic.

### 4.3 Units, resolved

perftest's default **`MB/sec` column is mebibytes per second**, not megabytes. It
divides by `0x100000`. Verify from the unit-independent `MsgRate` column:

```
0.039650 Mpps x 65536 B = 2,598,502,400 B/s
  / 1048576  = 2478.1   -> matches the reported 2478.13 MB/sec
  / 1e6      = 2598.5   -> matches nothing
```

So converting a perftest `MB/sec` figure with `x 8 / 1000` under-reports by about 4.9%.
**Always pass `--report_gbits`** and skip the conversion entirely. Every figure in this
document uses it.

The previously recorded **19.70 Gb/s / 78.8% of line rate** came from that conversion
error applied to a transient-contaminated run, so it was wrong twice over. Superseded by
the 22.53 Gb/s median in section 4.1. Do not quote it.

### 4.4 Latency, `ib_send_lat`

```bash
sudo taskset -c 2 ib_send_lat -d rocep1s0f0 -x 3 -F
sudo limen-b taskset -c 6 ib_send_lat -d rocep4s0f0 -x 3 -F 192.168.100.1
```

2 byte messages, 1000 iterations. Microseconds.

| | t_min | typical (p50) | avg | stdev | p99 | p99.9 | t_max |
|---|---|---|---|---|---|---|---|
| server (`.1`) | 1.11 | **1.17** | 1.29 | 0.40 | 3.93 | 8.00 | 8.00 |
| client (`.2`) | 1.10 | **1.17** | 1.30 | 0.48 | 3.78 | 8.35 | 8.35 |

Quote p50, p99, and p99.9 together, never p50 alone. Ignore the mean: the tail drags it
and it carries nothing the percentiles do not.

The tail runs about 3.3x the median. Cause is host contention from both endpoints
sharing one machine's cores and cache. That is a property of the single-host topology,
not of the fabric, and it is the correct thing to say when asked.

**Not yet multi-run.** Given what section 4.2 showed for bandwidth, these single-run
latency figures deserve the same five-run treatment before being quoted anywhere that
matters.

### 4.5 Sanity check, `ibv_rc_pingpong`

```bash
sudo ibv_rc_pingpong -d rocep1s0f0 -g 3
sudo limen-b ibv_rc_pingpong -d rocep4s0f0 -g 3 192.168.100.1
```

Result: 8,192,000 bytes in 0.01 s, 1000 iters at 9.81 usec/iter, reported as
6683.94 Mbit/sec.

Two of those numbers mislead:

- Default message size is 4096 B. 1000 iters x 4096 B x 2 directions = 8,192,000 B,
  confirming the byte count counts round trips.
- **9.81 usec is a round trip**, ping plus pong. One way is roughly 4.9 usec.
- The **6683.94 Mbit/sec is not a bandwidth measurement.** `rc_pingpong` is strictly
  synchronous, one message in flight, nothing pipelined, so that figure is message size
  divided by round-trip latency. It is a latency number in bandwidth clothing, and it is
  not comparable to `ib_send_bw`, which keeps 128 work requests outstanding.

Of the ~4.9 usec one way, 1.31 usec is unavoidable serialization (4096 B at 25 Gb/s).
The rest is host stack, CQ polling, and same-box CPU contention.

---

## 5. Bringup

Three scripts plus a systemd unit. Verified surviving a cold boot on 2026-08-18.

| Path | Purpose |
|---|---|
| `/usr/local/sbin/limen-fabric-up.sh` | full bringup, idempotent, self-elevating |
| `/usr/local/bin/limen-verify` | check both namespaces, print both GID indices, ping across the DAC |
| `/usr/local/bin/limen-b` | run a command inside the `limen-b` namespace |
| `/etc/systemd/system/limen-fabric.service` | bringup at boot, ordered before the desktop stack |

### What does and does not survive a power cycle

Survives: the scripts, the systemd unit,
`/etc/NetworkManager/conf.d/99-limen-unmanaged.conf`.

Does **not** survive, and is rebuilt by the unit on every boot: `netns exclusive` mode
(kernel state, resets to `shared`), the `limen-b` namespace, the device placement, and
both IP addresses.

### The EBUSY trap

`rdma system set netns exclusive` returns `Device or resource busy` when **any** network
namespace other than init exists on the system. Not when a device is in use, which is
what the message implies. The kernel walks its registered-namespace list and bails on
the first non-init entry.

Blockers found on this box: sixteen Firefox snap sandboxes (one net namespace per
content process), `accounts-daemon`, `rtkit-daemon`, and a stale `limen-b` from a
previous run.

The check applies **only at the moment of the mode change**. Once exclusive is set,
namespaces may return freely. The requirement is one clean window, not a permanently
clean system, which is why the boot-time ordering works.

Manual recovery:

```bash
sudo ip netns del limen-b          # it blocks the very change needed to create it
pkill -f firefox
sudo systemctl stop accounts-daemon rtkit-daemon
sudo lsns -t net                   # must show exactly one line
sudo rdma system set netns exclusive
```

Use `lsns -t net`, not `ip netns list`. The latter only shows namespaces with a bind
mount under `/var/run/netns` and misses every snap sandbox.

### Ordering constraints

- Delete any stale `limen-b` **before** setting exclusive mode. The reverse order works
  on a first run and fails on every subsequent one.
- The unit's `Before=` list exists to win the boot race against `snapd`,
  `display-manager`, `accounts-daemon` and `rtkit-daemon`. The script also retries five
  times, stopping dbus-activated daemons between attempts, because `Before=` only orders
  against units systemd pulls into the boot transaction.
- Installing Docker, containerd, or libvirt on this host will break boot-time bringup.
  Each creates a net namespace early.
- Moving a netdev into a namespace **wipes its IP configuration**. The script re-adds it.
- Both interfaces carry `noprefixroute`, meaning NetworkManager owns them by default.
  They are marked unmanaged, otherwise NM reclaims and reconfigures them.
- If a boot ever fails, `sudo limen-fabric-up.sh --force` also kills Firefox and fixes it
  in one command. The journal will name the offending namespaces.

---

## 6. Known issues

**Startup transients depress single-run bandwidth averages.** Explained in section 4.2.
Check peak against average before trusting any run.

**Latency not multi-run.** See section 4.4.

**Path MTU capped at 1024.** See section 3.

**Test harness and namespaces.** `trd-02-tests.sh --ssh` cannot reach a namespace; the
harness invokes the client binary directly. Either start servers by hand and use the
interactive prompt, or wrap the binary path in `limen-b`.

**Fat tail latency.** p99 at roughly 3.3x p50, from both endpoints sharing one host.
Inherent to the topology, not a defect.

---

## 7. Quick reference

### Devices

| RDMA device | Netdev | Namespace | Address | MAC | Node GUID |
|---|---|---|---|---|---|
| `rocep1s0f0` | `enp1s0f0np0` | root | 192.168.100.1/24 | 04:3f:72:e8:46:5a | 043f720300e8465a |
| `rocep4s0f0` | `enp4s0f0np0` | `limen-b` | 192.168.100.2/24 | 04:3f:72:d5:46:c4 | (run `limen-b ibv_devices`) |
| `rocep1s0f1` | unused | root | | | 043f720300e8465b |
| `rocep4s0f1` | unused | root | | | 043f720300d546c5 |

Only the `f0` port of each card is cabled. The `f1` ports are idle.

### Constants

```
subnet          192.168.100.0/24
server addr     192.168.100.1
client addr     192.168.100.2
GID index       3          (both sides, RoCE v2, IPv4-mapped)
RoCE UDP port   4791
side channel    18515      (ibv_rc_pingpong default)
harness ports   18515 (TRD-02), 18600+ (TRD-03)
namespace       limen-b
CPU pins        core 2 (server), core 6 (client)
```

### Commands

```bash
# bring the fabric up by hand (the systemd unit does this at boot)
sudo limen-fabric-up.sh                # add --force to also kill Firefox

# check both namespaces, print both GID indices, ping across the DAC
sudo limen-verify

# run anything inside the client namespace
sudo limen-b <command>

# confirm the boot-time bringup worked
journalctl -u limen-fabric -b

# ---- verification pair, raw verbs ----
sudo ibv_rc_pingpong -d rocep1s0f0 -g 3
sudo limen-b ibv_rc_pingpong -d rocep4s0f0 -g 3 192.168.100.1

# ---- latency ----
sudo taskset -c 2 ib_send_lat -d rocep1s0f0 -x 3 -F
sudo limen-b taskset -c 6 ib_send_lat -d rocep4s0f0 -x 3 -F 192.168.100.1

# ---- bandwidth, always with --report_gbits ----
sudo taskset -c 2 ib_send_bw -d rocep1s0f0 -x 3 -F --report_gbits
sudo limen-b taskset -c 6 ib_send_bw -d rocep4s0f0 -x 3 -F --report_gbits 192.168.100.1

# ---- five-run bandwidth, prints the full labeled data row ----
for i in 1 2 3 4 5; do
  sudo taskset -c 2 ib_send_bw -d rocep1s0f0 -x 3 -F --report_gbits >/dev/null 2>&1 &
  sleep 1
  sudo limen-b taskset -c 6 ib_send_bw -d rocep4s0f0 -x 3 -F --report_gbits 192.168.100.1 \
    2>/dev/null | awk '/^ 65536/ {printf "peak=%s avg=%s msgrate=%s\n", $3, $4, $5}'
  sleep 2
done

# ---- connection manager path ----
sudo rping -s -a 192.168.100.1 -v -C 10
sudo limen-b rping -c -a 192.168.100.1 -v -C 10

# ---- own binaries, TRD-02 onward ----
sudo ./build/limen_connect -d rocep1s0f0 -g 3
sudo limen-b ./build/limen_connect -d rocep4s0f0 -g 3 192.168.100.1
```

### Diagnostics

```bash
ibv_devices                                  # devices visible in this namespace
ibv_devinfo -d rocep1s0f0                    # port state, link layer, MTU
rdma system show                             # netns mode: shared or exclusive
sudo lsns -t net                             # every net namespace, snaps included
ip neigh show dev enp1s0f0np0                # FAILED means no dmac, RTR will fail
ip route get 192.168.100.1                   # 'local' or 'dev lo' means the split is gone
show_gids                                    # GID table, if installed
sudo tcpdump -i enp1s0f0np0 -n udp port 4791 -c 20
```

### Symptom to cause

| Symptom | Cause |
|---|---|
| `Failed to modify QP to RTR` | namespace split gone, peer address resolving as local |
| `client read/write: No space left on device` | downstream of the above; the server died before writing its dest back |
| `Device or resource busy` on `netns exclusive` | a non-init net namespace exists (Firefox snaps, stale `limen-b`, daemons) |
| `setting the network namespace failed: Operation not permitted` | missing `sudo`; `ip netns exec` requires root |
| GID prints all zeroes | index selects an unpopulated table entry |
| ping works one direction only | firewall on the silent side |
| address gone after reboot | NetworkManager reclaimed the interface, or the unit lost the boot race |