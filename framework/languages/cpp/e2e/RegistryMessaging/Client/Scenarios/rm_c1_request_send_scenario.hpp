/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::registry_messaging::client
{

inline void run_rm_c1_request_send_scenario (const client_options_t &options)
{
    const auto provider_a_url = options.http_a_endpoint;
    auto request = post_json<profile_req_t, profile_res_t> (
      provider_a_url, "/profile/request", profile_req_t{.value = "c1"});
    ensure (request.value == "profile:c1", "RM-C1 reply mismatch");

    auto sent = post_json<profile_msg_t, operation_status_t> (
      provider_a_url, "/profile/command", profile_msg_t{.command_id = "cmd-c1"});
    ensure (sent.status == "sent", "RM-C1 send endpoint failed");
    std::this_thread::sleep_for (std::chrono::milliseconds (100));

    const auto evidence_a = fetch_evidence (options.http_a_endpoint);
    const auto evidence_b = fetch_evidence (options.http_b_endpoint);
    bool command_recorded = false;
    for (const auto &snapshot : {evidence_a, evidence_b}) {
        for (const auto &entry : snapshot.entries) {
            if (entry.marker == "ProfileMsg" && entry.value == "cmd-c1") {
                command_recorded = true;
            }
        }
    }
    ensure (command_recorded, "RM-C1 send handler evidence was not recorded");
    std::cout << "scenario RM-C1 passed\n";
}

} // namespace zlink::framework::e2e::registry_messaging::client
