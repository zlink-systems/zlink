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
        total += size > minimum_frame_charge ? size : minimum_frame_charge;
    }
    return total;
}

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

bool zlink::socket_base_t::has_send_pending () const
{
    const socket_send_pending_runtime_t &pending = send_pending_runtime ();
    scoped_lock_t lock (pending.sync);
    return !pending.by_op.empty () || !pending.completions.empty ();
}

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

namespace
{
void send_pending_deadline_cleanup (void *userdata_)
{
    delete static_cast<
      std::pair<zlink::socket_base_t *, zlink_send_op_id_t> *> (userdata_);
}
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
        pending.pending_msgs =
          pending.pending_msgs > 0 ? pending.pending_msgs - 1 : 0;
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

    //  One scope for the whole record. This is the structural reason the
    //  array form exists: the per-handle send sequence is taken and released
    //  inside a single call, so a multipart record can never hold it across
    //  application code.
    std::unique_ptr<socket_public_send_scope_t> scope =
      begin_public_send_scope (true);
    if (!scope) {
        //  A lifecycle refusal (close admitted, context terminated) is not a
        //  route failure. Keep the record pending so the close / term path
        //  owns the terminal cause instead of reporting ESHUTDOWN here.
        errno = EAGAIN;
        return -1;
    }

    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    if (record_->has_target)
        copy_routing_id_from_bytes (record_->target.peer_rid.data (),
                                    record_->target.peer_rid.size (), &rid);

    const size_t count = record_->parts.size ();
    for (size_t i = 0; i != count; ++i) {
        const int flags =
          ZLINK_DONTWAIT | (i + 1 < count ? ZLINK_SNDMORE : 0);
        msg_t *msg = reinterpret_cast<msg_t *> (&record_->parts[i]);
        //  A routed target selects and pins the application pipe at the
        //  beginning of a logical record.  The socket's xsend path owns the
        //  continuation state after that first part: ROUTER keeps
        //  `_current_out`/`_more_out`, while DEALER keeps the load-balancer's
        //  multipart pipe.  Calling xsend_routed again for a continuation
        //  would either trip ROUTER's message-start assertion or make
        //  DEALER reject the part as EFSM.  Continue through the ordinary
        //  xsend path so the whole record remains one gated sequence.
        const bool routed_start = record_->has_target && i == 0;
        const int rc =
          routed_start
            ? send_direct_with_retry (&rid, msg, flags, *scope, NULL, 0, false,
                                      NULL, record_->target.transport_pair_id,
                                      record_->target
                                        .transport_pair_generation)
            : send_direct_with_retry (NULL, msg, flags, *scope);
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
        (void) rollback_scoped (*scope);
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

    std::set<routed_send_target_key_t> blocked;
    while (true) {
        send_pending_record_t *record = NULL;
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
        }
        if (!record)
            break;

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

int zlink::socket_base_t::send_async_submit (
  zlink_msg_t *parts_, size_t part_count_,
  const zlink_send_async_options_t *options_, zlink_send_op_id_t *op_id_out_)
{
    if (op_id_out_)
        *op_id_out_ = 0;

    if (!parts_ || part_count_ == 0 || !options_
        || options_->struct_size != sizeof (zlink_send_async_options_t)) {
        errno = EINVAL;
        return -1;
    }
    if (!socket_type_supports_send_async (options.type)) {
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
    if (!send_complete_handler_active ()) {
        //  Without a completion channel the operation could never report its
        //  own outcome, which would strand the caller's suspension forever.
        errno = EINVAL;
        return -1;
    }
    //  A completion callback hands the result to application state and
    //  returns. Submitting from inside one is the retry loop this design
    //  removed, so it is refused rather than silently supported.
    if (socket_send_complete_dispatch_scope_t::dispatching_any ()) {
        errno = EDEADLK;
        return -1;
    }
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    socket_public_api_scope_t admission (lifecycle);
    if (!admission.acquired ())
        return -1;

    //  Resolve the exact target now. Deferring the choice to completion time
    //  would make per-target FIFO order impossible to state.
    routed_send_target_key_t target;
    bool has_target = false;
    if (options.type == ZLINK_CORE_SOCKET_ROUTER
        || options.type == ZLINK_CORE_SOCKET_STREAM
        || options.type == ZLINK_CORE_SOCKET_DEALER) {
        zlink_routed_submit_target_t resolved;
        memset (&resolved, 0, sizeof (resolved));
        if (options_->target) {
            resolved = *options_->target;
        } else {
            if (options.type != ZLINK_CORE_SOCKET_DEALER) {
                errno = EINVAL;
                return -1;
            }
            if (process_commands (0, true) != 0)
                return -1;
            if (xselect_routed_submit_target (NULL, &resolved) != 0)
                return -1;
        }
        if (!valid_routing_id (&resolved.peer_rid)
            || resolved.transport_pair_id == 0
            || resolved.transport_pair_generation == 0) {
            errno = EINVAL;
            return -1;
        }
        target = routed_send_target_key_t (
          resolved.peer_rid.data, resolved.peer_rid.size,
          resolved.transport_pair_id, resolved.transport_pair_generation);
        has_target = true;
    }

    const uint64_t charge = record_charge_bytes (parts_, part_count_);
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    send_pending_record_t *record = NULL;
    zlink_send_op_id_t op_id = 0;
    {
        scoped_lock_t lock (pending.sync);
        if (pending.failing) {
            errno = ESHUTDOWN;
            return -1;
        }
        //  Bounded pending is the whole point: an unbounded reservation queue
        //  is a high-water mark bypass wearing a different name. Overflow is
        //  reported to the caller, which is where the application owns the
        //  policy decision.
        const uint64_t max_msgs = options.send_pending_max_msgs;
        const uint64_t max_bytes = options.send_pending_max_bytes;
        if (pending.pending_msgs + 1 > max_msgs
            || (max_bytes > 0 && pending.pending_bytes + charge > max_bytes)) {
            errno = EAGAIN;
            return -1;
        }

        record = new (std::nothrow) send_pending_record_t ();
        if (!record) {
            errno = ENOMEM;
            return -1;
        }
        record->op_id = pending.next_op_id++;
        record->userdata = options_->userdata;
        record->target = target;
        record->has_target = has_target;
        record->charge_bytes = charge;
        record->timeout_ms = options_->timeout_ms;
        record->parts.assign (parts_, parts_ + part_count_);
        //  Ownership transfer happens here and is irreversible: from this
        //  point Core closes the parts, never the caller.
        pending.queues[target].push_back (record);
        pending.by_op[record->op_id] = record;
        pending.pending_msgs += 1;
        pending.pending_bytes += charge;
        op_id = record->op_id;
    }
    //  `record` is no longer safe to dereference: the moment the pending
    //  mutex was released, the admit loop or the deadline thread could have
    //  completed this operation and destroyed it. Everything below uses the
    //  op id, which is monotonic and never reused.

    //  The caller's message handles are now Core's copies. Blank the caller's
    //  array so a stray close on its side cannot double-free the frames.
    for (size_t i = 0; i != part_count_; ++i) {
        const int rc = reinterpret_cast<msg_t *> (&parts_[i])->init ();
        errno_assert (rc == 0);
    }
    if (op_id_out_)
        *op_id_out_ = op_id;

    if (options_->timeout_ms > 0) {
        std::pair<socket_base_t *, zlink_send_op_id_t> *entry =
          new (std::nothrow)
            std::pair<socket_base_t *, zlink_send_op_id_t> (this, op_id);
        if (entry) {
            std::shared_ptr<request_timeout::task_t> task =
              request_timeout::schedule (options_->timeout_ms,
                                         &send_pending_deadline_trampoline,
                                         entry,
                                         &send_pending_deadline_cleanup);
            scoped_lock_t lock (pending.sync);
            std::map<zlink_send_op_id_t, send_pending_record_t *>::iterator it =
              pending.by_op.find (op_id);
            if (it != pending.by_op.end ())
                it->second->deadline = task;
            else if (task)
                request_timeout::cancel (task);
        }
    }

    //  Fast path: try the physical submit on the calling thread. When the
    //  target has room this costs zero thread hops, and the completion runs
    //  inline before this call returns.
    drive_send_pending ();
    dispatch_send_completions_if_local ();
    return 0;
}

int zlink::socket_base_t::send_async_cancel (zlink_send_op_id_t op_id_)
{
    if (op_id_ == 0) {
        errno = EINVAL;
        return -1;
    }
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    send_pending_record_t *record = NULL;
    {
        scoped_lock_t lock (pending.sync);
        std::map<zlink_send_op_id_t, send_pending_record_t *>::iterator it =
          pending.by_op.find (op_id_);
        if (it == pending.by_op.end ()) {
            errno = ENOENT;
            return -1;
        }
        if (it->second->claimed) {
            //  Admission already owns the record and cannot be rolled back.
            //  The completion still fires exactly once, as ADMITTED.
            errno = EBUSY;
            return -1;
        }
        it->second->claimed = true;
        record = it->second;
    }
    //  A cancelled operation still completes: silence here would strand every
    //  binding suspension that maps drop/cancel onto this call.
    finish_send_pending (record, ZLINK_SEND_TERMINAL, ECANCELED);
    dispatch_send_completions_if_local ();
    return 0;
}
