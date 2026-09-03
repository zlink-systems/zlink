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
#include "api/socket/socket_completion_queue_internal.hpp"
#include "zlink.h"

namespace zlink
{
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
        pair_state (pair_state_),
        completion_generation (0),
        completion_owner_connection_id (0)
    {
    }

    const std::string endpoint_uri;
    const std::string protocol;
    const std::string address;
    const options_t connect_options;
    const uint64_t pair_id;
    const std::shared_ptr<transport_pair_state_t> pair_state;
    // The Completion child is reusable across a shared reconnect, but its
    // publication belongs to one exact Application owner generation at a time.
    // A stale cancel must never retire a child already adopted by a newer one.
    uint64_t completion_generation;
    uint64_t completion_owner_connection_id;
};

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
        command_drain_active (false),
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
    // Published before the command owner clears the mailbox pending hint and
    // retained until every claimed command-side state change is visible.
    std::atomic<bool> command_drain_active;
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
        // Capacity EAGAIN is a per-pipe readiness miss. The fair queue may
        // skip that queued record and admit another source in this same
        // receive attempt, so it must not poison the whole record scope.
        if (errno != EAGAIN)
            _admission_failed = true;
        return -1;
    }

    // Deferred admission lets a routed receiver inspect the queued first
    // application frame before paying whole-record ownership. The admission
    // hook invokes acquire_before_frame() while its receive attempt is active.
    void begin_deferred_attempt (socket_receive_runtime_t *runtime_,
                                 bool *sync_held_)
    {
        zlink_assert (_owner == owner_none);
        _attempt_runtime = runtime_;
        _attempt_sync_held = sync_held_;
    }

    void end_deferred_attempt ()
    {
        _attempt_runtime = NULL;
        _attempt_sync_held = NULL;
    }

    int acquire_before_frame ()
    {
        if (!_attempt_runtime || !_attempt_sync_held) {
            errno = EFAULT;
            return -1;
        }

        const bool sync_was_held = *_attempt_sync_held;
        if (!sync_was_held) {
            _attempt_runtime->sync.lock ();
            *_attempt_sync_held = true;
        }
        if (prepare_receive_attempt () != 0) {
            if (!sync_was_held) {
                _attempt_runtime->sync.unlock ();
                *_attempt_sync_held = false;
            }
            return -1;
        }
        _runtime = _attempt_runtime;
        _owner = sync_was_held ? owner_async_sync : owner_public;
#ifdef ZLINK_BUILD_TESTS
        socket_receive_runtime_t::record_hook_fn hook =
          _runtime->record_acquired_hook.load (std::memory_order_acquire);
        if (hook)
            hook (_runtime->record_hook_userdata.load (std::memory_order_acquire));
#endif
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
    socket_receive_runtime_t *_attempt_runtime = NULL;
    bool *_attempt_sync_held = NULL;

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

// Internal REQUEST admission hooks. The resolver runs only after one pending
// record has won admission or a pre-admission terminal race.
typedef void (*send_pending_request_resolved_fn) (void *context_,
                                                  bool admitted_,
                                                  int terminal_errno_);
typedef void (*send_pending_request_cleanup_fn) (void *context_);
// Promote a submit-call-owned REQUEST observer/resolution context only when
// admission has actually fallen back to a pending record. On success the
// pending record owns both returned pointers and releases the resolution
// context through send_pending_request_cleanup_fn.
typedef int (*send_pending_request_promote_fn) (
  void *inline_context_, void **pending_observer_userdata_out_,
  void **pending_resolution_context_out_);

//  One pending nonblocking SEND/REQUEST admission record. Core owns every
//  part in `parts` after the public submit returns ZLINK_SUBMIT_OK.
struct send_pending_record_t
{
    send_pending_record_t () :
        op_id (0),
        pull_completion (false),
        request_admission (false),
        completion_reservation (NULL),
        admission_observer (NULL),
        admission_observer_userdata (NULL),
        request_resolution_context (NULL),
        request_resolved (NULL),
        request_cleanup (NULL),
        has_target (false),
        charge_bytes (0),
        claimed (false),
        deferred_terminal_errno (0)
    {
    }

    zlink_send_op_id_t op_id;
    bool pull_completion;
    bool request_admission;
    socket_completion::reservation_t *completion_reservation;
    pipe_write_observer_fn admission_observer;
    void *admission_observer_userdata;
    void *request_resolution_context;
    send_pending_request_resolved_fn request_resolved;
    send_pending_request_cleanup_fn request_cleanup;
    routed_send_target_key_t target;
    bool has_target;
    std::vector<zlink_msg_t> parts;
    uint64_t charge_bytes;
    //  Set while the admit loop owns this record outside the pending mutex so
    //  a concurrent terminal resolver cannot race the physical submit.
    bool claimed;
    //  Pipe detach can race a physical admission while `claimed` keeps this
    //  record outside the pending mutex. The detach owner publishes its
    //  terminal cause here; the admission owner consumes it before releasing
    //  the claim. Access is serialized by socket_send_pending_runtime_t::sync.
    int deferred_terminal_errno;
};

//  Per-target direct-admission reservation. STREAM packet sends retain
//  inactive entries for the lifetime of one exact transport target so the
//  steady-state echo path does not allocate and free a tree node per packet.
//  Detach marks an active retained entry for removal by its current owner.
struct send_inline_attempt_state_t
{
    send_inline_attempt_state_t (bool active_ = false,
                                 bool retained_ = false) :
        active (active_), retained (retained_), retire (false),
        retire_errno (0)
    {
    }

    bool active;
    bool retained;
    bool retire;
    int retire_errno;
};

// A synchronous NONE submit is not a pending record and therefore does not
// reserve either pending-pool capacity or a completion slot.  It still needs a
// socket-local fence against an explicit logical-target removal while it has
// dropped the public send scope to wait for reconnect/progress.  Entries exist
// only while at least one NONE call is waiting on the key.
struct send_logical_wait_state_t
{
    send_logical_wait_state_t () : epoch (0), terminal_errno (0), waiters (0)
    {
    }

    uint64_t epoch;
    int terminal_errno;
    uint32_t waiters;
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
        admission_gate (false),
        next_op_id (1),
        pending_msgs (0),
        enqueue_epoch (0),
        redrive_epoch (0),
        pending_bytes (0),
        completion_capacity_blocked (false),
        failing (false)
    {
    }

    mutable mutex_t sync;
    //  Serializes physical asynchronous admission. With no queued work the
    //  submitter acquires this atomically and avoids the pending mutex/maps.
    std::atomic<bool> admission_gate;
    zlink_send_op_id_t next_op_id;
    //  Plain sockets use the default-constructed key. Paired routed sockets
    //  key by peer rid + transport pair identity/generation; an unpaired
    //  ROUTER uses peer rid + its immutable physical route incarnation.
    std::map<routed_send_target_key_t, std::deque<send_pending_record_t *> >
      queues;
    //  Reserves per-target ordering while a submitter attempts the direct
    //  admission path outside the pending mutex. Different targets may still
    //  admit independently when one target is backpressured. STREAM retains
    //  inactive exact-target entries until detach to avoid steady-state node
    //  churn; `active` alone controls ordering exclusion.
    std::map<routed_send_target_key_t, send_inline_attempt_state_t>
      inline_attempts;
    std::map<routed_send_target_key_t, send_logical_wait_state_t>
      logical_waits;
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
    // Set only after the unified completion reservation limit rejects a
    // public submit. Unlike pipe HWM, this condition can recover either when
    // a completion is dequeued or when every older pending record admits and
    // a new submit can take the immediate-id-0 path.
    std::atomic<bool> completion_capacity_blocked;
    //  Set once close or context termination has failed every pending record.
    //  New submits are refused from that point on.
    std::atomic<bool> failing;
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
    // Pipe termination is reported while the command executor owns the
    // receive mutex. Routing cleanup must wait until that outer scope
    // is gone, so keep a lifetime-pinned intrusive queue with no allocation
    // failure in the terminal path.
    mutex_t deferred_socket_msg_termination_sync;
    pipe_t *deferred_socket_msg_termination_head;
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
        public_command_wait_owner_epoch (0)
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
    uint64_t public_command_wait_owner_epoch;
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
        async_processing_done (true),
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
    std::atomic<bool> async_processing_done;
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
    socket_endpoint_runtime_t endpoint_runtime;
    socket_command_runtime_t command_runtime;
    socket_receive_runtime_t receive_runtime;
    socket_monitor_runtime_t monitor_runtime;
    socket_dispatch_bridge_t dispatch_bridge;
    socket_submit_progress_runtime_t submit_progress_runtime;
    socket_send_pending_runtime_t send_pending_runtime;
    socket_completion::queue_state_t completion_runtime;
    socket_lifecycle_coordinator_t lifecycle_coordinator;
};
}

#endif
