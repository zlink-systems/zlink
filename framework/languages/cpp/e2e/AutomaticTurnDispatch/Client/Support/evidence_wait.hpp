/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/automatic_turn_dispatch_contracts.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <thread>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

template <typename TConnector>
std::optional<await_evidence_res_t>
wait_for_evidence_snapshot (TConnector &connector,
                            const std::string &request_id,
                            const std::string &marker,
                            const std::string &target_node_rid,
                            std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    std::optional<await_evidence_res_t> latest;
    while (std::chrono::steady_clock::now () < deadline) {
        auto result =
          connector.request (await_evidence_req_t{.request_id = request_id})
            .packet_name (await_evidence_req_t::packet_name)
            .metadata (target_node_rid_metadata, target_node_rid)
            .timeout (std::chrono::milliseconds (5000))
            .template submit<await_evidence_res_t> ();
        if (static_cast<bool> (result)) {
            latest = result.value ();
            for (const auto &line : latest->evidence) {
                if (line.find ("request=" + request_id) != std::string::npos
                    && line.find (marker) != std::string::npos) {
                    return latest;
                }
            }
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    (void) latest;
    return std::nullopt;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
