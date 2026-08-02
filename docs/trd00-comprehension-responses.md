Q1:
rxe0 is a software provider that presents the exact same RDMA interface as a hardware adapter. It implements RDMA via the kernel . The kernel is what exists in this SW that wouldn't exist on the regular HW implementation. Of the 3 properties of RDMA, this eliminates the kernel bypass and zero copy as a consequence.

Q2: UDP is the thinnest possible protocol that allows routing and port numbers to demultiplex RoCE traffic from other incoming traffic. The headers that InfiniBand provides act as the reliability layering while still bypassing the kernel; TCP is redundant since InfiniBand already supplies reliability, and because TCP connection state is managed by the kernel. The whole point of RDMA is to bypass the kernel, TCP actively requires it. 

Q3: GID = identification # used by RDMA; in RoCE it's derived from the IP address. Can have multiple attached to a single IP address b/c of different versions (RoCE v1 and v2) and can further increase based on if using IPv4 and/or IPv6 addresses. Rping did not ask for the GID because it has its own connection manager that finds the GID.

Q4: Kernel bypass. R4 packet capture succeeds because the kernel is involved with transporting over the network, it would not pick up over hardware RDMA because hardware bypasses the kernel entirely. It shows that this fabric has zero copy & DMA but not kernel bypass

Q5: TCP won leagues ahead of RDMA. This is because RDMA is being simulated on top of TCP so it requires strictly more work. On hardware this would be FAR faster since it bypasses the kernel entirely.

Q6: InfiniBand as the transport layer and Ethernet as the link layer is not a contradiction because one describes the transport protocl while the other describes the physical fabric/technology that the transport is being carried over. 

Q7: Each tests a different thing. R2 tests if verbs can be send properly. R3 tests if the connection manager is working. Failing both can muddy the reason, but failing one before/after the other makes it clear if it is a communication/verbs problem or a connection manager problem.

Q8: For a 1-sided operation, the remote CPU is not made aware of nor contributes to the data movement. The sender sends data to the remote NIC, the remote NIC writes to memory, and the CPU is not told or given a completion. The unique problem for one sided RDMA is that the remote CPU does not know that its memory was changed at all. The CPU inherently knows when data enters because it is expecting it (from the socket such as from read() or recv()).
