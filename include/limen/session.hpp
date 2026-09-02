#pragma once
#include "limen/cm.hpp"
#include "limen/verbs.hpp"
#include <cstdint>
#include <infiniband/verbs.h>

namespace limen {

constexpr uint64_t RECV_WRID_TAG  = 0x1ULL << 63;
constexpr uint64_t SEND_WRID_TAG  = 0x1ULL << 62;

class SessionError : public std::runtime_error {
public:
    SessionError(const char *op, int err);
    int error() const noexcept { return _err; }
private:
    int _err;
};

struct SessionConfig {
    uint16_t tcp_port            = 18515;
    uint32_t recv_size           = 0x1000;    // region size in bytes
    uint32_t recv_slot_size = 0;   // bytes per receive slot; recv_wr * this must be <= recv_size
    uint32_t send_size           = 0x1000;    // region size in bytes
    int      access_flags        = IBV_ACCESS_LOCAL_WRITE
                                 | IBV_ACCESS_REMOTE_WRITE
                                 | IBV_ACCESS_REMOTE_READ;
    uint32_t send_wr             = 1;
    uint32_t recv_wr             = 1;
    int      cqe                 = 0;    // 0 = clamp to device max
    uint8_t  initiator_depth     = 1;
    uint8_t  responder_resources = 1;
    uint8_t  retry_count         = 7;
    uint8_t  rnr_retry_count     = 7;
    int      timeout_ms          = 5000;
};

class Session {
public:
    Session() noexcept = default;

    ~Session() noexcept {close();}
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) noexcept        = default;
    Session& operator=(Session&&) noexcept;

    //  create a session (client or server mode)
    static Session create_client_session(const char* , const SessionConfig&);
    static Session create_server_session(const SessionConfig&);
    //  close all the wrappers, in correct order
    int close() noexcept;

    //  getters for raw pointers
    ibv_qp*     qp()    const noexcept {return _id.qp(); }
    rdma_cm_id* id()    const noexcept {return _id.get();}
    ibv_mr*     send_mr() const noexcept {return _send_mr.get();}
    ibv_mr*      recv_mr() const noexcept {return _recv_mr.get();}
    ibv_pd*     pd() const noexcept {return _pd.get();}
    ibv_cq*     cq() const noexcept{return _cq.get();}
    rdma_event_channel* ec() const noexcept {return _ec.get();}

    //  replenish recvs
    int      repost_recv(uint32_t slot) noexcept;   // returns ibv_post_recv rc
    static uint32_t slot_of(uint64_t wr_id) noexcept;
    static bool     is_recv_wrid(uint64_t wr_id) noexcept;


    //  getters for private data members
    ConnInfo peer()  const noexcept {return _peer;};   // host byte order, validated non-zero
    uint8_t negotiated_initiator_depth()     const noexcept {return _init_depth;}
    uint8_t negotiated_responder_resources() const noexcept {return _resp_res;}
    bool has_peer() {return _has_peer;}
    bool is_client() {return _is_client;}
    SessionConfig* config_ptr() noexcept{return &_init_config;}

    int  disconnect() noexcept;                    // rdma_disconnect
    void wait_for_disconnect(int timeout_ms);      // DISCONNECTED or TIMEWAIT_EXIT

private:
    EventChannel _ec;
    ConnectionId  _id;
    ConnInfo      _peer{};
    ProtectionDomain _pd;
    MemoryRegion _recv_mr;
    MemoryRegion _send_mr;
    CompletionQueue _cq;

    uint8_t       _init_depth = 0;
    uint8_t       _resp_res   = 0;
    bool          _has_peer = false;
    bool          _is_client = false;
    SessionConfig _init_config;

};

// Shared, no args-struct coupling.
void fill_qp_init_attr(ibv_qp_init_attr*, uint32_t send_wr, uint32_t recv_wr);


static_assert(std::is_nothrow_move_constructible_v<Session>);
static_assert(std::is_nothrow_move_assignable_v<Session>);
} // namespace limen