/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/errors/result.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace zlink::framework::detail
{
// Borrows sender fields only for the synchronous measure/write call.
struct stream_header_view_t
{
    const std::map<std::string, std::string> &metadata;
    stream_message_kind_t kind;
    stream_codec_t codec;
    stream_header_flags_t flags;
    std::optional<std::uint64_t> request_seq;
    std::string_view packet_name;
    std::optional<std::string_view> correlation_id;
    std::optional<std::string_view> flow_id;
    std::optional<flow_origin_t> flow_origin;
    std::optional<std::uint16_t> actor_slot;
};

void propagate_stream_header_flow (stream_header_view_t &header);
result_t<zlink::message_t> encode_stream_header_message (const stream_header_view_t &header);
} // namespace zlink::framework::detail
