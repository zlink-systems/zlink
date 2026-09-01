/* SPDX-License-Identifier: MPL-2.0 */

//  Core-owned asynchronous send admission (zlink_send_async family).
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
#include "api/socket/request_timeout_scheduler_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/err.hpp"
#include "utils/likely.hpp"
#include "utils/macros.hpp"
#include "utils/routing_id.hpp"

#include <set>

namespace
{
bool socket_type_supports_send_async (int type_)
{
    return type_ == ZLINK_CORE_SOCKET_PAIR || type_ == ZLINK_CORE_SOCKET_DEALER
           || type_ == ZLINK_CORE_SOCKET_ROUTER
           || type_ == ZLINK_CORE_SOCKET_STREAM;
}

void close_send_attempt_parts (std::vector<zlink_msg_t> *parts_)
{
    const int saved_errno = errno;
    for (size_t i = 0; i != parts_->size (); ++i) {
        zlink::msg_t *msg =
          reinterpret_cast<zlink::msg_t *> (&(*parts_)[i]);
        if (msg->check ()) {
            const int rc = msg->close ();
            errno_assert (rc == 0);
        }
    }
    parts_->clear ();
    errno = saved_errno;
}

int copy_send_attempt_parts (zlink_msg_t *parts_,
                             size_t part_count_,
                             std::vector<zlink_msg_t> *copies_)
{
    try {
        copies_->resize (part_count_);
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }

    size_t initialized = 0;
    for (; initialized != part_count_; ++initialized) {
        zlink::msg_t *copy =
          reinterpret_cast<zlink::msg_t *> (&(*copies_)[initialized]);
        if (copy->init () != 0)
            break;
    }
    if (initialized != part_count_) {
        const int saved_errno = errno;
        copies_->resize (initialized);
        close_send_attempt_parts (copies_);
        errno = saved_errno;
        return -1;
    }

    for (size_t i = 0; i != part_count_; ++i) {
        zlink::msg_t *copy =
          reinterpret_cast<zlink::msg_t *> (&(*copies_)[i]);
        zlink::msg_t *source =
          reinterpret_cast<zlink::msg_t *> (&parts_[i]);
        if (copy->copy (*source) != 0) {
            const int saved_errno = errno;
            close_send_attempt_parts (copies_);
            errno = saved_errno;
            return -1;
        }
    }
    return 0;
}

bool send_target_matches (const zlink::routed_send_target_key_t &candidate_,
                          const zlink_routing_id_t *peer_rid_,
                          uint64_t transport_pair_id_,
                          uint64_t transport_pair_generation_,
                          uint64_t route_incarnation_id_)
{
    return candidate_.transport_pair_id == transport_pair_id_
           && candidate_.transport_pair_generation
                == transport_pair_generation_
           && candidate_.route_incarnation_id == route_incarnation_id_
           && candidate_.peer_rid.size () == peer_rid_->size
           && (peer_rid_->size == 0
               || memcmp (candidate_.peer_rid.data (), peer_rid_->data,
                          peer_rid_->size)
                    == 0);
}

}

bool zlink::socket_type_supports_send_completion (int type_)
{
    return socket_type_supports_send_async (type_);
}

bool zlink::socket_base_t::send_complete_handler_active () const
{
    return send_pending_runtime ().handler_installed.load (
      std::memory_order_acquire);
}

bool zlink::socket_base_t::has_send_pending () const
{
    const socket_send_pending_runtime_t &pending = send_pending_runtime ();
    scoped_lock_t lock (pending.sync);
    return !pending.by_op.empty () || pending.completion_head != NULL;
}

bool zlink::socket_base_t::begin_send_async_public_call ()
{
    if (socket_send_complete_dispatch_scope_t::dispatching_any ()) {
        errno = EDEADLK;
        return false;
    }

    // Completion owners hold this gate across dispatch. Taking it while the
    // depth changes closes the check-then-callback race: either dispatch has
    // already finished, or its next depth check observes this public call.
    scoped_lock_t owner_lock (_completion_owner_sync);

    // Keep the socket alive after the wrapper drops its public-handle pin and
    // until the final-depth transition has published the completion wake.
    // The wrapper already owns public-handle admission, so only the mailbox
    // lifetime pin is needed here; taking a second lifecycle admission would
    // add two locked RMWs to every async submit.
    inc_mailbox_ref ();
    send_pending_runtime ().public_async_depth.fetch_add (
      1, std::memory_order_acq_rel);
    return true;
}

void zlink::socket_base_t::end_send_async_public_call ()
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    const uint32_t old = pending.public_async_depth.fetch_sub (
      1, std::memory_order_acq_rel);
    zlink_assert (old > 0);
    // A public submit must not run its completion callback inline while
    // releasing this depth. In particular, a callback may close its own
    // socket and wait for the async mailbox to quiesce; dispatching here while
    // holding the completion-owner gate would then deadlock that mailbox.
    // Publish a wake only for an actual queued completion. The owner gate
    // makes the queue check and notification atomic with every completion
    // drain, while unresolved admission work remains an internal redrive.
    if (old == 1) {
        scoped_lock_t owner_lock (_completion_owner_sync);
        bool completion_ready = false;
        {
            scoped_lock_t lock (pending.sync);
            completion_ready = pending.completion_head != NULL;
        }
        if (completion_ready)
            notify_request_completion ();
    }
    dec_mailbox_ref ();
}

#ifdef ZLINK_BUILD_TESTS
void zlink::socket_base_t::test_set_send_pending_gate_release_hook (
  socket_send_pending_runtime_t::gate_release_hook_fn hook_, void *userdata_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    pending.gate_release_hook.store (NULL, std::memory_order_release);
    // Invalidate readers that loaded the previous hook but have not yet
    // published their active pin. This must happen before waiting for active
    // readers, otherwise a same-function-pointer rearm could make such a
    // reader call the new stack-owned userdata as if it were the old probe.
    pending.gate_release_hook_generation.fetch_add (1,
                                                     std::memory_order_acq_rel);
    while (pending.gate_release_hook_active.load (std::memory_order_acquire)
           != 0)
        std::this_thread::yield ();
    pending.gate_release_hook_userdata.store (userdata_,
                                              std::memory_order_relaxed);
    pending.gate_release_hook_generation.fetch_add (1,
                                                     std::memory_order_release);
    pending.gate_release_hook.store (hook_, std::memory_order_release);
}

void zlink::socket_base_t::test_set_send_inline_fallback_hook (
  socket_send_pending_runtime_t::inline_fallback_hook_fn hook_,
  void *userdata_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    scoped_lock_t lock (pending.sync);
    pending.inline_fallback_hook = hook_;
    pending.inline_fallback_hook_userdata = userdata_;
}

void zlink::socket_base_t::test_set_send_target_failure_progress_hook (
  socket_send_pending_runtime_t::target_failure_progress_hook_fn hook_,
  void *userdata_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    scoped_lock_t lock (pending.sync);
    pending.target_failure_progress_hook = hook_;
    pending.target_failure_progress_hook_userdata = userdata_;
}

void zlink::socket_base_t::test_set_send_deadline_enqueue_hook (
  socket_send_pending_runtime_t::deadline_enqueue_hook_fn hook_,
  void *userdata_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    scoped_lock_t lock (pending.sync);
    pending.deadline_enqueue_hook = hook_;
    pending.deadline_enqueue_hook_userdata = userdata_;
}

void zlink::socket_base_t::test_set_send_next_op_id (
  zlink_send_op_id_t next_op_id_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    scoped_lock_t lock (pending.sync);
    zlink_assert (pending.by_op.empty ());
    pending.next_op_id = next_op_id_;
}

void zlink::socket_base_t::test_set_send_fail_after_queue_push (bool enabled_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    scoped_lock_t lock (pending.sync);
    pending.fail_after_queue_push = enabled_;
}
#endif

int zlink::socket_base_t::socket_set_send_complete_handler (
  zlink_send_complete_handler_fn handler_, void *userdata_)
{
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    socket_public_api_scope_t admission (lifecycle);
    if (!admission.acquired ())
        return -1;
    if (!handler_) {
        errno = EINVAL;
        return -1;
    }
    if (!socket_type_supports_send_async (options.type)) {
        errno = ENOTSUP;
        return -1;
    }
    //  Replacing the handler from inside its own dispatch would let a
    //  completion decide who receives the completions still queued behind it.
    if (socket_send_complete_dispatch_scope_t::dispatching_socket (this)) {
        errno = EDEADLK;
        return -1;
    }
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    //  The admit loop and the completion dispatch both run on the socket's
    //  async mailbox owner unless a POLLCOMPLETION registration takes that
    //  ownership. Starting it here is the same handoff the removed readiness
    //  handlers performed.
    retain_async_command_processing ();
    if (!lifecycle.is_async_mailbox_active ()) {
        io_thread_t *io_thread = choose_io_thread (options.affinity);
        if (!io_thread || start_async_mailbox_processing (io_thread) != 0) {
            if (!io_thread)
                errno = EAGAIN;
            return -1;
        }
        lifecycle.wait_async_started (1000);
    }

    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    {
        scoped_lock_t lock (pending.sync);
        pending.handler = handler_;
        pending.handler_userdata = userdata_;
    }
    pending.handler_installed.store (true, std::memory_order_release);
    return 0;
}

void zlink::socket_base_t::destroy_send_pending_record (
  send_pending_record_t *record_)
{
    if (!record_)
        return;
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
    delete record_;
}

void zlink::socket_base_t::send_pending_deadline_trampoline (void *userdata_)
{
    //  Firing transfers the scheduler payload to this handler. Publish only
    //  the immutable op id to the socket mailbox: close first quiesces that
    //  single command owner and cancel() joins this trampoline before reap,
    //  so no scheduler thread races socket-owned pending state or destruction.
    std::unique_ptr<std::pair<socket_base_t *, zlink_send_op_id_t> > entry (
      static_cast<std::pair<socket_base_t *, zlink_send_op_id_t> *> (
        userdata_));
    if (!entry || !entry->first)
        return;

#ifdef ZLINK_BUILD_TESTS
    socket_send_pending_runtime_t::deadline_enqueue_hook_fn hook = NULL;
    void *hook_userdata = NULL;
    {
        socket_send_pending_runtime_t &pending =
          entry->first->send_pending_runtime ();
        scoped_lock_t lock (pending.sync);
        hook = pending.deadline_enqueue_hook;
        hook_userdata = pending.deadline_enqueue_hook_userdata;
    }
    if (hook)
        hook (hook_userdata);
#endif

    command_t wake;
    memset (&wake, 0, sizeof (wake));
    wake.destination = entry->first;
    wake.type = command_t::send_pending_timeout;
    wake.args.send_pending_timeout.op_id = entry->second;
    static_cast<mailbox_t *> (entry->first->_mailbox)->send (wake);
}

void zlink::socket_base_t::process_send_pending_timeout (uint64_t op_id_)
{
    on_send_pending_deadline (
      static_cast<zlink_send_op_id_t> (op_id_));
}

void zlink::socket_base_t::on_send_pending_deadline (zlink_send_op_id_t op_id_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    send_pending_record_t *record = NULL;
    {
        scoped_lock_t lock (pending.sync);
        std::map<zlink_send_op_id_t, send_pending_record_t *>::iterator it =
          pending.by_op.find (op_id_);
        //  Expiry and admission can reach for the same record. The claim flag
        //  is the single atomic winner: whoever holds it decides the result.
        if (it == pending.by_op.end () || it->second->claimed)
            return;
        record = it->second;
        record->claimed = true;
    }
    finish_send_pending (record, ZLINK_SEND_TIMED_OUT, ETIMEDOUT);
    dispatch_send_completions_if_local ();
}

//  Remove a resolved record from the pending books and queue its completion.
void zlink::socket_base_t::finish_send_pending (
  send_pending_record_t *record_, zlink_send_complete_result_t result_,
  int terminal_errno_)
{
    if (!record_)
        return;

    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    std::shared_ptr<request_timeout::task_t> deadline;
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

        record_->completion_result = result_;
        record_->completion_errno =
          result_ == ZLINK_SEND_ADMITTED ? 0 : terminal_errno_;
        record_->completion_next = NULL;
        if (pending.completion_tail)
            pending.completion_tail->completion_next = record_;
        else
            pending.completion_head = record_;
        pending.completion_tail = record_;
        deadline.swap (record_->deadline);
    }
    if (deadline)
        request_timeout::cancel (deadline);
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
    return try_admit_send_parts (record_->parts.data (), record_->parts.size (),
                                 record_->target, record_->has_target, true,
                                 record_);
}

int zlink::socket_base_t::try_admit_send_parts (
  zlink_msg_t *parts_, size_t part_count_,
  const routed_send_target_key_t &target_, bool has_target_,
  bool commands_already_processed_, send_pending_record_t *record_)
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
      commands_already_processed_);
}

int zlink::socket_base_t::try_admit_send_parts_scoped (
  zlink_msg_t *parts_, size_t part_count_,
  const routed_send_target_key_t &target_, bool has_target_,
  socket_public_send_scope_t &scope, pipe_t **attempted_pipe_out_,
  bool commands_already_processed_)
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
            const int rc =
              routed_start
                ? send_direct_with_retry (
                    &rid, msg, flags, scope, NULL,
                    0, false,
                    attempted_pipe_out_, target_.transport_pair_id,
                    target_.transport_pair_generation, true,
                    commands_already_processed_, NULL, NULL, NULL,
                    target_.route_incarnation_id)
                : send_direct_with_retry (
                    NULL, msg, flags, scope, NULL, 0, false, NULL, 0, 0,
                    true, commands_already_processed_);
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
    std::vector<zlink_msg_t> attempt_parts;
    if (copy_send_attempt_parts (parts_, count, &attempt_parts) != 0)
        return -1;
    const int rc = attempt (attempt_parts.data (), true);
    const int saved_errno = errno;
    close_send_attempt_parts (&attempt_parts);
    errno = saved_errno;
    return rc;
}

//  Admit whatever the current pipe state allows. Head-of-line within a target
//  is deliberate: the target queue is one logical stream.
void zlink::socket_base_t::drive_send_pending ()
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    if (!send_complete_handler_active ())
        return;
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
#ifdef ZLINK_BUILD_TESTS
            const uint64_t gate_hook_generation =
              pending.gate_release_hook_generation.load (
                std::memory_order_acquire);
            socket_send_pending_runtime_t::gate_release_hook_fn gate_hook =
              pending.gate_release_hook.load (std::memory_order_acquire);
            if (gate_hook) {
                pending.gate_release_hook_active.fetch_add (
                  1, std::memory_order_acq_rel);
                // Disarm can win between the first load and the active pin.
                // Recheck after publishing the pin before dereferencing the
                // test's stack-owned userdata. The generation rejects a
                // disarm/rearm ABA where the function pointer is unchanged.
                if (pending.gate_release_hook_generation.load (
                      std::memory_order_acquire)
                    == gate_hook_generation
                    && pending.gate_release_hook.load (
                         std::memory_order_acquire)
                         == gate_hook) {
                    gate_hook (pending.gate_release_hook_userdata.load (
                      std::memory_order_acquire));
                }
                pending.gate_release_hook_active.fetch_sub (
                  1, std::memory_order_acq_rel);
            }
#endif
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
        int deferred_terminal_errno = 0;
        {
            scoped_lock_t lock (pending.sync);
            deferred_terminal_errno = record->deferred_terminal_errno;
            if (deferred_terminal_errno == 0 && failure_errno == EAGAIN) {
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
        if (failure_errno == EAGAIN) {
            continue;
        }
        finish_send_pending (record, ZLINK_SEND_TERMINAL, failure_errno);
    }
}

//  Dispatch here only when this socket still owns its completion dispatch.
//  A ZLINK_POLLCOMPLETION registration moves that ownership to the poller wait
//  thread, and running the callback anywhere else would break the single
//  dispatch-owner rule the reply completions already rely on.
void zlink::socket_base_t::dispatch_send_completions_if_local ()
{
    //  Already inside this socket's dispatch loop: that loop drains whatever
    //  was just queued, and re-entering here would take the owner gate twice
    //  on the same thread.
    if (socket_send_complete_dispatch_scope_t::dispatching_socket (this))
        return;
    scoped_lock_t owner_lock (_completion_owner_sync);
    if (_completion_poller_refs.load (std::memory_order_acquire) != 0) {
        socket_send_pending_runtime_t &pending = send_pending_runtime ();
        // The final public-depth release re-enters this owner gate. Publishing
        // earlier would let the waiter consume the wake while callbacks are
        // still deliberately barred from dispatch.
        if (pending.public_async_depth.load (std::memory_order_acquire) != 0)
            return;
        bool completion_ready = false;
        {
            scoped_lock_t lock (pending.sync);
            completion_ready = pending.completion_head != NULL;
        }
        // Producers that resolve work outside poller_wait (notably public
        // cancellation) must wake the transferred dispatch owner themselves.
        // The notification is tied to an actual queued completion, so a
        // redrive-only wake cannot escape as public POLLCOMPLETION.
        if (completion_ready)
            notify_request_completion ();
        return;
    }
    (void) dispatch_send_completions (false);
}

int zlink::socket_base_t::drain_send_completions ()
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    if (pending.public_async_depth.load (std::memory_order_acquire) != 0)
        return 0;
    {
        scoped_lock_t lock (pending.sync);
        if (!pending.completion_head)
            return 0;
    }
    return dispatch_send_completions (false);
}

//  Run the completion callback for every resolved record and return the
//  number actually invoked. One socket never dispatches two completions
//  concurrently: the callback scope serialises them, exactly as the removed
//  readiness dispatch loop did.
int zlink::socket_base_t::dispatch_send_completions (bool closing_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    if (!closing_
        && pending.public_async_depth.load (std::memory_order_acquire) != 0)
        return 0;

    int dispatched = 0;
    while (true) {
        send_pending_record_t *record = NULL;
        zlink_send_complete_handler_fn handler = NULL;
        void *userdata = NULL;
        {
            scoped_lock_t lock (pending.sync);
            if (!pending.completion_head)
                return dispatched;
            handler = pending.handler;
            userdata = pending.handler_userdata;
            if (!handler)
                return dispatched;
            record = pending.completion_head;
            pending.completion_head = record->completion_next;
            if (!pending.completion_head)
                pending.completion_tail = NULL;
            record->completion_next = NULL;
        }

        zlink_send_complete_event_t event;
        memset (&event, 0, sizeof (event));
        event.op_id = record->op_id;
        event.userdata = record->userdata;
        copy_routing_id_from_bytes (record->target.peer_rid.data (),
                                    record->target.peer_rid.size (),
                                    &event.peer_rid);
        event.transport_pair_id = record->target.transport_pair_id;
        event.transport_pair_generation =
          record->target.transport_pair_generation;
        event.result = record->completion_result;
        event.terminal_errno = record->completion_errno;

        if (closing_) {
            socket_send_complete_dispatch_scope_t dispatch_scope (this);
            handler (public_handle (), &event, userdata);
            ++dispatched;
            destroy_send_pending_record (record);
            continue;
        }

        bool close_requested = false;
        {
            socket_callback_scope_t callback_scope (this);
            if (!callback_scope.acquired ()) {
                //  Close won the lifecycle race after this completion was
                //  popped. Put the allocation-free node back at the front so
                //  the closing dispatcher delivers it exactly once.
                scoped_lock_t lock (pending.sync);
                record->completion_next = pending.completion_head;
                pending.completion_head = record;
                if (!pending.completion_tail)
                    pending.completion_tail = record;
                return dispatched;
            }
            socket_send_complete_dispatch_scope_t dispatch_scope (this);
            handler (public_handle (), &event, userdata);
            ++dispatched;
            close_requested = lifecycle_coordinator ().public_close_requested ();
        }
        destroy_send_pending_record (record);
        if (close_requested)
            return dispatched;
    }
}

void zlink::socket_base_t::notify_send_pending_writable (pipe_t *pipe_)
{
    LIBZLINK_UNUSED (pipe_);
    if (!send_complete_handler_active ())
        return;
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
    notify_send_pending_writable (NULL);
}

void zlink::socket_base_t::fail_send_pending_for_target (
  const zlink_routing_id_t *peer_rid_, uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_, int terminal_errno_,
  uint64_t route_incarnation_id_)
{
    const bool pair_default_target =
      options.type == ZLINK_CORE_SOCKET_PAIR && peer_rid_
      && peer_rid_->size == 0 && transport_pair_id_ == 0
      && transport_pair_generation_ == 0 && route_incarnation_id_ == 0;
    if (!send_complete_handler_active ()
        || (!pair_default_target && !valid_routing_id (peer_rid_)))
        return;

    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    {
        scoped_lock_t lock (pending.sync);
        std::map<routed_send_target_key_t,
                 send_inline_attempt_state_t>::iterator attempt =
          pending.inline_attempts.begin ();
        while (attempt != pending.inline_attempts.end ()) {
            if (!send_target_matches (
                  attempt->first, peer_rid_, transport_pair_id_,
                  transport_pair_generation_, route_incarnation_id_)) {
                ++attempt;
                continue;
            }
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
    bool completed_any = false;
    while (true) {
        send_pending_record_t *doomed = NULL;
        {
            scoped_lock_t lock (pending.sync);
            for (std::map<routed_send_target_key_t,
                          std::deque<send_pending_record_t *> >::iterator queue =
                   pending.queues.begin ();
                 queue != pending.queues.end () && !doomed; ++queue) {
                const routed_send_target_key_t &candidate = queue->first;
                if (!send_target_matches (
                      candidate, peer_rid_, transport_pair_id_,
                      transport_pair_generation_, route_incarnation_id_))
                    continue;
                //  Submit order is preserved on the failure path too.
                for (std::deque<send_pending_record_t *>::iterator it =
                       queue->second.begin ();
                     it != queue->second.end (); ++it) {
                    if ((*it)->claimed) {
                        if ((*it)->deferred_terminal_errno == 0)
                            (*it)->deferred_terminal_errno = terminal_errno_;
                        continue;
                    }
                    (*it)->claimed = true;
                    doomed = *it;
                    break;
                }
            }
        }
        if (!doomed)
            break;
        finish_send_pending (doomed, ZLINK_SEND_TERMINAL, terminal_errno_);
        completed_any = true;
#ifdef ZLINK_BUILD_TESTS
        socket_send_pending_runtime_t::target_failure_progress_hook_fn hook =
          NULL;
        void *hook_userdata = NULL;
        {
            scoped_lock_t lock (pending.sync);
            hook = pending.target_failure_progress_hook;
            hook_userdata = pending.target_failure_progress_hook_userdata;
        }
        // Deterministically expose the inter-iteration publication window. The
        // hook runs without pending/route locks, matching a concurrent submit.
        if (hook)
            hook (hook_userdata);
#endif
    }
    if (completed_any)
        dispatch_send_completions_if_local ();
}

void zlink::socket_base_t::fail_send_pending_for_pipe (pipe_t *pipe_,
                                                        int terminal_errno_)
{
    if (!pipe_ || !send_complete_handler_active ())
        return;
    if (pipe_->get_transport_lane () == transport_lane_completion)
        return;

    // PAIR has one implicit destination and therefore queues async records
    // under the default key; its pipe does not need (and normally has no)
    // routing id. Terminate that queue directly on peer detach.
    if (options.type == ZLINK_CORE_SOCKET_PAIR) {
        zlink_routing_id_t default_rid;
        memset (&default_rid, 0, sizeof (default_rid));
        fail_send_pending_for_target (&default_rid, 0, 0, terminal_errno_);
        return;
    }

    const blob_t &routing_id = pipe_->get_routing_id ();
    if (routing_id.size () == 0)
        return;

    uint64_t pair_id = pipe_->get_transport_pair_id ();
    uint64_t pair_generation = pipe_->get_transport_pair_generation ();
    uint64_t route_incarnation_id = 0;
    //  STREAM peers do not negotiate a completion lane, so their stable
    //  transport connection id is the exact target identity - the same
    //  substitution STREAM select and STREAM exact submit use.
    if (options.type == ZLINK_CORE_SOCKET_STREAM && pair_id == 0) {
        pair_id = pipe_->get_transport_connection_id ();
        pair_generation = pair_id == 0 ? 0 : 1;
    } else if (options.type == ZLINK_CORE_SOCKET_ROUTER && pair_id == 0) {
        route_incarnation_id = pipe_->get_route_incarnation_id ();
    }
    zlink_routing_id_t peer_rid;
    memset (&peer_rid, 0, sizeof (peer_rid));
    if (routing_id.size () != 0)
        copy_routing_id_from_bytes (routing_id.data (), routing_id.size (),
                                    &peer_rid);
    fail_send_pending_for_target (&peer_rid, pair_id, pair_generation,
                                  terminal_errno_, route_incarnation_id);
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
