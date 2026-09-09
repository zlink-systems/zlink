/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include <stdio.h>
#include <new>
#include <stddef.h>

#include "utils/macros.hpp"
#include "utils/config.hpp"
#include "core/pipe_internal.hpp"
#include "core/transport_pair_policy.hpp"
#include "core/ctx.hpp"
#include "core/flow_state_frame.hpp"
#include "protocol/zmp_protocol.hpp"
#include "protocol/zmp_peer_weight.hpp"
#include "utils/err.hpp"
#include "utils/debug_log.hpp"
#include "utils/heap_owner.hpp"
#include "core/ypipe.hpp"
#include "core/ypipe_conflate.hpp"

using zlink::pipe_detail::consume_if_delimiter;
using zlink::pipe_detail::head_reclassify_armed;
using zlink::pipe_detail::head_reclassify_idle;
using zlink::pipe_detail::head_reclassify_queued;
using zlink::pipe_detail::pipe_debug_log;

namespace
{
struct normalized_head_probe_t
{
    zlink::pipe_normalized_head_kind_t kind;
};

void probe_normalized_head (const zlink::msg_t &msg_, void *userdata_)
{
    normalized_head_probe_t *const probe =
      static_cast<normalized_head_probe_t *> (userdata_);

    // Core control frames share the count-1 Application FIFO with public
    // DATA/REQUEST and socket-local REPLY traffic. Classify them before
    // request/reply metadata so they are drained internally at a record
    // boundary and never activated on the public receive path.
    if ((msg_.flags () & zlink::msg_t::command) != 0) {
        probe->kind = zlink::pipe_head_control;
        return;
    }

    unsigned char wire_kind = zlink::zmp_kind_data;
    uint64_t request_sequence = 0;
    if (!msg_.get_request_reply_metadata (&wire_kind, &request_sequence)) {
        probe->kind = zlink::pipe_head_data;
        return;
    }

    if (request_sequence == 0) {
        probe->kind = zlink::pipe_head_invalid;
        return;
    }

    switch (wire_kind) {
        case zlink::zmp_kind_request:
            probe->kind = zlink::pipe_head_request;
            break;
        case zlink::zmp_kind_reply:
            probe->kind = zlink::pipe_head_reply;
            break;
        case zlink::zmp_kind_error_reply:
            probe->kind = zlink::pipe_head_error_reply;
            break;
        default:
            probe->kind = zlink::pipe_head_invalid;
            break;
    }
}
}

bool zlink::pipe_t::check_read ()
{
    // Hot path: PAIR/DEALER steady-state recv reaches this path for every
    // message. Any extra locking or bookkeeping here shows up directly in
    // small-message throughput.
    const lifecycle_state_t state = _state.load (std::memory_order_acquire);
    if (unlikely (state != active && state != waiting_for_delimiter))
        return false;
    if (unlikely (!_in_active.load (std::memory_order_acquire))) {
        // Recover from a missed read-activation edge if data is already
        // visible in the inbound pipe.
        if (!_in_pipe->check_read ())
            return false;
        _in_active.store (true, std::memory_order_release);
    }

    // Inspect and consume a delimiter in one queue operation. In particular,
    // a conflate writer cannot replace the inspected frame before it is read.
    msg_t delimiter;
    const ypipe_read_result_t delimiter_result =
      _in_pipe->read_if (&delimiter, &consume_if_delimiter, NULL);
    if (delimiter_result == ypipe_read_empty) {
        _in_active.store (false, std::memory_order_release);
        return false;
    }
    if (delimiter_result == ypipe_read_consumed) {
        process_delimiter ();
        return false;
    }

    return true;
}

zlink::pipe_normalized_head_kind_t
zlink::pipe_t::probe_normalized_head_kind ()
{
    const lifecycle_state_t state = _state.load (std::memory_order_acquire);
    if (unlikely (state != active && state != waiting_for_delimiter))
        return pipe_head_empty;

    // Arm before observing publication. If the writer publishes first, the
    // non-sleeping probe below sees it. If it publishes later, flush changes
    // this marker to queued and sends one explicit activation.
    _head_reclassify_wake.store (head_reclassify_armed,
                                 std::memory_order_seq_cst);

    normalized_head_probe_t probe = {pipe_head_invalid};
    if (!_in_pipe->probe_if_published (&probe_normalized_head, &probe))
        return pipe_head_empty;

    _in_active.store (true, std::memory_order_release);
    _head_reclassify_wake.store (head_reclassify_idle,
                                 std::memory_order_release);
    return probe.kind;
}

bool zlink::pipe_t::read (msg_t *msg_)
{
    return read_internal<false> (msg_, NULL, NULL, NULL, NULL);
}

bool zlink::pipe_t::requires_record_admission (const msg_t &msg_)
{
    const bool candidate = (msg_.flags () & msg_t::more) != 0
                           || msg_.get_request_reply_metadata (NULL, NULL);
    if (!candidate)
        return false;

    return !msg_.is_credential () && !msg_.is_routing_id ()
           && !msg_.is_delimiter ();
}

bool zlink::pipe_t::read_with_record_admission (
  msg_t *msg_, read_admission_fn *admission_, void *userdata_,
  bool *admission_failed_out_, bool *admission_consumed_out_)
{
    return read_internal<true> (msg_, admission_, userdata_,
                                admission_failed_out_,
                                admission_consumed_out_);
}

namespace
{
struct pipe_read_admission_probe_t
{
    zlink::pipe_t *pipe;
    zlink::pipe_t::read_admission_fn *admission;
    void *userdata;
    int result;
    int error_number;
};

bool invoke_pipe_record_admission (const zlink::msg_t &msg_, void *userdata_)
{
    pipe_read_admission_probe_t *const probe =
      static_cast<pipe_read_admission_probe_t *> (userdata_);
    if (!zlink::pipe_t::requires_record_admission (msg_)) {
        probe->result = 0;
        return true;
    }
    probe->result = probe->admission (probe->pipe, msg_, probe->userdata);
    probe->error_number = errno;
    return probe->result == 0
           || probe->result == zlink::pipe_t::read_admission_reject_consume;
}

}

template <bool WithAdmission>
bool zlink::pipe_t::read_internal (msg_t *msg_,
                                   read_admission_fn *admission_,
                                   void *userdata_,
                                   bool *admission_failed_out_,
                                   bool *admission_consumed_out_)
{
    const lifecycle_state_t state = _state.load (std::memory_order_acquire);
    if (unlikely (state != active && state != waiting_for_delimiter))
        return false;
    if (WithAdmission) {
        if (admission_failed_out_)
            *admission_failed_out_ = false;
        if (admission_consumed_out_)
            *admission_consumed_out_ = false;
    }
    if (unlikely (!_in_active.load (std::memory_order_acquire))) {
        if (!_in_pipe->check_read ())
            return false;
        _in_active.store (true, std::memory_order_release);
    }

    bool prefetched_batch_exhausted = false;
    while (true) {
        prefetched_batch_exhausted = false;
        // Raw terminal and private bookkeeping frames do not need a
        // whole-record lease. Classify and conditionally dequeue them in the
        // same queue operation that runs admission for record starts.
        if (WithAdmission && admission_) {
            pipe_read_admission_probe_t probe = {this, admission_, userdata_, 0,
                                                 errno};
            const ypipe_read_result_t read_result =
              _in_pipe->read_if (msg_, &invoke_pipe_record_admission, &probe,
                                 &prefetched_batch_exhausted);
            if (read_result != ypipe_read_consumed) {
                if (probe.result != 0 && admission_failed_out_)
                    *admission_failed_out_ = true;
                if (read_result == ypipe_read_empty)
                    _in_active.store (false, std::memory_order_release);
                errno = probe.error_number;
                return false;
            }
            if (probe.result == read_admission_reject_consume) {
                // A negative two is a terminal admission error (for example
                // lazy state allocation). The conditional queue read consumed
                // it atomically with the admission decision, unlike EAGAIN
                // capacity rejection, which leaves the frame queued.
                const int saved_errno = probe.error_number;
                if (_registry_accounting) {
                    const uint64_t frame_bytes = frame_accounted_bytes (msg_);
                    const uint64_t counted_messages =
                      counted_pending_message_ref (*msg_);
                    get_ctx ()->_physical_queue_registry.release_committed_frame (
                      _in_physical_queue, frame_bytes, counted_messages);
                }
                account_inbound_frame (msg_, prefetched_batch_exhausted);
                if (admission_failed_out_)
                    *admission_failed_out_ = true;
                if (admission_consumed_out_)
                    *admission_consumed_out_ = true;
                errno = saved_errno;
                return true;
            }
        } else if (!_in_pipe->read (msg_, &prefetched_batch_exhausted)) {
            _in_active.store (false, std::memory_order_release);
            return false;
        }

        //  If this is a credential, ignore it and receive next message.
        if (unlikely (msg_->is_credential ())) {
            if (_registry_accounting)
                get_ctx ()->_physical_queue_registry.release_committed_frame (
                  _in_physical_queue, frame_accounted_bytes (msg_),
                  counted_pending_message_ref (*msg_));
            account_inbound_frame (msg_, prefetched_batch_exhausted);
            const int rc = msg_->close ();
            zlink_assert (rc == 0);
        } else {
            break;
        }
    }

    //  If delimiter was read, start termination process of the pipe.
    if (msg_->is_delimiter ()) {
        process_delimiter ();
        return false;
    }

    if (_registry_accounting) {
        const uint64_t frame_bytes = frame_accounted_bytes (msg_);
        const uint64_t counted_messages = counted_pending_message_ref (*msg_);
        get_ctx ()->_physical_queue_registry.release_committed_frame (
          _in_physical_queue, frame_bytes, counted_messages);
    }
    account_inbound_frame (msg_, prefetched_batch_exhausted);

    return true;
}

int zlink::pipe_t::reserve_inbound_decoder_frame (
  uint64_t payload_bytes_, unsigned char msg_flags_, bool track_multipart_,
  decoder_frame_reservation_t *reservation_storage_,
  decoder_frame_reservation_t **reservation_out_)
{
    if (!reservation_out_) {
        errno = EFAULT;
        return -1;
    }
    *reservation_out_ = NULL;

    scoped_optional_lock_t lock (_session_pipe ? NULL : &_out_sync);
    if (_state != active || !_out_physical_queue) {
        errno = ETERM;
        return -1;
    }
    const uint64_t hwm = _hwm.load (std::memory_order_acquire);

    uint64_t bytes_written =
      _bytes_written.load (std::memory_order_acquire);
    uint64_t peers_bytes_read =
      _peers_bytes_read.load (std::memory_order_acquire);
    bool multipart_started_empty = track_multipart_
      && (_decoder_multipart_started_empty
          || (_out_incomplete_bytes == 0
              && bytes_written <= peers_bytes_read));

    const uint64_t frame_bytes =
      payload_bytes_ > UINT64_MAX - static_cast<uint64_t> (sizeof (msg_t))
        ? UINT64_MAX
        : payload_bytes_ + static_cast<uint64_t> (sizeof (msg_t));
    const uint64_t candidate_bytes =
      frame_bytes == UINT64_MAX
          || _out_incomplete_bytes > UINT64_MAX - frame_bytes
        ? UINT64_MAX
        : _out_incomplete_bytes + frame_bytes;
    const bool more = (msg_flags_ & msg_t::more) != 0;
    bool allow_empty_exception =
      !more && multipart_started_empty;
    const bool byte_credit_ready =
      hwm == 0 || allow_empty_exception
      || (candidate_bytes != UINT64_MAX
          && (bytes_written <= peers_bytes_read
                ? candidate_bytes <= hwm
                : bytes_written - peers_bytes_read <= hwm
                    && candidate_bytes
                         <= hwm - (bytes_written - peers_bytes_read)));
    if (!byte_credit_ready) {
        refresh_peer_credit_snapshot_unlocked ();
        bytes_written = _bytes_written.load (std::memory_order_acquire);
        peers_bytes_read =
          _peers_bytes_read.load (std::memory_order_acquire);
        if (track_multipart_ && !multipart_started_empty
            && _out_incomplete_bytes == 0
            && bytes_written <= peers_bytes_read) {
            multipart_started_empty = true;
            allow_empty_exception = !more;
        }
        const uint64_t in_flight =
          bytes_written > peers_bytes_read
            ? bytes_written - peers_bytes_read
            : 0;
        if (hwm > 0 && !allow_empty_exception
            && (candidate_bytes == UINT64_MAX || in_flight > hwm
                || candidate_bytes > hwm - in_flight)) {
            arm_hwm_credit_wait_unlocked ();
            refresh_peer_credit_snapshot_unlocked ();
            bytes_written =
              _bytes_written.load (std::memory_order_acquire);
            peers_bytes_read =
              _peers_bytes_read.load (std::memory_order_acquire);
            const uint64_t refreshed_in_flight =
              bytes_written > peers_bytes_read
                ? bytes_written - peers_bytes_read
                : 0;
            const bool refreshed_ready =
              candidate_bytes != UINT64_MAX
              && refreshed_in_flight <= hwm
              && candidate_bytes <= hwm - refreshed_in_flight;
            if (refreshed_ready)
                clear_hwm_credit_wait_unlocked ();
            else {
                errno = EAGAIN;
                return -1;
            }
        }
    }

    int rc = 0;
    if (!_registry_accounting) {
        if (reservation_storage_->active) {
            errno = EBUSY;
            return -1;
        }
        reservation_storage_->queue_id = 0;
        reservation_storage_->generation = _out_generation;
        reservation_storage_->frame_bytes = frame_bytes;
        reservation_storage_->payload_bytes = payload_bytes_;
        reservation_storage_->msg_flags = msg_flags_;
        reservation_storage_->multipart_started_empty =
          multipart_started_empty;
        reservation_storage_->active = true;
        *reservation_out_ = reservation_storage_;
    } else {
        decoder_frame_reservation_request_t request;
        request.payload_bytes = payload_bytes_;
        request.msg_flags = msg_flags_;
        request.multipart_started_empty = multipart_started_empty;
        request.qualify_multipart_from_queue_state = false;
        rc = get_ctx ()->_physical_queue_registry.reserve_decoder_frame (
          _out_physical_queue, request, reservation_storage_, reservation_out_);
    }
    if (rc == 0) {
        if (track_multipart_ && (msg_flags_ & msg_t::more) != 0)
            _decoder_multipart_started_empty =
              multipart_started_empty;
        //  Admission normally stays writable for many frames. Publish the
        //  waiter transition only when credit recovery actually changes the
        //  writer state; rewriting the shared marker for every decoded frame
        //  creates avoidable reader-side cache traffic.
        bool expected = false;
        if (_out_active.compare_exchange_strong (
              expected, true, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
            _waiting_for_byte_credit.store (false,
                                             std::memory_order_release);
        }
        return 0;
    }

    if (errno != EAGAIN)
        return -1;

    //  Mark the exact writer as waiting before rechecking its physical
    //  direction. A credit return racing this transition will either observe
    //  the waiter or be visible to this second admission attempt.
    arm_hwm_credit_wait_unlocked ();
    refresh_peer_credit_snapshot_unlocked ();
    decoder_frame_reservation_request_t request;
    request.payload_bytes = payload_bytes_;
    request.msg_flags = msg_flags_;
    request.multipart_started_empty = multipart_started_empty;
    request.qualify_multipart_from_queue_state = false;
    rc = get_ctx ()->_physical_queue_registry.reserve_decoder_frame (
      _out_physical_queue, request, reservation_storage_, reservation_out_);
    if (rc == 0) {
        clear_hwm_credit_wait_unlocked ();
        if (track_multipart_ && (msg_flags_ & msg_t::more) != 0)
            _decoder_multipart_started_empty =
              multipart_started_empty;
    }
    return rc;
}

int zlink::pipe_t::write_reserved_decoder_frame (
  msg_t *msg_, decoder_frame_reservation_t **reservation_)
{
    if (!msg_ || !reservation_ || !*reservation_) {
        errno = EFAULT;
        return -1;
    }
    scoped_optional_lock_t lock (_session_pipe ? NULL : &_out_sync);
    if (_state != active || !_out_pipe) {
        get_ctx ()->_physical_queue_registry.release_decoder_frame (
          reservation_);
        errno = ETERM;
        return -1;
    }
    const uint64_t hwm = _hwm.load (std::memory_order_acquire);

    decoder_frame_reservation_t *const reserved = *reservation_;
    if (!_registry_accounting) {
        const uint64_t incomplete_before = _out_incomplete_bytes;
        *reservation_ = NULL;
        if (!reserved->active
            || reserved->payload_bytes != static_cast<uint64_t> (msg_->size ())
            || reserved->msg_flags != msg_->flags ()) {
            reserved->active = false;
            errno = EPROTO;
            return -1;
        }
        reserved->active = false;

        if (reserved->frame_bytes == UINT64_MAX
            || _out_incomplete_bytes
                 > UINT64_MAX - reserved->frame_bytes
            || _out_incomplete_payload_bytes
                 > UINT64_MAX - reserved->payload_bytes) {
            errno = EMSGSIZE;
            return -1;
        }
        _out_incomplete_bytes += reserved->frame_bytes;
        _out_incomplete_payload_bytes += reserved->payload_bytes;

        const bool more = (reserved->msg_flags & msg_t::more) != 0;
        const bool complete_frame = !more && !msg_->is_delimiter ();
        const uint64_t bytes_written =
          _bytes_written.load (std::memory_order_acquire);
        const uint64_t peers_bytes_read =
          _peers_bytes_read.load (std::memory_order_acquire);
        const uint64_t in_flight =
          bytes_written > peers_bytes_read
            ? bytes_written - peers_bytes_read
            : 0;
        const bool oversize =
          complete_frame && hwm > 0 && in_flight == 0
          && _out_incomplete_bytes > hwm;

        const ypipe_replacement_accounting_t replaced =
          publish_outbound_frame_unlocked (*msg_, more);
        if (complete_frame) {
            const uint64_t message_bytes = _out_incomplete_bytes;
            const uint64_t msgs_written =
              _msgs_written.load (std::memory_order_acquire);
            const uint64_t retained_bytes = bytes_written - replaced.bytes;
            const uint64_t new_bytes_written =
              UINT64_MAX - retained_bytes < message_bytes
                ? UINT64_MAX
                : retained_bytes + message_bytes;
            uint64_t new_msgs_written = msgs_written - replaced.complete_messages;
            if (!msg_->is_routing_id () && !msg_->is_credential ())
                ++new_msgs_written;
            publish_outbound_ledger_unlocked (new_msgs_written,
                                              new_bytes_written);
            _out_complete_record_pending = true;
            if (oversize)
                record_oversize_message_admission (message_bytes);
            _out_incomplete_bytes = 0;
            _out_incomplete_payload_bytes = 0;
            _out_multipart_started_empty = false;
            _decoder_multipart_started_empty = false;
        }
        publish_outbound_accounting_unlocked (
          more || incomplete_before != 0);
        return 0;
    }

    const uint64_t incomplete_before = _out_incomplete_bytes;
    const uint64_t payload_before = _out_incomplete_payload_bytes;
    const bool multipart_started_empty_before =
      _out_multipart_started_empty;
    if (!append_outbound_frame_bytes_unlocked (msg_)) {
        get_ctx ()->_physical_queue_registry.release_decoder_frame (
          reservation_);
        return -1;
    }
    const uint64_t payload_bytes = static_cast<uint64_t> (msg_->size ());
    if (UINT64_MAX - _out_incomplete_payload_bytes < payload_bytes) {
        _out_incomplete_bytes = incomplete_before;
        _out_incomplete_payload_bytes = payload_before;
        _out_multipart_started_empty = multipart_started_empty_before;
        get_ctx ()->_physical_queue_registry.release_decoder_frame (
          reservation_);
        errno = EMSGSIZE;
        return -1;
    }
    _out_incomplete_payload_bytes += payload_bytes;

    const bool more = (msg_->flags () & msg_t::more) != 0;
    const bool complete_frame = !more && !msg_->is_delimiter ();
    const uint64_t bytes_written =
      _bytes_written.load (std::memory_order_acquire);
    const uint64_t peers_bytes_read =
      _peers_bytes_read.load (std::memory_order_acquire);
    const uint64_t in_flight =
      bytes_written > peers_bytes_read
        ? bytes_written - peers_bytes_read
        : 0;
    bool oversize = complete_frame && hwm > 0 && in_flight == 0
                    && _out_incomplete_bytes > hwm;
    bool registry_oversize = false;
    const int commit_rc =
      get_ctx ()->_physical_queue_registry.commit_decoder_frame (
        _out_physical_queue, reservation_, payload_bytes, msg_->flags (),
        counted_pending_message_ref (*msg_), &registry_oversize);
    if (commit_rc != 0) {
        _out_incomplete_bytes = incomplete_before;
        _out_incomplete_payload_bytes = payload_before;
        _out_multipart_started_empty = multipart_started_empty_before;
        return -1;
    }
    oversize = oversize || registry_oversize;

    const ypipe_replacement_accounting_t replaced =
      publish_outbound_frame_unlocked (*msg_, more);
    if (complete_frame) {
        const uint64_t message_bytes = _out_incomplete_bytes;
        const uint64_t msgs_written =
          _msgs_written.load (std::memory_order_acquire);
        const uint64_t retained_bytes = bytes_written - replaced.bytes;
        const uint64_t new_bytes_written =
          UINT64_MAX - retained_bytes < message_bytes
            ? UINT64_MAX
            : retained_bytes + message_bytes;
        uint64_t new_msgs_written = msgs_written - replaced.complete_messages;
        if (!msg_->is_routing_id () && !msg_->is_credential ())
            ++new_msgs_written;
        publish_outbound_ledger_unlocked (new_msgs_written,
                                          new_bytes_written);
        _out_complete_record_pending = true;
        if (oversize)
            record_oversize_message_admission (message_bytes);
        _out_incomplete_bytes = 0;
        _out_incomplete_payload_bytes = 0;
        _out_multipart_started_empty = false;
        _decoder_multipart_started_empty = false;
    }
    publish_outbound_accounting_unlocked (
      more || incomplete_before != 0);
    return 0;
}

void zlink::pipe_t::release_decoder_frame_reservation (
  decoder_frame_reservation_t **reservation_)
{
    if (!reservation_ || !*reservation_)
        return;
    decoder_frame_reservation_t *const reservation = *reservation_;
    *reservation_ = NULL;
    reservation->active = false;
}

bool zlink::pipe_t::check_write ()
{
    return check_write_admission () == pipe_message_admission_ready;
}

bool zlink::pipe_t::try_reserve_request_correlation (
  uint64_t accounted_bytes_, uint64_t *release_epoch_out_)
{
    if (accounted_bytes_ == 0)
        accounted_bytes_ = 1;

    scoped_lock_t lock (_out_sync);
    if (_state != active) {
        errno = EHOSTUNREACH;
        return false;
    }
    if (_transport_pair_write_held || remote_flow_blocked_unlocked ()) {
        errno = EAGAIN;
        return false;
    }

    const bool overflow =
      UINT64_MAX - _request_correlation_bytes < accounted_bytes_;
    // Small requests use their lifecycle bytes directly. Above 1 KiB the
    // charge grows cubically so a large request cannot create the secure
    // completion backlog that a byte-linear window permits. The empty-pair
    // exception still admits one request larger than the work budget.
    const uint64_t work_charge =
      transport_pair_policy::request_correlation_work_charge (
        accounted_bytes_);
    const uint64_t work_budget =
      transport_pair_policy::request_correlation_work_budget;
    const bool work_overflow =
      UINT64_MAX - _request_correlation_work < work_charge;
    const bool work_window_empty = _request_correlation_work == 0
                                   && _request_correlation_count == 0;
    const bool work_full =
      !work_window_empty
      && (work_charge > work_budget
          || _request_correlation_work > work_budget - work_charge);
    const bool count_full =
      _request_correlation_count
      >= transport_pair_policy::request_correlation_count_budget;
    // Application HWM bounds frames still resident in the physical queue.
    // Correlation outlives that queue credit, so this independent work/count
    // window owns its admission after the peer has dequeued the request.
    if (overflow || work_overflow || work_full || count_full) {
        _request_correlation_waiting = true;
        if (release_epoch_out_)
            *release_epoch_out_ = request_correlation_release_epoch ();
        errno = ENOBUFS;
        return false;
    }

    _request_correlation_bytes += accounted_bytes_;
    _request_correlation_work += work_charge;
    ++_request_correlation_count;
    return true;
}

uint64_t zlink::pipe_t::request_correlation_release_epoch () const
{
    return _request_correlation_release_epoch.load (std::memory_order_acquire);
}

void zlink::pipe_t::release_request_correlation (uint64_t accounted_bytes_)
{
    bool schedule_activation = false;
    uint64_t generation = 0;
    uint64_t msgs_read = 0;
    uint64_t bytes_read = 0;
    {
        scoped_lock_t lock (_out_sync);
        zlink_assert (accounted_bytes_ <= _request_correlation_bytes);
        const uint64_t work_charge =
          transport_pair_policy::request_correlation_work_charge (
            accounted_bytes_);
        zlink_assert (work_charge <= _request_correlation_work);
        zlink_assert (_request_correlation_count != 0);
        _request_correlation_bytes -= accounted_bytes_;
        _request_correlation_work -= work_charge;
        --_request_correlation_count;
        if (_request_correlation_waiting) {
            // Only a reservation return advances this edge. Snapshotting it
            // under the same lock as refusal closes the token-registration race.
            _request_correlation_release_epoch.fetch_add (
              1, std::memory_order_release);
            _request_correlation_waiting = false;
            _request_correlation_activation_pending = true;
            generation = _out_generation;
            msgs_read =
              _peers_msgs_read.load (std::memory_order_acquire);
            bytes_read =
              _peers_bytes_read.load (std::memory_order_acquire);
            schedule_activation = true;
        }
    }
    if (schedule_activation)
        // Completion may run on the timeout scheduler. Always cross the pipe
        // mailbox so scheduler state is mutated only by the socket owner.
        send_activate_write_deferred (this, generation, msgs_read, bytes_read);
}


void zlink::pipe_t::account_inbound_frame (
  const msg_t *msg_, bool prefetched_batch_exhausted_)
{
    const bool completes_multipart = _in_incomplete_bytes != 0;
    const uint64_t frame_bytes = frame_accounted_bytes (msg_);
    _in_incomplete_bytes =
      frame_bytes == UINT64_MAX || UINT64_MAX - _in_incomplete_bytes < frame_bytes
        ? UINT64_MAX
        : _in_incomplete_bytes + frame_bytes;

    if ((msg_->flags () & msg_t::more) != 0) {
        // A snapshot may observe a multipart while its reader has consumed
        // only its prefix. Publish that prefix locally so the registry can
        // subtract it lazily without a registry update on this frame.
        _published_incomplete_bytes_read.store (_in_incomplete_bytes,
                                                std::memory_order_release);
        return;
    }

    _bytes_read =
      UINT64_MAX - _bytes_read < _in_incomplete_bytes
        ? UINT64_MAX
        : _bytes_read + _in_incomplete_bytes;
    if (!msg_->is_routing_id () && !msg_->is_credential ())
        ++_msgs_read;
    _in_incomplete_bytes = 0;
    // Publish a reset only when a multipart prefix was visible. Single-frame
    // messages leave this snapshot component at zero, so rewriting it on every
    // receive adds coherence traffic without changing observable state. Keep
    // the reset before the completed total so a sampler that observes the
    // latter cannot combine it with an old multipart prefix.
    if (completes_multipart)
        _published_incomplete_bytes_read.store (0, std::memory_order_release);
    publish_ledger_unlocked (&_inbound_ledger_sequence,
                             &_published_msgs_read, &_published_bytes_read,
                             _msgs_read, _bytes_read);

    const uint64_t credit_delta = _bytes_read - _last_credit_bytes_read;
    const uint64_t lwm = _lwm.load (std::memory_order_relaxed);
    const bool lwm_reached = lwm > 0 && credit_delta >= lwm;
    bool waiter_lwm_reached = false;
    bool blocked_writer_drained = false;
    bool writer_waiting = false;
    pipe_t *const peer = get_peer ();
    if (credit_delta > 0 && peer) {
        writer_waiting =
          peer->_waiting_for_byte_credit.load (std::memory_order_acquire);
        if (!writer_waiting
            && (lwm_reached || prefetched_batch_exhausted_)) {
            // Close the credit-boundary race against the writer's matching
            // waiter-before-credit fence. Either this second load observes
            // the waiter or the writer's credit recheck observes our update.
            std::atomic_thread_fence (std::memory_order_seq_cst);
            writer_waiting = peer->_waiting_for_byte_credit.load (
              std::memory_order_acquire);
        }
        if (writer_waiting) {
            // A deferred physical-queue shrink intentionally leaves applied
            // accounting above the planned HWM, and endpoint plan application
            // can race a later replan. The peer already enforces the planned
            // value, so a blocked writer must use that same window for credit
            // wakeups instead of waiting for a stale, larger cached LWM.
            const uint64_t planned_in = planned_in_hwm ();
            const uint64_t planned_lwm =
              planned_in == 0
                ? 1
                : apply_lwm_hint (planned_in, compute_lwm (planned_in),
                                  _lwm_hint);
            waiter_lwm_reached = credit_delta >= planned_lwm;
            if (!waiter_lwm_reached && prefetched_batch_exhausted_) {
                // This is the rare blocked-writer fallback below the planned
                // LWM. Verify that the published queue is actually drained;
                // a prefetched batch boundary alone does not mean that the
                // next batch is absent and must not trigger a premature wake.
                // The non-sleeping probe also preserves the hot-path rule that
                // a preview never publishes the ypipe sleep marker.
                normalized_head_probe_t next_head = {pipe_head_invalid};
                blocked_writer_drained =
                  _in_pipe
                  && !_in_pipe->probe_if_published (&probe_normalized_head,
                                                    &next_head);
            }
        }
    }
    const bool credit_boundary =
      credit_delta > 0
      && (lwm_reached || waiter_lwm_reached || blocked_writer_drained);
    if (credit_boundary) {
        _last_credit_bytes_read = _bytes_read;
        // Active writers sample published credit when their cached window
        // fills. Only a writer that parked for credit needs an owner-thread
        // activation; periodic credit snapshots must not schedule I/O work.
        if (writer_waiting)
            send_activate_write (peer, _in_generation, _msgs_read,
                                 _bytes_read);
    }
    if (!_registry_accounting && credit_boundary)
        get_ctx ()->_physical_queue_registry.refresh_application_hwm_if_drained (
          _in_physical_queue);
}
