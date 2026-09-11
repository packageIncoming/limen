#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include <ostream>
#include <sys/poll.h>
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
#include <cmath>

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

int post_send(
    bool signaled,
    uint32_t seq,
    uint32_t num_slots,
    uint64_t buff_addr, 
    ibv_qp* queue_pair, 
    uint32_t message_size,
    uint32_t lkey,
    bool inline_enabled)
{
    //  create the ibv_send_wr
    ibv_send_wr wr{};
    ibv_sge sge{};
    ibv_send_wr* bad = nullptr;

    //  for now each entry has a single SGE
    //  slot[i] has address &(buffer) + ([i]* [message_size])
    uint32_t slot = seq % num_slots;
    sge.addr = limen::slot_addr(buff_addr, slot, message_size);
    sge.length = message_size;
    sge.lkey = lkey;

    wr.num_sge = 1;
    wr.sg_list = &sge;
    wr.next = nullptr;
    wr.wr_id = seq | SEND_WRID_TAG;
    wr.opcode =  IBV_WR_SEND;
    wr.send_flags =   IBV_SEND_FENCE;
    
    if (inline_enabled)
    {
        wr.send_flags |= IBV_SEND_INLINE;
    }
    if (signaled) 
    {
        wr.send_flags |= IBV_SEND_SIGNALED;
    }

    return ibv_post_send(queue_pair, &wr, &bad);
}

void report_config_mode(const pingpong_parsed_args& args, const limen::SessionConfig& cfg)
{
    ibv_qp_init_attr qp_init_attr{};
    ibv_device_attr device_attr{};
    limen::Context ctx(args.device_name);

    if (ibv_query_device(ctx.get(), &device_attr) != 0)
    {
        throw limen::VerbsError("bla bla bla",errno);
    }

    //  This section is sort of a hacky way to get around making a Session while still getting accurate data
    //  Calculate the allocation request amounts
    //  NOTE: certain adapters evenly split max_qp_wr across send and recv work requests
    uint32_t cqe = cfg.cqe > 0 ? std::min(cfg.cqe, device_attr.max_cqe) : device_attr.max_cqe;
    uint32_t recv_wr = std::min(cfg.recv_wr, (uint32_t)device_attr.max_qp_wr/2);
    uint32_t send_wr = std::min(cfg.send_wr, (uint32_t)device_attr.max_qp_wr/2);

    std::cout << "send_wr: " << send_wr << " recv_wr: " << recv_wr << std::endl;
    limen::ProtectionDomain pd(ctx);
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
}

bool can_post(RunConfig &run_config, RunState &state)
{
    uint64_t outstanding_sends = state.posted - state.covered;
    bool has_work = run_config.is_client ? true : state.responses_owed>0;
    return (
        has_work && 
        state.posted < run_config.iterations && 
        (outstanding_sends < run_config.eff_pipeline) &&
        (run_config.inline_ok || state.covered+ run_config.eff_pipeline>state.posted)
    );
}


//  0 on success 1 on fail
int post_one(limen::Session &session, RunConfig &run_config, RunState &state)
{
    uint64_t send_slot_num = state.posted % run_config.send_slots;
    void* send_addr = reinterpret_cast<void*>(limen::slot_addr((uint64_t)(uintptr_t)session.send_mr()->addr, send_slot_num, run_config.message_size));
    limen::fill_pattern(send_addr, run_config.message_size, state.posted);
    bool signal_this_event =  (state.posted % run_config.eff_signal_every == 0 || (state.posted+1 == run_config.iterations));
    
    int rc = post_send(
        signal_this_event,
        state.posted,
        run_config.send_slots,
        (uint64_t)(uintptr_t)session.send_mr()->addr, session.qp(),
        run_config.message_size,
        session.send_mr()->lkey,
        run_config.inline_ok
    );
    if (rc != 0)
    {
        fprintf(stderr,"post_one:post_send %s (%s)\n",strerrorname_np(rc),strerror(rc));
        return EXIT_VERB_ERROR;
    }
    state.posted++;
    if (run_config.is_client == false)
    {
        state.responses_owed--;
    }

    return 0;
}

//  returns 0 on successful WC, 1 if the WC is not successful, -1 on unexpected error 
int handle_wc(ibv_wc& wc, limen::Session &session, RunConfig &run_config, RunState &state)
{
    ibv_qp_init_attr qp_init_attr{};
    ibv_qp_attr      qp_attr{};
    if (wc.status != IBV_WC_SUCCESS)
    {
        //  if the status is not successful then WCs from this one onward
        //  are bad & have to be flushed accordingly
        std::cout << std::format("qp_num={:#08x}\n",session.qp()->qp_num);
        std::cout << "\tnote: opcode and byte_len are not valid on an error completion\n";
        ibv_query_qp(session.qp(), &qp_attr, IBV_QP_STATE, &qp_init_attr);
        std::cout << "qp_state_after_error: " << limen::qp_state_to_str(qp_attr.cur_qp_state)  << std::endl;
        if (state.first_error_status == IBV_WC_SUCCESS)
            state.first_error_status = wc.status;
        return 1;
    }
    else
    {
        //  successful, increment counters
        if (wc.opcode == IBV_WC_SEND)
        {
            uint64_t send_seq_num = limen::Session::remove_tags(wc.wr_id);
            state.covered = send_seq_num+1;
            state.send_completions++;
        }
        else if(wc.opcode == IBV_WC_RECV)
        {
            //  verify the payload
            uint32_t recv_slot_num = limen::Session::remove_tags(wc.wr_id);
            void* recv_addr = reinterpret_cast<void*>(
                limen::slot_addr((uint64_t)(uintptr_t)session.recv_mr()->addr, recv_slot_num, run_config.message_size));
            if (limen::verify_pattern(recv_addr, wc.byte_len, state.recv_count) > 0) state.mismatches++;
            //  repost the recv
            int rc = session.repost_recv(recv_slot_num);
            if (rc !=0)
            {
                //  failed to allocate slot
                fprintf(stderr,"handle_wc:post_recv %s (%s)\n",strerrorname_np(rc),strerror(rc));
                return -1;
            }
            state.recv_count++;
            if (run_config.is_client == false)
            {
                state.responses_owed++;
            }
        }

    }
    return 0;
}


int drain(limen::Session &session, RunConfig &run_config, RunState &state)
{
    int handled = 0;

    //  poll->handle() to clear out what's present
    ibv_wc wc;
    while (ibv_poll_cq(session.cq(),1,&wc)>0) 
    {
        if (handle_wc(wc,session,run_config,state) <0) return -1;
        std::cout << limen::wc_to_str(wc) << std::endl;
        handled++;
    }
    return handled;
}

//  handles reaping cqe in event mode
int reap_event(limen::Session &session, RunConfig &run_config, RunState &state)
{
    int handled = 0;
    ibv_wc wc{};

    //  poll->handle() to clear out what's present
    handled = drain(session,run_config,state);
    if (handled < 0) return -1;
    if (handled > 0) return handled;        // caller may be able to post now

    //  arm()
    if (session.req_notify_cq(0) !=0) return -1;

    //  poll()->handle() to get race polls
    if (!run_config.broken_arming_enabled)
    {
        int n = drain(session, run_config, state);
        if (n < 0) return -1;
        if (n > 0) { state.race_polls_hit++; return n; }
    }

    //poll on completion channel fd
    pollfd pfd{};
    pfd.fd = session.comp_channel_fd();
    pfd.events = POLLIN;
    int poll_result = poll(&pfd, 1, run_config.poll_timeout_ms);
    if (poll_result == 0)
    {
        state.timeout = true;
        return handled;
    } 
    else if (poll_result <0)
    {
        if (errno == EINTR) return handled;
        return -1;
    }

    //  get_cq_event()
    if (session.get_cq_event() != 0) return handled;
    state.events_received++;

    //  poll()->handle()
    int start_count = handled;
    while (ibv_poll_cq(session.cq(),1,&wc)>0) 
    {
        if (handle_wc(wc,session,run_config,state) <0) return -1;
        std::cout << limen::wc_to_str(wc) << std::endl;
        handled++;
    }
    if (start_count == handled)
        state.empty_events++;   // no completions 

    //  ack()
    if (state.events_received - state.events_acked >= 64)
    {
        if (session.ack_cq_events(64)!=0)
            return -1;
        state.events_acked +=64;
    }

    return handled;   
}

//  handles reaping cqe in poll mode
int reap_poll(limen::Session &session, RunConfig &run_config, RunState &state)
{
    return  drain(session, run_config, state);
}



//  returns # of successful reaps or -1 on fail
int reap(limen::Session &session, RunConfig &run_config, RunState &state)
{
    return run_config.wc_reap_mode == reap_mode::EVENT ? 
        reap_event(session,run_config,state) : 
        reap_poll(session,run_config,state);
}

int main(int argc, char* argv[])
{
    try
    {
        pingpong_parsed_args args{};
        parse_argv(argc, argv, &args);

        if (args.unsignaled)
        {
            args.signal_every = std::min(args.pipeline,UINT64_MAX);
        }

        int  exit_rc = EXIT_SUCCESS;
        bool is_client = false;

        //  Everything sizes off the pipeline depth. The QP does not exist yet, so
        //  query the device and pre-clamp so creation cannot fail. The granted
        //  max_send_wr can still come back lower, so eff_pipeline is re-clamped
        //  after finish().
        uint32_t dev_max_qp_wr = 0;
        {
            limen::Context probe(args.device_name);   //  requires -d
            ibv_device_attr da{};
            if (ibv_query_device(probe.get(), &da) != 0)
            {
                throw limen::SessionError("ibv_query_device (pipeline pre-clamp)", errno);
            }
            dev_max_qp_wr = (uint32_t)da.max_qp_wr;
            //  Exit early on invalid resource request 
            if (args.signal_every >= dev_max_qp_wr)
            {
                std::fprintf(stderr,
                    "--signal-every %" PRIu64 " must be less than device max_qp_wr (%u)\n",
                    args.signal_every, dev_max_qp_wr);
                return EXIT_USAGE_ERROR;
            }
        }

        //  A window wider than the ring stalls at the ring; a period wider than the
        //  window deadlocks, because only a signalled completion reopens the window.
        //  Collapsing all three onto one number makes both unreachable.
        uint64_t max_send_slots = std::max<uint64_t>(1, SEND_MR_BYTE_CAP / args.message_size);
        uint64_t eff_pipeline = std::min({ args.pipeline,(uint64_t)dev_max_qp_wr,max_send_slots });
        uint64_t eff_signal_every = std::min<uint64_t>(args.signal_every, eff_pipeline);

        limen::SessionConfig cfg{};

        cfg.recv_wr        = args.no_recv ? 0 : RECV_QUEUE_DEPTH;
        cfg.recv_slots     = cfg.recv_wr;          // a receive consumes one of each
        cfg.recv_slot_size = args.message_size;

        cfg.send_slots     = eff_pipeline;         // one slot per in-flight send, no more
        cfg.send_wr        = std::max<uint64_t>(eff_pipeline * 4, 64);
        cfg.send_slot_size = args.message_size;

        //  Sends in flight plus posted receives must both fit, or the CQ overruns,
        //  which arrives as an async catastrophic event rather than a poll error.
        cfg.cqe            = (int)(cfg.send_wr + cfg.recv_wr + 16);

        cfg.retry_count     = 7;                  // transport retries on timeout/NAK
        cfg.rnr_retry_count = args.rnr_retry;     // retries specifically on receiver-not-ready
        cfg.tcp_port            = (uint16_t)args.tcp_port;
        cfg.initiator_depth     = 1;
        cfg.responder_resources = 1;

        cfg.use_comp_channel = (args.reap == reap_mode::EVENT);

        //  Perform --report-config path & exit early
        if (args.report_config)
        {
            if (args.device_name == nullptr)
            {
                std::fprintf(stderr, "--report-config requires -d <device>\n");
                return EXIT_USAGE_ERROR;
            }
            report_config_mode(args,cfg);
            return EXIT_SUCCESS;
        }

        //  print role
        if (args.addr != nullptr)
        {
            printf("role: client\n");
            is_client = true;
        } else
        {
            printf("role: server\n");
        }

        //  Create session
        limen::Session session = is_client ? 
            limen::Session::create_client_session(args.addr, cfg) : 
            limen::Session::create_server_session(cfg);
        
        //  Clamp again against the granted maximum, update eff_signal_every in response too                
        eff_pipeline = std::min<uint64_t>(eff_pipeline, session.max_send_wr());
        eff_signal_every = std::min<uint64_t>(eff_signal_every, eff_pipeline);

        //  apply moderate
        const char* moderation = "off";
        char modbuf[32];
        if (args.moderate)
        {
            ibv_modify_cq_attr attr{};
            attr.attr_mask          = IBV_CQ_ATTR_MODERATE;
            attr.moderate.cq_count  = (uint16_t)args.moderate_count;
            attr.moderate.cq_period = (uint16_t)args.moderate_usec;

            if (ibv_modify_cq(session.cq(), &attr) == 0)
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

        //  figure out if inline applies
        bool inline_enabled = args.inline_data;
        if (inline_enabled && session.max_inline_data() < args.message_size)
        {
            std::cout << 
            std::format(
                "inline requested but message size ({} bytes) > max_inline_data ({} bytes)\n\tinlining will not apply, use smaller message size",
                args.message_size,
                session.max_inline_data()
            ) << std::endl;
            inline_enabled = false;
        }
        //  now create RunConfig which stores final values for configurations
        RunConfig run_config{};
        run_config.iterations = args.iterations;
        run_config.message_size = args.message_size;
        run_config.eff_pipeline = eff_pipeline;
        run_config.eff_signal_every = eff_signal_every;
        run_config.send_slots = cfg.send_slots; //  always valid since it's sized to pipeline depth or to the maximum memory size
        run_config.granted_send_wr = session.max_send_wr();
        run_config.inline_ok = inline_enabled;
        run_config.is_client = is_client;
        run_config.wc_reap_mode = args.reap;
        run_config.rnr_retry = args.rnr_retry;
        run_config.unsignaled = args.unsignaled;
        run_config.broken_arming_enabled = args.broken_arming;

        /////////////////////////////////////////
        // END OF SETUP
        ////////////////////////////////////////

        //  print configuration information
        std::cout << std::format(
            "config: inline={}(max={}) signal_every={} pipeline={} reap={} moderation={}",
            run_config.inline_ok? "on":"off",
            session.max_inline_data(),
            run_config.eff_signal_every,
            run_config.eff_pipeline,
            run_config.wc_reap_mode == reap_mode::POLL? "poll": "event",
            moderation
        ) << std::endl;

        //  print peer information
        std::cout << std::format(
            "peer: addr={:#016x} rkey={:#08x} length={}\n",
            session.peer().addr,
            session.peer().rkey,
            session.peer().length
        );
        //  print pingpong-specific information
        std::cout << std::format(
            "pingpong: role={} iterations={} size={} signaled={} rnr_retry={}",
            is_client ? "client" : "server",
            run_config.iterations,
            run_config.message_size,
            run_config.unsignaled ? "no" : "yes",
            run_config.rnr_retry
        ) << std::endl;

        //  print queue information
        std::printf("recv: posted=%u depth=%u size=%u\n",
                    cfg.recv_wr, cfg.recv_wr, cfg.recv_slot_size);
        //  cm event printouts  
        if (is_client) {
            std::printf("cm: event ADDR_RESOLVED\n");
            std::printf("cm: event ROUTE_RESOLVED\n");
            std::printf("cm: event ESTABLISHED\n");
        } else {
            std::printf("cm: event CONNECT_REQUEST (adopted new id)\n");
            std::printf("cm: event ESTABLISHED\n");
        }

        //  now create RunState which stores variables and counters relevant to the pingpong loop
        RunState state{};

        //####################//
        // MAIN PINGPONG LOOP //
        // client sends #[pipeline] SENDs, server responds to each one by one 
        //####################//
        std::chrono::time_point last_valid_check = std::chrono::steady_clock::now();
        //  poll for completion in a loop w/ 10 second timeout
        while (true)
        {
                
            while (can_post(run_config,state))
                if (post_one(session,run_config,state) != 0) return EXIT_VERB_ERROR;


            int reap_count =reap(session,run_config,state); 
            if ( reap_count <0) return EXIT_VERB_ERROR;
            
            if (state.first_error_status != IBV_WC_SUCCESS )  break;
            if (state.timeout == true)
            {
                fprintf(stderr,"main:reap_event loop timeout\n");
                exit_rc = EXIT_COMPLETION_CHANNEL_TIMEOUT;
                break;
            }

            if (reap_count >0)
                //  update last_valid check
                last_valid_check = std::chrono::steady_clock::now();
            
            //  if its been more than 10sec without a valid (cqe_count>0) check then break as timeout
            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - last_valid_check) > std::chrono::seconds(10))
            {
                fprintf(stderr,"main:ibv_poll_cq loop timeout (10sec)\n");
                exit_rc = EXIT_COMPLETION_STATUS_ERROR;
                break;
            }

            //  Successful completion exit
            if (state.recv_count      >= run_config.iterations
                && state.posted       >= run_config.iterations
                && state.covered      >= run_config.iterations
                && state.responses_owed == 0) break;
        }

        std::cout << std::format(
            "result: iterations={} sent={} received={} send_completions={} mismatches={} ",
            run_config.iterations,
            state.posted,
            state.recv_count,
            state.send_completions,
            state.mismatches
        );

        if (state. first_error_status != IBV_WC_SUCCESS)
            std::cout << std::format(" first_error={}",limen::wc_status_name(state.first_error_status));
        
        std::cout << std::endl;

        //  ack remaining WCs
        uint32_t owed = (state.events_received - state.events_acked);
        if (owed > 0)
        {
            session.ack_cq_events(owed);
            state.events_acked += owed;
        }

        std::cout << std::format(
            "events: received={} acked={} empty_events={} race_polls_hit={}",
            state.events_received,
            state.events_acked,
            state.empty_events,
            state.race_polls_hit
        ) << std::endl;

        //  send disconnect if client
        if (is_client)
        {
            std::cout << "cm: disconnect requested" << std::endl;
            session.disconnect();
        }
        //  wait for disconnect or timeout_wait
        session.wait_for_disconnect(10000); // wait 10sec for disconnect
        std::cout << "cm: event DISCONNECTED" << std::endl;


        //  print out teardown diagnostics
        std::printf("teardown: qp=%s cq=%s rx_mr=%s tx_mr=%s pd=%s id=%s channel=%s context=%s\n",
                    session.qp()      ? "ok" : "n/a",
                    session.cq()      ? "ok" : "n/a",
                    session.recv_mr() ? "ok" : "n/a",
                    session.send_mr() ? "ok" : "n/a",
                    session.pd()          ? "ok" : "n/a",
                    session.id() ? "ok" : "n/a",
                    session.ec() ? "ok" : "n/a",
                    session.id()->verbs ? "ok" : "n/a"
                );
        return exit_rc;
    }
    catch (const limen::SessionError& e) { fprintf(stderr, "%s\n", e.what()); return EXIT_CONNECTION_MANAGER_FAILURE; }
    catch (const limen::VerbsError& e)   { fprintf(stderr, "%s\n", e.what()); return EXIT_VERB_ERROR; }
    catch (const std::exception& e)      { fprintf(stderr, "unhandled: %s\n", e.what()); return EXIT_VERB_ERROR; }
}