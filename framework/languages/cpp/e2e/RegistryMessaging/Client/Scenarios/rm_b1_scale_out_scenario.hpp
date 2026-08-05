/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <iostream>
#include <set>

namespace zlink::framework::e2e::registry_messaging::client
{

inline void run_rm_b1_scale_out_scenario (const client_options_t &options)
{
    const auto provider_a_url = options.http_a_endpoint;
    const auto provider_b_url = options.http_b_endpoint;
    for (int index = 0; index < 10; ++index) {
        auto reply = post_json<profile_req_t, profile_res_t> (
          provider_a_url, "/profile/request",
          profile_req_t{.value = "scale-out-before-" + std::to_string (index)});
        ensure (reply.provider_rid == "api-a",
                "RM-B1 initial traffic should only use api-a");
    }
    const auto baseline_a = fetch_evidence (provider_a_url);
    ensure (evidence_value_prefix_count (
              baseline_a, "ProfileReq", "scale-out-before-") == 10,
            "RM-B1 baseline evidence was not api-a only");

    touch_file (options.ready_file);
    wait_for_file (options.continue_file, options.control_wait);

    std::set<std::string> providers;
    for (int index = 0; index < 40; ++index) {
        auto reply = post_json<profile_req_t, profile_res_t> (
          provider_a_url, "/profile/request",
          profile_req_t{.value = "scale-out-after-" + std::to_string (index)});
        providers.insert (reply.provider_rid);
    }
    ensure (providers.contains ("api-a") && providers.contains ("api-b"),
            "RM-B1 did not route to both providers after scale-out");
    const auto after_a = fetch_evidence (provider_a_url);
    const auto after_b = fetch_evidence (provider_b_url);
    const auto after_a_count = evidence_value_prefix_count (
      after_a, "ProfileReq", "scale-out-after-");
    const auto after_b_count = evidence_value_prefix_count (
      after_b, "ProfileReq", "scale-out-after-");
    ensure (after_a_count > 0 && after_b_count > 0
              && after_a_count + after_b_count == 40,
            "RM-B1 provider evidence did not match 40 replies");
    std::cout << "scenario RM-B1 passed\n";
}

} // namespace zlink::framework::e2e::registry_messaging::client
