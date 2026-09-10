/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_PART_HELPER_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_PART_HELPER_INTERNAL_HPP_INCLUDED__

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "sockets/common/socket_runtime.hpp"
#include "api/socket/inline_msg_buffer_internal.hpp"
#include "api/message/submit_result_internal.hpp"
#include <zlink.h>

enum zlink_part_flag_t
{
    ZLINK_PART_FINAL = 0,
    ZLINK_PART_MORE = 1
};

namespace zlink
{
class socket_base_t;

namespace part_helper_internal
{
enum recv_family_t
{
    recv_family_none = 0,
    recv_family_basic,
    recv_family_subscribe,
    recv_family_xpub,
    recv_family_router
};

// Keep common short send records inline. Larger multipart records retain the
// same ownership model and spill through inline_msg_buffer_t's dynamic path.
const size_t inline_send_part_capacity = 4;
typedef zlink::socket_internal::inline_msg_buffer_t<inline_send_part_capacity>
  send_part_buffer_t;

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
    recv_sequence_state_t recv;
};

struct recv_record_metadata_t
{
    bool return_source_rid_as_null;
    zlink_routing_id_t source_node_rid;
    uint64_t request_seq;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
};

enum staged_recv_record_result_t
{
    staged_recv_record_error = -1,
    staged_recv_record_none = 0,
    staged_recv_record_taken = 1
};

int validate_send_flags (zlink_send_flags_t flags_);
void copy_routing_id (const zlink_routing_id_t *src_, zlink_routing_id_t *dest_);
void consume_send_part (zlink_msg_t *part_);
std::shared_ptr<handle_state_t> find_or_create_socket_state (zlink::socket_base_t *socket_);
std::shared_ptr<handle_state_t> find_socket_state (zlink::socket_base_t *socket_);
staged_recv_record_result_t try_take_staged_recv_record (
  const std::shared_ptr<handle_state_t> &state_,
  recv_family_t family_,
  zlink_msg_t *parts_out_,
  size_t parts_capacity_,
  size_t *part_count_out_,
  recv_record_metadata_t *metadata_out_);
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
zlink::socket_base_t *reset_recv_sequence (recv_sequence_state_t *state_);
int prepare_recv_step (recv_family_t family_,
                       zlink::socket_base_t *source_socket_,
                       const std::shared_ptr<handle_state_t> &state_,
                       bool *first_part_out_,
                       zlink::socket_base_t **active_source_socket_out_);
void complete_recv_step (const std::shared_ptr<handle_state_t> &state_,
                         zlink_part_flag_t has_more_);
void abort_recv_step (const std::shared_ptr<handle_state_t> &state_);
void cleanup_socket (zlink::socket_base_t *socket_);

int validate_whole_send (zlink::socket_base_t *socket_, zlink_msg_t *parts_,
                         size_t part_count_, int argument_errno_);

template <typename SubmitRecord>
zlink_submit_result_t submit_whole_record (
  zlink::socket_base_t *socket_, zlink_msg_t *parts_, size_t part_count_,
  int argument_errno_, SubmitRecord submit_record_)
{
    const zlink_submit_result_t result =
      validate_whole_send (socket_, parts_, part_count_, argument_errno_) != 0
        ? zlink::submit_result_internal::from_errno (errno)
        : submit_record_ ();
    const int saved_errno = errno;
    if (parts_)
        for (size_t i = 0; i < part_count_; ++i)
            consume_send_part (&parts_[i]);
    errno = saved_errno;
    return result;
}

}
}

#endif
