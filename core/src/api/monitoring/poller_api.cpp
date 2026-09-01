/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <new>
#include <unordered_map>
#include <vector>

#include "api/core/close_result_internal.hpp"
#include "api/core/config_result_internal.hpp"
#include "api/monitoring/poller_api_internal.hpp"
#include "api/monitoring/timer_api_internal.hpp"
#include "utils/clock.hpp"

namespace
{
std::mutex poller_registry_sync;
std::unordered_map<void *, poller_handle_t *> poller_registry;

long remaining_timeout_ms (long timeout_ms_, zlink::clock_t &clock_, uint64_t deadline_ms_)
{
    if (timeout_ms_ < 0)
        return -1;
    if (timeout_ms_ == 0)
        return 0;
    const uint64_t now_ms = clock_.now_ms ();
    if (now_ms >= deadline_ms_)
        return 0;
    return static_cast<long> (deadline_ms_ - now_ms);
}

}

int poller_acquire (void *poller_, poller_handle_t **poller_out_)
{
    if (!poller_ || !poller_out_) {
        errno = EFAULT;
        return -1;
    }
    std::lock_guard<std::mutex> lock (poller_registry_sync);
    const auto it = poller_registry.find (poller_);
    if (it == poller_registry.end ()) {
        errno = EFAULT;
        return -1;
    }
    if (it->second->destroying.load (std::memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }
    it->second->api_refs.fetch_add (1, std::memory_order_acquire);
    *poller_out_ = it->second;
    return 0;
}

void poller_release (poller_handle_t *poller_)
{
    if (poller_)
        poller_->api_refs.fetch_sub (1, std::memory_order_release);
}

int poller_begin_destroy (void *poller_, poller_handle_t **poller_out_)
{
    if (!poller_ || !poller_out_) {
        errno = EFAULT;
        return -1;
    }
    std::lock_guard<std::mutex> lock (poller_registry_sync);
    const auto it = poller_registry.find (poller_);
    if (it == poller_registry.end ()) {
        errno = EFAULT;
        return -1;
    }
    poller_handle_t *poller = it->second;
    if (poller->api_refs.load (std::memory_order_acquire) != 0) {
        errno = EBUSY;
        return 1;
    }
    // Block new API entry while waiting for the operation mutex. Existing
    // API guards have already been accounted for by api_refs above.
    poller->destroying.store (true, std::memory_order_release);
    std::unique_lock<std::mutex> operation_lock (poller->operation_sync);
    poller_registry.erase (it);
    operation_lock.unlock ();
    *poller_out_ = poller;
    return 0;
}

void *zlink_poller_new (void)
{
    poller_handle_t *poller = new (std::nothrow) poller_handle_t;
    if (!poller) {
        errno = ENOMEM;
        return NULL;
    }
    try {
        std::lock_guard<std::mutex> lock (poller_registry_sync);
        poller_registry.emplace (poller, poller);
    } catch (...) {
        delete poller;
        errno = ENOMEM;
        return NULL;
    }
    return static_cast<void *> (poller);
}

zlink_close_result_t zlink_poller_destroy (void **poller_p_)
{
    if (!poller_p_ || !*poller_p_) {
        errno = EFAULT;
        return ZLINK_CLOSE_INVALID_HANDLE;
    }
    poller_handle_t *poller = NULL;
    const int begin_result = poller_begin_destroy (*poller_p_, &poller);
    if (begin_result != 0)
        return begin_result > 0 ? ZLINK_CLOSE_BUSY : ZLINK_CLOSE_INVALID_HANDLE;
    if (!poller)
        return ZLINK_CLOSE_INVALID_HANDLE;
    std::unique_lock<std::mutex> operation_lock (poller->operation_sync, std::try_to_lock);
    if (!operation_lock.owns_lock ()) {
        errno = EBUSY;
        return ZLINK_CLOSE_BUSY;
    }
    // Detach native registrations before releasing the socket lifetime and
    // completion ownership. The socket_poller_t destructor must not retain a
    // raw socket pointer after those ownership gates have been released.
    while (!poller->registrations.empty ()) {
        const poller_registration_t registration = poller->registrations.back ();
        if (registration.socket)
            (void) poller->poller.remove (
              static_cast<zlink::socket_base_t *> (registration.socket));
        else if (registration.fd != zlink::retired_fd)
            (void) poller->poller.remove_fd (registration.fd);
        release_poller_registration (registration);
        poller->registrations.pop_back ();
    }
    poller->tag = 0xdeadbeef;
    // The lock is a member of poller. Release it before delete so the RAII
    // destructor never unlocks storage whose lifetime has ended.
    operation_lock.unlock ();
    delete poller;
    *poller_p_ = NULL;
    return ZLINK_CLOSE_OK;
}

int zlink_poller_size (void *poller_, zlink_config_result_t *error_out_)
{
    poller_api_guard_t guard (poller_);
    poller_handle_t *poller = guard.get ();
    if (!poller) {
        if (error_out_)
            *error_out_ = ZLINK_CONFIG_INVALID_HANDLE;
        return -1;
    }
    std::lock_guard<std::mutex> operation_lock (poller->operation_sync);
    if (poller->wait_active) {
        errno = EBUSY;
        if (error_out_)
            *error_out_ = ZLINK_CONFIG_BUSY;
        return -1;
    }
    if (error_out_)
        *error_out_ = ZLINK_CONFIG_OK;
    return poller->poller.size ();
}

zlink_config_result_t
zlink_poller_add_fd (void *poller_, zlink_fd_t fd_, void *user_data_, short events_)
{
    poller_api_guard_t guard (poller_);
    poller_handle_t *poller = guard.get ();
    if (!poller)
        return ZLINK_CONFIG_INVALID_HANDLE;
    std::lock_guard<std::mutex> operation_lock (poller->operation_sync);
    if (poller->wait_active) {
        errno = EBUSY;
        return ZLINK_CONFIG_BUSY;
    }
    if (validate_fd_poller_event_mask (events_) != 0)
        return zlink::config_result_internal::from_errno (errno);
    if (poller_find_fd_registration_index (poller, fd_, poller_subject_fd) >= 0) {
        errno = EEXIST;
        return ZLINK_CONFIG_CONFLICT;
    }
    return zlink::config_result_internal::from_rc (
      poller_add_fd_registration (poller, fd_, user_data_, events_, NULL, poller_subject_fd));
}

zlink_config_result_t zlink_poller_modify_fd (void *poller_, zlink_fd_t fd_, short events_)
{
    poller_api_guard_t guard (poller_);
    poller_handle_t *poller = guard.get ();
    if (!poller)
        return ZLINK_CONFIG_INVALID_HANDLE;
    std::lock_guard<std::mutex> operation_lock (poller->operation_sync);
    if (poller->wait_active) {
        errno = EBUSY;
        return ZLINK_CONFIG_BUSY;
    }
    if (validate_fd_poller_event_mask (events_) != 0)
        return zlink::config_result_internal::from_errno (errno);
    const int index = poller_find_fd_registration_index (poller, fd_, poller_subject_fd);
    if (index < 0) {
        errno = ENOENT;
        return ZLINK_CONFIG_NOT_FOUND;
    }
    if (poller->poller.modify_fd (fd_, events_) != 0)
        return zlink::config_result_internal::from_errno (errno);
    poller->registrations[static_cast<size_t> (index)].events = events_;
    return ZLINK_CONFIG_OK;
}

zlink_config_result_t zlink_poller_remove_fd (void *poller_, zlink_fd_t fd_)
{
    poller_api_guard_t guard (poller_);
    poller_handle_t *poller = guard.get ();
    if (!poller)
        return ZLINK_CONFIG_INVALID_HANDLE;
    std::lock_guard<std::mutex> operation_lock (poller->operation_sync);
    if (poller->wait_active) {
        errno = EBUSY;
        return ZLINK_CONFIG_BUSY;
    }
    const int index = poller_find_fd_registration_index (poller, fd_, poller_subject_fd);
    if (index < 0) {
        errno = ENOENT;
        return ZLINK_CONFIG_NOT_FOUND;
    }
    return zlink::config_result_internal::from_rc (poller_remove_registration_at (poller, index));
}

zlink_config_result_t zlink_poller_add_timer (void *poller_, void *timer_, void *user_data_)
{
    poller_api_guard_t guard (poller_);
    poller_handle_t *poller = guard.get ();
    timer_handle_t *timer = as_timer_handle (timer_);
    if (!poller || !timer)
        return ZLINK_CONFIG_INVALID_HANDLE;
    std::lock_guard<std::mutex> operation_lock (poller->operation_sync);
    if (poller->wait_active) {
        errno = EBUSY;
        return ZLINK_CONFIG_BUSY;
    }

    if (poller_find_registration_index (poller, timer_, poller_subject_timer)
        >= 0) {
        errno = EEXIST;
        return ZLINK_CONFIG_CONFLICT;
    }

    if (timer_handle_acquire_poller_ref (timer) != 0)
        return zlink::config_result_internal::from_errno (errno);

    zlink_fd_t fd = 0;
    if (timer_handle_signaler_fd (timer, &fd) != 0
        || poller_add_fd_registration (poller, fd, user_data_, ZLINK_POLLIN, timer_,
                                       poller_subject_timer)
             != 0) {
        const int err = errno;
        timer_handle_release_poller_ref (timer);
        errno = err;
        return zlink::config_result_internal::from_errno (err);
    }
    return ZLINK_CONFIG_OK;
}

zlink_config_result_t zlink_poller_remove_timer (void *poller_, void *timer_)
{
    poller_api_guard_t guard (poller_);
    poller_handle_t *poller = guard.get ();
    timer_handle_t *timer = as_timer_handle (timer_);
    if (!poller || !timer)
        return ZLINK_CONFIG_INVALID_HANDLE;
    std::lock_guard<std::mutex> operation_lock (poller->operation_sync);
    if (poller->wait_active) {
        errno = EBUSY;
        return ZLINK_CONFIG_BUSY;
    }

    const int index = poller_find_registration_index (poller, timer_, poller_subject_timer);
    if (index < 0) {
        errno = ENOENT;
        return ZLINK_CONFIG_NOT_FOUND;
    }
    return zlink::config_result_internal::from_rc (poller_remove_registration_at (poller, index));
}

int zlink_poller_wait (void *poller_,
                       zlink_poller_event_t *events_,
                       int n_events_,
                       long timeout_,
                       zlink_config_result_t *error_out_)
{
    poller_api_guard_t guard (poller_);
    poller_handle_t *poller = guard.get ();
    if (!poller) {
        if (error_out_)
            *error_out_ = ZLINK_CONFIG_INVALID_HANDLE;
        return -1;
    }
    {
        std::lock_guard<std::mutex> operation_lock (poller->operation_sync);
        if (n_events_ <= 0 || !events_) {
            errno = EINVAL;
            if (error_out_)
                *error_out_ = ZLINK_CONFIG_INVALID_ARGUMENT;
            return -1;
        }
        if (poller->wait_active) {
            errno = EBUSY;
            if (error_out_)
                *error_out_ = ZLINK_CONFIG_BUSY;
            return -1;
        }
        poller->wait_active = true;
    }

    struct wait_scope_t
    {
        explicit wait_scope_t (poller_handle_t *poller_) : poller (poller_) {}
        ~wait_scope_t ()
        {
            std::lock_guard<std::mutex> lock (poller->operation_sync);
            poller->wait_active = false;
        }
        poller_handle_t *poller;
    } wait_scope (poller);

    zlink::clock_t clock;
    const uint64_t deadline_ms =
      timeout_ > 0 ? clock.now_ms () + static_cast<uint64_t> (timeout_) : 0;
    while (true) {
        int native_capacity = 0;
        {
            std::lock_guard<std::mutex> operation_lock (poller->operation_sync);
            native_capacity =
              std::max (n_events_, static_cast<int> (poller->registrations.size ()));
            try {
                poller->native_events.resize (static_cast<size_t> (native_capacity));
            } catch (...) {
                errno = ENOMEM;
                if (error_out_)
                    *error_out_ = ZLINK_CONFIG_INTERNAL_ERROR;
                return -1;
            }
        }
        const int rc = poller->poller.wait (
          poller->native_events.empty () ? NULL : poller->native_events.data (), native_capacity,
          remaining_timeout_ms (timeout_, clock, deadline_ms));
        if (rc < 0) {
            if (errno == EAGAIN) {
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

        // wait_active prevents registration mutation for the duration of
        // this call, so conversion can inspect the stable registration table
        // without holding operation_sync across socket readiness calls.
        int public_count = 0;
        for (int i = 0; i < rc; ++i) {
            const poller_registration_t *registration =
              poller_find_registration_for_native (poller, poller->native_events[i]);

            zlink_poller_event_t candidate;
            if (poller_fill_public_event_from_registration (registration,
                                                            poller->native_events[i], &candidate)
                != 0) {
                if (error_out_)
                    *error_out_ = zlink::config_result_internal::from_errno (errno);
                return -1;
            }
            if (candidate.events == 0)
                continue;
            int duplicate_index = -1;
            for (int j = 0; j < public_count; ++j) {
                if (events_[j].source_kind == candidate.source_kind
                    && events_[j].socket == candidate.socket && events_[j].fd == candidate.fd
                    && events_[j].timer == candidate.timer) {
                    duplicate_index = j;
                    break;
                }
            }
            if (duplicate_index >= 0) {
                events_[duplicate_index].events =
                  static_cast<short> (events_[duplicate_index].events | candidate.events);
            } else if (public_count < n_events_) {
                events_[public_count++] = candidate;
            }
        }

        if (public_count > 0) {
            for (int i = 0; i < public_count; ++i) {
                if (events_[i].source_kind != ZLINK_POLLER_SOURCE_SOCKET
                    || !events_[i].socket)
                    continue;
                const int primary_index = poller_find_registration_index (
                  poller, events_[i].socket, poller_subject_none);
                if (primary_index < 0)
                    continue;
                const poller_registration_t &primary =
                  poller->registrations[static_cast<size_t> (primary_index)];
                const short other_events =
                  static_cast<short> (primary.events & ~ZLINK_POLLIN);
                uint32_t ready_events = 0;
                if (other_events != 0
                    && static_cast<zlink::socket_base_t *> (primary.socket)
                           ->get_events (other_events, &ready_events)
                         == 0) {
                    events_[i].events =
                      static_cast<short> (events_[i].events | ready_events);
                }
            }
            if (error_out_)
                *error_out_ = ZLINK_CONFIG_OK;
            return public_count;
        }
    }
}
