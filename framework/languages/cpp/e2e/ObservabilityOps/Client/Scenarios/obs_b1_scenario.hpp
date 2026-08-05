/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "../Support/scenario_context.hpp"

namespace zlink::framework::e2e::observability_ops::client::scenarios
{
inline void run_obs_b1_scenario (const verification_input_t &input)
{
    const auto body = read_json (input, "sessionEvidence");
    const auto opened = metrics_named (body, "zlink.stream.connections.opened");
    const auto closed = metrics_named (body, "zlink.stream.connections.closed");
    const auto active = metrics_named (body, "zlink.stream.connections.active");
    require (opened.size () >= 2 && !closed.empty () && active.size () >= 2,
             "OBS-B1 session connection instruments are incomplete");
    double total = 0;
    for (const auto &metric : active) {
        total += metric.at ("value").get<double> ();
    }
    require (total == 0, "OBS-B1 active connection count did not return to zero");
}
} // namespace zlink::framework::e2e::observability_ops::client::scenarios
