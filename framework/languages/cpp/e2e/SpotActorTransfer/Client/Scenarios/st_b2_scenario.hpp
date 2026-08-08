/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-B2: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_b2_scenario ()
{
    const auto spot = create_spot_until_placed_on (
      _nodes.b, "spot-cleanup-after-success-" + unique_suffix (),
      "actor-b");
    const auto actor = create_actor_until_placed_on (
      _nodes.a, "actor-cleanup-after-success-" + unique_suffix (),
      e2e::actor_type_stateful, 22, "actor-a");
    const auto &actor_id = actor.actor_id;
    const auto &spot_id = spot.spot_id;

    auto join_task = std::async (
      std::launch::async, [&] { return join_actor (_nodes.a, actor_id, {"ST-B2", spot_id}); });
    wait_evidence (_nodes.a, {"message_flow|" + actor_id + "|commit_ack|"});
    const auto join = join_task.get ();
    require (join.accepted, "ST-B2 join was rejected.");
    require_no_contains (get_evidence (_nodes.a), "message_flow|" + actor_id + "|source_cleanup|",
                         "ST-B2 source cleanup completed before source shutdown.");
    const auto before = probe_actor (_nodes.a, actor_id, {"ST-B2", "before-source-cleanup-loss"});
    require (before.node_rid == "actor-b", "ST-B2 probe expected actor-b, got " + before.node_rid);

    shutdown_node (_nodes.a);
    const auto after = probe_actor (_nodes.b, actor_id, {"ST-B2", "after-source-cleanup-loss"});
    require (after.node_rid == "actor-b",
             "ST-B2 target ownership was lost after source shutdown: " + after.node_rid);
    require (after.state_version == 22, "ST-B2 state changed after source cleanup loss: "
                                          + std::to_string (after.state_version));
    wait_evidence (_nodes.b, {
                               "transfer|" + actor_id + "|joined|" + spot_id + ":22",
                               "ST-B2|" + actor_id + "|packet_handler|after-source-cleanup-loss",
                             });
}

} // namespace
