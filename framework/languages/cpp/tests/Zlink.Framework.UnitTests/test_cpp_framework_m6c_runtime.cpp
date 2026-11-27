/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_memory_authority_fixture.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace m6c_test
{

void test_context_t::require (bool condition, const char *message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "V11-M6C-CPP: " << message << '\n';
}

} // namespace m6c_test

using namespace m6c_test;

int main (int argc, char **argv)
{
    zlink::framework::completion_owner_t completion_owner;
    zlink::framework::runtime::configure_handler_coroutine_executor (4);
    struct executor_shutdown_t
    {
        ~executor_shutdown_t ()
        {
            zlink::framework::runtime::shutdown_handler_coroutine_executor ();
        }
    } executor_shutdown;
    test_context_t test;
    if (argc == 2 && std::string_view (argv[1]) == "--telemetry-consumers") {
        test_actor_return_to_entry_spot_skips_admission_and_runs_lifecycle_callbacks (
          test, std::make_shared<memory_authority_store_t> ());
        test_application_relocation_remote_production_path (test, true);
        test_return_actor_relocation_replaces_departed_spot_instance (test);
        test_return_actor_relocation_replaces_departed_spot_instance (test, true);
        test_return_actor_relocation_replaces_departed_spot_instance (test, false, true);
        std::cerr << "Telemetry consumers helpers=5 failures=" << test.failures << '\n';
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--target-cutover") {
        test_relocation_target_cutover_and_authority_settlement (test);
        std::cerr << "target-cutover failures=" << test.failures << '\n';
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--request-registry") {
        test_relocation_request_source_registration (test, false);
        test_relocation_request_source_registration (test, true);
        test_relocation_request_source_registration (test, false, true);
        std::cerr << "request-registry cases=3 failures=" << test.failures << '\n';
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--precutover-metric-terminal") {
        test_source_relocation_failure_stops_state_chunks_and_unseals (test);
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--source-outcomes") {
        test_source_relocation_metric_outcomes (test);
        test_relocation_metric_starts_only_after_unit_admission (test);
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--spot-restore-staging") {
        test_spot_restore_stages_before_publication (test);
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--aggregate-source") {
        test_application_user_spot_aggregate_remote_production_path (test);
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--binding-absent") {
        test_application_relocation_remote_production_path (test, false, false, false, false, true);
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--route-command44-order") {
        test_application_relocation_remote_production_path (test, false, false, false, true);
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--route-first-push") {
        test_application_relocation_remote_production_path (test, false, false, true);
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--ready-expired") {
        test_relocation_ready_boundary (test, ready_boundary_case_t::expired_remote);
        test_relocation_ready_boundary (test, ready_boundary_case_t::expired_local);
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--ready-boundary") {
        for (const auto mode :
             {ready_boundary_case_t::ready, ready_boundary_case_t::expired_remote,
              ready_boundary_case_t::expired_local, ready_boundary_case_t::shutdown_restore,
              ready_boundary_case_t::shutdown_route})
            test_relocation_ready_boundary (test, mode);
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--boundary-reply") {
        std::cerr << "Boundary reply follow=enabled\n";
        test_boundary_application_preserves_original_reply (test);
        std::cerr << "Boundary reply follow=none\n";
        test_boundary_application_preserves_original_reply (test, true);
        std::cerr << "Boundary reply source=expired\n";
        test_boundary_application_preserves_original_reply (test, false, true);
        std::cerr << "Boundary reply cases=3 failures=" << test.failures << '\n';
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--source-cutover-cancel") {
        test_source_relocation_settles_by_authority (test, false, true);
        std::cerr << "Source cutover cancellation cases=3 failures=" << test.failures << '\n';
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--source-terminal-lifecycle") {
        test_source_relocation_settles_by_authority (test, true, false, true);
        test_source_relocation_settles_by_authority (test, true, false, false, true);
        test_delay_owner_final_join_closes_accepted_registration (test);
        std::cerr << "Source terminal lifecycle cases=5 failures=" << test.failures << '\n';
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--source-management-cancel") {
        test_source_relocation_settles_by_authority (test, true);
        std::cerr << "Source management cancellation cases=3 failures=" << test.failures << '\n';
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--source-settlement-chain") {
        test_source_settlement_after_subsequent_moves (test);
        test_source_settlement_preserve_and_members (test);
        std::cerr << "Source settlement chain cases=18 failures=" << test.failures << '\n';
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--queue-lifetime") {
        test_pending_actor_turn_survives_queue_removal (test);
        test_pending_actor_turn_survives_queue_removal (test, true);
        std::cerr << "Queue lifetime cases=2 failures=" << test.failures << '\n';
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--source-return") {
        test_return_actor_relocation_replaces_departed_spot_instance (test, false, true);
        std::cerr << "Source return cases=1 failures=" << test.failures << '\n';
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--session-seal") {
        test_application_relocation_remote_production_path (test);
        test_application_relocation_remote_production_path (test, false, true);
        test_application_relocation_remote_production_path (test, true);
        std::cerr << "Session seal cases=3 failures=" << test.failures << '\n';
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 3 && std::string_view (argv[1]) == "--route-network") {
        const int repetitions = std::stoi (argv[2]);
        if (repetitions <= 0)
            return EXIT_FAILURE;
        for (int i = 0; i < repetitions; ++i) {
            test_cutover_replays_over_selected_network_route (test);
            if (test.failures != 0)
                return EXIT_FAILURE;
            std::cerr << "network-cutover " << i + 1 << '/' << repetitions << " passed\n";
        }
        return EXIT_SUCCESS;
    }
    if (argc == 3 && std::string_view (argv[1]) == "--route-settlement") {
        const int repetitions = std::stoi (argv[2]);
        if (repetitions <= 0)
            return EXIT_FAILURE;
        for (int i = 0; i < repetitions; ++i) {
            test_source_relocation_settles_by_authority (test);
            test_source_relocation_settles_by_authority (test, true);
            test_source_relocation_settles_by_authority (test, true, false, true);
            test_source_relocation_settles_by_authority (test, true, false, false, true);
            test_delay_owner_final_join_closes_accepted_registration (test);
            if (test.failures != 0)
                return EXIT_FAILURE;
            std::cerr << "settlement " << i + 1 << '/' << repetitions << " passed\n";
        }
        return EXIT_SUCCESS;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--missing-settlement") {
        test_public_authority_store_adapter (test);
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--relocation-shutdown") {
        for (int mode = 0; mode != 3; ++mode)
            test_relocation_target_shutdown (test, mode);
        return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    for (int mode = 0; mode != 3; ++mode)
        test_relocation_target_shutdown (test, mode);
    test_spot_lifecycle_domain_rejects_invalid_kind_combinations (test);
    test_generation_barrier_quiesces_yield_spot_and_timer (test);
    test_relocation_ready_completion_runs_once_on_spot_turn (test);
    test_relocation_ready_defer_rejects_only_its_own_turn (test);
    test_relocation_ready_defer_holds_queued_timer_turn (test);
    test_actor_leave_after_relocation_defer_runs_lifecycle_callbacks (test);
    test_remote_actor_leave_commits_before_source_callback (test);
    test_actor_return_to_entry_spot_skips_admission_and_runs_lifecycle_callbacks (
      test, std::make_shared<memory_authority_store_t> ());
    test_temporary_channel_request_yield_owns_call_state (test);
    test_accepted_message_payload_is_deserialized_once (test);
    test_close_eligibility_preserves_execution_and_deletion_is_fenced (test);
    test_envelope_round_trip (test);
    test_actor_join_recovery_round_trip (test);
    test_spot_restore_stages_before_publication (test);
    test_concurrent_spot_restore_owns_one_reservation (test);
    test_restore_validates_generation_before_spot_publication (test);
    test_return_relocation_replaces_departed_remnant (test);
    test_pending_restore_holds_ingress_before_rollback (test);
    test_public_relocation_store_adapter (test);
    test_public_authority_store_adapter (test);
    test_source_relocation_metric_outcomes (test);
    test_relocation_metric_starts_only_after_unit_admission (test);
    test_source_relocation_failure_stops_state_chunks_and_unseals (test);
    test_boundary_application_preserves_original_reply (test);
    test_boundary_application_preserves_original_reply (test, true);
    test_boundary_application_preserves_original_reply (test, false, true);
    test_relocation_request_source_registration (test, false);
    test_relocation_request_source_registration (test, true);
    test_relocation_request_source_registration (test, false, true);
    test_boundary_preserve_restores_acceptance_order (test);
    test_cutover_replays_over_selected_network_route (test);
    test_source_relocation_settles_by_authority (test);
    test_source_relocation_settles_by_authority (test, true);
    test_source_relocation_settles_by_authority (test, true, false, true);
    test_source_relocation_settles_by_authority (test, true, false, false, true);
    test_delay_owner_final_join_closes_accepted_registration (test);
    test_source_relocation_settles_by_authority (test, false, true);
    test_source_settlement_after_subsequent_moves (test);
    test_source_settlement_preserve_and_members (test);
    test_relocation_target_cutover_and_authority_settlement (test);
    test_source_completion_wait_scope (test);
    test_pending_actor_turn_survives_queue_removal (test);
    test_pending_actor_turn_survives_queue_removal (test, true);
    for (const auto mode :
         {ready_boundary_case_t::ready, ready_boundary_case_t::expired_remote,
          ready_boundary_case_t::expired_local, ready_boundary_case_t::shutdown_restore,
          ready_boundary_case_t::shutdown_route})
        test_relocation_ready_boundary (test, mode);
    test_application_relocation_remote_production_path (test, false, false, true);
    test_application_relocation_remote_production_path (test, false, false, false, true);
    test_application_relocation_remote_production_path (test, false, false, false, false, true);
    test_application_relocation_remote_production_path (test);
    test_application_relocation_remote_production_path (test, false, true);
    test_application_relocation_remote_production_path (test, true);
    test_application_user_spot_aggregate_remote_production_path (test);
    test_aggregate_seal_failure_preserves_earlier_application_work (test);
    test_relocation_adapter_single_capture_restore_path (test);
    test_entry_spot_actor_relocation_restore_resolves_local_entry_spot (test);
    test_return_actor_relocation_replaces_departed_spot_instance (test);
    test_return_actor_relocation_replaces_departed_spot_instance (test, true);
    test_return_actor_relocation_replaces_departed_spot_instance (test, false, true);
    test_entry_spot_actor_relocation_restore_fails_without_local_entry_spot (test);
    test_relocation_hold_restores_without_dedicated_limits (test);
    test_stateful_application_queue_accepts_active_backlog (test);
    test_advertised_receive_chunk_limit_wiring (test);
    test_actor_join_wire_gate_records_target_authority (test);
    return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
