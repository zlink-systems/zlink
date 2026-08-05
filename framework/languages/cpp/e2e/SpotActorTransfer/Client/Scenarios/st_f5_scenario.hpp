/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-F5: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_f5_scenario ()
{
    const auto actor_id = "actor-message-follow-chain-" + unique_suffix ();
    const auto spot_b = "spot-map-chain-b-" + unique_suffix ();
    const auto spot_a_final = "spot-map-chain-a-final-" + unique_suffix ();
    const auto placed_spot_b =
      create_spot_until_placed_on (_nodes.b, spot_b, "actor-b");
    const auto placed_spot_a_final =
      create_spot_until_placed_on (_nodes.a, spot_a_final, "actor-a");
    const auto actor = create_actor_until_placed_on (
      _nodes.a, actor_id, e2e::actor_type_stateful, 105, "actor-a");
    const auto old_ref_a = get_actor_ref (_nodes.a, actor.actor_id);
    require (join_actor (_nodes.a, actor.actor_id,
                         {"ST-F5", placed_spot_b.spot_id}).accepted,
             "ST-F5 first transfer was rejected.");
    wait_evidence (
      _nodes.a,
      {"message_flow|" + actor.actor_id
       + "|message_follow_registered|actor-b:" + placed_spot_b.spot_id});
    const auto old_ref_b = get_actor_ref (_nodes.b, actor.actor_id);
    require (join_actor (_nodes.b, actor.actor_id,
                         {"ST-F5", placed_spot_a_final.spot_id}).accepted,
             "ST-F5 chained transfer was rejected.");
    wait_evidence (_nodes.b,
                   {"message_flow|" + actor.actor_id
                    + "|message_follow_registered|actor-a:"
                    + placed_spot_a_final.spot_id});
    require (message_follow_entries (_nodes.a, actor.actor_id).size () == 1,
             "ST-F5 actor-a retained more than one Message Follow route.");
    require (message_follow_entries (_nodes.b, actor.actor_id).size () == 1,
             "ST-F5 actor-b retained more than one Message Follow route.");

    send_ref (_nodes.a, actor.actor_id, old_ref_a, {"ST-F5", "chain-to-final"});
    wait_evidence (
      _nodes.a,
      {"ST-F5|" + actor.actor_id + "|handoff_packet|chain-to-final"});

    wait_evidence (
      _nodes.a,
      {"message_flow|" + actor.actor_id + "|message_follow_route_removed|"});
    wait_evidence (
      _nodes.b,
      {"message_flow|" + actor.actor_id + "|message_follow_route_removed|"});
    const auto current = probe_ref (
      _nodes.a, actor.actor_id, get_actor_ref (_nodes.a, actor.actor_id),
      {"ST-F5", "current-ref-after-route-removal"});
    require (current.succeeded,
             "ST-F5 current target ref failed after Message Follow removal.");
    const auto stale_b = probe_ref (
      _nodes.b, actor.actor_id, old_ref_b,
      {"ST-F5", "after-message-follow-removal-b"});
    require (!stale_b.succeeded
               && (stale_b.error_kind == "ActorLocationStale"
                   || stale_b.error_kind == "ActorRouteNotFound"),
             "ST-F5 expected node-b Message Follow route removal, got '"
               + stale_b.error_kind + "'.");
    const auto evidence = get_evidence (_nodes.a);
    require_no_contains (
      evidence,
      "ST-F5|" + actor.actor_id + "|packet_handler|after-message-follow-removal",
      "ST-F5 packet sent after Message Follow removal reached the target handler.");
}

} // namespace
