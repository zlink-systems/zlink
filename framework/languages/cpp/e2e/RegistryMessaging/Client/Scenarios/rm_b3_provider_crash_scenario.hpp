/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <future>
#include <iostream>
#include <nlohmann/json.hpp>

namespace zlink::framework::e2e::registry_messaging::client
{

inline void run_rm_b3_provider_crash_scenario (const client_options_t &options)
{
    constexpr auto inflight_value = "rm-b3-inflight";
    auto inflight = std::async (
      std::launch::async, [&options] {
          return post_json<profile_req_t, request_failure_res_t> (
            options.store_consumer_url, "/profile/scale-in-transition",
            profile_req_t{.value = inflight_value}, std::chrono::seconds (20));
      });

    wait_evidence_contains (options.http_a_endpoint, "ProfileReqStarted", inflight_value,
                            std::chrono::seconds (10));
    touch_file (options.ready_file);
    wait_for_file (options.continue_file, options.control_wait);

    const auto result = inflight.get ();
    ensure (result.failed, "RM-B3 in-flight request unexpectedly succeeded");
    ensure (result.error_type == "Unavailable"
              || result.error_type == "DeadlineExceeded"
              || result.error_type == "TimeoutException",
            "RM-B3 in-flight request returned an unexpected public error: "
              + result.error_type);
    ensure (!evidence_contains (fetch_evidence (options.http_b_endpoint),
                                "ProfileReq", inflight_value),
            "RM-B3 replayed the in-flight request on provider B");

    auto status_client = zlink::http_client::client_t::create ()
                           .base_url (options.store_consumer_url)
                           .timeout (std::chrono::seconds (2))
                           .build ();
    const auto removal_deadline = std::chrono::steady_clock::now ()
                                  + std::chrono::seconds (30);
    bool provider_a_removed = false;
    while (std::chrono::steady_clock::now () < removal_deadline) {
        try {
            const auto status = status_client
                                  .get ("/client-server/status?channel=registry.messaging.api")
                                  .submit<nlohmann::json> ()
                                  .result ();
            if (status) {
                const auto &body = status.value ().body;
                provider_a_removed = body.value ("readyServerCount", 0) == 1
                                     && body.value ("selectable", false);
                if (provider_a_removed)
                    break;
            }
        }
        catch (const zlink::framework::framework_exception_t &) {
            // The consumer may briefly restart its HTTP accept loop while the
            // failed admitted server is removed. The public status poll remains
            // bounded and does not create a second application request.
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    ensure (provider_a_removed,
            "RM-B3 provider A did not leave the public ready target set");

    for (int index = 0; index < 20; ++index) {
        const auto reply = post_json<profile_req_t, profile_res_t> (
          options.store_consumer_url, "/profile/request",
          profile_req_t{.value = "rm-b3-after-" + std::to_string (index)},
          std::chrono::seconds (5));
        ensure (reply.provider_rid == "api-b",
                "RM-B3 routed a post-crash request to provider A");
    }
    ensure (evidence_value_prefix_count (fetch_evidence (options.http_b_endpoint),
                                          "ProfileReq", "rm-b3-after-") == 20,
            "RM-B3 provider B did not record every post-crash request exactly once");
    std::cout << "scenario RM-B3 passed\n";
}

} // namespace zlink::framework::e2e::registry_messaging::client
