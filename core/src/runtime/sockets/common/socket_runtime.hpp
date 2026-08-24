/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SOCKET_RUNTIME_HPP_INCLUDED__
#define __ZLINK_SOCKET_RUNTIME_HPP_INCLUDED__

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "core/endpoint.hpp"
#include "core/mailbox.hpp"
#include "core/own.hpp"
#include "core/pipe.hpp"
#include "core/poller.hpp"
#include "core/thread.hpp"
#include "utils/atomic_counter.hpp"
#include "utils/condition_variable.hpp"
#include "utils/mutex.hpp"
#include "zlink.h"

namespace zlink
{
namespace request_timeout
{
struct task_t;
}

class io_thread_t;
class mailbox_t;
class socket_base_t;

enum
{
    socket_monitor_max_values = 4,
    //  Mirror the public ZLINK_MONITOR_EVENT_FLAG_* bit values; the record's
    //  internal_flags is copied straight into the wire event's flags field.
    socket_monitor_internal_connection_ready_edge = 1u << 0,
    socket_monitor_internal_send_flow_writable = 1u << 1,
    socket_monitor_internal_flow_state_stale_generation = 1u << 2,
    socket_monitor_internal_flow_state_stale_epoch = 1u << 3
};

struct socket_monitor_event_record_t
{
    socket_monitor_event_record_t () :
        event (0),
        values_count (0),
        internal_flags (0),
        transport_pair_id (0),
        transport_pair_generation (0),
        transport_lane (transport_lane_application)
    {
        memset (values, 0, sizeof (values));
        memset (&routing_id, 0, sizeof (routing_id));
    }

    uint64_t event;
    uint64_t values[socket_monitor_max_values];
    uint64_t values_count;
    zlink_routing_id_t routing_id;
    endpoint_uri_pair_t endpoint_uri_pair;
    uint32_t internal_flags;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    transport_lane_t transport_lane;
};

typedef void (socket_monitor_worker_idle_fn) (void *);

struct socket_endpoint_pipe_t
{
    socket_endpoint_pipe_t () : endpoint (NULL), pipe (NULL), local_type (endpoint_type_none) {}
    socket_endpoint_pipe_t (own_t *endpoint_, pipe_t *pipe_, endpoint_type_t local_type_) :
        endpoint (endpoint_), pipe (pipe_), local_type (local_type_)
    {
    }
    socket_endpoint_pipe_t (own_t *endpoint_,
                            pipe_t *pipe_,
                            endpoint_type_t local_type_,
                            const std::shared_ptr<transport_pair_state_t> &pair_state_) :
        endpoint (endpoint_),
        pipe (pipe_),
        local_type (local_type_),
        transport_pair_state (pair_state_)
    {
    }

    own_t *endpoint;
    pipe_t *pipe;
    endpoint_type_t local_type;
    //  Paired transports keep the shared pair state here so that terminating
    //  one endpoint can stop the whole pair from reconnecting. Both lanes of
    //  one connect share this state and the same endpoint key.
    std::shared_ptr<transport_pair_state_t> transport_pair_state;
};
typedef std::multimap<std::string, socket_endpoint_pipe_t> socket_endpoints_t;

class socket_inprocs_t
{
  public:
    void emplace (const char *endpoint_uri_, pipe_t *pipe_);
    int erase_pipes (const std::string &endpoint_uri_str_);
    void erase_pipe (const pipe_t *pipe_);

  private:
    typedef std::multimap<std::string, pipe_t *> map_t;
    map_t _inprocs;
};

struct socket_endpoint_runtime_t
{
    typedef array_t<pipe_t, 3> attached_pipes_t;

    socket_endpoints_t endpoints;
    socket_inprocs_t inprocs;
    attached_pipes_t attached_pipes;
    zlink_routing_id_t last_recv_source_rid;
    bool last_recv_source_rid_valid;
    std::string last_endpoint;

    socket_endpoint_runtime_t () : last_recv_source_rid (), last_recv_source_rid_valid (false) {}

    void attach_pipe (pipe_t *pipe_);
    void detach_pipe (pipe_t *pipe_);
    size_t attached_pipe_count () const;
    bool has_attached_pipes () const;
    pipe_t *attached_pipe (size_t index_);
    const pipe_t *attached_pipe (size_t index_) const;

    void store_last_recv_source_rid (const zlink_routing_id_t *source_rid_);
    void clear_last_recv_source_rid ();
    bool copy_last_recv_source_rid (zlink_routing_id_t *out_) const;
    void set_last_endpoint (const std::string &endpoint_);
    const std::string &last_endpoint_uri () const;
};

class socket_command_runtime_t
{
  public:
    socket_command_runtime_t () : last_command_tsc (0), recv_ticks (0) {}

    bool should_skip_throttled_command_poll (uint64_t tsc_);
    bool should_poll_commands_after_recv (int inbound_poll_rate_);
    void reset_recv_ticks ();
    bool should_block_on_recv () const;

  private:
    uint64_t last_command_tsc;
    int recv_ticks;
};

struct socket_monitor_runtime_t
{
    socket_monitor_runtime_t () :
        socket (NULL),
        events (0),
        events_atomic (0),
        lossy (true),
        queue_hwm_bytes (0),
        queue_accounted_bytes (0),
        event_accounted_bytes (0),
        queue_stop (false),
        task_id (0),
        task_running (false),
        owns_async_command_processing (false)
    {
    }

    uint32_t ready_count () const;
    bool mark_ready_connection (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                const unsigned char *routing_id_,
                                size_t routing_id_size_,
                                uint32_t *ready_count_out_,
                                uint64_t transport_pair_id_ = 0,
                                uint64_t transport_pair_generation_ = 0);
    bool erase_ready_connection (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                 const unsigned char *routing_id_,
                                 size_t routing_id_size_,
                                 uint32_t *ready_count_out_,
                                 uint64_t transport_pair_id_ = 0,
                                 uint64_t transport_pair_generation_ = 0);
    bool erase_ready_connection_for_endpoint (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                              uint32_t *ready_count_out_,
                                              uint64_t transport_pair_id_ = 0,
                                              uint64_t transport_pair_generation_ = 0);
    bool mark_transport_pair_lane_ready (
      const endpoint_uri_pair_t &endpoint_uri_pair_,
      transport_lane_t lane_,
      uint64_t pair_id_,
      uint64_t generation_);
    void erase_transport_pair_readiness_for_endpoint (
      const endpoint_uri_pair_t &endpoint_uri_pair_);
    void reset_worker_state (uint64_t hwm_bytes_, uint64_t event_accounted_bytes_);
    void start_task (uint64_t task_id_);
    bool dequeue_worker_event_nowait (socket_monitor_event_record_t *out_);
    void requeue_worker_event_front (const socket_monitor_event_record_t &record_);
    void complete_worker_event ();
    void enqueue_worker_event (const socket_monitor_event_record_t &record_);
    void stop_task ();

    void *socket;
    int64_t events;
    std::atomic<int64_t> events_atomic;
    bool lossy;
    mutable mutex_t sync;
    mutex_t queue_sync;
    condition_variable_t queue_cv;
    std::deque<socket_monitor_event_record_t> queue;
    uint64_t queue_hwm_bytes;
    uint64_t queue_accounted_bytes;
    uint64_t event_accounted_bytes;
    bool queue_stop;
    uint64_t task_id;
    bool task_running;
    // A raw monitor can bootstrap the socket command executor while the
    // application waits only on monitor events. The lease ends with that
    // monitor unless a longer-lived async consumer explicitly takes it over.
    std::atomic<bool> owns_async_command_processing;
    std::set<std::string> ready_connections;
    std::map<std::string, uint8_t> transport_pair_ready_lanes;
};

// A socket can have a long-lived asynchronous mailbox executor while a public
// receive is blocking on the same socket.  The executor owns command delivery;
// this runtime provides the separate hand-off used to wake the public receiver
// after an application pipe has actually been activated.
struct socket_receive_runtime_t
{
    enum mode_t
    {
        mode_plain,
        mode_pipe,
        mode_routed
    };

    socket_receive_runtime_t () :
        async_command_handoff_pending (false),
        receive_owner (receive_owner_available),
        progress_epoch (0),
        waiters (0)
#ifdef ZLINK_BUILD_TESTS
        ,
        public_mailbox_drains (0),
        async_mailbox_drains (0),
        wait_hook (NULL),
        wait_hook_userdata (NULL)
#endif
    {
    }

    mutex_t command_owner_sync;
    std::atomic<bool> async_command_handoff_pending;
    // The async mailbox executor mutates receive-side socket state under
    // `sync`.  These operations own the transition from a lock-free public
    // receive lease to that exclusive owner; callers do not manipulate the
    // gate or reader count directly.
    bool try_acquire_public_receive_lease ()
    {
        uint8_t expected = receive_owner_available;
        while (!receive_owner.compare_exchange_weak (
          expected, receive_owner_public, std::memory_order_acquire,
          std::memory_order_relaxed)) {
            if (expected == receive_owner_async)
                return false;
            expected = receive_owner_available;
        }
        return true;
    }

    void release_public_receive_lease ()
    {
        receive_owner.store (receive_owner_available,
                             std::memory_order_release);
    }

    void require_receive_sync_for_async_owner ()
    {
        uint8_t expected = receive_owner_available;
        while (!receive_owner.compare_exchange_weak (
          expected, receive_owner_async, std::memory_order_acquire,
          std::memory_order_relaxed)) {
            if (expected == receive_owner_async)
                return;
            expected = receive_owner_available;
        }
    }

    void release_receive_sync_from_async_owner ()
    {
        receive_owner.store (receive_owner_available,
                             std::memory_order_release);
    }

  private:
    enum receive_owner_t : uint8_t
    {
        receive_owner_available,
        receive_owner_public,
        receive_owner_async
    };
    std::atomic<uint8_t> receive_owner;

  public:
    mutex_t sync;
    condition_variable_t progress_cv;
    uint64_t progress_epoch;
    uint32_t waiters;
#ifdef ZLINK_BUILD_TESTS
    typedef void (*wait_hook_fn) (void *userdata_);
    std::atomic<uint64_t> public_mailbox_drains;
    std::atomic<uint64_t> async_mailbox_drains;
    wait_hook_fn wait_hook;
    void *wait_hook_userdata;
#endif
};

struct routed_send_target_key_t
{
    routed_send_target_key_t () : transport_pair_id (0), transport_pair_generation (0) {}
    routed_send_target_key_t (const void *routing_id_,
                              size_t routing_id_size_,
                              uint64_t transport_pair_id_,
                              uint64_t transport_pair_generation_) :
        peer_rid (routing_id_ && routing_id_size_
                    ? std::string (static_cast<const char *> (routing_id_), routing_id_size_)
                    : std::string ()),
        transport_pair_id (transport_pair_id_),
        transport_pair_generation (transport_pair_generation_)
    {
    }

    bool operator< (const routed_send_target_key_t &other_) const
    {
        if (peer_rid != other_.peer_rid)
            return peer_rid < other_.peer_rid;
        if (transport_pair_id != other_.transport_pair_id)
            return transport_pair_id < other_.transport_pair_id;
        return transport_pair_generation < other_.transport_pair_generation;
    }

    std::string peer_rid;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
};

//  One pending asynchronous send record. Core owns every part in `parts`
//  from the moment zlink_send_async() returned ZLINK_SUBMIT_OK.
struct send_pending_record_t
{
    send_pending_record_t () :
        op_id (0),
        userdata (NULL),
        has_target (false),
        charge_bytes (0),
        timeout_ms (0),
        claimed (false)
    {
    }

    zlink_send_op_id_t op_id;
    void *userdata;
    routed_send_target_key_t target;
    bool has_target;
    std::vector<zlink_msg_t> parts;
    uint64_t charge_bytes;
    uint32_t timeout_ms;
    //  Set while the admit loop owns this record outside the pending mutex.
    //  A cancel that arrives in that window reports INVALID_STATE instead of
    //  racing the physical submit.
    bool claimed;
    std::shared_ptr<zlink::request_timeout::task_t> deadline;
};

//  One resolved completion waiting for dispatch. Completions are never
//  coalesced: exactly one record exists per operation.
struct send_complete_record_t
{
    send_complete_record_t () :
        op_id (0), userdata (NULL), result (ZLINK_SEND_ADMITTED),
        terminal_errno (0)
    {
    }

    zlink_send_op_id_t op_id;
    void *userdata;
    routed_send_target_key_t target;
    zlink_send_complete_result_t result;
    int terminal_errno;
};

//  Per-socket asynchronous send admission state.
//
//  Ordering: records for one target form a FIFO. The admit loop only ever
//  looks at the head of each target queue, so head-of-line blocking within a
//  target is intentional - reordering would rearrange one logical stream on
//  the wire. Different targets are independent.
struct socket_send_pending_runtime_t
{
    socket_send_pending_runtime_t () :
        handler (NULL),
        handler_userdata (NULL),
        handler_installed (false),
        next_op_id (1),
        pending_msgs (0),
        pending_bytes (0),
        failing (false)
    {
    }

    mutable mutex_t sync;
    zlink_send_complete_handler_fn handler;
    void *handler_userdata;
    std::atomic<bool> handler_installed;
    zlink_send_op_id_t next_op_id;
    //  Plain sockets use the default-constructed key; routed sockets key by
    //  peer rid + transport pair identity + generation.
    std::map<routed_send_target_key_t, std::deque<send_pending_record_t *> >
      queues;
    std::map<zlink_send_op_id_t, send_pending_record_t *> by_op;
    uint64_t pending_msgs;
    uint64_t pending_bytes;
    std::deque<send_complete_record_t> completions;
    //  Set once close or context termination has failed every pending record.
    //  New submits are refused from that point on.
    bool failing;
};

struct socket_dispatch_bridge_t
{
    socket_dispatch_bridge_t () :
        socket_msg_handler (NULL),
        socket_msg_handler_subject (NULL),
        socket_msg_handler_userdata (NULL),
        send_recovery_pending_flag (false),
        send_recovery_ready_flag (false)
    {
    }

    void mark_send_recovery_pending ();
    void clear_send_recovery_pending ();
    void mark_send_recovery_ready ();
    void clear_send_recovery_ready ();
    bool send_recovery_pending () const;
    bool send_recovery_ready () const;

    std::atomic<zlink_socket_msg_handler_fn> socket_msg_handler;
    std::atomic<void *> socket_msg_handler_subject;
    std::atomic<void *> socket_msg_handler_userdata;
    std::atomic<bool> send_recovery_pending_flag;
    std::atomic<bool> send_recovery_ready_flag;
    std::recursive_mutex socket_msg_dispatch_sync;
};

class socket_lifecycle_coordinator_t
{
  public:
    socket_lifecycle_coordinator_t () :
        public_api_state (0),
        callback_api_depth (0),
        close_deferred (false),
        mailbox_refcnt (0),
        destroy_pending (false),
        reaper_poller_value (NULL),
        destroyed (false),
        async_mailbox_active (false),
        async_quiesce_pending (false),
        async_processing_done (true),
        async_processing_started (false),
        async_quiesce_completed (false)
    {
    }

    bool enter_public_api ();
    void leave_public_api ();
    // A poller registration keeps the socket object usable until the
    // registration is removed. This admission is held across the
    // registration lifetime rather than only during zlink_poller_add().
    bool acquire_poller_registration ();
    // Returns true when this release removed the last mailbox/lifetime pin.
    bool release_poller_registration ();
    bool enter_public_api_and_lock_sync ();
    bool enter_callback_api ();
    bool leave_callback_api ();
    bool begin_close_or_fail_busy (bool from_self_callback_);
    bool public_close_requested () const;
    bool public_api_sync_held () const;
    void lock_public_api_sync ();
    void unlock_public_api_sync ();
    void unlock_public_api_sync_and_leave ();

    int start_async_mailbox_processing (mailbox_t *mailbox_,
                                        io_thread_t *io_thread_,
                                        mailbox_t::mailbox_handler_t handler_,
                                        void *handler_arg_,
                                        mailbox_t::mailbox_pre_post_t pre_post_);
    void mark_async_processing_started ();
    void wait_async_started (int timeout_ms_);
    void stop_async_mailbox_processing (mailbox_t *mailbox_);
    void mark_async_processing_stopped (mailbox_t *mailbox_);
    void wait_async_quiesced (int timeout_ms_);
    bool is_async_mailbox_active () const;
    bool is_async_quiesce_pending () const;
    void complete_deferred_close_handoff (mailbox_t *mailbox_, int timeout_ms_);
    void clear_deferred_close ();
    bool take_deferred_close ();
    void mark_destroy_pending ();
    void clear_destroy_pending ();
    bool is_destroy_pending () const;
    void set_reaper_poller (poller_t *poller_);
    poller_t *reaper_poller () const;
    void mark_destroyed ();
    bool is_destroyed () const;
    int mailbox_refcount ();
    void inc_mailbox_ref ();
    bool dec_mailbox_ref ();

    std::atomic<uint32_t> public_api_state;
    std::atomic<uint32_t> callback_api_depth;
    std::atomic<bool> close_deferred;
    atomic_counter_t mailbox_refcnt;
    bool destroy_pending;
    poller_t *reaper_poller_value;
    bool destroyed;
    std::atomic<bool> async_mailbox_active;
    std::atomic<bool> async_quiesce_pending;
    std::atomic<bool> async_processing_done;
    std::atomic<bool> async_processing_started;
    std::atomic<bool> async_quiesce_completed;
    mutex_t async_done_mu;
    condition_variable_t async_done_cv;
};

class socket_callback_scope_t
{
  public:
    explicit socket_callback_scope_t (socket_base_t *socket_);
    ~socket_callback_scope_t ();

    bool acquired () const { return _entered; }

  private:
    socket_base_t *_socket;
    socket_lifecycle_coordinator_t *_coordinator;
    bool _entered;
};

class socket_public_api_scope_t
{
  public:
    explicit socket_public_api_scope_t (socket_lifecycle_coordinator_t &coordinator_) :
        _coordinator (&coordinator_), _entered (coordinator_.enter_public_api ())
    {
    }

    ~socket_public_api_scope_t ()
    {
        if (_entered)
            _coordinator->leave_public_api ();
    }

    bool acquired () const { return _entered; }

  private:
    socket_lifecycle_coordinator_t *_coordinator;
    bool _entered;
};

class socket_public_api_lock_scope_t
{
  public:
    explicit socket_public_api_lock_scope_t (socket_lifecycle_coordinator_t &coordinator_) :
        _coordinator (&coordinator_), _locked (true)
    {
        _coordinator->lock_public_api_sync ();
    }

    ~socket_public_api_lock_scope_t ()
    {
        if (_locked)
            _coordinator->unlock_public_api_sync ();
    }

  private:
    socket_lifecycle_coordinator_t *_coordinator;
    bool _locked;
};


class socket_public_send_scope_t
{
  public:
    socket_public_send_scope_t (socket_lifecycle_coordinator_t &coordinator_, bool needs_sync_);
    ~socket_public_send_scope_t ();

    bool acquired () const { return _entered; }
    bool sync_locked () const { return _sync_locked; }
    bool should_hold_sync_during_retry (bool retry_progress_owner_active_) const;
    void release_sync_for_retry ();
    void reacquire_sync_after_retry ();
    void unlock_sync ();
    void relock_sync ();

  private:
    socket_lifecycle_coordinator_t *_coordinator;
    bool _entered;
    bool _needs_sync;
    bool _sync_locked;
};

//  Marks the calling thread as running inside a Core completion callback for
//  one socket. Both send-complete and reply callbacks use this global TLS
//  scope; public submission entry points reject re-entry on any socket with
//  EDEADLK.
class socket_send_complete_dispatch_scope_t
{
  public:
    explicit socket_send_complete_dispatch_scope_t (socket_base_t *socket_);
    ~socket_send_complete_dispatch_scope_t ();

    static socket_base_t *current_socket ();
    static bool dispatching_socket (const socket_base_t *socket_);
    static bool dispatching_any ();

  private:
    socket_base_t *_previous;
};

struct socket_runtime_t
{
    socket_endpoint_runtime_t endpoint_runtime;
    socket_command_runtime_t command_runtime;
    socket_receive_runtime_t receive_runtime;
    socket_monitor_runtime_t monitor_runtime;
    socket_dispatch_bridge_t dispatch_bridge;
    socket_send_pending_runtime_t send_pending_runtime;
    socket_lifecycle_coordinator_t lifecycle_coordinator;
};
}

#endif
