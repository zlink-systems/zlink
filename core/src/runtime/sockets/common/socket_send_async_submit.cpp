/* SPDX-License-Identifier: MPL-2.0 */

//  Public asynchronous send submission and cancellation. Queue driving and
//  completion dispatch live in socket_send_complete.cpp; this module owns the
//  API-side validation and message ownership transfer into that queue.

#include "utils/precompiled.hpp"

#include "api/socket/request_timeout_scheduler_internal.hpp"
#include "core/pipe.hpp"
#include "sockets/common/socket_base.hpp"
#include "sockets/stream/stream_dispatch_internal.hpp"
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

void close_owned_parts (std::vector<zlink_msg_t> *parts_)
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

int copy_owned_parts (zlink_msg_t *parts_,
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
        close_owned_parts (copies_);
        errno = saved_errno;
        return -1;
    }

    for (size_t i = 0; i != part_count_; ++i) {
        zlink::msg_t *copy =
          reinterpret_cast<zlink::msg_t *> (&(*copies_)[i]);
        zlink::msg_t *source = reinterpret_cast<zlink::msg_t *> (&parts_[i]);
        if (copy->copy (*source) != 0) {
            const int saved_errno = errno;
            close_owned_parts (copies_);
            errno = saved_errno;
            return -1;
        }
    }
    return 0;
}

bool stream_rid_matches (const zlink_routing_id_t &rid_, uint32_t value_)
{
    return rid_.size == 4
           && rid_.data[0] == static_cast<uint8_t> (value_ >> 24)
           && rid_.data[1] == static_cast<uint8_t> ((value_ >> 16) & 0xff)
           && rid_.data[2] == static_cast<uint8_t> ((value_ >> 8) & 0xff)
           && rid_.data[3] == static_cast<uint8_t> (value_ & 0xff);
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
    bool stream_fast_eagain = false;
    bool stream_fast_reserved = false;
    inline_attempt_map_t::iterator stream_fast_attempt =
      pending.inline_attempts.end ();
    std::vector<zlink_msg_t> stream_fast_parts;
    const auto release_stream_fast_reservation = [&] () -> bool {
        bool need_redrive = false;
        scoped_lock_t lock (pending.sync);
        if (stream_fast_attempt != pending.inline_attempts.end ()
            && stream_fast_attempt->second.active) {
            stream_fast_attempt->second.active = false;
            if (!stream_fast_attempt->second.retained
                || stream_fast_attempt->second.retire) {
                pending.inline_attempts.erase (stream_fast_attempt);
                stream_fast_attempt = pending.inline_attempts.end ();
            }
            if (pending.pending_msgs.load (std::memory_order_relaxed) != 0) {
                const std::map<
                  routed_send_target_key_t,
                  std::deque<send_pending_record_t *> >::const_iterator queue =
                  pending.queues.find (target);
                need_redrive = queue != pending.queues.end ()
                               && !queue->second.empty ();
            }
            if (need_redrive)
                pending.redrive_epoch.fetch_add (1,
                                                 std::memory_order_release);
        }
        stream_fast_reserved = false;
        return need_redrive;
    };
    const auto redrive_stream_pending = [&] () {
        drive_send_pending ();
    };

    // A STREAM packet callback already owns a stable current pipe and exact
    // RID. Reserve that target before the direct attempt. The current-pipe
    // try-write preserves caller ownership on EAGAIN, so copy the part only
    // when Core actually has to retain pending work.
    if (options.type == ZLINK_CORE_SOCKET_STREAM && options_->target
        && valid_routing_id (&options_->target->peer_rid)
        && stream_dispatch_in_callback ()
        && stream_dispatch_context_t::in_packet_callback ()) {
        const uint32_t current_routing_id =
          stream_dispatch_context_t::current_routing_id ();
        pipe_t *const dispatch_pipe =
          stream_dispatch_context_t::current_pipe ();
        pipe_t *const direct_out =
          dispatch_pipe ? dispatch_pipe->get_peer () : NULL;
        if (current_routing_id != 0 && direct_out
            && stream_rid_matches (options_->target->peer_rid,
                                   current_routing_id)) {
            uint64_t pair_id = direct_out->get_transport_pair_id ();
            uint64_t pair_generation =
              direct_out->get_transport_pair_generation ();
            if (pair_id == 0) {
                pair_id = direct_out->get_transport_connection_id ();
                pair_generation = pair_id == 0 ? 0 : 1;
            }
            const bool target_unspecified =
              options_->target->transport_pair_id == 0
              && options_->target->transport_pair_generation == 0;
            const bool target_matches =
              options_->target->transport_pair_id == pair_id
              && options_->target->transport_pair_generation
                   == pair_generation;
            if (pair_id != 0 && pair_generation != 0
                && (target_unspecified || target_matches)) {
                try {
                    target = routed_send_target_key_t (
                      options_->target->peer_rid.data,
                      options_->target->peer_rid.size, pair_id,
                      pair_generation);
                } catch (...) {
                    errno = ENOMEM;
                    return -1;
                }

                {
                    scoped_lock_t lock (pending.sync);
                    if (pending.failing) {
                        errno = ESHUTDOWN;
                        return -1;
                    }
                    bool target_queue_empty =
                      pending.pending_msgs.load (std::memory_order_relaxed)
                      == 0;
                    if (!target_queue_empty) {
                        const std::map<
                          routed_send_target_key_t,
                          std::deque<send_pending_record_t *> >::const_iterator
                          queue = pending.queues.find (target);
                        target_queue_empty =
                          queue == pending.queues.end ()
                          || queue->second.empty ();
                    }
                    if (target_queue_empty) {
                        try {
                            stream_fast_reserved =
                              reserve_inline_attempt_locked (
                                target, true, &stream_fast_attempt);
                        } catch (...) {
                            errno = ENOMEM;
                            return -1;
                        }
                    }
                }
                if (stream_fast_reserved) {
                    admission.unlock_sync ();
                    const int fast_rc =
                      stream_dispatch_send_current_msg_from_io (
                        reinterpret_cast<msg_t *> (&parts_[0]),
                        ZLINK_DONTWAIT);
                    const int fast_errno = errno;
                    if (fast_rc >= 0) {
                        // A follower published before this release is visible
                        // under the same mutex and needs an explicit wake. A
                        // follower published afterwards drives itself, while
                        // unrelated targets do not need a global rescan.
                        if (release_stream_fast_reservation ())
                            redrive_stream_pending ();
                        return 0;
                    }
                    if (fast_errno == EAGAIN) {
                        // The current-pipe try-write does not consume its
                        // message on EAGAIN. Only pay for the ownership shadow
                        // when Core really has to retain pending work.
                        if (copy_owned_parts (parts_, 1, &stream_fast_parts)
                            != 0) {
                            if (release_stream_fast_reservation ())
                                redrive_stream_pending ();
                            errno = ENOMEM;
                            return -1;
                        }
                        stream_fast_eagain = true;
                        has_target = true;
                    } else {
                        if (release_stream_fast_reservation ())
                            redrive_stream_pending ();
                        errno = fast_errno;
                        return -1;
                    }
                }
            }
        }
    }

    //  Resolve the exact target now. Deferring the choice to completion time
    //  would make per-target FIFO order impossible to state.
    try {
        if (stream_fast_eagain) {
            // Exact current-pipe identity was already snapshotted above.
        } else if (options.type == ZLINK_CORE_SOCKET_ROUTER
                   || options.type == ZLINK_CORE_SOCKET_STREAM
                   || options.type == ZLINK_CORE_SOCKET_DEALER) {
            zlink_routed_submit_target_t resolved;
            uint64_t resolved_connection_id = 0;
            uint64_t resolved_route_incarnation_id = 0;
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
                        if (xselect_routed_submit_target_internal (
                              &requested_rid, &resolved,
                              &resolved_connection_id,
                              &resolved_route_incarnation_id)
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
            const bool resolved_pair = resolved.transport_pair_id != 0
                                       && resolved.transport_pair_generation != 0;
            const bool resolved_unpaired = resolved.transport_pair_id == 0
                                           && resolved.transport_pair_generation == 0
                                           && resolved_connection_id != 0
                                           && resolved_route_incarnation_id != 0;
            if (!valid_routing_id (&resolved.peer_rid)
                || (!deferred_target_select && !resolved_pair
                    && !resolved_unpaired)) {
                errno = EINVAL;
                return -1;
            }
            if (!deferred_target_select)
                target = routed_send_target_key_t (
                  resolved.peer_rid.data, resolved.peer_rid.size,
                  resolved.transport_pair_id,
                  resolved.transport_pair_generation,
                  resolved_unpaired ? resolved_route_incarnation_id : 0);
            else
                target = routed_send_target_key_t (
                  resolved.peer_rid.data, resolved.peer_rid.size, 0, 0);
            has_target = true;
        }
    } catch (...) {
        if (stream_fast_reserved) {
            const bool need_redrive =
              release_stream_fast_reservation ();
            close_owned_parts (&stream_fast_parts);
            admission.unlock_sync ();
            if (need_redrive)
                redrive_stream_pending ();
        }
        errno = ENOMEM;
        return -1;
    }
    bool attempt_inline = stream_fast_eagain;
    bool gate_acquired = false;
    routed_send_target_key_t inline_reservation_key;
    try {
        inline_reservation_key = target;
    } catch (...) {
        if (stream_fast_eagain) {
            const bool need_redrive =
              release_stream_fast_reservation ();
            close_owned_parts (&stream_fast_parts);
            if (need_redrive)
                redrive_stream_pending ();
        }
        errno = ENOMEM;
        return -1;
    }
    bool target_reserved = false;
    bool reservation_allocation_failed = false;
    if (!stream_fast_eagain) {
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
        requested_rid = options_->target->peer_rid;
        if (select_routed_submit_target_internal (
              &requested_rid, &resolved, &resolved_connection_id,
              &resolved_route_incarnation_id)
            != 0)
            return -1;
        try {
            target = routed_send_target_key_t (
              resolved.peer_rid.data, resolved.peer_rid.size,
              resolved.transport_pair_id, resolved.transport_pair_generation,
              resolved.transport_pair_id == 0
                ? resolved_route_incarnation_id
                : 0);
        } catch (...) {
            errno = ENOMEM;
            return -1;
        }
        deferred_target_select = false;
    }

    //  Preserve the old binding-owned admission fast path: when no operation
    //  is already ahead of this target, attempt admission before allocating a
    //  pending record. Success is reported by op_id == 0 and never generates
    //  a completion callback. The caller can therefore complete an awaitable
    //  locally without entering Core's callback dispatch scope.
    std::optional<send_pending_record_t> inline_record;
    int inline_terminal_errno = 0;
    bool inline_record_owns_copies = false;
    int inline_errno = EAGAIN;
    routed_send_attempt_identity_t attempted_identity;
    if (attempt_inline) {
        if (stream_fast_eagain) {
            try {
                inline_record.emplace ();
                inline_record->target = target;
                inline_record->parts.swap (stream_fast_parts);
            } catch (...) {
                const bool need_redrive =
                  release_stream_fast_reservation ();
                close_owned_parts (&stream_fast_parts);
                if (need_redrive)
                    redrive_stream_pending ();
                errno = ENOMEM;
                return -1;
            }
            inline_record->has_target = true;
            inline_record_owns_copies = true;
        } else {
            //  The inline attempt must not borrow caller storage. A synchronous
            //  send consumes its native part on every ordinary failure, while
            //  an async submit that returns non-OK must preserve caller
            //  ownership. A real msg_t copy keeps those contracts compatible
            //  when a later queue/map allocation rejects the async operation.
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
                dispatch_send_completions_if_local ();
                errno = ENOMEM;
                return -1;
            }
            inline_record->has_target = has_target;
            if (copy_owned_parts (parts_, part_count_, &inline_record->parts)
                != 0) {
                pending.admission_gate.store (false,
                                              std::memory_order_release);
                {
                    scoped_lock_t lock (pending.sync);
                    release_inline_attempt_locked (
                      inline_reservation_key);
                }
                admission.unlock_sync ();
                drive_send_pending ();
                dispatch_send_completions_if_local ();
                errno = ENOMEM;
                return -1;
            }
            inline_record_owns_copies = true;
            socket_public_send_scope_t physical_admission (
              lifecycle, true, socket_send_admission_complete);
            int inline_rc = -1;
            if (physical_admission.acquired ()) {
                inline_rc = deferred_target_select
                              ? send_routed_scoped (
                                  &options_->target->peer_rid,
                                  reinterpret_cast<msg_t *> (
                                    inline_record->parts.data ()),
                                  ZLINK_DONTWAIT, physical_admission, NULL, 0,
                                  NULL, 0, 0, false, NULL, NULL,
                                  &attempted_identity)
                              : try_admit_send_parts_scoped (
                                  inline_record->parts.data (),
                                  inline_record->parts.size (), target,
                                  has_target, physical_admission);
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
                close_owned_parts (&inline_record->parts);
                inline_record_owns_copies = false;
                consume_caller_parts (parts_, part_count_);
                //  Operations queued behind the direct attempt may also fit.
                if (pending.pending_msgs.load (std::memory_order_acquire)
                    != 0) {
                    drive_send_pending ();
                    dispatch_send_completions_if_local ();
                }
                return 0;
            }
        }

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
                close_owned_parts (&inline_record->parts);
                inline_record_owns_copies = false;
                errno = inline_errno;
                return -1;
            }
            try {
                target = routed_send_target_key_t (
                  options_->target->peer_rid.data,
                  options_->target->peer_rid.size,
                  attempted_identity.transport_pair_id,
                  attempted_identity.transport_pair_generation,
                  attempted_unpaired
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
                close_owned_parts (&inline_record->parts);
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
#ifdef ZLINK_BUILD_TESTS
        else if (pending.inline_fallback_hook) {
            //  Hold the exact EAGAIN-to-queue-publication window open for a
            //  deterministic FIFO regression. The per-target reservation and
            //  admission gate must remain owned while the hook runs.
            pending.inline_fallback_hook (
              pending.inline_fallback_hook_userdata);
        }
#endif
    }

    const uint64_t charge = record_charge_bytes (parts_, part_count_);
    send_pending_record_t *record =
      new (std::nothrow) send_pending_record_t ();
    bool record_owns_copies = false;
    zlink_send_op_id_t op_id = 0;
    int pending_reject_errno = record ? 0 : ENOMEM;
    if (record) {
        record->userdata = options_->userdata;
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
        record->timeout_ms = options_->timeout_ms;
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
            // Route replacement and pending publication form one ordered step.
            // If the physical attempt belonged to a route already retired by
            // direct I/O handover, accept it as a terminal async operation
            // instead of publishing work that no later detach can identify.
            if (inline_terminal_errno == 0 && has_target
                && !xsend_pending_target_current_locked (record->target)) {
                inline_terminal_errno = ENOTCONN;
                record->claimed = true;
            }
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
#ifdef ZLINK_BUILD_TESTS
                    if (pending.fail_after_queue_push) {
                        pending_reject_errno = ENOMEM;
                    } else
#endif
                    {
                        indexed = pending.by_op.insert (
                          std::make_pair (candidate_op_id, record)).second;
                    }
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
                    pending.pending_bytes += charge;
                    pending.enqueue_epoch.fetch_add (
                      1, std::memory_order_release);
                    op_id = candidate_op_id;
                }
            }
        }
        if (attempt_inline) {
            const bool released =
              release_inline_attempt_locked (inline_reservation_key);
            if (stream_fast_reserved && released)
                pending.redrive_epoch.fetch_add (1,
                                                 std::memory_order_release);
        }
    }
    if (target_publication_lock.owns_lock ())
        target_publication_lock.unlock ();

    if (attempt_inline) {
        {
            scoped_lock_t lock (pending.sync);
            const bool released =
              release_inline_attempt_locked (inline_reservation_key);
            if (stream_fast_reserved && released)
                pending.redrive_epoch.fetch_add (1,
                                                 std::memory_order_release);
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
                close_owned_parts (&inline_record->parts);
            else
                inline_record->parts.clear ();
        }
        if (record) {
            if (record_owns_copies)
                close_owned_parts (&record->parts);
            else
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
        finish_send_pending (record, ZLINK_SEND_TERMINAL,
                             inline_terminal_errno);
        dispatch_send_completions_if_local ();
        return 0;
    }

    bool timeout_setup_failed = false;
    if (options_->timeout_ms > 0) {
        std::pair<socket_base_t *, zlink_send_op_id_t> *entry =
          new (std::nothrow)
            std::pair<socket_base_t *, zlink_send_op_id_t> (this, op_id);
        if (!entry) {
            timeout_setup_failed = true;
        } else {
            std::shared_ptr<request_timeout::task_t> task =
              request_timeout::schedule (options_->timeout_ms,
                                         &send_pending_deadline_trampoline,
                                         entry,
                                         &send_pending_deadline_cleanup);
            if (!task) {
                timeout_setup_failed = true;
            } else {
                bool cancel_task = false;
                {
                    scoped_lock_t lock (pending.sync);
                    std::map<zlink_send_op_id_t,
                             send_pending_record_t *>::iterator it =
                      pending.by_op.find (op_id);
                    if (it != pending.by_op.end ())
                        it->second->deadline = task;
                    else
                        cancel_task = true;
                }
                //  cancel() can wait for a firing deadline whose handler
                //  takes pending.sync, so it must run after releasing that
                //  mutex.
                if (cancel_task)
                    request_timeout::cancel (task);
            }
        }
    }

    if (timeout_setup_failed) {
        send_pending_record_t *failed = NULL;
        {
            scoped_lock_t lock (pending.sync);
            std::map<zlink_send_op_id_t, send_pending_record_t *>::iterator it =
              pending.by_op.find (op_id);
            if (it != pending.by_op.end () && !it->second->claimed) {
                failed = it->second;
                failed->claimed = true;
            }
        }
        //  Submission has already transferred ownership and published its
        //  operation id. Failure to create the requested deadline therefore
        //  resolves that admitted operation through the normal completion
        //  channel instead of silently creating an unbounded wait.
        if (failed)
            finish_send_pending (failed, ZLINK_SEND_TERMINAL, ENOMEM);
        dispatch_send_completions_if_local ();
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
