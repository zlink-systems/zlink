/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-A1: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_a1_scenario ()
{
    const auto spot_id = "spot-local-ok-" + unique_suffix ();
    const auto node_a_store_before =
      get_relocation_store_activity (_nodes.a);
    const auto spot = create_spot (_nodes.a, spot_id);
    const auto actor_id = "actor-local-ok-" + unique_suffix ();
    const auto actor =
      create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 11);
    require (
      actor.node_rid == spot.node_rid,
      "ST-A1 could not obtain a Framework-selected Actor and Spot on the same owner");
    const auto source_ref = get_actor_ref (_nodes.a, actor_id);
    auto &owner =
      spot.node_rid == "actor-a" ? _nodes.a : _nodes.b;

    const auto join = join_actor (owner, actor_id, {"ST-A1", spot_id});
    require (join.accepted, "ST-A1 join was rejected.");

    const auto probe = probe_actor (owner, actor_id, {"ST-A1", "after-joined"});
    require (probe.node_rid == spot.node_rid,
             "ST-A1 probe expected " + spot.node_rid + ", got " + probe.node_rid);
    require (probe.spot_id == spot_id, "ST-A1 probe did not reach target spot.");
    const auto committed_ref = get_actor_ref (owner, actor_id);
    require (
      committed_ref.generation == source_ref.generation,
      "ST-A1 same-node Join changed Actor ObjectGeneration.");

    assert_evidence_sequence (
      owner,
      {"ST-A1|" + actor_id + "|admission|spot=" + spot_id,
       "message_flow|" + actor_id + "|location_committed|",
       "transfer|" + actor_id + "|joined|" + spot_id + ":11",
       "transfer|" + actor_id + "|leave|11",
       "ST-A1|" + actor_id + "|join_completion_accepted|"});
    wait_evidence (
      owner,
      {"ST-A1|" + actor_id + "|success_reply|" + spot_id,
       "ST-A1|" + actor_id + "|packet_handler|after-joined"});

    require (
      get_relocation_store_activity (_nodes.a) == node_a_store_before,
      "ST-A1 local join accessed the Relocation Store.");
    for (const auto &entry : get_evidence (owner)) {
        if (entry.actor_id != actor_id) {
            continue;
        }
        require (entry.kind.find ("message_follow") == std::string::npos,
                 "ST-A1 local join used Message Follow.");
        require (entry.kind != "transfer_out" && entry.kind != "transfer_in",
                 "ST-A1 local join used the remote relocation adapter.");
    }
}

} // namespace
