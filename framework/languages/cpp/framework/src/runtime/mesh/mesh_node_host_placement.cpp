/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/mesh_node_host_service_internal.hpp"

#include "runtime/execution/task_result.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <numeric>
#include <string_view>

namespace zlink::framework::runtime
{

using mesh_node_host_service_detail::choose_placement_target;

namespace
{
bool capacity_available (const capacity_usage_t &usage)
{
    return usage.limit == 0
           || usage.active + usage.reserved < static_cast<std::uint64_t> (usage.limit);
}

bool placement_capacity_available (const mesh_node_descriptor_t &descriptor,
                                   placement_object_kind_t kind,
                                   std::string_view stable_type)
{
    if (kind == placement_object_kind_t::actor)
        return capacity_available (descriptor.capacity.actors);
    if (!capacity_available (descriptor.capacity.spots))
        return false;
    const auto typed = std::find_if (
      descriptor.capacity.spot_types.begin (), descriptor.capacity.spot_types.end (),
      [&] (const spot_type_capacity_t &candidate) {
          return candidate.object_kind == kind && candidate.stable_type == stable_type;
      });
    return typed == descriptor.capacity.spot_types.end () || capacity_available (typed->usage);
}

bool supports_placement (const mesh_node_descriptor_t &descriptor,
                         placement_object_kind_t kind,
                         std::string_view stable_type)
{
    return descriptor.object_role == object_role_t::server && descriptor.placement_weight > 0
           && (descriptor.state == framework_runtime_state_t::serving
               || descriptor.state == framework_runtime_state_t::preparing)
           && placement_capacity_available (descriptor, kind, stable_type)
           && std::any_of (
             descriptor.object_capabilities.begin (), descriptor.object_capabilities.end (),
             [&] (const auto &capability) {
                 return capability.object_kind == kind && capability.stable_type == stable_type
                        && (kind != placement_object_kind_t::actor
                            || (descriptor.entry_spot_id && capability.entry_accepted));
             });
}
} // namespace

namespace mesh_node_host_service_detail
{
mesh_node_descriptor_t
choose_placement_target (const std::vector<mesh_node_descriptor_t> &candidates,
                         std::string_view identity)
{
    const auto total_weight =
      std::accumulate (candidates.begin (), candidates.end (), std::uint64_t{0},
                       [] (std::uint64_t total, const auto &candidate) {
                           return total + static_cast<std::uint64_t> (candidate.placement_weight);
                       });
    auto choice = std::hash<std::string>{}(std::string (identity)) % total_weight;
    auto selected = candidates.front ();
    for (const auto &candidate : candidates) {
        const auto weight = static_cast<std::uint64_t> (candidate.placement_weight);
        if (choice < weight) {
            selected = candidate;
            break;
        }
        choice -= weight;
    }
    return selected;
}

task_t<std::vector<mesh_node_descriptor_t>>
placement_candidates (live_location_reader_t &discovery,
                      std::string mesh_name,
                      placement_object_kind_t kind,
                      std::string stable_type,
                      std::shared_ptr<detail::mesh_node_runtime_t> source,
                      std::chrono::steady_clock::time_point deadline,
                      std::function<bool (const mesh_node_descriptor_t &)> excluded)
{
    bool refresh_available = true;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto observation = discovery.mesh_observation (mesh_name);
        std::vector<mesh_node_descriptor_t> serving;
        bool wait_for_refresh = false;
        if (observation) {
            const auto local = source->status ();
            for (const auto &row : observation->sources) {
                const auto &descriptor = row.descriptor;
                if (!row.available () || !supports_placement (descriptor, kind, stable_type)
                    || excluded (descriptor))
                    continue;
                if (descriptor.state == framework_runtime_state_t::serving
                    && (descriptor.rid == local.routing_id ()
                          ? descriptor.lifecycle_generation == local.lifecycle_generation ()
                          : source->has_admitted_peer (descriptor.rid,
                                                       descriptor.lifecycle_generation)))
                    serving.push_back (descriptor);
                else
                    wait_for_refresh = true;
            }
        }
        if (observation != discovery.mesh_observation (mesh_name))
            continue;
        if (std::chrono::steady_clock::now () >= deadline)
            break;
        if (!serving.empty ())
            co_return serving;
        if (wait_for_refresh) {
            auto changed = co_await observe_until (observation->changed.task (), deadline);
            if (!changed)
                co_return detail::propagate_failure<std::vector<mesh_node_descriptor_t>> (
                  changed, "Placement discovery wait failed");
        } else if (refresh_available) {
            refresh_available = false;
            co_await discovery.poll_mesh_sources (mesh_name, deadline);
        } else {
            co_return serving;
        }
    }
    co_return result_t<std::vector<mesh_node_descriptor_t>>::failure (
      framework_error_kind_t::deadline_exceeded, "Placement deadline exceeded");
}
} // namespace mesh_node_host_service_detail

task_t<mesh_node_descriptor_t> mesh_node_host_service_t::select_actor_entry_spot (actor_ref_t actor)
{
    const auto source = co_await find_local_actor_node_async (actor.node_rid ());
    if (!source)
        co_return result_t<mesh_node_descriptor_t>::failure (
          framework_error_kind_t::unavailable, "Actor source MeshNode is unavailable");
    const auto identity = co_await source->identity_async ();
    auto candidates =
      co_await _services->get_required<store_location_resolvers_t> ().list_live_mesh_nodes (
        identity.first);
    const auto stable_type = detail::actor_ref_access_t::actor_type (actor);
    const auto source_rid = source->status ().routing_id ();
    std::erase_if (candidates, [&] (const auto &descriptor) {
        return descriptor.state != framework_runtime_state_t::serving
               || !supports_placement (descriptor, placement_object_kind_t::actor, stable_type)
               || (descriptor.rid != source_rid
                   && !source->has_admitted_peer (descriptor.rid, descriptor.lifecycle_generation))
               || descriptor.lifecycle_generation == 0 || descriptor.lease_generation <= 0;
    });
    if (candidates.empty ())
        co_return result_t<mesh_node_descriptor_t>::failure (
          framework_error_kind_t::unavailable, "No compatible Entry Spot target is available");
    co_return choose_placement_target (candidates, actor.actor_id ().value ());
}

} // namespace zlink::framework::runtime
