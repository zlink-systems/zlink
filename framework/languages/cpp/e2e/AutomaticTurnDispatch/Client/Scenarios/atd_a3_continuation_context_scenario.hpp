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
std::string run_atd_a3_continuation_context_scenario (TConnector &connector,
                                                     TConnector &observer,
                                                     const std::string &spot_id)
{
    const auto request_id = unique_id ("ATD-A3");
    connector.send (await_msg_t{.request_id = request_id,
                                .delay_ms = 50,
                                .correlation_id = "corr-a3"})
        .packet_name (await_msg_t::packet_name)
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
    ensure (static_cast<bool> (evidence), "ATD-A3 evidence wait failed");
    ensure (contains_in_order (evidence.value ().evidence, request_id,
                               {"await-started", "await-released", "await-resumed",
                                "await-completed"}),
            "ATD-A3 marker order mismatch");
    ensure (every_request_line_has (evidence.value ().evidence, request_id,
                                    {"spot=" + spot_id, "correlation=corr-a3"}),
            "ATD-A3 context evidence mismatch");
    std::cout << "scenario ATD-A3 passed\n";
    return request_id;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
