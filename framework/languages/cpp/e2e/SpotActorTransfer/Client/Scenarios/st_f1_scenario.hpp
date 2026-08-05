/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-F1: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_f1_scenario ()
{
    const auto actor_id = "actor-inflight-order-" + unique_suffix ();
    const auto spot_id = "spot-inflight-order-" + unique_suffix ();
    create_spot (_nodes.b, spot_id, "delay-joined");
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 101);
    const auto old_ref = get_actor_ref (_nodes.a, actor_id);

    auto join_task = std::async (
      std::launch::async, [&] { return join_actor (_nodes.a, actor_id, {"ST-F1", spot_id}); });
    wait_evidence (_nodes.b, {"ST-F1|" + actor_id + "|joined_wait|" + spot_id});
    for (const auto *marker : {"P1", "P2", "P3"}) {
        send_ref (_nodes.a, actor_id, old_ref, {"ST-F1", marker});
    }
    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    const auto source_evidence = get_evidence (_nodes.a);
    require_no_contains (source_evidence, "ST-F1|" + actor_id + "|handoff_packet|",
                         "ST-F1 packet ran on the source node.");
    wait_evidence (_nodes.a, {"message_flow|" + actor_id + "|handoff_backlog|"});

    release_joined_gate (_nodes.b, spot_id);
    require (join_task.get ().accepted, "ST-F1 transfer was rejected.");
    wait_evidence (_nodes.b, {"message_flow|" + actor_id + "|backlog_enqueued|",
                              "message_flow|" + actor_id + "|location_committed|"});
    assert_correlated_transfer_markers (
      {&_nodes.a, &_nodes.b}, actor_id,
      {"handoff_backlog", "backlog_enqueued", "location_committed"});
    assert_evidence_sequence (_nodes.b, {"message_flow|" + actor_id + "|backlog_enqueued|",
                                         "message_flow|" + actor_id + "|location_committed|"});
    assert_evidence_order (_nodes.b, actor_id, "handoff_packet", {"P1", "P2", "P3"});
}

} // namespace
