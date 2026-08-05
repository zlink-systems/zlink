/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/ctx_inproc_registry.hpp"

#include "core/command.hpp"
#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"

zlink::endpoint_t::endpoint_t () : socket (NULL)
{
}

zlink::endpoint_t::endpoint_t (socket_base_t *socket_, const options_t &options_) :
    socket (socket_), options (options_)
{
}

zlink::ctx_inproc_registry_t::pending_connection_t::pending_connection_t () :
    connect_pipe (NULL), bind_pipe (NULL)
{
}

zlink::ctx_inproc_registry_t::pending_connection_t::pending_connection_t (
  const endpoint_t &endpoint_, pipe_t *connect_pipe_, pipe_t *bind_pipe_) :
    endpoint (endpoint_), connect_pipe (connect_pipe_), bind_pipe (bind_pipe_)
{
}

zlink::ctx_inproc_registry_t::ctx_inproc_registry_t ()
{
}

int zlink::ctx_inproc_registry_t::register_endpoint (const char *addr_, const endpoint_t &endpoint_)
{
    scoped_lock_t locker (_sync);

    const std::pair<endpoints_t::iterator, bool> inserted =
      _endpoints.insert (endpoints_t::value_type (std::string (addr_), endpoint_));
    if (!inserted.second) {
        errno = EADDRINUSE;
        return -1;
    }

    return 0;
}

int zlink::ctx_inproc_registry_t::unregister_endpoint (const std::string &addr_,
                                                       const socket_base_t *socket_)
{
    scoped_lock_t locker (_sync);

    const endpoints_t::iterator it = _endpoints.find (addr_);
    if (it == _endpoints.end () || it->second.socket != socket_) {
        errno = ENOENT;
        return -1;
    }

    _endpoints.erase (it);
    return 0;
}

void zlink::ctx_inproc_registry_t::unregister_endpoints (const socket_base_t *socket_)
{
    scoped_lock_t locker (_sync);

    for (endpoints_t::iterator it = _endpoints.begin (), end = _endpoints.end (); it != end;) {
        if (it->second.socket == socket_)
#if __cplusplus >= 201103L || (defined _MSC_VER && _MSC_VER >= 1700)
            it = _endpoints.erase (it);
#else
            _endpoints.erase (it++);
#endif
        else
            ++it;
    }
}

zlink::endpoint_t zlink::ctx_inproc_registry_t::find_endpoint (const char *addr_)
{
    scoped_lock_t locker (_sync);

    const endpoints_t::iterator it = _endpoints.find (addr_);
    if (it == _endpoints.end ()) {
        errno = ECONNREFUSED;
        return endpoint_t ();
    }

    endpoint_t endpoint = it->second;
    endpoint.socket->inc_seqnum ();
    return endpoint;
}

bool zlink::ctx_inproc_registry_t::pend_connection (const std::string &addr_,
                                                    const endpoint_t &endpoint_,
                                                    pipe_t **pipes_)
{
    scoped_lock_t locker (_sync);

    const pending_connection_t pending_connection (endpoint_, pipes_[0], pipes_[1]);
    const endpoints_t::iterator it = _endpoints.find (addr_);
    if (it == _endpoints.end ()) {
        endpoint_.socket->inc_seqnum ();
        _pending_connections.insert (pending_connections_t::value_type (addr_, pending_connection));
        return false;
    }

    connect_inproc_sockets (it->second.socket, it->second.options, pending_connection,
                            connect_side);
    return true;
}

void zlink::ctx_inproc_registry_t::connect_pending (const char *addr_, socket_base_t *bind_socket_)
{
    scoped_lock_t locker (_sync);

    const endpoints_t::iterator endpoint_it = _endpoints.find (addr_);
    if (endpoint_it == _endpoints.end ())
        return;

    const std::pair<pending_connections_t::iterator, pending_connections_t::iterator> pending =
      _pending_connections.equal_range (addr_);
    for (pending_connections_t::iterator p = pending.first; p != pending.second; ++p) {
        connect_inproc_sockets (bind_socket_, endpoint_it->second.options, p->second, bind_side);
    }

    _pending_connections.erase (pending.first, pending.second);
}

bool zlink::ctx_inproc_registry_t::has_pending_for_socket (
  const std::string &addr_, const socket_base_t *socket_) const
{
    if (!socket_)
        return false;

    scoped_lock_t locker (_sync);
    const std::pair<pending_connections_t::const_iterator, pending_connections_t::const_iterator>
      pending = _pending_connections.equal_range (addr_);
    for (pending_connections_t::const_iterator it = pending.first; it != pending.second; ++it) {
        if (it->second.endpoint.socket == socket_)
            return true;
    }
    return false;
}

size_t zlink::ctx_inproc_registry_t::materialize_pending_for_socket (
  const std::string &addr_, const socket_base_t *socket_, socket_base_t *bind_socket_)
{
    if (!socket_ || !bind_socket_)
        return 0;

    options_t bind_options;
    bind_options.type = ZLINK_CORE_SOCKET_PAIR;

    size_t count = 0;
    scoped_lock_t locker (_sync);
    const std::pair<pending_connections_t::iterator, pending_connections_t::iterator> pending =
      _pending_connections.equal_range (addr_);
    pending_connections_t::iterator it = pending.first;
    while (it != pending.second) {
        if (it->second.endpoint.socket != socket_) {
            ++it;
            continue;
        }

        connect_inproc_sockets (bind_socket_, bind_options, it->second, bind_side);
        it = _pending_connections.erase (it);
        ++count;
    }
    return count;
}

void zlink::ctx_inproc_registry_t::collect_pending_addresses (std::vector<std::string> *out_) const
{
    if (!out_)
        return;

    scoped_lock_t locker (_sync);
    out_->clear ();
    out_->reserve (_pending_connections.size ());
    for (pending_connections_t::const_iterator it = _pending_connections.begin (),
                                               end = _pending_connections.end ();
         it != end; ++it)
        out_->push_back (it->first);
}

void zlink::ctx_inproc_registry_t::connect_inproc_sockets (
  socket_base_t *bind_socket_,
  const options_t &bind_options_,
  const pending_connection_t &pending_connection_,
  side side_)
{
    pending_connection_.bind_pipe->set_tid (bind_socket_->get_tid ());

    if (!bind_options_.recv_routing_id) {
        msg_t msg;
        const bool ok = pending_connection_.bind_pipe->read (&msg);
        zlink_assert (ok);
        const int rc = msg.close ();
        errno_assert (rc == 0);
    }

    // Each direction uses the actual reader's inbound limit regardless of
    // connection order or conflate policy.
    pending_connection_.connect_pipe->set_max_message_bytes (
      bind_options_.maxmsgsize > 0
        ? static_cast<uint64_t> (bind_options_.maxmsgsize)
        : 0);
    pending_connection_.bind_pipe->set_max_message_bytes (
      pending_connection_.endpoint.options.maxmsgsize > 0
        ? static_cast<uint64_t> (
            pending_connection_.endpoint.options.maxmsgsize)
        : 0);

    if (pending_connection_.connect_pipe->get_transport_pair_id () != 0) {
        const uintptr_t bind_instance =
          reinterpret_cast<uintptr_t> (bind_socket_);
        const uintptr_t connect_instance =
          reinterpret_cast<uintptr_t> (pending_connection_.endpoint.socket);
        pending_connection_.connect_pipe->set_transport_peer_identity (
          reinterpret_cast<const unsigned char *> (&bind_instance),
          sizeof (bind_instance));
        pending_connection_.bind_pipe->set_transport_peer_identity (
          reinterpret_cast<const unsigned char *> (&connect_instance),
          sizeof (connect_instance));
    }

    if (!get_effective_conflate_option (pending_connection_.endpoint.options)) {
        pending_connection_.connect_pipe->set_hwms_boost (bind_options_.sndhwm,
                                                          bind_options_.rcvhwm);
        pending_connection_.bind_pipe->set_hwms_boost (pending_connection_.endpoint.options.sndhwm,
                                                       pending_connection_.endpoint.options.rcvhwm);

        pending_connection_.connect_pipe->set_hwms (pending_connection_.endpoint.options.rcvhwm,
                                                    pending_connection_.endpoint.options.sndhwm);
        pending_connection_.bind_pipe->set_hwms (bind_options_.rcvhwm, bind_options_.sndhwm);

    } else {
        pending_connection_.connect_pipe->set_hwms (0, 0);
        pending_connection_.bind_pipe->set_hwms (0, 0);
    }

    if (side_ == bind_side) {
        command_t cmd;
        cmd.type = command_t::bind;
        cmd.args.bind.pipe = pending_connection_.bind_pipe;
        // process_command releases every pipe reference carried by a command.
        if (!pending_connection_.bind_pipe->retain_lifetime_ref ())
            return;
        bind_socket_->inc_seqnum ();
        bind_socket_->process_command (cmd);
        pending_connection_.endpoint.socket->validate_inproc_connection (
          pending_connection_.connect_pipe);
        bind_socket_->emit_inproc_connection_ready (pending_connection_.bind_pipe);
        pending_connection_.endpoint.socket->emit_inproc_connection_ready (
          pending_connection_.connect_pipe);
        bind_socket_->send_inproc_connected (pending_connection_.endpoint.socket);
    } else {
        if (!pending_connection_.connect_pipe->send_bind (
              bind_socket_, pending_connection_.bind_pipe, true))
            return;
        bind_socket_->emit_inproc_connection_ready (pending_connection_.bind_pipe);
    }

    if (pending_connection_.endpoint.options.recv_routing_id
        && pending_connection_.endpoint.socket->check_tag ()) {
        send_routing_id (pending_connection_.bind_pipe, bind_options_);
    }
}
