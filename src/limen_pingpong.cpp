#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include <netinet/in.h>
#include <rdma/rdma_cma.h>
#include <utility>
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
            std::format("get_expected_event fail: unexpected event type {}", event.name()).c_str(),
            EINVAL
        );
    }

    return event;
}

void fill_qp_init_attr(ibv_qp_init_attr* qp_init_attr, ibv_device_attr* device_attr, pingpong_parsed_args* args)
{
    //  fill qp_init_attr.cap
    //  NOTE: CALLER MUST SET qp_init_attr's send_cq AND recv_cq
    qp_init_attr->cap.max_send_wr = std::min((uint32_t)args->iterations,(uint32_t)device_attr->max_qp_wr);
    qp_init_attr->cap.max_recv_wr = std::min(RECV_QUEUE_DEPTH,device_attr->max_qp_wr);
    qp_init_attr->cap.max_send_sge = 1;
    qp_init_attr->cap.max_recv_sge = 1;

    //  fill qp_init_attr to make the QP
    qp_init_attr->srq=NULL;
    qp_init_attr->qp_type = IBV_QPT_RC;
    qp_init_attr->sq_sig_all= 0;
}


int main(int argc, char* argv[])
{
    // Misc. variables
    pingpong_parsed_args args{};
    // parse args
    parse_argv(argc,argv,&args);

    //  Variables:
    int rc = 0;
    int exit_rc=0;
    bool is_client = false;

    //  Device-based variables
    ibv_device_attr device_attr{};
    int mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

    ibv_qp_init_attr qp_init_attr{};

    //  QP transition variables
    ibv_qp_attr qp_attr{};
    int attr_mask=0;


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

    //  create event channel
    limen::EventChannel ec = limen::EventChannel::create();
    //  create connection ID
    limen::ConnectionId conn_id(ec,RDMA_PS_TCP);
    //  perform server or client path CM-managed bringup
    limen::ProtectionDomain pd;
    limen::MemoryRegion recv_mr;
    size_t recv_mr_size = 0;    //  needs to be set once args.rx_depth is clamped; only happens after filling qp_init_attr
    limen::MemoryRegion send_mr;
    size_t send_mr_size = args.message_size; // we're only holding 1 send at a time
    limen::CompletionQueue cq;
    int cqe = 0;    //  needs to be set after querying device

    limen::ConnInfo remote_conninfo{};
    limen::ConnectionId remote_conn_id;

    if (is_client)
    {
        try {   //  client mode
            //  construct the sockaddr_in struct that points to the server
            sockaddr_in remote_sockaddr{};  //  describes the remote address
            remote_sockaddr.sin_family = AF_INET;
            remote_sockaddr.sin_port = htons(args.tcp_port);
            if (inet_pton(AF_INET, args.addr, &remote_sockaddr.sin_addr) != 1)
            {
                perror("exchange_as_client:inet_pton");
                return 1;
            }

            //  resolve addr
            std::cout << std::format("cm: resolving {}:{}\n",args.addr,args.tcp_port);
            limen::Event e;
            rdma_resolve_addr(conn_id.get(), nullptr, (struct sockaddr*) &remote_sockaddr, 5000);
            e = get_expected_event(ec, RDMA_CM_EVENT_ADDR_RESOLVED, 5000);
            std::cout << "cm: event ADDR_RESOLVED" << std::endl;
            
            //  resolve route
            rdma_resolve_route(conn_id.get(), 5000);
            e = get_expected_event(ec, RDMA_CM_EVENT_ROUTE_RESOLVED, 5000);
            std::cout << "cm: event ROUTE_RESOLVED" << std::endl;

            //  get device attributes
            ibv_context* device_context = conn_id.get()->verbs;
            rc = ibv_query_device(device_context,&device_attr);
            if (rc != 0)
            {
                //  failed to get device attributes
                perror("ibv_query_device");
                return EXIT_VERB_ERROR;
            }

            //  fill qp_init_attr
            fill_qp_init_attr(&qp_init_attr,&device_attr, &args);

            //  clamp rx_depth, set recv_mr_size, set cqe
            args.rx_depth = std::min(args.rx_depth,static_cast<uint64_t>(qp_init_attr.cap.max_recv_wr));
            recv_mr_size = args.message_size*args.rx_depth;
            cqe=  std::min(COMPLETE_QUEUE_DEPTH,device_attr.max_cqe);

            //  device has now been decided, create the wrapper instances
            pd = limen::ProtectionDomain(conn_id.get()->verbs);
            recv_mr = limen::MemoryRegion(pd,recv_mr_size,mr_access_flags);
            send_mr = limen::MemoryRegion(pd,send_mr_size,mr_access_flags);
            cq = limen::CompletionQueue(conn_id.get()->verbs,cqe,nullptr,nullptr,0);

            //  set send_cq and recv_cq of qp_init_attr
            qp_init_attr.send_cq = cq.get();
            qp_init_attr.recv_cq = cq.get();

            //  print out qp: line from filled qp_init_attr 
            printf(
                "qp: type=RC max_send_wr=%i max_recv_wr=%i max_send_sge=%i max_recv_sge=%i\n",
                qp_init_attr.cap.max_send_wr,
                qp_init_attr.cap.max_recv_wr,
                qp_init_attr.cap.max_send_sge,
                qp_init_attr.cap.max_recv_sge
            );
            printf("recv: posted=%zu depth=%lu size=%zu\n",recv_mr.get()->length / send_mr_size,args.rx_depth,args.message_size);
            printf("cq: cqe=%i (requested %i)\n",cq.get()->cqe, COMPLETE_QUEUE_DEPTH);
            
            //  create QP
            rdma_create_qp(conn_id.get(), pd.get(), &qp_init_attr);
            std::cout << std::format("cm: qp created qp_num={:#08x}\n",conn_id.qp()->qp_num);
            
            // //  post work requests
            if (!args.no_recv)
            {
                for (uint32_t slot =0; slot < args.rx_depth; slot++)
                {
                    rc = post_recv(slot, (uint64_t)(uintptr_t)recv_mr.get()->addr, conn_id.qp(), args.message_size,  recv_mr.get()->lkey);
                    if (rc !=0)
                    {
                        //  failed to allocate slot
                        fprintf(stderr,"main:post_recv %s (%s)\n",strerrorname_np(rc),strerror(rc));
                        exit_rc = EXIT_VERB_ERROR;
                        return exit_rc;
                    }
                }
            }

            //  connect
            //  fill rdma_conn_param struct
            rdma_conn_param cp{};
            limen::ConnInfo outbound_cinfo = limen::to_wire_format(limen::ConnInfo(
            (uint64_t)(uintptr_t)recv_mr.get()->addr,
                recv_mr.get()->rkey,
                recv_mr.get()->length
            ));

            cp.private_data = (void*)&outbound_cinfo;
            cp.private_data_len = sizeof(outbound_cinfo);
            cp.responder_resources = (uint8_t)std::min<uint32_t>(1, device_attr.max_qp_rd_atom);
            cp.initiator_depth     = (uint8_t)std::min<uint32_t>(1, device_attr.max_qp_init_rd_atom);
            cp.retry_count         = 7;
            cp.rnr_retry_count     = (uint8_t)args.rnr_retry;

            rdma_connect(conn_id.get(), &cp);
            std::cout << "cm: connect finished\n";

            e = get_expected_event(ec, RDMA_CM_EVENT_ESTABLISHED, 5000);
            limen::ConnInfo remote_raw{};
            e.copy_private_data(&remote_raw, sizeof(remote_raw));
            remote_conninfo = limen::from_wire_format(remote_raw);
            std::cout << std::format("cm: connect private_data_len={}\n",sizeof(remote_raw));
            std::cout << "cm: event ESTABLISHED" << std::endl;

            remote_conn_id = std::move(conn_id);
        } 
        catch (limen::VerbsError& e) {
            std::cout << e.what() << std::endl;
            return 7;
        }
    } else {
        try {   //  server mode
            limen::Event e;
            //  bind addr
            sockaddr_in server_sockaddr{};  //  describes the local (server) address
            server_sockaddr.sin_family = AF_INET;
            server_sockaddr.sin_port = htons(args.tcp_port);
            server_sockaddr.sin_addr.s_addr = INADDR_ANY;   //  side effect: conn_id.get()->verbs not set until a CONNECT_REQUEST arrives
            rdma_bind_addr(conn_id.get(), (struct sockaddr*)&server_sockaddr);

            //  listen on addr, wait for a connect request
            rdma_listen(conn_id.get(), 1);
            std::cout << "cm: listening on server..."<<std::endl;
            //  this event has the new id associated with the client
            e = get_expected_event(ec, RDMA_CM_EVENT_CONNECT_REQUEST, -1);
            std::cout << "cm: event CONNECT_REQUEST" << std::endl;

            //  adopt event->id as 2nd identifier
            limen::ConnectionId client_conn_id = limen::ConnectionId::adopt(e.id());

            //  copy out payload
            limen::ConnInfo remote_raw{};
            e.copy_private_data(&remote_raw, sizeof(remote_conninfo));
            remote_conninfo = limen::from_wire_format(remote_raw);

            //  get device attributes
            if (client_conn_id.get()->verbs == nullptr)
            {
                std::cout << "null" << std::endl;
            }
            ibv_context* device_context = client_conn_id.get()->verbs;
            rc = ibv_query_device(device_context,&device_attr);
            if (rc != 0)
            {
                //  failed to get device attributes
                perror("ibv_query_device");
                return EXIT_VERB_ERROR;
            }

            //  fill qp_init_attr
            fill_qp_init_attr(&qp_init_attr,&device_attr, &args);

            //  clamp rx_depth, set recv_mr_size, set cqe
            args.rx_depth = std::min(args.rx_depth,static_cast<uint64_t>(qp_init_attr.cap.max_recv_wr));
            recv_mr_size = args.message_size*args.rx_depth;
            cqe = std::min(COMPLETE_QUEUE_DEPTH,device_attr.max_cqe);

            //  device has now been decided, create the wrapper instances
            pd = limen::ProtectionDomain(client_conn_id.get()->verbs);
            recv_mr = limen::MemoryRegion(pd,recv_mr_size,mr_access_flags);
            send_mr = limen::MemoryRegion(pd,send_mr_size,mr_access_flags);
            cq = limen::CompletionQueue(client_conn_id.get()->verbs,cqe,nullptr,nullptr,0);

            //  set send_cq and recv_cq of qp_init_attr
            qp_init_attr.send_cq = cq.get();
            qp_init_attr.recv_cq = cq.get();

            printf(
                "qp: type=RC max_send_wr=%i max_recv_wr=%i max_send_sge=%i max_recv_sge=%i\n",
                qp_init_attr.cap.max_send_wr,
                qp_init_attr.cap.max_recv_wr,
                qp_init_attr.cap.max_send_sge,
                qp_init_attr.cap.max_recv_sge
            );
            printf("recv: posted=%zu depth=%lu size=%zu\n",recv_mr.get()->length / send_mr_size,args.rx_depth,args.message_size);
            printf("cq: cqe=%i (requested %i)\n",cq.get()->cqe, COMPLETE_QUEUE_DEPTH);

            //  create QP on adopted identifier
            rdma_create_qp(client_conn_id.get(), pd.get(), &qp_init_attr);
            std::cout << std::format("cm: qp created qp_num={:#08x}\n",client_conn_id.qp()->qp_num);

            //  accept w/ own payload
            rdma_conn_param cp{};
            limen::ConnInfo outbound_cinfo = limen::to_wire_format(limen::ConnInfo(
            (uint64_t)(uintptr_t)recv_mr.get()->addr,
                recv_mr.get()->rkey,
                recv_mr.get()->length
            ));

            cp.private_data = (void*)&outbound_cinfo;
            cp.private_data_len = sizeof(outbound_cinfo);
            cp.responder_resources = (uint8_t)std::min<uint32_t>(1, device_attr.max_qp_rd_atom);
            cp.initiator_depth     = (uint8_t)std::min<uint32_t>(1, device_attr.max_qp_init_rd_atom);
            cp.retry_count         = 7;
            cp.rnr_retry_count     = (uint8_t)args.rnr_retry;

            // //  post work requests
            if (!args.no_recv)
            {
                for (uint32_t slot =0; slot < args.rx_depth; slot++)
                {
                    rc = post_recv(slot, (uint64_t)(uintptr_t)recv_mr.get()->addr, client_conn_id.qp(), args.message_size,  recv_mr.get()->lkey);
                    if (rc !=0)
                    {
                        //  failed to allocate slot
                        fprintf(stderr,"main:post_recv %s (%s)\n",strerrorname_np(rc),strerror(rc));
                        exit_rc = EXIT_VERB_ERROR;
                        return exit_rc;
                    }
                }
            }

            rdma_accept(client_conn_id.get(),&cp);
            //  wait for ESTABLISHED
            e = get_expected_event(ec, RDMA_CM_EVENT_ESTABLISHED, -1);
            std::cout << std::format("cm: connect private_data_len={}\n",sizeof(remote_raw));
            std::cout << "cm: event ESTABLISHED" << std::endl;
            remote_conn_id = std::move(client_conn_id);
        }
        catch (limen::VerbsError& e) {
            std::cout << e.what() << std::endl;
            return 7;
        }


    }

    // std::cout << std::format(
    //     "local info: addr={:#x} rkey={:#x} length={:#x}\n",
    //     (uint64_t)(uintptr_t)recv_mr.get()->addr,
    //     recv_mr.get()->rkey,
    //     recv_mr.get()->length
    // );

    std::cout << std::format(
        "peer: addr={:#016x} rkey={:#08x} length={}\n",
        remote_conninfo.addr,
        remote_conninfo.rkey,
        remote_conninfo.length
    );


    //  Verify QP in RTS
    attr_mask = IBV_QP_STATE | IBV_QP_AV;
    rc = ibv_query_qp(remote_conn_id.qp(), &qp_attr, attr_mask, &qp_init_attr);
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

    std::cout << std::format(
        "pingpong: role={} iterations={} size={} signaled={} rnr_retry={}",
        is_client ? "client" : "server",
        args.iterations,
        args.message_size,
        args.unsignaled ? "no" : "yes",
        args.rnr_retry
    ) << std::endl;

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
            void* send_addr = reinterpret_cast<void*>(slot_addr((uint64_t)(uintptr_t)send_mr.get()->addr, 0, args.message_size));
            fill_pattern(send_addr, args.message_size, send_count);
            rc = post_send(!(args.unsignaled),0, (uint64_t)(uintptr_t)send_mr.get()->addr, remote_conn_id.qp(), args.message_size, send_mr.get()->lkey);
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
            int cqe_count = ibv_poll_cq(cq.get(),COMPLETE_QUEUE_DEPTH,wc_arr.data());
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
                        std::cout << std::format("qp_num={:#08x}\n",remote_conn_id.qp()->qp_num);
                        std::cout << "\tnote: opcode and byte_len are not valid on an error completion\n";
                        ibv_query_qp(remote_conn_id.qp(), &qp_attr, IBV_QP_STATE, &qp_init_attr);
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
                            void* recv_addr = reinterpret_cast<void*>(slot_addr((uint64_t)(uintptr_t)recv_mr.get()->addr,slot_num,args.message_size));
                            if (verify_pattern(recv_addr, wc->byte_len, recv_count) > 0)
                            {
                                mismatch_count++;
                            }
                            //  post replacement recv
                            rc = post_recv(slot_num, (uint64_t)(uintptr_t)recv_mr.get()->addr, remote_conn_id.qp(), args.message_size, recv_mr.get()->lkey);
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
                                void* send_addr = reinterpret_cast<void*>(slot_addr((uint64_t)(uintptr_t)send_mr.get()->addr,0,args.message_size));
                                fill_pattern(send_addr, args.message_size, send_count);
                                rc = post_send(!(args.unsignaled), 0, (uint64_t)(uintptr_t) send_mr.get()->addr, remote_conn_id.qp(), args.message_size, send_mr.get()->lkey);
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

    //  send disconnect if client
    if (is_client)
    {
        std::cout << "cm: disconnect requested" << std::endl;
        rdma_disconnect(remote_conn_id.get());
    }
    //  wait for disconnect or timeout_wait
    limen::Event e(ec);
    if (e.type() != RDMA_CM_EVENT_DISCONNECTED && e.type() != RDMA_CM_EVENT_TIMEWAIT_EXIT)
    {
        throw limen::VerbsError("bad disconnect",EINVAL);
    }
    std::cout << "cm: event DISCONNECTED" << std::endl;


    std::printf("teardown: qp=%s cq=%s rx_mr=%s tx_mr=%s pd=%s id=%s channel=%s context=%s\n",
                remote_conn_id.qp()      ? "ok" : "n/a",
                cq.get()      ? "ok" : "n/a",
                recv_mr.get() ? "ok" : "n/a",
                send_mr.get() ? "ok" : "n/a",
                pd.get()          ? "ok" : "n/a",
                remote_conn_id.get() ? "ok" : "n/a",
                ec.get() ? "ok" : "n/a",
                remote_conn_id.get()->verbs ? "ok" : "n/a");
    return 0;

}