#pragma once
#include <climits>
#include <infiniband/verbs.h>
#include "limen/cm.hpp"


#define SEND_QUEUE_DEPTH 16
#define RECV_QUEUE_DEPTH 16
#define COMPLETE_QUEUE_DEPTH (SEND_QUEUE_DEPTH + RECV_QUEUE_DEPTH)
#define U32_TO_U24_MASK (0x00FFFFFF)
#define SIDE_CHANNEL_MSG_SZ (sizeof("qpn=0xffffffff psn=0xffffff gid=0000:0000:0000:0000:0000:ffff:ffff:ffff lid=0xffff") - 1)

constexpr uint64_t RECV_WRID_TAG  = 0x1ULL << 63;
constexpr uint64_t SEND_WRID_TAG  = 0x1ULL << 62;
constexpr uint64_t SEND_MR_BYTE_CAP = 64ull << 20;   // 64 MiB of registered send memory

enum class reap_mode { POLL, EVENT };

typedef struct pingpong_parsed_args {
    const char* device_name{nullptr};
    int gid_index{INT_MAX};
    int port{1};
    uint64_t tcp_port{18515};
    uint64_t message_size{4096};
    uint64_t iterations{100};
    uint64_t rx_depth{8};
    int rnr_retry{7};
    bool no_recv{false};
    bool unsignaled{false};

    //  TRD-07
    bool      inline_data{false};        // --inline        ('inline' is a keyword)
    uint64_t  signal_every{1};           // --signal-every n, 1 = signal every send
    uint64_t  pipeline{1};               // --pipeline depth
    reap_mode reap{reap_mode::POLL};     // --reap poll|event
    bool      moderate{false};           // --moderate given at all
    uint64_t  moderate_count{0};         //   c of c:u
    uint64_t  moderate_usec{0};          //   u of c:u
    bool      report_config{false};      // --report-config, print and exit 0
    bool      broken_arming{false};      // --broken-arming, hidden, R8

    const char* addr{nullptr};
} pingpong_parsed_args;
void parse_argv(int arg, char* argv[], pingpong_parsed_args* args_container);

void print_help(bool to_error=false);

int post_send(
    bool signaled,
    uint32_t seq,
    uint32_t num_slots,
    uint64_t buff_addr, 
    ibv_qp* queue_pair, 
    uint32_t message_size,
    uint32_t lkey,
    bool inline_enabled
);

//  calls ec::wait(timeout)
limen::Event get_expected_event(limen::EventChannel& ec, rdma_cm_event_type event_type, int timeout_ms);

void fill_qp_init_attr(ibv_qp_init_attr* qp_init_attr, ibv_device_attr* device_attr, pingpong_parsed_args* args);
