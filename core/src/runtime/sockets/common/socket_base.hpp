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

typedef int (*zlink_stream_on_raw_fn) (const zlink_routing_id_t *rid_, zlink_msg_t *msg_);

namespace zlink
{
class ctx_t;
class msg_t;
class pipe_t;
class io_thread_t;
class socket_base_t;

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

//  Socket types that own an asynchronous send completion channel. This is the
//  same set zlink_send_async accepts, and it is what widens the
//  ZLINK_POLLCOMPLETION registration check beyond reply completions.
bool socket_type_supports_send_completion (int type_);

typedef void (*sub_io_handler_fn) (const zlink_routing_id_t *source_rid_,
                                   const char *topic_,
                                   size_t topic_len_,
                                   zlink_msg_t *parts_,
                                   size_t part_count_,
                                   void *userdata_);

struct socket_request_reply_bridge_t
{
    socket_request_reply_bridge_t () :
        request_reply_state_present (false),
        part_helper_state_present (false)
    {
    }

    std::shared_ptr<socket_reqrep_internal::socket_request_reply_state_t> request_reply_state;
    std::shared_ptr<part_helper_internal::handle_state_t> part_helper_state;
    std::atomic<bool> request_reply_state_present;
    std::atomic<bool> part_helper_state_present;
};

struct transport_pair_pipes_t
{
    transport_pair_pipes_t () :
        application (NULL),
        completion (NULL),
        generation (0),
        application_attached (false),
        application_validated (false),
        completion_validated (false),
        ready (false),
        draining (false),
        remote_flow_seen (false),
        remote_flow_paused (false),
        remote_flow_pause_accounted (false),
        remote_flow_epoch (0)
    {
    }

    pipe_t *application;
    pipe_t *completion;
    uint64_t generation;
    bool application_attached;
    bool application_validated;
    bool completion_validated;
    bool ready;
    //  Set while a thread consumes this pair's Completion pipe. The pipe has a
    //  single-consumer queue, so two owners must not read it at once.
    bool draining;
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
};

class socket_recv_source_rid_scope_t
{
  public:
    socket_recv_source_rid_scope_t (socket_base_t *socket_, bool enabled_);
    ~socket_recv_source_rid_scope_t ();

  private:
    socket_base_t *_prev_socket;
    bool _prev_enabled;
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
    friend class socket_callback_scope_t;
#ifdef ZLINK_BUILD_TESTS
    friend class session_termination_test_access_t;
#endif

  public:
    //  Returns false if object is not a socket.
    bool check_tag () const;
    int socket_type () const;

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
    // Internal helper for logical multipart wrappers that already hold the
    // public send scope for the whole transaction.
    int send_scoped (zlink::msg_t *msg_,
                     int flags_,
                     socket_public_send_scope_t &scope_,
                     zlink::pipe_t **pipe_out_ = NULL);
    int send_routed (const zlink_routing_id_t *target_rid_, zlink::msg_t *msg_, int flags_);
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
                            uint64_t expected_transport_pair_generation_ = 0);
    int select_routed_submit_target (
      const zlink_routing_id_t *router_rid_or_null_,
      zlink_routed_submit_target_t *target_out_);
    bool transport_pair_application_ready (const zlink::pipe_t *pipe_) const;

    //  Socket-wide local receive-flow state (Core internal; the public API and
    //  its result mapping are a later step). RUNNING is 0 and PAUSED is 1.
    //  Returns 0 on success and -1 with errno set:
    //    EINVAL   state outside the enum
    //    ENOTSUP  socket type without a completion lane
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
    //  Classifies one completion-lane frame. Returns true when the frame was a
    //  Core-internal flow-state frame and has been consumed, whether or not it
    //  could be applied; the caller must then not pass it anywhere else.
    bool consume_receive_flow_state_frame (pipe_t *completion_pipe_,
                                           const zlink::msg_t &msg_);

#ifdef ZLINK_BUILD_TESTS
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
    bool test_set_pair_received_flow_state (uint64_t transport_pair_id_,
                                            uint64_t transport_pair_generation_,
                                            bool paused_);
    zlink::pipe_t *test_pair_pipe (uint64_t transport_pair_id_,
                                   uint64_t transport_pair_generation_,
                                   bool completion_lane_) const;
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
    //  Counts the writable edges published by releasing a transport-pair hold.
    //  A pair whose peer is already PAUSED must never produce one.
    bool test_pair_is_ready (uint64_t transport_pair_id_,
                             uint64_t transport_pair_generation_) const;
    //  Seeds and reads the socket-wide epoch, so the wraparound boundary is
    //  reachable without performing 2^64 state changes.
    void test_set_local_receive_flow_epoch (uint64_t epoch_);
    uint64_t test_local_receive_flow_epoch () const;
#endif
    std::unique_ptr<socket_public_send_scope_t> begin_public_send_scope (bool force_sync_);
    std::unique_ptr<socket_public_api_scope_t> begin_public_api_scope ();
    int rollback ();
    int rollback_scoped (socket_public_send_scope_t &scope_);
    int recv (zlink::msg_t *msg_, int flags_);
    int recv_pipe (zlink::msg_t *msg_, zlink::pipe_t **pipe_out_, int flags_);
    int recv_routed (zlink::msg_t *msg_,
                     zlink_routing_id_t *source_rid_out_,
                     int flags_,
                     uint64_t *connection_id_out_ = NULL,
                     zlink::pipe_t **source_pipe_out_ = NULL);
    //  These three return a pipe whose lifetime is PINNED: they take a
    //  lifetime ref while the transport-pair table is locked, because the
    //  table slot is the only thing that proves the pipe is still alive and
    //  the caller dereferences the result after the table is unlocked. Every
    //  caller must call release_lifetime_ref () after its last dereference,
    //  including on its error paths.
    pipe_t *completion_pipe_for_application (pipe_t *application_pipe_) const;
    pipe_t *application_pipe_for_completion (pipe_t *completion_pipe_) const;
    pipe_t *completion_pipe_for_peer (const zlink_routing_id_t *peer_rid_) const;
    //  NOT YET pinned, only because router_admission.cpp uses it as a plain
    //  predicate and would leak a pin. Its other caller,
    //  send_completion_frames_for_transport_pair (), DOES dereference the
    //  result after the table is unlocked - the same shape as the bug fixed
    //  above - so this accessor still needs the pin plus a matching release
    //  at the router_admission call site.
    pipe_t *completion_pipe_for_transport_pair (uint64_t transport_pair_id_,
                                                uint64_t transport_pair_generation_) const;
    void cache_completion_pipe_routing_id (pipe_t *application_pipe_);
    //  Request/reply submit entries write to transport pipes directly instead
    //  of going through send()/recv(). They still have to drain pending socket
    //  commands (throttled, exactly like the send() entry does); otherwise a
    //  backpressured completion pipe never observes the peer's activate-write
    //  command and every submit retry fails with EAGAIN.
    int process_submit_commands ();
    int close ();
    int close (int handoff_timeout_ms_);
    // Reserve close before the public wrapper tears down handlers or request
    // state. Returns 1 when close was deferred from a send-ready callback.
    int begin_close_handoff ();
    void complete_close_handoff ();
    int socket_msg_dispatch_from_io (zlink::msg_t *msg_, zlink::pipe_t *pipe_);
    int peer_command_from_io (zlink::msg_t *msg_, zlink::pipe_t *pipe_);
    int socket_msg_dispatch_stop ();
    void socket_msg_dispatch_drain_pending ();
    //  Asynchronous send admission (zlink_send_async family).
    int socket_set_send_complete_handler (zlink_send_complete_handler_fn handler_,
                                          void *userdata_);
    int send_async_submit (zlink_msg_t *parts_,
                           size_t part_count_,
                           const zlink_send_async_options_t *options_,
                           zlink_send_op_id_t *op_id_out_);
    int send_async_cancel (zlink_send_op_id_t op_id_);
    bool send_complete_handler_active () const;
    //  Admit whatever the current pipe state allows, then hand resolved
    //  completions to the callback. Called by the async mailbox loop and by
    //  the POLLCOMPLETION drain owner.
    void drive_send_pending ();
    int drain_send_completions ();
    bool has_send_pending () const;
    bool socket_msg_dispatch_active () const;
    int ensure_async_command_processing ();
    int ensure_completion_processing ();
    void acquire_completion_poller ();
    void release_completion_poller ();
    bool acquire_poller_registration ();
    void release_poller_registration ();

    //  True when the calling thread has taken ownership of this socket's
    //  completion processing, which is the only place a registered reply
    //  handler may run. Ownership is taken by a wait that asked for
    //  ZLINK_POLLCOMPLETION and by the async mailbox worker that stands in for
    //  a poller. Any other command drain records readiness instead of running
    //  user code, so a handler never runs re-entrantly inside an unrelated
    //  send, recv or option call.
    bool completion_drain_permitted () const;

    //  Delivers replies from every ready Completion pipe. Called from the
    //  owned drain points, because a pipe that was made readable while no
    //  owner was draining does not report readiness again.
    void process_ready_completion_pipes ();
    void resume_completion_processing_if_needed ();
    void notify_request_completion ();
    int drain_request_completions ();
    void acknowledge_request_completion_notification ();
    static socket_base_t *current_socket_msg_dispatch_socket ();
    static socket_base_t *current_send_complete_dispatch_socket ();
    static zlink::pipe_t *current_socket_msg_dispatch_pipe ();
    static void *current_socket_msg_dispatch_subject ();
    static bool current_socket_msg_dispatch_source_rid (zlink_routing_id_t *out_);
    bool recv_source_rid_capture_requested () const;
    int stream_dispatch_msg_from_io (zlink::msg_t *msg_, zlink::pipe_t *pipe_);
    virtual int sub_dispatch_start (sub_io_handler_fn callback_, void *userdata_);
    virtual int sub_dispatch_stop ();
    virtual bool sub_dispatch_active () const;
    virtual int xpub_dispatch_start ();
    virtual bool xpub_dispatch_active () const;
    virtual int stream_dispatch_start_raw (zlink_stream_on_raw_fn callback_);
    virtual int stream_set_msg_handler_with_userdata (zlink_socket_msg_handler_fn handler_,
                                                      void *userdata_);
    virtual int
    stream_set_packet_msg_handler_with_userdata (zlink_stream_packet_handler_fn handler_,
                                                 void *userdata_);
    virtual int stream_mark_raw_part_receive ();
    virtual int stream_dispatch_stop ();
    virtual bool stream_dispatch_active () const;
    virtual bool stream_dispatch_in_callback () const;
    virtual uint32_t stream_dispatch_inflight () const;
    virtual int stream_dispatch_send_from_io (const zlink_routing_id_t *rid_,
                                              const void *data_,
                                              size_t size_,
                                              int flags_);
    virtual int stream_dispatch_send_msg_from_io (const zlink_routing_id_t *rid_,
                                                  zlink::msg_t *msg_,
                                                  int flags_);
    virtual int stream_dispatch_send_current_msg_from_io (zlink::msg_t *msg_, int flags_);
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

    int monitor (const char *endpoint_,
                 uint64_t events_,
                 int event_version_,
                 int type_,
                 uint64_t monitor_hwm_bytes_);

    void event_connected (const endpoint_uri_pair_t &endpoint_uri_pair_, zlink::fd_t fd_);
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
                                const endpoint_uri_pair_t &endpoint_uri_pair_);
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
    bool has_request_reply_state () const;
    std::shared_ptr<socket_reqrep_internal::socket_request_reply_state_t>
    set_request_reply_state (
      const std::shared_ptr<socket_reqrep_internal::socket_request_reply_state_t> &state_);
    void clear_request_reply_state ();
    std::shared_ptr<part_helper_internal::handle_state_t> part_helper_state () const;
    bool has_part_helper_state () const;
    std::shared_ptr<part_helper_internal::handle_state_t>
    set_part_helper_state (const std::shared_ptr<part_helper_internal::handle_state_t> &state_);
    void clear_part_helper_state ();

    bool is_ctx_terminated () const;

    //  Re-evaluate a paired route after the transport engine installs its
    //  concrete endpoint and connection identity.
    void notify_transport_pair_ready (zlink::pipe_t *pipe_);

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
    virtual int xsend_pipe (
      zlink::msg_t *msg_, zlink::pipe_t **pipe_out_,
      pipe_message_admission_t *admission_out_ = NULL);
    virtual int xsend_routed (const zlink_routing_id_t *target_rid_,
                              zlink::msg_t *msg_,
                              uint64_t *connection_id_out_,
                              uint64_t expected_connection_id_,
                              zlink::pipe_t **pipe_out_,
                              uint64_t expected_transport_pair_id_ = 0,
                              uint64_t expected_transport_pair_generation_ = 0,
                              pipe_message_admission_t *admission_out_ = NULL);
    virtual int xselect_routed_submit_target (
      const zlink_routing_id_t *router_rid_or_null_,
      zlink_routed_submit_target_t *target_out_);
    virtual bool xsubmit_retry_allowed (const zlink_routing_id_t *target_rid_, int err_) const;
    virtual int xrollback ();

    //  The default implementation assumes that recv in not supported.
    virtual bool xhas_in ();
    virtual int xrecv (zlink::msg_t *msg_);
    virtual int xrecv_pipe (zlink::msg_t *msg_, zlink::pipe_t **pipe_out_);
    virtual int xrecv_routed (zlink::msg_t *msg_,
                              zlink_routing_id_t *source_rid_out_,
                              uint64_t *connection_id_out_,
                              zlink::pipe_t **source_pipe_out_ = NULL);
    virtual int xterm_peer_rid (const zlink_routing_id_t *peer_rid_);
    int xterm_transport_pair (uint64_t transport_pair_id_,
                              uint64_t transport_pair_generation_);
    virtual int xsocket_msg_dispatch (zlink::msg_t *msg_, zlink::pipe_t *pipe_);
    virtual int xstream_dispatch_msg (zlink::msg_t *msg_, zlink::pipe_t *pipe_);
    virtual int xpeer_command (zlink::msg_t *msg_, zlink::pipe_t *pipe_);
    virtual void xlocal_peer_weight_changed ();
    virtual int apply_peer_weight (zlink::pipe_t *pipe_, uint32_t weight_);
    virtual void xarm_socket_msg_dispatch ();
    virtual void xdispatch_io ();
    virtual uint32_t monitor_ready_count () const;

    //  i_pipe_events will be forwarded to these functions.
    virtual void xread_activated (pipe_t *pipe_);
    virtual void xwrite_activated (pipe_t *pipe_);
    virtual void xhiccuped (pipe_t *pipe_);
    virtual void xpipe_terminated (pipe_t *pipe_) = 0;

    //  the default implementation assumes that joub and leave are not supported.
    virtual int xjoin (const char *group_);
    virtual int xleave (const char *group_);

    void invoke_socket_msg_handler (zlink_socket_msg_handler_fn handler_,
                                    const zlink_routing_id_t *source_rid_,
                                    zlink_msg_t *parts_,
                                    size_t part_count_);
    static void store_socket_msg_part (std::vector<zlink_msg_t> *parts_,
                                       zlink::msg_t *msg_);
    static int init_peer_weight_command (zlink::msg_t *msg_, uint32_t weight_);
    static bool decode_peer_weight_command (const zlink::msg_t &msg_, uint32_t *weight_out_);
    void broadcast_local_peer_weight ();
    void send_local_peer_weight (pipe_t *pipe_);

    //  Completion-lane flow state. The frame is Core internal: it is written on
    //  the completion lane as a command frame and consumed by the peer's Core,
    //  so no application or Framework receive path can observe it.
    void write_receive_flow_state_frame (pipe_t *completion_pipe_,
                                         unsigned char state_,
                                         uint64_t epoch_);
    void sync_local_receive_flow_state_to_pair (pipe_t *completion_pipe_);

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
    void stop_async_mailbox_processing ();
    void wait_async_quiesced (int timeout_ms_);
    zlink_socket_msg_handler_fn socket_msg_handler () const;
    static void close_socket_msg_parts (std::vector<zlink_msg_t> *parts_);
    static void resolve_socket_msg_source_rid (pipe_t *pipe_, zlink_routing_id_t *out_);

  public:
    void store_last_recv_source_rid (pipe_t *pipe_);
    void store_last_recv_source_rid (const zlink_routing_id_t *source_rid_);
    void clear_last_recv_source_rid ();
    bool copy_last_recv_source_rid (zlink_routing_id_t *out_) const;
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
                     uint64_t *connection_id_out_);
    typedef std::unique_lock<std::recursive_mutex> socket_msg_dispatch_lock_t;
    socket_msg_dispatch_lock_t lock_socket_msg_dispatch ()
    {
        return socket_msg_dispatch_lock_t (dispatch_runtime ().socket_msg_dispatch_sync);
    }
    socket_msg_dispatch_lock_t lock_socket_msg_dispatch_if_active ()
    {
        socket_msg_dispatch_lock_t lock (
          dispatch_runtime ().socket_msg_dispatch_sync, std::defer_lock);
        if (socket_msg_dispatch_active ())
            lock.lock ();
        return lock;
    }
    //  A pipe became writable again: nudge the admit loop for that target.
    void notify_send_pending_writable (pipe_t *pipe_);
    //  A route ended: fail every pending record bound to that exact target.
    void fail_send_pending_for_pipe (pipe_t *pipe_, int terminal_errno_);
    void fail_send_pending_for_target (const zlink_routing_id_t *peer_rid_,
                                       uint64_t transport_pair_id_,
                                       uint64_t transport_pair_generation_,
                                       int terminal_errno_);
    void emit_socket_monitor_value_event (uint64_t event_,
                                          uint64_t value_,
                                          const endpoint_uri_pair_t &endpoint_uri_pair_);
    void emit_peer_weight_changed (pipe_t *pipe_, uint32_t weight_);
    void snapshot_attached_pipes (std::vector<pipe_t *> *out_);
    bool has_attached_pipes () const;

  private:
    friend class ctx_t;
    friend class socket_poller_t;
    friend struct multipart_send_facade_t;

    int get_events_for_poller (int events_, uint32_t *out_);

    // Direct public send currently shares one scope between single-part and
    // logical multipart wrappers. Keep the admission/sync decision and the
    // blocking retry runner behind one internal boundary so future structural
    // candidates can change them independently.
    bool direct_send_needs_public_api_sync () const;
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
                                bool record_context_admission_ = true);

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


    void *socket_msg_handler_subject () const;
    void *socket_msg_handler_userdata () const;


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
      const std::shared_ptr<transport_pair_state_t> &pair_state_);

    //  To be called after processing commands or invoking any command
    //  handlers explicitly. If required, it will deallocate the socket.
    void check_destroy ();

    //  Moves the flags from the message to local variables,
    //  to be later retrieved by getsockopt.
    void extract_flags (const msg_t *msg_);

    //  Used to check whether the object is a socket.
    uint32_t _tag;

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
    //  in a predefined time period.
    int process_commands (int timeout_, bool throttle_);
    void inc_mailbox_ref ();
    void dec_mailbox_ref ();
    void finalize_destroy ();
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
  private:
#endif
    //  close / ctx term: fail every pending record fast. LINGER does not
    //  apply - it covers bytes already admitted, and a pending record is by
    //  definition not admitted yet.
    void fail_all_send_pending (int terminal_errno_);
    void dispatch_send_completions (bool closing_ = false);
    void dispatch_send_completions_if_local ();
    //  Claim/finish helpers used by the admit loop.
    bool claim_send_pending_head (send_pending_record_t **out_);
    int try_admit_send_pending (send_pending_record_t *record_);
    void finish_send_pending (send_pending_record_t *record_,
                              zlink_send_complete_result_t result_,
                              int terminal_errno_);
    void destroy_send_pending_record (send_pending_record_t *record_);
    void on_send_pending_deadline (zlink_send_op_id_t op_id_);
    static void send_pending_deadline_trampoline (void *userdata_);
    static void reaper_mailbox_handler (void *arg_);
    static void reaper_mailbox_pre_post (void *arg_);
    static void async_mailbox_handler (void *arg_);
    static void async_mailbox_pre_post (void *arg_);
    static socket_base_t *current_async_mailbox_dispatch_socket ();
    void defer_close_handoff_from_async_owner ();
    void finish_deferred_close_after_async_quiesced ();

    //  Handlers for incoming commands.
    void process_stop () ZLINK_FINAL;
    void process_bind (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void process_term (int linger_) ZLINK_FINAL;
    void process_term_endpoint (std::string *endpoint_) ZLINK_FINAL;

    void refresh_attached_pipe_hwms ();
    void update_pipe_options (int option_);

    std::string resolve_tcp_addr (std::string endpoint_uri_, const char *tcp_address_);
    // A normal public close cannot reap the socket while its asynchronous
    // mailbox owner still holds a scheduled callback. Callback-initiated
    // close uses the separate deferred handoff path and does not wait here.
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
    //  transition. It is recursive because a reply callback may re-enter a
    //  poller registration API on the same socket.
    mutable mutex_t _completion_owner_sync;
    std::atomic<uint32_t> _completion_poller_refs;
    std::atomic<bool> _request_completion_pending;
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
    uint32_t _local_peer_weight;
    typedef std::pair<uint64_t, uint64_t> transport_pair_key_t;
    typedef std::map<transport_pair_key_t, transport_pair_pipes_t> transport_pairs_t;
    //  Owner of the transport pair table. Pair admission, readiness, teardown
    //  and Completion pipe consumption all run under this mutex; the pipes
    //  themselves are drained outside it so a reply handler never runs with the
    //  table locked.
    mutable mutex_t _transport_pairs_sync;
    transport_pairs_t _transport_pairs;
    std::deque<transport_pair_key_t> _ready_completion_pairs;
    std::set<transport_pair_key_t> _ready_completion_pair_set;
    //  Socket-wide local receive-flow state, guarded by _transport_pairs_sync
    //  so pair fanout and new-pair synchronisation serialise against the same
    //  state. The epoch advances only on a real state change; a repeated call
    //  with the same state succeeds without emitting anything.
    unsigned char _local_receive_flow_state;
    uint64_t _local_receive_flow_epoch;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (socket_base_t)
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

    void add_out_pipe (blob_t routing_id_, pipe_t *pipe_, bool locally_initiated_);
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
