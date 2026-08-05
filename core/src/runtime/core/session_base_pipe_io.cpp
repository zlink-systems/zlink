/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/session_base.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"

namespace
{
const bool router_route_trace_on = zlink::debug_env_enabled ("ZLINK_DEBUG_ROUTER_ROUTE");

enum session_push_trace_event_t
{
    session_push_trace_command,
    session_push_trace_payload,
    session_push_trace_write,
    session_push_trace_write_failed
};

inline void trace_router_session_push (zlink::socket_base_t *socket_,
                                       zlink::msg_t *msg_,
                                       session_push_trace_event_t event_)
{
    if (!router_route_trace_on || !socket_
        || socket_->socket_type () != ZLINK_CORE_SOCKET_ROUTER)
        return;

    switch (event_) {
        case session_push_trace_command: {
            static std::atomic<int> g_router_command_logs (0);
            if (g_router_command_logs.fetch_add (1, std::memory_order_acq_rel) < 64)
                std::fprintf (
                  stderr, "[router-route] session push command socket=%d size=%zu flags=%u\n",
                  socket_->socket_id (), msg_->size (), static_cast<unsigned> (msg_->flags ()));
            break;
        }
        case session_push_trace_payload: {
            static std::atomic<int> g_router_payload_logs (0);
            if (g_router_payload_logs.fetch_add (1, std::memory_order_acq_rel) < 64)
                std::fprintf (
                  stderr,
                  "[router-route] session push payload socket=%d size=%zu flags=%u more=%d "
                  "routing_id=%d\n",
                  socket_->socket_id (), msg_->size (), static_cast<unsigned> (msg_->flags ()),
                  (msg_->flags () & zlink::msg_t::more) != 0 ? 1 : 0,
                  msg_->is_routing_id () ? 1 : 0);
            break;
        }
        case session_push_trace_write: {
            static std::atomic<int> g_router_write_logs (0);
            if (g_router_write_logs.fetch_add (1, std::memory_order_acq_rel) < 64)
                std::fprintf (
                  stderr,
                  "[router-route] session wrote pipe socket=%d size=%zu flags=%u more=%d "
                  "routing_id=%d\n",
                  socket_->socket_id (), msg_->size (), static_cast<unsigned> (msg_->flags ()),
                  (msg_->flags () & zlink::msg_t::more) != 0 ? 1 : 0,
                  msg_->is_routing_id () ? 1 : 0);
            break;
        }
        case session_push_trace_write_failed: {
            static std::atomic<int> g_router_write_fail_logs (0);
            if (g_router_write_fail_logs.fetch_add (1, std::memory_order_acq_rel) < 64)
                std::fprintf (
                  stderr, "[router-route] session pipe write failed socket=%d size=%zu flags=%u\n",
                  socket_->socket_id (), msg_->size (), static_cast<unsigned> (msg_->flags ()));
            break;
        }
    }
}
}

int zlink::session_base_t::pull_msg (msg_t *msg_)
{
    while (true) {
        if (!_pipe || !_pipe->read (msg_)) {
            errno = EAGAIN;
            return -1;
        }
        const bool more = (msg_->flags () & msg_t::more) != 0;
        const uint64_t stamped = msg_->transport_connection_id ();
        const uint64_t current = _pipe->get_transport_connection_id ();
        if (_dropping_stale_transport_message
            || (stamped != 0 && stamped != current)) {
            _dropping_stale_transport_message = more;
            const int close_rc = msg_->close ();
            errno_assert (close_rc == 0);
            const int init_rc = msg_->init ();
            errno_assert (init_rc == 0);
            continue;
        }
        _incomplete_in = more;
        return 0;
    }
}

int zlink::session_base_t::push_msg (msg_t *msg_)
{
    if (_pipe)
        msg_->set_transport_connection_id (
          _pipe->get_transport_connection_id ());
    if ((msg_->flags () & msg_t::command) && !msg_->is_subscribe () && !msg_->is_cancel ()) {
        trace_router_session_push (_socket, msg_, session_push_trace_command);
        if (_socket) {
            const int control_rc = _socket->peer_command_from_io (msg_, _pipe);
            if (control_rc < 0)
                return -1;
            if (control_rc > 0) {
                const int rc = msg_->close ();
                errno_assert (rc == 0);
                return msg_->init ();
            }
        }
        return 0;
    }

    trace_router_session_push (_socket, msg_, session_push_trace_payload);

    if (_socket && _socket->socket_msg_dispatch_active ()) {
        const int dispatch_rc = _socket->socket_msg_dispatch_from_io (msg_, _pipe);
        if (dispatch_rc < 0)
            return -1;
        if (dispatch_rc > 0) {
            const int rc = msg_->close ();
            errno_assert (rc == 0);
            return msg_->init ();
        }
    }

    if (options.type == ZLINK_CORE_SOCKET_STREAM && _socket && _pipe
        && _socket->stream_dispatch_active ()) {
        const int dispatch_rc = _socket->stream_dispatch_msg_from_io (msg_, _pipe);
        if (dispatch_rc < 0)
            return -1;
        if (dispatch_rc > 1)
            return 0;
        if (dispatch_rc > 0) {
            const int rc = msg_->close ();
            errno_assert (rc == 0);
            return msg_->init ();
        }
    }

    if (_pipe && _pipe->write (msg_)) {
        trace_router_session_push (_socket, msg_, session_push_trace_write);
        const int rc = msg_->init ();
        errno_assert (rc == 0);
        return 0;
    }

    trace_router_session_push (_socket, msg_, session_push_trace_write_failed);
    errno = EAGAIN;
    return -1;
}

void zlink::session_base_t::reset ()
{
}

void zlink::session_base_t::flush ()
{
    if (_pipe)
        _pipe->flush ();
}

void zlink::session_base_t::rollback ()
{
    if (_pipe)
        _pipe->rollback ();
}

void zlink::session_base_t::clean_pipes ()
{
    zlink_assert (_pipe != NULL);

    _pipe->rollback ();
    _pipe->flush ();

    while (_incomplete_in) {
        msg_t msg;
        int rc = msg.init ();
        errno_assert (rc == 0);
        rc = pull_msg (&msg);
        if (rc != 0) {
            const int saved_errno = errno;
            rc = msg.close ();
            errno_assert (rc == 0);
            if (saved_errno == EAGAIN) {
                _incomplete_in = false;
                break;
            }
            errno_assert (false);
        }
        rc = msg.close ();
        errno_assert (rc == 0);
    }
}
