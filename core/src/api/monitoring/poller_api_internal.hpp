/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_POLLER_API_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_POLLER_API_INTERNAL_HPP_INCLUDED__

#include "utils/precompiled.hpp"

#include <unordered_map>
#include <vector>
#include "api/socket/socket_api_internal.hpp"
#include "core/socket_poller.hpp"

enum poller_subject_kind_t
{
    poller_subject_none = 0,
    poller_subject_fd,
    poller_subject_timer
};

struct poller_registration_t
{
    poller_registration_t () :
        socket (NULL),
        fd (zlink::retired_fd),
        subject (NULL),
        subject_kind (poller_subject_none),
        user_data (NULL),
        events (0),
        owns_completion_processing (false)
    {
    }

    void *socket;
    zlink_fd_t fd;
    void *subject;
    poller_subject_kind_t subject_kind;
    void *user_data;
    short events;
    bool owns_completion_processing;
};

//  Releases per-subject references taken when a registration was added.
void release_poller_registration (const poller_registration_t &registration_);

struct poller_handle_t
{
    poller_handle_t () : tag (0x706f6c6c) {}

    bool check_tag () const { return tag == 0x706f6c6c; }

    uint32_t tag;
    zlink::socket_poller_t poller;
    std::vector<poller_registration_t> registrations;
    std::vector<zlink::socket_poller_t::event_t> native_events;
    std::unordered_map<void *, size_t> socket_registration_indices;
    std::unordered_map<zlink_fd_t, size_t> fd_registration_indices;
};

static inline poller_handle_t *as_poller_handle (void *poller_)
{
    if (!poller_) {
        errno = EFAULT;
        return NULL;
    }

    poller_handle_t *poller = static_cast<poller_handle_t *> (poller_);
    if (!poller->check_tag ()) {
        errno = EFAULT;
        return NULL;
    }
    return poller;
}

void *poller_index_user_data (size_t index_);
bool poller_index_from_user_data (void *user_data_, size_t item_count_, size_t *index_out_);
void poller_set_pollitem_revents_by_identity (zlink_pollitem_t *items_,
                                              int nitems_,
                                              const zlink::socket_poller_t::event_t &event_);
const poller_registration_t *
poller_find_registration_for_native (poller_handle_t *poller_,
                                     const zlink::socket_poller_t::event_t &native_);
int poller_fill_public_event_from_registration (
  const poller_registration_t *registration_,
  const zlink::socket_poller_t::event_t &native_,
  zlink_poller_event_t *event_out_);
int validate_socket_callback_poller_events (socket_handle_t handle_, short events_);
void release_poller_registration (const poller_registration_t &registration_);
int poller_add_registration (poller_handle_t *poller_,
                             zlink::socket_base_t *socket_,
                             void *user_data_,
                             short events_,
                             void *subject_,
                             poller_subject_kind_t subject_kind_);
int poller_add_fd_registration (poller_handle_t *poller_,
                                zlink_fd_t fd_,
                                void *user_data_,
                                short events_,
                                void *subject_,
                                poller_subject_kind_t subject_kind_);
int poller_find_registration_index (poller_handle_t *poller_, void *subject_);
int poller_find_registration_index (poller_handle_t *poller_,
                                    void *subject_,
                                    poller_subject_kind_t subject_kind_);
int poller_find_fd_registration_index (poller_handle_t *poller_,
                                       zlink_fd_t fd_,
                                       poller_subject_kind_t subject_kind_);
int poller_remove_registration_at (poller_handle_t *poller_, int index_);
int poller_remove_all_registrations_for_subject (poller_handle_t *poller_, void *subject_);

#endif
