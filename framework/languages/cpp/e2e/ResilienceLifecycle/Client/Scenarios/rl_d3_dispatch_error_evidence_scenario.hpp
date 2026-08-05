/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline void run_rl_d3_dispatch_error_evidence_scenario (const client_options_t &options)
{
    const auto missing = post_consumer_missing (options, "rl-d3-missing");
    ensure (missing.failed, "RL-D3 missing request unexpectedly succeeded");
    ensure (missing.error_type == "HandlerNotFound",
            "RL-D3 missing handler error type mismatch: " + missing.error_type);
    post_consumer_command (options, "rl-d3-missing-send", "/profile/command/missing");
    const auto normal = post_consumer_profile (options, "rl-d3-normal");
    ensure (normal.value == "profile:rl-d3-normal",
            "RL-D3 normal request after missing packet failed");

    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    const auto evidence_a = fetch_evidence (options.http_a_endpoint);
    const auto evidence_b = fetch_evidence (options.http_b_endpoint);
    bool reply_error_recorded = false;
    bool drop_recorded = false;
    for (const auto &snapshot : {evidence_a, evidence_b}) {
        for (const auto &entry : snapshot.entries) {
            if (entry.marker != "DispatchError") {
                continue;
            }
            if (entry.value == "handler_missing:reply_error") {
                reply_error_recorded = true;
            }
            if (entry.value == "handler_missing:drop") {
                drop_recorded = true;
            }
        }
    }
    ensure (reply_error_recorded, "RL-D3 missing request dispatch evidence was not recorded");
    ensure (drop_recorded, "RL-D3 missing send dispatch evidence was not recorded");
    std::cout << "scenario RL-D3 client passed\n";
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
