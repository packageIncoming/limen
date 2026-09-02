#include "limen/session.hpp"

namespace limen 
{
    //  helpers
    void fill_qp_init_attr(ibv_qp_init_attr*, const ibv_device_attr*, uint32_t send_wr, uint32_t recv_wr)
    {

    }
    //  PendingConnection functions
    PendingConnection PendingConnection::resolve(EventChannel &ec, const char *peer, const SessionConfig & session_config)
    {

    }

    PendingConnection PendingConnection::listen(EventChannel &ec, const SessionConfig &config)
    {

    }

    PendingConnection::PendingConnection(PendingConnection&& o ) noexcept
    {

    }

    PendingConnection& PendingConnection::operator=(PendingConnection&& o) noexcept
    {

    }

    ibv_context* PendingConnection::verbs() const noexcept
    {

    }

    Session PendingConnection::finish(ibv_pd* pd, ibv_qp_init_attr& qp_init_attr, ConnInfo local) &&
    {

    }

    
    //  Session functions
    Session::Session(Session&&) noexcept
    {

    }

    Session& Session::operator=(Session&&) noexcept
    {

    }

    int Session::disconnect() noexcept
    {

    }

    void Session::wait_for_disconnect(int timeout_ms)
    {
        
    }





}