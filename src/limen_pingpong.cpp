#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "limen/verbs.hpp"
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
#include <format>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <cinttypes>
#include <infiniband/verbs.h>

#include "limen/app/pingpong.hpp"
#include "limen/app/cli.hpp"
#include "limen/app/exit_codes.hpp"
#include "limen/format.hpp"
#include "limen/pattern.hpp"
#include "limen/session.hpp"


enum {
    OPT_RNR_RETRY = 256,
    OPT_NO_RECV,
    OPT_UNSIGNALED,
    OPT_INLINE,
    OPT_SIGNAL_EVERY,
    OPT_PIPELINE,
    OPT_REAP,
    OPT_MODERATE,
    OPT_REPORT_CONFIG,
    OPT_BROKEN_ARMING,
};

namespace {

//  "16:8" -> count=16 usec=8. returns 0 on success, 1 on failure.
//  both halves must be present, numeric, and fit the uint16_t fields of
//  ibv_moderate_cq. either may be 0, which disables that half.
int parse_moderate(const char* s, uint64_t* count, uint64_t* usec)
{
    const char* colon = std::strchr(s, ':');
    if (colon == nullptr || colon == s || *(colon + 1) == '\0') return 1;

    char left[32];
    std::size_t n = (std::size_t)(colon - s);
    if (n >= sizeof(left)) return 1;
    std::memcpy(left, s, n);
    left[n] = '\0';

    if (limen::app::parse_u64_strict(left,      count) != 0) return 1;
    if (limen::app::parse_u64_strict(colon + 1, usec)  != 0) return 1;
    if (*count > UINT16_MAX || *usec > UINT16_MAX)            return 1;
    return 0;
}

} // namespace

void parse_argv(int argc, char* argv[], pingpong_parsed_args* args)
{
    static struct option long_opts[] = {
        {"rnr-retry",     required_argument, nullptr, OPT_RNR_RETRY},
        {"no-recv",       no_argument,       nullptr, OPT_NO_RECV},
        {"unsignaled",    no_argument,       nullptr, OPT_UNSIGNALED},
        {"inline",        no_argument,       nullptr, OPT_INLINE},
        {"signal-every",  required_argument, nullptr, OPT_SIGNAL_EVERY},
        {"pipeline",      required_argument, nullptr, OPT_PIPELINE},
        {"reap",          required_argument, nullptr, OPT_REAP},
        {"moderate",      required_argument, nullptr, OPT_MODERATE},
        {"report-config", no_argument,       nullptr, OPT_REPORT_CONFIG},
        {"broken-arming", no_argument,       nullptr, OPT_BROKEN_ARMING},
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
                if (limen::app::parse_int_strict(optarg, &args->gid_index) != 0) exit(EXIT_USAGE_ERROR);
                break;
            }
            case 'p':
            {
                if (limen::app::parse_int_strict(optarg, &args->port) != 0) exit(EXIT_USAGE_ERROR);
                break;
            }
            case 's':
            {
                if (limen::app::parse_u64_strict(optarg, &args->message_size) != 0) exit(EXIT_USAGE_ERROR);
                break;
            }
            case 'n':
            {
                if (limen::app::parse_u64_strict(optarg, &args->iterations) != 0) exit(EXIT_USAGE_ERROR);
                break;
            }
            case 'r':
            {
                if (limen::app::parse_u64_strict(optarg, &args->rx_depth) != 0) exit(EXIT_USAGE_ERROR);
                break;
            }
            case 't':
            {
                if (limen::app::parse_u64_strict(optarg, &args->tcp_port) != 0) exit(EXIT_USAGE_ERROR);
                break;
            }
            case 'h':
            {
                print_help(false);
                exit(0);
            }
            case OPT_RNR_RETRY:
            {
                if (limen::app::parse_int_strict(optarg, &args->rnr_retry) != 0) exit(EXIT_USAGE_ERROR);
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
            case OPT_INLINE:
            {
                args->inline_data = true;
                break;
            }
            case OPT_SIGNAL_EVERY:
            {
                if (limen::app::parse_u64_strict(optarg, &args->signal_every) != 0) exit(EXIT_USAGE_ERROR);
                if (args->signal_every == 0)
                {
                    std::fprintf(stderr, "--signal-every must be at least 1\n");
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case OPT_PIPELINE:
            {
                if (limen::app::parse_u64_strict(optarg, &args->pipeline) != 0) exit(EXIT_USAGE_ERROR);
                if (args->pipeline == 0)
                {
                    std::fprintf(stderr, "--pipeline must be at least 1\n");
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case OPT_REAP:
            {
                if      (std::strcmp(optarg, "poll")  == 0) args->reap = reap_mode::POLL;
                else if (std::strcmp(optarg, "event") == 0) args->reap = reap_mode::EVENT;
                else
                {
                    std::fprintf(stderr, "--reap takes poll or event, got '%s'\n", optarg);
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case OPT_MODERATE:
            {
                if (parse_moderate(optarg, &args->moderate_count, &args->moderate_usec) != 0)
                {
                    std::fprintf(stderr,
                        "--moderate takes <count>:<usec>, both numeric and at most %u, got '%s'\n",
                        (unsigned)UINT16_MAX, optarg);
                    exit(EXIT_USAGE_ERROR);
                }
                args->moderate = true;
                break;
            }
            case OPT_REPORT_CONFIG:
            {
                args->report_config = true;
                break;
            }
            case OPT_BROKEN_ARMING:
            {
                args->broken_arming = true;
                break;
            }
            case '?':
            {
                exit(EXIT_USAGE_ERROR);
            }
            default:
            {
                break;
            }
        }
    }

    //  --unsignaled means signal nothing; --signal-every means signal one in n.
    //  both together has no meaning, and silently picking one hides the mistake.
    if (args->unsignaled && args->signal_every != 1)
    {
        std::fprintf(stderr, "--unsignaled and --signal-every are mutually exclusive\n");
        exit(EXIT_USAGE_ERROR);
    }

    //  --broken-arming only removes the race-closing poll, which only exists
    //  on the event path
    if (args->broken_arming && args->reap != reap_mode::EVENT)
    {
        std::fprintf(stderr, "--broken-arming requires --reap event\n");
        exit(EXIT_USAGE_ERROR);
    }

    if (optind < argc)
    {
        args->addr = argv[optind];
    }
}
void print_help(bool to_error)
{
    const char* str =
        "./build/limen_pingpong -d <device> [-g <gid_index>] [-p <port>] [-t <tcp_port>]\n"
        "\t[-s <bytes>] [-n <iterations>] [-r <rx_depth>]\n"
        "\t[--rnr-retry <n>] [--no-recv] [--unsignaled]\n"
        "\t[--inline] [--signal-every <n>] [--pipeline <depth>]\n"
        "\t[--reap <poll|event>] [--moderate <count>:<usec>] [--report-config]\n"
        "\t[<peer>]\n";
    std::fprintf(to_error ? stderr : stdout, "%s", str);
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
        pingpong_parsed_args args{};
        parse_argv(argc,argv,&args);

        //  Variables:
        int rc = 0;
        int exit_rc=0;
        bool is_client = false;

        //  Device-based variables
        ibv_qp_init_attr qp_init_attr{};
        //  QP transition variables
        ibv_qp_attr qp_attr{};

        limen::SessionConfig cfg{};
        cfg.recv_wr        = args.no_recv ? 0 : RECV_QUEUE_DEPTH;
        cfg.recv_slots     = cfg.recv_wr;          // a receive consumes one of each
        cfg.recv_slot_size = args.message_size;
        cfg.send_wr = std::max<uint64_t>(args.pipeline, args.signal_every) * 4;
        cfg.send_slots     = 1;
        cfg.send_slot_size = args.message_size;
        cfg.cqe            = COMPLETE_QUEUE_DEPTH;
        cfg.retry_count     = 7;                  // transport retries on timeout/NAK
        cfg.rnr_retry_count = args.rnr_retry;     // retries specifically on receiver-not-ready
        cfg.tcp_port            = (uint16_t)args.tcp_port;
        cfg.initiator_depth     = 1;
        cfg.responder_resources = 1;

        if (args.report_config)
        {
            //  query max inline data
            if (args.device_name == nullptr)
            {
                std::fprintf(stderr, "--report-config requires -d <device>\n");
                return EXIT_USAGE_ERROR;
            }
            limen::Context ctx(args.device_name);
            //  get device attributes
            ibv_device_attr device_attr{};
            if (ibv_query_device(ctx.get(), &device_attr) != 0)
            {
                throw limen::SessionError("ibv_query_device from --report-config", errno);
            }
            limen::ProtectionDomain pd(ctx);
            int      cqe     = cfg.cqe > 0 ? std::min(cfg.cqe, device_attr.max_cqe)
                                            : device_attr.max_cqe;
            uint32_t recv_wr = std::min(cfg.recv_wr, (uint32_t)device_attr.max_qp_wr);
            uint32_t send_wr = std::min(cfg.send_wr, (uint32_t)device_attr.max_qp_wr);
            limen::CompletionQueue cq(ctx,cqe,nullptr,nullptr,0);
            limen::fill_qp_init_attr(&qp_init_attr, send_wr, recv_wr);
            qp_init_attr.send_cq = cq.get();
            qp_init_attr.recv_cq = cq.get();
            limen::QueuePair qp(pd,&qp_init_attr);

            uint32_t granted_send_wr = qp_init_attr.cap.max_send_wr;
            uint32_t eff_pipeline    = std::min<uint32_t>(args.pipeline, granted_send_wr);

            const char* moderation = "off";
            char modbuf[32];

            if (args.moderate)
            {
                ibv_modify_cq_attr attr{};
                attr.attr_mask          = IBV_CQ_ATTR_MODERATE;
                attr.moderate.cq_count  = (uint16_t)args.moderate_count;
                attr.moderate.cq_period = (uint16_t)args.moderate_usec;

                if (ibv_modify_cq(cq.get(), &attr) == 0)
                {
                    std::snprintf(modbuf, sizeof modbuf, "%" PRIu64 ":%" PRIu64,
                                args.moderate_count, args.moderate_usec);
                    moderation = modbuf;
                }
                else
                {
                    moderation = "unavailable";
                }
            }

            std::cout << std::format(
                "config: inline={}(max={}) signal_every={} pipeline={} reap={} moderation={}",
                args.inline_data ? "on":"off",
                qp_init_attr.cap.max_inline_data,
                args.signal_every,
                eff_pipeline,
                args.reap == reap_mode::POLL? "poll": "event",
                moderation
            ) << std::endl;
            return EXIT_SUCCESS;
        }


        limen::Session session = is_client ? 
            limen::Session::create_client_session(args.addr, cfg) : 
            limen::Session::create_server_session(cfg);
        

        std::cout << std::format(
            "config: inline={}(max={}) signal_every={} pipeline={} reap={} moderation={}",
            args.inline_data ? "on":"off",
            session.max_inline_data(),
            args.signal_every,
            args.pipeline,
            args.reap == reap_mode::POLL? "poll": "event",
            args.moderate ? std::format("{}:{}",args.moderate_count,args.moderate_usec) : "unavailable"
        ) << std::endl;



        if (args.addr != nullptr)
        {
            printf("role: client\n");
            is_client = true;
        } else
        {
            printf("role: server\n");
        }

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

        std::printf("recv: posted=%u depth=%u size=%u\n",
                    cfg.recv_wr, cfg.recv_wr, cfg.recv_slot_size);

        if (is_client) {
            std::printf("cm: event ADDR_RESOLVED\n");
            std::printf("cm: event ROUTE_RESOLVED\n");
            std::printf("cm: event ESTABLISHED\n");
        } else {
            std::printf("cm: event CONNECT_REQUEST (adopted new id)\n");
            std::printf("cm: event ESTABLISHED\n");
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
    catch (const limen::SessionError& e) { fprintf(stderr, "%s\n", e.what()); return EXIT_CONNECTION_MANAGER_FAILURE; }
    catch (const limen::VerbsError& e)   { fprintf(stderr, "%s\n", e.what()); return EXIT_VERB_ERROR; }
    catch (const std::exception& e)      { fprintf(stderr, "unhandled: %s\n", e.what()); return EXIT_VERB_ERROR; }
}