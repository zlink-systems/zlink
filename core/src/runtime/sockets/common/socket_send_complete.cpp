/* SPDX-License-Identifier: MPL-2.0 */

// Core-owned pull-completion SEND and internal REQUEST admission.
//
//  Async retries use the blocking send's frame admission rules, with the
//  socket mailbox owner driving retries and completion replacing the caller
//  wake. PAIR may publish through a success-only whole-record shortcut; every
//  refusal falls back to the frame path that owns errno, wait and rollback.

#include "utils/precompiled.hpp"

#include "core/c_api_copy_internal.hpp"
#include "core/mailbox.hpp"
#include "sockets/common/socket_base.hpp"
#include "sockets/common/socket_submit_retry_fault_injection.hpp"
#include "utils/err.hpp"
#include "utils/likely.hpp"
#include "utils/macros.hpp"
#include "utils/routing_id.hpp"

#include <algorithm>

namespace
{
void signal_logical_wait_state (
  zlink::send_logical_wait_state_t *state_, int terminal_errno_)
{
    zlink_assert (state_);
    ++state_->epoch;
    if (state_->epoch == 0)
        ++state_->epoch;
    state_->terminal_errno = terminal_errno_ != 0 ? terminal_errno_ : EIO;
}

bool retryable_pull_target_errno (int err_)
{
    // A pull-completion record is keyed by its logical destination.  Losing
    // the currently selected physical pipe is therefore a wait condition,
    // exactly like HWM pressure, until an explicit logical-target removal or
    // another permanent cause completes it.
    return err_ == EAGAIN || err_ == ENOTCONN || err_ == EHOSTUNREACH;
}

bool contains_routed_send_target (
  const std::vector<zlink::routed_send_target_key_t> &targets_,
  size_t target_count_, const zlink::routed_send_target_key_t &target_)
{
    const std::vector<zlink::routed_send_target_key_t>::const_iterator end =
      targets_.begin () + target_count_;
    const std::vector<zlink::routed_send_target_key_t>::const_iterator found =
      std::lower_bound (targets_.begin (), end, target_);
    return found != end && !(target_ < *found);
}

void append_routed_send_target (
  std::vector<zlink::routed_send_target_key_t> *targets_,
  size_t *target_count_, const zlink::routed_send_target_key_t &target_)
{
    const size_t insert_at = static_cast<size_t> (
      std::lower_bound (targets_->begin (),
                        targets_->begin () + *target_count_, target_)
      - targets_->begin ());
    if (insert_at != *target_count_
        && !(target_ < (*targets_)[insert_at]))
        return;
    if (*target_count_ == targets_->size ())
        targets_->push_back (target_);
    else
        (*targets_)[*target_count_] = target_;
    std::rotate (targets_->begin () + insert_at,
                 targets_->begin () + *target_count_,
                 targets_->begin () + *target_count_ + 1);
    ++*target_count_;
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

bool zlink::socket_type_supports_send_completion (int type_)
{
    return type_ == ZLINK_CORE_SOCKET_PAIR || type_ == ZLINK_CORE_SOCKET_DEALER
           || type_ == ZLINK_CORE_SOCKET_ROUTER
           || type_ == ZLINK_CORE_SOCKET_STREAM;
}

void zlink::socket_base_t::close_send_parts (
  std::vector<zlink_msg_t> *parts_)
{
    const int saved_errno = errno;
    close_send_part_array (parts_->data (), parts_->size ());
    parts_->clear ();
    errno = saved_errno;
}

int zlink::socket_base_t::copy_send_parts (
  zlink_msg_t *parts_, size_t part_count_, std::vector<zlink_msg_t> *copies_)
{
    try {
        copies_->resize (part_count_);
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }

    if (copy_send_part_array (parts_, part_count_, copies_->data ()) == 0)
        return 0;

    const int saved_errno = errno;
    copies_->clear ();
    errno = saved_errno;
    return -1;
}

bool zlink::socket_base_t::has_send_pending () const
{
    const socket_send_pending_runtime_t &pending = send_pending_runtime ();
    scoped_lock_t lock (pending.sync);
    return !pending.by_op.empty ();
}

void zlink::socket_base_t::destroy_send_pending_record (
  send_pending_record_t *record_)
{
    if (!record_)
        return;
    if (record_->completion_reservation) {
        socket_completion::release (&completion_runtime (),
                                    record_->completion_reservation);
        record_->completion_reservation = NULL;
    }
    // Close is correct after admission too because the pipe retains its own
    // frame-content reference.
    close_send_parts (&record_->parts);
    if (record_->request_cleanup && record_->request_resolution_context)
        record_->request_cleanup (record_->request_resolution_context);
    record_->request_resolution_context = NULL;
    delete record_;
}

// Remove a resolved record and publish to the pull queue or the internal
// REQUEST admission resolver.
void zlink::socket_base_t::finish_send_pending (
  send_pending_record_t *record_, zlink_send_complete_result_t result_,
  int terminal_errno_)
{
    if (!record_)
        return;

    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    socket_completion::reservation_t *pull_reservation = NULL;
    bool pull_completion = false;
    bool request_admission = false;
    send_pending_request_resolved_fn request_resolved = NULL;
    void *request_resolution_context = NULL;
    bool pending_became_empty = false;
    {
        scoped_lock_t lock (pending.sync);
        std::map<routed_send_target_key_t,
                 std::deque<send_pending_record_t *> >::iterator queue =
          pending.queues.find (record_->target);
        if (queue != pending.queues.end ()) {
            for (std::deque<send_pending_record_t *>::iterator it =
                   queue->second.begin ();
                 it != queue->second.end (); ++it) {
                if (*it == record_) {
                    queue->second.erase (it);
                    break;
                }
            }
            if (queue->second.empty ())
                pending.queues.erase (queue);
        }
        pending.by_op.erase (record_->op_id);
        const uint64_t pending_count =
          pending.pending_msgs.load (std::memory_order_relaxed);
        pending.pending_msgs.store (pending_count > 0 ? pending_count - 1 : 0,
                                    std::memory_order_release);
        pending_became_empty = pending_count == 1;
        pending.pending_bytes = pending.pending_bytes > record_->charge_bytes
                                  ? pending.pending_bytes
                                      - record_->charge_bytes
                                  : 0;

        pull_completion = record_->pull_completion;
        request_admission = record_->request_admission;
        request_resolved = record_->request_resolved;
        request_resolution_context = record_->request_resolution_context;
        pull_reservation = record_->completion_reservation;
        record_->completion_reservation = NULL;
    }
    if (pending_became_empty
        && pending.completion_capacity_blocked.load (
          std::memory_order_acquire)
        && transport_has_out ()) {
        dispatch_runtime ().mark_send_recovery_ready ();
        static_cast<mailbox_t *> (_mailbox)->signal ();
    }
    if (request_admission) {
        if (request_resolved)
            request_resolved (request_resolution_context,
                              result_ == ZLINK_SEND_ADMITTED,
                              result_ == ZLINK_SEND_ADMITTED
                                ? 0
                                : terminal_errno_);
        destroy_send_pending_record (record_);
    } else {
        zlink_assert (pull_completion);
        if (socket_completion::publish_send (
              &completion_runtime (), pull_reservation, result_,
              result_ == ZLINK_SEND_ADMITTED ? 0 : terminal_errno_)
            == 0)
            notify_request_completion ();
        else
            socket_completion::release (&completion_runtime (),
                                        pull_reservation);
        destroy_send_pending_record (record_);
    }
}

void zlink::socket_base_t::mark_send_completion_capacity_blocked ()
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    pending.completion_capacity_blocked.store (true,
                                               std::memory_order_release);
    dispatch_runtime ().mark_send_recovery_pending ();
    // A completion consumer can release the first slot after reserve() reports
    // EAGAIN but before this recovery marker is fully armed.  Publish the
    // marker first and then re-sample the authoritative queue count: either the
    // consumer observes completion_capacity_blocked and notifies us, or this
    // check observes its released slot.  Without the re-sample, the consumer's
    // ready edge can be overwritten by mark_send_recovery_pending() above.
    const bool completion_capacity_available =
      socket_completion::outstanding (&completion_runtime ())
      < socket_completion::max_outstanding_completions;
    if (completion_capacity_available
        || (pending.pending_msgs.load (std::memory_order_acquire) == 0
            && transport_has_out ())) {
        dispatch_runtime ().mark_send_recovery_ready ();
        static_cast<mailbox_t *> (_mailbox)->signal ();
    }
}

void zlink::socket_base_t::clear_public_send_recovery_state ()
{
    send_pending_runtime ().completion_capacity_blocked.store (
      false, std::memory_order_release);
    dispatch_runtime ().clear_send_recovery_pending ();
}

void zlink::socket_base_t::notify_send_completion_capacity_available ()
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    if (!pending.completion_capacity_blocked.exchange (
          false, std::memory_order_acq_rel))
        return;
    dispatch_runtime ().mark_send_recovery_pending ();
    dispatch_runtime ().mark_send_recovery_ready ();
    static_cast<mailbox_t *> (_mailbox)->signal ();
}

//  Physically submit one record. Returns 0 on admission, -1 with errno set
//  otherwise; EAGAIN means "not now, keep the record pending".
int zlink::socket_base_t::try_admit_send_pending (
  send_pending_record_t *record_)
{
    if (!record_ || record_->parts.empty ()) {
        errno = EFAULT;
        return -1;
    }

    // The pending driver is entered only after its owner has handled (or
    // deliberately handed off) command progress. Polling again while the
    // physical complete-record scope owns the public sync bit can recursively
    // take that same bit on the async mailbox thread.
    return try_admit_send_parts (
      record_->parts.data (), record_->parts.size (), record_->target,
      record_->has_target, true, record_, record_->admission_observer,
      record_->admission_observer_userdata, record_->request_admission);
}

int zlink::socket_base_t::try_admit_send_parts (
  zlink_msg_t *parts_, size_t part_count_,
  const routed_send_target_key_t &target_, bool has_target_,
  bool commands_already_processed_, send_pending_record_t *record_,
  pipe_write_observer_fn observer_, void *observer_userdata_,
  bool request_admission_)
{
    if (!parts_ || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    //  One scope for the whole record. This is the structural reason the
    //  array form exists: the per-handle send sequence is taken and released
    //  inside a single call, so a multipart record can never hold it across
    //  application code.
    socket_public_send_scope_t scope (
      lifecycle_coordinator (), true, socket_send_admission_complete);
    if (!scope.acquired ()) {
        //  A lifecycle refusal (close admitted, context terminated) is not a
        //  route failure. Keep the record pending so the close / term path
        //  owns the terminal cause instead of reporting ESHUTDOWN here.
        errno = EAGAIN;
        return -1;
    }

    //  Pipe detach takes this lifecycle sync before publishing its pending
    //  terminal handoff. Check that handoff only after acquiring the same
    //  sync: a detach that linearized first prevents physical admission,
    //  while a detach that waits behind this scope cannot override a send
    //  that has already admitted successfully.
    if (record_) {
        scoped_lock_t lock (send_pending_runtime ().sync);
        if (record_->deferred_terminal_errno != 0) {
            errno = record_->deferred_terminal_errno;
            return -1;
        }
    }

    return try_admit_send_parts_scoped (
      parts_, part_count_, target_, has_target_, scope, NULL,
      commands_already_processed_, observer_, observer_userdata_,
      request_admission_, record_ == NULL);
}

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
            //  Nothing reached the pipe, so the record is untouched.
            //  Backpressure keeps it reserved; a route failure completes
            //  it with that cause. Lifecycle refusals are the exception:
            //  close and context termination own the pending terminal.
            errno = failure_errno == ESHUTDOWN || failure_errno == ETERM
                      ? EAGAIN
                      : failure_errno;
            return -1;
        }

        //  Drop the half-written physical attempt before releasing the scope.
        //  Framed sockets can discover byte HWM on a continuation. An async
        //  record has a pristine Core-owned shadow, so retain EAGAIN and retry
        //  the complete record after the credit wake.
        (void) rollback_scoped (scope);
        errno = failure_errno;
        return -1;
    }
    return 0;
}

//  Admit whatever the current pipe state allows. Head-of-line within a target
//  is deliberate: the target queue is one logical stream.
void zlink::socket_base_t::drive_send_pending ()
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    if (pending.pending_msgs.load (std::memory_order_acquire) == 0)
        return;
    bool gate_expected = false;
    if (!pending.admission_gate.compare_exchange_strong (
          gate_expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire))
        return;

    // Only the admission-gate owner accesses this scratch storage. Its logical
    // size is local to this drive call, while constructed key capacity survives
    // repeated backpressure cycles.
    std::vector<routed_send_target_key_t> &blocked_targets =
      pending.blocked_targets_scratch;
    size_t blocked_target_count = 0;
    uint64_t observed_redrive_epoch =
      pending.redrive_epoch.load (std::memory_order_acquire);
    while (true) {
        send_pending_record_t *record = NULL;
        uint64_t scanned_enqueue_epoch = 0;
        {
            scoped_lock_t lock (pending.sync);
            for (std::map<routed_send_target_key_t,
                          std::deque<send_pending_record_t *> >::iterator it =
                   pending.queues.begin ();
                 it != pending.queues.end (); ++it) {
                const std::map<routed_send_target_key_t,
                               send_inline_attempt_state_t>::const_iterator
                  inline_attempt = pending.inline_attempts.find (it->first);
                if (it->second.empty ()
                    || contains_routed_send_target (
                      blocked_targets, blocked_target_count, it->first)
                    || (inline_attempt != pending.inline_attempts.end ()
                        && inline_attempt->second.active))
                    continue;
                send_pending_record_t *head = it->second.front ();
                if (head->claimed)
                    continue;
                head->claimed = true;
                record = head;
                break;
            }
            //  Queue insertion publishes the record under this same lock and
            //  then advances the epoch.  Snapshotting here lets the gate
            //  owner detect an insertion that raced its final empty scan.
            scanned_enqueue_epoch = pending.enqueue_epoch.load (
              std::memory_order_acquire);
        }
        if (!record) {
            pending.admission_gate.store (false, std::memory_order_release);

            //  A submitter that published before the gate release observed
            //  the gate as owned and returned from its drive attempt. Retake
            //  ownership when its epoch is newer. A physical-admission wake
            //  also invalidates the local blocked set: its mailbox command may
            //  already have observed the gate as owned and returned. Otherwise
            //  either no work arrived or a caller arriving after these checks
            //  will acquire the now-free gate itself.
            const uint64_t latest_redrive_epoch =
              pending.redrive_epoch.load (std::memory_order_acquire);
            const bool redrive_requested =
              latest_redrive_epoch != observed_redrive_epoch;
            if (pending.enqueue_epoch.load (std::memory_order_acquire)
                  != scanned_enqueue_epoch
                || redrive_requested) {
                bool reacquire_expected = false;
                if (pending.admission_gate.compare_exchange_strong (
                      reacquire_expected, true, std::memory_order_acq_rel,
                      std::memory_order_acquire)) {
                    if (redrive_requested)
                        blocked_target_count = 0;
                    observed_redrive_epoch = latest_redrive_epoch;
                    continue;
                }
            }
            return;
        }

        const int rc = try_admit_send_pending (record);
        if (rc == 0) {
            finish_send_pending (record, ZLINK_SEND_ADMITTED, 0);
            continue;
        }
        const int failure_errno = errno;
        const bool retry_later =
          failure_errno == EAGAIN
          || (record->pull_completion
              && retryable_pull_target_errno (failure_errno));
        int deferred_terminal_errno = 0;
        {
            scoped_lock_t lock (pending.sync);
            deferred_terminal_errno = record->deferred_terminal_errno;
            if (deferred_terminal_errno == 0 && retry_later) {
                record->claimed = false;
                try {
                    append_routed_send_target (
                      &blocked_targets, &blocked_target_count, record->target);
                } catch (...) {
                    //  A failed bookkeeping allocation must not escape
                    //  through a public C entry point or strand the admission
                    //  gate. Preserve the same handoff checks as the normal
                    //  final scan: a wake or insertion may already have
                    //  observed this gate as owned.
                    const uint64_t latest_redrive_epoch =
                      pending.redrive_epoch.load (std::memory_order_acquire);
                    const bool redrive_requested =
                      latest_redrive_epoch != observed_redrive_epoch;
                    const bool enqueue_requested =
                      pending.enqueue_epoch.load (std::memory_order_acquire)
                      != scanned_enqueue_epoch;
                    pending.admission_gate.store (false,
                                                  std::memory_order_release);
                    if (redrive_requested || enqueue_requested) {
                        bool reacquire_expected = false;
                        if (pending.admission_gate.compare_exchange_strong (
                              reacquire_expected, true,
                              std::memory_order_acq_rel,
                              std::memory_order_acquire)) {
                            if (redrive_requested)
                                blocked_target_count = 0;
                            observed_redrive_epoch = latest_redrive_epoch;
                            continue;
                        }
                    }
                    return;
                }
                const uint64_t latest_redrive_epoch =
                  pending.redrive_epoch.load (std::memory_order_acquire);
                if (latest_redrive_epoch != observed_redrive_epoch) {
                    blocked_target_count = 0;
                    observed_redrive_epoch = latest_redrive_epoch;
                }
            }
        }
        if (deferred_terminal_errno != 0) {
            finish_send_pending (record, ZLINK_SEND_TERMINAL,
                                 deferred_terminal_errno);
            continue;
        }
        if (retry_later) {
            continue;
        }
        finish_send_pending (record, ZLINK_SEND_TERMINAL, failure_errno);
    }
}

void zlink::socket_base_t::notify_send_pending_writable (pipe_t *pipe_)
{
    LIBZLINK_UNUSED (pipe_);
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    if (pending.pending_msgs.load (std::memory_order_acquire) == 0)
        return;
    {
        scoped_lock_t lock (pending.sync);
        if (pending.by_op.empty ())
            return;
        pending.redrive_epoch.fetch_add (1, std::memory_order_release);
    }
    command_t wake;
    memset (&wake, 0, sizeof (wake));
    wake.destination = this;
    wake.type = command_t::send_pending;
    static_cast<mailbox_t *> (_mailbox)->send (wake);
}

void zlink::socket_base_t::notify_incremental_send_released ()
{
    lifecycle_coordinator ().release_public_multipart_control_boundary ();
    if (lifecycle_coordinator ().deferred_peer_controls_pending_cached ())
        flush_deferred_peer_controls ();
    notify_send_pending_writable (NULL);
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

void zlink::socket_base_t::fail_pull_send_pending_for_logical_target (
  const zlink_routing_id_t *peer_rid_, int terminal_errno_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    const std::string logical_rid =
      peer_rid_ && peer_rid_->size
        ? std::string (reinterpret_cast<const char *> (peer_rid_->data),
                       peer_rid_->size)
        : std::string ();
    bool signaled_waiter = false;
    {
        scoped_lock_t lock (pending.sync);
        for (std::map<routed_send_target_key_t,
                      send_logical_wait_state_t>::iterator it =
               pending.logical_waits.begin ();
             it != pending.logical_waits.end (); ++it) {
            if (it->first.logical_endpoint.empty ()
                && it->first.peer_rid == logical_rid) {
                signal_logical_wait_state (&it->second, terminal_errno_);
                signaled_waiter = true;
            }
        }
    }
    if (signaled_waiter) {
        command_t wake;
        memset (&wake, 0, sizeof (wake));
        wake.destination = this;
        wake.type = command_t::send_pending;
        static_cast<mailbox_t *> (_mailbox)->send (wake);
    }
    bool completed_any = false;
    while (true) {
        send_pending_record_t *doomed = NULL;
        {
            scoped_lock_t lock (pending.sync);
            for (std::map<zlink_send_op_id_t,
                          send_pending_record_t *>::iterator it =
                   pending.by_op.begin ();
                 it != pending.by_op.end (); ++it) {
                send_pending_record_t *candidate = it->second;
                if (!candidate->pull_completion || candidate->claimed
                    || candidate->target.peer_rid != logical_rid)
                    continue;
                candidate->claimed = true;
                doomed = candidate;
                break;
            }
        }
        if (!doomed)
            break;
        finish_send_pending (doomed, ZLINK_SEND_TERMINAL,
                             terminal_errno_);
        completed_any = true;
    }
    if (completed_any)
        notify_request_completion ();
}

void zlink::socket_base_t::fail_pull_send_pending_for_logical_endpoint (
  const std::string &endpoint_, int terminal_errno_)
{
    if (endpoint_.empty ())
        return;
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    bool signaled_waiter = false;
    {
        scoped_lock_t lock (pending.sync);
        for (std::map<routed_send_target_key_t,
                      send_logical_wait_state_t>::iterator it =
               pending.logical_waits.begin ();
             it != pending.logical_waits.end (); ++it) {
            if (it->first.logical_endpoint == endpoint_) {
                signal_logical_wait_state (&it->second, terminal_errno_);
                signaled_waiter = true;
            }
        }
    }
    if (signaled_waiter) {
        command_t wake;
        memset (&wake, 0, sizeof (wake));
        wake.destination = this;
        wake.type = command_t::send_pending;
        static_cast<mailbox_t *> (_mailbox)->send (wake);
    }
    bool completed_any = false;
    while (true) {
        send_pending_record_t *doomed = NULL;
        {
            scoped_lock_t lock (pending.sync);
            for (std::map<zlink_send_op_id_t,
                          send_pending_record_t *>::iterator it =
                   pending.by_op.begin ();
                 it != pending.by_op.end (); ++it) {
                send_pending_record_t *const candidate = it->second;
                if (!candidate->pull_completion || candidate->claimed
                    || candidate->target.logical_endpoint != endpoint_)
                    continue;
                candidate->claimed = true;
                doomed = candidate;
                break;
            }
        }
        if (!doomed)
            break;
        finish_send_pending (doomed, ZLINK_SEND_TERMINAL,
                             terminal_errno_);
        completed_any = true;
    }
    if (completed_any)
        notify_request_completion ();
}

//  close / ctx term: fail fast. LINGER covers bytes already admitted to a
//  pipe; a pending record has not been admitted, so draining it here would
//  make close wait on the peer's consumption rate.
void zlink::socket_base_t::fail_all_send_pending (int terminal_errno_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    {
        scoped_lock_t lock (pending.sync);
        pending.failing.store (true, std::memory_order_release);
        for (std::map<routed_send_target_key_t,
                      send_logical_wait_state_t>::iterator wait =
               pending.logical_waits.begin ();
             wait != pending.logical_waits.end (); ++wait)
            signal_logical_wait_state (&wait->second, terminal_errno_);
        std::map<routed_send_target_key_t,
                 send_inline_attempt_state_t>::iterator attempt =
          pending.inline_attempts.begin ();
        while (attempt != pending.inline_attempts.end ()) {
            if (attempt->second.active) {
                if (!attempt->second.retire) {
                    attempt->second.retire = true;
                    attempt->second.retire_errno = terminal_errno_;
                }
                ++attempt;
            } else {
                attempt = pending.inline_attempts.erase (attempt);
            }
        }
    }
    while (true) {
        send_pending_record_t *doomed = NULL;
        {
            scoped_lock_t lock (pending.sync);
            for (std::map<zlink_send_op_id_t,
                          send_pending_record_t *>::iterator it =
                   pending.by_op.begin ();
                 it != pending.by_op.end (); ++it) {
                if (it->second->claimed) {
                    if (it->second->deferred_terminal_errno == 0)
                        it->second->deferred_terminal_errno = terminal_errno_;
                    continue;
                }
                it->second->claimed = true;
                doomed = it->second;
                break;
            }
        }
        if (!doomed)
            break;
        finish_send_pending (doomed, ZLINK_SEND_TERMINAL, terminal_errno_);
    }
}
