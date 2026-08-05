/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/http_client.hpp>
#include <zlink/stream_connector.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_d2_scenario (const std::string &play_b_http_endpoint,
                                const std::string &session_stream_endpoint)
{
    if (play_b_http_endpoint.empty ()) {
        throw std::runtime_error ("playBHttpEndpoint is required for SM-D2");
    }
    if (session_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-D2");
    }

    constexpr auto actor_id = "actor-sm-d2";
    auto play_b = zlink::http_client::client_t::create ()
                    .base_url (play_b_http_endpoint)
                    .build ();
    auto joined =
      play_b.post ("/spot/join")
        .body (join_req_t{.key = "b-sm-d2-remote",
                          .actor_id = actor_id,
                          .display_name = "SM-D2 Remote",
                          .level = 2,
                          .tags = {"stream", "SM-D2"}})
        .submit_raw ()
        .result ();
    if (!joined) {
        throw std::runtime_error (joined.error () ? joined.error ()->what ()
                                                  : "SM-D2 join HTTP failed");
    }
    if (joined.value ().status >= 400) {
        throw std::runtime_error ("SM-D2 join HTTP status "
                                  + std::to_string (joined.value ().status) + ": "
                                  + joined.value ().body);
    }
    const auto join_reply = nlohmann::json::parse (joined.value ().body).get<join_res_t> ();
    if (join_reply.owner_node_rid != "play-b" || join_reply.actor.actor_id != actor_id) {
        throw std::runtime_error ("SM-D2 actor was not created on remote play-b node");
    }
    std::cout << "sm-d2 actor-ref node=" << join_reply.actor.node_rid
              << " type=" << join_reply.actor.actor_type
              << " actor=" << join_reply.actor.actor_id
              << " generation=" << join_reply.actor.generation
              << " spot=" << join_reply.spot_id << '\n';

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;

    auto remote = zlink::stream_connector::connector_factory_t::create (options);
    auto unbound = zlink::stream_connector::connector_factory_t::create (options);

    auto remote_connected = remote.connect ();
    if (!remote_connected) {
        throw std::runtime_error ("SM-D2 remote stream connect failed");
    }
    auto unbound_connected = unbound.connect ();
    if (!unbound_connected) {
        throw std::runtime_error ("SM-D2 unbound stream connect failed");
    }

    auto auth =
      remote.request (stream_auth_req_t{"play-b", actor_id, "SM-D2 Remote", join_reply.actor})
        .packet_name ("StreamAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!auth || auth.value ().session_node_rid != "session-a") {
        throw std::runtime_error (
          std::string ("SM-D2 stream auth failed: ")
          + (auth.error () ? auth.error ()->message : "unexpected session node"));
    }

    auto ping = remote.request (actor_ping_req_t{"remote-relay"})
                  .packet_name ("ActorPingReq")
                  .metadata ("actor-id", actor_id)
                  .timeout (std::chrono::milliseconds (3000))
                  .submit<actor_ping_res_t> ();
    if (!ping) {
        throw std::runtime_error ("SM-D2 actor ping request failed");
    }
    if (ping.value ().actor_id != actor_id || ping.value ().node_rid != "play-b"
        || ping.value ().value != "remote-relay" || ping.value ().seen != 1) {
        throw std::runtime_error ("SM-D2 actor ping reply mismatch");
    }

    auto remote_wait =
      remote.wait_for<actor_push_notify_t> (std::chrono::milliseconds (10000))
        .to_future ("SM-D2 remote stream push notify missing");
    auto unbound_wait =
      unbound.wait_for<actor_push_notify_t> (std::chrono::milliseconds (500));
    auto pushed = remote.request (actor_push_req_t{"push-remote"})
                    .packet_name ("PushReq")
                    .metadata ("actor-id", actor_id)
                    .timeout (std::chrono::milliseconds (3000))
                    .submit<actor_push_res_t> ();
    if (!pushed || !pushed.value ().pushed || pushed.value ().actor_id != actor_id) {
        throw std::runtime_error ("SM-D2 actor push request failed");
    }

    auto notify = remote_wait.get ();
    if (notify.actor_id != actor_id || notify.value != "push-remote") {
        throw std::runtime_error ("SM-D2 remote stream push notify mismatch");
    }
    auto unbound_notify = unbound_wait.submit ();
    if (unbound_notify) {
        throw std::runtime_error ("SM-D2 unbound stream received push");
    }

    (void) remote.close ();
    (void) unbound.close ();
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
