/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/monitoring/poller_api_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/message/recv_result_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_completion_queue_internal.hpp"
#include "core/msg.hpp"
#include "sockets/stream/stream.hpp"
#include "utils/allocator.hpp"

namespace
{
bool empty_routing_id (const zlink_routing_id_t &rid_)
{
    if (rid_.size != 0)
        return false;
    for (size_t i = 0; i < sizeof (rid_.data); ++i) {
        if (rid_.data[i] != 0)
            return false;
    }
    return true;
}

bool empty_completion (const zlink_completion_t &completion_)
{
    return completion_.struct_size == sizeof (zlink_completion_t)
           && completion_.kind == static_cast<zlink_completion_kind_t> (0)
           && completion_.completion_id == 0
           && completion_.user_context == NULL
           && empty_routing_id (completion_.peer_rid)
           && completion_.send_result
                == static_cast<zlink_send_complete_result_t> (0)
           && completion_.send_terminal_errno == 0
           && completion_.request_result
                == static_cast<zlink_request_result_t> (0)
           && completion_.reply_parts == NULL
           && completion_.reply_part_count == 0;
}

bool initialized_empty_message (zlink_msg_t *message_)
{
    if (!message_)
        return false;
    const zlink::msg_t *message =
      reinterpret_cast<const zlink::msg_t *> (message_);
    return message->check () && message->size () == 0;
}
}

zlink_recv_result_t zlink_stream_recv_packet (
  void *stream_, const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *header_out_, zlink_msg_t *body_out_, zlink_recv_flags_t flags_)
{
    if (!stream_) {
        errno = EFAULT;
        return ZLINK_RECV_INVALID_HANDLE;
    }

    socket_handle_t handle = as_socket_handle (stream_);
    if (!handle.socket)
        return zlink::recv_result_internal::from_errno (errno);

    // A borrowed RID is invalidated by every data-recv entry on this socket,
    // including calls that fail later validation. Do not, however, overwrite
    // the caller's output variable unless a packet is actually returned.
    handle.socket->clear_last_recv_source_rid ();

    if (!header_out_ || !body_out_) {
        errno = EFAULT;
        return ZLINK_RECV_INVALID_HANDLE;
    }
    if (validate_recv_flags (flags_) != 0)
        return zlink::recv_result_internal::from_errno (errno);
    if (header_out_ == body_out_ || !initialized_empty_message (header_out_)
        || !initialized_empty_message (body_out_)) {
        errno = EINVAL;
        return ZLINK_RECV_INVALID_STATE;
    }

    if (socket_type (handle) != ZLINK_CORE_SOCKET_STREAM) {
        errno = ENOTSUP;
        return ZLINK_RECV_NOT_SUPPORTED;
    }

    zlink_routing_id_t source_rid;
    memset (&source_rid, 0, sizeof (source_rid));
    if (static_cast<zlink::stream_t *> (handle.socket)
          ->recv_packet (&source_rid,
                         reinterpret_cast<zlink::msg_t *> (header_out_),
                         reinterpret_cast<zlink::msg_t *> (body_out_),
                         static_cast<int> (flags_))
        != 0)
        return zlink::recv_result_internal::from_errno (errno);

    handle.socket->store_last_recv_source_rid (&source_rid);
    if (source_rid_out_)
        *source_rid_out_ = handle.socket->last_recv_source_rid_view ();
    return ZLINK_RECV_OK;
}

zlink_recv_result_t zlink_completion_recv (
  void *s_, zlink_completion_t *completion_out_, zlink_recv_flags_t flags_)
{
    if (!s_ || !completion_out_) {
        errno = EFAULT;
        return ZLINK_RECV_INVALID_HANDLE;
    }
    if (!empty_completion (*completion_out_)) {
        errno = EINVAL;
        return ZLINK_RECV_INVALID_STATE;
    }
    if (validate_recv_flags (flags_) != 0)
        return zlink::recv_result_internal::from_errno (errno);

    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return zlink::recv_result_internal::from_errno (errno);
    const int type = socket_type (handle);
    if (type != ZLINK_CORE_SOCKET_PAIR && type != ZLINK_CORE_SOCKET_DEALER
        && type != ZLINK_CORE_SOCKET_ROUTER
        && type != ZLINK_CORE_SOCKET_STREAM) {
        errno = ENOTSUP;
        return ZLINK_RECV_NOT_SUPPORTED;
    }

    // A DONTWAIT pull is itself a socket progress point.  Drain commands
    // before driving pending SEND records so a physical detach/reconnect can
    // replace the pipe behind the same logical target.  If context shutdown
    // was observed, prefer an already-published completion; otherwise surface
    // the lifecycle result with the caller output still empty.
    const int command_progress_rc = handle.socket->process_submit_commands ();
    const int command_progress_errno = errno;
    handle.socket->drive_send_pending ();
    if (command_progress_rc != 0
        && !zlink::socket_completion::has_ready (
          &handle.socket->completion_runtime ())) {
        errno = command_progress_errno;
        return zlink::recv_result_internal::from_errno (errno);
    }

    // Pending SEND admission and wire REQUEST replies need a command owner
    // even when the application uses blocking pull without a poller.
    if (!zlink::socket_completion::has_ready (
          &handle.socket->completion_runtime ())
        && flags_ != ZLINK_RECV_FLAGS_DONTWAIT
        && handle.socket->receive_timeout_ms () != 0) {
        const int progress_rc = handle.socket->ensure_completion_processing ();
        if (progress_rc != 0 && errno != EAGAIN)
            return zlink::recv_result_internal::from_errno (errno);
    }

    if (zlink::socket_completion::recv (
          &handle.socket->completion_runtime (), completion_out_, flags_,
          handle.socket->receive_timeout_ms ()) != 0)
        return zlink::recv_result_internal::from_errno (errno);

    return ZLINK_RECV_OK;
}

void zlink_completion_close (zlink_completion_t *completion_)
{
    if (!completion_
        || completion_->struct_size != sizeof (zlink_completion_t))
        return;

    if (completion_->reply_parts) {
        zlink_multipart_close (completion_->reply_parts,
                               completion_->reply_part_count);
        zlink::dealloc (completion_->reply_parts);
    }

    memset (completion_, 0, sizeof (*completion_));
    completion_->struct_size = sizeof (zlink_completion_t);
}
