/* SPDX-License-Identifier: MPL-2.0 */

// Core-owned pull-completion SEND and internal REQUEST admission.
//
//  The mechanism is the blocking send's own wait loop with two substitutions:
//  the thread that retries is the socket's async mailbox owner instead of the
//  caller, and the exit is a completion notification instead of a condvar
//  return. Nothing here invents a new admission rule - every record is
//  admitted through the same send_direct_with_retry() entry, so it competes
//  for the same byte high-water mark as a synchronous send.

#include "utils/precompiled.hpp"

#include "core/c_api_copy_internal.hpp"
#include "core/mailbox.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"
#include "utils/likely.hpp"
#include "utils/macros.hpp"
#include "utils/routing_id.hpp"

#include <set>

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

int zlink::socket_base_t::reserve_shared_pending_record (
  uint64_t charge_bytes_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    scoped_lock_t lock (pending.sync);
    const uint64_t next_count =
      pending.pending_msgs.load (std::memory_order_relaxed) == UINT64_MAX
        ? UINT64_MAX
        : pending.pending_msgs.load (std::memory_order_relaxed) + 1;
    const uint64_t next_bytes =
      UINT64_MAX - pending.pending_bytes < charge_bytes_
        ? UINT64_MAX
        : pending.pending_bytes + charge_bytes_;
    if ((options.send_pending_max_msgs != 0
         && next_count > options.send_pending_max_msgs)
        || (options.send_pending_max_bytes != 0
            && next_bytes > options.send_pending_max_bytes)) {
        errno = EAGAIN;
        return -1;
    }
    pending.pending_msgs.store (next_count, std::memory_order_release);
    pending.pending_bytes = next_bytes;
    errno = 0;
    return 0;
}

void zlink::socket_base_t::release_shared_pending_record (
  uint64_t charge_bytes_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    scoped_lock_t lock (pending.sync);
    const uint64_t count =
      pending.pending_msgs.load (std::memory_order_relaxed);
    pending.pending_msgs.store (count > 0 ? count - 1 : 0,
                                std::memory_order_release);
    pending.pending_bytes = pending.pending_bytes > charge_bytes_
                              ? pending.pending_bytes - charge_bytes_
                              : 0;
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
    //  Parts are closed the same way a synchronous send consumes them: close
    //  is correct after admission too, because the pipe holds its own
    //  reference to the frame content.
    for (size_t i = 0; i != record_->parts.size (); ++i) {
        msg_t *msg = reinterpret_cast<msg_t *> (&record_->parts[i]);
        if (msg->check ()) {
            const int rc = msg->close ();
            errno_assert (rc == 0);
        }
    }
    record_->parts.clear ();
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
      request_admission_);
}

int zlink::socket_base_t::try_admit_send_parts_scoped (
  zlink_msg_t *parts_, size_t part_count_,
  const routed_send_target_key_t &target_, bool has_target_,
  socket_public_send_scope_t &scope, pipe_t **attempted_pipe_out_,
  bool commands_already_processed_, pipe_write_observer_fn observer_,
  void *observer_userdata_, bool request_admission_)
{
    if (!parts_ || part_count_ == 0 || !scope.acquired ()) {
        errno = EFAULT;
        return -1;
    }

    const size_t count = part_count_;
    const auto attempt = [&] (zlink_msg_t *attempt_parts_,
                              bool retry_whole_on_backpressure_) -> int {
        zlink_routing_id_t rid;
        memset (&rid, 0, sizeof (rid));
        if (has_target_)
            copy_routing_id_from_bytes (target_.peer_rid.data (),
                                        target_.peer_rid.size (), &rid);

        for (size_t i = 0; i != count; ++i) {
            const int flags =
              ZLINK_DONTWAIT | (i + 1 < count ? ZLINK_SNDMORE : 0);
            msg_t *msg =
              reinterpret_cast<msg_t *> (&attempt_parts_[i]);
            //  A routed target selects and pins the application pipe at the
            //  beginning of a logical record.  The socket's xsend path owns
            //  the continuation state after that first part: ROUTER keeps
            //  `_current_out`/`_more_out`, while DEALER keeps the load
            //  balancer's multipart pipe. Continue through ordinary xsend so
            //  the whole record remains one gated sequence.
            const bool routed_start = has_target_ && i == 0;
            const bool selected_pipe_start =
              i == 0 && target_.selected_pipe != NULL;
            const bool configured_endpoint_start =
              i == 0 && !selected_pipe_start
              && !target_.logical_endpoint.empty ();
            const bool observe_commit = observer_ && i + 1 == count;
            const int rc = selected_pipe_start
              ? xsend_selected_pipe (
                  target_.selected_pipe, msg, flags, request_admission_,
                  NULL, observe_commit ? observer_ : NULL,
                  observe_commit ? observer_userdata_ : NULL)
              : configured_endpoint_start
              ? xsend_configured_endpoint (
                  target_.logical_endpoint, msg, flags, request_admission_,
                  attempted_pipe_out_, NULL,
                  observe_commit ? observer_ : NULL,
                  observe_commit ? observer_userdata_ : NULL)
              : routed_start
                ? send_direct_with_retry (
                    &rid, msg, flags, scope, NULL,
                    0, false,
                    attempted_pipe_out_, target_.transport_pair_id,
                    target_.transport_pair_generation, true,
                    commands_already_processed_,
                    observe_commit ? observer_ : NULL,
                    observe_commit ? observer_userdata_ : NULL, NULL,
                    target_.route_incarnation_id)
                : send_direct_with_retry (
                    NULL, msg, flags, scope, NULL, 0, false, NULL, 0, 0,
                    true, commands_already_processed_,
                    observe_commit ? observer_ : NULL,
                    observe_commit ? observer_userdata_ : NULL);
            if (rc == 0)
                continue;

            const int failure_errno = errno;
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

            //  Drop the half-written physical attempt before releasing the
            //  scope. Framed sockets can discover byte HWM on a continuation.
            //  An async record has a pristine Core-owned shadow, so retain
            //  EAGAIN and retry the complete record after the credit wake.
            (void) rollback_scoped (scope);
            errno = retry_whole_on_backpressure_ && failure_errno == EAGAIN
                      ? EAGAIN
                      : failure_errno == EAGAIN ? ECONNABORTED
                                                : failure_errno;
            return -1;
        }
        return 0;
    };

    if (count == 1)
        return attempt (parts_, false);

    // PAIR, DEALER and ROUTER all perform physical multipart admission one
    // frame at a time. Send shallow Core message copies so a later-frame
    // atomic abort never consumes the pending record's pristine parts. The
    // retry starts at frame zero; no independent record can inherit a prefix.
    // STREAM rejects multipart before this path, so every supported async
    // framed socket uses the same attempt ownership rule.
    const size_t inline_attempt_capacity = 4;
    if (count <= inline_attempt_capacity) {
        zlink_msg_t attempt_parts[inline_attempt_capacity];
        if (copy_send_part_array (parts_, count, attempt_parts) != 0)
            return -1;
        const int rc = attempt (attempt_parts, true);
        const int saved_errno = errno;
        // Every successful xsend detaches its input by reinitializing it as an
        // empty message. Only a failed or partial attempt can still own refs.
        if (rc != 0)
            close_send_part_array (attempt_parts, count);
        errno = saved_errno;
        return rc;
    }

    std::vector<zlink_msg_t> attempt_parts;
    try {
        attempt_parts.resize (count);
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
    if (copy_send_part_array (parts_, count, attempt_parts.data ()) != 0)
        return -1;
    const int rc = attempt (attempt_parts.data (), true);
    const int saved_errno = errno;
    if (rc != 0)
        close_send_part_array (attempt_parts.data (), count);
    errno = saved_errno;
    return rc;
}

//  Admit whatever the current pipe state allows. Head-of-line within a target
//  is deliberate: the target queue is one logical stream.
void zlink::socket_base_t::drive_send_pending ()
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    bool gate_expected = false;
    if (!pending.admission_gate.compare_exchange_strong (
          gate_expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire))
        return;

    std::set<routed_send_target_key_t> blocked;
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
                if (it->second.empty () || blocked.count (it->first) != 0
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
                        blocked.clear ();
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
                    blocked.insert (record->target);
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
                                blocked.clear ();
                            observed_redrive_epoch = latest_redrive_epoch;
                            continue;
                        }
                    }
                    return;
                }
                const uint64_t latest_redrive_epoch =
                  pending.redrive_epoch.load (std::memory_order_acquire);
                if (latest_redrive_epoch != observed_redrive_epoch) {
                    blocked.clear ();
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
    {
        socket_send_pending_runtime_t &pending = send_pending_runtime ();
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
    if (lifecycle_coordinator ().public_multipart_send_active ())
        return;

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
    for (size_t i = 0; i != targets.size (); ++i) {
        (void) targets[i]->flush_pending_peer_controls ();
        targets[i]->release_lifetime_ref ();
    }
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
        pending.failing = true;
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
