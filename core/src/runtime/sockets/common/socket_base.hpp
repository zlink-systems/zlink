/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SOCKET_BASE_HPP_INCLUDED__
#define __ZLINK_SOCKET_BASE_HPP_INCLUDED__

#include <string>
#include <map>
#include <stdarg.h>
#include <atomic>
#include <deque>
#include <set>
#include <mutex>
#include <memory>
#include <optional>
#include <vector>

#include "core/own.hpp"
#include "utils/array.hpp"
#include "utils/blob.hpp"
#include "utils/stdint.hpp"
#include "core/poller.hpp"
#include "core/i_poll_events.hpp"
#include "core/i_mailbox.hpp"
#include "core/thread.hpp"
#include "core/auto_hwm_policy.hpp"
#include "core/ctx_physical_queue_registry.hpp"
#include "utils/clock.hpp"
#include "core/pipe.hpp"
#include "core/endpoint.hpp"
#include "utils/atomic_counter.hpp"
#include "utils/condition_variable.hpp"
#include "sockets/common/socket_runtime.hpp"
#include "zlink.h"

extern "C" {
void zlink_free_event (void *data_, void *hint_);
}

namespace zlink
{
class ctx_t;
class msg_t;
class pipe_t;
class io_thread_t;
class socket_base_t;
class socket_public_handle_t;
class session_base_t;

#ifdef ZLINK_BUILD_TESTS
typedef bool (*transport_pair_owner_after_claim_test_hook_fn) (
  uint64_t connection_id_, uint64_t pair_id_, uint64_t generation_,
  void *userdata_);
void test_set_transport_pair_owner_after_claim_hook (
  transport_pair_owner_after_claim_test_hook_fn hook_, void *userdata_);

enum async_owner_transition_test_point_t
{
    async_owner_test_monitor_acquire_before_gate = 1,
    async_owner_test_idle_stop_gate_held = 2,
    async_owner_test_explicit_stop_before_detach = 3,
    async_owner_test_transport_acquire_waiting_for_quiesce = 4
};
typedef void (*async_owner_transition_test_hook_fn) (
  async_owner_transition_test_point_t point_, void *userdata_);
void test_set_async_owner_transition_hook (
  async_owner_transition_test_hook_fn hook_, void *userdata_);
#endif

namespace socket_reqrep_internal
{
struct socket_request_reply_state_t;
}

namespace part_helper_internal
{
struct handle_state_t;
}
}

namespace zlink
{
class address_t;
struct multipart_send_facade_t;

//  Socket types that own a pull SEND-completion channel. This set widens the
//  ZLINK_POLLCOMPLETION registration check beyond reply completions.
bool socket_type_supports_send_completion (int type_);

struct socket_request_reply_bridge_t
{
    socket_request_reply_bridge_t () :
        request_reply_state_present (false),
        part_helper_state_present (false),
        part_helper_send_active_flag (false),
        part_helper_recv_ready_flag (false)
    {
    }

    std::shared_ptr<socket_reqrep_internal::socket_request_reply_state_t> request_reply_state;
    std::shared_ptr<part_helper_internal::handle_state_t> part_helper_state;
    std::atomic<bool> request_reply_state_present;
    std::atomic<bool> part_helper_state_present;
    std::atomic<bool> part_helper_send_active_flag;
    std::atomic<bool> part_helper_recv_ready_flag;
};

struct transport_pair_pipes_t
{
    transport_pair_pipes_t () :
        application (NULL),
        completion (NULL),
        generation (0),
        expected_lane_count (0),
        application_attached (false),
        application_validated (false),
        completion_validated (false),
        ready (false),
        draining (false),
        local_peer_weight_advertised (100),
        remote_flow_seen (false),
        remote_flow_paused (false),
        remote_flow_pause_accounted (false),
        remote_flow_epoch (0)
    {
    }

    pipe_t *application;
    pipe_t *completion;
    uint64_t generation;
    //  The READY handshake selects one immutable topology for this physical
    //  pair. A value of 1 admits Application alone; 2 retains the historical
    //  Application+Completion pair. Zero means no validated lane has been
    //  admitted yet and is never a ready topology.
    unsigned char expected_lane_count;
    bool application_attached;
    bool application_validated;
    bool completion_validated;
    bool ready;
    //  Set while a thread consumes this pair's Completion pipe. The pipe has a
    //  single-consumer queue, so two owners must not read it at once.
    bool draining;
    //  Local peer-weight is absolute. Each new physical pair starts from the
    //  scheduler default and publishes a non-default value exactly once after
    //  readiness; later changes update this record only after delivery.
    uint32_t local_peer_weight_advertised;
    //  Last remote receive-flow state accepted for this pair. The state is
    //  absolute, so a repeated value is idempotent and a frame whose epoch does
    //  not advance is ignored. The pair key already carries the connection
    //  generation, so a late frame from a previous physical connection cannot
    //  reach this record.
    bool remote_flow_seen;
    bool remote_flow_paused;
    //  Whether a pause is currently counted for this pair in the flow gauge
    //  and has an open duration measurement. This is the applied and accounted
    //  state, which is not the same thing as the received state above: a frame
    //  is recorded when it is accepted but only becomes accounted when the pipe
    //  actually flips. Termination must release from this marker, or a pair
    //  that terminates in the gap either takes a count it never added - the
    //  gauge is socket-wide, so it would steal another pair's - or leaves its
    //  own count behind for good.
    bool remote_flow_pause_accounted;
    uint64_t remote_flow_epoch;
    //  A flow frame may arrive on a completion lane before the socket thread
    //  has registered and validated that lane. Keep the latest frame per
    //  candidate connection until admission identifies the winning pipe.
    //  Staging is not acceptance: it changes no epoch, blocker, metric, or
    //  event while the transport pair is unregistered. Admission may later
    //  accept only the exact physical source that passed pair validation.
    //  The source is a transport connection id rather than a pipe address so
    //  allocator reuse cannot turn stale state into the winner's state.
    struct pending_flow_slot_t
    {
        pending_flow_slot_t () :
            valid (false), paused (false), epoch (0), source_connection_id (0)
        {
        }

        bool valid;
        bool paused;
        uint64_t epoch;
        uint64_t source_connection_id;
    };
    static const size_t pending_flow_slot_count = 2;
    pending_flow_slot_t pending_flow[pending_flow_slot_count];

    unsigned int expected_lane_mask () const
    {
        return expected_lane_count == 1u ? 1u
               : expected_lane_count == 2u ? 3u
                                            : 0u;
    }

    unsigned int validated_lane_mask () const
    {
        return (application && application_validated ? 1u : 0u)
               | (completion && completion_validated ? 2u : 0u);
    }

    bool accepts_lane_count (unsigned char lane_count_,
                             transport_lane_t lane_) const
    {
        if ((lane_count_ != 1u && lane_count_ != 2u)
            || (lane_ != transport_lane_application
                && lane_ != transport_lane_completion)
            || (lane_ == transport_lane_completion && lane_count_ != 2u))
            return false;
        return expected_lane_count == 0u
               || expected_lane_count == lane_count_;
    }

    pipe_t *completion_source () const
    {
        return expected_lane_count == 1u ? application : completion;
    }
};

struct accepted_transport_pair_identity_t
{
    accepted_transport_pair_identity_t () : pair_id (0), generation (0) {}
    accepted_transport_pair_identity_t (uint64_t pair_id_,
                                        uint64_t generation_) :
        pair_id (pair_id_), generation (generation_)
    {
    }

    uint64_t pair_id;
    uint64_t generation;
};

//  Marks the calling thread as the owner of one socket's completion
//  processing for the lifetime of the scope.
class completion_drain_scope_t
{
  public:
    explicit completion_drain_scope_t (const socket_base_t *socket_);
    ~completion_drain_scope_t ();

  private:
    completion_drain_scope_t (const completion_drain_scope_t &);
    completion_drain_scope_t &operator= (const completion_drain_scope_t &);

    const socket_base_t *_previous;
};

class socket_base_t : public own_t,
                      public array_item_t<>,
                      public i_poll_events,
                      public i_pipe_events
{
    friend class reaper_t;
    friend class socket_public_handle_t;
#ifdef ZLINK_BUILD_TESTS
    friend class session_termination_test_access_t;
#endif

  public:
    //  Returns false if object is not a socket.
    bool check_tag () const;
    int socket_type () const;
    void set_public_handle (socket_public_handle_t *handle_)
    {
        _public_handle = handle_;
    }
    void *public_handle () const
    {
        return static_cast<void *> (_public_handle);
    }

    //  Create a socket of a specified type.
    static socket_base_t *create (int type_, zlink::ctx_t *parent_, uint32_t tid_, int sid_);

    //  Returns the mailbox associated with this socket.
    i_mailbox *get_mailbox () const;

    //  Interrupt blocking call if the socket is stuck in one.
    //  This function can be called from a different thread!
    void stop ();

    //  Interface for communication with the API layer.
    int setsockopt (int option_, const void *optval_, size_t optvallen_);
    int getsockopt (int option_, void *optval_, size_t *optvallen_);
    int get_events (int events_, uint32_t *out_);
    int get_events_internal (int events_, uint32_t *out_);
    void set_all_pipes_nodelay ();
    int bind (const char *endpoint_uri_);
    int connect (const char *endpoint_uri_);
    int term_endpoint (const char *endpoint_uri_);
    int term_peer_rid (const zlink_routing_id_t *peer_rid_);
    int term_transport_pair (uint64_t transport_pair_id_,
                             uint64_t transport_pair_generation_);
    int send (zlink::msg_t *msg_, int flags_);
    int send_complete_record (zlink::msg_t *msg_, int flags_);
    // Internal helper for logical multipart wrappers that already hold the
    // public send scope for the whole transaction.
    int send_scoped (zlink::msg_t *msg_,
                     int flags_,
                     socket_public_send_scope_t &scope_,
                     zlink::pipe_t **pipe_out_ = NULL,
                     bool report_multipart_abort_ = false,
                     pipe_write_observer_fn observer_ = NULL,
                     void *observer_userdata_ = NULL);
    int send_routed (const zlink_routing_id_t *target_rid_, zlink::msg_t *msg_, int flags_);
    int send_routed_complete_record (const zlink_routing_id_t *target_rid_,
                                     zlink::msg_t *msg_,
                                     int flags_);
    // Submit to the exact transport pair selected by
    // select_routed_submit_target(). A replacement peer with the same RID
    // must not receive an operation parked for the previous generation.
    int send_routed_transport_pair (const zlink_routing_id_t *target_rid_,
                                    uint64_t transport_pair_id_,
                                    uint64_t transport_pair_generation_,
                                    zlink::msg_t *msg_,
                                    int flags_);
    int send_routed_scoped (const zlink_routing_id_t *target_rid_,
                            zlink::msg_t *msg_,
                            int flags_,
                            socket_public_send_scope_t &scope_,
                            uint64_t *connection_id_out_ = NULL,
                            uint64_t expected_connection_id_ = 0,
                            zlink::pipe_t **pipe_out_ = NULL,
                            uint64_t expected_transport_pair_id_ = 0,
                            uint64_t expected_transport_pair_generation_ = 0,
                            bool report_multipart_abort_ = false,
                            pipe_write_observer_fn observer_ = NULL,
                            void *observer_userdata_ = NULL,
                            routed_send_attempt_identity_t
                              *attempt_identity_out_ = NULL,
                            uint64_t expected_route_incarnation_id_ = 0);
    int select_routed_submit_target (
      const zlink_routing_id_t *router_rid_or_null_,
      zlink_routed_submit_target_t *target_out_);
    bool transport_pair_application_ready (const zlink::pipe_t *pipe_) const;

    //  Socket-wide local receive-flow state (Core internal; the public API and
    //  its result mapping are a later step). RUNNING is 0 and PAUSED is 1.
    //  Returns 0 on success and -1 with errno set:
    //    EINVAL   state outside the enum
    //    ENOTSUP  socket type without paired receive-flow control
    //    ETERM    context already terminated
    //  The completion boundary is the moment this socket stores the state.
    //  Fanout to the current pairs and synchronisation of a later pair both
    //  read that same stored state under one mutex, so they never disagree.
    int set_local_receive_flow_state (int state_);
    int get_local_receive_flow_state () const;
    //  Test/observability accessor: last remote state accepted for one pair.
    bool remote_receive_flow_paused (uint64_t transport_pair_id_,
                                     uint64_t transport_pair_generation_) const;
    //  Whether the pair's application pipe has already applied the remote
    //  state on its own thread. The socket record above changes when the frame
    //  is accepted; this one changes when the send blocker actually moves.
    bool application_pipe_remote_flow_paused (
      uint64_t transport_pair_id_, uint64_t transport_pair_generation_) const;
    static bool socket_type_supports_receive_flow_state (int type_);
    //  Classifies one topology-selected control frame. Returns true when the
    //  frame was Core-internal FLOWSTATE and has been consumed, whether or not
    //  it could be applied; the caller must then not pass it anywhere else.
    bool consume_receive_flow_state_frame (pipe_t *source_pipe_,
                                           const zlink::msg_t &msg_);

  private:
    //  These helpers run under _transport_pairs_sync. A held frame does not
    //  consume the pair epoch until validation promotes the winning source.
    void promote_pending_flow_state_locked (transport_pair_pipes_t &pair_);
    void buffer_pending_flow_state_locked (transport_pair_pipes_t &pair_,
                                           uint64_t source_connection_id_,
                                           bool paused_,
                                           uint64_t epoch_);
    static void discard_pending_flow_state_locked (
      transport_pair_pipes_t &pair_, uint64_t source_connection_id_,
      bool discard_all_);

  public:

#ifdef ZLINK_BUILD_TESTS
    //  Test-only synchronized snapshot of the socket monitor readiness set.
    uint32_t test_monitor_ready_count () const;
    static uint64_t test_local_peer_weight_send_attempt_count ();
    //  Test-only observation and injection for the completion-lane flow state.
    //  These compile out of the shipped runtime, so nothing here is reachable
    //  from a hot path.
    //  Writes the pair's received state without applying it to the pipe, which
    //  is the state a frame leaves behind between acceptance and application.
    //  That gap is a cross-thread race in production; this makes it reachable.
    //  Retains the pair's application pipe and hands it back, the way flow
    //  receipt does before it leaves the table mutex. The retain outlives
    //  termination, which is what lets a command report late.
    zlink::pipe_t *test_retain_application_pipe (
      uint64_t transport_pair_id_, uint64_t transport_pair_generation_);
    void test_release_pipe (zlink::pipe_t *pipe_);
    //  Reports a flow-state transition exactly as the queued command's handler
    //  does, so a late report can be delivered on purpose.
    void test_deliver_late_flow_state (zlink::pipe_t *pipe_,
                                       bool paused_,
                                       uint64_t epoch_);
    void test_consume_late_flow_state_frame (zlink::pipe_t *pipe_,
                                             bool paused_,
                                             uint64_t epoch_);
    bool test_set_pair_received_flow_state (uint64_t transport_pair_id_,
                                            uint64_t transport_pair_generation_,
                                            bool paused_);
    zlink::pipe_t *test_pair_pipe (uint64_t transport_pair_id_,
                                   uint64_t transport_pair_generation_,
                                   bool completion_lane_) const;
    bool test_pair_identity_for_peer (
      const unsigned char *peer_routing_id_, size_t peer_routing_id_size_,
      uint64_t *transport_pair_id_out_,
      uint64_t *transport_pair_generation_out_,
      bool *ready_out_ = NULL) const;
    bool test_completion_pair_queued (
      uint64_t transport_pair_id_,
      uint64_t transport_pair_generation_) const;
    int test_process_commands_only ();
    //  Reads the pipe's send-blocker causes without evaluating - and therefore
    //  without mutating - any of them.
    bool test_application_pipe_flow_probe (
      uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
      bool *out_active_, bool *hwm_full_, bool *remote_paused_,
      bool *byte_credit_waiter_ = NULL,
      uint64_t *in_flight_bytes_ = NULL) const;
    //  Queues one flow-state pipe command with a caller-chosen epoch, which is
    //  how a stale replay and a newer acceptance can be ordered on purpose.
    bool test_deliver_flow_state_command (uint64_t transport_pair_id_,
                                          uint64_t transport_pair_generation_,
                                          unsigned char state_,
                                          uint64_t epoch_);
    bool test_pending_flow_buffered (bool *paused_out_,
                                     uint64_t *epoch_out_,
                                     uint64_t *pair_id_out_ = NULL,
                                     uint64_t *generation_out_ = NULL,
                                     uint64_t *source_connection_id_out_ = NULL) const;
    bool test_buffer_flow_frame (uint64_t transport_pair_id_,
                                 uint64_t transport_pair_generation_,
                                 uint64_t source_connection_id_,
                                 bool paused_,
                                 uint64_t epoch_);
    //  Reports whether both validated lanes have admitted this pair.
    bool test_pair_is_ready (uint64_t transport_pair_id_,
                             uint64_t transport_pair_generation_) const;
    //  Seeds and reads the socket-wide epoch, so the wraparound boundary is
    //  reachable without performing 2^64 state changes.
    void test_set_local_receive_flow_epoch (uint64_t epoch_);
    uint64_t test_local_receive_flow_epoch () const;
    void test_fail_next_recv_pipe_pin ();
#endif
    // Pins an inbound source pipe while preserving the receive-path test
    // failpoint used to prove that no reply target escapes an unowned source.
    bool retain_received_source_pipe_ref (pipe_t *pipe_) const;
    bool begin_public_send_scope (
      std::optional<socket_public_send_scope_t> *scope_out_);
    bool begin_complete_send_scope (
      std::optional<socket_public_send_scope_t> *scope_out_);
    std::unique_ptr<socket_public_send_scope_t> begin_complete_send_scope ();
    void notify_incremental_send_released ();
    void hold_incremental_send_control_boundary ();
    void clear_incremental_send_control_boundary ();
    std::unique_ptr<socket_public_api_scope_t> begin_public_api_scope ();
    int rollback ();
    int rollback_scoped (socket_public_send_scope_t &scope_);
    int recv (zlink::msg_t *msg_, int flags_,
              bool *multipart_aborted_out_ = NULL);
    int recv_pipe (zlink::msg_t *msg_, zlink::pipe_t **pipe_out_, int flags_,
                   bool pin_pipe_out_ = false,
                   zlink::socket_receive_record_scope_t *record_scope_ = NULL);
    int recv_routed (zlink::msg_t *msg_,
                     zlink_routing_id_t *source_rid_out_,
                     int flags_,
                     uint64_t *connection_id_out_ = NULL,
                     zlink::pipe_t **source_pipe_out_ = NULL,
                     bool pin_source_pipe_out_ = false,
                     uint64_t *transport_pair_id_out_ = NULL,
                     uint64_t *transport_pair_generation_out_ = NULL,
                     uint64_t *route_binding_token_out_ = NULL,
                     zlink::socket_receive_record_scope_t *record_scope_ = NULL,
                     pipe_t::read_admission_fn *admission_ = NULL,
                     void *admission_userdata_ = NULL);
    // Reads a continuation while record_scope_ retains the receive owner
    // acquired by recv_pipe()/recv_routed(). Multipart callers release the
    // scope only after the complete record has been assembled and published.
    int recv_record_continuation (
      zlink::msg_t *msg_, zlink::socket_receive_record_scope_t &record_scope_);
    //  These three return a pipe whose lifetime is PINNED: they take a
    //  lifetime ref while the transport-pair table is locked, because the
    //  table slot is the only thing that proves the pipe is still alive and
    //  the caller dereferences the result after the table is unlocked. Every
    //  caller must call release_lifetime_ref () after its last dereference,
    //  including on its error paths.
    pipe_t *completion_pipe_for_application (pipe_t *application_pipe_) const;
    pipe_t *application_pipe_for_completion (pipe_t *completion_pipe_) const;
    pipe_t *completion_pipe_for_peer (const zlink_routing_id_t *peer_rid_) const;
    //  Borrowed predicate view. Do not dereference the result after another
    //  executor can process pipe termination.
    pipe_t *completion_pipe_for_transport_pair (uint64_t transport_pair_id_,
                                                uint64_t transport_pair_generation_) const;
    //  Pinned transport-pair lookup for callers that dereference the result.
    //  The caller releases the returned pipe exactly once.
    pipe_t *retain_completion_pipe_for_transport_pair (
      uint64_t transport_pair_id_, uint64_t transport_pair_generation_) const;
    //  Exact ready-pair lookup used by topology-aware reply routing. The
    //  returned pipe is pinned and must be released exactly once by the
    //  caller. A Completion lookup on a count-1 pair returns NULL.
    pipe_t *retain_transport_pair_pipe (
      uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
      transport_lane_t lane_) const;
    //  Reconnect-aware lookup for a saved logical route. Selects the latest
    //  ready pair that still matches peer RID and peer socket type, then pins
    //  the requested physical lane. A physical reconnect may allocate a new
    //  pair id, so pair identity is intentionally not part of this fallback.
    virtual pipe_t *retain_current_transport_pair_pipe (
      const zlink_routing_id_t *peer_rid_, int peer_socket_type_,
      transport_lane_t lane_) const;
    void cache_completion_pipe_routing_id (pipe_t *application_pipe_);
    //  Request/reply submit entries write to transport pipes directly instead
    //  of going through send()/recv(). They have to drain pending socket
    //  commands when the mailbox reports a pending batch; otherwise a queued
    //  bind, flow-state transition or activate-write can remain unapplied
    //  across the direct transport operation with no later public call to make
    //  progress. An empty mailbox keeps the normal hot-path throttle.
    int process_submit_commands ();
    //  Park on the socket mailbox for at most timeout_ms_ (or until a command
    //  such as activate_write / pair-ready lands), then drain it. The bounded
    //  wait primitive for public submit paths that must retry without
    //  sleeping through a fixed slice.
    uint64_t observe_submit_progress () const;
    int wait_submit_progress (uint64_t observed_epoch_, int timeout_ms_,
                              bool *owner_progress_held_);
    int wait_submit_progress (socket_public_send_scope_t &send_scope_,
                              uint64_t observed_epoch_, int timeout_ms_,
                              bool *owner_progress_held_);
    // Publish an admission-relevant route/credit transition to a blocked
    // request-reply submitter. The waiter check keeps ordinary transitions
    // out of the condition-variable mutex.
    void notify_submit_progress ();
    int close ();
    int close (int handoff_timeout_ms_);
    // Reserve close before the public wrapper tears down request state.
    int begin_close_handoff ();
    void complete_close_handoff ();
    int peer_command_from_io (zlink::msg_t *msg_, zlink::pipe_t *pipe_);
    int send_completion_submit (zlink_msg_t *parts_,
                                size_t part_count_,
                                const zlink_routing_id_t *target_rid_,
                                void *user_context_,
                                zlink_completion_id_t *completion_id_out_,
                                const routed_send_target_key_t
                                  *committed_target_ = NULL,
                                bool admission_gate_preacquired_ = false);
    // An incremental PAIR/DEALER send already owns the socket's multipart
    // admission scope at FINAL. Reuse that scope for the common immediate
    // whole-record attempt; EAGAIN leaves every input untouched so the caller
    // can release the multipart marker and enter the ordinary pending path.
    int try_immediate_completion_send_scoped (
      zlink_msg_t *parts_, size_t part_count_,
      socket_public_send_scope_t &send_scope_,
      routed_send_target_key_t *fallback_target_out_ = NULL,
      bool *fallback_target_valid_out_ = NULL,
      bool *admission_gate_retained_out_ = NULL);
    int send_completion_submit_blocking (
      zlink_msg_t *parts_, size_t part_count_,
      const zlink_routing_id_t *target_rid_or_null_);
    int request_admission_submit (
      zlink_msg_t *parts_, size_t part_count_,
      const zlink_routing_id_t *target_rid_or_null_,
      pipe_write_observer_fn admission_observer_,
      void *admission_observer_userdata_,
      send_pending_request_resolved_fn resolved_,
      send_pending_request_cleanup_fn cleanup_,
      send_pending_request_promote_fn promote_,
      void *resolution_context_, bool *pending_out_);
    int request_admission_submit_blocking (
      zlink_msg_t *parts_, size_t part_count_,
      const zlink_routing_id_t *target_rid_or_null_,
      pipe_write_observer_fn admission_observer_,
      void *admission_observer_userdata_);
    // Admit whatever the current pipe state allows. Resolution is published
    // only to the pull completion queue or an internal REQUEST admission hook.
    void drive_send_pending ();
    bool has_send_pending () const;
    void mark_send_completion_capacity_blocked ();
    void notify_send_completion_capacity_available ();
    socket_completion::queue_state_t &completion_runtime ()
    {
        return _runtime.completion_runtime;
    }
    const socket_completion::queue_state_t &completion_runtime () const
    {
        return _runtime.completion_runtime;
    }
    int receive_timeout_ms () const { return options.rcvtimeo; }
    int send_timeout_ms () const { return options.sndtimeo; }
    int adopt_accepted_transport_pair (
      const unsigned char *peer_routing_id_, size_t peer_routing_id_size_,
      uint64_t *pair_id_out_, uint64_t *generation_out_);
    void release_accepted_transport_pair (
      const unsigned char *peer_routing_id_, size_t peer_routing_id_size_,
      uint64_t pair_id_, uint64_t generation_);
    //  A physical lane uses this snapshot when its pair-fence timer expires.
    //  The pair key includes the generation, so a stale lane cannot observe a
    //  replacement pair as its own admission.
    bool transport_pair_is_ready (uint64_t transport_pair_id_,
                                  uint64_t transport_pair_generation_) const;
    int ensure_async_command_processing (bool retain_ = false);
    int ensure_completion_processing ();
    // A monitor can borrow the temporary executor installed for a deferred
    // transport-pair owner decision.  These helpers linearize that handoff
    // with the final owner-progress lease so neither side stops an executor
    // that the other has just adopted.
    int acquire_monitor_async_command_processing ();
    void release_monitor_async_command_processing (bool wait_for_quiescence_);
    void request_unowned_async_command_processing_stop (
      bool wait_for_quiescence_ = false);
    bool stop_unowned_async_command_processing_at_idle ();
    //  Active paired handshakes need one socket-owner mailbox turn after the
    //  peer HELLO reveals its type. The lease installs an asynchronous owner
    //  only while such decisions are outstanding; monitor, completion and
    //  probe consumers can retain that owner independently.
    int acquire_transport_pair_owner_progress ();
    int acquire_transport_pair_owner_progress_for_submit (int timeout_ms_);
    void release_transport_pair_owner_progress ();
    bool acquire_completion_poller (void *owner_);
    void release_completion_poller (void *owner_);
    bool acquire_poller_registration ();
    void release_poller_registration ();

    // True when the calling thread owns completion-lane command processing.
    bool completion_drain_permitted () const;

    //  Delivers replies from every ready Completion pipe. Called from the
    //  owned drain points, because a pipe that was made readable while no
    //  owner was draining does not report readiness again.
    void process_ready_completion_pipes ();
    //  Count-1 private heads use a pipe-owned intrusive MPSC queue. Publication
    //  retains both the pipe object and its inbound ypipe, so physical detach
    //  may invalidate readiness without making a queued pointer unsafe.
    bool enqueue_count1_completion_pipe (zlink::pipe_t *pipe_);
    void discard_count1_completion_ready_pipes ();
    bool release_count1_completion_drain (zlink::pipe_t *pipe_);
    //  Finish one Completion-pipe drain without losing an activation that
    //  raced the final empty read. Returns true when this owner reclaimed the
    //  exact pair and must drain it again.
    bool finish_completion_pipe_drain (uint64_t transport_pair_id_,
                                       uint64_t transport_pair_generation_,
                                       zlink::pipe_t *completion_pipe_);
    //  A budgeted pipe yields at a whole-record boundary. Release only the
    //  exact claimed source, then place remaining work at the queue tail so a
    //  different ready pair gets an owner turn first.
    bool requeue_completion_pipe_after_budget (
      uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
      zlink::pipe_t *completion_pipe_, bool receive_sync_held_);
    void drain_claimed_completion_pipe (uint64_t transport_pair_id_,
                                        uint64_t transport_pair_generation_,
                                        zlink::pipe_t *completion_pipe_);
    void resume_completion_processing_if_needed ();
    void notify_request_completion ();
    int drain_request_completions ();
    void acknowledge_request_completion_notification ();
    virtual int stream_mark_raw_part_receive ();
    virtual std::recursive_mutex *api_sync_mutex ();

    //  These functions are used by the polling mechanism to determine
    //  which events are to be reported from this socket.
    bool has_in ();
    bool has_out ();
    bool transport_has_out ();

    //  Joining and leaving groups
    int join (const char *group_);
    int leave (const char *group_);

    //  Using this function reaper thread ask the socket to register with
    //  its poller.
    void start_reaping (poller_t *poller_);

    //  i_poll_events implementation. This interface is used when socket
    //  is handled by the poller in the reaper thread.
    void in_event () ZLINK_FINAL;
    void out_event () ZLINK_FINAL;
    void timer_event (int id_) ZLINK_FINAL;

    //  i_pipe_events interface implementation.
    void read_activated (pipe_t *pipe_) ZLINK_FINAL;
    void write_activated (pipe_t *pipe_) ZLINK_FINAL;
    void hiccuped (pipe_t *pipe_) ZLINK_FINAL;
    void pipe_peer_terminated (pipe_t *pipe_) ZLINK_FINAL;
    void pipe_terminated (pipe_t *pipe_) ZLINK_FINAL;
    //  Releases the gauge slot and closes the pause duration for a pair that
    //  is torn down while paused, which never gets a RESUMED of its own.
    void flow_pause_released_on_termination (pipe_t *pipe_);
    void flow_state_applied (pipe_t *pipe_, bool paused_, uint64_t epoch_,
                             bool actual_writable_) ZLINK_FINAL;
    void flow_state_received (pipe_t *source_pipe_, unsigned char state_,
                              uint64_t epoch_) ZLINK_FINAL;
    void peer_weight_received (pipe_t *pipe_, uint32_t weight_) ZLINK_FINAL;

    int monitor (const char *endpoint_,
                 uint64_t events_,
                 int event_version_,
                 int type_,
                 uint64_t monitor_hwm_bytes_);

    void event_connected (
      const endpoint_uri_pair_t &endpoint_uri_pair_,
      zlink::fd_t fd_,
      transport_lane_t transport_lane_ = transport_lane_application,
      uint64_t transport_pair_id_ = 0,
      uint64_t transport_pair_generation_ = 0);
    void event_connect_delayed (const endpoint_uri_pair_t &endpoint_uri_pair_, int err_);
    void event_connect_retried (const endpoint_uri_pair_t &endpoint_uri_pair_, int interval_);
    void event_listening (const endpoint_uri_pair_t &endpoint_uri_pair_, zlink::fd_t fd_);
    void event_bind_failed (const endpoint_uri_pair_t &endpoint_uri_pair_, int err_);
    void event_accepted (const endpoint_uri_pair_t &endpoint_uri_pair_, zlink::fd_t fd_);
    void event_accept_failed (const endpoint_uri_pair_t &endpoint_uri_pair_, int err_);
    void event_closed (const endpoint_uri_pair_t &endpoint_uri_pair_, zlink::fd_t fd_);
    void event_close_failed (const endpoint_uri_pair_t &endpoint_uri_pair_, int err_);
    void event_disconnected (const endpoint_uri_pair_t &endpoint_uri_pair_,
                             uint64_t reason_,
                             const unsigned char *routing_id_,
                             size_t routing_id_size_,
                             transport_lane_t transport_lane_ = transport_lane_application,
                             uint64_t transport_pair_id_ = 0,
                             uint64_t transport_pair_generation_ = 0);
    void event_handshake_failed_no_detail (const endpoint_uri_pair_t &endpoint_uri_pair_, int err_);
    void event_handshake_failed_protocol (const endpoint_uri_pair_t &endpoint_uri_pair_, int err_);
    void event_handshake_failed_auth (const endpoint_uri_pair_t &endpoint_uri_pair_, int err_);
    void event_connection_ready_changed (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                         const unsigned char *routing_id_,
                                         size_t routing_id_size_,
                                         transport_lane_t transport_lane_ = transport_lane_application,
                                         uint64_t transport_pair_id_ = 0,
                                         uint64_t transport_pair_generation_ = 0);
    void event_transport_pair_lane_ready (
      const endpoint_uri_pair_t &endpoint_uri_pair_,
      const unsigned char *routing_id_,
      size_t routing_id_size_,
      transport_lane_t lane_,
      uint64_t pair_id_,
      uint64_t generation_);
    void validate_inproc_connection (pipe_t *pipe_);
    void emit_inproc_connection_ready (pipe_t *pipe_);
    int materialize_inproc_completion_lane (
      socket_base_t *bind_socket_, const options_t &bind_options_,
      const std::string &endpoint_uri_, uint64_t pair_id_,
      uint64_t pair_generation_, bool bind_side_direct_);

    //  Query the state of a specific peer. The default implementation
    //  always returns an ENOTSUP error.
    virtual int get_peer_state (const void *routing_id_, size_t routing_id_size_) const;

    int monitor_snapshot (zlink_monitor_status_t *out_);
    void auto_hwm_admission_counters (uint64_t *attempts_,
                                      uint64_t *blocked_) const;
    void auto_hwm_queue_counters (uint64_t *current_bytes_,
                                  uint64_t *oversize_count_,
                                  uint64_t *oversize_max_bytes_) const;
    void reset_auto_hwm_admission_counters ();
    //  Remote receive-flow observation counters (plan §6). All transitions
    //  happen off the per-message hot path: only a PAUSED<->RUNNING flip on
    //  this pipe's own thread and a rejected stale/duplicate frame on the
    //  transport I/O thread touch these.
    void flow_state_metrics (uint64_t *paused_connections_,
                             uint64_t *pause_applied_total_,
                             uint64_t *resume_applied_total_,
                             uint64_t *stale_total_,
                             uint64_t *last_pause_duration_ms_) const;
    void reset_flow_state_metrics ();
    void note_flow_state_stale (bool generation_stale_,
                                uint64_t received_generation_,
                                uint64_t current_generation_,
                                uint64_t received_epoch_,
                                uint64_t current_epoch_,
                                uint64_t pair_id_,
                                pipe_t *application_pipe_);
    auto_hwm_socket_plan_t prepare_auto_hwm_socket_plan (const auto_hwm_context_plan_t &context_);
    void collect_auto_hwm_queue_policies (
      std::vector<physical_queue_endpoint_policy_t> *out_);
    physical_queue_endpoint_policy_t make_auto_hwm_queue_policy (
      const std::shared_ptr<physical_queue_record_t> &queue_,
      bool writer_) const;
    void apply_physical_auto_hwm_plan (const auto_hwm_context_plan_t &context_,
                                       uint32_t recalc_reason_);
    void refresh_auto_hwm_policy (bool force_apply_ = false);
    void set_auto_hwm_role (auto_hwm_role_t role_);
    void set_auto_hwm_policy_enabled (bool enabled_);
    int configure_internal_monitor_queue (uint64_t hwm_bytes_);
    bool monitor_has_attached_pipes () const;
    void socket_peer_remote_endpoints (std::vector<std::string> *out_);
    void socket_bound_endpoints (std::set<std::string> *out_) const;
    bool socket_has_endpoint_history () const;
    bool socket_has_attached_pipes () const;
    bool socket_has_manual_connect_endpoints () const;
    int set_peer_weight (uint32_t weight_);
    int get_peer_weight (uint32_t *weight_out_) const;
    uint32_t local_peer_weight () const;
    int socket_id () const;
    std::shared_ptr<socket_reqrep_internal::socket_request_reply_state_t>
    request_reply_state () const;
    std::shared_ptr<socket_reqrep_internal::socket_request_reply_state_t>
    set_request_reply_state (
      const std::shared_ptr<socket_reqrep_internal::socket_request_reply_state_t> &state_);
    void clear_request_reply_state ();
    // Reply-token receive admission is owned by the request/reply registry,
    // while source scheduling is owned by the concrete socket FQ. These two
    // bridge methods keep the capacity decision and slot-release redrive at
    // their respective owners without exposing registry internals to sockets.
    bool router_reply_receive_slot_available () const;
    void reply_target_slots_released (size_t released_slots_);
    void revoke_router_reply_targets_for_rid (
      const zlink_routing_id_t *peer_rid_);
    std::shared_ptr<part_helper_internal::handle_state_t> part_helper_state () const;
    // Borrowed helper state is valid only while the caller pins this socket's
    // public handle. The socket keeps the immutable shared owner until final
    // destruction; close only withdraws the publication bit.
    part_helper_internal::handle_state_t *borrow_part_helper_state () const;
    std::shared_ptr<part_helper_internal::handle_state_t>
    set_part_helper_state (const std::shared_ptr<part_helper_internal::handle_state_t> &state_);
    void clear_part_helper_state ();
    bool part_helper_send_active () const;
    void set_part_helper_send_active (bool active_);
    bool part_helper_recv_ready () const;
    void set_part_helper_recv_ready (bool ready_);
    //  zlink_recv_part() buffers a complete physical DATA record before it
    //  publishes the first part. On a count-1 pair, keep the following REPLY
    //  private until that buffered public record reaches its FINAL part.
    int begin_public_part_receive_delivery_hold ();
    void bind_public_part_receive_delivery_hold (pipe_t *source_pipe_);
    void end_public_part_receive_delivery_hold (
      bool receive_sync_held_ = false);

    bool is_ctx_terminated () const;

  protected:
    socket_base_t (zlink::ctx_t *parent_, uint32_t tid_, int sid_);
    ~socket_base_t () ZLINK_OVERRIDE;

    //  Concrete algorithms for the x- methods are to be defined by
    //  individual socket types.
    virtual void xattach_pipe (zlink::pipe_t *pipe_,
                               bool subscribe_to_all_ = false,
                               bool locally_initiated_ = false) = 0;

    //  A paired Router may learn the peer routing ID before the Completion
    //    lane makes the transport pair ready. Concrete routing sockets can
    //  publish the deferred pair-ready edge when the second lane attaches.
    virtual bool emit_transport_pair_ready (zlink::pipe_t *pipe_)
    {
        LIBZLINK_UNUSED (pipe_);
        return false;
    }

    //  The default implementation assumes there are no specific socket
    //  options for the particular socket type. If not so, ZLINK_FINAL this
    //  method.
    virtual int xsetsockopt (int option_, const void *optval_, size_t optvallen_);

    //  The default implementation assumes there are no specific socket
    //  options for the particular socket type. If not so, ZLINK_FINAL this
    //  method.
    virtual int xgetsockopt (int option_, void *optval_, size_t *optvallen_);

    //  The default implementation assumes that send is not supported.
    virtual bool xhas_out ();
    virtual int xsend (
      zlink::msg_t *msg_,
      pipe_message_admission_t *admission_out_ = NULL);
    // Concrete sockets may commit a complete record in one pipe-owned
    // transaction. False means no frame was published or source ownership
    // transferred, and the caller must use the ordinary per-frame path.
    virtual bool xtry_send_complete_record (zlink::msg_t *parts_,
                                            size_t part_count_)
    {
        LIBZLINK_UNUSED (parts_);
        LIBZLINK_UNUSED (part_count_);
        return false;
    }
    virtual int xsend_pipe (
      zlink::msg_t *msg_, zlink::pipe_t **pipe_out_,
      pipe_message_admission_t *admission_out_ = NULL,
      pipe_write_observer_fn observer_ = NULL,
      void *observer_userdata_ = NULL);
    virtual int xsend_routed (const zlink_routing_id_t *target_rid_,
                              zlink::msg_t *msg_,
                              uint64_t *connection_id_out_,
                              uint64_t expected_connection_id_,
                              zlink::pipe_t **pipe_out_,
                              uint64_t expected_transport_pair_id_ = 0,
                              uint64_t expected_transport_pair_generation_ = 0,
                              pipe_message_admission_t *admission_out_ = NULL,
                              pipe_write_observer_fn observer_ = NULL,
                              void *observer_userdata_ = NULL,
                              routed_send_attempt_identity_t
                                *attempt_identity_out_ = NULL,
                              uint64_t expected_route_incarnation_id_ = 0,
                              bool request_only_ = false);
    //  Hot-path pair for the single-part blocking send: pick the pipe the
    //  load balancer would commit to, then admit one frame to exactly that
    //  pipe. Both run under one send scope, so the pointer never outlives
    //  the scope that observed it. Sockets without configured-endpoint
    //  routing report ENOTSUP and the caller uses the general path.
    virtual int xselect_routed_submit_pipe (pipe_t **pipe_out_,
                                            bool request_only_);
    // REQUEST route history is needed only if direct admission falls back to
    // a configured-endpoint retry. Keep its string/map work off the selected
    // pipe fast path.
    virtual int xcommit_request_submit_pipe (pipe_t *pipe_);
    virtual int xsend_selected_pipe (pipe_t *pipe_, msg_t *msg_, int flags_,
                                     bool request_only_,
                                     pipe_message_admission_t *admission_out_,
                                     pipe_write_observer_fn observer_,
                                     void *observer_userdata_);
    virtual int xsend_configured_endpoint (
      const std::string &endpoint_, zlink::msg_t *msg_, int flags_,
      bool request_only_,
      zlink::pipe_t **pipe_out_,
      pipe_message_admission_t *admission_out_ = NULL,
      pipe_write_observer_fn observer_ = NULL,
      void *observer_userdata_ = NULL);
    virtual int xselect_routed_submit_target (
      const zlink_routing_id_t *router_rid_or_null_,
      zlink_routed_submit_target_t *target_out_);
    virtual int xselect_routed_submit_target_internal (
      const zlink_routing_id_t *router_rid_or_null_,
      zlink_routed_submit_target_t *target_out_,
      uint64_t *transport_connection_id_out_,
      uint64_t *route_incarnation_id_out_);
    virtual int xselect_request_submit_target (
      const zlink_routing_id_t *router_rid_or_null_,
      zlink_routed_submit_target_t *target_out_,
      uint64_t *transport_connection_id_out_,
      uint64_t *route_incarnation_id_out_,
      std::string *logical_endpoint_out_);
    virtual void xforget_request_route_endpoint (
      const std::string &endpoint_)
    {
        LIBZLINK_UNUSED (endpoint_);
    }
    // ROUTER can replace a route from direct I/O dispatch while pending send is
    // between physical EAGAIN and pending publication. Concrete sockets that
    // have such a route fence validate and publish under that fence followed by
    // send_pending_runtime::sync. No path may take those locks in reverse.
    virtual std::mutex *send_pending_target_mutex () const
    {
        return NULL;
    }
    virtual bool xsend_pending_target_current_locked (
      const routed_send_target_key_t &target_) const
    {
        LIBZLINK_UNUSED (target_);
        return true;
    }
    virtual bool xsubmit_retry_allowed (const zlink_routing_id_t *target_rid_, int err_) const;
    virtual int xrollback ();

    //  The default implementation assumes that recv in not supported.
    virtual bool xhas_in ();
    virtual size_t xredrive_reply_token_waiters (size_t max_pipes_)
    {
        LIBZLINK_UNUSED (max_pipes_);
        return 0;
    }
    virtual int xrecv (zlink::msg_t *msg_);
    virtual int xrecv_pipe (zlink::msg_t *msg_, zlink::pipe_t **pipe_out_);
    virtual int xrecv_routed (zlink::msg_t *msg_,
                              zlink_routing_id_t *source_rid_out_,
                              uint64_t *connection_id_out_,
                              zlink::pipe_t **source_pipe_out_ = NULL,
                              pipe_t::read_admission_fn *admission_ = NULL,
                              void *admission_userdata_ = NULL,
                              uint64_t *route_binding_token_out_ = NULL);
    virtual int xterm_peer_rid (const zlink_routing_id_t *peer_rid_);
    int xterm_transport_pair (uint64_t transport_pair_id_,
                              uint64_t transport_pair_generation_);
    virtual void xsocket_msg_pipe_terminated (zlink::pipe_t *pipe_);
    virtual int xpeer_command (zlink::msg_t *msg_, zlink::pipe_t *pipe_);
    virtual void xlocal_peer_weight_changed ();
    virtual int apply_peer_weight (zlink::pipe_t *pipe_, uint32_t weight_);
    virtual void initialize_peer_weight (zlink::pipe_t *pipe_,
                                         uint32_t weight_);
    void initialize_recorded_peer_weight (zlink::pipe_t *pipe_);
    // Caller holds pipe_->transport_sync() until the scheduler publishes the
    // returned value. This keeps reconnect generation changes ordered before
    // route/LB visibility without taking route locks in the inverse order.
    bool recorded_peer_weight_ready_locked (zlink::pipe_t *pipe_,
                                            uint32_t *weight_out_) const;
    virtual uint32_t monitor_ready_count () const;

    //  Reclassifies the next queued record on a ready count-1 Application
    //  pipe. The caller already owns the receive runtime (public lease or
    //  receive.sync). Normally true means completion work was queued. With
    //  claim_private_head_ true it instead means the current completion owner
    //  reclaimed a private head directly after an empty-read race.
    bool reclassify_transport_pair_application_head (
      pipe_t *pipe_, bool claim_private_head_ = false);

    //  i_pipe_events will be forwarded to these functions.
    virtual void xread_deactivated (pipe_t *pipe_);
    virtual void xread_activated (pipe_t *pipe_);
    virtual void xwrite_activated (pipe_t *pipe_);
    virtual void xhiccuped (pipe_t *pipe_);
    virtual void xpipe_terminated (pipe_t *pipe_) = 0;

    //  the default implementation assumes that joub and leave are not supported.
    virtual int xjoin (const char *group_);
    virtual int xleave (const char *group_);

    int accept_peer_weight (pipe_t *pipe_, uint32_t weight_);
    bool deliver_local_peer_weight (pipe_t *pipe_, uint32_t weight_);
    void broadcast_local_peer_weight ();
    bool send_local_peer_weight (pipe_t *pipe_);

    //  Topology-selected flow state. Count 1 stages a Core control on the
    //  Application path; count 2 writes the historical Completion command.
    //  Both are consumed by the peer Core and never reach public receive.
    void write_receive_flow_state_frame (pipe_t *control_pipe_,
                                         unsigned char state_,
                                         uint64_t epoch_);
    void sync_local_receive_flow_state_to_pair (pipe_t *control_pipe_);

    //  Delay actual destruction of the socket.
    void process_destroy () ZLINK_FINAL;

    int connect_internal (const char *endpoint_uri_);
    int term_endpoint_internal (const char *endpoint_uri_);
    int bind_inproc_endpoint (const char *endpoint_uri_);
    int bind_transport_listener (const std::string &protocol_,
                                 const std::string &address_,
                                 io_thread_t *io_thread_);
    int resolve_connect_address (const std::string &protocol_,
                                 const std::string &address_,
                                 address_t *paddr_) const;
    int start_async_mailbox_processing (io_thread_t *io_thread_);
    bool stop_async_mailbox_processing (bool require_unowned_ = false);
    void wait_async_quiesced (int timeout_ms_);
    static void resolve_socket_msg_source_rid (pipe_t *pipe_, zlink_routing_id_t *out_);

  public:
    void store_last_recv_source_rid (pipe_t *pipe_);
    void store_last_recv_source_rid (const zlink_routing_id_t *source_rid_);
    void clear_last_recv_source_rid ();
    bool copy_last_recv_source_rid (zlink_routing_id_t *out_) const;
    const zlink_routing_id_t *last_recv_source_rid_view () const;
    socket_base_t *detach_monitor_socket (bool send_monitor_stopped_event_ = true);
    void arm_send_recovery_after_backpressure ();

  protected:
    // Transfer a monitor-started command executor lease to a longer-lived
    // async socket consumer before registering that consumer.
    void retain_async_command_processing ();
    int recv_common (zlink::msg_t *msg_, int flags_,
                     zlink::socket_receive_runtime_t::mode_t mode_,
                     zlink::pipe_t **pipe_out_,
                     zlink_routing_id_t *source_rid_out_,
                     uint64_t *connection_id_out_,
                     bool pin_pipe_out_ = false,
                     zlink::socket_receive_record_scope_t *record_scope_ = NULL);
    // Concrete receive algorithms may fence their socket-specific queue state,
    // but the receive runtime and its ownership protocol remain base-owned.
    mutex_t &receive_sync () { return receive_runtime ().sync; }
    //  A pipe became writable again: nudge the admit loop for that target.
    void notify_send_pending_writable (pipe_t *pipe_);
    void flush_deferred_peer_controls ();
    void mark_deferred_peer_controls ();
    //  A route ended: fail every pending record bound to that exact target.
    void fail_pull_send_pending_for_logical_target (
      const zlink_routing_id_t *peer_rid_, int terminal_errno_);
    void fail_pull_send_pending_for_logical_endpoint (
      const std::string &endpoint_, int terminal_errno_);
    void fail_pull_request_pending_for_logical_target (
      const zlink_routing_id_t *peer_rid_);
    void fail_pull_request_pending_for_logical_endpoint (
      const std::string &endpoint_);
    void emit_socket_monitor_value_event (uint64_t event_,
                                          uint64_t value_,
                                          const endpoint_uri_pair_t &endpoint_uri_pair_);
    void emit_peer_weight_changed (pipe_t *pipe_, uint32_t weight_,
                                   const blob_t *public_routing_id_ = NULL);
    void snapshot_attached_pipes (std::vector<pipe_t *> *out_);
    bool has_attached_pipes () const;

  private:
    friend class ctx_t;
    friend class socket_poller_t;
    friend struct multipart_send_facade_t;

    auto_hwm_socket_plan_t prepare_auto_hwm_socket_plan_locked (
      const auto_hwm_context_plan_t &context_);
    physical_queue_endpoint_policy_t make_auto_hwm_queue_policy_locked (
      const std::shared_ptr<physical_queue_record_t> &queue_,
      bool writer_) const;

    int get_events_for_poller (int events_, uint32_t *out_,
                               bool transport_output_);

    bool has_stable_completion_processing_owner () const;
    void invalidate_completion_processing_owner ();
    void publish_completion_processing_owner ();
    int acquire_transport_pair_owner_progress_with_timeout (
      int timeout_ms_, int timeout_errno_);

    // Direct public send currently shares one scope between single-part and
    // logical multipart wrappers. Keep the admission/sync decision and the
    // blocking retry runner behind one internal boundary so future structural
    // candidates can change them independently.
    int send_direct_with_retry (const zlink_routing_id_t *target_rid_,
                                zlink::msg_t *msg_,
                                int flags_,
                                socket_public_send_scope_t &scope_,
                                uint64_t *connection_id_out_ = NULL,
                                uint64_t expected_connection_id_ = 0,
                                bool report_multipart_abort_ = false,
                                zlink::pipe_t **pipe_out_ = NULL,
                                uint64_t expected_transport_pair_id_ = 0,
                                uint64_t expected_transport_pair_generation_ = 0,
                                bool record_context_admission_ = true,
                                bool commands_already_processed_ = false,
                                pipe_write_observer_fn observer_ = NULL,
                                void *observer_userdata_ = NULL,
                                routed_send_attempt_identity_t
                                  *attempt_identity_out_ = NULL,
                                uint64_t expected_route_incarnation_id_ = 0,
                                bool manage_public_send_recovery_ = true,
                                bool request_only_ = false);

    enum
    {
        monitor_max_values = zlink::socket_monitor_max_values
    };

    typedef zlink::socket_monitor_event_record_t monitor_event_record_t;
    typedef zlink::socket_endpoint_pipe_t endpoint_pipe_t;
    typedef zlink::socket_endpoints_t endpoints_t;
    typedef zlink::socket_inprocs_t inprocs_t;
    typedef zlink::socket_endpoint_runtime_t endpoint_runtime_t;
    typedef zlink::socket_command_runtime_t command_runtime_t;
    typedef zlink::socket_receive_runtime_t receive_runtime_t;
    typedef zlink::socket_monitor_runtime_t monitor_runtime_t;
    typedef zlink::socket_dispatch_bridge_t dispatch_bridge_t;
    typedef zlink::socket_lifecycle_coordinator_t lifecycle_coordinator_t;
    typedef zlink::socket_runtime_t socket_runtime_t;

    endpoint_runtime_t &endpoint_runtime () { return _runtime.endpoint_runtime; }
    const endpoint_runtime_t &endpoint_runtime () const { return _runtime.endpoint_runtime; }
    command_runtime_t &command_runtime () { return _runtime.command_runtime; }
    const command_runtime_t &command_runtime () const { return _runtime.command_runtime; }
    receive_runtime_t &receive_runtime () { return _runtime.receive_runtime; }
    const receive_runtime_t &receive_runtime () const { return _runtime.receive_runtime; }
    monitor_runtime_t &monitor_runtime () { return _runtime.monitor_runtime; }
    const monitor_runtime_t &monitor_runtime () const { return _runtime.monitor_runtime; }
    dispatch_bridge_t &dispatch_runtime () { return _runtime.dispatch_bridge; }
    const dispatch_bridge_t &dispatch_runtime () const { return _runtime.dispatch_bridge; }
    socket_submit_progress_runtime_t &submit_progress_runtime ()
    {
        return _runtime.submit_progress_runtime;
    }
    socket_send_pending_runtime_t &send_pending_runtime ()
    {
        return _runtime.send_pending_runtime;
    }
    const socket_send_pending_runtime_t &send_pending_runtime () const
    {
        return _runtime.send_pending_runtime;
    }
    lifecycle_coordinator_t &lifecycle_coordinator () { return _runtime.lifecycle_coordinator; }
    lifecycle_coordinator_t &lifecycle_coordinator () const
    {
        return _runtime.lifecycle_coordinator;
    }

    // test if event should be sent and then dispatch it
    void event (const endpoint_uri_pair_t &endpoint_uri_pair_,
                const unsigned char *routing_id_,
                size_t routing_id_size_,
                 uint64_t values_[],
                 uint64_t values_count_,
                 uint64_t type_,
                 uint32_t internal_flags_ = 0,
                 transport_lane_t transport_lane_ = transport_lane_application,
                 uint64_t transport_pair_id_ = 0,
                 uint64_t transport_pair_generation_ = 0);
    // Socket event data dispatch
    static void monitor_task_main (void *arg_);
    static void monitor_delivery_ready_pump (void *arg_);
    void pump_monitor_events ();
    void enqueue_monitor_event (const monitor_event_record_t &record_);
    bool build_monitor_event_record (monitor_event_record_t *out_,
                                     uint64_t event_,
                                     const uint64_t values_[],
                                     uint64_t values_count_,
                                     const unsigned char *routing_id_,
                                     size_t routing_id_size_,
                                     const endpoint_uri_pair_t &endpoint_uri_pair_) const;
    bool dispatch_monitor_event (void *monitor_socket_,
                                 const monitor_event_record_t &record_) const;

    // Monitor socket cleanup
    void stop_monitor (bool send_monitor_stopped_event_ = true);

    //  Creates new endpoint ID and adds the endpoint to the map.
    void add_endpoint (const endpoint_uri_pair_t &endpoint_pair_, own_t *endpoint_, pipe_t *pipe_);
    //  Paired transports register both lanes under the same endpoint key and
    //  keep the shared pair state so that terminating the endpoint stops the
    //  whole pair instead of leaving one lane to reconnect on its own.
    void add_transport_pair_endpoint (
      const endpoint_uri_pair_t &endpoint_pair_,
      own_t *endpoint_,
      pipe_t *pipe_,
      const std::shared_ptr<transport_pair_state_t> &pair_state_,
      const std::shared_ptr<transport_pair_connect_intent_t> &intent_,
      transport_lane_t lane_);
    int create_connect_session (
      const std::string &protocol_, const std::string &address_,
      const endpoint_uri_pair_t &endpoint_pair_, io_thread_t *io_thread_,
      const options_t &lane_options_,
      const std::shared_ptr<transport_pair_state_t> &pair_state_,
      const std::shared_ptr<transport_pair_connect_intent_t> &intent_);
    int create_resolved_connect_session (
      address_t *paddr_, const endpoint_uri_pair_t &endpoint_pair_,
      io_thread_t *io_thread_, const options_t &lane_options_,
      const std::shared_ptr<transport_pair_state_t> &pair_state_,
      const std::shared_ptr<transport_pair_connect_intent_t> &intent_);

    //  To be called after processing commands or invoking any command
    //  handlers explicitly. If required, it will deallocate the socket.
    void check_destroy ();

    //  Moves the flags from the message to local variables,
    //  to be later retrieved by getsockopt.
    void extract_flags (const msg_t *msg_);

    //  Used to check whether the object is a socket.
    socket_public_handle_t *_public_handle;
    std::atomic<uint32_t> _tag;

    //  If true, associated context was already terminated.
    std::atomic<bool> _ctx_terminated;

    //  Parse URI string.
    static int parse_uri (const char *uri_, std::string &scheme_, std::string &path_);

    //  Check whether transport protocol, as specified in connect or
    //  bind, is available and compatible with the socket type.
    int check_protocol (const std::string &protocol_) const;

    //  Register the pipe with this socket.
    void attach_pipe (zlink::pipe_t *pipe_,
                      bool subscribe_to_all_ = false,
                      bool locally_initiated_ = false,
                      bool transport_validated_ = true);

    //  Processes commands sent to this socket (if any). If timeout is -1,
    //  returns only after at least one command was processed.
    //  If throttle argument is true, commands are processed at most once
    //  in a predefined time period. force_if_command_pending bypasses only a
    //  throttled skip backed by a mailbox command hint.
    int process_commands (int timeout_,
                          bool throttle_,
                          bool force_if_command_pending_ = false,
                          const uint64_t *observed_command_wait_epoch_ = NULL);
    bool try_inc_mailbox_ref ();
    void inc_mailbox_ref ();
    void dec_mailbox_ref ();
    void schedule_finalize_destroy ();
    void finalize_destroy ();
    void finish_close_reap ();
    void materialize_pending_inprocs_before_reap ();
    void process_async_mailbox ();
    void notify_receive_progress_locked ();
    void notify_receive_progress ();
    int wait_receive_progress (uint64_t observed_epoch_, int timeout_ms_);
    //  Stage 1 (plan 7.1): poll readiness consults this once per socket per
    //  zlink_poll(), so keep it inlineable instead of a cross-TU call.
    bool async_mailbox_owns_commands () const
    {
        const receive_runtime_t &receive = receive_runtime ();
        const socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
        return receive.async_command_handoff_pending.load (
                 std::memory_order_acquire)
               || lifecycle.async_mailbox_active.load (std::memory_order_acquire)
               || lifecycle.async_quiesce_pending.load (std::memory_order_acquire);
    }
#ifdef ZLINK_BUILD_TESTS
  public:
    void test_receive_owner_snapshot (uint64_t *progress_epoch_out_,
                                      uint64_t *public_mailbox_drains_out_,
                                      uint64_t *async_mailbox_drains_out_);
    void test_set_receive_wait_hook (receive_runtime_t::wait_hook_fn hook_,
                                     void *userdata_);
    void test_set_receive_record_hooks (
      receive_runtime_t::record_hook_fn acquired_hook_,
      receive_runtime_t::record_hook_fn contention_hook_, void *userdata_);
    void test_set_receive_command_sync_probe_hook (
      receive_runtime_t::command_sync_probe_hook_fn hook_, void *userdata_);
    bool test_resume_deferred_transport_pair_owner_request ();
  private:
#endif
    //  close / ctx term: fail every pending record fast. LINGER does not
    //  apply - it covers bytes already admitted, and a pending record is by
    //  definition not admitted yet.
    void fail_all_send_pending (int terminal_errno_);
    //  Claim/finish helpers used by the admit loop.
    bool claim_send_pending_head (send_pending_record_t **out_);
    int try_admit_send_pending (send_pending_record_t *record_);
    int select_routed_submit_target_internal (
      const zlink_routing_id_t *router_rid_or_null_,
      zlink_routed_submit_target_t *target_out_,
      uint64_t *transport_connection_id_out_,
      uint64_t *route_incarnation_id_out_);
    int try_admit_send_parts (zlink_msg_t *parts_,
                              size_t part_count_,
                              const routed_send_target_key_t &target_,
                              bool has_target_,
                              bool commands_already_processed_,
                              send_pending_record_t *record_ = NULL,
                              pipe_write_observer_fn observer_ = NULL,
                              void *observer_userdata_ = NULL,
                              bool request_admission_ = false);
    int try_admit_send_parts_scoped (
      zlink_msg_t *parts_, size_t part_count_,
      const routed_send_target_key_t &target_, bool has_target_,
      socket_public_send_scope_t &scope_,
      zlink::pipe_t **attempted_pipe_out_ = NULL,
      bool commands_already_processed_ = false,
      pipe_write_observer_fn observer_ = NULL,
      void *observer_userdata_ = NULL,
      bool request_admission_ = false,
      bool manage_public_send_recovery_ = true,
      const zlink_routing_id_t *transient_target_rid_ = NULL,
      zlink::pipe_t *transient_selected_pipe_ = NULL);
    void finish_send_pending (send_pending_record_t *record_,
                              zlink_send_complete_result_t result_,
                              int terminal_errno_);
    void destroy_send_pending_record (send_pending_record_t *record_);
    static void close_send_parts (std::vector<zlink_msg_t> *parts_);
    static int copy_send_parts (zlink_msg_t *parts_,
                                size_t part_count_,
                                std::vector<zlink_msg_t> *copies_);
    int send_pending_submit (zlink_msg_t *parts_,
                             size_t part_count_,
                             const zlink_routed_submit_target_t *target_,
                             zlink_send_op_id_t *op_id_out_,
                             bool pull_completion_,
                             void *user_context_,
                             zlink_completion_id_t *completion_id_out_,
                             bool request_admission_ = false,
                             pipe_write_observer_fn admission_observer_ = NULL,
                             void *admission_observer_userdata_ = NULL,
                             send_pending_request_resolved_fn request_resolved_ = NULL,
                             send_pending_request_cleanup_fn request_cleanup_ = NULL,
                             void *request_resolution_context_ = NULL,
                             const routed_send_target_key_t
                               *committed_target_ = NULL,
                             bool admission_gate_preacquired_ = false,
                             send_pending_request_promote_fn
                               request_promote_ = NULL);
    static void reaper_mailbox_handler (void *arg_);
    static void reaper_mailbox_pre_post (void *arg_);
    static void async_mailbox_handler (void *arg_);
    static void async_mailbox_pre_post (void *arg_);
    static socket_base_t *current_async_mailbox_dispatch_socket ();
    void defer_socket_msg_pipe_termination (pipe_t *pipe_);
    void process_deferred_socket_msg_pipe_terminations ();

    //  Handlers for incoming commands.
    void process_stop () ZLINK_FINAL;
    void process_bind (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void process_term (int linger_) ZLINK_FINAL;
    void process_term_endpoint (std::string *endpoint_) ZLINK_FINAL;
    void process_transport_pair_owner_request (
      zlink::session_base_t *session_, int peer_socket_type_,
      uint64_t connection_id_, uint64_t pair_id_, uint64_t generation_,
      unsigned char lane_) ZLINK_FINAL;

    void refresh_attached_pipe_hwms ();
    void update_pipe_options (int option_);

    std::string resolve_tcp_addr (std::string endpoint_uri_, const char *tcp_address_);
    // A normal public close cannot reap the socket while its asynchronous
    // mailbox owner still holds scheduled work.
    void finish_close_handoff (int handoff_timeout_ms_ = -1);

    //  Socket's mailbox object.
    i_mailbox *_mailbox;

    //  Keep these counters in all builds so the class layout does not vary
    //  across translation units compiled with different debug settings.
    int _term_pipe_acks_registered;
    int _term_pipe_acks_received;
    std::set<pipe_t *> _term_pipes;

    //  Improves efficiency of time measurement.
    clock_t _clock;
    mutable socket_runtime_t _runtime;
    socket_request_reply_bridge_t _request_reply_bridge;
    auto_hwm_role_t _auto_hwm_role;
    bool _auto_hwm_role_override;
    bool _auto_hwm_policy_enabled;
    bool _manual_sndhwm;
    bool _manual_rcvhwm;
    //  Serializes completion drains and the async-worker/public-poller owner
    //  transition.
    mutable mutex_t _completion_owner_sync;
    // Even generations denote no stable completion owner (or an ownership
    // handoff); odd generations denote an installed public-poller or retained
    // async owner. A request can validate the same odd generation twice and
    // avoid both owner locks in the steady state.
    std::atomic<uint32_t> _completion_processing_owner_generation;
    std::atomic<uint32_t> _completion_poller_refs;
    std::atomic<void *> _completion_poller_owner;
    std::atomic<bool> _request_completion_pending;
    mutable mutex_t _transport_pair_owner_progress_sync;
    uint32_t _transport_pair_owner_progress_refs;
    bool _async_command_processing_stop_requested;
    std::atomic<bool> _async_command_processing_retained;
    //  Auto-HWM planning runs on the context control runtime while option
    //  updates and monitor snapshots run on public threads. Keep the socket
    //  plan and the option values used to derive it in one snapshot domain.
    //  This lock is never acquired by send admission.
    mutable mutex_t _auto_hwm_sync;
    auto_hwm_context_plan_t _auto_hwm_context_plan;
    auto_hwm_socket_plan_t _auto_hwm_socket_plan;
    uint64_t _auto_hwm_last_recalc_ms;
    uint32_t _auto_hwm_last_recalc_reason;
    alignas (64) std::atomic<uint64_t> _auto_hwm_send_attempts;
    alignas (64) std::atomic<uint64_t> _auto_hwm_send_blocked_attempts;
    //  Remote receive-flow observation counters (plan §6). Gauge and
    //  monotonic totals; reset_flow_state_metrics() zeroes all of them.
    std::atomic<uint64_t> _flow_paused_connections;
    std::atomic<uint64_t> _flow_pause_applied_total;
    std::atomic<uint64_t> _flow_resume_applied_total;
    std::atomic<uint64_t> _flow_state_stale_total;
    std::atomic<uint64_t> _flow_last_pause_duration_ms;
    //  Public option updates are serialized by the socket API lock, while
    //  asynchronous route adoption and dispatch read the current policy
    //  without that lock. The value is independent of the option snapshot;
    //  relaxed atomic access removes that cross-thread data race while the
    //  existing locks continue to order each update's delivery side effects.
    std::atomic<uint32_t> _local_peer_weight;
    typedef std::pair<uint64_t, uint64_t> transport_pair_key_t;
    typedef std::map<transport_pair_key_t, transport_pair_pipes_t> transport_pairs_t;
    //  Owner of the transport pair table. Pair admission, readiness, teardown
    //  and Completion pipe consumption all run under this mutex; the pipes
    //  themselves are drained outside it so completion processing never runs
    //  with the table locked.
    mutable mutex_t _transport_pairs_sync;
    transport_pairs_t _transport_pairs;
    typedef std::map<std::string, accepted_transport_pair_identity_t>
      accepted_transport_pairs_t;
    //  Passive network sessions have no configured endpoint shared by both
    //  physical connections. READY's adopted Routing-Id is therefore the
    //  socket-local association key; the values remain local lifetime fences
    //  and never appear on the wire or public surface.
    accepted_transport_pairs_t _accepted_transport_pairs;
    std::deque<transport_pair_key_t> _ready_completion_pairs;
    std::set<transport_pair_key_t> _ready_completion_pair_set;
    //  Lock-free producer head for count-1 shared Application pipes. The
    //  completion owner atomically detaches one finite batch and reverses it to
    //  preserve FIFO publication order; requeues therefore belong to the next
    //  bounded owner turn.
    std::atomic<pipe_t *> _ready_count1_completion_pipes;
    std::atomic<bool> _public_part_receive_delivery_hold_active;
    pipe_t *_public_part_receive_delivery_hold_pipe;
    transport_pair_key_t _public_part_receive_delivery_hold_key;
    //  Socket-wide local receive-flow state, guarded by _transport_pairs_sync
    //  so pair fanout and new-pair synchronisation serialise against the same
    //  state. The epoch advances only on a real state change; a repeated call
    //  with the same state succeeds without emitting anything.
    unsigned char _local_receive_flow_state;
    uint64_t _local_receive_flow_epoch;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (socket_base_t)
};

//  Blocking submit paths may temporarily retain the executor that owns
//  transport-pair command progress. Keep that lease release in one shared
//  scope so every early return follows the same ownership rule.
class transport_pair_owner_progress_scope_t
{
  public:
    explicit transport_pair_owner_progress_scope_t (socket_base_t *socket_) :
        _socket (socket_), _held (false)
    {
    }

    ~transport_pair_owner_progress_scope_t ()
    {
        if (_held)
            _socket->release_transport_pair_owner_progress ();
    }

    bool *held_state () { return &_held; }

  private:
    socket_base_t *_socket;
    bool _held;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (transport_pair_owner_progress_scope_t)
};

class routing_socket_base_t : public socket_base_t
{
  protected:
    routing_socket_base_t (class ctx_t *parent_, uint32_t tid_, int sid_);
    ~routing_socket_base_t () ZLINK_OVERRIDE;

    // methods from socket_base_t
    int xsetsockopt (int option_, const void *optval_, size_t optvallen_) ZLINK_OVERRIDE;
    void xwrite_activated (pipe_t *pipe_) ZLINK_FINAL;

    // own methods
    std::string extract_connect_routing_id ();
    bool connect_routing_id_is_set () const;

    struct out_pipe_t
    {
        pipe_t *pipe;
        bool active;
        bool locally_initiated;
        uint32_t weight;
    };

    void add_out_pipe (blob_t routing_id_, pipe_t *pipe_,
                       bool locally_initiated_, uint32_t initial_weight_ = 100);
    bool has_out_pipe (const blob_t &routing_id_) const;
    out_pipe_t *lookup_out_pipe (const blob_t &routing_id_);
    const out_pipe_t *lookup_out_pipe (const blob_t &routing_id_) const;
    void erase_out_pipe (const pipe_t *pipe_);
    int terminate_out_pipe_by_routing_id (const zlink_routing_id_t *peer_rid_);
    out_pipe_t try_erase_out_pipe (const blob_t &routing_id_);
    void mark_out_pipe_active (out_pipe_t *out_pipe_);
    void mark_out_pipe_inactive (out_pipe_t *out_pipe_);
    void update_out_pipe_weight (out_pipe_t *out_pipe_, uint32_t weight_);
    bool has_writable_weighted_out_pipes () const;
    bool xsubmit_retry_allowed (const zlink_routing_id_t *target_rid_,
                                int err_) const ZLINK_OVERRIDE;
    // ROUTER mutates routes from direct I/O dispatch and supplies a lifecycle
    // fence. STREAM remains owner-serialized and returns no mutex, avoiding a
    // partial lock policy on its independent routing path.
    virtual std::mutex *route_lifecycle_mutex () const
    {
        return NULL;
    }
    template <typename Func> bool any_of_out_pipes (Func func_)
    {
        bool res = false;
        for (out_pipes_t::iterator it = _out_pipes.begin (), end = _out_pipes.end ();
             it != end && !res; ++it) {
            res |= func_ (it->second);
        }

        return res;
    }

  private:
    //  Outbound pipes indexed by the peer IDs.
    typedef std::map<blob_t, out_pipe_t> out_pipes_t;
    typedef std::map<pipe_t *, out_pipes_t::iterator> out_pipe_index_t;
    typedef std::set<blob_t> submit_retry_local_rids_t;
    out_pipes_t _out_pipes;
    out_pipe_index_t _out_pipe_index;
    submit_retry_local_rids_t _submit_retry_local_rids;
    size_t _writable_weighted_out_pipes;

    // Next assigned name on a zlink_connect() call used by ROUTER socket type.
    std::string _connect_routing_id;
};
}

#endif
