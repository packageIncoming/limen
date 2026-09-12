#pragma once
#include "limen/session.hpp"
#include <cstdint>
#include <infiniband/verbs.h>

#define SEND_QUEUE_DEPTH 16
#define RECV_QUEUE_DEPTH 16
#define COMPLETE_QUEUE_DEPTH (SEND_QUEUE_DEPTH + RECV_QUEUE_DEPTH)
#define U32_TO_U24_MASK (0x00FFFFFF)

constexpr uint64_t RECV_WRID_TAG  = 0x1ULL << 63;
constexpr uint64_t SEND_WRID_TAG  = 0x1ULL << 62;
constexpr uint64_t SEND_MR_BYTE_CAP = 64ull << 20;   // 64 MiB of registered send memory


enum class reap_mode { POLL, EVENT };


struct RunConfig {           
    uint64_t iterations;
    uint64_t message_size;
    uint64_t eff_pipeline;
    uint64_t eff_signal_every;
    uint64_t send_slots;
    uint32_t granted_send_wr;

    bool     inline_ok;
    bool     is_client;
    bool unsignaled;
    bool broken_arming_enabled;

    bool verify_payload;
    bool fenced;

    uint64_t max_outstanding{0};

    reap_mode wc_reap_mode;
    int rnr_retry;
    uint64_t poll_timeout_ms{5000};
    uint64_t response_size{0};

};

struct RunState {
    uint64_t posted{0};            
    uint64_t covered{0};            
    uint64_t recv_count{0};
    uint64_t send_completions{0};
    uint64_t mismatches{0};

    uint64_t responses_owed{0};

    uint64_t events_received{0};    
    uint64_t events_acked{0};       
    uint64_t empty_events{0};    
    uint64_t race_polls_hit{0};

    bool timeout{false};

    ibv_wc_status first_error_status{IBV_WC_SUCCESS};
};


int post_send(
    bool signaled,
    uint32_t seq,
    uint32_t num_slots,
    uint64_t buff_addr, 
    ibv_qp* queue_pair, 
    uint32_t message_size,
    uint32_t lkey,
    bool inline_enabled
);

bool can_post(RunConfig &run_config, RunState &state);

int post_one(limen::Session &session, RunConfig &run_config, RunState &state);

int handle_wc(ibv_wc& wc, limen::Session &session, RunConfig &run_config, RunState &state);

int drain(limen::Session &session, RunConfig &run_config, RunState &state);

int reap_event(limen::Session &session, RunConfig &run_config, RunState &state);

int reap_poll(limen::Session &session, RunConfig &run_config, RunState &state);

int reap(limen::Session &session, RunConfig &run_config, RunState &state);

