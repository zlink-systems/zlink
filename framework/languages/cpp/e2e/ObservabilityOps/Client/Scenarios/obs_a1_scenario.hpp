/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "../Support/scenario_context.hpp"

namespace zlink::framework::e2e::observability_ops::client::scenarios
{
inline void run_obs_a1_scenario (const verification_input_t &input)
{
    const auto session = read_lines (input, "sessionLog");
    const auto spot = read_lines (input, "spotLog");
    require_flow_sequence (
      {session, session, spot},
      {"phase=received outcome=succeeded surface=stream_session kind=request label=cpp-obs-session packet=ObsActionReq",
       "phase=sent outcome=succeeded surface=spot_route kind=request label=cpp-obs-session packet=ObsActionReq",
       "phase=received outcome=succeeded surface=spot_route kind=request label=cpp-obs-play-b packet=ObsActionReq"},
      "OBS-A1 flow did not preserve the STREAM to route to room order");
    require_shared_flow (
      session,
      {"phase=received outcome=succeeded surface=stream_session kind=request label=cpp-obs-session packet=ObsActionReq",
       "origin=application"},
      "OBS-A1 action flow has no application origin");
}
} // namespace zlink::framework::e2e::observability_ops::client::scenarios
