/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/router/router.hpp"
#include "sockets/router/router_debug.hpp"

#include "core/c_api_copy_internal.hpp"
#include "core/pipe.hpp"
#include "protocol/wire.hpp"
#include "utils/routing_id.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace zlink
{
int router_t::xselect_routed_submit_target (
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_)
{
    std::lock_guard<std::mutex> route_lifecycle_lock (
      _out_pipes_sync);
    return select_routed_submit_target_locked (
      router_rid_or_null_, target_out_, NULL, NULL, false);
}

int router_t::xselect_routed_submit_target_internal (
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_,
  uint64_t *transport_connection_id_out_,
  uint64_t *route_incarnation_id_out_)
{
    std::lock_guard<std::mutex> route_lifecycle_lock (
      _out_pipes_sync);
    return select_routed_submit_target_locked (
      router_rid_or_null_, target_out_, transport_connection_id_out_,
      route_incarnation_id_out_, true);
}

int router_t::xselect_request_submit_target (
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_,
  uint64_t *transport_connection_id_out_,
  uint64_t *route_incarnation_id_out_,
  std::string *logical_endpoint_out_)
{
    if (logical_endpoint_out_)
        logical_endpoint_out_->clear ();
    std::lock_guard<std::mutex> route_lifecycle_lock (_out_pipes_sync);
    const int rc = select_routed_submit_target_locked (
      router_rid_or_null_, target_out_, transport_connection_id_out_,
      route_incarnation_id_out_, true);
    if (rc != 0) {
        // Ordinary routed DATA treats an absent or currently unavailable route
        // as a connection failure.  REQUEST has a narrower public contract:
        // a RID that is not present in the routing map is a missing target.
        // Preserve EHOSTUNREACH for an existing-but-unavailable route while
        // normalizing only the absent-map case to ENOENT.
        if (errno == EHOSTUNREACH
            && valid_routing_id (router_rid_or_null_)) {
            const blob_t routing_id (
              const_cast<unsigned char *> (router_rid_or_null_->data),
              router_rid_or_null_->size, reference_tag_t ());
            const out_pipe_t *const out_pipe = lookup_out_pipe (routing_id);
            if (!out_pipe || !out_pipe->pipe)
                errno = ENOENT;
        }
        return -1;
    }

    const blob_t routing_id (
      const_cast<unsigned char *> (router_rid_or_null_->data),
      router_rid_or_null_->size, reference_tag_t ());
    const out_pipe_t *const out_pipe = lookup_out_pipe (routing_id);
    if (!out_pipe || !out_pipe->pipe
        || out_pipe->pipe->get_peer_socket_type ()
             != ZLINK_CORE_SOCKET_ROUTER) {
        errno = EPROTOTYPE;
        return -1;
    }
    return 0;
}

int router_t::select_routed_submit_target_locked (
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_,
  uint64_t *transport_connection_id_out_,
  uint64_t *route_incarnation_id_out_, bool allow_unpaired_) const
{
    if (transport_connection_id_out_)
        *transport_connection_id_out_ = 0;
    if (route_incarnation_id_out_)
        *route_incarnation_id_out_ = 0;
    if (!valid_routing_id (router_rid_or_null_) || !target_out_) {
        errno = EINVAL;
        return -1;
    }

    const blob_t routing_id (
      const_cast<unsigned char *> (router_rid_or_null_->data),
      router_rid_or_null_->size, reference_tag_t ());
    const out_pipe_t *out_pipe = lookup_out_pipe (routing_id);
    if (!out_pipe || !out_pipe->pipe) {
        errno = EHOSTUNREACH;
        return -1;
    }
    if (out_pipe->weight == 0) {
        errno = ECONNREFUSED;
        return -1;
    }

    pipe_t *const pipe = out_pipe->pipe;
    const uint64_t pair_id = pipe->get_transport_pair_id ();
    const uint64_t pair_generation =
      pipe->get_transport_pair_generation ();
    if (pair_id != 0) {
        if (pair_generation == 0
            || !transport_pair_application_ready (pipe)) {
            errno = EHOSTUNREACH;
            return -1;
        }
    } else {
        const uint64_t connection_id =
          pipe->get_transport_connection_id ();
        if (!allow_unpaired_ || !transport_connection_id_out_
            || !route_incarnation_id_out_
            || connection_id == 0 || !pipe->is_lifecycle_active ()) {
            errno = EHOSTUNREACH;
            return -1;
        }
        *transport_connection_id_out_ = connection_id;
        *route_incarnation_id_out_ = pipe->get_route_incarnation_id ();
    }

    copy_routing_id_from_bytes (router_rid_or_null_->data,
                                router_rid_or_null_->size,
                                &target_out_->peer_rid);
    target_out_->transport_pair_id = pair_id;
    target_out_->transport_pair_generation = pair_generation;
    return 0;
}

bool router_t::xsend_pending_target_current_locked (
  const routed_send_target_key_t &target_) const
{
    if (target_.peer_rid.empty ())
        return false;
    const blob_t routing_id (
      const_cast<unsigned char *> (
        reinterpret_cast<const unsigned char *> (target_.peer_rid.data ())),
      target_.peer_rid.size (), reference_tag_t ());
    const out_pipe_t *const current = lookup_out_pipe (routing_id);
    if (!current || !current->pipe)
        return false;

    pipe_t *const pipe = current->pipe;
    if (target_.transport_pair_id != 0
        || target_.transport_pair_generation != 0) {
        return target_.route_incarnation_id == 0
               && pipe->get_transport_pair_id ()
                    == target_.transport_pair_id
               && pipe->get_transport_pair_generation ()
                    == target_.transport_pair_generation;
    }
    return target_.route_incarnation_id != 0
           && pipe->get_transport_pair_id () == 0
           && pipe->get_transport_pair_generation () == 0
           && pipe->get_route_incarnation_id ()
                == target_.route_incarnation_id;
}

pipe_t *router_t::find_transport_pair_pipe (
  const zlink_routing_id_t *target_rid_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_) const
{
    if (!target_rid_ || transport_pair_id_ == 0 || transport_pair_generation_ == 0)
        return NULL;

    const blob_t target_rid (const_cast<unsigned char *> (target_rid_->data),
                             target_rid_->size, reference_tag_t ());
    const out_pipe_t *current = lookup_out_pipe (target_rid);
    if (current && current->pipe
        && current->pipe->get_transport_lane () == transport_lane_application
        && current->pipe->get_transport_pair_id () == transport_pair_id_
        && current->pipe->get_transport_pair_generation () == transport_pair_generation_) {
        if (router_debug::enabled ())
            fprintf (stderr, "router pair lookup: current pipe=%p pair=%llu/%llu\\n",
                     static_cast<void *> (current->pipe),
                     static_cast<unsigned long long> (transport_pair_id_),
                     static_cast<unsigned long long> (transport_pair_generation_));
        return current->pipe;
    }
    // Exact targets are capabilities for the current RID binding. A handover
    // keeps the superseded pipe in _standby_pipes only so it can be promoted
    // if the winner terminates; accepting new work into that pipe can report
    // success after its transport has already disconnected. Once another
    // pair owns the RID, callers must observe the old capability as stale and
    // select the replacement target instead.
    return NULL;
}

pipe_t *router_t::retain_current_transport_pair_pipe (
  const zlink_routing_id_t *peer_rid_, int peer_socket_type_,
  transport_lane_t lane_) const
{
    if (!valid_routing_id (peer_rid_)
        || (lane_ != transport_lane_application
            && lane_ != transport_lane_completion))
        return NULL;

    const blob_t peer_rid (
      const_cast<unsigned char *> (peer_rid_->data), peer_rid_->size,
      reference_tag_t ());
    std::lock_guard<std::mutex> route_lifecycle_lock (_out_pipes_sync);
    const out_pipe_t *const current = lookup_out_pipe (peer_rid);
    pipe_t *const application = current ? current->pipe : NULL;
    if (!application
        || application->get_transport_lane () != transport_lane_application
        || application->get_peer_socket_type () != peer_socket_type_
        || application->get_transport_pair_id () == 0
        || application->get_transport_pair_generation () == 0)
        return NULL;

    pipe_t *const selected = socket_base_t::retain_transport_pair_pipe (
      application->get_transport_pair_id (),
      application->get_transport_pair_generation (), lane_);
    if (!selected)
        return NULL;

    const bool valid =
      selected->get_peer_socket_type () == peer_socket_type_
      && selected->get_transport_connection_id () != 0;
    if (!valid) {
        selected->release_lifetime_ref ();
        return NULL;
    }
    return selected;
}

bool router_t::emit_transport_pair_ready (pipe_t *pipe_)
{
    endpoint_uri_pair_t endpoint_pair;
    blob_t public_routing_id;
    uint64_t pair_id = 0;
    uint64_t pair_generation = 0;
    {
        std::lock_guard<std::mutex> route_lifecycle_lock (
          _out_pipes_sync);
        if (!pipe_ || pipe_->get_transport_pair_id () == 0
            || pipe_->get_transport_lane () != transport_lane_application
            // Every caller, including the session-side endpoint refresh,
            // shares this data-plane gate. Pair-table admission alone is not
            // readiness: route adoption and write release must both be done.
            || !transport_pair_application_ready (pipe_)
            || !pipe_->transport_pair_writes_released ())
            return false;

        const blob_t *routing_id = NULL;
        const blob_t &pipe_routing_id = pipe_->get_routing_id ();
        const out_pipe_t *current =
          pipe_routing_id.size () > 0 ? lookup_out_pipe (pipe_routing_id)
                                      : NULL;
        if (current && current->pipe == pipe_)
            routing_id = &pipe_routing_id;
        else {
            const std::map<pipe_t *, blob_t>::const_iterator standby =
              _standby_pipes.find (pipe_);
            if (standby != _standby_pipes.end ()
                && standby->second.size () > 0)
                routing_id = &standby->second;
        }
        if (!routing_id)
            return false;

        public_routing_id =
          blob_t (routing_id->data (), routing_id->size ());
        endpoint_pair = pipe_->get_endpoint_pair ();
        endpoint_pair.connection_id =
          pipe_->get_transport_connection_id ();
        pair_id = pipe_->get_transport_pair_id ();
        pair_generation = pipe_->get_transport_pair_generation ();
    }
    event_connection_ready_changed (
      endpoint_pair, public_routing_id.data (), public_routing_id.size (),
      transport_lane_application, pair_id, pair_generation);
    return true;
}

bool router_t::identify_peer (pipe_t *pipe_, bool locally_initiated_,
                              route_adoption_actions_t *actions_,
                              uint32_t initial_weight_)
{
    msg_t msg;
    blob_t routing_id;

    if (locally_initiated_ && connect_routing_id_is_set ()) {
        const std::string connect_routing_id = extract_connect_routing_id ();
        routing_id.set (reinterpret_cast<const unsigned char *> (connect_routing_id.c_str ()),
                        connect_routing_id.length ());
    } else if (locally_initiated_ && pipe_->get_routing_id ().size () != 0) {
        //  A reconnect has already consumed the one-shot connect routing ID.
        //  The new Application pipe retains that ID so the replacement route
        //  uses the same public identity instead of waiting for an identity
        //  frame that paired transports do not send.
        const blob_t &retained_routing_id = pipe_->get_routing_id ();
        routing_id.set (retained_routing_id.data (), retained_routing_id.size ());
    } else {
        msg.init ();
        const bool ok = pipe_->read (&msg);
        if (!ok)
            return false;
        if (msg.size () == 0) {
            unsigned char buf[5];
            buf[0] = 0;
            put_uint32 (buf + 1, _next_integral_routing_id++);
            routing_id.set (buf, sizeof buf);
            msg.close ();
        } else {
            routing_id.set (static_cast<unsigned char *> (msg.data ()), msg.size ());
            msg.close ();
        }
    }

    return adopt_peer_routing_id (pipe_, ZLINK_MOVE (routing_id),
                                  locally_initiated_, actions_,
                                  initial_weight_);
}

bool router_t::duplicate_pipe_should_replace (const out_pipe_t &existing_outpipe_,
                                              const blob_t &routing_id_,
                                              bool locally_initiated_) const
{
    if (!existing_outpipe_.active)
        return true;

    // A reconnect created by the same side supersedes the older route. The
    // reconnect carries the current Framework transport generation; retaining
    // the older current route would make readiness select a pipe that is no
    // longer connected to the replacement process.
    if (existing_outpipe_.locally_initiated == locally_initiated_)
        return true;

    // When both peers connect to each other, both physical directions can
    // arrive with the same routing id. Pick one direction from the two stable
    // routing ids so both peers make the same decision and reconnects converge
    // instead of continuously handing over to each other.
    const size_t local_size = options.routing_id_size;
    const size_t peer_size = routing_id_.size ();
    const size_t common_size = std::min (local_size, peer_size);
    int cmp = 0;
    if (common_size > 0)
        cmp = std::memcmp (options.routing_id, routing_id_.data (), common_size);
    if (cmp == 0) {
        if (local_size < peer_size)
            cmp = -1;
        else if (local_size > peer_size)
            cmp = 1;
    }

    const bool locally_initiated_pipe_wins = cmp < 0;
    return locally_initiated_ == locally_initiated_pipe_wins;
}

bool router_t::adopt_peer_routing_id (pipe_t *pipe_, blob_t routing_id_,
                                      bool locally_initiated_,
                                      route_adoption_actions_t *actions_,
                                      uint32_t initial_weight_)
{
    const out_pipe_t *const existing_outpipe = lookup_out_pipe (routing_id_);
    if (existing_outpipe) {
        //  A reconnect from the same locally initiated endpoint replaces its
        //  previous generation even when general ROUTER handover is disabled.
        //  Otherwise the old pipe can keep the routing ID until its terminate
        //  command is processed and the new generation remains anonymous.
        const std::string &new_endpoint =
          pipe_->get_endpoint_pair ().identifier ();
        const std::string &existing_endpoint =
          existing_outpipe->pipe->get_endpoint_pair ().identifier ();
        const bool same_local_endpoint_reconnect =
          locally_initiated_ && existing_outpipe->locally_initiated
          && !new_endpoint.empty () && new_endpoint == existing_endpoint;
        const bool paired_application =
          pipe_->get_transport_pair_id () != 0
          && pipe_->get_transport_lane () == transport_lane_application;
        const bool reciprocal_duplicate =
          existing_outpipe->locally_initiated != locally_initiated_;
        if (!_handover && !same_local_endpoint_reconnect
            && !(paired_application && reciprocal_duplicate))
            return false;

        if (!duplicate_pipe_should_replace (*existing_outpipe, routing_id_, locally_initiated_)) {
            unsigned char buf[5];
            buf[0] = 0;
            put_uint32 (buf + 1, _next_integral_routing_id++);
            blob_t standby_routing_id (buf, sizeof buf);
            blob_t original_routing_id (
              routing_id_.data (), routing_id_.size ());
            pipe_->invalidate_router_route_binding ();
            pipe_->set_router_socket_routing_id (
              standby_routing_id);
            add_out_pipe (
              ZLINK_MOVE (standby_routing_id), pipe_,
              locally_initiated_, initial_weight_);
            _standby_pipes.ZLINK_MAP_INSERT_OR_EMPLACE (
              pipe_, ZLINK_MOVE (original_routing_id));
            return true;
        }

        if (router_debug::enabled ()) {
            char rid_text[160];
            router_debug::format_routing_id (routing_id_, rid_text,
                                             sizeof (rid_text));
            fprintf (stderr,
                     "router identify_peer: replace duplicate rid=%s existing_local=%d "
                     "new_local=%d\n",
                     rid_text, existing_outpipe->locally_initiated ? 1 : 0,
                     locally_initiated_ ? 1 : 0);
        }

        unsigned char buf[5];
        buf[0] = 0;
        put_uint32 (buf + 1, _next_integral_routing_id++);
        blob_t new_routing_id (buf, sizeof buf);

        pipe_t *const old_pipe = existing_outpipe->pipe;
        const bool old_locally_initiated = existing_outpipe->locally_initiated;
        const uint32_t old_peer_weight = existing_outpipe->weight;
        old_pipe->invalidate_router_route_binding ();
        erase_out_pipe (old_pipe);
        old_pipe->set_router_socket_routing_id (new_routing_id);
        add_out_pipe (ZLINK_MOVE (new_routing_id), old_pipe, old_locally_initiated);
        out_pipe_t *const demoted =
          lookup_out_pipe (old_pipe->get_routing_id ());
        zlink_assert (demoted && demoted->pipe == old_pipe);
        update_out_pipe_weight (demoted, old_peer_weight);
        const bool paired_same_direction_reconnect =
          _handover && paired_application
          && old_locally_initiated == locally_initiated_;
        if (paired_same_direction_reconnect || reciprocal_duplicate) {
            blob_t original_routing_id (
              routing_id_.data (), routing_id_.size ());
            _standby_pipes.ZLINK_MAP_INSERT_OR_EMPLACE (
              old_pipe, ZLINK_MOVE (original_routing_id));
        } else if (old_pipe == _current_in) {
            _terminate_current_in = true;
        } else if (actions_ && old_pipe->retain_lifetime_ref ())
            actions_->terminate_pipe = old_pipe;
    }

    pipe_->invalidate_router_route_binding ();
    pipe_->set_router_socket_routing_id (routing_id_);
    add_out_pipe (ZLINK_MOVE (routing_id_), pipe_, locally_initiated_,
                  initial_weight_);
    pipe_->publish_router_route_binding ();
    if (actions_)
        actions_->cache_completion = true;
    if (router_debug::enabled ()) {
        char rid_text[160];
        router_debug::format_routing_id (
          pipe_->get_routing_id (), rid_text, sizeof (rid_text));
        fprintf (stderr, "router identify_peer: add out pipe rid=%s\n", rid_text);
    }
    return true;
}

void router_t::finish_route_adoption (pipe_t *adopted_pipe_,
                                      route_adoption_actions_t *actions_)
{
    if (!actions_)
        return;
    if (actions_->terminate_pipe) {
        actions_->terminate_pipe->terminate (true);
        actions_->terminate_pipe->release_lifetime_ref ();
        actions_->terminate_pipe = NULL;
    }
    if (actions_->cache_completion) {
        cache_completion_pipe_routing_id (adopted_pipe_);
        actions_->cache_completion = false;
        // RID adoption can happen directly for an inproc attach as well as
        // from an activate-read command. A reply waiting on a temporarily
        // absent logical route must observe either transition immediately.
        notify_submit_progress ();
    }
}

int router_t::apply_peer_weight (pipe_t *pipe_, uint32_t weight_)
{
    if (!pipe_)
        return 1;

    blob_t public_routing_id;
    {
        std::lock_guard<std::mutex> route_lifecycle_lock (
          _out_pipes_sync);
        const blob_t &routing_id = pipe_->get_routing_id ();
        out_pipe_t *out_pipe = lookup_out_pipe (routing_id);
        if (!out_pipe || out_pipe->pipe != pipe_
            || out_pipe->weight == weight_)
            return 1;

        update_out_pipe_weight (out_pipe, weight_);
        const std::map<pipe_t *, blob_t>::const_iterator standby =
          _standby_pipes.find (pipe_);
        const blob_t &event_routing_id =
          standby != _standby_pipes.end () ? standby->second : routing_id;
        public_routing_id = blob_t (
          event_routing_id.data (), event_routing_id.size ());
    }
    notify_send_pending_writable (pipe_);
    // Monitor enqueueing has its own synchronization. Never call it while the
    // I/O/owner route-lifecycle fence is held.
    emit_peer_weight_changed (pipe_, weight_, &public_routing_id);
    return 1;
}

void router_t::initialize_peer_weight (pipe_t *pipe_, uint32_t weight_)
{
    if (!pipe_)
        return;
    std::lock_guard<std::mutex> route_lifecycle_lock (_out_pipes_sync);
    const blob_t &routing_id = pipe_->get_routing_id ();
    out_pipe_t *const out_pipe = lookup_out_pipe (routing_id);
    if (out_pipe && out_pipe->pipe == pipe_ && out_pipe->weight != weight_)
        update_out_pipe_weight (out_pipe, weight_);
}

#ifdef ZLINK_BUILD_TESTS
uint32_t router_t::test_peer_weight (pipe_t *pipe_) const
{
    if (!pipe_)
        return 0;
    std::lock_guard<std::mutex> route_lifecycle_lock (
      _out_pipes_sync);
    const out_pipe_t *const out_pipe = lookup_out_pipe (pipe_->get_routing_id ());
    return out_pipe && out_pipe->pipe == pipe_ ? out_pipe->weight : 0;
}
#endif
}
