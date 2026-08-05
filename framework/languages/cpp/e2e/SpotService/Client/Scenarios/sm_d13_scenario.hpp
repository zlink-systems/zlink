/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/stream_connector.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_d13_scenario (const std::string &session_stream_endpoint)
{
    if (session_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-D13");
    }

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    options.heartbeat.enabled = true;
    options.heartbeat.interval = std::chrono::milliseconds (200);
    options.heartbeat.timeout = std::chrono::milliseconds (2000);

    constexpr auto actor_id = "actor-sm-d13";
    auto stream = zlink::stream_connector::connector_factory_t::create (options);
    auto connected = stream.connect ();
    if (!connected) {
        throw std::runtime_error (
          "SM-D13 stream connect failed: "
          + (connected.error () ? connected.error ()->message : "unknown stream error"));
    }

    auto auth = stream.request (stream_ensure_auth_req_t{"play-a", actor_id, "SM-D13 Heartbeat"})
                  .packet_name ("StreamEnsureAuthReq")
                  .timeout (std::chrono::milliseconds (3000))
                  .submit<stream_auth_res_t> ();
    if (!auth || auth.value ().actor.actor_id != actor_id
        || auth.value ().session_node_rid != "session-a") {
        throw std::runtime_error (
          "SM-D13 stream auth failed: "
          + (auth.error () ? auth.error ()->message : "stream auth reply mismatch"));
    }

    std::this_thread::sleep_for (std::chrono::milliseconds (700));
    if (!stream.is_connected ()) {
        throw std::runtime_error ("SM-D13 heartbeat-enabled stream disconnected");
    }

    auto ping = stream.request (actor_ping_req_t{"heartbeat"})
                  .packet_name ("ActorPingReq")
                  .timeout (std::chrono::milliseconds (3000))
                  .submit<actor_ping_res_t> ();
    if (!ping || ping.value ().actor_id != actor_id || ping.value ().node_rid != "play-a"
        || ping.value ().value != "heartbeat") {
        throw std::runtime_error ("SM-D13 heartbeat stream request mismatch");
    }

    (void) stream.close ();
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
