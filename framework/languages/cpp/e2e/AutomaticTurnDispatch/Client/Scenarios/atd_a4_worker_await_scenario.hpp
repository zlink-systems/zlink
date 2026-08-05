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
std::string run_atd_a4_worker_await_scenario (TConnector &connector,
                                             TConnector &observer,
                                             const std::string &spot_id)
{
    const auto request_id = unique_id ("ATD-A4");
    connector.send (worker_await_msg_t{.request_id = request_id, .delay_ms = 1500})
      .packet_name (worker_await_msg_t::packet_name)
      .metadata (spot_id_metadata, spot_id)
      .submit ();
    auto worker_released =
      observer.request (
                  await_evidence_wait_req_t{.request_id = request_id,
                                            .marker = "worker-await-released",
                                            .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (worker_released), "ATD-A4 worker-await-released wait failed");
    ensure (contains_request_marker (worker_released.value ().evidence, request_id,
                                     "worker-await-released"),
            "ATD-A4 worker-await-released marker was not observed");
    observer.send (probe_msg_t{.request_id = request_id, .marker = "worker-probe"})
        .packet_name (probe_msg_t::packet_name)
        .metadata (spot_id_metadata, spot_id)
        .submit ();
    auto evidence =
      observer.request (
                  await_evidence_wait_req_t{.request_id = request_id,
                                            .marker = "worker-await-completed",
                                            .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (evidence), "ATD-A4 evidence wait failed");
    ensure (contains_in_order (evidence.value ().evidence, request_id,
                               {"worker-await-started", "worker-await-released",
                                "probe-started", "probe-completed",
                                "worker-await-resumed", "worker-await-completed"}),
            "ATD-A4 marker order mismatch");
    std::cout << "scenario ATD-A4 passed\n";
    return request_id;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
