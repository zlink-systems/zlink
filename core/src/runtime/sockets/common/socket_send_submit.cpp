/* SPDX-License-Identifier: MPL-2.0 */

// SEND and REQUEST admission submission.

#include "utils/precompiled.hpp"

#include "core/c_api_copy_internal.hpp"

#include "core/pipe.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"
#include "utils/routing_id.hpp"

#include <optional>

namespace
{
const zlink::routed_send_target_key_t empty_routed_send_target;

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

class blocking_send_wait_guard_t
{
  public:
    blocking_send_wait_guard_t (
      zlink::socket_blocking_send_runtime_t *runtime_) :
        _runtime (runtime_), _target (NULL), _epoch (0), _acquired (false)
    {
    }

    ~blocking_send_wait_guard_t ()
    {
        if (!_acquired)
            return;
        const int saved_errno = errno;
        zlink::scoped_lock_t lock (_runtime->sync);
        std::map<zlink::routed_send_target_key_t,
                 zlink::blocking_send_wait_state_t>::iterator it =
          _runtime->logical_waits.find (*_target);
        zlink_assert (it != _runtime->logical_waits.end ());
        zlink_assert (it->second.waiters != 0);
        if (--it->second.waiters == 0)
            _runtime->logical_waits.erase (it);
        errno = saved_errno;
    }

    void bind_target (const zlink::routed_send_target_key_t *target_)
    {
        zlink_assert (!_acquired);
        _target = target_;
    }

    bool acquire ()
    {
        if (!_target) {
            errno = EFAULT;
            return false;
        }
        zlink::scoped_lock_t lock (_runtime->sync);
        try {
            const std::pair<
              std::map<zlink::routed_send_target_key_t,
                       zlink::blocking_send_wait_state_t>::iterator,
              bool>
              inserted = _runtime->logical_waits.insert (
                std::make_pair (*_target,
                                zlink::blocking_send_wait_state_t ()));
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
                       zlink::blocking_send_wait_state_t>::const_iterator it =
          _runtime->logical_waits.find (*_target);
        if (it == _runtime->logical_waits.end ()
            || it->second.epoch == _epoch)
            return 0;
        return it->second.terminal_errno != 0
                 ? it->second.terminal_errno
                 : EIO;
    }

  private:
    zlink::socket_blocking_send_runtime_t *_runtime;
    const zlink::routed_send_target_key_t *_target;
    uint64_t _epoch;
    bool _acquired;
};

bool retryable_logical_send_errno (int err_)
{
    return err_ == EAGAIN || err_ == ENOTCONN || err_ == EHOSTUNREACH;
}

}

struct zlink::socket_base_t::submit_timeout_budget_t
{
    submit_timeout_budget_t (clock_t *clock_, int timeout_ms_) :
        clock (clock_), timeout_ms (timeout_ms_),
        deadline_ms (timeout_ms_ < 0
                       ? 0
                       : clock_->now_ms ()
                           + static_cast<uint64_t> (timeout_ms_))
    {
    }

    int refresh_timeout ()
    {
        if (timeout_ms <= 0)
            return timeout_ms;
        const uint64_t now = clock->now_ms ();
        if (now >= deadline_ms) {
            timeout_ms = 0;
            return timeout_ms;
        }
        const uint64_t remaining = deadline_ms - now;
        timeout_ms = remaining > static_cast<uint64_t> (INT_MAX)
                       ? INT_MAX
                       : static_cast<int> (remaining);
        return timeout_ms;
    }

    clock_t *clock;
    int timeout_ms;
    uint64_t deadline_ms;
};

struct zlink::socket_base_t::completion_submit_wait_context_t
{
    explicit completion_submit_wait_context_t (socket_base_t *socket_) :
        socket (socket_),
        timeout (&socket_->_clock, socket_->options.sndtimeo),
        progress_owner (socket_)
    {
    }

    int remaining_timeout () { return timeout.refresh_timeout (); }

    int wait_for_progress (uint64_t observed_progress_)
    {
        const int remaining = remaining_timeout ();
        if (remaining == 0) {
            errno = EAGAIN;
            return -1;
        }

        if (socket->wait_submit_progress (
              observed_progress_, remaining,
              progress_owner.held_state ())
            != 0) {
            if (socket->lifecycle_coordinator ().public_close_requested ())
                errno = ESHUTDOWN;
            return -1;
        }
        if (socket->lifecycle_coordinator ().public_close_requested ()) {
            errno = ESHUTDOWN;
            return -1;
        }
        return 0;
    }

    socket_base_t *socket;
    submit_timeout_budget_t timeout;
    transport_pair_owner_progress_scope_t progress_owner;
};

struct zlink::socket_base_t::request_submit_selection_t
{
    request_submit_selection_t () :
        transport_connection_id (0), route_incarnation_id (0)
    {
        memset (&resolved, 0, sizeof (resolved));
    }

    zlink_routed_submit_target_t resolved;
    uint64_t transport_connection_id;
    uint64_t route_incarnation_id;
    std::optional<std::string> logical_endpoint;
};

int zlink::socket_base_t::try_send_parts_scoped_once (
  zlink_msg_t *parts_, size_t part_count_,
  const zlink_routing_id_t *target_rid_or_null_,
  socket_public_send_scope_t &send_scope_)
{
    if (!parts_ || part_count_ < 2 || !send_scope_.acquired ()) {
        errno = EINVAL;
        return -1;
    }
    if (target_rid_or_null_) {
        if (options.type != ZLINK_CORE_SOCKET_ROUTER) {
            errno = ENOTSUP;
            return -1;
        }
    } else if (options.type != ZLINK_CORE_SOCKET_PAIR
               && options.type != ZLINK_CORE_SOCKET_DEALER) {
        errno = ENOTSUP;
        return -1;
    }

    const int rc = try_admit_send_parts_scoped (
      parts_, part_count_, empty_routed_send_target, false, send_scope_, NULL,
      false, NULL, NULL, false, false, target_rid_or_null_);
    return rc;
}

zlink::socket_base_t::dealer_completion_fast_result_t
zlink::socket_base_t::try_dealer_completion_submit_fast (
  zlink_msg_t *parts_, size_t part_count_,
  std::optional<routed_send_target_key_t> *target_out_)
{
    socket_public_send_scope_t fast_scope (
      lifecycle_coordinator (), true, socket_send_admission_complete);
    if (!fast_scope.acquired ())
        return dealer_completion_fast_failed;
    if (process_submit_commands () != 0)
        return dealer_completion_fast_failed;

    pipe_t *selected = NULL;
    if (xselect_routed_submit_pipe (&selected, false) != 0 || !selected) {
        fast_scope.unlock_sync ();
        return dealer_completion_fast_select_required;
    }
    if (try_admit_send_parts_scoped (
          parts_, part_count_, empty_routed_send_target, false, fast_scope,
          NULL, true, NULL, NULL, false, true, NULL, selected)
        == 0)
        return dealer_completion_fast_admitted;

    const int fast_errno = errno;
    if (!retryable_logical_send_errno (fast_errno)) {
        errno = fast_errno;
        return dealer_completion_fast_failed;
    }

    bool target_committed = false;
    try {
        const std::string logical_endpoint =
          selected->get_endpoint_pair ().identifier ();
        const blob_t &rid = selected->get_routing_id ();
        target_out_->emplace (rid.data (), rid.size (), 0, 0, 0,
                              logical_endpoint);
        target_committed = !logical_endpoint.empty ();
    } catch (...) {
        errno = ENOMEM;
        return dealer_completion_fast_failed;
    }

    // The selected pipe must remain pinned through target materialization;
    // only the subsequent endpoint wait runs without the lifecycle sync.
    fast_scope.unlock_sync ();
    return target_committed ? dealer_completion_fast_target_committed
                            : dealer_completion_fast_select_required;
}

int zlink::socket_base_t::wait_for_dealer_completion_submit_target (
  completion_submit_wait_context_t &wait_,
  std::optional<routed_send_target_key_t> *target_out_)
{
    // Selection is committed once, at FINAL. A never-handshaken endpoint may
    // become eligible during the same entry budget; after selection, retries
    // use only its configured endpoint.
    while (true) {
        const uint64_t observed_progress = observe_submit_progress ();
        zlink_routed_submit_target_t resolved;
        memset (&resolved, 0, sizeof (resolved));
        std::optional<std::string> logical_endpoint;
        int select_errno = 0;
        {
            socket_public_send_scope_t selection_scope (
              lifecycle_coordinator (), true,
              socket_send_admission_complete);
            if (!selection_scope.acquired ())
                return -1;
            if (process_submit_commands () != 0)
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
                        logical_endpoint.emplace (
                          pipe->get_endpoint_pair ().identifier ());
                    } catch (...) {
                        errno = ENOMEM;
                        return -1;
                    }
                    break;
                }
                if (!logical_endpoint || logical_endpoint->empty ())
                    select_errno = EHOSTUNREACH;
            } else {
                select_errno = errno;
            }
        }

        if (logical_endpoint && !logical_endpoint->empty ()) {
            try {
                target_out_->emplace (
                  resolved.peer_rid.data, resolved.peer_rid.size, 0, 0, 0,
                  *logical_endpoint);
            } catch (...) {
                errno = ENOMEM;
                return -1;
            }
            return 0;
        }
        if (!retryable_logical_send_errno (select_errno)) {
            errno = select_errno;
            return -1;
        }
        if (wait_.wait_for_progress (observed_progress) != 0)
            return -1;
    }
}

int zlink::socket_base_t::wait_for_completion_submit_admission (
  zlink_msg_t *parts_, size_t part_count_,
  const routed_send_target_key_t *committed_dealer_target_or_null_,
  const zlink_routing_id_t *transient_routed_target_or_null_,
  completion_submit_wait_context_t &wait_)
{
    const bool has_routed_target = transient_routed_target_or_null_ != NULL;
    bool transient_raw_target = has_routed_target;
    std::optional<routed_send_target_key_t> materialized_routed_target;
    const routed_send_target_key_t *target =
      committed_dealer_target_or_null_;

    socket_blocking_send_runtime_t &wait_runtime = blocking_send_runtime ();
    blocking_send_wait_guard_t logical_wait (&wait_runtime);
    if (target)
        logical_wait.bind_target (target);
    else if (!has_routed_target)
        logical_wait.bind_target (&empty_routed_send_target);
    bool logical_wait_registered = false;

    const auto normalize_wait_failure = [&] (int err_) -> int {
        return err_ == ECANCELED
                   && lifecycle_coordinator ().public_close_requested ()
                 ? ESHUTDOWN
                 : err_;
    };

    while (true) {
        const uint64_t observed_progress = observe_submit_progress ();
        int explicit_failure =
          normalize_wait_failure (logical_wait.failure_errno ());
        if (explicit_failure != 0) {
            errno = explicit_failure;
            return -1;
        }

        int admission_errno = 0;
        int admission_rc = -1;
        bool retry_committed_raw_target = false;
        {
            socket_public_send_scope_t physical_scope (
              lifecycle_coordinator (), true,
              socket_send_admission_complete);
            if (!physical_scope.acquired ())
                return -1;
            if (process_submit_commands () == 0) {
                // The registered wait already owns this submission's first
                // admission; a wake must not add another metric attempt.
                admission_rc = try_admit_send_parts_scoped (
                  parts_, part_count_,
                  target ? *target : empty_routed_send_target,
                  has_routed_target, physical_scope, NULL, true, NULL, NULL,
                  false, true,
                  transient_raw_target
                    ? transient_routed_target_or_null_
                    : NULL,
                  NULL, !logical_wait_registered);
            }
            admission_errno = errno;

            if (transient_raw_target && admission_rc != 0
                && retryable_logical_send_errno (admission_errno)) {
                try {
                    materialized_routed_target.emplace (
                      transient_routed_target_or_null_->data,
                      transient_routed_target_or_null_->size, 0, 0, 0);
                    target = &*materialized_routed_target;
                    transient_raw_target = false;
                    logical_wait.bind_target (target);
                    retry_committed_raw_target =
                      admission_errno == ENOTCONN
                      || admission_errno == EHOSTUNREACH;
                } catch (...) {
                    admission_errno = ENOMEM;
                }
            }

            // Register before releasing lifecycle sync so detach/removal
            // cannot publish a terminal handoff between refusal and waiter.
            if (admission_rc != 0
                && retryable_logical_send_errno (admission_errno)
                && !logical_wait_registered) {
                if (logical_wait.acquire ()) {
                    logical_wait_registered = true;
                } else {
                    admission_errno = errno;
                }
            }
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

        // A transient connection failure can leave this just-committed route
        // writable without a new progress edge. Revalidate it exactly once;
        // HWM EAGAIN always waits for real credit.
        if (retry_committed_raw_target) {
            if (wait_.remaining_timeout () == 0) {
                errno = EAGAIN;
                return -1;
            }
            continue;
        }
        if (wait_.wait_for_progress (observed_progress) != 0)
            return -1;
    }
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
    completion_submit_wait_context_t wait (this);
    std::optional<routed_send_target_key_t> target;

    if (type == ZLINK_CORE_SOCKET_DEALER) {
        const dealer_completion_fast_result_t fast_result =
          try_dealer_completion_submit_fast (parts_, part_count_, &target);
        if (fast_result == dealer_completion_fast_failed)
            return -1;
        if (fast_result == dealer_completion_fast_admitted)
            return 0;
        if (fast_result == dealer_completion_fast_select_required
            && wait_for_dealer_completion_submit_target (wait, &target) != 0)
            return -1;
        zlink_assert (target);
        return wait_for_completion_submit_admission (
          parts_, part_count_, &*target, NULL, wait);
    }

    const zlink_routing_id_t *const transient_routed_target =
      type == ZLINK_CORE_SOCKET_ROUTER || type == ZLINK_CORE_SOCKET_STREAM
        ? target_rid_or_null_
        : NULL;
    return wait_for_completion_submit_admission (
      parts_, part_count_, NULL, transient_routed_target, wait);
}

int zlink::socket_base_t::request_admission_submit (
  zlink_msg_t *parts_, size_t part_count_,
  const zlink_routing_id_t *target_rid_or_null_,
  pipe_write_observer_fn admission_observer_,
  void *admission_observer_userdata_)
{
    if (!parts_ || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    request_submit_selection_t selection;
    const request_admission_fast_result_t result =
      try_request_admission_submit_fast (
        parts_, part_count_, target_rid_or_null_, admission_observer_,
        admission_observer_userdata_, &selection);
    if (result == request_admission_fast_admitted)
        return 0;
    if (result == request_admission_fast_failed)
        return -1;

    // DONTWAIT performs exactly one physical admission attempt. An unready
    // DEALER or a known ROUTER target shares SEND's payload-free WRITABLE wait
    // contract at the public API boundary.
    errno = EAGAIN;
    return -1;
}

zlink::socket_base_t::request_admission_fast_result_t
zlink::socket_base_t::try_request_admission_submit_fast (
  zlink_msg_t *parts_, size_t part_count_,
  const zlink_routing_id_t *target_rid_or_null_,
  pipe_write_observer_fn admission_observer_,
  void *admission_observer_userdata_,
  request_submit_selection_t *selection_out_)
{
    if (options.type == ZLINK_CORE_SOCKET_DEALER
        && !target_rid_or_null_) {
        socket_public_send_scope_t fast_scope (
          lifecycle_coordinator (), true, socket_send_admission_complete);
        if (!fast_scope.acquired ())
            return request_admission_fast_failed;
        if (process_submit_commands () != 0)
            return request_admission_fast_failed;

        pipe_t *selected = NULL;
        if (xselect_routed_submit_pipe (&selected, true) != 0 || !selected) {
            fast_scope.unlock_sync ();
            return request_admission_fast_select_required;
        }
        const int rc = try_admit_send_parts_scoped (
          parts_, part_count_, empty_routed_send_target, false, fast_scope,
          NULL, true, admission_observer_, admission_observer_userdata_,
          true, true, NULL, selected);
        if (rc == 0) {
            // Multipart attempts use shallow retry copies; the single-part
            // attempt already moved its caller-owned input directly.
            if (part_count_ > 1)
                consume_caller_parts (parts_, part_count_);
            errno = 0;
            return request_admission_fast_admitted;
        }

        const int fast_errno = errno;
        if (fast_errno != EAGAIN) {
            errno = fast_errno;
            return request_admission_fast_failed;
        }
        if (xcommit_request_submit_pipe (selected) != 0)
            return request_admission_fast_failed;
        try {
            selection_out_->logical_endpoint.emplace (
              selected->get_endpoint_pair ().identifier ());
            const blob_t &rid = selected->get_routing_id ();
            copy_routing_id_from_bytes (
              rid.data (), rid.size (), &selection_out_->resolved.peer_rid);
        } catch (...) {
            errno = ENOMEM;
            return request_admission_fast_failed;
        }

        const bool target_selected =
          selection_out_->logical_endpoint
          && !selection_out_->logical_endpoint->empty ();
        fast_scope.unlock_sync ();
        return target_selected ? request_admission_fast_target_selected
                               : request_admission_fast_select_required;
    }

    if (options.type == ZLINK_CORE_SOCKET_ROUTER
        && target_rid_or_null_) {
        socket_public_send_scope_t fast_scope (
          lifecycle_coordinator (), true, socket_send_admission_complete);
        if (!fast_scope.acquired ())
            return request_admission_fast_failed;
        if (process_submit_commands () != 0)
            return request_admission_fast_failed;
        const int rc = try_admit_send_parts_scoped (
          parts_, part_count_, empty_routed_send_target, true, fast_scope,
          NULL, true, admission_observer_, admission_observer_userdata_, true,
          true, target_rid_or_null_);
        if (rc == 0) {
            if (part_count_ > 1)
                consume_caller_parts (parts_, part_count_);
            errno = 0;
            return request_admission_fast_admitted;
        }

        const int fast_errno = errno;
        if (fast_errno != EAGAIN) {
            errno = fast_errno;
            return request_admission_fast_failed;
        }
        selection_out_->resolved.peer_rid = *target_rid_or_null_;
        fast_scope.unlock_sync ();
        return request_admission_fast_target_selected;
    }

    return request_admission_fast_select_required;
}

int zlink::socket_base_t::prepare_request_submit_target (
  const zlink_routing_id_t *target_rid_or_null_,
  request_admission_fast_result_t fast_result_,
  submit_timeout_budget_t &timeout_,
  request_submit_selection_t *selection_,
  routed_send_target_key_t *target_out_)
{
    bool target_selected =
      fast_result_ == request_admission_fast_target_selected;
    while (!target_selected) {
        if (options.type == ZLINK_CORE_SOCKET_DEALER
            && !selection_->logical_endpoint)
            selection_->logical_endpoint.emplace ();
        int selection_errno = 0;
        {
            socket_public_send_scope_t selection_scope (
              lifecycle_coordinator (), true,
              socket_send_admission_complete);
            if (!selection_scope.acquired ())
                return -1;
            if (process_submit_commands () != 0)
                return -1;
            if (xselect_request_submit_target (
                  target_rid_or_null_, &selection_->resolved,
                  &selection_->transport_connection_id,
                  &selection_->route_incarnation_id,
                  selection_->logical_endpoint
                    ? &*selection_->logical_endpoint
                    : NULL)
                == 0) {
                target_selected = true;
            } else {
                selection_errno = errno;
            }
        }

        if (target_selected)
            break;
        const bool dealer_wait =
          options.type == ZLINK_CORE_SOCKET_DEALER
          && (selection_errno == ENOTCONN
              || selection_errno == ECONNREFUSED);
        if (!dealer_wait || timeout_.timeout_ms == 0) {
            errno = selection_errno;
            return -1;
        }
        if (timeout_.refresh_timeout () == 0) {
            errno = selection_errno;
            return -1;
        }
        if (process_commands (timeout_.timeout_ms, false) != 0)
            return -1;
        (void) timeout_.refresh_timeout ();
    }

    try {
        const bool configured_endpoint_target =
          selection_->logical_endpoint
          && !selection_->logical_endpoint->empty ();
        *target_out_ = routed_send_target_key_t (
          selection_->resolved.peer_rid.data,
          selection_->resolved.peer_rid.size,
          configured_endpoint_target
            ? 0
            : selection_->resolved.transport_pair_id,
          configured_endpoint_target
            ? 0
            : selection_->resolved.transport_pair_generation,
          configured_endpoint_target ? 0 : selection_->route_incarnation_id,
          selection_->logical_endpoint
            ? *selection_->logical_endpoint
            : empty_routed_send_target.logical_endpoint);
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int zlink::socket_base_t::wait_for_request_submit_admission (
  zlink_msg_t *parts_, size_t part_count_,
  const routed_send_target_key_t &target_,
  pipe_write_observer_fn admission_observer_,
  void *admission_observer_userdata_,
  submit_timeout_budget_t &timeout_)
{
    while (true) {
        socket_public_send_scope_t physical_scope (
          lifecycle_coordinator (), true, socket_send_admission_complete);
        if (!physical_scope.acquired ())
            return -1;
        const int rc = try_admit_send_parts_scoped (
          parts_, part_count_, target_, true, physical_scope, NULL, false,
          admission_observer_, admission_observer_userdata_, true);
        const int saved_errno = errno;
        physical_scope.unlock_sync ();
        if (rc == 0) {
            if (part_count_ > 1)
                consume_caller_parts (parts_, part_count_);
            errno = 0;
            return 0;
        }
        if (saved_errno != EAGAIN || timeout_.timeout_ms == 0) {
            errno = saved_errno;
            return -1;
        }

        // A registered completion poller is the sole drain owner. Borrow it
        // after backpressure so a full reply lane cannot block request credit.
        if (_completion_poller_refs.load (std::memory_order_acquire) != 0) {
            scoped_lock_t owner_lock (_completion_owner_sync);
            if (_completion_poller_refs.load (std::memory_order_acquire) != 0) {
                const completion_drain_scope_t drain_scope (this);
                acknowledge_request_completion_notification ();
                process_ready_completion_pipes ();
                (void) drain_request_completions ();
            }
        }

        if (timeout_.refresh_timeout () == 0) {
            errno = EAGAIN;
            return -1;
        }
        if (process_commands (timeout_.timeout_ms, false) != 0)
            return -1;
        if (timeout_.timeout_ms > 0
            && timeout_.refresh_timeout () == 0) {
            errno = EAGAIN;
            return -1;
        }
    }
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

    submit_timeout_budget_t timeout (&_clock, options.sndtimeo);
    request_submit_selection_t selection;
    const request_admission_fast_result_t fast_result =
      try_request_admission_submit_fast (
        parts_, part_count_, target_rid_or_null_, admission_observer_,
        admission_observer_userdata_, &selection);
    if (fast_result == request_admission_fast_failed)
        return -1;
    if (fast_result == request_admission_fast_admitted)
        return 0;

    routed_send_target_key_t target;
    if (prepare_request_submit_target (
          target_rid_or_null_, fast_result, timeout, &selection, &target)
        != 0)
        return -1;
    return wait_for_request_submit_admission (
      parts_, part_count_, target, admission_observer_,
      admission_observer_userdata_, timeout);
}
