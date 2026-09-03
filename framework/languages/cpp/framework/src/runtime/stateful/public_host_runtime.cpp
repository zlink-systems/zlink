/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/locations/pending_creation_projection.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"

#include <service_wire_constants.hpp>
#include <service_wire_pilot_codec.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace zlink::framework::runtime::host
{

bool
bound_session_bind_actor_matches (
  const protocol::actor_route_fence_t &requested,
  const std::optional<stateful::object_ref_t> &local_actor,
  const zlink::routing_id_t &local_routing_id,
  std::uint64_t local_node_generation) noexcept
{
    return local_actor && local_actor->key == requested.actor_id
           && local_actor->object_generation == requested.object_generation
           && local_actor->authority_owner_generation == requested.authority_owner_generation
           && local_actor->node_id == local_routing_id.to_string ()
           && requested.target_node_routing_id == local_routing_id.to_bytes ()
           && requested.target_node_generation == local_node_generation;
}

bound_session_bind_admission_t
classify_bound_session_bind_admission (bool local_actor_matches) noexcept
{
    return local_actor_matches ? bound_session_bind_admission_t::ready
                               : bound_session_bind_admission_t::actor_not_ready;
}

namespace
{

constexpr std::string_view multipart_packet_name = protocol::framework_multipart_packet_name;
constexpr std::string_view multipart_content_type = protocol::framework_multipart_content_type;

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

/* Full-vocabulary 1:1 decode of an explicit relocationFailed(53) wire
 * failure_code into cpp's typed classification, so a source that receives
 * one can act on the actual reason instead of the code being ignored.
 * relocationDataLost(35) decodes to data_lost — the target's own encode
 * side (complete_relocation_assembly / dispatch_user_spot_operations)
 * reserves 35 for a verified checksum/assembly/digest/conflict integrity
 * failure; every other wire code here maps to the kind that already
 * carries that meaning elsewhere in the framework (e.g. requestFailed(17)
 * -> internal_failure, the same target-side restore/factory/staging
 * failure class). An unrecognized code is internal_failure, not silently
 * dropped. */
framework_error_kind_t map_relocation_failure_code (std::uint32_t wire_code) noexcept
{
    switch (static_cast<protocol::framework_error_code> (wire_code)) {
        case protocol::framework_error_code::relocationDataLost:
            return framework_error_kind_t::data_lost;
        case protocol::framework_error_code::requestRejected:
            return framework_error_kind_t::rejected;
        case protocol::framework_error_code::requestProtocolError:
        case protocol::framework_error_code::payloadDecodeFailed:
            return framework_error_kind_t::protocol_error;
        case protocol::framework_error_code::workerQueueFull:
            return framework_error_kind_t::capacity_exceeded;
        case protocol::framework_error_code::workerTimedOut:
            return framework_error_kind_t::deadline_exceeded;
        case protocol::framework_error_code::actorTypeMismatch:
        case protocol::framework_error_code::spotTypeMismatch:
            return framework_error_kind_t::type_mismatch;
        case protocol::framework_error_code::handlerNotFound:
            return framework_error_kind_t::not_configured;
        case protocol::framework_error_code::routeNotConnected:
        case protocol::framework_error_code::spotMoving:
            return framework_error_kind_t::unavailable;
        case protocol::framework_error_code::actorRouteNotFound:
        case protocol::framework_error_code::spotRouteNotFound:
        case protocol::framework_error_code::requestTargetNotFound:
        case protocol::framework_error_code::routeHandlerNotFound:
        case protocol::framework_error_code::actorDispatchHandlerNotFound:
            return framework_error_kind_t::not_found;
        case protocol::framework_error_code::actorAlreadyExists:
        case protocol::framework_error_code::actorCreateRejected:
            return framework_error_kind_t::already_exists;
        case protocol::framework_error_code::actorSessionNotBound:
        case protocol::framework_error_code::actorLocationStale:
        case protocol::framework_error_code::spotGenerationStale:
            return framework_error_kind_t::invalid_operation;
        case protocol::framework_error_code::actorCreateFailed:
        case protocol::framework_error_code::spotCreateFailed:
        case protocol::framework_error_code::requestFailed:
        case protocol::framework_error_code::workerFailed:
        case protocol::framework_error_code::none:
        default:
            return framework_error_kind_t::internal_failure;
    }
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

bool user_spot_operation_replay_expired (std::uint64_t deadline_unix_ms,
                                         std::int64_t now_unix_ms,
                                         std::chrono::milliseconds replay_retention)
{
    return now_unix_ms >= 0 && static_cast<std::uint64_t> (now_unix_ms) > deadline_unix_ms
           && static_cast<std::uint64_t> (now_unix_ms) - deadline_unix_ms
                > static_cast<std::uint64_t> (
                  std::max<std::int64_t> (0, replay_retention.count ()));
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

auto session_relocation_key (const protocol::session_relocation_seal_t &seal)
{
    return std::tuple{seal.relocation.high,    seal.relocation.low,
                      seal.actor.actor_id,     seal.actor.object_generation,
                      seal.session_routing_id, seal.binding_generation};
}

auto session_relocation_key (const protocol::session_relocation_route_t &route)
{
    return std::tuple{route.relocation.high,    route.relocation.low,
                      route.actor.actor_id,     route.actor.object_generation,
                      route.session_routing_id, route.binding_generation};
}

void append_u32 (std::vector<std::uint8_t> &out, std::uint32_t value)
{
    out.push_back (static_cast<std::uint8_t> ((value >> 24u) & 0xffu));
    out.push_back (static_cast<std::uint8_t> ((value >> 16u) & 0xffu));
    out.push_back (static_cast<std::uint8_t> ((value >> 8u) & 0xffu));
    out.push_back (static_cast<std::uint8_t> (value & 0xffu));
}

std::uint32_t read_u32 (const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
    if (offset + 4 > bytes.size ()) {
        throw protocol::service_wire_error_t ("framework multipart payload is truncated");
    }
    const auto value = (static_cast<std::uint32_t> (bytes[offset]) << 24u)
                       | (static_cast<std::uint32_t> (bytes[offset + 1]) << 16u)
                       | (static_cast<std::uint32_t> (bytes[offset + 2]) << 8u)
                       | static_cast<std::uint32_t> (bytes[offset + 3]);
    offset += 4;
    return value;
}

std::vector<std::uint8_t> encode_parts (const std::vector<zlink::message_t> &parts)
{
    if (parts.empty ()) {
        throw std::invalid_argument ("framework multipart requires at least one part");
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

std::vector<zlink::message_t> decode_parts (const std::vector<std::uint8_t> &encoded)
{
    std::size_t offset = 0;
    const auto count = read_u32 (encoded, offset);
    if (count == 0 || count > (encoded.size () - offset) / sizeof (std::uint32_t)) {
        throw protocol::service_wire_error_t ("framework multipart part count is invalid");
    }
    std::vector<zlink::message_t> parts;
    parts.reserve (count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto size = read_u32 (encoded, offset);
        if (size > encoded.size () - offset) {
            throw protocol::service_wire_error_t ("framework multipart part is truncated");
        }
        parts.push_back (
          zlink::message_t::from (std::span<const std::uint8_t> (encoded.data () + offset, size)));
        offset += size;
    }
    if (offset != encoded.size ()) {
        throw protocol::service_wire_error_t ("framework multipart payload has trailing bytes");
    }
    return parts;
}

struct canonical_actor_join_decode_t
{
    protocol::actor_join_request_t request;
    std::optional<protocol::application_payload_t> payload;
};

std::optional<canonical_actor_join_decode_t>
try_decode_canonical_actor_join (const std::vector<std::vector<std::uint8_t>> &parts)
{
    try {
        const auto decoded = protocol::decode_actor_join_28 (parts);
        canonical_actor_join_decode_t result{
          protocol::actor_join_request_t{
            decoded.correlation,
            protocol::actor_route_fence_t{decoded.actor.id, decoded.actor.generation,
                                          decoded.actor.target_node_rid,
                                          decoded.actor.target_node_generation,
                                          decoded.actor.expected_authority_owner_generation,
                                          decoded.actor.expected_owner_lease_generation},
            decoded.entry,
            protocol::spot_route_fence_t{decoded.target_spot.id, decoded.target_spot.generation,
                                         decoded.target_spot.target_node_rid,
                                         decoded.target_spot.target_node_generation,
                                         decoded.target_spot.expected_authority_owner_generation,
                                         decoded.target_spot.expected_owner_lease_generation}},
          std::nullopt};
        if (decoded.payload) {
            result.payload = protocol::application_payload_t{decoded.payload->packet_name,
                                                             decoded.payload->content_type,
                                                             decoded.payload->payload};
        }
        return result;
    }
    catch (const std::exception &) {
        return std::nullopt;
    }
}

struct route_owner_fence_read_t
{
    host::route_fence_t fence;
    std::optional<std::chrono::steady_clock::duration> admission_lifetime;
};

std::optional<route_owner_fence_read_t>
read_route_owner_fence (const std::shared_ptr<zlink::framework::location_repository_t> &store,
                        char object_kind,
                        std::string_view object_id,
                        std::uint64_t object_generation,
                        std::uint64_t authority_owner_generation,
                        std::uint64_t owner_lease_generation,
                        /* No default: the caller must pass its configured
   * location_options_t::owner_lease_fencing_margin. A hardcoded margin
   * larger than the deployment's owner_lease_ttl makes every store-backed
   * fence read return nullopt (permanent stale_route). */
                        std::chrono::milliseconds owner_lease_fencing_margin)
{
    if (object_id.empty () || object_generation == 0)
        return std::nullopt;
    if (authority_owner_generation != 0 || owner_lease_generation != 0) {
        if (authority_owner_generation == 0 || owner_lease_generation == 0)
            return std::nullopt;
        return route_owner_fence_read_t{{authority_owner_generation, owner_lease_generation},
                                        std::nullopt};
    }
    if (!store) {
        trace_mesh_host ("route-owner-fence-read", "reason=no-store");
        return std::nullopt;
    }
    try {
        auto read = store
                      ->read_authority (object_kind == '1' ? actor_authority_key (object_id)
                                                           : spot_authority_key (object_id))
                      .result ();
        if (!read) {
            trace_mesh_host ("route-owner-fence-read", "reason=authority-read-failed");
            return std::nullopt;
        }
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
        if (!snapshot || snapshot->authority_owner_generation == 0
            || snapshot->owner.lease_generation <= 0) {
            trace_mesh_host (
              "route-owner-fence-read",
              std::string ("reason=snapshot-mismatch snapshot=")
                + (snapshot
                    ? "generation=" + std::to_string (snapshot->object_generation)
                         + " authority=" + std::to_string (snapshot->authority_owner_generation)
                         + " lease=" + std::to_string (snapshot->owner.lease_generation)
                     : "missing")
                + " expected_generation=" + std::to_string (object_generation));
            return std::nullopt;
        }
        location_options_t location_options;
        location_options.owner_lease_fencing_margin = owner_lease_fencing_margin;
        live_location_reader_t live (*store, std::move (location_options));
        const auto admission_lifetime = live.owner_admission_lifetime (snapshot->owner);
        if (!admission_lifetime) {
            trace_mesh_host ("route-owner-fence-read",
                             "reason=admission-lifetime-null owner=" + snapshot->owner.owner_id
                               + " lease=" + std::to_string (snapshot->owner.lease_generation)
                               + " margin_ms="
                               + std::to_string (owner_lease_fencing_margin.count ()));
            return std::nullopt;
        }
        return route_owner_fence_read_t{
          {snapshot->authority_owner_generation,
           static_cast<std::uint64_t> (snapshot->owner.lease_generation)},
          admission_lifetime};
    }
    catch (...) {
        return std::nullopt;
    }
}

std::string spot_route_cache_key (const zlink::routing_id_t &target_node_rid,
                                  std::string_view target_spot_id,
                                  std::uint64_t target_spot_generation)
{
    return target_node_rid.to_string () + ":" + std::string (target_spot_id) + ":"
           + std::to_string (target_spot_generation);
}

zlink::submit_result_t submitted (bool accepted)
{
    return accepted ? zlink::submit_result_t::ok : zlink::submit_result_t::not_connected;
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
            throw protocol::service_wire_error_t ("mailbox record is not application messaging");
    }
}

operation_kind_t operation_kind (record_kind_t)
{
    return operation_kind_t::none;
}

bool is_request (record_kind_t kind)
{
    return kind == record_kind_t::node_request || kind == record_kind_t::channel_request
           || kind == record_kind_t::spot_request || kind == record_kind_t::actor_request;
}

std::vector<std::vector<std::uint8_t>>
unpack_infrastructure_reply (const std::vector<std::uint8_t> &packed)
{
    if (packed.empty () || packed.front () == 0 || packed.front () > 2) {
        throw protocol::service_wire_error_t ("invalid packed infrastructure reply");
    }
    std::size_t offset = 1;
    std::vector<std::vector<std::uint8_t>> parts;
    parts.reserve (packed.front ());
    for (std::uint8_t index = 0; index < packed.front (); ++index) {
        const auto length = read_u32 (packed, offset);
        if (packed.size () - offset < length) {
            throw protocol::service_wire_error_t ("truncated packed infrastructure reply");
        }
        parts.emplace_back (packed.begin () + static_cast<std::ptrdiff_t> (offset),
                            packed.begin () + static_cast<std::ptrdiff_t> (offset + length));
        offset += length;
    }
    if (offset != packed.size ()) {
        throw protocol::service_wire_error_t ("packed infrastructure reply has trailing bytes");
    }
    return parts;
}

std::string user_spot_operation_key (const std::vector<std::uint8_t> &source,
                                     std::uint64_t source_generation,
                                     const protocol::wire_operation_id_t &operation)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill ('0');
    for (const auto value : source)
        stream << std::setw (2) << static_cast<unsigned> (value);
    stream << ':' << source_generation << ':' << operation.high << ':' << operation.low;
    return stream.str ();
}

std::vector<std::byte> ready_user_spot_authority_payload (const stateful::object_ref_t &object,
                                                          const std::string &stable_type,
                                                          const object_creation_target_t &target)
{
    // The authority payload is framework-owned. Application creation bytes
    // remain in the reservation projection and are never published as Ready.
    if (target.owner.lease_generation <= 0)
        throw std::invalid_argument ("user Spot authority owner lease is invalid");
    return encode_user_spot_authority_payload (
      {.state = user_spot_authority_state_t::ready,
       .stable_type = stable_type,
       .spot_id = object.key,
       .owner_id = target.owner.owner_id,
       .owner_lease_generation = static_cast<std::uint64_t> (target.owner.lease_generation),
       .mesh_name = target.mesh_name,
       .node_rid = target.node_rid,
       .node_generation = target.node_lifecycle_generation});
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

void append_u64_value (std::vector<std::uint8_t> &out, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back (static_cast<std::uint8_t> (value >> shift));
}

std::uint64_t read_u64_value (std::span<const std::uint8_t> bytes, std::size_t &offset)
{
    if (bytes.size () - offset < 8)
        throw protocol::service_wire_error_t ("Instance authority payload is truncated");
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index)
        value = (value << 8) | bytes[offset++];
    return value;
}

void append_text32_value (std::vector<std::uint8_t> &out, const std::string &value)
{
    if (value.size () > std::numeric_limits<std::uint32_t>::max ())
        throw protocol::service_wire_error_t ("Instance authority text exceeds u32");
    append_u32 (out, static_cast<std::uint32_t> (value.size ()));
    out.insert (out.end (), value.begin (), value.end ());
}

std::string read_text32_value (const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
    const auto size = read_u32 (bytes, offset);
    if (bytes.size () - offset < size)
        throw protocol::service_wire_error_t ("Instance authority text is truncated");
    std::string value (bytes.begin () + static_cast<std::ptrdiff_t> (offset),
                       bytes.begin () + static_cast<std::ptrdiff_t> (offset + size));
    offset += size;
    return value;
}

std::vector<std::byte> encode_instance_ready_state (const instance_ready_state_t &state)
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
    } else {
        bytes.push_back (0);
    }
    std::vector<std::byte> result;
    result.reserve (bytes.size ());
    for (const auto value : bytes)
        result.push_back (static_cast<std::byte> (value));
    return result;
}

std::optional<instance_ready_state_t>
decode_instance_ready_state (const std::vector<std::byte> &payload, bool capture_flow)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve (payload.size ());
    for (const auto value : payload)
        bytes.push_back (std::to_integer<std::uint8_t> (value));
    if (bytes.size () < 5 || bytes[0] != 'Z' || bytes[1] != 'L' || bytes[2] != 'I'
        || bytes[3] != 'R' || bytes[4] != 1)
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
            /* flow-correlation §4: at Off the persisted reply's flow pair is
             * neither validated nor materialized (structural skip only). */
            state.reply = protocol::decode_application_payload (
              std::span<const std::uint8_t> (bytes).subspan (offset, size), capture_flow);
            offset += size;
        }
        if (offset != bytes.size () || state.stable_type.empty () || state.spot_id.empty ()
            || state.object_generation == 0 || state.authority_owner_generation == 0
            || (state.operation.high == 0 && state.operation.low == 0))
            return std::nullopt;
        return state;
    }
    catch (const protocol::service_wire_error_t &) {
        return std::nullopt;
    }
}

std::vector<std::byte> closing_user_spot_authority_payload (const stateful::object_ref_t &object,
                                                            const std::string &stable_type,
                                                            const object_creation_target_t &target)
{
    if (target.owner.lease_generation <= 0)
        throw std::invalid_argument ("user Spot authority owner lease is invalid");
    return encode_user_spot_authority_payload (
      {.state = user_spot_authority_state_t::closing,
       .stable_type = stable_type,
       .spot_id = object.key,
       .owner_id = target.owner.owner_id,
       .owner_lease_generation = static_cast<std::uint64_t> (target.owner.lease_generation),
       .mesh_name = target.mesh_name,
       .node_rid = target.node_rid,
       .node_generation = target.node_lifecycle_generation});
}

struct instance_closing_state_t
{
    std::string stable_type;
    std::string spot_id;
    std::uint64_t object_generation = 0;
    std::uint64_t authority_owner_generation = 0;
};

std::vector<std::byte> encode_instance_closing_state (const instance_closing_state_t &state)
{
    const std::string value = "zlink:instance-spot:closing:v1\n" + state.stable_type + "\n"
                              + state.spot_id + "\n" + std::to_string (state.object_generation)
                              + "\n" + std::to_string (state.authority_owner_generation);
    std::vector<std::byte> result;
    result.reserve (value.size ());
    for (const auto character : value)
        result.push_back (static_cast<std::byte> (static_cast<unsigned char> (character)));
    return result;
}

std::optional<instance_closing_state_t>
decode_instance_closing_state (const std::vector<std::byte> &payload)
{
    std::string value;
    value.reserve (payload.size ());
    for (const auto character : payload)
        value.push_back (static_cast<char> (std::to_integer<unsigned char> (character)));
    constexpr std::string_view prefix = "zlink:instance-spot:closing:v1\n";
    if (value.rfind (prefix, 0) != 0)
        return std::nullopt;
    std::vector<std::string> fields;
    std::size_t begin = prefix.size ();
    while (begin <= value.size ()) {
        const auto end = value.find ('\n', begin);
        fields.push_back (
          value.substr (begin, end == std::string::npos ? std::string::npos : end - begin));
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
        return instance_closing_state_t{fields[0], fields[1], object_generation, owner_generation};
    }
    catch (...) {
        return std::nullopt;
    }
}

std::uint64_t unix_milliseconds_now ()
{
    return static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                         std::chrono::system_clock::now ().time_since_epoch ())
                                         .count ());
}

zlink::framework::object_reservation_fence_t
public_fence (const protocol::user_spot_reservation_fence_t &wire,
              const std::string &mesh_name,
              const std::string &stable_type)
{
    if (wire.target_owner_lease_generation
        > static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ()))
        throw protocol::service_wire_error_t (
          "target owner lease generation exceeds the public Store range");
    return {
      wire.reservation_id,
      wire.expected_store_version,
      wire.object_generation,
      wire.authority_owner_generation,
      {mesh_name,
       node_rid_t::from_string (
         zlink::routing_id_t::from (wire.target_node_routing_id).to_string ()),
       wire.target_node_generation,
       {wire.target_owner_id, static_cast<std::int64_t> (wire.target_owner_lease_generation)}},
      {0, wire.pending_capacity_delta,
       spot_type_capacity_delta_t{placement_object_kind_t::user_spot, stable_type,
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

spot_handle_t::spot_handle_t (std::shared_ptr<public_host_runtime_t> host,
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

task_t<zlink::submit_result_t>
spot_handle_t::send_to_spot (const zlink::routing_id_t &target_node_rid,
                             const std::string &target_spot_id,
                             std::uint64_t target_spot_generation,
                             const std::vector<zlink::message_t> &parts,
                             zlink::send_flags_t,
                             std::span<const std::uint8_t> metadata)
{
    if (!_host) {
        co_return zlink::submit_result_t::invalid_handle;
    }
    const auto peer = _host->transport ().topology ().peer (target_node_rid.to_bytes ());
    const auto target_node_generation =
      peer ? peer->descriptor.lifecycle_generation : _host->status ().lifecycle_generation ();
    const auto route_fence =
      _host->resolve_spot_route_fence (target_node_rid, target_spot_id, target_spot_generation);
    if (!route_fence)
        co_return zlink::submit_result_t::not_found;
    const auto target = protocol::spot_route_fence_t{
      target_spot_id,         target_spot_generation, target_node_rid.to_bytes (),
      target_node_generation, route_fence->first,     route_fence->second};
    if (target.target_node_routing_id == _host->status ().routing_id ().to_bytes ()) {
        co_return _host->enqueue_local_spot_send (target, parts);
    }
    co_return co_await _host->transport ().send_to_spot_result (
      target_node_rid.to_bytes (), spot_id (), target, _host->encode_application (parts, metadata));
}

task_t<zlink::submit_result_t>
spot_handle_t::request_to_spot (const zlink::routing_id_t &target_node_rid,
                                const std::string &target_spot_id,
                                std::uint64_t target_spot_generation,
                                const std::vector<zlink::message_t> &parts,
                                call_id_t &operation,
                                zlink::send_flags_t,
                                std::chrono::milliseconds timeout,
                                std::span<const std::uint8_t> metadata,
                                spot_request_completion_t completion)
{
    if (!_host) {
        co_return zlink::submit_result_t::invalid_handle;
    }
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument ("framework SPOT request timeout must be positive");
    }
    operation = _host->next_operation ();
    if (!_host->try_reserve_completion (operation))
        co_return zlink::submit_result_t::backpressured;
    try {
        const auto peer = _host->transport ().topology ().peer (target_node_rid.to_bytes ());
        const auto target_node_generation =
          peer ? peer->descriptor.lifecycle_generation : _host->status ().lifecycle_generation ();
        const auto route_fence =
          _host->resolve_spot_route_fence (target_node_rid, target_spot_id, target_spot_generation);
        if (!route_fence) {
            _host->release_completion (operation);
            co_return zlink::submit_result_t::not_found;
        }
        const auto target = protocol::spot_route_fence_t{
          target_spot_id,         target_spot_generation, target_node_rid.to_bytes (),
          target_node_generation, route_fence->first,     route_fence->second};
        const auto host = _host;
        const auto direct_completion = static_cast<bool> (completion);
        if (target.target_node_routing_id == host->status ().routing_id ().to_bytes ()) {
            const auto submitted = host->enqueue_local_spot_request (
              target, parts, operation, timeout, metadata, std::move (completion));
            if (submitted != zlink::submit_result_t::ok)
                host->release_completion (operation);
            co_return submitted;
        }
        const auto accepted = co_await _host->transport ().request_to_spot (
          target_node_rid.to_bytes (), spot_id (), target,
          _host->encode_application (parts, metadata), timeout,
          [host, operation, completion = std::move (completion), direct_completion] (
            foundation::operation_terminal_t terminal, std::vector<std::uint8_t> payload) mutable {
              if (!direct_completion) {
                  try {
                      host->complete_operation (operation, operation_kind_t::none, terminal,
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
                      /* flow-correlation §4: reply flow pair is observation-
                       * only — skip validation/materialization at Off. */
                      decoded =
                        result_t<std::vector<zlink::message_t>>::success (host->decode_application (
                          protocol::decode_application_payload (payload, host->capture_flow ())));
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
          },
          protocol::wire_operation_id_t{operation.high, operation.low}, std::nullopt);
        if (!accepted)
            _host->release_completion (operation);
        co_return submitted (accepted);
    }
    catch (...) {
        _host->release_completion (operation);
        throw;
    }
}

zlink::submit_result_t spot_handle_t::publish (const std::string &channel_name,
                                               const std::string &,
                                               const std::vector<zlink::message_t> &parts,
                                               zlink::send_flags_t,
                                               std::span<const std::uint8_t> metadata)
{
    if (!_host)
        return zlink::submit_result_t::invalid_handle;
    const auto targets = _host->transport ().topology ().peers ();
    const auto encoded = _host->encode_application (parts, metadata);
    ready_record_t owner;
    owner.owner_kind = owner_kind_t::node;
    owner.domain = ready_domain_t::application;
    receive_record_t local;
    local.kind = record_kind_t::node_send;
    local.domain = ready_domain_t::application;
    local.source_node_rid = _host->status ().routing_id ();
    _host->_local_dispatch_completion_lane
      .run ([&] {
        _host->_local_application_dispatches.push_back (
          public_host_runtime_t::local_application_dispatch_t{std::move (owner), std::move (local),
                                                              parts});
      })
      .get ();
    _host->_transport->signal_activity ();

    // The public publish boundary owns only local dequeue acceptance. Physical
    // fanout is scheduled by the logical multicast executor above this host.
    return zlink::submit_result_t::ok;
}

task_t<void> spot_handle_t::publish_tail (const std::vector<zlink::message_t> &parts,
                                          std::span<const std::uint8_t> metadata)
{
    if (!_host) {
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "logical multicast publisher is not connected");
    }
    const auto targets = _host->transport ().topology ().peers ();
    const auto encoded = _host->encode_application (parts, metadata);
    std::exception_ptr first_failure;
    for (const auto &target : targets) {
        try {
            const auto submitted = co_await _host->transport ().send_to_node_result (
              target.descriptor.node_routing_id, encoded);
            if (submitted != zlink::submit_result_t::ok && !first_failure) {
                first_failure = std::make_exception_ptr (framework_exception_t (
                  runtime::messaging::map_submit_result_error_kind (submitted),
                  "logical multicast physical fanout was not admitted"));
            }
        }
        catch (...) {
            if (!first_failure)
                first_failure = std::current_exception ();
        }
    }
    if (first_failure)
        std::rethrow_exception (first_failure);
    co_return;
}

void spot_handle_t::set_subscription (const std::string &, const std::string &)
{
}

void spot_handle_t::unset_subscription (const std::string &, const std::string &)
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

actor_handle_t::actor_handle_t (std::shared_ptr<public_host_runtime_t> host,
                                actor_ref_t actor,
                                stateful::object_ref_t object) :
    _host (std::move (host)), _actor (std::move (actor)), _object (std::move (object))
{
}

const actor_ref_t &actor_handle_t::ref () const noexcept
{
    return _actor;
}

zlink::submit_result_t actor_handle_t::join_entry_spot (const zlink::routing_id_t &target_node_rid,
                                                        const std::vector<zlink::message_t> &parts,
                                                        call_id_t &operation,
                                                        std::chrono::milliseconds timeout)
{
    if (!_host) {
        return zlink::submit_result_t::invalid_handle;
    }
    const auto entry = _host->entry_spot ();
    return join_spot (target_node_rid, entry.spot_id (), entry.status ().lifecycle_generation (),
                      parts, operation, timeout);
}

zlink::submit_result_t actor_handle_t::join_spot (const zlink::routing_id_t &target_node_rid,
                                                  const std::string &target_spot_id,
                                                  std::uint64_t target_spot_generation,
                                                  const std::vector<zlink::message_t> &parts,
                                                  call_id_t &operation,
                                                  std::chrono::milliseconds)
{
    if (!_host) {
        return zlink::submit_result_t::invalid_handle;
    }
    if (target_node_rid.to_bytes () != _host->status ().routing_id ().to_bytes ()) {
        return zlink::submit_result_t::not_connected;
    }
    return _host->begin_local_actor_join (_actor, target_spot_id, target_spot_generation, parts,
                                          operation);
}

task_t<zlink::submit_result_t> actor_handle_t::send_to (const actor_ref_t &target,
                                                        const std::vector<zlink::message_t> &parts,
                                                        zlink::send_flags_t,
                                                        std::span<const std::uint8_t> metadata)
{
    if (!_host)
        co_return zlink::submit_result_t::invalid_handle;
    co_return co_await _host->send_to_actor (target, parts, metadata);
}

task_t<zlink::submit_result_t>
actor_handle_t::request_to (const actor_ref_t &target,
                            const std::vector<zlink::message_t> &parts,
                            call_id_t &operation,
                            zlink::send_flags_t,
                            std::chrono::milliseconds timeout,
                            std::span<const std::uint8_t> metadata)
{
    if (!_host)
        co_return zlink::submit_result_t::invalid_handle;
    co_return co_await _host->request_to_actor (target, parts, operation, timeout, metadata);
}

public_host_runtime_t::public_host_runtime_t (host_options_t options) :
    _options (std::move (options)),
    _entry_spot_id (zlink::framework::detail::new_entry_spot_id (
      zlink::routing_id_t::from (_options.mesh.descriptor.node_routing_id).to_string ())),
    _transport (
      std::make_shared<mesh::raw_mesh_node_owner_t> (_options.mesh, _options.core_context)),
    _relocation_wire (
      std::make_unique<stateful::raw_relocation_replay_coordinator_t> (*_transport)),
    _objects (_options.mesh.application_message_budget,
              _options.mesh.infrastructure_message_budget,
              _options.mesh.application_byte_budget,
              _options.mesh.infrastructure_byte_budget),
    _sessions ([this] (const std::string &actor_id) {
        return _spot_actor_index_lane
          .run ([&] {
              const auto found = _actors.find (actor_id);
              return found == _actors.end () ? std::optional<stateful::object_ref_t>{}
                                             : std::make_optional (found->second.second);
          })
          .get ();
    })
{
    if (_options.session_relocation_seal_timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument (
          "Session relocation seal timeout must be a positive whole-millisecond duration");
    }
    /* Thread the host's flow-capture provider (flow-correlation §4) into the
     * relocation replay wire; the host outlives the coordinator it owns. */
    _relocation_wire->set_flow_capture_provider ([this] { return capture_flow (); });
    const auto &descriptor = _options.mesh.descriptor;
    _objects.replace_placement_candidates ({stateful::placement_candidate_t{
      descriptor.mesh_name,
      std::string (descriptor.node_routing_id.begin (), descriptor.node_routing_id.end ()),
      _options.object_stable_types, descriptor.placement_weight, descriptor.active_capacity_limit,
      descriptor.active_capacity_used, descriptor.pending_capacity_limit,
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
        throw std::invalid_argument ("stateful dispatch authority resolver must not be empty");
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started || _stateful_dispatch)
              throw std::logic_error ("stateful dispatch must be configured once before host start");
          _stateful_dispatch = std::make_unique<stateful::raw_stateful_dispatch_t> (
            _objects, *_transport, std::move (resolver));
          /* flow-correlation §4: the ingest path gates flow capture on the host's
           * provider; the host outlives the dispatch it owns. */
          _stateful_dispatch->set_flow_capture_provider ([this] { return capture_flow (); });
      })
      .get ();
}

void public_host_runtime_t::configure_message_follow_handler (
  std::function<void (const protocol::message_follow_notice_t &)> handler)
{
    _lifecycle_configuration_lane
      .run ([&] { _message_follow_handler = std::move (handler); })
      .get ();
}

void public_host_runtime_t::configure_actor_join_relocation (
  actor_join_relocation_prepare_validator_t prepare_validator,
  actor_join_recovery_consumer_t recovery_consumer,
  actor_join_authority_spot_resolver_t authority_spot_resolver,
  actor_join_recovery_rollback_t recovery_rollback,
  actor_join_committed_authority_adopter_t authority_adopter)
{
    if (!prepare_validator || !recovery_consumer || !authority_spot_resolver || !authority_adopter)
        throw std::invalid_argument ("Actor Join relocation callbacks must not be empty");
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started)
              throw std::logic_error ("Actor Join relocation must be configured before start");
          _actor_join_relocation_prepare_validator = std::move (prepare_validator);
          _actor_join_recovery_consumer = std::move (recovery_consumer);
          _actor_join_authority_spot_resolver = std::move (authority_spot_resolver);
          _actor_join_recovery_rollback = std::move (recovery_rollback);
          _actor_join_committed_authority_adopter = std::move (authority_adopter);
      })
      .get ();
}

void public_host_runtime_t::configure_bound_session_operations (
  bound_session_operations_t operations)
{
    if (!operations.bind || !operations.send || !operations.replaced
        || !operations.commit_relocation_route || !operations.prepare_relocation_target_route)
        throw std::invalid_argument ("bound Session operations must all be configured");
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started)
              throw std::logic_error ("bound Session operations must be configured before start");
          _bound_session_operations = std::move (operations);
      })
      .get ();
}

void public_host_runtime_t::configure_late_session_route_update (
  std::function<void (const protocol::session_relocation_route_t &)> reporter)
{
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started)
              throw std::logic_error ("late Session route reporter must be configured before start");
          _late_session_route_update_reporter = std::move (reporter);
      })
      .get ();
}

void public_host_runtime_t::start ()
{
    std::function<void ()> maintenance_started;
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started || _closing) {
              return;
          }
          _transport->start ();
          _started = true;
          maintenance_started = _maintenance_started;
      })
      .get ();
    if (maintenance_started)
        maintenance_started ();
}

void public_host_runtime_t::close () noexcept
{
    bool closing_started = false;
    std::function<void ()> maintenance_closing;
    {
        _lifecycle_configuration_lane
          .run ([&] {
              if (!_started || _closing) {
                  return;
              }
              _closing = true;
              closing_started = true;
              maintenance_closing = _maintenance_closing;
          })
          .get ();
        if (!closing_started)
            return;
    }
    if (maintenance_closing) {
        try {
            maintenance_closing ();
        }
        catch (...) {
        }
    }
    terminate_local_spot_requests (foundation::operation_terminal_t::shutdown);
    _lifecycle_configuration_lane.run ([&] { _started = false; }).get ();
    _relocation_session_terminal_lane
      .run ([&] {
          _session_seal_terminals.clear ();
          _session_journal_terminals.clear ();
      })
      .get ();
    _local_dispatch_completion_lane
      .run ([&] {
        _completions.clear ();
        _local_application_dispatches.clear ();
        _local_spot_requests.clear ();
        _local_spot_request_deadlines.clear ();
      })
      .get ();
    auto retained_outbound = _sessions.take_all_retained_outbound ();
    for (auto &settle : retained_outbound) {
        if (!settle)
            continue;
        try {
            settle (false);
        }
        catch (...) {
        }
    }
    _transport->close ();
    _lifecycle_configuration_lane.run ([&] { _closing = false; }).get ();
}

bool public_host_runtime_t::connect_peer (const std::string &endpoint,
                                          std::optional<zlink::routing_id_t> expected,
                                          std::uint64_t expected_lifecycle_generation,
                                          std::string security_identity)
{
    bool connected = false;
    if (expected) {
        auto descriptor = _options.mesh.descriptor;
        descriptor.node_routing_id = expected->to_bytes ();
        descriptor.advertised_endpoint = endpoint;
        descriptor.lifecycle_generation = expected_lifecycle_generation;
        descriptor.security_identity = std::move (security_identity);
        connected = _transport->connect_peer (endpoint, std::move (descriptor));
    } else {
        connected = _transport->connect_peer (endpoint);
    }
    if (connected) {
        _peer_endpoint_lane
          .run ([&] {
              _peer_endpoints.insert_or_assign (endpoint,
                                                expected ? expected->to_string () : std::string{});
          })
          .get ();
    }
    return connected;
}

void public_host_runtime_t::expect_peer (const std::string &endpoint,
                                         const zlink::routing_id_t &expected,
                                         std::uint64_t expected_lifecycle_generation,
                                         std::string security_identity)
{
    auto descriptor = _options.mesh.descriptor;
    descriptor.node_routing_id = expected.to_bytes ();
    descriptor.advertised_endpoint = endpoint;
    descriptor.lifecycle_generation = expected_lifecycle_generation;
    descriptor.security_identity = std::move (security_identity);
    _transport->expect_peer (std::move (descriptor));
}

void public_host_runtime_t::forget_peer (const std::string &endpoint,
                                         const zlink::routing_id_t &expected)
{
    _transport->forget_peer (expected.to_bytes (), endpoint);
}

void public_host_runtime_t::disconnect_peer (const std::string &endpoint) noexcept
{
    _peer_endpoint_lane.run ([&] { _peer_endpoints.erase (endpoint); }).get ();
    _transport->disconnect_peer (endpoint);
}

bool public_host_runtime_t::disconnect_peer (
  const std::vector<std::uint8_t> &expected_routing_id,
  const std::string &endpoint) noexcept
{
    const auto endpoint_retained =
      _transport->disconnect_peer (expected_routing_id, endpoint);
    if (!endpoint_retained) {
        _peer_endpoint_lane.run ([&] { _peer_endpoints.erase (endpoint); }).get ();
    }
    return endpoint_retained;
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
    /* _maintenance is set at most once, during configure_relocation /
     * configure_maintenance, both of which reject the call once the host
     * has started; it is never reassigned afterward. status() is called
     * from many contexts, some of which already hold _mutex (a plain,
     * non-recursive mutex), so this reads the pointer without locking
     * rather than risk a self-deadlock. */
    const auto *maintenance_ptr = _maintenance.get ();
    const bool safe_to_shutdown = !maintenance_ptr || maintenance_ptr->relocation_units_settled ();
    return {state, zlink::routing_id_t::from (descriptor.node_routing_id),
            descriptor.advertised_endpoint, descriptor.lifecycle_generation, safe_to_shutdown};
}

std::size_t public_host_runtime_t::pending_operation_count () const noexcept
{
    return _local_dispatch_completion_lane.run ([&] { return _completions.size (); }).get ();
}

void public_host_runtime_t::set_channel_weight (const std::string &channel_name,
                                                std::uint32_t weight)
{
    if (weight > 10000) {
        throw std::invalid_argument ("channel weight exceeds 10000");
    }
    auto descriptor = _transport->topology ().local_descriptor ();
    const auto found =
      std::find_if (descriptor.channels.begin (), descriptor.channels.end (),
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

task_t<bool>
public_host_runtime_t::send_message_follow (const std::vector<std::uint8_t> &target_routing_id,
                                            const protocol::message_follow_notice_t &notice)
{
    co_return co_await _transport->send_message_follow (target_routing_id, notice);
}

stateful::stateful_object_runtime_t &public_host_runtime_t::objects () noexcept
{
    return _objects;
}

stateful::stateful_error_t
public_host_runtime_t::destroy_application_actor (std::string_view actor_id,
                                                  std::uint64_t object_generation)
{
    stateful::object_ref_t object;
    const auto generation_stale = _spot_actor_index_lane
      .run ([&] {
        const auto found = _actors.find (std::string (actor_id));
        if (found != _actors.end ()) {
            if (found->second.second.object_generation != object_generation)
                return true;
            object = found->second.second;
        }
        return false;
      })
      .get ();
    if (generation_stale)
        return stateful::stateful_error_t::generation_stale;
    if (object.key.empty ()) {
        const auto local = _objects.find (stateful::object_kind_t::actor, std::string (actor_id));
        if (!local)
            return stateful::stateful_error_t::not_found;
        if (local->object_generation != object_generation)
            return stateful::stateful_error_t::generation_stale;
        object = *local;
    }
    const auto destroyed = _objects.destroy_actor (object);
    if (destroyed != stateful::stateful_error_t::none)
        return destroyed;
    _spot_actor_index_lane
      .run ([&] {
          const auto found = _actors.find (std::string (actor_id));
          if (found != _actors.end () && found->second.second.object_generation == object_generation)
              _actors.erase (found);
      })
      .get ();
    return stateful::stateful_error_t::none;
}

stateful::stream_session_registry_t &public_host_runtime_t::sessions () noexcept
{
    return _sessions;
}

stateful::raw_relocation_replay_coordinator_t &public_host_runtime_t::relocation_wire () noexcept
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
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started)
              throw std::logic_error ("User Spot operations must be configured before start");
          _user_spot_store = std::move (store);
          _user_spot_materializer = std::move (materializer);
          _route_cache_lane.run ([this] { _spot_route_fences.clear (); }).get ();
      })
      .get ();
}

void public_host_runtime_t::configure_spot_route_fence_resolver (
  spot_route_fence_resolver_t resolver)
{
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started)
              throw std::logic_error ("Spot route fence resolver must be configured before host start");
          _spot_route_fence_resolver = std::move (resolver);
          _route_cache_lane.run ([this] { _spot_route_fences.clear (); }).get ();
      })
      .get ();
}

void public_host_runtime_t::configure_peer_readiness_resolver (peer_readiness_resolver_t resolver)
{
    _lifecycle_configuration_lane
      .run ([&] { _peer_readiness_resolver = std::move (resolver); })
      .get ();
}

void public_host_runtime_t::configure_actor_create_operations (
  actor_create_operation_target_t target)
{
    if (!target)
        throw std::invalid_argument ("Actor create operation target is required");
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started || _actor_create_target)
              throw std::logic_error (
                "Actor create operations must be configured once before host start");
          _actor_create_target = std::move (target);
      })
      .get ();
}

void public_host_runtime_t::configure_actor_join_operations (actor_join_operation_target_t target)
{
    if (!target)
        throw std::invalid_argument ("Actor join operation target is required");
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started || _actor_join_target)
              throw std::logic_error ("Actor join operations must be configured once before host start");
          _actor_join_target = std::move (target);
      })
      .get ();
}

void public_host_runtime_t::configure_instance_spot_operations (
  std::shared_ptr<zlink::framework::location_repository_t> store,
  std::shared_ptr<stateful::relocation_store_port_t> relocations,
  location_owner_token_t owner,
  instance_spot_activation_materializer_t materializer)
{
    if (!store || !relocations || owner.owner_id.empty () || owner.lease_generation <= 0
        || !materializer)
        throw std::invalid_argument ("Instance Spot operations require Location and Relocation "
                                     "Stores, an owner lease, and a materializer");
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started)
              throw std::logic_error ("Instance Spot operations must be configured before start");
          _user_spot_store = std::move (store);
          _session_relocations = relocations;
          _instance_spot_relocations = std::move (relocations);
          _instance_spot_owner = std::move (owner);
          _instance_spot_materializer = std::move (materializer);
      })
      .get ();
}

std::optional<instance_spot_close_completion_t>
public_host_runtime_t::begin_instance_spot_close (const std::string &stable_type,
                                                  const std::string &spot_id,
                                                  std::uint64_t object_generation,
                                                  std::uint64_t authority_owner_generation)
{
    if (stable_type.empty () || spot_id.empty () || object_generation == 0
        || authority_owner_generation == 0)
        return std::nullopt;

    std::shared_ptr<zlink::framework::location_repository_t> store;
    location_owner_token_t instance_owner;
    _lifecycle_configuration_lane
      .run ([&] {
          store = _user_spot_store;
          instance_owner = _instance_spot_owner;
      })
      .get ();
    if (!store || instance_owner.owner_id.empty () || instance_owner.lease_generation <= 0)
        return std::nullopt;

    const auto authority_key = spot_authority_key (spot_id);
    const auto current = store->read_authority (authority_key).result ().value ();
    const auto *snapshot = std::get_if<authority_snapshot_t> (&current);
    if (!snapshot || snapshot->allocation.state != placement_allocation_state_t::active
        || snapshot->allocation.object_kind != placement_object_kind_t::instance_spot
        || snapshot->allocation.stable_type != stable_type
        || snapshot->object_generation != object_generation
        || snapshot->authority_owner_generation != authority_owner_generation
        || snapshot->allocation.target.owner.owner_id != instance_owner.owner_id
        || snapshot->allocation.target.owner.lease_generation != instance_owner.lease_generation)
        return std::nullopt;

    const auto local = status ();
    if (snapshot->allocation.target.mesh_name != _options.mesh.descriptor.mesh_name
        || snapshot->allocation.target.node_rid.value ()
             != node_rid_t::from_string (local.routing_id ().to_string ()).value ()
        || snapshot->allocation.target.node_lifecycle_generation != local.lifecycle_generation ())
        return std::nullopt;

    const auto ready = decode_instance_ready_state (snapshot->payload, capture_flow ());
    if (!ready || ready->stable_type != stable_type || ready->spot_id != spot_id
        || ready->object_generation != object_generation
        || ready->authority_owner_generation != authority_owner_generation || !ready->completed
        || !ready->recovery_reference.empty ())
        return std::nullopt;

    const auto sealed =
      store
        ->compare_exchange_authority (
          authority_key, snapshot->store_version,
          authority_put_t{encode_instance_closing_state (instance_closing_state_t{
            stable_type, spot_id, object_generation, authority_owner_generation})})
        .result ()
        .value ();
    const auto *closing = std::get_if<authority_stored_t> (&sealed);
    if (!closing)
        return std::nullopt;

    const auto ready_payload = snapshot->payload;
    const auto closing_version = closing->snapshot.store_version;
    auto completed = std::make_shared<std::atomic_bool> (false);
    return instance_spot_close_completion_t{
      [store = std::move (store), authority_key, ready_payload, closing_version,
       completed = std::move (completed)] (bool local_closed) {
          if (completed->exchange (true, std::memory_order_acq_rel)) {
              return false;
          }
          if (!local_closed) {
              (void) store
                ->compare_exchange_authority (authority_key, closing_version,
                                              authority_put_t{ready_payload})
                .result ();
              return false;
          }

          const auto deleted =
            store->compare_exchange_authority (authority_key, closing_version, authority_delete_t{})
              .result ()
              .value ();
          return std::holds_alternative<authority_deleted_t> (deleted);
      }};
}

bool public_host_runtime_t::evict_instance_spot (const std::string &stable_type,
                                                 const std::string &spot_id,
                                                 std::uint64_t object_generation,
                                                 std::uint64_t authority_owner_generation,
                                                 std::function<bool ()> close_local)
{
    if (!close_local)
        return false;

    auto completion = begin_instance_spot_close (stable_type, spot_id, object_generation,
                                                 authority_owner_generation);
    if (!completion)
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
    return (*completion) (local_closed);
}

void public_host_runtime_t::configure_session_route_owner (
  std::function<std::optional<location_owner_token_t> ()> owner_resolver)
{
    if (!owner_resolver)
        throw std::invalid_argument ("Session route owner resolver is required");
    _lifecycle_configuration_lane
      .run ([&] { _session_route_owner_resolver = std::move (owner_resolver); })
      .get ();
}

void public_host_runtime_t::configure_session_relocation_store (
  std::shared_ptr<stateful::relocation_store_port_t> relocations)
{
    if (!relocations)
        throw std::invalid_argument ("Session relocation Store is required");
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started)
              throw std::logic_error ("Session relocation Store must be configured before start");
          _session_relocations = std::move (relocations);
      })
      .get ();
}

std::pair<bool, std::optional<protocol::session_relocation_sealed_t>>
public_host_runtime_t::admit_session_relocation_seal (
  const protocol::session_relocation_seal_t &seal,
  const location_owner_token_t &session_owner,
  std::vector<std::uint8_t> response_routing_id,
  session_seal_local_completion_t local_completion)
{
    if (session_owner.owner_id != seal.session_owner_id
        || seal.session_owner_lease_generation
             > static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ())
        || session_owner.lease_generation
             != static_cast<std::int64_t> (seal.session_owner_lease_generation)) {
        return {false, std::nullopt};
    }

    const auto relocation_key = session_relocation_key (seal);
    bool existing_terminal = false;
    const auto existing_result = _relocation_session_terminal_lane
                                   .run ([&] () -> std::optional<
                                           std::pair<bool,
                                                     std::optional<protocol::session_relocation_sealed_t>>> {
                                       const auto cached = _session_seal_terminals.find (relocation_key);
                                       if (cached != _session_seal_terminals.end ()) {
                                           existing_terminal = true;
                                           if (cached->second.seal != seal)
                                               return {{false, std::nullopt}};
                                           if (cached->second.consumed && !cached->second.ready)
                                               return {{false, std::nullopt}};
                                           if (!response_routing_id.empty ())
                                               cached->second.response_routing_id =
                                                 std::move (response_routing_id);
                                           if (cached->second.ready)
                                               return {{true, cached->second.sealed}};
                                           if (local_completion)
                                               cached->second.local_completions.push_back (
                                                 std::move (local_completion));
                                           return {{true, std::nullopt}};
                                       }
                                       if (_session_seal_terminals.size () >= 65'536) {
                                           const auto consumed = std::find_if (
                                             _session_seal_terminals.begin (),
                                             _session_seal_terminals.end (), [] (const auto &entry) {
                                                 return entry.second.consumed;
                                             });
                                           if (consumed == _session_seal_terminals.end ())
                                               return {{false, std::nullopt}};
                                           _session_seal_terminals.erase (consumed);
                                       }
                                       return std::nullopt;
                                   })
                                   .get ();
    if (existing_terminal)
        return *existing_result;

    const auto session_id = zlink::routing_id_t::from (seal.session_routing_id).to_hex ();
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor,
      seal.actor.actor_id,
      seal.actor.object_generation,
      seal.actor.authority_owner_generation,
      {},
      zlink::routing_id_t::from (seal.actor.target_node_routing_id).to_string ()};
    const auto admission = _sessions.seal_remote_route (session_id, seal.binding_generation, actor,
                                                        seal.actor.target_node_generation,
                                                        seal.actor.owner_lease_generation);
    if ((admission.error != stateful::stateful_error_t::none
         && admission.error != stateful::stateful_error_t::backpressured)
        || !admission.binding || admission.barrier.token == 0) {
        return {false, std::nullopt};
    }

    const protocol::session_relocation_sealed_t ack{seal.relocation,
                                                    seal.coordinator,
                                                    seal.actor,
                                                    seal.session_owner_node_routing_id,
                                                    seal.session_owner_node_generation,
                                                    seal.session_owner_id,
                                                    seal.session_owner_lease_generation,
                                                    seal.session_routing_id,
                                                    seal.binding_generation};
    const auto ready = _sessions.remote_route_seal_ready (admission.barrier);
    bool inserted = false;
    std::optional<protocol::session_relocation_sealed_t> immediate;
    _relocation_session_terminal_lane
      .run ([&] {
          session_seal_terminal_record_t record{seal,
                                              ack,
                                              admission.last_accepted_sequence,
                                              admission.barrier,
                                              std::chrono::steady_clock::now ()
                                                + _options.session_relocation_seal_timeout,
                                              false,
                                              ready,
                                              response_routing_id,
                                              {}};
          if (local_completion && !ready)
              record.local_completions.push_back (local_completion);
          const auto [stored, was_inserted] =
            _session_seal_terminals.emplace (relocation_key, std::move (record));
          inserted = was_inserted;
          if (!inserted) {
              if (stored->second.seal != seal) {
                  immediate.reset ();
              } else if (stored->second.ready) {
                  if (!response_routing_id.empty ())
                      stored->second.response_routing_id = response_routing_id;
                  immediate = stored->second.sealed;
              } else {
                  if (!response_routing_id.empty ())
                      stored->second.response_routing_id = response_routing_id;
                  if (local_completion)
                      stored->second.local_completions.push_back (local_completion);
              }
          } else if (ready) {
              immediate = ack;
          }
      })
      .get ();
    if (!inserted) {
        (void) _sessions.abort_barrier (admission.barrier);
        const auto stored_matches = _relocation_session_terminal_lane
                                      .run ([&] {
                                          const auto stored =
                                            _session_seal_terminals.find (relocation_key);
                                          return stored != _session_seal_terminals.end ()
                                                 && stored->second.seal == seal;
                                      })
                                      .get ();
        if (!stored_matches)
            return {false, std::nullopt};
    }
    return {true, std::move (immediate)};
}

task_t<bool>
public_host_runtime_t::seal_session_remote (const zlink::routing_id_t &session_owner_node,
                                            protocol::session_relocation_seal_t seal,
                                            std::chrono::milliseconds timeout,
                                            session_relocation_journal_capture_t capture_journal,
                                            session_relocation_seal_completion_t completion)
{
    if (!capture_journal || !completion)
        throw std::invalid_argument (
          "Session relocation seal requires journal capture and completion callbacks");
    const auto relocation_key = session_relocation_key (seal);
    std::optional<session_relocation_seal_result_t> cached_result;
    std::shared_ptr<stateful::relocation_store_port_t> relocations;
    const auto journal_admitted = _relocation_session_terminal_lane
                                    .run ([&] {
                                        const auto cached =
                                          _session_journal_terminals.find (relocation_key);
                                        if (cached != _session_journal_terminals.end ()) {
                                            if (cached->second.first != seal)
                                                return false;
                                            cached_result = cached->second.second;
                                        } else if (_session_journal_terminals.size () >= 65'536) {
                                            return false;
                                        }
                                        relocations = _session_relocations;
                                        return true;
                                    })
                                    .get ();
    if (!journal_admitted)
        co_return false;
    if (cached_result) {
        completion (foundation::operation_terminal_t::completed, std::move (cached_result));
        co_return true;
    }
    if (!relocations)
        co_return false;
    const auto expected = seal;
    const auto weak_host = weak_from_this ();
    auto response = std::make_shared<
      std::function<void (foundation::operation_terminal_t, std::vector<std::uint8_t>)>> (
      [weak_host, expected, relocation_key, relocations = std::move (relocations),
       capture_journal = std::move (capture_journal), completion = std::move (completion)] (
        foundation::operation_terminal_t terminal, std::vector<std::uint8_t> payload) mutable {
          if (terminal != foundation::operation_terminal_t::completed) {
              completion (terminal, std::nullopt);
              return;
          }
          try {
              const auto host = weak_host.lock ();
              if (!host) {
                  completion (foundation::operation_terminal_t::transport_failed, std::nullopt);
                  return;
              }
              const auto sealed = protocol::decode_session_relocation_sealed (payload);
              if (sealed.relocation != expected.relocation
                  || sealed.coordinator != expected.coordinator || sealed.actor != expected.actor
                  || sealed.session_owner_node_routing_id != expected.session_owner_node_routing_id
                  || sealed.session_owner_node_generation != expected.session_owner_node_generation
                  || sealed.session_owner_id != expected.session_owner_id
                  || sealed.session_owner_lease_generation
                       != expected.session_owner_lease_generation
                  || sealed.session_routing_id != expected.session_routing_id
                  || sealed.binding_generation != expected.binding_generation) {
                  completion (foundation::operation_terminal_t::transport_failed, std::nullopt);
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
                  zlink::routing_id_t::from (expected.actor.target_node_routing_id).to_string ()},
                expected.binding_generation,
                0,
                capture_journal ()};
              stateful::durable_session_journal_store_t journal_store (relocations);
              const auto root = journal_store.prepare (record);
              const auto recovered = journal_store.recover (root);
              if (!recovered || *recovered != record) {
                  journal_store.cleanup (root);
                  completion (foundation::operation_terminal_t::transport_failed, std::nullopt);
                  return;
              }
              const session_relocation_seal_result_t result{sealed, root};
              std::optional<session_relocation_seal_result_t> existing_result;
              bool conflicting_terminal = false;
              bool terminal_capacity_exhausted = false;
              host->_relocation_session_terminal_lane
                .run ([&] {
                  terminal_capacity_exhausted =
                    host->_session_journal_terminals.size () >= 65'536
                    && !host->_session_journal_terminals.contains (relocation_key);
                  if (!terminal_capacity_exhausted) {
                      const auto [stored, inserted] = host->_session_journal_terminals.emplace (
                        relocation_key, std::pair{expected, result});
                      if (!inserted) {
                          journal_store.cleanup (root);
                          conflicting_terminal = stored->second.first != expected;
                          if (!conflicting_terminal)
                              existing_result = stored->second.second;
                      }
                  }
                })
                .get ();
              if (terminal_capacity_exhausted) {
                  journal_store.cleanup (root);
                  completion (foundation::operation_terminal_t::transport_failed, std::nullopt);
                  return;
              }
              if (conflicting_terminal) {
                  completion (foundation::operation_terminal_t::transport_failed, std::nullopt);
                  return;
              }
              if (existing_result) {
                  completion (terminal, std::move (existing_result));
                  return;
              }
              completion (terminal, result);
          }
          catch (...) {
              completion (foundation::operation_terminal_t::transport_failed, std::nullopt);
          }
      });

    const auto local = status ();
    if (local.routing_id ().to_bytes () == session_owner_node.to_bytes ()) {
        std::function<std::optional<location_owner_token_t> ()> owner_resolver;
        _lifecycle_configuration_lane
          .run ([&] { owner_resolver = _session_route_owner_resolver; })
          .get ();
        if (local.lifecycle_generation () != seal.session_owner_node_generation || !owner_resolver)
            co_return false;
        const auto owner = owner_resolver ();
        if (!owner)
            co_return false;
        auto local_completion =
          [response] (foundation::operation_terminal_t terminal,
                      std::optional<protocol::session_relocation_sealed_t> sealed) mutable {
              (*response) (terminal, sealed ? protocol::encode_session_relocation_sealed (*sealed)
                                            : std::vector<std::uint8_t>{});
          };
        auto [accepted, immediate] =
          admit_session_relocation_seal (seal, *owner, {}, local_completion);
        if (!accepted)
            co_return false;
        if (immediate)
            local_completion (foundation::operation_terminal_t::completed, std::move (*immediate));
        co_return true;
    }
    co_return co_await _transport->request_session_relocation_seal (
      session_owner_node.to_bytes (), std::move (seal), timeout,
      [response] (foundation::operation_terminal_t terminal,
                  std::vector<std::uint8_t> payload) mutable {
          (*response) (terminal, std::move (payload));
      });
}

task_t<bool> public_host_runtime_t::activate_instance_spot_remote (
  const zlink::routing_id_t &target_node,
  protocol::instance_spot_activation_header_t request,
  std::optional<std::vector<std::uint8_t>> metadata,
  protocol::application_payload_t application_payload,
  std::chrono::milliseconds timeout,
  instance_spot_activation_completion_t completion)
{
    if (!completion)
        throw std::invalid_argument ("Instance Spot activation completion is required");
    co_return co_await _transport->request_instance_spot_activation (
      target_node.to_bytes (), std::move (request), std::move (metadata),
      std::move (application_payload), timeout,
      [completion = std::move (completion), capture = capture_flow ()] (
        foundation::operation_terminal_t terminal, std::vector<std::uint8_t> packed) mutable {
          protocol::reply_header_t reply{};
          std::optional<protocol::application_payload_t> application_reply;
          if (terminal == foundation::operation_terminal_t::completed) {
              try {
                  const auto parts = unpack_infrastructure_reply (packed);
                  reply = protocol::decode_reply_header (parts.front ());
                  if (parts.size () == 2)
                      application_reply = protocol::decode_application_payload (parts[1], capture);
              }
              catch (const protocol::service_wire_error_t &) {
                  //  Spec 32-framework-error-model:91-92 — a reply that can't
                  //  be processed is ProtocolError, not a transport failure.
                  //  Synthesize the protocolError wire terminal; the sink's
                  //  reply_header_exception mapper classifies it.
                  reply = {};
                  reply.terminal_result = 104;
                  //  requestProtocolError(16): the schema integrity rule
                  //  forbids a typed terminal with failure none.
                  reply.failure_code = 16;
                  application_reply.reset ();
              }
          }
          completion (terminal, reply, std::move (application_reply));
      });
}

task_t<bool> public_host_runtime_t::send_instance_spot_activation_remote (
  const zlink::routing_id_t &target_node,
  protocol::instance_spot_activation_header_t request,
  std::optional<std::vector<std::uint8_t>> metadata,
  protocol::application_payload_t application_payload)
{
    co_return co_await _transport->send_instance_spot_activation (
      target_node.to_bytes (), std::move (request), std::move (metadata),
      std::move (application_payload));
}

// Test-visible forwarder for the anonymous-namespace classifier above (kept
// out of the class API surface — this is not part of the framework's public
// contract, only a seam so the cross-language failure-code mapping can be
// pinned directly without standing up a full relocation round trip).
framework_error_kind_t classify_relocation_failure_code (std::uint32_t wire_code) noexcept
{
    return map_relocation_failure_code (wire_code);
}

stateful::relocation_reason_t classify_relocation_failure_reason (std::uint32_t wire_code) noexcept
{
    switch (map_relocation_failure_code (wire_code)) {
        case framework_error_kind_t::data_lost:
            return stateful::relocation_reason_t::checksum_mismatch;
        // capacity_exceeded here is the REMOTE node's full queue (spec 32
        // classifies another node's capacity as Unavailable) — it must not
        // surface as a source-owned permit failure, so it falls through to
        // restore_failed with the other remote-side terminal codes.
        case framework_error_kind_t::deadline_exceeded:
            return stateful::relocation_reason_t::turn_active;
        default:
            return stateful::relocation_reason_t::restore_failed;
    }
}

task_t<stateful::relocation_reason_t> public_host_runtime_t::prepare_relocation_remote (
  const zlink::routing_id_t &target_node,
  protocol::relocation_prepare_t prepare,
  std::chrono::milliseconds timeout,
  std::vector<protocol::session_relocation_route_t> session_routes)
{
    if (timeout <= std::chrono::milliseconds::zero ())
        co_return stateful::relocation_reason_t::restore_failed;
    const auto response = co_await _transport->request_relocation_prepare (
      target_node.to_bytes (), prepare, timeout, std::move (session_routes));
    if (response.failed) {
        // Exact-identity fencing already ran in request_relocation_prepare
        // — this is not a timeout, it is the target's own explicit,
        // matching-identity rejection. Map and surface its failure_code
        // instead of letting it collapse into the same "no result" a
        // timeout produces.
        const auto kind = map_relocation_failure_code (response.failed->failure_code);
        trace_mesh_host ("relocation-prepare-failed",
                         "wire_failure_code=" + std::to_string (response.failed->failure_code)
                           + " kind=" + std::to_string (static_cast<int> (kind)));
        co_return classify_relocation_failure_reason (response.failed->failure_code);
    }
    if (!response.ready)
        co_return stateful::relocation_reason_t::restore_failed;
    const auto &ready = *response.ready;
    if (ready.relocation != prepare.relocation
        || ready.target_attempt_generation != prepare.target_attempt_generation
        || ready.coordinator != prepare.coordinator || ready.target != prepare.target
        || ready.object != prepare.object
        || ready.sender_role != protocol::relocation_role_t::target)
        co_return stateful::relocation_reason_t::restore_failed;
    co_return stateful::relocation_reason_t::none;
}

task_t<bool>
public_host_runtime_t::cutover_relocation_remote (const zlink::routing_id_t &target_node,
                                                  protocol::relocation_cutover_t cutover)
{
    co_return co_await _transport->send_relocation_control (target_node.to_bytes (), cutover);
}

stateful::stateful_error_t
public_host_runtime_t::ingest_stateful (const stateful::object_ref_t &owner)
{
    return _stateful_dispatch ? _stateful_dispatch->ingest (owner)
                              : stateful::stateful_error_t::invalid;
}

task_t<bool>
public_host_runtime_t::route_session_remote (const zlink::routing_id_t &session_owner_node,
                                             protocol::session_relocation_route_t route)
{
    co_return co_await _transport->send_session_relocation_route (session_owner_node.to_bytes (),
                                                                  route);
}

std::size_t public_host_runtime_t::recover_instance_spot_activations ()
{
    std::shared_ptr<zlink::framework::location_repository_t> store;
    std::shared_ptr<stateful::relocation_store_port_t> relocations;
    instance_spot_activation_materializer_t materializer;
    _lifecycle_configuration_lane
      .run ([&] {
          store = _user_spot_store;
          relocations = _instance_spot_relocations;
          materializer = _instance_spot_materializer;
      })
      .get ();
    if (!store || !relocations || !materializer)
        return 0;
    const auto local = _transport->topology ().local_descriptor ();
    std::size_t recovered = 0;
    std::optional<authority_scan_cursor_t> cursor;
    do {
        const auto scanned = store->list_authorities ("zla1:s:", cursor, 256).result ().value ();
        const auto *page = std::get_if<authority_page_t> (&scanned);
        if (!page)
            break;
        for (const auto &entry : page->items) {
            if (const auto closing = decode_instance_closing_state (entry.snapshot.payload);
                closing
                && entry.snapshot.allocation.object_kind == placement_object_kind_t::instance_spot
                && entry.snapshot.allocation.state == placement_allocation_state_t::active
                && entry.snapshot.allocation.stable_type == closing->stable_type
                && entry.snapshot.allocation.target.node_rid.value ()
                     == node_rid_t::from_string (
                          zlink::routing_id_t::from (local.node_routing_id).to_string ())
                          .value ()
                && entry.snapshot.allocation.target.node_lifecycle_generation
                     == local.lifecycle_generation
                && entry.snapshot.object_generation == closing->object_generation
                && entry.snapshot.authority_owner_generation
                     == closing->authority_owner_generation) {
                const auto deleted =
                  store
                    ->compare_exchange_authority (entry.key, entry.snapshot.store_version,
                                                  authority_delete_t{})
                    .result ()
                    .value ();
                if (std::holds_alternative<authority_deleted_t> (deleted))
                    ++recovered;
                continue;
            }
            const auto state =
              decode_instance_ready_state (entry.snapshot.payload, capture_flow ());
            if (!state || state->recovery_reference.empty ()
                || entry.snapshot.allocation.object_kind != placement_object_kind_t::instance_spot
                || entry.snapshot.allocation.target.node_rid.value ()
                     != node_rid_t::from_string (
                          zlink::routing_id_t::from (local.node_routing_id).to_string ())
                          .value ()
                || entry.snapshot.allocation.target.node_lifecycle_generation
                     != local.lifecycle_generation)
                continue;
            const auto payload = relocations->get (state->recovery_reference);
            if (!payload
                || stateful::maintenance_runtime_t::crc32c (*payload) != state->recovery_checksum)
                continue;
            protocol::instance_activation_recovery_t recovery;
            try {
                recovery =
                  protocol::decode_instance_activation_recovery (*payload, capture_flow ());
            }
            catch (const protocol::service_wire_error_t &) {
                continue;
            }
            auto updated = *state;
            if (!updated.completed) {
                bool prepared = false;
                try {
                    prepared = materializer.prepare (recovery.activation);
                }
                catch (...) {
                    prepared = false;
                }
                if (!prepared)
                    continue;
                auto result = materializer.dispatch (recovery.activation, recovery.metadata,
                                                     recovery.application_payload);
                updated.completed = true;
                updated.terminal_result = result.terminal_result;
                updated.failure_code = result.failure_code;
                updated.reply = std::move (result.application_reply);
                const auto terminal = store
                                        ->compare_exchange_authority (
                                          entry.key, entry.snapshot.store_version,
                                          authority_restore_t{encode_instance_ready_state (updated),
                                                              entry.snapshot.owner})
                                        .result ()
                                        .value ();
                const auto *stored = std::get_if<authority_stored_t> (&terminal);
                if (!stored)
                    continue;
                updated.recovery_reference.clear ();
                updated.recovery_checksum = 0;
                const auto cleared = store
                                       ->compare_exchange_authority (
                                         entry.key, stored->snapshot.store_version,
                                         authority_restore_t{encode_instance_ready_state (updated),
                                                             stored->snapshot.owner})
                                       .result ()
                                       .value ();
                if (!std::holds_alternative<authority_stored_t> (cleared))
                    continue;
            } else {
                updated.recovery_reference.clear ();
                updated.recovery_checksum = 0;
                const auto cleared = store
                                       ->compare_exchange_authority (
                                         entry.key, entry.snapshot.store_version,
                                         authority_restore_t{encode_instance_ready_state (updated),
                                                             entry.snapshot.owner})
                                       .result ()
                                       .value ();
                if (!std::holds_alternative<authority_stored_t> (cleared))
                    continue;
            }
            relocations->remove (state->recovery_reference);
            ++recovered;
        }
        cursor = page->next_cursor;
    } while (cursor);
    return recovered;
}

task_t<bool>
public_host_runtime_t::create_user_spot_remote (const zlink::routing_id_t &target_node,
                                                protocol::user_spot_create_header_t request,
                                                std::chrono::milliseconds timeout,
                                                user_spot_create_completion_t completion)
{
    if (!completion)
        throw std::invalid_argument ("User Spot create completion is required");
    co_return co_await _transport->request_user_spot_create (
      target_node.to_bytes (), std::move (request), timeout,
      [completion = std::move (completion), capture = capture_flow ()] (
        foundation::operation_terminal_t terminal, std::vector<std::uint8_t> packed) mutable {
          protocol::user_spot_create_reply_t reply;
          std::optional<protocol::application_payload_t> application_reply;
          if (terminal == foundation::operation_terminal_t::completed) {
              try {
                  const auto parts = unpack_infrastructure_reply (packed);
                  reply = protocol::decode_user_spot_create_reply (parts.front ());
                  if (parts.size () == 2)
                      application_reply = protocol::decode_application_payload (parts[1], capture);
              }
              catch (const protocol::service_wire_error_t &) {
                  //  Spec 32-framework-error-model:91-92 — a reply that can't
                  //  be processed is ProtocolError, not a transport failure.
                  //  Synthesize the protocolError wire terminal so the
                  //  consumer-side mapper classifies it correctly; the
                  //  completed terminal is kept so the reply header is read.
                  reply = {};
                  reply.header.terminal_result = 104;
                  //  requestProtocolError(16): the schema integrity rule
                  //  forbids a typed terminal with failure none.
                  reply.header.failure_code = 16;
                  application_reply.reset ();
              }
          }
          completion (terminal, std::move (reply), std::move (application_reply));
      });
}

task_t<bool>
public_host_runtime_t::create_actor_remote (const zlink::routing_id_t &target_node,
                                            protocol::actor_create_header_t request,
                                            std::chrono::milliseconds timeout,
                                            actor_create_operation_completion_t completion)
{
    if (!completion)
        throw std::invalid_argument ("Actor create completion is required");
    if (target_node == status ().routing_id ()) {
        actor_create_operation_target_t target;
        _lifecycle_configuration_lane.run ([&] { target = _actor_create_target; }).get ();
        if (!target)
            co_return false;
        auto completed = std::make_shared<std::atomic_bool> (false);
        auto forward = [completion = std::move (completion),
                        completed] (actor_create_operation_result_t result) mutable {
            if (completed->exchange (true, std::memory_order_acq_rel))
                return;
            completion (foundation::operation_terminal_t::completed, std::move (result.reply),
                        std::move (result.application_reply));
        };
        try {
            target (request, forward);
        }
        catch (const std::exception &) {
            actor_create_operation_result_t result;
            result.reply.header = {
              request.correlation, 105u,
              static_cast<std::uint32_t> (protocol::framework_error_code::actorCreateFailed)};
            forward (std::move (result));
        }
        catch (...) {
            actor_create_operation_result_t result;
            result.reply.header = {
              request.correlation, 105u,
              static_cast<std::uint32_t> (protocol::framework_error_code::actorCreateFailed)};
            forward (std::move (result));
        }
        co_return true;
    }
    co_return co_await _transport->request_actor_create (
      target_node.to_bytes (), std::move (request), timeout,
      [completion = std::move (completion), capture = capture_flow ()] (
        foundation::operation_terminal_t terminal, std::vector<std::uint8_t> packed) mutable {
          protocol::actor_create_reply_t reply;
          std::optional<protocol::application_payload_t> application_reply;
          if (terminal == foundation::operation_terminal_t::completed) {
              try {
                  const auto parts = unpack_infrastructure_reply (packed);
                  reply = protocol::decode_actor_create_reply (parts.front ());
                  if (parts.size () == 2)
                      application_reply = protocol::decode_application_payload (parts[1], capture);
              }
              catch (const protocol::service_wire_error_t &) {
                  //  Spec 32-framework-error-model:91-92 — a reply that can't
                  //  be processed is ProtocolError, not a transport failure.
                  //  Synthesize the protocolError wire terminal so the
                  //  consumer-side mapper classifies it correctly.
                  reply = {};
                  reply.header.terminal_result = 104;
                  //  requestProtocolError(16): the schema integrity rule
                  //  forbids a typed terminal with failure none.
                  reply.header.failure_code = 16;
                  application_reply.reset ();
              }
          }
          completion (terminal, std::move (reply), std::move (application_reply));
      });
}

task_t<bool>
public_host_runtime_t::close_user_spot_remote (const zlink::routing_id_t &target_node,
                                               protocol::user_spot_close_header_t request,
                                               std::chrono::milliseconds timeout,
                                               user_spot_close_completion_t completion)
{
    if (!completion)
        throw std::invalid_argument ("User Spot close completion is required");
    co_return co_await _transport->request_user_spot_close (
      target_node.to_bytes (), std::move (request), timeout,
      [completion = std::move (completion)] (foundation::operation_terminal_t terminal,
                                             std::vector<std::uint8_t> packed) mutable {
          protocol::user_spot_close_reply_t reply;
          if (terminal == foundation::operation_terminal_t::completed) {
              try {
                  const auto parts = unpack_infrastructure_reply (packed);
                  if (parts.size () != 1)
                      throw protocol::service_wire_error_t (
                        "User Spot close reply carries a payload");
                  reply = protocol::decode_user_spot_close_reply (parts.front ());
              }
              catch (const protocol::service_wire_error_t &) {
                  //  Spec 32-framework-error-model:91-92 — a reply that can't
                  //  be processed is ProtocolError, not a transport failure.
                  //  Synthesize the protocolError wire terminal so the
                  //  consumer-side mapper classifies it correctly.
                  reply = {};
                  reply.header.terminal_result = 104;
                  //  requestProtocolError(16): the schema integrity rule
                  //  forbids a typed terminal with failure none.
                  reply.header.failure_code = 16;
              }
          }
          completion (terminal, std::move (reply));
      });
}

spot_handle_t public_host_runtime_t::entry_spot ()
{
    return get_or_create_spot (_entry_spot_id);
}

spot_handle_t public_host_runtime_t::get_or_create_spot (std::string spot_id)
{
    const auto &key = spot_id;
    const auto existing = _spot_actor_index_lane
      .run ([&] () -> std::optional<stateful::object_ref_t> {
        const auto found = _spots.find (key);
        if (found != _spots.end ()) {
            return found->second;
        }
        return std::nullopt;
      })
      .get ();
    if (existing)
        return spot_handle_t (shared_from_this (), *existing);
    auto created =
      _objects.begin_create (stateful::create_request_t{stateful::object_kind_t::user_spot,
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
        && _objects.commit_create (created.attempt) != stateful::stateful_error_t::none) {
        throw std::runtime_error ("framework Spot Ready commit failed");
    }
    auto object = _objects.find (stateful::object_kind_t::user_spot, key);
    if (!object) {
        throw std::runtime_error ("framework Spot authority is unavailable");
    }
    _spot_actor_index_lane.run ([&] { _spots.insert_or_assign (key, *object); }).get ();
    return spot_handle_t (shared_from_this (), *object);
}

spot_handle_t public_host_runtime_t::bind_relocation_spot (stateful::object_ref_t object)
{
    const auto bound = _spot_actor_index_lane
      .run ([&] {
          const auto [found, _] = _spots.insert_or_assign (object.key, std::move (object));
          return found->second;
      })
      .get ();
    return spot_handle_t (shared_from_this (), bound);
}

stateful::stateful_error_t
public_host_runtime_t::advance_local_actor_authority (const stateful::object_ref_t &committed)
{
    return _spot_actor_index_lane
      .run ([&] {
          const auto found = _actors.find (committed.key);
          if (found == _actors.end ()) {
              return stateful::stateful_error_t::none;
          }
          const auto advanced =
            _objects.advance_local_actor_authority (committed, found->second.first);
          if (advanced == stateful::stateful_error_t::none)
              found->second.second = committed;
          return advanced;
      })
      .get ();
}

actor_handle_t public_host_runtime_t::create_actor (std::string actor_type, std::string actor_id)
{
    const auto existing = _spot_actor_index_lane
      .run ([&] () -> std::optional<std::pair<actor_ref_t, stateful::object_ref_t>> {
        const auto found = _actors.find (actor_id);
        if (found != _actors.end ()) {
            return std::make_pair (
              framework_actor_ref (found->second.second, found->second.first), found->second.second);
        }
        return std::nullopt;
      })
      .get ();
    if (existing)
        return actor_handle_t (shared_from_this (), existing->first, existing->second);
    auto created =
      _objects.begin_create (stateful::create_request_t{stateful::object_kind_t::actor,
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
        && _objects.commit_create (created.attempt) != stateful::stateful_error_t::none) {
        throw std::runtime_error ("framework Actor Ready commit failed");
    }
    auto object = _objects.find (stateful::object_kind_t::actor, actor_id);
    if (!object) {
        throw std::runtime_error ("framework Actor authority is unavailable");
    }
    _spot_actor_index_lane
      .run ([&] { _actors.insert_or_assign (actor_id, std::make_pair (actor_type, *object)); })
      .get ();
    return actor_handle_t (shared_from_this (), framework_actor_ref (*object, actor_type), *object);
}

actor_handle_t public_host_runtime_t::create_reserved_actor (std::string actor_type,
                                                             stateful::object_ref_t reserved)
{
    if (reserved.kind != stateful::object_kind_t::actor)
        throw std::invalid_argument ("reserved Actor reference has an invalid object kind");
    const auto existing = _spot_actor_index_lane
      .run ([&] () -> std::optional<actor_handle_t> {
        const auto found = _actors.find (reserved.key);
        if (found != _actors.end ()) {
            if (found->second.second.object_generation != reserved.object_generation
                || found->second.second.authority_owner_generation
                     != reserved.authority_owner_generation) {
                const auto adopted = _objects.adopt_reserved_actor_owner (reserved, actor_type);
                if (adopted == stateful::stateful_error_t::none) {
                    found->second.second = reserved;
                    return actor_handle_t (
                      shared_from_this (), framework_actor_ref (reserved, found->second.first), reserved);
                }
                throw std::runtime_error (
                  "reserved Actor generation does not match the local Actor");
            }
            return actor_handle_t (shared_from_this (),
                                   framework_actor_ref (found->second.second, found->second.first),
                                   found->second.second);
        }
        return std::nullopt;
      })
      .get ();
    if (existing)
        return std::move (*existing);
    auto created = _objects.begin_reserved_object (reserved, actor_type, {});
    if (created.error != stateful::stateful_error_t::none)
        throw std::runtime_error ("reserved framework Actor authority creation failed");
    if (created.factory_owner
        && _objects.commit_create (created.attempt) != stateful::stateful_error_t::none)
        throw std::runtime_error ("reserved framework Actor Ready commit failed");
    auto object = _objects.find (stateful::object_kind_t::actor, reserved.key);
    if (!object)
        throw std::runtime_error ("reserved framework Actor authority is unavailable");
    _spot_actor_index_lane
      .run ([&] { _actors.insert_or_assign (reserved.key, std::make_pair (actor_type, *object)); })
      .get ();
    return actor_handle_t (shared_from_this (), framework_actor_ref (*object, actor_type), *object);
}

std::optional<route_fence_t>
public_host_runtime_t::resolve_spot_route_fence (const zlink::routing_id_t &target_node_rid,
                                                 std::string_view target_spot_id,
                                                 std::uint64_t target_spot_generation)
{
    spot_route_fence_resolver_t resolver;
    std::shared_ptr<zlink::framework::location_repository_t> store;
    _lifecycle_configuration_lane
      .run ([&] {
          resolver = _spot_route_fence_resolver;
          store = _user_spot_store;
      })
      .get ();
    if (resolver) {
        try {
            return resolver (target_node_rid, target_spot_id, target_spot_generation);
        }
        catch (...) {
            return std::nullopt;
        }
    }

    const auto key = spot_route_cache_key (target_node_rid, target_spot_id, target_spot_generation);
    const auto cached =
      _route_cache_lane
        .run ([this, &key] () -> std::optional<route_fence_t> {
            const auto found = _spot_route_fences.find (key);
            if (found == _spot_route_fences.end ())
                return std::nullopt;
            if (std::chrono::steady_clock::now () < found->second.expires_at)
                return found->second.fence;
            _spot_route_fences.erase (found);
            return std::nullopt;
        })
        .get ();
    if (cached) {
        return cached;
    }

    const auto measured_at = std::chrono::steady_clock::now ();
    const auto read = read_route_owner_fence (store, '2', target_spot_id, target_spot_generation, 0,
                                              0, _options.owner_lease_fencing_margin);
    if (read && read->admission_lifetime) {
        const auto admission_expires_at = measured_at + *read->admission_lifetime;
        if (std::chrono::steady_clock::now () >= admission_expires_at)
            return std::nullopt;
        if (_options.route_cache_max_age <= std::chrono::milliseconds::zero ())
            return read->fence;
        const auto lifetime =
          std::min (std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                      _options.route_cache_max_age),
                    *read->admission_lifetime);
        _route_cache_lane
          .run ([this, &key, fence = read->fence, expires_at = measured_at + lifetime] {
              _spot_route_fences.insert_or_assign (
                key, cached_spot_route_fence_t{std::move (fence), expires_at});
          })
          .get ();
    }
    return read ? std::optional<route_fence_t> (read->fence) : std::nullopt;
}

void public_host_runtime_t::invalidate_spot_route_fence (
  const protocol::message_follow_notice_t &notice)
{
    const auto *source = std::get_if<protocol::spot_route_fence_t> (&notice.source);
    if (!source)
        return;
    const auto target_node = zlink::routing_id_t::from (source->target_node_routing_id);
    const auto key = spot_route_cache_key (target_node, source->spot_id, source->object_generation);
    // A late notice may describe an older owner lease. Do not remove a
    // route that was published after that notice.
    const route_fence_t source_fence{source->authority_owner_generation,
                                     source->owner_lease_generation};
    _route_cache_lane
      .run ([this, &key, &source_fence] {
          const auto found = _spot_route_fences.find (key);
          if (found != _spot_route_fences.end () && found->second.fence == source_fence)
              _spot_route_fences.erase (found);
      })
      .get ();
}

task_t<zlink::submit_result_t> public_host_runtime_t::send_to_actor (
  const actor_ref_t &target,
  const std::vector<zlink::message_t> &parts,
  std::span<const std::uint8_t> metadata,
  std::uint64_t authority_owner_generation,
  std::uint64_t owner_lease_generation,
  std::optional<protocol::actor_message_header_t::bound_session_source_t> bound_session_source)
{
    const auto target_routing_id =
      zlink::routing_id_t::from (std::string (target.node_rid ().value ()));
    if (target_routing_id.to_bytes () == status ().routing_id ().to_bytes ()) {
        co_return enqueue_local_actor_message (target, record_kind_t::actor_send, parts,
                                               std::nullopt, std::move (bound_session_source));
    }
    const auto peer = _transport->topology ().peer (target_routing_id.to_bytes ());
    if (!peer) {
        co_return zlink::submit_result_t::not_connected;
    }
    peer_readiness_resolver_t readiness_resolver;
    _lifecycle_configuration_lane
      .run ([&] { readiness_resolver = _peer_readiness_resolver; })
      .get ();
    if (readiness_resolver && !readiness_resolver (target_routing_id)) {
        co_return zlink::submit_result_t::not_connected;
    }
    const auto node_generation = peer->descriptor.lifecycle_generation;
    const auto current_peer = _transport->topology ().peer (target_routing_id.to_bytes ());
    if (!current_peer || current_peer->descriptor.lifecycle_generation != node_generation) {
        co_return zlink::submit_result_t::not_connected;
    }
    const auto object = _objects.find (stateful::object_kind_t::actor,
                                       std::string (target.actor_id ().value ()));
    const auto authority_generation = authority_owner_generation != 0 ? authority_owner_generation
                                      : object ? object->authority_owner_generation
                                               : target.object_generation ();
    const auto route_fence = read_route_owner_fence (
      _user_spot_store, '1', target.actor_id ().value (), target.object_generation (),
      authority_generation, owner_lease_generation, _options.owner_lease_fencing_margin);
    if (!route_fence || route_fence->fence.first != authority_generation)
        co_return zlink::submit_result_t::not_found;
    co_return co_await _transport->send_to_actor_result (
      zlink::routing_id_t::from (std::string (target.node_rid ().value ())).to_bytes (),
      std::nullopt,
      protocol::actor_route_fence_t{
        std::string (target.actor_id ().value ()), target.object_generation (),
        zlink::routing_id_t::from (std::string (target.node_rid ().value ())).to_bytes (),
        node_generation, authority_generation, route_fence->fence.second},
      encode_application (parts, metadata), std::move (bound_session_source));
}

task_t<zlink::submit_result_t> public_host_runtime_t::send_bound_session (
  const actor_ref_t &actor,
  const zlink::routing_id_t &session_owner,
  std::uint64_t expected_binding_generation,
  std::uint64_t authority_owner_generation,
  std::uint64_t owner_lease_generation,
  const std::vector<zlink::message_t> &parts,
  zlink::framework::detail::backend::raw_send_stage_trace_t trace)
{
    const auto local = status ();
    const auto target_node =
      zlink::routing_id_t::from (std::string (actor.node_rid ().value ())).to_bytes ();
    if (target_node != local.routing_id ().to_bytes ()) {
        trace_mesh_host ("bound-session-send-rejected",
                         "reason=actor-node-mismatch actor="
                           + std::string (actor.actor_id ().value ()));
        co_return zlink::submit_result_t::not_found;
    }
    if (authority_owner_generation == 0) {
        trace_mesh_host ("bound-session-send-rejected",
                         "reason=bound-route-fence-mismatch actor="
                           + std::string (actor.actor_id ().value ()));
        co_return zlink::submit_result_t::not_found;
    }
    co_return co_await _transport->send_bound_session_result (
      session_owner.to_bytes (),
      protocol::bound_session_send_t{
        protocol::actor_route_fence_t{std::string (actor.actor_id ().value ()),
                                      actor.object_generation (), local.routing_id ().to_bytes (),
                                      local.lifecycle_generation (), authority_owner_generation,
                                      owner_lease_generation},
        expected_binding_generation},
      encode_application (parts), std::move (trace));
}

task_t<zlink::submit_result_t> public_host_runtime_t::request_to_actor (
  const actor_ref_t &target,
  const std::vector<zlink::message_t> &parts,
  call_id_t &operation,
  std::chrono::milliseconds timeout,
  std::span<const std::uint8_t> metadata,
  std::uint64_t authority_owner_generation,
  std::uint64_t owner_lease_generation,
  std::optional<protocol::actor_message_header_t::bound_session_source_t> bound_session_source)
{
    operation = next_operation ();
    if (!try_reserve_completion (operation))
        co_return zlink::submit_result_t::backpressured;
    const auto target_routing_id =
      zlink::routing_id_t::from (std::string (target.node_rid ().value ()));
    if (target_routing_id.to_bytes () == status ().routing_id ().to_bytes ()) {
        const auto accepted = enqueue_local_actor_message (
          target, record_kind_t::actor_request, parts, operation, std::move (bound_session_source));
        if (accepted != zlink::submit_result_t::ok)
            release_completion (operation);
        co_return accepted;
    }
    const auto peer = _transport->topology ().peer (target_routing_id.to_bytes ());
    if (!peer) {
        release_completion (operation);
        co_return zlink::submit_result_t::not_connected;
    }
    peer_readiness_resolver_t readiness_resolver;
    _lifecycle_configuration_lane
      .run ([&] { readiness_resolver = _peer_readiness_resolver; })
      .get ();
    if (readiness_resolver && !readiness_resolver (target_routing_id)) {
        release_completion (operation);
        co_return zlink::submit_result_t::not_connected;
    }
    const auto node_generation = peer->descriptor.lifecycle_generation;
    const auto current_peer = _transport->topology ().peer (target_routing_id.to_bytes ());
    if (!current_peer || current_peer->descriptor.lifecycle_generation != node_generation) {
        release_completion (operation);
        co_return zlink::submit_result_t::not_connected;
    }
    const auto object = _objects.find (stateful::object_kind_t::actor,
                                       std::string (target.actor_id ().value ()));
    const auto authority_generation = authority_owner_generation != 0 ? authority_owner_generation
                                      : object ? object->authority_owner_generation
                                               : target.object_generation ();
    const auto route_fence = read_route_owner_fence (
      _user_spot_store, '1', target.actor_id ().value (), target.object_generation (),
      authority_generation, owner_lease_generation, _options.owner_lease_fencing_margin);
    if (!route_fence || route_fence->fence.first != authority_generation) {
        release_completion (operation);
        co_return zlink::submit_result_t::not_found;
    }
    const auto host = shared_from_this ();
    const auto accepted = co_await _transport->request_to_actor (
      zlink::routing_id_t::from (std::string (target.node_rid ().value ())).to_bytes (),
      std::nullopt,
      protocol::actor_route_fence_t{
        std::string (target.actor_id ().value ()), target.object_generation (),
        zlink::routing_id_t::from (std::string (target.node_rid ().value ())).to_bytes (),
        node_generation, authority_generation, route_fence->fence.second},
      encode_application (parts, metadata), timeout,
      [host, operation] (foundation::operation_terminal_t terminal,
                         std::vector<std::uint8_t> payload) mutable {
          host->complete_operation (operation, operation_kind_t::none, terminal,
                                    std::move (payload));
      },
      protocol::wire_operation_id_t{operation.high, operation.low},
      std::move (bound_session_source), std::nullopt);
    if (!accepted)
        release_completion (operation);
    co_return submitted (accepted);
}

task_t<zlink::submit_result_t>
public_host_runtime_t::send_to_node (const zlink::routing_id_t &target,
                                     const std::vector<zlink::message_t> &parts)
{
    const auto target_bytes = target.to_bytes ();
    co_return co_await _transport->send_to_node_result (target_bytes, encode_application (parts));
}

task_t<zlink::submit_result_t>
public_host_runtime_t::request_to_node (const zlink::routing_id_t &target,
                                        const std::vector<zlink::message_t> &parts,
                                        call_id_t &operation,
                                        std::chrono::milliseconds timeout)
{
    operation = next_operation ();
    if (!try_reserve_completion (operation))
        co_return zlink::submit_result_t::backpressured;
    const auto host = shared_from_this ();
    const auto accepted = co_await _transport->request_to_node (
      target.to_bytes (), encode_application (parts), timeout,
      [host, operation] (foundation::operation_terminal_t terminal,
                         std::vector<std::uint8_t> payload) mutable {
          host->complete_operation (operation, operation_kind_t::none, terminal,
                                    std::move (payload));
      });
    if (!accepted) {
        release_completion (operation);
        //  Spec 32-framework-error-model:76-77 -- a target this runtime has
        //  never admitted (absent from the live peer table) does not exist
        //  from the requester's perspective and must complete NotFound, not
        //  Unavailable. `submitted(false)` below collapses every rejected
        //  request_to_node into `not_connected` (-> Unavailable via
        //  boundary_error_t::disconnected, see call_facade_runtime.cpp), but
        //  that is only correct for a target that WAS reachable and merely
        //  is not right now. service_topology_registry_t has no "known but
        //  currently unreachable" state distinct from "not a peer" --
        //  disconnect() erases the peer entry outright -- so any target
        //  absent from the live peer table is, by this runtime's own model,
        //  simply a target that does not exist. Matches Java's
        //  ZLinkJavaRawSpotNode.classifyNodeSendTarget (peerState absent ->
        //  TARGET_NOT_FOUND, i.e. NotFound) and Node's
        //  raw-service-mesh-runtime.ts knownTarget gate (unknown RID ->
        //  RequestResult.NotFound).
        if (!_transport->topology ().peer (target.to_bytes ()))
            co_return zlink::submit_result_t::not_found;
    }
    co_return submitted (accepted);
}

task_t<zlink::submit_result_t>
public_host_runtime_t::send_to_channel (const std::string &channel_name,
                                        const std::vector<zlink::message_t> &parts)
{
    co_return co_await _transport->send_to_channel_result (channel_name,
                                                           encode_application (parts));
}

task_t<zlink::submit_result_t>
public_host_runtime_t::request_to_channel (const std::string &channel_name,
                                           const std::vector<zlink::message_t> &parts,
                                           call_id_t &operation,
                                           std::chrono::milliseconds timeout)
{
    operation = next_operation ();
    if (!try_reserve_completion (operation))
        co_return zlink::submit_result_t::backpressured;
    const auto host = shared_from_this ();
    const auto accepted = co_await _transport->request_to_channel (
      channel_name, encode_application (parts), timeout,
      [host, operation] (foundation::operation_terminal_t terminal,
                         std::vector<std::uint8_t> payload) mutable {
          host->complete_operation (operation, operation_kind_t::none, terminal,
                                    std::move (payload));
      });
    if (!accepted)
        release_completion (operation);
    co_return submitted (accepted);
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
            && attempt.attempt_expires_at != std::chrono::steady_clock::time_point{}
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
            const relocation_attempt_key_t key{attempt.prepare.relocation.high,
                                               attempt.prepare.relocation.low,
                                               attempt.prepare.target_attempt_generation};
            _relocation_session_terminal_lane
              .run ([&] {
                  if (!_relocation_target_attempts.contains (key))
                      _relocation_target_attempts.emplace (key, std::move (attempt));
              })
              .get ();
            continue;
        }
        for (const auto &object : attempt.wire_objects) {
            try {
                (void) _relocation_wire->unregister_target (
                  attempt.prepare.relocation, attempt.prepare.target_attempt_generation, object);
            }
            catch (...) {
            }
        }
        try {
            if (attempt.targets.size () == 1)
                (void) _objects.abort_relocation_restore (attempt.targets.front (),
                                                          attempt.restore_identity);
            else
                (void) _objects.abort_relocation_restore_aggregate (attempt.targets,
                                                                    attempt.restore_identity);
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
    expired = _relocation_session_terminal_lane
                .run ([&] {
                    return take_expired_relocation_target_attempts_locked (
                      std::chrono::steady_clock::now ());
                })
                .get ();
    cleanup_expired_relocation_target_attempts (std::move (expired));
}

void public_host_runtime_t::poll_relocation_target_attempts ()
{
    /* 28/52: command 44 (session_relocation_route) is a one-way send,
     * submitted exactly once per route -- there is no periodic re-select
     * and resend of an incomplete route here. Only attempts not yet
     * finalized (S2 not yet reached) are polled to finalize. */
    std::vector<relocation_attempt_key_t> pending;
    const auto now = std::chrono::steady_clock::now ();
    _relocation_session_terminal_lane
      .run ([&] {
          for (const auto &[key, attempt] : _relocation_target_attempts) {
              if (!attempt.target_finalized && attempt.ready
                  && (attempt.cutover_received
                      || (attempt.ready_fallback_at != std::chrono::steady_clock::time_point{}
                          && attempt.ready_fallback_at <= now))) {
                  pending.push_back (key);
              }
          }
      })
      .get ();
    for (const auto &key : pending)
        (void) try_finalize_relocation_target (key);
}

void public_host_runtime_t::flush_pending_session_relocation_seals ()
{
    std::vector<session_relocation_key_t> pending;
    std::vector<session_relocation_key_t> expired;
    const auto now = std::chrono::steady_clock::now ();
    _relocation_session_terminal_lane
      .run ([&] {
          for (const auto &[key, record] : _session_seal_terminals) {
              if (!record.consumed && record.expires_at <= now)
                  expired.push_back (key);
              else if (!record.consumed && !record.ready)
                  pending.push_back (key);
          }
      })
      .get ();
    for (const auto &key : expired) {
        stateful::stream_barrier_t barrier;
        std::vector<session_seal_local_completion_t> local_completions;
        const auto expired_current = _relocation_session_terminal_lane
                                       .run ([&] {
                                           const auto found = _session_seal_terminals.find (key);
                                           if (found == _session_seal_terminals.end ()
                                               || found->second.consumed
                                               || found->second.expires_at > now)
                                               return false;
                                           barrier = found->second.barrier;
                                           local_completions =
                                             std::move (found->second.local_completions);
                                           found->second.consumed = true;
                                           return true;
                                       })
                                       .get ();
        if (!expired_current)
            continue;
        (void) _sessions.close_remote_route_seal (barrier);
        for (auto &complete : local_completions) {
            complete (foundation::operation_terminal_t::timed_out, std::nullopt);
        }
    }
    for (const auto &key : pending) {
        stateful::stream_barrier_t barrier;
        const auto pending_current = _relocation_session_terminal_lane
                                       .run ([&] {
                                           const auto found = _session_seal_terminals.find (key);
                                           if (found == _session_seal_terminals.end ()
                                               || found->second.consumed || found->second.ready)
                                               return false;
                                           barrier = found->second.barrier;
                                           return true;
                                       })
                                       .get ();
        if (!pending_current)
            continue;
        if (!_sessions.remote_route_seal_ready (barrier))
            continue;
        protocol::session_relocation_sealed_t sealed;
        std::vector<std::uint8_t> target;
        std::vector<session_seal_local_completion_t> local_completions;
        const auto completed = _relocation_session_terminal_lane
                                 .run ([&] {
                                     const auto found = _session_seal_terminals.find (key);
                                     if (found == _session_seal_terminals.end ()
                                         || found->second.consumed)
                                         return false;
                                     found->second.ready = true;
                                     sealed = found->second.sealed;
                                     target = found->second.response_routing_id;
                                     local_completions =
                                       std::move (found->second.local_completions);
                                     return true;
                                 })
                                 .get ();
        if (!completed)
            continue;
        if (!target.empty ())
            (void) _transport->send_session_relocation_sealed (target, sealed);
        for (auto &complete : local_completions) {
            complete (foundation::operation_terminal_t::completed, sealed);
        }
    }
}

task_t<void> public_host_runtime_t::submit_relocation_session_routes (relocation_attempt_key_t key)
{
    /* 28 §4.7/52: command 44 is a one-way submit -- there is no application
     * reply, so there is nothing to retry on. Each route is dispatched at
     * most once: `send_attempted` is stamped under lock before the send is
     * issued, so a re-entry for this key (this function may be invoked
     * again for an already-finalized attempt on a duplicate cutover or
     * relocationData delivery) can never re-dispatch a route that a prior
     * call already attempted, whether that prior send succeeded or
     * failed. A late duplicate 44 could otherwise cross with a newer
     * relocation and corrupt routing. */
    struct due_route_t
    {
        std::size_t index = 0;
        protocol::session_relocation_route_t route;
    };
    std::vector<due_route_t> due;
    const auto routes_current = _relocation_session_terminal_lane
                                  .run ([&] {
                                      const auto found = _relocation_target_attempts.find (key);
                                      if (found == _relocation_target_attempts.end ()
                                          || found->second.authority_committed_at
                                               == std::chrono::steady_clock::time_point{})
                                          return false;
                                      for (std::size_t index = 0;
                                           index != found->second.session_routes.size (); ++index) {
                                          auto &state = found->second.session_routes[index];
                                          if (state.completed || state.send_attempted)
                                              continue;
                                          state.send_attempted = true;
                                          due.push_back ({index, state.route});
                                      }
                                      return true;
                                  })
                                  .get ();
    if (!routes_current)
        co_return;

    for (auto &pending : due) {
        bool submitted = false;
        try {
            submitted = co_await route_session_remote (
              zlink::routing_id_t::from (pending.route.session_owner_node_routing_id),
              pending.route);
        }
        catch (...) {
            submitted = false;
        }
        /* Whether the one-way send succeeded or failed, this route will
         * never be attempted again (Finding 8), so the source-local
         * journal-terminal bookkeeping this route's seal prepared is done
         * either way and must be released now -- there is no future retry
         * left to release it on. */
        std::optional<stateful::durable_session_journal_root_t> completed_journal;
        std::shared_ptr<stateful::relocation_store_port_t> session_relocations;
        const auto route_current = _relocation_session_terminal_lane
                                     .run ([&] {
                                         const auto found = _relocation_target_attempts.find (key);
                                         if (found == _relocation_target_attempts.end ()
                                             || pending.index >= found->second.session_routes.size ())
                                             return false;
                                         auto &state = found->second.session_routes[pending.index];
                                         if (state.completed || state.route != pending.route)
                                             return false;
                                         if (submitted) {
                                             state.completed = true;
                                         } else {
                /* Record the failure in the (already-bounded, retention-
                 * limited) per-attempt state rather than swallow it --
                 * there is no gated trace/diagnostics sink reachable from
                 * public_host_runtime_t to route this through instead.
                 * This is a one-way send: it is not retried. */
                                             state.send_failed = true;
                                         }
                                         const auto journal = _session_journal_terminals.find (
                                           session_relocation_key (state.route));
                                         if (journal != _session_journal_terminals.end ()) {
                                             completed_journal = journal->second.second.journal_root;
                                             session_relocations = _session_relocations;
                                             _session_journal_terminals.erase (journal);
                                         }
                                         return true;
                                     })
                                     .get ();
        if (!route_current)
            continue;
        if (completed_journal && session_relocations) {
            try {
                stateful::durable_session_journal_store_t journal_store (
                  std::move (session_relocations));
                journal_store.cleanup (*completed_journal);
            }
            catch (...) {
            }
        }
    }
    co_return;
}

void public_host_runtime_t::start_relocation_session_route_submission (relocation_attempt_key_t key)
{
    auto pending =
      std::make_shared<task_t<void>> (submit_relocation_session_routes (std::move (key)));
    detail::observe_task_completion (*pending, [pending] (const result_t<void> &) {});
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
            const auto current = _relocation_authority->read (target.kind, target.key);
            if (!current || current->target != target)
                return false;
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

bool public_host_runtime_t::relocation_target_authority_committed_strict (
  const relocation_target_attempt_t &attempt) const noexcept
{
    if (!_relocation_authority || attempt.targets.empty ())
        return false;
    try {
        for (const auto &target : attempt.targets) {
            const auto current = _relocation_authority->read (target.kind, target.key);
            if (!current || current->target != target)
                return false;
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

bool public_host_runtime_t::commit_relocation_target_authority (
  relocation_target_attempt_t &attempt) noexcept
{
    if (!_relocation_authority || attempt.sources.empty ()
        || attempt.sources.size () != attempt.targets.size ())
        return false;

    const location_owner_token_t target_owner{
      attempt.prepare.target.target_owner_id,
      static_cast<std::int64_t> (attempt.prepare.target.target_owner_lease_generation)};
    if (target_owner.owner_id.empty () || target_owner.lease_generation <= 0)
        return false;

    try {
        const auto adopt_store_fences = [&] {
            std::vector<stateful::authority_relocation_reference_t> current;
            current.reserve (attempt.sources.size ());
            for (std::size_t index = 0; index != attempt.sources.size (); ++index) {
                const auto read = _relocation_authority->read (attempt.sources[index].kind,
                                                               attempt.sources[index].key);
                if (!read || read->source != attempt.sources[index])
                    return false;
                const auto &staged = attempt.targets[index];
                const auto &committed = read->target;
                if (committed.kind != staged.kind || committed.key != staged.key
                    || committed.object_generation != staged.object_generation
                    || committed.mesh_name != staged.mesh_name
                    || committed.node_id != staged.node_id
                    || committed.authority_owner_generation
                         <= read->source.authority_owner_generation
                    || read->target_owner.owner_id != target_owner.owner_id
                    || read->target_owner.lease_generation != target_owner.lease_generation
                    || read->relocation_reference != attempt.restore_identity.reference
                    || read->checksum_crc32c != attempt.restore_identity.checksum_crc32c
                    || read->inventory_digest != attempt.restore_identity.inventory_digest) {
                    return false;
                }
                current.push_back (*read);
            }
            for (std::size_t index = 0; index != current.size (); ++index) {
                const auto &committed = current[index].target;
                if (committed != attempt.targets[index]
                    && _objects.reconcile_relocation_restore_authority (
                         attempt.targets[index], committed, attempt.restore_identity)
                         != stateful::stateful_error_t::none) {
                    return false;
                }
            }
            for (std::size_t index = 0; index != current.size (); ++index) {
                attempt.sources[index] = current[index].source;
                attempt.targets[index] = current[index].target;
            }
            return true;
        };
        if (adopt_store_fences ())
            return true;

        if (attempt.sources.size () == 1) {
            const object_creation_target_t target_placement{
              attempt.targets.front ().mesh_name,
              node_rid_t::from_string (attempt.targets.front ().node_id),
              attempt.prepare.target.target_node_generation, target_owner};
            std::vector<std::byte> target_application_payload;
            if (attempt.targets.front ().kind == stateful::object_kind_t::actor
                && _actor_join_authority_spot_resolver) {
                const auto spot = _actor_join_authority_spot_resolver (attempt.targets.front ());
                if (spot) {
                    target_application_payload =
                      runtime::encode_actor_authority_payload (runtime::actor_authority_payload_t{
                        .state = runtime::actor_authority_state_t::ready,
                        .stable_type = std::get<0> (*spot),
                        .actor_id = attempt.targets.front ().key,
                        .current_spot_id = std::get<1> (*spot),
                        .current_spot_generation = std::get<2> (*spot),
                        .current_spot_kind = runtime::actor_authority_spot_kind_t::user,
                        .owner_id = target_owner.owner_id,
                        .owner_lease_generation =
                          static_cast<std::uint64_t> (target_owner.lease_generation),
                        .mesh_name = attempt.targets.front ().mesh_name,
                        .node_rid = node_rid_t::from_string (attempt.targets.front ().node_id),
                        .node_generation = attempt.prepare.target.target_node_generation});
                }
            }
            const auto published = _relocation_authority->publish (
              attempt.sources.front (), attempt.targets.front (), target_owner, target_placement,
              attempt.restore_identity.reference, attempt.restore_identity.checksum_crc32c,
              attempt.restore_identity.inventory_digest, std::move (target_application_payload));
            if (published.status != stateful::authority_publish_status_t::published
                || !published.current || published.current->source != attempt.sources.front ())
                return adopt_store_fences ();
            const auto &committed = published.current->target;
            const auto &staged = attempt.targets.front ();
            if (committed.kind != staged.kind || committed.key != staged.key
                || committed.object_generation != staged.object_generation
                || committed.mesh_name != staged.mesh_name || committed.node_id != staged.node_id
                || committed.authority_owner_generation
                     <= published.current->source.authority_owner_generation
                || published.current->target_owner.owner_id != target_owner.owner_id
                || published.current->target_owner.lease_generation != target_owner.lease_generation
                || published.current->relocation_reference != attempt.restore_identity.reference
                || published.current->checksum_crc32c != attempt.restore_identity.checksum_crc32c
                || published.current->inventory_digest != attempt.restore_identity.inventory_digest)
                return false;
            if (committed != staged
                && _objects.reconcile_relocation_restore_authority (staged, committed,
                                                                    attempt.restore_identity)
                     != stateful::stateful_error_t::none)
                return false;
            attempt.sources.front () = published.current->source;
            attempt.targets.front () = committed;
            return true;
        }

        if (relocation_target_authority_committed_strict (attempt))
            return adopt_store_fences ();

        if (!_aggregate_relocation_authority)
            return false;
        if (!attempt.authority_fence) {
            const auto prepared = _aggregate_relocation_authority->prepare (
              attempt.sources, attempt.targets.front ().node_id, target_owner,
              attempt.restore_identity.reference, attempt.restore_identity.checksum_crc32c,
              attempt.restore_identity.inventory_digest);
            if (prepared.status != stateful::aggregate_publish_status_t::prepared)
                return relocation_target_authority_committed_strict (attempt)
                       && adopt_store_fences ();
            attempt.authority_fence = prepared.fence;
            const relocation_attempt_key_t key{attempt.prepare.relocation.high,
                                               attempt.prepare.relocation.low,
                                               attempt.prepare.target_attempt_generation};
            _relocation_session_terminal_lane
              .run ([&] {
                  const auto found = _relocation_target_attempts.find (key);
                  if (found != _relocation_target_attempts.end ()
                      && found->second.prepare == attempt.prepare)
                      found->second.authority_fence = attempt.authority_fence;
              })
              .get ();
        }
        const auto committed = _aggregate_relocation_authority->commit (*attempt.authority_fence);
        return (committed.status == stateful::aggregate_publish_status_t::committed
                && adopt_store_fences ())
               || adopt_store_fences ();
    }
    catch (...) {
        return false;
    }
}

bool public_host_runtime_t::adopt_committed_session_route_authorities (
  relocation_target_attempt_t &attempt) const noexcept
{
    for (auto &state : attempt.session_routes) {
        auto &route = state.route;
        if (route.route.action != protocol::session_relocation_route_action_t::commit
            || route.route.target_node_routing_id != attempt.prepare.target.target_node_routing_id
            || route.route.target_node_generation != attempt.prepare.target.target_node_generation)
            return false;

        const auto source =
          std::find_if (attempt.sources.begin (), attempt.sources.end (),
                        [&route] (const stateful::object_ref_t &candidate) {
                            return candidate.kind == stateful::object_kind_t::actor
                                   && candidate.key == route.actor.actor_id
                                   && candidate.object_generation == route.actor.object_generation;
                        });
        if (source == attempt.sources.end ())
            return false;
        const auto index =
          static_cast<std::size_t> (std::distance (attempt.sources.begin (), source));
        const auto &target = attempt.targets[index];
        if (target.kind != stateful::object_kind_t::actor || target.key != source->key
            || target.object_generation != source->object_generation
            || target.authority_owner_generation <= source->authority_owner_generation)
            return false;

        /* Command 44 is post-CAS. Its two authority fences come from the
         * exact Store relocation row adopted above; the pre-commit route
         * envelope is only transport staging and must never predict the
         * owner generation that the Session owner commits. */
        route.route.previous_authority_owner_generation = source->authority_owner_generation;
        route.route.target_authority_owner_generation = target.authority_owner_generation;
    }
    return true;
}

bool public_host_runtime_t::try_finalize_relocation_target (const relocation_attempt_key_t &key)
{
    relocation_target_attempt_t attempt;
    const auto attempt_current = _relocation_session_terminal_lane
                                   .run ([&] {
                                       const auto found = _relocation_target_attempts.find (key);
                                       if (found == _relocation_target_attempts.end ())
                                           return false;
                                       if (found->second.target_finalized) {
                                           attempt = found->second;
                                       } else {
                                           const auto now = std::chrono::steady_clock::now ();
                                           if (!found->second.ready
                                               || (!found->second.cutover_received
                                                   && (found->second.ready_fallback_at
                                                         == std::chrono::steady_clock::time_point{}
                                                       || now < found->second.ready_fallback_at)))
                                               return false;
                                           attempt = found->second;
                                       }
                                       return true;
                                   })
                                   .get ();
    if (!attempt_current)
        return false;
    if (attempt.target_finalized) {
        start_relocation_session_route_submission (key);
        return true;
    }

    if (!commit_relocation_target_authority (attempt))
        return false;
    if (!adopt_committed_session_route_authorities (attempt))
        return false;

    /* S2 (owner CAS confirmed): stamp once, on the first tick that
     * observes the authority commit, so a retried finalize does not push
     * the target_resume window forward. */
    const auto stamped = _relocation_session_terminal_lane
                           .run ([&] {
                               const auto found = _relocation_target_attempts.find (key);
                               if (found == _relocation_target_attempts.end ())
                                   return false;
                               if (found->second.prepare != attempt.prepare
                                   || found->second.session_routes.size ()
                                        != attempt.session_routes.size ())
                                   return false;
                               found->second.sources = attempt.sources;
                               found->second.targets = attempt.targets;
                               found->second.authority_fence = attempt.authority_fence;
                               for (std::size_t index = 0;
                                    index != attempt.session_routes.size (); ++index) {
                                   if (found->second.session_routes[index].send_attempted)
                                       return false;
                                   found->second.session_routes[index].route =
                                     attempt.session_routes[index].route;
                               }
                               if (found->second.authority_committed_at
                                   == std::chrono::steady_clock::time_point{})
                                   found->second.authority_committed_at =
                                     std::chrono::steady_clock::now ();
                               attempt.authority_committed_at = found->second.authority_committed_at;
                               return true;
                           })
                           .get ();
    if (!stamped)
        return false;

    // ZLJR-backed User-Spot Join owns a bound-Session prewarm at the
    // target. A recovery-free maintenance/whole-node import has no joined
    // Actor runtime to prewarm; it still submits its one-way Session route
    // below, but must retain the generic import path.
    const auto join_prepare = _actor_join_relocation_prepare_validator
                                ? _actor_join_relocation_prepare_validator (attempt.prepare)
                                : std::optional<bool>{};
    const auto canonical_user_spot_join = join_prepare && *join_prepare;
    if (canonical_user_spot_join) {
        for (const auto &target : attempt.targets) {
            if (target.kind == stateful::object_kind_t::actor
                && (!_actor_join_committed_authority_adopter
                    || !_actor_join_committed_authority_adopter (
                      target, attempt.prepare.target.target_node_generation,
                      attempt.prepare.target.target_owner_lease_generation))) {
                return false;
            }
        }
        for (const auto &route_state : attempt.session_routes) {
            const auto &route = route_state.route;
            if (!_bound_session_operations.prepare_relocation_target_route
                || !_bound_session_operations.prepare_relocation_target_route (
                  route, attempt.prepare.target.target_owner_lease_generation))
                return false;
        }
    }

    /* The target Actor route is now installed and S2 authority is durable.
     * Submit command 44 before opening the restored Actor lifecycle: an
     * OnJoined callback may immediately push to its bound Session, and that
     * push must be ordered behind the Session owner's route commit. */
    start_relocation_session_route_submission (key);

    const auto committed =
      attempt.targets.size () == 1
        ? _objects.commit_relocation_restore (attempt.targets.front (), attempt.restore_identity)
        : _objects.commit_relocation_restore_aggregate (attempt.targets, attempt.restore_identity);
    if (committed != stateful::stateful_error_t::none
        && committed != stateful::stateful_error_t::already_exists)
        return false;

    const auto actor_indexes_current = _spot_actor_index_lane
      .run ([&] {
        for (std::size_t index = 0; index != attempt.targets.size (); ++index) {
            const auto &target = attempt.targets[index];
            if (target.kind != stateful::object_kind_t::actor)
                continue;
            const auto &wire = attempt.wire_objects[index];
            const auto current = _actors.find (target.key);
            if (current != _actors.end ()
                && (current->second.second.object_generation > target.object_generation
                    || current->second.second.authority_owner_generation
                         > target.authority_owner_generation))
                return false;
            _actors.insert_or_assign (target.key, std::make_pair (wire.stable_type, target));
        }
        return true;
      })
      .get ();
    if (!actor_indexes_current)
        return false;

    for (const auto &object : attempt.wire_objects)
        (void) _relocation_wire->unregister_target (
          attempt.prepare.relocation, attempt.prepare.target_attempt_generation, object);

    const auto finalized = _relocation_session_terminal_lane
                             .run ([&] {
                                 const auto found = _relocation_target_attempts.find (key);
                                 if (found == _relocation_target_attempts.end ())
                                     return false;
                                 found->second.target_finalized = true;
                                 found->second.attempt_expires_at = {};
                                 return true;
                             })
                             .get ();
    if (!finalized)
        return false;
    /* Metrics 25 §"zlink.relocation": cutover_timeout counts a fallback CAS
     * (target proceeded without a verified cutover); target_resume is the
     * target-local S2 (owner CAS confirmed) -> dispatch-open duration.
     * Both are emitted exactly once per attempt, on the same tick that
     * flips target_finalized. */
    if (!attempt.cutover_received && _relocation_target_metrics.cutover_timeout)
        _relocation_target_metrics.cutover_timeout ();
    if (_relocation_target_metrics.target_resume_seconds
        && attempt.authority_committed_at != std::chrono::steady_clock::time_point{}) {
        const auto elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now ()
                                                            - attempt.authority_committed_at);
        _relocation_target_metrics.target_resume_seconds (elapsed.count ());
    }
    start_relocation_session_route_submission (key);
    return true;
}

void public_host_runtime_t::reply_relocation_assembly_failure (
  const pending_relocation_assembly_t &pending, protocol::framework_error_code code)
{
    (void) _transport->reply_relocation_failed (
      pending.request,
      protocol::relocation_failed_t{
        pending.prepare.relocation, pending.prepare.target_attempt_generation,
        pending.prepare.coordinator, pending.prepare.target, pending.prepare.object,
        protocol::relocation_role_t::target, static_cast<std::uint32_t> (code)});
}

void public_host_runtime_t::rollback_actor_join_recoveries (
  const std::vector<std::pair<std::string, stateful::object_ref_t>> &consumed) noexcept
{
    if (!_actor_join_recovery_rollback)
        return;
    for (const auto &[stable_type, target] : consumed) {
        try {
            _actor_join_recovery_rollback (stable_type, target);
        }
        catch (...) {
        }
    }
}

void public_host_runtime_t::discard_relocation_assembly_staging (
  const pending_relocation_assembly_t &pending,
  const relocation_assembly_staging_t &staging) noexcept
{
    unregister_relocation_wire_targets (
      pending.prepare.relocation, pending.prepare.target_attempt_generation, staging.wire_objects);
}

void public_host_runtime_t::unregister_relocation_wire_targets (
  const protocol::relocation_id_t &relocation,
  std::uint64_t target_attempt_generation,
  const std::vector<protocol::relocation_object_t> &wire_objects) noexcept
{
    for (const auto &wire_object : wire_objects) {
        try {
            (void) _relocation_wire->unregister_target (relocation, target_attempt_generation,
                                                        wire_object);
        }
        catch (...) {
        }
    }
}

bool public_host_runtime_t::restore_relocation_assembly (
  const pending_relocation_assembly_t &pending, const relocation_assembly_staging_t &staging)
{
    stateful::stateful_error_t restored = stateful::stateful_error_t::conflict;
    try {
        std::optional<stateful::object_ref_t> actor_join_target_spot;
        if (staging.targets.size () == 1
            && staging.targets.front ().kind == stateful::object_kind_t::actor
            && _actor_join_authority_spot_resolver) {
            const auto spot = _actor_join_authority_spot_resolver (staging.targets.front ());
            if (spot) {
                actor_join_target_spot = stateful::object_ref_t{stateful::object_kind_t::user_spot,
                                                                std::get<1> (*spot),
                                                                std::get<2> (*spot),
                                                                0,
                                                                staging.targets.front ().mesh_name,
                                                                staging.targets.front ().node_id};
            }
        }
        restored = staging.targets.size () == 1
                     ? _objects.restore_relocation (
                         staging.frozen.front (), staging.targets.front (),
                         staging.restore_identity, {}, std::move (actor_join_target_spot))
                     : _objects.restore_relocation_aggregate (staging.frozen, staging.targets,
                                                              staging.restore_identity, {});
    }
    catch (...) {
        discard_relocation_assembly_staging (pending, staging);
        reply_relocation_assembly_failure (pending, protocol::framework_error_code::requestFailed);
        return false;
    }
    if (restored == stateful::stateful_error_t::none
        || restored == stateful::stateful_error_t::already_exists)
        return true;
    discard_relocation_assembly_staging (pending, staging);
    reply_relocation_assembly_failure (pending, protocol::framework_error_code::requestFailed);
    return false;
}

void public_host_runtime_t::activate_relocation_assembly (
  const relocation_attempt_key_t &key,
  const pending_relocation_assembly_t &pending,
  relocation_assembly_staging_t staging)
{
    relocation_target_attempt_t attempt;
    attempt.prepare = pending.prepare;
    attempt.restore_identity = staging.restore_identity;
    attempt.sources = std::move (staging.sources);
    attempt.targets = std::move (staging.targets);
    attempt.wire_objects = std::move (staging.wire_objects);
    for (const auto &route : staging.session_routes) {
        attempt.session_routes.emplace_back ();
        attempt.session_routes.back ().route = route;
    }
    attempt.ready = true;
    attempt.attempt_expires_at = std::chrono::steady_clock::now () + relocation_attempt_retention;
    const auto inserted = _relocation_session_terminal_lane
                            .run ([&] {
                                return _relocation_target_attempts.emplace (key, std::move (attempt))
                                  .second;
                            })
                            .get ();
    if (!inserted) {
        reply_relocation_assembly_failure (pending,
                                           protocol::framework_error_code::requestFailed);
        return;
    }
    const auto ready_sent = _transport->reply_relocation_ready (
      pending.request, protocol::relocation_ready_t{
                         pending.prepare.relocation, pending.prepare.target_attempt_generation,
                         pending.prepare.coordinator, pending.prepare.target,
                         pending.prepare.object, protocol::relocation_role_t::target});
    if (ready_sent) {
        _relocation_session_terminal_lane
          .run ([&] {
              const auto found = _relocation_target_attempts.find (key);
              if (found != _relocation_target_attempts.end ())
                  found->second.ready_fallback_at =
                    std::chrono::steady_clock::now () + _relocation_cutover_wait;
          })
          .get ();
        return;
    }
    std::optional<relocation_target_attempt_t> aborted;
    _relocation_session_terminal_lane
      .run ([&] {
          const auto found = _relocation_target_attempts.find (key);
          if (found != _relocation_target_attempts.end ()) {
              aborted.emplace (std::move (found->second));
              _relocation_target_attempts.erase (found);
          }
      })
      .get ();
    if (!aborted)
        return;
    unregister_relocation_wire_targets (aborted->prepare.relocation,
                                        aborted->prepare.target_attempt_generation,
                                        aborted->wire_objects);
    try {
        if (aborted->targets.size () == 1)
            (void) _objects.abort_relocation_restore (aborted->targets.front (),
                                                      aborted->restore_identity);
        else
            (void) _objects.abort_relocation_restore_aggregate (aborted->targets,
                                                                aborted->restore_identity);
    }
    catch (...) {
    }
}

void public_host_runtime_t::complete_relocation_assembly (const relocation_attempt_key_t &key,
                                                          pending_relocation_assembly_t pending)
{
    // relocationDataLost(35) is reserved for a verified checksum/assembly/
    // digest/conflict integrity failure — the assembled payload itself, or
    // its identity against the negotiated Prepare, is provably wrong.
    // A restore/factory/staging failure below (target queue registration,
    // the factory/restore path throwing, a retried restore still failing,
    // or a duplicate attempt-key conflict) is not a payload integrity
    // failure and must not encode 35; it maps to requestFailed(17), same
    // as maintenance_runtime.cpp's aggregate path already does for the
    // equivalent restore_failed case (relocation_terminal_t::blocked, not
    // data_lost).
    const auto reply_failure = [&] (protocol::framework_error_code code =
                                      protocol::framework_error_code::relocationDataLost) {
        reply_relocation_assembly_failure (pending, code);
    };
    auto payload = pending.assembly.take_payload ();
    /* The direct-transfer payload is exactly the schema's
     * relocation-envelope-v1 logical stream (28 §4.2): no provider
     * envelope, no embedded digest, no session-route section. Identity
     * and integrity were already verified by the assembly against the
     * command-40 manifest. */
    const auto envelope = stateful::maintenance_runtime_t::decode_envelope (payload);
    if (!envelope) {
        trace_mesh_host ("relocation-assembly-failed", "stage=decode");
        reply_failure ();
        return;
    }
    if (envelope->object.kind != pending.prepare.object.kind
        || envelope->object.object_id != pending.prepare.object.object_id
        || envelope->object.object_generation != pending.prepare.object.object_generation
        || envelope->application_version != pending.prepare.application_version
        || (envelope->object.kind != protocol::relocation_object_kind_t::instance_spot
            && envelope->object.expected_authority_owner_generation
                 != pending.prepare.object.expected_authority_owner_generation)) {
        trace_mesh_host ("relocation-assembly-failed",
                         "stage=principal-identity-or-application-version");
        reply_failure ();
        return;
    }

    const auto local = status ();
    stateful::object_kind_t principal_kind;
    switch (envelope->object.kind) {
        case protocol::relocation_object_kind_t::actor:
            principal_kind = stateful::object_kind_t::actor;
            break;
        case protocol::relocation_object_kind_t::user_spot:
            principal_kind = stateful::object_kind_t::user_spot;
            break;
        case protocol::relocation_object_kind_t::instance_spot:
            principal_kind = stateful::object_kind_t::instance_spot;
            break;
        default:
            reply_failure ();
            return;
    }
    stateful::relocation_participant_identity_t principal_identity;
    principal_identity.owner.kind = principal_kind;
    principal_identity.owner.key = envelope->object.object_id;
    principal_identity.owner.object_generation = envelope->object.object_generation;
    principal_identity.owner.authority_owner_generation =
      pending.prepare.object.expected_authority_owner_generation != 0
        ? pending.prepare.object.expected_authority_owner_generation
        : envelope->object.expected_authority_owner_generation;
    principal_identity.owner.mesh_name = _options.mesh.descriptor.mesh_name;
    principal_identity.owner.node_id = std::string (pending.prepare.source_node_routing_id.begin (),
                                                    pending.prepare.source_node_routing_id.end ());
    principal_identity.stable_type = pending.prepare.object.stable_type;

    /* Participant identity is deliberately absent from the stream. The
     * canonical ordered inventory is reconstructed from Location Store
     * authority keys: the principal row plus, for a User Spot aggregate,
     * every Actor row whose authority payload projects membership of that
     * exact Spot. */
    std::vector<stateful::relocation_participant_identity_t> inventory;
    std::optional<std::vector<stateful::relocation_participant_identity_t>> rows;
    try {
        rows = _relocation_authority ? _relocation_authority->list_participant_identities ()
                                     : std::nullopt;
    }
    catch (...) {
        rows = std::nullopt;
    }
    if (envelope->application_states.size () == 1) {
        /* Even a single-participant unit takes its stable type (which the
         * wire object deliberately omits for Actors and User Spots) from
         * the principal's authority row when the store can serve it. */
        if (rows) {
            for (const auto &row : *rows) {
                if (row.owner.kind != principal_kind
                    || row.owner.key != principal_identity.owner.key)
                    continue;
                if (!row.stable_type.empty ())
                    principal_identity.stable_type = row.stable_type;
                if (!row.owner.mesh_name.empty ())
                    principal_identity.owner.mesh_name = row.owner.mesh_name;
                if (!row.owner.node_id.empty ())
                    principal_identity.owner.node_id = row.owner.node_id;
                break;
            }
        }
        inventory.push_back (principal_identity);
    } else {
        if (!rows) {
            // Inventory enumeration unavailable — a staging failure, not a
            // verified payload integrity failure.
            reply_failure (protocol::framework_error_code::requestFailed);
            return;
        }
        for (auto &row : *rows) {
            if (row.owner.kind == principal_kind && row.owner.key == principal_identity.owner.key) {
                if (row.owner.object_generation != principal_identity.owner.object_generation
                    || row.owner.authority_owner_generation
                         != principal_identity.owner.authority_owner_generation) {
                    trace_mesh_host ("relocation-assembly-failed", "stage=principal-row-fence");
                    reply_failure ();
                    return;
                }
                auto principal_row = principal_identity;
                if (!row.stable_type.empty ())
                    principal_row.stable_type = row.stable_type;
                if (!row.owner.mesh_name.empty ())
                    principal_row.owner.mesh_name = row.owner.mesh_name;
                if (!row.owner.node_id.empty ())
                    principal_row.owner.node_id = row.owner.node_id;
                inventory.push_back (std::move (principal_row));
            } else if (principal_kind == stateful::object_kind_t::user_spot
                       && row.owner.kind == stateful::object_kind_t::actor && row.spot_membership
                       && row.spot_membership->first == principal_identity.owner.key) {
                if (row.owner.mesh_name.empty ())
                    row.owner.mesh_name = principal_identity.owner.mesh_name;
                if (row.owner.node_id.empty ())
                    row.owner.node_id = principal_identity.owner.node_id;
                inventory.push_back (std::move (row));
            }
        }
        if (inventory.size () != envelope->application_states.size ()) {
            trace_mesh_host ("relocation-assembly-failed",
                             "stage=inventory-count derived=" + std::to_string (inventory.size ())
                               + " declared="
                               + std::to_string (envelope->application_states.size ()));
            reply_failure ();
            return;
        }
    }

    auto materialized =
      stateful::maintenance_runtime_t::materialize_envelope (*envelope, std::move (inventory));
    if (!materialized) {
        trace_mesh_host ("relocation-assembly-failed", "stage=materialize");
        reply_failure ();
        return;
    }
    relocation_assembly_staging_t staging;
    staging.frozen = std::move (*materialized);
    auto &frozen = staging.frozen;

    auto &sources = staging.sources;
    auto &targets = staging.targets;
    auto &wire_objects = staging.wire_objects;
    bool principal_found = false;
    for (const auto &saved : frozen) {
        protocol::relocation_object_kind_t kind;
        switch (saved.owner.kind) {
            case stateful::object_kind_t::actor:
                kind = protocol::relocation_object_kind_t::actor;
                break;
            case stateful::object_kind_t::user_spot:
                kind = protocol::relocation_object_kind_t::user_spot;
                break;
            case stateful::object_kind_t::instance_spot:
                kind = protocol::relocation_object_kind_t::instance_spot;
                break;
            default:
                reply_failure ();
                return;
        }
        if (saved.owner.authority_owner_generation == std::numeric_limits<std::uint64_t>::max ()) {
            reply_failure ();
            return;
        }
        protocol::relocation_object_t wire_object{kind, saved.stable_type, saved.owner.key,
                                                  saved.owner.object_generation,
                                                  saved.owner.authority_owner_generation};
        principal_found = principal_found || wire_object == pending.prepare.object;
        auto target = saved.owner;
        target.node_id = local.routing_id ().to_string ();
        ++target.authority_owner_generation;
        sources.push_back (saved.owner);
        targets.push_back (std::move (target));
        wire_objects.push_back (std::move (wire_object));
    }
    if (!principal_found) {
        trace_mesh_host ("relocation-assembly-failed", "stage=principal-not-found");
        reply_failure ();
        return;
    }

    /* No wrapper digest travels with the stream any more: the derived
     * inventory digest is recomputed from the reconstructed participants,
     * the same value every source computes before publishing an authority
     * row (an unpublished row's all-zero digest is the pending sentinel). */
    std::vector<stateful::object_ref_t> digest_owners;
    digest_owners.reserve (frozen.size ());
    for (const auto &saved : frozen)
        digest_owners.push_back (saved.owner);
    const auto inventory_digest =
      stateful::maintenance_runtime_t::compute_inventory_digest (digest_owners);

    /* Bound-session commit routes ride beside the Restore request (the
     * schema payload has no session-route section). Validate each staged
     * route against the reconstructed participants and this exact prepare
     * before command 44 leaves this target after CAS and queue opening. */
    auto &staged_session_routes = staging.session_routes;
    for (std::size_t part = 1; part < pending.request.parts.size (); ++part) {
        protocol::session_relocation_route_t route;
        try {
            route = protocol::decode_session_relocation_route (pending.request.parts[part]);
        }
        catch (...) {
            reply_failure ();
            return;
        }
        const auto saved =
          std::find_if (frozen.begin (), frozen.end (), [&route] (const auto &candidate) {
              return candidate.owner.kind == stateful::object_kind_t::actor
                     && candidate.owner.key == route.actor.actor_id
                     && candidate.owner.object_generation == route.actor.object_generation;
          });
        const auto duplicate = std::find_if (
          staged_session_routes.begin (), staged_session_routes.end (),
          [&route] (const auto &candidate) { return candidate.actor == route.actor; });
        if (saved == frozen.end () || duplicate != staged_session_routes.end ()
            || route.relocation != pending.prepare.relocation
            || route.coordinator != pending.prepare.coordinator
            || route.sender_role != protocol::relocation_role_t::target
            || route.route.action != protocol::session_relocation_route_action_t::commit
            || route.route.previous_authority_owner_generation
                 != saved->owner.authority_owner_generation
            || route.route.target_authority_owner_generation
                 != saved->owner.authority_owner_generation + 1
            || route.route.target_node_routing_id != local.routing_id ().to_bytes ()
            || route.route.target_node_generation != local.lifecycle_generation ()) {
            trace_mesh_host ("relocation-assembly-failed", "stage=session-route");
            reply_failure ();
            return;
        }
        staged_session_routes.push_back (std::move (route));
    }

    staging.restore_identity = {"direct:" + std::to_string (pending.prepare.relocation.high) + ":"
                                  + std::to_string (pending.prepare.relocation.low) + ":"
                                  + std::to_string (pending.prepare.target_attempt_generation),
                                pending.prepare.payload_checksum_crc32c, inventory_digest};
    std::size_t registered_count = 0;
    for (; registered_count != targets.size (); ++registered_count) {
        if (register_relocation_target_queue (pending.prepare, targets[registered_count],
                                              wire_objects[registered_count]))
            continue;
        for (std::size_t index = 0; index != registered_count; ++index) {
            try {
                (void) _relocation_wire->unregister_target (
                  pending.prepare.relocation, pending.prepare.target_attempt_generation,
                  wire_objects[index]);
            }
            catch (...) {
            }
        }
        // Target relocation-wire registration failed (e.g. an existing
        // registration for this object) — a staging conflict, not a
        // payload integrity failure.
        reply_failure (protocol::framework_error_code::requestFailed);
        return;
    }
    /* Every Actor whose ZLJR record the consumer already took. A staging
     * failure past this point must give those entries back (15 §4.2), or the
     * leftover refuses every later Join attempt for that Actor on this node. */
    std::vector<std::pair<std::string, stateful::object_ref_t>> consumed;
    if (_actor_join_recovery_consumer) {
        for (std::size_t index = 0; index != frozen.size (); ++index) {
            if (frozen[index].owner.kind != stateful::object_kind_t::actor)
                continue;
            const auto stable_type = frozen[index].stable_type;
            if (!_actor_join_recovery_consumer (frozen[index], targets[index], pending.prepare)) {
                rollback_actor_join_recoveries (consumed);
                unregister_relocation_wire_targets (pending.prepare.relocation,
                                                    pending.prepare.target_attempt_generation,
                                                    wire_objects);
                reply_failure (protocol::framework_error_code::requestProtocolError);
                return;
            }
            consumed.emplace_back (stable_type, targets[index]);
        }
    }
    // Factory/restore failures are staging failures, not payload-integrity
    // failures; the helper tears down every queue it registered first.
    if (!restore_relocation_assembly (pending, staging)) {
        rollback_actor_join_recoveries (consumed);
        return;
    }

    activate_relocation_assembly (key, pending, std::move (staging));
}

bool public_host_runtime_t::register_relocation_target_queue (
  const protocol::relocation_prepare_t &prepare,
  const stateful::object_ref_t &target,
  const protocol::relocation_object_t &wire_object)
{
    try {
        const relocation_attempt_key_t attempt_key{prepare.relocation.high, prepare.relocation.low,
                                                   prepare.target_attempt_generation};
        return _relocation_wire->register_target (
          {prepare.relocation, prepare.target_attempt_generation, prepare.coordinator,
           prepare.source_node_routing_id, prepare.source_node_generation, wire_object,
           [this, target, attempt_key] (const protocol::relocation_data_t &data) {
               const auto frozen_record = data.record;
               const auto staged =
                 _stateful_dispatch
                 && _stateful_dispatch->stage_relocated (
                      target, {0, protocol::encode_frozen_record (frozen_record)},
                      [this, data, frozen_record] (
                        const std::optional<protocol::application_payload_t> &reply) {
                          if (!frozen_record.reply_route_id)
                              return true;
                          const auto terminal_sequence = frozen_record.operation.low != 0
                                                           ? frozen_record.operation.low
                                                           : frozen_record.operation.high;
                          const protocol::reply_relay_t relay{
                            frozen_record.operation,
                            *frozen_record.reply_route_id,
                            data.relocation,
                            data.target_attempt_generation,
                            data.coordinator,
                            1,
                            terminal_sequence,
                            reply ? 0u : 105u,
                            reply ? protocol::framework_error_code::none
                                  : protocol::framework_error_code::requestFailed};
                          return _relocation_wire->register_terminal_target (
                            {relay, frozen_record.source, reply,
                             [] (protocol::reply_relay_ack_status_t) { return true; },
                             [] { return true; }, data.coordinator.node_routing_id});
                      })
                      == stateful::stateful_error_t::none;
               /* Pre-boundary relay verification (28 §4.4/§12): count and
               * checksum every relocationData record the target actually
               * staged for this attempt, in receive order, so the cutover
               * comparison can detect a defect before CAS runs. Only
               * records that were accepted into staging are counted here
               * — accumulating a record that failed to stage (invalid or
               * backpressured) would let the boundary count/checksum match
               * the source's manifest even though the record itself is
               * missing, which would let cutover CAS open with data
               * silently lost. This mirrors the source's
               * boundary_batch_checksum computation exactly.
               *
               * 28's "duplicatePayload: may-be-accepted-twice" means
               * stage_relocated legitimately reports success again for a
               * record the source resent (retransmission-window retry
               * resends the whole boundary batch ahead of each cutover
               * retry, see retain_retransmission_copies) — commit_
               * accepted_ingress allocates a fresh sequence per call and
               * does not itself detect the duplicate. Without a dedup
               * here, a resent record would inflate boundary_records_
               * received and the accumulator past the source's one-time
               * manifest, and the cutover comparison would then fail on
               * every retried unit. Dedup by content hash (relocationData
               * carries no explicit per-record ordinal) so a re-staged
               * duplicate is not re-counted. */
               if (staged) {
                   const auto encoded = protocol::encode_relocation_control (data);
                   const auto digest = std::hash<std::string_view>{}(std::string_view (
                     reinterpret_cast<const char *> (encoded.data ()), encoded.size ()));
                   _relocation_session_terminal_lane
                     .run ([&] {
                         const auto found = _relocation_target_attempts.find (attempt_key);
                         if (found != _relocation_target_attempts.end ()
                             && !found->second.cutover_received && !found->second.target_finalized
                             && found->second.boundary_record_digests_seen.insert (digest).second) {
                             ++found->second.boundary_records_received;
                             found->second.boundary_accumulator.update (encoded);
                         }
                     })
                     .get ();
               }
               return staged;
           },
           [] (const protocol::relocation_data_t &) {}});
    }
    catch (...) {
        return false;
    }
}

task_t<std::size_t> public_host_runtime_t::dispatch_user_spot_operations ()
{
    std::shared_ptr<zlink::framework::location_repository_t> store;
    user_spot_materializer_t materializer;
    actor_create_operation_target_t actor_create_target;
    actor_join_operation_target_t actor_join_target;
    instance_spot_activation_materializer_t instance_materializer;
    std::shared_ptr<stateful::relocation_store_port_t> instance_relocations;
    location_owner_token_t instance_owner;
    std::function<std::optional<location_owner_token_t> ()> session_route_owner_resolver;
    std::function<void (const protocol::message_follow_notice_t &)> message_follow_handler;
    bound_session_operations_t bound_session_operations;
    std::function<void (const protocol::session_relocation_route_t &)>
      late_session_route_update_reporter;
    _lifecycle_configuration_lane
      .run ([&] {
          store = _user_spot_store;
          materializer = _user_spot_materializer;
          actor_create_target = _actor_create_target;
          actor_join_target = _actor_join_target;
          instance_materializer = _instance_spot_materializer;
          instance_relocations = _instance_spot_relocations;
          instance_owner = _instance_spot_owner;
          session_route_owner_resolver = _session_route_owner_resolver;
          message_follow_handler = _message_follow_handler;
          bound_session_operations = _bound_session_operations;
          late_session_route_update_reporter = _late_session_route_update_reporter;
      })
      .get ();
    expire_relocation_target_attempts ();
    std::vector<pending_relocation_assembly_t> expired_relocation_assemblies;
    const auto now = std::chrono::steady_clock::now ();
    _relocation_session_terminal_lane
      .run ([&] {
          for (auto pending = _relocation_assemblies.begin ();
               pending != _relocation_assemblies.end ();) {
              if (pending->second.expires_at > now) {
                  ++pending;
                  continue;
              }
              expired_relocation_assemblies.push_back (std::move (pending->second));
              pending = _relocation_assemblies.erase (pending);
          }
      })
      .get ();
    for (const auto &expired : expired_relocation_assemblies) {
        (void) _transport->reply_relocation_failed (
          expired.request,
          protocol::relocation_failed_t{
            expired.prepare.relocation, expired.prepare.target_attempt_generation,
            expired.prepare.coordinator, expired.prepare.target, expired.prepare.object,
            protocol::relocation_role_t::target,
            static_cast<std::uint32_t> (protocol::framework_error_code::relocationDataLost)});
    }
    poll_relocation_target_attempts ();
    flush_pending_session_relocation_seals ();
    std::size_t count = 0;
    receive_batch_budget_t infrastructure_budget;
    while (auto claim = _transport->mailbox ().try_claim (
             mesh::service_mailbox_domain_t::infrastructure,
             dispatch_limits::receive_batch_messages, dispatch_limits::receive_batch_bytes)) {
        for (auto &mailbox_record : claim->records) {
            std::size_t record_bytes = 0;
            for (const auto &part : mailbox_record.parts)
                record_bytes =
                  part.size () > std::numeric_limits<std::size_t>::max () - record_bytes
                    ? std::numeric_limits<std::size_t>::max ()
                    : record_bytes + part.size ();
            infrastructure_budget.account (record_bytes);
            ++count;
            try {
                const auto wire = protocol::decode_header (mailbox_record.parts.front ());
                if (wire.kind == protocol::command::messageFollow) {
                    if (mailbox_record.parts.size () != 1
                        || mailbox_record.source_node_generation == 0)
                        continue;
                    const auto notice =
                      protocol::decode_message_follow (mailbox_record.parts.front ());
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
                if (wire.kind == protocol::command::boundSessionBind) {
                    const auto bind =
                      protocol::decode_bound_session_bind (mailbox_record.parts.front ());
                    const auto local = status ();
                    const auto actor =
                      _objects.find (stateful::object_kind_t::actor, bind.actor.actor_id);
                    const auto authority_matches =
                      bound_session_bind_actor_matches (
                        bind.actor, actor, local.routing_id (), local.lifecycle_generation ());
                    const auto admission =
                      classify_bound_session_bind_admission (authority_matches);
                    trace_mesh_host (
                      "bound-session-bind-admission",
                      "actor=" + bind.actor.actor_id + " admission="
                        + (admission == bound_session_bind_admission_t::ready ? "ready"
                           : admission == bound_session_bind_admission_t::stale_route
                             ? "stale_route"
                             : "actor_not_ready")
                        + " requested_authority="
                        + std::to_string (bind.actor.authority_owner_generation)
                        + " requested_lease=" + std::to_string (bind.actor.owner_lease_generation)
                        + " authority_matches=" + (authority_matches ? "true" : "false")
                        + " local_actor=" + (actor ? "found" : "missing"));
                    bound_session_bind_operation_result_t operation_result{
                      stateful::stateful_error_t::conflict, std::nullopt};
                    if (admission == bound_session_bind_admission_t::ready
                        && bound_session_operations.bind) {
                        operation_result = bound_session_operations.bind (
                          bind, zlink::routing_id_t::from (mailbox_record.source_routing_id),
                          mailbox_record.source_node_generation);
                    }
                    const auto replied = _transport->reply_bound_session_bind (
                      mailbox_record,
                      operation_result.error == stateful::stateful_error_t::none ? 0u
                      : admission == bound_session_bind_admission_t::stale_route
                        ? static_cast<std::uint32_t> (protocol::request_terminal_result::conflict)
                      : admission == bound_session_bind_admission_t::actor_not_ready
                        ? static_cast<std::uint32_t> (protocol::request_terminal_result::busy)
                        : static_cast<std::uint32_t> (protocol::request_terminal_result::notFound),
                      operation_result.error == stateful::stateful_error_t::none ? 0u
                      : admission == bound_session_bind_admission_t::stale_route
                        ? static_cast<std::uint32_t> (
                            protocol::framework_error_code::actorLocationStale)
                      : admission == bound_session_bind_admission_t::actor_not_ready
                        ? 0u
                        : static_cast<std::uint32_t> (
                            protocol::framework_error_code::actorSessionNotBound));
                    if (replied && operation_result.replacement) {
                        (void) co_await _transport->send_bound_session_replaced (
                          operation_result.replacement->retired_session
                            .session_owner_node_routing_id,
                          *operation_result.replacement);
                    }
                    continue;
                }
                if (wire.kind == protocol::command::boundSessionReplaced) {
                    const auto replacement =
                      protocol::decode_bound_session_replaced (mailbox_record.parts.front ());
                    trace_mesh_host ("bound-session-replacement-received",
                                     "actor=" + replacement.actor_authority.actor_id);
                    if (bound_session_operations.replaced)
                        bound_session_operations.replaced (replacement);
                    continue;
                }
                if (wire.kind == protocol::command::relocationPrepare) {
                    if (mailbox_record.parts.empty () || !mailbox_record.reply_token
                        || !session_route_owner_resolver)
                        continue;
                    const auto control =
                      protocol::decode_relocation_control (mailbox_record.parts.front ());
                    const auto *prepare = std::get_if<protocol::relocation_prepare_t> (&control);
                    const auto local = status ();
                    const auto owner = session_route_owner_resolver ();
                    if (!prepare || !owner
                        || prepare->initiator_role != protocol::relocation_role_t::source
                        || prepare->target.target_node_routing_id != local.routing_id ().to_bytes ()
                        || prepare->target.target_node_generation != local.lifecycle_generation ()
                        || prepare->target.target_owner_id != owner->owner_id
                        || prepare->target.target_owner_lease_generation
                             != static_cast<std::uint64_t> (owner->lease_generation)
                        || prepare->source_node_routing_id != mailbox_record.source_routing_id
                        || prepare->source_node_generation != mailbox_record.source_node_generation)
                        continue;

                    if (_actor_join_relocation_prepare_validator) {
                        const auto join_prepare =
                          _actor_join_relocation_prepare_validator (*prepare);
                        if (join_prepare && !*join_prepare) {
                            (void) _transport->reply_relocation_failed (
                              mailbox_record,
                              protocol::relocation_failed_t{
                                prepare->relocation, prepare->target_attempt_generation,
                                prepare->coordinator, prepare->target, prepare->object,
                                protocol::relocation_role_t::target,
                                static_cast<std::uint32_t> (
                                  protocol::framework_error_code::requestProtocolError)});
                            continue;
                        }
                    }

                    const relocation_attempt_key_t key{prepare->relocation.high,
                                                       prepare->relocation.low,
                                                       prepare->target_attempt_generation};
                    const auto prepare_state = _relocation_session_terminal_lane
                                                 .run ([&] {
                                                 const auto found =
                                                   _relocation_target_attempts.find (key);
                                                 if (found == _relocation_target_attempts.end ())
                                                     return 0;
                                                 if (found->second.prepare != *prepare)
                                                     return 2;
                                                 found->second.attempt_expires_at =
                                                   std::chrono::steady_clock::now ()
                                                   + relocation_attempt_retention;
                                                 return 1;
                                             })
                                             .get ();
                    if (prepare_state == 2)
                        continue;
                    const auto duplicate = prepare_state == 1;
                    if (duplicate) {
                        const auto replied = _transport->reply_relocation_ready (
                          mailbox_record, protocol::relocation_ready_t{
                                            prepare->relocation, prepare->target_attempt_generation,
                                            prepare->coordinator, prepare->target, prepare->object,
                                            protocol::relocation_role_t::target});
                        if (replied) {
                            _relocation_session_terminal_lane
                              .run ([&] {
                                  const auto found = _relocation_target_attempts.find (key);
                                  if (found != _relocation_target_attempts.end ())
                                      found->second.ready_fallback_at =
                                        std::chrono::steady_clock::now ()
                                        + _relocation_cutover_wait;
                              })
                              .get ();
                        }
                        continue;
                    }

                    if (prepare->payload_total_length == 0
                        || prepare->payload_total_length > protocol::relocationLogicalBytes
                        || prepare->payload_chunk_count == 0
                        || prepare->payload_chunk_count > protocol::relocationChunkCount)
                        continue;
                    _relocation_session_terminal_lane
                      .run ([&] {
                          const auto found = _relocation_assemblies.find (key);
                          if (found != _relocation_assemblies.end ()) {
                              if (found->second.prepare == *prepare)
                                  found->second.expires_at = std::chrono::steady_clock::now ()
                                                            + relocation_assembly_retention;
                              return;
                          }
                          _relocation_assemblies.emplace (
                            key,
                            pending_relocation_assembly_t{
                              *prepare, std::move (mailbox_record),
                              stateful::relocation_state_assembly_t{
                                prepare->relocation,
                                prepare->target_attempt_generation,
                                prepare->coordinator,
                                prepare->object,
                                {prepare->payload_total_length, prepare->payload_chunk_count,
                                 prepare->payload_checksum_crc32c}},
                              false,
                              std::chrono::steady_clock::now () + relocation_assembly_retention});
                      })
                      .get ();
                    continue;
                }
                if (wire.kind == protocol::command::relocationState) {
                    if (mailbox_record.parts.size () != 1)
                        continue;
                    const auto control =
                      protocol::decode_relocation_control (mailbox_record.parts.front ());
                    const auto *state = std::get_if<protocol::relocation_state_t> (&control);
                    if (!state || state->sender_role != protocol::relocation_role_t::source
                        || state->chunk_data.empty ())
                        continue;
                    const relocation_attempt_key_t key{state->relocation.high,
                                                       state->relocation.low,
                                                       state->target_attempt_generation};
                    std::optional<pending_relocation_assembly_t> completed;
                    std::optional<pending_relocation_assembly_t> failed;
                    const auto assembly_current = _relocation_session_terminal_lane
                                                    .run ([&] {
                                                        const auto found =
                                                          _relocation_assemblies.find (key);
                                                        if (found == _relocation_assemblies.end ()
                                                            || found->second.prepare.coordinator
                                                                 != state->coordinator
                                                            || found->second.prepare.object
                                                                 != state->object
                                                            || found->second.prepare
                                                                 .source_node_routing_id
                                                                 != mailbox_record.source_routing_id
                                                            || found->second.prepare
                                                                 .source_node_generation
                                                                 != mailbox_record.source_node_generation)
                                                            return false;
                                                        const auto accepted =
                                                          found->second.assembly.accept (*state);
                                                        if (accepted
                                                            == stateful::relocation_assembly_result_t::conflict) {
                                                            failed.emplace (std::move (found->second));
                                                            _relocation_assemblies.erase (found);
                                                        } else if (accepted
                                                                   == stateful::relocation_assembly_result_t::completed) {
                                                            completed.emplace (std::move (found->second));
                                                            _relocation_assemblies.erase (found);
                                                        }
                                                        return true;
                                                    })
                                                    .get ();
                    if (!assembly_current)
                        continue;
                    if (failed) {
                        (void) _transport->reply_relocation_failed (
                          failed->request,
                          protocol::relocation_failed_t{
                            failed->prepare.relocation, failed->prepare.target_attempt_generation,
                            failed->prepare.coordinator, failed->prepare.target,
                            failed->prepare.object, protocol::relocation_role_t::target,
                            static_cast<std::uint32_t> (
                              protocol::framework_error_code::relocationDataLost)});
                    }
                    if (completed)
                        complete_relocation_assembly (key, std::move (*completed));
                    continue;
                }
                if (wire.kind == protocol::command::relocationCutover) {
                    if (mailbox_record.parts.size () != 1)
                        continue;
                    const auto control =
                      protocol::decode_relocation_control (mailbox_record.parts.front ());
                    const auto *cutover = std::get_if<protocol::relocation_cutover_t> (&control);
                    if (!cutover || cutover->sender_role != protocol::relocation_role_t::source)
                        continue;
                    const relocation_attempt_key_t key{cutover->relocation.high,
                                                       cutover->relocation.low,
                                                       cutover->target_attempt_generation};
                    bool accepted = false;
                    std::optional<relocation_target_attempt_t> mismatched;
                    _relocation_session_terminal_lane
                      .run ([&] {
                          const auto found = _relocation_target_attempts.find (key);
                          if (found != _relocation_target_attempts.end ()
                              && found->second.prepare.coordinator == cutover->coordinator
                              && found->second.prepare.object == cutover->object
                              && found->second.prepare.source_node_routing_id
                                   == mailbox_record.source_routing_id
                              && found->second.prepare.source_node_generation
                                   == mailbox_record.source_node_generation) {
                              if (found->second.cutover_received || found->second.target_finalized) {
                                /* Late or duplicate cutover (28 §4.4): the
                                 * boundary already resolved — matched,
                                 * mismatched, or the target already moved
                                 * on via the cutover-timeout fallback.
                                 * State is never re-verified or changed. */
                              } else if (found->second.boundary_records_received
                                           == cutover->boundary_record_count
                                         && found->second.boundary_accumulator.value ()
                                              == cutover->boundary_checksum_crc32c) {
                                  found->second.cutover_received = true;
                                  accepted = true;
                              } else {
                                /* Ordered connection: the boundary record
                                 * count/checksum the source declares must
                                 * match what the target staged. A mismatch
                                 * here is an implementation defect, not a
                                 * retryable condition (28 §4.4/§12) — do
                                 * not run CAS on a payload that may be
                                 * incomplete; discard the prepared target
                                 * state instead (partial-state cleanup,
                                 * same path as an expired attempt). Cutover
                                 * is one-way and carries no request
                                 * sequence, so there is no reply route back
                                 * to source for this failure; source never
                                 * waits on a target completion reply
                                 * (28 §4.7) and its own submit/cutover-wait
                                 * timers unwind independently. */
                                  mismatched.emplace (std::move (found->second));
                                  _relocation_target_attempts.erase (found);
                              }
                            }
                      })
                      .get ();
                    if (accepted) {
                        (void) try_finalize_relocation_target (key);
                    } else if (mismatched) {
                        std::vector<relocation_target_attempt_t> cleanup;
                        cleanup.push_back (std::move (*mismatched));
                        cleanup_expired_relocation_target_attempts (std::move (cleanup));
                    }
                    continue;
                }
                if (wire.kind == protocol::command::relocationData
                    || wire.kind == protocol::command::replyRelay
                    || wire.kind == protocol::command::replyRelayAck) {
                    const auto processed = co_await _relocation_wire->process (mailbox_record);
                    if (wire.kind == protocol::command::relocationData
                        && processed == stateful::raw_relocation_replay_result_t::applied) {
                        const auto control =
                          protocol::decode_relocation_control (mailbox_record.parts.front ());
                        if (const auto *data =
                              std::get_if<protocol::relocation_data_t> (&control)) {
                            (void) try_finalize_relocation_target (
                              {data->relocation.high, data->relocation.low,
                               data->target_attempt_generation});
                        }
                    }
                    continue;
                }
                if (wire.kind != protocol::command::sessionRelocationSeal
                    && wire.kind != protocol::command::sessionRelocationRoute
                    && wire.kind != protocol::command::userSpotCreate
                    && wire.kind != protocol::command::userSpotClose
                    && wire.kind != protocol::command::actorCreate
                    && wire.kind != protocol::command::actorJoin
                    && wire.kind != protocol::command::instanceSpot)
                    throw protocol::service_wire_error_t (
                      "unsupported infrastructure mailbox command");

                if (wire.kind == protocol::command::sessionRelocationSeal) {
                    if (mailbox_record.parts.size () != 1 || !session_route_owner_resolver)
                        continue;
                    const auto seal =
                      protocol::decode_session_relocation_seal (mailbox_record.parts.front ());
                    const auto owner = session_route_owner_resolver ();
                    if (!owner)
                        continue;
                    const auto [accepted, immediate] = admit_session_relocation_seal (
                      seal, *owner, mailbox_record.source_routing_id);
                    if (!accepted)
                        continue;
                    if (immediate)
                        (void) co_await _transport->send_session_relocation_sealed (
                          mailbox_record.source_routing_id, *immediate);
                    continue;
                }

                if (wire.kind == protocol::command::sessionRelocationRoute) {
                    if (mailbox_record.parts.size () != 1 || !session_route_owner_resolver) {
                        continue;
                    }
                    const auto route =
                      protocol::decode_session_relocation_route (mailbox_record.parts.front ());
                    const auto authenticated_sender =
                      route.route.action == protocol::session_relocation_route_action_t::commit
                        ? route.sender_role == protocol::relocation_role_t::target
                            && mailbox_record.source_node_generation != 0
                            && mailbox_record.source_routing_id
                                 == route.route.target_node_routing_id
                            && mailbox_record.source_node_generation
                                 == route.route.target_node_generation
                        : route.sender_role == protocol::relocation_role_t::source
                            && mailbox_record.source_node_generation != 0
                            && mailbox_record.source_routing_id == route.coordinator.node_routing_id
                            && mailbox_record.source_node_generation
                                 == route.coordinator.node_generation;
                    if (!authenticated_sender) {
                        continue;
                    }

                    const auto owner = session_route_owner_resolver ();
                    if (!owner || owner->owner_id != route.session_owner_id
                        || route.session_owner_lease_generation > static_cast<std::uint64_t> (
                             std::numeric_limits<std::int64_t>::max ())
                        || owner->lease_generation
                             != static_cast<std::int64_t> (route.session_owner_lease_generation)) {
                        continue;
                    }

                    const auto relocation_key = session_relocation_key (route);
                    std::uint64_t sealed_authority = 0;
                    bool late_session_route_update = false;
                    const auto route_matches_seal = _relocation_session_terminal_lane
                                                     .run ([&] {
                                                         const auto sealed =
                                                           _session_seal_terminals.find (relocation_key);
                                                         if (sealed
                                                               == _session_seal_terminals.end ()
                                                             || sealed->second.consumed) {
                                                             late_session_route_update = true;
                                                             return true;
                                                         }
                                                         if (!sealed->second.ready
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
                                                                  != route.session_owner_node_routing_id
                                                             || sealed->second.seal
                                                                  .session_owner_node_generation
                                                                  != route.session_owner_node_generation
                                                             || sealed->second.seal.session_owner_id
                                                                  != route.session_owner_id
                                                             || sealed->second.seal
                                                                  .session_owner_lease_generation
                                                                  != route.session_owner_lease_generation
                                                             || sealed->second.seal.session_routing_id
                                                                  != route.session_routing_id
                                                             || sealed->second.seal.binding_generation
                                                                  != route.binding_generation)
                                                             return false;
                                                         sealed_authority = sealed->second.seal.actor
                                                                              .authority_owner_generation;
                                                         return !((route.route.action
                                                                    == protocol::session_relocation_route_action_t::commit
                                                                  && route.route
                                                                       .previous_authority_owner_generation
                                                                       != sealed_authority)
                                                                  || (route.route.action
                                                                        == protocol::session_relocation_route_action_t::abort
                                                                      && route.route
                                                                           .current_authority_owner_generation
                                                                           != sealed_authority));
                                                     })
                                                     .get ();
                    if (!route_matches_seal)
                        continue;
                    if (late_session_route_update) {
                        if (late_session_route_update_reporter) {
                            try {
                                late_session_route_update_reporter (route);
                            }
                            catch (...) {
                            }
                        }
                        continue;
                    }

                    const auto session_id =
                      zlink::routing_id_t::from (route.session_routing_id).to_hex ();
                    stateful::stream_route_admission_t admission;
                    if (route.route.action == protocol::session_relocation_route_action_t::commit) {
                        const auto current = _sessions.current_binding (route.actor.actor_id);
                        if (!current) {
                            continue;
                        }
                        const auto target_node =
                          zlink::routing_id_t::from (route.route.target_node_routing_id);
                        auto target_proof = _sessions.remote_tenure_proof (
                          route.actor.actor_id, route.binding_generation,
                          route.actor.object_generation,
                          route.route.target_authority_owner_generation, target_node.to_string (),
                          route.route.target_node_generation);
                        auto target = current->actor;
                        target.node_id = target_node.to_string ();
                        target.authority_owner_generation =
                          route.route.target_authority_owner_generation;
                        stateful::stream_session_registry_t::route_terminal_commit_t
                          commit_projection;
                        if (bound_session_operations.commit_relocation_route) {
                            commit_projection =
                              [&route, &bound_session_operations, previous = *current] (
                                const stateful::stream_route_admission_t &committed) {
                                  return committed.binding
                                         && bound_session_operations.commit_relocation_route (
                                           route, previous, *committed.binding);
                              };
                        }
                        admission = _sessions.commit_remote_route (
                          session_id, route.binding_generation, route.actor.actor_id,
                          route.actor.object_generation,
                          route.route.previous_authority_owner_generation, std::move (target),
                          route.route.target_node_generation,
                          target_proof ? target_proof->tenure.owner_lease_generation : 0,
                          std::move (commit_projection));
                    } else {
                        admission = _sessions.acknowledge_remote_abort (
                          session_id, route.binding_generation, route.actor.actor_id,
                          route.actor.object_generation,
                          route.route.current_authority_owner_generation);
                    }
                    if (admission.error != stateful::stateful_error_t::none || !admission.binding) {
                        continue;
                    }

                    _relocation_session_terminal_lane
                      .run ([&] {
                          const auto sealed = _session_seal_terminals.find (relocation_key);
                          if (sealed != _session_seal_terminals.end () && !sealed->second.consumed
                              && sealed->second.ready)
                              sealed->second.consumed = true;
                      })
                      .get ();
                    for (auto &settle : admission.retained_outbound) {
                        if (!settle)
                            continue;
                        try {
                            settle (route.route.action
                                    == protocol::session_relocation_route_action_t::commit);
                        }
                        catch (...) {
                        }
                    }
                    continue;
                }

                if (wire.kind == protocol::command::actorCreate) {
                    const auto request =
                      protocol::decode_actor_create_header (mailbox_record.parts.front ());
                    if (!actor_create_target
                        || request.deadline_unix_ms <= unix_milliseconds_now ()) {
                        actor_create_operation_result_t result;
                        result.reply.header.correlation = request.correlation;
                        result.reply.header.terminal_result = actor_create_target ? 101u : 105u;
                        result.reply.header.failure_code =
                          actor_create_target
                            ? 0u
                            : static_cast<std::uint32_t> (
                                protocol::framework_error_code::actorCreateFailed);
                        (void) _transport->reply_actor_create (
                          mailbox_record, result.reply, std::move (result.application_reply));
                    } else {
                        auto completed = std::make_shared<std::atomic_bool> (false);
                        auto reply = [weak = weak_from_this (), mailbox_record,
                                      completed] (actor_create_operation_result_t result) mutable {
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
                                protocol::framework_error_code::actorCreateFailed)};
                            reply (std::move (result));
                        }
                    }
                    continue;
                }

                if (wire.kind == protocol::command::actorJoin) {
                    if (mailbox_record.parts.empty () || mailbox_record.parts.size () > 2)
                        throw protocol::service_wire_error_t (
                          "Actor join has an invalid part count");
                    const auto canonical = try_decode_canonical_actor_join (mailbox_record.parts);
                    if (!canonical)
                        throw protocol::service_wire_error_t (
                          "Actor join is not a canonical command-28 record");
                    const auto &request = canonical->request;
                    const auto &payload = canonical->payload;
                    if (!actor_join_target) {
                        (void) _transport->reply_actor_join (
                          mailbox_record, protocol::actor_join_result_t::rejected, std::nullopt, 0,
                          0);
                        continue;
                    }
                    auto completed = std::make_shared<std::atomic_bool> (false);
                    auto reply = [weak = weak_from_this (), mailbox_record,
                                  completed] (actor_join_operation_result_t result) mutable {
                        if (completed->exchange (true, std::memory_order_acq_rel))
                            return;
                        const auto host = weak.lock ();
                        if (!host)
                            return;
                        try {
                            (void) host->_transport->reply_actor_join (
                              mailbox_record, result.join_result, result.spot,
                              result.membership_epoch, result.receive_chunk_limit_bytes,
                              result.terminal_result, result.failure_code,
                              std::move (result.application_reply));
                        }
                        catch (...) {
                        }
                    };
                    try {
                        actor_join_target (request, payload, reply);
                    }
                    catch (...) {
                        reply (actor_join_operation_result_t{});
                    }
                    continue;
                }

                if (wire.kind == protocol::command::instanceSpot) {
                    const auto request = protocol::decode_instance_spot_activation_header (
                      mailbox_record.parts.front ());
                    const auto expected_parts = request.has_metadata ? 3u : 2u;
                    if (mailbox_record.parts.size () != expected_parts)
                        throw protocol::service_wire_error_t (
                          "Instance Spot activation has an invalid part count");
                    if (!store || !instance_relocations || !instance_materializer) {
                        (void) _transport->reply_instance_spot_activation (
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
                    const auto application = protocol::decode_application_payload (
                      mailbox_record.parts[application_index], capture_flow ());
                    const auto reply_terminal = [&] (instance_spot_activation_result_t result) {
                        (void) _transport->reply_instance_spot_activation (
                          mailbox_record, result.terminal_result, result.failure_code,
                          std::move (result.application_reply));
                    };
                    if (request.target.deadline_unix_ms <= unix_milliseconds_now ()) {
                        reply_terminal ({101, 0, std::nullopt});
                        continue;
                    }
                    const protocol::instance_activation_recovery_t recovery{request, metadata,
                                                                            application};
                    const auto recovery_bytes =
                      protocol::encode_instance_activation_recovery (recovery);
                    auto fingerprint_request = request;
                    fingerprint_request.reply_route_id = request.request ? 1 : 0;
                    const auto fingerprint_bytes = protocol::encode_instance_activation_recovery (
                      {fingerprint_request, metadata, application});
                    std::vector<std::byte> recovery_public;
                    recovery_public.reserve (fingerprint_bytes.size ());
                    for (const auto value : fingerprint_bytes)
                        recovery_public.push_back (static_cast<std::byte> (value));
                    const auto request_sha256 = runtime::sha256 (recovery_public);
                    const auto authority_key = spot_authority_key (request.target.spot_id);
                    const auto join_existing = [&] (authority_read_result_t current) {
                        while (const auto *snapshot =
                                 std::get_if<authority_snapshot_t> (&current)) {
                            if (snapshot->allocation.object_kind
                                  != placement_object_kind_t::instance_spot
                                || snapshot->allocation.stable_type != request.target.stable_type) {
                                reply_terminal (
                                  {107,
                                   static_cast<std::uint32_t> (
                                     protocol::framework_error_code::spotTypeMismatch),
                                   std::nullopt});
                                return true;
                            }
                            if (snapshot->allocation.state
                                == placement_allocation_state_t::active) {
                                auto ready_state =
                                  decode_instance_ready_state (snapshot->payload, capture_flow ());
                                if (!ready_state) {
                                    if (const auto closing =
                                          decode_instance_closing_state (snapshot->payload);
                                        closing
                                        && closing->stable_type == request.target.stable_type
                                        && closing->spot_id == request.target.spot_id
                                        && closing->object_generation == snapshot->object_generation
                                        && closing->authority_owner_generation
                                             == snapshot->authority_owner_generation) {
                                        reply_terminal (
                                          {107,
                                           static_cast<std::uint32_t> (
                                             protocol::framework_error_code::spotMoving),
                                           std::nullopt});
                                        return true;
                                    }
                                    reply_terminal (
                                      {105,
                                       static_cast<std::uint32_t> (
                                         protocol::framework_error_code::requestFailed),
                                       std::nullopt});
                                    return true;
                                }
                                if (ready_state->operation == request.operation) {
                                    if (!ready_state->completed) {
                                        /* The reservation winner published
                                         * Ready before its durable terminal.
                                         * Join that pending operation. */
                                    } else if (ready_state->request_sha256 != request_sha256) {
                                        reply_terminal (
                                          {104,
                                           static_cast<std::uint32_t> (
                                             protocol::framework_error_code::requestProtocolError),
                                           std::nullopt});
                                        return true;
                                    } else {
                                        reply_terminal ({ready_state->terminal_result,
                                                         ready_state->failure_code,
                                                         ready_state->reply});
                                        return true;
                                    }
                                } else {
                                    const auto local = status ();
                                    const auto current_rid = zlink::routing_id_t::from (
                                      std::string (snapshot->allocation.target.node_rid.value ()));
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
                                          snapshot->allocation.target.node_lifecycle_generation;
                                        const auto copied_mailbox = mailbox_record;
                                        const auto remaining = std::chrono::milliseconds (
                                          request.target.deadline_unix_ms > unix_milliseconds_now ()
                                            ? request.target.deadline_unix_ms
                                                - unix_milliseconds_now ()
                                            : 0);
                                        auto relayed = std::make_shared<
                                          task_t<bool>> (activate_instance_spot_remote (
                                          current_rid, std::move (forwarded), metadata, application,
                                          remaining,
                                          [transport = _transport, copied_mailbox] (
                                            foundation::operation_terminal_t terminal,
                                            protocol::reply_header_t reply,
                                            std::optional<protocol::application_payload_t>
                                              application_reply) {
                                              if (terminal
                                                  != foundation::operation_terminal_t::completed) {
                                                  reply.terminal_result = 105;
                                                  reply.failure_code = static_cast<std::uint32_t> (
                                                    protocol::framework_error_code::requestFailed);
                                                  application_reply.reset ();
                                              }
                                              (void) transport->reply_instance_spot_activation (
                                                copied_mailbox, reply.terminal_result,
                                                reply.failure_code, std::move (application_reply));
                                          }));
                                        detail::observe_task_completion (
                                          *relayed,
                                          [relayed, transport = _transport,
                                           copied_mailbox] (const result_t<bool> &result) {
                                              if (result && result.value ())
                                                  return;
                                              (void) transport->reply_instance_spot_activation (
                                                copied_mailbox, 103, 0, std::nullopt);
                                          });
                                        return true;
                                    }
                                    bool prepared = false;
                                    try {
                                        prepared = instance_materializer.prepare (request);
                                    }
                                    catch (...) {
                                        prepared = false;
                                    }
                                    if (!prepared) {
                                        reply_terminal (
                                          {105,
                                           static_cast<std::uint32_t> (
                                             protocol::framework_error_code::spotCreateFailed),
                                           std::nullopt});
                                        return true;
                                    }
                                    auto result = instance_materializer.dispatch (request, metadata,
                                                                                  application);
                                    ready_state->operation = request.operation;
                                    ready_state->request_sha256 = request_sha256;
                                    ready_state->completed = true;
                                    ready_state->terminal_result = result.terminal_result;
                                    ready_state->failure_code = result.failure_code;
                                    ready_state->reply = result.application_reply;
                                    const auto stored =
                                      store
                                        ->compare_exchange_authority (
                                          authority_key, snapshot->store_version,
                                          authority_put_t{
                                            encode_instance_ready_state (*ready_state)})
                                        .result ()
                                        .value ();
                                    if (!std::holds_alternative<authority_stored_t> (stored))
                                        result = {105,
                                                  static_cast<std::uint32_t> (
                                                    protocol::framework_error_code::requestFailed),
                                                  std::nullopt};
                                    reply_terminal (std::move (result));
                                    return true;
                                }
                            }
                            if (request.target.deadline_unix_ms <= unix_milliseconds_now ()) {
                                reply_terminal ({101, 0, std::nullopt});
                                return true;
                            }
                            std::this_thread::sleep_for (std::chrono::milliseconds (1));
                            current = store->read_authority (authority_key).result ().value ();
                        }
                        return false;
                    };
                    const auto current = store->read_authority (authority_key).result ().value ();
                    if (join_existing (current)) {
                        continue;
                    }
                    const auto recovery_checksum =
                      stateful::maintenance_runtime_t::crc32c (recovery_bytes);
                    const auto recovery_root =
                      instance_relocations->put (recovery_bytes, std::chrono::hours (24));
                    if (recovery_root.reference.empty ()
                        || recovery_root.checksum_crc32c != recovery_checksum) {
                        reply_terminal ({105,
                                         static_cast<std::uint32_t> (
                                           protocol::framework_error_code::requestFailed),
                                         std::nullopt});
                        continue;
                    }
                    object_reserve_request_t reserve;
                    reserve.key = {placement_object_kind_t::instance_spot, request.target.spot_id};
                    reserve.intent.stable_type = request.target.stable_type;
                    reserve.intent.request_content_reference = recovery_root.reference;
                    reserve.intent.request_sha256 = request_sha256;
                    reserve.intent.request_encoded_size = recovery_public.size ();
                    reserve.target = {
                      request.target.mesh_name,
                      node_rid_t::from_string (
                        zlink::routing_id_t::from (request.target.target_node_routing_id)
                          .to_string ()),
                      request.target.target_node_generation, instance_owner};
                    const std::string creating = "zlink:instance-spot:creating:v1";
                    for (const auto value : creating)
                        reserve.creating_payload.push_back (
                          static_cast<std::byte> (static_cast<unsigned char> (value)));
                    reserve.capacity_bundle = {
                      0, 1,
                      spot_type_capacity_delta_t{placement_object_kind_t::instance_spot,
                                                 request.target.stable_type, 1}};
                    const auto reserved = store->reserve (reserve).result ().value ();
                    const auto *reservation = std::get_if<object_reserved_t> (&reserved);
                    if (!reservation) {
                        instance_relocations->remove (recovery_root.reference);
                        if (std::holds_alternative<object_type_mismatch_t> (reserved)) {
                            reply_terminal ({107,
                                             static_cast<std::uint32_t> (
                                               protocol::framework_error_code::spotTypeMismatch),
                                             std::nullopt});
                            continue;
                        }
                        if (!join_existing (
                              store->read_authority (authority_key).result ().value ()))
                            reply_terminal ({105,
                                             static_cast<std::uint32_t> (
                                               protocol::framework_error_code::requestFailed),
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
                        (void) store->abort ({reserve.key, reservation->fence}).result ().value ();
                        instance_relocations->remove (recovery_root.reference);
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
                    const auto committed = store
                                             ->commit ({reserve.key, reservation->fence,
                                                        encode_instance_ready_state (ready_state)})
                                             .result ()
                                             .value ();
                    const auto *created = std::get_if<object_committed_t> (&committed);
                    const auto *already = std::get_if<object_already_committed_t> (&committed);
                    if (!created && !already) {
                        reply_terminal (
                          {107,
                           static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                           std::nullopt});
                        continue;
                    }
                    const auto &ready_snapshot = created ? created->ready : already->ready;
                    auto result = instance_materializer.dispatch (request, metadata, application);
                    ready_state.completed = true;
                    ready_state.terminal_result = result.terminal_result;
                    ready_state.failure_code = result.failure_code;
                    ready_state.reply = result.application_reply;
                    const auto stored_terminal =
                      store
                        ->compare_exchange_authority (
                          authority_key, ready_snapshot.store_version,
                          authority_put_t{encode_instance_ready_state (ready_state)})
                        .result ()
                        .value ();
                    const auto *terminal_snapshot =
                      std::get_if<authority_stored_t> (&stored_terminal);
                    if (!terminal_snapshot) {
                        result = {105,
                                  static_cast<std::uint32_t> (
                                    protocol::framework_error_code::requestFailed),
                                  std::nullopt};
                    } else {
                        ready_state.recovery_reference.clear ();
                        ready_state.recovery_checksum = 0;
                        const auto cleared =
                          store
                            ->compare_exchange_authority (
                              authority_key, terminal_snapshot->snapshot.store_version,
                              authority_put_t{encode_instance_ready_state (ready_state)})
                            .result ()
                            .value ();
                        if (std::holds_alternative<authority_stored_t> (cleared))
                            instance_relocations->remove (recovery_root.reference);
                    }
                    reply_terminal (std::move (result));
                    continue;
                }

                if (wire.kind == protocol::command::userSpotCreate) {
                    const auto request =
                      protocol::decode_user_spot_create_header (mailbox_record.parts.front ());
                    auto fingerprint_request = request;
                    fingerprint_request.correlation = 1;
                    const auto request_fingerprint =
                      protocol::encode_user_spot_create_header (fingerprint_request);
                    const auto operation_key =
                      user_spot_operation_key (request.source_node_routing_id,
                                               request.source_node_generation, request.operation);
                    std::optional<user_spot_terminal_record_t> cached;
                    _user_spot_terminal_lane
                      .run ([&] {
                          const auto found = _user_spot_terminals.find (operation_key);
                          if (found != _user_spot_terminals.end ()) {
                              if (user_spot_operation_replay_expired (
                                    found->second.deadline_unix_ms, unix_milliseconds_now (),
                                    _options.user_spot_operation_replay_retention))
                                  _user_spot_terminals.erase (found);
                              else
                                  cached = found->second;
                          }
                      })
                      .get ();
                    if (cached) {
                        if (cached->kind != protocol::command::userSpotCreate
                            || cached->request_fingerprint != request_fingerprint)
                            throw protocol::service_wire_error_t (
                              "user spot operation identity was reused with a different request");
                        auto reply = protocol::decode_user_spot_create_reply (cached->header);
                        reply.header.correlation = request.correlation;
                        (void) _transport->reply_user_spot_create (mailbox_record, reply,
                                                                   cached->application_reply);
                        continue;
                    }
                    if (request.deadline_unix_ms <= unix_milliseconds_now ()) {
                        protocol::user_spot_create_reply_t reply{
                          {request.correlation, 101, 0},
                          protocol::user_spot_create_result_t::rejected,
                          {},
                          0};
                        (void) _transport->reply_user_spot_create (mailbox_record, reply,
                                                                   std::nullopt);
                        continue;
                    }
                    const auto has_terminal_capacity = _user_spot_terminal_lane
                                                         .run ([&] {
                        if (_user_spot_terminals.size () >= _options.user_spot_operation_capacity) {
                            const auto now = unix_milliseconds_now ();
                            std::erase_if (_user_spot_terminals, [this, now] (const auto &entry) {
                                return user_spot_operation_replay_expired (
                                  entry.second.deadline_unix_ms, now,
                                  _options.user_spot_operation_replay_retention);
                            });
                        }
                        return _user_spot_terminals.size ()
                               < _options.user_spot_operation_capacity;
                    })
                                                         .get ();
                    if (!has_terminal_capacity) {
                        //  Spec 32-framework-error-model:99-103 — encode
                        //  local operation-table saturation as Busy(108)+
                        //  None so the requesting peer classifies it as the
                        //  target's queue state (Unavailable). Terminated
                        //  (103) is reserved for actual shutdown.
                        protocol::user_spot_create_reply_t reply{
                          {request.correlation, 108, 0},
                          protocol::user_spot_create_result_t::rejected,
                          {},
                          0};
                        (void) _transport->reply_user_spot_create (mailbox_record, reply,
                                                                   std::nullopt);
                        continue;
                    }
                    auto terminal =
                      [&] (std::uint32_t terminal_result, std::uint32_t failure_code,
                           protocol::user_spot_create_result_t result, const std::string &spot,
                           std::uint64_t generation,
                           std::optional<protocol::application_payload_t> application_reply =
                             std::nullopt) {
                          protocol::user_spot_create_reply_t reply{
                            {request.correlation, terminal_result, failure_code},
                            result,
                            spot,
                            generation};
                          user_spot_terminal_record_t stored{
                            protocol::command::userSpotCreate, request.deadline_unix_ms,
                            request_fingerprint,
                            protocol::encode_user_spot_create_reply (request.correlation,
                                                                     terminal_result, failure_code,
                                                                     result, spot, generation),
                            application_reply};
                          _user_spot_terminal_lane
                            .run ([&] {
                                _user_spot_terminals.insert_or_assign (operation_key,
                                                                       std::move (stored));
                            })
                            .get ();
                          (void) _transport->reply_user_spot_create (mailbox_record, reply,
                                                                     std::move (application_reply));
                      };
                    if (!store || !materializer) {
                        terminal (105,
                                  static_cast<std::uint32_t> (
                                    protocol::framework_error_code::requestFailed),
                                  protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    if (request.deadline_unix_ms <= unix_milliseconds_now ()) {
                        terminal (101, 0, protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    const auto &global_id = request.spot_id;
                    const auto read =
                      store->read_authority (spot_authority_key (global_id)).result ().value ();
                    const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
                    const auto &reservation = request.reservation;
                    const auto exact =
                      snapshot && snapshot->store_version == reservation.expected_store_version
                      && snapshot->object_generation == reservation.object_generation
                      && snapshot->authority_owner_generation
                           == reservation.authority_owner_generation
                      && snapshot->owner.owner_id == reservation.target_owner_id
                      && snapshot->owner.lease_generation
                           == reservation.target_owner_lease_generation
                      && snapshot->allocation.object_kind == placement_object_kind_t::user_spot
                      && snapshot->allocation.stable_type == request.stable_type
                      && snapshot->allocation.target.node_rid.value ()
                           == node_rid_t::from_string (
                                zlink::routing_id_t::from (reservation.target_node_routing_id)
                                  .to_string ())
                                .value ()
                      && snapshot->allocation.target.node_lifecycle_generation
                           == reservation.target_node_generation
                      && snapshot->allocation.capacity_bundle.spot_slots
                           == reservation.pending_capacity_delta;
                    if (!exact) {
                        const auto stale =
                          snapshot && snapshot->object_generation != reservation.object_generation;
                        const auto type_mismatch =
                          snapshot
                          && snapshot->allocation.object_kind == placement_object_kind_t::user_spot
                          && snapshot->allocation.stable_type != request.stable_type;
                        terminal (107,
                                  static_cast<std::uint32_t> (
                                    stale ? protocol::framework_error_code::spotGenerationStale
                                    : type_mismatch
                                      ? protocol::framework_error_code::spotTypeMismatch
                                      : protocol::framework_error_code::spotMoving),
                                  protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    const auto fence = public_fence (
                      reservation, snapshot->allocation.target.mesh_name, request.stable_type);
                    const auto &pending = snapshot->pending_creation;
                    if (!pending || pending->reservation_id != fence.reservation_id) {
                        terminal (
                          107,
                          static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                          protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    const auto creation_payload =
                      runtime::decode_inline_creation_content (pending->request_content_reference);
                    if (!creation_payload
                        || pending->request_encoded_size != creation_payload->size ()
                        || pending->request_sha256 != runtime::sha256 (*creation_payload)) {
                        terminal (105,
                                  static_cast<std::uint32_t> (
                                    protocol::framework_error_code::requestFailed),
                                  protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    const stateful::object_ref_t exact_ref{
                      stateful::object_kind_t::user_spot,
                      request.spot_id,
                      reservation.object_generation,
                      reservation.authority_owner_generation,
                      snapshot->allocation.target.mesh_name,
                      std::string (snapshot->allocation.target.node_rid.value ())};
                    if (snapshot->allocation.state == placement_allocation_state_t::active) {
                        const auto existing =
                          _objects.find (stateful::object_kind_t::user_spot, exact_ref.key);
                        if (!existing || *existing != exact_ref) {
                            terminal (105,
                                      static_cast<std::uint32_t> (
                                        protocol::framework_error_code::requestFailed),
                                      protocol::user_spot_create_result_t::rejected, {}, 0);
                            continue;
                        }
                        terminal (0, 0, protocol::user_spot_create_result_t::existing,
                                  request.spot_id, exact_ref.object_generation);
                        continue;
                    }
                    auto local =
                      _objects.begin_reserved_object (exact_ref, request.stable_type, [&] {
                          std::vector<std::uint8_t> bytes;
                          bytes.reserve (creation_payload->size ());
                          for (const auto value : *creation_payload)
                              bytes.push_back (std::to_integer<std::uint8_t> (value));
                          return bytes;
                      }());
                    if (local.status == stateful::create_status_t::existing) {
                        terminal (0, 0, protocol::user_spot_create_result_t::existing,
                                  request.spot_id, exact_ref.object_generation);
                        continue;
                    }
                    if (!local.factory_owner) {
                        terminal (local.error == stateful::stateful_error_t::generation_stale ? 107
                                                                                              : 108,
                                  static_cast<std::uint32_t> (
                                    local.error == stateful::stateful_error_t::moving
                                      ? protocol::framework_error_code::spotMoving
                                    : local.error == stateful::stateful_error_t::generation_stale
                                      ? protocol::framework_error_code::spotGenerationStale
                                      : protocol::framework_error_code::requestFailed),
                                  protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    user_spot_materialize_result_t materialized;
                    try {
                        materialized =
                          materializer (exact_ref, request.stable_type, *creation_payload);
                    }
                    catch (...) {
                        (void) _objects.abort_create (local.attempt);
                        (void) store
                          ->abort (
                            {{placement_object_kind_t::user_spot, global_id},
                             public_fence (reservation, snapshot->allocation.target.mesh_name,
                                           request.stable_type)})
                          .result ();
                        terminal (105,
                                  static_cast<std::uint32_t> (
                                    protocol::framework_error_code::spotCreateFailed),
                                  protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    if (!materialized.accepted) {
                        (void) store
                          ->abort ({{placement_object_kind_t::user_spot, global_id}, fence})
                          .result ()
                          .value ();
                        (void) _objects.abort_create (local.attempt);
                        terminal (0, 0, protocol::user_spot_create_result_t::rejected,
                                  request.spot_id, exact_ref.object_generation,
                                  std::move (materialized.application_reply));
                        continue;
                    }
                    const auto committed =
                      store
                        ->commit ({{placement_object_kind_t::user_spot, global_id},
                                   fence,
                                   ready_user_spot_authority_payload (
                                     exact_ref, request.stable_type, fence.target)})
                        .result ()
                        .value ();
                    const auto *ready = std::get_if<object_committed_t> (&committed);
                    const auto *already = std::get_if<object_already_committed_t> (&committed);
                    if (!ready && !already) {
                        (void) _objects.abort_create (local.attempt);
                        terminal (
                          107,
                          static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                          protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    auto local_commit = _objects.commit_create (local.attempt);
                    if (local_commit != stateful::stateful_error_t::none) {
                        std::vector<std::uint8_t> creation_bytes;
                        creation_bytes.reserve (creation_payload->size ());
                        for (const auto value : *creation_payload)
                            creation_bytes.push_back (std::to_integer<std::uint8_t> (value));
                        const auto reconciled = _objects.begin_reserved_object (
                          exact_ref, request.stable_type, std::move (creation_bytes));
                        if (reconciled.status == stateful::create_status_t::existing
                            && reconciled.object == exact_ref)
                            local_commit = stateful::stateful_error_t::none;
                        else if ((reconciled.status == stateful::create_status_t::reserved
                                  || reconciled.status == stateful::create_status_t::joined)
                                 && reconciled.attempt != 0)
                            local_commit = _objects.commit_create (reconciled.attempt);
                    }
                    if (local_commit != stateful::stateful_error_t::none) {
                        terminal (105,
                                  static_cast<std::uint32_t> (
                                    protocol::framework_error_code::spotCreateFailed),
                                  protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    _spot_actor_index_lane
                      .run ([&] { _spots.insert_or_assign (exact_ref.key, exact_ref); })
                      .get ();
                    terminal (0, 0, protocol::user_spot_create_result_t::created, request.spot_id,
                              exact_ref.object_generation,
                              std::move (materialized.application_reply));
                    continue;
                }

                const auto request =
                  protocol::decode_user_spot_close_header (mailbox_record.parts.front ());
                auto fingerprint_request = request;
                fingerprint_request.correlation = 1;
                const auto request_fingerprint =
                  protocol::encode_user_spot_close_header (fingerprint_request);
                const auto operation_key =
                  user_spot_operation_key (request.source_node_routing_id,
                                           request.source_node_generation, request.operation);
                std::optional<user_spot_terminal_record_t> cached;
                _user_spot_terminal_lane
                  .run ([&] {
                      const auto found = _user_spot_terminals.find (operation_key);
                      if (found != _user_spot_terminals.end ()) {
                          if (user_spot_operation_replay_expired (
                                found->second.deadline_unix_ms, unix_milliseconds_now (),
                                _options.user_spot_operation_replay_retention))
                              _user_spot_terminals.erase (found);
                          else
                              cached = found->second;
                      }
                  })
                  .get ();
                if (cached) {
                    if (cached->kind != protocol::command::userSpotClose
                        || cached->request_fingerprint != request_fingerprint)
                        throw protocol::service_wire_error_t (
                          "user spot operation identity was reused with a different request");
                    auto reply = protocol::decode_user_spot_close_reply (cached->header);
                    reply.header.correlation = request.correlation;
                    (void) _transport->reply_user_spot_close (mailbox_record, reply);
                    continue;
                }
                if (request.deadline_unix_ms <= unix_milliseconds_now ()) {
                    protocol::user_spot_close_reply_t reply{{request.correlation, 101, 0}, false};
                    (void) _transport->reply_user_spot_close (mailbox_record, reply);
                    continue;
                }
                const auto has_terminal_capacity = _user_spot_terminal_lane
                                                     .run ([&] {
                    if (_user_spot_terminals.size () >= _options.user_spot_operation_capacity) {
                        const auto now = unix_milliseconds_now ();
                        std::erase_if (_user_spot_terminals, [this, now] (const auto &entry) {
                            return user_spot_operation_replay_expired (
                              entry.second.deadline_unix_ms, now,
                              _options.user_spot_operation_replay_retention);
                        });
                    }
                    return _user_spot_terminals.size () < _options.user_spot_operation_capacity;
                })
                                                     .get ();
                if (!has_terminal_capacity) {
                    //  Spec 32-framework-error-model:99-103 — Busy(108)+
                    //  None: the target's operation-table saturation, not a
                    //  shutdown terminal (103).
                    protocol::user_spot_close_reply_t reply{{request.correlation, 108, 0}, false};
                    (void) _transport->reply_user_spot_close (mailbox_record, reply);
                    continue;
                }
                auto terminal = [&] (std::uint32_t terminal_result, std::uint32_t failure_code,
                                     bool closed) {
                    protocol::user_spot_close_reply_t reply{
                      {request.correlation, terminal_result, failure_code}, closed};
                    user_spot_terminal_record_t stored{
                      protocol::command::userSpotClose, request.deadline_unix_ms,
                      request_fingerprint,
                      protocol::encode_user_spot_close_reply (request.correlation, terminal_result,
                                                              failure_code, closed),
                      std::nullopt};
                    _user_spot_terminal_lane
                      .run ([&] {
                          _user_spot_terminals.insert_or_assign (operation_key, std::move (stored));
                      })
                      .get ();
                    (void) _transport->reply_user_spot_close (mailbox_record, reply);
                };
                if (!store) {
                    terminal (
                      105,
                      static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
                      false);
                    continue;
                }
                const auto &global_id = request.target.spot_id;
                const auto read =
                  store->read_authority (spot_authority_key (global_id)).result ().value ();
                const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
                if (!snapshot) {
                    terminal (0, 0, false);
                    continue;
                }
                const auto &target = request.target;
                if (snapshot->object_generation != target.object_generation) {
                    terminal (107,
                              static_cast<std::uint32_t> (
                                protocol::framework_error_code::spotGenerationStale),
                              false);
                    continue;
                }
                if (snapshot->authority_owner_generation != target.authority_owner_generation
                    || snapshot->store_version != target.expected_store_version
                    || snapshot->allocation.object_kind != placement_object_kind_t::user_spot
                    || snapshot->allocation.state != placement_allocation_state_t::active
                    || snapshot->allocation.target.node_rid.value ()
                         != node_rid_t::from_string (
                              zlink::routing_id_t::from (target.target_node_routing_id)
                                .to_string ())
                              .value ()
                    || snapshot->allocation.target.node_lifecycle_generation
                         != target.target_node_generation) {
                    terminal (
                      107, static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                      false);
                    continue;
                }
                const stateful::object_ref_t exact_ref{
                  stateful::object_kind_t::user_spot,
                  target.spot_id,
                  target.object_generation,
                  target.authority_owner_generation,
                  snapshot->allocation.target.mesh_name,
                  std::string (snapshot->allocation.target.node_rid.value ())};
                if (snapshot->payload
                    != ready_user_spot_authority_payload (
                      exact_ref, snapshot->allocation.stable_type, snapshot->allocation.target)) {
                    terminal (
                      107, static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                      false);
                    continue;
                }
                const auto local =
                  _objects.find (stateful::object_kind_t::user_spot, exact_ref.key);
                if (!local || *local != exact_ref) {
                    terminal (
                      107, static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                      false);
                    continue;
                }
                const auto authority_key = spot_authority_key (global_id);
                const auto sealed =
                  store
                    ->compare_exchange_authority (
                      authority_key, snapshot->store_version,
                      authority_put_t{closing_user_spot_authority_payload (
                        exact_ref, snapshot->allocation.stable_type, snapshot->allocation.target)})
                    .result ()
                    .value ();
                const auto *closing = std::get_if<authority_stored_t> (&sealed);
                if (!closing) {
                    terminal (
                      107, static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                      false);
                    continue;
                }
                const auto rollback_closing = [&] {
                    return std::holds_alternative<authority_stored_t> (
                      store
                        ->compare_exchange_authority (authority_key,
                                                      closing->snapshot.store_version,
                                                      authority_put_t{snapshot->payload})
                        .result ()
                        .value ());
                };
                const auto [close_error, close_token] = _objects.begin_close_spot (exact_ref);
                if (close_error == stateful::stateful_error_t::generation_stale) {
                    (void) rollback_closing ();
                    terminal (107,
                              static_cast<std::uint32_t> (
                                protocol::framework_error_code::spotGenerationStale),
                              false);
                    continue;
                }
                if (close_error == stateful::stateful_error_t::moving) {
                    (void) rollback_closing ();
                    terminal (
                      107, static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                      false);
                    continue;
                }
                if (close_error == stateful::stateful_error_t::not_found || !close_token) {
                    (void) rollback_closing ();
                    terminal (
                      107, static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                      false);
                    continue;
                }
                if (_objects.commit_close_spot (*close_token) != stateful::stateful_error_t::none) {
                    (void) rollback_closing ();
                    terminal (
                      105,
                      static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
                      false);
                    continue;
                }
                const auto deleted =
                  store
                    ->compare_exchange_authority (authority_key, closing->snapshot.store_version,
                                                  authority_delete_t{})
                    .result ()
                    .value ();
                if (!std::holds_alternative<authority_deleted_t> (deleted)) {
                    terminal (
                      107, static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                      false);
                    continue;
                }
                _spot_actor_index_lane.run ([&] { _spots.erase (exact_ref.key); }).get ();
                terminal (0, 0, true);
            }
            catch (const protocol::service_wire_error_t &) {
                if (mailbox_record.reply_token && mailbox_record.correlation)
                    (void) _transport->reply_failure (
                      mailbox_record, 104,
                      static_cast<std::uint32_t> (
                        protocol::framework_error_code::requestProtocolError));
            }
            catch (const std::exception &) {
                if (mailbox_record.reply_token && mailbox_record.correlation)
                    (void) _transport->reply_failure (
                      mailbox_record, 105,
                      static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed));
            }
            catch (...) {
                if (mailbox_record.reply_token && mailbox_record.correlation)
                    (void) _transport->reply_failure (
                      mailbox_record, 105,
                      static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed));
            }
        }
        (void) _transport->mailbox ().release (*claim);
        if (infrastructure_budget.exhausted ())
            break;
    }
    co_return count;
}

bool public_host_runtime_t::dispatch_bound_session_send (
  const mesh::service_mailbox_record_t &mailbox_record,
  std::function<void ()> retain_mailbox_reservation,
  std::function<void ()> release_mailbox_reservation)
{
    if (mailbox_record.parts.size () != 2)
        return false;
    const auto record = protocol::decode_bound_session_send (mailbox_record.parts.front ());
    if (mailbox_record.source_routing_id != record.actor.target_node_routing_id
        || mailbox_record.source_node_generation == 0
        || mailbox_record.source_node_generation != record.actor.target_node_generation)
        return false;
    bound_session_operations_t operations;
    _lifecycle_configuration_lane.run ([&] { operations = _bound_session_operations; }).get ();
    if (!operations.capture_send && !operations.send)
        return false;
    const auto application =
      protocol::decode_application_payload (mailbox_record.parts.back (), capture_flow ());
    auto parts = decode_application (application);
    const auto target_node = zlink::routing_id_t::from (record.actor.target_node_routing_id);
    const stateful::stream_remote_tenure_t tenure{record.actor.actor_id,
                                                  record.actor.object_generation,
                                                  record.actor.authority_owner_generation,
                                                  target_node.to_string (),
                                                  record.actor.target_node_generation,
                                                  record.actor.owner_lease_generation,
                                                  record.expected_binding_generation};
    auto proof = _sessions.remote_tenure_proof (
      tenure.actor_id, tenure.binding_generation, tenure.object_generation,
      tenure.authority_owner_generation, tenure.target_node_id, tenure.target_node_generation);
    std::optional<stateful::stream_remote_tenure_proof_t> first_proof;
    const auto current = _sessions.current_binding (tenure.actor_id);
    const auto current_tenure =
      current && current->binding_generation == tenure.binding_generation
      && current->actor.object_generation == tenure.object_generation
      && current->actor.authority_owner_generation == tenure.authority_owner_generation
      && current->actor.node_id == tenure.target_node_id
      && current->target_node_generation == tenure.target_node_generation;
    if (current_tenure && tenure.owner_lease_generation != 0
        && current->owner_lease_generation != tenure.owner_lease_generation) {
        /* OwnerLeaseGeneration is route state, not a push-admission input.
         * Refresh the Session-owned route before delivery so a later command
         * 44 seals the fence actually used by this binding. */
        if (!_sessions.confirm_remote_tenure (tenure))
            return false;
        try {
            if (operations.confirm_remote_tenure)
                (void) operations.confirm_remote_tenure (record);
        }
        catch (...) {
        }
    }
    if (!current_tenure && !proof) {
        if (!current || !retain_mailbox_reservation || !release_mailbox_reservation)
            return false;
        first_proof = stateful::stream_remote_tenure_proof_t{tenure, target_node.to_string ()};
    }
    const auto execute_delivery = [operations,
                                   record] (std::vector<zlink::message_t> admitted_parts) {
        if (operations.capture_send) {
            auto capability = operations.capture_send (record);
            return capability ? (*capability) (std::move (admitted_parts))
                              : stateful::stateful_error_t::conflict;
        }
        return operations.send (record, std::move (admitted_parts));
    };
    auto retained_delivery =
      [execute_delivery, parts,
       release = std::move (release_mailbox_reservation)] (bool should_deliver) mutable {
          if (should_deliver) {
              try {
                  (void) execute_delivery (std::move (parts));
              }
              catch (...) {
              }
          }
          if (release) {
              try {
                  release ();
              }
              catch (...) {
              }
          }
      };
    const auto admitted =
      _sessions.admit_outbound (tenure, std::move (first_proof), std::move (retained_delivery));
    if (admitted.error != stateful::stateful_error_t::none)
        return false;
    if (admitted.kind == stateful::stream_outbound_admission_kind_t::retained) {
        retain_mailbox_reservation ();
        return true;
    }
    return execute_delivery (std::move (parts)) == stateful::stateful_error_t::none;
}

task_t<std::size_t> public_host_runtime_t::dispatch_ready (
  const std::function<void (
    const ready_record_t &, const receive_record_t &, std::vector<zlink::message_t>)> &dispatch,
  bool accept_application_receive)
{
    if (!dispatch) {
        throw std::invalid_argument ("framework public host dispatch callback is required");
    }
    expire_local_spot_requests ();
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    (void) co_await _relocation_wire->retry_terminal_relays (now);
    (void) _relocation_wire->reap_terminal_tombstones (now);
    (void) _transport->tick_liveness (now);
    (void) _transport->drain_monitor_events (now);
    std::size_t count = 0;
    receive_batch_budget_t budget;
    while (budget.can_receive ()) {
        const auto pumped = co_await _transport->pump_one (now, accept_application_receive);
        budget.account (_transport->last_pump_bytes ());
        trace_mesh_host ("pump", std::string ("result=") + pump_result_name (pumped) + " pending="
                                   + std::to_string (_transport->mailbox ().pending_messages (
                                     mesh::service_mailbox_domain_t::application)));
        if (pumped == mesh::raw_mesh_pump_result_t::no_data) {
            break;
        }
        ++count;
        if (pumped == mesh::raw_mesh_pump_result_t::capacity_exceeded) {
            trace_mesh_host (
              "route-control-capacity-exceeded",
              "pending admission capacity was exhausted for an ordinary routed control record");
            break;
        }
        if (pumped == mesh::raw_mesh_pump_result_t::application) {
            // Re-evaluate the host-wide byte budget before starting the next
            // ordinary routed receive. Control and application records share
            // this receive path and are reconsidered on the next pass.
            break;
        }
        if (budget.exhausted ())
            break;
    }
    (void) _transport->expire_requests (foundation::operation_registry_t::clock_t::now ());
    count += co_await dispatch_user_spot_operations ();
    bool application_dispatch_started = false;

    auto completions =
      _local_dispatch_completion_lane.run ([&] { return _completions.take_completed (); }).get ();
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
            _local_dispatch_completion_lane
              .run ([&] {
                if (_local_application_dispatches.empty ())
                    return;
                pending = std::move (_local_application_dispatches.front ());
                _local_application_dispatches.pop_front ();
                if (pending->record.kind == record_kind_t::spot_request) {
                    const auto found = _local_spot_requests.find (pending->record.operation_id);
                    if (found == _local_spot_requests.end ()) {
                        skip = true;
                    } else {
                        found->second.queued = false;
                        if (found->second.terminal_claimed) {
                            _local_spot_requests.erase (found);
                            skip = true;
                        }
                    }
                }
              })
              .get ();
            if (!pending)
                break;
            if (skip)
                continue;
            dispatch (pending->owner, pending->record, std::move (pending->parts));
            ++count;
            application_dispatch_started = true;
            break;
        }
    }

    if (accept_application_receive && !application_dispatch_started && _stateful_dispatch) {
        const auto local_node_id =
          zlink::routing_id_t::from (_transport->topology ().local_descriptor ().node_routing_id)
            .to_string ();
        for (const auto &item : _objects.inventory ()) {
            if (application_dispatch_started)
                break;
            if (item.state != stateful::object_state_t::ready)
                continue;
            if (item.owner.node_id != local_node_id)
                continue;
            for (;;) {
                const auto ingested = _stateful_dispatch->ingest (item.owner);
                if (ingested == stateful::stateful_error_t::not_found)
                    break;
                ++count;
                if (ingested != stateful::stateful_error_t::none)
                    break;
            }
            auto [claim_error, delivery] = _stateful_dispatch->try_claim (item.owner);
            if (claim_error != stateful::stateful_error_t::none || !delivery)
                continue;
            try {
                const auto &frozen = delivery->frozen;
                ready_record_t owner;
                owner.domain = ready_domain_t::application;
                receive_record_t record;
                record.domain = ready_domain_t::application;
                record.source_node_rid = zlink::routing_id_t::from (frozen.source.node_routing_id);
                record.operation_id = call_id_t{frozen.operation.high, frozen.operation.low};
                if (frozen.source_kind == protocol::frozen_source_kind_t::bound_session
                    && frozen.source_session_routing_id) {
                    record.source_session_rid =
                      zlink::routing_id_t::from (*frozen.source_session_routing_id);
                    record.source_binding_generation = frozen.source_binding_generation;
                    record.source_session_sequence = frozen.source_session_sequence;
                }
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
                record.operation_kind = operation_kind (record.kind);
                if (owner.owner_kind == owner_kind_t::actor) {
                    owner.actor = framework_actor_ref (item.owner, item.stable_type);
                } else {
                    owner.spot_id = item.owner.key;
                }
                auto completed = std::make_shared<std::atomic_bool> (false);
                record.complete_stateful_dispatch = [weak = weak_from_this (), delivery = *delivery,
                                                     completed] {
                    if (!completed->exchange (true, std::memory_order_acq_rel)) {
                        if (const auto host = weak.lock ()) {
                            auto pending = std::make_shared<task_t<stateful::stateful_error_t>> (
                              host->_stateful_dispatch->complete_async (delivery, std::nullopt));
                            detail::observe_task_completion (
                              *pending,
                              [pending] (const result_t<stateful::stateful_error_t> &) {});
                        }
                    }
                };
                if (delivery->request) {
                    record.reply_token.host = weak_from_this ();
                    record.reply_token.local_reply =
                      [weak = weak_from_this (), delivery = *delivery,
                       completed] (const std::vector<zlink::message_t> &parts) {
                          if (completed->exchange (true, std::memory_order_acq_rel))
                              return false;
                          const auto host = weak.lock ();
                          if (!host)
                              return false;
                          try {
                              auto pending = std::make_shared<task_t<stateful::stateful_error_t>> (
                                host->_stateful_dispatch->complete_async (
                                  delivery, host->encode_application (parts)));
                              detail::observe_task_completion (
                                *pending,
                                [pending] (const result_t<stateful::stateful_error_t> &) {});
                              return true;
                          }
                          catch (...) {
                              return false;
                          }
                      };
                }
                dispatch (owner, record, decode_application (delivery->payload));
                ++count;
                application_dispatch_started = true;
            }
            catch (const std::exception &) {
                try {
                    auto pending = std::make_shared<task_t<stateful::stateful_error_t>> (
                      _stateful_dispatch->complete_async (*delivery, std::nullopt));
                    detail::observe_task_completion (
                      *pending, [pending] (const result_t<stateful::stateful_error_t> &) {});
                }
                catch (...) {
                }
            }
            catch (...) {
                try {
                    auto pending = std::make_shared<task_t<stateful::stateful_error_t>> (
                      _stateful_dispatch->complete_async (*delivery, std::nullopt));
                    detail::observe_task_completion (
                      *pending, [pending] (const result_t<stateful::stateful_error_t> &) {});
                }
                catch (...) {
                }
            }
        }
    }

    while (accept_application_receive && !application_dispatch_started) {
        auto claim =
          _transport->mailbox ().try_claim (mesh::service_mailbox_domain_t::application, 1,
                                            dispatch_limits::application_mailbox_bytes);
        if (!claim)
            break;
        trace_mesh_host ("mailbox-claim",
                         std::string ("records=") + std::to_string (claim->records.size ()));
        auto claim_holder = std::make_shared<mesh::service_mailbox_claim_t> (std::move (*claim));
        auto claim_released = std::make_shared<std::atomic_bool> (false);
        auto claim_retained = std::make_shared<std::atomic_bool> (false);
        const auto retain_mailbox_reservation = [claim_retained] {
            claim_retained->store (true, std::memory_order_release);
        };
        const auto release_mailbox_reservation = [weak = weak_from_this (), claim_holder,
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
                const auto wire = protocol::decode_header (mailbox_record.parts.front ());
                if (wire.kind == protocol::command::boundSessionSend) {
                    (void) dispatch_bound_session_send (mailbox_record, retain_mailbox_reservation,
                                                        release_mailbox_reservation);
                    ++count;
                    application_dispatch_started = true;
                    continue;
                }
                const auto kind = record_kind (wire.kind);
                ready_record_t owner;
                owner.domain = ready_domain_t::application;
                receive_record_t record;
                record.kind = kind;
                record.domain = ready_domain_t::application;
                record.operation_kind = operation_kind (kind);
                record.source_node_rid =
                  zlink::routing_id_t::from (mailbox_record.source_routing_id);
                if (mailbox_record.operation) {
                    record.operation_id = {mailbox_record.operation->first,
                                           mailbox_record.operation->second};
                } else if (mailbox_record.correlation) {
                    record.operation_id = {status ().lifecycle_generation (),
                                           *mailbox_record.correlation};
                }
                if (is_request (kind)) {
                    record.reply_token = {
                      weak_from_this (),
                      std::make_shared<mesh::service_mailbox_record_t> (mailbox_record)};
                }
                if (kind == record_kind_t::channel_send || kind == record_kind_t::channel_request) {
                    owner.owner_kind = owner_kind_t::channel;
                    owner.channel_name =
                      kind == record_kind_t::channel_send
                        ? protocol::decode_channel_send_header (mailbox_record.parts.front ())
                        : protocol::decode_channel_request_header (mailbox_record.parts.front ())
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
                    const auto actor = protocol::decode_actor_message_header (
                      mailbox_record.parts.front (), wire.kind);
                    record.actor_route = actor.target;
                    record.message_follow_hop_count = actor.message_follow_hop_count;
                    record.reply_route_id = actor.correlation.value_or (0);
                    if (mailbox_record.bound_session_source) {
                        record.source_session_rid = zlink::routing_id_t::from (
                          mailbox_record.bound_session_source->session_routing_id);
                        record.source_binding_generation =
                          mailbox_record.bound_session_source->binding_generation;
                        record.source_session_sequence =
                          mailbox_record.bound_session_source->session_sequence;
                    }
                    const auto actor_type = _spot_actor_index_lane
                      .run ([&] {
                        std::string actor_type;
                        const auto found = _actors.find (actor.target.actor_id);
                        if (found != _actors.end ()) {
                            actor_type = found->second.first;
                        }
                        return actor_type;
                      })
                      .get ();
                    owner.actor = ::zlink::framework::detail::actor_ref_access_t::make (
                      node_rid_t::from_string (status ().routing_id ().to_string ()),
                      std::move (actor_type), actor.target.actor_id,
                      actor.target.object_generation);
                } else {
                    owner.owner_kind = owner_kind_t::node;
                }
                const auto payload =
                  protocol::decode_application_payload (mailbox_record.parts[1], capture_flow ());
                auto source = std::string ("-");
                if (!mailbox_record.source_routing_id.empty ()) {
                    source =
                      zlink::routing_id_t::from (mailbox_record.source_routing_id).to_string ();
                }
                trace_mesh_host ("dispatch",
                                 std::string ("kind=") + std::to_string (static_cast<int> (kind))
                                   + " source=" + source
                                   + " parts=" + std::to_string (mailbox_record.parts.size ()));
                record.release_mailbox_reservation = release_mailbox_reservation;
                record.retain_mailbox_reservation = retain_mailbox_reservation;
                record.transferred_owner_byte_cost = claim_holder->claimed_bytes;
                dispatch (owner, record, decode_application (payload));
                ++count;
                application_dispatch_started = true;
            }
            catch (const protocol::service_wire_error_t &) {
                if (mailbox_record.reply_token && mailbox_record.correlation) {
                    (void) _transport->reply_failure (
                      mailbox_record, 104,
                      static_cast<std::uint32_t> (
                        protocol::framework_error_code::requestProtocolError));
                }
                release_mailbox_reservation ();
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
    co_return count;
}

bool public_host_runtime_t::wait_for_dispatch_activity (std::chrono::milliseconds timeout,
                                                        bool accept_application_receive) noexcept
{
    try {
        const auto local_deadline = next_local_spot_request_deadline ();
        auto effective_timeout = timeout;
        if (accept_application_receive) {
            if (_local_dispatch_completion_lane
                  .run ([&] { return !_local_application_dispatches.empty (); })
                  .get ())
                return true;
        }
        if (local_deadline) {
            const auto now = std::chrono::steady_clock::now ();
            if (*local_deadline <= now)
                return true;
            const auto remaining =
              std::chrono::duration_cast<std::chrono::milliseconds> (*local_deadline - now);
            if (remaining <= std::chrono::milliseconds::zero ())
                return true;
            if (effective_timeout <= std::chrono::milliseconds::zero ()
                || remaining < effective_timeout) {
                effective_timeout = remaining;
            }
        }
        return _transport->wait_for_activity (effective_timeout, accept_application_receive);
    }
    catch (...) {
        return false;
    }
}

void public_host_runtime_t::signal_dispatch_activity () noexcept
{
    _transport->signal_activity ();
}

bool public_host_runtime_t::prepare_actor_transfer (const actor_transfer_prepare_t &prepare,
                                                    actor_transfer_token_t &token,
                                                    actor_transfer_prepare_result_t &result)
{
    auto actor = resolve_actor (prepare.actor);
    if (!actor) {
        return false;
    }
    stateful::stateful_error_t error = stateful::stateful_error_t::invalid;
    stateful::membership_token_t membership;
    if (prepare.role == actor_transfer_role_t::source) {
        std::tie (error, membership) = _objects.begin_remote_membership_move (
          *actor, stateful::object_ref_t{
                    stateful::object_kind_t::user_spot, prepare.target_spot_id,
                    prepare.target_spot_generation, prepare.target_spot_generation,
                    _options.mesh.descriptor.mesh_name, prepare.target_node_rid.to_string ()});
    } else {
        auto target = resolve_spot (prepare.target_spot_id);
        if (!target)
            return false;
        std::tie (error, membership) = _objects.begin_membership_move (*actor, *target);
    }
    if (error != stateful::stateful_error_t::none) {
        return false;
    }
    token._host = shared_from_this ();
    token._membership = membership;
    token._role = prepare.role;
    token._membership_epoch = 0;
    token._terminal = false;
    result.current_actor = framework_actor_ref (
      *actor,
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (prepare.actor)));
    result.membership_epoch = actor->authority_owner_generation;
    return true;
}

bool public_host_runtime_t::reply (const reply_token_t &token,
                                   const std::vector<zlink::message_t> &parts)
{
    try {
        if (token.local_reply) {
            return token.local_reply (parts);
        }
        return token.request && _transport->reply (*token.request, encode_application (parts));
    }
    catch (const zlink::submit_error_t &) {
        return false;
    }
}

std::optional<stateful::object_ref_t>
public_host_runtime_t::resolve_actor (const actor_ref_t &actor) const
{
    return _spot_actor_index_lane
      .run ([&] () -> std::optional<stateful::object_ref_t> {
          const auto found = _actors.find (std::string (actor.actor_id ().value ()));
          if (found == _actors.end ()
              || found->second.second.object_generation != actor.object_generation ()) {
              return std::nullopt;
          }
          return found->second.second;
      })
      .get ();
}

std::optional<stateful::object_ref_t>
public_host_runtime_t::resolve_spot (const std::string &spot_id) const
{
    return _spot_actor_index_lane
      .run ([&] {
          const auto found = _spots.find (spot_id);
          return found == _spots.end () ? std::optional<stateful::object_ref_t>{}
                                        : std::make_optional (found->second);
      })
      .get ();
}

protocol::application_payload_t
public_host_runtime_t::encode_application (const std::vector<zlink::message_t> &parts,
                                           std::span<const std::uint8_t>) const
{
    return {std::string (multipart_packet_name), std::string (multipart_content_type),
            encode_parts (parts)};
}

std::vector<zlink::message_t>
public_host_runtime_t::decode_application (const protocol::application_payload_t &payload) const
{
    if (payload.packet_name != multipart_packet_name
        || payload.content_type != multipart_content_type) {
        throw protocol::service_wire_error_t (
          "framework application payload profile is unsupported");
    }
    return decode_parts (payload.payload);
}

actor_ref_t public_host_runtime_t::framework_actor_ref (const stateful::object_ref_t &object,
                                                        std::string actor_type) const
{
    return ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (object.node_id), std::move (actor_type), object.key,
      object.object_generation);
}

call_id_t public_host_runtime_t::next_operation ()
{
    const auto low = _next_operation.fetch_add (1, std::memory_order_relaxed);
    if (low == 0) {
        throw std::overflow_error ("framework public host operation id is exhausted");
    }
    return {status ().lifecycle_generation (), low};
}

bool public_host_runtime_t::try_reserve_completion (call_id_t operation)
{
    return _local_dispatch_completion_lane.run ([&] { return _completions.reserve (operation); })
      .get ();
}

void public_host_runtime_t::release_completion (call_id_t operation) noexcept
{
    (void) _local_dispatch_completion_lane.run ([&] { (void) _completions.erase (operation); }).get ();
}

bool public_host_runtime_t::enqueue_completion (call_id_t operation,
                                                receive_record_t record,
                                                std::vector<zlink::message_t> parts)
{
    return _lifecycle_configuration_lane
      .run ([&] {
          if (!_started || _closing) {
              (void) _local_dispatch_completion_lane
                .run ([&] { (void) _completions.erase (operation); })
                .get ();
              return false;
          }
          try {
              return _local_dispatch_completion_lane
                .run ([&] {
                  return _completions.complete (
                    operation, std::make_pair (std::move (record), std::move (parts)));
                })
                .get ();
          }
          catch (...) {
              (void) _local_dispatch_completion_lane
                .run ([&] { (void) _completions.erase (operation); })
                .get ();
              return false;
          }
      })
      .get ();
}

zlink::submit_result_t
public_host_runtime_t::begin_local_actor_join (const actor_ref_t &actor,
                                               const std::string &target_spot_id,
                                               std::uint64_t target_spot_generation,
                                               const std::vector<zlink::message_t> &parts,
                                               call_id_t &operation)
{
    operation = next_operation ();
    if (!try_reserve_completion (operation))
        return zlink::submit_result_t::backpressured;
    const auto current = resolve_actor (actor);
    const auto target = resolve_spot (target_spot_id);
    //  Spec 32-framework-error-model:129-136 — typed Rejected is reserved for
    //  the application callback decision. A Framework prerequisite failure
    //  carries a classified wire terminal on the completion instead of a
    //  synthesized rejection, and the consumer maps it to the public kind.
    auto fail = [&] (std::uint32_t terminal_result, std::uint32_t failure_errno) {
        receive_record_t completion;
        completion.kind = record_kind_t::completion;
        completion.domain = ready_domain_t::infrastructure;
        completion.operation_id = operation;
        completion.operation_kind = operation_kind_t::actor_join;
        completion.source_node_rid = status ().routing_id ();
        completion.terminal_result = static_cast<int> (terminal_result);
        completion.failure_errno = static_cast<int> (failure_errno);
        (void) enqueue_completion (operation, std::move (completion), {});
    };
    if (!current || !target) {
        fail (102, 0); //  notFound: the join source or target doesn't exist.
        return zlink::submit_result_t::ok;
    }
    if (target->object_generation != target_spot_generation) {
        fail (107, 33); //  spotGenerationStale -> InvalidOperation.
        return zlink::submit_result_t::ok;
    }
    auto [error, membership] = _objects.begin_membership_move (*current, *target);
    if (error != stateful::stateful_error_t::none) {
        const auto classified =
          [] (stateful::stateful_error_t failure) -> std::pair<std::uint32_t, std::uint32_t> {
            switch (failure) {
                case stateful::stateful_error_t::not_found:
                    return {102, 0};
                case stateful::stateful_error_t::type_mismatch:
                    return {107, 4};
                case stateful::stateful_error_t::already_exists:
                    return {107, 3};
                case stateful::stateful_error_t::generation_stale:
                    return {107, 33};
                case stateful::stateful_error_t::moving:
                    return {107, 34};
                case stateful::stateful_error_t::conflict:
                    //  Source-local conflict (an active application turn or a
                    //  not-ready local object) is an operation forbidden in the
                    //  current state -> InvalidOperation (spec 32:41), not the
                    //  remote-owner Unavailable a bare conflict terminal maps
                    //  to (spec 32:99-103).
                    return {111, 0};
                case stateful::stateful_error_t::backpressured:
                    return {113, 0};
                case stateful::stateful_error_t::invalid:
                case stateful::stateful_error_t::instance_manager_create_forbidden:
                    return {111, 0};
                default:
                    return {105, 0};
            }
        }(error);
        fail (classified.first, classified.second);
        return zlink::submit_result_t::ok;
    }

    const auto actor_type =
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor));
    std::weak_ptr<public_host_runtime_t> weak = shared_from_this ();
    ready_record_t owner{.owner_kind = owner_kind_t::spot,
                         .domain = ready_domain_t::application,
                         .spot_id = target_spot_id};
    receive_record_t record;
    record.kind = record_kind_t::spot_control;
    record.domain = ready_domain_t::application;
    record.operation_id = operation;
    record.operation_kind = operation_kind_t::actor_join;
    record.source_node_rid = status ().routing_id ();
    record.actor_control = actor_control_t{lifecycle_kind_t::joined, actor};
    record.reply_token.local_actor_join =
      [weak, operation, actor_type, membership] (actor_join_result_t result,
                                                 const std::vector<zlink::message_t> &reply) {
          const auto host = weak.lock ();
          return host
                 && host->complete_local_actor_join (operation, actor_type, membership, result,
                                                     reply);
      };
    _local_dispatch_completion_lane
      .run ([&] {
        _local_application_dispatches.push_back (
          local_application_dispatch_t{std::move (owner), std::move (record), parts});
      })
      .get ();
    _transport->signal_activity ();
    return zlink::submit_result_t::ok;
}

bool public_host_runtime_t::complete_local_actor_join (call_id_t operation,
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
        auto [error, current] = _objects.commit_membership_move (membership);
        if (error != stateful::stateful_error_t::none) {
            //  Spec 32-framework-error-model:119-120 — a Framework execution
            //  failure after the application ACCEPTED the join is not an
            //  application rejection; carry a classified terminal
            //  (internalError) instead of synthesizing typed Rejected.
            completion.terminal_result = 105;
            completion.failure_errno = 0;
        } else {
            const auto actor = framework_actor_ref (current, actor_type);
            _spot_actor_index_lane
              .run ([&] {
                const auto found = _actors.find (current.key);
                if (found != _actors.end ())
                    found->second.second = current;
              })
              .get ();
            completion.join_completion = actor_join_completion_t{join_admission_t::accepted, actor};
        }
    } else {
        (void) _objects.abort_membership_move (membership);
        completion.join_completion = actor_join_completion_t{
          join_admission_t::rejected, framework_actor_ref (membership.actor, actor_type)};
    }

    return enqueue_completion (operation, std::move (completion), parts);
}

zlink::submit_result_t public_host_runtime_t::enqueue_local_actor_message (
  const actor_ref_t &target,
  record_kind_t kind,
  const std::vector<zlink::message_t> &parts,
  std::optional<call_id_t> operation,
  std::optional<protocol::actor_message_header_t::bound_session_source_t> bound_session_source)
{
    if (kind != record_kind_t::actor_send && kind != record_kind_t::actor_request) {
        return zlink::submit_result_t::invalid_argument;
    }
    const auto current = _objects.find (stateful::object_kind_t::actor,
                                        std::string (target.actor_id ().value ()));
    if (!current) {
        return zlink::submit_result_t::not_found;
    }

    ready_record_t owner{
      .owner_kind = owner_kind_t::actor,
      .domain = ready_domain_t::application,
      .actor = framework_actor_ref (
        *current,
        std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (target)))};
    receive_record_t record;
    record.kind = kind;
    record.domain = ready_domain_t::application;
    record.source_node_rid = status ().routing_id ();
    if (bound_session_source) {
        record.source_session_rid =
          zlink::routing_id_t::from (bound_session_source->session_routing_id);
        record.source_binding_generation = bound_session_source->binding_generation;
        record.source_session_sequence = bound_session_source->session_sequence;
    }
    if (operation) {
        record.operation_id = *operation;
        std::weak_ptr<public_host_runtime_t> weak = shared_from_this ();
        record.reply_token.host = weak;
        record.reply_token.local_reply =
          [weak, operation = *operation] (const std::vector<zlink::message_t> &reply) {
              const auto host = weak.lock ();
              return host && host->complete_local_request (operation, reply);
          };
    }
    const auto submitted = _lifecycle_configuration_lane
      .run ([&] {
          if (!_started || _closing) {
              return zlink::submit_result_t::terminated;
          }
          return _local_dispatch_completion_lane
            .run ([&] {
              _local_application_dispatches.push_back (
                local_application_dispatch_t{std::move (owner), std::move (record), parts});
              return zlink::submit_result_t::ok;
            })
            .get ();
      })
      .get ();
    if (submitted == zlink::submit_result_t::ok)
        _transport->signal_activity ();
    return submitted;
}

zlink::submit_result_t
public_host_runtime_t::enqueue_local_spot_send (const protocol::spot_route_fence_t &target,
                                                const std::vector<zlink::message_t> &parts)
{
    const auto local = status ();
    if (target.target_node_routing_id != local.routing_id ().to_bytes ()
        || target.target_node_generation != local.lifecycle_generation ()) {
        return zlink::submit_result_t::not_found;
    }
    const auto object = resolve_spot (target.spot_id);
    if (!object || object->node_id != local.routing_id ().to_string ()) {
        return zlink::submit_result_t::not_found;
    }

    ready_record_t owner{.owner_kind = owner_kind_t::spot,
                         .domain = ready_domain_t::application,
                         .spot_id = target.spot_id};
    receive_record_t record;
    record.kind = record_kind_t::spot_send;
    record.domain = ready_domain_t::application;
    record.source_node_rid = local.routing_id ();
    record.spot_route = target;

    const auto submitted = _lifecycle_configuration_lane
      .run ([&] {
          if (!_started || _closing) {
              return zlink::submit_result_t::terminated;
          }
          return _local_dispatch_completion_lane
            .run ([&] {
              _local_application_dispatches.push_back (
                local_application_dispatch_t{std::move (owner), std::move (record), parts});
              return zlink::submit_result_t::ok;
            })
            .get ();
      })
      .get ();
    if (submitted == zlink::submit_result_t::ok)
        _transport->signal_activity ();
    return submitted;
}

zlink::submit_result_t
public_host_runtime_t::enqueue_local_spot_request (const protocol::spot_route_fence_t &target,
                                                   const std::vector<zlink::message_t> &parts,
                                                   call_id_t operation,
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
    if (!object || object->node_id != local.routing_id ().to_string ()) {
        return zlink::submit_result_t::not_found;
    }

    ready_record_t owner{.owner_kind = owner_kind_t::spot,
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
    record.reply_token.local_reply = [weak,
                                      operation] (const std::vector<zlink::message_t> &reply) {
        const auto host = weak.lock ();
        if (!host) {
            return false;
        }
        return host->finish_local_spot_request (
          operation, foundation::operation_terminal_t::completed,
          result_t<std::vector<zlink::message_t>>::success (reply));
    };

    const auto deadline = std::chrono::steady_clock::now () + timeout;
    const auto submitted = _lifecycle_configuration_lane
      .run ([&] {
          if (!_started || _closing) {
              return zlink::submit_result_t::terminated;
          }
          return _local_dispatch_completion_lane
            .run ([&] {
              auto [found, inserted] = _local_spot_requests.emplace (
                operation,
                local_spot_request_state_t{deadline, std::move (completion), {}, true, false});
              if (!inserted) {
                  return zlink::submit_result_t::internal_error;
              }
              bool deadline_indexed = false;
              try {
                  auto deadline_entry = _local_spot_request_deadlines.emplace (deadline, operation);
                  deadline_indexed = true;
                  found->second.deadline_index = deadline_entry;
                  _local_application_dispatches.push_back (
                    local_application_dispatch_t{std::move (owner), std::move (record), parts});
              }
              catch (...) {
                  if (deadline_indexed)
                      _local_spot_request_deadlines.erase (found->second.deadline_index);
                  _local_spot_requests.erase (found);
                  throw;
              }
              return zlink::submit_result_t::ok;
            })
            .get ();
      })
      .get ();
    if (submitted == zlink::submit_result_t::ok)
        _transport->signal_activity ();
    return submitted;
}

bool public_host_runtime_t::finish_local_spot_request (
  call_id_t operation,
  foundation::operation_terminal_t terminal,
  result_t<std::vector<zlink::message_t>> result) noexcept
{
    local_spot_request_state_t pending{};
    try {
        const auto claimed = _local_dispatch_completion_lane
          .run ([&] {
            const auto found = _local_spot_requests.find (operation);
            if (found == _local_spot_requests.end () || found->second.terminal_claimed) {
                return false;
            }
            pending.deadline = found->second.deadline;
            pending.completion = std::move (found->second.completion);
            pending.queued = found->second.queued;
            pending.terminal_claimed = true;
            _local_spot_request_deadlines.erase (found->second.deadline_index);
            found->second.terminal_claimed = true;
            if (!found->second.queued) {
                _local_spot_requests.erase (found);
            }
            return true;
          })
          .get ();
        if (!claimed)
            return false;

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

        complete_operation (operation, operation_kind_t::none,
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
            std::optional<call_id_t> expired;
            expired = _local_dispatch_completion_lane
              .run ([&] () -> std::optional<call_id_t> {
                const auto now = std::chrono::steady_clock::now ();
                if (!_local_spot_request_deadlines.empty ()
                    && _local_spot_request_deadlines.begin ()->first <= now) {
                    return _local_spot_request_deadlines.begin ()->second;
                }
                return std::nullopt;
              })
              .get ();
            if (!expired) {
                return;
            }
            (void) finish_local_spot_request (*expired, foundation::operation_terminal_t::timed_out,
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
            std::optional<call_id_t> pending_operation;
            pending_operation = _local_dispatch_completion_lane
              .run ([&] () -> std::optional<call_id_t> {
                for (const auto &[operation, pending] : _local_spot_requests) {
                    if (!pending.terminal_claimed) {
                        return operation;
                    }
                }
                return std::nullopt;
              })
              .get ();
            if (!pending_operation)
                return;
            const auto error_kind = terminal == foundation::operation_terminal_t::shutdown
                                      ? framework_error_kind_t::shutting_down
                                      : framework_error_kind_t::internal_failure;
            (void) finish_local_spot_request (
              *pending_operation, terminal,
              result_t<std::vector<zlink::message_t>>::failure (
                error_kind, "SPOT request stopped because the runtime is shutting down"));
        }
    }
    catch (...) {
    }
}

std::optional<std::chrono::steady_clock::time_point>
public_host_runtime_t::next_local_spot_request_deadline () const
{
    return _local_dispatch_completion_lane
      .run ([&] () -> std::optional<std::chrono::steady_clock::time_point> {
        if (_local_spot_request_deadlines.empty ())
            return std::nullopt;
        return _local_spot_request_deadlines.begin ()->first;
      })
      .get ();
}

bool public_host_runtime_t::complete_local_request (call_id_t operation,
                                                    const std::vector<zlink::message_t> &parts)
{
    receive_record_t completion;
    completion.kind = record_kind_t::completion;
    completion.domain = ready_domain_t::infrastructure;
    completion.operation_id = operation;
    completion.source_node_rid = status ().routing_id ();
    return enqueue_completion (operation, std::move (completion), parts);
}

void public_host_runtime_t::complete_operation (call_id_t operation,
                                                operation_kind_t kind,
                                                foundation::operation_terminal_t terminal,
                                                std::vector<std::uint8_t> payload)
{
    try {
        const auto stopped = _lifecycle_configuration_lane
                               .run ([&] { return !_started || _closing; })
                               .get ();
        if (stopped) {
            release_completion (operation);
            return;
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
                record.terminal_result = static_cast<int> (zlink::request_result_t::timed_out);
                break;
            case foundation::operation_terminal_t::shutdown:
                record.terminal_result = static_cast<int> (zlink::request_result_t::terminated);
                break;
            default:
                record.terminal_result = static_cast<int> (zlink::request_result_t::internal_error);
                break;
        }
        std::vector<zlink::message_t> parts;
        if (record.terminal_result == 0) {
            try {
                parts = decode_application (
                  protocol::decode_application_payload (payload, capture_flow ()));
            }
            catch (const protocol::service_wire_error_t &) {
                record.terminal_result = static_cast<int> (zlink::request_result_t::protocol_error);
            }
        } else if (!payload.empty ()) {
            try {
                const auto reply = protocol::decode_reply_header (payload);
                record.terminal_result = static_cast<int> (reply.terminal_result);
                record.failure_errno = static_cast<int> (reply.failure_code);
            }
            catch (const protocol::service_wire_error_t &) {
                record.terminal_result = static_cast<int> (zlink::request_result_t::protocol_error);
                record.failure_errno = 0;
            }
        }
        if (!enqueue_completion (operation, std::move (record), std::move (parts)))
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

bool actor_transfer_token_t::commit (std::uint64_t membership_epoch)
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
    const auto [error, current] = host->objects ().commit_membership_move (_membership);
    _terminal = true;
    if (error == stateful::stateful_error_t::none) {
        host->_spot_actor_index_lane
          .run ([&] {
              const auto found = host->_actors.find (_membership.actor.key);
              if (found != host->_actors.end ())
                  found->second.second = current;
          })
          .get ();
    }
    return error == stateful::stateful_error_t::none;
}

bool actor_transfer_token_t::activate ()
{
    auto host = _host.lock ();
    if (!host || _terminal || _role != actor_transfer_role_t::target || _membership_epoch == 0)
        return false;
    const auto [error, current] = host->objects ().commit_membership_move (_membership);
    _terminal = true;
    if (error == stateful::stateful_error_t::none) {
        host->_spot_actor_index_lane
          .run ([&] {
              const auto found = host->_actors.find (_membership.actor.key);
              if (found != host->_actors.end ())
                  found->second.second = current;
          })
          .get ();
    }
    return error == stateful::stateful_error_t::none;
}

void actor_transfer_token_t::abort () noexcept
{
    if (auto host = _host.lock (); host && !_terminal) {
        (void) host->objects ().abort_membership_move (_membership);
    }
    _terminal = true;
}

zlink::submit_result_t reply (const reply_token_t &token,
                              const std::vector<zlink::message_t> &parts)
{
    const auto host = token.host.lock ();
    return host && host->reply (token, parts) ? zlink::submit_result_t::ok
                                              : zlink::submit_result_t::terminated;
}

bool actor_join_reply (const reply_token_t &token,
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
              static_cast<std::uint32_t> (protocol::framework_error_code::requestRejected));
        }
        catch (const zlink::submit_error_t &) {
            return false;
        }
    }
    return reply (token, parts) == zlink::submit_result_t::ok;
}

} // namespace zlink::framework::runtime::host
