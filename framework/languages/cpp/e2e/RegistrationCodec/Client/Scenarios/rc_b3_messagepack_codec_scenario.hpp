/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::registration_codec::client
{

inline void run_messagepack_codec_scenario (const client_options_t &options,
                                            const codec_roundtrip_scenario_res_t &roundtrip)
{
    ensure (roundtrip.messagepack.value == "messagepack:b3", "RC-B3 reply mismatch");
    ensure (roundtrip.messagepack.content_type == "application/x-msgpack",
            "RC-B3 content type mismatch");
    wait_evidence_contains (options.http_endpoint, "RC-B3-send",
                            "application/x-msgpack:send-b3", std::chrono::seconds (10));
    std::cout << "scenario RC-B3 passed\n";
}

} // namespace zlink::framework::e2e::registration_codec::client
