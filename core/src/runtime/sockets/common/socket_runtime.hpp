/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SOCKET_RUNTIME_HPP_INCLUDED__
#define __ZLINK_SOCKET_RUNTIME_HPP_INCLUDED__

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>

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
class io_thread_t;
class mailbox_t;
class socket_base_t;

enum
{
    socket_monitor_max_values = 4,
    socket_monitor_internal_connection_ready_edge = 1
};

struct socket_monitor_event_record_t
{
    socket_monitor_event_record_t () :
        event (0),
        values_count (0),
        internal_flags (0)
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
        queue_stop (false),
        task_id (0),
        task_running (false)
    {
    }

    uint32_t ready_count () const;
    bool mark_ready_connection (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                const unsigned char *routing_id_,
                                size_t routing_id_size_,
                                uint32_t *ready_count_out_);
    bool erase_ready_connection (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                 const unsigned char *routing_id_,
                                 size_t routing_id_size_,
                                 uint32_t *ready_count_out_);
    bool erase_ready_connection_for_endpoint (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                              uint32_t *ready_count_out_);
    bool mark_transport_pair_lane_ready (
      const endpoint_uri_pair_t &endpoint_uri_pair_,
      transport_lane_t lane_,
      uint64_t pair_id_,
      uint64_t generation_);
    void erase_transport_pair_readiness_for_endpoint (
      const endpoint_uri_pair_t &endpoint_uri_pair_);
    void reset_worker_state ();
    void start_task (uint64_t task_id_);
    bool dequeue_worker_event_nowait (socket_monitor_event_record_t *out_);
    void requeue_worker_event_front (const socket_monitor_event_record_t &record_);
    void enqueue_worker_event (const socket_monitor_event_record_t &record_, size_t hwm_);
    void stop_task ();

    void *socket;
    int64_t events;
    std::atomic<int64_t> events_atomic;
    bool lossy;
    mutable mutex_t sync;
    mutex_t queue_sync;
    condition_variable_t queue_cv;
    std::deque<socket_monitor_event_record_t> queue;
    bool queue_stop;
    uint64_t task_id;
    bool task_running;
    std::set<std::string> ready_connections;
    std::map<std::string, uint8_t> transport_pair_ready_lanes;
};

struct socket_dispatch_bridge_t
{
    socket_dispatch_bridge_t () :
        socket_msg_handler (NULL),
        socket_msg_handler_subject (NULL),
        socket_msg_handler_userdata (NULL),
        send_ready_handler (NULL),
        send_ready_handler_subject (NULL),
        send_ready_handler_userdata (NULL),
        send_ready_seq (0),
        send_ready_armed (false),
        send_ready_recovery_pending (false),
        send_ready_recovery_ready (false)
    {
    }

    bool load_send_ready_handler (zlink_send_ready_handler_fn *handler_out_,
                                  void **subject_out_,
                                  void **userdata_out_) const;
    void store_send_ready_handler (zlink_send_ready_handler_fn handler_,
                                   void *subject_,
                                   void *userdata_);
    bool arm_send_ready_notification ();
    bool consume_send_ready_notification ();
    void mark_send_recovery_pending ();
    void clear_send_recovery_pending ();
    void mark_send_recovery_ready ();
    void clear_send_recovery_ready ();
    bool send_recovery_pending () const;
    bool send_recovery_ready () const;

    std::atomic<zlink_socket_msg_handler_fn> socket_msg_handler;
    std::atomic<void *> socket_msg_handler_subject;
    std::atomic<void *> socket_msg_handler_userdata;
    std::atomic<zlink_send_ready_handler_fn> send_ready_handler;
    std::atomic<void *> send_ready_handler_subject;
    std::atomic<void *> send_ready_handler_userdata;
    std::atomic<uint32_t> send_ready_seq;
    mutex_t send_ready_writer_sync;
    std::atomic<bool> send_ready_armed;
    std::atomic<bool> send_ready_recovery_pending;
    std::atomic<bool> send_ready_recovery_ready;
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
        monitor_async_mailbox_owned (false),
        async_mailbox_active (false),
        async_quiesce_pending (false),
        async_processing_done (true),
        async_processing_started (false)
    {
    }

    bool enter_public_api ();
    void leave_public_api ();
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
    void set_monitor_async_mailbox_owned (bool owned_);
    bool is_monitor_async_mailbox_owned () const;
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
    bool monitor_async_mailbox_owned;
    std::atomic<bool> async_mailbox_active;
    std::atomic<bool> async_quiesce_pending;
    std::atomic<bool> async_processing_done;
    std::atomic<bool> async_processing_started;
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
    bool should_hold_sync_during_retry (bool send_ready_handler_active_) const;
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

class socket_send_ready_dispatch_scope_t
{
  public:
    explicit socket_send_ready_dispatch_scope_t (socket_base_t *socket_);
    ~socket_send_ready_dispatch_scope_t ();

    static socket_base_t *current_socket ();
    static bool dispatching_socket (const socket_base_t *socket_);

  private:
    socket_base_t *_previous;
};

struct socket_runtime_t
{
    socket_endpoint_runtime_t endpoint_runtime;
    socket_command_runtime_t command_runtime;
    socket_monitor_runtime_t monitor_runtime;
    socket_dispatch_bridge_t dispatch_bridge;
    socket_lifecycle_coordinator_t lifecycle_coordinator;
};
}

#endif
