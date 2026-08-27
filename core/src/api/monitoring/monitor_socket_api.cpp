/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/message/handler_result_internal.hpp"
#include "api/monitoring/monitor_api_internal.hpp"
#include "api/socket/socket_api_internal.hpp"

#include "core/ctx.hpp"
#include "utils/random.hpp"

namespace
{
int attach_socket_monitor_handler_state (void *monitor_,
                                         zlink_socket_monitor_handler_fn handler_,
                                         void *userdata_)
{
    if (!monitor_) {
        errno = EFAULT;
        return -1;
    }
    if (!handler_) {
        errno = EINVAL;
        return -1;
    }

    socket_handle_t handle = as_socket_handle (monitor_);
    if (!handle.socket)
        return -1;

    zlink::socket_base_t *socket = handle.socket;
    monitor_state_pin_t pin (socket);
    monitor_handler_state_t *state = pin.get ();
    if (!state) {
        errno = EINVAL;
        return -1;
    }
    if (state->socket_handler.load (std::memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }

    //  The registry pin keeps both the handler state and its socket alive:
    //  a concurrent close cannot pass unregister_monitor_handlers until this
    //  function returns. Release the public-handle pin before arming the
    //  immediate dispatch task, so a callback that self-closes does not see
    //  this registration call's internal pin as a competing public API.
    handle = socket_handle_t ();
    return set_monitor_handler_state (
      socket, state, handler_,
      state->snapshot_provider.load (std::memory_order_acquire),
      state->snapshot_subject.load (std::memory_order_acquire), userdata_);
}

void *open_socket_monitor_with_handler_internal (void *s_,
                                                 zlink_socket_monitor_event_mask_t events_,
                                                 int event_version_,
                                                 uint64_t requested_monitor_hwm_bytes_,
                                                 zlink_monitor_handler_fn handler_,
                                                 void *userdata_)
{
    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return NULL;
    if (!handler_) {
        errno = EINVAL;
        return NULL;
    }

    uint64_t monitor_hwm_bytes = requested_monitor_hwm_bytes_;
    if (monitor_hwm_bytes == 0
        && !socket_monitor_default_hwm_bytes (&monitor_hwm_bytes)) {
        errno = EINVAL;
        return NULL;
    }

    zlink_monitor_handler_fn effective_handler = handler_;
    if (handler_ == &zlink_monitor_ignore_handler)
        effective_handler = NULL;

    char endpoint[128];
    const uint32_t rand_id = zlink::generate_random ();
    snprintf (endpoint, sizeof endpoint, "inproc://monitor-%p-%u", static_cast<void *> (s_),
              rand_id);

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

    if (set_monitor_handler_state (monitor_socket_base, NULL, effective_handler,
                                   &socket_monitor_snapshot_provider,
                                   handle.socket->public_handle (), userdata_)
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
  void *socket_,
  zlink_socket_monitor_event_mask_t events_,
  int event_version_)
{
    return open_socket_monitor_with_handler_internal (
      socket_, events_, event_version_, 0, &zlink_monitor_ignore_handler, NULL);
}

void *zlink_socket_monitor_open (void *s_, const zlink_socket_monitor_open_options_t *options_)
{
    if (!options_) {
        errno = EINVAL;
        return NULL;
    }
    return open_socket_monitor_with_handler_internal (s_, options_->events, 3,
                                                      options_->monitor_hwm_bytes,
                                                      &zlink_monitor_ignore_handler, NULL);
}

zlink_handler_result_t zlink_socket_monitor_handler (void *monitor_,
                                                     zlink_socket_monitor_handler_fn handler_,
                                                     void *userdata_)
{
    return zlink::handler_result_internal::from_rc (
      attach_socket_monitor_handler_state (monitor_, handler_, userdata_));
}
