#ifndef limen_connect
#define limen_connect

#define SEND_QUEUE_DEPTH 128
#define RECV_QUEUE_DEPTH 128

typedef struct {
    const char* device_name{nullptr};
    uint64_t gid_index{UINT64_MAX};
    uint64_t port{1};
    uint64_t tcp_port{18515};
    uint64_t buffer_size{4096};
    const char* addr{nullptr};
} connect_parsed_args;

void parse_argv(int arg, char* argv[], connect_parsed_args* args_container);

void print_help(bool to_error=false);

#endif