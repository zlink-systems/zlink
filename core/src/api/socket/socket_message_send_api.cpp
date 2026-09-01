/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <climits>
#include <new>

#include "api/monitoring/poller_api_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_message_api_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/message/submit_result_internal.hpp"
#include "core/msg.hpp"
#include "core/multipart_send_txn.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"
#include "utils/likely.hpp"
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

void consume_checked_core_msg (zlink::msg_t *msg_)
{
    if (!msg_ || !msg_->check ())
        return;

    const int saved_errno = errno;
    const int close_rc = msg_->close ();
    errno_assert (close_rc == 0);
    const int init_rc = msg_->init ();
    errno_assert (init_rc == 0);
    errno = saved_errno;
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

int s_sendmsg (const socket_handle_t &handle_, zlink_msg_t *msg_, zlink_send_flags_t flags_)
{
    size_t sz = zlink_msg_size (msg_);
    int rc = handle_.socket->send_complete_record (
      reinterpret_cast<zlink::msg_t *> (msg_), flags_);
    if (unlikely (rc < 0))
        return -1;

    size_t max_msgsz = INT_MAX;
    return static_cast<int> (sz < max_msgsz ? sz : max_msgsz);
}

int validate_send_flags (int flags_)
{
    if (flags_ != 0 && flags_ != ZLINK_DONTWAIT) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

bool is_singlepart_fast_socket_type (int type_)
{
    return type_ == ZLINK_CORE_SOCKET_PAIR || type_ == ZLINK_CORE_SOCKET_DEALER;
}

int send_socket_singlepart_fast (const socket_handle_t &handle_,
                                 zlink_msg_t *msg_,
                                 zlink_send_flags_t flags_)
{
    if (!handle_.socket || !msg_) {
        errno = EFAULT;
        return -1;
    }
    if (validate_send_flags (flags_) != 0)
        return -1;

    zlink::msg_t *core_msg = reinterpret_cast<zlink::msg_t *> (msg_);
    if (!core_msg->check ()) {
        errno = EFAULT;
        return -1;
    }

    if (handle_.socket->send_complete_record (
          core_msg, static_cast<int> (flags_ & ZLINK_DONTWAIT))
        != 0)
        return -1;

    errno = 0;
    return 0;
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
                         zlink_send_flags_t flags_)
{
    if (!handle_.socket)
        return -1;

    if (!msg_) {
        errno = EINVAL;
        return -1;
    }

    zlink::msg_t *core_msg = reinterpret_cast<zlink::msg_t *> (msg_);
    if (!core_msg->check ()) {
        errno = EFAULT;
        return -1;
    }

    if (!is_stream_type (handle_)) {
        errno = EINVAL;
        return -1;
    }

    uint32_t routing_id = 0;
    if (!parse_stream_routing_id (rid_, &routing_id)) {
        const int err = errno;
        consume_checked_core_msg (core_msg);
        errno = err;
        return -1;
    }

    // A complete routed STREAM send takes the route-shard branch before the
    // legacy multipart state in stream_t::xsend. s_sendmsg's lifecycle scope
    // already serializes send/close; taking the wrapper mutex here would
    // invert it with mailbox command ownership during pipe termination.
    if (core_msg->set_routing_id (routing_id) != 0) {
        const int err = errno;
        consume_checked_core_msg (core_msg);
        errno = err;
        return -1;
    }

    const zlink_send_flags_t base_flags = static_cast<zlink_send_flags_t> (flags_ & ZLINK_DONTWAIT);
    const int send_rc = s_sendmsg (handle_, reinterpret_cast<zlink_msg_t *> (core_msg), base_flags);
    if (send_rc < 0) {
        const int err = errno;
        if (err != EAGAIN)
            consume_checked_core_msg (core_msg);
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
    if (validate_send_flags (flags_) != 0)
        return -1;
    return validate_send_parts (parts_, part_count_);
}

int send_socket_unrouted_parts (const socket_handle_t &handle_,
                                zlink_msg_t *parts_,
                                size_t part_count_,
                                zlink_send_flags_t flags_)
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
        const int rc = s_sendmsg (handle_, &parts_[0],
                                  static_cast<zlink_send_flags_t> (flags_ & ZLINK_DONTWAIT));
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
                              zlink_send_flags_t flags_)
{
    const int type = socket_type (handle_);
    if (type == ZLINK_CORE_SOCKET_STREAM) {
        if (part_count_ != 1) {
            errno = ENOTSUP;
            return -1;
        }

        const int rc = send_stream_message (handle_, target_rid_, &parts_[0], flags_);
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
          static_cast<int> (flags_ & ZLINK_DONTWAIT));
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
                       zlink_send_flags_t flags_)
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
                                                         part_count_ - 1, flags_);
                zlink::request_reply::consume_send_frame (&parts_[0]);
                if (rc != 0)
                    zlink::request_reply::consume_send_frames_from (parts_, 1, part_count_);
                return rc;
            }
        }

        return send_socket_unrouted_parts (handle_, parts_, part_count_, flags_);
    }

    return send_socket_routed_parts (handle_, target_rid_, parts_, part_count_, flags_);
}

int publish_socket_parts (const socket_handle_t &handle_,
                          const char *topic_id_,
                          zlink_msg_t *parts_,
                          size_t part_count_,
                          zlink_send_flags_t flags_,
                          bool fallback_on_missing_sndtimeo_)
{
    if (validate_socket_send_request (handle_, parts_, part_count_, flags_) != 0)
        return -1;

    const int type = socket_type (handle_);
    if (type != ZLINK_CORE_SOCKET_PUB && type != ZLINK_CORE_SOCKET_XPUB) {
        errno = ENOTSUP;
        return -1;
    }

    (void) fallback_on_missing_sndtimeo_;
    return zlink::logical_multipart_publish (handle_.socket, topic_id_, parts_, part_count_, flags_);
}

zlink_submit_result_t
submit_simple_part (void *handle_,
                    const zlink::part_helper_internal::send_sequence_spec_t &spec_,
                    zlink::socket_base_t *sink_socket_,
                    zlink_msg_t *part_,
                    zlink_part_flag_t part_flag_,
                    int (*send_fn_) (bool first_part_,
                                     zlink::part_helper_internal::handle_state_t *state_,
                                     zlink::socket_base_t *sink_socket_,
                                     const zlink::part_helper_internal::send_sequence_spec_t &spec_,
                                     zlink_msg_t *part_,
                                     zlink_send_flags_t flags_,
                                     zlink_part_flag_t part_flag_))
{
    if (!handle_ || !part_ || !send_fn_) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EFAULT;
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (zlink::part_helper_internal::validate_part_flag (part_flag_) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> state;
    bool first_part = false;
    if (zlink::part_helper_internal::prepare_send_step (handle_, spec_, sink_socket_, &state,
                                                        &first_part)
        != 0) {
        zlink::part_helper_internal::trace_routed_part_prepare_failed (spec_.family, errno);
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    if (send_fn_ (first_part, state.get (), sink_socket_, spec_, part_, spec_.flags, part_flag_)
        != 0) {
        const int saved_errno = errno;
        zlink::part_helper_internal::trace_routed_part_send_failed (spec_.family, first_part,
                                                                    saved_errno);
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    zlink::part_helper_internal::complete_send_step (state, part_flag_);
    return ZLINK_SUBMIT_OK;
}

int stage_public_send_part (
  const std::shared_ptr<zlink::part_helper_internal::handle_state_t> &state_,
  zlink_msg_t *part_)
{
    if (!state_ || !part_) {
        errno = EFAULT;
        return -1;
    }
    try {
        std::lock_guard<std::mutex> lock (state_->mutex);
        zlink_msg_t &slot = state_->send.buffered_parts.append_uninitialized ();
        zlink_msg_init (&slot);
        if (zlink_msg_move (&slot, part_) != 0) {
            zlink_msg_close (&slot);
            state_->send.buffered_parts.pop_back ();
            errno = EFAULT;
            return -1;
        }
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int collect_public_send_record (
  const std::shared_ptr<zlink::part_helper_internal::handle_state_t> &state_,
  zlink_msg_t *final_part_, std::vector<zlink_msg_t> *record_)
{
    if (!state_ || !final_part_ || !record_) {
        errno = EFAULT;
        return -1;
    }

    size_t initialized = 0;
    try {
        std::lock_guard<std::mutex> lock (state_->mutex);
        const size_t prefix_count = state_->send.buffered_parts.size ();
        record_->resize (prefix_count + 1);
        for (; initialized < record_->size (); ++initialized) {
            if (zlink_msg_init (&(*record_)[initialized]) != 0)
                break;
        }
        if (initialized != record_->size ()) {
            errno = ENOMEM;
        } else {
            for (size_t i = 0; i < prefix_count; ++i) {
                if (zlink_msg_move (&(*record_)[i],
                                    &state_->send.buffered_parts[i])
                    != 0) {
                    errno = EFAULT;
                    return -1;
                }
            }
            if (zlink_msg_move (&record_->back (), final_part_) != 0) {
                errno = EFAULT;
                return -1;
            }
            return 0;
        }
    } catch (...) {
        errno = ENOMEM;
    }

    const int saved_errno = errno;
    for (size_t i = 0; i < initialized; ++i)
        zlink_msg_close (&(*record_)[i]);
    record_->clear ();
    errno = saved_errno;
    return -1;
}

void consume_public_send_record (std::vector<zlink_msg_t> *record_)
{
    if (!record_)
        return;
    const int saved_errno = errno;
    for (size_t i = 0; i < record_->size (); ++i)
        zlink::part_helper_internal::consume_send_part (&(*record_)[i]);
    errno = saved_errno;
}

zlink_submit_result_t submit_public_send_record (
  const socket_handle_t &handle_, zlink::socket_base_t *socket_,
  const zlink_routing_id_t *target_rid_, zlink_msg_t *parts_,
  size_t part_count_, zlink_send_flags_t flags_, void *user_context_,
  zlink_completion_id_t *completion_id_out_)
{
    int rc = -1;
    if (flags_ == ZLINK_SEND_FLAGS_DONTWAIT) {
        rc = socket_->send_completion_submit (
          parts_, part_count_, target_rid_, user_context_,
          completion_id_out_);
    } else {
        rc = socket_->send_completion_submit_blocking (
          parts_, part_count_, target_rid_);
    }
    const int saved_errno = errno;
    for (size_t i = 0; i < part_count_; ++i)
        zlink::part_helper_internal::consume_send_part (&parts_[i]);
    errno = saved_errno;
    return zlink::submit_result_internal::from_rc (rc);
}

zlink_submit_result_t submit_completion_aware_part (
  void *public_handle_, const socket_handle_t &handle_,
  const zlink_routing_id_t *target_rid_, zlink_msg_t *part_,
  zlink_part_flag_t part_flag_, void *user_context_,
  zlink_completion_id_t *completion_id_out_,
  const zlink::part_helper_internal::send_sequence_spec_t &spec_)
{
    zlink::socket_base_t *const socket = handle_.socket;
    if (part_flag_ == ZLINK_PART_FINAL
        && !zlink::part_helper_internal::send_sequence_active (socket)) {
        return submit_public_send_record (
          handle_, socket, target_rid_, part_, 1, spec_.flags,
          user_context_, completion_id_out_);
    }

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> state;
    bool first_part = false;
    if (zlink::part_helper_internal::prepare_send_step (
          public_handle_, spec_, socket, &state, &first_part)
        != 0) {
        const int saved_errno = errno;
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (
          public_handle_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    if (part_flag_ == ZLINK_PART_MORE) {
        if (stage_public_send_part (state, part_) != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return zlink::submit_result_internal::from_errno (saved_errno);
        }
        zlink::part_helper_internal::complete_send_step (state,
                                                         ZLINK_PART_MORE);
        return ZLINK_SUBMIT_OK;
    }

    std::vector<zlink_msg_t> record;
    if (collect_public_send_record (state, part_, &record) != 0) {
        const int saved_errno = errno;
        consume_public_send_record (&record);
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    zlink::part_helper_internal::complete_send_step (state,
                                                     ZLINK_PART_FINAL);
    return submit_public_send_record (
      handle_, socket, target_rid_, record.data (), record.size (),
      spec_.flags, user_context_, completion_id_out_);
}

int send_socket_part_impl (bool,
                           zlink::part_helper_internal::handle_state_t *state_,
                           zlink::socket_base_t *sink_socket_,
                           const zlink::part_helper_internal::send_sequence_spec_t &,
                           zlink_msg_t *part_,
                           zlink_send_flags_t flags_,
                           zlink_part_flag_t part_flag_)
{
    if (!state_ || !sink_socket_ || !part_ || !state_->send.send_scope) {
        errno = EFAULT;
        return -1;
    }

    return sink_socket_->send_scoped (reinterpret_cast<zlink::msg_t *> (part_),
                                      static_cast<int> (flags_ & ZLINK_DONTWAIT)
                                        | (part_flag_ == ZLINK_PART_MORE ? ZLINK_SNDMORE : 0),
                                      *state_->send.send_scope, NULL, true);
}

int send_socket_part_routed_impl (bool first_part_,
                                  zlink::part_helper_internal::handle_state_t *state_,
                                  zlink::socket_base_t *sink_socket_,
                                  const zlink::part_helper_internal::send_sequence_spec_t &spec_,
                                  zlink_msg_t *part_,
                                  zlink_send_flags_t flags_,
                                  zlink_part_flag_t part_flag_)
{
    if (!state_ || !sink_socket_ || !part_ || !state_->send.send_scope) {
        errno = EFAULT;
        return -1;
    }

    if (sink_socket_->socket_type () == ZLINK_CORE_SOCKET_STREAM) {
        if (part_flag_ != ZLINK_PART_FINAL) {
            errno = ENOTSUP;
            return -1;
        }

        socket_handle_t handle = make_socket_handle (sink_socket_);
        return send_stream_message (handle, &spec_.rid1, part_, flags_);
    }

    if (first_part_) {
        zlink::part_helper_internal::trace_routed_part_first_send (spec_.rid1, part_, flags_);
        return sink_socket_->send_routed_scoped (
          &spec_.rid1, reinterpret_cast<zlink::msg_t *> (part_),
          static_cast<int> (flags_ & ZLINK_DONTWAIT)
            | (part_flag_ == ZLINK_PART_MORE ? ZLINK_SNDMORE : 0),
          *state_->send.send_scope, NULL, 0, NULL,
          spec_.transport_pair_id, spec_.transport_pair_generation, true);
    }

    return sink_socket_->send_scoped (reinterpret_cast<zlink::msg_t *> (part_),
                                      static_cast<int> (flags_ & ZLINK_DONTWAIT)
                                        | (part_flag_ == ZLINK_PART_MORE ? ZLINK_SNDMORE : 0),
                                      *state_->send.send_scope, NULL, true);
}

int send_socket_part_publish_impl (bool first_part_,
                                   zlink::part_helper_internal::handle_state_t *state_,
                                   zlink::socket_base_t *sink_socket_,
                                   const zlink::part_helper_internal::send_sequence_spec_t &spec_,
                                   zlink_msg_t *part_,
                                   zlink_send_flags_t flags_,
                                   zlink_part_flag_t part_flag_)
{
    if (!state_ || !sink_socket_ || !part_ || !state_->send.send_scope) {
        errno = EFAULT;
        return -1;
    }

    if (first_part_ && spec_.has_text1) {
        zlink::msg_t topic_msg;
        if (topic_msg.init_size (spec_.text1.size ()) != 0)
            return -1;
        if (!spec_.text1.empty ())
            memcpy (topic_msg.data (), spec_.text1.data (), spec_.text1.size ());
        const int topic_rc = sink_socket_->send_scoped (
          &topic_msg, static_cast<int> (flags_ & ZLINK_DONTWAIT) | ZLINK_SNDMORE,
          *state_->send.send_scope, NULL, true);
        const int saved_errno = errno;
        (void) topic_msg.close ();
        errno = saved_errno;
        if (topic_rc != 0)
            return -1;
    }

    return sink_socket_->send_scoped (reinterpret_cast<zlink::msg_t *> (part_),
                                      static_cast<int> (flags_ & ZLINK_DONTWAIT)
                                        | (part_flag_ == ZLINK_PART_MORE ? ZLINK_SNDMORE : 0),
                                      *state_->send.send_scope, NULL, true);
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

    return publish_socket_parts (handle, topic_id_, parts_, part_count_, flags_,
                                 (flags_ & ZLINK_DONTWAIT) == 0);
}

zlink_submit_result_t zlink_send_part (void *s_,
                                       zlink_msg_t *part_,
                                       zlink_send_flags_t flags_,
                                       zlink_part_flag_t part_flag_,
                                       void *user_context_,
                                       zlink_completion_id_t *completion_id_out_)
{
    if (completion_id_out_)
        *completion_id_out_ = 0;

    if (!part_) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }

    if (zlink::part_helper_internal::validate_send_flags (flags_) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (zlink::part_helper_internal::validate_part_flag (part_flag_) != 0
        || (user_context_
            && (flags_ != ZLINK_SEND_FLAGS_DONTWAIT
                || part_flag_ != ZLINK_PART_FINAL))) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }

    socket_handle_t socket_guard = as_socket_handle (s_);
    zlink::socket_base_t *socket = socket_guard.socket;
    if (!socket) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    const int type = socket->socket_type ();
    // The unrouted raw helper belongs only to PAIR and DEALER. ROUTER requires
    // a target RID, while STREAM requires its dedicated routed single-part
    // path. Letting either family reach xsend() can turn an unrouted payload
    // into transport framing state instead of rejecting the unsupported API.
    if (type != ZLINK_CORE_SOCKET_PAIR && type != ZLINK_CORE_SOCKET_DEALER) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = ENOTSUP;
        return zlink::submit_result_internal::from_errno (errno);
    }
    zlink::part_helper_internal::send_sequence_spec_t spec;
    spec.family = zlink::part_helper_internal::send_family_send;
    spec.flags = flags_;
    return submit_completion_aware_part (
      s_, socket_guard, NULL, part_, part_flag_, user_context_,
      completion_id_out_, spec);
}

zlink_submit_result_t zlink_send_part_rid (void *s_,
                                           const zlink_routing_id_t *target_rid_,
                                           zlink_msg_t *part_,
                                           zlink_send_flags_t flags_,
                                           zlink_part_flag_t part_flag_,
                                           void *user_context_,
                                           zlink_completion_id_t *completion_id_out_)
{
    if (completion_id_out_)
        *completion_id_out_ = 0;

    if (!part_) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }

    if (!zlink::valid_routing_id (target_rid_)) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (zlink::part_helper_internal::validate_send_flags (flags_) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (zlink::part_helper_internal::validate_part_flag (part_flag_) != 0
        || (user_context_
            && (flags_ != ZLINK_SEND_FLAGS_DONTWAIT
                || part_flag_ != ZLINK_PART_FINAL))) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }

    socket_handle_t socket_guard = as_socket_handle (s_);
    zlink::socket_base_t *socket = socket_guard.socket;
    if (!socket) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    const int type = socket->socket_type ();
    if (type == ZLINK_CORE_SOCKET_STREAM) {
        if (part_flag_ != ZLINK_PART_FINAL) {
            zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = ENOTSUP;
            return zlink::submit_result_internal::from_errno (errno);
        }

        return submit_public_send_record (
          socket_guard, socket, target_rid_, part_, 1, flags_, user_context_,
          completion_id_out_);
    }

    // Singlepart fast path for ROUTER+FINAL. Mirrors the DEALER fast path in
    // zlink_send_part: when the caller is sending a single FINAL part with
    // no competing multipart sequence on this socket, skip the
    // submit_simple_part scaffolding (handle_state shared_ptr lookup,
    // send_sequence_spec construction, copy_routing_id) and hand the part
    // straight to socket_base_t::send_routed.
    //
    // submit_simple_part is required for partial multipart sends (PART_MORE
    // followed by PART_FINAL) where the core has to remember per-handle
    // state across calls. For the FINAL-only case the state machine is a
    // no-op and the allocation shows up as a measurable per-message cost
    // on RR/DR-server hot paths at 100-socket fan-in.
    zlink::part_helper_internal::send_sequence_spec_t spec;
    spec.family = zlink::part_helper_internal::send_family_send_rid;
    spec.flags = flags_;
    spec.has_rid1 = true;
    zlink::part_helper_internal::copy_routing_id (target_rid_, &spec.rid1);
    return submit_completion_aware_part (
      s_, socket_guard, target_rid_, part_, part_flag_, user_context_,
      completion_id_out_, spec);
}

zlink_submit_result_t zlink_publish_part (void *subject_,
                                          const char *topic_id_,
                                          zlink_msg_t *part_,
                                          zlink_send_flags_t flags_,
                                          zlink_part_flag_t part_flag_)
{
    if (zlink::part_helper_internal::validate_send_flags (flags_) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (subject_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    socket_handle_t socket_guard = as_socket_handle (subject_);
    zlink::socket_base_t *socket = socket_guard.socket;
    if (!socket) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (subject_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    const int type = socket->socket_type ();
    if (type != ZLINK_CORE_SOCKET_PUB && type != ZLINK_CORE_SOCKET_XPUB) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (subject_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = ENOTSUP;
        return zlink::submit_result_internal::from_errno (errno);
    }

    // FINAL owns no state across public calls. Use complete-record admission
    // with the entry's existing socket pin, leaving helper state for actual
    // multipart sequences.
    if (part_flag_ == ZLINK_PART_FINAL
        && !zlink::part_helper_internal::send_sequence_active (socket)) {
        const int rc = publish_socket_parts (socket_guard, topic_id_, part_, 1,
                                             flags_, false);
        const int saved_errno = errno;
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_rc (rc);
    }

    try {
        zlink::part_helper_internal::send_sequence_spec_t spec;
        spec.family = zlink::part_helper_internal::send_family_publish;
        spec.flags = flags_;
        spec.has_text1 = topic_id_ != NULL;
        spec.text1 = topic_id_ ? topic_id_ : "";
        return submit_simple_part (subject_, spec, socket, part_, part_flag_,
                                   &send_socket_part_publish_impl);
    } catch (const std::bad_alloc &) {
        // PUB/XPUB pre-submit rejection owns the submitted part but does not
        // abort an already-open publish sequence.
        zlink::part_helper_internal::consume_send_part (part_);
        errno = ENOMEM;
        return zlink::submit_result_internal::from_errno (errno);
    }
}
