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
  zlink::socket_completion::request_writable_wait_t *request_wait_,
  zlink::socket_public_send_scope_t *send_scope_ = NULL)
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

    const int rc = send_scope_
      ? socket_->request_admission_submit_scoped (
          parts_, part_count_, peer_rid_, &publish_pending_pair_before_flush,
          &observer, *send_scope_)
      : socket_->request_admission_submit (
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

// Bundles the fields that finish_dontwait_request_admission_failure needs to
// both resolve the failed pending entry and, on EAGAIN, register a
// writable-wait continuation. Grouping them keeps the call sites in
// request_part_common from having to thread each field through separately.
struct request_admission_failure_ctx_t
{
    zlink::socket_base_t *socket;
    const zlink_routing_id_t *peer_rid;
    void *user_context;
    std::shared_ptr<reqrep::socket_request_reply_state_t> state;
    reqrep::pending_request_identity_t identity;
    zlink_completion_id_t *completion_id_out;
    zlink::socket_completion::request_writable_wait_t *request_wait;
};

zlink_submit_result_t finish_dontwait_request_admission_failure (
  const request_admission_failure_ctx_t &ctx_, int failure_errno_)
{
    // A mandatory ROUTER route miss is connectivity, not lookup ownership.
    const int normalized_errno =
      ctx_.peer_rid && failure_errno_ == ENOENT ? EHOSTUNREACH : failure_errno_;
    errno = normalized_errno;
    const zlink_submit_result_t failure = finish_request_submit_failure (
      ctx_.state, ctx_.identity,
      zlink::submit_result_internal::from_errno (normalized_errno));
    if (failure == ZLINK_SUBMIT_OK || normalized_errno != EAGAIN)
        return failure;

    if (ctx_.socket->register_send_writable_wait_after_failure (
          normalized_errno, ctx_.peer_rid, ctx_.user_context,
          ctx_.completion_id_out, ctx_.request_wait)
        != 0)
        return zlink::submit_result_internal::from_errno (errno);
    return ZLINK_SUBMIT_BACKPRESSURED;
}

bool message_has_group (const zlink_msg_t *part_);
int attach_request_reply_metadata (zlink_msg_t *part_,
                                   uint8_t message_type_,
                                   uint64_t request_seq_);

int checkout_router_reply_target (
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  const zlink_routing_id_t *peer_rid_, zlink_reply_token_t token_,
  reqrep::router_reply_target_t *target_out_)
{
    if (!state_ || !target_out_ || !zlink::valid_routing_id (peer_rid_)
        || token_ == 0) {
        errno = EFAULT;
        return -1;
    }

    std::unique_lock<std::mutex> lock (state_->mutex);

    reqrep::router_reply_target_t target;
    const reqrep::router_reply_target_take_result_t take_result =
      reqrep::take_router_reply_target_locked (
        state_.get (), token_, peer_rid_, &target);
    if (take_result != reqrep::router_reply_target_take_ok) {
        errno = take_result == reqrep::router_reply_target_take_busy ? EBUSY
                                                                     : ENOENT;
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

    *target_out_ = target;
    return 0;
}

int validate_router_reply_checkout (
  const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
  zlink_reply_token_t token_)
{
    if (!state_) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (state_->mutex);
    if (state_->closing) {
        errno = ESHUTDOWN;
        return -1;
    }
    reqrep::reply_target_store_t<reqrep::router_reply_target_t>::iterator it =
      state_->router_reply_targets.find (token_);
    if (it == state_->router_reply_targets.end () || !it->second.checked_out
        || it->second.revoked) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

class router_reply_checkout_t
{
  public:
    router_reply_checkout_t (
      const std::shared_ptr<reqrep::socket_request_reply_state_t> &state_,
      uint64_t token_, const reqrep::router_reply_target_t &target_) :
        _state (state_), _token (token_), _target (target_), _active (true)
    {
    }

    ~router_reply_checkout_t () { abandon (); }

    void commit ()
    {
        if (!_active)
            return;
        reqrep::commit_router_reply_target (_state, _token);
        if (_target.pipe)
            _target.pipe->release_lifetime_ref ();
        _active = false;
    }

    void abandon ()
    {
        if (!_active)
            return;
        reqrep::restore_router_reply_target (_state, _token);
        if (_target.pipe)
            _target.pipe->release_lifetime_ref ();
        _active = false;
    }

  private:
    std::shared_ptr<reqrep::socket_request_reply_state_t> _state;
    uint64_t _token;
    reqrep::router_reply_target_t _target;
    bool _active;
};

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
        if (validate_router_reply_checkout (request_state_, reply_token_)
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
  const socket_handle_t &handle_,
  const zlink_routing_id_t *peer_rid_, zlink_reply_token_t reply_token_,
  zlink_msg_t *parts_, size_t part_count_)
{
    const int reply_timeout_ms = handle_.socket->send_timeout_ms ();
    const std::chrono::steady_clock::time_point reply_started_at =
      std::chrono::steady_clock::now ();
    std::shared_ptr<reqrep::socket_request_reply_state_t> request_state =
      reqrep::find_request_reply_state (handle_);
    if (!request_state) {
        errno = ENOENT;
        return ZLINK_SUBMIT_NOT_FOUND;
    }
    reqrep::router_reply_target_t target;
    if (checkout_router_reply_target (
          request_state, peer_rid_, reply_token_, &target) != 0)
        return zlink::submit_result_internal::from_errno (errno);
    router_reply_checkout_t reply_context (
      request_state, reply_token_, target);
    std::optional<zlink::socket_public_send_scope_t> complete_scope;
    if (!handle_.socket->begin_complete_send_scope (&complete_scope))
        return zlink::submit_result_internal::from_errno (errno);
    if (message_has_group (&parts_[0])) {
        errno = EINVAL;
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }
    if (attach_request_reply_metadata (
          &parts_[0], zlink::request_reply::reply_type,
          target.wire_request_seq) != 0)
        return zlink::submit_result_internal::from_errno (errno);
    if (send_public_router_reply_with_wait (
          handle_.socket, *complete_scope, request_state, target,
          peer_rid_, reply_token_, parts_, part_count_ - 1,
          &parts_[part_count_ - 1], reply_timeout_ms, reply_started_at) != 0)
        return zlink::submit_result_internal::from_errno (errno);
    complete_scope.reset ();
    reply_context.commit ();
    errno = 0;
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

// Correlation publishes before the record flush so an eager reply cannot
// overtake the pending request's selected transport pair.
zlink_submit_result_t submit_request_record (
  const socket_handle_t &handle_, const zlink_routing_id_t *peer_rid_,
  zlink_msg_t *parts_, size_t part_count_, zlink_send_flags_t flags_,
  uint32_t timeout_ms_, void *user_context_,
  zlink_completion_id_t *completion_id_out_)
{
    if (message_has_group (&parts_[0])) {
        errno = EINVAL;
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }
    std::shared_ptr<reqrep::socket_request_reply_state_t> request_state;
    reqrep::pending_request_token_t pending_token;
    zlink_completion_id_t reserved_completion_id = 0;
    uint64_t request_seq = 0;
    if (reqrep::ensure_socket_pull_pending_request (
          handle_, timeout_ms_, peer_rid_, user_context_, &request_seq,
          &request_state, &pending_token, &reserved_completion_id) != 0)
        return zlink::submit_result_internal::from_errno (errno);
    if (attach_request_reply_metadata (
          &parts_[0], zlink::request_reply::request_type, request_seq) != 0)
        return finish_request_submit_failure (
          request_state, pending_token.identity,
          zlink::submit_result_internal::from_errno (errno));

    zlink::socket_completion::request_writable_wait_t request_wait;
    const int submit_rc = flags_ == ZLINK_SEND_FLAGS_DONTWAIT
      ? submit_pull_dontwait_request (
          handle_.socket, peer_rid_, parts_, part_count_, request_state,
          pending_token, &request_wait)
      : submit_pull_blocking_request (
          handle_.socket, peer_rid_, parts_, part_count_, request_state,
          pending_token);
    if (submit_rc != 0) {
        const int saved_errno = errno;
        if (flags_ == ZLINK_SEND_FLAGS_DONTWAIT) {
            request_admission_failure_ctx_t failure_ctx;
            failure_ctx.socket = handle_.socket;
            failure_ctx.peer_rid = peer_rid_;
            failure_ctx.user_context = user_context_;
            failure_ctx.state = request_state;
            failure_ctx.identity = pending_token.identity;
            failure_ctx.completion_id_out = completion_id_out_;
            failure_ctx.request_wait = &request_wait;
            return finish_dontwait_request_admission_failure (
              failure_ctx, saved_errno);
        }
        return finish_request_submit_failure (
          request_state, pending_token.identity,
          zlink::submit_result_internal::from_errno (saved_errno));
    }
    if (completion_id_out_)
        *completion_id_out_ = reserved_completion_id;
    errno = 0;
    return ZLINK_SUBMIT_OK;
}
}

zlink_submit_result_t zlink_request (
  void *s_, const zlink_routing_id_t *target_router_rid_or_null_,
  zlink_msg_t *parts_, size_t part_count_, zlink_send_flags_t flags_,
  uint32_t timeout_ms_, void *user_context_,
  zlink_completion_id_t *completion_id_out_)
{
    if (completion_id_out_)
        *completion_id_out_ = 0;
    socket_handle_t handle = as_socket_handle (s_);
    const int argument_errno =
      handle.socket && socket_type (handle) == ZLINK_CORE_SOCKET_ROUTER
          && !target_router_rid_or_null_ ? EFAULT : 0;
    return zlink::part_helper_internal::submit_whole_record (
      handle.socket, parts_, part_count_, argument_errno, [&] {
          const int type = socket_type (handle);
          if (zlink::part_helper_internal::validate_send_flags (flags_) != 0
              || (type == ZLINK_CORE_SOCKET_DEALER && target_router_rid_or_null_)
              || (type == ZLINK_CORE_SOCKET_ROUTER
                  && !zlink::valid_routing_id (target_router_rid_or_null_))) {
              errno = EINVAL;
              return ZLINK_SUBMIT_INVALID_ARGUMENT;
          }
          if (type != ZLINK_CORE_SOCKET_DEALER && type != ZLINK_CORE_SOCKET_ROUTER) {
              errno = ENOTSUP;
              return ZLINK_SUBMIT_NOT_SUPPORTED;
          }
          return submit_request_record (
            handle, target_router_rid_or_null_, parts_, part_count_, flags_,
            timeout_ms_, user_context_, completion_id_out_);
      });
}

zlink_submit_result_t zlink_reply (
  void *router_, const zlink_routing_id_t *source_rid_,
  zlink_reply_token_t reply_token_, zlink_msg_t *parts_, size_t part_count_)
{
    socket_handle_t handle = as_socket_handle (router_);
    return zlink::part_helper_internal::submit_whole_record (
      handle.socket, parts_, part_count_, !source_rid_ ? EFAULT : 0, [&] {
          if (!zlink::valid_routing_id (source_rid_) || reply_token_ == 0) {
              errno = EINVAL;
              return ZLINK_SUBMIT_INVALID_ARGUMENT;
          }
          if (socket_type (handle) != ZLINK_CORE_SOCKET_ROUTER) {
              errno = ENOTSUP;
              return ZLINK_SUBMIT_NOT_SUPPORTED;
          }
          return public_router_reply_submit (
            handle, source_rid_, reply_token_, parts_, part_count_);
      });
}
