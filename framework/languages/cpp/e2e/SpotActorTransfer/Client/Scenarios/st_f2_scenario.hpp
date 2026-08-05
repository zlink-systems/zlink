/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-F2: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_f2_scenario ()
{
    const auto actor_id = "actor-inflight-overtake-" + unique_suffix ();
    const auto spot_id = "spot-inflight-overtake-" + unique_suffix ();
    create_spot (_nodes.b, spot_id, "delay-joined");
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 102);
    const auto old_ref = get_actor_ref (_nodes.a, actor_id);

    auto join_task = std::async (
      std::launch::async, [&] { return join_actor (_nodes.a, actor_id, {"ST-F2", spot_id}); });
    wait_evidence (_nodes.b, {"ST-F2|" + actor_id + "|joined_wait|" + spot_id});
    for (const auto *marker : {"B1", "B2"}) {
        send_ref (_nodes.a, actor_id, old_ref, {"ST-F2", marker});
    }
    wait_evidence (_nodes.a, {"message_flow|" + actor_id + "|handoff_backlog|"});
    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    release_joined_gate (_nodes.b, spot_id);
    wait_evidence (_nodes.b, {"message_flow|" + actor_id + "|backlog_enqueued|",
                              "message_flow|" + actor_id + "|location_committed|"});
    const auto target_ref =
      e2e::actor_ref_snapshot_res_t{actor_id, "actor-b", old_ref.generation};
    send_ref (_nodes.b, actor_id, target_ref, {"ST-F2", "D1"});
    require (join_task.get ().accepted, "ST-F2 transfer was rejected.");
    assert_correlated_transfer_markers (
      {&_nodes.a, &_nodes.b}, actor_id,
      {"handoff_backlog", "backlog_enqueued", "location_committed"});
    assert_evidence_sequence (_nodes.b, {"message_flow|" + actor_id + "|backlog_enqueued|",
                                         "message_flow|" + actor_id + "|location_committed|",
                                         "ST-F2|" + actor_id + "|handoff_packet|D1"});
    assert_evidence_order (_nodes.b, actor_id, "handoff_packet", {"B1", "B2", "D1"});
}

} // namespace
