/* SPDX-License-Identifier: MPL-2.0 */

// Shared physical admission and payload-free WRITABLE wait helpers.

#include "utils/precompiled.hpp"

#include "core/c_api_copy_internal.hpp"
#include "core/mailbox.hpp"
#include "sockets/common/socket_base.hpp"
#include "sockets/common/socket_submit_retry_fault_injection.hpp"
#include "utils/err.hpp"
#include "utils/likely.hpp"
#include "utils/macros.hpp"
#include "utils/routing_id.hpp"

#include <atomic>

namespace
{
void fail_blocking_send_wait_state (
  zlink::blocking_send_wait_state_t *state_, int terminal_errno_)
{
    zlink_assert (state_);
    ++state_->epoch;
    if (state_->epoch == 0)
        ++state_->epoch;
    state_->terminal_errno = terminal_errno_ != 0 ? terminal_errno_ : EIO;
}

void close_send_part_array (zlink_msg_t *parts_, size_t part_count_)
{
    const int saved_errno = errno;
    for (size_t i = 0; i != part_count_; ++i) {
        zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (&parts_[i]);
        if (msg->check ()) {
            const int rc = msg->close ();
            errno_assert (rc == 0);
        }
    }
    errno = saved_errno;
}

int copy_send_part_array (zlink_msg_t *parts_,
                          size_t part_count_,
                          zlink_msg_t *copies_)
{
    size_t initialized = 0;
    for (; initialized != part_count_; ++initialized) {
        zlink::msg_t *copy =
          reinterpret_cast<zlink::msg_t *> (&copies_[initialized]);
        if (copy->init () != 0)
            break;
    }
    if (initialized != part_count_) {
        const int saved_errno = errno;
        close_send_part_array (copies_, initialized);
        errno = saved_errno;
        return -1;
    }

    for (size_t i = 0; i != part_count_; ++i) {
        zlink::msg_t *copy = reinterpret_cast<zlink::msg_t *> (&copies_[i]);
        zlink::msg_t *source = reinterpret_cast<zlink::msg_t *> (&parts_[i]);
        if (copy->copy (*source) != 0) {
            const int saved_errno = errno;
            close_send_part_array (copies_, part_count_);
            errno = saved_errno;
            return -1;
        }
    }
    return 0;
}

}

bool zlink::socket_type_supports_completion_pull (int type_)
{
    return type_ == ZLINK_CORE_SOCKET_PAIR || type_ == ZLINK_CORE_SOCKET_DEALER
           || type_ == ZLINK_CORE_SOCKET_ROUTER
           || type_ == ZLINK_CORE_SOCKET_STREAM;
}

bool zlink::socket_base_t::has_send_writable_wait () const
{
    return socket_completion::has_writable_wait (
      const_cast<socket_completion::queue_state_t *> (&completion_runtime ()));
}

bool zlink::socket_base_t::xsend_writable_target_ready (
  const zlink_routing_id_t *target_rid_or_null_)
{
    if (target_rid_or_null_)
        return false;
    return xhas_out ();
}

bool zlink::socket_base_t::xsend_writable_target_known (
  const zlink_routing_id_t *target_rid_or_null_)
{
    return target_rid_or_null_ == NULL;
}

bool zlink::socket_base_t::xsend_writable_target_for_pipe (
  pipe_t *pipe_, zlink_routing_id_t *target_rid_out_)
{
    LIBZLINK_UNUSED (pipe_);
    LIBZLINK_UNUSED (target_rid_out_);
    return false;
}

void zlink::socket_base_t::publish_send_writable_target (
  const zlink_routing_id_t *target_rid_or_null_, bool correlation_released_)
{
    const int saved_errno = errno;
    const int published = socket_completion::publish_writable_waiters (
      &completion_runtime (), target_rid_or_null_, ZLINK_SEND_ADMITTED, 0,
      correlation_released_);
    if (published > 0) {
        // The completion command supplies async-owner progress and
        // POLLCOMPLETION. signal() is deliberately unconditional here: an
        // older unread REQUEST completion may already own the coalescing bit,
        // while this new WRITABLE batch must still wake a POLLOUT-only poller.
        notify_request_completion ();
        static_cast<mailbox_t *> (_mailbox)->signal ();
    }
    errno = saved_errno;
}

void zlink::socket_base_t::request_correlation_released (pipe_t *pipe_)
{
    LIBZLINK_UNUSED (pipe_);
    if (has_send_writable_wait ())
        publish_send_writable_target (NULL, true);
}

void zlink::socket_base_t::notify_send_writable (pipe_t *pipe_)
{
    if (!has_send_writable_wait ())
        return;

    const int type = options.type;
    if (type == ZLINK_CORE_SOCKET_PAIR) {
        // PAIR activation commands intentionally avoid the socket-wide send
        // lock. The callback owns a lifetime reference to this exact pipe, so
        // inspect it directly instead of racing pair_t::_pipe through xhas_out().
        if (pipe_ && pipe_->is_lifecycle_active ()
            && pipe_->check_write_admission ()
                 == pipe_message_admission_ready)
            publish_send_writable_target (NULL);
        return;
    }

    zlink_routing_id_t target_rid;
    const zlink_routing_id_t *target = NULL;
    if (type == ZLINK_CORE_SOCKET_ROUTER
        || type == ZLINK_CORE_SOCKET_STREAM) {
        memset (&target_rid, 0, sizeof (target_rid));
        if (!xsend_writable_target_for_pipe (pipe_, &target_rid))
            return;
        target = &target_rid;
    }

    // Route/pipe locks are acquired and released inside this predicate before
    // the completion mutex is taken by publish_send_writable_target().
    if (xsend_writable_target_ready (target))
        publish_send_writable_target (target);
}

int zlink::socket_base_t::register_send_writable_wait_after_failure (
  int failure_errno_, const zlink_routing_id_t *target_rid_or_null_,
  void *user_context_, zlink_completion_id_t *completion_id_out_,
  socket_completion::request_writable_wait_t *request_wait_)
{
    if (completion_id_out_)
        *completion_id_out_ = 0;

    if (failure_errno_ != EAGAIN && failure_errno_ != ENOTCONN
        && failure_errno_ != EHOSTUNREACH
        && failure_errno_ != ECONNREFUSED) {
        errno = failure_errno_;
        return -1;
    }

    const int type = options.type;
    const bool routed = type == ZLINK_CORE_SOCKET_ROUTER
                        || type == ZLINK_CORE_SOCKET_STREAM;
    if ((routed && !valid_routing_id (target_rid_or_null_))
        || (!routed && target_rid_or_null_)
        || (type != ZLINK_CORE_SOCKET_PAIR
            && type != ZLINK_CORE_SOCKET_DEALER && !routed)) {
        errno = routed ? EINVAL : ENOTSUP;
        return -1;
    }
    if (routed && !xsend_writable_target_known (target_rid_or_null_)) {
        errno = EHOSTUNREACH;
        return -1;
    }

    const bool correlation_wait = request_wait_ && !request_wait_->empty ();
    socket_completion::reservation_t *reservation = NULL;
    zlink_completion_id_t completion_id = 0;
    if (socket_completion::reserve_writable_wait (
          &completion_runtime (), user_context_, target_rid_or_null_,
          &reservation, &completion_id, request_wait_)
        != 0)
        return -1;
    zlink_assert (reservation);

    // Pair the writer's linked wait record with every credit/attach wake: a
    // wake that already ran is observed by the readiness recheck, while a
    // queued or later wake finds the fully linked token in the queue-owned
    // FIFO. Do not synchronously drain the mailbox here. A queued wake already
    // owns a mailbox notification, and draining it once per HWM refusal turns
    // credit recovery across many sockets into a serialized submit-side loop.
    std::atomic_thread_fence (std::memory_order_seq_cst);
    bool ready = false;
    if (correlation_wait)
        publish_send_writable_target (NULL, true);
    else {
        socket_public_send_scope_t readiness_scope (
          lifecycle_coordinator (), true);
        if (readiness_scope.acquired ())
            ready = xsend_writable_target_ready (
              routed ? target_rid_or_null_ : NULL);
    }
    if (ready)
        publish_send_writable_target (
          routed ? target_rid_or_null_ : NULL);

    if (completion_id_out_)
        *completion_id_out_ = completion_id;
    errno = EAGAIN;
    return 0;
}

void zlink::socket_base_t::clear_public_send_recovery_state ()
{
    dispatch_runtime ().clear_send_recovery_pending ();
}

// Physically attempt one complete record.
int zlink::socket_base_t::try_admit_send_parts_scoped (
  zlink_msg_t *parts_, size_t part_count_,
  const routed_send_target_key_t &target_, bool has_target_,
  socket_public_send_scope_t &scope, pipe_t **attempted_pipe_out_,
  bool commands_already_processed_, pipe_write_observer_fn observer_,
  void *observer_userdata_, bool request_admission_,
  bool manage_public_send_recovery_,
  const zlink_routing_id_t *transient_target_rid_,
  pipe_t *transient_selected_pipe_)
{
    if (!parts_ || part_count_ == 0 || !scope.acquired ()) {
        errno = EFAULT;
        return -1;
    }

    const size_t count = part_count_;
    bool submit_commands_processed = commands_already_processed_;
    bool pair_complete_record_eligible =
      count > 1 && options.type == ZLINK_CORE_SOCKET_PAIR && !has_target_
      && !transient_target_rid_ && !transient_selected_pipe_
      && target_.logical_endpoint.empty () && !observer_
      && !request_admission_
      && !socket_submit_retry_fault::pending ();
    if (pair_complete_record_eligible) {
        for (size_t i = 0; i != count; ++i) {
            msg_t *const part = reinterpret_cast<msg_t *> (&parts_[i]);
            if (!part->check ()
                || part->size ()
                     > static_cast<size_t> (UINT32_MAX)) {
                pair_complete_record_eligible = false;
                break;
            }
        }
    }
    if (pair_complete_record_eligible) {
        if (unlikely (_ctx_terminated)) {
            errno = ETERM;
            return -1;
        }
        if (!submit_commands_processed) {
            if (process_submit_commands () != 0)
                return -1;
            submit_commands_processed = true;
        }
        if (unlikely (_ctx_terminated)) {
            errno = ETERM;
            return -1;
        }
        for (size_t i = 0; i != count; ++i) {
            msg_t *const part = reinterpret_cast<msg_t *> (&parts_[i]);
            part->reset_flags (msg_t::more);
            if (i + 1 < count)
                part->set_flags (msg_t::more);
        }
        if (xtry_send_complete_record (
              reinterpret_cast<msg_t *> (parts_), count)) {
            _auto_hwm_send_attempts.fetch_add (
              static_cast<uint64_t> (count), std::memory_order_relaxed);
            if (manage_public_send_recovery_)
                clear_public_send_recovery_state ();
            return 0;
        }
    }

    zlink_routing_id_t rid;
    if (transient_target_rid_) {
        rid = *transient_target_rid_;
    } else if (has_target_) {
        memset (&rid, 0, sizeof (rid));
        copy_routing_id_from_bytes (target_.peer_rid.data (),
                                    target_.peer_rid.size (), &rid);
    }

    // The fallback admits multipart records frame by frame. Copy only the
    // current prefix/final frame when rollback may need the pristine source;
    // single-part sends transfer the source directly.
    const bool multipart_attempt = count > 1;
    for (size_t i = 0; i != count; ++i) {
        const int flags =
          ZLINK_DONTWAIT | (i + 1 < count ? ZLINK_SNDMORE : 0);
        // PAIR leaves a failed FINAL untouched and rolls its prefix back.
        // Pass that FINAL directly so a successful record does not create
        // and release an otherwise redundant message owner. Prefix parts
        // still need retry copies because a later frame can reject the
        // complete record. Other socket types retain defensive copies:
        // for example, ROUTER can consume a successful no-route drop, and
        // an observer can reject after a pipe accepted the frame.
        const bool copy_attempt_part =
          multipart_attempt
          && (i + 1 < count || options.type != ZLINK_CORE_SOCKET_PAIR
              || observer_ != NULL);
        zlink_msg_t attempt_part;
        msg_t *msg = reinterpret_cast<msg_t *> (
          copy_attempt_part ? &attempt_part : &parts_[i]);
        if (copy_attempt_part
            && copy_send_part_array (&parts_[i], 1, &attempt_part) != 0) {
            const int copy_errno = errno;
            if (i != 0)
                (void) rollback_scoped (scope);
            errno = copy_errno;
            return -1;
        }
        //  A routed target selects and pins the application pipe at the
        //  beginning of a logical record.  The socket's xsend path owns
        //  the continuation state after that first part: ROUTER keeps
        //  `_current_out`/`_more_out`, while DEALER keeps the load
        //  balancer's multipart pipe. Continue through ordinary xsend so
        //  the whole record remains one gated sequence.
        const bool routed_start =
          (has_target_ || transient_target_rid_) && i == 0;
        pipe_t *const selected_pipe = transient_selected_pipe_;
        const bool selected_pipe_start = i == 0 && selected_pipe != NULL;
        const bool configured_endpoint_start =
          i == 0 && !selected_pipe_start && !target_.logical_endpoint.empty ();
        const bool observe_commit = observer_ && i + 1 == count;
        // One complete PAIR record owns a single public send scope. The first
        // frame is its command-observation point; polling the same mailbox
        // again before FINAL only repeats throttle bookkeeping while the
        // multipart marker and send sync still exclude another physical
        // sender.
        const bool frame_commands_already_processed =
          submit_commands_processed
          || (options.type == ZLINK_CORE_SOCKET_PAIR && i != 0);
        const int rc = selected_pipe_start
                         ? xsend_selected_pipe (
                             selected_pipe, msg, flags, request_admission_,
                             NULL, observe_commit ? observer_ : NULL,
                             observe_commit ? observer_userdata_ : NULL)
                       : configured_endpoint_start
                         ? xsend_configured_endpoint (
                             target_.logical_endpoint, msg, flags,
                             request_admission_, attempted_pipe_out_, NULL,
                             observe_commit ? observer_ : NULL,
                             observe_commit ? observer_userdata_ : NULL)
                       : routed_start
                         ? send_direct_with_retry (
                             &rid, msg, flags, scope, NULL, 0, false,
                             attempted_pipe_out_, target_.transport_pair_id,
                             target_.transport_pair_generation, true,
                             frame_commands_already_processed,
                             observe_commit ? observer_ : NULL,
                             observe_commit ? observer_userdata_ : NULL, NULL,
                             target_.route_incarnation_id,
                             manage_public_send_recovery_, request_admission_)
                         : send_direct_with_retry (
                             NULL, msg, flags, scope, NULL, 0, false, NULL, 0,
                             0, true, frame_commands_already_processed,
                             observe_commit ? observer_ : NULL,
                             observe_commit ? observer_userdata_ : NULL, NULL,
                             0, manage_public_send_recovery_);
        if (rc == 0)
            continue;

        const int failure_errno = errno;
        if (copy_attempt_part && msg->check ()) {
            const int close_rc = msg->close ();
            errno_assert (close_rc == 0);
        }
        if (i == 0) {
            // Nothing reached the pipe, so the source record is untouched.
            errno = request_admission_
                        && (failure_errno == ESHUTDOWN
                            || failure_errno == ETERM)
                      ? EAGAIN
                      : failure_errno;
            return -1;
        }

        // Drop the half-written physical attempt before releasing the scope.
        // Framed sockets can discover byte HWM on a continuation.
        (void) rollback_scoped (scope);
        errno = failure_errno;
        return -1;
    }
    return 0;
}

void zlink::socket_base_t::notify_incremental_send_released ()
{
    lifecycle_coordinator ().release_public_multipart_control_boundary ();
    if (lifecycle_coordinator ().deferred_peer_controls_pending_cached ())
        flush_deferred_peer_controls ();
}

void zlink::socket_base_t::hold_incremental_send_control_boundary ()
{
    lifecycle_coordinator ().hold_public_multipart_control_boundary ();
}

void zlink::socket_base_t::clear_incremental_send_control_boundary ()
{
    lifecycle_coordinator ().release_public_multipart_control_boundary ();
}

void zlink::socket_base_t::flush_deferred_peer_controls ()
{
    if (!lifecycle_coordinator ().take_deferred_peer_controls ())
        return;
    if (lifecycle_coordinator ().public_multipart_send_active ()) {
        lifecycle_coordinator ().mark_deferred_peer_controls ();
        return;
    }

    std::vector<pipe_t *> targets;
    {
        scoped_lock_t lock (_transport_pairs_sync);
        for (transport_pairs_t::const_iterator it = _transport_pairs.begin (),
                                               end = _transport_pairs.end ();
             it != end; ++it) {
            pipe_t *const application = it->second.application;
            pipe_t *const completion = it->second.completion;
            if (application && application->retain_lifetime_ref ())
                targets.push_back (application);
            if (completion && completion->retain_lifetime_ref ())
                targets.push_back (completion);
        }
    }
    bool remains_pending = false;
    for (size_t i = 0; i != targets.size (); ++i) {
        (void) targets[i]->flush_pending_peer_controls ();
        remains_pending = targets[i]->has_pending_peer_controls ()
                          || remains_pending;
        targets[i]->release_lifetime_ref ();
    }
    if (remains_pending)
        lifecycle_coordinator ().mark_deferred_peer_controls ();
}

void zlink::socket_base_t::mark_deferred_peer_controls ()
{
    lifecycle_coordinator ().mark_deferred_peer_controls ();
}

void zlink::socket_base_t::publish_send_writable_terminal (
  const zlink_routing_id_t *target_rid_or_null_, int terminal_errno_)
{
    const int saved_errno = errno;
    const int published = socket_completion::publish_writable_waiters (
      &completion_runtime (), target_rid_or_null_, ZLINK_SEND_TERMINAL,
      terminal_errno_);
    if (published > 0) {
        notify_request_completion ();
        static_cast<mailbox_t *> (_mailbox)->signal ();
    }
    errno = saved_errno;
}

void zlink::socket_base_t::fail_blocking_send_waits_for_logical_target (
  const zlink_routing_id_t *peer_rid_, int terminal_errno_)
{
    // Explicit removal of a logical target retires its WRITABLE wait tokens
    // too: a waiter parked on that target could otherwise never be woken.
    publish_send_writable_terminal (peer_rid_, terminal_errno_);
    socket_blocking_send_runtime_t &wait_runtime = blocking_send_runtime ();
    const std::string logical_rid =
      peer_rid_ && peer_rid_->size
        ? std::string (reinterpret_cast<const char *> (peer_rid_->data),
                       peer_rid_->size)
        : std::string ();
    bool signaled_waiter = false;
    {
        scoped_lock_t lock (wait_runtime.sync);
        for (std::map<routed_send_target_key_t,
                      blocking_send_wait_state_t>::iterator it =
               wait_runtime.logical_waits.begin ();
             it != wait_runtime.logical_waits.end (); ++it) {
            if (it->first.logical_endpoint.empty ()
                && it->first.peer_rid == logical_rid) {
                fail_blocking_send_wait_state (&it->second, terminal_errno_);
                signaled_waiter = true;
            }
        }
    }
    if (signaled_waiter)
        notify_submit_progress ();
}

void zlink::socket_base_t::fail_blocking_send_waits_for_logical_endpoint (
  const std::string &endpoint_, int terminal_errno_)
{
    if (endpoint_.empty ())
        return;
    socket_blocking_send_runtime_t &wait_runtime = blocking_send_runtime ();
    bool signaled_waiter = false;
    {
        scoped_lock_t lock (wait_runtime.sync);
        for (std::map<routed_send_target_key_t,
                      blocking_send_wait_state_t>::iterator it =
               wait_runtime.logical_waits.begin ();
             it != wait_runtime.logical_waits.end (); ++it) {
            if (it->first.logical_endpoint == endpoint_) {
                fail_blocking_send_wait_state (&it->second, terminal_errno_);
                signaled_waiter = true;
            }
        }
    }
    if (signaled_waiter)
        notify_submit_progress ();
}

// Close / context termination wakes synchronous SEND waiters.
void zlink::socket_base_t::fail_all_blocking_send_waits (int terminal_errno_)
{
    socket_blocking_send_runtime_t &wait_runtime = blocking_send_runtime ();
    scoped_lock_t lock (wait_runtime.sync);
    for (std::map<routed_send_target_key_t,
                  blocking_send_wait_state_t>::iterator wait =
           wait_runtime.logical_waits.begin ();
         wait != wait_runtime.logical_waits.end (); ++wait)
        fail_blocking_send_wait_state (&wait->second, terminal_errno_);
}
