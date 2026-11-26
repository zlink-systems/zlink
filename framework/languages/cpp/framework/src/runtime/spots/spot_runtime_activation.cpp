/* SPDX-License-Identifier: Elastic-2.0 */

#include "spot_runtime_internal.hpp"

#include <zlink/framework/contracts/configuration/zlink_builder.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/route_channel_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/actor_execution_context.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/configuration/request_defaults.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/spots/spot_route_internal_dispatcher.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include "runtime/timers/core_timer_drain_loop.hpp"
#include "runtime/utils/uuid.hpp"

#include <zlink/framework/contracts/channels/call.hpp>

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Eventing/timers.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <algorithm>
#include <charconv>
#include <cassert>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace zlink::framework
{

namespace service = runtime::host;

namespace
{
using detail::framework_worker_executor_core;
using detail::is_blank;

class spot_worker_scheduler_t final : public detail::worker_scheduler_t
{
  public:
    spot_worker_scheduler_t (std::shared_ptr<runtime::offload_executor_t> workers,
                             std::weak_ptr<detail::spot_context_state_t> owner) :
        _workers (std::move (workers)), _owner (std::move (owner))
    {
    }

    bool try_schedule (std::function<void (std::stop_token)> work) override
    {
        return _workers && _workers->try_submit_cancellable (std::move (work));
    }

    std::stop_token stop_token () const noexcept override
    {
        if (auto owner = _owner.lock (); owner && owner->node) {
            return owner->node->worker_cancellation.get_token ();
        }
        return {};
    }

  private:
    std::shared_ptr<runtime::offload_executor_t> _workers;
    std::weak_ptr<detail::spot_context_state_t> _owner;
};

std::shared_ptr<runtime::offload_executor_t>
framework_worker_executor (const std::shared_ptr<detail::spot_node_builder_state_t> &node)
{
    return node->lane.run ([&] { return framework_worker_executor_core (node); }).get ();
}

void configure_spot_execution (
  const std::shared_ptr<detail::spot_context_state_t> &state,
  const std::shared_ptr<runtime::offload_executor_t> &worker_executor,
  std::shared_ptr<runtime::serial_execution_queue_t> retained_queue = {})
{
    /* The queue owns turn ordering. Execution resources are shared by the
     * node; a Spot must not allocate its own worker pool. A yielded turn is
     * resumed through this same queue, so it does not require a Spot-local
     * executor. */
    state->serial_executor = worker_executor;
    state->serial_queue =
      retained_queue ? std::move (retained_queue)
                     : std::make_shared<runtime::serial_execution_queue_t> (
                         *state->serial_executor, runtime::serial_execution_queue_options_t{},
                         runtime::serial_execution_queue_t::error_handler_t{},
                         state->is_entry_spot () ? runtime::serial_lane_policy_t::entry_spot ()
                         : state->execution_mode == user_spot_execution_mode_t::spot_wide
                           ? runtime::serial_lane_policy_t::spot_wide ()
                           : runtime::serial_lane_policy_t::per_actor_spot ());
    state->spot_serial_executor = std::make_shared<detail::spot_serial_executor_t> (
      worker_executor, state->node->lane,
      state->is_entry_spot () ? runtime::serial_lane_policy_t::entry_spot ()
      : state->execution_mode == user_spot_execution_mode_t::spot_wide
        ? runtime::serial_lane_policy_t::spot_wide ()
        : runtime::serial_lane_policy_t::per_actor_spot (),
      state->serial_queue,
      state->node->location_lifecycle
        ? state->node->location_lifecycle->execution_runtime ().execution_policy ()
        : runtime::serial_execution_queue_t::execution_admission_t{});
    state->worker_scheduler = std::make_shared<spot_worker_scheduler_t> (worker_executor, state);
}

zlink::message_t encode_spot_publish_frame (std::string channel_name,
                                            std::string packet_name,
                                            const std::string &topic,
                                            std::string content_type,
                                            const zlink::message_t &payload)
{
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::publish;
    header.channel_name = std::move (channel_name);
    header.message_name = std::move (packet_name);
    header.content_type = std::move (content_type);
    header.topic = topic;
    header.source = header.channel_name;

    const auto header_bytes =
      runtime::messaging::envelope_codec_t{}.encode_header (header).to_bytes ();
    const auto body_bytes = payload.to_bytes ();
    std::vector<std::uint8_t> frame;
    frame.reserve (8 + header_bytes.size () + body_bytes.size ());
    frame.push_back (static_cast<std::uint8_t> ('Z'));
    frame.push_back (static_cast<std::uint8_t> ('L'));
    frame.push_back (static_cast<std::uint8_t> ('F'));
    frame.push_back (static_cast<std::uint8_t> ('E'));
    const auto header_size = static_cast<std::uint32_t> (header_bytes.size ());
    frame.push_back (static_cast<std::uint8_t> (header_size >> 24));
    frame.push_back (static_cast<std::uint8_t> (header_size >> 16));
    frame.push_back (static_cast<std::uint8_t> (header_size >> 8));
    frame.push_back (static_cast<std::uint8_t> (header_size));
    frame.insert (frame.end (), header_bytes.begin (), header_bytes.end ());
    frame.insert (frame.end (), body_bytes.begin (), body_bytes.end ());
    return zlink::message_t::from (frame);
}

spot_location_t make_spot_location (const detail::spot_node_builder_state_t &state,
                                    const std::string &spot_name,
                                    const spot_id_t &spot_id)
{
    const auto kind = state.snapshot.entry_spot_name && *state.snapshot.entry_spot_name == spot_name
                        ? zlink::spot_kind::entry
                        : zlink::spot_kind::user;
    return spot_location_t{
      .mesh_name = state.snapshot.name,
      .spot_id = spot_id,
      .spot_type = spot_name,
      .node_rid = zlink::routing_id_t::from (detail::effective_spot_node_rid (state.snapshot)),
      .spot_kind = kind,
      .route_endpoint = state.snapshot.router_bind_endpoint};
}
} // namespace

namespace detail
{

std::shared_ptr<runtime::offload_executor_t>
framework_worker_executor_core (const std::shared_ptr<detail::spot_node_builder_state_t> &node)
{
    if (node && node->worker_executor) {
        return node->worker_executor;
    }
    const auto &options = node->worker_options;
    auto executor = std::make_shared<runtime::offload_executor_t> (
      options.min_threads (), options.max_threads (), options.idle_timeout (), "zlink-spot-wrk");
    if (node) {
        node->worker_executor = executor;
    }
    return executor;
}

std::shared_ptr<runtime::runtime_failure_collector_t>
spot_node_builder_state_t::runtime_failures () const
{
    return runtime_failures_for (
      monitoring ? monitoring : (channel_runtime ? channel_runtime->monitoring : nullptr));
}

spot_node_builder_state_t::~spot_node_builder_state_t () = default;

std::uint64_t spot_generation (const detail::spot_context_state_t &context)
{
    if (const auto native = context.native_spot.lock ()) {
        return native->status ().lifecycle_generation ();
    }
    return 0;
}

task_t<std::optional<std::uint64_t>>
resolve_target_spot_generation (std::shared_ptr<detail::spot_node_builder_state_t> state,
                                zlink::routing_id_t target_node_rid,
                                spot_id_t target_spot_id)
{
    if (!state) {
        co_return std::nullopt;
    }
    std::shared_ptr<runtime::spot_address_resolver_t> resolver;
    std::string mesh_name;
    std::shared_ptr<service::spot_t> local_spot;
    state->lane.run_sync ([&] {
        mesh_name = state->snapshot.name;
        resolver = state->spot_location_resolver;
        const auto local = state->native_spots_by_id.find (target_spot_id);
        if (local != state->native_spots_by_id.end ()
            && detail::effective_spot_node_rid (state->snapshot) == target_node_rid.to_string ()) {
            local_spot = local->second;
        }
    });
    if (local_spot) {
        const auto generation = local_spot->status ().lifecycle_generation ();
        if (generation != 0) {
            co_return generation;
        }
    }
    if (!resolver) {
        co_return std::nullopt;
    }
    const auto address =
      co_await resolver->resolve_spot_address (std::move (mesh_name), target_spot_id);
    if (!address || address->node_rid != target_node_rid || address->spot_generation == 0) {
        co_return std::nullopt;
    }
    co_return address->spot_generation;
}

void report_logical_multicast_failure (const std::shared_ptr<spot_node_builder_state_t> &state,
                                       std::string_view channel_name,
                                       std::string_view mesh_name,
                                       std::string_view topic,
                                       std::string_view target_rid,
                                       dispatch_error_reason_t reason) noexcept
{
    if (!state)
        return;
    try {
        dispatch_error_reporter_t (state->dispatch).report_lazy ([&] {
            return message_dispatch_error_event_t{
              .surface = dispatch_error_surface_t::spot_route,
              .message_kind = dispatch_message_kind_t::send,
              .reason = reason,
              .action = dispatch_error_action_t::drop,
              .channel_name =
                channel_name.empty () ? std::nullopt : std::optional<std::string> (channel_name),
              .topic = std::string (topic),
              .mesh_name =
                mesh_name.empty () ? std::nullopt : std::optional<std::string> (mesh_name),
              .target_rid =
                target_rid.empty () ? std::nullopt : std::optional<std::string> (target_rid)};
        });
    }
    catch (...) {
        // Diagnostics cannot change the already-completed publish result.
    }
}

spot_node_runtime_t::spot_node_runtime_t (std::shared_ptr<spot_node_builder_state_t> state) :
    _state (std::move (state))
{
}

spot_manager_t spot_node_runtime_t::manager () const
{
    return spot_manager_t (_state);
}

spot_node_runtime_t spot_node_runtime_t::from (const spot_node_builder_t &builder)
{
    return spot_node_runtime_t (builder._state);
}

std::optional<spot_node_runtime_t> spot_node_runtime_t::from (const zlink_builder_t &builder,
                                                              const std::string &spot_node_name)
{
    if (!builder._state)
        return std::nullopt;
    const auto found = builder._state->mesh_nodes.find (spot_node_name);
    if (found != builder._state->mesh_nodes.end () && found->second && found->second->spot_state) {
        return spot_node_runtime_t (found->second->spot_state);
    }
    return std::nullopt;
}

std::vector<spot_node_snapshot_t> spot_node_runtime_t::snapshots (const zlink_builder_t &builder)
{
    std::vector<spot_node_snapshot_t> result;
    if (!builder._state)
        return result;
    result.reserve (builder._state->mesh_nodes.size ());
    for (const auto &[_, registration] : builder._state->mesh_nodes) {
        if (!registration || !registration->spot_state)
            continue;
        result.push_back (registration->spot_state->lane
                            .run ([state = registration->spot_state] { return state->snapshot; })
                            .get ());
    }
    return result;
}

local_spot_create_result_t spot_node_runtime_t::create_spot (std::string spot_name)
{
    return create_spot (std::move (spot_name), zlink::message_t{});
}

task_t<std::shared_ptr<local_spot_create_result_t>> spot_node_runtime_t::create_spot_context (
  std::string spot_name,
  spot_id_t spot_id,
  zlink::message_t request,
  std::uint64_t object_generation,
  std::string mesh_name,
  std::function<task_t<void> (void *)> staged_restore,
  std::uint64_t authority_owner_generation,
  std::shared_ptr<runtime::serial_execution_queue_t> retained_queue,
  std::function<task_t<void> (bool)> *preparation_terminal,
  std::shared_ptr<spot_serial_executor_t::preparation_t> inherited_operation)
{
    auto context_state = std::make_shared<spot_context_state_t> ();
    const auto id_value = std::string (spot_id);
    struct creation_plan_t
    {
        spot_lifecycle_callbacks_t lifecycle;
        std::optional<service_provider_t> root_services;
        std::shared_ptr<runtime::offload_executor_t> worker_executor;
        runtime::location_lifecycle_t *location_lifecycle = nullptr;
        std::shared_ptr<monitoring_runtime_state_t> monitoring;
        std::optional<spot_location_t> location;
        bool entry_spot = false;
    };
    auto plan = co_await _state->lane.run_task ([&] {
        /* graceful-drain-handoff §4-2: a draining node blocks new spot creation.
         * Existing spots (and in-progress transfer commits) keep running. */
        if (_state->host_draining () || _state->stopping.load (std::memory_order_acquire)) {
            throw framework_exception_t (framework_error_kind_t::shutting_down,
                                         "spot node is draining and rejects new spot creation");
        }
        const auto found = _state->spot_factories.find (spot_name);
        if (found == _state->spot_factories.end ()) {
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "spot factory is not registered");
        }

        creation_plan_t result;
        if (const auto lifecycle = _state->spot_lifecycles.find (spot_name);
            lifecycle != _state->spot_lifecycles.end ()) {
            result.lifecycle = lifecycle->second;
        }
        const auto instance_spot =
          std::find (_state->snapshot.instance_spot_names.begin (),
                     _state->snapshot.instance_spot_names.end (), spot_name)
          != _state->snapshot.instance_spot_names.end ();
        result.entry_spot =
          _state->snapshot.entry_spot_name && *_state->snapshot.entry_spot_name == spot_name;
        result.root_services = _state->root_services;
        result.worker_executor = framework_worker_executor_core (_state);
        result.location_lifecycle = _state->location_lifecycle;
        result.monitoring = _state->monitoring;
        if (result.location_lifecycle)
            result.location.emplace (make_spot_location (*_state, spot_name, spot_id));

        context_state->node = _state;
        context_state->lane_owner = _state;
        context_state->channel_runtime = _state->channel_runtime;
        context_state->node_rid =
          node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot));
        context_state->mesh_name =
          mesh_name.empty () ? _state->snapshot.name : std::move (mesh_name);
        context_state->spot_id = spot_id;
        context_state->object_generation = object_generation;
        context_state->authority_owner_generation = authority_owner_generation;
        context_state->spot_name = spot_name;
        context_state->lifecycle_domain = result.entry_spot ? spot_lifecycle_domain_t::entry ()
                                          : instance_spot   ? spot_lifecycle_domain_t::instance ()
                                                            : spot_lifecycle_domain_t::user ();
        if (const auto mode = _state->snapshot.spot_execution_modes.find (spot_name);
            mode != _state->snapshot.spot_execution_modes.end ()) {
            context_state->execution_mode = mode->second;
        }
        if (const auto mode = _state->spot_relocation_coordination_modes.find (spot_name);
            mode != _state->spot_relocation_coordination_modes.end ()) {
            context_state->relocation_coordination_mode = mode->second;
        }
        context_state->lifecycle = result.lifecycle;
        configure_spot_execution (context_state, result.worker_executor, retained_queue);
        if (const auto pending = _state->pending_spot_creations_by_id.find (id_value);
            pending != _state->pending_spot_creations_by_id.end ())
            pending->second.context = context_state;
        return result;
    });

    // MeshNode §5.1: creating a User Spot or cold-activating an Instance Spot holds one
    // activation admission until this activation ends. The Entry Spot never enters, and a
    // relocation target's Restore holds its admission from restore_spot_relocation_state.
    const auto activation_admission =
      plan.entry_spot || staged_restore
        ? std::shared_ptr<void>{}
        : _state->activation_admission->enter_scoped (
            activation_admission_t::spot_key (std::string (spot_id)));

    context_state->last_application_work_completed_ns.store (
      std::chrono::duration_cast<std::chrono::nanoseconds> (
        std::chrono::steady_clock::now ().time_since_epoch ())
        .count (),
      std::memory_order_relaxed);
    if (plan.root_services) {
        context_state->activation_scope = std::make_shared<service_scope_t> (
          service_scope_t::create (*plan.root_services, context_state->is_entry_spot ()
                                                          ? service_scope_kind_t::entry_spot
                                                          : service_scope_kind_t::spot_activation));
    }
    spot_context_t context (context_state);
    std::optional<message_t> create_reply;
    std::shared_ptr<service::spot_t> staged_native;
    // Barrier cancellation can settle this caller while the creation turn still runs, so
    // the turn's coroutine frame owns its inputs and outputs; parameters live in the frame,
    // unlike lambda captures. Keep the turn alive until asynchronous preparation
    // cleanup completes, including the staged native Spot on failed creation.
    struct creation_turn_t
    {
        spot_lifecycle_callbacks_t lifecycle;
        zlink::message_t request;
        std::function<task_t<void> (void *)> staged_restore;
        spot_create_response_t response = spot_create_response_t::accept ();
        std::optional<message_t> create_reply;
        std::shared_ptr<service::spot_t> staged_native;
    };
    const auto turn = std::make_shared<creation_turn_t> (
      creation_turn_t{plan.lifecycle, std::move (request), staged_restore});

    const auto preparation = inherited_operation
                               ? inherited_operation
                               : context_state->spot_serial_executor->begin_preparation ();
    const auto abort = [&] {
        if (inherited_operation)
            return preparation->lifecycle->run (
              [context_state, native = context_state->native_spot.lock ()] {
                  return context_state->discard_preparation (native);
              });
        return context_state->complete_preparation (false);
    };
    std::exception_ptr creation_failure;
    std::optional<local_spot_create_result_t> created;
    try {
        // The barrier may keep this callable after the turn, so it holds only weak references
        // and the running frame alone owns the turn; a failed creation then releases the
        // staged native Spot when this caller returns.
        do {
            co_await preparation->lifecycle->run ([weak_turn = std::weak_ptr (turn),
                                                   weak_preparation = std::weak_ptr (preparation),
                                                   inherited =
                                                     static_cast<bool> (inherited_operation),
                                                   weak_context = std::weak_ptr (context_state)] {
                auto turn = weak_turn.lock ();
                auto preparation = weak_preparation.lock ();
                auto context_state = weak_context.lock ();
                if (!turn || !preparation || !context_state)
                    return task_t<void> (result_t<void>::failure (
                      framework_error_kind_t::shutting_down, "Spot creation caller has returned"));
                return [] (std::shared_ptr<creation_turn_t> turn,
                           std::shared_ptr<spot_serial_executor_t::preparation_t> preparation,
                           bool inherited,
                           std::shared_ptr<spot_context_state_t> context_state) -> task_t<void> {
                    auto &plan = *turn;
                    auto &request = turn->request;
                    auto &staged_restore = turn->staged_restore;
                    auto &response = turn->response;
                    auto &create_reply = turn->create_reply;
                    auto &staged_native = turn->staged_native;
                    if (inherited)
                        context_state->spot_serial_executor->attach_preparation (preparation);
                    if (plan.lifecycle.create_entry_context_instance
                        || plan.lifecycle.create_instance_context_instance
                        || plan.lifecycle.create_spot_context_instance) {
                        service_provider_t empty_services;
                        auto &activation_services = context_state->activation_scope
                                                      ? context_state->activation_scope->provider ()
                                                      : empty_services;
                        if (plan.lifecycle.create_entry_context_instance) {
                            context_state->spot_instance =
                              plan.lifecycle.create_entry_context_instance (
                                entry_spot_context_t (context_state), activation_services);
                        } else if (plan.lifecycle.create_instance_context_instance) {
                            context_state->spot_instance =
                              plan.lifecycle.create_instance_context_instance (
                                instance_spot_context_t (context_state), activation_services);
                        } else {
                            context_state->spot_instance =
                              plan.lifecycle.create_spot_context_instance (
                                spot_context_t (context_state), activation_services);
                        }
                        if (!context_state->spot_instance)
                            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                                         "SPOT factory returned null");
                    }
                    if (context_state->spot_instance) {
                        if (staged_restore) {
                            co_await staged_restore (context_state->spot_instance.get ());
                        } else {
                            staged_native = attach_native_spot (context_state, false, false);
                            if (!context_state->is_instance_spot () && plan.lifecycle.on_create)
                                response = co_await plan.lifecycle.on_create (
                                  context_state->spot_instance.get (), request,
                                  *context_state->channel_runtime->serializers);
                        }
                        if (!response.accepted)
                            co_return;
                        create_reply = response.reply;
                        if (!staged_restore && plan.lifecycle.on_initialize)
                            co_await plan.lifecycle.on_initialize (
                              context_state->spot_instance.get ());
                        if (staged_restore)
                            staged_native = attach_native_spot (context_state, true, false);
                    } else {
                        staged_native = attach_native_spot (context_state, false, false);
                    }
                    co_return;
                }(std::move (turn), std::move (preparation), inherited, std::move (context_state));
            });
            auto &response = turn->response;
            create_reply = turn->create_reply;
            staged_native = turn->staged_native;
            if (!response.accepted) {
                created.emplace (local_spot_create_result_t{spot_id, spot_create_state_t::rejected,
                                                            response.reply, std::move (context)});
                break;
            }

            bool location_claimed = false;
            if (plan.location_lifecycle && plan.location) {
                auto location = std::move (*plan.location);
                if (const auto native = context_state->native_spot.lock ())
                    location.spot_generation = native->status ().lifecycle_generation ();
                const auto claimed =
                  co_await plan.location_lifecycle->claim_spot (std::move (location));
                if (claimed.status != location_write_status_t::stored) {
                    created.emplace (local_spot_create_result_t{
                      spot_id, spot_create_state_t::rejected, std::nullopt, std::move (context)});
                    break;
                }
                location_claimed = true;
            }

            const auto native = staged_native ? staged_native : context_state->native_spot.lock ();
            const auto published = co_await _state->lane.run_task ([&] {
                const auto previous = _state->spot_contexts_by_id.find (id_value);
                if ((previous != _state->spot_contexts_by_id.end ()
                     && (!retained_queue || !previous->second._state->closed))
                    || (_state->native_spots_by_id.contains (id_value)
                        && _state->native_spots_by_id.at (id_value) != native)) {
                    return false;
                }
                _state->spot_contexts_by_id.erase (id_value);
                _state->spot_contexts_by_id.emplace (id_value, spot_context_t (context_state));
                _state->spot_ids_by_name[spot_name] = spot_id;
                _state->spot_names_by_id[id_value] = spot_name;
                if (native)
                    _state->native_spots_by_id[id_value] = native;
                return true;
            });
            if (!published) {
                if (location_claimed && plan.location_lifecycle)
                    (void) co_await plan.location_lifecycle->release_spot_async (
                      spot_location_key_t{id_value});
                created.emplace (local_spot_create_result_t{spot_id, spot_create_state_t::rejected,
                                                            std::nullopt, std::move (context)});
                break;
            }

            if (plan.monitoring) {
                runtime::runtime_metrics_t metrics (plan.monitoring);
                if (metrics.enabled ()) {
                    const auto kind = plan.entry_spot                      ? "entry"
                                      : context_state->is_instance_spot () ? "instance"
                                                                           : "user";

                    metrics.updown ("zlink.spot.count", "{spot}", 1,
                                    {{"mesh_name", _state->snapshot.name}, {"spot_kind", kind}});
                }
            }
            if (!inherited_operation) {
                if (preparation_terminal)
                    *preparation_terminal = [context_state] (bool ready) {
                        return context_state->complete_preparation (ready);
                    };
                else if (!staged_restore)
                    context_state->complete_preparation (true);
            }
            created.emplace (local_spot_create_result_t{spot_id, spot_create_state_t::created,
                                                        create_reply, std::move (context)});
        } while (false);
    }
    catch (...) {
        creation_failure = std::current_exception ();
    }
    if (!created || created->state != spot_create_state_t::created) {
        try {
            co_await abort ();
        }
        catch (...) {
            report_spot_close_diagnostic (_state, std::string (context_state->spot_id),
                                          "preparation_terminal_failed", std::current_exception ());
        }
    }
    if (creation_failure)
        std::rethrow_exception (creation_failure);
    co_return std::make_shared<local_spot_create_result_t> (std::move (*created));
}

local_spot_create_result_t spot_node_runtime_t::create_spot (std::string spot_name,
                                                             zlink::message_t request)
{
    auto spot_id =
      _state->lane
        .run ([&] {
            const auto is_entry_spot =
              _state->snapshot.entry_spot_name && *_state->snapshot.entry_spot_name == spot_name;
            return is_entry_spot ? detail::new_entry_spot_id (_state->snapshot.name)
                                 : detail::new_user_spot_id ();
        })
        .get ();
    return get_or_create_spot (std::move (spot_name), std::move (spot_id), std::move (request));
}

local_spot_create_result_t spot_node_runtime_t::get_or_create_spot (std::string spot_name,
                                                                    spot_id_t spot_id)
{
    return get_or_create_spot (std::move (spot_name), std::move (spot_id), zlink::message_t{});
}

local_spot_create_result_t
spot_node_runtime_t::get_or_create_spot (std::string spot_name,
                                         spot_id_t spot_id,
                                         zlink::message_t request,
                                         std::uint64_t object_generation,
                                         std::string mesh_name,
                                         std::uint64_t authority_owner_generation,
                                         std::function<task_t<void> (bool)> *preparation_terminal)
{
    auto creation =
      get_or_create_spot_async (std::move (spot_name), std::move (spot_id), std::move (request),
                                object_generation, std::move (mesh_name),
                                authority_owner_generation, preparation_terminal)
        .result ()
        .value ();
    return std::move (*creation);
}

task_t<std::shared_ptr<local_spot_create_result_t>> spot_node_runtime_t::get_or_create_spot_async (
  std::string spot_name,
  spot_id_t spot_id,
  zlink::message_t request,
  std::uint64_t object_generation,
  std::string mesh_name,
  std::uint64_t authority_owner_generation,
  std::function<task_t<void> (bool)> *preparation_terminal)
{
    const auto state = _state;
    spot_node_runtime_t owner (state);
    const auto id_value = std::string (spot_id);
    struct creation_admission_t
    {
        std::shared_ptr<local_spot_create_result_t> existing;
        std::shared_ptr<task_completion_source_t<void>> pending;
        std::shared_ptr<task_completion_source_t<void>> completion;
        std::uint64_t reservation = 0;
    };
    auto admission = co_await state->lane.run_task ([&] {
        auto same_spot_type = [&] (const std::string &existing_name) {
            const auto existing_factory = state->spot_factories.find (existing_name);
            const auto requested_factory = state->spot_factories.find (spot_name);
            return existing_factory == state->spot_factories.end ()
                   || requested_factory == state->spot_factories.end ()
                   || existing_factory->second == requested_factory->second;
        };
        creation_admission_t result;
        if (const auto existing = state->spot_contexts_by_id.find (id_value);
            existing != state->spot_contexts_by_id.end ()) {
            const auto existing_name = state->spot_names_by_id.find (id_value);
            if (existing_name != state->spot_names_by_id.end ()
                && !same_spot_type (existing_name->second)) {
                throw framework_exception_t (framework_error_kind_t::type_mismatch,
                                             "spot id is already bound to a different spot type");
            }
            result.existing = std::make_shared<local_spot_create_result_t> (
              local_spot_create_result_t{spot_id, spot_create_state_t::existing, std::nullopt,
                                         spot_context_t (existing->second._state)});
            return result;
        }
        if (const auto pending = state->pending_spot_creations_by_id.find (id_value);
            pending != state->pending_spot_creations_by_id.end ()) {
            if (!same_spot_type (pending->second.spot_name)) {
                throw framework_exception_t (framework_error_kind_t::type_mismatch,
                                             "spot id is already bound to a different spot type");
            }
            result.pending = pending->second.completion;
            return result;
        }
        result.completion = std::make_shared<task_completion_source_t<void>> ();
        if (state->next_pending_spot_creation_reservation == 0) {
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "spot creation reservation sequence is exhausted");
        }
        result.reservation = state->next_pending_spot_creation_reservation++;
        const auto inserted = state->pending_spot_creations_by_id.emplace (
          id_value, detail::spot_node_builder_state_t::pending_spot_creation_t{
                      spot_name, result.completion, result.reservation});
        if (!inserted.second) {
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "spot creation reservation was not acquired");
        }
        return result;
    });

    if (admission.existing)
        co_return admission.existing;
    if (admission.pending) {
        co_await admission.pending->task ();
        co_return co_await state->lane.run_task ([&] {
            const auto created = state->spot_contexts_by_id.find (id_value);
            if (created == state->spot_contexts_by_id.end ()) {
                throw framework_exception_t (
                  framework_error_kind_t::internal_failure,
                  "concurrent spot creation completed without a context");
            }
            return std::make_shared<local_spot_create_result_t> (
              local_spot_create_result_t{spot_id_t (id_value), spot_create_state_t::existing,
                                         std::nullopt, spot_context_t (created->second._state)});
        });
    }

    std::exception_ptr failure;
    std::shared_ptr<local_spot_create_result_t> creation;
    try {
        creation = co_await owner.create_spot_context (
          std::move (spot_name), std::move (spot_id), std::move (request), object_generation,
          std::move (mesh_name), {}, authority_owner_generation, {}, preparation_terminal);
    }
    catch (...) {
        failure = std::current_exception ();
    }
    const auto owned = co_await state->lane.run_task ([&] {
        const auto found = state->pending_spot_creations_by_id.find (id_value);
        if (found == state->pending_spot_creations_by_id.end ()
            || found->second.reservation != admission.reservation)
            return false;
        state->pending_spot_creations_by_id.erase (found);
        return true;
    });
    if (failure) {
        if (owned)
            admission.completion->complete (detail::result_access_t::failure<void> (failure));
        std::rethrow_exception (failure);
    }
    if (!owned)
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "spot creation reservation ownership was lost");
    admission.completion->complete (result_t<void>::success ());
    co_return creation;
}

task_t<zlink::message_t> spot_node_runtime_t::dispatch_instance_activation (
  const spot_id_t &spot_id,
  std::string packet_name,
  std::string content_type,
  std::vector<std::uint8_t> payload,
  std::map<std::string, std::string> metadata,
  bool request,
  std::string correlation_id,
  service_provider_t &services,
  serializer_registry_t &serializers,
  std::function<void ()> accept_activation,
  std::optional<std::string> flow_id,
  std::optional<flow_origin_t> flow_origin,
  std::function<void ()> *accepted_turn_terminal,
  std::shared_ptr<const instance_spot_retained_message_t> retained_message)
{
    const auto received_at = std::chrono::system_clock::now ();
    if (accepted_turn_terminal)
        *accepted_turn_terminal = {};
    /* Instance Spot cold activation carries the first application message
     * outside the normal Spot route record. Keep it in the same diagnostic
     * stream as a Ready Spot dispatch so an operator can follow activation,
     * handler completion, and a reply in one file. The activation payload
     * carries the framework-owned flow pair when tracing is enabled; a target
     * creates a new inbound flow only when the source did not carry one. */
    const auto diagnostics_mode = detail::message_flow_tracer_t (_state->dispatch).mode ();
    auto flow_scope = runtime::flow_context_t::enter (
      std::move (flow_id), flow_origin, diagnostics_mode, flow_origin_t::inbound, std::nullopt);
    auto context = find_context (spot_id);
    const auto context_state = context ? context->_state : nullptr;
    const auto materialized =
      _state->lane
        .run ([&] {
            return context_state
                   && (context_state->spot_instance || (retained_message && context_state->closed))
                   && std::find (_state->snapshot.instance_spot_names.begin (),
                                 _state->snapshot.instance_spot_names.end (),
                                 context_state->spot_name)
                        != _state->snapshot.instance_spot_names.end ();
        })
        .get ();
    if (!materialized) {
        const auto error = detail::with_failure_code (
          detail::make_framework_origin_exception (
            framework_error_kind_t::unavailable,
            "Instance Spot activation target no longer holds its owner fence"),
          static_cast<std::uint32_t> (runtime::protocol::framework_error_code::spotMoving));
        return task_t<zlink::message_t> (
          detail::result_access_t::failure<zlink::message_t> (error));
    }
    auto state = context_state;
    std::function<void ()> release_accepted_turn;
    if (accepted_turn_terminal) {
        if (!state->admit (true, retained_message.get ())) {
            const auto error = state->admission_error ();
            return task_t<zlink::message_t> (
              detail::result_access_t::failure<zlink::message_t> (error));
        }
        const auto settled = std::make_shared<std::atomic_bool> (false);
        release_accepted_turn = [state, settled] {
            if (!settled->exchange (true, std::memory_order_acq_rel))
                state->leave_callback ();
        };
        *accepted_turn_terminal = release_accepted_turn;
    }
    const auto message_kind =
      request ? dispatch_message_kind_t::request : dispatch_message_kind_t::send;
    report_spot_dispatch_trace (_state, message_flow_outcome_t::received,
                                dispatch_error_surface_t::spot_route, message_kind, packet_name, {},
                                spot_id, {}, correlation_id);
    /* Copies exist only for the completion-time trace/error report; skip them
     * while tracing is fully off (spec 26 §4: no allocation before the gate). */
    const bool activation_trace_enabled =
      message_flow_tracer_t (_state->dispatch).capture_enabled ();
    std::string trace_packet_name;
    std::string trace_spot_id;
    std::string trace_correlation_id;
    if (activation_trace_enabled) {
        trace_packet_name = packet_name;
        trace_spot_id = std::string (spot_id);
        trace_correlation_id = correlation_id;
    }
    state->spot_serial_executor->admit_preparation_message (retained_message);
    auto handler_task = [&] {
        try {
            return spot_handler_registry_t (state).invoke_erased (
              [state, accept_activation = std::move (accept_activation)] {
                  accept_activation ();
                  state->spot_serial_executor->open_preparation_admission ();
              },
              spot_handler_kind_t::packet, packet_name, {}, std::type_index (typeid (void)),
              state->spot_instance.get (), nullptr, services, serializers,
              zlink::message_t::from (payload),
              spot_inbound_message_t{.content_type = std::move (content_type),
                                     .values = std::move (metadata),
                                     .mesh_name = state->mesh_name,
                                     .correlation_id = std::move (correlation_id),
                                     .received_at = received_at},
              true, {}, {}, {}, spot_handler_registry_t::actor_queue_dispatch_t::acquire, {}, {}, 0,
              {}, {}, retained_message);
        }
        catch (...) {
            if (release_accepted_turn)
                release_accepted_turn ();
            if (accepted_turn_terminal)
                *accepted_turn_terminal = {};
            throw;
        }
    }();
    detail::observe_task_completion (
      handler_task, [node = _state, state, request, message_kind, trace_packet_name, trace_spot_id,
                     trace_correlation_id] (const result_t<zlink::message_t> &result) {
          state->last_application_work_completed_ns.store (
            std::chrono::duration_cast<std::chrono::nanoseconds> (
              std::chrono::steady_clock::now ().time_since_epoch ())
              .count (),
            std::memory_order_relaxed);
          if (result) {
              report_spot_dispatch_trace (
                node,
                request ? message_flow_outcome_t::replied : message_flow_outcome_t::dispatched,
                dispatch_error_surface_t::spot_route,
                request ? dispatch_message_kind_t::response : message_kind, trace_packet_name, {},
                trace_spot_id, {}, trace_correlation_id);
              return;
          }
      });
    return handler_task;
}

std::optional<spot_info_t> spot_node_runtime_t::find_spot (spot_id_t spot_id) const
{
    if (const auto name = spot_name_for (spot_id)) {
        return spot_info_t{std::move (spot_id), *name};
    }
    return std::nullopt;
}

bool spot_node_runtime_t::matches_instance_activation (
  const std::string &spot_id,
  const std::string &stable_type,
  const std::string &mesh_name,
  std::uint64_t object_generation,
  std::uint64_t authority_owner_generation) const
{
    return _state->lane
      .run ([&] {
          const auto found = _state->spot_contexts_by_id.find (spot_id);
          if (found == _state->spot_contexts_by_id.end ())
              return false;
          const auto &local = *found->second._state;
          return local.node == _state && local.lifecycle_domain.is_instance ()
                 && local.spot_name == stable_type && local.mesh_name == mesh_name
                 && local.object_generation == object_generation
                 && local.authority_owner_generation == authority_owner_generation;
      })
      .get ();
}

std::vector<spot_info_t> spot_node_runtime_t::list_spots () const
{
    return _state->lane
      .run ([&] {
          std::vector<spot_info_t> spots;
          spots.reserve (_state->spot_names_by_id.size ());
          for (const auto &[rid, name] : _state->spot_names_by_id) {
              spots.push_back (spot_info_t{spot_id_t (rid), name});
          }
          return spots;
      })
      .get ();
}

task_t<bool> spot_node_runtime_t::close_spot (spot_id_t spot_id,
                                              spot_close_reason_t reason,
                                              std::chrono::system_clock::time_point deadline,
                                              std::stop_token cleanup_cancellation)
{
    struct close_plan_t
    {
        std::optional<spot_context_t> context;
    };
    auto plan = _state->lane
                  .run ([&] {
                      close_plan_t result;
                      const auto found = _state->spot_contexts_by_id.find (std::string (spot_id));
                      if (found != _state->spot_contexts_by_id.end ()) {
                          result.context.emplace (spot_context_t (found->second._state));
                      }
                      return result;
                  })
                  .get ();
    if (!plan.context) {
        return task_t<bool> (result_t<bool>::success (false));
    }
    auto completion = std::make_shared<task_completion_source_t<bool>> ();
    auto closed = completion->task ();
    plan.context->_state->request_close (
      {}, [completion] (result_t<bool> result) { completion->complete (std::move (result)); },
      reason, deadline, cleanup_cancellation);
    return closed;
}

void spot_node_runtime_t::close_user_spot_owner (const std::string &spot_id,
                                                 std::uint64_t object_generation,
                                                 std::chrono::system_clock::time_point deadline,
                                                 service::spot_close_begin_t begin,
                                                 service::spot_close_done_t done)
{
    auto state = _state->lane
                   .run ([&] {
                       const auto found = _state->spot_contexts_by_id.find (spot_id);
                       return found == _state->spot_contexts_by_id.end () || !found->second._state
                                  || found->second._state->object_generation != object_generation
                                ? std::shared_ptr<spot_context_state_t>{}
                                : found->second._state;
                   })
                   .get ();
    if (state) {
        state->request_close (std::move (begin), std::move (done),
                              spot_close_reason_t::explicit_close, deadline);
        return;
    }
    // 해당 generation의 local activation이 없으면 기존 authority owner가
    // Missing·다른 generation·owner fence를 분류합니다.
    service::run_authority_close (std::move (begin), std::move (done), {});
}

bool spot_node_runtime_t::close_all_spots (std::chrono::steady_clock::time_point deadline_at,
                                           std::chrono::system_clock::time_point closing_deadline,
                                           std::stop_source &cleanup_cancellation)
{
    const auto remaining = [deadline_at] {
        return std::max (std::chrono::milliseconds::zero (),
                         std::chrono::ceil<std::chrono::milliseconds> (
                           deadline_at - std::chrono::steady_clock::now ()));
    };
    // Host relocation §15: Draining finishes accepted operations. A Join stays
    // moving until its completion ordering ends (Spot-Actor membership §4), so
    // the shutdown Close retires its Actor only after that move closes.
    const auto moves_closed =
      _state->actor_transfer_coordinator->wait_for_source_completions ().result_for (remaining ());
    if (!moves_closed || !*moves_closed)
        return false;
    std::vector<spot_id_t> spots;
    std::vector<task_t<bool>> running_closes;
    _state->lane
      .run ([&] {
          spots.reserve (_state->spot_contexts_by_id.size ());
          for (const auto &[rid, context] : _state->spot_contexts_by_id) {
              if (!context._state)
                  continue;
              // A Close already running releases its authority before the
              // owner cleanup (Host relocation §14). Shutdown waits for it.
              auto running = std::make_shared<task_completion_source_t<bool>> ();
              if (context._state->merge_into_running_close_core ([running] (result_t<bool> result) {
                      running->complete (std::move (result));
                  })) {
                  running_closes.push_back (running->task ());
                  continue;
              }
              if (context._state->closed || context._state->native_spot.expired ())
                  continue;
              spots.push_back (spot_id_t (rid));
          }
      })
      .get ();
    const auto wait_for_close = [&remaining, &cleanup_cancellation] (const task_t<bool> &closing) {
        const auto result = closing.result_for (remaining ());
        // Host shutdown §14 observes accepted Close completion before local teardown.
        if (!result) {
            cleanup_cancellation.request_stop ();
            static_cast<void> (detail::task_result_access_t::read (closing));
        }
        return result && result->value ();
    };
    // Every Spot enters its existing Close gate before the host observes the
    // shared deadline. Entry closing must precede owner and transport teardown.
    for (auto &spot_id : spots)
        running_closes.push_back (close_spot (std::move (spot_id),
                                              spot_close_reason_t::host_shutdown, closing_deadline,
                                              cleanup_cancellation.get_token ()));
    return static_cast<std::size_t> (std::ranges::count_if (running_closes, wait_for_close))
             == running_closes.size ()
           && active_user_spot_count () == 0;
}

node_rid_t spot_node_runtime_t::node_rid () const
{
    return _state->lane
      .run ([&] {
          return node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot));
      })
      .get ();
}

std::optional<std::string> spot_node_runtime_t::spot_name_for (spot_id_t spot_id) const
{
    return _state->lane.run ([&] { return spot_name_for_unlocked (spot_id); }).get ();
}

std::optional<std::string>
spot_node_runtime_t::spot_name_for_unlocked (const spot_id_t &spot_id) const
{
    const auto found = _state->spot_names_by_id.find (std::string (spot_id));
    if (found == _state->spot_names_by_id.end ()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<spot_route_t> spot_node_runtime_t::resolve_spot (spot_id_t spot_id) const
{
    struct resolution_plan_t
    {
        std::optional<spot_route_t> local;
        std::vector<std::function<std::optional<spot_route_t> (spot_id_t)>> resolvers;
        std::shared_ptr<runtime::spot_address_resolver_t> location_resolver;
        std::string mesh_name;
    };
    auto plan =
      _state->lane
        .run ([&] {
            resolution_plan_t result;
            const auto found = _state->spot_names_by_id.find (std::string (spot_id));
            if (found != _state->spot_names_by_id.end ()) {
                result.local = spot_route_t{
                  node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot)),
                  spot_id, found->second};
                return result;
            }
            result.resolvers.reserve (_state->resolvers.size ());
            for (const auto &[_, resolver] : _state->resolvers)
                result.resolvers.push_back (resolver);
            result.location_resolver = _state->spot_location_resolver;
            result.mesh_name = _state->snapshot.name;
            return result;
        })
        .get ();
    if (plan.local)
        return std::move (plan.local);
    for (const auto &resolver : plan.resolvers) {
        if (auto route = resolver (spot_id)) {
            return route;
        }
    }
    if (plan.location_resolver) {
        const auto address = runtime::infrastructure_result ([&] {
                                 return plan.location_resolver->resolve_spot_address (
                                   std::move (plan.mesh_name), spot_id);
                             }).value ();
        if (address) {
            return spot_route_t{node_rid_t::from_string (address->node_rid.to_string ()),
                                spot_id_t (address->spot_id),
                                {}};
        }
    }
    return std::nullopt;
}

void spot_node_runtime_t::cancel_pending_work (
  std::chrono::steady_clock::time_point deadline) noexcept
{
    try {
        detail::drain_spot_node_executors (*_state, deadline);
    }
    catch (...) {
    }
}

void spot_node_runtime_t::release_native_handles () noexcept
{
    std::vector<std::shared_ptr<detail::spot_context_state_t>> contexts;
    try {
        const auto shutdown = _state->lane.run_sync ([&] {
            contexts.reserve (_state->spot_contexts_by_id.size ());
            for (auto &[_, context] : _state->spot_contexts_by_id) {
                if (context._state) {
                    context._state->closed = spot_close_reason_t::host_shutdown;
                    contexts.push_back (context._state);
                }
            }
            _state->spot_contexts_by_id.clear ();
            _state->spot_ids_by_name.clear ();
            _state->spot_names_by_id.clear ();
            _state->native_actors.clear ();
            _state->native_spots_by_id.clear ();
            _state->routed_control_spot.reset ();
            return _state->host_lifecycle ? _state->host_lifecycle () : host_lifecycle_snapshot_t{};
        });
        for (auto &context : contexts) {
            try {
                context->detach_application_instance (
                  true, spot_close_reason_t::host_shutdown,
                  shutdown.shutdown_deadline.value_or (
                    std::chrono::system_clock::time_point::max ()),
                  shutdown.cleanup_cancellation);
            }
            catch (...) {
                report_spot_close_diagnostic (_state, std::string (context->spot_id),
                                              "on_closing_failed", std::current_exception ());
            }
        }
        _state->lane.run_sync ([&] {
            std::atomic_store_explicit (&_state->route_client,
                                        std::shared_ptr<const route_client_t>{},
                                        std::memory_order_release);
            _state->channel_runtime.reset ();
            _state->root_services.reset ();
        });
    }
    catch (...) {
    }
}

void spot_node_runtime_t::request_stop () noexcept
{
    _state->stopping.store (true, std::memory_order_release);
}

void spot_node_runtime_t::bind_service_provider (service_provider_t &services)
{
    _state->lane.run_sync ([&] { _state->root_services = services; });
}

bool spot_node_runtime_t::stopping () const noexcept
{
    return _state->stopping.load (std::memory_order_acquire);
}

void spot_node_runtime_t::cancel_dispatch_waits () noexcept
{
    detail::visit_spot_node_dispatch_queues (*_state, [] (auto &queue) { queue.cancel_waits (); });
}

void spot_node_runtime_t::cancel_pending_dispatch () noexcept
{
    detail::visit_spot_node_dispatch_queues (*_state,
                                             [] (auto &queue) { queue.cancel_pending (); });
}

void spot_node_runtime_t::cancel_timers () noexcept
{
    try {
        const auto contexts = _state->lane.run_sync ([&] {
            std::vector<std::shared_ptr<spot_context_state_t>> result;
            result.reserve (_state->spot_contexts_by_id.size ());
            for (const auto &[_, context] : _state->spot_contexts_by_id) {
                if (context._state)
                    result.push_back (context._state);
            }
            return result;
        });
        for (const auto &context : contexts)
            detail::timer_runtime_t (context).cancel_all ();
    }
    catch (...) {
    }
}

void spot_node_runtime_t::attach_native_node (std::shared_ptr<service::mesh_node_t> node)
{
    struct attach_plan_t
    {
        std::vector<std::shared_ptr<spot_context_state_t>> contexts;
        std::chrono::milliseconds idle_timeout{0};
        bool create_control_spot = false;
        bool create_idle_timer = false;
    };
    const auto native = node;
    _state->actor_transfer_coordinator = node->objects ().actor_transfers ();
    _state->actor_transfer_coordinator->set_activity_handler (
      [weak = std::weak_ptr<service::mesh_node_t> (node)] {
          if (const auto host = weak.lock ())
              host->signal_dispatch_activity ();
      });
    const auto plan = _state->lane.run_sync ([&] {
        attach_plan_t result;
        _state->stopping.store (false, std::memory_order_release);
        _state->worker_cancellation = std::stop_source{};
        _state->native_node = node;
        if (_state->node_rid.empty ())
            _state->node_rid =
              node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot));
        result.create_control_spot = _state->spot_contexts_by_id.empty ();
        result.contexts.reserve (_state->spot_contexts_by_id.size ());
        for (const auto &[_, context] : _state->spot_contexts_by_id) {
            if (context._state)
                result.contexts.push_back (context._state);
        }
        result.idle_timeout = _state->instance_spot_idle_timeout;
        result.create_idle_timer = result.idle_timeout > std::chrono::milliseconds::zero ()
                                   && !_state->instance_spot_idle_timer;
        return result;
    });
    if (plan.create_control_spot && native) {
        auto control = std::make_shared<service::spot_t> (native->entry_spot ());
        _state->lane.run_sync ([&] {
            if (_state->spot_contexts_by_id.empty () && !_state->routed_control_spot)
                _state->routed_control_spot = std::move (control);
        });
    }
    for (const auto &context : plan.contexts)
        attach_native_spot (context);
    if (plan.create_idle_timer) {
        auto weak_state = std::weak_ptr<spot_node_builder_state_t> (_state);
        auto timer =
          std::make_unique<detail::core_timer_drain_loop_t> (_state->runtime_failures ());
        timer->start (plan.idle_timeout, std::numeric_limits<std::uint64_t>::max (),
                      [weak_state] (std::uint64_t) {
                          if (auto state = weak_state.lock ()) {
                              if (!state->stopping.load (std::memory_order_acquire))
                                  spot_node_runtime_t (std::move (state)).evict_idle_spots ();
                          }
                      });
        _state->lane.run_sync ([&] {
            if (!_state->instance_spot_idle_timer)
                _state->instance_spot_idle_timer = std::move (timer);
        });
    }
}

void spot_node_runtime_t::detach_native_node ()
{
    detail::core_timer_drain_loop_t *idle_timer = nullptr;
    auto native_spots = _state->lane.run_sync ([&] {
        std::vector<std::shared_ptr<service::spot_t>> result;
        idle_timer = _state->instance_spot_idle_timer.get ();
        result.reserve (_state->native_spots_by_id.size ());
        for (const auto &[_, native] : _state->native_spots_by_id) {
            if (native)
                result.push_back (native);
        }
        return result;
    });
    if (idle_timer)
        idle_timer->close ();

    std::exception_ptr close_error;
    for (const auto &native : native_spots) {
        try {
            native->close ();
        }
        catch (...) {
            if (!close_error) {
                close_error = std::current_exception ();
            }
        }
    }
    if (close_error) {
        std::rethrow_exception (close_error);
    }
    _state->lane.run_sync ([&] {
        _state->instance_spot_idle_timer.reset ();
        _state->native_node.reset ();
        _state->native_spots_by_id.clear ();
        _state->routed_control_spot.reset ();
        for (auto &[_, context] : _state->spot_contexts_by_id)
            context._state->native_spot.reset ();
    });
}

void spot_node_runtime_t::evict_idle_spots () noexcept
{
    try {
        auto [idle_timeout, admit_eviction, initial_candidates] =
          _state->lane
            .run ([&] {
                std::vector<std::shared_ptr<spot_context_state_t>> states;
                states.reserve (_state->spot_contexts_by_id.size ());
                for (const auto &[_, context] : _state->spot_contexts_by_id) {
                    if (context._state)
                        states.push_back (context._state);
                }
                return std::make_tuple (_state->instance_spot_idle_timeout,
                                        _state->admit_instance_spot_idle_eviction,
                                        std::move (states));
            })
            .get ();
        if (idle_timeout <= std::chrono::milliseconds::zero () || !admit_eviction
            || _state->stopping.load (std::memory_order_acquire))
            return;

        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds> (
                              std::chrono::steady_clock::now ().time_since_epoch ())
                              .count ();
        const auto timeout_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds> (idle_timeout).count ();
        std::vector<std::pair<std::shared_ptr<spot_context_state_t>, std::uint64_t>> candidates;
        for (const auto &state : initial_candidates) {
            const auto queue = state->serial_queue;
            if (!queue || queue->pending_count (runtime::serial_work_lane_t::application) != 0
                || queue->pending_count (runtime::serial_work_lane_t::lifecycle) != 0)
                continue;
            bool timer_busy = false;
            for (const auto &timer : state->timers) {
                if (!timer)
                    continue;
                std::lock_guard timer_lock (timer->mutex);
                if (!timer->disposed
                    && (timer->running || timer->pending_fire || timer->pending_fire_count != 0)) {
                    timer_busy = true;
                    break;
                }
            }
            if (timer_busy)
                continue;
            const auto last_ns =
              state->last_application_work_completed_ns.load (std::memory_order_relaxed);
            if (last_ns <= 0 || now_ns < last_ns || now_ns - last_ns < timeout_ns)
                continue;
            const auto reservation =
              _state->lane
                .run ([&] {
                    const auto found =
                      _state->spot_contexts_by_id.find (std::string (state->spot_id));
                    const auto last =
                      state->last_application_work_completed_ns.load (std::memory_order_relaxed);
                    const bool accepted =
                      found != _state->spot_contexts_by_id.end () && found->second._state == state
                      && !state->closed && state->is_instance_spot () && state->spot_instance
                      && state->actor_count == 0 && !state->relocation_boundary_active
                      && !state->relocation_ready_deferred && state->queued_routed_packets.empty ()
                      && state->authority_owner_generation != 0
                      && !_state->pending_spot_creations_by_id.contains (
                        std::string (state->spot_id))
                      && last > 0 && now_ns >= last && now_ns - last >= timeout_ns
                      && state->close_reservation == 0 && !state->admission_sealed
                      && state->callback_depth == 0;
                    if (!accepted)
                        return std::uint64_t{0};
                    /* Seal and reserve in this one node-owner turn. Admitted
                 * work either precedes this point and keeps callback_depth
                 * non-zero, or observes the seal and is refused (admit). */
                    const auto reservation = state->begin_idle_close_reservation ();
                    if (reservation == 0)
                        return std::uint64_t{0};
                    state->admission_sealed = true;
                    return reservation;
                })
                .get ();
            if (reservation == 0)
                continue;
            candidates.emplace_back (state, reservation);
        }

        for (const auto &[state, reservation] : candidates) {
            // Quiescence and age are re-checked before any authority write
            // (Object lifecycle §5); after that the local close cannot refuse.
            bool evicted = false;
            try {
                evicted =
                  state->idle_close_ready (reservation)
                  && admit_eviction (state->spot_id, state->spot_name, state->object_generation,
                                     state->authority_owner_generation, [state, reservation] {
                                         state->commit_idle_close (reservation);
                                     });
            }
            catch (...) {
                evicted = false;
            }
            if (!evicted) {
                state->cancel_idle_close_reservation (reservation);
            }
        }
    }
    catch (...) {
    }
}

void spot_node_runtime_t::bind_drain_flag (std::shared_ptr<std::atomic_bool> flag)
{
    _state->lane.run ([&] { _state->drain_flag = std::move (flag); }).get ();
}
void spot_node_runtime_t::bind_host_lifecycle (
  std::function<host_lifecycle_snapshot_t ()> read_lifecycle)
{
    _state->lane.run ([&] { _state->host_lifecycle = std::move (read_lifecycle); }).get ();
}

void spot_node_runtime_t::bind_spot_location_resolver (
  std::shared_ptr<runtime::spot_address_resolver_t> resolver)
{
    _state->lane.run ([&] { _state->spot_location_resolver = std::move (resolver); }).get ();
}

std::shared_ptr<service::mesh_node_t> spot_node_runtime_t::native_node () const
{
    return native_node_async ().result ().value ();
}

task_t<std::shared_ptr<service::mesh_node_t>> spot_node_runtime_t::native_node_async () const
{
    const auto state = _state;
    return state->lane.run_task ([state] { return state->native_node.lock (); });
}

task_t<void>
spot_node_runtime_t::send_spot_mesh_parts (const zlink::routing_id_t &target_node_rid,
                                           const spot_id_t &target_spot_id,
                                           runtime::messaging::message_parts_t parts) const
{
    const auto state = _state;
    const spot_node_runtime_t owner (state);
    auto node = co_await owner.native_node_async ();
    if (!node) {
        throw framework_exception_t (framework_error_kind_t::not_found,
                                     "SPOT mesh route requires a running native node");
    }
    try {
        auto native_parts = parts.items ();
        if (native_parts.empty ()) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "SPOT mesh send requires at least one message part");
        }
        auto egress = node->entry_spot ();
        const auto target_generation =
          co_await resolve_target_spot_generation (state, target_node_rid, target_spot_id);
        if (!target_generation) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "SPOT mesh send target generation is unavailable");
        }
        const auto submitted = co_await egress.send_to_spot (target_node_rid, target_spot_id,
                                                             *target_generation, native_parts);
        if (submitted != zlink::submit_result_t::ok) {
            throw framework_exception_t (
              runtime::messaging::map_submit_result_error_kind (submitted),
              "SPOT mesh send was not submitted");
        }
        co_return;
    }
    catch (const framework_exception_t &error) {
        throw error;
    }
    catch (const std::exception &error) {
        throw framework_exception_t (framework_error_kind_t::internal_failure, error.what ());
    }
}

task_t<void>
spot_node_runtime_t::send_spot_mesh_parts_exact (const spot_id_t &source_spot_id,
                                                 const zlink::routing_id_t &target_node_rid,
                                                 const spot_id_t &target_spot_id,
                                                 std::uint64_t target_spot_generation,
                                                 runtime::messaging::message_parts_t parts) const
{
    const auto source = _state->lane.run_sync ([&] {
        const auto found = _state->native_spots_by_id.find (source_spot_id);
        if (found != _state->native_spots_by_id.end ())
            return found->second;
        return std::shared_ptr<service::spot_t>{};
    });
    if (!source || target_spot_generation == 0) {
        throw framework_exception_t (
          framework_error_kind_t::not_found,
          "SPOT mesh send exact source or target generation is unavailable");
    }
    auto native_parts = std::move (parts).take_items ();
    if (native_parts.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "SPOT mesh send requires at least one message part");
    }
    const auto submitted = co_await source->send_to_spot (target_node_rid, target_spot_id,
                                                          target_spot_generation, native_parts);
    if (submitted != zlink::submit_result_t::ok) {
        throw framework_exception_t (runtime::messaging::map_submit_result_error_kind (submitted),
                                     "SPOT mesh send was not submitted");
    }
}

task_t<std::optional<std::uint64_t>>
spot_node_runtime_t::resolve_spot_generation (const zlink::routing_id_t &target_node_rid,
                                              const spot_id_t &target_spot_id) const
{
    return resolve_target_spot_generation (_state, target_node_rid, target_spot_id);
}

void spot_node_runtime_t::set_route_client (route_client_t route_client)
{
    std::atomic_store_explicit (&_state->route_client,
                                std::make_shared<const route_client_t> (std::move (route_client)),
                                std::memory_order_release);
}

std::vector<spot_context_t> spot_node_runtime_t::active_contexts () const
{
    return _state->lane
      .run ([&] {
          std::vector<spot_context_t> contexts;
          contexts.reserve (_state->spot_contexts_by_id.size ());
          for (const auto &[_, context] : _state->spot_contexts_by_id) {
              if (!context._state->closed && !context._state->native_spot.expired ()) {
                  contexts.push_back (spot_context_t (context._state));
              }
          }
          return contexts;
      })
      .get ();
}

std::size_t spot_node_runtime_t::active_user_spot_count () const
{
    return active_user_spot_count_async ().result ().value ();
}

task_t<std::size_t> spot_node_runtime_t::active_user_spot_count_async () const
{
    const auto state = _state;
    return state->lane.run_task (
      [state] { return spot_node_runtime_t (state).active_user_spot_count_on_lane (); });
}

std::size_t spot_node_runtime_t::active_user_spot_count_on_lane () const
{
    return static_cast<std::size_t> (std::count_if (
      _state->spot_contexts_by_id.begin (), _state->spot_contexts_by_id.end (),
      [&] (const auto &entry) {
          const auto &context = entry.second;
          return !context._state->closed && !context._state->native_spot.expired ()
                 && (!_state->snapshot.entry_spot_name
                     || context._state->spot_name != *_state->snapshot.entry_spot_name);
      }));
}

task_t<std::pair<std::size_t, std::size_t>> spot_node_runtime_t::monitoring_counts_async () const
{
    const auto state = _state;
    const spot_node_runtime_t runtime (state);
    const auto actors = co_await runtime.local_actor_inventory_async ();
    const auto spot_count = co_await runtime.active_user_spot_count_async ();
    co_return std::pair{actors.size (), spot_count};
}

} // namespace detail

std::shared_ptr<service::spot_t> detail::spot_node_runtime_t::attach_native_spot (
  const std::shared_ptr<detail::spot_context_state_t> &state, bool relocation_restore, bool publish)
{
    if (!state || !state->node) {
        return {};
    }
    struct attachment_plan_t
    {
        std::shared_ptr<service::mesh_node_t> native_node;
        std::shared_ptr<service::spot_t> native;
        std::string rid;
        std::string spot_name;
        std::string mesh_name;
        std::string node_rid;
        std::uint64_t object_generation = 0;
        std::uint64_t authority_owner_generation = 0;
        bool entry_spot = false;
        bool instance_spot = false;
        std::vector<spot_handler_descriptor_t> handlers;
    };
    auto owner = state->node;
    auto plan = owner->lane.run_sync ([&] {
        attachment_plan_t result;
        result.native_node = owner->native_node.lock ();
        result.rid = std::string (state->spot_id);
        result.spot_name = state->spot_name;
        result.mesh_name = state->mesh_name;
        result.node_rid = std::string (state->node_rid.value ());
        result.object_generation = state->object_generation;
        result.authority_owner_generation = state->authority_owner_generation;
        result.entry_spot =
          owner->snapshot.entry_spot_name && *owner->snapshot.entry_spot_name == state->spot_name;
        result.instance_spot = state->is_instance_spot ();
        result.handlers = state->handlers;
        result.native = state->native_spot.lock ();
        if (!result.native) {
            const auto found = owner->native_spots_by_id.find (result.rid);
            if (found != owner->native_spots_by_id.end ())
                result.native = found->second;
        }
        return result;
    });
    if (!plan.native_node)
        return {};

    auto native = plan.native;
    if (!native) {
        if (plan.entry_spot) {
            native = std::make_shared<service::spot_t> (plan.native_node->entry_spot ());
        } else {
            try {
                auto spot =
                  relocation_restore
                    ? plan.native_node->bind_relocation_spot (runtime::stateful::object_ref_t{
                        plan.instance_spot ? runtime::stateful::object_kind_t::instance_spot
                                           : runtime::stateful::object_kind_t::user_spot,
                        plan.rid, plan.object_generation, plan.authority_owner_generation,
                        plan.mesh_name, plan.node_rid})
                    : plan.native_node->get_or_create_spot (plan.rid);
                native = std::make_shared<service::spot_t> (std::move (spot));
            }
            catch (const std::exception &error) {
                throw framework_exception_t (framework_error_kind_t::internal_failure,
                                             "native spot facade creation failed for '"
                                               + plan.spot_name + "' (rid='" + plan.rid
                                               + "'): " + error.what ());
            }
        }
    }
    if (publish) {
        native = owner->lane.run_sync ([&] {
            auto [found, inserted] = owner->native_spots_by_id.emplace (plan.rid, native);
            if (!inserted)
                native = found->second;
            state->native_spot = native;
            return native;
        });
    } else {
        // Keep the facade out of the public native index until activation succeeds.
        owner->lane.run_sync ([&] { state->native_spot = native; });
    }

    for (const auto &handler : plan.handlers) {
        if (handler.kind == spot_handler_kind_t::subscription) {
            native->set_subscription (handler.channel_name, handler.topic);
        }
    }
    return native;
}

spot_node_builder_t::spot_node_builder_t () :
    _state (std::make_shared<detail::spot_node_builder_state_t> (""))
{
}

spot_node_builder_t::spot_node_builder_t (
  std::shared_ptr<detail::spot_node_builder_state_t> state) :
    _state (std::move (state))
{
}

spot_node_builder_t::~spot_node_builder_t () = default;
spot_node_builder_t::spot_node_builder_t (spot_node_builder_t &&) noexcept = default;
spot_node_builder_t &spot_node_builder_t::operator= (spot_node_builder_t &&) noexcept = default;

spot_node_builder_t &
spot_node_builder_t::set_message_follow_duration (std::chrono::milliseconds duration)
{
    _state->lane.run ([this, duration] { _state->message_follow_duration = duration; }).get ();
    return *this;
}

spot_node_builder_t &
spot_node_builder_t::accept_implicit_route_mesh (std::string route_channel_name,
                                                 std::vector<std::string> manual_connections)
{
    if (route_channel_name.empty () || is_blank (route_channel_name)) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "accepted SPOT route channel name is required");
    }
    for (const auto &endpoint : manual_connections) {
        if (endpoint.empty () || is_blank (endpoint)) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "accepted SPOT route manual endpoint is required");
        }
    }
    _state->lane
      .run ([this, route_channel_name = std::move (route_channel_name),
             manual_connections = std::move (manual_connections)] () mutable {
          const auto duplicate = std::any_of (
            _state->snapshot.accepted_route_channels.begin (),
            _state->snapshot.accepted_route_channels.end (),
            [&] (const auto &accepted) { return accepted.channel_name == route_channel_name; });
          if (duplicate) {
              throw framework_exception_t (framework_error_kind_t::protocol_error,
                                           "duplicate accepted SPOT route channel");
          }
          _state->snapshot.accepted_route_channels.push_back (accepted_spot_route_channel_t{
            std::move (route_channel_name), std::move (manual_connections)});
      })
      .get ();
    return *this;
}

spot_node_builder_t &spot_node_builder_t::add_spot_factory_erased (
  std::string spot_name,
  std::type_index spot_type,
  detail::spot_runtime_kind_t kind,
  user_spot_execution_mode_t execution_mode,
  std::int32_t stable_type_limit,
  spot_relocation_coordination_mode_t relocation_coordination_mode,
  detail::factory_relocation_configuration_t relocation)
{
    const bool entry_spot = kind == detail::spot_runtime_kind_t::entry;
    const bool instance_spot = kind == detail::spot_runtime_kind_t::instance;
    if (entry_spot && execution_mode != user_spot_execution_mode_t::spot_wide) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Entry Spot execution mode is fixed by the Framework");
    }
    if (relocation.kind == detail::factory_relocation_kind_t::preserve_state
        && (!relocation.capture || !relocation.restore)) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Spot preserve-state relocation callbacks must not be empty");
    }
    _state->lane
      .run ([this, spot_name = std::move (spot_name), spot_type, kind, execution_mode,
             stable_type_limit, relocation_coordination_mode,
             relocation = std::move (relocation)] () mutable {
          const bool entry_spot = kind == detail::spot_runtime_kind_t::entry;
          const bool instance_spot = kind == detail::spot_runtime_kind_t::instance;
          const auto [_, inserted] = _state->spot_factories.emplace (spot_name, spot_type);
          if (!inserted) {
              throw framework_exception_t (framework_error_kind_t::protocol_error,
                                           "duplicate spot factory registration");
          }
          if (entry_spot) {
              if (_state->snapshot.entry_spot_name) {
                  throw framework_exception_t (framework_error_kind_t::protocol_error,
                                               "entry spot is already registered");
              }
              _state->snapshot.entry_spot_name = spot_name;
          }
          if (instance_spot) {
              _state->snapshot.instance_spot_names.push_back (spot_name);
          }
          if (!entry_spot) {
              _state->spot_factory_relocations.emplace (spot_name, relocation);
              _state->spot_stable_type_limits.emplace (spot_name, stable_type_limit);
              _state->spot_relocation_coordination_modes.emplace (spot_name,
                                                                  relocation_coordination_mode);
          }
          _state->snapshot.spot_execution_modes.emplace (spot_name, execution_mode);
          _state->snapshot.spot_names.push_back (std::move (spot_name));
      })
      .get ();
    return *this;
}

spot_node_builder_t &spot_node_builder_t::add_actor_factory_erased (
  std::string actor_type,
  std::type_index actor_instance_type,
  std::function<std::shared_ptr<void> (std::string)> create_instance,
  std::function<void (void *, const actor_ref_t &, void *)> configure_instance,
  std::function<std::optional<zlink::message_t> (void *, serializer_registry_t &)>
    serialize_instance,
  std::function<void (void *, const zlink::message_t &, serializer_registry_t &)>
    deserialize_instance,
  std::function<std::shared_ptr<void> (actor_context_t)> create_context_instance,
  std::function<actor_t *(void *)> as_actor,
  detail::actor_join_completion_callback_t on_join_completed,
  detail::factory_relocation_configuration_t relocation,
  std::function<task_t<std::vector<std::byte>> (void *, std::stop_token)> capture,
  std::function<task_t<void> (void *, std::vector<std::byte>, std::stop_token)> restore,
  bool duplicate_bind_is_default,
  bool session_bound_is_default)
{
    if ((!create_instance && !create_context_instance) || !configure_instance || !serialize_instance
        || !deserialize_instance || !as_actor) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "actor factory callback must not be empty");
    }
    if (relocation.kind == detail::factory_relocation_kind_t::preserve_state
        && (!capture || !restore)) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Actor preserve-state relocation callbacks must not be empty");
    }
    _state->lane
      .run ([this, actor_type = std::move (actor_type), actor_instance_type,
             create_instance = std::move (create_instance),
             configure_instance = std::move (configure_instance),
             serialize_instance = std::move (serialize_instance),
             deserialize_instance = std::move (deserialize_instance),
             create_context_instance = std::move (create_context_instance),
             as_actor = std::move (as_actor), on_join_completed = std::move (on_join_completed),
             relocation = std::move (relocation), capture = std::move (capture),
             restore = std::move (restore), duplicate_bind_is_default,
             session_bound_is_default] () mutable {
          const auto [_, inserted] = _state->actor_factories.emplace (
            actor_type,
            detail::spot_node_builder_state_t::actor_factory_registration_t{
              actor_instance_type, relocation, std::move (create_instance),
              std::move (configure_instance), std::move (serialize_instance),
              std::move (deserialize_instance), std::move (create_context_instance),
              std::move (as_actor), std::move (on_join_completed), std::move (capture),
              std::move (restore), duplicate_bind_is_default, session_bound_is_default});
          if (!inserted) {
              throw framework_exception_t (framework_error_kind_t::already_exists,
                                           "duplicate actor factory registration");
          }
          _state->snapshot.actor_types.push_back (std::move (actor_type));
      })
      .get ();
    return *this;
}

void spot_node_builder_t::register_lifecycle_erased (std::string spot_name,
                                                     detail::spot_lifecycle_callbacks_t callbacks)
{
    _state->lane
      .run (
        [this, spot_name = std::move (spot_name), callbacks = std::move (callbacks)] () mutable {
            _state->spot_lifecycles[std::move (spot_name)] = std::move (callbacks);
        })
      .get ();
}

spot_node_builder_t &spot_node_builder_t::add_spot_resolver (
  std::string name, std::function<std::optional<spot_route_t> (spot_id_t)> resolver)
{
    if (name.empty () || !resolver) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "spot resolver requires a name and callback");
    }
    _state->lane
      .run ([this, name = std::move (name), resolver = std::move (resolver)] () mutable {
          const auto [_, inserted] =
            _state->resolvers.emplace (std::move (name), std::move (resolver));
          if (!inserted) {
              throw framework_exception_t (framework_error_kind_t::protocol_error,
                                           "duplicate spot resolver registration");
          }
      })
      .get ();
    return *this;
}

spot_node_snapshot_t spot_node_builder_t::snapshot () const
{
    return _state->lane.run ([this] { return _state->snapshot; }).get ();
}

detail::local_spot_create_result_t spot_node_builder_t::create_spot (std::string spot_name)
{
    return detail::spot_node_runtime_t (_state).create_spot (std::move (spot_name));
}

detail::local_spot_create_result_t spot_node_builder_t::create_spot_raw (std::string spot_name,
                                                                         zlink::message_t request)
{
    return detail::spot_node_runtime_t (_state).create_spot (std::move (spot_name),
                                                             std::move (request));
}

detail::local_spot_create_result_t spot_node_builder_t::create_spot (std::string spot_name,
                                                                     const message_t &request)
{
    const auto channel_runtime =
      _state ? _state->lane.run ([this] { return _state->channel_runtime; }).get ()
             : std::shared_ptr<detail::channel_runtime_state_t>{};
    if (!channel_runtime || !channel_runtime->serializers) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "spot create requires a serializer registry");
    }
    return create_spot_raw (std::move (spot_name),
                            detail::message_to_raw (request, *channel_runtime->serializers));
}

detail::local_spot_create_result_t spot_node_builder_t::get_or_create_spot (std::string spot_name,
                                                                            spot_id_t spot_id)
{
    return detail::spot_node_runtime_t (_state).get_or_create_spot (std::move (spot_name),
                                                                    std::move (spot_id));
}

detail::local_spot_create_result_t spot_node_builder_t::get_or_create_spot_raw (
  std::string spot_name, spot_id_t spot_id, zlink::message_t request)
{
    return detail::spot_node_runtime_t (_state).get_or_create_spot (
      std::move (spot_name), std::move (spot_id), std::move (request));
}

detail::local_spot_create_result_t spot_node_builder_t::get_or_create_spot (
  std::string spot_name, spot_id_t spot_id, const message_t &request)
{
    const auto channel_runtime =
      _state ? _state->lane.run ([this] { return _state->channel_runtime; }).get ()
             : std::shared_ptr<detail::channel_runtime_state_t>{};
    if (!channel_runtime || !channel_runtime->serializers) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "spot get or create requires a serializer registry");
    }
    return get_or_create_spot_raw (std::move (spot_name), std::move (spot_id),
                                   detail::message_to_raw (request, *channel_runtime->serializers));
}

task_t<std::optional<spot_info_t>> spot_node_builder_t::find_spot (spot_id_t spot_id) const
{
    return task_t<std::optional<spot_info_t>> (result_t<std::optional<spot_info_t>>::success (
      detail::spot_node_runtime_t (_state).find_spot (std::move (spot_id))));
}

task_t<std::vector<spot_info_t>> spot_node_builder_t::list_spots () const
{
    return task_t<std::vector<spot_info_t>> (result_t<std::vector<spot_info_t>>::success (
      detail::spot_node_runtime_t (_state).list_spots ()));
}

task_t<bool> spot_node_builder_t::close_spot (spot_id_t spot_id)
{
    return detail::spot_node_runtime_t (_state).close_spot (std::move (spot_id));
}

void spot_node_builder_t::retain_factory_builder (std::shared_ptr<void> builder)
{
    _state->lane
      .run ([this, builder = std::move (builder)] () mutable {
          _state->factory_builder_lifetimes.push_back (std::move (builder));
      })
      .get ();
}

std::optional<std::string> spot_node_builder_t::spot_name_for (spot_id_t spot_id) const
{
    return detail::spot_node_runtime_t (_state).spot_name_for (std::move (spot_id));
}

std::optional<spot_route_t> spot_node_builder_t::resolve_spot (spot_id_t spot_id) const
{
    return detail::spot_node_runtime_t (_state).resolve_spot (std::move (spot_id));
}

spot_manager_t::spot_manager_t () :
    _state (std::make_shared<detail::spot_node_builder_state_t> (""))
{
}

spot_manager_t::spot_manager_t (std::shared_ptr<detail::spot_node_builder_state_t> state) :
    _state (std::move (state))
{
}

spot_manager_t::spot_manager_t (std::shared_ptr<detail::spot_node_builder_state_t> state,
                                std::weak_ptr<detail::spot_context_state_t> source) :
    _state (std::move (state)), _source (std::move (source))
{
}

spot_manager_t::~spot_manager_t () = default;
spot_manager_t::spot_manager_t (spot_manager_t &&) noexcept = default;
spot_manager_t &spot_manager_t::operator= (spot_manager_t &&) noexcept = default;

spot_create_call_t::spot_create_call_t (std::shared_ptr<detail::spot_create_call_state_t> state) :
    _state (std::move (state))
{
}

spot_create_call_t::~spot_create_call_t () = default;
spot_create_call_t::spot_create_call_t (spot_create_call_t &&) noexcept = default;
spot_create_call_t &spot_create_call_t::operator= (spot_create_call_t &&) noexcept = default;

spot_create_call_t &spot_create_call_t::in_mesh (std::string mesh_name)
{
    if (!_state || _state->mesh_name)
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Spot create Mesh can be set only once");
    _state->mesh_name = std::move (mesh_name);
    return *this;
}

spot_create_call_t &spot_create_call_t::creation_request (message_t request)
{
    if (!_state || _state->request)
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Spot creation request can be set only once");
    _state->request = std::move (request);
    return *this;
}

spot_create_call_t &spot_create_call_t::timeout (std::chrono::milliseconds timeout)
{
    if (!_state || _state->timeout || timeout <= std::chrono::milliseconds::zero ())
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Spot create timeout must be positive and set only once");
    _state->timeout = timeout;
    return *this;
}

task_t<spot_create_result_t> spot_create_call_t::async ()
{
    if (!_state || _state->submitted)
        return task_t<spot_create_result_t> (result_t<spot_create_result_t>::failure (
          framework_error_kind_t::invalid_operation, "Spot create call was already submitted"));
    _state->submitted = true;
    if (const auto source = _state->source.lock ()) {
        try {
            source->ensure_relocation_turn_open ();
        }
        catch (const framework_exception_t &error) {
            return task_t<spot_create_result_t> (
              detail::result_access_t::failure<spot_create_result_t> (error));
        }
    }
    if (!_state->node)
        return task_t<spot_create_result_t> (result_t<spot_create_result_t>::failure (
          framework_error_kind_t::not_configured,
          "Spot manager is not connected to a Location runtime"));
    const auto create_user_spot =
      _state->node->lane.run ([&] { return _state->node->create_user_spot; }).get ();
    if (!create_user_spot)
        return task_t<spot_create_result_t> (result_t<spot_create_result_t>::failure (
          framework_error_kind_t::not_configured,
          "Spot manager is not connected to a Location runtime"));
    return create_user_spot (_state->exclusive, _state->spot_id, std::move (_state->stable_type),
                             std::move (_state->mesh_name), std::move (_state->request),
                             _state->timeout.value_or (std::chrono::seconds (30)));
}

task_t<spot_create_result_t> spot_create_call_t::yield ()
{
    return detail::yield_call<spot_create_result_t> ([this] { return async (); });
}

spot_create_call_t spot_manager_t::create (std::string stable_type)
{
    auto state = std::make_shared<detail::spot_create_call_state_t> ();
    state->node = _state;
    state->source = _source;
    state->exclusive = true;
    state->stable_type = std::move (stable_type);
    return spot_create_call_t (std::move (state));
}

spot_create_call_t spot_manager_t::get_or_create (spot_id_t spot_id, std::string stable_type)
{
    auto state = std::make_shared<detail::spot_create_call_state_t> ();
    state->node = _state;
    state->source = _source;
    state->spot_id = std::move (spot_id);
    state->stable_type = std::move (stable_type);
    return spot_create_call_t (std::move (state));
}

task_t<std::optional<spot_ref_t>> spot_manager_t::find (spot_id_t spot_id) const
{
    if (const auto source = _source.lock ()) {
        try {
            source->ensure_relocation_turn_open ();
        }
        catch (const framework_exception_t &error) {
            return task_t<std::optional<spot_ref_t>> (
              detail::result_access_t::failure<std::optional<spot_ref_t>> (error));
        }
    }
    if (!_state)
        return task_t<std::optional<spot_ref_t>> (result_t<std::optional<spot_ref_t>>::failure (
          framework_error_kind_t::not_configured,
          "Spot manager is not connected to a Location runtime"));
    const auto find_user_spot = _state->lane.run ([&] { return _state->find_user_spot; }).get ();
    if (!find_user_spot)
        return task_t<std::optional<spot_ref_t>> (result_t<std::optional<spot_ref_t>>::failure (
          framework_error_kind_t::not_configured,
          "Spot manager is not connected to a Location runtime"));
    return find_user_spot (std::move (spot_id));
}

task_t<bool> spot_manager_t::close (spot_ref_t spot)
{
    const auto entered = std::chrono::steady_clock::now ();
    if (const auto source = _source.lock ()) {
        try {
            source->ensure_relocation_turn_open ();
        }
        catch (const framework_exception_t &error) {
            return task_t<bool> (detail::result_access_t::failure<bool> (error));
        }
    }
    if (!_state)
        return task_t<bool> (
          result_t<bool>::failure (framework_error_kind_t::not_configured,
                                   "Spot manager is not connected to a Location runtime"));
    const auto deadline = entered + runtime::configuration::default_request_timeout;
    const auto wire_deadline = runtime::messaging::request_deadline_unix_ms (deadline);
    const auto close_user_spot = _state->lane.run ([&] { return _state->close_user_spot; }).get ();
    if (!close_user_spot)
        return task_t<bool> (
          result_t<bool>::failure (framework_error_kind_t::not_configured,
                                   "Spot manager is not connected to a Location runtime"));
    return close_user_spot (std::move (spot), deadline, wire_deadline);
}

spot_publisher_client_t::spot_publisher_client_t (spot_manager_t manager,
                                                  serializer_registry_t &serializers) :
    _manager (std::move (manager)),
    _channel_runtime (_manager._state->channel_runtime),
    _serializers (&serializers)
{
}

publish_call_t spot_publisher_client_t::publish_raw (std::string channel_name,
                                                     std::string topic,
                                                     std::string packet_name,
                                                     std::string content_type,
                                                     zlink::message_t payload) const
{
    if (channel_name.empty () || is_blank (channel_name)) {
        return publish_call_t (
          result_t<void>::failure (framework_error_kind_t::protocol_error,
                                   "logical multicast channel name must not be empty"));
    }
    if (topic.empty ()) {
        return publish_call_t (result_t<void>::failure (
          framework_error_kind_t::protocol_error, "logical multicast topic must not be empty"));
    }

    const auto diagnostics_mode = detail::message_flow_tracer_t (_manager._state->dispatch).mode ();
    auto frame = encode_spot_publish_frame (channel_name, packet_name, topic,
                                            std::move (content_type), payload);
    auto call =
      publish_call_t ([state = _manager._state, channel_name = std::move (channel_name),
                       topic = std::move (topic), frame = std::move (frame), diagnostics_mode] (
                        const publish_call_t::metadata_map_t &metadata) -> task_t<void> {
          auto native_node = state->lane.run_sync ([&] { return state->native_node.lock (); });
          if (!native_node)
              throw framework_exception_t (framework_error_kind_t::unavailable,
                                           "logical multicast route mesh is not connected");
          const auto capture = detail::dispatch_error_reporter_t (state->dispatch).enabled ();
          auto flow_scope = runtime::flow_context_t::enter_current_or_create (
            flow_origin_t::application, diagnostics_mode);
          std::vector<zlink::message_t> parts{frame};
          auto publisher = native_node->entry_spot ();
          auto encoded_metadata = detail::mesh_metadata_codec_t::encode (metadata);
          if (capture) {
              const auto mesh_name = state->lane.run_sync ([&] { return state->snapshot.name; });
              co_await publisher.publish (
                channel_name, topic, std::move (parts), std::move (encoded_metadata),
                [state, channel_name, mesh_name, topic] (const zlink::routing_id_t &target,
                                                         zlink::submit_result_t submitted) {
                    detail::report_logical_multicast_failure (
                      state, channel_name, mesh_name, topic, target.to_string (),
                      detail::dispatch_reason_from_submit_result (submitted));
                });
          } else {
              co_await publisher.publish (channel_name, topic, std::move (parts),
                                          std::move (encoded_metadata));
          }
          co_return;
      });
    call._preflight = [admission = _channel_runtime] {
        try {
            if (admission)
                detail::channel_runtime_t (admission).admit_outbound ();
            return result_t<void>::success ();
        }
        catch (...) {
            return detail::current_exception_result<void> ();
        }
    };
    return call;
}

} // namespace zlink::framework
