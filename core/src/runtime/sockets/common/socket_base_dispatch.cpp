/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "protocol/zmp_peer_weight.hpp"

#include "core/c_api_copy_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"
#include "utils/macros.hpp"
#include "utils/routing_id.hpp"

namespace
{
#ifdef ZLINK_BUILD_TESTS
std::atomic<uint64_t> g_local_peer_weight_send_attempt_count (0);
#endif

static void copy_routing_id (zlink_routing_id_t *out_, const zlink::blob_t &routing_id_)
{
    zlink::copy_routing_id_from_bytes (routing_id_.data (), routing_id_.size (), out_);
}
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

        xsocket_msg_pipe_terminated (pipe);
        pipe->release_lifetime_ref ();
    }
}

int zlink::socket_base_t::peer_command_from_io (msg_t *msg_, pipe_t *pipe_)
{
    // This entry always receives the session endpoint. Snapshot and retain its
    // socket peer before delivery; detach_peer_link() may run concurrently on
    // the socket mailbox executor.
    pipe_t *const socket_pipe = pipe_ ? pipe_->retain_peer_snapshot () : NULL;
    if (pipe_ && !socket_pipe)
        return 1;
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

bool zlink::socket_base_t::has_stable_completion_processing_owner () const
{
    const uint32_t generation =
      _completion_processing_owner_generation.load (std::memory_order_acquire);
    return (generation & 1u) != 0
           && _completion_processing_owner_generation.load (
                std::memory_order_acquire)
                == generation;
}

// Stable publications follow a canonical owner-field recheck while holding
// _completion_owner_sync. Invalidation is allowed to be conservative: the
// next request falls back to both owner locks and republishes the live owner.
void zlink::socket_base_t::invalidate_completion_processing_owner ()
{
    uint32_t generation =
      _completion_processing_owner_generation.load (std::memory_order_relaxed);
    while ((generation & 1u) != 0
           && !_completion_processing_owner_generation.compare_exchange_weak (
             generation, generation + 1, std::memory_order_acq_rel,
             std::memory_order_relaxed)) {
    }
}

void zlink::socket_base_t::publish_completion_processing_owner ()
{
    uint32_t generation =
      _completion_processing_owner_generation.load (std::memory_order_relaxed);
    while ((generation & 1u) == 0
           && !_completion_processing_owner_generation.compare_exchange_weak (
             generation, generation + 1, std::memory_order_release,
             std::memory_order_relaxed)) {
    }
}

int zlink::socket_base_t::ensure_completion_processing ()
{
    if (has_stable_completion_processing_owner ())
        return 0;

    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    for (;;) {
        bool wait_for_quiescence = false;
        bool started_here = false;
        {
            scoped_lock_t owner_lock (_completion_owner_sync);
            scoped_lock_t progress_lock (
              _transport_pair_owner_progress_sync);

            // A public completion owner can be between its stop request and
            // the old executor's physical mailbox detach. Do not treat either
            // its poller reference or the inactive lifecycle bit as a stable
            // owner until that detach has published completion.
            if (lifecycle.is_async_quiesce_pending ()) {
                wait_for_quiescence = true;
            } else if (_completion_poller_refs.load (
                         std::memory_order_acquire)
                       != 0) {
                publish_completion_processing_owner ();
                return 0;
            } else {
                // The progress gate keeps an active owner from entering its
                // idle detach while we publish the retained lease. Keep this
                // steady request path to the same release store as before;
                // only the cold owner-start path needs the prior value for a
                // failure rollback.
                if (lifecycle.is_async_mailbox_active ()) {
                    _async_command_processing_retained.store (
                      true, std::memory_order_release);
                    _async_command_processing_stop_requested = false;
                    publish_completion_processing_owner ();
                    return 0;
                }

                const bool retained_before =
                  _async_command_processing_retained.load (
                    std::memory_order_acquire);
                _async_command_processing_retained.store (
                  true, std::memory_order_release);
                _async_command_processing_stop_requested = false;

                io_thread_t *io_thread = choose_io_thread (options.affinity);
                if (!io_thread) {
                    // Retention is a lease on a usable command owner, not a
                    // record that an owner start was merely attempted. Restore
                    // the prior state so a later 0 -> 1 pending transition can
                    // retry acquisition after this cold failure.
                    if (!retained_before)
                        _async_command_processing_retained.store (
                          false, std::memory_order_release);
                    errno = EAGAIN;
                    return -1;
                }
                if (start_async_mailbox_processing (io_thread) != 0) {
                    const int start_errno = errno;
                    if (!retained_before)
                        _async_command_processing_retained.store (
                          false, std::memory_order_release);
                    errno = start_errno;
                    return -1;
                }
                publish_completion_processing_owner ();
                started_here = true;
            }
        }

        if (wait_for_quiescence) {
            wait_async_quiesced (10000);
            if (lifecycle.is_async_quiesce_pending ()) {
                errno = EBUSY;
                return -1;
            }
            continue;
        }
        if (started_here)
            lifecycle.wait_async_started (1000);
        return 0;
    }
}

bool zlink::socket_base_t::acquire_completion_poller (void *owner_)
{
    if (!owner_) {
        errno = EFAULT;
        return false;
    }
    bool quiesce_async_owner = false;
    {
        //  The owner gate fences an in-flight completion drain before the
        //  first public poller registration returns.
        scoped_lock_t owner_lock (_completion_owner_sync);
        void *expected = NULL;
        if (!_completion_poller_owner.compare_exchange_strong (
              expected, owner_, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
            errno = EBUSY;
            return false;
        }
        quiesce_async_owner =
          lifecycle_coordinator ().is_async_mailbox_active ()
          && current_async_mailbox_dispatch_socket () != this
          && stop_async_mailbox_processing ();
        if (quiesce_async_owner)
            invalidate_completion_processing_owner ();

        const uint32_t previous =
          _completion_poller_refs.fetch_add (1, std::memory_order_acq_rel);
        zlink_assert (previous == 0);
        if (!quiesce_async_owner) {
            publish_completion_processing_owner ();
        }
    }
    // A monitor must keep processing commands
    // even when the application never waits on its completion poller. The
    // owner gate and completion reference fence only completion drain; the
    // async executor already skips that drain while a public poller owns it.
    // Without a monitor the executor can still hand commands to the poller.
    if (quiesce_async_owner) {
        wait_async_quiesced (10000);
        // The wait runs outside the drain fence. Recheck the exact owner and
        // completed detach under that fence before making it visible to the
        // request fast path.
        scoped_lock_t owner_lock (_completion_owner_sync);
        if (_completion_poller_owner.load (std::memory_order_acquire) == owner_
            && _completion_poller_refs.load (std::memory_order_acquire) != 0
            && !lifecycle_coordinator ().is_async_quiesce_pending ())
            publish_completion_processing_owner ();
    }
    errno = 0;
    return true;
}

void zlink::socket_base_t::release_completion_poller (void *owner_)
{
    bool resume = false;
    {
        scoped_lock_t owner_lock (_completion_owner_sync);
        void *expected = owner_;
        if (_completion_poller_owner.load (std::memory_order_acquire)
            != expected)
            return;
        // Invalidate while the old owner is still canonical. A fast request
        // that observed the preceding odd generation therefore linearizes
        // before this handoff, never in the ownerless pointer/ref interval.
        invalidate_completion_processing_owner ();
        if (!_completion_poller_owner.compare_exchange_strong (
              expected, NULL, std::memory_order_acq_rel,
              std::memory_order_acquire))
            return;
        const uint32_t previous =
          _completion_poller_refs.fetch_sub (1, std::memory_order_acq_rel);
        zlink_assert (previous == 1);
        resume = previous == 1;
        if (resume && lifecycle_coordinator ().is_async_mailbox_active ()
            && !lifecycle_coordinator ().is_async_quiesce_pending ()) {
            scoped_lock_t progress_lock (
              _transport_pair_owner_progress_sync);
            if (lifecycle_coordinator ().is_async_mailbox_active ()
                && !lifecycle_coordinator ().is_async_quiesce_pending ()
                && _async_command_processing_retained.load (
                     std::memory_order_acquire))
                publish_completion_processing_owner ();
        }
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
    const bool already_pending =
      _request_completion_pending.exchange (true, std::memory_order_acq_rel);
    if (already_pending)
        return;
    command_t wake;
    memset (&wake, 0, sizeof (wake));
    wake.destination = this;
    wake.type = command_t::request_completion;
    static_cast<mailbox_t *> (_mailbox)->send (wake);
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

const zlink_routing_id_t *zlink::socket_base_t::last_recv_source_rid_view () const
{
    return endpoint_runtime ().last_recv_source_rid_valid
             ? &endpoint_runtime ().last_recv_source_rid
             : NULL;
}

void zlink::socket_base_t::arm_send_recovery_after_backpressure ()
{
    const bool was_pending = dispatch_runtime ().send_recovery_pending ();
    const bool was_ready = dispatch_runtime ().send_recovery_ready ();
    dispatch_runtime ().mark_send_recovery_pending ();
    // The writable edge may have been consumed by a temporary mailbox owner
    // immediately before this EAGAIN armed recovery. Recheck the transport
    // after publishing pending so that state becomes a level-ready poll edge
    // instead of waiting for a second pipe transition that may never come.
    if (transport_has_out ())
        dispatch_runtime ().mark_send_recovery_ready ();
    if (!was_pending
        || (!was_ready && dispatch_runtime ().send_recovery_ready ()))
        static_cast<mailbox_t *> (_mailbox)->signal ();
}

int zlink::socket_base_t::stream_mark_raw_part_receive ()
{
    errno = ENOTSUP;
    return -1;
}

void zlink::socket_base_t::xsocket_msg_pipe_terminated (pipe_t *pipe_)
{
    LIBZLINK_UNUSED (pipe_);
}

int zlink::socket_base_t::xpeer_command (msg_t *msg_, pipe_t *pipe_)
{
    //  Completion-lane flow state is Core internal. It arrives as a command
    //  frame, so the session never enqueues it on a pipe an application can
    //  read from.
    if (consume_receive_flow_state_frame (pipe_, *msg_))
        return 1;

    uint32_t weight = 100;
    const zmp_peer_weight::decode_result_t decoded =
      zmp_peer_weight::decode_command (*msg_, &weight);
    if (decoded == zmp_peer_weight::decode_not_weight_command)
        return 0;
    // Recognized but malformed/out-of-policy WEIGHT remains an internal
    // no-op. The transport stays usable and the frame can never surface as
    // Application data, matching FLOWSTATE command ownership.
    if (decoded != zmp_peer_weight::decode_ok || weight > max_peer_weight)
        return 1;
    if (!pipe_)
        return 1;
    const uint64_t connection_id = pipe_->get_transport_connection_id ();
    // Cache at the physical decode boundary before a following direct RID can
    // publish this route. The owner command remains the only dynamic scheduler
    // mutation path; this record only supplies its generation-tagged initial
    // value to a not-yet-visible scheduler entry.
    if (!pipe_->record_peer_weight_if_current (connection_id, weight))
        return 1;
    //  Network decoding runs on an I/O thread. Reuse the same exact-pipe
    //  owner command as inproc so every scheduler mutation, pending-state
    //  promotion and monitor event is serialized by the socket mailbox.
    (void) send_peer_weight (
      pipe_, weight, connection_id);
    return 1;
}

void zlink::socket_base_t::peer_weight_received (pipe_t *pipe_,
                                                 uint32_t weight_)
{
    (void) accept_peer_weight (pipe_, weight_);
}

int zlink::socket_base_t::accept_peer_weight (pipe_t *pipe_, uint32_t weight_)
{
    if (!pipe_ || weight_ > max_peer_weight
        || pipe_->get_transport_lane () != transport_lane_application)
        return 1;

    const uint64_t pair_id = pipe_->get_transport_pair_id ();
    const uint64_t pair_generation =
      pipe_->get_transport_pair_generation ();
    const uint64_t source_connection_id =
      pipe_->get_transport_connection_id ();
    if (pair_id != 0
        && (pair_generation == 0 || source_connection_id == 0))
        return 1;

    if (pair_id == 0)
        return apply_peer_weight (pipe_, weight_);

    bool apply_now = false;
    {
        scoped_lock_t pair_lock (_transport_pairs_sync);
        const transport_pair_key_t key (pair_id, pair_generation);
        const transport_pairs_t::const_iterator it =
          _transport_pairs.find (key);
        apply_now = it != _transport_pairs.end () && it->second.ready
                    && it->second.application == pipe_
                    && pipe_->is_lifecycle_active ();
    }
    return apply_now ? apply_peer_weight (pipe_, weight_) : 1;
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

bool zlink::socket_base_t::recorded_peer_weight_ready_locked (
  pipe_t *pipe_, uint32_t *weight_out_) const
{
    if (!pipe_ || pipe_->get_transport_lane () != transport_lane_application)
        return false;

    const uint64_t pair_id = pipe_->get_transport_pair_id ();
    if (pair_id != 0) {
        const uint64_t generation =
          pipe_->get_transport_pair_generation ();
        scoped_lock_t pair_lock (_transport_pairs_sync);
        const transport_pairs_t::const_iterator it =
          _transport_pairs.find (transport_pair_key_t (pair_id, generation));
        if (generation == 0 || it == _transport_pairs.end ()
            || !it->second.ready || it->second.application != pipe_
            || !pipe_->is_lifecycle_active ())
            return false;
    }

    uint32_t weight = 100;
    if (!pipe_->peer_weight (&weight))
        return false;
    if (weight_out_)
        *weight_out_ = weight;
    return true;
}

void zlink::socket_base_t::initialize_recorded_peer_weight (pipe_t *pipe_)
{
    if (!pipe_)
        return;
    scoped_fast_lock_t generation_lock (pipe_->transport_sync ());
    uint32_t weight = 100;
    if (recorded_peer_weight_ready_locked (pipe_, &weight))
        initialize_peer_weight (pipe_, weight);
}

void zlink::socket_base_t::initialize_peer_weight (pipe_t *pipe_,
                                                   uint32_t weight_)
{
    LIBZLINK_UNUSED (pipe_);
    LIBZLINK_UNUSED (weight_);
}

void zlink::socket_base_t::broadcast_local_peer_weight ()
{
    std::vector<pipe_t *> pipes;
    snapshot_attached_pipes (&pipes);
    for (size_t i = 0; i < pipes.size (); ++i)
        (void) send_local_peer_weight (pipes[i]);
}

bool zlink::socket_base_t::deliver_local_peer_weight (pipe_t *pipe_,
                                                      uint32_t weight_)
{
    if (!pipe_ || !pipe_->is_lifecycle_active ())
        return false;

    //  A session consumes ZMP command frames before application delivery.
    //  Paired inproc still uses the same pipe-owned boundary staging, but
    //  materialises its surviving controls as owner commands after preceding
    //  Application records are published. Unpaired inproc has no multipart
    //  pair policy and keeps its direct owner-command path.
    if (!pipe_->is_session_pipe ()) {
        if (pipe_->get_transport_pair_id () != 0) {
            if (pipe_->get_transport_pair_generation () == 0
                || (pipe_->get_transport_lane_count () != 1u
                    && pipe_->get_transport_lane_count () != 2u))
                return false;
            const bool defer =
              lifecycle_coordinator ().public_multipart_send_active ();
            const bool sent =
              pipe_->write_peer_weight_control_and_flush (weight_, defer);
            if (sent && defer)
                mark_deferred_peer_controls ();
            return sent;
        }
        pipe_t *const peer = pipe_->retain_peer_snapshot ();
        if (!peer)
            return false;
        const bool sent = send_peer_weight (
          peer, weight_, pipe_->get_transport_connection_id ());
        peer->release_lifetime_ref ();
        return sent;
    }

    const bool defer =
      lifecycle_coordinator ().public_multipart_send_active ();
    const bool sent =
      pipe_->write_peer_weight_control_and_flush (weight_, defer);
    if (sent && defer)
        mark_deferred_peer_controls ();
    return sent;
}

bool zlink::socket_base_t::send_local_peer_weight (pipe_t *pipe_)
{
#ifdef ZLINK_BUILD_TESTS
    g_local_peer_weight_send_attempt_count.fetch_add (
      1, std::memory_order_relaxed);
#endif
    if (!pipe_ || pipe_->get_transport_lane () != transport_lane_application)
        return false;

    const uint64_t pair_id = pipe_->get_transport_pair_id ();
    if (pair_id == 0) {
        const bool sent =
          deliver_local_peer_weight (pipe_, local_peer_weight ());
        if (sent)
            flush_deferred_peer_controls ();
        return sent;
    }

    const uint64_t pair_generation =
      pipe_->get_transport_pair_generation ();
    if (pair_generation == 0)
        return false;
    {
        scoped_lock_t pair_lock (_transport_pairs_sync);
        const transport_pair_key_t key (pair_id, pair_generation);
        const transport_pairs_t::iterator it = _transport_pairs.find (key);
        if (it == _transport_pairs.end () || !it->second.ready
            || it->second.application != pipe_
            || !pipe_->transport_pair_writes_released ())
            return false;
        const uint32_t weight = local_peer_weight ();
        if (it->second.local_peer_weight_advertised == weight)
            return true;

        // Holding the pair policy record across enqueue/write serializes a
        // pair-ready resync with a concurrent dynamic option update. Deferred
        // control flushing itself happens after this table lock is released.
        if (!deliver_local_peer_weight (pipe_, weight))
            return false;
        it->second.local_peer_weight_advertised = weight;
    }
    // FINAL may have released the multipart boundary between the pipe write
    // and flag publication. Recheck synchronously once no pair-table lock is
    // held; the FINAL path owns the complementary ordering.
    flush_deferred_peer_controls ();
    return true;
}

#ifdef ZLINK_BUILD_TESTS
uint64_t zlink::socket_base_t::test_local_peer_weight_send_attempt_count ()
{
    return g_local_peer_weight_send_attempt_count.load (
      std::memory_order_acquire);
}
#endif
