/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <string>
#include <string_view>

namespace zlink::framework
{
enum class message_flow_log_mode_t;
}

namespace zlink::framework::test::actor_gateway
{
enum class parked_request_case_t
{
    replay,
    direct_replay,
    former_pending_capacity
};

int actor_identity_validation_is_bounded_and_utf8_exact ();
int actor_send_is_one_shot ();
int stale_session_unbind_preserves_rebind ();
int replaced_session_find_is_exact_and_disconnects_once ();
int bound_actors_are_current_ordered_snapshot ();
int bind_or_get_reuses_same_physical_session_generation ();
int bind_or_get_completion_can_reenter_same_manager ();
int bind_or_get_all_exit_paths_complete_within_deadline ();
int direct_rebind_publication_is_atomic_and_old_disconnect_is_fenced ();
int destroyed_or_recreated_actor_ignores_stale_disconnect_handle ();
int get_or_create_does_not_reuse_disconnected_record ();
int get_or_create_refreshes_foreign_session_record ();
int session_disconnect_is_all_settled_and_token_fenced ();
int logical_disconnect_is_selected_and_keeps_session_live ();
int route_update_preserves_object_generation ();
int exact_session_generation_mismatch_is_invalid_operation ();
int bind_registration_contract_vectors ();
int actor_context_identity_and_source_fence_are_exact ();
int bound_session_route_preserves_private_fences ();
int bound_session_route_suspends_until_owner_lane_runs ();
int bound_session_send_does_not_publish_caller_location ();
int bound_session_ref_normalization_preserves_type_and_rejects_conflicts ();
int bound_session_route_installs_sink_and_fence_together ();
int bound_session_transition_is_atomic_and_idempotent ();
int authority_only_route_update_keeps_physical_session_current ();
int app_bound_session_sender_keeps_arguments_alive_after_route_lookup_suspends ();
int bound_session_push_detaches_before_direct_sink_entry ();
int admitted_bound_session_delivery_keeps_original_session_after_rebind ();
int generated_protobuf_bound_session_uses_typed_serializer_codec ();
int relocation_target_prewarm_publishes_store_confirmed_actor_and_session_fence_atomically ();
int stale_binding_registration_and_relocation_prepare_preserve_current_tenure ();
int relocation_prepare_preserves_runtime_failure ();
int bound_session_relay_admission_is_exact_and_monotonic ();
int session_relay_waiter_accepts_beyond_former_capacity ();
int relay_dispatch_scope_restores_nested_and_exception_state ();
int relay_dispatch_header_follows_handler_await ();
int session_relay_queue_is_ordered_without_blocking_other_actors ();
int session_relay_does_not_start_actor_dispatch_on_session_thread ();
int disconnect_notification_survives_pending_dispatcher_completion ();
int session_manager_disconnect_survives_pending_dispatcher_completion ();
int relay_request_survives_pending_dispatcher_completion ();
int relay_send_survives_pending_dispatcher_completion ();
int actor_request_completion_keeps_dedup_state_owned_after_runtime_wrapper_unwinds ();
int session_disconnect_callback_owns_spot_lifecycle_gate ();
int session_disconnect_resolves_context_without_ingress_metadata (bool replay);
int physical_disconnect_submits_all_before_waiting_for_terminal ();
int session_disconnect_dispatcher_preserves_identity_snapshot ();
int common_session_disconnect_uses_actor_packet_route ();
int old_owner_forwards_cold_probe_via_active_message_follow_route (bool disconnect = false);
int returned_owner_serves_locally_past_retained_message_follow_route ();
int source_cleanup_waits_for_leave_completion_before_erasing_actor ();
int sweep_preserves_pending_leave_added_after_snapshot ();
int leave_notification_travels_node_level_and_reaches_source_entry_spot_once ();
int reconcile_deadline_adopts_target_when_store_shows_committed ();
int reconcile_deadline_fast_fails_when_store_shows_source ();
int reconcile_deadline_fast_fails_when_store_has_no_commit ();
int reconcile_deadline_fast_fails_when_store_is_indeterminate ();
int known_generation_source_leave_claims_transfer ();
int early_zero_generation_leave_waits_for_source_transfer_completion (bool no_follow = false);
int relocation_barrier_fences_late_actor_fifo_admission (bool commit_before_post = false);
int relocation_barrier_fences_late_actor_request_without_deadlock ();
int rebound_session_keeps_prior_ingress_exact_fence ();
int reconnect_push_reaches_new_session_across_two_hosts ();
int old_stream_disconnect_does_not_retire_reconnected_binding ();
int command_38_rebind_is_owned_only_by_new_connection ();
int late_lower_generation_bind_and_record_are_rejected ();
int same_identity_registration_keeps_logical_binding_current ();
int same_rid_registration_retains_retired_close_handler ();
int binding_context_uses_gateway_state_lane ();
int session_bound_callback_policy ();
int duplicate_bind_callback_policy ();
int remote_actor_join_resolves_store_type_and_reports_typed_terminals ();
int remote_actor_join_store_read_does_not_wait_for_handler_workers ();
int join_completion_waits_for_bound_session_delivery_terminal ();
int terminal_reply_precedes_session_route_publication ();
int join_completion_callback_failure_is_reported ();
int actor_terminal_sdk_controlled_counter_projection ();
int actor_native_terminal_cause (std::string_view cause,
                                 zlink::framework::message_flow_log_mode_t mode,
                                 bool meter);
int local_relay_failure_returns_without_waiting_for_handoff_deadline ();
int local_relay_preserves_handoff_request_route (bool has_original_operation,
                                                 bool expire_native = false);
int parked_request_reply_case (const std::string &requester_rid,
                               parked_request_case_t test_case = parked_request_case_t::replay);
int parked_request_without_route_fence_receives_reply_after_replay ();
int pending_handoff_accepts_beyond_former_capacity ();
int same_operation_from_distinct_source_lifecycles_has_distinct_pending_terminal ();
int parked_cross_node_request_terminal_returns_to_parking_node ();
int relayed_request_without_deferred_terminal_receives_follow_reply ();
int actor_unbound_follows_accepted_push_submission ();
int actor_unbound_follows_session_termination (bool physical,
                                               bool disconnect_then_shutdown = false);
int session_disconnect_uses_actor_slot_issue_order ();
int close_unbound_session_reports_invalid_operation ();
int close_exact_session_binding_uses_physical_terminal ();
int actor_typed_wire_header_contract ();
int actor_reply_framing_vectors ();
int actor_error_payload_vectors ();
} // namespace zlink::framework::test::actor_gateway
