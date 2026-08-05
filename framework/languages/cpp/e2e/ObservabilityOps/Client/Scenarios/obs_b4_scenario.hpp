/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "../Support/scenario_context.hpp"

namespace zlink::framework::e2e::observability_ops::client::scenarios
{
inline void run_obs_b4_scenario (const verification_input_t &input)
{
    const auto metrics = read_json (input, "offNodeEvidence").at ("metrics");
    constexpr std::size_t observedMetricUpperBound = 0;
    require (metrics.size () <= observedMetricUpperBound,
             "OBS-B4 reader-less node accumulated metric samples");
    const auto trafficEvidence = read_lines (input, "trafficEvidence");
    require (has_line (trafficEvidence, "scenario flow trigger passed"),
             "OBS-B4 messaging did not complete with metrics disabled");
}
} // namespace zlink::framework::e2e::observability_ops::client::scenarios
