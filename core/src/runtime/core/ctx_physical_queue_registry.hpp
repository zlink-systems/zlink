/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CTX_PHYSICAL_QUEUE_REGISTRY_HPP_INCLUDED__
#define __ZLINK_CTX_PHYSICAL_QUEUE_REGISTRY_HPP_INCLUDED__

#include <atomic>
#include <map>
#include <memory>
#include <vector>

#include "core/auto_hwm_policy.hpp"
#include "core/options.hpp"
#include "utils/mutex.hpp"
#include "utils/stdint.hpp"

namespace zlink
{
class pipe_t;
struct physical_queue_record_t;
typedef std::shared_ptr<physical_queue_record_t> physical_queue_handle_t;

struct retained_credit_control_t;
struct retained_credit_origin_t;
struct decoder_frame_reservation_control_t;
struct decoder_frame_reservation_t;

struct decoder_frame_reservation_request_t
{
    decoder_frame_reservation_request_t ();

    uint64_t payload_bytes;
    unsigned char msg_flags;
    bool multipart_started_empty;
};

//  Move-only ownership for one physical frame removed from a queue without
//  publishing writer credit. Socket framing code may hold this token while it
//  prefetches or parses an envelope; only the public retained receive entry
//  converts it to application ownership.
class retained_credit_token_t
{
  public:
    retained_credit_token_t ();
    ~retained_credit_token_t ();
    retained_credit_token_t (retained_credit_token_t &&other_);
    retained_credit_token_t &operator= (retained_credit_token_t &&other_);

    bool empty () const;
    void reset ();
    int transfer_to_application (
      std::shared_ptr<retained_credit_origin_t> *origin_out_);

  private:
    friend class ctx_physical_queue_registry_t;
    explicit retained_credit_token_t (
      const std::shared_ptr<retained_credit_origin_t> &origin_);

    std::shared_ptr<retained_credit_origin_t> _origin;

    retained_credit_token_t (const retained_credit_token_t &);
    retained_credit_token_t &operator= (const retained_credit_token_t &);
};

void release_retained_credit_origin (
  std::shared_ptr<retained_credit_origin_t> *origin_);

//  Decoder reservations can outlive a pipe generation while an engine is
//  unwinding.  Release through the registry-owned control block so an old
//  token never dereferences a detached pipe or a destroyed registry.
void release_decoder_frame_reservation (
  decoder_frame_reservation_t **reservation_);

struct physical_queue_registry_snapshot_t
{
    physical_queue_registry_snapshot_t ();

    uint64_t active_application_direction_count;
    uint64_t active_completion_direction_count;
    uint64_t retired_direction_count;
    uint64_t application_current_accounted_bytes;
    uint64_t application_provisional_accounted_bytes;
    uint64_t application_peak_accounted_bytes;
    uint64_t completion_current_accounted_bytes;
    uint64_t completion_peak_accounted_bytes;
    uint64_t completion_pending_message_count;
    uint64_t monitor_applied_hwm_bytes;
    uint64_t monitor_current_accounted_bytes;
    uint64_t application_lease_accounted_bytes;
    uint64_t outstanding_application_lease_count;
    uint64_t deferred_origin_credit_bytes;
    uint64_t oversize_admission_count;
    uint64_t largest_oversize_message_bytes;
    uint64_t total_admission_attempts;
    uint64_t first_blocked_admission_attempts;
    uint32_t blocked_ratio_ppm;
    bool aggregate_overflow;
};

struct physical_queue_endpoint_policy_t
{
    physical_queue_endpoint_policy_t ();

    physical_queue_handle_t queue;
    auto_hwm_role_t role;
    bool writer;
    bool manual;
    bool planning_enabled;
    uint64_t hwm;
};

//  Owns the identity and lifecycle of physical ypipe directions. A record is
//  shared by the reader and writer pipe endpoints of exactly one direction;
//  endpoint objects never maintain independent copies of registry state.
class ctx_physical_queue_registry_t
{
  public:
    ctx_physical_queue_registry_t ();
    ~ctx_physical_queue_registry_t ();

    int create_pipepair_queues (
      uint64_t first_direction_hwm_,
      uint64_t second_direction_hwm_,
      physical_queue_class_t queue_class_,
      auto_hwm_role_t role_,
      bool planning_enabled_,
      const auto_hwm_context_plan_t &context_plan_,
      physical_queue_handle_t *first_direction_,
      physical_queue_handle_t *second_direction_);
    void classify_pipepair_queues (
      const physical_queue_handle_t &first_direction_,
      const physical_queue_handle_t &second_direction_,
      transport_lane_t lane_);
    void account_provisional_frame (const physical_queue_handle_t &direction_,
                                    uint64_t frame_bytes_);
    void commit_message (const physical_queue_handle_t &direction_,
                         uint64_t final_frame_bytes_,
                         bool counted_message_,
                         bool oversize_admission_);
    void rollback_provisional (const physical_queue_handle_t &direction_);
    void release_committed_frame (const physical_queue_handle_t &direction_,
                                  uint64_t frame_bytes_,
                                  uint64_t counted_message_count_);
    int retain_dequeued_frame (const physical_queue_handle_t &direction_,
                               pipe_t *reader_pipe_,
                               uint64_t frame_bytes_,
                               uint64_t counted_message_count_,
                               retained_credit_token_t *token_out_);
    int reserve_decoder_frame (
      const physical_queue_handle_t &direction_,
      const decoder_frame_reservation_request_t &request_,
      decoder_frame_reservation_t **reservation_out_);
    int commit_decoder_frame (
      decoder_frame_reservation_t **reservation_, uint64_t payload_bytes_,
      unsigned char msg_flags_, bool counted_message_,
      bool *oversize_admission_out_);
    void release_decoder_frame (decoder_frame_reservation_t **reservation_);
    void plan_application_queues (
      auto_hwm_context_plan_t *context_,
      const std::vector<physical_queue_endpoint_policy_t> &policies_);
    void record_endpoint_policy (
      const physical_queue_endpoint_policy_t &policy_);
    void record_admission_attempt (bool blocked_by_target_hwm_);
    void update_hwm_target (const physical_queue_handle_t &direction_,
                            uint64_t target_hwm_);
    uint64_t planned_hwm (const physical_queue_handle_t &direction_) const;
    uint64_t applied_hwm (const physical_queue_handle_t &direction_) const;
    uint64_t current_accounted_bytes (
      const physical_queue_handle_t &direction_) const;
    uint64_t generation (const physical_queue_handle_t &direction_) const;
    bool snapshot_queue_empty (
      const physical_queue_handle_t &direction_) const;
    void advance_generation (const physical_queue_handle_t &direction_);
    void release_endpoint (physical_queue_handle_t *direction_);
    void snapshot (physical_queue_registry_snapshot_t *out_) const;
    void reset_metrics ();
    void stop_retained_transfers ();
    void force_release_retained_credit ();

  private:
    friend class retained_credit_token_t;
    friend void release_retained_credit_origin (
      std::shared_ptr<retained_credit_origin_t> *origin_);
    friend void release_decoder_frame_reservation (
      decoder_frame_reservation_t **reservation_);
    uint64_t allocate_queue_id_unlocked ();

    mutable mutex_t _sync;
    std::map<uint64_t, physical_queue_handle_t> _directions;
    uint64_t _next_queue_id;
    uint64_t _application_reserved_minimum_bytes;
    std::atomic<uint64_t> _application_current_accounted_bytes;
    std::atomic<uint64_t> _application_provisional_accounted_bytes;
    std::atomic<uint64_t> _application_peak_accounted_bytes;
    std::atomic<uint64_t> _completion_current_accounted_bytes;
    std::atomic<uint64_t> _completion_peak_accounted_bytes;
    std::atomic<uint64_t> _completion_pending_message_count;
    std::atomic<uint64_t> _monitor_current_accounted_bytes;
    std::atomic<uint64_t> _application_lease_accounted_bytes;
    std::atomic<uint64_t> _outstanding_application_lease_count;
    std::atomic<uint64_t> _deferred_origin_credit_bytes;
    std::atomic<uint64_t> _oversize_admission_count;
    std::atomic<uint64_t> _largest_oversize_message_bytes;
    std::atomic<uint64_t> _total_admission_attempts;
    std::atomic<uint64_t> _first_blocked_admission_attempts;
    std::atomic<bool> _aggregate_overflow;
    std::shared_ptr<retained_credit_control_t> _retained_control;
    std::map<uint64_t, retained_credit_origin_t *> _retained_origins;
    uint64_t _next_retained_origin_id;
    std::shared_ptr<decoder_frame_reservation_control_t> _decoder_control;
    std::map<uint64_t, decoder_frame_reservation_t *> _decoder_reservations;
    uint64_t _next_decoder_reservation_id;

    bool transfer_retained_origin_to_application (
      retained_credit_origin_t *origin_);
    void release_retained_origin (retained_credit_origin_t *origin_,
                                  bool force_);
    void erase_direction_if_retired_and_drained_unlocked (
      const physical_queue_handle_t &direction_);
    void cancel_decoder_reservations_unlocked (
      const physical_queue_handle_t &direction_, uint64_t generation_);
    void refund_decoder_reservation_unlocked (
      decoder_frame_reservation_t *reservation_);
    void release_decoder_frame_unlocked (
      decoder_frame_reservation_t *reservation_);
    void force_cancel_decoder_reservations ();

    ZLINK_NON_COPYABLE_NOR_MOVABLE (ctx_physical_queue_registry_t)
};
}

#endif
