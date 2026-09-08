/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include <stdio.h>
#include <new>
#include <stddef.h>

#include "utils/macros.hpp"
#include "utils/config.hpp"
#include "core/pipe_internal.hpp"
#include "core/transport_pair_policy.hpp"
#include "core/ctx.hpp"
#include "core/flow_state_frame.hpp"
#include "protocol/zmp_protocol.hpp"
#include "protocol/zmp_peer_weight.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"
#include "utils/heap_owner.hpp"
#include "core/ypipe.hpp"
#include "core/ypipe_conflate.hpp"

using zlink::pipe_detail::consume_if_delimiter;
using zlink::pipe_detail::head_reclassify_armed;
using zlink::pipe_detail::head_reclassify_idle;
using zlink::pipe_detail::head_reclassify_queued;
using zlink::pipe_detail::pipe_debug_log;

void zlink::pipe_t::rollback ()
{
    rollback_unlocked ();
}

bool zlink::pipe_t::rollback_incomplete ()
{
    const bool incomplete =
      _out_incomplete_bytes != 0 || _out_incomplete_payload_bytes != 0
      || _out_multipart_started_empty || _out_owner_message_started
      || _out_owner_message_start_pending;
    if (incomplete)
        rollback_unlocked ();
    return incomplete;
}

void zlink::pipe_t::flush ()
{
    // Hot path: single-part send flushes on every completed message under the
    // endpoint's existing C2 owner turn.
    flush_unlocked ();
}

void zlink::pipe_t::process_activate_read ()
{
    // Hot path: every message that finds the inbound reader asleep runs this
    // command. `_state` and `_in_active` are the inbound receive cluster:
    // check_read()/read() test and assign exactly these two members with no
    // `_out_sync` held (see the identical guard at the top of check_read()).
    // `_out_sync` therefore protects nothing here, so the wake-or-not
    // decision is taken without it. Socket endpoints serialize both accesses
    // with their lifecycle turn; session endpoints publish the wake through
    // the atomic members. The lock is kept only for the head-reclassify
    // branch below, which reads the outbound topology cluster.
    const lifecycle_state_t state = _state.load (std::memory_order_acquire);
    if (state != active && state != waiting_for_delimiter)
        return;
    if (!_in_active.load (std::memory_order_acquire)) {
        _in_active.store (true, std::memory_order_release);
        _sink->read_activated (this);
        return;
    }
    //  An awake reader only needs a wake when a count-1 head reclassification
    //  was armed. That marker is atomic, so the common "nothing armed" answer
    //  costs one acquire load instead of a lock round trip.
    if (likely (_head_reclassify_wake.load (std::memory_order_acquire)
                == head_reclassify_idle))
        return;
    bool notify = false;
    {
        scoped_lock_t lock (_out_sync);
        if ((_state == active || _state == waiting_for_delimiter)
            && _in_active.load (std::memory_order_acquire)
            && _head_reclassify_wake.load (std::memory_order_acquire)
                 != head_reclassify_idle
            && peer_uses_routed_protocol_unlocked () && _transport_pair_id != 0
            && _transport_lane == transport_lane_application
            && _transport_lane_count.load (std::memory_order_acquire) == 1u
            && (!_session_pipe || !_session_io_writer)) {
            notify = _head_reclassify_wake.exchange (
                       head_reclassify_idle, std::memory_order_acq_rel)
                     != head_reclassify_idle;
        }
    }
    if (notify)
        _sink->read_activated (this);
}

void zlink::pipe_t::process_activate_write (uint64_t generation_,
                                            uint64_t msgs_read_,
                                            uint64_t bytes_read_)
{
    bool notify = false;
    bool correlation_released = false;
    {
        scoped_lock_t lock (_out_sync);

        // Correlation release uses this command only to cross the owner
        // mailbox. Its wake remains valid even if a reconnect advanced the
        // byte-credit generation before the owner processes the command.
        if (_request_correlation_activation_pending) {
            _request_correlation_activation_pending = false;
            correlation_released = true;
            // Preserve the cause until the socket consumes the corresponding
            // write activation. An ordinary send may have succeeded after the
            // request was rejected, but that must not consume this wake.
            _request_correlation_recovery.store (true,
                                                  std::memory_order_release);
            notify = _state == active
                     && _out_active.load (std::memory_order_acquire)
                     && !_transport_pair_write_held
                     && !remote_flow_blocked_unlocked ()
                     && check_hwm_unlocked ();
        }

        if (generation_ == _out_generation) {
            //  Remember the peer's message sequence number.
            _peers_msgs_read.store (msgs_read_, std::memory_order_release);
            _peers_bytes_read.store (bytes_read_, std::memory_order_release);

            bool expected = false;
            if (!_transport_pair_write_held && _state == active
                && check_hwm_unlocked ()
                && _out_active.compare_exchange_strong (
                  expected, true, std::memory_order_acq_rel,
                  std::memory_order_acquire)) {
                //  Byte credit removes only the HWM cause. While the peer keeps
                //  this pipe PAUSED the send-ready edge stays suppressed; the
                //  resume transition publishes it once every cause is clear.
                notify = !remote_flow_blocked_unlocked ();
            }
        }
    }

    if (correlation_released)
        _sink->request_correlation_released (this);
    if (notify)
        _sink->write_activated (this);
}

void zlink::pipe_t::process_hiccup (void *pipe_, uint64_t generation_)
{
    bool notify = false;
    {
        scoped_lock_t lock (_out_sync);

        //  Destroy old outpipe. Note that the read end of the pipe was already
        //  migrated to this thread.
        zlink_assert (_out_pipe);
        _out_pipe->flush ();
        msg_t msg;
        while (_out_pipe->read (&msg)) {
            const uint64_t frame_bytes = frame_accounted_bytes (&msg);
            if (!msg.is_delimiter () && _registry_accounting)
                get_ctx ()->_physical_queue_registry.release_committed_frame (
                  _out_physical_queue, frame_bytes,
                  counted_pending_message_ref (msg));
            const int rc = msg.close ();
            errno_assert (rc == 0);
        }
        release_discarded_pipe_accounting (_out_pipe,
                                           _out_physical_queue);
        const uint64_t incomplete_bytes = _out_incomplete_bytes;
        _out_incomplete_bytes = 0;
        _out_incomplete_payload_bytes = 0;
        _out_multipart_started_empty = false;
        //  A hiccup discards the outbound queue, including whatever the owner
        //  had accepted for the message in progress.
        _out_owner_message_started = false;
        _out_owner_message_start_pending = false;
        _decoder_multipart_started_empty = false;
        // A deferred command belongs to the discarded transport generation.
        // The session owner resynchronizes the current absolute policy after
        // publishing the replacement connection id.
        discard_pending_peer_controls_unlocked ();
        if (incomplete_bytes > 0 && _registry_accounting)
            get_ctx ()->_physical_queue_registry.rollback_provisional (
              _out_physical_queue, incomplete_bytes);
        LIBZLINK_DELETE (_out_pipe);

        //  Plug in the new outpipe.
        zlink_assert (pipe_);
        _out_pipe = static_cast<upipe_t *> (pipe_);
        _out_active.store (!_transport_pair_write_held,
                           std::memory_order_release);
        _out_generation = generation_;
        _out_complete_record_pending = false;
        publish_outbound_ledger_unlocked (0, 0);
        _peers_msgs_read.store (0, std::memory_order_release);
        _peers_bytes_read.store (0, std::memory_order_release);
        _waiting_for_byte_credit.store (false, std::memory_order_release);
        publish_outbound_accounting_unlocked (true);

        //  If appropriate, notify the user about the hiccup.
        notify = (_state == active && !_transport_pair_write_held);
    }

    if (notify)
        _sink->hiccuped (this);
}

void zlink::pipe_t::transition_to_inactive_state_unlocked (
  lifecycle_state_t state_)
{
    zlink_assert (state_ != active);
    _state.store (state_, std::memory_order_release);
}

void zlink::pipe_t::acknowledge_peer_termination_unlocked (
  lifecycle_state_t state_)
{
    transition_to_inactive_state_unlocked (state_);
    _out_pipe = NULL;
    (void) send_pipe_term_ack (get_peer ());
}

void zlink::pipe_t::process_pipe_term ()
{
    bool drain_complete = false;
    {
        scoped_lock_t lock (_out_sync);
        pipe_debug_log (this, "process_pipe_term", _state, _delay,
                        _endpoint_pair.identifier ().c_str ());

        //  Peer-induced termination is logically one-shot. During cascading
        //  socket teardown we can observe a duplicate term command after
        //  we've already transitioned into a peer-terminated state; treat that as
        //  an idempotent no-op instead of asserting.
        if (_state == waiting_for_delimiter) {
            return;
        }
        if (_state == term_ack_sent || _state == term_req_sent2) {
            (void) send_pipe_term_ack (get_peer ());
            return;
        }

        zlink_assert (_state == active || _state == delimiter_received || _state == term_req_sent1);

        //  This is the simple case of peer-induced termination. If there are no
        //  more pending messages to read, or if the pipe was configured to drop
        //  pending messages, we can move directly to the term_ack_sent state.
        //  Otherwise we'll hang up in waiting_for_delimiter state till all
        //  pending messages are read.
        if (_state == active) {
            bool pending_to_read = false;
            if (_in_pipe) {
                msg_t delimiter;
                const ypipe_read_result_t delimiter_result =
                  _in_pipe->read_if (&delimiter, &consume_if_delimiter, NULL);
                pending_to_read = delimiter_result == ypipe_read_rejected;
            }

            if (_delay && pending_to_read) {
                transition_to_inactive_state_unlocked (waiting_for_delimiter);
            } else {
                acknowledge_peer_termination_unlocked (term_ack_sent);
            }
        }

        //  Delimiter happened to arrive before the term command. Now we have the
        //  term command as well, so we can move straight to term_ack_sent state.
        else if (_state == delimiter_received) {
            acknowledge_peer_termination_unlocked (term_ack_sent);
        }

        //  This is the case where both ends of the pipe are closed in parallel.
        //  We simply reply to the request by ack and continue waiting for our
        //  own ack.
        else if (_state == term_req_sent1) {
            acknowledge_peer_termination_unlocked (term_req_sent2);
        }
        drain_complete = _state != waiting_for_delimiter;
    }
    // Pending correlation release can acquire the pipe's outbound gate.
    // Notify the owner only after the termination transition releases it.
    if (_sink)
        _sink->pipe_peer_terminated (this, drain_complete);
}

void zlink::pipe_t::process_pipe_term_ack ()
{
    pipe_debug_log (this, "process_pipe_term_ack", _state, _delay,
                    _endpoint_pair.identifier ().c_str ());
    bool ack_peer = false;
    {
        scoped_lock_t lock (_out_sync);

        //  In term_ack_sent and term_req_sent2 states there's nothing to do.
        //  Simply deallocate the pipe. In term_req_sent1 state we have to ack
        //  the peer before deallocating this side of the pipe.
        //  All the other states are invalid.
        if (_state == term_req_sent1) {
            _out_pipe = NULL;
            ack_peer = true;
        } else
            zlink_assert (_state == term_ack_sent || _state == term_req_sent2);
    }

    pipe_t *const peer = detach_peer_link ();

    //  A locally initiated close that receives the peer's acknowledgement
    //  owes one reciprocal acknowledgement. Queue it before reporting local
    //  completion so cascading socket/context teardown cannot discard the
    //  peer's final close notification. The captured pair-link reference pins
    //  this exact peer until send_pipe_term_ack acquires the command reference.
    if (ack_peer) {
        zlink_assert (peer);
        const bool queued = send_pipe_term_ack (peer);
        zlink_assert (queued);
    }
    if (peer)
        peer->release_lifetime_ref ();

    //  Notify the user that all the references to the pipe should be dropped.
    zlink_assert (_sink);
    // Only a locally initiated close reaches its final ack without a peer
    // term transition. Publish the logical termination exactly once in both
    // orders; owners must retire routes before inbound drain can delay release.
    if (ack_peer)
        _sink->pipe_peer_terminated (this, true);
    _sink->pipe_terminated (this);

    // A Completion reader can still be outside the socket receive mutex after
    // pipe_terminated() returns. Mark the inbound queue terminal now and
    // delete it only when no retained reader can dereference it. This state is
    // independent of the object lifetime reference: the latter pins `this`,
    // while this one pins `_in_pipe` and its physical-queue endpoints.
    const lifetime_state_t::transition_t inbound_transition =
      _inbound_read_lifetime.complete_termination ();
    zlink_assert (inbound_transition != lifetime_state_t::transition_invalid);
    if (inbound_transition == lifetime_state_t::transition_delete_owner)
        cleanup_inbound_pipe ();

    //  Pipe objects are always heap-allocated and reference-counted by protocol
    //  state transitions, so termination ack is the canonical final release.
    const lifetime_state_t::transition_t transition =
      _lifetime.complete_termination ();
    zlink_assert (transition != lifetime_state_t::transition_invalid);
    if (transition == lifetime_state_t::transition_delete_owner)
        zlink::release_heap_owned (this);
}

void zlink::pipe_t::cleanup_inbound_pipe ()
{
    upipe_t *const inbound = _in_pipe;
    if (!inbound)
        return;

    // We own the terminal transition of _inbound_read_lifetime, so no new
    // reader can enter and the last retained reader has already left.
    if (!_conflate) {
        msg_t msg;
        while (inbound->read (&msg)) {
            if (!msg.is_delimiter () && _registry_accounting)
                get_ctx ()->_physical_queue_registry.release_committed_frame (
                  _in_physical_queue, frame_accounted_bytes (&msg),
                  counted_pending_message_ref (msg));
            const int rc = msg.close ();
            errno_assert (rc == 0);
        }
    }

    release_discarded_pipe_accounting (inbound, _in_physical_queue);
    LIBZLINK_DELETE (_in_pipe);
    retire_physical_queue_endpoints ();
}

void zlink::pipe_t::process_pipe_hwm (uint64_t inhwm_, uint64_t outhwm_)
{
    set_hwms (inhwm_, outhwm_);
}

void zlink::pipe_t::set_nodelay ()
{
    scoped_lock_t lock (_out_sync);
    _delay = false;

    if (_state == waiting_for_delimiter) {
        rollback_unlocked (false);
        _out_pipe = NULL;
        (void) send_pipe_term_ack (get_peer ());
        transition_to_inactive_state_unlocked (term_ack_sent);
    }
}

void zlink::pipe_t::terminate (bool delay_)
{
    scoped_lock_t lock (_out_sync);
    pipe_debug_log (this, "terminate-begin", _state, delay_, _endpoint_pair.identifier ().c_str ());

    //  Overload the value specified at pipe creation.
    _delay = delay_;

    //  If terminate was already called, we can ignore the duplicate invocation.
    if (_state == term_req_sent1 || _state == term_req_sent2) {
        return;
    }
    //  If the pipe is in the final phase of async termination, it's going to
    //  closed anyway. No need to do anything special here.
    if (_state == term_ack_sent) {
        return;
    }
    //  The simple sync termination case. Ask the peer to terminate and wait
    //  for the ack.
    if (_state == active) {
        send_pipe_term (get_peer ());
        transition_to_inactive_state_unlocked (term_req_sent1);
    }
    //  There are still pending messages available, but the user calls
    //  'terminate'. We can act as if all the pending messages were read.
    else if (_state == waiting_for_delimiter && !_delay) {
        //  Drop any unfinished outbound messages.
        rollback_unlocked (false);
        _out_pipe = NULL;
        (void) send_pipe_term_ack (get_peer ());
        transition_to_inactive_state_unlocked (term_ack_sent);
    }
    //  If there are pending messages still available, do nothing.
    else if (_state == waiting_for_delimiter) {
    }
    //  We've already got delimiter, but not term command yet. We can ignore
    //  the delimiter and ack synchronously terminate as if we were in
    //  active state.
    else if (_state == delimiter_received) {
        send_pipe_term (get_peer ());
        transition_to_inactive_state_unlocked (term_req_sent1);
    }
    //  There are no other states.
    else {
        zlink_assert (false);
    }

    //  Stop outbound flow of messages.
    _out_active.store (false, std::memory_order_release);

    if (_out_pipe) {
        //  Drop any unfinished outbound messages.
        rollback_unlocked (false);

        //  Write the delimiter into the pipe. Note that watermarks are not
        //  checked; thus the delimiter can be written even when the pipe is full.
        msg_t msg;
        msg.init_delimiter ();
        publish_outbound_frame_unlocked (msg, false);
        flush_unlocked ();
    }
    pipe_debug_log (this, "terminate-end", _state, _delay, _endpoint_pair.identifier ().c_str ());
}

uint64_t zlink::pipe_t::compute_lwm (uint64_t hwm_)
{
    //  Compute the low water mark. Following point should be taken
    //  into consideration:
    //
    //  1. LWM has to be less than HWM.
    //  2. LWM cannot be set to very low value (such as zero) as after filling
    //     the queue it would start to refill only after all the messages are
    //     read from it and thus unnecessarily hold the progress back.
    //  3. LWM cannot be set to very high value (such as HWM-1) as it would
    //     result in lock-step filling of the queue - if a single message is
    //     read from a full queue, writer thread is resumed to write exactly one
    //     message to the queue and go back to sleep immediately. This would
    //     result in low performance.
    //
    //  Let's make LWM 1/2 of HWM.
    return hwm_ / 2 + hwm_ % 2;
}

uint64_t zlink::pipe_t::apply_lwm_hint (uint64_t hwm_,
                                       uint64_t lwm_,
                                       uint64_t lwm_hint_)
{
    if (hwm_ <= 0 || lwm_hint_ <= 0)
        return lwm_;

    uint64_t hinted = lwm_hint_;
    if (hinted >= hwm_)
        hinted = hwm_ - 1;
    if (hinted <= 0)
        hinted = 1;

    return std::min (lwm_, hinted);
}

void zlink::pipe_t::process_delimiter ()
{
    scoped_lock_t lock (_out_sync);
    pipe_debug_log (this, "process_delimiter", _state, _delay,
                    _endpoint_pair.identifier ().c_str ());
    if (_state == term_req_sent1 || _state == term_req_sent2 || _state == term_ack_sent) {
        return;
    }
    zlink_assert (_state == active || _state == waiting_for_delimiter);

    if (_state == active) {
        transition_to_inactive_state_unlocked (delimiter_received);
    }
    else {
        rollback_unlocked (false);
        _out_pipe = NULL;
        (void) send_pipe_term_ack (get_peer ());
        transition_to_inactive_state_unlocked (term_ack_sent);
    }
}

void zlink::pipe_t::hiccup ()
{
    //  If termination is already under way do nothing.
    if (_state != active)
        return;

    //  We'll drop the pointer to the inpipe. From now on, the peer is
    //  responsible for deallocating it.

    //  Create new inpipe, keeping the chunk granularity this pipe was
    //  created with (session pipes use the smaller per-connection chunk).
    if (_conflate)
        _in_pipe = new (std::nothrow) ypipe_conflate_t<msg_t> ();
    else if (_session_pipe)
        _in_pipe = new (std::nothrow) ypipe_t<msg_t, session_pipe_granularity> ();
    else
        _in_pipe = new (std::nothrow) ypipe_t<msg_t, message_pipe_granularity> ();

    alloc_assert (_in_pipe);
    _in_active.store (true, std::memory_order_release);
    get_ctx ()->_physical_queue_registry.advance_generation (_in_physical_queue);
    _in_generation = get_ctx ()->_physical_queue_registry.generation (
      _in_physical_queue);
    _msgs_read = 0;
    _bytes_read = 0;
    _published_incomplete_bytes_read.store (0, std::memory_order_release);
    publish_ledger_unlocked (&_inbound_ledger_sequence,
                             &_published_msgs_read, &_published_bytes_read, 0,
                             0);
    _last_credit_bytes_read = 0;
    _in_incomplete_bytes = 0;

    //  Notify the peer about the hiccup.
    send_hiccup (get_peer (), _in_pipe, _in_generation);
}

void zlink::pipe_t::set_hwms (uint64_t inhwm_, uint64_t outhwm_)
{
    uint64_t in = inhwm_;
    uint64_t out = outhwm_;
    physical_queue_handle_t in_queue;
    physical_queue_handle_t out_queue;
    {
        scoped_lock_t lock (_out_sync);
        if (_transport_lane == transport_lane_completion)
            in = out = 0;
        in_queue = _in_physical_queue;
        out_queue = _out_physical_queue;
    }

    // Updating a shrinking application target samples its writer pipe. Never
    // carry this endpoint's outbound lock into that registry path: the two
    // endpoints can receive HWM updates concurrently and would otherwise take
    // their `_out_sync` locks in opposite order.
    get_ctx ()->_physical_queue_registry.update_hwm_target (
      in_queue, in);
    get_ctx ()->_physical_queue_registry.update_hwm_target (
      out_queue, out);

    // Concurrent HWM updates may overlap outside the lock. Read the registry's
    // latest values only at publication time so the last publisher cannot
    // install a stale target captured before a newer update.
    scoped_lock_t lock (_out_sync);
    in = get_ctx ()->_physical_queue_registry.applied_hwm (in_queue);
    const uint64_t planned_in =
      get_ctx ()->_physical_queue_registry.planned_hwm (in_queue);
    out = get_ctx ()->_physical_queue_registry.planned_hwm (out_queue);
    _inhwm.store (in, std::memory_order_relaxed);
    const uint64_t lwm = apply_lwm_hint (
      planned_in, compute_lwm (planned_in), _lwm_hint);
    _lwm.store (lwm, std::memory_order_relaxed);
    _hwm.store (out, std::memory_order_release);
}

void zlink::pipe_t::set_lwm_hint (uint64_t lwm_hint_)
{
    scoped_lock_t lock (_out_sync);
    _lwm_hint = _transport_lane == transport_lane_completion
                  ? 0
                  : lwm_hint_;
    const uint64_t planned_in = planned_in_hwm ();
    _lwm.store (
      apply_lwm_hint (planned_in, compute_lwm (planned_in),
                      _lwm_hint),
      std::memory_order_relaxed);
}

bool zlink::pipe_t::check_hwm () const
{
    return _out_active.load (std::memory_order_acquire) && _state == active
           && check_hwm_unlocked ();
}

zlink::pipe_message_admission_t
zlink::pipe_t::check_hwm_for_message (const msg_t *msg_)
{
    if (!msg_)
        return pipe_message_admission_invalid;

    const uint64_t max_message_bytes =
      _max_message_bytes.load (std::memory_order_acquire);
    if (_state != active)
        return pipe_message_admission_inactive;
    if (_transport_pair_write_held)
        return pipe_message_admission_transport_wait;
    if (!_out_active.load (std::memory_order_acquire)) {
        // dist_t keeps a pipe in its matching set when message preflight is
        // rejected, so it can safely consume credit published by the peer
        // before the owner processes the matching activate_write command.
        // The generic check_hwm() probe must remain passive because its
        // callers may already have removed the pipe from their active set.
        if (!check_hwm_with_peer_snapshot_unlocked ())
            return pipe_message_admission_hwm_full;
        clear_hwm_credit_wait_unlocked ();
    }
    if (!check_hwm_with_peer_snapshot_unlocked ()) {
        arm_hwm_credit_wait_unlocked ();
        if (!check_hwm_with_peer_snapshot_unlocked ())
            return pipe_message_admission_hwm_full;
        clear_hwm_credit_wait_unlocked ();
    }
    if (msg_->is_delimiter ())
        return pipe_message_admission_ready;

    const uint64_t frame_bytes = frame_accounted_bytes (msg_);
    if (frame_bytes == UINT64_MAX
        || UINT64_MAX - _out_incomplete_bytes < frame_bytes)
        return pipe_message_admission_too_large;
    const uint64_t payload_bytes = static_cast<uint64_t> (msg_->size ());
    if (UINT64_MAX - _out_incomplete_payload_bytes < payload_bytes)
        return pipe_message_admission_too_large;

    const uint64_t prospective_payload =
      _out_incomplete_payload_bytes + payload_bytes;
    if (max_message_bytes != 0 && prospective_payload > max_message_bytes)
        return pipe_message_admission_too_large;
    const bool more = (msg_->flags () & msg_t::more) != 0;
    if (!can_commit_bytes_with_peer_snapshot_unlocked (
          _out_incomplete_bytes + frame_bytes, prospective_payload,
          !more
            && (_out_incomplete_bytes == 0
                || _out_multipart_started_empty))) {
        bool credit_ready = false;
        if (_bytes_written.load (std::memory_order_acquire)
            > _peers_bytes_read.load (std::memory_order_acquire)) {
            arm_hwm_credit_wait_unlocked ();
            credit_ready = can_commit_bytes_with_peer_snapshot_unlocked (
              _out_incomplete_bytes + frame_bytes, prospective_payload,
              !more
                && (_out_incomplete_bytes == 0
                    || _out_multipart_started_empty));
            if (credit_ready)
                clear_hwm_credit_wait_unlocked ();
        }
        if (!credit_ready)
            return pipe_message_admission_hwm_full;
    }
    return pipe_message_admission_ready;
}

void zlink::pipe_t::refresh_peer_credit_snapshot_unlocked ()
{
    pipe_t *const peer = get_peer ();
    if (!peer)
        return;

    const uint64_t peer_msgs_read =
      peer->_published_msgs_read.load (std::memory_order_acquire);
    const uint64_t peer_bytes_read =
      peer->_published_bytes_read.load (std::memory_order_acquire);
    if (peer_msgs_read
        > _peers_msgs_read.load (std::memory_order_acquire))
        _peers_msgs_read.store (peer_msgs_read, std::memory_order_release);
    if (peer_bytes_read
        > _peers_bytes_read.load (std::memory_order_acquire))
        _peers_bytes_read.store (peer_bytes_read, std::memory_order_release);
}

void zlink::pipe_t::send_hwms_to_peer (uint64_t inhwm_, uint64_t outhwm_)
{
    pipe_t *peer = NULL;
    {
        scoped_lock_t lock (_out_sync);

        //  HWM propagation is meaningful only while both ends are still in the
        //  steady-state data path. During async termination the peer can be in
        //  the final ack/delete phase, so skip late updates instead of sending
        //  pipe_hwm to a dying peer object.
        if (_state != active)
            return;

        peer = retain_peer_snapshot_unlocked ();
        if (!peer)
            return;
    }

    send_pipe_hwm (peer, inhwm_, outhwm_);
    peer->release_lifetime_ref ();
}

void zlink::pipe_t::set_endpoint_pair (zlink::endpoint_uri_pair_t endpoint_pair_)
{
    // Static endpoint metadata is published exactly once before the pipe is
    // exposed to its socket/session owner. Reconnect identity is the only
    // mutable member and publishes independently through its atomic wrapper.
    zlink_assert (_endpoint_pair.local_type == endpoint_type_none);
    const uint64_t connection_id = endpoint_pair_.connection_id.load ();
    _endpoint_pair = ZLINK_MOVE (endpoint_pair_);
    set_transport_connection_id (connection_id);
}

const zlink::endpoint_uri_pair_t &zlink::pipe_t::get_endpoint_pair () const
{
    return _endpoint_pair;
}

void zlink::pipe_t::set_transport_connection_id (uint64_t connection_id_)
{
    if (_transport_lifetime) {
        scoped_lock_t generation_lock (_transport_lifetime->transport_sync);
        _transport_lifetime->connection_id.store (
          connection_id_, std::memory_order_release);
        _endpoint_pair.connection_id = connection_id_;
        return;
    }
    _endpoint_pair.connection_id = connection_id_;
}

uint64_t zlink::pipe_t::get_transport_connection_id () const
{
    return _transport_lifetime
             ? _transport_lifetime->connection_id.load (
                 std::memory_order_acquire)
             : _endpoint_pair.connection_id.load ();
}

bool zlink::pipe_t::try_claim_transport_disconnected_event ()
{
    // A raw connector can reuse this pipe across reconnects. Claim the
    // physical connection, so replacing it does not inherit the old claim.
    const uint64_t connection_id = get_transport_connection_id ();
    return connection_id != 0
           && _transport_disconnected_event_connection_id.exchange (
                connection_id, std::memory_order_acq_rel) != connection_id;
}

uint64_t zlink::pipe_t::get_route_incarnation_id () const
{
    zlink_assert (_transport_lifetime);
    return _transport_lifetime->route_incarnation_id;
}

void zlink::pipe_t::set_transport_pair (transport_lane_t lane_,
                                        uint64_t pair_id_,
                                        uint64_t generation_)
{
    get_ctx ()->_physical_queue_registry.classify_pipepair_queues (
      _in_physical_queue, _out_physical_queue, lane_);
    _transport_lane = lane_;
    _transport_pair_id = pair_id_;
    _transport_pair_generation = generation_;
    if (lane_ == transport_lane_completion)
        set_hwms (0, 0);
}

void zlink::pipe_t::set_transport_lane_count (unsigned char lane_count_)
{
    zlink_assert (lane_count_ == 1u || lane_count_ == 2u);
    _transport_lane_count.store (lane_count_, std::memory_order_release);
}

zlink::transport_lane_t zlink::pipe_t::get_transport_lane () const
{
    return _transport_lane;
}

unsigned char zlink::pipe_t::get_transport_lane_count () const
{
    return _transport_lane_count.load (std::memory_order_acquire);
}

bool zlink::pipe_t::uses_registry_accounting () const
{
    return _registry_accounting;
}

uint64_t zlink::pipe_t::get_transport_pair_id () const
{
    return _transport_pair_id;
}

uint64_t zlink::pipe_t::get_transport_pair_generation () const
{
    return _transport_pair_generation;
}

void zlink::pipe_t::set_locally_initiated (bool value_)
{
    _locally_initiated = value_;
}

bool zlink::pipe_t::is_locally_initiated () const
{
    return _locally_initiated;
}

void zlink::pipe_t::send_disconnect_msg ()
{
    scoped_lock_t lock (_out_sync);
    if (_disconnect_msg.size () > 0 && _out_pipe) {
        // Rollback any incomplete message in the pipe, and push the disconnect message.
        rollback_unlocked (false);

        const bool written = write_message_unlocked (&_disconnect_msg, false);
        zlink_assert (written);
        flush_unlocked ();
        _disconnect_msg.init ();
    }
}

void zlink::pipe_t::set_disconnect_msg (const std::vector<unsigned char> &disconnect_)
{
    _disconnect_msg.close ();
    const int rc = _disconnect_msg.init_buffer (&disconnect_[0], disconnect_.size ());
    errno_assert (rc == 0);
}

void zlink::pipe_t::send_hiccup_msg (const std::vector<unsigned char> &hiccup_)
{
    scoped_lock_t lock (_out_sync);
    if (!hiccup_.empty () && _out_pipe) {
        msg_t msg;
        const int rc = msg.init_buffer (&hiccup_[0], hiccup_.size ());
        errno_assert (rc == 0);

        const bool written = write_message_unlocked (&msg, false);
        zlink_assert (written);
        flush_unlocked ();
    }
}
