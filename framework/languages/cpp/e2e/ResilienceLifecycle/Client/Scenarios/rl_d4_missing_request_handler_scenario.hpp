/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/resilience_request_support.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <iostream>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline void run_rl_d4_missing_request_handler_scenario (const client_options_t &options)
{
    auto consumer = zlink::http_client::client_t::create ()
                      .base_url (options.http_consumer_endpoint)
                      .timeout (std::chrono::milliseconds (10000))
                      .build ();
    const auto failure = consumer.post ("/profile/request/missing")
                           .body (profile_req_t{.value = "fast", .marker = "rl-d4-missing"})
                           .submit<request_failure_res_t> ().result ().value ().body;
    ensure (failure.failed, "RL-D4 expected public failure payload");
    ensure (failure.error_type == "HandlerNotFound",
            "RL-D4 missing handler error type mismatch: " + failure.error_type);
    wait_provider_evidence_contains (options, "DispatchError", "handler_missing:reply_error",
                                     std::chrono::milliseconds (5000));
    std::cout << "scenario RL-D4 passed\n";
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
