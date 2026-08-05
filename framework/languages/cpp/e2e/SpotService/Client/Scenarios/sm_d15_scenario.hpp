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

inline void run_sm_d15_scenario (const std::string &play_http_endpoint,
                                 const std::string &gateway_http_endpoint,
                                 const std::string &session_stream_endpoint)
{
    if (play_http_endpoint.empty () || gateway_http_endpoint.empty ()
        || session_stream_endpoint.empty ()) {
        throw std::runtime_error ("SM-D15 requires play-a, gateway, and stream endpoints");
    }
    constexpr auto actor_id = "actor-sm-d15-cpp";
    constexpr auto marker = "sm-d15-cpp";

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (10000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto stream = zlink::stream_connector::connector_factory_t::create (options);
    if (!stream.connect ()) {
        throw std::runtime_error ("SM-D15 stream connect failed");
    }
    auto auth = stream.request (stream_ensure_auth_req_t{"play-a", actor_id, "SM-D15"})
                  .packet_name ("StreamEnsureAuthReq")
                  .timeout (std::chrono::milliseconds (10000))
                  .submit<stream_auth_res_t> ();
    if (!auth) {
        throw std::runtime_error ("SM-D15 stream auth failed");
    }
    auto bound = stream.request (actor_ping_req_t{"d15-bind-probe"})
                   .packet_name ("ActorPingReq")
                   .timeout (std::chrono::milliseconds (10000))
                   .submit<actor_ping_res_t> ();
    if (!bound || bound.value ().actor_id != actor_id) {
        throw std::runtime_error ("SM-D15 bind probe failed");
    }

    auto wait =
      stream.wait_for<actor_push_notify_t> (std::chrono::milliseconds (10000))
        .to_future ("SM-D15 push notify missing");
    auto gateway = zlink::http_client::client_t::create ().base_url (gateway_http_endpoint).build ();
    auto pushed =
      gateway.post ("/actor/push")
        .body (actor_push_by_actor_req_t{.actor_id = actor_id, .value = marker})
        .submit_raw ()
        .result ();
    if (!pushed) {
        throw std::runtime_error (
          std::string ("SM-D15 gateway push HTTP failed: ")
          + (pushed.error () ? pushed.error ()->what () : "unknown HTTP client error"));
    }
    if (pushed.value ().status >= 400) {
        throw std::runtime_error ("SM-D15 gateway push HTTP failed: status="
                                 + std::to_string (pushed.value ().status)
                                 + " body=" + pushed.value ().body);
    }
    const auto result =
      nlohmann::json::parse (pushed.value ().body).get<actor_push_by_actor_res_t> ();
    if (!result.delivered || result.actor_id != actor_id) {
        throw std::runtime_error ("SM-D15 gateway actor push failed: error_kind="
                                 + result.error_kind + " actor_id=" + result.actor_id);
    }
    auto play_a = zlink::http_client::client_t::create ().base_url (play_http_endpoint).build ();
    auto play_evidence =
      play_a.post ("/evidence/wait")
        .body (evidence_wait_req_t{
          .contains_all = {"ActorPushedSession", actor_id, marker},
          .timeout_milliseconds = 10000})
        .submit_raw ()
        .result ();
    auto gateway_evidence =
      gateway.post ("/evidence/wait")
        .body (evidence_wait_req_t{
          .contains_all = {"ActorPushDelivered", actor_id, marker},
          .timeout_milliseconds = 10000})
        .submit_raw ()
        .result ();
    if (!play_evidence || play_evidence.value ().status >= 400 || !gateway_evidence
        || gateway_evidence.value ().status >= 400) {
        throw std::runtime_error ("SM-D15 push evidence missing");
    }
    const auto notify = wait.get ();
    if (notify.actor_id != actor_id || notify.value != marker) {
        throw std::runtime_error ("SM-D15 gateway actor push result mismatch");
    }
    (void) stream.close ();
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
