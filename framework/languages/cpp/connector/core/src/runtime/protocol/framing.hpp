/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/connector_runtime.hpp"

#include <vector>

namespace zlink::stream_connector::detail
{
struct stream_header_t;

std::vector<packet_handler_entry_t> select_packet_handlers (connector_state_t &state,
                                                            const dispatch_envelope_t &envelope);
void dispatch_packet (connector_state_t &state,
                      const dispatch_envelope_t &envelope,
                      const std::vector<packet_handler_entry_t> &handlers);
result_t<dispatch_envelope_t> decode_inbound_packet (connector_state_t &state,
                                                     const stream_header_t &header,
                                                     std::vector<std::uint8_t> payload);

} // namespace zlink::stream_connector::detail
