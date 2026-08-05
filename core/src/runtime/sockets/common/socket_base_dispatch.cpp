/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/c_api_copy_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "sockets/common/socket_dispatch_context.hpp"
#include "utils/err.hpp"
#include "utils/macros.hpp"

namespace
{
static void copy_routing_id (zlink_routing_id_t *out_, const zlink::blob_t &routing_id_)
{
    zlink::copy_routing_id_from_bytes (routing_id_.data (), routing_id_.size (), out_);
}
}

zlink::socket_recv_source_rid_scope_t::socket_recv_source_rid_scope_t (socket_base_t *socket_,
                                                                       bool enabled_) :
    _prev_socket (socket_recv_source_rid_context_t::current_socket ()),
    _prev_enabled (socket_recv_source_rid_context_t::current_enabled ())
{
    socket_recv_source_rid_context_t::set (socket_, enabled_);
}

zlink::socket_recv_source_rid_scope_t::~socket_recv_source_rid_scope_t ()
{
    socket_recv_source_rid_context_t::set (_prev_socket, _prev_enabled);
}

bool zlink::socket_base_t::recv_source_rid_capture_requested () const
{
    return socket_recv_source_rid_context_t::capture_requested (this);
}

int zlink::socket_base_t::stream_dispatch_msg_from_io (msg_t *msg_, pipe_t *pipe_)
{
    return xstream_dispatch_msg (msg_, pipe_);
}

int zlink::socket_base_t::socket_msg_dispatch_from_io (msg_t *msg_, pipe_t *pipe_)
{
    if (!socket_msg_dispatch_active ())
        return 0;

    if (options.type == ZLINK_CORE_SOCKET_STREAM) {
        socket_msg_dispatch_context_t context (NULL, pipe_, NULL, NULL);
        return xsocket_msg_dispatch (msg_, pipe_);
    }

    std::lock_guard<std::recursive_mutex> dispatch_lock (
      dispatch_runtime ().socket_msg_dispatch_sync);
    if (!socket_msg_dispatch_active ())
        return 0;
    socket_msg_dispatch_context_t context (NULL, pipe_, NULL, NULL);
    return xsocket_msg_dispatch (msg_, pipe_);
}

int zlink::socket_base_t::peer_command_from_io (msg_t *msg_, pipe_t *pipe_)
{
    std::lock_guard<std::recursive_mutex> dispatch_lock (
      dispatch_runtime ().socket_msg_dispatch_sync);
    pipe_t *socket_pipe = pipe_;
    if (socket_pipe && socket_pipe->get_peer ())
        socket_pipe = socket_pipe->get_peer ();
    return xpeer_command (msg_, socket_pipe);
}

int zlink::socket_base_t::socket_set_msg_handler (zlink_socket_msg_handler_fn handler_)
{
    return socket_set_msg_handler_ex (handler_, NULL);
}

int zlink::socket_base_t::socket_set_msg_handler_ex (zlink_socket_msg_handler_fn handler_,
                                                     void *subject_)
{
    return socket_set_msg_handler_with_userdata (handler_, subject_, NULL);
}

int zlink::socket_base_t::socket_set_msg_handler_with_userdata (
  zlink_socket_msg_handler_fn handler_, void *subject_, void *userdata_)
{
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    socket_public_api_scope_t admission (lifecycle);
    if (!admission.acquired ())
        return -1;
    if (!handler_) {
        errno = EINVAL;
        return -1;
    }
    if (socket_msg_dispatch_active ()) {
        errno = EBUSY;
        return -1;
    }

    io_thread_t *io_thread = choose_io_thread (options.affinity);
    if (!io_thread) {
        errno = EAGAIN;
        return -1;
    }

    dispatch_runtime ().socket_msg_handler.store (handler_, std::memory_order_release);
    dispatch_runtime ().socket_msg_handler_subject.store (subject_, std::memory_order_release);
    dispatch_runtime ().socket_msg_handler_userdata.store (userdata_, std::memory_order_release);
    const bool async_already_active =
      lifecycle_coordinator ().is_async_mailbox_active ();
    if (!async_already_active && start_async_mailbox_processing (io_thread) != 0) {
        dispatch_runtime ().socket_msg_handler.store (NULL, std::memory_order_release);
        dispatch_runtime ().socket_msg_handler_subject.store (NULL, std::memory_order_release);
        dispatch_runtime ().socket_msg_handler_userdata.store (NULL, std::memory_order_release);
        return -1;
    }
    _completion_async_owned = false;

    lifecycle_coordinator ().wait_async_started (1000);

    xarm_socket_msg_dispatch ();

    // Existing readable pipes may have been attached before dispatch mode was
    // enabled. Drain them once so typed callback users do not miss the first
    // message after installing the handler.
    xdispatch_io ();
    return 0;
}

int zlink::socket_base_t::ensure_completion_processing ()
{
    if (_completion_poller_refs.load (std::memory_order_acquire) != 0)
        return 0;
    if (lifecycle_coordinator ().is_async_mailbox_active ())
        return 0;
    io_thread_t *io_thread = choose_io_thread (options.affinity);
    if (!io_thread) {
        errno = EAGAIN;
        return -1;
    }
    if (start_async_mailbox_processing (io_thread) != 0)
        return -1;
    lifecycle_coordinator ().wait_async_started (1000);
    _completion_async_owned = true;
    return 0;
}

void zlink::socket_base_t::release_completion_processing ()
{
    if (!_completion_async_owned)
        return;
    if (lifecycle_coordinator ().is_async_mailbox_active ()) {
        stop_async_mailbox_processing ();
        wait_async_quiesced (10000);
    }
    _completion_async_owned = false;
}

void zlink::socket_base_t::acquire_completion_poller ()
{
    const uint32_t previous =
      _completion_poller_refs.fetch_add (1, std::memory_order_acq_rel);
    if (previous == 0)
        release_completion_processing ();
}

void zlink::socket_base_t::release_completion_poller ()
{
    const uint32_t previous =
      _completion_poller_refs.fetch_sub (1, std::memory_order_acq_rel);
    zlink_assert (previous > 0);
    if (previous == 1)
        resume_completion_processing_if_needed ();
}

void zlink::socket_base_t::notify_request_completion ()
{
    _request_completion_pending.store (true, std::memory_order_release);
    static_cast<mailbox_t *> (_mailbox)->signal ();
}

int zlink::socket_base_t::socket_msg_dispatch_stop ()
{
    if (!socket_msg_dispatch_active ()) {
        errno = EINVAL;
        return -1;
    }

    dispatch_runtime ().socket_msg_handler.store (NULL, std::memory_order_release);
    dispatch_runtime ().socket_msg_handler_subject.store (NULL, std::memory_order_release);
    dispatch_runtime ().socket_msg_handler_userdata.store (NULL, std::memory_order_release);

    {
        std::lock_guard<std::recursive_mutex> dispatch_lock (
          dispatch_runtime ().socket_msg_dispatch_sync);
    }

    if (lifecycle_coordinator ().is_async_mailbox_active ()) {
        stop_async_mailbox_processing ();
        if (current_async_mailbox_dispatch_socket () != this)
            wait_async_quiesced (10000);
    } else if (lifecycle_coordinator ().is_async_quiesce_pending ()) {
        if (current_async_mailbox_dispatch_socket () != this)
            wait_async_quiesced (10000);
    }

    return 0;
}

void zlink::socket_base_t::socket_msg_dispatch_drain_pending ()
{
    if (!socket_msg_dispatch_active ())
        return;

    std::lock_guard<std::recursive_mutex> dispatch_lock (
      dispatch_runtime ().socket_msg_dispatch_sync);
    if (socket_msg_dispatch_active ()) {
        xarm_socket_msg_dispatch ();
        xdispatch_io ();
    }
}

int zlink::socket_base_t::socket_set_send_ready_handler (zlink_send_ready_handler_fn handler_)
{
    return socket_set_send_ready_handler_ex (handler_, NULL);
}

int zlink::socket_base_t::socket_set_send_ready_handler_ex (zlink_send_ready_handler_fn handler_,
                                                            void *subject_)
{
    return socket_set_send_ready_handler_with_userdata (handler_, subject_, NULL);
}

int zlink::socket_base_t::socket_set_send_ready_handler_with_userdata (
  zlink_send_ready_handler_fn handler_, void *subject_, void *userdata_)
{
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    socket_public_api_scope_t admission (lifecycle);
    if (!admission.acquired ())
        return -1;
    if (!handler_) {
        errno = EINVAL;
        return -1;
    }
    if (socket_send_ready_dispatch_scope_t::dispatching_socket (this)) {
        errno = EDEADLK;
        return -1;
    }

    dispatch_bridge_t &dispatch = dispatch_runtime ();
    dispatch.store_send_ready_handler (handler_, subject_, userdata_);
    return 0;
}

bool zlink::socket_base_t::socket_msg_dispatch_active () const
{
    return dispatch_runtime ().socket_msg_handler.load (std::memory_order_acquire) != NULL;
}

bool zlink::socket_base_t::send_ready_handler_active () const
{
    return dispatch_runtime ().send_ready_handler.load (std::memory_order_acquire) != NULL;
}

zlink::socket_base_t *zlink::socket_base_t::current_socket_msg_dispatch_socket ()
{
    return socket_msg_dispatch_context_t::current_socket ();
}

zlink::socket_base_t *zlink::socket_base_t::current_send_ready_dispatch_socket ()
{
    return socket_send_ready_dispatch_scope_t::current_socket ();
}

zlink::pipe_t *zlink::socket_base_t::current_socket_msg_dispatch_pipe ()
{
    return socket_msg_dispatch_context_t::current_pipe ();
}

void *zlink::socket_base_t::current_socket_msg_dispatch_subject ()
{
    return socket_msg_dispatch_context_t::current_subject ();
}

bool zlink::socket_base_t::current_socket_msg_dispatch_source_rid (zlink_routing_id_t *out_)
{
    return socket_msg_dispatch_context_t::current_source_rid (out_);
}

void zlink::socket_base_t::invoke_send_ready_handler ()
{
    zlink_send_ready_handler_fn handler = NULL;
    void *subject = NULL;
    void *userdata = NULL;
    if (!dispatch_runtime ().load_send_ready_handler (&handler, &subject, &userdata))
        return;

    socket_callback_scope_t callback_scope (this);
    if (!callback_scope.acquired ())
        return;

    socket_send_ready_dispatch_scope_t dispatch_scope (this);
    handler (subject ? subject : this, userdata);
}

zlink_socket_msg_handler_fn zlink::socket_base_t::socket_msg_handler () const
{
    return dispatch_runtime ().socket_msg_handler.load (std::memory_order_acquire);
}

zlink_send_ready_handler_fn zlink::socket_base_t::socket_send_ready_handler () const
{
    zlink_send_ready_handler_fn handler = NULL;
    void *subject = NULL;
    void *userdata = NULL;
    if (!dispatch_runtime ().load_send_ready_handler (&handler, &subject, &userdata))
        return NULL;
    return handler;
}

void *zlink::socket_base_t::socket_msg_handler_subject () const
{
    return dispatch_runtime ().socket_msg_handler_subject.load (std::memory_order_acquire);
}

void *zlink::socket_base_t::socket_msg_handler_userdata () const
{
    return dispatch_runtime ().socket_msg_handler_userdata.load (std::memory_order_acquire);
}

void *zlink::socket_base_t::socket_send_ready_handler_subject () const
{
    zlink_send_ready_handler_fn handler = NULL;
    void *subject = NULL;
    void *userdata = NULL;
    if (!dispatch_runtime ().load_send_ready_handler (&handler, &subject, &userdata))
        return NULL;
    return subject;
}

void *zlink::socket_base_t::socket_send_ready_handler_userdata () const
{
    zlink_send_ready_handler_fn handler = NULL;
    void *subject = NULL;
    void *userdata = NULL;
    if (!dispatch_runtime ().load_send_ready_handler (&handler, &subject, &userdata))
        return NULL;
    return userdata;
}

void zlink::socket_base_t::invoke_socket_msg_handler (zlink_socket_msg_handler_fn handler_,
                                                      const zlink_routing_id_t *source_rid_,
                                                      zlink_msg_t *parts_,
                                                      size_t part_count_)
{
    socket_callback_scope_t callback_scope (this);
    if (!callback_scope.acquired ()) {
        for (size_t i = 0; i < part_count_; ++i) {
            const int rc = reinterpret_cast<msg_t *> (&parts_[i])->close ();
            errno_assert (rc == 0);
        }
        return;
    }
    socket_msg_dispatch_context_t context (this, socket_msg_dispatch_context_t::current_pipe (),
                                           socket_msg_handler_subject (), source_rid_);
    handler_ (source_rid_, parts_, part_count_, socket_msg_handler_userdata ());
}

void zlink::socket_base_t::close_socket_msg_parts (std::vector<zlink_msg_t> *parts_)
{
    if (!parts_)
        return;

    for (size_t i = 0; i < parts_->size (); ++i) {
        const int rc = reinterpret_cast<msg_t *> (&(*parts_)[i])->close ();
        errno_assert (rc == 0);
    }
    parts_->clear ();
}

void zlink::socket_base_t::resolve_socket_msg_source_rid (pipe_t *pipe_, zlink_routing_id_t *out_)
{
    if (!out_)
        return;

    memset (out_, 0, sizeof (*out_));
    if (!pipe_)
        return;

    const blob_t &pipe_routing_id = pipe_->get_routing_id ();
    if (pipe_routing_id.size () > 0) {
        copy_routing_id (out_, pipe_routing_id);
        return;
    }

    pipe_t *peer = pipe_->get_peer ();
    if (!peer)
        return;

    copy_routing_id (out_, peer->get_routing_id ());
}

void zlink::socket_base_t::store_last_recv_source_rid (pipe_t *pipe_)
{
    zlink_routing_id_t rid;
    resolve_socket_msg_source_rid (pipe_, &rid);
    store_last_recv_source_rid (&rid);
}

void zlink::socket_base_t::store_last_recv_source_rid (const zlink_routing_id_t *source_rid_)
{
    endpoint_runtime ().store_last_recv_source_rid (source_rid_);
}

void zlink::socket_base_t::clear_last_recv_source_rid ()
{
    endpoint_runtime ().clear_last_recv_source_rid ();
}

bool zlink::socket_base_t::copy_last_recv_source_rid (zlink_routing_id_t *out_) const
{
    return endpoint_runtime ().copy_last_recv_source_rid (out_);
}

void zlink::socket_base_t::arm_send_ready_notification ()
{
    dispatch_runtime ().arm_send_ready_notification ();
    notify_send_ready_if_armed ();
}

void zlink::socket_base_t::arm_send_ready_after_backpressure ()
{
    const bool was_pending = dispatch_runtime ().send_recovery_pending ();
    dispatch_runtime ().mark_send_recovery_pending ();
    if (!was_pending)
        static_cast<mailbox_t *> (_mailbox)->signal ();
    arm_send_ready_notification ();
}

void zlink::socket_base_t::notify_send_ready_if_armed ()
{
    if (!has_out ())
        return;

    if (!dispatch_runtime ().consume_send_ready_notification ())
        return;

    zlink_send_ready_handler_fn handler = socket_send_ready_handler ();
    if (handler) {
        invoke_send_ready_handler ();
    }
}

int zlink::socket_base_t::sub_dispatch_start (sub_io_handler_fn callback_, void *userdata_)
{
    LIBZLINK_UNUSED (callback_);
    LIBZLINK_UNUSED (userdata_);
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::sub_dispatch_stop ()
{
    errno = ENOTSUP;
    return -1;
}

bool zlink::socket_base_t::sub_dispatch_active () const
{
    return false;
}

int zlink::socket_base_t::xpub_dispatch_start ()
{
    errno = ENOTSUP;
    return -1;
}

bool zlink::socket_base_t::xpub_dispatch_active () const
{
    return false;
}

int zlink::socket_base_t::stream_dispatch_start_raw (zlink_stream_on_raw_fn)
{
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::stream_set_msg_handler_with_userdata (zlink_socket_msg_handler_fn, void *)
{
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::stream_set_packet_msg_handler_with_userdata (
  zlink_stream_packet_handler_fn, void *)
{
    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::stream_dispatch_stop ()
{
    return 0;
}

bool zlink::socket_base_t::stream_dispatch_active () const
{
    return false;
}

bool zlink::socket_base_t::stream_dispatch_in_callback () const
{
    return false;
}

uint32_t zlink::socket_base_t::stream_dispatch_inflight () const
{
    return 0;
}

int zlink::socket_base_t::stream_dispatch_send_from_io (const zlink_routing_id_t *,
                                                        const void *,
                                                        size_t,
                                                        int)
{
    return 0;
}

int zlink::socket_base_t::stream_dispatch_send_msg_from_io (const zlink_routing_id_t *,
                                                            msg_t *,
                                                            int)
{
    return 0;
}

int zlink::socket_base_t::stream_dispatch_send_current_msg_from_io (msg_t *, int)
{
    return 0;
}

int zlink::socket_base_t::xsocket_msg_dispatch (msg_t *msg_, pipe_t *pipe_)
{
    LIBZLINK_UNUSED (msg_);
    LIBZLINK_UNUSED (pipe_);
    return 0;
}

int zlink::socket_base_t::xstream_dispatch_msg (msg_t *msg_, pipe_t *pipe_)
{
    LIBZLINK_UNUSED (msg_);
    LIBZLINK_UNUSED (pipe_);
    return 0;
}

int zlink::socket_base_t::xpeer_command (msg_t *msg_, pipe_t *pipe_)
{
    uint32_t weight = 100;
    if (!decode_peer_weight_command (*msg_, &weight))
        return 0;
    return apply_peer_weight (pipe_, weight);
}

void zlink::socket_base_t::xlocal_peer_weight_changed ()
{
    broadcast_local_peer_weight ();
}

int zlink::socket_base_t::apply_peer_weight (pipe_t *pipe_, uint32_t weight_)
{
    LIBZLINK_UNUSED (pipe_);
    LIBZLINK_UNUSED (weight_);
    return 1;
}

void zlink::socket_base_t::broadcast_local_peer_weight ()
{
    std::vector<pipe_t *> pipes;
    snapshot_attached_pipes (&pipes);
    for (size_t i = 0; i < pipes.size (); ++i)
        send_local_peer_weight (pipes[i]);
}

void zlink::socket_base_t::send_local_peer_weight (pipe_t *pipe_)
{
    if (!pipe_)
        return;

    msg_t msg;
    if (msg.init () != 0)
        return;
    if (init_peer_weight_command (&msg, local_peer_weight ()) != 0) {
        const int close_rc = msg.close ();
        errno_assert (close_rc == 0);
        return;
    }
    const int rc = pipe_->write_and_flush (&msg);
    LIBZLINK_UNUSED (rc);
    const int close_rc = msg.close ();
    errno_assert (close_rc == 0);
}

void zlink::socket_base_t::xdispatch_io ()
{
}
