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
    // Serializes public monitor replacement without extending the event-state
    // lock across context/socket creation or async mailbox ownership changes.
    mutex_t operation_sync;
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
        wait_hook_userdata (NULL),
        record_acquired_hook (NULL),
        record_contention_hook (NULL),
        record_hook_userdata (NULL),
        command_sync_probe_hook (NULL),
        command_sync_probe_userdata (NULL)
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
#ifdef ZLINK_BUILD_TESTS
        bool contention_reported = false;
#endif
        while (!receive_owner.compare_exchange_weak (
          expected, receive_owner_public, std::memory_order_acquire,
          std::memory_order_relaxed)) {
            if (expected == receive_owner_async)
                return false;
#ifdef ZLINK_BUILD_TESTS
            if (expected == receive_owner_public && !contention_reported
                && record_contention_hook.load (std::memory_order_acquire)) {
                socket_receive_runtime_t::record_hook_fn hook =
                  record_contention_hook.load (std::memory_order_relaxed);
                if (hook) {
                    contention_reported = true;
                    hook (record_hook_userdata.load (
                      std::memory_order_acquire));
                }
            }
#endif
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

// Owns one receive-side socket transaction after its first frame has been
// consumed. A public transaction retains both its public receive lease and
// receive_runtime_t::sync across every continuation; when an asynchronous
// mailbox owns socket commands, the transaction already enters through that
// same sync. This prevents either another reader or a mailbox command from
// changing receive-side socket state between API-level frame reads.
class socket_receive_record_scope_t
{
  public:
    typedef int (*admission_fn) (void *userdata_);
    typedef void (*admission_rollback_fn) (void *userdata_);

    socket_receive_record_scope_t () :
        _runtime (NULL),
        _owner (owner_none),
        _admission (NULL),
        _admission_rollback (NULL),
        _admission_userdata (NULL),
        _admission_failed (false)
    {
    }

    ~socket_receive_record_scope_t () { release (); }

    void set_admission (admission_fn admission_,
                        admission_rollback_fn rollback_, void *userdata_)
    {
        zlink_assert (_owner == owner_none);
        _admission = admission_;
        _admission_rollback = rollback_;
        _admission_userdata = userdata_;
    }

    int prepare_receive_attempt ()
    {
        if (!_admission)
            return 0;
        if (_admission (_admission_userdata) == 0)
            return 0;
        _admission_failed = true;
        return -1;
    }

    bool admission_failed () const { return _admission_failed; }

    void rollback_receive_attempt ()
    {
        if (_admission_rollback)
            _admission_rollback (_admission_userdata);
    }

    bool owns (const socket_receive_runtime_t *runtime_) const
    {
        return _runtime == runtime_ && _owner != owner_none;
    }

    void adopt_public_owner (socket_receive_runtime_t *runtime_)
    {
        zlink_assert (runtime_ != NULL);
        zlink_assert (_owner == owner_none);
        _runtime = runtime_;
        _owner = owner_public;
#ifdef ZLINK_BUILD_TESTS
        socket_receive_runtime_t::record_hook_fn hook =
          _runtime->record_acquired_hook.load (std::memory_order_acquire);
        if (hook)
            hook (_runtime->record_hook_userdata.load (
              std::memory_order_acquire));
#endif
    }

    void adopt_async_sync (socket_receive_runtime_t *runtime_)
    {
        zlink_assert (runtime_ != NULL);
        zlink_assert (_owner == owner_none);
        _runtime = runtime_;
        _owner = owner_async_sync;
#ifdef ZLINK_BUILD_TESTS
        socket_receive_runtime_t::record_hook_fn hook =
          _runtime->record_acquired_hook.load (std::memory_order_acquire);
        if (hook)
            hook (_runtime->record_hook_userdata.load (
              std::memory_order_acquire));
#endif
    }

    void release ()
    {
        if (!_runtime)
            return;
        if (_owner == owner_public) {
            // Keep the public lease closed until command-side mutation is no
            // longer excluded by sync. A new public reader can then acquire
            // the lease without overlapping the record that just completed.
            _runtime->sync.unlock ();
            _runtime->release_public_receive_lease ();
        } else if (_owner == owner_async_sync)
            _runtime->sync.unlock ();
        _runtime = NULL;
        _owner = owner_none;
    }

  private:
    enum owner_t
    {
        owner_none,
        owner_public,
        owner_async_sync
    };

    socket_receive_runtime_t *_runtime;
    owner_t _owner;
    admission_fn _admission;
    admission_rollback_fn _admission_rollback;
    void *_admission_userdata;
    bool _admission_failed;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (socket_receive_record_scope_t)
};

// Immutable physical route selected for one routed write attempt. This value
// survives an EAGAIN result; unlike pipe_t*, it does not borrow the selected
// pipe's lifetime while an async submit prepares its pending key.
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
    // Mutable engine generation observed by this one physical attempt. It is
    // retained for wire/message stamping decisions, not as the identity of an
    // unpaired ROUTER pending queue across reconnect.
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
                              uint64_t route_incarnation_id_ = 0) :
        peer_rid (routing_id_ && routing_id_size_
                    ? std::string (static_cast<const char *> (routing_id_), routing_id_size_)
                    : std::string ()),
        transport_pair_id (transport_pair_id_),
        transport_pair_generation (transport_pair_generation_),
        route_incarnation_id (route_incarnation_id_)
    {
    }

    bool operator< (const routed_send_target_key_t &other_) const
    {
        if (peer_rid != other_.peer_rid)
            return peer_rid < other_.peer_rid;
        if (transport_pair_id != other_.transport_pair_id)
            return transport_pair_id < other_.transport_pair_id;
        if (transport_pair_generation != other_.transport_pair_generation)
            return transport_pair_generation < other_.transport_pair_generation;
        return route_incarnation_id < other_.route_incarnation_id;
    }

    std::string peer_rid;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    // Immutable identity of one physical unpaired ROUTER pipepair. It is
    // independent of the mutable network connection id, so an engine reset
    // cannot orphan pending work and a replacement pipe cannot consume it.
    uint64_t route_incarnation_id;
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
        claimed (false),
        deferred_terminal_errno (0),
        completion_result (ZLINK_SEND_ADMITTED),
        completion_errno (0),
        completion_next (NULL)
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
    //  Pipe detach can race a physical admission while `claimed` keeps this
    //  record outside the pending mutex. The detach owner publishes its
    //  terminal cause here; the admission owner consumes it before releasing
    //  the claim. Access is serialized by socket_send_pending_runtime_t::sync.
    int deferred_terminal_errno;
    std::shared_ptr<zlink::request_timeout::task_t> deadline;
    //  Resolution reuses the already allocated pending record as an
    //  intrusive completion node. Completion publication after submit
    //  acceptance therefore performs no allocation.
    zlink_send_complete_result_t completion_result;
    int completion_errno;
    send_pending_record_t *completion_next;
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
        admission_gate (false),
        public_async_depth (0),
        next_op_id (1),
        pending_msgs (0),
        enqueue_epoch (0),
        redrive_epoch (0),
        pending_bytes (0),
        completion_head (NULL),
        completion_tail (NULL),
        failing (false)
#ifdef ZLINK_BUILD_TESTS
        , gate_release_hook (NULL), gate_release_hook_userdata (NULL),
        gate_release_hook_generation (0), gate_release_hook_active (0),
        inline_fallback_hook (NULL), inline_fallback_hook_userdata (NULL),
        target_failure_progress_hook (NULL),
        target_failure_progress_hook_userdata (NULL),
        deadline_enqueue_hook (NULL), deadline_enqueue_hook_userdata (NULL),
        fail_after_queue_push (false)
#endif
    {
    }

    mutable mutex_t sync;
    zlink_send_complete_handler_fn handler;
    void *handler_userdata;
    std::atomic<bool> handler_installed;
    //  Serializes physical asynchronous admission. With no queued work the
    //  submitter acquires this atomically and avoids the pending mutex/maps.
    std::atomic<bool> admission_gate;
    //  Completion dispatch is deferred until the public send_async/cancel
    //  wrapper releases its handle pin. This makes callback self-close valid
    //  even for a completion resolved before the submitting call returns.
    std::atomic<uint32_t> public_async_depth;
    zlink_send_op_id_t next_op_id;
    //  Plain sockets use the default-constructed key. Paired routed sockets
    //  key by peer rid + transport pair identity/generation; an unpaired
    //  ROUTER uses peer rid + its immutable physical route incarnation.
    std::map<routed_send_target_key_t, std::deque<send_pending_record_t *> >
      queues;
    //  Reserves per-target ordering while a submitter attempts the direct
    //  admission path outside the pending mutex. Different targets may still
    //  admit independently when one target is backpressured.
    std::set<routed_send_target_key_t> inline_attempts;
    std::map<zlink_send_op_id_t, send_pending_record_t *> by_op;
    std::atomic<uint64_t> pending_msgs;
    //  Incremented after every queue insertion.  The admission driver uses
    //  this to close the empty-scan/gate-release handoff window without
    //  confusing an already blocked queue with newly published work.
    std::atomic<uint64_t> enqueue_epoch;
    //  Incremented before publishing a physical-admission wake. Unlike a new
    //  queue insertion, a writable or multipart-release wake invalidates the
    //  driver's local blocked-target set. Keeping the generation persistent
    //  closes the window where another thread consumes the mailbox command
    //  while the current driver still owns admission_gate.
    std::atomic<uint64_t> redrive_epoch;
    uint64_t pending_bytes;
    send_pending_record_t *completion_head;
    send_pending_record_t *completion_tail;
    //  Set once close or context termination has failed every pending record.
    //  New submits are refused from that point on.
    bool failing;
#ifdef ZLINK_BUILD_TESTS
    typedef void (*gate_release_hook_fn) (void *userdata_);
    // Test-only gate handoff probes run on the mailbox owner while tests arm
    // and disarm them from an application thread. Atomics plus the active
    // counter make disarm a lifetime barrier for stack-owned probe data. The
    // generation additionally prevents a reader that observed an old hook
    // from treating a same-function-pointer rearm as that old installation.
    std::atomic<gate_release_hook_fn> gate_release_hook;
    std::atomic<void *> gate_release_hook_userdata;
    std::atomic<uint64_t> gate_release_hook_generation;
    std::atomic<uint32_t> gate_release_hook_active;
    typedef void (*inline_fallback_hook_fn) (void *userdata_);
    inline_fallback_hook_fn inline_fallback_hook;
    void *inline_fallback_hook_userdata;
    typedef void (*target_failure_progress_hook_fn) (void *userdata_);
    target_failure_progress_hook_fn target_failure_progress_hook;
    void *target_failure_progress_hook_userdata;
    typedef void (*deadline_enqueue_hook_fn) (void *userdata_);
    deadline_enqueue_hook_fn deadline_enqueue_hook;
    void *deadline_enqueue_hook_userdata;
    bool fail_after_queue_push;
#endif
};

struct socket_dispatch_bridge_t
{
    socket_dispatch_bridge_t () :
        socket_msg_handler (NULL),
        socket_msg_handler_subject (NULL),
        socket_msg_handler_userdata (NULL),
        send_recovery_pending_flag (false),
        send_recovery_ready_flag (false),
        deferred_socket_msg_dispatch_pending (false),
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

    std::atomic<zlink_socket_msg_handler_fn> socket_msg_handler;
    std::atomic<void *> socket_msg_handler_subject;
    std::atomic<void *> socket_msg_handler_userdata;
    std::atomic<bool> send_recovery_pending_flag;
    std::atomic<bool> send_recovery_ready_flag;
    // Read activation is delivered while the command executor owns the
    // receive mutex. Application callbacks must run only after that owner has
    // released it, otherwise callback re-entry establishes the inverse
    // dispatch -> receive lock order.
    std::atomic<bool> deferred_socket_msg_dispatch_pending;
    std::recursive_mutex socket_msg_dispatch_sync;
    // Pipe termination is reported while the command executor owns the
    // receive mutex. Assembly/routing cleanup must wait until that outer scope
    // is gone, so keep a lifetime-pinned intrusive queue with no allocation
    // failure in the terminal path.
    mutex_t deferred_socket_msg_termination_sync;
    pipe_t *deferred_socket_msg_termination_head;
    pipe_t *deferred_socket_msg_termination_tail;
};

class socket_lifecycle_coordinator_t
{
  public:
    socket_lifecycle_coordinator_t () :
        public_api_state (0),
        callback_api_depth (0),
        close_deferred (false),
        mailbox_ref_state (0),
        destroy_pending (false),
        reaper_poller_value (NULL),
        destroyed (false),
        async_mailbox_active (false),
        async_quiesce_pending (false),
        async_processing_done (true),
        async_processing_started (false),
        async_quiesce_completed (false),
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
    bool enter_callback_api ();
    bool leave_callback_api ();
    bool begin_close_or_fail_busy (bool from_self_callback_);
    bool public_close_requested () const;
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
    bool try_inc_mailbox_ref ();
    void inc_mailbox_ref ();
    bool dec_mailbox_ref ();
    bool seal_mailbox_refs_if_zero ();
    bool mailbox_refs_sealed () const;

    std::atomic<uint64_t> public_api_state;
    std::atomic<uint32_t> callback_api_depth;
    std::atomic<bool> close_deferred;
    std::atomic<uint32_t> mailbox_ref_state;
    std::atomic<bool> destroy_pending;
    poller_t *reaper_poller_value;
    bool destroyed;
    std::atomic<bool> async_mailbox_active;
    std::atomic<bool> async_quiesce_pending;
    std::atomic<bool> async_processing_done;
    std::atomic<bool> async_processing_started;
    std::atomic<bool> async_quiesce_completed;
    mutex_t async_done_mu;
    condition_variable_t async_done_cv;

  private:
    void mark_public_api_sync_owned ();
    void unmark_public_api_sync_owned ();

    static thread_local socket_lifecycle_coordinator_t
      *_current_thread_public_api_sync_owner;
    socket_lifecycle_coordinator_t *_previous_thread_public_api_sync_owner;
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

//  Marks the calling thread as running inside a Core completion callback for
//  one socket. Both send-complete and reply callbacks use this global TLS
//  scope; public submission entry points reject re-entry on any socket with
//  EDEADLK.
class socket_send_complete_dispatch_scope_t
{
  public:
    explicit socket_send_complete_dispatch_scope_t (socket_base_t *socket_);
    ~socket_send_complete_dispatch_scope_t ();

    static socket_base_t *current_socket () { return _dispatch_socket; }
    static bool dispatching_socket (const socket_base_t *socket_)
    {
        return _dispatch_socket == socket_;
    }
    static bool dispatching_any () { return _dispatch_socket != NULL; }

  private:
    inline static thread_local socket_base_t *_dispatch_socket = NULL;
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
