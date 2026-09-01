#include "limen/verbs.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <infiniband/verbs.h>
#include <sys/resource.h>


namespace {

void trace_release(const char* name) noexcept
{
    if (std::getenv("LIMEN_TRACE_RELEASE"))
    {
        std::fprintf(stderr, "release: %s\n", name);
    }
}
int find_device_index(ibv_device** list, const char* name) noexcept
{
    if (list == nullptr || name == nullptr) { return -1; }
    for (int i = 0; list[i] != nullptr; ++i)
    {
        if (std::strcmp(ibv_get_device_name(list[i]), name) == 0)
        {
            return i;
        }
    }
    return -1;
}
} /* anonymous namespace */


limen::VerbsError::VerbsError(const char*op, int err):std::runtime_error(std::format("{}: {}",op,strerror(err))) ,_err(err){}


//  DeviceList functions
limen::DeviceList::DeviceList()
{
    this->_device_list = ibv_get_device_list(nullptr);
    if (this->_device_list == nullptr)
    {
        throw VerbsError("ibv_get_device_list",errno);
    }

}

limen::DeviceList::~DeviceList() noexcept
{
    if (_device_list != nullptr)
    {
        ibv_free_device_list(this->_device_list);
        _device_list = nullptr;
    }

}


//  Context functions
limen::Context::Context(const char* device_name)
{
    DeviceList devices_list;

    int device_idx = find_device_index(devices_list.get(),device_name);
    if (device_idx == -1)
    {
        //  did not find device, throw VerbsError
        throw VerbsError("find_device_by_name",ENODEV);
    }


    //  get device context
    ibv_context* device_context = ibv_open_device(devices_list.get()[device_idx]);
    if (device_context == nullptr)
    {
        //  failed to open device context
        throw VerbsError("ibv_open_device",ENODEV);
    }

    _h = ResourceHandle<ibv_context, ibv_close_device>(device_context);

}

int limen::Context::close() noexcept
{
    if (_h.get()) { trace_release("context"); }
    int rc = _h.close();
    if (rc != 0)
    {
        std::fprintf(stderr, "ibv_close_device: %s\n", strerror(rc));
        return rc;
    }
    return rc;
}

//  ProtectionDomain functions
limen::ProtectionDomain::ProtectionDomain(const limen::Context& device_context)
{
    ibv_pd* pd = ibv_alloc_pd(device_context.get());
    if (pd == nullptr)
    {
        int e = errno;
        std::fprintf(stderr, "ibv_alloc_pd: %s\n", strerror(e));
        throw VerbsError("ibv_alloc_pd",e);
    }
    _h = ResourceHandle<ibv_pd, ibv_dealloc_pd>(pd);

}

//  Same, against a context this object does not own (rdma_cm_id->verbs).
limen::ProtectionDomain::ProtectionDomain(ibv_context* device_context)
{
    ibv_pd* pd = ibv_alloc_pd(device_context);
    if (pd == nullptr)
    {
        int e = errno;
        std::fprintf(stderr, "ibv_alloc_pd: %s\n", strerror(e));
        throw VerbsError("ibv_alloc_pd",e);
    }
    _h = ResourceHandle<ibv_pd, ibv_dealloc_pd>(pd);

}

int limen::ProtectionDomain::close() noexcept
{
    if (_h.get()) { trace_release("pd"); }
    int rc = _h.close();
    if (rc != 0)
    {
        std::fprintf(stderr, "ibv_dealloc_pd: %s\n", strerror(rc));    
        return rc;
    }
    return rc;
}


//  CompletionQueue functions
limen::CompletionQueue::CompletionQueue(const Context& device_context, int cqe, void* cq_context, ibv_comp_channel* channel, int comp_vector)     
{
    ibv_cq* completion_queue = ibv_create_cq(
        device_context.get(),
        cqe,
        cq_context,
        channel,
        comp_vector
    );
    if (completion_queue == nullptr)
    {
        //  failed to create completion queue
        int e = errno;
        std::fprintf(stderr, "ibv_create_cq: %s\n", strerror(e));
        throw VerbsError("ibv_create_cq",e);
    }
    _h = ResourceHandle<ibv_cq, ibv_destroy_cq>(completion_queue);
}

//  Same, against a context this object does not own (rdma_cm_id->verbs).
limen::CompletionQueue::CompletionQueue(ibv_context* device_context, int cqe, void* cq_context, ibv_comp_channel* channel, int comp_vector)
{
    ibv_cq* completion_queue = ibv_create_cq(
        device_context,
        cqe,
        cq_context,
        channel,
        comp_vector
    );
    if (completion_queue == nullptr)
    {
        //  failed to create completion queue
        int e = errno;
        std::fprintf(stderr, "ibv_create_cq: %s\n", strerror(e));
        throw VerbsError("ibv_create_cq",e);
    }
    _h = ResourceHandle<ibv_cq, ibv_destroy_cq>(completion_queue);
}

int limen::CompletionQueue::close() noexcept
{
    if (_h.get()) { trace_release("cq"); }
    int rc = _h.close();
    if (rc != 0)
    {
        std::fprintf(stderr, "ibv_destroy_cq: %s\n", strerror(rc));
        return rc;
    }
    return rc;
}


//  QueuePair functions

limen::QueuePair::QueuePair(const ProtectionDomain& pd, ibv_qp_init_attr* qp_init_attr)
{
    ibv_qp* qp = ibv_create_qp(pd.get(),qp_init_attr);
    if (qp == nullptr)
    {
        int e = errno;
        std::fprintf(stderr, "ibv_create_qp: %s\n", strerror(e));
        throw VerbsError("ibv_create_qp",e);
    }
    _h = ResourceHandle<ibv_qp, ibv_destroy_qp>(qp);

}

int limen::QueuePair::close() noexcept
{
    if (_h.get()) { trace_release("qp"); }
    int rc = _h.close();
    if (rc != 0)
    {
        std::fprintf(stderr, "ibv_destroy_qp: %s\n", strerror(rc));    
        return rc;
    }
    return rc;
}

//  MemoryRegion functions
limen::MemoryRegion::MemoryRegion(const ProtectionDomain& pd, std::size_t bytes, int access)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
        if (bytes > rl.rlim_cur) {
            std::fprintf(stderr,
                "ibv_reg_mr: %zu bytes exceeds RLIMIT_MEMLOCK (%llu bytes)\n",
                bytes, (unsigned long long)rl.rlim_cur);
            throw VerbsError("ibv_reg_mr", ENOMEM);
        }
    }

    _buf = aligned_alloc(4096, bytes);
    if (_buf == nullptr) {
        std::fprintf(stderr, "aligned_alloc: %s\n", strerror(errno));
        throw VerbsError("aligned_alloc", errno);
    }
    memset(_buf, 0, bytes);
    _buf_size = bytes;

    ibv_mr* mr = ibv_reg_mr(pd.get(), _buf, bytes, access);
    if (mr == nullptr) {
        int e = errno;
        free(_buf);
        _buf = nullptr;
        _buf_size = 0;
        std::fprintf(stderr, "ibv_reg_mr: %s\n", strerror(e));
        throw VerbsError("ibv_reg_mr", e);
    }
    _h = ResourceHandle<ibv_mr, ibv_dereg_mr>(mr);
}

int limen::MemoryRegion::close() noexcept 
{
    if (_h.get()) { trace_release("mr"); }
    int rc = _h.close();
    if (rc != 0)
    {
        //  The region is still registered, so the buffer stays allocated.
        //  Freeing memory the NIC still holds a translation for is worse
        //  than leaking it.
        std::fprintf(stderr, "ibv_dereg_mr: %s\n", strerror(rc));    
        return rc;
    }

    //  free buffer too
    if (_buf != nullptr)
    {
        free(_buf);
        _buf = nullptr;
        _buf_size = 0;
    }

    return rc;
}


//  Endpoint functions
limen::Endpoint::Endpoint(
        const char* device_name,
        int cqe, 
        void* cq_context,
        ibv_comp_channel* channel,
        int comp_vector,
        ibv_qp_init_attr* qp_init_attr,
        std::size_t message_size,
        int rx_depth,
        int buffer_access_flags
):
_ctx(device_name),
_pd(_ctx),
_recv_mr(_pd,message_size*rx_depth,buffer_access_flags),
_send_mr(_pd,message_size,buffer_access_flags),
_cq(_ctx,cqe,cq_context,channel,comp_vector)
{
    //  set send and recv completion queues in here
    qp_init_attr->send_cq = _cq.get();
    qp_init_attr->recv_cq = _cq.get();
    _qp = limen::QueuePair(_pd,qp_init_attr);
}