/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "../Support/scenario_context.hpp"

namespace zlink::framework::e2e::observability_ops::client::scenarios
{
inline void run_obs_c2_scenario (const verification_input_t &input)
{
    const auto body = read_json (input, "sourceEvidence");
    require (has_drain_state (body, "drained") && !has_drain_state (body, "force_stopping"),
             "OBS-C2 handoff did not finish through natural drain");
    require (!metrics_named (body, "zlink.actor.transfers").empty (),
             "OBS-C2 actor transfer instrument is missing");
    require (read_json (input, "postMovePing").value ("nodeRid", "") == "play-b",
             "OBS-C2 post-move request did not reach the target owner");
}
} // namespace zlink::framework::e2e::observability_ops::client::scenarios
