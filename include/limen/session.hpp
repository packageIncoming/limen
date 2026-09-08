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
    uint16_t tcp_port            = 18515;   // CM listen/connect port, not an RDMA port

    //  receive region: recv_wr is both the queue depth and the slot count,
    //  because a receive consumes one of each. region size is derived.
    uint32_t recv_wr             = 1;       // recv queue depth 
    uint32_t recv_slots          = 0;       // recv slots pre-posted; 0 for pure one-sided
    uint32_t recv_slot_size      = 0;       // bytes per receive slot
    //  registered length is recv_wr * recv_slot_size, and this is the region
    //  whose addr/rkey/length go out in the private data: the peer writes here

    //  send region: queue depth and slot count are independent. a WR is a
    //  descriptor in the adapter, a slot is payload in your memory. inline
    //  sends take a WR and no slot; reads take a WR and name a destination slot.
    uint32_t send_wr             = 1;       // send queue depth, clamped to max_qp_wr
    uint32_t send_slots          = 1;       // ring depth for buffer reuse (TRD-07 R4)
    uint32_t send_slot_size      = 0;       // bytes per send slot
    //  registered length is send_slots * send_slot_size, local only, never exposed

    //  MR permissions, split by region: minimum privilege per TRD-01 R7
    int recv_access_flags        = IBV_ACCESS_LOCAL_WRITE
                                 | IBV_ACCESS_REMOTE_WRITE
                                 | IBV_ACCESS_REMOTE_READ;
    int send_access_flags        = IBV_ACCESS_LOCAL_WRITE;

    int      cqe                 = 0;       // completion queue capacity, shared send+recv; 0 = device max
    uint8_t  initiator_depth     = 1;       // outbound RDMA READs you will have outstanding
    uint8_t  responder_resources = 1;       // inbound RDMA READs you will service
    uint8_t  retry_count         = 7;       // transport retries on timeout or NAK
    uint8_t  rnr_retry_count     = 7;       // RNR retries; configures the PEER's QP, set it on the receive-queue owner
    int      timeout_ms          = 5000;    // per CM step; -1 blocks
};

struct GrantedCaps {
    uint32_t max_send_wr = 0;
    uint32_t max_recv_wr = 0;
    uint32_t max_inline_data = 0;
    uint8_t  initiator_depth = 0;      // negotiated at ESTABLISHED, not at create
    uint8_t  responder_resources = 0;
};

class Session {
friend class PendingConnection;
public:

    Session() noexcept = default;

    ~Session() noexcept {close();}
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) noexcept        = default;
    Session& operator=(Session&&) noexcept;

    //  create a session (client or server mode)
    static Session create_client_session(const char* peer, const SessionConfig& config);
    static Session create_server_session(const SessionConfig& config);
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
    bool has_peer() {return _has_peer;}
    bool is_client() {return _is_client;}
    
    uint8_t negotiated_initiator_depth()     const noexcept {return _caps.initiator_depth;}
    uint8_t negotiated_responder_resources() const noexcept {return _caps.responder_resources;}
    uint32_t max_send_wr() const noexcept {return _caps.max_send_wr;}
    uint32_t max_recv_wr() const noexcept {return _caps.max_recv_wr;}
    uint32_t max_inline_data() const noexcept {return _caps.max_inline_data;}

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

    bool          _has_peer = false;
    bool          _is_client = false;

    SessionConfig _init_config; //  the requested resources
    GrantedCaps   _caps{};  //  the granted resources (maxes) 
};




// A connection that owns every resource a Session owns and has exchanged
// no private data. The CM forces resource creation between the point where
// id->verbs becomes valid and the connect/accept call, so that window is a
// distinct state with a distinct set of legal operations.
//
// Client: constructed after ROUTE_RESOLVED, before rdma_connect.
// Server: constructed after CONNECT_REQUEST (id adopted, peer ConnInfo
//         already snapshotted from the private data), before rdma_accept.
class PendingConnection {
public:
    PendingConnection() noexcept = default;
    ~PendingConnection() noexcept {close();}
    PendingConnection(const PendingConnection&)            = delete;
    PendingConnection& operator=(const PendingConnection&) = delete;
    PendingConnection(PendingConnection&&) noexcept;
    PendingConnection& operator=(PendingConnection&&) noexcept;

    // rdma_resolve_addr + rdma_resolve_route, then PD, MRs, CQ, QP, recv posts.
    static PendingConnection resolve(const char* peer, const SessionConfig&);

    // rdma_bind_addr + rdma_listen, blocks for CONNECT_REQUEST, adopts the
    // new id, snapshots peer ConnInfo, then PD, MRs, CQ, QP, recv posts.
    static PendingConnection listen(const SessionConfig&);

    // Pre-exchange access. Writes here are ordered before the peer can reach
    // the region, because the peer cannot reach ESTABLISHED until finish().
    ibv_mr*      recv_mr() const noexcept { return _recv_mr.get();}
    ibv_mr*      send_mr() const noexcept {return _send_mr.get();}
    ibv_qp*      qp()      const noexcept {return _id.qp();}
    ibv_cq*      cq()      const noexcept {return _cq.get();}
    ibv_context* verbs()   const noexcept {return _id.get()->verbs;}

    // Server only: arrived with CONNECT_REQUEST, host byte order.
    // Client: not yet known, has_peer() is false until finish().
    ConnInfo peer()     const noexcept {return _peer;};
    bool     has_peer() const noexcept {return _has_peer;};
    bool     is_client() const noexcept {return _is_client;};

    uint32_t max_send_wr() const noexcept {return _caps.max_send_wr;}
    uint32_t max_recv_wr() const noexcept {return _caps.max_recv_wr;}
    uint32_t max_inline_data() const noexcept {return _caps.max_inline_data;}

    // rdma_connect or rdma_accept with this side's ConnInfo as private data,
    // wait for ESTABLISHED, capture the negotiated initiator_depth and
    // responder_resources, move every resource into the returned Session.
    // Consumes *this; the moved-from object is safe to destroy.
    // Throws SessionError; on throw *this is left destructible but unusable.
    Session finish() &&;

    int close() noexcept;

private:
    EventChannel    _ec;
    ConnectionId     _listen_id;   // server only; must outlive rdma_accept
    ConnectionId    _id;
    ProtectionDomain _pd;
    MemoryRegion    _recv_mr;
    MemoryRegion    _send_mr;
    CompletionQueue _cq;
    ConnInfo        _peer{};
    SessionConfig   _config{};
    GrantedCaps     _caps{};    //  holds the granted resource caps (like max_send_wr, max_recv_wr)
    bool            _is_client = false;
    bool            _has_peer  = false;
};

// Shared, no args-struct coupling.
void fill_qp_init_attr(ibv_qp_init_attr*, uint32_t send_wr, uint32_t recv_wr);


static_assert(std::is_nothrow_move_constructible_v<Session>);
static_assert(std::is_nothrow_move_assignable_v<Session>);
} // namespace limen