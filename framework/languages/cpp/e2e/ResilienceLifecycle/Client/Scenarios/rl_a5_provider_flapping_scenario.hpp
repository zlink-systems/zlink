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

inline void run_rl_a5_provider_flapping_probe (const client_options_t &options)
{
    const auto &phase = options.flap_phase;
    const auto &cycle = options.flap_cycle;
    auto consumer = zlink::http_client::client_t::create ()
                      .base_url (options.http_consumer_endpoint)
                      .timeout (std::chrono::milliseconds (10000))
                      .build ();

    if (phase == "down") {
        for (int index = 0; index < 4; ++index) {
            const auto marker = "rl-a5-down-" + cycle + "-" + std::to_string (index);
            const auto reply = consumer.post ("/profile/request")
                                 .body (profile_req_t{.value = "fast", .marker = marker})
                                 .submit<profile_res_t> ().result ().value ().body;
            ensure (reply.provider_rid == "api-a",
                    "RL-A5 down window did not converge to api-a");
        }

        const auto evidence = fetch_evidence (options.http_a_endpoint);
        ensure (evidence_contains (evidence, "ProfileReq", "rl-a5-down-" + cycle + "-0"),
                "RL-A5 down window did not record provider A evidence");
        std::cout << "scenario RL-A5 down passed\n";
        return;
    }

    if (phase == "up") {
        for (int index = 0; index < 24; ++index) {
            const auto marker = "rl-a5-up-" + cycle + "-" + std::to_string (index);
            const auto reply = consumer.post ("/profile/request")
                                 .body (profile_req_t{.value = "fast", .marker = marker})
                                 .submit<profile_res_t> ().result ().value ().body;
            ensure (reply.value == "profile:fast",
                    "RL-A5 up-window request returned an invalid value");
        }

        const auto prefix = "rl-a5-up-" + cycle + "-";
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (15);
        while (std::chrono::steady_clock::now () < deadline) {
            const auto evidence = fetch_evidence (options.http_b_endpoint);
            for (const auto &entry : evidence.entries) {
                if (entry.marker == "ProfileReq" && entry.value.rfind (prefix, 0) == 0) {
                    std::cout << "scenario RL-A5 up passed\n";
                    return;
                }
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }

        throw std::runtime_error ("RL-A5 up window did not record provider B evidence");
    }

    throw std::runtime_error ("RL-A5 requires flapPhase to be down or up");
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
