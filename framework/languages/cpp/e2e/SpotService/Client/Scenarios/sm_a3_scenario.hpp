/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_a3_scenario (const std::string &play_http_endpoint,
                                const std::string &play_b_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-A3");
    }
    if (play_b_http_endpoint.empty ()) {
        throw std::runtime_error ("playBHttpEndpoint is required for SM-A3");
    }

    auto play_a = zlink::http_client::client_t::create ()
                 .base_url (play_http_endpoint)
                 .build ();
    auto joined =
      play_a.post ("/spot/join")
        .body (join_req_t{.key = "sm-a3-route",
                          .actor_id = "sm-a3-actor",
                          .display_name = "SM-A3",
                          .level = 3,
                          .tags = {"direct"}})
        .submit_raw ()
        .result ();
    if (!joined) {
        throw std::runtime_error (joined.error () ? joined.error ()->what ()
                                                  : "SM-A3 join HTTP failed");
    }
    if (joined.value ().status >= 400) {
        throw std::runtime_error ("SM-A3 join HTTP status "
                                  + std::to_string (joined.value ().status) + ": "
                                  + joined.value ().body);
    }
    auto created = nlohmann::json::parse (joined.value ().body).get<join_res_t> ();

    auto play_b = zlink::http_client::client_t::create ()
                    .base_url (play_b_http_endpoint)
                    .build ();
    auto raw =
      play_b.post ("/spot/direct")
        .body (direct_spot_route_req_t{.target_node_rid = "play-a",
                                       .spot_id = created.spot_id,
                                       .value = "route-direct"})
        .submit_raw ()
        .result ();
    if (!raw) {
        throw std::runtime_error (raw.error () ? raw.error ()->what () : "SM-A3 HTTP failed");
    }
    if (raw.value ().status >= 400) {
        throw std::runtime_error ("SM-A3 HTTP status " + std::to_string (raw.value ().status)
                                  + ": " + raw.value ().body);
    }

    auto routed = nlohmann::json::parse (raw.value ().body).get<direct_spot_res_t> ();
    if (routed.spot_id != created.spot_id) {
        throw std::runtime_error ("SM-A3 route reached wrong spot: " + routed.spot_id);
    }
    if (routed.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-A3 route reached wrong node: " + routed.owner_node_rid);
    }
    if (routed.value != "route-direct:reply") {
        throw std::runtime_error ("SM-A3 direct route reply mismatch: " + routed.value);
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
