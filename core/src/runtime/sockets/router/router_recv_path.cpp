/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/c_api_copy_internal.hpp"
#include "utils/macros.hpp"
#include "sockets/router/router.hpp"
#include "sockets/common/socket_dispatch_loop_internal.hpp"
#include "core/pipe.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"

#include <cstdlib>
#include <cstdio>

namespace
{
const bool router_debug_on = zlink::debug_env_enabled ("ZLINK_ROUTER_DEBUG");

bool router_debug_enabled ()
{
    return router_debug_on;
}

void format_routing_id_debug (const zlink_routing_id_t *rid_, char *buf_, size_t buf_size_)
{
    if (!buf_ || buf_size_ == 0)
        return;

    if (!rid_ || rid_->size == 0) {
        std::snprintf (buf_, buf_size_, "<empty>");
        return;
    }

    size_t used = 0;
    for (size_t i = 0; i < rid_->size && used + 4 < buf_size_; ++i) {
        const unsigned char c = rid_->data[i];
        const int rc = std::snprintf (buf_ + used, buf_size_ - used, "%c%02X",
                                      (c >= 32 && c <= 126) ? static_cast<char> (c) : '.',
                                      static_cast<unsigned> (c));
        if (rc <= 0)
            break;
        used += static_cast<size_t> (rc);
        if (i + 1 < rid_->size && used + 2 < buf_size_) {
            buf_[used++] = ' ';
            buf_[used] = '\0';
        }
    }
}

void format_blob_routing_id_debug (const zlink::blob_t &routing_id_, char *buf_, size_t buf_size_)
{
    zlink_routing_id_t rid;
    zlink::copy_routing_id_from_bytes (routing_id_.data (), routing_id_.size (), &rid);
    format_routing_id_debug (&rid, buf_, buf_size_);
}

}

void zlink::router_t::copy_router_pipe_source_rid (pipe_t *pipe_,
                                                    zlink_routing_id_t *out_) const
{
    if (!out_)
        return;

    out_->size = 0;
    if (!pipe_)
        return;

    //  A reciprocal duplicate keeps its physical pipe under an internal
    //  standby ID, while the application-facing peer identity remains the
    //  original node routing ID. Request replies still retain source_pipe,
    //  so normalizing this metadata does not change the selected transport.
    const std::map<pipe_t *, blob_t>::const_iterator standby =
      _standby_pipes.find (pipe_);
    const blob_t *routing_id =
      standby != _standby_pipes.end () ? &standby->second : &pipe_->get_routing_id ();
    if (routing_id->size () > 0) {
        copy_routing_id_from_bytes (routing_id->data (), routing_id->size (), out_);
        return;
    }

    pipe_t *peer = pipe_->get_peer ();
    if (!peer)
        return;

    const blob_t &peer_routing_id = peer->get_routing_id ();
    copy_routing_id_from_bytes (peer_routing_id.data (), peer_routing_id.size (), out_);
}

void zlink::router_t::xattach_pipe (pipe_t *pipe_, bool subscribe_to_all_, bool locally_initiated_)
{
    LIBZLINK_UNUSED (subscribe_to_all_);

    zlink_assert (pipe_);

    if (_probe_router) {
        msg_t probe_msg;
        int rc = probe_msg.init ();
        errno_assert (rc == 0);

        rc = pipe_->get_transport_pair_id () != 0
               ? pipe_->write_transport_probe_and_flush (&probe_msg)
               : pipe_->write_and_flush (&probe_msg);
        LIBZLINK_UNUSED (rc);

        rc = probe_msg.close ();
        errno_assert (rc == 0);
    }

    socket_msg_dispatch_lock_t dispatch_lock = lock_socket_msg_dispatch ();
    const bool routing_id_ok = identify_peer (pipe_, locally_initiated_);
    if (router_debug_enabled ()) {
        fprintf (stderr, "router xattach_pipe: pipe=%p local=%d routing_id_ok=%d\n",
                 static_cast<void *> (pipe_), locally_initiated_ ? 1 : 0, routing_id_ok ? 1 : 0);
    }
    if (routing_id_ok) {
        _fq.attach (pipe_);
        (void) pipe_->check_read ();
        if (socket_msg_dispatch_active ())
            _fq.deactivate (pipe_);
        if (local_peer_weight () != 100)
            send_local_peer_weight (pipe_);
    } else {
        const blob_t &routing_id = pipe_->get_routing_id ();
        const out_pipe_t *const out_pipe =
          routing_id.size () > 0 ? lookup_out_pipe (routing_id) : NULL;
        pipe_t *const peer = pipe_->get_peer ();
        if (out_pipe && (out_pipe->pipe == pipe_ || out_pipe->pipe == peer)) {
            _anonymous_pipes.erase (pipe_);
            _fq.attach (pipe_);
            (void) pipe_->check_read ();
            if (socket_msg_dispatch_active ())
                _fq.deactivate (pipe_);
        } else {
            _anonymous_pipes[pipe_] = locally_initiated_;
        }
    }
}

void zlink::router_t::xread_activated (pipe_t *pipe_)
{
    socket_msg_dispatch_lock_t dispatch_lock = lock_socket_msg_dispatch ();
    const std::map<pipe_t *, bool>::iterator it = _anonymous_pipes.find (pipe_);
    if (router_debug_enabled ()) {
        char rid_text[160];
        format_blob_routing_id_debug (pipe_->get_routing_id (), rid_text, sizeof (rid_text));
        fprintf (stderr, "router xread_activated: pipe=%p anonymous=%d pipe_rid=%s\n",
                 static_cast<void *> (pipe_), it != _anonymous_pipes.end () ? 1 : 0, rid_text);
    }
    if (it == _anonymous_pipes.end ())
        _fq.activated (pipe_);
    else {
        const bool routing_id_ok = identify_peer (pipe_, it->second);
        if (router_debug_enabled ()) {
            fprintf (stderr, "router xread_activated identify_peer: pipe=%p ok=%d\n",
                     static_cast<void *> (pipe_), routing_id_ok ? 1 : 0);
        }
        if (routing_id_ok) {
            _anonymous_pipes.erase (it);
            _fq.attach (pipe_);
            (void) pipe_->check_read ();
        }
    }

    if (!socket_msg_dispatch_active ())
        return;

    msg_t msg;
    const int init_rc = msg.init ();
    errno_assert (init_rc == 0);

    pipe_t *dispatch_pipe = NULL;
    while (_fq.recvpipe (&msg, &dispatch_pipe) == 0) {
        const int dispatch_rc = xsocket_msg_dispatch (&msg, dispatch_pipe);
        if (dispatch_rc <= 0)
            break;
    }

    const int close_rc = msg.close ();
    errno_assert (close_rc == 0);
}

int zlink::router_t::xrecv (msg_t *msg_)
{
    socket_msg_dispatch_lock_t dispatch_lock = lock_socket_msg_dispatch ();
    if (_prefetched) {
        if (!_routing_id_sent) {
            const int rc = msg_->move (_prefetched_id);
            errno_assert (rc == 0);
            _routing_id_sent = true;
        } else {
            const int rc = msg_->move (_prefetched_msg);
            errno_assert (rc == 0);
            _prefetched = false;
        }
        _more_in = (msg_->flags () & msg_t::more) != 0;

        if (!_more_in) {
            if (_terminate_current_in) {
                _current_in->terminate (true);
                _terminate_current_in = false;
            }
            _current_in = NULL;
        }
        return 0;
    }

    pipe_t *pipe = NULL;
    int rc = _fq.recvpipe (msg_, &pipe);
    while (rc == 0 && msg_->is_routing_id ())
        rc = _fq.recvpipe (msg_, &pipe);

    if (rc != 0)
        return -1;

    zlink_assert (pipe != NULL);

    if (_more_in) {
        _more_in = (msg_->flags () & msg_t::more) != 0;

        if (!_more_in) {
            if (_terminate_current_in) {
                _current_in->terminate (true);
                _terminate_current_in = false;
            }
            _current_in = NULL;
        }
    } else {
        rc = _prefetched_msg.move (*msg_);
        errno_assert (rc == 0);
        _prefetched = true;
        _current_in = pipe;

        const blob_t &routing_id = pipe->get_routing_id ();
        rc = msg_->init_size (routing_id.size ());
        errno_assert (rc == 0);
        memcpy (msg_->data (), routing_id.data (), routing_id.size ());
        msg_->set_flags (msg_t::more);
        _routing_id_sent = true;
    }

    return 0;
}

int zlink::router_t::xrecv_routed (msg_t *msg_,
                                  zlink_routing_id_t *source_rid_out_,
                                  uint64_t *connection_id_out_,
                                  pipe_t **source_pipe_out_)
{
    socket_msg_dispatch_lock_t dispatch_lock = lock_socket_msg_dispatch ();
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (source_pipe_out_)
        *source_pipe_out_ = NULL;
    if (_prefetched) {
        if (source_rid_out_)
            copy_router_pipe_source_rid (_current_in, source_rid_out_);
        if (connection_id_out_ && _current_in)
            *connection_id_out_ =
              _prefetched_msg.transport_connection_id ();
        if (source_pipe_out_)
            *source_pipe_out_ = _current_in;

        const int rc = msg_->move (_prefetched_msg);
        errno_assert (rc == 0);
        _prefetched = false;
        _routing_id_sent = true;
        _more_in = (msg_->flags () & msg_t::more) != 0;

        if (!_more_in) {
            if (_terminate_current_in) {
                _current_in->terminate (true);
                _terminate_current_in = false;
            }
            _current_in = NULL;
        }
        return 0;
    }

    pipe_t *pipe = NULL;
    int rc = _fq.recvpipe (msg_, &pipe);

    while (rc == 0 && msg_->is_routing_id ())
        rc = _fq.recvpipe (msg_, &pipe);

    if (rc != 0)
        return -1;

    zlink_assert (pipe != NULL);

    if (!_more_in) {
        _current_in = pipe;
        if (source_rid_out_)
            copy_router_pipe_source_rid (pipe, source_rid_out_);
        _routing_id_sent = true;
    } else if (_current_in && source_rid_out_) {
        copy_router_pipe_source_rid (_current_in, source_rid_out_);
    }
    if (connection_id_out_) {
        *connection_id_out_ = msg_->transport_connection_id ();
    }
    if (source_pipe_out_)
        *source_pipe_out_ = _current_in;

    _more_in = (msg_->flags () & msg_t::more) != 0;

    if (!_more_in) {
        if (_terminate_current_in) {
            _current_in->terminate (true);
            _terminate_current_in = false;
        }
        _current_in = NULL;
    }

    return 0;
}

void zlink::router_t::xdispatch_io ()
{
    socket_msg_dispatch_lock_t dispatch_lock = lock_socket_msg_dispatch ();
    if (!socket_msg_dispatch_active ())
        return;
    zlink::drain_socket_dispatch_loop (
      [this] (msg_t *msg_, pipe_t **pipe_out_) { return _fq.recvpipe (msg_, pipe_out_); },
      [this] (msg_t *msg_, pipe_t *pipe_) { return xsocket_msg_dispatch (msg_, pipe_); });
}

bool zlink::router_t::xhas_in ()
{
    socket_msg_dispatch_lock_t dispatch_lock = lock_socket_msg_dispatch ();
    if (socket_msg_dispatch_active ()) {
        zlink::drain_socket_dispatch_loop (
          [this] (msg_t *msg_, pipe_t **pipe_out_) { return _fq.recvpipe (msg_, pipe_out_); },
          [this] (msg_t *msg_, pipe_t *pipe_) { return xsocket_msg_dispatch (msg_, pipe_); });
        return false;
    }

    if (_more_in)
        return true;

    if (_prefetched)
        return true;

    pipe_t *pipe = NULL;
    int rc = _fq.recvpipe (&_prefetched_msg, &pipe);

    while (rc == 0 && _prefetched_msg.is_routing_id ())
        rc = _fq.recvpipe (&_prefetched_msg, &pipe);

    if (rc != 0)
        return false;

    zlink_assert (pipe != NULL);

    const blob_t &routing_id = pipe->get_routing_id ();
    rc = _prefetched_id.init_size (routing_id.size ());
    errno_assert (rc == 0);
    memcpy (_prefetched_id.data (), routing_id.data (), routing_id.size ());
    _prefetched_id.set_flags (msg_t::more);

    _prefetched = true;
    _routing_id_sent = false;
    _current_in = pipe;

    return true;
}

int zlink::router_t::get_peer_state (const void *routing_id_, size_t routing_id_size_) const
{
    int res = 0;

    const blob_t routing_id_blob (static_cast<unsigned char *> (const_cast<void *> (routing_id_)),
                                  routing_id_size_, reference_tag_t ());
    const out_pipe_t *out_pipe = lookup_out_pipe (routing_id_blob);
    if (!out_pipe) {
        errno = EHOSTUNREACH;
        return -1;
    }

    if (out_pipe->weight == 0)
        return 0;

    if (out_pipe->pipe->check_hwm ())
        res |= ZLINK_POLLOUT;

    return res;
}
