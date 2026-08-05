/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* TA-B1: the client owns the requests, failure classification, and evidence assertions. */
inline void run_ta_b1_scenario (zlink::http_client::client_t &actor,
                                zlink::http_client::client_t &caller)
{
    ensure_ready (actor, caller, "TA-B1", "missing-actor");
    assert_call (caller, "TA-B1-destroy", "missing-actor", "destroy", "reply:destroy", false);
    require_location (caller, "TA-B1-location", "missing-actor", "missing");
    assert_call (caller, "TA-B1-send-submit", "missing-actor", "missing", "sent", true);
    assert_failure (caller, "TA-B1-missing-request", "missing-actor", "actor_route_not_found",
                    false);

    const auto evidence = actor.get ("/evidence").submit<std::vector<e2e::actor_evidence_t>> ().result ().value ().body;
    require_no_evidence (evidence, "TA-B1-send-submit");
    require_no_evidence (evidence, "TA-B1-missing-request");
}

} // namespace
