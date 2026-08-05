/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline void run_rl_a1_provider_restart_scenario (const client_options_t &options)
{
    for (int index = 0; index < 12; ++index) {
        const auto marker = "rl-a1-down-" + std::to_string (index);
        const auto reply = post_consumer_profile (options, "fast", marker);
        ensure (reply.provider_rid == "api-a",
                "RL-A1 request during api-b restart did not use surviving provider");
    }
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    bool evidence_recorded = false;
    do {
        for (const auto &entry : fetch_evidence (options.http_a_endpoint).entries) {
            if (entry.marker == "ProfileReq" && entry.value.rfind ("rl-a1-down-", 0) == 0) {
                evidence_recorded = true;
                break;
            }
        }
        if (evidence_recorded) {
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    } while (std::chrono::steady_clock::now () < deadline);
    ensure (evidence_recorded, "RL-A1 surviving provider evidence missing");

    touch_file (options.ready_file);
    wait_for_file (options.continue_file);

    bool restored = false;
    for (int index = 0; index < 300; ++index) {
        const auto marker = "rl-a1-restored-" + std::to_string (index);
        try {
            const auto reply = post_consumer_profile (options, "fast", marker, "/profile/request",
                                                     std::chrono::seconds (10));
            if (reply.provider_rid == "api-b") {
                restored = true;
                break;
            }
        }
        catch (const std::exception &) {
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    ensure (restored, "RL-A1 did not route traffic to restarted provider");
    std::cout << "scenario RL-A1 client passed\n";
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
