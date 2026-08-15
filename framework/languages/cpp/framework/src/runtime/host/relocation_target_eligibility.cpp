#include "runtime/host/relocation_target_eligibility.hpp"
#include "runtime/client_server/weighted_selector.hpp"

#include <algorithm>

namespace zlink::framework
{
namespace
{

bool capacity_available (const capacity_usage_t &usage)
{
    return usage.limit == 0
           || usage.active + usage.reserved < static_cast<std::uint64_t> (usage.limit);
}

// Candidate supports the required (kind, stable_type), honoring the
// source-declared relocation policy (a snapshot policy needs a snapshot
// adapter) and population/reservation capacity. This is the per-capability
// rule of the preflight supports_relocation_source, narrowed to one required
// type of the relocation unit.
bool candidate_supports (const mesh_node_descriptor_t &source,
                         const mesh_node_descriptor_t &candidate,
                         placement_object_kind_t kind,
                         std::string_view stable_type)
{
    const auto required = std::find_if (
      source.object_capabilities.begin (), source.object_capabilities.end (),
      [&] (const object_capability_t &value) {
          return value.object_kind == kind && value.stable_type == stable_type;
      });
    const bool requires_snapshot =
      required != source.object_capabilities.end ()
      && required->policy == maintenance_policy_kind_t::snapshot;

    const auto supported = std::find_if (
      candidate.object_capabilities.begin (), candidate.object_capabilities.end (),
      [&] (const object_capability_t &value) {
          return value.object_kind == kind && value.stable_type == stable_type
                 && (!requires_snapshot
                     || (value.policy == maintenance_policy_kind_t::snapshot
                         && value.has_snapshot_adapter));
      });
    if (supported == candidate.object_capabilities.end ())
        return false;

    if (kind == placement_object_kind_t::actor)
        return capacity_available (candidate.capacity.actors);

    if (!capacity_available (candidate.capacity.spots))
        return false;
    const auto typed = std::find_if (
      candidate.capacity.spot_types.begin (), candidate.capacity.spot_types.end (),
      [&] (const spot_type_capacity_t &value) {
          return value.object_kind == kind && value.stable_type == stable_type;
      });
    return typed == candidate.capacity.spot_types.end ()
           || capacity_available (typed->usage);
}

}

bool relocation_unit_target_eligible (
  const mesh_node_descriptor_t &source,
  const mesh_node_descriptor_t &candidate,
  std::int64_t effective_target_application_version,
  std::string_view spot_type,
  const std::vector<std::string> &actor_types)
{
    if (candidate.state != framework_runtime_state_t::serving
        || candidate.object_role != object_role_t::server
        || candidate.placement_weight <= 0
        || candidate.application_version != effective_target_application_version
        || (source.maintenance_wave
            && candidate.maintenance_wave == source.maintenance_wave)) {
        return false;
    }
    // An actor-only relocation unit has no user Spot requirement.
    if (!spot_type.empty ()
        && !candidate_supports (source, candidate, placement_object_kind_t::user_spot,
                                spot_type))
        return false;
    for (const auto &actor_type : actor_types) {
        if (!candidate_supports (source, candidate, placement_object_kind_t::actor, actor_type))
            return false;
    }
    return candidate.activation_concurrency.limit <= 0
           || candidate.activation_concurrency.active
                < static_cast<std::uint32_t> (candidate.activation_concurrency.limit);
}

std::optional<std::string> select_weighted_relocation_target (
  std::span<const std::pair<std::string, std::uint32_t>> candidates)
{
    std::vector<runtime::client_server::weighted_candidate_t> weighted;
    weighted.reserve (candidates.size ());
    for (const auto &[key, weight] : candidates)
        weighted.push_back (
          runtime::client_server::weighted_candidate_t{key, weight, key});
    runtime::client_server::smooth_weighted_selector_t selector;
    selector.set_candidates (weighted);
    return selector.select ();
}

}
