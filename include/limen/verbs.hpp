#ifndef limen_namespace
#define limen_namespace
#include <infiniband/verbs.h>
#include <stdexcept>
#include <utility>
#include <cstddef>
#include <cstdint>

namespace limen {

class VerbsError : public std::runtime_error {
public:
    VerbsError(const char *op, int err);
    int error() const noexcept;
};

//  template for resource handles, each class has a _h which is an instance of one of these
template<typename Handle, int (*Deleter)(Handle*)>
class ResourceHandle{
public:
    ResourceHandle() noexcept = default;
    explicit ResourceHandle(Handle *h) noexcept : h_(h) {}
    ~ResourceHandle() noexcept { close(); }

    ResourceHandle(const ResourceHandle&)            = delete;
    ResourceHandle& operator=(const ResourceHandle&) = delete;

    ResourceHandle(ResourceHandle&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    ResourceHandle& operator=(ResourceHandle&& o) noexcept {
        if (this != &o) { close(); h_ = std::exchange(o.h_, nullptr); }
        return *this;
    }

    Handle *get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != nullptr; }

    int close() noexcept {
        if (!h_) return 0;
        int rc = Deleter(h_);
        if (rc == 0) h_ = nullptr;
        return rc;
    }

private:
    Handle *h_ = nullptr;
};

class DeviceList {
public:
    DeviceList();
    ~DeviceList() noexcept;                              
    DeviceList(const DeviceList&)                = delete;
    DeviceList& operator=(const DeviceList&)     = delete;
    DeviceList(DeviceList&&) noexcept            = delete;
    DeviceList& operator=(DeviceList&&) noexcept = delete;

    ibv_device** get()   const noexcept;              
private:
    ibv_device** device_list = nullptr;
};

class Context {
public:
    Context() noexcept = default;
    explicit Context(const char *device_name);        
    ~Context() noexcept;                              
    Context(const Context&)                = delete;
    Context& operator=(const Context&)     = delete;
    Context(Context&&) noexcept            = default;
    Context& operator=(Context&&) noexcept = default;

    ibv_context *get()   const noexcept;              
    explicit operator bool() const noexcept;
    int close() noexcept;       
private:
    limen::ResourceHandle<ibv_context, ibv_close_device> _h;               
};


class ProtectionDomain {
public:
    ProtectionDomain() noexcept = default;
    explicit ProtectionDomain(ibv_context* device_context);        
    ~ProtectionDomain() noexcept;                              
    ProtectionDomain(const ProtectionDomain&)                = delete;
    ProtectionDomain& operator=(const ProtectionDomain&)     = delete;
    ProtectionDomain(ProtectionDomain&&) noexcept            = default;
    ProtectionDomain& operator=(ProtectionDomain&&) noexcept = default;

    ibv_pd *get()   const noexcept;              
    explicit operator bool() const noexcept;
    int close() noexcept;       
private:
    limen::ResourceHandle<ibv_pd, ibv_dealloc_pd> _h;               
};

class CompletionQueue {
public:
    CompletionQueue() noexcept = default;
    explicit CompletionQueue(ibv_context* device_context, int cqe, void* cq_context, ibv_comp_channel* channel, int comp_vector);        
    ~CompletionQueue() noexcept;                              
    CompletionQueue(const CompletionQueue&)                = delete;
    CompletionQueue& operator=(const CompletionQueue&)     = delete;
    CompletionQueue(CompletionQueue&&) noexcept            = default;
    CompletionQueue& operator=(CompletionQueue&&) noexcept = default;

    ibv_cq *get()   const noexcept;              
    explicit operator bool() const noexcept;
    int close() noexcept;       
private:
    limen::ResourceHandle<ibv_cq, ibv_destroy_cq> _h;               
};


class QueuePair {
public:
    QueuePair() noexcept = default;
    explicit QueuePair(ibv_pd* pd, ibv_qp_init_attr* qp_init_attr);        
    ~QueuePair() noexcept;                              
    QueuePair(const QueuePair&)                = delete;
    QueuePair& operator=(const QueuePair&)     = delete;
    QueuePair(QueuePair&&) noexcept            = default;
    QueuePair& operator=(QueuePair&&) noexcept = default;

    ibv_qp *get()   const noexcept;              
    explicit operator bool() const noexcept;
    int close() noexcept;       
private:
    limen::ResourceHandle<ibv_qp, ibv_destroy_qp> _h;               
};


class MemoryRegion {
public:
    MemoryRegion() noexcept = default;
    MemoryRegion(ProtectionDomain& pd, std::size_t bytes, int access);
    ~MemoryRegion() noexcept;                              
    MemoryRegion(const MemoryRegion&)                = delete;
    MemoryRegion& operator=(const MemoryRegion&)     = delete;
    MemoryRegion(MemoryRegion&&) noexcept            = default;
    MemoryRegion& operator=(MemoryRegion&&) noexcept = default;

    ibv_mr     *get()    const noexcept;
    void       *data()   const noexcept;              /* the owned buffer */
    std::size_t size()   const noexcept;
    int close() noexcept;
    uint32_t lkey() const noexcept;
    uint32_t rkey() const noexcept;
private:
    void* buf;
    ResourceHandle<ibv_mr, ibv_dereg_mr> _h;

};

} /* namespace limen */

#endif