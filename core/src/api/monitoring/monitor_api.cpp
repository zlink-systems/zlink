/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/monitoring/monitor_api_internal.hpp"

#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdio.h>

#include "api/core/close_result_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "core/ctx.hpp"
#include "core/control_runtime.hpp"
#include "sockets/common/socket_close_ops.hpp"
#include "utils/clock.hpp"
#include "utils/mutex.hpp"
#include "utils/sleep.hpp"

namespace
{
void monitor_debug_logf (const char *fmt_, ...)
{
    if (!std::getenv ("ZLINK_DEBUG_MONITOR_SHUTDOWN"))
        return;
    std::fprintf (stderr, "[monitor-close] ");
    va_list args;
    va_start (args, fmt_);
    std::vfprintf (stderr, fmt_, args);
    va_end (args);
    std::fflush (stderr);
}

struct monitor_handler_registry_t
{
    //  std::mutex + condition_variable: the delete paths wait here until
    //  every reader pin drains, which zlink::mutex_t cannot express.
    std::mutex sync;
    std::condition_variable drained;
    std::map<zlink::socket_base_t *, monitor_handler_state_t *> handlers;
};

monitor_handler_registry_t &monitor_handler_registry ()
{
    //  Monitor callbacks may still be unwinding when a process exits after a
    //  fatal test or application error. Keep the registry valid through
    //  static teardown; normal monitor close still removes every entry.
    static monitor_handler_registry_t *registry = new monitor_handler_registry_t ();
    return *registry;
}

void stop_monitor_handler_state (monitor_handler_state_t *state_)
{
    if (!state_)
        return;

    state_->stop.store (true, std::memory_order_release);
    if (state_->socket) {
        zlink::control_runtime_t *runtime =
          state_->socket->get_ctx ()->control_runtime ();
        if (runtime && state_->dispatch_task_id != 0)
            (void) runtime->remove_task (state_->dispatch_task_id);
    }
    delete state_;
}

void stop_monitor_handler_worker (monitor_handler_state_t *state_)
{
    if (!state_)
        return;

    state_->stop.store (true, std::memory_order_release);
    if (state_->socket)
        state_->socket->stop ();
    if (state_->socket) {
        zlink::control_runtime_t *runtime =
          state_->socket->get_ctx ()->control_runtime ();
        if (runtime && state_->dispatch_task_id != 0)
            (void) runtime->remove_task (state_->dispatch_task_id);
    }
}

//  Removes the registry entry and blocks until every reader pin on state_
//  has been released. After this returns the caller owns the storage and may
//  delete it without racing zlink_monitor_status/recv-model readers.
void erase_monitor_handler_state_and_wait (zlink::socket_base_t *socket_,
                                           monitor_handler_state_t *state_)
{
    if (!state_)
        return;

    monitor_handler_registry_t &registry = monitor_handler_registry ();
    std::unique_lock<std::mutex> lock (registry.sync);
    if (socket_) {
        std::map<zlink::socket_base_t *, monitor_handler_state_t *>::iterator it =
          registry.handlers.find (socket_);
        if (it != registry.handlers.end () && it->second == state_)
            registry.handlers.erase (it);
    }
    state_->unregistered = true;
    while (state_->registry_pins > 0)
        registry.drained.wait (lock);
}

void finalize_monitor_handler_self_close (monitor_handler_state_t *state_)
{
    if (!state_)
        return;

    zlink::socket_base_t *socket = state_->socket;
    zlink::control_runtime_t *runtime =
      socket ? socket->get_ctx ()->control_runtime () : NULL;
    const uint64_t dispatch_task_id = state_->dispatch_task_id;
    state_->stop.store (true, std::memory_order_release);
    zlink::socket_base_t *raw_monitor_source = raw_monitor_snapshot_subject (state_);
    if (raw_monitor_source && raw_monitor_source != socket) {
        state_->snapshot_subject.store (NULL, std::memory_order_release);
        (void) raw_monitor_source->monitor (NULL, 0, 3, ZLINK_CORE_SOCKET_PAIR);
    } else {
        clear_raw_monitor_snapshot_subjects (socket);
    }
    erase_monitor_handler_state_and_wait (socket, state_);
    state_->socket = NULL;
    state_->dispatch_task_id = 0;
    if (runtime && dispatch_task_id != 0)
        (void) runtime->remove_task (dispatch_task_id);
    if (socket) {
        zlink::part_helper_internal::cleanup_handle (socket);
        socket->stop ();
        socket->close ();
    }
    delete state_;
}

void monitor_handler_task (void *arg_)
{
    monitor_handler_state_t *state_ = static_cast<monitor_handler_state_t *> (arg_);
    if (!state_ || !state_->socket)
        return;

    void *monitor_socket = static_cast<void *> (state_->socket);
    while (!state_->stop.load (std::memory_order_acquire)) {
        zlink_monitor_event_t event;
        const int rc = recv_socket_monitor_event_unchecked (monitor_socket, &event, ZLINK_DONTWAIT);
        if (rc != 0)
            break;

        zlink_monitor_handler_fn handler = NULL;
        void *handler_userdata = NULL;
        {
            zlink::scoped_lock_t lock (state_->dispatch_sync);
            if (state_->stop.load (std::memory_order_acquire)
                || state_->close_requested.load (std::memory_order_acquire)) {
                break;
            }

            handler = state_->socket_handler.load (std::memory_order_acquire);
            if (!handler)
                break;

            handler_userdata = state_->socket_handler_userdata.load (std::memory_order_acquire);
            state_->callback_depth.fetch_add (1, std::memory_order_acq_rel);
        }
        const zlink::monitor_dispatch_context_t dispatch_scope (state_);
        handler (&event, handler_userdata);
        const int depth_after = state_->callback_depth.fetch_sub (1, std::memory_order_acq_rel) - 1;
        if (state_->close_requested.load (std::memory_order_acquire) && depth_after == 0) {
            finalize_monitor_handler_self_close (state_);
            return;
        }
    }

    if (state_->close_requested.load (std::memory_order_acquire)
        && state_->callback_depth.load (std::memory_order_acquire) == 0) {
        finalize_monitor_handler_self_close (state_);
    }
}
}

int require_monitor_recv_model (void *monitor_)
{
    if (!monitor_) {
        errno = EFAULT;
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
    return 0;
}

monitor_handler_state_t *pin_monitor_handler_state (zlink::socket_base_t *socket_)
{
    monitor_handler_registry_t &registry = monitor_handler_registry ();
    std::lock_guard<std::mutex> lock (registry.sync);
    std::map<zlink::socket_base_t *, monitor_handler_state_t *>::iterator it =
      registry.handlers.find (socket_);
    if (it == registry.handlers.end () || !it->second || it->second->unregistered)
        return NULL;
    it->second->registry_pins += 1;
    return it->second;
}

void unpin_monitor_handler_state (monitor_handler_state_t *state_)
{
    if (!state_)
        return;
    monitor_handler_registry_t &registry = monitor_handler_registry ();
    std::lock_guard<std::mutex> lock (registry.sync);
    state_->registry_pins -= 1;
    if (state_->registry_pins == 0 && state_->unregistered)
        registry.drained.notify_all ();
}

//  Membership probe without a pin: safe because it never dereferences the
//  mapped state.
static bool monitor_state_registered (zlink::socket_base_t *socket_)
{
    monitor_handler_registry_t &registry = monitor_handler_registry ();
    std::lock_guard<std::mutex> lock (registry.sync);
    return registry.handlers.find (socket_) != registry.handlers.end ();
}

zlink::socket_base_t *raw_monitor_snapshot_subject (monitor_handler_state_t *state_)
{
    if (!state_)
        return NULL;

    monitor_snapshot_provider_fn provider =
      state_->snapshot_provider.load (std::memory_order_acquire);
    if (provider != &socket_monitor_snapshot_provider)
        return NULL;

    return static_cast<zlink::socket_base_t *> (
      state_->snapshot_subject.load (std::memory_order_acquire));
}

void clear_raw_monitor_snapshot_subjects (zlink::socket_base_t *source_)
{
    if (!source_)
        return;

    monitor_handler_registry_t &registry = monitor_handler_registry ();
    std::lock_guard<std::mutex> lock (registry.sync);
    for (std::map<zlink::socket_base_t *, monitor_handler_state_t *>::iterator it =
           registry.handlers.begin ();
         it != registry.handlers.end (); ++it) {
        monitor_handler_state_t *state = it->second;
        if (!state)
            continue;

        monitor_snapshot_provider_fn provider =
          state->snapshot_provider.load (std::memory_order_acquire);
        if (provider != &socket_monitor_snapshot_provider)
            continue;

        if (state->snapshot_subject.load (std::memory_order_acquire) == source_)
            state->snapshot_subject.store (NULL, std::memory_order_release);
    }
}

void unregister_monitor_handlers (zlink::socket_base_t *socket_)
{
    if (!socket_)
        return;

    monitor_handler_registry_t &registry = monitor_handler_registry ();
    monitor_handler_state_t *state = NULL;
    {
        //  Find + erase are one critical section so exactly one closer wins
        //  the delete; the winner then waits for reader pins to drain before
        //  the storage goes away.
        std::unique_lock<std::mutex> lock (registry.sync);
        std::map<zlink::socket_base_t *, monitor_handler_state_t *>::iterator it =
          registry.handlers.find (socket_);
        if (it == registry.handlers.end ())
            return;
        state = it->second;
        registry.handlers.erase (it);
        state->unregistered = true;
        while (state->registry_pins > 0)
            registry.drained.wait (lock);
    }

    stop_monitor_handler_state (state);
}

int set_monitor_handler_state (zlink::socket_base_t *socket_,
                               monitor_handler_state_t *expected_state_,
                               zlink_monitor_handler_fn socket_handler_,
                               monitor_snapshot_provider_fn snapshot_provider_,
                               void *snapshot_subject_,
                               void *socket_handler_userdata_)
{
    if (!socket_) {
        errno = EINVAL;
        return -1;
    }

    monitor_handler_registry_t &registry = monitor_handler_registry ();
    monitor_handler_state_t *state = NULL;
    {
        std::lock_guard<std::mutex> lock (registry.sync);
        std::map<zlink::socket_base_t *, monitor_handler_state_t *>::iterator it =
          registry.handlers.find (socket_);
        if (expected_state_) {
            //  Update path: the caller pinned this exact state. If a
            //  concurrent close already removed or replaced the entry, fail
            //  instead of resurrecting a registration for a closing socket.
            if (it == registry.handlers.end () || it->second != expected_state_
                || expected_state_->unregistered) {
                errno = ESHUTDOWN;
                return -1;
            }
            state = expected_state_;
        } else if (it == registry.handlers.end ()) {
            state = new (std::nothrow) monitor_handler_state_t (socket_);
            if (!state) {
                errno = ENOMEM;
                return -1;
            }
            registry.handlers[socket_] = state;
        } else {
            state = it->second;
        }
    }

    //  The handler must be visible before the dispatch task's first tick (a
    //  tick that sees no handler drops the event it already consumed), so
    //  publish first ??but restore the previous registration on any task
    //  failure so a failed call leaves the handle exactly as it was and no
    //  allocation failure escapes the C surface.
    //  dispatch_sync covers publication through dispatch_task_id storage:
    //  monitor_handler_task takes the same lock before loading the handler,
    //  so an immediate tick cannot run the callback (and a self-close cannot
    //  snapshot a not-yet-stored task ID) until registration has committed
    //  its complete identity.
    zlink::scoped_lock_t dispatch_lock (state->dispatch_sync);
    zlink_monitor_handler_fn prev_handler =
      state->socket_handler.load (std::memory_order_acquire);
    void *prev_userdata = state->socket_handler_userdata.load (std::memory_order_acquire);
    monitor_snapshot_provider_fn prev_provider =
      state->snapshot_provider.load (std::memory_order_acquire);
    void *prev_subject = state->snapshot_subject.load (std::memory_order_acquire);
    state->socket_handler.store (socket_handler_, std::memory_order_release);
    state->socket_handler_userdata.store (socket_handler_userdata_, std::memory_order_release);
    state->snapshot_provider.store (snapshot_provider_, std::memory_order_release);
    state->snapshot_subject.store (snapshot_subject_, std::memory_order_release);
    if (socket_handler_ && state->dispatch_task_id == 0) {
        int failure_errno = 0;
        uint64_t task_id = 0;
        zlink::control_runtime_t *runtime = socket_->get_ctx ()->control_runtime ();
        if (!runtime) {
            failure_errno = ETERM;
        } else {
            try {
                task_id = runtime->add_periodic_task (&monitor_handler_task, state, 10, true);
            }
            catch (const std::bad_alloc &) {
                failure_errno = ENOMEM;
            }
        }
        if (task_id == 0) {
            state->socket_handler.store (prev_handler, std::memory_order_release);
            state->socket_handler_userdata.store (prev_userdata, std::memory_order_release);
            state->snapshot_provider.store (prev_provider, std::memory_order_release);
            state->snapshot_subject.store (prev_subject, std::memory_order_release);
            if (failure_errno != 0)
                errno = failure_errno;
            return -1;
        }
        state->dispatch_task_id = task_id;
    }
    return 0;
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
        return ZLINK_CLOSE_INVALID_HANDLE;

    zlink::socket_base_t *socket = handle.socket;
    const int linger = 0;
    (void) socket->setsockopt (ZLINK_INTERNAL_OPT_LINGER, &linger, sizeof (linger));
    //  The pin keeps the state alive while this close inspects and detaches
    //  it. It is released before any path that unregisters (and then waits
    //  for pins) runs, so self-deadlock is impossible.
    zlink::socket_base_t *raw_monitor_source = NULL;
    zlink::socket_base_t *raw_source_monitor_socket = NULL;
    bool stop_socket_before_close = false;
    bool async_close_via_callback = false;
    bool self_dispatch_close = false;
    {
        monitor_state_pin_t monitor_pin (socket);
        monitor_handler_state_t *monitor_state = monitor_pin.get ();
        raw_monitor_source = raw_monitor_snapshot_subject (monitor_state);
        if (raw_monitor_source && raw_monitor_source != socket) {
            if (monitor_state)
                monitor_state->snapshot_subject.store (NULL, std::memory_order_release);
            raw_source_monitor_socket = raw_monitor_source->detach_monitor_socket (false);
        }
        monitor_debug_logf ("monitor_sid=%d raw_source_sid=%d source_monitor_sid=%d\n",
                            socket ? socket->socket_id () : -1,
                            raw_monitor_source ? raw_monitor_source->socket_id () : -1,
                            raw_source_monitor_socket ? raw_source_monitor_socket->socket_id ()
                                                      : -1);
        const bool had_dispatch_monitor =
          monitor_state && monitor_state->socket_handler.load (std::memory_order_acquire);
        const bool no_dispatch_monitor =
          monitor_state && !monitor_state->socket_handler.load (std::memory_order_acquire);
        stop_socket_before_close = no_dispatch_monitor;
        if (monitor_state) {
            zlink::scoped_lock_t dispatch_lock (monitor_state->dispatch_sync);
            if (had_dispatch_monitor) {
                monitor_state->socket_handler.store (NULL, std::memory_order_release);
                if (zlink::current_monitor_handler_state () != monitor_state) {
                    monitor_state->close_requested.store (true, std::memory_order_release);
                    monitor_state->stop.store (true, std::memory_order_release);
                    stop_socket_before_close = true;
                    async_close_via_callback = true;
                }
            }
            if (!async_close_via_callback
                && zlink::current_monitor_handler_state () != monitor_state
                && monitor_state->callback_depth.load (std::memory_order_acquire) > 0) {
                monitor_state->close_requested.store (true, std::memory_order_release);
                monitor_state->stop.store (true, std::memory_order_release);
                stop_socket_before_close = true;
                async_close_via_callback = true;
            }
        }
        self_dispatch_close =
          monitor_state && zlink::current_monitor_handler_state () == monitor_state;
    }
    if (self_dispatch_close) {
        const zlink_close_result_t rc = zlink_close (monitor);
        if (raw_source_monitor_socket)
            (void) zlink::socket_close_ops_t::request_close (raw_source_monitor_socket, 0);
        if (rc == ZLINK_CLOSE_OK)
            *monitor_p_ = NULL;
        return rc;
    }
    if (async_close_via_callback) {
        if (stop_socket_before_close)
            socket->stop ();
        zlink::part_helper_internal::cleanup_handle (monitor);
        const uint64_t deadline_ms = zlink::clock_t ().now_ms () + 250;
        while (monitor_state_registered (socket)
               && zlink::clock_t ().now_ms () < deadline_ms) {
            zlink::sleep_ms (1);
        }
        if (raw_source_monitor_socket)
            (void) zlink::socket_close_ops_t::request_close (raw_source_monitor_socket, 0);
        *monitor_p_ = NULL;
        return ZLINK_CLOSE_OK;
    }
    if (stop_socket_before_close) {
        socket->stop ();
    }
    const zlink_close_result_t rc = zlink_close (monitor);
    if (raw_source_monitor_socket)
        (void) zlink::socket_close_ops_t::request_close (raw_source_monitor_socket, 0);
    monitor_debug_logf ("monitor_close rc=%d source_monitor_sid=%d\n", static_cast<int> (rc),
                        raw_source_monitor_socket ? raw_source_monitor_socket->socket_id () : -1);
    if (rc == ZLINK_CLOSE_OK)
        *monitor_p_ = NULL;
    return rc;
}
