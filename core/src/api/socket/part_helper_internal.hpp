/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_PART_HELPER_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_PART_HELPER_INTERNAL_HPP_INCLUDED__

#include <memory>
#include <mutex>
#include <string>

#include "sockets/common/socket_runtime.hpp"
#include "api/socket/inline_msg_buffer_internal.hpp"
#include "api/message/submit_result_internal.hpp"
#include <zlink.h>

namespace zlink
{
class socket_base_t;
class pipe_t;

namespace part_helper_internal
{
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
    zlink::socket_base_t *source_socket;
    bool return_source_rid_as_null;
    zlink_routing_id_t source_node_rid;
    uint64_t request_seq;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    uint64_t route_generation;
    zlink::pipe_t *route_source_pipe;
    int subscribed;
    std::string topic_id;
    recv_part_buffer_t buffered_parts;
    bool public_delivery_hold;
};

struct recv_reset_cleanup_t
{
    recv_reset_cleanup_t () : route_source_pipe (NULL) {}
    recv_part_buffer_t parts;
    zlink::pipe_t *route_source_pipe;
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
    uint64_t route_generation;
    zlink::pipe_t *route_source_pipe;
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
staged_recv_record_result_t
try_take_staged_recv_record (const std::shared_ptr<handle_state_t> &state_,
                             zlink_msg_t *parts_out_,
                             size_t parts_capacity_,
                             size_t *part_count_out_,
                             recv_record_metadata_t *metadata_out_,
                             char *topic_id_out_ = NULL,
                             size_t topic_id_capacity_ = 0,
                             size_t *topic_id_len_out_ = NULL,
                             int *subscribed_out_ = NULL);
int stage_recv_sequence (const std::shared_ptr<handle_state_t> &state_,
                         zlink::socket_base_t *source_socket_,
                         const zlink_routing_id_t *source_node_rid_,
                         uint64_t request_seq_,
                         zlink_msg_t *parts_,
                         size_t part_count_,
                         uint64_t transport_pair_id_ = 0,
                         uint64_t transport_pair_generation_ = 0,
                         uint64_t route_generation_ = 0,
                         zlink::pipe_t *route_source_pipe_ = NULL,
                         std::string *topic_id_ = NULL,
                         int subscribed_ = 0);
int adopt_recv_public_delivery_hold (
  const std::shared_ptr<handle_state_t> &state_);
zlink::socket_base_t *reset_recv_sequence (
  recv_sequence_state_t *state_, recv_reset_cleanup_t *cleanup_ = NULL);
void finish_recv_reset_cleanup (recv_reset_cleanup_t *cleanup_);
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
