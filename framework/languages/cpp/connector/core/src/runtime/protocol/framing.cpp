/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/protocol/framing.hpp"

#include "runtime/protocol/compression/lz4_compression_codec.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/header_codec.hpp"
#include "runtime/transport/stream_connection.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace zlink::stream_connector::detail
{

namespace
{

zlink::message_t message_from_bytes (const std::vector<std::uint8_t> &bytes)
{
    return zlink::message_t::from (bytes);
}

bool has_flag (header_flags_t flags, header_flags_t flag) noexcept
{
    return (static_cast<std::uint8_t> (flags) & static_cast<std::uint8_t> (flag)) != 0;
}

result_t<std::vector<std::uint8_t>>
read_exact_from_connection (const std::shared_ptr<stream_connection_t> &connection,
                            std::size_t size)
{
    if (!connection) {
        return result_t<std::vector<std::uint8_t>>::failure (error_code_t::disconnected,
                                                             "stream connector is not connected");
    }
    std::vector<std::uint8_t> bytes (size);
    std::size_t offset = 0;
    while (offset < size) {
        boost::system::error_code error;
        const auto read = connection->read_some (bytes.data () + offset, size - offset, error);
        if (error) {
            return result_t<std::vector<std::uint8_t>>::failure (error_code_t::disconnected,
                                                                 error.message ());
        }
        if (read == 0) {
            return result_t<std::vector<std::uint8_t>>::failure (
              error_code_t::disconnected, "stream connector returned a zero-byte read");
        }
        offset += read;
    }
    return result_t<std::vector<std::uint8_t>>::success (std::move (bytes));
}

result_t<packet_t> read_stream_packet (connector_state_t &state,
                                       const std::shared_ptr<stream_connection_t> &connection)
{
    auto prefix_result = read_exact_from_connection (connection, 6);
    if (!prefix_result) {
        return result_t<packet_t>::failure (prefix_result.error ()->code,
                                            prefix_result.error ()->message);
    }
    const auto &prefix = prefix_result.value ();
    const auto header_size = static_cast<std::size_t> ((prefix[0] << 8) | prefix[1]);
    const auto payload_size =
      (static_cast<std::size_t> (prefix[2]) << 24) | (static_cast<std::size_t> (prefix[3]) << 16)
      | (static_cast<std::size_t> (prefix[4]) << 8) | static_cast<std::size_t> (prefix[5]);
    if (!frame_codec_t::validate_receive_frame_size (header_size, payload_size, state.options)) {
        return result_t<packet_t>::failure (error_code_t::frame_too_large,
                                            "Inbound stream frame exceeds configured limits.");
    }
    auto header_result = read_exact_from_connection (connection, header_size);
    if (!header_result) {
        return result_t<packet_t>::failure (header_result.error ()->code,
                                            header_result.error ()->message);
    }
    auto payload_result = read_exact_from_connection (connection, payload_size);
    if (!payload_result) {
        return result_t<packet_t>::failure (payload_result.error ()->code,
                                            payload_result.error ()->message);
    }
    auto header_bytes = std::move (header_result.value ());
    auto payload_bytes = std::move (payload_result.value ());
    const auto diagnostics_level = state.diagnostics_level_cell.load (std::memory_order_acquire);
    auto decoded =
      header_codec_t{}.decode (header_bytes, diagnostics_level != diagnostics_level_t::off);
    if (!decoded) {
        return result_t<packet_t>::failure (decoded.error ()->code, decoded.error ()->message);
    }
    return decode_inbound_packet (state, decoded.value (), std::move (payload_bytes));
}

} // namespace

void dispatch_packet (connector_state_t &state, const packet_t &packet)
{
    std::vector<packet_handler_entry_t> handlers;
    {
        std::lock_guard<std::mutex> lock (state.lifecycle_mutex);
        const auto found = state.packet_handlers.find (packet.name);
        if (found == state.packet_handlers.end ()) {
            return;
        }
        handlers = found->second;
    }
    /* stream-connector §5.5: a send or request started while this handler runs
     * continues the received message's flow. */
    flow_scope_t flow (packet);
    for (const auto &entry : handlers) {
        try {
            entry.handler (packet);
        }
        catch (...) {
        }
    }
}

result_t<std::vector<packet_t>>
drain_available_pushes (connector_state_t &state,
                        const std::shared_ptr<stream_connection_t> &connection)
{
    std::vector<packet_t> packets;
    while (connection && connection->is_open ()) {
        boost::system::error_code error;
        if (connection->available (error) == 0) {
            if (error) {
                return result_t<std::vector<packet_t>>::failure (error_code_t::disconnected,
                                                                 error.message ());
            }
            return result_t<std::vector<packet_t>>::success (std::move (packets));
        }
        auto packet = read_stream_packet (state, connection);
        if (!packet) {
            return result_t<std::vector<packet_t>>::failure (packet.error ()->code,
                                                             packet.error ()->message);
        }
        packets.push_back (std::move (packet.value ()));
    }
    return result_t<std::vector<packet_t>>::success (std::move (packets));
}

} // namespace zlink::stream_connector::detail
