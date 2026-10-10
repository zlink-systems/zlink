/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/execution/task_result.hpp"
#include "runtime/diagnostics/mesh_trace.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"

#include "runtime/stateful/public_host_runtime.hpp"
#include <zlink/framework/contracts/detail/handler_invocation.hpp>
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
#include "runtime/messaging/request_failure_mapper.hpp"

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

namespace
{
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
    std::shared_ptr<detail::deferred_barrier_t> gate;
    std::function<void ()> accepted;
    void finish ()
    {
        auto barrier = std::exchange (gate, {});
        auto terminal = std::exchange (accepted, {});
        try {
            if (terminal)
                terminal ();
        }
        catch (...) {
            if (barrier)
                (void) barrier->activate ([] {});
            throw;
        }
        if (barrier)
            (void) barrier->activate ([] {});
    }
    ~activation_terminal_t () noexcept
    {
        try {
            finish ();
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

class application_claim_release_state_t
{
  public:
    application_claim_release_state_t (std::size_t record_count,
                                       std::function<void ()> release_claim) :
        _remaining (record_count),
        _records (record_count),
        _release_claim (std::move (release_claim))
    {
    }

    void retain (std::size_t index) noexcept
    {
        _records[index].retained.store (true, std::memory_order_release);
    }

    bool retained (std::size_t index) const noexcept
    {
        return _records[index].retained.load (std::memory_order_acquire);
    }

    void release (std::size_t index)
    {
        if (!_records[index].released.exchange (true, std::memory_order_acq_rel))
            release_records (1);
    }

    void release_records (std::size_t count)
    {
        if (count == 0)
            return;
        const auto previous = _remaining.fetch_sub (count, std::memory_order_acq_rel);
        if (previous == count)
            _release_claim ();
    }

  private:
    struct record_state_t
    {
        std::atomic_bool retained{false};
        std::atomic_bool released{false};
    };

    std::atomic_size_t _remaining;
    std::vector<record_state_t> _records;
    std::function<void ()> _release_claim;
};

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
    const auto route_fence = co_await _host->resolve_spot_route_fence (
      target_node_rid, target_spot_id, target_spot_generation);
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
            const auto submitted = host->enqueue_local_spot_request (
              target, parts, operation, timeout, metadata, std::move (completion));
            co_return submitted;
        }
        operation.prepare_for_registration ();
        operation.id = _host->next_operation ();
        const auto accepted = co_await _host->transport ().request_to_spot (
          target_node_rid.to_bytes (), spot_id (), target,
          _host->encode_application (parts, metadata), timeout,
          [host, operation, completion = std::move (completion), direct_completion] (
            foundation::operation_terminal_t terminal, std::vector<std::uint8_t> payload) mutable {
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
                        protocol::decode_application_parts (
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

              try {
                  completion (terminal, std::move (decoded));
              }
              catch (...) {
              }
          },
          protocol::wire_operation_id_t{operation.id.high, operation.id.low}, std::nullopt);
        co_return submitted (accepted);
    }
    catch (...) {
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
          _host->admit_local_application (
            local_application_dispatch_t{std::move (owner), std::move (local), parts});
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
    co_await publish_tail_impl (parts, metadata, nullptr);
}

task_t<void> spot_handle_t::publish_tail (
  const std::vector<zlink::message_t> &parts,
  std::span<const std::uint8_t> metadata,
  std::function<void (const zlink::routing_id_t &, zlink::submit_result_t)> failure_observer)
{
    co_await publish_tail_impl (parts, metadata, &failure_observer);
}

task_t<void> spot_handle_t::publish_tail_impl (
  const std::vector<zlink::message_t> &parts,
  std::span<const std::uint8_t> metadata,
  const std::function<void (const zlink::routing_id_t &, zlink::submit_result_t)> *failure_observer)
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
            if (submitted != zlink::submit_result_t::ok && failure_observer != nullptr) {
                (*failure_observer) (zlink::routing_id_t::from (target.descriptor.node_routing_id),
                                     submitted);
            }
        }
        catch (...) {
            if (failure_observer != nullptr) {
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
                (*failure_observer) (zlink::routing_id_t::from (target.descriptor.node_routing_id),
                                     submitted);
            }
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
                                                        pending_operation_t &operation,
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
                                                  pending_operation_t &operation,
                                                  std::chrono::milliseconds timeout)
{
    if (!_host) {
        return zlink::submit_result_t::invalid_handle;
    }
    if (target_node_rid.to_bytes () != _host->status ().routing_id ().to_bytes ()) {
        return zlink::submit_result_t::not_connected;
    }
    return _host->begin_local_actor_join (_actor, target_spot_id, target_spot_generation, parts,
                                          operation, timeout);
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
  stateful::accepted_record_authority_resolver_t resolver)
{
    if (!resolver)
        throw std::invalid_argument ("stateful dispatch authority resolver must not be empty");
    _lifecycle_configuration_lane
      .run ([&] {
          if (_started || _stateful_dispatch)
              throw std::logic_error (
                "stateful dispatch must be configured once before host start");
          _stateful_dispatch = std::make_unique<stateful::raw_stateful_dispatch_t> (
            _objects, *_transport,
            [this, resolver = std::move (resolver)] (
              const stateful::accepted_record_authority_query_t &query) {
                auto accepted =
                  _relocation_session_terminal_lane
                    .run ([&] () -> std::optional<stateful::accepted_record_authority_t> {
                        for (const auto &[key, attempt] : _relocation_target_attempts) {
                            const auto &prepare = attempt.prepare;
                            if (prepare.source_node_routing_id != query.source_node_routing_id
                                || prepare.source_node_generation != query.source_node_generation
                                || prepare.target.target_node_routing_id
                                     != query.target_node_routing_id
                                || prepare.target.target_node_generation
                                     != query.target_node_generation
                                || std::find (attempt.targets.begin (), attempt.targets.end (),
                                              query.target)
                                     == attempt.targets.end ())
                                continue;
                            return stateful::accepted_record_authority_t{
                              {prepare.coordinator.owner_id, prepare.coordinator.lease_generation,
                               prepare.source_node_routing_id, prepare.source_node_generation},
                              prepare.target.target_owner_lease_generation};
                        }
                        return std::nullopt;
                    })
                    .get ();
                return accepted ? accepted : resolver (query);
            });
          /* flow-correlation §4: the ingest path gates flow capture on the host's
           * provider; the host outlives the dispatch it owns. */
          _stateful_dispatch->set_flow_capture_provider ([this] { return capture_flow (); });
      })
      .get ();
}

void public_host_runtime_t::forward_relocation_application (
  const stateful::object_ref_t &owner,
  const stateful::turn_record_t &record,
  const std::vector<std::uint8_t> &target_routing_id,
  std::uint64_t target_generation,
  std::uint64_t target_lease_generation,
  std::chrono::milliseconds window)
{
    if (!_stateful_dispatch)
        return;
    auto pending = std::make_shared<task_t<bool>> (_stateful_dispatch->forward_accepted (
      owner, record, target_routing_id, target_generation, target_lease_generation, window));
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
              throw std::logic_error (
                "late Session route reporter must be configured before start");
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
    const auto instance_admissions =
      _lifecycle_configuration_lane
        .run ([&] { return std::exchange (_instance_spot_activation_admissions, {}); })
        .get ();
    for (const auto &[spot_id, admission] : instance_admissions) {
        admission->complete (result_t<instance_activation_admission_t>::failure (
          framework_error_kind_t::shutting_down, "Instance Spot activation target stopped"));
    }
    if (maintenance_closing) {
        try {
            maintenance_closing ();
        }
        catch (...) {
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
    _local_dispatch_completion_lane.run ([&] { _local_application_dispatches.clear (); }).get ();
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
    const auto generation_stale =
      _spot_actor_index_lane
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
          if (found != _actors.end ()
              && found->second.second.object_generation == object_generation)
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
 * fence (the Closing StoreVersion). `after_delete` runs once the row is gone. */
std::function<task_t<bool> ()>
release_closing_authority (std::shared_ptr<zlink::framework::location_repository_t> store,
                           authority_key_t authority_key,
                           std::string closing_version,
                           std::function<result_t<bool> ()> before_delete,
                           std::function<void ()> after_delete)
{
    return [store = std::move (store), authority_key = std::move (authority_key),
            closing_version = std::move (closing_version),
            before_delete = std::move (before_delete), after_delete = std::move (after_delete)] () {
        auto completion = std::make_shared<task_completion_source_t<bool>> ();
        auto output = completion->task ();
        if (before_delete) {
            auto ready = before_delete ();
            if (!ready) {
                completion->complete (std::move (ready));
                return output;
            }
        }
        after_close_step (
          store->compare_exchange_authority (authority_key, closing_version, authority_delete_t{}),
          {}, [completion, after_delete] (result_t<authority_compare_exchange_result_t> deleted) {
              if (!deleted) {
                  completion->complete (close_store_failure (deleted));
                  return;
              }
              if (!std::holds_alternative<authority_deleted_t> (deleted.value ())) {
                  completion->complete (result_t<bool>::failure (
                    framework_error_kind_t::internal_failure, "Spot authority release failed"));
                  return;
              }
              if (after_delete)
                  after_delete ();
              completion->complete (result_t<bool>::success (true));
          });
        return output;
    };
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
                 release_closing_authority (store, authority_key, snapshot->store_version, {}, {}),
                 reincarnate_closing_authority (store, authority_key, snapshot->store_version,
                                                ready_payload),
                 [store, authority_key] (const authority_snapshot_t &next) {
                     return release_closing_authority (store, authority_key, next.store_version, {},
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
                                              {}, {}),
                   reincarnate_closing_authority (store, authority_key,
                                                  closing->snapshot.store_version, ready_payload),
                   [store, authority_key] (const authority_snapshot_t &next) {
                       return release_closing_authority (store, authority_key, next.store_version,
                                                         {}, {}) ();
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
    auto completion = std::make_shared<task_completion_source_t<spot_close_commit_t>> ();
    auto output = completion->task ();
    std::shared_ptr<zlink::framework::location_repository_t> store;
    std::function<std::optional<location_owner_token_t> ()> owner_resolver;
    _lifecycle_configuration_lane
      .run ([&] {
          store = _user_spot_store;
          owner_resolver = _session_route_owner_resolver;
      })
      .get ();
    if (!store) {
        complete_close_step (completion,
                             close_step_failure (framework_error_kind_t::not_configured,
                                                 "User Spot close requires a Location Store"));
        return output;
    }
    const auto authority_key = spot_authority_key (target.spot_id);
    auto self = shared_from_this ();
    after_close_step (
      store->read_authority (authority_key), {},
      [self, completion, store, owner_resolver, authority_key,
       target = std::move (target)] (result_t<authority_read_result_t> read) {
          const auto moving = [] {
              return close_step_failure (
                framework_error_kind_t::unavailable, "User Spot owner is moving",
                static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving));
          };
          if (!read) {
              complete_close_step (completion, {close_store_failure (read), {}});
              return;
          }
          const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
          if (!snapshot) {
              complete_close_step (completion, {});
              return;
          }
          if (snapshot->object_generation != target.object_generation) {
              complete_close_step (
                completion,
                close_step_failure (framework_error_kind_t::invalid_operation,
                                    "User Spot generation is stale",
                                    static_cast<std::uint32_t> (
                                      protocol::framework_error_code::spotGenerationStale)));
              return;
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
              || snapshot->allocation.target.node_lifecycle_generation
                   != target.target_node_generation
              || snapshot->allocation.target.mesh_name != self->_options.mesh.descriptor.mesh_name
              || snapshot->allocation.target.node_rid.value ()
                   != node_rid_t::from_string (local.routing_id ().to_string ()).value ()
              || snapshot->allocation.target.node_lifecycle_generation
                   != local.lifecycle_generation ()
              || (owner
                  && (snapshot->allocation.target.owner.owner_id != owner->owner_id
                      || snapshot->allocation.target.owner.lease_generation
                           != owner->lease_generation))) {
              complete_close_step (completion, moving ());
              return;
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
          const auto release_from = [self, store, authority_key, key = exact_ref.key] (
                                      std::string closing_version, stateful::object_ref_t ref) {
              return release_closing_authority (
                store, authority_key, std::move (closing_version),
                [self, ref] {
                    // The record that the Closing commit sealed is found by its
                    // exact ref, so a resumed Close retries a failed release.
                    const auto [error, closed] = self->_objects.close_spot (ref);
                    if (error != stateful::stateful_error_t::none
                        && error != stateful::stateful_error_t::not_found)
                        return result_t<bool>::failure (framework_error_kind_t::internal_failure,
                                                        "User Spot record release failed");
                    return result_t<bool>::success (true);
                },
                [self, key] {
                    self->_spot_actor_index_lane.run ([&] { self->_spots.erase (key); }).get ();
                });
          };

          if (snapshot->payload == closing_payload) {
              // Closing is committed for this owner and generation: a later
              // Close resumes the remaining steps without repeating the
              // completed ones.
              complete_close_step (completion, {result_t<bool>::success (true),
                                                release_from (snapshot->store_version, exact_ref)});
              return;
          }
          if (snapshot->payload != ready_payload) {
              complete_close_step (completion, moving ());
              return;
          }
          const auto local_object =
            self->_objects.find (stateful::object_kind_t::user_spot, exact_ref.key);
          if (!local_object || *local_object != exact_ref) {
              complete_close_step (completion, moving ());
              return;
          }
          const auto [close_error, eligible] = self->_objects.can_close_spot (exact_ref);
          if (close_error == stateful::stateful_error_t::generation_stale) {
              complete_close_step (
                completion,
                close_step_failure (framework_error_kind_t::invalid_operation,
                                    "User Spot generation is stale",
                                    static_cast<std::uint32_t> (
                                      protocol::framework_error_code::spotGenerationStale)));
              return;
          }
          if (close_error != stateful::stateful_error_t::none) {
              complete_close_step (completion, moving ());
              return;
          }
          // Actor membership keeps admission and authority (§7).
          if (!eligible) {
              complete_close_step (completion, {});
              return;
          }
          after_close_step (
            store->compare_exchange_authority (authority_key, snapshot->store_version,
                                               authority_put_t{closing_payload}),
            {},
            [self, completion, exact_ref, release_from,
             moving] (result_t<authority_compare_exchange_result_t> sealed) {
                const auto *closing =
                  sealed ? std::get_if<authority_stored_t> (&sealed.value ()) : nullptr;
                if (!closing) {
                    complete_close_step (
                      completion,
                      sealed ? moving () : spot_close_commit_t{close_store_failure (sealed), {}});
                    return;
                }
                complete_close_step (completion,
                                     {result_t<bool>::success (true),
                                      release_from (closing->snapshot.store_version, exact_ref)});
            });
      });
    return output;
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
    const auto existing_result =
      _relocation_session_terminal_lane
        .run ([&] () -> std::optional<
                       std::pair<bool, std::optional<protocol::session_relocation_sealed_t>>> {
            const auto cached = _session_seal_terminals.find (relocation_key);
            if (cached != _session_seal_terminals.end ()) {
                existing_terminal = true;
                if (cached->second.seal != seal)
                    return {{false, std::nullopt}};
                if (cached->second.consumed && !cached->second.ready)
                    return {{false, std::nullopt}};
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
    if (existing_terminal)
        return *existing_result;

    const auto session_id = zlink::routing_id_t::from (seal.session_routing_id).to_hex ();
    const auto admission = _sessions.seal_remote_route (
      session_id, seal.binding_generation, seal.actor.actor_id, seal.actor.object_generation);
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
    auto response = std::make_shared<
      std::function<void (foundation::operation_terminal_t, std::vector<std::uint8_t>)>> (
      [weak_host, expected, relocation_key, operation_deadline,
       relocations = std::move (relocations), capture_journal = std::move (capture_journal),
       completion = std::move (completion)] (foundation::operation_terminal_t terminal,
                                             std::vector<std::uint8_t> payload) mutable {
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
        _lifecycle_configuration_lane.run ([&] { owner_resolver = _session_route_owner_resolver; })
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
                  const auto parts = protocol::unpack_infrastructure_reply (packed);
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
                    prepared = materializer->prepare (recovery.activation, entry.snapshot);
                }
                catch (...) {
                    prepared = false;
                }
                if (!prepared)
                    continue;
                auto dispatched =
                  materializer
                    ->dispatch (
                      std::make_shared<const protocol::instance_activation_recovery_t> (recovery),
                      {}, &activation_terminal.gate)
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
                if (!stored)
                    continue;
                completed = stored->snapshot;
            }
            const auto completed_state = decode_instance_spot_authority_payload (completed.payload);
            if (!completed_state || !completed_state->activation_recovery
                || completed_state->activation_recovery->replay_cursor
                     != completed_state->activation_recovery->inbox_sequence)
                continue;
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
            if (!std::holds_alternative<authority_stored_t> (cleared))
                continue;
            relocations->remove (recovery_pointer.reference);
            activation_terminal.finish ();
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
          protocol::user_spot_create_reply_t reply{};
          std::optional<protocol::application_payload_t> application_reply;
          if (terminal == foundation::operation_terminal_t::completed) {
              try {
                  const auto parts = protocol::unpack_infrastructure_reply (packed);
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
                  const auto parts = protocol::unpack_infrastructure_reply (packed);
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
                  const auto parts = protocol::unpack_infrastructure_reply (packed);
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
                             const auto [found, _] =
                               _spots.insert_or_assign (object.key, std::move (object));
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
    _lifecycle_configuration_lane
      .run ([&] {
          resolver = _spot_route_fence_resolver;
          store = _user_spot_store;
      })
      .get ();
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
        co_return enqueue_local_actor_message (target, record_kind_t::actor_send, parts, nullptr,
                                               std::move (bound_session_source));
    }
    const auto peer = _transport->topology ().peer (target_routing_id.to_bytes ());
    if (!peer) {
        co_return zlink::submit_result_t::not_connected;
    }
    const auto node_generation = peer->descriptor.lifecycle_generation;
    const auto object =
      _objects.find (stateful::object_kind_t::actor, std::string (target.actor_id ().value ()));
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
        const auto accepted =
          enqueue_local_actor_message (target, record_kind_t::actor_request, parts, &operation,
                                       std::move (bound_session_source), timeout);
        co_return accepted;
    }
    operation.prepare_for_registration ();
    operation.id = next_operation ();
    const auto peer = _transport->topology ().peer (target_routing_id.to_bytes ());
    if (!peer) {
        co_return zlink::submit_result_t::not_connected;
    }
    const auto node_generation = peer->descriptor.lifecycle_generation;
    const auto object =
      _objects.find (stateful::object_kind_t::actor, std::string (target.actor_id ().value ()));
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
                         std::vector<std::uint8_t> payload) mutable {
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
    const auto target_bytes = target.to_bytes ();
    co_return co_await _transport->send_to_node_result (target_bytes, encode_application (parts));
}

task_t<zlink::submit_result_t>
public_host_runtime_t::send_to_node (const zlink::routing_id_t &target,
                                     std::vector<zlink::message_t> &&parts)
{
    const auto target_bytes = target.to_bytes ();
    co_return co_await _transport->send_to_node_result (target_bytes,
                                                        encode_application (std::move (parts)));
}

task_t<zlink::submit_result_t>
public_host_runtime_t::request_to_node (const zlink::routing_id_t &target,
                                        const std::vector<zlink::message_t> &parts,
                                        pending_operation_t &operation,
                                        std::chrono::milliseconds timeout)
{
    operation.prepare_for_registration ();
    operation.id = next_operation ();
    const auto host = shared_from_this ();
    const auto accepted = co_await _transport->request_to_node (
      target.to_bytes (), encode_application (parts), timeout,
      [host, operation] (foundation::operation_terminal_t terminal,
                         std::vector<std::uint8_t> payload) mutable {
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
                         std::vector<std::uint8_t> payload) mutable {
          host->complete_operation (operation, operation_kind_t::none, terminal,
                                    std::move (payload));
      });
    co_return submitted (accepted);
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
    // Retain only the terminal while cleanup runs, so shutdown cannot miss it.
    attempt = relocation_target_attempt_t{};
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
    auto terminals =
      _relocation_session_terminal_lane
        .run ([this] {
            _options.mesh.shutdown_admission_seal->store (true, std::memory_order_release);
            std::vector<task_t<void>> result;
            for (const auto &[key, attempt] : _relocation_target_attempts)
                if (attempt.terminal)
                    result.push_back (attempt.terminal->task ());
            return result;
        })
        .get ();
    discard_unverified_relocation_targets ();
    for (auto &terminal : terminals)
        co_await terminal;
}

void public_host_runtime_t::discard_relocation_target_attempts (
  std::vector<relocation_target_attempt_t> attempts) noexcept
{
    for (auto &attempt : attempts) {
        if (attempt.authority_fence && _aggregate_relocation_authority) {
            try {
                _aggregate_relocation_authority->abort (*attempt.authority_fence);
            }
            catch (...) {
            }
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
        const relocation_attempt_key_t key{attempt.prepare.relocation.high,
                                           attempt.prepare.relocation.low,
                                           attempt.prepare.target_attempt_generation};
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
              if (relocation_target_terminal (attempt)
                  || attempt.next_finalize_at == std::chrono::steady_clock::time_point{}
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
              }
              attempt.next_finalize_at = now + dispatch_limits::management_retry_interval;
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

        if (relocation_target_authority_committed_strict (attempt))
            return adopt_store_fences ();

        if (!_aggregate_relocation_authority)
            return false;
        if (!attempt.authority_fence) {
            const auto prepared = _aggregate_relocation_authority->prepare (
              attempt.sources, attempt.targets.front ().node_id, target_owner,
              attempt.restore_identity.reference, attempt.restore_identity.checksum_crc32c,
              attempt.restore_identity.inventory_digest,
              attempt.prepare.coordinator.expected_authority_store_version);
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
                if (!found->second.cutover_received
                    || found->second.next_finalize_at == std::chrono::steady_clock::time_point{}
                    || now < found->second.next_finalize_at)
                    return false;
                attempt = found->second;
                found->second.next_finalize_at = now + dispatch_limits::management_retry_interval;
            }
            return true;
        })
        .get ();
    if (!attempt_current)
        return false;
    if (relocation_target_terminal (attempt)) {
        start_relocation_session_route_submission (key);
        return true;
    }

    const auto settlement = commit_relocation_target_authority (attempt);
    if (settlement == relocation_target_settlement_t::discard) {
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

    const auto actor_indexes_current =
      _spot_actor_index_lane
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
                                     return std::shared_ptr<task_completion_source_t<void>>{};
                                 return found->second.terminal;
                             })
                             .get ();
    if (!finalized)
        return false;
    /* Metrics 25 §"zlink.relocation": target_resume is the target-local S2
     * (owner CAS confirmed) -> dispatch-open duration, emitted once on the
     * tick that completes the target terminal. */
    if (_relocation_target_metrics.target_resume_seconds
        && attempt.authority_committed_at != std::chrono::steady_clock::time_point{}) {
        const auto elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now ()
                                                            - attempt.authority_committed_at);
        _relocation_target_metrics.target_resume_seconds (elapsed.count ());
    }
    start_relocation_session_route_submission (key);
    finalized->complete (result_t<void>::success ());
    return true;
}

void public_host_runtime_t::reply_relocation_assembly_failure (
  const pending_relocation_assembly_t &pending, protocol::framework_error_code code)
{
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

task_t<bool> public_host_runtime_t::restore_relocation_assembly (
  std::shared_ptr<const pending_relocation_assembly_t> pending,
  std::shared_ptr<const relocation_assembly_staging_t> staging)
{
    stateful::stateful_error_t restored = stateful::stateful_error_t::conflict;
    std::optional<framework_exception_t> failure;
    try {
        std::optional<stateful::object_ref_t> actor_join_target_spot;
        if (staging->targets.size () == 1
            && staging->targets.front ().kind == stateful::object_kind_t::actor
            && _actor_join_authority_spot_resolver) {
            const auto spot = _actor_join_authority_spot_resolver (staging->targets.front ());
            if (spot) {
                actor_join_target_spot = stateful::object_ref_t{stateful::object_kind_t::user_spot,
                                                                std::get<1> (*spot),
                                                                std::get<2> (*spot),
                                                                0,
                                                                staging->targets.front ().mesh_name,
                                                                staging->targets.front ().node_id};
            }
        }
        restored =
          co_await (staging->targets.size () == 1
                      ? _objects.restore_relocation (
                          staging->frozen.front (), staging->targets.front (),
                          staging->restore_identity, {}, std::move (actor_join_target_spot))
                      : _objects.restore_relocation_aggregate (staging->frozen, staging->targets,
                                                               staging->restore_identity, {}));
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
    if (!failure
        && (restored == stateful::stateful_error_t::none
            || restored == stateful::stateful_error_t::already_exists))
        co_return true;
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

void public_host_runtime_t::activate_relocation_assembly (
  const relocation_attempt_key_t &key,
  const pending_relocation_assembly_t &pending,
  relocation_assembly_staging_t staging)
{
    relocation_target_attempt_t attempt;
    attempt.terminal = std::make_shared<task_completion_source_t<void>> ();
    attempt.prepare = pending.prepare;
    attempt.restore_identity = staging.restore_identity;
    attempt.sources = std::move (staging.sources);
    attempt.targets = std::move (staging.targets);
    attempt.wire_objects = std::move (staging.wire_objects);
    for (const auto &route : staging.session_routes) {
        attempt.session_routes.emplace_back ();
        attempt.session_routes.back ().route = route;
    }
    const auto inserted =
      _relocation_session_terminal_lane
        .run ([&] {
            if (_options.mesh.shutdown_admission_seal->load (std::memory_order_acquire))
                return false;
            return _relocation_target_attempts.try_emplace (key, std::move (attempt)).second;
        })
        .get ();
    if (!inserted) {
        std::vector<relocation_target_attempt_t> cleanup;
        cleanup.push_back (std::move (attempt));
        discard_relocation_target_attempts (std::move (cleanup));
        reply_relocation_assembly_failure (
          pending, static_cast<protocol::framework_error_code> (
                     messaging::request_failure_mapper_t{}.target_failure_code (
                       framework_error_kind_t::invalid_operation)));
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
                  found->second.next_finalize_at =
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
              aborted = take_relocation_target_discard (found->second);
          }
      })
      .get ();
    if (!aborted)
        return;
    std::vector<relocation_target_attempt_t> cleanup;
    cleanup.push_back (std::move (*aborted));
    discard_relocation_target_attempts (std::move (cleanup));
}

void public_host_runtime_t::complete_relocation_assembly (const relocation_attempt_key_t &key,
                                                          pending_relocation_assembly_t pending)
{
    // Verified identity and payload-integrity failures retain relocationDataLost.
    // Restore failures preserve their Framework kind and cause code instead.
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
    // failures; the helper tears down every queue it registered first. The
    // restore completes when its application materialization does; this
    // pump continues meanwhile and the attempt activates from that completion.
    auto held_pending = std::make_shared<pending_relocation_assembly_t> (std::move (pending));
    auto held_staging = std::make_shared<relocation_assembly_staging_t> (std::move (staging));
    auto restoring =
      std::make_shared<task_t<bool>> (restore_relocation_assembly (held_pending, held_staging));
    detail::observe_task_completion (
      *restoring, [self = shared_from_this (), restoring, key, held_pending, held_staging,
                   consumed = std::move (consumed)] (const result_t<bool> &restored) mutable {
          if (!restored || !restored.value ()) {
              self->rollback_actor_join_recoveries (consumed);
              return;
          }
          self->activate_relocation_assembly (key, *held_pending, std::move (*held_staging));
      });
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
               /* 28 §4.4: a pre-boundary relay record waits in the attempt's
                * boundary batch as its own accepted record; the verified
                * cutover alone stages the batch. A record after that
                * cutover can only be the source's retransmission of the
                * batch the target already took, so it is not staged again. */
               return _relocation_session_terminal_lane
                 .run ([&] {
                     const auto found = _relocation_target_attempts.find (attempt_key);
                     if (found == _relocation_target_attempts.end ())
                         return false;
                     if (!found->second.cutover_received)
                         found->second.boundary_batch.emplace_back (target, data);
                     return true;
                 })
                 .get ();
           },
           [] (const protocol::relocation_data_t &) {}});
    }
    catch (...) {
        return false;
    }
}

bool public_host_runtime_t::stage_relocation_record (const stateful::object_ref_t &target,
                                                     const protocol::relocation_data_t &data)
{
    const auto frozen_record = data.record;
    return _stateful_dispatch
           && _stateful_dispatch->stage_relocated (
                target, {0, protocol::encode_frozen_record (frozen_record)},
                [this, data,
                 frozen_record] (const std::optional<protocol::application_payload_t> &reply) {
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
}

task_t<void> public_host_runtime_t::dispatch_instance_spot_activation (
  protocol::instance_activation_recovery_t command,
  std::shared_ptr<const mesh::service_mailbox_record_t> mailbox_record,
  std::function<void (instance_spot_activation_result_t)> done)
{
    auto lifetime = shared_from_this ();
    const auto spot_id = command.activation.target.spot_id;
    std::shared_ptr<task_completion_source_t<instance_activation_admission_t>> admission;
    const auto previous_admission =
      _lifecycle_configuration_lane
        .run ([&] {
            if (_closing || !_started)
                throw framework_exception_t (framework_error_kind_t::shutting_down,
                                             "Instance Spot activation target stopped");
            if (command.activation.target.authority_owner_generation != 0
                && !_instance_spot_activation_admissions.contains (spot_id))
                return std::shared_ptr<task_completion_source_t<instance_activation_admission_t>>{};
            admission =
              std::make_shared<task_completion_source_t<instance_activation_admission_t>> ();
            return std::exchange (_instance_spot_activation_admissions[spot_id], admission);
        })
        .get ();
    auto running = std::make_shared<task_t<void>> (
      dispatch_instance_spot_activation_core (std::move (command), std::move (mailbox_record),
                                              std::move (done), admission, previous_admission));
    detail::observe_task_terminal (*running, [lifetime, running, admission,
                                              spot_id] (const result_t<void> &result) {
        if (result || !admission)
            return;
        lifetime->_lifecycle_configuration_lane
          .run ([&] {
              const auto found = lifetime->_instance_spot_activation_admissions.find (spot_id);
              if (found != lifetime->_instance_spot_activation_admissions.end ()
                  && found->second == admission)
                  lifetime->_instance_spot_activation_admissions.erase (found);
          })
          .get ();
        admission->complete (
          detail::result_access_t::failure<instance_activation_admission_t> (result.exception ()));
    });
    co_await *running;
}

task_t<void> public_host_runtime_t::dispatch_instance_spot_activation_core (
  protocol::instance_activation_recovery_t command,
  std::shared_ptr<const mesh::service_mailbox_record_t> mailbox_record,
  std::function<void (instance_spot_activation_result_t)> done,
  std::shared_ptr<task_completion_source_t<instance_activation_admission_t>> admission,
  std::shared_ptr<task_completion_source_t<instance_activation_admission_t>> previous_admission)
{
    auto lifetime = shared_from_this ();
    auto owned_command =
      std::make_shared<protocol::instance_activation_recovery_t> (std::move (command));
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
    const auto finish_admission = [lifetime, admission, spot_id = request.target.spot_id] (
                                    instance_activation_admission_t result) {
        if (!admission)
            return;
        lifetime->_lifecycle_configuration_lane
          .run ([&] {
              const auto found = lifetime->_instance_spot_activation_admissions.find (spot_id);
              if (found != lifetime->_instance_spot_activation_admissions.end ()
                  && found->second == admission)
                  lifetime->_instance_spot_activation_admissions.erase (found);
          })
          .get ();
        admission->complete (
          result_t<instance_activation_admission_t>::success (std::move (result)));
    };
    const auto reply_terminal = [lifetime, owned_command, transport = _transport, mailbox_record,
                                 done,
                                 finish_admission] (instance_spot_activation_result_t result) {
        if (result.terminal_result != 0)
            finish_admission (result);
        if (!done && result.terminal_result != 0) {
            detail::dispatch_error_reporter_t (lifetime->_options.mesh.dispatch).report_lazy ([&] {
                const auto error = messaging::request_failure_mapper_t{}.reply_header_exception (
                  result.terminal_result, result.failure_code, "Instance Spot activation");
                message_dispatch_error_event_t event{
                  .surface = dispatch_error_surface_t::instance_spot,
                  .message_kind = owned_command->activation.request
                                    ? dispatch_message_kind_t::request
                                    : dispatch_message_kind_t::send,
                  .reason = detail::dispatch_reason_from_error (&error),
                  .action = owned_command->activation.request ? dispatch_error_action_t::reply_error
                                                              : dispatch_error_action_t::drop,
                  .packet_name = owned_command->application_payload.packet_name,
                  .spot_id = owned_command->activation.target.spot_id,
                  .source_rid =
                    zlink::routing_id_t::from (owned_command->activation.source_node_routing_id)
                      .to_string (),
                  .exception = std::make_exception_ptr (error),
                  .mesh_name = lifetime->_options.mesh.descriptor.mesh_name};
                event.flow_id = owned_command->application_payload.flow_id;
                event.flow_origin = owned_command->application_payload.flow_origin;
                return event;
            });
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
    const auto instance_owner = instance_owner_resolver ? instance_owner_resolver () : std::nullopt;
    if (!store || !instance_relocations || !instance_materializer || !instance_owner) {
        reply_terminal ({105,
                         static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
                         std::nullopt});
        co_return;
    }
    if (request.target.authority_owner_generation == 0
        && request.target.deadline_unix_ms <= unix_milliseconds_now ()) {
        reply_terminal ({101, 0, std::nullopt});
        co_return;
    }
    const auto authority_key = spot_authority_key (request.target.spot_id);
    const auto reject_activation = [&] (std::optional<instance_spot_activation_result_t> result =
                                          std::nullopt) {
        if (request.target.authority_owner_generation == 0
            && request.target.deadline_unix_ms <= unix_milliseconds_now ()) {
            reply_terminal (
              {static_cast<std::uint32_t> (protocol::request_terminal_result::timedOut), 0,
               std::nullopt});
            return;
        }
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
    const auto join_existing = [&] (authority_read_result_t current) -> task_t<bool> {
        const auto *snapshot = std::get_if<authority_snapshot_t> (&current);
        if (request.target.authority_owner_generation != 0) {
            // Ready Instance direct의 object generation은 target 판정에 쓰지 않는다.
            // authority가 없으면 owner fence가 다른 것이다(Spot address messaging §9).
            if (!snapshot || snapshot->allocation.state != placement_allocation_state_t::active
                || snapshot->authority_owner_generation != request.target.authority_owner_generation
                || snapshot->owner.owner_id != request.target.owner_id
                || static_cast<std::uint64_t> (snapshot->owner.lease_generation)
                     != request.target.owner_lease_generation
                || snapshot->allocation.target.node_lifecycle_generation
                     != request.target.target_node_generation
                || snapshot->allocation.target.node_rid.value ()
                     != zlink::routing_id_t::from (request.target.target_node_routing_id)
                          .to_string ()) {
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
            reply_terminal (
              {107, static_cast<std::uint32_t> (protocol::framework_error_code::spotTypeMismatch),
               std::nullopt});
            co_return true;
        }
        if (snapshot->allocation.state == placement_allocation_state_t::active) {
            auto ready_state = decode_instance_spot_authority_payload (snapshot->payload);
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
            bool prepared = false;
            try {
                prepared = ready_state->state == instance_spot_authority_state_t::closing
                           || instance_materializer->prepare (request, *snapshot);
            }
            catch (...) {
                prepared = false;
            }
            if (!prepared) {
                reply_terminal (
                  {105,
                   static_cast<std::uint32_t> (protocol::framework_error_code::spotCreateFailed),
                   std::nullopt});
                co_return true;
            }
            auto running = std::make_shared<task_t<instance_spot_activation_result_t>> (
              instance_materializer->dispatch (owned_command, resume_missing, nullptr));
            finish_admission (*snapshot);
            detail::observe_task_terminal (
              *running, [running, reply_terminal] (
                          const result_t<instance_spot_activation_result_t> &result) {
                  if (result)
                      reply_terminal (result.value ());
                  else
                      reply_terminal (
                        {105,
                         static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
                         std::nullopt});
              });
            co_return true;
        }
        if (request.target.authority_owner_generation == 0
            && request.target.deadline_unix_ms <= unix_milliseconds_now ()) {
            reply_terminal ({101, 0, std::nullopt});
            co_return true;
        }
        // Local Creating work is joined through its target admission owner.
        // An authority without that owner cannot authorize a second local Reserve.
        reject_activation ();
        co_return true;
    };
    if (previous_admission) {
        const auto admitted = co_await previous_admission->task ();
        if (const auto *failure = std::get_if<instance_spot_activation_result_t> (&admitted)) {
            reject_activation (*failure);
            co_return;
        }
        if (!(co_await join_existing (
              authority_read_result_t{std::get<authority_snapshot_t> (admitted)})))
            reject_activation ();
        co_return;
    }
    auto current = co_await run_blocking_step<authority_read_result_t> (
      [store, authority_key] { return store->read_authority (authority_key); });
    if (request.target.authority_owner_generation == 0) {
        if (const auto *snapshot = std::get_if<authority_snapshot_t> (&current)) {
            const auto ready = decode_instance_spot_authority_payload (snapshot->payload);
            const auto local = status ();
            if (snapshot->allocation.target.node_rid.value () == local.routing_id ().to_string ()
                && snapshot->allocation.target.node_lifecycle_generation
                     == local.lifecycle_generation ()) {
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
                    const auto &prior = stored.activation;
                    if (prior.target.mesh_name != request.target.mesh_name
                        || prior.target.stable_type != request.target.stable_type
                        || prior.target.descriptor_version != request.target.descriptor_version
                        || prior.target.deadline_unix_ms != request.target.deadline_unix_ms
                        || prior.operation != request.operation
                        || prior.source_node_generation != request.source_node_generation
                        || prior.source_node_routing_id != request.source_node_routing_id
                        || prior.source_spot_id != request.source_spot_id
                        || prior.has_metadata != request.has_metadata
                        || stored.metadata != owned_command->metadata)
                        throw framework_exception_t (
                          framework_error_kind_t::protocol_error,
                          "Instance route does not match the stored activation");
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
            }
        }
    }
    if (co_await join_existing (current)) {
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
    const auto reserved = co_await store->reserve (reserve);
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
    bool prepared = false;
    try {
        prepared = instance_materializer->prepare (request, reservation->creating);
    }
    catch (...) {
        prepared = false;
    }
    if (!prepared) {
        (void) co_await store->abort ({reserve.key, reservation->fence});
        instance_relocations->remove (recovery_root.reference);
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
    const auto committed = co_await store->commit (
      {reserve.key, reservation->fence, encode_instance_spot_authority_payload (ready_state)}, {},
      reserve.operation_deadline);
    const auto *created = std::get_if<object_committed_t> (&committed);
    const auto *already = std::get_if<object_already_committed_t> (&committed);
    if (already) {
        if (!(co_await join_existing (authority_read_result_t{already->ready})))
            reply_terminal (
              {105, static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
               std::nullopt});
        co_return;
    }
    if (!created) {
        reply_terminal ({107,
                         static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving),
                         std::nullopt});
        co_return;
    }
    const auto &ready_snapshot = created->ready;
    auto activation_terminal = std::make_shared<activation_terminal_t> ();
    auto dispatched =
      instance_materializer->dispatch (owned_command, resume_missing, &activation_terminal->gate);
    finish_admission (ready_snapshot);
    auto result = co_await dispatched;
    activation_terminal->accepted = std::move (result.accepted_turn_terminal);
    result.accepted_turn_terminal = [activation_terminal] { activation_terminal->finish (); };
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
                result.terminal_result = 105;
                result.failure_code =
                  static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed);
                result.application_reply.reset ();
            }
        }
    }
    catch (const framework_exception_t &error) {
        const auto failure = messaging::request_failure_mapper_t{}.target_failure_reply (error);
        result.terminal_result = failure ? failure->terminal_result : 105;
        result.failure_code =
          failure ? failure->failure_code
                  : static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed);
        result.application_reply.reset ();
    }
    catch (...) {
        result.terminal_result = 105;
        result.failure_code =
          static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed);
        result.application_reply.reset ();
    }
    reply_terminal (std::move (result));
    co_return;
}

task_t<std::size_t> public_host_runtime_t::dispatch_user_spot_operations ()
{
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
            messaging::request_failure_mapper_t{}.target_failure_code (
              framework_error_kind_t::data_lost,
              static_cast<std::uint32_t> (protocol::framework_error_code::relocationDataLost))});
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
                    if (admission == bound_session_bind_admission_t::ready
                        && bound_session_operations.bind) {
                        operation_result = bound_session_operations.bind (
                          bind, zlink::routing_id_t::from (mailbox_record.source_routing_id),
                          mailbox_record.source_node_generation,
                          [this, &mailbox_record, &terminal_replied] {
                              terminal_replied =
                                _transport->reply_bound_session_bind (mailbox_record, 0u, 0u);
                              return terminal_replied;
                          });
                    }
                    const auto replied =
                      terminal_replied
                      || _transport->reply_bound_session_bind (
                        mailbox_record,
                        operation_result.error == stateful::stateful_error_t::none ? 0u
                        : admission == bound_session_bind_admission_t::stale_route
                          ? static_cast<std::uint32_t> (protocol::request_terminal_result::conflict)
                        : admission == bound_session_bind_admission_t::actor_not_ready
                          ? static_cast<std::uint32_t> (protocol::request_terminal_result::busy)
                          : static_cast<std::uint32_t> (
                              protocol::request_terminal_result::notFound),
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
                    const auto prepare_state =
                      _relocation_session_terminal_lane
                        .run ([&] {
                            const auto found = _relocation_target_attempts.find (key);
                            if (found == _relocation_target_attempts.end ())
                                return 0;
                            if (found->second.prepare != *prepare)
                                return 2;
                            return 1;
                        })
                        .get ();
                    if (prepare_state == 2)
                        continue;
                    const auto duplicate = prepare_state == 1;
                    if (duplicate) {
                        (void) _transport->reply_relocation_ready (
                          mailbox_record, protocol::relocation_ready_t{
                                            prepare->relocation, prepare->target_attempt_generation,
                                            prepare->coordinator, prepare->target, prepare->object,
                                            protocol::relocation_role_t::target});
                        continue;
                    }

                    if (prepare->payload_total_length == 0
                        || prepare->payload_total_length > protocol::relocationLogicalBytes
                        || prepare->payload_chunk_count == 0
                        || prepare->payload_chunk_count > protocol::relocationChunkCount)
                        continue;
                    const auto accepted =
                      _relocation_session_terminal_lane
                        .run ([&] {
                            const auto found = _relocation_assemblies.find (key);
                            if (found != _relocation_assemblies.end ()) {
                                if (found->second.prepare == *prepare)
                                    found->second.expires_at = std::chrono::steady_clock::now ()
                                                               + relocation_assembly_retention;
                                return true;
                            }
                            const auto current_state = status ().state;
                            if (current_state == node_status_t::state_t::draining
                                || current_state == node_status_t::state_t::stopped
                                || current_state == node_status_t::state_t::error)
                                return false;
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
                            return true;
                        })
                        .get ();
                    if (!accepted) {
                        (void) _transport->reply_relocation_failed (
                          mailbox_record,
                          protocol::relocation_failed_t{
                            prepare->relocation, prepare->target_attempt_generation,
                            prepare->coordinator, prepare->target, prepare->object,
                            protocol::relocation_role_t::target,
                            messaging::request_failure_mapper_t{}.target_failure_code (
                              framework_error_kind_t::shutting_down)});
                    }
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
                            messaging::request_failure_mapper_t{}.target_failure_code (
                              framework_error_kind_t::data_lost,
                              static_cast<std::uint32_t> (
                                protocol::framework_error_code::relocationDataLost))});
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
                    std::optional<
                      std::vector<std::pair<stateful::object_ref_t, protocol::relocation_data_t>>>
                      verified;
                    std::optional<relocation_target_attempt_t> mismatched;
                    _relocation_session_terminal_lane
                      .run ([&] {
                          const auto found = _relocation_target_attempts.find (key);
                          if (found == _relocation_target_attempts.end ()
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
                                    protocol::encode_relocation_control (record->second));
                              matches = accumulator.value () == cutover->boundary_checksum_crc32c;
                          }
                          if (matches) {
                              found->second.cutover_received = true;
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
                        for (const auto &[target, data] : *verified)
                            staged = staged && stage_relocation_record (target, data);
                        _relocation_session_terminal_lane
                          .run ([&] {
                              const auto found = _relocation_target_attempts.find (key);
                              if (found == _relocation_target_attempts.end ())
                                  return;
                              if (staged) {
                                  found->second.next_finalize_at =
                                    std::chrono::steady_clock::now ();
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
                    const auto route_matches_seal =
                      _relocation_session_terminal_lane
                        .run ([&] {
                            const auto sealed = _session_seal_terminals.find (relocation_key);
                            if (sealed == _session_seal_terminals.end ()
                                || sealed->second.consumed) {
                                late_session_route_update = true;
                                return true;
                            }
                            if (!sealed->second.ready
                                || sealed->second.seal.relocation != route.relocation
                                || sealed->second.seal.coordinator != route.coordinator
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
                            sealed_authority = sealed->second.seal.actor.authority_owner_generation;
                            return !((route.route.action
                                        == protocol::session_relocation_route_action_t::commit
                                      && route.route.previous_authority_owner_generation
                                           != sealed_authority)
                                     || (route.route.action
                                           == protocol::session_relocation_route_action_t::abort
                                         && route.route.current_authority_owner_generation
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
                          route.route.target_owner_lease_generation, std::move (commit_projection));
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
                    std::optional<std::vector<std::uint8_t>> metadata;
                    if (request.has_metadata)
                        metadata = mailbox_record.parts[1];
                    auto application = protocol::decode_application_payload (
                      mailbox_record.parts[request.has_metadata ? 2 : 1], capture_flow ());
                    auto reply_record =
                      std::make_shared<const mesh::service_mailbox_record_t> (mailbox_record);
                    auto running =
                      std::make_shared<task_t<void>> (dispatch_instance_spot_activation (
                        {request, std::move (metadata), std::move (application)}, reply_record));
                    detail::observe_task_terminal (
                      *running, [self = shared_from_this (), running,
                                 reply_record] (const result_t<void> &result) {
                          if (!result) {
                              const auto failure =
                                (result.error ()
                                   ? messaging::request_failure_mapper_t{}.target_failure_reply (
                                       *result.error ())
                                   : std::optional<messaging::request_wire_failure_t>{})
                                  .value_or (messaging::request_wire_failure_t{
                                    105, static_cast<std::uint32_t> (
                                           protocol::framework_error_code::requestFailed)});
                              (void) self->_transport->reply_instance_spot_activation (
                                *reply_record, failure.terminal_result, failure.failure_code);
                          }
                      });
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
                    const auto creation_payload = runtime::decode_inline_creation_content (
                      pending->request_content_reference, pending->request_sha256,
                      pending->request_encoded_size);
                    if (!creation_payload) {
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
                          ->abort ({{placement_object_kind_t::user_spot, global_id}, fence}, {},
                                   std::chrono::system_clock::time_point (
                                     std::chrono::milliseconds (request.deadline_unix_ms)))
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
                                     exact_ref, request.stable_type, fence.target)},
                                  {},
                                  std::chrono::system_clock::time_point (
                                    std::chrono::milliseconds (request.deadline_unix_ms)))
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
                if (mailbox_record.reply_token && mailbox_record.correlation)
                    (void) _transport->reply_failure (
                      mailbox_record, 104,
                      static_cast<std::uint32_t> (
                        protocol::framework_error_code::requestProtocolError));
            }
            catch (const std::exception &) {
                const auto failure = detail::current_exception_result<void> ();
                const auto *error = failure.error ();
                const auto terminal =
                  error && error->kind () == framework_error_kind_t::deadline_exceeded
                    ? std::pair{protocol::request_terminal_result::timedOut,
                                protocol::framework_error_code::none}
                    : std::pair{protocol::request_terminal_result::internalError,
                                protocol::framework_error_code::requestFailed};
                if (mailbox_record.reply_token && mailbox_record.correlation)
                    (void) _transport->reply_failure (mailbox_record,
                                                      static_cast<std::uint32_t> (terminal.first),
                                                      static_cast<std::uint32_t> (terminal.second));
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
  const mesh::service_mailbox_record_t &mailbox_record)
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
  const std::function<void (
    const ready_record_t &, const receive_record_t &, std::vector<zlink::message_t>)> &dispatch,
  bool accept_application_receive,
  const std::function<bool ()> &next_application_receive)
{
    if (!dispatch) {
        throw std::invalid_argument ("framework public host dispatch callback is required");
    }
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    (void) co_await _relocation_wire->retry_terminal_relays (now);
    (void) _relocation_wire->reap_terminal_tombstones (now);
    (void) _transport->tick_liveness (now);
    std::size_t count = 0;
    (void) _transport->expire_requests (foundation::operation_registry_t::clock_t::now ());
    count += co_await dispatch_user_spot_operations ();

    std::vector<stateful::object_inventory_t> stateful_owners;
    if (accept_application_receive && _stateful_dispatch) {
        const auto local_node_id =
          zlink::routing_id_t::from (_transport->topology ().local_descriptor ().node_routing_id)
            .to_string ();
        stateful_owners = _objects.inventory ();
        std::erase_if (stateful_owners, [&] (const auto &item) {
            return item.owner.node_id != local_node_id
                   || (item.state != stateful::object_state_t::ready
                       && item.state != stateful::object_state_t::moving
                       && item.state != stateful::object_state_t::recovering);
        });
    }
    auto next_stateful_owner = stateful_owners.begin ();
    receive_batch_budget_t budget;
    while (budget.can_receive ()) {
        const auto pumped = co_await _transport->pump_one (
          mesh::service_liveness_registry_t::clock_t::now (), accept_application_receive);
        budget.account (_transport->last_pump_bytes ());
        trace_mesh_host ("pump", std::string ("result=") + pump_result_name (pumped) + " pending="
                                   + std::to_string (_transport->mailbox ().pending_messages (
                                     mesh::service_mailbox_domain_t::application)));
        if (pumped != mesh::raw_mesh_pump_result_t::no_data)
            ++count;
        bool application_dispatch_started = pumped == mesh::raw_mesh_pump_result_t::application
                                            && _transport->mailbox ().has_application_dispatch ();

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
                      if (pending->record.kind == record_kind_t::spot_request
                          || pending->record.kind == record_kind_t::actor_request
                          || pending->record.kind == record_kind_t::spot_control)
                          skip = !_transport->operation_pending (pending->record.operation_id);
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
            while (!application_dispatch_started && next_stateful_owner != stateful_owners.end ()) {
                const auto &item = *next_stateful_owner++;
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
                    record.source_node_rid =
                      zlink::routing_id_t::from (frozen.source.node_routing_id);
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
                    record.complete_stateful_dispatch = [weak = weak_from_this (),
                                                         delivery = *delivery, completed] {
                        if (!completed->exchange (true, std::memory_order_acq_rel)) {
                            if (const auto host = weak.lock ()) {
                                auto pending =
                                  std::make_shared<task_t<stateful::stateful_error_t>> (
                                    host->_stateful_dispatch->complete_async (delivery,
                                                                              std::nullopt));
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
                                  auto pending =
                                    std::make_shared<task_t<stateful::stateful_error_t>> (
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
                    dispatch (owner, record,
                              protocol::decode_application_parts (delivery->payload));
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

        while (accept_application_receive && !application_dispatch_started
               && !_transport->mailbox ().has_application_dispatch ()) {
            auto claim = _transport->mailbox ().try_claim (
              mesh::service_mailbox_domain_t::application, 1, dispatch_limits::receive_batch_bytes);
            if (!claim)
                break;
            const auto dispatched = dispatch_application_claim (std::move (*claim), dispatch);
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

std::size_t public_host_runtime_t::dispatch_application_claim (
  mesh::service_mailbox_claim_t claim,
  const std::function<void (
    const ready_record_t &, const receive_record_t &, std::vector<zlink::message_t>)> &dispatch)
{
    std::size_t count = 0;
    trace_mesh_host ("mailbox-claim",
                     std::string ("records=") + std::to_string (claim.records.size ()));
    auto claim_holder = std::make_shared<mesh::service_mailbox_claim_t> (std::move (claim));
    auto release_state = std::make_shared<application_claim_release_state_t> (
      claim_holder->records.size (), [weak = weak_from_this (), claim_holder] {
          if (const auto host = weak.lock ())
              (void) host->_transport->mailbox ().release (*claim_holder);
      });
    for (std::size_t index = 0; index < claim_holder->records.size (); ++index) {
        auto &mailbox_record = claim_holder->records[index];
        const auto retain_mailbox_reservation = [release_state, index] {
            release_state->retain (index);
        };
        const auto release_mailbox_reservation = [release_state, index] {
            release_state->release (index);
        };
        try {
            if (mailbox_record.application) {
                auto application = std::move (*mailbox_record.application);
                application.record.before_application_handler =
                  mailbox_record.before_application_handler;
                application.record.release_mailbox_reservation = release_mailbox_reservation;
                application.record.retain_mailbox_reservation = retain_mailbox_reservation;
                application.record.transferred_owner_byte_cost = claim_holder->record_bytes[index];
                dispatch (application.owner, application.record, std::move (application.parts));
                ++count;
                if (!release_state->retained (index))
                    release_mailbox_reservation ();
                continue;
            }
            const auto wire = protocol::decode_header (mailbox_record.parts.front ());
            if (wire.kind == protocol::command::boundSessionSend) {
                // The outbound stream delivery is a transport handoff, not an application handler.
                mailbox_record.before_application_handler = {};
                (void) dispatch_bound_session_send (mailbox_record);
                ++count;
                if (!release_state->retained (index))
                    release_mailbox_reservation ();
                continue;
            }
            const auto kind = record_kind (wire.kind);
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
                record.reply_token = {
                  weak_from_this (),
                  std::shared_ptr<mesh::service_mailbox_record_t> (claim_holder, &mailbox_record)};
            }
            if (kind == record_kind_t::channel_send || kind == record_kind_t::channel_request) {
                owner.owner_kind = owner_kind_t::channel;
                owner.channel_name =
                  kind == record_kind_t::channel_send
                    ? protocol::decode_channel_send_header (mailbox_record.parts.front ())
                    : protocol::decode_channel_request_header (mailbox_record.parts.front ())
                        .channel_name;
                record.channel_name = owner.channel_name;
            } else if (kind == record_kind_t::spot_send || kind == record_kind_t::spot_request) {
                owner.owner_kind = owner_kind_t::spot;
                const auto spot =
                  protocol::decode_spot_message_header (mailbox_record.parts.front (), wire.kind);
                owner.spot_id = spot.target.spot_id;
                record.spot_route = spot.target;
            } else if (kind == record_kind_t::actor_send || kind == record_kind_t::actor_request) {
                owner.owner_kind = owner_kind_t::actor;
                const auto actor =
                  protocol::decode_actor_message_header (mailbox_record.parts.front (), wire.kind);
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
                                              const auto found =
                                                _actors.find (actor.target.actor_id);
                                              if (found != _actors.end ()) {
                                                  actor_type = found->second.first;
                                              }
                                              return actor_type;
                                          })
                                          .get ();
                owner.actor = ::zlink::framework::detail::actor_ref_access_t::make (
                  node_rid_t::from_string (status ().routing_id ().to_string ()),
                  std::move (actor_type), actor.target.actor_id, actor.target.object_generation);
            } else {
                owner.owner_kind = owner_kind_t::node;
            }
            const auto payload =
              protocol::decode_application_payload (mailbox_record.parts[1], capture_flow ());
            trace_mesh_host (
              "dispatch",
              std::string ("kind=") + std::to_string (static_cast<int> (kind)) + " source="
                + (mailbox_record.source_routing_id.empty () ? std::string ("-")
                                                             : record.source_node_rid.to_string ())
                + " parts=" + std::to_string (mailbox_record.parts.size ()));
            record.release_mailbox_reservation = release_mailbox_reservation;
            record.retain_mailbox_reservation = retain_mailbox_reservation;
            record.transferred_owner_byte_cost = claim_holder->record_bytes[index];
            dispatch (owner, record, protocol::decode_application_parts (payload));
            ++count;
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
            release_state->release_records (claim_holder->records.size () - index - 1);
            throw;
        }
        if (!release_state->retained (index))
            release_mailbox_reservation ();
    }
    return count;
}

bool public_host_runtime_t::dispatch_application_owner (
  const std::string &owner,
  const std::function<void (
    const ready_record_t &, const receive_record_t &, std::vector<zlink::message_t>)> &dispatch,
  const std::function<void ()> &started,
  const std::function<void ()> &rejected)
{
    auto claim = _transport->mailbox ().try_claim_owner (
      mesh::service_mailbox_domain_t::application, owner, dispatch_limits::receive_batch_messages,
      dispatch_limits::receive_batch_bytes);
    if (!claim)
        return false;
    const auto claimed = claim->records.size ();
    for (std::size_t index = 0; index < claimed; ++index)
        started ();
    std::size_t handed_off = 0;
    try {
        dispatch_application_claim (std::move (*claim), [&] (const ready_record_t &ready,
                                                             const receive_record_t &record,
                                                             std::vector<zlink::message_t> parts) {
            dispatch (ready, record, std::move (parts));
            ++handed_off;
        });
    }
    catch (...) {
        while (handed_off < claimed) {
            rejected ();
            ++handed_off;
        }
        throw;
    }
    while (handed_off < claimed) {
        rejected ();
        ++handed_off;
    }
    return true;
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
        return false;
    }
}

task_t<std::pair<bool, std::optional<std::chrono::steady_clock::time_point>>>
public_host_runtime_t::next_dispatch_activity_async ()
{
    const auto local_pending = co_await _local_dispatch_completion_lane.run_task (
      [this] { return !_local_application_dispatches.empty (); });
    auto next = _relocation_wire->next_activity ();
    co_return std::make_pair (
      local_pending, co_await _relocation_session_terminal_lane.run_task ([this, next] () mutable {
          const auto include = [&] (std::chrono::steady_clock::time_point deadline) {
              if (!next || deadline < *next)
                  next = deadline;
          };
          for (const auto &[key, assembly] : _relocation_assemblies)
              include (assembly.expires_at);
          for (const auto &[key, attempt] : _relocation_target_attempts) {
              if (!relocation_target_terminal (attempt)
                  && attempt.next_finalize_at != std::chrono::steady_clock::time_point{})
                  include (attempt.next_finalize_at);
          }
          for (const auto &[key, seal] : _session_seal_terminals) {
              if (!seal.consumed)
                  include (seal.expires_at);
          }
          return next;
      }));
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

void public_host_runtime_t::register_local_completion (pending_operation_t &operation,
                                                       std::chrono::milliseconds timeout,
                                                       spot_request_completion_t completion,
                                                       std::function<void ()> incomplete,
                                                       mesh_request_surface_t request_surface)
{
    try {
        operation.prepare_for_registration ();
        operation.local_result = std::make_shared<operation_completion_t> ();
        const auto source = operation.completion;
        const auto result = operation.local_result;
        const auto registered = _transport->register_local_operation (
          foundation::operation_registry_t::clock_t::now () + timeout,
          [source, result, completion = std::move (completion), incomplete] (
            foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) mutable {
              std::string terminal_message;
              if (terminal != foundation::operation_terminal_t::completed)
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
                  source->complete (
                    result_t<operation_completion_t>::success (std::move (*result)));
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
          std::nullopt, request_surface);
        if (!registered)
            throw framework_exception_t (framework_error_kind_t::shutting_down,
                                         "Operation completion registry is closed");
        operation.id = *registered;
    }
    catch (...) {
        if (incomplete)
            incomplete ();
        throw;
    }
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

zlink::submit_result_t
public_host_runtime_t::begin_local_actor_join (const actor_ref_t &actor,
                                               const std::string &target_spot_id,
                                               std::uint64_t target_spot_generation,
                                               const std::vector<zlink::message_t> &parts,
                                               pending_operation_t &operation,
                                               std::chrono::milliseconds timeout)
{
    return _lifecycle_configuration_lane
      .run ([&] {
          if (!_started || _closing)
              return zlink::submit_result_t::terminated;
          const auto current = resolve_actor (actor);
          const auto target = resolve_spot (target_spot_id);
          //  Spec 32-framework-error-model:129-136 — typed Rejected is reserved for
          //  the application callback decision. A Framework prerequisite failure
          //  carries a classified wire terminal on the completion instead of a
          //  synthesized rejection, and the consumer maps it to the public kind.
          auto fail = [&] (std::uint32_t terminal_result, std::uint32_t failure_errno) {
              register_local_completion (operation, timeout);
              receive_record_t completion;
              completion.kind = record_kind_t::completion;
              completion.domain = ready_domain_t::infrastructure;
              completion.operation_id = operation.id;
              completion.operation_kind = operation_kind_t::actor_join;
              completion.source_node_rid = status ().routing_id ();
              completion.terminal_result = static_cast<int> (terminal_result);
              completion.failure_errno = static_cast<int> (failure_errno);
              (void) enqueue_completion (operation, std::move (completion), {});
              return zlink::submit_result_t::ok;
          };
          if (!current || !target) {
              return fail (102, 0); // notFound: the join source or target doesn't exist.
          }
          if (target->object_generation != target_spot_generation) {
              return fail (107, 33); // spotGenerationStale -> InvalidOperation.
          }
          auto [error, membership] = _objects.begin_membership_move (*current, *target);
          if (error != stateful::stateful_error_t::none) {
              const auto classified = stateful_failure_pair (error);
              return fail (classified.first, classified.second);
          }

          auto waiter = _options.mesh.application_jobs
                          ? std::make_shared<application_job_queue_t::waiter_t> ()
                          : nullptr;
          register_local_completion (operation, timeout, {},
                                     [host = shared_from_this (), membership,
                                      cleanup = local_request_wait_cleanup (waiter)] {
                                         if (cleanup)
                                             cleanup ();
                                         (void) host->_objects.abort_membership_move (membership);
                                     });

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
          record.reply_token.local_actor_join =
            [weak, operation, actor_type, membership] (actor_join_result_t result,
                                                       const std::vector<zlink::message_t> &reply) {
                const auto host = weak.lock ();
                return host
                       && host->complete_local_actor_join (operation, actor_type, membership,
                                                           result, reply);
            };
          _local_dispatch_completion_lane
            .run ([&] {
                try {
                    admit_local_application (
                      local_application_dispatch_t{std::move (owner), std::move (record), parts},
                      waiter);
                }
                catch (...) {
                    _transport->unregister_local_operation (operation.id);
                    (void) _objects.abort_membership_move (membership);
                    throw;
                }
            })
            .get ();
          _transport->signal_activity ();
          return zlink::submit_result_t::ok;
      })
      .get ();
}

bool public_host_runtime_t::complete_local_actor_join (pending_operation_t operation,
                                                       std::string actor_type,
                                                       stateful::membership_token_t membership,
                                                       actor_join_result_t result,
                                                       const std::vector<zlink::message_t> &parts)
{
    return _transport->complete_local_operation (
      operation.id,
      [this, operation, actor_type = std::move (actor_type), membership, result, parts] () mutable {
          receive_record_t completion;
          completion.kind = record_kind_t::completion;
          completion.domain = ready_domain_t::infrastructure;
          completion.operation_id = operation.id;
          completion.operation_kind = operation_kind_t::actor_join;
          completion.source_node_rid =
            zlink::routing_id_t::from (_options.mesh.descriptor.node_routing_id);

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
                  completion.join_completion =
                    actor_join_completion_t{join_admission_t::accepted, actor};
              }
          } else {
              (void) _objects.abort_membership_move (membership);
              completion.join_completion = actor_join_completion_t{
                join_admission_t::rejected, framework_actor_ref (membership.actor, actor_type)};
          }

          *operation.local_result =
            operation_completion_t{std::move (completion), std::move (parts)};
      });
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

void public_host_runtime_t::admit_local_application (
  local_application_dispatch_t dispatch,
  const std::shared_ptr<application_job_queue_t::waiter_t> &waiter)
{
    if (!_options.mesh.application_jobs) {
        _local_application_dispatches.push_back (std::move (dispatch));
        return;
    }
    auto pending = waiter ? waiter : std::make_shared<application_job_queue_t::waiter_t> ();
    std::weak_ptr<public_host_runtime_t> weak = shared_from_this ();
    *pending = _options.mesh.application_jobs->wait_for_supply (
      [weak, waiter = pending, dispatch = std::move (dispatch)] (
        std::optional<application_job_queue_t::permit_t> reserved) mutable {
          auto waiting = std::move (waiter);
          if (!reserved)
              return;
          auto host = weak.lock ();
          if (!host)
              return;
          auto permit = std::make_shared<application_job_queue_t::permit_t> (std::move (*reserved));
          auto publish = [weak, waiting, permit, dispatch = std::move (dispatch)] () mutable {
              *waiting = {};
              auto host = weak.lock ();
              if (!host || !host->_transport->started ())
                  return;
              if (dispatch.record.operation_id != call_id_t{}
                  && !host->_transport->operation_pending (dispatch.record.operation_id))
                  return;
              permit->mark_queued ();
              dispatch.record.before_application_handler = [permit] {
                  permit->release_for_handler_entry ();
              };
              host->_local_application_dispatches.push_back (std::move (dispatch));
              host->_transport->signal_activity ();
          };
          if (host->_local_dispatch_completion_lane.is_on_lane ())
              publish ();
          else
              host->_local_dispatch_completion_lane.try_post (std::move (publish));
      },
      application_job_queue_t::origin_t::local);
}

zlink::submit_result_t public_host_runtime_t::enqueue_local_actor_message (
  const actor_ref_t &target,
  record_kind_t kind,
  const std::vector<zlink::message_t> &parts,
  pending_operation_t *operation,
  std::optional<protocol::actor_message_header_t::bound_session_source_t> bound_session_source,
  std::chrono::milliseconds timeout)
{
    if (kind != record_kind_t::actor_send && kind != record_kind_t::actor_request) {
        return zlink::submit_result_t::invalid_argument;
    }
    const auto current =
      _objects.find (stateful::object_kind_t::actor, std::string (target.actor_id ().value ()));
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
    const auto submitted =
      _lifecycle_configuration_lane
        .run ([&] {
            if (!_started || _closing) {
                return zlink::submit_result_t::terminated;
            }
            return _local_dispatch_completion_lane
              .run ([&] {
                  auto waiter = _options.mesh.application_jobs
                                  ? std::make_shared<application_job_queue_t::waiter_t> ()
                                  : nullptr;
                  if (operation) {
                      register_local_completion (*operation, timeout, {},
                                                 local_request_wait_cleanup (waiter),
                                                 mesh_request_surface_t::actor);
                      record.operation_id = operation->id;
                      std::weak_ptr<public_host_runtime_t> weak = shared_from_this ();
                      record.reply_token.host = weak;
                      record.reply_token.local_reply =
                        [weak,
                         operation = *operation] (const std::vector<zlink::message_t> &reply) {
                            const auto host = weak.lock ();
                            return host && host->complete_local_request (operation, reply);
                        };
                  }
                  try {
                      admit_local_application (
                        local_application_dispatch_t{std::move (owner), std::move (record), parts},
                        waiter);
                  }
                  catch (...) {
                      if (operation)
                          _transport->unregister_local_operation (operation->id);
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
                                       admit_local_application (local_application_dispatch_t{
                                         std::move (owner), std::move (record), parts});
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
                                                   pending_operation_t &operation,
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
    record.source_node_rid = local.routing_id ();
    record.spot_route = target;
    const auto submitted =
      _lifecycle_configuration_lane
        .run ([&] {
            if (!_started || _closing) {
                return zlink::submit_result_t::terminated;
            }
            return _local_dispatch_completion_lane
              .run ([&] {
                  auto waiter = _options.mesh.application_jobs
                                  ? std::make_shared<application_job_queue_t::waiter_t> ()
                                  : nullptr;
                  register_local_completion (operation, timeout, std::move (completion),
                                             local_request_wait_cleanup (waiter),
                                             mesh_request_surface_t::spot);
                  record.operation_id = operation.id;
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

                  try {
                      admit_local_application (
                        local_application_dispatch_t{std::move (owner), std::move (record), parts},
                        waiter);
                  }
                  catch (...) {
                      _transport->unregister_local_operation (operation.id);
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
                                                std::vector<std::uint8_t> payload)
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
                parts = protocol::decode_application_parts (
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
