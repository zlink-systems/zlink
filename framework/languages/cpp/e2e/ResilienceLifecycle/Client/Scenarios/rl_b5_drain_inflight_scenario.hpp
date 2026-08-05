/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"
#include "../Support/lifecycle_api_result.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline bool b5_file_contains (const std::string &path, const std::string &text)
{
    std::ifstream input (path);
    std::string line;
    while (std::getline (input, line)) {
        if (line.find (text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

inline std::string wait_b5_slow_provider (const client_options_t &options,
                                         const std::string &marker)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
    const auto &evidence_a = options.api_a_evidence_file;
    const auto &evidence_b = options.api_b_evidence_file;
    while (std::chrono::steady_clock::now () < deadline) {
        if (b5_file_contains (evidence_a, "profile-start|rid=api-a|marker=" + marker)) {
            return "api-a";
        }
        if (b5_file_contains (evidence_b, "profile-start|rid=api-b|marker=" + marker)) {
            return "api-b";
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error ("RL-B5 slow request did not start on any provider");
}

inline int b5_count_evidence_prefix (const evidence_snapshot_t &snapshot,
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

inline void b5_wait_provider_weight (zlink::http_client::client_t &provider, int expected)
{
    provider.post ("/admin/weight/wait")
      .body (nlohmann::json{{"expected", expected}}.dump (), "application/json")
      .submit_raw ()
      .result ()
      .value ();
}

inline void b5_wait_provider_evidence_prefix (const std::string &base_url,
                                              const std::string &prefix)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (15);
    while (std::chrono::steady_clock::now () < deadline) {
        try {
            const auto evidence = fetch_evidence (base_url);
            if (b5_count_evidence_prefix (evidence, "ProfileReq", prefix) > 0) {
                return;
            }
        }
        catch (const std::exception &) {
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("RL-B5 did not record provider evidence " + prefix);
}

inline void b5_wait_consumer_topology_weight (const client_options_t &options,
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

inline void run_rl_b5_drain_inflight_scenario (const client_options_t &options)
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

    try {
        provider_a.post ("/admin/restore").submit_raw ().result ().value ();
        provider_b.post ("/admin/restore").submit_raw ().result ().value ();
        b5_wait_provider_weight (provider_a, 100);
        b5_wait_provider_weight (provider_b, 100);

        const auto slow_marker = "rl-b5-slow";
        auto slow_consumer = zlink::http_client::client_t::create ()
                               .base_url (options.http_consumer_endpoint)
                               .timeout (std::chrono::milliseconds (10000))
                               .build ();
        auto slow = std::async (std::launch::async,
                                [slow_consumer = std::move (slow_consumer),
                                 slow_marker] () mutable {
            return slow_consumer.post ("/profile/request")
              .body (profile_req_t{.value = "slow", .marker = slow_marker})
              .submit<profile_res_t> ().result ().value ().body;
        });

        const auto slow_provider = wait_b5_slow_provider (options, slow_marker);
        auto &drained_provider = slow_provider == "api-a" ? provider_a : provider_b;
        const auto &drained_url = slow_provider == "api-a" ? options.http_a_endpoint
                                                            : options.http_b_endpoint;
        const auto healthy_provider = slow_provider == "api-a" ? "api-b" : "api-a";
        const auto before_drain = fetch_evidence (drained_url);

        try {
            drained_provider.post ("/admin/drain").submit_raw ().result ().value ();
            b5_wait_provider_weight (drained_provider, 0);
            b5_wait_consumer_topology_weight (options, slow_provider, 0);
        }
        catch (const std::exception &ex) {
            throw std::runtime_error (std::string ("RL-B5 failed during drain readiness: ") + ex.what ());
        }

        try {
            for (int index = 0; index < 12; ++index) {
                const auto marker = "rl-b5-drained-" + std::to_string (index);
                const auto reply = consumer.post ("/profile/request")
                                     .body (profile_req_t{.value = "fast", .marker = marker})
                                     .submit<profile_res_t> ().result ().value ().body;
                ensure (reply.provider_rid == healthy_provider,
                        "RL-B5 drain did not block new requests to drained provider");
            }
        }
        catch (const std::exception &ex) {
            throw std::runtime_error (std::string ("RL-B5 failed during drained traffic: ") + ex.what ());
        }

        try {
            const auto slow_reply = slow.get ();
            ensure (slow_reply.provider_rid == slow_provider && slow_reply.marker == slow_marker,
                    "RL-B5 in-flight slow reply did not complete on drained provider");
        }
        catch (const std::exception &ex) {
            throw std::runtime_error (std::string ("RL-B5 failed during slow completion: ") + ex.what ());
        }

        try {
            const auto after_drain = fetch_evidence (drained_url);
            const auto new_drained =
              b5_count_evidence_prefix (after_drain, "ProfileReq", "rl-b5-drained-")
              - b5_count_evidence_prefix (before_drain, "ProfileReq", "rl-b5-drained-");
            ensure (new_drained == 0, "RL-B5 drained provider accepted new requests after drain");
            ensure (b5_count_evidence_prefix (after_drain, "ProfileReq", slow_marker) > 0,
                    "RL-B5 in-flight completion evidence missing");
        }
        catch (const std::exception &ex) {
            throw std::runtime_error (std::string ("RL-B5 failed during drain evidence: ") + ex.what ());
        }

        try {
            drained_provider.post ("/admin/restore").submit_raw ().result ().value ();
            b5_wait_provider_weight (drained_provider, 100);
            b5_wait_consumer_topology_weight (options, slow_provider, 100);
        }
        catch (const std::exception &ex) {
            throw std::runtime_error (std::string ("RL-B5 failed during restore readiness: ") + ex.what ());
        }

        try {
            for (int index = 0; index < 40; ++index) {
                const auto marker = "rl-b5-after-" + std::to_string (index);
                const auto reply = consumer.post ("/profile/request")
                                     .body (profile_req_t{.value = "fast", .marker = marker})
                                     .submit<profile_res_t> ().result ().value ().body;
                ensure (reply.value == "profile:fast",
                        "RL-B5 restored request returned an unexpected value");
            }
        }
        catch (const std::exception &ex) {
            throw std::runtime_error (std::string ("RL-B5 failed during restored traffic: ") + ex.what ());
        }

        try {
            b5_wait_provider_evidence_prefix (drained_url, "rl-b5-after-");
        }
        catch (const std::exception &ex) {
            throw std::runtime_error (std::string ("RL-B5 failed during restored evidence: ") + ex.what ());
        }
    }
    catch (const std::exception &ex) {
        throw std::runtime_error (ex.what ());
    }

    std::cout << "scenario RL-B5 passed\n";
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
