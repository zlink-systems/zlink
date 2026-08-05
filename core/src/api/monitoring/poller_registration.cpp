/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <stdint.h>

#include "api/monitoring/poller_api_internal.hpp"
#include "api/monitoring/timer_api_internal.hpp"

void *poller_index_user_data (size_t index_)
{
    return reinterpret_cast<void *> (static_cast<uintptr_t> (index_) + 1u);
}

bool poller_index_from_user_data (void *user_data_, size_t item_count_, size_t *index_out_)
{
    if (!user_data_ || !index_out_)
        return false;
    const uintptr_t encoded = reinterpret_cast<uintptr_t> (user_data_);
    if (encoded == 0u)
        return false;
    const uintptr_t index_value = encoded - 1u;
    if (index_value >= static_cast<uintptr_t> (item_count_))
        return false;
    *index_out_ = static_cast<size_t> (index_value);
    return true;
}

int poller_add_registration (poller_handle_t *poller_,
                             zlink::socket_base_t *socket_,
                             void *user_data_,
                             short events_,
                             void *subject_,
                             poller_subject_kind_t subject_kind_)
{
    if (!poller_ || !socket_) {
        errno = EFAULT;
        return -1;
    }
    const size_t registration_index = poller_->registrations.size ();
    if (poller_->poller.add (socket_, poller_index_user_data (registration_index), events_) != 0)
        return -1;

    poller_registration_t registration;
    registration.socket = static_cast<void *> (socket_);
    registration.fd = zlink::retired_fd;
    registration.subject = subject_;
    registration.subject_kind = subject_kind_;
    registration.user_data = user_data_;
    registration.events = events_;
    poller_->registrations.push_back (registration);
    poller_->socket_registration_indices[registration.socket] = poller_->registrations.size () - 1;
    return 0;
}

int poller_add_fd_registration (poller_handle_t *poller_,
                                zlink_fd_t fd_,
                                void *user_data_,
                                short events_,
                                void *subject_,
                                poller_subject_kind_t subject_kind_)
{
    if (!poller_) {
        errno = EFAULT;
        return -1;
    }
    const size_t registration_index = poller_->registrations.size ();
    if (poller_->poller.add_fd (fd_, poller_index_user_data (registration_index), events_) != 0)
        return -1;

    poller_registration_t registration;
    registration.socket = NULL;
    registration.fd = fd_;
    registration.subject = subject_;
    registration.subject_kind = subject_kind_;
    registration.user_data = user_data_;
    registration.events = events_;
    poller_->registrations.push_back (registration);
    poller_->fd_registration_indices[registration.fd] = poller_->registrations.size () - 1;
    return 0;
}

int poller_find_registration_index (poller_handle_t *poller_, void *subject_)
{
    if (!poller_)
        return -1;
    for (size_t i = 0; i < poller_->registrations.size (); ++i) {
        if (poller_->registrations[i].subject == subject_)
            return static_cast<int> (i);
    }
    return -1;
}

int poller_find_registration_index (poller_handle_t *poller_,
                                    void *subject_,
                                    poller_subject_kind_t subject_kind_)
{
    if (!poller_)
        return -1;
    for (size_t i = 0; i < poller_->registrations.size (); ++i) {
        if (poller_->registrations[i].subject == subject_
            && poller_->registrations[i].subject_kind == subject_kind_) {
            return static_cast<int> (i);
        }
    }
    return -1;
}

int poller_find_fd_registration_index (poller_handle_t *poller_,
                                       zlink_fd_t fd_,
                                       poller_subject_kind_t subject_kind_)
{
    if (!poller_)
        return -1;
    for (size_t i = 0; i < poller_->registrations.size (); ++i) {
        if (!poller_->registrations[i].socket && poller_->registrations[i].fd == fd_
            && poller_->registrations[i].subject_kind == subject_kind_) {
            return static_cast<int> (i);
        }
    }
    return -1;
}

int poller_remove_registration_at (poller_handle_t *poller_, int index_)
{
    if (!poller_ || index_ < 0 || static_cast<size_t> (index_) >= poller_->registrations.size ()) {
        errno = EINVAL;
        return -1;
    }

    const poller_registration_t registration = poller_->registrations[static_cast<size_t> (index_)];
    const int rc =
      registration.socket
        ? poller_->poller.remove (static_cast<zlink::socket_base_t *> (registration.socket))
        : poller_->poller.remove_fd (registration.fd);
    if (rc == 0) {
        release_poller_registration (registration);
        if (registration.socket)
            poller_->socket_registration_indices.erase (registration.socket);
        else
            poller_->fd_registration_indices.erase (registration.fd);
        const size_t index = static_cast<size_t> (index_);
        const size_t last = poller_->registrations.size () - 1;
        if (index != last) {
            poller_->registrations[index] = poller_->registrations[last];
            const poller_registration_t &moved = poller_->registrations[index];
            if (moved.socket) {
                poller_->socket_registration_indices[moved.socket] = index;
                (void) poller_->poller.modify_user_data (
                  static_cast<zlink::socket_base_t *> (moved.socket),
                  poller_index_user_data (index));
            } else {
                poller_->fd_registration_indices[moved.fd] = index;
                (void) poller_->poller.modify_fd_user_data (moved.fd,
                                                            poller_index_user_data (index));
            }
        }
        poller_->registrations.pop_back ();
    }
    return rc;
}

int poller_remove_all_registrations_for_subject (poller_handle_t *poller_, void *subject_)
{
    if (!poller_) {
        errno = EFAULT;
        return -1;
    }

    bool removed = false;
    while (true) {
        const int index = poller_find_registration_index (poller_, subject_);
        if (index < 0)
            break;
        if (poller_remove_registration_at (poller_, index) != 0)
            return -1;
        removed = true;
    }

    if (!removed) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void release_poller_registration (const poller_registration_t &registration_)
{
    if (registration_.socket && registration_.owns_completion_processing) {
        static_cast<zlink::socket_base_t *> (registration_.socket)
          ->release_completion_poller ();
    }
    switch (registration_.subject_kind) {
        case poller_subject_timer:
            timer_handle_release_poller_ref (as_timer_handle (registration_.subject));
            break;
        case poller_subject_fd:
            break;
        default:
            break;
    }
}
