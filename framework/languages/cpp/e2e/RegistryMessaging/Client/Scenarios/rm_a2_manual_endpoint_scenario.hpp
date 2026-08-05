/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <future>
#include <iostream>
#include <map>
#include <set>
#include <thread>

namespace zlink::framework::e2e::registry_messaging::client
{

inline void run_rm_a2_manual_endpoint_scenario (const client_options_t &options)
{
    const auto provider_a_url = options.http_a_endpoint;
    const auto consumer_url = options.single_consumer_url;
    auto reply = post_json<profile_req_t, profile_res_t> (
      consumer_url, "/profile/request", profile_req_t{.value = "rm-a2"});
    ensure (reply.value == "profile:rm-a2", "RM-A2 reply payload mismatch");
    ensure (reply.provider_rid == "api-a",
            "RM-A2 did not use the requested provider endpoint");

    auto inflight = std::async (std::launch::async, [&consumer_url] {
        return post_json<profile_req_t, profile_res_t> (
          consumer_url, "/profile/request", profile_req_t{.value = "slow"},
          std::chrono::seconds (3));
    });
    touch_file (options.ready_file);
    wait_for_file (options.continue_file, options.control_wait);
    const auto inflight_reply = inflight.get ();
    ensure (inflight_reply.provider_rid == "api-a" && inflight_reply.value == "profile:slow",
            "RM-A2 manual in-flight request was disrupted by auto reconcile");

    std::set<std::string> routed_rids;
    std::map<std::string, std::string> routed_values;
    for (int index = 0; index < 16; ++index) {
        const auto value =
          "rm-a2-after-" + std::to_string (index);
        const auto routed = post_json<profile_req_t, profile_res_t> (
          consumer_url, "/profile/request",
          profile_req_t{.value = value});
        routed_rids.insert (routed.provider_rid);
        routed_values.try_emplace (routed.provider_rid, value);
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    std::cout << "RM-A2 routed providers";
    for (const auto &rid : routed_rids)
        std::cout << " " << rid;
    std::cout << std::endl;
    ensure (routed_rids.contains ("api-a") && routed_rids.contains ("api-b"),
            "RM-A2 manual and auto endpoints were not both retained");
    wait_evidence_contains (provider_a_url, "ProfileReq", "rm-a2", std::chrono::seconds (10));
    wait_evidence_contains (
      options.http_b_endpoint, "ProfileReq", routed_values.at ("api-b"),
      std::chrono::seconds (10));
    std::cout << "scenario RM-A2 passed\n";
}

} // namespace zlink::framework::e2e::registry_messaging::client
