/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/spot_lifecycle_order_context.hpp"
#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_a1_scenario (const std::string &play_http_endpoint,
                                spot_lifecycle_order_context_t &context)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-A1");
    }

    auto api = zlink::http_client::client_t::create ()
                 .base_url (play_http_endpoint)
                 .build ();
    auto raw =
      api.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = context.spot_id})
        .submit_raw ()
        .result ();
    if (!raw) {
        throw std::runtime_error (raw.error () ? raw.error ()->what () : "SM-A1 HTTP failed");
    }
    if (raw.value ().status >= 400) {
        throw std::runtime_error ("SM-A1 HTTP status " + std::to_string (raw.value ().status)
                                  + ": " + raw.value ().body);
    }
    auto created = nlohmann::json::parse (raw.value ().body).get<create_spot_res_t> ();
    if (created.spot_id != context.spot_id) {
        throw std::runtime_error ("SM-A1 spot id mismatch: " + created.spot_id);
    }
    if (created.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-A1 owner mismatch: " + created.owner_node_rid);
    }
}

inline void run_sm_a1_scenario (const std::string &play_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-A1");
    }

    auto api = zlink::http_client::client_t::create ()
                 .base_url (play_http_endpoint)
                 .build ();
    auto raw =
      api.post ("/spot/join")
        .body (join_req_t{.key = "a-room",
                          .actor_id = "alice",
                          .display_name = "Alice",
                          .level = 7,
                          .tags = {"alpha", "local"}})
        .submit_raw ()
        .result ();
    if (!raw) {
        throw std::runtime_error (raw.error () ? raw.error ()->what () : "SM-A1 HTTP failed");
    }
    if (raw.value ().status >= 400) {
        throw std::runtime_error ("SM-A1 HTTP status " + std::to_string (raw.value ().status)
                                  + ": " + raw.value ().body);
    }
    auto created = nlohmann::json::parse (raw.value ().body).get<join_res_t> ();

    if (created.spot_id != "user:play-a:a-room") {
        throw std::runtime_error ("SM-A1 spot id mismatch: " + created.spot_id);
    }
    if (created.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-A1 owner mismatch: " + created.owner_node_rid);
    }
    if (created.actor_id != "alice" || created.display_name != "Alice" || created.level != 7) {
        throw std::runtime_error ("SM-A1 join payload mismatch");
    }
    if (created.actor.node_rid.empty () || created.actor.actor_id != created.actor_id
        || created.actor.generation == 0) {
        throw std::runtime_error ("SM-A1 actor ref is not concrete");
    }

    auto location_raw = api.get ("/locations/spots").submit_raw ().result ();
    if (!location_raw) {
        throw std::runtime_error (location_raw.error () ? location_raw.error ()->what ()
                                                        : "SM-A1 location query failed");
    }
    if (location_raw.value ().status >= 400) {
        throw std::runtime_error ("SM-A1 location query HTTP status "
                                  + std::to_string (location_raw.value ().status) + ": "
                                  + location_raw.value ().body);
    }
    const auto rows = nlohmann::json::parse (location_raw.value ().body);
    const auto expected = std::find_if (rows.begin (), rows.end (), [&] (const auto &row) {
        return row.value ("mesh_name", "") == spot_mesh
               && row.value ("spot_id", "") == created.spot_id
               && row.value ("spot_type", "") == user_spot
               && row.value ("node_rid", "") == created.owner_node_rid
               && row.value ("spot_kind", "") == "user"
               && !row.value ("owner_id", "").empty () && row.value ("generation", 0) > 0;
    });
    if (expected == rows.end ()) {
        throw std::runtime_error ("SM-A1 spot location row mismatch: " + rows.dump ());
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
