/* SPDX-License-Identifier: Elastic-2.0 */

#include <zlink/framework/contracts/workers/worker.hpp>
#include "../support/execution_cases.hpp"
#include "../support/execution_settings.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "../support/scope_exit.hpp"
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

using zlink::framework::test::scope_exit_t;
using namespace zlink::framework::test::execution;

int main (int argc, char **argv)
{
    if (argc == 2 && std::string_view (argv[1]) == "--throwing-timer-callback-default-policy")
        return verify_throwing_timer_callback_preserves_default_policy () ? EXIT_SUCCESS
                                                                          : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--session-control-priority")
        return verify_session_control_lifecycle_priority () ? 0 : 1;
    if (argc == 2 && std::string_view (argv[1]) == "--fanout-state-owner")
        return verify_fanout_snapshot_state_owner () ? 0 : 1;
    if (argc == 1
        && (!verify_session_control_lifecycle_priority ()
            || !verify_fanout_snapshot_state_owner ()))
        return 1;
    zlink::framework::completion_owner_t completion_owner;
    if (argc == 2 && std::string_view (argv[1]) == "--timer-close-barrier") {
        zlink::framework::runtime::configure_handler_coroutine_executor (
          handler_coroutine_worker_count);
        scope_exit_t shutdown (
          [] { zlink::framework::runtime::shutdown_handler_coroutine_executor (); });
        return verify_close_waits_for_timer_callback_barrier () ? EXIT_SUCCESS : 41;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--bulk-deferred-actor-joins")
        return verify_bulk_deferred_actor_joins () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--telemetry-completion")
        return verify_actor_join_completion_observations ();
    if (argc == 2 && std::string_view (argv[1]) == "--handler-origin-terminals")
        return verify_all_handler_origin_terminals () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 1 && !verify_all_handler_origin_terminals ())
        return EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--intermediate-handler-terminal")
        return verify_intermediate_resume_keeps_handler_terminal () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--async-capture-owner")
        return verify_async_capture_releases_state_owner () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 1 && !verify_async_capture_releases_state_owner ())
        return EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--seal-cut-gate")
        return verify_seal_cut_takes_gate_and_publications () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 1 && !verify_seal_cut_takes_gate_and_publications ())
        return EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--seal-yield-terminal")
        return verify_seal_waits_for_yielded_handler_terminal () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 1 && !verify_seal_waits_for_yielded_handler_terminal ())
        return EXIT_FAILURE;
    bool phases_passed = true;
    for (const bool shared : {false, true}) {
        for (const bool cancel_queue : {false, true})
            for (const bool reached_first : {false, true})
                phases_passed = verify_cancelled_barrier_settles_running_phase (
                                  shared, cancel_queue, reached_first)
                                && phases_passed;
        phases_passed =
          verify_barrier_phase_completion_allows_reentrant_phase (shared) && phases_passed;
        for (const bool cancel : {false, true})
            phases_passed = verify_nested_barrier_phases (shared, cancel) && phases_passed;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--cancel-running-barrier-phase")
        return phases_passed ? EXIT_SUCCESS : EXIT_FAILURE;
    if (!phases_passed)
        return EXIT_FAILURE;
    bool mailbox_passed = verify_inactive_barrier_and_cancel_reentry (false);
    mailbox_passed = verify_inactive_barrier_and_cancel_reentry (true) && mailbox_passed;
    mailbox_passed = verify_fifo_indexes_across_shutdown (false) && mailbox_passed;
    mailbox_passed = verify_fifo_indexes_across_shutdown (true) && mailbox_passed;
    if (argc == 2 && std::string_view (argv[1]) == "--review-fifo-indexes")
        return mailbox_passed ? EXIT_SUCCESS : EXIT_FAILURE;
    mailbox_passed = verify_durable_tail_across_controls_and_export () && mailbox_passed;
    for (const bool shared : {false, true}) {
        mailbox_passed = verify_direct_continuation_after_close (shared) && mailbox_passed;
        for (const auto shutdown :
             {mailbox_shutdown_t::none, mailbox_shutdown_t::close, mailbox_shutdown_t::cancel})
            mailbox_passed = verify_idle_mailbox_terminal (shared, shutdown) && mailbox_passed;
    }
    if (!mailbox_passed)
        return EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--review-mailbox-terminal")
        return EXIT_SUCCESS;
    if (!verify_real_join_completion_callback_owner (0, false, true))
        return EXIT_FAILURE;
    if (!verify_accepted_continuation_survives_shutdown (false)
        || !verify_accepted_continuation_survives_shutdown (true))
        return EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--accepted-continuation-shutdown")
        return EXIT_SUCCESS;
    if (argc == 2 && std::string_view (argv[1]) == "--actor-finalize-cutover")
        return verify_actor_join_finalize_replies_after_target_activation () ? EXIT_SUCCESS
                                                                             : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--durable-policy-late-grant") {
        const bool policy = verify_durable_policy_late_grant (false);
        const bool transfer = verify_durable_policy_late_grant (true);
        return policy && transfer ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--deferred-lifecycle-prefix") {
        const bool wide = verify_deferred_after_lifecycle_prefix (false);
        const bool flat = verify_deferred_after_lifecycle_prefix (true);
        return wide && flat ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--join-completion-callback-owner") {
        bool passed = verify_real_join_completion_callback_owner (0, false);
        passed = verify_real_join_completion_callback_owner (0, true) && passed;
        passed = verify_real_join_completion_callback_owner (1, false) && passed;
        passed = verify_real_join_completion_callback_owner (2, false) && passed;
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--deferred-extra-boundaries")
        return verify_deferred_extra_boundaries () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--cancellable-serial-lifecycle")
        return verify_cancellable_serial_submission_lifecycle () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--deferred-coroutine-gate") {
        bool passed = true;
        for (const bool flat : {false, true})
            for (const bool fail : {false, true})
                for (const bool switched : {false, true})
                    passed =
                      verify_deferred_coroutine_gate_terminal (flat, fail, switched) && passed;
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--deferred-terminal-owner") {
        bool passed = true;
        for (const bool flat : {false, true})
            for (const bool fail : {false, true})
                for (const bool active : {false, true})
                    passed = verify_deferred_terminal_owner (flat, fail, active) && passed;
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--deferred-burst-terminal") {
        bool passed = true;
        for (const auto flat : {false, true})
            for (const auto fail_first : {false, true})
                passed = verify_deferred_burst_terminal (flat, fail_first) && passed;
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 1) {
        bool passed = verify_deferred_extra_boundaries ();
        passed = verify_durable_policy_late_grant (false) && passed;
        passed = verify_durable_policy_late_grant (true) && passed;
        passed = verify_deferred_after_lifecycle_prefix (false) && passed;
        passed = verify_deferred_after_lifecycle_prefix (true) && passed;
        for (const int policy : {0, 1, 2})
            passed = verify_real_join_completion_callback_owner (policy, false) && passed;
        passed = verify_real_join_completion_callback_owner (0, true) && passed;
        passed = verify_readiness_before_deferred_join () && passed;
        for (const bool flat : {false, true})
            for (const bool fail : {false, true})
                for (const bool active : {false, true})
                    passed = verify_deferred_terminal_owner (flat, fail, active) && passed;
        for (const auto flat : {false, true})
            for (const auto fail_first : {false, true})
                passed = verify_deferred_burst_terminal (flat, fail_first) && passed;
        for (const auto flat : {false, true}) {
            for (const auto fail_first : {false, true})
                passed = verify_deferred_actual_terminal (fail_first, false, flat) && passed;
            passed = verify_deferred_actual_terminal (false, true, flat) && passed;
        }
        for (const auto policy : {public_member_policy_t::spot_wide,
                                  public_member_policy_t::per_actor, public_member_policy_t::entry})
            for (const auto fail_first : {false, true})
                passed = verify_public_local_member_defer (fail_first, false, policy) && passed;
        for (const auto fail_first : {false, true})
            passed = verify_public_local_member_defer (fail_first, true) && passed;
        if (!passed)
            return EXIT_FAILURE;
    }

    if (argc == 2 && std::string_view (argv[1]) == "--flat-public-local-members") {
        bool passed = true;
        for (const auto policy : {public_member_policy_t::per_actor, public_member_policy_t::entry})
            for (const auto fail_first : {false, true})
                passed = verify_public_local_member_defer (fail_first, false, policy) && passed;
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (argc == 2 && std::string_view (argv[1]) == "--late-readiness") {
        const bool success = verify_public_local_member_defer (false, true);
        const bool failure = verify_public_local_member_defer (true, true);
        return success && failure ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (argc == 2 && std::string_view (argv[1]) == "--public-local-members") {
        const bool success = verify_public_local_member_defer (false);
        const bool failure = verify_public_local_member_defer (true);
        return success && failure ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--flat-deferred-actual-terminal") {
        bool passed = true;
        for (const auto fail_first : {false, true})
            passed = verify_deferred_actual_terminal (fail_first, false, true) && passed;
        passed = verify_deferred_actual_terminal (false, true, true) && passed;
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (argc == 2 && std::string_view (argv[1]) == "--readiness-before-deferred-join")
        return verify_readiness_before_deferred_join () ? EXIT_SUCCESS : EXIT_FAILURE;

    if (argc == 2 && std::string_view (argv[1]) == "--deferred-actual-terminal") {
        bool passed = true;
        for (const auto fail_first : {false, true})
            passed = verify_deferred_actual_terminal (fail_first, false) && passed;
        passed = verify_deferred_actual_terminal (false, true) && passed;
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (argc == 2 && std::string_view (argv[1]) == "--source-suffix-native-order")
        return verify_source_suffix_native_order_and_rollback () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--aggregate-membership-pending")
        return verify_relocation_membership_pending_releases_root_turn () ? EXIT_SUCCESS
                                                                          : EXIT_FAILURE;
    if (argc == 1
        && (!verify_relocation_participant_resources (true, false)
            || !verify_relocation_participant_resources (false, false)
            || !verify_relocation_participant_resources (false, true)
            || !verify_relocation_readiness_root_rejection ()
            || !verify_aggregate_capture_waits_for_participant_terminal ()
            || !verify_relocation_membership_pending_releases_root_turn ()))
        return EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--aggregate-failure-release") {
        const bool reserve_failure = verify_relocation_participant_resources (true, false);
        const bool run_failure = verify_relocation_participant_resources (false, false);
        return reserve_failure && run_failure ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--aggregate-holder-resources")
        return verify_relocation_participant_resources (true, false)
                   && verify_relocation_participant_resources (false, false)
                   && verify_relocation_participant_resources (false, true)
                   && verify_relocation_readiness_root_rejection ()
                 ? EXIT_SUCCESS
                 : EXIT_FAILURE;

    if (argc == 2 && std::string_view (argv[1]) == "--source-mesh-stopped-management")
        return verify_remote_actor_cutover_completion_is_target_owned (true, true, true)
                 ? EXIT_SUCCESS
                 : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--source-actor-drain")
        return verify_teardown_observes_actor_handoff_terminal () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--source-mesh-unknown")
        return verify_remote_actor_cutover_completion_is_target_owned (true, true) ? EXIT_SUCCESS
                                                                                   : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--source-mesh-cancel")
        return verify_remote_actor_cutover_completion_is_target_owned (true) ? EXIT_SUCCESS
                                                                             : EXIT_FAILURE;

    if (argc == 2 && std::string_view (argv[1]) == "--aggregate-capture-boundary")
        return verify_aggregate_capture_waits_for_participant_terminal () ? EXIT_SUCCESS
                                                                          : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--durable-head-admission") {
        const auto check = [] (const char *name, auto verify) {
            const bool passed = verify ();
            std::cout << "durable-head case=" << name << " passed=" << passed << '\n';
            return passed;
        };
        return check ("source-suffix", verify_source_suffix_native_order_and_rollback)
                   && check ("owner-transitions", verify_durable_head_owner_transitions)
                   && check ("rejection-close", verify_durable_head_rejection_and_close)
                 ? EXIT_SUCCESS
                 : EXIT_FAILURE;
    }
    if (argc == 1
        && (!verify_source_suffix_native_order_and_rollback ()
            || !verify_durable_head_owner_transitions ()
            || !verify_durable_head_rejection_and_close ()))
        return EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--spotwide-consumer-gate") {
        const bool passed = verify_spot_wide_consumer_waits_for_shared_gate ();
        std::cout << "SpotWide consumer waits for shared gate=" << passed << '\n';
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--spotwide-control-nonblocking")
        return verify_spot_wide_control_returns_without_waiting_for_gate () ? EXIT_SUCCESS
                                                                            : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--spotwide-cancel")
        return verify_spot_wide_cancel_removes_unclaimed_actor_head () ? EXIT_SUCCESS
                                                                       : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--spotwide-stop")
        return verify_spot_wide_stop_settles_queued_actor_rejection () ? EXIT_SUCCESS
                                                                       : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--spotwide-scheduler-stop")
        return verify_spot_wide_rejects_actor_when_gate_scheduler_stops () ? EXIT_SUCCESS
                                                                           : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--spotwide-actor-ready-lane")
        return verify_spot_wide_actor_ready_lane_tracks_lifecycle () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--spotwide-handoff-control")
        return verify_spot_wide_handoff_allows_spot_control () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--spotwide-gate-lifetime")
        return verify_spot_wide_terminal_retains_gate_lifetime () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "--relocation-ready-state-lane") {
        const bool passed = verify_relocation_ready_keeps_registering_turn_across_state_lane ();
        std::cout << "relocation readiness registering turn retained=" << passed << '\n';
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--actor-join-materialization") {
        zlink::framework::runtime::configure_handler_coroutine_executor (
          handler_coroutine_worker_count);
        const bool accepted = verify_join_commit_does_not_wait_for_joined_callback ();
        const bool failed = verify_join_commit_does_not_wait_for_joined_callback (true);
        const bool callback_failed =
          verify_join_commit_does_not_wait_for_joined_callback (true, true);
        const bool passed = accepted && failed && callback_failed;
        std::cout << "materialized Join: accepted=" << accepted << " failed=" << failed
                  << " callback-failed=" << callback_failed << '\n';
        zlink::framework::runtime::shutdown_handler_coroutine_executor ();
        if (!passed)
            std::cerr << "materialized Actor Join lifecycle failure lost its Failed completion or "
                         "barrier\n";
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (!verify_cpu_worker_completes_in_ordinary_turn ()) {
        std::cerr << "CPU worker did not complete while the ordinary handler retained its turn\n";
        return 141;
    }
    if (!verify_idle_timer_closes_before_executor_releases_last_node_reference ()) {
        return 138;
    }
    if (const auto failed = verify_deferred_work_keeps_lifecycle_fifo_position (); failed != 0) {
        std::cerr << "verify_deferred_work_keeps_lifecycle_fifo_position failed: " << failed
                  << '\n';
        return 230 + failed;
    }
    if (const auto failed = verify_deferred_join_waits_for_handler_terminal_across_yield ();
        failed != 0) {
        std::cerr << "verify_deferred_join_waits_for_handler_terminal_across_yield failed: "
                  << failed << '\n';
        return 235 + failed;
    }
    zlink::framework::runtime::configure_handler_coroutine_executor (
      handler_coroutine_worker_count);
    struct executor_shutdown_t
    {
        ~executor_shutdown_t ()
        {
            zlink::framework::runtime::shutdown_handler_coroutine_executor ();
        }
    } executor_shutdown;
    if (const auto error = verify_application_queue_limits_and_metrics (); error != 0) {
        return error;
    }

    if (const auto error = verify_logical_multicast_observation (); error != 0) {
        return error;
    }

    zlink::framework::worker_options_t worker_options;
    if (worker_options.min_threads () > worker_options.max_threads ()
        || worker_options.max_threads () == 0
        || worker_options.idle_timeout () < std::chrono::milliseconds::zero ()) {
        return 42;
    }
    worker_options.min_threads (2).max_threads (3).idle_timeout (std::chrono::milliseconds (7));
    if (worker_options.min_threads () != 2 || worker_options.max_threads () != 3
        || worker_options.idle_timeout () != std::chrono::milliseconds (7)) {
        return 43;
    }
    bool invalid_worker_options_rejected = false;
    try {
        worker_options.max_threads (1);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        invalid_worker_options_rejected =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!invalid_worker_options_rejected) {
        return 44;
    }

    std::atomic_bool abandoned_deadline_fired = false;
    const auto deadline_owner_start = std::chrono::steady_clock::now ();
    {
        auto control = std::make_shared<zlink::framework::detail::worker_control_t> (
          std::stop_token{}, std::chrono::hours (1));
        control->arm_deadline (std::chrono::hours (1),
                               [&] { return [&] { abandoned_deadline_fired.store (true); }; });
    }
    const auto deadline_owner_elapsed = std::chrono::steady_clock::now () - deadline_owner_start;
    if (abandoned_deadline_fired.load ()
        || deadline_owner_elapsed > std::chrono::milliseconds (250)) {
        return 45;
    }

    {
        zlink::framework::runtime::offload_executor_t saturated_executor (
          1, 1, std::chrono::milliseconds (0));
        std::mutex state_mutex;
        std::condition_variable state_changed;
        bool entered = false;
        bool release = false;
        std::atomic_int queued_runs = 0;
        // Releases the blocking job and drains before the locals above are destroyed.
        scope_exit_t drain_on_exit ([&] {
            {
                std::lock_guard lock (state_mutex);
                release = true;
            }
            state_changed.notify_all ();
            saturated_executor.drain ();
        });
        if (!saturated_executor.try_submit ([&] {
                std::unique_lock lock (state_mutex);
                entered = true;
                state_changed.notify_all ();
                state_changed.wait (lock, [&] { return release; });
            })) {
            return 54;
        }
        {
            std::unique_lock lock (state_mutex);
            if (!state_changed.wait_for (lock, std::chrono::seconds (1), [&] { return entered; })) {
                return 55;
            }
        }
        if (!saturated_executor.try_submit ([&] { ++queued_runs; })) {
            return 56;
        }
        if (!saturated_executor.try_submit ([&] { ++queued_runs; })) {
            return 57;
        }
        {
            std::lock_guard lock (state_mutex);
            release = true;
        }
        state_changed.notify_all ();
        saturated_executor.drain ();
        if (queued_runs.load () != 2) {
            return 58;
        }
    }

    if (!verify_timer_handler_activation_lifetime ()) {
        return 40;
    }
    if (!verify_throwing_timer_callback_preserves_default_policy ()) {
        std::cerr << "throwing timer callback changed the default exception policy\n";
        return 137;
    }
    if (!verify_timer_cancel_generation_contract ()) {
        return 114;
    }
    if (!verify_close_waits_for_timer_callback_barrier ()) {
        return 41;
    }
    if (!verify_suspended_lifecycle_keeps_fifo_and_allows_application ()) {
        return 132;
    }
    if (!verify_suspended_lifecycle_resumes_at_original_fifo_head ()) {
        return 133;
    }
    if (!verify_ready_lifecycle_continuation_pays_burst_debt ()) {
        return 134;
    }
    if (!verify_suspended_lifecycle_cancellation_releases_fifo ()) {
        return 135;
    }
    if (!verify_delayed_barrier_activation_uses_retained_turn ()) {
        return 136;
    }
    if (!verify_actor_join_finalize_replies_after_target_activation ()) {
        std::cerr << "actor cutover dispatcher regression failed\n";
        return 59;
    }
    if (!verify_spot_close_waits_for_earlier_lifecycle_join ()) {
        return 131;
    }
    if (!verify_timer_terminal_precedes_lifecycle_close_turn ()) {
        return 120;
    }
    if (!verify_timer_self_cancel_completes_after_callback ()) {
        return 121;
    }

    if (!verify_request_turn_mode (false, {1, 3, 2})) {
        return 25;
    }
    if (!verify_request_turn_mode (true, {1, 2, 3})) {
        return 26;
    }
    if (!verify_serial_resume_waits_behind_queued_work ()) {
        return 54;
    }
    if (!verify_serial_queue_lanes_are_unbounded ()) {
        return 50;
    }
    if (!verify_transferred_owner_reservation_is_continuous_until_terminal ()) {
        return 112;
    }
    if (!verify_transferred_owner_reservation_shares_unbounded_lifecycle_lane ()) {
        return 113;
    }
    if (!verify_serial_queue_owner_time_budget ()) {
        return 56;
    }
    if (!verify_serial_executor_submission_paths_select_queues ()) {
        return 118;
    }
    if (!verify_per_actor_two_actors_overlap ()) {
        return 119;
    }
    if (!verify_spot_wide_two_actors_are_serial ()) {
        return 120;
    }
    if (!verify_same_actor_fifo_in_both_modes ()) {
        return 121;
    }
    if (!verify_per_actor_timer_names_overlap_and_keep_fifo ()) {
        return 122;
    }
    if (!verify_actor_mailbox_backlog_is_accepted_in_both_modes ()) {
        return 123;
    }
    if (!verify_spot_wide_large_and_small_payloads_are_queued ()) {
        return 124;
    }
    if (!verify_spot_wide_upper_queue_accepts_former_boundaries ()) {
        return 125;
    }
    if (!verify_same_actor_synchronous_reentry_is_immediate ()) {
        return 127;
    }
    if (!verify_spot_wide_yield_retains_actor_claim ()) {
        return 126;
    }
    if (!verify_spot_wide_yield_blocks_idle_eviction_until_handler_terminal ()) {
        return 128;
    }
    if (!verify_cancellable_serial_submission_lifecycle ()) {
        return 92;
    }
    if (!verify_released_spot_turn_does_not_inline_lifecycle_task ()) {
        return 129;
    }
    if (!verify_relocation_ready_keeps_registering_turn_across_state_lane ()) {
        std::cerr << "relocation readiness lost the registering application turn" << std::endl;
        return 130;
    }
    if (!verify_spot_serial_task_async_shutdown_settlement ()) {
        return 93;
    }
    if (!verify_spot_serial_task_async_retains_work_until_terminal ()) {
        return 96;
    }
    if (!verify_common_dispatch_limits ()) {
        return 55;
    }
    if (!verify_fixture_accounting_boundaries ()) {
        return 60;
    }
    if (!verify_fixture_arbitration_and_owner_isolation ()) {
        return 61;
    }
    if (!verify_serial_lane_policies ()) {
        return 57;
    }
    if (!verify_runtime_observation_loss_and_terminal_retention ()) {
        return 52;
    }
    if (!verify_idle_instance_spot_eviction_closes_local_context ()) {
        return 53;
    }
    if (!verify_draining_host_refuses_new_admission_with_shutting_down ()) {
        return 138;
    }
    if (!verify_explicit_instance_spot_close_releases_authority_after_callback ()) {
        return 91;
    }
    if (!verify_close_terminal_for_operational_boundary ())
        return 140;
    if (!verify_spot_close_returns_turn_while_store_step_is_pending ()) {
        std::cerr << "Spot Close kept its lifecycle turn while a Store step was pending\n";
        return 137;
    }
    if (!verify_remote_actor_prepare_is_idempotent ()) {
        return 58;
    }
    if (!verify_wire_actor_join_admission_is_approval_only_and_later_attempt_wins ()) {
        std::cerr << "wire actor Join approval-only admission regression failed\n";
        return 117;
    }
    if (!verify_wire_join_requires_active_local_target ()) {
        std::cerr << "wire actor Join accepted an inactive target or held the receive loop\n";
        return 138;
    }
    if (!verify_relocation_abort_does_not_hold_receive_worker ()) {
        std::cerr << "relocation abort held the receive worker during Spot Close\n";
        return 139;
    }
    if (!verify_join_commit_does_not_wait_for_joined_callback ()) {
        std::cerr << "Join commit held the receive worker during OnJoinedActor\n";
        return 140;
    }
    if (!verify_join_commit_does_not_wait_for_joined_callback (true)
        || !verify_join_commit_does_not_wait_for_joined_callback (true, true)) {
        std::cerr
          << "materialized Actor Join lifecycle failure lost its Failed completion or barrier\n";
        return 141;
    }
    if (!verify_target_commit_stages_source_prefix_before_live_dispatch ()) {
        return 94;
    }
    if (!verify_actor_join_prewarm_parks_arrival_before_prepare ()) {
        std::cerr << "actor Join prewarm parking regression failed\n";
        return 113;
    }
    if (!verify_actor_join_prewarm_newest_attempt_evicts_placeholder ()) {
        std::cerr << "actor Join prewarm newest-attempt eviction regression failed\n";
        return 114;
    }
    if (!verify_actor_join_prewarm_fail_commit_clears_parked_backlog ()) {
        std::cerr << "actor Join prewarm fail_commit backlog cleanup regression failed\n";
        return 115;
    }
    if (!verify_actor_join_prewarm_newest_attempt_evicts_live_attempt_past_prepare ()) {
        std::cerr << "actor Join prewarm live-attempt (past PREPARE) eviction regression failed\n";
        return 116;
    }
    if (!verify_remote_actor_cutover_completion_is_target_owned ()) {
        std::cerr << "actor cutover production ownership regression failed\n";
        return 95;
    }
    if (!verify_teardown_observes_actor_handoff_terminal ())
        return EXIT_FAILURE;
    if (!verify_remote_actor_cutover_completion_is_target_owned (true, true, true))
        return EXIT_FAILURE;
    if (!verify_remote_actor_cutover_completion_is_target_owned (true)) {
        std::cerr << "source mesh cancellation terminal did not drain\n";
        return EXIT_FAILURE;
    }
    if (!verify_remote_actor_cutover_completion_is_target_owned (true, true)) {
        std::cerr << "source mesh UNKNOWN terminal did not drain\n";
        return EXIT_FAILURE;
    }
    if (!verify_remote_actor_completion_keeps_session_ref_until_route_ack ()) {
        return 62;
    }

    return verify_workers_and_spot_gate ();
}
