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

inline void run_sm_c1_scenario (const std::string &play_http_endpoint,
                                const std::string &play_b_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-C1");
    }
    if (play_b_http_endpoint.empty ()) {
        throw std::runtime_error ("playBHttpEndpoint is required for SM-C1");
    }

    constexpr auto spot_id = "user:play-a:sm-c1-channel";
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto external_channel = zlink::http_client::client_t::create ()
                              .base_url (play_b_http_endpoint)
                              .build ();

    auto created_raw =
      play_a.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = spot_id})
        .submit_raw ()
        .result ();
    if (!created_raw || created_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-C1 spot create failed");
    }
    const auto created =
      nlohmann::json::parse (created_raw.value ().body).get<create_spot_res_t> ();
    if (created.spot_id != spot_id || created.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-C1 spot was not created on play-a");
    }

    auto request_raw =
      external_channel.post ("/spot/direct")
        .body (direct_spot_route_req_t{
          .target_node_rid = "play-a",
          .spot_id = spot_id,
          .value = "sm-c1-request",
          .source_actor_id = "sm-c1-client"})
        .submit_raw ()
        .result ();
    if (!request_raw || request_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-C1 channel-to-spot request failed");
    }
    const auto via_channel =
      nlohmann::json::parse (request_raw.value ().body).get<direct_spot_res_t> ();
    if (via_channel.spot_id != spot_id || via_channel.owner_node_rid != "play-a"
        || via_channel.value != "sm-c1-request:reply") {
        throw std::runtime_error ("SM-C1 channel-to-spot request reply mismatch");
    }

    auto command_raw =
      external_channel.post ("/spot/state/command")
        .body (spot_state_command_route_req_t{
          .target_node_rid = "play-a", .spot_id = spot_id, .marker = "sm-c1-send"})
        .submit_raw ()
        .result ();
    if (!command_raw || command_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-C1 channel-to-spot command failed");
    }
    const auto command =
      nlohmann::json::parse (command_raw.value ().body).get<spot_state_command_route_res_t> ();
    if (!command.accepted) {
        throw std::runtime_error ("SM-C1 channel-to-spot command was not accepted");
    }

    const auto publish_marker = [&] (const std::string &marker) {
        return external_channel.post ("/spot/publish")
          .body (spot_publish_route_req_t{.spot_id = spot_id, .marker = marker})
          .submit_raw ()
          .result ();
    };
    bool mesh_ready = false;
    for (int attempt = 0; attempt < 100 && !mesh_ready; ++attempt) {
        auto warmup = publish_marker ("sm-c1-warmup");
        if (!warmup || warmup.value ().status >= 400) {
            throw std::runtime_error ("SM-C1 mesh readiness publish failed");
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        const auto evidence = play_a.get ("/evidence").submit<evidence_snapshot_t> ().result ().value ().body;
        for (const auto &entry : evidence.entries) {
            if (entry.marker == "MeshMsgReceived" && entry.spot_id == spot_id
                && entry.value == "evt-sm-c1:sm-c1-warmup") {
                mesh_ready = true;
                break;
            }
        }
    }
    if (!mesh_ready) {
        throw std::runtime_error ("SM-C1 mesh subscription did not become ready");
    }

    auto publish_raw = publish_marker ("sm-c1-publish");
    if (!publish_raw || publish_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-C1 channel-to-spot publish failed");
    }
    const auto publish =
      nlohmann::json::parse (publish_raw.value ().body).get<spot_publish_route_res_t> ();
    if (!publish.accepted) {
        throw std::runtime_error ("SM-C1 channel-to-spot publish was not accepted");
    }
    auto publish_observed =
      play_a.post ("/evidence/wait")
        .body (evidence_wait_req_t{
          .contains_all = {"MeshMsgReceived", spot_id, "evt-sm-c1:sm-c1-publish"}})
        .submit_raw ()
        .result ();
    if (!publish_observed || publish_observed.value ().status >= 400) {
        throw std::runtime_error ("SM-C1 published message was not observed by the target spot");
    }

    auto timeout_raw =
      external_channel.post ("/spot/slow/request")
        .body (spot_slow_route_req_t{.target_node_rid = "play-a",
                                     .spot_id = spot_id,
                                     .value = "sm-c1-timeout",
                                     .timeout_ms = 50})
        .submit_raw ()
        .result ();
    if (!timeout_raw || timeout_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-C1 slow route HTTP failed");
    }
    const auto timeout =
      nlohmann::json::parse (timeout_raw.value ().body).get<spot_slow_route_res_t> ();
    if (!timeout.timed_out) {
        throw std::runtime_error ("SM-C1 expected slow channel-to-spot request to time out");
    }

    auto after_timeout_raw =
      external_channel.post ("/spot/direct")
        .body (direct_spot_route_req_t{
          .target_node_rid = "play-a",
          .spot_id = spot_id,
          .value = "sm-c1-after-timeout",
          .source_actor_id = "sm-c1-client"})
        .submit_raw ()
        .result ();
    if (!after_timeout_raw || after_timeout_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-C1 post-timeout request failed");
    }
    const auto after_timeout =
      nlohmann::json::parse (after_timeout_raw.value ().body).get<direct_spot_res_t> ();
    if (after_timeout.spot_id != spot_id || after_timeout.owner_node_rid != "play-a"
        || after_timeout.value != "sm-c1-after-timeout:reply") {
        throw std::runtime_error ("SM-C1 post-timeout request reply mismatch");
    }

    auto missing_raw =
      external_channel.post ("/spot/missing-route")
        .body (spot_missing_route_req_t{
          .target_node_rid = "play-a", .spot_id = spot_id, .value = "sm-c1-missing"})
        .submit_raw ()
        .result ();
    if (!missing_raw || missing_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-C1 missing route HTTP failed");
    }
    const auto missing =
      nlohmann::json::parse (missing_raw.value ().body).get<spot_missing_route_res_t> ();
    if (!missing.request_failed || !missing.command_sent) {
        throw std::runtime_error ("SM-C1 missing spot route negative mismatch");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
