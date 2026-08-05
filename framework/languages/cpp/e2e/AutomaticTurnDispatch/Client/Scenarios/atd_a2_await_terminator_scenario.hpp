/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/automatic_turn_dispatch_contracts.hpp"
#include "../Support/scenario_assert.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

template <typename TConnector>
std::string run_atd_a2_await_terminator_scenario (TConnector &connector,
                                                 TConnector &observer,
                                                 const std::string &spot_id)
{
    const auto request_id = unique_id ("ATD-A2");
    connector.send (await_msg_t{.request_id = request_id,
                                .delay_ms = 1500,
                                .correlation_id = "corr-a2"})
      .packet_name (await_msg_t::packet_name)
      .metadata (spot_id_metadata, spot_id)
      .submit ();
    auto released =
      observer.request (
                await_evidence_wait_req_t{.request_id = request_id,
                                          .marker = "await-released",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (released), "ATD-A2 await-released wait failed");
    ensure (contains_request_marker (released.value ().evidence, request_id,
                                     "await-released"),
            "ATD-A2 await-released marker was not observed");
    observer.send (probe_msg_t{.request_id = request_id, .marker = "await-probe"})
        .packet_name (probe_msg_t::packet_name)
        .metadata (spot_id_metadata, spot_id)
        .submit ();
    auto evidence =
      observer.request (
                await_evidence_wait_req_t{.request_id = request_id,
                                          .marker = "await-completed",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (evidence), "ATD-A2 evidence wait failed");
    ensure (contains_in_order (evidence.value ().evidence, request_id,
                               {"await-started", "await-released", "probe-started",
                                "probe-completed", "await-resumed", "await-completed"}),
            "ATD-A2 marker order mismatch");
    std::cout << "scenario ATD-A2 passed\n";
    return request_id;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
