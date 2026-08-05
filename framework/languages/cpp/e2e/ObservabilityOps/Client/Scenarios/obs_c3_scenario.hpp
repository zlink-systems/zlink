/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "../Support/scenario_context.hpp"

namespace zlink::framework::e2e::observability_ops::client::scenarios
{
inline void run_obs_c3_scenario (const verification_input_t &input)
{
    const auto normal = read_json (input, "normalAction");
    require (normal.value ("value", -1) >= 0,
             "OBS-C3 normal request did not complete before drain");
    require (read_json (input, "rejectedCreate").value ("state", "") == "rejected",
             "OBS-C3 accepted a new Spot turn after drain admission closed");
    const auto drained = read_json (input, "drainedEvidence");
    require (has_drain_state (drained, "draining") && has_drain_state (drained, "drained")
               && !has_drain_state (drained, "force_stopping"),
             "OBS-C3 fixed drain did not reach one graceful terminal state");
    require (read_json (input, "closeAfterDrain").value ("state", "") == "not_closed",
             "OBS-C3 left a local user Spot open after the terminal barrier");
    require (read_json (input, "staleAction").value ("accepted", true) == false,
             "OBS-C3 stale handle triggered hidden remote creation");
    const auto state = read_json (input, "recreate").value ("state", "");
    require (state == "created", "OBS-C3 explicit local GetOrCreate did not create a new Spot");
}
} // namespace zlink::framework::e2e::observability_ops::client::scenarios
