#include <string>
#ifndef limen_pingpong

#include <climits>
#include <infiniband/verbs.h>

#define limen_pingpong

#define SEND_QUEUE_DEPTH 16
#define RECV_QUEUE_DEPTH 16
#define COMPLETE_QUEUE_DEPTH (SEND_QUEUE_DEPTH + RECV_QUEUE_DEPTH)
#define U32_TO_U24_MASK (0x00FFFFFF)
#define SIDE_CHANNEL_MSG_SZ (sizeof("qpn=0xffffffff psn=0xffffff gid=0000:0000:0000:0000:0000:ffff:ffff:ffff lid=0xffff") - 1)

constexpr uint64_t RECV_WRID_TAG  = 0x1ULL << 63;
constexpr uint64_t SEND_WRID_TAG  = 0x1ULL << 62;



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
    const char* addr{nullptr};
} pingpong_parsed_args;

typedef struct {
    uint32_t qpn;
    uint32_t psn;   //  MUST BE 24 BIT, SCALE WITH U32_TO_U24_MASK
    ibv_gid gid;
    uint16_t lid;   //  FROM PORT ATTRIBUTES

} endpoint_identity;

void parse_argv(int arg, char* argv[], pingpong_parsed_args* args_container);

void print_help(bool to_error=false);



int exchange_as_server(
    int* local_socket_fd,
    int* remote_socket_fd,
    endpoint_identity* remote_identity,
    endpoint_identity* local_identity,
    int tcp_port
);

int exchange_as_client(
    int* local_socket_fd,
    endpoint_identity* remote_identity,
    endpoint_identity* local_identity,
    int tcp_port,
    const char* server_addr
);

// turns it into "qpn={qpn in hex} psn={psn in hex } gid={gid string} lid={lid in hex}"
std::string identity_to_str(endpoint_identity* identity);

int send_endpoint_identity(int fd, endpoint_identity* local_identity);

int recv_endpoint_identity(int fd, endpoint_identity* remote_identity);

void print_reset_init_fail(int rc, ibv_qp_attr* qp_attr);

void print_init_rtr_fail(int rc, ibv_qp_attr* qp_attr);

void print_rtr_rts_fail(int rc, ibv_qp_attr* qp_attr);

int post_recv(uint32_t slot, uint64_t buff_addr, ibv_qp* queue_pair,  uint32_t message_size, uint32_t lkey);

int post_send(bool signaled, uint32_t slot, uint64_t buff_addr, ibv_qp* queue_pair,  uint32_t message_size, uint32_t lkey);

std::string wc_to_str(ibv_wc *wc);

#endif