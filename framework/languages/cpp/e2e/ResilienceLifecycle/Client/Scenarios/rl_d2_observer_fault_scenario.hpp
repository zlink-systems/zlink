/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"
#include "../Support/resilience_request_support.hpp"

#include <chrono>
#include <iostream>
#include <thread>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline void set_observer_fault_mode (const client_options_t &options, const std::string &path)
{
    post_provider_admin (options.http_a_endpoint, path);
    post_provider_admin (options.http_b_endpoint, path);
}

inline void run_observer_fault_scenario (const client_options_t &options)
{
    set_observer_fault_mode (options, "/admin/fault/observer-throws");

    const auto missing = post_consumer_missing (options, "rl-d2-error");
    ensure (missing.failed, "RL-D2 missing handler request should fail");
    ensure (missing.error_type == "HandlerNotFound",
            "RL-D2 missing handler error type mismatch: " + missing.error_type);

    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    ensure (any_provider_evidence_contains (options, "DispatchError",
                                            "handler_missing:reply_error"),
            "RL-D2 dispatch error evidence was not recorded");

    const auto follow_up = request_profile (options, "rl-d2-after");
    ensure (follow_up.value == "profile:rl-d2-after",
            "RL-D2 messaging did not continue after observer failure");

    set_observer_fault_mode (options, "/admin/fault/none");
    wait_provider_evidence_contains (options, "ProfileReq", "rl-d2-after",
                                     std::chrono::milliseconds (5000));
    std::cout << "scenario RL-D2 passed\n";
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
