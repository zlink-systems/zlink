/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/ctx.hpp"
#include "sockets/common/socket_base.hpp"
#include "sockets/common/socket_runtime.hpp"

void zlink::socket_endpoint_runtime_t::attach_pipe (pipe_t *pipe_)
{
    attached_pipes.push_back (pipe_);
}

void zlink::socket_endpoint_runtime_t::detach_pipe (pipe_t *pipe_)
{
    // A paired transport can terminate before both lanes finish their
    // handshake. Its socket-side pipe has an event sink but has not yet been
    // added to attached_pipes, so termination must tolerate that state.
    if (attached_pipes.contains (pipe_))
        attached_pipes.erase (pipe_);
}

size_t zlink::socket_endpoint_runtime_t::attached_pipe_count () const
{
    return attached_pipes.size ();
}

bool zlink::socket_endpoint_runtime_t::has_attached_pipes () const
{
    return !attached_pipes.empty ();
}

zlink::pipe_t *zlink::socket_endpoint_runtime_t::attached_pipe (size_t index_)
{
    return attached_pipes[index_];
}

const zlink::pipe_t *zlink::socket_endpoint_runtime_t::attached_pipe (size_t index_) const
{
    return attached_pipes[index_];
}

void zlink::socket_endpoint_runtime_t::disable_transport_pair_reconnects ()
{
    for (socket_endpoints_t::iterator it = endpoints.begin (),
                                      end = endpoints.end ();
         it != end; ++it) {
        if (it->second.transport_pair_state)
            it->second.transport_pair_state->disable_reconnect ();
    }
}

void zlink::socket_endpoint_runtime_t::store_last_recv_source_rid (
  const zlink_routing_id_t *source_rid_)
{
    if (!source_rid_) {
        clear_last_recv_source_rid ();
        return;
    }

    last_recv_source_rid = *source_rid_;
    last_recv_source_rid_valid = true;
}

void zlink::socket_endpoint_runtime_t::clear_last_recv_source_rid ()
{
    if (!last_recv_source_rid_valid)
        return;

    memset (&last_recv_source_rid, 0, sizeof (last_recv_source_rid));
    last_recv_source_rid_valid = false;
}

bool zlink::socket_endpoint_runtime_t::copy_last_recv_source_rid (zlink_routing_id_t *out_) const
{
    if (out_)
        memset (out_, 0, sizeof (*out_));

    if (!last_recv_source_rid_valid || !out_)
        return false;

    *out_ = last_recv_source_rid;
    return true;
}

void zlink::socket_endpoint_runtime_t::set_last_endpoint (const std::string &endpoint_)
{
    last_endpoint = endpoint_;
}

const std::string &zlink::socket_endpoint_runtime_t::last_endpoint_uri () const
{
    return last_endpoint;
}

void zlink::socket_inprocs_t::emplace (const char *endpoint_uri_, pipe_t *pipe_)
{
    _inprocs.ZLINK_MAP_INSERT_OR_EMPLACE (std::string (endpoint_uri_), pipe_);
}

int zlink::socket_inprocs_t::erase_pipes (const std::string &endpoint_uri_str_,
                                          socket_base_t *owner_)
{
    const std::pair<map_t::iterator, map_t::iterator> range =
      _inprocs.equal_range (endpoint_uri_str_);
    if (range.first == range.second) {
        errno = ENOENT;
        return -1;
    }

    std::vector<pipe_t *> pipes;
    for (map_t::iterator it = range.first; it != range.second; ++it)
        if (it->second->retain_lifetime_ref ())
            pipes.push_back (it->second);
    // Remove connect intent before materialization can observe peer termination
    // and queue a reconnect. Retain the pipes across that command progress.
    _inprocs.erase (range.first, range.second);

    // Pending peers have no binder to complete their termination handshake.
    // The context's existing helper gives them an owner before we terminate.
    (void) owner_->get_ctx ()->materialize_pending_inproc (endpoint_uri_str_, owner_);
    for (size_t i = 0; i != pipes.size (); ++i) {
        owner_->terminate_inproc_pipe_with_peer_progress (pipes[i]);
        pipes[i]->release_lifetime_ref ();
    }
    return 0;
}

void zlink::socket_inprocs_t::erase_pipe (const pipe_t *pipe_)
{
    for (map_t::iterator it = _inprocs.begin (), end = _inprocs.end (); it != end; ++it)
        if (it->second == pipe_) {
            _inprocs.erase (it);
            break;
        }
}

bool zlink::socket_inprocs_t::endpoint_for_pipe (
  const pipe_t *pipe_, std::string *endpoint_out_) const
{
    if (!pipe_ || !endpoint_out_)
        return false;

    for (map_t::const_iterator it = _inprocs.begin (), end = _inprocs.end ();
         it != end; ++it) {
        if (it->second != pipe_)
            continue;
        *endpoint_out_ = it->first;
        return true;
    }
    return false;
}
