/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <ctype.h>
#include <new>
#include <string>

#include "core/address.hpp"
#include "core/ctx.hpp"
#include "core/ctx_inproc_registry.hpp"
#include "core/endpoint.hpp"
#include "core/io_thread.hpp"
#include "core/pipe.hpp"
#include "core/session_base.hpp"
#include "core/transport_pair_policy.hpp"
#include "utils/random.hpp"
#include "sockets/common/socket_base.hpp"
#include "transports/ipc/ipc_address.hpp"
#include "transports/tcp/tcp_address.hpp"

// ASIO-only build: transport listeners are always included.
#include "transports/tcp/asio_tcp_listener.hpp"
#if defined ZLINK_HAVE_IPC
#include "transports/ipc/asio_ipc_listener.hpp"
#endif
#if defined ZLINK_HAVE_ASIO_SSL
#include "transports/tls/asio_tls_listener.hpp"
#endif

#if defined ZLINK_HAVE_WS
#include "transports/ws/asio_ws_listener.hpp"
#include "transports/ws/ws_address.hpp"
#endif
#ifdef ZLINK_HAVE_WSS
#include "transports/tls/wss_address.hpp"
#endif

namespace
{
bool combined_hwm_bytes (uint64_t first_, uint64_t second_, uint64_t *out_)
{
    if (!out_)
        return false;
    if (first_ == 0 || second_ == 0)
        *out_ = 0;
    else if (UINT64_MAX - first_ < second_)
        return false;
    else
        *out_ = first_ + second_;
    return true;
}

//  ZLINK_OPT_MAXMSGSIZE is an inbound limit where -1 means unlimited. pipe_t
//  expresses "no bound" as 0, so only a positive value becomes a bound.
uint64_t finite_max_message_bytes (int64_t maxmsgsize_)
{
    return maxmsgsize_ > 0 ? static_cast<uint64_t> (maxmsgsize_) : 0;
}

uint64_t allocate_transport_pair_id ()
{
    uint64_t pair_id = 0;
    while (pair_id == 0)
        zlink::generate_random_bytes (
          reinterpret_cast<unsigned char *> (&pair_id), sizeof (pair_id));
    return pair_id;
}
}

int zlink::socket_base_t::parse_uri (const char *uri_, std::string &scheme_, std::string &path_)
{
    zlink_assert (uri_ != NULL);

    const std::string uri (uri_);
    const std::string::size_type pos = uri.find ("://");
    if (pos == std::string::npos) {
        errno = EINVAL;
        return -1;
    }
    scheme_ = uri.substr (0, pos);
    path_ = uri.substr (pos + 3);

    if (scheme_.empty () || path_.empty ()) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int zlink::socket_base_t::check_protocol (const std::string &protocol_) const
{
    //  First check out whether the protocol is something we are aware of.
    if (protocol_ != protocol_name::inproc
#if defined ZLINK_HAVE_IPC
        && protocol_ != protocol_name::ipc
#endif
        && protocol_ != protocol_name::tcp
#ifdef ZLINK_HAVE_WS
        && protocol_ != protocol_name::ws
#endif
#ifdef ZLINK_HAVE_WSS
        && protocol_ != protocol_name::wss
#endif
#ifdef ZLINK_HAVE_TLS
        && protocol_ != protocol_name::tls
#endif
    ) {
        errno = EPROTONOSUPPORT;
        return -1;
    }

    return 0;
}

int zlink::socket_base_t::bind (const char *endpoint_uri_)
{
    socket_public_api_scope_t admission (lifecycle_coordinator ());
    if (!admission.acquired ())
        return -1;
    socket_public_api_lock_scope_t guard (lifecycle_coordinator ());

    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    int rc = process_commands (0, false);
    if (unlikely (rc != 0))
        return -1;

    std::string protocol;
    std::string address;
    if (parse_uri (endpoint_uri_, protocol, address) || check_protocol (protocol)) {
        return -1;
    }

    if (protocol == protocol_name::inproc)
        return bind_inproc_endpoint (endpoint_uri_);

    io_thread_t *io_thread = choose_io_thread (options.affinity);
    if (!io_thread) {
        errno = EMTHREAD;
        return -1;
    }

    return bind_transport_listener (protocol, address, io_thread);
}

int zlink::socket_base_t::connect (const char *endpoint_uri_)
{
    socket_public_api_scope_t admission (lifecycle_coordinator ());
    if (!admission.acquired ())
        return -1;
    socket_public_api_lock_scope_t guard (lifecycle_coordinator ());
    return connect_internal (endpoint_uri_);
}

int zlink::socket_base_t::connect_internal (const char *endpoint_uri_)
{
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    int rc = process_commands (0, false);
    if (unlikely (rc != 0))
        return -1;

    std::string protocol;
    std::string address;
    if (parse_uri (endpoint_uri_, protocol, address) || check_protocol (protocol)) {
        return -1;
    }

    if (options.type == ZLINK_CORE_SOCKET_STREAM) {
        errno = EOPNOTSUPP;
        return -1;
    }

    if (protocol == protocol_name::inproc) {
        const endpoint_t peer = find_endpoint (endpoint_uri_);
        const bool paired_transport =
          options.type == ZLINK_CORE_SOCKET_DEALER || options.type == ZLINK_CORE_SOCKET_ROUTER;
        const uint64_t pair_id = paired_transport ? allocate_transport_pair_id () : 0;
        const uint64_t pair_generation = paired_transport ? 1 : 0;
        const size_t lane_count = paired_transport ? 2 : 1;

        uint64_t sndhwm = options.sndhwm;
        uint64_t rcvhwm = options.rcvhwm;
        if (peer.socket != NULL
            && (!combined_hwm_bytes (options.sndhwm, peer.options.rcvhwm, &sndhwm)
                || !combined_hwm_bytes (options.rcvhwm, peer.options.sndhwm, &rcvhwm))) {
            errno = EOVERFLOW;
            return -1;
        }

        const bool conflate = get_effective_conflate_option (options);
        for (size_t lane_index = 0; lane_index < lane_count; ++lane_index) {
            object_t *parents[2] = {this, peer.socket == NULL ? this : peer.socket};
            pipe_t *new_pipes[2] = {NULL, NULL};
            uint64_t hwms[2] = {conflate ? 0 : sndhwm, conflate ? 0 : rcvhwm};
            bool conflates[2] = {conflate, conflate};
            rc = pipepair (parents, new_pipes, hwms, conflates);
            errno_assert (rc == 0);

            const transport_lane_t lane =
              lane_index == 0 ? transport_lane_application : transport_lane_completion;
            if (lane == transport_lane_completion) {
                hwms[0] = transport_pair_policy::completion_hwm (hwms[0]);
                hwms[1] = transport_pair_policy::completion_hwm (hwms[1]);
                new_pipes[0]->set_hwms (hwms[1], hwms[0]);
                new_pipes[1]->set_hwms (hwms[0], hwms[1]);
            }
            new_pipes[0]->set_transport_pair (lane, pair_id, pair_generation);
            new_pipes[1]->set_transport_pair (lane, pair_id, pair_generation);
            if (!conflate) {
                new_pipes[0]->set_hwms_boost (peer.options.sndhwm, peer.options.rcvhwm);
                new_pipes[1]->set_hwms_boost (options.sndhwm, options.rcvhwm);
            }
            //  inproc has no decoder to reject an oversize message, so each
            //  direction carries the reader's inbound maximum. It bounds the
            //  empty-pipe oversize exception; network directions get the same
            //  bound from the receiving decoder.
            new_pipes[0]->set_max_message_bytes (
              finite_max_message_bytes (peer.socket ? peer.options.maxmsgsize
                                                    : options.maxmsgsize));
            new_pipes[1]->set_max_message_bytes (
              finite_max_message_bytes (options.maxmsgsize));

            bool connected_inproc_now = false;
            if (!peer.socket) {
                send_routing_id (new_pipes[0], options);
                if (paired_transport && lane == transport_lane_application) {
                    new_pipes[0]->hold_writes_until_transport_pair_ready ();
                    new_pipes[1]->hold_writes_until_transport_pair_ready ();
                }
                options_t lane_options = options;
                lane_options.transport_lane = lane;
                lane_options.transport_pair_id = pair_id;
                lane_options.transport_pair_generation = pair_generation;
                lane_options.transport_pair_initiator = true;
                const endpoint_t endpoint (this, lane_options);
                connected_inproc_now =
                  pend_connection (std::string (endpoint_uri_), endpoint, new_pipes);
            } else {
                if (lane_index > 0)
                    peer.socket->inc_seqnum ();
                if (peer.options.recv_routing_id)
                    send_routing_id (new_pipes[0], options);
                if (options.recv_routing_id)
                    send_routing_id (new_pipes[1], peer.options);

                new_pipes[0]->set_peer_routing_id (peer.options.routing_id,
                                                   peer.options.routing_id_size);
                new_pipes[1]->set_peer_routing_id (options.routing_id,
                                                   options.routing_id_size);
                if (paired_transport) {
                    const uintptr_t peer_instance =
                      reinterpret_cast<uintptr_t> (peer.socket);
                    const uintptr_t local_instance =
                      reinterpret_cast<uintptr_t> (this);
                    new_pipes[0]->set_transport_peer_identity (
                      reinterpret_cast<const unsigned char *> (&peer_instance),
                      sizeof (peer_instance));
                    new_pipes[1]->set_transport_peer_identity (
                      reinterpret_cast<const unsigned char *> (&local_instance),
                      sizeof (local_instance));
                }
                if (paired_transport && lane == transport_lane_application) {
                    new_pipes[0]->hold_writes_until_transport_pair_ready ();
                    new_pipes[1]->hold_writes_until_transport_pair_ready ();
                }
                send_bind (peer.socket, new_pipes[1], false);
                peer.socket->emit_inproc_connection_ready (new_pipes[1]);
                connected_inproc_now = true;
            }

            attach_pipe (new_pipes[0], false, true, connected_inproc_now);
            if (connected_inproc_now)
                emit_inproc_connection_ready (new_pipes[0]);
            endpoint_runtime ().inprocs.emplace (endpoint_uri_, new_pipes[0]);
        }

        endpoint_runtime ().set_last_endpoint (endpoint_uri_);
        options.connected = true;
        return 0;
    }
    if (unlikely (0 != endpoint_runtime ().endpoints.count (endpoint_uri_)))
        return 0;

    io_thread_t *io_thread = choose_io_thread (options.affinity);
    if (!io_thread) {
        errno = EMTHREAD;
        return -1;
    }

    const bool subscribe_to_all = false;
    const bool paired_transport =
      options.type == ZLINK_CORE_SOCKET_DEALER || options.type == ZLINK_CORE_SOCKET_ROUTER;
    const uint64_t pair_id = paired_transport ? allocate_transport_pair_id () : 0;
    const std::shared_ptr<transport_pair_state_t> pair_state =
      paired_transport ? std::make_shared<transport_pair_state_t> ()
                       : std::shared_ptr<transport_pair_state_t> ();
    const uint64_t pair_generation =
      pair_state ? pair_state->current_generation () : 0;
    const size_t lane_count = paired_transport ? 2 : 1;

    for (size_t lane_index = 0; lane_index < lane_count; ++lane_index) {
        options_t lane_options = options;
        if (paired_transport) {
            lane_options.zmp_metadata = true;
            lane_options.transport_lane =
              lane_index == 0 ? transport_lane_application : transport_lane_completion;
            lane_options.transport_pair_id = pair_id;
            lane_options.transport_pair_generation = pair_generation;
            lane_options.transport_pair_initiator = true;
            lane_options.transport_pair_state = pair_state;
            if (lane_options.transport_lane == transport_lane_completion) {
                lane_options.sndhwm =
                  transport_pair_policy::completion_hwm (lane_options.sndhwm);
                lane_options.rcvhwm =
                  transport_pair_policy::completion_hwm (lane_options.rcvhwm);
                lane_options.sndbuf =
                  transport_pair_policy::completion_socket_buffer (lane_options.sndbuf);
                lane_options.rcvbuf =
                  transport_pair_policy::completion_socket_buffer (lane_options.rcvbuf);
            }
        }

        address_t *paddr = new (std::nothrow) address_t (protocol, address, this->get_ctx ());
        alloc_assert (paddr);
        if (resolve_connect_address (protocol, address, paddr) != 0) {
            LIBZLINK_DELETE (paddr);
            return -1;
        }

        session_base_t *session =
          session_base_t::create (io_thread, true, this, lane_options, paddr);
        errno_assert (session);
        pipe_t *newpipe = NULL;

        if (lane_options.immediate != 1 || subscribe_to_all) {
            object_t *parents[2] = {this, session};
            pipe_t *new_pipes[2] = {NULL, NULL};
            const bool conflate = get_effective_conflate_option (lane_options);
            uint64_t hwms[2] = {conflate ? 0 : lane_options.sndhwm,
                                conflate ? 0 : lane_options.rcvhwm};
            bool conflates[2] = {conflate, conflate};
            rc = pipepair (parents, new_pipes, hwms, conflates, true);
            errno_assert (rc == 0);
            new_pipes[0]->set_transport_pair (
              lane_options.transport_lane, pair_id, pair_generation);
            new_pipes[1]->set_transport_pair (
              lane_options.transport_lane, pair_id, pair_generation);
            new_pipes[0]->set_locally_initiated (paired_transport);

            if (!paired_transport)
                attach_pipe (new_pipes[0], subscribe_to_all, true);
            else if (lane_options.transport_lane
                     == transport_lane_application) {
                new_pipes[0]->hold_writes_until_transport_pair_ready ();
                attach_pipe (new_pipes[0], subscribe_to_all, true, false);
            } else
                new_pipes[0]->set_event_sink (this);
            newpipe = new_pipes[0];
            session->attach_pipe (new_pipes[1]);
        }

        std::string last_endpoint;
        paddr->to_string (last_endpoint);
        if (lane_index == 0)
            endpoint_runtime ().set_last_endpoint (last_endpoint);
        //  Both lanes of a pair share one endpoint key. The endpoint map is a
        //  multimap, so every endpoint-scoped operation - disconnect above all
        //  - reaches the whole pair without knowing that lanes exist.
        if (paired_transport)
            add_transport_pair_endpoint (
              make_unconnected_connect_endpoint_pair (endpoint_uri_),
              static_cast<own_t *> (session), newpipe, pair_state);
        else
            add_endpoint (make_unconnected_connect_endpoint_pair (endpoint_uri_),
                          static_cast<own_t *> (session), newpipe);
    }
    return 0;
}

void zlink::socket_base_t::socket_bound_endpoints (std::set<std::string> *out_) const
{
    if (!out_)
        return;

    out_->clear ();
    for (endpoints_t::const_iterator it = endpoint_runtime ().endpoints.begin (),
                                     end = endpoint_runtime ().endpoints.end ();
         it != end; ++it) {
        if (it->second.local_type == endpoint_type_bind)
            out_->insert (it->first);
    }
}

bool zlink::socket_base_t::socket_has_manual_connect_endpoints () const
{
    for (endpoints_t::const_iterator it = endpoint_runtime ().endpoints.begin (),
                                     end = endpoint_runtime ().endpoints.end ();
         it != end; ++it) {
        if (it->second.local_type == endpoint_type_connect)
            return true;
    }
    return false;
}

bool zlink::socket_base_t::socket_has_attached_pipes () const
{
    return has_attached_pipes ();
}

std::string zlink::socket_base_t::resolve_tcp_addr (std::string endpoint_uri_pair_,
                                                    const char *tcp_address_)
{
    if (endpoint_runtime ().endpoints.find (endpoint_uri_pair_)
        == endpoint_runtime ().endpoints.end ()) {
        tcp_address_t *tcp_addr = new (std::nothrow) tcp_address_t ();
        alloc_assert (tcp_addr);
        int rc = tcp_addr->resolve (tcp_address_, false, options.ipv6);

        if (rc == 0) {
            tcp_addr->to_string (endpoint_uri_pair_);
            if (endpoint_runtime ().endpoints.find (endpoint_uri_pair_)
                == endpoint_runtime ().endpoints.end ()) {
                rc = tcp_addr->resolve (tcp_address_, true, options.ipv6);
                if (rc == 0)
                    tcp_addr->to_string (endpoint_uri_pair_);
            }
        }
        LIBZLINK_DELETE (tcp_addr);
    }
    return endpoint_uri_pair_;
}

void zlink::socket_base_t::add_transport_pair_endpoint (
  const endpoint_uri_pair_t &endpoint_pair_,
  own_t *endpoint_,
  pipe_t *pipe_,
  const std::shared_ptr<transport_pair_state_t> &pair_state_)
{
    launch_child (endpoint_);
    endpoint_runtime ().endpoints.ZLINK_MAP_INSERT_OR_EMPLACE (
      endpoint_pair_.identifier (),
      endpoint_pipe_t (endpoint_, pipe_, endpoint_pair_.local_type, pair_state_));

    if (pipe_ != NULL) {
        endpoint_uri_pair_t pipe_endpoint_pair = endpoint_pair_;
        pipe_endpoint_pair.connection_id = 0;
        pipe_->set_endpoint_pair (ZLINK_MOVE (pipe_endpoint_pair));
    }
}

void zlink::socket_base_t::add_endpoint (const endpoint_uri_pair_t &endpoint_pair_,
                                         own_t *endpoint_,
                                         pipe_t *pipe_)
{
    launch_child (endpoint_);
    endpoint_runtime ().endpoints.ZLINK_MAP_INSERT_OR_EMPLACE (
      endpoint_pair_.identifier (), endpoint_pipe_t (endpoint_, pipe_, endpoint_pair_.local_type));

    if (pipe_ != NULL) {
        endpoint_uri_pair_t pipe_endpoint_pair = endpoint_pair_;
        //  add_endpoint receives the placeholder made before a connect
        //  attempt has a physical transport. Keep endpoint bookkeeping on
        //  the pipe, but leave its shared transport identity unbound until
        //  session_base_t installs the engine endpoint.
        pipe_endpoint_pair.connection_id = 0;
        pipe_->set_endpoint_pair (ZLINK_MOVE (pipe_endpoint_pair));
    }
}

int zlink::socket_base_t::term_endpoint_internal (const char *endpoint_uri_)
{
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    if (unlikely (!endpoint_uri_)) {
        errno = EINVAL;
        return -1;
    }

    const int rc = process_commands (0, false);
    if (unlikely (rc != 0))
        return -1;

    std::string uri_protocol;
    std::string uri_path;
    if (parse_uri (endpoint_uri_, uri_protocol, uri_path) || check_protocol (uri_protocol)) {
        return -1;
    }

    const std::string endpoint_uri_str = std::string (endpoint_uri_);
    if (uri_protocol == protocol_name::inproc) {
        //  A connect that is still pending (no binder yet) parks its peer
        //  pipe in the context registry where no socket ever runs the pipe
        //  termination handshake. Materialize it into a short-lived PAIR
        //  socket first so both pipe halves can terminate and this socket
        //  can be reaped before ctx term. Best effort: if the context cannot
        //  supply the helper socket (socket limit reached or termination
        //  already started), keep the previous disconnect semantics and fall
        //  through to the local cleanup.
        (void) get_ctx ()->materialize_pending_inproc (endpoint_uri_str, this);
        const int inproc_rc = unregister_endpoint (endpoint_uri_str, this) == 0
                                ? 0
                                : endpoint_runtime ().inprocs.erase_pipes (endpoint_uri_str);
        return inproc_rc;
    }

    const std::string resolved_endpoint_uri =
      (uri_protocol == protocol_name::tcp
#ifdef ZLINK_HAVE_TLS
       || uri_protocol == protocol_name::tls
#endif
       )
        ? resolve_tcp_addr (endpoint_uri_str, uri_path.c_str ())
        : endpoint_uri_str;

    const std::pair<endpoints_t::iterator, endpoints_t::iterator> range =
      endpoint_runtime ().endpoints.equal_range (resolved_endpoint_uri);
    if (range.first == range.second) {
        errno = ENOENT;
        return -1;
    }

    for (endpoints_t::iterator it = range.first; it != range.second; ++it) {
        //  A disconnect ends the whole pair. Without this the surviving lane's
        //  session would treat the peer pipe termination as a transport failure
        //  and redial an endpoint the caller has just removed.
        if (it->second.transport_pair_state)
            it->second.transport_pair_state->disable_reconnect ();
        if (it->second.pipe != NULL)
            it->second.pipe->terminate (false);
        term_child (it->second.endpoint);
    }

    for (size_t i = 0, size = endpoint_runtime ().attached_pipe_count (); i != size; ++i) {
        pipe_t *const pipe = endpoint_runtime ().attached_pipe (i);
        if (!pipe)
            continue;
        if (pipe->get_endpoint_pair ().identifier () == resolved_endpoint_uri)
            pipe->terminate (false);
    }
    endpoint_runtime ().endpoints.erase (range.first, range.second);
    return 0;
}

int zlink::socket_base_t::term_endpoint (const char *endpoint_uri_)
{
    socket_public_api_scope_t admission (lifecycle_coordinator ());
    if (!admission.acquired ())
        return -1;
    socket_public_api_lock_scope_t guard (lifecycle_coordinator ());
    return term_endpoint_internal (endpoint_uri_);
}

int zlink::socket_base_t::term_peer_rid (const zlink_routing_id_t *peer_rid_)
{
    if (!peer_rid_ || peer_rid_->size == 0) {
        errno = EINVAL;
        return -1;
    }

    socket_public_api_scope_t admission (lifecycle_coordinator ());
    if (!admission.acquired ())
        return -1;
    socket_public_api_lock_scope_t guard (lifecycle_coordinator ());

    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    const int rc = process_commands (0, false);
    if (unlikely (rc != 0))
        return -1;

    return xterm_peer_rid (peer_rid_);
}
