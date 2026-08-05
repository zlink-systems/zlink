/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/automatic_turn_dispatch_contracts.hpp"
#include "../Support/scenario_assert.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

template <typename TConnector>
std::string run_atd_a1_basic_terminator_scenario (TConnector &connector,
                                                 const std::string &spot_id)
{
    const auto request_id = unique_id ("ATD-A1");
    auto hold = std::async (std::launch::async, [&] {
        return connector.request (hold_req_t{.request_id = request_id, .delay_ms = 350})
          .packet_name (hold_req_t::packet_name)
          .metadata (spot_id_metadata, spot_id)
          .timeout (std::chrono::milliseconds (10000))
          .template submit<automatic_turn_dispatch_res_t> ();
    });
    std::this_thread::sleep_for (std::chrono::milliseconds (75));
    auto hold_probe =
      connector.request (probe_req_t{.request_id = request_id, .marker = "hold-probe"})
        .packet_name (probe_req_t::packet_name)
        .metadata (spot_id_metadata, spot_id)
        .timeout (std::chrono::milliseconds (10000))
        .template submit<automatic_turn_dispatch_res_t> ();
    ensure (static_cast<bool> (hold_probe), "ATD-A1 ProbeMsg send failed");
    auto hold_reply = hold.get ();
    ensure (static_cast<bool> (hold_reply), "ATD-A1 HoldReq failed");
    auto evidence =
      connector.request (
                 await_evidence_wait_req_t{.request_id = request_id,
                                           .marker = "probe-completed",
                                           .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (evidence), "ATD-A1 evidence wait failed");
    ensure (contains_in_order (evidence.value ().evidence, request_id,
                               {"hold-started", "hold-resumed", "hold-completed",
                                "probe-started"}),
            "ATD-A1 marker order mismatch");
    std::cout << "scenario ATD-A1 passed\n";
    return request_id;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
