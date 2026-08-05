/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/automatic_turn_dispatch_contracts.hpp"
#include "../Support/evidence_wait.hpp"
#include "../Support/scenario_assert.hpp"

#include <chrono>
#include <iostream>
#include <string>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

template <typename TConnector>
std::string run_atd_d2_remote_spot_await_scenario (TConnector &connector)
{
    const auto owner_spot_id = unique_id ("await-remote-owner");
    const auto target_spot_id = unique_id ("await-remote-target");
    auto owner =
      connector.request (ensure_spot_req_t{.spot_id = owner_spot_id})
        .packet_name (ensure_spot_req_t::packet_name)
        .timeout (std::chrono::milliseconds (15000))
        .template submit<ensure_spot_res_t> ();
    ensure (static_cast<bool> (owner), "ATD-D2 owner spot ensure failed");
    auto target =
      connector.request (ensure_spot_req_t{.spot_id = target_spot_id})
        .packet_name (ensure_spot_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-b")
        .timeout (std::chrono::milliseconds (15000))
        .template submit<ensure_spot_res_t> ();
    ensure (static_cast<bool> (target), "ATD-D2 target spot ensure failed");
    const auto request_id = unique_id ("ATD-D2");
    auto remote_reply =
      connector.request (remote_spot_await_req_t{.request_id = request_id,
                                                 .target_spot_id = target_spot_id,
                                                 .delay_ms = 350})
        .packet_name (remote_spot_await_req_t::packet_name)
        .metadata (spot_id_metadata, owner_spot_id)
        .timeout (std::chrono::milliseconds (30000))
        .template submit<automatic_turn_dispatch_res_t> ();
    ensure (static_cast<bool> (remote_reply), "ATD-D2 RemoteSpotYieldReq failed");
    ensure (remote_reply.value ().scenario_id == "ATD-D2",
            "ATD-D2 reply scenario mismatch");
    ensure (remote_reply.value ().node_rid == "play-a",
            "ATD-D2 caller continuation node mismatch");
    auto owner_evidence =
      wait_for_evidence_snapshot (connector, request_id, "remote-await-completed", "play-a",
                                  std::chrono::milliseconds (20000));
    ensure (static_cast<bool> (owner_evidence), "ATD-D2 owner evidence wait failed");
    ensure_contains_in_order (owner_evidence.value ().evidence, request_id,
                              {"remote-await-started",
                               "remote-await-released",
                               "remote-await-resumed",
                               "remote-await-completed"},
                              "ATD-D2 owner marker order mismatch");
    ensure (every_request_line_has (owner_evidence.value ().evidence, request_id,
                                    {"rid=play-a"}),
            "ATD-D2 owner evidence node mismatch");
    bool saw_target_node = false;
    for (const auto &line : owner_evidence.value ().evidence) {
        if (line.find ("remote-await-resumed") != std::string::npos
            && line.find ("targetNode=play-b") != std::string::npos) {
            saw_target_node = true;
        }
    }
    ensure (saw_target_node, "ATD-D2 target node marker missing");
    auto target_evidence =
      connector.request (await_evidence_req_t{.request_id = request_id})
        .packet_name (await_evidence_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-b")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (target_evidence), "ATD-D2 target evidence request failed");
    bool saw_target_yield = false;
    for (const auto &line : target_evidence.value ().evidence) {
        if (line.find ("await-started|rid=play-b") != std::string::npos
            && line.find ("spot=" + target_spot_id) != std::string::npos) {
            saw_target_yield = true;
        }
        ensure (line.find ("remote-await-resumed|rid=play-b") == std::string::npos,
                "ATD-D2 target node owned caller continuation");
    }
    ensure (saw_target_yield, "ATD-D2 target play-b marker missing");
    std::cout << "scenario ATD-D2 passed\n";
    return request_id;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
