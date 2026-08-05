/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "sockets/stream/stream.hpp"
#include "sockets/stream/stream_batch_policy.hpp"
#include "sockets/stream/stream_dispatch_internal.hpp"
#include "core/pipe.hpp"
#include "protocol/wire.hpp"
#include "utils/err.hpp"
#include "utils/likely.hpp"
#include <chrono>
#include <thread>

namespace
{
// STREAM echo and proxy traffic is often dominated by small payloads. A large
// mandatory batch floor improves large-frame throughput, but it also adds copy
// and queuing cost on 64B-class traffic. Keep the initial batch modest and let
// the ASIO stream encoder grow dynamically when the socket sustains larger
// bursts.
const int stream_batch_size_min = zlink::stream_batch_policy::minimum_send_batch_size ();

// Keep a small read headroom so framed application protocols are less likely
// to split at the exact payload boundary.
const int stream_batch_read_headroom = zlink::stream_batch_policy::read_headroom_bytes ();

bool is_stream_control_event (const unsigned char *payload_, size_t size_)
{
    LIBZLINK_UNUSED (payload_);
    return size_ == 0;
}

uint32_t claim_next_routing_id (std::atomic<uint32_t> &next_)
{
    while (true) {
        const uint32_t candidate = next_.fetch_add (1, std::memory_order_relaxed);
        if (candidate != 0)
            return candidate;
    }
}

uint32_t resolve_dispatch_routing_id (const zlink::msg_t *msg_, zlink::pipe_t *pipe_)
{
    if (msg_) {
        const uint32_t msg_routing_id = msg_->get_routing_id ();
        if (msg_routing_id != 0)
            return msg_routing_id;
    }

    if (!pipe_)
        return 0;

    uint32_t routing_id = pipe_->get_server_socket_routing_id ();
    if (routing_id != 0)
        return routing_id;

    const zlink::blob_t &router_routing_id = pipe_->get_routing_id ();
    if (router_routing_id.size () == 4)
        return zlink::get_uint32 (router_routing_id.data ());

    zlink::pipe_t *peer = pipe_->get_peer ();
    if (peer) {
        routing_id = peer->get_server_socket_routing_id ();
        if (routing_id != 0)
            return routing_id;

        const zlink::blob_t &peer_routing_id = peer->get_routing_id ();
        if (peer_routing_id.size () == 4)
            return zlink::get_uint32 (peer_routing_id.data ());
    }

    return 0;
}

zlink::pipe_t *resolve_connect_event_owner (zlink::pipe_t *pipe_)
{
    if (!pipe_)
        return NULL;

    zlink::pipe_t *owner = pipe_;
    zlink::pipe_t *peer = pipe_->get_peer ();
    if (peer && peer < owner)
        owner = peer;
    return owner;
}

zlink::pipe_t *resolve_direct_dispatch_output_pipe (const zlink::stream_t *socket_,
                                                    uint32_t routing_id_)
{
    if (!socket_ || !zlink::stream_dispatch_owns_socket (socket_))
        return NULL;

    if (zlink::stream_dispatch_context_t::current_routing_id () != routing_id_)
        return NULL;

    zlink::pipe_t *dispatch_pipe = zlink::stream_dispatch_context_t::current_pipe ();
    if (!dispatch_pipe)
        return NULL;

    zlink::pipe_t *out = dispatch_pipe->get_peer ();
    return out ? out : dispatch_pipe;
}

void reset_dispatched_msg (zlink::msg_t *msg_)
{
    if (!msg_)
        return;

    if (msg_->check ()) {
        if (msg_->size () == 0 && msg_->flags () == 0 && msg_->get_routing_id () == 0
            && msg_->group ()[0] == '\0') {
            return;
        }

        const int close_rc = msg_->close ();
        errno_assert (close_rc == 0);
    }

    const int init_rc = msg_->init ();
    errno_assert (init_rc == 0);
}

}

zlink::stream_t::stream_t (class ctx_t *parent_, uint32_t tid_, int sid_) :
    routing_socket_base_t (parent_, tid_, sid_),
    _prefetched (false),
    _routing_id_sent (false),
    _current_in (NULL),
    _more_in (false),
    _current_out (NULL),
    _more_out (false),
    _next_integral_routing_id (1),
    _dispatch_active (false),
    _dispatch_mode (dispatch_mode_none),
    _dispatch_inflight (0),
    _dispatch_raw_callback (NULL),
    _dispatch_msg_handler (NULL),
    _dispatch_msg_handler_userdata (NULL),
    _dispatch_packet_handler (NULL),
    _dispatch_packet_handler_userdata (NULL),
    _session_observer (NULL),
    _session_observer_userdata (NULL)
{
    options.type = ZLINK_CORE_SOCKET_STREAM;
    options.backlog = 65536;
    refresh_auto_hwm_policy ();

    const int stream_batch_size = stream_batch_size_min;
    const int stream_read_batch_size = zlink::stream_batch_policy::apply_read_headroom (
      stream_batch_size, stream_batch_read_headroom);
    options.in_batch_size = stream_read_batch_size;
    options.out_batch_size = stream_batch_size;

    _prefetched_id.init ();
    _prefetched_msg.init ();
}

zlink::stream_t::~stream_t ()
{
    _prefetched_id.close ();
    _prefetched_msg.close ();
}

zlink::stream_t::route_shard_t &zlink::stream_t::route_shard_for (uint32_t routing_id_)
{
    return _route_shards[routing_id_ % route_shard_count];
}

void zlink::stream_t::peer_routing_ids (std::vector<zlink_routing_id_t> *out_)
{
    out_->clear ();
    for (size_t i = 0; i < route_shard_count; ++i) {
        scoped_fast_lock_t shard_lock (_route_shards[i].sync);
        for (route_shard_t::routes_t::const_iterator route = _route_shards[i].routes.begin ();
             route != _route_shards[i].routes.end (); ++route) {
            zlink_routing_id_t rid;
            memset (&rid, 0, sizeof (rid));
            rid.size = sizeof (uint32_t);
            put_uint32 (rid.data, route->first);
            out_->push_back (rid);
        }
    }
}

void zlink::stream_t::set_session_observer (session_observer_fn observer_, void *userdata_)
{
    std::lock_guard<std::mutex> lock (_session_observer_mutex);
    _session_observer = observer_;
    _session_observer_userdata = userdata_;
}

void zlink::stream_t::clear_session_observer (void *userdata_)
{
    std::lock_guard<std::mutex> lock (_session_observer_mutex);
    if (_session_observer_userdata == userdata_) {
        _session_observer = NULL;
        _session_observer_userdata = NULL;
    }
}

void zlink::stream_t::notify_session_observer (uint32_t routing_id_, bool connected_)
{
    if (routing_id_ == 0)
        return;
    std::lock_guard<std::mutex> lock (_session_observer_mutex);
    if (!_session_observer)
        return;
    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    rid.size = sizeof (uint32_t);
    put_uint32 (rid.data, routing_id_);
    _session_observer (_session_observer_userdata, &rid, connected_);
}

void zlink::stream_t::xattach_pipe (pipe_t *pipe_, bool subscribe_to_all_, bool locally_initiated_)
{
    LIBZLINK_UNUSED (subscribe_to_all_);
    zlink_assert (pipe_);

    identify_peer (pipe_, locally_initiated_);
    _fq.attach (pipe_);
    maybe_emit_connect_event (pipe_);
    notify_session_observer (pipe_->get_server_socket_routing_id (), true);
}

void zlink::stream_t::xpipe_terminated (pipe_t *pipe_)
{
    zlink_assert (pipe_);

    const uint32_t server_routing_id = pipe_->get_server_socket_routing_id ();
    pipe_->reset_stream_packet_state ();

    std::lock_guard<std::recursive_mutex> api_lock (_api_mutex);
    if (pipe_ == _current_out)
        _current_out = NULL;

    if (server_routing_id != 0) {
        route_shard_t &shard = route_shard_for (server_routing_id);
        scoped_fast_lock_t shard_lock (shard.sync);
        route_shard_t::routes_t::iterator it = shard.routes.find (server_routing_id);
        if (it != shard.routes.end () && it->second == pipe_)
            shard.routes.erase (it);
    }
    erase_out_pipe (pipe_);
    _fq.pipe_terminated (pipe_);
    notify_session_observer (server_routing_id, false);
}

int zlink::stream_t::xterm_peer_rid (const zlink_routing_id_t *peer_rid_)
{
    if (!peer_rid_ || peer_rid_->size != sizeof (uint32_t)) {
        errno = EINVAL;
        return -1;
    }

    const uint32_t routing_id = get_uint32 (peer_rid_->data);
    route_shard_t &shard = route_shard_for (routing_id);
    bool terminated = false;
    {
        scoped_fast_lock_t shard_lock (shard.sync);
        route_shard_t::routes_t::iterator it = shard.routes.find (routing_id);
        if (it != shard.routes.end () && it->second) {
            // STREAM close must preserve frames that were accepted before
            // the disconnect request. The peer receives the pipe delimiter
            // only after those frames, which enables protocol-level closing
            // notifications without a timing workaround.
            it->second->terminate (true);
            terminated = true;
        }
    }
    if (terminated) {
        notify_session_observer (routing_id, false);
        return 0;
    }

    return terminate_out_pipe_by_routing_id (peer_rid_);
}

void zlink::stream_t::xread_activated (pipe_t *pipe_)
{
    _fq.activated (pipe_);
}

int zlink::stream_t::xsend (msg_t *msg_)
{
    if (!_more_out && !(msg_->flags () & msg_t::more) && msg_->get_routing_id () != 0) {
        const uint32_t routing_id = msg_->get_routing_id ();
        route_shard_t &shard = route_shard_for (routing_id);
        scoped_fast_lock_t shard_lock (shard.sync);

        route_shard_t::routes_t::iterator it = shard.routes.find (routing_id);
        if (it == shard.routes.end () || !it->second) {
            errno = EHOSTUNREACH;
            return -1;
        }

        pipe_t *out = it->second;
        if (msg_->size () == 0) {
            out->terminate (false);
        } else {
            const bool ok = out->write_single_message_and_flush_no_recursive_hwm_check (msg_);
            if (unlikely (!ok)) {
                errno = EAGAIN;
                return -1;
            }
        }

        const int init_rc = msg_->init ();
        errno_assert (init_rc == 0);
        return 0;
    }

    std::lock_guard<std::recursive_mutex> lk (_api_mutex);

    if (!_more_out) {
        zlink_assert (!_current_out);

        if (msg_->flags () & msg_t::more) {
            if (msg_->size () != 4) {
                errno = EINVAL;
                return -1;
            }

            const uint32_t routing_id = get_uint32 (static_cast<unsigned char *> (msg_->data ()));
            route_shard_t &shard = route_shard_for (routing_id);
            scoped_fast_lock_t shard_lock (shard.sync);
            route_shard_t::routes_t::iterator it = shard.routes.find (routing_id);
            if (it == shard.routes.end () || !it->second) {
                errno = EHOSTUNREACH;
                return -1;
            }

            _current_out = it->second;
            if (_current_out->check_write_status () != pipe_write_ready) {
                _current_out = NULL;
                errno = EAGAIN;
                return -1;
            }
        }

        _more_out = true;

        int rc = msg_->close ();
        errno_assert (rc == 0);
        rc = msg_->init ();
        errno_assert (rc == 0);
        return 0;
    }

    msg_->reset_flags (msg_t::more);
    _more_out = false;

    if (_current_out) {
        if (msg_->size () == 0) {
            _current_out->terminate (false);
            int rc = msg_->close ();
            errno_assert (rc == 0);
            rc = msg_->init ();
            errno_assert (rc == 0);
            _current_out = NULL;
            return 0;
        }

        const bool ok = _current_out->write_single_message_and_flush_no_recursive_hwm_check (msg_);
        if (likely (ok)) {
        } else {
            _current_out = NULL;
            const int rc = msg_->close ();
            errno_assert (rc == 0);
            errno = EAGAIN;
            return -1;
        }
        _current_out = NULL;
    } else {
        const int rc = msg_->close ();
        errno_assert (rc == 0);
    }

    const int rc = msg_->init ();
    errno_assert (rc == 0);
    return 0;
}

int zlink::stream_t::xrecv (msg_t *msg_)
{
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
        if (!_more_in)
            _current_in = NULL;
        return 0;
    }

    pipe_t *pipe = NULL;
    const int rc = _fq.recvpipe (msg_, &pipe);
    if (rc != 0)
        return -1;

    zlink_assert (pipe != NULL);

    if (_more_in) {
        _more_in = (msg_->flags () & msg_t::more) != 0;
        if (!_more_in)
            _current_in = NULL;
        return 0;
    }

    const int stash_rc = _prefetched_msg.move (*msg_);
    errno_assert (stash_rc == 0);
    _prefetched = true;
    _routing_id_sent = true;
    _current_in = pipe;

    const blob_t &routing_id = pipe->get_routing_id ();
    const int init_rc = msg_->init_size (routing_id.size ());
    errno_assert (init_rc == 0);
    if (routing_id.size () > 0)
        memcpy (msg_->data (), routing_id.data (), routing_id.size ());
    msg_->set_flags (msg_t::more);
    return 0;
}

bool zlink::stream_t::xhas_in ()
{
    return _prefetched || _fq.has_in ();
}

bool zlink::stream_t::xhas_out ()
{
    return true;
}

int zlink::stream_t::xsocket_msg_dispatch (msg_t *msg_, pipe_t *pipe_)
{
    LIBZLINK_UNUSED (msg_);
    LIBZLINK_UNUSED (pipe_);
    return 0;
}

int zlink::stream_t::xsetsockopt (int option_, const void *optval_, size_t optvallen_)
{
    if (option_ == ZLINK_INTERNAL_OPT_CONNECT_ROUTING_ID) {
        LIBZLINK_UNUSED (optval_);
        LIBZLINK_UNUSED (optvallen_);
        errno = EOPNOTSUPP;
        return -1;
    }

    return routing_socket_base_t::xsetsockopt (option_, optval_, optvallen_);
}

std::recursive_mutex *zlink::stream_t::api_sync_mutex ()
{
    return &_api_mutex;
}

uint32_t zlink::stream_t::resolve_dispatch_routing_id_fast (const msg_t *msg_, pipe_t *pipe_)
{
    if (pipe_) {
        const uint32_t pipe_routing_id = pipe_->get_server_socket_routing_id ();
        if (pipe_routing_id != 0)
            return pipe_routing_id;
    }

    if (msg_) {
        const uint32_t msg_routing_id = msg_->get_routing_id ();
        if (msg_routing_id != 0)
            return msg_routing_id;
    }

    return resolve_dispatch_routing_id (msg_, pipe_);
}

int zlink::stream_t::xstream_dispatch_msg (msg_t *msg_, pipe_t *pipe_)
{
    if (!_dispatch_active.load (std::memory_order_acquire))
        return 0;
    if (!msg_ || !pipe_)
        return 1;

    const unsigned char *payload = static_cast<const unsigned char *> (msg_->data ());
    const size_t payload_size = msg_->size ();
    if (is_stream_control_event (payload, payload_size))
        return 1;

    uint32_t routing_id_value = pipe_->get_server_socket_routing_id ();
    const bool steady_state_pipe_route = routing_id_value != 0;
    if (!steady_state_pipe_route)
        routing_id_value = resolve_dispatch_routing_id_fast (msg_, pipe_);
    if (routing_id_value == 0)
        routing_id_value = ensure_dispatch_routing_id (pipe_);
    if (routing_id_value == 0)
        return 1;

    if (!steady_state_pipe_route) {
        pipe_t *dispatch_out = pipe_->get_peer () ? pipe_->get_peer () : pipe_;
        route_shard_t &shard = route_shard_for (routing_id_value);
        scoped_fast_lock_t shard_lock (shard.sync);
        if (shard.routes.find (routing_id_value) == shard.routes.end ())
            shard.routes[routing_id_value] = dispatch_out;

        maybe_emit_connect_event (pipe_, routing_id_value);
    }

    zlink_routing_id_t rid;
    rid.size = 4;
    put_uint32 (rid.data, routing_id_value);

    const stream_dispatch_context_t dispatch_scope (this, pipe_, routing_id_value);
    switch (_dispatch_mode.load (std::memory_order_acquire)) {
        case dispatch_mode_raw:
            return stream_dispatch_raw_msg_from_io (&rid, msg_);
        case dispatch_mode_packet:
            return stream_dispatch_packet_msg_from_io (&rid, msg_, pipe_);
        default:
            break;
    }

    reset_dispatched_msg (msg_);
    return 1;
}

bool zlink::stream_t::stream_dispatch_owns_tls () const
{
    return zlink::stream_dispatch_owns_socket (this);
}

int zlink::stream_t::stream_dispatch_raw_msg_from_io (const zlink_routing_id_t *rid_, msg_t *msg_)
{
    zlink_stream_on_raw_fn raw_callback = _dispatch_raw_callback.load (std::memory_order_acquire);
    zlink_socket_msg_handler_fn handler = _dispatch_msg_handler.load (std::memory_order_acquire);

    _dispatch_inflight.fetch_add (1, std::memory_order_acq_rel);

    if (raw_callback) {
        const int cb_rc = raw_callback (rid_, reinterpret_cast<zlink_msg_t *> (msg_));
        reset_dispatched_msg (msg_);
        _dispatch_inflight.fetch_sub (1, std::memory_order_acq_rel);
        if (cb_rc != 0)
            stop_dispatch_from_callback ();
        return 2;
    }

    if (handler) {
        handler (rid_, reinterpret_cast<zlink_msg_t *> (msg_), 1,
                 _dispatch_msg_handler_userdata.load (std::memory_order_acquire));
        reset_dispatched_msg (msg_);
        _dispatch_inflight.fetch_sub (1, std::memory_order_acq_rel);
        return 2;
    }

    _dispatch_inflight.fetch_sub (1, std::memory_order_acq_rel);
    return 1;
}

int zlink::stream_t::stream_dispatch_packet_msg_from_io (const zlink_routing_id_t *rid_,
                                                         msg_t *msg_,
                                                         pipe_t *pipe_)
{
    zlink_stream_packet_handler_fn handler =
      _dispatch_packet_handler.load (std::memory_order_acquire);
    if (!handler) {
        return 1;
    }

    pipe_t::stream_packet_state_t &state = pipe_->stream_packet_state ();
    scoped_fast_lock_t packet_lock (pipe_->stream_packet_dispatch_sync ());

    const unsigned char *payload = static_cast<const unsigned char *> (msg_->data ());
    const size_t payload_size = msg_->size ();
    size_t offset = 0;

    while (offset < payload_size) {
        if (state.stage == pipe_t::stream_packet_state_t::prefix_stage) {
            const size_t remaining_prefix = sizeof (state.prefix) - state.prefix_used;
            const size_t to_copy =
              payload_size - offset < remaining_prefix ? payload_size - offset : remaining_prefix;
            if (to_copy > 0) {
                memcpy (state.prefix + state.prefix_used, payload + offset, to_copy);
                state.prefix_used += to_copy;
                offset += to_copy;
            }

            if (state.prefix_used < sizeof (state.prefix))
                continue;

            state.header_size = static_cast<size_t> (get_uint16 (state.prefix));
            state.body_size = static_cast<size_t> (get_uint32 (state.prefix + 2));

            if (options.maxmsgsize > 0) {
                const size_t limit = static_cast<size_t> (options.maxmsgsize);
                if (state.header_size > limit || state.body_size > limit
                    || state.header_size > limit - state.body_size) {
                    state.reset ();
                    reset_dispatched_msg (msg_);
                    if (pipe_) {
                        event_disconnected (pipe_->get_endpoint_pair (), EMSGSIZE,
                                            rid_ ? rid_->data : NULL, rid_ ? rid_->size : 0);
                        pipe_->terminate (false);
                    }
                    errno = EMSGSIZE;
                    return -1;
                }
            }

            if (state.header.init_size (state.header_size) != 0
                || state.body.init_size (state.body_size) != 0) {
                state.reset ();
                reset_dispatched_msg (msg_);
                if (pipe_) {
                    event_disconnected (pipe_->get_endpoint_pair (), EMSGSIZE,
                                        rid_ ? rid_->data : NULL, rid_ ? rid_->size : 0);
                    pipe_->terminate (false);
                }
                return -1;
            }

            state.header_used = 0;
            state.body_used = 0;
            state.stage = pipe_t::stream_packet_state_t::header_stage;

            if (state.header_size == 0)
                state.stage = pipe_t::stream_packet_state_t::body_stage;
        }

        if (state.stage == pipe_t::stream_packet_state_t::header_stage) {
            const size_t remaining_header = state.header_size - state.header_used;
            const size_t to_copy =
              payload_size - offset < remaining_header ? payload_size - offset : remaining_header;
            if (to_copy > 0) {
                memcpy (static_cast<unsigned char *> (state.header.data ()) + state.header_used,
                        payload + offset, to_copy);
                state.header_used += to_copy;
                offset += to_copy;
            }

            if (state.header_used < state.header_size)
                continue;

            state.stage = pipe_t::stream_packet_state_t::body_stage;
        }

        if (state.stage == pipe_t::stream_packet_state_t::body_stage) {
            const size_t remaining_body = state.body_size - state.body_used;
            const size_t to_copy =
              payload_size - offset < remaining_body ? payload_size - offset : remaining_body;
            if (to_copy > 0) {
                memcpy (static_cast<unsigned char *> (state.body.data ()) + state.body_used,
                        payload + offset, to_copy);
                state.body_used += to_copy;
                offset += to_copy;
            }

            if (state.body_used < state.body_size)
                continue;

            msg_t header_out;
            msg_t body_out;
            const int header_init_rc = header_out.init ();
            errno_assert (header_init_rc == 0);
            const int body_init_rc = body_out.init ();
            errno_assert (body_init_rc == 0);
            const int header_move_rc = header_out.move (state.header);
            errno_assert (header_move_rc == 0);
            const int body_move_rc = body_out.move (state.body);
            errno_assert (body_move_rc == 0);

            state.reset ();

            _dispatch_inflight.fetch_add (1, std::memory_order_acq_rel);
            handler (this, rid_, reinterpret_cast<zlink_msg_t *> (&header_out),
                     reinterpret_cast<zlink_msg_t *> (&body_out),
                     _dispatch_packet_handler_userdata.load (std::memory_order_acquire));
            reset_dispatched_msg (&header_out);
            reset_dispatched_msg (&body_out);
            _dispatch_inflight.fetch_sub (1, std::memory_order_acq_rel);
        }
    }

    reset_dispatched_msg (msg_);
    return 2;
}

uint32_t zlink::stream_t::ensure_dispatch_routing_id (pipe_t *pipe_)
{
    if (!pipe_)
        return 0;

    pipe_t *target = pipe_->get_peer () ? pipe_->get_peer () : pipe_;
    uint32_t routing_id_value = target->get_server_socket_routing_id ();
    if (routing_id_value != 0) {
        maybe_emit_connect_event (pipe_, routing_id_value);
        return routing_id_value;
    }

    routing_id_value = claim_next_routing_id (_next_integral_routing_id);

    unsigned char routing_buf[4];
    put_uint32 (routing_buf, routing_id_value);
    blob_t routing_id;
    routing_id.set (routing_buf, sizeof routing_buf);
    target->set_router_socket_routing_id (routing_id);
    target->set_server_socket_routing_id (routing_id_value);
    if (pipe_ != target) {
        pipe_->set_router_socket_routing_id (routing_id);
        pipe_->set_server_socket_routing_id (routing_id_value);
    }

    maybe_emit_connect_event (pipe_, routing_id_value);
    return routing_id_value;
}

void zlink::stream_t::identify_peer (pipe_t *pipe_, bool locally_initiated_)
{
    blob_t routing_id;

    uint32_t routing_id_value = pipe_->get_server_socket_routing_id ();
    if (routing_id_value == 0)
        routing_id_value = claim_next_routing_id (_next_integral_routing_id);

    unsigned char buf[4];
    put_uint32 (buf, routing_id_value);
    routing_id.set (buf, sizeof buf);

    pipe_->set_router_socket_routing_id (routing_id);
    pipe_->set_server_socket_routing_id (routing_id_value);
    pipe_t *peer = pipe_->get_peer ();
    if (peer) {
        peer->set_router_socket_routing_id (routing_id);
        peer->set_server_socket_routing_id (routing_id_value);
    }

    route_shard_t &shard = route_shard_for (routing_id_value);
    scoped_fast_lock_t shard_lock (shard.sync);
    shard.routes[routing_id_value] = pipe_;

    if (!has_out_pipe (routing_id))
        add_out_pipe (ZLINK_MOVE (routing_id), pipe_, locally_initiated_);
}

void zlink::stream_t::maybe_emit_connect_event (pipe_t *pipe_, uint32_t routing_id_value_)
{
    zlink_assert (pipe_);

    uint32_t resolved_routing_id = routing_id_value_;
    if (resolved_routing_id == 0)
        resolved_routing_id = pipe_->get_server_socket_routing_id ();
    if (resolved_routing_id == 0)
        return;

    pipe_t *event_owner = resolve_connect_event_owner (pipe_);
    if (!event_owner || !event_owner->mark_stream_connect_event_emitted ())
        return;

    unsigned char routing_id_data[4];
    put_uint32 (routing_id_data, resolved_routing_id);
    event_connection_ready_changed (pipe_->get_endpoint_pair (), routing_id_data,
                                    sizeof (routing_id_data));
}
