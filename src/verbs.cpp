#include "limen/verbs.hpp"
#include "limen/limen_common.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <infiniband/verbs.h>


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
    ibv_free_device_list(this->_device_list);

}


//  Context functions
limen::Context::Context(const char* device_name)
{
    DeviceList devices_list;

    int device_idx = find_device_by_name(devices_list.get(),device_name);
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
    int rc = _h.close();
    if (rc != 0)
    {
        std::fprintf(stderr, "ibv_close_device: %s\n", strerror(ENODEV));    
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
        std::fprintf(stderr, "ibv_alloc_pd: %s\n", strerror(ENOMEM));    
        throw VerbsError("ibv_alloc_pd",ENOMEM);
    }
    _h = ResourceHandle<ibv_pd, ibv_dealloc_pd>(pd);

}

int limen::ProtectionDomain::close() noexcept
{
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
    printf("%i\n",cqe);
    ibv_cq* completion_queue = ibv_create_cq(
        device_context.get(),
        3,
        cq_context,
        channel,
        comp_vector
    );
    if (completion_queue == nullptr)
    {
        //  failed to create completion queue
        std::fprintf(stderr, "ibv_create_cq: %s\n", strerror(errno));    
        throw VerbsError("ibv_create_cq",errno);
    }
    _h = ResourceHandle<ibv_cq, ibv_destroy_cq>(completion_queue);
}

int limen::CompletionQueue::close() noexcept
{
    int rc = _h.close();
    if (rc != 0)
    {
        std::fprintf(stderr, "ibv_destroy_pd: %s\n", strerror(ENOMEM));    
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
        std::fprintf(stderr, "ibv_create_qp: %s\n", strerror(errno));    
        throw VerbsError("ibv_create_qp",errno);
    }
    _h = ResourceHandle<ibv_qp, ibv_destroy_qp>(qp);

}

int limen::QueuePair::close() noexcept
{
    int rc = _h.close();
    if (rc != 0)
    {
        std::fprintf(stderr, "ibv_destroy_qp: %s\n", strerror(rc));    
        return rc;
    }
    return rc;
}

//  MemoryRegion functions
limen::MemoryRegion::MemoryRegion(ProtectionDomain& pd, std::size_t bytes, int access)
{
    _buf = aligned_alloc(4096, bytes);
    _buf_size = bytes;
    if (_buf == nullptr)
    {
        std::fprintf(stderr, "aligned_alloc: %s\n", strerror(errno));    
        throw VerbsError("aligned_alloc",errno);
    }
    memset(_buf,0,bytes);

    ibv_mr* mr = ibv_reg_mr(
        pd.get(),
        _buf,
        bytes,
        access
    );

    if (mr == nullptr)
    {
        std::fprintf(stderr, "ibv_reg_mr: %s\n", strerror(errno));    
        throw VerbsError("ibv_reg_mr",errno);
    }
    _h = ResourceHandle<ibv_mr, ibv_dereg_mr>(mr);

}

int limen::MemoryRegion::close() noexcept 
{
    int rc = _h.close();
    if (rc != 0)
    {
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

