/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-D2: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_d2_scenario ()
{
    const auto actor_id = "actor-stale-release-" + unique_suffix ();
    const auto spot_id = "spot-stale-release-" + unique_suffix ();
    create_spot (_nodes.b, spot_id);
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 81);

    const auto join = join_actor (_nodes.a, actor_id, {"ST-D2", spot_id});
    require (join.accepted, "ST-D2 join was rejected.");
    const auto before = get_actor_ref (_nodes.b, actor_id);
    require (before.node_rid == "actor-b",
             "ST-D2 target ref expected actor-b, got " + before.node_rid);

    (void) probe_actor (_nodes.a, actor_id, {"ST-D2", "before-delayed-cleanup"});
    wait_evidence (_nodes.a, {"message_flow|" + actor_id + "|source_cleanup|"});
    const auto after = get_actor_ref (_nodes.b, actor_id);
    require (after.node_rid == "actor-b",
             "ST-D2 target ref changed after delayed cleanup: " + after.node_rid);
    require (after.generation == before.generation,
             "ST-D2 generation changed after delayed cleanup.");
    const auto probe = probe_actor (_nodes.a, actor_id, {"ST-D2", "after-delayed-cleanup"});
    require (probe.node_rid == "actor-b", "ST-D2 probe expected actor-b, got " + probe.node_rid);
}

} // namespace
