/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_request_reply_pending_internal.hpp"
#include "api/socket/socket_request_reply_submit_internal.hpp"
#include "api/message/request_result_internal.hpp"
#include "api/message/submit_result_internal.hpp"
#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "utils/routing_id.hpp"

namespace reqrep = zlink::socket_reqrep_internal;

namespace
{
struct pending_pair_observer_t
{
    pending_pair_observer_t () :
        state (),
        identity (),
        pending (NULL),
        accounted_bytes (0),
        reserved_pipe (NULL),
        reservation_committed (false),
        request_wait (NULL),
        observer_errno (0)
    {
    }

    // The observer lives through the single physical admission attempt.
    std::shared_ptr<reqrep::socket_request_reply_state_t> state;
    reqrep::pending_request_identity_t identity;
    // The publication lock stays held from prepare through finish. The
    // pending entry therefore cannot be erased or invalidated before commit, so keep
    // the resolved entry instead of hashing the same sequence twice.
    reqrep::pending_request_t *pending;
    uint64_t accounted_bytes;
    zlink::pipe_t *reserved_pipe;
    bool reservation_committed;
    zlink::socket_completion::request_writable_wait_t *request_wait;
    int observer_errno;
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
    if (!observer || !observer->state
        || observer->identity.request_seq == 0
        || observer->identity.cookie == 0) {
        errno = ECANCELED;
        return false;
    }

    reqrep::socket_request_reply_state_t *const state =
      observer->state.get ();

    if (phase_ == zlink::pipe_write_observer_prepare) {
        observer->pending = NULL;
        observer->reserved_pipe = NULL;
        observer->reservation_committed = false;
        observer->publication_lock =
          std::unique_lock<std::mutex> (state->mutex);
        reqrep::pending_request_store_t::iterator
          pending = state->pending_requests.find (
            observer->identity.request_seq);
        if (state->closing
            || pending == state->pending_requests.end ()
            || !(pending->second.identity == observer->identity)) {
            observer->publication_lock.unlock ();
            errno = ECANCELED;
            return false;
        }
        if (!pipe_) {
            observer->publication_lock.unlock ();
            errno = EHOSTUNREACH;
            return false;
        }
        if (!pipe_->retain_lifetime_ref ()) {
            observer->publication_lock.unlock ();
            errno = EHOSTUNREACH;
            return false;
        }
        uint64_t release_epoch = 0;
        if (!pipe_->try_reserve_request_correlation (
              observer->accounted_bytes, &release_epoch)) {
            const int saved_errno = errno;
            observer->publication_lock.unlock ();
            if (saved_errno == ENOBUFS && observer->request_wait) {
                try {
                    std::shared_ptr<zlink::pipe_t> refused (
                      pipe_, [] (zlink::pipe_t *p_) { p_->release_lifetime_ref (); });
                    observer->request_wait->push_back (
                      std::make_pair (refused, release_epoch));
                } catch (const std::bad_alloc &) {
                    observer->observer_errno = ENOMEM;
                    errno = ENOMEM;
                    return false;
                }
            } else
                pipe_->release_lifetime_ref ();
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

int submit_pull_dontwait_request (
  zlink::socket_base_t *socket_, const zlink_routing_id_t *peer_rid_,
  zlink_msg_t *parts_, size_t part_count_,
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  const reqrep::pending_request_token_t &token_,
  zlink::socket_completion::request_writable_wait_t *request_wait_)
{
    if (!socket_ || !parts_ || part_count_ == 0 || !state_) {
        errno = EFAULT;
        return -1;
    }

    pending_pair_observer_t observer;
    observer.state = state_;
    observer.request_wait = request_wait_;
    observer.identity = token_.identity;
    observer.accounted_bytes = request_correlation_accounted_bytes (
      parts_, part_count_ - 1, &parts_[part_count_ - 1]);

    const int rc = socket_->request_admission_submit (
      parts_, part_count_, peer_rid_, &publish_pending_pair_before_flush,
      &observer);
    // The transport admission enum does not classify observer allocation
    // failures. Preserve that error alongside the correlation refusal cause.
    const int saved_errno =
      observer.observer_errno ? observer.observer_errno : errno;
    if (rc != 0) {
        errno = saved_errno;
        return -1;
    }

    (void) reqrep::arm_socket_pending_request_timeout (state_, token_);
    errno = 0;
    return 0;
}

int submit_pull_blocking_request (
  zlink::socket_base_t *socket_, const zlink_routing_id_t *peer_rid_,
  zlink_msg_t *parts_, size_t part_count_,
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  const reqrep::pending_request_token_t &token_)
{
    if (!socket_ || !parts_ || part_count_ == 0 || !state_) {
        errno = EFAULT;
        return -1;
    }

    pending_pair_observer_t observer;
    observer.state = state_;
    observer.identity = token_.identity;
    observer.accounted_bytes = request_correlation_accounted_bytes (
      parts_, part_count_ - 1, &parts_[part_count_ - 1]);
    if (socket_->request_admission_submit_blocking (
          parts_, part_count_, peer_rid_,
          &publish_pending_pair_before_flush, &observer)
        != 0)
        return -1;

    return reqrep::arm_socket_pending_request_timeout (state_, token_);
}

zlink_submit_result_t finish_request_submit_failure (
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  const reqrep::pending_request_identity_t &identity_,
  zlink_submit_result_t failure_)
{
    // A timeout or disconnect may remove the pending entry while the send
    // operation is still unwinding. In that case Core has already retained a
    // completion reservation and the caller must receive a successful submit
    // result so the accepted operation remains owned by that completion.
    if (!reqrep::erase_socket_pending_request (state_, identity_)) {
        errno = 0;
        return ZLINK_SUBMIT_OK;
    }
    return failure_;
}

zlink_submit_result_t finish_dontwait_request_admission_failure (
  zlink::socket_base_t *socket_, const zlink_routing_id_t *peer_rid_,
  void *user_context_,
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  const reqrep::pending_request_identity_t &identity_, int failure_errno_,
  zlink_completion_id_t *completion_id_out_,
  zlink::socket_completion::request_writable_wait_t *request_wait_)
{
    // A mandatory ROUTER route miss is connectivity, not lookup ownership.
    const int normalized_errno =
      peer_rid_ && failure_errno_ == ENOENT ? EHOSTUNREACH : failure_errno_;
    errno = normalized_errno;
    const zlink_submit_result_t failure = finish_request_submit_failure (
      state_, identity_,
      zlink::submit_result_internal::from_errno (normalized_errno));
    if (failure == ZLINK_SUBMIT_OK || normalized_errno != EAGAIN)
        return failure;

    if (socket_->register_send_writable_wait_after_failure (
          normalized_errno, peer_rid_, user_context_, completion_id_out_,
          request_wait_)
        != 0)
        return zlink::submit_result_internal::from_errno (errno);
    return ZLINK_SUBMIT_BACKPRESSURED;
}

bool message_has_group (const zlink_msg_t *part_);
int attach_request_reply_metadata (zlink_msg_t *part_,
                                   uint8_t message_type_,
                                   uint64_t request_seq_);

int checkout_public_router_reply_target (
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  const zlink_routing_id_t *peer_rid_, zlink_reply_token_t token_,
  reqrep::router_reply_target_t *target_out_)
{
    if (!state_ || !target_out_ || !zlink::valid_routing_id (peer_rid_)
        || token_ == 0) {
        errno = EFAULT;
        return -1;
    }

    const std::thread::id owner = std::this_thread::get_id ();
    std::unique_lock<std::mutex> lock (state_->mutex);
    if (state_->public_router_reply_active
        && state_->public_router_reply_owner != owner) {
        errno = EBUSY;
        return -1;
    }

#ifdef ZLINK_BUILD_TESTS
    try {
        reqrep::test_throw_request_reply_allocation_failpoint (
          reqrep::request_reply_allocation_reply_key);
    } catch (...) {
        // A continuation already owns the token checkout. Allocation failure
        // aborts that sequence and must make the live token available to a
        // fresh owner; a first-part failure has no checkout to abandon.
        const uint64_t active_checkout_token =
          state_->public_router_reply_active
            ? state_->public_router_reply_token
            : 0;
        lock.unlock ();
        if (active_checkout_token != 0)
            reqrep::abandon_public_router_reply_sequence (
              state_, active_checkout_token);
        errno = ENOMEM;
        return -1;
    }
#endif

    if (state_->public_router_reply_active) {
        const reqrep::fixed_routing_id_key_t &active_rid =
          state_->public_router_reply_target.peer_rid;
        const bool same_rid =
          !active_rid.empty () && active_rid.size () == peer_rid_->size
          && memcmp (active_rid.data (), peer_rid_->data,
                     peer_rid_->size)
               == 0;
        if (state_->public_router_reply_token != token_ || !same_rid) {
            const uint64_t active_checkout_token =
              state_->public_router_reply_token;
            lock.unlock ();
            reqrep::abandon_public_router_reply_sequence (
              state_, active_checkout_token);
            errno = EINVAL;
            return -1;
        }
        *target_out_ = state_->public_router_reply_target;
        return 0;
    }

    reqrep::router_reply_target_t target;
    if (!reqrep::take_router_reply_target_locked (
          state_.get (), token_, peer_rid_, &target)) {
        errno = ENOENT;
        return -1;
    }
    if (state_->closing) {
        lock.unlock ();
        reqrep::restore_router_reply_target (state_, token_);
        if (target.pipe)
            target.pipe->release_lifetime_ref ();
        errno = ESHUTDOWN;
        return -1;
    }

    state_->public_router_reply_active = true;
    state_->public_router_reply_owner = owner;
    state_->public_router_reply_token = token_;
    state_->public_router_reply_target = target;
    state_->public_router_reply_checkout_token.store (
      token_, std::memory_order_release);
    *target_out_ = target;
    return 0;
}

int validate_public_router_reply_checkout (
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  zlink_reply_token_t token_)
{
    if (!state_) {
        errno = EFAULT;
        return -1;
    }

    if (state_->public_router_reply_checkout_token.load (
          std::memory_order_acquire)
        == token_)
        return 0;

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->closing) {
        errno = ESHUTDOWN;
        return -1;
    }
    errno = ENOENT;
    return -1;
}

int send_public_router_reply_with_wait (
  zlink::socket_base_t *socket_,
  zlink::socket_public_send_scope_t &send_scope_,
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &request_state_,
  const reqrep::router_reply_target_t &target_,
  const zlink_routing_id_t *peer_rid_, zlink_reply_token_t reply_token_,
  zlink_msg_t *staged_parts_,
  size_t staged_part_count_, zlink_msg_t *final_part_, int timeout_ms_,
  const std::chrono::steady_clock::time_point &started_at_)
{
    const bool infinite = timeout_ms_ < 0;
    const std::chrono::steady_clock::time_point deadline =
      infinite
        ? std::chrono::steady_clock::time_point::max ()
        : started_at_ + std::chrono::milliseconds (timeout_ms_);
    zlink::transport_pair_owner_progress_scope_t progress_owner (socket_);

    for (;;) {
        if (socket_->is_ctx_terminated ()) {
            errno = ETERM;
            return -1;
        }
        if (socket_->process_submit_commands () != 0)
            return -1;
        if (validate_public_router_reply_checkout (request_state_, reply_token_)
            != 0)
            return -1;

        // Arm the progress generation before physical admission. Credit can
        // return immediately after the pipe reports HWM-full; observing only
        // after that failure would fold the activation into the waiter's
        // starting generation and sleep until SNDTIMEO despite a writable
        // pipe.
        const uint64_t observed_progress =
          socket_->observe_submit_progress ();
        zlink::pipe_message_admission_t last_admission =
          zlink::pipe_message_admission_invalid;
        zlink::pipe_t *const reply_transport =
          reqrep::retain_reply_transport_pipe (socket_, target_, peer_rid_);
        if (reply_transport) {
            const int rc = reqrep::send_completion_staged_frames_on_pipe (
              reply_transport, staged_parts_, staged_part_count_, final_part_,
              true, &last_admission);
            if (rc == 0)
                return 0;
            if (last_admission != zlink::pipe_message_admission_hwm_full
                && last_admission
                     != zlink::pipe_message_admission_transport_wait
                && last_admission
                     != zlink::pipe_message_admission_inactive)
                return -1;
            // A first-frame detach leaves every input part untouched. Keep
            // waiting for the same logical pair/RID within the entry snapshot.
        } else if (errno != ENOTCONN && errno != EHOSTUNREACH
                   && errno != EAGAIN) {
            return -1;
        }

        const std::chrono::steady_clock::time_point now =
          std::chrono::steady_clock::now ();
        if (!infinite && (timeout_ms_ == 0 || now >= deadline)) {
            errno = EAGAIN;
            return -1;
        }

        int wait_ms = -1;
        if (!infinite) {
            const long long remaining_ms =
              std::chrono::duration_cast<std::chrono::milliseconds> (
                deadline - now)
                .count ();
            wait_ms = remaining_ms > 0 ? static_cast<int> (remaining_ms) : 1;
        }
        const int wait_rc = socket_->wait_submit_progress (
          send_scope_, observed_progress, wait_ms,
          progress_owner.held_state ());
        if (wait_rc != 0)
            return -1;
    }
}

zlink_submit_result_t public_router_reply_submit (
  const socket_handle_t &handle_, void *router_,
  const zlink_routing_id_t *peer_rid_, zlink_reply_token_t reply_token_,
  zlink_msg_t *part_, zlink_part_flag_t part_flag_)
{
    int reply_timeout_ms = 0;
    std::chrono::steady_clock::time_point reply_started_at;
    if (part_flag_ == ZLINK_PART_FINAL) {
        // Snapshot once at FINAL entry. Later option changes cannot extend or
        // shorten this reply sequence's reconnect/admission budget.
        reply_timeout_ms = handle_.socket->send_timeout_ms ();
        reply_started_at = std::chrono::steady_clock::now ();
    }

    std::shared_ptr<reqrep::socket_request_reply_state_t> request_state =
      reqrep::find_request_reply_state (handle_);
    if (!request_state) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (
          router_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = ENOENT;
        return ZLINK_SUBMIT_NOT_FOUND;
    }

    reqrep::router_reply_target_t target;
    if (checkout_public_router_reply_target (
          request_state, peer_rid_, reply_token_, &target)
        != 0) {
        const int saved_errno = errno;
        if (saved_errno != EBUSY) {
            zlink::part_helper_internal::abort_current_non_publish_send_sequence (
              router_);
        }
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    zlink::part_helper_internal::send_sequence_spec_t spec;
    spec.family = zlink::part_helper_internal::send_family_router_reply;
    spec.request_like = true;
    spec.request_seq = reply_token_;
    spec.has_routing_id = true;
    zlink::part_helper_internal::copy_routing_id (
      peer_rid_, &spec.routing_id);

    zlink::part_helper_internal::handle_state_t *state = NULL;
    std::unique_lock<std::mutex> state_lock;
    bool first_part = false;
    const int prepare_rc =
      zlink::part_helper_internal::prepare_send_step_locked (
        spec, handle_.socket, &state, &state_lock, &first_part,
        part_flag_ == ZLINK_PART_MORE);
    if (prepare_rc == 1) {
        // A lone FINAL is one complete record, not an incremental multipart
        // owner. In particular it may coexist with an async complete-record
        // admission that has already incremented the lifecycle count; the
        // socket sync below serializes their physical writes. Treating this as
        // multipart would reject that ordinary race with EINVAL.
        std::optional<zlink::socket_public_send_scope_t> complete_scope;
        if (!handle_.socket->begin_complete_send_scope (&complete_scope)) {
            const int saved_errno = errno;
            reqrep::abandon_public_router_reply_sequence (
              request_state, reply_token_);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return zlink::submit_result_internal::from_errno (saved_errno);
        }

        if (message_has_group (part_)
            || attach_request_reply_metadata (
                 part_, zlink::request_reply::reply_type,
                 target.wire_request_seq)
                 != 0) {
            const int saved_errno = errno != 0 ? errno : EINVAL;
            reqrep::abandon_public_router_reply_sequence (
              request_state, reply_token_);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return zlink::submit_result_internal::from_errno (saved_errno);
        }

        if (send_public_router_reply_with_wait (
              handle_.socket, *complete_scope, request_state, target,
              peer_rid_, reply_token_, NULL, 0, part_, reply_timeout_ms,
              reply_started_at)
            != 0) {
            const int saved_errno = errno;
            reqrep::abandon_public_router_reply_sequence (
              request_state, reply_token_);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return zlink::submit_result_internal::from_errno (saved_errno);
        }

        reqrep::commit_public_router_reply_sequence (request_state,
                                                     reply_token_);
        return ZLINK_SUBMIT_OK;
    }
    if (prepare_rc != 0) {
        const int saved_errno = errno;
        reqrep::abandon_public_router_reply_sequence (request_state,
                                                      reply_token_);
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (
          router_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }
    if (part_flag_ == ZLINK_PART_MORE) {
        if (first_part && message_has_group (part_)) {
            state_lock.unlock ();
            reqrep::abandon_public_router_reply_sequence (
              request_state, reply_token_);
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = EINVAL;
            return ZLINK_SUBMIT_INVALID_ARGUMENT;
        }
        if (reqrep::stage_request_payload_part (state, part_) != 0) {
            const int saved_errno = errno;
            state_lock.unlock ();
            reqrep::abandon_public_router_reply_sequence (
              request_state, reply_token_);
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return zlink::submit_result_internal::from_errno (saved_errno);
        }
        zlink::part_helper_internal::complete_send_step_locked (
          state, ZLINK_PART_MORE);
        return ZLINK_SUBMIT_OK;
    }

    zlink_msg_t *const first_payload =
      state->send.buffered_parts.empty () ? part_
                                          : &state->send.buffered_parts[0];
    if (message_has_group (first_payload)
        || attach_request_reply_metadata (
             first_payload, zlink::request_reply::reply_type,
             target.wire_request_seq)
             != 0) {
        const int saved_errno = errno != 0 ? errno : EINVAL;
        state_lock.unlock ();
        reqrep::abandon_public_router_reply_sequence (request_state,
                                                      reply_token_);
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    state_lock.unlock ();
    if (send_public_router_reply_with_wait (
          handle_.socket, *state->send.send_scope, request_state, target,
          peer_rid_, reply_token_,
          state->send.buffered_parts.empty ()
            ? NULL
            : &state->send.buffered_parts[0],
          state->send.buffered_parts.size (), part_, reply_timeout_ms,
          reply_started_at)
        != 0) {
        const int saved_errno = errno;
        reqrep::abandon_public_router_reply_sequence (request_state,
                                                      reply_token_);
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return zlink::submit_result_internal::from_errno (saved_errno);
    }

    zlink::part_helper_internal::complete_send_step (state, ZLINK_PART_FINAL);
    reqrep::commit_public_router_reply_sequence (request_state, reply_token_);
    return ZLINK_SUBMIT_OK;
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
  void *user_context_,
  zlink::part_helper_internal::send_family_t family_,
  zlink_completion_id_t *completion_id_out_)
{
    zlink::msg_t *core_part = reinterpret_cast<zlink::msg_t *> (part_);
    if (!core_part->check ()) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EFAULT;
        return zlink::submit_result_internal::from_errno (errno);
    }

    zlink::part_helper_internal::send_sequence_spec_t spec;
    spec.family = family_;
    spec.flags = flags_;
    spec.timeout_ms = timeout_ms_;
    spec.request_like = true;
    if (peer_rid_) {
        spec.has_routing_id = true;
        zlink::part_helper_internal::copy_routing_id (
          peer_rid_, &spec.routing_id);
    }

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state;
    bool helper_state_matches_family = false;
    bool helper_send_active = false;
    if (socket_handle_.socket->part_helper_send_active ()) {
        helper_state = zlink::part_helper_internal::find_socket_state (
          socket_handle_.socket);
        if (helper_state) {
            std::lock_guard<std::mutex> lock (helper_state->mutex);
            helper_send_active = helper_state->send.active;
            if (helper_state->send.active
                && helper_state->send.spec.family == family_) {
                spec.request_seq = helper_state->send.spec.request_seq;
                spec.pending_cookie = helper_state->send.spec.pending_cookie;
                helper_state_matches_family = true;
            }
        }
    }

    if (part_flag_ == ZLINK_PART_MORE && spec.request_seq == 0) {
        if (!helper_send_active && message_has_group (part_)) {
            zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = EINVAL;
            return ZLINK_SUBMIT_INVALID_ARGUMENT;
        }
        zlink::part_helper_internal::handle_state_t *state = NULL;
        std::unique_lock<std::mutex> state_lock;
        bool first_part = false;
        if (zlink::part_helper_internal::prepare_send_step_locked (
              spec, socket_handle_.socket, &state, &state_lock,
              &first_part, true)
            != 0) {
            zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
            zlink::part_helper_internal::consume_send_part (part_);
            return zlink::submit_result_internal::from_errno (errno);
        }

        if (reqrep::stage_request_payload_part (state, part_) != 0) {
            const int saved_errno = errno;
            state_lock.unlock ();
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return zlink::submit_result_internal::from_errno (saved_errno);
        }

        zlink::part_helper_internal::complete_send_step_locked (
          state, ZLINK_PART_MORE);
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
    zlink_completion_id_t reserved_completion_id = 0;
    if (spec.request_seq == 0) {
        const int ensure_rc = reqrep::ensure_socket_pull_pending_request (
          socket_handle_, timeout_ms_, peer_rid_, user_context_,
          &spec.request_seq, &request_state, &pending_token,
          &reserved_completion_id);
        if (ensure_rc != 0) {
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
    if (part_flag_ == ZLINK_PART_FINAL && !helper_send_active) {
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

        zlink::socket_completion::request_writable_wait_t request_wait;
        const int submit_rc =
          flags_ == ZLINK_DONTWAIT
            ? submit_pull_dontwait_request (
                socket_handle_.socket, peer_rid_, part_, 1,
                request_state, pending_token, &request_wait)
            : submit_pull_blocking_request (
                socket_handle_.socket, peer_rid_, part_, 1,
                request_state, pending_token);
        if (submit_rc != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::consume_send_part (part_);
            if (flags_ == ZLINK_DONTWAIT)
                return finish_dontwait_request_admission_failure (
                  socket_handle_.socket, peer_rid_, user_context_,
                  request_state, pending_token.identity, saved_errno,
                  completion_id_out_, &request_wait);
            errno = saved_errno;
            return finish_request_submit_failure (
              request_state, pending_token.identity,
              zlink::submit_result_internal::from_errno (saved_errno));
        }
        if (completion_id_out_)
            *completion_id_out_ = reserved_completion_id;
        return ZLINK_SUBMIT_OK;
    }

    zlink::part_helper_internal::handle_state_t *state = NULL;
    std::unique_lock<std::mutex> state_lock;
    bool first_part = false;
    if (zlink::part_helper_internal::prepare_send_step_locked (
          spec, socket_handle_.socket, &state, &state_lock,
          &first_part, true)
        != 0) {
        const zlink_submit_result_t failure =
          zlink::submit_result_internal::from_errno (errno);
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (handle_);
        zlink::part_helper_internal::consume_send_part (part_);
        return finish_request_submit_failure (
          request_state, pending_token.identity, failure);
    }

    if (part_flag_ == ZLINK_PART_MORE) {
        if (first_part && message_has_group (part_)) {
            state_lock.unlock ();
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = EINVAL;
            return ZLINK_SUBMIT_INVALID_ARGUMENT;
        }
        if (reqrep::stage_request_payload_part (state, part_) != 0) {
            const int saved_errno = errno;
            state_lock.unlock ();
            zlink::part_helper_internal::abort_send_step (state);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = saved_errno;
            return finish_request_submit_failure (
              request_state, pending_token.identity,
              zlink::submit_result_internal::from_errno (saved_errno));
        }

        zlink::part_helper_internal::complete_send_step_locked (
          state, ZLINK_PART_MORE);
        return ZLINK_SUBMIT_OK;
    }

    zlink_msg_t *first_payload = state->send.buffered_parts.empty ()
                                  ? part_
                                  : &state->send.buffered_parts[0];
    if (attach_request_reply_metadata (
          first_payload, zlink::request_reply::request_type, spec.request_seq)
        != 0) {
        const int saved_errno = errno;
        state_lock.unlock ();
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return finish_request_submit_failure (
          request_state, pending_token.identity,
          zlink::submit_result_internal::from_errno (saved_errno));
    }

    if (reqrep::stage_request_payload_part (state, part_) != 0) {
        const int saved_errno = errno;
        state_lock.unlock ();
        zlink::part_helper_internal::abort_send_step (state);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = saved_errno;
        return finish_request_submit_failure (
          request_state, pending_token.identity,
          zlink::submit_result_internal::from_errno (saved_errno));
    }

    // request_admission_submit takes its own complete-record public send
    // scope. Transfer the staged opaque handles into inline storage and close
    // the incremental scope in the same helper-state lock turn. The ordinary
    // two-part request therefore performs neither a dynamic allocation nor a
    // second init/move pass.
    zlink::part_helper_internal::send_part_buffer_t request_parts;
    if (zlink::part_helper_internal::take_buffered_send_record_locked (
          state, &request_parts)
        != 0) {
        const int saved_errno = errno;
        state_lock.unlock ();
        zlink::part_helper_internal::abort_send_step (state);
        errno = saved_errno;
        return finish_request_submit_failure (
          request_state, pending_token.identity,
          zlink::submit_result_internal::from_errno (saved_errno));
    }
    state_lock.unlock ();
    zlink::socket_completion::request_writable_wait_t request_wait;
    const int submit_rc =
      flags_ == ZLINK_DONTWAIT
        ? submit_pull_dontwait_request (
            socket_handle_.socket, peer_rid_, request_parts.data (),
            request_parts.size (), request_state, pending_token, &request_wait)
        : submit_pull_blocking_request (
            socket_handle_.socket, peer_rid_, request_parts.data (),
            request_parts.size (), request_state, pending_token);
    if (submit_rc != 0) {
        const int saved_errno = errno;
        zlink_multipart_close (request_parts.data (), request_parts.size ());
        if (flags_ == ZLINK_DONTWAIT)
            return finish_dontwait_request_admission_failure (
              socket_handle_.socket, peer_rid_, user_context_, request_state,
              pending_token.identity, saved_errno, completion_id_out_, &request_wait);
        errno = saved_errno;
        return finish_request_submit_failure (
          request_state, pending_token.identity,
          zlink::submit_result_internal::from_errno (saved_errno));
    }
    if (completion_id_out_)
        *completion_id_out_ = reserved_completion_id;
    return ZLINK_SUBMIT_OK;
}
}

zlink_submit_result_t zlink_request_part (
  void *s_, const zlink_routing_id_t *target_router_rid_or_null_,
  zlink_msg_t *part_, zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_, uint32_t timeout_ms_, void *user_context_,
  zlink_completion_id_t *completion_id_out_)
{
    if (completion_id_out_)
        *completion_id_out_ = 0;

    if (!part_) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }
    if (zlink::part_helper_internal::validate_send_flags (flags_) != 0
        || zlink::part_helper_internal::validate_part_flag (part_flag_) != 0
        || (part_flag_ == ZLINK_PART_MORE
            && (timeout_ms_ != 0 || user_context_ != NULL))) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }

    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }

    const int type = socket_type (handle);
    if (type == ZLINK_CORE_SOCKET_DEALER) {
        if (target_router_rid_or_null_) {
            zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = EINVAL;
            return ZLINK_SUBMIT_INVALID_ARGUMENT;
        }
    } else if (type == ZLINK_CORE_SOCKET_ROUTER) {
        if (!zlink::valid_routing_id (target_router_rid_or_null_)) {
            zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
            zlink::part_helper_internal::consume_send_part (part_);
            errno = EINVAL;
            return ZLINK_SUBMIT_INVALID_ARGUMENT;
        }
    } else {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (s_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = ENOTSUP;
        return ZLINK_SUBMIT_NOT_SUPPORTED;
    }

    zlink_completion_id_t accepted_completion_id = 0;
    const zlink::part_helper_internal::send_family_t family =
      type == ZLINK_CORE_SOCKET_DEALER
        ? zlink::part_helper_internal::send_family_dealer_request
        : zlink::part_helper_internal::send_family_router_request;
    const zlink_submit_result_t result = request_part_common (
      handle, s_, target_router_rid_or_null_, part_, flags_, part_flag_,
      timeout_ms_, user_context_, family, &accepted_completion_id);
    if ((result == ZLINK_SUBMIT_OK
         || result == ZLINK_SUBMIT_BACKPRESSURED)
        && part_flag_ == ZLINK_PART_FINAL
        && completion_id_out_)
        *completion_id_out_ = accepted_completion_id;
    return result;
}

zlink_submit_result_t zlink_reply_part (
  void *router_, const zlink_routing_id_t *source_rid_,
  zlink_reply_token_t reply_token_, zlink_msg_t *part_,
  zlink_part_flag_t part_flag_)
{
    if (!part_) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (
          router_);
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }
    if (!zlink::valid_routing_id (source_rid_) || reply_token_ == 0
        || zlink::part_helper_internal::validate_part_flag (part_flag_) != 0) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (
          router_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = EINVAL;
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }

    socket_handle_t handle = as_socket_handle (router_);
    if (!handle.socket) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (
          router_);
        zlink::part_helper_internal::consume_send_part (part_);
        return zlink::submit_result_internal::from_errno (errno);
    }
    if (socket_type (handle) != ZLINK_CORE_SOCKET_ROUTER) {
        zlink::part_helper_internal::abort_current_non_publish_send_sequence (
          router_);
        zlink::part_helper_internal::consume_send_part (part_);
        errno = ENOTSUP;
        return ZLINK_SUBMIT_NOT_SUPPORTED;
    }

    return public_router_reply_submit (
      handle, router_, source_rid_, reply_token_, part_, part_flag_);
}
