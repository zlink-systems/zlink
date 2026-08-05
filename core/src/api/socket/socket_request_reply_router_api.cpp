/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>

#include "api/socket/part_helper_internal.hpp"
#include "api/message/recv_result_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_request_reply_runtime_io_helpers.hpp"
#include "api/socket/socket_request_reply_submit_internal.hpp"

namespace reqrep = zlink::socket_reqrep_internal;

namespace
{
struct router_recv_part_metadata_tls_t
{
    zlink_routing_id_t source_node_rid;
};

router_recv_part_metadata_tls_t &router_recv_part_metadata_tls ()
{
    static thread_local router_recv_part_metadata_tls_t metadata;
    return metadata;
}

void export_router_recv_part_metadata_view (const zlink_routing_id_t *source_node_rid_,
                                            uint64_t request_seq_,
                                            const zlink_routing_id_t **source_node_rid_out_,
                                            uint64_t *request_seq_out_)
{
    router_recv_part_metadata_tls_t &metadata = router_recv_part_metadata_tls ();
    zlink::part_helper_internal::copy_routing_id (source_node_rid_, &metadata.source_node_rid);

    if (source_node_rid_out_) {
        *source_node_rid_out_ = source_node_rid_ ? &metadata.source_node_rid : NULL;
    }
    if (request_seq_out_)
        *request_seq_out_ = request_seq_;
}

}

zlink_recv_result_t zlink_router_recv_part (void *router_,
                                            const zlink_routing_id_t **source_node_rid_out_,
                                            uint64_t *request_seq_out_,
                                            zlink_msg_t *part_out_,
                                            zlink_part_flag_t *has_more_out_,
                                            zlink_recv_flags_t flags_)
{
    if (!router_ || !source_node_rid_out_ || !request_seq_out_
        || !part_out_ || !has_more_out_) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }
    if (validate_recv_flags (flags_) != 0)
        return zlink::recv_result_internal::from_errno (errno);
    if (reqrep::validate_socket_type (router_, ZLINK_CORE_SOCKET_ROUTER) != 0)
        return zlink::recv_result_internal::from_errno (errno);

    socket_handle_t handle = as_socket_handle (router_);
    if (!handle.socket)
        return zlink::recv_result_internal::from_errno (EFAULT);

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      zlink::part_helper_internal::find_handle_state (router_);

    const bool recv_sequence_active =
      zlink::part_helper_internal::recv_sequence_active (helper_state);
    auto recv_router_parts_once = [&] (const zlink_routing_id_t **source_node_rid_out,
                                       uint64_t *request_seq_out, zlink_msg_t **parts_out,
                                       size_t *part_count_out) -> zlink_recv_result_t {
        return reqrep::recv_router_message_direct (
                 handle, source_node_rid_out, request_seq_out, parts_out, part_count_out,
                 static_cast<int> (flags_))
                 == 0
               ? ZLINK_RECV_OK
               : zlink::recv_result_internal::from_errno (errno);
    };

    if (!recv_sequence_active) {
        const zlink_routing_id_t *source_node_rid = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t recv_rc = recv_router_parts_once (
          &source_node_rid, &request_seq, &parts, &part_count);
        if (recv_rc != ZLINK_RECV_OK)
            return zlink::recv_result_internal::from_errno (errno);

        if (!parts || part_count == 0) {
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (part_count == 1) {
            if (zlink_msg_move (part_out_, &parts[0]) != 0) {
                zlink_multipart_close (parts, part_count);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_multipart_close (parts, part_count);
            export_router_recv_part_metadata_view (source_node_rid, request_seq,
                                                   source_node_rid_out_, request_seq_out_);
            *has_more_out_ = ZLINK_PART_FINAL;
            return ZLINK_RECV_OK;
        }

        if (!helper_state) {
            helper_state = zlink::part_helper_internal::find_or_create_handle_state (router_);
            if (!helper_state) {
                zlink_multipart_close (parts, part_count);
                return zlink::recv_result_internal::from_errno (errno);
            }
        }

        const int stage_rc = zlink::part_helper_internal::stage_recv_sequence (
          helper_state, zlink::part_helper_internal::recv_family_router,
          handle.socket, source_node_rid, request_seq, parts, part_count,
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
        zlink::part_helper_internal::export_recv_metadata (helper_state, source_node_rid_out_,
                                                           request_seq_out_);
        zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
        return ZLINK_RECV_OK;
    }

    zlink::socket_base_t *source_socket = handle.socket;
    bool first_part = false;
    if (zlink::part_helper_internal::prepare_recv_step (
          router_, zlink::part_helper_internal::recv_family_router, source_socket, &helper_state,
          &first_part, &source_socket)
        != 0) {
        return zlink::recv_result_internal::from_errno (errno);
    }

    if (first_part) {
        const zlink_routing_id_t *source_node_rid = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t recv_rc = recv_router_parts_once (
          &source_node_rid, &request_seq, &parts, &part_count);
        if (recv_rc != ZLINK_RECV_OK) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (!parts || part_count == 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (part_count == 1) {
            if (zlink_msg_move (part_out_, &parts[0]) != 0) {
                zlink_multipart_close (parts, part_count);
                zlink::part_helper_internal::abort_recv_step (helper_state);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_multipart_close (parts, part_count);
            export_router_recv_part_metadata_view (source_node_rid, request_seq,
                                                   source_node_rid_out_, request_seq_out_);
            *has_more_out_ = ZLINK_PART_FINAL;
            zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
            return ZLINK_RECV_OK;
        }

        const int stage_rc = zlink::part_helper_internal::stage_recv_sequence (
          helper_state, zlink::part_helper_internal::recv_family_router, source_socket,
          source_node_rid, request_seq, parts, part_count,
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
    } else {
        if (zlink::part_helper_internal::take_recv_part (helper_state, part_out_, has_more_out_)
            != 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
    }

    zlink::part_helper_internal::export_recv_metadata (helper_state, source_node_rid_out_,
                                                       request_seq_out_);
    zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
    return ZLINK_RECV_OK;
}

zlink_recv_result_t zlink_dealer_recv_part (void *dealer_,
                                            uint8_t *message_type_out_,
                                            uint64_t *request_seq_out_,
                                            zlink_msg_t *part_out_,
                                            zlink_part_flag_t *has_more_out_,
                                            zlink_recv_flags_t flags_)
{
    if (!dealer_ || !message_type_out_ || !request_seq_out_ || !part_out_ || !has_more_out_) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }
    if (validate_recv_flags (flags_) != 0)
        return zlink::recv_result_internal::from_errno (errno);
    if (reqrep::validate_socket_type (dealer_, ZLINK_CORE_SOCKET_DEALER) != 0)
        return zlink::recv_result_internal::from_errno (errno);

    socket_handle_t handle = as_socket_handle (dealer_);
    if (!handle.socket)
        return zlink::recv_result_internal::from_errno (EFAULT);

    std::shared_ptr<reqrep::socket_request_reply_state_t> state =
      reqrep::find_or_create_request_reply_state (handle);
    if (!state)
        return zlink::recv_result_internal::from_errno (errno);
    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      zlink::part_helper_internal::find_handle_state (dealer_);
    const bool recv_sequence_active =
      zlink::part_helper_internal::recv_sequence_active (helper_state);

    auto recv_dealer_parts_once = [&] (uint8_t *message_type_out, uint64_t *request_seq_out,
                                       zlink_msg_t **parts_out,
                                       size_t *part_count_out) -> zlink_recv_result_t {
        return reqrep::recv_dealer_message_direct (
                 handle, state, message_type_out, request_seq_out, parts_out,
                 part_count_out, static_cast<int> (flags_))
                 == 0
               ? ZLINK_RECV_OK
               : zlink::recv_result_internal::from_errno (errno);
    };

    if (!recv_sequence_active) {
        uint8_t message_type = ZLINK_DEALER_MESSAGE_RAW;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t recv_rc =
          recv_dealer_parts_once (&message_type, &request_seq, &parts, &part_count);
        if (recv_rc != ZLINK_RECV_OK)
            return zlink::recv_result_internal::from_errno (errno);

        if (!parts || part_count == 0) {
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (part_count == 1) {
            if (zlink_msg_move (part_out_, &parts[0]) != 0) {
                zlink_multipart_close (parts, part_count);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_multipart_close (parts, part_count);
            *message_type_out_ = message_type;
            *request_seq_out_ = request_seq;
            *has_more_out_ = ZLINK_PART_FINAL;
            return ZLINK_RECV_OK;
        }

        if (!helper_state) {
            helper_state = zlink::part_helper_internal::find_or_create_handle_state (dealer_);
            if (!helper_state) {
                zlink_multipart_close (parts, part_count);
                return zlink::recv_result_internal::from_errno (errno);
            }
        }

        const int stage_rc = zlink::part_helper_internal::stage_recv_sequence (
          helper_state, zlink::part_helper_internal::recv_family_dealer,
          handle.socket, NULL, request_seq, parts, part_count,
          std::this_thread::get_id ());
        zlink_multipart_close (parts, part_count);
        if (stage_rc != 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        {
            std::lock_guard<std::mutex> lock (helper_state->mutex);
            helper_state->recv.message_type = message_type;
        }
        if (zlink::part_helper_internal::take_recv_part (helper_state, part_out_, has_more_out_)
            != 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        *message_type_out_ = message_type;
        *request_seq_out_ = request_seq;
        zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
        return ZLINK_RECV_OK;
    }

    bool first_part = false;
    zlink::socket_base_t *source_socket = handle.socket;
    if (zlink::part_helper_internal::prepare_recv_step (
          dealer_, zlink::part_helper_internal::recv_family_dealer, source_socket, &helper_state,
          &first_part, &source_socket)
        != 0) {
        return zlink::recv_result_internal::from_errno (errno);
    }

    if (first_part) {
        uint8_t message_type = ZLINK_DEALER_MESSAGE_RAW;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t recv_rc =
          recv_dealer_parts_once (&message_type, &request_seq, &parts, &part_count);
        if (recv_rc != ZLINK_RECV_OK) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (!parts || part_count == 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (part_count == 1) {
            if (zlink_msg_move (part_out_, &parts[0]) != 0) {
                zlink_multipart_close (parts, part_count);
                zlink::part_helper_internal::abort_recv_step (helper_state);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_multipart_close (parts, part_count);
            *message_type_out_ = message_type;
            *request_seq_out_ = request_seq;
            *has_more_out_ = ZLINK_PART_FINAL;
            zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
            return ZLINK_RECV_OK;
        }

        const int stage_rc = zlink::part_helper_internal::stage_recv_sequence (
          helper_state, zlink::part_helper_internal::recv_family_dealer, source_socket, NULL,
          request_seq, parts, part_count, std::this_thread::get_id ());
        zlink_multipart_close (parts, part_count);
        if (stage_rc != 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        {
            std::lock_guard<std::mutex> lock (helper_state->mutex);
            helper_state->recv.message_type = message_type;
        }
    }

    if (zlink::part_helper_internal::take_recv_part (helper_state, part_out_, has_more_out_) != 0) {
        zlink::part_helper_internal::abort_recv_step (helper_state);
        return zlink::recv_result_internal::from_errno (errno);
    }
    {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        *message_type_out_ = helper_state->recv.message_type;
        *request_seq_out_ = helper_state->recv.request_seq;
    }
    zlink::part_helper_internal::complete_recv_step (helper_state, *has_more_out_);
    return ZLINK_RECV_OK;
}
