/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/monitoring/monitor_api_internal.hpp"

#include <condition_variable>
#include <map>
#include <mutex>

#include "api/core/close_result_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "sockets/common/socket_close_ops.hpp"

namespace
{
struct monitor_pull_registry_t
{
    //  std::mutex + condition_variable let monitor status/close lookup pin an
    //  association while a concurrent socket close removes it.
    std::mutex sync;
    std::condition_variable drained;
    std::map<zlink::socket_base_t *, monitor_pull_state_t *> states;
};

monitor_pull_registry_t &monitor_pull_registry ()
{
    static monitor_pull_registry_t *registry = new monitor_pull_registry_t ();
    return *registry;
}
}

int require_monitor_pull_handle (void *monitor_)
{
    if (!monitor_) {
        errno = EFAULT;
        return -1;
    }

    socket_handle_t handle = as_socket_handle (monitor_);
    if (!handle.socket)
        return -1;

    monitor_pull_state_pin_t pin (handle.socket);
    if (!pin.get ()) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

monitor_pull_state_t *pin_monitor_pull_state (zlink::socket_base_t *socket_)
{
    monitor_pull_registry_t &registry = monitor_pull_registry ();
    std::lock_guard<std::mutex> lock (registry.sync);
    std::map<zlink::socket_base_t *, monitor_pull_state_t *>::iterator it =
      registry.states.find (socket_);
    if (it == registry.states.end () || !it->second || it->second->unregistered)
        return NULL;
    ++it->second->registry_pins;
    return it->second;
}

void unpin_monitor_pull_state (monitor_pull_state_t *state_)
{
    if (!state_)
        return;

    monitor_pull_registry_t &registry = monitor_pull_registry ();
    std::lock_guard<std::mutex> lock (registry.sync);
    --state_->registry_pins;
    if (state_->registry_pins == 0 && state_->unregistered)
        registry.drained.notify_all ();
}

int register_monitor_pull_state (zlink::socket_base_t *socket_,
                                 void *snapshot_subject_)
{
    if (!socket_ || !snapshot_subject_) {
        errno = EINVAL;
        return -1;
    }

    monitor_pull_state_t *state =
      new (std::nothrow) monitor_pull_state_t (socket_, snapshot_subject_);
    if (!state) {
        errno = ENOMEM;
        return -1;
    }

    monitor_pull_registry_t &registry = monitor_pull_registry ();
    std::lock_guard<std::mutex> lock (registry.sync);
    if (registry.states.find (socket_) != registry.states.end ()) {
        delete state;
        errno = EBUSY;
        return -1;
    }
    try {
        registry.states[socket_] = state;
    }
    catch (const std::bad_alloc &) {
        delete state;
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

void unregister_monitor_pull_state (zlink::socket_base_t *socket_)
{
    if (!socket_)
        return;

    monitor_pull_registry_t &registry = monitor_pull_registry ();
    monitor_pull_state_t *state = NULL;
    {
        std::unique_lock<std::mutex> lock (registry.sync);
        std::map<zlink::socket_base_t *, monitor_pull_state_t *>::iterator it =
          registry.states.find (socket_);
        if (it == registry.states.end ())
            return;
        state = it->second;
        registry.states.erase (it);
        state->unregistered = true;
        while (state->registry_pins > 0)
            registry.drained.wait (lock);
    }
    delete state;
}

socket_handle_t monitor_snapshot_subject_handle (monitor_pull_state_t *state_)
{
    if (!state_)
        return socket_handle_t ();

    void *subject = state_->snapshot_subject.load (std::memory_order_acquire);
    if (!subject)
        return socket_handle_t ();
    const int saved_errno = errno;
    socket_handle_t handle = as_socket_handle (subject);
    errno = saved_errno;
    return handle;
}

void clear_raw_monitor_snapshot_subjects (zlink::socket_base_t *source_)
{
    if (!source_)
        return;

    monitor_pull_registry_t &registry = monitor_pull_registry ();
    std::lock_guard<std::mutex> lock (registry.sync);
    for (std::map<zlink::socket_base_t *, monitor_pull_state_t *>::iterator it =
           registry.states.begin ();
         it != registry.states.end (); ++it) {
        monitor_pull_state_t *state = it->second;
        if (state && state->snapshot_subject.load (std::memory_order_acquire)
                       == source_->public_handle ())
            state->snapshot_subject.store (NULL, std::memory_order_release);
    }
}

zlink_close_result_t zlink_monitor_close (void **monitor_p_)
{
    if (!monitor_p_ || !*monitor_p_) {
        errno = EFAULT;
        return ZLINK_CLOSE_INVALID_HANDLE;
    }

    void *monitor = *monitor_p_;
    socket_handle_t handle = as_socket_handle (monitor);
    if (!handle.socket)
        return zlink::close_result_internal::from_errno (errno);

    zlink::socket_base_t *socket = handle.socket;
    const int linger = 0;
    (void) socket->setsockopt (ZLINK_INTERNAL_OPT_LINGER, &linger, sizeof (linger));

    zlink::socket_base_t *raw_source_monitor_socket = NULL;
    {
        monitor_pull_state_pin_t pin (socket);
        monitor_pull_state_t *state = pin.get ();
        if (!state) {
            errno = EINVAL;
            return ZLINK_CLOSE_INVALID_HANDLE;
        }

        socket_handle_t source = monitor_snapshot_subject_handle (state);
        if (source.socket && source.socket != socket) {
            state->snapshot_subject.store (NULL, std::memory_order_release);
            raw_source_monitor_socket = source.socket->detach_monitor_socket (false);
        }
    }

    //  Monitor recv/close are caller-serialized. Stop the pull socket before
    //  handing it to the common close path; there is no callback worker to
    //  detach or defer.
    socket->stop ();
    handle = socket_handle_t ();
    const zlink_close_result_t rc = zlink_close (monitor);
    if (raw_source_monitor_socket)
        (void) zlink::socket_close_ops_t::request_close (raw_source_monitor_socket, 0);
    if (rc == ZLINK_CLOSE_OK)
        *monitor_p_ = NULL;
    return rc;
}
