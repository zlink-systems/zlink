/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <chrono>
#include <iostream>
#include <string>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline void run_rl_a2_provider_endpoint_remap_scenario (const client_options_t &options)
{
    bool remapped = false;
    for (int index = 0; index < 40; ++index) {
        const auto marker = "rl-a2-rescheduled-" + std::to_string (index);
        const auto reply = post_consumer_profile (options, "fast", marker, "/profile/request",
                                                 std::chrono::seconds (10));
        if (reply.provider_rid == "api-b") {
            remapped = true;
            break;
        }
    }
    ensure (remapped, "RL-A2 did not route traffic to remapped provider endpoint");

    touch_file (options.ready_file);
    wait_for_file (options.continue_file);

    bool restored = false;
    for (int index = 0; index < 40; ++index) {
        const auto marker = "rl-a2-original-restored-" + std::to_string (index);
        const auto reply = post_consumer_profile (options, "fast", marker, "/profile/request",
                                                 std::chrono::seconds (10));
        if (reply.provider_rid == "api-b") {
            restored = true;
            break;
        }
    }
    ensure (restored, "RL-A2 did not route traffic to restored provider endpoint");
    std::cout << "scenario RL-A2 client passed\n";
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
