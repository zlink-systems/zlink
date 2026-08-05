/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>

#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/monitoring/poller_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_request_reply_submit_internal.hpp"
#include "api/message/submit_result_internal.hpp"
#include "core/multipart_send_txn.hpp"
#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "utils/routing_id.hpp"

namespace reqrep = zlink::socket_reqrep_internal;

namespace
{
const size_t stack_request_reply_part_capacity = 8;

struct pending_pair_observer_t
{
    std::shared_ptr<reqrep::socket_request_reply_state_t> state;
    reqrep::pending_key_t key;
};

void record_pending_pair (zlink::pipe_t *pipe_, void *userdata_)
{
    pending_pair_observer_t *observer =
      static_cast<pending_pair_observer_t *> (userdata_);
    if (observer)
        reqrep::record_socket_pending_transport_pair (
          observer->state, observer->key, pipe_);
}

zlink_submit_result_t finish_request_submit_failure (
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  const reqrep::pending_key_t &key_,
  zlink_submit_result_t failure_)
{
    // A timeout or disconnect may remove the pending entry while the send
    // operation is still unwinding. In that case Core has already retained a
    // completion callback and the caller must receive a successful submit
    // result so its callback userdata remains owned by that completion.
    if (!reqrep::erase_socket_pending_request (state_, key_)) {
        errno = 0;
        return ZLINK_SUBMIT_OK;
    }
    return failure_;
}

int send_dealer_reply_to_target (zlink::socket_base_t *socket_,
                                 const reqrep::dealer_reply_target_t &target_,
                                 zlink::part_helper_internal::handle_state_t *helper_state_,
                                 zlink_msg_t *final_part_)
{
    if (!target_.pipe || !helper_state_ || !final_part_ || target_.request_seq == 0) {
        errno = EFAULT;
        return -1;
    }

    const size_t payload_count = helper_state_->send.buffered_parts.size () + 1;
    const size_t total_part_count = zlink::request_reply::control_part_count + payload_count;
    zlink_msg_t stack_combined[stack_request_reply_part_capacity];
    std::vector<zlink_msg_t> heap_combined;
    zlink_msg_t *combined =
      total_part_count <= stack_request_reply_part_capacity ? stack_combined : NULL;
    if (!combined) {
        heap_combined.resize (total_part_count);
        combined = &heap_combined[0];
    }
    for (size_t i = 0; i < total_part_count; ++i)
        zlink_msg_init (&combined[i]);

    if (zlink::request_reply::init_envelope_control_parts (
          combined, zlink::request_reply::reply_type, target_.request_seq)
        != 0) {
        const int saved_errno = errno;
        zlink::request_reply::close_built_parts (combined, total_part_count);
        errno = saved_errno;
        return -1;
    }

    size_t out_index = zlink::request_reply::control_part_count;
    for (size_t i = 0; i < helper_state_->send.buffered_parts.size (); ++i, ++out_index) {
        if (zlink_msg_move (&combined[out_index], &helper_state_->send.buffered_parts[i]) != 0) {
            const int saved_errno = errno;
            zlink::request_reply::close_built_parts (combined, total_part_count);
            errno = saved_errno;
            return -1;
        }
    }
    if (zlink_msg_move (&combined[out_index], final_part_) != 0) {
        const int saved_errno = errno;
        zlink::request_reply::close_built_parts (combined, total_part_count);
        errno = saved_errno;
        return -1;
    }

    if (reqrep::send_completion_frames (
          socket_, target_.pipe, NULL, combined, total_part_count)
        != 0) {
        const int saved_errno = errno;
        zlink::request_reply::close_built_parts (combined, total_part_count);
        errno = saved_errno;
        return -1;
    }

    zlink::request_reply::close_built_parts (combined, total_part_count);
    errno = 0;
    return 0;
}

zlink_submit_result_t request_part_common (void *handle_,
                                           const zlink_routing_id_t *peer_rid_,
                                           zlink_msg_t *part_,
                                           zlink_send_flags_t flags_,
                                           zlink_part_flag_t part_flag_,
                                           uint32_t timeout_ms_,
                                           zlink_reply_handler_fn handler_,
                                           void *userdata_,
                                           zlink::part_helper_internal::send_family_t family_)
{
    if (zlink::part_helper_internal::validate_part_flag (part_flag_) != 0) {
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (reqrep::validate_request_send_flags (flags_) != 0) {
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    socket_handle_t handle = as_socket_handle (handle_);
    if (!handle.socket) {
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EFAULT;
        return zlink::submit_result_internal::from_errno (errno);
    }

    zlink::part_helper_internal::send_sequence_spec_t spec;
    spec.family = family_;
    spec.flags = flags_;
    spec.timeout_ms = timeout_ms_;
    spec.handler = handler_;
    spec.userdata = userdata_;
    spec.request_like = true;
    if (peer_rid_) {
        spec.has_rid1 = true;
        zlink::part_helper_internal::copy_routing_id (peer_rid_, &spec.rid1);
    }

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      zlink::part_helper_internal::find_handle_state (handle_);
    bool helper_state_matches_family = false;
    bool helper_send_active = false;
    if (helper_state) {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        helper_send_active = helper_state->send.active;
        if (helper_state->send.active && helper_state->send.spec.family == family_) {
            spec.request_seq = helper_state->send.spec.request_seq;
            helper_state_matches_family = true;
        }
    }

    if (part_flag_ == ZLINK_PART_MORE && !handler_ && spec.request_seq == 0) {
        std::shared_ptr<zlink::part_helper_internal::handle_state_t> state;
        bool first_part = false;
        if (zlink::part_helper_internal::prepare_send_step (handle_, spec, handle.socket, &state,
                                                            &first_part)
            != 0) {
            zlink::part_helper_internal::consume_send_part (part_);
            return zlink::submit_result_internal::from_errno (errno);
        }

        if (reqrep::stage_request_payload_part (state.get (), part_) != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return zlink::submit_result_internal::from_errno (saved_errno);
        }

        return ZLINK_SUBMIT_OK;
    }

    std::shared_ptr<reqrep::socket_request_reply_state_t> request_state;
    reqrep::pending_key_t pending_key;
    if (spec.request_seq == 0) {
        if (reqrep::ensure_socket_pending_request (handle, peer_rid_, timeout_ms_, handler_,
                                                   userdata_, &spec.request_seq, &request_state,
                                                   &pending_key)
            != 0) {
            zlink::part_helper_internal::consume_send_part (part_);
            return zlink::submit_result_internal::from_errno (errno);
        }
    } else {
        request_state = reqrep::find_or_create_request_reply_state (handle);
        if (!request_state
            || reqrep::lookup_socket_pending_request_by_seq (request_state, spec.request_seq,
                                                             &pending_key)
                 != 0) {
            zlink::part_helper_internal::consume_send_part (part_);
            return zlink::submit_result_internal::from_errno (errno);
        }
    }

    zlink::msg_t *core_part = reinterpret_cast<zlink::msg_t *> (part_);
    if (!part_ || !core_part->check ()) {
        if (helper_state_matches_family)
            zlink::part_helper_internal::abort_send_step (helper_state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EFAULT;
        return finish_request_submit_failure (
          request_state, pending_key, zlink::submit_result_internal::from_errno (errno));
    }

    if (part_flag_ == ZLINK_PART_FINAL && !helper_send_active) {
        const size_t total_part_count = zlink::request_reply::control_part_count + 1;
        zlink_msg_t combined[total_part_count];
        for (size_t i = 0; i < total_part_count; ++i)
            zlink_msg_init (&combined[i]);
        if (zlink::request_reply::init_envelope_control_parts (
              combined, zlink::request_reply::request_type, spec.request_seq)
              != 0
            || zlink_msg_move (&combined[zlink::request_reply::control_part_count], part_) != 0) {
            const int saved_errno = errno;
            zlink::request_reply::close_built_parts (combined, total_part_count);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return finish_request_submit_failure (
              request_state, pending_key, zlink::submit_result_internal::from_errno (saved_errno));
        }

        pending_pair_observer_t pair_observer;
        pair_observer.state = request_state;
        pair_observer.key = pending_key;
        const int send_rc =
          peer_rid_
            ? zlink::logical_multipart_send_routed_tracked (
                handle.socket, peer_rid_, combined, total_part_count, flags_,
                &record_pending_pair, &pair_observer)
            : zlink::logical_multipart_send_tracked (
                handle.socket, combined, total_part_count, flags_,
                &record_pending_pair, &pair_observer);
        const int saved_errno = errno;
        zlink::request_reply::close_built_parts (combined, total_part_count);
        if (send_rc != 0) {
            errno = saved_errno;
            return finish_request_submit_failure (
              request_state, pending_key, zlink::submit_result_internal::from_errno (saved_errno));
        }
        return ZLINK_SUBMIT_OK;
    }

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> state;
    bool first_part = false;
    if (zlink::part_helper_internal::prepare_send_step (handle_, spec, handle.socket, &state,
                                                        &first_part)
        != 0) {
        const zlink_submit_result_t failure =
          zlink::submit_result_internal::from_errno (errno);
        zlink::part_helper_internal::consume_send_part (part_);
        return spec.request_seq != 0
                 ? finish_request_submit_failure (request_state, pending_key, failure)
                 : failure;
    }

    if (part_flag_ == ZLINK_PART_MORE) {
        if (reqrep::stage_request_payload_part (state.get (), part_) != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return finish_request_submit_failure (
              request_state, pending_key, zlink::submit_result_internal::from_errno (saved_errno));
        }

        return ZLINK_SUBMIT_OK;
    }

    if (first_part || !state->send.buffered_parts.empty ()) {
        const unsigned char type = family_ == zlink::part_helper_internal::send_family_router_reply
                                     ? zlink::request_reply::reply_type
                                     : zlink::request_reply::request_type;
        pending_pair_observer_t pair_observer;
        pair_observer.state = request_state;
        pair_observer.key = pending_key;
        if (zlink::request_reply::send_envelope_control_frames (
              type, spec.request_seq,
              [&] (size_t index_, const void *data_, size_t size_) {
                  zlink::pipe_t *selected_pipe = NULL;
                  const int rc = reqrep::send_request_frame (
                    handle.socket, state.get (),
                    index_ == zlink::request_reply::envelope_protocol_index ? peer_rid_ : NULL,
                    data_, size_, ZLINK_SNDMORE | (flags_ & ZLINK_DONTWAIT),
                    index_ == zlink::request_reply::envelope_protocol_index
                      ? &selected_pipe
                      : NULL);
                  if (rc == 0 && selected_pipe)
                      record_pending_pair (selected_pipe, &pair_observer);
                  return rc;
              })
            != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return finish_request_submit_failure (
              request_state, pending_key, zlink::submit_result_internal::from_errno (saved_errno));
        }
    }

    for (size_t i = 0; i < state->send.buffered_parts.size (); ++i) {
        if (reqrep::send_request_payload_part (handle.socket, state.get (), peer_rid_,
                                               &state->send.buffered_parts[i], flags_,
                                               ZLINK_PART_MORE)
            != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return finish_request_submit_failure (
              request_state, pending_key, zlink::submit_result_internal::from_errno (saved_errno));
        }
    }

    if (reqrep::send_request_payload_part (handle.socket, state.get (), peer_rid_, part_, flags_,
                                           part_flag_)
        != 0) {
        const int saved_errno = errno;
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return finish_request_submit_failure (
          request_state, pending_key, zlink::submit_result_internal::from_errno (saved_errno));
    }

    zlink::part_helper_internal::complete_send_step (state, part_flag_);
    return ZLINK_SUBMIT_OK;
}
}

zlink_submit_result_t zlink_dealer_request_part (void *dealer_,
                                                 zlink_msg_t *part_,
                                                 zlink_send_flags_t flags_,
                                                 zlink_part_flag_t part_flag_,
                                                 uint32_t timeout_ms_,
                                                 zlink_reply_handler_fn handler_,
                                                 void *userdata_)
{
    if (!handler_ && part_flag_ == ZLINK_PART_FINAL) {
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (reqrep::validate_socket_type (dealer_, ZLINK_CORE_SOCKET_DEALER) != 0) {
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    return request_part_common (dealer_, NULL, part_, flags_, part_flag_, timeout_ms_, handler_,
                                userdata_, zlink::part_helper_internal::send_family_dealer_request);
}

zlink_submit_result_t zlink_router_request_part (void *router_,
                                                 const zlink_routing_id_t *peer_rid_,
                                                 zlink_msg_t *part_,
                                                 zlink_send_flags_t flags_,
                                                 zlink_part_flag_t part_flag_,
                                                 uint32_t timeout_ms_,
                                                 zlink_reply_handler_fn handler_,
                                                 void *userdata_)
{
    if ((!handler_ && part_flag_ == ZLINK_PART_FINAL) || !zlink::valid_routing_id (peer_rid_)) {
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (reqrep::validate_socket_type (router_, ZLINK_CORE_SOCKET_ROUTER) != 0) {
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    return request_part_common (router_, peer_rid_, part_, flags_, part_flag_, timeout_ms_,
                                handler_, userdata_,
                                zlink::part_helper_internal::send_family_router_request);
}

zlink_submit_result_t zlink_router_reply_part (void *router_,
                                               const zlink_routing_id_t *peer_rid_,
                                               uint64_t request_seq_,
                                               zlink_msg_t *part_,
                                               zlink_part_flag_t part_flag_)
{
    if (!zlink::valid_routing_id (peer_rid_) || request_seq_ == 0) {
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (reqrep::validate_socket_type (router_, ZLINK_CORE_SOCKET_ROUTER) != 0) {
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    socket_handle_t handle = as_socket_handle (router_);
    zlink::part_helper_internal::send_sequence_spec_t spec;
    spec.family = zlink::part_helper_internal::send_family_router_reply;
    spec.request_like = true;
    spec.request_seq = request_seq_;
    spec.has_rid1 = true;
    zlink::part_helper_internal::copy_routing_id (peer_rid_, &spec.rid1);

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> state;
    bool first_part = false;
    if (zlink::part_helper_internal::prepare_send_step (router_, spec, handle.socket, &state,
                                                        &first_part)
        != 0) {
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    if (part_flag_ == ZLINK_PART_MORE) {
        if (reqrep::stage_request_payload_part (state.get (), part_) != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return zlink::submit_result_internal::from_errno (saved_errno);
        }
        return ZLINK_SUBMIT_OK;
    }

    const size_t payload_count = state->send.buffered_parts.size () + 1;
    std::vector<zlink_msg_t> payload (payload_count);
    for (size_t i = 0; i < payload_count; ++i)
        zlink_msg_init (&payload[i]);
    for (size_t i = 0; i < state->send.buffered_parts.size (); ++i)
        zlink_msg_move (&payload[i], &state->send.buffered_parts[i]);
    zlink_msg_move (&payload[payload_count - 1], part_);

    if (reqrep::send_request_reply_message (
          handle.socket, peer_rid_, &payload[0], payload_count, ZLINK_SEND_FLAGS_NONE,
          zlink::request_reply::reply_type, request_seq_)
        != 0) {
        const int saved_errno = errno;
        zlink::request_reply::close_built_parts (&payload);
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    zlink::request_reply::close_built_parts (&payload);
    zlink::part_helper_internal::complete_send_step (state, part_flag_);
    return ZLINK_SUBMIT_OK;
}

zlink_submit_result_t zlink_router_completion_control_part (
  void *router_,
  const zlink_routing_id_t *peer_rid_,
  zlink_msg_t *part_,
  zlink_part_flag_t part_flag_)
{
    if (zlink::part_helper_internal::validate_part_flag (part_flag_) != 0
        || !zlink::valid_routing_id (peer_rid_)) {
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (reqrep::validate_socket_type (router_, ZLINK_CORE_SOCKET_ROUTER) != 0) {
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    socket_handle_t handle = as_socket_handle (router_);
    zlink::part_helper_internal::send_sequence_spec_t spec;
    spec.family = zlink::part_helper_internal::send_family_router_completion_control;
    spec.has_rid1 = true;
    zlink::part_helper_internal::copy_routing_id (peer_rid_, &spec.rid1);

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> state;
    bool first_part = false;
    if (zlink::part_helper_internal::prepare_send_step (
          router_, spec, handle.socket, &state, &first_part)
        != 0) {
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    if (part_flag_ == ZLINK_PART_MORE) {
        if (reqrep::stage_request_payload_part (state.get (), part_) != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return zlink::submit_result_internal::from_errno (saved_errno);
        }
        return ZLINK_SUBMIT_OK;
    }

    const size_t payload_count = state->send.buffered_parts.size () + 1;
    std::vector<zlink_msg_t> payload (payload_count);
    for (size_t i = 0; i < payload_count; ++i)
        zlink_msg_init (&payload[i]);
    for (size_t i = 0; i < state->send.buffered_parts.size (); ++i)
        zlink_msg_move (&payload[i], &state->send.buffered_parts[i]);
    zlink_msg_move (&payload[payload_count - 1], part_);

    if (reqrep::send_request_reply_message (
          handle.socket, peer_rid_, &payload[0], payload_count,
          ZLINK_SEND_FLAGS_NONE, zlink::request_reply::completion_control_type, 0)
        != 0) {
        const int saved_errno = errno;
        zlink::request_reply::close_built_parts (&payload);
        zlink::part_helper_internal::abort_send_step (state);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    zlink::request_reply::close_built_parts (&payload);
    zlink::part_helper_internal::complete_send_step (state, part_flag_);
    return ZLINK_SUBMIT_OK;
}

zlink_submit_result_t zlink_dealer_reply_part (void *dealer_,
                                               uint64_t request_seq_,
                                               zlink_msg_t *part_,
                                               zlink_part_flag_t part_flag_)
{
    if (zlink::part_helper_internal::validate_part_flag (part_flag_) != 0) {
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (request_seq_ == 0) {
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (reqrep::validate_socket_type (dealer_, ZLINK_CORE_SOCKET_DEALER) != 0) {
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    socket_handle_t handle = as_socket_handle (dealer_);
    std::shared_ptr<reqrep::socket_request_reply_state_t> request_state =
      reqrep::find_request_reply_state (handle);
    if (!request_state) {
        zlink::part_helper_internal::consume_send_part (part_);
        errno = ENOENT;
        return zlink::submit_result_internal::from_errno (errno);
    }

    zlink::part_helper_internal::send_sequence_spec_t spec;
    spec.family = zlink::part_helper_internal::send_family_dealer_reply;
    spec.request_like = true;
    spec.request_seq = request_seq_;

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> state;
    bool first_part = false;
    if (zlink::part_helper_internal::prepare_send_step (dealer_, spec, handle.socket, &state,
                                                        &first_part)
        != 0) {
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    if (part_flag_ == ZLINK_PART_MORE) {
        if (reqrep::stage_request_payload_part (state.get (), part_) != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return zlink::submit_result_internal::from_errno (saved_errno);
        }
        return ZLINK_SUBMIT_OK;
    }

    reqrep::dealer_reply_target_t target;
    if (reqrep::take_dealer_reply_target (request_state, request_seq_, &target) != 0) {
        const int saved_errno = errno;
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    if (send_dealer_reply_to_target (handle.socket, target, state.get (), part_) != 0) {
        const int saved_errno = errno;
        reqrep::restore_dealer_reply_target (request_state, request_seq_, target);
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    reqrep::release_reply_target_slot (request_state);
    zlink::part_helper_internal::complete_send_step (state, part_flag_);
    return ZLINK_SUBMIT_OK;
}
