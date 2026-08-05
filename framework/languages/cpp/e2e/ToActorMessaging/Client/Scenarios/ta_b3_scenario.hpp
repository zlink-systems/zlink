/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* TA-B3: the client owns the requests, failure classification, and evidence assertions. */
inline void run_ta_b3_scenario (zlink::http_client::client_t &actor,
                                zlink::http_client::client_t &caller,
                                zlink::http_client::client_t &route_control)
{
    ensure_ready (actor, caller, "TA-B3", "ta-b3");
    capture_ref (caller, "TA-B3-capture-ref", "ta-b3");
    assert_captured_call (caller, "TA-B3-before-disconnect", "ta-b3", "before",
                          "reply:before");
    control_route (route_control, "disconnect");
    assert_captured_failure (caller, "TA-B3-route-not-connected", "ta-b3",
                             "route_not_connected");
    control_route (route_control, "reconnect");
    assert_captured_call (caller, "TA-B3-route-restored", "ta-b3", "restored",
                          "reply:restored");

    const auto evidence = actor.get ("/evidence").submit<std::vector<e2e::actor_evidence_t>> ().result ().value ().body;
    require_no_evidence (evidence, "TA-B3-route-not-connected");
    require_evidence (evidence, "TA-B3-route-restored", "request");
}

} // namespace
