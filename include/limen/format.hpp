#pragma once
#include <infiniband/verbs.h>
#include <string>

namespace limen {
    std::string gid_to_str(const ibv_gid* gid);
    int         str_to_gid(const std::string& s, ibv_gid* out);   // 0 ok, 1 fail
    std::string qp_state_to_str(ibv_qp_state s);

    const char* wc_status_name(ibv_wc_status s);
    const char* wc_opcode_str(ibv_wc_opcode op);
    std::string wc_to_str(const ibv_wc* wc);
}