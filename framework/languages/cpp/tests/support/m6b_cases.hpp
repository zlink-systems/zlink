/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

namespace zlink::framework::test::m6b
{
void verify_message_follow_invalidation_subscriptions_are_lifetime_safe ();
void verify_message_follow_unsubscribe_waits_for_capture_destruction ();
void verify_message_follow_route_admission_and_suppression ();

void verify_actor_client_authority_generation (bool invalidated);
void verify_actor_calls_keep_selected_route_until_follow_notice ();

void verify_mesh_node_role_is_available_before_local_descriptor_publish ();
void verify_mesh_stop_drains_admitted_request_completion ();
void verify_node_request_requires_remote_admission ();
void verify_unadmitted_request_is_dropped_without_framework_queue ();
void verify_queued_owner_accepts_request_without_blocking_other_owner ();
void verify_local_control_admission ();

void verify_remote_bound_session_bind_returns_terminal_fences ();
void verify_local_session_binding_uses_location_authority ();
void verify_bound_session_bind_uses_only_three_value_actor_fence ();
void verify_bind_handler_rejects_missing_session_owner_descriptor (bool test_seal_drop = true);

void verify_bound_session_push_uses_session_registry_when_gateway_projection_rejects ();
void verify_bound_session_push_admission_uses_only_session_owned_fields ();
void verify_bound_session_push_source_does_not_prejudge_current_binding ();

void verify_spot_id_contract ();
void verify_spot_route_fence_admission_precedes_body_decode ();
void verify_public_host_route_fence_reads_store_without_second_cache (bool release_lease);
void verify_public_host_send_uses_authority_generation ();
void verify_entry_spot_descriptor_mapping_survives_node_removal ();
void verify_raw_spot_and_actor_routing ();
void verify_dispatch_callbacks_survive_suspension (bool through_mesh, bool dispatch_marker);

void verify_user_spot_execution_mode_registration ();
void verify_self_actor_request_rejected_before_submission ();
void verify_actor_context_survives_coroutine_await ();
void verify_actor_yield_releases_spot_gate_before_reply ();
void verify_same_gate_request_rejected_before_submission ();
void verify_membership_turns_and_independent_infrastructure ();

void verify_creation_terminal_operation_isolation ();
void verify_typed_capacity_retry_uses_second_candidate ();
void verify_actor_create_does_not_replay_after_reciprocal_handover ();
void verify_global_identity_remote_create_and_generation_fence ();
void verify_instance_cold_activation_only_from_intent ();
void verify_creating_spot_subscription_uses_native_object_lifetime ();

void verify_host_send_forwarding_allocations ();
void verify_public_host_dispatches_one_application_record_per_turn ();
void verify_public_host_dispatches_one_node_record_per_claim ();
void verify_public_host_batches_with_finite_permits ();
void verify_public_host_fifo_drains_before_liveness_probe ();
void verify_local_application_enqueue_wakes_dispatch_wait ();
void verify_forwarded_request_return_dispatches_without_source_operation ();

void verify_session_relocation_gateway_commit_is_atomic ();
void verify_session_binding_and_terminal_once ();
void verify_session_ingress_sequence_is_scoped_by_actor_binding ();
void verify_session_route_supports_repeated_relocation ();
void verify_session_route_commit_stores_target_lease ();
void verify_displaced_stream_binding_can_be_restored ();
void verify_verified_remote_stream_binding ();
void verify_root_location_session_seal_timeout_is_startup_snapshot ();
void verify_same_node_session_seal_waits_for_active_ingress ();
void verify_absent_session_seal ();
void verify_configured_session_seal_timeout_closes_actual_owner ();

void verify_actor_commit_is_replayable_until_deadline ();
void verify_reconcile_management_deadline_advances_after_each_attempt ();
void verify_terminal_journal_preserves_outstanding_entries ();
void verify_unbounded_actor_handoff_backlog ();
void verify_relocation_finalize_write_lowers_published_bound ();
void verify_pending_relocation_management_bounds_dispatch_wait ();
void verify_failed_relay_persistence_rearms_dispatch_wait ();
void verify_relocation_assembly_rejects_mismatched_identity_chunk ();
void verify_relocation_assembly_rejects_checksum_mismatch ();
void verify_relocation_id_generation_retries_collisions ();
void verify_draining_target_relocation_prepare (bool already_accepted);
void verify_relocation_failure_code_classification_is_distinct ();

void verify_discovery_owner_metadata_preserved ();
void verify_memory_accepted_record_authority ();
void verify_batched_accepted_record_sources ();
void verify_location_store_accepted_record_authority ();
void verify_atomic_raw_stateful_ingress_commit ();
void verify_relocation_data_duplicates_are_distinct_arrivals ();
void verify_relocation_ingress_boundary_owns_saved_and_follow_only_work ();

void verify_logical_multicast_continues_after_one_target_failure ();
void verify_relocated_source_reply_failure_keeps_terminal_record ();
void verify_raw_terminal_reply_relay ();
void verify_durable_reply_relay_single_winner ();
void verify_aggregate_saved_request_terminal_identity ();
void verify_restored_request_completes_local_native_alias ();
void verify_public_host_dispatches_durable_reply_relay ();

bool verify_remote_user_spot_create_close_terminal_once (bool cleanup_only = false);
}
