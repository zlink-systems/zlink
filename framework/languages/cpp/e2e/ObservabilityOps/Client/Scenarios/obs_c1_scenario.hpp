/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "../Support/scenario_context.hpp"

namespace zlink::framework::e2e::observability_ops::client::scenarios
{
inline void run_obs_c1_scenario (const verification_input_t &input)
{
    const auto during = read_json (input, "duringEvidence");
    const auto final = read_json (input, "finalEvidence");
    require (has_drain_state (during, "draining") && !during.at ("ready").get<bool> (),
             "OBS-C1 did not expose draining and readiness=false");
    require (has_drain_state (final, "drained"), "OBS-C1 did not reach drained");
    require (read_json (input, "rejectedCreate").value ("state", "") == "rejected",
             "OBS-C1 create while draining was not explicitly rejected");
}
} // namespace zlink::framework::e2e::observability_ops::client::scenarios
