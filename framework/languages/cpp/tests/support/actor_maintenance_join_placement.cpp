/* SPDX-License-Identifier: Elastic-2.0 */

#include "actor_maintenance_join_host_fixture.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include <zlink/framework/contracts/configuration/route_mesh_runtime_options.hpp>
#include <algorithm>
#include <thread>

namespace zlink::framework::test::actor_maintenance_join
{
// Placement uses the requesting host's discovery observation of the published weight.
bool set_published_placement_weight (fw::service_provider_t &services,
                                     int weight,
                                     fw::service_provider_t *requester)
{
    const auto owner =
      services.get_required<fw::runtime::location_runtime_t> ().current_owner_token ();
    if (!owner)
        return false;
    const auto owns_descriptor = [&] (const fw::mesh_node_descriptor_t &descriptor) {
        return descriptor.owner_id == owner->owner_id
               && descriptor.lease_generation == owner->lease_generation;
    };
    const auto page = services.get_required<fw::location_repository_t> ()
                        .list_mesh_nodes (mesh_name)
                        .result ()
                        .value ();
    const auto published = std::find_if (page.items.begin (), page.items.end (), owns_descriptor);
    auto &options = services.get_required<fw::route_mesh_runtime_options_t> ().mesh (mesh_name);
    if (options.placement_weight () != weight)
        options.placement_weight (weight);
    const auto &discovery =
      (requester ? *requester : services).get_required<fw::runtime::live_location_reader_t> ();
    const auto observed_weight = [&] {
        const auto observation = discovery.mesh_observation (mesh_name);
        return observation
               && std::any_of (
                 observation->sources.begin (), observation->sources.end (), [&] (const auto &row) {
                     const auto &descriptor = row.descriptor;
                     return owns_descriptor (descriptor) && descriptor.placement_weight == weight
                            && (published == page.items.end ()
                                || descriptor.descriptor_revision
                                     >= published->descriptor_revision);
                 });
    };
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    while (!observed_weight () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    return observed_weight ();
}
} // namespace zlink::framework::test::actor_maintenance_join
