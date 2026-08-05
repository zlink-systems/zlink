/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <string.h>
#include <stdlib.h>

#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_message_api_internal.hpp"
#include "core/msg.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/recv_internal.hpp"
#include "core/recv_tls_view.hpp"
#include "core/scoped_msg.hpp"

namespace
{
zlink_routing_id_t &xpub_recv_source_rid_tls ()
{
    static thread_local zlink_routing_id_t rid;
    return rid;
}

inline void reset_routing_id_output (zlink_routing_id_t *source_rid_out_)
{
    if (source_rid_out_)
        source_rid_out_->size = 0;
}

bool is_direct_public_recv_fast_type (int type_)
{
    return type_ == ZLINK_CORE_SOCKET_PAIR || type_ == ZLINK_CORE_SOCKET_DEALER;
}

int recv_socket_subscribe_parts (socket_handle_t handle_,
                                 zlink_routing_id_t *source_rid_out_,
                                 zlink_msg_t **parts_out_,
                                 size_t *part_count_out_,
                                 char *topic_id_out_,
                                 size_t *topic_id_len_out_,
                                 zlink_send_flags_t flags_)
{
    if (!handle_.socket) {
        errno = EFAULT;
        return -1;
    }
    if (!parts_out_ || !part_count_out_ || !topic_id_len_out_) {
        errno = EFAULT;
        return -1;
    }
    if (validate_recv_flags (flags_) != 0)
        return -1;

    zlink_msg_t *first_slot = NULL;
    if (zlink::recv_tls_view::begin_with_first_slot (parts_out_, part_count_out_, &first_slot) != 0)
        return -1;
    reset_routing_id_output (source_rid_out_);

    const int type = socket_type (handle_);
    if (type != ZLINK_CORE_SOCKET_SUB && type != ZLINK_CORE_SOCKET_XSUB) {
        errno = ENOTSUP;
        return -1;
    }

    if (handle_.socket->sub_dispatch_active ()) {
        errno = EBUSY;
        return -1;
    }

    zlink::scoped_msg_t topic_frame;
    if (handle_.socket->recv (reinterpret_cast<zlink::msg_t *> (topic_frame.get ()), flags_) < 0) {
        return -1;
    }

    if (zlink::copy_bytes_to_sized_output (
          static_cast<const char *> (zlink_msg_data (topic_frame.get ())),
          zlink_msg_size (topic_frame.get ()), topic_id_out_, topic_id_len_out_)
        != 0) {
        return -1;
    }

    if (!zlink::msg_frame_has_more (topic_frame.ref ())) {
        errno = 0;
        return 0;
    }

    if (zlink::recv_followup_msg_socket (handle_.socket, first_slot) < 0) {
        zlink::recv_tls_view::abort ();
        return -1;
    }

    if (!zlink::msg_frame_has_more (*first_slot))
        return zlink::recv_tls_view::commit_reserved_single (parts_out_, part_count_out_);

    return zlink::export_reserved_followup_msg_sequence (handle_.socket, parts_out_,
                                                         part_count_out_, false);
}

int recv_socket_parts (socket_handle_t handle_,
                       zlink_routing_id_t *source_rid_out_,
                       zlink_msg_t **parts_out_,
                       size_t *part_count_out_,
                       zlink_send_flags_t flags_)
{
    // Hot path: PAIR/DEALER single-part public recv reaches here on every
    // message in with_zmq single. Keep single-part export lean and do not
    // accidentally fold it back into a heavier multipart path.
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

    const bool routed_router_payload = type == ZLINK_CORE_SOCKET_ROUTER && source_rid_out_ != NULL;
    const bool strip_recv_routing_id = type == ZLINK_CORE_SOCKET_STREAM || routed_router_payload;
    const bool direct_public_recv_fast =
      !strip_recv_routing_id && !source_rid_out_ && is_direct_public_recv_fast_type (type);

    if (direct_public_recv_fast) {
        zlink_msg_t *first_slot = NULL;
        if (zlink::recv_tls_view::begin_with_first_slot (parts_out_, part_count_out_, &first_slot)
            != 0)
            return -1;

        if (handle_.socket->socket_msg_dispatch_active ()) {
            errno = EBUSY;
            return -1;
        }

        if (handle_.socket->recv (reinterpret_cast<zlink::msg_t *> (first_slot), flags_) < 0)
            return -1;

        if (!zlink::msg_frame_has_more (*first_slot))
            return zlink::recv_tls_view::commit_reserved_single (parts_out_, part_count_out_);

        return zlink::export_reserved_followup_msg_sequence (handle_.socket, parts_out_,
                                                             part_count_out_, false);
    }

    if (zlink::recv_tls_view::begin (parts_out_, part_count_out_) != 0)
        return -1;
    reset_routing_id_output (source_rid_out_);

    zlink_msg_t first;
    zlink_msg_init (&first);
    if (type == ZLINK_CORE_SOCKET_ROUTER && source_rid_out_) {
        if (zlink::recv_msg_routed_socket (handle_.socket, &first, source_rid_out_, flags_) < 0) {
            zlink_msg_close (&first);
            return -1;
        }
    } else if (zlink::recv_msg_socket (handle_.socket, type, &first, flags_) < 0) {
        zlink_msg_close (&first);
        return -1;
    }

    if (type == ZLINK_CORE_SOCKET_STREAM && source_rid_out_)
        handle_.socket->copy_last_recv_source_rid (source_rid_out_);

    if (!zlink::msg_frame_has_more (first)) {
        if (strip_recv_routing_id && !routed_router_payload) {
            zlink_msg_close (&first);
            errno = 0;
            return 0;
        }
        return zlink::recv_tls_view::export_single (&first, parts_out_, part_count_out_);
    }

    if (strip_recv_routing_id) {
        if (type == ZLINK_CORE_SOCKET_STREAM && source_rid_out_)
            zlink::copy_routing_id_from_msg (first, source_rid_out_);

        if (!routed_router_payload) {
            zlink_msg_close (&first);

            zlink_msg_t payload;
            zlink_msg_init (&payload);
            if (zlink::recv_followup_msg_socket (handle_.socket, &payload) < 0) {
                zlink_msg_close (&payload);
                return -1;
            }
            return zlink::export_payload_msg_sequence (handle_.socket, &payload, parts_out_,
                                                       part_count_out_, true);
        }
    }

    return zlink::export_payload_msg_sequence (handle_.socket, &first, parts_out_, part_count_out_,
                                               true);
}

} // namespace

extern "C" int zlink_socket_xpub_recv_internal (void *socket_,
                                                zlink_routing_id_t *source_rid_out_,
                                                int *subscribed_out_,
                                                char *topic_id_out_,
                                                size_t *topic_id_len_,
                                                zlink_send_flags_t flags_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket)
        return -1;
    if (validate_recv_flags (flags_) != 0)
        return -1;
    if (!subscribed_out_ || !topic_id_len_) {
        errno = EFAULT;
        return -1;
    }
    if (!topic_id_out_ && *topic_id_len_ != 0) {
        errno = EFAULT;
        return -1;
    }
    if (socket_type (handle) != ZLINK_CORE_SOCKET_XPUB) {
        errno = EINVAL;
        return -1;
    }

    zlink::scoped_msg_t msg;
    if (zlink::recv_msg_socket (handle.socket, ZLINK_CORE_SOCKET_XPUB, msg.get (), flags_) < 0) {
        return -1;
    }

    if (source_rid_out_) {
        reset_routing_id_output (source_rid_out_);
        handle.socket->copy_last_recv_source_rid (source_rid_out_);
    }

    const unsigned char *data = static_cast<const unsigned char *> (zlink_msg_data (msg.get ()));
    const size_t size = zlink_msg_size (msg.get ());
    const size_t topic_len = size > 0 ? size - 1 : 0;
    *subscribed_out_ = size > 0 && data[0] != 0 ? 1 : 0;

    if (*topic_id_len_ < topic_len) {
        *topic_id_len_ = topic_len;
        errno = EMSGSIZE;
        return -1;
    }

    if (topic_id_out_ && topic_len > 0)
        memcpy (topic_id_out_, data + 1, topic_len);
    *topic_id_len_ = topic_len;

    errno = 0;
    return 0;
}

zlink_recv_result_t zlink_xpub_recv_part (void *xpub_,
                                          const zlink_routing_id_t **source_rid_out_,
                                          int *subscribed_out_,
                                          char *topic_id_buf_,
                                          size_t topic_id_capacity_,
                                          size_t *topic_id_len_out_,
                                          zlink_recv_flags_t flags_)
{
    if (!subscribed_out_ || !topic_id_len_out_) {
        errno = EFAULT;
        return ZLINK_RECV_INVALID_HANDLE;
    }
    *topic_id_len_out_ = topic_id_capacity_;
    zlink_routing_id_t tmp_rid;
    tmp_rid.size = 0;
    const int rc =
      zlink_socket_xpub_recv_internal (xpub_, &tmp_rid, subscribed_out_, topic_id_buf_,
                                       topic_id_len_out_, static_cast<zlink_send_flags_t> (flags_));
    if (rc != 0) {
        const int err = zlink_errno ();
        if (err == EAGAIN)
            return ZLINK_RECV_NO_DATA;
        if (err == ETERM)
            return ZLINK_RECV_TERMINATED;
        return ZLINK_RECV_INTERNAL_ERROR;
    }
    if (source_rid_out_) {
        zlink_routing_id_t &source_rid = xpub_recv_source_rid_tls ();
        source_rid = tmp_rid;
        *source_rid_out_ = &source_rid;
    }
    return ZLINK_RECV_OK;
}

extern "C" int zlink_socket_recv_internal (void *socket_,
                                           zlink_routing_id_t *source_rid_out_,
                                           zlink_msg_t **parts_out_,
                                           size_t *part_count_out_,
                                           zlink_send_flags_t flags_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket)
        return -1;
    return recv_socket_parts (handle, source_rid_out_, parts_out_, part_count_out_, flags_);
}

extern "C" int zlink_socket_subscribe_recv_internal (void *socket_,
                                                     zlink_routing_id_t *source_rid_out_,
                                                     zlink_msg_t **parts_out_,
                                                     size_t *part_count_out_,
                                                     char *topic_id_out_,
                                                     size_t *topic_id_len_out_,
                                                     zlink_send_flags_t flags_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket)
        return -1;
    return recv_socket_subscribe_parts (handle, source_rid_out_, parts_out_, part_count_out_,
                                        topic_id_out_, topic_id_len_out_, flags_);
}
