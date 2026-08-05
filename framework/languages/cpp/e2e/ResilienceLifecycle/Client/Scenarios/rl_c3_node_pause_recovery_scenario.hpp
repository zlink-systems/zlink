/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"
#include "../Support/lifecycle_api_result.hpp"
#include "../Support/topology_entry_result.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline void wait_c3_provider_health_down (zlink::http_client::client_t &provider)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now () < deadline) {
        auto response = provider.get ("/health").submit_raw ().result ();
        if (!response || response.value ().status != 200) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("RL-C3 provider B did not stop");
}

inline void wait_c3_location_ready (zlink::http_client::client_t &topology)
{
    topology.post ("/topology/wait")
      .body (nlohmann::json{{"routingId", "api-b"},
                            {"state", "Ready"},
                            {"expectedCount", 1},
                            {"timeoutMilliseconds", 30000}}
               .dump (),
             "application/json")
      .submit<std::vector<topology_entry_result_t>> ()
      .result ()
      .value ();
}

inline bool c3_evidence_prefix_exists (const std::string &base_url,
                                       const std::string &prefix)
{
    const auto evidence = fetch_evidence (base_url);
    for (const auto &entry : evidence.entries) {
        if (entry.marker == "ProfileReq" && entry.value.rfind (prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

inline void wait_c3_provider_evidence_prefix (const std::string &base_url,
                                              const std::string &prefix)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (15);
    while (std::chrono::steady_clock::now () < deadline) {
        if (c3_evidence_prefix_exists (base_url, prefix)) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("RL-C3 did not record provider evidence " + prefix);
}

inline void run_rl_c3_node_pause_recovery_probe (const client_options_t &options)
{
    auto consumer = zlink::http_client::client_t::create ()
                      .base_url (options.http_consumer_endpoint)
                      .timeout (std::chrono::milliseconds (10000))
                      .build ();
    auto topology = zlink::http_client::client_t::create ()
                      .base_url (options.http_consumer_endpoint)
                      .timeout (std::chrono::milliseconds (35000))
                      .build ();
    auto provider_b = zlink::http_client::client_t::create ()
                        .base_url (options.http_b_endpoint)
                        .timeout (std::chrono::milliseconds (10000))
                        .build ();

    provider_b.post ("/shutdown").submit_raw ().result ().value ();
    wait_c3_provider_health_down (provider_b);

    const auto during = consumer.post ("/profile/request")
                          .body (profile_req_t{.value = "fast",
                                               .marker = "rl-c3-during-down"})
                          .submit<profile_res_t> ().result ().value ().body;
    ensure (during.provider_rid == "api-a",
            "RL-C3 did not use surviving provider during node down");
    wait_c3_provider_evidence_prefix (options.http_a_endpoint,
                                      "rl-c3-during-down");

    touch_file (options.ready_file);
    wait_for_file (options.continue_file);

    wait_c3_location_ready (topology);
    for (int index = 0; index < 40; ++index) {
        const auto marker = "rl-c3-recovered-" + std::to_string (index);
        const auto reply = consumer.post ("/profile/request")
                             .body (profile_req_t{.value = "fast", .marker = marker})
                             .submit<profile_res_t> ().result ().value ().body;
        ensure (reply.value == "profile:fast",
                "RL-C3 recovered request returned an unexpected value");
    }
    wait_c3_provider_evidence_prefix (options.http_b_endpoint,
                                      "rl-c3-recovered-");

    std::cout << "scenario RL-C3 passed\n";
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
