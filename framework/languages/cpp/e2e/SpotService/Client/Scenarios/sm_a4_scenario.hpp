/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/spot_lifecycle_order_context.hpp"
#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_a4_scenario (const std::string &play_http_endpoint,
                                spot_lifecycle_order_context_t &context)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-A4");
    }

    auto api = zlink::http_client::client_t::create ()
                 .base_url (play_http_endpoint)
                 .build ();
    auto raw =
      api.post ("/spot/state/request")
        .body (spot_state_route_req_t{.spot_id = context.spot_id,
                                      .state = state_req_t{.op = "noop", .amount = 0}})
        .submit_raw ()
        .result ();
    if (!raw) {
        throw std::runtime_error (raw.error () ? raw.error ()->what ()
                                               : "SM-A4 route HTTP failed");
    }
    if (raw.value ().status >= 400) {
        throw std::runtime_error ("SM-A4 route HTTP status "
                                  + std::to_string (raw.value ().status) + ": "
                                  + raw.value ().body);
    }

    const auto reply = nlohmann::json::parse (raw.value ().body).get<state_res_t> ();
    if (reply.spot_id != context.spot_id) {
        throw std::runtime_error ("SM-A4 request reached wrong spot: " + reply.spot_id);
    }
    if (reply.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-A4 request reached wrong owner: "
                                  + reply.owner_node_rid);
    }
    context.current_value = reply.value;
}

inline void run_sm_a4_scenario (const std::string &play_http_endpoint,
                                const std::string &play_b_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-A4");
    }
    if (play_b_http_endpoint.empty ()) {
        throw std::runtime_error ("playBHttpEndpoint is required for SM-A4");
    }

    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto joined =
      play_a.post ("/spot/join")
        .body (join_req_t{.key = "sm-a4-owner",
                          .actor_id = "sm-a4-actor",
                          .display_name = "SM-A4",
                          .level = 4,
                          .tags = {"owner-routing"}})
        .submit_raw ()
        .result ();
    if (!joined) {
        throw std::runtime_error (joined.error () ? joined.error ()->what ()
                                                  : "SM-A4 join HTTP failed");
    }
    if (joined.value ().status >= 400) {
        throw std::runtime_error ("SM-A4 join HTTP status "
                                  + std::to_string (joined.value ().status) + ": "
                                  + joined.value ().body);
    }
    const auto created = nlohmann::json::parse (joined.value ().body).get<join_res_t> ();
    const auto expected_spot = user_spot_id_for_key ("sm-a4-owner");
    if (created.spot_id != expected_spot || created.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-A4 initial owner mapping mismatch");
    }

    auto play_b = zlink::http_client::client_t::create ()
                    .base_url (play_b_http_endpoint)
                    .build ();
    for (int index = 0; index < 2; ++index) {
        auto raw =
          play_b.post ("/spot/state/request")
            .body (spot_state_route_req_t{.key = "sm-a4-owner",
                                          .state = state_req_t{.op = "noop", .amount = 0}})
            .submit_raw ()
            .result ();
        if (!raw) {
            throw std::runtime_error (raw.error () ? raw.error ()->what ()
                                                   : "SM-A4 route HTTP failed");
        }
        if (raw.value ().status >= 400) {
            throw std::runtime_error ("SM-A4 route HTTP status "
                                      + std::to_string (raw.value ().status) + ": "
                                      + raw.value ().body);
        }

        const auto reply = nlohmann::json::parse (raw.value ().body).get<state_res_t> ();
        if (reply.spot_id != expected_spot) {
            throw std::runtime_error ("SM-A4 request reached wrong spot: " + reply.spot_id);
        }
        if (reply.owner_node_rid != "play-a") {
            throw std::runtime_error ("SM-A4 request reached wrong owner: "
                                      + reply.owner_node_rid);
        }
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
