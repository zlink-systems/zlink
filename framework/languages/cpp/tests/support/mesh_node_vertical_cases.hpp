/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

namespace framework_mesh_node_vertical_tests
{
void verify_object_client_registration_boundary ();
void verify_mesh_start_and_rollback_from_completion_turn ();
void verify_unselected_object_role_defaults_to_none ();
void verify_route_internal_packets_precede_application_dispatch ();
void verify_local_join_timeout_releases_membership ();
void verify_actor_route_resolver_preserves_unavailable ();
void verify_server_descriptor_publishes_framework_entry_spot_without_application_entry ();
void verify_descriptor_retire_order_and_pre_seal_rollback ();
void verify_local_node_submit_bridge ();
void verify_direct_target_falls_through_absent_location_store_entry ();
void verify_request_to_never_admitted_target_reports_not_found ();
void LocalPlacementIgnoresStaleStoreResult ();
void LocalLifecycleIgnoresStaleStoreResult ();
void verify_local_host_lifecycle_uses_runtime_owner ();
void verify_observation_starts_with_weight_changed_before_subscription ();
void verify_public_runtime_surface ();
void verify_automatic_intent_respects_manual_peer ();
void verify_manual_peer_public_status ();
void verify_location_store_blocks_placement ();
void verify_automatic_identity_and_port_builder ();
void verify_monitor_pump_does_not_wait_for_spot_lane ();
void verify_management_does_not_wait_for_spot_lane ();
void verify_slow_observer_does_not_block_stop ();
void verify_host_shutdown_seal_reaches_raw_mesh ();
void verify_fixed_drain_callback_barrier ();
void verify_accepted_handler_push_after_host_seal (bool shutdown);
void verify_local_and_wire_application_owner_keys_match ();
void verify_owner_drain_continuation_during_executor_shutdown ();
void verify_deferred_application_terminal_ownership ();
bool verify_application_terminal_drain_waits_for_capture_destruction ();
#if defined(__unix__)
int run_cross_process_delivery ();
#endif
void verify_peer_intent_ids_and_callback_lifetime ();
void verify_repeated_mesh_connect_keeps_one_physical_connection ();
} // namespace framework_mesh_node_vertical_tests
