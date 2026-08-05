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

inline void run_sm_d4_scenario (const std::string &session_stream_endpoint)
{
    if (session_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-D4");
    }

    constexpr auto first_actor_id = "actor-sm-d4-x";
    constexpr auto second_actor_id = "actor-sm-d4-y";

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto stream = zlink::stream_connector::connector_factory_t::create (options);

    auto connected = stream.connect ();
    if (!connected) {
        throw std::runtime_error ("SM-D4 stream connect failed");
    }

    auto first_auth =
      stream.request (stream_ensure_auth_req_t{"play-a", first_actor_id, "SM-D4 X"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    auto second_auth =
      stream.request (stream_ensure_auth_req_t{"play-a", second_actor_id, "SM-D4 Y"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!first_auth || !second_auth || first_auth.value ().actor.actor_id != first_actor_id
        || second_auth.value ().actor.actor_id != second_actor_id) {
        throw std::runtime_error ("SM-D4 multi-bind auth failed");
    }

    auto first_ping =
      stream.request (actor_ping_req_t{"to-x"})
        .packet_name ("ActorPingReq")
        .metadata ("actor-id", first_actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<actor_ping_res_t> ();
    auto second_ping =
      stream.request (actor_ping_req_t{"to-y"})
        .packet_name ("ActorPingReq")
        .metadata ("actor-id", second_actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<actor_ping_res_t> ();
    if (!first_ping || first_ping.value ().actor_id != first_actor_id
        || first_ping.value ().value != "to-x" || !second_ping
        || second_ping.value ().actor_id != second_actor_id
        || second_ping.value ().value != "to-y") {
        throw std::runtime_error ("SM-D4 actor-id selected relay mismatch");
    }

    auto first_wait = stream.wait_for<actor_push_notify_t> (std::chrono::milliseconds (10000))
                        .where (&actor_push_notify_t::actor_id, first_actor_id)
                        .to_future ("SM-D4 first actor push notify missing");
    auto first_push =
      stream.request (actor_push_req_t{"push-x"})
        .packet_name ("PushReq")
        .metadata ("actor-id", first_actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<actor_push_res_t> ();
    if (!first_push || !first_push.value ().pushed
        || first_push.value ().actor_id != first_actor_id) {
        throw std::runtime_error ("SM-D4 first actor push failed");
    }
    auto first_notify = first_wait.get ();
    if (first_notify.value != "push-x") {
        throw std::runtime_error ("SM-D4 first actor push notify mismatch");
    }

    auto second_wait = stream.wait_for<actor_push_notify_t> (std::chrono::milliseconds (10000))
                         .where (&actor_push_notify_t::actor_id, second_actor_id)
                         .to_future ("SM-D4 second actor push notify missing");
    auto second_push =
      stream.request (actor_push_req_t{"push-y"})
        .packet_name ("PushReq")
        .metadata ("actor-id", second_actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<actor_push_res_t> ();
    if (!second_push || !second_push.value ().pushed
        || second_push.value ().actor_id != second_actor_id) {
        throw std::runtime_error ("SM-D4 second actor push failed");
    }
    auto second_notify = second_wait.get ();
    if (second_notify.value != "push-y") {
        throw std::runtime_error ("SM-D4 second actor push notify mismatch");
    }

    auto missing_actor_id =
      stream.request (actor_ping_req_t{"missing-actor-id"})
        .packet_name ("ActorPingReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<actor_ping_res_t> ();
    if (missing_actor_id) {
        throw std::runtime_error ("SM-D4 actor-id-less request unexpectedly succeeded");
    }

    (void) stream.close ();
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
