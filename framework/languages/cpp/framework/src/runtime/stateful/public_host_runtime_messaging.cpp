/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/stateful/public_host_runtime_internal.hpp"

#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/mesh/service_channel_membership.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"

#include <exception>
#include <functional>
#include <runtime/locations/location_repository.hpp>
#include <service_wire_codec.hpp>
#include <service_wire_constants.hpp>
#include <service_wire_pilot_codec.hpp>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
#include <zlink/framework/detail/test_creation_probe.hpp>
#endif

#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
namespace zlink::framework::detail::test_creation_probe
{
std::atomic<void (*) ()> local_completion_sink_fault{nullptr};
namespace
{
std::atomic_size_t actor_request_submissions{0};
}
std::size_t actor_request_submission_count () noexcept
{
    return actor_request_submissions.load (std::memory_order_relaxed);
}
}
#endif

namespace zlink::framework::runtime::host
{

using public_host_runtime_detail::is_actor_application;
using public_host_runtime_detail::stateful_failure_pair;
using public_host_runtime_detail::trace_mesh_host_enabled;
using ::zlink::framework::detail::mesh_trace_enabled;

namespace
{
struct local_completion_sink_t
{
    local_completion_sink_t (
      std::shared_ptr<task_completion_source_t<operation_completion_t>> source,
      std::shared_ptr<operation_completion_t> result,
      spot_request_completion_t completion,
      std::function<task_t<void> ()> incomplete) :
        source (std::move (source)),
        result (std::move (result)),
        completion (std::move (completion)),
        incomplete (std::move (incomplete))
    {
    }

    void operator() (foundation::operation_terminal_t terminal,
                     ::zlink::framework::result_t<zlink::message_t> completion_payload)
    {
        auto payload =
          completion_payload ? std::move (completion_payload.value ()) : zlink::message_t{};
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
        if (terminal != foundation::operation_terminal_t::completed && incomplete)
            (void) finish_failure (std::move (*this), terminal, std::move (terminal_message),
                                   completion_payload.exception ());
        else
            publish (terminal, terminal_message, completion_payload.exception ());
    }

    static task_t<void> finish_failure (local_completion_sink_t sink,
                                        foundation::operation_terminal_t terminal,
                                        std::string terminal_message,
                                        std::exception_ptr failure)
    {
        try {
            co_await sink.incomplete ();
        }
        catch (const std::exception &error) {
            terminal_message += ": rollback failed: ";
            terminal_message += error.what ();
        }
        catch (...) {
            terminal_message += ": rollback failed with an unknown exception";
        }
        sink.publish (terminal, terminal_message, failure);
        co_return;
    }

    void publish (foundation::operation_terminal_t terminal,
                  const std::string &terminal_message,
                  std::exception_ptr failure)
    {
        if (completion) {
            auto decoded =
              failure ? detail::result_access_t::failure<std::vector<zlink::message_t>> (failure)
              : terminal == foundation::operation_terminal_t::completed
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
        } else {
            auto decoded = failure
                             ? detail::result_access_t::failure<operation_completion_t> (failure)
                           : terminal == foundation::operation_terminal_t::completed
                             ? result_t<operation_completion_t>::success (std::move (*result))
                           : terminal == foundation::operation_terminal_t::shutdown
                             ? detail::boundary_failure<operation_completion_t> (
                                 detail::boundary_error_t::shutdown, terminal_message)
                           : terminal == foundation::operation_terminal_t::timed_out
                             ? detail::boundary_failure<operation_completion_t> (
                                 detail::boundary_error_t::timed_out, terminal_message)
                             : result_t<operation_completion_t>::failure (
                                 framework_error_kind_t::internal_failure, terminal_message);
            // This callback owns the registry claim for every local terminal.
            // Reply tokens retain no receiver after registration.
            if (receiver)
                receiver (decoded);
            source->complete (std::move (decoded));
        }
    }

    std::shared_ptr<task_completion_source_t<operation_completion_t>> source;
    std::shared_ptr<operation_completion_t> result;
    spot_request_completion_t completion;
    std::function<task_t<void> ()> incomplete;
    std::function<void (const result_t<operation_completion_t> &)> receiver;
};

// Both immediate and suspended publication share the same terminal cleanup.
template <typename Start>
task_t<zlink::submit_result_t> finish_local_enqueue (mesh::raw_mesh_node_owner_t &transport,
                                                     pending_operation_t *operation,
                                                     Start start)
{
    const auto finish = [&transport, operation] (result_t<zlink::submit_result_t> result) {
        if ((!result || result.value () != zlink::submit_result_t::ok) && operation
            && !operation->registry)
            transport.unregister_local_operation (operation->id);
        if (result && result.value () == zlink::submit_result_t::ok)
            transport.signal_activity ();
        return result;
    };
    try {
        auto pending = start ();
        if (pending.await_ready ()) {
            (void) finish (result_t<zlink::submit_result_t>::success (pending.await_resume ()));
            return pending;
        }
        return [] (task_t<zlink::submit_result_t> pending,
                   decltype (finish) finish) -> task_t<zlink::submit_result_t> {
            std::optional<result_t<zlink::submit_result_t>> result;
            try {
                result.emplace (result_t<zlink::submit_result_t>::success (co_await pending));
            }
            catch (...) {
                result.emplace (detail::current_exception_result<zlink::submit_result_t> ());
            }
            co_return finish (std::move (*result));
        }(std::move (pending), finish);
    }
    catch (...) {
        return task_t<zlink::submit_result_t> (
          finish (detail::current_exception_result<zlink::submit_result_t> ()));
    }
}

task_t<zlink::submit_result_t> local_enqueue_result (zlink::submit_result_t result)
{
    return task_t<zlink::submit_result_t> (result_t<zlink::submit_result_t>::success (result));
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
           static_cast<std::uint64_t> (snapshot->owner.lease_generation),
           snapshot->allocation.target.node_lifecycle_generation},
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
} // namespace

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
    const auto route_fence = co_await _host->resolve_spot_route_fence (
      target_node_rid, target_spot_id, target_spot_generation);
    if (!route_fence)
        co_return zlink::submit_result_t::not_found;
    const auto target = protocol::spot_route_fence_t{target_spot_id,
                                                     target_spot_generation,
                                                     target_node_rid.to_bytes (),
                                                     route_fence->target_node_generation,
                                                     route_fence->authority_owner_generation,
                                                     route_fence->owner_lease_generation};
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
        const auto route_fence = co_await _host->resolve_spot_route_fence (
          target_node_rid, target_spot_id, target_spot_generation);
        if (!route_fence) {
            co_return zlink::submit_result_t::not_found;
        }
        const auto target = protocol::spot_route_fence_t{target_spot_id,
                                                         target_spot_generation,
                                                         target_node_rid.to_bytes (),
                                                         route_fence->target_node_generation,
                                                         route_fence->authority_owner_generation,
                                                         route_fence->owner_lease_generation};
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
            foundation::operation_terminal_t terminal,
            ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
              auto payload = std::move (completion_payload.value ());
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
    std::optional<route_fence_t> fence;
    if (resolver) {
        auto resolved = co_await await_result (
          resolver (target_node_rid, target_spot_id, target_spot_generation));
        if (resolved)
            fence = resolved.value ();
    } else {
        const auto measured_at = std::chrono::steady_clock::now ();
        const auto read =
          co_await read_route_owner_fence (store, '2', target_spot_id, target_spot_generation, 0, 0,
                                           _options.owner_lease_fencing_margin);
        if (read
            && (!read->admission_lifetime
                || std::chrono::steady_clock::now () < measured_at + *read->admission_lifetime))
            fence = read->fence;
    }
    if (!fence || fence->target_node_generation == 0)
        co_return std::nullopt;
    co_return fence;
}

task_t<zlink::submit_result_t> public_host_runtime_t::send_to_actor (
  const actor_ref_t &target,
  const std::vector<zlink::message_t> &parts,
  std::span<const std::uint8_t> metadata,
  std::uint64_t authority_owner_generation,
  std::uint64_t owner_lease_generation,
  std::optional<protocol::actor_message_header_t::bound_session_source_t> bound_session_source,
  std::uint64_t node_generation)
{
    const auto target_node_routing_id =
      zlink::routing_id_t::from (std::string (target.node_rid ().value ())).to_bytes ();
    if (target_node_routing_id == status ().routing_id ().to_bytes ()) {
        co_return co_await enqueue_local_actor_message (
          target, record_kind_t::actor_send, parts, nullptr, std::move (bound_session_source), {},
          authority_owner_generation, owner_lease_generation);
    }
    // Submit/completion §5: a missing Actor authority is NotFound before any route check.
    const auto object = co_await _objects.find_async (stateful::object_kind_t::actor,
                                                      std::string (target.actor_id ().value ()));
    const auto authority_generation = authority_owner_generation != 0 ? authority_owner_generation
                                      : object ? object->authority_owner_generation
                                               : target.object_generation ();
    const auto route_fence = co_await read_route_owner_fence (
      _user_spot_store, '1', std::string (target.actor_id ().value ()), target.object_generation (),
      authority_generation, owner_lease_generation, _options.owner_lease_fencing_margin);
    if (!route_fence || route_fence->fence.authority_owner_generation != authority_generation
        || node_generation == 0)
        co_return zlink::submit_result_t::not_found;
    // G is the authority's owner lifecycle generation. The send admission
    // accepts only a ready peer of G; any other peer is NotConnected.
    co_return co_await _transport->send_to_actor_result (
      target_node_routing_id, std::nullopt,
      protocol::actor_route_fence_t{std::string (target.actor_id ().value ()),
                                    target.object_generation (), target_node_routing_id,
                                    node_generation, authority_generation,
                                    route_fence->fence.owner_lease_generation},
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

task_t<void> public_host_runtime_t::register_actor_request (pending_operation_t &operation,
                                                            std::chrono::milliseconds timeout)
{
    const auto deadline = foundation::operation_registry_t::clock_t::now () + timeout;
    const auto reservation = co_await prepare_local_completion (operation);
    if (!reservation)
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "Actor request source is closed");
    operation.id = reservation->first;
    operation.registry = _transport->operation_registry ();
    auto host = shared_from_this ();
    if (!_transport->register_prepared_local_operation (
          *reservation, deadline,
          [host, operation] (auto terminal, auto payload) {
              if (!payload)
                  operation.completion->complete (
                    detail::result_access_t::failure<operation_completion_t> (
                      payload.exception ()));
              else
                  host->complete_operation (operation, operation_kind_t::none, terminal,
                                            std::move (payload.value ()));
          },
          mesh_request_surface_t::actor))
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "Actor request registry is closed");
}

task_t<zlink::submit_result_t> public_host_runtime_t::request_to_actor (
  const actor_ref_t &target,
  const std::vector<zlink::message_t> &parts,
  pending_operation_t &operation,
  std::chrono::milliseconds timeout,
  std::span<const std::uint8_t> metadata,
  std::uint64_t authority_owner_generation,
  std::uint64_t owner_lease_generation,
  std::optional<protocol::actor_message_header_t::bound_session_source_t> bound_session_source,
  std::uint64_t node_generation)
{
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
    ::zlink::framework::detail::test_creation_probe::actor_request_submissions.fetch_add (
      1, std::memory_order_relaxed);
#endif
    const auto target_routing_id =
      zlink::routing_id_t::from (std::string (target.node_rid ().value ()));
    if (target_routing_id.to_bytes () == status ().routing_id ().to_bytes ()) {
        const auto accepted = co_await enqueue_local_actor_message (
          target, record_kind_t::actor_request, parts, &operation, std::move (bound_session_source),
          timeout, authority_owner_generation, owner_lease_generation);
        co_return accepted;
    }
    if (!operation.registry) {
        operation.prepare_for_registration ();
        operation.id = next_operation ();
    }
    const auto object = co_await _objects.find_async (stateful::object_kind_t::actor,
                                                      std::string (target.actor_id ().value ()));
    const auto authority_generation = authority_owner_generation != 0 ? authority_owner_generation
                                      : object ? object->authority_owner_generation
                                               : target.object_generation ();
    const auto route_fence = co_await read_route_owner_fence (
      _user_spot_store, '1', std::string (target.actor_id ().value ()), target.object_generation (),
      authority_generation, owner_lease_generation, _options.owner_lease_fencing_margin);
    if (!route_fence || route_fence->fence.authority_owner_generation != authority_generation
        || node_generation == 0) {
        co_return zlink::submit_result_t::not_found;
    }
    const auto deadline =
      operation.registry ? operation.registry->deadline (operation.id) : std::nullopt;
    if (operation.registry && !deadline)
        co_return zlink::submit_result_t::not_connected;
    const auto host = shared_from_this ();
    auto public_operation = operation;
    public_operation.receiver = {};
    foundation::operation_registry_t::callback_t complete_public;
    if (operation.receiver) {
        complete_public = [host, public_operation] (auto terminal, auto payload) {
            // The prepared receiver owns application decoding. This task publishes only
            // the registry's terminal, including a timeout whose dispatcher ran later.
            auto record =
              payload ? host->decode_operation_record (public_operation.id, operation_kind_t::none,
                                                       terminal, payload.value ())
                      : detail::result_access_t::failure<receive_record_t> (payload.exception ());
            public_operation.completion->complete (
              record
                ? result_t<operation_completion_t>::success ({record.value (), {}})
                : detail::result_access_t::failure<operation_completion_t> (record.exception ()));
        };
    } else {
        complete_public = [host, public_operation] (auto terminal, auto payload) {
            public_operation.completion->complete (
              payload
                ? host->decode_operation_completion (public_operation, operation_kind_t::none,
                                                     terminal, payload.value ())
                : detail::result_access_t::failure<operation_completion_t> (payload.exception ()));
        };
    }
    auto submission = _transport->request_to_actor (
      zlink::routing_id_t::from (std::string (target.node_rid ().value ())).to_bytes (),
      std::nullopt,
      protocol::actor_route_fence_t{
        std::string (target.actor_id ().value ()), target.object_generation (),
        zlink::routing_id_t::from (std::string (target.node_rid ().value ())).to_bytes (),
        node_generation, authority_generation, route_fence->fence.owner_lease_generation},
      encode_application (parts, metadata), timeout, std::move (complete_public),
      protocol::wire_operation_id_t{operation.id.high, operation.id.low},
      std::move (bound_session_source), std::nullopt, 0,
      operation.receiver
        ? mesh::raw_mesh_node_owner_t::physical_receiver_t{[host, operation] (auto terminal,
                                                                              auto payload,
                                                                              bool claimed) {
              operation.receive (
                payload
                  ? host->decode_operation_completion (operation, operation_kind_t::none, terminal,
                                                       payload.value ())
                  : detail::result_access_t::failure<operation_completion_t> (payload.exception ()),
                claimed);
          }}
        : mesh::raw_mesh_node_owner_t::physical_receiver_t{},
      operation.registry, deadline);
    // C1 consumes the callback synchronously at publication. The binding/readiness
    // producer now owns physical delivery even if admission later fails.
    if (!complete_public)
        operation.receiver = {};
    const auto accepted = co_await submission;
    co_return submitted (accepted);
}

task_t<zlink::submit_result_t>
public_host_runtime_t::send_to_node (const zlink::routing_id_t &target,
                                     const std::vector<zlink::message_t> &parts)
{
    try {
        return _transport->send_to_node_result (
          std::span<const std::uint8_t> (target.data (), target.size ()),
          encode_application (parts));
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
        return _transport->send_to_node_result (
          std::span<const std::uint8_t> (target.data (), target.size ()),
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
try {
    operation.prepare_for_registration ();
    operation.id = next_operation ();
    const auto host = shared_from_this ();
    return request_to_node (
      target, std::move (parts), timeout,
      [host, operation] (foundation::operation_terminal_t terminal,
                         result_t<zlink::message_t> completion_payload) mutable {
          host->complete_operation (operation, operation_kind_t::none, terminal,
                                    std::move (completion_payload.value ()));
      });
}
catch (...) {
    return task_t<zlink::submit_result_t> (
      detail::current_exception_result<zlink::submit_result_t> ());
}

task_t<zlink::submit_result_t>
public_host_runtime_t::request_to_node (const zlink::routing_id_t &target,
                                        std::vector<zlink::message_t> parts,
                                        std::chrono::milliseconds timeout,
                                        foundation::operation_registry_t::callback_t &&completion,
                                        std::shared_ptr<task_completion_source_t<void>> admitted)
try {
    const auto host = shared_from_this ();
    const std::span<const std::uint8_t> target_bytes (target.data (), target.size ());
    auto accepted =
      _transport->request_to_node (target_bytes, encode_application (std::move (parts)), timeout,
                                   std::move (completion), {}, std::move (admitted));
    auto classify = [host, target] (const result_t<bool> &result) {
        if (!result)
            return detail::result_access_t::failure<zlink::submit_result_t> (result.exception ());
        return result_t<zlink::submit_result_t>::success (
          host->_transport->node_request_submission_result ({target.data (), target.size ()},
                                                            result.value ()));
    };
    if (accepted.await_ready ())
        return task_t<zlink::submit_result_t> (
          classify (detail::task_result_access_t::read (accepted)));
    return detail::project_task_terminal<zlink::submit_result_t> (accepted, std::move (classify));
}
catch (...) {
    return task_t<zlink::submit_result_t> (
      detail::current_exception_result<zlink::submit_result_t> ());
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
      [host,
       operation] (foundation::operation_terminal_t terminal,
                   ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
          auto payload = std::move (completion_payload.value ());
          host->complete_operation (operation, operation_kind_t::none, terminal,
                                    std::move (payload));
      });
    co_return submitted (accepted);
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
        std::tie (error, membership) =
          co_await _objects.begin_membership_move_async (*actor, std::move (target));
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

template <typename Parts>
bool public_host_runtime_t::reply_parts (const reply_token_t &token, Parts &&parts)
{
    try {
        if (token.local_reply)
            return token.local_reply (parts);
        return token.request
               && _transport->reply (*token.request,
                                     encode_application (std::forward<Parts> (parts)));
    }
    catch (const zlink::submit_error_t &) {
        return false;
    }
}

bool public_host_runtime_t::reply (const reply_token_t &token,
                                   const std::vector<zlink::message_t> &parts)
{
    return reply_parts (token, parts);
}

bool public_host_runtime_t::reply (const reply_token_t &token,
                                   std::vector<zlink::message_t> &&parts)
{
    return reply_parts (token, std::move (parts));
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

std::chrono::steady_clock::time_point public_host_runtime_t::register_local_completion (
  pending_operation_t &operation,
  const mesh::raw_mesh_node_owner_t::local_operation_reservation_t &reservation,
  std::chrono::milliseconds timeout,
  spot_request_completion_t completion,
  std::function<task_t<void> ()> incomplete,
  mesh_request_surface_t request_surface)
{
    const auto source = operation.completion;
    const auto result = std::make_shared<operation_completion_t> ();
    result->record.kind = record_kind_t::completion;
    result->record.domain = ready_domain_t::infrastructure;
    result->record.operation_id = reservation.first;
    result->record.source_node_rid =
      zlink::routing_id_t::from (_options.mesh.descriptor.node_routing_id);
    const auto deadline = operation.registry
                            ? operation.registry->deadline (operation.id)
                                .value_or (foundation::operation_registry_t::clock_t::time_point{})
                            : foundation::operation_registry_t::clock_t::now () + timeout;
    foundation::operation_registry_t::callback_t complete;
    local_completion_sink_t *receiver_owner = nullptr;
    try {
        auto sink = std::make_shared<local_completion_sink_t> (
          source, result, std::move (completion), std::move (incomplete));
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
        if (auto fault =
              ::zlink::framework::detail::test_creation_probe::local_completion_sink_fault
                .exchange (nullptr))
            fault ();
#endif
        receiver_owner = sink.get ();
        complete = foundation::operation_registry_t::callback_t{
          std::move (sink),
          [] (local_completion_sink_t &selected, foundation::operation_terminal_t terminal,
              ::zlink::framework::result_t<zlink::message_t> payload) {
              selected (terminal, std::move (payload));
          }};
        receiver_owner->receiver = std::exchange (operation.receiver, {});
        const auto registered =
          operation.registry
            ? operation.registry->continue_operation (operation.id, std::move (complete))
            : _transport->register_prepared_local_operation (reservation, deadline,
                                                             std::move (complete), request_surface);
        if (!registered)
            throw framework_exception_t (framework_error_kind_t::shutting_down,
                                         "Operation completion registry is closed");
    }
    catch (...) {
        // An unconsumed callback never acquired a C1 entry. Its physical
        // receiver still needs the same terminal and capture retirement.
        if (operation.registry) {
            if (complete)
                operation.receiver = std::move (receiver_owner->receiver);
        } else if (complete)
            complete (foundation::operation_terminal_t::shutdown,
                      result_t<zlink::message_t>::success ({}));
        else
            _transport->fail_local_operation (reservation.first,
                                              foundation::operation_terminal_t::transport_failed);
        throw;
    }
    if (!operation.registry)
        operation.id = reservation.first;
    operation.local_result = result;
    return deadline;
}


bool public_host_runtime_t::enqueue_completion (const pending_operation_t &operation,
                                                receive_record_t record,
                                                std::vector<zlink::message_t> parts)
{
    auto fill = [&] {
        *operation.local_result = operation_completion_t{std::move (record), std::move (parts)};
    };
    if (operation.registry)
        return operation.registry->complete (operation.id, zlink::message_t{}, fill);
    return _transport->complete_local_operation (operation.id, fill);
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
    if (current)
        std::tie (error, membership) =
          co_await _objects.begin_membership_move_async (*current, std::move (target));
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
    std::optional<std::chrono::steady_clock::time_point> operation_deadline;
    struct incomplete_join_t
    {
        static task_t<void> run (std::shared_ptr<public_host_runtime_t> host,
                                 stateful::membership_token_t membership,
                                 std::function<task_t<void> ()> cleanup)
        {
            if (cleanup)
                co_await cleanup ();
            if (membership.value != 0)
                (void) co_await host->_objects.abort_membership_move_async (membership);
        }
    };
    try {
        co_await _lifecycle_configuration_lane.run_task ([&] {
            if (!_started || _closing)
                return;
            try {
                operation_deadline = register_local_completion (
                  operation, *reservation, timeout, {},
                  [host = shared_from_this (), membership,
                   cleanup = local_request_wait_cleanup (waiter)] () -> task_t<void> {
                      return incomplete_join_t::run (host, membership, cleanup);
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
            record.operation_deadline = operation_deadline;
            record.source_node_rid = status ().routing_id ();
            record.actor_control = actor_control_t{lifecycle_kind_t::joined, actor};
            record.reply_token.local_actor_join =
              [weak, operation, actor_type,
               membership] (const result_t<actor_join_operation_result_t> &result,
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

bool public_host_runtime_t::complete_local_actor_join (
  pending_operation_t operation,
  std::string actor_type,
  stateful::membership_token_t membership,
  const result_t<actor_join_operation_result_t> &result,
  const std::vector<zlink::message_t> &parts)
{
    struct complete_actor_join_t
    {
        static task_t<void> run (std::shared_ptr<public_host_runtime_t> host,
                                 std::decay_t<decltype (operation)> operation,
                                 std::decay_t<decltype (std::move (actor_type))> actor_type,
                                 std::decay_t<decltype (membership)> membership,
                                 std::decay_t<decltype (result)> result,
                                 std::decay_t<decltype (parts)> parts)
        {
            auto *self = host.get ();
            receive_record_t completion;
            completion.kind = record_kind_t::completion;
            completion.domain = ready_domain_t::infrastructure;
            completion.operation_id = operation.id;
            completion.operation_kind = operation_kind_t::actor_join;
            completion.source_node_rid =
              zlink::routing_id_t::from (self->_options.mesh.descriptor.node_routing_id);

            if (!result) {
                (void) co_await self->_objects.abort_membership_move_async (membership);
                completion.join_completion = detail::propagate_failure<actor_join_completion_t> (
                  result, "Local Actor join failed");
            } else if (result.value ().terminal_result != 0) {
                (void) co_await self->_objects.abort_membership_move_async (membership);
                completion.terminal_result = result.value ().terminal_result;
                completion.failure_errno = result.value ().failure_code;
            } else if (result.value ().join_result == protocol::actor_join_result_t::accepted) {
                auto [error, current] =
                  co_await self->_objects.commit_membership_move_async (membership);
                if (error != stateful::stateful_error_t::none) {
                    //  Spec 32-framework-error-model:119-120 — a Framework execution
                    //  failure after the application ACCEPTED the join is not an
                    //  application rejection; carry a classified terminal
                    //  (internalError) instead of synthesizing typed Rejected.
                    const auto failure =
                      messaging::request_failure_mapper_t{}.target_failure_reply (
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
                    completion.join_completion = result_t<actor_join_completion_t>::success (
                      {join_admission_t::accepted, actor});
                }
            } else {
                (void) co_await self->_objects.abort_membership_move_async (membership);
                completion.join_completion = result_t<actor_join_completion_t>::success (
                  {join_admission_t::rejected,
                   self->framework_actor_ref (membership.actor, actor_type)});
            }

            *operation.local_result =
              operation_completion_t{std::move (completion), std::move (parts)};
        }
    };
    const bool completed = _transport->complete_local_operation_async (
      operation.id,
      [host = shared_from_this (), operation, actor_type = std::move (actor_type), membership,
       result = result, parts = parts] () mutable -> task_t<void> {
          return complete_actor_join_t::run (std::move (host), std::move (operation),
                                             std::move (actor_type), std::move (membership),
                                             std::move (result), std::move (parts));
      });
    return completed;
}

std::function<task_t<void> ()> public_host_runtime_t::local_request_wait_cleanup (
  const std::shared_ptr<application_job_queue_t::waiter_t> &waiter)
{
    if (!waiter)
        return {};
    struct wait_cleanup_t
    {
        static task_t<void> run (std::weak_ptr<public_host_runtime_t> weak,
                                 std::shared_ptr<application_job_queue_t::waiter_t> waiter)
        {
            const auto cancel = [waiter] { (void) waiter->cancel (); };
            if (auto host = weak.lock ();
                host && !host->_local_dispatch_completion_lane.is_on_lane ())
                co_await host->_local_dispatch_completion_lane.run_task (cancel);
            else
                cancel ();
        }
    };
    return [weak = weak_from_this (), waiter] () -> task_t<void> {
        return wait_cleanup_t::run (weak, waiter);
    };
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
    if (!is_actor_application (kind))
        return local_enqueue_result (zlink::submit_result_t::invalid_argument);
    // Identity lookup does not duplicate the Actor gate's recovery admission.
    return continue_task<zlink::submit_result_t> (
      resolve_actor_async (std::string (target.actor_id ().value ())),
      [this, &target, kind, &parts, operation,
       bound_session_source = std::move (bound_session_source), timeout, authority_owner_generation,
       owner_lease_generation] (
        std::optional<stateful::object_ref_t> current) mutable -> task_t<zlink::submit_result_t> {
          if (!current)
              return local_enqueue_result (zlink::submit_result_t::not_found);
          auto actor = framework_actor_ref (
            *current,
            std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (target)));
          auto source_node_rid = status ().routing_id ();
          std::optional<zlink::routing_id_t> source_session_rid;
          std::uint64_t source_binding_generation = 0;
          std::uint64_t source_session_sequence = 0;
          if (bound_session_source) {
              source_session_rid =
                zlink::routing_id_t::from (bound_session_source->session_routing_id);
              source_binding_generation = bound_session_source->binding_generation;
              source_session_sequence = bound_session_source->session_sequence;
          }
          auto waiter = operation && _options.mesh.application_jobs
                          ? std::make_shared<application_job_queue_t::waiter_t> ()
                          : nullptr;
          auto enqueue =
            [this, &parts, operation, kind, timeout, authority_owner_generation,
             owner_lease_generation, current = std::move (*current), actor = std::move (actor),
             source_node_rid = std::move (source_node_rid),
             source_session_rid = std::move (source_session_rid), source_binding_generation,
             source_session_sequence, waiter = std::move (waiter)] (
              std::optional<mesh::raw_mesh_node_owner_t::local_operation_reservation_t> reservation,
              std::optional<std::uint64_t> sequence) mutable -> task_t<zlink::submit_result_t> {
              call_id_t operation_id;
              if (sequence)
                  operation_id = {_options.mesh.descriptor.lifecycle_generation, *sequence};
              return finish_local_enqueue (*_transport, operation, [&] {
                  return _lifecycle_configuration_lane.run_task (
                    [this, &parts, operation, kind, reservation = std::move (reservation), timeout,
                     authority_owner_generation, owner_lease_generation,
                     current = std::move (current), actor = std::move (actor),
                     source_node_rid = std::move (source_node_rid),
                     source_session_rid = std::move (source_session_rid), source_binding_generation,
                     source_session_sequence, operation_id,
                     waiter = std::move (waiter)] () mutable -> task_t<zlink::submit_result_t> {
                        if (!_started || _closing)
                            return local_enqueue_result (zlink::submit_result_t::terminated);
                        reply_token_t reply_token;
                        if (operation) {
                            register_local_completion (
                              *operation,
                              reservation.value_or (
                                mesh::raw_mesh_node_owner_t::local_operation_reservation_t{
                                  operation->id, {}}),
                              timeout, {}, local_request_wait_cleanup (waiter),
                              mesh_request_surface_t::actor);
                            operation_id = operation->id;
                            std::weak_ptr<public_host_runtime_t> weak = shared_from_this ();
                            reply_token.host = weak;
                            reply_token.local_reply =
                              [weak, operation =
                                       *operation] (const std::vector<zlink::message_t> &reply) {
                                  const auto host = weak.lock ();
                                  return host && host->complete_local_request (operation, reply);
                              };
                        }
                        return _local_dispatch_completion_lane.run_task (
                          [this, &parts, operation, kind, authority_owner_generation,
                           owner_lease_generation, current = std::move (current),
                           actor = std::move (actor), source_node_rid = std::move (source_node_rid),
                           source_session_rid = std::move (source_session_rid),
                           source_binding_generation, source_session_sequence, operation_id,
                           reply_token = std::move (reply_token),
                           waiter = std::move (waiter)] () mutable {
                              ready_record_t owner{.owner_kind = owner_kind_t::actor,
                                                   .domain = ready_domain_t::application,
                                                   .actor = std::move (actor)};
                              receive_record_t record;
                              record.kind = kind;
                              record.domain = ready_domain_t::application;
                              record.source_node_rid = std::move (source_node_rid);
                              record.source_session_rid = std::move (source_session_rid);
                              record.source_binding_generation = source_binding_generation;
                              record.source_session_sequence = source_session_sequence;
                              record.operation_id = operation_id;
                              record.reply_token = std::move (reply_token);
                              if (_stateful_dispatch) {
                                  record.actor_route = {
                                    current.key,
                                    current.object_generation,
                                    _options.mesh.descriptor.node_routing_id,
                                    _options.mesh.descriptor.lifecycle_generation,
                                    authority_owner_generation != 0
                                      ? authority_owner_generation
                                      : current.authority_owner_generation,
                                    owner_lease_generation};
                                  if (operation)
                                      record.reply_route_id = operation->id.low;
                              }
                              admit_local_application (
                                local_application_dispatch_t{std::move (owner), std::move (record),
                                                             parts},
                                waiter);
                              return zlink::submit_result_t::ok;
                          });
                    });
              });
          };
          if (operation && operation->registry)
              return enqueue (std::nullopt, std::nullopt);
          if (operation)
              return continue_task<zlink::submit_result_t> (
                prepare_local_completion (*operation),
                [enqueue = std::move (enqueue)] (auto reservation) mutable {
                    if (!reservation)
                        return local_enqueue_result (zlink::submit_result_t::terminated);
                    return enqueue (std::move (reservation), std::nullopt);
                });
          if (_stateful_dispatch)
              return continue_task<zlink::submit_result_t> (
                _transport->next_operation_sequence_async (),
                [enqueue = std::move (enqueue)] (auto sequence) mutable {
                    if (!sequence)
                        return local_enqueue_result (zlink::submit_result_t::terminated);
                    return enqueue (std::nullopt, sequence);
                });
          return enqueue (std::nullopt, std::nullopt);
      });
}

task_t<zlink::submit_result_t>
public_host_runtime_t::enqueue_local_spot_send (const protocol::spot_route_fence_t &target,
                                                const std::vector<zlink::message_t> &parts,
                                                std::string source_spot_id)
{
    auto local = status ();
    if (target.target_node_routing_id != local.routing_id ().to_bytes ()
        || target.target_node_generation != local.lifecycle_generation ())
        return local_enqueue_result (zlink::submit_result_t::not_found);
    return continue_task<zlink::submit_result_t> (
      resolve_spot_async (target.spot_id),
      [this, &target, &parts, local = std::move (local),
       source_spot_id = std::move (source_spot_id)] (
        std::optional<stateful::object_ref_t> object) mutable -> task_t<zlink::submit_result_t> {
          if (!object || object->node_id != local.routing_id ().to_string ())
              return local_enqueue_result (zlink::submit_result_t::not_found);
          auto source_node_rid = local.routing_id ();
          auto route = target;
          return continue_task<zlink::submit_result_t> (
            _transport->next_operation_sequence_async (),
            [this, &parts, target = std::move (route),
             source_node_rid = std::move (source_node_rid),
             source_spot_id = std::move (source_spot_id)] (
              std::optional<std::uint64_t> sequence) mutable -> task_t<zlink::submit_result_t> {
                if (!sequence)
                    return local_enqueue_result (zlink::submit_result_t::terminated);
                const call_id_t operation_id{_options.mesh.descriptor.lifecycle_generation,
                                             *sequence};
                return finish_local_enqueue (*_transport, nullptr, [&] {
                    return _lifecycle_configuration_lane.run_task (
                      [this, &parts, target = std::move (target),
                       source_node_rid = std::move (source_node_rid), operation_id,
                       source_spot_id =
                         std::move (source_spot_id)] () mutable -> task_t<zlink::submit_result_t> {
                          if (!_started || _closing)
                              return local_enqueue_result (zlink::submit_result_t::terminated);
                          return _local_dispatch_completion_lane.run_task (
                            [this, &parts, target = std::move (target),
                             source_node_rid = std::move (source_node_rid), operation_id,
                             source_spot_id = std::move (source_spot_id)] () mutable {
                                ready_record_t owner{.owner_kind = owner_kind_t::spot,
                                                     .domain = ready_domain_t::application,
                                                     .spot_id = target.spot_id};
                                receive_record_t record;
                                record.kind = record_kind_t::spot_send;
                                record.domain = ready_domain_t::application;
                                record.source_node_rid = std::move (source_node_rid);
                                record.spot_route = std::move (target);
                                record.operation_id = operation_id;
                                admit_local_application (local_application_dispatch_t{
                                  std::move (owner), std::move (record), parts,
                                  std::move (source_spot_id)});
                                return zlink::submit_result_t::ok;
                            });
                      });
                });
            });
      });
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
    auto local = status ();
    if (target.target_node_routing_id != local.routing_id ().to_bytes ()
        || target.target_node_generation != local.lifecycle_generation ())
        return local_enqueue_result (zlink::submit_result_t::not_found);
    return continue_task<zlink::submit_result_t> (
      resolve_spot_async (target.spot_id),
      [this, &target, &parts, &operation, timeout, local = std::move (local),
       completion = std::move (completion), source_spot_id = std::move (source_spot_id)] (
        std::optional<stateful::object_ref_t> object) mutable -> task_t<zlink::submit_result_t> {
          if (!object || object->node_id != local.routing_id ().to_string ())
              return local_enqueue_result (zlink::submit_result_t::not_found);
          auto source_node_rid = local.routing_id ();
          auto route = target;
          auto waiter = _options.mesh.application_jobs
                          ? std::make_shared<application_job_queue_t::waiter_t> ()
                          : nullptr;
          return continue_task<zlink::submit_result_t> (
            prepare_local_completion (operation),
            [this, &parts, &operation, timeout, target = std::move (route),
             source_node_rid = std::move (source_node_rid), waiter = std::move (waiter),
             completion = std::move (completion), source_spot_id = std::move (source_spot_id)] (
              std::optional<mesh::raw_mesh_node_owner_t::local_operation_reservation_t>
                reservation) mutable -> task_t<zlink::submit_result_t> {
                if (!reservation)
                    return local_enqueue_result (zlink::submit_result_t::terminated);
                return finish_local_enqueue (*_transport, &operation, [&] {
                    return _lifecycle_configuration_lane.run_task (
                      [this, &parts, &operation, timeout, reservation = std::move (*reservation),
                       target = std::move (target), source_node_rid = std::move (source_node_rid),
                       waiter = std::move (waiter), completion = std::move (completion),
                       source_spot_id =
                         std::move (source_spot_id)] () mutable -> task_t<zlink::submit_result_t> {
                          if (!_started || _closing)
                              return local_enqueue_result (zlink::submit_result_t::terminated);
                          register_local_completion (
                            operation, reservation, timeout, std::move (completion),
                            local_request_wait_cleanup (waiter), mesh_request_surface_t::spot);
                          const auto operation_id = operation.id;
                          const auto reply_route_id = operation.id.low;
                          reply_token_t reply_token;
                          std::weak_ptr<public_host_runtime_t> weak = shared_from_this ();
                          reply_token.host = weak;
                          reply_token.local_reply =
                            [weak, operation] (const std::vector<zlink::message_t> &reply) {
                                const auto host = weak.lock ();
                                return host && host->complete_local_request (operation, reply);
                            };
                          return _local_dispatch_completion_lane.run_task (
                            [this, &parts, target = std::move (target),
                             source_node_rid = std::move (source_node_rid), operation_id,
                             reply_route_id, reply_token = std::move (reply_token),
                             waiter = std::move (waiter),
                             source_spot_id = std::move (source_spot_id)] () mutable {
                                ready_record_t owner{.owner_kind = owner_kind_t::spot,
                                                     .domain = ready_domain_t::application,
                                                     .spot_id = target.spot_id};
                                receive_record_t record;
                                record.kind = record_kind_t::spot_request;
                                record.domain = ready_domain_t::application;
                                record.source_node_rid = std::move (source_node_rid);
                                record.spot_route = std::move (target);
                                record.operation_id = operation_id;
                                record.reply_route_id = reply_route_id;
                                record.reply_token = std::move (reply_token);
                                admit_local_application (
                                  local_application_dispatch_t{std::move (owner),
                                                               std::move (record), parts,
                                                               std::move (source_spot_id)},
                                  waiter);
                                return zlink::submit_result_t::ok;
                            });
                      });
                });
            });
      });
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

result_t<receive_record_t>
public_host_runtime_t::decode_operation_record (call_id_t operation,
                                                operation_kind_t kind,
                                                foundation::operation_terminal_t terminal,
                                                const zlink::message_t &payload) const
{
    const auto status =
      messaging::request_failure_mapper_t{}.operation_reply_status (terminal, payload);
    if (!status)
        return detail::result_access_t::failure<receive_record_t> (status.exception ());
    receive_record_t record;
    record.kind = record_kind_t::completion;
    record.domain = ready_domain_t::infrastructure;
    record.operation_id = operation;
    record.operation_kind = kind;
    record.source_node_rid = zlink::routing_id_t::from (_options.mesh.descriptor.node_routing_id);
    record.terminal_result = static_cast<int> (status.value ().terminal_result);
    record.failure_errno = static_cast<int> (status.value ().failure_code);
    return result_t<receive_record_t>::success (std::move (record));
}

result_t<operation_completion_t>
public_host_runtime_t::decode_operation_completion (const pending_operation_t &operation,
                                                    operation_kind_t kind,
                                                    foundation::operation_terminal_t terminal,
                                                    zlink::message_t payload)
{
    try {
        auto decoded_record = decode_operation_record (operation.id, kind, terminal, payload);
        if (!decoded_record) {
            return detail::result_access_t::failure<operation_completion_t> (
              decoded_record.exception ());
        }
        auto record = std::move (decoded_record.value ());
        std::vector<zlink::message_t> parts;
        if (record.terminal_result == 0) {
            try {
                parts = protocol::decode_application_parts (std::move (payload), capture_flow ());
            }
            catch (const protocol::service_wire_error_t &) {
                record.terminal_result = static_cast<int> (zlink::request_result_t::protocol_error);
            }
        }
        return result_t<operation_completion_t>::success (
          operation_completion_t{std::move (record), std::move (parts)});
    }
    catch (const std::exception &error) {
        return result_t<operation_completion_t>::failure (framework_error_kind_t::internal_failure,
                                                          error.what ());
    }
}

void public_host_runtime_t::complete_operation (const pending_operation_t &operation,
                                                operation_kind_t kind,
                                                foundation::operation_terminal_t terminal,
                                                zlink::message_t payload)
{
    operation.completion->complete (
      decode_operation_completion (operation, kind, terminal, std::move (payload)));
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

template <typename Parts>
zlink::submit_result_t submit_reply (const reply_token_t &token, Parts &&parts)
{
    const auto host = token.host.lock ();
    return host && host->reply (token, std::forward<Parts> (parts))
             ? zlink::submit_result_t::ok
             : zlink::submit_result_t::terminated;
}

zlink::submit_result_t reply (const reply_token_t &token,
                              const std::vector<zlink::message_t> &parts)
{
    return submit_reply (token, parts);
}

zlink::submit_result_t reply (const reply_token_t &token, std::vector<zlink::message_t> &&parts)
{
    return submit_reply (token, std::move (parts));
}

bool actor_join_reply (const reply_token_t &token,
                       result_t<actor_join_result_t> result,
                       const std::vector<zlink::message_t> &parts)
{
    if (token.local_actor_join) {
        if (!result)
            return token.local_actor_join (
              detail::propagate_failure<actor_join_operation_result_t> (result,
                                                                        "Local Actor join failed"),
              parts);
        actor_join_operation_result_t local;
        local.join_result = result.value () == actor_join_result_t::accepted
                              ? protocol::actor_join_result_t::accepted
                              : protocol::actor_join_result_t::rejected;
        return token.local_actor_join (
          result_t<actor_join_operation_result_t>::success (std::move (local)), parts);
    }
    actor_join_operation_result_t completion;
    if (result) {
        completion.join_result = result.value () == actor_join_result_t::accepted
                                   ? protocol::actor_join_result_t::accepted
                                   : protocol::actor_join_result_t::rejected;
    } else {
        const messaging::request_failure_mapper_t mapper;
        const auto failure = mapper.target_failure_reply (result.exception ());
        completion.terminal_result = failure->terminal_result;
        completion.failure_code = failure->failure_code;
    }
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
