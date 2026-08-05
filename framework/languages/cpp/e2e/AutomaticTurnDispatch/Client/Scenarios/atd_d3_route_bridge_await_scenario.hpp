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
std::string run_atd_d3_route_bridge_await_scenario (TConnector &connector)
{
    const auto request_id = unique_id ("ATD-D3");
    const auto spot_id = unique_id ("await-route-bridge");
    auto spot =
      connector.request (ensure_spot_req_t{.spot_id = spot_id})
        .packet_name (ensure_spot_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-b")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<ensure_spot_res_t> ();
    ensure (static_cast<bool> (spot), "ATD-D3 ensure spot failed");
    ensure (spot.value ().spot_id == spot_id, "ATD-D3 ensure spot reply mismatch");

    connector.send (await_msg_t{.request_id = request_id,
                                .delay_ms = 1500,
                                .correlation_id = "route-bridge"})
        .packet_name (await_msg_t::packet_name)
        .metadata (spot_id_metadata, spot_id)
        .metadata (target_node_rid_metadata, "play-b")
        .submit ();
    auto released =
      connector.request (
                await_evidence_wait_req_t{.request_id = request_id,
                                          .marker = "await-released",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-b")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (released), "ATD-D3 await-released wait failed");
    ensure (contains_request_marker (released.value ().evidence, request_id,
                                     "await-released"),
            "ATD-D3 await-released marker was not observed");

    connector.send (probe_msg_t{.request_id = request_id, .marker = "route-bridge-probe"})
        .packet_name (probe_msg_t::packet_name)
        .metadata (spot_id_metadata, spot_id)
        .metadata (target_node_rid_metadata, "play-b")
        .submit ();

    auto evidence =
      connector.request (
                await_evidence_wait_req_t{.request_id = request_id,
                                          .marker = "await-completed",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-b")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (evidence), "ATD-D3 evidence wait failed");
    ensure (contains_in_order (evidence.value ().evidence, request_id,
                               {"await-started", "await-released", "probe-started",
                                "probe-completed", "await-resumed", "await-completed"}),
            "ATD-D3 marker order mismatch");

    bool saw_target_spot = false;
    for (const auto &line : evidence.value ().evidence) {
        if (line.find ("await-started|rid=play-b") != std::string::npos
            && line.find ("spot=" + spot_id) != std::string::npos
            && line.find ("handler=spot") != std::string::npos) {
            saw_target_spot = true;
        }
    }
    ensure (saw_target_spot, "ATD-D3 target Spot handler marker missing");
    std::cout << "scenario ATD-D3 passed\n";
    return request_id;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
