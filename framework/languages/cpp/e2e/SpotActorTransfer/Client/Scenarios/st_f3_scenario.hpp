/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-F3: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_f3_scenario ()
{
    const auto spot = create_spot_until_placed_on (
      _nodes.b, "spot-bound-order-" + unique_suffix (),
      "actor-b", "delay-joined");
    const auto actor = create_actor_until_placed_on (
      _nodes.a, "actor-bound-order-" + unique_suffix (),
      e2e::actor_type_stateful, 103, "actor-a");
    const auto &actor_id = actor.actor_id;
    const auto &spot_id = spot.spot_id;
    const auto old_ref = get_actor_ref (_nodes.a, actor_id);
    bound_session_t bound (_nodes.a_stream_endpoint, "ST-F3", old_ref);

    auto join_task = std::async (
      std::launch::async, [&] { return join_actor (_nodes.a, actor_id, {"ST-F3", spot_id}); });
    wait_evidence (_nodes.b, {"ST-F3|" + actor_id + "|joined_wait|" + spot_id});
    bound.send_packet ({"ST-F3", "S1"});
    bound.send_packet ({"ST-F3", "S2"});
    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    release_joined_gate (_nodes.b, spot_id);
    wait_evidence (_nodes.b, {"message_flow|" + actor_id + "|location_committed|"});
    bound.send_packet ({"ST-F3", "S3"});
    bound.send_packet ({"ST-F3", "S4"});
    require (join_task.get ().accepted,
             "ST-F3 deferred Join was not submitted.");
    wait_evidence (
      _nodes.b, {"ST-F3|" + actor_id + "|join_completion_accepted|"});
    wait_evidence (
      _nodes.b,
       {"ST-F3|" + actor_id + "|handoff_packet|S1",
        "ST-F3|" + actor_id + "|handoff_packet|S2",
        "ST-F3|" + actor_id + "|handoff_packet|S3",
       "ST-F3|" + actor_id + "|handoff_packet|S4"});
    assert_evidence_sequence (
      _nodes.b,
      {"ST-F3|" + actor_id + "|handoff_packet|S1",
       "ST-F3|" + actor_id + "|handoff_packet|S2",
       "ST-F3|" + actor_id + "|handoff_packet|S3",
       "ST-F3|" + actor_id + "|handoff_packet|S4"});
    std::map<std::string, int> handled;
    for (const auto &entry : get_evidence (_nodes.b)) {
        if (entry.scenario != "ST-F3" || entry.actor_id != actor_id)
            continue;
        if (entry.kind == "handoff_packet") {
            ++handled[entry.value];
        }
        if (entry.kind == "handoff_concurrency")
            require (entry.value == "1", "ST-F3 Actor handlers ran concurrently.");
    }
    for (const auto *marker : {"S1", "S2", "S3", "S4"}) {
        require (handled[marker] == 1,
                 std::string ("ST-F3 handler did not run exactly once: ") + marker);
    }
}

} // namespace
