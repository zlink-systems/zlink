/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/automatic_turn_dispatch_contracts.hpp"
#include "../Support/scenario_assert.hpp"

#include <chrono>
#include <iostream>
#include <string>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

template <typename TConnector>
void run_td_c2_http_async_exclusion_scenario (TConnector &connector,
                                              TConnector &observer,
                                              const std::string &spot_id)
{
    const auto request_id = unique_id ("TD-C2");
    connector.send (http_await_msg_t{.request_id = request_id,
                                     .delay_ms = 350,
                                     .terminator = "async"})
      .packet_name (http_await_msg_t::packet_name)
      .metadata (spot_id_metadata, spot_id)
      .submit ();
    auto held =
      observer.request (await_evidence_wait_req_t{.request_id = request_id,
                                                   .marker = "http-async-held",
                                                   .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::seconds (10))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (held), "TD-C2 held marker wait failed");
    ensure (contains_request_marker (held.value ().evidence, request_id,
                                     "http-async-held"),
            "TD-C2 held marker was not observed");
    observer.send (timer_start_msg_t{.request_id = request_id,
                                     .timer_name = request_id + "-timer",
                                     .mode = "fast",
                                     .period_ms = 30,
                                     .delay_ms = 0})
      .packet_name (timer_start_msg_t::packet_name)
      .metadata (spot_id_metadata, spot_id)
      .submit ();
    observer.send (probe_msg_t{.request_id = request_id, .marker = "http-probe"})
      .packet_name (probe_msg_t::packet_name)
      .metadata (spot_id_metadata, spot_id)
      .submit ();
    auto evidence =
      observer.request (await_evidence_wait_req_t{.request_id = request_id,
                                                   .marker = "probe-completed",
                                                   .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::seconds (10))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (evidence), "TD-C2 probe marker wait failed");
    ensure (contains_in_order (evidence.value ().evidence, request_id,
                               {"http-async-held", "http-async-completed",
                                "probe-started", "probe-completed"}),
            "TD-C2 probe entered while HTTP async held the turn");
    ensure (contains_in_order (evidence.value ().evidence, request_id,
                               {"http-async-completed", "timer-started"}),
            "TD-C2 timer callback entered while HTTP async held the turn");
    observer.send (timer_stop_msg_t{.request_id = request_id})
      .packet_name (timer_stop_msg_t::packet_name)
      .metadata (spot_id_metadata, spot_id)
      .submit ();
    std::cout << "scenario TD-C2 passed\n";
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
