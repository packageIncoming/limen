
## TRD-00: FABRIC UP

### Prior Knowledge Notes
* Normal networking: devices ask host CPU to move data to/from RAM
    - not very fast
* 3 main pillars of RDMA that go against normal networking: kernel bypass, zero copy, Remote CPU bypass
* Kernel bypass: work and completion queues are used to bypass kernel having to copy memory; kernel only active at initialization to secure memory & setup queues
    - Memory is mapped to process memory
* Zero copy: The (network) adapters are given physical memory addresses & write directly to them w/o socket buffers 
    - Kernel must guarantee that the physical addresses are secured & safe 
* Remote CPU bypass: unique to RDMA, in 1-sided communication, the sending adapter sends both the physical address to write to along with the data to write; receiving adapter goes and writes this data, with no notification/interrupt sent to the CPU. 
    - The remote CPU doesn't even know there was a transfer in 1-sided comms
    - In 2-sided comms, the receiving CPU makes a request, decides the physical address, and reaps a completion
* Key terms: Device context, protection domain, memory region, completion queue, queue pair, work request, and completion
* Verb API: post work to a work request queue, reap a completion later on
* RDMA originally built for Infiniband, RDMA over Converged Ethernet (RoCE) makes it more usable w/o Infiniband-specialized hardware/fabric
    -RoCE v1, v2; Limen uses v2
* Destination port = 4791, capturable by ```tcpdump```
* Infiniband uses GID (128 bit value) for addressing
* RoCE v2 requires lossless fabric (Priority Flow Control, Explicit Congestion Notification) since retransmission is very costly 
* Software RoCE v2 shipped in Linux under  ````rdma_rxe```, aka Soft-RoCE or just ```rxe```
    - Only difference compared to hardware is the name change (rxe->???)
    - But because it's software it uses the kernel & actually ends up slower than TCP because of the overhead; real RDMA MUCH faster
    - The coding & concepts remain the same
* The tools used in this TRD address the core steps of building up to RDMA & are basically a view into what we are making:
    - ibv_devinfo: "does a device exist & its port up?"
    - ibv_rc_pingpong: "can two nodes complete a transfer?"
    - rping: "does the connection manager path work?"
    - perftest: " how does this fabric perform?"
    


## Requirements Notes
S7- I ran 'sufo ufw disable'

R1- 
    -had to check h1 lv1
    -had to check h1 lv2 & run the cmd; show_gids does NOT ship with perftest
 
limen-node-0 GID: 1
all the indexes:
0  RoCE v2  fe80:0000:0000:0000:0a00:27ff:fe58:ef89
1  RoCE v2  0000:0000:0000:0000:0000:ffff:0a00:0001
2  RoCE v2  fe80:0000:0000:0000:dedb:4745:0a22:4f86

limen-node-1 GID: 1
all the indexes:
0  RoCE v2  fe80:0000:0000:0000:0a00:27ff:fe28:3db0
1  RoCE v2  0000:0000:0000:0000:0000:ffff:0a00:0002
2  RoCE v2  fe80:0000:0000:0000:e09c:aa29:e231:faad

I chose GID=1 for both nodes since the final 4 bytes (0a.00.00.02 or 0a.00.00.01) map to the IPv4 addresses of each node (node 0 is 10.0.0.1, node 1 is 10.0.0.2)

R2-
From node-1:
main@limen-node-1:~$ ibv_rc_pingpong -d rxe0 -g 1  -n 10
  local address:  LID 0x0000, QPN 0x000016, PSN 0x5f2537, GID ::ffff:10.0.0.2
  remote address: LID 0x0000, QPN 0x000016, PSN 0x466180, GID ::ffff:10.0.0.1
81920 bytes in 0.02 seconds = 30.79 Mbit/sec
10 iters in 0.02 seconds = 2128.80 usec/iter

And from node-0:
main@limen-node-0:~/limen$ ibv_rc_pingpong -d rxe0 -g 1 10.0.0.2  -n 10
  local address:  LID 0x0000, QPN 0x000016, PSN 0x466180, GID ::ffff:10.0.0.1
  remote address: LID 0x0000, QPN 0x000016, PSN 0x5f2537, GID ::ffff:10.0.0.2
81920 bytes in 0.02 seconds = 34.74 Mbit/sec
10 iters in 0.02 seconds = 1886.70 usec/iter

R3-
n0 acts as server, n1 as client
* What did rping ask? 
    - Was the device a server or client, the address to use (either to bind to as the server or to connect to as the client)
* What differed?
    - ibv_rc_pingpong wanted the GID and the device name while rping did not. I assume that this is what the Connection Manager does, i.e. discovering devices & the proper GID index to send messages over. Seems like the connection manager did it.

R4-
* I assume we have to use tcpdump
had to run ```sudo rdma link add rxe0 type rxe netdev enp0s8``` on both devices again to set up rxe0 
but then I ran ```ib_send_bw``` on node 0 and  ```ib_send_bw 10.0.0.1``` on node 1 

node0:
main@limen-node-0:~/limen$ ib_send_bw 
 WARNING: BW peak won't be measured in this run.

************************************
* Waiting for client to connect... *
************************************
---------------------------------------------------------------------------------------
                    Send BW Test
 Dual-port       : OFF          Device         : rxe0
 Number of qps   : 1            Transport type : IB
 Connection type : RC           Using SRQ      : OFF
 PCIe relax order: ON
 ibv_wr* API     : OFF
 RX depth        : 512
 CQ Moderation   : 1
 Mtu             : 1024[B]
 Link type       : Ethernet
 GID index       : 1
 Max inline data : 0[B]
 rdma_cm QPs     : OFF
 Data ex. method : Ethernet
---------------------------------------------------------------------------------------
 local address: LID 0000 QPN 0x0011 PSN 0xab87d6
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:00:00:01
 remote address: LID 0000 QPN 0x0019 PSN 0x7a9d52
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:00:00:02
---------------------------------------------------------------------------------------
 #bytes     #iterations    BW peak[MB/sec]    BW average[MB/sec]   MsgRate[Mpps]
 65536      1000             0.00               1.09        0.000018
---------------------------------------------------------------------------------------

node 1:
main@limen-node-1:~$ ib_send_bw 10.0.0.1
---------------------------------------------------------------------------------------
                    Send BW Test
 Dual-port       : OFF          Device         : rxe0
 Number of qps   : 1            Transport type : IB
 Connection type : RC           Using SRQ      : OFF
 PCIe relax order: ON
 ibv_wr* API     : OFF
 TX depth        : 128
 CQ Moderation   : 1
 Mtu             : 1024[B]
 Link type       : Ethernet
 GID index       : 1
 Max inline data : 0[B]
 rdma_cm QPs     : OFF
 Data ex. method : Ethernet
---------------------------------------------------------------------------------------
 local address: LID 0000 QPN 0x0019 PSN 0x7a9d52
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:00:00:02
 remote address: LID 0000 QPN 0x0011 PSN 0xab87d6
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:00:00:01
---------------------------------------------------------------------------------------
 #bytes     #iterations    BW peak[MB/sec]    BW average[MB/sec]   MsgRate[Mpps]
 65536      1000             2.14               0.93               0.000015
---------------------------------------------------------------------------------------

tcpdump gave these logs:
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

R4 ANS: since this stuff runs over the network, tcpdump is able to capture it. But on pure hardware this stuff would not be captured since it's not passed through the host kernel.


R2 (ibv_rc_pingpong cross-node) was removed from the gate. Raw-verbs RC data transfer across the fabric is already verified by R5 (ib_send_bw), which passes. ibv_rc_pingpong completes when both endpoints are launched in interactive shells but fails when the server is launched via non-interactive SSH (the harness's srv() path), while ib_send_bw and rping survive the same launch — isolating the failure to that tool's interaction with the non-interactive session environment, not the fabric or any project code. Retained as a manual sanity check; not gated.