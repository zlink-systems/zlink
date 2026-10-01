/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/monitoring/framework_runtime.hpp>
#include <zlink/framework/contracts/monitoring/route_mesh_runtime.hpp>
#include <utility>

namespace zlink::framework::detail
{

inline bool topology_is_terminal (topology_state_t state) noexcept
{
    return state == topology_state_t::stopped || state == topology_state_t::failed;
}

inline topology_state_t topology_state_for_host (framework_runtime_state_t state,
                                                 topology_state_t transport_state) noexcept
{
    switch (state) {
        case framework_runtime_state_t::preparing:
            return topology_state_t::starting;
        case framework_runtime_state_t::serving:
            return transport_state;
        case framework_runtime_state_t::relocating:
        case framework_runtime_state_t::relocated:
        case framework_runtime_state_t::draining:
            return topology_state_t::stopping;
        case framework_runtime_state_t::stopped:
            return topology_state_t::stopped;
        case framework_runtime_state_t::error:
            return topology_state_t::failed;
    }
    return topology_state_t::failed;
}

template <typename THostState>
mesh_peer_snapshot_t project_topology_peer (zlink::routing_id_t node_rid,
                                            THostState host_state,
                                            bool ready,
                                            bool connecting,
                                            topology_reason_t unavailable_reason)
{
    bool draining = host_state == THostState::draining;
    if constexpr (requires {
                      THostState::relocating;
                      THostState::relocated;
                  })
        draining =
          draining || host_state == THostState::relocating || host_state == THostState::relocated;
    if constexpr (requires { THostState::retiring; })
        draining = draining || host_state == THostState::retiring;
    const auto state = draining     ? peer_state_t::draining
                       : ready      ? peer_state_t::ready
                       : connecting ? peer_state_t::connecting
                                    : peer_state_t::not_connected;
    return {.node_rid = std::move (node_rid),
            .state = state,
            .unavailable_reason = draining ? std::optional{topology_reason_t::draining}
                                  : ready  ? std::nullopt
                                           : std::optional{unavailable_reason}};
}

} // namespace zlink::framework::detail
