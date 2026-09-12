#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <getopt.h>
#include <vector>
#include "limen/app/harness.hpp"

enum class bench_mode {
    LATENCY,
    RESPONSE,
    BANDWIDTH,
    SWEEP
};

enum class bench_op {
    SEND,
    WRITE,
    READ
};

enum {
    OPT_MODE = 256,
    OPT_WARMUP,
    OPT_RUNS,
    OPT_RATE,
    OPT_OP,
    OPT_JSON,
    OPT_CLOCK_FLOOR,

    // TRD-07
    OPT_INLINE,
    OPT_SIGNAL_EVERY,
    OPT_PIPELINE,
    OPT_REAP,
    OPT_MODERATE,
    OPT_REPORT_CONFIG,
    OPT_BROKEN_ARMING,
};

typedef struct bench_parsed_args {
    const char* device_name{nullptr};
    uint64_t tcp_port{18515};

    bench_mode mode{bench_mode::LATENCY};
    uint64_t message_size{4096};
    uint64_t iterations{100};
    uint64_t warmup{10};
    uint64_t runs{1};
    uint64_t rate{0};                    // 0 = unlimited
    bench_op op{bench_op::SEND};

    //  TRD-07
    bool      inline_data{false};        // --inline        ('inline' is a keyword)
    uint64_t  signal_every{1};           // --signal-every n, 1 = signal every send
    uint64_t  pipeline{1};               // --pipeline depth
    reap_mode reap{reap_mode::POLL};     // --reap poll|event
    bool      moderate{false};           // --moderate given at all
    uint64_t  moderate_count{0};         //   c of c:u
    uint64_t  moderate_usec{0};          //   u of c:u
    bool      report_config{false};      // --report-config, print and exit 0
    bool      broken_arming{false};      // --broken-arming, hidden, R8

    const char* json_path{nullptr};
    bool clock_floor{false};

    const char* peer{nullptr};
} bench_parsed_args;

struct BenchPlan {
    bench_mode mode{bench_mode::LATENCY};
    uint64_t   record{0};
    uint64_t   warmup{0};
    uint64_t   rate{0};
    uint64_t   message_size{0};

    bool       moderate{false};
    uint64_t   moderate_count{0};
    uint64_t   moderate_usec{0};

    //  filled by resolve_plan(), the loop reads only these four
    uint64_t   depth{1};
    uint64_t   interval_ns{0};
    bool       scheduled{false};
    bool       collect_samples{true};
};

struct Stats {
    double min{0}, p50{0}, p90{0}, p99{0}, p999{0}, max{0}, mean{0}, stddev{0};
};

struct RunResult {
    std::vector<uint64_t> samples;
    uint64_t ops_recorded{0};
    uint64_t bytes{0};
    uint64_t elapsed_ns{0};

    uint64_t behind{0};
    uint64_t max_late_ns{0};

    uint64_t max_batch{0};
    uint64_t batches{0};
    uint64_t batched_recvs{0};

    uint32_t granted_inline{0};
    uint64_t eff_pipeline{0};
    uint64_t eff_signal_every{0};
    bool     inline_applied{false};

    int      rc{0};
};

struct MultiRunResult {
    std::vector<RunResult> runs;
    std::vector<double>    medians_ns;
    double                 noise_pct{0.0};
};

inline uint64_t time_ns()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void parse_argv(int argc, char* argv[], bench_parsed_args* args);
void print_help(bool to_stderr);
int  parse_moderate(const char* s, uint64_t* count, uint64_t* usec);

uint64_t measure_clock_floor();

void resolve_plan(BenchPlan& plan, const bench_parsed_args& args);
limen::SessionConfig build_session_config(const bench_parsed_args& args, const BenchPlan& plan);

RunResult      run_once(const limen::SessionConfig& cfg, RunConfig& rc, const BenchPlan& plan, const char* peer);
MultiRunResult run_repeated(const limen::SessionConfig& cfg, RunConfig& rc, const BenchPlan& plan, const char* peer, uint64_t runs);
int            serve_forever(const limen::SessionConfig& cfg, RunConfig& rc);

Stats  compute(std::vector<uint64_t>& samples);
double median_ns(std::vector<uint64_t>& samples);

void print_clock_floor(uint64_t floor_ns, uint64_t smallest_expected_ns);
void print_conditions(const bench_parsed_args& args, uint64_t clock_floor_ns);
void print_stats(const Stats& st, const char* quantity);
void print_bandwidth(const RunResult& r, uint64_t message_size);
void print_schedule(const RunResult& r);
void print_noise_floor(const MultiRunResult& m, bool time_units);

int write_json(const char* path, const bench_parsed_args& args, const MultiRunResult& m, uint64_t clock_floor_ns);

void sweep_sizes(const bench_parsed_args& args, const char* peer, double noise_pct);
void sweep_options(const bench_parsed_args& args, const char* peer, double noise_pct);