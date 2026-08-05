/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_b2_scenario (const std::string &play_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-B2");
    }

    constexpr auto actor_id = "sm-b2-remote";
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto flow =
      play_a.post ("/spot/remote-actor")
        .body (remote_actor_flow_req_t{
          .join = join_req_t{.key = "b-sm-b2-remote",
                             .actor_id = actor_id,
                             .display_name = "SM-B2 Remote",
                             .level = 2,
                             .tags = {"remote", "actor"}},
          .state = state_req_t{.op = "add", .amount = 2}})
        .submit_raw ()
        .result ();
    if (!flow) {
        throw std::runtime_error (flow.error () ? flow.error ()->what ()
                                                : "SM-B2 remote actor HTTP failed");
    }
    if (flow.value ().status >= 400) {
        throw std::runtime_error ("SM-B2 remote actor HTTP status "
                                  + std::to_string (flow.value ().status) + ": "
                                  + flow.value ().body);
    }

    const auto reply =
      nlohmann::json::parse (flow.value ().body).get<remote_actor_flow_res_t> ();
    if (reply.join.owner_node_rid != "play-b"
        || reply.join.spot_id != "user:play-b:b-sm-b2-remote"
        || reply.join.actor_id != actor_id
        || reply.join.actor.node_rid != "play-b") {
        throw std::runtime_error ("SM-B2 remote join reply mismatch");
    }
    if (reply.state.owner_node_rid != "play-b" || reply.state.spot_id != reply.join.spot_id
        || reply.state.value != 2 || reply.state.sequence != 1) {
        throw std::runtime_error ("SM-B2 remote actor request mismatch");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
