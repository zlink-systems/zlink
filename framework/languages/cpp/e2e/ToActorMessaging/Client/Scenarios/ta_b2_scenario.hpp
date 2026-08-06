/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* TA-B2: the client owns the requests, failure classification, and evidence assertions. */
inline void run_ta_b2_scenario (zlink::http_client::client_t &actor_a,
                                zlink::http_client::client_t &actor_b,
                                zlink::http_client::client_t &caller)
{
    ensure_ready (actor_a, caller, "TA-B2-owner-a", "ta-b2-stale");
    const auto evidence_a_before = actor_a.get ("/evidence")
                                     .submit<std::vector<e2e::actor_evidence_t>> ()
                                     .result ().value ().body;
    const auto evidence_b_before = actor_b.get ("/evidence")
                                     .submit<std::vector<e2e::actor_evidence_t>> ()
                                     .result ().value ().body;
    auto has_creation = [] (const auto &evidence) {
        return std::any_of (evidence.begin (), evidence.end (), [] (const auto &entry) {
            return entry.actor_id == "ta-b2-stale" && entry.kind == "create";
        });
    };
    auto *owner = has_creation (evidence_a_before) ? &actor_a : &actor_b;
    capture_ref (caller, "TA-B2-capture-ref", "ta-b2-stale");
    assert_call (caller, "TA-B2-destroy", "ta-b2-stale", "destroy", "reply:destroy", false);
    require_location (caller, "TA-B2-old-owner-removed", "ta-b2-stale", "missing");

    // Recreate through the same public session gateway. Placement chooses the
    // owner; the contract only requires a new incarnation on the same Mesh,
    // not a caller-selected node.
    recreate_ready (*owner, caller, "TA-B2-owner-recreated", "ta-b2-stale");
    assert_captured_failure (caller, "TA-B2-stale-location", "ta-b2-stale",
                             "actor_location_stale");
    assert_call (caller, "TA-B2-current-ref", "ta-b2-stale", "current", "reply:current", false);

    const auto evidence = actor_evidence (actor_a, actor_b);
    require_no_evidence (evidence, "TA-B2-stale-location");
    require_evidence (evidence, "TA-B2-current-ref", "request");
}

} // namespace
