/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/protocol/service_wire_codec.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <type_traits>

namespace zlink::framework::runtime::protocol
{
namespace
{

constexpr std::size_t liveness_size = 13;
constexpr std::size_t prefix_size = 5;
constexpr auto relocation_resource_participant_limit =
  static_cast<std::size_t> (relocationResourceParticipants);
constexpr std::uint8_t application_payload_version = 1;
constexpr std::uint8_t application_payload_flow_version = 2;

void append_nonzero_u64 (std::vector<std::uint8_t> &bytes,
                         std::uint64_t value,
                         const char *field);
std::uint64_t read_nonzero_u64 (std::span<const std::uint8_t> bytes,
                                std::size_t &offset,
                                const char *field);

bool valid_flow_id (std::string_view value) noexcept
{
    if (value.size () != 36)
        return false;
    for (std::size_t index = 0; index < value.size (); ++index) {
        const char ch = value[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (ch != '-')
                return false;
            continue;
        }
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (!hex)
            return false;
    }
    if (value[14] != '7')
        return false;
    const char variant = value[19];
    return variant == '8' || variant == '9' || variant == 'a' || variant == 'b';
}

bool valid_flow_origin (std::uint8_t value) noexcept
{
    return value >= static_cast<std::uint8_t> (flow_origin_t::inbound)
           && value <= static_cast<std::uint8_t> (flow_origin_t::lifecycle);
}

std::uint32_t crc32c (std::span<const std::uint8_t> payload) noexcept
{
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : payload) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t> (
              -static_cast<std::int32_t> (crc & 1u));
            crc = (crc >> 1u) ^ (0x82f63b78u & mask);
        }
    }
    return ~crc;
}

void append_u32 (std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    bytes.push_back (static_cast<std::uint8_t> ((value >> 24u) & 0xffu));
    bytes.push_back (static_cast<std::uint8_t> ((value >> 16u) & 0xffu));
    bytes.push_back (static_cast<std::uint8_t> ((value >> 8u) & 0xffu));
    bytes.push_back (static_cast<std::uint8_t> (value & 0xffu));
}

void append_u16 (std::vector<std::uint8_t> &bytes, std::uint16_t value)
{
    bytes.push_back (static_cast<std::uint8_t> ((value >> 8u) & 0xffu));
    bytes.push_back (static_cast<std::uint8_t> (value & 0xffu));
}

void append_u64 (std::vector<std::uint8_t> &bytes, std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index) {
        bytes.push_back (static_cast<std::uint8_t> (
          (value >> ((7 - index) * 8)) & 0xffu));
    }
}

std::uint16_t read_u16 (std::span<const std::uint8_t> bytes,
                        std::size_t &offset)
{
    if (bytes.size () - offset < 2) {
        throw service_wire_error_t ("truncated u16 field");
    }
    const auto value = static_cast<std::uint16_t> (
      (static_cast<std::uint16_t> (bytes[offset]) << 8u)
      | bytes[offset + 1]);
    offset += 2;
    return value;
}

std::uint32_t read_u32 (std::span<const std::uint8_t> bytes, std::size_t &offset)
{
    if (bytes.size () - offset < 4) {
        throw service_wire_error_t ("truncated u32 field");
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value = (value << 8u) | bytes[offset++];
    }
    return value;
}

std::uint64_t read_u64 (std::span<const std::uint8_t> bytes,
                        std::size_t &offset)
{
    if (bytes.size () - offset < 8) {
        throw service_wire_error_t ("truncated u64 field");
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8u) | bytes[offset++];
    }
    return value;
}

bool valid_utf8 (std::span<const std::uint8_t> bytes)
{
    for (std::size_t index = 0; index < bytes.size ();) {
        const auto first = bytes[index];
        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7f) {
            if (first == 0) {
                return false;
            }
            ++index;
            continue;
        }
        if ((first & 0xe0u) == 0xc0u) {
            continuation = 1;
            codepoint = first & 0x1fu;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuation = 2;
            codepoint = first & 0x0fu;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuation = 3;
            codepoint = first & 0x07u;
        } else {
            return false;
        }
        if (bytes.size () - index - 1 < continuation) {
            return false;
        }
        for (std::size_t part = 0; part < continuation; ++part) {
            const auto next = bytes[index + part + 1];
            if ((next & 0xc0u) != 0x80u) {
                return false;
            }
            codepoint = (codepoint << 6u) | (next & 0x3fu);
        }
        if ((continuation == 1 && codepoint < 0x80)
            || (continuation == 2 && codepoint < 0x800)
            || (continuation == 3 && codepoint < 0x10000)
            || codepoint > 0x10ffff
            || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
        index += continuation + 1;
    }
    return true;
}

void append_text8 (std::vector<std::uint8_t> &bytes,
                   const std::string &value,
                   const char *field)
{
    if (value.empty () || value.size () > std::numeric_limits<std::uint8_t>::max ()
        || !valid_utf8 (std::span<const std::uint8_t> (
          reinterpret_cast<const std::uint8_t *> (value.data ()), value.size ()))) {
        throw service_wire_error_t (std::string (field)
                                    + " must be nonempty bounded UTF-8 without NUL");
    }
    bytes.push_back (static_cast<std::uint8_t> (value.size ()));
    bytes.insert (bytes.end (), value.begin (), value.end ());
}

std::string read_text8 (std::span<const std::uint8_t> bytes,
                        std::size_t &offset,
                        const char *field)
{
    if (offset >= bytes.size ()) {
        throw service_wire_error_t (std::string ("truncated ") + field);
    }
    const auto length = bytes[offset++];
    if (length == 0 || bytes.size () - offset < length) {
        throw service_wire_error_t (std::string ("invalid ") + field);
    }
    const auto value_bytes = bytes.subspan (offset, length);
    if (!valid_utf8 (value_bytes)) {
        throw service_wire_error_t (std::string ("invalid UTF-8 in ") + field);
    }
    offset += length;
    return std::string (
      reinterpret_cast<const char *> (value_bytes.data ()), value_bytes.size ());
}

void append_bytes8 (std::vector<std::uint8_t> &bytes,
                    const std::vector<std::uint8_t> &value,
                    const char *field)
{
    if (value.empty ()
        || value.size () > std::numeric_limits<std::uint8_t>::max ()) {
        throw service_wire_error_t (
          std::string (field) + " must contain 1..255 bytes");
    }
    bytes.push_back (static_cast<std::uint8_t> (value.size ()));
    bytes.insert (bytes.end (), value.begin (), value.end ());
}

std::vector<std::uint8_t> read_bytes8 (
  std::span<const std::uint8_t> bytes,
  std::size_t &offset,
  const char *field)
{
    if (offset >= bytes.size ()) {
        throw service_wire_error_t (std::string ("truncated ") + field);
    }
    const auto length = bytes[offset++];
    if (length == 0 || bytes.size () - offset < length) {
        throw service_wire_error_t (std::string ("invalid ") + field);
    }
    std::vector<std::uint8_t> value (
      bytes.begin () + static_cast<std::ptrdiff_t> (offset),
      bytes.begin () + static_cast<std::ptrdiff_t> (offset + length));
    offset += length;
    return value;
}

void append_text16 (std::vector<std::uint8_t> &bytes,
                    const std::string &value,
                    const char *field)
{
    if (value.empty () || value.size () > 4096
        || !valid_utf8 (std::span<const std::uint8_t> (
          reinterpret_cast<const std::uint8_t *> (value.data ()), value.size ()))) {
        throw service_wire_error_t (std::string (field)
                                    + " must be nonempty bounded UTF-8 without NUL");
    }
    append_u16 (bytes, static_cast<std::uint16_t> (value.size ()));
    bytes.insert (bytes.end (), value.begin (), value.end ());
}

std::string read_text16 (std::span<const std::uint8_t> bytes,
                         std::size_t &offset,
                         const char *field)
{
    const auto length = read_u16 (bytes, offset);
    if (length == 0 || length > 4096 || bytes.size () - offset < length) {
        throw service_wire_error_t (std::string ("invalid ") + field);
    }
    const auto value_bytes = bytes.subspan (offset, length);
    if (!valid_utf8 (value_bytes)) {
        throw service_wire_error_t (std::string ("invalid UTF-8 in ") + field);
    }
    offset += length;
    return std::string (
      reinterpret_cast<const char *> (value_bytes.data ()), value_bytes.size ());
}

void append_tlv (std::vector<std::uint8_t> &extension,
                 std::uint8_t id,
                 const std::vector<std::uint8_t> &value)
{
    extension.push_back (id);
    append_u32 (extension, static_cast<std::uint32_t> (value.size ()));
    extension.insert (extension.end (), value.begin (), value.end ());
}

std::uint8_t runtime_state_wire (mesh::service_node_state_t state)
{
    switch (state) {
        case mesh::service_node_state_t::preparing:
            return 0;
        case mesh::service_node_state_t::serving:
            return 1;
        case mesh::service_node_state_t::draining:
            return 2;
        case mesh::service_node_state_t::stopped:
            return 3;
        case mesh::service_node_state_t::error:
            return 4;
        default:
            throw service_wire_error_t (
              "retiring is a host state and cannot be encoded as a service descriptor");
    }
}

mesh::service_node_state_t runtime_state_from_wire (std::uint8_t value)
{
    switch (value) {
        case 0:
            return mesh::service_node_state_t::preparing;
        case 1:
            return mesh::service_node_state_t::serving;
        case 2:
            return mesh::service_node_state_t::draining;
        case 3:
            return mesh::service_node_state_t::stopped;
        case 4:
            return mesh::service_node_state_t::error;
        default:
            throw service_wire_error_t ("invalid runtime state");
    }
}

std::uint8_t object_role_wire (mesh::service_object_role_t role)
{
    return static_cast<std::uint8_t> (role);
}

mesh::service_object_role_t object_role_from_wire (std::uint8_t value)
{
    if (value > 2) {
        throw service_wire_error_t ("invalid object role");
    }
    return static_cast<mesh::service_object_role_t> (value);
}

void validate_admission_kind (command kind)
{
    if (kind != command::hello && kind != command::admit
        && kind != command::update) {
        throw service_wire_error_t (
          "command is not a RouteMesh admission record");
    }
}

void validate_kind (command kind)
{
    if (kind != command::livenessProbe && kind != command::livenessAck) {
        throw service_wire_error_t ("command is not a liveness record");
    }
}

} // namespace

std::vector<std::uint8_t> encode_node_send_header ()
{
    return {magic[0], magic[1], wire_major,
            static_cast<std::uint8_t> (command::nodeSend), 0};
}

std::vector<std::uint8_t>
encode_node_request_header (std::uint64_t correlation)
{
    if (correlation == 0) {
        throw service_wire_error_t ("request correlation must be nonzero");
    }
    std::vector<std::uint8_t> result{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::nodeRequest), 0};
    append_u64 (result, correlation);
    return result;
}

std::uint64_t
decode_node_request_header (std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::nodeRequest || header.flags != 0
        || bytes.size () != prefix_size + 8) {
        throw service_wire_error_t ("invalid nodeRequest header");
    }
    std::size_t offset = prefix_size;
    const auto correlation = read_u64 (bytes, offset);
    if (correlation == 0) {
        throw service_wire_error_t ("request correlation must be nonzero");
    }
    return correlation;
}

std::vector<std::uint8_t> encode_channel_request_header (
  std::uint64_t correlation,
  const std::string &channel_name)
{
    if (correlation == 0) {
        throw service_wire_error_t ("request correlation must be nonzero");
    }
    std::vector<std::uint8_t> result{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::channelRequest), 0};
    append_u64 (result, correlation);
    append_text8 (result, channel_name, "channel name");
    return result;
}

channel_request_header_t
decode_channel_request_header (std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::channelRequest || header.flags != 0) {
        throw service_wire_error_t ("invalid channelRequest header");
    }
    std::size_t offset = prefix_size;
    const auto correlation = read_u64 (bytes, offset);
    if (correlation == 0) {
        throw service_wire_error_t ("request correlation must be nonzero");
    }
    auto channel_name = read_text8 (bytes, offset, "channel name");
    if (offset != bytes.size ()) {
        throw service_wire_error_t ("channelRequest header has trailing bytes");
    }
    return {correlation, std::move (channel_name)};
}

std::vector<std::uint8_t>
encode_channel_send_header (const std::string &channel_name)
{
    std::vector<std::uint8_t> result{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::channelSend), 0};
    append_text8 (result, channel_name, "channel name");
    return result;
}

std::vector<std::uint8_t> encode_spot_message_header (
  command kind,
  const std::string &source_spot_id,
  const spot_route_fence_t &target,
  wire_operation_id_t operation,
  std::optional<std::uint64_t> correlation,
  std::uint8_t message_follow_hop_count)
{
    if (kind != command::spotSend && kind != command::spotRequest) {
        throw service_wire_error_t ("command is not a Spot message");
    }
    if ((operation.high == 0 && operation.low == 0)
        || (kind == command::spotRequest) != correlation.has_value ()
        || (correlation && *correlation == 0)
        || message_follow_hop_count > 8
        || target.object_generation == 0
        || target.target_node_generation == 0
        || target.authority_owner_generation == 0
        || target.owner_lease_generation == 0) {
        throw service_wire_error_t ("invalid Spot route fence");
    }
    std::vector<std::uint8_t> result{
      magic[0], magic[1], wire_major, static_cast<std::uint8_t> (kind), 0};
    if (correlation)
        append_u64 (result, *correlation);
    append_u64 (result, operation.high);
    append_u64 (result, operation.low);
    result.push_back (message_follow_hop_count);
    append_text8 (result, source_spot_id, "source SpotId");
    append_text8 (result, target.spot_id, "target SpotId");
    append_u64 (result, target.object_generation);
    append_bytes8 (
      result, target.target_node_routing_id, "target node RID");
    append_u64 (result, target.target_node_generation);
    append_u64 (result, target.authority_owner_generation);
    append_u64 (result, target.owner_lease_generation);
    return result;
}

spot_message_header_t decode_spot_message_header (
  std::span<const std::uint8_t> bytes,
  command expected_kind)
{
    const auto header = decode_header (bytes);
    if ((expected_kind != command::spotSend
         && expected_kind != command::spotRequest)
        || header.kind != expected_kind || header.flags != 0) {
        throw service_wire_error_t ("invalid Spot message header");
    }
    std::size_t offset = prefix_size;
    std::optional<std::uint64_t> correlation;
    if (expected_kind == command::spotRequest) {
        correlation = read_u64 (bytes, offset);
        if (*correlation == 0) {
            throw service_wire_error_t (
              "Spot request correlation must be nonzero");
        }
    }
    const wire_operation_id_t operation{
      read_u64 (bytes, offset), read_u64 (bytes, offset)};
    if (offset >= bytes.size ())
        throw service_wire_error_t ("missing Spot Message Follow hop count");
    const auto message_follow_hop_count = bytes[offset++];
    spot_message_header_t result;
    result.operation = operation;
    result.message_follow_hop_count = message_follow_hop_count;
    result.correlation = correlation;
    result.source_spot_id =
      read_text8 (bytes, offset, "source SpotId");
    result.target.spot_id =
      read_text8 (bytes, offset, "target SpotId");
    result.target.object_generation = read_u64 (bytes, offset);
    result.target.target_node_routing_id =
      read_bytes8 (bytes, offset, "target node RID");
    result.target.target_node_generation = read_u64 (bytes, offset);
    result.target.authority_owner_generation = read_u64 (bytes, offset);
    result.target.owner_lease_generation = read_u64 (bytes, offset);
    if ((result.operation.high == 0 && result.operation.low == 0)
        || result.message_follow_hop_count > 8
        || result.target.object_generation == 0
        || result.target.target_node_generation == 0
        || result.target.authority_owner_generation == 0
        || result.target.owner_lease_generation == 0
        || offset != bytes.size ()) {
        throw service_wire_error_t (
          "invalid or trailing Spot route fence");
    }
    return result;
}

std::vector<std::uint8_t> encode_actor_message_header (
  command kind,
  const std::optional<std::pair<std::string, std::uint64_t>> &source_actor,
  const actor_route_fence_t &target,
  wire_operation_id_t operation,
  std::optional<std::uint64_t> correlation,
  std::uint8_t message_follow_hop_count)
{
    if (kind != command::actorSend && kind != command::actorRequest) {
        throw service_wire_error_t ("command is not an Actor message");
    }
    if ((operation.high == 0 && operation.low == 0)
        || (kind == command::actorRequest) != correlation.has_value ()
        || (correlation && *correlation == 0)
        || message_follow_hop_count > 8
        || target.object_generation == 0
        || target.target_node_generation == 0
        || target.authority_owner_generation == 0
        || target.owner_lease_generation == 0) {
        throw service_wire_error_t ("invalid Actor route fence");
    }
    std::vector<std::uint8_t> result{
      magic[0], magic[1], wire_major, static_cast<std::uint8_t> (kind), 0};
    if (correlation)
        append_u64 (result, *correlation);
    append_u64 (result, operation.high);
    append_u64 (result, operation.low);
    result.push_back (message_follow_hop_count);
    if (source_actor) {
        append_text8 (result, source_actor->first, "source Actor ID");
        if (source_actor->second == 0) {
            throw service_wire_error_t (
              "source Actor generation must be nonzero");
        }
        append_u64 (result, source_actor->second);
    } else {
        result.push_back (0);
    }
    append_text8 (result, target.actor_id, "target Actor ID");
    append_u64 (result, target.object_generation);
    append_bytes8 (
      result, target.target_node_routing_id, "target node RID");
    append_u64 (result, target.target_node_generation);
    append_u64 (result, target.authority_owner_generation);
    append_u64 (result, target.owner_lease_generation);
    return result;
}

actor_message_header_t decode_actor_message_header (
  std::span<const std::uint8_t> bytes,
  command expected_kind)
{
    const auto header = decode_header (bytes);
    if ((expected_kind != command::actorSend
         && expected_kind != command::actorRequest)
        || header.kind != expected_kind || header.flags != 0) {
        throw service_wire_error_t ("invalid Actor message header");
    }
    std::size_t offset = prefix_size;
    std::optional<std::uint64_t> correlation;
    if (expected_kind == command::actorRequest) {
        correlation = read_u64 (bytes, offset);
        if (*correlation == 0) {
            throw service_wire_error_t (
              "Actor request correlation must be nonzero");
        }
    }
    const wire_operation_id_t operation{
      read_u64 (bytes, offset), read_u64 (bytes, offset)};
    if (offset >= bytes.size ())
        throw service_wire_error_t ("missing Actor Message Follow hop count");
    const auto message_follow_hop_count = bytes[offset++];
    actor_message_header_t result;
    result.operation = operation;
    result.message_follow_hop_count = message_follow_hop_count;
    result.correlation = correlation;
    if (offset >= bytes.size ()) {
        throw service_wire_error_t ("truncated source Actor");
    }
    if (bytes[offset] == 0) {
        ++offset;
    } else {
        auto actor_id = read_text8 (bytes, offset, "source Actor ID");
        const auto generation = read_u64 (bytes, offset);
        if (generation == 0) {
            throw service_wire_error_t (
              "source Actor generation must be nonzero");
        }
        result.source_actor =
          std::pair{std::move (actor_id), generation};
    }
    result.target.actor_id =
      read_text8 (bytes, offset, "target Actor ID");
    result.target.object_generation = read_u64 (bytes, offset);
    result.target.target_node_routing_id =
      read_bytes8 (bytes, offset, "target node RID");
    result.target.target_node_generation = read_u64 (bytes, offset);
    result.target.authority_owner_generation = read_u64 (bytes, offset);
    result.target.owner_lease_generation = read_u64 (bytes, offset);
    if ((result.operation.high == 0 && result.operation.low == 0)
        || result.message_follow_hop_count > 8
        || result.target.object_generation == 0
        || result.target.target_node_generation == 0
        || result.target.authority_owner_generation == 0
        || result.target.owner_lease_generation == 0
        || offset != bytes.size ()) {
        throw service_wire_error_t (
          "invalid or trailing Actor route fence");
    }
    return result;
}

namespace
{

std::vector<std::uint8_t> encode_message_follow_route_body (
  const message_follow_route_t &route)
{
    std::vector<std::uint8_t> body;
    std::visit (
      [&body] (const auto &value) {
          using route_type = std::decay_t<decltype (value)>;
          if constexpr (std::is_same_v<route_type, actor_route_fence_t>) {
              append_text8 (body, value.actor_id, "Message Follow Actor ID");
              append_nonzero_u64 (
                body, value.object_generation,
                "Message Follow Actor generation");
              append_bytes8 (
                body, value.target_node_routing_id,
                "Message Follow Actor node RID");
              append_nonzero_u64 (
                body, value.target_node_generation,
                "Message Follow Actor node generation");
              append_nonzero_u64 (
                body, value.authority_owner_generation,
                "Message Follow Actor authority generation");
              append_nonzero_u64 (
                body, value.owner_lease_generation,
                "Message Follow Actor owner lease generation");
          } else {
              append_text8 (body, value.spot_id, "Message Follow Spot ID");
              append_nonzero_u64 (
                body, value.object_generation,
                "Message Follow Spot generation");
              append_bytes8 (
                body, value.target_node_routing_id,
                "Message Follow Spot node RID");
              append_nonzero_u64 (
                body, value.target_node_generation,
                "Message Follow Spot node generation");
              append_nonzero_u64 (
                body, value.authority_owner_generation,
                "Message Follow Spot authority generation");
              append_nonzero_u64 (
                body, value.owner_lease_generation,
                "Message Follow Spot owner lease generation");
          }
      },
      route);
    if (body.size () > std::numeric_limits<std::uint16_t>::max ()) {
        throw service_wire_error_t (
          "Message Follow route body exceeds u16 length");
    }
    return body;
}

message_follow_route_t decode_message_follow_route (
  std::span<const std::uint8_t> bytes,
  std::size_t &offset)
{
    if (offset >= bytes.size ())
        throw service_wire_error_t (
          "Message Follow route kind is truncated");
    const auto kind = bytes[offset++];
    const auto length = read_u16 (bytes, offset);
    if (bytes.size () - offset < length)
        throw service_wire_error_t (
          "Message Follow route body is truncated");
    const auto body = bytes.subspan (offset, length);
    offset += length;
    std::size_t body_offset = 0;
    if (kind == 1) {
        actor_route_fence_t route;
        route.actor_id = read_text8 (
          body, body_offset, "Message Follow Actor ID");
        route.object_generation = read_nonzero_u64 (
          body, body_offset, "Message Follow Actor generation");
        route.target_node_routing_id = read_bytes8 (
          body, body_offset, "Message Follow Actor node RID");
        route.target_node_generation = read_nonzero_u64 (
          body, body_offset, "Message Follow Actor node generation");
        route.authority_owner_generation = read_nonzero_u64 (
          body, body_offset, "Message Follow Actor authority generation");
        route.owner_lease_generation = read_nonzero_u64 (
          body, body_offset, "Message Follow Actor owner lease generation");
        if (body_offset != body.size ())
            throw service_wire_error_t (
              "Message Follow Actor route has trailing bytes");
        return route;
    }
    if (kind == 2) {
        spot_route_fence_t route;
        route.spot_id = read_text8 (
          body, body_offset, "Message Follow Spot ID");
        route.object_generation = read_nonzero_u64 (
          body, body_offset, "Message Follow Spot generation");
        route.target_node_routing_id = read_bytes8 (
          body, body_offset, "Message Follow Spot node RID");
        route.target_node_generation = read_nonzero_u64 (
          body, body_offset, "Message Follow Spot node generation");
        route.authority_owner_generation = read_nonzero_u64 (
          body, body_offset, "Message Follow Spot authority generation");
        route.owner_lease_generation = read_nonzero_u64 (
          body, body_offset, "Message Follow Spot owner lease generation");
        if (body_offset != body.size ())
            throw service_wire_error_t (
              "Message Follow Spot route has trailing bytes");
        return route;
    }
    throw service_wire_error_t ("unknown Message Follow route kind");
}

} // namespace

std::vector<std::uint8_t>
encode_message_follow (const message_follow_notice_t &notice)
{
    if (notice.hop_count == 0
        || notice.hop_count > messageFollowHopCount
        || notice.queued_messages > messageFollowMessages
        || notice.queued_bytes > messageFollowBytes
        || (notice.original_operation.high == 0
            && notice.original_operation.low == 0)
        || notice.source.index () != notice.target.index ()) {
        throw service_wire_error_t (
          "Message Follow notice contains an invalid fence or bound");
    }
    const auto source = encode_message_follow_route_body (notice.source);
    const auto target = encode_message_follow_route_body (notice.target);
    std::vector<std::uint8_t> body;
    body.push_back (notice.source.index () == 0 ? 1 : 2);
    append_u16 (body, static_cast<std::uint16_t> (source.size ()));
    body.insert (body.end (), source.begin (), source.end ());
    body.push_back (notice.target.index () == 0 ? 1 : 2);
    append_u16 (body, static_cast<std::uint16_t> (target.size ()));
    body.insert (body.end (), target.begin (), target.end ());
    body.push_back (notice.hop_count);
    append_u32 (body, notice.queued_messages);
    append_u32 (body, notice.queued_bytes);
    append_u64 (body, notice.original_operation.high);
    append_u64 (body, notice.original_operation.low);
    append_u64 (body, notice.original_reply_route_id);
    if (body.size () > messageFollowBytes) {
        throw service_wire_error_t (
          "Message Follow notice exceeds its encoded byte bound");
    }
    std::vector<std::uint8_t> result{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::messageFollow), 0, 1};
    append_u32 (result, static_cast<std::uint32_t> (body.size ()));
    result.insert (result.end (), body.begin (), body.end ());
    return result;
}

message_follow_notice_t
decode_message_follow (std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::messageFollow || header.flags != 0)
        throw service_wire_error_t (
          "record is not a Message Follow notice");
    std::size_t offset = prefix_size;
    if (offset >= bytes.size () || bytes[offset++] != 1)
        throw service_wire_error_t (
          "Message Follow notice version must be one");
    const auto body_length = read_u32 (bytes, offset);
    if (body_length > messageFollowBytes
        || bytes.size () - offset != body_length)
        throw service_wire_error_t (
          "Message Follow notice has an invalid body length");
    const auto body = bytes.subspan (offset, body_length);
    std::size_t body_offset = 0;
    message_follow_notice_t notice;
    notice.source = decode_message_follow_route (body, body_offset);
    notice.target = decode_message_follow_route (body, body_offset);
    if (body_offset >= body.size ())
        throw service_wire_error_t (
          "Message Follow hop count is truncated");
    notice.hop_count = body[body_offset++];
    notice.queued_messages = read_u32 (body, body_offset);
    notice.queued_bytes = read_u32 (body, body_offset);
    notice.original_operation.high = read_u64 (body, body_offset);
    notice.original_operation.low = read_u64 (body, body_offset);
    notice.original_reply_route_id = read_u64 (body, body_offset);
    if (body_offset != body.size ())
        throw service_wire_error_t (
          "Message Follow notice has trailing bytes");
    (void) encode_message_follow (notice);
    return notice;
}

std::vector<std::uint8_t> encode_session_relocation_route (
  const session_relocation_route_t &record)
{
    if ((record.relocation.high == 0 && record.relocation.low == 0)
        || record.coordinator.lease_generation == 0
        || record.coordinator.node_generation == 0
        || (record.sender_role != relocation_role_t::target
            && record.sender_role != relocation_role_t::source
            && record.sender_role != relocation_role_t::coordinator)
        || record.actor.object_generation == 0
        || record.session_owner_node_generation == 0
        || record.session_owner_lease_generation == 0
        || record.binding_generation == 0) {
        throw service_wire_error_t (
          "Session relocation route contains a zero or invalid exact fence");
    }
    std::vector<std::uint8_t> bytes{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::sessionRelocationRoute), 0};
    append_u64 (bytes, record.relocation.high);
    append_u64 (bytes, record.relocation.low);
    append_text8 (bytes, record.coordinator.owner_id,
                  "coordinator owner ID");
    append_u64 (bytes, record.coordinator.lease_generation);
    append_bytes8 (bytes, record.coordinator.node_routing_id,
                   "coordinator node RID");
    append_u64 (bytes, record.coordinator.node_generation);
    append_text16 (bytes,
                   record.coordinator.expected_authority_store_version,
                   "expected authority StoreVersion");
    bytes.push_back (static_cast<std::uint8_t> (record.sender_role));
    append_text8 (bytes, record.actor.actor_id, "Actor ID");
    append_u64 (bytes, record.actor.object_generation);
    append_bytes8 (bytes, record.session_owner_node_routing_id,
                   "session owner node RID");
    append_u64 (bytes, record.session_owner_node_generation);
    append_text8 (bytes, record.session_owner_id, "session owner ID");
    append_u64 (bytes, record.session_owner_lease_generation);
    append_bytes8 (bytes, record.session_routing_id, "Session RID");
    append_u64 (bytes, record.binding_generation);

    std::vector<std::uint8_t> route;
    if (record.route.action == session_relocation_route_action_t::commit) {
        if ((record.sender_role != relocation_role_t::target
             && record.sender_role != relocation_role_t::coordinator)
            || record.route.previous_authority_owner_generation == 0
            || record.route.target_authority_owner_generation
                 <= record.route.previous_authority_owner_generation
            || record.route.target_node_generation == 0
            || record.route.current_authority_owner_generation != 0) {
            throw service_wire_error_t (
              "Session relocation commit route has an invalid authority fence");
        }
        append_u64 (route,
                    record.route.previous_authority_owner_generation);
        append_u64 (route,
                    record.route.target_authority_owner_generation);
        append_bytes8 (route, record.route.target_node_routing_id,
                       "target node RID");
        append_u64 (route, record.route.target_node_generation);
        append_u64 (route, record.route.replayed_high_water);
    }
    else if (record.route.action
             == session_relocation_route_action_t::abort) {
        if ((record.sender_role != relocation_role_t::source
             && record.sender_role != relocation_role_t::coordinator)
            || record.route.current_authority_owner_generation == 0
            || record.route.previous_authority_owner_generation != 0
            || record.route.target_authority_owner_generation != 0
            || !record.route.target_node_routing_id.empty ()
            || record.route.target_node_generation != 0
            || record.route.replayed_high_water != 0) {
            throw service_wire_error_t (
              "Session relocation abort route has an invalid authority fence");
        }
        append_u64 (route,
                    record.route.current_authority_owner_generation);
    }
    else {
        throw service_wire_error_t (
          "Session relocation route action is invalid");
    }
    bytes.push_back (static_cast<std::uint8_t> (record.route.action));
    append_u16 (bytes, static_cast<std::uint16_t> (route.size ()));
    bytes.insert (bytes.end (), route.begin (), route.end ());
    return bytes;
}

std::vector<std::uint8_t> encode_session_relocation_seal (
  const session_relocation_seal_t &record)
{
    if ((record.relocation.high == 0 && record.relocation.low == 0)
        || record.coordinator.lease_generation == 0
        || record.coordinator.node_generation == 0
        || (record.sender_role != relocation_role_t::source
            && record.sender_role != relocation_role_t::coordinator)
        || record.actor.object_generation == 0
        || record.actor.target_node_generation == 0
        || record.actor.authority_owner_generation == 0
        || record.actor.owner_lease_generation == 0
        || record.session_owner_node_generation == 0
        || record.session_owner_lease_generation == 0
        || record.binding_generation == 0) {
        throw service_wire_error_t (
          "Session relocation seal contains a zero or invalid exact fence");
    }
    std::vector<std::uint8_t> bytes{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::sessionRelocationSeal), 0};
    append_u64 (bytes, record.relocation.high);
    append_u64 (bytes, record.relocation.low);
    append_text8 (bytes, record.coordinator.owner_id,
                  "coordinator owner ID");
    append_u64 (bytes, record.coordinator.lease_generation);
    append_bytes8 (bytes, record.coordinator.node_routing_id,
                   "coordinator node RID");
    append_u64 (bytes, record.coordinator.node_generation);
    append_text16 (bytes,
                   record.coordinator.expected_authority_store_version,
                   "expected authority StoreVersion");
    bytes.push_back (static_cast<std::uint8_t> (record.sender_role));
    append_text8 (bytes, record.actor.actor_id, "Actor ID");
    append_u64 (bytes, record.actor.object_generation);
    append_bytes8 (bytes, record.actor.target_node_routing_id,
                   "Actor owner node RID");
    append_u64 (bytes, record.actor.target_node_generation);
    append_u64 (bytes, record.actor.authority_owner_generation);
    append_u64 (bytes, record.actor.owner_lease_generation);
    append_bytes8 (bytes, record.session_owner_node_routing_id,
                   "session owner node RID");
    append_u64 (bytes, record.session_owner_node_generation);
    append_text8 (bytes, record.session_owner_id, "session owner ID");
    append_u64 (bytes, record.session_owner_lease_generation);
    append_bytes8 (bytes, record.session_routing_id, "Session RID");
    append_u64 (bytes, record.binding_generation);
    return bytes;
}

session_relocation_seal_t decode_session_relocation_seal (
  std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::sessionRelocationSeal
        || header.flags != 0)
        throw service_wire_error_t (
          "record is not a Session relocation seal command");
    std::size_t offset = prefix_size;
    session_relocation_seal_t record;
    record.relocation.high = read_u64 (bytes, offset);
    record.relocation.low = read_u64 (bytes, offset);
    record.coordinator.owner_id =
      read_text8 (bytes, offset, "coordinator owner ID");
    record.coordinator.lease_generation = read_u64 (bytes, offset);
    record.coordinator.node_routing_id =
      read_bytes8 (bytes, offset, "coordinator node RID");
    record.coordinator.node_generation = read_u64 (bytes, offset);
    record.coordinator.expected_authority_store_version =
      read_text16 (bytes, offset, "expected authority StoreVersion");
    if (offset >= bytes.size ())
        throw service_wire_error_t (
          "Session relocation seal sender role is truncated");
    record.sender_role = static_cast<relocation_role_t> (bytes[offset++]);
    record.actor.actor_id = read_text8 (bytes, offset, "Actor ID");
    record.actor.object_generation = read_u64 (bytes, offset);
    record.actor.target_node_routing_id =
      read_bytes8 (bytes, offset, "Actor owner node RID");
    record.actor.target_node_generation = read_u64 (bytes, offset);
    record.actor.authority_owner_generation = read_u64 (bytes, offset);
    record.actor.owner_lease_generation = read_u64 (bytes, offset);
    record.session_owner_node_routing_id =
      read_bytes8 (bytes, offset, "session owner node RID");
    record.session_owner_node_generation = read_u64 (bytes, offset);
    record.session_owner_id =
      read_text8 (bytes, offset, "session owner ID");
    record.session_owner_lease_generation = read_u64 (bytes, offset);
    record.session_routing_id =
      read_bytes8 (bytes, offset, "Session RID");
    record.binding_generation = read_u64 (bytes, offset);
    if (offset != bytes.size ())
        throw service_wire_error_t (
          "Session relocation seal has trailing bytes");
    (void) encode_session_relocation_seal (record);
    return record;
}

std::vector<std::uint8_t> encode_session_relocation_sealed (
  const session_relocation_sealed_t &record)
{
    if ((record.relocation.high == 0 && record.relocation.low == 0)
        || record.coordinator.lease_generation == 0
        || record.coordinator.node_generation == 0
        || record.actor.object_generation == 0
        || record.actor.target_node_generation == 0
        || record.actor.authority_owner_generation == 0
        || record.actor.owner_lease_generation == 0
        || record.session_owner_node_generation == 0
        || record.session_owner_lease_generation == 0
        || record.binding_generation == 0) {
        throw service_wire_error_t (
          "Session relocation sealed ACK contains an invalid exact fence");
    }
    std::vector<std::uint8_t> bytes{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::sessionRelocationSealed), 0};
    append_u64 (bytes, record.relocation.high);
    append_u64 (bytes, record.relocation.low);
    append_text8 (bytes, record.coordinator.owner_id,
                  "coordinator owner ID");
    append_u64 (bytes, record.coordinator.lease_generation);
    append_bytes8 (bytes, record.coordinator.node_routing_id,
                   "coordinator node RID");
    append_u64 (bytes, record.coordinator.node_generation);
    append_text16 (bytes,
                   record.coordinator.expected_authority_store_version,
                   "expected authority StoreVersion");
    append_text8 (bytes, record.actor.actor_id, "Actor ID");
    append_u64 (bytes, record.actor.object_generation);
    append_bytes8 (bytes, record.actor.target_node_routing_id,
                   "Actor owner node RID");
    append_u64 (bytes, record.actor.target_node_generation);
    append_u64 (bytes, record.actor.authority_owner_generation);
    append_u64 (bytes, record.actor.owner_lease_generation);
    append_bytes8 (bytes, record.session_owner_node_routing_id,
                   "session owner node RID");
    append_u64 (bytes, record.session_owner_node_generation);
    append_text8 (bytes, record.session_owner_id, "session owner ID");
    append_u64 (bytes, record.session_owner_lease_generation);
    append_bytes8 (bytes, record.session_routing_id, "Session RID");
    append_u64 (bytes, record.binding_generation);
    append_u64 (bytes, record.last_accepted_session_sequence);
    return bytes;
}

session_relocation_sealed_t decode_session_relocation_sealed (
  std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::sessionRelocationSealed
        || header.flags != 0)
        throw service_wire_error_t (
          "record is not a Session relocation sealed ACK");
    std::size_t offset = prefix_size;
    session_relocation_sealed_t record;
    record.relocation.high = read_u64 (bytes, offset);
    record.relocation.low = read_u64 (bytes, offset);
    record.coordinator.owner_id =
      read_text8 (bytes, offset, "coordinator owner ID");
    record.coordinator.lease_generation = read_u64 (bytes, offset);
    record.coordinator.node_routing_id =
      read_bytes8 (bytes, offset, "coordinator node RID");
    record.coordinator.node_generation = read_u64 (bytes, offset);
    record.coordinator.expected_authority_store_version =
      read_text16 (bytes, offset, "expected authority StoreVersion");
    record.actor.actor_id = read_text8 (bytes, offset, "Actor ID");
    record.actor.object_generation = read_u64 (bytes, offset);
    record.actor.target_node_routing_id =
      read_bytes8 (bytes, offset, "Actor owner node RID");
    record.actor.target_node_generation = read_u64 (bytes, offset);
    record.actor.authority_owner_generation = read_u64 (bytes, offset);
    record.actor.owner_lease_generation = read_u64 (bytes, offset);
    record.session_owner_node_routing_id =
      read_bytes8 (bytes, offset, "session owner node RID");
    record.session_owner_node_generation = read_u64 (bytes, offset);
    record.session_owner_id =
      read_text8 (bytes, offset, "session owner ID");
    record.session_owner_lease_generation = read_u64 (bytes, offset);
    record.session_routing_id =
      read_bytes8 (bytes, offset, "Session RID");
    record.binding_generation = read_u64 (bytes, offset);
    record.last_accepted_session_sequence = read_u64 (bytes, offset);
    if (offset != bytes.size ())
        throw service_wire_error_t (
          "Session relocation sealed ACK has trailing bytes");
    (void) encode_session_relocation_sealed (record);
    return record;
}

session_relocation_route_t decode_session_relocation_route (
  std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::sessionRelocationRoute
        || header.flags != 0) {
        throw service_wire_error_t (
          "record is not a Session relocation route command");
    }
    std::size_t offset = prefix_size;
    session_relocation_route_t record;
    record.relocation.high = read_u64 (bytes, offset);
    record.relocation.low = read_u64 (bytes, offset);
    record.coordinator.owner_id =
      read_text8 (bytes, offset, "coordinator owner ID");
    record.coordinator.lease_generation = read_u64 (bytes, offset);
    record.coordinator.node_routing_id =
      read_bytes8 (bytes, offset, "coordinator node RID");
    record.coordinator.node_generation = read_u64 (bytes, offset);
    record.coordinator.expected_authority_store_version =
      read_text16 (bytes, offset, "expected authority StoreVersion");
    if (offset >= bytes.size ())
        throw service_wire_error_t (
          "Session relocation sender role is truncated");
    record.sender_role = static_cast<relocation_role_t> (bytes[offset++]);
    record.actor.actor_id = read_text8 (bytes, offset, "Actor ID");
    record.actor.object_generation = read_u64 (bytes, offset);
    record.session_owner_node_routing_id =
      read_bytes8 (bytes, offset, "session owner node RID");
    record.session_owner_node_generation = read_u64 (bytes, offset);
    record.session_owner_id =
      read_text8 (bytes, offset, "session owner ID");
    record.session_owner_lease_generation = read_u64 (bytes, offset);
    record.session_routing_id =
      read_bytes8 (bytes, offset, "Session RID");
    record.binding_generation = read_u64 (bytes, offset);
    if (offset >= bytes.size ())
        throw service_wire_error_t (
          "Session relocation route action is truncated");
    record.route.action =
      static_cast<session_relocation_route_action_t> (bytes[offset++]);
    const auto route_length = read_u16 (bytes, offset);
    if (bytes.size () - offset != route_length)
        throw service_wire_error_t (
          "Session relocation route length does not match");
    const auto route = bytes.subspan (offset, route_length);
    std::size_t route_offset = 0;
    if (record.route.action
        == session_relocation_route_action_t::commit) {
        record.route.previous_authority_owner_generation =
          read_u64 (route, route_offset);
        record.route.target_authority_owner_generation =
          read_u64 (route, route_offset);
        record.route.target_node_routing_id =
          read_bytes8 (route, route_offset, "target node RID");
        record.route.target_node_generation =
          read_u64 (route, route_offset);
        record.route.replayed_high_water =
          read_u64 (route, route_offset);
    }
    else if (record.route.action
             == session_relocation_route_action_t::abort) {
        record.route.current_authority_owner_generation =
          read_u64 (route, route_offset);
    }
    else {
        throw service_wire_error_t (
          "Session relocation route action is invalid");
    }
    if (route_offset != route.size ())
        throw service_wire_error_t (
          "Session relocation route has trailing bytes");
    (void) encode_session_relocation_route (record);
    return record;
}

std::vector<std::uint8_t> encode_session_relocation_routed (
  const session_relocation_routed_t &record)
{
    if ((record.relocation.high == 0 && record.relocation.low == 0)
        || record.coordinator.lease_generation == 0
        || record.coordinator.node_generation == 0
        || record.actor.object_generation == 0
        || record.session_owner_node_generation == 0
        || record.session_owner_lease_generation == 0
        || record.binding_generation == 0
        || record.current_authority_owner_generation == 0
        || (record.action != session_relocation_route_action_t::commit
            && record.action
                 != session_relocation_route_action_t::abort)) {
        throw service_wire_error_t (
          "Session relocation routed ACK contains an invalid exact fence");
    }
    std::vector<std::uint8_t> bytes{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::sessionRelocationRouted), 0};
    append_u64 (bytes, record.relocation.high);
    append_u64 (bytes, record.relocation.low);
    append_text8 (bytes, record.coordinator.owner_id,
                  "coordinator owner ID");
    append_u64 (bytes, record.coordinator.lease_generation);
    append_bytes8 (bytes, record.coordinator.node_routing_id,
                   "coordinator node RID");
    append_u64 (bytes, record.coordinator.node_generation);
    append_text16 (bytes,
                   record.coordinator.expected_authority_store_version,
                   "expected authority StoreVersion");
    append_text8 (bytes, record.actor.actor_id, "Actor ID");
    append_u64 (bytes, record.actor.object_generation);
    append_bytes8 (bytes, record.session_owner_node_routing_id,
                   "session owner node RID");
    append_u64 (bytes, record.session_owner_node_generation);
    append_text8 (bytes, record.session_owner_id, "session owner ID");
    append_u64 (bytes, record.session_owner_lease_generation);
    append_bytes8 (bytes, record.session_routing_id, "Session RID");
    append_u64 (bytes, record.binding_generation);
    bytes.push_back (static_cast<std::uint8_t> (record.action));
    append_u64 (bytes, record.current_authority_owner_generation);
    append_u64 (bytes, record.last_accepted_session_sequence);
    return bytes;
}

session_relocation_routed_t decode_session_relocation_routed (
  std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::sessionRelocationRouted
        || header.flags != 0) {
        throw service_wire_error_t (
          "record is not a Session relocation routed ACK");
    }
    std::size_t offset = prefix_size;
    session_relocation_routed_t record;
    record.relocation.high = read_u64 (bytes, offset);
    record.relocation.low = read_u64 (bytes, offset);
    record.coordinator.owner_id =
      read_text8 (bytes, offset, "coordinator owner ID");
    record.coordinator.lease_generation = read_u64 (bytes, offset);
    record.coordinator.node_routing_id =
      read_bytes8 (bytes, offset, "coordinator node RID");
    record.coordinator.node_generation = read_u64 (bytes, offset);
    record.coordinator.expected_authority_store_version =
      read_text16 (bytes, offset, "expected authority StoreVersion");
    record.actor.actor_id = read_text8 (bytes, offset, "Actor ID");
    record.actor.object_generation = read_u64 (bytes, offset);
    record.session_owner_node_routing_id =
      read_bytes8 (bytes, offset, "session owner node RID");
    record.session_owner_node_generation = read_u64 (bytes, offset);
    record.session_owner_id =
      read_text8 (bytes, offset, "session owner ID");
    record.session_owner_lease_generation = read_u64 (bytes, offset);
    record.session_routing_id =
      read_bytes8 (bytes, offset, "Session RID");
    record.binding_generation = read_u64 (bytes, offset);
    if (offset >= bytes.size ())
        throw service_wire_error_t (
          "Session relocation routed action is truncated");
    record.action =
      static_cast<session_relocation_route_action_t> (bytes[offset++]);
    record.current_authority_owner_generation = read_u64 (bytes, offset);
    record.last_accepted_session_sequence = read_u64 (bytes, offset);
    if (offset != bytes.size ())
        throw service_wire_error_t (
          "Session relocation routed ACK has trailing bytes");
    (void) encode_session_relocation_routed (record);
    return record;
}

namespace
{
bool valid_terminal_failure (std::uint32_t terminal,
                             framework_error_code failure) noexcept
{
    if (terminal == 0)
        return failure == framework_error_code::none;
    if (terminal == 101 || terminal == 103
        || (terminal >= 108 && terminal <= 113))
        return failure == framework_error_code::none;
    if (failure == framework_error_code::none)
        return false;
    switch (failure) {
        case framework_error_code::actorRouteNotFound:
        case framework_error_code::spotRouteNotFound:
        case framework_error_code::actorSessionNotBound:
        case framework_error_code::handlerNotFound:
        case framework_error_code::routeHandlerNotFound:
        case framework_error_code::actorDispatchHandlerNotFound:
        case framework_error_code::requestTargetNotFound:
            return terminal == 102;
        case framework_error_code::payloadDecodeFailed:
        case framework_error_code::requestProtocolError:
            return terminal == 104;
        case framework_error_code::actorCreateFailed:
        case framework_error_code::spotCreateFailed:
        case framework_error_code::routeNotConnected:
        case framework_error_code::requestFailed:
        case framework_error_code::workerTimedOut:
        case framework_error_code::workerFailed:
        case framework_error_code::relocationDataLost:
            return terminal == 105;
        case framework_error_code::requestRejected:
        case framework_error_code::workerQueueFull:
        case framework_error_code::actorCreateRejected:
            return terminal == 106;
        case framework_error_code::actorAlreadyExists:
        case framework_error_code::actorTypeMismatch:
        case framework_error_code::spotTypeMismatch:
        case framework_error_code::actorLocationStale:
        case framework_error_code::spotGenerationStale:
        case framework_error_code::spotMoving:
            return terminal == 107;
        default:
            return false;
    }
}

void append_coordinator (std::vector<std::uint8_t> &bytes,
                         const relocation_coordinator_fence_t &coordinator)
{
    if (coordinator.lease_generation == 0
        || coordinator.node_generation == 0) {
        throw service_wire_error_t (
          "relocation coordinator contains a zero required fence");
    }
    append_text8 (bytes, coordinator.owner_id, "coordinator owner ID");
    append_u64 (bytes, coordinator.lease_generation);
    append_bytes8 (bytes, coordinator.node_routing_id,
                   "coordinator node RID");
    append_u64 (bytes, coordinator.node_generation);
    append_text16 (bytes, coordinator.expected_authority_store_version,
                   "expected authority StoreVersion");
}

relocation_coordinator_fence_t read_coordinator (
  std::span<const std::uint8_t> bytes,
  std::size_t &offset)
{
    relocation_coordinator_fence_t result;
    result.owner_id = read_text8 (bytes, offset, "coordinator owner ID");
    result.lease_generation = read_u64 (bytes, offset);
    result.node_routing_id =
      read_bytes8 (bytes, offset, "coordinator node RID");
    result.node_generation = read_u64 (bytes, offset);
    result.expected_authority_store_version =
      read_text16 (bytes, offset, "expected authority StoreVersion");
    if (result.lease_generation == 0 || result.node_generation == 0)
        throw service_wire_error_t (
          "relocation coordinator contains a zero required fence");
    return result;
}

void append_nonzero_u64 (std::vector<std::uint8_t> &bytes,
                         std::uint64_t value,
                         const char *field)
{
    if (value == 0)
        throw service_wire_error_t (std::string (field) + " must be nonzero");
    append_u64 (bytes, value);
}

std::uint64_t read_nonzero_u64 (std::span<const std::uint8_t> bytes,
                                std::size_t &offset,
                                const char *field)
{
    const auto value = read_u64 (bytes, offset);
    if (value == 0)
        throw service_wire_error_t (std::string (field) + " must be nonzero");
    return value;
}

void append_ordinal (std::vector<std::uint8_t> &bytes,
                     std::uint64_t value,
                     const char *field)
{
    if (value > static_cast<std::uint64_t> (
                  std::numeric_limits<std::int64_t>::max ()))
        throw service_wire_error_t (std::string (field)
                                    + " exceeds the signed ordinal range");
    append_u64 (bytes, value);
}

std::uint64_t read_ordinal (std::span<const std::uint8_t> bytes,
                            std::size_t &offset,
                            const char *field)
{
    const auto value = read_u64 (bytes, offset);
    if (value > static_cast<std::uint64_t> (
                  std::numeric_limits<std::int64_t>::max ()))
        throw service_wire_error_t (std::string (field)
                                    + " exceeds the signed ordinal range");
    return value;
}

void append_relocation_id (std::vector<std::uint8_t> &bytes,
                           const relocation_id_t &value)
{
    if (value.high == 0 && value.low == 0)
        throw service_wire_error_t ("relocation ID must not be zero");
    append_u64 (bytes, value.high);
    append_u64 (bytes, value.low);
}

relocation_id_t read_relocation_id (std::span<const std::uint8_t> bytes,
                                    std::size_t &offset)
{
    relocation_id_t result{read_u64 (bytes, offset), read_u64 (bytes, offset)};
    if (result.high == 0 && result.low == 0)
        throw service_wire_error_t ("relocation ID must not be zero");
    return result;
}

void append_role (std::vector<std::uint8_t> &bytes,
                  relocation_role_t role)
{
    if (role != relocation_role_t::source
        && role != relocation_role_t::target
        && role != relocation_role_t::coordinator)
        throw service_wire_error_t ("invalid relocation role");
    bytes.push_back (static_cast<std::uint8_t> (role));
}

relocation_role_t read_role (std::span<const std::uint8_t> bytes,
                             std::size_t &offset)
{
    if (offset >= bytes.size ())
        throw service_wire_error_t ("relocation role is truncated");
    const auto role = static_cast<relocation_role_t> (bytes[offset++]);
    if (role != relocation_role_t::source
        && role != relocation_role_t::target
        && role != relocation_role_t::coordinator)
        throw service_wire_error_t ("invalid relocation role");
    return role;
}

void append_round (std::vector<std::uint8_t> &bytes,
                   relocation_round_t round)
{
    if (round != relocation_round_t::initial
        && round != relocation_round_t::prepared_replacement
        && round != relocation_round_t::post_commit_replacement)
        throw service_wire_error_t ("invalid relocation reservation round");
    bytes.push_back (static_cast<std::uint8_t> (round));
}

relocation_round_t read_round (std::span<const std::uint8_t> bytes,
                               std::size_t &offset)
{
    if (offset >= bytes.size ())
        throw service_wire_error_t ("relocation reservation round is truncated");
    const auto round = static_cast<relocation_round_t> (bytes[offset++]);
    if (round != relocation_round_t::initial
        && round != relocation_round_t::prepared_replacement
        && round != relocation_round_t::post_commit_replacement)
        throw service_wire_error_t ("invalid relocation reservation round");
    return round;
}

void append_candidate (std::vector<std::uint8_t> &bytes,
                       const relocation_candidate_t &value)
{
    append_bytes8 (bytes, value.node_routing_id, "target node RID");
    append_nonzero_u64 (bytes, value.node_generation,
                        "target node generation");
    append_text8 (bytes, value.owner_id, "target owner ID");
    append_nonzero_u64 (bytes, value.owner_lease_generation,
                        "target owner lease generation");
}

relocation_candidate_t read_candidate (std::span<const std::uint8_t> bytes,
                                       std::size_t &offset)
{
    relocation_candidate_t result;
    result.node_routing_id = read_bytes8 (bytes, offset, "target node RID");
    result.node_generation = read_nonzero_u64 (
      bytes, offset, "target node generation");
    result.owner_id = read_text8 (bytes, offset, "target owner ID");
    result.owner_lease_generation = read_nonzero_u64 (
      bytes, offset, "target owner lease generation");
    return result;
}

void append_object (std::vector<std::uint8_t> &bytes,
                    const relocation_object_t &value)
{
    std::vector<std::uint8_t> body;
    switch (value.kind) {
        case relocation_object_kind_t::actor:
        case relocation_object_kind_t::user_spot:
            append_text8 (body, value.object_id, "relocation object ID");
            append_nonzero_u64 (body, value.object_generation,
                                "relocation object generation");
            append_nonzero_u64 (body,
                                value.expected_authority_owner_generation,
                                "expected authority owner generation");
            break;
        case relocation_object_kind_t::instance_spot:
            append_text8 (body, value.stable_type, "Instance Spot type");
            append_text8 (body, value.object_id, "Instance Spot ID");
            append_nonzero_u64 (body, value.object_generation,
                                "Instance Spot generation");
            if (value.expected_authority_owner_generation != 0)
                throw service_wire_error_t (
                  "Instance Spot identity cannot contain an authority generation");
            break;
        default:
            throw service_wire_error_t ("invalid relocation object kind");
    }
    if (body.size () > std::numeric_limits<std::uint16_t>::max ())
        throw service_wire_error_t ("relocation object body exceeds u16");
    bytes.push_back (static_cast<std::uint8_t> (value.kind));
    append_u16 (bytes, static_cast<std::uint16_t> (body.size ()));
    bytes.insert (bytes.end (), body.begin (), body.end ());
}

relocation_object_t read_object (std::span<const std::uint8_t> bytes,
                                 std::size_t &offset)
{
    if (offset >= bytes.size ())
        throw service_wire_error_t ("relocation object kind is truncated");
    relocation_object_t result;
    result.kind = static_cast<relocation_object_kind_t> (bytes[offset++]);
    const auto length = read_u16 (bytes, offset);
    if (bytes.size () - offset < length)
        throw service_wire_error_t ("relocation object body is truncated");
    const auto body = bytes.subspan (offset, length);
    offset += length;
    std::size_t body_offset = 0;
    switch (result.kind) {
        case relocation_object_kind_t::actor:
        case relocation_object_kind_t::user_spot:
            result.object_id = read_text8 (body, body_offset,
                                           "relocation object ID");
            result.object_generation = read_nonzero_u64 (
              body, body_offset, "relocation object generation");
            result.expected_authority_owner_generation = read_nonzero_u64 (
              body, body_offset, "expected authority owner generation");
            break;
        case relocation_object_kind_t::instance_spot:
            result.stable_type = read_text8 (body, body_offset,
                                             "Instance Spot type");
            result.object_id = read_text8 (body, body_offset,
                                           "Instance Spot ID");
            result.object_generation = read_nonzero_u64 (
              body, body_offset, "Instance Spot generation");
            break;
        default:
            throw service_wire_error_t ("invalid relocation object kind");
    }
    if (body_offset != body.size ())
        throw service_wire_error_t ("relocation object has trailing bytes");
    return result;
}

void append_participants (std::vector<std::uint8_t> &bytes,
                          const std::vector<relocation_participant_t> &values)
{
    if (values.size () > relocation_resource_participant_limit)
        throw service_wire_error_t (
          "relocation participant count exceeds the protocol limit");
    append_u32 (bytes, static_cast<std::uint32_t> (values.size ()));
    std::uint64_t previous = 0;
    for (const auto &value : values) {
        if (value.participant_id == 0 || value.participant_id <= previous)
            throw service_wire_error_t (
              "relocation participants must be sorted and unique");
        previous = value.participant_id;
        append_u64 (bytes, value.participant_id);
        std::vector<std::uint8_t> identity;
        if (value.kind == relocation_participant_kind_t::bound_session) {
            append_bytes8 (identity, value.session_owner_node_routing_id,
                           "session owner node RID");
            append_nonzero_u64 (identity, value.session_owner_node_generation,
                                "session owner node generation");
            append_text8 (identity, value.session_owner_id,
                          "session owner ID");
            append_nonzero_u64 (identity, value.session_owner_lease_generation,
                                "session owner lease generation");
            append_bytes8 (identity, value.session_routing_id, "Session RID");
            append_nonzero_u64 (identity, value.binding_generation,
                                "binding generation");
        }
        else if (value.kind != relocation_participant_kind_t::object_mailbox) {
            throw service_wire_error_t ("invalid relocation participant kind");
        }
        if (identity.size () > std::numeric_limits<std::uint16_t>::max ())
            throw service_wire_error_t ("participant identity exceeds u16");
        bytes.push_back (static_cast<std::uint8_t> (value.kind));
        append_u16 (bytes, static_cast<std::uint16_t> (identity.size ()));
        bytes.insert (bytes.end (), identity.begin (), identity.end ());
        append_ordinal (bytes, value.allowance_messages,
                        "participant message allowance");
        append_ordinal (bytes, value.allowance_bytes,
                        "participant byte allowance");
    }
}

std::vector<relocation_participant_t> read_participants (
  std::span<const std::uint8_t> bytes, std::size_t &offset)
{
    const auto count = read_u32 (bytes, offset);
    if (count > relocation_resource_participant_limit)
        throw service_wire_error_t (
          "relocation participant count exceeds the protocol limit");
    // Every participant has at least an ID, kind, empty identity length and
    // two signed ordinals. Check the encoded lower bound before reserve so a
    // truncated count cannot request memory that the frame cannot contain.
    constexpr std::size_t minimum_encoded_participant_bytes = 8 + 1 + 2 + 8 + 8;
    if (count > (bytes.size () - offset) / minimum_encoded_participant_bytes)
        throw service_wire_error_t ("relocation participant count is truncated");
    std::vector<relocation_participant_t> result;
    result.reserve (count);
    std::uint64_t previous = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        relocation_participant_t value;
        value.participant_id = read_nonzero_u64 (
          bytes, offset, "participant ID");
        if (value.participant_id <= previous)
            throw service_wire_error_t (
              "relocation participants must be sorted and unique");
        previous = value.participant_id;
        if (offset >= bytes.size ())
            throw service_wire_error_t ("participant kind is truncated");
        value.kind = static_cast<relocation_participant_kind_t> (bytes[offset++]);
        const auto length = read_u16 (bytes, offset);
        if (bytes.size () - offset < length)
            throw service_wire_error_t ("participant identity is truncated");
        const auto identity = bytes.subspan (offset, length);
        offset += length;
        std::size_t identity_offset = 0;
        if (value.kind == relocation_participant_kind_t::bound_session) {
            value.session_owner_node_routing_id = read_bytes8 (
              identity, identity_offset, "session owner node RID");
            value.session_owner_node_generation = read_nonzero_u64 (
              identity, identity_offset, "session owner node generation");
            value.session_owner_id = read_text8 (
              identity, identity_offset, "session owner ID");
            value.session_owner_lease_generation = read_nonzero_u64 (
              identity, identity_offset, "session owner lease generation");
            value.session_routing_id = read_bytes8 (
              identity, identity_offset, "Session RID");
            value.binding_generation = read_nonzero_u64 (
              identity, identity_offset, "binding generation");
        }
        else if (value.kind != relocation_participant_kind_t::object_mailbox
                 || !identity.empty ()) {
            throw service_wire_error_t ("invalid relocation participant identity");
        }
        if (identity_offset != identity.size ())
            throw service_wire_error_t ("participant identity has trailing bytes");
        value.allowance_messages = read_ordinal (
          bytes, offset, "participant message allowance");
        value.allowance_bytes = read_ordinal (
          bytes, offset, "participant byte allowance");
        result.push_back (std::move (value));
    }
    return result;
}

void append_root (std::vector<std::uint8_t> &bytes,
                  const std::optional<relocation_root_t> &root)
{
    if (!root) {
        bytes.push_back (0);
        append_u16 (bytes, 0);
        return;
    }
    std::vector<std::uint8_t> body;
    append_text16 (body, root->reference, "relocation reference");
    append_u32 (body, root->checksum_crc32c);
    bytes.push_back (1);
    append_u16 (bytes, static_cast<std::uint16_t> (body.size ()));
    bytes.insert (bytes.end (), body.begin (), body.end ());
}

std::optional<relocation_root_t> read_root (
  std::span<const std::uint8_t> bytes, std::size_t &offset)
{
    if (offset >= bytes.size ())
        throw service_wire_error_t ("relocation root presence is truncated");
    const auto present = bytes[offset++];
    if (present > 1)
        throw service_wire_error_t ("invalid relocation root presence");
    const auto length = read_u16 (bytes, offset);
    if (bytes.size () - offset < length)
        throw service_wire_error_t ("relocation root is truncated");
    const auto body = bytes.subspan (offset, length);
    offset += length;
    if (present == 0) {
        if (!body.empty ())
            throw service_wire_error_t ("absent relocation root has a body");
        return std::nullopt;
    }
    std::size_t body_offset = 0;
    relocation_root_t result;
    result.reference = read_text16 (body, body_offset, "relocation reference");
    result.checksum_crc32c = read_u32 (body, body_offset);
    if (body_offset != body.size ())
        throw service_wire_error_t ("relocation root has trailing bytes");
    return result;
}

template <typename Record>
void append_relocation_base (std::vector<std::uint8_t> &bytes,
                             const Record &record)
{
    append_relocation_id (bytes, record.relocation);
    append_nonzero_u64 (bytes, record.target_attempt_generation,
                        "target attempt generation");
    append_coordinator (bytes, record.coordinator);
}

template <typename Record>
void read_relocation_base (std::span<const std::uint8_t> bytes,
                           std::size_t &offset,
                           Record &record)
{
    record.relocation = read_relocation_id (bytes, offset);
    record.target_attempt_generation = read_nonzero_u64 (
      bytes, offset, "target attempt generation");
    record.coordinator = read_coordinator (bytes, offset);
}

void append_request_source (std::vector<std::uint8_t> &bytes,
                            const request_source_fence_t &source)
{
    append_text8 (bytes, source.owner_id, "source owner ID");
    append_nonzero_u64 (bytes, source.lease_generation,
                        "source owner lease generation");
    append_bytes8 (bytes, source.node_routing_id, "source node RID");
    append_nonzero_u64 (bytes, source.node_generation,
                        "source node generation");
}

request_source_fence_t read_request_source (
  std::span<const std::uint8_t> bytes, std::size_t &offset)
{
    request_source_fence_t result;
    result.owner_id = read_text8 (bytes, offset, "source owner ID");
    result.lease_generation = read_nonzero_u64 (
      bytes, offset, "source owner lease generation");
    result.node_routing_id = read_bytes8 (bytes, offset, "source node RID");
    result.node_generation = read_nonzero_u64 (
      bytes, offset, "source node generation");
    return result;
}
}

std::vector<std::uint8_t> encode_reply_relay (const reply_relay_t &record)
{
    if ((record.operation.high == 0 && record.operation.low == 0)
        || record.reply_route_id == 0
        || (record.relocation.high == 0 && record.relocation.low == 0)
        || record.target_attempt_generation == 0
        || record.participant_id == 0 || record.sequence == 0
        || !valid_terminal_failure (record.terminal_result,
                                    record.failure_code)) {
        throw service_wire_error_t (
          "reply relay contains an invalid required field");
    }
    std::vector<std::uint8_t> context;
    append_u64 (context, record.relocation.high);
    append_u64 (context, record.relocation.low);
    append_u64 (context, record.target_attempt_generation);
    append_coordinator (context, record.coordinator);
    append_u64 (context, record.participant_id);
    append_u64 (context, record.sequence);
    if (context.size () > std::numeric_limits<std::uint16_t>::max ())
        throw service_wire_error_t ("reply relay context exceeds u16");

    std::vector<std::uint8_t> bytes{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::replyRelay), 0};
    append_u64 (bytes, record.operation.high);
    append_u64 (bytes, record.operation.low);
    append_u64 (bytes, record.reply_route_id);
    bytes.push_back (2);
    append_u16 (bytes, static_cast<std::uint16_t> (context.size ()));
    bytes.insert (bytes.end (), context.begin (), context.end ());
    append_u32 (bytes, record.terminal_result);
    append_u32 (bytes, static_cast<std::uint32_t> (record.failure_code));
    return bytes;
}

reply_relay_t decode_reply_relay (std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::replyRelay || header.flags != 0)
        throw service_wire_error_t ("record is not a reply relay");
    std::size_t offset = prefix_size;
    reply_relay_t record;
    record.operation.high = read_u64 (bytes, offset);
    record.operation.low = read_u64 (bytes, offset);
    record.reply_route_id = read_u64 (bytes, offset);
    if (offset >= bytes.size () || bytes[offset++] != 2)
        throw service_wire_error_t (
          "reply relay context is not maintenance relocation");
    const auto context_length = read_u16 (bytes, offset);
    if (bytes.size () - offset < context_length)
        throw service_wire_error_t ("reply relay context is truncated");
    const auto context = bytes.subspan (offset, context_length);
    offset += context_length;
    std::size_t context_offset = 0;
    record.relocation.high = read_u64 (context, context_offset);
    record.relocation.low = read_u64 (context, context_offset);
    record.target_attempt_generation = read_u64 (context, context_offset);
    record.coordinator = read_coordinator (context, context_offset);
    record.participant_id = read_u64 (context, context_offset);
    record.sequence = read_u64 (context, context_offset);
    if (context_offset != context.size ())
        throw service_wire_error_t ("reply relay context has trailing bytes");
    record.terminal_result = read_u32 (bytes, offset);
    record.failure_code =
      static_cast<framework_error_code> (read_u32 (bytes, offset));
    if (offset != bytes.size ())
        throw service_wire_error_t ("reply relay has trailing bytes");
    (void) encode_reply_relay (record);
    return record;
}

std::vector<std::uint8_t> encode_reply_relay_ack (
  const reply_relay_ack_t &record)
{
    if ((record.relocation.high == 0 && record.relocation.low == 0)
        || (record.operation.high == 0 && record.operation.low == 0)
        || record.reply_route_id == 0
        || record.request_source.lease_generation == 0
        || record.request_source.node_generation == 0
        || (record.status != reply_relay_ack_status_t::terminal_received
            && record.status != reply_relay_ack_status_t::already_terminal)) {
        throw service_wire_error_t (
          "reply relay ACK contains an invalid required field");
    }
    std::vector<std::uint8_t> bytes{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::replyRelayAck), 0};
    append_u64 (bytes, record.relocation.high);
    append_u64 (bytes, record.relocation.low);
    append_coordinator (bytes, record.coordinator);
    append_u64 (bytes, record.operation.high);
    append_u64 (bytes, record.operation.low);
    append_u64 (bytes, record.reply_route_id);
    append_text8 (bytes, record.request_source.owner_id,
                  "request source owner ID");
    append_u64 (bytes, record.request_source.lease_generation);
    append_bytes8 (bytes, record.request_source.node_routing_id,
                   "request source node RID");
    append_u64 (bytes, record.request_source.node_generation);
    bytes.push_back (static_cast<std::uint8_t> (record.status));
    return bytes;
}

reply_relay_ack_t decode_reply_relay_ack (
  std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::replyRelayAck || header.flags != 0)
        throw service_wire_error_t ("record is not a reply relay ACK");
    std::size_t offset = prefix_size;
    reply_relay_ack_t record;
    record.relocation.high = read_u64 (bytes, offset);
    record.relocation.low = read_u64 (bytes, offset);
    record.coordinator = read_coordinator (bytes, offset);
    record.operation.high = read_u64 (bytes, offset);
    record.operation.low = read_u64 (bytes, offset);
    record.reply_route_id = read_u64 (bytes, offset);
    record.request_source.owner_id =
      read_text8 (bytes, offset, "request source owner ID");
    record.request_source.lease_generation = read_u64 (bytes, offset);
    record.request_source.node_routing_id =
      read_bytes8 (bytes, offset, "request source node RID");
    record.request_source.node_generation = read_u64 (bytes, offset);
    if (offset >= bytes.size ())
        throw service_wire_error_t ("reply relay ACK status is truncated");
    record.status = static_cast<reply_relay_ack_status_t> (bytes[offset++]);
    if (offset != bytes.size ())
        throw service_wire_error_t ("reply relay ACK has trailing bytes");
    (void) encode_reply_relay_ack (record);
    return record;
}

std::vector<std::uint8_t> encode_relocation_control (
  const relocation_control_t &record)
{
    return std::visit ([] (const auto &value) {
        using record_t = std::decay_t<decltype (value)>;
        command kind;
        std::uint8_t flags = 0;
        if constexpr (std::is_same_v<record_t, relocation_prepare_t>)
            kind = command::relocationPrepare;
        else if constexpr (std::is_same_v<record_t, relocation_ready_t>) {
            kind = command::relocationReady;
            flags = 8;
        }
        else if constexpr (std::is_same_v<record_t, relocation_reserved_t>)
            kind = command::relocationReserved;
        else if constexpr (std::is_same_v<record_t, relocation_data_t>)
            kind = command::relocationData;
        else if constexpr (std::is_same_v<record_t, relocation_ack_t>)
            kind = command::relocationAck;
        else if constexpr (std::is_same_v<record_t, relocation_seal_t>)
            kind = command::relocationSeal;
        else
            kind = command::relocationComplete;

        std::vector<std::uint8_t> bytes{
          magic[0], magic[1], wire_major,
          static_cast<std::uint8_t> (kind), flags};

        if constexpr (std::is_same_v<record_t, relocation_prepare_t>) {
            append_relocation_id (bytes, value.relocation);
            append_nonzero_u64 (bytes, value.target_attempt_generation,
                                "target attempt generation");
            append_round (bytes, value.round);
            append_coordinator (bytes, value.coordinator);
            append_candidate (bytes, value.candidate);
            append_role (bytes, value.initiator_role);
            append_object (bytes, value.object);
            append_bytes8 (bytes, value.source_node_routing_id,
                           "source node RID");
            append_nonzero_u64 (bytes, value.source_node_generation,
                                "source node generation");
            append_ordinal (bytes, value.required_messages,
                            "required messages");
            append_ordinal (bytes, value.required_bytes, "required bytes");
            append_participants (bytes, value.participants);
            append_root (bytes, value.root);
            append_ordinal (bytes, value.application_version,
                            "application version");
        }
        else if constexpr (std::is_same_v<record_t, relocation_ready_t>) {
            const auto target_offer = value.role == relocation_role_t::target;
            const auto source_accept = value.role == relocation_role_t::source;
            if ((target_offer
                 && (value.offered_messages == 0 || value.offered_bytes == 0
                     || !value.participants.empty ()))
                || (source_accept
                    && (value.offered_messages != 0 || value.offered_bytes != 0
                        || value.participants.empty ()))
                || (!target_offer && !source_accept)) {
                throw service_wire_error_t (
                  "relocation ready offer or accept fields do not match its role");
            }
            append_relocation_id (bytes, value.relocation);
            append_nonzero_u64 (bytes, value.target_attempt_generation,
                                "target attempt generation");
            append_round (bytes, value.round);
            append_coordinator (bytes, value.coordinator);
            append_candidate (bytes, value.candidate);
            append_object (bytes, value.object);
            append_role (bytes, value.role);
            append_ordinal (bytes, value.offered_messages,
                            "offered messages");
            append_ordinal (bytes, value.offered_bytes, "offered bytes");
            append_participants (bytes, value.participants);

            std::vector<std::uint8_t> extension;
            std::vector<std::uint8_t> field;
            append_nonzero_u64 (field, value.source_node_generation,
                                "source node generation");
            append_tlv (extension, 2, field);
            field.clear ();
            append_nonzero_u64 (field, value.target_node_generation,
                                "target node generation");
            append_tlv (extension, 3, field);
            field.clear ();
            append_nonzero_u64 (field, value.reservation_generation,
                                "reservation generation");
            append_tlv (extension, 4, field);
            if (value.root) {
                field.clear ();
                append_text16 (field, value.root->reference,
                               "relocation reference");
                append_tlv (extension, 5, field);
                field.clear ();
                append_u32 (field, value.root->checksum_crc32c);
                append_tlv (extension, 6, field);
            }
            field.clear ();
            append_ordinal (field, value.application_version,
                            "application version");
            append_tlv (extension, 8, field);
            field.clear ();
            if (value.participant_progress.size ()
                > relocation_resource_participant_limit)
                throw service_wire_error_t (
                  "relocation participant progress count exceeds the protocol limit");
            append_u32 (field, static_cast<std::uint32_t> (
                                 value.participant_progress.size ()));
            std::uint64_t previous = 0;
            for (const auto &progress : value.participant_progress) {
                if (progress.participant_id == 0
                    || progress.participant_id <= previous
                    || progress.replay_cursor > progress.accepted_boundary)
                    throw service_wire_error_t (
                      "relocation participant progress is invalid");
                previous = progress.participant_id;
                append_u64 (field, progress.participant_id);
                append_ordinal (field, progress.accepted_boundary,
                                "accepted boundary");
                append_ordinal (field, progress.replay_cursor,
                                "replay cursor");
            }
            append_tlv (extension, 9, field);
            if (extension.size () > 1048576)
                throw service_wire_error_t (
                  "relocation extension exceeds 1048576 bytes");
            append_u32 (bytes, static_cast<std::uint32_t> (extension.size ()));
            bytes.insert (bytes.end (), extension.begin (), extension.end ());
        }
        else if constexpr (std::is_same_v<record_t, relocation_reserved_t>) {
            append_relocation_id (bytes, value.relocation);
            append_nonzero_u64 (bytes, value.target_attempt_generation,
                                "target attempt generation");
            append_round (bytes, value.round);
            append_coordinator (bytes, value.coordinator);
            append_candidate (bytes, value.candidate);
            append_nonzero_u64 (bytes, value.reservation_generation,
                                "reservation generation");
            append_participants (bytes, value.participants);
        }
        else if constexpr (std::is_same_v<record_t, relocation_data_t>) {
            append_relocation_base (bytes, value);
            append_role (bytes, value.sender_role);
            append_nonzero_u64 (bytes, value.participant_id,
                                "participant ID");
            append_nonzero_u64 (bytes, value.sequence, "sequence");
            if (value.frozen_record) {
                const auto frozen = encode_frozen_record (
                  *value.frozen_record);
                const auto decoded = decode_frozen_record (frozen);
                if (decoded.source != value.source)
                    throw service_wire_error_t (
                      "relocation data source does not match its frozen record");
                bytes.insert (bytes.end (), frozen.begin (), frozen.end ());
                return bytes;
            }
            bytes.push_back (13);
            std::vector<std::uint8_t> source;
            append_bytes8 (source, value.source.node_routing_id,
                           "source node RID");
            append_nonzero_u64 (source, value.source.node_generation,
                                "source node generation");
            append_text8 (source, value.source.owner_id, "source owner ID");
            append_nonzero_u64 (source, value.source.lease_generation,
                                "source owner lease generation");
            bytes.push_back (1);
            append_u16 (bytes, static_cast<std::uint16_t> (source.size ()));
            bytes.insert (bytes.end (), source.begin (), source.end ());
            bytes.push_back (0);
            append_u64 (bytes, 0);
            append_u64 (bytes, 0);
            append_u32 (bytes, 0);
            append_u16 (bytes, 0);
            const auto phase = static_cast<std::uint8_t> (value.phase);
            if (phase > 9)
                throw service_wire_error_t ("invalid relocation control phase");
            bytes.push_back (phase);
            append_role (bytes, value.sender_role);
            append_relocation_id (bytes, value.relocation);
            append_object (bytes, value.object);
            if (!valid_terminal_failure (value.terminal_result,
                                         value.failure_code))
                throw service_wire_error_t (
                  "relocation control terminal and failure do not match");
            append_u32 (bytes, value.terminal_result);
            append_u32 (bytes, static_cast<std::uint32_t> (
                                 value.failure_code));
        }
        else if constexpr (std::is_same_v<record_t, relocation_ack_t>) {
            append_relocation_base (bytes, value);
            append_role (bytes, value.sender_role);
            append_nonzero_u64 (bytes, value.participant_id,
                                "participant ID");
            append_ordinal (bytes, value.high_water, "high water");
        }
        else if constexpr (std::is_same_v<record_t, relocation_seal_t>) {
            append_relocation_base (bytes, value);
            append_role (bytes, value.sender_role);
            bytes.push_back (value.response ? 1 : 0);
            if (value.participants.size ()
                > relocation_resource_participant_limit)
                throw service_wire_error_t (
                  "relocation terminal participant count exceeds the protocol limit");
            append_u32 (bytes, static_cast<std::uint32_t> (
                                 value.participants.size ()));
            std::uint64_t previous = 0;
            for (const auto &participant : value.participants) {
                if (participant.participant_id == 0
                    || participant.participant_id <= previous)
                    throw service_wire_error_t (
                      "relocation terminal participants must be sorted and unique");
                previous = participant.participant_id;
                append_u64 (bytes, participant.participant_id);
                append_ordinal (bytes, participant.high_water, "high water");
            }
        }
        else {
            append_relocation_base (bytes, value);
            append_role (bytes, value.sender_role);
            append_request_source (bytes, value.source);
            const auto state = static_cast<std::uint8_t> (
              value.source_cleanup_state);
            if (state > 2)
                throw service_wire_error_t ("invalid source cleanup state");
            bytes.push_back (state);
        }
        return bytes;
    }, record);
}

relocation_control_t decode_relocation_control (
  std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    const auto expected_flags =
      header.kind == command::relocationReady ? 8 : 0;
    if (header.flags != expected_flags)
        throw service_wire_error_t ("invalid relocation control flags");
    std::size_t offset = prefix_size;
    switch (header.kind) {
        case command::relocationPrepare: {
            relocation_prepare_t result;
            result.relocation = read_relocation_id (bytes, offset);
            result.target_attempt_generation = read_nonzero_u64 (
              bytes, offset, "target attempt generation");
            result.round = read_round (bytes, offset);
            result.coordinator = read_coordinator (bytes, offset);
            result.candidate = read_candidate (bytes, offset);
            result.initiator_role = read_role (bytes, offset);
            result.object = read_object (bytes, offset);
            result.source_node_routing_id = read_bytes8 (
              bytes, offset, "source node RID");
            result.source_node_generation = read_nonzero_u64 (
              bytes, offset, "source node generation");
            result.required_messages = read_ordinal (
              bytes, offset, "required messages");
            result.required_bytes = read_ordinal (
              bytes, offset, "required bytes");
            result.participants = read_participants (bytes, offset);
            result.root = read_root (bytes, offset);
            result.application_version = read_ordinal (
              bytes, offset, "application version");
            if (offset != bytes.size ())
                throw service_wire_error_t (
                  "relocation prepare has trailing bytes");
            return result;
        }
        case command::relocationReady: {
            relocation_ready_t result;
            result.relocation = read_relocation_id (bytes, offset);
            result.target_attempt_generation = read_nonzero_u64 (
              bytes, offset, "target attempt generation");
            result.round = read_round (bytes, offset);
            result.coordinator = read_coordinator (bytes, offset);
            result.candidate = read_candidate (bytes, offset);
            result.object = read_object (bytes, offset);
            result.role = read_role (bytes, offset);
            result.offered_messages = read_ordinal (
              bytes, offset, "offered messages");
            result.offered_bytes = read_ordinal (
              bytes, offset, "offered bytes");
            result.participants = read_participants (bytes, offset);
            const auto target_offer = result.role == relocation_role_t::target;
            const auto source_accept = result.role == relocation_role_t::source;
            if ((target_offer
                 && (result.offered_messages == 0 || result.offered_bytes == 0
                     || !result.participants.empty ()))
                || (source_accept
                    && (result.offered_messages != 0 || result.offered_bytes != 0
                        || result.participants.empty ()))
                || (!target_offer && !source_accept)) {
                throw service_wire_error_t (
                  "relocation ready offer or accept fields do not match its role");
            }
            const auto extension_length = read_u32 (bytes, offset);
            if (extension_length > 1048576
                || bytes.size () - offset < extension_length)
                throw service_wire_error_t (
                  "relocation extension is truncated or exceeds its bound");
            const auto extension = bytes.subspan (offset, extension_length);
            offset += extension_length;
            std::size_t extension_offset = 0;
            std::uint8_t previous_id = 0;
            bool source_seen = false;
            bool target_seen = false;
            bool reservation_seen = false;
            bool reference_seen = false;
            bool checksum_seen = false;
            bool version_seen = false;
            bool progress_seen = false;
            std::string reference;
            std::uint32_t checksum = 0;
            while (extension_offset < extension.size ()) {
                const auto id = extension[extension_offset++];
                if (id <= previous_id)
                    throw service_wire_error_t (
                      "relocation extension fields must be ordered and unique");
                previous_id = id;
                const auto length = read_u32 (extension, extension_offset);
                if (extension.size () - extension_offset < length)
                    throw service_wire_error_t (
                      "relocation extension field is truncated");
                const auto field = extension.subspan (extension_offset, length);
                extension_offset += length;
                std::size_t field_offset = 0;
                if (id == 2) {
                    result.source_node_generation = read_nonzero_u64 (
                      field, field_offset, "source node generation");
                    source_seen = true;
                }
                else if (id == 3) {
                    result.target_node_generation = read_nonzero_u64 (
                      field, field_offset, "target node generation");
                    target_seen = true;
                }
                else if (id == 4) {
                    result.reservation_generation = read_nonzero_u64 (
                      field, field_offset, "reservation generation");
                    reservation_seen = true;
                }
                else if (id == 5) {
                    reference = read_text16 (
                      field, field_offset, "relocation reference");
                    reference_seen = true;
                }
                else if (id == 6) {
                    checksum = read_u32 (field, field_offset);
                    checksum_seen = true;
                }
                else if (id == 8) {
                    result.application_version = read_ordinal (
                      field, field_offset, "application version");
                    version_seen = true;
                }
                else if (id == 9) {
                    const auto count = read_u32 (field, field_offset);
                    if (count > relocation_resource_participant_limit)
                        throw service_wire_error_t (
                          "relocation participant progress count exceeds the protocol limit");
                    constexpr std::size_t minimum_encoded_progress_bytes = 8 + 8 + 8;
                    if (count > (field.size () - field_offset)
                                   / minimum_encoded_progress_bytes)
                        throw service_wire_error_t (
                          "relocation participant progress count is truncated");
                    std::uint64_t previous = 0;
                    result.participant_progress.reserve (count);
                    for (std::uint32_t index = 0; index < count; ++index) {
                        relocation_participant_progress_t progress;
                        progress.participant_id = read_nonzero_u64 (
                          field, field_offset, "participant ID");
                        progress.accepted_boundary = read_ordinal (
                          field, field_offset, "accepted boundary");
                        progress.replay_cursor = read_ordinal (
                          field, field_offset, "replay cursor");
                        if (progress.participant_id <= previous
                            || progress.replay_cursor
                                 > progress.accepted_boundary)
                            throw service_wire_error_t (
                              "relocation participant progress is invalid");
                        previous = progress.participant_id;
                        result.participant_progress.push_back (progress);
                    }
                    progress_seen = true;
                }
                else {
                    field_offset = field.size ();
                }
                if (field_offset != field.size ())
                    throw service_wire_error_t (
                      "relocation extension field has trailing bytes");
            }
            if (!source_seen || !target_seen || !reservation_seen
                || !version_seen || !progress_seen
                || reference_seen != checksum_seen)
                throw service_wire_error_t (
                  "relocation extension is missing a required field");
            if (reference_seen)
                result.root = relocation_root_t{std::move (reference), checksum};
            if (offset != bytes.size ())
                throw service_wire_error_t ("relocation ready has trailing bytes");
            return result;
        }
        case command::relocationReserved: {
            relocation_reserved_t result;
            result.relocation = read_relocation_id (bytes, offset);
            result.target_attempt_generation = read_nonzero_u64 (
              bytes, offset, "target attempt generation");
            result.round = read_round (bytes, offset);
            result.coordinator = read_coordinator (bytes, offset);
            result.candidate = read_candidate (bytes, offset);
            result.reservation_generation = read_nonzero_u64 (
              bytes, offset, "reservation generation");
            result.participants = read_participants (bytes, offset);
            if (offset != bytes.size ())
                throw service_wire_error_t (
                  "relocation reserved has trailing bytes");
            return result;
        }
        case command::relocationData: {
            relocation_data_t result;
            read_relocation_base (bytes, offset, result);
            result.sender_role = read_role (bytes, offset);
            result.participant_id = read_nonzero_u64 (
              bytes, offset, "participant ID");
            result.sequence = read_nonzero_u64 (bytes, offset, "sequence");
            if (offset >= bytes.size ())
                throw service_wire_error_t (
                  "relocation data frozen record is truncated");
            if (bytes[offset] != 13) {
                auto frozen = decode_frozen_record (bytes.subspan (offset));
                result.source = frozen.source;
                result.frozen_record = std::move (frozen);
                return result;
            }
            if (bytes[offset++] != 13 || offset >= bytes.size ()
                || bytes[offset++] != 1)
                throw service_wire_error_t (
                  "relocation data must carry a node relocationControl record");
            const auto source_length = read_u16 (bytes, offset);
            if (bytes.size () - offset < source_length)
                throw service_wire_error_t (
                  "relocation data source is truncated");
            const auto source = bytes.subspan (offset, source_length);
            offset += source_length;
            std::size_t source_offset = 0;
            result.source.node_routing_id = read_bytes8 (
              source, source_offset, "source node RID");
            result.source.node_generation = read_nonzero_u64 (
              source, source_offset, "source node generation");
            result.source.owner_id = read_text8 (
              source, source_offset, "source owner ID");
            result.source.lease_generation = read_nonzero_u64 (
              source, source_offset, "source owner lease generation");
            if (source_offset != source.size ())
                throw service_wire_error_t (
                  "relocation data source has trailing bytes");
            if (offset >= bytes.size () || bytes[offset++] != 0)
                throw service_wire_error_t (
                  "relocation data cannot contain metadata");
            const wire_operation_id_t operation{
              read_u64 (bytes, offset), read_u64 (bytes, offset)};
            if ((operation.high != 0 || operation.low != 0)
                || read_u32 (bytes, offset) != 0
                || read_u16 (bytes, offset) != 0)
                throw service_wire_error_t (
                  "relocation data operation or reply route changed");
            if (offset >= bytes.size ())
                throw service_wire_error_t (
                  "relocation data control phase is truncated");
            result.phase = static_cast<relocation_phase_t> (bytes[offset++]);
            const auto phase = static_cast<std::uint8_t> (result.phase);
            if (phase > 9)
                throw service_wire_error_t (
                  "invalid relocation data control phase");
            if (read_role (bytes, offset) != result.sender_role
                || read_relocation_id (bytes, offset) != result.relocation)
                throw service_wire_error_t (
                  "relocation data control fence changed");
            result.object = read_object (bytes, offset);
            result.terminal_result = read_u32 (bytes, offset);
            result.failure_code = static_cast<framework_error_code> (
              read_u32 (bytes, offset));
            if (!valid_terminal_failure (result.terminal_result,
                                         result.failure_code)
                || offset != bytes.size ())
                throw service_wire_error_t (
                  "relocation data control terminal is invalid or has trailing bytes");
            return result;
        }
        case command::relocationAck: {
            relocation_ack_t result;
            read_relocation_base (bytes, offset, result);
            result.sender_role = read_role (bytes, offset);
            result.participant_id = read_nonzero_u64 (
              bytes, offset, "participant ID");
            result.high_water = read_ordinal (bytes, offset, "high water");
            if (offset != bytes.size ())
                throw service_wire_error_t ("relocation ACK has trailing bytes");
            return result;
        }
        case command::relocationSeal: {
            relocation_seal_t result;
            read_relocation_base (bytes, offset, result);
            result.sender_role = read_role (bytes, offset);
            if (offset >= bytes.size () || bytes[offset] > 1)
                throw service_wire_error_t ("invalid relocation seal response");
            result.response = bytes[offset++] != 0;
            const auto count = read_u32 (bytes, offset);
            if (count > relocation_resource_participant_limit)
                throw service_wire_error_t (
                  "relocation terminal participant count exceeds the protocol limit");
            constexpr std::size_t minimum_encoded_terminal_bytes = 8 + 8;
            if (count > (bytes.size () - offset) / minimum_encoded_terminal_bytes)
                throw service_wire_error_t (
                  "relocation terminal participant count is truncated");
            result.participants.reserve (count);
            std::uint64_t previous = 0;
            for (std::uint32_t index = 0; index < count; ++index) {
                relocation_participant_terminal_t participant;
                participant.participant_id = read_nonzero_u64 (
                  bytes, offset, "participant ID");
                participant.high_water = read_ordinal (
                  bytes, offset, "high water");
                if (participant.participant_id <= previous)
                    throw service_wire_error_t (
                      "relocation terminal participants must be sorted and unique");
                previous = participant.participant_id;
                result.participants.push_back (participant);
            }
            if (offset != bytes.size ())
                throw service_wire_error_t (
                  "relocation seal has trailing bytes");
            return result;
        }
        case command::relocationComplete: {
            relocation_complete_t result;
            read_relocation_base (bytes, offset, result);
            result.sender_role = read_role (bytes, offset);
            result.source = read_request_source (bytes, offset);
            if (offset >= bytes.size () || bytes[offset] > 2)
                throw service_wire_error_t ("invalid source cleanup state");
            result.source_cleanup_state =
              static_cast<source_cleanup_state_t> (bytes[offset++]);
            if (offset != bytes.size ())
                throw service_wire_error_t (
                  "relocation complete has trailing bytes");
            return result;
        }
        default:
            throw service_wire_error_t (
              "record is not a maintenance relocation control command");
    }
}

namespace
{
bool read_bool8 (std::span<const std::uint8_t> bytes,
                 std::size_t &offset,
                 const char *field)
{
    if (offset >= bytes.size () || bytes[offset] > 1)
        throw service_wire_error_t (std::string ("invalid or truncated ")
                                    + field);
    return bytes[offset++] != 0;
}

std::span<const std::uint8_t> read_body16 (
  std::span<const std::uint8_t> bytes,
  std::size_t &offset,
  const char *field)
{
    const auto length = read_u16 (bytes, offset);
    if (bytes.size () - offset < length)
        throw service_wire_error_t (std::string (field) + " is truncated");
    const auto result = bytes.subspan (offset, length);
    offset += length;
    return result;
}

void require_end (std::span<const std::uint8_t> bytes,
                  std::size_t offset,
                  const char *field)
{
    if (offset != bytes.size ())
        throw service_wire_error_t (std::string (field)
                                    + " has trailing bytes");
}

void read_actor_identity (std::span<const std::uint8_t> bytes,
                          std::size_t &offset,
                          const char *field)
{
    (void) read_text8 (bytes, offset, field);
    (void) read_nonzero_u64 (bytes, offset, "Actor generation");
}

void read_spot_identity (std::span<const std::uint8_t> bytes,
                         std::size_t &offset)
{
    (void) read_text8 (bytes, offset, "Spot ID");
    (void) read_nonzero_u64 (bytes, offset, "Spot generation");
}

frozen_target_identity_t read_spot_route (
  std::span<const std::uint8_t> bytes,
  std::size_t &offset)
{
    frozen_target_identity_t result;
    result.kind = relocation_object_kind_t::user_spot;
    result.object_id = read_text8 (bytes, offset, "Spot ID");
    result.object_generation = read_nonzero_u64 (
      bytes, offset, "Spot generation");
    result.target_node_routing_id = read_bytes8 (
      bytes, offset, "target node RID");
    result.target_node_generation = read_nonzero_u64 (
      bytes, offset, "target node generation");
    result.authority_owner_generation = read_nonzero_u64 (
      bytes, offset, "expected authority owner generation");
    result.owner_lease_generation = read_nonzero_u64 (
      bytes, offset, "expected owner lease generation");
    return result;
}

frozen_target_identity_t read_actor_route (
  std::span<const std::uint8_t> bytes,
  std::size_t &offset)
{
    frozen_target_identity_t result;
    result.kind = relocation_object_kind_t::actor;
    result.object_id = read_text8 (bytes, offset, "Actor ID");
    result.object_generation = read_nonzero_u64 (
      bytes, offset, "Actor generation");
    result.target_node_routing_id = read_bytes8 (
      bytes, offset, "target node RID");
    result.target_node_generation = read_nonzero_u64 (
      bytes, offset, "target node generation");
    result.authority_owner_generation = read_nonzero_u64 (
      bytes, offset, "expected authority owner generation");
    result.owner_lease_generation = read_nonzero_u64 (
      bytes, offset, "expected owner lease generation");
    return result;
}

application_payload_t read_application_payload_envelope (
  std::span<const std::uint8_t> bytes,
  std::size_t &offset)
{
    const auto start = offset;
    if (offset >= bytes.size () || bytes[offset++] != 1)
        throw service_wire_error_t (
          "application payload envelope version must be one");
    const auto length = read_u32 (bytes, offset);
    if (bytes.size () - offset < length)
        throw service_wire_error_t (
          "application payload envelope is truncated");
    const auto body = bytes.subspan (offset, length);
    offset += length;
    std::size_t body_offset = 0;
    (void) read_text8 (body, body_offset, "packet name");
    (void) read_text8 (body, body_offset, "content type");
    const auto payload_length = read_u32 (body, body_offset);
    if (body.size () - body_offset != payload_length)
        throw service_wire_error_t (
          "application payload length does not match its body");
    return decode_application_payload (
      bytes.subspan (start, offset - start));
}

void read_metadata_frame (std::span<const std::uint8_t> bytes,
                          std::size_t &offset)
{
    const auto start = offset;
    if (offset >= bytes.size () || bytes[offset++] != 1)
        throw service_wire_error_t ("metadata frame version must be one");
    if (offset >= bytes.size ())
        throw service_wire_error_t ("metadata entry count is truncated");
    const auto count = bytes[offset++];
    std::vector<std::string> keys;
    keys.reserve (count);
    for (std::uint16_t index = 0; index < count; ++index) {
        auto key = read_text8 (bytes, offset, "metadata key");
        if (std::find (keys.begin (), keys.end (), key) != keys.end ())
            throw service_wire_error_t ("metadata keys must be unique");
        keys.push_back (std::move (key));
        (void) read_text16 (bytes, offset, "metadata value");
        if (offset - start > 1024)
            throw service_wire_error_t (
              "metadata frame exceeds 1024 encoded bytes");
    }
}

void read_membership_snapshot (std::span<const std::uint8_t> bytes,
                               std::size_t &offset)
{
    read_actor_identity (bytes, offset, "membership Actor ID");
    read_spot_identity (bytes, offset);
}

void read_optional_membership_snapshot (
  std::span<const std::uint8_t> bytes,
  std::size_t &offset)
{
    const auto present = read_bool8 (bytes, offset, "membership presence");
    const auto body = read_body16 (bytes, offset, "optional membership");
    if (!present) {
        if (!body.empty ())
            throw service_wire_error_t (
              "absent membership snapshot has a body");
        return;
    }
    std::size_t body_offset = 0;
    read_membership_snapshot (body, body_offset);
    require_end (body, body_offset, "optional membership snapshot");
}

void read_actor_control (std::span<const std::uint8_t> bytes,
                         std::size_t &offset)
{
    if (offset >= bytes.size ())
        throw service_wire_error_t ("Actor lifecycle kind is truncated");
    const auto lifecycle = bytes[offset++];
    if (lifecycle < 1 || lifecycle > 5)
        throw service_wire_error_t ("invalid Actor lifecycle kind");
    const auto body = read_body16 (bytes, offset, "Actor control body");
    std::size_t body_offset = 0;
    if (lifecycle == 1 || lifecycle == 4)
        read_membership_snapshot (body, body_offset);
    else if (lifecycle == 2) {
        read_optional_membership_snapshot (body, body_offset);
        read_membership_snapshot (body, body_offset);
    }
    else if (lifecycle == 3) {
        read_membership_snapshot (body, body_offset);
        read_membership_snapshot (body, body_offset);
    }
    else
        read_membership_snapshot (body, body_offset);
    require_end (body, body_offset, "Actor control body");
}

void read_send_ready_destination (std::span<const std::uint8_t> bytes,
                                  std::size_t &offset)
{
    if (offset >= bytes.size ())
        throw service_wire_error_t ("send-ready destination is truncated");
    const auto kind = bytes[offset++];
    if (kind < 1 || kind > 5)
        throw service_wire_error_t ("invalid send-ready destination kind");
    const auto body = read_body16 (bytes, offset, "send-ready destination");
    std::size_t body_offset = 0;
    if (kind == 1)
        (void) read_bytes8 (body, body_offset, "target node RID");
    else if (kind == 2)
        (void) read_text8 (body, body_offset, "channel name");
    else if (kind == 3)
        read_spot_route (body, body_offset);
    else {
        read_actor_route (body, body_offset);
        if (kind == 5)
            (void) read_nonzero_u64 (
              body, body_offset, "binding generation");
    }
    require_end (body, body_offset, "send-ready destination");
}

std::uint8_t read_instance_route (std::span<const std::uint8_t> bytes,
                                  std::size_t &offset)
{
    if (offset >= bytes.size ())
        throw service_wire_error_t ("Instance route kind is truncated");
    const auto kind = bytes[offset++];
    if (kind < 1 || kind > 2)
        throw service_wire_error_t ("invalid Instance route kind");
    const auto body = read_body16 (bytes, offset, "Instance route");
    std::size_t body_offset = 0;
    (void) read_bytes8 (body, body_offset, "target node RID");
    (void) read_nonzero_u64 (body, body_offset,
                             "target node generation");
    (void) read_text8 (body, body_offset, "target Spot ID");
    if (kind == 1) {
        (void) read_nonzero_u64 (body, body_offset, "object generation");
        (void) read_text8 (body, body_offset, "owner ID");
        (void) read_nonzero_u64 (
          body, body_offset, "authority owner generation");
        (void) read_nonzero_u64 (body, body_offset, "lease generation");
        (void) read_text16 (body, body_offset, "StoreVersion");
    }
    else {
        (void) read_text8 (body, body_offset, "target Mesh name");
        (void) read_text8 (body, body_offset, "stable type");
        (void) read_text8 (body, body_offset, "descriptor version");
        (void) read_nonzero_u64 (body, body_offset, "deadline Unix ms");
    }
    require_end (body, body_offset, "Instance route");
    return kind;
}

struct frozen_body_validation_t
{
    std::optional<std::uint8_t> instance_operation_kind;
    std::optional<frozen_target_identity_t> target;
    std::optional<application_payload_t> application;
};

frozen_body_validation_t read_frozen_body (
  std::span<const std::uint8_t> bytes,
  std::size_t &offset,
  frozen_record_kind_t kind)
{
    frozen_body_validation_t validation;
    const auto wire_kind = static_cast<std::uint8_t> (kind);
    if (wire_kind == 1 || wire_kind == 2) {
        read_application_payload_envelope (bytes, offset);
    }
    else if (wire_kind == 3 || wire_kind == 4) {
        (void) read_text8 (bytes, offset, "channel name");
        read_application_payload_envelope (bytes, offset);
    }
    else if (wire_kind == 5 || wire_kind == 6) {
        validation.target = read_spot_route (bytes, offset);
        validation.application =
          read_application_payload_envelope (bytes, offset);
    }
    else if (wire_kind == 7) {
        (void) read_text8 (bytes, offset, "channel name");
        (void) read_text8 (bytes, offset, "topic");
        read_application_payload_envelope (bytes, offset);
    }
    else if (wire_kind == 8) {
        read_actor_control (bytes, offset);
    }
    else if (wire_kind == 9 || wire_kind == 10) {
        validation.target = read_actor_route (bytes, offset);
        validation.application =
          read_application_payload_envelope (bytes, offset);
    }
    else if (wire_kind == 11) {
        const auto terminal = read_u32 (bytes, offset);
        const auto failure = static_cast<framework_error_code> (
          read_u32 (bytes, offset));
        const auto has_payload = read_bool8 (
          bytes, offset, "completion payload presence");
        if (!valid_terminal_failure (terminal, failure)
            || (terminal != 0 && has_payload))
            throw service_wire_error_t (
              "completion terminal, failure, and payload do not match");
        if (has_payload)
            read_application_payload_envelope (bytes, offset);
    }
    else if (wire_kind == 12) {
        read_send_ready_destination (bytes, offset);
    }
    else if (wire_kind == 13) {
        if (offset >= bytes.size () || bytes[offset] > 9)
            throw service_wire_error_t ("invalid relocation control phase");
        ++offset;
        (void) read_role (bytes, offset);
        (void) read_relocation_id (bytes, offset);
        (void) read_object (bytes, offset);
        const auto terminal = read_u32 (bytes, offset);
        const auto failure = static_cast<framework_error_code> (
          read_u32 (bytes, offset));
        if (!valid_terminal_failure (terminal, failure))
            throw service_wire_error_t (
              "relocation control terminal and failure do not match");
    }
    else if (wire_kind == 14) {
        (void) read_instance_route (bytes, offset);
        (void) read_nonzero_u64 (
          bytes, offset, "Instance source node generation");
        if (offset >= bytes.size () || bytes[offset] < 1 || bytes[offset] > 2)
            throw service_wire_error_t (
              "invalid Instance activation operation kind");
        validation.instance_operation_kind = bytes[offset++];
        read_application_payload_envelope (bytes, offset);
    }
    return validation;
}

bool operation_is_zero (const wire_operation_id_t &value) noexcept
{
    return value.high == 0 && value.low == 0;
}

bool metadata_allowed (frozen_record_kind_t kind) noexcept
{
    const auto value = static_cast<std::uint8_t> (kind);
    return (value >= 1 && value <= 7) || value == 9 || value == 10
           || value == 14;
}

void validate_operation_matrix (const frozen_record_t &record,
                                const frozen_body_validation_t &body)
{
    const auto kind = static_cast<std::uint8_t> (record.kind);
    const auto zero = operation_is_zero (record.operation);
    const auto operation = record.operation_kind;
    bool valid = false;
    if (kind == 1 || kind == 3 || kind == 7 || kind == 12)
        valid = operation == 0 && zero;
    else if (kind == 2)
        valid = operation == 1 && !zero;
    else if (kind == 4)
        valid = operation == 2 && !zero;
    else if (kind == 5 || kind == 9)
        valid = operation == 0 && !zero;
    else if (kind == 6)
        valid = operation == 3 && !zero;
    else if (kind == 10)
        valid = operation == 4 && !zero;
    else if (kind == 8)
        valid = (operation == 0 && zero)
                || ((operation == 6 || operation == 7 || operation == 8)
                    && !zero);
    else if (kind == 11)
        valid = operation >= 1 && operation <= 15 && !zero;
    else if (kind == 13)
        valid = operation == 0 && zero;
    else if (kind == 14 && body.instance_operation_kind) {
        valid = *body.instance_operation_kind == 1
                  ? operation == 0 && zero
                  : operation == 12 && !zero;
    }
    if (!valid)
        throw service_wire_error_t (
          "frozen record kind, operation kind, and operation ID do not match");
    const auto requires_reply = operation == 1 || operation == 2
                                || operation == 3 || operation == 4
                                || operation == 12;
    if (requires_reply != record.reply_route_id.has_value ())
        throw service_wire_error_t (
          "frozen reply route does not match the operation kind");
}

void append_frozen_source (std::vector<std::uint8_t> &output,
                           const frozen_application_record_t &record)
{
    std::vector<std::uint8_t> body;
    append_bytes8 (body, record.source.node_routing_id, "source node RID");
    if (record.source.node_generation == 0
        || record.source.owner_id.empty ()
        || record.source.lease_generation == 0) {
        throw service_wire_error_t (
          "frozen source fence contains a zero required field");
    }
    append_u64 (body, record.source.node_generation);
    append_text8 (body, record.source.owner_id, "source owner ID");
    append_u64 (body, record.source.lease_generation);
    if (record.source_kind == frozen_source_kind_t::spot) {
        if (!record.source_spot_id)
            throw service_wire_error_t ("frozen Spot source is missing");
        append_text8 (body, *record.source_spot_id, "source Spot ID");
    }
    else if (record.source_kind == frozen_source_kind_t::actor
             || record.source_kind == frozen_source_kind_t::bound_session) {
        if (!record.source_actor || record.source_actor->second == 0)
            throw service_wire_error_t ("frozen Actor source is missing");
        append_text8 (body, record.source_actor->first, "source Actor ID");
        append_u64 (body, record.source_actor->second);
        if (record.source_kind == frozen_source_kind_t::bound_session) {
            if (!record.source_session_routing_id
                || record.source_binding_generation == 0
                || record.source_session_sequence == 0) {
                throw service_wire_error_t (
                  "frozen bound Session source is incomplete");
            }
            append_bytes8 (body, *record.source_session_routing_id,
                           "source Session RID");
            append_u64 (body, record.source_binding_generation);
            append_u64 (body, record.source_session_sequence);
        }
    }
    if (body.size () > std::numeric_limits<std::uint16_t>::max ())
        throw service_wire_error_t ("frozen source exceeds u16 bound");
    append_u16 (output, static_cast<std::uint16_t> (body.size ()));
    output.insert (output.end (), body.begin (), body.end ());
}

void append_frozen_metadata (
  std::vector<std::uint8_t> &output,
  const std::vector<frozen_metadata_entry_t> &metadata)
{
    if (metadata.empty ()) {
        output.push_back (0);
        return;
    }
    if (metadata.size () > std::numeric_limits<std::uint8_t>::max ())
        throw service_wire_error_t ("too many frozen metadata entries");
    output.push_back (1);
    const auto start = output.size ();
    output.push_back (1);
    output.push_back (static_cast<std::uint8_t> (metadata.size ()));
    std::vector<std::string> keys;
    for (const auto &entry : metadata) {
        if (std::find (keys.begin (), keys.end (), entry.key) != keys.end ())
            throw service_wire_error_t ("frozen metadata keys must be unique");
        keys.push_back (entry.key);
        append_text8 (output, entry.key, "metadata key");
        append_text16 (output, entry.value, "metadata value");
        if (output.size () - start > 1024)
            throw service_wire_error_t (
              "frozen metadata frame exceeds 1024 encoded bytes");
    }
}

void append_frozen_spot_route (std::vector<std::uint8_t> &output,
                               const frozen_spot_application_body_t &body)
{
    append_text8 (output, body.target.spot_id, "target Spot ID");
    if (body.target.object_generation == 0
        || body.target.target_node_generation == 0
        || body.target.authority_owner_generation == 0
        || body.expected_owner_lease_generation == 0) {
        throw service_wire_error_t (
          "frozen Spot route contains a zero required field");
    }
    append_u64 (output, body.target.object_generation);
    append_bytes8 (output, body.target.target_node_routing_id,
                   "target node RID");
    append_u64 (output, body.target.target_node_generation);
    append_u64 (output, body.target.authority_owner_generation);
    append_u64 (output, body.expected_owner_lease_generation);
}

void append_frozen_actor_route (std::vector<std::uint8_t> &output,
                                const frozen_actor_application_body_t &body)
{
    append_text8 (output, body.target.actor_id, "target Actor ID");
    if (body.target.object_generation == 0
        || body.target.target_node_generation == 0
        || body.target.authority_owner_generation == 0
        || body.target.owner_lease_generation == 0) {
        throw service_wire_error_t (
          "frozen Actor route contains a zero required field");
    }
    append_u64 (output, body.target.object_generation);
    append_bytes8 (output, body.target.target_node_routing_id,
                   "target node RID");
    append_u64 (output, body.target.target_node_generation);
    append_u64 (output, body.target.authority_owner_generation);
    append_u64 (output, body.target.owner_lease_generation);
}
}

frozen_record_t decode_frozen_record (
  std::span<const std::uint8_t> bytes)
{
    std::size_t offset = 0;
    if (offset >= bytes.size ())
        throw service_wire_error_t ("frozen record kind is truncated");
    frozen_record_t result;
    result.kind = static_cast<frozen_record_kind_t> (bytes[offset++]);
    if (static_cast<std::uint8_t> (result.kind) < 1
        || static_cast<std::uint8_t> (result.kind) > 14)
        throw service_wire_error_t ("invalid frozen record kind");
    if (offset >= bytes.size ())
        throw service_wire_error_t ("frozen source kind is truncated");
    result.source_kind = static_cast<frozen_source_kind_t> (bytes[offset++]);
    if (static_cast<std::uint8_t> (result.source_kind) < 1
        || static_cast<std::uint8_t> (result.source_kind) > 4)
        throw service_wire_error_t ("invalid frozen source kind");
    const auto source = read_body16 (bytes, offset, "frozen source");
    std::size_t source_offset = 0;
    result.source.node_routing_id = read_bytes8 (
      source, source_offset, "source node RID");
    result.source.node_generation = read_nonzero_u64 (
      source, source_offset, "source node generation");
    result.source.owner_id = read_text8 (
      source, source_offset, "source owner ID");
    result.source.lease_generation = read_nonzero_u64 (
      source, source_offset, "source owner lease generation");
    if (result.source_kind == frozen_source_kind_t::spot) {
        result.source_spot_id = read_text8 (
          source, source_offset, "source Spot ID");
    }
    else if (result.source_kind == frozen_source_kind_t::actor
             || result.source_kind == frozen_source_kind_t::bound_session) {
        auto actor_id = read_text8 (source, source_offset, "source Actor ID");
        const auto generation = read_nonzero_u64 (
          source, source_offset, "source Actor generation");
        result.source_actor = std::pair{std::move (actor_id), generation};
        if (result.source_kind == frozen_source_kind_t::bound_session) {
            result.source_session_routing_id = read_bytes8 (
              source, source_offset, "source Session RID");
            result.source_binding_generation = read_nonzero_u64 (
              source, source_offset, "source binding generation");
            result.source_session_sequence = read_nonzero_u64 (
              source, source_offset, "source Session sequence");
        }
    }
    require_end (source, source_offset, "frozen source");
    const auto infrastructure = result.kind == frozen_record_kind_t::spot_control
                                || result.kind == frozen_record_kind_t::send_ready
                                || result.kind
                                     == frozen_record_kind_t::relocation_control;
    if (infrastructure && result.source_kind != frozen_source_kind_t::node)
        throw service_wire_error_t (
          "infrastructure frozen record requires a node source");
    result.has_metadata = read_bool8 (bytes, offset, "metadata presence");
    if (result.has_metadata) {
        if (!metadata_allowed (result.kind))
            throw service_wire_error_t (
              "metadata is forbidden for this frozen record kind");
        read_metadata_frame (bytes, offset);
    }
    result.operation.high = read_u64 (bytes, offset);
    result.operation.low = read_u64 (bytes, offset);
    result.operation_kind = read_u32 (bytes, offset);
    if (result.operation_kind > 15)
        throw service_wire_error_t ("invalid frozen operation kind");
    const auto reply = read_body16 (bytes, offset, "frozen reply route");
    std::size_t reply_offset = 0;
    const auto requires_reply = result.operation_kind == 1
                                || result.operation_kind == 2
                                || result.operation_kind == 3
                                || result.operation_kind == 4
                                || result.operation_kind == 12;
    if (requires_reply)
        result.reply_route_id = read_nonzero_u64 (
          reply, reply_offset, "reply route ID");
    require_end (reply, reply_offset, "frozen reply route");
    const auto body = read_frozen_body (bytes, offset, result.kind);
    require_end (bytes, offset, "frozen record");
    validate_operation_matrix (result, body);
    result.target = body.target;
    result.application = body.application;
    result.canonical_bytes.assign (bytes.begin (), bytes.end ());
    return result;
}

std::vector<std::uint8_t> encode_frozen_record (
  const frozen_record_t &record)
{
    if (record.canonical_bytes.empty ())
        throw service_wire_error_t (
          "canonical frozen record bytes must not be empty");
    const auto decoded = decode_frozen_record (record.canonical_bytes);
    auto expected = record;
    expected.canonical_bytes = decoded.canonical_bytes;
    if (decoded != expected)
        throw service_wire_error_t (
          "frozen record summary does not match its canonical bytes");
    return record.canonical_bytes;
}

frozen_record_t encode_frozen_application_record (
  const frozen_application_record_t &record)
{
    const auto kind = static_cast<std::uint8_t> (record.kind);
    const auto spot = kind == 5 || kind == 6;
    const auto actor = kind == 9 || kind == 10;
    if ((!spot && !actor)
        || (spot
            != std::holds_alternative<frozen_spot_application_body_t> (
              record.body))) {
        throw service_wire_error_t (
          "typed frozen application body does not match its record kind");
    }
    if (!record.metadata.empty () && !metadata_allowed (record.kind))
        throw service_wire_error_t (
          "metadata is forbidden for this frozen record kind");

    std::vector<std::uint8_t> bytes{
      static_cast<std::uint8_t> (record.kind),
      static_cast<std::uint8_t> (record.source_kind)};
    append_frozen_source (bytes, record);
    append_frozen_metadata (bytes, record.metadata);
    append_u64 (bytes, record.operation.high);
    append_u64 (bytes, record.operation.low);
    append_u32 (bytes, record.operation_kind);
    if (record.reply_route_id) {
        if (*record.reply_route_id == 0)
            throw service_wire_error_t ("frozen reply route ID is zero");
        append_u16 (bytes, 8);
        append_u64 (bytes, *record.reply_route_id);
    }
    else {
        append_u16 (bytes, 0);
    }
    if (spot) {
        const auto &body = std::get<frozen_spot_application_body_t> (
          record.body);
        append_frozen_spot_route (bytes, body);
        const auto application = encode_application_payload (body.application);
        bytes.insert (bytes.end (), application.begin (), application.end ());
    }
    else {
        const auto &body = std::get<frozen_actor_application_body_t> (
          record.body);
        append_frozen_actor_route (bytes, body);
        const auto application = encode_application_payload (body.application);
        bytes.insert (bytes.end (), application.begin (), application.end ());
    }
    return decode_frozen_record (bytes);
}

std::vector<std::uint8_t> encode_instance_spot_activation_header (
  const instance_spot_activation_header_t &record)
{
    if (record.target.target_node_generation == 0
        || record.target.deadline_unix_ms == 0
        || record.target.deadline_unix_ms
             > static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ())
        || record.source_node_generation == 0
        || (record.operation.high == 0 && record.operation.low == 0)
        || record.request != (record.reply_route_id != 0)) {
        throw service_wire_error_t (
          "Instance Spot activation contains a zero or inconsistent required field");
    }
    std::vector<std::uint8_t> route;
    append_bytes8 (route, record.target.target_node_routing_id,
                   "target node RID");
    append_u64 (route, record.target.target_node_generation);
    append_text8 (route, record.target.spot_id, "target SpotId");
    append_text8 (route, record.target.mesh_name, "target MeshName");
    append_text8 (route, record.target.stable_type, "stable type");
    append_text8 (route, record.target.descriptor_version,
                  "target descriptor version");
    append_u64 (route, record.target.deadline_unix_ms);
    if (route.size () > std::numeric_limits<std::uint16_t>::max ()) {
        throw service_wire_error_t (
          "Instance Spot activation route exceeds u16 bound");
    }

    std::vector<std::uint8_t> bytes{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::instanceSpot),
      static_cast<std::uint8_t> (
        record.has_metadata ? static_cast<std::uint8_t> (flag::metadata) : 0)};
    bytes.push_back (2);
    append_u16 (bytes, static_cast<std::uint16_t> (route.size ()));
    bytes.insert (bytes.end (), route.begin (), route.end ());
    append_u64 (bytes, record.source_node_generation);
    append_bytes8 (bytes, record.source_node_routing_id,
                   "source node RID");
    if (record.source_spot_id) {
        append_text8 (bytes, *record.source_spot_id, "source SpotId");
    }
    else {
        bytes.push_back (0);
    }
    bytes.push_back (record.request ? 2 : 1);
    append_u64 (bytes, record.operation.high);
    append_u64 (bytes, record.operation.low);
    if (record.request) {
        append_u64 (bytes, record.reply_route_id);
    }
    return bytes;
}

instance_spot_activation_header_t decode_instance_spot_activation_header (
  std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    const auto metadata_flag = static_cast<std::uint8_t> (flag::metadata);
    if (header.kind != command::instanceSpot
        || (header.flags & ~metadata_flag) != 0) {
        throw service_wire_error_t (
          "record is not an Instance Spot activation command");
    }
    std::size_t offset = prefix_size;
    if (offset >= bytes.size () || bytes[offset++] != 2) {
        throw service_wire_error_t (
          "Instance Spot activation route version is invalid");
    }
    const auto route_length = read_u16 (bytes, offset);
    if (route_length == 0 || bytes.size () - offset < route_length) {
        throw service_wire_error_t (
          "Instance Spot activation route is truncated");
    }
    const auto route = bytes.subspan (offset, route_length);
    offset += route_length;
    std::size_t route_offset = 0;
    instance_spot_activation_header_t record;
    record.target.target_node_routing_id =
      read_bytes8 (route, route_offset, "target node RID");
    record.target.target_node_generation = read_u64 (route, route_offset);
    record.target.spot_id = read_text8 (route, route_offset, "target SpotId");
    record.target.mesh_name = read_text8 (route, route_offset, "target MeshName");
    record.target.stable_type = read_text8 (route, route_offset, "stable type");
    record.target.descriptor_version =
      read_text8 (route, route_offset, "target descriptor version");
    record.target.deadline_unix_ms = read_u64 (route, route_offset);
    if (route_offset != route.size ()) {
        throw service_wire_error_t (
          "Instance Spot activation route has trailing bytes");
    }
    record.source_node_generation = read_u64 (bytes, offset);
    record.source_node_routing_id =
      read_bytes8 (bytes, offset, "source node RID");
    if (offset >= bytes.size ()) {
        throw service_wire_error_t (
          "Instance Spot activation source SpotId is truncated");
    }
    if (bytes[offset] == 0) {
        ++offset;
    }
    else {
        record.source_spot_id = read_text8 (bytes, offset, "source SpotId");
    }
    if (offset >= bytes.size ()) {
        throw service_wire_error_t (
          "Instance Spot activation operation kind is truncated");
    }
    const auto operation_kind = bytes[offset++];
    if (operation_kind != 1 && operation_kind != 2) {
        throw service_wire_error_t (
          "Instance Spot activation operation kind is invalid");
    }
    record.request = operation_kind == 2;
    record.operation.high = read_u64 (bytes, offset);
    record.operation.low = read_u64 (bytes, offset);
    if (record.request) {
        record.reply_route_id = read_u64 (bytes, offset);
    }
    record.has_metadata = (header.flags & metadata_flag) != 0;
    if (offset != bytes.size ()
        || record.target.target_node_generation == 0
        || record.target.deadline_unix_ms == 0
        || record.target.deadline_unix_ms
             > static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ())
        || record.source_node_generation == 0
        || (record.operation.high == 0 && record.operation.low == 0)
        || record.request != (record.reply_route_id != 0)) {
        throw service_wire_error_t (
          "Instance Spot activation contains invalid or trailing fields");
    }
    return record;
}

std::vector<std::uint8_t> encode_instance_activation_recovery (
  const instance_activation_recovery_t &record)
{
    const auto &activation = record.activation;
    if (activation.has_metadata != record.metadata.has_value ()) {
        throw service_wire_error_t (
          "Instance activation recovery metadata presence is inconsistent");
    }
    /* Reuse command-39 validation without persisting its transport prefix. */
    (void) encode_instance_spot_activation_header (activation);
    std::vector<std::uint8_t> body;
    append_text8 (body, activation.target.spot_id, "target SpotId");
    append_text8 (body, activation.target.stable_type, "stable type");
    append_text8 (body, activation.target.mesh_name, "target MeshName");
    append_bytes8 (body, activation.target.target_node_routing_id,
                   "target node RID");
    append_u64 (body, activation.target.target_node_generation);
    append_text8 (body, activation.target.descriptor_version,
                  "target descriptor version");
    append_bytes8 (body, activation.source_node_routing_id,
                   "source node RID");
    append_u64 (body, activation.source_node_generation);
    body.push_back (activation.source_spot_id ? 1 : 0);
    if (activation.source_spot_id)
        append_text8 (body, *activation.source_spot_id, "source SpotId");
    body.push_back (activation.request ? 2 : 1);
    append_u64 (body, activation.operation.high);
    append_u64 (body, activation.operation.low);
    if (activation.request)
        append_u64 (body, activation.reply_route_id);
    append_u64 (body, activation.target.deadline_unix_ms);
    body.push_back (activation.has_metadata ? 1 : 0);
    if (record.metadata) {
        if (record.metadata->empty ())
            throw service_wire_error_t (
              "Instance activation recovery metadata frame is empty");
        body.insert (body.end (), record.metadata->begin (),
                     record.metadata->end ());
    }
    const auto application =
      encode_application_payload (record.application_payload);
    body.insert (body.end (), application.begin (), application.end ());
    if (body.size () > std::numeric_limits<std::uint32_t>::max ()) {
        throw service_wire_error_t (
          "Instance activation recovery exceeds u32 body bound");
    }
    std::vector<std::uint8_t> result{'Z', 'L', 'I', 'A', 1};
    append_u16 (result, 0);
    append_u32 (result, static_cast<std::uint32_t> (body.size ()));
    result.insert (result.end (), body.begin (), body.end ());
    append_u32 (result, crc32c (result));
    return result;
}

instance_activation_recovery_t decode_instance_activation_recovery (
  std::span<const std::uint8_t> bytes)
{
    if (bytes.size () < 15 || bytes[0] != 'Z' || bytes[1] != 'L'
        || bytes[2] != 'I' || bytes[3] != 'A' || bytes[4] != 1) {
        throw service_wire_error_t (
          "Instance activation recovery prefix is invalid");
    }
    std::size_t offset = 5;
    if (read_u16 (bytes, offset) != 0) {
        throw service_wire_error_t (
          "Instance activation recovery flags are invalid");
    }
    const auto body_length = read_u32 (bytes, offset);
    if (body_length != bytes.size () - offset - 4) {
        throw service_wire_error_t (
          "Instance activation recovery body length does not match frame");
    }
    std::size_t checksum_offset = bytes.size () - 4;
    auto checksum_read_offset = checksum_offset;
    const auto expected_checksum = read_u32 (bytes, checksum_read_offset);
    if (expected_checksum != crc32c (bytes.first (checksum_offset))) {
        throw service_wire_error_t (
          "Instance activation recovery checksum mismatch");
    }
    const auto body_end = checksum_offset;
    instance_activation_recovery_t record;
    auto &activation = record.activation;
    activation.target.spot_id = read_text8 (bytes, offset, "target SpotId");
    activation.target.stable_type = read_text8 (bytes, offset, "stable type");
    activation.target.mesh_name = read_text8 (bytes, offset, "target MeshName");
    activation.target.target_node_routing_id =
      read_bytes8 (bytes, offset, "target node RID");
    activation.target.target_node_generation = read_u64 (bytes, offset);
    activation.target.descriptor_version =
      read_text8 (bytes, offset, "target descriptor version");
    activation.source_node_routing_id =
      read_bytes8 (bytes, offset, "source node RID");
    activation.source_node_generation = read_u64 (bytes, offset);
    if (offset >= body_end)
        throw service_wire_error_t (
          "Instance activation recovery source SpotId is truncated");
    const auto has_source_spot = bytes[offset++];
    if (has_source_spot == 1)
        activation.source_spot_id =
          read_text8 (bytes, offset, "source SpotId");
    else if (has_source_spot != 0)
        throw service_wire_error_t (
          "Instance activation recovery source SpotId flag is invalid");
    if (offset >= body_end || (bytes[offset] != 1 && bytes[offset] != 2))
        throw service_wire_error_t (
          "Instance activation recovery operation kind is invalid");
    activation.request = bytes[offset++] == 2;
    activation.operation.high = read_u64 (bytes, offset);
    activation.operation.low = read_u64 (bytes, offset);
    if (activation.request)
        activation.reply_route_id = read_u64 (bytes, offset);
    activation.target.deadline_unix_ms = read_u64 (bytes, offset);
    if (offset >= body_end || bytes[offset] > 1)
        throw service_wire_error_t (
          "Instance activation recovery metadata flag is invalid");
    activation.has_metadata = bytes[offset++] == 1;
    if (activation.has_metadata) {
        /* metadata-frame v1: version u8, entry count u16, then
         * text8 key and u16-length value for each entry. Keep the exact bytes. */
        const auto metadata_start = offset;
        if (offset >= body_end || bytes[offset++] != 1)
            throw service_wire_error_t (
              "Instance activation recovery metadata version is invalid");
        if (offset >= body_end)
            throw service_wire_error_t (
              "Instance activation recovery metadata count is truncated");
        const auto count = bytes[offset++];
        for (std::uint16_t index = 0; index < count; ++index) {
            (void) read_text8 (bytes, offset, "metadata key");
            const auto value_length = read_u16 (bytes, offset);
            if (body_end - offset < value_length)
                throw service_wire_error_t (
                  "Instance activation recovery metadata value is truncated");
            offset += value_length;
        }
        record.metadata = std::vector<std::uint8_t> (
          bytes.begin () + static_cast<std::ptrdiff_t> (metadata_start),
          bytes.begin () + static_cast<std::ptrdiff_t> (offset));
    }
    if (offset >= body_end)
        throw service_wire_error_t (
          "Instance activation recovery application payload is missing");
    record.application_payload = decode_application_payload (
      bytes.subspan (offset, body_end - offset));
    activation.has_metadata = record.metadata.has_value ();
    (void) encode_instance_spot_activation_header (activation);
    return record;
}

std::vector<std::uint8_t> encode_user_spot_create_header (
  const user_spot_create_header_t &record)
{
    if (record.correlation == 0
        || (record.operation.high == 0 && record.operation.low == 0)
        || record.source_node_generation == 0
        || record.reservation.object_generation == 0
        || record.reservation.authority_owner_generation == 0
        || record.reservation.target_node_generation == 0
        || record.reservation.target_owner_lease_generation == 0
        || record.reservation.pending_capacity_delta == 0
        || record.deadline_unix_ms == 0) {
        throw service_wire_error_t (
          "user Spot create contains a zero required fence");
    }
    std::vector<std::uint8_t> bytes{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::userSpotCreate), 0};
    append_u64 (bytes, record.correlation);
    append_u64 (bytes, record.operation.high);
    append_u64 (bytes, record.operation.low);
    append_bytes8 (
      bytes, record.source_node_routing_id, "source node RID");
    append_u64 (bytes, record.source_node_generation);
    append_text8 (bytes, record.spot_id, "spot ID");
    append_text8 (bytes, record.stable_type, "stable type");
    append_text8 (
      bytes, record.reservation.reservation_id, "reservation ID");
    append_text16 (
      bytes, record.reservation.expected_store_version,
      "expected StoreVersion");
    append_u64 (bytes, record.reservation.object_generation);
    append_u64 (
      bytes, record.reservation.authority_owner_generation);
    append_bytes8 (
      bytes, record.reservation.target_node_routing_id,
      "target node RID");
    append_u64 (bytes, record.reservation.target_node_generation);
    append_text8 (
      bytes, record.reservation.target_owner_id, "target owner ID");
    append_u64 (
      bytes, record.reservation.target_owner_lease_generation);
    append_u32 (bytes, record.reservation.pending_capacity_delta);
    append_u64 (bytes, record.deadline_unix_ms);
    return bytes;
}

user_spot_create_header_t decode_user_spot_create_header (
  std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::userSpotCreate || header.flags != 0) {
        throw service_wire_error_t (
          "record is not a User Spot create command");
    }
    std::size_t offset = prefix_size;
    user_spot_create_header_t record;
    record.correlation = read_u64 (bytes, offset);
    record.operation.high = read_u64 (bytes, offset);
    record.operation.low = read_u64 (bytes, offset);
    record.source_node_routing_id =
      read_bytes8 (bytes, offset, "source node RID");
    record.source_node_generation = read_u64 (bytes, offset);
    record.spot_id = read_text8 (bytes, offset, "spot ID");
    record.stable_type = read_text8 (bytes, offset, "stable type");
    record.reservation.reservation_id =
      read_text8 (bytes, offset, "reservation ID");
    record.reservation.expected_store_version =
      read_text16 (bytes, offset, "expected StoreVersion");
    record.reservation.object_generation = read_u64 (bytes, offset);
    record.reservation.authority_owner_generation =
      read_u64 (bytes, offset);
    record.reservation.target_node_routing_id =
      read_bytes8 (bytes, offset, "target node RID");
    record.reservation.target_node_generation =
      read_u64 (bytes, offset);
    record.reservation.target_owner_id =
      read_text8 (bytes, offset, "target owner ID");
    record.reservation.target_owner_lease_generation =
      read_u64 (bytes, offset);
    record.reservation.pending_capacity_delta =
      read_u32 (bytes, offset);
    record.deadline_unix_ms = read_u64 (bytes, offset);
    if (offset != bytes.size ()) {
        throw service_wire_error_t (
          "User Spot create command has trailing bytes");
    }
    if (record.correlation == 0
        || (record.operation.high == 0 && record.operation.low == 0)
        || record.source_node_generation == 0
        || record.reservation.object_generation == 0
        || record.reservation.authority_owner_generation == 0
        || record.reservation.target_node_generation == 0
        || record.reservation.target_owner_lease_generation == 0
        || record.reservation.pending_capacity_delta == 0
        || record.deadline_unix_ms == 0) {
        throw service_wire_error_t (
          "user Spot create contains a zero required fence");
    }
    return record;
}

std::vector<std::uint8_t> encode_actor_create_header (
  const actor_create_header_t &record)
{
    if (record.correlation == 0
        || (record.operation.high == 0 && record.operation.low == 0)
        || record.source_node_generation == 0
        || record.reservation.object_generation == 0
        || record.reservation.authority_owner_generation == 0
        || record.reservation.target_node_generation == 0
        || record.reservation.target_owner_lease_generation == 0
        || record.reservation.pending_capacity_delta == 0
        || record.deadline_unix_ms == 0)
        throw service_wire_error_t (
          "Actor create contains a zero required fence");
    std::vector<std::uint8_t> bytes{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::actorCreate), 0};
    append_u64 (bytes, record.correlation);
    append_u64 (bytes, record.operation.high);
    append_u64 (bytes, record.operation.low);
    append_bytes8 (bytes, record.source_node_routing_id,
                   "source node RID");
    append_u64 (bytes, record.source_node_generation);
    append_text8 (bytes, record.actor_id, "actor ID");
    append_text8 (bytes, record.stable_type, "stable type");
    append_text8 (bytes, record.reservation.reservation_id,
                  "reservation ID");
    append_text16 (bytes, record.reservation.expected_store_version,
                   "expected StoreVersion");
    append_u64 (bytes, record.reservation.object_generation);
    append_u64 (bytes, record.reservation.authority_owner_generation);
    append_bytes8 (bytes, record.reservation.target_node_routing_id,
                   "target node RID");
    append_u64 (bytes, record.reservation.target_node_generation);
    append_text8 (bytes, record.reservation.target_owner_id,
                  "target owner ID");
    append_u64 (bytes,
                record.reservation.target_owner_lease_generation);
    append_u32 (bytes, record.reservation.pending_capacity_delta);
    append_u64 (bytes, record.deadline_unix_ms);
    return bytes;
}

actor_create_header_t decode_actor_create_header (
  std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::actorCreate || header.flags != 0)
        throw service_wire_error_t (
          "record is not an Actor create command");
    std::size_t offset = prefix_size;
    actor_create_header_t record;
    record.correlation = read_u64 (bytes, offset);
    record.operation.high = read_u64 (bytes, offset);
    record.operation.low = read_u64 (bytes, offset);
    record.source_node_routing_id =
      read_bytes8 (bytes, offset, "source node RID");
    record.source_node_generation = read_u64 (bytes, offset);
    record.actor_id = read_text8 (bytes, offset, "actor ID");
    record.stable_type = read_text8 (bytes, offset, "stable type");
    record.reservation.reservation_id =
      read_text8 (bytes, offset, "reservation ID");
    record.reservation.expected_store_version =
      read_text16 (bytes, offset, "expected StoreVersion");
    record.reservation.object_generation = read_u64 (bytes, offset);
    record.reservation.authority_owner_generation =
      read_u64 (bytes, offset);
    record.reservation.target_node_routing_id =
      read_bytes8 (bytes, offset, "target node RID");
    record.reservation.target_node_generation =
      read_u64 (bytes, offset);
    record.reservation.target_owner_id =
      read_text8 (bytes, offset, "target owner ID");
    record.reservation.target_owner_lease_generation =
      read_u64 (bytes, offset);
    record.reservation.pending_capacity_delta =
      read_u32 (bytes, offset);
    record.deadline_unix_ms = read_u64 (bytes, offset);
    if (offset != bytes.size ())
        throw service_wire_error_t (
          "Actor create command has trailing bytes");
    (void) encode_actor_create_header (record);
    return record;
}

std::vector<std::uint8_t> encode_user_spot_close_header (
  const user_spot_close_header_t &record)
{
    if (record.correlation == 0
        || (record.operation.high == 0 && record.operation.low == 0)
        || record.source_node_generation == 0
        || record.target.object_generation == 0
        || record.target.target_node_generation == 0
        || record.target.authority_owner_generation == 0
        || record.deadline_unix_ms == 0) {
        throw service_wire_error_t (
          "user Spot close contains a zero required fence");
    }
    std::vector<std::uint8_t> bytes{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::userSpotClose), 0};
    append_u64 (bytes, record.correlation);
    append_u64 (bytes, record.operation.high);
    append_u64 (bytes, record.operation.low);
    append_bytes8 (
      bytes, record.source_node_routing_id, "source node RID");
    append_u64 (bytes, record.source_node_generation);
    std::vector<std::uint8_t> fence;
    append_text8 (fence, record.target.spot_id, "spot ID");
    append_u64 (fence, record.target.object_generation);
    append_bytes8 (
      fence, record.target.target_node_routing_id, "target node RID");
    append_u64 (fence, record.target.target_node_generation);
    append_u64 (fence, record.target.authority_owner_generation);
    append_text16 (
      fence, record.target.expected_store_version,
      "expected StoreVersion");
    if (fence.size () > std::numeric_limits<std::uint16_t>::max ()) {
        throw service_wire_error_t ("User Spot close fence is too large");
    }
    bytes.push_back (1);
    append_u16 (bytes, static_cast<std::uint16_t> (fence.size ()));
    bytes.insert (bytes.end (), fence.begin (), fence.end ());
    append_u64 (bytes, record.deadline_unix_ms);
    return bytes;
}

user_spot_close_header_t decode_user_spot_close_header (
  std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::userSpotClose || header.flags != 0) {
        throw service_wire_error_t (
          "record is not a User Spot close command");
    }
    std::size_t offset = prefix_size;
    user_spot_close_header_t record;
    record.correlation = read_u64 (bytes, offset);
    record.operation.high = read_u64 (bytes, offset);
    record.operation.low = read_u64 (bytes, offset);
    record.source_node_routing_id =
      read_bytes8 (bytes, offset, "source node RID");
    record.source_node_generation = read_u64 (bytes, offset);
    if (offset >= bytes.size () || bytes[offset++] != 1) {
        throw service_wire_error_t (
          "unsupported User Spot close fence version");
    }
    const auto fence_size = read_u16 (bytes, offset);
    if (bytes.size () - offset < fence_size) {
        throw service_wire_error_t (
          "truncated User Spot close fence");
    }
    const auto fence_end = offset + fence_size;
    const auto fence_bytes = bytes.first (fence_end);
    record.target.spot_id =
      read_text8 (fence_bytes, offset, "spot ID");
    record.target.object_generation = read_u64 (fence_bytes, offset);
    record.target.target_node_routing_id =
      read_bytes8 (fence_bytes, offset, "target node RID");
    record.target.target_node_generation =
      read_u64 (fence_bytes, offset);
    record.target.authority_owner_generation =
      read_u64 (fence_bytes, offset);
    record.target.expected_store_version =
      read_text16 (fence_bytes, offset, "expected StoreVersion");
    if (offset != fence_end) {
        throw service_wire_error_t (
          "User Spot close fence has trailing bytes");
    }
    record.deadline_unix_ms = read_u64 (bytes, offset);
    if (offset != bytes.size ()) {
        throw service_wire_error_t (
          "User Spot close command has trailing bytes");
    }
    if (record.correlation == 0
        || (record.operation.high == 0 && record.operation.low == 0)
        || record.source_node_generation == 0
        || record.target.object_generation == 0
        || record.target.target_node_generation == 0
        || record.target.authority_owner_generation == 0
        || record.deadline_unix_ms == 0) {
        throw service_wire_error_t (
          "user Spot close contains a zero required fence");
    }
    return record;
}

service_wire_header_t decode_header (std::span<const std::uint8_t> bytes)
{
    if (bytes.size () < prefix_size) {
        throw service_wire_error_t ("truncated service wire prefix");
    }
    if (bytes[0] != magic[0] || bytes[1] != magic[1]) {
        throw service_wire_error_t ("invalid service wire magic");
    }
    if (bytes[2] != wire_major) {
        throw service_wire_error_t ("unsupported service wire major");
    }
    return service_wire_header_t{
      static_cast<command> (bytes[3]), bytes[4]};
}

std::string
decode_channel_send_header (std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::channelSend || header.flags != 0) {
        throw service_wire_error_t ("frame is not a channelSend header");
    }
    std::size_t offset = prefix_size;
    auto channel_name = read_text8 (bytes, offset, "channel name");
    if (offset != bytes.size ()) {
        throw service_wire_error_t ("channelSend header has trailing bytes");
    }
    return channel_name;
}

std::vector<std::uint8_t>
encode_application_payload (const application_payload_t &payload)
{
    if (payload.flow_id.has_value () != payload.flow_origin.has_value ()) {
        throw service_wire_error_t (
          "application payload flow id and origin must be present together");
    }
    if (payload.flow_id && !valid_flow_id (*payload.flow_id)) {
        throw service_wire_error_t ("application payload flow id is invalid");
    }
    if (payload.payload.size ()
        > std::numeric_limits<std::uint32_t>::max ()) {
        throw service_wire_error_t ("application payload exceeds u32");
    }
    std::vector<std::uint8_t> body;
    append_text8 (body, payload.packet_name, "packet name");
    append_text8 (body, payload.content_type, "content type");
    append_u32 (body, static_cast<std::uint32_t> (payload.payload.size ()));
    body.insert (body.end (), payload.payload.begin (), payload.payload.end ());
    if (payload.flow_id) {
        append_text8 (body, *payload.flow_id, "flow id");
        body.push_back (static_cast<std::uint8_t> (*payload.flow_origin));
    }
    if (body.size () > std::numeric_limits<std::uint32_t>::max ()) {
        throw service_wire_error_t ("application payload envelope exceeds u32");
    }
    std::vector<std::uint8_t> result;
    result.reserve (5 + body.size ());
    result.push_back (payload.flow_id ? application_payload_flow_version
                                      : application_payload_version);
    append_u32 (result, static_cast<std::uint32_t> (body.size ()));
    result.insert (result.end (), body.begin (), body.end ());
    return result;
}

application_payload_t
decode_application_payload (std::span<const std::uint8_t> bytes)
{
    if (bytes.size () < 5
        || (bytes[0] != application_payload_version
            && bytes[0] != application_payload_flow_version)) {
        throw service_wire_error_t ("invalid application payload version");
    }
    const auto has_flow = bytes[0] == application_payload_flow_version;
    std::size_t offset = 1;
    const auto body_length = read_u32 (bytes, offset);
    if (body_length != bytes.size () - offset) {
        throw service_wire_error_t (
          "application payload body length does not match frame");
    }
    application_payload_t result;
    result.packet_name = read_text8 (bytes, offset, "packet name");
    result.content_type = read_text8 (bytes, offset, "content type");
    const auto payload_length = read_u32 (bytes, offset);
    if (payload_length > bytes.size () - offset) {
        throw service_wire_error_t (
          "application payload length does not match frame");
    }
    result.payload.assign (bytes.begin () + static_cast<std::ptrdiff_t> (offset),
                           bytes.begin () + static_cast<std::ptrdiff_t> (offset + payload_length));
    offset += payload_length;
    if (has_flow) {
        result.flow_id = read_text8 (bytes, offset, "flow id");
        if (!valid_flow_id (*result.flow_id) || offset >= bytes.size ()
            || !valid_flow_origin (bytes[offset])) {
            throw service_wire_error_t ("application payload flow context is invalid");
        }
        result.flow_origin = static_cast<flow_origin_t> (bytes[offset++]);
    }
    if (offset != bytes.size ()) {
        throw service_wire_error_t (
          "application payload has trailing fields");
    }
    return result;
}

std::vector<std::uint8_t> encode_route_mesh_admission (
  command kind,
  const mesh::service_node_descriptor_t &descriptor)
{
    validate_admission_kind (kind);
    try {
        static_cast<void> (
          mesh::service_topology_registry_t (descriptor));
    }
    catch (const std::invalid_argument &error) {
        throw service_wire_error_t (error.what ());
    }

    std::vector<std::uint8_t> route;
    append_text8 (route, descriptor.mesh_name, "mesh name");
    append_text8 (route, descriptor.security_identity, "security identity");
    append_u32 (route, descriptor.effective_max_message_bytes);
    append_u64 (route, descriptor.lifecycle_generation);
    append_u64 (route, descriptor.descriptor_revision);
    append_text16 (route, descriptor.advertised_endpoint, "advertised endpoint");
    if (descriptor.channels.size ()
        > std::numeric_limits<std::uint16_t>::max ()) {
        throw service_wire_error_t ("channel vector exceeds u16");
    }
    append_u16 (route, static_cast<std::uint16_t> (descriptor.channels.size ()));
    for (const auto &channel : descriptor.channels) {
        append_text8 (route, channel.name, "channel name");
        append_u32 (
          route,
          static_cast<std::uint32_t> (channel.weight));
    }

    std::vector<std::uint8_t> extension;
    append_tlv (
      extension, 1,
      {runtime_state_wire (descriptor.state)});
    std::vector<std::uint8_t> application_version;
    append_u64 (
      application_version,
      static_cast<std::uint64_t> (descriptor.application_version));
    append_tlv (extension, 2, application_version);

    if (descriptor.protocol_capabilities.size ()
        > std::numeric_limits<std::uint16_t>::max ()) {
        throw service_wire_error_t ("protocol capability vector exceeds u16");
    }
    std::vector<std::uint8_t> capabilities;
    append_u16 (
      capabilities,
      static_cast<std::uint16_t> (descriptor.protocol_capabilities.size ()));
    for (const auto &capability : descriptor.protocol_capabilities) {
        append_text8 (capabilities, capability, "protocol capability");
    }
    append_tlv (extension, 6, capabilities);
    append_tlv (
      extension, 7,
      {object_role_wire (descriptor.object_role)});
    for (const auto &[id, value] :
         std::array<std::pair<std::uint8_t, std::uint32_t>, 5>{
           std::pair<std::uint8_t, std::uint32_t>{
             8, static_cast<std::uint32_t> (
                  descriptor.placement_weight)},
           {9, descriptor.active_capacity_limit},
           {10, descriptor.pending_capacity_limit},
           {11, descriptor.active_capacity_used},
           {12, descriptor.pending_capacity_used}}) {
        std::vector<std::uint8_t> encoded;
        append_u32 (encoded, value);
        append_tlv (extension, id, encoded);
    }
    append_u32 (route, static_cast<std::uint32_t> (extension.size ()));
    route.insert (route.end (), extension.begin (), extension.end ());

    std::vector<std::uint8_t> result{
      magic[0], magic[1], wire_major, static_cast<std::uint8_t> (kind), 0,
      1};
    append_u32 (result, static_cast<std::uint32_t> (route.size ()));
    result.insert (result.end (), route.begin (), route.end ());
    return result;
}

mesh::service_node_descriptor_t decode_route_mesh_admission (
  std::span<const std::uint8_t> bytes,
  command expected_kind,
  std::vector<std::uint8_t> source_routing_id)
{
    validate_admission_kind (expected_kind);
    const auto header = decode_header (bytes);
    if (header.kind != expected_kind || header.flags != 0) {
        throw service_wire_error_t ("unexpected admission command or flags");
    }
    std::size_t offset = prefix_size;
    if (offset >= bytes.size () || bytes[offset++] != 1) {
        throw service_wire_error_t ("admission is not RouteMesh topology");
    }
    const auto route_length = read_u32 (bytes, offset);
    if (route_length != bytes.size () - offset) {
        throw service_wire_error_t ("RouteMesh admission length mismatch");
    }

    mesh::service_node_descriptor_t result;
    result.node_routing_id = std::move (source_routing_id);
    result.mesh_name = read_text8 (bytes, offset, "mesh name");
    result.security_identity =
      read_text8 (bytes, offset, "security identity");
    result.effective_max_message_bytes = read_u32 (bytes, offset);
    result.lifecycle_generation = read_u64 (bytes, offset);
    result.descriptor_revision = read_u64 (bytes, offset);
    result.advertised_endpoint =
      read_text16 (bytes, offset, "advertised endpoint");
    const auto channel_count = read_u16 (bytes, offset);
    result.channels.reserve (channel_count);
    for (std::uint16_t index = 0; index < channel_count; ++index) {
        auto name =
          read_text8 (bytes, offset, "channel name");
        const auto weight = read_u32 (bytes, offset);
        if (weight > 10000)
            throw service_wire_error_t (
              "channel weight is outside 0..10000");
        result.channels.push_back (
          {std::move (name), static_cast<int> (weight)});
    }

    const auto extension_length = read_u32 (bytes, offset);
    if (extension_length != bytes.size () - offset) {
        throw service_wire_error_t ("descriptor extension length mismatch");
    }
    const auto extension_end = offset + extension_length;
    std::uint16_t required = 0;
    std::uint8_t previous_id = 0;
    while (offset < extension_end) {
        const auto id = bytes[offset++];
        const auto length = read_u32 (bytes, offset);
        if (id <= previous_id || length > extension_end - offset) {
            throw service_wire_error_t (
              "descriptor TLV order or length is invalid");
        }
        previous_id = id;
        const auto value = bytes.subspan (offset, length);
        offset += length;
        std::size_t value_offset = 0;
        switch (id) {
            case 1:
                if (value.size () != 1) {
                    throw service_wire_error_t ("runtime state TLV length");
                }
                result.state = runtime_state_from_wire (value[0]);
                required |= 1u << 0u;
                break;
            case 2:
                if (value.size () != 8) {
                    throw service_wire_error_t (
                      "application version TLV length");
                }
                result.application_version =
                  static_cast<std::int64_t> (read_u64 (value, value_offset));
                if (result.application_version < 0) {
                    throw service_wire_error_t ("negative application version");
                }
                required |= 1u << 1u;
                break;
            case 6: {
                const auto count = read_u16 (value, value_offset);
                result.protocol_capabilities.clear ();
                result.protocol_capabilities.reserve (count);
                for (std::uint16_t index = 0; index < count; ++index) {
                    result.protocol_capabilities.push_back (
                      read_text8 (value, value_offset, "protocol capability"));
                }
                if (value_offset != value.size ()) {
                    throw service_wire_error_t (
                      "protocol capability TLV trailing bytes");
                }
                required |= 1u << 2u;
                break;
            }
            case 7:
                if (value.size () != 1) {
                    throw service_wire_error_t ("object role TLV length");
                }
                result.object_role = object_role_from_wire (value[0]);
                required |= 1u << 3u;
                break;
            case 8:
                result.placement_weight = static_cast<int> (
                  read_u32 (value, value_offset));
                required |= 1u << 4u;
                break;
            case 9:
                result.active_capacity_limit =
                  read_u32 (value, value_offset);
                required |= 1u << 5u;
                break;
            case 10:
                result.pending_capacity_limit =
                  read_u32 (value, value_offset);
                required |= 1u << 6u;
                break;
            case 11:
                result.active_capacity_used =
                  read_u32 (value, value_offset);
                required |= 1u << 7u;
                break;
            case 12:
                result.pending_capacity_used =
                  read_u32 (value, value_offset);
                required |= 1u << 8u;
                break;
            default:
                break;
        }
        if (id >= 8 && id <= 12 && value_offset != value.size ()) {
            throw service_wire_error_t ("u32 descriptor TLV length");
        }
    }
    if (required != 0x1ffu) {
        throw service_wire_error_t (
          "descriptor extension omits a required field");
    }
    try {
        static_cast<void> (mesh::service_topology_registry_t (result));
    }
    catch (const std::invalid_argument &error) {
        throw service_wire_error_t (error.what ());
    }
    return result;
}

std::vector<std::uint8_t> encode_client_server_client_admission (
  command kind,
  const client_server_client_admission_t &admission)
{
    validate_admission_kind (kind);
    if (admission.effective_max_message_bytes == 0) {
        throw service_wire_error_t (
          "client effective max message bytes must be nonzero");
    }
    std::vector<std::uint8_t> body;
    append_text8 (body, admission.channel_name, "channel name");
    body.push_back (1);
    append_text8 (
      body, admission.security_identity, "security identity");
    append_u32 (body, admission.effective_max_message_bytes);
    std::vector<std::uint8_t> client_server{1};
    append_u16 (
      client_server, static_cast<std::uint16_t> (body.size ()));
    client_server.insert (
      client_server.end (), body.begin (), body.end ());
    std::vector<std::uint8_t> result{
      magic[0], magic[1], wire_major, static_cast<std::uint8_t> (kind), 0, 2};
    append_u32 (
      result, static_cast<std::uint32_t> (client_server.size ()));
    result.insert (
      result.end (), client_server.begin (), client_server.end ());
    return result;
}

client_server_client_admission_t decode_client_server_client_admission (
  std::span<const std::uint8_t> bytes,
  command expected_kind)
{
    validate_admission_kind (expected_kind);
    const auto header = decode_header (bytes);
    if (header.kind != expected_kind || header.flags != 0) {
        throw service_wire_error_t ("unexpected client admission command");
    }
    std::size_t offset = prefix_size;
    if (bytes.size () - offset < 1 || bytes[offset++] != 2) {
        throw service_wire_error_t ("admission is not ClientServer");
    }
    const auto outer_length = read_u32 (bytes, offset);
    if (outer_length != bytes.size () - offset
        || bytes.size () - offset < 3 || bytes[offset++] != 1) {
        throw service_wire_error_t ("invalid ClientServer client envelope");
    }
    const auto body_length = read_u16 (bytes, offset);
    if (body_length != bytes.size () - offset) {
        throw service_wire_error_t ("ClientServer client length mismatch");
    }
    client_server_client_admission_t result;
    result.channel_name = read_text8 (bytes, offset, "channel name");
    if (offset >= bytes.size () || bytes[offset++] != 1) {
        throw service_wire_error_t ("invalid ClientServer direction");
    }
    result.security_identity =
      read_text8 (bytes, offset, "security identity");
    result.effective_max_message_bytes = read_u32 (bytes, offset);
    if (result.effective_max_message_bytes == 0 || offset != bytes.size ()) {
        throw service_wire_error_t (
          "invalid ClientServer client admission");
    }
    return result;
}

std::vector<std::uint8_t> encode_client_server_server_admission (
  command kind,
  const client_server_server_admission_t &admission)
{
    validate_admission_kind (kind);
    if (admission.lifecycle_generation == 0
        || admission.descriptor_revision == 0 || admission.weight > 10000
        || admission.effective_max_message_bytes == 0) {
        throw service_wire_error_t (
          "invalid ClientServer server descriptor");
    }
    std::vector<std::uint8_t> body;
    append_text8 (body, admission.channel_name, "channel name");
    body.push_back (1);
    append_bytes8 (
      body, admission.server_routing_id, "server routing id");
    append_u64 (body, admission.lifecycle_generation);
    append_u64 (body, admission.descriptor_revision);
    append_u32 (body, admission.weight);
    body.push_back (runtime_state_wire (admission.state));
    append_text8 (
      body, admission.security_identity, "security identity");
    append_u32 (body, admission.effective_max_message_bytes);
    append_text16 (
      body, admission.advertised_endpoint, "advertised endpoint");
    if (body.size () > std::numeric_limits<std::uint16_t>::max ()) {
        throw service_wire_error_t (
          "ClientServer server descriptor exceeds u16");
    }
    std::vector<std::uint8_t> client_server{2};
    append_u16 (
      client_server, static_cast<std::uint16_t> (body.size ()));
    client_server.insert (
      client_server.end (), body.begin (), body.end ());
    std::vector<std::uint8_t> result{
      magic[0], magic[1], wire_major, static_cast<std::uint8_t> (kind), 0, 2};
    append_u32 (
      result, static_cast<std::uint32_t> (client_server.size ()));
    result.insert (
      result.end (), client_server.begin (), client_server.end ());
    return result;
}

client_server_server_admission_t decode_client_server_server_admission (
  std::span<const std::uint8_t> bytes,
  command expected_kind)
{
    validate_admission_kind (expected_kind);
    const auto header = decode_header (bytes);
    if (header.kind != expected_kind || header.flags != 0) {
        throw service_wire_error_t ("unexpected server admission command");
    }
    std::size_t offset = prefix_size;
    if (bytes.size () - offset < 1 || bytes[offset++] != 2) {
        throw service_wire_error_t ("admission is not ClientServer");
    }
    const auto outer_length = read_u32 (bytes, offset);
    if (outer_length != bytes.size () - offset
        || bytes.size () - offset < 3 || bytes[offset++] != 2) {
        throw service_wire_error_t ("invalid ClientServer server envelope");
    }
    const auto body_length = read_u16 (bytes, offset);
    if (body_length != bytes.size () - offset) {
        throw service_wire_error_t ("ClientServer server length mismatch");
    }
    client_server_server_admission_t result;
    result.channel_name = read_text8 (bytes, offset, "channel name");
    if (offset >= bytes.size () || bytes[offset++] != 1) {
        throw service_wire_error_t ("invalid ClientServer direction");
    }
    result.server_routing_id =
      read_bytes8 (bytes, offset, "server routing id");
    result.lifecycle_generation = read_u64 (bytes, offset);
    result.descriptor_revision = read_u64 (bytes, offset);
    result.weight = read_u32 (bytes, offset);
    if (offset >= bytes.size ()) {
        throw service_wire_error_t ("truncated ClientServer state");
    }
    result.state = runtime_state_from_wire (bytes[offset++]);
    result.security_identity =
      read_text8 (bytes, offset, "security identity");
    result.effective_max_message_bytes = read_u32 (bytes, offset);
    result.advertised_endpoint =
      read_text16 (bytes, offset, "advertised endpoint");
    if (result.lifecycle_generation == 0
        || result.descriptor_revision == 0 || result.weight > 10000
        || result.effective_max_message_bytes == 0
        || offset != bytes.size ()) {
        throw service_wire_error_t (
          "invalid ClientServer server admission");
    }
    return result;
}

std::vector<std::uint8_t> encode_reject (std::uint32_t reason)
{
    if (reason < 1 || reason > 12) {
        throw service_wire_error_t ("invalid reject reason");
    }
    std::vector<std::uint8_t> result{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::reject), 0};
    append_u32 (result, reason);
    return result;
}

std::uint32_t decode_reject (std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::reject || header.flags != 0
        || bytes.size () != prefix_size + 4) {
        throw service_wire_error_t ("invalid reject record");
    }
    std::size_t offset = prefix_size;
    const auto reason = read_u32 (bytes, offset);
    if (reason < 1 || reason > 12) {
        throw service_wire_error_t ("invalid reject reason");
    }
    return reason;
}

std::vector<std::uint8_t> encode_reply_header (
  std::uint64_t correlation,
  std::uint32_t terminal_result,
  std::uint32_t failure_code)
{
    if (correlation == 0) {
        throw service_wire_error_t ("reply correlation must be nonzero");
    }
    if (!valid_terminal_failure (
          terminal_result,
          static_cast<framework_error_code> (failure_code))) {
        throw service_wire_error_t (
          "invalid reply terminal fields: terminal="
          + std::to_string (terminal_result) + ", failure="
          + std::to_string (failure_code));
    }
    std::vector<std::uint8_t> result{
      magic[0], magic[1], wire_major,
      static_cast<std::uint8_t> (command::reply), 0};
    append_u64 (result, correlation);
    append_u32 (result, terminal_result);
    append_u32 (result, failure_code);
    return result;
}

reply_header_t decode_reply_header (std::span<const std::uint8_t> bytes)
{
    const auto header = decode_header (bytes);
    if (header.kind != command::reply || header.flags != 0
        || bytes.size () != prefix_size + 16) {
        throw service_wire_error_t ("invalid reply header");
    }
    std::size_t offset = prefix_size;
    const auto correlation = read_u64 (bytes, offset);
    const auto terminal = read_u32 (bytes, offset);
    const auto failure = read_u32 (bytes, offset);
    if (correlation == 0
        || !valid_terminal_failure (
          terminal,
          static_cast<framework_error_code> (failure))) {
        throw service_wire_error_t (
          "invalid reply terminal fields: correlation="
          + std::to_string (correlation) + ", terminal="
          + std::to_string (terminal) + ", failure="
          + std::to_string (failure));
    }
    return {correlation, terminal, failure};
}

std::vector<std::uint8_t> encode_user_spot_create_reply (
  std::uint64_t correlation,
  std::uint32_t terminal_result,
  std::uint32_t failure_code,
  user_spot_create_result_t result,
  const std::string &spot_id,
  std::uint64_t object_generation)
{
    auto bytes =
      encode_reply_header (correlation, terminal_result, failure_code);
    if (terminal_result != 0) {
        return bytes;
    }
    const auto result_value = static_cast<std::uint8_t> (result);
    if (result_value < 1 || result_value > 3
        || object_generation == 0) {
        throw service_wire_error_t (
          "invalid User Spot create success reply");
    }
    bytes.push_back (result_value);
    append_text8 (bytes, spot_id, "spot ID");
    append_u64 (bytes, object_generation);
    return bytes;
}

user_spot_create_reply_t decode_user_spot_create_reply (
  std::span<const std::uint8_t> bytes)
{
    if (bytes.size () < prefix_size + 16) {
        throw service_wire_error_t (
          "truncated User Spot create reply");
    }
    const auto header =
      decode_reply_header (bytes.first (prefix_size + 16));
    user_spot_create_reply_t reply;
    reply.header = header;
    if (header.terminal_result != 0) {
        if (bytes.size () != prefix_size + 16) {
            throw service_wire_error_t (
              "failed User Spot create reply has a tail");
        }
        return reply;
    }
    std::size_t offset = prefix_size + 16;
    if (offset >= bytes.size () || bytes[offset] < 1
        || bytes[offset] > 3) {
        throw service_wire_error_t (
          "invalid User Spot create result");
    }
    reply.result =
      static_cast<user_spot_create_result_t> (bytes[offset++]);
    reply.spot_id =
      read_text8 (bytes, offset, "spot ID");
    reply.object_generation = read_u64 (bytes, offset);
    if (reply.object_generation == 0 || offset != bytes.size ()) {
        throw service_wire_error_t (
          "invalid User Spot create success reply");
    }
    return reply;
}

std::vector<std::uint8_t> encode_actor_create_reply (
  std::uint64_t correlation,
  std::uint32_t terminal_result,
  std::uint32_t failure_code,
  actor_create_result_t result,
  const std::vector<std::uint8_t> &node_routing_id,
  const std::string &actor_id,
  std::uint64_t object_generation)
{
    auto bytes = encode_reply_header (
      correlation, terminal_result, failure_code);
    if (terminal_result != 0)
        return bytes;
    const auto encoded = static_cast<std::uint8_t> (result);
    if (encoded < 1 || encoded > 3)
        throw service_wire_error_t (
          "invalid Actor create result");
    bytes.push_back (encoded);
    if (result != actor_create_result_t::rejected) {
        if (node_routing_id.empty () || actor_id.empty ()
            || object_generation == 0)
            throw service_wire_error_t (
              "invalid Actor create success reply");
        append_bytes8 (bytes, node_routing_id, "Actor node RID");
        append_text8 (bytes, actor_id, "actor ID");
        append_u64 (bytes, object_generation);
    }
    return bytes;
}

actor_create_reply_t decode_actor_create_reply (
  std::span<const std::uint8_t> bytes)
{
    if (bytes.size () < prefix_size + 16)
        throw service_wire_error_t (
          "truncated Actor create reply");
    actor_create_reply_t reply;
    reply.header = decode_reply_header (
      bytes.first (prefix_size + 16));
    if (reply.header.terminal_result != 0) {
        if (bytes.size () != prefix_size + 16)
            throw service_wire_error_t (
              "failed Actor create reply has a tail");
        return reply;
    }
    std::size_t offset = prefix_size + 16;
    if (offset >= bytes.size () || bytes[offset] < 1
        || bytes[offset] > 3)
        throw service_wire_error_t (
          "invalid Actor create result");
    reply.result = static_cast<actor_create_result_t> (
      bytes[offset++]);
    if (reply.result != actor_create_result_t::rejected) {
        reply.node_routing_id =
          read_bytes8 (bytes, offset, "Actor node RID");
        reply.actor_id = read_text8 (bytes, offset, "actor ID");
        reply.object_generation = read_u64 (bytes, offset);
        if (reply.node_routing_id.empty () || reply.actor_id.empty ()
            || reply.object_generation == 0)
            throw service_wire_error_t (
              "invalid Actor create success reply");
    }
    if (offset != bytes.size ())
        throw service_wire_error_t (
          "Actor create reply has trailing bytes");
    return reply;
}

std::vector<std::uint8_t> encode_user_spot_close_reply (
  std::uint64_t correlation,
  std::uint32_t terminal_result,
  std::uint32_t failure_code,
  bool closed)
{
    auto bytes =
      encode_reply_header (correlation, terminal_result, failure_code);
    if (terminal_result == 0) {
        bytes.push_back (closed ? 1 : 0);
    }
    return bytes;
}

user_spot_close_reply_t decode_user_spot_close_reply (
  std::span<const std::uint8_t> bytes)
{
    if (bytes.size () < prefix_size + 16) {
        throw service_wire_error_t (
          "truncated User Spot close reply");
    }
    const auto header =
      decode_reply_header (bytes.first (prefix_size + 16));
    user_spot_close_reply_t reply;
    reply.header = header;
    if (header.terminal_result != 0) {
        if (bytes.size () != prefix_size + 16) {
            throw service_wire_error_t (
              "failed User Spot close reply has a tail");
        }
        return reply;
    }
    if (bytes.size () != prefix_size + 17
        || (bytes.back () != 0 && bytes.back () != 1)) {
        throw service_wire_error_t (
          "invalid User Spot close success reply");
    }
    reply.closed = bytes.back () == 1;
    return reply;
}

std::vector<std::uint8_t> encode_liveness (command kind, std::uint64_t probe_id)
{
    validate_kind (kind);
    if (probe_id == 0) {
        throw service_wire_error_t ("liveness probe id must be nonzero");
    }
    std::vector<std::uint8_t> bytes (liveness_size);
    bytes[0] = magic[0];
    bytes[1] = magic[1];
    bytes[2] = wire_major;
    bytes[3] = static_cast<std::uint8_t> (kind);
    bytes[4] = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[5 + index] = static_cast<std::uint8_t> (
          (probe_id >> ((7 - index) * 8)) & 0xffu);
    }
    return bytes;
}

liveness_record_t decode_liveness (std::span<const std::uint8_t> bytes)
{
    if (bytes.size () < liveness_size) {
        throw service_wire_error_t ("truncated liveness record");
    }
    if (bytes.size () > liveness_size) {
        throw service_wire_error_t ("liveness record has trailing bytes");
    }
    const auto header = decode_header (bytes);
    const auto kind = header.kind;
    validate_kind (kind);
    if (header.flags != 0) {
        throw service_wire_error_t ("liveness record uses a forbidden flag");
    }
    std::uint64_t probe_id = 0;
    for (std::size_t index = 5; index < bytes.size (); ++index) {
        probe_id = (probe_id << 8u) | bytes[index];
    }
    if (probe_id == 0) {
        throw service_wire_error_t ("liveness probe id must be nonzero");
    }
    return liveness_record_t{kind, probe_id};
}

} // namespace zlink::framework::runtime::protocol
