/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <vector>

namespace zlink::framework::test::execution
{
enum class mailbox_shutdown_t
{
    none,
    close,
    cancel
};

enum class public_member_policy_t
{
    spot_wide,
    per_actor,
    entry
};

bool verify_timer_handler_activation_lifetime ();
bool verify_throwing_timer_callback_preserves_default_policy ();
bool verify_timer_cancel_generation_contract ();
bool verify_close_waits_for_timer_callback_barrier ();
bool verify_timer_terminal_precedes_lifecycle_close_turn ();
bool verify_timer_self_cancel_completes_after_callback ();
bool verify_idle_timer_closes_before_executor_releases_last_node_reference ();

bool verify_spot_close_waits_for_earlier_lifecycle_join ();
bool verify_suspended_lifecycle_keeps_fifo_and_allows_application ();
bool verify_suspended_lifecycle_resumes_at_original_fifo_head ();
bool verify_ready_lifecycle_continuation_pays_burst_debt ();
bool verify_suspended_lifecycle_cancellation_releases_fifo ();
bool verify_delayed_barrier_activation_uses_retained_turn ();
bool verify_cancellable_serial_submission_lifecycle ();
int verify_deferred_work_keeps_lifecycle_fifo_position ();
int verify_deferred_join_waits_for_handler_terminal_across_yield ();
bool verify_deferred_actual_terminal (bool fail_first, bool fail_handler, bool flat = false);
bool verify_readiness_before_deferred_join ();
bool verify_deferred_burst_terminal (bool flat, bool fail_first);
bool verify_deferred_coroutine_gate_terminal (bool flat, bool fail, bool switch_gate);
bool verify_deferred_terminal_owner (bool flat, bool fail, bool active_application);
bool verify_deferred_extra_boundaries ();
bool verify_deferred_after_lifecycle_prefix (bool flat);
bool verify_durable_policy_late_grant (bool transfer);

bool verify_cpu_worker_completes_in_ordinary_turn ();
int verify_workers_and_spot_gate ();

bool verify_request_turn_mode (bool release_turn, const std::vector<int> &expected);
bool verify_serial_resume_waits_behind_queued_work ();
bool verify_spot_wide_yield_retains_actor_claim ();
bool verify_spot_wide_yield_blocks_idle_eviction_until_handler_terminal ();
bool verify_same_actor_synchronous_reentry_is_immediate ();
bool verify_released_spot_turn_does_not_inline_lifecycle_task ();
bool verify_all_handler_origin_terminals ();

bool verify_serial_queue_lanes_are_unbounded ();
bool verify_transferred_owner_reservation_is_continuous_until_terminal ();
bool verify_transferred_owner_reservation_shares_unbounded_lifecycle_lane ();
bool verify_serial_queue_owner_time_budget ();
bool verify_serial_executor_submission_paths_select_queues ();
bool verify_per_actor_two_actors_overlap ();
bool verify_spot_wide_two_actors_are_serial ();
bool verify_same_actor_fifo_in_both_modes ();
bool verify_per_actor_timer_names_overlap_and_keep_fifo ();
bool verify_actor_mailbox_backlog_is_accepted_in_both_modes ();
bool verify_spot_wide_large_and_small_payloads_are_queued ();
bool verify_spot_wide_upper_queue_accepts_former_boundaries ();
bool verify_serial_lane_policies ();

bool verify_session_control_lifecycle_priority ();
bool verify_fanout_snapshot_state_owner ();
bool verify_async_capture_releases_state_owner ();

bool verify_spot_wide_consumer_waits_for_shared_gate ();
bool verify_spot_wide_cancel_removes_unclaimed_actor_head ();
bool verify_spot_wide_control_returns_without_waiting_for_gate ();
bool verify_spot_wide_stop_settles_queued_actor_rejection ();
bool verify_spot_wide_rejects_actor_when_gate_scheduler_stops ();
bool verify_spot_wide_actor_ready_lane_tracks_lifecycle ();
bool verify_spot_wide_handoff_allows_spot_control ();
bool verify_spot_wide_terminal_retains_gate_lifetime ();
bool verify_spot_serial_task_async_retains_work_until_terminal ();
bool verify_spot_serial_task_async_shutdown_settlement ();
bool verify_seal_cut_takes_gate_and_publications ();
bool verify_cancelled_barrier_settles_running_phase (bool shared,
                                                     bool cancel_queue,
                                                     bool reached_first);
bool verify_barrier_phase_completion_allows_reentrant_phase (bool shared);
bool verify_nested_barrier_phases (bool shared, bool cancel);

bool verify_common_dispatch_limits ();
bool verify_fixture_accounting_boundaries ();
bool verify_fixture_arbitration_and_owner_isolation ();
bool verify_runtime_observation_loss_and_terminal_retention ();

bool verify_draining_host_refuses_new_admission_with_shutting_down ();
bool verify_idle_instance_spot_eviction_closes_local_context ();
bool verify_explicit_instance_spot_close_releases_authority_after_callback ();
bool verify_close_terminal_for_operational_boundary ();
bool verify_spot_close_returns_turn_while_store_step_is_pending ();

bool verify_remote_actor_prepare_is_idempotent ();
bool verify_wire_actor_join_admission_is_approval_only_and_later_attempt_wins ();
bool verify_wire_join_requires_active_local_target ();
bool verify_relocation_abort_does_not_hold_receive_worker ();
bool verify_target_commit_stages_source_prefix_before_live_dispatch ();
bool verify_actor_join_prewarm_parks_arrival_before_prepare ();
bool verify_actor_join_prewarm_newest_attempt_evicts_placeholder ();
bool verify_actor_join_prewarm_fail_commit_clears_parked_backlog ();
bool verify_actor_join_prewarm_newest_attempt_evicts_live_attempt_past_prepare ();

bool verify_join_commit_does_not_wait_for_joined_callback (bool lifecycle_failure = false,
                                                           bool completion_failure = false);
bool verify_actor_join_finalize_replies_after_target_activation ();

bool verify_remote_actor_cutover_completion_is_target_owned (bool cancel_source_delay = false,
                                                             bool unknown_at_teardown = false,
                                                             bool stopped_management = false);
bool verify_remote_actor_completion_keeps_session_ref_until_route_ack ();

bool verify_relocation_ready_keeps_registering_turn_across_state_lane ();
bool verify_relocation_readiness_root_rejection ();
bool verify_relocation_participant_resources (bool fail_reserve, bool abort);
bool verify_relocation_membership_pending_releases_root_turn ();
bool verify_teardown_observes_actor_handoff_terminal ();
bool verify_aggregate_capture_waits_for_participant_terminal ();
bool verify_source_suffix_native_order_and_rollback ();
bool verify_public_local_member_defer (
  bool fail_first,
  bool other_handler = false,
  public_member_policy_t policy = public_member_policy_t::spot_wide);
bool verify_real_join_completion_callback_owner (int policy,
                                                 bool yield_callback,
                                                 bool close_before_callback = false);
int verify_actor_join_completion_observations ();
bool verify_bulk_deferred_actor_joins ();

bool verify_durable_head_owner_transitions ();
bool verify_durable_head_rejection_and_close ();
bool verify_accepted_continuation_survives_shutdown (bool close_before_resume);
bool verify_direct_continuation_after_close (bool shared);
bool verify_durable_tail_across_controls_and_export ();
bool verify_inactive_barrier_and_cancel_reentry (bool shared);
bool verify_fifo_indexes_across_shutdown (bool shared);
bool verify_idle_mailbox_terminal (bool shared, mailbox_shutdown_t shutdown);
bool verify_seal_waits_for_yielded_handler_terminal ();
bool verify_intermediate_resume_keeps_handler_terminal ();
int verify_application_queue_limits_and_metrics ();
int verify_logical_multicast_observation ();
}
