/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_c2_scenario (const std::string &play_http_endpoint,
                                const std::string &play_b_http_endpoint)
{
    if (play_http_endpoint.empty () || play_b_http_endpoint.empty ()) {
        throw std::runtime_error (
          "playHttpEndpoint and playBHttpEndpoint are required for SM-C2");
    }

    constexpr auto spot_id = "user:play-b:sm-c2-outbound";
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto play_b = zlink::http_client::client_t::create ()
                    .base_url (play_b_http_endpoint)
                    .build ();

    auto created_raw =
      play_b.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = spot_id})
        .submit_raw ()
        .result ();
    if (!created_raw || created_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-C2 spot create failed");
    }
    const auto created =
      nlohmann::json::parse (created_raw.value ().body).get<create_spot_res_t> ();
    if (created.spot_id != spot_id || created.owner_node_rid != "play-b") {
        throw std::runtime_error ("SM-C2 spot was not created on play-b");
    }

    auto outbound_raw =
      play_a.post ("/spot/outbound")
        .body (spot_outbound_route_req_t{
          .target_node_rid = "play-b", .spot_id = spot_id, .marker = "sm-c2"})
        .submit_raw ()
        .result ();
    if (!outbound_raw || outbound_raw.value ().status >= 400) {
        const auto status =
          outbound_raw ? std::to_string (outbound_raw.value ().status) : std::string ("none");
        const auto body = outbound_raw ? outbound_raw.value ().body : std::string ("");
        throw std::runtime_error ("SM-C2 outbound HTTP failed status=" + status
                                  + " body=" + body);
    }
    const auto outbound =
      nlohmann::json::parse (outbound_raw.value ().body).get<spot_outbound_route_res_t> ();
    if (outbound.spot_id != spot_id || outbound.marker != "sm-c2" || !outbound.accepted) {
        throw std::runtime_error ("SM-C2 outbound route was not accepted");
    }

    auto negative_raw =
      play_a.post ("/spot/outbound-negative")
        .body (spot_outbound_route_req_t{
          .target_node_rid = "play-b", .spot_id = spot_id, .marker = "sm-c2-missing"})
        .submit_raw ()
        .result ();
    if (!negative_raw || negative_raw.value ().status >= 400) {
        const auto status =
          negative_raw ? std::to_string (negative_raw.value ().status) : std::string ("none");
        const auto body = negative_raw ? negative_raw.value ().body : std::string ("");
        throw std::runtime_error ("SM-C2 outbound negative HTTP failed status=" + status
                                  + " body=" + body);
    }
    const auto negative =
      nlohmann::json::parse (negative_raw.value ().body).get<spot_outbound_route_res_t> ();
    if (negative.spot_id != spot_id || negative.marker != "sm-c2-missing"
        || !negative.accepted) {
        throw std::runtime_error ("SM-C2 outbound negative route was not accepted");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
