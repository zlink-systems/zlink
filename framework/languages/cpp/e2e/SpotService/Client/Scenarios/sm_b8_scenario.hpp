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

inline void run_sm_b8_scenario (const std::string &play_http_endpoint,
                                const std::string &session_stream_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-B8");
    }
    if (session_stream_endpoint.empty ()) {
        throw std::runtime_error ("streamEndpoint is required for SM-B8");
    }

    constexpr auto actor_id = "actor-sm-b8-destroy";

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (3000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto stream = zlink::stream_connector::connector_factory_t::create (options);

    auto connected = stream.connect ();
    if (!connected) {
        throw std::runtime_error ("SM-B8 stream connect failed");
    }
    auto auth =
      stream.request (stream_ensure_auth_req_t{"play-a", actor_id, "destroy"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (3000))
        .submit<stream_auth_res_t> ();
    if (!auth) {
        throw std::runtime_error ("SM-B8 stream auth failed");
    }

    auto destroyed =
      stream.request (destroy_actor_req_t{"destroy"})
        .packet_name ("DestroyActorReq")
        .metadata ("actor-id", actor_id)
        .timeout (std::chrono::milliseconds (3000))
        .submit<destroy_actor_res_t> ();
    if (!destroyed || !destroyed.value ().destroyed || destroyed.value ().actor_id != actor_id) {
        const auto detail =
          destroyed ? ("destroyed=" + std::string (destroyed.value ().destroyed ? "true" : "false")
                       + " actor_id=" + destroyed.value ().actor_id)
                    : (destroyed.error () ? destroyed.error ()->message : "destroy request failed");
        throw std::runtime_error ("SM-B8 destroy reply mismatch: " + detail);
    }

    auto after_destroy =
      stream.request (actor_ping_req_t{"after-destroy"})
        .packet_name ("ActorPingReq")
        .metadata ("actor-id", actor_id)
        .timeout (std::chrono::milliseconds (1000))
        .submit<actor_ping_res_t> ();
    (void) stream.close ();
    if (after_destroy) {
        throw std::runtime_error ("SM-B8 destroyed actor unexpectedly accepted request");
    }

    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto evidence =
      play_a.post ("/evidence/wait")
        .body (evidence_wait_req_t{.contains_all = {"ActorDestroyed", actor_id},
                                   .timeout_milliseconds = 3000})
        .submit<evidence_snapshot_t> ()
        .result ();
    if (!evidence) {
        throw std::runtime_error (evidence.error () ? evidence.error ()->what ()
                                                   : "SM-B8 destroy evidence wait failed");
    }
    for (const auto &entry : evidence.value ().body.entries) {
        if (entry.marker == "ActorDestroyFailed") {
            throw std::runtime_error ("SM-B8 actor destroy reported a failure");
        }
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
