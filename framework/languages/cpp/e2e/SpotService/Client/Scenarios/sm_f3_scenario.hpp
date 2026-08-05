/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_f3_scenario (const std::string &play_http_endpoint,
                                const std::string &play_b_http_endpoint,
                                const std::string &remote_spot)
{
    auto play = zlink::http_client::client_t::create ()
                  .base_url (play_http_endpoint)
                  .timeout (std::chrono::milliseconds (3000))
                  .build ();
    auto normal_route_after_spot =
      play.post ("/channel/control-ping")
        .body (channel_control_ping_req_t{.target_node_rid = "play-b",
                                          .value = "route-mixed-f3"})
        .submit_raw ()
        .result ();
    if (!normal_route_after_spot || normal_route_after_spot.value ().status >= 400) {
        throw std::runtime_error ("SM-F3 normal route packet failed after spot route");
    }

    auto play_b = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .timeout (std::chrono::milliseconds (3000))
                    .build ();
    auto raw =
      play_b.post ("/spot/direct")
        .body (direct_spot_route_req_t{.target_node_rid = "play-b",
                                       .spot_id = remote_spot,
                                       .value = "route-mixed",
                                       .source_actor_id = "external-client"})
        .submit_raw ()
        .result ();
    if (!raw || raw.value ().status >= 400) {
        throw std::runtime_error ("SM-F3 spot route packet failed after normal route");
    }
    const auto spot_route_after_normal =
      nlohmann::json::parse (raw.value ().body).get<direct_spot_res_t> ();
    if (spot_route_after_normal.value != "route-mixed:reply") {
        throw std::runtime_error ("SM-F3 spot route packet failed after normal route");
    }

    std::cout << "scenario SM-F3 passed\n";
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
