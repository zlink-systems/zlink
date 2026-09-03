/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_PART_HELPER_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_PART_HELPER_INTERNAL_HPP_INCLUDED__

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "sockets/common/socket_runtime.hpp"
#include "api/socket/inline_msg_buffer_internal.hpp"
#include <zlink.h>

namespace zlink
{
class socket_base_t;

namespace part_helper_internal
{
enum send_family_t
{
    send_family_none = 0,
    send_family_send,
    send_family_send_rid,
    send_family_publish,
    send_family_router_request,
    send_family_dealer_request,
    send_family_router_reply
};

enum recv_family_t
{
    recv_family_none = 0,
    recv_family_basic,
    recv_family_subscribe,
    recv_family_xpub,
    recv_family_router
};

struct send_sequence_spec_t
{
    send_sequence_spec_t ();

    send_family_t family;
    zlink_send_flags_t flags;
    uint32_t timeout_ms;
    uint64_t request_seq;
    uint64_t pending_cookie;
    zlink_routing_id_t routing_id;
    bool has_routing_id;
    std::string_view topic;
    bool has_topic;
    bool request_like;
};

// Keep common short send records inline. Larger multipart records retain the
// same ownership model and spill through inline_msg_buffer_t's dynamic path.
const size_t inline_send_part_capacity = 4;
typedef zlink::socket_internal::inline_msg_buffer_t<inline_send_part_capacity>
  send_part_buffer_t;

struct send_sequence_state_t
{
    send_sequence_state_t ();

    bool active;
    send_sequence_spec_t spec;
    std::string topic_storage;
    zlink::socket_base_t *sink_socket;
    std::optional<zlink::socket_public_send_scope_t> send_scope;
    send_part_buffer_t buffered_parts;
    std::thread::id owner_thread;
};

// Perf's normal multipart receive is two application parts. Keep those parts
// with the socket-owned receive sequence; larger records spill to bounded
// dynamic storage through the internal buffer's spill path.
const size_t inline_recv_part_capacity = 2;
typedef zlink::socket_internal::inline_msg_buffer_t<inline_recv_part_capacity>
  recv_part_buffer_t;

struct recv_sequence_state_t
{
    recv_sequence_state_t ();

    bool active;
    recv_family_t family;
    zlink::socket_base_t *source_socket;
    std::thread::id owner_thread;
    bool return_source_rid_as_null;
    zlink_routing_id_t source_node_rid;
    uint64_t request_seq;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    int subscribed;
    std::string topic_id;
    recv_part_buffer_t buffered_parts;
    size_t next_part_index;
    bool public_delivery_hold;
};

struct handle_state_t
{
    std::mutex mutex;
    send_sequence_state_t send;
    recv_sequence_state_t recv;
};

int validate_send_flags (zlink_send_flags_t flags_);
int validate_part_flag (zlink_part_flag_t part_flag_);
bool routing_id_equals (const zlink_routing_id_t &lhs_, const zlink_routing_id_t &rhs_);
void copy_routing_id (const zlink_routing_id_t *src_, zlink_routing_id_t *dest_);
void consume_send_part (zlink_msg_t *part_);
bool try_rollback_send_scope_locked (send_sequence_state_t *state_);
bool send_spec_equals (const send_sequence_spec_t &lhs_, const send_sequence_spec_t &rhs_);
bool routed_part_debug_enabled ();
void trace_routed_part_prepare_failed (send_family_t family_, int err_);
void trace_routed_part_send_failed (send_family_t family_, bool first_part_, int err_);
std::shared_ptr<handle_state_t> find_or_create_socket_state (zlink::socket_base_t *socket_);
std::shared_ptr<handle_state_t> find_socket_state (zlink::socket_base_t *socket_);
bool recv_sequence_active (const std::shared_ptr<handle_state_t> &state_);
int stage_recv_sequence (const std::shared_ptr<handle_state_t> &state_,
                         recv_family_t family_,
                         zlink::socket_base_t *source_socket_,
                         const zlink_routing_id_t *source_node_rid_,
                         uint64_t request_seq_,
                         zlink_msg_t *parts_,
                         size_t part_count_,
                         std::thread::id owner_thread_,
                         uint64_t transport_pair_id_ = 0,
                         uint64_t transport_pair_generation_ = 0);
int adopt_recv_public_delivery_hold (
  const std::shared_ptr<handle_state_t> &state_);
void set_recv_metadata (recv_sequence_state_t *recv_,
                        const zlink_routing_id_t *source_node_rid_,
                        uint64_t request_seq_);
int buffer_recv_parts (recv_sequence_state_t *recv_,
                       zlink_msg_t *parts_,
                       size_t part_count_);
int take_recv_part (recv_sequence_state_t *recv_,
                    zlink_msg_t *part_out_,
                    zlink_part_flag_t *has_more_out_);
int take_recv_part (const std::shared_ptr<handle_state_t> &state_,
                    zlink_msg_t *part_out_,
                    zlink_part_flag_t *has_more_out_);
int take_recv_part (const std::shared_ptr<handle_state_t> &state_,
                    zlink_msg_t *part_out_,
                    zlink_part_flag_t *has_more_out_,
                    const zlink_routing_id_t **source_node_rid_out_,
                    uint64_t *request_seq_out_,
                    uint64_t *transport_pair_id_out_,
                    uint64_t *transport_pair_generation_out_);
void reset_send_sequence (send_sequence_state_t *state_,
                          bool notify_release_ = true);
zlink::socket_base_t *reset_recv_sequence (recv_sequence_state_t *state_);
int prepare_send_step (const send_sequence_spec_t &spec_,
                       zlink::socket_base_t *sink_socket_,
                       std::shared_ptr<handle_state_t> *state_out_,
                       bool *first_part_out_);
// Returns 1 without creating a sequence when start_if_inactive_ is false and
// no sequence is active. On success, lock_out_ keeps the state mutex held so a
// caller can stage or collect the part in the same linearized state step.
int prepare_send_step_locked (const send_sequence_spec_t &spec_,
                              zlink::socket_base_t *sink_socket_,
                              handle_state_t **state_out_,
                              std::unique_lock<std::mutex> *lock_out_,
                              bool *first_part_out_,
                              bool start_if_inactive_);
int prepare_recv_step (recv_family_t family_,
                       zlink::socket_base_t *source_socket_,
                       const std::shared_ptr<handle_state_t> &state_,
                       bool *first_part_out_,
                       zlink::socket_base_t **active_source_socket_out_);
void complete_send_step (const std::shared_ptr<handle_state_t> &state_,
                         zlink_part_flag_t part_flag_);
void complete_send_step (handle_state_t *state_,
                         zlink_part_flag_t part_flag_);
// The caller must hold state_->mutex. These variants let a public part entry
// prepare, stage, and suspend/reset one helper step without dropping and
// reacquiring the same state lock.
void complete_send_step_locked (handle_state_t *state_,
                                zlink_part_flag_t part_flag_);
// The caller must hold state_->mutex.
int take_buffered_send_record_locked (handle_state_t *state_,
                                      send_part_buffer_t *parts_out_);
void complete_recv_step (const std::shared_ptr<handle_state_t> &state_,
                         zlink_part_flag_t has_more_);
void abort_send_step (handle_state_t *state_);
void abort_send_step (const std::shared_ptr<handle_state_t> &state_);
void abort_current_non_publish_send_sequence (void *handle_);
void abort_recv_step (const std::shared_ptr<handle_state_t> &state_);
void cleanup_socket (zlink::socket_base_t *socket_);
}
}

#endif
