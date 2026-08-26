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

bool zlink::socket_base_t::try_send_async_routed_single_immediate (
  const zlink_routing_id_t *target_rid_, zlink_msg_t *part_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    if (pending.pending_msgs.load (std::memory_order_acquire) != 0)
        return false;

    socket_public_send_scope_t admission (lifecycle_coordinator (), true);
    if (!admission.acquired ()
        || pending.pending_msgs.load (std::memory_order_acquire) != 0)
        return false;

    return send_routed_scoped (
             target_rid_, reinterpret_cast<msg_t *> (part_), ZLINK_DONTWAIT,
             admission)
           == 0;
}

bool zlink::socket_base_t::has_send_pending () const
{
    const socket_send_pending_runtime_t &pending = send_pending_runtime ();
    scoped_lock_t lock (pending.sync);
    return !pending.by_op.empty () || !pending.completions.empty ();
}

#ifdef ZLINK_BUILD_TESTS
void zlink::socket_base_t::test_set_send_pending_gate_release_hook (
  socket_send_pending_runtime_t::gate_release_hook_fn hook_, void *userdata_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    scoped_lock_t lock (pending.sync);
    pending.gate_release_hook = hook_;
    pending.gate_release_hook_userdata = userdata_;
}

void zlink::socket_base_t::test_set_send_next_op_id (
  zlink_send_op_id_t next_op_id_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    scoped_lock_t lock (pending.sync);
    zlink_assert (pending.by_op.empty ());
    pending.next_op_id = next_op_id_;
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
    //  The scheduler owns a raw pair (socket, op id). The op id is monotonic
    //  and never reused, so a completion that already fired simply finds no
    //  record and the expiry is a no-op.
    std::pair<socket_base_t *, zlink_send_op_id_t> *entry =
      static_cast<std::pair<socket_base_t *, zlink_send_op_id_t> *> (userdata_);
    if (!entry)
        return;
    entry->first->on_send_pending_deadline (entry->second);
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

        send_complete_record_t completion;
        completion.op_id = record_->op_id;
        completion.userdata = record_->userdata;
        completion.target = record_->target;
        completion.result = result_;
        completion.terminal_errno =
          result_ == ZLINK_SEND_ADMITTED ? 0 : terminal_errno_;
        pending.completions.push_back (completion);
        deadline.swap (record_->deadline);
    }
    if (deadline)
        request_timeout::cancel (deadline);
    destroy_send_pending_record (record_);
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

    return try_admit_send_parts (record_->parts.data (), record_->parts.size (),
                                 record_->target, record_->has_target);
}

int zlink::socket_base_t::try_admit_send_parts (
  zlink_msg_t *parts_, size_t part_count_,
  const routed_send_target_key_t &target_, bool has_target_)
{
    if (!parts_ || part_count_ == 0) {
        errno = EFAULT;
        return -1;
    }

    //  One scope for the whole record. This is the structural reason the
    //  array form exists: the per-handle send sequence is taken and released
    //  inside a single call, so a multipart record can never hold it across
    //  application code.
    socket_public_send_scope_t scope (lifecycle_coordinator (), true);
    if (!scope.acquired ()) {
        //  A lifecycle refusal (close admitted, context terminated) is not a
        //  route failure. Keep the record pending so the close / term path
        //  owns the terminal cause instead of reporting ESHUTDOWN here.
        errno = EAGAIN;
        return -1;
    }

    return try_admit_send_parts_scoped (parts_, part_count_, target_,
                                        has_target_, scope);
}

int zlink::socket_base_t::try_admit_send_parts_scoped (
  zlink_msg_t *parts_, size_t part_count_,
  const routed_send_target_key_t &target_, bool has_target_,
  socket_public_send_scope_t &scope, pipe_t **attempted_pipe_out_)
{
    if (!parts_ || part_count_ == 0 || !scope.acquired ()) {
        errno = EFAULT;
        return -1;
    }

    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    if (has_target_)
        copy_routing_id_from_bytes (target_.peer_rid.data (),
                                    target_.peer_rid.size (), &rid);

    const size_t count = part_count_;
    for (size_t i = 0; i != count; ++i) {
        const int flags =
          ZLINK_DONTWAIT | (i + 1 < count ? ZLINK_SNDMORE : 0);
        msg_t *msg = reinterpret_cast<msg_t *> (&parts_[i]);
        //  A routed target selects and pins the application pipe at the
        //  beginning of a logical record.  The socket's xsend path owns the
        //  continuation state after that first part: ROUTER keeps
        //  `_current_out`/`_more_out`, while DEALER keeps the load-balancer's
        //  multipart pipe.  Calling xsend_routed again for a continuation
        //  would either trip ROUTER's message-start assertion or make
        //  DEALER reject the part as EFSM.  Continue through the ordinary
        //  xsend path so the whole record remains one gated sequence.
        const bool routed_start = has_target_ && i == 0;
        const int rc =
          routed_start
            ? send_direct_with_retry (&rid, msg, flags, scope, NULL, 0, false,
                                      attempted_pipe_out_,
                                      target_.transport_pair_id,
                                      target_.transport_pair_generation)
            : send_direct_with_retry (NULL, msg, flags, scope);
        if (rc == 0)
            continue;

        const int failure_errno = errno;
        if (i == 0) {
            //  Nothing reached the pipe, so the record is untouched.
            //  Backpressure keeps it reserved; a route failure completes it
            //  with that cause. Lifecycle refusals are the one exception:
            //  close and context termination fail the whole pending set with
            //  their own cause, so reporting ESHUTDOWN/ETERM from this
            //  attempt would only race that path to a worse answer.
            //
            //  A route failure must stay terminal here. Retrying a dead exact
            //  target on the mailbox thread would re-enter the public send
            //  scope on every wake, and a concurrent close would never win
            //  the lifecycle gate.
            errno = failure_errno == ESHUTDOWN || failure_errno == ETERM
                      ? EAGAIN
                      : failure_errno;
            return -1;
        }
        //  A later part failed after the message start was accepted. The byte
        //  HWM is only tested at the message start, so this is a route
        //  failure rather than backpressure. Drop the half-written message so
        //  the peer never sees a truncated record.
        (void) rollback_scoped (scope);
        errno = failure_errno == EAGAIN ? ECONNABORTED : failure_errno;
        return -1;
    }
    return 0;
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
    while (true) {
        send_pending_record_t *record = NULL;
        uint64_t scanned_enqueue_epoch = 0;
        {
            scoped_lock_t lock (pending.sync);
            for (std::map<routed_send_target_key_t,
                          std::deque<send_pending_record_t *> >::iterator it =
                   pending.queues.begin ();
                 it != pending.queues.end (); ++it) {
                if (it->second.empty () || blocked.count (it->first) != 0)
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
            if (pending.gate_release_hook)
                pending.gate_release_hook (
                  pending.gate_release_hook_userdata);
#endif
            pending.admission_gate.store (false, std::memory_order_release);

            //  A submitter that published before the gate release observed
            //  the gate as owned and returned from its drive attempt. Retake
            //  ownership when its epoch is newer; otherwise either no work
            //  arrived or a submitter arriving after this check will acquire
            //  the now-free gate itself.
            if (pending.enqueue_epoch.load (std::memory_order_acquire)
                != scanned_enqueue_epoch) {
                bool reacquire_expected = false;
                if (pending.admission_gate.compare_exchange_strong (
                      reacquire_expected, true, std::memory_order_acq_rel,
                      std::memory_order_acquire))
                    continue;
            }
            return;
        }

        const int rc = try_admit_send_pending (record);
        if (rc == 0) {
            finish_send_pending (record, ZLINK_SEND_ADMITTED, 0);
            continue;
        }
        const int failure_errno = errno;
        if (failure_errno == EAGAIN) {
            scoped_lock_t lock (pending.sync);
            record->claimed = false;
            blocked.insert (record->target);
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
    if (_completion_poller_refs.load (std::memory_order_acquire) != 0)
        return;
    dispatch_send_completions (false);
}

int zlink::socket_base_t::drain_send_completions ()
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    {
        scoped_lock_t lock (pending.sync);
        if (pending.completions.empty ())
            return 0;
    }
    dispatch_send_completions (false);
    return 1;
}

//  Run the completion callback for every resolved record. One socket never
//  dispatches two completions concurrently: the callback scope serialises
//  them, exactly as the removed readiness dispatch loop did.
void zlink::socket_base_t::dispatch_send_completions (bool closing_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    while (true) {
        send_complete_record_t record;
        zlink_send_complete_handler_fn handler = NULL;
        void *userdata = NULL;
        {
            scoped_lock_t lock (pending.sync);
            if (pending.completions.empty ())
                return;
            handler = pending.handler;
            userdata = pending.handler_userdata;
            if (!handler)
                return;
            record = pending.completions.front ();
            pending.completions.pop_front ();
        }

        zlink_send_complete_event_t event;
        memset (&event, 0, sizeof (event));
        event.op_id = record.op_id;
        event.userdata = record.userdata;
        copy_routing_id_from_bytes (record.target.peer_rid.data (),
                                    record.target.peer_rid.size (),
                                    &event.peer_rid);
        event.transport_pair_id = record.target.transport_pair_id;
        event.transport_pair_generation =
          record.target.transport_pair_generation;
        event.result = record.result;
        event.terminal_errno = record.terminal_errno;

        if (closing_) {
            socket_send_complete_dispatch_scope_t dispatch_scope (this);
            handler (this, &event, userdata);
            continue;
        }

        bool close_requested = false;
        {
            socket_callback_scope_t callback_scope (this);
            if (!callback_scope.acquired ())
                return;
            socket_send_complete_dispatch_scope_t dispatch_scope (this);
            handler (this, &event, userdata);
            close_requested = lifecycle_coordinator ().public_close_requested ();
        }
        if (close_requested)
            return;
    }
}

void zlink::socket_base_t::notify_send_pending_writable (pipe_t *pipe_)
{
    LIBZLINK_UNUSED (pipe_);
    if (!send_complete_handler_active ())
        return;
    {
        const socket_send_pending_runtime_t &pending = send_pending_runtime ();
        scoped_lock_t lock (pending.sync);
        if (pending.by_op.empty ())
            return;
    }
    command_t wake;
    memset (&wake, 0, sizeof (wake));
    wake.destination = this;
    wake.type = command_t::send_pending;
    static_cast<mailbox_t *> (_mailbox)->send (wake);
}

void zlink::socket_base_t::fail_send_pending_for_target (
  const zlink_routing_id_t *peer_rid_, uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_, int terminal_errno_)
{
    if (!send_complete_handler_active () || !valid_routing_id (peer_rid_))
        return;

    const routed_send_target_key_t key (peer_rid_->data, peer_rid_->size,
                                        transport_pair_id_,
                                        transport_pair_generation_);
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    std::vector<send_pending_record_t *> doomed;
    {
        scoped_lock_t lock (pending.sync);
        std::map<routed_send_target_key_t,
                 std::deque<send_pending_record_t *> >::iterator queue =
          pending.queues.find (key);
        if (queue == pending.queues.end ())
            return;
        //  Submit order is preserved on the failure path too: the whole
        //  target queue fails in the order it was submitted.
        for (std::deque<send_pending_record_t *>::iterator it =
               queue->second.begin ();
             it != queue->second.end (); ++it) {
            if ((*it)->claimed)
                continue;
            (*it)->claimed = true;
            doomed.push_back (*it);
        }
    }
    for (size_t i = 0; i != doomed.size (); ++i)
        finish_send_pending (doomed[i], ZLINK_SEND_TERMINAL, terminal_errno_);
    if (!doomed.empty ())
        dispatch_send_completions_if_local ();
}

void zlink::socket_base_t::fail_send_pending_for_pipe (pipe_t *pipe_,
                                                        int terminal_errno_)
{
    if (!pipe_ || !send_complete_handler_active ())
        return;
    if (pipe_->get_transport_lane () == transport_lane_completion)
        return;

    const blob_t &routing_id = pipe_->get_routing_id ();
    if (routing_id.size () == 0)
        return;

    uint64_t pair_id = pipe_->get_transport_pair_id ();
    uint64_t pair_generation = pipe_->get_transport_pair_generation ();
    //  STREAM peers do not negotiate a completion lane, so their stable
    //  transport connection id is the exact target identity - the same
    //  substitution STREAM select and STREAM exact submit use.
    if (options.type == ZLINK_CORE_SOCKET_STREAM && pair_id == 0) {
        pair_id = pipe_->get_transport_connection_id ();
        pair_generation = pair_id == 0 ? 0 : 1;
    }
    zlink_routing_id_t peer_rid;
    copy_routing_id_from_bytes (routing_id.data (), routing_id.size (),
                                &peer_rid);
    fail_send_pending_for_target (&peer_rid, pair_id, pair_generation,
                                  terminal_errno_);
}

//  close / ctx term: fail fast. LINGER covers bytes already admitted to a
//  pipe; a pending record has not been admitted, so draining it here would
//  make close wait on the peer's consumption rate.
void zlink::socket_base_t::fail_all_send_pending (int terminal_errno_)
{
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    std::vector<send_pending_record_t *> doomed;
    {
        scoped_lock_t lock (pending.sync);
        pending.failing = true;
        for (std::map<zlink_send_op_id_t, send_pending_record_t *>::iterator
               it = pending.by_op.begin ();
             it != pending.by_op.end (); ++it) {
            if (it->second->claimed)
                continue;
            it->second->claimed = true;
            doomed.push_back (it->second);
        }
    }
    for (size_t i = 0; i != doomed.size (); ++i)
        finish_send_pending (doomed[i], ZLINK_SEND_TERMINAL, terminal_errno_);
}
