/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

#include <iostream>

namespace
{

/* TA-B3: the client owns the requests, failure classification, and evidence assertions. */
inline void run_ta_b3_scenario (zlink::http_client::client_t &actor,
                                zlink::http_client::client_t &actor_b,
                                zlink::http_client::client_t &caller,
                                zlink::http_client::client_t &route_control)
{
    (void) route_control;
    ensure_ready (actor, caller, "TA-B3", "ta-b3");
    capture_ref (caller, "TA-B3-capture-ref", "ta-b3");
    assert_captured_call (caller, "TA-B3-before-disconnect", "ta-b3", "before",
                          "reply:before");
    std::cout << "scenario-control TA-B3 disconnect-route" << std::endl;
    const auto disconnect_deadline = std::chrono::steady_clock::now ()
                                     + std::chrono::seconds (30);
    while (std::chrono::steady_clock::now () < disconnect_deadline) {
        const auto status = caller.post ("/route/status")
                              .body (nlohmann::json::object ())
                              .submit_raw ()
                              .result ();
        if (status && status.value ().body == "not_ready")
            break;
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    require (std::chrono::steady_clock::now () < disconnect_deadline,
             "TA-B3 route did not become unavailable before the request");
    assert_captured_failure (caller, "TA-B3-route-not-connected", "ta-b3",
                             "unavailable");
    std::cout << "scenario-control TA-B3 restore-route" << std::endl;
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::seconds (30);
    while (std::chrono::steady_clock::now () < deadline) {
        const auto status = caller.post ("/route/status")
                              .body (nlohmann::json::object ())
                              .submit_raw ()
                              .result ();
        if (status && status.value ().body == "ready")
            break;
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    require (std::chrono::steady_clock::now () < deadline,
             "TA-B3 route did not recover before the restored request");
    assert_captured_call (caller, "TA-B3-route-restored", "ta-b3", "restored",
                          "reply:restored");

    const auto evidence = actor_evidence (actor, actor_b);
    require_no_evidence (evidence, "TA-B3-route-not-connected");
    require_evidence (evidence, "TA-B3-route-restored", "request");
}

} // namespace
