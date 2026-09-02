/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <vector>

#include "core/flow_state_frame.hpp"
#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"
#include "utils/likely.hpp"
#include "zlink.h"

bool zlink::socket_base_t::socket_type_supports_receive_flow_state (int type_)
{
    //  Only DEALER/ROUTER sockets own paired receive-flow state. Count-1
    //  peers use their Application control path; count-2 peers retain the
    //  dedicated Completion path. Other socket patterns keep their existing
    //  byte HWM and transport backpressure unchanged.
    return type_ == ZLINK_CORE_SOCKET_DEALER || type_ == ZLINK_CORE_SOCKET_ROUTER;
}

int zlink::socket_base_t::set_local_receive_flow_state (int state_)
{
    if (state_ != flow_state::receive_flow_running
        && state_ != flow_state::receive_flow_paused) {
        errno = EINVAL;
        return -1;
    }
    if (!socket_type_supports_receive_flow_state (options.type)) {
        errno = ENOTSUP;
        return -1;
    }

    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    socket_public_api_scope_t admission (lifecycle);
    //  Close and this call race for the same socket state. Only whichever
    //  operation is admitted first is observable.
    if (!admission.acquired ())
        return -1;

    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    const unsigned char state = static_cast<unsigned char> (state_);
    uint64_t epoch = 0;
    bool wrapped = false;
    bool defer_application_controls = false;
    std::vector<pipe_t *> targets;
    {
        socket_public_api_lock_scope_t guard (lifecycle);
        scoped_lock_t lock (_transport_pairs_sync);
        if (_local_receive_flow_epoch != 0
            && _local_receive_flow_state == state) {
            //  Absolute state, not a counter: repeating the current state is a
            //  successful no-op and emits nothing.
            return 0;
        }
        _local_receive_flow_state = state;
        //  Continuing the sequence past the top would emit epochs an existing
        //  receiver rejects as stale, freezing every current pair until its
        //  generation changes anyway. Restart the sequence and force that
        //  generation change instead: the pairs are torn down, reconnect
        //  brings a fresh generation, and a fresh generation accepts the
        //  sequence from the start. 0 stays reserved as "never set".
        wrapped = _local_receive_flow_epoch == UINT64_MAX;
        _local_receive_flow_epoch = wrapped ? 1 : _local_receive_flow_epoch + 1;
        epoch = _local_receive_flow_epoch;
        defer_application_controls =
          lifecycle.public_multipart_send_active ();
        for (transport_pairs_t::iterator it = _transport_pairs.begin (),
                                         end = _transport_pairs.end ();
             it != end; ++it) {
            //  The table slot is what proves the pipe is still alive; the pipe
            //  itself is used after the table is unlocked.
            pipe_t *target = wrapped && it->second.application
                               ? it->second.application
                               : it->second.completion_source ();
            if (!it->second.ready || !target)
                continue;
            if (target->retain_lifetime_ref ())
                targets.push_back (target);
        }
    }

    for (size_t i = 0; i < targets.size (); ++i) {
        if (wrapped)
            targets[i]->terminate (false);
        else
            (void) targets[i]->write_flow_state_control_and_flush (
              state, epoch,
              defer_application_controls
                && targets[i]->get_transport_lane_count () == 1u);
        targets[i]->release_lifetime_ref ();
    }
    return 0;
}

int zlink::socket_base_t::get_local_receive_flow_state () const
{
    scoped_lock_t lock (_transport_pairs_sync);
    return static_cast<int> (_local_receive_flow_state);
}

bool zlink::socket_base_t::remote_receive_flow_paused (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_) const
{
    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pairs_t::const_iterator it = _transport_pairs.find (
      transport_pair_key_t (transport_pair_id_, transport_pair_generation_));
    return it != _transport_pairs.end () && it->second.remote_flow_paused;
}

bool zlink::socket_base_t::application_pipe_remote_flow_paused (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_) const
{
    pipe_t *application = NULL;
    {
        scoped_lock_t lock (_transport_pairs_sync);
        const transport_pairs_t::const_iterator it = _transport_pairs.find (
          transport_pair_key_t (transport_pair_id_, transport_pair_generation_));
        if (it == _transport_pairs.end () || !it->second.application)
            return false;
        application = it->second.application;
        if (!application->retain_lifetime_ref ())
            return false;
    }
    const bool paused = application->remote_flow_paused ();
    application->release_lifetime_ref ();
    return paused;
}

void zlink::socket_base_t::write_receive_flow_state_frame (
  pipe_t *control_pipe_, unsigned char state_, uint64_t epoch_)
{
    if (!control_pipe_ || !flow_state::state_valid (state_) || epoch_ == 0)
        return;

    const unsigned char lane_count =
      control_pipe_->get_transport_lane_count ();
    const transport_lane_t lane = control_pipe_->get_transport_lane ();
    if (control_pipe_->get_transport_pair_id () == 0
        || control_pipe_->get_transport_pair_generation () == 0
        || control_pipe_->get_transport_connection_id () == 0
        || (lane_count == 1u && lane != transport_lane_application)
        || (lane_count == 2u && lane != transport_lane_completion)
        || (lane_count != 1u && lane_count != 2u))
        return;

    // The pipe owns boundary staging and HWM/PAUSE bypass for both topology
    // selections. Network pipes append the wire command to their physical
    // FIFO; inproc pipes dispatch the same absolute state to the exact peer
    // lane's owner without exposing a public record.
    (void) control_pipe_->write_flow_state_control_and_flush (state_, epoch_);
}

void zlink::socket_base_t::sync_local_receive_flow_state_to_pair (
  pipe_t *control_pipe_)
{
    if (!control_pipe_)
        return;

    unsigned char state = flow_state::receive_flow_running;
    uint64_t epoch = 0;
    {
        scoped_lock_t lock (_transport_pairs_sync);
        //  A socket that never set a state has nothing to synchronise: RUNNING
        //  is what the peer already assumes for a fresh pair.
        if (_local_receive_flow_epoch == 0)
            return;
        state = _local_receive_flow_state;
        epoch = _local_receive_flow_epoch;
    }
    write_receive_flow_state_frame (control_pipe_, state, epoch);
}

bool zlink::socket_base_t::consume_receive_flow_state_frame (
  pipe_t *source_pipe_, const zlink::msg_t &msg_)
{
    flow_state::frame_t frame;
    const flow_state::decode_result_t decoded =
      flow_state::decode_frame (msg_, &frame);
    if (decoded == flow_state::decode_not_flow_frame)
        return false;
    //  An unsupported version or a malformed frame is still ours: it is
    //  consumed here and never handed to another frame handler.
    if (decoded != flow_state::decode_ok || !source_pipe_)
        return true;

    //  The lane and physical connection of the receiving pipe are local truth
    //  the peer cannot influence. FLOWSTATE carries no pair identity on the
    //  wire. Count 1 admits it only from Application; count 2 admits it only
    //  from Completion.
    const unsigned char lane_count = source_pipe_->get_transport_lane_count ();
    const transport_lane_t source_lane = source_pipe_->get_transport_lane ();
    if ((lane_count == 1u && source_lane != transport_lane_application)
        || (lane_count == 2u && source_lane != transport_lane_completion)
        || (lane_count != 1u && lane_count != 2u))
        return true;

    const uint64_t pair_id = source_pipe_->get_transport_pair_id ();
    const uint64_t generation =
      source_pipe_->get_transport_pair_generation ();
    const uint64_t source_connection_id = msg_.transport_connection_id ();
    const uint64_t current_connection_id =
      source_pipe_->get_transport_connection_id ();
    if (pair_id == 0 || generation == 0)
        return true;
    //  A retired physical connection is consumed without a public event. It
    //  has no current peer identity, but still contributes to the diagnostic
    //  stale counter.
    if (source_connection_id == 0
        || source_connection_id != current_connection_id
        || !source_pipe_->is_lifecycle_active ()) {
        note_flow_state_stale (true, generation, generation, frame.epoch, 0,
                               pair_id, NULL);
        return true;
    }

    const bool paused = frame.state == flow_state::receive_flow_paused;
    pipe_t *application = NULL;
    {
        scoped_lock_t lock (_transport_pairs_sync);
        // Termination removes this exact table entry under the same mutex.
        // Recheck liveness while holding it before operator[] can create a
        // pre-attach record; if termination starts afterwards, its cleanup is
        // ordered after this update and removes the record again.
        if (!source_pipe_->is_lifecycle_active ())
            return true;
        //  The transport I/O thread can decode the frame before this socket's
        //  mailbox has admitted the same physical connection. Create the pair
        //  record when needed, but do not accept the state until validation
        //  identifies this exact topology-selected connection as the winner. The
        //  unregistered frame itself therefore produces no state transition,
        //  metric, or event.
        transport_pair_pipes_t &pair =
          _transport_pairs[transport_pair_key_t (pair_id, generation)];
        pipe_t *const registered_source =
          lane_count == 1u ? pair.application : pair.completion;
        if (!source_pipe_->is_lifecycle_active ()
            || source_pipe_->get_transport_connection_id ()
                 != source_connection_id
            || (pair.expected_lane_count != 0u
                && pair.expected_lane_count != lane_count)
            || (registered_source && registered_source != source_pipe_)) {
            _flow_state_stale_total.fetch_add (1, std::memory_order_relaxed);
            return true;
        }
        if (!pair.ready || registered_source != source_pipe_) {
            buffer_pending_flow_state_locked (pair, source_connection_id,
                                              paused, frame.epoch);
            return true;
        }

        //  Duplicate or reversed epoch within one generation.
        if (pair.remote_flow_seen && frame.epoch <= pair.remote_flow_epoch) {
            note_flow_state_stale (false, generation, generation,
                                   frame.epoch, pair.remote_flow_epoch,
                                   pair_id, pair.application);
            return true;
        }
        pair.remote_flow_epoch = frame.epoch;
        pair.remote_flow_seen = true;
        if (pair.remote_flow_paused == paused)
            return true;
        pair.remote_flow_paused = paused;
        application = pair.application;
        if (application && !application->retain_lifetime_ref ())
            application = NULL;
    }

    if (application) {
        send_flow_state (application,
                         paused ? static_cast<unsigned char> (1)
                                : static_cast<unsigned char> (0),
                         frame.epoch);
        application->release_lifetime_ref ();
    }
    return true;
}

void zlink::socket_base_t::buffer_pending_flow_state_locked (
  transport_pair_pipes_t &pair_, uint64_t source_connection_id_, bool paused_,
  uint64_t epoch_)
{
    if (source_connection_id_ == 0)
        return;

    //  Update only this candidate's slot. A losing connection must never burn
    //  the winner's epoch or overwrite its one-shot absolute state.
    for (size_t i = 0; i < transport_pair_pipes_t::pending_flow_slot_count;
         ++i) {
        transport_pair_pipes_t::pending_flow_slot_t &slot = pair_.pending_flow[i];
        if (!slot.valid || slot.source_connection_id != source_connection_id_)
            continue;
        if (epoch_ <= slot.epoch)
            return;
        slot.paused = paused_;
        slot.epoch = epoch_;
        return;
    }
    for (size_t i = 0; i < transport_pair_pipes_t::pending_flow_slot_count;
         ++i) {
        transport_pair_pipes_t::pending_flow_slot_t &slot = pair_.pending_flow[i];
        if (slot.valid)
            continue;
        slot.valid = true;
        slot.paused = paused_;
        slot.epoch = epoch_;
        slot.source_connection_id = source_connection_id_;
        return;
    }
    //  The bounded candidate set is full. Refuse the newcomer instead of
    //  evicting a possible winner; only a validated source can be promoted.
}

void zlink::socket_base_t::promote_pending_flow_state_locked (
  transport_pair_pipes_t &pair_)
{
    bool found = false;
    bool paused = false;
    uint64_t epoch = 0;
    pipe_t *const winner = pair_.completion_source ();
    const uint64_t winner_connection_id =
      winner ? winner->get_transport_connection_id () : 0;
    for (size_t i = 0; i < transport_pair_pipes_t::pending_flow_slot_count;
         ++i) {
        transport_pair_pipes_t::pending_flow_slot_t &slot = pair_.pending_flow[i];
        if (slot.valid && winner_connection_id != 0
            && slot.source_connection_id == winner_connection_id) {
            found = true;
            paused = slot.paused;
            epoch = slot.epoch;
        }
        slot = transport_pair_pipes_t::pending_flow_slot_t ();
    }

    if (!found)
        return;
    if (pair_.remote_flow_seen && epoch <= pair_.remote_flow_epoch)
        return;
    pair_.remote_flow_epoch = epoch;
    pair_.remote_flow_seen = true;
    pair_.remote_flow_paused = paused;
}

void zlink::socket_base_t::discard_pending_flow_state_locked (
  transport_pair_pipes_t &pair_, uint64_t source_connection_id_,
  bool discard_all_)
{
    for (size_t i = 0; i < transport_pair_pipes_t::pending_flow_slot_count;
         ++i) {
        transport_pair_pipes_t::pending_flow_slot_t &slot = pair_.pending_flow[i];
        if (!slot.valid)
            continue;
        if (discard_all_ || slot.source_connection_id == source_connection_id_)
            slot = transport_pair_pipes_t::pending_flow_slot_t ();
    }
}

void zlink::socket_base_t::flow_state_applied (
  pipe_t *pipe_, bool paused_, uint64_t epoch_, bool actual_writable_)
{
    //  Called synchronously, on this pipe's own (socket-owning) thread, right
    //  after pipe_t::process_flow_state() commits a real PAUSED<->RUNNING
    //  flip. Never on a stale, duplicate, or same-state frame, and never on a
    //  normal data frame - so this never runs on the per-message path.
    if (!pipe_)
        return;

    //  Flow-state receipt retains this pipe and queues the command after it has
    //  left the table mutex, so a concurrent termination can clear the pair and
    //  settle its accounting before the command runs. The retain keeps the pipe
    //  alive for exactly that long, so a late command would otherwise report
    //  against a pair that no longer exists: a late RUNNING would release a
    //  count this pair no longer holds - and the gauge is socket-wide, so it
    //  would take another pair's - while a late PAUSE would add one that
    //  nothing will ever release.
    //
    //  The reporting pipe must therefore still be this pair's registered
    //  application pipe. If it is not, the termination path has already settled
    //  everything and this command has nothing left to do: no gauge, no totals,
    //  no duration, no marker and no event.
    //
    //  The marker moves inside the same critical section as that check, so it
    //  can never disagree with the gauge about whether a pause is counted.
    {
        scoped_lock_t lock (_transport_pairs_sync);
        const transport_pairs_t::iterator it =
          _transport_pairs.find (transport_pair_key_t (
            pipe_->get_transport_pair_id (),
            pipe_->get_transport_pair_generation ()));
        if (it == _transport_pairs.end () || it->second.application != pipe_)
            return;
        // Network decode records this before queuing the pipe command. Inproc
        // delivers the same control directly to the exact Application owner,
        // so commit the generation-scoped pair record here as well.
        if (!it->second.remote_flow_seen
            || epoch_ > it->second.remote_flow_epoch) {
            it->second.remote_flow_seen = true;
            it->second.remote_flow_epoch = epoch_;
            it->second.remote_flow_paused = paused_;
        }
        it->second.remote_flow_pause_accounted = paused_;
    }

    if (paused_) {
        _flow_paused_connections.fetch_add (1, std::memory_order_relaxed);
        _flow_pause_applied_total.fetch_add (1, std::memory_order_relaxed);
        pipe_->set_remote_flow_pause_started_ms (_clock.now_ms ());
    } else {
        //  The gauge only ever moves in matched +1/-1 steps from this same
        //  thread (process_flow_state() is delivered through this socket's
        //  own command queue), so a plain decrement guarded against underflow
        //  is enough; no CAS retry loop is needed.
        uint64_t paused_connections =
          _flow_paused_connections.load (std::memory_order_relaxed);
        if (paused_connections > 0)
            _flow_paused_connections.fetch_sub (1, std::memory_order_relaxed);
        _flow_resume_applied_total.fetch_add (1, std::memory_order_relaxed);
        const uint64_t started_ms = pipe_->remote_flow_pause_started_ms ();
        if (started_ms != 0) {
            const uint64_t now_ms = _clock.now_ms ();
            _flow_last_pause_duration_ms.store (
              now_ms > started_ms ? now_ms - started_ms : 0,
              std::memory_order_relaxed);
        }
    }

    const blob_t &routing_id = pipe_->get_routing_id ();
    const unsigned char *routing_id_data =
      routing_id.size () > 0 ? routing_id.data () : NULL;
    //  The public event carries one value, so the epoch takes it and the
    //  RESUMED-only "did this actually make the pipe writable" answer travels
    //  in the event's documented event-specific flags field. Routing ID, pair
    //  ID and generation are separate fields of the same event, so plan 6's
    //  whole field list reaches an operator without widening the ABI.
    uint64_t values[1] = {epoch_};
    const uint32_t flags =
      !paused_ && actual_writable_
        ? static_cast<uint32_t> (socket_monitor_internal_send_flow_writable)
        : 0u;
    endpoint_uri_pair_t endpoint_pair = pipe_->get_endpoint_pair ();
    endpoint_pair.connection_id = pipe_->get_transport_connection_id ();
    event (endpoint_pair, routing_id_data, routing_id.size (), values, 1,
          paused_ ? ZLINK_EVENT_SEND_FLOW_PAUSED : ZLINK_EVENT_SEND_FLOW_RESUMED,
          flags, pipe_->get_transport_lane (), pipe_->get_transport_pair_id (),
          pipe_->get_transport_pair_generation ());
}

void zlink::socket_base_t::flow_state_received (
  pipe_t *source_pipe_, unsigned char state_, uint64_t epoch_)
{
    if (!source_pipe_ || !flow_state::state_valid (state_) || epoch_ == 0
        || source_pipe_->get_transport_lane ()
             != transport_lane_completion
        || source_pipe_->get_transport_lane_count () != 2u
        || source_pipe_->get_transport_connection_id () == 0
        || !source_pipe_->is_lifecycle_active ())
        return;

    pipe_t *application = NULL;
    {
        scoped_lock_t lock (_transport_pairs_sync);
        const transport_pairs_t::const_iterator it = _transport_pairs.find (
          transport_pair_key_t (source_pipe_->get_transport_pair_id (),
                                source_pipe_->get_transport_pair_generation ()));
        if (it == _transport_pairs.end () || !it->second.ready
            || it->second.expected_lane_count != 2u
            || it->second.completion != source_pipe_
            || !it->second.application
            || !source_pipe_->is_lifecycle_active ())
            return;
        application = it->second.application;
        if (!application->retain_lifetime_ref ())
            return;
    }

    // Keep the same Application-owner command boundary used by network decode.
    // A detach after the exact source check is fenced again when the command
    // reaches pipe_t::process_flow_state() and flow_state_applied().
    send_flow_state (application, state_, epoch_);
    application->release_lifetime_ref ();
}

void zlink::socket_base_t::flow_pause_released_on_termination (pipe_t *pipe_)
{
    //  A pair torn down while paused never reaches a RESUMED, so nothing else
    //  would ever match its +1 on the gauge and repeated disconnect-while-
    //  paused would drift it upwards for good. This is a lifecycle release,
    //  not a resume: it closes the pause measurement and frees the gauge slot
    //  but raises no RESUMED event and does not count as a resume applied,
    //  because the peer never resumed anything.
    uint64_t paused_connections =
      _flow_paused_connections.load (std::memory_order_relaxed);
    if (paused_connections > 0)
        _flow_paused_connections.fetch_sub (1, std::memory_order_relaxed);

    if (!pipe_)
        return;
    const uint64_t started_ms = pipe_->remote_flow_pause_started_ms ();
    if (started_ms != 0) {
        const uint64_t now_ms = _clock.now_ms ();
        _flow_last_pause_duration_ms.store (
          now_ms > started_ms ? now_ms - started_ms : 0,
          std::memory_order_relaxed);
        pipe_->set_remote_flow_pause_started_ms (0);
    }
}

void zlink::socket_base_t::note_flow_state_stale (
  bool generation_stale_, uint64_t received_generation_,
  uint64_t current_generation_, uint64_t received_epoch_,
  uint64_t current_epoch_, uint64_t pair_id_,
  pipe_t *application_pipe_)
{
    _flow_state_stale_total.fetch_add (1, std::memory_order_relaxed);
    //  A frame for another physical connection has no public peer identity.
    //  It contributes to the diagnostic counter only.
    if (generation_stale_ || !application_pipe_)
        return;

    endpoint_uri_pair_t endpoint_pair = application_pipe_->get_endpoint_pair ();
    endpoint_pair.connection_id =
      application_pipe_->get_transport_connection_id ();
    const blob_t &routing_id = application_pipe_->get_routing_id ();
    const unsigned char *routing_id_data =
      routing_id.size () > 0 ? routing_id.data () : NULL;
    uint64_t values[4] = {received_epoch_, received_generation_, received_epoch_,
                          current_epoch_};
    event (endpoint_pair, routing_id_data, routing_id.size (), values, 4,
           ZLINK_EVENT_FLOW_STATE_STALE,
           socket_monitor_internal_flow_state_stale_epoch,
           transport_lane_application, pair_id_, current_generation_);
}

void zlink::socket_base_t::flow_state_metrics (
  uint64_t *paused_connections_, uint64_t *pause_applied_total_,
  uint64_t *resume_applied_total_, uint64_t *stale_total_,
  uint64_t *last_pause_duration_ms_) const
{
    if (paused_connections_)
        *paused_connections_ =
          _flow_paused_connections.load (std::memory_order_relaxed);
    if (pause_applied_total_)
        *pause_applied_total_ =
          _flow_pause_applied_total.load (std::memory_order_relaxed);
    if (resume_applied_total_)
        *resume_applied_total_ =
          _flow_resume_applied_total.load (std::memory_order_relaxed);
    if (stale_total_)
        *stale_total_ = _flow_state_stale_total.load (std::memory_order_relaxed);
    if (last_pause_duration_ms_)
        *last_pause_duration_ms_ =
          _flow_last_pause_duration_ms.load (std::memory_order_relaxed);
}

void zlink::socket_base_t::reset_flow_state_metrics ()
{
    _flow_paused_connections.store (0, std::memory_order_relaxed);
    _flow_pause_applied_total.store (0, std::memory_order_relaxed);
    _flow_resume_applied_total.store (0, std::memory_order_relaxed);
    _flow_state_stale_total.store (0, std::memory_order_relaxed);
    _flow_last_pause_duration_ms.store (0, std::memory_order_relaxed);
}

#ifdef ZLINK_BUILD_TESTS
zlink::pipe_t *zlink::socket_base_t::test_pair_pipe (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
  bool completion_lane_) const
{
    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pairs_t::const_iterator it = _transport_pairs.find (
      transport_pair_key_t (transport_pair_id_, transport_pair_generation_));
    if (it == _transport_pairs.end ())
        return NULL;
    return completion_lane_ ? it->second.completion : it->second.application;
}

bool zlink::socket_base_t::test_pair_identity_for_peer (
  const unsigned char *peer_routing_id_, size_t peer_routing_id_size_,
  uint64_t *transport_pair_id_out_,
  uint64_t *transport_pair_generation_out_, bool *ready_out_) const
{
    if ((!peer_routing_id_ && peer_routing_id_size_ != 0)
        || !transport_pair_id_out_
        || !transport_pair_generation_out_)
        return false;

    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pairs_t::const_iterator end = _transport_pairs.end ();
    transport_pairs_t::const_iterator selected = end;
    for (transport_pairs_t::const_iterator it = _transport_pairs.begin ();
         it != end; ++it) {
        pipe_t *const pipe = it->second.application
                               ? it->second.application
                               : it->second.completion;
        if (!pipe)
            continue;
        const blob_t &peer_identity =
          pipe->get_transport_peer_identity ().size () != 0
            ? pipe->get_transport_peer_identity ()
            : pipe->get_routing_id ();
        if (peer_identity.size () != peer_routing_id_size_
            || (peer_routing_id_size_ != 0
                && memcmp (peer_identity.data (), peer_routing_id_,
                           peer_routing_id_size_) != 0))
            continue;
        const bool pair_ready = it->second.ready
                                && it->second.application
                                && it->second.completion_source ();
        if (pair_ready) {
            selected = it;
            break;
        }
        if (selected == end)
            selected = it;
    }
    if (selected == end)
        return false;
    *transport_pair_id_out_ = selected->first.first;
    *transport_pair_generation_out_ = selected->first.second;
    if (ready_out_)
        *ready_out_ = selected->second.ready
                      && selected->second.application
                      && selected->second.completion_source ();
    return true;
}

bool zlink::socket_base_t::test_application_pipe_flow_probe (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
  bool *out_active_, bool *hwm_full_, bool *remote_paused_,
  bool *byte_credit_waiter_, uint64_t *in_flight_bytes_) const
{
    pipe_t *application =
      test_pair_pipe (transport_pair_id_, transport_pair_generation_, false);
    if (!application || !application->retain_lifetime_ref ())
        return false;
    application->test_flow_probe (out_active_, hwm_full_, remote_paused_,
                                  byte_credit_waiter_, in_flight_bytes_);
    application->release_lifetime_ref ();
    return true;
}

bool zlink::socket_base_t::test_deliver_flow_state_command (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
  unsigned char state_, uint64_t epoch_)
{
    pipe_t *application =
      test_pair_pipe (transport_pair_id_, transport_pair_generation_, false);
    if (!application || !application->retain_lifetime_ref ())
        return false;
    send_flow_state (application, state_, epoch_);
    application->release_lifetime_ref ();
    return true;
}

bool zlink::socket_base_t::test_pending_flow_buffered (
  bool *paused_out_, uint64_t *epoch_out_, uint64_t *pair_id_out_,
  uint64_t *generation_out_, uint64_t *source_connection_id_out_) const
{
    scoped_lock_t lock (_transport_pairs_sync);
    for (transport_pairs_t::const_iterator it = _transport_pairs.begin (),
                                           end = _transport_pairs.end ();
         it != end; ++it) {
        for (size_t i = 0; i < transport_pair_pipes_t::pending_flow_slot_count;
             ++i) {
            if (!it->second.pending_flow[i].valid)
                continue;
            if (paused_out_)
                *paused_out_ = it->second.pending_flow[i].paused;
            if (epoch_out_)
                *epoch_out_ = it->second.pending_flow[i].epoch;
            if (pair_id_out_)
                *pair_id_out_ = it->first.first;
            if (generation_out_)
                *generation_out_ = it->first.second;
            if (source_connection_id_out_)
                *source_connection_id_out_ =
                  it->second.pending_flow[i].source_connection_id;
            return true;
        }
    }
    return false;
}

bool zlink::socket_base_t::test_buffer_flow_frame (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
  uint64_t source_connection_id_, bool paused_, uint64_t epoch_)
{
    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pairs_t::iterator it = _transport_pairs.find (
      transport_pair_key_t (transport_pair_id_, transport_pair_generation_));
    if (it == _transport_pairs.end ())
        return false;
    buffer_pending_flow_state_locked (it->second, source_connection_id_,
                                      paused_, epoch_);
    return true;
}
#endif


#ifdef ZLINK_BUILD_TESTS
void zlink::socket_base_t::test_set_local_receive_flow_epoch (uint64_t epoch_)
{
    scoped_lock_t lock (_transport_pairs_sync);
    _local_receive_flow_epoch = epoch_;
}

uint64_t zlink::socket_base_t::test_local_receive_flow_epoch () const
{
    scoped_lock_t lock (_transport_pairs_sync);
    return _local_receive_flow_epoch;
}
#endif

#ifdef ZLINK_BUILD_TESTS
bool zlink::socket_base_t::test_pair_is_ready (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_) const
{
    return transport_pair_is_ready (transport_pair_id_,
                                    transport_pair_generation_);
}
#endif

bool zlink::socket_base_t::transport_pair_is_ready (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_) const
{
    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pairs_t::const_iterator it = _transport_pairs.find (
      transport_pair_key_t (transport_pair_id_, transport_pair_generation_));
    return it != _transport_pairs.end () && it->second.ready;
}

#ifdef ZLINK_BUILD_TESTS
bool zlink::socket_base_t::test_set_pair_received_flow_state (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
  bool paused_)
{
    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pairs_t::iterator it = _transport_pairs.find (
      transport_pair_key_t (transport_pair_id_, transport_pair_generation_));
    if (it == _transport_pairs.end ())
        return false;
    it->second.remote_flow_seen = true;
    it->second.remote_flow_paused = paused_;
    return true;
}
#endif

#ifdef ZLINK_BUILD_TESTS
zlink::pipe_t *zlink::socket_base_t::test_retain_application_pipe (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_)
{
    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pairs_t::const_iterator it = _transport_pairs.find (
      transport_pair_key_t (transport_pair_id_, transport_pair_generation_));
    if (it == _transport_pairs.end () || !it->second.application)
        return NULL;
    pipe_t *application = it->second.application;
    return application->retain_lifetime_ref () ? application : NULL;
}

void zlink::socket_base_t::test_release_pipe (pipe_t *pipe_)
{
    if (pipe_)
        pipe_->release_lifetime_ref ();
}

void zlink::socket_base_t::test_deliver_late_flow_state (pipe_t *pipe_,
                                                         bool paused_,
                                                         uint64_t epoch_)
{
    //  The same call pipe_t::process_flow_state () makes once the queued
    //  command finally runs.
    flow_state_applied (pipe_, paused_, epoch_, false);
}

void zlink::socket_base_t::test_consume_late_flow_state_frame (
  pipe_t *pipe_, bool paused_, uint64_t epoch_)
{
    if (!pipe_)
        return;
    flow_state::frame_t frame;
    frame.state = paused_ ? flow_state::receive_flow_paused
                          : flow_state::receive_flow_running;
    frame.epoch = epoch_;
    msg_t msg;
    const int init_rc = msg.init ();
    errno_assert (init_rc == 0);
    const int frame_rc = flow_state::init_frame (&msg, frame);
    errno_assert (frame_rc == 0);
    msg.set_transport_connection_id (pipe_->get_transport_connection_id ());
    (void) consume_receive_flow_state_frame (pipe_, msg);
    const int close_rc = msg.close ();
    errno_assert (close_rc == 0);
}
#endif
