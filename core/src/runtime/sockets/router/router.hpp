/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ROUTER_HPP_INCLUDED__
#define __ZLINK_ROUTER_HPP_INCLUDED__

#include <map>
#include <mutex>
#include <deque>
#include <vector>

#include "sockets/common/socket_base.hpp"
#include "utils/blob.hpp"
#include "core/msg.hpp"
#include "sockets/internal/fq.hpp"
#include "api/socket/part_helper_internal.hpp"

namespace zlink
{
class ctx_t;
class pipe_t;

//  Outbound eligibility is tracked through routing-id lookup and cached
//  writable state to avoid full pipe scans on the hot path.
class router_t : public routing_socket_base_t
{
  public:
    router_t (zlink::ctx_t *parent_, uint32_t tid_, int sid_);
    ~router_t () ZLINK_OVERRIDE;

    //  Overrides of functions from socket_base_t.
    void xattach_pipe (zlink::pipe_t *pipe_,
                       bool subscribe_to_all_,
                       bool locally_initiated_) ZLINK_FINAL;
    bool emit_transport_pair_ready (zlink::pipe_t *pipe_) ZLINK_OVERRIDE;
    int xsetsockopt (int option_, const void *optval_, size_t optvallen_) ZLINK_FINAL;
    int xgetsockopt (int option_, void *optval_, size_t *optvallen_) ZLINK_FINAL;
    int xsend (zlink::msg_t *msg_,
               pipe_message_admission_t *admission_out_ = NULL) ZLINK_OVERRIDE;
    int xsend_pipe (
      zlink::msg_t *msg_, zlink::pipe_t **pipe_out_,
      pipe_message_admission_t *admission_out_ = NULL,
      pipe_write_observer_fn observer_ = NULL,
      void *observer_userdata_ = NULL) ZLINK_OVERRIDE;
    int xsend_routed (const zlink_routing_id_t *target_rid_,
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
                      bool request_only_ = false)
      ZLINK_OVERRIDE;
    int xselect_routed_submit_target (
      const zlink_routing_id_t *router_rid_or_null_,
      zlink_routed_submit_target_t *target_out_) ZLINK_OVERRIDE;
    int xselect_request_submit_target (
      const zlink_routing_id_t *router_rid_or_null_,
      zlink_routed_submit_target_t *target_out_,
      uint64_t *transport_connection_id_out_,
      uint64_t *route_incarnation_id_out_,
      std::string *logical_endpoint_out_) ZLINK_OVERRIDE;
    int xrecv (zlink::msg_t *msg_) ZLINK_OVERRIDE;
    int xrecv_pipe (zlink::msg_t *msg_,
                    zlink::pipe_t **pipe_out_) ZLINK_OVERRIDE;
    int xrecv_routed (zlink::msg_t *msg_,
                      zlink_routing_id_t *source_rid_out_,
                      uint64_t *connection_id_out_,
                      zlink::pipe_t **source_pipe_out_ = NULL,
                      pipe_t::read_admission_fn *admission_ = NULL,
                      void *admission_userdata_ = NULL,
                      uint64_t *route_binding_token_out_ = NULL) ZLINK_OVERRIDE;
    bool xhas_in () ZLINK_OVERRIDE;
    bool has_route_change () const ZLINK_OVERRIDE;
    int routes_snapshot (zlink_router_route_t *routes_, size_t capacity_,
                         size_t *count_);
    bool is_selected_pipe (pipe_t *pipe_, uint64_t generation_ = 0,
                           uint64_t *observed_generation_out_ = NULL) const;
    template <typename Action>
    auto within_receive_turn (Action action_) -> decltype (action_ ())
    {
        const socket_receive_entry_scope_t receive_turn (receive_runtime ());
        return action_ ();
    }
    // Buffer-too-small staging is cold. Hold the route lock across its token
    // check and helper publication so a concurrent handover either precedes
    // staging or clears the staged record at the change point.
    template <typename Stage>
    int stage_selected_record (pipe_t *pipe_, uint64_t generation_, Stage stage_)
    {
        return within_receive_turn ([&] () -> int {
            std::lock_guard<std::mutex> lock (_out_pipes_sync);
            if (generation_ == 0 || !is_selected_pipe (pipe_, generation_)) {
                errno = ESTALE;
                return -1;
            }
            return stage_ ();
        });
    }
    uint64_t last_recv_route_generation () const;
    void set_last_recv_route_generation (uint64_t generation_);
    size_t xredrive_reply_token_waiters (size_t max_pipes_) ZLINK_OVERRIDE;
    bool xhas_out () ZLINK_OVERRIDE;
    void xread_deactivated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void xread_activated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void xpipe_terminated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void xsocket_msg_pipe_terminated (zlink::pipe_t *pipe_) ZLINK_OVERRIDE;
    int xterm_peer_rid (const zlink_routing_id_t *peer_rid_,
                        pipe_t **target_out_,
                        bool *delay_out_) ZLINK_OVERRIDE
    {
        fail_blocking_send_waits_for_logical_target (peer_rid_, ENOENT);
        revoke_router_reply_targets_for_rid (peer_rid_);
        *delay_out_ = false;
        return prepare_out_pipe_termination_by_routing_id (peer_rid_,
                                                            target_out_);
    }
    int get_peer_state (const void *routing_id_, size_t routing_id_size_) const ZLINK_FINAL;
    pipe_t *retain_current_transport_pair_pipe (
      const zlink_routing_id_t *peer_rid_, int peer_socket_type_,
      transport_lane_t lane_) const ZLINK_OVERRIDE;
#ifdef ZLINK_BUILD_TESTS
    uint32_t test_peer_weight (zlink::pipe_t *pipe_) const;
#endif

  protected:
    //  Rollback any message parts that were sent but not yet flushed.
    int xrollback () ZLINK_OVERRIDE;
    bool xsend_writable_target_ready (
      const zlink_routing_id_t *target_rid_or_null_) ZLINK_OVERRIDE;
    bool xsend_writable_target_known (
      const zlink_routing_id_t *target_rid_or_null_) ZLINK_OVERRIDE;
    bool xsend_writable_target_for_pipe (
      zlink::pipe_t *pipe_, zlink_routing_id_t *target_rid_out_) ZLINK_OVERRIDE;

  private:
    int send_with_observer (zlink::msg_t *msg_,
                            pipe_message_admission_t *admission_out_,
                            pipe_write_observer_fn observer_,
                            void *observer_userdata_);
    struct route_discard_batch_t
    {
        ~route_discard_batch_t ();
        void close_messages ();
        struct followup_t
        {
            pipe_t *pipe;
            bool delimiter;
            bool recheck;
        };
        std::deque<msg_t> messages;
        part_helper_internal::recv_part_buffer_t staged_parts;
        std::vector<followup_t> followups;
    };
    void finish_route_discard (route_discard_batch_t *batch_);
    struct route_adoption_actions_t
    {
        route_adoption_actions_t () :
            terminate_pipe (NULL),
            superseded_pipe (NULL),
            staged_hold_socket (NULL),
            staged_route_source_pipe (NULL),
            staged_reply_token (0),
            cache_completion (false)
        {
        }
        pipe_t *terminate_pipe;
        pipe_t *superseded_pipe;
        socket_base_t *staged_hold_socket;
        pipe_t *staged_route_source_pipe;
        uint64_t staged_reply_token;
        zlink_routing_id_t staged_reply_rid;
        bool cache_completion;
        route_discard_batch_t discarded;
    };

    //  Receive peer id and update lookup map. The caller finishes returned
    //  pipe/cross-component actions only after dropping the route mutex.
    bool identify_peer (pipe_t *pipe_, bool locally_initiated_,
                        route_adoption_actions_t *actions_);
    bool adopt_peer_routing_id (pipe_t *pipe_, blob_t routing_id_,
                                bool locally_initiated_,
                                route_adoption_actions_t *actions_);
    void finish_route_adoption (pipe_t *adopted_pipe_,
                                route_adoption_actions_t *actions_);
    void publish_route_change (const blob_t &routing_id_,
                               route_discard_batch_t *discarded_);
    uint64_t next_route_generation ();
    void discard_unselected_record (msg_t *first_, pipe_t *pipe_);
    socket_base_t *discard_route_records (
      pipe_t *pipe_, route_discard_batch_t *discarded_,
      pipe_t **staged_route_source_pipe_out_ = NULL,
      uint64_t *staged_reply_token_out_ = NULL,
      zlink_routing_id_t *staged_reply_rid_out_ = NULL);
    uint64_t recv_selected (msg_t *msg_, pipe_t **pipe_,
                            pipe_t::read_admission_fn *admission_,
                            void *userdata_);
    bool duplicate_pipe_should_replace (const out_pipe_t &existing_outpipe_,
                                        const blob_t &routing_id_,
                                        bool locally_initiated_) const;
    void copy_router_pipe_source_rid (
      pipe_t *pipe_, zlink_routing_id_t *out_,
      uint64_t *route_binding_token_out_) const;
    void finish_current_in_record ();
    void reset_current_in_after_multipart_abort ();
    void select_current_out_pipe (pipe_t *pipe_)
    {
        _current_out = pipe_;
        _current_out_connection_id = pipe_->get_transport_connection_id ();
    }
    void clear_current_out_pipe ()
    {
        _current_out = NULL;
        _current_out_connection_id = 0;
    }
    out_pipe_t *find_transport_pair_out_pipe (
      const zlink_routing_id_t *target_rid_, uint64_t transport_pair_id_,
      uint64_t transport_pair_generation_);
    int select_routed_submit_target_in_turn (
      const zlink_routing_id_t *router_rid_or_null_,
      zlink_routed_submit_target_t *target_out_,
      uint64_t *transport_connection_id_out_,
      uint64_t *route_incarnation_id_out_, bool allow_unpaired_,
      const out_pipe_t **selected_out_pipe_out_) const;
    int apply_peer_weight (pipe_t *pipe_, uint32_t weight_) ZLINK_OVERRIDE;
    std::mutex *route_lifecycle_mutex () const ZLINK_OVERRIDE
    {
        return &_out_pipes_sync;
    }

    //  Fair queueing object for inbound pipes.
    fq_t _fq;

    //  True iff there is a message held in the pre-fetch buffer.
    bool _prefetched;

    //  If true, the receiver got the message part with
    //  the peer's identity.
    bool _routing_id_sent;

    //  Holds the prefetched identity.
    msg_t _prefetched_id;

    //  Holds the prefetched message.
    msg_t _prefetched_msg;

    //  The pipe we are currently reading from
    zlink::pipe_t *_current_in;

    //  Should current_in should be terminate after all parts received?
    bool _terminate_current_in;

    //  If true, more incoming message parts are expected.
    bool _more_in;

    //  Pipes awaiting a peer routing id retain whether this socket initiated
    //  the transport. Duplicate admission needs that direction after the
    //  asynchronous handshake completes.
    std::map<pipe_t *, bool> _anonymous_pipes;

    //  Reciprocal connectors can establish two physical pipes for one stable
    //  routing id. The deterministic loser stays attached under an internal
    //  id instead of being terminated into a reconnect loop. If the selected
    //  pipe closes, the existing standby is promoted immediately.
    std::map<pipe_t *, blob_t> _standby_pipes;

    //  The pipe we are currently writing to.
    zlink::pipe_t *_current_out;
    uint64_t _current_out_connection_id;

    //  If true, more outgoing message parts are expected.
    bool _more_out;

    //  Routing IDs are generated. It's a simple increment and wrap-over
    //  algorithm. This value is the next ID to use (if not used already).
    uint32_t _next_integral_routing_id;

    // If true, report EAGAIN to the caller instead of silently dropping
    // the message targeting an unknown peer.
    bool _mandatory;
    // if true, send an empty message to every connected router peer
    bool _probe_router;

    // If true, the router will reassign an identity upon encountering a
    // name collision. The selected pipe takes the identity.
    bool _handover;
    // Route lifecycle is shared by public API and mailbox paths. Keep this
    // ordinary (non-recursive); monitor events and observer-backed writes stay
    // outside it.
    mutable std::mutex _out_pipes_sync;
    std::atomic<uint64_t> _route_revision;
    std::atomic<uint64_t> _observed_route_revision;
    uint64_t _next_route_generation;
    std::atomic<uint64_t> _last_recv_route_generation;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (router_t)
};
}

#endif
