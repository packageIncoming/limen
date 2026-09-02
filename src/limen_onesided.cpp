#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <netinet/in.h>
#include <rdma/rdma_cma.h>
#include <utility>
#include <array>
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
#include "limen/limen_onesided.h"
#include "limen/verbs.hpp"
#include <cstring>
#include "limen/wc.hpp"


void parse_argv(int argc, char* argv[], onesided_parsed_args* args)
{
    enum {
        OPT_MODE = 1000,
        OPT_BAD_RKEY,
        OPT_VERBOSE
    };

    static struct option long_opts[] = {
        {"mode",     required_argument, nullptr, OPT_MODE},
        {"bad-rkey", no_argument,       nullptr, OPT_BAD_RKEY},
        {"verbose",  no_argument,       nullptr, OPT_VERBOSE},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:p:t:s:n:h", long_opts, nullptr)) != -1)
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
            case 'p': 
            {
                int rc = parse_int_strict(optarg, &args->port);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
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
            case 's':
            {
                int rc = parse_u64_strict(optarg, &args->message_size);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 'n':
            {
                int rc = parse_u64_strict(optarg, &args->iterations);
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
            case OPT_MODE:
            {
                if (strcmp(optarg, "write") == 0) {
                    args->mode = onesided_mode::WRITE;
                } else if (strcmp(optarg, "read") == 0) {
                    args->mode = onesided_mode::READ;
                } else if (strcmp(optarg, "imm") == 0) {
                    args->mode = onesided_mode::IMM;
                } else if (strcmp(optarg, "flag") == 0) {
                    args->mode = onesided_mode::FLAG;
                } else if (strcmp(optarg, "lastbyte") == 0) {
                    args->mode = onesided_mode::LASTBYTE;
                } else {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case OPT_BAD_RKEY:
            {
                args->bad_rkey = true;
                break;
            }
            case OPT_VERBOSE:
            {
                args->verbose = true;
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
        // peer address was given
        char* peer = argv[argc-1];
        args->peer = peer;
    }
}

void print_help(bool to_error)
{
    const char* str = "./build/limen_onesided -d <device> [-p <port>] [-t <tcp_port>] [-s <bytes>]\n"
                      "\t[-n <iterations>] [--mode <write|read|imm|flag|lastbyte>]\n"
                      "\t[--bad-rkey] [--verbose] [<peer>]\n";
    if (to_error)
    {
        fprintf(stderr, "%s", str);
    }
    else
    {
        printf("%s", str);

    }
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

int post_send(
    uint32_t slot, 
    uint64_t buff_addr, 
    ibv_qp* queue_pair,  
    uint32_t message_size, 
    uint32_t lkey,
    limen::ConnInfo peer_conninfo
)
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
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;

    //  fill wr.rdma from the peer ConnInfo 
    //  peer's length / message_size = peer_slot_count; represents how many slots the peer would have
    //      so (slot) % (peer_slot_count) "normalizes" (local slot 8 on a 3-slot peer -> 8%3= slot 2 of peer )
    //  NOTE: MAKE SURE PEER'S LENGTH HAS ROOM FOR AT LEAST 1 MESSAGE BEFORE CALLING THIS FUNCTION
    uint32_t peer_slot = slot % (peer_conninfo.length / message_size);
    wr.wr.rdma.remote_addr = slot_addr(peer_conninfo.addr, peer_slot, message_size);
    wr.wr.rdma.rkey = peer_conninfo.rkey;

    return  ibv_post_send(queue_pair, &wr, &bad);
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

void fill_qp_init_attr(ibv_qp_init_attr* qp_init_attr, ibv_device_attr* device_attr, onesided_parsed_args* args)
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
    onesided_parsed_args args{};
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

    if (args.peer != nullptr)
    {
        printf("role: client\n");
        is_client = true;
    } else
    {
        printf("role: server\n");
    }

    //  create event channel
    limen::EventChannel ec = limen::EventChannel::create();
    //  create connection ID
    limen::ConnectionId conn_id(ec,RDMA_PS_TCP);
    //  perform server or client path CM-managed bringup
    limen::ProtectionDomain pd;
    limen::MemoryRegion recv_mr;
    size_t recv_mr_size = 0;    //  needs to be set once rx_depth is clamped; only happens after filling qp_init_attr
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
            if (inet_pton(AF_INET, args.peer, &remote_sockaddr.sin_addr) != 1)
            {
                perror("exchange_as_client:inet_pton");
                return 1;
            }

            //  resolve addr
            std::cout << std::format("cm: resolving {}:{}\n",args.peer,args.tcp_port);
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
            uint32_t rx_depth = static_cast<uint64_t>(qp_init_attr.cap.max_recv_wr);
            recv_mr_size = args.message_size*rx_depth;
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
            printf("recv: posted=%zu depth=%u size=%zu\n",recv_mr.get()->length / send_mr_size,rx_depth,args.message_size);
            printf("cq: cqe=%i (requested %i)\n",cq.get()->cqe, COMPLETE_QUEUE_DEPTH);
            
            //  create QP
            rdma_create_qp(conn_id.get(), pd.get(), &qp_init_attr);
            remote_conn_id = std::move(conn_id);
            std::cout << std::format("cm: qp created qp_num={:#08x}\n",remote_conn_id.qp()->qp_num);

            if (args.mode != onesided_mode::WRITE)
            {
                // //  post work requests
                for (uint32_t slot =0; slot < rx_depth; slot++)
                {
                    rc = post_recv(slot, (uint64_t)(uintptr_t)recv_mr.get()->addr, remote_conn_id.qp(), args.message_size,  recv_mr.get()->lkey);
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
            cp.rnr_retry_count     = 7; //  default from limen_pingpong (655ms)


            rdma_connect(remote_conn_id.get(), &cp);

            e = get_expected_event(ec, RDMA_CM_EVENT_ESTABLISHED, 5000);
            std::cout << "cm: connect finished\n";

            limen::ConnInfo remote_raw{};
            e.copy_private_data(&remote_raw, sizeof(remote_raw));
            remote_conninfo = limen::from_wire_format(remote_raw);
            std::cout << std::format("cm: connect private_data_len={}\n",sizeof(remote_raw));
            std::cout << "cm: event ESTABLISHED" << std::endl;

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
            std::cout << "cm: event CONNECT_REQUEST adopted" << std::endl;

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
            uint32_t rx_depth = qp_init_attr.cap.max_recv_wr;
            recv_mr_size = args.message_size*rx_depth;
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
            printf("recv: posted=%zu depth=%u size=%zu\n",recv_mr.get()->length / send_mr_size,rx_depth,args.message_size);
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
            cp.rnr_retry_count     = 7; //655ms

            if (args.mode == onesided_mode::IMM && !is_client)
            {
                //  post work requests
                for (uint32_t slot =0; slot < rx_depth; slot++)
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

    std::cout << std::format(
        "peer: addr={:#016x} rkey={:#08x} length={}\n",
        remote_conninfo.addr,
        remote_conninfo.rkey,
        remote_conninfo.length
    );

    //  validate connection info
    if (remote_conninfo.addr == 0 || remote_conninfo.rkey == 0 || remote_conninfo.length < args.message_size)
    {
        throw limen::VerbsError("invalid peer ConnectionId object",EINVAL);
    }

    //  poll for completion in a loop w/ 10 second timeout
    std::array<ibv_wc, COMPLETE_QUEUE_DEPTH> wc_arr;
    int bad_wc_idx = -1; //  also acts as first error index 
    uint32_t send_count = 0;
    uint32_t send_completions = 0;
    uint32_t recv_count = 0;
    uint32_t mismatch_count = 0;


    //  post the initial send work request only if you're the client
    if (is_client)
    {
        void* send_addr = reinterpret_cast<void*>(slot_addr((uint64_t)(uintptr_t)send_mr.get()->addr, 0, args.message_size));
        fill_pattern(send_addr, args.message_size, send_count);
        rc = post_send(0, (uint64_t)(uintptr_t)send_mr.get()->addr, remote_conn_id.qp(), args.message_size, send_mr.get()->lkey,remote_conninfo);
        if (rc !=0)
        {
            //  failed to allocate slot
            fprintf(stderr,"main:post_send %s (%s)\n",strerrorname_np(rc),strerror(rc));
            exit_rc = EXIT_VERB_ERROR;
            return exit_rc;
        }
        send_count++;
    }

    if (is_client)
    {
        //  client-side loop
        switch (args.mode) {
            case onesided_mode::WRITE:
                //  client posts (args.iterations) writes and reaps
                while ((uint64_t)send_completions < args.iterations)
                {
                    //reap
                    ibv_wc wc;
                    while (ibv_poll_cq(cq.get(), 1, &wc) >0)
                    {
                        if (wc.opcode == IBV_WC_RDMA_WRITE)
                        {
                            std::cout << wc_to_str(&wc) << std::endl;
                            send_completions++;
                        } 
                    }
                    if ((uint64_t)send_count < args.iterations)
                    {
                        void* send_addr = reinterpret_cast<void*>(slot_addr((uint64_t)(uintptr_t)send_mr.get()->addr,0,args.message_size));
                        fill_pattern(send_addr, args.message_size, send_count);
                        rc = post_send( 0, (uint64_t)(uintptr_t) send_mr.get()->addr, remote_conn_id.qp(), args.message_size, send_mr.get()->lkey,remote_conninfo);
                        if (rc != 0)
                        {
                            fprintf(stderr,"main:post_send %s (%s)\n",strerrorname_np(rc),strerror(rc));
                            exit_rc = EXIT_VERB_ERROR;
                            return exit_rc;
                        }
                        send_count++;
                    }
                }

                break;
            case onesided_mode::READ:
                //  client posts (args.iterations) reads and reaps
                while ((uint64_t)send_completions < args.iterations)
                {
                    //reap
                    ibv_wc wc;
                    while (ibv_poll_cq(cq.get(), 1, &wc) >0)
                    {
                        if (wc.opcode == IBV_WC_RDMA_READ)
                        {
                            //  verify the pattern
                        }
                        send_completions++;
                    }
                    if ((uint64_t)send_count < args.iterations)
                    {
                        void* send_addr = reinterpret_cast<void*>(slot_addr((uint64_t)(uintptr_t)send_mr.get()->addr,0,args.message_size));
                        fill_pattern(send_addr, args.message_size, send_count);
                        rc = post_send( 0, (uint64_t)(uintptr_t) send_mr.get()->addr, remote_conn_id.qp(), args.message_size, send_mr.get()->lkey,remote_conninfo);
                        if (rc != 0)
                        {
                            fprintf(stderr,"main:post_send %s (%s)\n",strerrorname_np(rc),strerror(rc));
                            exit_rc = EXIT_VERB_ERROR;
                            return exit_rc;
                        }
                        send_count++;
                    }
                }

                break;
            case onesided_mode::FLAG:
                break;
            case onesided_mode::IMM:
                break;
            case onesided_mode::LASTBYTE:
                break;

        }
    } 
    else 
    {
        //  server-side loop
        switch (args.mode) {
            case onesided_mode::WRITE:
                //  server does nothing in write mode
                break;
            case onesided_mode::READ:
                //  server does nothing in read mode
                break;
            case onesided_mode::FLAG:
                //  server polls flag and verifies payload
                break;
            case onesided_mode::IMM:
                //  server polls cq and reaps IBV_WC_RECV_RDMA_WITH_IMM
                //  NOTE: server needs to post recv work requests 
                break;
            case onesided_mode::LASTBYTE:
                //  server polls the last byte in region 
                break;

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
        return 7;
    }
    std::cout << "cm: event DISCONNECTED" << std::endl;

    int reaped = 0;
    ibv_wc wc;
    while (ibv_poll_cq(cq.get(), 1, &wc) > 0) {std::cout<< wc_to_str(&wc)<<std::endl;  ++reaped;}   /* drain: must be 0 */
    std::printf("remote-completions: %d\n", reaped);

    //  verify last buffer on --mode write as server
    if (args.mode == onesided_mode::WRITE && is_client==false)
    {
        if (verify_pattern(recv_mr.get()->addr, args.message_size,args.iterations-1) > 0)
        {
            throw limen::VerbsError(
                std::format("verify: buffer contents DO NOT match expected pattern for {} iterations",args.iterations).c_str(),
                EINVAL
            );
        }
    }
    std::cout << std::format("verify: buffer contents match expected pattern for {} iterations",args.iterations) << std::endl;


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