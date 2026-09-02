#pragma once
#include "limen/cm.hpp"
#include <cstdint>

namespace limen {

struct SessionConfig {
    uint16_t tcp_port            = 0;
    uint8_t  initiator_depth     = 1;    // requested outstanding RDMA READs
    uint8_t  responder_resources = 1;
    uint8_t  retry_count         = 7;
    uint8_t  rnr_retry_count     = 7;
    int      timeout_ms          = 5000; // per CM step; -1 blocks
};

class Session {
public:
    Session() noexcept = default;
    ~Session() noexcept = default;
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;

    ibv_qp*     qp()    const noexcept {return _id.qp(); }
    rdma_cm_id* id()    const noexcept {return _id.get();}
    ConnInfo    peer()  const noexcept {return _peer;};   // host byte order, validated non-zero

    uint8_t negotiated_initiator_depth()     const noexcept {return _init_depth;}
    uint8_t negotiated_responder_resources() const noexcept {return _resp_res;}

    int  disconnect() noexcept;                    // rdma_disconnect
    void wait_for_disconnect(int timeout_ms);      // DISCONNECTED or TIMEWAIT_EXIT

private:
    EventChannel* _ec = nullptr;
    ConnectionId  _id;
    ConnInfo      _peer{};
    uint8_t       _init_depth = 0;
    uint8_t       _resp_res   = 0;
};

// Shared, no args-struct coupling.
void fill_qp_init_attr(ibv_qp_init_attr*, const ibv_device_attr*, uint32_t send_wr, uint32_t recv_wr);

} // namespace limen