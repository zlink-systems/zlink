/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/mesh_node_vertical_cases.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>

#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

int main (int argc, char **argv)
{
    using namespace framework_mesh_node_vertical_tests;

    if (argc == 2 && std::string_view (argv[1]) == "--completion-start") {
        verify_mesh_start_and_rollback_from_completion_turn ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--route-internal-owner-lease") {
        verify_route_internal_packets_precede_application_dispatch ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--terminal-owner-capture-lifetime")
        return verify_application_terminal_drain_waits_for_capture_destruction () ? 0 : 1;
    if (argc == 2 && std::string_view (argv[1]) == "--connect-intent") {
        verify_repeated_mesh_connect_keeps_one_physical_connection ();
        verify_peer_intent_ids_and_callback_lifetime ();
        return 0;
    }
    if ((argc == 2 || argc == 3) && std::string_view (argv[1]) == "--automatic-intent") {
        const int repetitions = argc == 3 ? std::stoi (argv[2]) : 1;
        if (repetitions <= 0)
            return 2;
        for (int i = 0; i < repetitions; ++i) {
            verify_automatic_intent_respects_manual_peer ();
            std::fprintf (stderr, "automatic %d/%d passed\n", i + 1, repetitions);
        }
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--manual-peer-status") {
        verify_manual_peer_public_status ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--monitor-pump-async") {
        verify_monitor_pump_does_not_wait_for_spot_lane ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--stale-local-lifecycle") {
        LocalLifecycleIgnoresStaleStoreResult ();
        verify_local_host_lifecycle_uses_runtime_owner ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--stale-local-placement") {
        LocalPlacementIgnoresStaleStoreResult ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--monitor-snapshot") {
        LocalPlacementIgnoresStaleStoreResult ();
        verify_observation_starts_with_weight_changed_before_subscription ();
        verify_public_runtime_surface ();
        verify_manual_peer_public_status ();
        verify_location_store_blocks_placement ();
        verify_slow_observer_does_not_block_stop ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--management-async") {
        verify_management_does_not_wait_for_spot_lane ();
        return 0;
    }
#if defined(__unix__)
    if (argc == 2 && std::string_view (argv[1]) == "--cross-process")
        return run_cross_process_delivery ();
#endif
    LocalLifecycleIgnoresStaleStoreResult ();
    verify_local_host_lifecycle_uses_runtime_owner ();
    LocalPlacementIgnoresStaleStoreResult ();
    verify_local_join_timeout_releases_membership ();
    verify_unselected_object_role_defaults_to_none ();
    verify_automatic_identity_and_port_builder ();
    verify_observation_starts_with_weight_changed_before_subscription ();
    verify_public_runtime_surface ();
    verify_automatic_intent_respects_manual_peer ();
    verify_manual_peer_public_status ();
    verify_location_store_blocks_placement ();
    verify_slow_observer_does_not_block_stop ();
    verify_object_client_registration_boundary ();
    verify_host_shutdown_seal_reaches_raw_mesh ();
    verify_fixed_drain_callback_barrier ();
    verify_accepted_handler_push_after_host_seal (false);
    verify_accepted_handler_push_after_host_seal (true);
    verify_local_and_wire_application_owner_keys_match ();
    verify_owner_drain_continuation_during_executor_shutdown ();
    verify_deferred_application_terminal_ownership ();
    verify_server_descriptor_publishes_framework_entry_spot_without_application_entry ();
    verify_descriptor_retire_order_and_pre_seal_rollback ();
    verify_local_node_submit_bridge ();
    verify_route_internal_packets_precede_application_dispatch ();
    verify_direct_target_falls_through_absent_location_store_entry ();
    verify_request_to_never_admitted_target_reports_not_found ();
#if defined(__unix__)
    // Fork the delivery peers before any runtime threads exist in their parent.
    // A fork after the unit fixtures inherits dispatcher state, but not its worker.
    const auto delivery = fork ();
    assert (delivery >= 0);
    if (delivery == 0) {
        execl (argv[0], argv[0], "--cross-process", static_cast<char *> (nullptr));
        _exit (127);
    }
    int delivery_status = 0;
    assert (waitpid (delivery, &delivery_status, 0) == delivery);
    if (!WIFEXITED (delivery_status) || WEXITSTATUS (delivery_status) != 0)
        return WIFEXITED (delivery_status) ? WEXITSTATUS (delivery_status) : 4;
#else
    // Cross-process metadata and delivery coverage remains POSIX-only because
    // the repository has no Windows C++ unit-test child-process harness.  A
    // different in-process smoke check must not stand in for that scenario.
#endif
    verify_actor_route_resolver_preserves_unavailable ();
    verify_mesh_start_and_rollback_from_completion_turn ();
    return 0;
}
