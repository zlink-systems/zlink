/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/result.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_models.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::stream_connector::detail
{

struct stream_header_t
{
    message_kind_t kind = message_kind_t::send;
    codec_t codec = codec_t::raw;
    header_flags_t flags = header_flags_t::none;
    std::optional<std::uint64_t> request_seq;
    std::string name;
    metadata_t metadata;
    std::string correlation_id;
    std::optional<std::uint16_t> actor_slot;
};

struct actor_bound_t
{
    std::uint16_t actor_slot = 0;
    std::string actor_id;
};

class actor_binding_control_codec_t
{
  public:
    static constexpr const char *bound_name = "$zlink.actor.bound";
    static constexpr const char *unbound_name = "$zlink.actor.unbound";
    static result_t<actor_bound_t> decode_bound (const std::vector<std::uint8_t> &payload);
    static result_t<std::uint16_t> decode_unbound (const std::vector<std::uint8_t> &payload);
};

/* Versioned session-closing control payload (graceful-drain-handoff §7.1):
 * u8 version=1, u8 reason(1..6), u16 diagnostic length (network order,
 * 0..512), UTF-8 diagnostic bytes. */
struct session_closing_t
{
    close_reason_t reason = close_reason_t::transport_error;
    std::string diagnostic;
};

class session_closing_codec_t
{
  public:
    static constexpr const char *control_name = "session-closing";
    static constexpr std::uint8_t version = 1;
    static constexpr std::size_t max_diagnostic_bytes = 512;

    static result_t<std::vector<std::uint8_t>> encode (const session_closing_t &closing);
    static result_t<session_closing_t> decode (const std::vector<std::uint8_t> &payload);
};

/* The wire marker and flow field width remain for structural decoding. */
class flow_id_codec_t
{
  public:
    static constexpr std::uint8_t format_marker = 0xF2;
    static constexpr std::size_t encoded_length = 36;
};

class header_codec_t
{
  public:
    result_t<std::vector<std::uint8_t>> encode (const stream_header_t &header) const;
    result_t<stream_header_t> decode (const std::vector<std::uint8_t> &bytes) const;
};

} // namespace zlink::stream_connector::detail
