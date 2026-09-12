#include "limen/app/cli.hpp"
#include "limen/app/bench.hpp"
#include "limen/app/exit_codes.hpp"
#include "limen/session.hpp"
#include "limen/verbs.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <getopt.h>
#include <numeric>
#include <thread>
#include <vector>
#include <cinttypes>
#include <unistd.h>
#include <sys/utsname.h>

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

void print_help(bool to_stderr)
{
    FILE* out = to_stderr ? stderr : stdout;

    std::fprintf(out,
        "Usage: limen_bench -d <device> [-t <tcp_port>] --mode <latency|response|bandwidth|sweep>\n"
        "                   [-s <bytes>] [-n <iterations>] [--warmup <n>] [--runs <n>]\n"
        "                   [--rate <ops_per_sec>] [--op <send|write|read>]\n"
        "                   [--inline] [--signal-every <n>] [--pipeline <depth>]\n"
        "                   [--reap <poll|event>] [--moderate <count:usec>] [--report-config]\n"
        "                   [--json <path>] [--clock-floor] <peer>\n"
        "\n"
        "Required:\n"
        "  -d <device>            IB device name to use\n"
        "  --mode <mode>          latency, response, bandwidth, or sweep (default: latency)\n"
        "\n"
        "Benchmark Options:\n"
        "  -t <tcp_port>          TCP port for connection setup (default: 18515)\n"
        "  -s <bytes>             Message size in bytes (default: 4096)\n"
        "  -n <iterations>        Recorded iterations (default: 100)\n"
        "  --warmup <n>           Discarded iterations before recording (default: 10)\n"
        "  --runs <n>             Repeat the whole measurement n times (default: 1)\n"
        "  --rate <ops_per_sec>   Target rate, required by --mode response\n"
        "  --op <send|write|read> RDMA operation to use (default: send)\n"
        "  --json <path>          Write results and raw samples to a JSON file\n"
        "  --clock-floor          Report clock_gettime cost before measuring\n"
        "\n"
        "  --inline               Use inline data for sends\n"
        "  --signal-every <n>     Signal every n-th send (default: 1)\n"
        "  --pipeline <depth>     Operations in flight (default: 1)\n"
        "  --reap <poll|event>    CQ completion notification method (default: poll)\n"
        "  --moderate <c:u>       Set CQ moderation (count:usec, e.g., 16:8)\n"
        "  --report-config        Print the conditions block and exit 0\n"
        "\n"
        "  -h                     Show this help message and exit\n"
        "\n"
        "  <peer>                 IP address or hostname of the peer (server) node.\n"
        "                         Leave empty to run in server mode.\n"
    );
}

void parse_argv(int argc, char* argv[], bench_parsed_args* args)
{
    static struct option long_opts[] = {
        {"mode",          required_argument, nullptr, OPT_MODE},
        {"warmup",        required_argument, nullptr, OPT_WARMUP},
        {"runs",          required_argument, nullptr, OPT_RUNS},
        {"rate",          required_argument, nullptr, OPT_RATE},
        {"op",            required_argument, nullptr, OPT_OP},
        {"json",          required_argument, nullptr, OPT_JSON},
        {"clock-floor",   no_argument,       nullptr, OPT_CLOCK_FLOOR},

        // TRD-07
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
    while ((opt = getopt_long(argc, argv, "d:t:s:n:h", long_opts, nullptr)) != -1)
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
            case 't':
            {
                if (limen::app::parse_u64_strict(optarg, &args->tcp_port) != 0)
                {
                    std::fprintf(stderr, "-t requires a positive integer, got '%s'\n", optarg);
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 's':
            {
                if (limen::app::parse_u64_strict(optarg, &args->message_size) != 0)
                {
                    std::fprintf(stderr, "-s requires a positive integer, got '%s'\n", optarg);
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 'n':
            {
                if (limen::app::parse_u64_strict(optarg, &args->iterations) != 0)
                {
                    std::fprintf(stderr, "-n requires a positive integer, got '%s'\n", optarg);
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case 'h':
            {
                print_help(false);
                exit(0);
            }
            case OPT_MODE:
            {
                if      (std::strcmp(optarg, "latency")   == 0) args->mode = bench_mode::LATENCY;
                else if (std::strcmp(optarg, "response")  == 0) args->mode = bench_mode::RESPONSE;
                else if (std::strcmp(optarg, "bandwidth") == 0) args->mode = bench_mode::BANDWIDTH;
                else if (std::strcmp(optarg, "sweep")     == 0) args->mode = bench_mode::SWEEP;
                else
                {
                    std::fprintf(stderr, "--mode requires latency, response, bandwidth, or sweep. Got '%s'\n", optarg);
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case OPT_WARMUP:
            {
                //  parse_u64_strict rejects 0, which is a legal warmup count
                if (std::strcmp(optarg, "0") == 0) { args->warmup = 0; break; }
                if (limen::app::parse_u64_strict(optarg, &args->warmup) != 0)
                {
                    std::fprintf(stderr, "--warmup requires a non-negative integer, got '%s'\n", optarg);
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case OPT_RUNS:
            {
                if (limen::app::parse_u64_strict(optarg, &args->runs) != 0)
                {
                    std::fprintf(stderr, "--runs requires a positive integer, got '%s'\n", optarg);
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case OPT_RATE:
            {
                if (std::strcmp(optarg, "0") == 0) { args->rate = 0; break; }
                if (limen::app::parse_u64_strict(optarg, &args->rate) != 0)
                {
                    std::fprintf(stderr, "--rate requires a non-negative integer, got '%s'\n", optarg);
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case OPT_OP:
            {
                if      (std::strcmp(optarg, "send")  == 0) args->op = bench_op::SEND;
                else if (std::strcmp(optarg, "write") == 0) args->op = bench_op::WRITE;
                else if (std::strcmp(optarg, "read")  == 0) args->op = bench_op::READ;
                else
                {
                    std::fprintf(stderr, "--op requires send, write, or read. Got '%s'\n", optarg);
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case OPT_JSON:
            {
                args->json_path = optarg;
                break;
            }
            case OPT_CLOCK_FLOOR:
            {
                args->clock_floor = true;
                break;
            }
            case OPT_INLINE:
            {
                args->inline_data = true;
                break;
            }
            case OPT_SIGNAL_EVERY:
            {
                if (limen::app::parse_u64_strict(optarg, &args->signal_every) != 0)
                {
                    std::fprintf(stderr, "--signal-every requires a positive integer, got '%s'\n", optarg);
                    exit(EXIT_USAGE_ERROR);
                }
                break;
            }
            case OPT_PIPELINE:
            {
                if (limen::app::parse_u64_strict(optarg, &args->pipeline) != 0)
                {
                    std::fprintf(stderr, "--pipeline requires a positive integer, got '%s'\n", optarg);
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

    //  --broken-arming only removes the race-closing poll, which only exists
    //  on the event path
    if (args->broken_arming && args->reap != reap_mode::EVENT)
    {
        std::fprintf(stderr, "--broken-arming requires --reap event\n");
        exit(EXIT_USAGE_ERROR);
    }

    if (optind < argc)
    {
        args->peer = argv[optind];
    }

    if (args->peer != nullptr && args->mode == bench_mode::RESPONSE && args->rate == 0)
    {
        std::fprintf(stderr, "--mode response requires --rate > 0\n");
        exit(EXIT_USAGE_ERROR);
    }
}

// ------------------------------------------------------------------ R1

uint64_t measure_clock_floor()
{
    const int DISCARD = 1000;
    const int KEEP    = 10000;

    std::vector<uint64_t> d;
    d.reserve(KEEP);

    for (int i = 0; i < KEEP + DISCARD; ++i)
    {
        uint64_t a = time_ns(), b = time_ns();
        if (i >= DISCARD) d.push_back(b - a);
    }
    std::sort(d.begin(), d.end());
    return d[d.size() / 2];
}

void print_clock_floor(uint64_t floor_ns, uint64_t smallest_expected_ns)
{
    double pct = smallest_expected_ns > 0
               ? (double)floor_ns / (double)smallest_expected_ns * 100.0
               : 0.0;

    std::printf("clock: median=%" PRIu64 " ns, %.2f%% of a %" PRIu64 " ns measurement%s\n",
                floor_ns, pct, smallest_expected_ns,
                pct > 2.0 ? "  [EXCEEDS 2%, state the error bar]" : "");
}

// ------------------------------------------------------------------ R3

static int percentile_as_idx(uint32_t pct, int size)
{
    if (size <= 0) return 0;
    return (int)std::llround((size - 1) * (pct / 100.0));
}

Stats compute(std::vector<uint64_t>& samples)
{
    Stats st{};
    if (samples.empty()) return st;

    std::sort(samples.begin(), samples.end());
    int size = (int)samples.size();

    st.min  = (double)samples.front();
    st.p50  = (double)samples[percentile_as_idx(50, size)];
    st.p90  = (double)samples[percentile_as_idx(90, size)];
    st.p99  = (double)samples[percentile_as_idx(99, size)];
    st.p999 = (double)samples[std::min<int>(size - 1, (int)std::llround((size - 1) * 0.999))];
    st.max  = (double)samples.back();

    double sum = 0.0;
    for (uint64_t v : samples) sum += (double)v;
    st.mean = sum / size;

    double var = 0.0;
    for (uint64_t v : samples) var += ((double)v - st.mean) * ((double)v - st.mean);
    st.stddev = std::sqrt(var / size);

    return st;
}

double median_ns(std::vector<uint64_t>& samples)
{
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    return (double)samples[percentile_as_idx(50, (int)samples.size())];
}

// ------------------------------------------------------------------ planning

void resolve_plan(BenchPlan& plan, const bench_parsed_args& args)
{
    plan.mode           = args.mode;
    plan.record         = args.iterations;
    plan.warmup         = args.warmup;
    plan.rate           = args.rate;
    plan.message_size   = args.message_size;
    plan.moderate       = args.moderate;
    plan.moderate_count = args.moderate_count;
    plan.moderate_usec  = args.moderate_usec;

    switch (args.mode)
    {
        case bench_mode::LATENCY:
        case bench_mode::SWEEP:
            plan.depth           = 1;
            plan.interval_ns     = 0;
            plan.scheduled       = false;
            plan.collect_samples = true;
            break;
        case bench_mode::RESPONSE:
            plan.depth           = args.pipeline;
            plan.interval_ns     = args.rate > 0 ? 1000000000ull / args.rate : 0;
            plan.scheduled       = true;
            plan.collect_samples = true;
            break;
        case bench_mode::BANDWIDTH:
            plan.depth           = args.pipeline;
            plan.interval_ns     = 0;
            plan.scheduled       = false;
            plan.collect_samples = false;
            break;
    }
}

limen::SessionConfig build_session_config(const bench_parsed_args& args, const BenchPlan& plan)
{
    uint32_t dev_max_qp_wr = 0;
    {
        limen::Context probe(args.device_name);
        ibv_device_attr da{};
        if (ibv_query_device(probe.get(), &da) != 0)
            throw limen::SessionError("ibv_query_device (pipeline pre-clamp)", errno);

        dev_max_qp_wr = (uint32_t)da.max_qp_wr;
        if (args.signal_every >= dev_max_qp_wr)
        {
            std::fprintf(stderr,
                "--signal-every %" PRIu64 " must be less than device max_qp_wr (%u)\n",
                args.signal_every, dev_max_qp_wr);
            throw limen::SessionError("signal_every exceeds max_qp_wr", EINVAL);
        }
    }

    uint64_t max_send_slots = std::max<uint64_t>(1, (64ull * 1024 * 1024) / plan.message_size);
    uint64_t eff_pipeline   = std::min({ plan.depth, (uint64_t)dev_max_qp_wr, max_send_slots });

    limen::SessionConfig cfg{};
    cfg.recv_wr        = (uint32_t)std::clamp<uint64_t>((64ull * 1024 * 1024) / plan.message_size, 64, 512);
    cfg.recv_slots     = cfg.recv_wr;
    cfg.recv_slot_size = (uint32_t)plan.message_size;

    cfg.send_slots     = (uint32_t)eff_pipeline;
    cfg.send_wr        = (uint32_t)std::max<uint64_t>(eff_pipeline * 4, 64);
    cfg.send_slot_size = (uint32_t)plan.message_size;
    cfg.cqe            = (int)(cfg.send_wr + cfg.recv_wr + 16);

    cfg.retry_count         = 7;
    cfg.rnr_retry_count     = 7;
    cfg.tcp_port            = (uint16_t)args.tcp_port;
    cfg.initiator_depth     = 1;
    cfg.responder_resources = 1;
    cfg.use_comp_channel    = (args.reap == reap_mode::EVENT);
    return cfg;
}

// ------------------------------------------------------------------ R4, R5

RunResult run_once(const limen::SessionConfig& cfg, RunConfig& rc, const BenchPlan& plan, const char* peer)
{
    RunResult r{};
    uint64_t total_ops = plan.record + plan.warmup;

    limen::Session session;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        try { session = limen::Session::create_client_session(peer, cfg); break; }
        catch (const limen::SessionError&)
        {
            if (attempt == 2) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    rc.iterations       = total_ops;
    rc.message_size     = plan.message_size;
    rc.send_slots       = cfg.send_slots;
    rc.granted_send_wr  = session.max_send_wr();

    rc.eff_pipeline     = std::min<uint64_t>(plan.depth, session.max_send_wr());
    rc.eff_signal_every = std::min<uint64_t>(rc.eff_signal_every, rc.eff_pipeline);
    rc.max_outstanding  = plan.collect_samples ? rc.eff_pipeline : 0;
    rc.is_client        = true;

    if (rc.inline_ok && session.max_inline_data() < plan.message_size) rc.inline_ok = false;

    if (plan.moderate)
    {
        ibv_modify_cq_attr attr{};
        attr.attr_mask          = IBV_CQ_ATTR_MODERATE;
        attr.moderate.cq_count  = (uint16_t)plan.moderate_count;
        attr.moderate.cq_period = (uint16_t)plan.moderate_usec;
        ibv_modify_cq(session.cq(), &attr);
    }

    r.granted_inline   = session.max_inline_data();
    r.eff_pipeline     = rc.eff_pipeline;
    r.eff_signal_every = rc.eff_signal_every;
    r.inline_applied   = rc.inline_ok;

    RunState state{};
    if (plan.collect_samples) r.samples.reserve(plan.record);
    if (plan.scheduled)       r.late_ns.reserve(plan.record);

    //  the measured window opens when the warmup-th operation retires, not at
    //  connect. zero until then.
    uint64_t t_start = 0;
    uint64_t t_end   = 0;

    if (plan.depth == 1 && !plan.scheduled && plan.collect_samples)
    {
        //  closed loop, one outstanding. service time.
        for (uint64_t i = 0; i < total_ops; ++i)
        {
            if (t_start == 0 && i >= plan.warmup) t_start = time_ns();

            uint64_t start_ts = time_ns();

            if (post_one(session, rc, state) != 0) { r.rc = EXIT_VERB_ERROR; break; }

            uint64_t target_recv = state.recv_count + 1;
            while (state.recv_count < target_recv)
            {
                if (reap(session, rc, state) < 0)               { r.rc = EXIT_VERB_ERROR; break; }
                if (state.first_error_status != IBV_WC_SUCCESS) break;
                if (state.timeout)                              { r.rc = EXIT_COMPLETION_CHANNEL_TIMEOUT; break; }
            }
            if (r.rc != 0) break;
            if (state.first_error_status != IBV_WC_SUCCESS) { r.rc = EXIT_COMPLETION_STATUS_ERROR; break; }

            uint64_t dt = time_ns() - start_ts;
            if (i >= plan.warmup) r.samples.push_back(dt);
        }
        t_end = time_ns();
    }
    else
    {
        //  open loop. posts and completions are decoupled, so operation i's
        //  start time lives in start_ts[i] and is retired in post order.
        std::vector<uint64_t> start_ts;
        start_ts.reserve(total_ops);

        uint64_t retired = 0;

        //  schedule origin. zero until warmup retires, so warmup runs
        //  unscheduled and connection setup does not become fake lateness.
        uint64_t sched_base   = 0;
        uint64_t sched_origin = 0;

        auto last_progress = std::chrono::steady_clock::now();

        while (true)
        {
            while (can_post(rc, state))
            {
                uint64_t intended;

                if (plan.scheduled && sched_base != 0)
                {
                    intended = sched_base + (state.posted - sched_origin) * plan.interval_ns;
                    uint64_t now = time_ns();

                    //  early. fall through to reap rather than busy-waiting,
                    //  the completion queue still needs draining.
                    if (now < intended) break;

                    //  late. do NOT sleep, the lateness is the measurement.
                    //  record the magnitude: a spin loop sampling every ~200 ns
                    //  overshoots by nanoseconds on nearly every slot, which is
                    //  not the same event as falling a whole interval behind.
                    if (now > intended)
                    {
                        uint64_t late = now - intended;
                        r.behind++;
                        r.late_ns.push_back(late);
                        if (late > r.max_late_ns) r.max_late_ns = late;
                    }
                }
                else
                {
                    intended = time_ns();
                }

                if (post_one(session, rc, state) != 0) { r.rc = EXIT_VERB_ERROR; break; }
                start_ts.push_back(intended);
            }
            if (r.rc != 0) break;

            //  reap() counts send completions too, so the recv delta is the
            //  only valid reply count
            uint64_t before = state.recv_count;
            if (reap(session, rc, state) < 0) { r.rc = EXIT_VERB_ERROR; break; }
            uint64_t arrived = state.recv_count - before;

            if (arrived > 0)
            {
                if (arrived > r.max_batch) r.max_batch = arrived;
                r.batches++;
                r.batched_recvs += arrived;

                uint64_t now = time_ns();

                for (uint64_t k = 0; k < arrived; k++, retired++)
                {
                    if (retired < plan.warmup) continue;

                    if (t_start == 0)
                    {
                        t_start      = now;
                        sched_base   = now;
                        sched_origin = state.posted;
                    }
                    if (plan.collect_samples) r.samples.push_back(now - start_ts[retired]);
                }

                last_progress = std::chrono::steady_clock::now();
            }

            if (state.first_error_status != IBV_WC_SUCCESS) { r.rc = EXIT_COMPLETION_STATUS_ERROR;    break; }
            if (state.timeout)                              { r.rc = EXIT_COMPLETION_CHANNEL_TIMEOUT; break; }

            if (std::chrono::steady_clock::now() - last_progress > std::chrono::seconds(30))
            {
                std::fprintf(stderr, "run_once: 30s without a completion\n");
                r.rc = EXIT_COMPLETION_STATUS_ERROR;
                break;
            }

            //  retired, not posted. posted means issued, not completed.
            if (retired >= total_ops) break;
        }
        t_end = time_ns();
    }

    r.ops_recorded = plan.collect_samples
                   ? r.samples.size()
                   : (total_ops > plan.warmup ? total_ops - plan.warmup : 0);
    r.bytes        = r.ops_recorded * plan.message_size;
    r.elapsed_ns   = (t_start > 0 && t_end > t_start) ? t_end - t_start : 0;

    uint32_t owed = (uint32_t)(state.events_received - state.events_acked);
    if (owed > 0)
    {
        session.ack_cq_events(owed);
        state.events_acked += owed;
    }

    session.disconnect();

    try { session.wait_for_disconnect(10000); }
    catch (const limen::SessionError&) {}

    return r;
}

// ------------------------------------------------------------------ R7

MultiRunResult run_repeated(const limen::SessionConfig& cfg, RunConfig& rc, const BenchPlan& plan, const char* peer, uint64_t runs)
{
    MultiRunResult results{};

    for (uint64_t i = 0; i < runs; i++)
    {
        RunConfig rc_run = rc;
        RunResult res{};

        try { res = run_once(cfg, rc_run, plan, peer); }
        catch (const limen::SessionError& e)
        {
            std::fprintf(stderr, "run %" PRIu64 " failed: %s\n", i + 1, e.what());
            continue;
        }

        if (res.rc != 0)
        {
            std::fprintf(stderr, "run %" PRIu64 " exited %d\n", i + 1, res.rc);
            continue;
        }

        std::vector<uint64_t> sorted = res.samples;
        double figure = plan.collect_samples ? compute(sorted).p50 : (double)res.elapsed_ns;

        results.medians_ns.push_back(figure);
        results.runs.push_back(std::move(res));
    }

    if (results.medians_ns.empty()) return results;

    double mean = std::accumulate(results.medians_ns.begin(), results.medians_ns.end(), 0.0)
                / results.medians_ns.size();

    auto [lo, hi] = std::minmax_element(results.medians_ns.begin(), results.medians_ns.end());

    results.noise_pct = mean > 0.0 ? (*hi - *lo) / mean * 100.0 : 0.0;

    return results;
}

// ------------------------------------------------------------------ server

int serve_forever(const limen::SessionConfig& cfg, RunConfig& rc)
{
    while (true)
    {
        try
        {
            limen::Session session = limen::Session::create_server_session(cfg);
            RunState state{};

            rc.is_client        = false;
            rc.granted_send_wr  = session.max_send_wr();
            rc.eff_pipeline     = std::min<uint64_t>(cfg.send_slots, session.max_send_wr());
            rc.eff_signal_every = std::min<uint64_t>(rc.eff_signal_every, rc.eff_pipeline);
            rc.iterations       = UINT64_MAX;
            rc.max_outstanding = 0;

            auto last_progress = std::chrono::steady_clock::now();

            while (true)
            {
                while (can_post(rc, state))
                    if (post_one(session, rc, state) != 0) return EXIT_VERB_ERROR;

                int n = reap(session, rc, state);
                if (n < 0) return EXIT_VERB_ERROR;
                if (n > 0) last_progress = std::chrono::steady_clock::now();

                if (state.first_error_status != IBV_WC_SUCCESS) break;
                if (state.timeout) break;

                //  the client is gone or idle. tear down and go back to accepting.
                if (std::chrono::steady_clock::now() - last_progress > std::chrono::seconds(2)) break;
            }

            std::printf("session closed, recv=%" PRIu64 "\n", state.recv_count);
            std::fflush(stdout);

            try { session.wait_for_disconnect(2000); }
            catch (const limen::SessionError&) {}
        }
        catch (const limen::SessionError& e)
        {
            std::fprintf(stderr, "serve_forever: %s\n", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

// ------------------------------------------------------------------ R9

static void read_first_line(const char* path, char* out, size_t n)
{
    out[0] = '\0';
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) return;
    if (std::fgets(out, (int)n, f) != nullptr) out[std::strcspn(out, "\n")] = '\0';
    std::fclose(f);
}

static void read_os_pretty(char* out, size_t n)
{
    out[0] = '\0';
    FILE* f = std::fopen("/etc/os-release", "r");
    if (f == nullptr) return;

    char line[256];
    while (std::fgets(line, sizeof line, f) != nullptr)
    {
        if (std::strncmp(line, "PRETTY_NAME=", 12) != 0) continue;
        const char* v = line + 12;
        if (*v == '"') ++v;
        std::snprintf(out, n, "%s", v);
        out[std::strcspn(out, "\"\n")] = '\0';
        break;
    }
    std::fclose(f);
}

static const char* mtu_str(ibv_mtu m)
{
    switch (m)
    {
        case IBV_MTU_256:  return "256";
        case IBV_MTU_512:  return "512";
        case IBV_MTU_1024: return "1024";
        case IBV_MTU_2048: return "2048";
        case IBV_MTU_4096: return "4096";
        default:           return "unknown";
    }
}

static const char* mode_str(bench_mode m)
{
    switch (m)
    {
        case bench_mode::LATENCY:   return "latency";
        case bench_mode::RESPONSE:  return "response";
        case bench_mode::BANDWIDTH: return "bandwidth";
        case bench_mode::SWEEP:     return "sweep";
    }
    return "unknown";
}

static void now_iso8601(char* out, size_t n)
{
    std::time_t now = std::time(nullptr);
    std::strftime(out, n, "%Y-%m-%dT%H:%M:%S%z", std::localtime(&now));
}

int first_affinity_cpu()
{
    cpu_set_t s;
    CPU_ZERO(&s);
    if (sched_getaffinity(0, sizeof s, &s) != 0) return -1;
    for (int i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &s)) return i;
    return -1;
}

void affinity_str(char* out, size_t n)
{
    cpu_set_t s;
    CPU_ZERO(&s);
    out[0] = '\0';
    if (sched_getaffinity(0, sizeof s, &s) != 0) { std::snprintf(out, n, "unknown"); return; }

    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (CPU_COUNT(&s) >= (int)online) { std::snprintf(out, n, "unpinned (all %ld)", online); return; }

    size_t used = 0;
    for (int i = 0; i < CPU_SETSIZE && used + 8 < n; i++)
        if (CPU_ISSET(i, &s))
            used += (size_t)std::snprintf(out + used, n - used, "%s%d", used ? "," : "", i);
}

void cstate_str(int cpu, char* out, size_t n)
{
    out[0] = '\0';
    if (cpu < 0) { std::snprintf(out, n, "unknown"); return; }

    size_t used = 0;
    for (int i = 0; i < 16; i++)
    {
        char path[256], name[64], dis[64];

        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/name", cpu, i);
        read_first_line(path, name, sizeof name);
        if (!name[0]) break;

        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/disable", cpu, i);
        read_first_line(path, dis, sizeof dis);

        if (used + 24 >= n) break;
        used += (size_t)std::snprintf(out + used, n - used, "%s%s:%s",
                                      used ? " " : "", name, dis[0] == '1' ? "off" : "on");
    }
    if (!out[0]) std::snprintf(out, n, "no cpuidle");
}

void print_conditions(const bench_parsed_args& args, uint64_t clock_floor_ns)
{
    char os[256];     read_os_pretty(os, sizeof os);
    char clksrc[64];  read_first_line("/sys/devices/system/clocksource/clocksource0/current_clocksource", clksrc, sizeof clksrc);
    char datebuf[64]; now_iso8601(datebuf, sizeof datebuf);

    utsname u{};
    uname(&u);

    int  pinned = first_affinity_cpu();
    char affinity[128]; affinity_str(affinity, sizeof affinity);
    char cstates[256];  cstate_str(pinned, cstates, sizeof cstates);

    char path[256];
    char gov[64];
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", pinned >= 0 ? pinned : 0);
    read_first_line(path, gov, sizeof gov);

    char siblings[64];
    siblings[0] = '\0';
    if (pinned >= 0)
    {
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", pinned);
        read_first_line(path, siblings, sizeof siblings);
    }

    char smt[64];
    read_first_line("/sys/devices/system/cpu/smt/control", smt, sizeof smt);

    char devinfo[256];
    std::snprintf(devinfo, sizeof devinfo, "%s (query failed)", args.device_name ? args.device_name : "none");
    try
    {
        limen::Context probe(args.device_name);
        ibv_device_attr da{};
        ibv_port_attr   pa{};
        ibv_query_device(probe.get(), &da);
        ibv_query_port(probe.get(), 1, &pa);
        std::snprintf(devinfo, sizeof devinfo,
                      "%s fw=%s max_qp_wr=%d max_cqe=%d active_mtu=%s",
                      args.device_name, da.fw_ver, da.max_qp_wr, da.max_cqe, mtu_str(pa.active_mtu));
    }
    catch (const std::exception&) {}

    char numa_path[256];
    char numa[64];
    std::snprintf(numa_path, sizeof numa_path,
                  "/sys/class/infiniband/%s/device/numa_node", args.device_name ? args.device_name : "");
    read_first_line(numa_path, numa, sizeof numa);

    std::printf("conditions:\n");
    std::printf("  date            %s\n", datebuf);
    std::printf("  os              %s\n", os[0] ? os : "unknown");
    std::printf("  kernel          %s %s\n", u.sysname, u.release);
    std::printf("  cpus            %ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    std::printf("  affinity        %s\n", affinity[0] ? affinity : "unknown");
    std::printf("  smt             %s%s%s\n",
                smt[0] ? smt : "unknown",
                siblings[0] ? ", siblings " : "",
                siblings[0] ? siblings : "");
    std::printf("  governor        %s\n", gov[0] ? gov : "unknown");
    std::printf("  cstates         %s\n", cstates);
    std::printf("  clocksource     %s\n", clksrc[0] ? clksrc : "unknown");
    std::printf("  device          %s\n", devinfo);
    std::printf("  numa_node       %s\n", numa[0] ? numa : "unknown");
    std::printf("  clock_floor_ns  %" PRIu64 "\n", clock_floor_ns);
    std::printf("  mode            %s\n", mode_str(args.mode));
    std::printf("  message_size    %" PRIu64 "\n", args.message_size);
    std::printf("  iterations      %" PRIu64 "\n", args.iterations);
    std::printf("  warmup          %" PRIu64 "\n", args.warmup);
    std::printf("  runs            %" PRIu64 "\n", args.runs);
    std::printf("  rate            %" PRIu64 "\n", args.rate);
    std::printf("  inline          %s\n", args.inline_data ? "requested" : "off");
    std::printf("  signal_every    %" PRIu64 "\n", args.signal_every);
    std::printf("  pipeline        %" PRIu64 "\n", args.pipeline);
    std::printf("  reap            %s\n", args.reap == reap_mode::POLL ? "poll" : "event");
    std::printf("\n");
}

// ------------------------------------------------------------------ output

void print_stats(const Stats& st, const char* quantity)
{
    std::printf("report: quantity=%s\n", quantity);
    std::printf("percentile:   min    50     90     99     99.9\n");
    std::printf("latency (us): %-6.2f %-6.2f %-6.2f %-6.2f %-6.2f\n",
                st.min / 1000.0, st.p50 / 1000.0, st.p90 / 1000.0, st.p99 / 1000.0, st.p999 / 1000.0);
    std::printf("mean (us):    %-6.2f max (us): %-6.2f stddev (us): %-6.2f\n",
                st.mean / 1000.0, st.max / 1000.0, st.stddev / 1000.0);
    std::printf("median/mean:  %.3f\n\n", st.mean > 0.0 ? st.p50 / st.mean : 0.0);
}

void print_bandwidth(const RunResult& r, uint64_t message_size)
{
    if (r.elapsed_ns == 0)
    {
        std::printf("report: quantity=bandwidth (no measured window)\n\n");
        return;
    }

    double sec    = (double)r.elapsed_ns / 1e9;
    double bytes  = (double)r.bytes;
    double mib_s  = bytes / sec / (1024.0 * 1024.0);
    double gbit_s = bytes * 8.0 / sec / 1e9;
    double msg_s  = (double)r.ops_recorded / sec;

    std::printf("report: quantity=bandwidth\n");
    std::printf("bandwidth: %.2f MiB/s  %.3f Gbit/s  %.0f msg/s  (size=%" PRIu64 " B, %.3f s, %" PRIu64 " ops)\n\n",
                mib_s, gbit_s, msg_s, message_size, sec, r.ops_recorded);
}

void print_schedule(const RunResult& r, uint64_t interval_ns)
{
    std::vector<uint64_t> late = r.late_ns;

    if (late.empty())
    {
        std::printf("schedule: on time for all %" PRIu64 " recorded ops\n", r.ops_recorded);
    }
    else
    {
        Stats st = compute(late);

        uint64_t missed = 0;
        if (interval_ns > 0)
            for (uint64_t v : late)
                if (v > interval_ns) missed++;

        std::printf("schedule: late=%zu/%" PRIu64 " ops  p50=%.3f us  p99=%.3f us  max=%.2f us\n",
                    late.size(), r.ops_recorded,
                    st.p50 / 1000.0, st.p99 / 1000.0, st.max / 1000.0);
        std::printf("          past a full %.2f us slot: %" PRIu64 " (%.2f%%)\n",
                    interval_ns / 1000.0, missed,
                    r.ops_recorded > 0 ? (double)missed / (double)r.ops_recorded * 100.0 : 0.0);
    }

    std::printf("batching: max=%" PRIu64 " mean=%.2f over %" PRIu64 " reaps\n\n",
                r.max_batch,
                r.batches > 0 ? (double)r.batched_recvs / (double)r.batches : 0.0,
                r.batches);
}

void print_noise_floor(const MultiRunResult& m, bool time_units)
{
    if (m.medians_ns.size() < 2)
    {
        std::printf("noise floor: n/a, %zu run\n\n", m.medians_ns.size());
        return;
    }

    std::printf("noise floor: %.2f%% over %zu runs (%s:",
                m.noise_pct, m.medians_ns.size(), time_units ? "medians us" : "elapsed s");

    for (double v : m.medians_ns)
        std::printf(" %.3f", time_units ? v / 1000.0 : v / 1e9);

    std::printf(")\n\n");
}

// ------------------------------------------------------------------ JSON

int write_json(const char* path, const bench_parsed_args& args, const MultiRunResult& m, uint64_t clock_floor_ns)
{
    FILE* f = std::fopen(path, "w");
    if (f == nullptr)
    {
        std::fprintf(stderr, "write_json: cannot open %s\n", path);
        return -1;
    }

    char os[256];     read_os_pretty(os, sizeof os);
    char clksrc[64];  read_first_line("/sys/devices/system/clocksource/clocksource0/current_clocksource", clksrc, sizeof clksrc);
    char datebuf[64]; now_iso8601(datebuf, sizeof datebuf);

    utsname u{};
    uname(&u);

    int  pinned = first_affinity_cpu();
    char affinity[128]; affinity_str(affinity, sizeof affinity);
    char cstates[256];  cstate_str(pinned, cstates, sizeof cstates);

    char path_buf[256];
    char gov[64];
    std::snprintf(path_buf, sizeof path_buf, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", pinned >= 0 ? pinned : 0);
    read_first_line(path_buf, gov, sizeof gov);

    char siblings[64];
    siblings[0] = '\0';
    if (pinned >= 0)
    {
        std::snprintf(path_buf, sizeof path_buf, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", pinned);
        read_first_line(path_buf, siblings, sizeof siblings);
    }

    char smt[64];
    read_first_line("/sys/devices/system/cpu/smt/control", smt, sizeof smt);

    uint64_t interval_ns = args.rate > 0 ? 1000000000ull / args.rate : 0;

    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"conditions\": {\n");
    std::fprintf(f, "    \"date\": \"%s\",\n", datebuf);
    std::fprintf(f, "    \"os\": \"%s\",\n", os);
    std::fprintf(f, "    \"kernel\": \"%s\",\n", u.release);
    std::fprintf(f, "    \"cpus\": %ld,\n", sysconf(_SC_NPROCESSORS_ONLN));
    std::fprintf(f, "    \"affinity\": \"%s\",\n", affinity[0] ? affinity : "unknown");
    std::fprintf(f, "    \"pinned_cpu\": %d,\n", pinned);
    std::fprintf(f, "    \"smt\": \"%s\",\n", smt[0] ? smt : "unknown");
    std::fprintf(f, "    \"thread_siblings\": \"%s\",\n", siblings[0] ? siblings : "unknown");
    std::fprintf(f, "    \"governor\": \"%s\",\n", gov[0] ? gov : "unknown");
    std::fprintf(f, "    \"cstates\": \"%s\",\n", cstates);
    std::fprintf(f, "    \"clocksource\": \"%s\",\n", clksrc);
    std::fprintf(f, "    \"clock_floor_ns\": %" PRIu64 ",\n", clock_floor_ns);
    std::fprintf(f, "    \"device\": \"%s\"\n", args.device_name ? args.device_name : "");
    std::fprintf(f, "  },\n");

    std::fprintf(f, "  \"config\": {\n");
    std::fprintf(f, "    \"mode\": \"%s\",\n", mode_str(args.mode));
    std::fprintf(f, "    \"message_size\": %" PRIu64 ",\n", args.message_size);
    std::fprintf(f, "    \"iterations\": %" PRIu64 ",\n", args.iterations);
    std::fprintf(f, "    \"warmup\": %" PRIu64 ",\n", args.warmup);
    std::fprintf(f, "    \"runs\": %" PRIu64 ",\n", args.runs);
    std::fprintf(f, "    \"rate\": %" PRIu64 ",\n", args.rate);
    std::fprintf(f, "    \"interval_ns\": %" PRIu64 ",\n", interval_ns);
    std::fprintf(f, "    \"inline\": %s,\n", args.inline_data ? "true" : "false");
    std::fprintf(f, "    \"signal_every\": %" PRIu64 ",\n", args.signal_every);
    std::fprintf(f, "    \"pipeline\": %" PRIu64 ",\n", args.pipeline);
    std::fprintf(f, "    \"reap\": \"%s\"\n", args.reap == reap_mode::POLL ? "poll" : "event");
    std::fprintf(f, "  },\n");

    std::fprintf(f, "  \"noise_pct\": %.4f,\n", m.noise_pct);

    std::fprintf(f, "  \"medians_ns\": [");
    for (size_t i = 0; i < m.medians_ns.size(); ++i)
        std::fprintf(f, "%s%.1f", i > 0 ? ", " : "", m.medians_ns[i]);
    std::fprintf(f, "],\n");

    std::fprintf(f, "  \"runs\": [\n");
    for (size_t i = 0; i < m.runs.size(); ++i)
    {
        const RunResult& r = m.runs[i];

        std::vector<uint64_t> late = r.late_ns;
        Stats ls{};
        uint64_t missed = 0;
        if (!late.empty())
        {
            ls = compute(late);
            if (interval_ns > 0)
                for (uint64_t v : late)
                    if (v > interval_ns) missed++;
        }

        std::fprintf(f, "    {\n");
        std::fprintf(f, "      \"ops\": %" PRIu64 ",\n",              r.ops_recorded);
        std::fprintf(f, "      \"bytes\": %" PRIu64 ",\n",            r.bytes);
        std::fprintf(f, "      \"elapsed_ns\": %" PRIu64 ",\n",       r.elapsed_ns);
        std::fprintf(f, "      \"late_count\": %" PRIu64 ",\n",       r.behind);
        std::fprintf(f, "      \"late_p50_ns\": %.1f,\n",             ls.p50);
        std::fprintf(f, "      \"late_p99_ns\": %.1f,\n",             ls.p99);
        std::fprintf(f, "      \"late_max_ns\": %" PRIu64 ",\n",      r.max_late_ns);
        std::fprintf(f, "      \"missed_slots\": %" PRIu64 ",\n",     missed);
        std::fprintf(f, "      \"max_batch\": %" PRIu64 ",\n",        r.max_batch);
        std::fprintf(f, "      \"batches\": %" PRIu64 ",\n",          r.batches);
        std::fprintf(f, "      \"batched_recvs\": %" PRIu64 ",\n",    r.batched_recvs);
        std::fprintf(f, "      \"granted_inline\": %u,\n",            r.granted_inline);
        std::fprintf(f, "      \"eff_pipeline\": %" PRIu64 ",\n",     r.eff_pipeline);
        std::fprintf(f, "      \"eff_signal_every\": %" PRIu64 ",\n", r.eff_signal_every);
        std::fprintf(f, "      \"inline_applied\": %s,\n",            r.inline_applied ? "true" : "false");
        std::fprintf(f, "      \"samples_ns\": [");
        for (size_t k = 0; k < r.samples.size(); ++k)
            std::fprintf(f, "%s%" PRIu64, k > 0 ? "," : "", r.samples[k]);
        std::fprintf(f, "]\n");
        std::fprintf(f, "    }%s\n", i + 1 < m.runs.size() ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");

    std::fclose(f);
    std::printf("json: wrote %s\n", path);
    return 0;
}
// ------------------------------------------------------------------ R6

static RunConfig make_run_config(const bench_parsed_args& a, const limen::SessionConfig& cfg)
{
    RunConfig rc{};
    rc.message_size          = a.message_size;
    rc.eff_signal_every      = a.signal_every;
    rc.send_slots            = cfg.send_slots;
    rc.inline_ok             = a.inline_data;
    rc.wc_reap_mode          = a.reap;
    rc.rnr_retry             = 7;
    rc.unsignaled            = false;
    rc.broken_arming_enabled = a.broken_arming;
    rc.verify_payload        = false;
    rc.fenced                = false;
    rc.max_outstanding       = 0;
    return rc;
}

//  runs one cell to completion. returns rc != 0 on any failure.
static RunResult run_cell(const bench_parsed_args& a, const char* peer)
{
    RunResult r{};
    BenchPlan plan{};
    resolve_plan(plan, a);
    plan.depth = a.pipeline;

    limen::SessionConfig cfg{};
    try { cfg = build_session_config(a, plan); }
    catch (const limen::SessionError& e)
    {
        std::fprintf(stderr, "  cell setup: %s\n", e.what());
        r.rc = EXIT_USAGE_ERROR;
        return r;
    }

    RunConfig rc = make_run_config(a, cfg);

    try { r = run_once(cfg, rc, plan, peer); }
    catch (const limen::SessionError& e)
    {
        std::fprintf(stderr, "  cell run: %s\n", e.what());
        r.rc = EXIT_CONNECTION_MANAGER_FAILURE;
    }
    return r;
}

struct CellResult {
    bool     ok{false};
    double   med_us{0.0};
    double   p99_us{0.0};
    double   noise_pct{0.0};
    uint64_t runs_ok{0};
    uint64_t eff_pipeline{0};
    uint64_t eff_signal_every{0};
};

//  runs one configuration `runs` times. the median and p99 come from every
//  sample merged; the noise floor comes from the spread of the per-run medians,
//  measured in this exact configuration rather than borrowed from another.
static CellResult run_cell_repeated(const bench_parsed_args& a, const char* peer, uint64_t runs)
{
    CellResult out{};
    std::vector<double>   medians;
    std::vector<uint64_t> merged;

    for (uint64_t i = 0; i < runs; ++i)
    {
        bench_parsed_args one = a;
        one.runs = 1;

        RunResult r = run_cell(one, peer);
        if (r.rc != 0 || r.samples.empty()) continue;

        std::vector<uint64_t> s = r.samples;
        medians.push_back(compute(s).p50);
        merged.insert(merged.end(), r.samples.begin(), r.samples.end());

        out.eff_pipeline     = r.eff_pipeline;
        out.eff_signal_every = r.eff_signal_every;
    }

    if (medians.empty()) return out;

    Stats st   = compute(merged);
    out.med_us = st.p50 / 1000.0;
    out.p99_us = st.p99 / 1000.0;

    double mean = std::accumulate(medians.begin(), medians.end(), 0.0) / medians.size();
    auto [lo, hi] = std::minmax_element(medians.begin(), medians.end());

    out.noise_pct = (medians.size() > 1 && mean > 0.0) ? (*hi - *lo) / mean * 100.0 : 0.0;
    out.runs_ok   = medians.size();
    out.ok        = true;
    return out;
}

void sweep_sizes(const bench_parsed_args& args, const char* peer, double noise_pct)
{
    uint64_t bw_depth = args.pipeline > 1 ? args.pipeline : 64;

    std::printf("size sweep, latency at depth 1 and bandwidth at depth %" PRIu64
                " (latency noise floor %.2f%%)\n", bw_depth, noise_pct);
    std::printf("%9s %8s %9s %9s %9s %9s %9s %11s %9s\n",
                "size(B)", "iters", "bw_iters", "med(us)", "p90(us)", "p99(us)", "p99.9(us)",
                "Gbit/s", "msg/s");

    for (uint64_t size = 64; size <= 1048576; size <<= 1)
    {
        uint64_t iters = std::clamp<uint64_t>((64ull * 1024 * 1024) / size, 200, 100000);

        bench_parsed_args lat = args;
        lat.message_size = size;
        lat.iterations   = iters;
        lat.warmup       = std::min<uint64_t>(args.warmup, iters / 4);
        lat.mode         = bench_mode::LATENCY;
        lat.pipeline     = 1;
        lat.runs         = 1;

        RunResult rl = run_cell(lat, peer);
        if (rl.rc != 0 || rl.samples.empty()) continue;

        //  bandwidth needs enough ops to fill and drain the pipeline many times
        //  over. the latency count bottoms out at 200, which is only three
        //  pipeline fills at depth 64 and produces unrepeatable figures.
        uint64_t bw_iters = std::clamp<uint64_t>((64ull * 1024 * 1024) / size, 2000, 100000);

        bench_parsed_args bw = lat;
        bw.mode       = bench_mode::BANDWIDTH;
        bw.pipeline   = bw_depth;
        bw.iterations = bw_iters;
        bw.warmup     = std::min<uint64_t>(args.warmup, bw_iters / 4);

        RunResult rb = run_cell(bw, peer);

        Stats st = compute(rl.samples);

        double sec    = rb.elapsed_ns / 1e9;
        double gbit_s = (rb.rc == 0 && sec > 0) ? (double)rb.bytes * 8.0 / sec / 1e9 : 0.0;
        double msg_s  = (rb.rc == 0 && sec > 0) ? (double)rb.ops_recorded / sec      : 0.0;

        std::printf("%9" PRIu64 " %8" PRIu64 " %9" PRIu64 " %9.2f %9.2f %9.2f %9.2f %11.3f %9.0f\n",
                    size, iters, bw_iters,
                    st.p50 / 1000.0, st.p90 / 1000.0, st.p99 / 1000.0, st.p999 / 1000.0,
                    gbit_s, msg_s);
        std::fflush(stdout);
    }
    std::printf("\n");
}

static const char* verdict(double delta_pct, double noise_pct)
{
    if (std::fabs(delta_pct) <= noise_pct) return "within noise";
    return delta_pct < 0 ? "SIGNIFICANT" : "SIGNIFICANT (worse)";
}

void sweep_options(const bench_parsed_args& args, const char* peer)
{
    struct Cell {
        const char* label;
        bool        inline_data;
        uint64_t    signal_every;
        uint64_t    pipeline;
        reap_mode   reap;
    };

    const uint64_t BASE_DEPTH = 16;
    const uint64_t runs       = args.runs > 0 ? args.runs : 1;

    const Cell cells[] = {
        { "baseline",        false, 1,  BASE_DEPTH, reap_mode::POLL  },
        { "inline=on",       true,  1,  BASE_DEPTH, reap_mode::POLL  },
        { "signal_every=16", false, 16, BASE_DEPTH, reap_mode::POLL  },
        { "pipeline=1",      false, 1,  1,          reap_mode::POLL  },
        { "reap=event",      false, 1,  BASE_DEPTH, reap_mode::EVENT },
    };

    auto configure = [&](const Cell& c) {
        bench_parsed_args a = args;
        a.mode          = bench_mode::LATENCY;
        a.inline_data   = c.inline_data;
        a.signal_every  = c.signal_every;
        a.pipeline      = c.pipeline;
        a.reap          = c.reap;
        a.broken_arming = false;
        return a;
    };

    //  the floor is measured in the baseline configuration itself. a floor taken
    //  at depth 1 says nothing about the repeatability of a depth-16 median, and
    //  using one to gate the other makes every verdict below meaningless.
    CellResult base = run_cell_repeated(configure(cells[0]), peer, runs);
    if (!base.ok)
    {
        std::printf("option sweep at size=%" PRIu64 ": baseline failed, no verdicts\n\n",
                    args.message_size);
        return;
    }

    std::printf("option sweep at size=%" PRIu64 ", baseline depth %" PRIu64 "\n",
                args.message_size, BASE_DEPTH);
    std::printf("floor %.2f%% over %" PRIu64 " runs, measured in the baseline configuration\n",
                base.noise_pct, base.runs_ok);
    std::printf("%-18s %9s %10s %9s %9s %9s %9s  %s\n",
                "option", "med(us)", "delta", "p99(us)", "noise", "eff_pipe", "eff_sig", "verdict");

    std::printf("%-18s %9.2f %10s %9.2f %8.2f%% %9" PRIu64 " %9" PRIu64 "  %s\n",
                cells[0].label, base.med_us, "-", base.p99_us, base.noise_pct,
                base.eff_pipeline, base.eff_signal_every, "-");
    std::fflush(stdout);

    for (size_t i = 1; i < sizeof(cells) / sizeof(cells[0]); ++i)
    {
        const Cell& c = cells[i];

        CellResult r = run_cell_repeated(configure(c), peer, runs);
        if (!r.ok)
        {
            std::printf("%-18s %9s %10s %9s %9s %9s %9s  %s\n",
                        c.label, "-", "-", "-", "-", "-", "-", "failed");
            continue;
        }

        double delta_pct = (r.med_us - base.med_us) / base.med_us * 100.0;

        //  a delta is only real if it clears the spread of both configurations
        double gate = std::max(base.noise_pct, r.noise_pct);

        char deltabuf[16];
        std::snprintf(deltabuf, sizeof deltabuf, "%+.1f%%", delta_pct);

        std::printf("%-18s %9.2f %10s %9.2f %8.2f%% %9" PRIu64 " %9" PRIu64 "  %s\n",
                    c.label, r.med_us, deltabuf, r.p99_us, r.noise_pct,
                    r.eff_pipeline, r.eff_signal_every, verdict(delta_pct, gate));
        std::fflush(stdout);
    }

    std::printf("%-18s %9s %10s %9s %9s %9s %9s  %s\n",
                "op=write", "-", "-", "-", "-", "-", "-",
                "not implemented, post_one emits IBV_WR_SEND only");
    std::printf("\nnote: pipeline=1 also takes the closed-loop branch in run_once, so depth\n");
    std::printf("      and code path are confounded in that row. its delta is queueing\n");
    std::printf("      delay from 16 in flight, not a property of the send path.\n\n");
}




// ------------------------------------------------------------------ main

int main(int argc, char* argv[])
{
    try
    {
        bench_parsed_args args{};
        parse_argv(argc, argv, &args);

        if (args.device_name == nullptr)
        {
            std::fprintf(stderr, "-d <device> is required\n");
            return EXIT_USAGE_ERROR;
        }

        uint64_t clock_floor_ns = measure_clock_floor();

        BenchPlan plan{};
        resolve_plan(plan, args);

        limen::SessionConfig cfg = build_session_config(args, plan);
        RunConfig rc = make_run_config(args, cfg);

        if (args.report_config)
        {
            print_conditions(args, clock_floor_ns);
            return EXIT_SUCCESS;
        }

        if (args.peer == nullptr)
        {
            std::printf("role: server\n");
            std::fflush(stdout);
            return serve_forever(cfg, rc);
        }

        std::printf("role: client\n");
        if (args.clock_floor) print_clock_floor(clock_floor_ns, 2500);
        print_conditions(args, clock_floor_ns);

        if (args.mode == bench_mode::SWEEP)
        {
            //  this floor gates the size sweep only. the option sweep measures
            //  its own, in the configuration it actually compares against.
            bench_parsed_args base = args;
            base.mode     = bench_mode::LATENCY;
            base.pipeline = 1;

            BenchPlan base_plan{};
            resolve_plan(base_plan, base);

            limen::SessionConfig base_cfg = build_session_config(base, base_plan);
            RunConfig base_rc = make_run_config(base, base_cfg);

            MultiRunResult m = run_repeated(base_cfg, base_rc, base_plan, args.peer, args.runs);
            if (m.runs.empty())
            {
                std::fprintf(stderr, "no baseline runs completed\n");
                return EXIT_VERB_ERROR;
            }

            print_noise_floor(m, true);
            if (args.json_path) write_json(args.json_path, args, m, clock_floor_ns);

            sweep_sizes(args, args.peer, m.noise_pct);
            sweep_options(args, args.peer);
            return EXIT_SUCCESS;
        }

        MultiRunResult m = run_repeated(cfg, rc, plan, args.peer, args.runs);
        if (m.runs.empty())
        {
            std::fprintf(stderr, "no runs completed\n");
            return EXIT_VERB_ERROR;
        }

        print_noise_floor(m, plan.collect_samples);

        if (plan.collect_samples)
        {
            std::vector<uint64_t> s = m.runs.back().samples;
            Stats st = compute(s);
            print_stats(st, plan.scheduled ? "response_time" : "service_time");
            if (plan.scheduled) print_schedule(m.runs.back(), plan.interval_ns);
        }
        else
        {
            print_bandwidth(m.runs.back(), plan.message_size);
        }

        if (args.json_path) write_json(args.json_path, args, m, clock_floor_ns);

        return EXIT_SUCCESS;
    }
    catch (const limen::SessionError& e) { std::fprintf(stderr, "%s\n", e.what()); return EXIT_CONNECTION_MANAGER_FAILURE; }
    catch (const limen::VerbsError& e)   { std::fprintf(stderr, "%s\n", e.what()); return EXIT_VERB_ERROR; }
    catch (const std::exception& e)      { std::fprintf(stderr, "unhandled: %s\n", e.what()); return EXIT_VERB_ERROR; }
}