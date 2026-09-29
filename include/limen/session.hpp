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

    //  send region: queue depth and slot count are independent. a WR is a
    //  descriptor in the adapter, a slot is payload in your memory. inline
    //  sends take a WR and no slot; reads take a WR and name a destination slot.
    uint32_t send_wr             = 1;       // send queue depth, clamped to max_qp_wr
    uint32_t send_slots          = 1;       // ring depth for buffer reuse (TRD-07 R4)
    uint32_t send_slot_size      = 0;       // bytes per send slot
    int      cqe                 = 0;       // receive CQ capacity; also holds send completions when send_cqe is 0; 0 = device max
    int      send_cqe            = 0;

    //  MR permissions, split by region: minimum privilege per TRD-01 R7
    int recv_access_flags        = IBV_ACCESS_LOCAL_WRITE
                                 | IBV_ACCESS_REMOTE_WRITE
                                 | IBV_ACCESS_REMOTE_READ;
    int send_access_flags        = IBV_ACCESS_LOCAL_WRITE;

    bool     use_comp_channel    = false;

    uint8_t  initiator_depth     = 1;       // outbound RDMA READs you will have outstanding
    uint8_t  responder_resources = 1;       // inbound RDMA READs you will service
    uint8_t  retry_count         = 7;       // transport retries on timeout or NAK
    uint8_t  rnr_retry_count     = 7;       // RNR retries; configures the PEER's QP, set it on the receive-queue owner
    uint8_t  min_rnr_timer       = 1;       // wait this side asks for in its RNR NAKs; 1 = 0.01 ms. 0 leaves the CM's value (655 ms)
    int      timeout_ms          = 5000;    // per CM step; -1 blocks
};

struct GrantedCaps {
    uint32_t max_send_wr = 0;
    uint32_t max_recv_wr = 0;
    uint32_t max_inline_data = 0;
    uint8_t  initiator_depth = 0;      // negotiated at ESTABLISHED, not at create
    uint8_t  responder_resources = 0;
};

// A bound, listening rdma_cm_id that outlives the sessions accepted through it.
//
// PendingConnection::listen() binds, listens, accepts once, and then drops the
// listening id when the PendingConnection temporary dies inside finish(). A
// server that accepts more than one connection therefore has no listener at all
// between sessions, and every reconnect races the rebind. Listener holds the
// bind for the life of the process; accept_on() takes one connection off it and
// migrates the new id to its own event channel.
class Listener {
public:
    Listener() noexcept = default;
    ~Listener() noexcept = default;
    Listener(const Listener&)            = delete;
    Listener& operator=(const Listener&) = delete;
    Listener(Listener&&) noexcept        = default;
    Listener& operator=(Listener&&) noexcept = default;

    //  rdma_bind_addr + rdma_listen. Throws SessionError.
    static Listener bind(const SessionConfig& config);

    EventChannel& ec()       noexcept { return _ec; }
    rdma_cm_id*   id() const noexcept { return _id.get(); }
    bool          valid() const noexcept { return _id.get() != nullptr; }

private:
    EventChannel _ec;
    ConnectionId _id;
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
    //  accept one connection on a listener that stays bound across sessions
    static Session accept_on(Listener& listener, const SessionConfig& config);

    //  true when a CM event (DISCONNECTED, TIMEWAIT_EXIT, ...) is already
    //  queued. Non-blocking. The data loop calls this instead of inferring
    //  peer departure from an idle timer.
    bool cm_event_pending() noexcept { return _ec.get() != nullptr && _ec.wait(0) == 0; }
    //  close all the wrappers, in correct order
    int close() noexcept;

    //  getters for raw pointers
    ibv_qp*     qp()    const noexcept {return _id.qp(); }
    rdma_cm_id* id()    const noexcept {return _id.get();}
    ibv_mr*     send_mr() const noexcept {return _send_mr.get();}
    ibv_mr*      recv_mr() const noexcept {return _recv_mr.get();}
    ibv_pd*     pd() const noexcept {return _pd.get();}
    ibv_cq*     cq() const noexcept{return _cq.get();}             //  receives (and sends, unless split)
    ibv_cq*     send_cq() const noexcept{return _send_cq.get() ? _send_cq.get() : _cq.get();}
    bool        split_cq() const noexcept{return _send_cq.get() != nullptr;}
    rdma_event_channel* ec() const noexcept {return _ec.get();}

    //  the CQ's completion channel, or nullptr when use_comp_channel was false.
    //  distinct from ec(), which is the CM event channel.
    ibv_comp_channel* comp_channel() const noexcept {return _comp_channel.get();}
    int comp_channel_fd() const noexcept {return _comp_channel.fd();}
    int req_notify_cq(int solicited_only)   {return _cq.req_notify_cq(solicited_only);}
    int get_cq_event()  noexcept {_cq.inc_received_events();return _comp_channel.get_cq_event(nullptr, nullptr);}
    int ack_cq_events(int nevents) noexcept {return _cq.ack_cq_events(nevents);};

    //  replenish recvs
    int      repost_recv(uint32_t slot) noexcept;   // returns ibv_post_recv rc
    static uint32_t remove_tags(uint64_t wr_id) noexcept;
    static bool     is_recv_wrid(uint64_t wr_id) noexcept;

    //  getters for private data members
    ConnInfo peer()  const noexcept {return _peer;};   // host byte order, validated non-zero
    bool has_peer()  const noexcept {return _has_peer;}
    bool is_client() const noexcept {return _is_client;}

    //  getters for granted capacities
    uint8_t negotiated_initiator_depth()     const noexcept {return _caps.initiator_depth;}
    uint8_t negotiated_responder_resources() const noexcept {return _caps.responder_resources;}
    uint32_t max_send_wr() const noexcept {return _caps.max_send_wr;}
    uint32_t max_recv_wr() const noexcept {return _caps.max_recv_wr;}
    uint32_t max_inline_data() const noexcept {return _caps.max_inline_data;}

    SessionConfig* config_ptr() noexcept{return &_init_config;}

    //  exit/disconnect functions
    int  disconnect() noexcept;                    // rdma_disconnect
    void wait_for_disconnect(int timeout_ms);      // DISCONNECTED or TIMEWAIT_EXIT

private:
    EventChannel _ec;
    ConnectionId  _id;
    ConnInfo      _peer{};
    ProtectionDomain _pd;
    MemoryRegion _recv_mr;
    MemoryRegion _send_mr;
    //  declared before _cq to read in creation order; destruction order is
    //  hand-sequenced in close(), where the CQ must go first.
    CompletionChannel _comp_channel;
    CompletionQueue _cq;
    CompletionQueue _send_cq;      //  empty unless config.send_cqe > 0

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

    // rdma_resolve_addr + rdma_resolve_route, then PD, MRs, comp channel, CQ,
    // QP, recv posts.
    static PendingConnection resolve(const char* peer, const SessionConfig&);

    // rdma_bind_addr + rdma_listen, blocks for CONNECT_REQUEST, adopts the
    // new id, snapshots peer ConnInfo, then PD, MRs, comp channel, CQ, QP,
    // recv posts.
    static PendingConnection listen(const SessionConfig&);

    // Same as listen() from CONNECT_REQUEST onward, but takes the bind from a
    // Listener that stays alive afterwards. The accepted id is migrated to a
    // fresh event channel so the Session can own and close it independently.
    static PendingConnection accept_on(Listener& listener, const SessionConfig&);

    // Pre-exchange access. Writes here are ordered before the peer can reach
    // the region, because the peer cannot reach ESTABLISHED until finish().
    ibv_mr*      recv_mr() const noexcept { return _recv_mr.get();}
    ibv_mr*      send_mr() const noexcept {return _send_mr.get();}
    ibv_qp*      qp()      const noexcept {return _id.qp();}
    ibv_cq*      cq()      const noexcept {return _cq.get();}
    ibv_cq*      send_cq() const noexcept {return _send_cq.get() ? _send_cq.get() : _cq.get();}
    ibv_context* verbs()   const noexcept {return _id.get()->verbs;}
    ibv_comp_channel* comp_channel() const noexcept {return _comp_channel.get();}

    // Server only: arrived with CONNECT_REQUEST, host byte order.
    // Client: not yet known, has_peer() is false until finish().
    ConnInfo peer()     const noexcept {return _peer;};
    bool     has_peer() const noexcept {return _has_peer;};
    bool     is_client() const noexcept {return _is_client;};

    //  valid after the QP is created; initiator_depth and responder_resources
    //  stay zero until finish() reads them from the ESTABLISHED event.
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
    //  everything from ibv_query_device through the pre-accept recv posts.
    //  shared by listen() and accept_on(), which differ only in where the
    //  CONNECT_REQUEST came from and who owns the bind.
    static PendingConnection build_accepted(EventChannel ec,
                                            ConnectionId id,
                                            ConnInfo     peer_info,
                                            const SessionConfig& config);

    EventChannel    _ec;
    ConnectionId     _listen_id;   // server only; must outlive rdma_accept
    ConnectionId    _id;
    ProtectionDomain _pd;
    MemoryRegion    _recv_mr;
    MemoryRegion    _send_mr;
    CompletionChannel _comp_channel;   // empty unless config.use_comp_channel
    CompletionQueue _cq;
    CompletionQueue _send_cq;          // empty unless config.send_cqe > 0
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