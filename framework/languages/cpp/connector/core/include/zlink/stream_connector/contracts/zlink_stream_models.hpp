/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/zlink_stream_enums.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
    std::vector<std::uint8_t> payload;
    std::optional<std::string> actor_id;
};

class request_sending_context_t
{
  public:
    request_sending_context_t (std::string name,
                               std::optional<std::string> actor,
                               metadata_t &metadata) :
        request_packet_name (std::move (name)), actor_id (std::move (actor)), _metadata (metadata)
    {
    }

    const std::string request_packet_name;
    const std::optional<std::string> actor_id;

    void set_metadata (std::string key, std::string value)
    {
        _metadata.with (std::move (key), std::move (value));
    }

  private:
    metadata_t &_metadata;
};

struct reply_received_context_t
{
    std::string request_packet_name;
    std::optional<std::string> actor_id;
    bool succeeded = false;
    std::optional<packet_t> reply;
    std::optional<error_t> error;
    std::chrono::milliseconds elapsed{0};
};

/// A received message: the decoded payload with everything the receiving code
/// needs to place it (stream-connector §5.5).
///
/// Predicates and returns of the wait surfaces deal in this type, not in
/// the payload alone, so a predicate can also read the packet name and the
/// metadata (§10.1).
template <typename TPayload> struct message_t
{
    std::string packet_name;
    TPayload payload{};
    metadata_t metadata;
    std::optional<std::string> actor_id;
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
