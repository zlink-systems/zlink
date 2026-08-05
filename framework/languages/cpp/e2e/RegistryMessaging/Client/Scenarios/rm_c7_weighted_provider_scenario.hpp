/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>

namespace zlink::framework::e2e::registry_messaging::client
{

inline void run_rm_c7_weighted_provider_scenario (const client_options_t &options)
{
    const auto consumer_url = options.single_consumer_url;
    std::set<std::string> seen;
    for (int attempt = 0; attempt < 120 && seen.size () < 2; ++attempt) {
        auto reply = post_json<profile_req_t, profile_res_t> (
          consumer_url, "/profile/request",
          profile_req_t{.value = "rm-c7-warm-" + std::to_string (attempt)});
        seen.insert (reply.provider_rid);
        if (seen.size () < 2) {
            std::this_thread::sleep_for (std::chrono::milliseconds (150));
        }
    }
    ensure (seen.count ("api-a") == 1 && seen.count ("api-b") == 1,
            "RM-C7 warm-up never reached both weighted providers");

    std::map<std::string, int> counts;
    constexpr int request_count = 240;
    for (int index = 0; index < request_count; ++index) {
        auto reply = post_json<profile_req_t, profile_res_t> (
          consumer_url, "/profile/request",
          profile_req_t{.value = "weighted-" + std::to_string (index)});
        ++counts[reply.provider_rid];
    }
    ensure (counts["api-a"] > 0 && counts["api-b"] > 0,
            "RM-C7 did not use both weighted providers");
    ensure (counts["api-a"] + counts["api-b"] == request_count, "RM-C7 count mismatch");
    std::cout << "scenario RM-C7 counts api-a=" << counts["api-a"]
              << " api-b=" << counts["api-b"] << std::endl;
    ensure (counts["api-a"] > counts["api-b"] * 2,
            "RM-C7 did not clearly prefer the higher weight provider");
    std::cout << "scenario RM-C7 passed\n";
}

} // namespace zlink::framework::e2e::registry_messaging::client
