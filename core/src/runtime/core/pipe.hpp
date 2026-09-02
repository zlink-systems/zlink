/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_PIPE_HPP_INCLUDED__
#define __ZLINK_PIPE_HPP_INCLUDED__

#include <atomic>
#include <memory>

#include "core/auto_hwm_policy.hpp"
#include "core/ctx_physical_queue_registry.hpp"
#include "core/ypipe_base.hpp"
#include "utils/config.hpp"
#include "core/object.hpp"
#include "utils/stdint.hpp"
#include "utils/array.hpp"
#include "utils/blob.hpp"
#include "core/options.hpp"
#include "core/endpoint.hpp"
#include "core/msg.hpp"
#include "core/pipe_stream_packet_state.hpp"
#include "utils/fast_mutex.hpp"
#include "utils/macros.hpp"

namespace zlink
{
class pipe_t;
enum pipe_write_observer_phase_t
{
    pipe_write_observer_prepare,
    pipe_write_observer_commit,
    pipe_write_observer_finish
};
typedef bool (*pipe_write_observer_fn) (
  pipe_t *pipe_, void *userdata_, pipe_write_observer_phase_t phase_);

enum pipe_write_status_t
{
    pipe_write_ready = 0,
    pipe_write_hwm_full,
    pipe_write_transport_wait,
    pipe_write_inactive
};

enum pipe_message_admission_t : int
{
    pipe_message_admission_ready = 0,
    pipe_message_admission_hwm_full,
    pipe_message_admission_request_full,
    pipe_message_admission_transport_wait,
    pipe_message_admission_too_large,
    pipe_message_admission_inactive,
    pipe_message_admission_invalid
};

//  Normalized kind of the first queued application frame. This deliberately
//  separates an empty queue from malformed request/reply metadata so callers
//  can route public receive and socket-local completion work without removing
//  the frame that owns the decision.
enum pipe_normalized_head_kind_t
{
    pipe_head_empty = 0,
    pipe_head_data,
    pipe_head_request,
    pipe_head_reply,
    pipe_head_error_reply,
    pipe_head_control,
    pipe_head_invalid
};

//  Reports whether applying a remote receive-flow state actually changed the
//  pipe's own paused/running record. A stale or duplicate frame that
//  apply_remote_flow_state() already rejected reports no_transition, so the
//  observation layer never sees an event for a frame that changed nothing.
enum flow_state_transition_t
{
    flow_state_no_transition = 0,
    flow_state_transition_paused,
    flow_state_transition_resumed
};

struct transport_lifetime_t
{
    transport_lifetime_t (uint64_t connection_id_,
                          uint64_t route_incarnation_id_) :
        connection_id (connection_id_),
        route_incarnation_id (route_incarnation_id_),
        stream_routing_id (0),
        stream_route_closed (false),
        stream_connect_event_emitted (false)
    {
    }
    std::atomic<uint64_t> connection_id;
    const uint64_t route_incarnation_id;
    std::atomic<uint32_t> stream_routing_id;
    std::atomic<bool> stream_route_closed;
    std::atomic<bool> stream_connect_event_emitted;
    // Rare physical-transport generation changes and STREAM parser/callback
    // publication share this gate. Hot connection-id reads remain atomic.
    fast_mutex_t transport_sync;
    pipe_stream_packet_state_t stream_packet_state;
};

//  Create a pipepair for bi-directional transfer of messages.
//  First HWM is for messages passed from first pipe to the second pipe.
//  Second HWM is for messages passed from second pipe to the first pipe.
//  Delay specifies how the pipe behaves when the peer terminates. If true
//  pipe receives all the pending messages before terminating, otherwise it
//  terminates straight away.
//  If conflate is true, only the most recently arrived message could be
//  read (older messages are discarded)
//  If session_pipe is true the ypipes use the smaller
//  session_pipe_granularity chunk (per-connection pipes); otherwise the
//  default message_pipe_granularity is used.
int pipepair (zlink::object_t *parents_[2],
              zlink::pipe_t *pipes_[2],
              const uint64_t hwms_[2],
              const bool conflate_[2],
              bool session_pipe_ = false,
              transport_lane_t lane_ = transport_lane_application,
              auto_hwm_role_t role_ = auto_hwm_role_none,
              bool planning_enabled_ = false,
              physical_queue_class_t queue_class_ =
                physical_queue_class_application,
              int session_owner_index_ = -1);

struct i_pipe_events
{
    virtual ~i_pipe_events () ZLINK_DEFAULT;

    virtual void read_activated (zlink::pipe_t *pipe_) = 0;
    virtual void write_activated (zlink::pipe_t *pipe_) = 0;
    virtual void hiccuped (zlink::pipe_t *pipe_) = 0;
    virtual void pipe_peer_terminated (zlink::pipe_t *pipe_) = 0;
    virtual void pipe_terminated (zlink::pipe_t *pipe_) = 0;

    //  Reports a real PAUSED/RUNNING transition applied to this pipe's remote
    //  receive-flow record (never a stale or duplicate frame, and never a
    //  repeat of the current state). Called synchronously on this pipe's own
    //  thread right after the mutation, from pipe_t::process_flow_state().
    //  Only socket_base_t overrides this; every other sink keeps the default
    //  no-op because only socket-owned application pipes carry flow state.
    virtual void flow_state_applied (zlink::pipe_t *pipe_, bool paused_,
                                     uint64_t epoch_, bool actual_writable_)
    {
        LIBZLINK_UNUSED (pipe_);
        LIBZLINK_UNUSED (paused_);
        LIBZLINK_UNUSED (epoch_);
        LIBZLINK_UNUSED (actual_writable_);
    }

    //  Delivers a FLOWSTATE owner command that arrived on an inproc count-2
    //  Completion pipe. Socket sinks validate that exact source against the
    //  ready pair, then forward the state to the pair's Application pipe;
    //  other pipe owners ignore it.
    virtual void flow_state_received (zlink::pipe_t *source_pipe_,
                                      unsigned char state_, uint64_t epoch_)
    {
        LIBZLINK_UNUSED (source_pipe_);
        LIBZLINK_UNUSED (state_);
        LIBZLINK_UNUSED (epoch_);
    }

    //  Delivers one peer-weight control command on the destination pipe's
    //  owner thread. Socket sinks validate pair identity before mutating their
    //  scheduler; other pipe owners ignore this control.
    virtual void peer_weight_received (zlink::pipe_t *pipe_, uint32_t weight_)
    {
        LIBZLINK_UNUSED (pipe_);
        LIBZLINK_UNUSED (weight_);
    }
};

//  Note that pipe can be stored in three different arrays.
//  The array of inbound pipes (1), the array of outbound pipes (2) and
//  the generic array of pipes to be deallocated (3).

class pipe_t ZLINK_FINAL : public object_t,
                           public array_item_t<1>,
                           public array_item_t<2>,
                           public array_item_t<3>
{
    template <typename T> friend void release_heap_owned (T *);
#ifdef ZLINK_BUILD_TESTS
    friend class session_termination_test_access_t;
#endif

    //  This allows pipepair to create pipe objects.
    friend int pipepair (zlink::object_t *parents_[2],
                         zlink::pipe_t *pipes_[2],
                         const uint64_t hwms_[2],
                         const bool conflate_[2],
                         bool session_pipe_,
                         transport_lane_t lane_,
                         auto_hwm_role_t role_,
                         bool planning_enabled_,
                         physical_queue_class_t queue_class_,
                         int session_owner_index_);

  public:
    typedef pipe_stream_packet_state_t stream_packet_state_t;

    //  Specifies the object to send events to.
    void set_event_sink (i_pipe_events *sink_);

    //  Pipe endpoint can store an routing ID to be used by its clients.
    void set_server_socket_routing_id (uint32_t server_socket_routing_id_);
    uint32_t get_server_socket_routing_id () const;

    //  Pipe endpoint can store an opaque ID to be used by its clients.
    void set_router_socket_routing_id (const blob_t &router_socket_routing_id_);
    //  Copies the routing ID while holding the cold publication lock. Use this
    //  only from foreign-thread control/error paths; socket-owner hot paths
    //  retain the zero-overhead get_routing_id() view.
    void snapshot_routing_id (blob_t *routing_id_) const;
    const blob_t &get_routing_id () const;
    pipe_t *get_peer () const;
    //  Returns a lifetime-pinned peer snapshot for control/monitor paths. The
    //  caller must release_lifetime_ref() after its last dereference.
    pipe_t *retain_peer_snapshot () const;
    bool is_session_pipe () const;
    void set_peer_routing_id (const unsigned char *data_, size_t size_);
    void set_peer_socket_type (int socket_type_);
    int get_peer_socket_type () const;
    void set_transport_peer_identity (const unsigned char *data_, size_t size_);
    const blob_t &get_transport_peer_identity () const;
    uint64_t get_msgs_written () const;
    uint64_t get_msgs_read () const;
    uint64_t get_bytes_written () const;
    uint64_t get_bytes_read () const;
    void record_peer_weight (uint64_t connection_id_, uint32_t weight_);
    bool record_peer_weight_if_current (uint64_t connection_id_,
                                        uint32_t weight_);
    bool peer_weight (uint32_t *weight_out_,
                      uint64_t *connection_id_out_ = NULL) const;
    uint64_t get_snd_pending_msgs () const;
    uint64_t get_rcv_pending_msgs_approx () const;
    uint64_t get_snd_pending_bytes () const;
    uint64_t get_rcv_pending_bytes_approx () const;
    uint64_t get_snd_queue_accounted_bytes () const;
    uint64_t get_rcv_queue_accounted_bytes () const;
    const std::shared_ptr<physical_queue_record_t> &in_physical_queue () const;
    const std::shared_ptr<physical_queue_record_t> &out_physical_queue () const;
    uint64_t planned_out_hwm () const;
    uint64_t applied_out_hwm () const;
    // Request correlation remains live after the application queue releases
    // its byte credit. Keep that second lifetime bounded per physical route so
    // a fast request lane cannot indefinitely outrun its completion lane.
    bool try_reserve_request_correlation (uint64_t accounted_bytes_);
    void release_request_correlation (uint64_t accounted_bytes_);
    static uint64_t frame_accounted_bytes (const msg_t *msg_);
    uint64_t planned_in_hwm () const;
    uint64_t applied_in_hwm () const;
    void apply_physical_queue_hwm_plan ();
    uint64_t get_oversize_message_admission_count () const;
    uint64_t get_oversize_message_admission_max_bytes () const;
    void reset_oversize_message_admission_metrics ();
    uint64_t get_connected_time () const;
    void refresh_write_credit (uint64_t peer_msgs_read_, uint64_t peer_bytes_read_);
    bool mark_stream_connect_event_emitted ();
    void reset_stream_connect_event_emitted ();
    bool mark_connection_ready_event_emitted ();
    void reset_connection_ready_event_emitted ();
    stream_packet_state_t &stream_packet_state ();
    const stream_packet_state_t &stream_packet_state () const;
    fast_mutex_t &transport_sync ();
    void reset_stream_packet_state ();
    void close_stream_route ();
    bool stream_route_closed () const;

    //  Returns true if there is at least one message to read in the pipe.
    bool check_read ();

    //  Classifies the first queued frame without consuming it. Untyped frames
    //  are DATA. Typed frames require a recognized request/reply kind and a
    //  nonzero request sequence; malformed metadata is INVALID.
    pipe_normalized_head_kind_t probe_normalized_head_kind ();

    //  Reads a message to the underlying pipe.
    bool read (msg_t *msg_);
    typedef int (read_admission_fn) (pipe_t *pipe_, const msg_t &msg_,
                                     void *userdata_);
    enum
    {
        read_admission_reject_consume = -2
    };
    //  Returns true when the frame starts a record that must acquire receive
    //  admission before it leaves the queue. Raw terminal and private
    //  bookkeeping frames remain on the ordinary dequeue path.
    static bool requires_record_admission (const msg_t &msg_);
    //  Evaluates whole-record admission before removing metadata-bearing or
    //  multipart frames. A capacity rejection leaves the first frame queued.
    bool read_with_record_admission (
      msg_t *msg_, read_admission_fn *admission_, void *userdata_,
      bool *admission_failed_out_, bool *admission_consumed_out_);
    //  Probes the queued head without consuming it. When the head starts a
    //  record, admission_ decides whether that record is currently
    //  receivable. This is used by level readiness and fair-queue skipping so
    //  a capacity-blocked REQUEST cannot make POLLIN lie or stall other pipes.
    bool check_read_with_record_admission (
      read_admission_fn *admission_, void *userdata_,
      bool *admission_failed_out_);
    int reserve_inbound_decoder_frame (
      uint64_t payload_bytes_, unsigned char msg_flags_, bool track_multipart_,
      decoder_frame_reservation_t *reservation_storage_,
      decoder_frame_reservation_t **reservation_out_);
    int write_reserved_decoder_frame (
      msg_t *msg_, decoder_frame_reservation_t **reservation_);
    void release_decoder_frame_reservation (
      decoder_frame_reservation_t **reservation_);
    void finish_direct_decoder_frame (unsigned char msg_flags_);

    //  Checks whether messages can be written to the pipe. If the pipe is
    //  closed or if writing the message would cause high watermark the
    //  function returns false.
    bool check_write ();

    //  Checks whether messages can be written to the pipe and reports whether
    //  failure was caused by HWM or inactive pipe state.
    pipe_write_status_t check_write_status ();
    pipe_message_admission_t check_write_admission ();

    //  Consumes the HWM-credit wake marker. Transport-pair activation also
    //  produces write activation, but must not be reported as credit recovery.
    bool take_hwm_credit_recovery ();
    //  Consumes the request-correlation wake marker. Correlation capacity can
    //  return after an unrelated ordinary send cleared the socket-wide send
    //  recovery flag, so the pipe republishes that distinct cause.
    bool take_request_correlation_recovery ();

    // Paired transports expose the Application route before both physical
    // lanes finish their handshake so a blocking send can wait on EAGAIN.
    // The existing outbound-active flag keeps those writes out of the pipe
    // until the pair is validated.
    void hold_writes_until_transport_pair_ready ();
    bool release_writes_for_transport_pair ();
    bool transport_pair_writes_released () const;

    //  Remote receive-flow state, carried by the paired completion lane. It is
    //  an independent send blocker: it never touches the byte HWM counters and
    //  clearing it only removes its own cause. A PAUSE that arrives while a
    //  multipart message is already started applies from the next message, so
    //  the started message keeps its existing atomicity.
    //  Admits the beginning of an owner-held message and records it as started
    //  while the outbound-state lock is held. A classic ROUTER consumes the
    //  routing-ID part without writing it, so the outbound byte counters alone
    //  cannot tell that a message is in progress. The record clears itself when
    //  the message commits, is rolled back or is discarded by a hiccup.
    pipe_message_admission_t admit_owner_message_start ();
    //  Writes the next part of an owner-started message. The first part reuses
    //  the readiness decision made by admit_owner_message_start(); its actual
    //  byte-HWM admission is still checked while the part is recorded.
    bool write_owner_started_message (
      const msg_t *msg_, pipe_message_admission_t *admission_out_ = NULL);
    //  Request correlation must be visible after this exact candidate has
    //  accepted the message but before the first application frame is made
    //  observable. The prepare phase owns the request-state lock before the
    //  pipe lock is acquired; commit and the optional final flush then happen
    //  under that same pipe lock so termination cannot pass between them.
    bool write_owner_started_message_observed (
      const msg_t *msg_, pipe_write_observer_fn observer_,
      void *observer_userdata_,
      pipe_message_admission_t *admission_out_ = NULL);
    //  Applies an absolute remote state on the pipe's own thread and reports
    //  whether the caller must publish the write-activated edge. Callers that
    //  already run on that thread use this directly, so the state is in effect
    //  before any admission transition they make next.
    //  out_transition_ reports whether this call actually flipped the pipe's
    //  paused/running record (as opposed to a stale or duplicate epoch, or a
    //  repeat of the same state). out_actual_writable_ is sampled under the
    //  same lock right after the mutation, so a PAUSED->RUNNING transition
    //  reports whether every other send-blocker cause is already clear too.
    bool apply_remote_flow_state (unsigned char state_, uint64_t epoch_,
                                  flow_state_transition_t *out_transition_ = NULL,
                                  bool *out_actual_writable_ = NULL);
    bool remote_flow_paused () const;
    //  Timestamp (ms) at which this pipe's remote flow state most recently
    //  became PAUSED. Set and read only from the socket-owning thread that
    //  processes flow_state commands, so it needs no lock of its own.
    void set_remote_flow_pause_started_ms (uint64_t ms_);
    uint64_t remote_flow_pause_started_ms () const;
    //  Whether the remote state blocks the next message on this pipe. Unlike
    //  remote_flow_paused () this honours the in-progress message exception, so
    //  readiness predicates agree with send admission.
    bool remote_flow_blocks_next_message () const;
#ifdef ZLINK_BUILD_TESTS
    //  Test-only windows onto the pure byte-charge and LWM arithmetic, so the
    //  exact contract can be asserted without standing up a transport.
    static uint64_t test_frame_accounted_bytes (const msg_t *msg_);
    static uint64_t test_compute_lwm (uint64_t hwm_);
    static uint64_t test_apply_lwm_hint (uint64_t hwm_,
                                         uint64_t lwm_,
                                         uint64_t lwm_hint_);
    //  Test-only: reports the send-blocker causes without evaluating any of
    //  them, so observing the pipe cannot change it.
    void test_flow_probe (bool *out_active_,
                          bool *hwm_full_,
                          bool *remote_paused_,
                          bool *byte_credit_waiter_ = NULL,
                          uint64_t *in_flight_bytes_ = NULL) const;
#endif
    //  Consumes the remote-pause wake marker, mirroring the HWM-credit marker
    //  so a resume publishes exactly one routed send-ready edge.
    bool take_flow_resume_recovery ();

    //  Writes a message to the underlying pipe. Returns false if the
    //  message does not pass check_write. If false, the message object
    //  retains ownership of its message buffer.
    bool write (const msg_t *msg_,
                pipe_message_admission_t *admission_out_ = NULL);

    //  Writes a message assuming HWM was already checked by caller.
    //  Still validates pipe active/termination state.
    bool write_no_hwm_check (const msg_t *msg_);

    // Writes the transport's routing-id setup frame even while a paired
    // Application lane is held for Completion-lane validation.
    bool write_routing_id_and_flush (const msg_t *msg_);
    bool write_transport_probe_and_flush (const msg_t *msg_);

    //  Accepts the latest absolute peer-weight command on an Application
    //  session pipe after pair admission. It writes immediately at a message
    //  boundary, or stages the weight behind an open multipart and appends it
    //  only after final commit/rollback. Policy bypasses Application HWM and
    //  remote PAUSE, but never the pair hold or ypipe commit boundary.
    bool write_peer_weight_control_and_flush (uint32_t weight_,
                                              bool defer_flush_ = false);

    //  FLOWSTATE uses the topology-selected control connection: Application
    //  for count 1 and Completion for count 2. Keep its latest absolute value
    //  in a slot independent from WEIGHT, then publish surviving controls in
    //  enqueue order at the next record boundary. The control bypasses HWM and
    //  remote PAUSE, but never inactive state, the initial pair hold, or an
    //  open multipart.
    bool write_flow_state_control_and_flush (unsigned char state_,
                                             uint64_t epoch_,
                                             bool defer_flush_ = false);
    // Publishes controls deliberately staged above pipe-local multipart state
    // once the socket-level public multipart lease has ended.
    bool flush_pending_peer_controls ();

    //  Writes a message and flushes it downstream under the same pipe lock.
    //  Use this for final single-part send hot paths to avoid paying for
    //  separate write/flush lock acquisitions.
    bool write_and_flush (const msg_t *msg_,
                          pipe_message_admission_t *admission_out_ = NULL);

    //  Writes a message with the HWM check performed under the already-held
    //  pipe lock without re-entering check_hwm().
    bool write_no_recursive_hwm_check (
      const msg_t *msg_, pipe_message_admission_t *admission_out_ = NULL);

    //  Writes and flushes with the same non-recursive HWM check variant.
    bool write_and_flush_no_recursive_hwm_check (
      const msg_t *msg_, pipe_message_admission_t *admission_out_ = NULL);

    //  Fast path for a single non-routing-id message that is always flushed.
    bool write_single_message_and_flush_no_recursive_hwm_check (
      const msg_t *msg_, pipe_message_admission_t *admission_out_ = NULL);
    bool write_message_observed (
      const msg_t *msg_, pipe_write_observer_fn observer_,
      void *observer_userdata_,
      pipe_message_admission_t *admission_out_ = NULL);


    //  Remove unfinished parts of the outbound message from the pipe.
    void rollback ();

    //  Remove an unfinished outbound message, if one exists. Returns true
    //  only when a multipart prefix was actually discarded. This keeps the
    //  normal single-message path free of an owner-side multipart flag while
    //  still allowing a failed continuation to be reported as an atomic
    //  record abort.
    bool rollback_incomplete ();

    //  Flush the messages downstream.
    void flush ();

    //  Temporarily disconnects the inbound message stream and drops
    //  all the messages on the fly. Causes 'hiccuped' event to be generated
    //  in the peer.
    void hiccup ();

    //  Ensure the pipe won't block on receiving pipe_term.
    void set_nodelay ();

    //  Ask pipe to terminate. The termination will happen asynchronously
    //  and user will be notified about actual deallocation by 'terminated'
    //  event. If delay is true, the pending messages will be processed
    //  before actual shutdown.
    void terminate (bool delay_);

    //  Set the high water marks.
    void set_hwms (uint64_t inhwm_, uint64_t outhwm_);
    void set_lwm_hint (uint64_t lwm_hint_);

    //  Set the boost to high water marks, used by inproc sockets so total hwm are sum of connect and bind sockets watermarks

    //  Payload bound for a complete message, or 0 when the reader has no
    //  finite bound. The empty-pipe oversize exception is enabled only when
    //  this value is finite.
    void set_max_message_bytes (uint64_t max_message_bytes_);

    // send command to peer for notify the change of hwm
    void send_hwms_to_peer (uint64_t inhwm_, uint64_t outhwm_);

    //  Returns true if HWM is not reached
    bool check_hwm () const;
    //  Checks whether the current multipart transaction can commit with this
    //  frame. A rejected final frame makes the pipe wait for byte credit.
    pipe_message_admission_t check_hwm_for_message (const msg_t *msg_);

    void set_endpoint_pair (endpoint_uri_pair_t endpoint_pair_);
    const endpoint_uri_pair_t &get_endpoint_pair () const;
    void set_transport_connection_id (uint64_t connection_id_);
    uint64_t get_transport_connection_id () const;
    // Claims the one physical DISCONNECTED monitor edge owned by this socket
    // endpoint. The transport error path and explicit local termination can
    // race; only the winner publishes the event.
    bool try_claim_transport_disconnected_event ();
    uint64_t get_route_incarnation_id () const;
    void set_transport_pair (transport_lane_t lane_,
                             uint64_t pair_id_,
                             uint64_t generation_);
    void set_transport_lane_count (unsigned char lane_count_);
    transport_lane_t get_transport_lane () const;
    unsigned char get_transport_lane_count () const;
    //  Socket-published mirror of "this pipe is the Application lane of a
    //  ready transport pair". Written by the socket under its pair-table
    //  mutex at admission and detach; read lock-free on the send/recv hot
    //  path so per-message routing does not take that mutex.
    void set_transport_pair_application_ready (bool ready_);
    bool transport_pair_application_ready_cached () const;
    bool uses_registry_accounting () const;
    uint64_t get_transport_pair_id () const;
    uint64_t get_transport_pair_generation () const;
    void set_locally_initiated (bool value_);
    bool is_locally_initiated () const;

    void send_disconnect_msg ();
    void set_disconnect_msg (const std::vector<unsigned char> &disconnect_);

    void send_hiccup_msg (const std::vector<unsigned char> &hiccup_);

    bool retain_lifetime_ref ();
    void release_lifetime_ref ();
    bool has_completed_termination () const;
    //  Completion-lane drains can run outside the socket receive mutex. They
    //  retain the inbound queue separately so process_pipe_term_ack() can
    //  report socket termination immediately while deferring queue deletion
    //  until the last in-flight reader exits.
    bool retain_inbound_read_ref ();
    void release_inbound_read_ref ();
    //  A request/reply target may be published only while the application
    //  pipe is still active. Callers hold a lifetime ref while checking this.
    bool is_lifecycle_active () const;

  private:
    friend class ctx_physical_queue_registry_t;
    friend class socket_base_t;

    //  Type of the underlying lock-free pipe.
    typedef ypipe_base_t<msg_t> upipe_t;

    //  Command handlers.
    void process_flow_state (unsigned char state_,
                             uint64_t epoch_) ZLINK_OVERRIDE;
    void process_peer_weight (uint32_t weight_,
                              uint64_t connection_id_) ZLINK_OVERRIDE;
    void process_activate_read () ZLINK_OVERRIDE;
    void process_activate_write (uint64_t generation_,
                                 uint64_t msgs_read_,
                                 uint64_t bytes_read_) ZLINK_OVERRIDE;
    void process_hiccup (void *pipe_, uint64_t generation_) ZLINK_OVERRIDE;
    void process_pipe_term () ZLINK_OVERRIDE;
    void process_pipe_term_ack () ZLINK_OVERRIDE;
    void process_pipe_hwm (uint64_t inhwm_, uint64_t outhwm_) ZLINK_OVERRIDE;

    //  Handler for delimiter read from the pipe.
    void process_delimiter ();

    //  These helpers require `_out_sync` to be held already. They define the
    //  coupled outbound invariants that future `_out_sync` refactors must
    //  preserve: `_out_pipe` lifetime, `_state`, `_out_active`, and
    //  `_peers_msgs_read` move together across write/flush/terminate paths.
    //  enforce_incremental_hwm_ rejects a multipart as soon as the frames
    //  accumulated so far exceed the HWM, instead of waiting for the final
    //  frame. Only writers that check the HWM per call may ask for it: the
    //  *_no_recursive_hwm_check writers admit a whole message up front, so
    //  rejecting one of its later frames would break that guarantee and make
    //  the caller drop a message it was told it could send.
    bool write_message_unlocked (const msg_t *msg_,
                                 bool enforce_hwm_,
                                 bool enforce_incremental_hwm_ = false,
                                 pipe_message_admission_t *admission_out_ = NULL);
    pipe_message_admission_t write_state_admission_unlocked () const;
    bool remote_flow_blocked_unlocked () const;
    bool write_state_ready_unlocked (
      pipe_message_admission_t *admission_out_) const;
    bool hwm_credit_ready_unlocked (
      pipe_message_admission_t *admission_out_);
    void rollback_unlocked (bool publish_peer_control_ = true);
    void flush_unlocked ();
    static uint64_t committed_frame_accounted_bytes_ref (const msg_t &msg_);
    static bool counted_pending_message_ref (const msg_t &msg_);
    void publish_outbound_frame_unlocked (const msg_t &msg_, bool more_);
    void release_discarded_pipe_accounting (upipe_t *pipe_,
                                            const std::shared_ptr<physical_queue_record_t> &queue_);
    bool append_outbound_frame_bytes_unlocked (const msg_t *msg_);
    bool peer_control_slots_enabled_unlocked () const;
    uint64_t next_peer_control_sequence_unlocked ();
    bool stage_peer_weight_control_unlocked (uint32_t weight_);
    bool stage_flow_state_control_unlocked (unsigned char state_,
                                            uint64_t epoch_);
    bool append_pending_peer_controls_unlocked ();
    bool dispatch_pending_inproc_controls_unlocked ();
    bool flush_pending_peer_controls_unlocked ();
    bool pending_peer_controls_unlocked () const;
    void discard_pending_peer_controls_unlocked ();
    bool can_commit_bytes_unlocked (uint64_t message_bytes_,
                                    uint64_t payload_bytes_,
                                    bool allow_empty_pipe_exception_) const;
    bool can_commit_bytes_with_peer_snapshot_unlocked (
      uint64_t message_bytes_,
      uint64_t payload_bytes_,
      bool allow_empty_pipe_exception_);
    bool check_hwm_with_peer_snapshot_unlocked ();
    void refresh_peer_credit_snapshot_unlocked ();
    pipe_t *retain_peer_snapshot_unlocked () const;
    void account_inbound_frame (const msg_t *msg_);
    void snapshot_outbound_queue_accounting (const pipe_t *reader_,
                                             uint64_t *provisional_out_,
                                             uint64_t *committed_out_) const;
    void publish_session_outbound_accounting_unlocked (
      bool provisional_changed_);
    template <bool WithAdmission>
    bool read_internal (msg_t *msg_, read_admission_fn *admission_,
                        void *userdata_, bool *admission_failed_out_,
                        bool *admission_consumed_out_);
    void refresh_inbound_lwm_from_physical_queue ();

    //  Constructor is private. Pipe can only be created using
    //  pipepair function.
    pipe_t (object_t *parent_,
            upipe_t *inpipe_,
            upipe_t *outpipe_,
            uint64_t inhwm_,
            uint64_t outhwm_,
            bool conflate_,
            bool session_pipe_,
            const std::shared_ptr<transport_lifetime_t> &transport_lifetime_,
            const std::shared_ptr<physical_queue_record_t> &in_physical_queue_,
            const std::shared_ptr<physical_queue_record_t> &out_physical_queue_,
            bool registry_accounting_,
            bool session_io_writer_);

    //  Pipepair uses this function to let us know about
    //  the peer pipe object.
    void set_peer (pipe_t *peer_);
    pipe_t *detach_peer_link ();
    void retire_physical_queue_endpoints ();
    void cleanup_inbound_pipe ();

    //  Destructor is private. Pipe objects destroy themselves.
    ~pipe_t () ZLINK_OVERRIDE;

    //  Underlying pipes for both directions.
    //  `_out_pipe`, `_state`, `_out_active`, and `_peers_msgs_read` are a
    //  single outbound state cluster guarded by `_out_sync`.
    upipe_t *_in_pipe;
    upipe_t *_out_pipe;

    //  Can the pipe be read from / written to?
    bool _in_active;
    bool _out_active;
    bool _transport_pair_write_held;
    //  Remote receive-flow state applied on this pipe's own thread. Guarded by
    //  _out_sync, exactly like _transport_pair_write_held.
    bool _remote_flow_paused;
    // A request-only admission miss does not remove this pipe from ordinary
    // send scheduling. It still needs one owner-thread wake when terminal
    // completion returns correlation capacity.
    bool _request_correlation_waiting;
    bool _request_correlation_activation_pending;
    //  Set while the pipe's owner holds an accepted message that has not been
    //  written yet. Guarded by _out_sync with the rest of the outbound state.
    bool _out_owner_message_started;
    //  The first owner message part can reuse the readiness check performed
    //  when the owner consumed its routing identifier.
    bool _out_owner_message_start_pending;
    //  Epoch of the last applied remote state. A command that does not advance
    //  it is a stale replay: the attach-time replay and a freshly accepted
    //  frame can be queued in either order.
    uint64_t _remote_flow_epoch;
    //  See set_remote_flow_pause_started_ms(); only touched from the
    //  socket-owning thread, never under _out_sync.
    uint64_t _remote_flow_pause_started_ms;
    std::atomic<bool> _waiting_for_byte_credit;
    std::atomic<bool> _request_correlation_recovery;
    mutable std::atomic<bool> _waiting_for_flow_resume;

    //  High watermark for the outbound pipe.
    uint64_t _hwm;
    uint64_t _request_correlation_bytes;
    uint64_t _request_correlation_work;
    uint64_t _request_correlation_count;

    //  Low watermark for the inbound pipe.
    std::atomic<uint64_t> _lwm;
    std::atomic<uint64_t> _inhwm;
    uint64_t _lwm_hint;

    //  Number of messages read and written so far.
    uint64_t _msgs_read;
    uint64_t _msgs_written;
    uint64_t _bytes_read;
    uint64_t _bytes_written;
    std::atomic<uint64_t> _published_msgs_read;
    std::atomic<uint64_t> _published_bytes_read;
    // Only multipart reads need this extra publication. Single-part traffic
    // remains on the existing complete-message credit publication path.
    std::atomic<uint64_t> _published_incomplete_bytes_read;
    //  A session decoder owns this endpoint without taking _out_sync. Its
    //  queue total is the synchronization source for cold Auto-HWM snapshots;
    //  the provisional value only classifies that total for diagnostics.
    std::atomic<uint64_t> _published_outbound_total_bytes;
    std::atomic<uint64_t> _published_outbound_provisional_bytes;
    uint64_t _last_credit_bytes_read;
    uint64_t _in_generation;
    uint64_t _out_generation;
    uint64_t _in_incomplete_bytes;
    uint64_t _out_incomplete_bytes;
    uint64_t _out_incomplete_payload_bytes;
    bool _out_multipart_started_empty;
    bool _decoder_multipart_started_empty;
    //  Public payload bound for one complete message, or 0 when unlimited.
    uint64_t _max_message_bytes;
    uint64_t _oversize_message_admission_count;
    uint64_t _oversize_message_admission_max_bytes;
    uint64_t _connected_time;

    //  Last received peer's msgs_read. The actual number in the peer
    //  can be higher at the moment.
    uint64_t _peers_msgs_read;
    uint64_t _peers_bytes_read;

    //  The pipe object on the other side of the pipepair. Each non-null link
    //  owns one lifetime reference on the pointed-to endpoint. Termination
    //  detaches the link atomically and releases (or transfers) that reference
    //  only after its final peer access.
    std::atomic<pipe_t *> _peer;

    //  Sink to send events to.
    i_pipe_events *_sink;

    //  States of the pipe endpoint:
    //  active: common state before any termination begins,
    //  delimiter_received: delimiter was read from pipe before
    //      term command was received,
    //  waiting_for_delimiter: term command was already received
    //      from the peer but there are still pending messages to read,
    //  term_ack_sent: all pending messages were already read and
    //      all we are waiting for is ack from the peer,
    //  term_req_sent1: 'terminate' was explicitly called by the user,
    //  term_req_sent2: user called 'terminate' and then we've got
    //      term command from the peer as well.
    enum
    {
        active,
        delimiter_received,
        waiting_for_delimiter,
        term_ack_sent,
        term_req_sent1,
        term_req_sent2
    } _state;

    //  If true, we receive all the pending inbound messages before
    //  terminating. If false, we terminate immediately when the peer
    //  asks us to.
    bool _delay;

    //  Routing id of the writer. Used uniquely by the reader side.
    blob_t _router_socket_routing_id;
    blob_t _transport_peer_identity;
    std::atomic<bool> _connection_ready_event_emitted;
    class lifetime_state_t
    {
      public:
        enum transition_t
        {
            transition_invalid,
            transition_complete,
            transition_delete_owner
        };

        lifetime_state_t ();
        bool retain ();
        transition_t release ();
        transition_t complete_termination ();
        bool terminal () const;
        uint32_t refs () const;

      private:
        friend class session_termination_test_access_t;
        static const uint32_t terminal_bit = 0x80000000U;
        static const uint32_t refs_mask = terminal_bit - 1U;
        std::atomic<uint32_t> _state;
    };

    lifetime_state_t _lifetime;
    lifetime_state_t _inbound_read_lifetime;
    // Intrusive, allocation-free link used while socket-message teardown is
    // deferred beyond the outer receive-command critical section.
    pipe_t *_deferred_socket_msg_termination_next;

    //  Computes appropriate low watermark from the given high watermark.
    static uint64_t compute_lwm (uint64_t hwm_);
    static uint64_t apply_lwm_hint (uint64_t hwm_,
                                    uint64_t lwm_,
                                    uint64_t lwm_hint_);
    bool check_hwm_unlocked () const;

    const bool _conflate;

    //  True for session<->socket pipes; hiccup() recreates the inpipe with
    //  the matching (smaller) chunk granularity.
    const bool _session_pipe;
    //  True only for the endpoint owned by session_base_t's I/O thread. The
    //  peer endpoint remains on the ordinary _out_sync socket-send path.
    const bool _session_io_writer;

    // The endpoints of this pipe.
    endpoint_uri_pair_t _endpoint_pair;
    std::shared_ptr<transport_lifetime_t> _transport_lifetime;
    std::shared_ptr<physical_queue_record_t> _in_physical_queue;
    std::shared_ptr<physical_queue_record_t> _out_physical_queue;
    transport_lane_t _transport_lane;
    std::atomic<unsigned char> _transport_lane_count;
    std::atomic<bool> _transport_pair_application_ready;
    //  Lock-free mirror of `_state == active`. `_state` only ever leaves
    //  `active`, so every transition clears this flag under `_out_sync` and
    //  readers never need that lock.
    std::atomic<bool> _state_active;
    const bool _registry_accounting;
    uint64_t _transport_pair_id;
    uint64_t _transport_pair_generation;
    bool _locally_initiated;
    std::atomic<int> _peer_socket_type;
    std::atomic<uint64_t> _peer_weight_connection_id;
    std::atomic<uint32_t> _peer_weight;
    std::atomic<bool> _transport_disconnected_event_claimed;

    // Disconnect msg
    msg_t _disconnect_msg;
    // Latest absolute controls deferred behind an open Application multipart.
    // Each update moves its slot to a shared monotonic sequence. Wire frames
    // (or inproc owner commands) are materialised only at the next boundary.
    uint32_t _pending_peer_weight;
    uint64_t _pending_peer_weight_sequence;
    unsigned char _pending_flow_state;
    uint64_t _pending_flow_state_epoch;
    uint64_t _pending_flow_state_sequence;
    bool _pending_flow_state_valid;
    uint64_t _pending_peer_control_sequence;
    mutable fast_mutex_t _out_sync;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (pipe_t)
};

void send_routing_id (pipe_t *pipe_, const options_t &options_);

void send_hello_msg (pipe_t *pipe_, const options_t &options_);
}

#endif
