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

inline void run_sm_d11_scenario (const std::string &session_http_endpoint,
                                 const std::string &session_stream_endpoint)
{
    if (session_http_endpoint.empty ()) {
        throw std::runtime_error ("sessionHttpEndpoint is required for SM-D11");
    }
    if (session_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-D11");
    }

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;

    constexpr auto actor_id = "actor-sm-d11";
    auto stream = zlink::stream_connector::connector_factory_t::create (options);
    auto connected = stream.connect ();
    if (!connected) {
        throw std::runtime_error ("SM-D11 stream connect failed");
    }
    auto auth =
      stream.request (stream_ensure_auth_req_t{"play-a", actor_id, "SM-D11 Stream Channel"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!auth || auth.value ().actor.actor_id != actor_id
        || auth.value ().session_node_rid != "session-a") {
        throw std::runtime_error ("SM-D11 stream auth reply mismatch");
    }

    auto stream_reply =
      stream.request (actor_ping_req_t{"stream-side"})
        .packet_name ("ActorPingReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<actor_ping_res_t> ();
    if (!stream_reply || stream_reply.value ().actor_id != actor_id
        || stream_reply.value ().node_rid != "play-a"
        || stream_reply.value ().value != "stream-side") {
        throw std::runtime_error ("SM-D11 stream request reply mismatch");
    }

    auto session = zlink::http_client::client_t::create ()
                     .base_url (session_http_endpoint)
                     .build ();
    auto channel_raw =
      session.post ("/channel/control-ping")
        .body (channel_control_ping_req_t{.target_node_rid = "play-a",
                                          .value = "channel-side"})
        .submit_raw ()
        .result ();
    if (!channel_raw || channel_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-D11 channel HTTP request failed");
    }
    auto channel_reply =
      nlohmann::json::parse (channel_raw.value ().body).get<channel_control_ping_res_t> ();
    if (channel_reply.node_rid != "play-a" || channel_reply.value != "channel-side") {
        throw std::runtime_error ("SM-D11 channel reply mismatch");
    }

    (void) stream.close ();
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
