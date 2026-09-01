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
    int error() const noexcept { return _err; }
private:
    int _err;
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

    ibv_device** get()   const noexcept {return _device_list;}              
private:
    ibv_device** _device_list = nullptr;
};

class Context {
public:
    Context() noexcept = default;
    explicit Context(const char *device_name);        
    ~Context() noexcept {close();};                              
    Context(const Context&)                = delete;
    Context& operator=(const Context&)     = delete;
    Context(Context&&) noexcept            = default;
    Context& operator=(Context&&) noexcept = default;

    ibv_context *get()   const noexcept {return _h.get();}              
    explicit operator bool() const noexcept{ return static_cast<bool>(_h);}
    int close() noexcept;       
private:
    limen::ResourceHandle<ibv_context, ibv_close_device> _h;               
};


class ProtectionDomain {
public:
    ProtectionDomain() noexcept = default;
    explicit ProtectionDomain(const Context& device_context);        
    //  borrowed context (e.g. rdma_cm_id->verbs), not owned by this object
    explicit ProtectionDomain(ibv_context* device_context);
    ~ProtectionDomain() noexcept {close();}                              
    ProtectionDomain(const ProtectionDomain&)                = delete;
    ProtectionDomain& operator=(const ProtectionDomain&)     = delete;
    ProtectionDomain(ProtectionDomain&&) noexcept            = default;
    ProtectionDomain& operator=(ProtectionDomain&&) noexcept = default;

    ibv_pd *get()   const noexcept {return _h.get();}              
    explicit operator bool() const noexcept { return static_cast<bool>(_h);}
    int close() noexcept;       
private:
    limen::ResourceHandle<ibv_pd, ibv_dealloc_pd> _h;               
};

class CompletionQueue {
public:
    CompletionQueue() noexcept = default;
    explicit CompletionQueue(const Context& device_context, int cqe, void* cq_context, ibv_comp_channel* channel, int comp_vector);        
    //  borrowed context (e.g. rdma_cm_id->verbs), not owned by this object
    explicit CompletionQueue(ibv_context* device_context, int cqe, void* cq_context, ibv_comp_channel* channel, int comp_vector);
    ~CompletionQueue() noexcept {close();}                              
    CompletionQueue(const CompletionQueue&)                = delete;
    CompletionQueue& operator=(const CompletionQueue&)     = delete;
    CompletionQueue(CompletionQueue&&) noexcept            = default;
    CompletionQueue& operator=(CompletionQueue&&) noexcept = default;

    ibv_cq *get()   const noexcept {return _h.get();}        
    int size() const noexcept { return _h.get() ? _h.get()->cqe : 0;}      
    explicit operator bool() const noexcept { return static_cast<bool>(_h);}
    int close() noexcept;       
private:
    limen::ResourceHandle<ibv_cq, ibv_destroy_cq> _h;               
};


class QueuePair {
public:
    QueuePair() noexcept = default;
    explicit QueuePair(const ProtectionDomain& pd, ibv_qp_init_attr* qp_init_attr);        
    ~QueuePair() noexcept {close();}                              
    QueuePair(const QueuePair&)                = delete;
    QueuePair& operator=(const QueuePair&)     = delete;
    QueuePair(QueuePair&&) noexcept            = default;
    QueuePair& operator=(QueuePair&&) noexcept = default;

    ibv_qp *get()   const noexcept {return _h.get();}              
    explicit operator bool() const noexcept { return static_cast<bool>(_h);}
    int close() noexcept;       
private:
    limen::ResourceHandle<ibv_qp, ibv_destroy_qp> _h;               
};


class MemoryRegion {
public:
    MemoryRegion() noexcept = default;
    MemoryRegion(const ProtectionDomain& pd, std::size_t bytes, int access);
    ~MemoryRegion() noexcept {close();}                              
    MemoryRegion(const MemoryRegion&)                = delete;
    MemoryRegion& operator=(const MemoryRegion&)     = delete;

    MemoryRegion(MemoryRegion&& o) noexcept
        : _buf(std::exchange(o._buf, nullptr)),
        _buf_size(std::exchange(o._buf_size, 0)),
        _h(std::move(o._h)) {}

    MemoryRegion& operator=(MemoryRegion&& o) noexcept {
        if (this != &o) {
            close();
            _buf = std::exchange(o._buf, nullptr);
            _buf_size = std::exchange(o._buf_size, 0);
            _h = std::move(o._h);
        }
        return *this;
    }

    ibv_mr     *get()    const noexcept {return _h.get();}
    void       *data()   const noexcept {return _buf;}              /* the owned buffer */
    std::size_t size()   const noexcept {return _buf_size;}
    int close() noexcept;
    uint32_t lkey() const noexcept { return _h.get() ? _h.get()->lkey : 0; }
    uint32_t rkey() const noexcept { return _h.get() ? _h.get()->rkey : 0; }
    explicit operator bool() const noexcept { return static_cast<bool>(_h) && (_buf!=nullptr);}

private:
    void* _buf = nullptr;
    size_t _buf_size = 0;
    ResourceHandle<ibv_mr, ibv_dereg_mr> _h;

};

class Endpoint {
public:
    Endpoint() noexcept = default;

    Endpoint(
            const char* device_name,
            int cqe, 
            void* cq_context,
            ibv_comp_channel* channel,
            int comp_vector,
            ibv_qp_init_attr* qp_init_attr,
            std::size_t message_size,
            int rx_depth,
            int buffer_access_flags
    );

    ~Endpoint() noexcept                     = default;                            
    Endpoint(const Endpoint&)                = delete;
    Endpoint& operator=(const Endpoint&)     = delete;
    Endpoint(Endpoint&&) noexcept            = default;
    Endpoint& operator=(Endpoint&&) noexcept = default;

    //  accessors
    ibv_context* get_ctx() const noexcept {return _ctx.get();}
    ibv_pd* pd() const noexcept {return _pd.get();}
    ibv_mr* get_recv_mr() const noexcept {return _recv_mr.get();}
    ibv_mr* get_send_mr() const noexcept {return _send_mr.get();}
    ibv_cq* get_cq() const noexcept {return _cq.get();}
    ibv_qp* get_qp() const noexcept {return _qp.get();}



private:
    Context _ctx;
    ProtectionDomain _pd;
    MemoryRegion _recv_mr;
    MemoryRegion _send_mr;
    CompletionQueue _cq;
    QueuePair _qp;
};

} /* namespace limen */

#endif