/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/stateful/public_host_runtime_internal.hpp"

#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/session_binding_diagnostics.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/pending_creation_projection.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/mesh/service_channel_membership.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"

#include <algorithm>
#include <exception>
#include <functional>
#include <iomanip>
#include <limits>
#include <runtime/locations/location_repository.hpp>
#include <service_wire_codec.hpp>
#include <service_wire_constants.hpp>
#include <service_wire_pilot_codec.hpp>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>

namespace zlink::framework::runtime::host
{

using public_host_runtime_detail::session_relocation_key;
using public_host_runtime_detail::stateful_failure_pair;
using public_host_runtime_detail::trace_mesh_host_enabled;
using ::zlink::framework::detail::mesh_trace_enabled;

namespace
{
constexpr std::chrono::hours instance_activation_recovery_retention{24};

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

bool user_spot_operation_replay_expired (std::uint64_t deadline_unix_ms,
                                         std::int64_t now_unix_ms,
                                         std::chrono::milliseconds replay_retention)
{
    return now_unix_ms >= 0 && static_cast<std::uint64_t> (now_unix_ms) > deadline_unix_ms
           && static_cast<std::uint64_t> (now_unix_ms) - deadline_unix_ms
                > static_cast<std::uint64_t> (
                  std::max<std::int64_t> (0, replay_retention.count ()));
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
} // namespace

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

void public_host_runtime_t::configure_location_execution (
  std::function<std::chrono::steady_clock::time_point ()> clock,
  std::function<task_t<runtime::serial_execution_queue_t::ready_confirmation_t> (
    stateful::object_ref_t)> observer,
  std::function<std::optional<location_owner_token_t> (const runtime::serial_execution_queue_t &)>
    bound_owner)
{
    if (!clock || !observer)
        throw std::invalid_argument (
          "Location execution observation requires a clock and an owner");
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started)
            throw std::logic_error ("Location execution must be configured before host start");
        _location_execution_clock = std::move (clock);
        _store_ready_observer = std::move (observer);
        _bound_execution_owner = std::move (bound_owner);
    });
}

task_t<std::function<task_t<void> (std::optional<authority_snapshot_t>)>>
public_host_runtime_t::prepare_store_ready (stateful::object_ref_t object)
{
    using confirmation_t = std::function<task_t<void> (std::optional<authority_snapshot_t>)>;
    if (!_store_ready_observer)
        co_return confirmation_t{};
    auto writer = co_await _store_ready_observer (object);
    if (!writer)
        co_return confirmation_t{};
    co_return [host = shared_from_this (), object = std::move (object), writer = std::move (writer),
               clock = _location_execution_clock] (std::optional<authority_snapshot_t> snapshot) {
        return host->_objects.confirm_location_ready (object, std::move (snapshot), writer,
                                                      clock ());
    };
}

task_t<std::optional<stateful::accepted_record_authority_t>>
public_host_runtime_t::accepted_record_authority (
  stateful::accepted_record_authority_query_t query,
  std::shared_ptr<const location_owner_token_t> owner,
  live_location_reader_t *sources) const
{
    using authority_t = std::optional<stateful::accepted_record_authority_t>;
    using descriptor_t = std::shared_ptr<const mesh_node_descriptor_t>;
    try {
        const auto &local = _options.mesh.descriptor;
        // The execution owner checks a Spot's current incarnation. Actor admission
        // validates the native object fence here, independently of sender availability.
        if (query.target_node_routing_id != local.node_routing_id
            || query.target_node_generation != local.lifecycle_generation)
            throw detail_make_boundary_exception (detail::boundary_error_t::stale_generation,
                                                  "accepted target fence is stale");
        if (query.target.kind == stateful::object_kind_t::actor
            && query.requested_object_generation == query.target.object_generation
            && query.requested_authority_owner_generation
                 != query.target.authority_owner_generation) {
            if (query.direct_request && owner && _local_lease_observation && sources
                && query.requested_authority_owner_generation
                     > query.target.authority_owner_generation)
                return accept_committed_actor_request (std::move (query), *sources);
            throw detail_make_boundary_exception (detail::boundary_error_t::stale_generation,
                                                  "accepted target fence is stale");
        }
        // Admission has ended: only the exact mailbox's retained execution owner may accept.
        // Healthy admission continues without this state-lane lookup.
        if (!owner && _bound_execution_owner) {
            auto gate = _objects.execution_gate (query.target);
            return continue_task<authority_t> (
              std::move (gate),
              [self = shared_from_this (), query = std::move (query),
               sources] (std::shared_ptr<runtime::serial_execution_queue_t> queue) mutable {
                  auto token = queue ? self->_bound_execution_owner (*queue) : std::nullopt;
                  if (!token)
                      return task_t<authority_t> (result_t<authority_t>::success (std::nullopt));
                  return self->accepted_record_authority (
                    std::move (query),
                    std::make_shared<const location_owner_token_t> (std::move (*token)), sources);
              });
        }
        const auto finish =
          [owner = std::move (owner)] (stateful::accepted_record_authority_query_t query) {
              authority_t accepted;
              if (owner && !owner->owner_id.empty () && owner->lease_generation > 0
                  && query.source_original) {
                  protocol::request_source_fence_t source;
                  if (const auto *reference =
                        std::get_if<mesh::local_request_source_t> (&*query.source_original)) {
                      source = {reference->lease->owner_id,
                                static_cast<std::uint64_t> (reference->lease->lease_generation),
                                query.source_node_routing_id, query.source_node_generation};
                  } else {
                      const auto &descriptor = std::get<descriptor_t> (*query.source_original);
                      if (!descriptor)
                          return task_t<authority_t> (
                            result_t<authority_t>::success (std::move (accepted)));
                      source = {descriptor->owner_id,
                                static_cast<std::uint64_t> (descriptor->lease_generation),
                                query.source_node_routing_id, query.source_node_generation};
                  }
                  accepted = stateful::accepted_record_authority_t{
                    std::move (source), static_cast<std::uint64_t> (owner->lease_generation)};
              }
              return task_t<authority_t> (result_t<authority_t>::success (std::move (accepted)));
          };
        if (query.source_original)
            if (const auto *lookup =
                  std::get_if<mesh::request_source_lookup_t> (&*query.source_original)) {
                _transport->mailbox ().start_source_lookup (*lookup);
                auto pending = (*lookup)->completion.task ();
                return continue_task<authority_t> (
                  std::move (pending),
                  [query = std::move (query), finish] (descriptor_t descriptor) mutable {
                      query.source_original = mesh::request_source_t{std::move (descriptor)};
                      return finish (std::move (query));
                  });
            }
        // Location §9.3: the first accepting owner resolves once; forwarded records keep it.
        if (!query.source_original && sources) {
            auto cached = sources->cached_request_source (
              local.mesh_name, query.source_node_routing_id, query.source_node_generation);
            if (!cached) {
                auto pending = sources->request_source (
                  local.mesh_name, query.source_node_routing_id, query.source_node_generation);
                return continue_task<authority_t> (
                  std::move (pending),
                  [query = std::move (query), finish] (descriptor_t descriptor) mutable {
                      query.source_original = mesh::request_source_t{std::move (descriptor)};
                      return finish (std::move (query));
                  });
            }
            query.source_original = mesh::request_source_t{std::move (*cached)};
        }
        return finish (std::move (query));
    }
    catch (...) {
        return task_t<authority_t> (detail::current_exception_result<authority_t> ());
    }
}

task_t<std::optional<stateful::accepted_record_authority_t>>
public_host_runtime_t::accept_committed_actor_request (
  stateful::accepted_record_authority_query_t query, live_location_reader_t &sources) const
{
    auto expected = query.target;
    expected.authority_owner_generation = query.requested_authority_owner_generation;
    // The mailbox retains the original in order while the target confirms the Store.
    // SpotWide commit publishes ordinary participant rows, and the registered
    // relocation attempt owns their shared restore identity.
    const auto staged_owner = _local_lease_observation ();
    const auto restore = co_await _relocation_session_terminal_lane.run_task (
      [&] () -> std::optional<stateful::relocation_restore_identity_t> {
          for (const auto &[key, attempt] : _relocation_target_attempts) {
              const auto &target = attempt.prepare.target;
              if (!staged_owner || !attempt.cutover_received || relocation_target_terminal (attempt)
                  || target.target_node_routing_id != query.target_node_routing_id
                  || target.target_node_generation != query.target_node_generation
                  || target.target_owner_id != staged_owner->owner_id
                  || target.target_owner_lease_generation
                       != static_cast<std::uint64_t> (staged_owner->lease_generation))
                  continue;
              const auto participant = std::find_if (
                attempt.targets.begin (), attempt.targets.end (), [&] (const auto &candidate) {
                    return candidate == query.target || candidate == expected;
                });
              const auto index = static_cast<std::size_t> (participant - attempt.targets.begin ());
              if (index < attempt.sources.size ()
                  && expected.authority_owner_generation
                       > attempt.sources[index].authority_owner_generation)
                  return attempt.restore_identity;
          }
          return std::nullopt;
      });
    auto owner = staged_owner;
    const auto stale = [] {
        return detail_make_boundary_exception (detail::boundary_error_t::stale_generation,
                                               "accepted target fence is stale");
    };
    if (restore) {
        const auto read = co_await sources.read_authority (actor_authority_key (expected.key));
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
        owner = _local_lease_observation ();
        if (!snapshot || snapshot->object_generation != expected.object_generation
            || snapshot->authority_owner_generation != expected.authority_owner_generation
            || snapshot->allocation.object_kind != placement_object_kind_t::actor
            || snapshot->allocation.state != placement_allocation_state_t::active
            || snapshot->allocation.target.mesh_name != expected.mesh_name
            || snapshot->allocation.target.node_rid.value () != expected.node_id
            || snapshot->allocation.target.node_lifecycle_generation != query.target_node_generation
            || !owner || owner->owner_id != staged_owner->owner_id
            || owner->lease_generation != staged_owner->lease_generation
            || owner->owner_id != snapshot->owner.owner_id
            || owner->lease_generation != snapshot->owner.lease_generation)
            throw stale ();
    } else {
        const auto current = co_await _objects.find_async (expected.kind, expected.key);
        if (current != expected)
            throw stale ();
    }
    query.target = expected;
    auto authority =
      co_await accepted_record_authority (std::move (query), std::move (owner), &sources);
    if (authority)
        authority->committed_target = std::make_shared<const stateful::committed_target_ingress_t> (
          stateful::committed_target_ingress_t{
            std::move (expected), restore.value_or (stateful::relocation_restore_identity_t{})});
    co_return authority;
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

void public_host_runtime_t::configure_user_spot_operations (
  std::shared_ptr<zlink::framework::location_repository_t> store,
  user_spot_materializer_t materializer,
  user_spot_closer_t closer)
{
    if (!store || !materializer)
        throw std::invalid_argument (
          "User Spot operations require a Location Store and materializer");
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started)
            throw std::logic_error ("User Spot operations must be configured before start");
        _user_spot_store = std::move (store);
        _user_spot_materializer = std::move (materializer);
        _user_spot_closer = std::move (closer);
    });
}

void public_host_runtime_t::configure_spot_route_fence_resolver (
  spot_route_fence_resolver_t resolver)
{
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started)
            throw std::logic_error (
              "Spot route fence resolver must be configured before host start");
        _spot_route_fence_resolver = std::move (resolver);
    });
}

void public_host_runtime_t::configure_actor_destroy (actor_destroy_target_t target)
{
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started)
            throw std::logic_error ("Actor destroy target must be configured before host start");
        _actor_destroy_target = std::move (target);
    });
}

void public_host_runtime_t::configure_actor_create_operations (
  actor_create_operation_target_t target)
{
    if (!target)
        throw std::invalid_argument ("Actor create operation target is required");
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started || _actor_create_target)
            throw std::logic_error (
              "Actor create operations must be configured once before host start");
        _actor_create_target = std::move (target);
    });
}

void public_host_runtime_t::configure_actor_join_operations (actor_join_operation_target_t target)
{
    if (!target)
        throw std::invalid_argument ("Actor join operation target is required");
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started || _actor_join_target)
            throw std::logic_error (
              "Actor join operations must be configured once before host start");
        _actor_join_target = std::move (target);
    });
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
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started)
            throw std::logic_error ("Instance Spot operations must be configured before start");
        _user_spot_store = std::move (store);
        _session_relocations = relocations;
        _instance_spot_relocations = std::move (relocations);
        _instance_spot_owner = std::move (owner);
        for (const auto &type : materializer.metric_types)
            _instance_admission_metrics.emplace (
              type, std::make_shared<runtime::instance_spot_admission_metrics_t> (
                      _options.mesh.descriptor.mesh_name, type, _options.mesh.metric_provider));
        _instance_spot_materializer =
          std::make_shared<const instance_spot_activation_materializer_t> (
            std::move (materializer));
    });
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

/* Deletes only the authority version owned by this operation. The Store
 * transaction also releases its active capacity. */
task_t<bool>
release_authority_async (std::shared_ptr<zlink::framework::location_repository_t> store,
                         authority_key_t authority_key,
                         std::string expected_store_version,
                         std::function<task_t<bool> ()> before_delete)
{
    if (before_delete)
        (void) co_await before_delete ();
    const auto deleted = co_await await_result (store->compare_exchange_authority (
      authority_key, expected_store_version, authority_delete_t{}));
    if (!deleted)
        co_return close_store_failure (deleted);
    if (!std::holds_alternative<authority_deleted_t> (deleted.value ()))
        co_return result_t<bool>::failure (framework_error_kind_t::internal_failure,
                                           "Spot authority release failed");
    co_return true;
}

std::function<task_t<bool> ()>
release_authority (std::shared_ptr<zlink::framework::location_repository_t> store,
                   authority_key_t authority_key,
                   std::string expected_store_version,
                   std::function<task_t<bool> ()> before_delete)
{
    return [store = std::move (store), authority_key = std::move (authority_key),
            expected_store_version = std::move (expected_store_version),
            before_delete = std::move (before_delete)] {
        return release_authority_async (store, authority_key, expected_store_version,
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
    _lifecycle_configuration_lane.run_sync ([&] {
        store = _user_spot_store;
        instance_owner_resolver = _instance_spot_owner;
    });
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
                completion, {result_t<bool>::success (true),
                             release_authority (store, authority_key, snapshot->store_version, {}),
                             reincarnate_closing_authority (store, authority_key,
                                                            snapshot->store_version, ready_payload),
                             [store, authority_key] (const authority_snapshot_t &next) {
                                 return release_authority (store, authority_key, next.store_version,
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
                   release_authority (store, authority_key, closing->snapshot.store_version, {}),
                   reincarnate_closing_authority (store, authority_key,
                                                  closing->snapshot.store_version, ready_payload),
                   [store, authority_key] (const authority_snapshot_t &next) {
                       return release_authority (store, authority_key, next.store_version, {}) ();
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
        return release_authority (store, authority_key, std::move (closing_version), [self, ref] {
            return release_user_spot_record_async (self, ref);
        });
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
        foundation::operation_terminal_t terminal,
        ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
          auto packed = std::move (completion_payload.value ());
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

task_t<std::size_t> public_host_runtime_t::recover_instance_spot_activations ()
{
    std::shared_ptr<zlink::framework::location_repository_t> store;
    std::shared_ptr<stateful::relocation_store_port_t> relocations;
    std::shared_ptr<const instance_spot_activation_materializer_t> materializer;
    co_await _lifecycle_configuration_lane.run_task ([&] {
        store = _user_spot_store;
        relocations = _instance_spot_relocations;
        materializer = _instance_spot_materializer;
    });
    if (!store || !relocations || !materializer)
        co_return 0;
    const auto local = _transport->topology ().local_descriptor ();
    std::size_t recovered = 0;
    std::optional<authority_scan_cursor_t> cursor;
    do {
        const auto scanned = co_await store->list_authorities ("zla1:s:", cursor, 256);
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
                if (co_await store->release_ended_reservation (entry.key,
                                                               entry.snapshot.store_version))
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
                const auto deleted = co_await store->compare_exchange_authority (
                  entry.key, entry.snapshot.store_version, authority_delete_t{});
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
                    prepared = co_await materializer->prepare (recovery.activation, entry.snapshot,
                                                               activation_terminal.preparation);
                }
                catch (...) {
                    prepared = false;
                }
                if (!prepared) {
                    co_await activation_terminal.finish (false);
                    continue;
                }
                auto dispatched = co_await materializer->dispatch (
                  std::make_shared<const protocol::instance_activation_recovery_t> (recovery), {},
                  [] {});
                activation_terminal.accepted = std::move (dispatched.accepted_turn_terminal);
                auto updated = *state;
                updated.activation_recovery->replay_cursor =
                  updated.activation_recovery->inbox_sequence;
                const auto terminal = co_await store->compare_exchange_authority (
                  entry.key, entry.snapshot.store_version,
                  authority_restore_t{encode_instance_spot_authority_payload (updated),
                                      entry.snapshot.owner});
                const auto *stored = std::get_if<authority_stored_t> (&terminal);
                if (!stored) {
                    co_await activation_terminal.finish (false);
                    continue;
                }
                completed = stored->snapshot;
            }
            const auto completed_state = decode_instance_spot_authority_payload (completed.payload);
            if (!completed_state || !completed_state->activation_recovery
                || completed_state->activation_recovery->replay_cursor
                     != completed_state->activation_recovery->inbox_sequence) {
                co_await activation_terminal.finish (false);
                continue;
            }
            auto released = *completed_state;
            released.activation_recovery.reset ();
            auto confirm = co_await prepare_store_ready (
              {stateful::object_kind_t::instance_spot, state->spot_id, completed.object_generation,
               completed.authority_owner_generation, completed.allocation.target.mesh_name,
               std::string (completed.allocation.target.node_rid.value ())});
            const auto cleared = co_await store->compare_exchange_authority (
              entry.key, completed.store_version,
              authority_restore_t{encode_instance_spot_authority_payload (released),
                                  completed.owner});
            if (!std::holds_alternative<authority_stored_t> (cleared)) {
                co_await activation_terminal.finish (false);
                continue;
            }
            if (confirm)
                co_await confirm (std::get<authority_stored_t> (cleared).snapshot);
            relocations->remove (recovery_pointer.reference);
            co_await activation_terminal.finish ();
            ++recovered;
        }
        cursor = page->next_cursor;
    } while (cursor);
    co_return recovered;
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
        foundation::operation_terminal_t terminal,
        ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
          if (!completion_payload) {
              completion (terminal,
                          detail::propagate_failure<protocol::user_spot_create_reply_t> (
                            completion_payload, "User Spot creation failed"),
                          std::nullopt);
              return;
          }
          auto packed = std::move (completion_payload.value ());
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
          completion (terminal,
                      result_t<protocol::user_spot_create_reply_t>::success (std::move (reply)),
                      std::move (application_reply));
      });
}

task_t<bool>
public_host_runtime_t::create_actor_remote (zlink::routing_id_t target_node,
                                            protocol::actor_create_header_t request,
                                            std::chrono::milliseconds timeout,
                                            actor_create_operation_completion_t completion)
{
    if (!completion)
        throw std::invalid_argument ("Actor create completion is required");
    if (target_node == status ().routing_id ()) {
        actor_create_operation_target_t target;
        co_await _lifecycle_configuration_lane.run_task ([&] { target = _actor_create_target; });
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
        foundation::operation_terminal_t terminal,
        ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
          auto packed = std::move (completion_payload.value ());
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
                                               std::chrono::steady_clock::time_point deadline,
                                               user_spot_close_completion_t completion)
{
    if (!completion)
        throw std::invalid_argument ("User Spot close completion is required");
    co_return co_await _transport->request_user_spot_close (
      target_node.to_bytes (), std::move (request), deadline,
      [completion = std::move (completion)] (
        foundation::operation_terminal_t terminal,
        ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
          auto packed = std::move (completion_payload.value ());
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

task_t<stateful::stateful_error_t>
public_host_runtime_t::advance_local_actor_authority (stateful::object_ref_t committed,
                                                      std::optional<authority_snapshot_t> ready)
{
    auto confirm = co_await prepare_store_ready (committed);
    const auto host = shared_from_this ();
    const auto previous = co_await _spot_actor_index_lane.run_task (
      [&] () -> std::optional<std::pair<std::string, stateful::object_ref_t>> {
          const auto found = _actors.find (committed.key);
          if (found == _actors.end ())
              return std::nullopt;
          return found->second;
      });
    if (!previous)
        co_return stateful::stateful_error_t::none;
    const auto advanced =
      co_await host->_objects.advance_local_actor_authority (committed, previous->first);
    if (advanced != stateful::stateful_error_t::none)
        co_return advanced;
    const auto published = co_await _spot_actor_index_lane.run_task ([&] {
        const auto found = _actors.find (committed.key);
        if (found == _actors.end ())
            return stateful::stateful_error_t::not_found;
        // A duplicate publication is idempotent. A superseding identity or
        // retirement must not be overwritten by the earlier object result.
        if (found->second.first != previous->first
            || (found->second.second != previous->second && found->second.second != committed))
            return stateful::stateful_error_t::generation_stale;
        found->second.second = committed;
        return stateful::stateful_error_t::none;
    });
    if (published == stateful::stateful_error_t::none && confirm)
        co_await confirm (std::move (ready));
    co_return published;
}

actor_handle_t public_host_runtime_t::create_actor (std::string actor_type, std::string actor_id)
{
    const auto existing = _spot_actor_index_lane.run_sync (
      [&] () -> std::optional<std::pair<actor_ref_t, stateful::object_ref_t>> {
          const auto found = _actors.find (actor_id);
          if (found != _actors.end ()) {
              return std::make_pair (
                framework_actor_ref (found->second.second, found->second.first),
                found->second.second);
          }
          return std::nullopt;
      });
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
    _spot_actor_index_lane.run_sync (
      [&] { _actors.insert_or_assign (actor_id, std::make_pair (actor_type, *object)); });
    return actor_handle_t (shared_from_this (), framework_actor_ref (*object, actor_type), *object);
}

task_t<actor_handle_t>
public_host_runtime_t::create_reserved_actor_async (std::string actor_type,
                                                    stateful::object_ref_t reserved)
{
    if (reserved.kind != stateful::object_kind_t::actor)
        throw std::invalid_argument ("reserved Actor reference has an invalid object kind");
    const auto host = shared_from_this ();
    const auto previous = co_await _spot_actor_index_lane.run_task (
      [&] () -> std::optional<std::pair<std::string, stateful::object_ref_t>> {
          const auto found = _actors.find (reserved.key);
          if (found == _actors.end ())
              return std::nullopt;
          return found->second;
      });
    if (previous) {
        if (previous->second.object_generation == reserved.object_generation
            && previous->second.authority_owner_generation == reserved.authority_owner_generation)
            co_return actor_handle_t (shared_from_this (),
                                      framework_actor_ref (previous->second, previous->first),
                                      previous->second);
        const auto adopted =
          co_await host->_objects.adopt_reserved_actor_owner (reserved, actor_type);
        if (adopted != stateful::stateful_error_t::none)
            throw std::runtime_error ("reserved Actor generation does not match the local Actor");
        co_await _spot_actor_index_lane.run_task ([&] {
            const auto found = _actors.find (reserved.key);
            if (found == _actors.end () || found->second.first != previous->first
                || (found->second.second != previous->second && found->second.second != reserved))
                throw std::runtime_error (
                  "reserved Actor generation does not match the local Actor");
            found->second.second = reserved;
        });
        co_return actor_handle_t (host, framework_actor_ref (reserved, previous->first), reserved);
    }
    auto created = co_await _objects.begin_reserved_object_async (reserved, actor_type, {});
    if (created.error != stateful::stateful_error_t::none)
        throw std::runtime_error ("reserved framework Actor authority creation failed");
    //  Blocking on the object lane would hold a completion worker until that same dispatcher
    //  delivered its result.
    if (created.factory_owner) {
        const auto committed = co_await _objects.commit_create_async (created.attempt);
        if (committed != stateful::stateful_error_t::none)
            throw std::runtime_error ("reserved framework Actor Ready commit failed");
    }
    auto object = co_await _objects.find_async (stateful::object_kind_t::actor, reserved.key);
    if (!object)
        throw std::runtime_error ("reserved framework Actor authority is unavailable");
    co_await _spot_actor_index_lane.run_task (
      [&] { _actors.insert_or_assign (reserved.key, std::make_pair (actor_type, *object)); });
    co_return actor_handle_t (shared_from_this (), framework_actor_ref (*object, actor_type),
                              *object);
}

task_t<void> public_host_runtime_t::dispatch_instance_spot_activation (
  protocol::instance_activation_recovery_t command,
  std::shared_ptr<const mesh::service_mailbox_reply_target_t> mailbox_record,
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
                      .packet_name = owned_command->application_payload.packet_name (),
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
                          .packet_name = owned_command->application_payload.packet_name (),
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
        previous = co_await _lifecycle_configuration_lane.run_task ([&] {
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
        });
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
        auto completed = admission->task ();
        if (result.terminal_result != 0
            && admission->complete (result_t<instance_activation_admission_t>::success (result)))
            return;
        const auto terminal = detail::task_result_access_t::read (completed);
        if (!terminal || !std::holds_alternative<instance_activation_ready_t> (terminal.value ())) {
            if (result.accepted_turn_terminal)
                result.accepted_turn_terminal ();
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
    if (!result) {
        const auto failure =
          messaging::request_failure_mapper_t{}.target_failure_reply (result.exception ());
        reply_terminal (
          {failure->terminal_result, failure->failure_code, std::nullopt, {}, result.exception ()});
    }
    const auto observed_result = co_await runtime::await_result (std::move (observed));
    if (previous)
        co_await previous->terminal.task ();
    co_await _lifecycle_configuration_lane.run_task ([&] {
        const auto found = _instance_spot_activation_admissions.find (spot_id);
        if (found != _instance_spot_activation_admissions.end () && found->second == operation)
            _instance_spot_activation_admissions.erase (found);
    });
    operation->terminal.complete (result_t<void>::success ());
    observed_result.value ();
}

task_t<void> public_host_runtime_t::dispatch_instance_spot_activation_core (
  std::shared_ptr<protocol::instance_activation_recovery_t> owned_command,
  std::shared_ptr<const mesh::service_mailbox_reply_target_t> mailbox_record,
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
    co_await _lifecycle_configuration_lane.run_task ([&] {
        store = _user_spot_store;
        instance_relocations = _instance_spot_relocations;
        instance_materializer = _instance_spot_materializer;
        instance_owner_resolver = _instance_spot_owner;
    });
    const auto registered_metric = _instance_admission_metrics.find (request.target.stable_type);
    const auto instance_metric =
      registered_metric == _instance_admission_metrics.end () ? nullptr : registered_metric->second;
    const auto finish_admission = [admission] (instance_activation_admission_t result) {
        if (admission)
            admission->complete (
              result_t<instance_activation_admission_t>::success (std::move (result)));
    };
    const auto accepted_activation = [admission] {
        if (!admission)
            return true;
        const auto completed = admission->task ();
        return completed.await_ready () && detail::task_result_access_t::read (completed)
               && std::holds_alternative<instance_activation_ready_t> (
                 detail::task_result_access_t::read (completed).value ());
    };
    const auto accept_activation =
      [admission, admission_command] (const authority_snapshot_t &ready) -> std::function<void ()> {
        if (!admission)
            return [] {};
        return [admission, admission_command, ready] {
            if (admission->complete (result_t<instance_activation_admission_t>::success (
                  instance_activation_ready_t{ready, admission_command})))
                return;
            const auto terminal = detail::task_result_access_t::read (admission->task ());
            terminal.value ();
            if (const auto *failure =
                  std::get_if<instance_spot_activation_result_t> (&terminal.value ())) {
                if (failure->exception)
                    std::rethrow_exception (failure->exception);
                throw messaging::request_failure_mapper_t{}.reply_header_exception (
                  failure->terminal_result, failure->failure_code, "Instance Spot activation");
            }
        };
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
    const auto reject_activation =
      [reply_terminal] (std::optional<instance_spot_activation_result_t> result = std::nullopt) {
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
    struct join_existing_instance_t
    {
        static task_t<bool>
        run (std::decay_t<decltype (lifetime)> self,
             std::decay_t<decltype (owned_command)> owned_command,
             std::decay_t<decltype (instance_metric)> instance_metric,
             std::decay_t<decltype (reply_terminal)> reply_terminal,
             std::decay_t<decltype (instance_materializer)> instance_materializer,
             std::decay_t<decltype (resume_missing)> resume_missing,
             std::decay_t<decltype (accept_activation)> accept_activation,
             std::decay_t<decltype (accepted_activation)> accepted_activation,
             std::decay_t<decltype (reject_activation)> reject_activation,
             authority_read_result_t current,
             std::optional<instance_spot_authority_payload_t> ready_state)
        {
            auto &request = owned_command->activation;

            const auto *snapshot = std::get_if<authority_snapshot_t> (&current);
            if (request.target.authority_owner_generation != 0) {
                // Authority generations identify owner changes within one incarnation.
                if (snapshot && snapshot->allocation.state == placement_allocation_state_t::active
                    && snapshot->object_generation < request.target.object_generation) {
                    const auto failure =
                      messaging::request_failure_mapper_t{}.target_failure_reply (
                        framework_error_kind_t::invalid_operation,
                        static_cast<std::uint32_t> (
                          protocol::framework_error_code::spotGenerationStale));
                    reply_terminal (
                      {failure->terminal_result, failure->failure_code, std::nullopt});
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
                    const auto failure =
                      messaging::request_failure_mapper_t{}.target_failure_reply (
                        framework_error_kind_t::unavailable,
                        static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving));
                    reply_terminal (
                      {failure->terminal_result, failure->failure_code, std::nullopt});
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
                  {107,
                   static_cast<std::uint32_t> (protocol::framework_error_code::spotTypeMismatch),
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
                        reply_terminal ({105,
                                         static_cast<std::uint32_t> (
                                           protocol::framework_error_code::requestFailed),
                                         std::nullopt});
                        co_return true;
                    }
                }
                if ((ready_state->state != instance_spot_authority_state_t::ready
                     && ready_state->state != instance_spot_authority_state_t::closing)
                    || ready_state->stable_type != snapshot->allocation.stable_type
                    || ready_state->spot_id != request.target.spot_id) {
                    reply_terminal (
                      {105,
                       static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
                       std::nullopt});
                    co_return true;
                }
                if (ready_state->state == instance_spot_authority_state_t::closing
                    && request.target.authority_owner_generation != 0
                    && !request.target.instance_intent) {
                    if (instance_metric)
                        instance_metric->conflict (
                          runtime::instance_spot_admission_metrics_t::conflict_reason_t::closing);
                    const auto failure =
                      messaging::request_failure_mapper_t{}.target_failure_reply (
                        framework_error_kind_t::not_found);
                    reply_terminal (
                      {failure->terminal_result, failure->failure_code, std::nullopt});
                    co_return true;
                }
                const auto local = self->status ();
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
                    prepared = ready_state->state == instance_spot_authority_state_t::closing;
                    if (!prepared)
                        prepared = co_await instance_materializer->prepare (
                          request, *snapshot, activation_terminal->preparation);
                }
                catch (...) {
                    preparation_error = std::current_exception ();
                }
                if (!prepared) {
                    co_await activation_terminal->finish (false);
                    if (preparation_error)
                        std::rethrow_exception (preparation_error);
                    reply_terminal ({105,
                                     static_cast<std::uint32_t> (
                                       protocol::framework_error_code::spotCreateFailed),
                                     std::nullopt});
                    co_return true;
                }
                auto running = std::make_shared<task_t<instance_spot_activation_result_t>> (
                  instance_materializer->dispatch (owned_command, resume_missing,
                                                   accept_activation (*snapshot)));
                detail::observe_task_terminal (
                  *running, [running, reply_terminal, activation_terminal, accepted_activation] (
                              const result_t<instance_spot_activation_result_t> &result) {
                      if (result) {
                          auto terminal = result.value ();
                          activation_terminal->accepted =
                            std::move (terminal.accepted_turn_terminal);
                          terminal.accepted_turn_terminal = [activation_terminal,
                                                             accepted_activation] {
                              activation_terminal->finish (accepted_activation ());
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
        }
    };
    const auto join_existing =
      [&] (const authority_read_result_t &current,
           std::optional<instance_spot_authority_payload_t> ready_state) -> task_t<bool> {
        return join_existing_instance_t::run (lifetime, owned_command, instance_metric,
                                              reply_terminal, instance_materializer, resume_missing,
                                              accept_activation, accepted_activation,
                                              reject_activation, current, std::move (ready_state));
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
    const auto activation_creation_target = [&] {
        return object_creation_target_t{
          request.target.mesh_name,
          node_rid_t::from_string (
            zlink::routing_id_t::from (request.target.target_node_routing_id).to_string ()),
          request.target.target_node_generation, *instance_owner};
    };
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
                release.target = activation_creation_target ();
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
    reserve.target = activation_creation_target ();
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
        prepared = co_await instance_materializer->prepare (request, reservation->creating,
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
    auto dispatched = instance_materializer->dispatch (owned_command, resume_missing,
                                                       accept_activation (ready_snapshot));
    const auto dispatch_result = co_await await_result (std::move (dispatched));
    if (!accepted_activation ()) {
        if (dispatch_result)
            activation_terminal->accepted = dispatch_result.value ().accepted_turn_terminal;
        struct release_failed_activation_t
        {
            static task_t<void>
            run (std::decay_t<decltype (activation_terminal)> activation_terminal,
                 std::decay_t<decltype (store)> store,
                 std::decay_t<decltype (authority_key)> authority_key,
                 std::decay_t<decltype (ready_snapshot)> ready_snapshot,
                 std::decay_t<decltype (instance_relocations)> instance_relocations,
                 std::decay_t<decltype (recovery_root)> recovery_root)
            {
                co_await activation_terminal->finish (false);
                (void) co_await release_authority_async (store, authority_key,
                                                         ready_snapshot.store_version, {});
                instance_relocations->remove (recovery_root.reference);
            }
        };
        const auto release_failed_activation = [&] () -> task_t<void> {
            return release_failed_activation_t::run (activation_terminal, store, authority_key,
                                                     ready_snapshot, instance_relocations,
                                                     recovery_root);
        };
        const auto cleanup = co_await await_result (release_failed_activation ());
        if (!cleanup) {
            _options.mesh.runtime_failures->capture ([&] { cleanup.value (); });
            cleanup.value ();
        }
        if (!dispatch_result)
            dispatch_result.value ();
        auto terminal = dispatch_result.value ();
        terminal.accepted_turn_terminal = {};
        reply_terminal (std::move (terminal));
        co_return;
    }
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
            auto confirm = co_await prepare_store_ready (
              {stateful::object_kind_t::instance_spot, request.target.spot_id,
               ready_snapshot.object_generation, ready_snapshot.authority_owner_generation,
               ready_snapshot.allocation.target.mesh_name,
               std::string (ready_snapshot.allocation.target.node_rid.value ())});
            const auto cleared = co_await run_blocking_step<authority_compare_exchange_result_t> (
              [store, authority_key, version = terminal_snapshot->snapshot.store_version,
               payload = encode_instance_spot_authority_payload (ready_state)] {
                  return store->compare_exchange_authority (authority_key, version,
                                                            authority_put_t{payload});
              });
            if (std::holds_alternative<authority_stored_t> (cleared)) {
                if (confirm)
                    co_await confirm (std::get<authority_stored_t> (cleared).snapshot);
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
    const auto dispatch_claim = [this] () -> task_t<std::size_t> {
        receive_batch_budget_t infrastructure_budget;
        auto claim = _transport->mailbox ().try_claim (
          mesh::service_mailbox_domain_t::infrastructure, dispatch_limits::receive_batch_messages,
          dispatch_limits::receive_batch_bytes);
        if (!claim)
            return task_t<std::size_t> (result_t<std::size_t>::success (0));
        try {
            return dispatch_user_spot_operations (std::move (claim), infrastructure_budget);
        }
        catch (...) {
            // The coroutine takes the claim in its first instruction. If its
            // frame or promise cannot be created, this scope still owns it.
            (void) _transport->mailbox ().release (*claim);
            throw;
        }
    };
    try {
        const auto now = std::chrono::steady_clock::now ();
        if (relocation_management_due (now)) {
            std::vector<pending_relocation_assembly_t> expired_relocation_assemblies;
            _relocation_session_terminal_lane.run_sync ([&] {
                for (auto pending = _relocation_assemblies.begin ();
                     pending != _relocation_assemblies.end ();) {
                    if (pending->second.expires_at > now) {
                        ++pending;
                        continue;
                    }
                    expired_relocation_assemblies.push_back (std::move (pending->second));
                    pending = _relocation_assemblies.erase (pending);
                }
            });
            for (const auto &expired : expired_relocation_assemblies)
                reply_relocation_assembly_failure (
                  expired, protocol::framework_error_code::relocationDataLost);
            return continue_task<std::size_t> (
              poll_relocation_target_attempts (), [this, dispatch_claim] {
                  flush_pending_session_relocation_seals ();
                  _relocation_session_terminal_lane.run_sync (
                    [this] { publish_relocation_next_due_on_lane (); });

                  return dispatch_claim ();
              });
        }
        return dispatch_claim ();
    }
    catch (...) {
        return task_t<std::size_t> (detail::current_exception_result<std::size_t> ());
    }
}

task_t<std::pair<std::size_t, receive_batch_budget_t>>
public_host_runtime_t::dispatch_user_spot_batch (
  std::shared_ptr<location_repository_t> store,
  user_spot_materializer_t materializer,
  user_spot_closer_t user_spot_closer,
  actor_destroy_target_t actor_destroy_target,
  actor_create_operation_target_t actor_create_target,
  actor_join_operation_target_t actor_join_target,
  std::function<std::optional<location_owner_token_t> ()> session_route_owner_resolver,
  std::function<void (const protocol::message_follow_notice_t &)> message_follow_handler,
  bound_session_operations_t bound_session_operations,
  std::function<void (const protocol::session_relocation_route_t &)>
    late_session_route_update_reporter,
  receive_batch_budget_t infrastructure_budget,
  std::size_t count,
  std::shared_ptr<mesh::service_mailbox_claim_t> claim)
{
    for (std::size_t index = 0; index < claim->claimed_messages; ++index) {
        auto original = _transport->mailbox ().take ({claim, index});
        if (!original)
            continue;
        auto &mailbox_record = *original;
        std::size_t record_bytes = 0;
        for (const auto &part : mailbox_record.parts ())
            record_bytes = part.size () > std::numeric_limits<std::size_t>::max () - record_bytes
                             ? std::numeric_limits<std::size_t>::max ()
                             : record_bytes + part.size ();
        infrastructure_budget.account (record_bytes);
        ++count;
        activation_terminal_t preparation_terminal;
        std::optional<std::pair<std::uint32_t, std::uint32_t>> dispatch_failure;
        try {
            const auto wire = mailbox_record.service_header ();
            if (wire.kind == protocol::command::actorDestroy) {
                if (actor_destroy_target) {
                    auto command = protocol::decode_actor_destroy (
                      protocol::message_bytes (mailbox_record.parts ().front ()));
                    auto pending = actor_destroy_target (std::move (command.actor));
                    detail::observe_task_completion (pending, [] (const result_t<bool> &result) {
                        if (!result && result.error ())
                            trace_mesh_host ("actor-destroy-failed", result.error ()->what ());
                    });
                }
                continue;
            }
            if (wire.kind == protocol::command::messageFollow) {
                if (mailbox_record.parts ().size () != 1
                    || mailbox_record.source_node_generation == 0)
                    continue;
                const auto notice = protocol::decode_message_follow (
                  protocol::message_bytes (mailbox_record.parts ().front ()));
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
                  protocol::message_bytes (mailbox_record.parts ().front ()));
                const auto local = status ();
                const auto actor = co_await _objects.find_async (stateful::object_kind_t::actor,
                                                                 bind.actor.actor_id);
                const auto authority_matches = bound_session_bind_actor_matches (
                  bind.actor, actor, local.routing_id (), local.lifecycle_generation ());
                const auto admission = classify_bound_session_bind_admission (authority_matches);
                trace_mesh_host ("bound-session-bind-admission",
                                 "actor=" + bind.actor.actor_id + " admission="
                                   + (admission == bound_session_bind_admission_t::ready ? "ready"
                                      : admission == bound_session_bind_admission_t::stale_route
                                        ? "stale_route"
                                        : "actor_not_ready")
                                   + " requested_authority="
                                   + std::to_string (bind.actor.authority_owner_generation)
                                   + " requested_lease="
                                   + std::to_string (bind.actor.owner_lease_generation)
                                   + " authority_matches=" + (authority_matches ? "true" : "false")
                                   + " local_actor=" + (actor ? "found" : "missing"));
                bound_session_bind_operation_result_t operation_result{
                  stateful::stateful_error_t::conflict, std::nullopt};
                bool terminal_replied = false;
                const auto session_owner_rid = mailbox_record.source_rid ();
                std::optional<location_owner_token_t> session_owner;
                if (admission == bound_session_bind_admission_t::ready
                    && bound_session_operations.bind && bound_session_operations.session_owner)
                    session_owner = co_await bound_session_operations.session_owner (
                      session_owner_rid, mailbox_record.source_node_generation);
                if (admission == bound_session_bind_admission_t::ready && session_owner) {
                    operation_result = co_await bound_session_operations.bind (
                      bind, session_owner_rid, mailbox_record.source_node_generation,
                      *session_owner,
                      [this, &mailbox_record,
                       &terminal_replied] (std::uint32_t terminal, std::uint32_t failure,
                                           std::optional<protocol::stream_bind_reply_tail_t> tail) {
                          terminal_replied = _transport->reply_bound_session_bind (
                            mailbox_record, terminal, failure, std::move (tail));
                          return terminal_replied;
                      });
                }
                // One terminal per bind. Session-Actor binding §4: without the Session
                // owner descriptor the bind is Unavailable.
                using terminal_t = protocol::request_terminal_result;
                using code_t = protocol::framework_error_code;
                const auto [terminal, code] =
                  operation_result.terminal_result
                    ? std::pair{static_cast<terminal_t> (*operation_result.terminal_result),
                                static_cast<code_t> (operation_result.failure_code)}
                  : operation_result.error == stateful::stateful_error_t::none
                    ? std::pair{terminal_t::ok, code_t::none}
                  : admission == bound_session_bind_admission_t::stale_route
                    ? std::pair{terminal_t::conflict, code_t::actorLocationStale}
                  : admission == bound_session_bind_admission_t::actor_not_ready
                    ? std::pair{terminal_t::busy, code_t::none}
                    : std::pair{terminal_t::internalError, code_t::routeNotConnected};
                const auto replied =
                  terminal_replied
                  || _transport->reply_bound_session_bind (
                    mailbox_record, static_cast<std::uint32_t> (terminal),
                    static_cast<std::uint32_t> (code), operation_result.reply_tail);
                if (replied && operation_result.replacement) {
                    (void) co_await _transport->send_bound_session_replaced (
                      operation_result.replacement->retired_session.session_owner_node_routing_id,
                      *operation_result.replacement);
                }
                continue;
            }
            if (wire.kind == protocol::command::boundSessionClose) {
                const auto request = protocol::decode_bound_session_close (
                  protocol::message_bytes (mailbox_record.parts ().front ()));
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
                  closed
                    ? std::optional<messaging::request_wire_failure_t>{}
                    : messaging::request_failure_mapper_t{}.target_failure_reply (*closed.error ());
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
                  protocol::message_bytes (mailbox_record.parts ().front ()));
                trace_mesh_host ("bound-session-replacement-received",
                                 "actor=" + replacement.actor_authority.actor_id);
                if (bound_session_operations.replaced)
                    bound_session_operations.replaced (replacement);
                continue;
            }
            if (wire.kind == protocol::command::relocationPrepare) {
                if (mailbox_record.parts ().empty () || !mailbox_record.reply_token ()
                    || !session_route_owner_resolver)
                    continue;
                const auto control = protocol::decode_relocation_control (
                  protocol::message_bytes (mailbox_record.parts ().front ()));
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
                    const auto join_prepare = _actor_join_relocation_prepare_validator (*prepare);
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
                        std::pair<prepare_state_t, std::shared_ptr<task_completion_source_t<void>>>;
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
                      if (_options.mesh.shutdown_admission_seal->load (std::memory_order_acquire)
                          || local.state == node_status_t::state_t::draining
                          || local.state == node_status_t::state_t::stopped
                          || local.state == node_status_t::state_t::error)
                          return state_t{prepare_state_t::rejected, nullptr};
                      lower_relocation_next_due_on_lane (relocation_due_now);
                      _relocation_assemblies.emplace (
                        key, pending_relocation_assembly_t{
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
                if (mailbox_record.parts ().size () != 1)
                    continue;
                const auto control = protocol::decode_relocation_control (
                  protocol::message_bytes (mailbox_record.parts ().front ()));
                const auto *state = std::get_if<protocol::relocation_state_t> (&control);
                if (!state || state->sender_role != protocol::relocation_role_t::source
                    || state->chunk_data.empty ())
                    continue;
                const relocation_attempt_key_t key{state->relocation.high, state->relocation.low,
                                                   state->target_attempt_generation};
                std::shared_ptr<pending_relocation_assembly_t> completed;
                std::optional<pending_relocation_assembly_t> failed;
                const auto assembly_current =
                  co_await _relocation_session_terminal_lane.run_task ([&] {
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
                      } else if (accepted == stateful::relocation_assembly_result_t::completed) {
                          completed = std::make_shared<pending_relocation_assembly_t> (
                            std::move (found->second));
                          relocation_target_attempt_t attempt;
                          attempt.prepare = completed->prepare;
                          attempt.pending = completed;
                          attempt.terminal = std::make_shared<task_completion_source_t<void>> ();
                          lower_relocation_next_due_on_lane (relocation_due_now);
                          _relocation_target_attempts.emplace (key, std::move (attempt));
                          _relocation_assemblies.erase (found);
                      }
                      return true;
                  });
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
                if (mailbox_record.parts ().size () != 1)
                    continue;
                const auto control = protocol::decode_relocation_control (
                  protocol::message_bytes (mailbox_record.parts ().front ()));
                const auto *cutover = std::get_if<protocol::relocation_cutover_t> (&control);
                if (!cutover || cutover->sender_role != protocol::relocation_role_t::source)
                    continue;
                const relocation_attempt_key_t key{cutover->relocation.high,
                                                   cutover->relocation.low,
                                                   cutover->target_attempt_generation};
                std::optional<std::vector<
                  std::tuple<stateful::object_ref_t, protocol::relocation_data_t, std::uint64_t>>>
                  verified;
                std::optional<relocation_target_attempt_t> mismatched;
                co_await _relocation_session_terminal_lane.run_task ([&] {
                    const auto found = _relocation_target_attempts.find (key);
                    if (found == _relocation_target_attempts.end ()
                        || !relocation_target_wire_ready (found->second)
                        || found->second.prepare.coordinator != cutover->coordinator
                        || found->second.prepare.object != cutover->object
                        || found->second.prepare.source_node_routing_id
                             != mailbox_record.source_routing_id
                        || found->second.prepare.source_node_generation
                             != mailbox_record.source_node_generation
                        || _options.mesh.shutdown_admission_seal->load (std::memory_order_acquire)
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
                        verified.emplace (std::make_move_iterator (
                                            batch.end () - static_cast<std::ptrdiff_t> (count)),
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
                });
                if (verified) {
                    bool staged = true;
                    for (const auto &[target, data, participant_id] : *verified)
                        if (staged)
                            staged =
                              co_await stage_relocation_record (target, data, participant_id);
                    co_await _relocation_session_terminal_lane.run_task ([&] {
                        const auto found = _relocation_target_attempts.find (key);
                        if (found == _relocation_target_attempts.end ())
                            return;
                        if (staged) {
                            set_relocation_finalize_at_on_lane (found->second,
                                                                std::chrono::steady_clock::now ());
                            return;
                        }
                        mismatched = take_relocation_target_discard (found->second);
                    });
                    if (staged)
                        (void) co_await try_finalize_relocation_target (key);
                }
                if (mismatched) {
                    std::vector<relocation_target_attempt_t> cleanup;
                    cleanup.push_back (std::move (*mismatched));
                    co_await discard_relocation_target_attempts (std::move (cleanup));
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
                throw protocol::service_wire_error_t ("unsupported infrastructure mailbox command");

            if (wire.kind == protocol::command::sessionRelocationSeal) {
                if (mailbox_record.parts ().size () != 1)
                    continue;
                const auto seal = [&] {
                    try {
                        return protocol::decode_session_relocation_seal (
                          protocol::message_bytes (mailbox_record.parts ().front ()));
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
                              event.source_rid = mailbox_record.source_rid ().to_hex ();
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
                      _options.mesh.dispatch, mailbox_record.source_routing_id, seal.actor.actor_id,
                      "sessionRelocationSeal", message_flow_reason_t::location_unavailable);
                    continue;
                }
                const auto reconcile = _transport->peer_reconcile ();
                const auto [accepted, immediate] = admit_session_relocation_seal (
                  seal, *owner, mailbox_record.source_routing_id, {}, reconcile);
                if (!accepted) {
                    detail::trace_session_binding_control_drop (
                      _options.mesh.dispatch, mailbox_record.source_routing_id, seal.actor.actor_id,
                      "sessionRelocationSeal", message_flow_reason_t::stale_target);
                    continue;
                }
                if (immediate)
                    (void) co_await _transport->send_session_relocation_sealed (
                      mailbox_record.source_routing_id, *immediate);
                continue;
            }

            if (wire.kind == protocol::command::sessionRelocationRoute) {
                if (mailbox_record.parts ().size () != 1 || !session_route_owner_resolver) {
                    continue;
                }
                const auto route = protocol::decode_session_relocation_route (
                  protocol::message_bytes (mailbox_record.parts ().front ()));
                const auto authenticated_sender =
                  route.route.action == protocol::session_relocation_route_action_t::commit
                    ? route.sender_role == protocol::relocation_role_t::target
                        && mailbox_record.source_node_generation != 0
                        && mailbox_record.source_routing_id == route.route.target_node_routing_id
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
                    || route.session_owner_lease_generation
                         > static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ())
                    || owner->lease_generation
                         != static_cast<std::int64_t> (route.session_owner_lease_generation)) {
                    continue;
                }

                const auto relocation_key = session_relocation_key (route);
                bool late_session_route_update = false;
                const auto route_matches_seal =
                  co_await _relocation_session_terminal_lane.run_task ([&] {
                      const auto sealed = _session_seal_terminals.find (relocation_key);
                      if (sealed == _session_seal_terminals.end () || sealed->second.consumed
                          || sealed->second.sealed.seal_result
                               == protocol::session_relocation_seal_result_t::binding_absent) {
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
                          || sealed->second.seal.session_routing_id != route.session_routing_id
                          || sealed->second.seal.binding_generation != route.binding_generation)
                          return false;
                      return true;
                  });
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
                    stateful::stream_session_registry_t::route_terminal_commit_t commit_projection;
                    if (bound_session_operations.commit_relocation_route) {
                        commit_projection = [&route, &bound_session_operations,
                                             previous = *current] (
                                              const stateful::stream_route_admission_t &committed) {
                            return committed.binding
                                   && bound_session_operations.commit_relocation_route (
                                     route, previous, *committed.binding);
                        };
                    }
                    admission = _sessions.commit_remote_route (
                      session_id, route.binding_generation, route.actor.actor_id,
                      route.actor.object_generation, std::move (target),
                      route.route.target_node_generation, route.route.target_owner_lease_generation,
                      std::move (commit_projection));
                } else {
                    admission = _sessions.acknowledge_remote_abort (
                      session_id, route.binding_generation, route.actor.actor_id,
                      route.actor.object_generation);
                }
                if (admission.error != stateful::stateful_error_t::none || !admission.binding) {
                    continue;
                }

                co_await _relocation_session_terminal_lane.run_task ([&] {
                    const auto sealed = _session_seal_terminals.find (relocation_key);
                    if (sealed != _session_seal_terminals.end () && !sealed->second.consumed
                        && sealed->second.ready)
                        sealed->second.consumed = true;
                });
                continue;
            }

            if (wire.kind == protocol::command::actorCreate) {
                const auto request = protocol::decode_actor_create_header (
                  protocol::message_bytes (mailbox_record.parts ().front ()));
                if (!actor_create_target || request.deadline_unix_ms <= unix_milliseconds_now ()) {
                    actor_create_operation_result_t result;
                    result.reply.header.correlation = request.correlation;
                    result.reply.header.terminal_result = actor_create_target ? 101u : 105u;
                    result.reply.header.failure_code =
                      actor_create_target ? 0u
                                          : static_cast<std::uint32_t> (
                                              protocol::framework_error_code::actorCreateFailed);
                    (void) _transport->reply_actor_create (mailbox_record, result.reply,
                                                           std::move (result.application_reply));
                } else {
                    auto completed = std::make_shared<std::atomic_bool> (false);
                    auto reply = [weak = weak_from_this (),
                                  reply_source = mailbox_record.reply_source (),
                                  completed] (actor_create_operation_result_t result) mutable {
                        if (completed->exchange (true, std::memory_order_acq_rel))
                            return;
                        const auto host = weak.lock ();
                        if (!host)
                            return;
                        try {
                            (void) host->_transport->reply_actor_create (
                              *reply_source, result.reply, std::move (result.application_reply));
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
                if (mailbox_record.parts ().empty () || mailbox_record.parts ().size () > 2)
                    throw protocol::service_wire_error_t ("Actor join has an invalid part count");
                auto canonical = try_decode_canonical_actor_join (
                  ::zlink::framework::detail::backend::copy_binding_parts (
                    mailbox_record.parts ()));
                if (!canonical)
                    throw protocol::service_wire_error_t (
                      "Actor join is not a canonical command-28 record");
                const auto &request = canonical->request;
                auto payload = std::move (canonical->payload);
                if (!actor_join_target) {
                    (void) _transport->reply_actor_join (
                      mailbox_record, protocol::actor_join_result_t::rejected, std::nullopt, 0, 0);
                    continue;
                }
                auto completed = std::make_shared<std::atomic_bool> (false);
                auto reply = [weak = weak_from_this (),
                              reply_source = mailbox_record.reply_source (),
                              completed] (actor_join_operation_result_t result) mutable {
                    if (completed->exchange (true, std::memory_order_acq_rel))
                        return;
                    const auto host = weak.lock ();
                    if (!host)
                        return;
                    try {
                        (void) host->_transport->reply_actor_join (
                          *reply_source, result.join_result, result.spot, result.membership_epoch,
                          result.receive_chunk_limit_bytes, result.terminal_result,
                          result.failure_code, std::move (result.application_reply));
                    }
                    catch (...) {
                        host->_options.mesh.runtime_failures->report (std::current_exception ());
                    }
                };
                try {
                    actor_join_target (request, std::move (payload), reply);
                }
                catch (...) {
                    reply (actor_join_operation_result_t{});
                }
                continue;
            }

            if (wire.kind == protocol::command::instanceSpot) {
                const auto request = protocol::decode_instance_spot_activation_header (
                  protocol::message_bytes (mailbox_record.parts ().front ()));
                const auto expected_parts = request.has_metadata ? 3u : 2u;
                if (mailbox_record.parts ().size () != expected_parts)
                    throw protocol::service_wire_error_t (
                      "Instance Spot activation has an invalid part count");
                std::optional<std::vector<std::uint8_t>> metadata;
                if (request.has_metadata)
                    metadata = mailbox_record.parts ()[1].to_bytes ();
                auto application = protocol::decode_application_payload (
                  protocol::message_bytes (mailbox_record.parts ()[request.has_metadata ? 2 : 1]),
                  capture_flow ());
                auto reply_record = mailbox_record.reply_source ();
                auto running = std::make_shared<task_t<void>> (dispatch_instance_spot_activation (
                  {request, std::move (metadata), std::move (application)}, reply_record));
                detail::observe_task_terminal (
                  *running, [self = shared_from_this (), running] (const result_t<void> &result) {
                      if (!result)
                          self->_options.mesh.runtime_failures->report (result.exception ());
                  });
                continue;
            }
            if (wire.kind == protocol::command::userSpotCreate) {
                const auto request = protocol::decode_user_spot_create_header (
                  protocol::message_bytes (mailbox_record.parts ().front ()));
                auto fingerprint_request = request;
                fingerprint_request.correlation = 1;
                const auto request_fingerprint =
                  protocol::encode_user_spot_create_header (fingerprint_request);
                const auto operation_key =
                  user_spot_operation_key (request.source_node_routing_id,
                                           request.source_node_generation, request.operation);
                std::optional<user_spot_terminal_record_t> cached;
                co_await _user_spot_terminal_lane.run_task ([&] {
                    const auto now = unix_milliseconds_now ();
                    std::erase_if (_user_spot_terminals, [this, now] (const auto &entry) {
                        return (!entry.second.header || !entry.second.header.value ().empty ())
                               && user_spot_operation_replay_expired (
                                 entry.second.deadline_unix_ms, now,
                                 _options.user_spot_operation_replay_retention);
                    });
                    const auto found = _user_spot_terminals.find (operation_key);
                    if (found != _user_spot_terminals.end ())
                        cached = found->second;
                });
                if (cached) {
                    if (cached->kind != protocol::command::userSpotCreate
                        || cached->request_fingerprint != request_fingerprint)
                        throw protocol::service_wire_error_t (
                          "user spot operation identity was reused with a different request");
                    if (!cached->header) {
                        (void) _transport->reply_user_spot_create (
                          mailbox_record,
                          detail::propagate_failure<protocol::user_spot_create_reply_t> (
                            cached->header, "User Spot creation failed"),
                          std::nullopt);
                        continue;
                    }
                    auto reply = protocol::decode_user_spot_create_reply (cached->header.value ());
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
                    (void) _transport->reply_user_spot_create (mailbox_record, reply, std::nullopt);
                    continue;
                }
                auto terminal =
                  [&] (std::uint32_t terminal_result, std::uint32_t failure_code,
                       protocol::user_spot_create_result_t result, const std::string &spot,
                       std::uint64_t generation,
                       std::optional<protocol::application_payload_t> application_reply =
                         std::nullopt,
                       result_t<void> outcome = result_t<void>::success ()) {
                      protocol::user_spot_create_reply_t reply{
                        {request.correlation, terminal_result, failure_code},
                        result,
                        spot,
                        generation};
                      user_spot_terminal_record_t stored{
                        protocol::command::userSpotCreate, request.deadline_unix_ms,
                        request_fingerprint,
                        outcome ? result_t<std::vector<std::uint8_t>>::success (
                                    protocol::encode_user_spot_create_reply (
                                      request.correlation, terminal_result, failure_code, result,
                                      spot, generation))
                                : detail::propagate_failure<std::vector<std::uint8_t>> (
                                    outcome, "User Spot creation failed"),
                        application_reply};
                      _user_spot_terminal_lane.run_sync ([&] {
                          _user_spot_terminals.insert_or_assign (operation_key, std::move (stored));
                      });
                      (void) _transport->reply_user_spot_create (
                        mailbox_record,
                        outcome ? result_t<protocol::user_spot_create_reply_t>::success (reply)
                                : detail::propagate_failure<protocol::user_spot_create_reply_t> (
                                    outcome, "User Spot creation failed"),
                        std::move (application_reply));
                  };
                if (!store || !materializer) {
                    terminal (
                      static_cast<std::uint32_t> (protocol::request_terminal_result::internalError),
                      static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
                      protocol::user_spot_create_result_t::rejected, {}, 0);
                    continue;
                }
                if (request.deadline_unix_ms <= unix_milliseconds_now ()) {
                    terminal (
                      static_cast<std::uint32_t> (protocol::request_terminal_result::timedOut), 0,
                      protocol::user_spot_create_result_t::rejected, {}, 0);
                    continue;
                }
                const auto &global_id = request.spot_id;
                const auto read = co_await store->read_authority (spot_authority_key (global_id));
                const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
                const auto &reservation = request.reservation;
                const auto exact =
                  snapshot && snapshot->store_version == reservation.expected_store_version
                  && snapshot->object_generation == reservation.object_generation
                  && snapshot->authority_owner_generation == reservation.authority_owner_generation
                  && snapshot->owner.owner_id == reservation.target_owner_id
                  && snapshot->owner.lease_generation == reservation.target_owner_lease_generation
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
                const auto fence = public_fence (reservation, snapshot->allocation.target.mesh_name,
                                                 request.stable_type);
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
                    terminal (
                      static_cast<std::uint32_t> (protocol::request_terminal_result::internalError),
                      static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
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
                    terminal (static_cast<std::uint32_t> (protocol::request_terminal_result::ok), 0,
                              protocol::user_spot_create_result_t::existing, request.spot_id,
                              exact_ref.object_generation);
                    continue;
                }
                auto local = co_await _objects.begin_reserved_object_async (
                  exact_ref, request.stable_type, [&] {
                      std::vector<std::uint8_t> bytes;
                      bytes.reserve (creation_payload->size ());
                      for (const auto value : *creation_payload)
                          bytes.push_back (std::to_integer<std::uint8_t> (value));
                      return bytes;
                  }());
                if (local.status == stateful::create_status_t::existing) {
                    terminal (static_cast<std::uint32_t> (protocol::request_terminal_result::ok), 0,
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
                auto materialization = result_t<void>::success ();
                try {
                    materialized =
                      co_await materializer (exact_ref, request.stable_type, *creation_payload);
                }
                catch (...) {
                    materialization = detail::current_exception_result<void> ();
                }
                if (!materialization) {
                    (void) _objects.abort_create (local.attempt);
                    (void) co_await await_result (store->abort (
                      {{placement_object_kind_t::user_spot, global_id},
                       public_fence (reservation, snapshot->allocation.target.mesh_name,
                                     request.stable_type)}));
                    terminal (
                      static_cast<std::uint32_t> (protocol::request_terminal_result::internalError),
                      static_cast<std::uint32_t> (protocol::framework_error_code::spotCreateFailed),
                      protocol::user_spot_create_result_t::rejected, {}, 0, std::nullopt,
                      materialization);
                    continue;
                }
                preparation_terminal.preparation = std::move (materialized.preparation_terminal);
                if (!materialized.accepted) {
                    co_await preparation_terminal.finish (false);
                    (void) co_await store->abort (
                      {{placement_object_kind_t::user_spot, global_id}, fence}, {},
                      std::chrono::system_clock::time_point (
                        std::chrono::milliseconds (request.deadline_unix_ms)));
                    (void) _objects.abort_create (local.attempt);
                    terminal (static_cast<std::uint32_t> (protocol::request_terminal_result::ok), 0,
                              protocol::user_spot_create_result_t::rejected, request.spot_id,
                              exact_ref.object_generation,
                              std::move (materialized.application_reply));
                    continue;
                }
                auto confirm = co_await prepare_store_ready (exact_ref);
                const auto committed =
                  co_await store->commit ({{placement_object_kind_t::user_spot, global_id},
                                           fence,
                                           ready_user_spot_authority_payload (
                                             exact_ref, request.stable_type, fence.target)},
                                          {},
                                          std::chrono::system_clock::time_point (
                                            std::chrono::milliseconds (request.deadline_unix_ms)));
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
                if (confirm)
                    co_await confirm (ready ? ready->ready : already->ready);
                auto local_commit = co_await _objects.commit_create_async (local.attempt);
                if (local_commit != stateful::stateful_error_t::none) {
                    std::vector<std::uint8_t> creation_bytes;
                    creation_bytes.reserve (creation_payload->size ());
                    for (const auto value : *creation_payload)
                        creation_bytes.push_back (std::to_integer<std::uint8_t> (value));
                    const auto reconciled = co_await _objects.begin_reserved_object_async (
                      exact_ref, request.stable_type, std::move (creation_bytes));
                    if (reconciled.status == stateful::create_status_t::existing
                        && reconciled.object == exact_ref)
                        local_commit = stateful::stateful_error_t::none;
                    else if ((reconciled.status == stateful::create_status_t::reserved
                              || reconciled.status == stateful::create_status_t::joined)
                             && reconciled.attempt != 0)
                        local_commit = co_await _objects.commit_create_async (reconciled.attempt);
                }
                if (local_commit != stateful::stateful_error_t::none) {
                    co_await preparation_terminal.finish (false);
                    terminal (
                      static_cast<std::uint32_t> (protocol::request_terminal_result::internalError),
                      static_cast<std::uint32_t> (protocol::framework_error_code::spotCreateFailed),
                      protocol::user_spot_create_result_t::rejected, {}, 0);
                    continue;
                }
                co_await preparation_terminal.finish ();
                terminal (static_cast<std::uint32_t> (protocol::request_terminal_result::ok), 0,
                          protocol::user_spot_create_result_t::created, request.spot_id,
                          exact_ref.object_generation, std::move (materialized.application_reply));
                continue;
            }

            const auto request = protocol::decode_user_spot_close_header (
              protocol::message_bytes (mailbox_record.parts ().front ()));
            auto fingerprint_request = request;
            fingerprint_request.correlation = 1;
            const auto request_fingerprint =
              protocol::encode_user_spot_close_header (fingerprint_request);
            const auto operation_key = user_spot_operation_key (
              request.source_node_routing_id, request.source_node_generation, request.operation);
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
            const auto admission = co_await _user_spot_terminal_lane.run_task ([&] {
                const auto now = unix_milliseconds_now ();
                std::erase_if (_user_spot_terminals, [this, now] (const auto &entry) {
                    return (!entry.second.header || !entry.second.header.value ().empty ())
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
                    if (found->second.header && found->second.header.value ().empty ()) {
                        found->second.waiting.emplace_back (mailbox_record.reply_source (),
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
                                              result_t<std::vector<std::uint8_t>>::success ({}),
                                              std::nullopt,
                                              {}});
                return admission_t::start;
            });
            if (admission == admission_t::joined)
                continue;
            if (admission == admission_t::replay) {
                auto reply = protocol::decode_user_spot_close_reply (cached->header.value ());
                reply.header.correlation = request.correlation;
                (void) _transport->reply_user_spot_close (mailbox_record, reply);
                continue;
            }
            if (admission == admission_t::expired) {
                protocol::user_spot_close_reply_t reply{{request.correlation, 101, 0}, false};
                (void) _transport->reply_user_spot_close (mailbox_record, reply);
                continue;
            }
            auto terminal = [this, transport = _transport,
                             reply_source = mailbox_record.reply_source (), operation_key,
                             correlation = request.correlation] (std::uint32_t terminal_result,
                                                                 std::uint32_t failure_code,
                                                                 bool closed) {
                std::vector<std::pair<std::shared_ptr<const mesh::service_mailbox_reply_target_t>,
                                      std::uint64_t>>
                  waiting;
                _user_spot_terminal_lane.run_sync ([&] {
                    auto &stored = _user_spot_terminals[operation_key];
                    stored.header = result_t<std::vector<std::uint8_t>>::success (
                      protocol::encode_user_spot_close_reply (correlation, terminal_result,
                                                              failure_code, closed));
                    waiting = std::exchange (stored.waiting, {});
                });
                waiting.emplace (waiting.begin (), reply_source, correlation);
                for (const auto &[record, record_correlation] : waiting) {
                    protocol::user_spot_close_reply_t reply{
                      {record_correlation, terminal_result, failure_code}, closed};
                    (void) transport->reply_user_spot_close (*record, reply);
                }
            };
            if (!store) {
                terminal (
                  105, static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed),
                  false);
                continue;
            }
            // The owner runtime checks the authority and runs every Close
            // step on the Spot lifecycle lane (Spot address messaging §7.1).
            auto begin = [this, target = request.target] { return begin_user_spot_close (target); };
            auto done = [terminal] (result_t<bool> result) {
                if (result) {
                    terminal (0, 0, result.value ());
                    return;
                }
                const auto failure =
                  messaging::request_failure_mapper_t{}.target_failure_reply (result.exception ());
                terminal (failure->terminal_result, failure->failure_code, false);
            };
            // The started operation settles its terminal record once, also
            // when the Close cannot be scheduled.
            try {
                if (user_spot_closer)
                    user_spot_closer (
                      request.target.spot_id, request.target.object_generation,
                      messaging::request_deadline_utc_from_unix_ms (request.deadline_unix_ms),
                      begin, done);
                else
                    run_authority_close (begin, done, {});
            }
            catch (...) {
                done (detail::current_exception_result<bool> ());
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
                    static_cast<std::uint32_t> (protocol::request_terminal_result::internalError),
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
                  messaging::request_failure_mapper_t{}.target_failure_reply (*cleanup.error ());
                if (failure)
                    dispatch_failure = {failure->terminal_result, failure->failure_code};
            }
            if (mailbox_record.reply_token () && mailbox_record.correlation)
                (void) _transport->reply_failure (mailbox_record, dispatch_failure->first,
                                                  dispatch_failure->second);
        }
    }
    co_return std::pair{count, infrastructure_budget};
}

task_t<std::size_t> public_host_runtime_t::dispatch_user_spot_operations (
  std::shared_ptr<mesh::service_mailbox_claim_t> &&first_claim,
  receive_batch_budget_t infrastructure_budget)
{
    // task_t starts synchronously. Own the claim before any suspension; the
    // reference parameter is never read again after this transfer.
    static_assert (
      std::is_nothrow_move_constructible_v<std::shared_ptr<mesh::service_mailbox_claim_t>>);
    auto claim = std::move (first_claim);
    std::shared_ptr<zlink::framework::location_repository_t> store;
    user_spot_materializer_t materializer;
    user_spot_closer_t user_spot_closer;
    actor_destroy_target_t actor_destroy_target;
    actor_create_operation_target_t actor_create_target;
    actor_join_operation_target_t actor_join_target;
    std::function<std::optional<location_owner_token_t> ()> session_route_owner_resolver;
    std::function<void (const protocol::message_follow_notice_t &)> message_follow_handler;
    bound_session_operations_t bound_session_operations;
    std::function<void (const protocol::session_relocation_route_t &)>
      late_session_route_update_reporter;
    co_await _lifecycle_configuration_lane.run_task ([&] {
        store = _user_spot_store;
        materializer = _user_spot_materializer;
        user_spot_closer = _user_spot_closer;
        actor_destroy_target = _actor_destroy_target;
        actor_create_target = _actor_create_target;
        actor_join_target = _actor_join_target;
        session_route_owner_resolver = _session_route_owner_resolver;
        message_follow_handler = _message_follow_handler;
        bound_session_operations = _bound_session_operations;
        late_session_route_update_reporter = _late_session_route_update_reporter;
    });
    std::size_t count = 0;
    // Record-processing storage belongs to a claimed infrastructure batch.
    const auto dispatch_batch = [&] (const std::shared_ptr<mesh::service_mailbox_claim_t> &claim)
      -> task_t<std::pair<std::size_t, receive_batch_budget_t>> {
        return dispatch_user_spot_batch (
          store, materializer, user_spot_closer, actor_destroy_target, actor_create_target,
          actor_join_target, session_route_owner_resolver, message_follow_handler,
          bound_session_operations, late_session_route_update_reporter, infrastructure_budget,
          count, claim);
    };
    do {
        {
            const auto release_claim = [this] (mesh::service_mailbox_claim_t *owned) {
                (void) _transport->mailbox ().release (*owned);
            };
            const std::unique_ptr<mesh::service_mailbox_claim_t, decltype (release_claim)>
              claim_owner (claim.get (), release_claim);
            std::tie (count, infrastructure_budget) = co_await dispatch_batch (claim);
        }
        if (infrastructure_budget.exhausted ())
            break;
        claim.reset ();
    } while ((claim = _transport->mailbox ().try_claim (
                mesh::service_mailbox_domain_t::infrastructure,
                dispatch_limits::receive_batch_messages, dispatch_limits::receive_batch_bytes)));
    co_return count;
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

actor_ref_t public_host_runtime_t::framework_actor_ref (const stateful::object_ref_t &object,
                                                        std::string actor_type) const
{
    return ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (object.node_id), std::move (actor_type), object.key,
      object.object_generation);
}

} // namespace zlink::framework::runtime::host
