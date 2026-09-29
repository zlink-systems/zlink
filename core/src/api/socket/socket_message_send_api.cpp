/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <new>

#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_message_api_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/message/submit_result_internal.hpp"
#include "core/msg.hpp"
#include "core/multipart_send_txn.hpp"
#include "utils/routing_id.hpp"

namespace
{
int validate_send_parts (zlink_msg_t *parts_, size_t part_count_)
{
    if ((!parts_ && part_count_ > 0) || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    return 0;
}

bool try_extract_router_target_rid (const zlink_msg_t *part_, zlink_routing_id_t *out_)
{
    if (!part_ || !out_)
        return false;

    zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (const_cast<zlink_msg_t *> (part_));
    if (!msg->check ())
        return false;

    const size_t size = msg->size ();
    if (size == 0 || size > sizeof (out_->data))
        return false;

    out_->size = static_cast<uint8_t> (size);
    memcpy (out_->data, msg->data (), size);
    return true;
}

bool parse_stream_routing_id (const zlink_routing_id_t *rid_, uint32_t *routing_id_out_)
{
    if (!rid_ || !routing_id_out_ || rid_->size == 0 || rid_->size > sizeof (rid_->data)
        || rid_->size != 4) {
        errno = EINVAL;
        return false;
    }

    *routing_id_out_ =
      (static_cast<uint32_t> (rid_->data[0]) << 24) | (static_cast<uint32_t> (rid_->data[1]) << 16)
      | (static_cast<uint32_t> (rid_->data[2]) << 8) | static_cast<uint32_t> (rid_->data[3]);
    return true;
}

int send_stream_message (const socket_handle_t &handle_,
                         const zlink_routing_id_t *rid_,
                         zlink_msg_t *msg_,
                         zlink_send_flags_t flags_,
                         bool manage_public_send_recovery_)
{
    zlink::msg_t *core_msg = reinterpret_cast<zlink::msg_t *> (msg_);
    if (!core_msg->check ()) {
        errno = EFAULT;
        return -1;
    }

    uint32_t routing_id = 0;
    if (!parse_stream_routing_id (rid_, &routing_id)) {
        const int err = errno;
        zlink::request_reply::consume_send_frame (msg_);
        errno = err;
        return -1;
    }

    // A complete routed STREAM send takes the route-shard branch before the
    // legacy multipart state in stream_t::xsend. send_complete_record() owns
    // the lifecycle scope; taking the wrapper mutex here would
    // invert it with mailbox command ownership during pipe termination.
    if (core_msg->set_routing_id (routing_id) != 0) {
        const int err = errno;
        zlink::request_reply::consume_send_frame (msg_);
        errno = err;
        return -1;
    }

    const zlink_send_flags_t base_flags = static_cast<zlink_send_flags_t> (flags_ & ZLINK_DONTWAIT);
    const int send_rc =
      handle_.socket->send_complete_record (
        core_msg, base_flags, manage_public_send_recovery_);
    if (send_rc < 0) {
        const int err = errno;
        if (manage_public_send_recovery_ && err != EAGAIN)
            zlink::request_reply::consume_send_frame (msg_);
        errno = err;
        return -1;
    }

    errno = 0;
    return 0;
}

int validate_socket_send_request (const socket_handle_t &handle_,
                                  zlink_msg_t *parts_,
                                  size_t part_count_,
                                  zlink_send_flags_t flags_)
{
    if (!handle_.socket) {
        errno = EFAULT;
        return -1;
    }
    if (zlink::part_helper_internal::validate_send_flags (flags_) != 0)
        return -1;
    return validate_send_parts (parts_, part_count_);
}

int send_socket_unrouted_parts (const socket_handle_t &handle_,
                                zlink_msg_t *parts_,
                                size_t part_count_,
                                zlink_send_flags_t flags_,
                                bool manage_public_send_recovery_)
{
    // Hot path: PAIR/DEALER single-part public send reaches here on every
    // message. Keep this path free of extra allocation and avoid adding
    // indirection beyond the public contract checks.
    const int type = socket_type (handle_);
    if (type == ZLINK_CORE_SOCKET_PUB || type == ZLINK_CORE_SOCKET_SUB
        || type == ZLINK_CORE_SOCKET_XSUB || type == ZLINK_CORE_SOCKET_XPUB) {
        errno = ENOTSUP;
        return -1;
    }

    if (part_count_ == 1) {
        const int rc = handle_.socket->send_complete_record (
          reinterpret_cast<zlink::msg_t *> (&parts_[0]),
          static_cast<zlink_send_flags_t> (flags_ & ZLINK_DONTWAIT),
          manage_public_send_recovery_);
        if (rc < 0)
            return -1;
        errno = 0;
        return 0;
    }

    return zlink::logical_multipart_send (handle_.socket, parts_, part_count_, flags_);
}

int send_socket_routed_parts (const socket_handle_t &handle_,
                              const zlink_routing_id_t *target_rid_,
                              zlink_msg_t *parts_,
                              size_t part_count_,
                              zlink_send_flags_t flags_,
                              bool manage_public_send_recovery_)
{
    const int type = socket_type (handle_);
    if (type == ZLINK_CORE_SOCKET_STREAM) {
        if (part_count_ != 1) {
            errno = ENOTSUP;
            return -1;
        }

        const int rc = send_stream_message (
          handle_, target_rid_, &parts_[0], flags_,
          manage_public_send_recovery_);
        if (rc < 0)
            return -1;
        errno = 0;
        return 0;
    }

    if (type != ZLINK_CORE_SOCKET_ROUTER) {
        errno = ENOTSUP;
        return -1;
    }

    if (part_count_ == 1) {
        const int rc = handle_.socket->send_routed_complete_record (
          target_rid_, reinterpret_cast<zlink::msg_t *> (&parts_[0]),
          static_cast<int> (flags_ & ZLINK_DONTWAIT),
          manage_public_send_recovery_);
        if (rc != 0)
            return -1;
        errno = 0;
        return 0;
    }

    return zlink::logical_multipart_send_routed (handle_.socket, target_rid_, parts_, part_count_,
                                                 flags_);
}

int send_socket_parts (const socket_handle_t &handle_,
                       const zlink_routing_id_t *target_rid_,
                       zlink_msg_t *parts_,
                       size_t part_count_,
                       zlink_send_flags_t flags_,
                       bool manage_public_send_recovery_ = true)
{
    if (validate_socket_send_request (handle_, parts_, part_count_, flags_) != 0)
        return -1;

    if (!target_rid_) {
        const int type = socket_type (handle_);
        const bool blocking_send = (flags_ & ZLINK_DONTWAIT) == 0;

        if (type == ZLINK_CORE_SOCKET_ROUTER && blocking_send && part_count_ > 1) {
            zlink_routing_id_t target_rid;
            memset (&target_rid, 0, sizeof (target_rid));
            if (try_extract_router_target_rid (&parts_[0], &target_rid)) {
                const int rc = send_socket_routed_parts (handle_, &target_rid, parts_ + 1,
                                                         part_count_ - 1, flags_,
                                                         manage_public_send_recovery_);
                zlink::request_reply::consume_send_frame (&parts_[0]);
                if (rc != 0)
                    zlink::request_reply::consume_send_frames_from (parts_, 1, part_count_);
                return rc;
            }
        }

        return send_socket_unrouted_parts (
          handle_, parts_, part_count_, flags_,
          manage_public_send_recovery_);
    }

    return send_socket_routed_parts (
      handle_, target_rid_, parts_, part_count_, flags_,
      manage_public_send_recovery_);
}

int publish_socket_parts (const socket_handle_t &handle_,
                          const char *topic_id_,
                          zlink_msg_t *parts_,
                          size_t part_count_,
                          zlink_send_flags_t flags_)
{
    if (validate_socket_send_request (handle_, parts_, part_count_, flags_) != 0)
        return -1;

    const int type = socket_type (handle_);
    if (type != ZLINK_CORE_SOCKET_PUB && type != ZLINK_CORE_SOCKET_XPUB) {
        errno = ENOTSUP;
        return -1;
    }

    return zlink::logical_multipart_publish (handle_.socket, topic_id_, parts_, part_count_, flags_);
}

zlink_submit_result_t submit_public_send_record (
  const socket_handle_t &handle_, const zlink_routing_id_t *target_rid_,
  zlink_msg_t *parts_,
  size_t part_count_, zlink_send_flags_t flags_, void *user_context_,
  zlink_completion_id_t *completion_id_out_)
{
    if (completion_id_out_)
        *completion_id_out_ = 0;

    int rc = -1;
    if (flags_ == ZLINK_SEND_FLAGS_DONTWAIT) {
        // The successful hot path remains a single allocation-free admission
        // attempt. Only its retryable fallback registers a payload-free wait
        // token for the exact logical target.
        if (part_count_ == 1)
            rc = send_socket_parts (handle_, target_rid_, parts_, part_count_,
                                    flags_, false);
        else {
            std::optional<zlink::socket_public_send_scope_t> scope;
            if (handle_.socket->begin_complete_send_scope (&scope))
                rc = handle_.socket->try_send_parts_scoped_once (
                  parts_, part_count_, target_rid_, *scope);
        }
    } else {
        rc = handle_.socket->send_completion_submit_blocking (
          parts_, part_count_, target_rid_);
    }
    int saved_errno = rc == 0 ? 0 : errno;
    if (rc != 0 && flags_ == ZLINK_SEND_FLAGS_DONTWAIT) {
        (void) handle_.socket->register_send_writable_wait_after_failure (
          saved_errno, target_rid_, user_context_, completion_id_out_);
        saved_errno = errno;
    }
    errno = saved_errno;
    return zlink::submit_result_internal::from_rc (rc);
}


}

extern "C" int zlink_socket_send_internal (void *socket_,
                                           zlink_msg_t *parts_,
                                           size_t part_count_,
                                           zlink_send_flags_t flags_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket)
        return -1;

    return send_socket_parts (handle, NULL, parts_, part_count_, flags_);
}

extern "C" int zlink_socket_send_rid_internal (void *socket_,
                                               const zlink_routing_id_t *target_rid_,
                                               zlink_msg_t *parts_,
                                               size_t part_count_,
                                               zlink_send_flags_t flags_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket)
        return -1;

    return send_socket_parts (handle, target_rid_, parts_, part_count_, flags_);
}

extern "C" int zlink_socket_publish_internal (void *socket_,
                                              const char *topic_id_,
                                              zlink_msg_t *parts_,
                                              size_t part_count_,
                                              zlink_send_flags_t flags_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket)
        return -1;

    return publish_socket_parts (handle, topic_id_, parts_, part_count_, flags_);
}

zlink_submit_result_t zlink_send (
  void *s_, zlink_msg_t *parts_, size_t part_count_,
  zlink_send_flags_t flags_, void *user_context_,
  zlink_completion_id_t *completion_id_out_)
{
    if (completion_id_out_)
        *completion_id_out_ = 0;
    socket_handle_t handle = as_socket_handle (s_);
    return zlink::part_helper_internal::submit_whole_record (
      handle.socket, parts_, part_count_, 0, [&] {
          if (zlink::part_helper_internal::validate_send_flags (flags_) != 0
              || (user_context_ && flags_ != ZLINK_SEND_FLAGS_DONTWAIT)) {
              errno = EINVAL;
              return ZLINK_SUBMIT_INVALID_ARGUMENT;
          }
          const int type = socket_type (handle);
          if (type != ZLINK_CORE_SOCKET_PAIR && type != ZLINK_CORE_SOCKET_DEALER) {
              errno = ENOTSUP;
              return ZLINK_SUBMIT_NOT_SUPPORTED;
          }
          return submit_public_send_record (
            handle, NULL, parts_, part_count_, flags_, user_context_,
            completion_id_out_);
      });
}

zlink_submit_result_t zlink_send_rid (
  void *s_, const zlink_routing_id_t *target_rid_,
  zlink_msg_t *parts_, size_t part_count_, zlink_send_flags_t flags_,
  void *user_context_, zlink_completion_id_t *completion_id_out_)
{
    if (completion_id_out_)
        *completion_id_out_ = 0;
    socket_handle_t handle = as_socket_handle (s_);
    return zlink::part_helper_internal::submit_whole_record (
      handle.socket, parts_, part_count_, !target_rid_ ? EFAULT : 0, [&] {
          if (!zlink::valid_routing_id (target_rid_)
              || zlink::part_helper_internal::validate_send_flags (flags_) != 0
              || (user_context_ && flags_ != ZLINK_SEND_FLAGS_DONTWAIT)) {
              errno = EINVAL;
              return ZLINK_SUBMIT_INVALID_ARGUMENT;
          }
          const int type = socket_type (handle);
          if ((type != ZLINK_CORE_SOCKET_ROUTER && type != ZLINK_CORE_SOCKET_STREAM)
              || (type == ZLINK_CORE_SOCKET_STREAM && part_count_ != 1)) {
              errno = ENOTSUP;
              return ZLINK_SUBMIT_NOT_SUPPORTED;
          }
          return submit_public_send_record (
            handle, target_rid_, parts_, part_count_, flags_, user_context_,
            completion_id_out_);
      });
}

zlink_submit_result_t zlink_publish (
  void *subject_, const char *topic_id_, zlink_msg_t *parts_,
  size_t part_count_, zlink_send_flags_t flags_)
{
    socket_handle_t handle = as_socket_handle (subject_);
    return zlink::part_helper_internal::submit_whole_record (
      handle.socket, parts_, part_count_, !topic_id_ ? EFAULT : 0, [&] {
          return zlink::submit_result_internal::from_rc (
            publish_socket_parts (handle, topic_id_, parts_, part_count_, flags_));
      });
}
