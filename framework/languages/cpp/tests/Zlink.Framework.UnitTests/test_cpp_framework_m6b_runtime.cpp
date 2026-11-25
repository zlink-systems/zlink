/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/dispatch/coroutine_executor.hpp"
#include "../support/m6b_cases.hpp"
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include <initializer_list>
#include <string_view>

using namespace zlink::framework::test::m6b;

int main (int argc, char **argv)
{
    zlink::framework::completion_owner_t completion_owner;
    if (argc == 3 && std::string_view (argv[1]) == "--actor-authority-generation") {
        zlink::framework::runtime::install_host_context_hooks ();
        verify_actor_client_authority_generation (std::string_view (argv[2]) == "invalidated");
        return 0;
    }
    // This test drives runtime parts without a host.
    zlink::framework::runtime::install_host_context_hooks ();
    if (argc == 2 && std::string_view (argv[1]) == "--message-follow-capture-lifetime") {
        verify_message_follow_unsubscribe_waits_for_capture_destruction ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--telemetry-create-close")
        return verify_remote_user_spot_create_close_terminal_once () ? 0 : 1;
    if (argc == 2 && std::string_view (argv[1]) == "--telemetry-returned-request") {
        verify_forwarded_request_return_dispatches_without_source_operation ();
        return 0;
    }
    verify_host_send_forwarding_allocations ();
    if (argc == 2 && std::string_view (argv[1]) == "--returned-request-dispatch") {
        verify_forwarded_request_return_dispatches_without_source_operation ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--send-forwarding-allocations")
        return 0;
    if (argc == 2 && std::string_view (argv[1]) == "--send-payload-lifetime") {
        verify_public_host_dispatches_one_application_record_per_turn ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--accepted-record-authority") {
        verify_discovery_owner_metadata_preserved ();
        verify_memory_accepted_record_authority ();
        verify_batched_accepted_record_sources ();
        verify_location_store_accepted_record_authority ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--aggregate-request-identity") {
        verify_aggregate_saved_request_terminal_identity ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--restored-request-alias") {
        verify_local_control_admission ();
        verify_restored_request_completes_local_native_alias ();
        return 0;
    }

    if (argc == 3 && std::string_view (argv[1]) == "--dispatch-callback-lifetime") {
        const std::string_view mode (argv[2]);
        verify_dispatch_callbacks_survive_suspension (mode.starts_with ("mesh"),
                                                      mode.ends_with ("dispatch"));
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--dispatch-callback-lifetime") {
        for (const bool through_mesh : {false, true})
            for (const bool dispatch_marker : {false, true})
                verify_dispatch_callbacks_survive_suspension (through_mesh, dispatch_marker);
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--bind-registration-terminal") {
        verify_remote_bound_session_bind_returns_terminal_fences ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--descriptor-bind-handler") {
        verify_bind_handler_rejects_missing_session_owner_descriptor (false);
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--descriptor-seal-drop") {
        verify_bind_handler_rejects_missing_session_owner_descriptor ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--creating-subscription") {
        verify_creating_spot_subscription_uses_native_object_lifetime ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--unadmitted-request") {
        verify_unadmitted_request_is_dropped_without_framework_queue ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--owner-request-rejection") {
        verify_queued_owner_accepts_request_without_blocking_other_owner ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--fifo-liveness") {
        verify_public_host_fifo_drains_before_liveness_probe ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--handoff-backlog") {
        verify_unbounded_actor_handoff_backlog ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--accepted-payload-owner") {
        verify_atomic_raw_stateful_ingress_commit ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--binding-absent") {
        verify_absent_session_seal ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--session-owner-route") {
        verify_same_node_session_seal_waits_for_active_ingress ();
        verify_session_relocation_gateway_commit_is_atomic ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--preparation-cleanup-terminal") {
        return verify_remote_user_spot_create_close_terminal_once (true) ? 0 : 1;
    }
    if (!verify_remote_user_spot_create_close_terminal_once (true))
        return 1;
    verify_actor_create_does_not_replay_after_reciprocal_handover ();
    verify_message_follow_invalidation_subscriptions_are_lifetime_safe ();
    verify_actor_client_authority_generation (false);
    verify_actor_client_authority_generation (true);
    verify_actor_calls_keep_selected_route_until_follow_notice ();
    verify_session_relocation_gateway_commit_is_atomic ();
    verify_mesh_node_role_is_available_before_local_descriptor_publish ();
    verify_mesh_stop_drains_admitted_request_completion ();
    verify_remote_bound_session_bind_returns_terminal_fences ();
    verify_local_session_binding_uses_location_authority ();
    verify_bound_session_bind_uses_only_three_value_actor_fence ();
    verify_bind_handler_rejects_missing_session_owner_descriptor ();
    verify_bound_session_push_uses_session_registry_when_gateway_projection_rejects ();
    verify_bound_session_push_admission_uses_only_session_owned_fields ();
    verify_bound_session_push_source_does_not_prejudge_current_binding ();
    verify_spot_id_contract ();
    verify_spot_route_fence_admission_precedes_body_decode ();
    verify_public_host_route_fence_reads_store_without_second_cache (false);
    verify_public_host_route_fence_reads_store_without_second_cache (true);
    verify_public_host_send_uses_authority_generation ();
    verify_entry_spot_descriptor_mapping_survives_node_removal ();
    verify_user_spot_execution_mode_registration ();
    verify_self_actor_request_rejected_before_submission ();
    verify_actor_context_survives_coroutine_await ();
    verify_actor_yield_releases_spot_gate_before_reply ();
    verify_same_gate_request_rejected_before_submission ();
    verify_creation_terminal_operation_isolation ();
    verify_typed_capacity_retry_uses_second_candidate ();
    verify_global_identity_remote_create_and_generation_fence ();
    verify_membership_turns_and_independent_infrastructure ();
    verify_instance_cold_activation_only_from_intent ();
    verify_session_binding_and_terminal_once ();
    verify_session_ingress_sequence_is_scoped_by_actor_binding ();
    verify_session_route_supports_repeated_relocation ();
    verify_session_route_commit_stores_target_lease ();
    verify_displaced_stream_binding_can_be_restored ();
    verify_verified_remote_stream_binding ();
    verify_message_follow_route_admission_and_suppression ();
    verify_actor_commit_is_replayable_until_deadline ();
    verify_reconcile_management_deadline_advances_after_each_attempt ();
    verify_terminal_journal_preserves_outstanding_entries ();
    verify_unbounded_actor_handoff_backlog ();
    verify_public_host_dispatches_one_application_record_per_turn ();
    verify_public_host_dispatches_one_node_record_per_claim ();
    verify_public_host_batches_with_finite_permits ();
    verify_public_host_fifo_drains_before_liveness_probe ();
    verify_logical_multicast_continues_after_one_target_failure ();
    verify_creating_spot_subscription_uses_native_object_lifetime ();
    verify_local_application_enqueue_wakes_dispatch_wait ();
    verify_relocation_finalize_write_lowers_published_bound ();
    verify_pending_relocation_management_bounds_dispatch_wait ();
    verify_failed_relay_persistence_rearms_dispatch_wait ();
    verify_root_location_session_seal_timeout_is_startup_snapshot ();
    verify_absent_session_seal ();
    verify_same_node_session_seal_waits_for_active_ingress ();
    verify_configured_session_seal_timeout_closes_actual_owner ();
    verify_discovery_owner_metadata_preserved ();
    verify_memory_accepted_record_authority ();
    verify_batched_accepted_record_sources ();
    verify_location_store_accepted_record_authority ();
    verify_raw_spot_and_actor_routing ();
    verify_atomic_raw_stateful_ingress_commit ();
    verify_relocated_source_reply_failure_keeps_terminal_record ();
    verify_forwarded_request_return_dispatches_without_source_operation ();
    verify_node_request_requires_remote_admission ();
    verify_unadmitted_request_is_dropped_without_framework_queue ();
    verify_queued_owner_accepts_request_without_blocking_other_owner ();
    verify_aggregate_saved_request_terminal_identity ();
    verify_restored_request_completes_local_native_alias ();
    verify_local_control_admission ();
    verify_raw_terminal_reply_relay ();
    verify_relocation_data_duplicates_are_distinct_arrivals ();
    verify_relocation_ingress_boundary_owns_saved_and_follow_only_work ();
    verify_durable_reply_relay_single_winner ();
    verify_public_host_dispatches_durable_reply_relay ();
    verify_remote_user_spot_create_close_terminal_once ();
    verify_relocation_assembly_rejects_mismatched_identity_chunk ();
    verify_relocation_assembly_rejects_checksum_mismatch ();
    verify_relocation_id_generation_retries_collisions ();
    verify_draining_target_relocation_prepare (false);
    verify_draining_target_relocation_prepare (true);
    verify_relocation_failure_code_classification_is_distinct ();
    return 0;
}
