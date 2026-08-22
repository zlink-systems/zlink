/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <vector>

#include "core/flow_state_frame.hpp"
#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"
#include "utils/likely.hpp"

bool zlink::socket_base_t::socket_type_supports_receive_flow_state (int type_)
{
    //  Only the paired DEALER/ROUTER transports own a completion lane. PAIR,
    //  the PUB/SUB family and STREAM keep their existing byte HWM and
    //  transport backpressure unchanged.
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
        //  0 is the "never set" marker and the frame contract refuses it, so
        //  wrapping into it would silence this socket's flow state for good.
        //  Skip it, exactly like the transport-pair generation does.
        _local_receive_flow_epoch = _local_receive_flow_epoch == UINT64_MAX
                                      ? 1
                                      : _local_receive_flow_epoch + 1;
        epoch = _local_receive_flow_epoch;
        for (transport_pairs_t::iterator it = _transport_pairs.begin (),
                                         end = _transport_pairs.end ();
             it != end; ++it) {
            pipe_t *completion = it->second.completion;
            if (!it->second.ready || !completion)
                continue;
            //  The table slot is what proves the pipe is still alive; the frame
            //  itself is written after the table is unlocked.
            if (completion->retain_lifetime_ref ())
                targets.push_back (completion);
        }
    }

    for (size_t i = 0; i < targets.size (); ++i) {
        write_receive_flow_state_frame (targets[i], state, epoch);
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
  pipe_t *completion_pipe_, unsigned char state_, uint64_t epoch_)
{
    if (!completion_pipe_
        || completion_pipe_->get_transport_lane () != transport_lane_completion)
        return;

    flow_state::frame_t frame;
    frame.version = flow_state::frame_protocol_version;
    frame.state = state_;
    frame.pair_id = completion_pipe_->get_transport_pair_id ();
    frame.generation = completion_pipe_->get_transport_pair_generation ();
    frame.epoch = epoch_;
    if (frame.pair_id == 0 || frame.generation == 0 || frame.epoch == 0)
        return;

    msg_t msg;
    if (msg.init () != 0)
        return;
    if (flow_state::init_frame (&msg, frame) != 0) {
        const int close_rc = msg.close ();
        errno_assert (close_rc == 0);
        return;
    }
    msg.set_transport_connection_id (
      completion_pipe_->get_transport_connection_id ());
    if (completion_pipe_->write_and_flush (&msg)) {
        //  The pipe took ownership of the frame's content.
        const int init_rc = msg.init ();
        errno_assert (init_rc == 0);
    }
    const int close_rc = msg.close ();
    errno_assert (close_rc == 0);
}

void zlink::socket_base_t::sync_local_receive_flow_state_to_pair (
  pipe_t *completion_pipe_)
{
    if (!completion_pipe_)
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
    write_receive_flow_state_frame (completion_pipe_, state, epoch);
}

bool zlink::socket_base_t::consume_receive_flow_state_frame (
  pipe_t *completion_pipe_, const zlink::msg_t &msg_)
{
    flow_state::frame_t frame;
    const flow_state::decode_result_t decoded =
      flow_state::decode_frame (msg_, &frame);
    if (decoded == flow_state::decode_not_flow_frame)
        return false;
    //  An unsupported version or a malformed frame is still ours: it is
    //  consumed here and never handed to another frame handler.
    if (decoded != flow_state::decode_ok || !completion_pipe_)
        return true;

    //  Both lanes of a pair carry the same id and generation, so those two
    //  fields cannot tell them apart. The lane of the receiving pipe is local
    //  truth the peer cannot influence: flow state travels on the completion
    //  lane only, and an application-lane frame is dropped.
    if (completion_pipe_->get_transport_lane () != transport_lane_completion)
        return true;

    const uint64_t pair_id = completion_pipe_->get_transport_pair_id ();
    const uint64_t generation =
      completion_pipe_->get_transport_pair_generation ();
    //  A frame that names another pair, or a generation other than the one
    //  this physical connection carries, is a late frame from a previous
    //  connection and is ignored.
    if (pair_id == 0 || generation == 0 || frame.pair_id != pair_id
        || frame.generation != generation)
        return true;

    const bool paused = frame.state == flow_state::receive_flow_paused;
    pipe_t *application = NULL;
    {
        scoped_lock_t lock (_transport_pairs_sync);
        //  The frame is decoded on the transport I/O thread and can overtake
        //  the socket thread's own pair admission for the very same physical
        //  connection. The record is therefore created here when needed;
        //  attach_pipe applies it as soon as the application lane exists.
        transport_pair_pipes_t &pair =
          _transport_pairs[transport_pair_key_t (pair_id, generation)];
        //  Once the pair knows its completion lane, only that exact pipe may
        //  carry the state.
        if (pair.completion && pair.completion != completion_pipe_)
            return true;
        if (!pair.ready || pair.completion != completion_pipe_) {
            //  The pipe's own lane proves the frame came in on a completion
            //  lane, but not yet that this connection is the one that owns the
            //  pair: on a passive transport the peer supplies the pair
            //  identity in its metadata. Hold the frame, latest-only, and
            //  leave the accepted state and its epoch untouched so a
            //  connection that never wins registration cannot burn either.
            buffer_pending_flow_state_locked (
              pair, completion_pipe_->get_transport_connection_id (), paused,
              frame.epoch);
            return true;
        }
        //  Duplicate or reversed epoch within one generation.
        if (pair.remote_flow_seen && frame.epoch <= pair.remote_flow_epoch)
            return true;
        pair.remote_flow_epoch = frame.epoch;
        pair.remote_flow_seen = true;
        if (pair.remote_flow_paused == paused)
            return true;
        pair.remote_flow_paused = paused;
        //  Published while the mutex is held, so a decision taken under it
        //  either sees this change or is abandoned before it publishes.
        _flow_state_sequence.fetch_add (1, std::memory_order_acq_rel);
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
#endif

#ifdef ZLINK_BUILD_TESTS
uint32_t zlink::socket_base_t::test_transport_write_release_edges () const
{
    return _test_transport_write_release_edges.load (std::memory_order_relaxed);
}
#endif

#ifdef ZLINK_BUILD_TESTS
bool zlink::socket_base_t::test_flow_frame_accepted_before_pair_ready () const
{
    scoped_lock_t lock (_transport_pairs_sync);
    for (transport_pairs_t::const_iterator it = _transport_pairs.begin (),
                                           end = _transport_pairs.end ();
         it != end; ++it) {
        bool pending = false;
        for (size_t i = 0; i < transport_pair_pipes_t::pending_flow_slot_count;
             ++i)
            pending = pending || it->second.pending_flow[i].valid;
        if ((it->second.remote_flow_seen || pending) && !it->second.ready)
            return true;
    }
    return false;
}

bool zlink::socket_base_t::test_pending_flow_buffered (
  bool *paused_out_, uint64_t *epoch_out_, uint64_t *pair_id_out_,
  uint64_t *generation_out_) const
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

bool zlink::socket_base_t::test_any_pair_accepted_flow_state () const
{
    scoped_lock_t lock (_transport_pairs_sync);
    for (transport_pairs_t::const_iterator it = _transport_pairs.begin (),
                                           end = _transport_pairs.end ();
         it != end; ++it) {
        if (it->second.remote_flow_seen)
            return true;
    }
    return false;
}
#endif

#ifdef ZLINK_BUILD_TESTS
namespace
{
zlink::socket_base_t::test_attach_flow_window_fn tls_attach_flow_window_hook =
  NULL;
}

void zlink::socket_base_t::test_set_attach_flow_window_hook (
  test_attach_flow_window_fn hook_)
{
    tls_attach_flow_window_hook = hook_;
}

void zlink::socket_base_t::test_run_attach_flow_window_hook (
  socket_base_t *socket_, uint64_t transport_pair_id_, uint64_t generation_)
{
    test_attach_flow_window_fn hook = tls_attach_flow_window_hook;
    if (hook)
        hook (socket_, transport_pair_id_, generation_);
}
#endif

void zlink::socket_base_t::buffer_pending_flow_state_locked (
  transport_pair_pipes_t &pair_, uint64_t source_connection_id_, bool paused_,
  uint64_t epoch_)
{
    //  Without a connection id the candidate cannot be told apart from any
    //  other, so holding the state would risk applying it on behalf of a
    //  different connection. Every network connection has one by the time it
    //  can deliver a frame.
    if (source_connection_id_ == 0)
        return;

    //  One slot per candidate, latest state wins within a slot. A candidate
    //  never touches another candidate's slot, so a source that goes on to
    //  lose registration cannot destroy the winner's state.
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
    //  Every slot belongs to another candidate. Refuse the newcomer rather
    //  than evict an existing candidate; only one of them can win anyway, and
    //  refusing keeps this bounded without ever damaging a held state.
}

void zlink::socket_base_t::promote_pending_flow_state_locked (
  transport_pair_pipes_t &pair_)
{
    bool found = false;
    bool paused = false;
    uint64_t epoch = 0;
    const uint64_t winner_connection_id =
      pair_.completion ? pair_.completion->get_transport_connection_id () : 0;
    for (size_t i = 0; i < transport_pair_pipes_t::pending_flow_slot_count;
         ++i) {
        transport_pair_pipes_t::pending_flow_slot_t &slot = pair_.pending_flow[i];
        //  Only the candidate that won registration is promoted. Everything
        //  else is dropped outright: it never reaches the accepted state and
        //  never advances the epoch.
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
    const bool changed = pair_.remote_flow_paused != paused;
    pair_.remote_flow_epoch = epoch;
    pair_.remote_flow_seen = true;
    pair_.remote_flow_paused = paused;
    if (changed)
        _flow_state_sequence.fetch_add (1, std::memory_order_acq_rel);
}

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
namespace
{
zlink::socket_base_t::test_attach_flow_window_fn
  tls_attach_publish_window_hook = NULL;
}

void zlink::socket_base_t::test_set_attach_publish_window_hook (
  test_attach_flow_window_fn hook_)
{
    tls_attach_publish_window_hook = hook_;
}

void zlink::socket_base_t::test_run_attach_publish_window_hook (
  socket_base_t *socket_, uint64_t transport_pair_id_, uint64_t generation_)
{
    test_attach_flow_window_fn hook = tls_attach_publish_window_hook;
    if (hook)
        hook (socket_, transport_pair_id_, generation_);
}
#endif

#ifdef ZLINK_BUILD_TESTS
bool zlink::socket_base_t::test_pair_is_ready (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_) const
{
    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pairs_t::const_iterator it = _transport_pairs.find (
      transport_pair_key_t (transport_pair_id_, transport_pair_generation_));
    return it != _transport_pairs.end () && it->second.ready;
}
#endif

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
