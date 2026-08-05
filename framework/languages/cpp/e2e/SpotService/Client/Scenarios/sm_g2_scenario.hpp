/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline evidence_snapshot_t fetch_sm_g2_evidence (zlink::http_client::client_t &client,
                                                 const std::string &name)
{
    auto raw = client.get ("/evidence").submit_raw ().result ();
    if (!raw) {
        throw std::runtime_error (raw.error () ? raw.error ()->what ()
                                               : "SM-G2 " + name + " evidence HTTP failed");
    }
    if (raw.value ().status >= 400) {
        throw std::runtime_error ("SM-G2 " + name + " evidence HTTP status "
                                  + std::to_string (raw.value ().status) + ": "
                                  + raw.value ().body);
    }
    return nlohmann::json::parse (raw.value ().body).get<evidence_snapshot_t> ();
}

inline bool sm_g2_has_spot_request (const evidence_snapshot_t &snapshot,
                                    const std::string &spot_id,
                                    const std::string &value)
{
    for (const auto &entry : snapshot.entries) {
        if (entry.marker == "SpotToSpotRequest" && entry.spot_id == spot_id
            && entry.value == value) {
            return true;
        }
    }
    return false;
}

inline direct_spot_res_t post_sm_g2_direct_request (zlink::http_client::client_t &client,
                                                   const direct_spot_route_req_t &request,
                                                   const std::string &label)
{
    std::string last_error = label + " was not attempted";
    auto raw = client.post ("/spot/direct").body (request).submit_raw ().result ();
    if (raw && raw.value ().status < 400) {
        return nlohmann::json::parse (raw.value ().body).get<direct_spot_res_t> ();
    }

    if (raw) {
        last_error = label + " HTTP status " + std::to_string (raw.value ().status) + ": "
                     + raw.value ().body;
    } else {
        last_error = raw.error () ? raw.error ()->what () : label + " HTTP failed";
    }
    throw std::runtime_error ("SM-G2 " + last_error);
}

inline void run_sm_g2_scenario (const std::string &play_http_endpoint,
                                const std::string &play_b_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-G2");
    }
    if (play_b_http_endpoint.empty ()) {
        throw std::runtime_error ("playBHttpEndpoint is required for SM-G2");
    }

    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto play_b = zlink::http_client::client_t::create ()
                    .base_url (play_b_http_endpoint)
                    .build ();

    constexpr auto first_owner_spot = "user:play-a:sm-g2-owner";
    constexpr auto remapped_owner_spot = "user:play-b:sm-g2-owner";

    auto first_created_raw =
      play_a.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = first_owner_spot})
        .submit_raw ()
        .result ();
    if (!first_created_raw || first_created_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-G2 first owner spot create failed");
    }
    const auto first_created =
      nlohmann::json::parse (first_created_raw.value ().body).get<create_spot_res_t> ();
    if (first_created.spot_id != first_owner_spot
        || first_created.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-G2 first owner was not created on play-a");
    }

    const auto first_reply = post_sm_g2_direct_request (
      play_b,
      direct_spot_route_req_t{.target_node_rid = "play-a",
                              .spot_id = first_owner_spot,
                              .value = "sm-g2-before-remap",
                              .source_actor_id = "sm-g2-client"},
      "first owner request");
    if (first_reply.spot_id != first_owner_spot || first_reply.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-G2 first owner request reached the wrong node");
    }

    auto remapped_created_raw =
      play_b.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = remapped_owner_spot})
        .submit_raw ()
        .result ();
    if (!remapped_created_raw || remapped_created_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-G2 remapped owner spot create failed");
    }
    const auto remapped_created =
      nlohmann::json::parse (remapped_created_raw.value ().body).get<create_spot_res_t> ();
    if (remapped_created.spot_id != remapped_owner_spot
        || remapped_created.owner_node_rid != "play-b") {
        throw std::runtime_error ("SM-G2 remapped owner was not created on play-b");
    }

    const auto remapped_reply = post_sm_g2_direct_request (
      play_a,
      direct_spot_route_req_t{.target_node_rid = "play-b",
                              .spot_id = remapped_owner_spot,
                              .value = "sm-g2-after-remap",
                              .source_actor_id = "sm-g2-client"},
      "remapped owner request");
    if (remapped_reply.spot_id != remapped_owner_spot
        || remapped_reply.owner_node_rid != "play-b") {
        throw std::runtime_error ("SM-G2 remapped owner request reached the wrong node");
    }

    const auto play_a_evidence = fetch_sm_g2_evidence (play_a, "play-a");
    const auto play_b_evidence = fetch_sm_g2_evidence (play_b, "play-b");
    if (!sm_g2_has_spot_request (play_a_evidence, first_owner_spot, "sm-g2-before-remap")) {
        throw std::runtime_error ("SM-G2 play-a evidence is missing the first owner request");
    }
    if (!sm_g2_has_spot_request (play_b_evidence, remapped_owner_spot, "sm-g2-after-remap")) {
        throw std::runtime_error ("SM-G2 play-b evidence is missing the remapped owner request");
    }
    if (sm_g2_has_spot_request (play_a_evidence, remapped_owner_spot, "sm-g2-after-remap")) {
        throw std::runtime_error ("SM-G2 remapped owner leaked to play-a");
    }
    if (sm_g2_has_spot_request (play_b_evidence, first_owner_spot, "sm-g2-before-remap")) {
        throw std::runtime_error ("SM-G2 first owner leaked to play-b");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
