/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_e3_scenario (const std::string &play_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-E3");
    }

    constexpr auto spot_id = "user:play-a:sm-e3-idle";
    constexpr auto timer_name = "sm-e3-idle";
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();

    auto idle =
      play_a.post ("/spot/idle-close/start")
        .body (spot_idle_close_req_t{.spot_id = spot_id,
                                     .name = timer_name,
                                     .period_ms = 50})
        .submit_raw ()
        .result ();
    if (!idle) {
        throw std::runtime_error (idle.error () ? idle.error ()->what ()
                                                : "SM-E3 idle close HTTP failed");
    }
    if (idle.value ().status >= 400) {
        throw std::runtime_error ("SM-E3 idle close HTTP status "
                                  + std::to_string (idle.value ().status) + ": "
                                  + idle.value ().body);
    }
    const auto idle_reply =
      nlohmann::json::parse (idle.value ().body).get<spot_idle_close_res_t> ();
    if (!idle_reply.closed || idle_reply.spot_id != spot_id || idle_reply.name != timer_name) {
        throw std::runtime_error ("SM-E3 idle close reply mismatch");
    }

    auto closed_spot_request =
      play_a.post ("/spot/missing-target/request")
        .body (spot_missing_target_req_t{.spot_id = spot_id})
        .submit_raw ()
        .result ();
    if (!closed_spot_request || closed_spot_request.value ().status >= 400) {
        throw std::runtime_error ("SM-E3 closed spot request endpoint failed");
    }
    const auto closed_spot_reply =
      nlohmann::json::parse (closed_spot_request.value ().body)
        .get<spot_missing_target_res_t> ();
    if (closed_spot_reply.spot_id != spot_id || !closed_spot_reply.failed) {
        throw std::runtime_error ("SM-E3 closed spot request did not fail");
    }

    bool has_idle_close = false;
    bool has_closing = false;
    for (const auto &entry : idle_reply.evidence.entries) {
        if (entry.spot_id != spot_id) {
            continue;
        }
        if (entry.marker == "SpotIdleTimerClosed"
            && entry.value == std::string (timer_name) + ":closed=true") {
            has_idle_close = true;
        }
        if (entry.marker == "SpotClosing") {
            has_closing = true;
        }
    }
    if (!has_idle_close || !has_closing) {
        throw std::runtime_error ("SM-E3 idle close evidence mismatch");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
