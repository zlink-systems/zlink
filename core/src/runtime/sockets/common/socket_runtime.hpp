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
#include "utils/likely.hpp"
#include "utils/mutex.hpp"
#include "api/socket/socket_completion_queue_internal.hpp"
#include "zlink.h"

namespace zlink
{
class io_thread_t;
class mailbox_t;
class socket_base_t;
class socket_lifecycle_coordinator_t;

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

struct socket_endpoint_pipe_t
{
    socket_endpoint_pipe_t () : endpoint (NULL), pipe (NULL), local_type (endpoint_type_none),
                                transport_lane (transport_lane_application) {}
    socket_endpoint_pipe_t (own_t *endpoint_, pipe_t *pipe_, endpoint_type_t local_type_) :
        endpoint (endpoint_), pipe (pipe_), local_type (local_type_),
        transport_lane (transport_lane_application)
    {
    }
    socket_endpoint_pipe_t (own_t *endpoint_,
                            pipe_t *pipe_,
                            endpoint_type_t local_type_,
                            const std::shared_ptr<transport_pair_state_t> &pair_state_,
                            const std::shared_ptr<struct transport_pair_connect_intent_t> &intent_,
                            transport_lane_t lane_) :
        endpoint (endpoint_),
        pipe (pipe_),
        local_type (local_type_),
        transport_pair_state (pair_state_),
        transport_pair_connect_intent (intent_),
        transport_lane (lane_)
    {
    }

    own_t *endpoint;
    pipe_t *pipe;
    endpoint_type_t local_type;
    //  Paired transports keep the shared pair state here so that terminating
    //  one endpoint can stop the whole pair from reconnecting. Both lanes of
    //  one connect share this state and the same endpoint key.
    std::shared_ptr<transport_pair_state_t> transport_pair_state;
    std::shared_ptr<struct transport_pair_connect_intent_t>
      transport_pair_connect_intent;
    transport_lane_t transport_lane;
};
typedef std::multimap<std::string, socket_endpoint_pipe_t> socket_endpoints_t;

//  Immutable connect-time inputs shared by the Application-first endpoint and
//  its optional Completion child. Only the socket mailbox owner publishes the
//  exact generation and owner connection that materialized that child.
struct transport_pair_connect_intent_t
{
    transport_pair_connect_intent_t (
      const std::string &endpoint_uri_, const std::string &protocol_,
      const std::string &address_, const options_t &options_, uint64_t pair_id_,
      const std::shared_ptr<transport_pair_state_t> &pair_state_) :
        endpoint_uri (endpoint_uri_),
        protocol (protocol_),
        address (address_),
        connect_options (options_),
        pair_id (pair_id_),
        pair_state (pair_state_)
    {
    }

    const std::string endpoint_uri;
    const std::string protocol;
    const std::string address;
    const options_t connect_options;
    const uint64_t pair_id;
    const std::shared_ptr<transport_pair_state_t> pair_state;
};

class socket_inprocs_t
{
  public:
    void emplace (const char *endpoint_uri_, pipe_t *pipe_);
    int erase_pipes (const std::string &endpoint_uri_str_,
                     socket_base_t *owner_,
                     std::vector<pipe_t *> *terminating_pipes_,
                     std::vector<pipe_t *> *peer_progress_pipes_);
    void erase_pipe (const pipe_t *pipe_);
    bool endpoint_for_pipe (const pipe_t *pipe_, std::string *endpoint_out_) const;

    template <typename Visitor> void for_each_unique_endpoint (Visitor visitor_) const
    {
        for (map_t::const_iterator it = _inprocs.begin (), end = _inprocs.end (); it != end;) {
            map_t::const_iterator next = it;
            do {
                ++next;
            } while (next != end && next->first == it->first);
            visitor_ (it->first);
            it = next;
        }
    }

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
    void disable_transport_pair_reconnects ();

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
    // A C3 scheduling hint sampled before entering a receive turn.
    std::atomic<int> recv_ticks;
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
    bool erase_transport_pair_readiness (
      const endpoint_uri_pair_t &endpoint_uri_pair_,
      uint64_t pair_id_,
      uint64_t generation_);
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
    // Serializes public monitor replacement without extending the event-state
    // lock across context/socket creation or async mailbox ownership changes.
    recursive_mutex_t operation_sync;
    mutable recursive_mutex_t sync;
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

// Socket receive state is C2: public attempts, whole records, readiness and
// commands all use the lifecycle socket turn. Async installation changes who
// drains commands, never the exclusion mechanism for socket state.
struct socket_receive_runtime_t
{
    enum mode_t { mode_plain, mode_pipe, mode_routed };
    socket_receive_runtime_t (socket_lifecycle_coordinator_t &coordinator_) :
        coordinator (coordinator_),
        async_command_handoff_pending (false),
        command_drain_active (false),
        progress_epoch (0),
        waiters (0)
#ifdef ZLINK_BUILD_TESTS
        ,
        public_mailbox_drains (0),
        async_mailbox_drains (0),
        wait_hook (NULL),
        wait_hook_userdata (NULL),
        record_acquired_hook (NULL),
        record_contention_hook (NULL),
        record_hook_userdata (NULL),
        command_sync_probe_hook (NULL),
        command_sync_probe_userdata (NULL)
#endif
    {
    }


    socket_lifecycle_coordinator_t &coordinator;
    std::atomic<bool> async_command_handoff_pending;
    std::atomic<bool> command_drain_active;

    // Returns whether this scope acquired the turn, so an internal call
    // already inside a command/public turn does not release its caller's turn.
    bool acquire_turn ();
    void release_turn ();
    void report_turn_contention ();

    // C3 publication. The registration/publication seq_cst pair prevents a
    // missed edge; only a registered waiter makes notification take a mutex.
    void publish_receive_progress ()
    {
        progress_epoch.fetch_add (1, std::memory_order_seq_cst);
        if (waiters.load (std::memory_order_seq_cst) == 0)
            return;
        scoped_lock_t lock (progress_sync);
        progress_cv.broadcast ();
    }

    mutex_t progress_sync;
    condition_variable_t progress_cv;
    std::atomic<uint64_t> progress_epoch;
    std::atomic<uint32_t> waiters;
#ifdef ZLINK_BUILD_TESTS
    typedef void (*wait_hook_fn) (void *userdata_);
    std::atomic<uint64_t> public_mailbox_drains;
    std::atomic<uint64_t> async_mailbox_drains;
    wait_hook_fn wait_hook;
    void *wait_hook_userdata;
    typedef void (*record_hook_fn) (void *userdata_);
    std::atomic<record_hook_fn> record_acquired_hook;
    std::atomic<record_hook_fn> record_contention_hook;
    std::atomic<void *> record_hook_userdata;
    typedef void (*command_sync_probe_hook_fn) (void *userdata_,
                                                int command_type_,
                                                bool receive_sync_was_busy_,
                                                bool public_api_sync_owned_);
    std::atomic<command_sync_probe_hook_fn> command_sync_probe_hook;
    std::atomic<void *> command_sync_probe_userdata;
#endif
};

class socket_receive_entry_scope_t
{
  public:
    explicit socket_receive_entry_scope_t (socket_receive_runtime_t &runtime_) :
        _runtime (runtime_), _owns_turn (runtime_.acquire_turn ()) {}
    ~socket_receive_entry_scope_t ()
    {
        if (_owns_turn)
            _runtime.release_turn ();
    }
    bool release ()
    {
        const bool owns = _owns_turn;
        _owns_turn = false;
        return owns;
    }

  private:
    socket_receive_runtime_t &_runtime;
    bool _owns_turn;
    ZLINK_NON_COPYABLE_NOR_MOVABLE (socket_receive_entry_scope_t)
};

// A complete physical record retains the same socket turn across frame
// reads, then releases it before returning buffered public parts or waiting.
class socket_receive_record_scope_t
{
  public:
    typedef int (*admission_fn) (void *userdata_);
    typedef void (*admission_rollback_fn) (void *userdata_);

    socket_receive_record_scope_t () :
        _runtime (NULL), _owns_turn (false), _admission (NULL),
        _admission_rollback (NULL), _admission_userdata (NULL),
        _admission_failed (false), _attempt_runtime (NULL) {}
    ~socket_receive_record_scope_t () { release (); }

    void set_admission (admission_fn admission_,
                        admission_rollback_fn rollback_, void *userdata_)
    {
        zlink_assert (!_runtime);
        _admission = admission_;
        _admission_rollback = rollback_;
        _admission_userdata = userdata_;
    }
    int prepare_receive_attempt ()
    {
        if (!_admission || _admission (_admission_userdata) == 0)
            return 0;
        if (errno != EAGAIN)
            _admission_failed = true;
        return -1;
    }
    void begin_deferred_attempt (socket_receive_runtime_t *runtime_)
    {
        zlink_assert (!_runtime);
        _attempt_runtime = runtime_;
    }
    void end_deferred_attempt () { _attempt_runtime = NULL; }
    int acquire_before_frame ()
    {
        if (!_attempt_runtime) {
            errno = EFAULT;
            return -1;
        }
        if (prepare_receive_attempt () != 0)
            return -1;
        adopt (_attempt_runtime, false);
        return 0;
    }
    bool admission_failed () const { return _admission_failed; }
    void rollback_receive_attempt ()
    {
        if (_admission_rollback)
            _admission_rollback (_admission_userdata);
    }
    bool owns (const socket_receive_runtime_t *runtime_) const
    {
        return _runtime == runtime_;
    }
    void adopt (socket_receive_runtime_t *runtime_, bool owns_turn_)
    {
        zlink_assert (runtime_);
        const bool acquired = !_runtime;
        _runtime = runtime_;
        _owns_turn = owns_turn_;
#ifdef ZLINK_BUILD_TESTS
        socket_receive_runtime_t::record_hook_fn hook =
          runtime_->record_acquired_hook.load (std::memory_order_acquire);
        if (acquired && hook)
            hook (runtime_->record_hook_userdata.load (std::memory_order_acquire));
#endif
    }
    void release ()
    {
        if (_runtime && _owns_turn)
            _runtime->release_turn ();
        _runtime = NULL;
        _owns_turn = false;
    }

  private:
    socket_receive_runtime_t *_runtime;
    bool _owns_turn;
    admission_fn _admission;
    admission_rollback_fn _admission_rollback;
    void *_admission_userdata;
    bool _admission_failed;
    socket_receive_runtime_t *_attempt_runtime;
    ZLINK_NON_COPYABLE_NOR_MOVABLE (socket_receive_record_scope_t)
};

// Immutable physical route selected for one routed write attempt. This value
// survives an EAGAIN result without extending the selected pipe's lifetime
// across a blocking retry.
struct routed_send_attempt_identity_t
{
    routed_send_attempt_identity_t () :
        transport_pair_id (0),
        transport_pair_generation (0),
        transport_connection_id (0),
        route_incarnation_id (0)
    {
    }

    void reset ()
    {
        transport_pair_id = 0;
        transport_pair_generation = 0;
        transport_connection_id = 0;
        route_incarnation_id = 0;
    }

    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    // Mutable engine generation is retained for wire/message stamping, while
    // the immutable route incarnation identifies an unpaired ROUTER retry.
    uint64_t transport_connection_id;
    // Set only for an unpaired ROUTER attempt.
    uint64_t route_incarnation_id;
};

struct routed_send_target_key_t
{
    routed_send_target_key_t () :
        transport_pair_id (0),
        transport_pair_generation (0),
        route_incarnation_id (0)
    {
    }
    routed_send_target_key_t (const void *routing_id_,
                              size_t routing_id_size_,
                              uint64_t transport_pair_id_,
                              uint64_t transport_pair_generation_,
                              uint64_t route_incarnation_id_ = 0,
                              const std::string &logical_endpoint_ =
                                std::string ()) :
        peer_rid (routing_id_ && routing_id_size_
                    ? std::string (static_cast<const char *> (routing_id_), routing_id_size_)
                    : std::string ()),
        logical_endpoint (logical_endpoint_),
        transport_pair_id (transport_pair_id_),
        transport_pair_generation (transport_pair_generation_),
        route_incarnation_id (route_incarnation_id_)
    {
    }

    bool operator< (const routed_send_target_key_t &other_) const
    {
        if (logical_endpoint != other_.logical_endpoint)
            return logical_endpoint < other_.logical_endpoint;
        if (peer_rid != other_.peer_rid)
            return peer_rid < other_.peer_rid;
        if (transport_pair_id != other_.transport_pair_id)
            return transport_pair_id < other_.transport_pair_id;
        if (transport_pair_generation != other_.transport_pair_generation)
            return transport_pair_generation < other_.transport_pair_generation;
        return route_incarnation_id < other_.route_incarnation_id;
    }

    std::string peer_rid;
    // DEALER pins a public operation to the configured endpoint chosen at
    // FINAL. The peer RID is retained only as the last handshake identity and
    // completion correlation; it is not the reconnect key.
    std::string logical_endpoint;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    // Immutable identity of one physical unpaired ROUTER pipepair. It is
    // independent of the mutable network connection id, so an engine reset
    // cannot orphan pending work and a replacement pipe cannot consume it.
    uint64_t route_incarnation_id;
};

// A blocking submit drops the public send scope while waiting for progress.
// Keep target-removal state until every waiter for that key has resumed so an
// explicit disconnect cannot be mistaken for a retryable connectivity gap.
struct blocking_send_wait_state_t
{
    blocking_send_wait_state_t () : epoch (0), terminal_errno (0), waiters (0)
    {
    }

    uint64_t epoch;
    int terminal_errno;
    uint32_t waiters;
};

// Per-socket blocking submit wait state.
struct socket_blocking_send_runtime_t
{
    socket_blocking_send_runtime_t () {}

    mutable recursive_mutex_t sync;
    std::map<routed_send_target_key_t, blocking_send_wait_state_t>
      logical_waits;
};

struct socket_dispatch_bridge_t
{
    socket_dispatch_bridge_t () :
        send_recovery_pending_flag (false),
        send_recovery_ready_flag (false),
        deferred_socket_msg_termination_head (NULL),
        deferred_socket_msg_termination_tail (NULL)
    {
    }

    void mark_send_recovery_pending ();
    void clear_send_recovery_pending ();
    void mark_send_recovery_ready ();
    void clear_send_recovery_ready ();
    bool send_recovery_pending () const;
    bool send_recovery_ready () const;

    std::atomic<bool> send_recovery_pending_flag;
    std::atomic<bool> send_recovery_ready_flag;
    // Pipe termination is reported inside command dispatch. A lifetime-pinned
    // intrusive queue separates that dispatch from route retirement without
    // allocation failure; retirement reacquires the same socket turn.
    // The head doubles as the queue's emptiness answer. It is atomic so the
    // command drain can ask that question — the only question it asks on the
    // hot path, where the queue is empty — without taking the mutex; every
    // mutation of the queue itself still runs under the mutex.
    recursive_mutex_t deferred_socket_msg_termination_sync;
    std::atomic<pipe_t *> deferred_socket_msg_termination_head;
    pipe_t *deferred_socket_msg_termination_tail;
};

// Reply submitters that encounter physical backpressure wait for socket state
// to be applied, rather than merely for its command to be enqueued. The atomic
// waiter count keeps the ordinary command path out of this mutex and CV.
struct socket_submit_progress_runtime_t
{
    socket_submit_progress_runtime_t () :
        epoch (0),
        waiters (0),
        public_command_wait_owner_active (false),
        public_command_wait_owner_retirement_epoch (0)
    {
    }

    mutex_t sync;
    condition_variable_t cv;
    std::atomic<uint64_t> epoch;
    std::atomic<uint32_t> waiters;
    // Protected by sync. A PAIR with no asynchronous executor elects one
    // blocked public sender to drain mailbox commands; concurrent senders stay
    // on the epoch/CV channel until that owner publishes progress or retires.
    bool public_command_wait_owner_active;
    uint64_t public_command_wait_owner_retirement_epoch;
};

class socket_lifecycle_coordinator_t
{
  public:
    socket_lifecycle_coordinator_t () :
        public_api_state (0),
        mailbox_ref_state (0),
        destroy_pending (false),
        reaper_poller_value (NULL),
        destroyed (false),
        async_mailbox_active (false),
        async_quiesce_pending (false),
        async_processing_started (false),
        async_quiesce_completed (false),
        public_multipart_control_boundary (false),
        deferred_peer_controls_pending (false),
        _previous_thread_public_api_sync_owner (NULL)
    {
    }

    bool enter_public_api ();
    void leave_public_api ();
    bool enter_public_send (bool needs_sync_,
                            bool multipart_sequence_,
                            bool *sync_locked_out_,
                            bool *multipart_active_out_);
    void leave_public_send (bool sync_locked_, bool multipart_sequence_);
    void suspend_public_multipart_send (bool sync_locked_);
    bool resume_public_multipart_send (bool needs_sync_,
                                       bool *sync_locked_out_);
    void release_public_multipart_marker (bool sync_locked_);
    // A poller registration keeps the socket object usable until the
    // registration is removed. This admission is held across the
    // registration lifetime rather than only during zlink_poller_add().
    bool acquire_poller_registration ();
    // Returns true when this release removed the last mailbox/lifetime pin.
    bool release_poller_registration ();
    bool enter_public_api_and_lock_sync ();
    bool begin_close_or_fail_busy ();
    bool public_close_requested () const;
    bool public_multipart_send_active () const;
    void hold_public_multipart_control_boundary ();
    void release_public_multipart_control_boundary ();
    void mark_deferred_peer_controls ();
    bool deferred_peer_controls_pending_cached () const
    {
        return deferred_peer_controls_pending.load (std::memory_order_acquire);
    }
    bool take_deferred_peer_controls ();
    bool public_api_sync_held () const;
    bool public_api_sync_owned_by_current_thread () const;
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
    void complete_deferred_close_handoff (mailbox_t *mailbox_,
                                          socket_base_t *socket_,
                                          int timeout_ms_);
    void mark_destroy_pending ();
    void clear_destroy_pending ();
    bool is_destroy_pending () const;
    void set_reaper_poller (poller_t *poller_);
    poller_t *reaper_poller () const;
    void mark_destroyed ();
    bool is_destroyed () const;
    int mailbox_refcount ();
    bool try_inc_mailbox_ref ();
    void inc_mailbox_ref ();
    bool dec_mailbox_ref ();
    bool seal_mailbox_refs_if_zero ();
    bool mailbox_refs_sealed () const;

    std::atomic<uint64_t> public_api_state;
    std::atomic<uint32_t> mailbox_ref_state;
    std::atomic<bool> destroy_pending;
    poller_t *reaper_poller_value;
    bool destroyed;
    std::atomic<bool> async_mailbox_active;
    std::atomic<bool> async_quiesce_pending;
    std::atomic<bool> async_processing_started;
    std::atomic<bool> async_quiesce_completed;
    // Completion-aware part APIs stage a multipart locally.  Keep the
    // control-ordering boundary alive while the multipart marker is handed
    // off to the complete-record submit.
    std::atomic<bool> public_multipart_control_boundary;
    std::atomic<bool> deferred_peer_controls_pending;
    mutex_t async_done_mu;
    condition_variable_t async_done_cv;

  private:
    void mark_public_api_sync_owned ();
    void unmark_public_api_sync_owned ();

    static thread_local socket_lifecycle_coordinator_t
      *_current_thread_public_api_sync_owner;
    socket_lifecycle_coordinator_t *_previous_thread_public_api_sync_owner;
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
    explicit socket_public_api_lock_scope_t (
      socket_lifecycle_coordinator_t &coordinator_, bool lock_ = true) :
        _coordinator (&coordinator_), _locked (lock_)
    {
        if (_locked)
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

enum socket_send_admission_mode_t
{
    socket_send_admission_none = 0,
    socket_send_admission_complete,
    socket_send_admission_multipart
};

class socket_public_send_scope_t
{
  public:
    socket_public_send_scope_t (socket_lifecycle_coordinator_t &coordinator_,
                                bool needs_sync_,
                                socket_send_admission_mode_t admission_mode_ =
                                  socket_send_admission_none);
    socket_public_send_scope_t (const socket_public_send_scope_t &) = delete;
    socket_public_send_scope_t &operator= (const socket_public_send_scope_t &) = delete;
    socket_public_send_scope_t (socket_public_send_scope_t &&other_) noexcept;
    socket_public_send_scope_t &operator= (socket_public_send_scope_t &&) = delete;
    ~socket_public_send_scope_t ();

    bool acquired () const { return _entered; }
    bool multipart_active () const { return _multipart_active; }
    bool sync_locked () const { return _sync_locked; }
    bool multipart_marker_owned () const { return _multipart_marker_owned; }
    bool close_cleanup_ready () const
    {
        return _multipart_marker_owned && !_entered
               && (!_needs_sync || _sync_locked);
    }
    bool should_hold_sync_during_retry (bool retry_progress_owner_active_) const;
    void release_sync_for_retry ();
    void reacquire_sync_after_retry ();
    void unlock_sync ();
    void relock_sync ();
    void suspend_multipart_call ();
    bool resume_multipart_call ();
    bool lock_multipart_for_close_cleanup ();

  private:
    socket_lifecycle_coordinator_t *_coordinator;
    bool _entered;
    bool _needs_sync;
    bool _sync_locked;
    socket_send_admission_mode_t _admission_mode;
    bool _multipart_active;
    bool _multipart_marker_owned;
};

struct socket_runtime_t
{
    socket_runtime_t () : receive_runtime (lifecycle_coordinator) {}
    socket_lifecycle_coordinator_t lifecycle_coordinator;
    socket_endpoint_runtime_t endpoint_runtime;
    socket_command_runtime_t command_runtime;
    socket_receive_runtime_t receive_runtime;
    socket_monitor_runtime_t monitor_runtime;
    socket_dispatch_bridge_t dispatch_bridge;
    socket_submit_progress_runtime_t submit_progress_runtime;
    socket_blocking_send_runtime_t blocking_send_runtime;
    socket_completion::queue_state_t completion_runtime;
};
}

#endif
