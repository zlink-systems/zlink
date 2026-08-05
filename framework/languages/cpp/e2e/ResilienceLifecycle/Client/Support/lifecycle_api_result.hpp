/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/resilience_lifecycle_contracts.hpp"
#include "scenario_assert.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <string>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline evidence_snapshot_t fetch_evidence (const std::string &base_url)
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (base_url)
                    .timeout (std::chrono::milliseconds (1000))
                    .build ();
    return client.get ("/evidence").submit<evidence_snapshot_t> ().result ().value ().body;
}

inline profile_res_t post_consumer_profile (const client_options_t &options,
                                            const std::string &value,
                                            const std::string &marker = {},
                                            const std::string &path = "/profile/request",
                                            std::chrono::milliseconds timeout =
                                              std::chrono::milliseconds (3000))
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (options.http_consumer_endpoint)
                    .timeout (timeout)
                    .build ();
    auto request = profile_req_t{.value = value, .marker = marker.empty () ? value : marker};
    return client.post (path).body (request).submit<profile_res_t> ().result ().value ().body;
}

inline zlink::http_client::raw_http_response_t
post_consumer_profile_raw (const client_options_t &options,
                           const std::string &value,
                           const std::string &marker = {},
                           const std::string &path = "/profile/request",
                           std::chrono::milliseconds timeout = std::chrono::milliseconds (3000))
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (options.http_consumer_endpoint)
                    .timeout (timeout)
                    .build ();
    auto request = profile_req_t{.value = value, .marker = marker.empty () ? value : marker};
    auto result = client.post (path).body (request).submit_raw ().result ();
    ensure (result.has_value (),
            std::string ("consumer HTTP request failed: ")
              + (result.error () ? result.error ()->what () : "unknown error"));
    return result.value ();
}

inline request_failure_res_t post_consumer_missing (const client_options_t &options,
                                                    const std::string &value)
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (options.http_consumer_endpoint)
                    .timeout (std::chrono::milliseconds (3000))
                    .build ();
    return client.post ("/profile/request/missing")
      .body (profile_req_t{.value = value, .marker = value})
      .submit<request_failure_res_t> ().result ().value ().body;
}

inline operation_status_t post_consumer_command (const client_options_t &options,
                                                 const std::string &command_id,
                                                 const std::string &path = "/profile/command")
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (options.http_consumer_endpoint)
                    .timeout (std::chrono::milliseconds (3000))
                    .build ();
    return client.post (path)
      .body (profile_msg_t{.command_id = command_id, .marker = command_id})
      .submit<operation_status_t> ().result ().value ().body;
}

inline void post_provider_admin (const std::string &base_url, const std::string &path)
{
    auto http = zlink::http_client::client_t::create ()
                  .base_url (base_url)
                  .timeout (std::chrono::milliseconds (1000))
                  .build ();
    auto response = http.post (path).submit_raw ().result ();
    ensure (response && response.value ().status < 400,
            "provider admin call failed: " + base_url + path);
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

} // namespace zlink::framework::e2e::resilience_lifecycle::client
