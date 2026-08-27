#include <rdma/rdma_cma.h>
#include "limen/verbs.hpp"

namespace limen {

class EventChannel {                       /* rdma_event_channel * */
public:
    EventChannel() noexcept = default;
    EventChannel(std::nullptr_t) = delete;
    static EventChannel create();           /* throws VerbsError */
    /* five special members, move-only, noexcept dtor */

    ~EventChannel() noexcept;                              
    EventChannel(const EventChannel&)                = delete;
    EventChannel& operator=(const EventChannel&)     = delete;
    EventChannel(EventChannel&&) noexcept;
    EventChannel& operator=(EventChannel&&) noexcept = default;

    rdma_event_channel *get() const noexcept;
    int close() noexcept;
private:
    rdma_event_channel* _event_channel = nullptr;
};

class ConnectionId {                        /* rdma_cm_id * */
public:
    ConnectionId() noexcept = default;
    ConnectionId(EventChannel& ch, rdma_port_space ps);   /* creates */
    static ConnectionId adopt(rdma_cm_id *id) noexcept;   /* takes an existing one */
    /* five special members, move-only, noexcept dtor */
    rdma_cm_id *get() const noexcept;
    ibv_qp     *qp()  const noexcept;       /* nullptr before create_qp */
    int close() noexcept;
};

class Event {                               /* rdma_cm_event *, acked on destruction */
public:
    explicit Event(EventChannel& ch);       /* blocks; throws VerbsError */
    ~Event() noexcept;                      /* acknowledges */
    Event(const Event&)            = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) noexcept;
    Event& operator=(Event&&) noexcept;

    rdma_cm_event_type type()   const noexcept;
    const char        *name()   const noexcept;   /* rdma_event_str */
    int                status() const noexcept;
    rdma_cm_id        *id()     const noexcept;

    /* Copies at most `len` bytes of private data out. Returns bytes copied.
       There is no accessor returning the raw pointer, by design. */
    size_t copy_private_data(void *dst, std::size_t len) const noexcept;

    int ack() noexcept;                     /* idempotent; dtor calls it */
};

struct ConnInfo {                            /* the handshake payload */
    uint64_t addr;
    uint32_t rkey;
    uint32_t length;
};                                           /* 16 bytes; must fit in 56 */

} /* namespace limen */