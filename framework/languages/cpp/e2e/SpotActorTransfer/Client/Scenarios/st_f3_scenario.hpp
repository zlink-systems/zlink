/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-F3: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_f3_scenario ()
{
    const auto actor_id = "actor-bound-order-" + unique_suffix ();
    const auto spot_id = "spot-bound-order-" + unique_suffix ();
    create_spot (_nodes.b, spot_id, "delay-joined");
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 103);
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
    require (join_task.get ().accepted, "ST-F3 transfer was rejected.");
    assert_evidence_sequence (_nodes.b, {"message_flow|" + actor_id + "|backlog_enqueued|",
                                         "message_flow|" + actor_id + "|location_committed|",
                                         "ST-F3|" + actor_id + "|handoff_packet|S3"});
    assert_evidence_order (_nodes.b, actor_id, "handoff_packet", {"S1", "S2", "S3", "S4"});
}

} // namespace
