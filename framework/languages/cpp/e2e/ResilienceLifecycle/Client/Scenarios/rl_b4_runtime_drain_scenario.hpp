/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/resilience_request_support.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline int count_evidence_prefix (const evidence_snapshot_t &snapshot,
                                  const std::string &marker,
                                  const std::string &prefix)
{
    int count = 0;
    for (const auto &entry : snapshot.entries) {
        if (entry.marker == marker && entry.value.rfind (prefix, 0) == 0) {
            ++count;
        }
    }
    return count;
}

inline void wait_provider_evidence_prefix_on (const std::string &base_url,
                                              const std::string &prefix)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (15);
    while (std::chrono::steady_clock::now () < deadline) {
        try {
            const auto evidence = fetch_evidence (base_url);
            if (count_evidence_prefix (evidence, "ProfileReq", prefix) > 0) {
                return;
            }
        }
        catch (const std::exception &) {
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("timed out waiting for provider evidence " + prefix);
}

inline void wait_provider_weight_on (zlink::http_client::client_t &provider, int expected)
{
    auto result = provider.post ("/admin/weight/wait")
                    .body (nlohmann::json{{"expected", expected}}.dump (), "application/json")
                    .submit_raw ()
                    .result ();
    if (!result) {
        throw std::runtime_error (result.error () ? result.error ()->what ()
                                                  : "provider weight wait failed");
    }
    if (result.value ().status >= 400) {
        throw std::runtime_error ("provider weight wait failed with status "
                                  + std::to_string (result.value ().status) + ": "
                                  + result.value ().body);
    }
}

inline void wait_consumer_topology_weight (const client_options_t &options,
                                           const std::string &routing_id,
                                           std::uint32_t expected)
{
    auto consumer = zlink::http_client::client_t::create ()
                      .base_url (options.http_consumer_endpoint)
                      .timeout (std::chrono::milliseconds (3000))
                      .build ();
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30);
    int stable_observations = 0;
    while (std::chrono::steady_clock::now () < deadline) {
        try {
            auto entries = consumer.get ("/topology").submit<std::vector<topology_entry_result_t>> ().result ().value ().body;
            bool matched = false;
            for (const auto &entry : entries) {
                if (entry.routing_id == routing_id && entry.weight == expected) {
                    matched = true;
                    break;
                }
            }
            stable_observations = matched ? stable_observations + 1 : 0;
            if (stable_observations >= 3) {
                return;
            }
        }
        catch (const std::exception &) {
            stable_observations = 0;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("timed out waiting for topology weight " + routing_id + "="
                              + std::to_string (expected));
}

inline void post_provider_admin_on (zlink::http_client::client_t &provider,
                                    const std::string &path)
{
    auto result = provider.post (path).submit_raw ().result ();
    if (!result) {
        throw std::runtime_error (result.error () ? result.error ()->what ()
                                                  : "provider admin call failed");
    }
    if (result.value ().status >= 400) {
        throw std::runtime_error ("provider admin call failed with status "
                                  + std::to_string (result.value ().status) + " for " + path
                                  + ": " + result.value ().body);
    }
}

inline void run_rl_b4_runtime_drain_scenario (const client_options_t &options)
{
    auto consumer = zlink::http_client::client_t::create ()
                      .base_url (options.http_consumer_endpoint)
                      .timeout (std::chrono::milliseconds (10000))
                      .build ();
    auto provider_b = zlink::http_client::client_t::create ()
                        .base_url (options.http_b_endpoint)
                        .timeout (std::chrono::milliseconds (10000))
                        .build ();

    std::string phase = "initial evidence";
    try {
        const auto before_drain = fetch_evidence (options.http_b_endpoint);
        phase = "drain admin";
        post_provider_admin_on (provider_b, "/admin/drain");
        phase = "wait drain weight";
        wait_provider_weight_on (provider_b, 0);
        wait_consumer_topology_weight (options, "api-b", 0);

        phase = "drained traffic";
        for (int index = 0; index < 20; ++index) {
            const auto marker = "rl-b4-drained-" + std::to_string (index);
            const auto reply = consumer.post ("/profile/request")
                                 .body (profile_req_t{.value = "fast", .marker = marker})
                                 .submit<profile_res_t> ().result ().value ().body;
            ensure (reply.provider_rid == "api-a", "RL-B4 drained provider received request");
        }

        phase = "after-drain evidence";
        const auto after_drain = fetch_evidence (options.http_b_endpoint);
        const auto new_b =
          count_evidence_prefix (after_drain, "ProfileReq", "rl-b4-drained-")
          - count_evidence_prefix (before_drain, "ProfileReq", "rl-b4-drained-");
        ensure (new_b == 0, "RL-B4 api-b evidence changed after drain");
        phase = "surviving provider evidence";
        wait_provider_evidence_prefix_on (options.http_a_endpoint,
                                          "rl-b4-drained-");

        phase = "restore admin";
        post_provider_admin_on (provider_b, "/admin/restore");
        phase = "wait restore weight";
        wait_provider_weight_on (provider_b, 100);
        wait_consumer_topology_weight (options, "api-b", 100);

        phase = "restored traffic";
        const auto first_reply =
          consumer.post ("/profile/request")
            .body (profile_req_t{.value = "fast", .marker = "rl-b4-restored-first"})
            .submit<profile_res_t> ().result ().value ().body;
        ensure (first_reply.value == "profile:fast",
                "RL-B4 first restored request returned an unexpected value");
        bool observed_b = first_reply.provider_rid == "api-b";
        for (int index = 0; index < 20 && !observed_b; ++index) {
            const auto marker = "rl-b4-restored-" + std::to_string (index);
            const auto reply = consumer.post ("/profile/request")
                                 .body (profile_req_t{.value = "fast", .marker = marker})
                                 .submit<profile_res_t> ().result ().value ().body;
            ensure (reply.value == "profile:fast",
                    "RL-B4 restored request returned an unexpected value");
            observed_b = reply.provider_rid == "api-b";
        }
        ensure (observed_b, "RL-B4 did not route restored traffic to api-b");
        std::cout << "scenario RL-B4 passed\n";
    }
    catch (const std::exception &error) {
        throw std::runtime_error (std::string ("RL-B4 failed during ") + phase + ": "
                                  + error.what ());
    }
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
