#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include "limen/verbs.hpp"
#endif
#include <cstdlib>
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
#include "limen/verbs.hpp"

void parse_argv(int argc, char* argv[], connect_parsed_args* args)
{
    static struct option long_opts[] = {
        {"force-rtr-fail", no_argument, nullptr, 'F'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:g:p:t:s:h", long_opts, nullptr)) != -1)
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
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 'p': 
            {
                int rc = parse_int_strict(optarg,&args->port);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 's':
            {
                int rc = parse_u64_strict(optarg,&args->buffer_size);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
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
                    exit(EXIT_USAGE_ERROR);
                }
                break;

            }
            case 'F':
            {
                args->force_rtr_fail = true;
                break;
            }
            case '?':
            {
                exit(EXIT_USAGE_ERROR);
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
        close(socket_fd);
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
        close(socket_fd);
        close(client_fd);        
        return 1;
    }


    //  execute recv_endpoint_identity
    if (recv_endpoint_identity(client_fd,remote_identity) != 0)
    {
        fprintf(stderr,"exchange_as_server:recv_endpoint_identity");
        close(socket_fd);
        close(client_fd);        
        return 1;
    }

    //  verify valid data in endpoint_identity struct

    //  print out remote identity

    remote_identity_str = "remote: " + identity_to_str(remote_identity);

    std::cout << remote_identity_str << std::endl;



    close(socket_fd);
    close(client_fd);

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
        close(socket_fd);
        return 1;
    }


    //  construct the sockaddr_in struct that points to the server
    server_sockaddr.sin_family = AF_INET;
    server_sockaddr.sin_port = htons(tcp_port);
    if (inet_pton(AF_INET, server_addr, &server_sockaddr.sin_addr) != 1)
    {
        perror("exchange_as_client:inet_pton");
        close(socket_fd);
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
        close(socket_fd);
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
            close(socket_fd);
            return 1;
        }
        if (prc < 0)
        {
            perror("exchange_as_client:poll");
            close(socket_fd);
            return 1;
        }

        //  poll fired, but that includes connection failure, check SO_ERROR
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0)
        {
            fprintf(stderr, "exchange_as_client:connect failed: %s\n", strerror(so_error));
            close(socket_fd);
            return 1;
        }
    }

    //  restore blocking mode so send/recv loops behave normally
    fcntl(socket_fd, F_SETFL, flags);

    //  execute send_endpoint_identity

    if (send_endpoint_identity(socket_fd, local_identity) != 0)
    {
        fprintf(stderr,"exchange_as_server:send_endpoint_identity");
        close(socket_fd);
        return 1;
    }


    //  execute recv_endpoint_identity

    if (recv_endpoint_identity(socket_fd,remote_identity) != 0)
    {
        fprintf(stderr,"exchange_as_server:recv_endpoint_identity");
        close(socket_fd);
        return 1;
    }

    //  verify valid data in endpoint_identity struct

    //  print out remote identity

    remote_identity_str = "remote: " + identity_to_str(remote_identity);
    std::cout << remote_identity_str << std::endl;

    close(socket_fd);
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

void print_reset_init_fail(int rc, ibv_qp_attr* qp_attr)
{
    fprintf(stderr,"state: RESET -> INIT FAILED: %s (%s)\n", strerrorname_np(rc),std::strerror(rc));
    fprintf(stderr,"attr_mask: IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS\n");
    
    //  print the fields that were changed
    fprintf(stderr,"qp_state: %s\n",qp_state_to_str(qp_attr->qp_state).c_str());
    fprintf(stderr,"pkey_index: %i\n",qp_attr->pkey_index);
    fprintf(stderr,"port_num: %i\n",qp_attr->port_num);
    fprintf(stderr,"qp_access_flags: %i\n",qp_attr->qp_access_flags);


}

void print_init_rtr_fail(int rc, ibv_qp_attr* qp_attr)
{
    fprintf(stderr,"state: INIT -> RTR FAILED: %s (%s)\n", strerrorname_np(rc),std::strerror(rc));
    fprintf(stderr,"attr_mask: IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER\n");
    
    //  print the fields that were changed
    fprintf(stderr, "qp_state: %s\n", qp_state_to_str(qp_attr->qp_state).c_str());
    fprintf(stderr, "path_mtu: %i\n", qp_attr->path_mtu);
    fprintf(stderr, "dest_qp_num: %#010x\n", qp_attr->dest_qp_num);
    fprintf(stderr, "rq_psn: %#08x\n", qp_attr->rq_psn);
    fprintf(stderr, "max_dest_rd_atomic: %i\n", qp_attr->max_dest_rd_atomic);
    fprintf(stderr, "min_rnr_timer: %i\n", qp_attr->min_rnr_timer);
    fprintf(stderr, "ah_attr: is_global=%i dlid=%#06x sl=%i src_path_bits=%i\n",
        qp_attr->ah_attr.is_global, qp_attr->ah_attr.dlid,
        qp_attr->ah_attr.sl, qp_attr->ah_attr.src_path_bits);
    fprintf(stderr, "dgid: %s\n", gid_to_str(&qp_attr->ah_attr.grh.dgid).c_str());
    fprintf(stderr, "sgid_index: %i\n", qp_attr->ah_attr.grh.sgid_index);
    fprintf(stderr, "hint: if dgid is all zero or is_global=0, GRH was never populated\n");

}

void print_rtr_rts_fail(int rc, ibv_qp_attr* qp_attr)
{
    fprintf(stderr, "state: RTR -> RTS FAILED: %s (%s)\n", strerrorname_np(rc), std::strerror(rc));
    fprintf(stderr, "attr_mask: IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC\n");
    fprintf(stderr, "qp_state: %s\n", qp_state_to_str(qp_attr->qp_state).c_str());
    fprintf(stderr, "sq_psn: %#08x\n", qp_attr->sq_psn);
    fprintf(stderr, "timeout: %i\n", qp_attr->timeout);
    fprintf(stderr, "retry_cnt: %i\n", qp_attr->retry_cnt);
    fprintf(stderr, "rnr_retry: %i\n", qp_attr->rnr_retry);
    fprintf(stderr, "max_rd_atomic: %i\n", qp_attr->max_rd_atomic);
}




int main(int argc, char* argv[])
{
    // Misc. variables
    connect_parsed_args args{};
    int rc = 0;
    int exit_rc=0;
    endpoint_identity local_identity{};
    endpoint_identity remote_identity{};

    //  Device-based variables
    limen::Context device_context;
    ibv_device_attr device_attr{};
    ibv_gid gid{};

    //  Port-based variables
    ibv_port_attr port_attr{};

    //  QP-based variables
    ibv_qp_init_attr qp_init_attr{};

    //  Endpoint Identity strings
    std::string local_identity_str;
    std::string remote_identity_str;

    //  QP transition variables
    ibv_qp_attr qp_attr{};
    int attr_mask=0;


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
        exit_rc = EXIT_USAGE_ERROR;
        return exit_rc;
    }
    if (args.gid_index == INT_MAX )
    {
        fprintf(stderr,"-g required\n");
        print_help(true); 
        exit_rc = EXIT_USAGE_ERROR;
        return exit_rc;
    }

    device_context = limen::Context(args.device_name);

    //  get device attributes
    rc = ibv_query_device(device_context.get(),&device_attr);
    if (rc != 0)
    {
        //  failed to get device attributes
        perror("ibv_query_device");
        exit_rc = EXIT_VERB_ERROR;
        return exit_rc;
    }

    //  get port attributes
    rc = ibv_query_port(device_context.get(),args.port,&port_attr);
    if (rc != 0)
    {
        //  failed to get port attributes
        perror("ibv_query_port");
        exit_rc = EXIT_VERB_ERROR;
        return exit_rc;
    }

    //  verify the GID index against gid_tbl_len
    if (args.gid_index >= port_attr.gid_tbl_len)
    {
        fprintf(stderr, "invalid gid_index: gid_index %i > gid_tbl_len  %i\n",args.gid_index,port_attr.gid_tbl_len);
        exit_rc = EXIT_USAGE_ERROR;
        return exit_rc;
    }

    //  get ibv_gid
    if (ibv_query_gid(device_context.get(),args.port,args.gid_index,&gid) == -1)
    {
        //  failed to get ibv_gid
        fprintf(stderr,"ibv_query_gid failed\n");
        exit_rc = EXIT_VERB_ERROR;
        return exit_rc;
    }

    //  fill qp_cap for qp_init_attr
    qp_init_attr.cap.max_send_wr = std::min(SEND_QUEUE_DEPTH,device_attr.max_qp_wr);
    qp_init_attr.cap.max_recv_wr = std::min(RECV_QUEUE_DEPTH,device_attr.max_qp_wr);
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;


    //  fill qp_init_attr to make the QP
    qp_init_attr.srq=NULL;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all= 1;

    limen::Endpoint endpoint(
        args.device_name,
        std::min(COMPLETE_QUEUE_DEPTH,device_attr.max_cqe),
        nullptr,
        nullptr,
        0,
        &qp_init_attr,
        4096,
        8,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ
    );

    printf("cq: cqe=%i (requested %i)\n",endpoint.get_cq()->cqe, COMPLETE_QUEUE_DEPTH);


    //  print out qp: line from filled qp_init_attr 
    printf(
        "qp: type=RC max_send_wr=%i max_recv_wr=%i max_send_sge=%i max_recv_sge=%i\n",
        qp_init_attr.cap.max_send_wr,
        qp_init_attr.cap.max_recv_wr,
        qp_init_attr.cap.max_send_sge,
        qp_init_attr.cap.max_recv_sge
    );


    //  populate local identity struct
    local_identity.qpn = endpoint.get_qp()->qp_num;
    srand48(time(nullptr));
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
            exit_rc = EXIT_SIDE_CHANNEL_ERROR;
            return exit_rc;
        }

    } else {
        //  server
        if (exchange_as_server(&remote_identity, &local_identity, args.tcp_port)!=0)
        {
            fprintf(stderr,"side channel exchange as server failed\n");
            exit_rc = EXIT_SIDE_CHANNEL_ERROR;
            return exit_rc;
        }
    }


    //  perform RESET->INIT transition
    
    //  fill qp_attr
    qp_attr.qp_state        =   IBV_QPS_INIT;
    qp_attr.pkey_index      =   0;
    qp_attr.port_num        =   1;
    qp_attr.qp_access_flags =   IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    //  set flags for attr_mask
    attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    //  execute ibv_modify_qp
    rc = ibv_modify_qp(endpoint.get_qp(), &qp_attr, attr_mask);
    if (rc!=0)
    {
        //  failed to perform RESET->INIT transition
        print_reset_init_fail(rc,&qp_attr);
        exit_rc = EXIT_VERB_ERROR;
        return exit_rc;
    }

    printf("state: RESET -> INIT ok\n");

    //  perform INIT->RTR transition
    //  clear qp_attr & attr_mask
    qp_attr = {};
    attr_mask = 0;

    //  fill qp_attr
    qp_attr.qp_state            =       IBV_QPS_RTR;
    //  NOTE: this is a simplifcation since it assumes the MTU is the same on both ends; 
    //  a hardened implementation would check for the minimum between the two ports
    qp_attr.path_mtu            =       port_attr.active_mtu;
    qp_attr.dest_qp_num         =       remote_identity.qpn;
    qp_attr.rq_psn              =       remote_identity.psn;
    qp_attr.max_dest_rd_atomic  =       1;
    qp_attr.min_rnr_timer       =       12; //  corresponds to 0.64ms
    //  fill ah_attr directly on qp_attr
    //  since we're running RoCEv2 we fill the grh
    qp_attr.ah_attr.dlid        =   0;   //  not used in RoCEv2
    qp_attr.ah_attr.is_global   =   1;
    qp_attr.ah_attr.grh.dgid    =   remote_identity.gid;
    qp_attr.ah_attr.grh.hop_limit   =   2;
    qp_attr.ah_attr.grh.sgid_index  =   args.gid_index;
    qp_attr.ah_attr.port_num    =   args.port;


    //  set flags for attr_mask
    attr_mask = 	IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    if (args.force_rtr_fail)
    {
        qp_attr.ah_attr.is_global   =   0;
    }

    //  execute ibv_modify_qp
    rc = ibv_modify_qp(endpoint.get_qp(), &qp_attr, attr_mask);
    if (rc!=0)
    {
        //  failed to perform RESET->INIT transition
        print_init_rtr_fail(rc,&qp_attr);
        exit_rc = EXIT_VERB_ERROR;
        return exit_rc;
    }

    printf("state: INIT -> RTR ok\n");

    
    //  perform RTR->RTS transition
    //  clear qp_attr & attr_mask
    qp_attr = {};
    attr_mask = 0;

    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = local_identity.psn;
    qp_attr.timeout = 14;
    qp_attr.retry_cnt = 7;
    qp_attr.rnr_retry = 7;
    qp_attr.max_rd_atomic = std::min(1,device_attr.max_qp_rd_atom);

    attr_mask = 	IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_TIMEOUT;

    //  execute ibv_modify_qp
    rc = ibv_modify_qp(endpoint.get_qp(), &qp_attr, attr_mask);
    if (rc!=0)
    {
        //  failed to perform RESET->INIT transition
        print_rtr_rts_fail(rc,&qp_attr);
        exit_rc = EXIT_VERB_ERROR;
        return exit_rc;
    }

    printf("state: RTR -> RTS ok\n");

    //  Verify QP in RTS
    attr_mask = IBV_QP_STATE | IBV_QP_AV;
    rc = ibv_query_qp(endpoint.get_qp(), &qp_attr, attr_mask, &qp_init_attr);
    if (rc !=0)
    {
        //  failed to query
        perror("main:ibv_query_qp");
        exit_rc = EXIT_VERB_ERROR;
        return exit_rc;
    }

    if (qp_attr.qp_state != IBV_QPS_RTS)
    {
        fprintf(stderr,"local qp not in state IBV_QPS_RTS after RTR -> RTS transition\n");
        exit_rc = EXIT_VERB_ERROR;
        return exit_rc;
    }

    printf("verify: qp_state=RTS\n");

    const char *qp_s = "ok", *cq_s = "ok", *mr_s = "ok",
               *pd_s = "ok", *ctx_s = "ok";

    printf("teardown: qp=%s cq=%s mr=%s pd=%s context=%s\n",
           qp_s, cq_s, mr_s, pd_s, ctx_s);
    return exit_rc;


}