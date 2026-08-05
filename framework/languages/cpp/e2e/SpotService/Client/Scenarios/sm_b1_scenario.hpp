/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_b1_scenario (const std::string &play_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-B1");
    }

    constexpr auto actor_id = "sm-b1-local";
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto joined =
      play_a.post ("/spot/join")
        .body (join_req_t{.key = "sm-b1-local",
                          .actor_id = actor_id,
                          .display_name = "SM-B1 Local",
                          .level = 1,
                          .tags = {"local", "actor"}})
        .submit_raw ()
        .result ();
    if (!joined) {
        throw std::runtime_error (joined.error () ? joined.error ()->what ()
                                                  : "SM-B1 join HTTP failed");
    }
    if (joined.value ().status >= 400) {
        throw std::runtime_error ("SM-B1 join HTTP status "
                                  + std::to_string (joined.value ().status) + ": "
                                  + joined.value ().body);
    }
    const auto reply = nlohmann::json::parse (joined.value ().body).get<join_res_t> ();
    if (reply.owner_node_rid != "play-a" || reply.spot_id != "user:play-a:sm-b1-local"
        || reply.actor.actor_id != actor_id || reply.actor.node_rid != "play-a") {
        throw std::runtime_error ("SM-B1 local join reply mismatch");
    }

    auto state =
      play_a.post ("/spot/state")
        .body (spot_state_req_t{.actor_id = actor_id,
                                .state = state_req_t{.op = "add", .amount = 1}})
        .submit_raw ()
        .result ();
    if (!state) {
        throw std::runtime_error (state.error () ? state.error ()->what ()
                                                 : "SM-B1 state HTTP failed");
    }
    if (state.value ().status >= 400) {
        throw std::runtime_error ("SM-B1 state HTTP status "
                                  + std::to_string (state.value ().status) + ": "
                                  + state.value ().body);
    }
    const auto state_reply = nlohmann::json::parse (state.value ().body).get<state_res_t> ();
    if (state_reply.owner_node_rid != "play-a" || state_reply.spot_id != reply.spot_id
        || state_reply.value != 1 || state_reply.sequence != 1) {
        throw std::runtime_error ("SM-B1 local actor request mismatch");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
