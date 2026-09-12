#pragma once
#include <climits>
#include <infiniband/verbs.h>
#include "limen/app/harness.hpp"





typedef struct pingpong_parsed_args {
    const char* device_name{nullptr};
    int gid_index{INT_MAX};
    int port{1};
    uint64_t tcp_port{18515};
    uint64_t message_size{4096};
    uint64_t iterations{100};
    uint64_t rx_depth{8};
    int rnr_retry{7};
    bool no_recv{false};
    bool unsignaled{false};

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

    const char* addr{nullptr};
} pingpong_parsed_args;


void parse_argv(int arg, char* argv[], pingpong_parsed_args* args_container);

void print_help(bool to_error=false);