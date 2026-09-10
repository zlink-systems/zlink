/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>
#include <mutex>
#include <new>

#include "api/socket/part_helper_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "core/c_api_copy_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/routing_id.hpp"

namespace
{
void publish_buffered_recv_readiness (
  zlink::part_helper_internal::recv_sequence_state_t *state_)
{
    if (!state_ || !state_->source_socket)
        return;

    state_->source_socket->set_part_helper_recv_ready (
      state_->active
      && state_->next_part_index < state_->buffered_parts.size ());
}

}

zlink::part_helper_internal::recv_sequence_state_t::recv_sequence_state_t () :
    active (false),
    family (recv_family_none),
    source_socket (NULL),
    owner_thread (),
    return_source_rid_as_null (true),
    request_seq (0),
    transport_pair_id (0),
    transport_pair_generation (0),
    subscribed (0),
    next_part_index (0),
    public_delivery_hold (false)
{
    memset (&source_node_rid, 0, sizeof (source_node_rid));
}

int zlink::part_helper_internal::validate_send_flags (zlink_send_flags_t flags_)
{
    if (flags_ != 0 && flags_ != ZLINK_DONTWAIT) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int zlink::part_helper_internal::adopt_recv_public_delivery_hold (
  const std::shared_ptr<handle_state_t> &state_)
{
    if (!state_) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (!state_->recv.active || !state_->recv.source_socket
        || state_->recv.public_delivery_hold) {
        errno = EINVAL;
        return -1;
    }
    state_->recv.public_delivery_hold = true;
    return 0;
}

void zlink::part_helper_internal::copy_routing_id (const zlink_routing_id_t *src_,
                                                   zlink_routing_id_t *dest_)
{
    if (!dest_)
        return;

    if (!src_) {
        memset (dest_, 0, sizeof (*dest_));
        return;
    }

    zlink::copy_routing_id_from_bytes (src_->data, src_->size, dest_);
}

void zlink::part_helper_internal::consume_send_part (zlink_msg_t *part_)
{
    zlink::request_reply::consume_send_frame (part_);
}

int zlink::part_helper_internal::validate_whole_send (
  zlink::socket_base_t *socket_, zlink_msg_t *parts_, size_t part_count_,
  int argument_errno_)
{
    if (!parts_ || argument_errno_ != 0) {
        errno = !parts_ ? EFAULT : argument_errno_;
        return -1;
    }
    if (part_count_ == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!socket_)
        return -1;
    return 0;
}



zlink::part_helper_internal::staged_recv_record_result_t
zlink::part_helper_internal::try_take_staged_recv_record (
  const std::shared_ptr<handle_state_t> &state_, recv_family_t family_,
  zlink_msg_t *parts_out_, size_t parts_capacity_, size_t *part_count_out_,
  recv_record_metadata_t *metadata_out_)
{
    if (!state_)
        return staged_recv_record_none;
    if (!parts_out_ || !part_count_out_ || !metadata_out_) {
        errno = EFAULT;
        return staged_recv_record_error;
    }

    socket_base_t *held_socket = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        recv_sequence_state_t &recv = state_->recv;
        if (!recv.active)
            return staged_recv_record_none;
        if (recv.family != family_
            || recv.owner_thread != std::this_thread::get_id ()
            || recv.next_part_index != 0) {
            errno = EBUSY;
            return staged_recv_record_error;
        }

        const size_t part_count = recv.buffered_parts.size ();
        if (parts_capacity_ < part_count) {
            *part_count_out_ = part_count;
            errno = ENOBUFS;
            return staged_recv_record_error;
        }

        metadata_out_->return_source_rid_as_null =
          recv.return_source_rid_as_null;
        metadata_out_->source_node_rid = recv.source_node_rid;
        metadata_out_->request_seq = recv.request_seq;
        metadata_out_->transport_pair_id = recv.transport_pair_id;
        metadata_out_->transport_pair_generation =
          recv.transport_pair_generation;
        // Caller slots are uninitialized (whole-message recv does not require
        // init), so adopt rather than move: adopt overwrites without inspecting
        // or closing the destination, avoiding an uninitialized read.
        for (size_t i = 0; i < part_count; ++i) {
            const int adopt_rc =
              zlink_msg_adopt (&parts_out_[i], &recv.buffered_parts[i]);
            errno_assert (adopt_rc == 0);
        }
        *part_count_out_ = part_count;
        held_socket = reset_recv_sequence (&recv);
    }
    if (held_socket)
        held_socket->end_public_part_receive_delivery_hold ();
    errno = 0;
    return staged_recv_record_taken;
}

int zlink::part_helper_internal::stage_recv_sequence (const std::shared_ptr<handle_state_t> &state_,
                                                      recv_family_t family_,
                                                      zlink::socket_base_t *source_socket_,
                                                      const zlink_routing_id_t *source_node_rid_,
                                                      uint64_t request_seq_,
                                                      zlink_msg_t *parts_,
                                                      size_t part_count_,
                                                      std::thread::id owner_thread_,
                                                      uint64_t transport_pair_id_,
                                                      uint64_t transport_pair_generation_)
{
    if (!state_ || !parts_ || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->recv.active) {
        errno = EINVAL;
        return -1;
    }

    state_->recv.active = true;
    state_->recv.family = family_;
    state_->recv.source_socket = source_socket_;
    state_->recv.owner_thread = owner_thread_;
    set_recv_metadata (&state_->recv, source_node_rid_, request_seq_);
    state_->recv.transport_pair_id = transport_pair_id_;
    state_->recv.transport_pair_generation =
      transport_pair_generation_;
    if (buffer_recv_parts (&state_->recv, parts_, part_count_) != 0) {
        const int saved_errno = errno;
        reset_recv_sequence (&state_->recv);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

void zlink::part_helper_internal::set_recv_metadata (recv_sequence_state_t *recv_,
                                                     const zlink_routing_id_t *source_node_rid_,
                                                     uint64_t request_seq_)
{
    if (!recv_)
        return;

    recv_->return_source_rid_as_null = source_node_rid_ == NULL;
    copy_routing_id (source_node_rid_, &recv_->source_node_rid);
    recv_->request_seq = request_seq_;
}

int zlink::part_helper_internal::buffer_recv_parts (recv_sequence_state_t *recv_,
                                                    zlink_msg_t *parts_,
                                                    size_t part_count_)
{
    if (!recv_ || !parts_ || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    try {
        recv_->buffered_parts.resize (part_count_);
    } catch (...) {
        recv_->next_part_index = 0;
        errno = ENOMEM;
        return -1;
    }
    recv_->next_part_index = 0;
    for (size_t i = 0; i < part_count_; ++i)
        zlink_msg_init (&recv_->buffered_parts[i]);

    for (size_t i = 0; i < part_count_; ++i) {
        if (zlink_msg_move (&recv_->buffered_parts[i], &parts_[i]) != 0) {
            for (size_t j = 0; j < recv_->buffered_parts.size (); ++j)
                zlink_msg_close (&recv_->buffered_parts[j]);
            recv_->buffered_parts.clear ();
            recv_->next_part_index = 0;
            errno = EFAULT;
            return -1;
        }
    }

    publish_buffered_recv_readiness (recv_);
    return 0;
}

int zlink::part_helper_internal::take_recv_part (recv_sequence_state_t *recv_,
                                                 zlink_msg_t *part_out_,
                                                 zlink_part_flag_t *has_more_out_)
{
    if (!recv_ || !part_out_ || !has_more_out_) {
        errno = EFAULT;
        return -1;
    }
    if (recv_->next_part_index >= recv_->buffered_parts.size ()) {
        errno = EPROTO;
        return -1;
    }
    if (zlink_msg_move (part_out_, &recv_->buffered_parts[recv_->next_part_index]) != 0) {
        errno = EFAULT;
        return -1;
    }
    ++recv_->next_part_index;
    *has_more_out_ =
      recv_->next_part_index < recv_->buffered_parts.size () ? ZLINK_PART_MORE : ZLINK_PART_FINAL;
    publish_buffered_recv_readiness (recv_);
    return 0;
}

int zlink::part_helper_internal::take_recv_part (const std::shared_ptr<handle_state_t> &state_,
                                                 zlink_msg_t *part_out_,
                                                 zlink_part_flag_t *has_more_out_)
{
    if (!state_) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    return take_recv_part (&state_->recv, part_out_, has_more_out_);
}

int zlink::part_helper_internal::take_recv_part (
  const std::shared_ptr<handle_state_t> &state_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *request_seq_out_,
  uint64_t *transport_pair_id_out_,
  uint64_t *transport_pair_generation_out_)
{
    if (!state_) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (take_recv_part (&state_->recv, part_out_, has_more_out_) != 0)
        return -1;
    if (source_node_rid_out_) {
        *source_node_rid_out_ = state_->recv.return_source_rid_as_null
                                  ? NULL
                                  : &state_->recv.source_node_rid;
    }
    if (request_seq_out_)
        *request_seq_out_ = state_->recv.request_seq;
    if (transport_pair_id_out_)
        *transport_pair_id_out_ = state_->recv.transport_pair_id;
    if (transport_pair_generation_out_)
        *transport_pair_generation_out_ =
          state_->recv.transport_pair_generation;
    return 0;
}

zlink::socket_base_t *zlink::part_helper_internal::reset_recv_sequence (
  recv_sequence_state_t *state_)
{
    if (!state_)
        return NULL;

    socket_base_t *const held_socket =
      state_->public_delivery_hold ? state_->source_socket : NULL;

    state_->active = false;
    publish_buffered_recv_readiness (state_);

    for (size_t i = 0; i < state_->buffered_parts.size (); ++i)
        zlink_msg_close (&state_->buffered_parts[i]);
    state_->buffered_parts.clear ();
    state_->next_part_index = 0;

    state_->family = recv_family_none;
    state_->source_socket = NULL;
    state_->owner_thread = std::thread::id ();
    state_->return_source_rid_as_null = true;
    copy_routing_id (NULL, &state_->source_node_rid);
    state_->request_seq = 0;
    state_->transport_pair_id = 0;
    state_->transport_pair_generation = 0;
    state_->subscribed = 0;
    state_->topic_id.clear ();
    state_->public_delivery_hold = false;
    return held_socket;
}

int zlink::part_helper_internal::prepare_recv_step (
  recv_family_t family_,
  zlink::socket_base_t *source_socket_,
  const std::shared_ptr<handle_state_t> &state_,
  bool *first_part_out_,
  zlink::socket_base_t **active_source_socket_out_)
{
    if (!state_ || !first_part_out_ || !active_source_socket_out_) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    const std::thread::id current_thread = std::this_thread::get_id ();

    if (!state_->recv.active) {
        state_->recv.active = true;
        state_->recv.family = family_;
        state_->recv.source_socket = source_socket_;
        state_->recv.owner_thread = current_thread;
        state_->recv.return_source_rid_as_null = true;
        state_->recv.request_seq = 0;
        state_->recv.transport_pair_id = 0;
        state_->recv.transport_pair_generation = 0;
        state_->recv.topic_id.clear ();
        memset (&state_->recv.source_node_rid, 0,
                sizeof (state_->recv.source_node_rid));
        *first_part_out_ = true;
    } else {
        if (state_->recv.family != family_
            || state_->recv.owner_thread != current_thread) {
            errno = EBUSY;
            return -1;
        }
        *first_part_out_ = false;
    }

    *active_source_socket_out_ = state_->recv.source_socket;
    return 0;
}

void zlink::part_helper_internal::complete_recv_step (const std::shared_ptr<handle_state_t> &state_,
                                                      zlink_part_flag_t has_more_)
{
    if (!state_ || has_more_ != ZLINK_PART_FINAL)
        return;

    socket_base_t *held_socket = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        held_socket = reset_recv_sequence (&state_->recv);
    }
    if (held_socket)
        held_socket->end_public_part_receive_delivery_hold ();
}

void zlink::part_helper_internal::abort_recv_step (const std::shared_ptr<handle_state_t> &state_)
{
    if (!state_)
        return;

    socket_base_t *held_socket = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        held_socket = reset_recv_sequence (&state_->recv);
    }
    if (held_socket)
        held_socket->end_public_part_receive_delivery_hold ();
}
