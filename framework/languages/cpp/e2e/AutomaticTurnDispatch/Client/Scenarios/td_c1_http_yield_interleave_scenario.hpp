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
void run_td_c1_http_yield_interleave_scenario (TConnector &connector,
                                               TConnector &observer,
                                               const std::string &spot_id)
{
    const auto request_id = unique_id ("TD-C1");
    connector.send (http_await_msg_t{.request_id = request_id,
                                     .delay_ms = 1500,
                                     .terminator = "yield"})
      .packet_name (http_await_msg_t::packet_name)
      .metadata (spot_id_metadata, spot_id)
      .submit ();
    auto released =
      observer.request (await_evidence_wait_req_t{.request_id = request_id,
                                                   .marker = "http-yield-released",
                                                   .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::seconds (10))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (released), "TD-C1 release marker wait failed");
    ensure (contains_request_marker (released.value ().evidence, request_id,
                                     "http-yield-released"),
            "TD-C1 release marker was not observed");
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
                                                   .marker = "http-yield-completed",
                                                   .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::seconds (10))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (evidence), "TD-C1 completion marker wait failed");
    ensure (contains_in_order (evidence.value ().evidence, request_id,
                               {"http-yield-released", "probe-started",
                                "probe-completed", "http-yield-resumed"}),
            "TD-C1 probe did not run while the HTTP yield released the turn");
    ensure (contains_in_order (evidence.value ().evidence, request_id,
                               {"http-yield-released", "timer-fast-completed",
                                "http-yield-resumed"}),
            "TD-C1 timer did not run while the HTTP yield released the turn");
    observer.send (timer_stop_msg_t{.request_id = request_id})
      .packet_name (timer_stop_msg_t::packet_name)
      .metadata (spot_id_metadata, spot_id)
      .submit ();
    std::cout << "scenario TD-C1 passed\n";
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
