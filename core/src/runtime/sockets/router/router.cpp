/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/macros.hpp"
#include "sockets/router/router.hpp"
#include "sockets/router/router_debug.hpp"
#include "core/pipe.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/mailbox.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "utils/random.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"
#include <cstdio>

const bool zlink::router_debug::enabled_flag =
  zlink::debug_env_enabled ("ZLINK_ROUTER_DEBUG");

namespace
{
const int router_transport_write_batch_size = 16 * 1024;
}

zlink::router_t::router_t (class ctx_t *parent_, uint32_t tid_, int sid_) :
    routing_socket_base_t (parent_, tid_, sid_),
    _fq (fq_t::publish_receive_activity),
    _prefetched (false),
    _routing_id_sent (false),
    _current_in (NULL),
    _terminate_current_in (false),
    _more_in (false),
    _current_out (NULL),
    _current_out_connection_id (0),
    _more_out (false),
    _next_integral_routing_id (generate_random ()),
    _mandatory (true),
    _probe_router (false),
    _handover (options.rid_duplicate_policy == ZLINK_RID_DUPLICATE_HANDOVER),
    _route_revision (0),
    _observed_route_revision (0),
    _next_route_generation (1),
    _last_recv_route_generation (0)
{
    options.type = ZLINK_CORE_SOCKET_ROUTER;
    options.out_batch_size = router_transport_write_batch_size;
    options.recv_routing_id = true;
    options.can_send_hello_msg = true;
    options.can_recv_disconnect_msg = true;

    _prefetched_id.init ();
    _prefetched_msg.init ();
}

uint64_t zlink::router_t::next_route_generation ()
{
    const uint64_t value = _next_route_generation;
    _next_route_generation += 2;
    return value;
}

void zlink::router_t::publish_route_change (
  const blob_t &routing_id_, route_discard_batch_t *discarded_)
{
    // Every selected-route transition discards records already queued on all
    // standbys for this RID. A later promotion must start with a clean pipe.
    for (std::map<pipe_t *, blob_t>::const_iterator standby =
           _standby_pipes.begin ();
         standby != _standby_pipes.end (); ++standby) {
        if (!(standby->second < routing_id_)
            && !(routing_id_ < standby->second))
            discard_route_records (standby->first, discarded_);
    }
    // An ended route keeps its records, including one staged in the part
    // helper, until another pipe is selected for its RID.
    if (lookup_out_pipe (routing_id_)) {
        const std::map<blob_t, pipe_t *>::iterator ended =
          _ended_routes.find (routing_id_);
        if (ended != _ended_routes.end ()) {
            discard_route_records (ended->second, discarded_);
            _ended_routes.erase (ended);
        }
        discard_staged_record (routing_id_, discarded_);
    }
    _route_revision.fetch_add (1, std::memory_order_release);
    static_cast<mailbox_t *> (get_mailbox ())->signal ();
}

bool zlink::router_t::has_route_change () const
{
    const uint64_t observed =
      _observed_route_revision.load (std::memory_order_acquire);
    const uint64_t published =
      _route_revision.load (std::memory_order_acquire);
    return published != observed;
}

int zlink::router_t::routes_snapshot (zlink_router_route_t *routes_,
                                      size_t capacity_, size_t *count_)
{
    std::lock_guard<std::mutex> lock (_out_pipes_sync);
    size_t count = 0;
    for (out_pipes_t::const_iterator it = _out_pipes.begin ();
         it != _out_pipes.end (); ++it)
        if (is_selected_pipe (it->second.pipe))
            ++count;
    *count_ = count;
    if (capacity_ < count) {
        errno = ENOBUFS;
        return -1;
    }
    size_t index = 0;
    for (out_pipes_t::const_iterator it = _out_pipes.begin ();
         it != _out_pipes.end (); ++it) {
        if (!is_selected_pipe (it->second.pipe))
            continue;
        const uint64_t generation =
          it->second.pipe->router_route_binding_token ();
        zlink_router_route_t &route = routes_[index++];
        copy_routing_id_from_bytes (it->first.data (), it->first.size (),
                                    &route.rid);
        route.route_generation = generation;
    }
    _observed_route_revision.store (
      _route_revision.load (std::memory_order_acquire),
      std::memory_order_release);
    errno = 0;
    return 0;
}

bool zlink::router_t::is_selected_pipe (pipe_t *pipe_,
                                        uint64_t generation_,
                                        uint64_t *observed_generation_out_) const
{
    const uint64_t token = pipe_ ? pipe_->router_route_binding_token () : 0;
    if (observed_generation_out_)
        *observed_generation_out_ = token;
    return token != 0 && (generation_ == 0 || token == generation_);
}

void zlink::router_t::discard_route_records (
  pipe_t *pipe_, route_discard_batch_t *discarded_)
{
    // The route mutex is held by the caller. A selected or ended route loses
    // its token before queued or prefetched records can be read again.
    const uint64_t generation = pipe_->router_route_binding_token ();
    if (generation != 0)
        pipe_->invalidate_router_route_binding ();
    zlink_assert (!is_selected_pipe (pipe_));
    zlink_assert (discarded_);
    route_discard_batch_t::followup_t followup = {pipe_, false, false};
    // Allocate this slot before consuming frames; the discard batch itself
    // closes already detached handles if a later cold allocation fails.
    discarded_->followups.push_back (followup);
    route_discard_batch_t::followup_t &pending =
      discarded_->followups.back ();
    _fq.discard_pending_records (pipe_, &discarded_->messages,
                                 &pending.delimiter, &pending.recheck);
    if (_current_in == pipe_) {
        if (_prefetched) {
            discarded_->messages.emplace_back ();
            int rc = discarded_->messages.back ().init ();
            errno_assert (rc == 0);
            rc = discarded_->messages.back ().move (_prefetched_msg);
            errno_assert (rc == 0);
            discarded_->messages.emplace_back ();
            rc = discarded_->messages.back ().init ();
            errno_assert (rc == 0);
            rc = discarded_->messages.back ().move (_prefetched_id);
            errno_assert (rc == 0);
            _prefetched = false;
        }
        reset_current_in_after_multipart_abort ();
    }
}

void zlink::router_t::discard_staged_record (const blob_t &routing_id_,
                                             route_discard_batch_t *discarded_)
{
    // The route mutex is held by the caller, which has just selected a new
    // pipe for this RID. A staged record from this RID therefore came from a
    // pipe that is no longer selected, even when that pipe has already been
    // detached from the fair queue.
    const std::shared_ptr<part_helper_internal::handle_state_t> state =
      part_helper_state ();
    if (!state)
        return;
    std::lock_guard<std::mutex> lock (state->mutex);
    const zlink_routing_id_t &source = state->recv.source_node_rid;
    if (!state->recv.active
        || state->recv.family != part_helper_internal::recv_family_router
        || source.size != routing_id_.size ()
        || memcmp (source.data, routing_id_.data (), source.size) != 0)
        return;
    zlink_assert (!discarded_->staged_hold_socket
                  && !discarded_->staged_route_source_pipe
                  && discarded_->staged_reply_token == 0);
    if (state->recv.request_seq != 0) {
        discarded_->staged_reply_token = state->recv.request_seq;
        discarded_->staged_reply_rid = state->recv.source_node_rid;
    }
    discarded_->staged_route_source_pipe = state->recv.route_source_pipe;
    state->recv.route_source_pipe = NULL;
    discarded_->staged_parts.take_from (&state->recv.buffered_parts);
    discarded_->staged_hold_socket =
      part_helper_internal::reset_recv_sequence (&state->recv);
}

zlink::router_t::route_discard_batch_t::~route_discard_batch_t ()
{
    close_messages ();
}

void zlink::router_t::route_discard_batch_t::close_messages ()
{
    for (std::deque<msg_t>::iterator it = messages.begin ();
         it != messages.end (); ++it) {
        const int rc = it->close ();
        errno_assert (rc == 0);
    }
    messages.clear ();
    for (size_t i = 0; i < staged_parts.size (); ++i) {
        const int rc = zlink_msg_close (&staged_parts[i]);
        errno_assert (rc == 0);
    }
    staged_parts.clear ();
}

void zlink::router_t::finish_route_discard (route_discard_batch_t *batch_)
{
    for (std::vector<route_discard_batch_t::followup_t>::iterator it =
           batch_->followups.begin ();
         it != batch_->followups.end (); ++it) {
        if (it->delimiter)
            it->pipe->complete_route_discard_delimiter ();
        if (it->recheck && it->pipe->check_read ())
            _fq.activated (it->pipe);
    }
    batch_->followups.clear ();
    batch_->close_messages ();
    if (batch_->staged_hold_socket) {
        batch_->staged_hold_socket->end_public_part_receive_delivery_hold ();
        batch_->staged_hold_socket = NULL;
    }
    if (batch_->staged_route_source_pipe) {
        batch_->staged_route_source_pipe->release_lifetime_ref ();
        batch_->staged_route_source_pipe = NULL;
    }
    if (batch_->staged_reply_token != 0) {
        socket_reqrep_internal::revoke_router_reply_target (
          make_socket_handle (this), &batch_->staged_reply_rid,
          batch_->staged_reply_token);
        batch_->staged_reply_token = 0;
    }
}

uint64_t zlink::router_t::last_recv_route_generation () const
{
    return _last_recv_route_generation.load (std::memory_order_acquire);
}

void zlink::router_t::set_last_recv_route_generation (uint64_t generation_)
{
    _last_recv_route_generation.store (generation_, std::memory_order_release);
}

zlink::router_t::~router_t ()
{
    zlink_assert (_anonymous_pipes.empty ());
    _prefetched_id.close ();
    _prefetched_msg.close ();
}

int zlink::router_t::xsetsockopt (int option_, const void *optval_, size_t optvallen_)
{
    const bool is_int = (optvallen_ == sizeof (int));
    int value = 0;
    if (is_int)
        memcpy (&value, optval_, sizeof (int));

    switch (option_) {
        case ZLINK_INTERNAL_OPT_ROUTER_MANDATORY:
            if (is_int && value >= 0) {
                std::lock_guard<std::mutex> route_lifecycle_lock (
                  _out_pipes_sync);
                _mandatory = (value != 0);
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_PROBE_ROUTER:
            if (is_int && value >= 0) {
                std::lock_guard<std::mutex> route_lifecycle_lock (
                  _out_pipes_sync);
                _probe_router = (value != 0);
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_ROUTER_HANDOVER:
        case ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY:
            if (is_int && value >= 0) {
                if (option_ == ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY
                    && value != ZLINK_RID_DUPLICATE_REJECT && value != ZLINK_RID_DUPLICATE_HANDOVER)
                    break;
                std::lock_guard<std::mutex> route_lifecycle_lock (
                  _out_pipes_sync);
                _handover = option_ == ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY
                              ? value == ZLINK_RID_DUPLICATE_HANDOVER
                              : value != 0;
                return 0;
            }
            break;

        default:
            return routing_socket_base_t::xsetsockopt (option_, optval_, optvallen_);
    }
    errno = EINVAL;
    return -1;
}

int zlink::router_t::xgetsockopt (int option_, void *optval_, size_t *optvallen_)
{
    if (!optval_ || !optvallen_ || *optvallen_ != sizeof (int)) {
        errno = EINVAL;
        return -1;
    }

    int *value = static_cast<int *> (optval_);
    switch (option_) {
        case ZLINK_INTERNAL_OPT_ROUTER_MANDATORY:
        {
            std::lock_guard<std::mutex> route_lifecycle_lock (
              _out_pipes_sync);
            *value = _mandatory ? 1 : 0;
            return 0;
        }
        case ZLINK_INTERNAL_OPT_PROBE_ROUTER:
        {
            std::lock_guard<std::mutex> route_lifecycle_lock (
              _out_pipes_sync);
            *value = _probe_router ? 1 : 0;
            return 0;
        }
        case ZLINK_INTERNAL_OPT_ROUTER_HANDOVER:
        {
            std::lock_guard<std::mutex> route_lifecycle_lock (
              _out_pipes_sync);
            *value = _handover ? 1 : 0;
            return 0;
        }
        case ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY:
        {
            std::lock_guard<std::mutex> route_lifecycle_lock (
              _out_pipes_sync);
            *value = _handover ? ZLINK_RID_DUPLICATE_HANDOVER : ZLINK_RID_DUPLICATE_REJECT;
            return 0;
        }
        default:
            return routing_socket_base_t::xgetsockopt (option_, optval_, optvallen_);
    }
}


void zlink::router_t::xpipe_terminated (pipe_t *pipe_)
{
    route_discard_batch_t discarded;
    {
        std::lock_guard<std::mutex> route_lifecycle_lock (_out_pipes_sync);
        // Receive-side ownership is released before socket-message teardown
        // takes its dispatch fence. Keep FQ/current-record state in this phase.
        if (pipe_ == _current_in) {
            // A prefetched frame still belongs to the terminating pipe. It
            // must not be presented with metadata from the next active pipe.
            if (_prefetched && !_routing_id_sent) {
                discarded.messages.emplace_back ();
                int rc = discarded.messages.back ().init ();
                errno_assert (rc == 0);
                rc = discarded.messages.back ().move (_prefetched_id);
                errno_assert (rc == 0);
                discarded.messages.emplace_back ();
                rc = discarded.messages.back ().init ();
                errno_assert (rc == 0);
                rc = discarded.messages.back ().move (_prefetched_msg);
                errno_assert (rc == 0);
                _prefetched = false;
            }
            if (!_prefetched)
                _routing_id_sent = false;
            _current_in = NULL;
            _terminate_current_in = false;
            _more_in = false;
        }
        const std::map<blob_t, pipe_t *>::iterator ended =
          _ended_routes.find (pipe_->get_routing_id ());
        if (ended != _ended_routes.end () && ended->second == pipe_)
            _ended_routes.erase (ended);
        _fq.pipe_terminated (pipe_);
    }
    finish_route_discard (&discarded);
}

void zlink::router_t::xsocket_msg_pipe_terminated (pipe_t *pipe_)
{
    bool rollback_outbound = false;
    pipe_t *promoted_writable_pipe = NULL;
    route_discard_batch_t discarded;
    {
        std::lock_guard<std::mutex> route_lifecycle_lock (
          _out_pipes_sync);
        // Direct network dispatch can register an anonymous ROUTER route on a
        // session thread. Route teardown therefore shares this post-receive
        // dispatch fence as well; otherwise termination can race that
        // registration or leave a late partial record keyed by a dead endpoint.
        const blob_t &terminated_routing_id = pipe_->get_routing_id ();
        const std::map<pipe_t *, blob_t>::iterator terminated_standby =
          _standby_pipes.find (pipe_);
        const bool was_standby = terminated_standby != _standby_pipes.end ();
        if (was_standby) {
            _standby_pipes.erase (terminated_standby);
        }

        pipe_t *standby_to_promote = NULL;
        blob_t standby_routing_id;
        if (!was_standby) {
            for (std::map<pipe_t *, blob_t>::iterator standby =
                   _standby_pipes.begin ();
                 standby != _standby_pipes.end (); ++standby) {
                const bool same_routing_id =
                  !(standby->second < terminated_routing_id)
                  && !(terminated_routing_id < standby->second);
                if (!same_routing_id)
                    continue;
                standby_to_promote = standby->first;
                standby_routing_id =
                  blob_t (standby->second.data (), standby->second.size ());
                _standby_pipes.erase (standby);
                break;
            }
        }

        if (router_debug::enabled ()) {
            char rid_text[160];
            router_debug::format_routing_id (
              pipe_->get_routing_id (), rid_text, sizeof (rid_text));
            fprintf (stderr,
                     "router xpipe_terminated: pipe=%p rid=%s anonymous=%d\n",
                     static_cast<void *> (pipe_), rid_text,
                     _anonymous_pipes.count (pipe_) != 0 ? 1 : 0);
        }
        const bool selected_route_lost = is_selected_pipe (pipe_);
        if (0 == _anonymous_pipes.erase (pipe_)) {
            // The end of a selected pipe is not a selection. Its records stay
            // receivable until a successor is selected for this RID.
            if (selected_route_lost && _fq.has_pipe (pipe_)) {
                const bool retained =
                  _ended_routes
                    .ZLINK_MAP_INSERT_OR_EMPLACE (
                      blob_t (terminated_routing_id.data (),
                              terminated_routing_id.size ()),
                      pipe_)
                    .second;
                zlink_assert (retained);
            }
            erase_out_pipe (pipe_);
            rollback_outbound = true;
            if (pipe_ == _current_out) {
                clear_current_out_pipe ();
                _more_out = false;
            }
        }

        if (standby_to_promote) {
            const out_pipe_t *const standby_out =
              lookup_out_pipe (standby_to_promote->get_routing_id ());
            zlink_assert (standby_out);
            const bool locally_initiated = standby_out->locally_initiated;
            const uint32_t peer_weight = standby_out->weight;
            zlink_assert (!is_selected_pipe (standby_to_promote));
            discard_route_records (standby_to_promote, &discarded);
            erase_out_pipe (standby_to_promote);
            standby_to_promote->set_router_socket_routing_id (
              standby_routing_id);
            add_out_pipe (ZLINK_MOVE (standby_routing_id),
                          standby_to_promote, locally_initiated);
            standby_to_promote->publish_router_route_binding (
              next_route_generation ());
            zlink_assert (is_selected_pipe (standby_to_promote));
            out_pipe_t *const promoted =
              lookup_out_pipe (standby_to_promote->get_routing_id ());
            zlink_assert (promoted && promoted->pipe == standby_to_promote);
            update_out_pipe_weight (promoted, peer_weight);
            if (standby_to_promote->retain_lifetime_ref ())
                promoted_writable_pipe = standby_to_promote;
        }
        if (selected_route_lost)
            publish_route_change (terminated_routing_id, &discarded);
    }
    finish_route_discard (&discarded);
    if (rollback_outbound)
        pipe_->rollback ();
    if (promoted_writable_pipe) {
        notify_send_writable (promoted_writable_pipe);
        promoted_writable_pipe->release_lifetime_ref ();
    }
}

int zlink::router_t::xrollback ()
{
    pipe_t *rollback_pipe = NULL;
    {
        std::lock_guard<std::mutex> route_lifecycle_lock (
          _out_pipes_sync);
        if (_current_out && _current_out->retain_lifetime_ref ())
            rollback_pipe = _current_out;
        clear_current_out_pipe ();
        _more_out = false;
    }
    if (rollback_pipe) {
        rollback_pipe->rollback ();
        rollback_pipe->release_lifetime_ref ();
    }
    return 0;
}
