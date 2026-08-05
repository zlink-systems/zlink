/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* TA-A4: the client owns the requests, failure classification, and evidence assertions. */
inline void run_ta_a4_scenario (zlink::http_client::client_t &actor,
                                zlink::http_client::client_t &caller,
                                zlink::http_client::client_t &session_gateway,
                                const std::string &session_stream)
{
    ensure_ready (actor, caller, "TA-A4", "ta-a4");
    bound_actor_session_t session (session_stream, "TA-A4-bind", "ta-a4");
    session.close ();
    wait_session_evidence (session_gateway, "TA-A4-disconnect", "ta-a4", "disconnect");
    assert_call (caller, "TA-A4-disconnected-send", "ta-a4", "a4-send", "sent", true);
    assert_call (caller, "TA-A4-disconnected-request", "ta-a4", "a4-request", "reply:a4-request",
                 false);
    assert_call (caller, "TA-A4-destroy", "ta-a4", "destroy", "reply:destroy", false);
    require_location (caller, "TA-A4-location-missing", "ta-a4", "missing");
    assert_failure (caller, "TA-A4-destroyed", "ta-a4", "actor_route_not_found", false);

    const auto evidence = actor.get ("/evidence").submit<std::vector<e2e::actor_evidence_t>> ().result ().value ().body;
    require_evidence (evidence, "TA-A4-disconnected-send", "send");
}

} // namespace
