/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_runtime.hpp"

namespace
{
zlink::socket_base_t *&send_complete_dispatch_socket_tls ()
{
    static thread_local zlink::socket_base_t *socket = NULL;
    return socket;
}
}

void zlink::socket_dispatch_bridge_t::mark_send_recovery_pending ()
{
    send_recovery_pending_flag.store (true, std::memory_order_release);
    send_recovery_ready_flag.store (false, std::memory_order_release);
}

void zlink::socket_dispatch_bridge_t::clear_send_recovery_pending ()
{
    send_recovery_pending_flag.store (false, std::memory_order_release);
    send_recovery_ready_flag.store (false, std::memory_order_release);
}

void zlink::socket_dispatch_bridge_t::mark_send_recovery_ready ()
{
    if (!send_recovery_pending_flag.load (std::memory_order_acquire))
        return;
    send_recovery_ready_flag.store (true, std::memory_order_release);
}

void zlink::socket_dispatch_bridge_t::clear_send_recovery_ready ()
{
    send_recovery_ready_flag.store (false, std::memory_order_release);
}

bool zlink::socket_dispatch_bridge_t::send_recovery_pending () const
{
    return send_recovery_pending_flag.load (std::memory_order_acquire);
}

bool zlink::socket_dispatch_bridge_t::send_recovery_ready () const
{
    return send_recovery_ready_flag.load (std::memory_order_acquire);
}

zlink::socket_send_complete_dispatch_scope_t::
  socket_send_complete_dispatch_scope_t (socket_base_t *socket_) :
    _previous (send_complete_dispatch_socket_tls ())
{
    send_complete_dispatch_socket_tls () = socket_;
}

zlink::socket_send_complete_dispatch_scope_t::
  ~socket_send_complete_dispatch_scope_t ()
{
    send_complete_dispatch_socket_tls () = _previous;
}

zlink::socket_base_t *
zlink::socket_send_complete_dispatch_scope_t::current_socket ()
{
    return send_complete_dispatch_socket_tls ();
}

bool zlink::socket_send_complete_dispatch_scope_t::dispatching_socket (
  const socket_base_t *socket_)
{
    return send_complete_dispatch_socket_tls () == socket_;
}

//  Any completion callback on any socket bars a submit on any socket: the
//  contract is "hand the completion to application state and return", and a
//  nested submit is exactly the retry loop D1 removed.
bool zlink::socket_send_complete_dispatch_scope_t::dispatching_any ()
{
    return send_complete_dispatch_socket_tls () != NULL;
}
