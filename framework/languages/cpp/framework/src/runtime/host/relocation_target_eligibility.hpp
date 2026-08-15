#pragma once

#include "runtime/locations/location_records.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zlink::framework
{

// Applies the frozen relocation candidate narrowing (spec
// server/languages/cpp/interfaces/02-configuration-host, "Both modes narrow
// candidates in the following order") to the ACTUAL target selection of a
// single relocation unit. It mirrors the preflight helper
// supports_relocation_source so the selector cannot pick a candidate the
// preflight would have rejected: a serving Object Server on the effective
// target version, not in source's non-empty maintenance wave, with factory /
// stable-type / relocation-policy (snapshot-adapter) compatibility and
// population/reservation capacity for the unit's user Spot and Actor types.
//
// `spot_type` is the unit's user Spot stable type (empty for an actor-only
// unit, which imposes no user Spot requirement); `actor_types` are the unit's
// Actor stable types. `source` supplies the maintenance wave and the declared
// relocation policy per capability. The Core-peer admitted/ready gate (spec
// step 5) is applied by the caller, which owns the Core peer table.
bool relocation_unit_target_eligible (
  const mesh_node_descriptor_t &source,
  const mesh_node_descriptor_t &candidate,
  std::int64_t effective_target_application_version,
  std::string_view spot_type,
  const std::vector<std::string> &actor_types);

// Applies node-wide placement weight to the final eligible candidate set (spec
// step 6) and returns the selected candidate key. Uses the framework's
// deterministic smooth weighted selector (no randomness), so a higher weight is
// preferred and the choice is reproducible. `candidates` are (key, weight)
// pairs; a zero weight is excluded. Returns nullopt when the set is empty or all
// weights are zero.
std::optional<std::string> select_weighted_relocation_target (
  std::span<const std::pair<std::string, std::uint32_t>> candidates);

}
