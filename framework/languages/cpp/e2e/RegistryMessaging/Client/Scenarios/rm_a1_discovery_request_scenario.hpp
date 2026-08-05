/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <iostream>
namespace zlink::framework::e2e::registry_messaging::client
{

inline void run_rm_a1_discovery_request_scenario (const client_options_t &options)
{
    const auto consumer_url = options.store_consumer_url;
    const auto reply = post_json<profile_req_t, profile_res_t> (
      consumer_url, "/profile/request", profile_req_t{.value = "rm-a1"});
    ensure (reply.value == "profile:rm-a1", "RM-A1 reply payload mismatch");
    ensure (reply.provider_rid == "api-a" || reply.provider_rid == "api-b",
            "RM-A1 provider rid was not api-a/api-b");

    if (reply.provider_rid == "api-a") {
        ensure (evidence_contains (
                  fetch_evidence (options.http_a_endpoint), "ProfileReq", "rm-a1"),
                "RM-A1 selected provider evidence was not recorded");
    } else {
        ensure (evidence_contains (
                  fetch_evidence (options.http_b_endpoint), "ProfileReq", "rm-a1"),
                "RM-A1 selected provider evidence was not recorded");
    }
    std::cout << "scenario RM-A1 passed\n";
}

} // namespace zlink::framework::e2e::registry_messaging::client
