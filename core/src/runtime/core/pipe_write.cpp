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

#ifdef ZLINK_BUILD_TESTS
namespace
{
std::atomic<zlink::pipe_write_commit_test_hook_fn> pipe_write_commit_test_hook (
  NULL);
std::atomic<void *> pipe_write_commit_test_hook_userdata (NULL);
}

void zlink::test_set_pipe_write_commit_hook (
  pipe_write_commit_test_hook_fn hook_, void *userdata_)
{
    if (!hook_) {
        pipe_write_commit_test_hook.store (NULL, std::memory_order_release);
        pipe_write_commit_test_hook_userdata.store (NULL,
                                                    std::memory_order_release);
        return;
    }
    pipe_write_commit_test_hook_userdata.store (userdata_,
                                                std::memory_order_release);
    pipe_write_commit_test_hook.store (hook_, std::memory_order_release);
}
#endif

using zlink::pipe_detail::consume_if_delimiter;
using zlink::pipe_detail::head_reclassify_armed;
using zlink::pipe_detail::head_reclassify_idle;
using zlink::pipe_detail::head_reclassify_queued;
using zlink::pipe_detail::pipe_debug_log;

zlink::pipe_message_admission_t zlink::pipe_t::admit_owner_message_start ()
{
    pipe_message_admission_t admission;
    if (!admit_write_unlocked (&admission))
        return admission;

    _out_owner_message_started = true;
    _out_owner_message_start_pending = true;
    return pipe_message_admission_ready;
}

bool zlink::pipe_t::write_owner_started_message (
  const msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    if (unlikely (!admit_owner_started_write_unlocked (admission_out_)))
        return false;

    const bool more = (msg_->flags () & msg_t::more) != 0;
    if (!write_message_unlocked (msg_, true, true, admission_out_))
        return false;
    if (!more)
        flush_unlocked ();
    return true;
}

bool zlink::pipe_t::write_owner_started_message_observed (
  const msg_t *msg_, pipe_write_observer_fn observer_,
  void *observer_userdata_, pipe_message_admission_t *admission_out_)
{
    if (!observer_) {
        errno = EFAULT;
        return false;
    }
    if (!observer_ (this, observer_userdata_,
                    pipe_write_observer_prepare)) {
        const int observer_errno = errno;
        if (admission_out_) {
            *admission_out_ = observer_errno == ENOBUFS
                                ? pipe_message_admission_request_full
                              : observer_errno == EAGAIN
                                ? pipe_message_admission_transport_wait
                              : observer_errno == EHOSTUNREACH
                                ? pipe_message_admission_inactive
                                : pipe_message_admission_invalid;
        }
        errno = observer_errno == ENOBUFS ? EAGAIN : observer_errno;
        return false;
    }
    if (!retain_lifetime_ref ()) {
        (void) observer_ (this, observer_userdata_,
                          pipe_write_observer_finish);
        errno = EHOSTUNREACH;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_inactive;
        return false;
    }

    bool written = false;
    bool committed = false;
    {
        written = admit_owner_started_write_unlocked (admission_out_);

        const bool more = (msg_->flags () & msg_t::more) != 0;
        if (written)
            written = write_message_unlocked (msg_, true, true,
                                               admission_out_);
        if (written) {
            committed = observer_ (this, observer_userdata_,
                                   pipe_write_observer_commit);
            zlink_assert (committed);
            if (committed && !more)
                flush_unlocked ();
        }
    }

    (void) observer_ (this, observer_userdata_, pipe_write_observer_finish);
    release_lifetime_ref ();
    if (written && !committed) {
        terminate (false);
        errno = EPROTO;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_invalid;
        return false;
    }
    return written;
}

bool zlink::pipe_t::remote_flow_blocks_next_message () const
{
    scoped_lock_t lock (_out_sync);
    return remote_flow_blocked_unlocked ();
}

void zlink::pipe_t::arm_hwm_credit_wait_unlocked ()
{
    _out_active.store (false, std::memory_order_release);
    _waiting_for_byte_credit.store (true, std::memory_order_release);
    // Pair the waiter publication with the following peer-credit sample. A
    // racing reader either observes the waiter and emits activate_write, or
    // its already-published credit is consumed by the writer's recheck.
    std::atomic_thread_fence (std::memory_order_seq_cst);
}

void zlink::pipe_t::clear_hwm_credit_wait_unlocked ()
{
    _out_active.store (true, std::memory_order_release);
    _waiting_for_byte_credit.store (false, std::memory_order_release);
}

bool zlink::pipe_t::admit_owner_started_write_unlocked (
  pipe_message_admission_t *admission_out_)
{
    const bool reuse_start_admission = _out_owner_message_start_pending;
    _out_owner_message_start_pending = false;
    if (!reuse_start_admission)
        return admit_write_unlocked (admission_out_);

    //  PAUSE and HWM apply at the next message boundary, but termination and
    //  the initial transport-pair hold can still revoke an accepted start.
    if (_state != active) {
        if (admission_out_)
            *admission_out_ = pipe_message_admission_inactive;
        return false;
    }
    if (_transport_pair_write_held) {
        if (admission_out_)
            *admission_out_ = pipe_message_admission_transport_wait;
        return false;
    }
    return true;
}

zlink::pipe_message_admission_t zlink::pipe_t::check_write_admission ()
{
    pipe_message_admission_t admission = pipe_message_admission_invalid;
    (void) admit_write_unlocked (&admission);
    return admission;
}

bool zlink::pipe_t::take_hwm_credit_recovery ()
{
    scoped_lock_t lock (_out_sync);
    const bool recovery =
      _waiting_for_byte_credit.load (std::memory_order_acquire);
    // process_activate_write() releases _out_sync before notifying the socket.
    // If another sender filled the pipe in that interval, _out_active is false
    // and this marker belongs to the new wait, not the activation being
    // delivered. Preserve it so the next peer drain still emits a wake.
    if (recovery && _out_active.load (std::memory_order_acquire))
        _waiting_for_byte_credit.store (false, std::memory_order_release);
    return recovery;
}

bool zlink::pipe_t::take_request_correlation_recovery ()
{
    return _request_correlation_recovery.exchange (
      false, std::memory_order_acq_rel);
}

void zlink::pipe_t::hold_writes_until_transport_pair_ready ()
{
    scoped_lock_t lock (_out_sync);
    _transport_pair_write_held = true;
    _out_active.store (false, std::memory_order_release);
}

bool zlink::pipe_t::release_writes_for_transport_pair ()
{
    scoped_lock_t lock (_out_sync);
    if (!_transport_pair_write_held)
        return false;
    _transport_pair_write_held = false;
    // The routing-id preamble may still consume the cached credit. Reuse
    // normal admission's snapshot/waiter/recheck so either read order wakes
    // the writer when the empty pipe can admit its first complete message.
    if (_state != active || !hwm_credit_ready_unlocked (NULL))
        return false;
    _out_active.store (true, std::memory_order_release);
    //  This transition removes only the transport-wait cause. A remote PAUSE
    //  that is still in effect keeps the pipe unwritable.
    return !remote_flow_blocked_unlocked ();
}

bool zlink::pipe_t::transport_pair_writes_released () const
{
    scoped_lock_t lock (_out_sync);
    return !_transport_pair_write_held;
}

bool zlink::pipe_t::remote_flow_paused () const
{
    scoped_lock_t lock (_out_sync);
    return _remote_flow_paused;
}

#ifdef ZLINK_BUILD_TESTS
void zlink::pipe_t::test_flow_probe (bool *out_active_,
                                     bool *hwm_full_,
                                     bool *remote_paused_,
                                     bool *byte_credit_waiter_,
                                     uint64_t *in_flight_bytes_) const
{
    scoped_lock_t lock (_out_sync);
    if (out_active_)
        *out_active_ = _out_active.load (std::memory_order_acquire);
    //  Deliberately the cached peer credit, without refreshing it: a test has
    //  to be able to see that the writer still believes it is full.
    if (hwm_full_)
        *hwm_full_ = !check_hwm_unlocked ();
    if (remote_paused_)
        *remote_paused_ = _remote_flow_paused;
    if (byte_credit_waiter_)
        *byte_credit_waiter_ =
          _waiting_for_byte_credit.load (std::memory_order_acquire);
    if (in_flight_bytes_) {
        const uint64_t bytes_written =
          _bytes_written.load (std::memory_order_acquire);
        const uint64_t peers_bytes_read =
          _peers_bytes_read.load (std::memory_order_acquire);
        *in_flight_bytes_ = bytes_written > peers_bytes_read
                              ? bytes_written - peers_bytes_read
                              : 0;
    }
}

#endif

#ifdef ZLINK_BUILD_TESTS
uint64_t zlink::pipe_t::test_frame_accounted_bytes (const msg_t *msg_)
{
    return frame_accounted_bytes (msg_);
}

uint64_t zlink::pipe_t::test_compute_lwm (uint64_t hwm_)
{
    return compute_lwm (hwm_);
}

uint64_t zlink::pipe_t::test_apply_lwm_hint (uint64_t hwm_, uint64_t lwm_,
                                             uint64_t lwm_hint_)
{
    return apply_lwm_hint (hwm_, lwm_, lwm_hint_);
}

#endif

bool zlink::pipe_t::take_flow_resume_recovery ()
{
    scoped_lock_t lock (_out_sync);
    const bool recovery =
      _waiting_for_flow_resume.load (std::memory_order_acquire);
    // A new PAUSE can race the socket callback after the prior RESUME released
    // _out_sync. Keep a marker armed for that newer pause instead of consuming
    // it as part of the older activation.
    if (recovery && !_remote_flow_paused)
        _waiting_for_flow_resume.store (false, std::memory_order_release);
    return recovery;
}

void zlink::pipe_t::process_flow_state (unsigned char state_, uint64_t epoch_)
{
    const unsigned char lane_count = get_transport_lane_count ();
    if (_transport_pair_id == 0 || _transport_pair_generation == 0
        || get_transport_connection_id () == 0 || !is_lifecycle_active ())
        return;

    // Count-2 inproc FLOWSTATE reaches the exact Completion endpoint as an
    // owner command. It must be mapped by the socket's ready pair table before
    // touching the Application pipe that owns send admission and accounting.
    if (_transport_lane == transport_lane_completion) {
        if (lane_count == 2u && _sink)
            _sink->flow_state_received (this, state_, epoch_);
        return;
    }

    // Network decode already maps count-2 control to Application, while
    // count-1 inproc delivers directly to Application. Both converge here.
    if (_transport_lane != transport_lane_application
        || (lane_count != 1u && lane_count != 2u))
        return;

    flow_state_transition_t transition = flow_state_no_transition;
    bool actual_writable = false;
    const bool notify =
      apply_remote_flow_state (state_, epoch_, &transition, &actual_writable);
    //  Observation never gates the send path: this call happens after the
    //  transition already committed, off the per-message write/read path, and
    //  only on an actual PAUSED<->RUNNING flip (never on a stale, duplicate,
    //  or same-state frame).
    if (transition != flow_state_no_transition)
        _sink->flow_state_applied (
          this, transition == flow_state_transition_paused, epoch_,
          actual_writable);
    if (notify)
        _sink->write_activated (this);
}

void zlink::pipe_t::process_peer_weight (uint32_t weight_,
                                         uint64_t connection_id_)
{
    scoped_lock_t generation_lock (_transport_lifetime->transport_sync);
    //  The command retains this exact endpoint until processing completes.
    //  Its immutable transport identity prevents a delayed command from an
    //  old generation (or a recycled connection) changing a replacement.
    if (weight_ > max_peer_weight
        || _transport_lane != transport_lane_application
        || (_transport_pair_id != 0 && connection_id_ == 0)
        || get_transport_connection_id () != connection_id_
        || !is_lifecycle_active ())
        return;

    // The exact pipe owns the latest value even before bind/xattach installs
    // its socket sink. A later scheduler attachment reads the same generation-
    // scoped record, so owner-command ordering cannot lose pre-admission state.
    _peer_weight.store (weight_, std::memory_order_release);
    _peer_weight_connection_id.store (connection_id_,
                                      std::memory_order_release);
    if (_sink)
        _sink->peer_weight_received (this, weight_);
}

bool zlink::pipe_t::apply_remote_flow_state (
  unsigned char state_, uint64_t epoch_,
  flow_state_transition_t *out_transition_, bool *out_actual_writable_)
{
    if (out_transition_)
        *out_transition_ = flow_state_no_transition;
    if (out_actual_writable_)
        *out_actual_writable_ = false;
    const bool paused = state_ != 0;
    bool notify = false;
    {
        scoped_lock_t lock (_out_sync);
        //  A replay whose epoch does not advance is stale. Without this an
        //  attach-time replay queued after a newer acceptance would reinstate
        //  the older state, and the socket record - which already holds the
        //  newer one - would deduplicate every correction away.
        //
        //  0 is the "never set" marker and is invalid at every receiving
        //  layer, this one included: treating it as a reset would let it
        //  override whatever ordering the pipe has already established.
        if (epoch_ == 0 || epoch_ <= _remote_flow_epoch)
            return false;
        _remote_flow_epoch = epoch_;
        if (_remote_flow_paused == paused)
            return false;
        _remote_flow_paused = paused;
        if (out_transition_)
            *out_transition_ = paused ? flow_state_transition_paused
                                       : flow_state_transition_resumed;
        //  Resuming removes only the remote-pause cause. Termination and the
        //  transport-pair hold keep their own state, so the send-ready edge is
        //  published only when every cause is clear.
        if (!paused && _state == active && !_transport_pair_write_held
            && _out_active.load (std::memory_order_acquire)) {
            //  A send refused by the remote cause never evaluated the HWM, so
            //  no cause currently owns the pending wake. Hand it to the
            //  byte-credit cause using the classic lost-wakeup discipline:
            //  arm first, then re-read the credit the peer has published.
            //
            //  Arming first is what closes the race. A reader that publishes
            //  credit after this store sees the armed waiter and sends the
            //  activation itself; a reader that published before it is picked
            //  up by the fresh re-read below. Deciding from the cached
            //  snapshot instead would miss a sub-LWM read that published
            //  credit while no waiter was registered - that read is the last
            //  one that would ever have qualified.
            //  Arm and re-read form a store-load pair, which is the one order
            //  a release store does not constrain. The fence keeps the re-read
            //  below from being hoisted above the arm above. The reader's own
            //  half of this pair - it publishes credit before reading the
            //  waiter - lives in the inbound accounting path and needs the
            //  matching barrier there; see the worklog.
            //
            //  If _out_active was already false, byte-HWM admission owns the
            //  wake. A remote resume must not replace that decision with the
            //  coarser current-in-flight check: no peer credit was returned,
            //  and the part that reached HWM can still be rejected.
            _out_active.store (false, std::memory_order_release);
            _waiting_for_byte_credit.store (true, std::memory_order_release);
            std::atomic_thread_fence (std::memory_order_seq_cst);
            if (check_hwm_with_peer_snapshot_unlocked ()) {
                _out_active.store (true, std::memory_order_release);
                notify = true;
            }
        }
        if (out_actual_writable_)
            *out_actual_writable_ = write_state_ready_unlocked (NULL);
    }
    return notify;
}

void zlink::pipe_t::set_remote_flow_pause_started_ms (uint64_t ms_)
{
    _remote_flow_pause_started_ms = ms_;
}

uint64_t zlink::pipe_t::remote_flow_pause_started_ms () const
{
    return _remote_flow_pause_started_ms;
}

bool zlink::pipe_t::write (
  const msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    // Hot path: PAIR/DEALER steady-state send reaches this path for every
    // message. The socket lifecycle turn (or session I/O owner) is this
    // endpoint's sole C2 writer, so it needs no per-message mutex.
    if (unlikely (!admit_write_unlocked (admission_out_)))
        return false;

    return write_message_unlocked (msg_, true, true, admission_out_);
}

bool zlink::pipe_t::write_routing_id_and_flush (const msg_t *msg_)
{
    if (!msg_ || !msg_->is_routing_id ()) {
        errno = EINVAL;
        return false;
    }

    if (_state != active)
        return false;
    if (!write_message_unlocked (msg_, false))
        return false;
    flush_unlocked ();
    return true;
}

bool zlink::pipe_t::write_transport_probe_and_flush (const msg_t *msg_)
{
    if (!msg_ || msg_->size () != 0 || (msg_->flags () & msg_t::more) != 0) {
        errno = EINVAL;
        return false;
    }

    // Application writes remain blocked until both transport lanes pass
    // admission. A ROUTER probe is queued early so a same-thread peer does not
    // need another API call to process the later lane-ready mailbox command.
    // The peer socket still withholds reads until its own pair is ready.
    if (!_transport_pair_write_held || _state != active)
        return false;
    if (!write_message_unlocked (msg_, false))
        return false;
    flush_unlocked ();
    return true;
}

bool zlink::pipe_t::peer_control_slots_enabled_unlocked () const
{
    const unsigned char lane_count =
      _transport_lane_count.load (std::memory_order_acquire);
    if (_transport_pair_id == 0 || _transport_pair_generation == 0
        || !peer_uses_routed_protocol_unlocked ())
        return false;

    if (lane_count == 1u)
        return _transport_lane == transport_lane_application;
    if (lane_count == 2u)
        return _transport_lane == transport_lane_application
               || _transport_lane == transport_lane_completion;
    return false;
}

void zlink::pipe_t::discard_pending_peer_controls_unlocked ()
{
    _pending_peer_weight = pending_peer_weight_unset;
    _pending_peer_weight_sequence = 0;
    _pending_flow_state = flow_state::receive_flow_running;
    _pending_flow_state_epoch = 0;
    _pending_flow_state_sequence = 0;
    _pending_flow_state_valid = false;
    _pending_peer_control_sequence = 0;
}

uint64_t zlink::pipe_t::next_peer_control_sequence_unlocked ()
{
    if (_pending_peer_control_sequence != UINT64_MAX)
        return ++_pending_peer_control_sequence;

    // Only two slots survive. Rebase them without changing their relative
    // order, then assign the new update a strictly later sequence. This keeps
    // the long-lived socket correct even at the uint64 wrap boundary.
    const bool has_weight =
      _pending_peer_weight != pending_peer_weight_unset;
    const bool has_flow = _pending_flow_state_valid;
    if (has_weight && has_flow) {
        if (_pending_peer_weight_sequence < _pending_flow_state_sequence) {
            _pending_peer_weight_sequence = 1;
            _pending_flow_state_sequence = 2;
        } else {
            _pending_flow_state_sequence = 1;
            _pending_peer_weight_sequence = 2;
        }
        _pending_peer_control_sequence = 2;
    } else if (has_weight) {
        _pending_peer_weight_sequence = 1;
        _pending_peer_control_sequence = 1;
    } else if (has_flow) {
        _pending_flow_state_sequence = 1;
        _pending_peer_control_sequence = 1;
    } else {
        _pending_peer_control_sequence = 0;
    }
    return ++_pending_peer_control_sequence;
}

bool zlink::pipe_t::stage_peer_weight_control_unlocked (uint32_t weight_)
{
    if (weight_ > max_peer_weight)
        return false;
    _pending_peer_weight = weight_;
    _pending_peer_weight_sequence = next_peer_control_sequence_unlocked ();
    return true;
}

bool zlink::pipe_t::stage_flow_state_control_unlocked (
  unsigned char state_, uint64_t epoch_)
{
    if (!flow_state::state_valid (state_) || epoch_ == 0)
        return false;
    _pending_flow_state = state_;
    _pending_flow_state_epoch = epoch_;
    _pending_flow_state_sequence = next_peer_control_sequence_unlocked ();
    _pending_flow_state_valid = true;
    return true;
}

bool zlink::pipe_t::append_pending_peer_controls_unlocked ()
{
    if (!pending_peer_controls_unlocked ())
        return true;
    if (_out_incomplete_bytes != 0 || _out_incomplete_payload_bytes != 0)
        return false;
    if (_state != active || _transport_pair_write_held || !_out_pipe
        || !_session_pipe)
        return false;

    while (pending_peer_controls_unlocked ()) {
        const bool append_weight =
          _pending_peer_weight != pending_peer_weight_unset
          && (!_pending_flow_state_valid
              || _pending_peer_weight_sequence
                   < _pending_flow_state_sequence);

        msg_t command;
        const int init_rc = command.init ();
        errno_assert (init_rc == 0);
        int frame_rc = 0;
        if (append_weight) {
            frame_rc =
              zmp_peer_weight::init_command (&command, _pending_peer_weight);
        } else {
            flow_state::frame_t frame;
            frame.state = _pending_flow_state;
            frame.epoch = _pending_flow_state_epoch;
            frame_rc = flow_state::init_frame (&command, frame);
        }
        if (frame_rc != 0) {
            const int close_rc = command.close ();
            errno_assert (close_rc == 0);
            return false;
        }

        // Session pull drops a nonzero stamp from a retired transport
        // generation. A hiccup also discards both pending slots before binding
        // a replacement ypipe.
        command.set_transport_connection_id (get_transport_connection_id ());
        const uint64_t control_bytes = frame_accounted_bytes (&command);
        //  A peer-control frame is later dequeued and released through the
        //  same physical-queue registry path as data frames, so commit its
        //  charge here to keep committed accounting balanced. The completion
        //  lane that carries these controls has planned_hwm 0, so this commit
        //  does not impose a byte HWM on control frames. Application-lane pipes
        //  without registry accounting keep the plain write and no release.
        if (_registry_accounting) {
            get_ctx ()->_physical_queue_registry.commit_message (
              _out_physical_queue, control_bytes,
              counted_pending_message_ref (command), false);
            publish_outbound_frame_unlocked (command, false);
        } else {
            _out_pipe->write (command, false);
        }
        const uint64_t bytes_written =
          _bytes_written.load (std::memory_order_acquire);
        const uint64_t msgs_written =
          _msgs_written.load (std::memory_order_acquire);
        const uint64_t new_bytes_written =
          control_bytes == UINT64_MAX
              || UINT64_MAX - bytes_written < control_bytes
            ? UINT64_MAX
            : bytes_written + control_bytes;
        const uint64_t new_msgs_written =
          msgs_written == UINT64_MAX ? UINT64_MAX : msgs_written + 1;
        publish_outbound_ledger_unlocked (new_msgs_written,
                                          new_bytes_written);
        _out_complete_record_pending = true;
        const int reset_rc = command.init ();
        errno_assert (reset_rc == 0);

        if (append_weight) {
            _pending_peer_weight = pending_peer_weight_unset;
            _pending_peer_weight_sequence = 0;
        } else {
            _pending_flow_state = flow_state::receive_flow_running;
            _pending_flow_state_epoch = 0;
            _pending_flow_state_sequence = 0;
            _pending_flow_state_valid = false;
        }
        publish_outbound_accounting_unlocked (false);
    }
    _pending_peer_control_sequence = 0;
    return true;
}

bool zlink::pipe_t::dispatch_pending_inproc_controls_unlocked ()
{
    if (!pending_peer_controls_unlocked ())
        return true;
    if (_out_incomplete_bytes != 0 || _out_incomplete_payload_bytes != 0)
        return false;
    if (_state != active || _transport_pair_write_held || !_out_pipe
        || _session_pipe)
        return false;

    pipe_t *const peer = retain_peer_snapshot_unlocked ();
    if (!peer)
        return false;
    const uint64_t source_connection_id = get_transport_connection_id ();
    // Do not take the peer's outbound lock while holding ours. Pair identity
    // and connection id are immutable/atomic; the destination command checks
    // lifecycle on its owner thread before applying the control.
    const bool exact_peer = peer->get_transport_lane () == _transport_lane
                            && peer->get_transport_lane_count ()
                                 == get_transport_lane_count ()
                            && peer->get_transport_pair_id ()
                                 == _transport_pair_id
                            && peer->get_transport_pair_generation ()
                                 == _transport_pair_generation
                            && peer->get_transport_connection_id ()
                                 == source_connection_id
                            && (_transport_pair_id == 0
                                || (source_connection_id != 0
                                    && _transport_pair_generation != 0));
    if (!exact_peer) {
        peer->release_lifetime_ref ();
        return false;
    }

    bool delivered = true;
    while (pending_peer_controls_unlocked ()) {
        const bool deliver_weight =
          _pending_peer_weight != pending_peer_weight_unset
          && (!_pending_flow_state_valid
              || _pending_peer_weight_sequence
                   < _pending_flow_state_sequence);
        if (deliver_weight) {
            // WEIGHT remains Application-only even though the common pending
            // control machinery also serves count-2 Completion FLOWSTATE.
            if (_transport_lane != transport_lane_application
                || peer->get_transport_lane ()
                     != transport_lane_application) {
                delivered = false;
                break;
            }
            delivered = send_peer_weight (
              peer, _pending_peer_weight, source_connection_id);
            if (!delivered)
                break;
            _pending_peer_weight = pending_peer_weight_unset;
            _pending_peer_weight_sequence = 0;
        } else {
            send_flow_state (peer, _pending_flow_state,
                             _pending_flow_state_epoch);
            _pending_flow_state = flow_state::receive_flow_running;
            _pending_flow_state_epoch = 0;
            _pending_flow_state_sequence = 0;
            _pending_flow_state_valid = false;
        }
    }
    if (!pending_peer_controls_unlocked ())
        _pending_peer_control_sequence = 0;
    peer->release_lifetime_ref ();
    return delivered && !pending_peer_controls_unlocked ();
}

bool zlink::pipe_t::flush_pending_peer_controls_unlocked ()
{
    if (!pending_peer_controls_unlocked ())
        return true;
    if (_out_incomplete_bytes != 0 || _out_incomplete_payload_bytes != 0
        || _state != active || _transport_pair_write_held || !_out_pipe)
        return false;
    flush_unlocked ();
    return !pending_peer_controls_unlocked ();
}

bool zlink::pipe_t::write_peer_weight_control_and_flush (uint32_t weight_,
                                                         bool defer_flush_)
{
    if (weight_ > max_peer_weight
        || _transport_lane != transport_lane_application
        || _registry_accounting || !peer_control_slots_enabled_unlocked ()) {
        errno = EINVAL;
        return false;
    }

    scoped_lock_t lock (_out_sync);
    if (_state != active || _transport_pair_write_held)
        return false;
    // A terminal wire record (or an inproc owner command) must not overtake an
    // Application multipart. Final commit or rollback publishes the latest
    // surviving controls at the next real boundary.
    if (defer_flush_ || _out_incomplete_bytes != 0
        || _out_incomplete_payload_bytes != 0)
        return stage_peer_weight_control_unlocked (weight_);
    if (!stage_peer_weight_control_unlocked (weight_))
        return false;
    return flush_pending_peer_controls_unlocked ();
}

bool zlink::pipe_t::write_flow_state_control_and_flush (
  unsigned char state_, uint64_t epoch_, bool defer_flush_)
{
    const unsigned char lane_count = get_transport_lane_count ();
    const bool topology_control_lane =
      (lane_count == 1u && _transport_lane == transport_lane_application)
      || (lane_count == 2u && _transport_lane == transport_lane_completion);
    if (!flow_state::state_valid (state_) || epoch_ == 0
        || !topology_control_lane || _transport_pair_id == 0
        || _transport_pair_generation == 0
        || !peer_control_slots_enabled_unlocked ()) {
        errno = EINVAL;
        return false;
    }

    scoped_lock_t lock (_out_sync);
    if (_state != active || _transport_pair_write_held)
        return false;
    if (defer_flush_ || _out_incomplete_bytes != 0
        || _out_incomplete_payload_bytes != 0)
        return stage_flow_state_control_unlocked (state_, epoch_);
    if (!stage_flow_state_control_unlocked (state_, epoch_))
        return false;
    return flush_pending_peer_controls_unlocked ();
}

bool zlink::pipe_t::flush_pending_peer_controls ()
{
    scoped_lock_t lock (_out_sync);
    if (!peer_control_slots_enabled_unlocked ()) {
        discard_pending_peer_controls_unlocked ();
        return false;
    }
    return flush_pending_peer_controls_unlocked ();
}

bool zlink::pipe_t::has_pending_peer_controls ()
{
    scoped_lock_t lock (_out_sync);
    return pending_peer_controls_unlocked ();
}

bool zlink::pipe_t::write_and_flush (
  const msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    if (unlikely (!admit_write_unlocked (admission_out_)))
        return false;

    const bool more = (msg_->flags () & msg_t::more) != 0;
    if (!write_message_unlocked (msg_, true, true, admission_out_))
        return false;
    if (!more)
        flush_unlocked ();

    return true;
}

bool zlink::pipe_t::write_and_flush_if_transport_connection (
  const msg_t *msg_, uint64_t connection_id_,
  pipe_message_admission_t *admission_out_)
{
    if (unlikely (!_transport_lifetime || connection_id_ == 0
                  || _transport_lifetime->connection_id.load (
                       std::memory_order_acquire)
                       != connection_id_)) {
        if (admission_out_)
            *admission_out_ = pipe_message_admission_inactive;
        errno = EAGAIN;
        return false;
    }
    // Admission observes the C3 connection identity while the endpoint turn
    // owns the producer. A concurrent retirement may follow this check; every
    // reply part carries this identity and session pull drops the old record
    // rather than delivering it on a replacement transport.
    return write_and_flush (msg_, admission_out_);
}

bool zlink::pipe_t::try_write_complete_record_and_flush (
  const msg_t *parts_, size_t part_count_)
{
    if (!parts_ || part_count_ < 2)
        return false;

    const uint64_t max_message_bytes =
      _max_message_bytes.load (std::memory_order_acquire);
    if (unlikely (!write_state_ready_unlocked (NULL))
        || _registry_accounting || _conflate || !_out_pipe
        || _out_incomplete_bytes != 0
        || _out_incomplete_payload_bytes != 0
        || _out_multipart_started_empty || _out_owner_message_started
        || _out_owner_message_start_pending)
        return false;

    uint64_t record_bytes = 0;
    uint64_t payload_bytes = 0;
    for (size_t i = 0; i != part_count_; ++i) {
        const msg_t &part = parts_[i];
        const bool expected_more = i + 1 < part_count_;
        if (!part.check ()
            || ((part.flags () & msg_t::more) != 0) != expected_more
            || part.is_delimiter ()
            || part.is_routing_id () || part.is_credential ())
            return false;

        const uint64_t frame_bytes = frame_accounted_bytes (&part);
        const uint64_t part_payload = static_cast<uint64_t> (part.size ());
        if (frame_bytes == UINT64_MAX
            || UINT64_MAX - record_bytes < frame_bytes
            || UINT64_MAX - payload_bytes < part_payload)
            return false;
        record_bytes += frame_bytes;
        payload_bytes += part_payload;
    }

    // This subset deliberately excludes the empty-pipe oversize exception.
    // The generic path owns incremental HWM refusal, waiter publication, and
    // retry/rollback whenever the whole record is not already admissible.
    if ((max_message_bytes != 0 && payload_bytes > max_message_bytes)
        || !can_commit_bytes_with_peer_snapshot_unlocked (
          record_bytes, payload_bytes, false, &max_message_bytes))
        return false;

    for (size_t i = 0; i != part_count_; ++i) {
        const bool written =
          write_message_unlocked (&parts_[i], false, false, NULL,
                                  &max_message_bytes);
        zlink_assert (written);
        LIBZLINK_UNUSED (written);
    }
    flush_unlocked ();
    return true;
}

bool zlink::pipe_t::write_no_recursive_hwm_check (
  const msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    if (unlikely (!admit_write_unlocked (admission_out_)))
        return false;

    return write_message_unlocked (msg_, true, false, admission_out_);
}

bool zlink::pipe_t::write_single_message_and_flush_no_recursive_hwm_check (
  const msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    if (unlikely (!write_state_ready_unlocked (admission_out_)))
        return false;
    const uint64_t max_message_bytes =
      _max_message_bytes.load (std::memory_order_acquire);
    const uint64_t hwm = _hwm.load (std::memory_order_acquire);

    // The distributor, ROUTER, and STREAM direct-send paths own a complete
    // single application message. Keep that explicit contract out of the generic
    // multipart/registry state machine: it avoids provisional counter writes
    // and repeated policy branches for every subscriber while preserving the
    // same byte-HWM decision in the endpoint owner turn.
    if (likely (!_registry_accounting && !_conflate
                && (msg_->flags () & msg_t::more) == 0
                && !msg_->is_delimiter ()
                && _out_incomplete_bytes == 0
                && _out_incomplete_payload_bytes == 0
                && !_out_multipart_started_empty
                && !_out_owner_message_started
                && !_out_owner_message_start_pending)) {
        if (admission_out_)
            *admission_out_ = pipe_message_admission_invalid;
        const uint64_t payload_bytes = static_cast<uint64_t> (msg_->size ());
        const uint64_t frame_bytes = frame_accounted_bytes (msg_);
        if (unlikely (frame_bytes == UINT64_MAX
                      || (max_message_bytes != 0
                          && payload_bytes > max_message_bytes))) {
            errno = EMSGSIZE;
            if (admission_out_)
                *admission_out_ = pipe_message_admission_too_large;
            return false;
        }

        if (unlikely (!can_commit_bytes_with_peer_snapshot_unlocked (
                        frame_bytes, payload_bytes, true))) {
            bool credit_ready = false;
            if (_bytes_written.load (std::memory_order_acquire)
                > _peers_bytes_read.load (std::memory_order_acquire)) {
                arm_hwm_credit_wait_unlocked ();
                credit_ready = can_commit_bytes_with_peer_snapshot_unlocked (
                  frame_bytes, payload_bytes, true);
                if (credit_ready)
                    clear_hwm_credit_wait_unlocked ();
            }
            if (!credit_ready) {
                errno = EAGAIN;
                if (admission_out_)
                    *admission_out_ = pipe_message_admission_hwm_full;
                return false;
            }
        }

        _out_pipe->write (*msg_, false);
        const uint64_t bytes_written =
          _bytes_written.load (std::memory_order_acquire);
        const uint64_t peers_bytes_read =
          _peers_bytes_read.load (std::memory_order_acquire);
        const uint64_t in_flight =
          bytes_written > peers_bytes_read
            ? bytes_written - peers_bytes_read
            : 0;
        if (hwm > 0 && in_flight == 0 && frame_bytes > hwm) {
            record_oversize_message_admission (frame_bytes);
        }
        const uint64_t msgs_written =
          _msgs_written.load (std::memory_order_acquire);
        const uint64_t new_bytes_written =
          UINT64_MAX - bytes_written < frame_bytes
            ? UINT64_MAX
            : bytes_written + frame_bytes;
        uint64_t new_msgs_written = msgs_written;
        if (!msg_->is_routing_id () && !msg_->is_credential ())
            ++new_msgs_written;
        publish_outbound_ledger_unlocked (new_msgs_written,
                                          new_bytes_written);
        _out_complete_record_pending = true;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_ready;
        publish_outbound_accounting_unlocked (false);
        flush_unlocked ();
        return true;
    }

    if (unlikely (!hwm_credit_ready_unlocked (admission_out_)))
        return false;

    if (!write_message_unlocked (msg_, true, false, admission_out_))
        return false;
    flush_unlocked ();
    return true;
}

bool zlink::pipe_t::write_message_observed (
  const msg_t *msg_, pipe_write_observer_fn observer_,
  void *observer_userdata_, pipe_message_admission_t *admission_out_)
{
    if (!observer_ || !msg_) {
        errno = EINVAL;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_invalid;
        return false;
    }
    if (!observer_ (this, observer_userdata_,
                    pipe_write_observer_prepare)) {
        const int observer_errno = errno;
        if (admission_out_) {
            *admission_out_ = observer_errno == ENOBUFS
                                ? pipe_message_admission_request_full
                              : observer_errno == EAGAIN
                                ? pipe_message_admission_transport_wait
                              : observer_errno == EHOSTUNREACH
                                ? pipe_message_admission_inactive
                                : pipe_message_admission_invalid;
        }
        errno = observer_errno == ENOBUFS ? EAGAIN : observer_errno;
        return false;
    }
    if (!retain_lifetime_ref ()) {
        (void) observer_ (this, observer_userdata_,
                          pipe_write_observer_finish);
        errno = EHOSTUNREACH;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_inactive;
        return false;
    }

    bool written = false;
    bool committed = false;
    {
        written = admit_write_unlocked (admission_out_);
        const bool more = (msg_->flags () & msg_t::more) != 0;
        if (written)
            written = write_message_unlocked (msg_, true, more,
                                               admission_out_);
        if (written) {
            committed = observer_ (this, observer_userdata_,
                                   pipe_write_observer_commit);
            zlink_assert (committed);
            if (committed && !more)
                flush_unlocked ();
        }
    }

    (void) observer_ (this, observer_userdata_, pipe_write_observer_finish);
    release_lifetime_ref ();
    if (written && !committed) {
        terminate (false);
        errno = EPROTO;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_invalid;
        return false;
    }
    return written;
}


uint64_t zlink::pipe_t::frame_accounted_bytes (const msg_t *msg_)
{
    const uint64_t metadata_bytes = static_cast<uint64_t> (sizeof (msg_t));
    if (msg_->is_delimiter ())
        return metadata_bytes;
    const uint64_t payload_bytes = static_cast<uint64_t> (msg_->size ());
    return UINT64_MAX - payload_bytes < metadata_bytes
             ? UINT64_MAX
             : payload_bytes + metadata_bytes;
}

uint64_t zlink::pipe_t::committed_frame_accounted_bytes_ref (
  const msg_t &msg_)
{
    return msg_.is_delimiter () ? 0 : frame_accounted_bytes (&msg_);
}

bool zlink::pipe_t::counted_pending_message_ref (const msg_t &msg_)
{
    return (msg_.flags () & msg_t::more) == 0 && !msg_.is_routing_id ()
           && !msg_.is_credential () && !msg_.is_delimiter ();
}

zlink::ypipe_replacement_accounting_t
zlink::pipe_t::publish_outbound_frame_unlocked (const msg_t &msg_, bool more_)
{
    ypipe_replacement_accounting_t replaced;
    if (!_conflate) {
        _out_pipe->write (msg_, more_);
        return replaced;
    }

    _out_pipe->write_with_replacement_accounting (
      msg_, more_, &pipe_t::committed_frame_accounted_bytes_ref,
      &pipe_t::counted_pending_message_ref, &replaced);
    if (_registry_accounting && replaced.bytes > 0)
        get_ctx ()->_physical_queue_registry.release_committed_frame (
          _out_physical_queue, replaced.bytes, replaced.complete_messages);
    return replaced;
}

void zlink::pipe_t::release_discarded_pipe_accounting (
  upipe_t *pipe_, const std::shared_ptr<physical_queue_record_t> &queue_)
{
    if (!pipe_)
        return;
    ypipe_replacement_accounting_t discarded;
    pipe_->discard_accounting (&pipe_t::committed_frame_accounted_bytes_ref,
                               &pipe_t::counted_pending_message_ref,
                               &discarded);
    if (discarded.bytes > 0 && _registry_accounting)
        get_ctx ()->_physical_queue_registry.release_committed_frame (
          queue_, discarded.bytes, discarded.complete_messages);
}

void zlink::pipe_t::set_max_message_bytes (uint64_t max_message_bytes_)
{
    // A completed transport handshake can update the socket-owned endpoint
    // after it is writable. A new record observes this publication with an
    // acquire load; an already-admitted complete record keeps its admission
    // snapshot through commit.
    _max_message_bytes.store (max_message_bytes_, std::memory_order_release);
}

bool zlink::pipe_t::write_message_unlocked (const msg_t *msg_,
                                            bool enforce_hwm_,
                                            bool enforce_incremental_hwm_,
                                            pipe_message_admission_t *admission_out_,
                                            const uint64_t *max_message_bytes_snapshot_)
{
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    const uint64_t max_message_bytes = max_message_bytes_snapshot_
                                         ? *max_message_bytes_snapshot_
                                         : _max_message_bytes.load (
                                             std::memory_order_acquire);
    const uint64_t hwm = _hwm.load (std::memory_order_acquire);
    const uint64_t incomplete_before = _out_incomplete_bytes;
    const uint64_t payload_before = _out_incomplete_payload_bytes;
    const bool multipart_started_empty_before =
      _out_multipart_started_empty;
    if (!append_outbound_frame_bytes_unlocked (msg_)) {
        if (admission_out_)
            *admission_out_ = pipe_message_admission_too_large;
        return false;
    }
    const uint64_t frame_payload_bytes = static_cast<uint64_t> (msg_->size ());
    if (UINT64_MAX - _out_incomplete_payload_bytes < frame_payload_bytes) {
        _out_incomplete_bytes = incomplete_before;
        errno = EMSGSIZE;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_too_large;
        return false;
    }
    _out_incomplete_payload_bytes += frame_payload_bytes;

    if (max_message_bytes != 0
        && _out_incomplete_payload_bytes > max_message_bytes) {
        _out_incomplete_bytes = incomplete_before;
        _out_incomplete_payload_bytes = payload_before;
        _out_multipart_started_empty = multipart_started_empty_before;
        errno = EMSGSIZE;
        if (admission_out_)
            *admission_out_ = pipe_message_admission_too_large;
        return false;
    }

    const bool more = (msg_->flags () & msg_t::more) != 0;
    const bool commits_bytes = !more && !msg_->is_delimiter ();
    if (more && incomplete_before == 0) {
        refresh_peer_credit_snapshot_unlocked ();
        _out_multipart_started_empty =
          _bytes_written.load (std::memory_order_acquire)
          <= _peers_bytes_read.load (std::memory_order_acquire);
    }
    bool commit_credit_ready =
      !(commits_bytes || enforce_incremental_hwm_) || !enforce_hwm_
      || can_commit_bytes_with_peer_snapshot_unlocked (
        _out_incomplete_bytes, _out_incomplete_payload_bytes,
        !more
          && (incomplete_before == 0 || _out_multipart_started_empty),
        &max_message_bytes);
    if (!commit_credit_ready) {
        const bool exceeds_max_message_size =
          max_message_bytes != 0
          && _out_incomplete_payload_bytes > max_message_bytes;
        if (!exceeds_max_message_size
            && _bytes_written.load (std::memory_order_acquire)
                 > _peers_bytes_read.load (std::memory_order_acquire)) {
            arm_hwm_credit_wait_unlocked ();
            commit_credit_ready =
              can_commit_bytes_with_peer_snapshot_unlocked (
                _out_incomplete_bytes, _out_incomplete_payload_bytes,
                !more
                  && (incomplete_before == 0
                      || _out_multipart_started_empty),
                &max_message_bytes);
            if (commit_credit_ready)
                clear_hwm_credit_wait_unlocked ();
        }
        if (!commit_credit_ready) {
            _out_incomplete_bytes = incomplete_before;
            _out_incomplete_payload_bytes = payload_before;
            _out_multipart_started_empty = multipart_started_empty_before;
            errno = exceeds_max_message_size ? EMSGSIZE : EAGAIN;
            if (admission_out_)
                *admission_out_ = exceeds_max_message_size
                                    ? pipe_message_admission_too_large
                                    : pipe_message_admission_hwm_full;
            return false;
        }
    }

    const uint64_t bytes_written =
      _bytes_written.load (std::memory_order_acquire);
    const uint64_t peers_bytes_read =
      _peers_bytes_read.load (std::memory_order_acquire);
    if (_registry_accounting) {
        const uint64_t frame_bytes = frame_accounted_bytes (msg_);
        if (more) {
            get_ctx ()->_physical_queue_registry.account_provisional_frame (
              _out_physical_queue, frame_bytes);
        } else if (!msg_->is_delimiter ()) {
            const uint64_t in_flight =
              bytes_written > peers_bytes_read
                ? bytes_written - peers_bytes_read
                : 0;
            const bool oversize_admission =
              enforce_hwm_ && hwm > 0 && in_flight == 0
              && (_out_incomplete_bytes > hwm
                  || UINT64_MAX - in_flight < _out_incomplete_bytes
                  || in_flight + _out_incomplete_bytes > hwm);
            get_ctx ()->_physical_queue_registry.commit_message (
              _out_physical_queue, frame_bytes,
              counted_pending_message_ref (*msg_),
              oversize_admission);
        }
    }
    const ypipe_replacement_accounting_t replaced =
      publish_outbound_frame_unlocked (*msg_, more);
    if (commits_bytes) {
        const uint64_t message_bytes = _out_incomplete_bytes;
        const uint64_t in_flight =
          bytes_written > peers_bytes_read
            ? bytes_written - peers_bytes_read
            : 0;
        if (enforce_hwm_ && hwm > 0 && in_flight == 0
            && (message_bytes > hwm
                || (UINT64_MAX - in_flight < message_bytes
                    || in_flight + message_bytes > hwm))) {
            record_oversize_message_admission (message_bytes);
        }
        uint64_t new_msgs_written =
          _msgs_written.load (std::memory_order_acquire);
        uint64_t new_bytes_written;
        // Replacements retire only records still owned by the queue. Bytes
        // already read, other topics and a started record keep their charge.
        const uint64_t retained_bytes = bytes_written - replaced.bytes;
        new_msgs_written -= replaced.complete_messages;
        new_bytes_written =
          UINT64_MAX - retained_bytes < message_bytes
            ? UINT64_MAX : retained_bytes + message_bytes;
        if (!msg_->is_routing_id () && !msg_->is_credential ())
            ++new_msgs_written;
        publish_outbound_ledger_unlocked (new_msgs_written,
                                          new_bytes_written);
        _out_complete_record_pending = true;
        _out_incomplete_bytes = 0;
        _out_incomplete_payload_bytes = 0;
        _out_multipart_started_empty = false;
        //  The message the owner started is now committed, so the next one
        //  faces the remote flow state again.
        _out_owner_message_started = false;
        _out_owner_message_start_pending = false;
    }
    if (admission_out_)
        *admission_out_ = pipe_message_admission_ready;
    publish_outbound_accounting_unlocked (
      more || incomplete_before != 0);
#ifdef ZLINK_BUILD_TESTS
    const pipe_write_commit_test_hook_fn commit_hook =
      pipe_write_commit_test_hook.load (std::memory_order_acquire);
    if (commit_hook)
        commit_hook (
          this, more,
          pipe_write_commit_test_hook_userdata.load (std::memory_order_acquire));
#endif
    return true;
}


void zlink::pipe_t::snapshot_outbound_queue_accounting (
  const pipe_t *reader_, uint64_t *provisional_out_,
  uint64_t *committed_out_) const
{
    if (provisional_out_)
        *provisional_out_ = 0;
    if (committed_out_)
        *committed_out_ = 0;

    uint64_t completed_read = 0;
    uint64_t partial_read = 0;
    if (reader_) {
        completed_read = reader_->_published_bytes_read.load (
          std::memory_order_acquire);
        partial_read = reader_->_published_incomplete_bytes_read.load (
          std::memory_order_acquire);
    }
    const uint64_t consumed =
      UINT64_MAX - completed_read < partial_read
        ? UINT64_MAX
        : completed_read + partial_read;

    // Every endpoint owner publishes this pair after each outbound state
    // transition. Load the classification first: a newer hint synchronizes
    // with the preceding total publication; an older hint remains safe after
    // clamping it inside the authoritative total.
    const uint64_t published_provisional =
      _published_outbound_provisional_bytes.load (std::memory_order_acquire);
    const uint64_t published_total =
      _published_outbound_total_bytes.load (std::memory_order_acquire);
    const uint64_t available =
      published_total > consumed ? published_total - consumed : 0;
    const uint64_t provisional = std::min (published_provisional, available);

    if (provisional_out_)
        *provisional_out_ = provisional;
    if (committed_out_)
        *committed_out_ = available - provisional;
}

void zlink::pipe_t::rollback_unlocked (bool publish_peer_control_)
{
    //  Remove incomplete message from the outbound pipe.
    msg_t msg;
    if (_out_pipe) {
        while (_out_pipe->unwrite (&msg)) {
            zlink_assert (msg.flags () & msg_t::more);
            const int rc = msg.close ();
            errno_assert (rc == 0);
        }
    }
    if (_out_incomplete_bytes > 0 && _registry_accounting)
        get_ctx ()->_physical_queue_registry.rollback_provisional (
          _out_physical_queue, _out_incomplete_bytes);
    _out_incomplete_bytes = 0;
    _out_incomplete_payload_bytes = 0;
    _out_multipart_started_empty = false;
    _decoder_multipart_started_empty = false;
    //  The owner's accepted-but-unwritten part is gone with the rest of the
    //  message, so the in-progress exception ends here.
    _out_owner_message_started = false;
    _out_owner_message_start_pending = false;
    publish_outbound_accounting_unlocked (true);
    if (pending_peer_controls_unlocked ()
        && peer_control_slots_enabled_unlocked ()) {
        // A public Application rollback keeps the latest absolute controls
        // and publishes them at the now-clean message boundary.
        // Termination/disconnect rollbacks must not leak control into a
        // retiring generation; its normal reconnect/pair-ready resync owns
        // publication on the replacement transport.
        if (publish_peer_control_ && _state == active)
            (void) flush_pending_peer_controls_unlocked ();
        else
            discard_pending_peer_controls_unlocked ();
    }
}

void zlink::pipe_t::flush_unlocked ()
{
    //  The peer does not exist anymore at this point.
    if (_state == term_ack_sent)
        return;

    // Check the concrete slots first. Ordinary PAIR/PUB/SUB/XPUB/XSUB/STREAM
    // sends never populate them, so their hot flush path does not even load
    // pair/type policy state. Only a validated D/R control writer can make the
    // slower topology check reachable.
    const bool publish_pending_controls =
      unlikely (pending_peer_controls_unlocked ())
      && peer_control_slots_enabled_unlocked ()
      && _out_incomplete_bytes == 0 && _out_incomplete_payload_bytes == 0;
    // Network controls are records in the same physical FIFO, so append them
    // before publishing the boundary. Inproc controls are owner commands;
    // publish preceding Application bytes first, then enqueue the commands on
    // the peer owner mailbox in the same surviving-slot order.
    if (publish_pending_controls && _session_pipe)
        (void) append_pending_peer_controls_unlocked ();
    const bool completed_record = _out_complete_record_pending;
    _out_complete_record_pending = false;
    const bool sleeping = _out_pipe && !_out_pipe->flush ();
    const bool reclassify_candidate =
      !sleeping && completed_record
      && peer_uses_routed_protocol_unlocked ()
      && _transport_pair_id != 0
      && _transport_lane == transport_lane_application
      && _transport_lane_count.load (std::memory_order_acquire) == 1u
      && (!_session_pipe || _session_io_writer);
    pipe_t *const peer =
      sleeping || reclassify_candidate ? get_peer () : NULL;
    bool explicit_reclassify = false;
    if (reclassify_candidate && peer
        && peer->_head_reclassify_wake.load (std::memory_order_acquire)
             == head_reclassify_armed) {
        unsigned char expected = head_reclassify_armed;
        explicit_reclassify =
          peer->_head_reclassify_wake.compare_exchange_strong (
            expected, head_reclassify_queued, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
    if (sleeping || explicit_reclassify)
        send_activate_read (peer);
    if (publish_pending_controls && !_session_pipe)
        (void) dispatch_pending_inproc_controls_unlocked ();
}

void zlink::pipe_t::publish_outbound_accounting_unlocked (
  bool provisional_changed_)
{
    const uint64_t bytes_written =
      _bytes_written.load (std::memory_order_acquire);
    const uint64_t total =
      UINT64_MAX - bytes_written < _out_incomplete_bytes
        ? UINT64_MAX
        : bytes_written + _out_incomplete_bytes;
    // Publish the authoritative total first. A concurrent snapshot can retain
    // an older provisional classification, but clamps it inside this total, so
    // Auto-HWM never double-counts or reads the decoder-owned local fields.
    _published_outbound_total_bytes.store (total, std::memory_order_release);
    if (provisional_changed_)
        _published_outbound_provisional_bytes.store (
          _out_incomplete_bytes, std::memory_order_release);
}
