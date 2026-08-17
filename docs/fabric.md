# Limen Fabric

**Live environment document.** Describes the fabric all Limen code builds and runs
against. Last updated 2026-08-16.

The soft-RoCE VM setup is superseded and archived in `baseline-rxe.md`. It remains usable
for development without hardware access, but no number from it is quotable.

---

## 1. Concepts

Notes from TRD-00 prior knowledge, kept because they are the vocabulary everything
downstream assumes.

**Conventional networking** has the device interrupt the CPU to move data into and out of
RAM through socket buffers. Every byte is touched by the kernel at least once.

**RDMA rests on three bypasses:**

- **Kernel bypass.** Work queues and completion queues are mapped into process memory. The
  kernel participates at setup — securing memory, creating queues — and then leaves the
  data path entirely.
- **Zero copy.** The adapter is given addresses and DMAs directly to and from them. No
  socket buffers. The kernel's job is to guarantee those addresses stay valid, which is
  what pinning accomplishes.
- **Remote CPU bypass.** Unique to RDMA. In a one-sided operation the initiator supplies
  both the data and the destination address; the responding adapter performs the write with
  no interrupt and no notification. The remote CPU does not know a transfer occurred. In
  two-sided operation the receiver posts a receive, chooses the buffer, and reaps a
  completion.

**Objects:** device context, protection domain, memory region, completion queue, queue
pair, work request, completion.

**The verbs model:** post a work request to a queue, reap a completion later. Everything
is asynchronous by construction.

**RoCE.** RDMA was built for InfiniBand. RDMA over Converged Ethernet carries it on
ordinary Ethernet. Limen uses **RoCE v2**, which encapsulates in UDP on **port 4791**.
RoCE v1 is a different encapsulation (Ethertype 0x8915) and is not routable; selecting a
v1 GID by accident is a real failure mode — see §5.

**Addressing** is by GID, a 128-bit value.

**Lossless fabric.** RoCE v2 conventionally requires PFC and ECN because retransmission is
expensive. This fabric is direct-attach back-to-back with no switch, so there is no
oversubscription and no congestion to manage, and none of that is configured. That is a
limitation to disclose, not a result to claim — see §8.

**TRD-00's tools map to the build:**

| Tool | Question it answers |
|---|---|
| `ibv_devinfo` | Does a device exist, and is its port up? |
| `ibv_rc_pingpong` | Can two endpoints complete a transfer? |
| `rping` | Does the connection manager path work? |
| `perftest` | How does this fabric perform? |

---

## 2. Topology

Single host, two physically separate HCAs joined by a direct-attach copper cable.

**This is not loopback.** Each transfer crosses two independent PCIe links, two ASICs, and
a 25G SerDes wire. The two RDMA devices have separate contexts, separate protection
domains, and separate queue pairs, exactly as two hosts would.

| | |
|---|---|
| Host | Dell OptiPlex 7050 MT, Ubuntu 24.04 Desktop, UEFI boot |
| Card A | ConnectX-4 Lx, Dell OEM CX4121C, PCI `01:00.0`, SLOT2 |
| Card B | ConnectX-4 Lx, Dell OEM CX4121C, PCI `04:00.0`, SLOT4 |
| Cable | 10Gtek CAB-ZSP/ZSP-P0.3M, 25G SFP28 passive DAC, 0.3 m, 30 AWG |
| Link | Card A port 0 ↔ Card B port 0 |

---

## 3. PCIe

Card A: `LnkSta: Speed 8GT/s, Width x8` — Gen3 x8, CPU-attached.
Card B: SLOT4 is x16 mechanical, x4 electrical, PCH-attached behind DMI.

Verify with:

```bash
sudo lspci -vvv -s 01:00.0 | grep -i lnksta
```

`sudo` is required. Without it `lspci` prints `Capabilities: <access denied>` and the
`LnkSta` line is absent entirely rather than wrong.

**Known asymmetry.** Card A has roughly 63 Gb/s of PCIe budget, card B roughly 31.5 Gb/s.
Neither binds at 25G line rate for the message sizes measured, but the endpoints are not
equivalent and this belongs in any writeup.

---

## 4. Cards: firmware and configuration

| | Card A | Card B |
|---|---|---|
| PCI | `01:00.0` / `01:00.1` | `04:00.0` / `04:00.1` |
| FW version | 14.27.6122 | 14.28.4512 |
| FW release | 2020-08-12 | 2020-01-10 |
| PSID | DEL2420110034 | DEL2420110034 |
| UEFI ROM | 14.20.27 | 14.22.15 |
| PXE ROM | 3.5.903 | 3.6.203 |
| Base MAC | `04:3f:72:d5:46:c4` | `04:3f:72:e8:46:5a` |

**Known asymmetry.** One minor revision apart. Same PSID, so both accept the same Dell
image if matching ever becomes necessary. Deliberately not matched: flashing a working card
carries brick risk for no observed benefit. Revisit only if endpoint behaviour diverges in
a way application code does not explain.

**Tooling.** Ubuntu ships `mstflint`, which provides `mstflint` and `mstconfig` — the
open-source equivalents of MFT's `flint` and `mlxconfig`. No tarball download, no kernel
module, and devices are addressed by PCI address rather than through `/dev/mst`.

```bash
sudo apt install -y mstflint
sudo mstflint  -d 01:00.0 query
sudo mstconfig -d 01:00.0 query
```

**Changes applied:**

```bash
sudo mstconfig -d 04:00.0 set EXP_ROM_PXE_ENABLE=0 EXP_ROM_UEFI_x86_ENABLE=0
```

Card A's older firmware does not expose those tokens. Settings apply on **cold power
cycle**, not on reboot. All `mstconfig` changes are reversible with
`sudo mstconfig -d <addr> reset`.

**SR-IOV was already disabled** on both cards out of the box (`SRIOV_EN False(0)`,
`NUM_OF_VFS 0`), contrary to the ConnectX-4 Lx default documented by Red Hat.

**`mstflint drom` is unavailable on these cards.** Dell builds the firmware with a unified
FW/ROM product version:

```
-E- Remove ROM failed: The device FW contains common FW/ROM Product Version -
    The ROM cannot be updated separately.
```

The expansion ROM can be disabled but not removed. Card configuration is a flag; the ROM
image stays in flash and the 1 MB expansion ROM BAR is still advertised.

---

## 5. Devices, addressing, GIDs

### Naming

Inbox `rdma-core` on 24.04 uses persistent PCI-based names, **not** `mlx5_N`:

| RDMA device | netdev | IP | Status |
|---|---|---|---|
| `rocep1s0f0` | `enp1s0f0np0` | 192.168.100.1/24 | PORT_ACTIVE, cabled |
| `rocep1s0f1` | `enp1s0f1np1` | — | PORT_DOWN |
| `rocep4s0f0` | `enp4s0f0np0` | 192.168.100.2/24 | PORT_ACTIVE, cabled |
| `rocep4s0f1` | `enp4s0f1np1` | — | PORT_DOWN |

**`rdma link add` does not apply.** The mlx5 driver creates devices automatically at probe.
Any TRD step carried over from the rxe setup that creates a device by hand is obsolete.

### GID selection

`show_gids` is not present — it ships with MLNX_OFED, not inbox `rdma-core`. Enumerate via
sysfs:

```bash
for d in /sys/class/infiniband/*/ports/1/gids/*; do
  gid=$(cat $d)
  [ "$gid" = "0000:0000:0000:0000:0000:0000:0000:0000" ] && continue
  idx=$(basename $d)
  dev=$(echo $d | cut -d/ -f5)
  type=$(cat /sys/class/infiniband/$dev/ports/1/gid_attrs/types/$idx 2>/dev/null)
  ndev=$(cat /sys/class/infiniband/$dev/ports/1/gid_attrs/ndevs/$idx 2>/dev/null)
  echo "$dev idx=$idx type=$type ndev=$ndev gid=$gid"
done
```

Current table for the two cabled ports:

```
rocep1s0f0 idx=0 IB/RoCE v1  fe80::63f:72ff:fee8:465a
rocep1s0f0 idx=1 RoCE v2     fe80::63f:72ff:fee8:465a
rocep1s0f0 idx=2 IB/RoCE v1  ::ffff:192.168.100.1
rocep1s0f0 idx=3 RoCE v2     ::ffff:192.168.100.1
rocep4s0f0 idx=0 IB/RoCE v1  fe80::63f:72ff:fed5:46c4
rocep4s0f0 idx=1 RoCE v2     fe80::63f:72ff:fed5:46c4
rocep4s0f0 idx=2 IB/RoCE v1  ::ffff:192.168.100.2
rocep4s0f0 idx=3 RoCE v2     ::ffff:192.168.100.2
```

**Use index 3 on both cards.** RoCE v2 with the IPv4-mapped GID.

Index 1 is also RoCE v2 but carries the link-local IPv6 GID. Indices 0 and 2 are RoCE v1 —
a different encapsulation entirely, and not to be used. This is the substantive difference
from rxe, which reported every entry as RoCE v2 and made the choice free.

**RoCE v2 requires** `ah_attr.is_global = 1` with the GRH populated (`sgid_index`, remote
`dgid`). Omitting it is a leading cause of `EINVAL` on the INIT → RTR transition.

### Wire inspection

**`tcpdump` captures nothing.** Kernel bypass means the host stack never sees the frames.
TRD-00 R4 predicted this correctly. Alternatives:

```bash
ethtool -S enp1s0f0np0 | grep -iE 'rx_packets|tx_packets|rx_bytes|tx_bytes'
```

Adapter counters confirm traffic moved. Actual wire-format inspection would need a switch
mirror or a tap, neither of which exists here. For wire-format questions, use the rxe VMs —
that is the one thing they do better than the hardware.

---

## 6. Baseline measurements

Recorded 2026-08-16. `perftest` from Ubuntu 24.04 inbox packages. All runs `-x 3 -F`,
MTU 1024, RC, 1 QP.

```bash
# server
ib_send_lat -d rocep1s0f0 -x 3 -F
# client
ib_send_lat -d rocep4s0f0 -x 3 -F 192.168.100.1
```

### Latency — `ib_send_lat`, 2 bytes, 1000 iterations, max inline 236 B

| | t_min | t_typical | t_avg | stdev | p99 | p99.9 |
|---|---|---|---|---|---|---|
| A → B | 1.05 µs | **1.12 µs** | 1.23 µs | 0.13 | 2.43 µs | 3.48 µs |
| B → A | 1.05 µs | **1.13 µs** | 1.23 µs | 0.12 | 2.35 µs | 3.64 µs |

`Max inline data: 236[B]` — sends under 236 bytes ride inside the work request itself and
the adapter never issues a separate DMA read for the payload. That is why the 2-byte
number is as low as it is, and it is the correct thing to say when asked.

### Bandwidth — `ib_send_bw`, 64 KB, 1000 iterations

| | BW avg | Gb/s | % of 25G line rate |
|---|---|---|---|
| A → B | 2462.21 MB/s | 19.70 | 78.8% |
| B → A | 2467.89 MB/s | 19.74 | 79.0% |

Symmetric in both directions, confirming the PCIe asymmetry in §3 is not binding at this
message size.

---

## 7. Hardware vs soft-RoCE

| | soft-RoCE (VMs) | Hardware | Ratio |
|---|---|---|---|
| Latency, typical | 724 µs | 1.13 µs | ~640x |
| Latency, p99.9 | 37,858 µs | 3.64 µs | ~10,400x |
| Bandwidth | 3.06 MB/s | 2462 MB/s | ~805x |

**Do not quote these ratios without the caveat.** More than one variable changed: the rxe
numbers came from VirtualBox guests on a laptop over a host-only adapter, so hypervisor
scheduling contributes a large and unquantified share of that 640x. A fair claim is
"hardware RoCE at 1.1 µs typical against a soft-RoCE development setup in the hundreds of
microseconds." Presenting 640x as the RDMA-versus-emulation figure will draw a correct
objection from anyone who knows the field.

### Device limits: hardware is *smaller* in places

| Attribute | rxe0 | ConnectX-4 Lx | Note |
|---|---|---|---|
| `max_qp_wr` | 1,048,576 | **8,192** | 128x smaller |
| `max_sge` | 32 | **30** | |
| `max_qp_rd_atom` | 128 | **16** | 8x smaller |
| `max_msg_sz` | 2 GiB | **1 GiB** | half |
| `max_cqe` | 32,767 | 4,194,303 | larger |
| `max_cq` | 1,048,576 | 16,777,216 | larger |
| `max_mr` | 524,287 | 16,777,216 | larger |

**This is a trap.** Any queue depth, SGE count, or outstanding-read count sized against
rxe's advertised maxima will fail on real hardware. `max_qp_rd_atom` matters from TRD-06,
where outstanding RDMA READs are limited by responder resources. Query the device and size
against what it reports; never hardcode.

---

## 8. Caveats on all numbers from this fabric

**Single host.** Both endpoints share CPU cores and one memory controller. Median latency
is representative of a true two-node fabric; **tail latency is not**. p99 sits near 2x the
median and p99.9 near 3x, wider than two hosts with core pinning would produce. State this
whenever the numbers are presented.

**MTU 1024.** The netdev MTU is at its default, so the RoCE path MTU is 1024 B. Raising the
netdev MTU to 4200 permits a 4096 B path MTU and should recover part of the gap to line
rate. Not yet done.

**No PFC, DCB, or ECN.** Direct-attach back-to-back, no switch, no oversubscription. This
sidesteps the hardest part of production RoCE deployment. A limitation, not a result.

**No CPU isolation.** No `taskset`, no `isolcpus`, governor not pinned to performance.
Doing these should tighten the tail.

**Firmware asymmetry** between the two endpoints, per §4.

---

## 9. Host setup

### Packages

```bash
sudo apt install -y rdma-core ibverbs-utils rdmacm-utils perftest \
  libibverbs-dev librdmacm-dev build-essential cmake ninja-build git \
  gdb clang clang-format clang-tidy valgrind tmux tcpdump iperf3 \
  mstflint linux-headers-$(uname -r) openssh-server
```

### Persistent addressing

`/etc/netplan/99-limen.yaml`, mode 600:

```yaml
network:
  version: 2
  ethernets:
    enp1s0f0np0:
      addresses: [192.168.100.1/24]
    enp4s0f0np0:
      addresses: [192.168.100.2/24]
```

```bash
sudo chmod 600 /etc/netplan/*.yaml
sudo netplan apply
```

Note: `sudo echo "..." >> /etc/file` does not work — the shell opens the redirect as your
user before `sudo` runs. Use `sudo tee` with a heredoc.

### Locked memory

Registration pins pages and the default `RLIMIT_MEMLOCK` will cause `ibv_reg_mr` failures
that read as application bugs. In `/etc/security/limits.conf`:

```
mert soft memlock unlimited
mert hard memlock unlimited
```

Log out and back in; confirm with `ulimit -l`.

### Firewall

Not relevant. RDMA traffic bypasses the kernel network stack and netfilter never sees it.
The `ufw disable` step from the rxe setup exists only because rxe ran over the kernel's own
UDP path.

### Internet

Via iPhone USB tethering — `ipheth`, interface `enx*`. The onboard Intel NIC
(`enp0s31f6`) has no cable. `dhclient` was removed in 24.04; use
`nmcli device connect <iface>`. The tether interface shows `NO-CARRIER` until Personal
Hotspot is toggled on with the phone unlocked.

### Verification sequence

```bash
lspci | grep -i Mellanox                       # 4 functions
sudo lspci -vvv -s 01:00.0 | grep -i lnksta    # 8GT/s x8
ip -br link                                    # both cabled ports UP
sudo ethtool enp1s0f0np0 | grep -E "Speed|Link detected"   # 25000Mb/s, yes
ibv_devices                                    # 4 roce* devices
ibv_devinfo | grep -E "hca_id|state|link_layer"            # 2x PORT_ACTIVE
ib_send_lat -d rocep1s0f0 -x 3 -F              # + client, see §6
```

---

## 10. Failed: HP Z240 SFF as a second node

The Z240 will not POST with a ConnectX-4 Lx installed. HP diagnostic code **3.2** — three
red blinks, two white, repeating — meaning the embedded controller timed out waiting for
BIOS to return from memory initialisation.

Ruled out, one variable per boot:

- Both cards, both full-length slots (x16 electrical and x4 electrical)
- BIOS 01.35 and 01.92 (latest, Aug 2024, SoftPaq `sp154266`, image `N51_0192.bin`)
- Early PCIe Delay enabled
- Legacy Support / CSM disabled, pure UEFI
- Slot 1 Option ROM Download unchecked
- Slot 1 Speed forced to Gen1
- Expansion ROMs disabled card-side via `mstconfig`
- SR-IOV confirmed already disabled

**Leading hypothesis:** the SMBus pins, B5 (SMCLK) and B6 (SMDAT). ConnectX-4 Lx supports
BMC manageability over MCTP on SMBus, and HP's embedded controller owns that bus. A Z240
owner with a different multi-port NIC reported the identical code and resolved it by
masking B5/B6 with tape. Not attempted — physical modification of the cards is out of
scope.

HP's own Z240 SFF spec sheet states that when the x16 slot is not driving graphics, only
cards certified as after-market options for the platform are supported.

**Resolution:** a second node will be a used OptiPlex 7050, or a 7040/7060/7070 with a
CPU-attached x16 slot, where this card is confirmed working. The Z240 remains a Windows
workstation and SSH client.

**Migration to two hosts,** when that happens: change the device name and the peer IP.
Nothing else. The code is identical.

### BIOS notes worth keeping

- Dell 7050: F2 for setup, F12 for the one-time boot menu. Boot List Option must be UEFI,
  not Legacy, or a UEFI Ubuntu install is invisible and the machine falls through to PXE
  (`PXE-E61: Media test failure` is the onboard NIC with no cable, not the Mellanox card).
- HP Z240: F10 for setup. BIOS images must sit at `Hewlett-Packard\BIOS\New\`,
  `EFI\HP\BIOS\New\`, or the `Previous` equivalents on a FAT32 stick. `HP\BIOS\New\` is
  not searched.