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

inline void run_sm_d3_scenario (const std::string &play_http_endpoint,
                                const std::string &session_stream_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-D3");
    }
    if (session_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-D3");
    }

    constexpr auto entry_actor_id = "actor-sm-d3-entry";
    zlink::stream_connector::connector_options_t entry_options;
    entry_options.endpoint = session_stream_endpoint;
    entry_options.connect_timeout = std::chrono::milliseconds (3000);
    entry_options.request_timeout = std::chrono::milliseconds (3000);
    entry_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto entry = zlink::stream_connector::connector_factory_t::create (entry_options);
    auto entry_connected = entry.connect ();
    if (!entry_connected) {
        throw std::runtime_error ("SM-D3 entry stream connect failed");
    }
    auto entry_auth =
      entry.request (stream_ensure_auth_req_t{"play-a", entry_actor_id, "SM-D3 Entry"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!entry_auth || entry_auth.value ().session_node_rid != "session-a"
        || entry_auth.value ().actor.actor_id != entry_actor_id) {
        throw std::runtime_error ("SM-D3 entry stream auth failed");
    }

    auto entry_ping =
      entry.request (actor_ping_req_t{"entry-relay"})
        .packet_name ("ActorPingReq")
        .metadata ("actor-id", entry_actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<actor_ping_res_t> ();
    if (!entry_ping || entry_ping.value ().actor_id != entry_actor_id
        || entry_ping.value ().node_rid != "play-a" || entry_ping.value ().value != "entry-relay"
        || entry_ping.value ().seen != 1) {
        throw std::runtime_error ("SM-D3 entry actor ping mismatch");
    }

    auto entry_wait =
      entry.wait_for<actor_push_notify_t> (std::chrono::milliseconds (10000))
        .to_future ("SM-D3 entry push notify missing");
    auto entry_push =
      entry.request (actor_push_req_t{"entry-push"})
        .packet_name ("PushReq")
        .metadata ("actor-id", entry_actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<actor_push_res_t> ();
    if (!entry_push || !entry_push.value ().pushed
        || entry_push.value ().actor_id != entry_actor_id) {
        throw std::runtime_error ("SM-D3 entry actor push failed");
    }
    auto entry_notify = entry_wait.get ();
    if (entry_notify.actor_id != entry_actor_id || entry_notify.value != "entry-push") {
        throw std::runtime_error ("SM-D3 entry push notify mismatch");
    }

    constexpr auto user_actor_id = "actor-sm-d3-user";
    constexpr auto user_key = "sm-d3-user";
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto joined =
      play_a.post ("/spot/join")
        .body (join_req_t{.key = user_key,
                          .actor_id = user_actor_id,
                          .display_name = "SM-D3 User",
                          .level = 3,
                          .tags = {"stream", "SM-D3"}})
        .submit_raw ()
        .result ();
    if (!joined || joined.value ().status >= 400) {
        throw std::runtime_error ("SM-D3 user spot join failed");
    }
    const auto user_join = nlohmann::json::parse (joined.value ().body).get<join_res_t> ();
    if (user_join.owner_node_rid != "play-a" || user_join.actor.actor_id != user_actor_id) {
        throw std::runtime_error ("SM-D3 user actor was not created on play-a");
    }

    zlink::stream_connector::connector_options_t user_options;
    user_options.endpoint = session_stream_endpoint;
    user_options.connect_timeout = std::chrono::milliseconds (3000);
    user_options.request_timeout = std::chrono::milliseconds (3000);
    user_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto user = zlink::stream_connector::connector_factory_t::create (user_options);
    auto user_connected = user.connect ();
    if (!user_connected) {
        throw std::runtime_error ("SM-D3 user stream connect failed");
    }
    auto user_auth =
      user.request (stream_auth_req_t{"play-a", user_actor_id, "SM-D3 User", user_join.actor})
        .packet_name ("StreamAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!user_auth || user_auth.value ().session_node_rid != "session-a") {
        throw std::runtime_error ("SM-D3 user stream auth failed");
    }

    auto user_ping =
      user.request (actor_ping_req_t{"user-relay"})
        .packet_name ("ActorPingReq")
        .metadata ("actor-id", user_actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<actor_ping_res_t> ();
    if (!user_ping || user_ping.value ().actor_id != user_actor_id
        || user_ping.value ().node_rid != "play-a" || user_ping.value ().value != "user-relay"
        || user_ping.value ().spot_id != user_join.spot_id) {
        throw std::runtime_error ("SM-D3 user actor ping mismatch");
    }

    auto user_wait =
      user.wait_for<actor_push_notify_t> (std::chrono::milliseconds (10000))
        .to_future ("SM-D3 user push notify missing");
    auto user_push =
      user.request (actor_push_req_t{"user-push"})
        .packet_name ("PushReq")
        .metadata ("actor-id", user_actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<actor_push_res_t> ();
    if (!user_push || !user_push.value ().pushed
        || user_push.value ().actor_id != user_actor_id) {
        throw std::runtime_error ("SM-D3 user actor push failed");
    }
    auto user_notify = user_wait.get ();
    if (user_notify.actor_id != user_actor_id || user_notify.value != "user-push") {
        throw std::runtime_error ("SM-D3 user push notify mismatch");
    }

    (void) entry.close ();
    (void) user.close ();
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
