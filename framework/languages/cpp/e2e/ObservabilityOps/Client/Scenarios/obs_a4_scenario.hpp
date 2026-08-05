/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "../Support/scenario_context.hpp"

namespace zlink::framework::e2e::observability_ops::client::scenarios
{
inline void run_obs_a4_scenario (const verification_input_t &input)
{
    const auto publisher = read_lines (input, "publisherLog");
    const auto subscriberLogs = read_line_groups (input, "subscriberLogs");
    require_fanout_flow (publisher, subscriberLogs,
                         "OBS-A4 one publish flow did not reach every subscriber");
    require (has_line (read_lines (input, "timerLog"), "origin=timer"),
             "OBS-A4 timer publish has no timer origin");
}
} // namespace zlink::framework::e2e::observability_ops::client::scenarios
