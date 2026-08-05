/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "../Support/scenario_context.hpp"

namespace zlink::framework::e2e::observability_ops::client::scenarios
{
inline void run_obs_c4_scenario (const verification_input_t &input)
{
    const auto session = read_json (input, "sessionEvidence");
    require (has_drain_state (session, "force_stopping"),
             "OBS-C4 Session did not enter force_stopping");
    require (has_line (read_lines (input, "connectorLog"), "closeReason=server_drain"),
             "OBS-C4 connector did not expose server_drain");
}
} // namespace zlink::framework::e2e::observability_ops::client::scenarios
