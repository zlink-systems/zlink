/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* TA-A1: the client owns the requests, failure classification, and evidence assertions. */
inline void run_ta_a1_scenario (zlink::http_client::client_t &actor,
                                zlink::http_client::client_t &caller,
                                zlink::http_client::client_t &session_gateway,
                                const std::string &session_stream)
{
    ensure_ready (actor, caller, "TA-A1", "ta-a1");
    bound_actor_session_t session (session_stream, "TA-A1-bind", "ta-a1");
    auto before = session.expect_push ("TA-A1-before");
    push_actor (actor, "TA-A1-before", "ta-a1", "BeforeNotify");
    require (before.get ().value == "BeforeNotify", "TA-A1 BeforeNotify mismatch");
    assert_call (caller, "TA-A1-send", "ta-a1", "a1-send", "sent", true);
    assert_call (caller, "TA-A1-request", "ta-a1", "a1-request", "reply:a1-request", false);
    auto after = session.expect_push ("TA-A1-after");
    push_actor (actor, "TA-A1-after", "ta-a1", "AfterNotify");
    require (after.get ().value == "AfterNotify", "TA-A1 AfterNotify mismatch");
    const auto bindings = session_evidence (session_gateway);
    require (count_session_evidence (bindings, "ta-a1", "bind") == 1,
             "TA-A1 no-bind calls changed the existing session binding");
    require_session_evidence (bindings, "ta-a1", "bind", "session-a");

    const auto evidence = actor.get ("/evidence").submit<std::vector<e2e::actor_evidence_t>> ().result ().value ().body;
    require_evidence (evidence, "TA-A1-send", "send");
    require_evidence (evidence, "TA-A1-request", "request");
}

} // namespace
