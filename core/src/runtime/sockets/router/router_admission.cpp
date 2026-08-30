/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/router/router.hpp"

#include "core/c_api_copy_internal.hpp"
#include "core/pipe.hpp"
#include "protocol/wire.hpp"
#include "utils/debug_log.hpp"
#include "utils/routing_id.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
const bool router_debug_on = zlink::debug_env_enabled ("ZLINK_ROUTER_DEBUG");

void format_blob_routing_id_debug (const zlink::blob_t &routing_id_, char *buf_, size_t buf_size_)
{
    if (!buf_ || buf_size_ == 0)
        return;
    if (routing_id_.size () == 0) {
        std::snprintf (buf_, buf_size_, "<empty>");
        return;
    }

    size_t used = 0;
    for (size_t i = 0; i < routing_id_.size () && used + 4 < buf_size_; ++i) {
        const unsigned char c = routing_id_.data ()[i];
        const int rc = std::snprintf (buf_ + used, buf_size_ - used, "%c%02X",
                                      (c >= 32 && c <= 126) ? static_cast<char> (c) : '.',
                                      static_cast<unsigned> (c));
        if (rc <= 0)
            break;
        used += static_cast<size_t> (rc);
        if (i + 1 < routing_id_.size () && used + 2 < buf_size_) {
            buf_[used++] = ' ';
            buf_[used] = '\0';
        }
    }
}

bool router_debug_enabled ()
{
    return router_debug_on;
}
}

namespace zlink
{
int router_t::xselect_routed_submit_target (
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_)
{
    if (!valid_routing_id (router_rid_or_null_) || !target_out_) {
        errno = EINVAL;
        return -1;
    }

    const blob_t routing_id (
      const_cast<unsigned char *> (router_rid_or_null_->data),
      router_rid_or_null_->size, reference_tag_t ());
    const out_pipe_t *out_pipe = lookup_out_pipe (routing_id);
    if (!out_pipe || !out_pipe->pipe
        || !transport_pair_application_ready (out_pipe->pipe)) {
        errno = EHOSTUNREACH;
        return -1;
    }
    if (out_pipe->weight == 0) {
        errno = ECONNREFUSED;
        return -1;
    }

    copy_routing_id_from_bytes (router_rid_or_null_->data,
                                router_rid_or_null_->size,
                                &target_out_->peer_rid);
    target_out_->transport_pair_id =
      out_pipe->pipe->get_transport_pair_id ();
    target_out_->transport_pair_generation =
      out_pipe->pipe->get_transport_pair_generation ();
    return 0;
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
        if (router_debug_enabled ())
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

bool router_t::emit_transport_pair_ready (pipe_t *pipe_)
{
    if (!pipe_ || pipe_->get_transport_pair_id () == 0
        || pipe_->get_transport_lane () != transport_lane_application
        // Every caller, including the session-side endpoint refresh, shares
        // this data-plane gate. Pair-table admission alone is not readiness:
        // the route must still have had its transport write hold released.
        || !transport_pair_application_ready (pipe_)
        || !pipe_->transport_pair_writes_released ())
        return false;

    const blob_t *routing_id = NULL;
    const blob_t &pipe_routing_id = pipe_->get_routing_id ();
    const out_pipe_t *current =
      pipe_routing_id.size () > 0 ? lookup_out_pipe (pipe_routing_id) : NULL;
    if (current && current->pipe == pipe_)
        routing_id = &pipe_routing_id;
    else {
        const std::map<pipe_t *, blob_t>::const_iterator standby =
          _standby_pipes.find (pipe_);
        if (standby != _standby_pipes.end () && standby->second.size () > 0)
            routing_id = &standby->second;
    }
    if (!routing_id)
        return false;

    endpoint_uri_pair_t endpoint_pair = pipe_->get_endpoint_pair ();
    endpoint_pair.connection_id = pipe_->get_transport_connection_id ();
    event_connection_ready_changed (
      endpoint_pair, routing_id->data (), routing_id->size (),
      transport_lane_application, pipe_->get_transport_pair_id (),
      pipe_->get_transport_pair_generation ());
    return true;
}

bool router_t::identify_peer (pipe_t *pipe_, bool locally_initiated_)
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

    return adopt_peer_routing_id (pipe_, ZLINK_MOVE (routing_id), locally_initiated_);
}

bool router_t::duplicate_pipe_should_replace (const out_pipe_t &existing_outpipe_,
                                              const blob_t &routing_id_,
                                              bool locally_initiated_) const
{
    if (!existing_outpipe_.active || existing_outpipe_.weight == 0)
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

bool router_t::adopt_peer_routing_id (pipe_t *pipe_, blob_t routing_id_, bool locally_initiated_)
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
            pipe_->set_router_socket_routing_id (
              standby_routing_id);
            add_out_pipe (
              ZLINK_MOVE (standby_routing_id), pipe_,
              locally_initiated_);
            _standby_pipes.ZLINK_MAP_INSERT_OR_EMPLACE (
              pipe_, ZLINK_MOVE (original_routing_id));
            return true;
        }

        if (router_debug_enabled ()) {
            char rid_text[160];
            format_blob_routing_id_debug (routing_id_, rid_text, sizeof (rid_text));
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
        erase_out_pipe (old_pipe);
        old_pipe->set_router_socket_routing_id (new_routing_id);
        add_out_pipe (ZLINK_MOVE (new_routing_id), old_pipe, old_locally_initiated);
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
        } else {
            old_pipe->terminate (true);
        }
    }

    pipe_->set_router_socket_routing_id (routing_id_);
    add_out_pipe (ZLINK_MOVE (routing_id_), pipe_, locally_initiated_);
    cache_completion_pipe_routing_id (pipe_);
    // When Application is the second lane, pair-table admission precedes
    // xattach_pipe() and this stays suppressed until the common attach path
    // releases the write hold. A route adopted later sees that released hold
    // and publishes the complementary edge here.
    (void) emit_transport_pair_ready (pipe_);
    if (router_debug_enabled ()) {
        char rid_text[160];
        format_blob_routing_id_debug (pipe_->get_routing_id (), rid_text, sizeof (rid_text));
        fprintf (stderr, "router identify_peer: add out pipe rid=%s\n", rid_text);
    }
    if (local_peer_weight () != 100)
        send_local_peer_weight (pipe_);

    return true;
}

void router_t::promote_anonymous_pipe_for_dispatch (pipe_t *pipe_)
{
    if (!pipe_)
        return;

    // The caller owns _dispatch_route_lifecycle_mu across route adoption and
    // FQ publication so termination cannot observe a half-promoted endpoint.
    const std::map<pipe_t *, bool>::iterator it = _anonymous_pipes.find (pipe_);
    if (it == _anonymous_pipes.end ())
        return;

    _anonymous_pipes.erase (it);
    _fq.attach (pipe_);
    if (socket_msg_dispatch_active ()) {
        (void) pipe_->check_read ();
        _fq.deactivate (pipe_);
    }
}

int router_t::apply_peer_weight (pipe_t *pipe_, uint32_t weight_)
{
    if (!pipe_)
        return 1;

    const blob_t &routing_id = pipe_->get_routing_id ();
    out_pipe_t *out_pipe = lookup_out_pipe (routing_id);
    if (!out_pipe || out_pipe->pipe != pipe_)
        return 1;
    if (out_pipe->weight == weight_)
        return 1;

    update_out_pipe_weight (out_pipe, weight_);
    emit_peer_weight_changed (pipe_, weight_);
    return 1;
}
}
