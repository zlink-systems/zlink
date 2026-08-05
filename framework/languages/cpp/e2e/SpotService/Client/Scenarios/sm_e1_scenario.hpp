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

inline void run_sm_e1_scenario (const std::string &play_http_endpoint,
                                const std::string &play_b_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-E1");
    }
    if (play_b_http_endpoint.empty ()) {
        throw std::runtime_error ("playBHttpEndpoint is required for SM-E1");
    }

    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto play_b = zlink::http_client::client_t::create ()
                    .base_url (play_b_http_endpoint)
                    .build ();
    constexpr auto spot_id = "user:play-b:sm-e1-missing";
    auto created =
      play_b.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = spot_id})
        .submit_raw ()
        .result ();
    if (!created) {
        throw std::runtime_error (created.error () ? created.error ()->what ()
                                                   : "SM-E1 create spot HTTP failed");
    }
    if (created.value ().status >= 400) {
        throw std::runtime_error ("SM-E1 create spot HTTP status "
                                  + std::to_string (created.value ().status) + ": "
                                  + created.value ().body);
    }
    const auto create_reply = nlohmann::json::parse (created.value ().body).get<create_spot_res_t> ();
    if (!create_reply.created || create_reply.spot_id != spot_id
        || create_reply.owner_node_rid != "play-b") {
        throw std::runtime_error ("SM-E1 create spot reply mismatch");
    }

    auto missing =
      play_a.post ("/spot/missing-route")
        .body (spot_missing_route_req_t{.target_node_rid = "play-b",
                                        .spot_id = spot_id,
                                        .value = "missing-route"})
        .submit_raw ()
        .result ();
    if (!missing) {
        throw std::runtime_error (missing.error () ? missing.error ()->what ()
                                                   : "SM-E1 missing route HTTP failed");
    }
    if (missing.value ().status >= 400) {
        throw std::runtime_error ("SM-E1 missing route HTTP status "
                                  + std::to_string (missing.value ().status) + ": "
                                  + missing.value ().body);
    }
    const auto missing_reply =
      nlohmann::json::parse (missing.value ().body).get<spot_missing_route_res_t> ();
    if (!missing_reply.request_failed) {
        throw std::runtime_error ("SM-E1 missing route reply mismatch");
    }

    auto request_missing_handler = [&] {
        return play_a.post ("/spot/missing-handler/request")
          .body (spot_missing_handler_req_t{.spot_id = spot_id})
          .submit_raw ()
          .result ();
    };
    auto missing_handler_request = request_missing_handler ();
    const auto location_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (missing_handler_request && missing_handler_request.value ().status == 500
           && missing_handler_request.value ().body.find ("no live location row")
                != std::string::npos
           && std::chrono::steady_clock::now () < location_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
        missing_handler_request = request_missing_handler ();
    }
    if (!missing_handler_request || missing_handler_request.value ().status >= 400) {
        throw std::runtime_error (
          !missing_handler_request
            ? (missing_handler_request.error () ? missing_handler_request.error ()->what ()
                                                 : "SM-E1 missing handler request endpoint failed")
            : "SM-E1 missing handler request HTTP status "
                + std::to_string (missing_handler_request.value ().status) + ": "
                + missing_handler_request.value ().body);
    }
    const auto missing_handler_request_reply =
      nlohmann::json::parse (missing_handler_request.value ().body)
        .get<spot_missing_handler_res_t> ();
    if (missing_handler_request_reply.spot_id != spot_id
        || !missing_handler_request_reply.failed) {
        throw std::runtime_error ("SM-E1 missing handler request reply mismatch");
    }

    auto missing_handler_command =
      play_a.post ("/spot/missing-handler/command")
        .body (spot_missing_command_req_t{.spot_id = spot_id, .marker = "sm-e1-missing-command"})
        .submit_raw ()
        .result ();
    if (!missing_handler_command || missing_handler_command.value ().status >= 400) {
        throw std::runtime_error ("SM-E1 missing handler command endpoint failed");
    }
    const auto missing_handler_command_reply =
      nlohmann::json::parse (missing_handler_command.value ().body)
        .get<spot_missing_command_res_t> ();
    if (missing_handler_command_reply.spot_id != spot_id
        || missing_handler_command_reply.marker != "sm-e1-missing-command"
        || !missing_handler_command_reply.sent) {
        throw std::runtime_error ("SM-E1 missing handler command reply mismatch");
    }

    const auto absent_spot_id = user_spot_id_for_key ("b-sm-e1-missing-target");
    auto missing_target =
      play_a.post ("/spot/missing-target/request")
        .body (spot_missing_target_req_t{.spot_id = absent_spot_id})
        .submit_raw ()
        .result ();
    if (!missing_target || missing_target.value ().status >= 400) {
        throw std::runtime_error ("SM-E1 missing target endpoint failed");
    }
    const auto missing_target_reply =
      nlohmann::json::parse (missing_target.value ().body).get<spot_missing_target_res_t> ();
    if (missing_target_reply.spot_id != absent_spot_id || !missing_target_reply.failed) {
        throw std::runtime_error ("SM-E1 missing target reply mismatch");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
