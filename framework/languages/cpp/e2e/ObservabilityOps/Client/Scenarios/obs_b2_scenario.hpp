/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "../Support/scenario_context.hpp"

namespace zlink::framework::e2e::observability_ops::client::scenarios
{
inline void run_obs_b2_scenario (const verification_input_t &input)
{
    const auto transfer = read_json (input, "transferEvidence");
    const auto transfers = metrics_named (transfer, "zlink.actor.transfers");
    const auto durations = metrics_named (transfer, "zlink.actor.transfer.duration");
    const auto pending =
      metrics_named (transfer, "zlink.actor.transfer.pending_requests.count");
    require (metric_total (transfer, "zlink.actor.transfers") == 1,
             "OBS-B2 actor transfer counter is not exactly one");
    require (transfers.size () == 1 && durations.size () == 1
               && durations.front ().at ("kind") == "histogram"
               && durations.front ().at ("value").get<double> () > 0,
             "OBS-B2 transfer duration does not cover the completed transfer");
    require (pending.size () == 1 && pending.front ().at ("kind") == "histogram",
             "OBS-B2 pending request count was not recorded once before moving");
}
} // namespace zlink::framework::e2e::observability_ops::client::scenarios
