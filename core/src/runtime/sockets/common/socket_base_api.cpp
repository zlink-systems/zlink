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
}

void zlink::socket_base_t::finish_close_handoff (int handoff_timeout_ms_)
{
    lifecycle_coordinator ().complete_deferred_close_handoff (static_cast<mailbox_t *> (_mailbox),
                                                              handoff_timeout_ms_);

    _tag = 0xdeadbeef;
    send_reap (this);
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
        if (!matches && pipe->get_peer ()) {
            const blob_t &peer_routing_id = pipe->get_peer ()->get_routing_id ();
            matches = peer_routing_id.size () == peer_rid_->size && peer_routing_id.size () > 0
                      && memcmp (peer_routing_id.data (), peer_rid_->data, peer_rid_->size) == 0;
        }
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

    match->terminate (false);
    return 0;
}

void zlink::socket_base_t::attach_pipe (pipe_t *pipe_,
                                        bool subscribe_to_all_,
                                        bool locally_initiated_,
                                        bool transport_validated_)
{
    pipe_->set_event_sink (this);
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
                if (!same_pair_peer_identity (pair.application, pair.completion)) {
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
        xattach_pipe (application, subscribe_to_all_,
                      locally_initiated_ || application->is_locally_initiated ());
        if (dispatch_runtime ().send_recovery_pending () && transport_has_out ()) {
            dispatch_runtime ().mark_send_recovery_ready ();
            static_cast<mailbox_t *> (_mailbox)->signal ();
        }
    }
    if (ready_application
        && ready_application->release_writes_for_transport_pair ())
        write_activated (ready_application);
    if (ready_completion) {
        if (ready_completion->check_read ()) {
            if (completion_drain_permitted ())
                socket_reqrep_internal::process_completion_pipe (
                  this, ready_completion);
            else {
                scoped_lock_t lock (_transport_pairs_sync);
                if (_ready_completion_pair_set.insert (pair_key).second)
                    _ready_completion_pairs.push_back (pair_key);
                notify_request_completion ();
            }
        }
        if (ready_application && ready_application->check_read ())
            xread_activated (ready_application);
    } else if (completion) {
        bool pair_ready = false;
        {
            scoped_lock_t lock (_transport_pairs_sync);
            pair_ready = _transport_pairs[pair_key].ready;
        }
        if (pair_ready && pipe_->check_read ()) {
            if (completion_drain_permitted ())
                socket_reqrep_internal::process_completion_pipe (this, pipe_);
            else {
                scoped_lock_t lock (_transport_pairs_sync);
                if (_ready_completion_pair_set.insert (pair_key).second)
                    _ready_completion_pairs.push_back (pair_key);
                notify_request_completion ();
            }
        }
    }
    if (get_ctx ())
        get_ctx ()->schedule_auto_hwm_recalculate ();

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
        rc = options.setsockopt (option_, optval_, optvallen_);
        if (rc == 0) {
            if (option_ == ZLINK_INTERNAL_OPT_SNDHWM)
                _manual_sndhwm = true;
            else if (option_ == ZLINK_INTERNAL_OPT_RCVHWM)
                _manual_rcvhwm = true;
            else if (option_ == ZLINK_INTERNAL_OPT_SNDBUF)
                _manual_sndbuf = true;
            else if (option_ == ZLINK_INTERNAL_OPT_RCVBUF)
                _manual_rcvbuf = true;
            else if (option_ == ZLINK_INTERNAL_OPT_AUTO_HWM_MSG_UNIT_BYTES)
                _auto_hwm_msg_unit_override = options.auto_hwm_msg_unit_bytes > 0;

            if (option_ == ZLINK_INTERNAL_OPT_AUTO_HWM_MSG_UNIT_BYTES
                || option_ == ZLINK_INTERNAL_OPT_SNDBUF || option_ == ZLINK_INTERNAL_OPT_RCVBUF) {
                _auto_hwm_last_recalc_reason = ZLINK_AUTO_HWM_RECALC_REASON_REFRESH;
                refresh_auto_hwm_policy ();
            }

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

    if (unlikely (_ctx_terminated)) {
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

        //  Only a caller that asks for ZLINK_POLLCOMPLETION owns the
        //  completion drain, so only that caller runs reply handlers.
        bool completion_notified = false;
        int completion_controls = 0;
        if (events_ & ZLINK_POLLCOMPLETION) {
            const completion_drain_scope_t drain_scope (this);
            completion_notified =
              _request_completion_pending.exchange (false, std::memory_order_acq_rel);
            process_ready_completion_pipes ();
            completion_controls = drain_request_completion_controls ();
            if (completion_controls < 0)
                return -1;
        }

        uint32_t events = 0;
        if ((events_ & ZLINK_POLLCOMPLETION)
            && (completion_notified || completion_controls > 0))
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

    const int rc = process_commands (0, false);
    if (rc != 0 && (errno == EINTR || errno == ETERM))
        return -1;
    errno_assert (rc == 0);

    bool completion_notified = false;
    int completion_controls = 0;
    if (events_ & ZLINK_POLLCOMPLETION) {
        const completion_drain_scope_t drain_scope (this);
        completion_notified =
          _request_completion_pending.exchange (false, std::memory_order_acq_rel);
        process_ready_completion_pipes ();
        completion_controls = drain_request_completion_controls ();
        if (completion_controls < 0)
            return -1;
    }

    uint32_t events = 0;
    if ((events_ & ZLINK_POLLCOMPLETION)
        && (completion_notified || completion_controls > 0))
        events |= ZLINK_POLLCOMPLETION;
    if ((events_ & ZLINK_POLLIN) && has_in ())
        events |= ZLINK_POLLIN;
    if ((events_ & ZLINK_POLLOUT) && has_out ())
        events |= ZLINK_POLLOUT;

    *out_ = events;
    return 0;
}

int zlink::socket_base_t::drain_request_completion_controls ()
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
    //  Claim the pipes first and drain them with the table unlocked: draining
    //  runs the application's reply handler, which may call back into this
    //  socket.
    std::vector<transport_pair_key_t> claimed;
    {
        scoped_lock_t lock (_transport_pairs_sync);
        while (!_ready_completion_pairs.empty ()) {
            const transport_pair_key_t key = _ready_completion_pairs.front ();
            _ready_completion_pairs.pop_front ();
            _ready_completion_pair_set.erase (key);
            transport_pairs_t::iterator it = _transport_pairs.find (key);
            if (it == _transport_pairs.end () || !it->second.ready
                || !it->second.completion || it->second.draining)
                continue;
            it->second.draining = true;
            claimed.push_back (key);
        }
    }

    for (size_t i = 0; i < claimed.size (); ++i) {
        pipe_t *completion = NULL;
        {
            scoped_lock_t lock (_transport_pairs_sync);
            transport_pairs_t::const_iterator it = _transport_pairs.find (claimed[i]);
            if (it != _transport_pairs.end ())
                completion = it->second.completion;
        }
        if (completion && completion->check_read ())
            socket_reqrep_internal::process_completion_pipe (this, completion);
        scoped_lock_t lock (_transport_pairs_sync);
        transport_pairs_t::iterator it = _transport_pairs.find (claimed[i]);
        if (it != _transport_pairs.end ())
            it->second.draining = false;
    }
}

void zlink::socket_base_t::read_activated (pipe_t *pipe_)
{
    if (pipe_ && pipe_->get_transport_pair_id () != 0
        && pipe_->get_transport_lane () == transport_lane_completion) {
        bool claimed = false;
        const transport_pair_key_t pair_key (pipe_->get_transport_pair_id (),
                                             pipe_->get_transport_pair_generation ());
        {
            scoped_lock_t lock (_transport_pairs_sync);
            transport_pairs_t::iterator it = _transport_pairs.find (pair_key);
            if (it != _transport_pairs.end () && it->second.ready
                && !it->second.draining) {
                claimed = true;
                if (completion_drain_permitted ())
                    it->second.draining = true;
            }
        }
        if (!claimed)
            return;
        if (!completion_drain_permitted ()) {
            //  No owner is draining on this thread. Record the readiness so the
            //  completion owner wakes up and drains the pipe itself.
            scoped_lock_t lock (_transport_pairs_sync);
            if (_ready_completion_pair_set.insert (pair_key).second)
                _ready_completion_pairs.push_back (pair_key);
            notify_request_completion ();
            return;
        }
        socket_reqrep_internal::process_completion_pipe (this, pipe_);
        scoped_lock_t lock (_transport_pairs_sync);
        transport_pairs_t::iterator it = _transport_pairs.find (pair_key);
        if (it != _transport_pairs.end ())
            it->second.draining = false;
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
    xread_activated (pipe_);
}

void zlink::socket_base_t::write_activated (pipe_t *pipe_)
{
    const bool completion =
      pipe_ && pipe_->get_transport_pair_id () != 0
      && pipe_->get_transport_lane () == transport_lane_completion;
    if (!completion)
        xwrite_activated (pipe_);
    if (dispatch_runtime ().send_recovery_pending ()
        && !dispatch_runtime ().send_recovery_ready ()) {
        dispatch_runtime ().mark_send_recovery_ready ();
        static_cast<mailbox_t *> (_mailbox)->signal ();
    }
    notify_send_ready_if_armed ();
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
    if (pair_id != 0) {
        scoped_lock_t lock (_transport_pairs_sync);
        transport_pairs_t::iterator pair_it =
          _transport_pairs.find (transport_pair_key_t (
            pair_id, pipe_->get_transport_pair_generation ()));
        if (pair_it != _transport_pairs.end ()) {
            application_attached = pair_it->second.application_attached;
            paired_pipe = completion ? pair_it->second.application : pair_it->second.completion;
            if (completion)
                pair_it->second.completion = NULL;
            else
                pair_it->second.application = NULL;
            if (!pair_it->second.application && !pair_it->second.completion)
                _transport_pairs.erase (pair_it);
        }
    }

    if (!completion && application_attached)
        xpipe_terminated (pipe_);
    if (!completion)
        socket_reqrep_internal::forget_router_reply_targets_for_pipe (
          request_reply_state (), pipe_);
    endpoint_runtime ().inprocs.erase_pipe (pipe_);

    uint32_t ready_count = 0;
    bool ready_changed = false;
    {
        scoped_lock_t lock (monitor_runtime ().sync);
        endpoint_runtime ().detach_pipe (pipe_);
        ready_changed = monitor_runtime ().erase_ready_connection (endpoint_pair, routing_id_data,
                                                                   routing_id_size, &ready_count);
    }
    if (ready_changed) {
        uint64_t values[1] = {ready_count};
        event (endpoint_pair, routing_id_data, routing_id_size, values, 1,
               ZLINK_EVENT_CONNECTION_READY);
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
    if (paired_pipe && !is_terminating ()) {
        paired_pipe->terminate (false);
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
    return it == _transport_pairs.end () || !it->second.ready
             ? NULL
             : it->second.completion;
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
    return it == _transport_pairs.end () || !it->second.ready
             || it->second.completion != completion_pipe_
             ? NULL
             : it->second.application;
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
    return selected;
}

int zlink::socket_base_t::socket_id () const
{
    return options.socket_id;
}

bool zlink::socket_base_t::is_ctx_terminated () const
{
    return _ctx_terminated.load (std::memory_order_acquire);
}
