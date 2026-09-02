#include "limen/cm.hpp"
#include "limen/verbs.hpp"
#include <cstdlib>
#include <cstring>
#include <endian.h>
#include <rdma/rdma_cma.h>
#include <sys/poll.h>
#include <utility>


//  EventChannel functions
limen::EventChannel limen::EventChannel::create()
{
    //  static factory function to create a blank EventChannel instance
    //  we do this b/c rdma_create_event_channel
    EventChannel e;
    e._event_channel = rdma_create_event_channel();

    if (e._event_channel == nullptr)
    {
        //  failed to create, errno is set
        std::fprintf(stderr, "rdma_create_event_channel: %s\n", strerror(errno));
        throw VerbsError("rdma_create_event_channel",errno);
    }

    return e;
}

int limen::EventChannel::close() noexcept
{
    //  (rdma_destroy_event_channel):
    //  All rdma_cm_id's associated with the event channel must be destroyed,
    //  and all returned events must be acked before calling this function.
    if (!_event_channel) return 0;
    rdma_destroy_event_channel(_event_channel);
    _event_channel = nullptr;
    return 0;
}

limen::EventChannel& limen::EventChannel::operator=(limen::EventChannel&& o) noexcept
{
    if (this != &o)
    {
        close();
        _event_channel = std::exchange(o._event_channel,nullptr);
    }
    return *this;
}

int limen::EventChannel::wait(int timeout_ms) noexcept
{
    //  waits for an event on the event channel using poll(), returns 0 if found 1 if timedout or err
    pollfd pfd{};
    pfd.fd = _event_channel->fd;
    pfd.events = POLLIN;
    
    if(poll(&pfd, 1, timeout_ms) <=0)
    {
        return 1;
    }
    return 0;
}


//  ConnectionId functions
limen::ConnectionId::ConnectionId(EventChannel& ch, rdma_port_space ps)
{
    int rc = rdma_create_id(
        ch.get(),
        &_cm_id,
        nullptr,
        ps
    );
    if (rc)
    {
        std::fprintf(stderr, "rdma_create_id: %s\n", strerror(errno));
        throw VerbsError("rdma_create_id",errno);
    }
}


limen::ConnectionId limen::ConnectionId::adopt(rdma_cm_id *id) noexcept
{
    //  takes an existing rdma_cm_id & owns its destruction
    limen::ConnectionId cid;
    cid._cm_id = id;
    return cid;
}

int limen::ConnectionId::close() noexcept
{
    //  need to free the QP (rdma_destroy_qp) before the rdma_cm_id
    if (this->_cm_id == nullptr) return 0;

    if (_cm_id->qp)
    {
        rdma_destroy_qp(_cm_id);
    }
    int rc = rdma_destroy_id(_cm_id);
    if (rc == 0) _cm_id = nullptr;
    return rc;
}

limen::ConnectionId& limen::ConnectionId::operator=(limen::ConnectionId&& o) noexcept
{
    if (this != &o)
    {
        close();
        //  NOTE: a failing release would cause a leak (old id lost w/ no recovery)
        _cm_id = std::exchange(o._cm_id,nullptr);
    }
    return *this;
}


//  Event functions
limen::Event::Event(EventChannel& ch)
{
    int rc = rdma_get_cm_event(ch.get(),&_event);
    if (rc != 0)
    {
        //  failed to get an event
        std::fprintf(stderr, "rdma_get_cm_event: %s\n", strerror(errno));
        throw VerbsError("rdma_get_cm_event",errno);
    }
    //  snapshot at creation so that its accessible even after ack()
    _type = _event->event;
    _status = _event->status;
    _id = _event->id;
    _init_depth = _event->param.conn.initiator_depth;
    _resp_res = _event->param.conn.responder_resources;

    //  snapshot the private_data
    if (_event->param.conn.private_data && _event->param.conn.private_data_len>0)
    {
        //  there is data to copy
        _pd_size = std::min<size_t>(_event->param.conn.private_data_len,sizeof _pd);
        memcpy(_pd, _event->param.conn.private_data, _pd_size);
    }
}

limen::Event::Event(limen::Event&& o) noexcept
{
    _event = std::exchange(o._event, nullptr);
    _type = o._type;
    _status = o._status;
    _id = std::exchange(o._id,nullptr);
    _pd_size = o._pd_size;
    _init_depth = o._init_depth;
    _resp_res = o._resp_res;
    memcpy(_pd, o._pd, _pd_size);
}

limen::Event& limen::Event::operator=(limen::Event&& o) noexcept 
{
    if (this != &o)
    {
        ack();
        _event = std::exchange(o._event, nullptr);
        _type = o._type;
        _status = o._status;
        _id = std::exchange(o._id,nullptr);
        _pd_size = o._pd_size;
        _init_depth = o._init_depth;
        _resp_res = o._resp_res;
        memcpy(_pd, o._pd, _pd_size);
    }
    return *this;
}

int limen::Event::ack() noexcept
{
    //  returns errno on fail
    if (_event == nullptr) return 0;
    int rc = rdma_ack_cm_event(_event);
    _event = nullptr;
    if (rc != 0)
    {
        return errno;
    }   
    return 0;
}

size_t limen::Event::copy_private_data(void* dst, size_t len) const noexcept
{
    if (dst == nullptr) return 0;
    size_t amount = std::min<size_t>(len,_pd_size);
    memcpy(dst, _pd, amount);
    return amount;
}

//  Converts from host to network (big-endian)
limen::ConnInfo limen::to_wire_format(limen::ConnInfo src)
{
    return limen::ConnInfo(
        htobe64(src.addr),
        htobe32(src.rkey),
        htobe32(src.length)
    );

}

limen::ConnInfo limen::from_wire_format(limen::ConnInfo src)
{
    return limen::ConnInfo(
        be64toh(src.addr),
        be32toh(src.rkey),
        be32toh(src.length) 
    );
}
