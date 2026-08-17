#ifndef limen_connect

#include <climits>
#include <infiniband/verbs.h>

#define limen_connect

#define SEND_QUEUE_DEPTH 16
#define RECV_QUEUE_DEPTH 16
#define COMPLETE_QUEUE_DEPTH (SEND_QUEUE_DEPTH + RECV_QUEUE_DEPTH)
#define U32_TO_U24_MASK (0x00FFFFFF)

typedef struct {
    const char* device_name{nullptr};
    int gid_index{INT_MAX};
    int port{1};
    uint64_t tcp_port{18515};
    uint64_t buffer_size{4096};
    const char* addr{nullptr};
} connect_parsed_args;

typedef struct {
    uint32_t qpn;
    uint32_t psn;   //  MUST BE 24 BIT, SCALE WITH U32_TO_U24_MASK
    ibv_gid gid;
    uint16_t lid;   //  FROM PORT ATTRIBUTES

} endpoint_identity;

void parse_argv(int arg, char* argv[], connect_parsed_args* args_container);

void print_help(bool to_error=false);


void generate_packet_sequence_number(uint32_t* psn);

int exchange_as_server();

int exchange_as_client();

void send_endpoint_identity(int fd, endpoint_identity* e_id);





#endif