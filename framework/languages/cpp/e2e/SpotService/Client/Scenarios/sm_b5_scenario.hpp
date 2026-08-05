/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_b5_scenario (const std::string &play_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-B5");
    }

    constexpr auto actor_id = "sm-b5-missing";
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto missing =
      play_a.post ("/spot/missing-actor")
        .body (missing_actor_req_t{
          .join = join_req_t{.key = "sm-b5-missing",
                             .actor_id = actor_id,
                             .display_name = "SM-B5 Missing",
                             .level = 5,
                             .tags = {"missing", "actor"}},
          .packet_name = "MissingActorPacket"})
        .submit_raw ()
        .result ();
    if (!missing) {
        throw std::runtime_error (missing.error () ? missing.error ()->what ()
                                                  : "SM-B5 missing actor HTTP failed");
    }
    if (missing.value ().status >= 400) {
        throw std::runtime_error ("SM-B5 missing actor HTTP status "
                                  + std::to_string (missing.value ().status) + ": "
                                  + missing.value ().body);
    }

    const auto reply =
      nlohmann::json::parse (missing.value ().body).get<missing_actor_res_t> ();
    if (!reply.failed) {
        throw std::runtime_error ("SM-B5 missing actor request unexpectedly succeeded");
    }
    if (reply.error_kind != "handler_not_found") {
        throw std::runtime_error ("SM-B5 missing actor error kind mismatch: "
                                  + reply.error_kind);
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
