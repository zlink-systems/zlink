/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_base.hpp"
#include "core/ctx.hpp"
#include "core/ctx_physical_queue_registry.hpp"
#include "sockets/common/socket_submit_retry_fault_injection.hpp"
#include "utils/err.hpp"
#include "utils/likely.hpp"

#include <atomic>
#include <limits>

namespace
{
void prepare_direct_send_message (zlink::msg_t *msg_, int flags_)
{
    msg_->reset_flags (zlink::msg_t::more);
    if (flags_ & ZLINK_SNDMORE)
        msg_->set_flags (zlink::msg_t::more);
}

bool is_submit_retry_errno (int err_)
{
    return err_ == ENOTCONN || err_ == EHOSTUNREACH || err_ == ECONNREFUSED;
}

bool submit_retry_enabled (const zlink::options_t &options_, int flags_)
{
    return (flags_ & ZLINK_DONTWAIT) == 0
           && options_.submit_retry_mode == ZLINK_SUBMIT_RETRY_LOCAL_FAILURE
           && options_.submit_retry_timeout > 0 && options_.submit_retry_attempts > 0
           && options_.reconnect_ivl > 0;
}

int effective_submit_retry_timeout (const zlink::options_t &options_)
{
    if (options_.sndtimeo <= 0)
        return options_.submit_retry_timeout;
    return options_.submit_retry_timeout < options_.sndtimeo ? options_.submit_retry_timeout
                                                             : options_.sndtimeo;
}

int submit_retry_wait_ms (int remaining_ms_)
{
    if (remaining_ms_ <= 0)
        return 0;
    return remaining_ms_ < 20 ? remaining_ms_ : 20;
}

template <typename Receive>
int receive_once_guarded (zlink::socket_receive_runtime_t &runtime_,
                          const Receive &receive_,
                          uint64_t *observed_epoch_out_,
                          zlink::socket_receive_record_scope_t *record_scope_ = NULL,
                          bool defer_record_scope_ = false)
{
    // The normal public-recv path does not share its socket with an async
    // mailbox owner.  The runtime owns the handoff when that changes.
    if (runtime_.try_acquire_public_receive_lease ()) {
        // A whole-record receive also fences mailbox commands, whose
        // receive-side mutations already run under this sync. Ordinary
        // single-frame receive keeps the lock-free public fast path.
        if (record_scope_ && !defer_record_scope_)
            runtime_.sync.lock ();
        if (observed_epoch_out_)
            *observed_epoch_out_ = runtime_.progress_epoch;
        if (record_scope_ && !defer_record_scope_
            && record_scope_->prepare_receive_attempt () != 0) {
            runtime_.sync.unlock ();
            runtime_.release_public_receive_lease ();
            return -1;
        }
        bool sync_held = !defer_record_scope_ && record_scope_;
        if (record_scope_ && defer_record_scope_)
            record_scope_->begin_deferred_attempt (&runtime_, &sync_held);
        const int rc = receive_ ();
        if (record_scope_ && defer_record_scope_)
            record_scope_->end_deferred_attempt ();
        if (record_scope_ && defer_record_scope_ && record_scope_->owns (&runtime_)) {
            if (rc != 0) {
                record_scope_->rollback_receive_attempt ();
                record_scope_->release ();
            }
            return rc;
        }
        if (rc == 0 && record_scope_ && !defer_record_scope_)
            record_scope_->adopt_public_owner (&runtime_);
        else {
            if (record_scope_ && !defer_record_scope_)
                record_scope_->rollback_receive_attempt ();
            if (record_scope_ && sync_held)
                runtime_.sync.unlock ();
            runtime_.release_public_receive_lease ();
        }
        return rc;
    }

    runtime_.sync.lock ();
    if (observed_epoch_out_)
        *observed_epoch_out_ = runtime_.progress_epoch;
    if (record_scope_ && !defer_record_scope_
        && record_scope_->prepare_receive_attempt () != 0) {
        runtime_.sync.unlock ();
        return -1;
    }
    bool sync_held = true;
    if (record_scope_ && defer_record_scope_)
        record_scope_->begin_deferred_attempt (&runtime_, &sync_held);
    const int rc = receive_ ();
    if (record_scope_ && defer_record_scope_)
        record_scope_->end_deferred_attempt ();
    if (record_scope_ && defer_record_scope_ && record_scope_->owns (&runtime_)) {
        if (rc != 0) {
            record_scope_->rollback_receive_attempt ();
            record_scope_->release ();
        }
        return rc;
    }
    if (rc == 0 && record_scope_ && !defer_record_scope_)
        record_scope_->adopt_async_sync (&runtime_);
    else {
        if (record_scope_ && !defer_record_scope_)
            record_scope_->rollback_receive_attempt ();
        if (sync_held)
            runtime_.sync.unlock ();
    }
    return rc;
}

bool receive_multipart_abort_as_no_data (int rc_,
                                         bool *multipart_aborted_out_)
{
    if (rc_ == 0 || errno != ECONNABORTED)
        return false;

    // fq_t uses ECONNABORTED as an internal record-boundary marker when the
    // pipe that supplied an exposed multipart prefix disappears.  Return one
    // transient miss to the public caller without retrying into another
    // pipe's message.
    if (multipart_aborted_out_)
        *multipart_aborted_out_ = true;
    errno = EAGAIN;
    return true;
}
}

bool zlink::socket_base_t::retain_received_source_pipe_ref (pipe_t *pipe_) const
{
    if (!pipe_)
        return false;
    return pipe_->retain_lifetime_ref ();
}

int zlink::socket_base_t::send (msg_t *msg_, int flags_)
{
    // Hot path: every public single-part send pays this steady-state cost.
    // Keep lifecycle/backpressure logic correct, but avoid thickening the
    // success path with new work that is not contract-critical.
    socket_public_send_scope_t send_scope (lifecycle_coordinator (), true);
    if (!send_scope.acquired ())
        return -1;

    return send_scoped (msg_, flags_, send_scope);
}

int zlink::socket_base_t::send_complete_record (
  msg_t *msg_, int flags_, bool manage_public_send_recovery_)
{
    socket_public_send_scope_t send_scope (
      lifecycle_coordinator (), true,
      socket_send_admission_complete);
    if (!send_scope.acquired ())
        return -1;

    return send_scoped (msg_, flags_, send_scope, NULL, false, NULL, NULL,
                        manage_public_send_recovery_);
}

int zlink::socket_base_t::send_scoped (msg_t *msg_,
                                       int flags_,
                                       socket_public_send_scope_t &send_scope,
                                       pipe_t **pipe_out_,
                                       bool report_multipart_abort_,
                                       pipe_write_observer_fn observer_,
                                       void *observer_userdata_,
                                       bool manage_public_send_recovery_)
{
    return send_direct_with_retry (NULL, msg_, flags_, send_scope, NULL, 0,
                                   report_multipart_abort_, pipe_out_, 0, 0,
                                   true, false, observer_,
                                   observer_userdata_, NULL, 0,
                                   manage_public_send_recovery_);
}

int zlink::socket_base_t::send_routed (const zlink_routing_id_t *target_rid_,
                                       msg_t *msg_,
                                       int flags_)
{
    socket_public_send_scope_t send_scope (lifecycle_coordinator (), true);
    if (!send_scope.acquired ())
        return -1;

    return send_routed_scoped (target_rid_, msg_, flags_, send_scope);
}

int zlink::socket_base_t::send_routed_complete_record (
  const zlink_routing_id_t *target_rid_, msg_t *msg_, int flags_,
  bool manage_public_send_recovery_)
{
    socket_public_send_scope_t send_scope (
      lifecycle_coordinator (), true, socket_send_admission_complete);
    if (!send_scope.acquired ())
        return -1;

    return send_routed_scoped (target_rid_, msg_, flags_, send_scope, NULL, 0,
                               NULL, 0, 0, false, NULL, NULL, NULL, 0,
                               manage_public_send_recovery_);
}

int zlink::socket_base_t::select_routed_submit_target (
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_)
{
    if (!target_out_) {
        errno = EFAULT;
        return -1;
    }
    memset (target_out_, 0, sizeof (*target_out_));

    socket_public_send_scope_t send_scope (lifecycle_coordinator (), true);
    if (!send_scope.acquired ())
        return -1;
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }
    if (process_commands (0, true) != 0)
        return -1;

    return xselect_routed_submit_target (router_rid_or_null_, target_out_);
}

int zlink::socket_base_t::select_routed_submit_target_internal (
  const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_,
  uint64_t *transport_connection_id_out_,
  uint64_t *route_incarnation_id_out_)
{
    if (!target_out_ || !transport_connection_id_out_
        || !route_incarnation_id_out_) {
        errno = EFAULT;
        return -1;
    }
    memset (target_out_, 0, sizeof (*target_out_));
    *transport_connection_id_out_ = 0;
    *route_incarnation_id_out_ = 0;

    socket_public_send_scope_t send_scope (lifecycle_coordinator (), true);
    if (!send_scope.acquired ())
        return -1;
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }
    if (process_commands (0, true) != 0)
        return -1;

    return xselect_routed_submit_target_internal (
      router_rid_or_null_, target_out_, transport_connection_id_out_,
      route_incarnation_id_out_);
}

int zlink::socket_base_t::send_routed_scoped (const zlink_routing_id_t *target_rid_,
                                              msg_t *msg_,
                                              int flags_,
                                              socket_public_send_scope_t &send_scope,
                                              uint64_t *connection_id_out_,
                                              uint64_t expected_connection_id_,
                                              zlink::pipe_t **pipe_out_,
                                              uint64_t expected_transport_pair_id_,
                                              uint64_t expected_transport_pair_generation_,
                                              bool report_multipart_abort_,
                                              pipe_write_observer_fn observer_,
                                              void *observer_userdata_,
                                              routed_send_attempt_identity_t
                                                *attempt_identity_out_,
                                              uint64_t
                                                expected_route_incarnation_id_,
                                              bool manage_public_send_recovery_)
{
    if (unlikely (!target_rid_)) {
        errno = EFAULT;
        return -1;
    }

    return send_direct_with_retry (
      target_rid_, msg_, flags_, send_scope, connection_id_out_,
      expected_connection_id_, report_multipart_abort_, pipe_out_, expected_transport_pair_id_,
      expected_transport_pair_generation_, true, false, observer_,
      observer_userdata_, attempt_identity_out_,
      expected_route_incarnation_id_, manage_public_send_recovery_);
}

bool zlink::socket_base_t::begin_public_send_scope (
  std::optional<socket_public_send_scope_t> *scope_out_)
{
    if (!scope_out_) {
        errno = EFAULT;
        return false;
    }

    // Incremental multipart owns pipe-local staged state across public calls;
    // every socket send also fences socket-owned pipe selection/lifetime.
    scope_out_->emplace (lifecycle_coordinator (), true,
                         socket_send_admission_multipart);
    if (!(*scope_out_)->acquired ()) {
        scope_out_->reset ();
        return false;
    }
    return true;
}

bool zlink::socket_base_t::begin_complete_send_scope (
  std::optional<socket_public_send_scope_t> *scope_out_)
{
    if (!scope_out_) {
        errno = EFAULT;
        return false;
    }

    scope_out_->emplace (lifecycle_coordinator (), true,
                         socket_send_admission_complete);
    if (!(*scope_out_)->acquired ()) {
        scope_out_->reset ();
        return false;
    }
    return true;
}

std::unique_ptr<zlink::socket_public_send_scope_t>
zlink::socket_base_t::begin_complete_send_scope ()
{
    std::unique_ptr<socket_public_send_scope_t> send_scope (
      new (std::nothrow) socket_public_send_scope_t (
        lifecycle_coordinator (), true,
        socket_send_admission_complete));
    if (!send_scope) {
        errno = ENOMEM;
        return std::unique_ptr<socket_public_send_scope_t> ();
    }
    if (!send_scope->acquired ())
        return std::unique_ptr<socket_public_send_scope_t> ();
    return send_scope;
}

bool zlink::socket_base_t::xsubmit_retry_allowed (const zlink_routing_id_t *target_rid_,
                                                  int err_) const
{
    LIBZLINK_UNUSED (target_rid_);
    LIBZLINK_UNUSED (err_);
    return true;
}

int zlink::socket_base_t::send_direct_with_retry (const zlink_routing_id_t *target_rid_,
                                                  msg_t *msg_,
                                                  int flags_,
                                                  socket_public_send_scope_t &send_scope,
                                                  uint64_t *connection_id_out_,
                                                  uint64_t expected_connection_id_,
                                                  bool report_multipart_abort_,
                                                  pipe_t **pipe_out_,
                                                  uint64_t expected_transport_pair_id_,
                                                  uint64_t expected_transport_pair_generation_,
                                                  bool record_context_admission_,
                                                  bool commands_already_processed_,
                                                  pipe_write_observer_fn observer_,
                                                  void *observer_userdata_,
                                                  routed_send_attempt_identity_t
                                                    *attempt_identity_out_,
                                                  uint64_t
                                                    expected_route_incarnation_id_,
                                                  bool
                                                    manage_public_send_recovery_,
                                                  bool request_only_)
{
    zlink_assert (send_scope.acquired ());
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (pipe_out_)
        *pipe_out_ = NULL;
    if (attempt_identity_out_)
        attempt_identity_out_->reset ();

    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    if (unlikely (!msg_ || !msg_->check ())) {
        errno = EFAULT;
        return -1;
    }

    const auto finish_multipart_abort = [&] () -> int {
        // xsend has already rolled back the staged prefix. Never retry only
        // the failed continuation. Preserve its cause (for example EMSGSIZE,
        // EAGAIN or EHOSTUNREACH) for the public result mapper.
        if (manage_public_send_recovery_)
            dispatch_runtime ().clear_send_recovery_pending ();
        if (report_multipart_abort_ || (flags_ & ZLINK_DONTWAIT)
            || options.sndtimeo == 0)
            return -1;

        // Legacy internal callers that do not request an abort report keep
        // the historical blocking-drop behavior.
        int abort_rc = msg_->close ();
        errno_assert (abort_rc == 0);
        abort_rc = msg_->init ();
        errno_assert (abort_rc == 0);
        return 0;
    };
    if (unlikely (options.type != ZLINK_CORE_SOCKET_STREAM
                  && msg_->size ()
                       > static_cast<size_t> (
                         std::numeric_limits<uint32_t>::max ()))) {
        errno = EMSGSIZE;
        return -1;
    }

    int rc = 0;
    if (!commands_already_processed_) {
        rc = process_commands (0, true);
        if (unlikely (rc != 0)) {
            if (manage_public_send_recovery_)
                dispatch_runtime ().clear_send_recovery_pending ();
            return -1;
        }
    }

    prepare_direct_send_message (msg_, flags_);

    _auto_hwm_send_attempts.fetch_add (1, std::memory_order_relaxed);
    pipe_message_admission_t first_admission =
      pipe_message_admission_invalid;
#ifdef ZLINK_BUILD_TESTS
    int injected_errno = 0;
    bool submit_failure_was_injected = false;
    if (zlink::socket_submit_retry_fault::consume (&injected_errno)) {
        submit_failure_was_injected = true;
        rc = -1;
        errno = injected_errno;
    } else
#endif
        rc = target_rid_
               ? xsend_routed (target_rid_, msg_, connection_id_out_,
                               expected_connection_id_, pipe_out_,
                               expected_transport_pair_id_,
                               expected_transport_pair_generation_,
                               &first_admission, observer_, observer_userdata_,
                               attempt_identity_out_,
                               expected_route_incarnation_id_,
                               request_only_)
               : xsend_pipe (msg_, pipe_out_, &first_admission, observer_,
                             observer_userdata_);
    if (record_context_admission_ && rc != 0
        && first_admission == pipe_message_admission_hwm_full)
        _auto_hwm_send_blocked_attempts.fetch_add (
          1, std::memory_order_relaxed);
    if (rc == 0) {
        if (manage_public_send_recovery_)
            clear_public_send_recovery_state ();
        return 0;
    }
    if (unlikely (rc == -2))
        return finish_multipart_abort ();
    if (unlikely (first_admission == pipe_message_admission_request_full)) {
        // Request-correlation capacity is returned only when the application
        // drains a terminal reply or timeout completion. A blocking retry here
        // would keep that same application thread inside submit until SNDTIMEO
        // and prevent it from making the progress that releases the capacity.
        // Publish the normal recovery edge, but return backpressure immediately
        // regardless of the ordinary send flags.
        // Correlation capacity is independent of pipe writability.  Do not
        // reconcile this edge with transport_has_out(): a writable pipe can
        // still be request-full and would otherwise publish false POLLOUT.
        if (manage_public_send_recovery_) {
            const bool was_pending =
              dispatch_runtime ().send_recovery_pending ();
            dispatch_runtime ().mark_send_recovery_pending ();
            if (!was_pending)
                static_cast<mailbox_t *> (_mailbox)->signal ();
        }
        errno = EAGAIN;
        return -1;
    }
    if (
#ifdef ZLINK_BUILD_TESTS
        !submit_failure_was_injected
        &&
#endif
        errno != EAGAIN && is_submit_retry_errno (errno)
        && (options.type == ZLINK_CORE_SOCKET_DEALER
            || options.type == ZLINK_CORE_SOCKET_ROUTER)) {
        // Application-first connect-before-bind keeps its count-unknown pipe
        // out of FQ/LB until the binder resolves the peer type. Preserve the
        // established public admission result for that live, held intent:
        // DONTWAIT reports temporary backpressure, not a refused peer. A test
        // fault represents the already-classified public failure and must not
        // be reclassified by unrelated staged state. This is an error-only
        // scan, so it does not thicken the steady-state send path.
        bool staged_pair_intent = false;
        {
            scoped_lock_t lock (monitor_runtime ().sync);
            const size_t count = endpoint_runtime ().attached_pipe_count ();
            for (size_t i = 0; i != count; ++i) {
                pipe_t *const pipe = endpoint_runtime ().attached_pipe (i);
                if (pipe && pipe->get_transport_pair_id () != 0
                    && pipe->get_transport_lane ()
                         == transport_lane_application
                    && pipe->get_transport_lane_count () == 0u) {
                    staged_pair_intent = true;
                    break;
                }
            }
        }
        if (staged_pair_intent)
            errno = EAGAIN;
    }
    if (errno != EAGAIN && (flags_ & ZLINK_DONTWAIT) == 0
        && options.sndtimeo != 0 && !submit_retry_enabled (options, flags_)
        && is_submit_retry_errno (errno)
        && socket_has_manual_connect_endpoints ()
        && xsubmit_retry_allowed (target_rid_, errno)) {
        // Paired transports expose no application pipe until both lanes have
        // validated their handshake. A blocking send to a locally initiated
        // endpoint waits for that attach just as it waits for HWM credit.
        errno = EAGAIN;
    }
    if (manage_public_send_recovery_ && unlikely (errno != EAGAIN)
        && submit_retry_enabled (options, flags_)
        && is_submit_retry_errno (errno) && xsubmit_retry_allowed (target_rid_, errno)) {
        const uint64_t end = _clock.now_ms () + effective_submit_retry_timeout (options);
        int attempts_left = options.submit_retry_attempts;
        int last_errno = errno;
        if (manage_public_send_recovery_) {
            dispatch_runtime ().mark_send_recovery_pending ();
            if (transport_has_out ())
                dispatch_runtime ().mark_send_recovery_ready ();
        }
        while (attempts_left > 0) {
            const uint64_t now = _clock.now_ms ();
            if (now >= end)
                break;
            int remaining = static_cast<int> (end - now);
            if (remaining <= 0)
                break;

            const int wait_ms = submit_retry_wait_ms (remaining);
            const uint64_t attempt_at =
              now + static_cast<uint64_t> (wait_ms);
            do {
                const uint64_t before_wait = _clock.now_ms ();
                if (before_wait >= attempt_at)
                    break;
                rc = process_commands (
                  static_cast<int> (attempt_at - before_wait), false);
                if (unlikely (rc != 0)) {
                    if (manage_public_send_recovery_)
                        dispatch_runtime ().clear_send_recovery_pending ();
                    return -1;
                }
            } while (_clock.now_ms () < attempt_at);
            if (_clock.now_ms () >= end)
                break;
            if (!dispatch_runtime ().send_recovery_ready ())
                continue;
            dispatch_runtime ().clear_send_recovery_ready ();
            --attempts_left;

            prepare_direct_send_message (msg_, flags_);
#ifdef ZLINK_BUILD_TESTS
            if (zlink::socket_submit_retry_fault::consume (&injected_errno)) {
                rc = -1;
                errno = injected_errno;
            } else
#endif
                rc = target_rid_
                       ? xsend_routed (target_rid_, msg_, connection_id_out_,
                                       expected_connection_id_, pipe_out_,
                                       expected_transport_pair_id_,
                                       expected_transport_pair_generation_, NULL,
                                       observer_, observer_userdata_,
                                       attempt_identity_out_,
                                       expected_route_incarnation_id_,
                                       request_only_)
                       : xsend_pipe (msg_, pipe_out_, NULL, observer_,
                                     observer_userdata_);
            if (unlikely (rc == -2))
                return finish_multipart_abort ();
            if (rc == 0) {
                if (manage_public_send_recovery_)
                    clear_public_send_recovery_state ();
                return 0;
            }
            if (errno != EAGAIN)
                last_errno = errno;
            if (errno != EAGAIN && !is_submit_retry_errno (errno))
                break;
            if (!xsubmit_retry_allowed (target_rid_, errno))
                break;
        }
        errno = last_errno;
    }
    if (unlikely (errno != EAGAIN)) {
        const int failure_errno = errno;
        const bool async_retryable =
          ((flags_ & ZLINK_DONTWAIT) || options.sndtimeo == 0)
          && is_submit_retry_errno (failure_errno)
          && xsubmit_retry_allowed (target_rid_, failure_errno);
        if (async_retryable) {
            // A binding-owned async submit treats local connection admission
            // exactly like HWM recovery: retain the failed result for the
            // caller, but keep the recovery edge armed until an application
            // pipe attaches or becomes writable.  Clearing the pending bit
            // here loses the first-target wake after ECONNREFUSED.
            if (manage_public_send_recovery_)
                arm_send_recovery_after_backpressure ();
        } else {
            if (manage_public_send_recovery_)
                dispatch_runtime ().clear_send_recovery_pending ();
        }
        errno = failure_errno;
        return -1;
    }
    if ((flags_ & ZLINK_DONTWAIT) || options.sndtimeo == 0) {
        if (manage_public_send_recovery_)
            arm_send_recovery_after_backpressure ();
        return -1;
    }

    int timeout = options.sndtimeo;
    const uint64_t end = timeout < 0 ? 0 : (_clock.now_ms () + timeout);
    // A mailbox-owned command path re-enters this socket to release byte
    // credit, so a blocking public send must let that command make progress.
    // A paired connector starts its temporary mailbox owner only after HELLO
    // reveals the peer type. A first blocking send can reach this wait while
    // the Application pipe is still held, before that handoff is visible. If
    // it keeps the lifecycle sync here, the session I/O callback cannot acquire
    // the same sync to request the lane-count owner decision and the send waits
    // until SNDTIMEO. Manual DEALER/ROUTER connects are paired transports, so
    // release the sync for their pre-owner handshake window as well as for an
    // owner that is already active.
    const bool paired_connect_owner_may_start =
      (options.type == ZLINK_CORE_SOCKET_DEALER
       || options.type == ZLINK_CORE_SOCKET_ROUTER)
      && socket_has_manual_connect_endpoints ();
    const bool retry_progress_owner_active =
      async_mailbox_owns_commands () || paired_connect_owner_may_start;
    const bool hold_sync_during_retry =
      send_scope.should_hold_sync_during_retry (retry_progress_owner_active);

    if (!hold_sync_during_retry)
        send_scope.release_sync_for_retry ();

    while (true) {
        rc = process_commands (timeout, false);
        if (unlikely (rc != 0)) {
            if (manage_public_send_recovery_)
                dispatch_runtime ().clear_send_recovery_pending ();
            return -1;
        }
        if (!hold_sync_during_retry)
            send_scope.reacquire_sync_after_retry ();
        rc = target_rid_
               ? xsend_routed (target_rid_, msg_, connection_id_out_,
                               expected_connection_id_, pipe_out_,
                               expected_transport_pair_id_,
                               expected_transport_pair_generation_, NULL,
                               observer_, observer_userdata_,
                               attempt_identity_out_,
                               expected_route_incarnation_id_,
                               request_only_)
               : xsend_pipe (msg_, pipe_out_, NULL, observer_,
                             observer_userdata_);
        if (unlikely (rc == -2))
            return finish_multipart_abort ();
        if (rc == 0) {
            if (manage_public_send_recovery_)
                clear_public_send_recovery_state ();
            break;
        }
        if (errno != EAGAIN && is_submit_retry_errno (errno)
            && socket_has_manual_connect_endpoints ()
            && xsubmit_retry_allowed (target_rid_, errno)) {
            // Pair admission can advance one mailbox command at a time.  A
            // later retry may therefore observe a newly attached but not yet
            // admitted lane and must keep waiting just like the first try.
            errno = EAGAIN;
        }
        if (!hold_sync_during_retry)
            send_scope.release_sync_for_retry ();
        if (unlikely (errno != EAGAIN)) {
            if (manage_public_send_recovery_)
                dispatch_runtime ().clear_send_recovery_pending ();
            return -1;
        }
        if (manage_public_send_recovery_)
            arm_send_recovery_after_backpressure ();
        if (timeout > 0) {
            timeout = static_cast<int> (end - _clock.now_ms ());
            if (timeout <= 0) {
                errno = EAGAIN;
                return -1;
            }
        }
    }

    return 0;
}

int zlink::socket_base_t::rollback ()
{
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    socket_public_api_scope_t admission (lifecycle);
    if (!admission.acquired ())
        return -1;

    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    socket_public_api_lock_scope_t guard (lifecycle);
    const int rc = xrollback ();
    return rc;
}

int zlink::socket_base_t::rollback_scoped (socket_public_send_scope_t &scope_)
{
    zlink_assert (
      scope_.acquired ()
      || (lifecycle_coordinator ().public_close_requested ()
          && scope_.close_cleanup_ready ()));
    // A blocking send releases the lifecycle sync while its async command
    // owner makes progress. process_commands() can then fail or time out
    // before the retry path reacquires it. Rollback is socket-state mutation,
    // so centralize the hand-back here rather than relying on every caller.
    scope_.relock_sync ();
    const int rc = xrollback ();
    return rc;
}

int zlink::socket_base_t::recv (msg_t *msg_, int flags_,
                                bool *multipart_aborted_out_)
{
    // Plain public receive is the dominant data-plane role. Keep its state
    // machine explicit instead of routing every frame through the routed
    // receive policy branch in recv_common(). The guarded receive
    // operation still owns the public/async reader handoff.
    if (multipart_aborted_out_)
        *multipart_aborted_out_ = false;
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }
    if (unlikely (!msg_ || !msg_->check ())) {
        errno = EFAULT;
        return -1;
    }

    if (command_runtime ().should_poll_commands_after_recv (inbound_poll_rate)) {
        if (unlikely (process_commands (0, false) != 0))
            return -1;
        command_runtime ().reset_recv_ticks ();
    }

    const auto recv_once = [&] () -> int { return xrecv (msg_); };
    uint64_t observed_epoch = 0;
    int rc = receive_once_guarded (receive_runtime (), recv_once,
                                   &observed_epoch);
    if (unlikely (receive_multipart_abort_as_no_data (
          rc, multipart_aborted_out_)))
        return -1;
    if (unlikely (rc != 0 && errno != EAGAIN))
        return -1;
    if (rc == 0) {
        extract_flags (msg_);
        return 0;
    }

    if ((flags_ & ZLINK_DONTWAIT) || options.rcvtimeo == 0) {
        if (unlikely (process_commands (0, false) != 0))
            return -1;
        command_runtime ().reset_recv_ticks ();
        rc = receive_once_guarded (receive_runtime (), recv_once,
                                   &observed_epoch);
        if (unlikely (receive_multipart_abort_as_no_data (
              rc, multipart_aborted_out_)))
            return -1;
        if (rc < 0)
            return rc;
        extract_flags (msg_);
        return 0;
    }

    int timeout = options.rcvtimeo;
    const uint64_t end = timeout < 0 ? 0 : (_clock.now_ms () + timeout);
    bool block = command_runtime ().should_block_on_recv ();
    while (true) {
        const int progress_rc = async_mailbox_owns_commands ()
                                  ? wait_receive_progress (
                                      observed_epoch, block ? timeout : 0)
                                  : process_commands (block ? timeout : 0,
                                                      false);
        if (unlikely (progress_rc != 0))
            return -1;
        rc = receive_once_guarded (receive_runtime (), recv_once,
                                   &observed_epoch);
        if (unlikely (receive_multipart_abort_as_no_data (
              rc, multipart_aborted_out_)))
            return -1;
        if (rc == 0) {
            command_runtime ().reset_recv_ticks ();
            break;
        }
        if (unlikely (errno != EAGAIN))
            return -1;
        block = true;
        if (timeout > 0) {
            timeout = static_cast<int> (end - _clock.now_ms ());
            if (timeout <= 0) {
                errno = EAGAIN;
                return -1;
            }
        }
    }

    extract_flags (msg_);
    return 0;
}

int zlink::socket_base_t::recv_common (
  msg_t *msg_, int flags_,
  receive_runtime_t::mode_t mode_, pipe_t **pipe_out_,
  zlink_routing_id_t *source_rid_out_,
  uint64_t *connection_id_out_, bool pin_pipe_out_,
  socket_receive_record_scope_t *record_scope_)
{
    if (pipe_out_)
        *pipe_out_ = NULL;
    if (source_rid_out_)
        source_rid_out_->size = 0;
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }
    if (unlikely (!msg_ || !msg_->check ())) {
        errno = EFAULT;
        return -1;
    }

    if (command_runtime ().should_poll_commands_after_recv (inbound_poll_rate)) {
        if (unlikely (process_commands (0, false) != 0))
            return -1;
        command_runtime ().reset_recv_ticks ();
    }

    const auto recv_once = [&] () -> int {
        int rc = 0;
        if (mode_ == receive_runtime_t::mode_pipe)
            rc = xrecv_pipe (msg_, pipe_out_);
        else if (mode_ == receive_runtime_t::mode_routed)
            rc = xrecv_routed (
              msg_, source_rid_out_, connection_id_out_, pipe_out_, NULL,
              NULL, NULL);
        else
            rc = xrecv (msg_);
        // Socket-specific readers may expose the pipe they attempted before
        // discovering that no frame was available. The public helper only
        // transfers a lifetime pin with a consumed frame, so never let a
        // failure path look pinned to its caller.
        if (rc != 0 && pipe_out_)
            *pipe_out_ = NULL;
        if (rc == 0 && pin_pipe_out_ && pipe_out_ && *pipe_out_
            && !(*pipe_out_)->retain_lifetime_ref ()) {
            *pipe_out_ = NULL;
            // The frame was already consumed. Preserve successful receive
            // ownership and let the request/reply reader reject a request
            // without a live source pipe while draining the record tail.
            return 0;
        }
        return rc;
    };

    uint64_t observed_epoch = 0;
    int rc = receive_once_guarded (receive_runtime (), recv_once,
                                   &observed_epoch, record_scope_);
    if (record_scope_ && record_scope_->admission_failed ())
        return -1;
    if (unlikely (receive_multipart_abort_as_no_data (rc, NULL)))
        return -1;
    if (unlikely (rc != 0 && errno != EAGAIN))
        return -1;
    if (rc == 0) {
        extract_flags (msg_);
        return 0;
    }

    if ((flags_ & ZLINK_DONTWAIT) || options.rcvtimeo == 0) {
        if (unlikely (process_commands (0, false) != 0))
            return -1;
        command_runtime ().reset_recv_ticks ();
        rc = receive_once_guarded (receive_runtime (), recv_once,
                                   &observed_epoch, record_scope_);
        if (record_scope_ && record_scope_->admission_failed ())
            return -1;
        if (unlikely (receive_multipart_abort_as_no_data (rc, NULL)))
            return -1;
        if (rc < 0)
            return rc;
        extract_flags (msg_);
        return 0;
    }

    int timeout = options.rcvtimeo;
    const uint64_t end = timeout < 0 ? 0 : (_clock.now_ms () + timeout);
    bool block = command_runtime ().should_block_on_recv ();
    while (true) {
        const int progress_rc = async_mailbox_owns_commands ()
                                  ? wait_receive_progress (
                                      observed_epoch, block ? timeout : 0)
                                  : process_commands (block ? timeout : 0,
                                                      false);
        if (unlikely (progress_rc != 0))
            return -1;
        rc = receive_once_guarded (receive_runtime (), recv_once,
                                   &observed_epoch, record_scope_);
        if (record_scope_ && record_scope_->admission_failed ())
            return -1;
        if (unlikely (receive_multipart_abort_as_no_data (rc, NULL)))
            return -1;
        if (rc == 0) {
            command_runtime ().reset_recv_ticks ();
            break;
        }
        if (unlikely (errno != EAGAIN))
            return -1;
        block = true;
        if (timeout > 0) {
            timeout = static_cast<int> (end - _clock.now_ms ());
            if (timeout <= 0) {
                errno = EAGAIN;
                return -1;
            }
        }
    }

    extract_flags (msg_);
    return 0;
}

int zlink::socket_base_t::recv_pipe (msg_t *msg_, pipe_t **pipe_out_, int flags_,
                                     bool pin_pipe_out_,
                                     socket_receive_record_scope_t *record_scope_)
{
    return recv_common (msg_, flags_, receive_runtime_t::mode_pipe,
                        pipe_out_, NULL, NULL, pin_pipe_out_, record_scope_);
}

int zlink::socket_base_t::recv_routed (msg_t *msg_,
                                      zlink_routing_id_t *source_rid_out_,
                                      int flags_,
                                      uint64_t *connection_id_out_,
                                      pipe_t **source_pipe_out_,
                                      bool pin_source_pipe_out_,
                                      uint64_t *transport_pair_id_out_,
                                      uint64_t *transport_pair_generation_out_,
                                      uint64_t *route_binding_token_out_,
                                      socket_receive_record_scope_t *record_scope_,
                                      pipe_t::read_admission_fn *admission_,
                                      void *admission_userdata_)
{
    if (source_rid_out_)
        source_rid_out_->size = 0;
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (source_pipe_out_)
        *source_pipe_out_ = NULL;
    if (transport_pair_id_out_)
        *transport_pair_id_out_ = 0;
    if (transport_pair_generation_out_)
        *transport_pair_generation_out_ = 0;
    if (route_binding_token_out_)
        *route_binding_token_out_ = 0;
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }
    if (unlikely (!msg_ || !msg_->check ())) {
        errno = EFAULT;
        return -1;
    }

    if (command_runtime ().should_poll_commands_after_recv (inbound_poll_rate)) {
        if (unlikely (process_commands (0, false) != 0))
            return -1;
        command_runtime ().reset_recv_ticks ();
    }

    const auto recv_once = [&] () -> int {
        const int rc = xrecv_routed (msg_, source_rid_out_, connection_id_out_,
                                     source_pipe_out_, admission_,
                                     admission_userdata_,
                                     route_binding_token_out_);
        if (rc != 0 && source_pipe_out_)
            *source_pipe_out_ = NULL;
        if (rc == 0 && source_pipe_out_ && *source_pipe_out_) {
            if (transport_pair_id_out_)
                *transport_pair_id_out_ =
                  (*source_pipe_out_)->get_transport_pair_id ();
            if (transport_pair_generation_out_)
                *transport_pair_generation_out_ =
                  (*source_pipe_out_)->get_transport_pair_generation ();
        }
        if (rc == 0 && pin_source_pipe_out_ && source_pipe_out_
            && *source_pipe_out_
            && !retain_received_source_pipe_ref (*source_pipe_out_)) {
            *source_pipe_out_ = NULL;
            // Do not turn a consumed frame into an unowned failure result.
            // The request/reply reader handles a null live-source reference;
            // transport-pair identity was copied above while receive ownership
            // still prevented pipe deallocation.
            return 0;
        }
        return rc;
    };
    uint64_t observed_epoch = 0;
    int rc = receive_once_guarded (receive_runtime (), recv_once,
                                   &observed_epoch, record_scope_, admission_ != NULL);
    if (record_scope_ && record_scope_->admission_failed ())
        return -1;
    if (unlikely (receive_multipart_abort_as_no_data (rc, NULL)))
        return -1;
    if (unlikely (rc != 0 && errno != EAGAIN))
        return -1;
    if (rc == 0) {
        extract_flags (msg_);
        return 0;
    }

    if ((flags_ & ZLINK_DONTWAIT) || options.rcvtimeo == 0) {
        if (unlikely (process_commands (0, false) != 0))
            return -1;
        command_runtime ().reset_recv_ticks ();
        rc = receive_once_guarded (receive_runtime (), recv_once,
                                   &observed_epoch, record_scope_, admission_ != NULL);
        if (record_scope_ && record_scope_->admission_failed ())
            return -1;
        if (unlikely (receive_multipart_abort_as_no_data (rc, NULL)))
            return -1;
        if (rc < 0)
            return rc;
        extract_flags (msg_);
        return 0;
    }

    int timeout = options.rcvtimeo;
    const uint64_t end = timeout < 0 ? 0 : (_clock.now_ms () + timeout);
    bool block = command_runtime ().should_block_on_recv ();
    while (true) {
        const int progress_rc = async_mailbox_owns_commands ()
                                  ? wait_receive_progress (
                                      observed_epoch, block ? timeout : 0)
                                  : process_commands (block ? timeout : 0,
                                                      false);
        if (unlikely (progress_rc != 0))
            return -1;
        rc = receive_once_guarded (receive_runtime (), recv_once,
                                   &observed_epoch, record_scope_, admission_ != NULL);
        if (record_scope_ && record_scope_->admission_failed ())
            return -1;
        if (unlikely (receive_multipart_abort_as_no_data (rc, NULL)))
            return -1;
        if (rc == 0) {
            command_runtime ().reset_recv_ticks ();
            break;
        }
        if (unlikely (errno != EAGAIN))
            return -1;
        block = true;
        if (timeout > 0) {
            timeout = static_cast<int> (end - _clock.now_ms ());
            if (timeout <= 0) {
                errno = EAGAIN;
                return -1;
            }
        }
    }

    extract_flags (msg_);
    return 0;
}

int zlink::socket_base_t::recv_record_continuation (
  msg_t *msg_, socket_receive_record_scope_t &record_scope_)
{
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }
    if (unlikely (!msg_ || !msg_->check ())
        || unlikely (!record_scope_.owns (&receive_runtime ()))) {
        errno = EFAULT;
        return -1;
    }

    const int rc = xrecv (msg_);
    if (unlikely (receive_multipart_abort_as_no_data (rc, NULL)))
        return -1;
    if (rc != 0)
        return -1;
    extract_flags (msg_);
    return 0;
}

void zlink::socket_base_t::extract_flags (const msg_t *msg_)
{
    if (unlikely (msg_->flags () & msg_t::routing_id))
        zlink_assert (options.recv_routing_id);
}
