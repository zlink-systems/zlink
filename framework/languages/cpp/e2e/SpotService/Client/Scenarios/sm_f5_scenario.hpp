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

inline void run_sm_f5_scenario (const std::string &play_http_endpoint,
                                const std::string &play_b_http_endpoint)
{
    auto play = zlink::http_client::client_t::create ()
                  .base_url (play_http_endpoint)
                  .timeout (std::chrono::milliseconds (3000))
                  .build ();
    auto play_b = zlink::http_client::client_t::create ()
                    .base_url (play_b_http_endpoint)
                    .timeout (std::chrono::milliseconds (3000))
                    .build ();
    const auto spot_key = std::string ("b-sm-f5-close");
    const auto target_spot = user_spot_id_for_key (spot_key);

    auto create =
      play_b.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = target_spot})
        .submit_raw ()
        .result ();
    if (!create || create.value ().status >= 400) {
        throw std::runtime_error ("SM-F5 target Spot create request failed");
    }
    const auto create_reply =
      nlohmann::json::parse (create.value ().body).get<create_spot_res_t> ();
    if (!create_reply.created || create_reply.spot_id != target_spot
        || create_reply.owner_node_rid != "play-b") {
        throw std::runtime_error ("SM-F5 target Spot create reply mismatch");
    }

    const auto require_channel_ping = [&play] (const std::string &value) {
        auto response =
          play.post ("/channel/control-ping")
            .body (channel_control_ping_req_t{.target_node_rid = "play-b", .value = value})
            .submit_raw ()
            .result ();
        if (!response || response.value ().status >= 400) {
            throw std::runtime_error ("SM-F5 ordinary channel request failed");
        }
        const auto reply =
          nlohmann::json::parse (response.value ().body).get<channel_control_ping_res_t> ();
        if (reply.node_rid != "play-b" || reply.value != value) {
            throw std::runtime_error ("SM-F5 ordinary channel reply mismatch");
        }
    };

    require_channel_ping ("channel-before-close-f5");

    auto before_close =
      play.post ("/spot/direct")
        .body (direct_spot_route_req_t{.target_node_rid = "play-b",
                                       .spot_id = target_spot,
                                       .value = "spot-before-close-f5",
                                       .source_actor_id = "external-client"})
        .submit_raw ()
        .result ();
    if (!before_close || before_close.value ().status >= 400) {
        throw std::runtime_error ("SM-F5 spot route failed before close");
    }
    const auto before_close_reply =
      nlohmann::json::parse (before_close.value ().body).get<direct_spot_res_t> ();
    if (before_close_reply.value != "spot-before-close-f5:reply") {
        throw std::runtime_error ("SM-F5 spot route reply mismatch before close");
    }

    auto close =
      play_b.post ("/spot/close")
        .body (close_spot_req_t{.key = spot_key})
        .submit_raw ()
        .result ();
    if (!close || close.value ().status >= 400) {
        throw std::runtime_error ("SM-F5 target Spot close request failed");
    }
    const auto close_reply =
      nlohmann::json::parse (close.value ().body).get<close_spot_res_t> ();
    if (!close_reply.closed || close_reply.spot_id != target_spot) {
        throw std::runtime_error ("SM-F5 target Spot was not closed");
    }

    auto after_close =
      play.post ("/spot/direct")
        .body (direct_spot_route_req_t{.target_node_rid = "play-b",
                                       .spot_id = target_spot,
                                       .value = "spot-after-close-f5",
                                       .source_actor_id = "external-client"})
        .submit_raw ()
        .result ();
    if (after_close && after_close.value ().status < 400) {
        throw std::runtime_error ("SM-F5 closed spot route unexpectedly succeeded");
    }

    require_channel_ping ("channel-after-close-f5");
    std::cout << "scenario SM-F5 passed\n";
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
