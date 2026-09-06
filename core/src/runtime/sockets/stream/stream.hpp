/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_STREAM_HPP_INCLUDED__
#define __ZLINK_STREAM_HPP_INCLUDED__

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "sockets/common/socket_base.hpp"
#include "sockets/internal/fq.hpp"
#include "core/ctx_physical_queue_registry.hpp"
#include "utils/mutex.hpp"
#include "utils/stdint.hpp"

namespace zlink
{
class ctx_t;
class pipe_t;

class stream_t ZLINK_FINAL : public routing_socket_base_t
{
  public:
    stream_t (zlink::ctx_t *parent_, uint32_t tid_, int sid_);
    ~stream_t () ZLINK_OVERRIDE;

    void xattach_pipe (zlink::pipe_t *pipe_,
                       bool subscribe_to_all_,
                       bool locally_initiated_) ZLINK_FINAL;
    int xsend (zlink::msg_t *msg_,
               pipe_message_admission_t *admission_out_ = NULL) ZLINK_OVERRIDE;
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
    int xterm_peer_rid (const zlink_routing_id_t *peer_rid_) ZLINK_OVERRIDE;
    int xrecv (zlink::msg_t *msg_) ZLINK_OVERRIDE;
    int xrecv_routed (zlink::msg_t *msg_,
                      zlink_routing_id_t *source_rid_out_,
                      uint64_t *connection_id_out_,
                      zlink::pipe_t **source_pipe_out_,
                      pipe_t::read_admission_fn *admission_,
                      void *admission_userdata_,
                      uint64_t *route_binding_token_out_) ZLINK_OVERRIDE;
    bool xhas_in () ZLINK_OVERRIDE;
    bool xhas_out () ZLINK_OVERRIDE;
    void xread_activated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void xpipe_terminated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    int xsetsockopt (int option_, const void *optval_, size_t optvallen_) ZLINK_FINAL;
    int stream_mark_raw_part_receive () ZLINK_OVERRIDE;
    std::recursive_mutex *api_sync_mutex () ZLINK_OVERRIDE;

    // Pull one decoded PACKET-mode record. The public C adapter validates its
    // aggregate outputs, then calls this owner to apply the ordinary socket
    // receive timeout and transfer header/body ownership together.
    int recv_packet (zlink_routing_id_t *source_rid_out_,
                     zlink::msg_t *header_out_,
                     zlink::msg_t *body_out_,
                     int flags_);

  private:
    bool xsend_writable_target_ready (
      const zlink_routing_id_t *target_rid_or_null_) ZLINK_OVERRIDE;
    bool xsend_writable_target_known (
      const zlink_routing_id_t *target_rid_or_null_) ZLINK_OVERRIDE;
    bool xsend_writable_target_for_pipe (
      zlink::pipe_t *pipe_, zlink_routing_id_t *target_rid_out_) ZLINK_OVERRIDE;

    enum
    {
        route_shard_count = 64
    };

    struct route_shard_t
    {
        typedef std::map<uint32_t, zlink::pipe_t *> routes_t;

        mutex_t sync;
        routes_t routes;
    };

    struct packet_record_t
    {
        packet_record_t ();
        packet_record_t (packet_record_t &&other_);
        packet_record_t &operator= (packet_record_t &&other_);
        ~packet_record_t ();

        uint32_t source_rid;
        zlink::msg_t header;
        zlink::msg_t body;
        uint64_t accounted_bytes;

      private:
        packet_record_t (const packet_record_t &);
        packet_record_t &operator= (const packet_record_t &);
    };

    route_shard_t &route_shard_for (uint32_t routing_id_);
    bool publish_route_locked (uint32_t routing_id_,
                               zlink::pipe_t *pipe_);
    bool identify_peer (pipe_t *pipe_, bool locally_initiated_);
    void maybe_emit_connect_event (pipe_t *pipe_, uint32_t routing_id_value_ = 0);
    void queue_stream_notify (uint32_t routing_id_);
    bool packet_queue_at_limit () const;
    int enqueue_packet (uint32_t source_rid_,
                        zlink::msg_t *header_,
                        zlink::msg_t *body_);
    int decode_packet_bytes (uint32_t source_rid_,
                             zlink::msg_t *raw_,
                             zlink::pipe_t *source_pipe_,
                             size_t start_offset_,
                             size_t *next_offset_out_);
    int pump_packet_receive_queue ();
    int xrecv_packet_header (zlink::msg_t *header_out_);
    void clear_packet_receive_queue ();
    void clear_pending_packet_input ();
    fq_t _fq;

    std::deque<uint32_t> _stream_notify_routing_ids;

    std::deque<packet_record_t> _packet_receive_queue;
    uint64_t _packet_receive_accounted_bytes;
    zlink::msg_t _packet_pending_input;
    size_t _packet_pending_input_offset;
    zlink::pipe_t *_packet_pending_input_pipe;
    uint32_t _packet_pending_input_rid;
    bool _packet_pending_input_valid;
    zlink::msg_t _packet_recv_body;
    uint32_t _packet_recv_rid;
    bool _packet_recv_body_ready;

    zlink::pipe_t *_current_out;
    bool _more_out;

    std::atomic<uint32_t> _next_integral_routing_id;
    std::mutex _route_publication_mutex;
    route_shard_t _route_shards[route_shard_count];

    mutable std::recursive_mutex _api_mutex;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (stream_t)
};
}

#endif
