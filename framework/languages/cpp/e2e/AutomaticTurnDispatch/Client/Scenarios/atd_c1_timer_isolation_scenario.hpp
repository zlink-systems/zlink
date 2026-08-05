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
std::string run_atd_c1_timer_isolation_scenario (TConnector &connector,
                                                TConnector &observer,
                                                const std::string &spot_id)
{
    const auto request_id = unique_id ("ATD-C1");
    connector.send (timer_start_msg_t{.request_id = request_id,
                                      .timer_name = request_id + "-await",
                                      .mode = "await-on-first",
                                      .period_ms = 500,
                                      .delay_ms = 350})
        .packet_name (timer_start_msg_t::packet_name)
        .metadata (spot_id_metadata, spot_id)
        .submit ();
    auto timer_await_released =
      observer.request (
                await_evidence_wait_req_t{.request_id = request_id,
                                          .marker = "timer-await-released",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (timer_await_released),
            "ATD-C1 timer-await-released wait failed");
    connector.send (timer_start_msg_t{.request_id = request_id,
                                      .timer_name = request_id + "-fast",
                                      .mode = "fast",
                                      .period_ms = 50,
                                      .delay_ms = 0})
        .packet_name (timer_start_msg_t::packet_name)
        .metadata (spot_id_metadata, spot_id)
        .submit ();
    auto timer_fast_completed =
      observer.request (
                await_evidence_wait_req_t{.request_id = request_id,
                                          .marker = "timer-fast-completed",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (timer_fast_completed),
            "ATD-C1 timer-fast-completed wait failed");
    auto evidence =
      observer.request (
                await_evidence_wait_req_t{.request_id = request_id,
                                          .marker = "timer-await-completed",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure_result (evidence, "ATD-C1 evidence wait failed");
    ensure_contains_in_order (evidence.value ().evidence, request_id,
                              {"timer-await-started",
                               "timer-await-released",
                               "timer-fast-started",
                               "timer-fast-completed",
                               "timer-await-resumed",
                               "timer-await-completed"},
                              "ATD-C1 marker order mismatch");
    connector.send (timer_stop_msg_t{.request_id = request_id})
        .packet_name (timer_stop_msg_t::packet_name)
        .metadata (spot_id_metadata, spot_id)
        .submit ();
    std::cout << "scenario ATD-C1 passed\n";
    return request_id;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
