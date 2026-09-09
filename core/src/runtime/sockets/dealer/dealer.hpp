/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_DEALER_HPP_INCLUDED__
#define __ZLINK_DEALER_HPP_INCLUDED__

#include <map>
#include <string>

#include "sockets/common/socket_base.hpp"
#include "sockets/internal/fq.hpp"
#include "sockets/internal/lb.hpp"

namespace zlink
{
class ctx_t;
class msg_t;
class pipe_t;
class io_thread_t;
class socket_base_t;

class dealer_t : public socket_base_t
{
  public:
    dealer_t (zlink::ctx_t *parent_, uint32_t tid_, int sid_);
    ~dealer_t () ZLINK_OVERRIDE;

    int sendpipe_to (
      zlink::pipe_t *pipe_, zlink::msg_t *msg_, int flags_,
      pipe_message_admission_t *admission_out_ = NULL,
      pipe_write_observer_fn observer_ = NULL,
      void *observer_userdata_ = NULL);
#ifdef ZLINK_BUILD_TESTS
    uint32_t test_peer_weight (zlink::pipe_t *pipe_) const;
    size_t test_peer_weight_count (uint32_t weight_) const;
#endif

  protected:
    //  Overrides of functions from socket_base_t.
    void xattach_pipe (zlink::pipe_t *pipe_,
                       bool subscribe_to_all_,
                       bool locally_initiated_) ZLINK_FINAL;
    int xsetsockopt (int option_, const void *optval_, size_t optvallen_) ZLINK_OVERRIDE;
    int xgetsockopt (int option_, void *optval_, size_t *optvallen_) ZLINK_OVERRIDE;
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
    int xselect_routed_submit_pipe (pipe_t **pipe_out_,
                                    bool request_only_) ZLINK_OVERRIDE;
    int xcommit_request_submit_pipe (pipe_t *pipe_) ZLINK_OVERRIDE;
    int xsend_selected_pipe (pipe_t *pipe_, msg_t *msg_, int flags_,
                             bool request_only_,
                             pipe_message_admission_t *admission_out_,
                             pipe_write_observer_fn observer_,
                             void *observer_userdata_) ZLINK_OVERRIDE;
    int xsend_configured_endpoint (
      const std::string &endpoint_, zlink::msg_t *msg_, int flags_,
      bool request_only_,
      zlink::pipe_t **pipe_out_,
      pipe_message_admission_t *admission_out_ = NULL,
      pipe_write_observer_fn observer_ = NULL,
      void *observer_userdata_ = NULL) ZLINK_OVERRIDE;
    int xselect_routed_submit_target (
      const zlink_routing_id_t *router_rid_or_null_,
      zlink_routed_submit_target_t *target_out_) ZLINK_OVERRIDE;
    int xselect_request_submit_target (
      const zlink_routing_id_t *router_rid_or_null_,
      zlink_routed_submit_target_t *target_out_,
      uint64_t *transport_connection_id_out_,
      uint64_t *route_incarnation_id_out_,
      std::string *logical_endpoint_out_) ZLINK_OVERRIDE;
    void xforget_request_route_endpoint (
      const std::string &endpoint_) ZLINK_OVERRIDE;
    int xrecv (zlink::msg_t *msg_) ZLINK_OVERRIDE;
    int xrecv_pipe (zlink::msg_t *msg_, zlink::pipe_t **pipe_out_) ZLINK_OVERRIDE;
    bool xhas_in () ZLINK_OVERRIDE;
    bool xhas_out () ZLINK_OVERRIDE;
    int xrollback () ZLINK_OVERRIDE;
    void xread_deactivated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void xread_activated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void xwrite_activated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void xpipe_terminated (zlink::pipe_t *pipe_) ZLINK_OVERRIDE;

    int recvpipe (zlink::msg_t *msg_, zlink::pipe_t **pipe_);

  private:
    static bool active_submit_candidate (dealer_t *dealer_, pipe_t *pipe_);
    static bool routed_submit_candidate (pipe_t *pipe_, void *userdata_);
    static bool request_submit_candidate (pipe_t *pipe_, void *userdata_);
    int send_selected_pipe (
      pipe_t *pipe_, msg_t *msg_, int flags_, bool request_only_,
      pipe_t **pipe_out_, pipe_message_admission_t *admission_out_,
      pipe_write_observer_fn observer_, void *observer_userdata_);
    int apply_peer_weight (pipe_t *pipe_, uint32_t weight_) ZLINK_OVERRIDE;
    void update_request_route_weight (pipe_t *pipe_, uint32_t weight_);
    void remember_request_route (pipe_t *pipe_, uint32_t weight_);

    struct request_route_history_t
    {
        request_route_history_t () : weight (100), running_value (0) {}

        std::string peer_rid;
        uint32_t weight;
        int64_t running_value;
    };
    typedef std::map<std::string, request_route_history_t>
      request_route_history_map_t;

    //  Messages are fair-queued from inbound pipes. And load-balanced to
    //  the outbound pipes.
    fq_t _fq;
    lb_t _lb;
    request_route_history_map_t _request_route_history;

    // if true, send an empty message to every connected router peer
    bool _probe_router;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (dealer_t)
};
}

#endif
