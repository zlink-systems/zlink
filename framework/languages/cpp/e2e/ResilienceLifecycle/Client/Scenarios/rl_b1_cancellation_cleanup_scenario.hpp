/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline void run_rl_b1_cancellation_cleanup_scenario (const client_options_t &options)
{
    const auto slow = post_consumer_profile_raw (
      options, "slow", "slow", "/profile/request/timeout/100");
    const auto timeout = nlohmann::json::parse (slow.body).get<request_failure_res_t> ();
    ensure (timeout.failed && timeout.error_type == "TimeoutException",
            "RL-B1 slow request did not return TimeoutException: " + timeout.error_type);
    const auto after = post_consumer_profile (options, "rl-b1-after-timeout");
    ensure (after.value == "profile:rl-b1-after-timeout", "RL-B1 post-timeout request failed");
    std::this_thread::sleep_for (std::chrono::milliseconds (1100));
    const auto later = post_consumer_profile (options, "rl-b1-later");
    ensure (later.value == "profile:rl-b1-later", "RL-B1 later request failed");
    wait_provider_evidence_contains (options, "ProfileReq", "rl-b1-after-timeout",
                                     std::chrono::seconds (10));
    wait_provider_evidence_contains (options, "ProfileReq", "rl-b1-later",
                                     std::chrono::seconds (10));
    std::cout << "scenario RL-B1 client passed\n";
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
