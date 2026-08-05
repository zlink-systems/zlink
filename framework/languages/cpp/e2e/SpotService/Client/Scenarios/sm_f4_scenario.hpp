/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline std::size_t sm_f4_dispatch_failure_count (const evidence_snapshot_t &snapshot)
{
    std::size_t count = 0;
    for (const auto &entry : snapshot.entries) {
        if (entry.marker == "SpotRouteDispatchFailure") {
            ++count;
        }
    }
    return count;
}

inline bool sm_f4_has_dispatch_failure (const evidence_snapshot_t &snapshot,
                                        const std::string &packet_name,
                                        const std::string &action)
{
    for (const auto &entry : snapshot.entries) {
        if (entry.marker == "SpotRouteDispatchFailure" && entry.actor_id == packet_name
            && entry.value == "handler_missing:" + action) {
            return true;
        }
    }
    return false;
}

inline void run_sm_f4_scenario (const std::string &play_http_endpoint,
                                const std::string &play_b_http_endpoint,
                                const std::string &remote_spot)
{
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .timeout (std::chrono::milliseconds (3000))
                    .build ();
    auto play_b = zlink::http_client::client_t::create ()
                    .base_url (play_b_http_endpoint)
                    .timeout (std::chrono::milliseconds (3000))
                    .build ();
    const std::string target_key = "b-sm-f4-route-negative";
    const std::string target_spot = "user:play-b:" + target_key;
    const std::string source_spot = "user:play-a:a-sm-f4-route-source";

    auto source_created =
      play_a.post ("/spot/create")
        .body (create_spot_req_t{.spot_id = source_spot})
        .submit<create_spot_res_t> ()
        .result ();
    if (!source_created || source_created.value ().body.spot_id != source_spot) {
        throw std::runtime_error ("SM-F4 source spot setup failed");
    }

    auto created = play_b.post ("/spot/create")
                     .body (create_spot_req_t{.spot_id = target_spot})
                     .submit<create_spot_res_t> ()
                     .result ();
    if (!created || created.value ().body.spot_id != target_spot) {
        throw std::runtime_error ("SM-F4 target spot setup failed");
    }

    auto evidence_before_a = play_a.get ("/evidence").submit<evidence_snapshot_t> ().result ();
    auto evidence_before_b = play_b.get ("/evidence").submit<evidence_snapshot_t> ().result ();
    if (!evidence_before_a || !evidence_before_b) {
        throw std::runtime_error ("SM-F4 initial evidence read failed");
    }
    const auto dispatch_failures_before =
      sm_f4_dispatch_failure_count (evidence_before_a.value ().body)
      + sm_f4_dispatch_failure_count (evidence_before_b.value ().body);

    auto closed = play_b.post ("/spot/close")
                    .body (close_spot_req_t{.key = target_key})
                    .submit<close_spot_res_t> ()
                    .result ();
    if (!closed || !closed.value ().body.closed) {
        throw std::runtime_error ("SM-F4 target spot close failed");
    }

    auto missing_route =
      play_a.post ("/spot/to-spot/negative")
        .body (spot_to_spot_route_req_t{.source_node_rid = "play-a",
                                        .source_spot_id = source_spot,
                                        .target_node_rid = "play-b",
                                        .target_spot_id = target_spot,
                                        .marker = "missing-route"})
        .submit<spot_to_spot_negative_route_res_t> ()
        .result ();
    if (!missing_route || !missing_route.value ().body.request_failed) {
        throw std::runtime_error ("SM-F4 missing target request unexpectedly succeeded");
    }

    auto evidence_after_a =
      play_a.post ("/evidence/wait")
        .body (evidence_wait_req_t{
          .contains_all = {"SpotRouteDispatchFailure", "DirectSpotReq", "reply_error"},
          .timeout_milliseconds = 10000})
        .submit<evidence_snapshot_t> ()
        .result ();
    auto evidence_after_b =
      play_b.post ("/evidence/wait")
        .body (evidence_wait_req_t{
          .contains_all = {"SpotRouteDispatchFailure", "DirectSpotMsg", "drop"},
          .timeout_milliseconds = 10000})
        .submit<evidence_snapshot_t> ()
        .result ();
    if (!evidence_after_a || !evidence_after_b) {
        throw std::runtime_error ("SM-F4 dispatch failure evidence was not observed");
    }
    const auto dispatch_failures_after =
      sm_f4_dispatch_failure_count (evidence_after_a.value ().body)
      + sm_f4_dispatch_failure_count (evidence_after_b.value ().body);
    if (dispatch_failures_after < dispatch_failures_before + 2
        || !sm_f4_has_dispatch_failure (evidence_after_a.value ().body, "DirectSpotReq",
                                        "reply_error")
        || !sm_f4_has_dispatch_failure (evidence_after_b.value ().body, "DirectSpotMsg", "drop")) {
        throw std::runtime_error ("SM-F4 dispatch failure classification mismatch");
    }

    auto raw =
      play_a.post ("/spot/direct")
        .body (direct_spot_route_req_t{.target_node_rid = "play-b",
                                       .spot_id = remote_spot,
                                       .value = "route-recovery",
                                       .source_actor_id = "external-client"})
        .submit_raw ()
        .result ();
    if (!raw || raw.value ().status >= 400) {
        throw std::runtime_error ("SM-F4 recovery spot route request failed");
    }
    const auto recovery_after_negative =
      nlohmann::json::parse (raw.value ().body).get<direct_spot_res_t> ();
    if (recovery_after_negative.value != "route-recovery:reply") {
        throw std::runtime_error ("SM-F4 recovery spot route request failed");
    }

    std::cout << "scenario SM-F4 passed\n";
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
