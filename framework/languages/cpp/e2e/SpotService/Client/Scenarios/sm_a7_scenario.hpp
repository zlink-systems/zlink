/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_a7_scenario (const std::string &play_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-A7");
    }

    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto raw =
      play_a.post ("/spot/type-mismatch")
        .body (type_mismatch_req_t{"sm-a7-mismatch"})
        .submit_raw ()
        .result ();
    if (!raw) {
        throw std::runtime_error (raw.error () ? raw.error ()->what ()
                                               : "SM-A7 type mismatch HTTP failed");
    }
    if (raw.value ().status >= 400) {
        throw std::runtime_error ("SM-A7 type mismatch HTTP status "
                                  + std::to_string (raw.value ().status) + ": "
                                  + raw.value ().body);
    }

    const auto reply = nlohmann::json::parse (raw.value ().body).get<type_mismatch_res_t> ();
    if (!reply.rejected || reply.error_kind != "spot_type_mismatch") {
        throw std::runtime_error ("SM-A7 expected spot_type_mismatch");
    }
    if (reply.spot_name != user_spot || reply.value != 17) {
        throw std::runtime_error ("SM-A7 original spot state changed");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
