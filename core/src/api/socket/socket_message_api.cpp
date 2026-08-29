/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_message_api_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/message/recv_result_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_request_reply_runtime_io_helpers.hpp"
#include "core/recv_internal.hpp"

namespace
{
zlink_routing_id_t &recv_part_source_rid_tls ()
{
    static thread_local zlink_routing_id_t rid;
    return rid;
}

void close_buffered_recv_parts (
  zlink::part_helper_internal::recv_part_buffer_t *parts_)
{
    if (!parts_)
        return;
    for (size_t i = 0; i < parts_->size (); ++i)
        zlink_msg_close (&(*parts_)[i]);
    parts_->clear ();
}

void discard_subscribe_payload_tail (zlink::socket_base_t *socket_)
{
    while (socket_) {
        zlink::msg_t part;
        const int init_rc = part.init ();
        errno_assert (init_rc == 0);
        if (socket_->recv (&part, ZLINK_DONTWAIT) != 0) {
            const int close_rc = part.close ();
            errno_assert (close_rc == 0);
            return;
        }
        const bool more = (part.flags () & zlink::msg_t::more) != 0;
        const int close_rc = part.close ();
        errno_assert (close_rc == 0);
        if (!more)
            return;
    }
}

}

zlink_recv_result_t zlink_recv_part (void *s_,
                                     const zlink_routing_id_t **source_rid_out_,
                                     zlink_msg_t *part_out_,
                                     zlink_part_flag_t *has_more_out_,
                                     zlink_recv_flags_t flags_)
{
    if (!s_ || !part_out_ || !has_more_out_) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }
    if (validate_recv_flags (flags_) != 0)
        return zlink::recv_result_internal::from_errno (errno);

    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::recv_result_internal::from_errno (errno);

    const int type = socket_type (handle);
    const bool expose_source_rid = type == ZLINK_CORE_SOCKET_STREAM;
    std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t> request_state;
    const bool dealer_request_surface = type == ZLINK_CORE_SOCKET_DEALER;
    // A blocking generic DEALER receive participates in the same transport
    // ownership transition as the typed receive API. Establish the internal
    // request/reply state before it waits, so a concurrent request remains on
    // the paired transport rather than creating a separate payload queue.
    if (dealer_request_surface)
        request_state = zlink::socket_reqrep_internal::find_or_create_request_reply_state (handle);
    else if (handle.socket->has_request_reply_state ())
        request_state = handle.socket->request_reply_state ();
    if (dealer_request_surface && !request_state)
        return zlink::recv_result_internal::from_errno (errno);
    zlink::socket_base_t *recv_source_socket = handle.socket;
    auto recv_parts_once = [&] (zlink_routing_id_t *source_rid_,
                                zlink_msg_t **parts_,
                                size_t *part_count_,
                                zlink_recv_flags_t recv_flags_,
                                zlink_msg_t *terminal_part_out_,
                                bool *terminal_part_returned_out_) -> int {
        if (dealer_request_surface) {
            uint8_t message_type = 0;
            uint64_t request_seq = 0;
            if (source_rid_)
                source_rid_->size = 0;
            return zlink::socket_reqrep_internal::recv_dealer_message_direct (
              handle, request_state, &message_type, &request_seq, parts_, part_count_,
              static_cast<int> (recv_flags_), terminal_part_out_,
              terminal_part_returned_out_);
        }
        if (terminal_part_returned_out_)
            *terminal_part_returned_out_ = false;
        return zlink_socket_recv_handle_internal (
          handle, source_rid_, parts_, part_count_,
          static_cast<zlink_send_flags_t> (recv_flags_));
    };
    if (type == ZLINK_CORE_SOCKET_PUB || type == ZLINK_CORE_SOCKET_XPUB
        || type == ZLINK_CORE_SOCKET_SUB || type == ZLINK_CORE_SOCKET_XSUB
        || type == ZLINK_CORE_SOCKET_ROUTER) {
        errno = ENOTSUP;
        return zlink::recv_result_internal::from_errno (errno);
    }
    if (type == ZLINK_CORE_SOCKET_STREAM
        && handle.socket->stream_mark_raw_part_receive () != 0)
        return zlink::recv_result_internal::from_errno (errno);

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> existing_state =
      handle.socket->has_part_helper_state ()
        ? handle.socket->part_helper_state ()
        : std::shared_ptr<zlink::part_helper_internal::handle_state_t> ();
    const bool recv_sequence_active =
      zlink::part_helper_internal::recv_sequence_active (existing_state);

    if (!recv_sequence_active) {
        zlink_routing_id_t source_rid;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        bool terminal_part_returned = false;
        source_rid.size = 0;
        if (recv_parts_once (&source_rid, &parts, &part_count, flags_,
                             part_out_, &terminal_part_returned)
            != 0)
            return zlink::recv_result_internal::from_errno (errno);

        if (terminal_part_returned) {
            if (source_rid_out_)
                *source_rid_out_ = NULL;
            *has_more_out_ = ZLINK_PART_FINAL;
            return ZLINK_RECV_OK;
        }

        if (!parts || part_count == 0) {
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (part_count == 1) {
            zlink_routing_id_t &recv_part_source_rid = recv_part_source_rid_tls ();
            zlink::socket_reqrep_internal::assign_routing_id_compact (&recv_part_source_rid,
                                                                       source_rid);
            if (zlink_msg_move (part_out_, &parts[0]) != 0) {
                zlink_multipart_close (parts, part_count);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_multipart_close (parts, part_count);
            if (source_rid_out_) {
                if (expose_source_rid) {
                    *source_rid_out_ =
                      recv_part_source_rid.size == 0 ? NULL : &recv_part_source_rid;
                } else {
                    *source_rid_out_ = NULL;
                }
            }
            *has_more_out_ = ZLINK_PART_FINAL;
            return ZLINK_RECV_OK;
        }

        std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
          existing_state ? existing_state
                         : zlink::part_helper_internal::find_or_create_socket_state (
                             handle.socket);
        if (!helper_state) {
            zlink_multipart_close (parts, part_count);
            return zlink::recv_result_internal::from_errno (errno);
        }

        const int stage_rc = zlink::part_helper_internal::stage_recv_sequence (
          helper_state, zlink::part_helper_internal::recv_family_basic, recv_source_socket,
          expose_source_rid ? &source_rid : NULL, 0, parts, part_count,
          std::this_thread::get_id ());
        zlink_multipart_close (parts, part_count);
        if (stage_rc != 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (helper_state->recv.buffered_parts.empty ()) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (zlink::part_helper_internal::take_recv_part (helper_state, part_out_, has_more_out_)
            != 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (source_rid_out_) {
            zlink::part_helper_internal::export_recv_metadata (helper_state, source_rid_out_, NULL);
        }
        zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
        return ZLINK_RECV_OK;
    }

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      existing_state;
    if (!helper_state)
        return zlink::recv_result_internal::from_errno (errno);

    bool first_part = false;
    zlink::socket_base_t *source_socket = NULL;
    if (zlink::part_helper_internal::prepare_recv_step (
          s_, zlink::part_helper_internal::recv_family_basic, recv_source_socket, &helper_state,
          &first_part, &source_socket)
        != 0) {
        return zlink::recv_result_internal::from_errno (errno);
    }

    if (first_part) {
        zlink_routing_id_t source_rid;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        memset (&source_rid, 0, sizeof (source_rid));
        if (recv_parts_once (&source_rid, &parts, &part_count, flags_, NULL,
                             NULL)
            != 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (!parts || part_count == 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (part_count == 1) {
            {
                std::lock_guard<std::mutex> lock (helper_state->mutex);
                helper_state->recv.source_node_rid = source_rid;
                helper_state->recv.return_source_rid_as_null =
                  helper_state->recv.source_node_rid.size == 0;
            }
            if (zlink_msg_move (part_out_, &parts[0]) != 0) {
                zlink_multipart_close (parts, part_count);
                zlink::part_helper_internal::abort_recv_step (helper_state);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_multipart_close (parts, part_count);
            if (source_rid_out_) {
                std::lock_guard<std::mutex> lock (helper_state->mutex);
                *source_rid_out_ = helper_state->recv.return_source_rid_as_null
                                     ? NULL
                                     : &helper_state->recv.source_node_rid;
            }
            *has_more_out_ = ZLINK_PART_FINAL;
            zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
            return ZLINK_RECV_OK;
        }

        const int stage_rc = zlink::part_helper_internal::stage_recv_sequence (
          helper_state, zlink::part_helper_internal::recv_family_basic, recv_source_socket,
          &source_rid, 0, parts, part_count, std::this_thread::get_id ());
        zlink_multipart_close (parts, part_count);
        if (stage_rc != 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (helper_state->recv.buffered_parts.empty ()) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (zlink::part_helper_internal::take_recv_part (helper_state, part_out_, has_more_out_)
            != 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
    } else {
        if (zlink::part_helper_internal::take_recv_part (helper_state, part_out_, has_more_out_)
            != 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
    }

    if (source_rid_out_) {
        zlink::part_helper_internal::export_recv_metadata (helper_state, source_rid_out_, NULL);
    }
    zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
    return ZLINK_RECV_OK;
}

zlink_recv_result_t zlink_subscribe_part (void *subject_,
                                          const zlink_routing_id_t **source_rid_out_,
                                          char *topic_id_buf_,
                                          size_t topic_id_capacity_,
                                          size_t *topic_id_len_out_,
                                          zlink_msg_t *part_out_,
                                          zlink_part_flag_t *has_more_out_,
                                          zlink_recv_flags_t flags_)
{
    if (!subject_ || !topic_id_len_out_ || !part_out_ || !has_more_out_) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }
    if (topic_id_capacity_ > 0 && !topic_id_buf_) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }
    if (validate_recv_flags (flags_) != 0)
        return zlink::recv_result_internal::from_errno (errno);

    socket_handle_t handle = as_socket_handle (subject_);
    if (!handle.socket)
        return zlink::recv_result_internal::from_errno (errno);

    const int type = socket_type (handle);
    if (type != ZLINK_CORE_SOCKET_SUB && type != ZLINK_CORE_SOCKET_XSUB) {
        errno = ENOTSUP;
        return zlink::recv_result_internal::from_errno (errno);
    }

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      zlink::part_helper_internal::find_socket_state (handle.socket);
    bool sequence_active = false;
    if (helper_state) {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        sequence_active = helper_state->recv.active;
        if (sequence_active
            && (helper_state->recv.family != zlink::part_helper_internal::recv_family_subscribe
                || helper_state->recv.owner_thread != std::this_thread::get_id ())) {
            errno = EINVAL;
            return zlink::recv_result_internal::from_errno (errno);
        }
    }

    if (!sequence_active) {
        zlink_msg_t topic_frame;
        zlink_msg_init (&topic_frame);
        if (handle.socket->recv (reinterpret_cast<zlink::msg_t *> (&topic_frame), flags_) != 0) {
            zlink_msg_close (&topic_frame);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }

        const bool topic_has_more =
          (reinterpret_cast<const zlink::msg_t *> (&topic_frame)->flags () & zlink::msg_t::more)
          != 0;
        std::string topic_id;
        try {
            topic_id.assign (
              static_cast<const char *> (zlink_msg_data (&topic_frame)),
              zlink_msg_size (&topic_frame));
        } catch (...) {
            zlink_msg_close (&topic_frame);
            if (topic_has_more)
                discard_subscribe_payload_tail (handle.socket);
            errno = ENOMEM;
            return zlink::recv_result_internal::from_errno (errno);
        }
        zlink_msg_close (&topic_frame);

        if (!topic_has_more) {
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        zlink_msg_t first_payload;
        zlink_msg_init (&first_payload);
        if (handle.socket->recv (
              reinterpret_cast<zlink::msg_t *> (&first_payload), flags_)
            != 0) {
            zlink_msg_close (&first_payload);
            return zlink::recv_result_internal::from_errno (errno);
        }

        const bool first_payload_has_more =
          (reinterpret_cast<const zlink::msg_t *> (&first_payload)->flags ()
           & zlink::msg_t::more)
          != 0;
        if (!first_payload_has_more && topic_id_capacity_ >= topic_id.size ()
            && topic_id_capacity_ != 0) {
            // A terminal payload has no continuation state to own. Return it
            // directly and reserve the buffered sequence machinery for actual
            // multipart subscriptions.
            *topic_id_len_out_ = topic_id.size ();
            if (!topic_id.empty ())
                memcpy (topic_id_buf_, topic_id.data (), topic_id.size ());
            if (zlink_msg_move (part_out_, &first_payload) != 0) {
                zlink_msg_close (&first_payload);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            if (source_rid_out_)
                *source_rid_out_ = NULL;
            *has_more_out_ = ZLINK_PART_FINAL;
            return ZLINK_RECV_OK;
        }

        zlink::part_helper_internal::recv_part_buffer_t buffered_parts;
        try {
            buffered_parts.resize (1);
        } catch (...) {
            zlink_msg_close (&first_payload);
            if (first_payload_has_more)
                discard_subscribe_payload_tail (handle.socket);
            errno = ENOMEM;
            return zlink::recv_result_internal::from_errno (errno);
        }
        zlink_msg_init (&buffered_parts[0]);
        if (zlink_msg_move (&buffered_parts[0], &first_payload) != 0) {
            zlink_msg_close (&first_payload);
            zlink_msg_close (&buffered_parts[0]);
            errno = EFAULT;
            return zlink::recv_result_internal::from_errno (errno);
        }
        bool payload_has_more = first_payload_has_more;
        while (payload_has_more) {
            try {
                buffered_parts.resize (buffered_parts.size () + 1);
            } catch (...) {
                close_buffered_recv_parts (&buffered_parts);
                discard_subscribe_payload_tail (handle.socket);
                errno = ENOMEM;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_msg_t &slot = buffered_parts.back ();
            zlink_msg_init (&slot);
            if (handle.socket->recv (reinterpret_cast<zlink::msg_t *> (&slot), flags_) != 0) {
                const int saved_errno = errno;
                close_buffered_recv_parts (&buffered_parts);
                errno = saved_errno;
                return zlink::recv_result_internal::from_errno (errno);
            }

            payload_has_more =
              (reinterpret_cast<const zlink::msg_t *> (&slot)->flags () & zlink::msg_t::more) != 0;
        }

        if (!helper_state)
            helper_state = zlink::part_helper_internal::find_or_create_socket_state (
              handle.socket);
        if (!helper_state) {
            const int saved_errno = errno;
            close_buffered_recv_parts (&buffered_parts);
            errno = saved_errno;
            return zlink::recv_result_internal::from_errno (errno);
        }

        bool first_part = false;
        zlink::socket_base_t *source_socket = NULL;
        if (zlink::part_helper_internal::prepare_recv_step (
              subject_, zlink::part_helper_internal::recv_family_subscribe, handle.socket,
              &helper_state, &first_part, &source_socket)
            != 0) {
            const int saved_errno = errno;
            close_buffered_recv_parts (&buffered_parts);
            errno = saved_errno;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (!first_part) {
            close_buffered_recv_parts (&buffered_parts);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EINVAL;
            return zlink::recv_result_internal::from_errno (errno);
        }

        int buffer_rc = 0;
        {
            std::lock_guard<std::mutex> lock (helper_state->mutex);
            helper_state->recv.topic_id.swap (topic_id);
            buffer_rc = zlink::part_helper_internal::buffer_recv_parts (
              &helper_state->recv, buffered_parts.data (), buffered_parts.size ());
        }
        close_buffered_recv_parts (&buffered_parts);
        if (buffer_rc != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = saved_errno;
            return zlink::recv_result_internal::from_errno (errno);
        }
    }

    int copy_errno = 0;
    {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        *topic_id_len_out_ = helper_state->recv.topic_id.size ();
        if (topic_id_capacity_ == 0
            || topic_id_capacity_ < helper_state->recv.topic_id.size ())
            copy_errno = ENOBUFS;
        else if (!helper_state->recv.topic_id.empty ())
            memcpy (topic_id_buf_, helper_state->recv.topic_id.data (),
                    helper_state->recv.topic_id.size ());
    }
    if (copy_errno != 0) {
        errno = copy_errno;
        return zlink::recv_result_internal::from_errno (errno);
    }

    if (zlink::part_helper_internal::take_recv_part (
          helper_state, part_out_, has_more_out_)
        != 0) {
        zlink::part_helper_internal::abort_recv_step (helper_state);
        return zlink::recv_result_internal::from_errno (errno);
    }
    if (source_rid_out_)
        *source_rid_out_ = NULL;
    zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
    return ZLINK_RECV_OK;
}
