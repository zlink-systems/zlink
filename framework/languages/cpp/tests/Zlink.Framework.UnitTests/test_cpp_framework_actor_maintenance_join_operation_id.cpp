/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <variant>

namespace zlink::framework::test::actor_maintenance_join
{
TEST (ActorMaintenanceJoin, RemoteDeferredJoinAcceptedPreservesOperationId)
{
    auto evidence = std::make_shared<observations_t> ();
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target;
    release_callbacks_t release{*evidence};
    configure (source, evidence, first_rid, store, relocations);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto source_services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (source_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (set_published_placement_weight (source_services, 0));

    configure (target, evidence, second_rid, store, relocations);
    target.start ();
    ASSERT_TRUE (ready (target));
    auto target_services = target.app.advanced ().services ().build_provider ();
    auto &spots = target_services.get_required<fw::spot_manager_t> ();
    for (const auto *spot : {rejected_spot, local_spot})
        ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (spot), room_type).async ().result ());
    ASSERT_TRUE (
      peer_ready (source_services.get_required<fw::route_mesh_runtime_t> (), second_rid));

    auto &client = source_services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (move_to (client, *evidence, rejected_spot, 1));
    std::uint64_t source_high;
    {
        std::lock_guard lock (evidence->mutex);
        EXPECT_EQ (evidence->completion_owners.front (), first_rid);
        const auto *rejected =
          std::get_if<fw::actor_join_rejected_t> (&evidence->completions.front ());
        ASSERT_NE (rejected, nullptr);
        source_high = rejected->operation_id_high;
    }
    ASSERT_NE (source_high, 0u);
    const auto source_mesh =
      fw::detail::mesh_node_runtime_t::registrations (source.app.advanced ().zlink ()).front ();
    const auto source_low = source_mesh->next_join_completion_operation.load ();

    ASSERT_TRUE (move_to (client, *evidence, local_spot, 2));
    EXPECT_EQ (source_mesh->next_join_completion_operation.load (), source_low + 1);
    std::lock_guard lock (evidence->mutex);
    ASSERT_EQ (evidence->completions.size (), 2u);
    EXPECT_EQ (evidence->completion_owners.back (), second_rid);
    const auto *accepted = std::get_if<fw::actor_join_accepted_t> (&evidence->completions.back ());
    ASSERT_NE (accepted, nullptr);
    EXPECT_EQ (accepted->operation_id_high, source_high);
    EXPECT_EQ (accepted->operation_id_low, source_low);
}
} // namespace zlink::framework::test::actor_maintenance_join
