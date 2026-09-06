/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <string.h>

#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_message_api_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/message/recv_result_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
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

int recv_socket_subscribe_parts (const socket_handle_t &handle_,
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

    zlink::scoped_msg_t topic_frame;
    const int topic_rc = handle_.socket->recv (
      reinterpret_cast<zlink::msg_t *> (topic_frame.get ()), flags_);
    if (topic_rc < 0) {
        return -1;
    }
    // Topic metadata is consumed by Core and is never caller-visible.

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

    const int first_rc = zlink::recv_followup_msg_socket (handle_.socket, first_slot);
    if (first_rc < 0) {
        zlink::recv_tls_view::abort ();
        return -1;
    }

    if (!zlink::msg_frame_has_more (*first_slot)) {
        return zlink::recv_tls_view::commit_reserved_single (parts_out_, part_count_out_);
    }

    return zlink::export_reserved_followup_msg_sequence (handle_.socket, parts_out_,
                                                         part_count_out_, false);
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

    // DEALER raw receive still applies record-structure validation: only the
    // first application part may carry a request/reply kind. The direct raw
    // request/reply reader clears a first-part kind without creating a reply
    // token, and rejects a later kind consistently with the wire decoder.
    if (type == ZLINK_CORE_SOCKET_DEALER) {
        reset_routing_id_output (source_rid_out_);
        return zlink::socket_reqrep_internal::recv_dealer_message_direct (
          handle_, parts_out_, part_count_out_, static_cast<int> (flags_));
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
    if (!xpub_) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }

    socket_handle_t handle = as_socket_handle (xpub_);
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
    bool sequence_active = false;
    if (helper_state) {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        sequence_active = helper_state->recv.active;
        if (sequence_active
            && (helper_state->recv.family
                  != zlink::part_helper_internal::recv_family_xpub
                || helper_state->recv.owner_thread
                     != std::this_thread::get_id ())) {
            errno = EBUSY;
            return zlink::recv_result_internal::from_errno (errno);
        }
    }

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

        bool first_part = false;
        zlink::socket_base_t *source_socket = NULL;
        if (zlink::part_helper_internal::prepare_recv_step (
              zlink::part_helper_internal::recv_family_xpub,
              handle.socket, helper_state, &first_part, &source_socket)
            != 0)
            return zlink::recv_result_internal::from_errno (errno);
        if (!first_part) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EBUSY;
            return zlink::recv_result_internal::from_errno (errno);
        }

        try {
            std::lock_guard<std::mutex> lock (helper_state->mutex);
            helper_state->recv.topic_id.assign (
              topic_len == 0
                ? ""
                : reinterpret_cast<const char *> (data + 1),
              topic_len);
            helper_state->recv.subscribed = subscribed;
            zlink_routing_id_t source_rid;
            const bool has_source_rid =
              handle.socket->copy_last_recv_source_rid (&source_rid);
            zlink::part_helper_internal::set_recv_metadata (
              &helper_state->recv, has_source_rid ? &source_rid : NULL, 0);
        } catch (...) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = ENOMEM;
            return zlink::recv_result_internal::from_errno (errno);
        }
    }

    size_t topic_len = 0;
    int subscribed = 0;
    zlink_routing_id_t source_rid;
    bool has_source_rid = false;
    {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        topic_len = helper_state->recv.topic_id.size ();
        if (topic_id_capacity_ < topic_len) {
            *topic_id_len_out_ = topic_len;
            errno = ENOBUFS;
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (topic_len > 0)
            memcpy (topic_id_buf_, helper_state->recv.topic_id.data (),
                    topic_len);
        subscribed = helper_state->recv.subscribed;
        has_source_rid = !helper_state->recv.return_source_rid_as_null;
        if (has_source_rid)
            source_rid = helper_state->recv.source_node_rid;
    }

    if (has_source_rid)
        handle.socket->store_last_recv_source_rid (&source_rid);
    *topic_id_len_out_ = topic_len;
    *subscribed_out_ = subscribed;
    if (source_rid_out_)
        *source_rid_out_ = has_source_rid
                             ? handle.socket->last_recv_source_rid_view ()
                             : NULL;
    zlink::part_helper_internal::complete_recv_step (
      helper_state, ZLINK_PART_FINAL);
    errno = 0;
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
    return zlink_socket_recv_handle_internal (handle, source_rid_out_, parts_out_,
                                              part_count_out_, flags_);
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
    const int rc = recv_socket_subscribe_parts (
      handle, source_rid_out_, parts_out_, part_count_out_, topic_id_out_,
      topic_id_len_out_, flags_);
    if (rc == 0 && parts_out_ && part_count_out_ && *parts_out_
        && *part_count_out_ != 0)
        // XSUB rejects metadata after the topic frame, before any payload is
        // exported. Keep the first public payload boundary explicit.
        zlink::request_reply::clear_request_reply_metadata (&(*parts_out_)[0]);
    return rc;
}
