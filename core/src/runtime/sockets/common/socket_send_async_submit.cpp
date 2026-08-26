/* SPDX-License-Identifier: MPL-2.0 */

//  Public asynchronous send submission and cancellation. Queue driving and
//  completion dispatch live in socket_send_complete.cpp; this module owns the
//  API-side validation and message ownership transfer into that queue.

#include "utils/precompiled.hpp"

#include "api/socket/request_timeout_scheduler_internal.hpp"
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
        total += size > minimum_frame_charge ? size : minimum_frame_charge;
    }
    return total;
}

void send_pending_deadline_cleanup (void *userdata_)
{
    delete static_cast<
      std::pair<zlink::socket_base_t *, zlink_send_op_id_t> *> (userdata_);
}
}

int zlink::socket_base_t::send_async_submit (
  zlink_msg_t *parts_, size_t part_count_,
  const zlink_send_async_options_t *options_, zlink_send_op_id_t *op_id_out_)
{
    if (op_id_out_)
        *op_id_out_ = 0;

    // Admit at the common public boundary before send-async-specific
    // validation. A callback on one socket must receive EDEADLK even when it
    // targets another socket whose completion handler is not installed yet.
    socket_lifecycle_coordinator_t &lifecycle = lifecycle_coordinator ();
    socket_public_send_scope_t admission (lifecycle, true);
    if (!admission.acquired ())
        return -1;

    if (!parts_ || part_count_ == 0 || !options_
        || options_->struct_size != sizeof (zlink_send_async_options_t)) {
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
    if (!send_complete_handler_active ()) {
        //  Without a completion channel the operation could never report its
        //  own outcome, which would strand the caller's suspension forever.
        errno = EINVAL;
        return -1;
    }
    if (unlikely (_ctx_terminated)) {
        errno = ETERM;
        return -1;
    }

    //  Resolve the exact target now. Deferring the choice to completion time
    //  would make per-target FIFO order impossible to state.
    routed_send_target_key_t target;
    bool has_target = false;
    bool deferred_target_select = false;
    if (options.type == ZLINK_CORE_SOCKET_ROUTER
        || options.type == ZLINK_CORE_SOCKET_STREAM
        || options.type == ZLINK_CORE_SOCKET_DEALER) {
        zlink_routed_submit_target_t resolved;
        memset (&resolved, 0, sizeof (resolved));
        if (options_->target) {
            resolved = *options_->target;
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
                if (options.type == ZLINK_CORE_SOCKET_ROUTER
                    && part_count_ == 1) {
                    deferred_target_select = true;
                } else {
                    if (process_commands (0, true) != 0)
                        return -1;
                    const zlink_routing_id_t requested_rid = resolved.peer_rid;
                    if (xselect_routed_submit_target (&requested_rid, &resolved)
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
            if (xselect_routed_submit_target (NULL, &resolved) != 0)
                return -1;
        }
        if (!valid_routing_id (&resolved.peer_rid)
            || (!deferred_target_select
                && (resolved.transport_pair_id == 0
                    || resolved.transport_pair_generation == 0))) {
            errno = EINVAL;
            return -1;
        }
        if (!deferred_target_select)
            target = routed_send_target_key_t (
              resolved.peer_rid.data, resolved.peer_rid.size,
              resolved.transport_pair_id, resolved.transport_pair_generation);
        else
            target = routed_send_target_key_t (
              resolved.peer_rid.data, resolved.peer_rid.size, 0, 0);
        has_target = true;
    }
    socket_send_pending_runtime_t &pending = send_pending_runtime ();
    bool attempt_inline = false;
    bool gate_acquired = false;
    const routed_send_target_key_t inline_reservation_key = target;
    bool target_reserved = false;
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
        if (target_queue_empty
            && pending.inline_attempts.insert (inline_reservation_key).second)
            target_reserved = true;
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
            pending.inline_attempts.erase (inline_reservation_key);
        }
    }
    if (!attempt_inline)
        admission.unlock_sync ();
    if (deferred_target_select && !attempt_inline) {
        zlink_routed_submit_target_t resolved;
        zlink_routing_id_t requested_rid;
        memset (&resolved, 0, sizeof (resolved));
        memset (&requested_rid, 0, sizeof (requested_rid));
        requested_rid = options_->target->peer_rid;
        if (select_routed_submit_target (&requested_rid, &resolved) != 0)
            return -1;
        target = routed_send_target_key_t (
          resolved.peer_rid.data, resolved.peer_rid.size,
          resolved.transport_pair_id, resolved.transport_pair_generation);
        deferred_target_select = false;
    }

    //  Preserve the old binding-owned admission fast path: when no operation
    //  is already ahead of this target, attempt admission before allocating a
    //  pending record. Success is reported by op_id == 0 and never generates
    //  a completion callback. The caller can therefore complete an awaitable
    //  locally without entering Core's callback dispatch scope.
    std::optional<send_pending_record_t> inline_record;
    const bool borrowed_inline = attempt_inline && part_count_ == 1;
    const unsigned char borrowed_original_flags =
      borrowed_inline
        ? reinterpret_cast<msg_t *> (parts_)->flags ()
        : 0;
    int inline_terminal_errno = 0;
    if (attempt_inline) {
        if (!borrowed_inline) {
            inline_record.emplace ();
            inline_record->target = target;
            inline_record->has_target = has_target;
            try {
                inline_record->parts.assign (parts_, parts_ + part_count_);
            } catch (...) {
                pending.admission_gate.store (false,
                                              std::memory_order_release);
                {
                    scoped_lock_t lock (pending.sync);
                    pending.inline_attempts.erase (inline_reservation_key);
                }
                admission.unlock_sync ();
                drive_send_pending ();
                dispatch_send_completions_if_local ();
                errno = ENOMEM;
                return -1;
            }
        }
        pipe_t *attempted_pipe = NULL;
        const int inline_rc = borrowed_inline && deferred_target_select
                                ? send_routed_scoped (
                                    &options_->target->peer_rid,
                                    reinterpret_cast<msg_t *> (parts_),
                                    ZLINK_DONTWAIT, admission, NULL, 0,
                                    &attempted_pipe)
                                : borrowed_inline
                                  ? try_admit_send_parts_scoped (
                                      parts_, part_count_, target, has_target,
                                      admission)
                                  : try_admit_send_parts_scoped (
                                      inline_record->parts.data (),
                                      inline_record->parts.size (), target,
                                      has_target, admission);
        const int inline_errno = errno;
        const uint64_t attempted_pair_id =
          attempted_pipe ? attempted_pipe->get_transport_pair_id () : 0;
        const uint64_t attempted_pair_generation =
          attempted_pipe ? attempted_pipe->get_transport_pair_generation () : 0;
        admission.unlock_sync ();
        if (inline_rc == 0) {
            pending.admission_gate.store (false, std::memory_order_release);
            {
                scoped_lock_t lock (pending.sync);
                pending.inline_attempts.erase (inline_reservation_key);
            }
            if (!borrowed_inline) {
                //  The pipe now owns its frame references. Close Core's moved
                //  handles and blank the caller's handles exactly as the
                //  pending ownership-transfer path does.
                for (size_t i = 0; i != inline_record->parts.size (); ++i) {
                    msg_t *msg = reinterpret_cast<msg_t *> (
                      &inline_record->parts[i]);
                    if (msg->check ()) {
                        const int rc = msg->close ();
                        errno_assert (rc == 0);
                    }
                }
                inline_record->parts.clear ();
                for (size_t i = 0; i != part_count_; ++i) {
                    const int rc =
                      reinterpret_cast<msg_t *> (&parts_[i])->init ();
                    errno_assert (rc == 0);
                }
            }
            //  Operations queued behind the direct attempt may also fit now.
            if (pending.pending_msgs.load (std::memory_order_acquire) != 0) {
                drive_send_pending ();
                dispatch_send_completions_if_local ();
            }
            return 0;
        }

        if (deferred_target_select) {
            if (!attempted_pipe) {
                pending.admission_gate.store (false,
                                              std::memory_order_release);
                {
                    scoped_lock_t lock (pending.sync);
                    pending.inline_attempts.erase (inline_reservation_key);
                }
                msg_t *borrowed = reinterpret_cast<msg_t *> (parts_);
                borrowed->reset_flags (msg_t::more);
                if (borrowed_original_flags & msg_t::more)
                    borrowed->set_flags (msg_t::more);
                errno = inline_errno;
                return -1;
            }
            target = routed_send_target_key_t (
              options_->target->peer_rid.data,
              options_->target->peer_rid.size, attempted_pair_id,
              attempted_pair_generation);
            deferred_target_select = false;
        }

        if (inline_errno != EAGAIN) {
            //  A multipart attempt can fail after its first part reached the
            //  pipe and was rolled back. Preserve the accepted-operation
            //  ownership rule for every post-validation attempt: publish a
            //  non-zero operation id and report this terminal through the
            //  completion channel instead of returning borrowed handles whose
            //  internal state may already have participated in a send.
            inline_terminal_errno = inline_errno;
        }
    }

    const uint64_t charge = record_charge_bytes (parts_, part_count_);
    send_pending_record_t *record =
      new (std::nothrow) send_pending_record_t ();
    zlink_send_op_id_t op_id = 0;
    int pending_reject_errno = record ? 0 : ENOMEM;
    if (record) {
        record->userdata = options_->userdata;
        record->target = target;
        record->has_target = has_target;
        record->charge_bytes = charge;
        record->timeout_ms = options_->timeout_ms;
        record->claimed = inline_terminal_errno != 0;
        try {
            if (attempt_inline && !borrowed_inline)
                record->parts.swap (inline_record->parts);
            else
                record->parts.assign (parts_, parts_ + part_count_);
        } catch (...) {
            pending_reject_errno = ENOMEM;
        }
    }

    //  Reserve queue capacity only after the pending record is completely
    //  prepared. This keeps allocation failure outside the synchronized
    //  runtime state and makes rejection leave caller ownership intact.
    if (pending_reject_errno == 0) {
        scoped_lock_t lock (pending.sync);
        if (inline_terminal_errno == 0 && pending.failing) {
            pending_reject_errno = ESHUTDOWN;
        } else {
            //  Async admission waits by default, matching the former
            //  binding-owned queues. A non-zero option is an explicit
            //  application overload policy; zero means unlimited and normal
            //  HWM pressure never becomes an immediate submit failure.
            const uint64_t max_msgs = options.send_pending_max_msgs;
            const uint64_t max_bytes = options.send_pending_max_bytes;
            if (inline_terminal_errno == 0
                && ((max_msgs > 0
                     && pending.pending_msgs.load (std::memory_order_relaxed)
                          + 1
                          > max_msgs)
                    || (max_bytes > 0
                        && pending.pending_bytes + charge > max_bytes))) {
                pending_reject_errno = EAGAIN;
            } else if (pending.next_op_id == 0) {
                //  Operation id zero permanently denotes immediate admission.
                //  Once the socket-local uint64 sequence wraps, refuse new
                //  pending ownership instead of publishing an unobservable
                //  operation that cannot be cancelled or correlated.
                pending_reject_errno = EOVERFLOW;
            } else {
                record->op_id = pending.next_op_id++;
                //  Ownership transfer happens here and is irreversible: from
                //  this point Core closes the parts, never the caller. Keep
                //  the inline reservation until the original operation is at
                //  the front, so a later submit cannot overtake it between
                //  EAGAIN and queue insertion.
                if (attempt_inline)
                    pending.queues[target].push_front (record);
                else
                    pending.queues[target].push_back (record);
                pending.by_op[record->op_id] = record;
                pending.pending_msgs.fetch_add (1, std::memory_order_release);
                pending.pending_bytes += charge;
                pending.enqueue_epoch.fetch_add (1, std::memory_order_release);
                op_id = record->op_id;
            }
        }
        if (attempt_inline)
            pending.inline_attempts.erase (inline_reservation_key);
    }

    if (attempt_inline) {
        {
            scoped_lock_t lock (pending.sync);
            pending.inline_attempts.erase (inline_reservation_key);
        }
        pending.admission_gate.store (false, std::memory_order_release);
    }

    if (pending_reject_errno != 0) {
        //  No ownership transfer occurred on rejection. Release the target
        //  reservation and let already-queued followers make progress.  The
        //  borrowed fast attempt normalizes the native MORE flag, so restore
        //  that flag before returning caller ownership.
        if (borrowed_inline) {
            msg_t *borrowed = reinterpret_cast<msg_t *> (parts_);
            borrowed->reset_flags (msg_t::more);
            if (borrowed_original_flags & msg_t::more)
                borrowed->set_flags (msg_t::more);
        }
        if (inline_record)
            inline_record->parts.clear ();
        if (record) {
            record->parts.clear ();
            delete record;
            record = NULL;
        }
        drive_send_pending ();
        dispatch_send_completions_if_local ();
        errno = pending_reject_errno;
        return -1;
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

    if (inline_terminal_errno != 0) {
        finish_send_pending (record, ZLINK_SEND_TERMINAL,
                             inline_terminal_errno);
        dispatch_send_completions_if_local ();
        return 0;
    }

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

    //  A non-zero operation id is the pending disposition. Core now owns the
    //  record and completes it exactly once after admission, timeout, cancel
    //  or termination. The immediate-admission path returned op_id == 0 and
    //  intentionally produced no callback.
    //
    //  Retry once after publishing the record so a writable edge racing the
    //  initial EAGAIN cannot be lost between the direct attempt and queue
    //  insertion. If it admits here, it is still a pending operation and its
    //  callback may run before this function returns.
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
            //  Another resolver already owns the record and cannot be rolled
            //  back. It still completes exactly once, normally as ADMITTED
            //  but possibly as TERMINAL if route resolution is failing.
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
