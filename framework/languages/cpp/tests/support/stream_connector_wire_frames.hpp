/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/header_codec.hpp"
#include <zlink/stream_connector.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace stream_connector_fixture
{
inline std::vector<std::uint8_t> as_bytes (std::string_view text)
{
    return {text.begin (), text.end ()};
}

inline std::string as_string (const std::vector<std::uint8_t> &bytes)
{
    return {bytes.begin (), bytes.end ()};
}

struct server_frame_t
{
    zlink::stream_connector::detail::stream_header_t header;
    std::string payload;
    bool compressed = false;
};

inline std::optional<server_frame_t> try_read_server_frame (std::string &buffer)
{
    if (buffer.size () < 6) {
        return std::nullopt;
    }
    const auto header_size =
      (static_cast<std::uint8_t> (buffer[0]) << 8) | static_cast<std::uint8_t> (buffer[1]);
    const auto payload_size =
      (static_cast<std::size_t> (static_cast<std::uint8_t> (buffer[2])) << 24)
      | (static_cast<std::size_t> (static_cast<std::uint8_t> (buffer[3])) << 16)
      | (static_cast<std::size_t> (static_cast<std::uint8_t> (buffer[4])) << 8)
      | static_cast<std::uint8_t> (buffer[5]);
    if (buffer.size () < 6 + header_size + payload_size) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> header_bytes (
      buffer.begin () + 6, buffer.begin () + 6 + static_cast<std::ptrdiff_t> (header_size));
    auto decoded = zlink::stream_connector::detail::header_codec_t{}.decode (header_bytes);
    if (!decoded) {
        return std::nullopt;
    }
    std::string payload (buffer.begin () + 6 + static_cast<std::ptrdiff_t> (header_size),
                         buffer.begin () + 6 + static_cast<std::ptrdiff_t> (header_size)
                           + static_cast<std::ptrdiff_t> (payload_size));
    buffer.erase (0, 6 + header_size + payload_size);
    const bool compressed =
      (static_cast<std::uint8_t> (decoded.value ().flags)
       & static_cast<std::uint8_t> (zlink::stream_connector::header_flags_t::payload_compressed))
      != 0;
    if (compressed) {
        payload = as_string (zlink::stream_connector::lz4_compression_codec ()->decompress (
          as_bytes (payload), 64 * 1024));
    }
    return server_frame_t{decoded.value (), std::move (payload), compressed};
}

inline zlink::message_t make_server_frame (zlink::stream_connector::message_kind_t kind,
                                           std::uint64_t seq,
                                           std::string name,
                                           std::string payload,
                                           bool compressed = false,
                                           std::optional<std::uint16_t> actor_slot = std::nullopt)
{
    const bool reply = kind == zlink::stream_connector::message_kind_t::response
                       || kind == zlink::stream_connector::message_kind_t::error;
    zlink::stream_connector::detail::stream_header_t header;
    header.kind = kind;
    header.codec = kind == zlink::stream_connector::message_kind_t::error
                     ? zlink::stream_connector::codec_t::json
                     : zlink::stream_connector::codec_t::raw;
    header.flags = compressed ? zlink::stream_connector::header_flags_t::payload_compressed
                              : zlink::stream_connector::header_flags_t::none;
    header.request_seq = kind == zlink::stream_connector::message_kind_t::request
                             || kind == zlink::stream_connector::message_kind_t::response
                             || kind == zlink::stream_connector::message_kind_t::error
                           ? std::optional<std::uint64_t>{seq}
                           : std::optional<std::uint64_t>{};
    header.name = reply ? std::string{} : name;
    header.actor_slot = actor_slot;
    auto header_bytes = zlink::stream_connector::detail::header_codec_t{}.encode (header);
    zlink::stream_connector::connector_options_t options;
    options.compression = zlink::stream_connector::compression_t::lz4;
    if (compressed) {
        payload = as_string (
          zlink::stream_connector::lz4_compression_codec ()->compress (as_bytes (payload)));
    }
    std::vector<std::uint8_t> payload_bytes (payload.begin (), payload.end ());
    options.max_send_payload_size = std::max (options.max_send_payload_size, payload_bytes.size ());
    auto frame = zlink::stream_connector::detail::frame_codec_t::encode (header_bytes.value (),
                                                                         payload_bytes, options);
    return zlink::message_t::from (std::string (frame.value ().begin (), frame.value ().end ()));
}
} // namespace stream_connector_fixture
