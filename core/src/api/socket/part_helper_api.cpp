/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>

#include "api/socket/part_helper_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/pipe.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/routing_id.hpp"

static int
buffer_recv_parts (zlink::part_helper_internal::recv_sequence_state_t *, zlink_msg_t *, size_t);

namespace
{
void publish_buffered_recv_readiness (
  zlink::part_helper_internal::recv_sequence_state_t *state_)
{
    if (!state_ || !state_->source_socket)
        return;

    state_->source_socket->set_part_helper_recv_ready (state_->active);
}

}

zlink::part_helper_internal::recv_sequence_state_t::recv_sequence_state_t () :
    active (false),
    source_socket (NULL),
    return_source_rid_as_null (true),
    request_seq (0),
    transport_pair_id (0),
    transport_pair_generation (0),
    route_generation (0),
    route_source_pipe (NULL),
    subscribed (0),
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
  const std::shared_ptr<handle_state_t> &state_,
  zlink_msg_t *parts_out_,
  size_t parts_capacity_,
  size_t *part_count_out_,
  recv_record_metadata_t *metadata_out_,
  char *topic_id_out_,
  size_t topic_id_capacity_,
  size_t *topic_id_len_out_,
  int *subscribed_out_)
{
    if (!state_)
        return staged_recv_record_none;
    if (!part_count_out_ || !metadata_out_) {
        errno = EFAULT;
        return staged_recv_record_error;
    }

    socket_base_t *held_socket = NULL;
    {
        std::lock_guard<std::mutex> lock (state_->mutex);
        recv_sequence_state_t &recv = state_->recv;
        if (!recv.active)
            return staged_recv_record_none;
        metadata_out_->return_source_rid_as_null =
          recv.return_source_rid_as_null;
        metadata_out_->source_node_rid = recv.source_node_rid;
        metadata_out_->request_seq = recv.request_seq;
        metadata_out_->transport_pair_id = recv.transport_pair_id;
        metadata_out_->transport_pair_generation =
          recv.transport_pair_generation;
        metadata_out_->route_generation = recv.route_generation;

        const size_t part_count = recv.buffered_parts.size ();
        if (parts_capacity_ < part_count
            || (topic_id_len_out_ && topic_id_capacity_ < recv.topic_id.size ())) {
            *part_count_out_ = part_count;
            if (topic_id_len_out_)
                *topic_id_len_out_ = recv.topic_id.size ();
            errno = ENOBUFS;
            return staged_recv_record_error;
        }

        zlink_assert (parts_out_ || part_count == 0);
        if (topic_id_len_out_) {
            *topic_id_len_out_ = recv.topic_id.size ();
            if (!recv.topic_id.empty ())
                memcpy (topic_id_out_, recv.topic_id.data (), recv.topic_id.size ());
        }
        if (subscribed_out_)
            *subscribed_out_ = recv.subscribed;

        // Caller slots are uninitialized (whole-message recv does not require
        // init), so adopt rather than move: adopt overwrites without inspecting
        // or closing the destination, avoiding an uninitialized read.
        for (size_t i = 0; i < part_count; ++i) {
            const int adopt_rc =
              zlink_msg_adopt (&parts_out_[i], &recv.buffered_parts[i]);
            errno_assert (adopt_rc == 0);
        }
        *part_count_out_ = part_count;
        metadata_out_->route_source_pipe = recv.route_source_pipe;
        recv.route_source_pipe = NULL;
        held_socket = reset_recv_sequence (&recv);
    }
    if (held_socket)
        held_socket->end_public_part_receive_delivery_hold ();
    errno = 0;
    return staged_recv_record_taken;
}

int zlink::part_helper_internal::stage_recv_sequence (const std::shared_ptr<handle_state_t> &state_,
                                                      zlink::socket_base_t *source_socket_,
                                                      const zlink_routing_id_t *source_node_rid_,
                                                      uint64_t request_seq_,
                                                      zlink_msg_t *parts_,
                                                      size_t part_count_,
                                                      uint64_t transport_pair_id_,
                                                      uint64_t transport_pair_generation_,
                                                      uint64_t route_generation_,
                                                      zlink::pipe_t *route_source_pipe_,
                                                      std::string *topic_id_,
                                                      int subscribed_)
{
    if (!state_ || (part_count_ != 0 && !parts_) || (part_count_ == 0 && !topic_id_)) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    zlink_assert (!state_->recv.active);

    state_->recv.active = true;
    state_->recv.source_socket = source_socket_;
    state_->recv.return_source_rid_as_null = source_node_rid_ == NULL;
    copy_routing_id (source_node_rid_, &state_->recv.source_node_rid);
    state_->recv.request_seq = request_seq_;
    state_->recv.transport_pair_id = transport_pair_id_;
    state_->recv.transport_pair_generation =
      transport_pair_generation_;
    state_->recv.route_generation = route_generation_;
    if (part_count_ != 0 && buffer_recv_parts (&state_->recv, parts_, part_count_) != 0) {
        const int saved_errno = errno;
        reset_recv_sequence (&state_->recv);
        errno = saved_errno;
        return -1;
    }
    // The caller transfers its receive-turn pin only after staging succeeds.
    state_->recv.route_source_pipe = route_source_pipe_;
    if (topic_id_)
        state_->recv.topic_id.swap (*topic_id_);
    state_->recv.subscribed = subscribed_;
    publish_buffered_recv_readiness (&state_->recv);
    return 0;
}

static int buffer_recv_parts (zlink::part_helper_internal::recv_sequence_state_t *recv_,
                              zlink_msg_t *parts_,
                              size_t part_count_)
{
    if (!recv_ || !parts_ || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    try {
        recv_->buffered_parts.resize (part_count_);
    }
    catch (const std::bad_alloc &) {
        errno = ENOMEM;
        return -1;
    }
    catch (const std::length_error &) {
        errno = ENOMEM;
        return -1;
    }
    for (size_t i = 0; i < part_count_; ++i)
        zlink_msg_init (&recv_->buffered_parts[i]);

    for (size_t i = 0; i < part_count_; ++i) {
        if (zlink_msg_move (&recv_->buffered_parts[i], &parts_[i]) != 0) {
            for (size_t j = 0; j < i; ++j)
                zlink_msg_move (&parts_[j], &recv_->buffered_parts[j]);
            recv_->buffered_parts.clear ();
            errno = EFAULT;
            return -1;
        }
    }

    return 0;
}

zlink::socket_base_t *zlink::part_helper_internal::reset_recv_sequence (
  recv_sequence_state_t *state_, recv_reset_cleanup_t *cleanup_)
{
    if (!state_)
        return NULL;

    socket_base_t *const held_socket =
      state_->public_delivery_hold ? state_->source_socket : NULL;

    state_->active = false;
    publish_buffered_recv_readiness (state_);

    if (cleanup_)
        cleanup_->parts.take_from (&state_->buffered_parts);
    else {
        for (size_t i = 0; i < state_->buffered_parts.size (); ++i)
            zlink_msg_close (&state_->buffered_parts[i]);
        state_->buffered_parts.clear ();
    }

    state_->source_socket = NULL;
    state_->return_source_rid_as_null = true;
    copy_routing_id (NULL, &state_->source_node_rid);
    state_->request_seq = 0;
    if (state_->route_source_pipe) {
        if (cleanup_)
            cleanup_->route_source_pipe = state_->route_source_pipe;
        else
            state_->route_source_pipe->release_lifetime_ref ();
        state_->route_source_pipe = NULL;
    }
    state_->transport_pair_id = 0;
    state_->transport_pair_generation = 0;
    state_->route_generation = 0;
    state_->subscribed = 0;
    state_->topic_id.clear ();
    state_->public_delivery_hold = false;
    return held_socket;
}

void zlink::part_helper_internal::finish_recv_reset_cleanup (
  recv_reset_cleanup_t *cleanup_)
{
    if (!cleanup_)
        return;
    for (size_t i = 0; i < cleanup_->parts.size (); ++i)
        zlink_msg_close (&cleanup_->parts[i]);
    cleanup_->parts.clear ();
    if (cleanup_->route_source_pipe) {
        cleanup_->route_source_pipe->release_lifetime_ref ();
        cleanup_->route_source_pipe = NULL;
    }
}
