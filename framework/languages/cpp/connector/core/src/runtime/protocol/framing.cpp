/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/protocol/framing.hpp"

#include "runtime/protocol/compression/lz4_compression_codec.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/header_codec.hpp"
#include "runtime/transport/stream_connection.hpp"

#include <algorithm>
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

void publish_observer_error (const std::vector<std::function<void (const error_t &)>> &handlers,
                             const error_t &error) noexcept
{
    for (const auto &handler : handlers) {
        try {
            handler (error);
        }
        catch (...) {
        }
    }
}

void enqueue_inbound_observer_notification (connector_state_t &state,
                                            const stream_header_t &header,
                                            std::size_t payload_length,
                                            const std::vector<std::uint8_t> &payload_bytes)
{
    std::vector<std::shared_ptr<inbound_observer_entry_t>> observers;
    for (const auto &observer : state.inbound_observers) {
        if (observer && observer->active.load () && observer->callback) {
            observers.push_back (observer);
        }
    }
    if (observers.empty ()) {
        return;
    }
    const auto pending =
      state.pending_inbound_observer_notifications.fetch_add (1) + static_cast<std::size_t> (1);
    if (pending > state.options.max_inbound_observer_notifications) {
        state.pending_inbound_observer_notifications.fetch_sub (1);
        if (!state.inbound_observer_drop_report_pending.exchange (true)) {
            publish_observer_error (
              state.error_handlers,
              error_t{
                error_code_t::observer_dropped,
                "Inbound observer notification was dropped because the observer queue is full."});
            state.inbound_observer_drop_report_pending.store (false);
        }
        return;
    }
    auto error_handlers = state.error_handlers;
    inbound_observation_t observation;
    observation.kind = header.kind;
    observation.name = header.name;
    observation.codec = header.codec;
    observation.request_seq = header.request_seq;
    observation.metadata = header.metadata;
    observation.payload_length = payload_length;
    observation.compressed = has_flag (header.flags, header_flags_t::payload_compressed);
    observation.received_at = std::chrono::steady_clock::now ();
    const auto preview_length =
      std::min (state.options.max_inbound_observer_payload_preview_bytes, payload_bytes.size ());
    observation.payload_preview.assign (payload_bytes.begin (),
                                        payload_bytes.begin ()
                                          + static_cast<std::ptrdiff_t> (preview_length));
    auto shared_state = state.shared_from_this ();
    post_runtime_operation ([state = std::move (shared_state), observers = std::move (observers),
                             error_handlers = std::move (error_handlers),
                             observation = std::move (observation)] () mutable {
        for (const auto &observer : observers) {
            if (!observer || !observer->active.load ()) {
                continue;
            }
            try {
                observer->callback (observation);
            }
            catch (const std::exception &ex) {
                publish_observer_error (error_handlers, error_t{error_code_t::observer_failed,
                                                                "Inbound observer callback failed: "
                                                                  + std::string (ex.what ())});
            }
            catch (...) {
                publish_observer_error (
                  error_handlers,
                  error_t{error_code_t::observer_failed, "Inbound observer callback failed."});
            }
        }
        state->pending_inbound_observer_notifications.fetch_sub (1);
    });
}

result_t<std::vector<std::uint8_t>>
read_exact_from_connection (const std::shared_ptr<stream_connection_t> &connection,
                            std::size_t size)
{
    if (!connection) {
        return result_t<std::vector<std::uint8_t>>::failure (
          error_code_t::disconnected, "stream connector is not connected");
    }
    std::vector<std::uint8_t> bytes (size);
    std::size_t offset = 0;
    while (offset < size) {
        boost::system::error_code error;
        const auto read = connection->read_some (bytes.data () + offset, size - offset, error);
        if (error) {
            return result_t<std::vector<std::uint8_t>>::failure (
              error_code_t::disconnected, error.message ());
        }
        if (read == 0) {
            return result_t<std::vector<std::uint8_t>>::failure (
              error_code_t::disconnected, "stream connector returned a zero-byte read");
        }
        offset += read;
    }
    return result_t<std::vector<std::uint8_t>>::success (std::move (bytes));
}

result_t<packet_t> read_stream_packet (
  connector_state_t &state,
  const std::shared_ptr<stream_connection_t> &connection)
{
    auto prefix_result = read_exact_from_connection (connection, 6);
    if (!prefix_result) {
        return result_t<packet_t>::failure (
          prefix_result.error_code (), prefix_result.error ()->message);
    }
    const auto &prefix = prefix_result.value ();
    const auto header_size = static_cast<std::size_t> ((prefix[0] << 8) | prefix[1]);
    const auto payload_size =
      (static_cast<std::size_t> (prefix[2]) << 24) | (static_cast<std::size_t> (prefix[3]) << 16)
      | (static_cast<std::size_t> (prefix[4]) << 8) | static_cast<std::size_t> (prefix[5]);
    if (!frame_codec_t::validate_receive_frame_size (header_size, payload_size, state.options)) {
        return result_t<packet_t>::failure (
          error_code_t::frame_too_large, "Inbound stream frame exceeds configured limits.");
    }
    auto header_result = read_exact_from_connection (connection, header_size);
    if (!header_result) {
        return result_t<packet_t>::failure (
          header_result.error_code (), header_result.error ()->message);
    }
    auto payload_result = read_exact_from_connection (connection, payload_size);
    if (!payload_result) {
        return result_t<packet_t>::failure (
          payload_result.error_code (), payload_result.error ()->message);
    }
    auto header_bytes = std::move (header_result.value ());
    auto payload_bytes = std::move (payload_result.value ());
    auto decoded = header_codec_t{}.decode (header_bytes);
    if (!decoded) {
        return result_t<packet_t>::failure (decoded.error_code (), decoded.error ()->message);
    }
    auto header = decoded.value ();
    enqueue_inbound_observer_notification (state, header, payload_size, payload_bytes);
    state.last_inbound_received = std::chrono::steady_clock::now ();
    const bool compressed = has_flag (header.flags, header_flags_t::payload_compressed);
    zlink::message_t payload;
    try {
        payload = message_from_bytes (payload_bytes);
    }
    catch (const std::exception &error) {
        return result_t<packet_t>::failure (error_code_t::frame_decode_failed, error.what ());
    }
    if (compressed) {
        if (!state.compression_codec) {
            return result_t<packet_t>::failure (
              error_code_t::decompression_failed,
              "stream connector compression codec is not configured");
        }
        if (state.options.compression == compression_t::lz4 && !state.lz4_enabled) {
            return result_t<packet_t>::failure (error_code_t::decompression_failed,
                                                "LZ4 compression is not enabled");
        }
        try {
            payload = state.compression_codec->decompress (
              payload, state.options.max_receive_payload_size);
            if (payload.size () > state.options.max_receive_payload_size) {
                return result_t<packet_t>::failure (
                  error_code_t::decompression_failed,
                  "decompressed stream payload exceeds maximum stream payload size");
            }
        }
        catch (const std::exception &error) {
            return result_t<packet_t>::failure (error_code_t::decompression_failed,
                                                error.what ());
        }
    }
    if (header.kind == message_kind_t::control && header.name == "$zlink.heartbeat.ping") {
        state.heartbeat_pong_due = true;
    }
    packet_t packet;
    packet.name = std::move (header.name);
    packet.metadata = std::move (header.metadata);
    packet.codec = header.codec;
    packet.compressed = compressed;
    packet.payload = std::move (payload);
    return result_t<packet_t>::success (std::move (packet));
}

} // namespace

void dispatch_packet (connector_state_t &state, const packet_t &packet)
{
    const auto found = state.packet_handlers.find (packet.name);
    if (found == state.packet_handlers.end ()) {
        return;
    }
    for (const auto &handler : found->second) {
        handler (packet);
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
                return result_t<std::vector<packet_t>>::failure (
                  error_code_t::disconnected, error.message ());
            }
            return result_t<std::vector<packet_t>>::success (std::move (packets));
        }
        auto packet = read_stream_packet (state, connection);
        if (!packet) {
            return result_t<std::vector<packet_t>>::failure (
              packet.error_code (), packet.error ()->message);
        }
        packets.push_back (std::move (packet.value ()));
    }
    return result_t<std::vector<packet_t>>::success (std::move (packets));
}

} // namespace zlink::stream_connector::detail
