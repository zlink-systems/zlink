/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <atomic>
#include <future>
#include <iostream>
#include <optional>
#include <set>
#include <vector>

namespace zlink::framework::e2e::registry_messaging::client
{

inline void run_rm_b2_scale_in_scenario (const client_options_t &options)
{
    const auto consumer_url = options.store_consumer_url;
    auto location_client = zlink::http_client::client_t::create ()
                             .base_url (consumer_url)
                             .timeout (std::chrono::seconds (5))
                             .build ();
    std::set<std::string> before;
    for (int index = 0; index < 80 && before.size () < 2; ++index) {
        auto reply = post_json<profile_req_t, profile_res_t> (
          consumer_url, "/profile/request",
          profile_req_t{.value = "scale-in-before-" + std::to_string (index)});
        before.insert (reply.provider_rid);
    }
    ensure (before.contains ("api-a") && before.contains ("api-b"),
            "RM-B2 did not start with both providers");

    struct transition_result_t
    {
        bool success = false;
        std::optional<zlink::framework::framework_error_kind_t> error_kind;
    };
    std::atomic<bool> start_requests{false};
    std::vector<std::future<transition_result_t>> transition_requests;
    transition_requests.reserve (16);
    for (int index = 0; index < 16; ++index) {
        transition_requests.push_back (
          std::async (std::launch::async, [&consumer_url, &start_requests] {
              while (!start_requests.load (std::memory_order_acquire)) {
                  std::this_thread::yield ();
              }
                  const auto reply = post_json<profile_req_t, request_failure_res_t> (
                    consumer_url, "/profile/scale-in-transition", profile_req_t{.value = "slow"},
                    std::chrono::seconds (60));
              if (!reply.failed) {
                  return transition_result_t{.success = true};
              }
                  if (reply.error_type == "DeadlineExceeded"
                      || reply.error_type == "Unavailable"
                      || reply.error_type == "ShuttingDown"
                      || reply.error_type == "TimeoutException") {
                  std::cerr << "RM-B2 transition public error type="
                            << reply.error_type << '\n';
                  return transition_result_t{.error_kind =
                    reply.error_type == "DeadlineExceeded"
                      || reply.error_type == "TimeoutException"
                      ? zlink::framework::framework_error_kind_t::deadline_exceeded
                    : reply.error_type == "ShuttingDown"
                      ? zlink::framework::framework_error_kind_t::shutting_down
                      : zlink::framework::framework_error_kind_t::unavailable};
              }
              throw std::runtime_error (
                "RM-B2 transition returned unexpected public error type: "
                + reply.error_type);
          }));
    }
    start_requests.store (true, std::memory_order_release);
    touch_file (options.ready_file);
    wait_for_file (options.continue_file, options.control_wait);

    int transition_successes = 0;
    int transition_errors = 0;
    for (auto &pending : transition_requests) {
        const auto completed = pending.get ();
        if (completed.success) {
            ++transition_successes;
            continue;
        }
        ensure (completed.error_kind == zlink::framework::framework_error_kind_t::deadline_exceeded
                  || completed.error_kind
                       == zlink::framework::framework_error_kind_t::shutting_down
                  || completed.error_kind
                       == zlink::framework::framework_error_kind_t::unavailable,
                "RM-B2 transition request returned an unexpected public error");
        ++transition_errors;
    }
    std::cout << "RM-B2 transition completed successes=" << transition_successes
              << " public_errors=" << transition_errors << '\n';

    bool api_b_removed = false;
    for (int attempt = 0; attempt < 150 && !api_b_removed; ++attempt) {
        const auto peers_result = location_client.get ("/locations/peers")
                                     .submit<nlohmann::json> ()
                                     .result ();
        if (!peers_result) {
            std::this_thread::sleep_for (std::chrono::milliseconds (200));
            continue;
        }
        const auto &peers = peers_result.value ().body;
        api_b_removed = true;
        for (const auto &entry : peers) {
            if (entry.value ("mesh_name", "") == api_channel
                && entry.value ("role", "") == "router"
                && entry.value ("node_rid", "") == "api-b") {
                api_b_removed = false;
                break;
            }
        }
        if (!api_b_removed) {
            std::this_thread::sleep_for (std::chrono::milliseconds (200));
        }
    }
    ensure (api_b_removed, "RM-B2 timed out waiting for api-b location row removal");

    for (int index = 0; index < 20; ++index) {
        auto reply = post_json<profile_req_t, profile_res_t> (
          consumer_url, "/profile/request",
          profile_req_t{.value = "scale-in-after-" + std::to_string (index)},
          std::chrono::seconds (5));
        ensure (reply.provider_rid == "api-a",
                "RM-B2 routed to removed provider after scale-in");
    }
    std::cout << "scenario RM-B2 passed\n";
}

} // namespace zlink::framework::e2e::registry_messaging::client
