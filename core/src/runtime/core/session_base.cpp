/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/macros.hpp"
#include "core/session_base.hpp"
#include "core/transport_pair_policy.hpp"
#include "engine/i_engine.hpp"
#include "utils/err.hpp"
#include "utils/env.hpp"
#include "core/pipe.hpp"
#include "utils/likely.hpp"
#include "core/address.hpp"
#include "sockets/common/socket_base.hpp"

// ASIO-only build: Transport connecters are always included
#include "transports/tcp/asio_tcp_connecter.hpp"
#if defined ZLINK_HAVE_IPC
#include "transports/ipc/asio_ipc_connecter.hpp"
#endif
#if defined ZLINK_HAVE_ASIO_SSL
#include "transports/tls/asio_tls_connecter.hpp"
#endif
#if defined ZLINK_HAVE_WS
#include "transports/ws/asio_ws_connecter.hpp"
#endif
#include "core/ctx.hpp"

namespace
{
// STREAM request/response style traffic can oscillate on stale message-credit
// updates because generic pipes only publish read progress every HWM/2 reads.
// Keep STREAM credit updates more frequent so the writer sees peer progress
// sooner, especially on same-connection echo/proxy flows.
const int stream_pipe_lwm_hint = zlink::env::positive_int ("ZLINK_STREAM_PIPE_LWM_HINT", 4);
const uint64_t stream_pipe_lwm_hint_unit_bytes = 1024;
}

zlink::session_base_t *zlink::session_base_t::create (class io_thread_t *io_thread_,
                                                      bool active_,
                                                      class socket_base_t *socket_,
                                                      const options_t &options_,
                                                      address_t *addr_)
{
    session_base_t *s = NULL;
    switch (options_.type) {
        case ZLINK_CORE_SOCKET_DEALER:
        case ZLINK_CORE_SOCKET_ROUTER:
        case ZLINK_CORE_SOCKET_PUB:
        case ZLINK_CORE_SOCKET_XPUB:
        case ZLINK_CORE_SOCKET_SUB:
        case ZLINK_CORE_SOCKET_XSUB:
        case ZLINK_CORE_SOCKET_PAIR:
        case ZLINK_CORE_SOCKET_STREAM:
            s = new (std::nothrow) session_base_t (io_thread_, active_, socket_, options_, addr_);
            break;

        default:
            errno = EINVAL;
            return NULL;
    }
    alloc_assert (s);
    return s;
}

zlink::session_base_t::session_base_t (class io_thread_t *io_thread_,
                                       bool active_,
                                       class socket_base_t *socket_,
                                       const options_t &options_,
                                       address_t *addr_) :
    own_t (io_thread_, options_),
    io_object_t (io_thread_),
    _active (active_),
    _pipe (NULL),
    _socket_pipe (NULL),
    _incomplete_in (false),
    _dropping_stale_transport_message (false),
    _pending (false),
    _engine (NULL),
    _socket (socket_),
    _pending_peer_routing_id (),
    _pending_peer_routing_id_valid (false),
    _pending_peer_socket_type (0),
      _peer_max_message_bytes (0),
      _transport_lane (options_.transport_lane),
      _transport_pair_id (options_.transport_pair_id),
      _transport_pair_generation (options_.transport_pair_generation),
      _socket_pipe_bound (false),
      _transport_pair_reconnect_in_progress (false),
      _transport_pair_owner_request_state (transport_pair_owner_idle),
      _transport_pair_owner_connection_id (0),
      _transport_pair_owner_pair_id (0),
      _transport_pair_owner_generation (0),
      _transport_pair_owner_progress_held (false),
      _io_thread (io_thread_),
    _has_linger_timer (false),
    _addr (addr_)
{
}

const zlink::endpoint_uri_pair_t &zlink::session_base_t::get_endpoint () const
{
    return _engine->get_endpoint ();
}

void zlink::session_base_t::set_peer_routing_id (const unsigned char *data_, size_t size_)
{
    //  STREAM owns its integral route in stream_t. In particular, a WS
    //  protocol identity must not overwrite the session-side route that the
    //  socket/direct-dispatch path publishes for this transport.
    if (options.type == ZLINK_CORE_SOCKET_STREAM)
        return;

    if (_pipe) {
        _pipe->set_peer_routing_id (data_, size_);
        //  Once bound, the peer endpoint belongs to the socket thread. Keep
        //  handshake publication on this session-owned endpoint; Router and
        //  STREAM publish their socket-side route on the socket thread.
        if (_socket_pipe && !_socket_pipe_bound) {
            if (_socket_pipe->get_transport_pair_id () != 0)
                _socket_pipe->set_transport_peer_identity (data_, size_);
            const bool preserve_connect_routing_id =
              _socket_pipe->is_locally_initiated ()
              && _socket_pipe->get_transport_lane ()
                   == transport_lane_application
              && _socket_pipe->get_routing_id ().size () != 0;
            if (!preserve_connect_routing_id)
                _socket_pipe->set_peer_routing_id (data_, size_);
        }
        return;
    }

    if (size_ > 0 && data_) {
        _pending_peer_routing_id.set (data_, size_);
        _pending_peer_routing_id_valid = true;
    } else {
        _pending_peer_routing_id.clear ();
        _pending_peer_routing_id_valid = false;
    }
}

void zlink::session_base_t::set_peer_socket_type (int socket_type_)
{
    _pending_peer_socket_type = socket_type_;
    if (_pipe)
        _pipe->set_peer_socket_type (socket_type_);
    if (_socket_pipe && !_socket_pipe_bound)
        _socket_pipe->set_peer_socket_type (socket_type_);
}

void zlink::session_base_t::set_peer_max_message_bytes (uint64_t max_message_bytes_)
{
    _peer_max_message_bytes = max_message_bytes_;
    if (_socket_pipe)
        _socket_pipe->set_max_message_bytes (max_message_bytes_);
}

void zlink::session_base_t::snapshot_peer_routing_id (blob_t *routing_id_) const
{
    zlink_assert (routing_id_);
    if (_pipe)
        _pipe->snapshot_routing_id (routing_id_);
    else
        routing_id_->clear ();
}

bool zlink::session_base_t::try_claim_transport_disconnected_event ()
{
    // Every established transport shares the socket endpoint's claim with
    // pipe termination. A failed handshake may not have created a pipe yet.
    if (!_socket_pipe)
        return true;
    return _socket_pipe->try_claim_transport_disconnected_event ();
}

int zlink::session_base_t::set_peer_transport_pair (transport_lane_t lane_,
                                                    uint64_t pair_id_,
                                                    uint64_t generation_)
{
    if (_transport_pair_id != 0
        && (_transport_pair_id != pair_id_
            || _transport_pair_generation != generation_
            || _transport_lane != lane_)) {
        errno = EPROTO;
        return -1;
    }
    _transport_lane = lane_;
    _transport_pair_id = pair_id_;
    _transport_pair_generation = generation_;
    if (lane_ == transport_lane_completion) {
        options.sndhwm = 0;
        options.rcvhwm = 0;
        options.sndbuf =
          transport_pair_policy::completion_socket_buffer (options.sndbuf);
        options.rcvbuf =
          transport_pair_policy::completion_socket_buffer (options.rcvbuf);
    }
    if (_pipe)
        _pipe->set_transport_pair (lane_, pair_id_, generation_);
    return 0;
}

int zlink::session_base_t::set_transport_lane_count (unsigned char lane_count_)
{
    if (lane_count_ != 1u && lane_count_ != 2u) {
        errno = EPROTO;
        return -1;
    }
    if (options.transport_lane_count != 0
        && options.transport_lane_count != lane_count_) {
        errno = EPROTO;
        return -1;
    }
    options.transport_lane_count = lane_count_;
    if (_pipe)
        _pipe->set_transport_lane_count (lane_count_);
    if (_socket_pipe)
        _socket_pipe->set_transport_lane_count (lane_count_);
    return 0;
}

int zlink::session_base_t::request_transport_pair_owner_decision (
  int peer_socket_type_)
{
    if (!is_active_transport_pair () || !_engine) {
        errno = EPROTO;
        return -1;
    }

    const uint64_t connection_id = _engine->get_endpoint ().connection_id;
    const uint64_t generation = _transport_pair_generation;
    {
        std::lock_guard<std::mutex> lock (_transport_pair_owner_sync);
        if (_transport_pair_owner_request_state == transport_pair_owner_pending
            || _transport_pair_owner_request_state
                 == transport_pair_owner_claimed
            || _transport_pair_owner_request_state
                 == transport_pair_owner_committed) {
            errno = EALREADY;
            return -1;
        }
        _transport_pair_owner_request_state = transport_pair_owner_pending;
        _transport_pair_owner_connection_id = connection_id;
        _transport_pair_owner_pair_id = _transport_pair_id;
        _transport_pair_owner_generation = generation;
    }

    if (_socket->acquire_transport_pair_owner_progress () != 0) {
        const int progress_errno = errno;
        std::lock_guard<std::mutex> lock (_transport_pair_owner_sync);
        if (_transport_pair_owner_request_state == transport_pair_owner_pending
            && _transport_pair_owner_connection_id == connection_id
            && _transport_pair_owner_pair_id == _transport_pair_id
            && _transport_pair_owner_generation == generation)
            _transport_pair_owner_request_state = transport_pair_owner_idle;
        errno = progress_errno;
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock (_transport_pair_owner_sync);
        _transport_pair_owner_progress_held = true;
    }

    // Reserve the response command before publishing the request. The socket
    // owner sends exactly one decision on every path without incrementing this
    // session again.
    inc_seqnum ();
    send_transport_pair_owner_request (
      _socket, this, peer_socket_type_, connection_id, _transport_pair_id,
      generation, static_cast<unsigned char> (_transport_lane));
    return 0;
}

bool zlink::session_base_t::claim_transport_pair_owner_request (
  uint64_t connection_id_, uint64_t pair_id_, uint64_t generation_)
{
    std::lock_guard<std::mutex> lock (_transport_pair_owner_sync);
    if (_transport_pair_owner_request_state != transport_pair_owner_pending
        || _transport_pair_owner_connection_id != connection_id_
        || _transport_pair_owner_pair_id != pair_id_
        || _transport_pair_owner_generation != generation_)
        return false;
    _transport_pair_owner_request_state = transport_pair_owner_claimed;
    return true;
}

bool zlink::session_base_t::commit_transport_pair_owner_request (
  uint64_t connection_id_, uint64_t pair_id_, uint64_t generation_,
  std::unique_lock<std::mutex> *commit_guard_out_)
{
    if (!commit_guard_out_ || commit_guard_out_->owns_lock ())
        return false;

    std::unique_lock<std::mutex> guard (_transport_pair_owner_sync);
    if (_transport_pair_owner_request_state != transport_pair_owner_claimed
        || _transport_pair_owner_connection_id != connection_id_
        || _transport_pair_owner_pair_id != pair_id_
        || _transport_pair_owner_generation != generation_)
        return false;

    _transport_pair_owner_request_state = transport_pair_owner_committed;
    *commit_guard_out_ = std::move (guard);
    return true;
}

void zlink::session_base_t::process_transport_pair_owner_decision (
  uint64_t connection_id_, uint64_t pair_id_, uint64_t generation_,
  unsigned char lane_count_, int error_number_)
{
    {
        std::lock_guard<std::mutex> lock (_transport_pair_owner_sync);
        if (_transport_pair_owner_request_state
              != transport_pair_owner_committed
            || _transport_pair_owner_connection_id != connection_id_
            || _transport_pair_owner_pair_id != pair_id_
            || _transport_pair_owner_generation != generation_)
            return;
        _transport_pair_owner_request_state = transport_pair_owner_idle;
    }

    if (!_engine || _engine->get_endpoint ().connection_id != connection_id_
        || _transport_pair_id != pair_id_
        || _transport_pair_generation != generation_) {
        release_transport_pair_owner_progress_if_held ();
        return;
    }
    _engine->transport_lane_count_decided (lane_count_, error_number_);
    if (error_number_ != 0)
        release_transport_pair_owner_progress_if_held ();
}

void zlink::session_base_t::release_transport_pair_owner_progress_if_held ()
{
    bool release = false;
    {
        std::lock_guard<std::mutex> lock (_transport_pair_owner_sync);
        if (_transport_pair_owner_progress_held) {
            _transport_pair_owner_progress_held = false;
            release = true;
        }
    }
    if (release)
        _socket->release_transport_pair_owner_progress ();
}

zlink::session_base_t::~session_base_t ()
{
    zlink_assert (!_pipe);
    zlink_assert (!_socket_pipe);

    //  If there's still a pending linger timer, remove it.
    if (_has_linger_timer) {
        cancel_timer (linger_timer_id);
        _has_linger_timer = false;
    }

    //  Close the engine.
    if (_engine)
        _engine->terminate ();

    LIBZLINK_DELETE (_addr);
}

void zlink::session_base_t::attach_pipe (pipe_t *pipe_)
{
    zlink_assert (!is_terminating ());
    zlink_assert (!_pipe);
    zlink_assert (pipe_);
    _pipe = pipe_;
    _pipe->set_event_sink (this);
    retain_socket_pipe (_pipe->get_peer ());
    // Non-paired transports attach the socket-side end before the session is
    // started. Paired DEALER/ROUTER transports defer that bind until both
    // lanes have completed and validated their handshake.
    _socket_pipe_bound = pipe_->get_transport_pair_id () == 0;
}

void zlink::session_base_t::pipe_terminated (pipe_t *pipe_)
{
    // Drop the reference to the deallocated pipe if required.
    zlink_assert (pipe_ == _pipe || _terminating_pipes.count (pipe_) == 1);

    const bool current_pipe = pipe_ == _pipe;
    if (current_pipe) {
        // If this is our current pipe, remove it
        release_socket_pipe ();
        _pipe = NULL;
        if (_has_linger_timer) {
            cancel_timer (linger_timer_id);
            _has_linger_timer = false;
        }
        if (!_pending && _engine) {
            _engine->terminate ();
            _engine = NULL;
        }
    } else
        // Remove the pipe from the detached pipes set
        _terminating_pipes.erase (pipe_);

    // raw_socket has been removed

    //  If we are waiting for pending messages to be sent, at this point
    //  we are sure that there will be no more messages and we can proceed
    //  with termination safely.
    if (_pending && !_pipe && _terminating_pipes.empty ()) {
        _pending = false;
        own_t::process_term (0);
    } else if (current_pipe && !_pending && !_pipe
               && _terminating_pipes.empty () && !is_terminating ()) {
        if (is_active_transport_pair ()
            && options.transport_pair_state->can_reconnect ())
            start_transport_pair_reconnect (false);
        else
            terminate ();
    }
}

void zlink::session_base_t::read_activated (pipe_t *pipe_)
{
    // Skip activating if we're detaching this pipe
    if (unlikely (pipe_ != _pipe)) {
        zlink_assert (_terminating_pipes.count (pipe_) == 1);
        return;
    }

    if (unlikely (_engine == NULL)) {
        if (_pipe)
            _pipe->check_read ();
        return;
    }

    _engine->restart_output ();
}

void zlink::session_base_t::write_activated (pipe_t *pipe_)
{
    // Skip activating if we're detaching this pipe
    if (_pipe != pipe_) {
        zlink_assert (_terminating_pipes.count (pipe_) == 1);
        return;
    }

    if (_engine)
        _engine->restart_input ();
}

void zlink::session_base_t::hiccuped (pipe_t *)
{
    //  Hiccups are always sent from session to socket, not the other
    //  way round.
    zlink_assert (false);
}

void zlink::session_base_t::pipe_peer_terminated (pipe_t *pipe_, bool drain_complete_)
{
    if (!drain_complete_ || pipe_ != _pipe || _pending || !_engine)
        return;

    _engine->terminate ();
    _engine = NULL;
    if (is_active_transport_pair ()
        && options.transport_pair_state->can_reconnect ())
        start_transport_pair_reconnect (false);
}

zlink::socket_base_t *zlink::session_base_t::get_socket () const
{
    return _socket;
}

void zlink::session_base_t::process_plug ()
{
    if (_active)
        start_connecting (false);
}

void zlink::session_base_t::process_attach (i_engine *engine_)
{
    zlink_assert (engine_ != NULL);
    zlink_assert (!_engine);
    _engine = engine_;

    if (!engine_->has_handshake_stage ())
        engine_ready ();

    //  Plug in the engine.
    _engine->plug (_io_thread, this);
}

void zlink::session_base_t::engine_ready ()
{
    //  Create the pipe if it does not exist yet.
    if (!_pipe && !is_terminating ()) {
        object_t *parents[2] = {this, _socket};
        pipe_t *pipes[2] = {NULL, NULL};

        const bool conflate = get_effective_conflate_option (options);

        uint64_t hwms[2] = {conflate ? 0 : options.rcvhwm,
                            conflate ? 0 : options.sndhwm};
        bool conflates[2] = {conflate, conflate};
        //  Session<->socket pipes back one transport connection; use the
        //  small per-connection chunk granularity.
        const physical_queue_endpoint_policy_t attach_policy =
          _socket->make_auto_hwm_queue_policy (
            std::shared_ptr<physical_queue_record_t> (), true);
        const int rc = pipepair (
          parents, pipes, hwms, conflates, true, _transport_lane,
          attach_policy.role, attach_policy.planning_enabled,
          physical_queue_class_application, 0);
        if (rc != 0) {
            const int pipepair_errno = errno;
            const endpoint_uri_pair_t endpoint = _engine->get_endpoint ();
            _engine->terminate ();
            _engine = NULL;
            if (_active) {
                _socket->event_connect_delayed (endpoint, pipepair_errno);
                reconnect ();
            } else {
                _socket->event_accept_failed (endpoint, pipepair_errno);
                terminate ();
            }
            errno = pipepair_errno;
            release_transport_pair_owner_progress_if_held ();
            return;
        }
        pipes[0]->set_transport_pair (
          _transport_lane, _transport_pair_id, _transport_pair_generation);
        pipes[1]->set_transport_pair (
          _transport_lane, _transport_pair_id, _transport_pair_generation);
        if (options.transport_lane_count != 0) {
            pipes[0]->set_transport_lane_count (options.transport_lane_count);
            pipes[1]->set_transport_lane_count (options.transport_lane_count);
        }
        pipes[1]->set_locally_initiated (_active);
        //  Re-apply the CONNECT_ROUTING_ID alias captured at connect time to the
        //  socket-side pipe of every (re)materialization, keyed by the stable
        //  transport pair id. Reconnect and IMMEDIATE=1 first-connect both reach
        //  the routing socket only here, so without this the replacement route
        //  would silently fall back to the peer identity.
        if (_active && _transport_lane == transport_lane_application)
            _socket->apply_pending_connect_routing_id (_transport_pair_id, pipes[1]);
        pipes[0]->set_max_message_bytes (
          options.maxmsgsize > 0 ? static_cast<uint64_t> (options.maxmsgsize) : 0);
        pipes[1]->set_max_message_bytes (_peer_max_message_bytes);
        if (_transport_pair_id != 0
            && _transport_lane == transport_lane_application)
            pipes[1]->hold_writes_until_transport_pair_ready ();

        //  Plug the local end of the pipe.
        pipes[0]->set_event_sink (this);

        if (options.type == ZLINK_CORE_SOCKET_STREAM && stream_pipe_lwm_hint > 0) {
            const uint64_t stream_lwm_hint_bytes =
              static_cast<uint64_t> (stream_pipe_lwm_hint)
              * stream_pipe_lwm_hint_unit_bytes;
            pipes[0]->set_lwm_hint (stream_lwm_hint_bytes);
            pipes[1]->set_lwm_hint (stream_lwm_hint_bytes);
        }

        //  Remember the local end of the pipe.
        zlink_assert (!_pipe);
        _pipe = pipes[0];
        retain_socket_pipe (pipes[1]);

        //  The endpoints strings are not set on bind, set them here so that
        //  events can use them.
        pipes[0]->set_endpoint_pair (_engine->get_endpoint ());
        pipes[1]->set_endpoint_pair (_engine->get_endpoint ());

        if (_pending_peer_socket_type != 0) {
            pipes[0]->set_peer_socket_type (_pending_peer_socket_type);
            pipes[1]->set_peer_socket_type (_pending_peer_socket_type);
        }

        if (_pending_peer_routing_id_valid) {
            // Publish the handshake identity to the session-owned endpoint
            // used by I/O-thread disconnect reporting, and initialize the
            // socket endpoint before send_bind transfers its ownership.
            pipes[0]->set_peer_routing_id (_pending_peer_routing_id.data (),
                                           _pending_peer_routing_id.size ());
            //  Never clobber a CONNECT_ROUTING_ID alias already bound to the
            //  socket-side pipe; the session-side pipe still carries the peer
            //  identity for disconnect reporting.
            if (pipes[1]->get_routing_id ().size () == 0)
                pipes[1]->set_peer_routing_id (_pending_peer_routing_id.data (),
                                               _pending_peer_routing_id.size ());
            if (_transport_pair_id != 0)
                pipes[1]->set_transport_peer_identity (
                  _pending_peer_routing_id.data (), _pending_peer_routing_id.size ());
            _pending_peer_routing_id.clear ();
            _pending_peer_routing_id_valid = false;
        }

    }
    // Publish the live transport identity before the socket can observe the
    // queued bind.  The socket-side pair admission may run immediately on a
    // different thread and CONNECTION_READY must never expose a zero or stale
    // connection id.
    if (_pipe)
        _pipe->set_transport_connection_id (
          _engine->get_endpoint ().connection_id);
    if (_socket_pipe)
        _socket_pipe->set_transport_connection_id (
          _engine->get_endpoint ().connection_id);
    if (_pipe && !_socket_pipe_bound && _socket_pipe) {
        send_bind (_socket, _socket_pipe);
        _socket_pipe_bound = true;
    }
    if (is_active_transport_pair ()) {
        options.transport_pair_state->mark_ready (
          _transport_lane, _transport_pair_generation);
        _transport_pair_reconnect_in_progress = false;
    }
    // The bind command was queued before this release. The socket mailbox
    // therefore admits the READY pipe and synchronizes its current control
    // state before the temporary executor is allowed to stop.
    release_transport_pair_owner_progress_if_held ();
}

void zlink::session_base_t::engine_error (bool handshaked_, zlink::i_engine::error_reason_t reason_)
{
    {
        std::lock_guard<std::mutex> lock (_transport_pair_owner_sync);
        if (_transport_pair_owner_request_state == transport_pair_owner_pending
            || _transport_pair_owner_request_state
                 == transport_pair_owner_claimed)
            _transport_pair_owner_request_state = transport_pair_owner_idle;
    }
    release_transport_pair_owner_progress_if_held ();
    if (_pipe)
        _pipe->clear_transport_connection_id_before_peer_writes ();
    //  Engine is dead. Let's forget about it.
    _engine = NULL;

    //  Remove any half-done messages from the pipes.
    if (_pipe) {
        clean_pipes ();

        //  Only send disconnect message if socket was accepted and handshake was completed
        if (!_active && handshaked_ && options.can_recv_disconnect_msg
            && !options.disconnect_msg.empty ()) {
            _pipe->set_disconnect_msg (options.disconnect_msg);
            _pipe->send_disconnect_msg ();
        }

        //  Only send hiccup message if socket was connected and handshake was completed
        if (_active && handshaked_ && options.can_recv_hiccup_msg && !options.hiccup_msg.empty ()) {
            _pipe->send_hiccup_msg (options.hiccup_msg);
        }
    }

    zlink_assert (reason_ == i_engine::connection_error || reason_ == i_engine::timeout_error
                  || reason_ == i_engine::protocol_error);

    if ((reason_ == i_engine::connection_error
         || reason_ == i_engine::timeout_error)
        && is_active_transport_pair ()
        && options.transport_pair_state->can_reconnect ()) {
        start_transport_pair_reconnect (true);
    } else {
        if (reason_ == i_engine::protocol_error && is_active_transport_pair ())
            options.transport_pair_state->disable_reconnect ();
        switch (reason_) {
        case i_engine::timeout_error:
            /* FALLTHROUGH */
        case i_engine::connection_error:
            if (_active) {
                reconnect ();
                break;
            }

        case i_engine::protocol_error:
            if (_pending) {
                if (_pipe)
                    _pipe->terminate (false);
            } else {
                terminate ();
            }
            break;
        }
    }

    //  Just in case there's only a delimiter in the pipe.
    if (_pipe)
        _pipe->check_read ();
}

void zlink::session_base_t::process_term (int linger_)
{
    zlink_assert (!_pending);
    release_transport_pair_owner_progress_if_held ();

    //  If the termination of the pipe happens before the term command is
    //  delivered there's nothing much to do. We can proceed with the
    //  standard termination immediately.
    if (!_pipe && _terminating_pipes.empty ()) {
        own_t::process_term (0);
        return;
    }

    _pending = true;

    if (_pipe != NULL) {
        //  If there's finite linger value, delay the termination.
        //  If linger is infinite (negative) we don't even have to set
        //  the timer.
        if (linger_ > 0) {
            zlink_assert (!_has_linger_timer);
            add_timer (linger_, linger_timer_id);
            _has_linger_timer = true;
        }

        //  Start pipe termination process. Delay the termination till all messages
        //  are processed in case the linger time is non-zero.
        //  Once the transport engine is gone there is no consumer that can
        //  deliver messages already queued on the socket-to-session pipe.
        //  Waiting for its delimiter would therefore retain the session's
        //  owner termination ack forever. Force the pipe handshake to drop
        //  that undeliverable tail and complete instead.
        _pipe->terminate (linger_ != 0 && _engine != NULL);

        //  In case there's no engine and there's only delimiter in the
        //  pipe it wouldn't be ever read. Thus we check for it explicitly.
        if (!_engine)
            _pipe->check_read ();
    }
}

void zlink::session_base_t::timer_event (int id_)
{
    //  Linger period expired. We can proceed with termination even though
    //  there are still pending messages to be sent.
    zlink_assert (id_ == linger_timer_id);
    _has_linger_timer = false;

    //  Ask pipe to terminate even though there may be pending messages in it.
    zlink_assert (_pipe);
    _pipe->terminate (false);
}

void zlink::session_base_t::process_conn_failed ()
{
    // Endpoint registration belongs to the original connect URI. A resolved
    // address can name another live intent on this socket (e.g. localhost
    // alongside 127.0.0.1), so it cannot identify the intent being terminated.
    std::string *ep = new std::string (
      _addr->protocol + "://" + _addr->address);
    send_term_endpoint (_socket, ep);
}

void zlink::session_base_t::reconnect ()
{
    // STREAM routes identify one physical connection. Reusing its pipe would
    // preserve the old RID and suppress the new connection's READY edge.
    // Delayed-connect sockets likewise create a pipe for each live transport.
    if (_pipe
        && (options.immediate == 1 || options.type == ZLINK_CORE_SOCKET_STREAM)) {
        if (options.type != ZLINK_CORE_SOCKET_STREAM)
            _pipe->hiccup ();
        _pipe->terminate (false);
        _terminating_pipes.insert (_pipe);
        release_socket_pipe ();
        _pipe = NULL;

        if (_has_linger_timer) {
            cancel_timer (linger_timer_id);
            _has_linger_timer = false;
        }
    }

    reset ();

    //  Reconnect.
    if (options.reconnect_ivl > 0)
        start_connecting (true);
    else
        process_conn_failed ();

    //  For subscriber sockets we hiccup the inbound pipe, which will cause
    //  the socket object to resend all the subscriptions.
    if (_pipe && (options.type == ZLINK_CORE_SOCKET_SUB || options.type == ZLINK_CORE_SOCKET_XSUB))
        _pipe->hiccup ();
}

bool zlink::session_base_t::is_active_transport_pair () const
{
    return _active && _transport_pair_id != 0
           && options.transport_pair_state.get () != NULL;
}

void zlink::session_base_t::start_transport_pair_reconnect (bool force_)
{
    if (!is_active_transport_pair ()
        || !options.transport_pair_state->can_reconnect ()
        || (_transport_pair_reconnect_in_progress && !force_))
        return;

    _transport_pair_reconnect_in_progress = true;
    _transport_pair_generation =
      options.transport_pair_state->begin_reset ();
    options.transport_pair_generation = _transport_pair_generation;

    if (_pipe) {
        pipe_t *old_pipe = _pipe;
        old_pipe->terminate (false);
        _terminating_pipes.insert (old_pipe);
        release_socket_pipe ();
        _pipe = NULL;
    }

    reset ();
    if (options.reconnect_ivl > 0)
        start_connecting (true);
    else
        process_conn_failed ();
}

void zlink::session_base_t::retain_socket_pipe (pipe_t *pipe_)
{
    zlink_assert (!_socket_pipe);
    if (!pipe_)
        return;
    if (!pipe_->retain_lifetime_ref ())
        return;
    _socket_pipe = pipe_;
}

void zlink::session_base_t::release_socket_pipe ()
{
    _socket_pipe_bound = false;
    if (!_socket_pipe)
        return;
    pipe_t *pipe = _socket_pipe;
    _socket_pipe = NULL;
    pipe->release_lifetime_ref ();
}

void zlink::session_base_t::start_connecting (bool wait_)
{
    zlink_assert (_active);

    if (is_active_transport_pair ()) {
        _transport_pair_generation =
          options.transport_pair_state->current_generation ();
        options.transport_pair_generation = _transport_pair_generation;
    }

    //  Choose I/O thread to run connecter in. Given that we are already
    //  running in an I/O thread, there must be at least one available.
    io_thread_t *io_thread = options.type == ZLINK_CORE_SOCKET_STREAM
                               ? choose_io_thread_stream (options.affinity)
                               : choose_io_thread_transport (options.affinity);
    zlink_assert (io_thread);

    //  Create the connecter object.
    own_t *connecter = NULL;
    if (_addr->protocol == protocol_name::tcp) {
        //  Use ASIO-based connecter for async_connect
        connecter =
          new (std::nothrow) asio_tcp_connecter_t (io_thread, this, options, _addr, wait_);
    }
#if defined ZLINK_HAVE_TLS && defined ZLINK_HAVE_ASIO_SSL
    else if (_addr->protocol == protocol_name::tls) {
        //  ASIO-only: Use ASIO-based TLS connecter with SSL/TLS encryption
        connecter =
          new (std::nothrow) asio_tls_connecter_t (io_thread, this, options, _addr, wait_);
    }
#endif
#if defined ZLINK_HAVE_IPC
    else if (_addr->protocol == protocol_name::ipc) {
        connecter =
          new (std::nothrow) asio_ipc_connecter_t (io_thread, this, options, _addr, wait_);
    }
#endif
#if defined ZLINK_HAVE_WS
    //  WebSocket transport (ws://, wss://)
    else if (_addr->protocol == protocol_name::ws
#if defined ZLINK_HAVE_WSS
             || _addr->protocol == protocol_name::wss
#endif
    ) {
        connecter = new (std::nothrow) asio_ws_connecter_t (io_thread, this, options, _addr, wait_);
    }
#endif
    if (connecter != NULL) {
        alloc_assert (connecter);
        launch_child (connecter);
        return;
    }

    zlink_assert (false);
}
