/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "../Support/scenario_context.hpp"

namespace zlink::framework::e2e::observability_ops::client::scenarios
{
inline void run_obs_c5_scenario (const verification_input_t &input)
{
    const auto rolling = read_json (input, "rollingEvidence");
    const auto forced = read_json (input, "forcedEvidence");
    require (has_drain_state (rolling, "drained") && !has_drain_state (rolling, "force_stopping"),
             "OBS-C5 rolling drain did not terminate naturally");
    require (has_drain_state (forced, "force_stopping"),
             "OBS-C5 zero-target drain did not force stop");
    require (!metrics_named (forced, "zlink.drain.forced").empty (),
             "OBS-C5 forced drain counter is missing");
}
} // namespace zlink::framework::e2e::observability_ops::client::scenarios
