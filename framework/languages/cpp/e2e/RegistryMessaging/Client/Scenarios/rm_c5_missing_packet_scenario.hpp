/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::registry_messaging::client
{

inline void run_rm_c5_missing_packet_scenario (const client_options_t &options)
{
    const auto consumer = options.store_consumer_url;
    auto missing = post_json<profile_req_t, request_failure_res_t> (
      consumer, "/profile/missing-request", profile_req_t{.value = "missing"});
    ensure (missing.failed, "RM-C5 missing request unexpectedly succeeded");
    // The common error contract exposes a missing handler as public NotFound;
    // handler_missing remains an internal message-flow reason in evidence.
    ensure (missing.error_type == "NotFound",
            "RM-C5 missing handler error type mismatch: " + missing.error_type);
    auto dropped = post_json<profile_msg_t, operation_status_t> (
      consumer, "/profile/missing-command", profile_msg_t{.command_id = "missing-send"});
    ensure (dropped.status == "sent", "RM-C5 missing send endpoint failed");
    auto normal = post_json<profile_req_t, profile_res_t> (
      consumer, "/profile/request", profile_req_t{.value = "normal"});
    ensure (normal.value == "profile:normal", "RM-C5 normal request after missing packet failed");

    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    const auto evidence_a = fetch_evidence (options.http_a_endpoint);
    const auto evidence_b = fetch_evidence (options.http_b_endpoint);
    bool reply_error_recorded = false;
    bool drop_recorded = false;
    for (const auto &snapshot : {evidence_a, evidence_b}) {
        for (const auto &entry : snapshot.entries) {
            if (entry.marker != "DispatchError") {
                continue;
            }
            if (entry.value == "handler_missing:reply_error") {
                reply_error_recorded = true;
            }
            if (entry.value == "handler_missing:drop") {
                drop_recorded = true;
            }
        }
    }
    ensure (reply_error_recorded, "RM-C5 missing request dispatch evidence was not recorded");
    ensure (drop_recorded, "RM-C5 missing send dispatch evidence was not recorded");
    std::cout << "scenario RM-C5 passed\n";
}

} // namespace zlink::framework::e2e::registry_messaging::client
