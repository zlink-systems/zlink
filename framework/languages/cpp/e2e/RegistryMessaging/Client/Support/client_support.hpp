/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../../Shared/registry_messaging_contracts.hpp"

#include <zlink/http_client.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <string_view>

namespace zlink::framework::e2e::registry_messaging::client
{

struct client_options_t
{
    std::string scenario;
    std::string api_a2_endpoint;
    std::string http_a_endpoint;
    std::string http_b_endpoint;
    std::string http_a2_endpoint;
    std::string http_workflow_endpoint;
    std::string direct_consumer_url;
    std::string single_consumer_url;
    std::string store_consumer_url;
    std::string backpressure_consumer_url;
    std::string ready_file;
    std::string continue_file;
    std::chrono::milliseconds control_wait{60000};
};

inline client_options_t read_client_options (int argc, char **argv)
{
    std::string path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        constexpr std::string_view prefix = "--config=";
        if (argument.rfind (prefix, 0) != 0) {
            throw std::runtime_error ("unknown RegistryMessaging client option: " + argument);
        }
        path = argument.substr (prefix.size ());
    }
    if (path.empty ()) {
        throw std::runtime_error ("RegistryMessaging client requires --config=<path>");
    }
    std::ifstream input (path);
    if (!input) {
        throw std::runtime_error ("cannot open RegistryMessaging client config: " + path);
    }
    const auto section = nlohmann::json::parse (input).at ("e2e");
    const auto value = [&] (const char *key) {
        const auto found = section.find (key);
        return found == section.end () ? std::string{} : found->get<std::string> ();
    };
    return {.scenario = value ("scenario"),
            .api_a2_endpoint = value ("apiA2Endpoint"),
            .http_a_endpoint = value ("httpAEndpoint"),
            .http_b_endpoint = value ("httpBEndpoint"),
            .http_a2_endpoint = value ("httpA2Endpoint"),
            .http_workflow_endpoint = value ("httpWorkflowEndpoint"),
            .direct_consumer_url = value ("directConsumerUrl"),
            .single_consumer_url = value ("singleConsumerUrl"),
            .store_consumer_url = value ("storeConsumerUrl"),
            .backpressure_consumer_url = value ("backpressureConsumerUrl"),
            .ready_file = value ("readyFile"),
            .continue_file = value ("continueFile"),
            .control_wait = std::chrono::milliseconds (
              section.value ("controlWaitMs", 60000))};
}

inline void ensure (bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

inline void touch_file (const std::string &path)
{
    if (path.empty ()) {
        return;
    }
    std::ofstream file (path);
    file << "ready\n";
}

inline void wait_for_file (const std::string &path, std::chrono::milliseconds timeout)
{
    if (path.empty ()) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    do {
        if (std::filesystem::exists (path)) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    } while (std::chrono::steady_clock::now () < deadline);
    throw std::runtime_error ("timed out waiting for " + path);
}

inline evidence_snapshot_t fetch_evidence (const std::string &base_url)
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (base_url)
                    .timeout (std::chrono::milliseconds (1000))
                    .build ();
    return client.get ("/evidence").submit<evidence_snapshot_t> ().result ().value ().body;
}

inline bool evidence_contains (const evidence_snapshot_t &snapshot,
                               const std::string &marker,
                               const std::string &value)
{
    for (const auto &entry : snapshot.entries) {
        if (entry.marker == marker && entry.value == value) {
            return true;
        }
    }
    return false;
}

inline int evidence_value_prefix_count (const evidence_snapshot_t &snapshot,
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

template <typename TRequest, typename TReply>
inline TReply post_json (const std::string &base_url,
                         const std::string &path,
                         const TRequest &request,
                         std::chrono::milliseconds timeout = std::chrono::seconds (30))
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (base_url)
                    .timeout (timeout)
                    .build ();
    try {
        return client.post (path).body (request).template submit<TReply> ().result ().value ().body;
    }
    catch (const zlink::framework::framework_exception_t &error) {
        std::cerr << "registry-messaging http failure path=" << path
                  << " kind=" << static_cast<int> (error.kind ())
                  << " code=" << error.code ().value ()
                  << " message=" << error.what () << '\n';
        throw;
    }
}

inline void post_raw (const std::string &base_url,
                      const std::string &path,
                      std::chrono::milliseconds timeout = std::chrono::seconds (30))
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (base_url)
                    .timeout (timeout)
                    .build ();
    auto response = client.post (path).submit_raw ().result ();
    ensure (response && response.value ().status < 400, "HTTP POST failed: " + path);
}

inline bool any_provider_evidence_contains (const client_options_t &options,
                                            const std::string &marker,
                                            const std::string &value)
{
    return evidence_contains (fetch_evidence (options.http_a_endpoint), marker, value)
           || evidence_contains (fetch_evidence (options.http_b_endpoint), marker, value);
}

inline void wait_provider_evidence_contains (const client_options_t &options,
                                             const std::string &marker,
                                             const std::string &value,
                                             std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (any_provider_evidence_contains (options, marker, value)) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("timed out waiting for provider evidence " + marker + "=" + value);
}

inline evidence_snapshot_t wait_evidence_contains (const std::string &base_url,
                                                   const std::string &marker,
                                                   const std::string &value,
                                                   std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        auto snapshot = fetch_evidence (base_url);
        if (evidence_contains (snapshot, marker, value)) {
            return snapshot;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("timed out waiting for evidence " + marker + "=" + value);
}

} // namespace zlink::framework::e2e::registry_messaging::client
