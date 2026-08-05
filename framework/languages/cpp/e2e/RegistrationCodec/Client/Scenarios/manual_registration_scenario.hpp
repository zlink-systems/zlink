/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::registration_codec::client
{

inline void run_manual_registration_scenario (const client_options_t &options)
{
    const auto reply =
      post_empty<echo_manual_res_t> (options.http_endpoint,
                                    "/registration/manual");
    ensure (reply.value == "manual:manual", "RC-A3 reply mismatch");
    ensure (reply.packet_name == "EchoManual", "RC-A3 packet name mismatch");
    ensure (reply.content_type == "application/json", "RC-A3 content type mismatch");
    wait_evidence_contains (options.http_endpoint, "RC-A3-send",
                            "application/json:send-a3", std::chrono::seconds (10));
    std::cout << "scenario RC-A3 passed\n";
}

} // namespace zlink::framework::e2e::registration_codec::client
