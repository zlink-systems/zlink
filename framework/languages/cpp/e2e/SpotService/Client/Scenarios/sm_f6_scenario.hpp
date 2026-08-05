/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{
inline void run_sm_f6_scenario (const std::string &multi_a_http_endpoint,
                                const std::string &multi_b_http_endpoint,
                                const std::string &run_id)
{
    if (multi_a_http_endpoint.empty () || multi_b_http_endpoint.empty ()) {
        throw std::runtime_error ("SM-F6 requires multi-node HTTP endpoints");
    }
    auto multi_a =
      zlink::http_client::client_t::create ()
        .base_url (multi_a_http_endpoint)
        .timeout (std::chrono::seconds (45))
        .build ();
    auto multi_b =
      zlink::http_client::client_t::create ()
        .base_url (multi_b_http_endpoint)
        .timeout (std::chrono::seconds (45))
        .build ();

    if (run_id.empty ()) {
        throw std::runtime_error ("SM-F6 requires runId");
    }
    const auto &suffix = run_id;
    const auto source_spot_id = "spot-sm-f6-source-cpp-" + suffix;
    const auto target_spot_id = "spot-sm-f6-target-cpp-" + suffix;
    const auto actor_id = "actor-sm-f6-cpp-" + suffix;
    const auto marker = "sm-f6-cpp-" + suffix;

    auto target =
      multi_b.post ("/spot/create-user-local")
        .body (create_spot_req_t{.spot_id = target_spot_id})
        .submit_raw ()
        .result ();
    if (!target || target.value ().status >= 400) {
        throw std::runtime_error ("SM-F6 target spot create failed");
    }
    const auto created = nlohmann::json::parse (target.value ().body).get<create_spot_res_t> ();
    if (created.owner_node_rid != "multi-b") {
        throw std::runtime_error ("SM-F6 target spot node mismatch");
    }

    auto mesh =
      multi_a.post ("/spot/spot-only/request-send")
        .body (spot_only_mesh_req_t{.source_spot_id = source_spot_id,
                                    .target_spot_id = target_spot_id,
                                    .marker = marker})
        .submit_raw ()
        .result ();
    if (!mesh || mesh.value ().status >= 400) {
        throw std::runtime_error (
          "SM-F6 spot-only request/send failed: "
          + (mesh ? mesh.value ().body
                  : (mesh.error () ? mesh.error ()->what () : "HTTP failed")));
    }
    const auto mesh_result =
      nlohmann::json::parse (mesh.value ().body).get<spot_only_mesh_res_t> ();
    if (mesh_result.target_spot_id != target_spot_id || mesh_result.target_value != 7) {
        throw std::runtime_error ("SM-F6 spot-only mesh reply mismatch");
    }

    auto join =
      multi_a.post ("/actor/spot-only-join")
        .body (spot_only_join_req_t{.target_spot_id = target_spot_id,
                                    .actor_id = actor_id,
                                    .marker = marker})
        .submit_raw ()
        .result ();
    if (!join || join.value ().status >= 400) {
        throw std::runtime_error (
          "SM-F6 spot-only actor join failed: "
          + (join ? "status=" + std::to_string (join.value ().status)
                      + " body=" + join.value ().body
                  : (join.error () ? join.error ()->what () : "HTTP failed")));
    }
    const auto joined = nlohmann::json::parse (join.value ().body).get<spot_only_join_res_t> ();
    if (!joined.accepted || joined.actor_id != actor_id) {
        throw std::runtime_error ("SM-F6 spot-only actor join was rejected");
    }

    (void) multi_b;
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
