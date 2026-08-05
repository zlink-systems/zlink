/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <future>
#include <iostream>
#include <vector>

namespace zlink::framework::e2e::registry_messaging::client
{

inline void run_rm_c9_backpressure_scenario (const client_options_t &options)
{
    const auto consumer = options.backpressure_consumer_url;
    constexpr int slow_send_count = 8;
    post_raw (consumer, "/profile/backpressure/reset");
    std::vector<std::future<std::string>> sends;
    sends.reserve (slow_send_count);
    for (int index = 0; index < slow_send_count; ++index) {
        sends.push_back (std::async (std::launch::async, [consumer, index] {
            auto outcome = post_json<profile_msg_t, backpressure_send_res_t> (
              consumer, "/profile/backpressure/send",
              profile_msg_t{.command_id = "rm-c9-slow-" + std::to_string (index)},
              std::chrono::seconds (2));
            return outcome.outcome;
        }));
    }

    int submitted = 0;
    for (auto &send : sends) {
        const auto outcome = send.get ();
        if (outcome == "Submitted") {
            ++submitted;
        }
    }
    ensure (submitted == slow_send_count, "RM-C9 expected all one-way sends to be submitted");

    std::this_thread::sleep_for (std::chrono::seconds (10));
    wait_provider_evidence_contains (options, "ProfileMsg", "rm-c9-slow-0", std::chrono::seconds (20));
    auto recovery = post_json<profile_req_t, profile_res_t> (
      consumer, "/profile/request", profile_req_t{.value = "rm-c9-after"});
    ensure (recovery.value == "profile:rm-c9-after", "RM-C9 recovery reply mismatch");
    wait_provider_evidence_contains (options, "ProfileReq", "rm-c9-after", std::chrono::seconds (20));
    std::cout << "scenario RM-C9 passed\n";
}

} // namespace zlink::framework::e2e::registry_messaging::client
