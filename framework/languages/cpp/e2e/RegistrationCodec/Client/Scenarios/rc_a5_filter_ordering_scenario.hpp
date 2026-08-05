/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <iostream>
#include <vector>

namespace zlink::framework::e2e::registration_codec::client
{

inline void run_filter_ordering_scenario (const client_options_t &options)
{
    const auto &http_endpoint = options.http_endpoint;
    const auto reply = post_empty<filter_order_res_t> (http_endpoint, "/registration/filter-order");
    const std::vector<std::string> expected{
      "first-before", "second-before", "handler", "second-after", "first-after"};
    ensure (reply.value == "filter-order", "RC-A5 reply value mismatch");
    ensure (reply.order == std::vector<std::string>{"first-before", "second-before", "handler"},
            "RC-A5 handler-visible filter order mismatch");

    const auto expected_json = nlohmann::json (expected).dump ();
    wait_evidence_contains (http_endpoint, "RC-A5", expected_json, std::chrono::seconds (10));
    std::cout << "scenario RC-A5 passed\n";
}

} // namespace zlink::framework::e2e::registration_codec::client
