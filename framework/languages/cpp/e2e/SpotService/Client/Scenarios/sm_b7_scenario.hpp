/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/http_client.hpp>
#include <zlink/stream_connector.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_b7_scenario (const std::string &play_http_endpoint,
                                const std::string &session_stream_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-B7");
    }
    if (session_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-B7");
    }

    constexpr auto actor_id = "sm-b7-order";
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto joined =
      play_a.post ("/spot/join")
        .body (join_req_t{.key = "sm-b7-order",
                          .actor_id = actor_id,
                          .display_name = "SM-B7 Order",
                          .level = 7,
                          .tags = {"order", "SM-B7"}})
        .submit_raw ()
        .result ();
    if (!joined) {
        throw std::runtime_error (joined.error () ? joined.error ()->what ()
                                                 : "SM-B7 join HTTP failed");
    }
    if (joined.value ().status >= 400) {
        throw std::runtime_error ("SM-B7 join HTTP status "
                                  + std::to_string (joined.value ().status) + ": "
                                  + joined.value ().body);
    }
    const auto join_reply = nlohmann::json::parse (joined.value ().body).get<join_res_t> ();

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto stream = zlink::stream_connector::connector_factory_t::create (options);

    auto connected = stream.connect ();
    if (!connected) {
        throw std::runtime_error ("SM-B7 stream connect failed");
    }
    auto auth =
      stream.request (stream_auth_req_t{"play-a", actor_id, "SM-B7 Order", join_reply.actor})
        .packet_name ("StreamAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!auth) {
        throw std::runtime_error ("SM-B7 stream auth failed");
    }

    auto first = stream.request (actor_ping_req_t{"order-1"})
                   .packet_name ("ActorPingReq")
                   .metadata ("actor-id", actor_id)
                   .timeout (std::chrono::milliseconds (3000))
                   .submit<actor_ping_res_t> ();
    auto second = stream.request (actor_ping_req_t{"order-2"})
                    .packet_name ("ActorPingReq")
                    .metadata ("actor-id", actor_id)
                    .timeout (std::chrono::milliseconds (3000))
                    .submit<actor_ping_res_t> ();
    (void) stream.close ();

    if (!first || !second) {
        throw std::runtime_error ("SM-B7 actor ping request failed");
    }
    if (first.value ().actor_id != actor_id || first.value ().value != "order-1"
        || first.value ().seen != 1 || second.value ().actor_id != actor_id
        || second.value ().value != "order-2" || second.value ().seen != 2) {
        throw std::runtime_error ("SM-B7 actor ping order mismatch");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
