/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-F5: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_f5_scenario ()
{
    /* ST-F5 follows the common scenario: one relocation publishes a temporary
     * old-route follow, then the old route expires while the global Actor route
     * remains usable. ST-F4 covers delivery during the follow window; this
     * scenario keeps the cleanup and current-route assertions separate. */
    const auto setup = relocate_for_message_follow ("ST-F5", 105);
    send_ref (_nodes.a, setup.actor_id, setup.old_ref, {"ST-F5", "chain-to-final"});
    wait_evidence (_nodes.b,
                   {"ST-F5|" + setup.actor_id + "|handoff_packet|chain-to-final"});
    wait_evidence (_nodes.a,
                   {"message_flow|" + setup.actor_id
                    + "|message_follow_route_removed|"});

    const auto current = probe_actor (
      _nodes.a, setup.actor_id, {"ST-F5", "current-global-route-after-removal"});
    require (current.marker == "current-global-route-after-removal"
               && current.node_rid == "actor-b",
             "ST-F5 current global Actor route failed after Message Follow removal.");
    const auto stale = probe_ref (
      _nodes.a, setup.actor_id, setup.old_ref,
      {"ST-F5", "after-message-follow-removal"});
    require (!stale.succeeded && stale.error_kind == "Unavailable",
             "ST-F5 expected old route removal, got '" + stale.error_kind + "'.");
    require_no_contains (
      get_evidence (_nodes.b),
      "ST-F5|" + setup.actor_id + "|packet_handler|after-message-follow-removal",
      "ST-F5 old-route request after Message Follow removal reached the target handler.");
}

} // namespace
