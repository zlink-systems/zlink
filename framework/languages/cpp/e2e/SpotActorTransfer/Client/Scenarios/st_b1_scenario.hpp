/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-B1: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_b1_scenario ()
{
    const auto spot = create_spot_until_placed_on (
      _nodes.b, "spot-remote-ok-" + unique_suffix (), "actor-b");
    const auto actor = create_actor_until_placed_on (
      _nodes.a, "actor-remote-ok-" + unique_suffix (),
      e2e::actor_type_stateful, 21, "actor-a");
    const auto &actor_id = actor.actor_id;
    const auto &spot_id = spot.spot_id;

    const auto join = join_actor (_nodes.a, actor_id, {"ST-B1", spot_id});
    require (join.accepted, "ST-B1 deferred Join was not submitted.");
    wait_evidence (
      _nodes.b, {"ST-B1|" + actor_id + "|join_completion_accepted|"});

    const auto probe = probe_actor (_nodes.a, actor_id, {"ST-B1", "after-transfer"});
    require (probe.node_rid == "actor-b", "ST-B1 probe expected actor-b, got " + probe.node_rid);
    require (probe.state_version == 21,
             "ST-B1 state version expected 21, got " + std::to_string (probe.state_version));

    assert_evidence_sequence (_nodes.a, {"transfer|" + actor_id + "|transfer_out|21",
                                         "transfer|" + actor_id + "|leave|21",
                                         "message_flow|" + actor_id + "|commit_request|",
                                         "message_flow|" + actor_id + "|commit_ack|"});
    wait_evidence (_nodes.a, {"ST-B1|" + actor_id + "|success_reply|" + spot_id});
    assert_evidence_sequence (_nodes.b, {"ST-B1|" + actor_id + "|admission|",
                                         "transfer|" + actor_id + "|transfer_in|21",
                                         "message_flow|" + actor_id + "|location_committed|",
                                         "transfer|" + actor_id + "|joined|" + spot_id + ":21",
                                         "ST-B1|" + actor_id + "|packet_handler|after-transfer"});
    std::size_t handler_count = 0;
    for (const auto &entry : get_evidence (_nodes.b)) {
        if (entry.scenario == "ST-B1" && entry.actor_id == actor_id
            && entry.kind == "packet_handler" && entry.value == "after-transfer") {
            ++handler_count;
        }
    }
    require (handler_count == 1,
             "ST-B1 request was not dispatched exactly once after transfer.");
    wait_evidence (_nodes.a, {"message_flow|" + actor_id + "|source_cleanup|"});

    assert_correlated_transfer_markers (
      {&_nodes.a, &_nodes.b}, actor_id,
      {"commit_request", "location_committed", "commit_ack", "source_cleanup"});
}

} // namespace
