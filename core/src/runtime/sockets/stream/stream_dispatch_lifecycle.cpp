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
    if (_dispatch_active.load (std::memory_order_acquire)
        || _raw_part_receive_active.load (std::memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }

    clear_packet_dispatch_state ();
    _dispatch_mode.store (dispatch_mode_raw, std::memory_order_release);
    _dispatch_raw_callback.store (callback_, std::memory_order_release);
    _dispatch_msg_handler.store (NULL, std::memory_order_release);
    _dispatch_msg_handler_userdata.store (NULL, std::memory_order_release);
    _dispatch_packet_handler.store (NULL, std::memory_order_release);
    _dispatch_packet_handler_userdata.store (NULL, std::memory_order_release);
    _dispatch_active.store (true, std::memory_order_release);
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
    if (_dispatch_active.load (std::memory_order_acquire)
        || _raw_part_receive_active.load (std::memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }

    clear_packet_dispatch_state ();
    _dispatch_mode.store (dispatch_mode_raw, std::memory_order_release);
    _dispatch_raw_callback.store (NULL, std::memory_order_release);
    _dispatch_msg_handler.store (handler_, std::memory_order_release);
    _dispatch_msg_handler_userdata.store (userdata_, std::memory_order_release);
    _dispatch_packet_handler.store (NULL, std::memory_order_release);
    _dispatch_packet_handler_userdata.store (NULL, std::memory_order_release);
    _dispatch_active.store (true, std::memory_order_release);
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
    if (_dispatch_active.load (std::memory_order_acquire)
        || _raw_part_receive_active.load (std::memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }

    clear_packet_dispatch_state ();
    _dispatch_mode.store (dispatch_mode_packet, std::memory_order_release);
    _dispatch_raw_callback.store (NULL, std::memory_order_release);
    _dispatch_msg_handler.store (NULL, std::memory_order_release);
    _dispatch_msg_handler_userdata.store (NULL, std::memory_order_release);
    _dispatch_packet_handler.store (handler_, std::memory_order_release);
    _dispatch_packet_handler_userdata.store (userdata_, std::memory_order_release);
    _dispatch_active.store (true, std::memory_order_release);
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
    if (_dispatch_active.load (std::memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }
    _raw_part_receive_active.store (true, std::memory_order_release);
    return 0;
}

int zlink::stream_t::stream_dispatch_stop ()
{
    std::lock_guard<std::recursive_mutex> lock (_api_mutex);

    if (stream_dispatch_in_callback ()) {
        errno = EBUSY;
        return -1;
    }

    _dispatch_active.store (false, std::memory_order_release);
    _dispatch_mode.store (dispatch_mode_none, std::memory_order_release);
    _dispatch_raw_callback.store (NULL, std::memory_order_release);
    _dispatch_msg_handler.store (NULL, std::memory_order_release);
    _dispatch_msg_handler_userdata.store (NULL, std::memory_order_release);
    _dispatch_packet_handler.store (NULL, std::memory_order_release);
    _dispatch_packet_handler_userdata.store (NULL, std::memory_order_release);
    clear_packet_dispatch_state ();
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

uint32_t zlink::stream_t::stream_dispatch_inflight () const
{
    return _dispatch_inflight.load (std::memory_order_acquire);
}

void zlink::stream_t::stop_dispatch_from_callback ()
{
    std::lock_guard<std::recursive_mutex> lk (_api_mutex);
    _dispatch_active.store (false, std::memory_order_release);
    _dispatch_mode.store (dispatch_mode_none, std::memory_order_release);
    _dispatch_raw_callback.store (NULL, std::memory_order_release);
    _dispatch_msg_handler.store (NULL, std::memory_order_release);
    _dispatch_msg_handler_userdata.store (NULL, std::memory_order_release);
    _dispatch_packet_handler.store (NULL, std::memory_order_release);
    _dispatch_packet_handler_userdata.store (NULL, std::memory_order_release);
}

void zlink::stream_t::clear_packet_dispatch_state ()
{
    std::vector<pipe_t *> pipes;
    snapshot_attached_pipes (&pipes);
    for (size_t i = 0; i < pipes.size (); ++i) {
        if (pipes[i])
            pipes[i]->reset_stream_packet_state ();
    }
}
