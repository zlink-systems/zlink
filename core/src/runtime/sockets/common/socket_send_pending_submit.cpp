/* SPDX-License-Identifier: MPL-2.0 */

// Pull-completion SEND and internal REQUEST admission submission. Queue
// driving lives in socket_send_complete.cpp; this module owns validation and
// message ownership transfer into the shared pending owner.

#include "utils/precompiled.hpp"

#include "core/c_api_copy_internal.hpp"

#include "core/pipe.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"
#include "utils/likely.hpp"
#include "utils/routing_id.hpp"

#include <optional>

namespace
{
//  Per-part byte charge for the pending bound. The byte axis reuses Core's
//  own minimum frame charge so a flood of tiny records cannot make the bound
//  meaningless.
uint64_t record_charge_bytes (const zlink_msg_t *parts_, size_t part_count_)
{
    const uint64_t minimum_frame_charge =
      static_cast<uint64_t> (sizeof (zlink::msg_t));
    uint64_t total = 0;
    for (size_t i = 0; i != part_count_; ++i) {
        const zlink::msg_t *msg =
          reinterpret_cast<const zlink::msg_t *> (&parts_[i]);
        const uint64_t size = static_cast<uint64_t> (msg->size ());
        const uint64_t part_charge =
          size > minimum_frame_charge ? size : minimum_frame_charge;
        if (UINT64_MAX - total < part_charge)
            return UINT64_MAX;
        total += part_charge;
    }
    return total;
}

void consume_caller_parts (zlink_msg_t *parts_, size_t part_count_)
{
    const int saved_errno = errno;
    for (size_t i = 0; i != part_count_; ++i) {
        zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (&parts_[i]);
        if (msg->check ()) {
            const int close_rc = msg->close ();
            errno_assert (close_rc == 0);
        }
        const int init_rc = msg->init ();
        errno_assert (init_rc == 0);
    }
    errno = saved_errno;
}

class logical_send_wait_guard_t
{
  public:
    logical_send_wait_guard_t (
      zlink::socket_send_pending_runtime_t *runtime_,
      const zlink::routed_send_target_key_t *target_) :
        _runtime (runtime_), _target (target_), _epoch (0), _acquired (false)
    {
    }

    ~logical_send_wait_guard_t ()
    {
        if (!_acquired)
            return;
        const int saved_errno = errno;
        zlink::scoped_lock_t lock (_runtime->sync);
        std::map<zlink::routed_send_target_key_t,
                 zlink::send_logical_wait_state_t>::iterator it =
          _runtime->logical_waits.find (*_target);
        zlink_assert (it != _runtime->logical_waits.end ());
        zlink_assert (it->second.waiters != 0);
        if (--it->second.waiters == 0)
            _runtime->logical_waits.erase (it);
        errno = saved_errno;
    }

    bool acquire ()
    {
        zlink::scoped_lock_t lock (_runtime->sync);
        try {
            const std::pair<
              std::map<zlink::routed_send_target_key_t,
                       zlink::send_logical_wait_state_t>::iterator,
              bool>
              inserted = _runtime->logical_waits.insert (
                std::make_pair (*_target,
                                zlink::send_logical_wait_state_t ()));
            ++inserted.first->second.waiters;
            _epoch = inserted.first->second.epoch;
            _acquired = true;
            return true;
        } catch (...) {
            errno = ENOMEM;
            return false;
        }
    }

    int failure_errno () const
    {
        if (!_acquired)
            return 0;
        zlink::scoped_lock_t lock (_runtime->sync);
        const std::map<zlink::routed_send_target_key_t,
                       zlink::send_logical_wait_state_t>::const_iterator it =
          _runtime->logical_waits.find (*_target);
        if (it == _runtime->logical_waits.end ()
            || it->second.epoch == _epoch)
            return 0;
        return it->second.terminal_errno != 0
                 ? it->second.terminal_errno
                 : EIO;
    }

  private:
    zlink::socket_send_pending_runtime_t *_runtime;
    const zlink::routed_send_target_key_t *_target;
    uint64_t _epoch;
    bool _acquired;
};

bool retryable_logical_send_errno (int err_)
{
    return err_ == EAGAIN || err_ == ENOTCONN || err_ == EHOSTUNREACH;
}

}

int zlink::socket_base_t::send_completion_submit (
  zlink_msg_t *parts_, size_t part_count_,
  const zlink_routing_id_t *target_rid_, void *user_context_,
  zlink_completion_id_t *completion_id_out_)
{
    if (completion_id_out_)
        *completion_id_out_ = 0;

    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    if (target_rid_)
        target.peer_rid = *target_rid_;

    return send_pending_submit (parts_, part_count_,
                                target_rid_ ? &target : NULL, NULL,
                                true, user_context_, completion_id_out_);
}

int zlink::socket_base_t::send_completion_submit_blocking (
  zlink_msg_t *parts_, size_t part_count_,
  const zlink_routing_id_t *target_rid_or_null_)
{
    if (!parts_ || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    const int type = options.type;
    if (type != ZLINK_CORE_SOCKET_PAIR && type != ZLINK_CORE_SOCKET_DEALER
        && type != ZLINK_CORE_SOCKET_ROUTER
        && type != ZLINK_CORE_SOCKET_STREAM) {
        errno = ENOTSUP;
        return -1;
    }
    if ((type == ZLINK_CORE_SOCKET_ROUTER
         || type == ZLINK_CORE_SOCKET_STREAM)
        && !valid_routing_id (target_rid_or_null_)) {
        errno = EINVAL;
        return -1;
    }
    if ((type == ZLINK_CORE_SOCKET_PAIR || type == ZLINK_CORE_SOCKET_DEALER)
        && target_rid_or_null_) {
        errno = EINVAL;
        return -1;
    }

    // SNDTIMEO is an entry snapshot.  Option changes made while this call is
    // waiting cannot extend or shorten its budget.
    const int timeout_snapshot = options.sndtimeo;
    const uint64_t deadline =
      timeout_snapshot < 0 ? 0 : _clock.now_ms () + timeout_snapshot;

    routed_send_target_key_t target;
    bool has_target = false;
    std::string logical_endpoint;

    const auto remaining_timeout = [&] () -> int {
        if (timeout_snapshot < 0)
            return -1;
        const uint64_t now = _clock.now_ms ();
        if (now >= deadline)
            return 0;
        const uint64_t remaining = deadline - now;
        return remaining > static_cast<uint64_t> (INT_MAX)
                 ? INT_MAX
                 : static_cast<int> (remaining);
    };

    const auto wait_for_progress = [&] () -> int {
        const int remaining = remaining_timeout ();
        if (remaining == 0) {
            errno = EAGAIN;
            return -1;
        }

        // Public close may be accepted while this call still owns a handle
        // pin.  Final destruction (and its mailbox stop command) is then
        // deferred until this call returns, so an infinite mailbox wait would
        // deadlock the close fence.  Poll in short slices and observe the
        // socket-owned lifecycle state directly.
        const int wait_slice =
          remaining < 0 ? 10 : (remaining < 10 ? remaining : 10);
        if (process_commands (wait_slice, false) != 0) {
            if (lifecycle_coordinator ().public_close_requested ())
                errno = ESHUTDOWN;
            return -1;
        }
        if (lifecycle_coordinator ().public_close_requested ()) {
            errno = ESHUTDOWN;
            return -1;
        }
        return 0;
    };

    bool fast_selected = false;
    if (type == ZLINK_CORE_SOCKET_DEALER) {
        //  Blocking-send fast path: select the pipe and admit the whole
        //  record to it under one scope, the way a plain DEALER send always
        //  did. Success returns without endpoint strings, attached-pipe
        //  snapshots or the pending machinery. A retryable refusal commits
        //  this selection to the configured-endpoint wait loop below (the
        //  same pipe, now addressed by its endpoint); any other failure is
        //  final, with the errno the general attempt would have produced.
        socket_public_send_scope_t fast_scope (
          lifecycle_coordinator (), true, socket_send_admission_complete);
        if (!fast_scope.acquired ())
            return -1;
        if (process_commands (0, true) != 0)
            return -1;
        pipe_t *selected = NULL;
        if (xselect_routed_submit_pipe (&selected, false) == 0 && selected) {
            routed_send_target_key_t fast_target;
            fast_target.selected_pipe = selected;
            if (try_admit_send_parts_scoped (parts_, part_count_, fast_target,
                                             false, fast_scope, NULL, true,
                                             NULL, NULL, false)
                == 0)
                return 0;
            const int fast_errno = errno;
            if (!retryable_logical_send_errno (fast_errno)) {
                errno = fast_errno;
                return -1;
            }
            //  Commit the selection while the scope still pins the pipe.
            try {
                logical_endpoint =
                  selected->get_endpoint_pair ().identifier ();
                const blob_t &rid = selected->get_routing_id ();
                target = routed_send_target_key_t (
                  rid.data (), rid.size (), 0, 0, 0, logical_endpoint);
            } catch (...) {
                errno = ENOMEM;
                return -1;
            }
            fast_selected = !logical_endpoint.empty ();
        }
        fast_scope.unlock_sync ();
    }

    if (type == ZLINK_CORE_SOCKET_DEALER && !fast_selected) {
        // Selection is committed once, at FINAL.  A never-handshaken endpoint
        // may become eligible during the same entry budget; after selection,
        // retries use only its configured endpoint.
        while (true) {
            zlink_routed_submit_target_t resolved;
            memset (&resolved, 0, sizeof (resolved));
            int select_errno = 0;
            bool selected = false;
            {
                socket_public_send_scope_t selection_scope (
                  lifecycle_coordinator (), true,
                  socket_send_admission_complete);
                if (!selection_scope.acquired ())
                    return -1;
                if (process_commands (0, true) != 0)
                    return -1;
                if (xselect_routed_submit_target (NULL, &resolved) == 0) {
                    std::vector<pipe_t *> attached;
                    snapshot_attached_pipes (&attached);
                    for (size_t i = 0; i != attached.size (); ++i) {
                        pipe_t *const pipe = attached[i];
                        if (!pipe
                            || pipe->get_transport_lane ()
                                 != transport_lane_application
                            || pipe->get_transport_pair_id ()
                                 != resolved.transport_pair_id
                            || pipe->get_transport_pair_generation ()
                                 != resolved.transport_pair_generation)
                            continue;
                        try {
                            logical_endpoint =
                              pipe->get_endpoint_pair ().identifier ();
                        } catch (...) {
                            errno = ENOMEM;
                            return -1;
                        }
                        break;
                    }
                    if (!logical_endpoint.empty ())
                        selected = true;
                    else
                        select_errno = EHOSTUNREACH;
                } else {
                    select_errno = errno;
                }
            }
            if (selected) {
                try {
                    target = routed_send_target_key_t (
                      resolved.peer_rid.data, resolved.peer_rid.size, 0, 0, 0,
                      logical_endpoint);
                } catch (...) {
                    errno = ENOMEM;
                    return -1;
                }
                break;
            }
            if (!retryable_logical_send_errno (select_errno)) {
                errno = select_errno;
                return -1;
            }
            if (wait_for_progress () != 0)
                return -1;
        }
    } else if (type == ZLINK_CORE_SOCKET_ROUTER
               || type == ZLINK_CORE_SOCKET_STREAM) {
        try {
            target = routed_send_target_key_t (
              target_rid_or_null_->data, target_rid_or_null_->size, 0, 0);
        } catch (...) {
            errno = ENOMEM;
            return -1;
        }
        has_target = true;
    }

    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    logical_send_wait_guard_t logical_wait (&pending, &target);
    bool logical_wait_registered = false;

    const auto normalize_wait_failure = [&] (int err_) -> int {
        return err_ == ECANCELED
                   && lifecycle_coordinator ().public_close_requested ()
                 ? ESHUTDOWN
                 : err_;
    };

    while (true) {
        int explicit_failure =
          normalize_wait_failure (logical_wait.failure_errno ());
        if (explicit_failure != 0) {
            errno = explicit_failure;
            return -1;
        }

        int admission_errno = 0;
        int admission_rc = -1;
        {
            socket_public_send_scope_t physical_scope (
              lifecycle_coordinator (), true,
              socket_send_admission_complete);
            if (!physical_scope.acquired ())
                return -1;
            if (process_commands (0, true) == 0) {
                admission_rc = try_admit_send_parts_scoped (
                  parts_, part_count_, target, has_target, physical_scope,
                  NULL, true, NULL, NULL, false);
            }
            admission_errno = errno;

            // Most blocking sends admit immediately. Register a logical
            // waiter only once the call will actually release the lifecycle
            // sync and wait. Keeping registration inside the physical scope
            // preserves the detach/removal fence: a terminal handoff cannot
            // slip between the failed admission and waiter publication.
            if (admission_rc != 0
                && retryable_logical_send_errno (admission_errno)
                && !logical_wait_registered) {
                if (logical_wait.acquire ()) {
                    logical_wait_registered = true;
                } else {
                    admission_errno = errno;
                }
            }
            // A successful admission can release the sync bit and inflight
            // count together in the scope destructor. A failed attempt must
            // drop only the sync bit before it waits for progress.
            if (admission_rc != 0)
                physical_scope.unlock_sync ();
        }
        if (admission_rc == 0)
            return 0;

        explicit_failure =
          normalize_wait_failure (logical_wait.failure_errno ());
        if (explicit_failure != 0) {
            errno = explicit_failure;
            return -1;
        }
        if (!retryable_logical_send_errno (admission_errno)) {
            errno = admission_errno;
            return -1;
        }
        if (wait_for_progress () != 0)
            return -1;
    }
}

int zlink::socket_base_t::request_admission_submit (
  zlink_msg_t *parts_, size_t part_count_,
  const zlink_routing_id_t *target_rid_or_null_,
  pipe_write_observer_fn admission_observer_,
  void *admission_observer_userdata_,
  send_pending_request_resolved_fn resolved_,
  send_pending_request_cleanup_fn cleanup_,
  void *resolution_context_, bool *pending_out_)
{
    if (pending_out_)
        *pending_out_ = false;

    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    if (target_rid_or_null_)
        target.peer_rid = *target_rid_or_null_;

    zlink_send_op_id_t internal_op_id = 0;
    const int rc = send_pending_submit (
      parts_, part_count_, target_rid_or_null_ ? &target : NULL,
      &internal_op_id, true, NULL, NULL,
      true, admission_observer_, admission_observer_userdata_, resolved_,
      cleanup_, resolution_context_);
    if (rc == 0 && pending_out_)
        *pending_out_ = internal_op_id != 0;
    return rc;
}

int zlink::socket_base_t::request_admission_submit_blocking (
  zlink_msg_t *parts_, size_t part_count_,
  const zlink_routing_id_t *target_rid_or_null_,
  pipe_write_observer_fn admission_observer_,
  void *admission_observer_userdata_)
{
    if (!parts_ || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    zlink_routed_submit_target_t resolved;
    memset (&resolved, 0, sizeof (resolved));
    uint64_t resolved_connection_id = 0;
    uint64_t resolved_route_incarnation_id = 0;
    std::string logical_endpoint;
    int timeout = options.sndtimeo;
    const uint64_t end = timeout < 0 ? 0 : _clock.now_ms () + timeout;
    bool fast_selected = false;
    if (options.type == ZLINK_CORE_SOCKET_DEALER && !target_rid_or_null_) {
        //  Same fast path as the blocking send: select the ROUTER-peer pipe
        //  and admit the request record to it under one scope. Backpressure
        //  commits this selection to the configured-endpoint retry loop
        //  below; every other refusal is final, as in the general attempt.
        socket_public_send_scope_t fast_scope (
          lifecycle_coordinator (), true, socket_send_admission_complete);
        if (!fast_scope.acquired ())
            return -1;
        if (process_commands (0, true) != 0)
            return -1;
        pipe_t *selected = NULL;
        if (xselect_routed_submit_pipe (&selected, true) == 0 && selected) {
            routed_send_target_key_t fast_target;
            fast_target.selected_pipe = selected;
            const int rc = try_admit_send_parts_scoped (
              parts_, part_count_, fast_target, false, fast_scope, NULL,
              true, admission_observer_, admission_observer_userdata_, true);
            if (rc == 0) {
                errno = 0;
                return 0;
            }
            const int fast_errno = errno;
            if (fast_errno != EAGAIN) {
                errno = fast_errno;
                return -1;
            }
            try {
                logical_endpoint =
                  selected->get_endpoint_pair ().identifier ();
                const blob_t &rid = selected->get_routing_id ();
                copy_routing_id_from_bytes (rid.data (), rid.size (),
                                            &resolved.peer_rid);
            } catch (...) {
                errno = ENOMEM;
                return -1;
            }
            fast_selected = !logical_endpoint.empty ();
        }
        fast_scope.unlock_sync ();
    }
    while (!fast_selected) {
        int selection_errno = 0;
        {
            socket_public_send_scope_t selection_scope (
              lifecycle_coordinator (), true, socket_send_admission_complete);
            if (!selection_scope.acquired ())
                return -1;
            if (process_commands (0, true) != 0)
                return -1;
            if (xselect_request_submit_target (
                  target_rid_or_null_, &resolved, &resolved_connection_id,
                  &resolved_route_incarnation_id, &logical_endpoint)
                == 0)
                break;
            selection_errno = errno;
        }

        const bool dealer_wait =
          options.type == ZLINK_CORE_SOCKET_DEALER
          && (selection_errno == ENOTCONN
              || selection_errno == ECONNREFUSED);
        if (!dealer_wait || timeout == 0) {
            errno = selection_errno;
            return -1;
        }
        if (timeout > 0) {
            const uint64_t now = _clock.now_ms ();
            if (now >= end) {
                errno = selection_errno;
                return -1;
            }
            timeout = static_cast<int> (end - now);
        }
        if (process_commands (timeout, false) != 0)
            return -1;
        if (timeout > 0) {
            const uint64_t now = _clock.now_ms ();
            if (now >= end)
                timeout = 0;
            else
                timeout = static_cast<int> (end - now);
        }
    }

    routed_send_target_key_t target;
    try {
        target = routed_send_target_key_t (
          resolved.peer_rid.data, resolved.peer_rid.size, 0, 0, 0,
          logical_endpoint);
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }

    while (true) {
        socket_public_send_scope_t physical_scope (
          lifecycle_coordinator (), true, socket_send_admission_complete);
        if (!physical_scope.acquired ())
            return -1;
        const int rc = try_admit_send_parts_scoped (
          parts_, part_count_, target, true, physical_scope, NULL, false,
          admission_observer_, admission_observer_userdata_, true);
        const int saved_errno = errno;
        physical_scope.unlock_sync ();
        if (rc == 0) {
            errno = 0;
            return 0;
        }
        if (saved_errno != EAGAIN || timeout == 0) {
            errno = saved_errno;
            return -1;
        }

        if (timeout > 0) {
            const uint64_t now = _clock.now_ms ();
            if (now >= end) {
                errno = EAGAIN;
                return -1;
            }
            timeout = static_cast<int> (end - now);
        }
        const int process_rc = process_commands (timeout, false);
        if (process_rc != 0)
            return -1;
        if (timeout > 0) {
            const uint64_t now = _clock.now_ms ();
            if (now >= end) {
                errno = EAGAIN;
                return -1;
            }
            timeout = static_cast<int> (end - now);
        }
    }
}

int zlink::socket_base_t::send_pending_submit (
  zlink_msg_t *parts_, size_t part_count_,
  const zlink_routed_submit_target_t *target_, zlink_send_op_id_t *op_id_out_,
  bool pull_completion_, void *user_context_,
  zlink_completion_id_t *completion_id_out_, bool request_admission_,
  pipe_write_observer_fn admission_observer_,
  void *admission_observer_userdata_,
  send_pending_request_resolved_fn request_resolved_,
  send_pending_request_cleanup_fn request_cleanup_,
  void *request_resolution_context_)
{
    if (op_id_out_)
        *op_id_out_ = 0;
    if (completion_id_out_)
        *completion_id_out_ = 0;

    // Admit at the common complete-record boundary.
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    socket_public_send_scope_t admission (lifecycle, true);
    if (!admission.acquired ())
        return -1;

    if (!parts_ || part_count_ == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!socket_type_supports_send_completion (options.type)) {
        errno = ENOTSUP;
        return -1;
    }
    //  STREAM carries raw TCP bytes with no frame boundaries, so a record is
    //  always exactly one part there.
    if (options.type == ZLINK_CORE_SOCKET_STREAM && part_count_ != 1) {
        errno = ENOTSUP;
        return -1;
    }
    for (size_t i = 0; i != part_count_; ++i) {
        if (!reinterpret_cast<msg_t *> (&parts_[i])->check ()) {
            errno = EFAULT;
            return -1;
        }
    }
    if (!pull_completion_ && !request_admission_) {
        errno = EINVAL;
        return -1;
    }
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    typedef std::map<routed_send_target_key_t, send_inline_attempt_state_t>
      inline_attempt_map_t;
    const auto reserve_inline_attempt_locked = [&pending] (
                                                 const routed_send_target_key_t
                                                   &key_,
                                                 bool retain_,
                                                 inline_attempt_map_t::iterator
                                                   *attempt_out_) -> bool {
        if (attempt_out_)
            *attempt_out_ = pending.inline_attempts.end ();
        inline_attempt_map_t::iterator attempt =
          pending.inline_attempts.find (key_);
        if (attempt != pending.inline_attempts.end ()) {
            if (attempt->second.active || attempt->second.retire)
                return false;
            attempt->second.active = true;
            attempt->second.retained =
              attempt->second.retained || retain_;
            if (attempt_out_)
                *attempt_out_ = attempt;
            return true;
        }
        const std::pair<inline_attempt_map_t::iterator, bool> inserted =
          pending.inline_attempts.insert (std::make_pair (
            key_, send_inline_attempt_state_t (true, retain_)));
        if (inserted.second && attempt_out_)
            *attempt_out_ = inserted.first;
        return inserted.second;
    };
    const auto release_inline_attempt_locked = [&pending] (
                                                 const routed_send_target_key_t
                                                   &key_) -> bool {
        std::map<routed_send_target_key_t,
                 send_inline_attempt_state_t>::iterator attempt =
          pending.inline_attempts.find (key_);
        if (attempt == pending.inline_attempts.end ()
            || !attempt->second.active)
            return false;
        attempt->second.active = false;
        if (!attempt->second.retained || attempt->second.retire)
            pending.inline_attempts.erase (attempt);
        return true;
    };
    routed_send_target_key_t target;
    bool has_target = false;
    bool deferred_target_select = false;
    std::string request_logical_endpoint;

    //  Resolve the exact target now. Deferring the choice to completion time
    //  would make per-target FIFO order impossible to state.
    try {
        if (options.type == ZLINK_CORE_SOCKET_ROUTER
                   || options.type == ZLINK_CORE_SOCKET_STREAM
                   || options.type == ZLINK_CORE_SOCKET_DEALER) {
            zlink_routed_submit_target_t resolved;
            uint64_t resolved_connection_id = 0;
            uint64_t resolved_route_incarnation_id = 0;
            memset (&resolved, 0, sizeof (resolved));
            if (target_) {
                resolved = *target_;
                //  A routed binding may provide only the peer id and let this
                //  submit snapshot the exact transport pair. This folds target
                //  selection into the same public API admission instead of
                //  requiring a second Core boundary crossing per message.
                if (resolved.transport_pair_id == 0
                    && resolved.transport_pair_generation == 0
                    && (options.type == ZLINK_CORE_SOCKET_ROUTER
                        || options.type == ZLINK_CORE_SOCKET_STREAM)) {
                    if (!valid_routing_id (&resolved.peer_rid)) {
                        errno = EINVAL;
                        return -1;
                    }
                    if (!request_admission_
                        && options.type == ZLINK_CORE_SOCKET_ROUTER
                        && part_count_ == 1) {
                        deferred_target_select = true;
                    } else {
                        if (process_commands (0, true) != 0)
                            return -1;
                        const zlink_routing_id_t requested_rid = resolved.peer_rid;
                        const int select_rc = request_admission_
                          ? xselect_request_submit_target (
                              &requested_rid, &resolved,
                              &resolved_connection_id,
                              &resolved_route_incarnation_id,
                              &request_logical_endpoint)
                          : xselect_routed_submit_target_internal (
                              &requested_rid, &resolved,
                              &resolved_connection_id,
                              &resolved_route_incarnation_id);
                        if (select_rc
                            != 0)
                            return -1;
                    }
                }
            } else {
                if (options.type != ZLINK_CORE_SOCKET_DEALER) {
                    errno = EINVAL;
                    return -1;
                }
                if (process_commands (0, true) != 0)
                    return -1;
                const int select_rc = request_admission_
                  ? xselect_request_submit_target (
                      NULL, &resolved, &resolved_connection_id,
                      &resolved_route_incarnation_id,
                      &request_logical_endpoint)
                  : xselect_routed_submit_target (NULL, &resolved);
                if (select_rc != 0)
                    return -1;
            }
            const bool resolved_pair = resolved.transport_pair_id != 0
                                       && resolved.transport_pair_generation != 0;
            const bool resolved_unpaired = resolved.transport_pair_id == 0
                                           && resolved.transport_pair_generation == 0
                                           && resolved_connection_id != 0
                                           && resolved_route_incarnation_id != 0;
            std::string logical_endpoint = request_logical_endpoint;
            const bool dealer_endpoint_target =
              options.type == ZLINK_CORE_SOCKET_DEALER
              && !logical_endpoint.empty ();
            if ((!dealer_endpoint_target
                 && !valid_routing_id (&resolved.peer_rid))
                || (!deferred_target_select && !dealer_endpoint_target
                    && !resolved_pair
                    && !resolved_unpaired)) {
                errno = EINVAL;
                return -1;
            }
            if (pull_completion_
                && options.type == ZLINK_CORE_SOCKET_DEALER
                && !request_admission_ && !deferred_target_select) {
                std::vector<pipe_t *> attached;
                snapshot_attached_pipes (&attached);
                for (size_t i = 0; i != attached.size (); ++i) {
                    pipe_t *const pipe = attached[i];
                    if (!pipe
                        || pipe->get_transport_lane ()
                             != transport_lane_application
                        || pipe->get_transport_pair_id ()
                             != resolved.transport_pair_id
                        || pipe->get_transport_pair_generation ()
                             != resolved.transport_pair_generation)
                        continue;
                    logical_endpoint =
                      pipe->get_endpoint_pair ().identifier ();
                    break;
                }
                if (logical_endpoint.empty ()) {
                    errno = EHOSTUNREACH;
                    return -1;
                }
            }
            if (!deferred_target_select)
                target = routed_send_target_key_t (
                  resolved.peer_rid.data, resolved.peer_rid.size,
                  pull_completion_ ? 0 : resolved.transport_pair_id,
                  pull_completion_ ? 0 : resolved.transport_pair_generation,
                  pull_completion_
                    ? 0
                    : resolved_unpaired ? resolved_route_incarnation_id : 0,
                  logical_endpoint);
            else
                target = routed_send_target_key_t (
                  resolved.peer_rid.data, resolved.peer_rid.size, 0, 0);
            has_target = true;
        }
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
    bool attempt_inline = false;
    bool gate_acquired = false;
    routed_send_target_key_t inline_reservation_key;
    try {
        inline_reservation_key = target;
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
    bool target_reserved = false;
    bool reservation_allocation_failed = false;
    {
        scoped_lock_t lock (pending.sync);
        if (pending.failing) {
            errno = ESHUTDOWN;
            return -1;
        }
        bool target_queue_empty = true;
        if (deferred_target_select) {
            for (std::map<routed_send_target_key_t,
                          std::deque<send_pending_record_t *> >::const_iterator
                   it = pending.queues.begin ();
                 it != pending.queues.end (); ++it) {
                if (!it->second.empty ()
                    && it->first.peer_rid == target.peer_rid) {
                    target_queue_empty = false;
                    break;
                }
            }
        } else {
            const std::map<routed_send_target_key_t,
                           std::deque<send_pending_record_t *> >::const_iterator
              queue = pending.queues.find (target);
            target_queue_empty =
              queue == pending.queues.end () || queue->second.empty ();
        }
        if (target_queue_empty) {
            try {
                if (reserve_inline_attempt_locked (inline_reservation_key,
                                                   false, NULL))
                    target_reserved = true;
            } catch (...) {
                reservation_allocation_failed = true;
            }
        }
    }
    if (reservation_allocation_failed) {
        errno = ENOMEM;
        return -1;
    }
    if (target_reserved) {
        bool gate_expected = false;
        gate_acquired = pending.admission_gate.compare_exchange_strong (
          gate_expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire);
    }
    if (gate_acquired) {
        attempt_inline = true;
    } else {
        if (target_reserved) {
            scoped_lock_t lock (pending.sync);
            if (release_inline_attempt_locked (inline_reservation_key))
                pending.redrive_epoch.fetch_add (1,
                                                 std::memory_order_release);
        }
    }
    // Physical admission has its own complete-record scope below. Release the
    // boundary lock before taking it so an incremental multipart sequence can
    // win this race and turn the async operation into pending work instead of
    // being rejected at submit time.
    admission.unlock_sync ();
    if (deferred_target_select && !attempt_inline) {
        zlink_routed_submit_target_t resolved;
        zlink_routing_id_t requested_rid;
        uint64_t resolved_connection_id = 0;
        uint64_t resolved_route_incarnation_id = 0;
        memset (&resolved, 0, sizeof (resolved));
        memset (&requested_rid, 0, sizeof (requested_rid));
        requested_rid = target_->peer_rid;
        if (select_routed_submit_target_internal (
              &requested_rid, &resolved, &resolved_connection_id,
              &resolved_route_incarnation_id)
            != 0)
            return -1;
        try {
            target = routed_send_target_key_t (
              resolved.peer_rid.data, resolved.peer_rid.size,
              pull_completion_ ? 0 : resolved.transport_pair_id,
              pull_completion_ ? 0 : resolved.transport_pair_generation,
              pull_completion_
                ? 0
                : resolved.transport_pair_id == 0
                ? resolved_route_incarnation_id
                : 0);
        } catch (...) {
            errno = ENOMEM;
            return -1;
        }
        deferred_target_select = false;
    }

    // Preserve direct admission when no operation is ahead of this target.
    // Success is reported by completion id 0 and creates no queue record.
    std::optional<send_pending_record_t> inline_record;
    int inline_terminal_errno = 0;
    bool inline_record_owns_copies = false;
    int inline_errno = EAGAIN;
    routed_send_attempt_identity_t attempted_identity;
    if (attempt_inline) {
        // The pull API consumes every input on every result and can use caller
        // storage for the direct attempt; it creates a shadow only for the
        // exact EAGAIN fallback.
            try {
                inline_record.emplace ();
                inline_record->target = target;
            } catch (...) {
                pending.admission_gate.store (false,
                                              std::memory_order_release);
                {
                    scoped_lock_t lock (pending.sync);
                    release_inline_attempt_locked (
                      inline_reservation_key);
                }
                admission.unlock_sync ();
                drive_send_pending ();
                errno = ENOMEM;
                return -1;
            }
            inline_record->has_target = has_target;
            zlink_msg_t *const attempt_parts = parts_;
            const size_t attempt_part_count = part_count_;
            socket_public_send_scope_t physical_admission (
              lifecycle, true, socket_send_admission_complete);
            int inline_rc = -1;
            if (physical_admission.acquired ()) {
                inline_rc = deferred_target_select
                              ? send_routed_scoped (
                                  &target_->peer_rid,
                                  reinterpret_cast<msg_t *> (
                                    attempt_parts),
                                  ZLINK_DONTWAIT, physical_admission, NULL, 0,
                                  NULL, 0, 0, false,
                                  admission_observer_,
                                  admission_observer_userdata_,
                                  &attempted_identity)
                              : try_admit_send_parts_scoped (
                                  attempt_parts, attempt_part_count, target,
                                  has_target, physical_admission, NULL, false,
                                  admission_observer_,
                                  admission_observer_userdata_,
                                  request_admission_);
            } else {
                // An open incremental sequence is physical admission
                // contention, not an invalid async submission. Preserve the
                // record and retry after the multipart marker is released.
                errno = EAGAIN;
            }
            inline_errno = errno;
            physical_admission.unlock_sync ();
            if (inline_rc == 0) {
                pending.admission_gate.store (false,
                                              std::memory_order_release);
                {
                    scoped_lock_t lock (pending.sync);
                    release_inline_attempt_locked (
                      inline_reservation_key);
                }
                //  The pipe now owns its frame references. Close Core's moved
                //  copies, then consume the caller's original references only
                //  after the async submit has committed successfully.
                if (inline_record_owns_copies)
                    close_send_parts (&inline_record->parts);
                inline_record_owns_copies = false;
                consume_caller_parts (parts_, part_count_);
                //  Operations queued behind the direct attempt may also fit.
                if (pending.pending_msgs.load (std::memory_order_acquire)
                    != 0) {
                    drive_send_pending ();
                }
                return 0;
            }
            if (inline_errno != EAGAIN) {
                pending.admission_gate.store (false,
                                              std::memory_order_release);
                {
                    scoped_lock_t lock (pending.sync);
                    release_inline_attempt_locked (
                      inline_reservation_key);
                }
                errno = inline_errno;
                return -1;
            }
            if (copy_send_parts (parts_, part_count_, &inline_record->parts)
                     != 0) {
                pending.admission_gate.store (false,
                                              std::memory_order_release);
                {
                    scoped_lock_t lock (pending.sync);
                    release_inline_attempt_locked (
                      inline_reservation_key);
                }
                errno = ENOMEM;
                return -1;
            }
            inline_record_owns_copies = true;

        if (deferred_target_select) {
            const bool attempted_pair =
              attempted_identity.transport_pair_id != 0
              && attempted_identity.transport_pair_generation != 0;
            const bool attempted_unpaired =
              attempted_identity.transport_pair_id == 0
              && attempted_identity.transport_pair_generation == 0
              && attempted_identity.transport_connection_id != 0
              && attempted_identity.route_incarnation_id != 0;
            if (!attempted_pair && !attempted_unpaired) {
                pending.admission_gate.store (false,
                                              std::memory_order_release);
                {
                    scoped_lock_t lock (pending.sync);
                    release_inline_attempt_locked (
                      inline_reservation_key);
                }
                close_send_parts (&inline_record->parts);
                inline_record_owns_copies = false;
                errno = inline_errno;
                return -1;
            }
            try {
                target = routed_send_target_key_t (
                  target_->peer_rid.data,
                  target_->peer_rid.size,
                  pull_completion_ ? 0
                                   : attempted_identity.transport_pair_id,
                  pull_completion_ ? 0
                                   : attempted_identity.transport_pair_generation,
                  pull_completion_
                    ? 0
                    : attempted_unpaired
                    ? attempted_identity.route_incarnation_id
                    : 0);
            } catch (...) {
                pending.admission_gate.store (false,
                                              std::memory_order_release);
                {
                    scoped_lock_t lock (pending.sync);
                    release_inline_attempt_locked (
                      inline_reservation_key);
                }
                close_send_parts (&inline_record->parts);
                inline_record_owns_copies = false;
                errno = ENOMEM;
                return -1;
            }
            deferred_target_select = false;
        }

        if (inline_errno != EAGAIN) {
            //  A multipart attempt can fail after its first part reached the
            //  pipe and was rolled back. Preserve the accepted-operation
            //  ownership rule for every post-validation attempt: publish a
            //  non-zero operation id and report this terminal through the
            //  completion channel. The caller originals remain untouched
            //  until the pending record and operation id both commit.
            inline_terminal_errno = inline_errno;
        }
    }

    const uint64_t charge = record_charge_bytes (parts_, part_count_);
    socket_completion::reservation_t *completion_reservation = NULL;
    zlink_completion_id_t completion_id = 0;
    int pending_reject_errno = 0;
    if (pull_completion_ && !request_admission_) {
        const zlink_routing_id_t *completion_peer =
          (options.type == ZLINK_CORE_SOCKET_ROUTER
           || options.type == ZLINK_CORE_SOCKET_STREAM)
            ? target_ ? &target_->peer_rid : NULL
            : NULL;
        if (socket_completion::reserve (
              &completion_runtime (), ZLINK_COMPLETION_SEND, user_context_,
              completion_peer, &completion_reservation, &completion_id)
            != 0)
            pending_reject_errno = errno;
    }
    send_pending_record_t *record =
      pending_reject_errno == 0
        ? new (std::nothrow) send_pending_record_t ()
        : NULL;
    bool record_owns_copies = false;
    zlink_send_op_id_t op_id = 0;
    if (!record && pending_reject_errno == 0)
        pending_reject_errno = ENOMEM;
    if (record) {
        record->pull_completion = pull_completion_;
        record->request_admission = request_admission_;
        record->completion_reservation = completion_reservation;
        record->admission_observer = admission_observer_;
        record->admission_observer_userdata = admission_observer_userdata_;
        record->request_resolution_context = request_resolution_context_;
        record->request_resolved = request_resolved_;
        record->request_cleanup = request_cleanup_;
        try {
            record->target = target;
            if (attempt_inline) {
                record->parts.swap (inline_record->parts);
                record_owns_copies = true;
                inline_record_owns_copies = false;
            } else {
                record->parts.assign (parts_, parts_ + part_count_);
            }
        } catch (...) {
            pending_reject_errno = ENOMEM;
        }
        record->has_target = has_target;
        record->charge_bytes = charge;
        record->claimed = inline_terminal_errno != 0;
    }

    //  Reserve queue capacity only after the pending record is completely
    //  prepared. This keeps allocation failure outside the synchronized
    //  runtime state and makes rejection leave caller ownership intact.
    std::unique_lock<std::mutex> target_publication_lock;
    if (has_target) {
        if (std::mutex *const sync = send_pending_target_mutex ())
            target_publication_lock = std::unique_lock<std::mutex> (*sync);
    }
    if (pending_reject_errno == 0) {
        scoped_lock_t lock (pending.sync);
        if (inline_terminal_errno == 0 && pending.failing) {
            pending_reject_errno = ESHUTDOWN;
        } else {
            // STREAM detach can retire the exact target after the current-pipe
            // attempt returned EAGAIN but before this record is published.
            // The fail sweep cannot see an unpublished record, so preserve the
            // accepted-operation contract by publishing it terminal here.
            if (inline_terminal_errno == 0 && attempt_inline) {
                const std::map<
                  routed_send_target_key_t,
                  send_inline_attempt_state_t>::const_iterator attempt =
                  pending.inline_attempts.find (inline_reservation_key);
                if (attempt != pending.inline_attempts.end ()
                    && attempt->second.active && attempt->second.retire) {
                    inline_terminal_errno =
                      attempt->second.retire_errno != 0
                        ? attempt->second.retire_errno
                        : ENOTCONN;
                    record->claimed = true;
                }
            }
            // A non-zero option is an explicit application overload policy;
            // zero means unlimited and normal HWM pressure stays pending.
            const uint64_t max_msgs = options.send_pending_max_msgs;
            const uint64_t max_bytes = options.send_pending_max_bytes;
            const uint64_t reserved_bytes =
              UINT64_MAX - pending.pending_bytes < charge
                ? UINT64_MAX
                : pending.pending_bytes + charge;
            if (inline_terminal_errno == 0
                && ((max_msgs > 0
                     && pending.pending_msgs.load (std::memory_order_relaxed)
                          + 1
                          > max_msgs)
                    || (max_bytes > 0
                        && reserved_bytes > max_bytes))) {
                pending_reject_errno = EAGAIN;
            } else if (pending.next_op_id == 0) {
                //  Operation id zero permanently denotes immediate admission.
                //  Once the socket-local uint64 sequence wraps, refuse new
                //  pending ownership instead of publishing an unobservable
                //  operation that cannot be correlated.
                pending_reject_errno = EOVERFLOW;
            } else {
                const zlink_send_op_id_t candidate_op_id = pending.next_op_id;
                typedef std::map<routed_send_target_key_t,
                                 std::deque<send_pending_record_t *> >
                  queue_map_t;
                queue_map_t::iterator queue = pending.queues.end ();
                bool queue_created = false;
                bool queued = false;
                bool indexed = false;
                try {
                    const std::pair<queue_map_t::iterator, bool> inserted_queue =
                      pending.queues.insert (
                        std::make_pair (
                          target, std::deque<send_pending_record_t *> ()));
                    queue = inserted_queue.first;
                    queue_created = inserted_queue.second;
                    //  Keep the inline reservation until the original
                    //  operation is at the front, so a later submit cannot
                    //  overtake it between EAGAIN and queue insertion.
                    if (attempt_inline)
                        queue->second.push_front (record);
                    else
                        queue->second.push_back (record);
                    queued = true;
                    indexed = pending.by_op.insert (
                      std::make_pair (candidate_op_id, record)).second;
                    if (!indexed)
                        pending_reject_errno = pending_reject_errno != 0
                                                  ? pending_reject_errno
                                                  : EOVERFLOW;
                } catch (...) {
                    pending_reject_errno = ENOMEM;
                }

                if (pending_reject_errno != 0) {
                    if (queued) {
                        if (attempt_inline)
                            queue->second.pop_front ();
                        else
                            queue->second.pop_back ();
                    }
                    if (queue_created && queue != pending.queues.end ()
                        && queue->second.empty ())
                        pending.queues.erase (queue);
                } else {
                    //  Publish the id and counters only after both container
                    //  insertions commit. Until here a failure still returns
                    //  caller ownership exactly as the async API promises.
                    record->op_id = candidate_op_id;
                    ++pending.next_op_id;
                    pending.pending_msgs.fetch_add (
                      1, std::memory_order_release);
                    pending.pending_bytes = reserved_bytes;
                    pending.enqueue_epoch.fetch_add (
                      1, std::memory_order_release);
                    op_id = candidate_op_id;
                }
            }
        }
        if (attempt_inline) {
            (void) release_inline_attempt_locked (inline_reservation_key);
        }
    }
    if (target_publication_lock.owns_lock ())
        target_publication_lock.unlock ();

    if (attempt_inline) {
        {
            scoped_lock_t lock (pending.sync);
            (void) release_inline_attempt_locked (inline_reservation_key);
        }
        if (gate_acquired)
            pending.admission_gate.store (false,
                                          std::memory_order_release);
    }

    if (pending_reject_errno != 0) {
        //  No ownership transfer occurred on rejection. Release the target
        //  reservation and let already-queued followers make progress.  The
        if (inline_record) {
            if (inline_record_owns_copies)
                close_send_parts (&inline_record->parts);
            else
                inline_record->parts.clear ();
        }
        if (record) {
            if (record_owns_copies)
                close_send_parts (&record->parts);
            else
                record->parts.clear ();
            delete record;
            record = NULL;
        }
        if (completion_reservation)
            socket_completion::release (&completion_runtime (),
                                        completion_reservation);
        drive_send_pending ();
        errno = pending_reject_errno;
        return -1;
    }
    // `record` is no longer safe to dereference once the pending mutex is
    // released: the admit loop may already have completed and destroyed it.

    //  With an inline shadow record Core owns copied references, so release
    //  the originals. The queued-only path transferred the original handles
    //  structurally and only needs to blank the caller's array.
    if (record_owns_copies)
        consume_caller_parts (parts_, part_count_);
    else {
        for (size_t i = 0; i != part_count_; ++i) {
            const int rc = reinterpret_cast<msg_t *> (&parts_[i])->init ();
            errno_assert (rc == 0);
        }
    }
    if (op_id_out_)
        *op_id_out_ = op_id;
    if (completion_id_out_)
        *completion_id_out_ = completion_id;

    if (inline_terminal_errno != 0) {
        finish_send_pending (record, ZLINK_SEND_TERMINAL,
                             inline_terminal_errno);
        return 0;
    }

    // Retry once after publication so a writable edge racing the direct
    // EAGAIN cannot be lost between attempt and queue insertion.
    drive_send_pending ();
    return 0;
}
