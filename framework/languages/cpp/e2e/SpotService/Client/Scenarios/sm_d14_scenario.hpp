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

inline void run_sm_d14_scenario (const std::string &session_tls_stream_endpoint)
{
    if (session_tls_stream_endpoint.empty ()) {
        throw std::runtime_error ("tlsStreamEndpoint is required for SM-D14");
    }

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_tls_stream_endpoint;
    options.transport = zlink::stream_connector::transport_t::tls;
    options.connect_timeout = std::chrono::milliseconds (5000);
    options.request_timeout = std::chrono::milliseconds (5000);
    options.heartbeat.enabled = false;
    options.reconnect.enabled = false;
    options.reconnect.max_attempts = 1;
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;

    options.skip_server_certificate_validation = true;
    auto tls = zlink::stream_connector::connector_factory_t::create (options);
    auto connected = tls.connect ();
    if (!connected) {
        throw std::runtime_error ("SM-D14 TLS stream connect failed");
    }

    constexpr auto actor_id = "actor-sm-d14-tls";
    auto auth =
      tls.request (stream_ensure_auth_req_t{"play-a", actor_id, "SM-D14 TLS"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (5000))
        .submit<stream_auth_res_t> ();
    if (!auth) {
        throw std::runtime_error (
          std::string ("SM-D14 TLS auth failed: ")
          + (auth.error () ? auth.error ()->message : "unknown stream auth error"));
    }
    if (auth.value ().actor.actor_id != actor_id || auth.value ().session_node_rid != "session-a") {
        throw std::runtime_error (
          "SM-D14 TLS auth reply mismatch: actor=" + auth.value ().actor.actor_id
          + " session=" + auth.value ().session_node_rid);
    }

    auto push_wait =
      tls.wait_for<actor_push_notify_t> (std::chrono::milliseconds (10000))
        .to_future ("SM-D14 TLS push notify missing");
    auto reply = tls.request (actor_push_req_t{"tls-push"})
                   .packet_name ("PushReq")
                   .timeout (std::chrono::milliseconds (5000))
                   .submit<actor_push_res_t> ();
    if (!reply || !reply.value ().pushed || reply.value ().actor_id != actor_id) {
        throw std::runtime_error ("SM-D14 TLS actor push reply mismatch");
    }

    const auto notify = push_wait.get ();
    if (notify.actor_id != actor_id || notify.value != "tls-push") {
        throw std::runtime_error ("SM-D14 TLS push notify mismatch");
    }

    (void) tls.close ();

    options.skip_server_certificate_validation = false;
    auto strict = zlink::stream_connector::connector_factory_t::create (options);
    const auto strict_connected = strict.connect ();
    if (strict_connected) {
        (void) strict.close ();
        throw std::runtime_error (
          "SM-D14 strict TLS validation accepted the self-signed certificate");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
