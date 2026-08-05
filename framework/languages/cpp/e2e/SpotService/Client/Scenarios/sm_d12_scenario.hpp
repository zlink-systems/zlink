/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/stream_connector.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_d12_scenario (const std::string &session_a_stream_endpoint,
                                 const std::string &session_b_stream_endpoint)
{
    if (session_a_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-D12");
    }
    if (session_b_stream_endpoint.empty ()) {
        throw std::runtime_error ("alternateStreamEndpoint is required for SM-D12");
    }

    constexpr auto actor_id = "actor-sm-d12-transfer";

    zlink::stream_connector::connector_options_t first_options;
    first_options.endpoint = session_a_stream_endpoint;
    first_options.connect_timeout = std::chrono::milliseconds (10000);
    first_options.request_timeout = std::chrono::milliseconds (10000);
    first_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;

    auto first = zlink::stream_connector::connector_factory_t::create (first_options);
    auto first_connected = first.connect ();
    if (!first_connected) {
        throw std::runtime_error ("SM-D12 first stream connect failed");
    }

    auto first_auth =
      first.request (stream_ensure_auth_req_t{"play-a", actor_id, "SM-D12 Transfer"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (10000))
        .submit<stream_auth_res_t> ();
    if (!first_auth || first_auth.value ().actor.actor_id != actor_id
        || first_auth.value ().session_node_rid != "session-a") {
        const auto detail =
          first_auth ? "session_node_rid=" + first_auth.value ().session_node_rid
                         + " actor_id=" + first_auth.value ().actor.actor_id
                     : first_auth.error () ? first_auth.error ()->message
                                           : "unknown stream error";
        throw std::runtime_error ("SM-D12 first stream auth failed: " + detail);
    }

    auto first_ping =
      first.request (actor_ping_req_t{"before-transfer"})
        .packet_name ("ActorPingReq")
        .timeout (std::chrono::milliseconds (10000))
        .submit<actor_ping_res_t> ();
    if (!first_ping || first_ping.value ().actor_id != actor_id
        || first_ping.value ().node_rid != "play-a" || first_ping.value ().seen != 1) {
        throw std::runtime_error ("SM-D12 first actor state mismatch");
    }

    (void) first.close ();

    zlink::stream_connector::connector_options_t second_options;
    second_options.endpoint = session_b_stream_endpoint;
    second_options.connect_timeout = std::chrono::milliseconds (10000);
    second_options.request_timeout = std::chrono::milliseconds (10000);
    second_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;

    auto second = zlink::stream_connector::connector_factory_t::create (second_options);
    auto second_connected = second.connect ();
    if (!second_connected) {
        throw std::runtime_error ("SM-D12 second stream connect failed");
    }

    auto second_auth =
      second.request (stream_ensure_auth_req_t{"play-a", actor_id, "SM-D12 Transfer"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (10000))
        .submit<stream_auth_res_t> ();
    if (!second_auth || second_auth.value ().actor.actor_id != actor_id
        || second_auth.value ().session_node_rid != "session-b") {
        const auto detail =
          second_auth ? "session_node_rid=" + second_auth.value ().session_node_rid
                          + " actor_id=" + second_auth.value ().actor.actor_id
                      : second_auth.error () ? second_auth.error ()->message
                                             : "unknown stream error";
        throw std::runtime_error ("SM-D12 second stream auth failed: " + detail);
    }

    auto snapshot =
      second.request (snapshot_req_t{actor_id})
        .packet_name ("SnapshotReq")
        .timeout (std::chrono::milliseconds (10000))
        .submit<snapshot_res_t> ();
    if (!snapshot || snapshot.value ().actor_id != actor_id || snapshot.value ().seen != 1) {
        const auto detail =
          snapshot ? "actor_id=" + snapshot.value ().actor_id
                       + " seen=" + std::to_string (snapshot.value ().seen)
                   : snapshot.error () ? snapshot.error ()->message : "unknown stream error";
        throw std::runtime_error ("SM-D12 snapshot mismatch: " + detail);
    }

    auto notify_wait =
      second.wait_for<actor_push_notify_t> (std::chrono::milliseconds (10000))
        .to_future ("SM-D12 push notify missing");
    auto pushed =
      second.request (actor_push_req_t{"after-transfer"})
        .packet_name ("PushReq")
        .timeout (std::chrono::milliseconds (10000))
        .submit<actor_push_res_t> ();
    if (!pushed || !pushed.value ().pushed || pushed.value ().actor_id != actor_id
        || pushed.value ().seen != 2) {
        throw std::runtime_error ("SM-D12 push request failed");
    }
    auto notify = notify_wait.get ();
    if (notify.actor_id != actor_id || notify.value != "after-transfer" || notify.seen != 2) {
        throw std::runtime_error ("SM-D12 push notify mismatch");
    }

    (void) second.close ();
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
