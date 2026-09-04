/* SPDX-License-Identifier: MPL-2.0 */

// Blocking SEND and internal REQUEST admission submission. Queue driving lives
// in socket_send_complete.cpp; only REQUEST transfers message ownership into
// the pending owner.

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
const zlink::routed_send_target_key_t empty_routed_send_target;

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
      zlink::socket_send_pending_runtime_t *runtime_) :
        _runtime (runtime_), _target (NULL), _epoch (0), _acquired (false)
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

class preacquired_admission_gate_guard_t
{
  public:
    preacquired_admission_gate_guard_t (std::atomic<bool> *gate_,
                                        bool active_) :
        _gate (gate_), _active (active_)
    {
    }

    ~preacquired_admission_gate_guard_t ()
    {
        if (_active)
            _gate->store (false, std::memory_order_release);
    }

    void activate () { _active = true; }
    void release ()
    {
        if (_active)
            _gate->store (false, std::memory_order_release);
        _active = false;
    }
    void handoff () { _active = false; }

  private:
    std::atomic<bool> *_gate;
    bool _active;
};

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

    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    logical_send_wait_guard_t logical_wait (&pending);
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
                admission_rc = try_admit_send_parts_scoped (
                  parts_, part_count_,
                  target ? *target : empty_routed_send_target,
                  has_routed_target, physical_scope, NULL, true, NULL, NULL,
                  false, true,
                  transient_raw_target
                    ? transient_routed_target_or_null_
                    : NULL);
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
  void *admission_observer_userdata_,
  send_pending_request_resolved_fn resolved_,
  send_pending_request_cleanup_fn cleanup_,
  send_pending_request_promote_fn promote_,
  void *resolution_context_, bool *pending_out_)
{
    if (pending_out_)
        *pending_out_ = false;

    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    if (target_rid_or_null_)
        target.peer_rid = *target_rid_or_null_;

    zlink_send_op_id_t internal_op_id = 0;
    const int rc = request_pending_submit (
      parts_, part_count_, target_rid_or_null_ ? &target : NULL,
      &internal_op_id, admission_observer_, admission_observer_userdata_,
      resolved_, cleanup_, resolution_context_, promote_);
    if (rc == 0 && pending_out_)
        *pending_out_ = internal_op_id != 0;
    return rc;
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

int zlink::socket_base_t::request_pending_submit (
  zlink_msg_t *parts_, size_t part_count_,
  const zlink_routed_submit_target_t *target_, zlink_send_op_id_t *op_id_out_,
  pipe_write_observer_fn admission_observer_,
  void *admission_observer_userdata_,
  send_pending_request_resolved_fn request_resolved_,
  send_pending_request_cleanup_fn request_cleanup_,
  void *request_resolution_context_,
  send_pending_request_promote_fn request_promote_)
{
    if (op_id_out_)
        *op_id_out_ = 0;

    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    bool admission_gate_preacquired_ = false;
    preacquired_admission_gate_guard_t preacquired_gate (
      &pending.admission_gate, admission_gate_preacquired_);

    // Admit at the common complete-record boundary.
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    socket_public_send_scope_t admission (lifecycle, true);
    if (!admission.acquired ())
        return -1;

    if (!parts_ || part_count_ == 0) {
        errno = EINVAL;
        return -1;
    }
    if (options.type != ZLINK_CORE_SOCKET_DEALER
        && options.type != ZLINK_CORE_SOCKET_ROUTER) {
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
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    routed_send_target_key_t direct_fallback_target;
    bool direct_fallback_target_valid = false;

    // A connected DEALER's normal DONTWAIT path must use the pipe selected by
    // its load balancer in this exact send scope.  Resolving that selection to
    // a peer RID / transport-pair tuple (and, for pull completion, scanning the
    // attached-pipe table back to an endpoint) before the physical write made
    // strings and table lookups part of every REQUEST.  Own the admission gate
    // first so no pending operation can overtake this attempt, then write the
    // selected pipe directly.  Only a retryable physical refusal materializes
    // the configured endpoint used by the pending retry path below.
    if (!admission_gate_preacquired_ && !target_
        && options.type == ZLINK_CORE_SOCKET_DEALER
        && pending.pending_msgs.load (std::memory_order_acquire) == 0
        && !pending.failing.load (std::memory_order_acquire)) {
        bool gate_expected = false;
        if (pending.admission_gate.compare_exchange_strong (
              gate_expected, true, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
            preacquired_gate.activate ();
            int direct_rc = -1;
            int direct_errno = 0;
            pipe_t *selected_pipe = NULL;
            if (process_submit_commands () == 0
                && xselect_routed_submit_pipe (&selected_pipe, true)
                     == 0
                && selected_pipe) {
                direct_rc = try_admit_send_parts_scoped (
                  parts_, part_count_, empty_routed_send_target, false,
                  admission, NULL, true, admission_observer_,
                  admission_observer_userdata_, true, true,
                  NULL, selected_pipe);
                direct_errno = errno;
            } else {
                direct_errno = errno;
            }

            if (direct_rc == 0) {
                clear_public_send_recovery_state ();
                consume_caller_parts (parts_, part_count_);
                preacquired_gate.release ();
                if (pending.pending_msgs.load (std::memory_order_acquire) != 0)
                    drive_request_pending ();
                errno = 0;
                return 0;
            }

            if (!retryable_logical_send_errno (direct_errno)) {
                preacquired_gate.release ();
                if (pending.pending_msgs.load (std::memory_order_acquire) != 0)
                    drive_request_pending ();
                errno = direct_errno;
                return -1;
            }

            if (selected_pipe) {
                if (xcommit_request_submit_pipe (selected_pipe) != 0) {
                    const int commit_errno = errno;
                    preacquired_gate.release ();
                    if (pending.pending_msgs.load (std::memory_order_acquire)
                        != 0)
                        drive_request_pending ();
                    errno = commit_errno;
                    return -1;
                }
                try {
                    const std::string &logical_endpoint =
                      selected_pipe->get_endpoint_pair ().identifier ();
                    if (logical_endpoint.empty ()) {
                        preacquired_gate.release ();
                        if (pending.pending_msgs.load (
                              std::memory_order_acquire)
                            != 0)
                            drive_request_pending ();
                        errno = EHOSTUNREACH;
                        return -1;
                    }
                    const blob_t &rid = selected_pipe->get_routing_id ();
                    direct_fallback_target = routed_send_target_key_t (
                      rid.data (), rid.size (), 0, 0, 0,
                      logical_endpoint);
                    direct_fallback_target_valid = true;
                } catch (...) {
                    preacquired_gate.release ();
                    if (pending.pending_msgs.load (std::memory_order_acquire)
                        != 0)
                        drive_request_pending ();
                    errno = ENOMEM;
                    return -1;
                }
                // Keep the gate through front insertion below. A racing submit
                // can publish only behind this selected operation.
                admission_gate_preacquired_ = true;
            } else {
                // Selection itself reported a retryable state. No pipe identity
                // exists to commit, so release the gate and let the established
                // disconnected/history slow path resolve it below.
                preacquired_gate.release ();
                if (pending.pending_msgs.load (std::memory_order_acquire) != 0)
                    drive_request_pending ();
            }
        }
    }

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
    std::string request_logical_endpoint;

    //  Resolve the exact target now. Deferring the choice to completion time
    //  would make per-target FIFO order impossible to state. A DEALER
    //  multipart fast attempt can arrive with the FINAL-time endpoint already
    //  committed; preserve that identity instead of selecting a second time.
    if (direct_fallback_target_valid) {
        target = std::move (direct_fallback_target);
        has_target = true;
    } else try {
        if (options.type == ZLINK_CORE_SOCKET_ROUTER
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
                    && options.type == ZLINK_CORE_SOCKET_ROUTER) {
                    if (!valid_routing_id (&resolved.peer_rid)) {
                        errno = EINVAL;
                        return -1;
                    }
                    if (process_submit_commands () != 0)
                        return -1;
                    const zlink_routing_id_t requested_rid = resolved.peer_rid;
                    if (xselect_request_submit_target (
                          &requested_rid, &resolved,
                          &resolved_connection_id,
                          &resolved_route_incarnation_id,
                          &request_logical_endpoint)
                        != 0)
                        return -1;
                }
            } else {
                if (options.type != ZLINK_CORE_SOCKET_DEALER) {
                    errno = EINVAL;
                    return -1;
                }
                if (process_submit_commands () != 0)
                    return -1;
                if (xselect_request_submit_target (
                      NULL, &resolved, &resolved_connection_id,
                      &resolved_route_incarnation_id,
                      &request_logical_endpoint)
                    != 0)
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
                || (!dealer_endpoint_target && !resolved_pair
                    && !resolved_unpaired)) {
                errno = EINVAL;
                return -1;
            }
            target = routed_send_target_key_t (
              resolved.peer_rid.data, resolved.peer_rid.size, 0, 0, 0,
              logical_endpoint);
            has_target = true;
        }
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
    bool attempt_inline = false;
    bool gate_acquired = false;
    routed_send_target_key_t inline_reservation_key;
    if (!admission_gate_preacquired_) {
        try {
            inline_reservation_key = target;
        } catch (...) {
            errno = ENOMEM;
            return -1;
        }
    }
    bool target_reserved = false;
    bool reservation_allocation_failed = false;
    // The overwhelmingly common case has no pending record. The socket-wide
    // admission gate is already the physical-send serialization point, so win
    // it before touching the per-target map. A racing submitter either wins
    // this CAS (and therefore linearizes first) or publishes behind this
    // attempt while the gate remains owned. On EAGAIN this attempt is inserted
    // at the front before the gate is released, preserving target FIFO.
    if (admission_gate_preacquired_) {
        gate_acquired = true;
        attempt_inline = true;
        preacquired_gate.handoff ();
    } else if (pending.pending_msgs.load (std::memory_order_acquire) == 0
        && !pending.failing.load (std::memory_order_acquire)) {
        bool gate_expected = false;
        gate_acquired = pending.admission_gate.compare_exchange_strong (
          gate_expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire);
        attempt_inline = gate_acquired;
    }
    if (!gate_acquired) {
        scoped_lock_t lock (pending.sync);
        if (pending.failing.load (std::memory_order_acquire)) {
            errno = ESHUTDOWN;
            return -1;
        }
        const std::map<routed_send_target_key_t,
                       std::deque<send_pending_record_t *> >::const_iterator
          queue = pending.queues.find (target);
        const bool target_queue_empty =
          queue == pending.queues.end () || queue->second.empty ();
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
    if (!gate_acquired && target_reserved) {
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
    // Preserve direct admission when no request is ahead of this target.
    // Immediate admission creates no pending queue record.
    std::optional<send_pending_record_t> inline_record;
    int inline_terminal_errno = 0;
    bool inline_record_owns_copies = false;
    int inline_errno = EAGAIN;
    if (attempt_inline) {
        // The request API consumes every input on every result and can use
        // caller storage for the direct attempt. Do not construct or copy a
        // pending record until the physical attempt actually reports EAGAIN.
            zlink_msg_t *const attempt_parts = parts_;
            const size_t attempt_part_count = part_count_;
            socket_public_send_scope_t physical_admission (
              lifecycle, true, socket_send_admission_complete);
            int inline_rc = -1;
            if (physical_admission.acquired ()) {
                inline_rc = try_admit_send_parts_scoped (
                  attempt_parts, attempt_part_count, target, has_target,
                  physical_admission, NULL, false, admission_observer_,
                  admission_observer_userdata_, true);
            } else {
                // An open incremental sequence is physical admission
                // contention, not an invalid async submission. Preserve the
                // record and retry after the multipart marker is released.
                errno = EAGAIN;
            }
            inline_errno = errno;
            physical_admission.unlock_sync ();
            if (inline_rc == 0) {
                clear_public_send_recovery_state ();
                pending.admission_gate.store (false,
                                              std::memory_order_release);
                if (target_reserved) {
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
                    drive_request_pending ();
                }
                return 0;
            }
            if (inline_errno != EAGAIN) {
                pending.admission_gate.store (false,
                                              std::memory_order_release);
                if (target_reserved) {
                    scoped_lock_t lock (pending.sync);
                    release_inline_attempt_locked (
                      inline_reservation_key);
                }
                if (pending.pending_msgs.load (std::memory_order_acquire) != 0)
                    drive_request_pending ();
                errno = inline_errno;
                return -1;
            }
            try {
                inline_record.emplace ();
                inline_record->target = target;
            } catch (...) {
                pending.admission_gate.store (false,
                                              std::memory_order_release);
                if (target_reserved) {
                    scoped_lock_t lock (pending.sync);
                    release_inline_attempt_locked (
                      inline_reservation_key);
                }
                drive_request_pending ();
                errno = ENOMEM;
                return -1;
            }
            inline_record->has_target = has_target;
            if (copy_send_parts (parts_, part_count_, &inline_record->parts)
                     != 0) {
                pending.admission_gate.store (false,
                                              std::memory_order_release);
                if (target_reserved) {
                    scoped_lock_t lock (pending.sync);
                    release_inline_attempt_locked (
                      inline_reservation_key);
                }
                if (pending.pending_msgs.load (std::memory_order_acquire) != 0)
                    drive_request_pending ();
                errno = ENOMEM;
                return -1;
            }
            inline_record_owns_copies = true;

    }

    const uint64_t charge = record_charge_bytes (parts_, part_count_);
    int pending_reject_errno = 0;
    send_pending_record_t *record = new (std::nothrow) send_pending_record_t ();
    bool record_owns_copies = false;
    bool request_context_promoted = false;
    zlink_send_op_id_t op_id = 0;
    if (!record)
        pending_reject_errno = ENOMEM;
    void *record_observer_userdata = admission_observer_userdata_;
    void *record_resolution_context = request_resolution_context_;
    if (record && request_promote_) {
        record_observer_userdata = NULL;
        record_resolution_context = NULL;
        if (request_promote_ (
              request_resolution_context_, &record_observer_userdata,
              &record_resolution_context)
            != 0) {
            pending_reject_errno = errno != 0 ? errno : ENOMEM;
        } else {
            request_context_promoted = true;
        }
    }
    if (record && pending_reject_errno == 0) {
        record->admission_observer = admission_observer_;
        record->admission_observer_userdata = record_observer_userdata;
        record->request_resolution_context = record_resolution_context;
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
        if (inline_terminal_errno == 0
            && pending.failing.load (std::memory_order_acquire)) {
            pending_reject_errno = ESHUTDOWN;
        } else {
            // STREAM detach can retire the exact target after the current-pipe
            // attempt returned EAGAIN but before this record is published.
            // The fail sweep cannot see an unpublished record, so preserve the
            // accepted-operation contract by publishing it terminal here.
            if (inline_terminal_errno == 0 && target_reserved) {
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
            // Pending limits remain the REQUEST overload policy. Ordinary
            // DONTWAIT SEND never reaches this queue.
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
                    //  request that cannot be correlated.
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
        if (target_reserved) {
            (void) release_inline_attempt_locked (inline_reservation_key);
        }
    }
    if (target_publication_lock.owns_lock ())
        target_publication_lock.unlock ();

    if (attempt_inline) {
        if (target_reserved) {
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
            if (request_context_promoted && record->request_cleanup
                && record->request_resolution_context) {
                record->request_cleanup (
                  record->request_resolution_context);
                record->request_resolution_context = NULL;
            }
            delete record;
            record = NULL;
        }
        drive_request_pending ();
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

    if (inline_terminal_errno != 0) {
        finish_request_pending (record, false, inline_terminal_errno);
        return 0;
    }

    // Retry once after publication so a writable edge racing the direct
    // EAGAIN cannot be lost between attempt and queue insertion.
    drive_request_pending ();
    return 0;
}
