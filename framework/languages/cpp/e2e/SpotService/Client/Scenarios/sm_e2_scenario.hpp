/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_e2_scenario (const std::string &play_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-E2");
    }

    constexpr auto spot_id = "user:play-a:sm-e2-timer";
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();

    auto created =
      play_a.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = spot_id})
        .submit_raw ()
        .result ();
    if (!created) {
        throw std::runtime_error (created.error () ? created.error ()->what ()
                                                   : "SM-E2 create spot HTTP failed");
    }
    if (created.value ().status >= 400) {
        throw std::runtime_error ("SM-E2 create spot HTTP status "
                                  + std::to_string (created.value ().status) + ": "
                                  + created.value ().body);
    }
    const auto create_reply = nlohmann::json::parse (created.value ().body).get<create_spot_res_t> ();
    if (!create_reply.created || create_reply.spot_id != spot_id
        || create_reply.owner_node_rid != "play-a") {
        throw std::runtime_error ("SM-E2 create spot reply mismatch");
    }

    auto observed =
      play_a.post ("/evidence/wait")
        .body (evidence_wait_req_t{
          .contains_all = {"SpotTimerTick", spot_id, "sm-e2-tick:1"},
          .timeout_milliseconds = 5000})
        .submit_raw ()
        .result ();
    if (!observed) {
        throw std::runtime_error (observed.error () ? observed.error ()->what ()
                                                    : "SM-E2 evidence wait HTTP failed");
    }
    if (observed.value ().status >= 400) {
        throw std::runtime_error ("SM-E2 evidence wait HTTP status "
                                  + std::to_string (observed.value ().status) + ": "
                                  + observed.value ().body);
    }
    const auto evidence =
      nlohmann::json::parse (observed.value ().body).get<evidence_snapshot_t> ();
    bool has_timer_tick = false;
    for (const auto &entry : evidence.entries) {
        if (entry.marker == "SpotTimerTick" && entry.spot_id == spot_id
            && entry.value == "sm-e2-tick:1") {
            has_timer_tick = true;
            break;
        }
    }
    if (!has_timer_tick) {
        throw std::runtime_error ("SM-E2 timer tick evidence was not observed");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
