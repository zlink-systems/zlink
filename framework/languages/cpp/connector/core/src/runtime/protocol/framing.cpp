/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/protocol/framing.hpp"

#include <exception>
#include <vector>

namespace zlink::stream_connector::detail
{

std::vector<packet_handler_entry_t> select_packet_handlers (connector_state_t &state,
                                                            const dispatch_envelope_t &envelope)
{
    std::vector<packet_handler_entry_t> handlers;
    {
        std::lock_guard<std::mutex> lock (state.lifecycle_mutex);
        const auto found = state.packet_handlers.find (envelope.packet.name);
        if (found == state.packet_handlers.end ()) {
            return handlers;
        }
        for (const auto &entry : found->second) {
            if (entry.takes (envelope)) {
                handlers.push_back (entry);
            }
        }
    }
    return handlers;
}

void dispatch_packet (connector_state_t &state,
                      const dispatch_envelope_t &envelope,
                      const std::vector<packet_handler_entry_t> &handlers)
{
    for (const auto &entry : handlers) {
        try {
            entry.handler (envelope);
        }
        catch (const std::exception &error) {
            publish_error (state, {error_code_t::user_callback_failed, error.what ()});
        }
        catch (...) {
            publish_error (state, {error_code_t::user_callback_failed, "packet handler failed"});
        }
    }
}

} // namespace zlink::stream_connector::detail
