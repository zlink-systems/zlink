/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/store_failure_contracts.hpp"

#include <zlink/http_client.hpp>

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::e2e::store_failure::client
{

inline void ensure (bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

template <typename TRequest, typename TReply>
inline TReply post_json (const std::string &base_url,
                         const std::string &path,
                         const TRequest &request,
                         std::chrono::milliseconds timeout = std::chrono::seconds (10))
{
    auto client =
      zlink::http_client::client_t::create ().base_url (base_url).timeout (timeout).build ();
    return client.post (path).body (request).template submit<TReply> ().result ().value ().body;
}

template <typename TReply>
inline TReply get_json (const std::string &base_url,
                        const std::string &path,
                        std::chrono::milliseconds timeout = std::chrono::seconds (10))
{
    auto client =
      zlink::http_client::client_t::create ().base_url (base_url).timeout (timeout).build ();
    return client.get (path).template submit<TReply> ().result ().value ().body;
}

inline void post_empty (const std::string &base_url, const std::string &path)
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (base_url)
                    .timeout (std::chrono::seconds (3))
                    .build ();
    auto response = client.post (path).submit_raw ().result ();
    ensure (response && response.value ().status < 400, "HTTP POST failed: " + base_url + path);
}

inline bool try_get_health (const std::string &base_url)
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (base_url)
                    .timeout (std::chrono::milliseconds (500))
                    .build ();
    try {
        auto response = client.get ("/health").submit_raw ().result ();
        return response && response.value ().status == 200;
    }
    catch (...) {
        return false;
    }
}

inline void wait_down (const std::string &base_url)
{
    for (int i = 0; i < 80; ++i) {
        if (!try_get_health (base_url)) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("node did not go down: " + base_url);
}

inline void wait_ready (const std::string &base_url)
{
    for (int i = 0; i < 120; ++i) {
        if (try_get_health (base_url)) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("node did not become ready: " + base_url);
}

inline void docker_action (const std::string &container, const std::string &action)
{
    ensure (!container.empty (), "Redis container name is required");
    const auto command = "docker " + action + " " + container + " >/dev/null";
    const auto code = std::system (command.c_str ());
    ensure (code == 0, "docker " + action + " failed for " + container);
}

inline void stop_store (const std::string &container) { docker_action (container, "stop -t 0"); }

inline void restart_store (const std::string &container)
{
    docker_action (container, "restart");
    const auto command = "docker exec " + container + " redis-cli ping >/dev/null 2>&1";
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (std::system (command.c_str ()) == 0) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error ("Redis did not become ready after docker restart");
}

inline runtime_status_res_t get_status (const std::string &base_url)
{
    return get_json<runtime_status_res_t> (base_url, "/query/status");
}

inline std::vector<peer_row_res_t> try_get_peers (const std::string &base_url)
{
    try {
        return get_json<std::vector<peer_row_res_t>> (base_url, "/query/peers",
                                                      std::chrono::seconds (3));
    }
    catch (...) {
        return {};
    }
}

template <typename Accept>
inline runtime_status_res_t wait_status (const std::string &base_url,
                                         Accept accept,
                                         std::chrono::milliseconds timeout,
                                         const std::string &failure)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    runtime_status_res_t last;
    while (std::chrono::steady_clock::now () < deadline) {
        last = get_status (base_url);
        if (accept (last)) {
            return last;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (150));
    }
    throw std::runtime_error (failure + " last healthy=" + (last.store_healthy ? "true" : "false")
                              + " lease="
                              + (last.owner_lease_healthy ? "true" : "false")
                              + " error=" + last.last_error);
}

template <typename Accept>
inline std::vector<peer_row_res_t> wait_peers (const std::string &base_url,
                                               Accept accept,
                                               std::chrono::milliseconds timeout,
                                               const std::string &failure)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    std::vector<peer_row_res_t> last;
    while (std::chrono::steady_clock::now () < deadline) {
        last = try_get_peers (base_url);
        if (accept (last)) {
            return last;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (150));
    }
    throw std::runtime_error (failure);
}

inline bool has_rid (const std::vector<peer_row_res_t> &peers, const std::string &rid)
{
    for (const auto &peer : peers) {
        if (peer.rid == rid) {
            return true;
        }
    }
    return false;
}

inline std::vector<socket_evidence_entry_t> connection_evidence (const std::string &consumer_url)
{
    return get_json<std::vector<socket_evidence_entry_t>> (consumer_url, "/query/connections");
}

inline std::size_t connection_event_count (const std::vector<socket_evidence_entry_t> &entries,
                                           const std::string &kind,
                                           const std::string &endpoint)
{
    (void) endpoint;
    return static_cast<std::size_t> (
      std::count_if (entries.begin (), entries.end (), [&] (const auto &entry) {
          return entry.kind == kind;
      }));
}

inline void wait_connected (const std::string &consumer_url, const std::string &endpoint)
{
    for (int attempt = 0; attempt < 80; ++attempt) {
        const auto entries = connection_evidence (consumer_url);
        if (connection_event_count (entries, "Connected", endpoint) > 0
            || connection_event_count (entries, "ConnectionReady", endpoint) > 0) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("connection evidence did not become ready: " + endpoint);
}

inline std::string unique_marker (const std::string &prefix)
{
    return prefix + "-"
           + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ());
}

inline profile_res_t request_profile (const std::string &consumer_url,
                                      const std::string &marker,
                                      std::chrono::milliseconds timeout = std::chrono::seconds (10))
{
    return post_json<profile_req_t, profile_res_t> (
      consumer_url, "/profile/request", {.value = "fast", .marker = marker}, timeout);
}

inline void set_store_delay (const std::string &redis_proxy_admin_url, int milliseconds)
{
    auto status = post_json<store_delay_req_t, operation_status_t> (
      redis_proxy_admin_url, "/delay", {.milliseconds = milliseconds}, std::chrono::seconds (3));
    ensure (status.status == "ok", "Redis latency proxy returned unexpected status");
}

inline std::vector<profile_res_t> drive_requests (const std::string &consumer_url,
                                                  const std::string &marker_prefix,
                                                  std::chrono::milliseconds window,
                                                  const std::string &scenario)
{
    std::vector<profile_res_t> replies;
    const auto deadline = std::chrono::steady_clock::now () + window;
    int index = 0;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto marker = unique_marker (marker_prefix + "-" + std::to_string (index++));
        auto reply = request_profile (consumer_url, marker);
        ensure (reply.value == "profile:fast", scenario + " request returned unexpected value");
        replies.push_back (std::move (reply));
        std::this_thread::sleep_for (std::chrono::milliseconds (150));
    }
    ensure (!replies.empty (), scenario + " request window produced no traffic");
    return replies;
}

} // namespace zlink::framework::e2e::store_failure::client
