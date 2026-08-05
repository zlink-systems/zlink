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

inline void sm_b9_verify_admission (const std::string &owner_http_endpoint,
                                    const std::string &session_stream_endpoint,
                                    const std::string &node_rid,
                                    const std::string &spot_id,
                                    const std::string &actor_id)
{
    auto owner = zlink::http_client::client_t::create ().base_url (owner_http_endpoint).build ();
    auto created =
      owner.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = spot_id})
        .submit_raw ()
        .result ();
    if (!created || created.value ().status >= 400) {
        throw std::runtime_error ("SM-B9 spot create failed on " + node_rid);
    }

    zlink::stream_connector::connector_options_t options;
    options.endpoint = session_stream_endpoint;
    options.connect_timeout = std::chrono::milliseconds (3000);
    options.request_timeout = std::chrono::milliseconds (10000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto stream = zlink::stream_connector::connector_factory_t::create (options);
    if (!stream.connect ()) {
        throw std::runtime_error ("SM-B9 stream connect failed");
    }

    auto auth =
      stream.request (stream_ensure_auth_req_t{node_rid, actor_id, "SM-B9 admission"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (10000))
        .submit<stream_auth_res_t> ();
    if (!auth) {
        throw std::runtime_error ("SM-B9 allowed actor auth failed");
    }
    auto allowed =
      stream
        .request (join_admitted_user_spot_actor_req_t{.spot_id = spot_id,
                                                      .actor_id = actor_id,
                                                      .allow = true,
                                                      .reason = "allowed"})
        .packet_name ("JoinAdmittedUserSpotActorReq")
        .timeout (std::chrono::milliseconds (10000))
        .submit<join_admitted_user_spot_actor_res_t> ();
    if (!allowed || !allowed.value ().accepted || allowed.value ().spot_id != spot_id) {
        throw std::runtime_error ("SM-B9 allowed join was rejected");
    }

    const auto rejected_actor_id = actor_id + "-rejected";
    auto rejected_auth =
      stream.request (stream_ensure_auth_req_t{node_rid, rejected_actor_id, "SM-B9 rejected"})
        .packet_name ("StreamEnsureAuthReq")
        .timeout (std::chrono::milliseconds (10000))
        .submit<stream_auth_res_t> ();
    if (!rejected_auth) {
        throw std::runtime_error ("SM-B9 rejected actor auth failed");
    }
    auto rejected =
      stream
        .request (join_admitted_user_spot_actor_req_t{.spot_id = spot_id,
                                                      .actor_id = rejected_actor_id,
                                                      .allow = false,
                                                      .reason = "capacity"})
        .packet_name ("JoinAdmittedUserSpotActorReq")
        .metadata ("actor-id", rejected_actor_id)
        .timeout (std::chrono::milliseconds (10000))
        .submit<join_admitted_user_spot_actor_res_t> ();
    if (!rejected || rejected.value ().accepted
        || rejected.value ().error_kind != "ActorJoinRejected") {
        throw std::runtime_error ("SM-B9 rejected join was not classified");
    }

    auto evidence =
      owner.post ("/evidence/wait")
        .body (evidence_wait_req_t{.contains_all = {spot_id, actor_id, "allowed",
                                                    rejected_actor_id, "capacity"},
                                   .timeout_milliseconds = 10000})
        .submit_raw ()
        .result ();
    if (!evidence || evidence.value ().status >= 400) {
        throw std::runtime_error ("SM-B9 admission evidence wait failed");
    }
    const auto snapshot = nlohmann::json::parse (evidence.value ().body).get<evidence_snapshot_t> ();
    for (const auto &entry : snapshot.entries) {
        if (entry.marker == "SpotActorJoined" && entry.spot_id == spot_id
            && entry.actor_id == rejected_actor_id) {
            throw std::runtime_error ("SM-B9 rejected actor was joined to the user spot");
        }
    }
    (void) stream.close ();
}

inline void run_sm_b9_scenario (const std::string &play_http_endpoint,
                                const std::string &play_b_http_endpoint,
                                const std::string &session_stream_endpoint)
{
    if (play_http_endpoint.empty () || play_b_http_endpoint.empty ()
        || session_stream_endpoint.empty ()) {
        throw std::runtime_error (
          "SM-B9 requires play-a, play-b, and session stream endpoints");
    }
    sm_b9_verify_admission (play_http_endpoint, session_stream_endpoint, "play-a",
                            "spot-sm-b9-local-cpp", "actor-sm-b9-local-cpp");
    sm_b9_verify_admission (play_b_http_endpoint, session_stream_endpoint, "play-b",
                            "spot-sm-b9-remote-cpp", "actor-sm-b9-remote-cpp");
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
