/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/connector_runtime.hpp"

#include <vector>

namespace zlink::stream_connector::detail
{

void dispatch_packet (connector_state_t &state, const packet_t &packet);
result_t<std::vector<packet_t>>
drain_available_pushes (connector_state_t &state,
                        const std::shared_ptr<stream_connection_t> &connection);

} // namespace zlink::stream_connector::detail
