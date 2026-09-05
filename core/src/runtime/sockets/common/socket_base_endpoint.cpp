/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "api/socket/socket_request_reply_internal.hpp"

#include <ctype.h>
#include <new>
#include <string>

#include "core/address.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/ctx.hpp"
#include "core/ctx_inproc_registry.hpp"
#include "core/endpoint.hpp"
#include "core/io_thread.hpp"
#include "core/pipe.hpp"
#include "core/session_base.hpp"
#include "core/transport_pair_policy.hpp"
#include "protocol/zmp_control.hpp"
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

#ifdef ZLINK_BUILD_TESTS
std::atomic<zlink::transport_pair_owner_after_claim_test_hook_fn>
  owner_after_claim_test_hook (NULL);
std::atomic<void *> owner_after_claim_test_hook_userdata (NULL);

struct deferred_transport_pair_owner_test_request_t
{
    deferred_transport_pair_owner_test_request_t () :
        owner (NULL),
        session (NULL),
        peer_socket_type (0),
        connection_id (0),
        pair_id (0),
        generation (0),
        lane (0),
        active (false)
    {
    }

    zlink::socket_base_t *owner;
    zlink::session_base_t *session;
    int peer_socket_type;
    uint64_t connection_id;
    uint64_t pair_id;
    uint64_t generation;
    unsigned char lane;
    bool active;
};

std::mutex deferred_owner_test_sync;
deferred_transport_pair_owner_test_request_t deferred_owner_test_request;
#endif
}

#ifdef ZLINK_BUILD_TESTS
void zlink::test_set_transport_pair_owner_after_claim_hook (
  transport_pair_owner_after_claim_test_hook_fn hook_, void *userdata_)
{
    if (!hook_) {
        owner_after_claim_test_hook.store (NULL, std::memory_order_release);
        owner_after_claim_test_hook_userdata.store (NULL,
                                                    std::memory_order_release);
        return;
    }
    owner_after_claim_test_hook_userdata.store (userdata_,
                                                std::memory_order_release);
    owner_after_claim_test_hook.store (hook_, std::memory_order_release);
}

bool zlink::socket_base_t::test_resume_deferred_transport_pair_owner_request ()
{
    deferred_transport_pair_owner_test_request_t request;
    {
        std::lock_guard<std::mutex> lock (deferred_owner_test_sync);
        if (!deferred_owner_test_request.active
            || deferred_owner_test_request.owner != this)
            return false;
        request = deferred_owner_test_request;
        deferred_owner_test_request.active = false;
        deferred_owner_test_request.owner = NULL;
        deferred_owner_test_request.session = NULL;
    }

    // Requeue the exact original request. The session's pre-reserved decision
    // seqnum keeps it alive, and the normal handler retires the socket command
    // seqnum plus the owner-progress lease on every stale/canceled path.
    send_transport_pair_owner_request (
      this, request.session, request.peer_socket_type, request.connection_id,
      request.pair_id, request.generation, request.lane);
    return true;
}
#endif

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

    if (options.type == ZLINK_CORE_SOCKET_STREAM
        && options.stream_recv_mode == ZLINK_STREAM_RECV_MODE_UNSPECIFIED) {
        errno = EINVAL;
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

int zlink::socket_base_t::connect_internal (const char *endpoint_uri_,
                                            bool process_pending_commands_)
{
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    if (options.type == ZLINK_CORE_SOCKET_STREAM
        && options.stream_recv_mode == ZLINK_STREAM_RECV_MODE_UNSPECIFIED) {
        errno = EINVAL;
        return -1;
    }

    int rc = 0;
    if (process_pending_commands_) {
        rc = process_commands (0, false);
        if (unlikely (rc != 0))
            return -1;
    }

    std::string protocol;
    std::string address;
    if (parse_uri (endpoint_uri_, protocol, address) || check_protocol (protocol)) {
        return -1;
    }

    if (protocol == protocol_name::inproc) {
        const endpoint_t peer = find_endpoint (endpoint_uri_);
        const bool paired_transport =
          options.type == ZLINK_CORE_SOCKET_DEALER || options.type == ZLINK_CORE_SOCKET_ROUTER;
        const uint64_t pair_id = paired_transport ? allocate_transport_pair_id () : 0;
        if (paired_transport)
            snapshot_pending_connect_routing_id (pair_id);
        const uint64_t pair_generation = paired_transport ? 1 : 0;
        const unsigned char transport_lane_count =
          paired_transport && peer.socket
            ? zmp_control::expected_transport_lane_count (options.type,
                                                          peer.options.type)
            : 0;
        if (paired_transport && peer.socket && transport_lane_count == 0) {
            // find_endpoint() pins the bind socket until the first bind
            // command consumes that reservation. No lane is created for an
            // incompatible peer, so release the reservation explicitly.
            send_inproc_connected (peer.socket);
            errno = EPROTONOSUPPORT;
            return -1;
        }
        // A connect-first inproc cannot know the peer type yet. It creates
        // only Application; the registry publishes the final lane count and
        // materializes Completion if the later bind reveals ROUTER-ROUTER.
        const size_t lane_count =
          peer.socket && transport_lane_count == 2u ? 2u : 1u;

        const uint64_t sndhwm = options.sndhwm;
        const uint64_t rcvhwm = options.rcvhwm;

        const bool conflate = get_effective_conflate_option (options);
        bool peer_seqnum_reserved = peer.socket != NULL;
        pipe_t *paired_application_pipe = NULL;
        for (size_t lane_index = 0; lane_index < lane_count; ++lane_index) {
            const transport_lane_t lane =
              lane_index == 0 ? transport_lane_application
                              : transport_lane_completion;
            const physical_queue_class_t queue_class =
              options.physical_queue_class == physical_queue_class_monitor
                  || (peer.socket
                      && peer.socket->options.physical_queue_class
                           == physical_queue_class_monitor)
                ? physical_queue_class_monitor
                : physical_queue_class_application;
            object_t *parents[2] = {this, peer.socket == NULL ? this : peer.socket};
            pipe_t *new_pipes[2] = {NULL, NULL};
            uint64_t hwms[2] = {
              lane == transport_lane_completion || conflate ? 0 : sndhwm,
              lane == transport_lane_completion || conflate ? 0 : rcvhwm};
            bool conflates[2] = {
              lane == transport_lane_application && conflate,
              lane == transport_lane_application && conflate};
            physical_queue_endpoint_policy_t local_attach_policy;
            physical_queue_endpoint_policy_t peer_attach_policy;
            if (lane == transport_lane_application) {
                local_attach_policy = make_auto_hwm_queue_policy (
                  std::shared_ptr<physical_queue_record_t> (), true);
            }
            if (lane == transport_lane_application && peer.socket)
                peer_attach_policy = peer.socket->make_auto_hwm_queue_policy (
                  std::shared_ptr<physical_queue_record_t> (), false);
            const bool planning_enabled =
              local_attach_policy.planning_enabled
              || peer_attach_policy.planning_enabled;
            const auto_hwm_role_t reservation_role =
              local_attach_policy.role != auto_hwm_role_none
                ? local_attach_policy.role
                : peer_attach_policy.role;
            rc = pipepair (parents, new_pipes, hwms, conflates, false, lane,
                           reservation_role, planning_enabled, queue_class);
            if (rc != 0) {
                if (peer_seqnum_reserved)
                    send_inproc_connected (peer.socket);
                if (paired_application_pipe)
                    paired_application_pipe->terminate (false);
                return -1;
            }

            // Connect-before-bind re-enters admission through a bind command,
            // so the connector direction must survive on the pipe itself.
            new_pipes[0]->set_locally_initiated (true);
            new_pipes[0]->set_transport_pair (lane, pair_id, pair_generation);
            new_pipes[1]->set_transport_pair (lane, pair_id, pair_generation);
            if (paired_transport && transport_lane_count != 0) {
                new_pipes[0]->set_transport_lane_count (transport_lane_count);
                new_pipes[1]->set_transport_lane_count (transport_lane_count);
            }
            // Inproc has no engine/session boundary that would normally
            // publish endpoint metadata. Give both pipe halves the same
            // physical connection identity before either half is attached.
            endpoint_uri_pair_t connect_endpoint_pair =
              make_unconnected_connect_endpoint_pair (endpoint_uri_);
            endpoint_uri_pair_t bind_endpoint_pair =
              make_unconnected_bind_endpoint_pair (endpoint_uri_);
            bind_endpoint_pair.connection_id =
              connect_endpoint_pair.connection_id.load ();
            new_pipes[0]->set_endpoint_pair (
              ZLINK_MOVE (connect_endpoint_pair));
            new_pipes[1]->set_endpoint_pair (
              ZLINK_MOVE (bind_endpoint_pair));
            if (lane == transport_lane_application && peer.socket) {
                get_ctx ()->record_auto_hwm_endpoint_policy (
                  make_auto_hwm_queue_policy (
                    new_pipes[0]->out_physical_queue (), true));
                get_ctx ()->record_auto_hwm_endpoint_policy (
                  peer.socket->make_auto_hwm_queue_policy (
                    new_pipes[1]->in_physical_queue (), false));
                get_ctx ()->record_auto_hwm_endpoint_policy (
                  peer.socket->make_auto_hwm_queue_policy (
                    new_pipes[1]->out_physical_queue (), true));
                get_ctx ()->record_auto_hwm_endpoint_policy (
                  make_auto_hwm_queue_policy (
                    new_pipes[0]->in_physical_queue (), false));
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
                // The Completion lane carries only ZMP reply/error/flow
                // records. A synthetic ROUTER identity frame has no
                // completion owner and is therefore a protocol error, while
                // the Application lane still needs the normal inproc routing
                // preamble.
                if (lane == transport_lane_application)
                    send_routing_id (new_pipes[0], options);
                if (paired_transport && lane == transport_lane_application) {
                    new_pipes[0]->hold_writes_until_transport_pair_ready ();
                    new_pipes[1]->hold_writes_until_transport_pair_ready ();
                }
                options_t lane_options = options;
                lane_options.transport_lane = lane;
                lane_options.transport_lane_count = 0;
                lane_options.transport_pair_id = pair_id;
                lane_options.transport_pair_generation = pair_generation;
                lane_options.transport_pair_initiator = true;
                const endpoint_t endpoint (this, lane_options);
                connected_inproc_now =
                  pend_connection (std::string (endpoint_uri_), endpoint, new_pipes);
            } else {
                if (lane == transport_lane_application
                    && peer.options.recv_routing_id)
                    send_routing_id (new_pipes[0], options);
                if (lane == transport_lane_application
                    && options.recv_routing_id)
                    send_routing_id (new_pipes[1], peer.options);

                new_pipes[0]->set_peer_routing_id (peer.options.routing_id,
                                                   peer.options.routing_id_size);
                new_pipes[1]->set_peer_routing_id (options.routing_id,
                                                   options.routing_id_size);
                new_pipes[0]->set_peer_socket_type (peer.options.type);
                new_pipes[1]->set_peer_socket_type (options.type);
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
                const int saved_errno = errno;
                const bool peer_progress_started =
                  transport_lane_count == 1u
                  && peer.socket->ensure_async_command_processing () == 0;
                errno = saved_errno;
                const bool bind_sent =
                  send_bind (peer.socket, new_pipes[1], lane_index != 0);
                const int bind_errno = errno;
                if (peer_progress_started)
                    peer.socket->request_unowned_async_command_processing_stop ();
                errno = bind_errno;
                if (!bind_sent) {
                    if (peer_seqnum_reserved)
                        send_inproc_connected (peer.socket);
                    new_pipes[0]->terminate (false);
                    new_pipes[1]->terminate (false);
                    if (paired_application_pipe)
                        paired_application_pipe->terminate (false);
                    errno = ECANCELED;
                    return -1;
                }
                if (lane_index == 0)
                    peer_seqnum_reserved = false;
                // send_bind() transfers paired pipe admission to the peer's
                // mailbox. Its attach path publishes pair readiness after
                // both lanes are validated and Router RID adoption completes;
                // reading the peer-owned RID here would race that admission.
                if (!paired_transport)
                    peer.socket->emit_inproc_connection_ready (new_pipes[1]);
                connected_inproc_now = true;
            }

            // Bind the connect-time alias to the socket-side pipe. Inproc has
            // already published the peer identity above; the alias is the local
            // route identity and must win, so this runs after that publication
            // and before admission.
            if (paired_transport && lane == transport_lane_application)
                apply_pending_connect_routing_id (pair_id, new_pipes[0]);
            attach_pipe (new_pipes[0], false, true, connected_inproc_now);
            if (connected_inproc_now)
                emit_inproc_connection_ready (new_pipes[0]);
            endpoint_runtime ().inprocs.emplace (endpoint_uri_, new_pipes[0]);
            if (paired_transport && lane == transport_lane_application)
                paired_application_pipe = new_pipes[0];
        }

        endpoint_runtime ().set_last_endpoint (endpoint_uri_);
        options.connected = true;
        return 0;
    }
    if (unlikely (0 != endpoint_runtime ().endpoints.count (endpoint_uri_)))
        return 0;

    const bool paired_transport =
      options.type == ZLINK_CORE_SOCKET_DEALER || options.type == ZLINK_CORE_SOCKET_ROUTER;
    const uint64_t pair_id = paired_transport ? allocate_transport_pair_id () : 0;
    // Capture the CONNECT_ROUTING_ID alias synchronously, bound to this
    // connect's transport pair id, before a later connect can overwrite the
    // socket-global slot.
    if (paired_transport)
        snapshot_pending_connect_routing_id (pair_id);
    const std::shared_ptr<transport_pair_state_t> pair_state =
      paired_transport ? std::make_shared<transport_pair_state_t> ()
                       : std::shared_ptr<transport_pair_state_t> ();
    options_t lane_options = options;
    if (paired_transport) {
        lane_options.zmp_metadata = true;
        lane_options.transport_lane = transport_lane_application;
        lane_options.transport_lane_count = 0;
        lane_options.transport_pair_id = pair_id;
        lane_options.transport_pair_generation = pair_state->current_generation ();
        lane_options.transport_pair_initiator = true;
        lane_options.transport_pair_state = pair_state;
    }

    const endpoint_uri_pair_t endpoint_pair =
      make_unconnected_connect_endpoint_pair (endpoint_uri_);
    const std::shared_ptr<transport_pair_connect_intent_t> intent =
      paired_transport
        ? std::make_shared<transport_pair_connect_intent_t> (
            endpoint_uri_, protocol, address, options, pair_id, pair_state)
        : std::shared_ptr<transport_pair_connect_intent_t> ();
    io_thread_t *io_thread = choose_io_thread_transport (lane_options.affinity);
    if (!io_thread) {
        errno = EMTHREAD;
        return -1;
    }
    return create_connect_session (protocol, address, endpoint_pair, io_thread,
                                   lane_options, pair_state, intent);
}

int zlink::socket_base_t::materialize_inproc_completion_lane (
  socket_base_t *bind_socket_, const options_t &bind_options_,
  const std::string &endpoint_uri_, uint64_t pair_id_,
  uint64_t pair_generation_, bool bind_side_direct_)
{
    if (!bind_socket_ || pair_id_ == 0 || pair_generation_ == 0
        || zmp_control::expected_transport_lane_count (
             options.type, bind_options_.type)
             != 2u) {
        errno = EPROTO;
        return -1;
    }

    object_t *parents[2] = {this, bind_socket_};
    pipe_t *new_pipes[2] = {NULL, NULL};
    const uint64_t hwms[2] = {0, 0};
    const bool conflates[2] = {false, false};
    if (pipepair (parents, new_pipes, hwms, conflates, false,
                  transport_lane_completion, auto_hwm_role_none, false,
                  physical_queue_class_application)
        != 0)
        return -1;

    new_pipes[0]->set_transport_pair (
      transport_lane_completion, pair_id_, pair_generation_);
    new_pipes[1]->set_transport_pair (
      transport_lane_completion, pair_id_, pair_generation_);
    new_pipes[0]->set_transport_lane_count (2);
    new_pipes[1]->set_transport_lane_count (2);
    new_pipes[0]->set_locally_initiated (true);
    endpoint_uri_pair_t connect_endpoint_pair =
      make_unconnected_connect_endpoint_pair (endpoint_uri_);
    endpoint_uri_pair_t bind_endpoint_pair =
      make_unconnected_bind_endpoint_pair (endpoint_uri_);
    bind_endpoint_pair.connection_id =
      connect_endpoint_pair.connection_id.load ();
    new_pipes[0]->set_endpoint_pair (ZLINK_MOVE (connect_endpoint_pair));
    new_pipes[1]->set_endpoint_pair (ZLINK_MOVE (bind_endpoint_pair));
    new_pipes[0]->set_max_message_bytes (
      finite_max_message_bytes (bind_options_.maxmsgsize));
    new_pipes[1]->set_max_message_bytes (
      finite_max_message_bytes (options.maxmsgsize));
    new_pipes[0]->set_peer_routing_id (bind_options_.routing_id,
                                       bind_options_.routing_id_size);
    new_pipes[1]->set_peer_routing_id (options.routing_id,
                                       options.routing_id_size);
    new_pipes[0]->set_peer_socket_type (bind_options_.type);
    new_pipes[1]->set_peer_socket_type (options.type);
    const uintptr_t bind_instance =
      reinterpret_cast<uintptr_t> (bind_socket_);
    const uintptr_t connect_instance = reinterpret_cast<uintptr_t> (this);
    new_pipes[0]->set_transport_peer_identity (
      reinterpret_cast<const unsigned char *> (&bind_instance),
      sizeof (bind_instance));
    new_pipes[1]->set_transport_peer_identity (
      reinterpret_cast<const unsigned char *> (&connect_instance),
      sizeof (connect_instance));

    bool bind_sent = false;
    if (bind_side_direct_) {
        command_t cmd;
        cmd.type = command_t::bind;
        cmd.args.bind.pipe = new_pipes[1];
        if (new_pipes[1]->retain_lifetime_ref ()) {
            bind_socket_->inc_seqnum ();
            bind_socket_->process_command (cmd);
            bind_sent = true;
        }
    } else {
        bind_sent = new_pipes[0]->send_bind (
          bind_socket_, new_pipes[1], true);
    }

    if (!bind_sent) {
        new_pipes[0]->terminate (false);
        new_pipes[1]->terminate (false);
        errno = ECANCELED;
        return -1;
    }

    attach_pipe (new_pipes[0], false, true, true);
    emit_inproc_connection_ready (new_pipes[0]);
    bind_socket_->emit_inproc_connection_ready (new_pipes[1]);
    endpoint_runtime ().inprocs.emplace (endpoint_uri_.c_str (), new_pipes[0]);
    return 0;
}

int zlink::socket_base_t::create_connect_session (
  const std::string &protocol_, const std::string &address_,
  const endpoint_uri_pair_t &endpoint_pair_, io_thread_t *io_thread_,
  const options_t &lane_options_,
  const std::shared_ptr<transport_pair_state_t> &pair_state_,
  const std::shared_ptr<transport_pair_connect_intent_t> &intent_)
{
    address_t *paddr =
      new (std::nothrow) address_t (protocol_, address_, this->get_ctx ());
    alloc_assert (paddr);
    if (resolve_connect_address (protocol_, address_, paddr) != 0) {
        LIBZLINK_DELETE (paddr);
        return -1;
    }

    return create_resolved_connect_session (
      paddr, endpoint_pair_, io_thread_, lane_options_, pair_state_, intent_);
}

int zlink::socket_base_t::create_resolved_connect_session (
  address_t *paddr, const endpoint_uri_pair_t &endpoint_pair_,
  io_thread_t *io_thread_, const options_t &lane_options_,
  const std::shared_ptr<transport_pair_state_t> &pair_state_,
  const std::shared_ptr<transport_pair_connect_intent_t> &intent_)
{
    zlink_assert (paddr);
    const bool paired_transport = pair_state_.get () != NULL;
    const bool subscribe_to_all = false;
    const uint64_t pair_id = paired_transport ? intent_->pair_id : 0;
    const uint64_t pair_generation =
      paired_transport ? pair_state_->current_generation () : 0;

    session_base_t *session = session_base_t::create (
      io_thread_, true, this, lane_options_, paddr);
    errno_assert (session);
    pipe_t *newpipe = NULL;

    if (lane_options_.immediate != 1 || subscribe_to_all) {
        object_t *parents[2] = {this, session};
        pipe_t *new_pipes[2] = {NULL, NULL};
        const bool conflate = get_effective_conflate_option (lane_options_);
        uint64_t hwms[2] = {conflate ? 0 : lane_options_.sndhwm,
                            conflate ? 0 : lane_options_.rcvhwm};
        bool conflates[2] = {conflate, conflate};
        const physical_queue_endpoint_policy_t attach_policy =
          make_auto_hwm_queue_policy (
            std::shared_ptr<physical_queue_record_t> (), true);
        const int rc = pipepair (
          parents, new_pipes, hwms, conflates, true,
          lane_options_.transport_lane, attach_policy.role,
          attach_policy.planning_enabled, physical_queue_class_application, 1);
        if (rc != 0) {
            const int pipepair_errno = errno;
            std::string failed_endpoint;
            paddr->to_string (failed_endpoint);
            event_connect_delayed (
              make_unconnected_connect_endpoint_pair (failed_endpoint),
              pipepair_errno);
            launch_child (session);
            term_child (session);
            errno = pipepair_errno;
            return -1;
        }
        new_pipes[0]->set_transport_pair (
          lane_options_.transport_lane, pair_id, pair_generation);
        new_pipes[1]->set_transport_pair (
          lane_options_.transport_lane, pair_id, pair_generation);
        if (lane_options_.transport_lane_count != 0) {
            new_pipes[0]->set_transport_lane_count (
              lane_options_.transport_lane_count);
            new_pipes[1]->set_transport_lane_count (
              lane_options_.transport_lane_count);
        }
        new_pipes[0]->set_locally_initiated (paired_transport);

        endpoint_uri_pair_t pipe_endpoint_pair = endpoint_pair_;
        pipe_endpoint_pair.connection_id = 0;
        new_pipes[0]->set_endpoint_pair (ZLINK_MOVE (pipe_endpoint_pair));

        if (!paired_transport)
            attach_pipe (new_pipes[0], subscribe_to_all, true);
        else if (lane_options_.transport_lane == transport_lane_application) {
            // Bind the connect-time alias to this pipe before admission so RID
            // adoption reads it from the pipe rather than a shared slot.
            apply_pending_connect_routing_id (pair_id, new_pipes[0]);
            new_pipes[0]->hold_writes_until_transport_pair_ready ();
            attach_pipe (new_pipes[0], subscribe_to_all, true, false);
        } else
            new_pipes[0]->set_event_sink (this);
        newpipe = new_pipes[0];
        session->attach_pipe (new_pipes[1]);
    }

    std::string last_endpoint;
    paddr->to_string (last_endpoint);
    if (!paired_transport
        || lane_options_.transport_lane == transport_lane_application)
        endpoint_runtime ().set_last_endpoint (last_endpoint);
    if (paired_transport)
        add_transport_pair_endpoint (
          endpoint_pair_, static_cast<own_t *> (session), newpipe, pair_state_,
          intent_, lane_options_.transport_lane);
    else
        add_endpoint (endpoint_pair_, static_cast<own_t *> (session), newpipe);
    return 0;
}

bool zlink::socket_base_t::socket_has_endpoint_history () const
{
    return !endpoint_runtime ().last_endpoint_uri ().empty ();
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
  const std::shared_ptr<transport_pair_state_t> &pair_state_,
  const std::shared_ptr<transport_pair_connect_intent_t> &intent_,
  transport_lane_t lane_)
{
    launch_child (endpoint_);
    endpoint_runtime ().endpoints.ZLINK_MAP_INSERT_OR_EMPLACE (
      endpoint_pair_.identifier (),
      endpoint_pipe_t (endpoint_, pipe_, endpoint_pair_.local_type, pair_state_,
                       intent_, lane_));
}

void zlink::socket_base_t::process_transport_pair_owner_request (
  session_base_t *session_, int peer_socket_type_, uint64_t connection_id_,
  uint64_t pair_id_, uint64_t generation_, unsigned char lane_)
{
    unsigned char lane_count = 0;
    int decision_error = 0;
    std::shared_ptr<transport_pair_connect_intent_t> intent;

    if (!session_
        || !session_->claim_transport_pair_owner_request (
          connection_id_, pair_id_, generation_)) {
        decision_error = ECANCELED;
    } else if (is_terminating () || _ctx_terminated) {
        decision_error = ETERM;
    } else if (connection_id_ == 0 || pair_id_ == 0
               || generation_ == 0
               || (lane_ != transport_lane_application
                   && lane_ != transport_lane_completion)) {
        decision_error = EPROTO;
    } else {
        const transport_lane_t lane =
          static_cast<transport_lane_t> (lane_);
        for (endpoints_t::iterator it = endpoint_runtime ().endpoints.begin (),
                                   end = endpoint_runtime ().endpoints.end ();
             it != end; ++it) {
            endpoint_pipe_t &entry = it->second;
            if (entry.endpoint != static_cast<own_t *> (session_)
                || entry.transport_lane != lane
                || !entry.transport_pair_state
                || !entry.transport_pair_connect_intent
                || entry.transport_pair_connect_intent->pair_id != pair_id_
                || entry.transport_pair_state
                     != entry.transport_pair_connect_intent->pair_state
                || entry.transport_pair_state->current_generation ()
                     != generation_)
                continue;
            intent = entry.transport_pair_connect_intent;
            break;
        }
        if (!intent) {
            decision_error = ECANCELED;
        } else {
            lane_count = zmp_control::expected_transport_lane_count (
              options.type, peer_socket_type_);
            if (lane_count == 0
                || (lane == transport_lane_completion && lane_count != 2u)
                || !intent->pair_state->set_expected_lane_count (lane_count))
                decision_error = EPROTO;
        }
    }

#ifdef ZLINK_BUILD_TESTS
    if (decision_error == 0) {
        const transport_pair_owner_after_claim_test_hook_fn hook =
          owner_after_claim_test_hook.load (std::memory_order_acquire);
        if (hook
            && hook (connection_id_, pair_id_, generation_,
                     owner_after_claim_test_hook_userdata.load (
                       std::memory_order_acquire))) {
            std::lock_guard<std::mutex> lock (deferred_owner_test_sync);
            zlink_assert (!deferred_owner_test_request.active);
            deferred_owner_test_request.owner = this;
            deferred_owner_test_request.session = session_;
            deferred_owner_test_request.peer_socket_type = peer_socket_type_;
            deferred_owner_test_request.connection_id = connection_id_;
            deferred_owner_test_request.pair_id = pair_id_;
            deferred_owner_test_request.generation = generation_;
            deferred_owner_test_request.lane = lane_;
            deferred_owner_test_request.active = true;
            return;
        }
    }
#endif

    address_t *prepared_completion_address = NULL;
    std::unique_ptr<options_t> completion_options;
    io_thread_t *completion_io_thread = NULL;
    endpoint_uri_pair_t completion_endpoint_pair;
    bool completion_child_exists = false;
    if (decision_error == 0 && lane_count == 2u
        && lane_ == transport_lane_application) {
        for (endpoints_t::const_iterator it =
               endpoint_runtime ().endpoints.begin (),
                                         end = endpoint_runtime ().endpoints.end ();
             it != end; ++it) {
            if (it->second.transport_pair_connect_intent == intent
                && it->second.transport_lane == transport_lane_completion) {
                completion_child_exists = true;
                break;
            }
        }
        if (!completion_child_exists) {
            completion_options.reset (
              new (std::nothrow) options_t (intent->connect_options));
            alloc_assert (completion_options.get ());
            completion_options->zmp_metadata = true;
            completion_options->transport_lane = transport_lane_completion;
            completion_options->transport_lane_count = 2;
            completion_options->transport_pair_id = intent->pair_id;
            completion_options->transport_pair_generation = generation_;
            completion_options->transport_pair_initiator = true;
            completion_options->transport_pair_state = intent->pair_state;
            completion_options->sndhwm = 0;
            completion_options->rcvhwm = 0;
            completion_options->sndbuf =
              transport_pair_policy::completion_socket_buffer (
                completion_options->sndbuf);
            completion_options->rcvbuf =
              transport_pair_policy::completion_socket_buffer (
                completion_options->rcvbuf);

            completion_io_thread =
              choose_io_thread_transport (completion_options->affinity);
            if (!completion_io_thread)
                decision_error = EMTHREAD;
        }
        if (decision_error == 0 && !completion_child_exists) {
            completion_endpoint_pair = make_unconnected_connect_endpoint_pair (
              intent->endpoint_uri.c_str ());
            prepared_completion_address = new (std::nothrow) address_t (
              intent->protocol, intent->address, this->get_ctx ());
            alloc_assert (prepared_completion_address);
            if (resolve_connect_address (
                  intent->protocol, intent->address,
                  prepared_completion_address)
                != 0) {
                LIBZLINK_DELETE (prepared_completion_address);
                decision_error = errno != 0 ? errno : EIO;
            }
        }
    }

    std::unique_lock<std::mutex> owner_commit_guard;
    std::unique_lock<std::mutex> pair_registration_guard;
    if (decision_error == 0
        && (!session_->commit_transport_pair_owner_request (
              connection_id_, pair_id_, generation_, &owner_commit_guard)
            || !intent->pair_state->acquire_owner_registration_lease (
              generation_, &pair_registration_guard))) {
        decision_error = ECANCELED;
    }

    if (decision_error == 0 && lane_count == 2u
        && lane_ == transport_lane_application) {
        if (!completion_child_exists) {
            if (create_resolved_connect_session (
                  prepared_completion_address, completion_endpoint_pair,
                  completion_io_thread, *completion_options,
                  intent->pair_state,
                  intent)
                != 0) {
                prepared_completion_address = NULL;
                decision_error = errno != 0 ? errno : EIO;
            } else {
                prepared_completion_address = NULL;
            }
        }
    }
    LIBZLINK_DELETE (prepared_completion_address);

    // Error decisions are committed too when the owner won the exact request.
    // This lets the reserved response seqnum retire normally. A timeout that
    // changed claimed to canceled first is deliberately left canceled.
    if (session_ && !owner_commit_guard.owns_lock ())
        (void) session_->commit_transport_pair_owner_request (
          connection_id_, pair_id_, generation_, &owner_commit_guard);
    if (pair_registration_guard.owns_lock ())
        pair_registration_guard.unlock ();
    if (owner_commit_guard.owns_lock ())
        owner_commit_guard.unlock ();

    send_transport_pair_owner_decision (
      session_, connection_id_, pair_id_, generation_, lane_count,
      decision_error);
}

void zlink::socket_base_t::add_endpoint (const endpoint_uri_pair_t &endpoint_pair_,
                                         own_t *endpoint_,
                                         pipe_t *pipe_)
{
    launch_child (endpoint_);
    endpoint_runtime ().endpoints.ZLINK_MAP_INSERT_OR_EMPLACE (
      endpoint_pair_.identifier (), endpoint_pipe_t (endpoint_, pipe_, endpoint_pair_.local_type));
}

void zlink::socket_base_t::terminate_inproc_pipe_with_peer_progress (pipe_t *pipe_)
{
    if (!pipe_)
        return;

    pipe_t *const peer = pipe_->retain_peer_snapshot ();
    socket_base_t *peer_socket = NULL;
    bool peer_progress_started = false;
    const int saved_errno = errno;
    const bool paired_transport =
      options.type == ZLINK_CORE_SOCKET_DEALER
      || options.type == ZLINK_CORE_SOCKET_ROUTER;
    if (paired_transport && pipe_->get_transport_lane_count () == 1u && peer
        && !peer->is_session_pipe () && peer->_sink) {
        peer->set_nodelay ();
        peer_socket = static_cast<socket_base_t *> (peer->_sink);
        peer_progress_started =
          peer_socket->ensure_async_command_processing () == 0;
    }

    pipe_->send_disconnect_msg ();
    // Explicit endpoint disconnect should not defer pipe teardown. The
    // non-inproc term_endpoint path also uses terminate(false).
    pipe_->terminate (false);

    if (peer_progress_started)
        peer_socket->request_unowned_async_command_processing_stop ();
    if (peer)
        peer->release_lifetime_ref ();
    errno = saved_errno;
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
    const auto fail_public_pending_for_endpoint =
      [this] (const std::string &identifier_) {
          socket_reqrep_internal::fail_pending_requests_for_logical_endpoint (
            request_reply_state (), identifier_);
          fail_blocking_send_waits_for_logical_endpoint (identifier_, ENOENT);
          xforget_request_route_endpoint (identifier_);
          if (options.type == ZLINK_CORE_SOCKET_PAIR) {
              fail_blocking_send_waits_for_logical_target (NULL, ENOENT);
              return;
          }
          std::vector<pipe_t *> attached;
          snapshot_attached_pipes (&attached);
          for (size_t i = 0; i != attached.size (); ++i) {
              pipe_t *const pipe = attached[i];
              if (!pipe
                  || pipe->get_endpoint_pair ().identifier () != identifier_)
                  continue;
              const blob_t &routing_id = pipe->get_routing_id ();
              if (routing_id.size () == 0)
                  continue;
              zlink_routing_id_t rid;
              memset (&rid, 0, sizeof (rid));
              copy_routing_id_from_bytes (routing_id.data (),
                                          routing_id.size (), &rid);
              fail_blocking_send_waits_for_logical_target (&rid, ENOENT);
          }
      };
    if (uri_protocol == protocol_name::inproc) {
        fail_public_pending_for_endpoint (endpoint_uri_str);
        if (unregister_endpoint (endpoint_uri_str, this) == 0) {
            std::vector<pipe_t *> attached;
            std::vector<pipe_t *> terminating;
            snapshot_attached_pipes (&attached);
            for (size_t i = 0; i != attached.size (); ++i) {
                pipe_t *const pipe = attached[i];
                if (pipe
                    && pipe->get_endpoint_pair ().identifier ()
                         == endpoint_uri_str) {
                    if (pipe->retain_lifetime_ref ())
                        terminating.push_back (pipe);
                    terminate_inproc_pipe_with_peer_progress (pipe);
                }
            }
            // Complete the local half of the inproc termination handshake
            // before returning the explicit disconnect. The peer executor
            // sends the first ack asynchronously; this socket is the current
            // public command owner and must drain that ack so the peer can
            // receive the reciprocal ack without another application call.
            const int disconnect_errno = errno;
            for (int attempt = 0; attempt != 20; ++attempt) {
                bool complete = true;
                for (size_t i = 0; i != terminating.size (); ++i)
                    complete = complete
                               && terminating[i]->has_completed_termination ();
                if (complete)
                    break;
                (void) process_commands (10, false);
            }
            for (size_t i = 0; i != terminating.size (); ++i)
                terminating[i]->release_lifetime_ref ();
            errno = disconnect_errno;
            return 0;
        }
        return endpoint_runtime ().inprocs.erase_pipes (endpoint_uri_str,
                                                        this);
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

    fail_public_pending_for_endpoint (resolved_endpoint_uri);

    for (endpoints_t::iterator it = range.first; it != range.second; ++it) {
        //  A disconnect ends the whole pair. Without this the surviving lane's
        //  session would treat the peer pipe termination as a transport failure
        //  and redial an endpoint the caller has just removed.
        if (it->second.transport_pair_state)
            it->second.transport_pair_state->disable_reconnect ();
        if (it->second.transport_pair_connect_intent)
            forget_pending_connect_routing_id (
              it->second.transport_pair_connect_intent->pair_id);
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
