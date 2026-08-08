/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-F4: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_f4_scenario ()
{
    const auto setup = relocate_for_message_follow ("ST-F4", 104);
    send_ref (_nodes.a, setup.actor_id, setup.old_ref, {"ST-F4", "G1"});
    wait_evidence (_nodes.a, {"message_flow|" + setup.actor_id + "|message_follow_relay|"});
    wait_evidence (_nodes.b, {"ST-F4|" + setup.actor_id + "|handoff_packet|G1"});

    wait_evidence (
      _nodes.a, {"message_flow|" + setup.actor_id + "|message_follow_route_removed|"});
    const auto expired = probe_ref (
      _nodes.a, setup.actor_id, setup.old_ref, {"ST-F4", "G2"});
    require (!expired.succeeded && expired.error_kind == "Unavailable",
             "ST-F4 expired old-route request did not return Unavailable: '"
               + expired.error_kind + "'.");
    require_no_contains (get_evidence (_nodes.b), "ST-F4|" + setup.actor_id + "|packet_handler|G2",
                         "ST-F4 expired old-route request reached the target handler.");
}

} // namespace
