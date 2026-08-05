/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "../Support/scenario_context.hpp"

namespace zlink::framework::e2e::observability_ops::client::scenarios
{
inline void run_obs_a3_scenario (const verification_input_t &input)
{
    const auto upstream = read_lines (input, "upstreamLog");
    const auto downstream = read_lines (input, "downstreamLog");
    require_same_flow (
      upstream,
      "phase=received outcome=succeeded surface=stream_session kind=request label=cpp-obs-session packet=ObsActionReq",
      downstream,
      "phase=received outcome=succeeded surface=spot_route kind=request label=cpp-obs-play-b packet=ObsActionReq",
      "OBS-A3 downstream node recreated or lost the upstream flow");
    for (const auto &line : read_optional_lines (input, "offNodeLog")) {
        require (line.find ("flow=") == std::string::npos,
                 "OBS-A3 tracing-off node emitted a flow line");
    }
}
} // namespace zlink::framework::e2e::observability_ops::client::scenarios
