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

bool zlink::socket_dispatch_bridge_t::routed_send_ready_handler_active () const
{
    scoped_lock_t lock (routed_send_ready_sync);
    return routed_send_ready_handler != NULL;
}

void zlink::socket_dispatch_bridge_t::store_routed_send_ready_handler (
  zlink_routed_send_ready_handler_fn handler_, void *userdata_)
{
    scoped_lock_t lock (routed_send_ready_sync);
    routed_send_ready_handler = handler_;
    routed_send_ready_userdata = userdata_;
}

bool zlink::socket_dispatch_bridge_t::load_routed_send_ready_handler (
  zlink_routed_send_ready_handler_fn *handler_out_, void **userdata_out_) const
{
    if (!handler_out_ || !userdata_out_)
        return false;

    scoped_lock_t lock (routed_send_ready_sync);
    *handler_out_ = routed_send_ready_handler;
    *userdata_out_ = routed_send_ready_userdata;
    return routed_send_ready_handler != NULL;
}

bool zlink::socket_dispatch_bridge_t::enqueue_routed_send_ready (
  const routed_send_target_key_t &key_,
  zlink_routed_send_ready_state_t state_,
  int terminal_errno_)
{
    scoped_lock_t lock (routed_send_ready_sync);
    if (!routed_send_ready_handler || key_.peer_rid.empty ())
        return false;

    if (state_ == ZLINK_ROUTED_SEND_TERMINAL) {
        if (!routed_send_ready_terminal.insert (key_).second)
            return false;
        routed_send_ready_pending[key_] =
          routed_send_ready_record_t (key_, state_, terminal_errno_);
        return true;
    }

    if (routed_send_ready_terminal.count (key_) != 0
        || routed_send_ready_pending.count (key_) != 0)
        return false;

    routed_send_ready_pending.insert (
      std::make_pair (key_, routed_send_ready_record_t (key_, state_, 0)));
    return true;
}

bool zlink::socket_dispatch_bridge_t::take_routed_send_ready (
  routed_send_ready_record_t *out_)
{
    if (!out_)
        return false;

    scoped_lock_t lock (routed_send_ready_sync);
    if (routed_send_ready_pending.empty ())
        return false;
    std::map<routed_send_target_key_t, routed_send_ready_record_t>::iterator it =
      routed_send_ready_pending.begin ();
    *out_ = it->second;
    routed_send_ready_pending.erase (it);
    return true;
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
