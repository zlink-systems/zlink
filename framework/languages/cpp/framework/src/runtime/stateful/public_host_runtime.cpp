/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/stateful/public_host_runtime.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/locations/pending_creation_projection.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"

#include <service_wire_constants.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace zlink::framework::runtime::host
{
namespace
{

constexpr std::string_view multipart_packet_name =
  protocol::framework_multipart_packet_name;
constexpr std::string_view multipart_content_type =
  protocol::framework_multipart_content_type;

bool mesh_trace_enabled ()
{
    const char *value = std::getenv ("ZLINK_CPP_MESH_TRACE");
    return value != nullptr && *value != '\0' && std::string_view (value) != "0";
}

void trace_mesh_host (std::string_view stage, std::string_view detail)
{
    if (mesh_trace_enabled ())
        std::cerr << "zlink mesh-host stage=" << stage << " " << detail << '\n';
}

const char *pump_result_name (mesh::raw_mesh_pump_result_t result) noexcept
{
    switch (result) {
        case mesh::raw_mesh_pump_result_t::no_data:
            return "no-data";
        case mesh::raw_mesh_pump_result_t::infrastructure:
            return "infrastructure";
        case mesh::raw_mesh_pump_result_t::application:
            return "application";
        case mesh::raw_mesh_pump_result_t::backpressured:
            return "backpressured";
        case mesh::raw_mesh_pump_result_t::capacity_exceeded:
            return "capacity-exceeded";
        case mesh::raw_mesh_pump_result_t::protocol_error:
            return "protocol-error";
    }
    return "unknown";
}

bool user_spot_operation_replay_expired (
  std::uint64_t deadline_unix_ms,
  std::int64_t now_unix_ms,
  std::chrono::milliseconds replay_retention)
{
    return now_unix_ms >= 0
      && static_cast<std::uint64_t> (now_unix_ms) > deadline_unix_ms
      && static_cast<std::uint64_t> (now_unix_ms) - deadline_unix_ms
           > static_cast<std::uint64_t> (
             std::max<std::int64_t> (0, replay_retention.count ()));
}

bool same_relocation_wire_object (
  const protocol::relocation_object_t &left,
  const protocol::relocation_object_t &right) noexcept
{
    if (left.kind != right.kind || left.object_id != right.object_id
        || left.object_generation != right.object_generation
        || left.expected_authority_owner_generation
             != right.expected_authority_owner_generation)
        return false;
    return left.kind
             != protocol::relocation_object_kind_t::instance_spot
           || left.stable_type == right.stable_type;
}

bool same_relocation_source_fence (
  const protocol::request_source_fence_t &source,
  const protocol::relocation_coordinator_fence_t &coordinator) noexcept
{
    return source.owner_id == coordinator.owner_id
           && source.lease_generation == coordinator.lease_generation
           && source.node_routing_id == coordinator.node_routing_id
           && source.node_generation == coordinator.node_generation;
}

void append_u32 (std::vector<std::uint8_t> &out, std::uint32_t value)
{
    out.push_back (static_cast<std::uint8_t> ((value >> 24u) & 0xffu));
    out.push_back (static_cast<std::uint8_t> ((value >> 16u) & 0xffu));
    out.push_back (static_cast<std::uint8_t> ((value >> 8u) & 0xffu));
    out.push_back (static_cast<std::uint8_t> (value & 0xffu));
}

std::uint32_t read_u32 (const std::vector<std::uint8_t> &bytes,
                        std::size_t &offset)
{
    if (offset + 4 > bytes.size ()) {
        throw protocol::service_wire_error_t (
          "framework multipart payload is truncated");
    }
    const auto value =
      (static_cast<std::uint32_t> (bytes[offset]) << 24u)
      | (static_cast<std::uint32_t> (bytes[offset + 1]) << 16u)
      | (static_cast<std::uint32_t> (bytes[offset + 2]) << 8u)
      | static_cast<std::uint32_t> (bytes[offset + 3]);
    offset += 4;
    return value;
}

std::vector<std::uint8_t> encode_parts (
  const std::vector<zlink::message_t> &parts)
{
    if (parts.empty ()) {
        throw std::invalid_argument (
          "framework multipart requires at least one part");
    }
    if (parts.size () > std::numeric_limits<std::uint32_t>::max ()) {
        throw std::length_error ("framework multipart part count is too large");
    }
    std::vector<std::uint8_t> encoded;
    append_u32 (encoded, static_cast<std::uint32_t> (parts.size ()));
    for (const auto &part : parts) {
        const auto bytes = part.to_bytes ();
        if (bytes.size () > std::numeric_limits<std::uint32_t>::max ()) {
            throw std::length_error ("framework multipart part is too large");
        }
        append_u32 (encoded, static_cast<std::uint32_t> (bytes.size ()));
        encoded.insert (encoded.end (), bytes.begin (), bytes.end ());
    }
    return encoded;
}

std::vector<zlink::message_t> decode_parts (
  const std::vector<std::uint8_t> &encoded)
{
    std::size_t offset = 0;
    const auto count = read_u32 (encoded, offset);
    if (count == 0
        || count > (encoded.size () - offset) / sizeof (std::uint32_t)) {
        throw protocol::service_wire_error_t (
          "framework multipart part count is invalid");
    }
    std::vector<zlink::message_t> parts;
    parts.reserve (count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto size = read_u32 (encoded, offset);
        if (size > encoded.size () - offset) {
            throw protocol::service_wire_error_t (
              "framework multipart part is truncated");
        }
        parts.push_back (zlink::message_t::from (
          std::span<const std::uint8_t> (encoded.data () + offset, size)));
        offset += size;
    }
    if (offset != encoded.size ()) {
        throw protocol::service_wire_error_t (
          "framework multipart payload has trailing bytes");
    }
    return parts;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>>
read_route_owner_fence (
  const std::shared_ptr<zlink::framework::location_repository_t> &store,
  char object_kind,
  std::string_view object_id,
  std::uint64_t object_generation,
  std::uint64_t authority_owner_generation = 0,
  std::uint64_t owner_lease_generation = 0)
{
    if (object_id.empty () || object_generation == 0)
        return std::nullopt;
    if (authority_owner_generation != 0
        || owner_lease_generation != 0) {
        if (authority_owner_generation == 0
            || owner_lease_generation == 0)
            return std::nullopt;
        return std::pair{authority_owner_generation,
                         owner_lease_generation};
    }
    if (!store)
        return std::nullopt;
    try {
        auto read = store->read_authority (
          authority_key_t{
            std::string (1, object_kind) + ":" + std::string (object_id)})
          .result ();
        if (!read)
            return std::nullopt;
        const auto *snapshot =
          std::get_if<authority_snapshot_t> (&read.value ());
        if (!snapshot || snapshot->object_generation != object_generation
            || snapshot->authority_owner_generation == 0
            || snapshot->owner.lease_generation <= 0)
            return std::nullopt;
        return std::pair{
          snapshot->authority_owner_generation,
          static_cast<std::uint64_t> (snapshot->owner.lease_generation)};
    }
    catch (...) {
        return std::nullopt;
    }
}

std::string spot_route_cache_key (
  const zlink::routing_id_t &target_node_rid,
  std::string_view target_spot_id,
  std::uint64_t target_spot_generation)
{
    return target_node_rid.to_string () + ":" + std::string (target_spot_id)
           + ":" + std::to_string (target_spot_generation);
}

zlink::submit_result_t submitted (bool accepted)
{
    return accepted ? zlink::submit_result_t::ok
                    : zlink::submit_result_t::not_connected;
}

record_kind_t record_kind (protocol::command command)
{
    switch (command) {
        case protocol::command::nodeSend:
            return record_kind_t::node_send;
        case protocol::command::nodeRequest:
            return record_kind_t::node_request;
        case protocol::command::channelSend:
            return record_kind_t::channel_send;
        case protocol::command::channelRequest:
            return record_kind_t::channel_request;
        case protocol::command::spotSend:
            return record_kind_t::spot_send;
        case protocol::command::spotRequest:
            return record_kind_t::spot_request;
        case protocol::command::actorSend:
            return record_kind_t::actor_send;
        case protocol::command::actorRequest:
            return record_kind_t::actor_request;
        default:
            throw protocol::service_wire_error_t (
              "mailbox record is not application messaging");
    }
}

operation_kind_t operation_kind (record_kind_t)
{
    return operation_kind_t::none;
}

bool is_request (record_kind_t kind)
{
    return kind == record_kind_t::node_request
           || kind == record_kind_t::channel_request
           || kind == record_kind_t::spot_request
           || kind == record_kind_t::actor_request;
}

std::vector<std::vector<std::uint8_t>>
unpack_infrastructure_reply (const std::vector<std::uint8_t> &packed)
{
    if (packed.empty () || packed.front () == 0
        || packed.front () > 2) {
        throw protocol::service_wire_error_t (
          "invalid packed infrastructure reply");
    }
    std::size_t offset = 1;
    std::vector<std::vector<std::uint8_t>> parts;
    parts.reserve (packed.front ());
    for (std::uint8_t index = 0; index < packed.front (); ++index) {
        const auto length = read_u32 (packed, offset);
        if (packed.size () - offset < length) {
            throw protocol::service_wire_error_t (
              "truncated packed infrastructure reply");
        }
        parts.emplace_back (
          packed.begin () + static_cast<std::ptrdiff_t> (offset),
          packed.begin ()
            + static_cast<std::ptrdiff_t> (offset + length));
        offset += length;
    }
    if (offset != packed.size ()) {
        throw protocol::service_wire_error_t (
          "packed infrastructure reply has trailing bytes");
    }
    return parts;
}

std::string user_spot_operation_key (
  const std::vector<std::uint8_t> &source,
  std::uint64_t source_generation,
  const protocol::wire_operation_id_t &operation)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill ('0');
    for (const auto value : source)
        stream << std::setw (2) << static_cast<unsigned> (value);
    stream << ':' << source_generation << ':' << operation.high
           << ':' << operation.low;
    return stream.str ();
}

std::vector<std::byte> ready_user_spot_authority_payload (
  const stateful::object_ref_t &object,
  const std::string &stable_type)
{
    // The authority payload is framework-owned. Application creation bytes
    // remain in the reservation projection and are never published as Ready.
    const std::string value =
      "zlink:user-spot:ready:v1\n" + stable_type + "\n"
      + object.key + "\n" + std::to_string (object.object_generation)
      + "\n"
      + std::to_string (object.authority_owner_generation);
    std::vector<std::byte> result;
    result.reserve (value.size ());
    for (const auto character : value)
        result.push_back (
          static_cast<std::byte> (
            static_cast<unsigned char> (character)));
    return result;
}

struct instance_ready_state_t
{
    std::string stable_type;
    std::string spot_id;
    std::uint64_t object_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    std::string recovery_reference;
    std::uint32_t recovery_checksum = 0;
    protocol::wire_operation_id_t operation;
    std::array<std::byte, 32> request_sha256{};
    bool completed = false;
    std::uint32_t terminal_result = 0;
    std::uint32_t failure_code = 0;
    std::optional<protocol::application_payload_t> reply;
};

void append_u64_value (std::vector<std::uint8_t> &out,
                       std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back (static_cast<std::uint8_t> (value >> shift));
}

std::uint64_t read_u64_value (std::span<const std::uint8_t> bytes,
                              std::size_t &offset)
{
    if (bytes.size () - offset < 8)
        throw protocol::service_wire_error_t (
          "Instance authority payload is truncated");
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index)
        value = (value << 8) | bytes[offset++];
    return value;
}

void append_text32_value (std::vector<std::uint8_t> &out,
                          const std::string &value)
{
    if (value.size () > std::numeric_limits<std::uint32_t>::max ())
        throw protocol::service_wire_error_t (
          "Instance authority text exceeds u32");
    append_u32 (out, static_cast<std::uint32_t> (value.size ()));
    out.insert (out.end (), value.begin (), value.end ());
}

std::string read_text32_value (const std::vector<std::uint8_t> &bytes,
                               std::size_t &offset)
{
    const auto size = read_u32 (bytes, offset);
    if (bytes.size () - offset < size)
        throw protocol::service_wire_error_t (
          "Instance authority text is truncated");
    std::string value (
      bytes.begin () + static_cast<std::ptrdiff_t> (offset),
      bytes.begin () + static_cast<std::ptrdiff_t> (offset + size));
    offset += size;
    return value;
}

std::vector<std::byte> encode_instance_ready_state (
  const instance_ready_state_t &state)
{
    std::vector<std::uint8_t> bytes{'Z', 'L', 'I', 'R', 1};
    append_text32_value (bytes, state.stable_type);
    append_text32_value (bytes, state.spot_id);
    append_u64_value (bytes, state.object_generation);
    append_u64_value (bytes, state.authority_owner_generation);
    append_text32_value (bytes, state.recovery_reference);
    append_u32 (bytes, state.recovery_checksum);
    append_u64_value (bytes, state.operation.high);
    append_u64_value (bytes, state.operation.low);
    for (const auto value : state.request_sha256)
        bytes.push_back (std::to_integer<std::uint8_t> (value));
    bytes.push_back (state.completed ? 1 : 0);
    append_u32 (bytes, state.terminal_result);
    append_u32 (bytes, state.failure_code);
    if (state.reply) {
        bytes.push_back (1);
        const auto reply = protocol::encode_application_payload (*state.reply);
        append_u32 (bytes, static_cast<std::uint32_t> (reply.size ()));
        bytes.insert (bytes.end (), reply.begin (), reply.end ());
    }
    else {
        bytes.push_back (0);
    }
    std::vector<std::byte> result;
    result.reserve (bytes.size ());
    for (const auto value : bytes)
        result.push_back (static_cast<std::byte> (value));
    return result;
}

std::optional<instance_ready_state_t> decode_instance_ready_state (
  const std::vector<std::byte> &payload)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve (payload.size ());
    for (const auto value : payload)
        bytes.push_back (std::to_integer<std::uint8_t> (value));
    if (bytes.size () < 5 || bytes[0] != 'Z' || bytes[1] != 'L'
        || bytes[2] != 'I' || bytes[3] != 'R' || bytes[4] != 1)
        return std::nullopt;
    try {
        std::size_t offset = 5;
        instance_ready_state_t state;
        state.stable_type = read_text32_value (bytes, offset);
        state.spot_id = read_text32_value (bytes, offset);
        state.object_generation = read_u64_value (bytes, offset);
        state.authority_owner_generation = read_u64_value (bytes, offset);
        state.recovery_reference = read_text32_value (bytes, offset);
        state.recovery_checksum = read_u32 (bytes, offset);
        state.operation.high = read_u64_value (bytes, offset);
        state.operation.low = read_u64_value (bytes, offset);
        if (bytes.size () - offset < state.request_sha256.size ())
            return std::nullopt;
        for (auto &value : state.request_sha256)
            value = static_cast<std::byte> (bytes[offset++]);
        if (offset >= bytes.size () || bytes[offset] > 1)
            return std::nullopt;
        state.completed = bytes[offset++] == 1;
        state.terminal_result = read_u32 (bytes, offset);
        state.failure_code = read_u32 (bytes, offset);
        if (offset >= bytes.size () || bytes[offset] > 1)
            return std::nullopt;
        const auto has_reply = bytes[offset++] == 1;
        if (has_reply) {
            const auto size = read_u32 (bytes, offset);
            if (bytes.size () - offset < size)
                return std::nullopt;
            state.reply = protocol::decode_application_payload (
              std::span<const std::uint8_t> (bytes).subspan (offset, size));
            offset += size;
        }
        if (offset != bytes.size () || state.stable_type.empty ()
            || state.spot_id.empty () || state.object_generation == 0
            || state.authority_owner_generation == 0
            || (state.operation.high == 0 && state.operation.low == 0))
            return std::nullopt;
        return state;
    }
    catch (const protocol::service_wire_error_t &) {
        return std::nullopt;
    }
}

std::vector<std::byte> closing_user_spot_authority_payload (
  const stateful::object_ref_t &object)
{
    const std::string value =
      "zlink:user-spot:closing:v1\n" + object.key + "\n"
      + std::to_string (object.object_generation) + "\n"
      + std::to_string (object.authority_owner_generation);
    std::vector<std::byte> result;
    result.reserve (value.size ());
    for (const auto character : value)
        result.push_back (
          static_cast<std::byte> (
            static_cast<unsigned char> (character)));
    return result;
}

struct instance_closing_state_t
{
    std::string stable_type;
    std::string spot_id;
    std::uint64_t object_generation = 0;
    std::uint64_t authority_owner_generation = 0;
};

std::vector<std::byte> encode_instance_closing_state (
  const instance_closing_state_t &state)
{
    const std::string value =
      "zlink:instance-spot:closing:v1\n" + state.stable_type + "\n"
      + state.spot_id + "\n" + std::to_string (state.object_generation)
      + "\n" + std::to_string (state.authority_owner_generation);
    std::vector<std::byte> result;
    result.reserve (value.size ());
    for (const auto character : value)
        result.push_back (
          static_cast<std::byte> (
            static_cast<unsigned char> (character)));
    return result;
}

std::optional<instance_closing_state_t> decode_instance_closing_state (
  const std::vector<std::byte> &payload)
{
    std::string value;
    value.reserve (payload.size ());
    for (const auto character : payload)
        value.push_back (static_cast<char> (std::to_integer<unsigned char> (character)));
    constexpr std::string_view prefix =
      "zlink:instance-spot:closing:v1\n";
    if (value.rfind (prefix, 0) != 0)
        return std::nullopt;
    std::vector<std::string> fields;
    std::size_t begin = prefix.size ();
    while (begin <= value.size ()) {
        const auto end = value.find ('\n', begin);
        fields.push_back (value.substr (begin, end == std::string::npos
                                                 ? std::string::npos
                                                 : end - begin));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    if (fields.size () != 4 || fields[0].empty () || fields[1].empty ())
        return std::nullopt;
    try {
        const auto object_generation = std::stoull (fields[2]);
        const auto owner_generation = std::stoull (fields[3]);
        if (object_generation == 0 || owner_generation == 0)
            return std::nullopt;
        return instance_closing_state_t{
          fields[0], fields[1], object_generation, owner_generation};
    }
    catch (...) {
        return std::nullopt;
    }
}

std::uint64_t unix_milliseconds_now ()
{
    return static_cast<std::uint64_t> (
      std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::system_clock::now ().time_since_epoch ())
        .count ());
}

zlink::framework::object_reservation_fence_t public_fence (
  const protocol::user_spot_reservation_fence_t &wire,
  const std::string &mesh_name,
  const std::string &stable_type)
{
    if (wire.target_owner_lease_generation
        > static_cast<std::uint64_t> (
          std::numeric_limits<std::int64_t>::max ()))
        throw protocol::service_wire_error_t (
          "target owner lease generation exceeds the public Store range");
    return {
      wire.reservation_id,
      wire.expected_store_version,
      wire.object_generation,
      wire.authority_owner_generation,
      {mesh_name,
       node_rid_t::from_string (
         zlink::routing_id_t::from (
           wire.target_node_routing_id)
           .to_string ()),
       wire.target_node_generation,
       {wire.target_owner_id,
        static_cast<std::int64_t> (
          wire.target_owner_lease_generation)}},
      {0,
       wire.pending_capacity_delta,
       spot_type_capacity_delta_t{
         placement_object_kind_t::user_spot,
         stable_type,
         wire.pending_capacity_delta}}};
}

} // namespace

zlink::routing_id_t node_status_t::routing_id () const
{
    return node_routing_id;
}

std::string node_status_t::local_endpoint () const
{
    return endpoint;
}

std::uint64_t node_status_t::lifecycle_generation () const noexcept
{
    return generation;
}

std::uint64_t spot_status_t::lifecycle_generation () const noexcept
{
    return generation;
}

spot_handle_t::spot_handle_t (
  std::shared_ptr<public_host_runtime_t> host,
  stateful::object_ref_t object) :
    _host (std::move (host)), _object (std::move (object))
{
}

spot_status_t spot_handle_t::status () const
{
    return {_object.object_generation};
}

const std::string &spot_handle_t::spot_id () const noexcept
{
    return _object.key;
}

zlink::submit_result_t spot_handle_t::send_to_spot (
  const zlink::routing_id_t &target_node_rid,
  const std::string &target_spot_id,
  std::uint64_t target_spot_generation,
  const std::vector<zlink::message_t> &parts,
  zlink::send_flags_t,
  std::span<const std::uint8_t> metadata)
{
    if (!_host) {
        return zlink::submit_result_t::invalid_handle;
    }
    const auto peer = _host->transport ().topology ().peer (
      target_node_rid.to_bytes ());
    const auto target_node_generation =
      peer ? peer->descriptor.lifecycle_generation
           : _host->status ().lifecycle_generation ();
    const auto route_fence = _host->resolve_spot_route_fence (
      target_node_rid, target_spot_id, target_spot_generation);
    if (!route_fence)
        return zlink::submit_result_t::not_found;
    const auto target = protocol::spot_route_fence_t{
      target_spot_id,
      target_spot_generation,
      target_node_rid.to_bytes (),
      target_node_generation,
      route_fence->first,
      route_fence->second};
    return submitted (_host->transport ().send_to_spot (
      target_node_rid.to_bytes (), spot_id (), target,
      _host->encode_application (parts, metadata)));
}

zlink::submit_result_t spot_handle_t::request_to_spot (
  const zlink::routing_id_t &target_node_rid,
  const std::string &target_spot_id,
  std::uint64_t target_spot_generation,
  const std::vector<zlink::message_t> &parts,
  operation_id_t &operation,
  zlink::send_flags_t,
  std::chrono::milliseconds timeout,
  std::span<const std::uint8_t> metadata,
  spot_request_completion_t completion)
{
    if (!_host) {
        return zlink::submit_result_t::invalid_handle;
    }
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument (
          "framework SPOT request timeout must be positive");
    }
    operation = _host->next_operation ();
    if (!_host->try_reserve_completion (operation))
        return zlink::submit_result_t::backpressured;
    try {
        const auto peer = _host->transport ().topology ().peer (
          target_node_rid.to_bytes ());
        const auto target_node_generation =
          peer ? peer->descriptor.lifecycle_generation
               : _host->status ().lifecycle_generation ();
        const auto route_fence = _host->resolve_spot_route_fence (
          target_node_rid, target_spot_id, target_spot_generation);
        if (!route_fence) {
            _host->release_completion (operation);
            return zlink::submit_result_t::not_found;
        }
        const auto target = protocol::spot_route_fence_t{
          target_spot_id,
          target_spot_generation,
          target_node_rid.to_bytes (),
          target_node_generation,
          route_fence->first,
          route_fence->second};
        const auto host = _host;
        const auto direct_completion = static_cast<bool> (completion);
        if (target.target_node_routing_id
            == host->status ().routing_id ().to_bytes ()) {
            const auto submitted = host->enqueue_local_spot_request (
              target, parts, operation, timeout, metadata,
              std::move (completion));
            if (submitted != zlink::submit_result_t::ok)
                host->release_completion (operation);
            return submitted;
        }
        const auto accepted = _host->transport ().request_to_spot (
          target_node_rid.to_bytes (), spot_id (), target,
          _host->encode_application (parts, metadata), timeout,
          [host, operation, completion = std::move (completion), direct_completion] (
            foundation::operation_terminal_t terminal,
            std::vector<std::uint8_t> payload) mutable {
              if (!direct_completion) {
                  try {
                      host->complete_operation (
                        operation, operation_kind_t::none, terminal,
                        std::move (payload));
                  }
                  catch (...) {
                      host->release_completion (operation);
                  }
                  return;
              }
              result_t<std::vector<zlink::message_t>> decoded =
                result_t<std::vector<zlink::message_t>>::failure (
                  framework_error_kind_t::internal_failure,
                  "SPOT request completion was not decoded");
              if (terminal == foundation::operation_terminal_t::completed) {
                  try {
                      decoded = result_t<std::vector<zlink::message_t>>::success (
                        host->decode_application (
                          protocol::decode_application_payload (payload)));
                  }
                  catch (const protocol::service_wire_error_t &error) {
                      decoded = result_t<std::vector<zlink::message_t>>::failure (
                        framework_error_kind_t::protocol_error, error.what ());
                  }
                  catch (const std::exception &error) {
                      decoded = result_t<std::vector<zlink::message_t>>::failure (
                        framework_error_kind_t::internal_failure, error.what ());
                  }
              }
              host->release_completion (operation);
              try {
                  completion (terminal, std::move (decoded));
              }
              catch (...) {
              }
          }, protocol::wire_operation_id_t{operation.high, operation.low});
        if (!accepted)
            _host->release_completion (operation);
        return submitted (accepted);
    }
    catch (...) {
        _host->release_completion (operation);
        throw;
    }
}

zlink::submit_result_t spot_handle_t::publish (
  const std::string &channel_name,
  const std::string &,
  const std::vector<zlink::message_t> &parts,
  zlink::send_flags_t,
  std::span<const std::uint8_t> metadata)
{
    if (!_host)
        return zlink::submit_result_t::invalid_handle;
    const auto targets = _host->transport ().topology ().peers ();
    const auto encoded =
      _host->encode_application (parts, metadata);
    ready_record_t owner;
    owner.owner_kind = owner_kind_t::node;
    owner.domain = ready_domain_t::application;
    receive_record_t local;
    local.kind = record_kind_t::node_send;
    local.domain = ready_domain_t::application;
    local.source_node_rid = _host->status ().routing_id ();
    {
        std::lock_guard lock (_host->_mutex);
        _host->_local_application_dispatches.push_back (
          public_host_runtime_t::local_application_dispatch_t{
            std::move (owner), std::move (local), parts});
    }
    for (const auto &target : targets) {
        (void) _host->transport ().send_to_node (
          target.descriptor.node_routing_id,
          encoded);
    }
    return zlink::submit_result_t::ok;
}

void spot_handle_t::set_subscription (const std::string &,
                                      const std::string &)
{
}

void spot_handle_t::unset_subscription (const std::string &,
                                        const std::string &)
{
}

bool spot_handle_t::close () noexcept
{
    if (!_host) {
        return false;
    }
    const auto [error, closed] = _host->objects ().close_spot (_object);
    return error == stateful::stateful_error_t::none && closed;
}

actor_handle_t::actor_handle_t (
  std::shared_ptr<public_host_runtime_t> host,
  actor_ref_t actor,
  stateful::object_ref_t object) :
    _host (std::move (host)),
    _actor (std::move (actor)),
    _object (std::move (object))
{
}

const actor_ref_t &actor_handle_t::ref () const noexcept
{
    return _actor;
}

zlink::submit_result_t actor_handle_t::join_entry_spot (
  const zlink::routing_id_t &target_node_rid,
  const std::vector<zlink::message_t> &parts,
  operation_id_t &operation,
  std::chrono::milliseconds timeout)
{
    if (!_host) {
        return zlink::submit_result_t::invalid_handle;
    }
    const auto entry = _host->entry_spot ();
    return join_spot (
      target_node_rid, entry.spot_id (),
      entry.status ().lifecycle_generation (), parts, operation, timeout);
}

zlink::submit_result_t actor_handle_t::join_spot (
  const zlink::routing_id_t &target_node_rid,
  const std::string &target_spot_id,
  std::uint64_t target_spot_generation,
  const std::vector<zlink::message_t> &parts,
  operation_id_t &operation,
  std::chrono::milliseconds)
{
    if (!_host) {
        return zlink::submit_result_t::invalid_handle;
    }
    if (target_node_rid.to_bytes ()
        != _host->status ().routing_id ().to_bytes ()) {
        return zlink::submit_result_t::not_connected;
    }
    return _host->begin_local_actor_join (
      _actor, target_spot_id, target_spot_generation, parts, operation);
}

zlink::submit_result_t actor_handle_t::send_to (
  const actor_ref_t &target,
  const std::vector<zlink::message_t> &parts,
  zlink::send_flags_t,
  std::span<const std::uint8_t> metadata)
{
    return !_host ? zlink::submit_result_t::invalid_handle
                  : _host->send_to_actor (target, parts, metadata);
}

zlink::submit_result_t actor_handle_t::request_to (
  const actor_ref_t &target,
  const std::vector<zlink::message_t> &parts,
  operation_id_t &operation,
  zlink::send_flags_t,
  std::chrono::milliseconds timeout,
  std::span<const std::uint8_t> metadata)
{
    return !_host ? zlink::submit_result_t::invalid_handle
                  : _host->request_to_actor (
                      target, parts, operation, timeout, metadata);
}

public_host_runtime_t::public_host_runtime_t (host_options_t options) :
    _options (std::move (options)),
    _entry_spot_id (
      zlink::framework::detail::new_entry_spot_id (
        zlink::routing_id_t::from (
          _options.mesh.descriptor.node_routing_id)
          .to_string ())),
    _transport (
      std::make_shared<mesh::raw_mesh_node_owner_t> (_options.mesh)),
    _relocation_wire (
      std::make_unique<stateful::raw_relocation_replay_coordinator_t> (
        *_transport)),
    _objects (
      _options.mesh.application_message_budget,
      _options.mesh.infrastructure_message_budget,
      _options.mesh.application_byte_budget,
      _options.mesh.infrastructure_byte_budget),
    _sessions ([this] (const std::string &actor_id) {
        std::lock_guard lock (_mutex);
        const auto found = _actors.find (actor_id);
        return found == _actors.end ()
                 ? std::optional<stateful::object_ref_t>{}
                 : std::make_optional (found->second.second);
    })
{
    const auto &descriptor = _options.mesh.descriptor;
    _objects.replace_placement_candidates (
      {stateful::placement_candidate_t{
        descriptor.mesh_name,
        std::string (descriptor.node_routing_id.begin (),
                     descriptor.node_routing_id.end ()),
        _options.object_stable_types,
        descriptor.placement_weight,
        descriptor.active_capacity_limit,
        descriptor.active_capacity_used,
        descriptor.pending_capacity_limit,
        descriptor.pending_capacity_used}});
}

public_host_runtime_t::~public_host_runtime_t ()
{
    close ();
}

void public_host_runtime_t::configure_stateful_dispatch (
  stateful::accepted_record_authority_resolver_t resolver)
{
    if (!resolver)
        throw std::invalid_argument (
          "stateful dispatch authority resolver must not be empty");
    std::lock_guard lock (_mutex);
    if (_started || _stateful_dispatch)
        throw std::logic_error (
          "stateful dispatch must be configured once before host start");
    _stateful_dispatch =
      std::make_unique<stateful::raw_stateful_dispatch_t> (
        _objects, *_transport, std::move (resolver));
}

void public_host_runtime_t::configure_message_follow_handler (
  std::function<void (const protocol::message_follow_notice_t &)> handler)
{
    std::lock_guard lock (_mutex);
    _message_follow_handler = std::move (handler);
}

void public_host_runtime_t::start ()
{
    std::lock_guard lock (_mutex);
    if (_started || _closing) {
        return;
    }
    _transport->start ();
    _started = true;
    if (_maintenance_started)
        _maintenance_started ();
}

void public_host_runtime_t::close () noexcept
{
    std::function<void ()> maintenance_closing;
    {
        std::lock_guard lock (_mutex);
        if (!_started || _closing) {
            return;
        }
        _closing = true;
        maintenance_closing = _maintenance_closing;
    }
    if (maintenance_closing) {
        try {
            maintenance_closing ();
        }
        catch (...) {
        }
    }
    terminate_local_spot_requests (
      foundation::operation_terminal_t::shutdown);
    {
        std::lock_guard lock (_mutex);
        _started = false;
        _completions.clear ();
        _local_application_dispatches.clear ();
        _local_spot_requests.clear ();
        _local_spot_request_deadlines.clear ();
        _local_spot_request_bytes = 0;
        _session_seal_terminals.clear ();
        _session_journal_terminals.clear ();
        _session_route_terminals.clear ();
    }
    _transport->close ();
    {
        std::lock_guard lock (_mutex);
        _closing = false;
    }
}

bool public_host_runtime_t::connect_peer (
  const std::string &endpoint,
  std::optional<zlink::routing_id_t> expected,
  std::uint64_t expected_lifecycle_generation,
  std::string security_identity)
{
    bool connected = false;
    if (expected) {
        auto descriptor = _options.mesh.descriptor;
        descriptor.node_routing_id = expected->to_bytes ();
        descriptor.advertised_endpoint = endpoint;
        descriptor.lifecycle_generation =
          expected_lifecycle_generation;
        descriptor.security_identity =
          std::move (security_identity);
        connected = _transport->connect_peer (
          endpoint, std::move (descriptor));
    } else {
        connected = _transport->connect_peer (endpoint);
    }
    if (connected) {
        std::lock_guard lock (_mutex);
        _peer_endpoints.insert_or_assign (
          endpoint, expected ? expected->to_string () : std::string{});
    }
    return connected;
}

void public_host_runtime_t::expect_peer (
  const std::string &endpoint,
  const zlink::routing_id_t &expected,
  std::uint64_t expected_lifecycle_generation,
  std::string security_identity)
{
    auto descriptor = _options.mesh.descriptor;
    descriptor.node_routing_id = expected.to_bytes ();
    descriptor.advertised_endpoint = endpoint;
    descriptor.lifecycle_generation =
      expected_lifecycle_generation;
    descriptor.security_identity = std::move (security_identity);
    _transport->expect_peer (std::move (descriptor));
}

void public_host_runtime_t::forget_peer (
  const std::string &endpoint,
  const zlink::routing_id_t &expected)
{
    _transport->forget_peer (expected.to_bytes (), endpoint);
}

void public_host_runtime_t::disconnect_peer (
  const std::string &endpoint) noexcept
{
    std::lock_guard lock (_mutex);
    _peer_endpoints.erase (endpoint);
}

node_status_t public_host_runtime_t::status () const
{
    const auto descriptor = _transport->topology ().local_descriptor ();
    node_status_t::state_t state = node_status_t::state_t::preparing;
    switch (descriptor.state) {
        case mesh::service_node_state_t::serving:
            state = node_status_t::state_t::serving;
            break;
        case mesh::service_node_state_t::draining:
        case mesh::service_node_state_t::retiring:
            state = node_status_t::state_t::draining;
            break;
        case mesh::service_node_state_t::stopped:
            state = node_status_t::state_t::stopped;
            break;
        case mesh::service_node_state_t::error:
            state = node_status_t::state_t::error;
            break;
        default:
            break;
    }
    return {state,
            zlink::routing_id_t::from (descriptor.node_routing_id),
            descriptor.advertised_endpoint,
            descriptor.lifecycle_generation};
}

void public_host_runtime_t::set_channel_weight (
  const std::string &channel_name,
  std::uint32_t weight)
{
    if (weight > 10000) {
        throw std::invalid_argument ("channel weight exceeds 10000");
    }
    auto descriptor = _transport->topology ().local_descriptor ();
    const auto found = std::find_if (
      descriptor.channels.begin (), descriptor.channels.end (),
      [&] (const auto &channel) { return channel.name == channel_name; });
    if (found == descriptor.channels.end ()) {
        throw std::invalid_argument ("channel is not registered");
    }
    found->weight = weight;
    ++descriptor.descriptor_revision;
    _transport->topology ().publish_local (std::move (descriptor));
}

mesh::raw_mesh_node_owner_t &public_host_runtime_t::transport () noexcept
{
    return *_transport;
}

bool public_host_runtime_t::send_message_follow (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::message_follow_notice_t &notice)
{
    return _transport->send_message_follow (target_routing_id, notice);
}

stateful::stateful_object_runtime_t &
public_host_runtime_t::objects () noexcept
{
    return _objects;
}

stateful::stream_session_registry_t &
public_host_runtime_t::sessions () noexcept
{
    return _sessions;
}

stateful::raw_relocation_replay_coordinator_t &
public_host_runtime_t::relocation_wire () noexcept
{
    return *_relocation_wire;
}

void public_host_runtime_t::configure_user_spot_operations (
  std::shared_ptr<zlink::framework::location_repository_t> store,
  user_spot_materializer_t materializer)
{
    if (!store || !materializer)
        throw std::invalid_argument (
          "User Spot operations require a Location Store and materializer");
    std::lock_guard lock (_mutex);
    if (_started)
        throw std::logic_error (
          "User Spot operations must be configured before start");
    _user_spot_store = std::move (store);
    _user_spot_materializer = std::move (materializer);
    std::lock_guard route_cache_lock (_route_cache_mutex);
    _spot_route_fences.clear ();
}

void public_host_runtime_t::configure_spot_route_fence_resolver (
  spot_route_fence_resolver_t resolver)
{
    std::lock_guard lock (_mutex);
    if (_started)
        throw std::logic_error (
          "Spot route fence resolver must be configured before host start");
    _spot_route_fence_resolver = std::move (resolver);
    std::lock_guard route_cache_lock (_route_cache_mutex);
    _spot_route_fences.clear ();
}

void public_host_runtime_t::configure_actor_create_operations (
  actor_create_operation_target_t target)
{
    if (!target)
        throw std::invalid_argument (
          "Actor create operation target is required");
    std::lock_guard lock (_mutex);
    if (_started || _actor_create_target)
        throw std::logic_error (
          "Actor create operations must be configured once before host start");
    _actor_create_target = std::move (target);
}

void public_host_runtime_t::configure_instance_spot_operations (
  std::shared_ptr<zlink::framework::location_repository_t> store,
  std::shared_ptr<stateful::relocation_store_port_t> relocations,
  location_owner_token_t owner,
  instance_spot_activation_materializer_t materializer)
{
    if (!store || !relocations || owner.owner_id.empty ()
        || owner.lease_generation <= 0 || !materializer)
        throw std::invalid_argument (
          "Instance Spot operations require Location and Relocation Stores, an owner lease, and a materializer");
    std::lock_guard lock (_mutex);
    if (_started)
        throw std::logic_error (
          "Instance Spot operations must be configured before start");
    _user_spot_store = std::move (store);
    _session_relocations = relocations;
    _instance_spot_relocations = std::move (relocations);
    _instance_spot_owner = std::move (owner);
    _instance_spot_materializer = std::move (materializer);
}

bool public_host_runtime_t::evict_instance_spot (
  const std::string &stable_type,
  const std::string &spot_id,
  std::uint64_t object_generation,
  std::uint64_t authority_owner_generation,
  std::function<bool ()> close_local)
{
    if (!close_local || stable_type.empty () || spot_id.empty ()
        || object_generation == 0 || authority_owner_generation == 0)
        return false;

    std::shared_ptr<zlink::framework::location_repository_t> store;
    location_owner_token_t instance_owner;
    {
        std::lock_guard lock (_mutex);
        store = _user_spot_store;
        instance_owner = _instance_spot_owner;
    }
    if (!store || instance_owner.owner_id.empty ()
        || instance_owner.lease_generation <= 0)
        return false;

    const authority_key_t authority_key{
      std::to_string (static_cast<int> (placement_object_kind_t::instance_spot))
      + ":" + spot_id};
    const auto current = store->read_authority (authority_key).result ().value ();
    const auto *snapshot = std::get_if<authority_snapshot_t> (&current);
    if (!snapshot
        || snapshot->allocation.state != placement_allocation_state_t::active
        || snapshot->allocation.object_kind
             != placement_object_kind_t::instance_spot
        || snapshot->allocation.stable_type != stable_type
        || snapshot->object_generation != object_generation
        || snapshot->authority_owner_generation
             != authority_owner_generation
        || snapshot->allocation.target.owner.owner_id
             != instance_owner.owner_id
        || snapshot->allocation.target.owner.lease_generation
             != instance_owner.lease_generation)
        return false;

    const auto local = status ();
    if (snapshot->allocation.target.mesh_name != _options.mesh.descriptor.mesh_name
        || snapshot->allocation.target.node_rid.value ()
             != node_rid_t::from_string (local.routing_id ().to_string ()).value ()
        || snapshot->allocation.target.node_lifecycle_generation
             != local.lifecycle_generation ())
        return false;

    const auto ready = decode_instance_ready_state (snapshot->payload);
    if (!ready || ready->stable_type != stable_type
        || ready->spot_id != spot_id
        || ready->object_generation != object_generation
        || ready->authority_owner_generation != authority_owner_generation
        || !ready->completed || !ready->recovery_reference.empty ())
        return false;

    const auto sealed = store
      ->compare_exchange_authority (
        authority_key, snapshot->store_version,
        authority_put_t{
          encode_instance_closing_state (
            instance_closing_state_t{
              stable_type, spot_id, object_generation,
              authority_owner_generation}),
          authority_generation_transition_t::preserve})
      .result ().value ();
    const auto *closing = std::get_if<authority_stored_t> (&sealed);
    if (!closing)
        return false;

    bool local_closed = false;
    try {
        local_closed = close_local ();
    }
    catch (...) {
        /* try_close_idle marks the local context closed before invoking the
         * application callback. A callback exception therefore still leaves
         * the local instance unavailable and the sealed authority must not be
         * reopened. */
        local_closed = true;
    }
    if (!local_closed) {
        (void) store
          ->compare_exchange_authority (
            authority_key, closing->snapshot.store_version,
            authority_put_t{
              snapshot->payload,
              authority_generation_transition_t::preserve})
          .result ();
        return false;
    }

    const auto deleted = store
      ->compare_exchange_authority (
        authority_key, closing->snapshot.store_version, authority_delete_t{})
      .result ().value ();
    return std::holds_alternative<authority_deleted_t> (deleted);
}

void public_host_runtime_t::configure_session_route_owner (
  std::function<std::optional<location_owner_token_t> ()>
    owner_resolver)
{
    if (!owner_resolver)
        throw std::invalid_argument (
          "Session route owner resolver is required");
    std::lock_guard lock (_mutex);
    _session_route_owner_resolver = std::move (owner_resolver);
}

void public_host_runtime_t::configure_session_relocation_store (
  std::shared_ptr<stateful::relocation_store_port_t> relocations)
{
    if (!relocations)
        throw std::invalid_argument (
          "Session relocation Store is required");
    std::lock_guard lock (_mutex);
    if (_started)
        throw std::logic_error (
          "Session relocation Store must be configured before start");
    _session_relocations = std::move (relocations);
}

bool public_host_runtime_t::seal_session_remote (
  const zlink::routing_id_t &session_owner_node,
  protocol::session_relocation_seal_t seal,
  std::chrono::milliseconds timeout,
  session_relocation_journal_capture_t capture_journal,
  session_relocation_seal_completion_t completion)
{
    if (!capture_journal || !completion)
        throw std::invalid_argument (
          "Session relocation seal requires journal capture and completion callbacks");
    const auto relocation_key = std::pair{
      seal.relocation.high, seal.relocation.low};
    std::optional<session_relocation_seal_result_t> cached_result;
    std::shared_ptr<stateful::relocation_store_port_t> relocations;
    {
        std::lock_guard lock (_mutex);
        const auto cached =
          _session_journal_terminals.find (relocation_key);
        if (cached != _session_journal_terminals.end ()) {
            if (cached->second.first != seal)
                return false;
            cached_result = cached->second.second;
        }
        relocations = _session_relocations;
    }
    if (cached_result) {
        completion (foundation::operation_terminal_t::completed,
                    std::move (cached_result));
        return true;
    }
    if (!relocations)
        return false;
    const auto expected = seal;
    const auto host = shared_from_this ();
    return _transport->request_session_relocation_seal (
      session_owner_node.to_bytes (), seal, timeout,
      [host, expected, relocation_key,
       relocations = std::move (relocations),
       capture_journal = std::move (capture_journal),
       completion = std::move (completion)] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> payload) mutable {
          if (terminal
              != foundation::operation_terminal_t::completed) {
              completion (terminal, std::nullopt);
              return;
          }
          try {
              const auto sealed =
                protocol::decode_session_relocation_sealed (payload);
              if (sealed.relocation != expected.relocation
                  || sealed.coordinator != expected.coordinator
                  || sealed.actor != expected.actor
                  || sealed.session_owner_node_routing_id
                       != expected.session_owner_node_routing_id
                  || sealed.session_owner_node_generation
                       != expected.session_owner_node_generation
                  || sealed.session_owner_id
                       != expected.session_owner_id
                  || sealed.session_owner_lease_generation
                       != expected.session_owner_lease_generation
                  || sealed.session_routing_id
                       != expected.session_routing_id
                  || sealed.binding_generation
                       != expected.binding_generation) {
                  completion (
                    foundation::operation_terminal_t::transport_failed,
                    std::nullopt);
                  return;
              }
              stateful::durable_session_journal_record_t record{
                expected.relocation.high,
                expected.relocation.low,
                stateful::object_ref_t{
                  stateful::object_kind_t::actor,
                  expected.actor.actor_id,
                  expected.actor.object_generation,
                  expected.actor.authority_owner_generation,
                  {},
                  zlink::routing_id_t::from (
                    expected.actor.target_node_routing_id)
                    .to_string ()},
                expected.binding_generation,
                sealed.last_accepted_session_sequence,
                capture_journal ()};
              stateful::durable_session_journal_store_t journal_store (
                relocations);
              const auto root = journal_store.prepare (record);
              const auto recovered = journal_store.recover (root);
              if (!recovered || *recovered != record) {
                  journal_store.cleanup (root);
                  completion (
                    foundation::operation_terminal_t::transport_failed,
                    std::nullopt);
                  return;
              }
              const session_relocation_seal_result_t result{
                sealed, root};
              std::optional<session_relocation_seal_result_t>
                existing_result;
              bool conflicting_terminal = false;
              {
                  std::lock_guard lock (host->_mutex);
                  if (host->_session_journal_terminals.size ()
                        >= 65'536
                      && !host->_session_journal_terminals.contains (
                        relocation_key))
                      host->_session_journal_terminals.erase (
                        host->_session_journal_terminals.begin ());
                  const auto [stored, inserted] =
                    host->_session_journal_terminals.emplace (
                      relocation_key,
                      std::pair{expected, result});
                  if (!inserted) {
                      journal_store.cleanup (root);
                      conflicting_terminal =
                        stored->second.first != expected;
                      if (!conflicting_terminal)
                          existing_result = stored->second.second;
                  }
              }
              if (conflicting_terminal) {
                  completion (
                    foundation::operation_terminal_t::transport_failed,
                    std::nullopt);
                  return;
              }
              if (existing_result) {
                  completion (terminal, std::move (existing_result));
                  return;
              }
              completion (
                terminal, result);
          }
          catch (...) {
              completion (
                foundation::operation_terminal_t::transport_failed,
                std::nullopt);
          }
      });
}

bool public_host_runtime_t::activate_instance_spot_remote (
  const zlink::routing_id_t &target_node,
  protocol::instance_spot_activation_header_t request,
  std::optional<std::vector<std::uint8_t>> metadata,
  protocol::application_payload_t application_payload,
  std::chrono::milliseconds timeout,
  instance_spot_activation_completion_t completion)
{
    if (!completion)
        throw std::invalid_argument (
          "Instance Spot activation completion is required");
    return _transport->request_instance_spot_activation (
      target_node.to_bytes (), std::move (request), std::move (metadata),
      std::move (application_payload), timeout,
      [completion = std::move (completion)] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> packed) mutable {
          protocol::reply_header_t reply{};
          std::optional<protocol::application_payload_t>
            application_reply;
          if (terminal == foundation::operation_terminal_t::completed) {
              try {
                  const auto parts = unpack_infrastructure_reply (packed);
                  reply = protocol::decode_reply_header (parts.front ());
                  if (parts.size () == 2)
                      application_reply =
                        protocol::decode_application_payload (parts[1]);
              }
              catch (const protocol::service_wire_error_t &) {
                  terminal = foundation::operation_terminal_t::transport_failed;
              }
          }
          completion (terminal, reply, std::move (application_reply));
      });
}

bool public_host_runtime_t::send_instance_spot_activation_remote (
  const zlink::routing_id_t &target_node,
  protocol::instance_spot_activation_header_t request,
  std::optional<std::vector<std::uint8_t>> metadata,
  protocol::application_payload_t application_payload)
{
    return _transport->send_instance_spot_activation (
      target_node.to_bytes (), std::move (request), std::move (metadata),
      std::move (application_payload));
}

bool public_host_runtime_t::prepare_relocation_remote (
  const zlink::routing_id_t &target_node,
  protocol::relocation_prepare_t prepare,
  std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero ())
        return false;
    const relocation_attempt_key_t key{
      prepare.relocation.high,
      prepare.relocation.low,
      prepare.target_attempt_generation};
    {
        std::lock_guard lock (_mutex);
        _relocation_ready_responses.erase (key);
    }
    if (!_transport->send_relocation_control (
          target_node.to_bytes (), prepare))
        return false;

    protocol::relocation_ready_t ready;
    {
        std::unique_lock lock (_mutex);
        if (!_relocation_changed.wait_for (
              lock, timeout, [&] {
                  return _relocation_ready_responses.contains (key)
                         || !_started;
              }))
            return false;
        const auto found = _relocation_ready_responses.find (key);
        if (found == _relocation_ready_responses.end ())
            return false;
        ready = found->second;
        _relocation_ready_responses.erase (found);
    }
    if (ready.relocation != prepare.relocation
        || ready.target_attempt_generation
             != prepare.target_attempt_generation
        || ready.round != prepare.round
        || ready.coordinator != prepare.coordinator
        || ready.candidate != prepare.candidate
        || !same_relocation_wire_object (
             ready.object, prepare.object)
        || ready.role != protocol::relocation_role_t::target
        || ready.root != prepare.root
        || !ready.participants.empty ()
        || ready.offered_messages
             < std::max<std::uint64_t> (
               1, prepare.required_messages)
        || ready.offered_bytes
             < std::max<std::uint64_t> (1, prepare.required_bytes)
        || ready.reservation_generation == 0)
        return false;
    return _transport->send_relocation_control (
      target_node.to_bytes (),
      protocol::relocation_reserved_t{
        ready.relocation,
        ready.target_attempt_generation,
        ready.round,
        ready.coordinator,
        ready.candidate,
        ready.reservation_generation,
        prepare.participants});
}

bool public_host_runtime_t::complete_relocation_remote (
  const zlink::routing_id_t &target_node,
  protocol::relocation_complete_t complete,
  protocol::request_source_fence_t expected_target,
  std::chrono::milliseconds timeout,
  bool wait_for_target)
{
    const relocation_attempt_key_t key{
      complete.relocation.high,
      complete.relocation.low,
      complete.target_attempt_generation};
    if (wait_for_target) {
        std::lock_guard lock (_mutex);
        _relocation_complete_responses.erase (key);
        _relocation_complete_responses.emplace (
          key,
          relocation_completion_wait_t{
            complete.coordinator,
            std::move (expected_target),
            complete.source_cleanup_state});
    }
    if (!_transport->send_relocation_control (
          target_node.to_bytes (), complete)) {
        if (wait_for_target) {
            std::lock_guard lock (_mutex);
            _relocation_complete_responses.erase (key);
        }
        return false;
    }
    if (!wait_for_target)
        return true;
    std::unique_lock lock (_mutex);
    if (!_relocation_changed.wait_for (
          lock, timeout, [&] {
              const auto found =
                _relocation_complete_responses.find (key);
              return (found != _relocation_complete_responses.end ()
                      && found->second.accepted)
                     || !_started;
          })) {
        _relocation_complete_responses.erase (key);
        return false;
    }
    const auto found = _relocation_complete_responses.find (key);
    const auto accepted =
      found != _relocation_complete_responses.end ()
      && found->second.accepted;
    _relocation_complete_responses.erase (key);
    return accepted;
}

bool public_host_runtime_t::complete_relocated_source (
  const stateful::object_ref_t &owner,
  std::uint64_t sequence,
  const protocol::reply_relay_t &relay,
  const std::optional<protocol::application_payload_t> &reply)
{
    return _stateful_dispatch
             && _stateful_dispatch->complete_relocated_source (
               owner, sequence, relay, reply);
}

bool public_host_runtime_t::acknowledge_relocated_source (
  const stateful::object_ref_t &owner,
  const protocol::wire_operation_id_t &operation)
{
    return _stateful_dispatch
           && _stateful_dispatch->acknowledge_relocated_source (
             owner, operation);
}

stateful::stateful_error_t public_host_runtime_t::ingest_stateful (
  const stateful::object_ref_t &owner)
{
    return _stateful_dispatch
             ? _stateful_dispatch->ingest (owner)
             : stateful::stateful_error_t::invalid;
}

bool public_host_runtime_t::route_session_remote (
  const zlink::routing_id_t &session_owner_node,
  protocol::session_relocation_route_t route,
  std::chrono::milliseconds timeout,
  session_relocation_route_completion_t completion)
{
    if (!completion)
        throw std::invalid_argument (
          "Session relocation route completion is required");
    const auto expected = route;
    return _transport->request_session_relocation_route (
      session_owner_node.to_bytes (), route, timeout,
      [expected, completion = std::move (completion)] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> payload) mutable {
          if (terminal
              != foundation::operation_terminal_t::completed) {
              completion (terminal, std::nullopt);
              return;
          }
          try {
              const auto ack =
                protocol::decode_session_relocation_routed (
                  payload);
              const auto expected_authority_generation =
                expected.route.action
                    == protocol::session_relocation_route_action_t::commit
                  ? expected.route
                      .target_authority_owner_generation
                  : expected.route
                      .current_authority_owner_generation;
              const auto expected_high_water =
                expected.route.action
                    == protocol::session_relocation_route_action_t::commit
                  ? expected.route.replayed_high_water
                  : ack.last_accepted_session_sequence;
              if (ack.relocation != expected.relocation
                  || ack.coordinator != expected.coordinator
                  || ack.actor != expected.actor
                  || ack.session_owner_node_routing_id
                       != expected.session_owner_node_routing_id
                  || ack.session_owner_node_generation
                       != expected.session_owner_node_generation
                  || ack.session_owner_id
                       != expected.session_owner_id
                  || ack.session_owner_lease_generation
                       != expected.session_owner_lease_generation
                  || ack.session_routing_id
                       != expected.session_routing_id
                  || ack.binding_generation
                       != expected.binding_generation
                  || ack.action != expected.route.action
                  || ack.current_authority_owner_generation
                       != expected_authority_generation
                  || ack.last_accepted_session_sequence
                       != expected_high_water) {
                  completion (
                    foundation::operation_terminal_t::transport_failed,
                    std::nullopt);
                  return;
              }
              completion (terminal, ack);
          }
          catch (const protocol::service_wire_error_t &) {
              completion (
                foundation::operation_terminal_t::transport_failed,
                std::nullopt);
          }
      });
}

std::size_t public_host_runtime_t::recover_instance_spot_activations ()
{
    std::shared_ptr<zlink::framework::location_repository_t> store;
    std::shared_ptr<stateful::relocation_store_port_t> relocations;
    instance_spot_activation_materializer_t materializer;
    {
        std::lock_guard lock (_mutex);
        store = _user_spot_store;
        relocations = _instance_spot_relocations;
        materializer = _instance_spot_materializer;
    }
    if (!store || !relocations || !materializer)
        return 0;
    const auto local = _transport->topology ().local_descriptor ();
    std::size_t recovered = 0;
    std::optional<authority_scan_cursor_t> cursor;
    do {
        const auto scanned = store
          ->list_authorities (
            std::to_string (static_cast<int> (
              placement_object_kind_t::instance_spot)) + ":",
            cursor, 256)
          .result ().value ();
        const auto *page = std::get_if<authority_page_t> (&scanned);
        if (!page)
            break;
        for (const auto &entry : page->items) {
            if (const auto closing = decode_instance_closing_state (
                  entry.snapshot.payload);
                closing
                && entry.snapshot.allocation.object_kind
                     == placement_object_kind_t::instance_spot
                && entry.snapshot.allocation.state
                     == placement_allocation_state_t::active
                && entry.snapshot.allocation.stable_type
                     == closing->stable_type
                && entry.snapshot.allocation.target.node_rid.value ()
                     == node_rid_t::from_string (
                          zlink::routing_id_t::from (
                            local.node_routing_id).to_string ())
                          .value ()
                && entry.snapshot.allocation.target
                     .node_lifecycle_generation
                     == local.lifecycle_generation
                && entry.snapshot.object_generation
                     == closing->object_generation
                && entry.snapshot.authority_owner_generation
                     == closing->authority_owner_generation) {
                const auto deleted = store
                  ->compare_exchange_authority (
                    entry.key, entry.snapshot.store_version,
                    authority_delete_t{})
                  .result ().value ();
                if (std::holds_alternative<authority_deleted_t> (deleted))
                    ++recovered;
                continue;
            }
            const auto state = decode_instance_ready_state (
              entry.snapshot.payload);
            if (!state || state->recovery_reference.empty ()
                || entry.snapshot.allocation.object_kind
                     != placement_object_kind_t::instance_spot
                || entry.snapshot.allocation.target.node_rid.value ()
                     != node_rid_t::from_string (
                          zlink::routing_id_t::from (
                            local.node_routing_id).to_string ())
                          .value ()
                || entry.snapshot.allocation.target
                     .node_lifecycle_generation
                     != local.lifecycle_generation)
                continue;
            const auto payload = relocations->get (
              state->recovery_reference);
            if (!payload
                || stateful::maintenance_runtime_t::crc32c (*payload)
                     != state->recovery_checksum)
                continue;
            protocol::instance_activation_recovery_t recovery;
            try {
                recovery =
                  protocol::decode_instance_activation_recovery (*payload);
            }
            catch (const protocol::service_wire_error_t &) {
                continue;
            }
            auto updated = *state;
            if (!updated.completed) {
                bool prepared = false;
                try {
                    prepared = materializer.prepare (
                      recovery.activation);
                }
                catch (...) {
                    prepared = false;
                }
                if (!prepared)
                    continue;
                auto result = materializer.dispatch (
                  recovery.activation, recovery.metadata,
                  recovery.application_payload);
                updated.completed = true;
                updated.terminal_result = result.terminal_result;
                updated.failure_code = result.failure_code;
                updated.reply = std::move (result.application_reply);
                const auto terminal = store
                  ->compare_exchange_authority (
                    entry.key, entry.snapshot.store_version,
                    authority_restore_t{
                      encode_instance_ready_state (updated),
                      entry.snapshot.owner})
                  .result ().value ();
                const auto *stored =
                  std::get_if<authority_stored_t> (&terminal);
                if (!stored)
                    continue;
                updated.recovery_reference.clear ();
                updated.recovery_checksum = 0;
                const auto cleared = store
                  ->compare_exchange_authority (
                    entry.key, stored->snapshot.store_version,
                    authority_restore_t{
                      encode_instance_ready_state (updated),
                      stored->snapshot.owner})
                  .result ().value ();
                if (!std::holds_alternative<authority_stored_t> (
                      cleared))
                    continue;
            }
            else {
                updated.recovery_reference.clear ();
                updated.recovery_checksum = 0;
                const auto cleared = store
                  ->compare_exchange_authority (
                    entry.key, entry.snapshot.store_version,
                    authority_restore_t{
                      encode_instance_ready_state (updated),
                      entry.snapshot.owner})
                  .result ().value ();
                if (!std::holds_alternative<authority_stored_t> (
                      cleared))
                    continue;
            }
            relocations->remove (state->recovery_reference);
            ++recovered;
        }
        cursor = page->next_cursor;
    } while (cursor);
    return recovered;
}

bool public_host_runtime_t::create_user_spot_remote (
  const zlink::routing_id_t &target_node,
  protocol::user_spot_create_header_t request,
  std::chrono::milliseconds timeout,
  user_spot_create_completion_t completion)
{
    if (!completion)
        throw std::invalid_argument (
          "User Spot create completion is required");
    return _transport->request_user_spot_create (
      target_node.to_bytes (), std::move (request), timeout,
      [completion = std::move (completion)] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> packed) mutable {
          protocol::user_spot_create_reply_t reply;
          std::optional<protocol::application_payload_t>
            application_reply;
          if (terminal
              == foundation::operation_terminal_t::completed) {
              try {
                  const auto parts =
                    unpack_infrastructure_reply (packed);
                  reply =
                    protocol::decode_user_spot_create_reply (
                      parts.front ());
                  if (parts.size () == 2)
                      application_reply =
                        protocol::decode_application_payload (parts[1]);
              }
              catch (const protocol::service_wire_error_t &) {
                  terminal =
                    foundation::operation_terminal_t::transport_failed;
              }
          }
          completion (
            terminal, std::move (reply),
            std::move (application_reply));
      });
}

bool public_host_runtime_t::create_actor_remote (
  const zlink::routing_id_t &target_node,
  protocol::actor_create_header_t request,
  std::chrono::milliseconds timeout,
  actor_create_operation_completion_t completion)
{
    if (!completion)
        throw std::invalid_argument (
          "Actor create completion is required");
    if (target_node == status ().routing_id ()) {
        actor_create_operation_target_t target;
        {
            std::lock_guard lock (_mutex);
            target = _actor_create_target;
        }
        if (!target)
            return false;
        auto completed = std::make_shared<std::atomic_bool> (false);
        auto forward = [completion = std::move (completion), completed] (
                         actor_create_operation_result_t result) mutable {
            if (completed->exchange (true, std::memory_order_acq_rel))
                return;
            completion (
              foundation::operation_terminal_t::completed,
              std::move (result.reply), std::move (result.application_reply));
        };
        try {
            target (request, forward);
        }
        catch (const std::exception &) {
            actor_create_operation_result_t result;
            result.reply.header = {
              request.correlation, 105u,
              static_cast<std::uint32_t> (
                protocol::framework_error_code::actorCreateFailed)};
            forward (std::move (result));
        }
        catch (...) {
            actor_create_operation_result_t result;
            result.reply.header = {
              request.correlation, 105u,
              static_cast<std::uint32_t> (
                protocol::framework_error_code::actorCreateFailed)};
            forward (std::move (result));
        }
        return true;
    }
    return _transport->request_actor_create (
      target_node.to_bytes (), std::move (request), timeout,
      [completion = std::move (completion)] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> packed) mutable {
          protocol::actor_create_reply_t reply;
          std::optional<protocol::application_payload_t>
            application_reply;
          if (terminal
              == foundation::operation_terminal_t::completed) {
              try {
                  const auto parts =
                    unpack_infrastructure_reply (packed);
                  reply = protocol::decode_actor_create_reply (
                    parts.front ());
                  if (parts.size () == 2)
                      application_reply =
                        protocol::decode_application_payload (
                          parts[1]);
              }
              catch (const protocol::service_wire_error_t &) {
                  terminal =
                    foundation::operation_terminal_t::transport_failed;
              }
          }
          completion (terminal, std::move (reply),
                      std::move (application_reply));
      });
}

bool public_host_runtime_t::close_user_spot_remote (
  const zlink::routing_id_t &target_node,
  protocol::user_spot_close_header_t request,
  std::chrono::milliseconds timeout,
  user_spot_close_completion_t completion)
{
    if (!completion)
        throw std::invalid_argument (
          "User Spot close completion is required");
    return _transport->request_user_spot_close (
      target_node.to_bytes (), std::move (request), timeout,
      [completion = std::move (completion)] (
        foundation::operation_terminal_t terminal,
        std::vector<std::uint8_t> packed) mutable {
          protocol::user_spot_close_reply_t reply;
          if (terminal
              == foundation::operation_terminal_t::completed) {
              try {
                  const auto parts =
                    unpack_infrastructure_reply (packed);
                  if (parts.size () != 1)
                      throw protocol::service_wire_error_t (
                        "User Spot close reply carries a payload");
                  reply =
                    protocol::decode_user_spot_close_reply (
                      parts.front ());
              }
              catch (const protocol::service_wire_error_t &) {
                  terminal =
                    foundation::operation_terminal_t::transport_failed;
              }
          }
          completion (terminal, std::move (reply));
      });
}

spot_handle_t public_host_runtime_t::entry_spot ()
{
    return get_or_create_spot (_entry_spot_id);
}

spot_handle_t public_host_runtime_t::get_or_create_spot (
  std::string spot_id)
{
    const auto &key = spot_id;
    {
        std::lock_guard lock (_mutex);
        const auto found = _spots.find (key);
        if (found != _spots.end ()) {
            return spot_handle_t (shared_from_this (), found->second);
        }
    }
    auto created = _objects.begin_create (
      stateful::create_request_t{
        stateful::object_kind_t::user_spot,
        key,
        "framework.spot",
        _options.mesh.descriptor.mesh_name,
        {},
        false,
        false});
    if (created.error != stateful::stateful_error_t::none) {
        throw std::runtime_error ("framework Spot authority creation failed");
    }
    if (created.factory_owner
        && _objects.commit_create (created.attempt)
             != stateful::stateful_error_t::none) {
        throw std::runtime_error ("framework Spot Ready commit failed");
    }
    auto object = _objects.find (stateful::object_kind_t::user_spot, key);
    if (!object) {
        throw std::runtime_error ("framework Spot authority is unavailable");
    }
    {
        std::lock_guard lock (_mutex);
        _spots.insert_or_assign (key, *object);
    }
    return spot_handle_t (shared_from_this (), *object);
}

actor_handle_t public_host_runtime_t::create_actor (
  std::string actor_type,
  std::string actor_id)
{
    {
        std::lock_guard lock (_mutex);
        const auto found = _actors.find (actor_id);
        if (found != _actors.end ()) {
            return actor_handle_t (
              shared_from_this (),
              framework_actor_ref (found->second.second, found->second.first),
              found->second.second);
        }
    }
    auto created = _objects.begin_create (
      stateful::create_request_t{
        stateful::object_kind_t::actor,
        actor_id,
        actor_type,
        _options.mesh.descriptor.mesh_name,
        {},
        false,
        false});
    if (created.error != stateful::stateful_error_t::none) {
        throw std::runtime_error ("framework Actor authority creation failed");
    }
    if (created.factory_owner
        && _objects.commit_create (created.attempt)
             != stateful::stateful_error_t::none) {
        throw std::runtime_error ("framework Actor Ready commit failed");
    }
    auto object = _objects.find (stateful::object_kind_t::actor, actor_id);
    if (!object) {
        throw std::runtime_error ("framework Actor authority is unavailable");
    }
    {
        std::lock_guard lock (_mutex);
        _actors.insert_or_assign (
          actor_id, std::make_pair (actor_type, *object));
    }
    return actor_handle_t (
      shared_from_this (), framework_actor_ref (*object, actor_type), *object);
}

actor_handle_t public_host_runtime_t::create_reserved_actor (
  std::string actor_type,
  stateful::object_ref_t reserved)
{
    if (reserved.kind != stateful::object_kind_t::actor)
        throw std::invalid_argument (
          "reserved Actor reference has an invalid object kind");
    {
        std::lock_guard lock (_mutex);
        const auto found = _actors.find (reserved.key);
        if (found != _actors.end ()) {
            if (found->second.second.object_generation
                  != reserved.object_generation
                || found->second.second.authority_owner_generation
                     != reserved.authority_owner_generation)
            {
                const auto adopted = _objects.adopt_reserved_actor_owner (
                  reserved, actor_type);
                if (adopted == stateful::stateful_error_t::none) {
                    found->second.second = reserved;
                    return actor_handle_t (
                      shared_from_this (),
                      framework_actor_ref (reserved, found->second.first),
                      reserved);
                }
                throw std::runtime_error (
                  "reserved Actor generation does not match the local Actor");
            }
            return actor_handle_t (
              shared_from_this (),
              framework_actor_ref (found->second.second, found->second.first),
              found->second.second);
        }
    }
    auto created = _objects.begin_reserved_object (
      reserved, actor_type, {});
    if (created.error != stateful::stateful_error_t::none)
        throw std::runtime_error (
          "reserved framework Actor authority creation failed");
    if (created.factory_owner
        && _objects.commit_create (created.attempt)
             != stateful::stateful_error_t::none)
        throw std::runtime_error (
          "reserved framework Actor Ready commit failed");
    auto object = _objects.find (
      stateful::object_kind_t::actor, reserved.key);
    if (!object)
        throw std::runtime_error (
          "reserved framework Actor authority is unavailable");
    {
        std::lock_guard lock (_mutex);
        _actors.insert_or_assign (
          reserved.key, std::make_pair (actor_type, *object));
    }
    return actor_handle_t (
      shared_from_this (), framework_actor_ref (*object, actor_type), *object);
}

std::optional<route_fence_t>
public_host_runtime_t::resolve_spot_route_fence (
  const zlink::routing_id_t &target_node_rid,
  std::string_view target_spot_id,
  std::uint64_t target_spot_generation)
{
    spot_route_fence_resolver_t resolver;
    std::shared_ptr<zlink::framework::location_repository_t> store;
    {
        std::lock_guard lock (_mutex);
        resolver = _spot_route_fence_resolver;
        store = _user_spot_store;
    }
    if (resolver) {
        try {
            return resolver (
              target_node_rid, target_spot_id, target_spot_generation);
        }
        catch (...) {
            return std::nullopt;
        }
    }

    const auto key = spot_route_cache_key (
      target_node_rid, target_spot_id, target_spot_generation);
    {
        std::lock_guard lock (_route_cache_mutex);
        const auto found = _spot_route_fences.find (key);
        if (found != _spot_route_fences.end ()) {
            if (std::chrono::steady_clock::now ()
                < found->second.expires_at)
                return found->second.fence;
            _spot_route_fences.erase (found);
        }
    }

    const auto fence = read_route_owner_fence (
      store, '2', target_spot_id, target_spot_generation);
    if (fence && _options.route_cache_max_age
                     > std::chrono::milliseconds::zero ()) {
        std::lock_guard lock (_route_cache_mutex);
        _spot_route_fences.insert_or_assign (
          key,
          cached_spot_route_fence_t{
            *fence,
            std::chrono::steady_clock::now ()
              + _options.route_cache_max_age});
    }
    return fence;
}

void public_host_runtime_t::invalidate_spot_route_fence (
  const protocol::message_follow_notice_t &notice)
{
    const auto *source = std::get_if<protocol::spot_route_fence_t> (
      &notice.source);
    if (!source)
        return;
    const auto target_node = zlink::routing_id_t::from (
      source->target_node_routing_id);
    const auto key = spot_route_cache_key (
      target_node, source->spot_id, source->object_generation);
    std::lock_guard lock (_route_cache_mutex);
    const auto found = _spot_route_fences.find (key);
    if (found == _spot_route_fences.end ())
        return;

    // A late notice may describe an older owner lease. Do not remove a
    // route that was published after that notice.
    const route_fence_t source_fence{
      source->authority_owner_generation,
      source->owner_lease_generation};
    if (found->second.fence == source_fence)
        _spot_route_fences.erase (found);
}

zlink::submit_result_t public_host_runtime_t::send_to_actor (
  const actor_ref_t &target,
  const std::vector<zlink::message_t> &parts,
  std::span<const std::uint8_t> metadata,
  std::uint64_t authority_owner_generation,
  std::uint64_t owner_lease_generation)
{
    const auto target_routing_id =
      zlink::routing_id_t::from (
        std::string (target.node_rid ().value ()));
    if (target_routing_id.to_bytes ()
          == status ().routing_id ().to_bytes ()
        && resolve_actor (target)) {
        return submitted (enqueue_local_actor_message (
          target, record_kind_t::actor_send, parts));
    }
    const auto peer = _transport->topology ().peer (
      target_routing_id.to_bytes ());
    const auto node_generation =
      peer ? peer->descriptor.lifecycle_generation
           : status ().lifecycle_generation ();
    const auto object = resolve_actor (target);
    const auto authority_generation = authority_owner_generation != 0
      ? authority_owner_generation
      : object ? object->authority_owner_generation : target.object_generation ();
    const auto route_fence = read_route_owner_fence (
      _user_spot_store, '1', target.actor_id ().value (), target.object_generation (),
      authority_generation, owner_lease_generation);
    if (!route_fence || route_fence->first != authority_generation)
        return zlink::submit_result_t::not_found;
    return submitted (_transport->send_to_actor (
      zlink::routing_id_t::from (
        std::string (target.node_rid ().value ())).to_bytes (), std::nullopt,
      protocol::actor_route_fence_t{
        std::string (target.actor_id ().value ()),
        target.object_generation (),
        zlink::routing_id_t::from (
          std::string (target.node_rid ().value ())).to_bytes (),
        node_generation,
        authority_generation,
        route_fence->second},
      encode_application (parts, metadata)));
}

zlink::submit_result_t public_host_runtime_t::request_to_actor (
  const actor_ref_t &target,
  const std::vector<zlink::message_t> &parts,
  operation_id_t &operation,
  std::chrono::milliseconds timeout,
  std::span<const std::uint8_t> metadata,
  std::uint64_t authority_owner_generation,
  std::uint64_t owner_lease_generation)
{
    operation = next_operation ();
    if (!try_reserve_completion (operation))
        return zlink::submit_result_t::backpressured;
    const auto target_routing_id =
      zlink::routing_id_t::from (
        std::string (target.node_rid ().value ()));
    if (target_routing_id.to_bytes ()
          == status ().routing_id ().to_bytes ()
        && resolve_actor (target)) {
        const auto accepted = enqueue_local_actor_message (
          target, record_kind_t::actor_request, parts, operation);
        if (!accepted)
            release_completion (operation);
        return submitted (accepted);
    }
    const auto peer = _transport->topology ().peer (
      target_routing_id.to_bytes ());
    const auto node_generation =
      peer ? peer->descriptor.lifecycle_generation
           : status ().lifecycle_generation ();
    const auto object = resolve_actor (target);
    const auto authority_generation = authority_owner_generation != 0
      ? authority_owner_generation
      : object ? object->authority_owner_generation : target.object_generation ();
    const auto route_fence = read_route_owner_fence (
      _user_spot_store, '1', target.actor_id ().value (), target.object_generation (),
      authority_generation, owner_lease_generation);
    if (!route_fence || route_fence->first != authority_generation) {
        release_completion (operation);
        return zlink::submit_result_t::not_found;
    }
    const auto host = shared_from_this ();
    const auto accepted = _transport->request_to_actor (
      zlink::routing_id_t::from (
        std::string (target.node_rid ().value ())).to_bytes (), std::nullopt,
      protocol::actor_route_fence_t{
        std::string (target.actor_id ().value ()),
        target.object_generation (),
        zlink::routing_id_t::from (
          std::string (target.node_rid ().value ())).to_bytes (),
        node_generation,
        authority_generation,
        route_fence->second},
      encode_application (parts, metadata), timeout,
      [host, operation] (foundation::operation_terminal_t terminal,
                         std::vector<std::uint8_t> payload) mutable {
          host->complete_operation (
            operation, operation_kind_t::none, terminal,
            std::move (payload));
      }, protocol::wire_operation_id_t{operation.high, operation.low});
    if (!accepted)
        release_completion (operation);
    return submitted (accepted);
}

zlink::submit_result_t public_host_runtime_t::send_to_node (
  const zlink::routing_id_t &target,
  const std::vector<zlink::message_t> &parts)
{
    const auto target_bytes = target.to_bytes ();
    if (_transport->send_to_node (target_bytes, encode_application (parts)))
        return zlink::submit_result_t::ok;
    return _transport->topology ().peer (target_bytes)
      ? zlink::submit_result_t::backpressured
      : zlink::submit_result_t::not_connected;
}

zlink::submit_result_t public_host_runtime_t::request_to_node (
  const zlink::routing_id_t &target,
  const std::vector<zlink::message_t> &parts,
  operation_id_t &operation,
  std::chrono::milliseconds timeout)
{
    operation = next_operation ();
    if (!try_reserve_completion (operation))
        return zlink::submit_result_t::backpressured;
    const auto host = shared_from_this ();
    const auto accepted = _transport->request_to_node (
      target.to_bytes (), encode_application (parts), timeout,
      [host, operation] (foundation::operation_terminal_t terminal,
                         std::vector<std::uint8_t> payload) mutable {
          host->complete_operation (
            operation, operation_kind_t::none, terminal,
            std::move (payload));
      });
    if (!accepted)
        release_completion (operation);
    return submitted (accepted);
}

zlink::submit_result_t public_host_runtime_t::send_to_channel (
  const std::string &channel_name,
  const std::vector<zlink::message_t> &parts)
{
    return submitted (_transport->send_to_channel (
      channel_name, encode_application (parts)));
}

zlink::submit_result_t public_host_runtime_t::request_to_channel (
  const std::string &channel_name,
  const std::vector<zlink::message_t> &parts,
  operation_id_t &operation,
  std::chrono::milliseconds timeout)
{
    operation = next_operation ();
    if (!try_reserve_completion (operation))
        return zlink::submit_result_t::backpressured;
    const auto host = shared_from_this ();
    const auto accepted = _transport->request_to_channel (
      channel_name, encode_application (parts), timeout,
      [host, operation] (foundation::operation_terminal_t terminal,
                         std::vector<std::uint8_t> payload) mutable {
          host->complete_operation (
            operation, operation_kind_t::none, terminal,
            std::move (payload));
      });
    if (!accepted)
        release_completion (operation);
    return submitted (accepted);
}

std::vector<public_host_runtime_t::relocation_target_attempt_t>
public_host_runtime_t::take_expired_relocation_target_attempts_locked (
  std::chrono::steady_clock::time_point now)
{
    std::vector<relocation_target_attempt_t> expired;
    for (auto current = _relocation_target_attempts.begin ();
         current != _relocation_target_attempts.end ();) {
        const auto &attempt = current->second;
        if (!attempt.target_finalized
            && attempt.attempt_expires_at !=
                 std::chrono::steady_clock::time_point{}
            && attempt.attempt_expires_at <= now) {
            expired.push_back (std::move (current->second));
            current = _relocation_target_attempts.erase (current);
        } else {
            ++current;
        }
    }
    return expired;
}

void public_host_runtime_t::cleanup_expired_relocation_target_attempts (
  std::vector<relocation_target_attempt_t> attempts) noexcept
{
    for (auto &attempt : attempts) {
        if (relocation_target_authority_committed (attempt)) {
            attempt.attempt_expires_at = {};
            const relocation_attempt_key_t key{
              attempt.prepare.relocation.high,
              attempt.prepare.relocation.low,
              attempt.prepare.target_attempt_generation};
            std::lock_guard lock (_mutex);
            if (!_relocation_target_attempts.contains (key))
                _relocation_target_attempts.emplace (
                  key, std::move (attempt));
            continue;
        }
        for (const auto &participant : attempt.prepare.participants) {
            try {
                (void) _relocation_wire->unregister_target (
                  attempt.prepare.relocation,
                  attempt.prepare.target_attempt_generation,
                  participant.participant_id);
            }
            catch (...) {
            }
        }
        try {
            if (attempt.targets.size () == 1)
                (void) _objects.abort_relocation_restore (
                  attempt.targets.front (), attempt.restore_identity);
            else
                (void) _objects.abort_relocation_restore_aggregate (
                  attempt.targets, attempt.restore_identity);
        }
        catch (...) {
        }
        if (_stateful_dispatch)
            for (const auto &target : attempt.targets) {
                try {
                    (void) _stateful_dispatch->discard_pending (target);
                }
                catch (...) {
                }
            }
    }
}

void public_host_runtime_t::expire_relocation_target_attempts ()
{
    std::vector<relocation_target_attempt_t> expired;
    {
        std::lock_guard lock (_mutex);
        expired = take_expired_relocation_target_attempts_locked (
          std::chrono::steady_clock::now ());
    }
    cleanup_expired_relocation_target_attempts (std::move (expired));
}

bool public_host_runtime_t::relocation_target_authority_committed (
  const relocation_target_attempt_t &attempt) const noexcept
{
    if (!_relocation_authority)
        return true;
    try {
        if (attempt.targets.empty ())
            return false;
        for (const auto &target : attempt.targets) {
            const auto current = _relocation_authority->read (
              target.kind, target.key);
            if (!current || current->target != target)
                return false;
        }
        return true;
    }
    catch (...) {
        // An unavailable authority store cannot prove that abort is safe.
        return true;
    }
}

bool public_host_runtime_t::send_relocation_target_terminal (
  const relocation_attempt_key_t &key)
{
    expire_relocation_target_attempts ();
    relocation_target_attempt_t attempt;
    {
        std::lock_guard lock (_mutex);
        const auto found = _relocation_target_attempts.find (key);
        if (found == _relocation_target_attempts.end ()
            || !found->second.target_finalized
            || !found->second.completion
            || found->second.completion_source_routing_id.empty ())
            return false;
        attempt = found->second;
    }

    if (!attempt.terminal_response) {
        std::function<std::optional<location_owner_token_t> ()>
          owner_resolver;
        {
            std::lock_guard lock (_mutex);
            owner_resolver = _session_route_owner_resolver;
        }
        const auto owner =
          owner_resolver ? owner_resolver () : std::nullopt;
        if (!owner)
            return false;
        const auto local = status ();
        const auto &complete = *attempt.completion;
        const protocol::relocation_complete_t response{
          complete.relocation,
          complete.target_attempt_generation,
          complete.coordinator,
          protocol::relocation_role_t::target,
          {owner->owner_id,
           static_cast<std::uint64_t> (owner->lease_generation),
           local.routing_id ().to_bytes (),
           local.lifecycle_generation ()},
          complete.source_cleanup_state};
        {
            std::lock_guard lock (_mutex);
            const auto found = _relocation_target_attempts.find (key);
            if (found == _relocation_target_attempts.end ()
                || !found->second.target_finalized)
                return false;
            if (!found->second.terminal_response)
                found->second.terminal_response = response;
            attempt = found->second;
        }
    }
    if (!attempt.terminal_response)
        return false;

    bool sent = false;
    try {
        sent = _transport->send_relocation_control (
          attempt.completion_source_routing_id,
          *attempt.terminal_response);
    }
    catch (...) {
        sent = false;
    }
    if (!sent)
        return false;

    return true;
}

bool public_host_runtime_t::try_finalize_relocation_target (
  const relocation_attempt_key_t &key)
{
    relocation_target_attempt_t attempt;
    {
        std::lock_guard lock (_mutex);
        const auto found = _relocation_target_attempts.find (key);
        if (found == _relocation_target_attempts.end ()
            || (!found->second.target_finalized
                && (!found->second.completion
                    || !found->second.reserved)))
            return false;
        attempt = found->second;
    }
    if (attempt.target_finalized)
        return send_relocation_target_terminal (key);

    const auto &complete = *attempt.completion;
    for (const auto &[participant, high_water] :
         attempt.expected_high_water) {
        if (_relocation_wire->target_high_water (
              complete.relocation,
              complete.target_attempt_generation,
              participant)
            != high_water)
            return false;
    }
    for (const auto &[participant, _] :
         attempt.expected_high_water)
        (void) _relocation_wire->seal_target (
          complete.relocation,
          complete.target_attempt_generation,
          participant);
    for (const auto &[participant, _] :
         attempt.expected_high_water) {
        if (!_relocation_wire->drain_target (
              complete.relocation,
              complete.target_attempt_generation,
              participant))
            return false;
    }
    for (const auto &[participant, high_water] :
         attempt.expected_high_water) {
        if (_relocation_wire->target_high_water (
              complete.relocation,
              complete.target_attempt_generation,
              participant)
            != high_water)
            return false;
    }
    const auto committed =
      attempt.targets.size () == 1
        ? _objects.commit_relocation_restore (
            attempt.targets.front (), attempt.restore_identity)
        : _objects.commit_relocation_restore_aggregate (
            attempt.targets, attempt.restore_identity);
    if (committed != stateful::stateful_error_t::none
        && committed
             != stateful::stateful_error_t::already_exists)
        return false;
    for (const auto &[participant, _] :
         attempt.expected_high_water)
        (void) _relocation_wire->unregister_target (
          complete.relocation,
          complete.target_attempt_generation,
          participant);
    {
        std::lock_guard lock (_mutex);
        const auto found = _relocation_target_attempts.find (key);
        if (found == _relocation_target_attempts.end ())
            return false;
        found->second.target_finalized = true;
        found->second.attempt_expires_at = {};
    }
    return send_relocation_target_terminal (key);
}

std::size_t public_host_runtime_t::dispatch_user_spot_operations ()
{
    std::shared_ptr<zlink::framework::location_repository_t> store;
    user_spot_materializer_t materializer;
    actor_create_operation_target_t actor_create_target;
    instance_spot_activation_materializer_t instance_materializer;
    std::shared_ptr<stateful::relocation_store_port_t>
      instance_relocations;
    std::shared_ptr<stateful::relocation_store_port_t> relocation_store;
    location_owner_token_t instance_owner;
    std::function<std::optional<location_owner_token_t> ()>
      session_route_owner_resolver;
    std::function<void (const protocol::message_follow_notice_t &)>
      message_follow_handler;
    {
        std::lock_guard lock (_mutex);
        store = _user_spot_store;
        materializer = _user_spot_materializer;
        actor_create_target = _actor_create_target;
        instance_materializer = _instance_spot_materializer;
        instance_relocations = _instance_spot_relocations;
        relocation_store = _session_relocations;
        instance_owner = _instance_spot_owner;
        session_route_owner_resolver =
          _session_route_owner_resolver;
        message_follow_handler = _message_follow_handler;
    }
    expire_relocation_target_attempts ();
    std::size_t count = 0;
    receive_batch_budget_t infrastructure_budget;
    while (auto claim = _transport->mailbox ().try_claim (
             mesh::service_mailbox_domain_t::infrastructure,
             dispatch_limits::receive_batch_messages,
             dispatch_limits::receive_batch_bytes)) {
        for (auto &mailbox_record : claim->records) {
            std::size_t record_bytes = 0;
            for (const auto &part : mailbox_record.parts)
                record_bytes = part.size () >
                                   std::numeric_limits<std::size_t>::max ()
                                     - record_bytes
                                 ? std::numeric_limits<std::size_t>::max ()
                                 : record_bytes + part.size ();
            infrastructure_budget.account (record_bytes);
            ++count;
            try {
                const auto wire =
                  protocol::decode_header (
                    mailbox_record.parts.front ());
                if (wire.kind == protocol::command::messageFollow) {
                    if (mailbox_record.parts.size () != 1
                        || mailbox_record.source_node_generation == 0)
                        continue;
                    const auto notice = protocol::decode_message_follow (
                      mailbox_record.parts.front ());
                    invalidate_spot_route_fence (notice);
                    if (message_follow_handler) {
                        try {
                            message_follow_handler (notice);
                        }
                        catch (...) {
                        }
                    }
                    continue;
                }
                if (wire.kind
                    == protocol::command::relocationReady) {
                    if (mailbox_record.parts.size () != 1)
                        continue;
                    const auto control =
                      protocol::decode_relocation_control (
                        mailbox_record.parts.front ());
                    const auto *ready =
                      std::get_if<protocol::relocation_ready_t> (
                        &control);
                    if (!ready
                        || ready->candidate.node_routing_id
                             != mailbox_record.source_routing_id
                        || ready->candidate.node_generation
                             != mailbox_record.source_node_generation)
                        continue;
                    {
                        std::lock_guard lock (_mutex);
                        _relocation_ready_responses.insert_or_assign (
                          relocation_attempt_key_t{
                            ready->relocation.high,
                            ready->relocation.low,
                            ready->target_attempt_generation},
                          *ready);
                    }
                    _relocation_changed.notify_all ();
                    continue;
                }
                if (wire.kind
                    == protocol::command::relocationPrepare) {
                    if (mailbox_record.parts.size () != 1
                        || !relocation_store
                        || !session_route_owner_resolver)
                        continue;
                    const auto control =
                      protocol::decode_relocation_control (
                        mailbox_record.parts.front ());
                    const auto *prepare =
                      std::get_if<protocol::relocation_prepare_t> (
                        &control);
                    const auto local = status ();
                    const auto owner =
                      session_route_owner_resolver ();
                    if (!prepare || !prepare->root || !owner
                        || prepare->candidate.node_routing_id
                             != local.routing_id ().to_bytes ()
                        || prepare->candidate.node_generation
                             != local.lifecycle_generation ()
                        || prepare->candidate.owner_id
                             != owner->owner_id
                        || prepare->candidate
                             .owner_lease_generation
                             != static_cast<std::uint64_t> (
                               owner->lease_generation)
                        || prepare->source_node_routing_id
                             != mailbox_record.source_routing_id
                        || prepare->source_node_generation
                             != mailbox_record.source_node_generation)
                        continue;
                    const relocation_attempt_key_t key{
                      prepare->relocation.high,
                      prepare->relocation.low,
                      prepare->target_attempt_generation};
                    bool duplicate_finalized = false;
                    bool duplicate_active = false;
                    {
                        std::lock_guard lock (_mutex);
                        const auto found =
                          _relocation_target_attempts.find (key);
                        if (found != _relocation_target_attempts.end ()) {
                            if (found->second.prepare != *prepare)
                                continue;
                            if (found->second.target_finalized) {
                                found->second
                                  .completion_source_routing_id =
                                  mailbox_record.source_routing_id;
                                duplicate_finalized = true;
                            } else {
                                found->second.attempt_expires_at =
                                  std::chrono::steady_clock::now ()
                                  + relocation_attempt_retention;
                                duplicate_active = true;
                            }
                        }
                    }
                    if (duplicate_finalized) {
                        (void) send_relocation_target_terminal (key);
                        continue;
                    }
                    if (duplicate_active) {
                        const auto offered_messages =
                          std::max<std::uint64_t> (
                            1, prepare->required_messages);
                        const auto offered_bytes =
                          std::max<std::uint64_t> (
                            1, prepare->required_bytes);
                        (void) _transport
                          ->send_relocation_control (
                            mailbox_record.source_routing_id,
                            protocol::relocation_ready_t{
                              prepare->relocation,
                              prepare->target_attempt_generation,
                              prepare->round,
                              prepare->coordinator,
                              prepare->candidate,
                              prepare->object,
                              protocol::relocation_role_t::target,
                              offered_messages,
                              offered_bytes,
                              {},
                              prepare->source_node_generation,
                              local.lifecycle_generation (),
                              prepare->target_attempt_generation,
                              prepare->root,
                              prepare->application_version,
                              {}});
                        continue;
                    }
                    const auto payload =
                      relocation_store->get (
                        prepare->root->reference);
                    if (!payload
                        || stateful::maintenance_runtime_t::crc32c (
                             *payload)
                             != prepare->root->checksum_crc32c)
                        continue;

                    std::vector<stateful::frozen_object_state_t>
                      frozen;
                    stateful::inventory_digest_t inventory_digest{};
                    if (const auto single =
                          stateful::maintenance_runtime_t::decode (
                            *payload)) {
                        frozen.push_back (single->first);
                        inventory_digest = single->second;
                    }
                    else if (const auto aggregate =
                               stateful::maintenance_runtime_t::
                                 decode_aggregate (*payload)) {
                        frozen = aggregate->first;
                        inventory_digest = aggregate->second;
                    }
                    if (frozen.empty ()
                        || frozen.size ()
                             != prepare->participants.size ())
                        continue;

                    std::vector<stateful::object_ref_t> targets;
                    std::map<std::uint64_t, std::uint64_t>
                      expected_high_water;
                    bool valid = true;
                    for (std::size_t index = 0;
                         index != frozen.size (); ++index) {
                        const auto participant_id =
                          prepare->participants[index]
                            .participant_id;
                        if (participant_id == 0
                            || expected_high_water.contains (
                              participant_id)) {
                            valid = false;
                            break;
                        }
                        auto target = frozen[index].owner;
                        target.node_id =
                          local.routing_id ().to_string ();
                        ++target.authority_owner_generation;
                        expected_high_water.emplace (
                          participant_id,
                          frozen[index].pending_application.size ());
                        frozen[index].pending_application.clear ();
                        targets.push_back (std::move (target));
                    }
                    if (!valid)
                        continue;
                    const stateful::relocation_restore_identity_t
                      restore_identity{
                        prepare->root->reference,
                        prepare->root->checksum_crc32c,
                        inventory_digest};

                    std::size_t registered_count = 0;
                    const auto rollback_target = [&] {
                        for (std::size_t index = 0;
                             index != registered_count; ++index) {
                            const auto participant =
                              prepare->participants[index]
                                .participant_id;
                            try {
                                (void) _relocation_wire
                                  ->unregister_target (
                                    prepare->relocation,
                                    prepare
                                      ->target_attempt_generation,
                                    participant);
                            }
                            catch (...) {
                            }
                            if (_stateful_dispatch) {
                                try {
                                    (void) _stateful_dispatch
                                      ->discard_pending (
                                        targets[index]);
                                }
                                catch (...) {
                                }
                            }
                        }
                        try {
                            if (targets.size () == 1)
                                (void) _objects
                                  .abort_relocation_restore (
                                    targets.front (), restore_identity);
                            else
                                (void) _objects
                                  .abort_relocation_restore_aggregate (
                                    targets, restore_identity);
                        }
                        catch (...) {
                        }
                        if (_stateful_dispatch)
                            for (const auto &target : targets) {
                                try {
                                    (void) _stateful_dispatch
                                      ->discard_pending (target);
                                }
                                catch (...) {
                                }
                            }
                    };
                    for (std::size_t index = 0;
                         index != targets.size (); ++index) {
                        protocol::relocation_object_kind_t kind;
                        switch (targets[index].kind) {
                        case stateful::object_kind_t::actor:
                            kind =
                              protocol::relocation_object_kind_t::
                                actor;
                            break;
                        case stateful::object_kind_t::user_spot:
                            kind =
                              protocol::relocation_object_kind_t::
                                user_spot;
                            break;
                        case stateful::object_kind_t::instance_spot:
                            kind =
                              protocol::relocation_object_kind_t::
                                instance_spot;
                            break;
                        default:
                            valid = false;
                            continue;
                        }
                        const auto participant_id =
                          prepare->participants[index]
                            .participant_id;
                        const auto target = targets[index];
                        ++registered_count;
                        bool registered_ok = false;
                        try {
                            registered_ok =
                              _relocation_wire->register_target ({
                                prepare->relocation,
                                prepare->target_attempt_generation,
                                prepare->coordinator,
                                participant_id,
                                prepare->source_node_routing_id,
                                prepare->source_node_generation,
                                {kind,
                                 frozen[index].stable_type,
                                 target.key,
                                 target.object_generation,
                                 target.authority_owner_generation - 1},
                                [this, target] (
                                  const protocol::relocation_data_t
                                    &record) {
                                  if (!record.frozen_record)
                                      return false;
                                  const auto frozen =
                                    *record.frozen_record;
                                  return _stateful_dispatch
                                    && _stateful_dispatch
                                         ->stage_relocated (
                                           target,
                                           {record.sequence,
                                            protocol::
                                              encode_frozen_record (
                                                frozen)},
                                           [this,
                                            record,
                                            frozen] (
                                             const std::optional<
                                               protocol::
                                                 application_payload_t>
                                               &reply) {
                                               if (!frozen.reply_route_id)
                                                   return true;
                                               const auto relay =
                                                 protocol::reply_relay_t{
                                                   frozen.operation,
                                                   *frozen.reply_route_id,
                                                   record.relocation,
                                                   record
                                                     .target_attempt_generation,
                                                   record.coordinator,
                                                   record.participant_id,
                                                   record.sequence,
                                                   reply ? 0u : 105u,
                                                   reply
                                                     ? protocol::
                                                         framework_error_code::
                                                           none
                                                     : protocol::
                                                         framework_error_code::
                                                           requestFailed};
                                               if (!_relocation_wire
                                                      ->register_terminal_target (
                                                        {relay,
                                                         frozen.source,
                                                         reply,
                                                         [] (
                                                           protocol::
                                                             reply_relay_ack_status_t) {
                                                             return true;
                                                         },
                                                         [] { return true; },
                                                         record.coordinator
                                                           .node_routing_id}))
                                                   return false;
                                               (void) _relocation_wire
                                                 ->retry_terminal_relays (
                                                   stateful::
                                                     raw_relocation_replay_coordinator_t::
                                                       clock_t::now ());
                                               return true;
                                           })
                                           == stateful::
                                              stateful_error_t::none;
                              },
                              [this, target] (
                                const protocol::relocation_data_t &record) {
                                  if (_stateful_dispatch)
                                      (void) _stateful_dispatch
                                        ->discard_pending (
                                          target, record.sequence);
                                }});
                        }
                        catch (...) {
                            registered_ok = false;
                        }
                        if (!registered_ok) {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid) {
                        rollback_target ();
                        continue;
                    }
                    stateful::stateful_error_t restored =
                      stateful::stateful_error_t::conflict;
                    try {
                        restored =
                          targets.size () == 1
                            ? _objects.restore_relocation (
                                frozen.front (),
                                targets.front (),
                                restore_identity,
                                {})
                            : _objects.restore_relocation_aggregate (
                                frozen,
                                targets,
                                restore_identity,
                                {});
                    }
                    catch (...) {
                        rollback_target ();
                        continue;
                    }
                    if (restored != stateful::stateful_error_t::none
                        && restored
                             != stateful::stateful_error_t::already_exists) {
                        rollback_target ();
                        continue;
                    }
                    const auto attempt_expires_at =
                      std::chrono::steady_clock::now ()
                      + relocation_attempt_retention;
                    bool inserted = false;
                    try {
                        std::lock_guard lock (_mutex);
                        if (_relocation_target_attempts.size ()
                            < relocation_terminal_capacity) {
                            relocation_target_attempt_t attempt{
                              *prepare,
                              restore_identity,
                              targets,
                              expected_high_water,
                              false};
                            attempt.attempt_expires_at =
                              attempt_expires_at;
                            const auto [_, did_insert] =
                              _relocation_target_attempts.emplace (
                                key, std::move (attempt));
                            inserted = did_insert;
                        }
                    }
                    catch (...) {
                        rollback_target ();
                        continue;
                    }
                    if (!inserted) {
                        rollback_target ();
                        continue;
                    }
                    const auto offered_messages =
                      std::max<std::uint64_t> (
                        1, prepare->required_messages);
                    const auto offered_bytes =
                      std::max<std::uint64_t> (
                        1, prepare->required_bytes);
                    (void) _transport
                      ->send_relocation_control (
                        mailbox_record.source_routing_id,
                        protocol::relocation_ready_t{
                          prepare->relocation,
                          prepare->target_attempt_generation,
                          prepare->round,
                          prepare->coordinator,
                          prepare->candidate,
                          prepare->object,
                          protocol::relocation_role_t::target,
                          offered_messages,
                          offered_bytes,
                          {},
                          prepare->source_node_generation,
                          local.lifecycle_generation (),
                          prepare->target_attempt_generation,
                          prepare->root,
                          prepare->application_version,
                          {}});
                    continue;
                }
                if (wire.kind
                    == protocol::command::relocationReserved) {
                    if (mailbox_record.parts.size () != 1)
                        continue;
                    const auto control =
                      protocol::decode_relocation_control (
                        mailbox_record.parts.front ());
                    const auto *reserved =
                      std::get_if<
                        protocol::relocation_reserved_t> (
                        &control);
                    if (!reserved)
                        continue;
                    const relocation_attempt_key_t key{
                      reserved->relocation.high,
                      reserved->relocation.low,
                      reserved->target_attempt_generation};
                    std::lock_guard lock (_mutex);
                    const auto found =
                      _relocation_target_attempts.find (key);
                    if (found
                          == _relocation_target_attempts.end ()
                        || found->second.prepare.round
                             != reserved->round
                        || found->second.prepare.coordinator
                             != reserved->coordinator
                        || found->second.prepare.candidate
                             != reserved->candidate
                        || found->second.prepare.participants
                             != reserved->participants
                        || reserved->reservation_generation == 0)
                        continue;
                    found->second.reserved = true;
                    found->second.attempt_expires_at =
                      std::chrono::steady_clock::now ()
                      + relocation_attempt_retention;
                    continue;
                }
                if (wire.kind
                    == protocol::command::relocationComplete) {
                    if (mailbox_record.parts.size () != 1)
                        continue;
                    const auto control =
                      protocol::decode_relocation_control (
                        mailbox_record.parts.front ());
                    const auto *complete =
                      std::get_if<
                        protocol::relocation_complete_t> (
                        &control);
                    if (!complete)
                        continue;
                    const relocation_attempt_key_t key{
                      complete->relocation.high,
                      complete->relocation.low,
                      complete->target_attempt_generation};
                    if (complete->sender_role
                        == protocol::relocation_role_t::target) {
                        bool accepted = false;
                        {
                            std::lock_guard lock (_mutex);
                            const auto found =
                              _relocation_complete_responses.find (key);
                            if (found !=
                                  _relocation_complete_responses.end ()
                                && complete->coordinator
                                     == found->second.coordinator
                                && complete->source
                                     == found->second.expected_target
                                && complete->source_cleanup_state
                                     == found->second.source_cleanup_state
                                && mailbox_record.source_routing_id
                                     == found->second.expected_target
                                          .node_routing_id
                                && mailbox_record.source_node_generation
                                     == found->second.expected_target
                                          .node_generation) {
                                found->second.accepted = true;
                                accepted = true;
                            }
                        }
                        if (accepted)
                            _relocation_changed.notify_all ();
                        continue;
                    }
                    relocation_target_attempt_t target_attempt;
                    {
                        std::lock_guard lock (_mutex);
                        const auto found =
                          _relocation_target_attempts.find (key);
                        if (found
                              == _relocation_target_attempts.end ()
                            || found->second.prepare.coordinator
                                 != complete->coordinator
                            || complete->sender_role
                                 != protocol::relocation_role_t::source
                            || !same_relocation_source_fence (
                                 complete->source,
                                 found->second.prepare.coordinator)
                            || complete->source.node_routing_id
                                 != found->second.prepare
                                      .source_node_routing_id
                            || complete->source.node_generation
                                 != found->second.prepare
                                      .source_node_generation
                            || mailbox_record.source_routing_id
                                 != complete->source.node_routing_id
                            || mailbox_record.source_node_generation
                                 != complete->source.node_generation)
                            continue;
                        if (!found->second.target_finalized)
                            found->second.attempt_expires_at =
                              std::chrono::steady_clock::now ()
                              + relocation_attempt_retention;
                        target_attempt = found->second;
                    }
                    if (complete->source_cleanup_state
                        == protocol::source_cleanup_state_t::
                             completed) {
                        {
                            std::lock_guard lock (_mutex);
                            const auto found =
                              _relocation_target_attempts.find (key);
                            if (found
                                != _relocation_target_attempts.end ()) {
                                found->second.completion = *complete;
                                found->second
                                  .completion_source_routing_id =
                                  mailbox_record.source_routing_id;
                            }
                        }
                        (void) try_finalize_relocation_target (key);
                        continue;
                    }
                    if (target_attempt.target_finalized) {
                        {
                            std::lock_guard lock (_mutex);
                            const auto found =
                              _relocation_target_attempts.find (key);
                            if (found != _relocation_target_attempts.end ()
                                && !mailbox_record.source_routing_id.empty ())
                                found->second
                                  .completion_source_routing_id =
                                  mailbox_record.source_routing_id;
                        }
                        (void) send_relocation_target_terminal (key);
                        continue;
                    }
                    for (const auto &[participant, _] :
                         target_attempt.expected_high_water)
                        (void) _relocation_wire
                          ->seal_target (
                            complete->relocation,
                            complete->target_attempt_generation,
                            participant);
                    for (const auto &[participant, _] :
                         target_attempt.expected_high_water)
                        (void) _relocation_wire
                          ->drain_target (
                            complete->relocation,
                            complete->target_attempt_generation,
                            participant);
                    bool finalized = true;
                    {
                        const auto aborted =
                          target_attempt.targets.size () == 1
                            ? _objects.abort_relocation_restore (
                                target_attempt.targets.front (),
                                target_attempt.restore_identity)
                            : _objects
                                .abort_relocation_restore_aggregate (
                                  target_attempt.targets,
                                  target_attempt.restore_identity);
                        finalized =
                          aborted == stateful::stateful_error_t::none
                          || aborted
                               == stateful::stateful_error_t::
                                 already_exists;
                    }
                    if (!finalized)
                        continue;
                    for (const auto &[participant, _] :
                         target_attempt.expected_high_water)
                        (void) _relocation_wire
                          ->unregister_target (
                            complete->relocation,
                            complete->target_attempt_generation,
                            participant);
                    if (_stateful_dispatch)
                        for (const auto &target : target_attempt.targets)
                            (void) _stateful_dispatch
                              ->discard_pending (target);
                    {
                        std::lock_guard lock (_mutex);
                        const auto found =
                          _relocation_target_attempts.find (key);
                        if (found == _relocation_target_attempts.end ())
                            continue;
                        found->second.completion = *complete;
                        found->second
                          .completion_source_routing_id =
                            mailbox_record.source_routing_id;
                        found->second.target_finalized = true;
                        found->second.attempt_expires_at = {};
                    }
                    (void) send_relocation_target_terminal (key);
                    continue;
                }
                if (wire.kind == protocol::command::relocationData
                    || wire.kind == protocol::command::relocationAck
                    || wire.kind == protocol::command::replyRelay
                    || wire.kind == protocol::command::replyRelayAck) {
                    const auto processed =
                      _relocation_wire->process (mailbox_record);
                    if (wire.kind
                          == protocol::command::relocationData
                        && (processed
                              == stateful::
                                raw_relocation_replay_result_t::applied
                            || processed
                                 == stateful::
                                   raw_relocation_replay_result_t::
                                     duplicate)) {
                        const auto control =
                          protocol::decode_relocation_control (
                            mailbox_record.parts.front ());
                        if (const auto *data =
                              std::get_if<
                                protocol::relocation_data_t> (
                                &control)) {
                            (void) try_finalize_relocation_target ({
                              data->relocation.high,
                              data->relocation.low,
                              data->target_attempt_generation});
                        }
                    }
                    continue;
                }
                if (wire.kind
                      != protocol::command::sessionRelocationSeal
                    && wire.kind
                         != protocol::command::sessionRelocationRoute
                    && wire.kind != protocol::command::userSpotCreate
                    && wire.kind
                         != protocol::command::userSpotClose
                    && wire.kind != protocol::command::actorCreate
                    && wire.kind
                         != protocol::command::instanceSpot)
                    throw protocol::service_wire_error_t (
                      "unsupported infrastructure mailbox command");

                if (wire.kind
                    == protocol::command::sessionRelocationSeal) {
                    if (mailbox_record.parts.size () != 1
                        || !session_route_owner_resolver)
                        continue;
                    const auto seal =
                      protocol::decode_session_relocation_seal (
                        mailbox_record.parts.front ());
                    const auto owner = session_route_owner_resolver ();
                    if (!owner
                        || owner->owner_id != seal.session_owner_id
                        || seal.session_owner_lease_generation
                             > static_cast<std::uint64_t> (
                               std::numeric_limits<std::int64_t>::max ())
                        || owner->lease_generation
                             != static_cast<std::int64_t> (
                               seal.session_owner_lease_generation))
                        continue;
                    const auto relocation_key = std::pair{
                      seal.relocation.high,
                      seal.relocation.low};
                    std::optional<protocol::session_relocation_sealed_t>
                      cached_ack;
                    {
                        std::lock_guard lock (_mutex);
                        const auto cached =
                          _session_seal_terminals.find (
                            relocation_key);
                        if (cached
                            != _session_seal_terminals.end ()) {
                            if (cached->second.seal != seal)
                                continue;
                            cached_ack = cached->second.sealed;
                        }
                        else if (_session_seal_terminals.size ()
                                 >= 65'536) {
                            const auto consumed = std::find_if (
                              _session_seal_terminals.begin (),
                              _session_seal_terminals.end (),
                              [] (const auto &entry) {
                                  return entry.second.consumed;
                              });
                            if (consumed
                                == _session_seal_terminals.end ())
                                continue;
                            _session_seal_terminals.erase (consumed);
                        }
                    }
                    if (cached_ack) {
                        (void) _transport
                          ->send_session_relocation_sealed (
                            mailbox_record.source_routing_id,
                            *cached_ack);
                        continue;
                    }
                    const auto session_id =
                      zlink::routing_id_t::from (
                        seal.session_routing_id)
                        .to_string ();
                    const stateful::object_ref_t actor{
                      stateful::object_kind_t::actor,
                      seal.actor.actor_id,
                      seal.actor.object_generation,
                      seal.actor.authority_owner_generation,
                      {},
                      zlink::routing_id_t::from (
                        seal.actor.target_node_routing_id)
                        .to_string ()};
                    const auto admission =
                      _sessions.seal_remote_route (
                        session_id, seal.binding_generation,
                        actor, seal.actor.target_node_generation,
                        seal.actor.owner_lease_generation);
                    if (admission.error
                          != stateful::stateful_error_t::none
                        || !admission.binding
                        || admission.barrier.token == 0)
                        continue;
                    const protocol::session_relocation_sealed_t ack{
                      seal.relocation,
                      seal.coordinator,
                      seal.actor,
                      seal.session_owner_node_routing_id,
                      seal.session_owner_node_generation,
                      seal.session_owner_id,
                      seal.session_owner_lease_generation,
                      seal.session_routing_id,
                      seal.binding_generation,
                      admission.last_accepted_sequence};
                    {
                        std::lock_guard lock (_mutex);
                        const auto [stored, inserted] =
                          _session_seal_terminals.emplace (
                            relocation_key,
                            session_seal_terminal_record_t{
                              seal, ack, admission.barrier, false});
                        if (!inserted
                            && (stored->second.seal != seal
                                || stored->second.sealed != ack)) {
                            (void) _sessions.abort_barrier (
                              admission.barrier);
                            continue;
                        }
                    }
                    (void) _transport
                      ->send_session_relocation_sealed (
                        mailbox_record.source_routing_id, ack);
                    continue;
                }

                if (wire.kind
                    == protocol::command::sessionRelocationRoute) {
                    if (mailbox_record.parts.size () != 1
                        || !session_route_owner_resolver)
                        continue;
                    const auto route =
                      protocol::decode_session_relocation_route (
                        mailbox_record.parts.front ());
                    const auto owner = session_route_owner_resolver ();
                    if (!owner
                        || owner->owner_id != route.session_owner_id
                        || route.session_owner_lease_generation
                             > static_cast<std::uint64_t> (
                               std::numeric_limits<std::int64_t>::max ())
                        || owner->lease_generation
                             != static_cast<std::int64_t> (
                               route.session_owner_lease_generation))
                        continue;
                    const auto relocation_key = std::pair{
                      route.relocation.high,
                      route.relocation.low};
                    std::optional<
                      protocol::session_relocation_routed_t>
                      cached_ack;
                    {
                        std::lock_guard lock (_mutex);
                        const auto cached =
                          _session_route_terminals.find (
                            relocation_key);
                        if (cached
                            != _session_route_terminals.end ()) {
                            if (cached->second.first != route)
                                continue;
                            cached_ack = cached->second.second;
                        }
                        else if (_session_route_terminals.size ()
                                 >= 65'536) {
                            _session_route_terminals.erase (
                              _session_route_terminals.begin ());
                        }
                    }
                    if (cached_ack) {
                        (void) _transport
                          ->send_session_relocation_routed (
                            mailbox_record.source_routing_id,
                            *cached_ack);
                        continue;
                    }
                    std::uint64_t sealed_high_water = 0;
                    {
                        std::lock_guard lock (_mutex);
                        const auto sealed =
                          _session_seal_terminals.find (
                            relocation_key);
                        if (sealed
                              == _session_seal_terminals.end ()
                            || sealed->second.consumed
                            || sealed->second.seal.relocation
                                 != route.relocation
                            || sealed->second.seal.coordinator
                                 != route.coordinator
                            || sealed->second.seal.actor.actor_id
                                 != route.actor.actor_id
                            || sealed->second.seal.actor.object_generation
                                 != route.actor.object_generation
                            || sealed->second.seal
                                 .session_owner_node_routing_id
                                 != route
                                      .session_owner_node_routing_id
                            || sealed->second.seal
                                 .session_owner_node_generation
                                 != route
                                      .session_owner_node_generation
                            || sealed->second.seal.session_owner_id
                                 != route.session_owner_id
                            || sealed->second.seal
                                 .session_owner_lease_generation
                                 != route
                                      .session_owner_lease_generation
                            || sealed->second.seal.session_routing_id
                                 != route.session_routing_id
                            || sealed->second.seal.binding_generation
                                 != route.binding_generation)
                            continue;
                        const auto sealed_authority =
                          sealed->second.seal.actor
                            .authority_owner_generation;
                        if ((route.route.action
                               == protocol::
                                    session_relocation_route_action_t::commit
                             && (route.route
                                   .previous_authority_owner_generation
                                   != sealed_authority
                                 || route.route.replayed_high_water
                                      != sealed->second.sealed
                                           .last_accepted_session_sequence))
                            || (route.route.action
                                  == protocol::
                                       session_relocation_route_action_t::abort
                                && route.route
                                     .current_authority_owner_generation
                                     != sealed_authority))
                            continue;
                        sealed_high_water =
                          sealed->second.sealed
                            .last_accepted_session_sequence;
                    }
                    const auto session_id =
                      zlink::routing_id_t::from (
                        route.session_routing_id)
                        .to_string ();
                    stateful::stream_route_admission_t admission;
                    if (route.route.action
                        == protocol::session_relocation_route_action_t::commit) {
                        const auto current =
                          _sessions.current_binding (
                            route.actor.actor_id);
                        if (!current)
                            continue;
                        auto target = current->actor;
                        target.node_id =
                          zlink::routing_id_t::from (
                            route.route.target_node_routing_id)
                            .to_string ();
                        target.authority_owner_generation =
                          route.route.target_authority_owner_generation;
                        admission = _sessions.commit_remote_route (
                          session_id, route.binding_generation,
                          route.actor.actor_id,
                          route.actor.object_generation,
                          route.route
                            .previous_authority_owner_generation,
                          std::move (target),
                          route.route.target_node_generation,
                          route.route.replayed_high_water);
                    }
                    else {
                        admission =
                          _sessions.acknowledge_remote_abort (
                            session_id, route.binding_generation,
                            route.actor.actor_id,
                            route.actor.object_generation,
                            route.route
                              .current_authority_owner_generation);
                    }
                    if (admission.error
                          != stateful::stateful_error_t::none
                        || !admission.binding
                        || admission.last_accepted_sequence
                             != sealed_high_water)
                        continue;
                    const protocol::session_relocation_routed_t ack{
                      route.relocation,
                      route.coordinator,
                      route.actor,
                      route.session_owner_node_routing_id,
                      route.session_owner_node_generation,
                      route.session_owner_id,
                      route.session_owner_lease_generation,
                      route.session_routing_id,
                      route.binding_generation,
                      route.route.action,
                      admission.binding->actor
                        .authority_owner_generation,
                      admission.last_accepted_sequence};
                    {
                        std::lock_guard lock (_mutex);
                        const auto sealed =
                          _session_seal_terminals.find (
                            relocation_key);
                        if (sealed
                              == _session_seal_terminals.end ()
                            || sealed->second.consumed)
                            continue;
                        const auto [stored, inserted] =
                          _session_route_terminals.emplace (
                            relocation_key,
                            std::pair{route, ack});
                        if (!inserted
                            && (stored->second.first != route
                                || stored->second.second != ack))
                            continue;
                        sealed->second.consumed = true;
                    }
                    (void) _transport
                      ->send_session_relocation_routed (
                        mailbox_record.source_routing_id, ack);
                    continue;
                }

                if (wire.kind == protocol::command::actorCreate) {
                    const auto request =
                      protocol::decode_actor_create_header (
                        mailbox_record.parts.front ());
                    if (!actor_create_target
                        || request.deadline_unix_ms
                             <= unix_milliseconds_now ()) {
                        actor_create_operation_result_t result;
                        result.reply.header.correlation = request.correlation;
                        result.reply.header.terminal_result =
                          actor_create_target ? 101u : 105u;
                        result.reply.header.failure_code =
                          actor_create_target
                            ? 0u
                            : static_cast<std::uint32_t> (
                                protocol::framework_error_code::
                                  actorCreateFailed);
                        (void) _transport->reply_actor_create (
                          mailbox_record, result.reply,
                          std::move (result.application_reply));
                    }
                    else {
                        auto completed = std::make_shared<std::atomic_bool> (
                          false);
                        auto reply = [weak = weak_from_this (),
                                      mailbox_record, completed] (
                                         actor_create_operation_result_t result) mutable {
                            if (completed->exchange (true, std::memory_order_acq_rel))
                                return;
                            const auto host = weak.lock ();
                            if (!host)
                                return;
                            try {
                                (void) host->_transport->reply_actor_create (
                                  mailbox_record, result.reply,
                                  std::move (result.application_reply));
                            }
                            catch (...) {
                            }
                        };
                        try {
                            actor_create_target (request, reply);
                        }
                        catch (...) {
                            actor_create_operation_result_t result;
                            result.reply.header = {
                              request.correlation, 105u,
                              static_cast<std::uint32_t> (
                                protocol::framework_error_code::
                                  actorCreateFailed)};
                            reply (std::move (result));
                        }
                    }
                    continue;
                }

                if (wire.kind == protocol::command::instanceSpot) {
                    const auto request =
                      protocol::decode_instance_spot_activation_header (
                        mailbox_record.parts.front ());
                    const auto expected_parts = request.has_metadata ? 3u : 2u;
                    if (mailbox_record.parts.size () != expected_parts)
                        throw protocol::service_wire_error_t (
                          "Instance Spot activation has an invalid part count");
                    if (!store || !instance_relocations
                        || !instance_materializer) {
                        (void) _transport
                          ->reply_instance_spot_activation (
                            mailbox_record, 105,
                            static_cast<std::uint32_t> (
                              protocol::framework_error_code::requestFailed));
                        continue;
                    }
                    std::optional<std::vector<std::uint8_t>> metadata;
                    std::size_t application_index = 1;
                    if (request.has_metadata) {
                        metadata = mailbox_record.parts[1];
                        application_index = 2;
                    }
                    const auto application =
                      protocol::decode_application_payload (
                        mailbox_record.parts[application_index]);
                    const auto reply_terminal = [&] (
                      instance_spot_activation_result_t result) {
                        (void) _transport
                          ->reply_instance_spot_activation (
                            mailbox_record, result.terminal_result,
                            result.failure_code,
                            std::move (result.application_reply));
                    };
                    if (request.target.deadline_unix_ms
                        <= unix_milliseconds_now ()) {
                        reply_terminal ({101, 0, std::nullopt});
                        continue;
                    }
                    const protocol::instance_activation_recovery_t recovery{
                      request, metadata, application};
                    const auto recovery_bytes =
                      protocol::encode_instance_activation_recovery (recovery);
                    auto fingerprint_request = request;
                    fingerprint_request.reply_route_id =
                      request.request ? 1 : 0;
                    const auto fingerprint_bytes =
                      protocol::encode_instance_activation_recovery (
                        {fingerprint_request, metadata, application});
                    std::vector<std::byte> recovery_public;
                    recovery_public.reserve (fingerprint_bytes.size ());
                    for (const auto value : fingerprint_bytes)
                        recovery_public.push_back (
                          static_cast<std::byte> (value));
                    const auto request_sha256 =
                      runtime::sha256 (recovery_public);
                    const authority_key_t authority_key{
                      std::to_string (static_cast<int> (
                        placement_object_kind_t::instance_spot))
                      + ":" + request.target.spot_id};
                    const auto join_existing = [&] (
                      authority_read_result_t current) {
                        while (const auto *snapshot =
                                 std::get_if<authority_snapshot_t> (
                                   &current)) {
                            if (snapshot->allocation.object_kind
                                  != placement_object_kind_t::instance_spot
                                || snapshot->allocation.stable_type
                                     != request.target.stable_type) {
                                reply_terminal ({107,
                                  static_cast<std::uint32_t> (
                                    protocol::framework_error_code::
                                      spotTypeMismatch),
                                  std::nullopt});
                                return true;
                            }
                            if (snapshot->allocation.state
                                  == placement_allocation_state_t::active) {
                                auto ready_state =
                                  decode_instance_ready_state (
                                    snapshot->payload);
                                if (!ready_state) {
                                    if (const auto closing =
                                          decode_instance_closing_state (
                                            snapshot->payload);
                                        closing
                                        && closing->stable_type
                                             == request.target.stable_type
                                        && closing->spot_id
                                             == request.target.spot_id
                                        && closing->object_generation
                                             == snapshot->object_generation
                                        && closing->authority_owner_generation
                                             == snapshot
                                                  ->authority_owner_generation) {
                                        reply_terminal ({107,
                                          static_cast<std::uint32_t> (
                                            protocol::framework_error_code::spotMoving),
                                          std::nullopt});
                                        return true;
                                    }
                                    reply_terminal ({105,
                                      static_cast<std::uint32_t> (
                                        protocol::framework_error_code::
                                          requestFailed),
                                      std::nullopt});
                                    return true;
                                }
                                if (ready_state->operation
                                      == request.operation) {
                                    if (!ready_state->completed) {
                                        /* The reservation winner published
                                         * Ready before its durable terminal.
                                         * Join that pending operation. */
                                    }
                                    else if (ready_state->request_sha256
                                               != request_sha256) {
                                        reply_terminal ({104,
                                          static_cast<std::uint32_t> (
                                            protocol::framework_error_code::
                                              requestProtocolError),
                                          std::nullopt});
                                        return true;
                                    }
                                    else {
                                        reply_terminal ({
                                          ready_state->terminal_result,
                                          ready_state->failure_code,
                                          ready_state->reply});
                                        return true;
                                    }
                                }
                                else {
                                    const auto local = status ();
                                    const auto current_rid =
                                      zlink::routing_id_t::from (
                                        std::string (snapshot->allocation
                                          .target.node_rid.value ()));
                                    if (current_rid.to_bytes ()
                                          != local.routing_id ().to_bytes ()) {
                                        auto forwarded = request;
                                        // The forwarding node becomes the wire
                                        // source. Keeping the original caller's
                                        // source fence makes raw transport reject
                                        // an otherwise valid activation before it
                                        // reaches the current authority owner.
                                        forwarded.source_node_routing_id =
                                          local.routing_id ().to_bytes ();
                                        forwarded.source_node_generation =
                                          local.lifecycle_generation ();
                                        forwarded.target.target_node_routing_id =
                                          current_rid.to_bytes ();
                                        forwarded.target.target_node_generation =
                                          snapshot->allocation.target
                                            .node_lifecycle_generation;
                                        const auto copied_mailbox =
                                          mailbox_record;
                                        const auto remaining =
                                          std::chrono::milliseconds (
                                            request.target.deadline_unix_ms
                                              > unix_milliseconds_now ()
                                              ? request.target.deadline_unix_ms
                                                  - unix_milliseconds_now ()
                                              : 0);
                                        const auto relayed =
                                          activate_instance_spot_remote (
                                            current_rid,
                                            std::move (forwarded), metadata,
                                            application, remaining,
                                            [transport = _transport,
                                             copied_mailbox] (
                                              foundation::operation_terminal_t terminal,
                                              protocol::reply_header_t reply,
                                              std::optional<protocol::application_payload_t>
                                                application_reply) {
                                                if (terminal
                                                      != foundation::operation_terminal_t::completed) {
                                                    reply.terminal_result = 105;
                                                    reply.failure_code =
                                                      static_cast<std::uint32_t> (
                                                        protocol::framework_error_code::requestFailed);
                                                    application_reply.reset ();
                                                }
                                                (void) transport
                                                  ->reply_instance_spot_activation (
                                                    copied_mailbox,
                                                    reply.terminal_result,
                                                    reply.failure_code,
                                                    std::move (
                                                      application_reply));
                                            });
                                        if (!relayed)
                                            reply_terminal ({103, 0,
                                              std::nullopt});
                                        return true;
                                    }
                                    bool prepared = false;
                                    try {
                                        prepared = instance_materializer.prepare (
                                          request);
                                    }
                                    catch (...) {
                                        prepared = false;
                                    }
                                    if (!prepared) {
                                        reply_terminal ({105,
                                          static_cast<std::uint32_t> (
                                            protocol::framework_error_code::
                                              spotCreateFailed),
                                          std::nullopt});
                                        return true;
                                    }
                                    auto result =
                                      instance_materializer.dispatch (
                                        request, metadata, application);
                                    ready_state->operation = request.operation;
                                    ready_state->request_sha256 = request_sha256;
                                    ready_state->completed = true;
                                    ready_state->terminal_result =
                                      result.terminal_result;
                                    ready_state->failure_code =
                                      result.failure_code;
                                    ready_state->reply =
                                      result.application_reply;
                                    const auto stored = store
                                      ->compare_exchange_authority (
                                        authority_key,
                                        snapshot->store_version,
                                        authority_put_t{
                                          encode_instance_ready_state (
                                            *ready_state),
                                          authority_generation_transition_t::
                                            preserve})
                                      .result ().value ();
                                    if (!std::holds_alternative<
                                          authority_stored_t> (stored))
                                        result = {105,
                                          static_cast<std::uint32_t> (
                                            protocol::framework_error_code::
                                              requestFailed),
                                          std::nullopt};
                                    reply_terminal (std::move (result));
                                    return true;
                                }
                            }
                            if (request.target.deadline_unix_ms
                                  <= unix_milliseconds_now ()) {
                                reply_terminal ({101, 0, std::nullopt});
                                return true;
                            }
                            std::this_thread::sleep_for (
                              std::chrono::milliseconds (1));
                            current = store->read_authority (
                              authority_key).result ().value ();
                        }
                        return false;
                    };
                    const auto current = store
                      ->read_authority (authority_key)
                      .result ().value ();
                    if (join_existing (current)) {
                        continue;
                    }
                    const auto recovery_checksum =
                      stateful::maintenance_runtime_t::crc32c (
                        recovery_bytes);
                    const auto recovery_root =
                      instance_relocations->put (
                        recovery_bytes, std::chrono::hours (24));
                    if (recovery_root.reference.empty ()
                        || recovery_root.checksum_crc32c
                             != recovery_checksum) {
                        reply_terminal ({105,
                          static_cast<std::uint32_t> (
                            protocol::framework_error_code::requestFailed),
                          std::nullopt});
                        continue;
                    }
                    object_reserve_request_t reserve;
                    reserve.key = {
                      placement_object_kind_t::instance_spot,
                      request.target.spot_id};
                    reserve.intent.stable_type =
                      request.target.stable_type;
                    reserve.intent.request_content_reference =
                      recovery_root.reference;
                    reserve.intent.request_sha256 = request_sha256;
                    reserve.intent.request_encoded_size =
                      recovery_public.size ();
                    reserve.target = {
                      request.target.mesh_name,
                      node_rid_t::from_string (
                        zlink::routing_id_t::from (
                          request.target.target_node_routing_id)
                          .to_string ()),
                      request.target.target_node_generation,
                      instance_owner};
                    const std::string creating =
                      "zlink:instance-spot:creating:v1";
                    for (const auto value : creating)
                        reserve.creating_payload.push_back (
                          static_cast<std::byte> (
                            static_cast<unsigned char> (value)));
                    reserve.capacity_bundle = {
                      0, 1,
                      spot_type_capacity_delta_t{
                        placement_object_kind_t::instance_spot,
                        request.target.stable_type, 1}};
                    const auto reserved =
                      store->reserve (reserve).result ().value ();
                    const auto *reservation =
                      std::get_if<object_reserved_t> (&reserved);
                    if (!reservation) {
                        instance_relocations->remove (
                          recovery_root.reference);
                        if (std::holds_alternative<object_type_mismatch_t> (
                              reserved)) {
                            reply_terminal ({107,
                              static_cast<std::uint32_t> (
                                protocol::framework_error_code::
                                  spotTypeMismatch),
                              std::nullopt});
                            continue;
                        }
                        if (!join_existing (
                              store->read_authority (authority_key)
                                .result ().value ()))
                            reply_terminal ({105,
                              static_cast<std::uint32_t> (
                                protocol::framework_error_code::
                                  requestFailed),
                              std::nullopt});
                        continue;
                    }
                    bool prepared = false;
                    try {
                        prepared = instance_materializer.prepare (request);
                    }
                    catch (...) {
                        prepared = false;
                    }
                    if (!prepared) {
                        (void) store->abort (
                          {reserve.key, reservation->fence})
                          .result ().value ();
                        instance_relocations->remove (
                          recovery_root.reference);
                        reply_terminal ({105,
                          static_cast<std::uint32_t> (
                            protocol::framework_error_code::spotCreateFailed),
                          std::nullopt});
                        continue;
                    }
                    instance_ready_state_t ready_state{
                      request.target.stable_type,
                      request.target.spot_id,
                      reservation->fence.object_generation,
                      reservation->fence.authority_owner_generation,
                      recovery_root.reference,
                      recovery_root.checksum_crc32c,
                      request.operation};
                    ready_state.request_sha256 = request_sha256;
                    const auto committed = store->commit (
                      {reserve.key, reservation->fence,
                       encode_instance_ready_state (ready_state)})
                      .result ().value ();
                    const auto *created =
                      std::get_if<object_committed_t> (&committed);
                    const auto *already =
                      std::get_if<object_already_committed_t> (&committed);
                    if (!created && !already) {
                        reply_terminal ({107,
                          static_cast<std::uint32_t> (
                            protocol::framework_error_code::spotMoving),
                          std::nullopt});
                        continue;
                    }
                    const auto &ready_snapshot =
                      created ? created->ready : already->ready;
                    auto result = instance_materializer.dispatch (
                      request, metadata, application);
                    ready_state.completed = true;
                    ready_state.terminal_result = result.terminal_result;
                    ready_state.failure_code = result.failure_code;
                    ready_state.reply = result.application_reply;
                    const auto stored_terminal =
                      store->compare_exchange_authority (
                        authority_key, ready_snapshot.store_version,
                        authority_put_t{
                          encode_instance_ready_state (ready_state),
                          authority_generation_transition_t::preserve})
                        .result ().value ();
                    const auto *terminal_snapshot =
                      std::get_if<authority_stored_t> (&stored_terminal);
                    if (!terminal_snapshot) {
                        result = {105,
                          static_cast<std::uint32_t> (
                            protocol::framework_error_code::requestFailed),
                          std::nullopt};
                    }
                    else {
                        ready_state.recovery_reference.clear ();
                        ready_state.recovery_checksum = 0;
                        const auto cleared =
                          store->compare_exchange_authority (
                            authority_key,
                            terminal_snapshot->snapshot.store_version,
                            authority_put_t{
                              encode_instance_ready_state (ready_state),
                              authority_generation_transition_t::preserve})
                            .result ().value ();
                        if (std::holds_alternative<authority_stored_t> (
                              cleared))
                            instance_relocations->remove (
                              recovery_root.reference);
                    }
                    reply_terminal (std::move (result));
                    continue;
                }

                if (wire.kind
                    == protocol::command::userSpotCreate) {
                    const auto request =
                      protocol::decode_user_spot_create_header (
                        mailbox_record.parts.front ());
                    auto fingerprint_request = request;
                    fingerprint_request.correlation = 1;
                    const auto request_fingerprint =
                      protocol::encode_user_spot_create_header (
                        fingerprint_request);
                    const auto operation_key =
                      user_spot_operation_key (
                        request.source_node_routing_id,
                        request.source_node_generation,
                        request.operation);
                    std::optional<user_spot_terminal_record_t>
                      cached;
                    {
                        std::lock_guard lock (_mutex);
                        const auto found =
                          _user_spot_terminals.find (
                            operation_key);
                        if (found != _user_spot_terminals.end ()) {
                            if (user_spot_operation_replay_expired (
                                  found->second.deadline_unix_ms,
                                  unix_milliseconds_now (),
                                  _options
                                    .user_spot_operation_replay_retention))
                                _user_spot_terminals.erase (found);
                            else
                                cached = found->second;
                        }
                    }
                    if (cached) {
                        if (cached->kind
                              != protocol::command::userSpotCreate
                            || cached->request_fingerprint
                                 != request_fingerprint)
                            throw protocol::service_wire_error_t (
                              "user spot operation identity was reused with a different request");
                        auto reply =
                          protocol::decode_user_spot_create_reply (
                            cached->header);
                        reply.header.correlation =
                          request.correlation;
                        (void) _transport
                          ->reply_user_spot_create (
                            mailbox_record, reply,
                            cached->application_reply);
                        continue;
                    }
                    if (request.deadline_unix_ms
                        <= unix_milliseconds_now ()) {
                        protocol::user_spot_create_reply_t reply{
                          {request.correlation, 101, 0},
                          protocol::user_spot_create_result_t::rejected,
                          {},
                          0};
                        (void) _transport->reply_user_spot_create (
                          mailbox_record, reply, std::nullopt);
                        continue;
                    }
                    {
                        std::lock_guard lock (_mutex);
                        if (_user_spot_terminals.size ()
                            >= _options.user_spot_operation_capacity) {
                            const auto now = unix_milliseconds_now ();
                            std::erase_if (
                              _user_spot_terminals,
                              [this, now] (const auto &entry) {
                                  return user_spot_operation_replay_expired (
                                    entry.second.deadline_unix_ms, now,
                                    _options
                                      .user_spot_operation_replay_retention);
                              });
                        }
                        if (_user_spot_terminals.size ()
                            >= _options.user_spot_operation_capacity) {
                            protocol::user_spot_create_reply_t reply{
                              {request.correlation, 103, 0},
                              protocol::user_spot_create_result_t::rejected,
                              {},
                              0};
                            (void) _transport->reply_user_spot_create (
                              mailbox_record, reply, std::nullopt);
                            continue;
                        }
                    }
                    auto terminal =
                      [&] (std::uint32_t terminal_result,
                           std::uint32_t failure_code,
                           protocol::user_spot_create_result_t
                             result,
                           const std::string &spot,
                           std::uint64_t generation,
                           std::optional<
                             protocol::application_payload_t>
                             application_reply = std::nullopt) {
                          protocol::user_spot_create_reply_t reply{
                            {request.correlation,
                             terminal_result,
                             failure_code},
                            result,
                            spot,
                            generation};
                          user_spot_terminal_record_t stored{
                            protocol::command::userSpotCreate,
                            request.deadline_unix_ms,
                            request_fingerprint,
                            protocol::
                              encode_user_spot_create_reply (
                                request.correlation,
                                terminal_result, failure_code,
                                result, spot, generation),
                            application_reply};
                          {
                              std::lock_guard lock (_mutex);
                              _user_spot_terminals
                                .insert_or_assign (
                                  operation_key,
                                  std::move (stored));
                          }
                          (void) _transport
                            ->reply_user_spot_create (
                              mailbox_record, reply,
                              std::move (
                                application_reply));
                      };
                    if (!store || !materializer) {
                        terminal (
                          105, static_cast<std::uint32_t> (
                                 protocol::framework_error_code::
                                   requestFailed),
                          protocol::user_spot_create_result_t::
                            rejected,
                          {}, 0);
                        continue;
                    }
                    if (request.deadline_unix_ms
                        <= unix_milliseconds_now ()) {
                        terminal (
                          101, 0,
                          protocol::user_spot_create_result_t::
                            rejected,
                          {}, 0);
                        continue;
                    }
                    const auto &global_id = request.spot_id;
                    const auto read =
                      store
                        ->read_authority (
                          {std::to_string (
                             static_cast<int> (
                               placement_object_kind_t::
                                 user_spot))
                           + ":" + global_id})
                        .result ()
                        .value ();
                    const auto *snapshot =
                      std::get_if<authority_snapshot_t> (&read);
                    const auto &reservation =
                      request.reservation;
                    const auto exact =
                      snapshot
                      && snapshot->store_version
                           == reservation
                                .expected_store_version
                      && snapshot->object_generation
                           == reservation.object_generation
                      && snapshot
                           ->authority_owner_generation
                           == reservation
                                .authority_owner_generation
                      && snapshot->owner.owner_id
                           == reservation.target_owner_id
                      && snapshot->owner.lease_generation
                           == reservation
                                .target_owner_lease_generation
                      && snapshot->allocation.object_kind
                           == placement_object_kind_t::
                                user_spot
                      && snapshot->allocation.stable_type
                           == request.stable_type
                      && snapshot->allocation.target.node_rid.value ()
                           == node_rid_t::from_string (
                                zlink::routing_id_t::from (
                                  reservation
                                    .target_node_routing_id)
                                  .to_string ())
                                .value ()
                      && snapshot->allocation.target
                           .node_lifecycle_generation
                           == reservation
                                .target_node_generation
                      && snapshot->allocation.capacity_bundle.spot_slots
                           == reservation
                                .pending_capacity_delta;
                    if (!exact) {
                        const auto stale =
                          snapshot
                          && snapshot->object_generation
                               != reservation
                                    .object_generation;
                        const auto type_mismatch =
                          snapshot
                          && snapshot->allocation.object_kind
                               == placement_object_kind_t::
                                    user_spot
                          && snapshot->allocation.stable_type
                               != request.stable_type;
                        terminal (
                          107,
                          static_cast<std::uint32_t> (
                            stale
                              ? protocol::framework_error_code::
                                  spotGenerationStale
                              : type_mismatch
                                ? protocol::
                                    framework_error_code::
                                      spotTypeMismatch
                                : protocol::
                                    framework_error_code::
                                      spotMoving),
                          protocol::user_spot_create_result_t::
                            rejected,
                          {}, 0);
                        continue;
                    }
                    const auto fence =
                      public_fence (
                        reservation,
                        snapshot->allocation.target.mesh_name,
                        request.stable_type);
                    const auto &pending =
                      snapshot->pending_creation;
                    if (!pending
                        || pending->reservation_id
                             != fence.reservation_id) {
                        terminal (
                          107,
                          static_cast<std::uint32_t> (
                            protocol::framework_error_code::
                              spotMoving),
                          protocol::user_spot_create_result_t::
                            rejected,
                          {}, 0);
                        continue;
                    }
                    const auto creation_payload =
                      runtime::decode_inline_creation_content (
                        pending->request_content_reference);
                    if (!creation_payload
                        || pending->request_encoded_size
                             != creation_payload->size ()
                        || pending->request_sha256
                             != runtime::sha256 (*creation_payload)) {
                        terminal (
                          105,
                          static_cast<std::uint32_t> (
                            protocol::framework_error_code::
                              requestFailed),
                          protocol::user_spot_create_result_t::
                            rejected,
                          {}, 0);
                        continue;
                    }
                    const stateful::object_ref_t exact_ref{
                      stateful::object_kind_t::user_spot,
                      request.spot_id,
                      reservation.object_generation,
                      reservation.authority_owner_generation,
                      snapshot->allocation.target.mesh_name,
                      std::string (
                        snapshot->allocation.target.node_rid.value ())};
                    if (snapshot->allocation.state
                        == placement_allocation_state_t::active) {
                        const auto existing = _objects.find (
                          stateful::object_kind_t::user_spot,
                          exact_ref.key);
                        if (!existing
                            || *existing != exact_ref) {
                            terminal (
                              105,
                              static_cast<std::uint32_t> (
                                protocol::
                                  framework_error_code::
                                    requestFailed),
                              protocol::
                                user_spot_create_result_t::
                                  rejected,
                              {}, 0);
                            continue;
                        }
                        terminal (
                          0, 0,
                          protocol::user_spot_create_result_t::
                            existing,
                          request.spot_id,
                          exact_ref.object_generation);
                        continue;
                    }
                    auto local =
                      _objects.begin_reserved_object (
                        exact_ref, request.stable_type,
                        [&] {
                            std::vector<std::uint8_t> bytes;
                            bytes.reserve (
                              creation_payload->size ());
                            for (const auto value :
                                 *creation_payload)
                                bytes.push_back (
                                  std::to_integer<
                                    std::uint8_t> (value));
                            return bytes;
                        } ());
                    if (local.status
                        == stateful::create_status_t::existing) {
                        terminal (
                          0, 0,
                          protocol::user_spot_create_result_t::
                            existing,
                          request.spot_id,
                          exact_ref.object_generation);
                        continue;
                    }
                    if (!local.factory_owner) {
                        terminal (
                          local.error
                                == stateful::stateful_error_t::
                                     generation_stale
                            ? 107
                            : 108,
                          static_cast<std::uint32_t> (
                            local.error
                                  == stateful::
                                       stateful_error_t::moving
                              ? protocol::
                                  framework_error_code::
                                    spotMoving
                              : local.error
                                    == stateful::
                                         stateful_error_t::
                                           generation_stale
                                ? protocol::
                                    framework_error_code::
                                      spotGenerationStale
                                : protocol::
                                    framework_error_code::
                                      requestFailed),
                          protocol::user_spot_create_result_t::
                            rejected,
                          {}, 0);
                        continue;
                    }
                    user_spot_materialize_result_t
                      materialized;
                    try {
                        materialized =
                          materializer (
                            exact_ref, request.stable_type,
                            *creation_payload);
                    }
                    catch (...) {
                        (void) _objects.abort_create (
                          local.attempt);
                        (void) store
                          ->abort (
                            {{placement_object_kind_t::
                                user_spot,
                              global_id},
                             public_fence (
                               reservation,
                               snapshot->allocation.target.mesh_name,
                               request.stable_type)})
                          .result ();
                        terminal (
                          105,
                          static_cast<std::uint32_t> (
                            protocol::framework_error_code::
                              spotCreateFailed),
                          protocol::user_spot_create_result_t::
                            rejected,
                          {}, 0);
                        continue;
                    }
                    if (!materialized.accepted) {
                        (void) store
                          ->abort (
                            {{placement_object_kind_t::
                                user_spot,
                              global_id},
                             fence})
                          .result ()
                          .value ();
                        (void) _objects.abort_create (
                          local.attempt);
                        terminal (
                          0, 0,
                          protocol::user_spot_create_result_t::
                            rejected,
                          request.spot_id,
                          exact_ref.object_generation,
                          std::move (
                            materialized.application_reply));
                        continue;
                    }
                    const auto committed =
                      store
                        ->commit (
                          {{placement_object_kind_t::user_spot,
                           global_id},
                           fence,
                           ready_user_spot_authority_payload (
                             exact_ref,
                             request.stable_type)})
                        .result ()
                        .value ();
                    const auto *ready =
                      std::get_if<object_committed_t> (
                        &committed);
                    const auto *already =
                      std::get_if<
                        object_already_committed_t> (
                        &committed);
                    if (!ready && !already) {
                        (void) _objects.abort_create (
                          local.attempt);
                        terminal (
                          107,
                          static_cast<std::uint32_t> (
                            protocol::framework_error_code::
                              spotMoving),
                          protocol::user_spot_create_result_t::
                            rejected,
                          {}, 0);
                        continue;
                    }
                    auto local_commit =
                      _objects.commit_create (local.attempt);
                    if (local_commit
                        != stateful::stateful_error_t::none) {
                        std::vector<std::uint8_t> creation_bytes;
                        creation_bytes.reserve (
                          creation_payload->size ());
                        for (const auto value : *creation_payload)
                            creation_bytes.push_back (
                              std::to_integer<std::uint8_t> (value));
                        const auto reconciled =
                          _objects.begin_reserved_object (
                            exact_ref, request.stable_type,
                            std::move (creation_bytes));
                        if (reconciled.status
                              == stateful::create_status_t::existing
                            && reconciled.object == exact_ref)
                            local_commit =
                              stateful::stateful_error_t::none;
                        else if (
                          (reconciled.status
                             == stateful::create_status_t::reserved
                           || reconciled.status
                                == stateful::create_status_t::joined)
                          && reconciled.attempt != 0)
                            local_commit = _objects.commit_create (
                              reconciled.attempt);
                    }
                    if (local_commit
                        != stateful::stateful_error_t::none) {
                        terminal (
                          105,
                          static_cast<std::uint32_t> (
                            protocol::framework_error_code::
                              spotCreateFailed),
                          protocol::user_spot_create_result_t::
                            rejected,
                          {}, 0);
                        continue;
                    }
                    {
                        std::lock_guard lock (_mutex);
                        _spots.insert_or_assign (
                          exact_ref.key, exact_ref);
                    }
                    terminal (
                      0, 0,
                      protocol::user_spot_create_result_t::
                        created,
                      request.spot_id,
                      exact_ref.object_generation,
                      std::move (
                        materialized.application_reply));
                    continue;
                }

                const auto request =
                  protocol::decode_user_spot_close_header (
                    mailbox_record.parts.front ());
                auto fingerprint_request = request;
                fingerprint_request.correlation = 1;
                const auto request_fingerprint =
                  protocol::encode_user_spot_close_header (
                    fingerprint_request);
                const auto operation_key =
                  user_spot_operation_key (
                    request.source_node_routing_id,
                    request.source_node_generation,
                    request.operation);
                std::optional<user_spot_terminal_record_t>
                  cached;
                {
                    std::lock_guard lock (_mutex);
                    const auto found =
                      _user_spot_terminals.find (
                        operation_key);
                    if (found != _user_spot_terminals.end ()) {
                        if (user_spot_operation_replay_expired (
                              found->second.deadline_unix_ms,
                              unix_milliseconds_now (),
                              _options
                                .user_spot_operation_replay_retention))
                            _user_spot_terminals.erase (found);
                        else
                            cached = found->second;
                    }
                }
                if (cached) {
                    if (cached->kind
                          != protocol::command::userSpotClose
                        || cached->request_fingerprint
                             != request_fingerprint)
                        throw protocol::service_wire_error_t (
                          "user spot operation identity was reused with a different request");
                    auto reply =
                      protocol::decode_user_spot_close_reply (
                        cached->header);
                    reply.header.correlation =
                      request.correlation;
                    (void) _transport->reply_user_spot_close (
                      mailbox_record, reply);
                    continue;
                }
                if (request.deadline_unix_ms
                    <= unix_milliseconds_now ()) {
                    protocol::user_spot_close_reply_t reply{
                      {request.correlation, 101, 0}, false};
                    (void) _transport->reply_user_spot_close (
                      mailbox_record, reply);
                    continue;
                }
                {
                    std::lock_guard lock (_mutex);
                    if (_user_spot_terminals.size ()
                        >= _options.user_spot_operation_capacity) {
                        const auto now = unix_milliseconds_now ();
                        std::erase_if (
                          _user_spot_terminals,
                          [this, now] (const auto &entry) {
                              return user_spot_operation_replay_expired (
                                entry.second.deadline_unix_ms, now,
                                _options
                                  .user_spot_operation_replay_retention);
                          });
                    }
                    if (_user_spot_terminals.size ()
                        >= _options.user_spot_operation_capacity) {
                        protocol::user_spot_close_reply_t reply{
                          {request.correlation, 103, 0}, false};
                        (void) _transport->reply_user_spot_close (
                          mailbox_record, reply);
                        continue;
                    }
                }
                auto terminal =
                  [&] (std::uint32_t terminal_result,
                       std::uint32_t failure_code,
                       bool closed) {
                      protocol::user_spot_close_reply_t reply{
                        {request.correlation,
                         terminal_result,
                         failure_code},
                        closed};
                      user_spot_terminal_record_t stored{
                        protocol::command::userSpotClose,
                        request.deadline_unix_ms,
                        request_fingerprint,
                        protocol::encode_user_spot_close_reply (
                          request.correlation,
                          terminal_result, failure_code,
                          closed),
                        std::nullopt};
                      {
                          std::lock_guard lock (_mutex);
                          _user_spot_terminals.insert_or_assign (
                            operation_key, std::move (stored));
                      }
                      (void) _transport
                        ->reply_user_spot_close (
                          mailbox_record, reply);
                  };
                if (!store) {
                    terminal (
                      105, static_cast<std::uint32_t> (
                             protocol::framework_error_code::
                               requestFailed),
                      false);
                    continue;
                }
                const auto &global_id = request.target.spot_id;
                const auto read =
                  store
                    ->read_authority (
                      {std::to_string (
                         static_cast<int> (
                           placement_object_kind_t::
                             user_spot))
                       + ":" + global_id})
                    .result ()
                    .value ();
                const auto *snapshot =
                  std::get_if<authority_snapshot_t> (&read);
                if (!snapshot) {
                    terminal (0, 0, false);
                    continue;
                }
                const auto &target = request.target;
                if (snapshot->object_generation
                      != target.object_generation) {
                    terminal (
                      107, static_cast<std::uint32_t> (
                             protocol::framework_error_code::
                               spotGenerationStale),
                      false);
                    continue;
                }
                if (snapshot->authority_owner_generation
                      != target.authority_owner_generation
                    || snapshot->store_version
                         != target.expected_store_version
                    || snapshot->allocation.object_kind
                         != placement_object_kind_t::user_spot
                    || snapshot->allocation.state
                         != placement_allocation_state_t::active
                    || snapshot->allocation.target.node_rid.value ()
                         != node_rid_t::from_string (
                              zlink::routing_id_t::from (
                                target
                                  .target_node_routing_id)
                                .to_string ())
                              .value ()
                    || snapshot->allocation.target
                         .node_lifecycle_generation
                         != target.target_node_generation) {
                    terminal (
                      107, static_cast<std::uint32_t> (
                             protocol::framework_error_code::
                               spotMoving),
                      false);
                    continue;
                }
                const stateful::object_ref_t exact_ref{
                  stateful::object_kind_t::user_spot,
                  target.spot_id,
                  target.object_generation,
                  target.authority_owner_generation,
                  snapshot->allocation.target.mesh_name,
                  std::string (
                    snapshot->allocation.target.node_rid.value ())};
                if (snapshot->payload
                    != ready_user_spot_authority_payload (
                      exact_ref, snapshot->allocation.stable_type)) {
                    terminal (
                      107, static_cast<std::uint32_t> (
                             protocol::framework_error_code::
                               spotMoving),
                      false);
                    continue;
                }
                const auto local = _objects.find (
                  stateful::object_kind_t::user_spot,
                  exact_ref.key);
                if (!local || *local != exact_ref) {
                    terminal (
                      107, static_cast<std::uint32_t> (
                             protocol::framework_error_code::
                               spotMoving),
                      false);
                    continue;
                }
                const authority_key_t authority_key{
                  std::to_string (
                    static_cast<int> (
                      placement_object_kind_t::user_spot))
                  + ":" + global_id};
                const auto sealed =
                  store
                    ->compare_exchange_authority (
                      authority_key,
                      snapshot->store_version,
                      authority_put_t{
                        closing_user_spot_authority_payload (
                          exact_ref),
                        authority_generation_transition_t::
                          preserve,
                        std::nullopt,
                        std::nullopt})
                    .result ()
                    .value ();
                const auto *closing =
                  std::get_if<authority_stored_t> (&sealed);
                if (!closing) {
                    terminal (
                      107, static_cast<std::uint32_t> (
                             protocol::framework_error_code::
                               spotMoving),
                      false);
                    continue;
                }
                const auto rollback_closing = [&] {
                    return std::holds_alternative<
                      authority_stored_t> (
                      store
                        ->compare_exchange_authority (
                          authority_key,
                          closing->snapshot.store_version,
                          authority_put_t{
                            snapshot->payload,
                            authority_generation_transition_t::
                              preserve,
                            std::nullopt,
                            std::nullopt})
                        .result ()
                        .value ());
                };
                const auto [close_error, close_token] =
                  _objects.begin_close_spot (exact_ref);
                if (close_error
                    == stateful::stateful_error_t::
                         generation_stale) {
                    (void) rollback_closing ();
                    terminal (
                      107, static_cast<std::uint32_t> (
                             protocol::framework_error_code::
                               spotGenerationStale),
                      false);
                    continue;
                }
                if (close_error
                    == stateful::stateful_error_t::moving) {
                    (void) rollback_closing ();
                    terminal (
                      107, static_cast<std::uint32_t> (
                             protocol::framework_error_code::
                               spotMoving),
                      false);
                    continue;
                }
                if (close_error
                      == stateful::stateful_error_t::not_found
                    || !close_token) {
                    (void) rollback_closing ();
                    terminal (
                      107, static_cast<std::uint32_t> (
                             protocol::framework_error_code::
                               spotMoving),
                      false);
                    continue;
                }
                if (_objects.commit_close_spot (
                      *close_token)
                    != stateful::stateful_error_t::none) {
                    (void) rollback_closing ();
                    terminal (
                      105, static_cast<std::uint32_t> (
                             protocol::framework_error_code::
                               requestFailed),
                      false);
                    continue;
                }
                const auto deleted =
                  store
                    ->compare_exchange_authority (
                      authority_key,
                      closing->snapshot.store_version,
                      authority_delete_t{})
                    .result ()
                    .value ();
                if (!std::holds_alternative<
                      authority_deleted_t> (deleted)) {
                    terminal (
                      107, static_cast<std::uint32_t> (
                             protocol::framework_error_code::
                               spotMoving),
                      false);
                    continue;
                }
                {
                    std::lock_guard lock (_mutex);
                    _spots.erase (exact_ref.key);
                }
                terminal (0, 0, true);
            }
            catch (const protocol::service_wire_error_t &) {
                if (mailbox_record.request_sequence
                    && mailbox_record.correlation)
                    (void) _transport->reply_failure (
                      mailbox_record, 104,
                      static_cast<std::uint32_t> (
                        protocol::framework_error_code::
                          requestProtocolError));
            }
            catch (const std::exception &) {
                if (mailbox_record.request_sequence
                    && mailbox_record.correlation)
                    (void) _transport->reply_failure (
                      mailbox_record, 105,
                      static_cast<std::uint32_t> (
                        protocol::framework_error_code::
                          requestFailed));
            }
            catch (...) {
                if (mailbox_record.request_sequence
                    && mailbox_record.correlation)
                    (void) _transport->reply_failure (
                      mailbox_record, 105,
                      static_cast<std::uint32_t> (
                        protocol::framework_error_code::requestFailed));
            }
        }
        (void) _transport->mailbox ().release (*claim);
        if (infrastructure_budget.exhausted ())
            break;
    }
    return count;
}

std::size_t public_host_runtime_t::dispatch_ready (
  const std::function<void (const ready_record_t &,
                            const receive_record_t &,
                            std::vector<zlink::message_t>)> &dispatch,
  bool accept_application_receive)
{
    if (!dispatch) {
        throw std::invalid_argument (
          "framework public host dispatch callback is required");
    }
    expire_local_spot_requests ();
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    (void) _relocation_wire->retry_source_replays (now);
    (void) _relocation_wire->retry_terminal_relays (now);
    (void) _relocation_wire->reap_terminal_tombstones (now);
    (void) _transport->drain_monitor_events (now);
    std::size_t count = 0;
    receive_batch_budget_t budget;
    while (budget.can_receive ()) {
        const auto pumped = _transport->pump_one (
          now, accept_application_receive);
        budget.account (_transport->last_pump_bytes ());
        trace_mesh_host (
          "pump",
          std::string ("result=") + pump_result_name (pumped)
            + " pending="
            + std::to_string (
              _transport->mailbox ().pending_messages (
                mesh::service_mailbox_domain_t::application)));
        if (pumped == mesh::raw_mesh_pump_result_t::no_data) {
            break;
        }
        ++count;
        if (pumped == mesh::raw_mesh_pump_result_t::capacity_exceeded) {
            trace_mesh_host (
              "completion-control-capacity-exceeded",
              "source connection was disconnected before the next control record");
            break;
        }
        if (pumped == mesh::raw_mesh_pump_result_t::application) {
            // Re-evaluate the host-wide byte budget before starting the next
            // application receive. Infrastructure controls may still arrive
            // through the bounded Completion callback on the next pass.
            break;
        }
        if (budget.exhausted ())
            break;
    }
    (void) _transport->expire_requests (
      foundation::operation_registry_t::clock_t::now ());
    count += dispatch_user_spot_operations ();
    bool application_dispatch_started = false;

    auto completions = _completions.take_completed ();
    for (auto &[_, completion] : completions) {
        ready_record_t owner;
        owner.owner_kind = owner_kind_t::node;
        owner.domain = ready_domain_t::infrastructure;
        dispatch (owner, completion.first, std::move (completion.second));
        ++count;
    }

    if (accept_application_receive) {
        for (;;) {
            std::optional<local_application_dispatch_t> pending;
            bool skip = false;
            {
                std::lock_guard lock (_mutex);
                if (_local_application_dispatches.empty ())
                    break;
                pending = std::move (_local_application_dispatches.front ());
                _local_application_dispatches.pop_front ();
                if (pending->record.kind == record_kind_t::spot_request) {
                    const auto found = _local_spot_requests.find (
                      pending->record.operation_id);
                    if (found == _local_spot_requests.end ()) {
                        skip = true;
                    } else {
                        found->second.queued = false;
                        if (found->second.terminal_claimed) {
                            if (found->second.payload_bytes
                                <= _local_spot_request_bytes) {
                                _local_spot_request_bytes -=
                                  found->second.payload_bytes;
                            } else {
                                _local_spot_request_bytes = 0;
                            }
                            _local_spot_requests.erase (found);
                            skip = true;
                        }
                    }
                }
            }
            if (skip)
                continue;
            if (!pending)
                break;
            dispatch (pending->owner, pending->record,
                      std::move (pending->parts));
            ++count;
            application_dispatch_started = true;
            break;
        }
    }

    if (accept_application_receive && !application_dispatch_started
        && _stateful_dispatch) {
        const auto local_node_id =
          zlink::routing_id_t::from (
            _transport->topology ().local_descriptor ().node_routing_id)
            .to_string ();
        for (const auto &item : _objects.inventory ()) {
            if (application_dispatch_started)
                break;
            if (item.state != stateful::object_state_t::ready)
                continue;
            if (item.owner.node_id != local_node_id)
                continue;
            for (;;) {
                const auto ingested =
                  _stateful_dispatch->ingest (item.owner);
                if (ingested == stateful::stateful_error_t::not_found)
                    break;
                ++count;
                if (ingested != stateful::stateful_error_t::none)
                    break;
            }
            auto [claim_error, delivery] =
              _stateful_dispatch->try_claim (item.owner);
            if (claim_error != stateful::stateful_error_t::none
                || !delivery)
                continue;
            try {
                const auto frozen =
                  protocol::decode_frozen_record (
                    delivery->turn.payload);
                ready_record_t owner;
                owner.domain = ready_domain_t::application;
                receive_record_t record;
                record.domain = ready_domain_t::application;
                record.source_node_rid =
                  zlink::routing_id_t::from (
                    frozen.source.node_routing_id);
                record.operation_id = operation_id_t{
                  frozen.operation.high, frozen.operation.low};
                switch (frozen.kind) {
                case protocol::frozen_record_kind_t::actor_send:
                    owner.owner_kind = owner_kind_t::actor;
                    record.kind = record_kind_t::actor_send;
                    break;
                case protocol::frozen_record_kind_t::actor_request:
                    owner.owner_kind = owner_kind_t::actor;
                    record.kind = record_kind_t::actor_request;
                    break;
                case protocol::frozen_record_kind_t::spot_send:
                    owner.owner_kind = owner_kind_t::spot;
                    record.kind = record_kind_t::spot_send;
                    break;
                case protocol::frozen_record_kind_t::spot_request:
                    owner.owner_kind = owner_kind_t::spot;
                    record.kind = record_kind_t::spot_request;
                    break;
                default:
                    throw protocol::service_wire_error_t (
                      "unsupported stateful application record");
                }
                record.operation_kind =
                  operation_kind (record.kind);
                if (owner.owner_kind == owner_kind_t::actor) {
                    owner.actor = framework_actor_ref (
                      item.owner, item.stable_type);
                }
                else {
                    owner.spot_id = item.owner.key;
                }
                auto completed =
                  std::make_shared<std::atomic_bool> (false);
                record.complete_stateful_dispatch =
                  [dispatch = _stateful_dispatch.get (),
                   delivery = *delivery,
                   completed] {
                      if (!completed->exchange (
                            true, std::memory_order_acq_rel))
                          (void) dispatch->complete (
                            delivery, std::nullopt);
                  };
                if (delivery->request) {
                    record.reply_token.host = weak_from_this ();
                    record.reply_token.local_reply =
                      [this,
                       dispatch = _stateful_dispatch.get (),
                       delivery = *delivery,
                       completed] (
                        const std::vector<zlink::message_t> &parts) {
                          if (completed->exchange (
                                true, std::memory_order_acq_rel))
                              return false;
                          return dispatch->complete (
                                   delivery,
                                   encode_application (parts))
                                 == stateful::stateful_error_t::none;
                      };
                }
                dispatch (
                  owner, record,
                  decode_application (delivery->payload));
                ++count;
                application_dispatch_started = true;
            }
            catch (const std::exception &) {
                (void) _stateful_dispatch->complete (
                  *delivery, std::nullopt);
            }
            catch (...) {
                (void) _stateful_dispatch->complete (
                  *delivery, std::nullopt);
            }
        }
    }

    while (accept_application_receive && !application_dispatch_started) {
        auto claim = _transport->mailbox ().try_claim (
          mesh::service_mailbox_domain_t::application, 1,
          dispatch_limits::application_mailbox_bytes);
        if (!claim)
            break;
        trace_mesh_host (
          "mailbox-claim",
          std::string ("records=") + std::to_string (claim->records.size ()));
        auto claim_holder = std::make_shared<mesh::service_mailbox_claim_t> (
          std::move (*claim));
        auto claim_released = std::make_shared<std::atomic_bool> (false);
        auto claim_retained = std::make_shared<std::atomic_bool> (false);
        const auto retain_mailbox_reservation = [claim_retained] {
            claim_retained->store (true, std::memory_order_release);
        };
        const auto release_mailbox_reservation = [weak = weak_from_this (),
                                                   claim_holder,
                                                   claim_released] {
            if (claim_released->exchange (true, std::memory_order_acq_rel)) {
                return;
            }
            if (const auto host = weak.lock ()) {
                (void) host->_transport->mailbox ().release (*claim_holder);
            }
        };
        for (auto &mailbox_record : claim_holder->records) {
            try {
                const auto wire =
                  protocol::decode_header (mailbox_record.parts.front ());
                const auto kind = record_kind (wire.kind);
                ready_record_t owner;
                owner.domain = ready_domain_t::application;
                receive_record_t record;
                record.kind = kind;
                record.domain = ready_domain_t::application;
                record.operation_kind = operation_kind (kind);
                record.source_node_rid =
                  zlink::routing_id_t::from (
                    mailbox_record.source_routing_id);
                if (mailbox_record.operation) {
                    record.operation_id = {
                      mailbox_record.operation->first,
                      mailbox_record.operation->second};
                } else if (mailbox_record.correlation) {
                    record.operation_id = {
                      status ().lifecycle_generation (),
                      *mailbox_record.correlation};
                }
                if (is_request (kind)) {
                    record.reply_token = {
                      weak_from_this (),
                      std::make_shared<mesh::service_mailbox_record_t> (
                        mailbox_record)};
                }
                if (kind == record_kind_t::channel_send
                    || kind == record_kind_t::channel_request) {
                    owner.owner_kind = owner_kind_t::channel;
                    owner.channel_name =
                      kind == record_kind_t::channel_send
                        ? protocol::decode_channel_send_header (
                            mailbox_record.parts.front ())
                        : protocol::decode_channel_request_header (
                            mailbox_record.parts.front ())
                            .channel_name;
                    record.channel_name = owner.channel_name;
                } else if (kind == record_kind_t::spot_send
                           || kind == record_kind_t::spot_request) {
                    owner.owner_kind = owner_kind_t::spot;
                    const auto spot = protocol::decode_spot_message_header (
                      mailbox_record.parts.front (), wire.kind);
                    owner.spot_id = spot.target.spot_id;
                    record.spot_route = spot.target;
                } else if (kind == record_kind_t::actor_send
                           || kind == record_kind_t::actor_request) {
                    owner.owner_kind = owner_kind_t::actor;
                    const auto actor =
                      protocol::decode_actor_message_header (
                        mailbox_record.parts.front (), wire.kind);
                    record.actor_route = actor.target;
                    record.message_follow_hop_count =
                      actor.message_follow_hop_count;
                    record.reply_route_id =
                      mailbox_record.request_sequence.value_or (0);
                    std::string actor_type;
                    {
                        std::lock_guard lock (_mutex);
                        const auto found = _actors.find (
                          actor.target.actor_id);
                        if (found != _actors.end ()) {
                            actor_type = found->second.first;
                        }
                    }
                    owner.actor = ::zlink::framework::detail::actor_ref_access_t::make (
                      node_rid_t::from_string (
                        status ().routing_id ().to_string ()),
                      std::move (actor_type),
                      actor.target.actor_id,
                      actor.target.object_generation);
                } else {
                    owner.owner_kind = owner_kind_t::node;
                }
                const auto payload =
                  protocol::decode_application_payload (
                    mailbox_record.parts[1]);
                auto source = std::string ("-");
                if (!mailbox_record.source_routing_id.empty ()) {
                    source = zlink::routing_id_t::from (
                      mailbox_record.source_routing_id).to_string ();
                }
                trace_mesh_host (
                  "dispatch",
                  std::string ("kind=")
                    + std::to_string (static_cast<int> (kind))
                    + " source=" + source
                    + " parts="
                    + std::to_string (mailbox_record.parts.size ()));
                record.release_mailbox_reservation =
                  release_mailbox_reservation;
                record.retain_mailbox_reservation =
                  retain_mailbox_reservation;
                dispatch (
                  owner, record, decode_application (payload));
                ++count;
                application_dispatch_started = true;
            }
            catch (const protocol::service_wire_error_t &) {
                release_mailbox_reservation ();
                if (mailbox_record.request_sequence
                    && mailbox_record.correlation) {
                    (void) _transport->reply_failure (
                      mailbox_record, 104,
                      static_cast<std::uint32_t> (
                        protocol::framework_error_code::
                          requestProtocolError));
                }
            }
            catch (...) {
                release_mailbox_reservation ();
                throw;
            }
        }
        if (!claim_retained->load (std::memory_order_acquire)) {
            release_mailbox_reservation ();
        }
    }
    return count;
}

bool public_host_runtime_t::wait_for_dispatch_activity (
  std::chrono::milliseconds timeout,
  bool accept_application_receive) noexcept
{
    try {
        const auto local_deadline = next_local_spot_request_deadline ();
        auto effective_timeout = timeout;
        if (accept_application_receive) {
            std::lock_guard lock (_mutex);
            if (!_local_application_dispatches.empty ())
                return true;
        }
        if (local_deadline) {
            const auto now = std::chrono::steady_clock::now ();
            if (*local_deadline <= now)
                return true;
            const auto remaining =
              std::chrono::duration_cast<std::chrono::milliseconds> (
                *local_deadline - now);
            if (remaining <= std::chrono::milliseconds::zero ())
                return true;
            if (effective_timeout <= std::chrono::milliseconds::zero ()
                || remaining < effective_timeout) {
                effective_timeout = remaining;
            }
        }
        return _transport->wait_for_activity (
          effective_timeout, accept_application_receive);
    }
    catch (...) {
        return false;
    }
}

bool public_host_runtime_t::prepare_actor_transfer (
  const actor_transfer_prepare_t &prepare,
  actor_transfer_token_t &token,
  actor_transfer_prepare_result_t &result)
{
    auto actor = resolve_actor (prepare.actor);
    if (!actor) {
        return false;
    }
    stateful::stateful_error_t error =
      stateful::stateful_error_t::invalid;
    stateful::membership_token_t membership;
    if (prepare.role
        == actor_transfer_role_t::source) {
        std::tie (error, membership) =
          _objects.begin_remote_membership_move (
            *actor,
            stateful::object_ref_t{
              stateful::object_kind_t::user_spot,
              prepare.target_spot_id,
              prepare.target_spot_generation,
              prepare.target_spot_generation,
              _options.mesh.descriptor.mesh_name,
              prepare.target_node_rid.to_string ()});
    } else {
        auto target =
          resolve_spot (
            prepare.target_spot_id);
        if (!target)
            return false;
        std::tie (error, membership) =
          _objects.begin_membership_move (
            *actor, *target);
    }
    if (error != stateful::stateful_error_t::none)
        return false;
    token._host = shared_from_this ();
    token._membership = membership;
    token._role = prepare.role;
    token._membership_epoch = 0;
    token._terminal = false;
    result.current_actor = framework_actor_ref (
      *actor, std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (prepare.actor)));
    result.membership_epoch = actor->authority_owner_generation;
    return true;
}

bool public_host_runtime_t::reply (
  const reply_token_t &token,
  const std::vector<zlink::message_t> &parts)
{
    try {
        if (token.local_reply) {
            return token.local_reply (parts);
        }
        return token.request
               && _transport->reply (
                 *token.request, encode_application (parts));
    }
    catch (const zlink::submit_error_t &) {
        return false;
    }
}

std::optional<stateful::object_ref_t>
public_host_runtime_t::resolve_actor (const actor_ref_t &actor) const
{
    std::lock_guard lock (_mutex);
    const auto found = _actors.find (std::string (actor.actor_id ().value ()));
    if (found == _actors.end ()
        || found->second.second.object_generation
             != actor.object_generation ()) {
        return std::nullopt;
    }
    return found->second.second;
}

std::optional<stateful::object_ref_t>
public_host_runtime_t::resolve_spot (
  const std::string &spot_id) const
{
    std::lock_guard lock (_mutex);
    const auto found = _spots.find (spot_id);
    return found == _spots.end ()
             ? std::optional<stateful::object_ref_t>{}
             : std::make_optional (found->second);
}

protocol::application_payload_t
public_host_runtime_t::encode_application (
  const std::vector<zlink::message_t> &parts,
  std::span<const std::uint8_t>) const
{
    return {
      std::string (multipart_packet_name),
      std::string (multipart_content_type),
      encode_parts (parts)};
}

std::vector<zlink::message_t>
public_host_runtime_t::decode_application (
  const protocol::application_payload_t &payload) const
{
    if (payload.packet_name != multipart_packet_name
        || payload.content_type != multipart_content_type) {
        throw protocol::service_wire_error_t (
          "framework application payload profile is unsupported");
    }
    return decode_parts (payload.payload);
}

actor_ref_t public_host_runtime_t::framework_actor_ref (
  const stateful::object_ref_t &object,
  std::string actor_type) const
{
    return ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (object.node_id),
      std::move (actor_type),
      object.key,
      object.object_generation);
}

operation_id_t public_host_runtime_t::next_operation ()
{
    std::lock_guard lock (_mutex);
    const auto low = _next_operation++;
    if (low == 0 || _next_operation == 0) {
        _next_operation = 1;
        throw std::overflow_error (
          "framework public host operation id is exhausted");
    }
    return {status ().lifecycle_generation (), low};
}

bool public_host_runtime_t::try_reserve_completion (
  operation_id_t operation)
{
    return _completions.reserve (operation);
}

void public_host_runtime_t::release_completion (
  operation_id_t operation) noexcept
{
    (void) _completions.erase (operation);
}

bool public_host_runtime_t::enqueue_completion (
  operation_id_t operation,
  receive_record_t record,
  std::vector<zlink::message_t> parts)
{
    std::lock_guard lock (_mutex);
    if (!_started || _closing) {
        (void) _completions.erase (operation);
        return false;
    }
    try {
        return _completions.complete (
          operation, std::make_pair (std::move (record), std::move (parts)));
    }
    catch (...) {
        (void) _completions.erase (operation);
        return false;
    }
}

zlink::submit_result_t public_host_runtime_t::begin_local_actor_join (
  const actor_ref_t &actor,
  const std::string &target_spot_id,
  std::uint64_t target_spot_generation,
  const std::vector<zlink::message_t> &parts,
  operation_id_t &operation)
{
    operation = next_operation ();
    if (!try_reserve_completion (operation))
        return zlink::submit_result_t::backpressured;
    const auto current = resolve_actor (actor);
    const auto target = resolve_spot (target_spot_id);
    auto reject = [&] {
        receive_record_t completion;
        completion.kind = record_kind_t::completion;
        completion.domain = ready_domain_t::infrastructure;
        completion.operation_id = operation;
        completion.operation_kind = operation_kind_t::actor_join;
        completion.source_node_rid = status ().routing_id ();
        completion.join_completion = actor_join_completion_t{
          join_admission_t::rejected, actor};
        (void) enqueue_completion (
          operation, std::move (completion), {});
    };
    if (!current || !target
        || target->object_generation != target_spot_generation) {
        reject ();
        return zlink::submit_result_t::ok;
    }
    auto [error, membership] =
      _objects.begin_membership_move (*current, *target);
    if (error != stateful::stateful_error_t::none) {
        reject ();
        return zlink::submit_result_t::ok;
    }

    const auto actor_type = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor));
    std::weak_ptr<public_host_runtime_t> weak = shared_from_this ();
    ready_record_t owner{
      .owner_kind = owner_kind_t::spot,
      .domain = ready_domain_t::application,
      .spot_id = target_spot_id};
    receive_record_t record;
    record.kind = record_kind_t::spot_control;
    record.domain = ready_domain_t::application;
    record.operation_id = operation;
    record.operation_kind = operation_kind_t::actor_join;
    record.source_node_rid = status ().routing_id ();
    record.actor_control = actor_control_t{
      lifecycle_kind_t::joined, actor};
    record.reply_token.local_actor_join =
      [weak, operation, actor_type, membership] (
        actor_join_result_t result,
        const std::vector<zlink::message_t> &reply) {
          const auto host = weak.lock ();
          return host
                 && host->complete_local_actor_join (
                   operation, actor_type, membership, result, reply);
      };
    {
        std::lock_guard lock (_mutex);
        _local_application_dispatches.push_back (
          local_application_dispatch_t{
            std::move (owner), std::move (record), parts});
    }
    return zlink::submit_result_t::ok;
}

bool public_host_runtime_t::complete_local_actor_join (
  operation_id_t operation,
  std::string actor_type,
  stateful::membership_token_t membership,
  actor_join_result_t result,
  const std::vector<zlink::message_t> &parts)
{
    receive_record_t completion;
    completion.kind = record_kind_t::completion;
    completion.domain = ready_domain_t::infrastructure;
    completion.operation_id = operation;
    completion.operation_kind = operation_kind_t::actor_join;
    completion.source_node_rid = status ().routing_id ();

    if (result == actor_join_result_t::accepted) {
        auto [error, current] =
          _objects.commit_membership_move (membership);
        if (error != stateful::stateful_error_t::none) {
            completion.terminal_result = 1;
            completion.join_completion = actor_join_completion_t{
              join_admission_t::rejected,
              framework_actor_ref (membership.actor, actor_type)};
        } else {
            const auto actor =
              framework_actor_ref (current, actor_type);
            {
                std::lock_guard lock (_mutex);
                const auto found = _actors.find (current.key);
                if (found != _actors.end ())
                    found->second.second = current;
            }
            completion.join_completion = actor_join_completion_t{
              join_admission_t::accepted, actor};
        }
    } else {
        (void) _objects.abort_membership_move (membership);
        completion.join_completion = actor_join_completion_t{
          join_admission_t::rejected,
          framework_actor_ref (membership.actor, actor_type)};
    }

    return enqueue_completion (
      operation, std::move (completion), parts);
}

bool public_host_runtime_t::enqueue_local_actor_message (
  const actor_ref_t &target,
  record_kind_t kind,
  const std::vector<zlink::message_t> &parts,
  std::optional<operation_id_t> operation)
{
    if (kind != record_kind_t::actor_send
        && kind != record_kind_t::actor_request) {
        return false;
    }
    const auto current = resolve_actor (target);
    if (!current) {
        return false;
    }

    ready_record_t owner{
      .owner_kind = owner_kind_t::actor,
      .domain = ready_domain_t::application,
      .actor = framework_actor_ref (
        *current, std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (target)))};
    receive_record_t record;
    record.kind = kind;
    record.domain = ready_domain_t::application;
    record.source_node_rid = status ().routing_id ();
    if (operation) {
        record.operation_id = *operation;
        std::weak_ptr<public_host_runtime_t> weak =
          shared_from_this ();
        record.reply_token.host = weak;
        record.reply_token.local_reply =
          [weak, operation = *operation] (
            const std::vector<zlink::message_t> &reply) {
              const auto host = weak.lock ();
              return host
                     && host->complete_local_request (
                       operation, reply);
          };
    }
    std::lock_guard lock (_mutex);
    _local_application_dispatches.push_back (
      local_application_dispatch_t{
        std::move (owner), std::move (record), parts});
    return true;
}

zlink::submit_result_t public_host_runtime_t::enqueue_local_spot_request (
  const protocol::spot_route_fence_t &target,
  const std::vector<zlink::message_t> &parts,
  operation_id_t operation,
  std::chrono::milliseconds timeout,
  std::span<const std::uint8_t> metadata,
  spot_request_completion_t completion)
{
    const auto local = status ();
    if (target.target_node_routing_id != local.routing_id ().to_bytes ()
        || target.target_node_generation != local.lifecycle_generation ()) {
        return zlink::submit_result_t::not_found;
    }
    const auto object = resolve_spot (target.spot_id);
    if (!object
        || object->object_generation != target.object_generation
        || object->node_id != local.routing_id ().to_string ()) {
        return zlink::submit_result_t::not_found;
    }

    std::size_t payload_bytes = runtime::dispatch_limits::fixed_work_byte_cost;
    const auto add_bytes = [&payload_bytes] (std::size_t bytes) {
        if (bytes > std::numeric_limits<std::size_t>::max () - payload_bytes)
            return false;
        payload_bytes += bytes;
        return true;
    };
    for (const auto &part : parts) {
        if (!add_bytes (part.size ()))
            return zlink::submit_result_t::backpressured;
    }
    if (!add_bytes (metadata.size ()))
        return zlink::submit_result_t::backpressured;
    const auto byte_budget = _options.mesh.application_byte_budget;
    if (payload_bytes > byte_budget) {
        return zlink::submit_result_t::backpressured;
    }

    ready_record_t owner{
      .owner_kind = owner_kind_t::spot,
      .domain = ready_domain_t::application,
      .spot_id = target.spot_id};
    receive_record_t record;
    record.kind = record_kind_t::spot_request;
    record.domain = ready_domain_t::application;
    record.operation_id = operation;
    record.source_node_rid = local.routing_id ();
    record.spot_route = target;
    std::weak_ptr<public_host_runtime_t> weak = shared_from_this ();
    record.reply_token.host = weak;
    record.reply_token.local_reply =
      [weak, operation] (const std::vector<zlink::message_t> &reply) {
          const auto host = weak.lock ();
          if (!host) {
              return false;
          }
          return host->finish_local_spot_request (
            operation,
            foundation::operation_terminal_t::completed,
            result_t<std::vector<zlink::message_t>>::success (reply));
      };

    const auto deadline = std::chrono::steady_clock::now () + timeout;
    std::lock_guard lock (_mutex);
    if (!_started || _closing) {
        return zlink::submit_result_t::terminated;
    }
    const auto message_budget = _options.mesh.application_message_budget;
    if (message_budget != 0 && _local_spot_requests.size () >= message_budget) {
        return zlink::submit_result_t::backpressured;
    }
    if (_local_spot_request_bytes > byte_budget
        || payload_bytes > byte_budget - _local_spot_request_bytes) {
        return zlink::submit_result_t::backpressured;
    }
    auto [found, inserted] = _local_spot_requests.emplace (
      operation,
      local_spot_request_state_t{
        deadline, payload_bytes, std::move (completion), {}, true, false});
    if (!inserted) {
        return zlink::submit_result_t::internal_error;
    }
    bool deadline_indexed = false;
    bool bytes_reserved = false;
    try {
        auto deadline_entry = _local_spot_request_deadlines.emplace (
          deadline, operation);
        deadline_indexed = true;
        found->second.deadline_index = deadline_entry;
        _local_spot_request_bytes += payload_bytes;
        bytes_reserved = true;
        _local_application_dispatches.push_back (
          local_application_dispatch_t{
            std::move (owner), std::move (record), parts});
    }
    catch (...) {
        if (bytes_reserved)
            _local_spot_request_bytes -= payload_bytes;
        if (deadline_indexed)
            _local_spot_request_deadlines.erase (
              found->second.deadline_index);
        _local_spot_requests.erase (found);
        throw;
    }
    return zlink::submit_result_t::ok;
}

bool public_host_runtime_t::finish_local_spot_request (
  operation_id_t operation,
  foundation::operation_terminal_t terminal,
  result_t<std::vector<zlink::message_t>> result) noexcept
{
    local_spot_request_state_t pending{};
    try {
        {
            std::lock_guard lock (_mutex);
            const auto found = _local_spot_requests.find (operation);
            if (found == _local_spot_requests.end ()
                || found->second.terminal_claimed) {
                return false;
            }
            pending.deadline = found->second.deadline;
            pending.payload_bytes = found->second.payload_bytes;
            pending.completion = std::move (found->second.completion);
            pending.queued = found->second.queued;
            pending.terminal_claimed = true;
            _local_spot_request_deadlines.erase (
              found->second.deadline_index);
            found->second.terminal_claimed = true;
            if (!found->second.queued) {
                if (pending.payload_bytes <= _local_spot_request_bytes) {
                    _local_spot_request_bytes -= pending.payload_bytes;
                } else {
                    _local_spot_request_bytes = 0;
                }
                _local_spot_requests.erase (found);
            }
        }

        if (pending.completion) {
            release_completion (operation);
            try {
                pending.completion (terminal, std::move (result));
            }
            catch (...) {
            }
            return true;
        }

        if (terminal == foundation::operation_terminal_t::completed && result) {
            const auto queued = complete_local_request (operation, result.value ());
            if (!queued) {
                release_completion (operation);
            }
            return queued;
        }

        complete_operation (
          operation, operation_kind_t::none,
          terminal == foundation::operation_terminal_t::completed
            ? foundation::operation_terminal_t::transport_failed
            : terminal,
          {});
        return true;
    }
    catch (...) {
        release_completion (operation);
        return false;
    }
}

void public_host_runtime_t::expire_local_spot_requests () noexcept
{
    try {
        for (;;) {
            std::optional<operation_id_t> expired;
            {
                std::lock_guard lock (_mutex);
                const auto now = std::chrono::steady_clock::now ();
                if (!_local_spot_request_deadlines.empty ()
                    && _local_spot_request_deadlines.begin ()->first <= now) {
                    expired = _local_spot_request_deadlines.begin ()->second;
                }
            }
            if (!expired) {
                return;
            }
            (void) finish_local_spot_request (
              *expired,
              foundation::operation_terminal_t::timed_out,
              result_t<std::vector<zlink::message_t>>::failure (
                framework_error_kind_t::deadline_exceeded,
                "SPOT request timed out before local dispatch"));
        }
    }
    catch (...) {
    }
}

void public_host_runtime_t::terminate_local_spot_requests (
  foundation::operation_terminal_t terminal) noexcept
{
    try {
        for (;;) {
            std::optional<operation_id_t> pending_operation;
            {
                std::lock_guard lock (_mutex);
                for (const auto &[operation, pending] : _local_spot_requests) {
                    if (!pending.terminal_claimed) {
                        pending_operation = operation;
                        break;
                    }
                }
                if (!pending_operation)
                    return;
            }
            const auto error_kind =
              terminal == foundation::operation_terminal_t::shutdown
                ? framework_error_kind_t::shutting_down
                : framework_error_kind_t::internal_failure;
            (void) finish_local_spot_request (
              *pending_operation, terminal,
              result_t<std::vector<zlink::message_t>>::failure (
                error_kind,
                "SPOT request stopped because the runtime is shutting down"));
        }
    }
    catch (...) {
    }
}

std::optional<std::chrono::steady_clock::time_point>
public_host_runtime_t::next_local_spot_request_deadline () const
{
    std::lock_guard lock (_mutex);
    if (_local_spot_request_deadlines.empty ())
        return std::nullopt;
    return _local_spot_request_deadlines.begin ()->first;
}

bool public_host_runtime_t::complete_local_request (
  operation_id_t operation,
  const std::vector<zlink::message_t> &parts)
{
    receive_record_t completion;
    completion.kind = record_kind_t::completion;
    completion.domain = ready_domain_t::infrastructure;
    completion.operation_id = operation;
    completion.source_node_rid = status ().routing_id ();
    return enqueue_completion (
      operation, std::move (completion), parts);
}

void public_host_runtime_t::complete_operation (
  operation_id_t operation,
  operation_kind_t kind,
  foundation::operation_terminal_t terminal,
  std::vector<std::uint8_t> payload)
{
    try {
        {
            std::lock_guard lock (_mutex);
            if (!_started || _closing) {
                (void) _completions.erase (operation);
                return;
            }
        }
        receive_record_t record;
        record.kind = record_kind_t::completion;
        record.domain = ready_domain_t::infrastructure;
        record.operation_id = operation;
        record.operation_kind = kind;
        record.source_node_rid = status ().routing_id ();
        switch (terminal) {
            case foundation::operation_terminal_t::completed:
                record.terminal_result = 0;
                break;
            case foundation::operation_terminal_t::timed_out:
                record.terminal_result = static_cast<int> (
                  zlink::request_result_t::timed_out);
                break;
            case foundation::operation_terminal_t::shutdown:
                record.terminal_result = static_cast<int> (
                  zlink::request_result_t::terminated);
                break;
            default:
                record.terminal_result = static_cast<int> (
                  zlink::request_result_t::internal_error);
                break;
        }
        std::vector<zlink::message_t> parts;
        if (record.terminal_result == 0) {
            try {
                parts = decode_application (
                  protocol::decode_application_payload (payload));
            }
            catch (const protocol::service_wire_error_t &) {
                record.terminal_result = static_cast<int> (
                  zlink::request_result_t::protocol_error);
            }
        }
        if (!enqueue_completion (
              operation, std::move (record), std::move (parts)))
            release_completion (operation);
    }
    catch (...) {
        release_completion (operation);
    }
}

bool actor_transfer_token_t::valid () const noexcept
{
    return !_terminal && _membership.value != 0 && !_host.expired ();
}

bool actor_transfer_token_t::commit (
  std::uint64_t membership_epoch)
{
    auto host = _host.lock ();
    if (!host || _terminal) {
        return false;
    }
    if (_role == actor_transfer_role_t::target) {
        if (membership_epoch == 0)
            return false;
        _membership_epoch = membership_epoch;
        return true;
    }
    const auto [error, current] =
      host->objects ().commit_membership_move (_membership);
    _terminal = true;
    if (error == stateful::stateful_error_t::none) {
        std::lock_guard lock (host->_mutex);
        const auto found = host->_actors.find (_membership.actor.key);
        if (found != host->_actors.end ())
            found->second.second = current;
    }
    return error == stateful::stateful_error_t::none;
}

bool actor_transfer_token_t::activate ()
{
    auto host = _host.lock ();
    if (!host || _terminal
        || _role
             != actor_transfer_role_t::target
        || _membership_epoch == 0)
        return false;
    const auto [error, current] =
      host->objects ().commit_membership_move (
        _membership);
    _terminal = true;
    if (error == stateful::stateful_error_t::none) {
        std::lock_guard lock (host->_mutex);
        const auto found = host->_actors.find (_membership.actor.key);
        if (found != host->_actors.end ())
            found->second.second = current;
    }
    return error
           == stateful::stateful_error_t::none;
}

void actor_transfer_token_t::abort () noexcept
{
    if (auto host = _host.lock (); host && !_terminal) {
        (void) host->objects ().abort_membership_move (_membership);
    }
    _terminal = true;
}

zlink::submit_result_t reply (
  const reply_token_t &token,
  const std::vector<zlink::message_t> &parts)
{
    const auto host = token.host.lock ();
    return host && host->reply (token, parts)
             ? zlink::submit_result_t::ok
             : zlink::submit_result_t::terminated;
}

bool actor_join_reply (
  const reply_token_t &token,
  actor_join_result_t result,
  const std::vector<zlink::message_t> &parts)
{
    if (token.local_actor_join) {
        return token.local_actor_join (result, parts);
    }
    if (result != actor_join_result_t::accepted) {
        const auto host = token.host.lock ();
        if (!host || !token.request) {
            return false;
        }
        try {
            return host->transport ().reply_failure (
              *token.request, 106,
              static_cast<std::uint32_t> (
                protocol::framework_error_code::requestRejected));
        }
        catch (const zlink::submit_error_t &) {
            return false;
        }
    }
    return reply (token, parts) == zlink::submit_result_t::ok;
}

} // namespace zlink::framework::runtime::host
