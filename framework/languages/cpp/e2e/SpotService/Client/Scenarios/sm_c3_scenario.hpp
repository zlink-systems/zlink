/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_c3_scenario (const std::string &play_http_endpoint,
                                const std::string &play_b_http_endpoint)
{
    if (play_http_endpoint.empty () || play_b_http_endpoint.empty ()) {
        throw std::runtime_error (
          "playHttpEndpoint and playBHttpEndpoint are required for SM-C3");
    }

    constexpr auto source_spot_id = "user:play-b:sm-c3-source";
    constexpr auto target_spot_id = "user:play-a:sm-c3-target";
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto play_b = zlink::http_client::client_t::create ()
                    .base_url (play_b_http_endpoint)
                    .build ();

    auto target_created_raw =
      play_a.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = target_spot_id})
        .submit_raw ()
        .result ();
    if (!target_created_raw || target_created_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-C3 target spot create failed");
    }
    const auto target_created =
      nlohmann::json::parse (target_created_raw.value ().body).get<create_spot_res_t> ();
    if (target_created.spot_id != target_spot_id
        || target_created.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-C3 target spot was not created on play-a");
    }

    auto source_created_raw =
      play_b.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = source_spot_id})
        .submit_raw ()
        .result ();
    if (!source_created_raw || source_created_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-C3 source spot create failed");
    }
    const auto source_created =
      nlohmann::json::parse (source_created_raw.value ().body).get<create_spot_res_t> ();
    if (source_created.spot_id != source_spot_id
        || source_created.owner_node_rid != "play-b") {
        throw std::runtime_error ("SM-C3 source spot was not created on play-b");
    }

    const auto route_request =
      spot_to_spot_route_req_t{.source_node_rid = "play-b",
                               .source_spot_id = source_spot_id,
                               .target_node_rid = "play-a",
                               .target_spot_id = target_spot_id,
                               .marker = "direct"};
    auto direct_raw =
      play_a.post ("/spot/to-spot/request").body (route_request).submit_raw ().result ();
    if (!direct_raw || direct_raw.value ().status >= 400) {
        const auto status =
          direct_raw ? std::to_string (direct_raw.value ().status) : std::string ("none");
        const auto body = direct_raw ? direct_raw.value ().body : std::string ("");
        throw std::runtime_error ("SM-C3 direct HTTP failed status=" + status + " body=" + body);
    }
    const auto direct =
      nlohmann::json::parse (direct_raw.value ().body).get<spot_to_spot_route_res_t> ();
    if (direct.source_spot_id != source_spot_id
        || direct.target_spot_id != target_spot_id
        || direct.target_value != "sm-c3-direct:reply") {
        throw std::runtime_error ("SM-C3 direct spot-to-spot reply mismatch");
    }

    auto timeout_raw =
      play_a.post ("/spot/to-spot/timeout")
        .body (spot_to_spot_route_req_t{.source_node_rid = "play-b",
                                        .source_spot_id = source_spot_id,
                                        .target_node_rid = "play-a",
                                        .target_spot_id = target_spot_id,
                                        .marker = "slow"})
        .submit_raw ()
        .result ();
    if (!timeout_raw || timeout_raw.value ().status >= 400) {
        const auto status =
          timeout_raw ? std::to_string (timeout_raw.value ().status) : std::string ("none");
        const auto body = timeout_raw ? timeout_raw.value ().body : std::string ("");
        throw std::runtime_error ("SM-C3 timeout HTTP failed status=" + status
                                  + " body=" + body);
    }
    const auto timeout =
      nlohmann::json::parse (timeout_raw.value ().body)
        .get<spot_to_spot_timeout_route_res_t> ();
    if (timeout.source_spot_id != source_spot_id
        || timeout.target_spot_id != target_spot_id || !timeout.failed) {
        throw std::runtime_error ("SM-C3 slow target request did not time out");
    }

    auto negative_raw =
      play_a.post ("/spot/to-spot/negative")
        .body (spot_to_spot_route_req_t{.source_node_rid = "play-b",
                                        .source_spot_id = source_spot_id,
                                        .target_node_rid = "play-a",
                                        .target_spot_id = target_spot_id,
                                        .marker = "missing"})
        .submit_raw ()
        .result ();
    if (!negative_raw || negative_raw.value ().status >= 400) {
        const auto status =
          negative_raw ? std::to_string (negative_raw.value ().status) : std::string ("none");
        const auto body = negative_raw ? negative_raw.value ().body : std::string ("");
        throw std::runtime_error ("SM-C3 negative HTTP failed status=" + status
                                  + " body=" + body);
    }
    const auto negative =
      nlohmann::json::parse (negative_raw.value ().body)
        .get<spot_to_spot_negative_route_res_t> ();
    if (negative.source_spot_id != source_spot_id
        || negative.target_spot_id != target_spot_id || !negative.request_failed) {
        throw std::runtime_error ("SM-C3 missing target handler request did not fail");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
