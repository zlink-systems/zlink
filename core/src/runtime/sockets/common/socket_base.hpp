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
typedef void (*sub_io_handler_fn) (const zlink_routing_id_t *source_rid_,
                                   const char *topic_,
                                   size_t topic_len_,
                                   zlink_msg_t *parts_,
                                   size_t part_count_,
                                   void *userdata_);

struct socket_request_reply_bridge_t
{
    std::shared_ptr<socket_reqrep_internal::socket_request_reply_state_t> request_reply_state;
    std::shared_ptr<part_helper_internal::handle_state_t> part_helper_state;
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
        draining (false)
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
    int send (zlink::msg_t *msg_, int flags_);
    // Internal helper for logical multipart wrappers that already hold the
    // public send scope for the whole transaction.
    int send_scoped (zlink::msg_t *msg_,
                     int flags_,
                     socket_public_send_scope_t &scope_,
                     zlink::pipe_t **pipe_out_ = NULL);
    int send_routed (const zlink_routing_id_t *target_rid_, zlink::msg_t *msg_, int flags_);
    int send_routed_scoped (const zlink_routing_id_t *target_rid_,
                            zlink::msg_t *msg_,
                            int flags_,
                            socket_public_send_scope_t &scope_,
                            uint64_t *connection_id_out_ = NULL,
                            uint64_t expected_connection_id_ = 0,
                            zlink::pipe_t **pipe_out_ = NULL);
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
    pipe_t *completion_pipe_for_application (pipe_t *application_pipe_) const;
    pipe_t *application_pipe_for_completion (pipe_t *completion_pipe_) const;
    pipe_t *completion_pipe_for_peer (const zlink_routing_id_t *peer_rid_) const;
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
    int socket_set_msg_handler (zlink_socket_msg_handler_fn handler_);
    int socket_set_msg_handler_ex (zlink_socket_msg_handler_fn handler_, void *subject_);
    int socket_set_msg_handler_with_userdata (zlink_socket_msg_handler_fn handler_,
                                              void *subject_,
                                              void *userdata_);
    int socket_msg_dispatch_stop ();
    void socket_msg_dispatch_drain_pending ();
    int socket_set_send_ready_handler (zlink_send_ready_handler_fn handler_);
    int socket_set_send_ready_handler_ex (zlink_send_ready_handler_fn handler_, void *subject_);
    int socket_set_send_ready_handler_with_userdata (zlink_send_ready_handler_fn handler_,
                                                     void *subject_,
                                                     void *userdata_);
    bool socket_msg_dispatch_active () const;
    bool send_ready_handler_active () const;
    int ensure_completion_processing ();
    void release_completion_processing ();
    void acquire_completion_poller ();
    void release_completion_poller ();

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
    int drain_request_completion_controls ();
    void acknowledge_request_completion_notification ();
    static socket_base_t *current_socket_msg_dispatch_socket ();
    static socket_base_t *current_send_ready_dispatch_socket ();
    static zlink::pipe_t *current_socket_msg_dispatch_pipe ();
    static void *current_socket_msg_dispatch_subject ();
    static bool current_socket_msg_dispatch_source_rid (zlink_routing_id_t *out_);
    bool recv_source_rid_capture_requested () const;
    void invoke_send_ready_handler ();
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

    int monitor (const char *endpoint_, uint64_t events_, int event_version_, int type_);

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
                             size_t routing_id_size_);
    void event_handshake_failed_no_detail (const endpoint_uri_pair_t &endpoint_uri_pair_, int err_);
    void event_handshake_failed_protocol (const endpoint_uri_pair_t &endpoint_uri_pair_, int err_);
    void event_handshake_failed_auth (const endpoint_uri_pair_t &endpoint_uri_pair_, int err_);
    void event_connection_ready_changed (const endpoint_uri_pair_t &endpoint_uri_pair_,
                                         const unsigned char *routing_id_,
                                         size_t routing_id_size_);
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
    bool auto_hwm_policy_enabled () const;
    auto_hwm_socket_plan_t prepare_auto_hwm_socket_plan (const auto_hwm_context_plan_t &context_);
    void apply_auto_hwm_socket_plan (const auto_hwm_context_plan_t &context_,
                                     const auto_hwm_socket_plan_t &plan_,
                                     bool force_apply_,
                                     uint32_t recalc_reason_);
    void record_auto_hwm_socket_plan (const auto_hwm_context_plan_t &context_,
                                      const auto_hwm_socket_plan_t &plan_,
                                      uint32_t recalc_reason_);
    void refresh_auto_hwm_policy (bool force_apply_ = false);
    void set_auto_hwm_role (auto_hwm_role_t role_);
    void set_auto_hwm_scope (auto_hwm_scope_t scope_, size_t scope_count_);
    void clear_auto_hwm_manual_overrides (bool sndhwm_, bool rcvhwm_, bool sndbuf_, bool rcvbuf_);
    bool auto_hwm_connection_bucket_state (uint32_t *bucket_index_out_,
                                           zlink_auto_hwm_profile_t *profile_out_,
                                           uint64_t *effective_message_bytes_out_) const;
    void set_auto_hwm_connection_bucket_state (uint32_t bucket_index_,
                                               zlink_auto_hwm_profile_t profile_,
                                               uint64_t effective_message_bytes_);
    void clear_auto_hwm_connection_bucket_state ();
    void set_auto_hwm_policy_enabled (bool enabled_);
    bool monitor_has_attached_pipes () const;
    void socket_peer_remote_endpoints (std::vector<std::string> *out_);
    void socket_bound_endpoints (std::set<std::string> *out_) const;
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
    std::shared_ptr<part_helper_internal::handle_state_t> part_helper_state () const;
    std::shared_ptr<part_helper_internal::handle_state_t>
    set_part_helper_state (const std::shared_ptr<part_helper_internal::handle_state_t> &state_);
    void clear_part_helper_state ();

    bool is_ctx_terminated () const;

  protected:
    socket_base_t (zlink::ctx_t *parent_, uint32_t tid_, int sid_);
    ~socket_base_t () ZLINK_OVERRIDE;

    //  Concrete algorithms for the x- methods are to be defined by
    //  individual socket types.
    virtual void xattach_pipe (zlink::pipe_t *pipe_,
                               bool subscribe_to_all_ = false,
                               bool locally_initiated_ = false) = 0;

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
    virtual int xsend (zlink::msg_t *msg_);
    virtual int xsend_pipe (zlink::msg_t *msg_, zlink::pipe_t **pipe_out_);
    virtual int xsend_routed (const zlink_routing_id_t *target_rid_,
                              zlink::msg_t *msg_,
                              uint64_t *connection_id_out_,
                              uint64_t expected_connection_id_,
                              zlink::pipe_t **pipe_out_);
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
    static void store_socket_msg_part (std::vector<zlink_msg_t> *parts_, zlink::msg_t *msg_);
    static int init_peer_weight_command (zlink::msg_t *msg_, uint32_t weight_);
    static bool decode_peer_weight_command (const zlink::msg_t &msg_, uint32_t *weight_out_);
    void broadcast_local_peer_weight ();
    void send_local_peer_weight (pipe_t *pipe_);

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
    void arm_send_ready_after_backpressure ();

  protected:
    typedef std::unique_lock<std::recursive_mutex> socket_msg_dispatch_lock_t;
    socket_msg_dispatch_lock_t lock_socket_msg_dispatch ()
    {
        return socket_msg_dispatch_lock_t (dispatch_runtime ().socket_msg_dispatch_sync);
    }
    void arm_send_ready_notification ();
    void notify_send_ready_if_armed ();
    void emit_socket_monitor_value_event (uint64_t event_,
                                          uint64_t value_,
                                          const endpoint_uri_pair_t &endpoint_uri_pair_);
    void emit_peer_weight_changed (pipe_t *pipe_, uint32_t weight_);
    void snapshot_attached_pipes (std::vector<pipe_t *> *out_);
    bool has_attached_pipes () const;

  private:
    friend struct multipart_send_facade_t;

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
                                zlink::pipe_t **pipe_out_ = NULL);

    enum
    {
        monitor_queue_hwm = 4096,
        monitor_max_values = zlink::socket_monitor_max_values
    };

    typedef zlink::socket_monitor_event_record_t monitor_event_record_t;
    typedef zlink::socket_endpoint_pipe_t endpoint_pipe_t;
    typedef zlink::socket_endpoints_t endpoints_t;
    typedef zlink::socket_inprocs_t inprocs_t;
    typedef zlink::socket_endpoint_runtime_t endpoint_runtime_t;
    typedef zlink::socket_command_runtime_t command_runtime_t;
    typedef zlink::socket_monitor_runtime_t monitor_runtime_t;
    typedef zlink::socket_dispatch_bridge_t dispatch_bridge_t;
    typedef zlink::socket_lifecycle_coordinator_t lifecycle_coordinator_t;
    typedef zlink::socket_runtime_t socket_runtime_t;

    endpoint_runtime_t &endpoint_runtime () { return _runtime.endpoint_runtime; }
    const endpoint_runtime_t &endpoint_runtime () const { return _runtime.endpoint_runtime; }
    command_runtime_t &command_runtime () { return _runtime.command_runtime; }
    const command_runtime_t &command_runtime () const { return _runtime.command_runtime; }
    monitor_runtime_t &monitor_runtime () { return _runtime.monitor_runtime; }
    const monitor_runtime_t &monitor_runtime () const { return _runtime.monitor_runtime; }
    dispatch_bridge_t &dispatch_runtime () { return _runtime.dispatch_bridge; }
    const dispatch_bridge_t &dispatch_runtime () const { return _runtime.dispatch_bridge; }
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
                uint32_t internal_flags_ = 0);

    zlink_send_ready_handler_fn socket_send_ready_handler () const;
    void *socket_msg_handler_subject () const;
    void *socket_msg_handler_userdata () const;
    void *socket_send_ready_handler_subject () const;
    void *socket_send_ready_handler_userdata () const;

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
    auto_hwm_scope_t _auto_hwm_scope;
    size_t _auto_hwm_scope_count;
    bool _auto_hwm_role_override;
    bool _auto_hwm_policy_enabled;
    bool _auto_hwm_msg_unit_override;
    bool _manual_sndhwm;
    bool _manual_rcvhwm;
    bool _manual_sndbuf;
    bool _manual_rcvbuf;
    bool _completion_async_owned;
    std::atomic<uint32_t> _completion_poller_refs;
    std::atomic<bool> _request_completion_pending;
    auto_hwm_context_plan_t _auto_hwm_context_plan;
    auto_hwm_socket_plan_t _auto_hwm_socket_plan;
    bool _auto_hwm_connection_bucket_state_valid;
    uint32_t _auto_hwm_connection_bucket_index;
    zlink_auto_hwm_profile_t _auto_hwm_connection_bucket_profile;
    uint64_t _auto_hwm_connection_bucket_message_bytes;
    uint64_t _auto_hwm_last_recalc_ms;
    uint32_t _auto_hwm_last_recalc_reason;
    uint64_t _auto_hwm_deferred_sndhwm;
    uint64_t _auto_hwm_deferred_rcvhwm;
    bool _auto_hwm_deferred_sndhwm_valid;
    bool _auto_hwm_deferred_rcvhwm_valid;
    alignas (64) std::atomic<uint64_t> _auto_hwm_send_attempts;
    alignas (64) std::atomic<uint64_t> _auto_hwm_send_blocked_attempts;
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
