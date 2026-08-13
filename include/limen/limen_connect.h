#ifndef limen_connect
#define limen_connect

#define SEND_QUEUE_DEPTH 16
#define RECV_QUEUE_DEPTH 16
#define COMPLETE_QUEUE_DEPTH (SEND_QUEUE_DEPTH + RECV_QUEUE_DEPTH)

typedef struct {
    const char* device_name{nullptr};
    uint64_t gid_index{UINT64_MAX};
    uint64_t port{1};
    uint64_t tcp_port{18515};
    uint64_t buffer_size{4096};
    const char* addr{nullptr};
} connect_parsed_args;

typedef struct {

} endpoint_identity;

void parse_argv(int arg, char* argv[], connect_parsed_args* args_container);

void print_help(bool to_error=false);

void assemble_endpoint_identity(endpoint_identity* e_id);

void print_endpoint_identity(endpoint_identity* e_id,char* buffer);

void generate_packet_sequence_number(uint32_t* psn);

void exchange_as_server();

void exchange_as_client();

void send_endpoint_identity(int fd, endpoint_identity* e_id);





#endif