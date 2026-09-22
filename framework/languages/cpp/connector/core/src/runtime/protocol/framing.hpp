/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/connector_runtime.hpp"

#include <vector>

namespace zlink::stream_connector::detail
{
struct stream_header_t;

void dispatch_packet (connector_state_t &state, const dispatch_envelope_t &envelope);
result_t<dispatch_envelope_t> decode_inbound_packet (connector_state_t &state,
                                                     const stream_header_t &header,
                                                     std::vector<std::uint8_t> payload);
result_t<std::vector<dispatch_envelope_t>>
drain_available_pushes (connector_state_t &state,
                        const std::shared_ptr<stream_connection_t> &connection);

} // namespace zlink::stream_connector::detail
