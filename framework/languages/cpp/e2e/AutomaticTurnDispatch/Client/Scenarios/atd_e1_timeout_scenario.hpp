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
std::string run_atd_e1_timeout_scenario (TConnector &connector)
{
    const auto spot_id = unique_id ("await-timeout");
    auto spot =
      connector.request (ensure_spot_req_t{.spot_id = spot_id})
        .packet_name (ensure_spot_req_t::packet_name)
        .timeout (std::chrono::milliseconds (30000))
        .template submit<ensure_spot_res_t> ();
    ensure (static_cast<bool> (spot), "ATD-E1 ensure spot failed");
    ensure (spot.value ().spot_id == spot_id, "ATD-E1 ensure spot reply mismatch");

    const auto request_id = unique_id ("ATD-E1");
    connector.send (await_timeout_msg_t{.request_id = request_id,
                                        .delay_ms = 700,
                                        .timeout_ms = 100})
        .packet_name (await_timeout_msg_t::packet_name)
        .metadata (spot_id_metadata, spot_id)
        .submit ();

    auto timeout_evidence =
      connector.request (
                await_evidence_wait_req_t{.request_id = request_id,
                                          .marker = "timeout-await-completed",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (timeout_evidence), "ATD-E1 timeout evidence wait failed");
    ensure_contains_in_order (timeout_evidence.value ().evidence, request_id,
                              {"timeout-await-started",
                               "timeout-await-released",
                               "timeout-await-completed"},
                              "ATD-E1 timeout marker order mismatch");

    connector.send (probe_msg_t{.request_id = request_id, .marker = "timeout-probe"})
        .packet_name (probe_msg_t::packet_name)
        .metadata (spot_id_metadata, spot_id)
        .submit ();

    auto probe_evidence =
      connector.request (
                await_evidence_wait_req_t{.request_id = request_id,
                                          .marker = "probe-completed",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (probe_evidence), "ATD-E1 probe evidence wait failed");
    ensure_contains_in_order (probe_evidence.value ().evidence, request_id,
                              {"timeout-await-completed",
                               "probe-started",
                               "probe-completed"},
                              "ATD-E1 cleanup probe marker order mismatch");
    ensure (every_request_line_has (probe_evidence.value ().evidence, request_id,
                                    {"rid=play-a"}),
            "ATD-E1 evidence node mismatch");

    std::cout << "scenario ATD-E1 passed\n";
    return request_id;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
