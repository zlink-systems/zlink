/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline void run_rl_b3_graceful_shutdown_scenario (const client_options_t &options)
{
    const auto before = post_consumer_profile (options, "fast", "rl-b3-before-shutdown");
    ensure (before.provider_rid == "api-a" || before.provider_rid == "api-b",
            "RL-B3 pre-shutdown request failed");

    touch_file (options.ready_file);
    wait_for_file (options.continue_file);

    for (int index = 0; index < 20; ++index) {
        const auto reply = post_consumer_profile (
          options, "rl-b3-after-shutdown-" + std::to_string (index));
        ensure (reply.provider_rid == "api-a",
                "RL-B3 routed to stopped provider after graceful shutdown");
    }
    std::cout << "scenario RL-B3 client passed\n";
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
