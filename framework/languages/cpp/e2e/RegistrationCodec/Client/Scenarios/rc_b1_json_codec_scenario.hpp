/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::registration_codec::client
{

inline void run_json_codec_scenario (const client_options_t &options,
                                     const codec_roundtrip_scenario_res_t &roundtrip)
{
    ensure (roundtrip.json.value == "json:b1", "RC-B1 reply mismatch");
    ensure (roundtrip.json.content_type == "application/json", "RC-B1 content type mismatch");
    wait_evidence_contains (options.http_endpoint, "RC-B1-send",
                            "application/json:send-b1", std::chrono::seconds (10));
    std::cout << "scenario RC-B1 passed\n";
}

} // namespace zlink::framework::e2e::registration_codec::client
