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

    monitor_state_pin_t pin (handle.socket);
    monitor_handler_state_t *state = pin.get ();
    if (!state) {
        errno = EINVAL;
        return -1;
    }
    if (state->socket_handler.load (std::memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }

    return set_monitor_handler_state (
      handle.socket, state, handler_,
      state->snapshot_provider.load (std::memory_order_acquire),
      state->snapshot_subject.load (std::memory_order_acquire), userdata_);
}

void *open_socket_monitor_with_handler_internal (void *s_,
                                                 zlink_socket_monitor_event_mask_t events_,
                                                 int event_version_,
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

    zlink_monitor_handler_fn effective_handler = handler_;
    if (handler_ == &zlink_monitor_ignore_handler)
        effective_handler = NULL;

    char endpoint[128];
    const uint32_t rand_id = zlink::generate_random ();
    snprintf (endpoint, sizeof endpoint, "inproc://monitor-%p-%u", static_cast<void *> (s_),
              rand_id);

    const int monitor_rc =
      handle.socket->monitor (endpoint, events_, event_version_,
                              ZLINK_CORE_SOCKET_PAIR);
    if (monitor_rc != 0)
        return NULL;

    zlink::socket_base_t *monitor_socket_base =
      handle.socket->get_ctx ()->create_socket (ZLINK_CORE_SOCKET_PAIR);
    void *monitor_socket = static_cast<void *> (monitor_socket_base);
    if (!monitor_socket) {
        handle.socket->monitor (NULL, 0, event_version_,
                                ZLINK_CORE_SOCKET_PAIR);
        return NULL;
    }
    monitor_socket_base->set_auto_hwm_policy_enabled (false);
    //  The monitor pipe used to hold 4,096 event messages. HWM is now byte
    //  based, so the same depth is expressed as the accounted charge of that
    //  many event frames: the wire event plus the per-frame minimum charge.
    //  Auto HWM is disabled for this socket, so nothing else would size it.
    const uint64_t monitor_event_depth = 4096;
    const uint64_t monitor_hwm =
      monitor_event_depth
      * (sizeof (socket_monitor_internal_event_t) + sizeof (zlink_msg_t));
    (void) monitor_socket_base->setsockopt (ZLINK_INTERNAL_OPT_SNDHWM, &monitor_hwm,
                                            sizeof (monitor_hwm));
    (void) monitor_socket_base->setsockopt (ZLINK_INTERNAL_OPT_RCVHWM, &monitor_hwm,
                                            sizeof (monitor_hwm));

    if (zlink_connect (monitor_socket, endpoint) != 0) {
        zlink_close (monitor_socket);
        handle.socket->monitor (NULL, 0, event_version_,
                                ZLINK_CORE_SOCKET_PAIR);
        return NULL;
    }

    if (set_monitor_handler_state (monitor_socket_base, NULL, effective_handler,
                                   &socket_monitor_snapshot_provider,
                                   static_cast<void *> (handle.socket), userdata_)
        != 0) {
        const int err = errno;
        zlink_close (monitor_socket);
        handle.socket->monitor (NULL, 0, event_version_,
                                ZLINK_CORE_SOCKET_PAIR);
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
      socket_, events_, event_version_, &zlink_monitor_ignore_handler, NULL);
}

void *zlink_socket_monitor_open (void *s_, const zlink_socket_monitor_open_options_t *options_)
{
    if (!options_) {
        errno = EINVAL;
        return NULL;
    }
    return open_socket_monitor_with_handler_internal (s_, options_->events, 3,
                                                      &zlink_monitor_ignore_handler, NULL);
}

zlink_handler_result_t zlink_socket_monitor_handler (void *monitor_,
                                                     zlink_socket_monitor_handler_fn handler_,
                                                     void *userdata_)
{
    return zlink::handler_result_internal::from_rc (
      attach_socket_monitor_handler_state (monitor_, handler_, userdata_));
}
