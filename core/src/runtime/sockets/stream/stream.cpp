/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "sockets/stream/stream.hpp"
#include "sockets/stream/stream_batch_policy.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/pipe.hpp"
#include "protocol/wire.hpp"
#include "utils/err.hpp"
#include "utils/likely.hpp"
#include "utils/routing_id.hpp"
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
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

uint32_t claim_next_routing_id (std::atomic<uint32_t> &next_)
{
    while (true) {
        const uint32_t candidate = next_.fetch_add (1, std::memory_order_relaxed);
        if (candidate != 0)
            return candidate;
    }
}

void close_local_msg (zlink::msg_t *msg_)
{
    if (!msg_ || !msg_->check ())
        return;

    // Stack-local packet parts do not return to the decoder for reuse. Close
    // their current ownership state without initializing an object that is
    // about to leave scope.
    const int close_rc = msg_->close ();
    errno_assert (close_rc == 0);
}

bool stream_exact_target_identity (const zlink::pipe_t *pipe_,
                                   uint64_t *pair_id_out_,
                                   uint64_t *pair_generation_out_)
{
    if (!pipe_ || !pair_id_out_ || !pair_generation_out_)
        return false;

    uint64_t pair_id = pipe_->get_transport_pair_id ();
    uint64_t pair_generation = pipe_->get_transport_pair_generation ();
    // Raw STREAM peers have one application lane rather than the negotiated
    // DEALER/ROUTER pair. A live connection id is their non-reusable exact
    // transport identity; generation one is sufficient because the id itself
    // changes on replacement.
    if (pair_id == 0) {
        pair_id = pipe_->get_transport_connection_id ();
        pair_generation = pair_id == 0 ? 0 : 1;
    }
    if (pair_id == 0 || pair_generation == 0)
        return false;
    *pair_id_out_ = pair_id;
    *pair_generation_out_ = pair_generation;
    return true;
}

}

zlink::stream_t::packet_record_t::packet_record_t () : accounted_bytes (0)
{
    memset (&source_rid, 0, sizeof (source_rid));
    const int header_rc = header.init ();
    errno_assert (header_rc == 0);
    const int body_rc = body.init ();
    errno_assert (body_rc == 0);
}

zlink::stream_t::packet_record_t::packet_record_t (packet_record_t &&other_) :
    source_rid (other_.source_rid),
    accounted_bytes (other_.accounted_bytes)
{
    const int header_rc = header.init ();
    errno_assert (header_rc == 0);
    const int body_rc = body.init ();
    errno_assert (body_rc == 0);
    const int header_move_rc = header.move (other_.header);
    errno_assert (header_move_rc == 0);
    const int body_move_rc = body.move (other_.body);
    errno_assert (body_move_rc == 0);
    other_.accounted_bytes = 0;
    memset (&other_.source_rid, 0, sizeof (other_.source_rid));
}

zlink::stream_t::packet_record_t &zlink::stream_t::packet_record_t::operator= (
  packet_record_t &&other_)
{
    if (this == &other_)
        return *this;

    const int header_close_rc = header.close ();
    errno_assert (header_close_rc == 0);
    const int header_init_rc = header.init ();
    errno_assert (header_init_rc == 0);
    const int body_close_rc = body.close ();
    errno_assert (body_close_rc == 0);
    const int body_init_rc = body.init ();
    errno_assert (body_init_rc == 0);
    const int header_move_rc = header.move (other_.header);
    errno_assert (header_move_rc == 0);
    const int body_move_rc = body.move (other_.body);
    errno_assert (body_move_rc == 0);
    source_rid = other_.source_rid;
    accounted_bytes = other_.accounted_bytes;
    other_.accounted_bytes = 0;
    memset (&other_.source_rid, 0, sizeof (other_.source_rid));
    return *this;
}

zlink::stream_t::packet_record_t::~packet_record_t ()
{
    if (header.check ()) {
        const int header_rc = header.close ();
        errno_assert (header_rc == 0);
    }
    if (body.check ()) {
        const int body_rc = body.close ();
        errno_assert (body_rc == 0);
    }
}

zlink::stream_t::stream_t (class ctx_t *parent_, uint32_t tid_, int sid_) :
    routing_socket_base_t (parent_, tid_, sid_),
    _prefetched (false),
    _routing_id_sent (false),
    _current_in (NULL),
    _more_in (false),
    _packet_receive_accounted_bytes (0),
    _packet_pending_input_offset (0),
    _packet_pending_input_pipe (NULL),
    _packet_pending_input_valid (false),
    _packet_recv_body_ready (false),
    _current_out (NULL),
    _more_out (false),
    _next_integral_routing_id (1),
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
    _packet_pending_input.init ();
    _packet_recv_body.init ();
    memset (&_packet_pending_input_rid, 0, sizeof (_packet_pending_input_rid));
    memset (&_packet_recv_rid, 0, sizeof (_packet_recv_rid));
}

zlink::stream_t::~stream_t ()
{
    // Route entries retain their pipe endpoints so raw steady-state lookups
    // cannot race final pipe deletion. Normally xpipe_terminated drains every
    // entry; release any residual route during socket teardown.
    std::vector<pipe_t *> retained_routes;
    {
        std::lock_guard<std::mutex> publication_lock (
          _route_publication_mutex);
        for (size_t i = 0; i < route_shard_count; ++i) {
            route_shard_t &shard = _route_shards[i];
            scoped_fast_lock_t shard_lock (shard.sync);
            for (route_shard_t::routes_t::const_iterator route =
                   shard.routes.begin ();
                 route != shard.routes.end (); ++route) {
                if (route->second.pipe)
                    retained_routes.push_back (route->second.pipe);
            }
            shard.routes.clear ();
        }
    }
    for (size_t i = 0; i < retained_routes.size (); ++i)
        retained_routes[i]->release_lifetime_ref ();

    _prefetched_id.close ();
    _prefetched_msg.close ();
    clear_packet_receive_queue ();
    if (_packet_pending_input.check ())
        _packet_pending_input.close ();
    if (_packet_recv_body.check ())
        _packet_recv_body.close ();
}

zlink::stream_t::route_shard_t &zlink::stream_t::route_shard_for (uint32_t routing_id_)
{
    return _route_shards[routing_id_ % route_shard_count];
}

bool zlink::stream_t::publish_route_locked (uint32_t routing_id_,
                                            pipe_t *pipe_,
                                            bool replace_existing_,
                                            pipe_t **replaced_pipe_out_)
{
    zlink_assert (routing_id_ != 0);
    zlink_assert (pipe_);
    zlink_assert (replaced_pipe_out_);
    *replaced_pipe_out_ = NULL;
    if (pipe_->stream_route_closed ())
        return false;

    uint64_t pair_id = 0;
    uint64_t pair_generation = 0;
    const bool have_identity =
      stream_exact_target_identity (pipe_, &pair_id, &pair_generation);

    route_shard_t &shard = route_shard_for (routing_id_);
    scoped_fast_lock_t shard_lock (shard.sync);
    route_shard_t::routes_t::iterator route = shard.routes.find (routing_id_);
    if (route == shard.routes.end ()) {
        if (!pipe_->retain_lifetime_ref ())
            return false;
        route = shard.routes.insert (
          std::make_pair (routing_id_, route_entry_t (pipe_))).first;
    } else if (route->second.pipe != pipe_) {
        if (!replace_existing_ || !pipe_->retain_lifetime_ref ())
            return false;
        *replaced_pipe_out_ = route->second.pipe;
        route->second = route_entry_t (pipe_);
    }

    // The route owns the last valid exact-target identity. The engine clears
    // the pipe's live connection id before pipe termination, so this snapshot
    // is what lets termination fail only records reserved for this connection.
    if (have_identity
        && (route->second.pair_id != pair_id
            || route->second.pair_generation != pair_generation)) {
        route->second.pair_id = pair_id;
        route->second.pair_generation = pair_generation;
    }
    return true;
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

    if (!identify_peer (pipe_, locally_initiated_))
        return;
    _fq.attach (pipe_);
    // Pending logical sends may already exist when a STREAM transport arrives.
    // Emit the first writable edge after identity assignment.
    if (has_request_pending ()
        && pipe_->check_write_admission () == pipe_message_admission_ready)
        notify_request_pending_writable (pipe_);
    maybe_emit_connect_event (pipe_);
    if (options.stream_notify)
        queue_stream_notify (pipe_->get_server_socket_routing_id ());
    notify_session_observer (pipe_->get_server_socket_routing_id (), true);
}

void zlink::stream_t::xpipe_terminated (pipe_t *pipe_)
{
    zlink_assert (pipe_);

    const uint32_t server_routing_id = pipe_->get_server_socket_routing_id ();
    pipe_->close_stream_route ();

    std::vector<pipe_t *> retired_routes;
    {
        std::lock_guard<std::recursive_mutex> api_lock (_api_mutex);
        if (options.stream_notify)
            queue_stream_notify (server_routing_id);
        if (pipe_ == _current_out)
            _current_out = NULL;
        if (_packet_pending_input_valid
            && _packet_pending_input_pipe == pipe_)
            clear_pending_packet_input ();

        // A connection may briefly have a provisional alias before its attach
        // command publishes the canonical ID. Remove every route owned
        // by this endpoint in the same publication order used by insertion.
        {
            std::lock_guard<std::mutex> publication_lock (
              _route_publication_mutex);
            for (size_t shard_index = 0; shard_index < route_shard_count;
                 ++shard_index) {
                route_shard_t &shard = _route_shards[shard_index];
                scoped_fast_lock_t shard_lock (shard.sync);
                route_shard_t::routes_t::iterator route = shard.routes.begin ();
                while (route != shard.routes.end ()) {
                    if (route->second.pipe != pipe_) {
                        ++route;
                        continue;
                    }

                    retired_routes.push_back (route->second.pipe);
                    route = shard.routes.erase (route);
                }
            }
        }

        erase_out_pipe (pipe_);
        _fq.pipe_terminated (pipe_);
    }

    for (size_t i = 0; i < retired_routes.size (); ++i) {
        retired_routes[i]->release_lifetime_ref ();
    }
    notify_session_observer (server_routing_id, false);
}

int zlink::stream_t::xterm_peer_rid (const zlink_routing_id_t *peer_rid_)
{
    if (!peer_rid_ || peer_rid_->size != sizeof (uint32_t)) {
        errno = EINVAL;
        return -1;
    }

    const uint32_t routing_id = get_uint32 (peer_rid_->data);
    fail_pull_send_pending_for_logical_target (peer_rid_, ENOENT);
    route_shard_t &shard = route_shard_for (routing_id);
    bool terminated = false;
    {
        scoped_fast_lock_t shard_lock (shard.sync);
        route_shard_t::routes_t::iterator it = shard.routes.find (routing_id);
        if (it != shard.routes.end () && it->second.pipe) {
            // STREAM close must preserve frames that were accepted before
            // the disconnect request. The peer receives the pipe delimiter
            // only after those frames, which enables protocol-level closing
            // notifications without a timing workaround.
            it->second.pipe->terminate (true);
            terminated = true;
        }
    }
    if (terminated) {
        notify_session_observer (routing_id, false);
        return 0;
    }

    return terminate_out_pipe_by_routing_id (peer_rid_);
}

bool zlink::stream_t::packet_queue_at_limit () const
{
    return options.rcvhwm != 0
           && _packet_receive_accounted_bytes >= options.rcvhwm;
}

int zlink::stream_t::enqueue_packet (const zlink_routing_id_t &source_rid_,
                                     msg_t *header_,
                                     msg_t *body_)
{
    if (!header_ || !body_) {
        errno = EFAULT;
        return -1;
    }

    const uint64_t header_charge = pipe_t::frame_accounted_bytes (header_);
    const uint64_t body_charge = pipe_t::frame_accounted_bytes (body_);
    const uint64_t charge =
      header_charge == UINT64_MAX || body_charge == UINT64_MAX
          || UINT64_MAX - header_charge < body_charge
        ? UINT64_MAX
        : header_charge + body_charge;

    try {
        _packet_receive_queue.emplace_back ();
    } catch (const std::bad_alloc &) {
        errno = ENOMEM;
        return -1;
    }

    packet_record_t &record = _packet_receive_queue.back ();
    record.source_rid = source_rid_;
    record.accounted_bytes = charge;
    const int header_move_rc = record.header.move (*header_);
    errno_assert (header_move_rc == 0);
    const int body_move_rc = record.body.move (*body_);
    errno_assert (body_move_rc == 0);
    _packet_receive_accounted_bytes =
      _packet_receive_accounted_bytes == UINT64_MAX || charge == UINT64_MAX
          || UINT64_MAX - _packet_receive_accounted_bytes < charge
        ? UINT64_MAX
        : _packet_receive_accounted_bytes + charge;
    return 0;
}

int zlink::stream_t::decode_packet_bytes (const zlink_routing_id_t &source_rid_,
                                          msg_t *raw_,
                                          pipe_t *source_pipe_,
                                          size_t start_offset_,
                                          size_t *next_offset_out_)
{
    if (!raw_ || !source_pipe_ || !next_offset_out_ || start_offset_ > raw_->size ()) {
        errno = EFAULT;
        return -1;
    }

    pipe_t::stream_packet_state_t &state = source_pipe_->stream_packet_state ();
    const unsigned char *payload = static_cast<const unsigned char *> (raw_->data ());
    const size_t payload_size = raw_->size ();
    size_t offset = start_offset_;
    *next_offset_out_ = start_offset_;

    const auto packet_total_size = [&] (size_t header_size_, size_t body_size_,
                                        size_t *total_size_) {
        if (header_size_ > std::numeric_limits<size_t>::max () - body_size_)
            return false;
        const size_t total_size = header_size_ + body_size_;
        const uint64_t limit = options.maxmsgsize > 0
                                 ? static_cast<uint64_t> (options.maxmsgsize)
                                 : 0;
        if (limit != 0
            && (static_cast<uint64_t> (header_size_) > limit
                || static_cast<uint64_t> (body_size_) > limit
                || static_cast<uint64_t> (total_size) > limit))
            return false;
        *total_size_ = total_size;
        return true;
    };
    const auto fail_packet = [&] (int failure_errno_) {
        state.reset ();
        event_disconnected (source_pipe_->get_endpoint_pair (), failure_errno_,
                            source_rid_.data, source_rid_.size);
        source_pipe_->terminate (false);
        _fq.deactivate (source_pipe_);
        errno = failure_errno_;
        return -1;
    };
    const auto enqueue_parts = [&] (msg_t *header_, msg_t *body_) {
        if (enqueue_packet (source_rid_, header_, body_) != 0) {
            const int saved_errno = errno;
            close_local_msg (header_);
            close_local_msg (body_);
            return fail_packet (saved_errno);
        }
        close_local_msg (header_);
        close_local_msg (body_);
        return 0;
    };
    const auto enqueue_completed_state = [&] () {
        msg_t header_out;
        msg_t body_out;
        const int header_init_rc = header_out.init ();
        errno_assert (header_init_rc == 0);
        const int body_init_rc = body_out.init ();
        errno_assert (body_init_rc == 0);

        if (state.storage == pipe_t::stream_packet_state_t::coalesced_storage) {
            const int header_view_rc =
              header_out.init_view (state.header, 0, state.header_size);
            int body_view_rc = 0;
            if (header_view_rc == 0) {
#ifdef ZLINK_BUILD_TESTS
                if (test_consume_stream_packet_allocation_failpoint (
                      stream_packet_allocation_body_view)) {
                    errno = ENOMEM;
                    body_view_rc = -1;
                } else
#endif
                    body_view_rc = body_out.init_view (
                      state.header, state.header_size, state.body_size);
            }
            if (header_view_rc != 0 || body_view_rc != 0) {
                const int saved_errno = errno;
                close_local_msg (&header_out);
                close_local_msg (&body_out);
                return fail_packet (saved_errno);
            }
        } else {
            const int header_move_rc = header_out.move (state.header);
            errno_assert (header_move_rc == 0);
            const int body_move_rc = body_out.move (state.body);
            errno_assert (body_move_rc == 0);
        }

        state.reset ();
        return enqueue_parts (&header_out, &body_out);
    };

    while (offset < payload_size) {
        if (state.stage == pipe_t::stream_packet_state_t::prefix_stage
            && state.prefix_used == 0) {
            const size_t available = payload_size - offset;
            if (available >= sizeof (state.prefix)) {
                const size_t header_size =
                  static_cast<size_t> (get_uint16 (payload + offset));
                const size_t body_size =
                  static_cast<size_t> (get_uint32 (payload + offset + 2));
                size_t packet_size = 0;
                if (!packet_total_size (header_size, body_size, &packet_size))
                    return fail_packet (EMSGSIZE);
                LIBZLINK_UNUSED (packet_size);

                const size_t after_prefix = available - sizeof (state.prefix);
                if (header_size <= after_prefix) {
                    const size_t after_header = after_prefix - header_size;
                    if (body_size <= after_header) {
                        msg_t header_out;
                        msg_t body_out;
                        const int body_init_rc = body_out.init ();
                        errno_assert (body_init_rc == 0);
                        const size_t header_offset = offset + sizeof (state.prefix);
                        const size_t body_offset = header_offset + header_size;
                        int header_part_rc = 0;
                        if (header_size <= msg_t::max_vsm_size) {
                            header_part_rc = header_out.init_buffer (
                              payload + header_offset, header_size);
                        } else {
                            const int header_init_rc = header_out.init ();
                            errno_assert (header_init_rc == 0);
                            header_part_rc =
                              header_out.init_view (*raw_, header_offset, header_size);
                        }
                        if (header_part_rc != 0
                            || body_out.init_view (*raw_, body_offset, body_size) != 0) {
                            const int saved_errno = errno;
                            close_local_msg (&header_out);
                            close_local_msg (&body_out);
                            return fail_packet (saved_errno);
                        }

                        offset = body_offset + body_size;
                        if (enqueue_parts (&header_out, &body_out) != 0)
                            return -1;
                        if (packet_queue_at_limit ())
                            break;
                        continue;
                    }
                }
            }
        }

        if (state.stage == pipe_t::stream_packet_state_t::prefix_stage) {
            const size_t remaining_prefix = sizeof (state.prefix) - state.prefix_used;
            const size_t to_copy =
              payload_size - offset < remaining_prefix ? payload_size - offset
                                                       : remaining_prefix;
            if (to_copy > 0) {
                memcpy (state.prefix + state.prefix_used, payload + offset, to_copy);
                state.prefix_used += to_copy;
                offset += to_copy;
            }
            if (state.prefix_used < sizeof (state.prefix))
                continue;

            state.header_size = static_cast<size_t> (get_uint16 (state.prefix));
            state.body_size = static_cast<size_t> (get_uint32 (state.prefix + 2));
            size_t packet_size = 0;
            if (!packet_total_size (state.header_size, state.body_size, &packet_size))
                return fail_packet (EMSGSIZE);

            int allocation_rc = 0;
            if (state.header_size > msg_t::max_vsm_size
                && state.body_size > msg_t::max_vsm_size) {
                state.storage = pipe_t::stream_packet_state_t::coalesced_storage;
#ifdef ZLINK_BUILD_TESTS
                if (test_consume_stream_packet_allocation_failpoint (
                      stream_packet_allocation_backing)) {
                    errno = ENOMEM;
                    allocation_rc = -1;
                } else
#endif
                    allocation_rc = state.header.init_size (packet_size);
            } else {
                allocation_rc = state.header.init_size (state.header_size);
                if (allocation_rc == 0)
                    allocation_rc = state.body.init_size (state.body_size);
            }
            if (allocation_rc != 0) {
                const int saved_errno = errno;
                return fail_packet (saved_errno);
            }

            state.header_used = 0;
            state.body_used = 0;
            state.stage = pipe_t::stream_packet_state_t::header_stage;
            if (state.header_size == 0)
                state.stage = pipe_t::stream_packet_state_t::body_stage;
            if (state.header_size == 0 && state.body_size == 0) {
                if (enqueue_completed_state () != 0)
                    return -1;
                if (packet_queue_at_limit ())
                    break;
                continue;
            }
        }

        if (state.stage == pipe_t::stream_packet_state_t::header_stage) {
            const size_t remaining_header = state.header_size - state.header_used;
            const size_t to_copy =
              payload_size - offset < remaining_header ? payload_size - offset
                                                       : remaining_header;
            if (to_copy > 0) {
                unsigned char *const header_data =
                  static_cast<unsigned char *> (state.header.data ());
                memcpy (header_data + state.header_used, payload + offset, to_copy);
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
              payload_size - offset < remaining_body ? payload_size - offset
                                                     : remaining_body;
            if (to_copy > 0) {
                unsigned char *const body_data = static_cast<unsigned char *> (
                  state.storage == pipe_t::stream_packet_state_t::coalesced_storage
                    ? state.header.data ()
                    : state.body.data ());
                const size_t body_offset =
                  state.storage == pipe_t::stream_packet_state_t::coalesced_storage
                    ? state.header_size
                    : 0;
                memcpy (body_data + body_offset + state.body_used,
                        payload + offset, to_copy);
                state.body_used += to_copy;
                offset += to_copy;
            }
            if (state.body_used < state.body_size)
                continue;
            if (enqueue_completed_state () != 0)
                return -1;
            if (packet_queue_at_limit ())
                break;
        }
    }

    *next_offset_out_ = offset;
    return 0;
}

void zlink::stream_t::clear_pending_packet_input ()
{
    if (_packet_pending_input.check ()) {
        const int close_rc = _packet_pending_input.close ();
        errno_assert (close_rc == 0);
        const int init_rc = _packet_pending_input.init ();
        errno_assert (init_rc == 0);
    }
    _packet_pending_input_offset = 0;
    _packet_pending_input_pipe = NULL;
    _packet_pending_input_valid = false;
    memset (&_packet_pending_input_rid, 0, sizeof (_packet_pending_input_rid));
}

void zlink::stream_t::clear_packet_receive_queue ()
{
    _packet_receive_queue.clear ();
    _packet_receive_accounted_bytes = 0;
}

int zlink::stream_t::pump_packet_receive_queue ()
{
    const size_t max_raw_chunks_per_pump = 64;
    size_t raw_chunks = 0;
    while (!packet_queue_at_limit () && raw_chunks < max_raw_chunks_per_pump) {
        msg_t raw;
        bool local_raw = false;
        pipe_t *source_pipe = NULL;
        zlink_routing_id_t source_rid;
        memset (&source_rid, 0, sizeof (source_rid));
        size_t start_offset = 0;

        if (_packet_pending_input_valid) {
            source_pipe = _packet_pending_input_pipe;
            source_rid = _packet_pending_input_rid;
            start_offset = _packet_pending_input_offset;
        } else {
            const int init_rc = raw.init ();
            errno_assert (init_rc == 0);
            local_raw = true;
            if (_fq.recvpipe (&raw, &source_pipe) != 0) {
                const int saved_errno = errno;
                const int close_rc = raw.close ();
                errno_assert (close_rc == 0);
                errno = saved_errno;
                return 0;
            }
            if (!source_pipe) {
                const int close_rc = raw.close ();
                errno_assert (close_rc == 0);
                errno = EAGAIN;
                return 0;
            }
            const uint32_t routing_id = source_pipe->get_server_socket_routing_id ();
            if (routing_id == 0) {
                const int close_rc = raw.close ();
                errno_assert (close_rc == 0);
                source_pipe->terminate (false);
                _fq.deactivate (source_pipe);
                ++raw_chunks;
                continue;
            }
            source_rid.size = sizeof (routing_id);
            put_uint32 (source_rid.data, routing_id);
        }

        msg_t *const input =
          _packet_pending_input_valid ? &_packet_pending_input : &raw;
        size_t next_offset = start_offset;
        const int decode_rc = decode_packet_bytes (
          source_rid, input, source_pipe, start_offset, &next_offset);
        if (decode_rc != 0) {
            if (_packet_pending_input_valid)
                clear_pending_packet_input ();
            if (local_raw) {
                const int saved_errno = errno;
                const int close_rc = raw.close ();
                errno_assert (close_rc == 0);
                errno = saved_errno;
            }
            ++raw_chunks;
            continue;
        }

        if (next_offset < input->size ()) {
            if (!_packet_pending_input_valid) {
                const int move_rc = _packet_pending_input.move (raw);
                errno_assert (move_rc == 0);
                _packet_pending_input_pipe = source_pipe;
                _packet_pending_input_rid = source_rid;
                _packet_pending_input_valid = true;
            }
            _packet_pending_input_offset = next_offset;
            return 0;
        }

        if (_packet_pending_input_valid)
            clear_pending_packet_input ();
        if (local_raw) {
            const int close_rc = raw.close ();
            errno_assert (close_rc == 0);
        }
        ++raw_chunks;
    }
    return 0;
}

int zlink::stream_t::xrecv_packet_header (msg_t *header_out_)
{
    if (_packet_recv_body_ready) {
        errno = EBUSY;
        return -1;
    }
    if (_packet_receive_queue.empty ())
        (void) pump_packet_receive_queue ();
    if (_packet_receive_queue.empty ()) {
        errno = EAGAIN;
        return -1;
    }

    packet_record_t &record = _packet_receive_queue.front ();
    const int header_move_rc = header_out_->move (record.header);
    errno_assert (header_move_rc == 0);
    const int body_move_rc = _packet_recv_body.move (record.body);
    errno_assert (body_move_rc == 0);
    _packet_recv_rid = record.source_rid;
    _packet_recv_body_ready = true;
    _packet_receive_accounted_bytes =
      record.accounted_bytes >= _packet_receive_accounted_bytes
        ? 0
        : _packet_receive_accounted_bytes - record.accounted_bytes;
    _packet_receive_queue.pop_front ();

    // Refill immediately after releasing queue credit so a packet already
    // buffered in a raw chunk keeps POLLIN level-triggered without waiting for
    // a transport activation that has already happened.
    (void) pump_packet_receive_queue ();
    return 0;
}

int zlink::stream_t::recv_packet (zlink_routing_id_t *source_rid_out_,
                                  msg_t *header_out_,
                                  msg_t *body_out_,
                                  int flags_)
{
    if (options.stream_recv_mode != ZLINK_STREAM_RECV_MODE_PACKET) {
        errno = ENOTSUP;
        return -1;
    }
    if (!source_rid_out_ || !header_out_ || !body_out_) {
        errno = EFAULT;
        return -1;
    }

    const int recv_rc = socket_base_t::recv (header_out_, flags_);
    if (recv_rc != 0)
        return -1;
    if (!_packet_recv_body_ready) {
        errno = EIO;
        return -1;
    }

    const int body_move_rc = body_out_->move (_packet_recv_body);
    errno_assert (body_move_rc == 0);
    *source_rid_out_ = _packet_recv_rid;
    _packet_recv_body_ready = false;
    memset (&_packet_recv_rid, 0, sizeof (_packet_recv_rid));
    return 0;
}

void zlink::stream_t::xread_activated (pipe_t *pipe_)
{
    _fq.activated (pipe_);
}

int zlink::stream_t::xsend (
  msg_t *msg_, pipe_message_admission_t *admission_out_)
{
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    if (!_more_out && !(msg_->flags () & msg_t::more) && msg_->get_routing_id () != 0) {
        const uint32_t routing_id = msg_->get_routing_id ();
        route_shard_t &shard = route_shard_for (routing_id);
        scoped_fast_lock_t shard_lock (shard.sync);

        route_shard_t::routes_t::iterator it = shard.routes.find (routing_id);
        if (it == shard.routes.end () || !it->second.pipe) {
            errno = EHOSTUNREACH;
            return -1;
        }

        pipe_t *out = it->second.pipe;
        if (msg_->size () == 0) {
            out->terminate (false);
        } else {
            pipe_message_admission_t write_admission =
              pipe_message_admission_invalid;
            const bool ok =
              out->write_single_message_and_flush_no_recursive_hwm_check (
                msg_, &write_admission);
            if (unlikely (!ok)) {
                if (admission_out_)
                    *admission_out_ = write_admission;
                errno = EAGAIN;
                return -1;
            }
        }

        const int init_rc = msg_->init ();
        errno_assert (init_rc == 0);
        if (admission_out_)
            *admission_out_ = pipe_message_admission_ready;
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
            if (it == shard.routes.end () || !it->second.pipe) {
                errno = EHOSTUNREACH;
                return -1;
            }

            _current_out = it->second.pipe;
            const pipe_message_admission_t write_admission =
              _current_out->check_write_admission ();
            if (write_admission != pipe_message_admission_ready) {
                if (admission_out_)
                    *admission_out_ = write_admission;
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
        if (admission_out_)
            *admission_out_ = pipe_message_admission_ready;
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
            if (admission_out_)
                *admission_out_ = pipe_message_admission_ready;
            return 0;
        }

        pipe_message_admission_t write_admission =
          pipe_message_admission_invalid;
        const bool ok =
          _current_out->write_single_message_and_flush_no_recursive_hwm_check (
            msg_, &write_admission);
        if (likely (ok)) {
        } else {
            if (admission_out_)
                *admission_out_ = write_admission;
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
    if (admission_out_)
        *admission_out_ = pipe_message_admission_ready;
    return 0;
}

int zlink::stream_t::xsend_routed (
  const zlink_routing_id_t *target_rid_, msg_t *msg_,
  uint64_t *connection_id_out_, uint64_t expected_connection_id_,
  pipe_t **pipe_out_, uint64_t expected_transport_pair_id_,
  uint64_t expected_transport_pair_generation_,
  pipe_message_admission_t *admission_out_,
  pipe_write_observer_fn observer_, void *observer_userdata_,
  routed_send_attempt_identity_t *attempt_identity_out_,
  uint64_t expected_route_incarnation_id_, bool request_only_)
{
    LIBZLINK_UNUSED (expected_connection_id_);
    LIBZLINK_UNUSED (observer_);
    LIBZLINK_UNUSED (observer_userdata_);
    LIBZLINK_UNUSED (expected_route_incarnation_id_);
    LIBZLINK_UNUSED (request_only_);
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (pipe_out_)
        *pipe_out_ = NULL;
    if (attempt_identity_out_)
        attempt_identity_out_->reset ();
    if (admission_out_)
        *admission_out_ = pipe_message_admission_invalid;
    if (!target_rid_ || target_rid_->size != sizeof (uint32_t)
        || !msg_ || (msg_->flags () & msg_t::more)) {
        errno = EINVAL;
        return -1;
    }

    const uint32_t routing_id = get_uint32 (target_rid_->data);
    route_shard_t &shard = route_shard_for (routing_id);
    scoped_fast_lock_t shard_lock (shard.sync);
    route_shard_t::routes_t::iterator it = shard.routes.find (routing_id);
    if (it == shard.routes.end () || !it->second.pipe) {
        errno = EHOSTUNREACH;
        return -1;
    }
    pipe_t *out = it->second.pipe;
    uint64_t pair_id = 0;
    uint64_t pair_generation = 0;
    const bool exact_pair_requested = expected_transport_pair_id_ != 0
                                      || expected_transport_pair_generation_ != 0;
    if (!stream_exact_target_identity (out, &pair_id, &pair_generation)
        || (exact_pair_requested
            && (expected_transport_pair_id_ == 0
                || expected_transport_pair_generation_ == 0
                || pair_id != expected_transport_pair_id_
                || pair_generation
                     != expected_transport_pair_generation_))) {
        errno = EHOSTUNREACH;
        return -1;
    }
    if (attempt_identity_out_) {
        attempt_identity_out_->transport_pair_id = pair_id;
        attempt_identity_out_->transport_pair_generation = pair_generation;
        attempt_identity_out_->transport_connection_id =
          out->get_transport_connection_id ();
    }
    if (msg_->size () == 0) {
        out->terminate (false);
    } else {
        pipe_message_admission_t write_admission = pipe_message_admission_invalid;
        if (!out->write_single_message_and_flush_no_recursive_hwm_check (
              msg_, &write_admission)) {
            if (admission_out_)
                *admission_out_ = write_admission;
            errno = EAGAIN;
            return -1;
        }
    }
    const int init_rc = msg_->init ();
    errno_assert (init_rc == 0);
    if (pipe_out_)
        *pipe_out_ = out;
    if (admission_out_)
        *admission_out_ = pipe_message_admission_ready;
    return 0;
}

int zlink::stream_t::xselect_routed_submit_target (
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_)
{
    if (!valid_routing_id (router_rid_or_null_) || !target_out_
        || router_rid_or_null_->size != sizeof (uint32_t)) {
        errno = EINVAL;
        return -1;
    }
    const uint32_t routing_id = get_uint32 (router_rid_or_null_->data);
    route_shard_t &shard = route_shard_for (routing_id);
    scoped_fast_lock_t shard_lock (shard.sync);
    route_shard_t::routes_t::iterator it = shard.routes.find (routing_id);
    if (it == shard.routes.end () || !it->second.pipe) {
        errno = EHOSTUNREACH;
        return -1;
    }
    copy_routing_id_from_bytes (router_rid_or_null_->data,
                                router_rid_or_null_->size,
                                &target_out_->peer_rid);
    if (!stream_exact_target_identity (
          it->second.pipe, &target_out_->transport_pair_id,
          &target_out_->transport_pair_generation)) {
        errno = EHOSTUNREACH;
        return -1;
    }
    return 0;
}

int zlink::stream_t::xrecv (msg_t *msg_)
{
    if (options.stream_recv_mode == ZLINK_STREAM_RECV_MODE_PACKET)
        return xrecv_packet_header (msg_);
    if (options.stream_recv_mode != ZLINK_STREAM_RECV_MODE_RAW) {
        errno = ENOTSUP;
        return -1;
    }

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

    if (!_more_in && !_stream_notify_routing_ids.empty ()) {
        const uint32_t routing_id = _stream_notify_routing_ids.front ();
        _stream_notify_routing_ids.pop_front ();

        const int init_rc = msg_->init_size (sizeof (routing_id));
        errno_assert (init_rc == 0);
        put_uint32 (static_cast<unsigned char *> (msg_->data ()), routing_id);
        msg_->set_flags (msg_t::more);
        const int notify_init_rc = _prefetched_msg.init_size (0);
        errno_assert (notify_init_rc == 0);
        _prefetched = true;
        _routing_id_sent = true;
        _current_in = NULL;
        _more_in = true;
        return 0;
    }

    pipe_t *pipe = NULL;
    const int rc = _fq.recvpipe (msg_, &pipe);
    if (rc != 0) {
        if (errno == ECONNABORTED) {
            _routing_id_sent = false;
            _current_in = NULL;
            _more_in = false;
        }
        return -1;
    }

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
    if (options.stream_recv_mode == ZLINK_STREAM_RECV_MODE_PACKET) {
        if (_packet_receive_queue.empty ())
            (void) pump_packet_receive_queue ();
        return !_packet_receive_queue.empty ();
    }
    if (options.stream_recv_mode != ZLINK_STREAM_RECV_MODE_RAW)
        return false;
    return _prefetched || !_stream_notify_routing_ids.empty () || _fq.has_in ();
}

bool zlink::stream_t::xhas_out ()
{
    return true;
}

bool zlink::stream_t::xsend_writable_target_ready (
  const zlink_routing_id_t *target_rid_or_null_)
{
    uint32_t routing_id = 0;
    if (!target_rid_or_null_ || target_rid_or_null_->size != 4)
        return false;
    routing_id = get_uint32 (target_rid_or_null_->data);

    route_shard_t &shard = route_shard_for (routing_id);
    scoped_fast_lock_t shard_lock (shard.sync);
    const route_shard_t::routes_t::const_iterator it =
      shard.routes.find (routing_id);
    if (it == shard.routes.end () || !it->second.pipe
        || !it->second.pipe->is_lifecycle_active ())
        return false;
    uint64_t pair_id = 0;
    uint64_t pair_generation = 0;
    if (!stream_exact_target_identity (it->second.pipe, &pair_id,
                                       &pair_generation))
        return false;
    return it->second.pipe->check_write_admission ()
           == pipe_message_admission_ready;
}

bool zlink::stream_t::xsend_writable_target_known (
  const zlink_routing_id_t *target_rid_or_null_)
{
    if (!target_rid_or_null_ || target_rid_or_null_->size != 4)
        return false;
    const uint32_t routing_id = get_uint32 (target_rid_or_null_->data);

    route_shard_t &shard = route_shard_for (routing_id);
    scoped_fast_lock_t shard_lock (shard.sync);
    const route_shard_t::routes_t::const_iterator it =
      shard.routes.find (routing_id);
    return it != shard.routes.end () && it->second.pipe
           && it->second.pipe->is_lifecycle_active ();
}

bool zlink::stream_t::xsend_writable_target_for_pipe (
  pipe_t *pipe_, zlink_routing_id_t *target_rid_out_)
{
    if (!pipe_ || !target_rid_out_)
        return false;
    const uint32_t routing_id = pipe_->get_server_socket_routing_id ();
    if (routing_id == 0)
        return false;

    route_shard_t &shard = route_shard_for (routing_id);
    scoped_fast_lock_t shard_lock (shard.sync);
    const route_shard_t::routes_t::const_iterator it =
      shard.routes.find (routing_id);
    if (it == shard.routes.end () || it->second.pipe != pipe_)
        return false;

    memset (target_rid_out_, 0, sizeof (*target_rid_out_));
    target_rid_out_->size = 4;
    put_uint32 (target_rid_out_->data, routing_id);
    return true;
}

int zlink::stream_t::xsetsockopt (int option_, const void *optval_, size_t optvallen_)
{
    if (option_ == ZLINK_INTERNAL_OPT_STREAM_NOTIFY
        || option_ == ZLINK_INTERNAL_OPT_STREAM_RECV_MODE) {
        if (socket_has_endpoint_history ()) {
            errno = EBUSY;
            return -1;
        }

        if (option_ == ZLINK_INTERNAL_OPT_STREAM_NOTIFY) {
            if (!optval_ || optvallen_ != sizeof (int)) {
                errno = EINVAL;
                return -1;
            }
            int value = 0;
            memcpy (&value, optval_, sizeof (value));
            if (value != 0 && value != 1) {
                errno = EINVAL;
                return -1;
            }
            if (value == 1
                && options.stream_recv_mode == ZLINK_STREAM_RECV_MODE_PACKET) {
                errno = ENOTSUP;
                return -1;
            }
        } else {
            if (!optval_ || optvallen_ != sizeof (zlink_stream_recv_mode_t)) {
                errno = EINVAL;
                return -1;
            }
            zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_UNSPECIFIED;
            memcpy (&mode, optval_, sizeof (mode));
            if (mode != ZLINK_STREAM_RECV_MODE_RAW
                && mode != ZLINK_STREAM_RECV_MODE_PACKET) {
                errno = EINVAL;
                return -1;
            }
            if (mode == ZLINK_STREAM_RECV_MODE_PACKET && options.stream_notify) {
                errno = ENOTSUP;
                return -1;
            }
        }

        const int rc = options.setsockopt (option_, optval_, optvallen_);
        if (rc == 0 && option_ == ZLINK_INTERNAL_OPT_STREAM_RECV_MODE) {
            clear_packet_receive_queue ();
            clear_pending_packet_input ();
        }
        return rc;
    }

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

bool zlink::stream_t::identify_peer (pipe_t *pipe_, bool locally_initiated_)
{
    blob_t routing_id;
    pipe_t *peer = NULL;
    pipe_t *replaced_pipe = NULL;
    uint32_t routing_id_value = 0;
    bool published = false;
    {
        scoped_fast_lock_t transport_gate (pipe_->transport_sync ());
        if (pipe_->stream_route_closed ())
            return false;
        peer = pipe_->retain_peer_snapshot ();
        if (!peer)
            return false;

        {
            // Identity assignment and route installation are one cold-path
            // publication transaction. The shared transport ID becomes visible
            // only after the route owns a lifetime reference.
            std::lock_guard<std::mutex> publication_lock (
              _route_publication_mutex);
            routing_id_value = pipe_->get_server_socket_routing_id ();
            if (routing_id_value == 0)
                routing_id_value = claim_next_routing_id (
                  _next_integral_routing_id);

            unsigned char buf[4];
            put_uint32 (buf, routing_id_value);
            routing_id.set (buf, sizeof buf);

            published = publish_route_locked (routing_id_value, pipe_, false,
                                              &replaced_pipe);
            if (published) {
                pipe_->set_router_socket_routing_id (routing_id);
                peer->set_router_socket_routing_id (routing_id);
                pipe_->set_server_socket_routing_id (routing_id_value);
            }
        }
    }
    if (replaced_pipe)
        replaced_pipe->release_lifetime_ref ();
    if (peer)
        peer->release_lifetime_ref ();
    if (!published)
        return false;

    if (!has_out_pipe (routing_id))
        add_out_pipe (ZLINK_MOVE (routing_id), pipe_, locally_initiated_);
    return true;
}

void zlink::stream_t::maybe_emit_connect_event (pipe_t *pipe_, uint32_t routing_id_value_)
{
    zlink_assert (pipe_);

    uint32_t resolved_routing_id = routing_id_value_;
    if (resolved_routing_id == 0)
        resolved_routing_id = pipe_->get_server_socket_routing_id ();
    if (resolved_routing_id == 0)
        return;

    if (!pipe_->mark_stream_connect_event_emitted ())
        return;

    unsigned char routing_id_data[4];
    put_uint32 (routing_id_data, resolved_routing_id);
    event_connection_ready_changed (pipe_->get_endpoint_pair (), routing_id_data,
                                    sizeof (routing_id_data));
}

void zlink::stream_t::queue_stream_notify (uint32_t routing_id_)
{
    if (routing_id_ != 0)
        _stream_notify_routing_ids.push_back (routing_id_);
}
