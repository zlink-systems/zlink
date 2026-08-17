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
    /* Optional pair (flow-correlation §3.2): both present or both absent. */
    std::string flow_id;
    std::optional<flow_origin_t> flow_origin;
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

/* flow_id wire form: lowercase hyphenated UUIDv7, 36 ASCII bytes. */
class flow_id_codec_t
{
  public:
    static constexpr std::uint8_t format_marker = 0xF2;
    static constexpr std::size_t encoded_length = 36;

    static std::string create ();
    static bool is_valid (std::string_view value) noexcept;
};

class header_codec_t
{
  public:
    result_t<std::vector<std::uint8_t>> encode (const stream_header_t &header) const;
    /* validate_flow=false (diagnostics level Off, flow-correlation §4): the
     * flow fields keep their structural length checks but are not validated
     * as flow values and must not be consumed as flow context. */
    result_t<stream_header_t> decode (const std::vector<std::uint8_t> &bytes,
                                      bool validate_flow = true) const;
};

} // namespace zlink::stream_connector::detail
