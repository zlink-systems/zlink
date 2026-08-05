/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::registration_codec::client
{

inline void run_protobuf_codec_scenario (const client_options_t &options,
                                         const codec_roundtrip_scenario_res_t &roundtrip)
{
    ensure (roundtrip.protobuf.value == "protobuf:b2", "RC-B2 reply mismatch");
    ensure (roundtrip.protobuf.content_type == "application/x-protobuf",
            "RC-B2 content type mismatch");
    wait_evidence_contains (options.http_endpoint, "RC-B2-send",
                            "application/x-protobuf:send-b2", std::chrono::seconds (10));
    std::cout << "scenario RC-B2 passed\n";
}

} // namespace zlink::framework::e2e::registration_codec::client
