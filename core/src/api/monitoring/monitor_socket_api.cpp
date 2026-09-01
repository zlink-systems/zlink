/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/monitoring/monitor_api_internal.hpp"
#include "api/socket/socket_api_internal.hpp"

#include "core/ctx.hpp"
#include "utils/random.hpp"

namespace
{
void *open_socket_monitor_pull_internal (void *s_,
                                         zlink_socket_monitor_event_mask_t events_,
                                         int event_version_,
                                         uint64_t requested_monitor_hwm_bytes_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return NULL;

    uint64_t monitor_hwm_bytes = requested_monitor_hwm_bytes_;
    if (monitor_hwm_bytes == 0
        && !socket_monitor_default_hwm_bytes (&monitor_hwm_bytes)) {
        errno = EINVAL;
        return NULL;
    }

    char endpoint[128];
    const uint32_t rand_id = zlink::generate_random ();
    snprintf (endpoint, sizeof endpoint, "inproc://monitor-%p-%u",
              static_cast<void *> (s_), rand_id);

    const int monitor_rc =
      handle.socket->monitor (endpoint, events_, event_version_,
                              ZLINK_CORE_SOCKET_PAIR, monitor_hwm_bytes);
    if (monitor_rc != 0)
        return NULL;

    zlink::socket_base_t *monitor_socket_base =
      handle.socket->get_ctx ()->create_socket (ZLINK_CORE_SOCKET_PAIR);
    if (!monitor_socket_base) {
        handle.socket->monitor (NULL, 0, event_version_,
                                ZLINK_CORE_SOCKET_PAIR, 0);
        return NULL;
    }
    void *monitor_socket =
      handle.socket->get_ctx ()->register_public_socket_handle (monitor_socket_base);
    if (!monitor_socket) {
        const int saved_errno = errno;
        monitor_socket_base->close ();
        handle.socket->monitor (NULL, 0, event_version_,
                                ZLINK_CORE_SOCKET_PAIR, 0);
        errno = saved_errno;
        return NULL;
    }
    if (monitor_socket_base->configure_internal_monitor_queue (
          monitor_hwm_bytes)
        != 0) {
        const int err = errno;
        zlink_close (monitor_socket);
        handle.socket->monitor (NULL, 0, event_version_,
                                ZLINK_CORE_SOCKET_PAIR, 0);
        errno = err == 0 ? EINVAL : err;
        return NULL;
    }

    if (zlink_connect (monitor_socket, endpoint) != 0) {
        zlink_close (monitor_socket);
        handle.socket->monitor (NULL, 0, event_version_,
                                ZLINK_CORE_SOCKET_PAIR, 0);
        return NULL;
    }

    if (register_monitor_pull_state (monitor_socket_base,
                                     handle.socket->public_handle ())
        != 0) {
        const int err = errno;
        zlink_close (monitor_socket);
        handle.socket->monitor (NULL, 0, event_version_,
                                ZLINK_CORE_SOCKET_PAIR, 0);
        errno = err;
        return NULL;
    }

    return monitor_socket;
}
}

void *open_socket_monitor_internal (
  void *socket_, zlink_socket_monitor_event_mask_t events_, int event_version_)
{
    return open_socket_monitor_pull_internal (socket_, events_, event_version_, 0);
}

void *zlink_socket_monitor_open (void *s_,
                                 const zlink_socket_monitor_open_options_t *options_)
{
    if (!options_) {
        errno = EINVAL;
        return NULL;
    }
    return open_socket_monitor_pull_internal (
      s_, options_->events, 3, options_->monitor_hwm_bytes);
}
