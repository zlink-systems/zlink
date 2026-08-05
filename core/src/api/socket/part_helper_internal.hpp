/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_PART_HELPER_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_PART_HELPER_INTERNAL_HPP_INCLUDED__

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sockets/common/socket_runtime.hpp"
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
    send_family_router_reply,
    send_family_router_completion_control,
    send_family_dealer_request_frame,
    send_family_dealer_reply
};

enum recv_family_t
{
    recv_family_none = 0,
    recv_family_basic,
    recv_family_subscribe,
    recv_family_router,
    recv_family_dealer
};

struct send_sequence_spec_t
{
    send_sequence_spec_t ();

    send_family_t family;
    zlink_send_flags_t flags;
    uint32_t timeout_ms;
    uint64_t request_seq;
    zlink_reply_handler_fn handler;
    void *userdata;
    zlink_routing_id_t rid1;
    zlink_routing_id_t rid2;
    bool has_rid1;
    bool has_rid2;
    std::string text1;
    std::string text2;
    bool has_text1;
    bool has_text2;
    bool request_like;
};

struct send_sequence_state_t
{
    send_sequence_state_t ();

    bool active;
    send_sequence_spec_t spec;
    zlink::socket_base_t *sink_socket;
    std::unique_ptr<zlink::socket_public_send_scope_t> send_scope;
    std::vector<zlink_msg_t> buffered_parts;
    std::thread::id owner_thread;
};

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
    uint8_t message_type;
    std::string topic_id;
    std::vector<zlink_msg_t> buffered_parts;
    size_t next_part_index;
};

struct handle_state_t
{
    std::mutex mutex;
    std::condition_variable cv;
    send_sequence_state_t send;
    recv_sequence_state_t recv;
};

int validate_send_flags (zlink_send_flags_t flags_);
int validate_part_flag (zlink_part_flag_t part_flag_);
bool routing_id_equals (const zlink_routing_id_t &lhs_, const zlink_routing_id_t &rhs_);
void copy_routing_id (const zlink_routing_id_t *src_, zlink_routing_id_t *dest_);
void consume_send_part (zlink_msg_t *part_);
bool send_spec_equals (const send_sequence_spec_t &lhs_, const send_sequence_spec_t &rhs_);
bool routed_part_debug_enabled ();
void trace_routed_part_prepare_failed (send_family_t family_, int err_);
void trace_routed_part_send_failed (send_family_t family_, bool first_part_, int err_);
void trace_routed_part_first_send (const zlink_routing_id_t &rid_,
                                   zlink_msg_t *part_,
                                   zlink_send_flags_t flags_);
std::shared_ptr<handle_state_t> find_or_create_handle_state (void *handle_);
std::shared_ptr<handle_state_t> find_handle_state (void *handle_);
bool recv_sequence_active (const std::shared_ptr<handle_state_t> &state_);
bool send_sequence_active (void *handle_);
int stage_recv_sequence (const std::shared_ptr<handle_state_t> &state_,
                         recv_family_t family_,
                         zlink::socket_base_t *source_socket_,
                         const zlink_routing_id_t *source_node_rid_,
                         uint64_t request_seq_,
                         zlink_msg_t *parts_,
                         size_t part_count_,
                         std::thread::id owner_thread_);
void set_recv_metadata (recv_sequence_state_t *recv_,
                        const zlink_routing_id_t *source_node_rid_,
                        uint64_t request_seq_);
int buffer_recv_parts (recv_sequence_state_t *recv_, zlink_msg_t *parts_, size_t part_count_);
int take_recv_part (recv_sequence_state_t *recv_,
                    zlink_msg_t *part_out_,
                    zlink_part_flag_t *has_more_out_);
int take_recv_part (const std::shared_ptr<handle_state_t> &state_,
                    zlink_msg_t *part_out_,
                    zlink_part_flag_t *has_more_out_);
void export_recv_metadata (const std::shared_ptr<handle_state_t> &state_,
                           const zlink_routing_id_t **source_node_rid_out_,
                           uint64_t *request_seq_out_);
void reset_send_sequence (send_sequence_state_t *state_);
void reset_recv_sequence (recv_sequence_state_t *state_);
bool aggregate_send_mode_active ();
void set_aggregate_send_mode (bool active_);
int prepare_send_step (void *handle_,
                       const send_sequence_spec_t &spec_,
                       zlink::socket_base_t *sink_socket_,
                       std::shared_ptr<handle_state_t> *state_out_,
                       bool *first_part_out_);
int prepare_staged_send_step (void *handle_,
                              const send_sequence_spec_t &spec_,
                              std::shared_ptr<handle_state_t> *state_out_,
                              bool *first_part_out_);
int stage_staged_send_part (handle_state_t *state_, zlink_msg_t *part_);
int move_staged_parts_for_submit (const std::shared_ptr<handle_state_t> &state_,
                                  zlink_msg_t *part_,
                                  std::vector<zlink_msg_t> *parts_out_);
int prepare_recv_step (void *handle_,
                       recv_family_t family_,
                       zlink::socket_base_t *source_socket_,
                       std::shared_ptr<handle_state_t> *state_out_,
                       bool *first_part_out_,
                       zlink::socket_base_t **active_source_socket_out_);
void complete_send_step (const std::shared_ptr<handle_state_t> &state_,
                         zlink_part_flag_t part_flag_);
void complete_recv_step (const std::shared_ptr<handle_state_t> &state_,
                         zlink_part_flag_t has_more_);
void abort_send_step (const std::shared_ptr<handle_state_t> &state_);
void abort_recv_step (const std::shared_ptr<handle_state_t> &state_);
int reject_if_send_sequence_open (void *handle_);
void invalidate_recv_sequence (void *handle_);
void cleanup_handle (void *handle_);
}
}

#endif
