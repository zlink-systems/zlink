/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_c4_scenario (const std::string &play_http_endpoint,
                                const std::string &gateway_http_endpoint)
{
    if (play_http_endpoint.empty () || gateway_http_endpoint.empty ()) {
        throw std::runtime_error (
          "playHttpEndpoint and gatewayHttpEndpoint are required for SM-C4");
    }

    constexpr auto subscribed_spot_id = "user:play-a:sm-c4-subscribed";
    constexpr auto unsubscribed_spot_id = "user:play-a:sm-c4-unsubscribed";
    constexpr auto marker = "sm-c4-publish";
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto gateway = zlink::http_client::client_t::create ()
                     .base_url (gateway_http_endpoint)
                     .build ();

    auto subscribed_created_raw =
      play_a.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = subscribed_spot_id})
        .submit_raw ()
        .result ();
    if (!subscribed_created_raw || subscribed_created_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-C4 subscribed spot create failed");
    }
    const auto subscribed_created =
      nlohmann::json::parse (subscribed_created_raw.value ().body).get<create_spot_res_t> ();
    if (subscribed_created.spot_id != subscribed_spot_id
        || subscribed_created.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-C4 subscribed spot was not created on play-a");
    }

    auto unsubscribed_created_raw =
      play_a.post ("/spot/create-alternate")
        .body (create_spot_req_t{.spot_id = unsubscribed_spot_id})
        .submit_raw ()
        .result ();
    if (!unsubscribed_created_raw || unsubscribed_created_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-C4 unsubscribed spot create failed");
    }
    const auto unsubscribed_created =
      nlohmann::json::parse (unsubscribed_created_raw.value ().body).get<create_spot_res_t> ();
    if (unsubscribed_created.spot_id != unsubscribed_spot_id
        || unsubscribed_created.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-C4 unsubscribed spot was not created on play-a");
    }

    auto publish_raw =
      gateway.post ("/spot/publish")
        .body (spot_publish_route_req_t{.spot_id = subscribed_spot_id, .marker = marker})
        .submit_raw ()
        .result ();
    if (!publish_raw || publish_raw.value ().status >= 400) {
        const auto status =
          publish_raw ? std::to_string (publish_raw.value ().status) : std::string ("none");
        const auto body = publish_raw ? publish_raw.value ().body : std::string ("");
        throw std::runtime_error ("SM-C4 gateway publish HTTP failed status=" + status
                                  + " body=" + body);
    }
    const auto publish =
      nlohmann::json::parse (publish_raw.value ().body).get<spot_publish_route_res_t> ();
    if (!publish.accepted) {
        throw std::runtime_error ("SM-C4 gateway publish was not accepted");
    }

    auto observed_raw =
      play_a.post ("/spot/publish/wait")
        .body (spot_publish_route_req_t{.spot_id = subscribed_spot_id, .marker = marker})
        .submit_raw ()
        .result ();
    if (!observed_raw || observed_raw.value ().status >= 400) {
        const auto status =
          observed_raw ? std::to_string (observed_raw.value ().status) : std::string ("none");
        const auto body = observed_raw ? observed_raw.value ().body : std::string ("");
        throw std::runtime_error ("SM-C4 publish wait HTTP failed status=" + status
                                  + " body=" + body);
    }
    const auto observed =
      nlohmann::json::parse (observed_raw.value ().body).get<spot_publish_observe_res_t> ();
    if (!observed.received || observed.spot_id != subscribed_spot_id
        || observed.marker != marker) {
        throw std::runtime_error ("SM-C4 publish wait reply mismatch");
    }
    for (const auto &entry : observed.evidence.entries) {
        if (entry.marker == "MeshMsgReceived" && entry.spot_id == unsubscribed_spot_id
            && entry.value == "evt-sm-c4:sm-c4-publish") {
            throw std::runtime_error ("SM-C4 unsubscribed spot received publish event");
        }
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
