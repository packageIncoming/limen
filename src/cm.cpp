#include "limen/cm.hpp"
#include <rdma/rdma_cma.h>

limen::EventChannel limen::EventChannel::create()
{
    //  static factory function to create a blank EventChannel instance
    //  we do this b/c rdma_create_event_channel
    EventChannel e;
    e._event_channel = rdma_create_event_channel();

    if (e._event_channel == nullptr)
    {
        //  failed to create, errno is set
        std::fprintf(stderr, "ibv_alloc_pd: %s\n", strerror(errno));
        throw VerbsError("ibv_alloc_pd",errno);
    }

    return e;
}