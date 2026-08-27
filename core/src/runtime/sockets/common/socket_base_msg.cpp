/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_base.hpp"
#include "core/ctx.hpp"
#include "core/ctx_physical_queue_registry.hpp"
#include "sockets/common/socket_submit_retry_fault_injection.hpp"
#include "utils/err.hpp"
#include "utils/likely.hpp"

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
                          uint64_t *observed_epoch_out_)
{
    // The normal public-recv path does not share its socket with an async
    // mailbox owner.  The runtime owns the handoff when that changes.
    if (runtime_.try_acquire_public_receive_lease ()) {
        if (observed_epoch_out_)
            *observed_epoch_out_ = runtime_.progress_epoch;
        const int rc = receive_ ();
        runtime_.release_public_receive_lease ();
        return rc;
    }

    zlink::scoped_lock_t lock (runtime_.sync);
    if (observed_epoch_out_)
        *observed_epoch_out_ = runtime_.progress_epoch;
    return receive_ ();
}
}

int zlink::socket_base_t::send (msg_t *msg_, int flags_)
{
    // Hot path: every public single-part send pays this steady-state cost.
    // Keep lifecycle/backpressure logic correct, but avoid thickening the
    // success path with new work that is not contract-critical.
    socket_public_send_scope_t send_scope (lifecycle_coordinator (),
                                           direct_send_needs_public_api_sync ());
    if (!send_scope.acquired ())
        return -1;

    return send_scoped (msg_, flags_, send_scope);
}

int zlink::socket_base_t::send_scoped (msg_t *msg_,
                                       int flags_,
                                       socket_public_send_scope_t &send_scope,
                                       pipe_t **pipe_out_,
                                       bool report_multipart_abort_)
{
    return send_direct_with_retry (NULL, msg_, flags_, send_scope, NULL, 0,
                                   report_multipart_abort_, pipe_out_);
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

int zlink::socket_base_t::send_routed_transport_pair (
  const zlink_routing_id_t *target_rid_, uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_, msg_t *msg_, int flags_)
{
    socket_public_send_scope_t send_scope (lifecycle_coordinator (), true);
    if (!send_scope.acquired ())
        return -1;

    return send_routed_scoped (
      target_rid_, msg_, flags_, send_scope, NULL, 0, NULL,
      transport_pair_id_, transport_pair_generation_);
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

int zlink::socket_base_t::send_routed_scoped (const zlink_routing_id_t *target_rid_,
                                              msg_t *msg_,
                                              int flags_,
                                              socket_public_send_scope_t &send_scope,
                                              uint64_t *connection_id_out_,
                                              uint64_t expected_connection_id_,
                                              zlink::pipe_t **pipe_out_,
                                              uint64_t expected_transport_pair_id_,
                                              uint64_t expected_transport_pair_generation_,
                                              bool report_multipart_abort_)
{
    if (unlikely (!target_rid_)) {
        errno = EFAULT;
        return -1;
    }

    return send_direct_with_retry (
      target_rid_, msg_, flags_, send_scope, connection_id_out_,
      expected_connection_id_, report_multipart_abort_, pipe_out_, expected_transport_pair_id_,
      expected_transport_pair_generation_);
}

std::unique_ptr<zlink::socket_public_send_scope_t>
zlink::socket_base_t::begin_public_send_scope (bool force_sync_)
{
    const bool needs_sync = force_sync_ || direct_send_needs_public_api_sync ();
    std::unique_ptr<socket_public_send_scope_t> send_scope (
      new (std::nothrow) socket_public_send_scope_t (lifecycle_coordinator (), needs_sync));
    if (!send_scope) {
        errno = ENOMEM;
        return std::unique_ptr<socket_public_send_scope_t> ();
    }
    if (!send_scope->acquired ())
        return std::unique_ptr<socket_public_send_scope_t> ();
    return send_scope;
}

std::unique_ptr<zlink::socket_public_api_scope_t>
zlink::socket_base_t::begin_public_api_scope ()
{
    std::unique_ptr<socket_public_api_scope_t> scope (
      new (std::nothrow) socket_public_api_scope_t (lifecycle_coordinator ()));
    if (!scope) {
        errno = ENOMEM;
        return std::unique_ptr<socket_public_api_scope_t> ();
    }
    if (!scope->acquired ())
        return std::unique_ptr<socket_public_api_scope_t> ();
    return scope;
}

bool zlink::socket_base_t::direct_send_needs_public_api_sync () const
{
    return options.type != ZLINK_CORE_SOCKET_PAIR;
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
                                                  bool record_context_admission_)
{
    zlink_assert (send_scope.acquired ());
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (pipe_out_)
        *pipe_out_ = NULL;

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

    int rc = process_commands (0, true);
    if (unlikely (rc != 0)) {
        dispatch_runtime ().clear_send_recovery_pending ();
        return -1;
    }

    prepare_direct_send_message (msg_, flags_);

    _auto_hwm_send_attempts.fetch_add (1, std::memory_order_relaxed);
    pipe_message_admission_t first_admission =
      pipe_message_admission_invalid;
#ifdef ZLINK_BUILD_TESTS
    int injected_errno = 0;
    if (zlink::socket_submit_retry_fault::consume (&injected_errno)) {
        rc = -1;
        errno = injected_errno;
    } else
#endif
        rc = target_rid_
               ? xsend_routed (target_rid_, msg_, connection_id_out_,
                               expected_connection_id_, pipe_out_,
                               expected_transport_pair_id_,
                               expected_transport_pair_generation_,
                               &first_admission)
               : xsend_pipe (msg_, pipe_out_, &first_admission);
    if (record_context_admission_ && rc != 0
        && first_admission == pipe_message_admission_hwm_full)
        _auto_hwm_send_blocked_attempts.fetch_add (
          1, std::memory_order_relaxed);
    if (rc == 0) {
        dispatch_runtime ().clear_send_recovery_pending ();
        return 0;
    }
    if (unlikely (rc == -2))
        return finish_multipart_abort ();
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
    if (unlikely (errno != EAGAIN) && submit_retry_enabled (options, flags_)
        && is_submit_retry_errno (errno) && xsubmit_retry_allowed (target_rid_, errno)) {
        const uint64_t end = _clock.now_ms () + effective_submit_retry_timeout (options);
        int attempts_left = options.submit_retry_attempts;
        int last_errno = errno;
        dispatch_runtime ().mark_send_recovery_pending ();
        if (transport_has_out ())
            dispatch_runtime ().mark_send_recovery_ready ();
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
                                       expected_transport_pair_generation_, NULL)
                       : xsend_pipe (msg_, pipe_out_, NULL);
            if (unlikely (rc == -2))
                return finish_multipart_abort ();
            if (rc == 0) {
                dispatch_runtime ().clear_send_recovery_pending ();
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
            arm_send_recovery_after_backpressure ();
        } else {
            dispatch_runtime ().clear_send_recovery_pending ();
        }
        errno = failure_errno;
        return -1;
    }
    if ((flags_ & ZLINK_DONTWAIT) || options.sndtimeo == 0) {
        const bool was_pending = dispatch_runtime ().send_recovery_pending ();
        dispatch_runtime ().mark_send_recovery_pending ();
        if (!was_pending)
            static_cast<mailbox_t *> (_mailbox)->signal ();
        return -1;
    }

    int timeout = options.sndtimeo;
    const uint64_t end = timeout < 0 ? 0 : (_clock.now_ms () + timeout);
    // A mailbox-owned command path re-enters this socket to release byte
    // credit. It needs the same retry handoff as a send-ready callback;
    // otherwise a blocking public send can prevent the command that makes it
    // writable from running.
    const bool retry_progress_owner_active =
      send_complete_handler_active () || async_mailbox_owns_commands ();
    const bool hold_sync_during_retry =
      send_scope.should_hold_sync_during_retry (retry_progress_owner_active);

    if (!hold_sync_during_retry)
        send_scope.release_sync_for_retry ();

    while (true) {
        rc = process_commands (timeout, false);
        if (unlikely (rc != 0)) {
            dispatch_runtime ().clear_send_recovery_pending ();
            return -1;
        }
        if (!hold_sync_during_retry)
            send_scope.reacquire_sync_after_retry ();
        rc = target_rid_
               ? xsend_routed (target_rid_, msg_, connection_id_out_,
                               expected_connection_id_, pipe_out_,
                               expected_transport_pair_id_,
                               expected_transport_pair_generation_, NULL)
               : xsend_pipe (msg_, pipe_out_, NULL);
        if (unlikely (rc == -2))
            return finish_multipart_abort ();
        if (rc == 0) {
            dispatch_runtime ().clear_send_recovery_pending ();
            break;
        }
        if (!hold_sync_during_retry)
            send_scope.release_sync_for_retry ();
        if (unlikely (errno != EAGAIN)) {
            dispatch_runtime ().clear_send_recovery_pending ();
            return -1;
        }
        const bool was_pending = dispatch_runtime ().send_recovery_pending ();
        dispatch_runtime ().mark_send_recovery_pending ();
        if (!was_pending)
            static_cast<mailbox_t *> (_mailbox)->signal ();
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
    zlink_assert (scope_.acquired ());
    const int rc = xrollback ();
    return rc;
}

int zlink::socket_base_t::recv (msg_t *msg_, int flags_)
{
    // Plain public receive is the dominant data-plane role. Keep its state
    // machine explicit instead of routing every frame through the routed
    // receive policy branch in recv_common(). The guarded receive
    // operation still owns the public/async reader handoff.
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
  uint64_t *connection_id_out_)
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
        if (mode_ == receive_runtime_t::mode_pipe)
            return xrecv_pipe (msg_, pipe_out_);
        if (mode_ == receive_runtime_t::mode_routed)
            return xrecv_routed (
              msg_, source_rid_out_, connection_id_out_, pipe_out_);
        return xrecv (msg_);
    };

    uint64_t observed_epoch = 0;
    int rc = receive_once_guarded (receive_runtime (), recv_once,
                                   &observed_epoch);
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

int zlink::socket_base_t::recv_pipe (msg_t *msg_, pipe_t **pipe_out_, int flags_)
{
    return recv_common (msg_, flags_, receive_runtime_t::mode_pipe,
                        pipe_out_, NULL, NULL);
}

int zlink::socket_base_t::recv_routed (msg_t *msg_,
                                      zlink_routing_id_t *source_rid_out_,
                                      int flags_,
                                      uint64_t *connection_id_out_,
                                      pipe_t **source_pipe_out_)
{
    if (source_rid_out_)
        source_rid_out_->size = 0;
    if (connection_id_out_)
        *connection_id_out_ = 0;
    if (source_pipe_out_)
        *source_pipe_out_ = NULL;
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
        return xrecv_routed (msg_, source_rid_out_, connection_id_out_,
                             source_pipe_out_);
    };
    uint64_t observed_epoch = 0;
    int rc = receive_once_guarded (receive_runtime (), recv_once,
                                   &observed_epoch);
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

void zlink::socket_base_t::extract_flags (const msg_t *msg_)
{
    if (unlikely (msg_->flags () & msg_t::routing_id))
        zlink_assert (options.recv_routing_id);
}
