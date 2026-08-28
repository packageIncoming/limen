#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include <rdma/rdma_cma.h>
#endif

#include <array>
#include <chrono>
#include <cstdint>
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
#include "limen/limen_pingpong.h"
#include "limen/verbs.hpp"

enum {
    OPT_RNR_RETRY = 256,   //  no short letter given for these three, per TRD-03's interface
    OPT_NO_RECV,
    OPT_UNSIGNALED,
};

void parse_argv(int argc, char* argv[], pingpong_parsed_args* args)
{
    static struct option long_opts[] = {
        {"rnr-retry",  required_argument, nullptr, OPT_RNR_RETRY},
        {"no-recv",    no_argument,       nullptr, OPT_NO_RECV},
        {"unsignaled", no_argument,       nullptr, OPT_UNSIGNALED},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:g:p:t:s:n:r:h", long_opts, nullptr)) != -1)
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
                int rc = parse_u64_strict(optarg,&args->message_size);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 'n':
            {
                int rc = parse_u64_strict(optarg,&args->iterations);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 'r':
            {
                int rc = parse_u64_strict(optarg,&args->rx_depth);
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
            case OPT_RNR_RETRY:
            {
                int rc = parse_int_strict(optarg, &args->rnr_retry);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case OPT_NO_RECV:
            {
                args->no_recv = true;
                break;
            }
            case OPT_UNSIGNALED:
            {
                args->unsignaled = true;
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
    const char* str = "./build/limen_pingpong -d <device> -g <gid_index> [-p <port>] [-t <tcp_port>]\n"
                       "\t[-s <bytes>] [-n <iterations>] [-r <rx_depth>]\n"
                       "\t[--rnr-retry <n>] [--no-recv] [--unsignaled] [<peer>]\n";
    if (to_error)
    {
        fprintf(stderr, "%s", str);
    }
    else
    {
        printf("%s", str);

    }
}

int exchange_as_server(
    int* local_socket_fd,
    int* remote_socket_fd,
    endpoint_identity* remote_identity,
    endpoint_identity* local_identity,
    int tcp_port
)
{

    // variables
    std::string remote_identity_str{};
    sockaddr_in sock_addr{}; 

    //  create socket
    *local_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (*local_socket_fd < 0) {
        fprintf(stderr,"failed to create socket.\n");
        return 1;
    }

    // set so_reuseaddr
    int opt = 1;
    if (setsockopt(*local_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("exchange_as_server:setsockopt");
        // close(*local_socket_fd);
        return 1;
    }

    //  bind to socket
    sock_addr.sin_family = AF_INET;          
    sock_addr.sin_port = htons(tcp_port);
    sock_addr.sin_addr.s_addr = INADDR_ANY;  

    if (bind(*local_socket_fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr)) < 0) {
        perror("exchange_as_server:bind");
        // close(*local_socket_fd);
        return 1;
    }

    //  listen on that socket
    if (listen(*local_socket_fd,1) < 0)
    {
        //  failed to listen on server socket
        perror("exchange_as_server:listen");
        // close(*local_socket_fd);
        return 1;
    }

    //  accept a single connection
    *remote_socket_fd = accept(*local_socket_fd, nullptr, nullptr);

    if (*remote_socket_fd <0)
    {
        //  failed to accept client
        perror("exchange_as_server:accept");
        // close(*local_socket_fd);
        return 1; 
    }

    //  execute send_endpoint_identity
    if (send_endpoint_identity(*remote_socket_fd, local_identity) != 0)
    {
        fprintf(stderr,"exchange_as_server:send_endpoint_identity");
        // close(*local_socket_fd);
        // close(*remote_socket_fd);        
        return 1;
    }


    //  execute recv_endpoint_identity
    if (recv_endpoint_identity(*remote_socket_fd,remote_identity) != 0)
    {
        fprintf(stderr,"exchange_as_server:recv_endpoint_identity");
        // close(*local_socket_fd);
        // close(*remote_socket_fd);        
        return 1;
    }

    //  verify valid data in endpoint_identity struct

    //  print out remote identity

    remote_identity_str = "remote: " + identity_to_str(remote_identity);

    std::cout << remote_identity_str << std::endl;



    // close(*local_socket_fd);
    // close(*remote_socket_fd);

    return 0;
}

int exchange_as_client(
    int* local_socket_fd,
    endpoint_identity* remote_identity,
    endpoint_identity* local_identity,
    int tcp_port,
    const char* server_addr
)
{

    // variables
    std::string local_identity_str{};
    std::string remote_identity_str{};
    sockaddr_in server_sockaddr{}; 

    //  create socket
    *local_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (*local_socket_fd < 0) {
        fprintf(stderr,"failed to create socket.\n");
        return 1;
    }

    // set so_reuseaddr
    int opt = 1;
    if (setsockopt(*local_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("exchange_as_server:setsockopt");
        // close(*local_socket_fd);
        return 1;
    }

    //  construct the sockaddr_in struct that points to the server
    server_sockaddr.sin_family = AF_INET;
    server_sockaddr.sin_port = htons(tcp_port);
    if (inet_pton(AF_INET, server_addr, &server_sockaddr.sin_addr) != 1)
    {
        perror("exchange_as_client:inet_pton");
        // close(*local_socket_fd);
        return 1;
    }

    //  set non-blocking so connect() can be bounded by a timeout
    int flags = fcntl(*local_socket_fd, F_GETFL, 0);
    fcntl(*local_socket_fd, F_SETFL, flags | O_NONBLOCK);

    //  connect to the server
    int crc = connect(*local_socket_fd, (struct sockaddr*)&server_sockaddr, sizeof(server_sockaddr));
    if (crc != 0 && errno != EINPROGRESS)
    {
        perror("exchange_as_client:connect");
        // close(*local_socket_fd);
        return 1;
    }

    if (crc != 0)  //  EINPROGRESS: handshake in flight, wait for it
    {
        struct pollfd pfd{};
        pfd.fd = *local_socket_fd;
        pfd.events = POLLOUT;

        int prc = poll(&pfd, 1, 3000);  //  3s timeout
        if (prc == 0)
        {
            fprintf(stderr, "exchange_as_client:connect timed out\n");
            close(*local_socket_fd);
            return 1;
        }
        if (prc < 0)
        {
            perror("exchange_as_client:poll");
            close(*local_socket_fd);
            return 1;
        }

        //  poll fired, but that includes connection failure, check SO_ERROR
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(*local_socket_fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0)
        {
            fprintf(stderr, "exchange_as_client:connect failed: %s\n", strerror(so_error));
            // close(*local_socket_fd);
            return 1;
        }
    }

    //  restore blocking mode so send/recv loops behave normally
    fcntl(*local_socket_fd, F_SETFL, flags);

    //  execute send_endpoint_identity

    if (send_endpoint_identity(*local_socket_fd, local_identity) != 0)
    {
        fprintf(stderr,"exchange_as_server:send_endpoint_identity");
        // close(*local_socket_fd);
        return 1;
    }


    //  execute recv_endpoint_identity
    if (recv_endpoint_identity(*local_socket_fd,remote_identity) != 0)
    {
        fprintf(stderr,"exchange_as_server:recv_endpoint_identity");
        // close(*local_socket_fd);
        return 1;
    }

    //  verify valid data in endpoint_identity struct

    //  print out remote identity

    remote_identity_str = "remote: " + identity_to_str(remote_identity);
    std::cout << remote_identity_str << std::endl;

    // close(*local_socket_fd);
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

int post_recv(uint32_t slot, uint64_t buff_addr, ibv_qp* queue_pair,  uint32_t message_size, uint32_t lkey)
{
    //  create the ibv_recv_wr
    ibv_recv_wr wr{};
    ibv_sge sge{};
    ibv_recv_wr* bad = nullptr;

    //  for now each entry has a single SGE
    //  slot[i] has address &(buffer) + ([i]* [message_size])
    sge.addr = slot_addr(buff_addr, slot, message_size);
    sge.length = message_size;
    sge.lkey = lkey;

    wr.num_sge = 1;
    wr.sg_list = &sge;
    wr.next = nullptr;
    wr.wr_id = slot | RECV_WRID_TAG;

    int rc = 0;

    rc = ibv_post_recv(queue_pair, &wr,  &bad);

    return rc;
}

int post_send(bool signaled, uint32_t slot, uint64_t buff_addr, ibv_qp* queue_pair,  uint32_t message_size, uint32_t lkey)
{
    //  create the ibv_send_wr
    ibv_send_wr wr{};
    ibv_sge sge{};
    ibv_send_wr* bad = nullptr;

    //  for now each entry has a single SGE
    //  slot[i] has address &(buffer) + ([i]* [message_size])
    sge.addr = slot_addr(buff_addr, slot, message_size);
    sge.length = message_size;
    sge.lkey = lkey;

    wr.num_sge = 1;
    wr.sg_list = &sge;
    wr.next = nullptr;
    wr.wr_id = slot | SEND_WRID_TAG;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags =   IBV_SEND_FENCE ;
    if (signaled) 
    {
        wr.send_flags = wr.send_flags | IBV_SEND_SIGNALED;
    }

    int rc = 0;

    rc = ibv_post_send(queue_pair, &wr, &bad);

    return rc;
}

static const char* wc_status_name(enum ibv_wc_status s)
{
    switch (s) {
    case IBV_WC_SUCCESS:            return "SUCCESS";
    case IBV_WC_LOC_LEN_ERR:        return "LOC_LEN_ERR";
    case IBV_WC_LOC_QP_OP_ERR:      return "LOC_QP_OP_ERR";
    case IBV_WC_LOC_EEC_OP_ERR:     return "LOC_EEC_OP_ERR";
    case IBV_WC_LOC_PROT_ERR:       return "LOC_PROT_ERR";
    case IBV_WC_WR_FLUSH_ERR:       return "WR_FLUSH_ERR";
    case IBV_WC_MW_BIND_ERR:        return "MW_BIND_ERR";
    case IBV_WC_BAD_RESP_ERR:       return "BAD_RESP_ERR";
    case IBV_WC_LOC_ACCESS_ERR:     return "LOC_ACCESS_ERR";
    case IBV_WC_REM_INV_REQ_ERR:    return "REM_INV_REQ_ERR";
    case IBV_WC_REM_ACCESS_ERR:     return "REM_ACCESS_ERR";
    case IBV_WC_REM_OP_ERR:         return "REM_OP_ERR";
    case IBV_WC_RETRY_EXC_ERR:      return "RETRY_EXC_ERR";
    case IBV_WC_RNR_RETRY_EXC_ERR:  return "RNR_RETRY_EXC_ERR";
    case IBV_WC_LOC_RDD_VIOL_ERR:   return "LOC_RDD_VIOL_ERR";
    case IBV_WC_REM_INV_RD_REQ_ERR: return "REM_INV_RD_REQ_ERR";
    case IBV_WC_REM_ABORT_ERR:      return "REM_ABORT_ERR";
    case IBV_WC_INV_EECN_ERR:       return "INV_EECN_ERR";
    case IBV_WC_INV_EEC_STATE_ERR:  return "INV_EEC_STATE_ERR";
    case IBV_WC_FATAL_ERR:          return "FATAL_ERR";
    case IBV_WC_RESP_TIMEOUT_ERR:   return "RESP_TIMEOUT_ERR";
    case IBV_WC_GENERAL_ERR:        return "GENERAL_ERR";
    case IBV_WC_TM_ERR:             return "TM_ERR";
    case IBV_WC_TM_RNDV_INCOMPLETE: return "TM_RNDV_INCOMPLETE";
    }
    return "UNKNOWN";
}

std::string wc_to_str(ibv_wc *wc)
{
    if (wc->status == IBV_WC_SUCCESS)
    {
        //  successful
        if (wc->opcode == IBV_WC_RECV)
        {
            return std::format(
                "completion: wr_id={:#016x} opcode={} status={} byte_len={}",
                wc->wr_id,
                ibv_wc_opcode_str(wc->opcode),
                wc_status_name(wc->status),
                wc->byte_len
            );
        }
        else {
            return std::format(
                "completion: wr_id={:#016x} opcode={} status={}",
                wc->wr_id,
                ibv_wc_opcode_str(wc->opcode),
                wc_status_name(wc->status)
            );
        }
    }
    else 
    {
        //  unsuccessful
        return std::format(
            "completion: wr_id={:#016x} status={} vendor_err={:#08x}", 
            wc->wr_id,
            wc_status_name(wc->status),
            wc->vendor_err
        );
    }

}

static void fill_pattern(void *buf, size_t len, unsigned iter)
{
    unsigned char *p = static_cast<unsigned char*>(buf);
    for (size_t i = 0; i < len; i++)
        p[i] = (unsigned char)((i * 31u + iter * 131u) & 0xff);
}

static size_t verify_pattern(const void *buf, size_t len, unsigned iter)
{
    const unsigned char *p = static_cast<const unsigned char*>(buf);
    for (size_t i = 0; i < len; i++) {
        unsigned char want = (unsigned char)((i * 31u + iter * 131u) & 0xff);
        if (p[i] != want) {
            fprintf(stderr, "mismatch at offset %zu: expected 0x%02x got 0x%02x "
                            "(iteration %u)\n", i, want, p[i], iter);
            return i + 1;                 /* non-zero = mismatch, and where */
        }
    }
    return 0;
}

limen::Event get_expected_event(limen::EventChannel& event_channel, rdma_cm_event_type event_type, int timeout_ms)
{
    //  wait for event to appear
    if (event_channel.wait(timeout_ms) != 0)
    {
        //  throw error
        throw limen::VerbsError("get_expected_event fail: timeout",ETIMEDOUT);
    }
    //  an event should be available now
    limen::Event event(event_channel);
    //  make sure it matches what the caller wanted
    if (event.type() != event_type)
    {
        //  throw error
        throw limen::VerbsError(
            std::format("get_expected_event fail: unexpected event type %s", event.name()).c_str(),
            EINVAL
        );
    }

    return event;
}


int main(int argc, char* argv[])
{
    // Misc. variables
    pingpong_parsed_args args{};
    // parse argsq
    parse_argv(argc,argv,&args);

    //  Variables:
    int rc = 0;
    int exit_rc=0;
    bool is_client = false;
    endpoint_identity local_identity{};
    endpoint_identity remote_identity{};

    //  Device-based variables
    ibv_device_attr device_attr{};
    ibv_gid gid{};
    const int send_buf_size = args.message_size;
    int mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

    //  Port-based variables
    ibv_port_attr port_attr{};


    // ibv_qp* queue_pair = nullptr;
    ibv_qp_init_attr qp_init_attr{};

    //  Endpoint Identity strings
    std::string local_identity_str;
    std::string remote_identity_str;

    //  QP transition variables
    ibv_qp_attr qp_attr{};
    int attr_mask=0;

    //  Side Channel-based variables
    int local_socket_fd = -1;
    int remote_socket_fd = -1;
    char ready;

    if (args.addr != nullptr)
    {
        printf("role: client\n");
        is_client = true;
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

    //  get device context
    limen::Context device_context(args.device_name);

    //  get device attributes
    rc = ibv_query_device(device_context.get(),&device_attr);
    if (rc != 0)
    {
        //  failed to get device attributes
        perror("ibv_query_device");
        exit_rc = EXIT_VERB_ERROR;
        return exit_rc;
        // return exit_rc;
    }

    //  get port attributes
    rc = ibv_query_port(device_context.get(),args.port,&port_attr);
    if (rc != 0)
    {
        //  failed to get port attributes
        perror("ibv_query_port");
        exit_rc = EXIT_VERB_ERROR;
        return exit_rc;
        // return exit_rc;
    }

    //  verify the GID index against gid_tbl_len
    if (args.gid_index >= port_attr.gid_tbl_len)
    {
        fprintf(stderr, "invalid gid_index: gid_index %i > gid_tbl_len  %i\n",args.gid_index,port_attr.gid_tbl_len);
        exit_rc = EXIT_USAGE_ERROR;
        return exit_rc;
        // return exit_rc;
    }

    //  get ibv_gid
    if (ibv_query_gid(device_context.get(),args.port,args.gid_index,&gid) == -1)
    {
        //  failed to get ibv_gid
        fprintf(stderr,"ibv_query_gid failed\n");
        exit_rc = EXIT_VERB_ERROR;
        return exit_rc;
    }

    //  create event channel
    limen::EventChannel ec = limen::EventChannel::create();

    //  create connection ID
    limen::ConnectionId conn_id(ec,RDMA_PS_TCP);

    //  fill qp_init_attr.cap
    //  qp_init_attr's send_cq and recv_cq filled in Endpoint constructor
    qp_init_attr.cap.max_send_wr = std::min((uint32_t)args.iterations,(uint32_t)device_attr.max_qp_wr);
    qp_init_attr.cap.max_recv_wr = std::min(RECV_QUEUE_DEPTH,device_attr.max_qp_wr);
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;


    //  fill qp_init_attr to make the QP
    qp_init_attr.srq=NULL;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all= 0;

    //  clamp rx_depth 
    args.rx_depth = std::min(args.rx_depth,static_cast<uint64_t>(qp_init_attr.cap.max_recv_wr));

    //  Create Endpoint instance which carries everything
    limen::Endpoint endpoint(
        args.device_name,
        std::min(COMPLETE_QUEUE_DEPTH,device_attr.max_cqe),
        nullptr,
        nullptr,
        0,
        &qp_init_attr,
        args.message_size,
        args.rx_depth,
        mr_access_flags
    );

    printf("recv: posted=%zu depth=%lu size=%zu\n",endpoint.get_recv_mr()->length / send_buf_size,args.rx_depth,args.message_size);

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
    if (is_client)
    {
        //  client
        // if (exchange_as_client(&local_socket_fd,&remote_identity,&local_identity,args.tcp_port,args.addr) != 0)
        // {
        //     fprintf(stderr,"side channel exchange as client failed\n");
        //     exit_rc = EXIT_SIDE_CHANNEL_ERROR;
        //     return exit_rc;
        // }
        try {
            //  resolve address
            sockaddr_in server_sockaddr{};
            //  construct the sockaddr_in struct that points to the server
            server_sockaddr.sin_family = AF_INET;
            server_sockaddr.sin_port = htons(args.tcp_port);
            if (inet_pton(AF_INET, args.addr, &server_sockaddr.sin_addr) != 1)
            {
                perror("exchange_as_client:inet_pton");
                // close(*local_socket_fd);
                return 1;
            }
            rdma_resolve_addr(conn_id.get(), nullptr, (struct sockaddr*) &server_sockaddr, 5000);
            limen::Event resolve_addr_event = get_expected_event(ec, RDMA_CM_EVENT_ADDR_RESOLVED, 5000);

            //  resolve route

            //  create QP

            //
        } catch (limen::VerbsError& e) {
            
            return 7;
        }


    } else {
        //  server
        if (exchange_as_server(&local_socket_fd,&remote_socket_fd,&remote_identity, &local_identity, args.tcp_port)!=0)
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
    qp_attr.rnr_retry = args.rnr_retry;
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

    //  post work requests
    if (!args.no_recv)
    {
        for (uint32_t slot =0; slot < args.rx_depth; slot++)
        {
            rc = post_recv(slot, (uint64_t)(uintptr_t)endpoint.get_recv_mr()->addr, endpoint.get_qp(), args.message_size,  endpoint.get_recv_mr()->lkey);
            if (rc !=0)
            {
                //  failed to allocate slot
                fprintf(stderr,"main:post_recv %s (%s)\n",strerrorname_np(rc),strerror(rc));
                exit_rc = EXIT_VERB_ERROR;
                return exit_rc;
            }
        }
    }
    //  send ready signal (single byte 'R')
    ready  ='R';
    {
        int socket;
        if (is_client)
        {
            socket  = local_socket_fd;   
        }
        else
        {
            socket = remote_socket_fd;
        }
        if (send(socket, &ready, 1, MSG_NOSIGNAL) != 1)
        {
            perror("ready_exchange:send");
            return 1;
        }
        if (recv(socket, &ready, 1, 0) != 1)
        {
            perror("ready_exchange:recv");
            return 1;
        }
    }



    //  poll for completion in a loop w/ 10 second timeout
    std::array<ibv_wc, COMPLETE_QUEUE_DEPTH> wc_arr;
    int bad_wc_idx; //  also acts as first error index 
    uint32_t send_count;
    uint32_t send_completions;
    uint32_t recv_count;
    uint32_t mismatch_count;

    {
        send_completions=0;
        bad_wc_idx = -1;
        send_count = 0;
        recv_count = 0;
        mismatch_count = 0;
        //  post the initial send work request only if you're the client
        if (is_client)
        {
            void* send_addr = reinterpret_cast<void*>(slot_addr((uint64_t)(uintptr_t)endpoint.get_send_mr()->addr, 0, args.message_size));
            fill_pattern(send_addr, args.message_size, send_count);
            rc = post_send(!(args.unsignaled),0, (uint64_t)(uintptr_t)endpoint.get_send_mr()->addr, endpoint.get_qp(), args.message_size, endpoint.get_send_mr()->lkey);
            if (rc !=0)
            {
                //  failed to allocate slot
                fprintf(stderr,"main:post_send %s (%s)\n",strerrorname_np(rc),strerror(rc));
                exit_rc = EXIT_VERB_ERROR;
                return exit_rc;
            }
            send_count++;
        }

        std::chrono::time_point last_valid_check = std::chrono::steady_clock::now();
        while (true)
        {
            int cqe_count = ibv_poll_cq(endpoint.get_cq(),COMPLETE_QUEUE_DEPTH,wc_arr.data());
            if (cqe_count < 0)
            {
                //  error
                fprintf(stderr,"main:ibv_poll_cq error\n");
                exit_rc = EXIT_VERB_ERROR;
                return exit_rc;
            }
            if (cqe_count > 0)
            {
                //  completions reported, handle them
                for (int i =0; i < cqe_count; i++)
                {
                    ibv_wc* wc = &wc_arr[i];
                    std::cout << wc_to_str(wc) << std::endl;

                    if (wc_arr[i].status != IBV_WC_SUCCESS)
                    {
                        //  if the status is not successful then WCs from this one onward
                        //  are bad & have to be flushed accordingly
                        std::cout << std::format("qp_num={:#08x}\n",endpoint.get_qp()->qp_num);
                        std::cout << "\tnote: opcode and byte_len are not valid on an error completion\n";
                        ibv_query_qp(endpoint.get_qp(), &qp_attr, IBV_QP_STATE, &qp_init_attr);
                        std::cout << "qp_state_after_error: ERR"  << std::endl;
                        bad_wc_idx = i;
                        break;
                    }
                    else
                    {
                        //  successful, increment counters
                        if (wc->opcode == IBV_WC_SEND)
                        {
                            send_completions++;
                        }
                        else if(wc->opcode == IBV_WC_RECV)
                        {
                            uint32_t slot_num = wc->wr_id & ~(RECV_WRID_TAG);

                            //  verify the payload
                            void* recv_addr = reinterpret_cast<void*>(slot_addr((uint64_t)(uintptr_t)endpoint.get_recv_mr()->addr,slot_num,args.message_size));
                            if (verify_pattern(recv_addr, wc->byte_len, recv_count) > 0)
                            {
                                mismatch_count++;
                            }
                            //  post replacement recv
                            rc = post_recv(slot_num, (uint64_t)(uintptr_t)endpoint.get_recv_mr()->addr, endpoint.get_qp(), args.message_size, endpoint.get_recv_mr()->lkey);
                            if (rc !=0)
                            {
                                //  failed to allocate slot
                                fprintf(stderr,"main:post_recv %s (%s)\n",strerrorname_np(rc),strerror(rc));
                                exit_rc = EXIT_VERB_ERROR;
                                return exit_rc;
                            }
                            recv_count++;

                            //  post reply
                            //  the client will execute (n+1) SENDs since it executed the initial
                            //  SEND before the loop; this prevents sending that n+1th 
                            if ((uint64_t)send_count < args.iterations)
                            {
                                void* send_addr = reinterpret_cast<void*>(slot_addr((uint64_t)(uintptr_t)endpoint.get_send_mr()->addr,0,args.message_size));
                                fill_pattern(send_addr, args.message_size, send_count);
                                rc = post_send(!(args.unsignaled), 0, (uint64_t)(uintptr_t) endpoint.get_send_mr()->addr, endpoint.get_qp(), args.message_size, endpoint.get_send_mr()->lkey);
                                if (rc != 0)
                                {
                                    fprintf(stderr,"main:post_send %s (%s)\n",strerrorname_np(rc),strerror(rc));
                                    exit_rc = EXIT_VERB_ERROR;
                                    return exit_rc;
                                }
                                send_count++;
                            }
                        }
                    }
                }
                //  update last_valid check
                if (bad_wc_idx > -1) break;
                if ((uint64_t)recv_count >= args.iterations
                    && (args.unsignaled || send_completions >= send_count)) break;
                last_valid_check = std::chrono::steady_clock::now();
            }
            //  if its been more than 10sec without a valid (cqe_count>0) check then break as timeout
            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - last_valid_check) > std::chrono::seconds(10))
            {
                fprintf(stderr,"main:ibv_poll_cq loop timeout (10sec)\n");
                exit_rc = EXIT_COMPLETION_STATUS_ERROR;
                return exit_rc;
            }
        }
    }


    std::cout << std::format(
        "result: iterations={} sent={} received={} mismatches={} send_completions={}",
        args.iterations,
        send_count,
        recv_count,
        mismatch_count,
        send_completions
    );
    if (bad_wc_idx > -1)
    {
        std::cout << std::format(" first_error={}",wc_status_name(wc_arr[bad_wc_idx].status));
    }
    std::cout << std::endl;

    std::printf("teardown: qp=%s cq=%s rx_mr=%s tx_mr=%s pd=%s context=%s\n",
                endpoint.get_qp()      ? "ok" : "n/a",
                endpoint.get_cq()      ? "ok" : "n/a",
                endpoint.get_recv_mr() ? "ok" : "n/a",
                endpoint.get_send_mr() ? "ok" : "n/a",
                endpoint.pd()          ? "ok" : "n/a",
                endpoint.get_ctx()     ? "ok" : "n/a");
    return exit_rc;

}