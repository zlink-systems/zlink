/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/stateful/public_host_runtime_internal.hpp"

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/mesh/service_channel_membership.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/spots/activation_admission.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <exception>
#include <functional>
#include <iostream>
#include <runtime/locations/location_repository.hpp>
#include <service_wire_codec.hpp>
#include <service_wire_constants.hpp>
#include <service_wire_pilot_codec.hpp>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <zlink/framework/contracts/spots/spot_identity.hpp>

namespace zlink::framework::runtime::host
{

namespace public_host_runtime_detail
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
        case stateful::stateful_error_t::none:
            return {static_cast<std::uint32_t> (protocol::request_terminal_result::internalError),
                    static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed)};
        case stateful::stateful_error_t::unavailable:
            // Unavailable's representative code (error model §5); schema pins it to internalError.
            return {static_cast<std::uint32_t> (protocol::request_terminal_result::internalError),
                    static_cast<std::uint32_t> (protocol::framework_error_code::routeNotConnected)};
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

void trace_mesh_host_enabled (std::string_view stage, std::string_view detail)
{
    std::cerr << "zlink mesh-host stage=" << stage << " " << detail << '\n';
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
} // namespace public_host_runtime_detail

using public_host_runtime_detail::is_actor_application;
using public_host_runtime_detail::is_stateful_application;
using public_host_runtime_detail::local_application_query;
using public_host_runtime_detail::trace_mesh_host_enabled;
using ::zlink::framework::detail::mesh_trace_enabled;

namespace
{
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
                event.packet_name = application.packet_name ();
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

    void reset (std::size_t size)
    {
        for (auto &entry : entries) {
            entry.original.reset ();
            entry.retained.store (false, std::memory_order_relaxed);
        }
        entries = std::span<entry_t> (storage.data (), size);
        index = count = handed_off = 0;
    }

    void take_ordinary (const std::shared_ptr<mesh::service_mailbox_claim_t> &claim,
                        bool stateful) noexcept
    {
        // These owners can enter a relocation cut even after this claim is made.
        const auto kind = mesh::service_mailbox_t::application_owner_kind (claim->owner);
        if (stateful && (kind == owner_kind_t::actor || kind == owner_kind_t::spot))
            return;
        for (std::size_t index = 0; index < claim->claimed_messages; ++index)
            entries[index].original.emplace (std::move (*claim->records[index]));
    }

    std::array<entry_t, dispatch_limits::receive_batch_messages> storage;
    std::span<entry_t> entries;
    std::shared_ptr<mesh::service_mailbox_claim_t> claim;
    mesh::service_mailbox_t::application_dispatch_t dispatch;
    std::function<void ()> rejected;
    std::size_t index = 0;
    std::size_t count = 0;
    std::size_t handed_off = 0;

    void handoff (const ready_record_t &ready,
                  const receive_record_t &record,
                  std::vector<zlink::message_t> parts,
                  application_job_queue_t::permit_t permit)
    {
        dispatch (ready, record, std::move (parts), std::move (permit));
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
        // Local reply callbacks do not alias their moved application originals.
        // Raw replies retain routing storage, after source admission is consumed.
        for (auto &entry : entries) {
            if (!entry.original)
                continue;
            if (entry.original->application)
                entry.original.reset ();
            else
                entry.original->source_original.reset ();
        }
        dispatch = {};
        rejected = {};
        claim.reset ();
        return count;
    }
};

namespace
{
std::shared_ptr<application_claim_batch_t> &application_claim_storage ()
{
    static thread_local std::shared_ptr<application_claim_batch_t> storage;
    return storage;
}

std::shared_ptr<application_claim_batch_t> acquire_application_claim_batch (std::size_t size)
{
    assert (size <= dispatch_limits::receive_batch_messages);
    auto &storage = application_claim_storage ();
    // Aliased originals and suspended handlers keep their existing batch alive.
    if (!storage || storage.use_count () != 1)
        storage = std::make_shared<application_claim_batch_t> ();
    storage->reset (size);
    return storage;
}

void recycle_application_claim_batch (const std::shared_ptr<application_claim_batch_t> &batch)
{
    auto &storage = application_claim_storage ();
    // A nested dispatch may have replaced this worker's current storage.
    if (storage != batch)
        return;
    if (batch.use_count () == 2)
        batch->reset (0);
    else
        storage.reset ();
}
} // namespace

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
          return _spot_actor_index_lane.run_sync ([&] {
              const auto found = _actors.find (actor_id);
              return found == _actors.end () ? std::optional<stateful::object_ref_t>{}
                                             : std::make_optional (found->second.second);
          });
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
    _relocation_metrics =
      std::make_shared<runtime::relocation_metrics_t> (_options.mesh.metric_provider);
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
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started || _stateful_dispatch)
            throw std::logic_error ("stateful dispatch must be configured once before host start");
        if (sources || _local_lease_observation)
            _transport->configure_request_sources (sources, _local_lease_observation);
        _stateful_dispatch = std::make_unique<stateful::raw_stateful_dispatch_t> (
          _objects, *_transport,
          [this, resolver =
                   std::move (resolver)] (const stateful::accepted_record_authority_query_t &query)
            -> task_t<std::optional<stateful::accepted_record_authority_t>> {
              using authority_t = std::optional<stateful::accepted_record_authority_t>;
              try {
                  auto accepted =
                    _relocation_session_terminal_lane.run_task ([&] () -> authority_t {
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
                  // resolve_accepted_authority owns query until this task completes;
                  // the configured dispatch owns resolver for the same lifetime.
                  return continue_task<authority_t> (
                    std::move (accepted), [&resolver, &query] (authority_t authority) {
                        if (authority)
                            return task_t<authority_t> (
                              result_t<authority_t>::success (std::move (authority)));
                        return resolver (query);
                    });
              }
              catch (...) {
                  return task_t<authority_t> (detail::current_exception_result<authority_t> ());
              }
          });
        /* flow-correlation §4: the ingest path gates flow capture on the host's
           * provider; the host outlives the dispatch it owns. */
        _stateful_dispatch->set_flow_capture_provider ([this] { return capture_flow (); });
    });
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
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started || _stateful_dispatch)
            throw std::logic_error ("stateful dispatch must be configured once before host start");
        _local_lease_observation = std::move (owner);
    });
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

void public_host_runtime_t::configure_bound_session_operations (
  bound_session_operations_t operations)
{
    if (!operations.bind || !operations.send || !operations.replaced
        || !operations.commit_relocation_route || !operations.prepare_relocation_target_route)
        throw std::invalid_argument ("bound Session operations must all be configured");
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started)
            throw std::logic_error ("bound Session operations must be configured before start");
        _bound_session_operations = std::move (operations);
    });
}

void public_host_runtime_t::start ()
{
    auto population_metrics = _lifecycle_configuration_lane.run_sync ([&] {
        if (_started || _closing) {
            return task_t<void> (result_t<void>::success ());
        }
        _instance_activation_metrics.connect (_options.mesh.metric_provider);
        auto configured = _objects.configure_population_metrics (_options.mesh.descriptor.mesh_name,
                                                                 _options.mesh.metric_provider);
        auto owner = std::make_unique<completion_owner_t> ();
        _transport->start ();
        _completion_owner = std::move (owner);
        _started = true;
        return configured;
    });
    detail::task_result_access_t::read (population_metrics).value ();
}

void public_host_runtime_t::close () noexcept
{
    bool closing_started = false;
    std::function<void ()> maintenance_closing;
    {
        _lifecycle_configuration_lane.run_sync ([&] {
            if (!_started || _closing) {
                return;
            }
            _closing = true;
            closing_started = true;
            maintenance_closing = _maintenance_closing;
        });
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
        detail::task_result_access_t::read (target_terminal).value ();
    _lifecycle_configuration_lane.run_sync ([&] { _started = false; });
    _relocation_session_terminal_lane.run_sync ([&] {
        _session_seal_terminals.clear ();
        _session_journal_terminals.clear ();
    });
    _sessions.drop_held_relays (message_flow_reason_t::shutdown);
    _transport->close ();
    (void) _objects.retire_population_metrics_async ();
    _local_dispatch_completion_lane.run_sync ([&] { _local_application_dispatches.clear (); });
    _lifecycle_configuration_lane.run_sync ([&] {
        _completion_owner.reset ();
        _closing = false;
    });
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
    return _transport->connect_peer_endpoint (endpoint, std::move (expected),
                                              expected_lifecycle_generation,
                                              std::move (security_identity), automatic);
}

void public_host_runtime_t::expect_peer (const std::string &endpoint,
                                         const zlink::routing_id_t &expected,
                                         std::uint64_t expected_lifecycle_generation,
                                         std::string security_identity)
{
    _transport->expect_peer (expected_peer_descriptor (
      endpoint, expected, expected_lifecycle_generation, std::move (security_identity)));
}

void public_host_runtime_t::forget_peer (const std::string &endpoint,
                                         const zlink::routing_id_t &expected)
{
    _transport->forget_peer (expected.to_bytes (), endpoint);
}

task_t<void> public_host_runtime_t::expect_peer_async (std::string endpoint,
                                                       zlink::routing_id_t expected,
                                                       std::uint64_t expected_lifecycle_generation,
                                                       std::string security_identity)
{
    return _transport->expect_peer_async (expected_peer_descriptor (
      endpoint, expected, expected_lifecycle_generation, std::move (security_identity)));
}

task_t<void> public_host_runtime_t::forget_peer_async (std::string endpoint,
                                                       zlink::routing_id_t expected)
{
    return _transport->forget_peer_async (expected.to_bytes (), std::move (endpoint));
}

mesh::service_node_descriptor_t
public_host_runtime_t::expected_peer_descriptor (const std::string &endpoint,
                                                 const zlink::routing_id_t &expected,
                                                 std::uint64_t expected_lifecycle_generation,
                                                 std::string security_identity) const
{
    auto descriptor = _options.mesh.descriptor;
    descriptor.node_routing_id = expected.to_bytes ();
    descriptor.advertised_endpoint = endpoint;
    descriptor.lifecycle_generation = expected_lifecycle_generation;
    descriptor.security_identity = std::move (security_identity);
    return descriptor;
}

void public_host_runtime_t::disconnect_peer (const std::string &endpoint) noexcept
{
    _transport->disconnect_peer_endpoint (endpoint);
}

bool public_host_runtime_t::disconnect_peer (const std::vector<std::uint8_t> &expected_routing_id,
                                             const std::string &endpoint) noexcept
{
    return _transport->disconnect_peer_endpoint (expected_routing_id, endpoint);
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
           + _relocation_session_terminal_lane.run_sync ([this] {
                 return std::count_if (
                   _relocation_target_attempts.begin (), _relocation_target_attempts.end (),
                   [] (const auto &item) { return !relocation_target_terminal (item.second); });
             });
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

stateful::stateful_object_runtime_t &public_host_runtime_t::objects () noexcept
{
    return _objects;
}

stateful::stream_session_registry_t &public_host_runtime_t::sessions () noexcept
{
    return _sessions;
}

task_t<bool> public_host_runtime_t::dispatch_bound_session_send (
  const mesh::service_mailbox_record_t &mailbox_record)
{
    if (mailbox_record.parts ().size () != 2)
        co_return false;
    const auto record = protocol::decode_bound_session_send (
      protocol::message_bytes (mailbox_record.parts ().front ()));
    if (mailbox_record.source_routing_id != record.actor.target_node_routing_id
        || mailbox_record.source_node_generation == 0
        || mailbox_record.source_node_generation != record.actor.target_node_generation)
        co_return false;
    bound_session_operations_t operations;
    _lifecycle_configuration_lane.run_sync ([&] { operations = _bound_session_operations; });
    if (!operations.capture_send && !operations.send)
        co_return false;
    const auto application = protocol::decode_application_payload (
      protocol::message_bytes (mailbox_record.parts ().back ()), capture_flow ());
    auto parts = protocol::decode_application_parts (application);
    bound_session_operations_t::delivery_capability_t delivery;
    std::optional<task_t<stateful::stateful_error_t>> submitted;
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
                  return task_t<stateful::stateful_error_t> (
                    result_t<stateful::stateful_error_t>::success (
                      operations.send (record, std::move (payload))));
              };
          }
          submitted.emplace (delivery (std::move (parts)));
          return true;
      });
    if (admitted != stateful::stateful_error_t::none)
        co_return false;
    co_return (co_await *submitted) == stateful::stateful_error_t::none;
}

task_t<std::size_t>
public_host_runtime_t::dispatch_ready (mesh::service_mailbox_t::application_dispatch_t dispatch,
                                       bool accept_application_receive,
                                       std::function<bool ()> next_application_receive,
                                       std::optional<zlink::poll_event_flag_t> waited)
{
    return dispatch_ready_impl (std::move (dispatch), accept_application_receive,
                                std::move (next_application_receive), waited,
                                !_transport->mailbox ().has_application_dispatch ());
}

task_t<std::size_t> public_host_runtime_t::dispatch_hosted_ready (
  mesh::service_mailbox_t::application_dispatch_t dispatch,
  bool accept_application_receive,
  std::function<bool ()> next_application_receive,
  std::optional<zlink::poll_event_flag_t> waited)
{
    return dispatch_ready_impl (std::move (dispatch), accept_application_receive,
                                std::move (next_application_receive), waited, false);
}

task_t<std::size_t> public_host_runtime_t::dispatch_ready_impl (
  mesh::service_mailbox_t::application_dispatch_t dispatch,
  bool accept_application_receive,
  std::function<bool ()> next_application_receive,
  std::optional<zlink::poll_event_flag_t> waited,
  bool standalone)
{
    if (!dispatch)
        throw std::invalid_argument ("framework public host dispatch callback is required");
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    (void) co_await _relocation_wire->retry_terminal_relays (now);
    (void) _relocation_wire->reap_terminal_tombstones (now);
    (void) _transport->tick_liveness (now);
    std::size_t count = 0;
    count += co_await dispatch_user_spot_operations ();

    std::vector<stateful::object_ref_t> stateful_owners;
    if (accept_application_receive && _stateful_dispatch && standalone) {
        auto local_node_id =
          zlink::routing_id_t::from (_transport->topology ().local_descriptor ().node_routing_id)
            .to_string ();
        stateful_owners =
          co_await _objects.application_owner_refs_async (std::move (local_node_id));
    }
    auto next_stateful_owner = stateful_owners.begin ();
    receive_batch_budget_t budget;
    // One poll per receive turn; the turn then reads records until the ROUTER
    // reports none or the budget ends (08-messaging-hot-path I2).
    const auto readiness = _transport->begin_receive_turn (waited);
    if (readiness
        && (static_cast<short> (*readiness)
            & static_cast<short> (zlink::poll_event_flag_t::pollcompletion))
             != 0)
        ++count;
    const bool receive_ready =
      readiness && accept_application_receive
      && (static_cast<short> (*readiness) & static_cast<short> (zlink::poll_event_flag_t::pollin))
           != 0;
    while (budget.can_receive ()) {
        const auto pumped = !readiness ? mesh::raw_mesh_pump_result_t::protocol_error
                            : receive_ready
                              ? _transport->receive_next (
                                  readiness, mesh::service_liveness_registry_t::clock_t::now ())
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
                mesh::service_mailbox_application_t pending;
                _local_dispatch_completion_lane.run_sync ([&] {
                    if (_local_application_dispatches.empty ())
                        return;
                    if (!accept_application_receive
                        && !_local_application_dispatches.front ()
                              ->record.before_application_handler
                        && !_local_application_dispatches.front ()->application_permit)
                        return;
                    pending = std::move (_local_application_dispatches.front ());
                    _local_application_dispatches.pop_front ();
                });
                if (!pending)
                    break;
                // Accepted request payloads outlive their caller. Join admission
                // controls still require their operation's terminal claim.
                if (pending->record.kind == record_kind_t::spot_control
                    && !_transport->operation_pending (pending->record.operation_id))
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
                dispatch (pending->owner, pending->record, std::move (pending->parts),
                          std::move (pending->application_permit));
                ++count;
                application_dispatch_started = true;
                break;
            }
        }

        if (accept_application_receive && !application_dispatch_started && _stateful_dispatch) {
            while (!application_dispatch_started && next_stateful_owner != stateful_owners.end ()) {
                const auto &owner = *next_stateful_owner++;
                application_dispatch_started = co_await dispatch_stateful_owner (owner, dispatch);
                if (application_dispatch_started)
                    ++count;
            }
        }

        while (accept_application_receive && !application_dispatch_started && standalone) {
            std::shared_ptr<application_claim_batch_t> batch;
            auto claim = _transport->mailbox ().try_claim (
              mesh::service_mailbox_domain_t::application, 1, dispatch_limits::receive_batch_bytes,
              [&] (const auto &claimed) {
                  batch = acquire_application_claim_batch (claimed->claimed_messages);
                  batch->take_ordinary (claimed, static_cast<bool> (_stateful_dispatch));
              });
            if (!claim)
                break;
            batch->claim = std::move (claim);
            batch->dispatch = dispatch;
            auto result = dispatch_application_claim (batch);
            std::size_t dispatched;
            if (const auto ready = std::get_if<std::size_t> (&result)) {
                dispatched = *ready;
                recycle_application_claim_batch (batch);
            } else {
                recycle_application_claim_batch (batch);
                dispatched = co_await std::get<task_t<std::size_t>> (std::move (result));
            }
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
    try {
        if (object.kind != stateful::object_kind_t::actor)
            return task_t<bool> (result_t<bool>::success (false));
        return _transport->mailbox ().notify_stateful_ready (
          object.key, [weak = weak_from_this (), object] (const auto &dispatch) {
              const auto host = weak.lock ();
              return host && host->claim_stateful_owner (object, dispatch);
          });
    }
    catch (...) {
        return task_t<bool> (detail::current_exception_result<bool> ());
    }
}

task_t<bool> public_host_runtime_t::dispatch_stateful_owner (
  const stateful::object_ref_t &object,
  const mesh::service_mailbox_t::application_dispatch_t &dispatch,
  std::shared_ptr<runtime::serial_execution_queue_t> gate,
  std::shared_ptr<stateful::relocation_seal_binding_t> restoring)
{
    if (object.kind == stateful::object_kind_t::actor) {
        // A ready notification keeps the synchronous owner claim on this stack (#2418).
        try {
            return continue_task<bool> (
              notify_stateful_owner (object), [this, &object, &dispatch] (bool notified) {
                  return task_t<bool> (
                    result_t<bool>::success (notified || claim_stateful_owner (object, dispatch)));
              });
        }
        catch (...) {
            return task_t<bool> (detail::current_exception_result<bool> ());
        }
    }
    // The gate admission that observed a restoring seal (native publication or the
    // aborted cut's finish) hands it here; an owner claim without one asks the gate
    // nothing, and a ready claim finishes on this stack (#2418).
    using claim_t = stateful::raw_stateful_dispatch_t::owner_delivery_t;
    const auto deliver = [this, &object, &dispatch] (claim_t &claim) {
        return claim.error == stateful::stateful_error_t::none && claim.delivery
               && dispatch_stateful_delivery (object, std::move (*claim.delivery),
                                              std::move (claim.before_application_handler),
                                              std::move (claim.stable_type), dispatch);
    };
    try {
        if (!restoring)
            return continue_task<bool> (
              _stateful_dispatch->try_claim_async (object), [deliver] (claim_t claim) {
                  return task_t<bool> (result_t<bool>::success (deliver (claim)));
              });
    }
    catch (...) {
        return task_t<bool> (detail::current_exception_result<bool> ());
    }
    // Restoring: the prefix enters the FIFO before the seal is released.
    return [] (public_host_runtime_t *self, const stateful::object_ref_t &object,
               decltype (deliver) deliver, std::shared_ptr<runtime::serial_execution_queue_t> gate,
               std::shared_ptr<stateful::relocation_seal_binding_t> restoring) -> task_t<bool> {
        bool dispatched = false;
        for (;;) {
            auto claim = co_await self->_stateful_dispatch->try_claim_async (object);
            if (claim.error != stateful::stateful_error_t::none || !claim.delivery) {
                (void) co_await gate->release_relocation_seal (std::move (restoring));
                co_return dispatched;
            }
            dispatched = deliver (claim) || dispatched;
            // Publication import returns the native claim before the next owner claim.
            // New root handlers remain behind this same seal until the prefix is in the FIFO.
            (void) co_await gate->admit_native_application ({});
        }
    }(this, object, deliver, std::move (gate), std::move (restoring));
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
                previous->admission.reservation
                  .emplace<serial_application_admission_t::initial_t> ();
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
        dispatch (owner, record, std::move (parts), {});
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
  const stateful::object_ref_t &object,
  mesh::service_mailbox_t::claim_handle_t slot,
  stateful::claimed_ingress_t &&query,
  std::shared_ptr<const local_application_dispatch_t> application,
  std::optional<stateful::retained_relocation_source_t> retained,
  const mesh::service_mailbox_t::application_dispatch_t &dispatch)
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
    auto [error, authority] = co_await _stateful_dispatch->accepted_spot_authority (
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
        auto application_permit = original->application_permit
                                    ? std::move (*original->application_permit)
                                    : application_job_queue_t::permit_t{};
        ready_record_t owner;
        receive_record_t record;
        if (application) {
            parts = std::move (original->application->parts);
            owner = std::move (original->application->owner);
            record = std::move (original->application->record);
        } else {
            owner.owner_kind = owner_kind_t::spot;
            owner.domain = ready_domain_t::application;
            owner.spot_id = object.key;
            record.domain = ready_domain_t::application;
            record.kind =
              header.correlation ? record_kind_t::spot_request : record_kind_t::spot_send;
            record.operation_kind = operation_kind (record.kind);
            record.source_node_rid = original->source_rid ();
            record.operation_id = {header.operation.high, header.operation.low};
            record.reply_route_id = header.correlation.value_or (0);
            original->replace_parts ({});
            if (header.correlation)
                record.reply_token = {
                  weak_from_this (),
                  std::make_shared<mesh::service_mailbox_record_t> (std::move (*original))};
        }
        record.spot_route = header.target;
        record.before_application_handler = std::move (before);
        record.transferred_owner_byte_cost = slot.claim->records[slot.index].byte_cost;
        auto accepted = std::make_shared<runtime::serial_application_record_t> ();
        accepted->target = std::move (target);
        accepted->admission.jobs = _options.mesh.application_jobs;
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
        accepted->set_spot_identity ({std::move (header), std::move (authority->source)},
                                     parts.front ().copy ());
        record.retain_mailbox_reservation = [] {};
        accepted->transferred = std::exchange (credit, {});
        record.release_mailbox_reservation = [accepted] { accepted->release_native_claim (); };
        try {
            dispatch (owner, record, std::move (parts), std::move (application_permit));
        }
        catch (...) {
            accepted->release_native_claim ();
            throw;
        }
    };
    if (gate) {
        auto restoring = co_await gate->admit_native_application (publish);
        while (restoring) {
            (void) co_await dispatch_stateful_owner (object, dispatch, gate, std::move (restoring));
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
            batch = acquire_application_claim_batch (claim->claimed_messages);
        batch->claim = std::move (claim);
        batch->dispatch = std::move (dispatch);
        auto result = dispatch_application_claim (batch);
        if (const auto count = std::get_if<std::size_t> (&result)) {
            const auto dispatched = *count;
            recycle_application_claim_batch (batch);
            return task_t<std::size_t> (result_t<std::size_t>::success (dispatched));
        }
        recycle_application_claim_batch (batch);
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
                            return kept_t{kept_local_t{original.application.retain (),
                                                       original.source_original}};
                        }
                        const auto kind = original.application_kind ();
                        if (!kind || !is_stateful_application (*kind))
                            return std::nullopt;
                        return kept_t{
                          stateful::claimed_ingress_t::prepare (original, sources, *mesh_name)};
                    });
            if (auto *kept = std::get_if<kept_t> (&step)) {
                // Only stateful ingress needs a suspended cursor. The canonical
                // slot remains the original owner throughout authority preparation.
                // Each kept form suspends in its own continuation, which holds only
                // that form's locals and then continues this claim's cursor.
                using dispatch_t = mesh::service_mailbox_t::application_dispatch_t;
                const auto local_fallback = [] (std::shared_ptr<public_host_runtime_t> self,
                                                std::shared_ptr<application_claim_batch_t> batch,
                                                kept_local_t local) -> task_t<void> {
                    const mesh::service_mailbox_t::claim_handle_t slot{batch->claim, batch->index};
                    auto &entry = batch->entries[batch->index];
                    const dispatch_t dispatch = [batch] (const auto &ready, const auto &record,
                                                         auto parts, auto permit) {
                        batch->handoff (ready, record, std::move (parts), std::move (permit));
                    };
                    auto query = local_application_query (*local.application);
                    const auto object = co_await self->accept_local_application (
                      slot, std::move (query), std::move (local.application),
                      std::move (local.source_original), entry.original);
                    if (object) {
                        (void) co_await self->dispatch_stateful_owner (*object, dispatch);
                        ++batch->count;
                    }
                };
                const auto local_owner =
                  [] (std::shared_ptr<public_host_runtime_t> self,
                      std::shared_ptr<application_claim_batch_t> batch, kept_local_t local,
                      decltype (local_fallback) fallback) -> task_t<std::size_t> {
                    std::exception_ptr failure;
                    try {
                        bool native = false;
                        if (!is_actor_application (local.application->record.kind)) {
                            const mesh::service_mailbox_t::claim_handle_t slot{batch->claim,
                                                                               batch->index};
                            const dispatch_t dispatch = [batch] (const auto &ready,
                                                                 const auto &record, auto parts,
                                                                 auto permit) {
                                batch->handoff (ready, record, std::move (parts),
                                                std::move (permit));
                            };
                            std::optional<stateful::retained_relocation_source_t> retained;
                            const auto owner = co_await self->_objects.resolve_spot_async (
                              local.application->owner.spot_id, &retained);
                            if (owner) {
                                stateful::claimed_ingress_t query;
                                query.source_original = local.source_original;
                                query.source_routing_id =
                                  local.application->record.source_node_rid.to_bytes ();
                                query.source_node_generation =
                                  self->_options.mesh.descriptor.lifecycle_generation;
                                native = co_await self->dispatch_native_spot (
                                  *owner, slot, std::move (query), local.application,
                                  std::move (retained), dispatch);
                            }
                        }
                        if (native)
                            ++batch->count;
                        else
                            co_await fallback (self, batch, std::move (local));
                        ++batch->index;
                    }
                    catch (...) {
                        failure = std::current_exception ();
                    }
                    co_return co_await self->continue_application_claim (batch,
                                                                         std::move (failure));
                };
                const auto ingress_owner =
                  [] (std::shared_ptr<public_host_runtime_t> self,
                      std::shared_ptr<application_claim_batch_t> batch,
                      stateful::claimed_ingress_t query) -> task_t<std::size_t> {
                    std::exception_ptr failure;
                    try {
                        auto &claim = batch->claim;
                        const auto index = batch->index;
                        const mesh::service_mailbox_t::claim_handle_t slot{claim, index};
                        auto &entry = batch->entries[index];
                        const dispatch_t dispatch = [batch] (const auto &ready, const auto &record,
                                                             auto parts, auto permit) {
                            batch->handoff (ready, record, std::move (parts), std::move (permit));
                        };
                        std::optional<stateful::object_ref_t> native_owner;
                        std::optional<stateful::retained_relocation_source_t> retained;
                        const auto owner_kind =
                          mesh::service_mailbox_t::application_owner_kind (claim->owner);
                        if (owner_kind == owner_kind_t::actor)
                            native_owner = co_await self->_objects.find_async (
                              stateful::object_kind_t::actor,
                              claim->owner.substr (
                                detail::activation_admission_t::actor_key_prefix.size ()),
                              std::nullopt, &retained);
                        else if (owner_kind == owner_kind_t::spot) {
                            const auto key = claim->owner.substr (
                              detail::activation_admission_t::spot_key_prefix.size ());
                            native_owner =
                              co_await self->_objects.resolve_spot_async (key, &retained);
                        }
                        bool native = false;
                        if (native_owner) {
                            std::optional<stateful::claimed_ingress_t> prepared;
                            if (native_owner->kind != stateful::object_kind_t::actor) {
                                native = co_await self->dispatch_native_spot (
                                  *native_owner, slot, std::move (query), {}, retained, dispatch);
                                // A cut or rejection keeps the original; only this cold
                                // ingress prepares its immutable query again from that slot.
                            } else {
                                prepared.emplace (std::move (query));
                            }
                            if (!native) {
                                const auto accepted =
                                  co_await self->_stateful_dispatch->ingest_claimed (
                                    *native_owner, slot, std::move (retained), std::move (prepared),
                                    &*native_owner);
                                if (accepted == stateful::stateful_error_t::none)
                                    (void) co_await self->dispatch_stateful_owner (*native_owner,
                                                                                   dispatch);
                                self->_transport->signal_activity ();
                            }
                            ++batch->count;
                            ++batch->index;
                        } else {
                            entry.original = self->_transport->mailbox ().take (slot);
                            if (!entry.original)
                                ++batch->index;
                        }
                    }
                    catch (...) {
                        failure = std::current_exception ();
                    }
                    co_return co_await self->continue_application_claim (batch,
                                                                         std::move (failure));
                };
                if (auto *local = std::get_if<kept_local_t> (kept))
                    return local_owner (shared_from_this (), batch, std::move (*local),
                                        local_fallback);
                return ingress_owner (shared_from_this (), batch,
                                      std::get<stateful::claimed_ingress_t> (std::move (*kept)));
            }
            if (auto *taken = std::get_if<mesh::service_mailbox_record_t> (&step))
                entry.original.emplace (std::move (*taken));
            if (!entry.original)
                continue;
            auto &mailbox_record = *entry.original;
            release_mailbox_reservation = std::exchange (mailbox_record.transferred, {});
            const host::mailbox_callback_t retain_mailbox_reservation (
              std::shared_ptr<std::atomic_bool> (batch, &entry.retained));
            if (mailbox_record.application) {
                auto application = std::move (*mailbox_record.application);
                application.record.before_application_handler =
                  mailbox_record.before_application_handler;
                application.record.release_mailbox_reservation = release_mailbox_reservation;
                application.record.retain_mailbox_reservation = retain_mailbox_reservation;
                application.record.transferred_owner_byte_cost = claim->records[index].byte_cost;
                batch->handoff (
                  application.owner, application.record, std::move (application.parts),
                  mailbox_record.application_permit ? std::move (*mailbox_record.application_permit)
                                                    : std::move (application.application_permit));
                ++batch->count;
                if (!entry.retained.load (std::memory_order_acquire))
                    release_mailbox_reservation ();
                continue;
            }
            if (mailbox_record.parts ().size () < 2)
                throw protocol::service_wire_error_t (
                  "application wire requires a header and payload");
            const auto wire = mailbox_record.service_header ();
            if (wire.kind == protocol::command::logicalMulticast) {
                namespace generated = protocol::generated::detail;
                generated::logicalMulticast_23_t multicast;
                generated::reader_t reader (
                  protocol::message_bytes (mailbox_record.parts ().front ()));
                if (generated::decode_head_logicalMulticast_23 (reader, multicast)
                    != generated::error_code::ok)
                    throw protocol::service_wire_error_t ("invalid logical multicast frame");
                auto targets = _objects.multicast_local_targets (multicast.channelName.value,
                                                                 multicast.topic.value);
                auto metadata =
                  (multicast.flags & static_cast<std::uint8_t> (protocol::flag::metadata)) == 0
                    ? std::shared_ptr<const std::vector<std::uint8_t>>{}
                    : std::make_shared<const std::vector<std::uint8_t>> (
                        mailbox_record.parts ()[1].to_bytes ());
                auto parts = protocol::decode_application_parts (
                  std::move (mailbox_record.body_parts ().back ()));
                auto before = std::move (mailbox_record.before_application_handler);
                auto pending = std::make_shared<task_t<void>> (enqueue_local_multicast (
                  std::move (targets), std::move (multicast.channelName.value),
                  std::move (multicast.topic.value), std::move (parts), std::move (metadata),
                  mailbox_record.source_rid (), std::move (before),
                  mailbox_record.application_permit ? std::move (*mailbox_record.application_permit)
                                                    : application_job_queue_t::permit_t{}));
                detail::observe_task_completion (
                  *pending, [self = shared_from_this (), pending] (const result_t<void> &) {});
                ++batch->count;
                if (!entry.retained.load (std::memory_order_acquire))
                    release_mailbox_reservation ();
                continue;
            }
            if (mailbox_record.parts ().size () != 2)
                throw protocol::service_wire_error_t ("application wire requires two parts");
            if (wire.kind == protocol::command::boundSessionSend) {
                // The outbound stream delivery is a transport handoff, not an application handler.
                mailbox_record.before_application_handler = {};
                mailbox_record.application_permit.reset ();
                const auto deliver =
                  [] (std::shared_ptr<public_host_runtime_t> self,
                      std::shared_ptr<application_claim_batch_t> batch, task_t<bool> delivery,
                      mesh::service_mailbox_settlement_t release) -> task_t<std::size_t> {
                    std::exception_ptr failure;
                    try {
                        (void) co_await delivery;
                        ++batch->count;
                        if (!batch->entries[batch->index].retained.load (std::memory_order_acquire))
                            release ();
                        ++batch->index;
                    }
                    catch (...) {
                        failure = std::current_exception ();
                    }
                    co_return co_await self->continue_application_claim (batch,
                                                                         std::move (failure));
                };
                return deliver (shared_from_this (), batch,
                                dispatch_bound_session_send (mailbox_record),
                                std::move (release_mailbox_reservation));
            }
            const auto wire_kind = mailbox_record.application_kind ();
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
            record.source_node_rid = mailbox_record.source_rid ();
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
                owner.channel_name =
                  kind == record_kind_t::channel_send
                    ? protocol::decode_channel_send_header (
                        protocol::message_bytes (mailbox_record.parts ().front ()))
                    : protocol::decode_channel_request_header (
                        protocol::message_bytes (mailbox_record.parts ().front ()))
                        .channel_name;
                record.channel_name = owner.channel_name;
            } else if (kind == record_kind_t::spot_send || kind == record_kind_t::spot_request) {
                owner.owner_kind = owner_kind_t::spot;
                const auto spot = protocol::decode_spot_message_header (
                  protocol::message_bytes (mailbox_record.parts ().front ()), wire.kind);
                owner.spot_id = spot.target.spot_id;
                record.spot_route = spot.target;
            } else if (kind == record_kind_t::actor_send || kind == record_kind_t::actor_request) {
                owner.owner_kind = owner_kind_t::actor;
                const auto actor = protocol::decode_actor_message_header (
                  protocol::message_bytes (mailbox_record.parts ().front ()), wire.kind);
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
            auto parts = protocol::decode_application_parts (
              std::move (mailbox_record.body_parts ()[0]), capture_flow ());
            trace_mesh_host (
              "dispatch",
              std::string ("kind=") + std::to_string (static_cast<int> (kind)) + " source="
                + (mailbox_record.source_routing_id.empty () ? std::string ("-")
                                                             : record.source_node_rid.to_string ())
                + " parts=" + std::to_string (mailbox_record.parts ().size ()));
            record.release_mailbox_reservation = release_mailbox_reservation;
            record.retain_mailbox_reservation = retain_mailbox_reservation;
            record.transferred_owner_byte_cost = claim->records[index].byte_cost;
            batch->handoff (owner, record, std::move (parts),
                            mailbox_record.application_permit
                              ? std::move (*mailbox_record.application_permit)
                              : application_job_queue_t::permit_t{});
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
        const auto failure =
          messaging::request_failure_mapper_t{}.target_failure_reply (*unhandled);
        _options.mesh.runtime_failures->report (*unhandled);
        if (failure)
            rejection = *failure;
    }
    if (!entry.original)
        entry.original = _transport->mailbox ().take (slot);
    if (!entry.original)
        return;
    auto &mailbox_record = *entry.original;
    handler_entry_t terminal (std::exchange (mailbox_record.before_application_handler, {}),
                              mailbox_record.application_permit
                                ? std::move (*mailbox_record.application_permit)
                                : application_job_queue_t::permit_t{});
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
                                                   std::exception_ptr failure)
{
    // The suspended record's continuation calls this; a ready rest of the claim
    // returns its count without another frame.
    try {
        if (failure) {
            fail_application_claim (batch, std::move (failure));
            ++batch->index;
        }
        auto next = dispatch_application_claim (batch);
        if (const auto count = std::get_if<std::size_t> (&next))
            return task_t<std::size_t> (result_t<std::size_t>::success (*count));
        return std::get<task_t<std::size_t>> (std::move (next));
    }
    catch (...) {
        return task_t<std::size_t> (detail::current_exception_result<std::size_t> ());
    }
}

public_host_runtime_t::application_owner_result_t
public_host_runtime_t::dispatch_application_owner (
  std::string owner,
  mesh::service_mailbox_t::application_dispatch_t dispatch,
  std::function<void ()> started,
  std::function<void ()> rejected)
{
    std::shared_ptr<application_claim_batch_t> batch;
    std::size_t claimed = 0;
    const auto transfer = [&] (const auto &claim) {
        claimed = claim->claimed_messages;
        for (std::size_t index = 0; index < claimed; ++index)
            started ();
        batch = acquire_application_claim_batch (claimed);
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
            return false;
        auto result = dispatch_application_claim (batch);
        if (std::holds_alternative<std::size_t> (result)) {
            recycle_application_claim_batch (batch);
            return true;
        }
        recycle_application_claim_batch (batch);
        return [] (task_t<std::size_t> pending) -> task_t<bool> {
            (void) co_await pending;
            co_return true;
        }(std::get<task_t<std::size_t>> (std::move (result)));
    }
    catch (...) {
        if (batch) {
            batch->finish ();
            recycle_application_claim_batch (batch);
        } else
            for (std::size_t index = 0; index < claimed; ++index)
                rejected ();
        return task_t<bool> (detail::current_exception_result<bool> ());
    }
}

std::optional<zlink::poll_event_flag_t> public_host_runtime_t::wait_for_dispatch_activity (
  std::chrono::milliseconds timeout,
  bool accept_application_receive,
  std::optional<std::chrono::steady_clock::time_point> next_activity) noexcept
{
    return wait_for_dispatch_activity_impl (timeout, accept_application_receive, next_activity,
                                            true);
}

std::optional<zlink::poll_event_flag_t> public_host_runtime_t::wait_for_hosted_dispatch_activity (
  std::chrono::milliseconds timeout,
  bool accept_application_receive,
  std::optional<std::chrono::steady_clock::time_point> next_activity) noexcept
{
    return wait_for_dispatch_activity_impl (timeout, accept_application_receive, next_activity,
                                            false);
}

std::optional<zlink::poll_event_flag_t> public_host_runtime_t::wait_for_dispatch_activity_impl (
  std::chrono::milliseconds timeout,
  bool accept_application_receive,
  std::optional<std::chrono::steady_clock::time_point> next_activity,
  bool standalone) noexcept
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
        return standalone
                 ? _transport->wait_for_activity (timeout, accept_application_receive)
                 : _transport->wait_for_hosted_activity (timeout, accept_application_receive);
    }
    catch (...) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        return std::nullopt;
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

void public_host_runtime_t::signal_dispatch_activity () noexcept
{
    _transport->signal_activity ();
}

void public_host_runtime_t::publish_local_application (mesh::service_mailbox_application_t original)
{
    auto &dispatch = *original;
    if (!_stateful_dispatch || !is_stateful_application (dispatch.record.kind)) {
        _local_application_dispatches.push_back (std::move (original));
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
    record.application_permit.emplace (std::move (dispatch.application_permit));
    record.application = std::move (original);
    if (!_transport->mailbox ().try_enqueue (std::move (record)))
        throw framework_exception_t (framework_error_kind_t::rejected,
                                     "local application mailbox is closed");
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
      *owner, std::move (accepted_record), std::move (ingress), original, std::move (terminal),
      authority->committed_target.get ());
    if (!original)
        co_return std::nullopt;
    if (accepted != stateful::stateful_error_t::none)
        throw protocol::service_wire_error_t ("local accepted Actor enqueue failed");
    co_return authority->committed_target ? authority->committed_target->target
                                          : std::move (*owner);
}

task_t<void> public_host_runtime_t::enqueue_local_multicast (
  task_t<std::vector<stateful::object_ref_t>> target_snapshot,
  std::string channel_name,
  std::string topic,
  std::vector<zlink::message_t> parts,
  std::shared_ptr<const std::vector<std::uint8_t>> metadata,
  zlink::routing_id_t source_node,
  std::function<void ()> before_application_handler,
  application_job_queue_t::permit_t application_permit)
{
    auto pending_permit = std::move (application_permit);
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
            local_application_dispatch_t dispatch{std::move (owner), std::move (record), parts};
            dispatch.application_permit = std::move (pending_permit);
            admit_local_application (std::move (dispatch), {}, completion);
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
    if (!_options.mesh.application_jobs || dispatch.record.before_application_handler
        || dispatch.application_permit) {
        publish_local_application (std::move (dispatch));
        if (admitted)
            admitted->complete (result_t<void>::success ());
        return;
    }
    mesh::service_mailbox_application_t original (std::move (dispatch));
    auto application = original.retain_mutable ();
    auto pending = waiter ? waiter : std::make_shared<application_job_queue_t::waiter_t> ();
    std::weak_ptr<public_host_runtime_t> weak = shared_from_this ();
    const auto stop_admission = [] (const auto &completion) {
        if (completion)
            completion->complete (detail::boundary_failure<void> (
              detail::boundary_error_t::shutdown, "local application admission stopped"));
    };
    auto register_supply = waiter ? &application_job_queue_t::register_request_supply
                                  : &application_job_queue_t::register_supply;
    auto registration = (_options.mesh.application_jobs.get ()->*register_supply) (
      [weak, waiter = pending, application, admitted,
       stop_admission] (std::optional<application_job_queue_t::permit_t> reserved) mutable {
          auto waiting = std::move (waiter);
          auto host = weak.lock ();
          if (!reserved || !host) {
              stop_admission (admitted);
              return;
          }
          application->application_permit = std::move (*reserved);
          auto publish = [weak, waiting, application = std::move (application), admitted,
                          stop_admission] () mutable {
              auto host = weak.lock ();
              if (!host || !host->_transport->started ()) {
                  application.reset ();
                  stop_admission (admitted);
                  return;
              }
              auto &dispatch = *application;
              if (uses_local_operation_registry (dispatch.record.kind)
                  && !host->_transport->operation_pending (dispatch.record.operation_id)) {
                  if (admitted)
                      admitted->complete (
                        result_t<void>::failure (framework_error_kind_t::unavailable,
                                                 "application operation ended before admission"));
                  return;
              }
              dispatch.application_permit.mark_queued ();
              mesh::service_mailbox_application_t original;
              original.emplace (std::move (application));
              try {
                  host->publish_local_application (std::move (original));
              }
              catch (...) {
                  if (!admitted)
                      throw;
                  admitted->complete (detail::current_exception_result<void> ());
                  return;
              }
              host->_transport->signal_activity ();
              if (admitted)
                  admitted->complete (result_t<void>::success ());
          };
          if (host->_local_dispatch_completion_lane.is_on_lane ()) {
              publish ();
              return;
          }
          std::function<void (std::exception_ptr)> abandon;
          if (admitted)
              abandon = [admitted, stop_admission] (std::exception_ptr) {
                  stop_admission (admitted);
              };
          if (!host->_local_dispatch_completion_lane.try_post (std::move (publish),
                                                               std::move (abandon)))
              stop_admission (admitted);
      },
      application_job_queue_t::origin_t::local);
    *pending = std::move (registration.waiter);
    _options.mesh.application_jobs->deliver_immediate (registration, *pending);
}

} // namespace zlink::framework::runtime::host
