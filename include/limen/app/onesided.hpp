#pragma once
#include <climits>
#include <infiniband/verbs.h>

#include "limen/cm.hpp"


#define SEND_QUEUE_DEPTH 16
#define RECV_QUEUE_DEPTH 16
#define COMPLETE_QUEUE_DEPTH (SEND_QUEUE_DEPTH + RECV_QUEUE_DEPTH)
#define U32_TO_U24_MASK (0x00FFFFFF)

constexpr uint64_t RECV_WRID_TAG  = 0x1ULL << 63;
constexpr uint64_t SEND_WRID_TAG  = 0x1ULL << 62;
constexpr uint64_t FLAG_WRID_TAG  = 0x1ULL << 61;


enum class onesided_mode {
    WRITE,
    READ,
    IMM,
    FLAG,
    LASTBYTE
};

typedef struct onesided_parsed_args {
    const char* device_name{nullptr};
    int port{1};
    uint64_t tcp_port{18515};
    uint64_t message_size{4096};
    uint64_t iterations{100};
    onesided_mode mode{onesided_mode::WRITE};
    bool bad_rkey{false};
    bool verbose{false};
    const char* peer{nullptr};
} onesided_parsed_args;

void parse_argv(int arg, char* argv[], onesided_parsed_args* args_container);

void print_help(bool to_error=false);

void print_reset_init_fail(int rc, ibv_qp_attr* qp_attr);

void print_init_rtr_fail(int rc, ibv_qp_attr* qp_attr);

void print_rtr_rts_fail(int rc, ibv_qp_attr* qp_attr);

int post_recv(uint32_t slot, uint64_t buff_addr, ibv_qp* queue_pair,  uint32_t message_size, uint32_t lkey);

//  primitive: one-sided RDMA at an explicit remote byte offset.
//  opcode must be IBV_WR_RDMA_WRITE or IBV_WR_RDMA_READ.
//  caller owns wr_id, the local address, and the length; nothing is derived.
//  caller is responsible for keeping [remote_offset, remote_offset+len)
//  inside peer_conninfo.length.
int post_rdma_at(
    ibv_wr_opcode opcode,
    uint64_t local_addr,
    uint32_t len,
    ibv_qp* queue_pair,
    uint32_t lkey,
    limen::ConnInfo peer_conninfo,
    uint64_t remote_offset,
    uint64_t wr_id,
    uint32_t imm_data
);

//  bypasses slot arithmetic, writes len bytes from local_addr
//  into peer_conninfo's address + remote_offset bytes
int post_write_at(
    uint64_t local_addr,
    uint32_t len,
    ibv_qp* queue_pair,
    uint32_t lkey,
    limen::ConnInfo peer_conninfo,
    uint64_t remote_offset,
    uint64_t wr_id
);

int post_write_imm_at(
    uint64_t local_addr,
    uint32_t len,
    ibv_qp* queue_pair,
    uint32_t lkey,
    limen::ConnInfo peer_conninfo,
    uint64_t remote_offset,
    uint64_t wr_id,
    uint32_t imm_data
);

//  bypasses slot arithmetic, reads len bytes from peer_conninfo's
//  address + remote_offset bytes into local_addr
int post_read_at(
    uint64_t local_addr,
    uint32_t len,
    ibv_qp* queue_pair,
    uint32_t lkey,
    limen::ConnInfo peer_conninfo,
    uint64_t remote_offset,
    uint64_t wr_id
);

//  slot-based wrappers. local and peer slot counts are independent,
//  so peer_slot is explicit rather than derived from local_slot.
//  wr_id is local_slot | SEND_WRID_TAG.
int post_send(
    uint32_t local_slot,
    uint32_t peer_slot,
    uint64_t buff_addr,
    ibv_qp* queue_pair,
    uint32_t message_size,
    uint32_t lkey,
    limen::ConnInfo peer_conninfo
);

int post_send_imm(
    uint32_t local_slot,
    uint32_t peer_slot,
    uint64_t buff_addr,
    ibv_qp* queue_pair,
    uint32_t message_size,
    uint32_t lkey,
    limen::ConnInfo peer_conninfo,
    uint32_t imm_data
);

int post_read(
    uint32_t local_slot,
    uint32_t peer_slot,
    uint64_t buff_addr,
    ibv_qp* queue_pair,
    uint32_t message_size,
    uint32_t lkey,
    limen::ConnInfo peer_conninfo
);


//  calls ec::wait(timeout)
limen::Event get_expected_event(limen::EventChannel& ec, rdma_cm_event_type event_type, int timeout_ms);

void fill_qp_init_attr(ibv_qp_init_attr* qp_init_attr, ibv_device_attr* device_attr, onesided_parsed_args* args);