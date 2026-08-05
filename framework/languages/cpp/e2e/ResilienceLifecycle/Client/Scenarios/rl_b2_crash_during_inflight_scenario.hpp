/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/resilience_request_support.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline bool file_contains (const std::string &path, const std::string &text)
{
    std::ifstream file (path);
    if (!file) {
        return false;
    }
    std::string line;
    while (std::getline (file, line)) {
        if (line.find (text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

inline void wait_topology_weight (zlink::http_client::client_t &topology,
                                  const std::string &routing_id,
                                  std::uint32_t weight)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30);
    while (std::chrono::steady_clock::now () < deadline) {
        auto result = topology.post ("/topology/wait")
                        .body (nlohmann::json{{"routingId", routing_id},
                                              {"state", "Ready"},
                                              {"expectedCount", 1},
                                              {"role", "router"},
                                              {"timeoutMilliseconds", 1000}}
                                 .dump (),
                               "application/json")
                        .submit<std::vector<topology_entry_result_t>> ()
                        .result ();
        if (result) {
            for (const auto &entry : result.value ().body) {
                if (entry.routing_id == routing_id && entry.state == "Ready"
                    && entry.weight == weight) {
                    return;
                }
            }
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("topology did not publish " + routing_id + " with expected weight");
}

inline void run_inflight_crash_scenario (const client_options_t &options)
{
    auto consumer = zlink::http_client::client_t::create ()
                      .base_url (options.http_consumer_endpoint)
                      .timeout (std::chrono::milliseconds (10000))
                      .build ();
    auto provider_a = zlink::http_client::client_t::create ()
                        .base_url (options.http_a_endpoint)
                        .timeout (std::chrono::milliseconds (10000))
                        .build ();
    auto provider_b = zlink::http_client::client_t::create ()
                        .base_url (options.http_b_endpoint)
                        .timeout (std::chrono::milliseconds (10000))
                        .build ();
    auto topology = zlink::http_client::client_t::create ()
                      .base_url (options.http_consumer_endpoint)
                      .timeout (std::chrono::milliseconds (35000))
                      .build ();

    provider_a.post ("/admin/drain").submit_raw ().result ().value ();
    provider_a.post ("/admin/weight/wait")
      .body (nlohmann::json{{"expected", 0}}.dump (), "application/json")
      .submit_raw ()
      .result ()
      .value ();

    const std::string marker = "rl-b2-slow";
    auto pending = std::async (std::launch::async, [marker, &options] {
        try {
            auto slow_consumer = zlink::http_client::client_t::create ()
                                   .base_url (options.http_consumer_endpoint)
                                   .timeout (std::chrono::milliseconds (10000))
                                   .build ();
            auto response = slow_consumer.post ("/profile/request/manual-b")
                              .body (profile_req_t{.value = "slow", .marker = marker})
                              .timeout (std::chrono::milliseconds (5000))
                              .submit<profile_res_t> ()
                              .result ();
            return response.has_value ();
        }
        catch (const std::exception &) {
            return false;
        }
    });

    const auto start_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    const auto start_marker = "profile-start|rid=api-b|marker=" + marker;
    const auto &provider_b_evidence = options.api_b_evidence_file;
    while (std::chrono::steady_clock::now () < start_deadline) {
        if (file_contains (provider_b_evidence, start_marker)) {
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    ensure (file_contains (provider_b_evidence, start_marker),
            "RL-B2 in-flight request did not start on provider B");
    touch_file (options.ready_file);
    wait_for_file (options.continue_file);

    topology.post ("/topology/wait")
      .body (nlohmann::json{{"routingId", "api-b"},
                            {"state", "Ready"},
                            {"expectedCount", 0},
                            {"timeoutMilliseconds", 30000}}
               .dump (),
             "application/json")
      .submit<std::vector<topology_entry_result_t>> ()
      .result ()
      .value ();

    ensure (!pending.get (),
            "RL-B2 in-flight request unexpectedly completed after provider crash");

    auto provider_a_restore = zlink::http_client::client_t::create ()
                                .base_url (options.http_a_endpoint)
                                .timeout (std::chrono::milliseconds (10000))
                                .build ();
    provider_a_restore.post ("/admin/restore").submit_raw ().result ().value ();
    provider_a_restore.post ("/admin/weight/wait")
      .body (nlohmann::json{{"expected", 100}}.dump (), "application/json")
      .submit_raw ()
      .result ()
      .value ();
    wait_topology_weight (topology, "api-a", 100);

    auto follow_up_consumer = zlink::http_client::client_t::create ()
                                .base_url (options.http_consumer_endpoint)
                                .timeout (std::chrono::milliseconds (10000))
                                .build ();
    const auto follow_up_raw = follow_up_consumer.post ("/profile/request")
                                 .body (profile_req_t{.value = "fast",
                                                      .marker = "rl-b2-after-crash"})
                                 .submit_raw ()
                                 .result ()
                                 .value ();
    ensure (follow_up_raw.status < 400,
            "RL-B2 surviving provider HTTP request failed with status "
              + std::to_string (follow_up_raw.status) + ": " + follow_up_raw.body);
    const auto follow_up = nlohmann::json::parse (follow_up_raw.body).get<profile_res_t> ();
    ensure (follow_up.provider_rid == "api-a", "RL-B2 surviving provider traffic failed");

    touch_file (options.drained_file);
    wait_for_file (options.restore_file);

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

    auto restored_consumer = zlink::http_client::client_t::create ()
                               .base_url (options.http_consumer_endpoint)
                               .timeout (std::chrono::milliseconds (10000))
                               .build ();
    for (int index = 0; index < 32; ++index) {
        const auto reply = restored_consumer.post ("/profile/request")
                             .body (profile_req_t{.value = "fast",
                                                  .marker = "rl-b2-restored-"
                                                            + std::to_string (index)})
                             .submit<profile_res_t> ().result ().value ().body;
        ensure (reply.value == "profile:fast",
                "RL-B2 restored request returned an invalid value");
    }

    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (15);
    while (std::chrono::steady_clock::now () < deadline) {
        const auto evidence = fetch_evidence (options.http_b_endpoint);
        for (const auto &entry : evidence.entries) {
            if (entry.marker == "ProfileReq" && entry.value.rfind ("rl-b2-restored-", 0) == 0) {
                touch_file (options.second_ready_file);
                wait_for_file (options.second_continue_file);

                topology.post ("/topology/wait")
                  .body (nlohmann::json{{"routingId", "api-b"},
                                        {"state", "Ready"},
                                        {"expectedCount", 0},
                                        {"timeoutMilliseconds", 30000}}
                           .dump (),
                         "application/json")
                  .submit<std::vector<topology_entry_result_t>> ()
                  .result ()
                  .value ();

                auto second_crash_consumer = zlink::http_client::client_t::create ()
                                               .base_url (
                                                 options.http_consumer_endpoint)
                                               .timeout (
                                                 std::chrono::milliseconds (10000))
                                               .build ();
                const auto second_crash_raw =
                  second_crash_consumer.post ("/profile/request")
                    .body (profile_req_t{.value = "fast",
                                         .marker = "rl-b2-second-crash-alternate"})
                    .submit_raw ()
                    .result ()
                    .value ();
                ensure (second_crash_raw.status < 400,
                        "RL-B2 second-crash alternate request failed with status "
                          + std::to_string (second_crash_raw.status) + ": "
                          + second_crash_raw.body);
                const auto second_crash =
                  nlohmann::json::parse (second_crash_raw.body).get<profile_res_t> ();
                ensure (second_crash.provider_rid == "api-a",
                        "RL-B2 second-crash request did not complete through provider A");
                ensure (second_crash.value == "profile:fast",
                        "RL-B2 second-crash request returned an invalid value");
                std::cout << "scenario RL-B2 second-crash alternate passed\n";
                std::cout << "scenario RL-B2 passed\n";
                return;
            }
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }

    throw std::runtime_error ("RL-B2 restored traffic did not reach provider B");
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
