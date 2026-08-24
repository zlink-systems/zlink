/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_POLLER_API_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_POLLER_API_INTERNAL_HPP_INCLUDED__

#include "utils/precompiled.hpp"

#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
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
        owns_completion_processing (false),
        owns_socket_lifetime (false)
    {
    }

    void *socket;
    zlink_fd_t fd;
    void *subject;
    poller_subject_kind_t subject_kind;
    void *user_data;
    short events;
    bool owns_completion_processing;
    bool owns_socket_lifetime;
};

//  Releases per-subject references taken when a registration was added.
void release_poller_registration (const poller_registration_t &registration_);

struct poller_handle_t
{
    poller_handle_t () : tag (0x706f6c6c), api_refs (0), destroying (false), wait_active (false) {}

    bool check_tag () const { return tag == 0x706f6c6c; }

    uint32_t tag;
    // The registry pins the object before any API dereferences the opaque
    // pointer. This prevents destroy from freeing the mutex between handle
    // validation and operation entry.
    std::atomic<size_t> api_refs;
    std::atomic<bool> destroying;
    bool wait_active;
    // All operations, including the blocking wait, use this mutex. Destroy
    // takes it with try_to_lock so a concurrent wait returns BUSY before the
    // handle or its registration storage can be freed.
    std::mutex operation_sync;
    zlink::socket_poller_t poller;
    std::vector<poller_registration_t> registrations;
    std::vector<zlink::socket_poller_t::event_t> native_events;
    std::unordered_map<void *, size_t> socket_registration_indices;
    std::unordered_map<zlink_fd_t, size_t> fd_registration_indices;
};

int poller_acquire (void *poller_, poller_handle_t **poller_out_);
void poller_release (poller_handle_t *poller_);
int poller_begin_destroy (void *poller_, poller_handle_t **poller_out_);

class poller_api_guard_t
{
  public:
    explicit poller_api_guard_t (void *poller_) : poller (NULL)
    {
        if (poller_acquire (poller_, &poller) != 0)
            poller = NULL;
    }

    ~poller_api_guard_t ()
    {
        if (poller)
            poller_release (poller);
    }

    explicit operator bool () const { return poller != NULL; }
    poller_handle_t *get () const { return poller; }

  private:
    poller_handle_t *poller;
};

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
int validate_socket_poller_event_mask (short events_, bool allow_completion_);
int validate_fd_poller_event_mask (short events_);
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
