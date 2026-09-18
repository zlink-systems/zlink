/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_enums.hpp>

#include <map>
#include <optional>
#include <string>
#include <utility>

namespace zlink::stream_connector
{

struct error_t
{
    error_code_t code = error_code_t::disconnected;
    std::string message;
};

struct metadata_t
{
    std::map<std::string, std::string> values;

    metadata_t &with (std::string key, std::string value)
    {
        values[std::move (key)] = std::move (value);
        return *this;
    }
};

struct packet_t
{
    std::string name;
    metadata_t metadata;
    codec_t codec = codec_t::raw;
    bool compressed = false;
    zlink::message_t payload;
    /* Flow of the received message (stream-connector §5.5). Both values are
     * empty when the diagnostics level is off (§13), and on an outbound packet
     * the connector fills them at encode time. */
    std::string flow_id;
    std::optional<flow_origin_t> flow_origin;
};

/// A received message: the decoded payload with everything the receiving code
/// needs to place it (stream-connector §5.5).
///
/// `flow_id` and `flow_origin` are empty when the diagnostics level is off
/// (§13). Predicates and returns of the wait surfaces deal in this type, not in
/// the payload alone, so a predicate can also read the packet name and the
/// metadata (§10.1).
template <typename TPayload> struct message_t
{
    std::string packet_name;
    TPayload payload{};
    metadata_t metadata;
    std::string flow_id;
    std::optional<flow_origin_t> flow_origin;
};

struct connection_state_changed_t
{
    connection_state_t previous = connection_state_t::created;
    connection_state_t current = connection_state_t::created;
    std::optional<error_t> error;
    /* Last observed close reason: set from a received `session-closing`
     * control before the server closes, or synthesized by the connector. */
    std::optional<close_reason_t> close_reason;
};

} // namespace zlink::stream_connector
