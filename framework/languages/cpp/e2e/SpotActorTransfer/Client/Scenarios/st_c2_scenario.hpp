/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-C2: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_c2_scenario ()
{
    const auto actor_id = "actor-source-down-after-commit-" + unique_suffix ();
    const auto spot_id = "spot-source-down-after-commit-" + unique_suffix ();
    create_spot (_nodes.b, spot_id);
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 61);
    const auto source_ref = get_actor_ref (_nodes.a, actor_id);
    bound_session_t bound (_nodes.b_stream_endpoint, "ST-C2", source_ref);
    auto before_push = bound.expect_push ("bound-before-transfer");
    const auto before_reply = bound.bound_push ({"ST-C2", "bound-before-transfer"});
    require (before_reply.node_rid == "actor-a",
             "ST-C2 pre-transfer bound push expected actor-a, got " + before_reply.node_rid);
    before_push.get ();

    const auto join = join_actor (_nodes.a, actor_id, {"ST-C2", spot_id});
    require (join.accepted, "ST-C2 join was rejected.");
    wait_evidence (_nodes.b, {
                               "transfer|" + actor_id + "|transfer_in|61",
                               "transfer|" + actor_id + "|joined|" + spot_id + ":61",
                             });
    const auto before_shutdown = get_actor_ref (_nodes.b, actor_id);
    require (before_shutdown.node_rid == "actor-b",
             "ST-C2 target ref expected actor-b, got " + before_shutdown.node_rid);

    shutdown_node (_nodes.a);
    std::this_thread::sleep_for (std::chrono::seconds (2));

    const auto after_shutdown = get_actor_ref (_nodes.b, actor_id);
    require (after_shutdown.node_rid == "actor-b",
             "ST-C2 target ref changed after source shutdown: " + after_shutdown.node_rid);
    require (after_shutdown.generation == before_shutdown.generation,
             "ST-C2 target generation changed after source shutdown.");

    const auto probe = probe_actor (_nodes.b, actor_id, {"ST-C2", "after-source-down"});
    require (probe.node_rid == "actor-b", "ST-C2 probe expected actor-b, got " + probe.node_rid);
    require (probe.spot_id == spot_id,
             "ST-C2 probe did not reach target spot after source shutdown.");
    auto pushed = bound.expect_push ("bound-after-source-down");
    const auto push_reply = bound_push (_nodes.b, actor_id, {"ST-C2", "bound-after-source-down"});
    const auto notify = pushed.get ();
    require (push_reply.node_rid == "actor-b",
             "ST-C2 bound push reply expected actor-b, got " + push_reply.node_rid);
    require (notify.node_rid == "actor-b",
             "ST-C2 bound push notify expected actor-b, got " + notify.node_rid);
    require (notify.marker == "bound-after-source-down",
             "ST-C2 bound push notify marker mismatch.");
    wait_evidence (_nodes.b, {
                               "ST-C2|" + actor_id + "|packet_handler|after-source-down",
                               "ST-C2|" + actor_id + "|bound_push|bound-after-source-down",
                             });
}

} // namespace
