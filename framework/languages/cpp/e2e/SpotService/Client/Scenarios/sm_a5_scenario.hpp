/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void ensure_sm_a5_http_success (const zlink::http_client::raw_http_response_t &response,
                                       const std::string &operation)
{
    if (response.status >= 400) {
        throw std::runtime_error (operation + " HTTP status " + std::to_string (response.status)
                                  + ": " + response.body);
    }
}

inline void run_sm_a5_scenario (const std::string &play_a_http_endpoint,
                                const std::string &play_b_http_endpoint)
{
    if (play_a_http_endpoint.empty () || play_b_http_endpoint.empty ()) {
        throw std::runtime_error (
          "playHttpEndpoint and playBHttpEndpoint are required "
          "for SM-A5");
    }

    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_a_http_endpoint)
                    .build ();
    auto play_b = zlink::http_client::client_t::create ()
                    .base_url (play_b_http_endpoint)
                    .build ();

    const auto key = std::string ("sm-a5-stage");
    const auto spot_id = user_spot_id_for_key (key);

    auto created =
      play_a.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = spot_id})
        .submit_raw ()
        .result ();
    if (!created) {
        throw std::runtime_error (created.error () ? created.error ()->what ()
                                                   : "SM-A5 create HTTP failed");
    }
    ensure_sm_a5_http_success (created.value (), "SM-A5 create");
    const auto create_reply =
      nlohmann::json::parse (created.value ().body).get<create_spot_res_t> ();
    if (create_reply.spot_id != spot_id || create_reply.owner_node_rid != "play-a"
        || !create_reply.created) {
        throw std::runtime_error ("SM-A5 create reply mismatch");
    }

    auto route_ready =
      play_b.post ("/spot/state/request")
        .body (spot_state_route_req_t{
          .spot_id = spot_id, .state = state_req_t{.op = "add", .amount = 0}})
        .submit_raw ()
        .result ();
    if (!route_ready) {
        throw std::runtime_error (route_ready.error () ? route_ready.error ()->what ()
                                                       : "SM-A5 route-ready HTTP failed");
    }
    ensure_sm_a5_http_success (route_ready.value (), "SM-A5 route-ready");
    const auto route_reply =
      nlohmann::json::parse (route_ready.value ().body).get<state_res_t> ();
    if (route_reply.spot_id != spot_id || route_reply.owner_node_rid != "play-a"
        || route_reply.value != 0) {
        throw std::runtime_error ("SM-A5 route-ready reply mismatch");
    }

    auto stage_probe =
      play_b.post ("/spot/stage/request")
        .body (spot_stage_probe_req_t{.spot_id = spot_id,
                                      .marker = "sm-a5-stage",
                                      .delta = 9})
        .submit_raw ()
        .result ();
    if (!stage_probe) {
        throw std::runtime_error (stage_probe.error () ? stage_probe.error ()->what ()
                                                       : "SM-A5 stage probe HTTP failed");
    }
    ensure_sm_a5_http_success (stage_probe.value (), "SM-A5 stage probe");
    const auto probe_reply =
      nlohmann::json::parse (stage_probe.value ().body).get<state_res_t> ();
    if (probe_reply.spot_id != spot_id || probe_reply.owner_node_rid != "play-a"
        || probe_reply.value != 9) {
        throw std::runtime_error ("SM-A5 stage probe reply mismatch");
    }

    auto timer_started =
      play_b.post ("/spot/stage/timer")
        .body (spot_stage_timer_req_t{.spot_id = spot_id,
                                      .name = "sm-a5-stage-timer",
                                      .period_ms = 50})
        .submit_raw ()
        .result ();
    if (!timer_started) {
        throw std::runtime_error (timer_started.error () ? timer_started.error ()->what ()
                                                         : "SM-A5 stage timer HTTP failed");
    }
    ensure_sm_a5_http_success (timer_started.value (), "SM-A5 stage timer");
    const auto timer_reply =
      nlohmann::json::parse (timer_started.value ().body).get<spot_stage_timer_res_t> ();
    if (timer_reply.spot_id != spot_id || timer_reply.name != "sm-a5-stage-timer"
        || !timer_reply.started) {
        throw std::runtime_error ("SM-A5 stage timer reply mismatch");
    }

    auto evidence =
      play_a.post ("/evidence/wait")
        .body (evidence_wait_req_t{
          .contains_all = {"SpotInitialized", spot_id, "StageRequest", "sm-a5-stage:9",
                           "StageTimer", "sm-a5-stage-timer:1"},
          .timeout_milliseconds = 10000})
        .submit_raw ()
        .result ();
    if (!evidence) {
        throw std::runtime_error (evidence.error () ? evidence.error ()->what ()
                                                   : "SM-A5 evidence wait HTTP failed");
    }
    ensure_sm_a5_http_success (evidence.value (), "SM-A5 evidence wait");

    auto closed =
      play_a.post ("/spot/close")
        .body (close_spot_req_t{.key = key})
        .submit_raw ()
        .result ();
    if (!closed) {
        throw std::runtime_error (closed.error () ? closed.error ()->what ()
                                                 : "SM-A5 close HTTP failed");
    }
    ensure_sm_a5_http_success (closed.value (), "SM-A5 close");
    const auto close_reply =
      nlohmann::json::parse (closed.value ().body).get<close_spot_res_t> ();
    if (close_reply.spot_id != spot_id || !close_reply.closed) {
        throw std::runtime_error ("SM-A5 close reply mismatch");
    }

    auto closing_evidence =
      play_a.post ("/evidence/wait")
        .body (evidence_wait_req_t{.contains_all = {"SpotClosing", spot_id},
                                   .timeout_milliseconds = 5000})
        .submit_raw ()
        .result ();
    if (!closing_evidence) {
        throw std::runtime_error (closing_evidence.error () ? closing_evidence.error ()->what ()
                                                           : "SM-A5 closing evidence wait failed");
    }
    ensure_sm_a5_http_success (closing_evidence.value (), "SM-A5 closing evidence wait");
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
