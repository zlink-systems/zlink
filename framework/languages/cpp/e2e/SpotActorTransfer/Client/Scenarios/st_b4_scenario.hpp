/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-B4: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_b4_scenario ()
{
    const auto actor_id = "actor-empty-state-" + unique_suffix ();
    const auto spot_id = "spot-empty-state-" + unique_suffix ();
    create_spot (_nodes.b, spot_id);
    create_actor (_nodes.a, actor_id, e2e::actor_type_empty_state, 41);

    const auto join = join_actor (_nodes.a, actor_id, {"ST-B4", spot_id});
    require (join.accepted, "ST-B4 join was rejected.");

    const auto probe = probe_actor (_nodes.a, actor_id, {"ST-B4", "after-empty-state-transfer"});
    require (probe.node_rid == "actor-b", "ST-B4 probe expected actor-b, got " + probe.node_rid);
    require (probe.state_version == 41,
             "ST-B4 loaded target state expected 41, got " + std::to_string (probe.state_version));

    wait_evidence (_nodes.a, {
                               "transfer|" + actor_id + "|transfer_out_empty|custom-adapter",
                               "transfer|" + actor_id + "|leave|41",
                             });
    wait_evidence (_nodes.b, {
                               "transfer|" + actor_id + "|transfer_in_empty|custom-adapter",
                               "transfer|" + actor_id + "|joined|" + spot_id + ":0",
                               "transfer|" + actor_id + "|domain_state_loaded|" + actor_id,
                               "ST-B4|" + actor_id + "|packet_handler|after-empty-state-transfer",
                             });
}

} // namespace
