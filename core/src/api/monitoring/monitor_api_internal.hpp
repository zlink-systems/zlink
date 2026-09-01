/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_MONITOR_API_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_MONITOR_API_INTERNAL_HPP_INCLUDED__

#include "zlink.h"

#include <atomic>

#include "api/socket/socket_api_internal.hpp"

//  Private version-4 raw monitor frame. Keep the producer and consumer on one
//  layout definition; this payload is not part of the public monitor ABI.
struct socket_monitor_internal_event_t
{
    zlink_monitor_event_t event;
    uint64_t connection_id;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    uint32_t transport_lane;
    uint32_t internal_flags;
};

inline uint64_t socket_monitor_event_accounted_bytes ()
{
    return static_cast<uint64_t> (sizeof (socket_monitor_internal_event_t))
           + static_cast<uint64_t> (sizeof (zlink_msg_t));
}

inline bool socket_monitor_default_hwm_bytes (uint64_t *out_)
{
    const uint64_t event_charge = socket_monitor_event_accounted_bytes ();
    const uint64_t default_event_depth = 4096;
    if (!out_ || event_charge > UINT64_MAX / default_event_depth)
        return false;
    *out_ = default_event_depth * event_charge;
    return true;
}

struct monitor_pull_state_t
{
    monitor_pull_state_t (zlink::socket_base_t *socket_, void *snapshot_subject_) :
        socket (socket_),
        snapshot_subject (snapshot_subject_),
        registry_pins (0),
        unregistered (false)
    {
    }

    zlink::socket_base_t *socket;
    std::atomic<void *> snapshot_subject;
    //  Guarded by the pull-state registry mutex. Readers pin the state for
    //  status/close association lookup; unregister waits for those pins.
    uint32_t registry_pins;
    bool unregistered;
};

//  Registry readers hold a pin for the whole time they dereference the
//  returned state; a concurrent close waits for the pins to drain before it
//  deletes the storage. Never call a state-deleting path while pinned.
monitor_pull_state_t *pin_monitor_pull_state (zlink::socket_base_t *socket_);
void unpin_monitor_pull_state (monitor_pull_state_t *state_);

//  RAII wrapper for the pin contract above.
class monitor_pull_state_pin_t
{
  public:
    explicit monitor_pull_state_pin_t (zlink::socket_base_t *socket_) :
        _state (pin_monitor_pull_state (socket_))
    {
    }
    ~monitor_pull_state_pin_t ()
    {
        if (_state)
            unpin_monitor_pull_state (_state);
    }
    monitor_pull_state_t *get () const { return _state; }
    void release ()
    {
        if (_state) {
            unpin_monitor_pull_state (_state);
            _state = NULL;
        }
    }

  private:
    monitor_pull_state_t *_state;
    monitor_pull_state_pin_t (const monitor_pull_state_pin_t &);
    monitor_pull_state_pin_t &operator= (const monitor_pull_state_pin_t &);
};

socket_handle_t monitor_snapshot_subject_handle (monitor_pull_state_t *state_);
void clear_raw_monitor_snapshot_subjects (zlink::socket_base_t *source_);
void unregister_monitor_pull_state (zlink::socket_base_t *socket_);
int register_monitor_pull_state (zlink::socket_base_t *socket_,
                                 void *snapshot_subject_);

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
int require_monitor_pull_handle (void *monitor_);

#endif
