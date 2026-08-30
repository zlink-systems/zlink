/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/ctx.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "core/mailbox.hpp"
#include "core/msg.hpp"
#include "core/options.hpp"
#include "utils/err.hpp"

namespace
{
bool same_pair_peer_identity (const zlink::pipe_t *first_, const zlink::pipe_t *second_)
{
    if (!first_ || !second_)
        return false;
    const zlink::blob_t &first = first_->get_transport_peer_identity ();
    const zlink::blob_t &second = second_->get_transport_peer_identity ();
    return first.size () > 0 && first.size () == second.size ()
           && memcmp (first.data (), second.data (), first.size ()) == 0;
}

class distinct_pipe_lifetime_refs_t
{
  public:
    explicit distinct_pipe_lifetime_refs_t (zlink::pipe_t *already_owned_) :
        _already_owned (already_owned_), _count (0)
    {
        memset (_pipes, 0, sizeof (_pipes));
    }

    ~distinct_pipe_lifetime_refs_t ()
    {
        for (size_t i = 0; i < _count; ++i)
            _pipes[i]->release_lifetime_ref ();
    }

    bool retain (zlink::pipe_t *pipe_)
    {
        if (!pipe_ || pipe_ == _already_owned)
            return true;
        for (size_t i = 0; i < _count; ++i) {
            if (_pipes[i] == pipe_)
                return true;
        }
        if (_count == sizeof (_pipes) / sizeof (_pipes[0])
            || !pipe_->retain_lifetime_ref ())
            return false;
        _pipes[_count++] = pipe_;
        return true;
    }

  private:
    zlink::pipe_t *_already_owned;
    zlink::pipe_t *_pipes[4];
    size_t _count;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (distinct_pipe_lifetime_refs_t)
};
}

void zlink::socket_base_t::finish_close_handoff (int handoff_timeout_ms_)
{
    lifecycle_coordinator ().complete_deferred_close_handoff (
      static_cast<mailbox_t *> (_mailbox), this, handoff_timeout_ms_);

    finish_close_reap ();
}

void zlink::socket_base_t::finish_close_reap ()
{
    fail_all_send_pending (_ctx_terminated ? ETERM : ECANCELED);
    dispatch_send_completions (true);
    materialize_pending_inprocs_before_reap ();
    static_cast<mailbox_t *> (_mailbox)->clear_signalers ();

    send_reap (this);
}

void zlink::socket_base_t::materialize_pending_inprocs_before_reap ()
{
    // Async command processing is quiesced before this helper runs, and the
    // socket has not reached the reaper yet. Materialization therefore cannot
    // erase this socket's inproc map while it is traversed. Iterate the
    // existing keys directly so close does not add an allocation-failure path.
    endpoint_runtime ().inprocs.for_each_unique_endpoint (
      [this] (const std::string &endpoint_) {
          (void) get_ctx ()->materialize_pending_inproc (endpoint_, this);
      });
}

int zlink::socket_base_t::get_peer_state (const void *routing_id_, size_t routing_id_size_) const
{
    LIBZLINK_UNUSED (routing_id_);
    LIBZLINK_UNUSED (routing_id_size_);

    errno = ENOTSUP;
    return -1;
}

int zlink::socket_base_t::xterm_peer_rid (const zlink_routing_id_t *peer_rid_)
{
    if (!peer_rid_ || peer_rid_->size == 0) {
        errno = EINVAL;
        return -1;
    }

    std::vector<pipe_t *> pipes;
    snapshot_attached_pipes (&pipes);

    pipe_t *match = NULL;
    for (size_t i = 0; i < pipes.size (); ++i) {
        pipe_t *pipe = pipes[i];
        if (!pipe)
            continue;

        const blob_t &routing_id = pipe->get_routing_id ();
        bool matches = routing_id.size () == peer_rid_->size && routing_id.size () > 0
                       && memcmp (routing_id.data (), peer_rid_->data, peer_rid_->size) == 0;
        pipe_t *const peer = pipe->retain_peer_snapshot ();
        if (!matches && peer) {
            const blob_t &peer_routing_id = peer->get_routing_id ();
            matches = peer_routing_id.size () == peer_rid_->size && peer_routing_id.size () > 0
                      && memcmp (peer_routing_id.data (), peer_rid_->data, peer_rid_->size) == 0;
        }
        if (peer)
            peer->release_lifetime_ref ();
        if (!matches)
            continue;
        if (match && match != pipe) {
            errno = EADDRINUSE;
            return -1;
        }
        match = pipe;
    }

    if (!match) {
        errno = ENOENT;
        return -1;
    }

    fail_send_pending_for_pipe (match, ENOTCONN);
    match->terminate (false);
    return 0;
}

int zlink::socket_base_t::xterm_transport_pair (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_)
{
    std::vector<pipe_t *> pipes;
    snapshot_attached_pipes (&pipes);

    size_t match_count = 0;
    for (size_t i = 0; i < pipes.size (); ++i) {
        pipe_t *pipe = pipes[i];
        if (!pipe || pipe->get_transport_pair_id () != transport_pair_id_
            || pipe->get_transport_pair_generation () != transport_pair_generation_)
            continue;
        const std::string endpoint = pipe->get_endpoint_pair ().identifier ();
        const std::pair<endpoints_t::iterator, endpoints_t::iterator> range =
          endpoint_runtime ().endpoints.equal_range (endpoint);
        for (endpoints_t::iterator it = range.first; it != range.second; ++it) {
            if (it->second.transport_pair_state)
                it->second.transport_pair_state->disable_reconnect ();
        }
        fail_send_pending_for_pipe (pipe, ENOTCONN);
        pipe->terminate (false);
        ++match_count;
    }

    if (match_count == 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

void zlink::socket_base_t::attach_pipe (pipe_t *pipe_,
                                        bool subscribe_to_all_,
                                        bool locally_initiated_,
                                        bool transport_validated_)
{
    pipe_->set_event_sink (this);
    // A bind command retains the pipe until dispatch, but termination can
    // already have made it unusable before that command reaches the socket.
    // Keep such a delayed bind out of both the socket and pair admission.
    if (!pipe_->active_for_reply_target ())
        return;
    const bool already_attached = endpoint_runtime ().attached_pipes.contains (pipe_);
    if (!already_attached) {
        scoped_lock_t lock (monitor_runtime ().sync);
        endpoint_runtime ().attach_pipe (pipe_);
    }

    const uint64_t pair_id = pipe_->get_transport_pair_id ();
    const bool completion =
      pair_id != 0 && pipe_->get_transport_lane () == transport_lane_completion;
    const transport_pair_key_t pair_key (pair_id,
                                         pipe_->get_transport_pair_generation ());
    bool attach_application = pair_id == 0;
    pipe_t *ready_application = NULL;
    pipe_t *ready_completion = NULL;
    pipe_t *pair_application = NULL;
    distinct_pipe_lifetime_refs_t pair_pipe_refs (pipe_);
    //  A rejected pair is torn down after the table is unlocked: terminate()
    //  reaches other objects and must not run under this mutex.
    pipe_t *reject_pipes[3] = {NULL, NULL, NULL};
    if (pair_id != 0) {
        scoped_lock_t lock (_transport_pairs_sync);
        transport_pair_pipes_t &pair = _transport_pairs[pair_key];
        pair.generation = pipe_->get_transport_pair_generation ();
        pipe_t *&lane_pipe = completion ? pair.completion : pair.application;
        if (lane_pipe && lane_pipe != pipe_) {
            reject_pipes[0] = pipe_;
            reject_pipes[1] = pair.application;
            reject_pipes[2] = pair.completion;
        } else {
            lane_pipe = pipe_;
            if (completion)
                pair.completion_validated =
                  pair.completion_validated || transport_validated_;
            else
                pair.application_validated =
                  pair.application_validated || transport_validated_;

            if (pair.application && !pair.application_attached) {
                attach_application = true;
                pair.application_attached = true;
            }

            if (pair.application && pair.completion
                && pair.application_validated && pair.completion_validated
                && !pair.ready) {
                // Either lane may start termination while its sibling's bind
                // is queued. A retained object is still alive, but it is not
                // an admissible transport; never publish a pair assembled
                // from an inactive lane.
                if (!pair.application->active_for_reply_target ()
                    || !pair.completion->active_for_reply_target ()
                    || !same_pair_peer_identity (pair.application,
                                                 pair.completion)) {
                    reject_pipes[0] = pipe_;
                    reject_pipes[1] = pair.application;
                    reject_pipes[2] = pair.completion;
                } else {
                    pair.ready = true;
                    ready_application = pair.application;
                    ready_completion = pair.completion;
                }
            }
            pair_application = pair.application;
        }
        //  reject_pipes[1] and [2] were read out of the table, and the table
        //  slot is the only thing that keeps them alive: the sibling lane's
        //  own pipe_terminated clears its slot under this same mutex and then
        //  finishes process_pipe_term_ack, which deallocates it. Both can run
        //  on a second mailbox executor while this attach is in flight, so pin
        //  them here - where a non-NULL slot still proves liveness - and drop
        //  the pins once terminate() has run below. reject_pipes[0] is the
        //  pipe being attached and is owned by this call, so it needs no pin.
        for (size_t i = 1; i < 3; ++i) {
            if (reject_pipes[i] && !pair_pipe_refs.retain (reject_pipes[i]))
                reject_pipes[i] = NULL;
        }
        if (!reject_pipes[0]) {
            if (pair_application
                && !pair_pipe_refs.retain (pair_application)) {
                pair_application = NULL;
                attach_application = false;
            }
            if (ready_application
                && !pair_pipe_refs.retain (ready_application))
                ready_application = NULL;
            if (ready_completion
                && !pair_pipe_refs.retain (ready_completion))
                ready_completion = NULL;
        }
    }

    if (reject_pipes[0]) {
        for (size_t i = 0; i < 3; ++i) {
            if (reject_pipes[i])
                reject_pipes[i]->terminate (false);
        }
        return;
    }

    if (attach_application) {
        pipe_t *application = pair_id == 0 ? pipe_ : pair_application;
        {
            receive_runtime_t &receive = receive_runtime ();
            scoped_lock_t receive_lock (receive.sync);
            xattach_pipe (
              application, subscribe_to_all_,
              locally_initiated_ || application->is_locally_initiated ());
            notify_receive_progress_locked ();
        }
        if (dispatch_runtime ().send_recovery_pending () && transport_has_out ()) {
            dispatch_runtime ().mark_send_recovery_ready ();
            static_cast<mailbox_t *> (_mailbox)->signal ();
        }
    }
    if (ready_application && ready_completion)
        cache_completion_pipe_routing_id (ready_application);
    //  A pair that has just become ready - including a reconnected one - gets
    //  the socket's latest stored local receive-flow state, so a peer never
    //  keeps sending into a socket that is still PAUSED.
    if (ready_completion)
        sync_local_receive_flow_state_to_pair (ready_completion);
    //  Applying the accepted state and releasing the transport-pair hold is one
    //  step, taken under the table mutex. Releasing the hold is an admission
    //  transition, and a pair whose peer is already PAUSED must never pass
    //  through a writable state on the way.
    //
    //  The state read at admission above can be stale by now: a transport I/O
    //  thread updates this record - under this same mutex - before it queues
    //  its own command, so re-reading it here is what makes the two producers
    //  linearizable. Both calls below only take the pipe's own lock and return
    //  a decision; the edges are published after this mutex is dropped.
    bool publish_flow_edge = false;
    bool transport_write_released = false;
    pipe_t *flow_edge_pipe = NULL;
    flow_state_transition_t flow_transition = flow_state_no_transition;
    bool flow_actual_writable = false;
    uint64_t flow_epoch = 0;
    {
        scoped_lock_t lock (_transport_pairs_sync);
        const transport_pairs_t::iterator it = _transport_pairs.find (pair_key);
        if (pair_id != 0 && it != _transport_pairs.end ()) {
            //  A completion frame can overtake this socket thread's pair
            //  admission. Validation has now selected the registered
            //  completion connection, so promote only that source before the
            //  application lane is released for writes.
            if (it->second.ready)
                promote_pending_flow_state_locked (it->second);
            if (it->second.remote_flow_seen && it->second.application) {
                pipe_t *const candidate = it->second.application;
                if (pair_pipe_refs.retain (candidate)) {
                    flow_edge_pipe = candidate;
                    flow_epoch = it->second.remote_flow_epoch;
                    publish_flow_edge = flow_edge_pipe->apply_remote_flow_state (
                      it->second.remote_flow_paused
                        ? static_cast<unsigned char> (1)
                        : static_cast<unsigned char> (0),
                      flow_epoch, &flow_transition, &flow_actual_writable);
                }
            }
        }
        if (ready_application)
            transport_write_released =
              ready_application->release_writes_for_transport_pair ();
    }
    //  Publishing an edge calls back into the socket and cannot run under the
    //  table mutex, so a state accepted in between can leave this edge
    //  momentarily stale. That transient is accepted by design: a send that
    //  slips through is still bounded by the byte HWM, and the state converges
    //  as soon as the queued command runs. See the worklog's accepted-by-design
    //  transients.
    //  This is the same PAUSED<->RUNNING flip that process_flow_state ()
    //  reports, only reached through pair admission instead of through a
    //  frame. It has to go through the same bookkeeping, or a pause applied
    //  here would raise no event, move no gauge, start no duration, and the
    //  matching RESUMED later would look unmatched. Emitted outside the table
    //  mutex, exactly like the edges below.
    if (flow_transition != flow_state_no_transition)
        flow_state_applied (flow_edge_pipe,
                            flow_transition == flow_state_transition_paused,
                            flow_epoch, flow_actual_writable);
    if (publish_flow_edge)
        write_activated (flow_edge_pipe);
    if (transport_write_released) {
        write_activated (ready_application);
        // A routed async submit can already be parked on transport_wait when
        // the Application/Completion pair becomes Ready. HWM recovery emits
        // its own edge from write_activated(), but releasing the pair hold is
        // a distinct admission transition and must wake that exact target too.
        notify_send_pending_writable (ready_application);
    }
    if (ready_application && socket_type () == ZLINK_CORE_SOCKET_ROUTER) {
        // ROUTER readiness is a public data-plane edge, not merely pair-table
        // admission. Publish it only after the application lane's transport
        // hold has been released and xwrite_activated() has made the route
        // selectable. Otherwise a consumer can observe CONNECTION_READY and
        // immediately receive ECONNREFUSED on the same admitted connection.
        emit_transport_pair_ready (ready_application);
    } else if (ready_application) {
        // A Router may still be waiting for RID adoption after pair
        // validation. Publish here only when the Application pipe already
        // has its peer RID; router_t::adopt_peer_routing_id publishes the
        // complementary case after route registration.
        endpoint_uri_pair_t endpoint_pair =
          ready_application->get_endpoint_pair ();
        endpoint_pair.connection_id =
          ready_application->get_transport_connection_id ();
        const blob_t &routing_id = ready_application->get_routing_id ();
        if (routing_id.size () > 0)
            event_connection_ready_changed (
              endpoint_pair, routing_id.data (), routing_id.size (),
              transport_lane_application, pair_key.first, pair_key.second);
    }
    if (ready_completion) {
        if (ready_completion->check_read ())
            read_activated (ready_completion);
        if (ready_application && ready_application->check_read ()) {
            receive_runtime_t &receive = receive_runtime ();
            scoped_lock_t receive_lock (receive.sync);
            xread_activated (ready_application);
            notify_receive_progress_locked ();
        }
    } else if (completion) {
        bool pair_ready = false;
        {
            scoped_lock_t lock (_transport_pairs_sync);
            const transport_pairs_t::const_iterator it =
              _transport_pairs.find (pair_key);
            pair_ready = it != _transport_pairs.end () && it->second.ready
                         && it->second.completion == pipe_;
        }
        if (pair_ready && pipe_->check_read ()) {
            read_activated (pipe_);
        }
    }
    if (get_ctx ()) {
        const bool recalculate_application_attach =
          attach_application
          && pipe_->get_transport_lane () == transport_lane_application;
        bool auto_hwm_policy_enabled = false;
        {
            scoped_lock_t lock (_auto_hwm_sync);
            auto_hwm_policy_enabled = _auto_hwm_policy_enabled;
        }
        if (recalculate_application_attach && auto_hwm_policy_enabled)
            (void) get_ctx ()->auto_hwm_recalculate_now ();
        else
            get_ctx ()->schedule_auto_hwm_recalculate ();
    }

    if (is_terminating ()) {
        if (_term_pipes.insert (pipe_).second) {
            register_term_acks (1);
            ++_term_pipe_acks_registered;
            pipe_->terminate (false);
        }
    }
}

int zlink::socket_base_t::setsockopt (int option_, const void *optval_, size_t optvallen_)
{
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    socket_public_api_scope_t admission (lifecycle);
    if (!admission.acquired ())
        return -1;

    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    // STREAM NOTIFY owns both option validation and its bind-time boundary in
    // stream_t. Do not reinterpret its deliberate EINVAL as a request to use
    // the generic option fallback, or a post-bind update would slip through.
    if (option_ == ZLINK_INTERNAL_OPT_STREAM_NOTIFY
        && socket_type () == ZLINK_CORE_SOCKET_STREAM) {
        socket_public_api_lock_scope_t guard (lifecycle);
        return xsetsockopt (option_, optval_, optvallen_);
    }

    int rc = 0;
    {
        socket_public_api_lock_scope_t guard (lifecycle);
        rc = xsetsockopt (option_, optval_, optvallen_);
    }
    if (rc == 0 || errno != EINVAL) {
        return rc;
    }

    {
        socket_public_api_lock_scope_t guard (lifecycle);
        scoped_lock_t auto_hwm_lock (_auto_hwm_sync);
        rc = options.setsockopt (option_, optval_, optvallen_);
        if (rc == 0) {
            if (option_ == ZLINK_INTERNAL_OPT_SNDHWM)
                _manual_sndhwm = true;
            else if (option_ == ZLINK_INTERNAL_OPT_RCVHWM)
                _manual_rcvhwm = true;

            if (option_ == ZLINK_INTERNAL_OPT_PEER_WEIGHT) {
                _local_peer_weight = static_cast<uint32_t> (options.peer_weight);
                xlocal_peer_weight_changed ();
            }
        }
        update_pipe_options (option_);
    }
    return rc;
}

int zlink::socket_base_t::getsockopt (int option_, void *optval_, size_t *optvallen_)
{
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    socket_public_api_scope_t admission (lifecycle);
    if (!admission.acquired ())
        return -1;

    //  A poller must keep watching the mailbox after stop() publishes context
    //  termination: process_stop() and an in-flight claimed send can still
    //  publish the final completion there. All user-visible options retain
    //  their ordinary ETERM boundary.
    if (unlikely (_ctx_terminated) && option_ != ZLINK_INTERNAL_OPT_FD) {
        errno = ETERM;
        return -1;
    }

    int rc = 0;
    {
        socket_public_api_lock_scope_t guard (lifecycle);
        rc = xgetsockopt (option_, optval_, optvallen_);
    }
    if (rc == 0 || errno != EINVAL) {
        return rc;
    }

    if (option_ == ZLINK_INTERNAL_OPT_FD) {
        rc =
          do_getsockopt<fd_t> (optval_, optvallen_, static_cast<mailbox_t *> (_mailbox)->get_fd ());
        return rc;
    }

    if (option_ == ZLINK_INTERNAL_OPT_EVENTS) {
        {
            socket_public_api_lock_scope_t guard (lifecycle);
            const int events_rc = process_commands (0, false);
            if (events_rc != 0 && (errno == EINTR || errno == ETERM))
                return -1;

            errno_assert (events_rc == 0);
            return do_getsockopt<int> (optval_, optvallen_, has_out () ? ZLINK_POLLOUT : 0);
        }
    }

    if (option_ == ZLINK_INTERNAL_OPT_LAST_ENDPOINT) {
        socket_public_api_lock_scope_t guard (lifecycle);
        return do_getsockopt (optval_, optvallen_, endpoint_runtime ().last_endpoint_uri ());
    }

    {
        socket_public_api_lock_scope_t guard (lifecycle);
        rc = options.getsockopt (option_, optval_, optvallen_);
    }
    return rc;
}

int zlink::socket_base_t::get_events (int events_, uint32_t *out_)
{
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    socket_public_api_scope_t admission (lifecycle);
    if (!admission.acquired ())
        return -1;

    if (!out_) {
        errno = EINVAL;
        return -1;
    }

    {
        socket_public_api_lock_scope_t guard (lifecycle);
        const int rc = process_commands (0, false);
        if (rc != 0 && (errno == EINTR || errno == ETERM))
            return -1;
        errno_assert (rc == 0);
    }

    //  Process readiness commands before trying pending records, but release
    //  the query sync bit first: physical admission takes that bit itself and
    //  would otherwise wait on this thread's own lock.
    if (events_ & ZLINK_POLLCOMPLETION)
        drive_send_pending ();

    {
        socket_public_api_lock_scope_t guard (lifecycle);
        //  Only a caller that asks for ZLINK_POLLCOMPLETION owns the
        //  completion drain, so only that caller runs reply handlers.
        bool completion_notified = false;
        int drained_completions = 0;
        if (events_ & ZLINK_POLLCOMPLETION) {
            scoped_lock_t owner_lock (_completion_owner_sync);
            const completion_drain_scope_t drain_scope (this);
            completion_notified =
              _request_completion_pending.exchange (false, std::memory_order_acq_rel);
            process_ready_completion_pipes ();
            drained_completions = drain_request_completions ();
            if (drained_completions < 0)
                return -1;
            //  The poller thread owns send completion dispatch for as long as
            //  its POLLCOMPLETION registration exists, so it drains what the
            //  admit pass above resolved.
            drained_completions += drain_send_completions ();
        }

        uint32_t events = 0;
        if ((events_ & ZLINK_POLLCOMPLETION)
            && (completion_notified || drained_completions > 0))
            events |= ZLINK_POLLCOMPLETION;
        if ((events_ & ZLINK_POLLOUT) && has_out ())
            events |= ZLINK_POLLOUT;

        *out_ = events;
    }
    return 0;
}

int zlink::socket_base_t::get_events_internal (int events_, uint32_t *out_)
{
    if (!out_) {
        errno = EINVAL;
        return -1;
    }

    if (lifecycle_coordinator ().public_close_requested ()) {
        *out_ = ZLINK_POLLERR;
        return 0;
    }

    const int rc = process_commands (0, false);
    if (unlikely (rc != 0)) {
        if (errno == EINTR)
            return -1;
        if (errno == ETERM) {
            const int terminal_errno = errno;
            if ((events_ & ZLINK_POLLCOMPLETION) == 0)
                return -1;

            // stop() publishes context termination before its command reaches
            // process_stop(). Resolve unclaimed sends here as well, closing
            // that publication window without driving physical admission.
            // Claimed sends receive the same deferred-terminal handoff used by
            // process_stop() and will wake the still-registered mailbox fd
            // when their attempt finishes.
            fail_all_send_pending (ETERM);

            // A POLLCOMPLETION registration remains the sole dispatch owner
            // during context shutdown, so drain already-resolved request and
            // send records under the same owner gate as the normal path.
            zlink_assert (
              _completion_poller_refs.load (std::memory_order_acquire) != 0);
            int drained_completions = 0;
            {
                scoped_lock_t owner_lock (_completion_owner_sync);
                const completion_drain_scope_t drain_scope (this);
                _request_completion_pending.exchange (
                  false, std::memory_order_acq_rel);
                drained_completions = drain_request_completions ();
                if (drained_completions < 0)
                    return -1;
                drained_completions += drain_send_completions ();
            }
            if (drained_completions > 0) {
                *out_ = ZLINK_POLLCOMPLETION;
                return 0;
            }

            // A claimed record can still be completing its deferred ETERM
            // handoff. Keep the poller alive on the mailbox descriptor rather
            // than reporting a stale completion notification or surfacing
            // ETERM before the callback exists.
            if (has_send_pending ()) {
                *out_ = 0;
                errno = terminal_errno;
                return 0;
            }

            errno = terminal_errno;
            return -1;
        }
    }
    errno_assert (rc == 0);

    bool completion_notified = false;
    int drained_completions = 0;
    if (events_ & ZLINK_POLLCOMPLETION) {
        zlink_assert (_completion_poller_refs.load (std::memory_order_acquire)
                      != 0);
        drive_send_pending ();
        scoped_lock_t owner_lock (_completion_owner_sync);
        const completion_drain_scope_t drain_scope (this);
        completion_notified =
          _request_completion_pending.exchange (false, std::memory_order_acq_rel);
        process_ready_completion_pipes ();
        drained_completions = drain_request_completions ();
        if (drained_completions < 0)
            return -1;
        drained_completions += drain_send_completions ();
    }

    uint32_t events = 0;
    if ((events_ & ZLINK_POLLCOMPLETION)
        && (completion_notified || drained_completions > 0))
        events |= ZLINK_POLLCOMPLETION;
    if ((events_ & ZLINK_POLLIN) && has_in ())
        events |= ZLINK_POLLIN;
    if ((events_ & ZLINK_POLLOUT) && has_out ())
        events |= ZLINK_POLLOUT;

    *out_ = events;
    return 0;
}

int zlink::socket_base_t::get_events_for_poller (int events_, uint32_t *out_)
{
    socket_public_api_scope_t admission (lifecycle_coordinator ());
    if (!admission.acquired ()) {
        if (out_)
            *out_ = ZLINK_POLLERR;
        return 0;
    }
    return get_events_internal (events_, out_);
}

int zlink::socket_base_t::drain_request_completions ()
{
    int drained = 0;
    std::shared_ptr<socket_reqrep_internal::socket_request_reply_state_t> socket_state =
      request_reply_state ();
    if (socket_state) {
        const int rc =
          socket_reqrep_internal::drain_reply_completions (socket_state, this);
        if (rc < 0)
            return -1;
        drained += rc;
    }

    return drained;
}

void zlink::socket_base_t::acknowledge_request_completion_notification ()
{
    _request_completion_pending.exchange (false, std::memory_order_acq_rel);
}

void zlink::socket_base_t::resume_completion_processing_if_needed ()
{
    if (_completion_poller_refs.load (std::memory_order_acquire) != 0)
        return;

    if (lifecycle_coordinator ().is_async_mailbox_active ()) {
        // The existing owner may already have consumed the earlier coalesced
        // completion wake while a public poller was active. Queue a real
        // command so a concurrent handler/reschedule boundary cannot lose the
        // 1 -> 0 ownership-transfer wake.
        command_t wake;
        memset (&wake, 0, sizeof (wake));
        wake.destination = this;
        wake.type = command_t::request_completion;
        static_cast<mailbox_t *> (_mailbox)->send (wake);
        return;
    }

    std::shared_ptr<socket_reqrep_internal::socket_request_reply_state_t> socket_state =
      request_reply_state ();
    if (socket_state && socket_reqrep_internal::has_pending_request_work (socket_state)) {
        (void) ensure_completion_processing ();
    }
}

int zlink::socket_base_t::join (const char *group_)
{
    return xjoin (group_);
}

int zlink::socket_base_t::leave (const char *group_)
{
    return xleave (group_);
}

std::recursive_mutex *zlink::socket_base_t::api_sync_mutex ()
{
    return NULL;
}

int zlink::socket_base_t::socket_type () const
{
    return options.type;
}

bool zlink::socket_base_t::has_in ()
{
    scoped_lock_t lock (receive_runtime ().sync);
    return xhas_in ();
}

bool zlink::socket_base_t::has_out ()
{
    const zlink::socket_dispatch_bridge_t &dispatch = dispatch_runtime ();
    if (!dispatch.send_recovery_pending ())
        return false;
    return dispatch.send_recovery_ready ();
}

bool zlink::socket_base_t::transport_has_out ()
{
    return xhas_out ();
}

namespace
{
//  Owner of completion processing on this thread, or NULL. A registered reply
//  handler runs only while its own socket owns the thread, so user code never
//  runs re-entrantly inside an unrelated send, recv or option call.
thread_local const zlink::socket_base_t *tls_completion_drain_owner = NULL;
}

zlink::completion_drain_scope_t::completion_drain_scope_t (const socket_base_t *socket_) :
    _previous (tls_completion_drain_owner)
{
    tls_completion_drain_owner = socket_;
}

zlink::completion_drain_scope_t::~completion_drain_scope_t ()
{
    tls_completion_drain_owner = _previous;
}

bool zlink::socket_base_t::completion_drain_permitted () const
{
    return tls_completion_drain_owner == this;
}

void zlink::socket_base_t::process_ready_completion_pipes ()
{
    //  Drain one claimed pipe at a time with the table unlocked: draining runs
    //  the application's reply handler, which may call back into this socket.
    //  The iterative form also avoids an allocation after a pipe was pinned.
    while (true) {
        transport_pair_key_t key (0, 0);
        pipe_t *completion = NULL;
        {
            scoped_lock_t lock (_transport_pairs_sync);
            while (!_ready_completion_pairs.empty () && !completion) {
                key = _ready_completion_pairs.front ();
                _ready_completion_pairs.pop_front ();
                _ready_completion_pair_set.erase (key);
                transport_pairs_t::iterator it = _transport_pairs.find (key);
                if (it == _transport_pairs.end () || !it->second.ready
                    || !it->second.completion || it->second.draining)
                    continue;
                // Retain while the table slot still proves liveness. The slot
                // can be cleared and the pipe deallocated on a second mailbox
                // executor as soon as this mutex is released.
                if (!it->second.completion->retain_lifetime_ref ())
                    continue;
                if (!it->second.completion->retain_inbound_read_ref ()) {
                    it->second.completion->release_lifetime_ref ();
                    continue;
                }
                it->second.draining = true;
                completion = it->second.completion;
            }
        }
        if (!completion)
            return;
        drain_claimed_completion_pipe (key.first, key.second, completion);
        completion->release_inbound_read_ref ();
        completion->release_lifetime_ref ();
    }
}

bool zlink::socket_base_t::finish_completion_pipe_drain (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
  pipe_t *completion_pipe_)
{
    if (!completion_pipe_)
        return false;

    const transport_pair_key_t pair_key (transport_pair_id_,
                                         transport_pair_generation_);
    bool exact_pair = false;
    {
        scoped_lock_t lock (_transport_pairs_sync);
        transport_pairs_t::iterator it = _transport_pairs.find (pair_key);
        if (it != _transport_pairs.end () && it->second.ready
            && it->second.completion == completion_pipe_
            && it->second.draining) {
            //  Publish the idle state before checking the pipe. An activation
            //  after this point can claim or queue the pair itself; an
            //  activation that arrived while draining was true is recovered
            //  by the check_read below.
            it->second.draining = false;
            exact_pair = true;
        }
    }
    if (!exact_pair || !completion_pipe_->check_read ())
        return false;

    //  Never call pipe methods while holding the table mutex. Revalidate the
    //  slot after check_read and claim it only if another completion owner did
    //  not get there first.
    scoped_lock_t lock (_transport_pairs_sync);
    transport_pairs_t::iterator it = _transport_pairs.find (pair_key);
    if (it == _transport_pairs.end () || !it->second.ready
        || it->second.completion != completion_pipe_
        || it->second.draining)
        return false;
    it->second.draining = true;
    return true;
}

void zlink::socket_base_t::drain_claimed_completion_pipe (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
  pipe_t *completion_pipe_)
{
    do {
        socket_reqrep_internal::process_completion_pipe (this,
                                                         completion_pipe_);
    } while (finish_completion_pipe_drain (
      transport_pair_id_, transport_pair_generation_, completion_pipe_));
}

void zlink::socket_base_t::read_activated (pipe_t *pipe_)
{
    if (pipe_ && pipe_->get_transport_pair_id () != 0
        && pipe_->get_transport_lane () == transport_lane_completion) {
        const bool can_drain = completion_drain_permitted ();
        bool claimed = false;
        bool queued = false;
        pipe_t *retained_completion = NULL;
        const transport_pair_key_t pair_key (pipe_->get_transport_pair_id (),
                                             pipe_->get_transport_pair_generation ());
        {
            scoped_lock_t lock (_transport_pairs_sync);
            transport_pairs_t::iterator it = _transport_pairs.find (pair_key);
            if (it != _transport_pairs.end () && it->second.ready
                && it->second.completion == pipe_
                && !it->second.draining) {
                claimed = true;
                if (can_drain) {
                    const bool lifetime_retained =
                      pipe_->retain_lifetime_ref ();
                    if (lifetime_retained
                        && pipe_->retain_inbound_read_ref ()) {
                        retained_completion = pipe_;
                        it->second.draining = true;
                    } else {
                        if (lifetime_retained)
                            pipe_->release_lifetime_ref ();
                        claimed = false;
                    }
                } else if (_ready_completion_pair_set.insert (pair_key).second) {
                    _ready_completion_pairs.push_back (pair_key);
                    queued = true;
                }
            }
        }
        if (!claimed)
            return;
        if (!can_drain) {
            //  No owner is draining on this thread. Record the readiness so the
            //  completion owner wakes up and drains the pipe itself.
            if (queued)
                notify_request_completion ();
            return;
        }
        drain_claimed_completion_pipe (pair_key.first, pair_key.second,
                                       retained_completion);
        retained_completion->release_inbound_read_ref ();
        retained_completion->release_lifetime_ref ();
        return;
    }
    if (pipe_ && pipe_->get_transport_pair_id () != 0) {
        scoped_lock_t lock (_transport_pairs_sync);
        transport_pairs_t::const_iterator it =
          _transport_pairs.find (transport_pair_key_t (
            pipe_->get_transport_pair_id (),
            pipe_->get_transport_pair_generation ()));
        if (it == _transport_pairs.end () || !it->second.ready)
            return;
    }
    receive_runtime_t &receive = receive_runtime ();
    scoped_lock_t receive_lock (receive.sync);
    xread_activated (pipe_);
    notify_receive_progress_locked ();
}

void zlink::socket_base_t::write_activated (pipe_t *pipe_)
{
    const bool completion =
      pipe_ && pipe_->get_transport_pair_id () != 0
      && pipe_->get_transport_lane () == transport_lane_completion;
    if (!completion) {
        xwrite_activated (pipe_);
        //  Byte credit and a remote RESUME are independent causes; either one
        //  can be the last cause to clear, and each arms its own wake marker.
        bool credit_recovery = false;
        bool flow_recovery = false;
        if (pipe_) {
            credit_recovery = pipe_->take_hwm_credit_recovery ();
            flow_recovery = pipe_->take_flow_resume_recovery ();
        }
        if (credit_recovery || flow_recovery)
            notify_send_pending_writable (pipe_);
    }
    if (dispatch_runtime ().send_recovery_pending ()
        && !dispatch_runtime ().send_recovery_ready ()) {
        dispatch_runtime ().mark_send_recovery_ready ();
        static_cast<mailbox_t *> (_mailbox)->signal ();
    }
}

void zlink::socket_base_t::hiccuped (pipe_t *pipe_)
{
    if (options.immediate == 1)
        pipe_->terminate (false);
    else
        xhiccuped (pipe_);
}

void zlink::socket_base_t::pipe_peer_terminated (pipe_t *pipe_)
{
    LIBZLINK_UNUSED (pipe_);
}

void zlink::socket_base_t::pipe_terminated (pipe_t *pipe_)
{
    const bool term_pipe_ack_expected = is_terminating () && _term_pipes.erase (pipe_) != 0;

    endpoint_uri_pair_t endpoint_pair;
    std::vector<unsigned char> routing_id_copy;
    if (pipe_) {
        endpoint_pair = pipe_->get_endpoint_pair ();
        const blob_t &routing_id = pipe_->get_routing_id ();
        if (routing_id.size () > 0)
            routing_id_copy.assign (routing_id.data (), routing_id.data () + routing_id.size ());
    }
    const unsigned char *routing_id_data = routing_id_copy.empty () ? NULL : &routing_id_copy[0];
    const size_t routing_id_size = routing_id_copy.size ();

    pipe_t *paired_pipe = NULL;
    const uint64_t pair_id = pipe_ ? pipe_->get_transport_pair_id () : 0;
    const uint64_t pair_generation =
      pipe_ ? pipe_->get_transport_pair_generation () : 0;
    const bool completion =
      pipe_ && pair_id != 0 && pipe_->get_transport_lane () == transport_lane_completion;
    bool application_attached = pair_id == 0;
    bool release_paused_pair_accounting = false;
    if (pair_id != 0) {
        scoped_lock_t lock (_transport_pairs_sync);
        transport_pairs_t::iterator pair_it =
          _transport_pairs.find (transport_pair_key_t (
            pair_id, pipe_->get_transport_pair_generation ()));
        if (pair_it != _transport_pairs.end ()) {
            application_attached = pair_it->second.application_attached;
            paired_pipe = completion ? pair_it->second.application : pair_it->second.completion;
            //  The sibling lane runs its own termination, and both lanes of a
            //  pair can be acknowledged on different mailbox executors at the
            //  same time (an application thread inside process_commands and
            //  the async mailbox worker). Once this map slot is cleared the
            //  sibling owes nothing to us any more and may finish
            //  process_pipe_term_ack - which deallocates it - before we reach
            //  the terminate() call below. Pin its lifetime here, while the
            //  map still guarantees it is alive, and drop the pin afterwards.
            if (paired_pipe && !paired_pipe->retain_lifetime_ref ())
                paired_pipe = NULL;
            if (completion)
                pair_it->second.completion = NULL;
            else
                pair_it->second.application = NULL;
            //  A candidate's unvalidated state cannot outlive the connection
            //  that supplied it. Once the completion lane is absent nothing
            //  in this pair can validate any remaining candidate.
            discard_pending_flow_state_locked (
              pair_it->second, pipe_->get_transport_connection_id (),
              pair_it->second.completion == NULL);
            //  A pair that is torn down while paused never sees a RESUMED, so
            //  its +1 on the gauge would never be matched. Release it here,
            //  once, on the lane that owns the accepted state - and only if
            //  the pause was actually accounted, never merely received.
            if (!completion && pair_it->second.remote_flow_pause_accounted) {
                pair_it->second.remote_flow_pause_accounted = false;
                release_paused_pair_accounting = true;
            }
            if (!pair_it->second.application && !pair_it->second.completion)
                _transport_pairs.erase (pair_it);
        }
    }

    if (release_paused_pair_accounting)
        flow_pause_released_on_termination (pipe_);

    if (!completion && application_attached) {
        fail_send_pending_for_pipe (pipe_, ENOTCONN);
        receive_runtime_t &receive = receive_runtime ();
        {
            scoped_lock_t receive_lock (receive.sync);
            xpipe_terminated (pipe_);
            notify_receive_progress_locked ();
        }
    }
    if (!completion
        && (options.type == ZLINK_CORE_SOCKET_PAIR
            || options.type == ZLINK_CORE_SOCKET_DEALER
            || options.type == ZLINK_CORE_SOCKET_ROUTER
            || options.type == ZLINK_CORE_SOCKET_SUB
            || options.type == ZLINK_CORE_SOCKET_XSUB)) {
        // process_commands() owns an outer receive scope around this callback.
        // Retain the pipe and defer assembly/routing teardown until that scope
        // is fully gone; direct I/O sees the non-active tombstone immediately.
        // STREAM and the remaining socket families keep their teardown wholly
        // in xpipe_terminated() and must not retain an unused queue node.
        defer_socket_msg_pipe_termination (pipe_);
    }
    if (!completion) {
        socket_reqrep_internal::forget_dealer_reply_targets_for_pipe (
          request_reply_state (), pipe_);
        socket_reqrep_internal::forget_router_reply_targets_for_pipe (
          request_reply_state (), pipe_);
    }
    endpoint_runtime ().inprocs.erase_pipe (pipe_);

    uint32_t ready_count = 0;
    bool ready_changed = false;
    bool ready_changed_by_endpoint = false;
    {
        scoped_lock_t lock (monitor_runtime ().sync);
        endpoint_runtime ().detach_pipe (pipe_);
        ready_changed = monitor_runtime ().erase_ready_connection (
          endpoint_pair, routing_id_data, routing_id_size, &ready_count,
          pair_id, pair_generation);
        if (!ready_changed)
            ready_changed_by_endpoint = monitor_runtime ().erase_ready_connection_for_endpoint (
              endpoint_pair, &ready_count, pair_id, pair_generation);
        ready_changed = ready_changed || ready_changed_by_endpoint;
    }
    if (ready_changed) {
        uint64_t values[1] = {ready_count};
        event (endpoint_pair, routing_id_data, routing_id_size, values, 1,
               ZLINK_EVENT_CONNECTION_READY, 0,
               pipe_ && pair_id != 0 ? pipe_->get_transport_lane ()
                                     : transport_lane_application,
               pair_id, pair_generation);
    }

    const std::string &identifier = pipe_->get_endpoint_pair ().identifier ();
    if (!identifier.empty ()) {
        std::pair<endpoints_t::iterator, endpoints_t::iterator> range;
        range = endpoint_runtime ().endpoints.equal_range (identifier);

        for (endpoints_t::iterator it = range.first; it != range.second; ++it) {
            if (it->second.pipe == pipe_) {
                it->second.pipe = NULL;
                break;
            }
        }
    }

    if (term_pipe_ack_expected) {
        ++_term_pipe_acks_received;
        unregister_term_ack ();
    }

    if (get_ctx ())
        get_ctx ()->schedule_auto_hwm_recalculate ();

    if (pair_id != 0 && !is_terminating ()) {
        socket_reqrep_internal::fail_disconnected_peer_requests (
          request_reply_state (), pair_id, pair_generation,
          routing_id_data, routing_id_size, ENOTCONN);
    }
    if (paired_pipe) {
        if (!is_terminating ())
            paired_pipe->terminate (false);
        //  Releases the pin taken above; this is the last reference when the
        //  sibling already completed its own termination, so the deallocation
        //  happens here instead of racing us.
        paired_pipe->release_lifetime_ref ();
    }
}

zlink::pipe_t *
zlink::socket_base_t::completion_pipe_for_application (pipe_t *application_pipe_) const
{
    if (!application_pipe_)
        return NULL;
    const transport_pair_key_t pair_key (
      application_pipe_->get_transport_pair_id (),
      application_pipe_->get_transport_pair_generation ());
    scoped_lock_t lock (_transport_pairs_sync);
    transport_pairs_t::const_iterator it = _transport_pairs.find (pair_key);
    if (it == _transport_pairs.end () || !it->second.ready)
        return NULL;
    //  The caller dereferences the result after this mutex is dropped, and
    //  the table slot is what keeps the pipe alive: the lane's own
    //  pipe_terminated clears the slot under this mutex and only afterwards
    //  finishes process_pipe_term_ack, which deallocates it. Pin the pipe
    //  while the slot still proves liveness; the caller releases the pin.
    pipe_t *completion = it->second.completion;
    return completion && completion->retain_lifetime_ref () ? completion : NULL;
}

zlink::pipe_t *
zlink::socket_base_t::application_pipe_for_completion (pipe_t *completion_pipe_) const
{
    if (!completion_pipe_)
        return NULL;
    const transport_pair_key_t pair_key (
      completion_pipe_->get_transport_pair_id (),
      completion_pipe_->get_transport_pair_generation ());
    scoped_lock_t lock (_transport_pairs_sync);
    transport_pairs_t::const_iterator it = _transport_pairs.find (pair_key);
    if (it == _transport_pairs.end () || !it->second.ready
        || it->second.completion != completion_pipe_)
        return NULL;
    //  Pinned for the same reason as completion_pipe_for_application; the
    //  caller releases the pin after its last dereference.
    pipe_t *application = it->second.application;
    return application && application->retain_lifetime_ref () ? application
                                                              : NULL;
}

zlink::pipe_t *
zlink::socket_base_t::completion_pipe_for_peer (const zlink_routing_id_t *peer_rid_) const
{
    if (!peer_rid_ || peer_rid_->size == 0)
        return NULL;
    pipe_t *selected = NULL;
    uint64_t selected_generation = 0;
    scoped_lock_t lock (_transport_pairs_sync);
    for (transport_pairs_t::const_iterator it = _transport_pairs.begin (),
                                           end = _transport_pairs.end ();
         it != end; ++it) {
        pipe_t *application = it->second.application;
        if (!application || !it->second.completion || !it->second.ready)
            continue;
        const blob_t &rid = application->get_routing_id ().size () > 0
                              ? application->get_routing_id ()
                              : it->second.completion->get_routing_id ();
        if (rid.size () == peer_rid_->size
            && memcmp (rid.data (), peer_rid_->data, rid.size ()) == 0
            && (!selected || it->first.second > selected_generation)) {
            selected = it->second.completion;
            selected_generation = it->first.second;
        }
    }
    //  Pinned for the same reason as completion_pipe_for_application; the
    //  caller releases the pin after its last dereference.
    return selected && selected->retain_lifetime_ref () ? selected : NULL;
}

zlink::pipe_t *zlink::socket_base_t::completion_pipe_for_transport_pair (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_) const
{
    if (transport_pair_id_ == 0 || transport_pair_generation_ == 0)
        return NULL;
    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pair_key_t key (transport_pair_id_, transport_pair_generation_);
    transport_pairs_t::const_iterator it = _transport_pairs.find (key);
    return it == _transport_pairs.end () || !it->second.ready
             ? NULL
             : it->second.completion;
}

zlink::pipe_t *zlink::socket_base_t::retain_completion_pipe_for_transport_pair (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_) const
{
    if (transport_pair_id_ == 0 || transport_pair_generation_ == 0)
        return NULL;
    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pair_key_t key (transport_pair_id_,
                                    transport_pair_generation_);
    transport_pairs_t::const_iterator it = _transport_pairs.find (key);
    if (it == _transport_pairs.end () || !it->second.ready)
        return NULL;
    pipe_t *const completion = it->second.completion;
    return completion && completion->retain_lifetime_ref () ? completion
                                                             : NULL;
}

void zlink::socket_base_t::cache_completion_pipe_routing_id (
  pipe_t *application_pipe_)
{
    if (!application_pipe_
        || application_pipe_->get_transport_lane ()
             != transport_lane_application)
        return;

    const blob_t &routing_id = application_pipe_->get_routing_id ();
    if (routing_id.size () == 0)
        return;

    const transport_pair_key_t key (
      application_pipe_->get_transport_pair_id (),
      application_pipe_->get_transport_pair_generation ());
    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pairs_t::const_iterator it = _transport_pairs.find (key);
    if (it == _transport_pairs.end () || !it->second.ready
        || it->second.application != application_pipe_
        || !it->second.completion)
        return;
    if (it->second.completion->get_routing_id ().size () == 0)
        it->second.completion->set_router_socket_routing_id (routing_id);
}

bool zlink::socket_base_t::transport_pair_application_ready (
  const pipe_t *pipe_) const
{
    if (!pipe_ || pipe_->get_transport_lane () != transport_lane_application
        || pipe_->get_transport_pair_id () == 0
        || pipe_->get_transport_pair_generation () == 0)
        return false;

    const transport_pair_key_t key (pipe_->get_transport_pair_id (),
                                    pipe_->get_transport_pair_generation ());
    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pairs_t::const_iterator it = _transport_pairs.find (key);
    return it != _transport_pairs.end () && it->second.ready
           && it->second.application == pipe_;
}

int zlink::socket_base_t::socket_id () const
{
    return options.socket_id;
}

bool zlink::socket_base_t::is_ctx_terminated () const
{
    return _ctx_terminated.load (std::memory_order_acquire);
}

void zlink::socket_base_t::notify_transport_pair_ready (pipe_t *pipe_)
{
    (void) emit_transport_pair_ready (pipe_);
}
