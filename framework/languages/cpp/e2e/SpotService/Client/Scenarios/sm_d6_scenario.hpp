/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/http_client.hpp>
#include <zlink/stream_connector.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_d6_scenario (const std::string &session_stream_endpoint,
                                const std::string &alternate_stream_endpoint,
                                const std::string &play_http_endpoint)
{
    if (session_stream_endpoint.empty () || alternate_stream_endpoint.empty ()
        || play_http_endpoint.empty ()) {
        throw std::runtime_error (
          "streamEndpoint, alternateStreamEndpoint, and playHttpEndpoint are required for SM-D6");
    }

    constexpr auto actor_id = "actor-sm-d6";
    constexpr auto shadow_actor_id = "actor-sm-d6-shadow";

    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    zlink::stream_connector::connector_options_t bound_options;
    bound_options.endpoint = session_stream_endpoint;
    bound_options.connect_timeout = std::chrono::milliseconds (3000);
    bound_options.request_timeout = std::chrono::milliseconds (3000);
    bound_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto bound = zlink::stream_connector::connector_factory_t::create (bound_options);
    auto bound_connected = bound.connect ();
    if (!bound_connected) {
        throw std::runtime_error ("SM-D6 bound stream connect failed");
    }
    auto bound_auth =
      bound.request (stream_ensure_auth_req_t{"play-a", actor_id, "SM-D6 Bound"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!bound_auth) {
        throw std::runtime_error ("SM-D6 bound stream auth failed");
    }
    auto routed =
      bound.request (actor_ping_req_t{"sm-d6-route"})
        .packet_name ("ActorPingReq")
        .metadata ("actor-id", actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<actor_ping_res_t> ();
    if (!routed || routed.value ().actor_id != actor_id) {
        throw std::runtime_error ("SM-D6 route establishment failed");
    }

    zlink::stream_connector::connector_options_t unbound_options;
    unbound_options.endpoint = alternate_stream_endpoint;
    unbound_options.connect_timeout = std::chrono::milliseconds (3000);
    unbound_options.request_timeout = std::chrono::milliseconds (3000);
    unbound_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto unbound = zlink::stream_connector::connector_factory_t::create (unbound_options);
    auto unbound_connected = unbound.connect ();
    if (!unbound_connected) {
        throw std::runtime_error ("SM-D6 unbound stream connect failed");
    }
    auto unbound_auth =
      unbound.request (stream_ensure_auth_req_t{"play-b", shadow_actor_id, "SM-D6 Shadow"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!unbound_auth) {
        throw std::runtime_error ("SM-D6 unbound stream auth failed");
    }

    auto bound_wait =
      bound.wait_for<actor_push_notify_t> (std::chrono::milliseconds (10000))
        .to_future ("SM-D6 bound push notify missing");
    auto unbound_wait =
      unbound.wait_for<actor_push_notify_t> (std::chrono::milliseconds (500));
    auto pushed =
      play_a.post ("/spot/push-bound-session")
        .body (bound_session_push_req_t{.actor_id = actor_id,
                                        .push = actor_push_req_t{"push-bound-only"}})
        .submit_raw ()
        .result ();
    if (!pushed || pushed.value ().status >= 400) {
        throw std::runtime_error (
          "SM-D6 push trigger failed: status="
          + std::to_string (pushed ? pushed.value ().status : 0)
          + " body=" + (pushed ? pushed.value ().body : "<transport failure>"));
    }
    const auto pushed_reply = nlohmann::json::parse (pushed.value ().body).get<actor_push_res_t> ();
    if (!pushed_reply.pushed || pushed_reply.actor_id != actor_id) {
        throw std::runtime_error ("SM-D6 push trigger reply mismatch");
    }

    auto notify = bound_wait.get ();
    if (notify.actor_id != actor_id || notify.value != "push-bound-only") {
        throw std::runtime_error ("SM-D6 bound push notify mismatch");
    }
    auto leaked = unbound_wait.submit ();
    if (leaked) {
        throw std::runtime_error ("SM-D6 unbound session received push");
    }

    std::mutex close_gate;
    std::condition_variable close_changed;
    std::optional<std::chrono::steady_clock::time_point> old_disconnected;
    std::optional<std::chrono::steady_clock::time_point> duplicate_received;
    std::optional<std::string> duplicate_payload;
    bound.on<zlink::stream_connector::packet_t> (
      "ActorBindingReplacedNotify",
      [&] (const zlink::stream_connector::packet_t &packet) {
          const std::lock_guard lock (close_gate);
          duplicate_payload = packet.payload.to_string ();
          duplicate_received = std::chrono::steady_clock::now ();
          close_changed.notify_all ();
      });
    bound.on_connection_state_changed (
      [&] (const zlink::stream_connector::connection_state_changed_t &event) {
          if (event.current
                == zlink::stream_connector::connection_state_t::disconnected
              || event.current
                   == zlink::stream_connector::connection_state_t::closed) {
              const std::lock_guard lock (close_gate);
              if (!old_disconnected)
                  old_disconnected = std::chrono::steady_clock::now ();
              close_changed.notify_all ();
          }
      });
    auto replacement_auth =
      unbound
        .request (stream_auth_req_t{
          "play-a", actor_id, "SM-D6 Replacement", bound_auth.value ().actor})
        .packet_name ("StreamAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!replacement_auth || replacement_auth.value ().actor.actor_id != actor_id) {
        throw std::runtime_error ("SM-D6 replacement stream auth failed");
    }
    if (!bound.is_connected ()) {
        throw std::runtime_error (
          "SM-D6 replacement bind waited for the retired session close");
    }
    {
        std::unique_lock lock (close_gate);
        if (!close_changed.wait_for (
              lock, std::chrono::seconds (3),
              [&] { return duplicate_payload.has_value (); })) {
            throw std::runtime_error ("SM-D6 duplicate-session notice missing");
        }
    }
    const auto duplicate = nlohmann::json::parse (
      *duplicate_payload)
                             .get<actor_push_notify_t> ();
    if (duplicate.actor_id != actor_id
        || duplicate.value != "duplicate-session") {
        throw std::runtime_error (
          "SM-D6 duplicate-session notice mismatch: actor="
          + duplicate.actor_id + " value=" + duplicate.value);
    }
    {
        std::unique_lock lock (close_gate);
        if (!close_changed.wait_for (
              lock, std::chrono::seconds (3),
              [&] { return old_disconnected.has_value (); })) {
            throw std::runtime_error (
              "SM-D6 retired session was not closed after callback completion");
        }
        if (*old_disconnected - *duplicate_received
            < std::chrono::milliseconds (80)) {
            throw std::runtime_error (
              "SM-D6 retired session closed before the 100 ms grace period");
        }
    }

    auto replacement_wait =
      unbound.wait_for<actor_push_notify_t> (std::chrono::milliseconds (10000))
        .to_future ("SM-D6 replacement push notify missing");
    auto replacement_push =
      play_a.post ("/spot/push-bound-session")
        .body (bound_session_push_req_t{
          .actor_id = actor_id,
          .push = actor_push_req_t{"push-replacement-only"}})
        .submit_raw ()
        .result ();
    if (!replacement_push || replacement_push.value ().status >= 400) {
        throw std::runtime_error (
          "SM-D6 replacement push trigger failed: status="
          + std::to_string (replacement_push ? replacement_push.value ().status : 0)
          + " body="
          + (replacement_push ? replacement_push.value ().body : "<transport failure>"));
    }
    const auto replacement_notify = replacement_wait.get ();
    if (replacement_notify.actor_id != actor_id
        || replacement_notify.value != "push-replacement-only") {
        throw std::runtime_error ("SM-D6 replacement push notify mismatch");
    }

    (void) bound.close ();
    (void) unbound.close ();
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
