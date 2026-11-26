/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

namespace zlink::framework::test::host_lifecycle
{
int verify_relocation_topology ();
int verify_shutdown_termination ();
bool verify_remote_actor_create_target_owns_completion ();
bool verify_first_request_after_remote_actor_create (int iterations);
bool verify_degraded_host_republishes_descriptor_after_owner_claim ();
bool verify_deferred_framework_apply_preserves_hosted_service_order ();
bool verify_object_store_configuration_preflight ();
bool verify_degraded_host_blocks_and_resumes_relocation ();
bool verify_host_relocation_needs_target_only_for_units ();
bool verify_host_without_mesh_validates_target_version ();
bool verify_relocation_retry_after_target_unavailable ();
bool verify_relocation_target_eligibility_applies_full_narrowing ();
bool verify_mesh_discovery_stop_wakes_polling_waits ();
bool verify_relocating_status_closes_admission ();
bool verify_relocation_join_ignores_deadline ();
bool verify_relocation_reject_reports_requested_target ();
bool verify_application_signaled_relocation (bool shutdown_during_move = false);
bool verify_relocated_actor_follow_and_shutdown (bool aggregate = false,
                                                 bool missing_source = false);
}
