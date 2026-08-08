/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-A2: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_a2_scenario ()
{
    const auto spot = create_spot_until_placed_on (
      _nodes.a, "spot-local-reject-" + unique_suffix (),
      "actor-a", "reject");
    const auto actor = create_actor_until_placed_on (
      _nodes.a, "actor-local-reject-" + unique_suffix (),
      e2e::actor_type_stateful, 12, "actor-a");
    const auto &actor_id = actor.actor_id;
    const auto &spot_id = spot.spot_id;

    const auto join = join_actor (_nodes.a, actor_id, {"ST-A2", spot_id, "reject"});
    require (join.accepted, "ST-A2 deferred Join was not submitted.");

    const auto evidence =
      wait_evidence (_nodes.a, {
        "ST-A2|" + actor_id + "|admission|spot=" + spot_id,
        "ST-A2|" + actor_id + "|join_completion_rejected|"});
    require_no_contains (evidence, "transfer|" + actor_id + "|joined|" + spot_id,
                         "ST-A2 joined side effect should not exist.");
    const auto probe = probe_actor (
      _nodes.a, actor_id, {"ST-A2", "after-reject"});
    require (probe.marker == "after-reject"
               && probe.node_rid == "actor-a",
             "ST-A2 rejected Actor did not remain at the source.");
}

} // namespace
