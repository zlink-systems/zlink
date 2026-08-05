/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_b4_scenario (const std::string &play_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-B4");
    }

    constexpr auto actor_id = "sm-b4-remote";
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto requested =
      play_a.post ("/spot/remote-actor-request")
        .body (remote_actor_request_req_t{.join = join_req_t{.key = "b-sm-b4-remote",
                                                             .actor_id = actor_id,
                                                             .display_name = "SM-B4 Remote",
                                                             .level = 4,
                                                             .tags = {"remote", "request"}},
                                          .state = state_req_t{.op = "add", .amount = 14}})
        .submit_raw ()
        .result ();
    if (!requested) {
        throw std::runtime_error (requested.error () ? requested.error ()->what ()
                                                     : "SM-B4 remote actor request HTTP failed");
    }
    if (requested.value ().status >= 400) {
        throw std::runtime_error ("SM-B4 remote actor request HTTP status "
                                  + std::to_string (requested.value ().status) + ": "
                                  + requested.value ().body);
    }

    const auto state_reply =
      nlohmann::json::parse (requested.value ().body).get<state_res_t> ();
    if (state_reply.owner_node_rid != "play-b"
        || state_reply.spot_id != "user:play-b:b-sm-b4-remote"
        || state_reply.value != 14 || state_reply.sequence != 1) {
        throw std::runtime_error ("SM-B4 remote actor request mismatch");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
