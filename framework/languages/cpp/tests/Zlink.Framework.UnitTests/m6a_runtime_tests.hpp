/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <service_wire_constants.hpp>
#include <string_view>
#include <zlink/framework/contracts/dispatch/execution.hpp>

namespace zlink::framework::test::m6a
{
namespace protocol = runtime::protocol;

void verify_node_request_submit_cost ();
void verify_request_submission_results ();
void verify_node_send_forwarding_allocations ();
void verify_bound_session_close_reply_grammar ();
void verify_bound_session_bind_retries_until_route_is_admitted ();
void verify_bound_session_bind_exposes_only_accepted_attempt ();
void verify_bound_session_bind_missing_descriptor (std::string_view outcome);
void verify_bound_session_bind_does_not_replay_after_route_change ();
void verify_bound_session_bind_unadmitted_deadline_fails_both_stages ();
void verify_bound_session_bind_permanent_absence_is_bounded ();
void verify_bound_session_bind_reply_completes_registered_operation ();
void verify_standby_promotion_public_send (bool observe_remote_first);
void verify_standby_promotion_readmits_both_peers ();
void verify_stale_rid_disconnect_preserves_same_endpoint_replacement ();
void verify_bilateral_raw_connection_without_public_pipe_id_keeps_survivor (bool lower_first);
void verify_bilateral_raw_connection_without_public_pipe_id_keeps_survivor ();
void verify_actor_create_command_49_roundtrip ();
void verify_actor_create_retries_until_route_is_admitted ();
void verify_actor_create_intent_removal_ends_operation ();
void verify_actor_create_readiness_deadline_is_timed_out ();
void verify_actor_create_from_dispatch_thread_does_not_block ();
void verify_topology_snapshot_and_connection_fence ();
void verify_route_mesh_descriptor_uses_generated_capability ();
void verify_admission_follows_latest_observed_route ();
void verify_lifecycle_token_requires_current_discovery_expectation ();
void verify_raw_admission_rejects_lifecycle_mismatch ();
void verify_object_client_connection_requirement ();
void verify_manual_object_client_pair_ends_not_required ();
void verify_signed_weight_contract ();
void verify_durable_attempt_waits_for_admit (protocol::command kind);
void verify_topology_ready_is_remote_admit_on_current_route ();
void verify_unadmitted_route (bool repeat_hello);
void verify_envelope_body_retains_native_storage ();
void verify_application_owner_drain_transitions ();
void verify_independent_mailbox_domains_and_claim_fence ();
void verify_completion_receipt_without_application_receive ();
void verify_liveness_shared_record_contract ();
void verify_liveness_reuses_probe_and_fences_reconnect ();
void verify_liveness_physical_close (bool receive, bool replacement);
void verify_liveness_without_selected_route ();
#if !defined(_WIN32)
void verify_liveness_propagates_binding_failure (bool missing = false);
#endif
void verify_location_descriptor_cas_snapshot_and_watch ();
void verify_manual_and_automatic_classic_fanout ();
void verify_client_server_stale_admission_reply_is_discarded ();
void verify_client_server_close_drains_parked_control_reply ();
void verify_client_server_plain_hello_is_rejected ();
void verify_client_server_independent_raw_path ();
void verify_client_server_admits_before_monitor_drain ();
void verify_client_server_weighted_selection ();
void verify_raw_owner_node_send_and_liveness (
  zlink::framework::message_flow_log_mode_t mode = zlink::framework::message_flow_log_mode_t::off,
  bool framework_multipart = false);
void verify_relocation_prepare_failed_reply_resolves_promptly_with_identity_fencing ();
void verify_relocation_prepare_failed_reply_with_mismatched_identity_is_fenced ();
void verify_prepare_does_not_replay_on_selected_route ();
void verify_authority_generation_request (std::string_view path, bool invalidated);
void verify_authority_generation_send ();
void verify_actor_join_waits_for_admit_without_replay ();
void verify_actor_join_durable_terminals ();
void verify_actor_join_ends_after_unexpected_admitted_peer_loss (bool monitor_disconnect);
void verify_actor_join_accepted_reply_threads_chunk_limit_and_epoch ();
void verify_actor_join_rejected_reply_completes_typed_failure ();
void verify_actor_join_wrong_source_generation_is_fenced ();
void verify_actor_join_mismatched_correlation_reply_classifies_protocol_error ();
void verify_actor_join_target_fence_reaches_admission ();
}
