
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
* Destination port = 4791, capturable by '''tcpdump'''
* Infiniband uses GID (128 bit value) for addressing
* RoCE v2 requires lossless fabric (Priority Flow Control, Explicit Congestion Notification) since retransmission is very costly 
* Software RoCE v2 shipped in Linux under  ''rdma_rxe''', aka Soft-RoCE or just '''rxe'''
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

