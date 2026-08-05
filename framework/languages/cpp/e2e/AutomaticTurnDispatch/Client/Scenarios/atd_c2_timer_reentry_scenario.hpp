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
std::string run_atd_c2_timer_reentry_scenario (TConnector &connector,
                                              TConnector &observer,
                                              const std::string &spot_id)
{
    const auto request_id = unique_id ("ATD-C2");
    connector.send (timer_start_msg_t{.request_id = request_id,
                                      .timer_name = request_id + "-same",
                                      .mode = "await-then-next",
                                      .period_ms = 340,
                                      .delay_ms = 350})
        .packet_name (timer_start_msg_t::packet_name)
        .metadata (spot_id_metadata, spot_id)
        .submit ();
    auto evidence =
      observer.request (
                await_evidence_wait_req_t{.request_id = request_id,
                                          .marker = "timer-next-completed",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure_result (evidence, "ATD-C2 evidence wait failed");
    ensure_contains_in_order (evidence.value ().evidence, request_id,
                              {"timer-await-started",
                               "timer-await-released",
                               "timer-await-resumed",
                               "timer-await-completed",
                               "timer-next-started",
                               "timer-next-completed"},
                              "ATD-C2 marker order mismatch");
    connector.send (timer_stop_msg_t{.request_id = request_id})
        .packet_name (timer_stop_msg_t::packet_name)
        .metadata (spot_id_metadata, spot_id)
        .submit ();
    std::cout << "scenario ATD-C2 passed\n";
    return request_id;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
