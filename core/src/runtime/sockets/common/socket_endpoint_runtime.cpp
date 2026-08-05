/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

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

int zlink::socket_inprocs_t::erase_pipes (const std::string &endpoint_uri_str_)
{
    const std::pair<map_t::iterator, map_t::iterator> range =
      _inprocs.equal_range (endpoint_uri_str_);
    if (range.first == range.second) {
        errno = ENOENT;
        return -1;
    }

    for (map_t::iterator it = range.first; it != range.second; ++it) {
        it->second->send_disconnect_msg ();
        // Explicit endpoint disconnect should not defer pipe teardown.
        // The non-inproc term_endpoint path also uses terminate(false).
        it->second->terminate (false);
    }
    _inprocs.erase (range.first, range.second);
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
