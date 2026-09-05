/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/ctx.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "core/mailbox.hpp"
#include "core/msg.hpp"
#include "core/options.hpp"
#include "utils/err.hpp"
#include "utils/random.hpp"

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

int zlink::socket_base_t::adopt_accepted_transport_pair (
  const unsigned char *peer_routing_id_, size_t peer_routing_id_size_,
  unsigned char lane_count_, uint64_t *pair_id_out_,
  uint64_t *generation_out_)
{
    if ((!peer_routing_id_ && peer_routing_id_size_ != 0)
        || (lane_count_ != 1u && lane_count_ != 2u) || !pair_id_out_
        || !generation_out_) {
        errno = EINVAL;
        return -1;
    }

    const std::string peer_key =
      peer_routing_id_size_ == 0
        ? std::string ()
        : std::string (reinterpret_cast<const char *> (peer_routing_id_),
                       peer_routing_id_size_);
    scoped_lock_t lock (_transport_pairs_sync);
    const accepted_transport_pairs_t::iterator existing =
      _accepted_transport_pairs.find (peer_key);
    if (existing != _accepted_transport_pairs.end () && lane_count_ == 2u) {
        *pair_id_out_ = existing->second.pair_id;
        *generation_out_ = existing->second.generation;
        return 0;
    }

    // A count-one pair is a complete physical connection by itself. Reusing
    // the previous connection's accepted pair ID makes pair-table admission
    // treat a same-RID handover as a duplicate Application lane and terminate
    // both pipes before ROUTER duplicate policy can run. Replace the registry
    // entry with a fresh ID; release from the older pipe is ID-qualified and
    // therefore cannot erase the new owner. Count-two transports still reuse
    // the entry so their Application and Completion lanes converge.
    if (existing != _accepted_transport_pairs.end ())
        _accepted_transport_pairs.erase (existing);

    uint64_t pair_id = 0;
    bool collision = false;
    do {
        generate_random_bytes (
          reinterpret_cast<unsigned char *> (&pair_id), sizeof (pair_id));
        collision = pair_id == 0;
        if (!collision) {
            for (transport_pairs_t::const_iterator it = _transport_pairs.begin (),
                                                   end = _transport_pairs.end ();
                 it != end; ++it) {
                if (it->first.first == pair_id) {
                    collision = true;
                    break;
                }
            }
        }
        if (!collision) {
            for (accepted_transport_pairs_t::const_iterator
                   it = _accepted_transport_pairs.begin (),
                   end = _accepted_transport_pairs.end ();
                 it != end; ++it) {
                if (it->second.pair_id == pair_id) {
                    collision = true;
                    break;
                }
            }
        }
    } while (collision);

    const uint64_t generation = 1;
    _accepted_transport_pairs.ZLINK_MAP_INSERT_OR_EMPLACE (
      peer_key, accepted_transport_pair_identity_t (pair_id, generation));
    *pair_id_out_ = pair_id;
    *generation_out_ = generation;
    return 0;
}

void zlink::socket_base_t::release_accepted_transport_pair (
  const unsigned char *peer_routing_id_, size_t peer_routing_id_size_,
  uint64_t pair_id_, uint64_t generation_)
{
    if ((!peer_routing_id_ && peer_routing_id_size_ != 0)
        || pair_id_ == 0 || generation_ == 0)
        return;

    const std::string peer_key =
      peer_routing_id_size_ == 0
        ? std::string ()
        : std::string (reinterpret_cast<const char *> (peer_routing_id_),
                       peer_routing_id_size_);
    scoped_lock_t lock (_transport_pairs_sync);
    const accepted_transport_pairs_t::iterator it =
      _accepted_transport_pairs.find (peer_key);
    if (it != _accepted_transport_pairs.end ()
        && it->second.pair_id == pair_id_
        && it->second.generation == generation_)
        _accepted_transport_pairs.erase (it);
}

void zlink::socket_base_t::finish_close_handoff (int handoff_timeout_ms_)
{
    lifecycle_coordinator ().complete_deferred_close_handoff (
      static_cast<mailbox_t *> (_mailbox), this, handoff_timeout_ms_);

    finish_close_reap ();
}

void zlink::socket_base_t::finish_close_reap ()
{
    socket_completion::close (&completion_runtime (),
                              _ctx_terminated ? ETERM : ESHUTDOWN);
    fail_all_blocking_send_waits (_ctx_terminated ? ETERM : ECANCELED);
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

    fail_blocking_send_waits_for_logical_target (peer_rid_, ENOENT);
    match->terminate (false);
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
    // Publish endpoint ownership under the same lock used by detach, and
    // recheck the pipe state there: if termination won before this point its
    // callback has either already removed the pipe or will observe this
    // insertion after the state transition.
    {
        scoped_lock_t lock (monitor_runtime ().sync);
        if (!pipe_->is_lifecycle_active ())
            return;
        const bool already_attached =
          endpoint_runtime ().attached_pipes.contains (pipe_);
        if (!already_attached)
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
    bool reject_attached_application = false;
    if (pair_id != 0) {
        const unsigned char lane_count = pipe_->get_transport_lane_count ();
        // An active connector and a connect-before-bind inproc endpoint both
        // create only their Application intent before the peer type is known.
        // Keep that unvalidated pipe endpoint-owned, but do not create a pair
        // record or expose it to FQ/LB. The later validated bind re-enters with
        // the negotiated count. No Completion pipe, validated pipe, ready bit,
        // or write-hold release may use this staging exception.
        if (lane_count == 0u && !transport_validated_ && !completion) {
            // The staged Application pipe is nevertheless a real physical
            // queue and owns the same atomic Auto-HWM minimum reservation as a
            // bind-first pipe. Publish that topology synchronously when the
            // policy is enabled so an immediate budget snapshot observes it.
            if (get_ctx ()) {
                bool auto_hwm_policy_enabled = false;
                {
                    scoped_lock_t lock (_auto_hwm_sync);
                    auto_hwm_policy_enabled = _auto_hwm_policy_enabled;
                }
                if (auto_hwm_policy_enabled)
                    (void) get_ctx ()->auto_hwm_recalculate_now ();
                else
                    get_ctx ()->schedule_auto_hwm_recalculate ();
            }
            return;
        }

        scoped_lock_t lock (_transport_pairs_sync);
        // A paired bind and its termination acknowledgement may execute on
        // different mailbox owners. Never insert a pipe whose termination
        // callback already passed the pair table; otherwise the raw pointer
        // would have no later callback that can erase it.
        if (!pipe_->is_lifecycle_active ())
            return;
        transport_pair_pipes_t &pair = _transport_pairs[pair_key];
        pair.generation = pipe_->get_transport_pair_generation ();
        // Every real paired pipe carries the READY-negotiated topology before
        // socket admission. Missing or conflicting internal state is a hard
        // contract failure; there is no count-2 inference or compatibility
        // fallback at this boundary.
        const bool invalid_lane_count = !pair.accepts_lane_count (
          lane_count, pipe_->get_transport_lane ());
        if (!invalid_lane_count && pair.expected_lane_count == 0u)
            pair.expected_lane_count = lane_count;
        pipe_t *&lane_pipe = completion ? pair.completion : pair.application;
        if (invalid_lane_count || (lane_pipe && lane_pipe != pipe_)) {
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

            // DEALER records a handshake-known route before its Completion
            // sibling arrives so a FINAL request can pin that configured
            // endpoint. The pipe's pair write hold still prevents admission.
            // ROUTER has no analogous configured-target selection and keeps
            // incomplete lanes out of its FQ entirely.
            if (options.type == ZLINK_CORE_SOCKET_DEALER
                && pair.application && !pair.application_attached) {
                attach_application = true;
                pair.application_attached = true;
            }

            const unsigned int expected_mask = pair.expected_lane_mask ();
            if (expected_mask != 0u
                && (pair.validated_lane_mask () & expected_mask)
                     == expected_mask
                && !pair.ready) {
                // Either lane may start termination while its sibling's bind
                // is queued. A retained object is still alive, but it is not
                // an admissible transport; never publish a pair assembled
                // from an inactive lane.
                if (!pair.application->is_lifecycle_active ()
                    || (pair.expected_lane_count == 2u
                        && (!pair.completion->is_lifecycle_active ()
                            || !same_pair_peer_identity (pair.application,
                                                        pair.completion)))) {
                    reject_pipes[0] = pipe_;
                    reject_pipes[1] = pair.application;
                    reject_pipes[2] = pair.completion;
                } else {
                    pair.ready = true;
                    pair.application->set_transport_pair_completion_pipe (
                      pair.expected_lane_count == 2u ? pair.completion : NULL);
                    pair.application->set_transport_pair_application_ready (
                      true);
                    ready_application = pair.application;
                    ready_completion = pair.completion_source ();
                    // The Application lane is not a socket-visible route
                    // until its Completion sibling has validated the same
                    // pair. Delaying FQ/LB attachment also gives an incomplete
                    // or duplicate lane no scheduler state that can outlive
                    // the pair-fence rejection.
                    if (!pair.application_attached) {
                        attach_application = true;
                        pair.application_attached = true;
                    }
                }
            }
            pair_application = pair.application;
        }
        if (reject_pipes[0] && pair.application_attached
            && pair.application) {
            reject_attached_application = true;
            pair.application_attached = false;
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
        // Duplicate count-two lanes and cross-lane identity/topology conflicts
        // are only knowable when socket admission compares both validated
        // physical connections. Publish the READY protocol failure for every
        // network connection before terminating the related lane set.
        for (size_t i = 0; i < 3; ++i) {
            pipe_t *const rejected = reject_pipes[i];
            if (!rejected || rejected->get_transport_connection_id () == 0)
                continue;
            bool already_reported = false;
            for (size_t prior = 0; prior < i; ++prior)
                already_reported = already_reported
                                   || reject_pipes[prior] == rejected;
            if (!already_reported)
                event_handshake_failed_protocol (
                  rejected->get_endpoint_pair (),
                  ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_READY);
        }
        if (reject_attached_application && reject_pipes[1]) {
            receive_runtime_t &receive = receive_runtime ();
            scoped_lock_t receive_lock (receive.sync);
            xpipe_terminated (reject_pipes[1]);
            notify_receive_progress_locked ();
        }
        for (size_t i = 0; i < 3; ++i) {
            if (reject_pipes[i])
                reject_pipes[i]->terminate (false);
        }
        return;
    }

    if (attach_application) {
        pipe_t *application = pair_id == 0 ? pipe_ : pair_application;
        bool application_active = false;
        {
            receive_runtime_t &receive = receive_runtime ();
            scoped_lock_t receive_lock (receive.sync);
            // Linearize with pipe_terminated(), which owns the same receive
            // lock around xpipe_terminated(). If termination already removed
            // the pipe, skip this stale bind; if it starts after this check,
            // its callback will remove the FQ/LB registration we add here.
            application_active = application->is_lifecycle_active ();
            if (application_active) {
                xattach_pipe (
                  application, subscribe_to_all_,
                  locally_initiated_ || application->is_locally_initiated ());
                if (application->get_transport_pair_id () != 0
                    && application->get_transport_lane_count () == 1u)
                    (void) reclassify_transport_pair_application_head (
                      application);
                notify_receive_progress_locked ();
            }
        }
        if (!application_active)
            return;
        if (dispatch_runtime ().send_recovery_pending () && transport_has_out ()) {
            dispatch_runtime ().mark_send_recovery_ready ();
            static_cast<mailbox_t *> (_mailbox)->signal ();
        }
        // A DONTWAIT SEND may have registered while this logical target had
        // no application pipe. xattach_pipe() has now published scheduler and
        // route state; the matcher rechecks exact writability before moving
        // any token to the completion queue.
        notify_send_writable (application);
    }
    if (ready_application && ready_completion
        && ready_completion != ready_application)
        cache_completion_pipe_routing_id (ready_application);
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
    }
    // Install a cached pre-ready absolute policy while the Application pipe
    // is still held. This is initial scheduler state, so it is monitor-silent;
    // later owner commands publish only real dynamic transitions. The helper
    // takes transport-generation before any derived route lock.
    if (ready_application) {
        initialize_recorded_peer_weight (ready_application);
        transport_write_released =
          ready_application->release_writes_for_transport_pair ();
    }
    // A count-1 FLOWSTATE shares the Application connection and is forbidden
    // from bypassing the initial pair hold. Resync only after admission has
    // released that hold; the pipe then preserves this control ahead of the
    // WEIGHT resync below and behind any already committed wire bytes. Count 2
    // keeps using its dedicated Completion source through the same ordering
    // point.
    if (ready_completion)
        sync_local_receive_flow_state_to_pair (ready_completion);
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
        //  xattach_pipe() deliberately attempts no speculative paired
        //  control write: the Application lane was still held then. This is
        //  the one local-policy readiness resync for the current generation.
        (void) send_local_peer_weight (ready_application);
        write_activated (ready_application);
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
        if (ready_application && ready_completion != ready_application
            && ready_application->check_read ()) {
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
                const uint32_t weight =
                  static_cast<uint32_t> (options.peer_weight);
                if (_local_peer_weight.load (std::memory_order_relaxed)
                    != weight) {
                    _local_peer_weight.store (weight,
                                              std::memory_order_relaxed);
                    xlocal_peer_weight_changed ();
                }
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

    {
        socket_public_api_lock_scope_t guard (lifecycle);
        //  Only a caller that asks for ZLINK_POLLCOMPLETION owns the
        //  completion drain, so only that caller runs reply handlers.
        int drained_completions = 0;
        if (events_ & ZLINK_POLLCOMPLETION) {
            scoped_lock_t owner_lock (_completion_owner_sync);
            const completion_drain_scope_t drain_scope (this);
            (void) _request_completion_pending.exchange (
              false, std::memory_order_acq_rel);
            process_ready_completion_pipes ();
            drained_completions = drain_request_completions ();
            if (drained_completions < 0)
                return -1;
        }

        uint32_t events = 0;
        if ((events_ & ZLINK_POLLCOMPLETION)
            && socket_completion::has_ready (&completion_runtime ()))
            events |= ZLINK_POLLCOMPLETION;
        if ((events_ & ZLINK_POLLOUT) && has_out ())
            events |= ZLINK_POLLOUT;

        *out_ = events;
    }
    return 0;
}

int zlink::socket_base_t::get_events_internal (
  int events_, uint32_t *out_, bool consume_primary_signaler_)
{
    if (!out_) {
        errno = EINVAL;
        return -1;
    }

    if (lifecycle_coordinator ().public_close_requested ()) {
        *out_ = ZLINK_POLLERR;
        return 0;
    }

    const int rc = process_commands (0, false, false, NULL,
                                     consume_primary_signaler_);
    if (unlikely (rc != 0)) {
        if (errno == EINTR)
            return -1;
        if (errno == ETERM) {
            const int terminal_errno = errno;
            if ((events_ & ZLINK_POLLCOMPLETION) == 0)
                return -1;

            // stop() publishes context termination before its command reaches
            // process_stop(). Wake synchronous send waiters here as well.
            fail_all_blocking_send_waits (ETERM);

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
            }
            if (drained_completions > 0) {
                *out_ = ZLINK_POLLCOMPLETION;
                return 0;
            }

            errno = terminal_errno;
            return -1;
        }
    }
    errno_assert (rc == 0);

    int drained_completions = 0;
    if (events_ & ZLINK_POLLCOMPLETION) {
        zlink_assert (_completion_poller_refs.load (std::memory_order_acquire)
                      != 0);
        scoped_lock_t owner_lock (_completion_owner_sync);
        const completion_drain_scope_t drain_scope (this);
        (void) _request_completion_pending.exchange (
          false, std::memory_order_acq_rel);
        process_ready_completion_pipes ();
        drained_completions = drain_request_completions ();
        if (drained_completions < 0)
            return -1;
    }

    uint32_t events = 0;
    if ((events_ & ZLINK_POLLCOMPLETION)
        && socket_completion::has_ready (&completion_runtime ()))
        events |= ZLINK_POLLCOMPLETION;
    if ((events_ & ZLINK_POLLIN) && has_in ())
        events |= ZLINK_POLLIN;
    if ((events_ & ZLINK_POLLOUT) && has_out ())
        events |= ZLINK_POLLOUT;

    *out_ = events;
    return 0;
}

int zlink::socket_base_t::get_events_for_poller (int events_, uint32_t *out_,
                                                 bool transport_output_,
                                                 bool consume_primary_signaler_)
{
    socket_public_api_scope_t admission (lifecycle_coordinator ());
    if (!admission.acquired ()) {
        if (out_)
            *out_ = ZLINK_POLLERR;
        return 0;
    }

    const int public_events =
      transport_output_ ? events_ & ~ZLINK_POLLOUT : events_;

    //  The async owner is scheduled from the command pipe and never waits on
    //  this descriptor. Retire the poller's previous edge before sampling the
    //  logical state. A concurrent command then either becomes visible to the
    //  sample or publishes a fresh post-commit edge, so neither a permanent
    //  readable/busy loop nor a lost wake is possible.
    if (consume_primary_signaler_ && async_mailbox_owns_commands ())
        static_cast<mailbox_t *> (_mailbox)->drain_primary_signaler ();

    const int rc = get_events_internal (public_events, out_,
                                        consume_primary_signaler_);
    if (rc != 0)
        return rc;
    // Apply queued activate-write/flow-resume commands before sampling the
    // physical route. Their mailbox notification is the transport poller's
    // wake edge on every supported platform.
    if (transport_output_ && (events_ & ZLINK_POLLOUT)
        && transport_has_out ())
        *out_ |= ZLINK_POLLOUT;

    return 0;
}

int zlink::socket_base_t::drain_request_completions ()
{
    return 0;
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
    if (has_send_writable_wait ()
        || (socket_state
            && socket_reqrep_internal::has_pending_request_work (
              socket_state))) {
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
    //  Public part receive owns the complete physical record, then returns
    //  one buffered part per call. Once a multipart prefix is exposed, the
    //  remaining parts stay level-ready even though xhas_in() has already
    //  advanced to the next physical record (which may be a private REPLY).
    if (part_helper_recv_ready ()) {
        const std::shared_ptr<part_helper_internal::handle_state_t> state =
          part_helper_state ();
        if (state) {
            std::lock_guard<std::mutex> lock (state->mutex);
            if (state->recv.active
                && state->recv.next_part_index
                     < state->recv.buffered_parts.size ())
                return true;
        }
    }
    scoped_lock_t lock (receive_runtime ().sync);
    return xhas_in ();
}

bool zlink::socket_base_t::has_out ()
{
    // Each unread WRITABLE completion keeps POLLOUT level-ready independently
    // of the legacy one-shot recovery flag. A successful competing send may
    // clear that flag, but must not hide another wait token's notification.
    if (socket_completion::has_ready_writable (&completion_runtime ()))
        return true;
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

bool zlink::socket_base_t::enqueue_count1_completion_pipe (pipe_t *pipe_)
{
    if (!pipe_ || pipe_->get_transport_pair_id () == 0
        || pipe_->get_transport_lane () != transport_lane_application
        || pipe_->get_transport_lane_count () != 1u
        || !pipe_->transport_pair_application_ready_cached ())
        return false;

    //  The ready queue outlives a transport-pair table slot. Pin both the pipe
    //  object and its independently retired inbound ypipe before publishing the
    //  intrusive node, then let a stale detach be filtered by the ready cache at
    //  the consumer boundary.
    if (!pipe_->retain_lifetime_ref ())
        return false;
    if (!pipe_->retain_inbound_read_ref ()) {
        pipe_->release_lifetime_ref ();
        return false;
    }

    unsigned char expected = pipe_t::count1_completion_idle;
    if (!pipe_->_count1_completion_ready_state.compare_exchange_strong (
          expected, pipe_t::count1_completion_queued,
          std::memory_order_acq_rel, std::memory_order_acquire)) {
        pipe_->release_inbound_read_ref ();
        pipe_->release_lifetime_ref ();
        return false;
    }

    //  Readiness can be invalidated after the first cache load but before the
    //  node becomes visible. No consumer can see it yet, so cancel this local
    //  publication synchronously and return both pins.
    if (!pipe_->transport_pair_application_ready_cached ()) {
        pipe_->_count1_completion_ready_state.store (
          pipe_t::count1_completion_idle, std::memory_order_release);
        pipe_->release_inbound_read_ref ();
        pipe_->release_lifetime_ref ();
        return false;
    }

    pipe_t *head =
      _ready_count1_completion_pipes.load (std::memory_order_acquire);
    do {
        pipe_->_count1_completion_ready_next.store (
          head, std::memory_order_relaxed);
    } while (!_ready_count1_completion_pipes.compare_exchange_weak (
      head, pipe_, std::memory_order_release, std::memory_order_acquire));
    return true;
}

bool zlink::socket_base_t::release_count1_completion_drain (pipe_t *pipe_)
{
    if (!pipe_)
        return false;
    unsigned char expected = pipe_t::count1_completion_draining;
    return pipe_->_count1_completion_ready_state.compare_exchange_strong (
      expected, pipe_t::count1_completion_idle, std::memory_order_acq_rel,
      std::memory_order_acquire);
}

void zlink::socket_base_t::discard_count1_completion_ready_pipes ()
{
    pipe_t *pipe =
      _ready_count1_completion_pipes.exchange (NULL, std::memory_order_acq_rel);
    while (pipe) {
        pipe_t *const next = pipe->_count1_completion_ready_next.load (
          std::memory_order_relaxed);
        pipe->_count1_completion_ready_next.store (NULL,
                                                    std::memory_order_relaxed);
        unsigned char expected = pipe_t::count1_completion_queued;
        const bool released =
          pipe->_count1_completion_ready_state.compare_exchange_strong (
            expected, pipe_t::count1_completion_idle,
            std::memory_order_acq_rel, std::memory_order_acquire);
        zlink_assert (released);
        pipe->release_inbound_read_ref ();
        pipe->release_lifetime_ref ();
        pipe = next;
    }
}

bool zlink::socket_base_t::reclassify_transport_pair_application_head (
  pipe_t *pipe_, bool claim_private_head_)
{
    if (!pipe_ || pipe_->get_transport_pair_id () == 0
        || pipe_->get_transport_lane () != transport_lane_application
        || pipe_->get_transport_lane_count () != 1u
        || !pipe_->transport_pair_application_ready_cached ())
        return false;

    const transport_pair_key_t key (pipe_->get_transport_pair_id (),
                                    pipe_->get_transport_pair_generation ());
    //  A ready count-1 Application pipe answers the common topology question
    //  from its admission-time cache. Only an actual public delivery hold
    //  needs the pair table to validate and pin the held generation.
    if (_public_part_receive_delivery_hold_active.load (
          std::memory_order_acquire)) {
        scoped_lock_t lock (_transport_pairs_sync);
        transport_pairs_t::const_iterator it = _transport_pairs.find (key);
        if (it == _transport_pairs.end () || !it->second.ready
            || it->second.expected_lane_count != 1u
            || it->second.application != pipe_)
            return false;
        if (_public_part_receive_delivery_hold_active
            && (!_public_part_receive_delivery_hold_pipe
                || (_public_part_receive_delivery_hold_pipe == pipe_
                    && _public_part_receive_delivery_hold_key == key))) {
            if (!_public_part_receive_delivery_hold_pipe) {
                _public_part_receive_delivery_hold_pipe = pipe_;
                _public_part_receive_delivery_hold_key = key;
            }
            return false;
        }
    }

    const pipe_normalized_head_kind_t head =
      pipe_->probe_normalized_head_kind ();
    if (head == pipe_head_data || head == pipe_head_request) {
        if (!pipe_->public_receive_active_cached ())
            xread_activated (pipe_);
        return false;
    }

    if (pipe_->public_receive_active_cached ())
        xread_deactivated (pipe_);
    if (head == pipe_head_empty)
        return false;
    if (head == pipe_head_invalid) {
        pipe_->terminate (false);
        return false;
    }

    if (claim_private_head_) {
        if (!pipe_->transport_pair_application_ready_cached ())
            return false;
        unsigned char expected = pipe_t::count1_completion_idle;
        return pipe_->_count1_completion_ready_state.compare_exchange_strong (
          expected, pipe_t::count1_completion_draining,
          std::memory_order_acq_rel, std::memory_order_acquire);
    }

    const bool queued = enqueue_count1_completion_pipe (pipe_);
    if (queued)
        notify_request_completion ();
    return queued;
}

int zlink::socket_base_t::begin_public_part_receive_delivery_hold ()
{
    scoped_lock_t lock (_transport_pairs_sync);
    if (_public_part_receive_delivery_hold_active) {
        errno = EBUSY;
        return -1;
    }
    _public_part_receive_delivery_hold_active = true;
    _public_part_receive_delivery_hold_pipe = NULL;
    _public_part_receive_delivery_hold_key = transport_pair_key_t (0, 0);
    return 0;
}

void zlink::socket_base_t::bind_public_part_receive_delivery_hold (
  pipe_t *source_pipe_)
{
    if (!source_pipe_
        || source_pipe_->get_transport_lane ()
             != transport_lane_application
        || source_pipe_->get_transport_lane_count () != 1u)
        return;

    const transport_pair_key_t key (
      source_pipe_->get_transport_pair_id (),
      source_pipe_->get_transport_pair_generation ());
    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pairs_t::const_iterator it = _transport_pairs.find (key);
    if (_public_part_receive_delivery_hold_active
        && it != _transport_pairs.end () && it->second.ready
        && it->second.expected_lane_count == 1u
        && it->second.application == source_pipe_) {
        _public_part_receive_delivery_hold_pipe = source_pipe_;
        _public_part_receive_delivery_hold_key = key;
    }
}

void zlink::socket_base_t::end_public_part_receive_delivery_hold (
  bool receive_sync_held_)
{
    //  Steady-state receives never hold; skip both locks unless a hold was
    //  published. The flag is only set by this socket's own request/reply
    //  path, so an unlocked read cannot miss a hold that must be released.
    if (!_public_part_receive_delivery_hold_active.load (
          std::memory_order_acquire))
        return;
    receive_runtime_t &receive = receive_runtime ();
    const auto release = [&] () {
        pipe_t *held_pipe = NULL;
        bool lifetime_retained = false;
        bool inbound_retained = false;
        bool had_hold = false;
        {
            scoped_lock_t lock (_transport_pairs_sync);
            had_hold = _public_part_receive_delivery_hold_active;
            if (!had_hold)
                return;

            if (_public_part_receive_delivery_hold_pipe) {
                const transport_pairs_t::const_iterator it =
                  _transport_pairs.find (
                    _public_part_receive_delivery_hold_key);
                if (it != _transport_pairs.end () && it->second.ready
                    && it->second.expected_lane_count == 1u
                    && it->second.application
                         == _public_part_receive_delivery_hold_pipe) {
                    held_pipe = it->second.application;
                    lifetime_retained = held_pipe->retain_lifetime_ref ();
                    if (lifetime_retained)
                        inbound_retained =
                          held_pipe->retain_inbound_read_ref ();
                    if (!inbound_retained) {
                        if (lifetime_retained)
                            held_pipe->release_lifetime_ref ();
                        held_pipe = NULL;
                        lifetime_retained = false;
                    }
                }
            }

            _public_part_receive_delivery_hold_active = false;
            _public_part_receive_delivery_hold_pipe = NULL;
            _public_part_receive_delivery_hold_key =
              transport_pair_key_t (0, 0);
        }

        if (held_pipe)
            (void) reclassify_transport_pair_application_head (held_pipe);
        notify_receive_progress_locked ();
        if (held_pipe) {
            held_pipe->release_inbound_read_ref ();
            held_pipe->release_lifetime_ref ();
        }
    };

    if (receive_sync_held_) {
        release ();
        return;
    }
    scoped_lock_t receive_lock (receive.sync);
    release ();
}

#ifdef ZLINK_BUILD_TESTS
bool zlink::socket_base_t::test_completion_pair_queued (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_) const
{
    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pair_key_t key (transport_pair_id_,
                                    transport_pair_generation_);
    const transport_pairs_t::const_iterator it = _transport_pairs.find (key);
    if (it == _transport_pairs.end () || !it->second.ready)
        return false;
    pipe_t *const source = it->second.completion_source ();
    if (source && source->get_transport_lane_count () == 1u
        && source->get_transport_lane () == transport_lane_application)
        return source->_count1_completion_ready_state.load (
                 std::memory_order_acquire)
               == pipe_t::count1_completion_queued;
    return _ready_completion_pair_set.count (key) != 0;
}

int zlink::socket_base_t::test_process_commands_only ()
{
    return process_commands (0, false, true);
}
#endif

void zlink::socket_base_t::process_ready_completion_pipes ()
{
    //  Producers publish count-1 private heads without the transport-pair table
    //  mutex. Detach one finite MPSC batch and reverse its Treiber order so the
    //  oldest publication drains first. A budget requeue is published only
    //  after this exchange and therefore belongs to the next owner turn.
    pipe_t *published =
      _ready_count1_completion_pipes.exchange (NULL, std::memory_order_acq_rel);
    pipe_t *count1_ready = NULL;
    while (published) {
        pipe_t *const next = published->_count1_completion_ready_next.load (
          std::memory_order_relaxed);
        published->_count1_completion_ready_next.store (
          count1_ready, std::memory_order_relaxed);
        count1_ready = published;
        published = next;
    }

    while (count1_ready) {
        pipe_t *const completion = count1_ready;
        count1_ready = completion->_count1_completion_ready_next.load (
          std::memory_order_relaxed);
        // Clear the detached-list link before publishing draining. A budget
        // requeue may reuse this exact intrusive node while the current owner
        // still holds the old queue references.
        completion->_count1_completion_ready_next.store (
          NULL, std::memory_order_relaxed);

        unsigned char expected = pipe_t::count1_completion_queued;
        const bool claimed =
          completion->_count1_completion_ready_state.compare_exchange_strong (
            expected, pipe_t::count1_completion_draining,
            std::memory_order_acq_rel, std::memory_order_acquire);
        zlink_assert (claimed);

        if (completion->transport_pair_application_ready_cached ()
            && completion->get_transport_pair_id () != 0
            && completion->get_transport_lane () == transport_lane_application
            && completion->get_transport_lane_count () == 1u) {
            drain_claimed_completion_pipe (
              completion->get_transport_pair_id (),
              completion->get_transport_pair_generation (), completion);
        } else {
            const bool released = release_count1_completion_drain (completion);
            zlink_assert (released);
        }
        completion->release_inbound_read_ref ();
        completion->release_lifetime_ref ();
    }

    //  Drain one claimed pipe at a time with the table unlocked: draining runs
    //  the application's reply handler, which may call back into this socket.
    //  The iterative form also avoids an allocation after a pipe was pinned.
    size_t queued_at_entry = 0;
    {
        scoped_lock_t lock (_transport_pairs_sync);
        queued_at_entry = _ready_completion_pairs.size ();
    }

    //  A pipe that exhausts its budget is appended to the tail, but it belongs
    //  to the next owner turn. Consume only the pop slots present on entry so
    //  this invocation cannot immediately select its own requeue.
    while (queued_at_entry != 0) {
        transport_pair_key_t key (0, 0);
        pipe_t *completion = NULL;
        {
            scoped_lock_t lock (_transport_pairs_sync);
            while (queued_at_entry != 0
                   && !_ready_completion_pairs.empty () && !completion) {
                key = _ready_completion_pairs.front ();
                _ready_completion_pairs.pop_front ();
                _ready_completion_pair_set.erase (key);
                --queued_at_entry;
                transport_pairs_t::iterator it = _transport_pairs.find (key);
                if (it == _transport_pairs.end () || !it->second.ready
                    || !it->second.completion_source ()
                    || it->second.draining)
                    continue;
                pipe_t *const completion_source =
                  it->second.completion_source ();
                // Retain while the table slot still proves liveness. The slot
                // can be cleared and the pipe deallocated on a second mailbox
                // executor as soon as this mutex is released.
                if (!completion_source->retain_lifetime_ref ())
                    continue;
                if (!completion_source->retain_inbound_read_ref ()) {
                    completion_source->release_lifetime_ref ();
                    continue;
                }
                it->second.draining = true;
                completion = completion_source;
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

    if (completion_pipe_->get_transport_lane_count () == 1u
        && completion_pipe_->get_transport_lane ()
             == transport_lane_application) {
        if (!release_count1_completion_drain (completion_pipe_))
            return false;
        // Reclassification handles PUBLIC/empty transitions as well as the
        // private case. If a private record raced the final empty read, claim it
        // directly for this owner rather than round-tripping through the queue.
        return reclassify_transport_pair_application_head (completion_pipe_,
                                                            true);
    }

    const transport_pair_key_t pair_key (transport_pair_id_,
                                         transport_pair_generation_);
    bool exact_pair = false;
    {
        scoped_lock_t lock (_transport_pairs_sync);
        transport_pairs_t::iterator it = _transport_pairs.find (pair_key);
        if (it != _transport_pairs.end () && it->second.ready
            && it->second.completion_source () == completion_pipe_
            && it->second.draining) {
            //  Publish the idle state before checking the pipe. An activation
            //  after this point can claim or queue the pair itself; an
            //  activation that arrived while draining was true is recovered
            //  by the check_read below.
            it->second.draining = false;
            exact_pair = true;
        }
    }
    if (!exact_pair)
        return false;
    if (!completion_pipe_->check_read ()) {
        return false;
    }

    //  Never call pipe methods while holding the table mutex. Revalidate the
    //  slot after check_read and claim it only if another completion owner did
    //  not get there first.
    scoped_lock_t lock (_transport_pairs_sync);
    transport_pairs_t::iterator it = _transport_pairs.find (pair_key);
    if (it == _transport_pairs.end () || !it->second.ready
        || it->second.completion_source () != completion_pipe_
        || it->second.draining)
        return false;
    it->second.draining = true;
    return true;
}

bool zlink::socket_base_t::requeue_completion_pipe_after_budget (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
  pipe_t *completion_pipe_, bool receive_sync_held_)
{
    if (!completion_pipe_)
        return false;

    if (completion_pipe_->get_transport_lane_count () == 1u
        && completion_pipe_->get_transport_lane ()
             == transport_lane_application) {
        if (!release_count1_completion_drain (completion_pipe_))
            return false;
        const bool queued =
          reclassify_transport_pair_application_head (completion_pipe_);
        if (receive_sync_held_)
            notify_receive_progress_locked ();
        else
            notify_receive_progress ();
        return queued;
    }

    const transport_pair_key_t pair_key (transport_pair_id_,
                                         transport_pair_generation_);
    {
        scoped_lock_t lock (_transport_pairs_sync);
        transport_pairs_t::iterator it = _transport_pairs.find (pair_key);
        if (it == _transport_pairs.end () || !it->second.ready
            || it->second.completion_source () != completion_pipe_
            || !it->second.draining)
            return false;
        it->second.draining = false;
    }

    if (!completion_pipe_->check_read ())
        return false;

    bool queued = false;
    {
        //  check_read() ran without the table mutex. Fence a detach, generation
        //  replacement, or competing claim before publishing the old key.
        scoped_lock_t lock (_transport_pairs_sync);
        transport_pairs_t::iterator it = _transport_pairs.find (pair_key);
        if (it != _transport_pairs.end () && it->second.ready
            && it->second.completion_source () == completion_pipe_
            && !it->second.draining
            && _ready_completion_pair_set.insert (pair_key).second) {
            _ready_completion_pairs.push_back (pair_key);
            queued = true;
        }
    }
    if (queued)
        notify_request_completion ();
    return queued;
}

void zlink::socket_base_t::drain_claimed_completion_pipe (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
  pipe_t *completion_pipe_)
{
    const bool count1_application =
      completion_pipe_ && completion_pipe_->get_transport_lane_count () == 1u
      && completion_pipe_->get_transport_lane ()
           == transport_lane_application;
    receive_runtime_t &receive = receive_runtime ();
    const auto drain = [&] (bool receive_sync_held_) {
        while (true) {
            const socket_reqrep_internal::completion_pipe_drain_result_t result =
              socket_reqrep_internal::process_completion_pipe (
                this, completion_pipe_);
            if (result
                == socket_reqrep_internal::completion_pipe_public_head) {
                if (count1_application) {
                    const bool released =
                      release_count1_completion_drain (completion_pipe_);
                    zlink_assert (released);
                } else {
                    const transport_pair_key_t key (
                      transport_pair_id_, transport_pair_generation_);
                    scoped_lock_t lock (_transport_pairs_sync);
                    transport_pairs_t::iterator it =
                      _transport_pairs.find (key);
                    if (it != _transport_pairs.end ()
                        && it->second.completion_source () == completion_pipe_)
                        it->second.draining = false;
                }
                (void) reclassify_transport_pair_application_head (
                  completion_pipe_);
                if (receive_sync_held_)
                    notify_receive_progress_locked ();
                else
                    notify_receive_progress ();
                return;
            }
            if (result
                == socket_reqrep_internal::completion_pipe_terminated) {
                if (count1_application)
                    (void) release_count1_completion_drain (completion_pipe_);
                return;
            }
            if (result
                == socket_reqrep_internal::completion_pipe_budget_exhausted) {
                (void) requeue_completion_pipe_after_budget (
                  transport_pair_id_, transport_pair_generation_,
                  completion_pipe_, receive_sync_held_);
                return;
            }
            if (!finish_completion_pipe_drain (
                  transport_pair_id_, transport_pair_generation_,
                  completion_pipe_))
                return;
        }
    };

    if (!count1_application) {
        drain (false);
        return;
    }
    if (receive.try_acquire_public_receive_lease ()) {
        drain (false);
        receive.release_public_receive_lease ();
        return;
    }
    scoped_lock_t receive_lock (receive.sync);
    drain (true);
}

void zlink::socket_base_t::read_activated (pipe_t *pipe_)
{
    if (pipe_ && pipe_->get_transport_pair_id () != 0
        && pipe_->get_transport_lane () == transport_lane_application
        && pipe_->get_transport_lane_count () == 1u) {
        {
            receive_runtime_t &receive = receive_runtime ();
            scoped_lock_t receive_lock (receive.sync);
            (void) reclassify_transport_pair_application_head (pipe_);
            notify_receive_progress_locked ();
        }
        if (completion_drain_permitted ())
            process_ready_completion_pipes ();
        return;
    }
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
                && it->second.completion_source () == pipe_
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
        bool request_correlation_recovery = false;
        bool flow_recovery = false;
        if (pipe_) {
            credit_recovery = pipe_->take_hwm_credit_recovery ();
            request_correlation_recovery =
              pipe_->take_request_correlation_recovery ();
            flow_recovery = pipe_->take_flow_resume_recovery ();
        }
        if (request_correlation_recovery)
            dispatch_runtime ().mark_send_recovery_pending ();
        // Sample WRITABLE only after consuming the marker for this activation.
        // A competing send can fill the newly active pipe and arm a fresh byte
        // waiter before this recheck; check_write_admission() then preserves
        // that new marker for the next credit edge instead of having it erased
        // by this callback.
        notify_send_writable (pipe_);
    }
    if (dispatch_runtime ().send_recovery_pending ()
        && !dispatch_runtime ().send_recovery_ready ()) {
        dispatch_runtime ().mark_send_recovery_ready ();
        static_cast<mailbox_t *> (_mailbox)->signal ();
    }
    // This callback is reached only after a pipe removed a real write blocker
    // (byte credit, remote flow, or the initial pair hold). It can also run
    // directly during inproc pair admission, outside process_commands().
    notify_submit_progress ();
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
        endpoint_pair.connection_id = pipe_->get_transport_connection_id ();
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
    // Only the connecting socket records a pipe in inprocs. Explicit
    // zlink_disconnect erases that record before termination, whereas an
    // unexpected peer detach leaves it available as the reconnect intent.
    std::string inproc_reconnect_endpoint;
    const bool reconnect_inproc =
      pipe_ && !completion && !is_terminating () && options.reconnect_ivl > 0
      && endpoint_runtime ().inprocs.endpoint_for_pipe (
        pipe_, &inproc_reconnect_endpoint);
    // A locally requested endpoint termination may tear the engine down
    // without entering asio_engine_t::error(). Publish its physical disconnect
    // here. The per-pipe claim makes this mutually exclusive with the normal
    // transport-error producer.
    if (pipe_ && pair_id != 0
        && pipe_->try_claim_transport_disconnected_event ()) {
        event_disconnected (
          endpoint_pair, ZLINK_DISCONNECT_UNKNOWN, routing_id_data,
          routing_id_size, pipe_->get_transport_lane (), pair_id,
          pair_generation);
    }
    if (pipe_ && pair_id != 0 && !pipe_->is_locally_initiated ()) {
        const blob_t &accepted_identity =
          pipe_->get_transport_peer_identity ();
        release_accepted_transport_pair (
          accepted_identity.size () != 0 ? accepted_identity.data ()
                                         : routing_id_data,
          accepted_identity.size () != 0 ? accepted_identity.size ()
                                         : routing_id_size,
          pair_id, pair_generation);
    }
    bool application_attached = pair_id == 0;
    bool release_paused_pair_accounting = false;
    pipe_t *paused_pair_application = NULL;
    if (pair_id != 0) {
        scoped_lock_t lock (_transport_pairs_sync);
        transport_pairs_t::iterator pair_it =
          _transport_pairs.find (transport_pair_key_t (
            pair_id, pipe_->get_transport_pair_generation ()));
        if (pair_it != _transport_pairs.end ()) {
            application_attached = pair_it->second.application_attached;
            paired_pipe = completion ? pair_it->second.application : pair_it->second.completion;
            // Readiness is an all-required-lanes invariant. The first physical
            // detach ends it immediately; retaining the surviving sibling only
            // keeps teardown/lifetime state, never a usable ready pair.
            pair_it->second.ready = false;
            if (pair_it->second.application) {
                pair_it->second.application
                  ->set_transport_pair_application_ready (false);
                pair_it->second.application
                  ->set_transport_pair_completion_pipe (NULL);
            }
            pair_it->second.draining = false;
            _ready_completion_pair_set.erase (transport_pair_key_t (
              pair_id, pipe_->get_transport_pair_generation ()));
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
            //  its +1 on the gauge would never be matched. Either physical
            //  lane can be the first termination callback. Claim the pair
            //  accounting on that first callback, while the table still owns
            //  the one-shot flag, and use the retained Application sibling for
            //  duration accounting when Completion terminated first.
            if (pair_it->second.remote_flow_pause_accounted) {
                pair_it->second.remote_flow_pause_accounted = false;
                release_paused_pair_accounting = true;
                paused_pair_application = completion ? paired_pipe : pipe_;
            }
            if (!pair_it->second.application && !pair_it->second.completion)
                _transport_pairs.erase (pair_it);
        }
    }

    if (release_paused_pair_accounting)
        flow_pause_released_on_termination (paused_pair_application);

    if (!completion && application_attached) {
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
               transport_lane_application,
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

    if (paired_pipe) {
        if (!is_terminating ())
            paired_pipe->terminate (false);
        //  Releases the pin taken above; this is the last reference when the
        //  sibling already completed its own termination, so the deallocation
        //  happens here instead of racing us.
        paired_pipe->release_lifetime_ref ();
    }

    // pipe_terminated runs under the command owner. Queue the reconnect so
    // connect_internal does not re-enter that owner while handling this pipe.
    if (reconnect_inproc && !is_terminating ())
        send_reconnect_inproc (
          this, new std::string (inproc_reconnect_endpoint));
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

zlink::pipe_t *zlink::socket_base_t::retain_transport_pair_pipe (
  uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
  transport_lane_t lane_) const
{
    if (transport_pair_id_ == 0 || transport_pair_generation_ == 0
        || (lane_ != transport_lane_application
            && lane_ != transport_lane_completion))
        return NULL;

    scoped_lock_t lock (_transport_pairs_sync);
    const transport_pair_key_t key (transport_pair_id_,
                                    transport_pair_generation_);
    const transport_pairs_t::const_iterator it = _transport_pairs.find (key);
    if (it == _transport_pairs.end () || !it->second.ready)
        return NULL;
    pipe_t *const selected = lane_ == transport_lane_application
                               ? it->second.application
                               : it->second.completion;
    if (!selected || !it->second.application
        || !it->second.application->is_lifecycle_active ()
        || !selected->is_lifecycle_active ())
        return NULL;
    return selected->retain_lifetime_ref () ? selected : NULL;
}

zlink::pipe_t *zlink::socket_base_t::retain_current_transport_pair_pipe (
  const zlink_routing_id_t *peer_rid_, int peer_socket_type_,
  transport_lane_t lane_) const
{
    if (!peer_rid_ || peer_rid_->size == 0
        || (lane_ != transport_lane_application
            && lane_ != transport_lane_completion))
        return NULL;

    pipe_t *selected = NULL;
    pipe_t *selected_application = NULL;
    uint64_t selected_connection_id = 0;
    scoped_lock_t lock (_transport_pairs_sync);
    for (transport_pairs_t::const_iterator it = _transport_pairs.begin (),
                                           end = _transport_pairs.end ();
         it != end; ++it) {
        if (!it->second.ready
            || (it->second.expected_lane_count != 1u
                && it->second.expected_lane_count != 2u)
            || (lane_ == transport_lane_completion
                && it->second.expected_lane_count != 2u))
            continue;

        pipe_t *const application = it->second.application;
        pipe_t *const candidate = lane_ == transport_lane_application
                                    ? application
                                    : it->second.completion;
        if (!application || !candidate
            || application->get_peer_socket_type () != peer_socket_type_
            || candidate->get_peer_socket_type () != peer_socket_type_
            || application->get_transport_lane_count ()
                 != it->second.expected_lane_count
            || candidate->get_transport_lane_count ()
                 != it->second.expected_lane_count)
            continue;

        const blob_t &application_rid = application->get_routing_id ();
        const blob_t &candidate_rid = candidate->get_routing_id ();
        const blob_t &rid = application_rid.size () != 0 ? application_rid
                                                         : candidate_rid;
        if (rid.size () != peer_rid_->size
            || memcmp (rid.data (), peer_rid_->data, rid.size ()) != 0)
            continue;

        const uint64_t connection_id =
          application->get_transport_connection_id ();
        if (connection_id == 0 || connection_id <= selected_connection_id)
            continue;

        selected = candidate;
        selected_application = application;
        selected_connection_id = connection_id;
    }

    // Do not fall back to an older ready record if the selected latest
    // connection has started teardown but its table callback has not run yet.
    if (!selected || !selected_application->is_lifecycle_active ()
        || !selected->is_lifecycle_active ()
        || selected->get_transport_connection_id () == 0)
        return NULL;
    return selected->retain_lifetime_ref () ? selected : NULL;
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
    //  The pair table publishes this bit into the Application pipe at
    //  admission and clears it on the first physical detach (see
    //  `set_transport_pair_application_ready`), so the per-message send and
    //  receive paths read it without the table mutex.
    return pipe_ && pipe_->get_transport_lane () == transport_lane_application
           && pipe_->transport_pair_application_ready_cached ();
}

int zlink::socket_base_t::socket_id () const
{
    return options.socket_id;
}

bool zlink::socket_base_t::is_ctx_terminated () const
{
    return _ctx_terminated.load (std::memory_order_acquire);
}
