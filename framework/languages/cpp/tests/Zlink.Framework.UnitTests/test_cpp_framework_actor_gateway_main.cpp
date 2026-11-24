/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_gateway_cases.hpp"
#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <iostream>
#include <string>
#include <string_view>

int main (int argc, char **argv)
{
    using namespace zlink::framework::test::actor_gateway;

    if (argc == 2 && std::string_view (argv[1]) == "--relay-sequence") {
        zlink::framework::completion_owner_t completion_owner;
        return bound_session_relay_admission_is_exact_and_monotonic ();
    }
    if (argc == 2 && std::string_view (argv[1]) == "--telemetry-consumers") {
        zlink::framework::completion_owner_t completion_owner;
        for (
          const auto run :
          {relocation_target_prewarm_publishes_store_confirmed_actor_and_session_fence_atomically,
           bound_session_push_detaches_before_direct_sink_entry,
           admitted_bound_session_delivery_keeps_original_session_after_rebind,
           common_session_disconnect_uses_actor_packet_route,
           join_completion_callback_failure_is_reported}) {
            if (const auto result = run (); result != 0)
                return result;
        }
        return 0;
    }

    if (argc == 1) {
        using zlink::framework::message_flow_log_mode_t;
        for (const auto cause : {"stale_target", "no_handler"})
            for (const auto mode :
                 {message_flow_log_mode_t::off, message_flow_log_mode_t::errors,
                  message_flow_log_mode_t::normal, message_flow_log_mode_t::detailed})
                for (const auto meter : {false, true})
                    if (const auto result = actor_native_terminal_cause (cause, mode, meter);
                        result != 0)
                        return result;
        for (const auto cause :
             {"disconnect_stale_target", "disconnect_no_handler", "application_not_found",
              "request_no_handler", "follow", "park", "replay_stale_target", "replay_no_handler",
              "replay_invalid_context", "replay_invalid_header"})
            if (const auto result =
                  actor_native_terminal_cause (cause, message_flow_log_mode_t::normal, true);
                result != 0)
                return result;
    }
    if (argc == 1)
        if (const auto result = actor_native_terminal_cause (
              "application_not_found", zlink::framework::message_flow_log_mode_t::detailed, true);
            result != 0)
            return result;
    if (argc >= 3 && std::string_view (argv[1]) == "--terminal-cause")
        return actor_native_terminal_cause (
          argv[2],
          argc > 3 ? static_cast<zlink::framework::message_flow_log_mode_t> (std::stoi (argv[3]))
                   : zlink::framework::message_flow_log_mode_t::normal,
          argc <= 4 || std::string_view (argv[4]) == "on");
    if (argc == 2 && std::string_view (argv[1]) == "--terminal-sdk")
        return actor_terminal_sdk_controlled_counter_projection ();
    if (argc == 2 && std::string_view (argv[1]) == "--binding-state-owner")
        return binding_context_uses_gateway_state_lane ();
    if (argc == 2 && std::string_view (argv[1]) == "--session-bound-policy")
        return session_bound_callback_policy ();
    if (argc == 2 && std::string_view (argv[1]) == "--duplicate-bind-policy")
        return duplicate_bind_callback_policy ();
    if (argc == 1 && binding_context_uses_gateway_state_lane () != 0)
        return 1;
    if (const auto framing = actor_reply_framing_vectors (); framing != 0)
        return 500 + framing;

    if (const auto errors = actor_error_payload_vectors (); errors != 0)
        return 600 + errors;
    zlink::framework::completion_owner_t completion_owner;
    if (argc == 2 && std::string (argv[1]) == "--actor-typed-wire")
        return actor_typed_wire_header_contract ();
    if (const auto wire = actor_typed_wire_header_contract (); wire != 0)
        return wire;
    if (argc == 1) {
        if (const auto registration = bind_registration_contract_vectors (); registration != 0)
            return registration;
    }
    if (argc == 2) {
        const std::string test (argv[1]);
        if (test == "--bind-registration")
            return bind_registration_contract_vectors ();

        if (test == "--prepare-runtime-failure")
            return relocation_prepare_preserves_runtime_failure ();
        if (test == "--join-completion-callback")
            return join_completion_callback_failure_is_reported ();
        if (test == "--join-completion-delivery") {
            const auto result = join_completion_waits_for_bound_session_delivery_terminal ();
            std::cerr << "Join completion delivery result=" << result << '\n';
            return result;
        }
        if (test == "--close-unbound")
            return close_unbound_session_reports_invalid_operation ();
        if (test == "--close-binding")
            return close_exact_session_binding_uses_physical_terminal ();
        if (test == "--known-generation-source-leave") {
            const auto result = known_generation_source_leave_claims_transfer ();
            std::cerr << "Known generation source leave result=" << result << '\n';
            return result;
        }
        if (test == "--session-disconnected-public")
            return session_disconnect_resolves_context_without_ingress_metadata (false);
        if (test == "--session-disconnected-replay")
            return session_disconnect_resolves_context_without_ingress_metadata (true);
        if (test == "--session-disconnected-terminal")
            return physical_disconnect_submits_all_before_waiting_for_terminal ();
        if (test == "--session-disconnected-gate")
            return session_disconnect_callback_owns_spot_lifecycle_gate ();
        if (test == "--session-disconnected-wire") {
            const auto identity = common_session_disconnect_uses_actor_packet_route ();
            std::cerr << "Session disconnect identity/turn/capture: " << identity << std::endl;
            if (identity != 0)
                return identity;
            const auto followed =
              old_owner_forwards_cold_probe_via_active_message_follow_route (true);
            std::cerr << "Session disconnect Message Follow: " << followed << std::endl;
            if (followed != 0)
                return followed;
            const auto all_settled = physical_disconnect_submits_all_before_waiting_for_terminal ();
            std::cerr << "Session disconnect all-settled submission: " << all_settled << std::endl;
            if (all_settled != 0)
                return all_settled;
            const auto snapshot = session_disconnect_dispatcher_preserves_identity_snapshot ();
            std::cerr << "Session disconnect source snapshot: " << snapshot << std::endl;
            return snapshot;
        }
        if (test == "--unbound-physical")
            return actor_unbound_follows_session_termination (true);
        if (test == "--unbound-logical")
            return actor_unbound_follows_session_termination (false);
        if (test == "--unbound-disconnect-then-shutdown") {
            constexpr auto stress_iterations = 100;
            for (auto iteration = 0; iteration < stress_iterations; ++iteration) {
                if (const auto result = actor_unbound_follows_session_termination (true, true);
                    result != 0)
                    return result;
            }
            return 0;
        }
        if (test == "--reconcile-source")
            return reconcile_deadline_fast_fails_when_store_shows_source ();
        if (test == "--reconcile-no-commit")
            return reconcile_deadline_fast_fails_when_store_has_no_commit ();
        if (test == "--reconcile-target")
            return reconcile_deadline_adopts_target_when_store_shows_committed ();
        if (test == "--reconcile-indeterminate")
            return reconcile_deadline_fast_fails_when_store_is_indeterminate ();
        if (test == "--infrastructure-store-wait")
            return remote_actor_join_store_read_does_not_wait_for_handler_workers ();
        if (test == "--fifo-request-suffix") {
            const auto result = relocation_barrier_fences_late_actor_request_without_deadlock ();
            std::cerr << test << " result=" << result << '\n';
            return result;
        }
        if (test == "--fifo-reserved-suffix" || test == "--fifo-committed-before-post") {
            const auto result = relocation_barrier_fences_late_actor_fifo_admission (
              test == "--fifo-committed-before-post");
            std::cerr << test << " result=" << result << '\n';
            return result;
        }
        if (test == "--native-relay-route")
            return local_relay_preserves_handoff_request_route (true);
        if (test == "--native-relay-operation")
            return local_relay_preserves_handoff_request_route (false);
        if (test == "--native-relay-deadline")
            return local_relay_preserves_handoff_request_route (false, true);
        if (test == "--native-relay-missing")
            return local_relay_failure_returns_without_waiting_for_handoff_deadline ();
        if (test == "--native-relay-replay")
            return parked_request_reply_case ("actor-a", parked_request_case_t::direct_replay);
        return 1;
    }
    if (const auto gate = session_disconnect_callback_owns_spot_lifecycle_gate (); gate != 0)
        return 207;
    if (const auto all_settled = physical_disconnect_submits_all_before_waiting_for_terminal ();
        all_settled != 0)
        return 206;
    if (const auto snapshot = session_disconnect_dispatcher_preserves_identity_snapshot ();
        snapshot != 0)
        return 570 + snapshot;
    if (const auto followed = old_owner_forwards_cold_probe_via_active_message_follow_route (true);
        followed != 0)
        return 560 + followed;
    for (const bool replay : {false, true}) {
        if (const auto context =
              session_disconnect_resolves_context_without_ingress_metadata (replay);
            context != 0)
            return context;
    }
    if (const auto disconnected = common_session_disconnect_uses_actor_packet_route ();
        disconnected != 0)
        return 550 + disconnected;
    if (const auto route = local_relay_preserves_handoff_request_route (true); route != 0)
        return 450 + route;
    if (const auto route = local_relay_preserves_handoff_request_route (false); route != 0)
        return 455 + route;
    if (const auto deadline = local_relay_preserves_handoff_request_route (false, true);
        deadline != 0)
        return 465 + deadline;
    if (const auto failure = local_relay_failure_returns_without_waiting_for_handoff_deadline ();
        failure != 0)
        return 460 + failure;
    if (const auto ordered = terminal_reply_precedes_session_route_publication (); ordered != 0)
        return 430 + ordered;
    if (const auto drained = actor_unbound_follows_accepted_push_submission (); drained != 0)
        return 435 + drained;
    if (const auto ordered = session_disconnect_uses_actor_slot_issue_order (); ordered != 0)
        return 440 + ordered;
    if (const auto protobuf_bound = generated_protobuf_bound_session_uses_typed_serializer_codec ();
        protobuf_bound != 0) {
        return 420 + protobuf_bound;
    }
    if (const auto handoff_fence = relocation_barrier_fences_late_actor_fifo_admission ();
        handoff_fence != 0) {
        std::cerr << "--fifo-reserved-suffix result=" << handoff_fence << '\n';
        return 400 + handoff_fence;
    }
    if (const auto completed_commit = relocation_barrier_fences_late_actor_fifo_admission (true);
        completed_commit != 0)
        return 470 + completed_commit;
    if (const auto handoff_fence_request =
          relocation_barrier_fences_late_actor_request_without_deadlock ();
        handoff_fence_request != 0) {
        return 410 + handoff_fence_request;
    }
    if (const auto completion_delivery =
          join_completion_waits_for_bound_session_delivery_terminal ();
        completion_delivery != 0) {
        return 395 + completion_delivery;
    }
    if (const auto store_resolution =
          remote_actor_join_resolves_store_type_and_reports_typed_terminals ();
        store_resolution != 0) {
        return 390 + store_resolution;
    }
    if (const auto pending_capacity = pending_handoff_accepts_beyond_former_capacity ();
        pending_capacity != 0) {
        return 385 + pending_capacity;
    }
    if (const auto parked_reply = parked_request_without_route_fence_receives_reply_after_replay ();
        parked_reply != 0) {
        return 360 + parked_reply;
    }
    if (const auto direct_reply =
          parked_request_reply_case ("actor-a", parked_request_case_t::direct_replay);
        direct_reply != 0) {
        return 375 + direct_reply;
    }
    if (const auto scoped_terminal =
          same_operation_from_distinct_source_lifecycles_has_distinct_pending_terminal ();
        scoped_terminal != 0) {
        return 365 + scoped_terminal;
    }
    if (const auto follow_reply =
          relayed_request_without_deferred_terminal_receives_follow_reply ();
        follow_reply != 0) {
        return 370 + follow_reply;
    }
    if (const auto cross_node_terminal =
          parked_cross_node_request_terminal_returns_to_parking_node ();
        cross_node_terminal != 0) {
        return 380 + cross_node_terminal;
    }
    if (const auto prepare = relocation_prepare_preserves_runtime_failure (); prepare != 0)
        return prepare;
    if (const auto callback = join_completion_callback_failure_is_reported (); callback != 0)
        return callback;
    if (const auto capacity = session_relay_waiter_accepts_beyond_former_capacity ();
        capacity != 0) {
        return 290 + capacity;
    }
    if (const auto finite = bind_or_get_all_exit_paths_complete_within_deadline (); finite != 0) {
        return 285 + finite;
    }
    if (const auto reentrant = bind_or_get_completion_can_reenter_same_manager (); reentrant != 0) {
        return 280 + reentrant;
    }
    if (const auto command_38 = command_38_rebind_is_owned_only_by_new_connection ();
        command_38 != 0) {
        return 270 + command_38;
    }
    if (const auto monotonic = late_lower_generation_bind_and_record_are_rejected ();
        monotonic != 0) {
        return 265 + monotonic;
    }
    if (const auto disconnect_fence = old_stream_disconnect_does_not_retire_reconnected_binding ();
        disconnect_fence != 0) {
        return 260 + disconnect_fence;
    }
    if (const auto handlers = same_rid_registration_retains_retired_close_handler ();
        handlers != 0) {
        return 250 + handlers;
    }
    if (const auto defensive = same_identity_registration_keeps_logical_binding_current ();
        defensive != 0) {
        return 245 + defensive;
    }
    if (const auto reconnect = reconnect_push_reaches_new_session_across_two_hosts ();
        reconnect != 0) {
        return 240 + reconnect;
    }
    if (const auto detached = bound_session_push_detaches_before_direct_sink_entry ();
        detached != 0) {
        return 220 + detached;
    }
    if (const auto admitted =
          admitted_bound_session_delivery_keeps_original_session_after_rebind ();
        admitted != 0) {
        return 225 + admitted;
    }
    if (const auto fence = rebound_session_keeps_prior_ingress_exact_fence (); fence != 0) {
        return 210 + fence;
    }
    if (const auto admission =
          actor_request_completion_keeps_dedup_state_owned_after_runtime_wrapper_unwinds ();
        admission != 0) {
        return 200 + admission;
    }
    if (const auto cold_forward = old_owner_forwards_cold_probe_via_active_message_follow_route ();
        cold_forward != 0) {
        return 300 + cold_forward;
    }
    if (const auto returned_owner =
          returned_owner_serves_locally_past_retained_message_follow_route ();
        returned_owner != 0) {
        return 305 + returned_owner;
    }
    if (const auto leave_race = source_cleanup_waits_for_leave_completion_before_erasing_actor ();
        leave_race != 0) {
        return 310 + leave_race;
    }
    if (const auto sweep_race = sweep_preserves_pending_leave_added_after_snapshot ();
        sweep_race != 0) {
        return 315 + sweep_race;
    }
    if (const auto reconcile_trap = reconcile_deadline_fast_fails_when_store_shows_source ();
        reconcile_trap != 0) {
        return 320 + reconcile_trap;
    }
    if (const auto no_commit = reconcile_deadline_fast_fails_when_store_has_no_commit ();
        no_commit != 0) {
        return 360 + no_commit;
    }
    if (const auto reconcile_adopt = reconcile_deadline_adopts_target_when_store_shows_committed ();
        reconcile_adopt != 0) {
        return 340 + reconcile_adopt;
    }
    if (const auto reconcile_indeterminate =
          reconcile_deadline_fast_fails_when_store_is_indeterminate ();
        reconcile_indeterminate != 0) {
        return 350 + reconcile_indeterminate;
    }
    if (const auto leave_notify =
          leave_notification_travels_node_level_and_reaches_source_entry_spot_once ();
        leave_notify != 0) {
        return 330 + leave_notify;
    }
    if (const auto no_follow_leave =
          early_zero_generation_leave_waits_for_source_transfer_completion (true);
        no_follow_leave != 0)
        return 500 + no_follow_leave;
    if (const auto known_leave = known_generation_source_leave_claims_transfer ();
        known_leave != 0) {
        return 380 + known_leave;
    }
    if (const auto early_leave =
          early_zero_generation_leave_waits_for_source_transfer_completion ();
        early_leave != 0) {
        return 390 + early_leave;
    }
    if (const auto pending = disconnect_notification_survives_pending_dispatcher_completion ();
        pending != 0) {
        return 190 + pending;
    }
    if (const auto pending = session_manager_disconnect_survives_pending_dispatcher_completion ();
        pending != 0) {
        return 195 + pending;
    }
    if (const auto pending = relay_request_survives_pending_dispatcher_completion ();
        pending != 0) {
        return 160 + pending;
    }
    if (const auto pending = relay_send_survives_pending_dispatcher_completion (); pending != 0) {
        return 170 + pending;
    }
    if (const auto queue = session_relay_queue_is_ordered_without_blocking_other_actors ();
        queue != 0) {
        return 150 + queue;
    }
    if (const auto execution = session_relay_does_not_start_actor_dispatch_on_session_thread ();
        execution != 0) {
        return 155 + execution;
    }
    if (const auto admission = bound_session_relay_admission_is_exact_and_monotonic ();
        admission != 0) {
        return 140 + admission;
    }
    if (const auto transition = bound_session_transition_is_atomic_and_idempotent ();
        transition != 0) {
        return 130 + transition;
    }
    if (const auto route_update = authority_only_route_update_keeps_physical_session_current ();
        route_update != 0) {
        return 180 + route_update;
    }
    if (const auto identity = actor_identity_validation_is_bounded_and_utf8_exact ();
        identity != 0) {
        return 120 + identity;
    }
    if (const auto relay_scope = relay_dispatch_scope_restores_nested_and_exception_state ();
        relay_scope != 0) {
        return 110 + relay_scope;
    }
    if (const auto relay_await = relay_dispatch_header_follows_handler_await (); relay_await != 0) {
        return 465 + relay_await;
    }
    if (const auto route_fence = bound_session_route_preserves_private_fences ();
        route_fence != 0) {
        return 100 + route_fence;
    }
    if (const auto pending_route = bound_session_route_suspends_until_owner_lane_runs ();
        pending_route != 0) {
        return 100 + pending_route;
    }
    if (const auto bound_send = bound_session_send_does_not_publish_caller_location ();
        bound_send != 0) {
        return 95 + bound_send;
    }
    if (const auto lifetime =
          app_bound_session_sender_keeps_arguments_alive_after_route_lookup_suspends ();
        lifetime != 0) {
        return 390 + lifetime;
    }
    if (const auto relocation_prewarm =
          relocation_target_prewarm_publishes_store_confirmed_actor_and_session_fence_atomically ();
        relocation_prewarm != 0) {
        return 370 + relocation_prewarm;
    }
    if (const auto stale_binding =
          stale_binding_registration_and_relocation_prepare_preserve_current_tenure ();
        stale_binding != 0) {
        return 380 + stale_binding;
    }
    if (const auto normalized =
          bound_session_ref_normalization_preserves_type_and_rejects_conflicts ();
        normalized != 0) {
        return 80 + normalized;
    }
    if (const auto atomic_route = bound_session_route_installs_sink_and_fence_together ();
        atomic_route != 0) {
        return 70 + atomic_route;
    }
    if (const auto context_fence = actor_context_identity_and_source_fence_are_exact ();
        context_fence != 0) {
        return 90 + context_fence;
    }
    if (const auto stale = stale_session_unbind_preserves_rebind (); stale != 0) {
        return stale;
    }
    if (const auto replaced = replaced_session_find_is_exact_and_disconnects_once ();
        replaced != 0) {
        return 10 + replaced;
    }
    if (const auto bound = bound_actors_are_current_ordered_snapshot (); bound != 0) {
        return 10 + bound;
    }
    if (const auto idempotent = bind_or_get_reuses_same_physical_session_generation ();
        idempotent != 0) {
        return 155 + idempotent;
    }
    if (const auto atomic_rebind =
          direct_rebind_publication_is_atomic_and_old_disconnect_is_fenced ();
        atomic_rebind != 0) {
        return 160 + atomic_rebind;
    }
    if (const auto stale_destroy = destroyed_or_recreated_actor_ignores_stale_disconnect_handle ();
        stale_destroy != 0) {
        return 170 + stale_destroy;
    }
    if (const auto exact = exact_session_generation_mismatch_is_invalid_operation (); exact != 0) {
        return exact;
    }
    const auto one_shot = actor_send_is_one_shot ();
    if (one_shot != 0)
        return 10 + one_shot;
    const auto reconnect = get_or_create_does_not_reuse_disconnected_record ();
    if (reconnect != 0)
        return 15 + reconnect;
    const auto foreign = get_or_create_refreshes_foreign_session_record ();
    if (foreign != 0)
        return 17 + foreign;
    const auto disconnected = session_disconnect_is_all_settled_and_token_fenced ();
    if (disconnected != 0)
        return 20 + disconnected;
    const auto logical = logical_disconnect_is_selected_and_keeps_session_live ();
    if (logical != 0)
        return 30 + logical;
    const auto route = route_update_preserves_object_generation ();
    return route == 0 ? 0 : 40 + route;
}
