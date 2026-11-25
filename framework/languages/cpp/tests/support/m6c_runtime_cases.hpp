/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <memory>

namespace zlink::framework::runtime::stateful
{
class authority_relocation_port_t;
}

namespace m6c_test
{

struct test_context_t
{
    int failures = 0;

    void require (bool condition, const char *message);
};

// Relocation §4.3: one source deadline covers Restore and remote Session readiness.
enum class ready_boundary_case_t
{
    ready,
    expired_remote,
    expired_local,
    shutdown_restore,
    shutdown_route
};

void test_actor_return_to_entry_spot_skips_admission_and_runs_lifecycle_callbacks (
  test_context_t &test,
  const std::shared_ptr<zlink::framework::runtime::stateful::authority_relocation_port_t>
    &authority);
void test_application_relocation_remote_production_path (test_context_t &test,
                                                         bool resolver_failure = false,
                                                         bool post_seal_lookup_failure = false,
                                                         bool missing_target_session_peer = false,
                                                         bool command44_order = false,
                                                         bool binding_absent = false);
void test_return_actor_relocation_replaces_departed_spot_instance (
  test_context_t &test, bool source_leave_fails = false, bool delayed_source_completion = false);
void test_relocation_target_cutover_and_authority_settlement (test_context_t &test);
void test_relocation_request_source_registration (test_context_t &test,
                                                  bool boundary,
                                                  bool aggregate = false);
void test_source_relocation_failure_stops_state_chunks_and_unseals (test_context_t &test);
void test_source_relocation_metric_outcomes (test_context_t &test);
void test_relocation_metric_starts_only_after_unit_admission (test_context_t &test);
void test_spot_restore_stages_before_publication (test_context_t &test);
void test_application_user_spot_aggregate_remote_production_path (test_context_t &test);
void test_relocation_ready_boundary (test_context_t &test, ready_boundary_case_t mode);
void test_boundary_application_preserves_original_reply (test_context_t &test,
                                                         bool no_follow = false,
                                                         bool source_expired = false);
void test_source_relocation_settles_by_authority (test_context_t &test,
                                                  bool cancel_management_delay = false,
                                                  bool cancel_cutover_submit = false,
                                                  bool cancel_pending_wait = false,
                                                  bool expired_teardown = false);
void test_delay_owner_final_join_closes_accepted_registration (test_context_t &test);
void test_source_settlement_after_subsequent_moves (test_context_t &test);
void test_source_settlement_preserve_and_members (test_context_t &test);
void test_pending_actor_turn_survives_queue_removal (test_context_t &test,
                                                     bool cancel_pending = false);
void test_cutover_replays_over_selected_network_route (test_context_t &test);
void test_public_authority_store_adapter (test_context_t &test);
void test_relocation_target_shutdown (test_context_t &test, int mode);
void test_spot_lifecycle_domain_rejects_invalid_kind_combinations (test_context_t &test);
void test_generation_barrier_quiesces_yield_spot_and_timer (test_context_t &test);
void test_relocation_ready_completion_runs_once_on_spot_turn (test_context_t &test);
void test_relocation_ready_defer_rejects_only_its_own_turn (test_context_t &test);
void test_relocation_ready_defer_holds_queued_timer_turn (test_context_t &test);
void test_actor_leave_after_relocation_defer_runs_lifecycle_callbacks (test_context_t &test);
void test_remote_actor_leave_commits_before_source_callback (test_context_t &test);
void test_temporary_channel_request_yield_owns_call_state (test_context_t &test);
void test_accepted_message_payload_is_deserialized_once (test_context_t &test);
void test_close_eligibility_preserves_execution_and_deletion_is_fenced (test_context_t &test);
void test_envelope_round_trip (test_context_t &test);
void test_actor_join_recovery_round_trip (test_context_t &test);
void test_concurrent_spot_restore_owns_one_reservation (test_context_t &test);
void test_restore_validates_generation_before_spot_publication (test_context_t &test);
void test_return_relocation_replaces_departed_remnant (test_context_t &test);
void test_pending_restore_holds_ingress_before_rollback (test_context_t &test);
void test_public_relocation_store_adapter (test_context_t &test);
void test_boundary_preserve_restores_acceptance_order (test_context_t &test);
void test_source_completion_wait_scope (test_context_t &test);
void test_aggregate_seal_failure_preserves_earlier_application_work (test_context_t &test);
void test_relocation_adapter_single_capture_restore_path (test_context_t &test);
void test_entry_spot_actor_relocation_restore_resolves_local_entry_spot (test_context_t &test);
void test_entry_spot_actor_relocation_restore_fails_without_local_entry_spot (test_context_t &test);
void test_relocation_hold_restores_without_dedicated_limits (test_context_t &test);
void test_stateful_application_queue_accepts_active_backlog (test_context_t &test);
void test_advertised_receive_chunk_limit_wiring (test_context_t &test);
void test_actor_join_wire_gate_records_target_authority (test_context_t &test);

} // namespace m6c_test
