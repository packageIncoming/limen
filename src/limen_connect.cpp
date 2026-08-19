#include <cstddef>
#include <cstdio>
#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#include <iostream>
#include <algorithm> 
#include <format>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>

#include <infiniband/verbs.h>
#include "limen/limen_common.h"
#include "limen/limen_connect.h"

void parse_argv(int argc, char* argv[],connect_parsed_args* args)
{
    int opt;
    while ((opt = getopt(argc, argv, "d:g:p:t:s:h")) != -1)
    {
        switch (opt)
        {
            case 0:
            {
                break;
            }
            case 'd':
            {
                args->device_name = optarg;
                break;
            }
            case 'g':
            {
                int rc = parse_int_strict(optarg,&args->gid_index);
                if (rc != 0)
                {
                    exit(1);
                }
                break;
            }
            case 'p': 
            {
                int rc = parse_int_strict(optarg,&args->port);
                if (rc != 0)
                {
                    exit(1);
                }
                break;
            }
            case 's':
            {
                int rc = parse_u64_strict(optarg,&args->buffer_size);
                if (rc != 0)
                {
                    exit(1);
                }
                break;
            }
            case 'h':
            {
                print_help(false);
                exit(0);
                return;
            }
            case 't':
            {
                int rc = parse_u64_strict(optarg, &args->tcp_port);
                if (rc != 0)
                {
                    exit(1);
                }
                break;

            }
            case '?':
            {
                exit(1);
                break;
            }
            default:
            {
                break;
            }
        }
    }
    if (optind < argc)
    {
        // address was given
        char* addr = argv[argc-1];
        args->addr = addr;

    }
}

void print_help(bool to_error)
{
    if (to_error)
    {
        fprintf(stderr, "./build/limen_connect -d <device> -g <gid_index> [-p <port>] [-t <tcp_port>] [-s <bytes>] [<peer>]\n");
    }
    else
    {
        printf("./build/limen_connect -d <device> -g <gid_index> [-p <port>] [-t <tcp_port>] [-s <bytes>] [<peer>]\n");

    }
}

int exchange_as_server(endpoint_identity* remote_identity,
    endpoint_identity* local_identity,
    int tcp_port
)
{

    // variables
    std::string remote_identity_str{};
    int socket_fd;
    sockaddr_in sock_addr{}; 
    int client_fd;

    //  create socket
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        fprintf(stderr,"failed to create socket.\n");
        return 1;
    }

    // set so_reuseaddr
    int opt = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("exchange_as_server:setsockopt");
        return 1;
    }

    //  bind to socket
    sock_addr.sin_family = AF_INET;          
    sock_addr.sin_port = htons(tcp_port);
    sock_addr.sin_addr.s_addr = INADDR_ANY;  

    if (bind(socket_fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr)) < 0) {
        perror("exchange_as_server:bind");
        close(socket_fd);
        return 1;
    }

    //  listen on that socket
    if (listen(socket_fd,1) < 0)
    {
        //  failed to listen on server socket
        perror("exchange_as_server:listen");
        close(socket_fd);
        return 1;
    }

    //  accept a single connection
    client_fd = accept(socket_fd, nullptr, nullptr);

    if (client_fd <0)
    {
        //  failed to accept client
        perror("exchange_as_server:accept");
        close(socket_fd);
        return 1; 
    }

    //  execute send_endpoint_identity
    if (send_endpoint_identity(client_fd, local_identity) != 0)
    {
        fprintf(stderr,"exchange_as_server:send_endpoint_identity");
        return 1;
    }


    //  execute recv_endpoint_identity
    if (recv_endpoint_identity(client_fd,remote_identity) != 0)
    {
        fprintf(stderr,"exchange_as_server:recv_endpoint_identity");
        return 1;
    }

    //  verify valid data in endpoint_identity struct

    //  print out remote identity

    remote_identity_str = "remote: " + identity_to_str(remote_identity);

    std::cout << remote_identity_str << std::endl;





    return 0;
}

int exchange_as_client(endpoint_identity* remote_identity,
    endpoint_identity* local_identity,
    int tcp_port,
    const char* server_addr
)
{
    // variables
    std::string local_identity_str{};
    std::string remote_identity_str{};
    int socket_fd;
    sockaddr_in server_sockaddr{}; 

    //  create socket
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        fprintf(stderr,"failed to create socket.\n");
        return 1;
    }

    // set so_reuseaddr
    int opt = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("exchange_as_server:setsockopt");
        return 1;
    }


    //  construct the sockaddr_in struct that points to the server
    server_sockaddr.sin_family = AF_INET;
    server_sockaddr.sin_port = htons(tcp_port);
    if (inet_pton(AF_INET, server_addr, &server_sockaddr.sin_addr) != 1)
    {
        perror("exchange_as_client:inet_pton");
        return 1;
    }

    //  set non-blocking so connect() can be bounded by a timeout
    int flags = fcntl(socket_fd, F_GETFL, 0);
    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);

    //  connect to the server
    int crc = connect(socket_fd, (struct sockaddr*)&server_sockaddr, sizeof(server_sockaddr));
    if (crc != 0 && errno != EINPROGRESS)
    {
        perror("exchange_as_client:connect");
        return 1;
    }

    if (crc != 0)  //  EINPROGRESS: handshake in flight, wait for it
    {
        struct pollfd pfd{};
        pfd.fd = socket_fd;
        pfd.events = POLLOUT;

        int prc = poll(&pfd, 1, 3000);  //  3s timeout
        if (prc == 0)
        {
            fprintf(stderr, "exchange_as_client:connect timed out\n");
            return 1;
        }
        if (prc < 0)
        {
            perror("exchange_as_client:poll");
            return 1;
        }

        //  poll fired, but that includes connection failure, check SO_ERROR
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0)
        {
            fprintf(stderr, "exchange_as_client:connect failed: %s\n", strerror(so_error));
            return 1;
        }
    }

    //  restore blocking mode so send/recv loops behave normally
    fcntl(socket_fd, F_SETFL, flags);

    //  execute send_endpoint_identity

    if (send_endpoint_identity(socket_fd, local_identity) != 0)
    {
        fprintf(stderr,"exchange_as_server:send_endpoint_identity");
        return 1;
    }


    //  execute recv_endpoint_identity

    if (recv_endpoint_identity(socket_fd,remote_identity) != 0)
    {
        fprintf(stderr,"exchange_as_server:recv_endpoint_identity");
        return 1;
    }

    //  verify valid data in endpoint_identity struct

    //  print out remote identity

    remote_identity_str = "remote: " + identity_to_str(remote_identity);
    std::cout << remote_identity_str << std::endl;



    return 0;
}

int send_endpoint_identity(int fd, endpoint_identity* local_identity)
{
    //  turn local_identity into str
    std::string local_identity_str = identity_to_str(local_identity);
    const char* id_str = local_identity_str.c_str();
    const int id_str_size = local_identity_str.size();
    int str_ptr = 0;

    while (true)
    {
        if (str_ptr >= id_str_size)
        {
            //  done sending
            break;
        }
        int remainder = (id_str_size - str_ptr);
        int send_amount = send(fd,(const void*)(id_str + str_ptr),remainder,MSG_NOSIGNAL);
        if (send_amount == -1)
        {
            //  failed to send
            perror("send_endpoint_identity:send");
            return 1;
        }
        if (send_amount==0)
        {
            //  sent nothing meaning nothing remains
            break;
        }

        //  update pointer based on how much was sent
        str_ptr += send_amount;

    }

    return 0;
}

int recv_endpoint_identity(int fd, endpoint_identity* remote_identity)
{
    std::string remote_identity_str(SIDE_CHANNEL_MSG_SZ, '\0');
    long unsigned int  tot_recv = 0;

    while (tot_recv < SIDE_CHANNEL_MSG_SZ)
    {
        int recv_count = recv(fd, remote_identity_str.data() + tot_recv,
                               SIDE_CHANNEL_MSG_SZ - tot_recv, 0);
        if (recv_count == -1)
        {
            perror("recv_endpoint_identity:recv");
            return 1;
        }
        if (recv_count == 0)
        {
            //  peer closed early, short message
            break;
        }
        tot_recv += recv_count;
    }

    remote_identity_str.resize(tot_recv);  // trim once, after the loop

    if (tot_recv != SIDE_CHANNEL_MSG_SZ)
    {
        fprintf(stderr, "recv_endpoint_identity: short read, got %ld of %ld bytes\n",
                tot_recv, SIDE_CHANNEL_MSG_SZ);
        return 1;
    }

    const char* format_str = "qpn=%x psn=%x gid=%127s lid=%x";
    uint32_t qpn{};
    uint32_t psn{};
    char gid_str[128];
    uint32_t lid32{};

    if (sscanf(remote_identity_str.c_str(), format_str, &qpn, &psn, gid_str, &lid32) != 4)
    {
        fprintf(stderr, "recv_endpoint_identity: sscanf did not parse 4 args from remote msg\n");
        return 1;
    }

    remote_identity->qpn = qpn;
    remote_identity->psn = psn & U32_TO_U24_MASK;
    remote_identity->lid = static_cast<uint16_t>(lid32);

    if (str_to_gid(gid_str, &remote_identity->gid) != 0)
    {
        fprintf(stderr, "recv_endpoint_identity: failed to parse gid\n");
        return 1;
    }

    if (str_to_gid(gid_str, &remote_identity->gid) != 0)
    {
        fprintf(stderr,"recv_endpoint_identity: str_to_gid failed\n");
        return 1;
    }

    return 0;
}

std::string identity_to_str(endpoint_identity* identity) 
{
    return std::format(
        "qpn={:#010x} psn={:#08x} gid={} lid={:#06x}",
            identity->qpn,
            identity->psn,
            gid_to_str(&identity->gid),
            identity->lid
        );
}


int main(int argc, char* argv[])
{
    // Misc. variables
    connect_parsed_args args;
    int rc;
    endpoint_identity local_identity{};
    endpoint_identity remote_identity{};

    //  Device-based variables
    ibv_device** devices_list;
    ibv_context* device_context;
    ibv_device_attr device_attr;
    ibv_pd* pd;
    ibv_gid gid;

    //  Port-based variables
    ibv_port_attr port_attr;

    //  QP-based variables
    ibv_cq* completion_queue;
    ibv_qp* queue_pair;
    ibv_qp_cap qp_cap{};
    ibv_qp_init_attr qp_init_attr{};

    // Endpoint Identity strings
    std::string local_identity_str;
    std::string remote_identity_str;


    // parse args
    parse_argv(argc,argv,&args);

    if (args.addr != nullptr)
    {
        printf("role: client\n");
    } else
    {
        printf("role: server\n");
    }

    // make sure device_name & gid_index are supplied
    if (args.device_name == nullptr)
    {
        fprintf(stderr,"-d required\n");
        print_help(true); 
        exit(1);
    }
    if (args.gid_index == INT_MAX )
    {
        fprintf(stderr,"-g required\n");
        print_help(true); 
        exit(1);
    }

    //  open devices list
    devices_list = ibv_get_device_list(NULL);
    if (devices_list == NULL)
    {
        //  failed to open device list
        perror("ibv_get_device_list");
        exit(1);

    }

    //  figure out index of associated device (-d)
    int device_idx = find_device_by_name(devices_list,args.device_name);
    if (device_idx == -1)
    {
        //  did not find device, exit early
        fprintf(stderr,"did not find device %s\n",args.device_name);
        exit(1);
    }

    //  get device context
    device_context = ibv_open_device(devices_list[device_idx]);
    if (device_context == NULL)
    {
        //  failed to open device context
        perror("ibv_open_device");
        exit(1);
    }

    //  get device attributes
    rc = ibv_query_device(device_context,&device_attr);
    if (rc != 0)
    {
        //  failed to get device attributes
        perror("ibv_query_device");
        exit(1);
    }

    //  get port attributes
    rc = ibv_query_port(device_context,args.port,&port_attr);
    if (rc != 0)
    {
        //  failed to get port attributes
        perror("ibv_query_port");
        exit(1);
    }

    //  verify the GID index against gid_tbl_len
    if (args.gid_index >= port_attr.gid_tbl_len)
    {
        fprintf(stderr, "invalid gid_index: gid_index %i > gid_tbl_len  %i\n",args.gid_index,port_attr.gid_tbl_len);
        exit(1);
    }

    //  create protection domain
    pd = ibv_alloc_pd(device_context);
    if (pd == NULL)
    {
        //  failed to create protection domain
        perror("ibv_alloc_pd");
        exit(1);
    }

    //  create the queues
    //  create completion queue
    completion_queue = ibv_create_cq(
        device_context,
        std::min(COMPLETE_QUEUE_DEPTH,device_attr.max_cqe)
        ,NULL,NULL,0
    );
    if (completion_queue == NULL)
    {
        //  failed to create completion queue
        perror("ibv_create_cq");
        exit(1);
    }
    printf("cq: cqe=%i (requested %i)\n",completion_queue->cqe, COMPLETE_QUEUE_DEPTH);

    //  fill qp_cap for qp_init_attr
    qp_cap.max_send_wr = std::min(SEND_QUEUE_DEPTH,device_attr.max_qp_wr);
    qp_cap.max_recv_wr = std::min(RECV_QUEUE_DEPTH,device_attr.max_qp_wr);
    qp_cap.max_send_sge = 1;
    qp_cap.max_recv_sge = 1;


    //  fill qp_init_attr to make the QP
    qp_init_attr.send_cq = completion_queue;
    qp_init_attr.recv_cq = completion_queue;
    qp_init_attr.srq=NULL;
    qp_init_attr.cap = qp_cap;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all= 1;

    //  get ibv_gid
    if (ibv_query_gid(device_context,args.port,args.gid_index,&gid) == -1)
    {
        //  failed to get ibv_gid
        fprintf(stderr,"ibv_query_gid failed\n");
        exit(EXIT_VERB_ERROR);
    }

    //  create reliable-connected queue pair
    queue_pair = ibv_create_qp(pd,&qp_init_attr);
    if (queue_pair == NULL)
    {
        perror("ibv_create_qp");
        exit(1);
    }

    //  print out qp: line from filled qp_init_attr 
    printf(
        "qp: type=RC max_send_wr=%i max_recv_wr=%i max_send_sge=%i max_recv_sge=%i\n",
        qp_cap.max_send_wr,
        qp_cap.max_recv_wr,
        qp_cap.max_send_sge,
        qp_cap.max_recv_sge
    );


    //  populate local identity struct
    local_identity.qpn = queue_pair->qp_num;
    local_identity.psn = lrand48() & U32_TO_U24_MASK;
    
    local_identity.gid = gid;
    local_identity.lid = port_attr.lid;

    //  print local identity string 
    local_identity_str =  "local: " + identity_to_str(&local_identity);
    std::cout << local_identity_str << std::endl;

    //  perform side-channel exchange (send struct as text not struct data)
    //  perform server or client path
    if (args.addr != nullptr)
    {
        //  client
        if (exchange_as_client(&remote_identity,&local_identity,args.tcp_port,args.addr) != 0)
        {
            fprintf(stderr,"side channel exchange as client failed\n");
            exit(EXIT_SIDE_CHANNEL_ERROR);
        }

    } else {
        //  server
        if (exchange_as_server(&remote_identity, &local_identity, args.tcp_port)!=0)
        {
            fprintf(stderr,"side channel exchange as server failed\n");
            exit(EXIT_SIDE_CHANNEL_ERROR);
        }
    }


    //  perform RESET->INIT transition

    //  perform INIT->RTR transition

    //  perform RTR->RTS transition

    //  Verify QPs are both RTS




    return 0;
}