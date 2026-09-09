
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <chrono>
#include <ostream>
#include "limen/app/cli.hpp"
#include "limen/app/exit_codes.hpp"
#include "limen/cm.hpp"
#include "limen/format.hpp"
#include "limen/pattern.hpp"
#include "limen/session.hpp"
#include <netinet/in.h>
#include <rdma/rdma_cma.h>
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
#include "limen/app/onesided.hpp"
#include "limen/verbs.hpp"
#include <cstring>


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
                int rc = limen::app::parse_int_strict(optarg, &args->port);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 't':
            {
                int rc = limen::app::parse_u64_strict(optarg, &args->tcp_port);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 's':
            {
                int rc = limen::app::parse_u64_strict(optarg, &args->message_size);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 'n':
            {
                int rc = limen::app::parse_u64_strict(optarg, &args->iterations);
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
    sge.addr = limen::slot_addr(buff_addr, slot, message_size);
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

//  primitive: one-sided RDMA at an explicit remote byte offset.
//  opcode must be IBV_WR_RDMA_WRITE or IBV_WR_RDMA_READ.
//  caller owns the wr_id, the local address, and the length; nothing is derived.
int post_rdma_at(
    ibv_wr_opcode    opcode,
    uint64_t         local_addr,
    uint32_t         len,
    ibv_qp*          queue_pair,
    uint32_t         lkey,
    limen::ConnInfo  peer_conninfo,
    uint64_t         remote_offset,
    uint64_t         wr_id,
    uint32_t imm_data          // network byte order; ignored unless opcode is WRITE_WITH_IMM
)
{
    ibv_send_wr  wr{};
    ibv_sge      sge{};
    ibv_send_wr* bad = nullptr;

    //  for now each entry has a single SGE
    //  WRITE: local is the source. READ: local is the destination.
    sge.addr   = local_addr;
    sge.length = len;
    sge.lkey   = lkey;

    wr.num_sge    = 1;
    wr.sg_list    = &sge;
    wr.next       = nullptr;
    wr.wr_id      = wr_id;
    wr.opcode     = opcode;
    wr.send_flags = IBV_SEND_SIGNALED;

    //  remote_offset is a byte offset into the peer's exposed region.
    //  caller is responsible for keeping [offset, offset+len) inside peer_conninfo.length.
    wr.wr.rdma.remote_addr = peer_conninfo.addr + remote_offset;
    wr.wr.rdma.rkey        = peer_conninfo.rkey;

    //  fill immediate data for WRITE_WITH_IMM calls
    if (imm_data)
        wr.imm_data = imm_data;


    return ibv_post_send(queue_pair, &wr, &bad);
}

int post_write_at(
    uint64_t         local_addr,
    uint32_t         len,
    ibv_qp*          queue_pair,
    uint32_t         lkey,
    limen::ConnInfo  peer_conninfo,
    uint64_t         remote_offset,
    uint64_t         wr_id
)
{
    return post_rdma_at(IBV_WR_RDMA_WRITE, local_addr, len, queue_pair,
                        lkey, peer_conninfo, remote_offset, wr_id,0);
}
int post_send_imm(
    uint32_t local_slot,
    uint32_t peer_slot,
    uint64_t buff_addr,
    ibv_qp* queue_pair,
    uint32_t message_size,
    uint32_t lkey,
    limen::ConnInfo peer_conninfo,
    uint32_t imm_data
)
{
    return post_rdma_at(
        IBV_WR_RDMA_WRITE_WITH_IMM,
        limen::slot_addr(buff_addr, local_slot, message_size),
        message_size,
        queue_pair,
        lkey,
        peer_conninfo,
        (uint64_t)peer_slot * message_size,
        local_slot | SEND_WRID_TAG,
        imm_data
    );
}
int post_read_at(
    uint64_t         local_addr,
    uint32_t         len,
    ibv_qp*          queue_pair,
    uint32_t         lkey,
    limen::ConnInfo  peer_conninfo,
    uint64_t         remote_offset,
    uint64_t         wr_id
)
{
    return post_rdma_at(IBV_WR_RDMA_READ, local_addr, len, queue_pair,
                        lkey, peer_conninfo, remote_offset, wr_id,0);
}

//  slot-based wrappers. peer_slot is explicit: local and peer slot counts
//  are independent, and only the caller knows the mapping it wants.
int post_send(
    uint32_t         local_slot,
    uint32_t         peer_slot,
    uint64_t         buff_addr,
    ibv_qp*          queue_pair,
    uint32_t         message_size,
    uint32_t         lkey,
    limen::ConnInfo  peer_conninfo
)
{
    return post_write_at(
        limen::slot_addr(buff_addr, local_slot, message_size),
        message_size,
        queue_pair,
        lkey,
        peer_conninfo,
        (uint64_t)peer_slot * message_size,
        local_slot | SEND_WRID_TAG
    );
}

int post_read(
    uint32_t         local_slot,
    uint32_t         peer_slot,
    uint64_t         buff_addr,
    ibv_qp*          queue_pair,
    uint32_t         message_size,
    uint32_t         lkey,
    limen::ConnInfo  peer_conninfo
)
{
    return post_read_at(
        limen::slot_addr(buff_addr, local_slot, message_size),
        message_size,
        queue_pair,
        lkey,
        peer_conninfo,
        (uint64_t)peer_slot * message_size,
        local_slot | SEND_WRID_TAG
    );
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
    try 
    {
    // Misc. variables
    onesided_parsed_args args{};
    // parse args
    parse_argv(argc,argv,&args);

    //  Variables:
    int rc = 0;
    int exit_rc=0;
    bool is_client = false;


    if (args.peer != nullptr)
    {
        printf("role: client\n");
        is_client = true;
    } else
    {
        printf("role: server\n");
    }


    limen::SessionConfig cfg{};
    cfg.recv_wr = (!is_client && args.mode == onesided_mode::IMM) ? RECV_QUEUE_DEPTH : 0;
    if (!is_client && args.mode == onesided_mode::IMM)
        cfg.recv_slots = cfg.recv_wr;
    if (!is_client && args.mode == onesided_mode::FLAG)
        cfg.recv_slots = 2;    //  1= default amount used for recv, 2-> slot 0 is reserved for the flag 
    
    cfg.recv_slot_size = args.message_size;
    cfg.send_wr = args.iterations * (args.mode == onesided_mode::FLAG ? 2 : 1);
    cfg.send_slots     = SEND_QUEUE_DEPTH/2;
    if (is_client && args.mode == onesided_mode::FLAG)
        cfg.send_slots++;   //  the first slot is reserved as the flag payload slot 
    cfg.send_slot_size = args.message_size;


    limen::Session session;

    if (is_client)
    {
        session = limen::Session::create_client_session( args.peer , cfg);
    }
    else
    {
        //  certain modes require some additional work done on the server before it can connect,
        //  so we set up a PendingConnection, do that stuff, then finish that connection
        limen::PendingConnection pending_connection = limen::PendingConnection::listen(cfg);
        //  READ mode on server needs to fill the buffer with an expected value
        limen::fill_pattern(pending_connection.recv_mr()->addr, pending_connection.recv_mr()->length, 0);
        session = std::move(pending_connection).finish();
    }

    std::cout << std::format("max_outstanding_reads={}\n", session.negotiated_initiator_depth());  

    limen::ConnInfo peer_info = session.peer();

    //  validate connection info
    if (peer_info.addr == 0 || peer_info.rkey == 0 || peer_info.length < args.message_size)
    {
        throw limen::VerbsError("invalid peer ConnectionId object",EINVAL);
    }

    if (args.bad_rkey)
    {
        //  corrupt before posting
        peer_info.rkey = 0;
    }

    std::cout << std::format(
        "peer: addr={:#016x} rkey={:#08x} length={}\n",
        peer_info.addr,
        peer_info.rkey,
        peer_info.length
    );


    uint32_t peer_slots = peer_info.length / args.message_size;
    uint32_t send_slots = session.send_mr()->length / args.message_size;
    uint32_t recv_slots = session.recv_mr()->length / args.message_size;


    std::array<ibv_wc, COMPLETE_QUEUE_DEPTH> wc_arr{};
    int bad_wc_idx = -1; //  also acts as first error index 
    uint32_t send_count = 0;
    uint32_t send_completions = 0;
    uint32_t recv_count = 0;
    uint32_t mismatch_count = 0;
    ibv_qp_attr qp_attr;    //  used for querying the QP when something goes wrong (WC status!= SUCCESS)
    ibv_qp_init_attr init_attr; //  used for querying QP when something goes wrong (WC status != SUCCESS)
    int reaped = 0;


    if (is_client)
    {
        //  client-side loop
        switch (args.mode) {
            case onesided_mode::WRITE:
            case onesided_mode::LASTBYTE:
                //  client posts (args.iterations) writes and reaps
                while ((uint64_t)send_completions < args.iterations && bad_wc_idx==-1)
                {
                    //reap
                    ibv_wc wc;
                    while (ibv_poll_cq(session.cq(), 1, &wc) >0)
                    {
                        if (wc.status != IBV_WC_SUCCESS)
                        {
                            //  if the status is not successful then WCs from this one onward
                            //  are bad & have to be flushed accordingly
                            std::cout << std::format("qp_num={:#08x}\n",session.qp()->qp_num);
                            std::cout << "\tnote: opcode and byte_len are not valid on an error completion\n";
                            std::cout << std::format("completion: wr_id={:#018x} status={} vendor_err={:#08x}\n",
                                                    wc.wr_id, limen::wc_status_name(wc.status), wc.vendor_err);
                            std::cout << "gates: MR access flags and QP qp_access_flags must both permit it\n";
                            ibv_query_qp(session.qp(), &qp_attr, IBV_QP_STATE, &init_attr);
                            std::cout << "qp_state_after_error: "  << limen::qp_state_to_str(session.qp()->state) <<  std::endl;
                            bad_wc_idx = 0;
                            break;
                        }
                        if (wc.opcode == IBV_WC_RDMA_WRITE)
                        {
                            std::cout << limen::wc_to_str(&wc) << std::endl;
                            send_completions++;
                        } 
                    }
                    if ((uint64_t)send_count < args.iterations)
                    {
                        int slot_num = send_count % send_slots;
                        void* send_addr = reinterpret_cast<void*>(limen::slot_addr((uint64_t)(uintptr_t)session.send_mr()->addr,slot_num,args.message_size));
                        limen::fill_pattern(send_addr, args.message_size, send_count);
                        rc = post_send(slot_num, slot_num % peer_slots,
                                    (uint64_t)(uintptr_t)session.send_mr()->addr,
                                    session.qp(), args.message_size,
                                    session.send_mr()->lkey, peer_info);
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
                while ((uint64_t)send_completions < args.iterations && bad_wc_idx==-1)
                {
                    //reap
                    ibv_wc wc;
                    while (ibv_poll_cq(session.cq(), 1, &wc) >0)
                    {
                        if (wc.status != IBV_WC_SUCCESS)
                        {
                            //  if the status is not successful then WCs from this one onward
                            //  are bad & have to be flushed accordingly
                            std::cout << std::format("qp_num={:#08x}\n",session.qp()->qp_num);
                            std::cout << std::format("completion: wr_id={:#018x} status={} vendor_err={:#08x}\n",
                                                    wc.wr_id, limen::wc_status_name(wc.status), wc.vendor_err);
                            std::cout << "gates: MR access flags and QP qp_access_flags must both permit it\n";
                            std::cout << "\tnote: opcode and byte_len are not valid on an error completion\n";
                            ibv_query_qp(session.qp(), &qp_attr, IBV_QP_STATE, &init_attr);
                            std::cout << "qp_state_after_error: "  << limen::qp_state_to_str(session.qp()->state) <<  std::endl;
                            bad_wc_idx = 0;
                            break;
                        }
                        if (wc.opcode == IBV_WC_RDMA_READ)
                        {
                            //  verify the pattern
                            int slot_num = limen::Session::remove_tags(wc.wr_id);
                            void* read_dest_addr = reinterpret_cast<void*>(limen::slot_addr((uint64_t)(uintptr_t)session.send_mr()->addr,slot_num,args.message_size));
                            std::cout << limen::wc_to_str(&wc) << std::endl;
                            if(limen::verify_pattern(read_dest_addr, args.message_size, 0) >0) mismatch_count++;
                            send_completions++;
                        }
                    }
                    if ((uint64_t)send_count < args.iterations)
                    {
                        uint32_t local_slot = send_count % send_slots;
                        rc = post_read(local_slot, local_slot % peer_slots,
                                    (uint64_t)(uintptr_t)session.send_mr()->addr,
                                    session.qp(), args.message_size,
                                    session.send_mr()->lkey, peer_info);
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
                //  client posts a write, then a second write to a location reserved for a flag (flag read by server to signal completion)
                while ((uint64_t)send_completions < args.iterations && bad_wc_idx==-1)
                {
                    //reap
                    ibv_wc wc;
                    while (ibv_poll_cq(session.cq(), 1, &wc) >0)
                    {
                        if (wc.status != IBV_WC_SUCCESS)
                        {
                            //  if the status is not successful then WCs from this one onward
                            //  are bad & have to be flushed accordingly
                            wc_arr[0]  = wc;          //  keep the failing completion; first_error reads it
                            bad_wc_idx = 0;

                            std::cout << std::format("qp_num={:#08x}\n", session.qp()->qp_num);
                            std::cout << "\tnote: opcode and byte_len are not valid on an error completion\n";
                            std::cout << std::format("completion: wr_id={:#018x} status={} vendor_err={:#08x}\n",
                                                    wc.wr_id, limen::wc_status_name(wc.status), wc.vendor_err);
                            std::cout << std::format("rkey=0x{:06x} (bad_rkey={})\n",
                                                    peer_info.rkey, args.bad_rkey ? "yes" : "no");
                            std::cout << "gates: MR access flags and QP qp_access_flags must both permit it\n";

                            ibv_query_qp(session.qp(), &qp_attr, IBV_QP_STATE, &init_attr);
                            std::cout << "qp_state_after_error: "
                                    << (qp_attr.qp_state == IBV_QPS_ERR ? "ERR"
                                                                        : limen::qp_state_to_str(qp_attr.qp_state))
                                    << std::endl;
                            break;
                        }
                        if (wc.opcode == IBV_WC_RDMA_WRITE)
                        {
                            std::cout << limen::wc_to_str(&wc) << std::endl;
                            if (!(wc.wr_id & FLAG_WRID_TAG))
                                send_completions++;
                        } 
                    }
                    if ((uint64_t)send_count < args.iterations)
                    {
                        uint32_t slot_num = 1+ (send_count % (send_slots-1));
                        uint32_t peer_slot = 1+(slot_num % (peer_slots - 1));
                        void* send_addr = reinterpret_cast<void*>(limen::slot_addr((uint64_t)(uintptr_t)session.send_mr()->addr,slot_num,args.message_size));
                        limen::fill_pattern(send_addr, args.message_size, send_count);

                        rc = post_send(slot_num, peer_slot,
                                    (uint64_t)(uintptr_t)session.send_mr()->addr,
                                    session.qp(), args.message_size,
                                    session.send_mr()->lkey, peer_info);
                        if (rc != 0)
                        {
                            fprintf(stderr,"main:post_send %s (%s)\n",strerrorname_np(rc),strerror(rc));
                            exit_rc = EXIT_VERB_ERROR;
                            return exit_rc;
                        }

                        //  now write to the flag
                        uint64_t* flag_src = reinterpret_cast<uint64_t*>(session.send_mr()->addr);
                        *flag_src = send_count+1;

                        rc = post_write_at((uint64_t)(uintptr_t)flag_src, sizeof(uint64_t),
                                        session.qp(), session.send_mr()->lkey,
                                        peer_info, 0, FLAG_WRID_TAG | (send_count));
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
            case onesided_mode::IMM:
                //  client posts a write with IMM data which triggers a recv wr on the server
                while ((uint64_t)send_completions < args.iterations && bad_wc_idx==-1)
                {
                    //reap
                    ibv_wc wc;
                    while (ibv_poll_cq(session.cq(), 1, &wc) >0)
                    {
                        // if (wc.opcode == IBV_WC_RDMA_WRITE)
                        // {
                            std::cout << limen::wc_to_str(&wc) << std::endl;
                            send_completions++;
                        // } 
                    }
                    if ((uint64_t)send_count < args.iterations)
                    {
                        int slot_num = send_count % send_slots;
                        void* send_addr = reinterpret_cast<void*>(limen::slot_addr((uint64_t)(uintptr_t)session.send_mr()->addr,slot_num,args.message_size));
                        limen::fill_pattern(send_addr, args.message_size, send_count);
                        rc = post_send_imm(slot_num, slot_num % peer_slots,
                                    (uint64_t)(uintptr_t)session.send_mr()->addr,
                                    session.qp(), args.message_size,
                                    session.send_mr()->lkey, peer_info,
                                    htonl(send_count));
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
            {
                //  flag is always slot 0 of the recv buffer, specifically the first 8 bytes
                volatile uint64_t* flag_addr = reinterpret_cast<volatile uint64_t*>(session.recv_mr()->addr);
                auto last_progress = std::chrono::steady_clock::now();
                uint64_t seen = *flag_addr;
                uint32_t spins = 0;

                while (*flag_addr < args.iterations)
                {
                    if (*flag_addr != seen)
                    {
                        seen = *flag_addr;
                        last_progress = std::chrono::steady_clock::now();
                    }
                    //  the clock call is far more expensive than the load it
                    //  guards, and polling it every pass widens the sampling
                    //  window this mode is meant to demonstrate
                    if (++spins >= 4096)
                    {
                        spins = 0;
                        if (std::chrono::steady_clock::now() - last_progress
                            > std::chrono::seconds(5))
                        {
                            std::cout << std::format(
                                "flag: stalled at {} of {} after 5s\n",
                                seen, args.iterations);
                            break;
                        }
                    }
                }
                recv_count = seen;
                //  NOTE: Having the client wait for the server to acknowledge a received write would just
                //  turn this into two-sided RC, so instead we just wait here.
                break;
            }
            case onesided_mode::IMM:
                //  server polls cq and reaps IBV_WC_RECV_RDMA_WITH_IMM
                //  NOTE: server needs to post recv work requests 
                while (recv_count < args.iterations)
                {
                    ibv_wc wc;
                    while (ibv_poll_cq(session.cq(), 1, &wc) >0)
                    {
                        if (wc.status != IBV_WC_SUCCESS)
                        {
                            std::cout << std::format("qp_num={:#08x}\n",session.qp()->qp_num);
                            std::cout << "\tnote: opcode and byte_len are not valid on an error completion\n";
                            ibv_query_qp(session.qp(), &qp_attr, IBV_QP_STATE, &init_attr);
                            std::cout << "qp_state_after_error: "  << limen::qp_state_to_str(session.qp()->state) <<  std::endl;
                            bad_wc_idx = 0;
                            break;
                        }
                        if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM)
                        {
                            uint32_t peer_send_counter = ntohl(wc.imm_data);
                            uint32_t slot_num = peer_send_counter % recv_slots;
                            //  we cannot perform verify_pattern here since the region can be written to while we check it
                            //  also, technically we could do repost_recv(0) every time & it makes no difference since
                            //  the address comes from the client not the local machine
                            std::cout << limen::wc_to_str(&wc) << std::endl;
                            session.repost_recv(slot_num);
                            recv_count++;
                            reaped++;
                        }
                    }
                }
                break;
            case onesided_mode::LASTBYTE:
                //  server polls the last byte in region 
                volatile uint8_t* lastbyte_addr = reinterpret_cast<volatile uint8_t*>((uintptr_t)session.recv_mr()->addr + session.recv_mr()->length-1);
                uint8_t last_val = *lastbyte_addr;
                auto last_valid_check = std::chrono::steady_clock::now();
                while (true) {
                    if (last_val != *lastbyte_addr)
                    {
                        recv_count++;
                        last_val = *lastbyte_addr;
                        if (limen::verify_pattern(session.recv_mr()->addr, args.message_size, recv_count - 1) > 0)
                            mismatch_count++;
                    }
                    if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - last_valid_check) > std::chrono::seconds(2))
                        break;  //  timeout after 2sec of no recvs
                }
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
        std::cout << std::format(" first_error={}",limen::wc_status_name(wc_arr[bad_wc_idx].status));
    }
    std::cout << std::endl;

    //  send disconnect if client
    if (is_client)
    {
        std::cout << "cm: disconnect requested" << std::endl;
        session.disconnect();
    }
    //  wait for disconnect or timeout_wait
    session.wait_for_disconnect(10000);
    std::cout << "cm: event DISCONNECTED" << std::endl;

    ibv_wc wc;
    while (ibv_poll_cq(session.cq(), 1, &wc) > 0) {std::cout<< limen::wc_to_str(&wc)<<std::endl;  ++reaped;}   /* drain: must be 0 */
    std::printf("remote-completions: %d\n", reaped);

    //  verify last buffer on --mode write as server
    if (args.mode == onesided_mode::WRITE && is_client == false)
    {
        if (limen::verify_pattern(session.recv_mr()->addr, args.message_size, args.iterations - 1) > 0)
        {
            std::cout << std::format(
                "verify: buffer contents DO NOT match expected pattern for {} iterations",
                args.iterations) << std::endl;
            mismatch_count++;
        }
        else
        {
            std::cout << std::format(
                "verify: buffer contents match expected pattern for {} iterations",
                args.iterations) << std::endl;
        }
    }


    std::printf("teardown: qp=%s cq=%s rx_mr=%s tx_mr=%s pd=%s id=%s channel=%s context=%s\n",
                session.qp()      ? "ok" : "n/a",
                session.cq()      ? "ok" : "n/a",
                session.recv_mr() ? "ok" : "n/a",
                session.send_mr() ? "ok" : "n/a",
                session.pd()          ? "ok" : "n/a",
                session.id() ? "ok" : "n/a",
                session.ec() ? "ok" : "n/a",
                session.id()->verbs ? "ok" : "n/a");
    //  a failed completion is a completion-status error, not a clean run
    if (bad_wc_idx > -1)      return EXIT_COMPLETION_STATUS_ERROR;
    if (mismatch_count > 0)   return EXIT_PAYLOAD_VERIFICATION_ERROR;
    return EXIT_SUCCESS;
    }
    catch (const limen::SessionError& e) { fprintf(stderr, "%s\n", e.what()); return EXIT_CONNECTION_MANAGER_FAILURE; }
    catch (const limen::VerbsError& e)   { fprintf(stderr, "%s\n", e.what()); return EXIT_VERB_ERROR; }
    catch (const std::exception& e)      { fprintf(stderr, "unhandled: %s\n", e.what()); return EXIT_VERB_ERROR; }
}