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

inline void run_sm_b6_scenario (const std::string &play_http_endpoint,
                                const std::string &session_stream_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-B6");
    }
    if (session_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-B6");
    }

    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();

    constexpr auto leave_actor_id = "sm-b6-left";
    auto leave_joined =
      play_a.post ("/spot/join")
        .body (join_req_t{.key = "sm-b6-left",
                          .actor_id = leave_actor_id,
                          .display_name = "SM-B6 Left",
                          .level = 6,
                          .tags = {"leave", "SM-B6"}})
        .submit_raw ()
        .result ();
    if (!leave_joined) {
        throw std::runtime_error (leave_joined.error () ? leave_joined.error ()->what ()
                                                        : "SM-B6 leave join HTTP failed");
    }
    if (leave_joined.value ().status >= 400) {
        throw std::runtime_error ("SM-B6 leave join HTTP status "
                                  + std::to_string (leave_joined.value ().status) + ": "
                                  + leave_joined.value ().body);
    }
    auto leave_actor =
      nlohmann::json::parse (leave_joined.value ().body).get<join_res_t> ().actor;

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto stream = zlink::stream_connector::connector_factory_t::create (options);

    auto connected = stream.connect ();
    if (!connected) {
        throw std::runtime_error ("SM-B6 leave stream connect failed");
    }

    auto leave_auth =
      stream.request (stream_auth_req_t{"play-a", leave_actor_id, "SM-B6 Left", leave_actor})
        .packet_name ("StreamAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!leave_auth) {
        throw std::runtime_error (leave_auth.error () ? leave_auth.error ()->message
                                                      : "SM-B6 leave stream auth failed");
    }

    auto left = stream.request (leave_req_t{.actor_id = leave_actor_id, .reason = "explicit"})
                  .packet_name ("LeaveReq")
                  .metadata ("actor-id", leave_actor_id)
                  .timeout (std::chrono::milliseconds (3000))
                  .submit<leave_res_t> ();
    if (!left || !left.value ().left || left.value ().actor_id != leave_actor_id) {
        throw std::runtime_error ("SM-B6 leave reply mismatch");
    }
    (void) stream.close ();

    constexpr auto disconnect_actor_id = "sm-b6-disconnect-d5-notified";
    auto disconnect_joined =
      play_a.post ("/spot/join")
        .body (join_req_t{.key = "sm-b6-disconnect",
                          .actor_id = disconnect_actor_id,
                          .display_name = "SM-B6 Disconnected",
                          .level = 6,
                          .tags = {"disconnect", "SM-B6"}})
        .submit_raw ()
        .result ();
    if (!disconnect_joined) {
        throw std::runtime_error (disconnect_joined.error () ? disconnect_joined.error ()->what ()
                                                            : "SM-B6 disconnect join HTTP failed");
    }
    if (disconnect_joined.value ().status >= 400) {
        throw std::runtime_error ("SM-B6 disconnect join HTTP status "
                                  + std::to_string (disconnect_joined.value ().status) + ": "
                                  + disconnect_joined.value ().body);
    }
    auto disconnect_actor =
      nlohmann::json::parse (disconnect_joined.value ().body).get<join_res_t> ().actor;

    auto disconnect_stream = zlink::stream_connector::connector_factory_t::create (options);
    auto disconnect_connected = disconnect_stream.connect ();
    if (!disconnect_connected) {
        throw std::runtime_error ("SM-B6 disconnect stream connect failed");
    }
    auto disconnect_auth =
      disconnect_stream.request (stream_auth_req_t{"play-a", disconnect_actor_id, "SM-B6 Disconnected",
                          disconnect_actor})
        .packet_name ("StreamAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!disconnect_auth) {
        throw std::runtime_error (disconnect_auth.error () ? disconnect_auth.error ()->message
                                                           : "SM-B6 disconnect stream auth failed");
    }
    (void) disconnect_stream.close ();
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
