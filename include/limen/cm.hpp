#include <rdma/rdma_cma.h>
#include <utility>

namespace limen {

class EventChannel {                       /* rdma_event_channel * */
public:
    EventChannel() noexcept = default;
    EventChannel(std::nullptr_t) = delete;
    static EventChannel create();           /* throws VerbsError */
    ~EventChannel() noexcept {close();}                              
    EventChannel(const EventChannel&)                = delete;
    EventChannel& operator=(const EventChannel&)     = delete;
    EventChannel(EventChannel&& o) noexcept { _event_channel = std::exchange(o._event_channel,nullptr);}
    EventChannel& operator=(EventChannel&&) noexcept;

    rdma_event_channel *get() const noexcept {return _event_channel;}
    int close() noexcept;
    int wait(int timeout_ms) noexcept;
private:
    rdma_event_channel* _event_channel = nullptr;
};

class ConnectionId {                        /* rdma_cm_id * */
public:
    ConnectionId() noexcept = default;
    ConnectionId(EventChannel& ch, rdma_port_space ps);   /* creates */
    static ConnectionId adopt(rdma_cm_id *id) noexcept;   /* takes an existing one */

    ~ConnectionId() noexcept {close();}                              
    ConnectionId(const ConnectionId&)                = delete;
    ConnectionId& operator=(const ConnectionId&)     = delete;
    ConnectionId(ConnectionId&& o) noexcept { _cm_id = std::exchange(o._cm_id,nullptr);}
    ConnectionId& operator=(ConnectionId&&) noexcept;
    
    rdma_cm_id *get() const noexcept {return this->_cm_id;}
    ibv_qp     *qp()  const noexcept {return this->_cm_id ?  this->_cm_id->qp : nullptr;}
    int close() noexcept;
private:
    rdma_cm_id* _cm_id = nullptr;
};

class Event {                               
public:
    Event() noexcept = default;
    explicit Event(EventChannel& ch);       
    ~Event() noexcept {ack();}                      
    Event(const Event&)            = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) noexcept;
    Event& operator=(Event&&) noexcept;

    rdma_cm_event_type type()   const noexcept;
    const char        *name()   const noexcept {return _event ? rdma_event_str(_type) : nullptr;}
    int                status() const noexcept {return _status;}
    rdma_cm_id        *id()     const noexcept {return _id;}

    size_t copy_private_data(void *dst, size_t len) const noexcept;

    int ack() noexcept;                     
private:
    rdma_cm_event* _event = nullptr;
    rdma_cm_event_type _type = {};
    int _status = -1;
    rdma_cm_id* _id = nullptr;
    size_t _pd_size = 0;
    char _pd[56];

};

struct ConnInfo {                            /* the handshake payload */
    uint64_t addr;
    uint32_t rkey;
    uint32_t length;
};                                           /* 16 bytes; must fit in 56 */
static_assert(sizeof(ConnInfo) == 16, "unexpected layout");
static_assert(sizeof(ConnInfo) <= 56,  "exceeds the rdma_connect private data limit");
} /* namespace limen */