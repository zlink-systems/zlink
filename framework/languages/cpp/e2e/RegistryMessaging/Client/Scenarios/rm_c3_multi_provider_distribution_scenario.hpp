/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <iostream>
#include <map>

namespace zlink::framework::e2e::registry_messaging::client
{

inline void run_rm_c3_multi_provider_distribution_scenario (const client_options_t &options)
{
    const auto before_a = fetch_evidence (options.http_a_endpoint);
    const auto before_b = fetch_evidence (options.http_b_endpoint);
    const std::string marker = "rm-c3";
    const auto consumer = options.direct_consumer_url;
    std::map<std::string, int> counts;
    constexpr int request_count = 90;
    for (int index = 0; index < request_count; ++index) {
        const auto value = marker + "-" + std::to_string (index);
        const auto reply = post_json<profile_req_t, profile_res_t> (
          consumer, "/profile/request", profile_req_t{.value = value});
        ensure (reply.value == "profile:" + value, "RM-C3 reply value mismatch");
        ensure (reply.provider_rid == "api-a" || reply.provider_rid == "api-b",
                "RM-C3 reply provider mismatch");
        ++counts[reply.provider_rid];
    }
    ensure (counts["api-a"] > 0 && counts["api-b"] > 0,
            "RM-C3 did not distribute to both providers");
    ensure (counts["api-a"] + counts["api-b"] == request_count, "RM-C3 count mismatch");
    const auto after_a = fetch_evidence (options.http_a_endpoint);
    const auto after_b = fetch_evidence (options.http_b_endpoint);
    const auto evidence_delta =
      evidence_value_prefix_count (after_a, "ProfileReq", marker)
      - evidence_value_prefix_count (before_a, "ProfileReq", marker)
      + evidence_value_prefix_count (after_b, "ProfileReq", marker)
      - evidence_value_prefix_count (before_b, "ProfileReq", marker);
    ensure (evidence_delta == request_count, "RM-C3 provider evidence count mismatch");
    std::cout << "scenario RM-C3 passed\n";
}

} // namespace zlink::framework::e2e::registry_messaging::client
