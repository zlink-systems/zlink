/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::registration_codec::client
{

inline void run_json_golden_scenario (const client_options_t &options)
{
    const auto reply = post_empty<json_golden_res_t> (
      options.http_endpoint, "/codec/json-golden");
    ensure (reply.display_name == "Ada Lovelace", "RC-B6 display name mismatch");
    ensure (reply.status == "ready", "RC-B6 status mismatch");
    ensure (reply.balance == -9'223'372'036'854'775'000,
            "RC-B6 signed 64-bit value mismatch");
    ensure (reply.payload == std::vector<std::uint8_t>{0x00, 0x7f, 0x80, 0xff},
            "RC-B6 Base64 bytes mismatch");
    ensure (reply.score == 2'147'000'001, "RC-B6 32-bit value mismatch");
    ensure (reply.ratio == 0.125, "RC-B6 floating-point value mismatch");
    ensure (!reply.optional_note.has_value (), "RC-B6 nullable value mismatch");
    ensure (reply.content_type == "application/json", "RC-B6 content type mismatch");
    std::cout << "scenario RC-B6 passed\n";
}

} // namespace zlink::framework::e2e::registration_codec::client
