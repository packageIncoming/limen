#include "limen/session.hpp"
#include "limen/pattern.hpp"
#include "limen/verbs.hpp"
#include <arpa/inet.h>
#include <cstdint>
#include <format>
#include <infiniband/verbs.h>
#include <utility>
#include <inttypes.h>

namespace 
{


    int post_recv(uint32_t slot, uint64_t buff_addr, ibv_qp* queue_pair,  uint32_t message_size, uint32_t lkey)
    {
        //  create the ibv_recv_wr
        ibv_recv_wr wr{};
        ibv_sge sge{};
        ibv_recv_wr* bad = nullptr;

        //  for now each entry has a single SGE
        //  slot[i] has address &(buffer) + ([i]* [message_size])
        sge.addr = limen::slot_addr(buff_addr, slot, message_size);
        sge.length = message_size;
        sge.lkey = lkey;

        wr.num_sge = 1;
        wr.sg_list = &sge;
        wr.next = nullptr;
        wr.wr_id = slot | limen::RECV_WRID_TAG;

        return ibv_post_recv(queue_pair, &wr,  &bad);
    }

    //  A CM step that times out or delivers the wrong event is a connection
    //  failure, not a verb failure. cm.cpp cannot throw SessionError without
    //  inverting the layering, so the translation happens here.
    limen::Event expect_event(limen::EventChannel& ec,
                            rdma_cm_event_type   want,
                            int                  timeout_ms,
                            const char*          where)
    {
        try {
            return limen::get_expected_event(ec, want, timeout_ms);
        } catch (const limen::VerbsError& e) {
            throw limen::SessionError(where, e.error());
        }
    }
}

namespace limen 
{
    SessionError::SessionError(const char*op, int err):std::runtime_error(std::format("{}: {}",op,strerror(err))) ,_err(err){}

    //  helpers
    void fill_qp_init_attr(ibv_qp_init_attr* qp_init_attr, uint32_t send_wr, uint32_t recv_wr)
    {
        //  fill qp_init_attr.cap
        //  NOTE: CALLER MUST SET qp_init_attr's send_cq AND recv_cq
        *qp_init_attr = ibv_qp_init_attr{}; //  zero it before use
        qp_init_attr->cap.max_send_wr = send_wr;
        qp_init_attr->cap.max_inline_data = 256;   
        qp_init_attr->cap.max_recv_wr = recv_wr;
        qp_init_attr->cap.max_send_sge = 1;
        qp_init_attr->cap.max_recv_sge = 1;

        //  fill qp_init_attr to make the QP
        qp_init_attr->srq=NULL;
        qp_init_attr->qp_type = IBV_QPT_RC;
        qp_init_attr->sq_sig_all= 0;
    }

    
    //  Session functions
    Session Session::create_client_session(const char* peer, const SessionConfig& config)
    {
        return PendingConnection::resolve(peer, config).finish();
    }

    Session Session::create_server_session(const SessionConfig& config)
    {
        return PendingConnection::listen(config).finish();
    }

    int Session::close() noexcept 
    {
        _id.destroy_qp();
        _cq.close();
        _send_mr.close();
        _recv_mr.close();
        _pd.close();
        _id.close();
        _ec.close();

        return 0;
    }
    
    Session& Session::operator=(Session&& o) noexcept
    {
        if (this == &o) return *this;
        close();
        _ec = std::move(o._ec);
        _id = std::move(o._id);
        _pd = std::move(o._pd);
        _recv_mr = std::move(o._recv_mr);
        _send_mr = std::move(o._send_mr);
        _cq = std::move(o._cq);
        _peer = o._peer;
        _has_peer = o._has_peer;
        _is_client = o._is_client;
        _init_config = o._init_config;
        _caps = o._caps;
        return *this;
    }

    int Session::disconnect() noexcept
    {
        if (rdma_disconnect(_id.get())!=0)
        {
            return errno;
        };
        return 0;
    }

    void Session::wait_for_disconnect(int timeout_ms)
    {
        //  wait for event to appear
        if (_ec.wait(timeout_ms) != 0)
        {
            //  throw error
            throw SessionError("wait_for_disconnect timeout",ETIMEDOUT);
        }
        Event e(_ec);
        if (e.type() != RDMA_CM_EVENT_DISCONNECTED && e.type() != RDMA_CM_EVENT_TIMEWAIT_EXIT)
        {
            throw SessionError(
                std::format("wait_for_disconnect unexpected event {}",e.name()).c_str(),
                errno);
        }
    }


    int      Session::repost_recv(uint32_t slot) noexcept
    {

        return post_recv(slot, (uint64_t)(uintptr_t)_recv_mr.get()->addr, qp(), _init_config.recv_slot_size, _recv_mr.get()->lkey);
    }
    uint32_t Session::remove_tags(uint64_t wr_id) noexcept
    {
        return static_cast<uint32_t>(wr_id & 0xFFFFFFFFULL);
    }
    bool Session::is_recv_wrid(uint64_t wr_id) noexcept
    {
        return (wr_id & RECV_WRID_TAG)!=0;
    }


    //  PendingConnection functions
    int PendingConnection::close() noexcept
    {
        _id.destroy_qp();
        _cq.close();
        _send_mr.close();
        _recv_mr.close();
        _pd.close();
        _id.close();
        _listen_id.close();
        _ec.close();
        return 0;
    }

    PendingConnection::PendingConnection(PendingConnection&& o) noexcept
    {
        _ec        = std::move(o._ec);
        _listen_id = std::move(o._listen_id);
        _id        = std::move(o._id);
        _pd        = std::move(o._pd);
        _recv_mr   = std::move(o._recv_mr);
        _send_mr   = std::move(o._send_mr);
        _cq        = std::move(o._cq);
        _peer      = o._peer;
        _config    = o._config;
        _is_client = o._is_client;
        _has_peer  = o._has_peer;
    }

    PendingConnection& PendingConnection::operator=(PendingConnection&& o) noexcept
    {
        if (this == &o) return *this;
        close();
        _ec        = std::move(o._ec);
        _listen_id = std::move(o._listen_id);
        _id        = std::move(o._id);
        _pd        = std::move(o._pd);
        _recv_mr   = std::move(o._recv_mr);
        _send_mr   = std::move(o._send_mr);
        _cq        = std::move(o._cq);
        _peer      = o._peer;
        _config    = o._config;
        _is_client = o._is_client;
        _has_peer  = o._has_peer;
        return *this;
    }

    PendingConnection PendingConnection::resolve(const char* peer, const SessionConfig& config)
    {

        EventChannel ec = EventChannel::create();
        ConnectionId conn_id(ec, RDMA_PS_TCP);

        //  construct the sockaddr_in struct that points to the server
        sockaddr_in remote_sockaddr{};
        remote_sockaddr.sin_family = AF_INET;
        remote_sockaddr.sin_port   = htons(config.tcp_port);
        if (inet_pton(AF_INET, peer, &remote_sockaddr.sin_addr) != 1)
        {
            throw SessionError("PendingConnection::resolve:inet_pton", errno);
        }

        //  resolve addr
        Event e;
        if (rdma_resolve_addr(conn_id.get(), nullptr, (struct sockaddr*)&remote_sockaddr, 5000) != 0)
        {
            throw SessionError("PendingConnection::resolve:rdma_resolve_addr", errno);
        }
        e = expect_event(ec, RDMA_CM_EVENT_ADDR_RESOLVED, 5000,
                         "PendingConnection::resolve:ADDR_RESOLVED");
        //  resolve route
        if (rdma_resolve_route(conn_id.get(), 5000) != 0)
        {
            throw SessionError("PendingConnection::resolve:rdma_resolve_route", errno);
        }
        e = expect_event(ec, RDMA_CM_EVENT_ROUTE_RESOLVED, 5000,
                         "PendingConnection::resolve:ROUTE_RESOLVED");
        //  get device attributes
        ibv_device_attr device_attr{};
        if (ibv_query_device(conn_id.get()->verbs, &device_attr) != 0)
        {
            throw SessionError("PendingConnection::resolve:ibv_query_device", errno);
        }

        //  fill qp_init_attr
        ibv_qp_init_attr qp_init_attr{};
        uint32_t recv_wr = std::min(config.recv_wr, (uint32_t)device_attr.max_qp_wr/2);
        uint32_t send_wr = std::min(config.send_wr, (uint32_t)device_attr.max_qp_wr/2);
        int cqe = config.cqe > 0 ? std::min(config.cqe, device_attr.max_cqe): device_attr.max_cqe;
        //  the pre-post loop indexes slots, so the region must hold them
        if (recv_wr > std::max(config.recv_slots, 1u))
        {
            throw SessionError("PendingConnection::resolve:recv_wr exceeds recv_slots", EINVAL);
        }
        uint32_t recv_len = config.recv_slot_size * std::max(config.recv_slots, 1u);
        uint32_t send_len = config.send_slot_size * std::max(config.send_slots, 1u);
        fill_qp_init_attr(&qp_init_attr, send_wr, recv_wr);

        //  device has now been decided, create the wrapper instances
        ProtectionDomain pd      = ProtectionDomain(conn_id.get()->verbs);
        MemoryRegion     recv_mr = MemoryRegion(pd, recv_len, config.recv_access_flags);
        MemoryRegion     send_mr = MemoryRegion(pd, send_len, config.send_access_flags);
        CompletionQueue  cq      = CompletionQueue(conn_id.get()->verbs, cqe, nullptr, nullptr, 0);
        GrantedCaps caps{};
        qp_init_attr.send_cq = cq.get();
        qp_init_attr.recv_cq = cq.get();

        //  create QP
        if (rdma_create_qp(conn_id.get(), pd.get(), &qp_init_attr) != 0)
        {
            throw SessionError("PendingConnection::resolve:rdma_create_qp", errno);
        }

        //  now we can fill _caps
        caps.max_inline_data = qp_init_attr.cap.max_inline_data;
        caps.max_recv_wr = qp_init_attr.cap.max_recv_wr;
        caps.max_send_wr = qp_init_attr.cap.max_send_wr;
        //  initiator_depth and responder resources have to be filled on finish 

        //  post work requests
        for (uint32_t slot = 0; slot < recv_wr; slot++)
        {
            int rc = post_recv(slot, (uint64_t)(uintptr_t)recv_mr.get()->addr, conn_id.qp(),
                               config.recv_slot_size, recv_mr.get()->lkey);
            if (rc != 0)
            {
                throw SessionError("PendingConnection::resolve:post_recv", rc);
            }
        }

        PendingConnection pc;
        pc._ec        = std::move(ec);
        pc._id        = std::move(conn_id);
        pc._pd        = std::move(pd);
        pc._recv_mr   = std::move(recv_mr);
        pc._send_mr   = std::move(send_mr);
        pc._cq        = std::move(cq);
        pc._config    = config;
        pc._is_client = true;
        pc._has_peer  = false;   //  client learns the peer at ESTABLISHED
        pc._caps = caps;
        return pc;
    }

    PendingConnection PendingConnection::listen(const SessionConfig& config)
    {
        EventChannel ec = EventChannel::create();
        ConnectionId listen_id(ec, RDMA_PS_TCP);
        Event e;

        //  bind addr
        sockaddr_in server_sockaddr{};
        server_sockaddr.sin_family      = AF_INET;
        server_sockaddr.sin_port        = htons(config.tcp_port);
        server_sockaddr.sin_addr.s_addr = INADDR_ANY;   //  verbs not set until CONNECT_REQUEST
        if (rdma_bind_addr(listen_id.get(), (struct sockaddr*)&server_sockaddr) != 0)
        {
            throw SessionError("PendingConnection::listen:rdma_bind_addr", errno);
        }
        if (rdma_listen(listen_id.get(), 1) != 0)
        {
            throw SessionError("PendingConnection::listen:rdma_listen", errno);
        }

        //  this event has the new id associated with the client
        e = expect_event(ec, RDMA_CM_EVENT_CONNECT_REQUEST, -1,
                         "PendingConnection::listen:CONNECT_REQUEST");
        //  adopt event->id as 2nd identifier
        ConnectionId client_conn_id = ConnectionId::adopt(e.id());

        //  copy out payload
        ConnInfo remote_raw{};
        e.copy_private_data(&remote_raw, sizeof(remote_raw));
        ConnInfo peer_info = from_wire_format(remote_raw);

        //  get device attributes
        ibv_device_attr device_attr{};
        if (ibv_query_device(client_conn_id.get()->verbs, &device_attr) != 0)
        {
            throw SessionError("PendingConnection::listen:ibv_query_device", errno);
        }


        uint32_t recv_len = config.recv_slot_size * std::max(config.recv_slots, 1u);
        uint32_t send_len = config.send_slot_size * std::max(config.send_slots, 1u);

        //  fill qp_init_attr
        //  NOTE: certain adapters evenly split max_qp_wr across send and recv work requests
        ibv_qp_init_attr qp_init_attr{};
        uint32_t recv_wr = std::min(config.recv_wr, (uint32_t)device_attr.max_qp_wr/2);
        uint32_t send_wr = std::min(config.send_wr, (uint32_t)device_attr.max_qp_wr/2);
        int cqe = config.cqe > 0 ? std::min(config.cqe, device_attr.max_cqe): device_attr.max_cqe;
        fill_qp_init_attr(&qp_init_attr, send_wr, recv_wr);

        //  the pre-post loop indexes slots, so the region must hold them
        if (recv_wr > std::max(config.recv_slots, 1u))
        {
            throw SessionError("PendingConnection::listen:recv_wr exceeds recv_slots", EINVAL);
        }

        //  device has now been decided, create the wrapper instances
        ProtectionDomain pd      = ProtectionDomain(client_conn_id.get()->verbs);
        MemoryRegion     recv_mr = MemoryRegion(pd, recv_len, config.recv_access_flags);
        MemoryRegion     send_mr = MemoryRegion(pd, send_len, config.send_access_flags);
        CompletionQueue  cq      = CompletionQueue(client_conn_id.get()->verbs, cqe, nullptr, nullptr, 0);
        GrantedCaps caps{};

        qp_init_attr.send_cq = cq.get();
        qp_init_attr.recv_cq = cq.get();

        //  create QP on adopted identifier
        if (rdma_create_qp(client_conn_id.get(), pd.get(), &qp_init_attr) != 0)
        {
            throw SessionError("PendingConnection::listen:rdma_create_qp", errno);
        }

        //  now we can fill _caps
        caps.max_inline_data = qp_init_attr.cap.max_inline_data;
        caps.max_recv_wr = qp_init_attr.cap.max_recv_wr;
        caps.max_send_wr = qp_init_attr.cap.max_send_wr;
        //  initiator_depth and responder resources have to be filled on finish 


        //  post work requests before accept, or the first inbound send hits RNR
        for (uint32_t slot = 0; slot < recv_wr; slot++)
        {
            int rc = post_recv(slot, (uint64_t)(uintptr_t)recv_mr.get()->addr, client_conn_id.qp(),
                               config.recv_slot_size, recv_mr.get()->lkey);
            if (rc != 0)
            {
                throw SessionError("PendingConnection::listen:post_recv", rc);
            }
        }

        PendingConnection pc;
        pc._ec        = std::move(ec);
        pc._listen_id = std::move(listen_id);
        pc._id        = std::move(client_conn_id);
        pc._pd        = std::move(pd);
        pc._recv_mr   = std::move(recv_mr);
        pc._send_mr   = std::move(send_mr);
        pc._cq        = std::move(cq);
        pc._peer      = peer_info;
        pc._config    = config;
        pc._is_client = false;
        pc._has_peer  = true;    //  arrived with CONNECT_REQUEST
        pc._caps = caps;
        return pc;
    }

    Session PendingConnection::finish() &&
    {
        //  fill rdma_conn_param struct
        rdma_conn_param cp{};
        ConnInfo outbound_cinfo = to_wire_format(ConnInfo(
            (uint64_t)(uintptr_t)_recv_mr.get()->addr,
            _recv_mr.get()->rkey,
            _recv_mr.get()->length
        ));

        cp.private_data        = (void*)&outbound_cinfo;
        cp.private_data_len    = sizeof(outbound_cinfo);
        cp.responder_resources = _config.responder_resources;
        cp.initiator_depth     = _config.initiator_depth;
        cp.retry_count         = _config.retry_count;
        cp.rnr_retry_count     = (uint8_t)_config.rnr_retry_count;

        Event e;
        if (_is_client)
        {
            if (rdma_connect(_id.get(), &cp) != 0)
            {
                throw SessionError("PendingConnection::finish:rdma_connect", errno);
            }
            e = expect_event(_ec, RDMA_CM_EVENT_ESTABLISHED, 5000,
                            "PendingConnection::finish:ESTABLISHED");
            ConnInfo remote_raw{};
            e.copy_private_data(&remote_raw, sizeof(remote_raw));
            _peer     = from_wire_format(remote_raw);
            _has_peer = true;
        }
        else
        {
            if (rdma_accept(_id.get(), &cp) != 0)
            {
                throw SessionError("PendingConnection::finish:rdma_accept", errno);
            }
            e = expect_event(_ec, RDMA_CM_EVENT_ESTABLISHED, -1,
                            "PendingConnection::finish:ESTABLISHED");
        }

        Session s;
        s._ec          = std::move(_ec);
        s._id          = std::move(_id);
        s._pd          = std::move(_pd);
        s._recv_mr     = std::move(_recv_mr);
        s._send_mr     = std::move(_send_mr);
        s._cq          = std::move(_cq);
        s._peer        = _peer;
        s._has_peer    = _has_peer;
        s._is_client   = _is_client;
        s._init_config = _config;
        s._caps = _caps;
        s._caps.initiator_depth = e.initiator_depth();
        s._caps.responder_resources = e.responder_resources();
        //  negotiated values from the ESTABLISHED event, not what was requested.
        return s;
    }

}