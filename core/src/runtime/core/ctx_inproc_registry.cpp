/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/ctx_inproc_registry.hpp"

#include "core/command.hpp"
#include "core/ctx.hpp"
#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "protocol/zmp_control.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"

#include <memory>

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
    std::unique_ptr<options_t> bind_options;
    std::vector<pending_connection_t> pending_connections;
    {
        scoped_lock_t locker (_sync);

        const endpoints_t::iterator endpoint_it = _endpoints.find (addr_);
        if (endpoint_it == _endpoints.end ())
            return;

        bind_options.reset (new options_t (endpoint_it->second.options));
        const std::pair<pending_connections_t::iterator,
                        pending_connections_t::iterator>
          pending = _pending_connections.equal_range (addr_);
        // Copy the complete batch before erasing it.  If allocation fails,
        // every pending pipe remains owned by the registry exactly as before.
        for (pending_connections_t::iterator p = pending.first;
             p != pending.second; ++p)
            pending_connections.push_back (p->second);
        _pending_connections.erase (pending.first, pending.second);
    }

    // Binding a pending inproc peer can synchronously attach the bind-side
    // pipe and queues connector-side attachment. Both may replan Auto-HWM and
    // acquire context/socket locks, so neither runs while the registry mutex
    // is held.
    for (std::vector<pending_connection_t>::const_iterator p =
           pending_connections.begin ();
         p != pending_connections.end (); ++p)
        connect_inproc_sockets (bind_socket_, *bind_options, *p, bind_side);
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

    std::vector<pending_connection_t> pending_connections;
    {
        scoped_lock_t locker (_sync);
        const std::pair<pending_connections_t::iterator,
                        pending_connections_t::iterator>
          pending = _pending_connections.equal_range (addr_);

        // First finish every possibly-throwing copy.  Only then remove the
        // selected entries so allocation failure cannot orphan their pipes.
        for (pending_connections_t::iterator it = pending.first;
             it != pending.second; ++it) {
            if (it->second.endpoint.socket == socket_)
                pending_connections.push_back (it->second);
        }
        pending_connections_t::iterator it = pending.first;
        while (it != pending.second) {
            if (it->second.endpoint.socket == socket_)
                it = _pending_connections.erase (it);
            else
                ++it;
        }
    }

    for (std::vector<pending_connection_t>::const_iterator it =
           pending_connections.begin ();
         it != pending_connections.end (); ++it)
        connect_inproc_sockets (bind_socket_, bind_options, *it, bind_side,
                                true);
    return pending_connections.size ();
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
  side side_,
  bool materialization_)
{
    pending_connection_.bind_pipe->set_tid (bind_socket_->get_tid ());

    const uint64_t pair_id =
      pending_connection_.connect_pipe->get_transport_pair_id ();
    const uint64_t pair_generation =
      pending_connection_.connect_pipe->get_transport_pair_generation ();
    const bool paired = pair_id != 0;
    const bool completion =
      paired && pending_connection_.connect_pipe->get_transport_lane ()
                  == transport_lane_completion;
    unsigned char lane_count =
      paired ? zmp_control::expected_transport_lane_count (
                 pending_connection_.endpoint.options.type,
                 bind_options_.type)
             : 0;
    // Closing a connect-first socket materializes its otherwise ownerless
    // pending half through an internal PAIR helper solely so the pipe
    // termination handshake can complete. It is not a negotiated peer.
    if (paired && lane_count == 0 && materialization_)
        lane_count = 1;
    if (paired && lane_count == 0) {
        pending_connection_.connect_pipe->terminate (false);
        pending_connection_.bind_pipe->terminate (false);
        if (side_ == bind_side)
            bind_socket_->send_inproc_connected (
              pending_connection_.endpoint.socket);
        return;
    }
    if (paired) {
        pending_connection_.connect_pipe->set_transport_lane_count (
          lane_count);
        pending_connection_.bind_pipe->set_transport_lane_count (lane_count);
    }

    // Pending inproc Application connections stage one routing-id frame
    // before the bind socket is known. Completion lanes deliberately stage no
    // such frame, so neither consume nor publish an identity on that lane.
    if (!completion && !bind_options_.recv_routing_id) {
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

    pending_connection_.connect_pipe->set_peer_routing_id (
      bind_options_.routing_id, bind_options_.routing_id_size);
    pending_connection_.bind_pipe->set_peer_routing_id (
      pending_connection_.endpoint.options.routing_id,
      pending_connection_.endpoint.options.routing_id_size);
    pending_connection_.connect_pipe->set_peer_socket_type (
      bind_options_.type);
    pending_connection_.bind_pipe->set_peer_socket_type (
      pending_connection_.endpoint.options.type);

    if (paired) {
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

    if (!completion) {
        ctx_t *const ctx = bind_socket_->get_ctx ();
        ctx->record_auto_hwm_endpoint_policy (
          pending_connection_.endpoint.socket->make_auto_hwm_queue_policy (
            pending_connection_.connect_pipe->out_physical_queue (), true));
        ctx->record_auto_hwm_endpoint_policy (
          bind_socket_->make_auto_hwm_queue_policy (
            pending_connection_.bind_pipe->in_physical_queue (), false));
        ctx->record_auto_hwm_endpoint_policy (
          bind_socket_->make_auto_hwm_queue_policy (
            pending_connection_.bind_pipe->out_physical_queue (), true));
        ctx->record_auto_hwm_endpoint_policy (
          pending_connection_.endpoint.socket->make_auto_hwm_queue_policy (
            pending_connection_.connect_pipe->in_physical_queue (), false));
    }
    if (completion) {
        pending_connection_.connect_pipe->set_hwms (0, 0);
        pending_connection_.bind_pipe->set_hwms (0, 0);
    } else if (!get_effective_conflate_option (pending_connection_.endpoint.options)) {
        pending_connection_.connect_pipe->set_hwms (pending_connection_.endpoint.options.rcvhwm,
                                                    pending_connection_.endpoint.options.sndhwm);
        pending_connection_.bind_pipe->set_hwms (bind_options_.rcvhwm, bind_options_.sndhwm);

    } else {
        pending_connection_.connect_pipe->set_hwms (0, 0);
        pending_connection_.bind_pipe->set_hwms (0, 0);
    }

    bool completion_materialized_before_application = false;
    if (!completion && lane_count == 2u && side_ == bind_side) {
        const std::string endpoint_uri =
          pending_connection_.connect_pipe->get_endpoint_pair ().identifier ();
        if (pending_connection_.endpoint.socket
              ->materialize_inproc_completion_lane (
                bind_socket_, bind_options_, endpoint_uri, pair_id,
                pair_generation, true)
            != 0) {
            // pend_connection() reserved one connector sequence number. No
            // application bind command will consume it on this failure path.
            bind_socket_->send_inproc_connected (
              pending_connection_.endpoint.socket);
            pending_connection_.connect_pipe->terminate (false);
            pending_connection_.bind_pipe->terminate (false);
            return;
        }
        completion_materialized_before_application = true;
    }

    if (side_ == bind_side) {
        command_t cmd;
        cmd.type = command_t::bind;
        cmd.args.bind.pipe = pending_connection_.bind_pipe;
        // process_command releases every pipe reference carried by a command.
        if (!pending_connection_.bind_pipe->retain_lifetime_ref ()) {
            // No bind command will consume the connect-before-bind sequence
            // reservation. Balance it even when close won this lifetime race.
            bind_socket_->send_inproc_connected (
              pending_connection_.endpoint.socket);
            pending_connection_.connect_pipe->terminate (false);
            pending_connection_.bind_pipe->terminate (false);
            return;
        }
        bind_socket_->inc_seqnum ();
        bind_socket_->process_command (cmd);
        if (paired) {
            // The connector may be concurrently sending while bind() owns the
            // bind socket's public synchronization. Queue paired scheduler
            // attachment through the normal command owner instead of
            // mutating DEALER/ROUTER state directly and risking both a data
            // race and cross-socket lock inversion. pend_connection() already
            // reserved the connector sequence number, so this bind command
            // consumes that reservation.
            if (!pending_connection_.connect_pipe->send_bind (
                  pending_connection_.endpoint.socket,
                  pending_connection_.connect_pipe, false)) {
                bind_socket_->send_inproc_connected (
                  pending_connection_.endpoint.socket);
                pending_connection_.connect_pipe->terminate (false);
                pending_connection_.bind_pipe->terminate (false);
                return;
            }
        } else {
            pending_connection_.endpoint.socket->validate_inproc_connection (
              pending_connection_.connect_pipe);
            pending_connection_.endpoint.socket->emit_inproc_connection_ready (
              pending_connection_.connect_pipe);
            bind_socket_->send_inproc_connected (
              pending_connection_.endpoint.socket);
        }
        bind_socket_->emit_inproc_connection_ready (pending_connection_.bind_pipe);
    } else {
        if (!pending_connection_.connect_pipe->send_bind (
              bind_socket_, pending_connection_.bind_pipe, true))
            return;
        bind_socket_->emit_inproc_connection_ready (pending_connection_.bind_pipe);
    }

    if (!completion && lane_count == 2u
        && !completion_materialized_before_application) {
        const std::string endpoint_uri =
          pending_connection_.connect_pipe->get_endpoint_pair ().identifier ();
        if (pending_connection_.endpoint.socket
              ->materialize_inproc_completion_lane (
                bind_socket_, bind_options_, endpoint_uri, pair_id,
                pair_generation, side_ == bind_side)
            != 0) {
            pending_connection_.connect_pipe->terminate (false);
            pending_connection_.bind_pipe->terminate (false);
            return;
        }
    }

    if (!completion
        && pending_connection_.endpoint.options.recv_routing_id
        && pending_connection_.endpoint.socket->check_tag ()) {
        send_routing_id (pending_connection_.bind_pipe, bind_options_);
    }
}
