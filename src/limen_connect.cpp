#include <cstdio>
#include <stdlib.h>
#include <unistd.h>
#include <iostream>
#include <algorithm> 
#include <format>

#include <infiniband/verbs.h>
#include "limen/limen_common.h"
#include "limen/limen_connect.h"

void parse_argv(int argc, char* argv[],connect_parsed_args* args)
{
    int opt;
    while ((opt = getopt(argc, argv, "d:g:p:t:s:")) != -1)
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
                printf("./build/limen_devinfo [-d <device>] [-p <port>] [-s <bytes>] [--check-access] [-h]\n");
                exit(0);
                return;
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

// int exchange_as_server(int tcp_port){
//     //  create socket
//     int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
//     if (serverSocket < 0) {
//         fprintf(stderr,"failed to create socket.\n");
//         return 1;
//     }

//     //  bind to socket
//     sockaddr_in sock_addr{}; 
//     sock_addr.sin_family = AF_INET;          
//     sock_addr.sin_port = htons(tcp_port);
//     sock_addr.sin_addr.s_addr = INADDR_ANY;  

//     if (bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
//         fprintf(stderr,"Bind failed. Port might already be in use.\n");
//         close(serverSocket);
//         return 1;
//     }

//     //  perform write to fd

//     //  perform read from fd

//     //  parse output into endpoint_identity struct

//     //  verify valid data in endpoint_identity struct

//     return 0;
// }

// int exchange_as_client()
// {
//     //
//     return 0;
// }

int main(int argc, char* argv[])
{
    // Misc. variables
    connect_parsed_args args;
    int rc;
    endpoint_identity local_identity{};
    // endpoint_identity remote_identity{};

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
    local_identity_str = std::format(
        "local: qpn={:#x} psn={:#x} gid={} lid={:#x}",
        local_identity.qpn,
        local_identity.psn,
        gid_to_str(&local_identity.gid),
        local_identity.lid
    );
    std::cout << local_identity_str << std::endl;

    //  perform side-channel exchange (send struct as text not struct data)
    //  perform server or client path
    // if (args.addr != nullptr)
    // {
    //     //  client
    //     if (exchange_as_client() != 0)
    //     {
    //         fprintf(stderr,"side channel exchange as client failed\n")
    //         exit(EXIT_SIDE_CHANNEL_ERROR);
    //     }

    // } else {
    //     //  server
    //     if (exchange_as_server() != 0)
    //     {
    //         fprintf(stderr,"side channel exchange as server failed\n")
    //         exit(EXIT_SIDE_CHANNEL_ERROR);
    //     }
    // }


    //  perform RESET->INIT transition

    //  perform INIT->RTR transition

    //  perform RTR->RTS transition

    //  Verify QPs are both RTS




    return 0;
}