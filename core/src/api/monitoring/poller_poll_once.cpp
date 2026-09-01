/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <array>
#include <new>
#include <utility>
#include <vector>

#include "api/core/config_result_internal.hpp"
#include "api/monitoring/poller_api_internal.hpp"

int zlink_poll (zlink_pollitem_t *items_,
                int nitems_,
                long timeout_,
                zlink_config_result_t *error_out_)
{
    if (nitems_ < 0 || (nitems_ > 0 && !items_)) {
        errno = EINVAL;
        if (error_out_)
            *error_out_ = ZLINK_CONFIG_INVALID_ARGUMENT;
        return -1;
    }
    if (nitems_ == 0) {
        errno = 0;
        if (error_out_)
            *error_out_ = ZLINK_CONFIG_OK;
        return 0;
    }

    // Keep every public-handle pin until after the local poller releases its
    // raw socket registrations. Declaration order is intentional: poller is
    // destroyed first on every return path.
    std::array<socket_handle_t, ZLINK_POLLITEMS_DFLT> inline_socket_handles;
    std::vector<socket_handle_t> overflow_socket_handles;
    socket_handle_t *socket_handles = inline_socket_handles.data ();
    zlink::socket_poller_t poller;
    std::array<zlink::socket_poller_t::event_t, ZLINK_POLLITEMS_DFLT>
      inline_events;
    std::vector<zlink::socket_poller_t::event_t> overflow_events;
    zlink::socket_poller_t::event_t *events = inline_events.data ();
    try {
        const size_t item_count = static_cast<size_t> (nitems_);
        if (item_count > inline_socket_handles.size ()) {
            overflow_socket_handles.resize (item_count);
            overflow_events.resize (item_count);
            socket_handles = overflow_socket_handles.data ();
            events = overflow_events.data ();
        }
        poller.reserve (item_count);
    }
    catch (const std::bad_alloc &) {
        errno = ENOMEM;
        if (error_out_)
            *error_out_ = zlink::config_result_internal::from_errno (errno);
        return -1;
    }
    for (int i = 0; i < nitems_; ++i) {
        items_[i].revents = 0;
        void *index_user_data = poller_index_user_data (static_cast<size_t> (i));
        if (items_[i].socket) {
            socket_handle_t handle = as_socket_handle (items_[i].socket);
            if (!handle.socket) {
                if (error_out_)
                    *error_out_ = zlink::config_result_internal::from_errno (errno);
                return -1;
            }
            if (validate_socket_poller_event_mask (items_[i].events, false) != 0) {
                if (error_out_)
                    *error_out_ = zlink::config_result_internal::from_errno (errno);
                return -1;
            }
            if (poller.add (handle.socket, index_user_data, items_[i].events) != 0) {
                if (error_out_)
                    *error_out_ = zlink::config_result_internal::from_errno (errno);
                return -1;
            }
            socket_handles[static_cast<size_t> (i)] = std::move (handle);
        } else {
            if (validate_fd_poller_event_mask (items_[i].events) != 0
                || poller.add_fd (items_[i].fd, index_user_data,
                                  items_[i].events)
                     != 0) {
                if (error_out_)
                    *error_out_ = zlink::config_result_internal::from_errno (errno);
                return -1;
            }
        }
    }

    const int rc = poller.wait (events, nitems_, timeout_);
    if (rc < 0) {
        if (errno == EAGAIN) {
            errno = 0;
            if (error_out_)
                *error_out_ = ZLINK_CONFIG_OK;
            return 0;
        }
        if (error_out_)
            *error_out_ = zlink::config_result_internal::from_errno (errno);
        return rc;
    }
    if (rc == 0) {
        errno = 0;
        if (error_out_)
            *error_out_ = ZLINK_CONFIG_OK;
        return 0;
    }
    for (int i = 0; i < rc; ++i) {
        size_t index = 0;
        if (poller_index_from_user_data (events[i].user_data, static_cast<size_t> (nitems_),
                                         &index)) {
            items_[static_cast<int> (index)].revents =
              static_cast<short> (items_[static_cast<int> (index)].revents | events[i].events);
            continue;
        }
        poller_set_pollitem_revents_by_identity (items_, nitems_, events[i]);
    }
    int public_count = 0;
    for (int i = 0; i < nitems_; ++i) {
        if (items_[i].revents != 0) {
            if (items_[i].socket) {
                const socket_handle_t &handle =
                  socket_handles[static_cast<size_t> (i)];
                const short other_events =
                  static_cast<short> (items_[i].events & ~ZLINK_POLLIN);
                uint32_t ready_events = 0;
                if (handle.socket && other_events != 0
                    && handle.socket->get_events (other_events, &ready_events) == 0) {
                    items_[i].revents =
                      static_cast<short> (items_[i].revents | ready_events);
                }
            }
            ++public_count;
        }
    }
    if (error_out_)
        *error_out_ = ZLINK_CONFIG_OK;
    return public_count;
}
