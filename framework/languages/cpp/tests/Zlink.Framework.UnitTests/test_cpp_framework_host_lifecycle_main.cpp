/* SPDX-License-Identifier: Elastic-2.0 */

#include "host_lifecycle_tests.hpp"
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include <cstdlib>
#include <string_view>

int main (int argc, char **argv)
{
    using namespace zlink::framework::test::host_lifecycle;
    zlink::framework::completion_owner_t completion_owner;
    if (argc == 2 && std::string_view (argv[1]) == "relocation-missing-source")
        return verify_relocated_actor_follow_and_shutdown (false, true) ? EXIT_SUCCESS
                                                                        : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "relocated-actor-shutdown")
        return verify_relocated_actor_follow_and_shutdown () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "relocated-aggregate-shutdown")
        return verify_relocated_actor_follow_and_shutdown (true) ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "accepted-source-shutdown")
        return verify_application_signaled_relocation (true) ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "stopped-host-rerun")
        return verify_deferred_framework_apply_preserves_hosted_service_order () ? EXIT_SUCCESS
                                                                                 : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "mesh-discovery-stop")
        return verify_mesh_discovery_stop_wakes_polling_waits () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "no-mesh-target-version")
        return verify_host_without_mesh_validates_target_version () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "degraded-host-relocation")
        return verify_degraded_host_blocks_and_resumes_relocation () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "retry-host-relocation")
        return verify_relocation_retry_after_target_unavailable () ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && std::string_view (argv[1]) == "empty-host-relocation")
        return verify_host_relocation_needs_target_only_for_units () ? EXIT_SUCCESS : EXIT_FAILURE;

    if (argc == 2 && std::string_view (argv[1]) == "relocation-topology")
        return verify_relocation_topology ();

    if (const char *iterations = std::getenv ("ZLINK_CPP_FIRST_ACTOR_REQUEST_ITERATIONS")) {
        return verify_first_request_after_remote_actor_create (std::atoi (iterations))
                 ? EXIT_SUCCESS
                 : EXIT_FAILURE;
    }

    if (!verify_degraded_host_republishes_descriptor_after_owner_claim ())
        return EXIT_FAILURE;

    if (!verify_degraded_host_blocks_and_resumes_relocation ())
        return EXIT_FAILURE;

    if (!verify_deferred_framework_apply_preserves_hosted_service_order ())
        return EXIT_FAILURE;

    if (!verify_object_store_configuration_preflight ())
        return EXIT_FAILURE;

    if (!verify_relocation_target_eligibility_applies_full_narrowing ())
        return EXIT_FAILURE;

    if (!verify_remote_actor_create_target_owns_completion ())
        return EXIT_FAILURE;

    if (!verify_first_request_after_remote_actor_create (5))
        return EXIT_FAILURE;

    if (!verify_mesh_discovery_stop_wakes_polling_waits ())
        return EXIT_FAILURE;

    if (!verify_host_without_mesh_validates_target_version ())
        return EXIT_FAILURE;

    if (!verify_host_relocation_needs_target_only_for_units ())
        return EXIT_FAILURE;

    if (!verify_relocation_retry_after_target_unavailable ())
        return EXIT_FAILURE;

    if (!verify_relocating_status_closes_admission ())
        return EXIT_FAILURE;

    if (!verify_relocation_join_ignores_deadline ())
        return EXIT_FAILURE;

    if (!verify_relocation_reject_reports_requested_target ())
        return EXIT_FAILURE;

    if (!verify_application_signaled_relocation (true))
        return EXIT_FAILURE;

    if (std::getenv ("ZLINK_CPP_RUN_HOST_RELOCATION_INTEGRATION")
        && !verify_application_signaled_relocation ())
        return EXIT_FAILURE;

    if (verify_relocation_topology () != EXIT_SUCCESS)
        return EXIT_FAILURE;

    return verify_shutdown_termination ();
}
