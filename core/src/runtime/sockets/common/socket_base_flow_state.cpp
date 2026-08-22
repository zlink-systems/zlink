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
        ++_local_receive_flow_epoch;
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
        //  Duplicate or reversed epoch within one generation.
        if (pair.remote_flow_seen && frame.epoch <= pair.remote_flow_epoch)
            return true;
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
                                : static_cast<unsigned char> (0));
        application->release_lifetime_ref ();
    }
    return true;
}
