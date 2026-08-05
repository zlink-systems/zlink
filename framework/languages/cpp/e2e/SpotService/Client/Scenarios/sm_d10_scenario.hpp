/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/http_client.hpp>
#include <zlink/stream_connector.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline zlink::stream_connector::connector_t sm_d10_connect_stream (
  const std::string &endpoint,
  std::size_t max_received_messages)
{
    zlink::stream_connector::connector_options_t options;
    options.endpoint = endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    options.max_received_messages = max_received_messages;

    auto stream = zlink::stream_connector::connector_factory_t::create (options);
    auto connected = stream.connect ();
    if (!connected) {
        throw std::runtime_error ("SM-D10 stream connect failed");
    }
    return stream;
}

inline void sm_d10_auth (zlink::stream_connector::connector_t &stream,
                         const std::string &target_node,
                         const std::string &actor_id,
                         const std::string &display_name,
                         const actor_ref_dto_t &actor)
{
    auto auth =
      stream.request (stream_auth_req_t{target_node, actor_id, display_name, actor})
        .packet_name ("StreamAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!auth || auth.value ().actor.actor_id != actor_id
        || auth.value ().session_node_rid.empty ()) {
        const auto detail =
          auth ? "session_node_rid=" + auth.value ().session_node_rid
                   + " actor_id=" + auth.value ().actor.actor_id
               : auth.error () ? auth.error ()->message : "unknown stream error";
        throw std::runtime_error ("SM-D10 auth failed for " + actor_id + ": " + detail);
    }
}

inline join_res_t sm_d10_join_actor (const std::string &play_http_endpoint,
                                     const std::string &key,
                                     const std::string &actor_id,
                                     const std::string &display_name)
{
    auto play = zlink::http_client::client_t::create ().base_url (play_http_endpoint).build ();
    auto joined =
      play.post ("/spot/join")
        .body (join_req_t{.key = key,
                          .actor_id = actor_id,
                          .display_name = display_name,
                          .level = 10,
                          .tags = {"stream", "SM-D10"}})
        .submit_raw ()
        .result ();
    if (!joined || joined.value ().status >= 400) {
        auto reason = std::string ("SM-D10 actor join failed");
        if (joined) {
            reason += ": status=" + std::to_string (joined.value ().status)
                      + " body=" + joined.value ().body;
        }
        throw std::runtime_error (reason);
    }
    return nlohmann::json::parse (joined.value ().body).get<join_res_t> ();
}

inline void run_sm_d10_scenario (const std::string &session_a_stream_endpoint,
                                 const std::string &session_b_stream_endpoint,
                                 const std::string &play_a_http_endpoint,
                                 const std::string &play_b_http_endpoint)
{
    if (session_a_stream_endpoint.empty () || session_b_stream_endpoint.empty ()
        || play_a_http_endpoint.empty () || play_b_http_endpoint.empty ()) {
        throw std::runtime_error (
          "streamEndpoint, alternateStreamEndpoint, playHttpEndpoint, and playBHttpEndpoint are required for SM-D10");
    }

    constexpr auto congested_actor = "actor-sm-d10-congested";
    constexpr auto isolated_actor = "actor-sm-d10-isolated";
    auto congested_join =
      sm_d10_join_actor (play_a_http_endpoint, "sm-d10-congested", congested_actor,
                         "SM-D10 Congested");
    auto isolated_join =
      sm_d10_join_actor (play_b_http_endpoint, "sm-d10-isolated", isolated_actor,
                         "SM-D10 Isolated");
    std::atomic_int congested_push_count{0};

    auto congested = sm_d10_connect_stream (session_a_stream_endpoint, 1);
    congested.on<actor_push_notify_t> (
      "ActorPushNotify", [&congested_push_count] (const actor_push_notify_t &) {
          ++congested_push_count;
          std::this_thread::sleep_for (std::chrono::milliseconds (150));
      });
    sm_d10_auth (congested, "play-a", congested_actor, "SM-D10 Congested",
                 congested_join.actor);

    auto isolated = sm_d10_connect_stream (session_b_stream_endpoint, 1024);
    sm_d10_auth (isolated, "play-b", isolated_actor, "SM-D10 Isolated", isolated_join.actor);

    for (int index = 0; index < 8; ++index) {
        const auto value = "burst-" + std::to_string (index);
        auto reply = congested.request (actor_push_req_t{value})
                       .packet_name ("PushReq")
                       .metadata ("actor-id", congested_actor)
                       .timeout (std::chrono::milliseconds (3000))
                       .submit<actor_push_res_t> ();
        if (!reply || !reply.value ().pushed || reply.value ().actor_id != congested_actor) {
            throw std::runtime_error ("SM-D10 congested push reply mismatch");
        }
    }

    std::this_thread::sleep_for (std::chrono::milliseconds (1200));
    if (congested_push_count.load () >= 8) {
        throw std::runtime_error ("SM-D10 expected bounded received-message queue");
    }
    if (congested.pending_dispatch_count () > 1) {
        throw std::runtime_error ("SM-D10 congested receive queue exceeded configured bound");
    }

    auto after_backpressure = congested.request (actor_ping_req_t{"after-backpressure"})
                                .packet_name ("ActorPingReq")
                                .metadata ("actor-id", congested_actor)
                                .timeout (std::chrono::milliseconds (3000))
                                .submit<actor_ping_res_t> ();
    if (!after_backpressure || after_backpressure.value ().actor_id != congested_actor
        || after_backpressure.value ().value != "after-backpressure") {
        throw std::runtime_error ("SM-D10 congested session stopped routing");
    }

    auto isolated_wait =
      isolated.wait_for<actor_push_notify_t> (std::chrono::milliseconds (10000))
        .to_future ("SM-D10 isolated push notify missing");
    auto isolated_reply = isolated.request (actor_push_req_t{"isolated-push"})
                            .packet_name ("PushReq")
                            .metadata ("actor-id", isolated_actor)
                            .timeout (std::chrono::milliseconds (3000))
                            .submit<actor_push_res_t> ();
    if (!isolated_reply || !isolated_reply.value ().pushed
        || isolated_reply.value ().actor_id != isolated_actor) {
        throw std::runtime_error ("SM-D10 isolated push reply mismatch");
    }
    const auto isolated_notify = isolated_wait.get ();
    if (isolated_notify.actor_id != isolated_actor
        || isolated_notify.value != "isolated-push") {
        throw std::runtime_error ("SM-D10 isolated push notify mismatch");
    }

    (void) congested.close ();
    (void) isolated.close ();
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
