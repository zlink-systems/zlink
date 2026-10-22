/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/execution/task_result.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/mesh_trace.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/session_binding_diagnostics.hpp"

#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/mesh/service_channel_membership.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include <zlink/framework/contracts/detail/handler_invocation.hpp>
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/spots/activation_admission.hpp"
#include "runtime/locations/pending_creation_projection.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/request_deadline.hpp"

#include <service_wire_constants.hpp>
#include <service_wire_pilot_codec.hpp>
#include <service_wire_codec.hpp>

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

namespace
{
template <typename Peer> bool peer_route_ready (const Peer &peer, std::uint64_t generation)
{
    return peer && peer->ready && peer->descriptor.lifecycle_generation == generation;
}

std::pair<std::uint32_t, std::uint32_t> stateful_failure_pair (stateful::stateful_error_t failure)
{
    switch (failure) {
        case stateful::stateful_error_t::not_found:
            return {static_cast<std::uint32_t> (protocol::request_terminal_result::notFound),
                    static_cast<std::uint32_t> (protocol::framework_error_code::none)};
        case stateful::stateful_error_t::type_mismatch:
            return {static_cast<std::uint32_t> (protocol::request_terminal_result::conflict),
                    static_cast<std::uint32_t> (protocol::framework_error_code::actorTypeMismatch)};
        case stateful::stateful_error_t::already_exists:
            return {
              static_cast<std::uint32_t> (protocol::request_terminal_result::conflict),
              static_cast<std::uint32_t> (protocol::framework_error_code::actorAlreadyExists)};
        case stateful::stateful_error_t::generation_stale:
            return {
              static_cast<std::uint32_t> (protocol::request_terminal_result::conflict),
              static_cast<std::uint32_t> (protocol::framework_error_code::spotGenerationStale)};
        case stateful::stateful_error_t::moving:
            return {static_cast<std::uint32_t> (protocol::request_terminal_result::conflict),
                    static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving)};
        case stateful::stateful_error_t::none:
            return {static_cast<std::uint32_t> (protocol::request_terminal_result::internalError),
                    static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed)};
        case stateful::stateful_error_t::conflict:
            // Source-local conflicts are InvalidOperation (error model §2, §5).
            // A remote owner's unavailable result is classified at its boundary.
            return {static_cast<std::uint32_t> (protocol::request_terminal_result::invalidState),
                    static_cast<std::uint32_t> (protocol::framework_error_code::none)};
        case stateful::stateful_error_t::backpressured:
            return {static_cast<std::uint32_t> (protocol::request_terminal_result::backpressured),
                    static_cast<std::uint32_t> (protocol::framework_error_code::none)};
        case stateful::stateful_error_t::invalid:
        case stateful::stateful_error_t::instance_manager_create_forbidden:
            return {static_cast<std::uint32_t> (protocol::request_terminal_result::invalidState),
                    static_cast<std::uint32_t> (protocol::framework_error_code::none)};
        default:
            return {static_cast<std::uint32_t> (protocol::request_terminal_result::internalError),
                    static_cast<std::uint32_t> (protocol::framework_error_code::none)};
    }
}

void report_stateful_dispatch_error (
  const dispatch_options_t &options,
  const stateful::stateful_delivery_t &delivery,
  std::exception_ptr error,
  const runtime::serial_application_record_t *accepted = nullptr) noexcept
{
    detail::dispatch_error_reporter_t (options).report_lazy ([&] {
        const bool actor = delivery.owner.kind == stateful::object_kind_t::actor;
        message_dispatch_error_event_t event{
          .surface =
            actor ? dispatch_error_surface_t::spot_actor : dispatch_error_surface_t::spot_route,
          .message_kind = actor ? (delivery.request ? dispatch_message_kind_t::actor_request
                                                    : dispatch_message_kind_t::actor_send)
                                : (delivery.request ? dispatch_message_kind_t::request
                                                    : dispatch_message_kind_t::send),
          .reason = dispatch_error_reason_t::handler_exception,
          .action =
            delivery.request ? dispatch_error_action_t::reply_error : dispatch_error_action_t::drop,
          .source_rid =
            zlink::routing_id_t::from (delivery.frozen.source.node_routing_id).to_string (),
          .exception = std::move (error)};
        if (actor)
            event.actor_id = delivery.owner.key;
        else
            event.spot_id = delivery.owner.key;
        try {
            const auto *identity = accepted ? accepted->identity () : nullptr;
            const auto &turn = identity ? *identity : delivery.turn;
            stateful::with_accepted_application (turn, [&] (auto, const auto &application) {
                event.packet_name = application.packet_name;
                event.flow_id = application.flow_id;
                event.flow_origin = application.flow_origin;
                const auto header_part = identity
                                           ? accepted->application_header ()
                                           : protocol::decode_application_header (application);
                const auto header =
                  actor
                    ? detail::decode_actor_packet_header (header_part,
                                                          delivery.request
                                                            ? detail::stream_message_kind_t::request
                                                            : detail::stream_message_kind_t::send,
                                                          true)
                    : messaging::envelope_codec_t{}.decode_header (header_part, true);
                if (header) {
                    event.packet_name = header.value ().message_name;
                    event.correlation_id = header.value ().correlation_id;
                    if (!event.flow_id) {
                        event.flow_id = header.value ().flow_id;
                        event.flow_origin = header.value ().flow_origin;
                    }
                }
            });
        }
        catch (const protocol::service_wire_error_t &) {
            // Spec 26 §6: optional diagnostic metadata must not replace the
            // original dispatch exception already retained by this event.
        }
        return event;
    });
}
}

bool bound_session_bind_actor_matches (const protocol::actor_route_fence_t &requested,
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

constexpr std::chrono::hours instance_activation_recovery_retention{24};

using ::zlink::framework::detail::mesh_trace_enabled;

void trace_mesh_host_enabled (std::string_view stage, std::string_view detail)
{
    std::cerr << "zlink mesh-host stage=" << stage << " " << detail << '\n';
}

// Gate argument evaluation too: diagnostic snapshots can enter another lane.
#define trace_mesh_host(stage, detail)                                                             \
    do {                                                                                           \
        if (mesh_trace_enabled ())                                                                 \
            trace_mesh_host_enabled (stage, detail);                                               \
    } while (false)

struct activation_terminal_t
{
    std::function<task_t<void> (bool)> preparation;
    std::function<void ()> accepted;
    task_t<void> finish (bool ready = true)
    {
        auto prepared = std::exchange (preparation, {});
        auto terminal = std::exchange (accepted, {});
        try {
            if (terminal)
                terminal ();
        }
        catch (...) {
            if (prepared)
                prepared (false);
            throw;
        }
        if (prepared)
            return prepared (ready);
        return task_t<void> (result_t<void>::success ());
    }
    ~activation_terminal_t () noexcept
    {
        if (!preparation && !accepted)
            return;
        try {
            auto cleanup = finish (false);
            detail::observe_task_terminal (cleanup, [] (const result_t<void> &result) {
                if (!result)
                    trace_mesh_host ("instance-activation-terminal", result.error ()->what ());
            });
        }
        catch (const std::exception &error) {
            trace_mesh_host ("instance-activation-terminal", error.what ());
        }
        catch (...) {
            trace_mesh_host ("instance-activation-terminal", "accepted terminal cleanup failed");
        }
    }
};

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
    return std::tuple{seal.relocation.high, seal.relocation.low, seal.actor.actor_id};
}

auto session_relocation_key (const protocol::session_relocation_route_t &route)
{
    return std::tuple{route.relocation.high, route.relocation.low, route.actor.actor_id};
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

task_t<std::optional<route_owner_fence_read_t>>
read_route_owner_fence (std::shared_ptr<zlink::framework::location_repository_t> store,
                        char object_kind,
                        std::string object_id,
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
        co_return std::nullopt;
    if (authority_owner_generation != 0 || owner_lease_generation != 0) {
        if (authority_owner_generation == 0 || owner_lease_generation == 0)
            co_return std::nullopt;
        co_return route_owner_fence_read_t{{authority_owner_generation, owner_lease_generation},
                                           std::nullopt};
    }
    if (!store) {
        trace_mesh_host ("route-owner-fence-read", "reason=no-store");
        co_return std::nullopt;
    }
    try {
        auto read = co_await await_result (store->read_authority (
          object_kind == '1' ? actor_authority_key (object_id) : spot_authority_key (object_id)));
        if (!read) {
            trace_mesh_host ("route-owner-fence-read", "reason=authority-read-failed");
            co_return std::nullopt;
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
            co_return std::nullopt;
        }
        location_options_t location_options;
        location_options.owner_lease_fencing_margin = owner_lease_fencing_margin;
        live_location_reader_t live (*store, std::move (location_options));
        const auto admission_lifetime = co_await live.owner_admission_lifetime (snapshot->owner);
        if (!admission_lifetime) {
            trace_mesh_host ("route-owner-fence-read",
                             "reason=admission-lifetime-null owner=" + snapshot->owner.owner_id
                               + " lease=" + std::to_string (snapshot->owner.lease_generation)
                               + " margin_ms="
                               + std::to_string (owner_lease_fencing_margin.count ()));
            co_return std::nullopt;
        }
        co_return route_owner_fence_read_t{
          {snapshot->authority_owner_generation,
           static_cast<std::uint64_t> (snapshot->owner.lease_generation)},
          admission_lifetime};
    }
    catch (...) {
        co_return std::nullopt;
    }
}

zlink::submit_result_t submitted (bool accepted)
{
    return accepted ? zlink::submit_result_t::ok : zlink::submit_result_t::not_connected;
}

std::optional<record_kind_t> record_kind (protocol::command command)
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
            return std::nullopt;
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

bool uses_local_operation_registry (record_kind_t kind)
{
    return is_request (kind) || kind == record_kind_t::spot_control;
}

bool is_actor_application (record_kind_t kind)
{
    return kind == record_kind_t::actor_send || kind == record_kind_t::actor_request;
}

bool is_stateful_application (record_kind_t kind)
{
    return is_actor_application (kind) || kind == record_kind_t::spot_send
           || kind == record_kind_t::spot_request;
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

// Source and route values of a local original that survive the authority suspension.
protocol::spot_message_header_t local_spot_header (const local_application_dispatch_t &dispatch)
{
    protocol::spot_message_header_t header;
    header.target = *dispatch.record.spot_route;
    header.source_spot_id = dispatch.source_spot_id;
    header.operation = {dispatch.record.operation_id.high, dispatch.record.operation_id.low};
    if (is_request (dispatch.record.kind))
        header.correlation = dispatch.record.reply_route_id;
    return header;
}

local_application_dispatch_t local_application_query (const local_application_dispatch_t &original)
{
    local_application_dispatch_t query;
    query.owner = original.owner;
    query.record.kind = original.record.kind;
    query.record.actor_route = original.record.actor_route;
    query.record.spot_route = original.record.spot_route;
    query.record.operation_id = original.record.operation_id;
    query.record.reply_route_id = original.record.reply_route_id;
    query.record.source_node_rid = original.record.source_node_rid;
    query.record.source_session_rid = original.record.source_session_rid;
    query.record.source_binding_generation = original.record.source_binding_generation;
    query.record.source_session_sequence = original.record.source_session_sequence;
    query.source_spot_id = original.source_spot_id;
    return query;
}

std::optional<owner_kind_t> stateful_application_owner (std::string_view owner)
{
    if (owner.starts_with (detail::activation_admission_t::actor_key_prefix))
        return owner_kind_t::actor;
    if (owner.starts_with (detail::activation_admission_t::spot_key_prefix))
        return owner_kind_t::spot;
    return std::nullopt;
}

} // namespace

// Dispatcher storage of one claim: taken originals keep request reply tokens alive
// without another allocation; retained marks a downstream owner of the credit.
struct application_claim_batch_t
{
    struct entry_t
    {
        std::optional<mesh::service_mailbox_record_t> original;
        std::atomic_bool retained{false};
    };

    explicit application_claim_batch_t (std::size_t size) : entries (size) {}

    void take_ordinary (const std::shared_ptr<mesh::service_mailbox_claim_t> &claim,
                        bool stateful) noexcept
    {
        // These owners can enter a relocation cut even after this claim is made.
        if (stateful && stateful_application_owner (claim->owner))
            return;
        for (std::size_t index = 0; index < claim->claimed_messages; ++index)
            entries[index].original.emplace (std::move (*claim->records[index]));
    }

    std::vector<entry_t> entries;
    std::shared_ptr<mesh::service_mailbox_claim_t> claim;
    mesh::service_mailbox_t::application_dispatch_t dispatch;
    std::function<void ()> rejected;
    std::size_t index = 0;
    std::size_t count = 0;
    std::size_t handed_off = 0;

    void handoff (const ready_record_t &ready,
                  const receive_record_t &record,
                  std::vector<zlink::message_t> parts)
    {
        dispatch (ready, record, std::move (parts));
        ++handed_off;
    }

    std::size_t finish ()
    {
        if (rejected) {
            while (handed_off < claim->claimed_messages) {
                rejected ();
                ++handed_off;
            }
        }
        dispatch = {};
        rejected = {};
        claim.reset ();
        return count;
    }
};

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
    const auto route_fence = co_await _host->resolve_spot_route_fence (
      target_node_rid, target_spot_id, target_spot_generation);
    if (!route_fence)
        co_return zlink::submit_result_t::not_found;
    const auto target = protocol::spot_route_fence_t{
      target_spot_id,         target_spot_generation, target_node_rid.to_bytes (),
      target_node_generation, route_fence->first,     route_fence->second};
    if (target.target_node_routing_id == _host->status ().routing_id ().to_bytes ()) {
        co_return co_await _host->enqueue_local_spot_send (target, parts, spot_id ());
    }
    co_return co_await _host->transport ().send_to_spot_result (
      target_node_rid.to_bytes (), spot_id (), target, _host->encode_application (parts, metadata));
}

task_t<zlink::submit_result_t>
spot_handle_t::request_to_spot (const zlink::routing_id_t &target_node_rid,
                                const std::string &target_spot_id,
                                std::uint64_t target_spot_generation,
                                const std::vector<zlink::message_t> &parts,
                                pending_operation_t &operation,
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
    try {
        const auto peer = _host->transport ().topology ().peer (target_node_rid.to_bytes ());
        const auto target_node_generation =
          peer ? peer->descriptor.lifecycle_generation : _host->status ().lifecycle_generation ();
        const auto route_fence = co_await _host->resolve_spot_route_fence (
          target_node_rid, target_spot_id, target_spot_generation);
        if (!route_fence) {
            co_return zlink::submit_result_t::not_found;
        }
        const auto target = protocol::spot_route_fence_t{
          target_spot_id,         target_spot_generation, target_node_rid.to_bytes (),
          target_node_generation, route_fence->first,     route_fence->second};
        const auto host = _host;
        const auto direct_completion = static_cast<bool> (completion);
        if (target.target_node_routing_id == host->status ().routing_id ().to_bytes ()) {
            const auto submitted = co_await host->enqueue_local_spot_request (
              target, parts, operation, timeout, metadata, std::move (completion), spot_id ());
            co_return submitted;
        }
        operation.prepare_for_registration ();
        operation.id = _host->next_operation ();
        const auto accepted = co_await _host->transport ().request_to_spot (
          target_node_rid.to_bytes (), spot_id (), target,
          _host->encode_application (parts, metadata), timeout,
          [host, operation, completion = std::move (completion), direct_completion] (
            foundation::operation_terminal_t terminal, zlink::message_t payload) mutable {
              if (!direct_completion) {
                  host->complete_operation (operation, operation_kind_t::none, terminal,
                                            std::move (payload));
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
                      decoded = result_t<std::vector<zlink::message_t>>::success (
                        protocol::decode_application_parts (std::move (payload),
                                                            host->capture_flow ()));
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

              try {
                  completion (terminal, std::move (decoded));
              }
              catch (...) {
                  host->_options.mesh.runtime_failures->report (std::current_exception ());
              }
          },
          protocol::wire_operation_id_t{operation.id.high, operation.id.low}, std::nullopt);
        co_return submitted (accepted);
    }
    catch (...) {
        throw;
    }
}

task_t<void> spot_handle_t::publish (
  std::string channel_name,
  std::string topic,
  std::vector<zlink::message_t> parts,
  std::vector<std::uint8_t> metadata,
  std::function<void (const zlink::routing_id_t &, zlink::submit_result_t)> failure_observer)
{
    if (!_host) {
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "logical multicast publisher is not connected");
    }
    const auto targets = _host->transport ().topology ().multicast_targets (channel_name);
    const auto local_descriptor = _host->transport ().topology ().local_descriptor ();
    std::optional<task_t<std::vector<stateful::object_ref_t>>> local_targets;
    if (mesh::find_service_channel (local_descriptor, channel_name))
        local_targets.emplace (_host->_objects.multicast_local_targets (channel_name, topic));
    std::exception_ptr first_failure;
    if (!targets.empty ()) {
        const auto encoded = _host->encode_application (parts);
        for (const auto &target : targets) {
            try {
                const auto submitted = co_await _host->transport ().send_logical_multicast (
                  target.descriptor.node_routing_id, channel_name, topic, _object.key, encoded,
                  metadata);
                if (submitted != zlink::submit_result_t::ok && !first_failure) {
                    first_failure = std::make_exception_ptr (framework_exception_t (
                      runtime::messaging::map_submit_result_error_kind (submitted),
                      "logical multicast physical fanout was not admitted"));
                }
                if (submitted != zlink::submit_result_t::ok && failure_observer) {
                    failure_observer (zlink::routing_id_t::from (target.descriptor.node_routing_id),
                                      submitted);
                }
            }
            catch (...) {
                if (failure_observer) {
                    auto submitted = zlink::submit_result_t::not_connected;
                    try {
                        throw;
                    }
                    catch (const framework_exception_t &error) {
                        if (detail::boundary_state (error) == detail::boundary_error_t::shutdown) {
                            submitted = zlink::submit_result_t::terminated;
                        } else if (detail::boundary_state (error)
                                   == detail::boundary_error_t::timed_out) {
                            submitted = zlink::submit_result_t::backpressured;
                        }
                    }
                    catch (...) {
                    }
                    failure_observer (zlink::routing_id_t::from (target.descriptor.node_routing_id),
                                      submitted);
                }
                if (!first_failure)
                    first_failure = std::current_exception ();
            }
        }
    }
    if (local_targets) {
        auto local_metadata =
          metadata.empty ()
            ? std::shared_ptr<const std::vector<std::uint8_t>>{}
            : std::make_shared<const std::vector<std::uint8_t>> (std::move (metadata));
        co_await _host->enqueue_local_multicast (
          std::move (*local_targets), std::move (channel_name), std::move (topic),
          std::move (parts), std::move (local_metadata),
          zlink::routing_id_t::from (local_descriptor.node_routing_id));
    }
    if (first_failure)
        std::rethrow_exception (first_failure);
}

void spot_handle_t::set_subscription (const std::string &channel_name, const std::string &topic)
{
    if (!_host) {
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "logical multicast subscriber is not connected");
    }
    const auto descriptor = _host->transport ().topology ().local_descriptor ();
    if (!mesh::find_service_channel (descriptor, channel_name)) {
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "SPOT does not participate in subscription channel: "
                                       + channel_name);
    }
    if (_host->_objects.set_subscription (_object, channel_name, topic)
        != stateful::stateful_error_t::none)
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "logical multicast subscriber is no longer registered");
}

void spot_handle_t::unset_subscription (const std::string &channel_name, const std::string &topic)
{
    if (!_host)
        return;
    _host->_objects.unset_subscription (_object, channel_name, topic);
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

task_t<zlink::submit_result_t>
actor_handle_t::join_entry_spot (const zlink::routing_id_t &target_node_rid,
                                 const std::vector<zlink::message_t> &parts,
                                 pending_operation_t &operation,
                                 std::chrono::milliseconds timeout)
{
    if (!_host) {
        co_return zlink::submit_result_t::invalid_handle;
    }
    const auto entry = co_await _host->get_or_create_spot_async (_host->_entry_spot_id);
    co_return co_await join_spot (target_node_rid, entry.spot_id (),
                                  entry.status ().lifecycle_generation (), parts, operation,
                                  timeout);
}

task_t<zlink::submit_result_t>
actor_handle_t::join_spot (const zlink::routing_id_t &target_node_rid,
                           const std::string &target_spot_id,
                           std::uint64_t target_spot_generation,
                           const std::vector<zlink::message_t> &parts,
                           pending_operation_t &operation,
                           std::chrono::milliseconds timeout)
{
    if (!_host) {
        co_return zlink::submit_result_t::invalid_handle;
    }
    if (target_node_rid.to_bytes () != _host->status ().routing_id ().to_bytes ()) {
        co_return zlink::submit_result_t::not_connected;
    }
    co_return co_await _host->begin_local_actor_join (
      _actor, target_spot_id, target_spot_generation, parts, operation, timeout);
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
                            pending_operation_t &operation,
                            zlink::send_flags_t,
                            std::chrono::milliseconds timeout,
                            std::span<const std::uint8_t> metadata)
{
    if (!_host)
        co_return zlink::submit_result_t::invalid_handle;
    co_return co_await _host->request_to_actor (target, parts, operation, timeout, metadata);
}

public_host_runtime_t::public_host_runtime_t (host_options_t options) :
    _options ([&] {
        if (!options.mesh.shutdown_admission_seal)
            options.mesh.shutdown_admission_seal = std::make_shared<std::atomic_bool> (false);
        return std::move (options);
    }()),
    _entry_spot_id (zlink::framework::detail::new_entry_spot_id (
      zlink::routing_id_t::from (_options.mesh.descriptor.node_routing_id).to_string ())),
    _transport (
      std::make_shared<mesh::raw_mesh_node_owner_t> (_options.mesh, _options.core_context)),
    _relocation_wire (
      std::make_unique<stateful::raw_relocation_replay_coordinator_t> (*_transport)),
    _objects (),
    _sessions (
      [this] (const std::string &actor_id) {
          return _spot_actor_index_lane
            .run ([&] {
                const auto found = _actors.find (actor_id);
                return found == _actors.end () ? std::optional<stateful::object_ref_t>{}
                                               : std::make_optional (found->second.second);
            })
            .get ();
      },
      [this] { _transport->signal_activity (); })
{
    if (_options.session_relocation_seal_timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument (
          "Session relocation seal timeout must be a positive whole-millisecond duration");
    }
    /* Thread the host's flow-capture provider (flow-correlation §4) into the
     * relocation replay wire; the host outlives the coordinator it owns. */
    _relocation_wire->set_flow_capture_provider ([this] { return capture_flow (); });
    const auto &descriptor = _options.mesh.descriptor;
    _relocation_metrics = std::make_shared<runtime::relocation_metrics_t> ();
    // This host's own object record is not a new target selection: the node placement weight
    // belongs to Framework placement (MeshNode §5.1), so the local candidate keeps the default
    // weight and weight 0 never blocks this node's own objects such as its Entry Spot.
    _objects.replace_placement_candidates (
      {stateful::placement_candidate_t{.mesh_name = descriptor.mesh_name,
                                       .node_id = std::string (descriptor.node_routing_id.begin (),
                                                               descriptor.node_routing_id.end ()),
                                       .stable_types = _options.object_stable_types,
                                       .active_capacity = descriptor.active_capacity_limit,
                                       .active_count = descriptor.active_capacity_used,
                                       .pending_capacity = descriptor.pending_capacity_limit,
                                       .pending_count = descriptor.pending_capacity_used}});
}

public_host_runtime_t::~public_host_runtime_t ()
{
    close ();
}

void public_host_runtime_t::configure_stateful_dispatch (
  stateful::accepted_record_authority_resolver_t resolver, live_location_reader_t *sources)
{
    if (!resolver)
        throw std::invalid_argument ("stateful dispatch authority resolver must not be empty");
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started || _stateful_dispatch)
              throw std::logic_error (
                "stateful dispatch must be configured once before host start");
          if (sources || _local_lease_observation)
              _transport->configure_request_sources (sources, _local_lease_observation);
          _stateful_dispatch = std::make_unique<stateful::raw_stateful_dispatch_t> (
            _objects, *_transport,
            [this, resolver = std::move (resolver)] (
              const stateful::accepted_record_authority_query_t &query)
              -> task_t<std::optional<stateful::accepted_record_authority_t>> {
                auto accepted = co_await _relocation_session_terminal_lane.run_task (
                  [&] () -> std::optional<stateful::accepted_record_authority_t> {
                      for (const auto &[key, attempt] : _relocation_target_attempts) {
                          const auto &prepare = attempt.prepare;
                          if (prepare.source_node_routing_id != query.source_node_routing_id
                              || prepare.source_node_generation != query.source_node_generation
                              || prepare.target.target_node_routing_id
                                   != query.target_node_routing_id
                              || prepare.target.target_node_generation
                                   != query.target_node_generation)
                              continue;
                          const auto target = std::find (attempt.targets.begin (),
                                                         attempt.targets.end (), query.target);
                          if (target == attempt.targets.end ())
                              continue;
                          const auto index = static_cast<std::size_t> (
                            std::distance (attempt.targets.begin (), target));
                          // A post-boundary relay can cross target CAS in flight (§4.4).
                          // Its exact source attempt remains valid alongside the committed fence.
                          if (query.requested_authority_owner_generation
                                != target->authority_owner_generation
                              && (index >= attempt.sources.size ()
                                  || query.requested_authority_owner_generation
                                       != attempt.sources[index].authority_owner_generation))
                              continue;
                          return stateful::accepted_record_authority_t{
                            {prepare.coordinator.owner_id, prepare.coordinator.lease_generation,
                             prepare.source_node_routing_id, prepare.source_node_generation},
                            prepare.target.target_owner_lease_generation};
                      }
                      return std::nullopt;
                  });
                if (accepted)
                    co_return accepted;
                co_return co_await resolver (query);
            });
          /* flow-correlation §4: the ingest path gates flow capture on the host's
           * provider; the host outlives the dispatch it owns. */
          _stateful_dispatch->set_flow_capture_provider ([this] { return capture_flow (); });
      })
      .get ();
    // The source seal cut takes accepted originals through this stateful dispatch.
    _objects.configure_relocation_cut (
      [this] (std::vector<stateful::object_ref_t> participants,
              std::shared_ptr<stateful::relocation_seal_binding_t> binding, std::stop_token) {
          return cut_relocation_source (std::move (participants), std::move (binding));
      });
}

void public_host_runtime_t::configure_stateful_dispatch (
  std::function<std::shared_ptr<const location_owner_token_t> ()> owner,
  live_location_reader_t *sources)
{
    if (!owner)
        throw std::invalid_argument ("stateful dispatch owner observation must not be empty");
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started || _stateful_dispatch)
              throw std::logic_error (
                "stateful dispatch must be configured once before host start");
          _local_lease_observation = std::move (owner);
      })
      .get ();
    configure_stateful_dispatch (
      [this, sources] (stateful::accepted_record_authority_query_t query) {
          // A local record was accepted under the lease observation it was published with;
          // a peer record is accepted under the current one.
          const auto *local =
            query.source_original
              ? std::get_if<mesh::local_request_source_t> (&*query.source_original)
              : nullptr;
          auto owner = local ? local->lease : _local_lease_observation ();
          return accepted_record_authority (std::move (query), std::move (owner), sources);
      },
      sources);
}

task_t<std::optional<stateful::accepted_record_authority_t>>
public_host_runtime_t::accepted_record_authority (
  stateful::accepted_record_authority_query_t query,
  std::shared_ptr<const location_owner_token_t> owner,
  live_location_reader_t *sources) const
{
    const auto &local = _options.mesh.descriptor;
    // The execution owner checks a Spot's current incarnation. Actor admission
    // validates the native object fence here, independently of sender availability.
    if ((query.target.kind == stateful::object_kind_t::actor
         && query.requested_authority_owner_generation != query.target.authority_owner_generation)
        || query.target_node_routing_id != local.node_routing_id
        || query.target_node_generation != local.lifecycle_generation)
        throw detail_make_boundary_exception (detail::boundary_error_t::stale_generation,
                                              "accepted target fence is stale");
    if (query.source_original)
        if (const auto *lookup =
              std::get_if<mesh::request_source_lookup_t> (&*query.source_original)) {
            _transport->mailbox ().start_source_lookup (*lookup);
            auto descriptor = co_await (*lookup)->completion.task ();
            query.source_original = mesh::request_source_t{std::move (descriptor)};
        }
    using authority_t = std::optional<stateful::accepted_record_authority_t>;
    authority_t accepted;
    // Location §9.3: the first accepting owner resolves once; forwarded records keep it.
    if (!query.source_original && sources) {
        auto cached = sources->cached_request_source (local.mesh_name, query.source_node_routing_id,
                                                      query.source_node_generation);
        std::shared_ptr<const mesh_node_descriptor_t> descriptor;
        if (cached)
            descriptor = std::move (*cached);
        else
            descriptor = co_await sources->request_source (
              local.mesh_name, query.source_node_routing_id, query.source_node_generation);
        if (descriptor)
            query.source_original = mesh::request_source_t{std::move (descriptor)};
    }
    if (owner && !owner->owner_id.empty () && owner->lease_generation > 0
        && query.source_original) {
        protocol::request_source_fence_t source;
        if (const auto *reference =
              std::get_if<mesh::local_request_source_t> (&*query.source_original)) {
            source = {reference->lease->owner_id,
                      static_cast<std::uint64_t> (reference->lease->lease_generation),
                      query.source_node_routing_id, query.source_node_generation};
        } else {
            const auto &descriptor =
              std::get<std::shared_ptr<const mesh_node_descriptor_t>> (*query.source_original);
            if (!descriptor)
                co_return accepted;
            source = {descriptor->owner_id,
                      static_cast<std::uint64_t> (descriptor->lease_generation),
                      query.source_node_routing_id, query.source_node_generation};
        }
        accepted = stateful::accepted_record_authority_t{
          std::move (source), static_cast<std::uint64_t> (owner->lease_generation)};
    }
    co_return accepted;
}

void public_host_runtime_t::forward_relocation_application (
  const stateful::object_ref_t &owner,
  const stateful::turn_record_t &record,
  const std::vector<std::uint8_t> &target_routing_id,
  std::uint64_t target_generation,
  std::uint64_t target_lease_generation,
  std::chrono::milliseconds window,
  std::optional<std::uint64_t> target_authority_generation)
{
    if (!_stateful_dispatch)
        return;
    auto pending = std::make_shared<task_t<bool>> (_stateful_dispatch->forward_accepted (
      owner, record, target_routing_id, target_generation, target_lease_generation, window,
      target_authority_generation));
    detail::observe_task_completion (
      *pending, [host = shared_from_this (), pending] (const result_t<bool> &) {});
}

void public_host_runtime_t::configure_message_follow_handler (
  std::function<void (const protocol::message_follow_notice_t &)> handler)
{
    _lifecycle_configuration_lane.run ([&] { _message_follow_handler = std::move (handler); })
      .get ();
}

void public_host_runtime_t::configure_actor_join_relocation (
  actor_join_relocation_prepare_validator_t prepare_validator,
  actor_join_recovery_consumer_t recovery_consumer,
  actor_relocation_authority_resolver_t authority_spot_resolver,
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
          _actor_relocation_authority_resolver = std::move (authority_spot_resolver);
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
              throw std::logic_error (
                "late Session route reporter must be configured before start");
          _late_session_route_update_reporter = std::move (reporter);
      })
      .get ();
}

void public_host_runtime_t::start ()
{
    auto population_metrics = _lifecycle_configuration_lane
                                .run ([&] {
                                    if (_started || _closing) {
                                        return task_t<void> (result_t<void>::success ());
                                    }
                                    auto configured = _objects.configure_population_metrics (
                                      _options.mesh.descriptor.mesh_name);
                                    auto owner = std::make_unique<completion_owner_t> ();
                                    _transport->start ();
                                    _completion_owner = std::move (owner);
                                    _started = true;
                                    return configured;
                                })
                                .get ();
    population_metrics.result ().value ();
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
            _options.mesh.runtime_failures->report (std::current_exception ());
        }
    }
    const auto already_sealed =
      _options.mesh.shutdown_admission_seal->load (std::memory_order_acquire);
    auto target_terminal = seal_relocation_targets ();
    // Hosted shutdown already drained this terminal at its original deadline.
    if (!already_sealed)
        target_terminal.result ().value ();
    _lifecycle_configuration_lane.run ([&] { _started = false; }).get ();
    _relocation_session_terminal_lane
      .run ([&] {
          _session_seal_terminals.clear ();
          _session_journal_terminals.clear ();
      })
      .get ();
    _sessions.drop_held_relays (message_flow_reason_t::shutdown);
    _transport->close ();
    (void) _objects.retire_population_metrics_async ();
    _local_dispatch_completion_lane.run ([&] { _local_application_dispatches.clear (); }).get ();
    _lifecycle_configuration_lane
      .run ([&] {
          _completion_owner.reset ();
          _closing = false;
      })
      .get ();
}

void public_host_runtime_t::configure_peer_reconcile (
  std::function<void (const std::vector<std::uint8_t> &)> request)
{
    _transport->configure_peer_reconcile (std::move (request));
}

bool public_host_runtime_t::connect_peer (const std::string &endpoint,
                                          std::optional<zlink::routing_id_t> expected,
                                          std::uint64_t expected_lifecycle_generation,
                                          std::string security_identity,
                                          bool automatic)
{
    return _peer_endpoint_lane
      .run ([&] {
          auto expected_key = expected ? expected->to_string () : std::string{};
          if (automatic && expected
              && (_transport->peer_route (expected->to_bytes ()).first != 0
                  || std::any_of (
                    _peer_endpoints.begin (), _peer_endpoints.end (), [&] (const auto &intent) {
                        return intent.first == endpoint || intent.second == expected_key;
                    })))
              return false;
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
          if (connected)
              _peer_endpoints.insert_or_assign (endpoint, std::move (expected_key));
          return connected;
      })
      .get ();
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

bool public_host_runtime_t::disconnect_peer (const std::vector<std::uint8_t> &expected_routing_id,
                                             const std::string &endpoint) noexcept
{
    const auto endpoint_retained = _transport->disconnect_peer (expected_routing_id, endpoint);
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
    return _transport->pending_operation_count ()
           + _relocation_session_terminal_lane
               .run ([this] {
                   return std::count_if (
                     _relocation_target_attempts.begin (), _relocation_target_attempts.end (),
                     [] (const auto &item) { return !relocation_target_terminal (item.second); });
               })
               .get ();
}

void public_host_runtime_t::set_channel_weight (const std::string &channel_name,
                                                std::uint32_t weight)
{
    if (weight > 10000) {
        throw std::invalid_argument ("channel weight exceeds 10000");
    }
    auto descriptor = _transport->topology ().local_descriptor ();
    const auto found = mesh::find_service_channel (descriptor, channel_name);
    if (!found) {
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

task_t<stateful::stateful_error_t>
public_host_runtime_t::destroy_application_actor_async (std::string actor_id,
                                                        std::uint64_t object_generation)
{
    const auto host = shared_from_this ();
    stateful::object_ref_t object;
    const auto generation_stale = co_await host->_spot_actor_index_lane.run_task ([&] {
        const auto found = host->_actors.find (std::string (actor_id));
        if (found != host->_actors.end ()) {
            if (found->second.second.object_generation != object_generation)
                return true;
            object = found->second.second;
        }
        return false;
    });
    if (generation_stale)
        co_return stateful::stateful_error_t::generation_stale;
    if (object.key.empty ()) {
        const auto local = co_await host->_objects.find_async (stateful::object_kind_t::actor,
                                                               std::string (actor_id));
        if (!local)
            co_return stateful::stateful_error_t::not_found;
        if (local->object_generation != object_generation)
            co_return stateful::stateful_error_t::generation_stale;
        object = *local;
    }
    const auto destroyed = co_await host->_objects.destroy_actor_async (object);
    if (destroyed != stateful::stateful_error_t::none)
        co_return destroyed;
    co_await host->_spot_actor_index_lane.run_task ([&] {
        const auto found = host->_actors.find (std::string (actor_id));
        if (found != host->_actors.end ()
            && found->second.second.object_generation == object_generation)
            host->_actors.erase (found);
    });
    co_return stateful::stateful_error_t::none;
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
  user_spot_materializer_t materializer,
  user_spot_closer_t closer)
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
          _user_spot_closer = std::move (closer);
      })
      .get ();
}

void public_host_runtime_t::configure_spot_route_fence_resolver (
  spot_route_fence_resolver_t resolver)
{
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started)
              throw std::logic_error (
                "Spot route fence resolver must be configured before host start");
          _spot_route_fence_resolver = std::move (resolver);
      })
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
              throw std::logic_error (
                "Actor join operations must be configured once before host start");
          _actor_join_target = std::move (target);
      })
      .get ();
}

void public_host_runtime_t::configure_instance_spot_operations (
  std::shared_ptr<zlink::framework::location_repository_t> store,
  std::shared_ptr<stateful::relocation_store_port_t> relocations,
  std::function<std::optional<location_owner_token_t> ()> owner,
  instance_spot_activation_materializer_t materializer)
{
    if (!store || !relocations || !owner || !materializer)
        throw std::invalid_argument ("Instance Spot operations require Location and Relocation "
                                     "Stores, an owner lease resolver, and a materializer");
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started)
              throw std::logic_error ("Instance Spot operations must be configured before start");
          _user_spot_store = std::move (store);
          _session_relocations = relocations;
          _instance_spot_relocations = std::move (relocations);
          _instance_spot_owner = std::move (owner);
          for (const auto &type : materializer.metric_types)
              _instance_admission_metrics.emplace (
                type, std::make_shared<runtime::instance_spot_admission_metrics_t> (
                        _options.mesh.descriptor.mesh_name, type));
          _instance_spot_materializer =
            std::make_shared<const instance_spot_activation_materializer_t> (
              std::move (materializer));
      })
      .get ();
}

namespace
{

using close_completion_t = std::shared_ptr<task_completion_source_t<spot_close_commit_t>>;

std::function<task_t<authority_snapshot_t> ()>
reincarnate_closing_authority (std::shared_ptr<location_repository_t> store,
                               authority_key_t key,
                               std::string version,
                               std::vector<std::byte> ready_payload)
{
    return [store, key, version, ready_payload] {
        auto completion = std::make_shared<task_completion_source_t<authority_snapshot_t>> ();
        auto output = completion->task ();
        after_close_step (
          store->compare_exchange_authority (key, version, authority_reincarnate_t{ready_payload}),
          {}, [completion] (result_t<authority_compare_exchange_result_t> exchanged) {
              if (!exchanged) {
                  completion->complete (result_t<authority_snapshot_t>::failure (
                    exchanged.error_kind (),
                    exchanged.error () ? exchanged.error ()->what () : "Spot Reincarnate failed"));
                  return;
              }
              const auto *stored = std::get_if<authority_stored_t> (&exchanged.value ());
              if (!stored) {
                  completion->complete (result_t<authority_snapshot_t>::failure (
                    framework_error_kind_t::unavailable, "Spot Reincarnate fence conflicted"));
                  return;
              }
              completion->complete (result_t<authority_snapshot_t>::success (stored->snapshot));
          });
        return output;
    };
}

void complete_close_step (const close_completion_t &completion, spot_close_commit_t commit)
{
    completion->complete (result_t<spot_close_commit_t>::success (std::move (commit)));
}

spot_close_commit_t
close_step_failure (framework_error_kind_t kind, const char *message, std::uint32_t cause_code = 0)
{
    return {detail::result_access_t::failure<bool> (
              detail::with_failure_code (framework_exception_t (kind, message), cause_code)),
            {}};
}

template <typename T> result_t<bool> close_store_failure (const result_t<T> &failed)
{
    return detail::propagate_failure<bool> (failed, "Location Store operation failed");
}

/* Step 4: deletes the Closing authority with the same owner and generation
 * fence (the Closing StoreVersion). */
task_t<bool>
release_closing_authority_async (std::shared_ptr<zlink::framework::location_repository_t> store,
                                 authority_key_t authority_key,
                                 std::string closing_version,
                                 std::function<task_t<bool> ()> before_delete)
{
    if (before_delete)
        (void) co_await before_delete ();
    const auto deleted = co_await await_result (
      store->compare_exchange_authority (authority_key, closing_version, authority_delete_t{}));
    if (!deleted)
        co_return close_store_failure (deleted);
    if (!std::holds_alternative<authority_deleted_t> (deleted.value ()))
        co_return result_t<bool>::failure (framework_error_kind_t::internal_failure,
                                           "Spot authority release failed");
    co_return true;
}

std::function<task_t<bool> ()>
release_closing_authority (std::shared_ptr<zlink::framework::location_repository_t> store,
                           authority_key_t authority_key,
                           std::string closing_version,
                           std::function<task_t<bool> ()> before_delete)
{
    return
      [store = std::move (store), authority_key = std::move (authority_key),
       closing_version = std::move (closing_version), before_delete = std::move (before_delete)] {
          return release_closing_authority_async (store, authority_key, closing_version,
                                                  before_delete);
      };
}

task_t<bool> release_user_spot_record_async (std::shared_ptr<public_host_runtime_t> host,
                                             stateful::object_ref_t ref)
{
    const auto [error, closed] = co_await host->objects ().close_spot_async (ref);
    if (error != stateful::stateful_error_t::none && error != stateful::stateful_error_t::not_found)
        co_return result_t<bool>::failure (framework_error_kind_t::internal_failure,
                                           "User Spot record release failed");
    co_return true;
}

} // namespace

task_t<spot_close_commit_t>
public_host_runtime_t::begin_instance_spot_close (const std::string &stable_type,
                                                  const std::string &spot_id,
                                                  std::uint64_t object_generation,
                                                  std::uint64_t authority_owner_generation)
{
    auto completion = std::make_shared<task_completion_source_t<spot_close_commit_t>> ();
    auto output = completion->task ();
    if (stable_type.empty () || spot_id.empty () || object_generation == 0
        || authority_owner_generation == 0) {
        complete_close_step (completion, {});
        return output;
    }

    std::shared_ptr<zlink::framework::location_repository_t> store;
    std::function<std::optional<location_owner_token_t> ()> instance_owner_resolver;
    _lifecycle_configuration_lane
      .run ([&] {
          store = _user_spot_store;
          instance_owner_resolver = _instance_spot_owner;
      })
      .get ();
    const auto instance_owner = instance_owner_resolver ? instance_owner_resolver () : std::nullopt;
    if (!store || !instance_owner) {
        complete_close_step (completion, {});
        return output;
    }

    const auto authority_key = spot_authority_key (spot_id);
    const auto local = status ();
    const auto mesh_name = _options.mesh.descriptor.mesh_name;
    after_close_step (
      store->read_authority (authority_key), {},
      [completion, store, authority_key, stable_type, spot_id, object_generation,
       authority_owner_generation, instance_owner = *instance_owner, local,
       mesh_name] (result_t<authority_read_result_t> current) {
          if (!current) {
              complete_close_step (completion, {close_store_failure (current), {}});
              return;
          }
          const auto *snapshot = std::get_if<authority_snapshot_t> (&current.value ());
          if (!snapshot || snapshot->allocation.state != placement_allocation_state_t::active
              || snapshot->allocation.object_kind != placement_object_kind_t::instance_spot
              || snapshot->allocation.stable_type != stable_type
              || snapshot->object_generation != object_generation
              || snapshot->authority_owner_generation != authority_owner_generation
              || snapshot->allocation.target.owner.owner_id != instance_owner.owner_id
              || snapshot->allocation.target.owner.lease_generation
                   != instance_owner.lease_generation
              || snapshot->allocation.target.mesh_name != mesh_name
              || snapshot->allocation.target.node_rid.value ()
                   != node_rid_t::from_string (local.routing_id ().to_string ()).value ()
              || snapshot->allocation.target.node_lifecycle_generation
                   != local.lifecycle_generation ()) {
              complete_close_step (completion, {});
              return;
          }

          // A Close that committed Closing for this owner and generation
          // resumes from its remaining steps (Spot address messaging §7).
          if (const auto closing = decode_instance_closing_state (snapshot->payload);
              closing && closing->stable_type == stable_type && closing->spot_id == spot_id
              && closing->object_generation == object_generation
              && closing->authority_owner_generation == authority_owner_generation) {
              const auto ready_payload = encode_instance_spot_authority_payload (
                {.state = instance_spot_authority_state_t::ready,
                 .stable_type = stable_type,
                 .spot_id = spot_id,
                 .owner_id = instance_owner.owner_id,
                 .owner_lease_generation =
                   static_cast<std::uint64_t> (instance_owner.lease_generation),
                 .mesh_name = snapshot->allocation.target.mesh_name,
                 .node_rid = snapshot->allocation.target.node_rid,
                 .node_generation = snapshot->allocation.target.node_lifecycle_generation});
              complete_close_step (
                completion,
                {result_t<bool>::success (true),
                 release_closing_authority (store, authority_key, snapshot->store_version, {}),
                 reincarnate_closing_authority (store, authority_key, snapshot->store_version,
                                                ready_payload),
                 [store, authority_key] (const authority_snapshot_t &next) {
                     return release_closing_authority (store, authority_key, next.store_version,
                                                       {}) ();
                 }});
              return;
          }

          const auto ready = decode_instance_spot_authority_payload (snapshot->payload);
          if (!ready || ready->state != instance_spot_authority_state_t::ready
              || ready->stable_type != stable_type || ready->spot_id != spot_id
              || ready->owner_id != instance_owner.owner_id
              || ready->owner_lease_generation
                   != static_cast<std::uint64_t> (instance_owner.lease_generation)
              || ready->mesh_name != snapshot->allocation.target.mesh_name
              || ready->node_rid.value () != snapshot->allocation.target.node_rid.value ()
              || ready->node_generation != snapshot->allocation.target.node_lifecycle_generation
              || ready->activation_recovery) {
              complete_close_step (completion, {});
              return;
          }
          after_close_step (
            store->compare_exchange_authority (
              authority_key, snapshot->store_version,
              authority_put_t{encode_instance_closing_state (instance_closing_state_t{
                stable_type, spot_id, object_generation, authority_owner_generation})}),
            {},
            [completion, store, ready_payload = snapshot->payload,
             authority_key] (result_t<authority_compare_exchange_result_t> sealed) {
                if (!sealed) {
                    complete_close_step (completion, {close_store_failure (sealed), {}});
                    return;
                }
                const auto *closing = std::get_if<authority_stored_t> (&sealed.value ());
                if (!closing) {
                    complete_close_step (completion, {});
                    return;
                }
                complete_close_step (
                  completion,
                  {result_t<bool>::success (true),
                   release_closing_authority (store, authority_key, closing->snapshot.store_version,
                                              {}),
                   reincarnate_closing_authority (store, authority_key,
                                                  closing->snapshot.store_version, ready_payload),
                   [store, authority_key] (const authority_snapshot_t &next) {
                       return release_closing_authority (store, authority_key, next.store_version,
                                                         {}) ();
                   }});
            });
      });
    return output;
}

bool public_host_runtime_t::evict_instance_spot (const std::string &stable_type,
                                                 const std::string &spot_id,
                                                 std::uint64_t object_generation,
                                                 std::uint64_t authority_owner_generation,
                                                 std::function<void ()> close_local)
{
    if (!close_local)
        return false;
    /* Idle cleanup calls this from its maintenance thread after it sealed the
     * activation and re-checked serial quiescence (Object lifecycle §5), so
     * the local close cannot be refused once Closing is committed and no path
     * returns the authority to Ready. */
    const auto begun = begin_instance_spot_close (stable_type, spot_id, object_generation,
                                                  authority_owner_generation)
                         .result ();
    if (!begun || !begun.value ().release)
        return false;
    try {
        close_local ();
    }
    catch (...) {
        /* commit_idle_close marks the local context closed before invoking
         * the application callback, so the activation is gone either way. */
    }
    const auto released = begun.value ().release ().result ();
    return released && released.value ();
}

task_t<spot_close_commit_t>
public_host_runtime_t::begin_user_spot_close (protocol::user_spot_close_fence_t target)
{
    auto [store, owner_resolver] = co_await _lifecycle_configuration_lane.run_task (
      [this] { return std::pair{_user_spot_store, _session_route_owner_resolver}; });
    if (!store)
        co_return close_step_failure (framework_error_kind_t::not_configured,
                                      "User Spot close requires a Location Store");
    const auto authority_key = spot_authority_key (target.spot_id);
    auto self = shared_from_this ();
    const auto read = co_await await_result (store->read_authority (authority_key));
    const auto moving = [] {
        return close_step_failure (
          framework_error_kind_t::unavailable, "User Spot owner is moving",
          static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving));
    };
    if (!read) {
        co_return spot_close_commit_t{close_store_failure (read), {}};
    }
    const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
    if (!snapshot) {
        co_return spot_close_commit_t{};
    }
    if (snapshot->object_generation != target.object_generation) {
        co_return close_step_failure (
          framework_error_kind_t::invalid_operation, "User Spot generation is stale",
          static_cast<std::uint32_t> (protocol::framework_error_code::spotGenerationStale));
    }
    // The one classification of a Close request (§7.1, §9): another
    // ObjectGeneration is InvalidOperation; any other owner fence field,
    // including this runtime's Mesh, node and current owner lease, or an
    // allocation that is not active is a moving owner (Unavailable).
    const auto local = self->status ();
    const auto owner = owner_resolver ? owner_resolver () : std::nullopt;
    // A local owner request carries no StoreVersion; command 48 always does.
    if (snapshot->authority_owner_generation != target.authority_owner_generation
        || (!target.expected_store_version.empty ()
            && snapshot->store_version != target.expected_store_version)
        || snapshot->allocation.object_kind != placement_object_kind_t::user_spot
        || snapshot->allocation.state != placement_allocation_state_t::active
        || snapshot->allocation.target.node_rid.value ()
             != node_rid_t::from_string (
                  zlink::routing_id_t::from (target.target_node_routing_id).to_string ())
                  .value ()
        || snapshot->allocation.target.node_lifecycle_generation != target.target_node_generation
        || snapshot->allocation.target.mesh_name != self->_options.mesh.descriptor.mesh_name
        || snapshot->allocation.target.node_rid.value ()
             != node_rid_t::from_string (local.routing_id ().to_string ()).value ()
        || snapshot->allocation.target.node_lifecycle_generation != local.lifecycle_generation ()
        || (owner
            && (snapshot->allocation.target.owner.owner_id != owner->owner_id
                || snapshot->allocation.target.owner.lease_generation
                     != owner->lease_generation))) {
        co_return moving ();
    }
    const stateful::object_ref_t exact_ref{
      stateful::object_kind_t::user_spot,
      target.spot_id,
      target.object_generation,
      target.authority_owner_generation,
      snapshot->allocation.target.mesh_name,
      std::string (snapshot->allocation.target.node_rid.value ())};
    const auto ready_payload = ready_user_spot_authority_payload (
      exact_ref, snapshot->allocation.stable_type, snapshot->allocation.target);
    const auto closing_payload = closing_user_spot_authority_payload (
      exact_ref, snapshot->allocation.stable_type, snapshot->allocation.target);

    // Step 4 releases the local object record, then the authority row.
    const auto release_from = [self, store, authority_key] (std::string closing_version,
                                                            stateful::object_ref_t ref) {
        return release_closing_authority (
          store, authority_key, std::move (closing_version),
          [self, ref] { return release_user_spot_record_async (self, ref); });
    };

    if (snapshot->payload == closing_payload) {
        // Closing is committed for this owner and generation: a later
        // Close resumes the remaining steps without repeating the
        // completed ones.
        co_return spot_close_commit_t{result_t<bool>::success (true),
                                      release_from (snapshot->store_version, exact_ref)};
    }
    if (snapshot->payload != ready_payload) {
        co_return moving ();
    }
    const auto local_object =
      co_await self->_objects.find_async (stateful::object_kind_t::user_spot, exact_ref.key);
    if (!local_object || *local_object != exact_ref) {
        co_return moving ();
    }
    const auto [close_error, eligible] = co_await self->_objects.can_close_spot_async (exact_ref);
    if (close_error == stateful::stateful_error_t::generation_stale) {
        co_return close_step_failure (
          framework_error_kind_t::invalid_operation, "User Spot generation is stale",
          static_cast<std::uint32_t> (protocol::framework_error_code::spotGenerationStale));
    }
    if (close_error != stateful::stateful_error_t::none) {
        co_return moving ();
    }
    // Actor membership keeps admission and authority (§7).
    if (!eligible) {
        co_return spot_close_commit_t{};
    }
    const auto sealed = co_await await_result (store->compare_exchange_authority (
      authority_key, snapshot->store_version, authority_put_t{closing_payload}));
    const auto *closing = sealed ? std::get_if<authority_stored_t> (&sealed.value ()) : nullptr;
    if (!closing)
        co_return sealed ? moving () : spot_close_commit_t{close_store_failure (sealed), {}};
    co_return spot_close_commit_t{result_t<bool>::success (true),
                                  release_from (closing->snapshot.store_version, exact_ref)};
}

task_t<spot_close_commit_t>
public_host_runtime_t::begin_local_user_spot_close (const std::string &spot_id,
                                                    std::uint64_t object_generation,
                                                    std::uint64_t authority_owner_generation)
{
    const auto local = status ();
    return begin_user_spot_close (protocol::user_spot_close_fence_t{spot_id,
                                                                    object_generation,
                                                                    local.routing_id ().to_bytes (),
                                                                    local.lifecycle_generation (),
                                                                    authority_owner_generation,
                                                                    {}});
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
  session_seal_local_completion_t local_completion,
  const std::function<void (const std::vector<std::uint8_t> &)> &reconcile)
{
    // Session-Actor binding §4: compare with this host's current identity exactly.
    const auto local = status ();
    if (session_owner.lease_generation <= 0
        || !protocol::same_session_owner_identity (
          {seal.session_owner_node_routing_id,
           seal.session_owner_node_generation,
           seal.session_owner_id,
           seal.session_owner_lease_generation,
           {},
           0},
          {local.routing_id ().to_bytes (),
           local.lifecycle_generation (),
           session_owner.owner_id,
           static_cast<std::uint64_t> (session_owner.lease_generation),
           {},
           0})) {
        return {false, std::nullopt};
    }

    flush_pending_session_relocation_seals ();
    const auto relocation_key = session_relocation_key (seal);
    const auto existing_result =
      _relocation_session_terminal_lane
        .run ([&] () -> std::optional<
                       std::pair<bool, std::optional<protocol::session_relocation_sealed_t>>> {
            const auto cached = _session_seal_terminals.find (relocation_key);
            if (cached != _session_seal_terminals.end ()) {
                if (cached->second.seal != seal)
                    return {{false, std::nullopt}};
                if (cached->second.consumed) {
                    const auto &logger =
                      detail::dispatch_options_access_t::logger (_options.mesh.dispatch);
                    if (logger)
                        detail::diagnostic_event_sink_t::log_if_configured (
                          logger, log_level_t::warn, "late_session_relocation_seal", {});
                    return {{true, std::nullopt}};
                }
                if (!response_routing_id.empty ())
                    cached->second.response_routing_id = std::move (response_routing_id);
                if (cached->second.ready)
                    return {{true, cached->second.sealed}};
                if (local_completion)
                    cached->second.local_completions.push_back (std::move (local_completion));
                return {{true, std::nullopt}};
            }
            return std::nullopt;
        })
        .get ();
    if (existing_result)
        return *existing_result;

    const auto session_id = stateful::stream_session_registry_t::connection_key (
      zlink::routing_id_t::from (seal.session_routing_id));
    const auto admission =
      _sessions.seal_remote_route (session_id, seal.binding_generation, seal.actor.actor_id,
                                   seal.actor.object_generation, seal.actor.owner_lease_generation);
    if (admission.error != stateful::stateful_error_t::none
        && admission.error != stateful::stateful_error_t::backpressured) {
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
                                                    seal.binding_generation,
                                                    admission.seal_result};
    const auto ready =
      admission.seal_result == protocol::session_relocation_seal_result_t::binding_absent
      || _sessions.remote_route_seal_ready (admission.barrier);
    bool inserted = false;
    std::optional<protocol::session_relocation_sealed_t> immediate;
    _relocation_session_terminal_lane
      .run ([&] {
          session_seal_terminal_record_t record{seal,
                                                ack,
                                                admission.barrier,
                                                std::chrono::steady_clock::now ()
                                                  + _options.session_relocation_seal_timeout,
                                                false,
                                                ready,
                                                response_routing_id,
                                                {}};
          if (local_completion && !ready)
              record.local_completions.push_back (local_completion);
          // A binding retains only its last closed record. The new seal owns its successor.
          for (auto it = _session_seal_terminals.begin (); it != _session_seal_terminals.end ();) {
              if (it->second.consumed && it->second.seal.actor.actor_id == seal.actor.actor_id)
                  it = _session_seal_terminals.erase (it);
              else
                  ++it;
          }
          const auto [stored, was_inserted] =
            _session_seal_terminals.emplace (relocation_key, std::move (record));
          lower_relocation_next_due_on_lane (relocation_due_now);
          inserted = was_inserted;
          if (inserted)
              _transport->signal_activity ();
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
        const auto stored_matches =
          _relocation_session_terminal_lane
            .run ([&] {
                const auto stored = _session_seal_terminals.find (relocation_key);
                return stored != _session_seal_terminals.end () && stored->second.seal == seal;
            })
            .get ();
        if (!stored_matches)
            return {false, std::nullopt};
    }
    request_peer_reconcile (seal.target_node_routing_id, seal.target_node_generation, reconcile);
    return {true, std::move (immediate)};
}

task_t<bool> public_host_runtime_t::seal_session_remote (
  const zlink::routing_id_t &session_owner_node,
  protocol::session_relocation_seal_t seal,
  std::chrono::milliseconds timeout,
  std::chrono::steady_clock::time_point operation_deadline,
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
    auto response =
      std::make_shared<std::function<void (foundation::operation_terminal_t, zlink::message_t)>> (
        [weak_host, expected, relocation_key, operation_deadline,
         relocations = std::move (relocations), capture_journal = std::move (capture_journal),
         completion = std::move (completion)] (foundation::operation_terminal_t terminal,
                                               zlink::message_t payload) mutable {
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
                const auto sealed =
                  protocol::decode_session_relocation_sealed (protocol::message_bytes (payload));
                if (sealed.relocation != expected.relocation
                    || sealed.coordinator != expected.coordinator || sealed.actor != expected.actor
                    || sealed.session_owner_node_routing_id
                         != expected.session_owner_node_routing_id
                    || sealed.session_owner_node_generation
                         != expected.session_owner_node_generation
                    || sealed.session_owner_id != expected.session_owner_id
                    || sealed.session_owner_lease_generation
                         != expected.session_owner_lease_generation
                    || sealed.session_routing_id != expected.session_routing_id
                    || sealed.binding_generation != expected.binding_generation) {
                    completion (foundation::operation_terminal_t::transport_failed, std::nullopt);
                    return;
                }
                if (sealed.seal_result
                    == protocol::session_relocation_seal_result_t::binding_absent) {
                    completion (terminal, session_relocation_seal_result_t{sealed, {}});
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
                const auto root = journal_store.prepare (record, operation_deadline);
                const auto recovered = journal_store.recover (root);
                if (!recovered || *recovered != record) {
                    journal_store.cleanup (root);
                    completion (foundation::operation_terminal_t::transport_failed, std::nullopt);
                    return;
                }
                const session_relocation_seal_result_t result{sealed, root};
                std::optional<session_relocation_seal_result_t> existing_result;
                bool conflicting_terminal = false;
                host->_relocation_session_terminal_lane
                  .run ([&] {
                      const auto [stored, inserted] = host->_session_journal_terminals.emplace (
                        relocation_key, std::pair{expected, result});
                      if (!inserted) {
                          journal_store.cleanup (root);
                          conflicting_terminal = stored->second.first != expected;
                          if (!conflicting_terminal)
                              existing_result = stored->second.second;
                      }
                  })
                  .get ();
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
                const auto failure = detail::current_exception_result<void> ();
                const auto *error = failure.error ();
                completion (error && error->kind () == framework_error_kind_t::deadline_exceeded
                              ? foundation::operation_terminal_t::timed_out
                              : foundation::operation_terminal_t::transport_failed,
                            std::nullopt);
            }
        });

    const auto local = status ();
    if (local.routing_id ().to_bytes () == session_owner_node.to_bytes ()) {
        std::function<std::optional<location_owner_token_t> ()> owner_resolver;
        std::function<void (const std::vector<std::uint8_t> &)> reconcile;
        _lifecycle_configuration_lane.run ([&] { owner_resolver = _session_route_owner_resolver; })
          .get ();
        reconcile = _transport->peer_reconcile ();
        if (local.lifecycle_generation () != seal.session_owner_node_generation || !owner_resolver)
            co_return false;
        const auto owner = owner_resolver ();
        if (!owner)
            co_return false;
        auto local_completion =
          [response] (foundation::operation_terminal_t terminal,
                      std::optional<protocol::session_relocation_sealed_t> sealed) mutable {
              (*response) (terminal, sealed
                                       ? zlink::message_t::from (
                                           protocol::encode_session_relocation_sealed (*sealed))
                                       : zlink::message_t{});
          };
        auto [accepted, immediate] =
          admit_session_relocation_seal (seal, *owner, {}, local_completion, reconcile);
        if (!accepted)
            co_return false;
        if (immediate)
            local_completion (foundation::operation_terminal_t::completed, std::move (*immediate));
        co_return true;
    }
    co_return co_await _transport->request_session_relocation_seal (
      session_owner_node.to_bytes (), std::move (seal), timeout,
      [response] (foundation::operation_terminal_t terminal, zlink::message_t payload) mutable {
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
        foundation::operation_terminal_t terminal, zlink::message_t packed) mutable {
          protocol::reply_header_t reply{};
          std::optional<protocol::application_payload_t> application_reply;
          if (terminal == foundation::operation_terminal_t::completed) {
              try {
                  const auto parts =
                    protocol::unpack_infrastructure_reply (protocol::message_bytes (packed));
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
    return messaging::request_failure_mapper_t{}.failure_code_kind (wire_code);
}

stateful::relocation_reason_t classify_relocation_failure_reason (std::uint32_t wire_code) noexcept
{
    switch (classify_relocation_failure_code (wire_code)) {
        case framework_error_kind_t::data_lost:
            return stateful::relocation_reason_t::checksum_mismatch;
        // Legacy remote capacity failures cannot identify an available target,
        // so they fall through with the other remote-side terminal codes.
        case framework_error_kind_t::deadline_exceeded:
            return stateful::relocation_reason_t::turn_active;
        default:
            return stateful::relocation_reason_t::restore_failed;
    }
}

task_t<stateful::relocation_reason_t>
public_host_runtime_t::prepare_relocation_remote (const zlink::routing_id_t &target_node,
                                                  protocol::relocation_prepare_t prepare,
                                                  std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero ())
        co_return stateful::relocation_reason_t::restore_failed;
    const auto response =
      co_await _transport->request_relocation_prepare (target_node.to_bytes (), prepare, timeout);
    if (response.failed) {
        // Exact-identity fencing already ran in request_relocation_prepare
        // — this is not a timeout, it is the target's own explicit,
        // matching-identity rejection. Map and surface its failure_code
        // instead of letting it collapse into the same "no result" a
        // timeout produces.
        const auto kind = classify_relocation_failure_code (response.failed->failure_code);
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
    std::shared_ptr<const instance_spot_activation_materializer_t> materializer;
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
            if (entry.snapshot.allocation.object_kind == placement_object_kind_t::instance_spot
                && entry.snapshot.allocation.state == placement_allocation_state_t::reserved
                && entry.snapshot.pending_creation
                && (entry.snapshot.allocation.target.node_rid.value ()
                      != zlink::routing_id_t::from (local.node_routing_id).to_string ()
                    || entry.snapshot.allocation.target.node_lifecycle_generation
                         != local.lifecycle_generation)) {
                if (store->release_ended_reservation (entry.key, entry.snapshot.store_version)
                      .result ()
                      .value ())
                    relocations->remove (
                      entry.snapshot.pending_creation->request_content_reference);
                continue;
            }
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
            const auto state = decode_instance_spot_authority_payload (entry.snapshot.payload);
            if (!state || state->state != instance_spot_authority_state_t::ready
                || !state->activation_recovery
                || entry.snapshot.allocation.object_kind != placement_object_kind_t::instance_spot
                || entry.snapshot.allocation.state != placement_allocation_state_t::active
                || entry.snapshot.allocation.stable_type != state->stable_type
                || entry.snapshot.allocation.target.node_rid.value ()
                     != node_rid_t::from_string (
                          zlink::routing_id_t::from (local.node_routing_id).to_string ())
                          .value ()
                || entry.snapshot.allocation.target.node_lifecycle_generation
                     != local.lifecycle_generation
                || entry.snapshot.allocation.target.mesh_name != state->mesh_name
                || entry.snapshot.allocation.target.owner.owner_id != state->owner_id
                || entry.snapshot.allocation.target.owner.lease_generation <= 0
                || static_cast<std::uint64_t> (
                     entry.snapshot.allocation.target.owner.lease_generation)
                     != state->owner_lease_generation
                || entry.snapshot.allocation.target.node_rid.value () != state->node_rid.value ()
                || entry.snapshot.allocation.target.node_lifecycle_generation
                     != state->node_generation)
                continue;
            const auto recovery_pointer = *state->activation_recovery;
            const auto payload = relocations->get (recovery_pointer.reference);
            if (!payload || payload->size () != recovery_pointer.encoded_size)
                continue;
            std::vector<std::byte> public_payload;
            public_payload.reserve (payload->size ());
            for (const auto value : *payload)
                public_payload.push_back (static_cast<std::byte> (value));
            if (runtime::sha256 (public_payload) != recovery_pointer.sha256)
                continue;
            protocol::instance_activation_recovery_t recovery;
            try {
                recovery =
                  protocol::decode_instance_activation_recovery (*payload, capture_flow ());
            }
            catch (const protocol::service_wire_error_t &) {
                continue;
            }
            auto completed = entry.snapshot;
            activation_terminal_t activation_terminal;
            if (recovery_pointer.replay_cursor < recovery_pointer.inbox_sequence) {
                bool prepared = false;
                try {
                    prepared = materializer->prepare (recovery.activation, entry.snapshot,
                                                      activation_terminal.preparation);
                }
                catch (...) {
                    prepared = false;
                }
                if (!prepared) {
                    activation_terminal.finish (false).result ().value ();
                    continue;
                }
                auto dispatched =
                  materializer
                    ->dispatch (
                      std::make_shared<const protocol::instance_activation_recovery_t> (recovery),
                      {})
                    .result ()
                    .value ();
                activation_terminal.accepted = std::move (dispatched.accepted_turn_terminal);
                auto updated = *state;
                updated.activation_recovery->replay_cursor =
                  updated.activation_recovery->inbox_sequence;
                const auto terminal =
                  store
                    ->compare_exchange_authority (
                      entry.key, entry.snapshot.store_version,
                      authority_restore_t{encode_instance_spot_authority_payload (updated),
                                          entry.snapshot.owner})
                    .result ()
                    .value ();
                const auto *stored = std::get_if<authority_stored_t> (&terminal);
                if (!stored) {
                    activation_terminal.finish (false).result ().value ();
                    continue;
                }
                completed = stored->snapshot;
            }
            const auto completed_state = decode_instance_spot_authority_payload (completed.payload);
            if (!completed_state || !completed_state->activation_recovery
                || completed_state->activation_recovery->replay_cursor
                     != completed_state->activation_recovery->inbox_sequence) {
                activation_terminal.finish (false).result ().value ();
                continue;
            }
            auto released = *completed_state;
            released.activation_recovery.reset ();
            const auto cleared =
              store
                ->compare_exchange_authority (
                  entry.key, completed.store_version,
                  authority_restore_t{encode_instance_spot_authority_payload (released),
                                      completed.owner})
                .result ()
                .value ();
            if (!std::holds_alternative<authority_stored_t> (cleared)) {
                activation_terminal.finish (false).result ().value ();
                continue;
            }
            relocations->remove (recovery_pointer.reference);
            activation_terminal.finish ().result ().value ();
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
        foundation::operation_terminal_t terminal, zlink::message_t packed) mutable {
          protocol::user_spot_create_reply_t reply{};
          std::optional<protocol::application_payload_t> application_reply;
          if (terminal == foundation::operation_terminal_t::completed) {
              try {
                  const auto parts =
                    protocol::unpack_infrastructure_reply (protocol::message_bytes (packed));
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
        foundation::operation_terminal_t terminal, zlink::message_t packed) mutable {
          protocol::actor_create_reply_t reply;
          std::optional<protocol::application_payload_t> application_reply;
          if (terminal == foundation::operation_terminal_t::completed) {
              try {
                  const auto parts =
                    protocol::unpack_infrastructure_reply (protocol::message_bytes (packed));
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
                                             zlink::message_t packed) mutable {
          protocol::user_spot_close_reply_t reply;
          if (terminal == foundation::operation_terminal_t::completed) {
              try {
                  const auto parts =
                    protocol::unpack_infrastructure_reply (protocol::message_bytes (packed));
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
    return get_or_create_spot_async (std::move (spot_id)).result ().value ();
}

task_t<spot_handle_t> public_host_runtime_t::get_or_create_spot_async (std::string spot_id)
{
    const auto &key = spot_id;
    if (const auto existing = co_await _objects.resolve_spot_async (key))
        co_return spot_handle_t (shared_from_this (), *existing);
    auto created = co_await _objects.begin_create_async (
      stateful::create_request_t{stateful::object_kind_t::user_spot,
                                 key,
                                 "framework.spot",
                                 _options.mesh.descriptor.mesh_name,
                                 {},
                                 false,
                                 false});
    if (created.error != stateful::stateful_error_t::none) {
        throw std::runtime_error ("framework Spot authority creation failed");
    }
    if (created.factory_owner) {
        const auto committed = co_await _objects.commit_create_async (created.attempt);
        if (committed != stateful::stateful_error_t::none)
            throw std::runtime_error ("framework Spot Ready commit failed");
    }
    auto object = co_await _objects.find_async (stateful::object_kind_t::user_spot, key);
    if (!object) {
        throw std::runtime_error ("framework Spot authority is unavailable");
    }
    co_return spot_handle_t (shared_from_this (), *object);
}

spot_handle_t public_host_runtime_t::bind_relocation_spot (stateful::object_ref_t object)
{
    return spot_handle_t (shared_from_this (), std::move (object));
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
    const auto existing =
      _spot_actor_index_lane
        .run ([&] () -> std::optional<std::pair<actor_ref_t, stateful::object_ref_t>> {
            const auto found = _actors.find (actor_id);
            if (found != _actors.end ()) {
                return std::make_pair (
                  framework_actor_ref (found->second.second, found->second.first),
                  found->second.second);
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
    const auto existing =
      _spot_actor_index_lane
        .run ([&] () -> std::optional<actor_handle_t> {
            const auto found = _actors.find (reserved.key);
            if (found != _actors.end ()) {
                if (found->second.second.object_generation != reserved.object_generation
                    || found->second.second.authority_owner_generation
                         != reserved.authority_owner_generation) {
                    const auto adopted = _objects.adopt_reserved_actor_owner (reserved, actor_type);
                    if (adopted == stateful::stateful_error_t::none) {
                        found->second.second = reserved;
                        return actor_handle_t (shared_from_this (),
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

task_t<std::optional<route_fence_t>>
public_host_runtime_t::resolve_spot_route_fence (zlink::routing_id_t target_node_rid,
                                                 std::string target_spot_id,
                                                 std::uint64_t target_spot_generation)
{
    spot_route_fence_resolver_t resolver;
    std::shared_ptr<zlink::framework::location_repository_t> store;
    _lifecycle_configuration_lane.run_sync ([&] {
        resolver = _spot_route_fence_resolver;
        store = _user_spot_store;
    });
    if (resolver) {
        auto resolved = co_await await_result (
          resolver (target_node_rid, target_spot_id, target_spot_generation));
        co_return resolved ? resolved.value () : std::nullopt;
    }

    const auto measured_at = std::chrono::steady_clock::now ();
    const auto read =
      co_await read_route_owner_fence (store, '2', target_spot_id, target_spot_generation, 0, 0,
                                       _options.owner_lease_fencing_margin);
    if (read && read->admission_lifetime
        && std::chrono::steady_clock::now () >= measured_at + *read->admission_lifetime)
        co_return std::nullopt;
    co_return read ? std::optional<route_fence_t> (read->fence) : std::nullopt;
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
        co_return co_await enqueue_local_actor_message (
          target, record_kind_t::actor_send, parts, nullptr, std::move (bound_session_source), {},
          authority_owner_generation, owner_lease_generation);
    }
    const auto peer = _transport->topology ().peer (target_routing_id.to_bytes ());
    if (!peer) {
        co_return zlink::submit_result_t::not_connected;
    }
    const auto node_generation = peer->descriptor.lifecycle_generation;
    const auto object = co_await _objects.find_async (stateful::object_kind_t::actor,
                                                      std::string (target.actor_id ().value ()));
    const auto authority_generation = authority_owner_generation != 0 ? authority_owner_generation
                                      : object ? object->authority_owner_generation
                                               : target.object_generation ();
    const auto route_fence = co_await read_route_owner_fence (
      _user_spot_store, '1', std::string (target.actor_id ().value ()), target.object_generation (),
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
    /* Session-Actor binding §3 item 3: the push source sends by SessionRid
     * and binding generation to the registered route. The Session owner
     * alone decides whether that binding is current. */
    const auto local = status ();
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
  pending_operation_t &operation,
  std::chrono::milliseconds timeout,
  std::span<const std::uint8_t> metadata,
  std::uint64_t authority_owner_generation,
  std::uint64_t owner_lease_generation,
  std::optional<protocol::actor_message_header_t::bound_session_source_t> bound_session_source)
{
    const auto target_routing_id =
      zlink::routing_id_t::from (std::string (target.node_rid ().value ()));
    if (target_routing_id.to_bytes () == status ().routing_id ().to_bytes ()) {
        const auto accepted = co_await enqueue_local_actor_message (
          target, record_kind_t::actor_request, parts, &operation, std::move (bound_session_source),
          timeout, authority_owner_generation, owner_lease_generation);
        co_return accepted;
    }
    operation.prepare_for_registration ();
    operation.id = next_operation ();
    const auto peer = _transport->topology ().peer (target_routing_id.to_bytes ());
    if (!peer) {
        co_return zlink::submit_result_t::not_connected;
    }
    const auto node_generation = peer->descriptor.lifecycle_generation;
    const auto object = co_await _objects.find_async (stateful::object_kind_t::actor,
                                                      std::string (target.actor_id ().value ()));
    const auto authority_generation = authority_owner_generation != 0 ? authority_owner_generation
                                      : object ? object->authority_owner_generation
                                               : target.object_generation ();
    const auto route_fence = co_await read_route_owner_fence (
      _user_spot_store, '1', std::string (target.actor_id ().value ()), target.object_generation (),
      authority_generation, owner_lease_generation, _options.owner_lease_fencing_margin);
    if (!route_fence || route_fence->fence.first != authority_generation) {
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
                         zlink::message_t payload) mutable {
          host->complete_operation (operation, operation_kind_t::none, terminal,
                                    std::move (payload));
      },
      protocol::wire_operation_id_t{operation.id.high, operation.id.low},
      std::move (bound_session_source), std::nullopt);
    co_return submitted (accepted);
}

task_t<zlink::submit_result_t>
public_host_runtime_t::send_to_node (const zlink::routing_id_t &target,
                                     const std::vector<zlink::message_t> &parts)
{
    try {
        return _transport->send_to_node_result (target.to_bytes (), encode_application (parts));
    }
    catch (...) {
        return task_t<zlink::submit_result_t> (
          detail::current_exception_result<zlink::submit_result_t> ());
    }
}

task_t<zlink::submit_result_t>
public_host_runtime_t::send_to_node (const zlink::routing_id_t &target,
                                     std::vector<zlink::message_t> &&parts)
{
    try {
        return _transport->send_to_node_result (target.to_bytes (),
                                                encode_application (std::move (parts)));
    }
    catch (...) {
        return task_t<zlink::submit_result_t> (
          detail::current_exception_result<zlink::submit_result_t> ());
    }
}

task_t<zlink::submit_result_t>
public_host_runtime_t::request_to_node (const zlink::routing_id_t &target,
                                        std::vector<zlink::message_t> parts,
                                        pending_operation_t &operation,
                                        std::chrono::milliseconds timeout)
{
    operation.prepare_for_registration ();
    operation.id = next_operation ();
    const auto host = shared_from_this ();
    const auto accepted = co_await _transport->request_to_node (
      target.to_bytes (), encode_application (std::move (parts)), timeout,
      [host, operation] (foundation::operation_terminal_t terminal,
                         zlink::message_t payload) mutable {
          host->complete_operation (operation, operation_kind_t::none, terminal,
                                    std::move (payload));
      });
    if (!accepted) {
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
                                           pending_operation_t &operation,
                                           std::chrono::milliseconds timeout)
{
    operation.prepare_for_registration ();
    operation.id = next_operation ();
    const auto host = shared_from_this ();
    const auto accepted = co_await _transport->request_to_channel (
      channel_name, encode_application (parts), timeout,
      [host, operation] (foundation::operation_terminal_t terminal,
                         zlink::message_t payload) mutable {
          host->complete_operation (operation, operation_kind_t::none, terminal,
                                    std::move (payload));
      });
    co_return submitted (accepted);
}

bool public_host_runtime_t::relocation_target_wire_ready (
  const relocation_target_attempt_t &attempt) noexcept
{
    return attempt.next_finalize_at != std::chrono::steady_clock::time_point{};
}

bool public_host_runtime_t::relocation_target_terminal (const relocation_target_attempt_t &attempt)
{
    return attempt.terminal && attempt.terminal->task ().await_ready ();
}

std::optional<public_host_runtime_t::relocation_target_attempt_t>
public_host_runtime_t::take_relocation_target_discard (relocation_target_attempt_t &attempt)
{
    if (attempt.targets.empty ())
        return std::nullopt;
    auto terminal = attempt.terminal;
    auto discarded = std::move (attempt);
    // Keep the same request identity and completion while cleanup runs.
    attempt = relocation_target_attempt_t{};
    attempt.prepare = discarded.prepare;
    attempt.pending = discarded.pending;
    attempt.restore_identity = discarded.restore_identity;
    attempt.terminal = std::move (terminal);
    return discarded;
}

void public_host_runtime_t::discard_unverified_relocation_targets (
  const std::vector<relocation_attempt_key_t> &observed)
{
    if (observed.empty ()
        && !_options.mesh.shutdown_admission_seal->load (std::memory_order_acquire))
        return;
    std::vector<relocation_target_attempt_t> discarded;
    _relocation_session_terminal_lane
      .run ([&] {
          const auto sealed =
            _options.mesh.shutdown_admission_seal->load (std::memory_order_acquire);
          for (auto &[key, attempt] : _relocation_target_attempts) {
              if (attempt.cutover_received)
                  continue;
              if (sealed || std::find (observed.begin (), observed.end (), key) != observed.end ())
                  if (auto staging = take_relocation_target_discard (attempt))
                      discarded.push_back (std::move (*staging));
          }
      })
      .get ();
    discard_relocation_target_attempts (std::move (discarded));
}

task_t<void> public_host_runtime_t::seal_relocation_targets ()
{
    std::vector<std::shared_ptr<std::stop_source>> cancellations;
    std::vector<pending_relocation_assembly_t> assemblies;
    auto terminals =
      _relocation_session_terminal_lane
        .run ([this, &cancellations, &assemblies] {
            _options.mesh.shutdown_admission_seal->store (true, std::memory_order_release);
            lower_relocation_next_due_on_lane (relocation_due_now);
            std::vector<task_t<void>> result;
            for (const auto &[key, attempt] : _relocation_target_attempts) {
                if (attempt.terminal)
                    result.push_back (attempt.terminal->task ());
                if (attempt.pending)
                    cancellations.push_back (attempt.pending->cancellation);
            }
            for (auto &[key, assembly] : _relocation_assemblies)
                assemblies.push_back (std::move (assembly));
            _relocation_assemblies.clear ();
            return result;
        })
        .get ();
    for (const auto &cancellation : cancellations)
        cancellation->request_stop ();
    for (const auto &assembly : assemblies)
        reply_relocation_assembly_failure (
          assembly, static_cast<protocol::framework_error_code> (
                      messaging::request_failure_mapper_t{}.target_failure_code (
                        framework_error_kind_t::shutting_down)));
    auto activations =
      _lifecycle_configuration_lane
        .run ([this] { return std::exchange (_instance_spot_activation_admissions, {}); })
        .get ();
    for (const auto &[spot_id, operation] : activations)
        operation->admission.complete (result_t<instance_activation_admission_t>::failure (
          framework_error_kind_t::shutting_down, "Instance Spot activation target stopped"));
    discard_unverified_relocation_targets ();
    for (auto &terminal : terminals)
        co_await terminal;
    for (const auto &[spot_id, operation] : activations)
        co_await operation->terminal.task ();
}

void public_host_runtime_t::discard_relocation_target_attempts (
  std::vector<relocation_target_attempt_t> attempts,
  std::optional<framework_exception_t> failure) noexcept
{
    for (auto &attempt : attempts) {
        _spot_actor_index_lane
          .run ([&] {
              for (const auto &target : attempt.targets) {
                  if (target.kind != stateful::object_kind_t::actor)
                      continue;
                  const auto current = _actors.find (target.key);
                  if (current != _actors.end () && current->second.second == target)
                      _actors.erase (current);
              }
          })
          .get ();
        if (attempt.authority_fence && _aggregate_relocation_authority) {
            try {
                _aggregate_relocation_authority->abort (*attempt.authority_fence);
            }
            catch (...) {
                _options.mesh.runtime_failures->report (std::current_exception ());
            }
        }
        for (const auto &object : attempt.wire_objects) {
            try {
                (void) _relocation_wire->unregister_target (
                  attempt.prepare.relocation, attempt.prepare.target_attempt_generation, object);
            }
            catch (...) {
                _options.mesh.runtime_failures->report (std::current_exception ());
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
            _options.mesh.runtime_failures->report (std::current_exception ());
        }
        if (_stateful_dispatch)
            for (const auto &target : attempt.targets) {
                try {
                    (void) _stateful_dispatch->discard_pending (target);
                }
                catch (...) {
                    _options.mesh.runtime_failures->report (std::current_exception ());
                }
            }
        const relocation_attempt_key_t key{attempt.prepare.relocation.high,
                                           attempt.prepare.relocation.low,
                                           attempt.prepare.target_attempt_generation};
        if (attempt.pending && !attempt.pending->ready->task ().await_ready ()) {
            const auto stopped = framework_exception_t (
              framework_error_kind_t::shutting_down, "Relocation target stopped before readiness");
            const auto &cause = failure ? *failure : stopped;
            trace_mesh_host ("relocation-assembly-failed", cause.what ());
            reply_relocation_assembly_failure (
              *attempt.pending, static_cast<protocol::framework_error_code> (
                                  messaging::request_failure_mapper_t{}.target_failure_code (
                                    cause.kind (), detail::failure_code (cause))));
        }
        _relocation_session_terminal_lane
          .run ([&] {
              const auto found = _relocation_target_attempts.find (key);
              if (found != _relocation_target_attempts.end ()
                  && found->second.terminal == attempt.terminal)
                  _relocation_target_attempts.erase (found);
          })
          .get ();
        attempt.terminal->complete (result_t<void>::success ());
    }
}

stateful::relocation_authority_fence_t
public_host_runtime_t::relocation_target_fence (const relocation_target_attempt_t &attempt)
{
    const auto &primary = stateful::relocation_primary (attempt.sources);
    const auto &coordinator = attempt.prepare.coordinator;
    return {primary.kind,
            primary.key,
            coordinator.expected_authority_store_version,
            {coordinator.owner_id, static_cast<std::int64_t> (coordinator.lease_generation)},
            {attempt.prepare.target.target_owner_id,
             static_cast<std::int64_t> (attempt.prepare.target.target_owner_lease_generation)},
            attempt.prepare.relocation};
}

public_host_runtime_t::relocation_target_settlement_t
public_host_runtime_t::observe_relocation_target (
  const stateful::relocation_authority_fence_t &fence) const noexcept
{
    if (!_relocation_authority)
        return relocation_target_settlement_t::retry;
    try {
        switch (_relocation_authority->observe_relocation (fence)) {
            case stateful::relocation_authority_t::target_committed:
                return relocation_target_settlement_t::committed;
            case stateful::relocation_authority_t::source_preserved:
                return relocation_target_settlement_t::discard;
            default:
                break;
        }
        // An earlier read naming the source alone never ends staging; the
        // target lease loss does (01 §10).
        const auto target_live = _relocation_authority->owner_lease_live (fence.target_owner);
        return target_live && !*target_live ? relocation_target_settlement_t::discard
                                            : relocation_target_settlement_t::retry;
    }
    catch (...) {
        return relocation_target_settlement_t::retry;
    }
}

void public_host_runtime_t::poll_relocation_target_attempts ()
{
    discard_unverified_relocation_targets ();
    /* 28/52: command 44 (session_relocation_route) is a one-way send,
     * submitted exactly once per route -- there is no periodic re-select
     * and resend of an incomplete route here. Attempts with a verified
     * cutover submit their target CAS; the others only read the authority
     * that ends their staging (01 §10). */
    std::vector<relocation_attempt_key_t> pending;
    std::vector<std::pair<relocation_attempt_key_t, stateful::relocation_authority_fence_t>>
      unverified;
    std::size_t cutover_warnings = 0;
    const auto now = std::chrono::steady_clock::now ();
    _relocation_session_terminal_lane
      .run ([&] {
          for (auto &[key, attempt] : _relocation_target_attempts) {
              if (relocation_target_terminal (attempt) || !relocation_target_wire_ready (attempt)
                  || attempt.next_finalize_at > now)
                  continue;
              if (attempt.cutover_received) {
                  pending.push_back (key);
                  continue;
              }
              /* RelocationCutoverWaitTimeout is a Warning threshold only
               * (28 §4.4): it never starts the CAS or dispatch. */
              if (!attempt.cutover_warned) {
                  attempt.cutover_warned = true;
                  ++cutover_warnings;
                  if (_relocation_metrics && !attempt.sources.empty ())
                      _relocation_metrics->timeout (attempt.sources.front ().kind);
              }
              set_relocation_finalize_at_on_lane (attempt,
                                                  now + dispatch_limits::management_retry_interval);
              unverified.emplace_back (key, relocation_target_fence (attempt));
          }
      })
      .get ();
    for (std::size_t index = 0; index != cutover_warnings; ++index) {
        trace_mesh_host ("relocation-cutover-timeout", "warning=cutover_timeout");
        if (_relocation_target_metrics.cutover_timeout)
            _relocation_target_metrics.cutover_timeout ();
    }
    std::vector<relocation_attempt_key_t> observed;
    for (const auto &[key, fence] : unverified)
        if (observe_relocation_target (fence) == relocation_target_settlement_t::discard)
            observed.push_back (key);
    discard_unverified_relocation_targets (observed);
    for (const auto &key : pending)
        (void) try_finalize_relocation_target (key);
}

void public_host_runtime_t::flush_pending_session_relocation_seals ()
{
    std::vector<session_relocation_key_t> pending;
    std::vector<session_relocation_key_t> expired;
    const auto now = std::chrono::steady_clock::now ();
    const auto consumed =
      _relocation_session_terminal_lane
        .run ([&] {
            std::map<session_relocation_key_t, protocol::session_relocation_seal_t> records;
            for (const auto &[key, record] : _session_seal_terminals)
                if (record.consumed
                    && record.sealed.seal_result
                         == protocol::session_relocation_seal_result_t::sealed)
                    records.emplace (key, record.seal);
            return records;
        })
        .get ();
    // Read each registry fact outside the terminal turn. Only the captured
    // records may be pruned with it; a successor record needs its own observation.
    std::map<std::string, std::optional<stateful::stream_binding_t>> current_bindings;
    for (const auto &[key, seal] : consumed) {
        const auto [current, inserted] = current_bindings.try_emplace (seal.actor.actor_id);
        if (inserted)
            current->second = _sessions.current_binding (seal.actor.actor_id);
    }
    _relocation_session_terminal_lane
      .run ([&] {
          for (auto it = _session_seal_terminals.begin (); it != _session_seal_terminals.end ();) {
              const auto &[key, record] = *it;
              if (record.sealed.seal_result
                  == protocol::session_relocation_seal_result_t::binding_absent) {
                  if (record.expires_at <= now) {
                      it = _session_seal_terminals.erase (it);
                      continue;
                  }
              } else if (record.consumed) {
                  const auto captured = consumed.find (key);
                  if (captured != consumed.end () && captured->second == record.seal) {
                      const auto &current = current_bindings.at (record.seal.actor.actor_id);
                      if (!current || current->binding_generation != record.seal.binding_generation
                          || current->connection.connection_id
                               != stateful::stream_session_registry_t::connection_key (
                                 zlink::routing_id_t::from (record.seal.session_routing_id))) {
                          it = _session_seal_terminals.erase (it);
                          continue;
                      }
                  }
              } else if (record.expires_at <= now)
                  expired.push_back (key);
              else if (!record.ready)
                  pending.push_back (key);
              ++it;
          }
      })
      .get ();
    for (const auto &key : expired) {
        stateful::stream_barrier_t barrier;
        std::vector<session_seal_local_completion_t> local_completions;
        const auto expired_current =
          _relocation_session_terminal_lane
            .run ([&] {
                const auto found = _session_seal_terminals.find (key);
                if (found == _session_seal_terminals.end () || found->second.consumed
                    || found->second.expires_at > now)
                    return false;
                barrier = found->second.barrier;
                local_completions = std::move (found->second.local_completions);
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
        const auto completed =
          _relocation_session_terminal_lane
            .run ([&] {
                const auto found = _session_seal_terminals.find (key);
                if (found == _session_seal_terminals.end () || found->second.consumed)
                    return false;
                found->second.ready = true;
                sealed = found->second.sealed;
                target = found->second.response_routing_id;
                local_completions = std::move (found->second.local_completions);
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
    const auto routes_current =
      _relocation_session_terminal_lane
        .run ([&] {
            const auto found = _relocation_target_attempts.find (key);
            if (found == _relocation_target_attempts.end ()
                || found->second.authority_committed_at == std::chrono::steady_clock::time_point{})
                return false;
            for (std::size_t index = 0; index != found->second.session_routes.size (); ++index) {
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
        const auto route_current =
          _relocation_session_terminal_lane
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
                const auto journal =
                  _session_journal_terminals.find (session_relocation_key (state.route));
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
                _options.mesh.runtime_failures->report (std::current_exception ());
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

bool public_host_runtime_t::submit_relocation_target_authority (
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
                && _actor_relocation_authority_resolver) {
                const auto spot = _actor_relocation_authority_resolver (attempt.targets.front ());
                if (spot) {
                    auto payload = *spot;
                    payload.owner_id = target_owner.owner_id;
                    payload.owner_lease_generation =
                      static_cast<std::uint64_t> (target_owner.lease_generation);
                    payload.node_generation = attempt.prepare.target.target_node_generation;
                    target_application_payload = runtime::encode_actor_authority_payload (payload);
                }
            }
            const auto published = _relocation_authority->publish (
              attempt.sources.front (), attempt.targets.front (), target_owner, target_placement,
              attempt.restore_identity.reference, attempt.restore_identity.checksum_crc32c,
              attempt.restore_identity.inventory_digest, std::move (target_application_payload),
              attempt.prepare.coordinator.expected_authority_store_version,
              attempt.prepare.relocation,
              {attempt.prepare.coordinator.owner_id,
               static_cast<std::int64_t> (attempt.prepare.coordinator.lease_generation)});
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

        if (!_aggregate_relocation_authority)
            return false;
        if (!attempt.authority_fence) {
            const auto prepared = _aggregate_relocation_authority->prepare (
              attempt.sources, attempt.targets.front ().node_id, target_owner,
              attempt.restore_identity.reference, attempt.restore_identity.checksum_crc32c,
              attempt.restore_identity.inventory_digest,
              attempt.prepare.coordinator.expected_authority_store_version);
            if (prepared.status != stateful::aggregate_publish_status_t::prepared)
                return false;
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
        /* The committed aggregate record identifies this move; adopt the
         * Store-assigned owner generation of every staged participant. */
        const auto committed = _aggregate_relocation_authority->commit (*attempt.authority_fence);
        if (committed.status != stateful::aggregate_publish_status_t::committed)
            return false;
        std::vector<stateful::object_ref_t> adopted;
        adopted.reserve (attempt.targets.size ());
        for (std::size_t index = 0; index != attempt.targets.size (); ++index) {
            const auto &staged = attempt.targets[index];
            const auto placement = std::find_if (
              committed.current.begin (), committed.current.end (), [&] (const auto &candidate) {
                  return candidate.target.kind == staged.kind && candidate.target.key == staged.key;
              });
            if (placement == committed.current.end ()
                || placement->target.object_generation != staged.object_generation
                || placement->target.mesh_name != staged.mesh_name
                || placement->target.node_id != staged.node_id
                || placement->target.authority_owner_generation
                     <= attempt.sources[index].authority_owner_generation
                || placement->target_owner.owner_id != target_owner.owner_id
                || placement->target_owner.lease_generation != target_owner.lease_generation)
                return false;
            if (placement->target != staged
                && _objects.reconcile_relocation_restore_authority (staged, placement->target,
                                                                    attempt.restore_identity)
                     != stateful::stateful_error_t::none)
                return false;
            adopted.push_back (placement->target);
        }
        attempt.targets = std::move (adopted);
        return true;
    }
    catch (...) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        return false;
    }
}

public_host_runtime_t::relocation_target_settlement_t
public_host_runtime_t::commit_relocation_target_authority (
  relocation_target_attempt_t &attempt) noexcept
{
    /* The original NewOwner CAS (the SpotWide whole-unit batch) with the same
     * RelocationId and expected StoreVersion; a CAS that does not commit is
     * settled by the Store reading, never by a count or a timer (01 §10). */
    if (submit_relocation_target_authority (attempt))
        return relocation_target_settlement_t::committed;
    return observe_relocation_target (relocation_target_fence (attempt))
               == relocation_target_settlement_t::discard
             ? relocation_target_settlement_t::discard
             : relocation_target_settlement_t::retry;
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
    const auto attempt_current =
      _relocation_session_terminal_lane
        .run ([&] {
            const auto found = _relocation_target_attempts.find (key);
            if (found == _relocation_target_attempts.end ())
                return false;
            if (relocation_target_terminal (found->second)) {
                attempt = found->second;
            } else {
                const auto now = std::chrono::steady_clock::now ();
                if (!found->second.cutover_received || !relocation_target_wire_ready (found->second)
                    || now < found->second.next_finalize_at)
                    return false;
                attempt = found->second;
                set_relocation_finalize_at_on_lane (
                  found->second, now + dispatch_limits::management_retry_interval);
            }
            return true;
        })
        .get ();
    if (!attempt_current)
        return false;
    if (relocation_target_terminal (attempt))
        return true;

    const auto discard_attempt = [&] {
        std::vector<relocation_target_attempt_t> discarded;
        _relocation_session_terminal_lane
          .run ([&] {
              const auto found = _relocation_target_attempts.find (key);
              if (found == _relocation_target_attempts.end ()
                  || relocation_target_terminal (found->second))
                  return;
              if (auto staging = take_relocation_target_discard (found->second))
                  discarded.push_back (std::move (*staging));
          })
          .get ();
        discard_relocation_target_attempts (std::move (discarded));
    };
    const auto settlement = commit_relocation_target_authority (attempt);
    if (settlement == relocation_target_settlement_t::discard) {
        discard_attempt ();
        return false;
    }
    if (settlement != relocation_target_settlement_t::committed)
        return false;
    if (!adopt_committed_session_route_authorities (attempt))
        return false;

    /* S2 (owner CAS confirmed): stamp once, on the first tick that
     * observes the authority commit, so a retried finalize does not push
     * the target_resume window forward. */
    const auto stamped =
      _relocation_session_terminal_lane
        .run ([&] {
            const auto found = _relocation_target_attempts.find (key);
            if (found == _relocation_target_attempts.end ())
                return false;
            if (found->second.prepare != attempt.prepare
                || found->second.session_routes.size () != attempt.session_routes.size ())
                return false;
            found->second.sources = attempt.sources;
            found->second.targets = attempt.targets;
            found->second.authority_fence = attempt.authority_fence;
            for (std::size_t index = 0; index != attempt.session_routes.size (); ++index) {
                if (found->second.session_routes[index].send_attempted)
                    return false;
                found->second.session_routes[index].route = attempt.session_routes[index].route;
            }
            if (found->second.authority_committed_at == std::chrono::steady_clock::time_point{})
                found->second.authority_committed_at = std::chrono::steady_clock::now ();
            attempt.authority_committed_at = found->second.authority_committed_at;
            return true;
        })
        .get ();
    if (!stamped)
        return false;

    // Raw identities were installed during target preparation. Reflect the
    // Store-assigned authority before lifecycle opens; User-Spot Join also
    // adopts that authority in its prepared application Actor.
    const auto join_prepare = _actor_join_relocation_prepare_validator
                                ? _actor_join_relocation_prepare_validator (attempt.prepare)
                                : std::optional<bool>{};
    const auto canonical_user_spot_join = join_prepare && *join_prepare;
    for (const auto &target : attempt.targets) {
        if (target.kind != stateful::object_kind_t::actor)
            continue;
        if (advance_local_actor_authority (target) != stateful::stateful_error_t::none
            || (canonical_user_spot_join
                && (!_actor_join_committed_authority_adopter
                    || !_actor_join_committed_authority_adopter (
                      target, attempt.prepare.target.target_node_generation,
                      attempt.prepare.target.target_owner_lease_generation))))
            return false;
    }
    for (const auto &route_state : attempt.session_routes) {
        const auto &route = route_state.route;
        if (!_bound_session_operations.prepare_relocation_target_route
            || !_bound_session_operations.prepare_relocation_target_route (
              route, attempt.prepare.target.target_owner_lease_generation))
            return false;
    }

    try {
        // Membership §4.2 steps 7-8: lifecycle and queue opening precede command 44.
        const auto committed = attempt.targets.size () == 1
                                 ? _objects.commit_relocation_restore (attempt.targets.front (),
                                                                       attempt.restore_identity)
                                 : _objects.commit_relocation_restore_aggregate (
                                     attempt.targets, attempt.restore_identity);
        if (committed != stateful::stateful_error_t::none
            && committed != stateful::stateful_error_t::already_exists) {
            discard_attempt ();
            return false;
        }
    }
    catch (...) {
        discard_attempt ();
        throw;
    }

    for (const auto &object : attempt.wire_objects)
        (void) _relocation_wire->unregister_target (
          attempt.prepare.relocation, attempt.prepare.target_attempt_generation, object);

    const auto finalized = _relocation_session_terminal_lane
                             .run ([&] {
                                 const auto found = _relocation_target_attempts.find (key);
                                 if (found == _relocation_target_attempts.end ())
                                     return std::shared_ptr<task_completion_source_t<void>>{};
                                 return found->second.terminal;
                             })
                             .get ();
    if (!finalized)
        return false;
    /* Metrics 25 §"zlink.relocation": target_resume is the target-local S2
     * (owner CAS confirmed) -> dispatch-open duration, emitted once on the
     * tick that completes the target terminal. */
    if (_relocation_metrics && !attempt.sources.empty ()
        && attempt.authority_committed_at != std::chrono::steady_clock::time_point{})
        _relocation_metrics->resume (
          attempt.sources.front ().kind,
          std::chrono::duration<double> (std::chrono::steady_clock::now ()
                                         - attempt.authority_committed_at)
            .count ());
    if (_relocation_target_metrics.target_resume_seconds
        && attempt.authority_committed_at != std::chrono::steady_clock::time_point{}) {
        const auto elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now ()
                                                            - attempt.authority_committed_at);
        _relocation_target_metrics.target_resume_seconds (elapsed.count ());
    }
    /* Relocation flow §4.6: command 44 follows target dispatch opening.
     * A completion on another execution resource may run before this stack
     * returns (Submit and completion §11), so send completion cannot order
     * the Session owner's route commit after the target becomes Ready. */
    start_relocation_session_route_submission (key);
    finalized->complete (result_t<void>::success ());
    for (const auto &target : attempt.targets) {
        auto notified = std::make_shared<task_t<bool>> (notify_stateful_owner (target));
        detail::observe_task_completion (
          *notified, [self = shared_from_this (), notified] (const result_t<bool> &result) {
              self->_options.mesh.runtime_failures->capture ([&] { (void) result.value (); });
          });
    }
    return true;
}

void public_host_runtime_t::reply_relocation_assembly_failure (
  const pending_relocation_assembly_t &pending, protocol::framework_error_code code)
{
    pending.ready->complete (detail::result_access_t::failure<void> (detail::with_failure_code (
      framework_exception_t (
        messaging::request_failure_mapper_t{}.failure_code_kind (static_cast<std::uint32_t> (code)),
        "Relocation target preparation failed"),
      static_cast<std::uint32_t> (code))));
    (void) _transport->reply_relocation_failed (
      pending.request, protocol::relocation_failed_t{
                         pending.prepare.relocation, pending.prepare.target_attempt_generation,
                         pending.prepare.coordinator, pending.prepare.target,
                         pending.prepare.object, protocol::relocation_role_t::target,
                         messaging::request_failure_mapper_t{}.target_failure_code (
                           messaging::request_failure_mapper_t{}.failure_code_kind (
                             static_cast<std::uint32_t> (code)),
                           static_cast<std::uint32_t> (code))});
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
            _options.mesh.runtime_failures->report (std::current_exception ());
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
            _options.mesh.runtime_failures->report (std::current_exception ());
        }
    }
}

task_t<bool> public_host_runtime_t::restore_relocation_assembly (
  std::shared_ptr<const pending_relocation_assembly_t> pending,
  std::shared_ptr<const relocation_assembly_staging_t> staging)
{
    stateful::stateful_error_t restored = stateful::stateful_error_t::conflict;
    std::optional<framework_exception_t> failure;
    try {
        // Location §9.3: relocation preserves the source recorded at first acceptance.
        const auto previous_actor_identities = co_await _spot_actor_index_lane.run_task ([&] {
            std::vector<stateful::object_ref_t> previous;
            for (const auto &target : staging->targets) {
                if (target.kind != stateful::object_kind_t::actor)
                    continue;
                const auto current = _actors.find (target.key);
                if (current != _actors.end () && current->second.second != target)
                    previous.push_back (current->second.second);
            }
            return previous;
        });
        std::optional<stateful::object_ref_t> actor_target_spot;
        if (staging->targets.size () == 1
            && staging->targets.front ().kind == stateful::object_kind_t::actor
            && _actor_relocation_authority_resolver) {
            const auto spot = _actor_relocation_authority_resolver (staging->targets.front ());
            if (spot) {
                actor_target_spot = stateful::object_ref_t{
                  stateful::object_kind_t::user_spot,  spot->current_spot_id,
                  spot->current_spot_generation,       0,
                  staging->targets.front ().mesh_name, staging->targets.front ().node_id};
            }
        }
        auto restored_state = staging->frozen;
        std::vector<std::tuple<stateful::object_ref_t, protocol::relocation_data_t, std::uint64_t>>
          saved_application;
        for (std::size_t index = 0; index < restored_state.size (); ++index) {
            if (staging->targets[index].kind != stateful::object_kind_t::actor)
                continue;
            for (const auto &turn : restored_state[index].pending_application)
                saved_application.emplace_back (
                  staging->targets[index],
                  protocol::relocation_data_t{
                    pending->prepare.relocation, pending->prepare.target_attempt_generation,
                    pending->prepare.coordinator, protocol::relocation_role_t::source,
                    staging->wire_objects[index], turn.frozen_record.value ()},
                  index + 1);
            restored_state[index].pending_application.clear ();
        }
        restored = co_await (
          staging->targets.size () == 1
            ? _objects.restore_relocation (
                restored_state.front (), staging->targets.front (), staging->restore_identity,
                pending->cancellation->get_token (), std::move (actor_target_spot))
            : _objects.restore_relocation_aggregate (restored_state, staging->targets,
                                                     staging->restore_identity,
                                                     pending->cancellation->get_token ()));
        // The Kernel restores Actor state; canonical raw intake installs each
        // saved payload together with its original terminal, before Ready.
        if (restored == stateful::stateful_error_t::none)
            for (const auto &[target, data, participant_id] : saved_application)
                if (!co_await stage_relocation_record (target, data, participant_id))
                    throw framework_exception_t (framework_error_kind_t::internal_failure,
                                                 "Restored Actor application intake failed");
        if (restored == stateful::stateful_error_t::none
            || restored == stateful::stateful_error_t::already_exists) {
            // Restore owns the retirement of a previous moving source record.
            // Reflect that accepted retirement in the same identity registry;
            // a different identity installed during restore remains untouched.
            co_await _spot_actor_index_lane.run_task ([&] {
                for (const auto &previous : previous_actor_identities) {
                    const auto current = _actors.find (previous.key);
                    if (current != _actors.end () && current->second.second == previous)
                        _actors.erase (current);
                }
            });
            co_return true;
        }
    }
    catch (const framework_exception_t &error) {
        failure = error;
    }
    catch (const std::exception &error) {
        failure.emplace (framework_error_kind_t::internal_failure, error.what ());
    }
    catch (...) {
        failure.emplace (framework_error_kind_t::internal_failure, "Relocation restore failed");
    }
    if (restored == stateful::stateful_error_t::none) {
        relocation_target_attempt_t owned;
        owned.terminal = std::make_shared<task_completion_source_t<void>> ();
        owned.prepare = pending->prepare;
        owned.restore_identity = staging->restore_identity;
        owned.targets = staging->targets;
        owned.wire_objects = staging->wire_objects;
        std::vector<relocation_target_attempt_t> discarded;
        discarded.push_back (std::move (owned));
        discard_relocation_target_attempts (std::move (discarded));
    } else
        discard_relocation_assembly_staging (*pending, *staging);
    const messaging::request_failure_mapper_t mapper;
    if (!failure) {
        const auto [terminal, code] = stateful_failure_pair (restored);
        failure = mapper.reply_header_exception (terminal, code, "Relocation restore");
    }
    reply_relocation_assembly_failure (
      *pending, static_cast<protocol::framework_error_code> (
                  mapper.target_failure_code (failure->kind (), detail::failure_code (*failure))));
    co_return false;
}

task_t<bool>
public_host_runtime_t::submit_relocation_ready_reply (protocol::relocation_prepare_t prepare,
                                                      const mesh::service_mailbox_record_t &request)
{
    auto submit = co_await _transport->prepare_relocation_ready_reply (
      request, protocol::relocation_ready_t{prepare.relocation, prepare.target_attempt_generation,
                                            prepare.coordinator, prepare.target, prepare.object,
                                            protocol::relocation_role_t::target});
    const relocation_attempt_key_t key{prepare.relocation.high, prepare.relocation.low,
                                       prepare.target_attempt_generation};
    co_return co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (_options.mesh.shutdown_admission_seal->load (std::memory_order_acquire)
            || found == _relocation_target_attempts.end () || found->second.prepare != prepare
            || !found->second.pending || found->second.targets.empty ())
            throw framework_exception_t (framework_error_kind_t::shutting_down,
                                         "Relocation target stopped before readiness reply");
        const auto sent = submit && submit ();
        if (sent && !relocation_target_wire_ready (found->second))
            set_relocation_finalize_at_on_lane (found->second, std::chrono::steady_clock::now ()
                                                                 + _relocation_cutover_wait);
        return sent;
    });
}

void public_host_runtime_t::observe_relocation_ready (
  std::shared_ptr<task_completion_source_t<void>> ready,
  mesh::service_mailbox_record_t request,
  protocol::relocation_prepare_t prepare)
{
    auto ready_task = ready->task ();
    detail::observe_task_completion (
      ready_task, [self = shared_from_this (),
                   request = std::make_shared<mesh::service_mailbox_record_t> (std::move (request)),
                   prepare = std::move (prepare)] (const result_t<void> &result) {
          const auto failed = [self, request, prepare] (const framework_exception_t &error) {
              (void) self->_transport->reply_relocation_failed (
                *request,
                protocol::relocation_failed_t{
                  prepare.relocation, prepare.target_attempt_generation, prepare.coordinator,
                  prepare.target, prepare.object, protocol::relocation_role_t::target,
                  messaging::request_failure_mapper_t{}.target_failure_code (
                    error.kind (), detail::failure_code (error))});
          };
          if (!result) {
              failed (result.error ()
                        ? *result.error ()
                        : framework_exception_t (framework_error_kind_t::internal_failure,
                                                 "Relocation readiness failed without a cause"));
              return;
          }
          auto submitted = std::make_shared<task_t<bool>> (
            self->submit_relocation_ready_reply (prepare, *request));
          detail::observe_task_completion (
            *submitted, [submitted, failed] (const result_t<bool> &sent) {
                if (!sent || !sent.value ())
                    failed (sent.error () ? *sent.error ()
                                          : framework_exception_t (
                                              framework_error_kind_t::unavailable,
                                              "Relocation readiness reply could not be submitted"));
            });
      });
}

void public_host_runtime_t::request_peer_reconcile (
  const std::vector<std::uint8_t> &target,
  std::uint64_t generation,
  const std::function<void (const std::vector<std::uint8_t> &)> &reconcile)
{
    const auto local = _transport->topology ().local_descriptor ();
    if (target == local.node_routing_id && generation == local.lifecycle_generation)
        return;
    if (!peer_route_ready (_transport->topology ().peer (target), generation) && reconcile)
        reconcile (target);
}

task_t<void> public_host_runtime_t::await_relocation_session_routes (
  std::shared_ptr<const pending_relocation_assembly_t> pending,
  const std::vector<protocol::session_relocation_route_t> &routes)
{
    const auto local = _transport->topology ().local_descriptor ();
    const auto reconcile = _transport->peer_reconcile ();
    using route_identity_t = std::pair<std::vector<std::uint8_t>, std::uint64_t>;
    std::map<route_identity_t, mesh::service_topology_registry_t::peer_observer_t> observers;
    for (const auto &route : routes) {
        if (route.session_owner_node_routing_id == local.node_routing_id
            && route.session_owner_node_generation == local.lifecycle_generation)
            continue;
        const route_identity_t identity{route.session_owner_node_routing_id,
                                        route.session_owner_node_generation};
        if (observers.contains (identity))
            continue;
        auto observer = _transport->topology ().observe_peer (identity.first);
        request_peer_reconcile (identity.first, identity.second, reconcile);
        observers.emplace (identity, std::move (observer));
    }
    auto control = std::make_shared<task_completion_source_t<void>> ();
    auto controlled = control->task ();
    std::stop_callback stopped (
      pending->cancellation->get_token (), [weak = std::weak_ptr{control}] {
          if (const auto signal = weak.lock ())
              signal->complete (result_t<void>::failure (framework_error_kind_t::shutting_down,
                                                         "Relocation target stopped"));
      });
    struct deadline_registration_t
    {
        std::shared_ptr<detail::deadline_state_t> state =
          std::make_shared<detail::deadline_state_t> ();
        ~deadline_registration_t ()
        {
            state->cancelled.store (true, std::memory_order_release);
            detail::deadline_scheduler_t::instance ().wake ();
        }
    } deadline;
    const auto at = messaging::request_deadline_from_unix_ms (pending->prepare.deadline_unix_ms);
    detail::deadline_scheduler_t::instance ().schedule (
      at, deadline.state, [weak = std::weak_ptr{control}] {
          if (const auto signal = weak.lock ())
              signal->complete (result_t<void>::failure (framework_error_kind_t::deadline_exceeded,
                                                         "Relocation source deadline expired"));
      });
    for (;;) {
        auto wake = std::make_shared<task_completion_source_t<void>> ();
        auto notified = wake->task ();
        const auto notify = [weak = std::weak_ptr{wake}] (const result_t<void> &result) {
            if (const auto signal = weak.lock ())
                signal->complete (result);
        };
        struct subscriptions_t
        {
            std::stop_source owner;
            ~subscriptions_t () { owner.request_stop (); }
        } subscriptions;
        bool all_ready = true;
        {
            detail::ambient_context_scope_t scope (nullptr, nullptr,
                                                   subscriptions.owner.get_token (), false, true);
            detail::observe_task_completion (controlled, notify);
            for (const auto &[identity, observer] : observers) {
                auto observation = observer ();
                if (peer_route_ready (observation.peer, identity.second))
                    continue;
                all_ready = false;
                detail::observe_task_completion (observation.changed, notify);
            }
        }
        if (controlled.await_ready ())
            co_await controlled;
        if (std::chrono::steady_clock::now () >= at)
            throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                         "Relocation source deadline expired");
        if (all_ready)
            co_return;
        co_await notified;
    }
}

task_t<void> public_host_runtime_t::finish_relocation_assembly (
  relocation_attempt_key_t key,
  std::shared_ptr<const pending_relocation_assembly_t> pending,
  std::shared_ptr<relocation_assembly_staging_t> staging,
  std::vector<std::pair<std::string, stateful::object_ref_t>> consumed)
{
    const auto registered = co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        return !_options.mesh.shutdown_admission_seal->load (std::memory_order_acquire)
               && found != _relocation_target_attempts.end () && found->second.pending == pending;
    });
    if (!registered) {
        rollback_actor_join_recoveries (consumed);
        discard_relocation_assembly_staging (*pending, *staging);
        co_await reply_relocation_assembly_failure (
          key, pending,
          static_cast<protocol::framework_error_code> (
            messaging::request_failure_mapper_t{}.target_failure_code (
              framework_error_kind_t::shutting_down)));
        co_return;
    }
    bool restored = false;
    std::optional<framework_exception_t> failure;
    try {
        restored = co_await restore_relocation_assembly (pending, staging);
        if (restored) {
            co_await await_relocation_session_routes (pending, staging->session_routes);
            if (co_await activate_relocation_assembly (key, pending, staging))
                co_return;
            failure.emplace (framework_error_kind_t::unavailable,
                             "Relocation readiness reply could not be submitted");
        }
    }
    catch (const framework_exception_t &error) {
        failure = error;
    }
    catch (const std::exception &error) {
        failure.emplace (framework_error_kind_t::internal_failure, error.what ());
    }
    rollback_actor_join_recoveries (consumed);
    if (!restored) {
        auto code = protocol::framework_error_code::requestFailed;
        if (failure) {
            trace_mesh_host ("relocation-assembly-failed", failure->what ());
            code = static_cast<protocol::framework_error_code> (
              messaging::request_failure_mapper_t{}.target_failure_code (
                failure->kind (), detail::failure_code (*failure)));
            if (!pending->ready->task ().await_ready ())
                discard_relocation_assembly_staging (*pending, *staging);
        }
        co_await reply_relocation_assembly_failure (key, pending, code);
        co_return;
    }
    auto discarded = co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (found == _relocation_target_attempts.end () || found->second.pending != pending)
            return std::optional<relocation_target_attempt_t>{};
        if (found->second.targets.empty () && !found->second.restore_identity.reference.empty ())
            return std::optional<relocation_target_attempt_t>{};
        if (restored && found->second.targets.empty ()) {
            found->second.restore_identity = staging->restore_identity;
            found->second.sources = std::move (staging->sources);
            found->second.targets = std::move (staging->targets);
            found->second.wire_objects = std::move (staging->wire_objects);
        }
        if (!found->second.targets.empty ())
            return take_relocation_target_discard (found->second);
        return std::optional<relocation_target_attempt_t>{};
    });
    if (discarded) {
        std::vector<relocation_target_attempt_t> cleanup;
        cleanup.push_back (std::move (*discarded));
        discard_relocation_target_attempts (std::move (cleanup), std::move (failure));
    }
}

task_t<bool> public_host_runtime_t::activate_relocation_assembly (
  const relocation_attempt_key_t &key,
  std::shared_ptr<const pending_relocation_assembly_t> pending,
  std::shared_ptr<relocation_assembly_staging_t> staging)
{
    std::optional<task_t<bool>> register_indexes;
    co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (_options.mesh.shutdown_admission_seal->load (std::memory_order_acquire)
            || found == _relocation_target_attempts.end () || found->second.pending != pending
            || relocation_target_terminal (found->second))
            throw framework_exception_t (framework_error_kind_t::shutting_down,
                                         "Relocation target stopped before activation");
        auto &attempt = found->second;
        attempt.restore_identity = staging->restore_identity;
        attempt.sources = std::move (staging->sources);
        attempt.targets = std::move (staging->targets);
        attempt.wire_objects = std::move (staging->wire_objects);
        for (auto &route : staging->session_routes) {
            attempt.session_routes.emplace_back ();
            attempt.session_routes.back ().route = std::move (route);
        }
        // The attempt owns cleanup before any raw identity is registered.
        // Submit on this turn so a later discard queues its index removal
        // after registration. The index turn reads only its owned inputs.
        register_indexes.emplace (_spot_actor_index_lane.run_task (
          [this, targets = attempt.targets, wire_objects = attempt.wire_objects] {
              // Relocation §4.3/§4.5: prepare raw identities before Ready CAS.
              // The recovering gate still prevents application execution.
              // Validate the whole unit before inserting any Actor.
              for (const auto &target : targets) {
                  if (target.kind != stateful::object_kind_t::actor)
                      continue;
                  const auto current = _actors.find (target.key);
                  if (current != _actors.end () && current->second.second != target)
                      return false;
              }
              for (std::size_t index = 0; index != targets.size (); ++index) {
                  const auto &target = targets[index];
                  if (target.kind == stateful::object_kind_t::actor)
                      _actors.try_emplace (target.key, wire_objects[index].stable_type, target);
              }
              return true;
          }));
    });
    if (!co_await *register_indexes)
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "Relocation target Actor identity is already registered");
    if (!co_await submit_relocation_ready_reply (pending->prepare, pending->request))
        co_return false;
    pending->ready->complete (result_t<void>::success ());
    co_return true;
}

task_t<void> public_host_runtime_t::reply_relocation_assembly_failure (
  relocation_attempt_key_t key,
  std::shared_ptr<const pending_relocation_assembly_t> pending,
  protocol::framework_error_code code)
{
    const auto terminal = co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (found == _relocation_target_attempts.end () || found->second.pending != pending)
            return std::shared_ptr<task_completion_source_t<void>>{};
        return found->second.terminal;
    });
    if (!pending->ready->task ().await_ready ())
        reply_relocation_assembly_failure (*pending, code);
    co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (found != _relocation_target_attempts.end () && found->second.pending == pending)
            _relocation_target_attempts.erase (found);
    });
    if (terminal)
        terminal->complete (result_t<void>::success ());
}

void public_host_runtime_t::complete_relocation_assembly (
  const relocation_attempt_key_t &key, std::shared_ptr<pending_relocation_assembly_t> held_pending)
{
    auto &pending = *held_pending;
    // Verified identity and payload-integrity failures retain relocationDataLost.
    // Restore failures preserve their Framework kind and cause code instead.
    const auto reply_failure = [&] (protocol::framework_error_code code =
                                      protocol::framework_error_code::relocationDataLost) {
        auto failed = std::make_shared<task_t<void>> (
          reply_relocation_assembly_failure (key, held_pending, code));
        detail::observe_task_completion (
          *failed, [self = shared_from_this (), failed] (const result_t<void> &result) {
              self->_options.mesh.runtime_failures->capture ([&] { result.value (); });
          });
    };
    try {
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
        principal_identity.owner.node_id =
          std::string (pending.prepare.source_node_routing_id.begin (),
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
                if (row.owner.kind == principal_kind
                    && row.owner.key == principal_identity.owner.key) {
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
                } else if ((principal_kind == stateful::object_kind_t::user_spot
                            || principal_kind == stateful::object_kind_t::instance_spot)
                           && row.owner.kind == stateful::object_kind_t::actor
                           && row.spot_membership
                           && row.spot_membership->first == principal_identity.owner.key
                           && row.spot_membership->second
                                == principal_identity.owner.object_generation) {
                    if (row.owner.mesh_name.empty ())
                        row.owner.mesh_name = principal_identity.owner.mesh_name;
                    if (row.owner.node_id.empty ())
                        row.owner.node_id = principal_identity.owner.node_id;
                    inventory.push_back (std::move (row));
                }
            }
            if (inventory.size () != envelope->application_states.size ()) {
                trace_mesh_host (
                  "relocation-assembly-failed",
                  "stage=inventory-count derived=" + std::to_string (inventory.size ())
                    + " declared=" + std::to_string (envelope->application_states.size ()));
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
            if (saved.owner.authority_owner_generation
                == std::numeric_limits<std::uint64_t>::max ()) {
                reply_failure ();
                return;
            }
            protocol::relocation_object_t wire_object{kind, saved.stable_type, saved.owner.key,
                                                      saved.owner.object_generation,
                                                      saved.owner.authority_owner_generation};
            principal_found = principal_found || wire_object == pending.prepare.object;
            auto target = saved.owner;
            target.node_id = local.routing_id ().to_string ();
            // Hidden staging has no target-issued generation until Store CAS.
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

        for (const auto &binding : envelope->session_bindings) {
            if (binding.participant_id == 0 || binding.participant_id > frozen.size ()) {
                reply_failure ();
                return;
            }
            const auto &saved = frozen[binding.participant_id - 1];
            if (saved.owner.kind != stateful::object_kind_t::actor) {
                reply_failure ();
                return;
            }
            protocol::session_relocation_route_t route;
            route.relocation = pending.prepare.relocation;
            route.coordinator = pending.prepare.coordinator;
            route.sender_role = protocol::relocation_role_t::target;
            route.actor = {saved.owner.key, saved.owner.object_generation};
            route.session_owner_node_routing_id = binding.session_owner_node_rid;
            route.session_owner_node_generation = binding.session_owner_node_generation;
            route.session_owner_id = binding.session_owner_id;
            route.session_owner_lease_generation = binding.session_owner_lease_generation;
            route.session_routing_id = binding.session_rid;
            route.binding_generation = binding.binding_generation;
            route.route.action = protocol::session_relocation_route_action_t::commit;
            route.route.previous_authority_owner_generation =
              saved.owner.authority_owner_generation;
            route.route.target_node_routing_id = local.routing_id ().to_bytes ();
            route.route.target_node_generation = local.lifecycle_generation ();
            route.route.target_owner_lease_generation =
              pending.prepare.target.target_owner_lease_generation;
            staging.session_routes.push_back (std::move (route));
        }

        staging.restore_identity = {"direct:" + std::to_string (pending.prepare.relocation.high)
                                      + ":" + std::to_string (pending.prepare.relocation.low) + ":"
                                      + std::to_string (pending.prepare.target_attempt_generation),
                                    pending.prepare.payload_checksum_crc32c, inventory_digest};
        std::size_t registered_count = 0;
        for (; registered_count != targets.size (); ++registered_count) {
            if (register_relocation_target_queue (pending.prepare, targets[registered_count],
                                                  wire_objects[registered_count],
                                                  registered_count + 1))
                continue;
            for (std::size_t index = 0; index != registered_count; ++index) {
                try {
                    (void) _relocation_wire->unregister_target (
                      pending.prepare.relocation, pending.prepare.target_attempt_generation,
                      wire_objects[index]);
                }
                catch (...) {
                    _options.mesh.runtime_failures->report (std::current_exception ());
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
                if (!_actor_join_recovery_consumer (frozen[index], targets[index],
                                                    pending.prepare)) {
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
        auto held_staging = std::make_shared<relocation_assembly_staging_t> (std::move (staging));
        auto preparing = std::make_shared<task_t<void>> (
          finish_relocation_assembly (key, held_pending, held_staging, std::move (consumed)));
        detail::observe_task_completion (
          *preparing, [self = shared_from_this (), preparing] (const result_t<void> &result) {
              self->_options.mesh.runtime_failures->capture ([&] { result.value (); });
          });
    }
    catch (const framework_exception_t &error) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        reply_failure (static_cast<protocol::framework_error_code> (
          messaging::request_failure_mapper_t{}.target_failure_code (
            error.kind (), detail::failure_code (error))));
    }
    catch (const std::exception &) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        reply_failure (static_cast<protocol::framework_error_code> (
          messaging::request_failure_mapper_t{}.target_failure_code (
            framework_error_kind_t::internal_failure)));
    }
}

bool public_host_runtime_t::register_relocation_target_queue (
  const protocol::relocation_prepare_t &prepare,
  const stateful::object_ref_t &target,
  const protocol::relocation_object_t &wire_object,
  std::uint64_t participant_id)
{
    try {
        const relocation_attempt_key_t attempt_key{prepare.relocation.high, prepare.relocation.low,
                                                   prepare.target_attempt_generation};
        return _relocation_wire->register_target (
          {prepare.relocation, prepare.target_attempt_generation, prepare.coordinator,
           prepare.source_node_routing_id, prepare.source_node_generation, wire_object,
           [this, target, attempt_key, participant_id] (const protocol::relocation_data_t &data) {
               /* 28 §4.4: a pre-boundary relay record waits in the attempt's
                * boundary batch as its own accepted record; the verified
                * cutover alone stages the batch. A record after that
                * cutover can only be the source's retransmission of the
                * batch the target already took, so it is not staged again. */
               return _relocation_session_terminal_lane
                 .run ([&] {
                     const auto found = _relocation_target_attempts.find (attempt_key);
                     if (found == _relocation_target_attempts.end ()
                         || !relocation_target_wire_ready (found->second))
                         return false;
                     if (!found->second.cutover_received)
                         found->second.boundary_batch.emplace_back (target, data, participant_id);
                     return true;
                 })
                 .get ();
           },
           [] (const protocol::relocation_data_t &) {}});
    }
    catch (...) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        return false;
    }
}

task_t<bool>
public_host_runtime_t::stage_relocation_record (const stateful::object_ref_t &target,
                                                const protocol::relocation_data_t &data,
                                                std::uint64_t participant_id)
{
    const auto frozen_record = data.record;
    if (!_stateful_dispatch)
        co_return false;
    const auto staged = co_await _stateful_dispatch->stage_relocated (
      target, {0, protocol::encode_frozen_record (frozen_record)},
      [this, data, participant_id,
       frozen_record] (const std::optional<protocol::application_payload_t> &reply) {
          if (!frozen_record.reply_route_id)
              return true;
          auto relay = stateful::maintenance_runtime_t::terminal_identity (
            data.relocation, data.target_attempt_generation, data.coordinator, participant_id,
            frozen_record);
          relay.terminal_result =
            reply ? 0u
                  : static_cast<std::uint32_t> (protocol::request_terminal_result::internalError);
          relay.failure_code = reply ? protocol::framework_error_code::none
                                     : protocol::framework_error_code::requestFailed;
          return _relocation_wire->register_terminal_target (
            {relay, frozen_record.source, reply,
             [] (protocol::reply_relay_ack_status_t) { return true; }, [] { return true; }});
      });
    co_return staged == stateful::stateful_error_t::none;
}

task_t<void> public_host_runtime_t::dispatch_instance_spot_activation (
  protocol::instance_activation_recovery_t command,
  std::shared_ptr<const mesh::service_mailbox_record_t> mailbox_record,
  std::function<void (instance_spot_activation_result_t)> done)
{
    auto lifetime = shared_from_this ();
    auto owned_command =
      std::make_shared<protocol::instance_activation_recovery_t> (std::move (command));
    const auto started = _instance_activation_metrics.enabled ()
                           ? std::chrono::steady_clock::now ()
                           : std::chrono::steady_clock::time_point{};
    const auto activation =
      (_instance_activation_metrics.enabled ()
       || detail::message_flow_tracer_t (_options.mesh.dispatch)
            .enabled_for (message_flow_outcome_t::dropped)
       || detail::dispatch_error_reporter_t (_options.mesh.dispatch).enabled ())
        ? std::make_shared<const protocol::instance_spot_activation_header_t> (
            owned_command->activation)
        : nullptr;
    const auto deliver_terminal = [lifetime, transport = _transport, owned_command, activation,
                                   mailbox_record,
                                   done] (instance_spot_activation_result_t result) {
        if (result.terminal_result != 0) {
            const detail::dispatch_error_reporter_t reporter (lifetime->_options.mesh.dispatch);
            if (reporter.enabled ()) {
                const auto error = messaging::request_failure_mapper_t{}.reply_header_exception (
                  result.terminal_result, result.failure_code, "Instance Spot activation");
                const auto exception =
                  result.exception ? result.exception : std::make_exception_ptr (error);
                reporter.report_lazy ([&] {
                    message_dispatch_error_event_t event{
                      .surface = dispatch_error_surface_t::instance_spot,
                      .message_kind = activation->request ? dispatch_message_kind_t::request
                                                          : dispatch_message_kind_t::send,
                      .reason = detail::dispatch_reason_from_error (&error),
                      .action = activation->request ? dispatch_error_action_t::reply_error
                                                    : dispatch_error_action_t::drop,
                      .packet_name = owned_command->application_payload.packet_name,
                      .spot_id = activation->target.spot_id,
                      .source_rid =
                        zlink::routing_id_t::from (activation->source_node_routing_id).to_string (),
                      .exception = exception,
                      .mesh_name = lifetime->_options.mesh.descriptor.mesh_name};
                    event.flow_id = owned_command->application_payload.flow_id;
                    event.flow_origin = owned_command->application_payload.flow_origin;
                    return event;
                });
            }
        }
        if (done) {
            done (std::move (result));
            return;
        }
        auto accepted_turn_terminal = std::move (result.accepted_turn_terminal);
        try {
            (void) transport->reply_instance_spot_activation (
              *mailbox_record, result.terminal_result, result.failure_code,
              std::move (result.application_reply));
        }
        catch (...) {
            if (accepted_turn_terminal)
                accepted_turn_terminal ();
            throw;
        }
        if (accepted_turn_terminal)
            accepted_turn_terminal ();
    };
    const auto record_activation = [lifetime, owned_command, activation,
                                    started] (instance_spot_activation_result_t result) {
        // Reuse the frozen factory catalog; wire type names must not create metric series.
        const auto &materializer = lifetime->_instance_spot_materializer;
        const bool collect_metrics =
          lifetime->_instance_activation_metrics.enabled () && materializer
          && materializer->relocation_policy
          && materializer->relocation_policy (activation->target.stable_type).has_value ();
        if (result.terminal_result != 0) {
            const auto &dispatch = lifetime->_options.mesh.dispatch;
            const detail::message_flow_tracer_t tracer (dispatch);
            if (collect_metrics || tracer.enabled_for (message_flow_outcome_t::dropped)) {
                const auto error = messaging::request_failure_mapper_t{}.reply_header_exception (
                  result.terminal_result, result.failure_code, "Instance Spot activation");
                const auto exception =
                  result.exception ? result.exception : std::make_exception_ptr (error);
                auto outcome = instance_spot_activation_outcome_t::rejected;
                const auto typed = detail::framework_error (exception);
                if (typed) {
                    switch (typed->kind ()) {
                        case framework_error_kind_t::deadline_exceeded:
                            outcome = instance_spot_activation_outcome_t::timed_out;
                            break;
                        case framework_error_kind_t::shutting_down:
                            outcome = instance_spot_activation_outcome_t::shutdown;
                            break;
                        case framework_error_kind_t::already_exists:
                            outcome = instance_spot_activation_outcome_t::conflict;
                            break;
                        case framework_error_kind_t::invalid_operation:
                            outcome = instance_spot_activation_outcome_t::fenced;
                            break;
                        case framework_error_kind_t::unavailable:
                        case framework_error_kind_t::internal_failure:
                        case framework_error_kind_t::not_found:
                            outcome = instance_spot_activation_outcome_t::store_failure;
                            break;
                        default:
                            break;
                    }
                } else if (error.kind () == framework_error_kind_t::shutting_down) {
                    outcome = instance_spot_activation_outcome_t::shutdown;
                }
                if (collect_metrics)
                    lifetime->_instance_activation_metrics.complete (
                      started, activation->target.stable_type, outcome);
                if (!activation->request && !result.accepted_turn_terminal) {
                    tracer.trace (message_flow_outcome_t::dropped, [&] {
                        message_flow_event_t event{
                          .outcome = message_flow_outcome_t::dropped,
                          .surface = dispatch_error_surface_t::instance_spot,
                          .message_kind = dispatch_message_kind_t::send,
                          .packet_name = owned_command->application_payload.packet_name,
                          .source_rid =
                            zlink::routing_id_t::from (activation->source_node_routing_id)
                              .to_string (),
                          .spot_id = activation->target.spot_id,
                          .error_action = dispatch_error_action_t::drop,
                          .exception = exception,
                          .flow_id = owned_command->application_payload.flow_id,
                          .flow_origin = owned_command->application_payload.flow_origin,
                          .mesh_name = lifetime->_options.mesh.descriptor.mesh_name,
                          .instance_spot_type = activation->target.stable_type,
                          .result = message_flow_result_t::dropped,
                          .reason = outcome == instance_spot_activation_outcome_t::timed_out
                                      ? message_flow_reason_t::activation_timeout
                                    : outcome == instance_spot_activation_outcome_t::shutdown
                                      ? message_flow_reason_t::shutdown
                                      : message_flow_reason_t::activation_rejected};
                        return event;
                    });
                }
            }
        } else if (collect_metrics) {
            lifetime->_instance_activation_metrics.complete (
              started, activation->target.stable_type, instance_spot_activation_outcome_t::ready);
        }
    };
    const auto fail_activation = [record_activation,
                                  deliver_terminal] (std::exception_ptr exception) {
        const auto failure = messaging::request_failure_mapper_t{}.target_failure_reply (exception);
        instance_spot_activation_result_t terminal{
          failure->terminal_result, failure->failure_code, std::nullopt, {}, exception};
        record_activation (terminal);
        deliver_terminal (std::move (terminal));
    };
    std::shared_ptr<instance_activation_operation_t> operation;
    const auto spot_id = owned_command->activation.target.spot_id;
    std::shared_ptr<task_completion_source_t<instance_activation_admission_t>> previous_admission;
    std::shared_ptr<instance_activation_operation_t> previous;
    try {
        previous =
          _lifecycle_configuration_lane
            .run ([&] {
                if (_closing || !_started
                    || _options.mesh.shutdown_admission_seal->load (std::memory_order_acquire))
                    throw framework_exception_t (framework_error_kind_t::shutting_down,
                                                 "Instance Spot activation target stopped");
                const auto found = _instance_spot_activation_admissions.find (spot_id);
                const auto pending = found != _instance_spot_activation_admissions.end ()
                                     && !found->second->admission.task ().await_ready ();
                if (owned_command->activation.target.authority_owner_generation != 0 && !pending)
                    return std::shared_ptr<instance_activation_operation_t>{};
                operation = std::make_shared<instance_activation_operation_t> ();
                auto previous =
                  std::exchange (_instance_spot_activation_admissions[spot_id], operation);
                if (pending)
                    previous_admission =
                      std::shared_ptr<task_completion_source_t<instance_activation_admission_t>> (
                        previous, &previous->admission);
                return previous;
            })
            .get ();
    }
    catch (...) {
        fail_activation (std::current_exception ());
        co_return;
    }
    if (!operation) {
        try {
            co_await dispatch_instance_spot_activation_core (
              owned_command, mailbox_record,
              [record_activation, deliver_terminal] (instance_spot_activation_result_t result) {
                  if (result.terminal_result != 0 && !result.accepted_turn_terminal)
                      record_activation (result);
                  deliver_terminal (std::move (result));
              },
              nullptr, nullptr);
        }
        catch (...) {
            fail_activation (std::current_exception ());
        }
        co_return;
    }
    const auto metric = _instance_admission_metrics.find (command.activation.target.stable_type);
    if (metric != _instance_admission_metrics.end () && metric->second->enabled ()) {
        const auto metrics = metric->second;
        const auto payload_bytes = command.application_payload.payload_bytes ().size ();
        metrics->retain (payload_bytes);
        auto observed_admission = operation->admission.task ();
        detail::observe_task_terminal (
          observed_admission,
          [metrics, payload_bytes] (const result_t<instance_activation_admission_t> &) {
              metrics->release (payload_bytes);
          });
    }
    auto admission = std::shared_ptr<task_completion_source_t<instance_activation_admission_t>> (
      operation, &operation->admission);
    auto deadline = std::make_shared<detail::deadline_state_t> ();
    auto observed = [] (task_t<instance_activation_admission_t> admitted,
                        std::shared_ptr<detail::deadline_state_t> deadline, auto record_activation,
                        auto deliver_terminal) -> task_t<void> {
        const auto result = co_await runtime::await_result (std::move (admitted));
        deadline->cancelled.store (true, std::memory_order_release);
        detail::deadline_scheduler_t::instance ().wake ();
        if (result && std::holds_alternative<instance_activation_ready_t> (result.value ())) {
            record_activation ({});
            co_return;
        }
        instance_spot_activation_result_t terminal;
        if (result)
            terminal = std::get<instance_spot_activation_result_t> (result.value ());
        else {
            const auto failure =
              messaging::request_failure_mapper_t{}.target_failure_reply (result.exception ());
            terminal = {failure->terminal_result,
                        failure->failure_code,
                        std::nullopt,
                        {},
                        result.exception ()};
        }
        record_activation (terminal);
        deliver_terminal (std::move (terminal));
    }(admission->task (), deadline, record_activation, deliver_terminal);
    const auto reply_terminal = [admission,
                                 deliver_terminal] (instance_spot_activation_result_t result) {
        if (result.terminal_result != 0 && !result.accepted_turn_terminal) {
            auto completed = admission->task ();
            if (admission->complete (result_t<instance_activation_admission_t>::success (result)))
                return;
            const auto terminal = completed.result ();
            if (!terminal
                || !std::holds_alternative<instance_activation_ready_t> (terminal.value ()))
                return;
        }
        deliver_terminal (std::move (result));
    };
    // Only the cold route carries a source deadline; Ready joins its pending admission.
    if (owned_command->activation.target.authority_owner_generation == 0
        && !admission->task ().await_ready ()) {
        detail::deadline_scheduler_t::instance ().schedule (
          messaging::request_deadline_from_unix_ms (
            owned_command->activation.target.deadline_unix_ms),
          deadline, [weak = std::weak_ptr (admission)] {
              if (auto current = weak.lock ())
                  current->complete (result_t<instance_activation_admission_t>::failure (
                    framework_error_kind_t::deadline_exceeded,
                    "Instance Spot activation deadline expired"));
          });
    }
    const auto result = co_await runtime::await_result (dispatch_instance_spot_activation_core (
      owned_command, mailbox_record, reply_terminal, admission, previous_admission));
    if (!result)
        admission->complete (
          detail::result_access_t::failure<instance_activation_admission_t> (result.exception ()));
    const auto observed_result = co_await runtime::await_result (std::move (observed));
    if (previous)
        co_await previous->terminal.task ();
    _lifecycle_configuration_lane
      .run ([&] {
          const auto found = _instance_spot_activation_admissions.find (spot_id);
          if (found != _instance_spot_activation_admissions.end () && found->second == operation)
              _instance_spot_activation_admissions.erase (found);
      })
      .get ();
    operation->terminal.complete (result_t<void>::success ());
    observed_result.value ();
}

task_t<void> public_host_runtime_t::dispatch_instance_spot_activation_core (
  std::shared_ptr<protocol::instance_activation_recovery_t> owned_command,
  std::shared_ptr<const mesh::service_mailbox_record_t> mailbox_record,
  std::function<void (instance_spot_activation_result_t)> done,
  std::shared_ptr<task_completion_source_t<instance_activation_admission_t>> admission,
  std::shared_ptr<task_completion_source_t<instance_activation_admission_t>> previous_admission)
{
    auto lifetime = shared_from_this ();
    std::shared_ptr<const protocol::instance_activation_recovery_t> admission_command =
      owned_command;
    auto &request = owned_command->activation;
    std::shared_ptr<location_repository_t> store;
    std::shared_ptr<stateful::relocation_store_port_t> instance_relocations;
    std::shared_ptr<const instance_spot_activation_materializer_t> instance_materializer;
    std::function<std::optional<location_owner_token_t> ()> instance_owner_resolver;
    _lifecycle_configuration_lane
      .run ([&] {
          store = _user_spot_store;
          instance_relocations = _instance_spot_relocations;
          instance_materializer = _instance_spot_materializer;
          instance_owner_resolver = _instance_spot_owner;
      })
      .get ();
    const auto registered_metric = _instance_admission_metrics.find (request.target.stable_type);
    const auto instance_metric =
      registered_metric == _instance_admission_metrics.end () ? nullptr : registered_metric->second;
    const auto finish_admission = [admission] (instance_activation_admission_t result) {
        if (admission)
            admission->complete (
              result_t<instance_activation_admission_t>::success (std::move (result)));
    };
    const auto reply_terminal = [done,
                                 finish_admission] (instance_spot_activation_result_t result) {
        if (result.terminal_result != 0)
            finish_admission (result);
        done (std::move (result));
    };
    const auto instance_owner = instance_owner_resolver ? instance_owner_resolver () : std::nullopt;
    if (!store || !instance_relocations || !instance_materializer || !instance_owner) {
        reply_terminal ({105,
                         static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
                         std::nullopt});
        co_return;
    }
    const auto authority_key = spot_authority_key (request.target.spot_id);
    const auto reject_activation = [&] (std::optional<instance_spot_activation_result_t> result =
                                          std::nullopt) {
        if (result) {
            reply_terminal (std::move (*result));
            return;
        }
        const auto failure = messaging::request_failure_mapper_t{}.target_failure_reply (
          framework_error_kind_t::unavailable);
        reply_terminal ({failure->terminal_result, failure->failure_code, std::nullopt});
    };
    const auto resume_missing = [weak = weak_from_this (), command = owned_command,
                                 mailbox_record] (std::function<void ()> &terminal_sink) {
        auto completion = std::make_shared<task_completion_source_t<zlink::message_t>> ();
        auto output = completion->task ();
        const auto host = weak.lock ();
        if (!host) {
            completion->complete (result_t<zlink::message_t>::failure (
              framework_error_kind_t::shutting_down, "Instance Spot owner stopped"));
            return output;
        }
        auto running = std::make_shared<task_t<void>> (host->dispatch_instance_spot_activation (
          *command, mailbox_record,
          [completion, &terminal_sink] (instance_spot_activation_result_t reply) {
              if (reply.accepted_turn_terminal) {
                  terminal_sink = [outer = std::move (terminal_sink),
                                   inner = std::move (reply.accepted_turn_terminal)] () mutable {
                      auto inner_terminal = std::exchange (inner, {});
                      auto outer_terminal = std::exchange (outer, {});
                      std::exception_ptr failure;
                      try {
                          if (inner_terminal)
                              inner_terminal ();
                      }
                      catch (...) {
                          failure = std::current_exception ();
                      }
                      try {
                          if (outer_terminal)
                              outer_terminal ();
                      }
                      catch (...) {
                          if (!failure)
                              throw;
                          trace_mesh_host ("instance-activation-terminal",
                                           "outer accepted terminal cleanup failed");
                      }
                      if (failure)
                          std::rethrow_exception (failure);
                  };
              }
              if (reply.terminal_result == 0) {
                  completion->complete (result_t<zlink::message_t>::success (
                    reply.application_reply
                      ? zlink::message_t::from (reply.application_reply->payload_bytes ())
                      : zlink::message_t{}));
                  return;
              }
              const auto error =
                runtime::messaging::request_failure_mapper_t{}.reply_header_exception (
                  reply.terminal_result, reply.failure_code, "Instance Spot Missing placement");
              completion->complete (detail::result_access_t::failure<zlink::message_t> (error));
          }));
        detail::observe_task_terminal (
          *running, [host, running, completion] (const result_t<void> &result) {
              if (!result)
                  completion->complete (result_t<zlink::message_t>::failure (
                    result.error_kind (), result.error ()
                                            ? result.error ()->what ()
                                            : "Instance Spot Missing placement failed"));
          });
        return output;
    };
    const auto decode_current_authority = [] (const authority_read_result_t &read) {
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
        return snapshot ? decode_instance_spot_authority_payload (snapshot->payload) : std::nullopt;
    };
    const auto validate_retransmission =
      [&] (const protocol::instance_activation_recovery_t &stored) {
          const auto &prior = stored.activation;
          if (prior.operation == request.operation
              && (prior.target.mesh_name != request.target.mesh_name
                  || prior.target.stable_type != request.target.stable_type
                  || prior.target.descriptor_version != request.target.descriptor_version
                  || prior.target.deadline_unix_ms != request.target.deadline_unix_ms
                  || prior.has_metadata != request.has_metadata
                  || stored.metadata != owned_command->metadata
                  || stored.application_payload != owned_command->application_payload))
              throw framework_exception_t (framework_error_kind_t::protocol_error,
                                           "Instance route does not match the stored activation");
      };
    const auto join_existing =
      [&] (const authority_read_result_t &current,
           std::optional<instance_spot_authority_payload_t> ready_state) -> task_t<bool> {
        const auto *snapshot = std::get_if<authority_snapshot_t> (&current);
        if (request.target.authority_owner_generation != 0) {
            // Authority generations identify owner changes within one incarnation.
            if (snapshot && snapshot->allocation.state == placement_allocation_state_t::active
                && snapshot->object_generation < request.target.object_generation) {
                const auto failure = messaging::request_failure_mapper_t{}.target_failure_reply (
                  framework_error_kind_t::invalid_operation,
                  static_cast<std::uint32_t> (protocol::framework_error_code::spotGenerationStale));
                reply_terminal ({failure->terminal_result, failure->failure_code, std::nullopt});
                co_return true;
            }
            if (!snapshot || snapshot->allocation.state != placement_allocation_state_t::active
                || (snapshot->object_generation == request.target.object_generation
                    && snapshot->authority_owner_generation
                         != request.target.authority_owner_generation)
                || snapshot->owner.owner_id != request.target.owner_id
                || static_cast<std::uint64_t> (snapshot->owner.lease_generation)
                     != request.target.owner_lease_generation
                || snapshot->allocation.target.node_lifecycle_generation
                     != request.target.target_node_generation
                || snapshot->allocation.target.node_rid.value ()
                     != zlink::routing_id_t::from (request.target.target_node_routing_id)
                          .to_string ()) {
                if (instance_metric)
                    instance_metric->conflict (
                      runtime::instance_spot_admission_metrics_t::conflict_reason_t::authority);
                // Refused before admission: spotMoving tells the caller that no
                // message was accepted (Spot address messaging §9, failover §4.4).
                const auto failure = messaging::request_failure_mapper_t{}.target_failure_reply (
                  framework_error_kind_t::unavailable,
                  static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving));
                reply_terminal ({failure->terminal_result, failure->failure_code, std::nullopt});
                co_return true;
            }
            request.target.stable_type = snapshot->allocation.stable_type;
            request.target.mesh_name = snapshot->allocation.target.mesh_name;
        }
        if (!snapshot)
            co_return false;
        if (snapshot->allocation.object_kind != placement_object_kind_t::instance_spot
            || snapshot->allocation.stable_type != request.target.stable_type) {
            if (instance_metric)
                instance_metric->conflict (
                  snapshot->allocation.object_kind != placement_object_kind_t::instance_spot
                    ? runtime::instance_spot_admission_metrics_t::conflict_reason_t::spot_kind
                    : runtime::instance_spot_admission_metrics_t::conflict_reason_t::spot_type);
            reply_terminal (
              {107, static_cast<std::uint32_t> (protocol::framework_error_code::spotTypeMismatch),
               std::nullopt});
            co_return true;
        }
        if (snapshot->allocation.state == placement_allocation_state_t::active) {
            if (!ready_state) {
                if (const auto closing = decode_instance_closing_state (snapshot->payload);
                    closing && closing->stable_type == request.target.stable_type
                    && closing->spot_id == request.target.spot_id
                    && closing->object_generation == snapshot->object_generation
                    && closing->authority_owner_generation
                         == snapshot->authority_owner_generation) {
                    ready_state = instance_spot_authority_payload_t{
                      .state = instance_spot_authority_state_t::closing,
                      .stable_type = closing->stable_type,
                      .spot_id = closing->spot_id,
                      .owner_id = snapshot->owner.owner_id,
                      .owner_lease_generation =
                        static_cast<std::uint64_t> (snapshot->owner.lease_generation),
                      .mesh_name = snapshot->allocation.target.mesh_name,
                      .node_rid = snapshot->allocation.target.node_rid,
                      .node_generation = snapshot->allocation.target.node_lifecycle_generation};
                }
                if (!ready_state) {
                    reply_terminal (
                      {105,
                       static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
                       std::nullopt});
                    co_return true;
                }
            }
            if ((ready_state->state != instance_spot_authority_state_t::ready
                 && ready_state->state != instance_spot_authority_state_t::closing)
                || ready_state->stable_type != snapshot->allocation.stable_type
                || ready_state->spot_id != request.target.spot_id) {
                reply_terminal (
                  {105, static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
                   std::nullopt});
                co_return true;
            }
            if (ready_state->state == instance_spot_authority_state_t::closing
                && request.target.authority_owner_generation != 0
                && !request.target.instance_intent) {
                if (instance_metric)
                    instance_metric->conflict (
                      runtime::instance_spot_admission_metrics_t::conflict_reason_t::closing);
                const auto failure = messaging::request_failure_mapper_t{}.target_failure_reply (
                  framework_error_kind_t::not_found);
                reply_terminal ({failure->terminal_result, failure->failure_code, std::nullopt});
                co_return true;
            }
            const auto local = status ();
            const auto current_rid = zlink::routing_id_t::from (
              std::string (snapshot->allocation.target.node_rid.value ()));
            if (current_rid.to_bytes () != local.routing_id ().to_bytes ()) {
                reject_activation ();
                co_return true;
            }
            auto activation_terminal = std::make_shared<activation_terminal_t> ();
            bool prepared = false;
            std::exception_ptr preparation_error;
            try {
                prepared = ready_state->state == instance_spot_authority_state_t::closing
                           || instance_materializer->prepare (request, *snapshot,
                                                              activation_terminal->preparation);
            }
            catch (...) {
                preparation_error = std::current_exception ();
            }
            if (!prepared) {
                co_await activation_terminal->finish (false);
                if (preparation_error)
                    std::rethrow_exception (preparation_error);
                reply_terminal (
                  {105,
                   static_cast<std::uint32_t> (protocol::framework_error_code::spotCreateFailed),
                   std::nullopt});
                co_return true;
            }
            auto running = std::make_shared<task_t<instance_spot_activation_result_t>> (
              instance_materializer->dispatch (owned_command, resume_missing));
            finish_admission (instance_activation_ready_t{*snapshot, admission_command});
            detail::observe_task_terminal (
              *running, [running, reply_terminal, activation_terminal] (
                          const result_t<instance_spot_activation_result_t> &result) {
                  if (result) {
                      auto terminal = result.value ();
                      activation_terminal->accepted = std::move (terminal.accepted_turn_terminal);
                      terminal.accepted_turn_terminal = [activation_terminal] {
                          activation_terminal->finish ();
                      };
                      reply_terminal (std::move (terminal));
                  } else {
                      auto cleanup =
                        std::make_shared<task_t<void>> (activation_terminal->finish (false));
                      detail::observe_task_terminal (
                        *cleanup, [cleanup, reply_terminal] (const result_t<void> &cleaned) {
                            if (!cleaned)
                                trace_mesh_host ("instance-activation-terminal",
                                                 cleaned.error ()->what ());
                            reply_terminal ({105,
                                             static_cast<std::uint32_t> (
                                               protocol::framework_error_code::requestFailed),
                                             std::nullopt});
                        });
                  }
              });
            co_return true;
        }
        // Local Creating work is joined through its target admission owner.
        // An authority without that owner cannot authorize a second local Reserve.
        reject_activation ();
        co_return true;
    };
    authority_read_result_t current;
    if (previous_admission) {
        const auto admitted = co_await previous_admission->task ();
        if (const auto *failure = std::get_if<instance_spot_activation_result_t> (&admitted)) {
            reject_activation (*failure);
            co_return;
        }
        const auto &ready = std::get<instance_activation_ready_t> (admitted);
        validate_retransmission (*ready.command);
        admission_command = ready.command;
        current = ready.snapshot;
        if (!(co_await join_existing (current, decode_current_authority (current))))
            reject_activation ();
        co_return;
    }
    current = co_await run_blocking_step<authority_read_result_t> (
      [store, authority_key] { return store->read_authority (authority_key); });
    auto current_ready = decode_current_authority (current);
    if (request.target.authority_owner_generation == 0) {
        if (const auto *snapshot = std::get_if<authority_snapshot_t> (&current)) {
            const auto &ready = current_ready;
            const auto local = status ();
            if (snapshot->allocation.target.node_rid.value () == local.routing_id ().to_string ()
                && snapshot->allocation.target.node_lifecycle_generation
                     == local.lifecycle_generation ()) {
                if (snapshot->allocation.state == placement_allocation_state_t::active
                    && snapshot->allocation.object_kind == placement_object_kind_t::instance_spot
                    && ready && ready->state == instance_spot_authority_state_t::ready
                    && instance_materializer->matches_local
                    && instance_materializer->matches_local (request, *snapshot)) {
                    (void) co_await join_existing (current, std::move (current_ready));
                    co_return;
                }
                const auto reference =
                  snapshot->pending_creation ? snapshot->pending_creation->request_content_reference
                  : ready && ready->activation_recovery ? ready->activation_recovery->reference
                                                        : std::string{};
                if (!reference.empty ()) {
                    const auto bytes =
                      co_await run_blocking_step<std::optional<std::vector<std::uint8_t>>> (
                        [instance_relocations, reference] {
                            return task_t<std::optional<std::vector<std::uint8_t>>> (
                              result_t<std::optional<std::vector<std::uint8_t>>>::success (
                                instance_relocations->get (reference)));
                        });
                    if (!bytes)
                        throw framework_exception_t (framework_error_kind_t::data_lost,
                                                     "Stored Instance activation is missing");
                    protocol::instance_activation_recovery_t stored;
                    try {
                        stored =
                          protocol::decode_instance_activation_recovery (*bytes, capture_flow ());
                    }
                    catch (const protocol::service_wire_error_t &error) {
                        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                                     error.what ());
                    }
                    validate_retransmission (stored);
                }
            }
            if (snapshot->allocation.state == placement_allocation_state_t::active && ready
                && ready->state == instance_spot_authority_state_t::ready) {
                object_reserve_request_t release;
                release.key = {placement_object_kind_t::instance_spot, request.target.spot_id};
                release.intent.stable_type = request.target.stable_type;
                if (instance_materializer->relocation_policy)
                    release.relocation_policy =
                      instance_materializer->relocation_policy (request.target.stable_type);
                const auto released = co_await store->release_ended_reservation (
                  authority_key, snapshot->store_version, {}, std::move (release));
                if (released)
                    current = authority_missing_t{};
                else
                    current = co_await store->read_authority (authority_key);
                current_ready = decode_current_authority (current);
            }
        }
    }
    if (co_await join_existing (current, std::move (current_ready))) {
        co_return;
    }
    if (std::holds_alternative<authority_missing_t> (current)
        && instance_materializer->select_target) {
        const auto selected = co_await instance_materializer->select_target (request);
        if (!selected) {
            const auto failure =
              messaging::request_failure_mapper_t{}.target_failure_reply (*selected.error ());
            if (!failure)
                throw *selected.error ();
            reply_terminal ({failure->terminal_result, failure->failure_code, std::nullopt});
            co_return;
        }
        request = selected.value ();
        const auto local = status ();
        const auto target = zlink::routing_id_t::from (request.target.target_node_routing_id);
        if (target.to_bytes () != local.routing_id ().to_bytes ()) {
            reject_activation ();
            co_return;
        }
    }
    const auto recovery_bytes = protocol::encode_instance_activation_recovery (*owned_command);
    std::vector<std::byte> recovery_public;
    recovery_public.reserve (recovery_bytes.size ());
    for (const auto value : recovery_bytes)
        recovery_public.push_back (static_cast<std::byte> (value));
    if (recovery_public.size () > actor_authority_detail::actor_authority_maximum_bytes) {
        reply_terminal ({105,
                         static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
                         std::nullopt});
        co_return;
    }
    const auto request_sha256 = runtime::sha256 (recovery_public);
    const auto recovery_checksum = stateful::maintenance_runtime_t::crc32c (recovery_bytes);
    const auto recovery_root =
      instance_relocations->put (recovery_bytes, instance_activation_recovery_retention,
                                 std::chrono::steady_clock::now ()
                                   + (std::chrono::system_clock::time_point (
                                        std::chrono::milliseconds (request.target.deadline_unix_ms))
                                      - std::chrono::system_clock::now ()));
    if (recovery_root.reference.empty () || recovery_root.checksum_crc32c != recovery_checksum) {
        reply_terminal ({105,
                         static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
                         std::nullopt});
        co_return;
    }
    object_reserve_request_t reserve;
    reserve.operation_deadline = std::chrono::system_clock::time_point (
      std::chrono::milliseconds (request.target.deadline_unix_ms));
    reserve.key = {placement_object_kind_t::instance_spot, request.target.spot_id};
    reserve.intent.stable_type = request.target.stable_type;
    reserve.intent.request_content_reference = recovery_root.reference;
    reserve.intent.request_sha256 = request_sha256;
    reserve.intent.request_encoded_size = recovery_public.size ();
    reserve.target = {
      request.target.mesh_name,
      node_rid_t::from_string (
        zlink::routing_id_t::from (request.target.target_node_routing_id).to_string ()),
      request.target.target_node_generation, *instance_owner};
    const std::string creating = "zlink:instance-spot:creating:v1";
    for (const auto value : creating)
        reserve.creating_payload.push_back (
          static_cast<std::byte> (static_cast<unsigned char> (value)));
    reserve.capacity_bundle = {0, 1,
                               spot_type_capacity_delta_t{placement_object_kind_t::instance_spot,
                                                          request.target.stable_type, 1}};
    object_reserve_result_t reserved;
    try {
        reserved = co_await store->reserve (reserve);
    }
    catch (...) {
        instance_relocations->remove (recovery_root.reference);
        throw;
    }
    const auto *reservation = std::get_if<object_reserved_t> (&reserved);
    if (!reservation) {
        instance_relocations->remove (recovery_root.reference);
        if (std::holds_alternative<object_type_mismatch_t> (reserved)) {
            reject_activation (instance_spot_activation_result_t{
              static_cast<std::uint32_t> (protocol::request_terminal_result::conflict),
              static_cast<std::uint32_t> (protocol::framework_error_code::spotTypeMismatch),
              std::nullopt});
            co_return;
        }
        if (std::holds_alternative<object_placement_capacity_exhausted_t> (reserved)) {
            reject_activation (instance_spot_activation_result_t{
              static_cast<std::uint32_t> (protocol::request_terminal_result::busy), 0,
              std::nullopt});
            co_return;
        }
        reject_activation ();
        co_return;
    }
    auto activation_terminal = std::make_shared<activation_terminal_t> ();
    bool prepared = false;
    std::exception_ptr preparation_error;
    try {
        prepared = instance_materializer->prepare (request, reservation->creating,
                                                   activation_terminal->preparation);
    }
    catch (...) {
        preparation_error = std::current_exception ();
    }
    if (!prepared) {
        co_await activation_terminal->finish (false);
        (void) co_await store->abort ({reserve.key, reservation->fence});
        instance_relocations->remove (recovery_root.reference);
        if (preparation_error)
            std::rethrow_exception (preparation_error);
        reply_terminal (
          {105, static_cast<std::uint32_t> (protocol::framework_error_code::spotCreateFailed),
           std::nullopt});
        co_return;
    }
    instance_spot_authority_payload_t ready_state{
      .state = instance_spot_authority_state_t::ready,
      .stable_type = request.target.stable_type,
      .spot_id = request.target.spot_id,
      .owner_id = instance_owner->owner_id,
      .owner_lease_generation = static_cast<std::uint64_t> (instance_owner->lease_generation),
      .mesh_name = request.target.mesh_name,
      .node_rid = node_rid_t::from_string (
        zlink::routing_id_t::from (request.target.target_node_routing_id).to_string ()),
      .node_generation = request.target.target_node_generation,
      .activation_recovery = activation_recovery_pointer_t{
        .reference = recovery_root.reference,
        .sha256 = request_sha256,
        .encoded_size = static_cast<std::uint32_t> (recovery_public.size ()),
        .inbox_sequence = 1,
        .replay_cursor = 0}};
    const auto commit_result = co_await await_result (store->commit (
      {reserve.key, reservation->fence, encode_instance_spot_authority_payload (ready_state)}, {},
      reserve.operation_deadline));
    if (!commit_result) {
        co_await activation_terminal->finish (false);
        (void) co_await store->abort ({reserve.key, reservation->fence});
        instance_relocations->remove (recovery_root.reference);
        commit_result.value ();
    }
    const auto &committed = commit_result.value ();
    const auto *created = std::get_if<object_committed_t> (&committed);
    const auto *already = std::get_if<object_already_committed_t> (&committed);
    if (already) {
        co_await activation_terminal->finish (false);
        const authority_read_result_t ready = already->ready;
        if (!(co_await join_existing (ready, decode_current_authority (ready))))
            reply_terminal (
              {105, static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
               std::nullopt});
        co_return;
    }
    if (!created) {
        co_await activation_terminal->finish (false);
        reply_terminal ({107,
                         static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                         std::nullopt});
        co_return;
    }
    const auto &ready_snapshot = created->ready;
    auto dispatched = instance_materializer->dispatch (owned_command, resume_missing);
    finish_admission (instance_activation_ready_t{ready_snapshot, admission_command});
    const auto dispatch_result = co_await await_result (std::move (dispatched));
    if (!dispatch_result) {
        co_await activation_terminal->finish (false);
        dispatch_result.value ();
    }
    auto result = dispatch_result.value ();
    activation_terminal->accepted = std::move (result.accepted_turn_terminal);
    result.accepted_turn_terminal = [activation_terminal] { activation_terminal->finish (); };
    std::optional<task_t<void>> preparation_cleanup;
    const auto fail_preparation = [&] {
        preparation_cleanup = activation_terminal->finish (false);
    };
    try {
        ready_state.activation_recovery->replay_cursor =
          ready_state.activation_recovery->inbox_sequence;
        const auto stored_terminal =
          co_await run_blocking_step<authority_compare_exchange_result_t> (
            [store, authority_key, version = ready_snapshot.store_version,
             payload = encode_instance_spot_authority_payload (ready_state)] {
                return store->compare_exchange_authority (authority_key, version,
                                                          authority_put_t{payload});
            });
        const auto *terminal_snapshot = std::get_if<authority_stored_t> (&stored_terminal);
        if (!terminal_snapshot) {
            fail_preparation ();
            result.terminal_result = 105;
            result.failure_code =
              static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed);
            result.application_reply.reset ();
        } else {
            ready_state.activation_recovery.reset ();
            const auto cleared = co_await run_blocking_step<authority_compare_exchange_result_t> (
              [store, authority_key, version = terminal_snapshot->snapshot.store_version,
               payload = encode_instance_spot_authority_payload (ready_state)] {
                  return store->compare_exchange_authority (authority_key, version,
                                                            authority_put_t{payload});
              });
            if (std::holds_alternative<authority_stored_t> (cleared)) {
                instance_relocations->remove (recovery_root.reference);
            } else {
                fail_preparation ();
                result.terminal_result = 105;
                result.failure_code =
                  static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed);
                result.application_reply.reset ();
            }
        }
    }
    catch (const framework_exception_t &error) {
        fail_preparation ();
        const auto failure = messaging::request_failure_mapper_t{}.target_failure_reply (error);
        result.terminal_result = failure ? failure->terminal_result : 105;
        result.failure_code =
          failure ? failure->failure_code
                  : static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed);
        result.application_reply.reset ();
    }
    catch (...) {
        fail_preparation ();
        result.terminal_result = 105;
        result.failure_code =
          static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed);
        result.application_reply.reset ();
    }
    if (preparation_cleanup) {
        const auto cleanup = co_await await_result (std::move (*preparation_cleanup));
        if (!cleanup) {
            const auto failure =
              messaging::request_failure_mapper_t{}.target_failure_reply (*cleanup.error ());
            result.terminal_result = failure ? failure->terminal_result : 105;
            result.failure_code =
              failure ? failure->failure_code
                      : static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed);
        }
    }
    reply_terminal (std::move (result));
    co_return;
}

task_t<std::size_t> public_host_runtime_t::dispatch_user_spot_operations ()
{
    const auto now = std::chrono::steady_clock::now ();
    if (relocation_management_due (now)) {
        std::vector<pending_relocation_assembly_t> expired_relocation_assemblies;
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
        for (const auto &expired : expired_relocation_assemblies)
            reply_relocation_assembly_failure (expired,
                                               protocol::framework_error_code::relocationDataLost);
        poll_relocation_target_attempts ();
        flush_pending_session_relocation_seals ();
        _relocation_session_terminal_lane.run ([this] { publish_relocation_next_due_on_lane (); })
          .get ();
    }
    auto claim = _transport->mailbox ().try_claim (
      mesh::service_mailbox_domain_t::infrastructure,
      dispatch_limits::receive_batch_messages, dispatch_limits::receive_batch_bytes);
    if (!claim)
        return task_t<std::size_t> (result_t<std::size_t>::success (0));
    try {
        return dispatch_user_spot_operations (std::move (claim));
    }
    catch (const std::bad_alloc &) {
        (void) _transport->mailbox ().release (*claim);
        throw;
    }
}

task_t<std::size_t> public_host_runtime_t::dispatch_user_spot_operations (
  std::shared_ptr<mesh::service_mailbox_claim_t> &&initial_claim)
{
    // Tasks start eagerly. Own the claim before configuration or suspension;
    // the reference is used only here, after frame and promise allocation.
    std::shared_ptr<mesh::service_mailbox_claim_t> claim (std::move (initial_claim));
    std::shared_ptr<zlink::framework::location_repository_t> store;
    user_spot_materializer_t materializer;
    user_spot_closer_t user_spot_closer;
    actor_create_operation_target_t actor_create_target;
    actor_join_operation_target_t actor_join_target;
    std::function<std::optional<location_owner_token_t> ()> session_route_owner_resolver;
    std::function<void (const protocol::message_follow_notice_t &)> message_follow_handler;
    bound_session_operations_t bound_session_operations;
    std::function<void (const protocol::session_relocation_route_t &)>
      late_session_route_update_reporter;
    _lifecycle_configuration_lane
      .run ([&] {
          store = _user_spot_store;
          materializer = _user_spot_materializer;
          user_spot_closer = _user_spot_closer;
          actor_create_target = _actor_create_target;
          actor_join_target = _actor_join_target;
          session_route_owner_resolver = _session_route_owner_resolver;
          message_follow_handler = _message_follow_handler;
          bound_session_operations = _bound_session_operations;
          late_session_route_update_reporter = _late_session_route_update_reporter;
      })
      .get ();
    std::size_t count = 0;
    receive_batch_budget_t infrastructure_budget;
    while (claim) {
        for (std::size_t index = 0; index < claim->claimed_messages; ++index) {
            auto original = _transport->mailbox ().take ({claim, index});
            if (!original)
                continue;
            auto &mailbox_record = *original;
            std::size_t record_bytes = 0;
            for (const auto &part : mailbox_record.parts)
                record_bytes =
                  part.size () > std::numeric_limits<std::size_t>::max () - record_bytes
                    ? std::numeric_limits<std::size_t>::max ()
                    : record_bytes + part.size ();
            infrastructure_budget.account (record_bytes);
            ++count;
            activation_terminal_t preparation_terminal;
            std::optional<std::pair<std::uint32_t, std::uint32_t>> dispatch_failure;
            try {
                const auto wire =
                  protocol::decode_header (protocol::message_bytes (mailbox_record.parts.front ()));
                if (wire.kind == protocol::command::messageFollow) {
                    if (mailbox_record.parts.size () != 1
                        || mailbox_record.source_node_generation == 0)
                        continue;
                    const auto notice = protocol::decode_message_follow (
                      protocol::message_bytes (mailbox_record.parts.front ()));
                    if (message_follow_handler) {
                        try {
                            message_follow_handler (notice);
                        }
                        catch (...) {
                            _options.mesh.runtime_failures->report (std::current_exception ());
                        }
                    }
                    continue;
                }
                if (wire.kind == protocol::command::boundSessionBind) {
                    const auto bind = protocol::decode_bound_session_bind (
                      protocol::message_bytes (mailbox_record.parts.front ()));
                    const auto local = status ();
                    const auto actor = co_await _objects.find_async (stateful::object_kind_t::actor,
                                                                     bind.actor.actor_id);
                    const auto authority_matches = bound_session_bind_actor_matches (
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
                    bool terminal_replied = false;
                    const auto session_owner_rid =
                      zlink::routing_id_t::from (mailbox_record.source_routing_id);
                    std::optional<location_owner_token_t> session_owner;
                    if (admission == bound_session_bind_admission_t::ready
                        && bound_session_operations.bind && bound_session_operations.session_owner)
                        session_owner = co_await bound_session_operations.session_owner (
                          session_owner_rid, mailbox_record.source_node_generation);
                    if (admission == bound_session_bind_admission_t::ready && session_owner) {
                        operation_result = bound_session_operations.bind (
                          bind, session_owner_rid, mailbox_record.source_node_generation,
                          *session_owner,
                          [this, &mailbox_record,
                           &terminal_replied] (const protocol::stream_bind_reply_tail_t &binding) {
                              terminal_replied = _transport->reply_bound_session_bind (
                                mailbox_record, 0u, 0u, binding);
                              return terminal_replied;
                          });
                    }
                    // One terminal per bind. Session-Actor binding §4: without the Session
                    // owner descriptor the bind is Unavailable.
                    using terminal_t = protocol::request_terminal_result;
                    using code_t = protocol::framework_error_code;
                    const auto [terminal, code] =
                      operation_result.error == stateful::stateful_error_t::none
                        ? std::pair{terminal_t::ok, code_t::none}
                      : admission == bound_session_bind_admission_t::stale_route
                        ? std::pair{terminal_t::conflict, code_t::actorLocationStale}
                      : admission == bound_session_bind_admission_t::actor_not_ready
                        ? std::pair{terminal_t::busy, code_t::none}
                        : std::pair{terminal_t::internalError, code_t::routeNotConnected};
                    const auto replied = terminal_replied
                                         || _transport->reply_bound_session_bind (
                                           mailbox_record, static_cast<std::uint32_t> (terminal),
                                           static_cast<std::uint32_t> (code));
                    if (replied && operation_result.replacement) {
                        (void) co_await _transport->send_bound_session_replaced (
                          operation_result.replacement->retired_session
                            .session_owner_node_routing_id,
                          *operation_result.replacement);
                    }
                    continue;
                }
                if (wire.kind == protocol::command::boundSessionClose) {
                    const auto request = protocol::decode_bound_session_close (
                      protocol::message_bytes (mailbox_record.parts.front ()));
                    const auto local = status ();
                    // Session-Actor binding §4: compare with this host's current
                    // descriptor identity.
                    const auto owner =
                      session_route_owner_resolver ? session_route_owner_resolver () : std::nullopt;
                    auto closed = result_t<void>::failure (framework_error_kind_t::unavailable,
                                                           "Session owner has no live descriptor");
                    if (owner && owner->lease_generation > 0)
                        closed = co_await runtime::await_result (_sessions.close_current (
                          request, {local.routing_id ().to_bytes (),
                                    local.lifecycle_generation (),
                                    owner->owner_id,
                                    static_cast<std::uint64_t> (owner->lease_generation),
                                    {},
                                    0}));
                    const auto failure =
                      closed ? std::optional<messaging::request_wire_failure_t>{}
                             : messaging::request_failure_mapper_t{}.target_failure_reply (
                                 *closed.error ());
                    _transport->reply_bound_session_bind (
                      mailbox_record,
                      failure ? failure->terminal_result
                              : static_cast<std::uint32_t> (protocol::request_terminal_result::ok),
                      failure ? failure->failure_code
                              : static_cast<std::uint32_t> (protocol::framework_error_code::none));
                    continue;
                }
                if (wire.kind == protocol::command::boundSessionReplaced) {
                    const auto replacement = protocol::decode_bound_session_replaced (
                      protocol::message_bytes (mailbox_record.parts.front ()));
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
                    const auto control = protocol::decode_relocation_control (
                      protocol::message_bytes (mailbox_record.parts.front ()));
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
                                messaging::request_failure_mapper_t{}.target_failure_code (
                                  framework_error_kind_t::protocol_error,
                                  static_cast<std::uint32_t> (
                                    protocol::framework_error_code::requestProtocolError))});
                            continue;
                        }
                    }

                    const relocation_attempt_key_t key{prepare->relocation.high,
                                                       prepare->relocation.low,
                                                       prepare->target_attempt_generation};
                    enum class prepare_state_t
                    {
                        fresh,
                        duplicate,
                        conflict,
                        rejected
                    };
                    const auto prepare_state =
                      co_await _relocation_session_terminal_lane.run_task ([&] {
                          using state_t =
                            std::pair<prepare_state_t,
                                      std::shared_ptr<task_completion_source_t<void>>>;
                          const auto attempt = _relocation_target_attempts.find (key);
                          if (attempt != _relocation_target_attempts.end ())
                              return attempt->second.prepare == *prepare && attempt->second.pending
                                       ? state_t{prepare_state_t::duplicate,
                                                 attempt->second.pending->ready}
                                       : state_t{prepare_state_t::conflict, nullptr};
                          const auto assembly = _relocation_assemblies.find (key);
                          if (assembly != _relocation_assemblies.end ()) {
                              if (assembly->second.prepare != *prepare)
                                  return state_t{prepare_state_t::conflict, nullptr};
                              assembly->second.expires_at =
                                std::chrono::steady_clock::now () + relocation_assembly_retention;
                              lower_relocation_next_due_on_lane (assembly->second.expires_at);
                              return state_t{prepare_state_t::duplicate, assembly->second.ready};
                          }
                          if (prepare->payload_total_length == 0
                              || prepare->payload_total_length > protocol::relocationLogicalBytes
                              || prepare->payload_chunk_count == 0
                              || prepare->payload_chunk_count > protocol::relocationChunkCount)
                              return state_t{prepare_state_t::conflict, nullptr};
                          if (_options.mesh.shutdown_admission_seal->load (
                                std::memory_order_acquire)
                              || local.state == node_status_t::state_t::draining
                              || local.state == node_status_t::state_t::stopped
                              || local.state == node_status_t::state_t::error)
                              return state_t{prepare_state_t::rejected, nullptr};
                          lower_relocation_next_due_on_lane (relocation_due_now);
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
                              std::chrono::steady_clock::now () + relocation_assembly_retention});
                          return state_t{prepare_state_t::fresh, nullptr};
                      });
                    if (prepare_state.first == prepare_state_t::duplicate) {
                        observe_relocation_ready (prepare_state.second, std::move (mailbox_record),
                                                  *prepare);
                    } else if (prepare_state.first != prepare_state_t::fresh) {
                        const auto failure = prepare_state.first == prepare_state_t::conflict
                                               ? framework_error_kind_t::protocol_error
                                               : framework_error_kind_t::shutting_down;
                        (void) _transport->reply_relocation_failed (
                          mailbox_record,
                          protocol::relocation_failed_t{
                            prepare->relocation, prepare->target_attempt_generation,
                            prepare->coordinator, prepare->target, prepare->object,
                            protocol::relocation_role_t::target,
                            messaging::request_failure_mapper_t{}.target_failure_code (failure)});
                    }
                    continue;
                }
                if (wire.kind == protocol::command::relocationState) {
                    if (mailbox_record.parts.size () != 1)
                        continue;
                    const auto control = protocol::decode_relocation_control (
                      protocol::message_bytes (mailbox_record.parts.front ()));
                    const auto *state = std::get_if<protocol::relocation_state_t> (&control);
                    if (!state || state->sender_role != protocol::relocation_role_t::source
                        || state->chunk_data.empty ())
                        continue;
                    const relocation_attempt_key_t key{state->relocation.high,
                                                       state->relocation.low,
                                                       state->target_attempt_generation};
                    std::shared_ptr<pending_relocation_assembly_t> completed;
                    std::optional<pending_relocation_assembly_t> failed;
                    const auto assembly_current =
                      _relocation_session_terminal_lane
                        .run ([&] {
                            const auto found = _relocation_assemblies.find (key);
                            if (found == _relocation_assemblies.end ()
                                || found->second.prepare.coordinator != state->coordinator
                                || found->second.prepare.object != state->object
                                || found->second.prepare.source_node_routing_id
                                     != mailbox_record.source_routing_id
                                || found->second.prepare.source_node_generation
                                     != mailbox_record.source_node_generation)
                                return false;
                            const auto accepted = found->second.assembly.accept (*state);
                            if (accepted == stateful::relocation_assembly_result_t::conflict) {
                                failed.emplace (std::move (found->second));
                                _relocation_assemblies.erase (found);
                            } else if (accepted
                                       == stateful::relocation_assembly_result_t::completed) {
                                completed = std::make_shared<pending_relocation_assembly_t> (
                                  std::move (found->second));
                                relocation_target_attempt_t attempt;
                                attempt.prepare = completed->prepare;
                                attempt.pending = completed;
                                attempt.terminal =
                                  std::make_shared<task_completion_source_t<void>> ();
                                lower_relocation_next_due_on_lane (relocation_due_now);
                                _relocation_target_attempts.emplace (key, std::move (attempt));
                                _relocation_assemblies.erase (found);
                            }
                            return true;
                        })
                        .get ();
                    if (!assembly_current)
                        continue;
                    if (failed)
                        reply_relocation_assembly_failure (
                          *failed, protocol::framework_error_code::relocationDataLost);
                    if (completed)
                        complete_relocation_assembly (key, std::move (completed));
                    continue;
                }
                if (wire.kind == protocol::command::relocationCutover) {
                    if (mailbox_record.parts.size () != 1)
                        continue;
                    const auto control = protocol::decode_relocation_control (
                      protocol::message_bytes (mailbox_record.parts.front ()));
                    const auto *cutover = std::get_if<protocol::relocation_cutover_t> (&control);
                    if (!cutover || cutover->sender_role != protocol::relocation_role_t::source)
                        continue;
                    const relocation_attempt_key_t key{cutover->relocation.high,
                                                       cutover->relocation.low,
                                                       cutover->target_attempt_generation};
                    std::optional<std::vector<std::tuple<
                      stateful::object_ref_t, protocol::relocation_data_t, std::uint64_t>>>
                      verified;
                    std::optional<relocation_target_attempt_t> mismatched;
                    _relocation_session_terminal_lane
                      .run ([&] {
                          const auto found = _relocation_target_attempts.find (key);
                          if (found == _relocation_target_attempts.end ()
                              || !relocation_target_wire_ready (found->second)
                              || found->second.prepare.coordinator != cutover->coordinator
                              || found->second.prepare.object != cutover->object
                              || found->second.prepare.source_node_routing_id
                                   != mailbox_record.source_routing_id
                              || found->second.prepare.source_node_generation
                                   != mailbox_record.source_node_generation
                              || _options.mesh.shutdown_admission_seal->load (
                                std::memory_order_acquire)
                              || found->second.cutover_received) {
                              /* Late or duplicate cutover (28 §4.4): the
                               * boundary already resolved and is never
                               * re-verified or changed. */
                              return;
                          }
                          /* The batch the source sent right before this
                           * cutover on the ordered connection is the
                           * received suffix of the declared length; a
                           * retransmitted whole batch replaces any partial
                           * earlier copy (28 §4.4). */
                          auto &batch = found->second.boundary_batch;
                          const auto count = cutover->boundary_record_count;
                          bool matches = count <= batch.size ();
                          if (matches) {
                              stateful::relocation_crc32c_accumulator_t accumulator;
                              for (auto record = batch.end () - static_cast<std::ptrdiff_t> (count);
                                   record != batch.end (); ++record)
                                  accumulator.update (
                                    protocol::encode_relocation_control (std::get<1> (*record)));
                              matches = accumulator.value () == cutover->boundary_checksum_crc32c;
                          }
                          if (matches) {
                              found->second.cutover_received = true;
                              lower_relocation_next_due_on_lane (relocation_due_now);
                              verified.emplace (
                                std::make_move_iterator (batch.end ()
                                                         - static_cast<std::ptrdiff_t> (count)),
                                std::make_move_iterator (batch.end ()));
                              batch.clear ();
                              return;
                          }
                          /* Ordered connection: a declared batch that does
                           * not match what arrived before it is an
                           * implementation defect, not a retryable
                           * condition (28 §4.4) — no CAS on a possibly
                           * incomplete batch; discard the staging. */
                          mismatched = take_relocation_target_discard (found->second);
                      })
                      .get ();
                    if (verified) {
                        bool staged = true;
                        for (const auto &[target, data, participant_id] : *verified)
                            if (staged)
                                staged =
                                  co_await stage_relocation_record (target, data, participant_id);
                        _relocation_session_terminal_lane
                          .run ([&] {
                              const auto found = _relocation_target_attempts.find (key);
                              if (found == _relocation_target_attempts.end ())
                                  return;
                              if (staged) {
                                  set_relocation_finalize_at_on_lane (
                                    found->second, std::chrono::steady_clock::now ());
                                  return;
                              }
                              mismatched = take_relocation_target_discard (found->second);
                          })
                          .get ();
                        if (staged)
                            (void) try_finalize_relocation_target (key);
                    }
                    if (mismatched) {
                        std::vector<relocation_target_attempt_t> cleanup;
                        cleanup.push_back (std::move (*mismatched));
                        discard_relocation_target_attempts (std::move (cleanup));
                    }
                    continue;
                }
                if (wire.kind == protocol::command::relocationData
                    || wire.kind == protocol::command::replyRelay
                    || wire.kind == protocol::command::replyRelayAck) {
                    (void) co_await _relocation_wire->process (mailbox_record);
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
                    if (mailbox_record.parts.size () != 1)
                        continue;
                    const auto seal = [&] {
                        try {
                            return protocol::decode_session_relocation_seal (
                              protocol::message_bytes (mailbox_record.parts.front ()));
                        }
                        catch (const protocol::service_wire_error_t &) {
                            detail::dispatch_error_reporter_t (_options.mesh.dispatch)
                              .report_lazy ([&] {
                                  message_dispatch_error_event_t event{};
                                  event.surface = dispatch_error_surface_t::stream_session;
                                  event.message_kind = dispatch_message_kind_t::control;
                                  event.reason = detail::dispatch_reason_from_error (
                                    framework_error_kind_t::protocol_error);
                                  event.action = dispatch_error_action_t::drop;
                                  event.packet_name = "sessionRelocationSeal";
                                  event.source_rid =
                                    zlink::routing_id_t::from (mailbox_record.source_routing_id)
                                      .to_hex ();
                                  event.exception = std::current_exception ();
                                  return event;
                              });
                            throw;
                        }
                    }();
                    const auto owner =
                      session_route_owner_resolver ? session_route_owner_resolver () : std::nullopt;
                    if (!owner) {
                        detail::trace_session_binding_control_drop (
                          _options.mesh.dispatch, mailbox_record.source_routing_id,
                          seal.actor.actor_id, "sessionRelocationSeal",
                          message_flow_reason_t::location_unavailable);
                        continue;
                    }
                    const auto reconcile = _transport->peer_reconcile ();
                    const auto [accepted, immediate] = admit_session_relocation_seal (
                      seal, *owner, mailbox_record.source_routing_id, {}, reconcile);
                    if (!accepted) {
                        detail::trace_session_binding_control_drop (
                          _options.mesh.dispatch, mailbox_record.source_routing_id,
                          seal.actor.actor_id, "sessionRelocationSeal",
                          message_flow_reason_t::stale_target);
                        continue;
                    }
                    if (immediate)
                        (void) co_await _transport->send_session_relocation_sealed (
                          mailbox_record.source_routing_id, *immediate);
                    continue;
                }

                if (wire.kind == protocol::command::sessionRelocationRoute) {
                    if (mailbox_record.parts.size () != 1 || !session_route_owner_resolver) {
                        continue;
                    }
                    const auto route = protocol::decode_session_relocation_route (
                      protocol::message_bytes (mailbox_record.parts.front ()));
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
                    bool late_session_route_update = false;
                    const auto route_matches_seal =
                      _relocation_session_terminal_lane
                        .run ([&] {
                            const auto sealed = _session_seal_terminals.find (relocation_key);
                            if (sealed == _session_seal_terminals.end () || sealed->second.consumed
                                || sealed->second.sealed.seal_result
                                     == protocol::session_relocation_seal_result_t::
                                       binding_absent) {
                                late_session_route_update = true;
                                return true;
                            }
                            if (!sealed->second.ready
                                || sealed->second.seal.relocation != route.relocation
                                || sealed->second.seal.actor.actor_id != route.actor.actor_id
                                || sealed->second.seal.actor.object_generation
                                     != route.actor.object_generation
                                || sealed->second.seal.session_owner_node_routing_id
                                     != route.session_owner_node_routing_id
                                || sealed->second.seal.session_owner_node_generation
                                     != route.session_owner_node_generation
                                || sealed->second.seal.session_owner_id != route.session_owner_id
                                || sealed->second.seal.session_owner_lease_generation
                                     != route.session_owner_lease_generation
                                || sealed->second.seal.session_routing_id
                                     != route.session_routing_id
                                || sealed->second.seal.binding_generation
                                     != route.binding_generation)
                                return false;
                            return true;
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
                                _options.mesh.runtime_failures->report (std::current_exception ());
                            }
                        }
                        continue;
                    }

                    const auto session_id = stateful::stream_session_registry_t::connection_key (
                      zlink::routing_id_t::from (route.session_routing_id));
                    stateful::stream_route_admission_t admission;
                    if (route.route.action == protocol::session_relocation_route_action_t::commit) {
                        const auto current = _sessions.current_binding (route.actor.actor_id);
                        if (!current) {
                            continue;
                        }
                        const auto target_node =
                          zlink::routing_id_t::from (route.route.target_node_routing_id);
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
                          route.actor.object_generation, std::move (target),
                          route.route.target_node_generation,
                          route.route.target_owner_lease_generation, std::move (commit_projection));
                    } else {
                        admission = _sessions.acknowledge_remote_abort (
                          session_id, route.binding_generation, route.actor.actor_id,
                          route.actor.object_generation);
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
                    continue;
                }

                if (wire.kind == protocol::command::actorCreate) {
                    const auto request = protocol::decode_actor_create_header (
                      protocol::message_bytes (mailbox_record.parts.front ()));
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
                                host->_options.mesh.runtime_failures->report (
                                  std::current_exception ());
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
                    const auto canonical = try_decode_canonical_actor_join (
                      ::zlink::framework::detail::backend::copy_binding_parts (
                        mailbox_record.parts));
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
                            host->_options.mesh.runtime_failures->report (
                              std::current_exception ());
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
                      protocol::message_bytes (mailbox_record.parts.front ()));
                    const auto expected_parts = request.has_metadata ? 3u : 2u;
                    if (mailbox_record.parts.size () != expected_parts)
                        throw protocol::service_wire_error_t (
                          "Instance Spot activation has an invalid part count");
                    std::optional<std::vector<std::uint8_t>> metadata;
                    if (request.has_metadata)
                        metadata = mailbox_record.parts[1].to_bytes ();
                    auto application = protocol::decode_application_payload (
                      protocol::message_bytes (mailbox_record.parts[request.has_metadata ? 2 : 1]),
                      capture_flow ());
                    auto reply_record =
                      std::make_shared<const mesh::service_mailbox_record_t> (mailbox_record);
                    auto running =
                      std::make_shared<task_t<void>> (dispatch_instance_spot_activation (
                        {request, std::move (metadata), std::move (application)}, reply_record));
                    detail::observe_task_terminal (*running, [self = shared_from_this (), running] (
                                                               const result_t<void> &result) {
                        if (!result)
                            self->_options.mesh.runtime_failures->report (result.exception ());
                    });
                    continue;
                }
                if (wire.kind == protocol::command::userSpotCreate) {
                    const auto request = protocol::decode_user_spot_create_header (
                      protocol::message_bytes (mailbox_record.parts.front ()));
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
                          const auto now = unix_milliseconds_now ();
                          std::erase_if (_user_spot_terminals, [this, now] (const auto &entry) {
                              return !entry.second.header.empty ()
                                     && user_spot_operation_replay_expired (
                                       entry.second.deadline_unix_ms, now,
                                       _options.user_spot_operation_replay_retention);
                          });
                          const auto found = _user_spot_terminals.find (operation_key);
                          if (found != _user_spot_terminals.end ())
                              cached = found->second;
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
                        terminal (static_cast<std::uint32_t> (
                                    protocol::request_terminal_result::internalError),
                                  static_cast<std::uint32_t> (
                                    protocol::framework_error_code::requestFailed),
                                  protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    if (request.deadline_unix_ms <= unix_milliseconds_now ()) {
                        terminal (
                          static_cast<std::uint32_t> (protocol::request_terminal_result::timedOut),
                          0, protocol::user_spot_create_result_t::rejected, {}, 0);
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
                        terminal (
                          static_cast<std::uint32_t> (protocol::request_terminal_result::conflict),
                          static_cast<std::uint32_t> (
                            stale           ? protocol::framework_error_code::spotGenerationStale
                            : type_mismatch ? protocol::framework_error_code::spotTypeMismatch
                                            : protocol::framework_error_code::spotMoving),
                          protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    const auto fence = public_fence (
                      reservation, snapshot->allocation.target.mesh_name, request.stable_type);
                    const auto &pending = snapshot->pending_creation;
                    if (!pending || pending->reservation_id != fence.reservation_id) {
                        terminal (
                          static_cast<std::uint32_t> (protocol::request_terminal_result::conflict),
                          static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                          protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    const auto creation_payload = runtime::decode_inline_creation_content (
                      pending->request_content_reference, pending->request_sha256,
                      pending->request_encoded_size);
                    if (!creation_payload) {
                        terminal (static_cast<std::uint32_t> (
                                    protocol::request_terminal_result::internalError),
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
                        const auto existing = co_await _objects.find_async (
                          stateful::object_kind_t::user_spot, exact_ref.key);
                        if (!existing || *existing != exact_ref) {
                            terminal (static_cast<std::uint32_t> (
                                        protocol::request_terminal_result::internalError),
                                      static_cast<std::uint32_t> (
                                        protocol::framework_error_code::requestFailed),
                                      protocol::user_spot_create_result_t::rejected, {}, 0);
                            continue;
                        }
                        terminal (
                          static_cast<std::uint32_t> (protocol::request_terminal_result::ok), 0,
                          protocol::user_spot_create_result_t::existing, request.spot_id,
                          exact_ref.object_generation);
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
                        terminal (
                          static_cast<std::uint32_t> (protocol::request_terminal_result::ok), 0,
                          protocol::user_spot_create_result_t::existing, request.spot_id,
                          exact_ref.object_generation);
                        continue;
                    }
                    if (!local.factory_owner) {
                        const auto [classified_terminal, classified_code] =
                          stateful_failure_pair (local.error);
                        terminal (classified_terminal, classified_code,
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
                        terminal (static_cast<std::uint32_t> (
                                    protocol::request_terminal_result::internalError),
                                  static_cast<std::uint32_t> (
                                    protocol::framework_error_code::spotCreateFailed),
                                  protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    preparation_terminal.preparation =
                      std::move (materialized.preparation_terminal);
                    if (!materialized.accepted) {
                        co_await preparation_terminal.finish (false);
                        (void) store
                          ->abort ({{placement_object_kind_t::user_spot, global_id}, fence}, {},
                                   std::chrono::system_clock::time_point (
                                     std::chrono::milliseconds (request.deadline_unix_ms)))
                          .result ()
                          .value ();
                        (void) _objects.abort_create (local.attempt);
                        terminal (
                          static_cast<std::uint32_t> (protocol::request_terminal_result::ok), 0,
                          protocol::user_spot_create_result_t::rejected, request.spot_id,
                          exact_ref.object_generation, std::move (materialized.application_reply));
                        continue;
                    }
                    const auto committed =
                      store
                        ->commit ({{placement_object_kind_t::user_spot, global_id},
                                   fence,
                                   ready_user_spot_authority_payload (
                                     exact_ref, request.stable_type, fence.target)},
                                  {},
                                  std::chrono::system_clock::time_point (
                                    std::chrono::milliseconds (request.deadline_unix_ms)))
                        .result ()
                        .value ();
                    const auto *ready = std::get_if<object_committed_t> (&committed);
                    const auto *already = std::get_if<object_already_committed_t> (&committed);
                    if (!ready && !already) {
                        co_await preparation_terminal.finish (false);
                        (void) _objects.abort_create (local.attempt);
                        terminal (
                          static_cast<std::uint32_t> (protocol::request_terminal_result::conflict),
                          static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                          protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    auto local_commit = co_await _objects.commit_create_async (local.attempt);
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
                            local_commit =
                              co_await _objects.commit_create_async (reconciled.attempt);
                    }
                    if (local_commit != stateful::stateful_error_t::none) {
                        co_await preparation_terminal.finish (false);
                        terminal (static_cast<std::uint32_t> (
                                    protocol::request_terminal_result::internalError),
                                  static_cast<std::uint32_t> (
                                    protocol::framework_error_code::spotCreateFailed),
                                  protocol::user_spot_create_result_t::rejected, {}, 0);
                        continue;
                    }
                    co_await preparation_terminal.finish ();
                    terminal (static_cast<std::uint32_t> (protocol::request_terminal_result::ok), 0,
                              protocol::user_spot_create_result_t::created, request.spot_id,
                              exact_ref.object_generation,
                              std::move (materialized.application_reply));
                    continue;
                }

                const auto request = protocol::decode_user_spot_close_header (
                  protocol::message_bytes (mailbox_record.parts.front ()));
                auto fingerprint_request = request;
                fingerprint_request.correlation = 1;
                const auto request_fingerprint =
                  protocol::encode_user_spot_close_header (fingerprint_request);
                const auto operation_key =
                  user_spot_operation_key (request.source_node_routing_id,
                                           request.source_node_generation, request.operation);
                /* One operation ID has one terminal result (§7.1). The table
                 * decides in one lane turn whether this record replays the
                 * terminal, joins the running operation, or starts it. */
                enum class admission_t
                {
                    start,
                    replay,
                    joined,
                    expired
                };
                std::optional<user_spot_terminal_record_t> cached;
                const auto admission =
                  _user_spot_terminal_lane
                    .run ([&] {
                        const auto now = unix_milliseconds_now ();
                        std::erase_if (_user_spot_terminals, [this, now] (const auto &entry) {
                            return !entry.second.header.empty ()
                                   && user_spot_operation_replay_expired (
                                     entry.second.deadline_unix_ms, now,
                                     _options.user_spot_operation_replay_retention);
                        });
                        const auto found = _user_spot_terminals.find (operation_key);
                        if (found != _user_spot_terminals.end ()) {
                            if (found->second.kind != protocol::command::userSpotClose
                                || found->second.request_fingerprint != request_fingerprint)
                                throw protocol::service_wire_error_t (
                                  "user spot operation identity was reused with a different "
                                  "request");
                            if (found->second.header.empty ()) {
                                found->second.waiting.emplace_back (mailbox_record,
                                                                    request.correlation);
                                return admission_t::joined;
                            }
                            cached = found->second;
                            return admission_t::replay;
                        }
                        if (request.deadline_unix_ms <= now)
                            return admission_t::expired;
                        _user_spot_terminals.insert_or_assign (
                          operation_key,
                          user_spot_terminal_record_t{protocol::command::userSpotClose,
                                                      request.deadline_unix_ms,
                                                      request_fingerprint,
                                                      {},
                                                      std::nullopt,
                                                      {}});
                        return admission_t::start;
                    })
                    .get ();
                if (admission == admission_t::joined)
                    continue;
                if (admission == admission_t::replay) {
                    auto reply = protocol::decode_user_spot_close_reply (cached->header);
                    reply.header.correlation = request.correlation;
                    (void) _transport->reply_user_spot_close (mailbox_record, reply);
                    continue;
                }
                if (admission == admission_t::expired) {
                    protocol::user_spot_close_reply_t reply{{request.correlation, 101, 0}, false};
                    (void) _transport->reply_user_spot_close (mailbox_record, reply);
                    continue;
                }
                auto terminal = [this, transport = _transport, mailbox_record, operation_key,
                                 correlation = request.correlation] (std::uint32_t terminal_result,
                                                                     std::uint32_t failure_code,
                                                                     bool closed) {
                    std::vector<std::pair<mesh::service_mailbox_record_t, std::uint64_t>> waiting;
                    _user_spot_terminal_lane
                      .run ([&] {
                          auto &stored = _user_spot_terminals[operation_key];
                          stored.header = protocol::encode_user_spot_close_reply (
                            correlation, terminal_result, failure_code, closed);
                          waiting = std::exchange (stored.waiting, {});
                      })
                      .get ();
                    waiting.emplace (waiting.begin (), mailbox_record, correlation);
                    for (const auto &[record, record_correlation] : waiting) {
                        protocol::user_spot_close_reply_t reply{
                          {record_correlation, terminal_result, failure_code}, closed};
                        (void) transport->reply_user_spot_close (record, reply);
                    }
                };
                if (!store) {
                    terminal (
                      105,
                      static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
                      false);
                    continue;
                }
                // The owner runtime checks the authority and runs every Close
                // step on the Spot lifecycle lane (Spot address messaging §7.1).
                auto begin = [this, target = request.target] {
                    return begin_user_spot_close (target);
                };
                auto done = [terminal] (result_t<bool> result) {
                    if (result) {
                        terminal (0, 0, result.value ());
                        return;
                    }
                    const auto failure =
                      messaging::request_failure_mapper_t{}.target_failure_reply (*result.error ());
                    terminal (failure->terminal_result, failure->failure_code, false);
                };
                // The started operation settles its terminal record once, also
                // when the Close cannot be scheduled.
                try {
                    if (user_spot_closer)
                        user_spot_closer (request.target.spot_id, begin, done);
                    else
                        run_authority_close (begin, done, {});
                }
                catch (const std::exception &error) {
                    done (result_t<bool>::failure (framework_error_kind_t::internal_failure,
                                                   error.what ()));
                }
            }
            catch (const protocol::service_wire_error_t &) {
                dispatch_failure = {104, static_cast<std::uint32_t> (
                                           protocol::framework_error_code::requestProtocolError)};
            }
            catch (const std::exception &) {
                const auto failure = detail::current_exception_result<void> ();
                const auto *error = failure.error ();
                dispatch_failure =
                  error && error->kind () == framework_error_kind_t::deadline_exceeded
                    ? std::pair{static_cast<std::uint32_t> (
                                  protocol::request_terminal_result::timedOut),
                                static_cast<std::uint32_t> (protocol::framework_error_code::none)}
                    : std::pair{
                        static_cast<std::uint32_t> (
                          protocol::request_terminal_result::internalError),
                        static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed)};
            }
            catch (...) {
                dispatch_failure = {
                  105, static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed)};
            }
            if (dispatch_failure) {
                const auto cleanup = co_await await_result (preparation_terminal.finish (false));
                if (!cleanup) {
                    const auto failure =
                      messaging::request_failure_mapper_t{}.target_failure_reply (
                        *cleanup.error ());
                    if (failure)
                        dispatch_failure = {failure->terminal_result, failure->failure_code};
                }
                if (mailbox_record.reply_token && mailbox_record.correlation)
                    (void) _transport->reply_failure (mailbox_record, dispatch_failure->first,
                                                      dispatch_failure->second);
            }
        }
        (void) _transport->mailbox ().release (*claim);
        if (infrastructure_budget.exhausted ())
            break;
        claim = _transport->mailbox ().try_claim (
          mesh::service_mailbox_domain_t::infrastructure,
          dispatch_limits::receive_batch_messages, dispatch_limits::receive_batch_bytes);
    }
    co_return count;
}

bool public_host_runtime_t::dispatch_bound_session_send (
  const mesh::service_mailbox_record_t &mailbox_record)
{
    if (mailbox_record.parts.size () != 2)
        return false;
    const auto record =
      protocol::decode_bound_session_send (protocol::message_bytes (mailbox_record.parts.front ()));
    if (mailbox_record.source_routing_id != record.actor.target_node_routing_id
        || mailbox_record.source_node_generation == 0
        || mailbox_record.source_node_generation != record.actor.target_node_generation)
        return false;
    bound_session_operations_t operations;
    _lifecycle_configuration_lane.run_sync ([&] { operations = _bound_session_operations; });
    if (!operations.capture_send && !operations.send)
        return false;
    const auto application = protocol::decode_application_payload (
      protocol::message_bytes (mailbox_record.parts.back ()), capture_flow ());
    auto parts = protocol::decode_application_parts (application);
    std::function<stateful::stateful_error_t (std::vector<zlink::message_t>)> delivery;
    const auto admitted = _sessions.capture_outbound (
      record.actor.actor_id, record.actor.object_generation, record.expected_binding_generation,
      [&] {
          if (operations.capture_send) {
              auto capability = operations.capture_send (record);
              if (!capability)
                  return false;
              delivery = std::move (*capability);
          } else {
              delivery = [operations, record] (std::vector<zlink::message_t> payload) {
                  return operations.send (record, std::move (payload));
              };
          }
          return true;
      });
    return admitted == stateful::stateful_error_t::none
           && delivery (std::move (parts)) == stateful::stateful_error_t::none;
}

task_t<std::size_t> public_host_runtime_t::dispatch_ready (
  std::function<void (
    const ready_record_t &, const receive_record_t &, std::vector<zlink::message_t>)> dispatch,
  bool accept_application_receive,
  std::function<bool ()> next_application_receive)
{
    if (!dispatch) {
        throw std::invalid_argument ("framework public host dispatch callback is required");
    }
    // The mailbox dispatcher is bound at host start and unbound after receive shutdown.
    const bool standalone = !_transport->mailbox ().has_application_dispatch ();
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    (void) co_await _relocation_wire->retry_terminal_relays (now);
    (void) _relocation_wire->reap_terminal_tombstones (now);
    (void) _transport->tick_liveness (now);
    std::size_t count = 0;
    (void) _transport->expire_requests (foundation::operation_registry_t::clock_t::now ());
    count += co_await dispatch_user_spot_operations ();

    std::vector<stateful::object_inventory_t> stateful_owners;
    if (accept_application_receive && _stateful_dispatch && standalone) {
        const auto local_node_id =
          zlink::routing_id_t::from (_transport->topology ().local_descriptor ().node_routing_id)
            .to_string ();
        stateful_owners = co_await _objects.inventory_async ();
        std::erase_if (stateful_owners, [&] (const auto &item) {
            return item.owner.node_id != local_node_id
                   || (item.state != stateful::object_state_t::ready
                       && item.state != stateful::object_state_t::moving
                       && item.state != stateful::object_state_t::recovering);
        });
    }
    auto next_stateful_owner = stateful_owners.begin ();
    receive_batch_budget_t budget;
    // One poll per receive turn; the turn then reads records until the ROUTER
    // reports none or the budget ends (08-messaging-hot-path I2).
    const auto readiness = _transport->begin_receive_turn ();
    const bool receive_ready =
      readiness && accept_application_receive
      && (static_cast<short> (*readiness) & static_cast<short> (zlink::poll_event_flag_t::pollin))
           != 0;
    while (budget.can_receive ()) {
        const auto pumped =
          !readiness ? mesh::raw_mesh_pump_result_t::protocol_error
          : receive_ready
            ? _transport->receive_next (mesh::service_liveness_registry_t::clock_t::now ())
            : mesh::raw_mesh_pump_result_t::no_data;
        budget.account (_transport->last_pump_bytes ());
        trace_mesh_host ("pump", std::string ("result=") + pump_result_name (pumped) + " pending="
                                   + std::to_string (_transport->mailbox ().pending_messages (
                                     mesh::service_mailbox_domain_t::application)));
        if (pumped != mesh::raw_mesh_pump_result_t::no_data)
            ++count;
        bool application_dispatch_started =
          pumped == mesh::raw_mesh_pump_result_t::application && !standalone;

        if (!application_dispatch_started) {
            for (;;) {
                std::optional<local_application_dispatch_t> pending;
                bool skip = false;
                _local_dispatch_completion_lane
                  .run ([&] {
                      if (_local_application_dispatches.empty ())
                          return;
                      if (!accept_application_receive
                          && !_local_application_dispatches.front ()
                                .record.before_application_handler)
                          return;
                      pending = std::move (_local_application_dispatches.front ());
                      _local_application_dispatches.pop_front ();
                      if (uses_local_operation_registry (pending->record.kind))
                          skip = !_transport->operation_pending (pending->record.operation_id);
                  })
                  .get ();
                if (!pending)
                    break;
                if (skip)
                    continue;
                if (pending->record.kind == record_kind_t::spot_multicast
                    && pending->record.spot_route) {
                    const auto &target = *pending->record.spot_route;
                    const stateful::object_ref_t reference{
                      stateful::object_kind_t::user_spot,
                      target.spot_id,
                      target.object_generation,
                      target.authority_owner_generation,
                      _options.mesh.descriptor.mesh_name,
                      zlink::routing_id_t::from (target.target_node_routing_id).to_string ()};
                    if (!co_await _objects.contains_async (reference))
                        continue;
                }
                dispatch (pending->owner, pending->record, std::move (pending->parts));
                ++count;
                application_dispatch_started = true;
                break;
            }
        }

        if (accept_application_receive && !application_dispatch_started && _stateful_dispatch) {
            while (!application_dispatch_started && next_stateful_owner != stateful_owners.end ()) {
                const auto &item = *next_stateful_owner++;
                application_dispatch_started =
                  co_await dispatch_stateful_owner (item.owner, dispatch);
                if (application_dispatch_started)
                    ++count;
            }
        }

        while (accept_application_receive && !application_dispatch_started && standalone) {
            std::shared_ptr<application_claim_batch_t> batch;
            auto claim = _transport->mailbox ().try_claim (
              mesh::service_mailbox_domain_t::application, 1, dispatch_limits::receive_batch_bytes,
              [&] (const auto &claimed) {
                  batch = std::make_shared<application_claim_batch_t> (claimed->claimed_messages);
                  batch->take_ordinary (claimed, static_cast<bool> (_stateful_dispatch));
              });
            if (!claim)
                break;
            batch->claim = std::move (claim);
            batch->dispatch = dispatch;
            auto result = dispatch_application_claim (batch);
            std::size_t dispatched;
            if (const auto ready = std::get_if<std::size_t> (&result))
                dispatched = *ready;
            else
                dispatched = co_await std::get<task_t<std::size_t>> (std::move (result));
            count += dispatched;
            application_dispatch_started = dispatched != 0;
        }
        // Management and completion collection belong to the bounded receive
        // turn. Each additional ordinary record still needs a fresh host permit.
        if (pumped == mesh::raw_mesh_pump_result_t::backpressured
            || (pumped == mesh::raw_mesh_pump_result_t::no_data && !application_dispatch_started)
            || budget.exhausted ())
            break;
        if (next_application_receive) {
            accept_application_receive = next_application_receive ();
            if (!accept_application_receive)
                break;
        } else if (application_dispatch_started
                   || pumped == mesh::raw_mesh_pump_result_t::application) {
            break;
        }
    }

    co_return count;
}

task_t<bool> public_host_runtime_t::notify_stateful_owner (const stateful::object_ref_t &object)
{
    if (object.kind != stateful::object_kind_t::actor)
        co_return false;
    co_return co_await _transport->mailbox ().notify_stateful_ready (
      object.key, [weak = weak_from_this (), object] (const auto &dispatch) {
          const auto host = weak.lock ();
          return host && host->claim_stateful_owner (object, dispatch);
      });
}

task_t<bool> public_host_runtime_t::dispatch_stateful_owner (
  const stateful::object_ref_t &object,
  const std::function<void (
    const ready_record_t &, const receive_record_t &, std::vector<zlink::message_t>)> &dispatch)
{
    if (object.kind == stateful::object_kind_t::actor) {
        if (co_await notify_stateful_owner (object))
            co_return true;
        co_return claim_stateful_owner (object, dispatch);
    }
    const auto locate =
      co_await _lifecycle_configuration_lane.run_task ([this] { return _relocation_gate; });
    std::shared_ptr<runtime::serial_execution_queue_t> gate;
    if (locate)
        gate = co_await locate (object);
    std::shared_ptr<stateful::relocation_seal_binding_t> restoring;
    if (gate)
        restoring = co_await gate->admit_native_application ({});
    bool dispatched = false;
    for (;;) {
        std::function<void ()> before;
        std::string stable_type;
        auto [error, delivery] =
          co_await _stateful_dispatch->try_claim_async (object, &before, &stable_type);
        if (error != stateful::stateful_error_t::none || !delivery) {
            if (restoring)
                (void) co_await gate->release_relocation_seal (std::move (restoring));
            co_return dispatched;
        }
        dispatched = dispatch_stateful_delivery (object, std::move (*delivery), std::move (before),
                                                 std::move (stable_type), dispatch)
                     || dispatched;
        if (!restoring)
            co_return dispatched;
        // Publication import returns the native claim before the next owner claim.
        // New root handlers remain behind this same seal until the prefix is in the FIFO.
        (void) co_await gate->admit_native_application ({});
    }
}

bool public_host_runtime_t::claim_stateful_owner (
  const stateful::object_ref_t &object,
  const mesh::service_mailbox_t::application_dispatch_t &dispatch)
{
    std::function<void ()> before_application_handler;
    std::string actor_type;
    auto [claim_error, delivery] =
      _stateful_dispatch->try_claim (object, &before_application_handler, &actor_type);
    if (claim_error != stateful::stateful_error_t::none || !delivery)
        return false;
    return dispatch_stateful_delivery (object, std::move (*delivery),
                                       std::move (before_application_handler),
                                       std::move (actor_type), dispatch);
}

bool public_host_runtime_t::dispatch_stateful_delivery (
  const stateful::object_ref_t &object,
  stateful::stateful_delivery_t claimed,
  std::function<void ()> before_application_handler,
  std::string actor_type,
  const mesh::service_mailbox_t::application_dispatch_t &dispatch)
{
    auto delivery = std::optional<stateful::stateful_delivery_t>{std::move (claimed)};
    receive_record_t record;
    try {
        const auto &frozen = delivery->frozen;
        ready_record_t owner;
        owner.domain = ready_domain_t::application;
        record.domain = ready_domain_t::application;
        record.before_application_handler = std::move (before_application_handler);
        record.source_node_rid = zlink::routing_id_t::from (frozen.source.node_routing_id);
        record.operation_id = call_id_t{frozen.operation.high, frozen.operation.low};
        record.reply_route_id = frozen.reply_route_id.value_or (0);
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
                throw protocol::service_wire_error_t ("unsupported stateful application record");
        }
        record.operation_kind = operation_kind (record.kind);
        if (owner.owner_kind == owner_kind_t::actor) {
            owner.actor = framework_actor_ref (object, actor_type);
        } else {
            owner.spot_id = object.key;
        }
        auto application_record = delivery->application_record;
        auto parts = with_accepted_application (
          delivery->turn,
          [] (protocol::frozen_record_kind_t, const protocol::application_payload_t &application) {
              return protocol::decode_application_parts (application);
          },
          capture_flow ());
        const auto sequence = delivery->turn.sequence;
        {
            if (application_record && application_record->admission.durable ()) {
                auto previous = std::move (application_record);
                application_record = std::make_shared<runtime::serial_application_record_t> ();
                application_record->admission = std::move (previous->admission);
                application_record->native_reply = std::move (previous->native_reply);
                previous->admission.reservation.emplace<std::function<void ()>> ();
            } else if (!application_record) {
                application_record = std::make_shared<runtime::serial_application_record_t> ();
            }
            application_record->admission.jobs = _options.mesh.application_jobs;
            application_record->payload () = std::move (delivery->turn);
            application_record->transferred = [weak = weak_from_this (), owner = delivery->owner,
                                               sequence] {
                if (const auto host = weak.lock ()) {
                    if (owner.kind == stateful::object_kind_t::actor) {
                        (void) host->_stateful_dispatch->transfer_application_claim (owner,
                                                                                     sequence);
                    } else {
                        auto pending = std::make_shared<task_t<stateful::stateful_error_t>> (
                          host->_stateful_dispatch->transfer_application_claim_async (owner,
                                                                                      sequence));
                        detail::observe_task_completion (
                          *pending,
                          [pending, host] (const result_t<stateful::stateful_error_t> &result) {
                              host->_options.mesh.runtime_failures->capture ([&] {
                                  if (result.value () != stateful::stateful_error_t::none)
                                      throw std::logic_error (
                                        "restored Spot claim transfer lost its owner");
                              });
                          });
                    }
                }
            };
            record.application_record = application_record;
        }
        delivery->turn = {};
        delivery->turn.sequence = sequence;
        delivery->application_record.reset ();
        auto completed = std::make_shared<std::atomic_bool> (false);
        auto native_terminal =
          owner.owner_kind == owner_kind_t::spot && sequence == 0 && application_record
            ? std::exchange (application_record->native_reply, {})
            : decltype (application_record->native_reply){};
        auto finish = [weak = weak_from_this (), delivery = *delivery, completed,
                       native_terminal = std::move (native_terminal),
                       accepted = std::weak_ptr (application_record)] (
                        std::optional<protocol::application_payload_t> reply,
                        std::exception_ptr error = {}) {
            const auto host = weak.lock ();
            if (!host)
                return false;
            const auto accepted_record = accepted.lock ();
            if (error)
                report_stateful_dispatch_error (host->_options.mesh.dispatch, delivery,
                                                std::move (error), accepted_record.get ());
            if (delivery.owner.kind == stateful::object_kind_t::actor && !accepted_record)
                return false;
            if (completed->exchange (true, std::memory_order_acq_rel))
                return false;
            try {
                if (delivery.owner.kind != stateful::object_kind_t::actor
                    && delivery.turn.sequence == 0 && accepted_record) {
                    accepted_record->release_native_claim ();
                    return !native_terminal || native_terminal (std::move (reply));
                }
                return (accepted_record
                          ? host->_stateful_dispatch->complete (delivery, std::move (reply),
                                                                accepted_record)
                          : host->_stateful_dispatch->complete (delivery, std::move (reply)))
                       == stateful::stateful_error_t::none;
            }
            catch (...) {
                report_stateful_dispatch_error (host->_options.mesh.dispatch, delivery,
                                                std::current_exception (), accepted_record.get ());
                return false;
            }
        };
        if (application_record)
            application_record->native_reply = finish;
        record.complete_stateful_dispatch = [finish, application_record] {
            // A transferred payload keeps its native terminal until the FIFO
            // that owns the accepted record settles it, including after abort.
            if (application_record && !application_record->transferred)
                return;
            (void) finish (std::nullopt);
        };
        if (delivery->request) {
            record.reply_token.host = weak_from_this ();
            record.reply_token.local_reply =
              [finish, application_record] (const std::vector<zlink::message_t> &parts) {
                  // Retain the accepted payload until its original token settles.
                  std::optional<protocol::application_payload_t> reply;
                  try {
                      reply = protocol::application_payload_t::from_parts (parts);
                  }
                  catch (...) {
                      (void) finish (std::nullopt, std::current_exception ());
                      return false;
                  }
                  return finish (std::move (reply));
              };
        }
        dispatch (owner, record, std::move (parts));
        return true;
    }
    catch (...) {
        report_stateful_dispatch_error (_options.mesh.dispatch, *delivery,
                                        std::current_exception (),
                                        record.application_record.get ());
        try {
            if (record.application_record && record.application_record->native_reply)
                (void) record.application_record->native_reply (std::nullopt);
            else
                (void) _stateful_dispatch->complete (*delivery, std::nullopt);
        }
        catch (...) {
            report_stateful_dispatch_error (_options.mesh.dispatch, *delivery,
                                            std::current_exception (),
                                            record.application_record.get ());
        }
    }
    return false;
}

task_t<bool> public_host_runtime_t::dispatch_native_spot (
  stateful::object_ref_t object,
  mesh::service_mailbox_t::claim_handle_t slot,
  stateful::claimed_ingress_t query,
  std::shared_ptr<const local_application_dispatch_t> application,
  std::optional<stateful::retained_relocation_source_t> retained,
  mesh::service_mailbox_t::application_dispatch_t dispatch)
{
    if (retained)
        co_return false; // The existing source proof owns hold and Message Follow ingress.
    protocol::spot_message_header_t header;
    if (application) {
        header = local_spot_header (*application);
    } else {
        if (query.error)
            std::rethrow_exception (query.error);
        if (!query.header)
            throw protocol::service_wire_error_t ("Spot ingress requires a header");
        const auto bytes = protocol::message_bytes (*query.header);
        header = protocol::decode_spot_message_header (bytes, protocol::decode_header (bytes).kind);
    }
    const auto [error, authority] = co_await _stateful_dispatch->accepted_spot_authority (
      object, header, std::move (query.source_routing_id), query.source_node_generation,
      std::move (query.source_original), std::move (retained));
    if (error != stateful::stateful_error_t::none)
        co_return false; // Existing ingress owns the exact rejection and reply mapping.
    header.target.owner_lease_generation = authority->target_owner_lease_generation;
    // Configured before transport start; the lifecycle lane rejects later writes
    // while the host is running or closing.
    const auto &locate = _relocation_gate;
    const auto &prepare = _native_spot_target;
    std::shared_ptr<runtime::serial_execution_queue_t> gate;
    std::shared_ptr<runtime::serial_application_target_t> target;
    std::vector<zlink::message_t> parts;
    try {
        if (!application) {
            parts = protocol::decode_application_parts (std::move (*query.body), capture_flow ());
        }
        if (application ? application->parts.empty () : parts.empty ())
            throw protocol::service_wire_error_t ("Spot application envelope is empty");
    }
    catch (const protocol::service_wire_error_t &) {
        co_return false; // Existing ingress owns protocol rejection and its reply.
    }
    if (prepare) {
        try {
            target = co_await prepare (object.key, header.target);
            if (target)
                gate = target->queue;
        }
        catch (const framework_exception_t &error) {
            target = std::make_shared<runtime::serial_application_target_t> ();
            target->context = error;
        }
    }
    if (!gate && locate)
        gate = co_await locate (object);
    if (!gate && !target)
        co_return false;
    bool native = false;
    const auto publish = [&] (bool after_cut) {
        if (after_cut)
            return; // The existing ingress takes the canonical slot directly into hold.
        native = true;
        auto original = _transport->mailbox ().take (slot);
        if (!original)
            return; // A cut already owns this original.
        auto credit = std::exchange (original->transferred, {});
        const auto release_credit = [] (mesh::service_mailbox_settlement_t *owned) {
            if (*owned)
                (*owned) ();
        };
        const std::unique_ptr<mesh::service_mailbox_settlement_t, decltype (release_credit)>
          credit_owner (&credit, release_credit);
        auto before = std::move (original->before_application_handler);
        ready_record_t owner;
        receive_record_t record;
        if (application) {
            parts = std::move (original->application->parts);
            owner = application->owner;
            record = application->record;
        } else {
            owner.owner_kind = owner_kind_t::spot;
            owner.domain = ready_domain_t::application;
            owner.spot_id = object.key;
            record.domain = ready_domain_t::application;
            record.kind =
              header.correlation ? record_kind_t::spot_request : record_kind_t::spot_send;
            record.operation_kind = operation_kind (record.kind);
            record.source_node_rid = zlink::routing_id_t::from (original->source_routing_id);
            record.operation_id = {header.operation.high, header.operation.low};
            record.reply_route_id = header.correlation.value_or (0);
            original->parts.clear ();
            if (header.correlation)
                record.reply_token = {
                  weak_from_this (),
                  std::make_shared<mesh::service_mailbox_record_t> (std::move (*original))};
        }
        record.spot_route = header.target;
        record.before_application_handler = std::move (before);
        record.transferred_owner_byte_cost = slot.claim->record_bytes[slot.index];
        auto accepted = std::make_shared<runtime::serial_application_record_t> ();
        accepted->target = std::move (target);
        accepted->admission.jobs = _options.mesh.application_jobs;
        accepted->set_spot_identity ({header, authority->source}, parts.front ().copy ());
        record.application_record = accepted;
        // The FIFO owns the original reply capability through capture and source abort.
        if (header.correlation) {
            accepted->native_reply = [weak = weak_from_this (), token = record.reply_token,
                                      correlation = *header.correlation] (
                                       std::optional<protocol::application_payload_t> reply) {
                const auto host = weak.lock ();
                if (!host)
                    return false;
                if (reply)
                    return host->reply (token, protocol::decode_application_parts (*reply));
                if (token.request)
                    return host->_transport->reply_failure (
                      *token.request,
                      static_cast<std::uint32_t> (protocol::request_terminal_result::internalError),
                      static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed));
                return host->_transport->fail_local_operation (
                  {host->_options.mesh.descriptor.lifecycle_generation, correlation},
                  foundation::operation_terminal_t::transport_failed);
            };
            record.reply_token = {};
            record.reply_token.host = weak_from_this ();
            record.reply_token.local_reply = [accepted] (const auto &reply) {
                return accepted->complete_native (
                  protocol::application_payload_t::from_parts (reply));
            };
        }
        record.retain_mailbox_reservation = [] {};
        accepted->transferred = std::exchange (credit, {});
        record.release_mailbox_reservation = [accepted] { accepted->release_native_claim (); };
        try {
            dispatch (owner, record, std::move (parts));
        }
        catch (...) {
            accepted->release_native_claim ();
            throw;
        }
    };
    if (gate) {
        auto restoring = co_await gate->admit_native_application (publish);
        while (restoring) {
            (void) co_await dispatch_stateful_owner (object, dispatch);
            restoring = co_await gate->admit_native_application (publish);
        }
    } else {
        publish (false);
    }
    co_return native;
}

task_t<std::size_t> public_host_runtime_t::dispatch_application_claim (
  std::shared_ptr<mesh::service_mailbox_claim_t> claim,
  mesh::service_mailbox_t::application_dispatch_t dispatch,
  std::shared_ptr<application_claim_batch_t> batch)
{
    try {
        if (!batch)
            batch = std::make_shared<application_claim_batch_t> (claim->claimed_messages);
        batch->claim = std::move (claim);
        batch->dispatch = std::move (dispatch);
        auto result = dispatch_application_claim (batch);
        if (const auto count = std::get_if<std::size_t> (&result))
            return task_t<std::size_t> (result_t<std::size_t>::success (*count));
        return std::get<task_t<std::size_t>> (std::move (result));
    }
    catch (...) {
        return task_t<std::size_t> (detail::current_exception_result<std::size_t> ());
    }
}

public_host_runtime_t::application_claim_result_t
public_host_runtime_t::dispatch_application_claim (
  const std::shared_ptr<application_claim_batch_t> &batch)
{
    const auto &claim = batch->claim;
    // Stateful originals stay in their slot across the authority suspension; every
    // other original transfers in the same boundary step that classifies it.
    struct kept_local_t
    {
        std::shared_ptr<const local_application_dispatch_t> application;
        std::optional<mesh::request_source_t> source_original;
    };
    using kept_t = std::variant<stateful::claimed_ingress_t, kept_local_t>;
    trace_mesh_host ("mailbox-claim",
                     std::string ("records=") + std::to_string (claim->claimed_messages));
    for (; batch->index < claim->claimed_messages; ++batch->index) {
        const auto index = batch->index;
        const mesh::service_mailbox_t::claim_handle_t slot{claim, index};
        auto &entry = batch->entries[index];
        mesh::service_mailbox_settlement_t release_mailbox_reservation;
        try {
            // A taken original is dispatched in its slot: each record move copies
            // container storage on some standard libraries (MSVC std::map).
            using step_t = std::variant<std::monostate, mesh::service_mailbox_record_t, kept_t>;
            auto step =
              entry.original
                ? step_t ()
                : _transport->mailbox ().take_unless (
                    slot,
                    [stateful = static_cast<bool> (_stateful_dispatch),
                     sources = _transport->request_sources (),
                     mesh_name = &_options.mesh.descriptor.mesh_name] (
                      mesh::service_mailbox_record_t &original) -> std::optional<kept_t> {
                        if (!stateful)
                            return std::nullopt;
                        if (original.application) {
                            const auto &record = original.application->record;
                            if (record.application_record || !is_stateful_application (record.kind))
                                return std::nullopt;
                            return kept_t{
                              kept_local_t{original.application, original.source_original}};
                        }
                        const auto kind =
                          record_kind (protocol::decode_header (
                                         protocol::message_bytes (original.parts.front ()))
                                         .kind);
                        if (!kind || !is_stateful_application (*kind))
                            return std::nullopt;
                        return kept_t{
                          stateful::claimed_ingress_t::prepare (original, sources, *mesh_name)};
                    });
            if (auto *kept = std::get_if<kept_t> (&step)) {
                // Only stateful ingress needs a suspended cursor. The canonical
                // slot remains the original owner throughout authority preparation.
                auto pending = [] (std::shared_ptr<public_host_runtime_t> self,
                                   std::shared_ptr<application_claim_batch_t> batch,
                                   kept_t kept) -> task_t<void> {
                    auto &claim = batch->claim;
                    const auto index = batch->index;
                    const mesh::service_mailbox_t::claim_handle_t slot{claim, index};
                    auto &entry = batch->entries[index];
                    const mesh::service_mailbox_t::application_dispatch_t dispatch =
                      [batch] (const auto &ready, const auto &record, auto parts) {
                          batch->handoff (ready, record, std::move (parts));
                      };
                    if (auto *local = std::get_if<kept_local_t> (&kept)) {
                        if (!is_actor_application (local->application->record.kind)) {
                            std::optional<stateful::retained_relocation_source_t> retained;
                            const auto owner = co_await self->_objects.resolve_spot_async (
                              local->application->owner.spot_id, &retained);
                            stateful::claimed_ingress_t query;
                            query.source_original = local->source_original;
                            query.source_routing_id =
                              local->application->record.source_node_rid.to_bytes ();
                            query.source_node_generation =
                              self->_options.mesh.descriptor.lifecycle_generation;
                            if (owner) {
                                const bool native = co_await self->dispatch_native_spot (
                                  *owner, slot, std::move (query), local->application,
                                  std::move (retained), dispatch);
                                if (native) {
                                    ++batch->count;
                                    ++batch->index;
                                    co_return;
                                }
                            }
                        }
                        auto query = local_application_query (*local->application);
                        const auto object = co_await self->accept_local_application (
                          slot, std::move (query), std::move (local->application),
                          std::move (local->source_original), entry.original);
                        if (object) {
                            (void) co_await self->dispatch_stateful_owner (*object, dispatch);
                            ++batch->count;
                        }
                        ++batch->index;
                        co_return;
                    }
                    std::optional<stateful::object_ref_t> native_owner;
                    std::optional<stateful::retained_relocation_source_t> retained;
                    const auto owner_kind = stateful_application_owner (claim->owner);
                    if (owner_kind == owner_kind_t::actor)
                        native_owner = co_await self->_objects.find_async (
                          stateful::object_kind_t::actor,
                          claim->owner.substr (
                            detail::activation_admission_t::actor_key_prefix.size ()),
                          std::nullopt, &retained);
                    else if (owner_kind == owner_kind_t::spot) {
                        const auto key = claim->owner.substr (
                          detail::activation_admission_t::spot_key_prefix.size ());
                        native_owner = co_await self->_objects.resolve_spot_async (key, &retained);
                    }
                    if (native_owner) {
                        auto &query = std::get<stateful::claimed_ingress_t> (kept);
                        std::optional<stateful::claimed_ingress_t> prepared;
                        if (native_owner->kind != stateful::object_kind_t::actor) {
                            const bool native = co_await self->dispatch_native_spot (
                              *native_owner, slot, std::move (query), {}, retained, dispatch);
                            if (native) {
                                ++batch->count;
                                ++batch->index;
                                co_return;
                            }
                            // A cut or rejection keeps the original; only this cold ingress
                            // prepares its immutable query again from that same slot.
                        } else {
                            prepared.emplace (std::move (query));
                        }
                        const auto accepted = co_await self->_stateful_dispatch->ingest_claimed (
                          *native_owner, slot, std::move (retained), std::move (prepared));
                        if (accepted == stateful::stateful_error_t::none)
                            (void) co_await self->dispatch_stateful_owner (*native_owner, dispatch);
                        self->_transport->signal_activity ();
                        ++batch->count;
                        ++batch->index;
                        co_return;
                    }
                    entry.original = self->_transport->mailbox ().take (slot);
                    if (!entry.original)
                        ++batch->index;
                }(shared_from_this (), batch, std::move (*kept));
                return continue_application_claim (batch, std::move (pending));
            }
            if (auto *taken = std::get_if<mesh::service_mailbox_record_t> (&step))
                entry.original.emplace (std::move (*taken));
            if (!entry.original)
                continue;
            auto &mailbox_record = *entry.original;
            release_mailbox_reservation = std::exchange (mailbox_record.transferred, {});
            const auto retain_mailbox_reservation = [batch, index] {
                batch->entries[index].retained.store (true, std::memory_order_release);
            };
            if (mailbox_record.application) {
                auto application = std::move (*mailbox_record.application);
                application.record.before_application_handler =
                  mailbox_record.before_application_handler;
                application.record.release_mailbox_reservation = release_mailbox_reservation;
                application.record.retain_mailbox_reservation = retain_mailbox_reservation;
                application.record.transferred_owner_byte_cost = claim->record_bytes[index];
                batch->handoff (application.owner, application.record,
                                std::move (application.parts));
                ++batch->count;
                if (!entry.retained.load (std::memory_order_acquire))
                    release_mailbox_reservation ();
                continue;
            }
            if (mailbox_record.parts.size () < 2)
                throw protocol::service_wire_error_t (
                  "application wire requires a header and payload");
            const auto wire =
              protocol::decode_header (protocol::message_bytes (mailbox_record.parts.at (0)));
            if (wire.kind == protocol::command::logicalMulticast) {
                namespace generated = protocol::generated::detail;
                generated::logicalMulticast_23_t multicast;
                generated::reader_t reader (
                  protocol::message_bytes (mailbox_record.parts.front ()));
                if (generated::decode_head_logicalMulticast_23 (reader, multicast)
                    != generated::error_code::ok)
                    throw protocol::service_wire_error_t ("invalid logical multicast frame");
                auto targets = _objects.multicast_local_targets (multicast.channelName.value,
                                                                 multicast.topic.value);
                auto metadata =
                  (multicast.flags & static_cast<std::uint8_t> (protocol::flag::metadata)) == 0
                    ? std::shared_ptr<const std::vector<std::uint8_t>>{}
                    : std::make_shared<const std::vector<std::uint8_t>> (
                        mailbox_record.parts[1].to_bytes ());
                auto parts =
                  protocol::decode_application_parts (std::move (mailbox_record.parts.back ()));
                auto pending = std::make_shared<task_t<void>> (enqueue_local_multicast (
                  std::move (targets), std::move (multicast.channelName.value),
                  std::move (multicast.topic.value), std::move (parts), std::move (metadata),
                  zlink::routing_id_t::from (mailbox_record.source_routing_id),
                  std::move (mailbox_record.before_application_handler)));
                detail::observe_task_completion (
                  *pending, [self = shared_from_this (), pending] (const result_t<void> &) {});
                ++batch->count;
                if (!entry.retained.load (std::memory_order_acquire))
                    release_mailbox_reservation ();
                continue;
            }
            if (mailbox_record.parts.size () != 2)
                throw protocol::service_wire_error_t ("application wire requires two parts");
            if (wire.kind == protocol::command::boundSessionSend) {
                // The outbound stream delivery is a transport handoff, not an application handler.
                mailbox_record.before_application_handler = {};
                (void) dispatch_bound_session_send (mailbox_record);
                ++batch->count;
                if (!entry.retained.load (std::memory_order_acquire))
                    release_mailbox_reservation ();
                continue;
            }
            const auto wire_kind = record_kind (wire.kind);
            if (!wire_kind)
                throw protocol::service_wire_error_t (
                  "mailbox record is not application messaging");
            const auto kind = *wire_kind;
            ready_record_t owner;
            owner.domain = ready_domain_t::application;
            receive_record_t record;
            record.before_application_handler = mailbox_record.before_application_handler;
            record.kind = kind;
            record.domain = ready_domain_t::application;
            record.operation_kind = operation_kind (kind);
            record.source_node_rid = zlink::routing_id_t::from (mailbox_record.source_routing_id);
            if (mailbox_record.operation) {
                record.operation_id = {mailbox_record.operation->first,
                                       mailbox_record.operation->second};
            } else if (mailbox_record.correlation) {
                record.operation_id = {_options.mesh.descriptor.lifecycle_generation,
                                       *mailbox_record.correlation};
            }
            if (is_request (kind)) {
                // The reply follows the taken original's lifetime in this claim batch.
                record.reply_token = {
                  weak_from_this (),
                  std::shared_ptr<mesh::service_mailbox_record_t> (batch, &mailbox_record)};
            }
            if (kind == record_kind_t::channel_send || kind == record_kind_t::channel_request) {
                owner.owner_kind = owner_kind_t::channel;
                owner.channel_name = kind == record_kind_t::channel_send
                                       ? protocol::decode_channel_send_header (
                                           protocol::message_bytes (mailbox_record.parts.front ()))
                                       : protocol::decode_channel_request_header (
                                           protocol::message_bytes (mailbox_record.parts.front ()))
                                           .channel_name;
                record.channel_name = owner.channel_name;
            } else if (kind == record_kind_t::spot_send || kind == record_kind_t::spot_request) {
                owner.owner_kind = owner_kind_t::spot;
                const auto spot = protocol::decode_spot_message_header (
                  protocol::message_bytes (mailbox_record.parts.front ()), wire.kind);
                owner.spot_id = spot.target.spot_id;
                record.spot_route = spot.target;
            } else if (kind == record_kind_t::actor_send || kind == record_kind_t::actor_request) {
                owner.owner_kind = owner_kind_t::actor;
                const auto actor = protocol::decode_actor_message_header (
                  protocol::message_bytes (mailbox_record.parts.front ()), wire.kind);
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
                const auto actor_type = _spot_actor_index_lane.run_sync ([&] {
                    std::string actor_type;
                    const auto found = _actors.find (actor.target.actor_id);
                    if (found != _actors.end ()) {
                        actor_type = found->second.first;
                    }
                    return actor_type;
                });
                owner.actor = ::zlink::framework::detail::actor_ref_access_t::make (
                  node_rid_t::from_string (status ().routing_id ().to_string ()),
                  std::move (actor_type), actor.target.actor_id, actor.target.object_generation);
            } else {
                owner.owner_kind = owner_kind_t::node;
            }
            auto parts = protocol::decode_application_parts (std::move (mailbox_record.parts[1]),
                                                             capture_flow ());
            trace_mesh_host (
              "dispatch",
              std::string ("kind=") + std::to_string (static_cast<int> (kind)) + " source="
                + (mailbox_record.source_routing_id.empty () ? std::string ("-")
                                                             : record.source_node_rid.to_string ())
                + " parts=" + std::to_string (mailbox_record.parts.size ()));
            record.release_mailbox_reservation = release_mailbox_reservation;
            record.retain_mailbox_reservation = retain_mailbox_reservation;
            record.transferred_owner_byte_cost = claim->record_bytes[index];
            batch->handoff (owner, record, std::move (parts));
            ++batch->count;
            if (!entry.retained.load (std::memory_order_acquire))
                release_mailbox_reservation ();
        }
        catch (...) {
            fail_application_claim (batch, std::current_exception (),
                                    std::move (release_mailbox_reservation));
        }
    }
    return batch->finish ();
}

void public_host_runtime_t::fail_application_claim (
  const std::shared_ptr<application_claim_batch_t> &batch,
  std::exception_ptr error,
  mesh::service_mailbox_settlement_t release_mailbox_reservation)
{
    const auto &claim = batch->claim;
    const auto index = batch->index;
    const mesh::service_mailbox_t::claim_handle_t slot{claim, index};
    auto &entry = batch->entries[index];
    std::variant<std::monostate, messaging::request_wire_failure_t, std::exception_ptr> rejection;
    try {
        std::rethrow_exception (error);
    }
    catch (const protocol::service_wire_error_t &) {
        rejection = messaging::request_wire_failure_t{
          static_cast<std::uint32_t> (protocol::request_terminal_result::protocolError),
          static_cast<std::uint32_t> (protocol::framework_error_code::requestProtocolError)};
    }
    catch (const framework_exception_t &error) {
        if (!entry.original)
            entry.original = _transport->mailbox ().take (slot);
        const auto failure = messaging::request_failure_mapper_t{}.target_failure_reply (error);
        if (failure && entry.original && !entry.retained.load (std::memory_order_acquire))
            rejection = *failure;
        else
            rejection = std::current_exception ();
    }
    catch (...) {
        rejection = std::current_exception ();
    }
    if (const auto unhandled = std::get_if<std::exception_ptr> (&rejection)) {
        if (release_mailbox_reservation)
            release_mailbox_reservation ();
        // Earlier credits stay with their owners; only the untaken suffix settles here.
        for (std::size_t remaining = index; remaining < claim->claimed_messages; ++remaining) {
            auto &original = batch->entries[remaining].original;
            auto abandoned = original ? std::exchange (original, std::nullopt)
                                      : _transport->mailbox ().take ({claim, remaining});
            if (abandoned)
                abandoned->transferred ();
        }
        batch->finish ();
        std::rethrow_exception (*unhandled);
    }
    if (!entry.original)
        entry.original = _transport->mailbox ().take (slot);
    if (!entry.original)
        return;
    auto &mailbox_record = *entry.original;
    if (!release_mailbox_reservation)
        release_mailbox_reservation = std::exchange (mailbox_record.transferred, {});
    const auto failure = std::get_if<messaging::request_wire_failure_t> (&rejection);
    if (failure && !mailbox_record.source_routing_id.empty () && mailbox_record.correlation)
        (void) _transport->reply_failure (mailbox_record, failure->terminal_result,
                                          failure->failure_code);
    if (failure || !entry.retained.load (std::memory_order_acquire))
        release_mailbox_reservation ();
}

task_t<std::size_t>
public_host_runtime_t::continue_application_claim (std::shared_ptr<application_claim_batch_t> batch,
                                                   task_t<void> pending)
{
    try {
        co_await pending;
    }
    catch (...) {
        fail_application_claim (batch, std::current_exception ());
        ++batch->index;
    }
    auto next = dispatch_application_claim (batch);
    if (const auto count = std::get_if<std::size_t> (&next))
        co_return *count;
    co_return co_await std::get<task_t<std::size_t>> (std::move (next));
}

task_t<bool> public_host_runtime_t::dispatch_application_owner (
  std::string owner,
  std::function<void (
    const ready_record_t &, const receive_record_t &, std::vector<zlink::message_t>)> dispatch,
  std::function<void ()> started,
  std::function<void ()> rejected)
{
    std::shared_ptr<application_claim_batch_t> batch;
    std::size_t claimed = 0;
    const auto transfer = [&] (const auto &claim) {
        claimed = claim->claimed_messages;
        for (std::size_t index = 0; index < claimed; ++index)
            started ();
        batch = std::make_shared<application_claim_batch_t> (claimed);
        batch->claim = claim;
        batch->dispatch = std::move (dispatch);
        batch->rejected = std::move (rejected);
        batch->take_ordinary (claim, static_cast<bool> (_stateful_dispatch));
    };
    try {
        // Account the accepted claim even if its consumer storage cannot be allocated.
        auto claim = _transport->mailbox ().try_claim_owner (
          mesh::service_mailbox_domain_t::application, owner,
          dispatch_limits::receive_batch_messages, dispatch_limits::receive_batch_bytes,
          std::ref (transfer));
        if (!claim)
            return task_t<bool> (result_t<bool>::success (false));
        auto result = dispatch_application_claim (batch);
        if (std::holds_alternative<std::size_t> (result))
            return task_t<bool> (result_t<bool>::success (true));
        return [] (task_t<std::size_t> pending) -> task_t<bool> {
            (void) co_await pending;
            co_return true;
        }(std::get<task_t<std::size_t>> (std::move (result)));
    }
    catch (...) {
        if (batch)
            batch->finish ();
        else
            for (std::size_t index = 0; index < claimed; ++index)
                rejected ();
        return task_t<bool> (detail::current_exception_result<bool> ());
    }
}

bool public_host_runtime_t::wait_for_dispatch_activity (
  std::chrono::milliseconds timeout,
  bool accept_application_receive,
  std::optional<std::chrono::steady_clock::time_point> next_activity) noexcept
{
    try {
        if (next_activity) {
            const auto now = std::chrono::steady_clock::now ();
            const auto remaining =
              *next_activity <= now
                ? std::chrono::milliseconds::zero ()
                : std::chrono::ceil<std::chrono::milliseconds> (*next_activity - now);
            if (timeout < std::chrono::milliseconds::zero () || remaining < timeout)
                timeout = remaining;
        }
        return _transport->wait_for_activity (timeout, accept_application_receive);
    }
    catch (...) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        return false;
    }
}

task_t<std::pair<bool, std::optional<std::chrono::steady_clock::time_point>>>
public_host_runtime_t::next_dispatch_activity_async ()
{
    const auto local_pending = co_await _local_dispatch_completion_lane.run_task (
      [this] { return !_local_application_dispatches.empty (); });
    auto next = _relocation_wire->next_activity ();
    auto relocation_next = _relocation_next_due.load (std::memory_order_acquire);
    if (relocation_next == relocation_due_now) {
        co_await _relocation_session_terminal_lane.run_task (
          [this] { publish_relocation_next_due_on_lane (); });
        relocation_next = _relocation_next_due.load (std::memory_order_acquire);
    }
    if (relocation_next != relocation_never_due) {
        const auto deadline = relocation_next == relocation_due_now
                                ? std::chrono::steady_clock::now ()
                                : std::chrono::steady_clock::time_point (
                                    std::chrono::steady_clock::duration (relocation_next));
        if (!next || deadline < *next)
            next = deadline;
    }
    co_return std::make_pair (local_pending, next);
}

bool public_host_runtime_t::relocation_management_due (
  std::chrono::steady_clock::time_point now) const noexcept
{
    return now.time_since_epoch ().count ()
           >= _relocation_next_due.load (std::memory_order_acquire);
}

void public_host_runtime_t::publish_relocation_next_due_on_lane () const
{
    auto next = relocation_never_due;
    const auto include = [&next] (std::chrono::steady_clock::time_point deadline) {
        next = std::min (next, deadline.time_since_epoch ().count ());
    };
    for (const auto &[key, assembly] : _relocation_assemblies)
        include (assembly.expires_at);
    for (const auto &[key, attempt] : _relocation_target_attempts) {
        if (!relocation_target_terminal (attempt) && relocation_target_wire_ready (attempt))
            include (attempt.next_finalize_at);
    }
    for (const auto &[key, seal] : _session_seal_terminals) {
        if (seal.consumed)
            continue;
        if (!seal.ready) {
            next = relocation_due_now;
            break;
        }
        include (seal.expires_at);
    }
    _relocation_next_due.store (next, std::memory_order_release);
}

void public_host_runtime_t::signal_dispatch_activity () noexcept
{
    _transport->signal_activity ();
}

task_t<bool>
public_host_runtime_t::prepare_actor_transfer_async (const actor_transfer_prepare_t &prepare,
                                                     actor_transfer_token_t &token,
                                                     actor_transfer_prepare_result_t &result)
{
    auto actor = co_await resolve_actor_async (prepare.actor);
    if (!actor) {
        co_return false;
    }
    stateful::stateful_error_t error = stateful::stateful_error_t::invalid;
    stateful::membership_token_t membership;
    if (prepare.role == actor_transfer_role_t::source) {
        std::tie (error, membership) = co_await _objects.begin_remote_membership_move_async (
          *actor, stateful::object_ref_t{
                    stateful::object_kind_t::user_spot, prepare.target_spot_id,
                    prepare.target_spot_generation, prepare.target_spot_generation,
                    _options.mesh.descriptor.mesh_name, prepare.target_node_rid.to_string ()});
    } else {
        auto target = co_await resolve_spot_async (prepare.target_spot_id);
        if (!target)
            co_return false;
        std::tie (error, membership) =
          co_await _objects.begin_membership_move_async (*actor, *target);
    }
    if (error != stateful::stateful_error_t::none) {
        co_return false;
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
    co_return true;
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


task_t<std::optional<stateful::object_ref_t>>
public_host_runtime_t::resolve_actor_async (const actor_ref_t &actor) const
{
    auto current = co_await resolve_actor_async (std::string (actor.actor_id ().value ()));
    if (current && current->object_generation != actor.object_generation ())
        co_return std::nullopt;
    co_return current;
}


task_t<std::optional<stateful::object_ref_t>>
public_host_runtime_t::resolve_actor_async (std::string actor_id) const
{
    return _spot_actor_index_lane.run_task (
      [this, actor_id = std::move (actor_id)] () -> std::optional<stateful::object_ref_t> {
          const auto found = _actors.find (actor_id);
          if (found == _actors.end ()) {
              return std::nullopt;
          }
          return found->second.second;
      });
}

task_t<std::optional<stateful::object_ref_t>>
public_host_runtime_t::resolve_spot_async (std::string spot_id) const
{
    return _objects.resolve_spot_async (std::move (spot_id));
}

protocol::application_payload_t
public_host_runtime_t::encode_application (const std::vector<zlink::message_t> &parts,
                                           std::span<const std::uint8_t>) const
{
    return protocol::application_payload_t::from_parts (parts);
}

protocol::application_payload_t
public_host_runtime_t::encode_application (std::vector<zlink::message_t> &&parts,
                                           std::span<const std::uint8_t>) const
{
    return protocol::application_payload_t::from_parts (std::move (parts));
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
    return {_options.mesh.descriptor.lifecycle_generation, low};
}

task_t<std::optional<mesh::raw_mesh_node_owner_t::local_operation_reservation_t>>
public_host_runtime_t::prepare_local_completion (pending_operation_t &operation)
{
    operation.prepare_for_registration ();
    return _transport->prepare_local_operation_async ();
}

void public_host_runtime_t::register_local_completion (
  pending_operation_t &operation,
  const mesh::raw_mesh_node_owner_t::local_operation_reservation_t &reservation,
  std::chrono::milliseconds timeout,
  spot_request_completion_t completion,
  std::function<void ()> incomplete,
  mesh_request_surface_t request_surface)
{
    const auto source = operation.completion;
    const auto result = std::make_shared<operation_completion_t> ();
    result->record.kind = record_kind_t::completion;
    result->record.domain = ready_domain_t::infrastructure;
    result->record.operation_id = reservation.first;
    result->record.source_node_rid =
      zlink::routing_id_t::from (_options.mesh.descriptor.node_routing_id);
    const auto registered = _transport->register_prepared_local_operation (
      reservation, foundation::operation_registry_t::clock_t::now () + timeout,
      [source, result, completion = std::move (completion),
       incomplete] (foundation::operation_terminal_t terminal, zlink::message_t payload) mutable {
          std::string terminal_message;
          if (terminal == foundation::operation_terminal_t::completed && payload.size () != 0) {
              try {
                  result->parts = protocol::decode_application_parts (std::move (payload));
              }
              catch (const protocol::service_wire_error_t &error) {
                  terminal = foundation::operation_terminal_t::transport_failed;
                  terminal_message = error.what ();
              }
          }
          if (terminal != foundation::operation_terminal_t::completed && terminal_message.empty ())
              terminal_message = "Local operation did not complete";
          if (terminal != foundation::operation_terminal_t::completed && incomplete) {
              try {
                  incomplete ();
              }
              catch (const std::exception &error) {
                  terminal_message += ": rollback failed: ";
                  terminal_message += error.what ();
              }
              catch (...) {
                  terminal_message += ": rollback failed with an unknown exception";
              }
          }
          if (completion) {
              auto decoded =
                terminal == foundation::operation_terminal_t::completed
                  ? result_t<std::vector<zlink::message_t>>::success (std::move (result->parts))
                : terminal == foundation::operation_terminal_t::shutdown
                  ? detail::boundary_failure<std::vector<zlink::message_t>> (
                      detail::boundary_error_t::shutdown, terminal_message)
                : terminal == foundation::operation_terminal_t::timed_out
                  ? detail::boundary_failure<std::vector<zlink::message_t>> (
                      detail::boundary_error_t::timed_out, terminal_message)
                  : result_t<std::vector<zlink::message_t>>::failure (
                      framework_error_kind_t::internal_failure, terminal_message);
              completion (terminal, std::move (decoded));
          } else if (terminal == foundation::operation_terminal_t::completed) {
              source->complete (result_t<operation_completion_t>::success (std::move (*result)));
          } else if (terminal == foundation::operation_terminal_t::shutdown) {
              source->complete (detail::boundary_failure<operation_completion_t> (
                detail::boundary_error_t::shutdown, terminal_message));
          } else if (terminal == foundation::operation_terminal_t::timed_out) {
              source->complete (detail::boundary_failure<operation_completion_t> (
                detail::boundary_error_t::timed_out, terminal_message));
          } else {
              source->complete (result_t<operation_completion_t>::failure (
                framework_error_kind_t::internal_failure, terminal_message));
          }
      },
      request_surface);
    if (!registered)
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "Operation completion registry is closed");
    operation.id = reservation.first;
    operation.local_result = result;
}


bool public_host_runtime_t::enqueue_completion (const pending_operation_t &operation,
                                                receive_record_t record,
                                                std::vector<zlink::message_t> parts)
{
    return _transport->complete_local_operation (
      operation.id, [result = operation.local_result, record = std::move (record),
                     parts = std::move (parts)] () mutable noexcept {
          *result = operation_completion_t{std::move (record), std::move (parts)};
      });
}

task_t<zlink::submit_result_t>
public_host_runtime_t::begin_local_actor_join (const actor_ref_t &actor,
                                               const std::string &target_spot_id,
                                               std::uint64_t target_spot_generation,
                                               const std::vector<zlink::message_t> &parts,
                                               pending_operation_t &operation,
                                               std::chrono::milliseconds timeout)
{
    const auto current = co_await resolve_actor_async (actor);
    auto target = co_await _objects.resolve_spot_async (target_spot_id);
    // The objects owner validates the caller's target incarnation as it begins
    // the move. Do not make a second generation decision in the host lane.
    if (target)
        target->object_generation = target_spot_generation;
    auto [error, membership] =
      std::pair{stateful::stateful_error_t::not_found, stateful::membership_token_t{}};
    if (current && target)
        std::tie (error, membership) =
          co_await _objects.begin_membership_move_async (*current, *target);
    auto waiter = membership.value != 0 && _options.mesh.application_jobs
                    ? std::make_shared<application_job_queue_t::waiter_t> ()
                    : nullptr;
    const auto reservation = co_await prepare_local_completion (operation);
    if (!reservation) {
        if (membership.value != 0)
            (void) co_await _objects.abort_membership_move_async (membership);
        co_return zlink::submit_result_t::terminated;
    }
    task_t<zlink::submit_result_t> dispatch{
      result_t<zlink::submit_result_t>::success (zlink::submit_result_t::terminated)};
    std::exception_ptr failure;
    try {
        co_await _lifecycle_configuration_lane.run_task ([&] {
            if (!_started || _closing)
                return;
            try {
                register_local_completion (
                  operation, *reservation, timeout, {},
                  [host = shared_from_this (), membership,
                   cleanup = local_request_wait_cleanup (waiter)] {
                      if (cleanup)
                          cleanup ();
                      if (membership.value != 0) {
                          const auto abort =
                            [] (std::shared_ptr<public_host_runtime_t> owner,
                                stateful::membership_token_t move) -> task_t<void> {
                              (void) co_await owner->_objects.abort_membership_move_async (move);
                          };
                          (void) abort (host, membership);
                      }
                  });
            }
            catch (const framework_exception_t &failure) {
                if (failure.kind () == framework_error_kind_t::shutting_down)
                    return;
                throw;
            }

            //  Spec 32-framework-error-model:129-136 — typed Rejected is reserved for
            //  the application callback decision. A Framework prerequisite failure
            //  carries a classified wire terminal on the completion instead of a
            //  synthesized rejection, and the consumer maps it to the public kind.
            auto fail = [&] (std::uint32_t terminal_result, std::uint32_t failure_errno) {
                receive_record_t completion;
                completion.kind = record_kind_t::completion;
                completion.domain = ready_domain_t::infrastructure;
                completion.operation_id = operation.id;
                completion.operation_kind = operation_kind_t::actor_join;
                completion.source_node_rid = status ().routing_id ();
                completion.terminal_result = static_cast<int> (terminal_result);
                completion.failure_errno = static_cast<int> (failure_errno);
                (void) enqueue_completion (operation, std::move (completion), {});
                dispatch = task_t<zlink::submit_result_t>{
                  result_t<zlink::submit_result_t>::success (zlink::submit_result_t::ok)};
            };
            if (error != stateful::stateful_error_t::none) {
                const auto classified = stateful_failure_pair (error);
                fail (classified.first, classified.second);
                return;
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
            record.operation_id = operation.id;
            record.operation_kind = operation_kind_t::actor_join;
            record.source_node_rid = status ().routing_id ();
            record.actor_control = actor_control_t{lifecycle_kind_t::joined, actor};
            record.reply_token.local_actor_join = [weak, operation, actor_type, membership] (
                                                    const actor_join_operation_result_t &result,
                                                    const std::vector<zlink::message_t> &reply) {
                const auto host = weak.lock ();
                return host
                       && host->complete_local_actor_join (operation, actor_type, membership,
                                                           result, reply);
            };
            dispatch = _local_dispatch_completion_lane.run_task (
              [&, owner = std::move (owner), record = std::move (record), waiter] () mutable {
                  admit_local_application (
                    local_application_dispatch_t{std::move (owner), std::move (record), parts},
                    waiter);
                  _transport->signal_activity ();
                  return zlink::submit_result_t::ok;
              });
        });
        const auto submitted = co_await dispatch;
        if (submitted != zlink::submit_result_t::ok) {
            _transport->unregister_local_operation (operation.id);
            if (membership.value != 0)
                (void) co_await _objects.abort_membership_move_async (membership);
        }
        co_return submitted;
    }
    catch (...) {
        failure = std::current_exception ();
    }
    _transport->unregister_local_operation (operation.id);
    if (membership.value != 0)
        (void) co_await _objects.abort_membership_move_async (membership);
    std::rethrow_exception (failure);
}

bool public_host_runtime_t::complete_local_actor_join (pending_operation_t operation,
                                                       std::string actor_type,
                                                       stateful::membership_token_t membership,
                                                       const actor_join_operation_result_t &result,
                                                       const std::vector<zlink::message_t> &parts)
{
    const bool completed = _transport->complete_local_operation_async (
      operation.id,
      [host = shared_from_this (), operation, actor_type = std::move (actor_type), membership,
       result, parts] () mutable -> task_t<void> {
          auto *self = host.get ();
          receive_record_t completion;
          completion.kind = record_kind_t::completion;
          completion.domain = ready_domain_t::infrastructure;
          completion.operation_id = operation.id;
          completion.operation_kind = operation_kind_t::actor_join;
          completion.source_node_rid =
            zlink::routing_id_t::from (self->_options.mesh.descriptor.node_routing_id);

          if (result.terminal_result != 0) {
              (void) co_await self->_objects.abort_membership_move_async (membership);
              completion.terminal_result = result.terminal_result;
              completion.failure_errno = result.failure_code;
          } else if (result.join_result == protocol::actor_join_result_t::accepted) {
              auto [error, current] =
                co_await self->_objects.commit_membership_move_async (membership);
              if (error != stateful::stateful_error_t::none) {
                  //  Spec 32-framework-error-model:119-120 — a Framework execution
                  //  failure after the application ACCEPTED the join is not an
                  //  application rejection; carry a classified terminal
                  //  (internalError) instead of synthesizing typed Rejected.
                  const auto failure = messaging::request_failure_mapper_t{}.target_failure_reply (
                    framework_error_kind_t::internal_failure);
                  completion.terminal_result = failure->terminal_result;
                  completion.failure_errno = failure->failure_code;
              } else {
                  const auto actor = self->framework_actor_ref (current, actor_type);
                  co_await self->_spot_actor_index_lane.run_task ([&] {
                      const auto found = self->_actors.find (current.key);
                      if (found != self->_actors.end ())
                          found->second.second = current;
                  });
                  completion.join_completion =
                    actor_join_completion_t{join_admission_t::accepted, actor};
              }
          } else {
              (void) co_await self->_objects.abort_membership_move_async (membership);
              completion.join_completion =
                actor_join_completion_t{join_admission_t::rejected,
                                        self->framework_actor_ref (membership.actor, actor_type)};
          }

          *operation.local_result =
            operation_completion_t{std::move (completion), std::move (parts)};
      });
    return completed;
}

std::function<void ()> public_host_runtime_t::local_request_wait_cleanup (
  const std::shared_ptr<application_job_queue_t::waiter_t> &waiter)
{
    if (!waiter)
        return {};
    std::weak_ptr<public_host_runtime_t> weak = shared_from_this ();
    return [weak, waiter] {
        if (auto host = weak.lock ())
            host->_local_dispatch_completion_lane.try_post ([waiter] { (void) waiter->cancel (); });
    };
}

void public_host_runtime_t::publish_local_application (local_application_dispatch_t dispatch)
{
    if (!_stateful_dispatch || !is_stateful_application (dispatch.record.kind)) {
        _local_application_dispatches.push_back (std::move (dispatch));
        return;
    }
    mesh::service_mailbox_record_t record;
    record.owner = mesh::service_mailbox_t::application_owner (dispatch.owner);
    record.domain = mesh::service_mailbox_domain_t::application;
    record.before_application_handler = dispatch.record.before_application_handler;
    record.source_routing_id = dispatch.record.source_node_rid.to_bytes ();
    record.operation =
      std::pair{dispatch.record.operation_id.high, dispatch.record.operation_id.low};
    if (is_request (dispatch.record.kind))
        record.correlation = dispatch.record.reply_route_id;
    record.source_original = mesh::raw_mesh_node_owner_t::local_accepted_source (
      record.source_routing_id, _options.mesh.descriptor.lifecycle_generation,
      {_local_lease_observation ? _local_lease_observation () : nullptr,
       std::shared_ptr<const mesh::service_node_descriptor_t> (shared_from_this (),
                                                               &_options.mesh.descriptor)});
    record.application = std::make_shared<local_application_dispatch_t> (std::move (dispatch));
    if (!_transport->mailbox ().try_enqueue (std::move (record)))
        throw protocol::service_wire_error_t ("local application mailbox is closed");
}

void public_host_runtime_t::configure_relocation_gate (
  relocation_gate_t gate,
  native_spot_target_t target,
  mesh::service_mailbox_t::application_dispatch_t resume)
{
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started || _closing)
              throw std::logic_error ("relocation gate must be configured before start");
          _relocation_gate = std::move (gate);
          _native_spot_target = std::move (target);
          _relocation_resume = std::move (resume);
      })
      .get ();
}

task_t<std::optional<std::vector<std::vector<stateful::turn_record_t>>>>
public_host_runtime_t::cut_relocation_source (
  std::vector<stateful::object_ref_t> participants,
  std::shared_ptr<stateful::relocation_seal_binding_t> binding)
{
    const auto [locate, resume] = co_await _lifecycle_configuration_lane.run_task (
      [this] { return std::pair{_relocation_gate, _relocation_resume}; });
    // Aggregate units contain one SpotWide Spot and its Actors. Actor FIFOs use
    // that Spot's shared gate; their own suffixes are captured by the Actor owner.
    const auto principal =
      std::find_if (participants.begin (), participants.end (), [] (const auto &owner) {
          return owner.kind != stateful::object_kind_t::actor;
      });
    const auto gate_index = static_cast<std::size_t> (principal - participants.begin ());
    std::shared_ptr<runtime::serial_execution_queue_t> gate;
    if (locate && principal != participants.end ())
        gate = co_await locate (*principal);
    if (gate) {
        binding->finish = [this, weak_gate = std::weak_ptr (gate), weak = std::weak_ptr (binding),
                           owner = *principal, resume] (bool restored) -> task_t<void> {
            const auto binding = weak.lock ();
            const auto gate = weak_gate.lock ();
            if (!binding || !gate)
                co_return;
            if (restored && resume) {
                (void) co_await dispatch_stateful_owner (owner, resume);
            } else if (!restored) {
                (void) co_await gate->release_relocation_seal (binding);
            }
        };
    }
    std::vector<std::string> owners;
    owners.reserve (participants.size ());
    for (const auto &participant : participants)
        owners.push_back (mesh::service_mailbox_t::application_owner (
          participant.kind == stateful::object_kind_t::actor ? owner_kind_t::actor
                                                             : owner_kind_t::spot,
          participant.key));
    // The cut takes only originals the stateful ingress places in a source FIFO.
    const auto admits = [] (const mesh::service_mailbox_record_t &original) {
        if (original.application) {
            const auto &record = original.application->record;
            return !record.application_record && is_stateful_application (record.kind);
        }
        if (original.parts.size () != 2)
            return false;
        try {
            const auto kind = record_kind (
              protocol::decode_header (protocol::message_bytes (original.parts.front ())).kind);
            return kind && is_stateful_application (*kind);
        }
        catch (...) {
            return false;
        }
    };
    std::optional<std::vector<std::vector<mesh::service_mailbox_record_t>>> taken;
    const auto bind = [&binding] { binding->bound.store (true, std::memory_order_release); };
    // Producer order: the mailbox, then the owner's own producer boundary inside step.
    const auto cut = [&] (const std::function<void ()> &step) {
        taken = _transport->mailbox ().cut_owners (owners, admits, step);
        return taken.has_value ();
    };
    std::vector<std::shared_ptr<runtime::serial_application_record_t>> suffix;
    if (gate)
        suffix = co_await gate->take_application_suffix (bind, cut, binding);
    else
        (void) cut (bind);
    if (!taken)
        co_return std::nullopt;
    // Originals accepted before the cut enter their source FIFOs in mailbox order, ahead of
    // the ingress held after it; each keeps its reply route and mailbox credit.
    for (std::size_t index = 0; index != participants.size (); ++index)
        for (auto &original : (*taken)[index])
            co_await ingest_cut_original (participants[index], std::move (original));
    // The owner's exported suffix keeps its accepted owner, like the Actor suffix.
    std::vector<std::vector<stateful::turn_record_t>> exported (participants.size ());
    for (auto &record : suffix) {
        stateful::turn_record_t turn;
        if (record->invocation ())
            turn.application_payload_bytes = record->invocation ()->first.size ();
        else if (record->payload ()) {
            turn = std::move (*record->payload ());
            record->payload ().reset ();
        }
        turn.accepted_owner = std::move (record);
        exported[gate_index].push_back (std::move (turn));
    }
    co_return exported;
}

task_t<void> public_host_runtime_t::ingest_cut_original (stateful::object_ref_t owner,
                                                         mesh::service_mailbox_record_t original)
{
    if (!original.application) {
        auto retained = co_await _objects.retained_relocation_source (owner);
        (void) co_await _stateful_dispatch->ingest_claimed (std::move (owner), std::move (original),
                                                            std::move (retained));
        co_return;
    }
    std::optional<mesh::service_mailbox_record_t> taken;
    std::optional<messaging::request_wire_failure_t> failure;
    taken.emplace (std::move (original));
    try {
        auto query = local_application_query (*taken->application);
        std::shared_ptr<const local_application_dispatch_t> application = taken->application;
        auto source = taken->source_original;
        (void) co_await accept_local_application (std::move (*taken), std::move (query),
                                                  std::move (application), std::move (source),
                                                  taken);
        co_return;
    }
    catch (const protocol::service_wire_error_t &) {
        failure = messaging::request_wire_failure_t{
          static_cast<std::uint32_t> (protocol::request_terminal_result::protocolError),
          static_cast<std::uint32_t> (protocol::framework_error_code::requestProtocolError)};
    }
    catch (const framework_exception_t &error) {
        if (error.kind () == framework_error_kind_t::internal_failure)
            _options.mesh.runtime_failures->report (std::current_exception ());
        failure = messaging::request_failure_mapper_t{}.target_failure_reply (error);
    }
    catch (...) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        failure = messaging::request_failure_mapper_t{}.target_failure_reply (
          framework_error_kind_t::internal_failure);
    }
    // A rejected original is answered once from the record this ingress still owns.
    if (!taken)
        co_return;
    const auto credit = std::exchange (taken->transferred, {});
    if (failure && !taken->source_routing_id.empty () && taken->correlation)
        (void) _transport->reply_failure (*taken, failure->terminal_result, failure->failure_code);
    if (credit)
        (void) credit ();
}

task_t<std::optional<stateful::object_ref_t>> public_host_runtime_t::accept_local_application (
  stateful::raw_stateful_dispatch_t::ingress_original_t ingress,
  local_application_dispatch_t dispatch,
  std::shared_ptr<const local_application_dispatch_t> application,
  std::optional<mesh::request_source_t> source_original,
  std::optional<mesh::service_mailbox_record_t> &original)
{
    // An owned original stays with the caller until the state lane takes it.
    if (auto *const owned = std::get_if<mesh::service_mailbox_record_t> (&ingress))
        original.emplace (std::move (*owned));
    const bool actor = is_actor_application (dispatch.record.kind);
    std::optional<stateful::retained_relocation_source_t> retained;
    std::optional<stateful::object_ref_t> owner;
    if (actor)
        owner = co_await _objects.find_async (
          stateful::object_kind_t::actor, std::string (dispatch.owner.actor->actor_id ().value ()),
          std::nullopt, &retained);
    else
        owner = co_await _objects.resolve_spot_async (dispatch.owner.spot_id, &retained);
    if (!owner)
        throw protocol::service_wire_error_t ("local accepted owner is unavailable");
    std::variant<protocol::actor_message_header_t, protocol::spot_message_header_t> header;
    if (actor) {
        protocol::actor_message_header_t value;
        value.target = *dispatch.record.actor_route;
        value.operation = {dispatch.record.operation_id.high, dispatch.record.operation_id.low};
        if (is_request (dispatch.record.kind))
            value.correlation = dispatch.record.reply_route_id;
        if (dispatch.record.source_session_rid)
            value.bound_session_source = protocol::actor_message_header_t::bound_session_source_t{
              dispatch.record.source_session_rid->to_bytes (),
              dispatch.record.source_binding_generation, dispatch.record.source_session_sequence};
        header = std::move (value);
    } else {
        header = local_spot_header (dispatch);
    }
    const auto [validation, authority] = co_await std::visit (
      [&] (const auto &value) {
          if constexpr (std::is_same_v<std::decay_t<decltype (value)>,
                                       protocol::actor_message_header_t>)
              return _stateful_dispatch->accepted_actor_authority (
                *owner, value, dispatch.record.source_node_rid.to_bytes (),
                _options.mesh.descriptor.lifecycle_generation, std::move (source_original), false,
                std::move (retained));
          else
              return _stateful_dispatch->accepted_spot_authority (
                *owner, value, dispatch.record.source_node_rid.to_bytes (),
                _options.mesh.descriptor.lifecycle_generation, std::move (source_original),
                std::move (retained));
      },
      header);
    if (validation != stateful::stateful_error_t::none) {
        if (auto *const slot = std::get_if<mesh::service_mailbox_t::claim_handle_t> (&ingress))
            original = _transport->mailbox ().take (*slot);
        if (!original)
            co_return std::nullopt;
        // The rejected original returns its mailbox credit at this accept boundary.
        if (const auto credit = std::exchange (original->transferred, {}))
            (void) credit ();
        throw framework_exception_t (validation == stateful::stateful_error_t::generation_stale
                                       ? framework_error_kind_t::invalid_operation
                                       : framework_error_kind_t::unavailable,
                                     "local accepted object source authority is unavailable");
    }
    // The accepted record reads the shared original; the state lane takes it.
    auto accepted_record = std::make_shared<runtime::serial_application_record_t> ();
    accepted_record->admission.jobs = _options.mesh.application_jobs;
    accepted_record->payload ().emplace ();
    accepted_record->payload ()->application_record = std::visit (
      [&] (auto &value) {
          value.target.owner_lease_generation = authority->target_owner_lease_generation;
          if constexpr (std::is_same_v<std::decay_t<decltype (value)>,
                                       protocol::actor_message_header_t>)
              return stateful::accepted_actor_record (value, authority->source,
                                                      encode_application (application->parts),
                                                      is_request (dispatch.record.kind));
          else
              return stateful::accepted_spot_record (value, authority->source,
                                                     encode_application (application->parts),
                                                     is_request (dispatch.record.kind));
      },
      header);
    std::function<bool (const std::optional<protocol::application_payload_t> &)> terminal;
    if (application->record.reply_token.local_reply) {
        // The original application keeps its local reply route until the terminal.
        terminal = [weak = weak_from_this (), source = dispatch.record.source_node_rid.to_bytes (),
                    correlation = dispatch.record.reply_route_id,
                    application] (const std::optional<protocol::application_payload_t> &reply) {
            if (reply)
                return application->record.reply_token.local_reply (
                  protocol::decode_application_parts (*reply));
            const auto host = weak.lock ();
            if (!host)
                return false;
            mesh::service_mailbox_record_t request;
            request.source_routing_id = source;
            request.correlation = correlation;
            return host->_transport->reply_failure (
              request,
              static_cast<std::uint32_t> (protocol::request_terminal_result::internalError),
              static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed));
        };
    }
    if (original) {
        ingress = std::move (*original);
        original.reset ();
    }
    const auto accepted = co_await _stateful_dispatch->accept_local (
      *owner, std::move (accepted_record), std::move (ingress), original, std::move (terminal));
    if (!original)
        co_return std::nullopt;
    if (accepted != stateful::stateful_error_t::none)
        throw protocol::service_wire_error_t ("local accepted Actor enqueue failed");
    co_return *owner;
}

task_t<void> public_host_runtime_t::enqueue_local_multicast (
  task_t<std::vector<stateful::object_ref_t>> target_snapshot,
  std::string channel_name,
  std::string topic,
  std::vector<zlink::message_t> parts,
  std::shared_ptr<const std::vector<std::uint8_t>> metadata,
  zlink::routing_id_t source_node,
  std::function<void ()> before_application_handler)
{
    const auto targets = co_await target_snapshot;
    const auto local = _transport->topology ().local_descriptor ();
    for (const auto &target : targets) {
        if (!co_await _objects.contains_async (target))
            continue;
        ready_record_t owner;
        owner.owner_kind = owner_kind_t::spot;
        owner.domain = ready_domain_t::application;
        owner.spot_id = target.key;
        receive_record_t record;
        record.kind = record_kind_t::spot_multicast;
        record.source_node_rid = source_node;
        record.channel_name = channel_name;
        record.topic = topic;
        record.spot_route = protocol::spot_route_fence_t{
          target.key, target.object_generation, local.node_routing_id, local.lifecycle_generation,
          target.authority_owner_generation};
        record.metadata = metadata;
        record.before_application_handler = std::move (before_application_handler);
        auto completion = _options.mesh.application_jobs
                            ? std::make_shared<task_completion_source_t<void>> ()
                            : std::shared_ptr<task_completion_source_t<void>>{};
        const auto enqueue = [&] {
            admit_local_application (
              local_application_dispatch_t{std::move (owner), std::move (record), parts}, {},
              completion);
        };
        if (_local_dispatch_completion_lane.is_on_lane ())
            enqueue ();
        else
            co_await _local_dispatch_completion_lane.run_task (enqueue);
        _transport->signal_activity ();
        if (completion)
            co_await completion->task ();
    }
}

void public_host_runtime_t::admit_local_application (
  local_application_dispatch_t dispatch,
  const std::shared_ptr<application_job_queue_t::waiter_t> &waiter,
  std::shared_ptr<task_completion_source_t<void>> admitted)
{
    if (!_options.mesh.application_jobs || dispatch.record.before_application_handler) {
        publish_local_application (std::move (dispatch));
        if (admitted)
            admitted->complete (result_t<void>::success ());
        return;
    }
    auto pending = waiter ? waiter : std::make_shared<application_job_queue_t::waiter_t> ();
    std::weak_ptr<public_host_runtime_t> weak = shared_from_this ();
    *pending = _options.mesh.application_jobs->wait_for_supply (
      [weak, waiter = pending, dispatch = std::move (dispatch),
       admitted] (std::optional<application_job_queue_t::permit_t> reserved) mutable {
          auto waiting = std::move (waiter);
          if (!reserved) {
              if (admitted)
                  admitted->complete (
                    result_t<void>::failure (framework_error_kind_t::unavailable,
                                             "application queue stopped before admission"));
              return;
          }
          auto host = weak.lock ();
          if (!host) {
              if (admitted)
                  admitted->complete (
                    result_t<void>::failure (framework_error_kind_t::unavailable,
                                             "application owner closed before admission"));
              return;
          }
          auto permit = std::make_shared<application_job_queue_t::permit_t> (std::move (*reserved));
          auto publish = [weak, waiting, permit, dispatch = std::move (dispatch),
                          admitted] () mutable {
              *waiting = {};
              auto host = weak.lock ();
              if (!host || !host->_transport->started ()) {
                  if (admitted)
                      admitted->complete (
                        result_t<void>::failure (framework_error_kind_t::unavailable,
                                                 "application owner closed before admission"));
                  return;
              }
              if (uses_local_operation_registry (dispatch.record.kind)
                  && !host->_transport->operation_pending (dispatch.record.operation_id)) {
                  if (admitted)
                      admitted->complete (
                        result_t<void>::failure (framework_error_kind_t::unavailable,
                                                 "application operation ended before admission"));
                  return;
              }
              permit->mark_queued ();
              dispatch.record.before_application_handler = [permit] {
                  permit->release_for_handler_entry ();
              };
              host->publish_local_application (std::move (dispatch));
              host->_transport->signal_activity ();
              if (admitted)
                  admitted->complete (result_t<void>::success ());
          };
          if (host->_local_dispatch_completion_lane.is_on_lane ())
              publish ();
          else if (!host->_local_dispatch_completion_lane.try_post (std::move (publish))
                   && admitted)
              admitted->complete (result_t<void>::failure (framework_error_kind_t::unavailable,
                                                           "application admission lane closed"));
      },
      application_job_queue_t::origin_t::local);
}

task_t<zlink::submit_result_t> public_host_runtime_t::enqueue_local_actor_message (
  const actor_ref_t &target,
  record_kind_t kind,
  const std::vector<zlink::message_t> &parts,
  pending_operation_t *operation,
  std::optional<protocol::actor_message_header_t::bound_session_source_t> bound_session_source,
  std::chrono::milliseconds timeout,
  std::uint64_t authority_owner_generation,
  std::uint64_t owner_lease_generation)
{
    if (!is_actor_application (kind)) {
        co_return zlink::submit_result_t::invalid_argument;
    }
    // The Actor execution gate holds payloads during recovery and completion.
    // Resolve identity here without a second Ready-only admission decision.
    const auto current = co_await resolve_actor_async (std::string (target.actor_id ().value ()));
    if (!current) {
        co_return zlink::submit_result_t::not_found;
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

    auto waiter = _options.mesh.application_jobs
                    ? std::make_shared<application_job_queue_t::waiter_t> ()
                    : nullptr;
    std::optional<mesh::raw_mesh_node_owner_t::local_operation_reservation_t> reservation;
    if (operation) {
        reservation = co_await prepare_local_completion (*operation);
        if (!reservation)
            co_return zlink::submit_result_t::terminated;
    } else if (_stateful_dispatch) {
        const auto sequence = co_await _transport->next_operation_sequence_async ();
        if (!sequence)
            co_return zlink::submit_result_t::terminated;
        record.operation_id = {_options.mesh.descriptor.lifecycle_generation, *sequence};
    }

    task_t<zlink::submit_result_t> dispatch{
      result_t<zlink::submit_result_t>::success (zlink::submit_result_t::terminated)};
    try {
        co_await _lifecycle_configuration_lane.run_task ([&] {
            if (!_started || _closing)
                return;
            if (operation) {
                register_local_completion (*operation, *reservation, timeout, {},
                                           local_request_wait_cleanup (waiter),
                                           mesh_request_surface_t::actor);
                record.operation_id = operation->id;
                std::weak_ptr<public_host_runtime_t> weak = shared_from_this ();
                record.reply_token.host = weak;
                record.reply_token.local_reply =
                  [weak, operation = *operation] (const std::vector<zlink::message_t> &reply) {
                      const auto host = weak.lock ();
                      return host && host->complete_local_request (operation, reply);
                  };
            }
            dispatch = _local_dispatch_completion_lane.run_task ([&] {
                if (_stateful_dispatch) {
                    record.actor_route = {current->key,
                                          current->object_generation,
                                          _options.mesh.descriptor.node_routing_id,
                                          _options.mesh.descriptor.lifecycle_generation,
                                          authority_owner_generation != 0
                                            ? authority_owner_generation
                                            : current->authority_owner_generation,
                                          owner_lease_generation};
                    if (operation)
                        record.reply_route_id = operation->id.low;
                }
                admit_local_application (
                  local_application_dispatch_t{std::move (owner), std::move (record), parts},
                  waiter);

                return zlink::submit_result_t::ok;
            });
        });
        const auto submitted = co_await dispatch;
        if (submitted != zlink::submit_result_t::ok) {
            if (operation)
                _transport->unregister_local_operation (operation->id);
        }
        if (submitted == zlink::submit_result_t::ok)
            _transport->signal_activity ();
        co_return submitted;
    }
    catch (...) {
        if (operation)
            _transport->unregister_local_operation (operation->id);
        throw;
    }
}

task_t<zlink::submit_result_t>
public_host_runtime_t::enqueue_local_spot_send (const protocol::spot_route_fence_t &target,
                                                const std::vector<zlink::message_t> &parts,
                                                std::string source_spot_id)
{
    const auto local = status ();
    if (target.target_node_routing_id != local.routing_id ().to_bytes ()
        || target.target_node_generation != local.lifecycle_generation ()) {
        co_return zlink::submit_result_t::not_found;
    }
    const auto object = co_await resolve_spot_async (target.spot_id);
    if (!object || object->node_id != local.routing_id ().to_string ()) {
        co_return zlink::submit_result_t::not_found;
    }

    ready_record_t owner{.owner_kind = owner_kind_t::spot,
                         .domain = ready_domain_t::application,
                         .spot_id = target.spot_id};
    receive_record_t record;
    record.kind = record_kind_t::spot_send;
    record.domain = ready_domain_t::application;
    record.source_node_rid = local.routing_id ();
    record.spot_route = target;
    const auto sequence = co_await _transport->next_operation_sequence_async ();
    if (!sequence)
        co_return zlink::submit_result_t::terminated;
    record.operation_id = {_options.mesh.descriptor.lifecycle_generation, *sequence};

    task_t<zlink::submit_result_t> dispatch{
      result_t<zlink::submit_result_t>::success (zlink::submit_result_t::terminated)};
    co_await _lifecycle_configuration_lane.run_task ([&] {
        if (!_started || _closing)
            return;
        dispatch = _local_dispatch_completion_lane.run_task ([&] {
            admit_local_application (local_application_dispatch_t{
              std::move (owner), std::move (record), parts, std::move (source_spot_id)});
            return zlink::submit_result_t::ok;
        });
    });
    const auto submitted = co_await dispatch;
    if (submitted == zlink::submit_result_t::ok)
        _transport->signal_activity ();
    co_return submitted;
}

task_t<zlink::submit_result_t>
public_host_runtime_t::enqueue_local_spot_request (const protocol::spot_route_fence_t &target,
                                                   const std::vector<zlink::message_t> &parts,
                                                   pending_operation_t &operation,
                                                   std::chrono::milliseconds timeout,
                                                   std::span<const std::uint8_t> metadata,
                                                   spot_request_completion_t completion,
                                                   std::string source_spot_id)
{
    const auto local = status ();
    if (target.target_node_routing_id != local.routing_id ().to_bytes ()
        || target.target_node_generation != local.lifecycle_generation ()) {
        co_return zlink::submit_result_t::not_found;
    }
    const auto object = co_await resolve_spot_async (target.spot_id);
    if (!object || object->node_id != local.routing_id ().to_string ()) {
        co_return zlink::submit_result_t::not_found;
    }

    ready_record_t owner{.owner_kind = owner_kind_t::spot,
                         .domain = ready_domain_t::application,
                         .spot_id = target.spot_id};
    receive_record_t record;
    record.kind = record_kind_t::spot_request;
    record.domain = ready_domain_t::application;
    record.source_node_rid = local.routing_id ();
    record.spot_route = target;

    auto waiter = _options.mesh.application_jobs
                    ? std::make_shared<application_job_queue_t::waiter_t> ()
                    : nullptr;
    const auto reservation = co_await prepare_local_completion (operation);
    if (!reservation)
        co_return zlink::submit_result_t::terminated;
    task_t<zlink::submit_result_t> dispatch{
      result_t<zlink::submit_result_t>::success (zlink::submit_result_t::terminated)};
    try {
        co_await _lifecycle_configuration_lane.run_task ([&] {
            if (!_started || _closing)
                return;
            register_local_completion (operation, *reservation, timeout, std::move (completion),
                                       local_request_wait_cleanup (waiter),
                                       mesh_request_surface_t::spot);
            record.operation_id = operation.id;
            record.reply_route_id = operation.id.low;
            std::weak_ptr<public_host_runtime_t> weak = shared_from_this ();
            record.reply_token.host = weak;
            record.reply_token.local_reply =
              [weak, operation] (const std::vector<zlink::message_t> &reply) {
                  const auto host = weak.lock ();
                  if (!host) {
                      return false;
                  }
                  return host->complete_local_request (operation, reply);
              };


            dispatch = _local_dispatch_completion_lane.run_task ([&] {
                admit_local_application (local_application_dispatch_t{std::move (owner),
                                                                      std::move (record), parts,
                                                                      std::move (source_spot_id)},
                                         waiter);

                return zlink::submit_result_t::ok;
            });
        });
        const auto submitted = co_await dispatch;
        if (submitted != zlink::submit_result_t::ok) {
            _transport->unregister_local_operation (operation.id);
        }
        if (submitted == zlink::submit_result_t::ok)
            _transport->signal_activity ();
        co_return submitted;
    }
    catch (...) {
        _transport->unregister_local_operation (operation.id);
        throw;
    }
}

bool public_host_runtime_t::complete_local_request (const pending_operation_t &operation,
                                                    const std::vector<zlink::message_t> &parts)
{
    receive_record_t completion;
    completion.kind = record_kind_t::completion;
    completion.domain = ready_domain_t::infrastructure;
    completion.operation_id = operation.id;
    completion.source_node_rid = status ().routing_id ();
    return enqueue_completion (operation, std::move (completion), parts);
}

void public_host_runtime_t::complete_operation (const pending_operation_t &operation,
                                                operation_kind_t kind,
                                                foundation::operation_terminal_t terminal,
                                                zlink::message_t payload)
{
    try {
        receive_record_t record;
        record.kind = record_kind_t::completion;
        record.domain = ready_domain_t::infrastructure;
        record.operation_id = operation.id;
        record.operation_kind = kind;
        record.source_node_rid =
          zlink::routing_id_t::from (_options.mesh.descriptor.node_routing_id);
        switch (terminal) {
            case foundation::operation_terminal_t::completed:
                record.terminal_result = 0;
                break;
            case foundation::operation_terminal_t::timed_out:
                record.terminal_result = static_cast<int> (zlink::request_result_t::timed_out);
                break;
            case foundation::operation_terminal_t::route_unavailable:
                record.terminal_result = static_cast<int> (zlink::request_result_t::not_connected);
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
                parts = protocol::decode_application_parts (std::move (payload), capture_flow ());
            }
            catch (const protocol::service_wire_error_t &) {
                record.terminal_result = static_cast<int> (zlink::request_result_t::protocol_error);
            }
        } else if (!payload.is_empty ()) {
            try {
                const auto reply =
                  protocol::decode_reply_header (protocol::message_bytes (payload));
                record.terminal_result = static_cast<int> (reply.terminal_result);
                record.failure_errno = static_cast<int> (reply.failure_code);
            }
            catch (const protocol::service_wire_error_t &) {
                record.terminal_result = static_cast<int> (zlink::request_result_t::protocol_error);
                record.failure_errno = 0;
            }
        }
        if (terminal == foundation::operation_terminal_t::shutdown) {
            operation.completion->complete (detail::boundary_failure<operation_completion_t> (
              detail::boundary_error_t::shutdown, "MeshNode operation owner is closed"));
        } else {
            operation.completion->complete (result_t<operation_completion_t>::success (
              operation_completion_t{std::move (record), std::move (parts)}));
        }
    }
    catch (const std::exception &error) {
        operation.completion->complete (result_t<operation_completion_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
}

task_t<bool> actor_transfer_token_t::commit (std::uint64_t membership_epoch)
{
    if (_role == actor_transfer_role_t::target) {
        if (_host.expired () || _terminal || membership_epoch == 0)
            co_return false;
        _membership_epoch = membership_epoch;
        co_return true;
    }
    co_return co_await commit_membership ();
}

task_t<bool> actor_transfer_token_t::activate ()
{
    if (_role != actor_transfer_role_t::target || _membership_epoch == 0)
        co_return false;
    co_return co_await commit_membership ();
}

task_t<bool> actor_transfer_token_t::commit_membership ()
{
    const auto host = _host.lock ();
    if (!host || _terminal)
        co_return false;
    const auto [error, current] =
      co_await host->objects ().commit_membership_move_async (_membership);
    _terminal = true;
    if (error == stateful::stateful_error_t::none) {
        co_await host->_spot_actor_index_lane.run_task ([&] {
            const auto found = host->_actors.find (_membership.actor.key);
            if (found != host->_actors.end ())
                found->second.second = current;
        });
    }
    co_return error == stateful::stateful_error_t::none;
}

zlink::submit_result_t reply (const reply_token_t &token,
                              const std::vector<zlink::message_t> &parts)
{
    const auto host = token.host.lock ();
    return host && host->reply (token, parts) ? zlink::submit_result_t::ok
                                              : zlink::submit_result_t::terminated;
}

bool actor_join_reply (const reply_token_t &token,
                       result_t<actor_join_result_t> result,
                       const std::vector<zlink::message_t> &parts)
{
    actor_join_operation_result_t completion;
    if (result) {
        completion.join_result = result.value () == actor_join_result_t::accepted
                                   ? protocol::actor_join_result_t::accepted
                                   : protocol::actor_join_result_t::rejected;
    } else {
        const messaging::request_failure_mapper_t mapper;
        const auto failure =
          result.error () ? mapper.target_failure_reply (*result.error ())
                          : mapper.target_failure_reply (framework_error_kind_t::internal_failure);
        completion.terminal_result = failure->terminal_result;
        completion.failure_code = failure->failure_code;
    }
    if (token.local_actor_join)
        return token.local_actor_join (completion, parts);
    const auto host = token.host.lock ();
    if (!host || !token.request)
        return false;
    try {
        if (completion.terminal_result != 0)
            return host->transport ().reply_failure (*token.request, completion.terminal_result,
                                                     completion.failure_code);
        if (completion.join_result == protocol::actor_join_result_t::rejected)
            return host->transport ().reply_actor_join (
              *token.request, completion.join_result, std::nullopt, 0, 0, 0, 0,
              parts.empty ()
                ? std::nullopt
                : std::make_optional (protocol::application_payload_t::from_parts (parts)));
        return reply (token, parts) == zlink::submit_result_t::ok;
    }
    catch (const zlink::submit_error_t &) {
        return false;
    }
}

} // namespace zlink::framework::runtime::host
