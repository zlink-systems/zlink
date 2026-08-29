/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/stream/stream.hpp"
#include "sockets/stream/stream_dispatch_internal.hpp"
#include "core/pipe.hpp"

int zlink::stream_t::stream_dispatch_start_raw (zlink_stream_on_raw_fn callback_)
{
    if (!callback_) {
        errno = EINVAL;
        return -1;
    }

    std::lock_guard<std::recursive_mutex> lock (_api_mutex);
    if (_dispatch_mode.load (std::memory_order_acquire) != dispatch_mode_none
        || _raw_part_receive_active.load (std::memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }

    // A callback may have stopped dispatch by publishing mode=none without
    // waiting on its own connection gate. Quiesce all previously published
    // routes before replacing the callback snapshot.
    clear_packet_dispatch_state ();
    _dispatch_raw_callback = callback_;
    _dispatch_msg_handler = NULL;
    _dispatch_msg_handler_userdata = NULL;
    _dispatch_packet_handler = NULL;
    _dispatch_packet_handler_userdata = NULL;
    // Mode is the single publication point for the callback fields above.
    _dispatch_mode.store (dispatch_mode_raw, std::memory_order_release);
    return 0;
}

int zlink::stream_t::stream_set_msg_handler_with_userdata (zlink_socket_msg_handler_fn handler_,
                                                           void *userdata_)
{
    std::lock_guard<std::recursive_mutex> lock (_api_mutex);

    if (!handler_) {
        errno = EINVAL;
        return -1;
    }
    if (_dispatch_mode.load (std::memory_order_acquire) != dispatch_mode_none
        || _raw_part_receive_active.load (std::memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }

    clear_packet_dispatch_state ();
    _dispatch_raw_callback = NULL;
    _dispatch_msg_handler = handler_;
    _dispatch_msg_handler_userdata = userdata_;
    _dispatch_packet_handler = NULL;
    _dispatch_packet_handler_userdata = NULL;
    _dispatch_mode.store (dispatch_mode_raw, std::memory_order_release);
    return 0;
}

int zlink::stream_t::stream_set_packet_msg_handler_with_userdata (
  zlink_stream_packet_handler_fn handler_, void *userdata_)
{
    std::lock_guard<std::recursive_mutex> lock (_api_mutex);

    if (!handler_) {
        errno = EINVAL;
        return -1;
    }
    if (_dispatch_mode.load (std::memory_order_acquire) != dispatch_mode_none
        || _raw_part_receive_active.load (std::memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }

    clear_packet_dispatch_state ();
    _dispatch_raw_callback = NULL;
    _dispatch_msg_handler = NULL;
    _dispatch_msg_handler_userdata = NULL;
    _dispatch_packet_handler = handler_;
    _dispatch_packet_handler_userdata = userdata_;
    _dispatch_mode.store (dispatch_mode_packet, std::memory_order_release);
    return 0;
}

int zlink::stream_t::stream_mark_raw_part_receive ()
{
    // Raw-part receive is permanent for the lifetime of the socket. Once it
    // wins the mode-selection race, later receive calls do not need to enter
    // the API mutex again.
    if (_raw_part_receive_active.load (std::memory_order_acquire))
        return 0;

    std::lock_guard<std::recursive_mutex> lock (_api_mutex);
    if (_dispatch_mode.load (std::memory_order_acquire) != dispatch_mode_none) {
        errno = EBUSY;
        return -1;
    }
    _raw_part_receive_active.store (true, std::memory_order_release);
    return 0;
}

int zlink::stream_t::stream_dispatch_stop ()
{
    if (stream_dispatch_in_callback ()) {
        errno = EBUSY;
        return -1;
    }

    std::lock_guard<std::recursive_mutex> lock (_api_mutex);

    // Stop admission before waiting on connection-local gates. Once every
    // published route has passed its gate, no callback can still reference
    // the callback/userdata snapshot cleared below.
    _dispatch_mode.store (dispatch_mode_none, std::memory_order_release);
    clear_packet_dispatch_state ();
    _dispatch_raw_callback = NULL;
    _dispatch_msg_handler = NULL;
    _dispatch_msg_handler_userdata = NULL;
    _dispatch_packet_handler = NULL;
    _dispatch_packet_handler_userdata = NULL;
    return 0;
}

bool zlink::stream_t::stream_dispatch_active () const
{
    return _dispatch_mode.load (std::memory_order_acquire) != dispatch_mode_none;
}

bool zlink::stream_t::stream_dispatch_in_callback () const
{
    return stream_dispatch_owns_socket (this);
}

void zlink::stream_t::stop_dispatch_from_callback ()
{
    // The current callback owns one route gate, so it cannot wait for all
    // gates without deadlocking itself. Publishing none is sufficient here;
    // detach or the next attach quiesces old gates before clearing/replacing
    // the callback fields.
    _dispatch_mode.store (dispatch_mode_none, std::memory_order_release);
}

void zlink::stream_t::clear_packet_dispatch_state ()
{
    std::vector<pipe_t *> pipes;
    {
        // Keep route publication and the quiescence snapshot indivisible. Do
        // not wait on a route gate while holding either this mutex or a shard:
        // callbacks may send through another route and need that shard.
        std::lock_guard<std::mutex> publication_lock (_dispatch_publication_mutex);
        for (size_t shard_index = 0; shard_index < route_shard_count; ++shard_index) {
            route_shard_t &shard = _route_shards[shard_index];
            scoped_fast_lock_t shard_lock (shard.sync);
            for (route_shard_t::routes_t::const_iterator route = shard.routes.begin ();
                 route != shard.routes.end (); ++route) {
                pipe_t *const pipe = route->second.pipe;
                if (!pipe)
                    continue;

                bool already_present = false;
                for (size_t pipe_index = 0; pipe_index < pipes.size (); ++pipe_index) {
                    if (pipes[pipe_index] == pipe) {
                        already_present = true;
                        break;
                    }
                }
                // A route is published as a raw pointer for the steady-state
                // send path. Pin it while the shard still excludes route
                // removal so the quiescence pass can safely use it after the
                // shard lock is released.
                if (!already_present && pipe->retain_lifetime_ref ())
                    pipes.push_back (pipe);
            }
        }
    }

    for (size_t i = 0; i < pipes.size (); ++i) {
        pipes[i]->reset_stream_packet_state ();
        pipes[i]->release_lifetime_ref ();
    }
}
