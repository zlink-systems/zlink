/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

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
struct pending_pair_observer_t
{
    pending_pair_observer_t () :
        state (NULL),
        identity (NULL),
        pending (NULL),
        accounted_bytes (0),
        reserved_pipe (NULL),
        reservation_committed (false)
    {
    }

    // Pipe write observers finish before the tracked send returns. Borrow the
    // caller's owner instead of adding shared_ptr atomics to every request.
    const std::shared_ptr<reqrep::socket_request_reply_state_t> *state;
    const reqrep::pending_request_identity_t *identity;
    // The publication lock stays held from prepare through finish. The map
    // entry therefore cannot be erased or invalidated before commit, so keep
    // the resolved entry instead of hashing the same sequence twice.
    reqrep::pending_request_t *pending;
    uint64_t accounted_bytes;
    zlink::pipe_t *reserved_pipe;
    bool reservation_committed;
    std::unique_lock<std::mutex> publication_lock;
};

uint64_t request_correlation_accounted_bytes (
  zlink_msg_t *staged_parts_, size_t staged_part_count_, zlink_msg_t *final_part_)
{
    uint64_t total = 0;
    for (size_t i = 0; i <= staged_part_count_; ++i) {
        zlink_msg_t *const part =
          i < staged_part_count_ ? &staged_parts_[i] : final_part_;
        const uint64_t frame = zlink::pipe_t::frame_accounted_bytes (
          reinterpret_cast<zlink::msg_t *> (part));
        if (UINT64_MAX - total < frame)
            return UINT64_MAX;
        total += frame;
    }
    return total == 0 ? 1 : total;
}

bool publish_pending_pair_before_flush (
  zlink::pipe_t *pipe_, void *userdata_,
  zlink::pipe_write_observer_phase_t phase_)
{
    pending_pair_observer_t *observer =
      static_cast<pending_pair_observer_t *> (userdata_);
    if (!observer || !observer->state || !*observer->state
        || !observer->identity) {
        errno = ECANCELED;
        return false;
    }

    reqrep::socket_request_reply_state_t *const state =
      observer->state->get ();

    if (phase_ == zlink::pipe_write_observer_prepare) {
        observer->pending = NULL;
        observer->reserved_pipe = NULL;
        observer->reservation_committed = false;
        observer->publication_lock =
          std::unique_lock<std::mutex> (state->mutex);
        std::unordered_map<uint64_t, reqrep::pending_request_t>::iterator
          pending = state->pending_requests.find (
            observer->identity->request_seq);
        if (state->closing
            || pending == state->pending_requests.end ()
            || !(pending->second.identity == *observer->identity)) {
            observer->publication_lock.unlock ();
            errno = ECANCELED;
            return false;
        }
        if (!pipe_ || !pipe_->retain_lifetime_ref ()) {
            observer->publication_lock.unlock ();
            errno = EHOSTUNREACH;
            return false;
        }
        if (!pipe_->try_reserve_request_correlation (
              observer->accounted_bytes)) {
            const int saved_errno = errno;
            pipe_->release_lifetime_ref ();
            observer->publication_lock.unlock ();
            errno = saved_errno;
            return false;
        }
        observer->pending = &pending->second;
        observer->reserved_pipe = pipe_;
        return true;
    }

    if (phase_ == zlink::pipe_write_observer_commit) {
        if (!pipe_ || !observer->pending
            || !observer->publication_lock.owns_lock ()) {
            errno = ECANCELED;
            return false;
        }
        observer->pending->transport_pair_id = pipe_->get_transport_pair_id ();
        observer->pending->transport_pair_generation =
          pipe_->get_transport_pair_generation ();
        const bool valid_pair = observer->pending->transport_pair_id != 0
                                && observer->pending->transport_pair_generation != 0;
        if (valid_pair) {
            observer->pending->correlation.adopt (
              observer->reserved_pipe, observer->accounted_bytes);
            observer->reservation_committed = true;
        }
        return valid_pair;
    }

    zlink::pipe_t *const uncommitted_pipe =
      observer->reservation_committed ? NULL : observer->reserved_pipe;
    observer->pending = NULL;
    observer->reserved_pipe = NULL;
    if (observer->publication_lock.owns_lock ())
        observer->publication_lock.unlock ();
    if (uncommitted_pipe) {
        uncommitted_pipe->release_request_correlation (
          observer->accounted_bytes);
        uncommitted_pipe->release_lifetime_ref ();
    }
    return true;
}

zlink_submit_result_t finish_request_submit_failure (
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  const reqrep::pending_request_identity_t &identity_,
  zlink_submit_result_t failure_)
{
    // A timeout or disconnect may remove the pending entry while the send
    // operation is still unwinding. In that case Core has already retained a
    // completion callback and the caller must receive a successful submit
    // result so its callback userdata remains owned by that completion.
    if (!reqrep::erase_socket_pending_request (state_, identity_)) {
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

    return reqrep::send_completion_staged_frames (
      socket_, target_.pipe, NULL,
      helper_state_->send.buffered_parts.empty ()
        ? NULL
        : &helper_state_->send.buffered_parts[0],
      helper_state_->send.buffered_parts.size (), final_part_);
}

void release_dealer_reply_target_pipe (
  const reqrep::dealer_reply_target_t &target_)
{
    if (!target_.pipe)
        return;
    const int saved_errno = errno;
    target_.pipe->release_lifetime_ref ();
    errno = saved_errno;
}

bool message_has_group (const zlink_msg_t *part_)
{
    if (!part_)
        return false;
    const zlink::msg_t *msg = reinterpret_cast<const zlink::msg_t *> (part_);
    if (!msg->check ())
        return false;
    const char *group = msg->group ();
    return group && group[0] != '\0';
}

int attach_request_reply_metadata (zlink_msg_t *part_,
                                   uint8_t message_type_,
                                   uint64_t request_seq_)
{
    if (!part_) {
        errno = EFAULT;
        return -1;
    }
    zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (part_);
    if (!msg->check ()) {
        errno = EFAULT;
        return -1;
    }
    return msg->set_request_reply_metadata (message_type_, request_seq_);
}

zlink_submit_result_t request_part_common (
  const socket_handle_t &socket_handle_,
  void *handle_,
  const zlink_routing_id_t *peer_rid_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  zlink_reply_handler_fn handler_,
  void *userdata_,
  zlink::part_helper_internal::send_family_t family_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_)
{
    if (zlink::part_helper_internal::validate_part_flag (part_flag_) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (reqrep::validate_request_send_flags (flags_) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (part_flag_ == ZLINK_PART_MORE
        && (timeout_ms_ != 0 || handler_ != NULL || userdata_ != NULL)) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }

    if (!socket_handle_.socket) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EFAULT;
        return zlink::submit_result_internal::from_errno (errno);
    }

    zlink::msg_t *core_part = reinterpret_cast<zlink::msg_t *> (part_);
    if (!part_ || !core_part->check ()) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
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
    spec.transport_pair_id = transport_pair_id_;
    spec.transport_pair_generation = transport_pair_generation_;
    if (peer_rid_) {
        spec.has_rid1 = true;
        zlink::part_helper_internal::copy_routing_id (peer_rid_, &spec.rid1);
    }

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      zlink::part_helper_internal::find_socket_state (socket_handle_.socket);
    bool helper_state_matches_family = false;
    bool helper_send_active = false;
    if (helper_state) {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        helper_send_active = helper_state->send.active;
        if (helper_state->send.active && helper_state->send.spec.family == family_) {
            spec.request_seq = helper_state->send.spec.request_seq;
            spec.pending_cookie = helper_state->send.spec.pending_cookie;
            helper_state_matches_family = true;
        }
    }

    if (part_flag_ == ZLINK_PART_MORE && !handler_ && spec.request_seq == 0) {
        if (!helper_send_active && message_has_group (part_)) {
            zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = EINVAL;
            return ZLINK_SUBMIT_INVALID_ARGUMENT;
        }
        std::shared_ptr<zlink::part_helper_internal::handle_state_t> state;
        bool first_part = false;
        if (zlink::part_helper_internal::prepare_send_step (
              handle_, spec, socket_handle_.socket, &state, &first_part)
            != 0) {
            zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
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

        zlink::part_helper_internal::complete_send_step (state,
                                                         ZLINK_PART_MORE);
        return ZLINK_SUBMIT_OK;
    }

    zlink_msg_t *metadata_part = part_;
    if (helper_state_matches_family && !helper_state->send.buffered_parts.empty ())
        metadata_part = &helper_state->send.buffered_parts[0];
    if (message_has_group (metadata_part)) {
        if (helper_state_matches_family)
            zlink::part_helper_internal::abort_send_step (helper_state);
        else
            zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }

    std::shared_ptr<reqrep::socket_request_reply_state_t> request_state;
    reqrep::pending_request_token_t pending_token;
    if (spec.request_seq == 0) {
        if (reqrep::ensure_socket_pending_request (
              socket_handle_, timeout_ms_, handler_, userdata_,
              &spec.request_seq, &request_state, &pending_token)
            != 0) {
            zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
            zlink::part_helper_internal::consume_send_part (part_);
            return zlink::submit_result_internal::from_errno (errno);
        }
    } else {
        request_state = reqrep::find_or_create_request_reply_state (socket_handle_);
        reqrep::pending_request_identity_t identity;
        identity.request_seq = spec.request_seq;
        identity.cookie = spec.pending_cookie;
        if (!request_state
            || reqrep::lookup_socket_pending_request (request_state, identity,
                                                      &pending_token)
                 != 0) {
            zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
            zlink::part_helper_internal::consume_send_part (part_);
            return zlink::submit_result_internal::from_errno (errno);
        }
    }
    spec.pending_cookie = pending_token.identity.cookie;

    // The one-part path publishes correlation after the selected candidate
    // accepts the write but before its final flush. Selection therefore keeps
    // the ordinary DEALER retry/weight-commit rules while an eager reply can
    // never overtake the pending-pair record.
    if (part_flag_ == ZLINK_PART_FINAL && !helper_send_active
        && transport_pair_id_ == 0 && transport_pair_generation_ == 0) {
        if (attach_request_reply_metadata (
              part_, zlink::request_reply::request_type, spec.request_seq)
            != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return finish_request_submit_failure (
              request_state, pending_token.identity,
              zlink::submit_result_internal::from_errno (saved_errno));
        }

        pending_pair_observer_t pair_observer;
        pair_observer.state = &request_state;
        pair_observer.identity = &pending_token.identity;
        pair_observer.accounted_bytes = request_correlation_accounted_bytes (
          NULL, 0, part_);
        const int send_rc =
          peer_rid_
            ? zlink::logical_multipart_send_routed_tracked (
                socket_handle_.socket, peer_rid_, part_, 1, flags_,
                &publish_pending_pair_before_flush, &pair_observer)
            : zlink::logical_multipart_send_tracked (
                socket_handle_.socket, part_, 1, flags_,
                &publish_pending_pair_before_flush, &pair_observer);
        const int saved_errno = errno;
        if (send_rc != 0) {
            // Scoped-send admission may fail before it takes ownership. The C
            // part contract still consumes the input, and clearing it here
            // also prevents internal metadata from escaping on that path.
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return finish_request_submit_failure (
              request_state, pending_token.identity,
              zlink::submit_result_internal::from_errno (saved_errno));
        }
        if (reqrep::arm_socket_pending_request_timeout (request_state, pending_token) != 0) {
            const int saved_errno = errno;
            return finish_request_submit_failure (
              request_state, pending_token.identity,
              zlink::submit_result_internal::from_errno (saved_errno));
        }
        return ZLINK_SUBMIT_OK;
    }

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> state;
    bool first_part = false;
    if (zlink::part_helper_internal::prepare_send_step (
          handle_, spec, socket_handle_.socket, &state, &first_part)
        != 0) {
        const zlink_submit_result_t failure =
          zlink::submit_result_internal::from_errno (errno);
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
        zlink::part_helper_internal::consume_send_part (part_);
        return spec.request_seq != 0
                 ? finish_request_submit_failure (request_state, pending_token.identity, failure)
                 : failure;
    }

    if (part_flag_ == ZLINK_PART_MORE) {
        if (first_part && message_has_group (part_)) {
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = EINVAL;
            return ZLINK_SUBMIT_INVALID_ARGUMENT;
        }
        if (reqrep::stage_request_payload_part (state.get (), part_) != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return finish_request_submit_failure (
              request_state, pending_token.identity,
              zlink::submit_result_internal::from_errno (saved_errno));
        }

        zlink::part_helper_internal::complete_send_step (state,
                                                         ZLINK_PART_MORE);
        return ZLINK_SUBMIT_OK;
    }

    zlink_msg_t *first_payload = state->send.buffered_parts.empty ()
                                  ? part_
                                  : &state->send.buffered_parts[0];
    if (attach_request_reply_metadata (
          first_payload, zlink::request_reply::request_type, spec.request_seq)
        != 0) {
        const int saved_errno = errno;
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return finish_request_submit_failure (
          request_state, pending_token.identity,
          zlink::submit_result_internal::from_errno (saved_errno));
    }

    pending_pair_observer_t pair_observer;
    pair_observer.state = &request_state;
    pair_observer.identity = &pending_token.identity;
    pair_observer.accounted_bytes = request_correlation_accounted_bytes (
      state->send.buffered_parts.empty ()
        ? NULL
        : &state->send.buffered_parts[0],
      state->send.buffered_parts.size (), part_);
    bool physical_first_part = true;
    bool pending_pair_recorded = false;
    for (size_t i = 0; i < state->send.buffered_parts.size (); ++i) {
        const bool observe_first_pipe =
          physical_first_part && !pending_pair_recorded;
        if (reqrep::send_request_payload_part (socket_handle_.socket, state.get (), peer_rid_,
                                               &state->send.buffered_parts[i], flags_,
                                               ZLINK_PART_MORE, physical_first_part,
                                               spec.transport_pair_id,
                                               spec.transport_pair_generation,
                                               NULL,
                                               observe_first_pipe
                                                 ? &publish_pending_pair_before_flush
                                                 : NULL,
                                               observe_first_pipe ? &pair_observer
                                                                  : NULL)
            != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return finish_request_submit_failure (
              request_state, pending_token.identity,
              zlink::submit_result_internal::from_errno (saved_errno));
        }
        if (observe_first_pipe)
            pending_pair_recorded = true;
        physical_first_part = false;
    }

    const bool observe_first_pipe =
      physical_first_part && !pending_pair_recorded;
    if (reqrep::send_request_payload_part (
          socket_handle_.socket, state.get (), peer_rid_, part_, flags_, part_flag_,
          physical_first_part, spec.transport_pair_id,
          spec.transport_pair_generation,
          NULL,
          observe_first_pipe ? &publish_pending_pair_before_flush : NULL,
          observe_first_pipe ? &pair_observer : NULL)
        != 0) {
        const int saved_errno = errno;
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return finish_request_submit_failure (
          request_state, pending_token.identity,
          zlink::submit_result_internal::from_errno (saved_errno));
    }
    if (reqrep::arm_socket_pending_request_timeout (request_state, pending_token) != 0) {
        const int saved_errno = errno;
        zlink::part_helper_internal::abort_send_step (state);
        errno = saved_errno;
        return finish_request_submit_failure (
          request_state, pending_token.identity,
          zlink::submit_result_internal::from_errno (saved_errno));
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
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (dealer_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }
    socket_handle_t handle = as_socket_handle (dealer_);
    if (reqrep::validate_socket_type (handle, ZLINK_CORE_SOCKET_DEALER) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (dealer_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    return request_part_common (
      handle, dealer_, NULL, part_, flags_, part_flag_, timeout_ms_, handler_,
      userdata_, zlink::part_helper_internal::send_family_dealer_request, 0, 0);
}

zlink_submit_result_t zlink_dealer_request_transport_pair_part (
  void *dealer_, const zlink_routed_submit_target_t *target_,
  zlink_msg_t *part_, zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_, uint32_t timeout_ms_,
  zlink_reply_handler_fn handler_, void *userdata_)
{
    if (!target_ || !zlink::valid_routing_id (&target_->peer_rid)
        || target_->transport_pair_id == 0
        || target_->transport_pair_generation == 0
        || (!handler_ && part_flag_ == ZLINK_PART_FINAL)) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (dealer_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }
    socket_handle_t handle = as_socket_handle (dealer_);
    if (reqrep::validate_socket_type (handle, ZLINK_CORE_SOCKET_DEALER)
        != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (dealer_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    return request_part_common (
      handle, dealer_, &target_->peer_rid, part_, flags_, part_flag_, timeout_ms_,
      handler_, userdata_,
      zlink::part_helper_internal::send_family_dealer_request,
      target_->transport_pair_id, target_->transport_pair_generation);
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
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (router_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }
    socket_handle_t handle = as_socket_handle (router_);
    if (reqrep::validate_socket_type (handle, ZLINK_CORE_SOCKET_ROUTER) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (router_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    return request_part_common (handle, router_, peer_rid_, part_, flags_, part_flag_,
                                timeout_ms_, handler_, userdata_,
                                zlink::part_helper_internal::send_family_router_request,
                                0, 0);
}

zlink_submit_result_t zlink_router_request_transport_pair_part (
  void *router_,
  const zlink_routing_id_t *peer_rid_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  zlink_reply_handler_fn handler_,
  void *userdata_)
{
    if ((!handler_ && part_flag_ == ZLINK_PART_FINAL)
        || !zlink::valid_routing_id (peer_rid_)
        || transport_pair_id_ == 0 || transport_pair_generation_ == 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (router_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }
    socket_handle_t handle = as_socket_handle (router_);
    if (reqrep::validate_socket_type (handle, ZLINK_CORE_SOCKET_ROUTER) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (router_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    return request_part_common (
      handle, router_, peer_rid_, part_, flags_, part_flag_, timeout_ms_, handler_, userdata_,
      zlink::part_helper_internal::send_family_router_request,
      transport_pair_id_, transport_pair_generation_);
}

zlink_submit_result_t zlink_router_reply_part (void *router_,
                                               const zlink_routing_id_t *peer_rid_,
                                               uint64_t request_seq_,
                                               zlink_msg_t *part_,
                                               zlink_part_flag_t part_flag_)
{
    if (zlink::part_helper_internal::validate_part_flag (part_flag_) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (router_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (!zlink::valid_routing_id (peer_rid_) || request_seq_ == 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (router_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }
    socket_handle_t handle = as_socket_handle (router_);
    if (reqrep::validate_socket_type (handle, ZLINK_CORE_SOCKET_ROUTER) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (router_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    zlink::msg_t *core_part = reinterpret_cast<zlink::msg_t *> (part_);
    if (!part_ || !core_part->check ()) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (router_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EFAULT;
        return zlink::submit_result_internal::from_errno (errno);
    }

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
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (router_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    if (part_flag_ == ZLINK_PART_MORE) {
        if (first_part && message_has_group (part_)) {
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = EINVAL;
            return ZLINK_SUBMIT_INVALID_ARGUMENT;
        }
        if (reqrep::stage_request_payload_part (state.get (), part_) != 0) {
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

    zlink_msg_t *first_payload = state->send.buffered_parts.empty ()
                                  ? part_
                                  : &state->send.buffered_parts[0];
    const bool first_payload_has_group = message_has_group (first_payload);
    if (first_payload_has_group) {
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }

    if (reqrep::send_request_reply_message (
          handle, peer_rid_,
          state->send.buffered_parts.empty () ? NULL
                                               : &state->send.buffered_parts[0],
          state->send.buffered_parts.size (), part_, ZLINK_SEND_FLAGS_NONE,
          zlink::request_reply::reply_type, request_seq_)
        != 0) {
        const int saved_errno = errno;
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    zlink::part_helper_internal::complete_send_step (state, part_flag_);
    return ZLINK_SUBMIT_OK;
}

zlink_submit_result_t zlink_dealer_reply_part (void *dealer_,
                                               uint64_t request_seq_,
                                               zlink_msg_t *part_,
                                               zlink_part_flag_t part_flag_)
{
    if (zlink::part_helper_internal::validate_part_flag (part_flag_) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (dealer_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (request_seq_ == 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (dealer_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }
    socket_handle_t handle = as_socket_handle (dealer_);
    if (reqrep::validate_socket_type (handle, ZLINK_CORE_SOCKET_DEALER) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (dealer_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    std::shared_ptr<reqrep::socket_request_reply_state_t> request_state =
      reqrep::find_request_reply_state (handle);
    if (!request_state) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (dealer_);
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
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (dealer_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    if (part_flag_ == ZLINK_PART_MORE) {
        if (first_part && message_has_group (part_)) {
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = EINVAL;
            return ZLINK_SUBMIT_INVALID_ARGUMENT;
        }
        if (reqrep::stage_request_payload_part (state.get (), part_) != 0) {
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

    zlink_msg_t *first_payload = state->send.buffered_parts.empty ()
                                  ? part_
                                  : &state->send.buffered_parts[0];
    if (message_has_group (first_payload)) {
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }

    //  DEALER replies bypass send()/recv() just like ROUTER replies. Apply a
    //  queued pair-state transition or termination before selecting the
    //  retained target; the no-command path remains mailbox-free.
    if (handle.socket->process_submit_commands () != 0) {
        const int saved_errno = errno;
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    reqrep::dealer_reply_target_t target;
    if (reqrep::take_dealer_reply_target (request_state, request_seq_, &target) != 0) {
        const int saved_errno = errno;
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    if (attach_request_reply_metadata (
          first_payload, zlink::request_reply::reply_type,
          target.request_seq)
        != 0) {
        const int saved_errno = errno;
        reqrep::restore_dealer_reply_target (request_state, request_seq_);
        release_dealer_reply_target_pipe (target);
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    if (send_dealer_reply_to_target (handle.socket, target, state.get (), part_) != 0) {
        const int saved_errno = errno;
        reqrep::restore_dealer_reply_target (request_state, request_seq_);
        release_dealer_reply_target_pipe (target);
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    reqrep::commit_dealer_reply_target (request_state, request_seq_);
    release_dealer_reply_target_pipe (target);
    zlink::part_helper_internal::complete_send_step (state, part_flag_);
    return ZLINK_SUBMIT_OK;
}
