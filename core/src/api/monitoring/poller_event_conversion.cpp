/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <unordered_map>

#include "api/monitoring/poller_api_internal.hpp"

namespace
{
bool registration_matches_native (const poller_registration_t &registration_,
                                  const zlink::socket_poller_t::event_t &native_)
{
    return (registration_.socket && registration_.socket == native_.socket)
           || (!registration_.socket && !native_.socket && registration_.fd == native_.fd);
}

}

void poller_set_pollitem_revents_by_identity (zlink_pollitem_t *items_,
                                              int nitems_,
                                              const zlink::socket_poller_t::event_t &event_)
{
    for (int j = 0; j < nitems_; ++j) {
        if ((items_[j].socket && items_[j].socket == event_.socket)
            || (!items_[j].socket && items_[j].fd == event_.fd)) {
            items_[j].revents = event_.events;
            return;
        }
    }
}

const poller_registration_t *
poller_find_registration_for_native (poller_handle_t *poller_,
                                     const zlink::socket_poller_t::event_t &native_)
{
    if (!poller_)
        return NULL;
    size_t native_index = 0;
    if (poller_index_from_user_data (native_.user_data, poller_->registrations.size (),
                                     &native_index)) {
        const poller_registration_t &registration = poller_->registrations[native_index];
        if (registration_matches_native (registration, native_))
            return &registration;
    }
    if (native_.socket) {
        std::unordered_map<void *, size_t>::const_iterator it =
          poller_->socket_registration_indices.find (native_.socket);
        if (it != poller_->socket_registration_indices.end ()
            && it->second < poller_->registrations.size ()) {
            return &poller_->registrations[it->second];
        }
    } else {
        std::unordered_map<zlink_fd_t, size_t>::const_iterator it =
          poller_->fd_registration_indices.find (native_.fd);
        if (it != poller_->fd_registration_indices.end ()
            && it->second < poller_->registrations.size ()) {
            return &poller_->registrations[it->second];
        }
    }
    for (size_t i = 0; i < poller_->registrations.size (); ++i) {
        const poller_registration_t &registration = poller_->registrations[i];
        if (registration.socket && registration.socket == native_.socket)
            return &registration;
        if (!registration.socket && registration.fd == native_.fd)
            return &registration;
    }
    return NULL;
}

int poller_fill_public_event_from_registration (
  const poller_registration_t *registration_,
  const zlink::socket_poller_t::event_t &native_,
  zlink_poller_event_t *event_out_)
{
    if (!event_out_) {
        errno = EFAULT;
        return -1;
    }

    memset (event_out_, 0, sizeof (*event_out_));
    event_out_->fd = 0;

    if (!registration_) {
        event_out_->source_kind =
          native_.socket ? ZLINK_POLLER_SOURCE_SOCKET : ZLINK_POLLER_SOURCE_FD;
        event_out_->socket = native_.socket;
        event_out_->fd = native_.fd;
        event_out_->timer = NULL;
        event_out_->user_data = native_.user_data;
        event_out_->events = native_.events;
        return 0;
    }

    if (registration_->socket) {
        event_out_->source_kind = ZLINK_POLLER_SOURCE_SOCKET;
        event_out_->socket = native_.socket;
        event_out_->fd = native_.fd;
        event_out_->timer = NULL;
        event_out_->user_data = registration_->user_data;
        event_out_->events = native_.events;
        return 0;
    }

    if (registration_->subject_kind == poller_subject_timer) {
        event_out_->source_kind = ZLINK_POLLER_SOURCE_TIMER;
        event_out_->socket = NULL;
        event_out_->fd = native_.fd;
        event_out_->timer = registration_->subject;
        event_out_->user_data = registration_->user_data;
        event_out_->events = native_.events;
        return 0;
    }

    event_out_->source_kind = ZLINK_POLLER_SOURCE_FD;
    event_out_->socket = NULL;
    event_out_->fd = native_.fd;
    event_out_->timer = NULL;
    event_out_->user_data = registration_->user_data;
    event_out_->events = native_.events;
    return 0;
}
