/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/ctx_physical_queue_registry.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

#include "core/pipe.hpp"
#include "utils/err.hpp"

namespace zlink
{
namespace
{
enum physical_queue_lane_t
{
    physical_queue_lane_unclassified = 0,
    physical_queue_lane_application,
    physical_queue_lane_completion,
    physical_queue_lane_monitor
};

uint64_t saturating_add (std::atomic<uint64_t> *value_, uint64_t amount_,
                         std::atomic<bool> *overflow_)
{
    uint64_t current = value_->load (std::memory_order_relaxed);
    while (true) {
        const bool overflow = UINT64_MAX - current < amount_;
        const uint64_t desired = overflow ? UINT64_MAX : current + amount_;
        if (value_->compare_exchange_weak (current, desired,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
            if (overflow)
                overflow_->store (true, std::memory_order_relaxed);
            return desired;
        }
    }
}

void saturating_increment_release (std::atomic<uint64_t> *value_,
                                   std::atomic<bool> *overflow_)
{
    uint64_t current = value_->load (std::memory_order_relaxed);
    while (true) {
        const bool overflow = current == UINT64_MAX;
        const uint64_t desired = overflow ? UINT64_MAX : current + 1;
        if (value_->compare_exchange_weak (current, desired,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
            if (overflow)
                overflow_->store (true, std::memory_order_relaxed);
            return;
        }
    }
}

void subtract_exact (std::atomic<uint64_t> *value_, uint64_t amount_)
{
    uint64_t current = value_->load (std::memory_order_relaxed);
    while (true) {
        zlink_assert (current >= amount_);
        if (value_->compare_exchange_weak (current, current - amount_,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed))
            return;
    }
}

bool try_subtract_exact (std::atomic<uint64_t> *value_, uint64_t amount_)
{
    uint64_t current = value_->load (std::memory_order_relaxed);
    while (current >= amount_) {
        if (value_->compare_exchange_weak (current, current - amount_,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed))
            return true;
    }
    return false;
}

void observe_peak (std::atomic<uint64_t> *peak_, uint64_t current_)
{
    uint64_t peak = peak_->load (std::memory_order_relaxed);
    while (peak < current_
           && !peak_->compare_exchange_weak (peak, current_,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
    }
}

uint32_t blocked_ratio_ppm (uint64_t attempts_, uint64_t blocked_)
{
    if (attempts_ == 0 || blocked_ == 0)
        return 0;
    if (blocked_ >= attempts_)
        return 1000000;

    // Long division keeps floor(blocked * 1,000,000 / attempts) exact even
    // after the counters grow beyond the range where the product fits u64.
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    const uint32_t scale = 1000000;
    for (int bit = 19; bit >= 0; --bit) {
        quotient *= 2;
        if (remainder >= attempts_ - remainder) {
            remainder -= attempts_ - remainder;
            ++quotient;
        } else {
            remainder += remainder;
        }
        if ((scale & (1u << bit)) != 0) {
            if (remainder >= attempts_ - blocked_) {
                remainder -= attempts_ - blocked_;
                ++quotient;
            } else {
                remainder += blocked_;
            }
        }
    }
    return static_cast<uint32_t> (quotient);
}

int accounting_lane (const physical_queue_record_t &direction_);

}

struct stored_endpoint_policy_t
{
    stored_endpoint_policy_t () :
        present (false), role (auto_hwm_role_none), manual (false),
        planning_enabled (false), hwm (0)
    {
    }
    bool present;
    auto_hwm_role_t role;
    bool manual;
    bool planning_enabled;
    uint64_t hwm;
};

struct physical_queue_record_t
{
    physical_queue_record_t (uint64_t queue_id_, uint64_t hwm_,
                             physical_queue_lane_t lane_,
                             uint64_t minimum_reservation_bytes_) :
        queue_id (queue_id_),
        generation (1),
        lane (lane_),
        endpoint_refs (2),
        minimum_reservation_bytes (minimum_reservation_bytes_),
        planned_hwm (hwm_),
        applied_hwm (hwm_),
        provisional_accounted_bytes (0),
        committed_accounted_bytes (0),
        completion_pending_message_count (0),
        application_writer (NULL),
        application_reader (NULL)
    {
    }

    const uint64_t queue_id;
    std::atomic<uint64_t> generation;
    std::atomic<int> lane;
    std::atomic<uint32_t> endpoint_refs;
    const uint64_t minimum_reservation_bytes;
    std::atomic<uint64_t> planned_hwm;
    std::atomic<uint64_t> applied_hwm;
    std::atomic<uint64_t> provisional_accounted_bytes;
    std::atomic<uint64_t> committed_accounted_bytes;
    std::atomic<uint64_t> completion_pending_message_count;
    // Protected by ctx_physical_queue_registry_t::_sync. These are used only
    // to sample the pipe-local application ledger outside the frame path.
    pipe_t *application_writer;
    pipe_t *application_reader;
    stored_endpoint_policy_t writer_policy;
    stored_endpoint_policy_t reader_policy;
};

namespace
{
int accounting_lane (const zlink::physical_queue_record_t &direction_)
{
    const int lane = direction_.lane.load (std::memory_order_acquire);
    return lane == physical_queue_lane_completion
             ? physical_queue_lane_completion
             : (lane == physical_queue_lane_monitor
                  ? physical_queue_lane_monitor
                  : physical_queue_lane_application);
}

uint64_t current_queue_bytes (const zlink::physical_queue_record_t &direction_)
{
    const uint64_t provisional = direction_.provisional_accounted_bytes.load (
      std::memory_order_relaxed);
    const uint64_t committed = direction_.committed_accounted_bytes.load (
      std::memory_order_relaxed);
    if (UINT64_MAX - provisional < committed)
        return UINT64_MAX;
    return provisional + committed;
}

void apply_deferred_hwm_if_drained (
  zlink::physical_queue_record_t *direction_)
{
    const uint64_t planned = direction_->planned_hwm.load (
      std::memory_order_acquire);
    const uint64_t applied = direction_->applied_hwm.load (
      std::memory_order_relaxed);
    if (planned == applied)
        return;
    if (planned == 0 || current_queue_bytes (*direction_) <= planned)
        direction_->applied_hwm.store (planned, std::memory_order_release);
}

uint64_t add_snapshot_value (uint64_t current_, uint64_t amount_,
                             bool *overflow_)
{
    if (UINT64_MAX - current_ < amount_) {
        *overflow_ = true;
        return UINT64_MAX;
    }
    return current_ + amount_;
}
}

zlink::decoder_frame_reservation_request_t::decoder_frame_reservation_request_t () :
    payload_bytes (0),
    msg_flags (0),
    multipart_started_empty (false),
    qualify_multipart_from_queue_state (false)
{
}

zlink::decoder_frame_reservation_t::decoder_frame_reservation_t () :
    queue_id (0),
    generation (0),
    frame_bytes (0),
    payload_bytes (0),
    msg_flags (0),
    multipart_started_empty (false),
    active (false)
{
}

void zlink::decoder_frame_reservation_t::reset ()
{
    queue_id = 0;
    generation = 0;
    frame_bytes = 0;
    payload_bytes = 0;
    msg_flags = 0;
    multipart_started_empty = false;
    active = false;
}
}

zlink::physical_queue_registry_snapshot_t::physical_queue_registry_snapshot_t () :
    active_application_direction_count (0),
    active_completion_direction_count (0),
    application_current_accounted_bytes (0),
    application_provisional_accounted_bytes (0),
    application_peak_accounted_bytes (0),
    completion_current_accounted_bytes (0),
    completion_peak_accounted_bytes (0),
    completion_pending_message_count (0),
    monitor_applied_hwm_bytes (0),
    monitor_current_accounted_bytes (0),
    oversize_admission_count (0),
    largest_oversize_message_bytes (0),
    total_admission_attempts (0),
    first_blocked_admission_attempts (0),
    blocked_ratio_ppm (0),
    aggregate_overflow (false)
{
}

zlink::physical_queue_endpoint_policy_t::physical_queue_endpoint_policy_t () :
    role (auto_hwm_role_none),
    writer (false),
    manual (false),
    planning_enabled (false),
    hwm (0)
{
}

zlink::ctx_physical_queue_registry_t::ctx_physical_queue_registry_t () :
    _next_queue_id (1),
    _application_reserved_minimum_bytes (0),
    _application_peak_accounted_bytes (0),
    _completion_peak_accounted_bytes (0),
    _oversize_admission_count (0),
    _largest_oversize_message_bytes (0),
    _total_admission_attempts (0),
    _first_blocked_admission_attempts (0),
    _aggregate_overflow (false)
{
}

zlink::ctx_physical_queue_registry_t::~ctx_physical_queue_registry_t ()
{
    zlink_assert (_directions.empty ());
    zlink_assert (_application_reserved_minimum_bytes == 0);
}

uint64_t zlink::ctx_physical_queue_registry_t::allocate_queue_id_unlocked ()
{
    const uint64_t first_candidate = _next_queue_id;
    do {
        const uint64_t candidate = _next_queue_id;
        _next_queue_id = candidate == UINT64_MAX ? 1 : candidate + 1;
        if (_directions.find (candidate) == _directions.end ())
            return candidate;
    } while (_next_queue_id != first_candidate);

    zlink_assert (false);
    return 0;
}

int zlink::ctx_physical_queue_registry_t::create_pipepair_queues (
  uint64_t first_direction_hwm_, uint64_t second_direction_hwm_,
  physical_queue_class_t queue_class_, auto_hwm_role_t role_,
  bool planning_enabled_,
  const auto_hwm_context_plan_t &context_plan_,
  physical_queue_handle_t *first_direction_,
  physical_queue_handle_t *second_direction_)
{
    zlink_assert (first_direction_);
    zlink_assert (second_direction_);

    *first_direction_ = physical_queue_handle_t ();
    *second_direction_ = physical_queue_handle_t ();

    const physical_queue_lane_t queue_lane =
      queue_class_ == physical_queue_class_completion
        ? physical_queue_lane_completion
        : (queue_class_ == physical_queue_class_monitor
             ? physical_queue_lane_monitor
             : physical_queue_lane_application);
    uint64_t per_direction_minimum = 0;
    if (queue_lane == physical_queue_lane_application
        && context_plan_.enabled && planning_enabled_
        && role_ != auto_hwm_role_none) {
        per_direction_minimum = auto_hwm_profile_minimum_bytes (
          context_plan_.profile, role_);
    }

    scoped_lock_t lock (_sync);
    const bool reservation_overflow =
      per_direction_minimum > UINT64_MAX / 2
      || _application_reserved_minimum_bytes
           > UINT64_MAX - per_direction_minimum * 2;
    const uint64_t pair_minimum =
      reservation_overflow ? UINT64_MAX : per_direction_minimum * 2;
    const uint64_t available_budget =
      _application_reserved_minimum_bytes
          >= context_plan_.effective_core_budget_bytes
        ? 0
        : context_plan_.effective_core_budget_bytes
            - _application_reserved_minimum_bytes;
    if (reservation_overflow || pair_minimum > available_budget) {
        errno = ENOBUFS;
        return -1;
    }

    const uint64_t first_hwm =
      queue_lane == physical_queue_lane_completion ? 0
                                                    : first_direction_hwm_;
    const uint64_t second_hwm =
      queue_lane == physical_queue_lane_completion ? 0
                                                    : second_direction_hwm_;
    physical_queue_handle_t first = std::make_shared<physical_queue_record_t> (
      allocate_queue_id_unlocked (), first_hwm, queue_lane,
      per_direction_minimum);
    physical_queue_handle_t second = std::make_shared<physical_queue_record_t> (
      allocate_queue_id_unlocked (), second_hwm, queue_lane,
      per_direction_minimum);
    zlink_assert (first->queue_id != second->queue_id);
    _directions.insert (std::make_pair (first->queue_id, first));
    _directions.insert (std::make_pair (second->queue_id, second));
    _application_reserved_minimum_bytes += pair_minimum;
    *first_direction_ = first;
    *second_direction_ = second;
    return 0;
}

void zlink::ctx_physical_queue_registry_t::classify_pipepair_queues (
  const physical_queue_handle_t &first_direction_,
  const physical_queue_handle_t &second_direction_,
  transport_lane_t lane_)
{
    zlink_assert (first_direction_);
    zlink_assert (second_direction_);
    zlink_assert (first_direction_.get () != second_direction_.get ());

    const physical_queue_lane_t queue_lane =
      lane_ == transport_lane_completion ? physical_queue_lane_completion
                                         : physical_queue_lane_application;
    scoped_lock_t lock (_sync);
    physical_queue_record_t *directions[2] = {first_direction_.get (),
                                               second_direction_.get ()};
    for (size_t i = 0; i != 2; ++i) {
        physical_queue_record_t *direction = directions[i];
        zlink_assert (direction->endpoint_refs > 0);
        zlink_assert (_directions.find (direction->queue_id) != _directions.end ());
        const int previous_lane =
          direction->lane.load (std::memory_order_relaxed);
        if (previous_lane == physical_queue_lane_monitor
            && queue_lane == physical_queue_lane_application)
            continue;
        zlink_assert (previous_lane == physical_queue_lane_unclassified
                      || previous_lane == queue_lane);
        if (previous_lane == physical_queue_lane_unclassified) {
            zlink_assert (direction->provisional_accounted_bytes.load (
                            std::memory_order_relaxed)
                          == 0);
            zlink_assert (direction->committed_accounted_bytes.load (
                            std::memory_order_relaxed)
                          == 0);
            direction->lane.store (queue_lane, std::memory_order_release);
            if (queue_lane == physical_queue_lane_completion) {
                direction->planned_hwm.store (0, std::memory_order_release);
                direction->applied_hwm.store (0, std::memory_order_release);
            }
        }
    }
}

void zlink::ctx_physical_queue_registry_t::bind_application_pipe_queue (
  const physical_queue_handle_t &direction_, pipe_t *writer_, pipe_t *reader_)
{
    if (!direction_ || !writer_ || !reader_)
        return;

    scoped_lock_t lock (_sync);
    const std::map<uint64_t, physical_queue_handle_t>::const_iterator known =
      _directions.find (direction_->queue_id);
    if (known == _directions.end ()
        || known->second.get () != direction_.get ()
        || accounting_lane (*direction_) != physical_queue_lane_application)
        return;
    zlink_assert (!direction_->application_writer);
    zlink_assert (!direction_->application_reader);
    direction_->application_writer = writer_;
    direction_->application_reader = reader_;
}

void zlink::ctx_physical_queue_registry_t::unbind_application_pipe_endpoint (
  const physical_queue_handle_t &direction_, pipe_t *pipe_, bool writer_)
{
    if (!direction_ || !pipe_)
        return;

    scoped_lock_t lock (_sync);
    const std::map<uint64_t, physical_queue_handle_t>::const_iterator known =
      _directions.find (direction_->queue_id);
    if (known == _directions.end ()
        || known->second.get () != direction_.get ())
        return;
    pipe_t *&endpoint = writer_ ? direction_->application_writer
                                : direction_->application_reader;
    if (endpoint == pipe_)
        endpoint = NULL;
}

bool zlink::ctx_physical_queue_registry_t::sample_application_pipe_queue (
  const physical_queue_handle_t &direction_, uint64_t *provisional_out_,
  uint64_t *committed_out_) const
{
    if (provisional_out_)
        *provisional_out_ = 0;
    if (committed_out_)
        *committed_out_ = 0;
    if (!direction_)
        return false;

    pipe_t *writer = NULL;
    pipe_t *reader = NULL;
    {
        scoped_lock_t lock (_sync);
        const std::map<uint64_t, physical_queue_handle_t>::const_iterator known =
          _directions.find (direction_->queue_id);
        if (known == _directions.end ()
            || known->second.get () != direction_.get ()
            || direction_->endpoint_refs == 0
            || accounting_lane (*direction_)
                 != physical_queue_lane_application)
            return false;

        writer = direction_->application_writer;
        reader = direction_->application_reader;
        if (!writer || !reader || !writer->retain_lifetime_ref ())
            return false;
        if (!reader->retain_lifetime_ref ()) {
            writer->release_lifetime_ref ();
            return false;
        }
    }

    writer->snapshot_outbound_queue_accounting (reader, provisional_out_,
                                                committed_out_);
    reader->release_lifetime_ref ();
    writer->release_lifetime_ref ();
    return true;
}

void zlink::ctx_physical_queue_registry_t::account_provisional_frame (
  const physical_queue_handle_t &direction_, uint64_t frame_bytes_)
{
    zlink_assert (direction_);
    zlink_assert (frame_bytes_ > 0);
    //  Context aggregates are sampled from queue-local counters, so the lane
    //  does not change what is charged here.
    saturating_add (&direction_->provisional_accounted_bytes, frame_bytes_,
                    &_aggregate_overflow);
}

void zlink::ctx_physical_queue_registry_t::commit_message (
  const physical_queue_handle_t &direction_, uint64_t final_frame_bytes_,
  bool counted_message_, bool oversize_admission_)
{
    zlink_assert (direction_);
    zlink_assert (final_frame_bytes_ > 0);
    const int lane = accounting_lane (*direction_);
    const uint64_t provisional =
      direction_->provisional_accounted_bytes.exchange (
        0, std::memory_order_relaxed);
    const bool message_overflow = UINT64_MAX - provisional < final_frame_bytes_;
    const uint64_t message_bytes =
      message_overflow ? UINT64_MAX : provisional + final_frame_bytes_;
    if (message_overflow)
        _aggregate_overflow.store (true, std::memory_order_relaxed);
    saturating_add (&direction_->committed_accounted_bytes, message_bytes,
                    &_aggregate_overflow);

    if (lane == physical_queue_lane_application) {
        if (oversize_admission_) {
            saturating_add (&_oversize_admission_count, 1,
                            &_aggregate_overflow);
            observe_peak (&_largest_oversize_message_bytes,
                          message_bytes);
        }
    } else if (lane == physical_queue_lane_completion) {
        if (counted_message_)
            saturating_add (&direction_->completion_pending_message_count, 1,
                            &_aggregate_overflow);
    } else {
        zlink_assert (lane == physical_queue_lane_monitor);
    }
}

void zlink::ctx_physical_queue_registry_t::rollback_provisional (
  const physical_queue_handle_t &direction_, uint64_t frame_bytes_)
{
    if (!direction_)
        return;

    uint64_t removed = frame_bytes_;
    if (removed == 0) {
        removed = direction_->provisional_accounted_bytes.exchange (
          0, std::memory_order_relaxed);
    } else {
        subtract_exact (&direction_->provisional_accounted_bytes, removed);
    }
    if (removed == 0)
        return;
    const int lane = accounting_lane (*direction_);
    if (lane == physical_queue_lane_application) {
    } else if (lane == physical_queue_lane_completion) {
    } else {
        zlink_assert (lane == physical_queue_lane_monitor);
    }
    apply_deferred_hwm_if_drained (direction_.get ());
}

void zlink::ctx_physical_queue_registry_t::release_committed_frame (
  const physical_queue_handle_t &direction_, uint64_t frame_bytes_,
  uint64_t counted_message_count_)
{
    if (!direction_ || frame_bytes_ == 0)
        return;
    subtract_exact (&direction_->committed_accounted_bytes, frame_bytes_);
    const int lane = accounting_lane (*direction_);
    if (lane == physical_queue_lane_application) {
    } else if (lane == physical_queue_lane_completion) {
        if (counted_message_count_ > 0) {
            subtract_exact (&direction_->completion_pending_message_count,
                            counted_message_count_);
        }
    } else {
        zlink_assert (lane == physical_queue_lane_monitor);
    }
    apply_deferred_hwm_if_drained (direction_.get ());
}

int zlink::ctx_physical_queue_registry_t::reserve_decoder_frame (
  const physical_queue_handle_t &direction_,
  const decoder_frame_reservation_request_t &request_,
  decoder_frame_reservation_t *reservation_storage_,
  decoder_frame_reservation_t **reservation_out_)
{
    if (!direction_ || !reservation_storage_ || !reservation_out_) {
        errno = EFAULT;
        return -1;
    }
    *reservation_out_ = NULL;
    if (reservation_storage_->active) {
        errno = EBUSY;
        return -1;
    }
    const uint64_t metadata_bytes = static_cast<uint64_t> (sizeof (msg_t));
    if (UINT64_MAX - request_.payload_bytes < metadata_bytes) {
        errno = EMSGSIZE;
        return -1;
    }
    const uint64_t frame_bytes = request_.payload_bytes + metadata_bytes;

    if (direction_->endpoint_refs == 0) {
        errno = ETERM;
        return -1;
    }
    const bool multipart_started_empty =
      request_.multipart_started_empty
      || (request_.qualify_multipart_from_queue_state
          && current_queue_bytes (*direction_) == 0);
    const int lane = accounting_lane (*direction_);
    if (lane != physical_queue_lane_application) {
        if (lane != physical_queue_lane_completion) {
            errno = EINVAL;
            return -1;
        }
    }

    if (lane == physical_queue_lane_application) {
        const uint64_t used = current_queue_bytes (*direction_);
        const bool more = (request_.msg_flags & msg_t::more) != 0;
        const uint64_t hwm = direction_->applied_hwm.load (
          std::memory_order_acquire);
        const bool fits = hwm == 0
                          || (used != UINT64_MAX
                              && UINT64_MAX - used >= frame_bytes
                              && used + frame_bytes <= hwm);
        //  The pipe freezes this qualification immediately before its first
        //  data frame. MORE frames never use the exception; only the final
        //  frame may complete the one message that began on an empty origin.
        const bool final_oversize =
          !more && multipart_started_empty;
        if (!fits && !final_oversize) {
            errno = EAGAIN;
            return -1;
        }
    }

    decoder_frame_reservation_t *const reservation = reservation_storage_;
    reservation->queue_id = direction_->queue_id;
    reservation->generation = direction_->generation.load (
      std::memory_order_acquire);
    reservation->frame_bytes = frame_bytes;
    reservation->payload_bytes = request_.payload_bytes;
    reservation->msg_flags = request_.msg_flags;
    reservation->multipart_started_empty = multipart_started_empty;
    reservation->active = true;
    *reservation_out_ = reservation;
    return 0;
}

int zlink::ctx_physical_queue_registry_t::commit_decoder_frame (
  const physical_queue_handle_t &direction_,
  decoder_frame_reservation_t **reservation_, uint64_t payload_bytes_,
  unsigned char msg_flags_, bool counted_message_,
  bool *oversize_admission_out_)
{
    if (!reservation_ || !*reservation_) {
        errno = EFAULT;
        return -1;
    }
    decoder_frame_reservation_t *reservation = *reservation_;
    *reservation_ = NULL;
    bool oversize = false;
    int failure_errno = 0;
    const bool same_generation =
      direction_ && direction_->queue_id == reservation->queue_id
      && direction_->endpoint_refs.load (std::memory_order_acquire) > 0
      && direction_->generation.load (std::memory_order_acquire)
           == reservation->generation;
    if (!reservation->active || !same_generation) {
        failure_errno = ETERM;
    } else if (reservation->payload_bytes != payload_bytes_
               || reservation->msg_flags != msg_flags_) {
        failure_errno = EPROTO;
    }

    if (failure_errno == 0) {
        const int lane = accounting_lane (*direction_);
        zlink_assert (lane == physical_queue_lane_application
                      || lane == physical_queue_lane_completion);
        if (lane == physical_queue_lane_application) {
            // Application queues enforce byte admission and publish in-flight
            // totals from their owning pipe. The reservation carries metadata
            // only and does not mutate shared registry counters.
        } else if ((msg_flags_ & msg_t::more) == 0) {
            const uint64_t message_bytes =
              direction_->provisional_accounted_bytes.exchange (
                0, std::memory_order_relaxed);
            const bool overflow = UINT64_MAX - message_bytes
                                  < reservation->frame_bytes;
            const uint64_t committed_bytes =
              overflow ? UINT64_MAX
                       : message_bytes + reservation->frame_bytes;
            if (overflow)
                _aggregate_overflow.store (true, std::memory_order_relaxed);
            saturating_add (&direction_->committed_accounted_bytes,
                            committed_bytes, &_aggregate_overflow);
            if (counted_message_) {
                saturating_add (&direction_->completion_pending_message_count,
                                1, &_aggregate_overflow);
            }
        } else {
            saturating_add (&direction_->provisional_accounted_bytes,
                            reservation->frame_bytes, &_aggregate_overflow);
        }
        reservation->active = false;
    }

    if (oversize_admission_out_)
        *oversize_admission_out_ = oversize;
    reservation->reset ();
    if (failure_errno != 0) {
        errno = failure_errno;
        return -1;
    }
    return 0;
}

void zlink::ctx_physical_queue_registry_t::release_decoder_frame (
  decoder_frame_reservation_t **reservation_)
{
    if (!reservation_ || !*reservation_)
        return;
    decoder_frame_reservation_t *const reservation = *reservation_;
    *reservation_ = NULL;
    reservation->active = false;
    reservation->reset ();
}

void zlink::ctx_physical_queue_registry_t::plan_application_queues (
  auto_hwm_context_plan_t *context_,
  const std::vector<physical_queue_endpoint_policy_t> &policies_)
{
    if (!context_)
        return;

    struct resolved_input_t
    {
        resolved_input_t () :
            queue (), role (auto_hwm_role_none), planning_enabled (false),
            writer_seen (false), reader_seen (false), auto_seen (false),
            finite_manual_seen (false), finite_manual_hwm (UINT64_MAX)
        {
        }
        physical_queue_handle_t queue;
        auto_hwm_role_t role;
        bool planning_enabled;
        bool writer_seen;
        bool reader_seen;
        bool auto_seen;
        bool finite_manual_seen;
        uint64_t finite_manual_hwm;
    };

    std::map<uint64_t, resolved_input_t> inputs;
    {
        scoped_lock_t lock (_sync);
        for (size_t i = 0; i != policies_.size (); ++i) {
            const physical_queue_endpoint_policy_t &policy = policies_[i];
            if (!policy.queue)
                continue;
            std::map<uint64_t, physical_queue_handle_t>::const_iterator known =
              _directions.find (policy.queue->queue_id);
            if (known == _directions.end ()
                || known->second.get () != policy.queue.get ()
                || policy.queue->endpoint_refs == 0
                || policy.queue->lane.load (std::memory_order_acquire)
                     != physical_queue_lane_application)
                continue;

            stored_endpoint_policy_t &stored =
              policy.writer ? policy.queue->writer_policy
                            : policy.queue->reader_policy;
            stored.present = true;
            stored.role = policy.role;
            stored.manual = policy.manual;
            stored.planning_enabled = policy.planning_enabled;
            stored.hwm = policy.hwm;
        }

        for (std::map<uint64_t, physical_queue_handle_t>::const_iterator it =
               _directions.begin ();
             it != _directions.end (); ++it) {
            const physical_queue_handle_t &queue = it->second;
            if (queue->endpoint_refs == 0
                || queue->lane.load (std::memory_order_acquire)
                     != physical_queue_lane_application)
                continue;
            const stored_endpoint_policy_t endpoint_policies[2] = {
              queue->writer_policy, queue->reader_policy};
            resolved_input_t &input = inputs[it->first];
            input.queue = queue;
            for (size_t endpoint_index = 0; endpoint_index != 2;
                 ++endpoint_index) {
                const stored_endpoint_policy_t &policy =
                  endpoint_policies[endpoint_index];
                if (!policy.present)
                    continue;
                input.planning_enabled =
                  input.planning_enabled || policy.planning_enabled;
                input.writer_seen = input.writer_seen || endpoint_index == 0;
                input.reader_seen = input.reader_seen || endpoint_index == 1;

                if (input.role == auto_hwm_role_none) {
                    input.role = policy.role;
                } else if (policy.role != auto_hwm_role_none) {
                const uint64_t current_max = auto_hwm_profile_maximum_bytes (
                  context_->profile, input.role);
                const uint64_t candidate_max = auto_hwm_profile_maximum_bytes (
                  context_->profile, policy.role);
                if (candidate_max < current_max
                    || (candidate_max == current_max
                        && policy.role < input.role))
                    input.role = policy.role;
                }

                const bool effective_manual =
                  policy.manual || !policy.planning_enabled;
                if (!effective_manual) {
                    input.auto_seen = true;
                } else if (policy.hwm > 0) {
                    input.finite_manual_seen = true;
                    input.finite_manual_hwm =
                      std::min (input.finite_manual_hwm, policy.hwm);
                }
            }
        }
    }

    if (!context_->enabled) {
        auto_hwm_context_finalize (context_, NULL, 0);
        return;
    }

    std::vector<resolved_input_t *> planned_inputs;
    std::vector<auto_hwm_socket_plan_t> queue_plans;
    uint64_t send_count = 0;
    uint64_t receive_count = 0;
    for (std::map<uint64_t, resolved_input_t>::iterator it = inputs.begin ();
         it != inputs.end (); ++it) {
        resolved_input_t &input = it->second;
        if (!input.planning_enabled || input.role == auto_hwm_role_none)
            continue;
        const bool manual = input.finite_manual_seen || !input.auto_seen;
        const uint64_t manual_hwm = input.finite_manual_seen
                                      ? input.finite_manual_hwm
                                      : 0;
        auto_hwm_socket_plan_t plan;
        auto_hwm_socket_plan_prepare (input.role, 1, 0, manual, manual_hwm,
                                      false, 0, true, &plan);
        planned_inputs.push_back (&input);
        queue_plans.push_back (plan);
        if (input.writer_seen)
            ++send_count;
        if (input.reader_seen)
            ++receive_count;
    }

    if (!queue_plans.empty ())
        auto_hwm_context_finalize (context_, &queue_plans[0],
                                   queue_plans.size ());
    else
        auto_hwm_context_finalize (context_, NULL, 0);
    context_->active_send_queue_count = send_count;
    context_->active_receive_queue_count = receive_count;
    context_->active_directional_queue_count = queue_plans.size ();

    uint64_t total_applied = 0;
    for (size_t i = 0; i != queue_plans.size (); ++i) {
        physical_queue_record_t *direction = planned_inputs[i]->queue.get ();
        const uint64_t target = queue_plans[i].sndhwm;
        update_hwm_target (planned_inputs[i]->queue, target);

        uint64_t applied = direction->applied_hwm.load (
          std::memory_order_acquire);
        if (applied == 0) {
            applied = queue_plans[i].maximum_hwm_bytes;
            context_->aggregate_hwm_valid = false;
        }
        total_applied = add_snapshot_value (
          total_applied, applied, &context_->aggregate_overflow);
    }
    context_->total_applied_hwm_bytes = total_applied;
    if (context_->aggregate_overflow)
        context_->budget_insufficient = true;
}

void zlink::ctx_physical_queue_registry_t::record_endpoint_policy (
  const physical_queue_endpoint_policy_t &policy_)
{
    if (!policy_.queue)
        return;
    scoped_lock_t lock (_sync);
    std::map<uint64_t, physical_queue_handle_t>::const_iterator known =
      _directions.find (policy_.queue->queue_id);
    if (known == _directions.end ()
        || known->second.get () != policy_.queue.get ())
        return;
    stored_endpoint_policy_t &stored =
      policy_.writer ? policy_.queue->writer_policy
                     : policy_.queue->reader_policy;
    stored.present = true;
    stored.role = policy_.role;
    stored.manual = policy_.manual;
    stored.planning_enabled = policy_.planning_enabled;
    stored.hwm = policy_.hwm;
}

void zlink::ctx_physical_queue_registry_t::record_admission_attempt (
  bool blocked_by_target_hwm_)
{
    // This is a per-frame send hot path. Publish total before blocked so a
    // snapshot that acquires blocked first can never observe blocked > total.
    saturating_increment_release (&_total_admission_attempts,
                                  &_aggregate_overflow);
    if (blocked_by_target_hwm_)
        saturating_increment_release (&_first_blocked_admission_attempts,
                                      &_aggregate_overflow);
}

void zlink::ctx_physical_queue_registry_t::update_hwm_target (
  const physical_queue_handle_t &direction_, uint64_t target_hwm_)
{
    if (!direction_)
        return;
    const uint64_t previous = direction_->applied_hwm.load (
      std::memory_order_acquire);
    direction_->planned_hwm.store (target_hwm_, std::memory_order_release);
    const bool grows = target_hwm_ == 0
                       || (previous != 0 && target_hwm_ >= previous);
    if (grows || current_accounted_bytes (direction_) <= target_hwm_)
        direction_->applied_hwm.store (target_hwm_,
                                       std::memory_order_release);
}

void zlink::ctx_physical_queue_registry_t::refresh_application_hwm_if_drained (
  const physical_queue_handle_t &direction_)
{
    if (!direction_)
        return;

    uint64_t planned = 0;
    uint64_t applied = 0;
    {
        scoped_lock_t lock (_sync);
        const std::map<uint64_t, physical_queue_handle_t>::const_iterator known =
          _directions.find (direction_->queue_id);
        if (known == _directions.end ()
            || known->second.get () != direction_.get ()
            || accounting_lane (*direction_)
                 != physical_queue_lane_application)
            return;
        planned = direction_->planned_hwm.load (std::memory_order_acquire);
        applied = direction_->applied_hwm.load (std::memory_order_acquire);
    }
    if (planned == applied)
        return;

    const uint64_t current = current_accounted_bytes (direction_);
    if (planned == 0 || current <= planned)
        direction_->applied_hwm.store (planned, std::memory_order_release);
}

uint64_t zlink::ctx_physical_queue_registry_t::planned_hwm (
  const physical_queue_handle_t &direction_) const
{
    return direction_ ? direction_->planned_hwm.load (std::memory_order_acquire)
                      : 0;
}

uint64_t zlink::ctx_physical_queue_registry_t::applied_hwm (
  const physical_queue_handle_t &direction_) const
{
    return direction_ ? direction_->applied_hwm.load (std::memory_order_acquire)
                      : 0;
}

uint64_t zlink::ctx_physical_queue_registry_t::current_accounted_bytes (
  const physical_queue_handle_t &direction_) const
{
    if (!direction_)
        return 0;

    bool application_direction = false;
    uint64_t fallback = 0;
    {
        scoped_lock_t lock (_sync);
        const std::map<uint64_t, physical_queue_handle_t>::const_iterator known =
          _directions.find (direction_->queue_id);
        if (known == _directions.end ()
            || known->second.get () != direction_.get ())
            return 0;
        application_direction =
          accounting_lane (*direction_) == physical_queue_lane_application;
        fallback = current_queue_bytes (*direction_);
    }

    if (application_direction) {
        uint64_t provisional = 0;
        uint64_t committed = 0;
        if (sample_application_pipe_queue (direction_, &provisional,
                                           &committed)) {
            return UINT64_MAX - provisional < committed
                     ? UINT64_MAX
                     : provisional + committed;
        }
    }
    return fallback;
}

uint64_t zlink::ctx_physical_queue_registry_t::generation (
  const physical_queue_handle_t &direction_) const
{
    if (!direction_)
        return 0;
    scoped_lock_t lock (_sync);
    return direction_->generation;
}

void zlink::ctx_physical_queue_registry_t::advance_generation (
  const physical_queue_handle_t &direction_)
{
    if (!direction_)
        return;

    scoped_lock_t lock (_sync);
    zlink_assert (direction_->endpoint_refs > 0);
    zlink_assert (_directions.find (direction_->queue_id) != _directions.end ());
    direction_->generation =
      direction_->generation == UINT64_MAX ? 1 : direction_->generation + 1;
}

void zlink::ctx_physical_queue_registry_t::release_endpoint (
  physical_queue_handle_t *direction_)
{
    if (!direction_ || !*direction_)
        return;

    physical_queue_handle_t direction = *direction_;
    {
        scoped_lock_t lock (_sync);
        std::map<uint64_t, physical_queue_handle_t>::iterator it =
          _directions.find (direction->queue_id);
        zlink_assert (it != _directions.end ());
        zlink_assert (it->second.get () == direction.get ());
        zlink_assert (direction->endpoint_refs > 0);
        --direction->endpoint_refs;
        if (direction->endpoint_refs == 0) {
            // Normal read, rollback, hiccup and termination paths publish
            // prompt refunds. Last-endpoint retirement is the sole lifecycle
            // backstop for record-owned charge that can no longer be observed.
            direction->provisional_accounted_bytes.store (
              0, std::memory_order_relaxed);
            direction->committed_accounted_bytes.store (
              0, std::memory_order_relaxed);
            const uint64_t completion_pending =
              direction->completion_pending_message_count.exchange (
                0, std::memory_order_relaxed);
            const int lane = accounting_lane (*direction);
            if (lane == physical_queue_lane_application) {
                zlink_assert (completion_pending == 0);
            } else if (lane == physical_queue_lane_completion) {
            } else {
                zlink_assert (lane == physical_queue_lane_monitor);
                zlink_assert (completion_pending == 0);
            }
            erase_direction_if_retired_and_drained_unlocked (direction);
        }
    }
    direction_->reset ();
}

void zlink::ctx_physical_queue_registry_t::erase_direction_if_retired_and_drained_unlocked (
  const physical_queue_handle_t &direction_)
{
    if (!direction_ || direction_->endpoint_refs != 0
        || direction_->provisional_accounted_bytes.load (
             std::memory_order_relaxed)
             != 0
        || direction_->committed_accounted_bytes.load (
             std::memory_order_relaxed)
             != 0)
        return;

    std::map<uint64_t, physical_queue_handle_t>::iterator it =
      _directions.find (direction_->queue_id);
    if (it == _directions.end ())
        return;
    zlink_assert (it->second.get () == direction_.get ());
    zlink_assert (_application_reserved_minimum_bytes
                  >= direction_->minimum_reservation_bytes);
    _application_reserved_minimum_bytes -=
      direction_->minimum_reservation_bytes;
    _directions.erase (it);
}

void zlink::ctx_physical_queue_registry_t::snapshot (
  physical_queue_registry_snapshot_t *out_) const
{
    if (!out_)
        return;

    struct application_direction_sample_t
    {
        application_direction_sample_t () :
            direction (), fallback_provisional (0), fallback_committed (0)
        {
        }

        physical_queue_handle_t direction;
        uint64_t fallback_provisional;
        uint64_t fallback_committed;
    };

    physical_queue_registry_snapshot_t current;
    std::vector<application_direction_sample_t> application_directions;
    {
        // Do not hold the registry mutex while sampling a pipe. Pipe teardown
        // takes this mutex, whereas the sampler takes the pipe's local lock.
        scoped_lock_t lock (_sync);
        for (std::map<uint64_t, physical_queue_handle_t>::const_iterator it =
               _directions.begin ();
             it != _directions.end (); ++it) {
            const physical_queue_handle_t &handle = it->second;
            const physical_queue_record_t &direction = *handle;
            const int lane = direction.lane.load (std::memory_order_acquire);
            const uint64_t provisional =
              direction.provisional_accounted_bytes.load (
                std::memory_order_relaxed);
            const uint64_t committed =
              direction.committed_accounted_bytes.load (
                std::memory_order_relaxed);
            if (lane == physical_queue_lane_application) {
                application_direction_sample_t sample;
                sample.direction = handle;
                sample.fallback_provisional = provisional;
                sample.fallback_committed = committed;
                application_directions.push_back (sample);
            } else if (lane == physical_queue_lane_completion) {
                current.completion_current_accounted_bytes =
                  add_snapshot_value (
                    current.completion_current_accounted_bytes, provisional,
                    &current.aggregate_overflow);
                current.completion_current_accounted_bytes =
                  add_snapshot_value (
                    current.completion_current_accounted_bytes, committed,
                    &current.aggregate_overflow);
                current.completion_pending_message_count =
                  add_snapshot_value (
                    current.completion_pending_message_count,
                    direction.completion_pending_message_count.load (
                      std::memory_order_relaxed),
                    &current.aggregate_overflow);
            } else if (lane == physical_queue_lane_monitor) {
                current.monitor_current_accounted_bytes =
                  add_snapshot_value (
                    current.monitor_current_accounted_bytes, provisional,
                    &current.aggregate_overflow);
                current.monitor_current_accounted_bytes =
                  add_snapshot_value (
                    current.monitor_current_accounted_bytes, committed,
                    &current.aggregate_overflow);
            }
            if (direction.endpoint_refs.load (std::memory_order_relaxed) != 0
                && lane == physical_queue_lane_application) {
                ++current.active_application_direction_count;
            } else if (lane == physical_queue_lane_completion) {
                ++current.active_completion_direction_count;
            } else if (lane == physical_queue_lane_monitor) {
                const uint64_t applied = direction.applied_hwm.load (
                  std::memory_order_acquire);
                zlink_assert (applied > 0);
                current.monitor_applied_hwm_bytes = add_snapshot_value (
                  current.monitor_applied_hwm_bytes, applied,
                  &current.aggregate_overflow);
            }
        }
    }

    for (size_t i = 0; i != application_directions.size (); ++i) {
        const application_direction_sample_t &sample =
          application_directions[i];
        uint64_t provisional = 0;
        uint64_t committed = 0;
        if (!sample_application_pipe_queue (sample.direction, &provisional,
                                            &committed)) {
            provisional = sample.fallback_provisional;
            committed = sample.fallback_committed;
        }
        current.application_provisional_accounted_bytes = add_snapshot_value (
          current.application_provisional_accounted_bytes, provisional,
          &current.aggregate_overflow);
        uint64_t queue_total = add_snapshot_value (
          provisional, committed, &current.aggregate_overflow);
        current.application_current_accounted_bytes = add_snapshot_value (
          current.application_current_accounted_bytes, queue_total,
          &current.aggregate_overflow);
    }
    observe_peak (&_application_peak_accounted_bytes,
                  current.application_current_accounted_bytes);
    current.application_peak_accounted_bytes =
      _application_peak_accounted_bytes.load (std::memory_order_relaxed);
    observe_peak (&_completion_peak_accounted_bytes,
                  current.completion_current_accounted_bytes);
    current.completion_peak_accounted_bytes =
      _completion_peak_accounted_bytes.load (std::memory_order_relaxed);
    current.oversize_admission_count =
      _oversize_admission_count.load (std::memory_order_relaxed);
    current.largest_oversize_message_bytes =
      _largest_oversize_message_bytes.load (std::memory_order_relaxed);
    current.first_blocked_admission_attempts =
      _first_blocked_admission_attempts.load (std::memory_order_acquire);
    current.total_admission_attempts =
      _total_admission_attempts.load (std::memory_order_acquire);
    current.blocked_ratio_ppm = blocked_ratio_ppm (
      current.total_admission_attempts,
      current.first_blocked_admission_attempts);
    current.aggregate_overflow = current.aggregate_overflow
      || _aggregate_overflow.load (std::memory_order_relaxed);
    *out_ = current;
}

void zlink::ctx_physical_queue_registry_t::reset_metrics ()
{
    physical_queue_registry_snapshot_t current;
    snapshot (&current);
    bool overflow = false;
    const uint64_t application_current =
      current.application_current_accounted_bytes;
    const uint64_t completion_current =
      current.completion_current_accounted_bytes;

    scoped_lock_t lock (_sync);
    _application_peak_accounted_bytes.store (application_current,
                                              std::memory_order_relaxed);
    _completion_peak_accounted_bytes.store (completion_current,
                                             std::memory_order_relaxed);
    _oversize_admission_count.store (0, std::memory_order_relaxed);
    _largest_oversize_message_bytes.store (0, std::memory_order_relaxed);
    _total_admission_attempts.store (0, std::memory_order_relaxed);
    _first_blocked_admission_attempts.store (0,
                                              std::memory_order_relaxed);
    _aggregate_overflow.store (overflow || current.aggregate_overflow,
                               std::memory_order_relaxed);
}
