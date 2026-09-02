#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <netinet/in.h>
#include <rdma/rdma_cma.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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

#include "limen/app/pingpong.hpp"
#include "limen/app/cli.hpp"
#include "limen/app/exit_codes.hpp"
#include "limen/format.hpp"
#include "limen/pattern.hpp"
#include "limen/session.hpp"

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
                int rc = limen::app::parse_int_strict(optarg,&args->gid_index);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 'p': 
            {
                int rc = limen::app::parse_int_strict(optarg,&args->port);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 's':
            {
                int rc = limen::app::parse_u64_strict(optarg,&args->message_size);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 'n':
            {
                int rc = limen::app::parse_u64_strict(optarg,&args->iterations);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 'r':
            {
                int rc = limen::app::parse_u64_strict(optarg,&args->rx_depth);
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
                int rc = limen::app::parse_u64_strict(optarg, &args->tcp_port);
                if (rc != 0)
                {
                    exit(EXIT_USAGE_ERROR);
                }
                break;

            }
            case OPT_RNR_RETRY:
            {
                int rc = limen::app::parse_int_strict(optarg, &args->rnr_retry);
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
    fprintf(stderr,"qp_state: %s\n",limen::qp_state_to_str(qp_attr->qp_state).c_str());
    fprintf(stderr,"pkey_index: %i\n",qp_attr->pkey_index);
    fprintf(stderr,"port_num: %i\n",qp_attr->port_num);
    fprintf(stderr,"qp_access_flags: %i\n",qp_attr->qp_access_flags);


}

void print_init_rtr_fail(int rc, ibv_qp_attr* qp_attr)
{
    fprintf(stderr,"state: INIT -> RTR FAILED: %s (%s)\n", strerrorname_np(rc),std::strerror(rc));
    fprintf(stderr,"attr_mask: IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER\n");
    
    //  print the fields that were changed
    fprintf(stderr, "qp_state: %s\n", limen::qp_state_to_str(qp_attr->qp_state).c_str());
    fprintf(stderr, "path_mtu: %i\n", qp_attr->path_mtu);
    fprintf(stderr, "dest_qp_num: %#010x\n", qp_attr->dest_qp_num);
    fprintf(stderr, "rq_psn: %#08x\n", qp_attr->rq_psn);
    fprintf(stderr, "max_dest_rd_atomic: %i\n", qp_attr->max_dest_rd_atomic);
    fprintf(stderr, "min_rnr_timer: %i\n", qp_attr->min_rnr_timer);
    fprintf(stderr, "ah_attr: is_global=%i dlid=%#06x sl=%i src_path_bits=%i\n",
        qp_attr->ah_attr.is_global, qp_attr->ah_attr.dlid,
        qp_attr->ah_attr.sl, qp_attr->ah_attr.src_path_bits);
    fprintf(stderr, "dgid: %s\n", limen::gid_to_str(&qp_attr->ah_attr.grh.dgid).c_str());
    fprintf(stderr, "sgid_index: %i\n", qp_attr->ah_attr.grh.sgid_index);
    fprintf(stderr, "hint: if dgid is all zero or is_global=0, GRH was never populated\n");

}

void print_rtr_rts_fail(int rc, ibv_qp_attr* qp_attr)
{
    fprintf(stderr, "state: RTR -> RTS FAILED: %s (%s)\n", strerrorname_np(rc), std::strerror(rc));
    fprintf(stderr, "attr_mask: IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC\n");
    fprintf(stderr, "qp_state: %s\n", limen::qp_state_to_str(qp_attr->qp_state).c_str());
    fprintf(stderr, "sq_psn: %#08x\n", qp_attr->sq_psn);
    fprintf(stderr, "timeout: %i\n", qp_attr->timeout);
    fprintf(stderr, "retry_cnt: %i\n", qp_attr->retry_cnt);
    fprintf(stderr, "rnr_retry: %i\n", qp_attr->rnr_retry);
    fprintf(stderr, "max_rd_atomic: %i\n", qp_attr->max_rd_atomic);
}


int post_send(bool signaled, uint32_t slot, uint64_t buff_addr, ibv_qp* queue_pair,  uint32_t message_size, uint32_t lkey)
{
    //  create the ibv_send_wr
    ibv_send_wr wr{};
    ibv_sge sge{};
    ibv_send_wr* bad = nullptr;

    //  for now each entry has a single SGE
    //  slot[i] has address &(buffer) + ([i]* [message_size])
    sge.addr = limen::slot_addr(buff_addr, slot, message_size);
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



int main(int argc, char* argv[])
{

    try
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

        ibv_qp_init_attr qp_init_attr{};

        //  QP transition variables
        ibv_qp_attr qp_attr{};


        if (args.addr != nullptr)
        {
            printf("role: client\n");
            is_client = true;
        } else
        {
            printf("role: server\n");
        }


        limen::SessionConfig cfg{};
        cfg.recv_wr        = args.no_recv? 0:RECV_QUEUE_DEPTH;
        cfg.recv_slot_size = args.message_size;
        cfg.recv_size      = args.message_size * std::max(cfg.recv_wr, 1u);
        cfg.send_wr        = args.iterations;
        cfg.send_size      = args.message_size;
        cfg.cqe            = COMPLETE_QUEUE_DEPTH;
        cfg.retry_count     = 7;                  // transport retries on timeout/NAK
        cfg.rnr_retry_count = args.rnr_retry;     // retries specifically on receiver-not-ready
        cfg.tcp_port            = (uint16_t)args.tcp_port;
        cfg.initiator_depth     = 1;
        cfg.responder_resources = 1;
        cfg.access_flags        = IBV_ACCESS_LOCAL_WRITE
                                | IBV_ACCESS_REMOTE_WRITE
                                | IBV_ACCESS_REMOTE_READ;


        limen::Session session = is_client ? 
            limen::Session::create_client_session(args.addr, cfg) : 
            limen::Session::create_server_session(cfg);

        std::cout << std::format(
            "peer: addr={:#016x} rkey={:#08x} length={}\n",
            session.peer().addr,
            session.peer().rkey,
            session.peer().length
        );


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
        int bad_wc_idx = -1; //  also acts as first error index 
        uint32_t send_count = 0;
        uint32_t send_completions = 0;
        uint32_t recv_count = 0;
        uint32_t mismatch_count = 0;

        //  post the initial send work request only if you're the client
        if (is_client)
        {
            void* send_addr = reinterpret_cast<void*>(limen::slot_addr((uint64_t)(uintptr_t)session.send_mr()->addr, 0, args.message_size));
            limen::fill_pattern(send_addr, args.message_size, send_count);
            rc = post_send(!(args.unsignaled),0, (uint64_t)(uintptr_t)session.send_mr()->addr, session.qp(), args.message_size, session.send_mr()->lkey);
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
            int cqe_count = ibv_poll_cq(session.cq(),COMPLETE_QUEUE_DEPTH,wc_arr.data());
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
                    std::cout << limen::wc_to_str(wc) << std::endl;

                    if (wc_arr[i].status != IBV_WC_SUCCESS)
                    {
                        //  if the status is not successful then WCs from this one onward
                        //  are bad & have to be flushed accordingly
                        std::cout << std::format("qp_num={:#08x}\n",session.qp()->qp_num);
                        std::cout << "\tnote: opcode and byte_len are not valid on an error completion\n";
                        ibv_query_qp(session.qp(), &qp_attr, IBV_QP_STATE, &qp_init_attr);
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

                            //  verify the payload
                            uint32_t slot_num = limen::Session::slot_of(wc->wr_id);
                            void* recv_addr = reinterpret_cast<void*>(
                                limen::slot_addr((uint64_t)(uintptr_t)session.recv_mr()->addr, slot_num, args.message_size));
                            if (limen::verify_pattern(recv_addr, wc->byte_len, recv_count) > 0) mismatch_count++;
                            //  repost the recv
                            rc = session.repost_recv(slot_num);
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
                                void* send_addr = reinterpret_cast<void*>(limen::slot_addr((uint64_t)(uintptr_t)session.send_mr()->addr,0,args.message_size));
                                limen::fill_pattern(send_addr, args.message_size, send_count);
                                rc = post_send(!(args.unsignaled), 0, (uint64_t)(uintptr_t) session.send_mr()->addr, session.qp(), args.message_size, session.send_mr()->lkey);
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
        session.wait_for_disconnect(10000); // wait 10sec for disconnect

        std::cout << "cm: event DISCONNECTED" << std::endl;


        std::printf("teardown: qp=%s cq=%s rx_mr=%s tx_mr=%s pd=%s id=%s channel=%s context=%s\n",
                    session.qp()      ? "ok" : "n/a",
                    session.cq()      ? "ok" : "n/a",
                    session.recv_mr() ? "ok" : "n/a",
                    session.send_mr() ? "ok" : "n/a",
                    session.pd()          ? "ok" : "n/a",
                    session.id() ? "ok" : "n/a",
                    session.ec() ? "ok" : "n/a",
                    session.id()->verbs ? "ok" : "n/a");
        return 0;
    }
    catch (const limen::SessionError& e) { fprintf(stderr, "%s\n", e.what()); return EXIT_FAILURE; }
    catch (const limen::VerbsError& e)   { fprintf(stderr, "%s\n", e.what()); return EXIT_FAILURE; }
}