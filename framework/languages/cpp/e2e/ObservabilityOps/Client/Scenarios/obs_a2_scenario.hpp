/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "../Support/scenario_context.hpp"

namespace zlink::framework::e2e::observability_ops::client::scenarios
{
inline void run_obs_a2_scenario (const verification_input_t &input)
{
    const auto lines = read_lines (input, "sessionLog");
    require_shared_flow (
      lines,
      {"phase=received outcome=succeeded surface=stream_session kind=request label=cpp-obs-session packet=ObsUnknownReq",
       "dispatch error", "outcome=failed"},
      "OBS-A2 success and dispatch error lines do not share one flow");
}
} // namespace zlink::framework::e2e::observability_ops::client::scenarios
