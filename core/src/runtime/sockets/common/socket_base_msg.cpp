/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_base.hpp"
#include "sockets/common/socket_submit_retry_fault_injection.hpp"
#include "utils/err.hpp"
#include "utils/likely.hpp"

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
                                       pipe_t **pipe_out_)
{
    return send_direct_with_retry (NULL, msg_, flags_, send_scope, NULL, 0, false, pipe_out_);
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

int zlink::socket_base_t::send_routed_scoped (const zlink_routing_id_t *target_rid_,
                                              msg_t *msg_,
                                              int flags_,
                                              socket_public_send_scope_t &send_scope,
                                              uint64_t *connection_id_out_,
                                              uint64_t expected_connection_id_,
                                              zlink::pipe_t **pipe_out_)
{
    if (unlikely (!target_rid_)) {
        errno = EFAULT;
        return -1;
    }

    return send_direct_with_retry (
      target_rid_, msg_, flags_, send_scope, connection_id_out_,
      expected_connection_id_, false, pipe_out_);
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
                                                  pipe_t **pipe_out_)
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

    int rc = process_commands (0, true);
    if (unlikely (rc != 0)) {
        dispatch_runtime ().clear_send_recovery_pending ();
        return -1;
    }

    prepare_direct_send_message (msg_, flags_);

    _auto_hwm_send_attempts.fetch_add (1, std::memory_order_relaxed);
#ifdef ZLINK_BUILD_TESTS
    int injected_errno = 0;
    if (zlink::socket_submit_retry_fault::consume (&injected_errno)) {
        rc = -1;
        errno = injected_errno;
    } else
#endif
        rc = target_rid_
               ? xsend_routed (target_rid_, msg_, connection_id_out_,
                               expected_connection_id_, pipe_out_)
               : xsend_pipe (msg_, pipe_out_);
    if (rc == 0) {
        dispatch_runtime ().clear_send_recovery_pending ();
        return 0;
    }
    if (unlikely (rc == -2)) {
        if (report_multipart_abort_) {
            dispatch_runtime ().clear_send_recovery_pending ();
            errno = EAGAIN;
            return -1;
        }
        if (!((flags_ & ZLINK_DONTWAIT) || options.sndtimeo == 0)) {
            rc = msg_->close ();
            errno_assert (rc == 0);
            rc = msg_->init ();
            errno_assert (rc == 0);
            dispatch_runtime ().clear_send_recovery_pending ();
            return 0;
        }
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

            arm_send_ready_notification ();
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
            _auto_hwm_send_attempts.fetch_add (1, std::memory_order_relaxed);
#ifdef ZLINK_BUILD_TESTS
            if (zlink::socket_submit_retry_fault::consume (&injected_errno)) {
                rc = -1;
                errno = injected_errno;
            } else
#endif
                rc = target_rid_
                       ? xsend_routed (target_rid_, msg_, connection_id_out_,
                                       expected_connection_id_, pipe_out_)
                       : xsend_pipe (msg_, pipe_out_);
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
        dispatch_runtime ().clear_send_recovery_pending ();
        if ((flags_ & ZLINK_DONTWAIT) || options.sndtimeo == 0) {
            if (errno == ENOTCONN || errno == EHOSTUNREACH || errno == ETIMEDOUT) {
                arm_send_ready_notification ();
            }
        }
        return -1;
    }
    _auto_hwm_send_blocked_attempts.fetch_add (1, std::memory_order_relaxed);

    if ((flags_ & ZLINK_DONTWAIT) || options.sndtimeo == 0) {
        const bool was_pending = dispatch_runtime ().send_recovery_pending ();
        dispatch_runtime ().mark_send_recovery_pending ();
        if (!was_pending)
            static_cast<mailbox_t *> (_mailbox)->signal ();
        arm_send_ready_notification ();
        return -1;
    }

    int timeout = options.sndtimeo;
    const uint64_t end = timeout < 0 ? 0 : (_clock.now_ms () + timeout);
    const bool hold_sync_during_retry =
      send_scope.should_hold_sync_during_retry (send_ready_handler_active ());

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
        _auto_hwm_send_attempts.fetch_add (1, std::memory_order_relaxed);
        rc = target_rid_
               ? xsend_routed (target_rid_, msg_, connection_id_out_,
                               expected_connection_id_, pipe_out_)
               : xsend_pipe (msg_, pipe_out_);
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
        _auto_hwm_send_blocked_attempts.fetch_add (1, std::memory_order_relaxed);
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
    // Hot path: every public direct recv eventually reaches here. Keep the
    // no-message and success paths lean; recv-mode costs show up directly in
    // PAIR/DEALER small-message benchmarks.
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

    int rc = xrecv (msg_);
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

        rc = xrecv (msg_);
        if (rc < 0)
            return rc;
        extract_flags (msg_);
        return 0;
    }

    int timeout = options.rcvtimeo;
    const uint64_t end = timeout < 0 ? 0 : (_clock.now_ms () + timeout);

    bool block = command_runtime ().should_block_on_recv ();
    while (true) {
        if (unlikely (process_commands (block ? timeout : 0, false) != 0))
            return -1;
        rc = xrecv (msg_);
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

    if (command_runtime ().should_poll_commands_after_recv (inbound_poll_rate)) {
        if (unlikely (process_commands (0, false) != 0))
            return -1;
        command_runtime ().reset_recv_ticks ();
    }

    int rc = xrecv_pipe (msg_, pipe_out_);
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

        rc = xrecv_pipe (msg_, pipe_out_);
        if (rc < 0)
            return rc;
        extract_flags (msg_);
        return 0;
    }

    int timeout = options.rcvtimeo;
    const uint64_t end = timeout < 0 ? 0 : (_clock.now_ms () + timeout);

    bool block = command_runtime ().should_block_on_recv ();
    while (true) {
        if (unlikely (process_commands (block ? timeout : 0, false) != 0))
            return -1;
        rc = xrecv_pipe (msg_, pipe_out_);
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
        if (unlikely (process_commands (0, false) != 0)) {
            return -1;
        }
        command_runtime ().reset_recv_ticks ();
    }

    int rc =
      xrecv_routed (msg_, source_rid_out_, connection_id_out_, source_pipe_out_);
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

        rc =
          xrecv_routed (msg_, source_rid_out_, connection_id_out_, source_pipe_out_);
        if (rc < 0)
            return rc;
        extract_flags (msg_);
        return 0;
    }

    int timeout = options.rcvtimeo;
    const uint64_t end = timeout < 0 ? 0 : (_clock.now_ms () + timeout);

    bool block = command_runtime ().should_block_on_recv ();
    while (true) {
        if (unlikely (process_commands (block ? timeout : 0, false) != 0))
            return -1;
        rc =
          xrecv_routed (msg_, source_rid_out_, connection_id_out_, source_pipe_out_);
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
