/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-F1: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_f1_scenario ()
{
    const auto source_spot = create_spot_until_placed_on (
      _nodes.a, "spot-inflight-source-" + unique_suffix (), "actor-a");
    const auto target_spot = create_spot_until_placed_on (
      _nodes.b, "spot-inflight-order-" + unique_suffix (),
      "actor-b", "delay-joined");
    const auto actor = create_actor_until_placed_on (
      _nodes.a, "actor-inflight-order-" + unique_suffix (),
      e2e::actor_type_stateful, 101, "actor-a");
    const auto &actor_id = actor.actor_id;
    const auto &spot_id = target_spot.spot_id;
    require (join_actor (_nodes.a, actor_id,
                         {"ST-F1-setup", source_spot.spot_id}).accepted,
             "ST-F1 could not place the Actor in its source User Spot.");
    const auto old_ref = get_actor_ref (_nodes.a, actor_id);

    bound_session_t bound (_nodes.a_stream_endpoint, "ST-F1", old_ref);
    bound.send_packet ({"ST-F1", "old-1"});
    wait_evidence (_nodes.a, {"ST-F1|" + actor_id + "|handler_wait|old-1"});
    bound.send_packet ({"ST-F1", "old-2"});

    auto join_task = std::async (
      std::launch::async, [&] { return join_actor (_nodes.a, actor_id, {"ST-F1", spot_id}); });
    bound.send_packet ({"ST-F1", "moving-1"});
    bound.send_packet ({"ST-F1", "moving-2"});
    const auto released = release_handler_gate (_nodes.a, actor_id);
    require (released.released, "ST-F1 old-1 handler gate was already released.");
    wait_evidence (_nodes.b, {"ST-F1|" + actor_id + "|joined_wait|" + spot_id});
    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    wait_evidence (_nodes.a,
                   {"message_flow|" + actor_id + "|handoff_backlog|"});

    release_joined_gate (_nodes.b, spot_id);
    require (join_task.get ().accepted, "ST-F1 transfer was rejected.");
    wait_evidence (_nodes.b, {"message_flow|" + actor_id + "|backlog_enqueued|",
                              "message_flow|" + actor_id + "|location_committed|"});
    wait_evidence (_nodes.b,
                   {"ST-F1|" + actor_id + "|handoff_packet|old-2",
                    "ST-F1|" + actor_id + "|handoff_packet|moving-1",
                    "ST-F1|" + actor_id + "|handoff_packet|moving-2"});
    assert_correlated_transfer_markers (
      {&_nodes.a, &_nodes.b}, actor_id,
      {"handoff_backlog", "backlog_enqueued", "location_committed"});
    auto evidence = get_evidence (_nodes.a);
    const auto target_evidence = get_evidence (_nodes.b);
    evidence.insert (evidence.end (), target_evidence.begin (), target_evidence.end ());
    std::vector<std::string> handled;
    for (const auto &entry : evidence) {
        if (entry.scenario == "ST-F1" && entry.actor_id == actor_id
            && entry.kind == "handoff_packet") {
            handled.push_back (entry.value);
        }
    }
    require (handled == std::vector<std::string>{"old-1", "old-2", "moving-1", "moving-2"},
             "ST-F1 handler order or exactly-once invariant changed across relocation.");
}

} // namespace
