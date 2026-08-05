/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-E2: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_e2_scenario ()
{
    const auto actor_id = "actor-bound-session-rebind-" + unique_suffix ();
    const auto spot_id = "spot-bound-session-rebind-" + unique_suffix ();
    create_spot (_nodes.b, spot_id);
    create_actor (_nodes.a, actor_id, e2e::actor_type_fail_transfer_out, 92);
    const auto source_ref = get_actor_ref (_nodes.a, actor_id);
    bound_session_t old_session (_nodes.a_stream_endpoint, "ST-E2", source_ref);
    auto before_push = old_session.expect_push ("before-failed-transfer");
    bound_push (_nodes.a, actor_id, {"ST-E2", "before-failed-transfer"});
    before_push.get ();

    const auto join = join_actor (_nodes.a, actor_id, {"ST-E2", spot_id});
    require (!join.accepted, "ST-E2 failed transfer was accepted.");

    auto source_push = old_session.expect_push ("after-failed-transfer");
    const auto push_reply = bound_push (_nodes.b, actor_id, {"ST-E2", "after-failed-transfer"});
    const auto notify = source_push.get ();
    require (push_reply.node_rid == "actor-a",
             "ST-E2 follow-up push did not execute on the source actor.");
    require (notify.node_rid == "actor-a" && notify.marker == "after-failed-transfer",
             "ST-E2 source bound session did not receive the follow-up notify.");

    const auto target_evidence = get_evidence (_nodes.b);
    require_no_contains (target_evidence, "ST-E2|" + actor_id + "|bound_push|after-failed-transfer",
                         "ST-E2 target processed bound push after failed transfer");
    require_no_contains (target_evidence, "transfer|" + actor_id + "|joined|" + spot_id,
                         "ST-E2 target actor joined after failed transfer");
}

} // namespace
