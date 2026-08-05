/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_runtime.hpp"

namespace
{
zlink::socket_base_t *&send_ready_dispatch_socket_tls ()
{
    static thread_local zlink::socket_base_t *socket = NULL;
    return socket;
}
}

bool zlink::socket_dispatch_bridge_t::load_send_ready_handler (
  zlink_send_ready_handler_fn *handler_out_, void **subject_out_, void **userdata_out_) const
{
    if (!handler_out_ || !subject_out_ || !userdata_out_)
        return false;

    while (true) {
        const uint32_t s1 = send_ready_seq.load (std::memory_order_acquire);
        if ((s1 & 1u) != 0)
            continue;

        zlink_send_ready_handler_fn handler = send_ready_handler.load (std::memory_order_acquire);
        void *subject = send_ready_handler_subject.load (std::memory_order_acquire);
        void *userdata = send_ready_handler_userdata.load (std::memory_order_acquire);
        const uint32_t s2 = send_ready_seq.load (std::memory_order_acquire);
        if (s1 != s2 || (s2 & 1u) != 0)
            continue;

        *handler_out_ = handler;
        *subject_out_ = subject;
        *userdata_out_ = userdata;
        return handler != NULL;
    }
}

void zlink::socket_dispatch_bridge_t::store_send_ready_handler (
  zlink_send_ready_handler_fn handler_, void *subject_, void *userdata_)
{
    scoped_lock_t writer_lock (send_ready_writer_sync);
    send_ready_seq.fetch_add (1, std::memory_order_acq_rel);
    send_ready_handler.store (handler_, std::memory_order_release);
    send_ready_handler_subject.store (subject_, std::memory_order_release);
    send_ready_handler_userdata.store (userdata_, std::memory_order_release);
    send_ready_seq.fetch_add (1, std::memory_order_acq_rel);
}

bool zlink::socket_dispatch_bridge_t::arm_send_ready_notification ()
{
    zlink_send_ready_handler_fn handler = NULL;
    void *subject = NULL;
    void *userdata = NULL;
    if (!load_send_ready_handler (&handler, &subject, &userdata))
        return false;

    send_ready_armed.store (true, std::memory_order_release);
    return true;
}

bool zlink::socket_dispatch_bridge_t::consume_send_ready_notification ()
{
    bool expected = true;
    return send_ready_armed.compare_exchange_strong (expected, false, std::memory_order_acq_rel,
                                                     std::memory_order_acquire);
}

void zlink::socket_dispatch_bridge_t::mark_send_recovery_pending ()
{
    send_ready_recovery_pending.store (true, std::memory_order_release);
    send_ready_recovery_ready.store (false, std::memory_order_release);
}

void zlink::socket_dispatch_bridge_t::clear_send_recovery_pending ()
{
    send_ready_recovery_pending.store (false, std::memory_order_release);
    send_ready_recovery_ready.store (false, std::memory_order_release);
}

void zlink::socket_dispatch_bridge_t::mark_send_recovery_ready ()
{
    if (!send_ready_recovery_pending.load (std::memory_order_acquire))
        return;
    send_ready_recovery_ready.store (true, std::memory_order_release);
}

void zlink::socket_dispatch_bridge_t::clear_send_recovery_ready ()
{
    send_ready_recovery_ready.store (false, std::memory_order_release);
}

bool zlink::socket_dispatch_bridge_t::send_recovery_pending () const
{
    return send_ready_recovery_pending.load (std::memory_order_acquire);
}

bool zlink::socket_dispatch_bridge_t::send_recovery_ready () const
{
    return send_ready_recovery_ready.load (std::memory_order_acquire);
}

zlink::socket_send_ready_dispatch_scope_t::socket_send_ready_dispatch_scope_t (
  socket_base_t *socket_) :
    _previous (send_ready_dispatch_socket_tls ())
{
    send_ready_dispatch_socket_tls () = socket_;
}

zlink::socket_send_ready_dispatch_scope_t::~socket_send_ready_dispatch_scope_t ()
{
    send_ready_dispatch_socket_tls () = _previous;
}

zlink::socket_base_t *zlink::socket_send_ready_dispatch_scope_t::current_socket ()
{
    return send_ready_dispatch_socket_tls ();
}

bool zlink::socket_send_ready_dispatch_scope_t::dispatching_socket (const socket_base_t *socket_)
{
    return send_ready_dispatch_socket_tls () == socket_;
}
