/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <string.h>
#include <new>

#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_message_api_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/message/recv_result_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "core/msg.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/recv_internal.hpp"
#include "core/recv_tls_view.hpp"
#include "core/scoped_msg.hpp"

namespace
{
inline void reset_routing_id_output (zlink_routing_id_t *source_rid_out_)
{
    if (source_rid_out_)
        source_rid_out_->size = 0;
}

int recv_socket_parts (const socket_handle_t &handle_,
                       zlink_routing_id_t *source_rid_out_,
                       zlink_msg_t **parts_out_,
                       size_t *part_count_out_,
                       zlink_send_flags_t flags_)
{
    // Hot path: PAIR single-part public recv reaches here on every message in
    // with_zmq single. Keep its export out of the multipart path.
    if (!handle_.socket) {
        errno = EFAULT;
        return -1;
    }
    if (!parts_out_ || !part_count_out_) {
        errno = EFAULT;
        return -1;
    }
    if (validate_recv_flags (flags_) != 0)
        return -1;

    const int type = socket_type (handle_);
    if (type == ZLINK_CORE_SOCKET_PUB || type == ZLINK_CORE_SOCKET_XPUB
        || type == ZLINK_CORE_SOCKET_SUB || type == ZLINK_CORE_SOCKET_XSUB) {
        errno = ENOTSUP;
        return -1;
    }
    if (type == ZLINK_CORE_SOCKET_ROUTER) {
        errno = EOPNOTSUPP;
        return -1;
    }

    const bool routed_receive = type == ZLINK_CORE_SOCKET_STREAM;
    const bool direct_public_recv_fast =
      routed_receive || (type == ZLINK_CORE_SOCKET_PAIR && !source_rid_out_);

    if (direct_public_recv_fast) {
        zlink_msg_t *first_slot = NULL;
        if (zlink::recv_tls_view::begin_with_first_slot (parts_out_, part_count_out_, &first_slot)
            != 0)
            return -1;

        const int recv_rc =
          routed_receive
            ? zlink::recv_msg_routed_socket (handle_.socket, first_slot,
                                             source_rid_out_, flags_)
            : handle_.socket->recv (
                reinterpret_cast<zlink::msg_t *> (first_slot), flags_);
        if (recv_rc < 0)
            return -1;

        if (!zlink::msg_frame_has_more (*first_slot)) {
            return zlink::recv_tls_view::commit_reserved_single (parts_out_, part_count_out_);
        }

        return zlink::export_reserved_followup_msg_sequence (handle_.socket, parts_out_,
                                                             part_count_out_, false);
    }

    if (zlink::recv_tls_view::begin (parts_out_, part_count_out_) != 0)
        return -1;
    reset_routing_id_output (source_rid_out_);

    zlink_msg_t first;
    zlink_msg_init (&first);
    if (zlink::recv_msg_socket (handle_.socket, type, &first, flags_) < 0) {
        zlink_msg_close (&first);
        return -1;
    }

    if (!zlink::msg_frame_has_more (first)) {
        return zlink::recv_tls_view::export_single (&first, parts_out_, part_count_out_);
    }

    return zlink::export_payload_msg_sequence (
      handle_.socket, &first, parts_out_, part_count_out_, true);
}

} // namespace

int zlink_socket_recv_handle_internal (const socket_handle_t &handle_,
                                       zlink_routing_id_t *source_rid_out_,
                                       zlink_msg_t **parts_out_,
                                       size_t *part_count_out_,
                                       zlink_send_flags_t flags_)
{
    const int rc = recv_socket_parts (
      handle_, source_rid_out_, parts_out_, part_count_out_, flags_);
    if (rc == 0 && parts_out_ && part_count_out_ && *parts_out_
        && *part_count_out_ != 0) {
        // PAIR and DEALER validate continuation metadata before export;
        // STREAM exports one independent RAW chunk per receive.
        zlink::request_reply::clear_request_reply_metadata (
          &(*parts_out_)[0]);
    }
    return rc;
}

static zlink_recv_result_t xpub_recv (void *xpub_,
                                          const zlink_routing_id_t **source_rid_out_,
                                          int *subscribed_out_,
                                          char *topic_id_buf_,
                                          size_t topic_id_capacity_,
                                          size_t *topic_id_len_out_,
                                          zlink_recv_flags_t flags_)
{
    if (!xpub_) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }

    socket_handle_t handle = as_socket_receive_handle (xpub_);
    if (!handle.socket)
        return zlink::recv_result_internal::from_errno (errno);
    handle.socket->clear_last_recv_source_rid ();

    if (!subscribed_out_ || !topic_id_len_out_
        || (topic_id_capacity_ > 0 && !topic_id_buf_)) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }
    if (validate_recv_flags (flags_) != 0)
        return zlink::recv_result_internal::from_errno (errno);
    if (socket_type (handle) != ZLINK_CORE_SOCKET_XPUB) {
        errno = ENOTSUP;
        return zlink::recv_result_internal::from_errno (errno);
    }

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      zlink::part_helper_internal::find_socket_state (handle.socket);
    const bool sequence_active = handle.socket->part_helper_recv_ready ();

    if (!sequence_active) {
        zlink::scoped_msg_t event;
        if (zlink::recv_msg_socket (
              handle.socket, ZLINK_CORE_SOCKET_XPUB, event.get (), flags_)
            < 0)
            return zlink::recv_result_internal::from_errno (errno);

        const unsigned char *data = static_cast<const unsigned char *> (
          zlink_msg_data (event.get ()));
        const size_t size = zlink_msg_size (event.get ());
        const size_t topic_len = size > 0 ? size - 1 : 0;
        const int subscribed = size > 0 && data[0] != 0 ? 1 : 0;

        if (topic_id_capacity_ >= topic_len) {
            if (topic_len > 0)
                memcpy (topic_id_buf_, data + 1, topic_len);
            *topic_id_len_out_ = topic_len;
            *subscribed_out_ = subscribed;
            if (source_rid_out_)
                *source_rid_out_ = handle.socket->last_recv_source_rid_view ();
            errno = 0;
            return ZLINK_RECV_OK;
        }

        if (!helper_state)
            helper_state =
              zlink::part_helper_internal::find_or_create_socket_state (
                handle.socket);
        if (!helper_state)
            return zlink::recv_result_internal::from_errno (errno);

        std::string topic_id;
        try {
            topic_id.assign (topic_len == 0 ? "" : reinterpret_cast<const char *> (data + 1),
                             topic_len);
        }
        catch (const std::bad_alloc &) {
            errno = ENOMEM;
            return zlink::recv_result_internal::from_errno (errno);
        }
        zlink_routing_id_t source_rid;
        const bool has_source_rid = handle.socket->copy_last_recv_source_rid (&source_rid);
        if (zlink::part_helper_internal::stage_recv_sequence (
              helper_state, handle.socket, has_source_rid ? &source_rid : NULL, 0, NULL, 0, 0, 0, 0,
              NULL, &topic_id, subscribed)
            != 0)
            return zlink::recv_result_internal::from_errno (errno);
    }

    zlink::part_helper_internal::recv_record_metadata_t metadata;
    size_t part_count = 0;
    const zlink::part_helper_internal::staged_recv_record_result_t rc =
      zlink::part_helper_internal::try_take_staged_recv_record (
        helper_state, NULL, 0, &part_count, &metadata, topic_id_buf_, topic_id_capacity_,
        topic_id_len_out_, subscribed_out_);
    if (rc != zlink::part_helper_internal::staged_recv_record_taken)
        return zlink::recv_result_internal::from_errno (errno);
    if (!metadata.return_source_rid_as_null)
        handle.socket->store_last_recv_source_rid (&metadata.source_node_rid);
    if (source_rid_out_)
        *source_rid_out_ =
          metadata.return_source_rid_as_null ? NULL : handle.socket->last_recv_source_rid_view ();
    errno = 0;
    return ZLINK_RECV_OK;
}

zlink_recv_result_t zlink_xpub_recv (
  void *xpub_, const zlink_routing_id_t **source_rid_out_,
  int *subscribed_out_, char *topic_id_buf_, size_t topic_id_capacity_,
  size_t *topic_id_len_out_, zlink_recv_flags_t flags_)
{
    return xpub_recv (xpub_, source_rid_out_, subscribed_out_, topic_id_buf_,
                       topic_id_capacity_, topic_id_len_out_, flags_);
}
