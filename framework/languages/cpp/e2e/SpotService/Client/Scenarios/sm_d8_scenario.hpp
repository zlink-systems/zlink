/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/stream_connector.hpp>

#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_d8_scenario (const std::string &session_stream_endpoint)
{
    if (session_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-D8");
    }

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;

    constexpr auto actor_id = "actor-sm-d8-reconnect";

    auto first = zlink::stream_connector::connector_factory_t::create (options);
    auto first_connected = first.connect ();
    if (!first_connected) {
        throw std::runtime_error ("SM-D8 first stream connect failed");
    }
    auto first_auth =
      first.request (stream_ensure_auth_req_t{"play-a", actor_id, "SM-D8 Reconnect"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!first_auth || first_auth.value ().actor.actor_id != actor_id
        || first_auth.value ().session_node_rid != "session-a") {
        throw std::runtime_error ("SM-D8 first auth reply mismatch");
    }

    std::promise<zlink::stream_connector::result_t<actor_ping_res_t>> pending_promise;
    auto pending = pending_promise.get_future ();
    first.request (slow_actor_ping_req_t{"before-disconnect", 1000})
      .packet_name ("SlowActorPingReq")
      .timeout (std::chrono::milliseconds (10000))
      .submit<actor_ping_res_t> (
        [&pending_promise] (zlink::stream_connector::result_t<actor_ping_res_t> result) mutable {
            pending_promise.set_value (std::move (result));
        });

    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    auto closed = first.close ();
    if (!closed) {
        throw std::runtime_error ("SM-D8 first stream close failed");
    }
    if (pending.wait_for (std::chrono::milliseconds (3000)) != std::future_status::ready) {
        throw std::runtime_error ("SM-D8 pending request did not complete after disconnect");
    }
    auto pending_result = pending.get ();
    if (pending_result) {
        throw std::runtime_error ("SM-D8 pending request unexpectedly succeeded after disconnect");
    }

    std::this_thread::sleep_for (std::chrono::milliseconds (1200));

    auto second = zlink::stream_connector::connector_factory_t::create (options);
    auto second_connected = second.connect ();
    if (!second_connected) {
        throw std::runtime_error ("SM-D8 second stream connect failed");
    }
    auto second_auth =
      second.request (stream_ensure_auth_req_t{"play-a", actor_id, "SM-D8 Reconnect"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!second_auth || second_auth.value ().actor.actor_id != actor_id
        || second_auth.value ().session_node_rid != "session-a") {
        throw std::runtime_error ("SM-D8 second auth reply mismatch");
    }

    auto resumed =
      second.request (actor_ping_req_t{"after-reconnect"})
        .packet_name ("ActorPingReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<actor_ping_res_t> ();
    if (!resumed || resumed.value ().actor_id != actor_id
        || resumed.value ().node_rid != "play-a"
        || resumed.value ().value != "after-reconnect") {
        throw std::runtime_error ("SM-D8 reconnected dispatch mismatch");
    }

    (void) second.close ();
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
