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

//  A pipe contributes at most its two directions; a caller may not hand the
//  extension more than one pipe at a time.
const size_t extension_capacity = 2;

uint64_t multiply_snapshot_value (uint64_t left_, uint64_t right_,
                                  bool *overflow_)
{
    if (left_ != 0 && right_ > UINT64_MAX / left_) {
        *overflow_ = true;
        return UINT64_MAX;
    }
    return left_ * right_;
}

//  One direction's planning input, merged from the reader and writer endpoint
//  policies the registry stores for it. Both the full replan and the attach
//  fast path resolve a direction through this single rule.
struct resolved_direction_t
{
    resolved_direction_t () :
        role (auto_hwm_role_none), planning_enabled (false),
        writer_seen (false), reader_seen (false), auto_seen (false),
        finite_manual_seen (false), finite_manual_hwm (UINT64_MAX)
    {
    }
    auto_hwm_role_t role;
    bool planning_enabled;
    bool writer_seen;
    bool reader_seen;
    bool auto_seen;
    bool finite_manual_seen;
    uint64_t finite_manual_hwm;
};

void resolve_direction (zlink_auto_hwm_profile_t profile_,
                        const physical_queue_record_t &record_,
                        resolved_direction_t *out_)
{
    const stored_endpoint_policy_t endpoint_policies[2] = {
      record_.writer_policy, record_.reader_policy};
    for (size_t endpoint_index = 0; endpoint_index != 2; ++endpoint_index) {
        const stored_endpoint_policy_t &policy =
          endpoint_policies[endpoint_index];
        if (!policy.present)
            continue;
        out_->planning_enabled =
          out_->planning_enabled || policy.planning_enabled;
        out_->writer_seen = out_->writer_seen || endpoint_index == 0;
        out_->reader_seen = out_->reader_seen || endpoint_index == 1;

        if (out_->role == auto_hwm_role_none) {
            out_->role = policy.role;
        } else if (policy.role != auto_hwm_role_none) {
            const uint64_t current_max =
              auto_hwm_profile_maximum_bytes (profile_, out_->role);
            const uint64_t candidate_max =
              auto_hwm_profile_maximum_bytes (profile_, policy.role);
            if (candidate_max < current_max
                || (candidate_max == current_max
                    && policy.role < out_->role))
                out_->role = policy.role;
        }

        const bool effective_manual =
          policy.manual || !policy.planning_enabled;
        if (!effective_manual) {
            out_->auto_seen = true;
        } else if (policy.hwm > 0) {
            out_->finite_manual_seen = true;
            out_->finite_manual_hwm =
              std::min (out_->finite_manual_hwm, policy.hwm);
        }
    }
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
    _application_reservation (),
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
    zlink_assert (_application_reservation.bytes == 0);
    zlink_assert (_application_reservation.directions == 0);
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
    const uint64_t added_directions = per_direction_minimum != 0 ? 2 : 0;
    const bool reservation_overflow =
      _application_reservation.directions > UINT64_MAX - added_directions
      || per_direction_minimum > UINT64_MAX / 2
      || _application_reservation.bytes
           > UINT64_MAX - per_direction_minimum * 2;
    const uint64_t pair_minimum =
      reservation_overflow ? UINT64_MAX : per_direction_minimum * 2;
    // Admission must use the topology being reserved, including this pair.
    // The seed plan has no queue count and therefore only the fixed cap;
    // using it here rejects connections that the same planner can fund.
    const uint64_t budget = context_plan_.configured_core_budget_bytes > 0
      ? context_plan_.configured_core_budget_bytes
      : auto_hwm_effective_budget_bytes (
          context_plan_.profile, context_plan_.resolved_memory_limit_bytes,
          reservation_overflow ? UINT64_MAX
            : _application_reservation.directions + added_directions);
    const uint64_t available_budget =
      _application_reservation.bytes
          >= budget
        ? 0
        : budget
            - _application_reservation.bytes;
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
    _application_reservation.bytes += pair_minimum;
    _application_reservation.directions += added_directions;
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

zlink::physical_queue_record_t *
zlink::ctx_physical_queue_registry_t::find_locked (
  const physical_queue_handle_t &direction_) const
{
    const std::map<uint64_t, physical_queue_handle_t>::const_iterator known =
      _directions.find (direction_->queue_id);
    if (known == _directions.end () || known->second.get () != direction_.get ())
        return NULL;
    return direction_.get ();
}

void zlink::ctx_physical_queue_registry_t::bind_application_pipe_queue (
  const physical_queue_handle_t &direction_, pipe_t *writer_, pipe_t *reader_)
{
    if (!direction_ || !writer_ || !reader_)
        return;

    scoped_lock_t lock (_sync);
    if (!find_locked (direction_)
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
    if (!find_locked (direction_))
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
        if (!find_locked (direction_) || direction_->endpoint_refs == 0
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
        resolved_input_t () : queue (), resolved () {}
        physical_queue_handle_t queue;
        resolved_direction_t resolved;
    };

    std::map<uint64_t, resolved_input_t> inputs;
    {
        scoped_lock_t lock (_sync);
        for (size_t i = 0; i != policies_.size (); ++i) {
            const physical_queue_endpoint_policy_t &policy = policies_[i];
            if (!policy.queue)
                continue;
            if (!find_locked (policy.queue)
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
            resolved_input_t &input = inputs[it->first];
            input.queue = queue;
            resolve_direction (context_->profile, *queue, &input.resolved);
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
        const resolved_direction_t &resolved = input.resolved;
        if (!resolved.planning_enabled || resolved.role == auto_hwm_role_none)
            continue;
        const bool manual =
          resolved.finite_manual_seen || !resolved.auto_seen;
        const uint64_t manual_hwm = resolved.finite_manual_seen
                                      ? resolved.finite_manual_hwm
                                      : 0;
        auto_hwm_socket_plan_t plan;
        auto_hwm_socket_plan_prepare (resolved.role, 1, 0, manual, manual_hwm,
                                      false, 0, true, &plan);
        planned_inputs.push_back (&input);
        queue_plans.push_back (plan);
        if (resolved.writer_seen)
            ++send_count;
        if (resolved.reader_seen)
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
    uint64_t auto_direction_count = 0;
    uint64_t max_queue_id = 0;
    auto_hwm_role_t auto_role = auto_hwm_role_none;
    bool auto_role_uniform = true;
    {
        scoped_lock_t lock (_sync);
        for (size_t i = 0; i != queue_plans.size (); ++i) {
            physical_queue_record_t *direction = planned_inputs[i]->queue.get ();
            const uint64_t target = queue_plans[i].sndhwm;
            update_hwm_target (planned_inputs[i]->queue, target);
            if (direction->queue_id > max_queue_id)
                max_queue_id = direction->queue_id;
            if (!queue_plans[i].manual_sndhwm) {
                ++auto_direction_count;
                if (auto_direction_count == 1)
                    auto_role = queue_plans[i].role;
                else if (auto_role != queue_plans[i].role)
                    auto_role_uniform = false;
            }

            uint64_t applied = direction->applied_hwm.load (
              std::memory_order_acquire);
            if (applied == 0) {
                applied = queue_plans[i].maximum_hwm_bytes;
                context_->aggregate_hwm_valid = false;
            }
            total_applied = add_snapshot_value (
              total_applied, applied, &context_->aggregate_overflow);
        }
    }
    context_->total_applied_hwm_bytes = total_applied;
    if (context_->aggregate_overflow)
        context_->budget_insufficient = true;
    //  Everything the attach extension needs is published into the plan
    //  record itself. A mixed role leaves the role at `none`, which is what
    //  makes the extension refuse the plan.
    context_->application_auto_direction_count = auto_direction_count;
    context_->application_auto_role =
      auto_role_uniform ? auto_role : auto_hwm_role_none;
    context_->max_planned_queue_id = max_queue_id;
}

bool zlink::ctx_physical_queue_registry_t::extend_application_plan (
  auto_hwm_context_plan_t *context_,
  const physical_queue_endpoint_policy_t *policies_, size_t policy_count_)
{
    //  `*context_` is the plan record itself and the only description of the
    //  last planning pass this function reads or writes. No copy of it lives
    //  in the registry, so there is nothing to keep in step and no
    //  invalidation rule: everything below is derived here and published
    //  back into the same record.
    if (!context_ || !policies_ || policy_count_ == 0
        || policy_count_ > extension_capacity)
        return false;
    //  The plan must describe automatic directions that share one role, be
    //  free of unlimited manual reservations, and carry no overflow or
    //  shortfall. Anything else has no single water level to extend.
    if (!context_->enabled || context_->aggregate_overflow
        || context_->budget_insufficient || !context_->aggregate_hwm_valid
        || context_->unlimited_manual_queue_count != 0
        || context_->application_auto_role == auto_hwm_role_none
        || context_->application_auto_direction_count == 0)
        return false;

    physical_queue_handle_t extensions[extension_capacity];
    size_t extension_count = 0;
    uint64_t added_send = 0;
    uint64_t added_receive = 0;
    bool overflow = false;

    scoped_lock_t lock (_sync);
    for (size_t i = 0; i != policy_count_; ++i) {
        const physical_queue_endpoint_policy_t &policy = policies_[i];
        //  Anything the extension cannot resolve exactly — a manual endpoint,
        //  a disabled or role-less policy, a queue that is not a live
        //  application direction, a role other than the plan's — falls back
        //  to the full replan.
        if (!policy.queue || !policy.planning_enabled || policy.manual
            || policy.role != context_->application_auto_role
            || !find_locked (policy.queue)
            || policy.queue->endpoint_refs == 0
            || policy.queue->lane.load (std::memory_order_acquire)
                 != physical_queue_lane_application)
            return false;
        //  Water-filling hands its division remainder out in stable queue-ID
        //  order. A direction whose ID is above every direction the plan
        //  covers is outside that prefix and is new to the plan; at or below
        //  it, only the full replan can tell the two apart.
        if (policy.queue->queue_id <= context_->max_planned_queue_id)
            return false;

        physical_queue_record_t *const record = policy.queue.get ();
        stored_endpoint_policy_t &stored =
          policy.writer ? record->writer_policy : record->reader_policy;
        stored.present = true;
        stored.role = policy.role;
        stored.manual = policy.manual;
        stored.planning_enabled = policy.planning_enabled;
        stored.hwm = policy.hwm;

        //  Resolve the direction through the same rule the full replan uses.
        resolved_direction_t resolved;
        resolve_direction (context_->profile, *record, &resolved);
        if (!resolved.planning_enabled || !resolved.auto_seen
            || resolved.finite_manual_seen
            || resolved.role != context_->application_auto_role)
            return false;

        extensions[extension_count++] = policy.queue;
        if (resolved.writer_seen)
            ++added_send;
        if (resolved.reader_seen)
            ++added_receive;
    }

    const uint64_t added_count = extension_count;
    const uint64_t new_direction_count = add_snapshot_value (
      context_->active_directional_queue_count, added_count, &overflow);
    const uint64_t new_auto_count = add_snapshot_value (
      context_->application_auto_direction_count, added_count, &overflow);
    if (overflow)
        return false;

    //  An explicit core budget always wins; otherwise the effective cap is
    //  re-resolved with the new count exactly as the full replan does.
    const uint64_t new_budget =
      context_->configured_core_budget_bytes > 0
        ? context_->configured_core_budget_bytes
        : auto_hwm_effective_budget_bytes (
            context_->profile, context_->resolved_memory_limit_bytes,
            new_direction_count);
    if (context_->manual_reserved_hwm_bytes > new_budget)
        return false;

    //  Water-filling over one role: every automatic direction starts at the
    //  role minimum and rises by the same share of what is left, capped at
    //  the role maximum. The division remainder is then handed out one byte
    //  at a time in queue-ID order, so the first `remainder` directions sit
    //  at `level + 1` — a prefix the directions attaching now are above.
    const uint64_t minimum = auto_hwm_profile_minimum_bytes (
      context_->profile, context_->application_auto_role);
    const uint64_t maximum = auto_hwm_profile_maximum_bytes (
      context_->profile, context_->application_auto_role);
    const uint64_t data_budget =
      new_budget - context_->manual_reserved_hwm_bytes;
    const uint64_t minimum_total =
      multiply_snapshot_value (minimum, new_auto_count, &overflow);
    if (overflow || minimum_total > data_budget)
        return false;
    const uint64_t remaining = data_budget - minimum_total;
    const uint64_t share = remaining / new_auto_count;
    const uint64_t headroom = maximum - minimum;
    const bool saturated = share >= headroom;
    const uint64_t level = saturated ? maximum : minimum + share;
    const uint64_t remainder =
      saturated ? 0 : remaining - share * new_auto_count;
    const uint64_t auto_total = add_snapshot_value (
      multiply_snapshot_value (level, new_auto_count, &overflow), remainder,
      &overflow);
    const uint64_t new_total_planned = add_snapshot_value (
      auto_total, context_->manual_reserved_hwm_bytes, &overflow);
    if (overflow)
        return false;

    //  Only the attaching directions are written, and each takes exactly
    //  `level`: their IDs are above the remainder prefix. When the level fell,
    //  the directions already in the plan keep their published target, which
    //  is why this plan records an applied total above its planned total —
    //  the state that tells the debounced replan there is work to finish
    //  (ctx_auto_hwm_state_t::recalc_due). That is the deferral
    //  06-auto-hwm.ko.md §2 grants this path for an O(log n) attach.
    uint64_t total_applied = context_->total_applied_hwm_bytes;
    uint64_t max_queue_id = context_->max_planned_queue_id;
    bool aggregate_hwm_valid = context_->aggregate_hwm_valid;
    for (size_t i = 0; i != extension_count; ++i) {
        update_hwm_target (extensions[i], level);
        if (extensions[i]->queue_id > max_queue_id)
            max_queue_id = extensions[i]->queue_id;
        uint64_t applied =
          extensions[i]->applied_hwm.load (std::memory_order_acquire);
        if (applied == 0) {
            applied = maximum;
            aggregate_hwm_valid = false;
        }
        total_applied = add_snapshot_value (total_applied, applied, &overflow);
    }
    const uint64_t new_send_count = add_snapshot_value (
      context_->active_send_queue_count, added_send, &overflow);
    const uint64_t new_receive_count = add_snapshot_value (
      context_->active_receive_queue_count, added_receive, &overflow);
    if (overflow) {
        //  The attaching directions already carry the new level, and the
        //  full replan the caller falls back to recomputes every aggregate
        //  from the registry, so leaving the plan record untouched here is
        //  safe.
        return false;
    }

    context_->effective_core_budget_bytes = new_budget;
    context_->active_directional_queue_count = new_direction_count;
    context_->active_send_queue_count = new_send_count;
    context_->active_receive_queue_count = new_receive_count;
    context_->application_auto_direction_count = new_auto_count;
    context_->max_planned_queue_id = max_queue_id;
    context_->total_planned_hwm_bytes = new_total_planned;
    context_->total_applied_hwm_bytes = total_applied;
    context_->aggregate_hwm_valid = aggregate_hwm_valid;
    return true;
}

void zlink::ctx_physical_queue_registry_t::record_endpoint_policy (
  const physical_queue_endpoint_policy_t &policy_)
{
    if (!policy_.queue)
        return;
    scoped_lock_t lock (_sync);
    if (!find_locked (policy_.queue))
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

    // The only effect of this call is publishing `planned` into `applied_hwm`.
    // When the two already agree there is nothing to publish, and no
    // registration answer can change that — so the equality is settled on the
    // handle the caller already owns, before the context-wide registry mutex.
    // Every message that drains a pipe reaches this point, and on that path
    // the two are equal.
    if (direction_->planned_hwm.load (std::memory_order_acquire)
        == direction_->applied_hwm.load (std::memory_order_acquire))
        return;

    uint64_t planned = 0;
    uint64_t applied = 0;
    {
        scoped_lock_t lock (_sync);
        if (!find_locked (direction_)
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
        if (!find_locked (direction_))
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
    zlink_assert (_application_reservation.bytes
                  >= direction_->minimum_reservation_bytes);
    _application_reservation.bytes -=
      direction_->minimum_reservation_bytes;
    if (direction_->minimum_reservation_bytes != 0) {
        zlink_assert (_application_reservation.directions != 0);
        --_application_reservation.directions;
    }
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
