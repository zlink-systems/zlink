/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-F2: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_f2_scenario ()
{
    const auto spot = create_spot_until_placed_on (
      _nodes.b, "spot-inflight-overtake-" + unique_suffix (),
      "actor-b", "delay-joined");
    const auto actor = create_actor_until_placed_on (
      _nodes.a, "actor-inflight-overtake-" + unique_suffix (),
      e2e::actor_type_stateful, 102, "actor-a");
    const auto &actor_id = actor.actor_id;
    const auto &spot_id = spot.spot_id;
    const auto old_ref = get_actor_ref (_nodes.a, actor_id);
    auto join_client = make_http (_nodes.a_url);
    auto join_task = std::async (
      std::launch::async,
      [&] { return join_actor (join_client, actor_id, {"ST-F2", spot_id}); });
    wait_evidence (_nodes.b, {"ST-F2|" + actor_id + "|joined_wait|" + spot_id});
    for (const auto *marker : {"B1", "B2"}) {
        send_ref (_nodes.a, actor_id, old_ref, {"ST-F2", marker});
    }
    wait_evidence (_nodes.a, {"message_flow|" + actor_id + "|handoff_backlog|"});
    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    release_joined_gate (_nodes.b, spot_id);
    wait_evidence (
      _nodes.b, {"message_flow|" + actor_id + "|location_committed|"});
    wait_evidence (_nodes.b,
                   {"message_flow|" + actor_id + "|backlog_enqueued|"});
    const auto direct =
      probe_actor (_nodes.b, actor_id, {"ST-F2", "D1"});
    require (direct.node_rid == "actor-b",
             "ST-F2 direct request did not execute on the target node.");
    require (join_task.get ().accepted,
             "ST-F2 deferred Join was not submitted.");
    wait_evidence (
      _nodes.b, {"ST-F2|" + actor_id + "|join_completion_accepted|"});
    assert_evidence_sequence (
      _nodes.b,
      {"message_flow|" + actor_id + "|location_committed|",
       "ST-F2|" + actor_id + "|handoff_packet|B1",
       "ST-F2|" + actor_id + "|handoff_packet|B2",
       "ST-F2|" + actor_id + "|packet_handler|D1"});
}

} // namespace
