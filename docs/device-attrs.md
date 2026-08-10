device: rxe0
	guid: 0x0a0027fffe58ef89
	fw_ver: 0.0.0
	phys_port_cnt: 1
	max_qp: 1048560
	max_qp_wr: 1048576
	max_cq: 1048576
	max_cqe: 32767
	max_mr: 524287
	max_mr_size: 18446744073709551615
	max_sge: 32
	max_qp_rd_atom: 128
port 1:
	state: PORT_ACTIVE
	link_layer: Ethernet
	active_mtu: 1024
	max_msg_sz: 2147483648
	gid_tbl_len: 1024
pd: allocated
mr: addr=0x60b7ab57a600 length=1000 lkey=0x000025c5 rkey=0x000025c5 access=LOCAL_WRITE|REMOTE_READ|REMOTE_WRITE
access-check: REMOTE_WRITE without LOCAL_WRITE rejected: EINVAL (Invalid argument)
teardown: mr=ok pd=ok context=ok
