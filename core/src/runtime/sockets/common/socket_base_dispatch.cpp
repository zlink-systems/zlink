/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/c_api_copy_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "sockets/common/socket_dispatch_context.hpp"
#include "utils/err.hpp"
#include "utils/macros.hpp"
#include "utils/routing_id.hpp"

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

    int rc = 0;
    if (options.type == ZLINK_CORE_SOCKET_STREAM) {
        {
            socket_msg_dispatch_context_t context (NULL, pipe_, NULL, NULL);
            rc = xsocket_msg_dispatch (msg_, pipe_);
        }
    } else {
        std::lock_guard<std::recursive_mutex> dispatch_lock (
          dispatch_runtime ().socket_msg_dispatch_sync);
        if (socket_msg_dispatch_active ()) {
            // A session can retain the socket endpoint just before termination
            // wins. Recheck liveness under the same dispatch-order fence used
            // by deferred assembly teardown so a late frame cannot recreate
            // state after that teardown.
            if (pipe_ && !pipe_->is_lifecycle_active ())
                rc = 1;
            else {
                socket_msg_dispatch_context_t context (NULL, pipe_, NULL, NULL);
                rc = xsocket_msg_dispatch (msg_, pipe_);
            }
        }
    }
    // A callback can re-enter command processing and observe pipe termination.
    // Its teardown is skipped while the callback context is live, then safely
    // reclaimed here after the dispatch lock and callback-owned parts are gone.
    process_deferred_socket_msg_pipe_terminations ();
    return rc;
}

void zlink::socket_base_t::defer_socket_msg_dispatch ()
{
    dispatch_runtime ().deferred_socket_msg_dispatch_pending.store (
      true, std::memory_order_release);
}

void zlink::socket_base_t::defer_socket_msg_pipe_termination (pipe_t *pipe_)
{
    if (!pipe_ || !pipe_->retain_lifetime_ref ())
        return;

    dispatch_bridge_t &dispatch = dispatch_runtime ();
    scoped_lock_t lock (dispatch.deferred_socket_msg_termination_sync);
    zlink_assert (!pipe_->_deferred_socket_msg_termination_next);
    if (dispatch.deferred_socket_msg_termination_tail)
        dispatch.deferred_socket_msg_termination_tail
          ->_deferred_socket_msg_termination_next = pipe_;
    else
        dispatch.deferred_socket_msg_termination_head = pipe_;
    dispatch.deferred_socket_msg_termination_tail = pipe_;
}

void zlink::socket_base_t::process_deferred_socket_msg_pipe_terminations ()
{
    // Recursive command processing from a handler must not destroy the vector
    // backing that handler's callback arguments. The outer direct-dispatch
    // entry retries immediately after the callback returns.
    if (current_socket_msg_dispatch_socket () == this)
        return;

    for (;;) {
        pipe_t *pipe = NULL;
        dispatch_bridge_t &dispatch = dispatch_runtime ();
        {
            scoped_lock_t queue_lock (
              dispatch.deferred_socket_msg_termination_sync);
            pipe = dispatch.deferred_socket_msg_termination_head;
            if (!pipe)
                break;
            dispatch.deferred_socket_msg_termination_head =
              pipe->_deferred_socket_msg_termination_next;
            if (!dispatch.deferred_socket_msg_termination_head)
                dispatch.deferred_socket_msg_termination_tail = NULL;
            pipe->_deferred_socket_msg_termination_next = NULL;
        }

        {
            std::lock_guard<std::recursive_mutex> dispatch_lock (
              dispatch.socket_msg_dispatch_sync);
            xsocket_msg_pipe_terminated (pipe);
        }
        pipe->release_lifetime_ref ();
    }

    process_deferred_socket_msg_dispatch ();
}

void zlink::socket_base_t::process_deferred_socket_msg_dispatch ()
{
    if (current_socket_msg_dispatch_socket () == this)
        return;

    socket_dispatch_bridge_t &dispatch = dispatch_runtime ();
    while (dispatch.deferred_socket_msg_dispatch_pending.exchange (
      false, std::memory_order_acq_rel)) {
        {
            std::lock_guard<std::recursive_mutex> dispatch_lock (
              dispatch.socket_msg_dispatch_sync);
            if (socket_msg_dispatch_active ())
                xdispatch_io ();
        }

        // A callback may have re-entered command processing and queued pipe
        // teardown. It was intentionally skipped while the callback context
        // was live; reclaim it before dispatching another queued record.
        process_deferred_socket_msg_pipe_terminations ();
    }
}

int zlink::socket_base_t::peer_command_from_io (msg_t *msg_, pipe_t *pipe_)
{
    // This entry always receives the session endpoint. Snapshot and retain its
    // socket peer before taking the dispatch fence; detach_peer_link() may run
    // concurrently on the socket mailbox executor.
    pipe_t *const socket_pipe = pipe_ ? pipe_->retain_peer_snapshot () : NULL;
    if (pipe_ && !socket_pipe)
        return 1;
    std::lock_guard<std::recursive_mutex> dispatch_lock (
      dispatch_runtime ().socket_msg_dispatch_sync);
    if (socket_pipe && !socket_pipe->is_lifecycle_active ()) {
        socket_pipe->release_lifetime_ref ();
        return 1;
    }
    const int rc = xpeer_command (msg_, socket_pipe);
    const int saved_errno = errno;
    if (socket_pipe)
        socket_pipe->release_lifetime_ref ();
    errno = saved_errno;
    return rc;
}

int zlink::socket_base_t::ensure_completion_processing ()
{
    scoped_lock_t owner_lock (_completion_owner_sync);
    if (_completion_poller_refs.load (std::memory_order_acquire) != 0)
        return 0;
    retain_async_command_processing ();
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
    return 0;
}

void zlink::socket_base_t::acquire_completion_poller ()
{
    bool quiesce_async_owner = false;
    {
        //  The owner gate fences an in-flight completion drain before the
        //  first public poller registration returns.
        scoped_lock_t owner_lock (_completion_owner_sync);
        const uint32_t previous =
          _completion_poller_refs.fetch_add (1, std::memory_order_acq_rel);
        quiesce_async_owner =
          previous == 0
          && lifecycle_coordinator ().is_async_mailbox_active ()
          && current_async_mailbox_dispatch_socket () != this
          && !socket_msg_dispatch_active ()
          && !send_complete_handler_active ();
        if (quiesce_async_owner)
            stop_async_mailbox_processing ();
    }
    //  With no async callback consumer, the public poller can process mailbox
    //  commands and completions itself.  Quiescing the second mailbox owner
    //  also keeps normal receive readiness single-threaded while the poller
    //  registration exists.
    if (quiesce_async_owner)
        wait_async_quiesced (10000);
}

void zlink::socket_base_t::release_completion_poller ()
{
    bool resume = false;
    {
        scoped_lock_t owner_lock (_completion_owner_sync);
        const uint32_t previous =
          _completion_poller_refs.fetch_sub (1, std::memory_order_acq_rel);
        zlink_assert (previous > 0);
        resume = previous == 1;
    }
    if (resume)
        resume_completion_processing_if_needed ();
}

bool zlink::socket_base_t::acquire_poller_registration ()
{
    return lifecycle_coordinator ().acquire_poller_registration ();
}

void zlink::socket_base_t::release_poller_registration ()
{
    const bool has_remaining_refs = lifecycle_coordinator ().release_poller_registration ();
    if (!has_remaining_refs && lifecycle_coordinator ().is_destroy_pending ())
        check_destroy ();
}

void zlink::socket_base_t::notify_request_completion ()
{
    // One pending command is sufficient until the completion owner consumes
    // it. A real mailbox command, rather than signal() alone, also schedules
    // the async owner and closes the enqueue-vs-reschedule lost-wake window.
    if (_request_completion_pending.exchange (true, std::memory_order_acq_rel))
        return;
    command_t wake;
    memset (&wake, 0, sizeof (wake));
    wake.destination = this;
    wake.type = command_t::request_completion;
    static_cast<mailbox_t *> (_mailbox)->send (wake);
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

    if (lifecycle_coordinator ().is_async_mailbox_active ()
        && !send_complete_handler_active ()) {
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
    {
        scoped_lock_t receive_lock (receive_runtime ().sync);
        if (!socket_msg_dispatch_active ())
            return;
        xarm_socket_msg_dispatch ();
        defer_socket_msg_dispatch ();
    }
    process_deferred_socket_msg_pipe_terminations ();
}

bool zlink::socket_base_t::socket_msg_dispatch_active () const
{
    return dispatch_runtime ().socket_msg_handler.load (std::memory_order_acquire) != NULL;
}

zlink::socket_base_t *zlink::socket_base_t::current_socket_msg_dispatch_socket ()
{
    return socket_msg_dispatch_context_t::current_socket ();
}

zlink::socket_base_t *zlink::socket_base_t::current_send_complete_dispatch_socket ()
{
    return socket_send_complete_dispatch_scope_t::current_socket ();
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

zlink_socket_msg_handler_fn zlink::socket_base_t::socket_msg_handler () const
{
    return dispatch_runtime ().socket_msg_handler.load (std::memory_order_acquire);
}

void *zlink::socket_base_t::socket_msg_handler_subject () const
{
    return dispatch_runtime ().socket_msg_handler_subject.load (std::memory_order_acquire);
}

void *zlink::socket_base_t::socket_msg_handler_userdata () const
{
    return dispatch_runtime ().socket_msg_handler_userdata.load (std::memory_order_acquire);
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
    // Socket-message handlers are a public raw receive boundary. Their
    // assemblers reject request/reply metadata on continuation frames, so
    // sanitize only the first application frame that can carry it.
    if (part_count_ != 0)
        reinterpret_cast<msg_t *> (&parts_[0])->reset_request_reply_metadata ();
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

void zlink::socket_base_t::arm_send_recovery_after_backpressure ()
{
    const bool was_pending = dispatch_runtime ().send_recovery_pending ();
    dispatch_runtime ().mark_send_recovery_pending ();
    if (!was_pending)
        static_cast<mailbox_t *> (_mailbox)->signal ();
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

int zlink::socket_base_t::stream_mark_raw_part_receive ()
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

void zlink::socket_base_t::xsocket_msg_pipe_terminated (pipe_t *pipe_)
{
    LIBZLINK_UNUSED (pipe_);
}

int zlink::socket_base_t::xstream_dispatch_msg (msg_t *msg_, pipe_t *pipe_)
{
    LIBZLINK_UNUSED (msg_);
    LIBZLINK_UNUSED (pipe_);
    return 0;
}

int zlink::socket_base_t::xpeer_command (msg_t *msg_, pipe_t *pipe_)
{
    //  Completion-lane flow state is Core internal. It arrives as a command
    //  frame, so the session never enqueues it on a pipe an application can
    //  read from.
    if (consume_receive_flow_state_frame (pipe_, *msg_))
        return 1;

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
    //  Peer weight controls application-lane scheduling only. Completion is
    //  reserved for replies and receive-flow control; unlike a network
    //  session, an inproc completion pipe has no command interceptor.
    if (pipe_->get_transport_pair_id () != 0
        && pipe_->get_transport_lane () == transport_lane_completion)
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
