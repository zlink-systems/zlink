/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::registry_messaging::client
{

inline void run_rm_c4_timeout_isolation_scenario (const client_options_t &options)
{
    const auto consumer = options.store_consumer_url;
    const auto timeout = post_json<profile_req_t, request_failure_res_t> (
      consumer, "/profile/slow-request", profile_req_t{.value = "slow"});
    ensure (timeout.failed, "RM-C4 expected the slow request to time out");
    ensure (timeout.error_type == "TimeoutException",
            "RM-C4 timeout error type mismatch: " + timeout.error_type);
    auto after = post_json<profile_req_t, profile_res_t> (
      consumer, "/profile/request", profile_req_t{.value = "rm-c4-after-timeout"});
    ensure (after.value == "profile:rm-c4-after-timeout", "RM-C4 post-timeout reply mismatch");
    std::this_thread::sleep_for (std::chrono::milliseconds (1100));
    auto later = post_json<profile_req_t, profile_res_t> (
      consumer, "/profile/request", profile_req_t{.value = "rm-c4-later"});
    ensure (later.value == "profile:rm-c4-later", "RM-C4 later reply mismatch");
    wait_provider_evidence_contains (options, "ProfileReq", "rm-c4-after-timeout",
                                     std::chrono::seconds (10));
    wait_provider_evidence_contains (options, "ProfileReq", "rm-c4-later", std::chrono::seconds (10));
    std::cout << "scenario RM-C4 passed\n";
}

} // namespace zlink::framework::e2e::registry_messaging::client
