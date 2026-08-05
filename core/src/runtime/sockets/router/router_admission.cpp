/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/router/router.hpp"

#include "core/pipe.hpp"
#include "protocol/wire.hpp"
#include "utils/debug_log.hpp"

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

    // A reconnect created by the same side always supersedes the older pipe.
    // Traffic observed on that older pipe is historical and does not prove
    // that its transport is still usable.
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
        if (!_handover && !same_local_endpoint_reconnect)
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
        const bool reciprocal_duplicate =
          old_locally_initiated != locally_initiated_;

        erase_out_pipe (old_pipe);
        old_pipe->set_router_socket_routing_id (new_routing_id);
        add_out_pipe (ZLINK_MOVE (new_routing_id), old_pipe, old_locally_initiated);
        if (reciprocal_duplicate) {
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
