/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/resilience_request_support.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline void run_quick_resilience_scenario (const client_options_t &options)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (6);
    while (std::chrono::steady_clock::now () < deadline) {
        try {
            const auto reply = post_consumer_profile (options, "rl-quick");
            if (!reply.provider_rid.empty ()) {
                std::cout << "scenario quick passed\n";
                return;
            }
        }
        catch (const std::exception &) {
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (200));
    }
    throw std::runtime_error ("quick scenario did not recover within timeout");
}

inline void run_rl_a3_reconnect_storm_probe (const client_options_t &options)
{
    auto consumer = zlink::http_client::client_t::create ()
                      .base_url (options.http_consumer_endpoint)
                      .timeout (std::chrono::milliseconds (10000))
                      .build ();

    for (int index = 0; index < 24; ++index) {
        const auto marker = "rl-a3-" + std::to_string (index);
        const auto reply = consumer.post ("/profile/request/new-client")
                             .body (profile_req_t{.value = "fast", .marker = marker})
                             .submit<profile_res_t> ().result ().value ().body;
        ensure (reply.value == "profile:fast", "RL-A3 storm request returned an invalid value");
        ensure (reply.provider_rid == "api-a" || reply.provider_rid == "api-b",
                "RL-A3 storm request returned an unexpected provider");
    }

    wait_provider_evidence_contains (options, "ProfileReq", "rl-a3-0",
                                     std::chrono::seconds (15));
    std::cout << "scenario RL-A3 passed\n";
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
