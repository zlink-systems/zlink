/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* TA-A3: the client owns the requests, failure classification, and evidence assertions. */
inline void run_ta_a3_scenario (zlink::http_client::client_t &actor,
                                zlink::http_client::client_t &caller,
                                zlink::http_client::client_t &session_gateway,
                                const std::string &session_stream)
{
    ensure_ready (actor, caller, "TA-A3", "ta-a3");
    assert_call (caller, "TA-A3-before-bind-send", "ta-a3", "before-send", "sent", true);
    assert_call (caller, "TA-A3-before-bind-request", "ta-a3", "before-request",
                 "reply:before-request", false);
    require (count_session_evidence (session_evidence (session_gateway), "ta-a3", "bind") == 0,
             "TA-A3 no-bind calls created a session binding");
    bound_actor_session_t session (session_stream, "TA-A3-bind", "ta-a3");
    require_session_evidence (session_evidence (session_gateway), "ta-a3", "bind", "session-b");
    assert_call (caller, "TA-A3-after-bind-send", "ta-a3", "a3-send", "sent", true);
    assert_call (caller, "TA-A3-after-bind-request", "ta-a3", "a3-request", "reply:a3-request",
                 false);
    auto pushed = session.expect_push ("TA-A3-late-bind");
    push_actor (actor, "TA-A3-late-bind", "ta-a3", "LateBindNotify");
    require (pushed.get ().value == "LateBindNotify", "TA-A3 LateBindNotify mismatch");

    const auto evidence = actor.get ("/evidence").submit<std::vector<e2e::actor_evidence_t>> ().result ().value ().body;
    require_evidence (evidence, "TA-A3-after-bind-request", "request");
}

} // namespace
