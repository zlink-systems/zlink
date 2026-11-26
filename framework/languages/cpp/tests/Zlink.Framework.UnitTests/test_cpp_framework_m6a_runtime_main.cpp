/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_runtime_tests.hpp"
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include <cstdio>
#include <string>

int main (int argc, char **argv)
{
    using namespace zlink::framework::test::m6a;
    zlink::framework::completion_owner_t completion_owner;
    if (argc == 4 && std::string_view (argv[1]) == "--authority-generation-case") {
        verify_authority_generation_request (argv[2], std::string_view (argv[3]) == "invalidated");
        return 0;
    }
    if (argc == 3 && std::string_view (argv[1]) == "--bound-session-descriptor-case") {
        verify_bound_session_bind_missing_descriptor (argv[2]);
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--bound-session-missing-descriptor") {
        for (const auto outcome : {"late", "timeout", "invalidated", "other-generation",
                                   "invalidated-other-generation", "late-endpoint-failure"})
            verify_bound_session_bind_missing_descriptor (outcome);
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--request-submit-cost") {
        verify_node_request_submit_cost ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--application-ingress-after-local-admit") {
        verify_application_ingress_after_local_admit ();
        return 0;
    }
    verify_node_request_submit_cost ();
    verify_node_send_forwarding_allocations ();
    if (argc == 2 && std::string_view (argv[1]) == "--send-forwarding-allocations")
        return 0;
    if (argc == 4 && std::string_view (argv[1]) == "--only") {
        const std::string_view name (argv[2]);
        const int repetitions = std::stoi (argv[3]);
        if (repetitions <= 0)
            return 2;
        for (int i = 0; i < repetitions; ++i) {
            if (name == "pre-bind")
                verify_durable_attempt_waits_for_admit (protocol::command::boundSessionBind);
            else if (name == "pre-create")
                verify_durable_attempt_waits_for_admit (protocol::command::actorCreate);
            else if (name == "pre-join")
                verify_durable_attempt_waits_for_admit (protocol::command::actorJoin);
            else if (name == "retries")
                verify_bound_session_bind_retries_until_route_is_admitted ();
            else if (name == "close-reply")
                verify_bound_session_close_reply_grammar ();
            else if (name == "client-server-close-drain")
                verify_client_server_close_drains_parked_control_reply ();
            else if (name == "silent")
                verify_unadmitted_route (false);
            else if (name == "prepare")
                verify_prepare_does_not_replay_on_selected_route ();
            else if (name == "hello")
                verify_unadmitted_route (true);
            else if (name == "native-standby")
                verify_standby_promotion_public_send (false);
            else if (name == "native-standby-ready")
                verify_standby_promotion_public_send (true);
            else if (name == "standby")
                verify_standby_promotion_readmits_both_peers ();
            else if (name == "replay")
                verify_bound_session_bind_does_not_replay_after_route_change ();
            else if (name == "create")
                verify_actor_create_retries_until_route_is_admitted ();
            else if (name == "stale-rid-retained")
                verify_stale_rid_disconnect_preserves_same_endpoint_replacement ();
            else if (name == "bilateral")
                verify_bilateral_raw_connection_without_public_pipe_id_keeps_survivor ();
            else if (name == "join-target-fence")
                verify_actor_join_target_fence_reaches_admission ();
            else if (name == "join")
                verify_actor_join_waits_for_admit_without_replay ();
            else
                return 2;
            std::fprintf (stderr, "%s %d/%d passed\n", argv[2], i + 1, repetitions);
        }
        return 0;
    }

    if (argc == 2 && std::string_view (argv[1]) == "--liveness-not-found-contract") {
        verify_liveness_without_selected_route ();
#if !defined(_WIN32)
        verify_liveness_propagates_binding_failure ();
        verify_liveness_propagates_binding_failure (true);
#endif
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--liveness-physical-close") {
        verify_liveness_without_selected_route ();
        verify_liveness_physical_close (true, false);
        verify_liveness_physical_close (false, true);
        verify_liveness_physical_close (false, false);
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--owner-rejection-observability") {
        verify_raw_owner_node_send_and_liveness (zlink::framework::message_flow_log_mode_t::off);
        verify_raw_owner_node_send_and_liveness (zlink::framework::message_flow_log_mode_t::errors);
        verify_raw_owner_node_send_and_liveness (zlink::framework::message_flow_log_mode_t::off,
                                                 true);
        verify_raw_owner_node_send_and_liveness (zlink::framework::message_flow_log_mode_t::errors,
                                                 true);
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--r6-create-lifecycle") {
        verify_actor_create_intent_removal_ends_operation ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--r6-join-durable") {
        verify_actor_join_waits_for_admit_without_replay ();
        verify_actor_join_durable_terminals ();
        verify_actor_join_rejected_reply_completes_typed_failure ();
        verify_actor_join_wrong_source_generation_is_fenced ();
        verify_actor_join_mismatched_correlation_reply_classifies_protocol_error ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--r6-join-owner-loss") {
        verify_actor_join_ends_after_unexpected_admitted_peer_loss (true);
        verify_actor_join_ends_after_unexpected_admitted_peer_loss (false);
        return 0;
    }
    verify_request_submission_results ();
    verify_application_ingress_after_local_admit ();
    verify_unadmitted_route (false);
    verify_unadmitted_route (true);
    verify_prepare_does_not_replay_on_selected_route ();
    verify_topology_ready_is_remote_admit_on_current_route ();
    verify_durable_attempt_waits_for_admit (protocol::command::boundSessionBind);
    verify_durable_attempt_waits_for_admit (protocol::command::actorCreate);
    verify_durable_attempt_waits_for_admit (protocol::command::actorJoin);
    verify_actor_create_command_49_roundtrip ();
    verify_bound_session_close_reply_grammar ();
    for (const auto outcome : {"late", "timeout", "invalidated", "other-generation",
                               "invalidated-other-generation", "late-endpoint-failure"})
        verify_bound_session_bind_missing_descriptor (outcome);
    verify_bound_session_bind_retries_until_route_is_admitted ();
    verify_bound_session_bind_exposes_only_accepted_attempt ();
    verify_bound_session_bind_unadmitted_deadline_fails_both_stages ();
    verify_bound_session_bind_does_not_replay_after_route_change ();
    verify_bound_session_bind_permanent_absence_is_bounded ();
    verify_bound_session_bind_reply_completes_registered_operation ();
    verify_actor_create_retries_until_route_is_admitted ();
    verify_actor_create_intent_removal_ends_operation ();
    verify_actor_create_readiness_deadline_is_timed_out ();
    verify_actor_create_from_dispatch_thread_does_not_block ();
    verify_topology_snapshot_and_connection_fence ();
    verify_route_mesh_descriptor_uses_generated_capability ();
    verify_admission_follows_latest_observed_route ();
    verify_lifecycle_token_requires_current_discovery_expectation ();
    verify_stale_rid_disconnect_preserves_same_endpoint_replacement ();
    verify_bilateral_raw_connection_without_public_pipe_id_keeps_survivor ();
    verify_raw_admission_rejects_lifecycle_mismatch ();
    verify_object_client_connection_requirement ();
    verify_manual_object_client_pair_ends_not_required ();
    verify_signed_weight_contract ();
    verify_envelope_body_retains_native_storage ();
    verify_application_owner_drain_transitions ();
    verify_independent_mailbox_domains_and_claim_fence ();
    verify_liveness_reuses_probe_and_fences_reconnect ();
    verify_liveness_shared_record_contract ();
    verify_completion_receipt_without_application_receive ();
    verify_location_descriptor_cas_snapshot_and_watch ();
    verify_manual_and_automatic_classic_fanout ();
    verify_client_server_stale_admission_reply_is_discarded ();
    verify_client_server_close_drains_parked_control_reply ();
    verify_client_server_plain_hello_is_rejected ();
    verify_client_server_independent_raw_path ();
    verify_client_server_admits_before_monitor_drain ();
    verify_client_server_weighted_selection ();
    verify_raw_owner_node_send_and_liveness ();
    verify_relocation_prepare_failed_reply_resolves_promptly_with_identity_fencing ();
    verify_relocation_prepare_failed_reply_with_mismatched_identity_is_fenced ();
    verify_actor_join_waits_for_admit_without_replay ();
    verify_actor_join_durable_terminals ();
    verify_actor_join_accepted_reply_threads_chunk_limit_and_epoch ();
    verify_actor_join_rejected_reply_completes_typed_failure ();
    verify_actor_join_wrong_source_generation_is_fenced ();
    verify_actor_join_mismatched_correlation_reply_classifies_protocol_error ();
    for (const auto path : {"join", "prepare", "spot", "actor", "actor-create", "instance",
                            "user-spot-create", "user-spot-close"})
        for (const bool invalidated : {false, true})
            verify_authority_generation_request (path, invalidated);
    verify_authority_generation_send ();
    verify_actor_join_target_fence_reaches_admission ();
    return 0;
}
