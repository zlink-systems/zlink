/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_MONITOR_API_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_MONITOR_API_INTERNAL_HPP_INCLUDED__

#include "zlink.h"

#include <atomic>

#include "sockets/common/socket_base.hpp"
#include "utils/mutex.hpp"

typedef int (*monitor_snapshot_provider_fn) (void *subject_, zlink_monitor_status_t *out_);

//  Private version-4 raw monitor frame. Keep the producer and consumer on one
//  layout definition; this payload is not part of the public monitor ABI.
struct socket_monitor_internal_event_t
{
    zlink_monitor_event_t event;
    uint64_t connection_id;
    uint32_t internal_flags;
};

struct monitor_handler_state_t
{
    monitor_handler_state_t (zlink::socket_base_t *socket_) :
        socket (socket_),
        socket_handler (NULL),
        socket_handler_userdata (NULL),
        snapshot_provider (NULL),
        snapshot_subject (NULL),
        stop (false),
        callback_depth (0),
        close_requested (false),
        dispatch_task_id (0),
        registry_pins (0),
        unregistered (false)
    {
    }

    zlink::socket_base_t *socket;
    std::atomic<zlink_monitor_handler_fn> socket_handler;
    std::atomic<void *> socket_handler_userdata;
    std::atomic<monitor_snapshot_provider_fn> snapshot_provider;
    std::atomic<void *> snapshot_subject;
    std::atomic<bool> stop;
    std::atomic<int> callback_depth;
    std::atomic<bool> close_requested;
    zlink::mutex_t dispatch_sync;
    uint64_t dispatch_task_id;
    //  Guarded by the registry mutex: readers pin the state through the
    //  registry, and the unregister/delete path waits until every pin is
    //  released before the storage goes away.
    uint32_t registry_pins;
    bool unregistered;
};

namespace zlink
{
class monitor_dispatch_context_t
{
  public:
    explicit monitor_dispatch_context_t (monitor_handler_state_t *state_);
    ~monitor_dispatch_context_t ();

    static monitor_handler_state_t *current_state ();
    static void *current_handle ();

  private:
    monitor_handler_state_t *_previous_state;
};

monitor_handler_state_t *current_monitor_handler_state ();
void *current_monitor_dispatch_handle ();
}

//  Registry readers hold a pin for the whole time they dereference the
//  returned state; a concurrent close waits for the pins to drain before it
//  deletes the storage. Never call a state-deleting path while pinned.
monitor_handler_state_t *pin_monitor_handler_state (zlink::socket_base_t *socket_);
void unpin_monitor_handler_state (monitor_handler_state_t *state_);

//  RAII wrapper for the pin contract above.
class monitor_state_pin_t
{
  public:
    explicit monitor_state_pin_t (zlink::socket_base_t *socket_) :
        _state (pin_monitor_handler_state (socket_))
    {
    }
    ~monitor_state_pin_t ()
    {
        if (_state)
            unpin_monitor_handler_state (_state);
    }
    monitor_handler_state_t *get () const { return _state; }
    void release ()
    {
        if (_state) {
            unpin_monitor_handler_state (_state);
            _state = NULL;
        }
    }

  private:
    monitor_handler_state_t *_state;
    monitor_state_pin_t (const monitor_state_pin_t &);
    monitor_state_pin_t &operator= (const monitor_state_pin_t &);
};

zlink::socket_base_t *raw_monitor_snapshot_subject (monitor_handler_state_t *state_);
void clear_raw_monitor_snapshot_subjects (zlink::socket_base_t *source_);
void unregister_monitor_handlers (zlink::socket_base_t *socket_);

//  expected_state_ == NULL: creation path (monitor open, unpublished socket).
//  Non-NULL: update path — the caller pinned that state, and the call fails
//  with ESHUTDOWN unless the registry still maps socket_ to exactly it.
int set_monitor_handler_state (zlink::socket_base_t *socket_,
                               monitor_handler_state_t *expected_state_,
                               zlink_monitor_handler_fn socket_handler_,
                               monitor_snapshot_provider_fn snapshot_provider_,
                               void *snapshot_subject_,
                               void *socket_handler_userdata_);

int socket_monitor_snapshot_provider (void *subject_, zlink_monitor_status_t *out_);
void *open_socket_monitor_internal (
  void *socket_,
  zlink_socket_monitor_event_mask_t events_,
  int event_version_);

int recv_socket_monitor_event_unchecked (void *monitor_socket_,
                                         zlink_monitor_event_t *event_,
                                         int flags_);
int recv_socket_monitor_event_internal (void *monitor_socket_,
                                        zlink_monitor_event_t *event_,
                                        uint64_t *connection_id_out_,
                                        uint32_t *internal_flags_out_,
                                        int flags_);
int require_monitor_recv_model (void *monitor_);

#endif
