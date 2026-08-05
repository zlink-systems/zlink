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
    if (!handle.socket) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }

    const int type = socket_type (handle);
    const bool expose_source_rid = type == ZLINK_CORE_SOCKET_STREAM;
    std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t> request_state;
    const bool dealer_request_surface = type == ZLINK_CORE_SOCKET_DEALER;
    if (dealer_request_surface) {
        request_state =
          zlink::socket_reqrep_internal::find_or_create_request_reply_state (handle);
        if (!request_state)
            return zlink::recv_result_internal::from_errno (errno);
    } else {
        request_state = zlink::socket_reqrep_internal::find_request_reply_state (handle);
    }
    zlink::socket_base_t *recv_source_socket = handle.socket;
    auto recv_parts_once = [&] (zlink_routing_id_t *source_rid_,
                                zlink_msg_t **parts_,
                                size_t *part_count_,
                                zlink_recv_flags_t recv_flags_) -> int {
        if (dealer_request_surface) {
            uint8_t message_type = 0;
            uint64_t request_seq = 0;
            if (source_rid_)
                source_rid_->size = 0;
            return zlink::socket_reqrep_internal::recv_dealer_message_direct (
              handle, request_state, &message_type, &request_seq, parts_, part_count_,
              static_cast<int> (recv_flags_));
        }
        return zlink_socket_recv_internal (
          s_, source_rid_, parts_, part_count_,
          static_cast<zlink_send_flags_t> (recv_flags_));
    };
    if (type == ZLINK_CORE_SOCKET_PUB || type == ZLINK_CORE_SOCKET_XPUB
        || type == ZLINK_CORE_SOCKET_SUB || type == ZLINK_CORE_SOCKET_XSUB
        || type == ZLINK_CORE_SOCKET_ROUTER) {
        errno = ENOTSUP;
        return zlink::recv_result_internal::from_errno (errno);
    }

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> existing_state =
      zlink::part_helper_internal::find_handle_state (s_);
    const bool recv_sequence_active =
      zlink::part_helper_internal::recv_sequence_active (existing_state);

    if (!recv_sequence_active) {
        zlink_routing_id_t source_rid;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        source_rid.size = 0;
        if (recv_parts_once (&source_rid, &parts, &part_count, flags_) != 0)
            return zlink::recv_result_internal::from_errno (errno);

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
          zlink::part_helper_internal::find_or_create_handle_state (s_);
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
      zlink::part_helper_internal::find_or_create_handle_state (s_);
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
        if (recv_parts_once (&source_rid, &parts, &part_count, flags_)
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
    if (validate_recv_flags (flags_) != 0)
        return zlink::recv_result_internal::from_errno (errno);

    socket_handle_t handle = as_socket_handle (subject_);
    if (!handle.socket) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      zlink::part_helper_internal::find_or_create_handle_state (subject_);
    if (!helper_state)
        return zlink::recv_result_internal::from_errno (errno);

    const int type = socket_type (handle);
    if (type != ZLINK_CORE_SOCKET_SUB && type != ZLINK_CORE_SOCKET_XSUB) {
        errno = ENOTSUP;
        return zlink::recv_result_internal::from_errno (errno);
    }

    bool sequence_active = false;
    {
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

        std::string topic_id (static_cast<const char *> (zlink_msg_data (&topic_frame)),
                              zlink_msg_size (&topic_frame));
        const bool topic_has_more =
          (reinterpret_cast<const zlink::msg_t *> (&topic_frame)->flags () & zlink::msg_t::more)
          != 0;
        zlink_msg_close (&topic_frame);

        if (!topic_has_more) {
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        std::vector<zlink_msg_t> buffered_parts;
        while (true) {
            buffered_parts.resize (buffered_parts.size () + 1);
            zlink_msg_t &slot = buffered_parts.back ();
            zlink_msg_init (&slot);
            if (handle.socket->recv (reinterpret_cast<zlink::msg_t *> (&slot), flags_) != 0) {
                const int saved_errno = errno;
                for (size_t i = 0; i < buffered_parts.size (); ++i)
                    zlink_msg_close (&buffered_parts[i]);
                errno = saved_errno;
                return zlink::recv_result_internal::from_errno (errno);
            }

            const bool has_more =
              (reinterpret_cast<const zlink::msg_t *> (&slot)->flags () & zlink::msg_t::more) != 0;
            if (!has_more)
                break;
        }

        bool first_part = false;
        zlink::socket_base_t *source_socket = NULL;
        if (zlink::part_helper_internal::prepare_recv_step (
              subject_, zlink::part_helper_internal::recv_family_subscribe, handle.socket,
              &helper_state, &first_part, &source_socket)
            != 0) {
            const int saved_errno = errno;
            for (size_t i = 0; i < buffered_parts.size (); ++i)
                zlink_msg_close (&buffered_parts[i]);
            errno = saved_errno;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (!first_part) {
            for (size_t i = 0; i < buffered_parts.size (); ++i)
                zlink_msg_close (&buffered_parts[i]);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EINVAL;
            return zlink::recv_result_internal::from_errno (errno);
        }

        {
            std::lock_guard<std::mutex> lock (helper_state->mutex);
            helper_state->recv.topic_id.swap (topic_id);
            helper_state->recv.buffered_parts.swap (buffered_parts);
            helper_state->recv.next_part_index = 1;
        }
        if (zlink_msg_move (part_out_, &helper_state->recv.buffered_parts[0]) != 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EFAULT;
            return zlink::recv_result_internal::from_errno (errno);
        }
    } else {
        bool range_failed = false;
        bool move_failed = false;
        {
            std::lock_guard<std::mutex> lock (helper_state->mutex);
            if (helper_state->recv.next_part_index >= helper_state->recv.buffered_parts.size ()) {
                range_failed = true;
            } else {
                move_failed =
                  zlink_msg_move (
                    part_out_,
                    &helper_state->recv.buffered_parts[helper_state->recv.next_part_index])
                  != 0;
                if (!move_failed)
                    ++helper_state->recv.next_part_index;
            }
        }
        if (range_failed) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (move_failed) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EFAULT;
            return zlink::recv_result_internal::from_errno (errno);
        }
    }

    int copy_errno = 0;
    {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        *topic_id_len_out_ = helper_state->recv.topic_id.size ();
        if (topic_id_capacity_ > 0) {
            if (!topic_id_buf_) {
                copy_errno = EFAULT;
            } else if (topic_id_capacity_ < helper_state->recv.topic_id.size ()) {
                copy_errno = EMSGSIZE;
            } else if (!helper_state->recv.topic_id.empty ()) {
                memcpy (topic_id_buf_, helper_state->recv.topic_id.data (),
                        helper_state->recv.topic_id.size ());
            }
        }
    }

    if (source_rid_out_)
        *source_rid_out_ = NULL;
    {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        *has_more_out_ =
          (helper_state->recv.next_part_index < helper_state->recv.buffered_parts.size ())
            ? ZLINK_PART_MORE
            : ZLINK_PART_FINAL;
    }
    zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
    if (copy_errno != 0) {
        errno = copy_errno;
        return zlink::recv_result_internal::from_errno (errno);
    }
    return ZLINK_RECV_OK;
}
