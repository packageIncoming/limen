R5 Measure the Fabric:

results of ib_send_bw:

n0:
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
 local address: LID 0000 QPN 0x0013 PSN 0x37cfaa
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:00:00:01
 remote address: LID 0000 QPN 0x001b PSN 0x267f33
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:00:00:02
---------------------------------------------------------------------------------------
 #bytes     #iterations    BW peak[MB/sec]    BW average[MB/sec]   MsgRate[Mpps]
 65536      1000             0.00               3.06                      0.000049
---------------------------------------------------------------------------------------

n1:
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
 local address: LID 0000 QPN 0x001b PSN 0x267f33
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:00:00:02
 remote address: LID 0000 QPN 0x0013 PSN 0x37cfaa
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:00:00:01
---------------------------------------------------------------------------------------
 #bytes     #iterations    BW peak[MB/sec]    BW average[MB/sec]   MsgRate[Mpps]
 65536      1000             3.75               3.06                     0.000049
---------------------------------------------------------------------------------------

Results of ib_send_lat:

n0:
************************************
* Waiting for client to connect... *
************************************
---------------------------------------------------------------------------------------
                    Send Latency Test
 Dual-port       : OFF          Device: rxe0
 Number of qps   : 1            Transport type: IB
 Connection type : RC           Using SRQ: OFF
 PCIe relax order: ON
 ibv_wr* API     : OFF
 RX depth        : 512
 Mtu             : 1024[B]
 Link type       : Ethernet
 GID index       : 1
 Max inline data : 0[B]
 rdma_cm QPs     : OFF
 Data ex. method : Ethernet
---------------------------------------------------------------------------------------
 local address: LID 0000 QPN 0x0014 PSN 0xa73d24
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:00:00:01
 remote address: LID 0000 QPN 0x001c PSN 0x91cabc
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:00:00:02
---------------------------------------------------------------------------------------
 #bytes #iterations    t_min[usec]    t_max[usec]  t_typical[usec]    t_avg[usec]    t_stdev[usec]   99% percentile[usec]   99.9% percentile[usec] 
 2       1000          453.06         37330.73     692.41            928.05           2465.901216.65                 37330.73
---------------------------------------------------------------------------------------
n1:
---------------------------------------------------------------------------------------
                    Send Latency Test
 Dual-port       : OFF          Device         : rxe0
 Number of qps   : 1            Transport type : IB
 Connection type : RC           Using SRQ      : OFF
 PCIe relax order: ON
 ibv_wr* API     : OFF
 TX depth        : 1
 Mtu             : 1024[B]
 Link type       : Ethernet
 GID index       : 1
 Max inline data : 0[B]
 rdma_cm QPs     : OFF
 Data ex. method : Ethernet
---------------------------------------------------------------------------------------
 local address: LID 0000 QPN 0x001c PSN 0x91cabc
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:00:00:02
 remote address: LID 0000 QPN 0x0014 PSN 0xa73d24
 GID: 00:00:00:00:00:00:00:00:00:00:255:255:10:00:00:01
---------------------------------------------------------------------------------------
 #bytes #iterations    t_min[usec]    t_max[usec]  t_typical[usec]    t_avg[usec]    t_stdev[usec]   99% percentile[usec]   99.9% percentile[usec] 
 2       1000          461.98         37858.01      724.36           927.05           2486.36         1084.09               37858.01
---------------------------------------------------------------------------------------


bandwidth: 3MB/sec
latency: ~724usec

R6 comparison vs tcp:

explanation: Between the two, regular TCP blows "RDMA" out of the water: TCP achieves about 1.2 Gbits/sec, while RDMA achieves about 3 MB/sec of bandwith. That's to be expected, though, since the RDMA we are doing is not hardware RDMA but instead software-simulated. This brings with it a lot of overhead from simulating RDMA AND THEN using TCP anyway; it will always be strictly more work than regular TCP.

Ran ```iperf3``` on node 1, results:
main@limen-node-1:~$ iperf3 -c 10.0.0.1
Connecting to host 10.0.0.1, port 5201
[  5] local 10.0.0.2 port 46486 connected to 10.0.0.1 port 5201
[ ID] Interval           Transfer     Bitrate         Retr  Cwnd
[  5]   0.00-1.00   sec   147 MBytes  1.23 Gbits/sec  184    280 KBytes       
[  5]   1.00-2.00   sec   160 MBytes  1.33 Gbits/sec   54    416 KBytes       
[  5]   2.00-3.00   sec   157 MBytes  1.32 Gbits/sec  310    287 KBytes       
[  5]   3.00-4.00   sec   158 MBytes  1.32 Gbits/sec   90    506 KBytes       
[  5]   4.00-5.00   sec   141 MBytes  1.19 Gbits/sec  180    283 KBytes       
[  5]   5.00-6.00   sec   153 MBytes  1.28 Gbits/sec    0    566 KBytes       
[  5]   6.00-7.00   sec   158 MBytes  1.33 Gbits/sec   90    469 KBytes       
[  5]   7.00-8.00   sec   162 MBytes  1.36 Gbits/sec  276    317 KBytes       
[  5]   8.00-9.00   sec   152 MBytes  1.28 Gbits/sec  180    272 KBytes       
[  5]   9.00-10.01  sec   155 MBytes  1.29 Gbits/sec   45    519 KBytes       
- - - - - - - - - - - - - - - - - - - - - - - - -
[ ID] Interval           Transfer     Bitrate         Retr
[  5]   0.00-10.01  sec  1.51 GBytes  1.29 Gbits/sec  1409             sender
[  5]   0.00-10.01  sec  1.50 GBytes  1.29 Gbits/sec                  receiver



R7 Capture the Device Attributes results:

from n0:
hca_id:	rxe0
	transport:			InfiniBand (0)
	fw_ver:				0.0.0
	node_guid:			0a00:27ff:fe58:ef89
	sys_image_guid:			0a00:27ff:fe58:ef89
	vendor_id:			0xffffff
	vendor_part_id:			0
	hw_ver:				0x0
	phys_port_cnt:			1
	max_mr_size:			0xffffffffffffffff
	page_size_cap:			0xfffff000
	max_qp:				1048560
	max_qp_wr:			1048576
	device_cap_flags:		0x01223c76
					BAD_PKEY_CNTR
					BAD_QKEY_CNTR
					AUTO_PATH_MIG
					CHANGE_PHY_PORT
					UD_AV_PORT_ENFORCE
					PORT_ACTIVE_EVENT
					SYS_IMAGE_GUID
					RC_RNR_NAK_GEN
					SRQ_RESIZE
					MEM_WINDOW
					MEM_MGT_EXTENSIONS
					MEM_WINDOW_TYPE_2B
	max_sge:			32
	max_sge_rd:			32
	max_cq:				1048576
	max_cqe:			32767
	max_mr:				524287
	max_pd:				1048576
	max_qp_rd_atom:			128
	max_ee_rd_atom:			0
	max_res_rd_atom:		258048
	max_qp_init_rd_atom:		128
	max_ee_init_rd_atom:		0
	atomic_cap:			ATOMIC_HCA (1)
	max_ee:				0
	max_rdd:			0
	max_mw:				524287
	max_raw_ipv6_qp:		0
	max_raw_ethy_qp:		0
	max_mcast_grp:			8192
	max_mcast_qp_attach:		56
	max_total_mcast_qp_attach:	458752
	max_ah:				32767
	max_fmr:			0
	max_srq:			917503
	max_srq_wr:			1048576
	max_srq_sge:			27
	max_pkeys:			64
	local_ca_ack_delay:		15
	general_odp_caps:
					ODP_SUPPORT
	rc_odp_caps:
					SUPPORT_SEND
					SUPPORT_RECV
					SUPPORT_WRITE
					SUPPORT_READ
					SUPPORT_ATOMIC
					SUPPORT_SRQ
					Unknown flags: 0xC0
	uc_odp_caps:
					NO SUPPORT
	ud_odp_caps:
					SUPPORT_SEND
					SUPPORT_RECV
					SUPPORT_SRQ
	xrc_odp_caps:
					NO SUPPORT
	completion_timestamp_mask not supported
	core clock not supported
	device_cap_flags_ex:		0x1C001223C76
					Unknown flags: 0x1C000000000
	tso_caps:
		max_tso:			0
	rss_caps:
		max_rwq_indirection_tables:			0
		max_rwq_indirection_table_size:			0
		rx_hash_function:				0x0
		rx_hash_fields_mask:				0x0
	max_wq_type_rq:			0
	packet_pacing_caps:
		qp_rate_limit_min:	0kbps
		qp_rate_limit_max:	0kbps
	tag matching not supported
	num_comp_vectors:		2
		port:	1
			state:			PORT_ACTIVE (4)
			max_mtu:		4096 (5)
			active_mtu:		1024 (3)
			sm_lid:			0
			port_lid:		0
			port_lmc:		0x00
			link_layer:		Ethernet
			max_msg_sz:		0x80000000
			port_cap_flags:		0x00010000
			port_cap_flags2:	0x0000
			max_vl_num:		1 (1)
			bad_pkey_cntr:		0x0
			qkey_viol_cntr:		0x0
			sm_sl:			0
			pkey_tbl_len:		1
			gid_tbl_len:		1024
			subnet_timeout:		0
			init_type_reply:	0
			active_width:		1X (1)
			active_speed:		2.5 Gbps (1)
			phys_state:		LINK_UP (5)
			GID[  0]:		fe80::a00:27ff:fe58:ef89, RoCE v2
			GID[  1]:		::ffff:10.0.0.1, RoCE v2
			GID[  2]:		fe80::dedb:4745:a22:4f86, RoCE v2

from n1:
hca_id: rxe0
        transport:                      InfiniBand (0)
        fw_ver:                         0.0.0
        node_guid:                      0a00:27ff:fe28:3db0
        sys_image_guid:                 0a00:27ff:fe28:3db0
        vendor_id:                      0xffffff
        vendor_part_id:                 0
        hw_ver:                         0x0
        phys_port_cnt:                  1
        max_mr_size:                    0xffffffffffffffff
        page_size_cap:                  0xfffff000
        max_qp:                         1048560
        max_qp_wr:                      1048576
        device_cap_flags:               0x01223c76
                                        BAD_PKEY_CNTR
                                        BAD_QKEY_CNTR
                                        AUTO_PATH_MIG
                                        CHANGE_PHY_PORT
                                        UD_AV_PORT_ENFORCE
                                        PORT_ACTIVE_EVENT
                                        SYS_IMAGE_GUID
                                        RC_RNR_NAK_GEN
                                        SRQ_RESIZE
                                        MEM_WINDOW
                                        MEM_MGT_EXTENSIONS
                                        MEM_WINDOW_TYPE_2B
        max_sge:                        32
        max_sge_rd:                     32
        max_cq:                         1048576
        max_cqe:                        32767
        max_mr:                         524287
        max_pd:                         1048576
        max_qp_rd_atom:                 128
        max_ee_rd_atom:                 0
        max_res_rd_atom:                258048
        max_qp_init_rd_atom:            128
        max_ee_init_rd_atom:            0
        atomic_cap:                     ATOMIC_HCA (1)
        max_ee:                         0
        max_rdd:                        0
        max_mw:                         524287
        max_raw_ipv6_qp:                0
        max_raw_ethy_qp:                0
        max_mcast_grp:                  8192
        max_mcast_qp_attach:            56
        max_total_mcast_qp_attach:      458752
        max_ah:                         32767
        max_fmr:                        0
        max_srq:                        917503
        max_srq_wr:                     1048576
        max_srq_sge:                    27
        max_pkeys:                      64
        local_ca_ack_delay:             15
        general_odp_caps:
                                        ODP_SUPPORT
        rc_odp_caps:
                                        SUPPORT_SEND
                                        SUPPORT_RECV
                                        SUPPORT_WRITE
                                        SUPPORT_READ
                                        SUPPORT_ATOMIC
                                        SUPPORT_SRQ
                                        Unknown flags: 0xC0
        uc_odp_caps:
                                        NO SUPPORT
        ud_odp_caps:
                                        SUPPORT_SEND
                                        SUPPORT_RECV
                                        SUPPORT_SRQ
        xrc_odp_caps:
                                        NO SUPPORT
        completion_timestamp_mask not supported
        core clock not supported
        device_cap_flags_ex:            0x1C001223C76
                                        Unknown flags: 0x1C000000000
        tso_caps:
                max_tso:                        0
        rss_caps:
                max_rwq_indirection_tables:                     0
                max_rwq_indirection_table_size:                 0
                rx_hash_function:                               0x0
                rx_hash_fields_mask:                            0x0
        max_wq_type_rq:                 0
        packet_pacing_caps:
                qp_rate_limit_min:      0kbps
                qp_rate_limit_max:      0kbps
        tag matching not supported
        num_comp_vectors:               2
                port:   1
                        state:                  PORT_ACTIVE (4)
                        max_mtu:                4096 (5)
                        active_mtu:             1024 (3)
                        sm_lid:                 0
                        port_lid:               0
                        port_lmc:               0x00
                        link_layer:             Ethernet
                        max_msg_sz:             0x80000000
                        port_cap_flags:         0x00010000
                        port_cap_flags2:        0x0000
                        max_vl_num:             1 (1)
                        bad_pkey_cntr:          0x0
                        qkey_viol_cntr:         0x0
                        sm_sl:                  0
                        pkey_tbl_len:           1
                        gid_tbl_len:            1024
                        subnet_timeout:         0
                        init_type_reply:        0
                        active_width:           1X (1)
                        active_speed:           2.5 Gbps (1)
                        phys_state:             LINK_UP (5)
                        GID[  0]:               fe80::a00:27ff:fe28:3db0, RoCE v2
                        GID[  1]:               ::ffff:10.0.0.2, RoCE v2
                        GID[  2]:               fe80::e09c:aa29:e231:faad, RoCE v2

Other stuff:
rxe0 is bound to enp0s8
disabled the firewall
To re-create the link (rxe0):
sudo rdma link add rxe0 type rxe netdev enp0s8
