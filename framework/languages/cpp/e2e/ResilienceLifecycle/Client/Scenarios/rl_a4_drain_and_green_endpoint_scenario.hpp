/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_options.hpp"
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

inline void wait_provider_weight (zlink::http_client::client_t &provider, int expected)
{
    provider.post ("/admin/weight/wait")
      .body (nlohmann::json{{"expected", expected}}.dump (), "application/json")
      .submit_raw ()
      .result ()
      .value ();
}

inline void wait_provider_health_down (zlink::http_client::client_t &provider,
                                       const std::string &message)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now () < deadline) {
        auto response = provider.get ("/health").submit_raw ().result ();
        if (!response || response.value ().status != 200) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error (message);
}

inline void wait_location_ready_count (zlink::http_client::client_t &topology,
                                       const std::string &routing_id,
                                       int expected_count)
{
    topology.post ("/topology/wait")
      .body (nlohmann::json{{"routingId", routing_id},
                            {"state", "Ready"},
                            {"expectedCount", expected_count},
                            {"timeoutMilliseconds", 30000}}
               .dump (),
             "application/json")
      .submit<std::vector<topology_entry_result_t>> ()
      .result ()
      .value ();
}

inline void wait_evidence_prefix (const std::string &base_url,
                                  const std::string &marker,
                                  const std::string &prefix)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (15);
    while (std::chrono::steady_clock::now () < deadline) {
        const auto evidence = fetch_evidence (base_url);
        for (const auto &entry : evidence.entries) {
            if (entry.marker == marker && entry.value.rfind (prefix, 0) == 0) {
                return;
            }
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("timed out waiting for evidence prefix " + marker + "=" + prefix);
}

inline void run_rl_a4_drain_and_green_endpoint_scenario (const client_options_t &options)
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
    auto green_provider = zlink::http_client::client_t::create ()
                            .base_url (options.http_b_green_endpoint)
                            .timeout (std::chrono::milliseconds (10000))
                            .build ();

    provider_b.post ("/admin/drain").submit_raw ().result ().value ();
    wait_provider_weight (provider_b, 0);
    touch_file (options.ready_file);
    wait_for_file (options.continue_file);

    for (int index = 0; index < 12; ++index) {
        const auto marker = "rl-a4-rolling-" + std::to_string (index);
        const auto reply = consumer.post ("/profile/request")
                             .body (profile_req_t{.value = "fast", .marker = marker})
                             .submit<profile_res_t> ().result ().value ().body;
        ensure (reply.value == "profile:fast",
                "RL-A4 rolling request returned an unexpected value");
        ensure (reply.provider_rid == "api-a" || reply.provider_rid == "api-b",
                "RL-A4 rolling request used an unexpected provider");
    }

    for (int index = 0; index < 32; ++index) {
        const auto marker = "rl-a4-green-" + std::to_string (index);
        const auto reply = consumer.post ("/profile/request")
                             .body (profile_req_t{.value = "fast", .marker = marker})
                             .submit<profile_res_t> ().result ().value ().body;
        ensure (reply.value == "profile:fast",
                "RL-A4 green request returned an unexpected value");
    }
    wait_evidence_prefix (options.http_b_green_endpoint, "ProfileReq", "rl-a4-green-");

    touch_file (options.drained_file);
    wait_for_file (options.restore_file);

    wait_location_ready_count (topology, "api-b", 1);
    for (int index = 0; index < 32; ++index) {
        const auto marker = "rl-a4-restored-" + std::to_string (index);
        const auto reply = consumer.post ("/profile/request")
                             .body (profile_req_t{.value = "fast", .marker = marker})
                             .submit<profile_res_t> ().result ().value ().body;
        ensure (reply.value == "profile:fast",
                "RL-A4 restored request returned an unexpected value");
    }
    wait_evidence_prefix (options.http_b_endpoint, "ProfileReq",
                          "rl-a4-restored-");

    std::cout << "scenario RL-A4 passed\n";
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
