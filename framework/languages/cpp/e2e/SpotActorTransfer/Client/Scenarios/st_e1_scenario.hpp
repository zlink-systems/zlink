/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-E1: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_e1_scenario ()
{
    const auto actor_id = "actor-bound-session-" + unique_suffix ();
    const auto spot_id = "spot-bound-session-" + unique_suffix ();
    create_spot (_nodes.b, spot_id);
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 91);
    const auto source_ref = get_actor_ref (_nodes.a, actor_id);
    bound_session_t bound (_nodes.a_stream_endpoint, "ST-E1", source_ref);
    auto before_push = bound.expect_push ("before-transfer");
    bound_push (_nodes.a, actor_id, {"ST-E1", "before-transfer"});
    before_push.get ();

    const auto join = join_actor (_nodes.a, actor_id, {"ST-E1", spot_id});
    require (join.accepted, "ST-E1 join was rejected.");
    auto pushed = bound.expect_push ("after-remote-transfer");
    const auto push_reply = bound_push (_nodes.b, actor_id, {"ST-E1", "after-remote-transfer"});
    const auto notify = pushed.get ();
    require (push_reply.node_rid == "actor-b",
             "ST-E1 bound push reply expected actor-b, got " + push_reply.node_rid);
    require (notify.node_rid == "actor-b",
             "ST-E1 bound push notify expected actor-b, got " + notify.node_rid);
    require (notify.state_version == 91,
             "ST-E1 bound push state expected 91, got " + std::to_string (notify.state_version));
}

} // namespace
