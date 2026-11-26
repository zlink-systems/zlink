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
using detail::actor_request_dedup_prefix;
using detail::deactivate_actor_location;
using detail::decrement_actor_count_unlocked;
using detail::erase_actor_route_unlocked;
using detail::release_actor_location_async;
using detail::report_spot_dispatch_trace;
using detail::spot_generation;

constexpr std::size_t max_spot_relocation_state_bytes = 64u * 1024u * 1024u;

std::uint64_t actor_generation_from_location (const actor_location_t &location)
{
    return location.actor_ref ? location.actor_ref->object_generation () : 0;
}

std::uint64_t owner_node_generation (const detail::spot_context_state_t &context)
{
    if (context.node) {
        if (const auto native = context.node->native_node.lock ()) {
            return native->status ().lifecycle_generation ();
        }
    }
    return 0;
}

std::uint64_t actor_membership_epoch (const actor_ref_t &actor,
                                      const detail::spot_context_state_t &context)
{
    if (!context.node) {
        return 0;
    }
    const auto found =
      context.node->core_actor_membership_epochs.find (std::string (actor.actor_id ().value ()));
    return found == context.node->core_actor_membership_epochs.end () ? 0 : found->second;
}

actor_location_t make_actor_location (const actor_ref_t &actor,
                                      const detail::spot_context_state_t &context)
{
    return actor_location_t{
      .mesh_name = context.node->snapshot.name,
      .actor_id = std::string (actor.actor_id ().value ()),
      .actor_type =
        std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
      .actor_ref = actor,
      .owner_node_rid = zlink::routing_id_t::from (std::string (actor.node_rid ().value ())),
      .owner_node_generation = owner_node_generation (context),
      .spot_id = context.spot_id,
      .spot_generation = spot_generation (context),
      .spot_kind = zlink::spot_kind::user,
      .membership_epoch = actor_membership_epoch (actor, context)};
}

actor_location_t make_entry_actor_location (const actor_ref_t &actor,
                                            const detail::spot_context_state_t &context)
{
    return actor_location_t{
      .mesh_name = context.node->snapshot.name,
      .actor_id = std::string (actor.actor_id ().value ()),
      .actor_type =
        std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
      .actor_ref = actor,
      .owner_node_rid = zlink::routing_id_t::from (std::string (context.node_rid.value ())),
      .owner_node_generation = owner_node_generation (context),
      .spot_id = context.spot_id,
      .spot_generation = spot_generation (context),
      .spot_kind = zlink::spot_kind::entry,
      .membership_epoch = actor_membership_epoch (actor, context)};
}

task_t<void>
deactivate_actor_location_async (std::weak_ptr<detail::spot_node_builder_state_t> weak_state,
                                 actor_location_t location)
{
    auto state = weak_state.lock ();
    if (!state) {
        co_return;
    }

    std::function<task_t<result_t<void>> (actor_ref_t)> destroy_actor_registry;
    if (!location.actor_ref) {
        co_return;
    }
    const auto &actor = *location.actor_ref;
    const auto key =
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)) + ":"
      + std::string (actor.actor_id ().value ());
    const auto blocked = co_await state->actor_transfer_coordinator->blocks_dispatch_async (key);
    const auto deactivated = co_await state->lane.run_task ([&] {
        // A lost claim races with a completed transfer: after this node hands the
        // actor to another node it records the newer generation and Message
        // Message Follow route. A loss notification for an older generation is stale
        // and must not erase that newer record.
        const auto recorded = state->actor_generations.find (key);
        if (recorded != state->actor_generations.end ()
            && recorded->second > actor.object_generation ()) {
            return false;
        }
        // ObjectGeneration alone cannot distinguish a returning incarnation
        // (A→B→A keeps the generation while the owner fences advance). While
        // a transfer for this Actor is in flight on this node, a location
        // loss can only refer to the superseded claim: the transfer commit
        // owns the record lifecycle, so the stale loss must not erase the
        // admission being established.
        if (blocked) {
            return false;
        }
        erase_actor_route_unlocked (*state, key);
        state->actor_created_keys.erase (key);
        state->destroyed_actor_keys.insert (key);
        state->actor_instances.erase (key);
        detail::erase_actor_instance_index_unlocked (
          *state, ::zlink::framework::detail::actor_ref_access_t::actor_type (actor),
          actor.actor_id ().value ());
        (void) state->dispatched_request_replies.erase_if ([&] (const auto &request_key) {
            return request_key.starts_with (actor_request_dedup_prefix (key));
        });
        destroy_actor_registry = state->destroy_actor_registry;
        return true;
    });
    if (!deactivated)
        co_return;
    if (destroy_actor_registry) {
        (void) co_await destroy_actor_registry (actor);
    }
}

struct actor_location_plan_t
{
    runtime::location_lifecycle_t *lifecycle = nullptr;
    std::shared_ptr<service::mesh_node_t> native_node;
    std::shared_ptr<service::spot_t> native_spot;
    std::string mesh_name;
    node_rid_t context_node_rid;
    spot_id_t spot_id;
    std::uint64_t membership_epoch = 0;
    bool attached = false;
};

actor_location_plan_t
capture_actor_location_plan (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                             const actor_ref_t &actor,
                             const detail::spot_context_state_t &context)
{
    return state->lane
      .run ([&] {
          actor_location_plan_t result;
          result.lifecycle = state->location_lifecycle;
          result.attached = context.node.get () == state.get ();
          if (!result.attached)
              return result;
          result.native_node = state->native_node.lock ();
          result.native_spot = context.native_spot.lock ();
          result.mesh_name = state->snapshot.name;
          result.context_node_rid = context.node_rid;
          result.spot_id = context.spot_id;
          const auto epoch =
            state->core_actor_membership_epochs.find (std::string (actor.actor_id ().value ()));
          if (epoch != state->core_actor_membership_epochs.end ())
              result.membership_epoch = epoch->second;
          return result;
      })
      .get ();
}

actor_location_t
materialize_actor_location (const actor_ref_t &actor, const actor_location_plan_t &plan, bool entry)
{
    return actor_location_t{
      .mesh_name = plan.mesh_name,
      .actor_id = std::string (actor.actor_id ().value ()),
      .actor_type =
        std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
      .actor_ref = actor,
      .owner_node_rid = entry
                          ? zlink::routing_id_t::from (std::string (plan.context_node_rid.value ()))
                          : zlink::routing_id_t::from (std::string (actor.node_rid ().value ())),
      .owner_node_generation =
        plan.native_node ? plan.native_node->status ().lifecycle_generation () : 0,
      .spot_id = plan.spot_id,
      .spot_generation = plan.native_spot ? plan.native_spot->status ().lifecycle_generation () : 0,
      .spot_kind = entry ? zlink::spot_kind::entry : zlink::spot_kind::user,
      .membership_epoch = plan.membership_epoch};
}

result_t<void> claim_actor_location_before_activation (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  const actor_ref_t &committed,
  const detail::spot_context_state_t &context,
  bool &claimed,
  bool takeover = false)
{
    claimed = false;
    const auto plan = capture_actor_location_plan (state, committed, context);
    if (!plan.lifecycle) {
        return result_t<void>::success ();
    }
    if (!plan.attached) {
        return result_t<void>::failure (framework_error_kind_t::unavailable,
                                        "actor location claim raced Spot context teardown");
    }
    if (plan.lifecycle->owns_actor (
          actor_location_key_t{plan.mesh_name, std::string (committed.actor_id ().value ())})) {
        return result_t<void>::success ();
    }

    auto location = materialize_actor_location (committed, plan, false);
    const auto claim_result = plan.lifecycle->claim_actor (
      location,
      [weak_state = std::weak_ptr<detail::spot_node_builder_state_t> (state)] (
        const actor_location_t &lost) { deactivate_actor_location (weak_state, lost); },
      takeover);
    if (claim_result.status != location_write_status_t::stored) {
        return result_t<void>::failure (claim_result.status
                                            == location_write_status_t::rejected_conflict
                                          ? framework_error_kind_t::already_exists
                                          : framework_error_kind_t::internal_failure,
                                        "actor location claim failed");
    }
    claimed = true;
    return result_t<void>::success ();
}
} // namespace

namespace detail
{

std::string actor_request_dedup_prefix (std::string_view actor_key)
{
    return std::to_string (actor_key.size ()) + ":" + std::string (actor_key);
}

void report_actor_leave_failure (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                 const result_t<void> &result,
                                 std::string_view spot_id,
                                 std::string_view actor_id,
                                 std::string_view transfer_id)
{
    if (result || !state
        || !detail::message_flow_tracer_t (state->dispatch)
              .enabled (message_flow_log_mode_t::errors))
        return;
    report_spot_dispatch_trace (
      state, message_flow_outcome_t::completed, dispatch_error_surface_t::spot_actor,
      dispatch_message_kind_t::control, "spot_actor_leave", {}, spot_id, actor_id, transfer_id,
      message_flow_result_t::failed, std::nullopt, "leave_failed", result.exception ());
}

void decrement_actor_count_unlocked (detail::spot_context_state_t &state)
{
    if (state.actor_count > 0) {
        state.actor_count--;
    }
}

void erase_actor_route_unlocked (detail::spot_node_builder_state_t &state, const std::string &key)
{
    state.actor_spot_ids.erase (key);
    state.actor_routes.erase (key);
    state.actor_generations.erase (key);
    state.actor_authority_fences.erase (key);
    state.native_actors.erase (key);
}

void deactivate_actor_location (std::weak_ptr<detail::spot_node_builder_state_t> weak_state,
                                const actor_location_t &location)
{
    auto pending = deactivate_actor_location_async (weak_state, location);
    detail::observe_task_completion (pending, [weak_state,
                                               location] (const result_t<void> &terminal) {
        report_actor_control_terminal (
          weak_state.lock (), terminal, {},
          location.actor_ref ? std::optional<std::string> (location.actor_ref->actor_id ().value ())
                             : std::nullopt);
    });
}

result_t<void> claim_pending_actor_location_before_activation (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  const actor_ref_t &source_actor,
  const spot_id_t &source_spot_id,
  const actor_ref_t &committed,
  const detail::spot_context_state_t &target,
  bool &claimed)
{
    claimed = false;
    const auto plan = capture_actor_location_plan (state, committed, target);
    if (!plan.lifecycle) {
        return result_t<void>::success ();
    }
    if (!plan.attached) {
        return result_t<void>::failure (framework_error_kind_t::unavailable,
                                        "pending actor location claim raced Spot context teardown");
    }
    const bool source_is_local =
      !source_actor.node_rid ().empty ()
      && source_actor.node_rid ().value () == plan.context_node_rid.value ();
    if (source_is_local
        && plan.lifecycle->owns_actor (
          actor_location_key_t{plan.mesh_name, std::string (committed.actor_id ().value ())})) {
        return result_t<void>::success ();
    }
    auto location = materialize_actor_location (committed, plan, false);
    location.actor_ref = source_actor;
    location.owner_node_rid =
      zlink::routing_id_t::from (std::string (source_actor.node_rid ().value ()));
    location.spot_id =
      source_spot_id.empty () ? std::string (source_actor.node_rid ().value ()) : source_spot_id;
    location.spot_kind = source_spot_id.empty () ? zlink::spot_kind::entry : zlink::spot_kind::user;
    const auto result = plan.lifecycle->claim_actor (
      std::move (location),
      [weak_state = std::weak_ptr<detail::spot_node_builder_state_t> (state)] (
        const actor_location_t &lost) { deactivate_actor_location (weak_state, lost); },
      true);
    if (result.status != location_write_status_t::stored) {
        return result_t<void>::failure (result.status == location_write_status_t::rejected_conflict
                                          ? framework_error_kind_t::already_exists
                                          : framework_error_kind_t::internal_failure,
                                        "pending actor location claim failed");
    }
    claimed = true;
    return result_t<void>::success ();
}

task_t<void> release_actor_location_async (std::shared_ptr<detail::spot_node_builder_state_t> state,
                                           actor_ref_t actor)
{
    if (!state || ::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
        co_return;
    }
    auto [lifecycle, mesh_name] = co_await state->lane.run_task (
      [state] { return std::make_pair (state->location_lifecycle, state->snapshot.name); });
    if (!lifecycle)
        co_return;
    (void) co_await lifecycle->release_actor_async (
      actor_location_key_t{std::move (mesh_name), std::string (actor.actor_id ().value ())});
}

void release_actor_location (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                             const actor_ref_t &actor)
{
    release_actor_location_async (state, actor).result ().value ();
}

result_t<void>
update_actor_location_after_move (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                  const actor_ref_t &actor,
                                  const detail::spot_context_state_t &context,
                                  bool entry)
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
        return result_t<void>::success ();
    }
    //  Spot context teardown can detach `node` while a membership commit is
    //  still completing on another boundary. Building the location would
    //  dereference the detached pointer; report a typed lifecycle failure
    //  instead (spec 15 §3 — membership/lifecycle stay ordered on the Spot
    //  turn boundary; a detached context means that boundary has ended).
    const auto plan = capture_actor_location_plan (state, actor, context);
    if (!plan.lifecycle)
        return result_t<void>::success ();
    if (!plan.attached) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "actor committed location update raced Spot context teardown");
    }
    auto location = materialize_actor_location (actor, plan, entry);
    const auto tracked = plan.lifecycle->owns_actor (
      actor_location_key_t{plan.mesh_name, std::string (actor.actor_id ().value ())});
    const auto updated = plan.lifecycle->update_actor_location (std::move (location));
    if (updated.status != location_write_status_t::stored) {
        return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                        "actor committed location update failed: status="
                                          + std::to_string (static_cast<int> (updated.status))
                                          + " tracked=" + (tracked ? "true" : "false"));
    }
    return result_t<void>::success ();
}

bool spot_node_runtime_t::stage_session_relocation_route (
  const std::string &transfer_id,
  std::vector<std::uint8_t> route,
  std::string actor_type,
  std::uint64_t target_owner_lease_generation)
{
    return _state->actor_transfer_coordinator->stage_session_relocation_route (
      transfer_id, std::move (route), std::move (actor_type), target_owner_lease_generation);
}

bool spot_node_runtime_t::commit_session_relocation_route_authority (
  const std::string &transfer_id,
  std::uint64_t previous_authority_owner_generation,
  std::uint64_t target_authority_owner_generation)
{
    return _state->actor_transfer_coordinator->commit_session_relocation_route_authority (
      transfer_id, previous_authority_owner_generation, target_authority_owner_generation);
}

bool spot_node_runtime_t::adopt_committed_actor_relocation_authority (
  const runtime::stateful::object_ref_t &target,
  std::uint64_t target_node_generation,
  std::uint64_t target_owner_lease_generation)
{
    if (target.kind != runtime::stateful::object_kind_t::actor || target.key.empty ()
        || target.object_generation == 0 || target.authority_owner_generation == 0
        || target.node_id.empty () || target_node_generation == 0
        || target_owner_lease_generation == 0)
        return false;

    return _state->lane
      .run ([&] {
          const auto type = _state->actor_types_by_id.find (target.key);
          if (type == _state->actor_types_by_id.end ())
              return false;
          const auto fence = _state->actor_authority_fences.find (type->second + ":" + target.key);
          if (fence == _state->actor_authority_fences.end ())
              return false;
          const auto target_node = zlink::routing_id_t::from (target.node_id).to_bytes ();
          auto &current = fence->second;
          if (current.actor_id != target.key
              || current.object_generation != target.object_generation
              || current.target_node_routing_id != target_node
              || current.target_node_generation != target_node_generation
              || current.authority_owner_generation > target.authority_owner_generation
              || (current.authority_owner_generation == target.authority_owner_generation
                  && current.owner_lease_generation != 0
                  && current.owner_lease_generation != target_owner_lease_generation)) {
              return false;
          }
          current.authority_owner_generation = target.authority_owner_generation;
          current.owner_lease_generation = target_owner_lease_generation;
          return true;
      })
      .get ();
}

task_t<bool> spot_node_runtime_t::activate_session_relocation_route (const std::string &transfer_id)
{
    const auto admission =
      _state->actor_transfer_coordinator->session_relocation_admission (transfer_id);
    if (!admission || admission->session_relocation_route.empty ())
        co_return false;
    runtime::protocol::session_relocation_route_t route;
    try {
        route =
          runtime::protocol::decode_session_relocation_route (admission->session_relocation_route);
    }
    catch (...) {
        co_return false;
    }
    if (route.route.action != runtime::protocol::session_relocation_route_action_t::commit
        || admission->session_relocation_committed_previous_authority_owner_generation == 0
        || admission->session_relocation_committed_target_authority_owner_generation
             <= admission->session_relocation_committed_previous_authority_owner_generation) {
        co_return false;
    }
    route.route.previous_authority_owner_generation =
      admission->session_relocation_committed_previous_authority_owner_generation;
    route.route.target_authority_owner_generation =
      admission->session_relocation_committed_target_authority_owner_generation;
    route.route.target_owner_lease_generation =
      admission->session_relocation_target_owner_lease_generation;
    const auto native = co_await native_node_async ();
    if (!native)
        co_return false;
    co_return co_await native->route_session_remote (
      zlink::routing_id_t::from (route.session_owner_node_routing_id), std::move (route));
}

std::optional<std::string> spot_node_runtime_t::resolve_actor_type (std::string_view actor_id) const
{
    return _state->lane
      .run ([&] () -> std::optional<std::string> {
          const auto found = _state->actor_types_by_id.find (std::string (actor_id));
          if (found == _state->actor_types_by_id.end ())
              return std::nullopt;
          return found->second;
      })
      .get ();
}

std::optional<std::uint64_t>
spot_node_runtime_t::resolve_actor_membership_epoch (std::string_view actor_id) const
{
    return _state->lane
      .run ([&] () -> std::optional<std::uint64_t> {
          const auto found = _state->core_actor_membership_epochs.find (std::string (actor_id));
          if (found == _state->core_actor_membership_epochs.end ())
              return std::nullopt;
          return found->second;
      })
      .get ();
}

actor_context_t spot_node_runtime_t::default_actor_context ()
{
    return actor_context_t{};
}

void spot_node_runtime_t::set_message_follow_duration (std::chrono::milliseconds duration)
{
    _state->lane.run ([&] { _state->message_follow_duration = duration; }).get ();
}

void spot_node_runtime_t::bind_relocation_store (
  std::shared_ptr<runtime::stateful::relocation_store_port_t> store)
{
    _state->lane.run ([&] { _state->relocation_store = std::move (store); }).get ();
}

void spot_node_runtime_t::bind_relocation_authority (
  std::shared_ptr<runtime::stateful::authority_relocation_port_t> authority)
{
    _state->lane.run ([&] { _state->relocation_authority = std::move (authority); }).get ();
}

namespace
{

// Parameters live in the coroutine frame, unlike lambda captures, which live in the
// closure object the caller destroys when it returns.
task_t<void> capture_relocation_payload (
  std::shared_ptr<std::vector<std::byte>> payload,
  std::function<task_t<std::vector<std::byte>> (void *, std::stop_token)> capture,
  std::shared_ptr<spot_context_state_t> context,
  std::stop_token cancellation)
{
    *payload = co_await capture (context->spot_instance.get (), cancellation);
}

task_t<std::vector<std::uint8_t>>
capture_spot_relocation_state_async (std::shared_ptr<spot_node_builder_state_t> state,
                                     runtime::stateful::object_ref_t spot,
                                     std::string stable_type,
                                     std::stop_token cancellation)
{
    if (spot.kind == runtime::stateful::object_kind_t::actor) {
        detail::spot_node_builder_state_t::actor_factory_registration_t factory;
        std::shared_ptr<void> instance;
        co_await state->lane.run_task ([&] {
            const auto configured = state->actor_factories.find (stable_type);
            const auto materialized = state->actor_instances.find (stable_type + ":" + spot.key);
            if (configured == state->actor_factories.end ()
                || materialized == state->actor_instances.end () || !materialized->second) {
                throw framework_exception_t (framework_error_kind_t::not_found,
                                             "Relocation source Actor is not materialized");
            }
            factory = configured->second;
            instance = materialized->second;
        });
        if (factory.relocation.kind != detail::factory_relocation_kind_t::preserve_state)
            co_return std::vector<std::uint8_t>{};
        const auto capture = factory.relocation.capture;
        if (!capture) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "State-preserving Actor relocation has no capture callback");
        }
        const auto captured = co_await capture (instance.get (), cancellation);
        if (captured.size () > max_spot_relocation_state_bytes) {
            throw framework_exception_t (framework_error_kind_t::rejected,
                                         "Actor relocation state exceeds the 64 MiB limit");
        }
        std::vector<std::uint8_t> output;
        output.reserve (captured.size ());
        for (const auto value : captured)
            output.push_back (std::to_integer<std::uint8_t> (value));
        co_return output;
    }

    std::shared_ptr<spot_context_state_t> context;
    detail::factory_relocation_configuration_t relocation;
    co_await state->lane.run_task ([&] {
        const auto found = state->spot_contexts_by_id.find (spot.key);
        const auto configured = state->spot_factory_relocations.find (stable_type);
        const auto candidate = found == state->spot_contexts_by_id.end ()
                                 ? nullptr
                                 : spot_context_access_t::state (found->second);
        if (!candidate || candidate->spot_name != stable_type
            || configured == state->spot_factory_relocations.end ()
            || !candidate->allows_relocation ()) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "Relocation source Spot is not materialized");
        }
        context = candidate;
        relocation = configured->second;
    });
    if (relocation.kind != detail::factory_relocation_kind_t::preserve_state)
        co_return std::vector<std::uint8_t>{};
    const auto capture = relocation.capture;
    if (!capture || !context->spot_instance) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "State-preserving Spot relocation has no capture callback");
    }
    // Barrier cancellation can settle the caller while the capture still runs, so the
    // coroutine frame owns everything it touches; the caller reads payload only on success.
    auto payload = std::make_shared<std::vector<std::byte>> ();
    // The barrier may keep these callables after the phase, so they hold weak references;
    // only the running frame extends the payload and the Spot context.
    const auto work = [weak_payload = std::weak_ptr (payload), capture,
                       weak_context = std::weak_ptr (context), cancellation] {
        auto owned_payload = weak_payload.lock ();
        auto owned_context = weak_context.lock ();
        if (!owned_payload || !owned_context)
            return task_t<void> (
              result_t<void>::failure (framework_error_kind_t::shutting_down,
                                       "Spot relocation capture caller has returned"));
        return capture_relocation_payload (std::move (owned_payload), capture,
                                           std::move (owned_context), cancellation);
    };
    const auto barrier =
      co_await state->lane.run_task ([&] { return context->relocation_ready_barrier; });
    const auto capture_task = [work, weak_context = std::weak_ptr (context)] {
        const auto owned_context = weak_context.lock ();
        if (!owned_context)
            return task_t<void> (
              result_t<void>::failure (framework_error_kind_t::shutting_down,
                                       "Spot relocation capture caller has returned"));
        return owned_context->run_serial_task ("spot-relocation-capture", work);
    };
    co_await (barrier ? barrier->run (capture_task) : capture_task ());
    std::vector<std::uint8_t> output;
    if (payload->size () > max_spot_relocation_state_bytes) {
        throw framework_exception_t (framework_error_kind_t::rejected,
                                     "Spot relocation state exceeds the 64 MiB limit");
    }
    output.reserve (payload->size ());
    for (const auto value : *payload)
        output.push_back (std::to_integer<std::uint8_t> (value));
    co_return output;
}

task_t<runtime::stateful::frozen_object_state_t>
capture_relocation_state_async (std::shared_ptr<spot_node_builder_state_t> state,
                                runtime::stateful::object_ref_t object,
                                std::string stable_type,
                                std::stop_token cancellation)
{
    runtime::stateful::frozen_object_state_t captured;
    captured.owner = object;
    captured.stable_type = stable_type;
    captured.application_state =
      co_await capture_spot_relocation_state_async (state, object, stable_type, cancellation);
    if (object.kind != runtime::stateful::object_kind_t::actor)
        co_return captured;
    auto backlog = state->actor_transfer_coordinator->take_backlog (stable_type + ":" + object.key);
    std::vector<handoff_packet_t> legacy;
    captured.pending_application.reserve (backlog.size ());
    for (auto &packet : backlog) {
        if (!packet.application_record || !packet.application_record->payload ()) {
            legacy.push_back (std::move (packet));
            continue;
        }
        auto &record = packet.application_record;
        captured.pending_application.push_back (std::move (*record->payload ()));
        record->payload ().reset ();
        captured.pending_application.back ().accepted_owner = std::move (record);
    }
    state->actor_transfer_coordinator->prepend_source_suffix (stable_type + ":" + object.key,
                                                              std::move (legacy));
    co_return captured;
}

} // namespace

task_t<runtime::stateful::frozen_object_state_t>
spot_node_runtime_t::capture_relocation_state (const runtime::stateful::object_ref_t &object,
                                               const std::string &stable_type,
                                               std::stop_token cancellation) const
{
    return capture_relocation_state_async (_state, object, stable_type, cancellation);
}

task_t<bool>
spot_node_runtime_t::restore_spot_relocation_state (runtime::stateful::frozen_object_state_t frozen,
                                                    runtime::stateful::object_ref_t target,
                                                    std::stop_token cancellation)
{
    const auto state = _state;
    spot_node_runtime_t owner (state);
    // MeshNode §5.1: a relocation target's Restore holds one activation admission until the
    // target commit or asynchronous materialization abort, or its failure here.
    const auto admission_key = activation_admission_t::spot_key (target.key);
    if (!state->activation_admission->try_enter (admission_key))
        co_return false;
    std::uint64_t reservation = 0;
    std::shared_ptr<task_completion_source_t<void>> completion;
    std::exception_ptr failure;
    try {
        detail::factory_relocation_configuration_t relocation;
        const auto admitted = co_await state->lane.run_task ([&] {
            const auto configured = state->spot_factory_relocations.find (frozen.stable_type);
            if (configured == state->spot_factory_relocations.end ())
                return false;
            if (state->spot_contexts_by_id.contains (target.key)
                || state->pending_spot_creations_by_id.contains (target.key)
                || state->next_pending_spot_creation_reservation == 0)
                return false;
            reservation = state->next_pending_spot_creation_reservation++;
            completion = std::make_shared<task_completion_source_t<void>> ();
            const auto inserted = state->pending_spot_creations_by_id.emplace (
              target.key, detail::spot_node_builder_state_t::pending_spot_creation_t{
                            frozen.stable_type, completion, reservation});
            if (!inserted.second)
                return false;
            relocation = configured->second;
            return true;
        });
        if (!admitted) {
            state->activation_admission->leave (admission_key);
            co_return false;
        }
        if (frozen.application_state.size () > max_spot_relocation_state_bytes)
            throw std::length_error ("Spot relocation state exceeds 64 MiB");
        std::function<task_t<void> (void *)> staged_restore;
        if (relocation.kind == detail::factory_relocation_kind_t::preserve_state) {
            if (!relocation.restore)
                throw std::logic_error ("Spot relocation restore callback is missing");
            std::vector<std::byte> payload;
            payload.reserve (frozen.application_state.size ());
            for (const auto value : frozen.application_state)
                payload.push_back (static_cast<std::byte> (value));
            staged_restore = [relocation = std::move (relocation), payload = std::move (payload),
                              cancellation] (void *instance) mutable {
                return relocation.restore (instance, std::move (payload), cancellation);
            };
        } else if (relocation.kind == detail::factory_relocation_kind_t::recreate) {
            if (!frozen.application_state.empty ())
                throw std::logic_error ("Recreated Spot has application state");
            staged_restore = [] (void *) -> task_t<void> { co_return; };
        } else {
            throw std::logic_error ("Spot relocation policy does not permit restore");
        }
        const auto owned = co_await state->lane.run_task ([&] {
            const auto found = state->pending_spot_creations_by_id.find (target.key);
            return found != state->pending_spot_creations_by_id.end ()
                   && found->second.reservation == reservation;
        });
        if (!owned)
            throw std::logic_error ("Relocation Spot reservation ownership was lost");
        auto materialization = co_await owner.create_spot_context (
          frozen.stable_type, spot_id_t (target.key), zlink::message_t{}, target.object_generation,
          target.mesh_name, std::move (staged_restore), target.authority_owner_generation);
        auto materialized = std::move (*materialization);
        const auto created = materialized.state == spot_create_state_t::created;
        if (created && materialized.context._state
            && materialized.context._state->execution_mode == user_spot_execution_mode_t::spot_wide
            && materialized.context._state->relocation_coordination_mode
                 == spot_relocation_coordination_mode_t::application_signaled) {
            co_await state->lane.run_task ([state = materialized.context._state] {
                if (state->serial_queue)
                    state->serial_queue->invalidate_execution_ready ();
                state->relocation_boundary_active = true;
                state->relocation_ready_deferred = true;
            });
        }
        const auto completed_reservation = co_await state->lane.run_task ([&] {
            const auto current = state->pending_spot_creations_by_id.find (target.key);
            if (current == state->pending_spot_creations_by_id.end ()
                || current->second.reservation != reservation) {
                return false;
            }
            state->pending_spot_creations_by_id.erase (current);
            return true;
        });
        if (completed_reservation) {
            if (created)
                completion->complete (result_t<void>::success ());
            else
                completion->complete (
                  result_t<void>::failure (framework_error_kind_t::internal_failure,
                                           "Relocation Spot activation was rejected"));
        }
        if (!created)
            state->activation_admission->leave (admission_key);
        co_return created;
    }
    catch (...) {
        failure = std::current_exception ();
    }
    {
        state->activation_admission->leave (admission_key);
        if (reservation != 0) {
            const auto error = failure;
            const auto owned = co_await state->lane.run_task ([&] {
                const auto current = state->pending_spot_creations_by_id.find (target.key);
                if (current == state->pending_spot_creations_by_id.end ()
                    || current->second.reservation != reservation) {
                    return false;
                }
                state->pending_spot_creations_by_id.erase (current);
                return true;
            });
            if (owned)
                completion->complete (detail::result_access_t::failure<void> (error));
        }
    }
    co_return false;
}

std::optional<bool> spot_node_runtime_t::validate_actor_join_relocation_prepare (
  const runtime::protocol::relocation_prepare_t &prepare) const
{
    if (prepare.object.kind != runtime::protocol::relocation_object_kind_t::actor)
        return std::nullopt;
    const auto handoff_id = runtime::protocol::actor_join_handoff_id (prepare.relocation);
    const auto admission = _state->actor_transfer_coordinator->admission (handoff_id);
    if (!admission || !admission->canonical_application_join)
        return std::nullopt;
    struct validation_projection_t
    {
        std::shared_ptr<spot_context_state_t> target;
        std::shared_ptr<service::mesh_node_t> native_node;
        bool matches = false;
    };
    const auto projection =
      _state->lane
        .run ([&] {
            validation_projection_t result;
            const auto target_context =
              _state->spot_contexts_by_id.find (std::string (admission->target_spot_id));
            if (target_context == _state->spot_contexts_by_id.end ()
                || !target_context->second._state)
                return result;
            const auto &target = *target_context->second._state;
            result.target = target_context->second._state;
            if (target.node)
                result.native_node = target.node->native_node.lock ();
            const auto source_node =
              zlink::routing_id_t::from (std::string (admission->source_actor.node_rid ().value ()))
                .to_bytes ();
            const auto local_node = _state->snapshot.routing_id
                                      ? _state->snapshot.routing_id->to_bytes ()
                                      : std::vector<std::uint8_t>{};
            result.matches =
              !local_node.empty () && !target.is_instance_spot ()
              && (target.is_entry_spot () ? prepare.target.target_node_generation
                                          : target.object_generation)
                   == admission->target_spot_generation
              && prepare.object.object_id == admission->source_actor.actor_id ().value ()
              && prepare.object.object_generation == admission->source_actor.object_generation ()
              && prepare.object.expected_authority_owner_generation
                   == admission->source_actor_authority_owner_generation
              && prepare.source_node_routing_id == source_node
              && prepare.source_node_generation == admission->source_node_generation
              && prepare.coordinator.node_routing_id == source_node
              && prepare.coordinator.node_generation == admission->source_node_generation
              && prepare.coordinator.lease_generation == admission->source_owner_lease_generation
              && prepare.target.target_node_routing_id == local_node;
            return result;
        })
        .get ();
    if (!projection.target || !projection.matches)
        return false;
    const auto target_node_generation =
      projection.native_node ? projection.native_node->status ().lifecycle_generation () : 0;
    return prepare.target.target_node_generation == target_node_generation;
}

task_t<bool> spot_node_runtime_t::consume_actor_join_recovery_async (
  runtime::stateful::frozen_object_state_t &frozen,
  runtime::stateful::object_ref_t target,
  runtime::protocol::relocation_prepare_t prepare)
{
    const auto consume = [] (spot_node_runtime_t self,
                             runtime::stateful::frozen_object_state_t &frozen,
                             runtime::stateful::object_ref_t target,
                             runtime::protocol::relocation_prepare_t prepare) -> task_t<bool> {
        std::optional<runtime::protocol::actor_join_recovery_t> recovery;
        auto recovery_record = frozen.pending_application.end ();
        try {
            for (auto current = frozen.pending_application.begin ();
                 current != frozen.pending_application.end (); ++current) {
                std::optional<runtime::protocol::frozen_record_t> decoded_record;
                const runtime::protocol::frozen_record_t *canonical = nullptr;
                if (current->frozen_record)
                    canonical = &*current->frozen_record;
                else if (!current->payload.empty ()) {
                    decoded_record = runtime::protocol::decode_frozen_record (current->payload);
                    canonical = &*decoded_record;
                }
                if (!canonical)
                    continue;
                auto decoded =
                  runtime::protocol::decode_actor_join_recovery_saved_work (*canonical);
                if (!decoded)
                    continue;
                if (recovery)
                    co_return false;
                recovery = std::move (*decoded);
                recovery_record = current;
            }
        }
        catch (...) {
            co_return false;
        }
        if (!recovery) {
            /* A matching command-40 admission is the canonical Join path.  It
         * cannot fall through to ordinary Actor relocation (and ultimately
         * the Entry Spot) without its durable ZLJR completion record.  A
         * non-Join/legacy relocation has no matching admission, so preserve
         * that path unchanged. */
            const auto join_prepare = self.validate_actor_join_relocation_prepare (prepare);
            co_return !join_prepare || !*join_prepare;
        }

        const auto admission =
          co_await self._state->actor_transfer_coordinator->admission_async (recovery->handoff_id);
        co_return co_await self._state->lane.run_task ([&] {
            const auto local_node = self._state->snapshot.routing_id
                                      ? self._state->snapshot.routing_id->to_bytes ()
                                      : std::vector<std::uint8_t>{};
            const auto valid =
              admission && !local_node.empty () && recovery->relocation == prepare.relocation
              && recovery->coordinator == prepare.coordinator
              && recovery->handoff_id
                   == runtime::protocol::actor_join_handoff_id (prepare.relocation)
              && recovery->actor_id == frozen.owner.key
              && recovery->actor_type == frozen.stable_type
              && recovery->actor_generation == frozen.owner.object_generation
              && recovery->actor_authority_owner_generation
                   == frozen.owner.authority_owner_generation
              && recovery->target_authority_owner_generation
                   == recovery->actor_authority_owner_generation
              && target.key == recovery->actor_id
              && target.object_generation == recovery->actor_generation
              && target.authority_owner_generation == frozen.owner.authority_owner_generation
              && recovery->target_node_routing_id == local_node
              && recovery->target_node_generation == prepare.target.target_node_generation
              && recovery->target_spot_id == std::string (admission->target_spot_id)
              && recovery->target_spot_generation == admission->target_spot_generation
              && recovery->target_spot_authority_owner_generation
                   == admission->target_spot_authority_owner_generation
              && recovery->reservation_token == admission->reservation_token
              && recovery->reserved_payload_bytes == admission->reserved_payload_bytes
              && recovery->actor_node_generation == admission->source_node_generation
              && recovery->expected_owner_lease_generation
                   == admission->source_owner_lease_generation;
            if (!valid)
                return false;
            const auto key = actor_key (::zlink::framework::detail::actor_ref_access_t::make (
              node_rid_t::from_string (target.node_id), frozen.stable_type, target.key,
              target.object_generation));
            if (!self._state->actor_join_relocation_recoveries
                   .emplace (key,
                             detail::spot_node_builder_state_t::actor_join_relocation_recovery_t{
                               recovery->handoff_id, spot_id_t (recovery->source_spot_id),
                               admission->target_spot_id, recovery->target_node_generation,
                               recovery->target_spot_generation, admission->source_actor,
                               recovery->operation.high, recovery->operation.low, recovery->reply})
                   .second)
                return false;
            frozen.pending_application.erase (recovery_record);
            return true;
        });
    };
    return consume (spot_node_runtime_t (_state), frozen, std::move (target), std::move (prepare));
}

namespace
{
task_t<actor_transfer_coordinator_t::backlog_result_t>
discard_actor_join_recovery_on_node_async (std::shared_ptr<spot_node_builder_state_t> node,
                                           std::string key,
                                           std::function<void ()> discard_materialization = {})
{
    const auto handoff = co_await node->lane.run_task ([&] () -> std::optional<std::string> {
        const auto found = node->actor_join_relocation_recoveries.find (key);
        return found == node->actor_join_relocation_recoveries.end ()
                 ? std::nullopt
                 : std::make_optional (found->second.handoff_id);
    });
    actor_transfer_coordinator_t::backlog_result_t discarded;
    if (handoff)
        discarded = co_await node->actor_transfer_coordinator->fail_commit_async (*handoff, false);
    else if (!discard_materialization)
        co_return discarded;
    co_await node->lane.run_task ([&] {
        const auto found = node->actor_join_relocation_recoveries.find (key);
        if (found != node->actor_join_relocation_recoveries.end () && handoff
            && found->second.handoff_id == *handoff)
            node->actor_join_relocation_recoveries.erase (found);
        if (discard_materialization)
            discard_materialization ();
    });
    co_return discarded;
}
} // namespace

task_t<void>
spot_node_runtime_t::discard_actor_join_recovery_async (std::string stable_type,
                                                        runtime::stateful::object_ref_t target)
{
    const auto discard = [] (std::shared_ptr<spot_node_builder_state_t> node,
                             std::string stable_type,
                             runtime::stateful::object_ref_t target) -> task_t<void> {
        if (target.kind != runtime::stateful::object_kind_t::actor || stable_type.empty ()
            || target.key.empty ())
            co_return;
        const auto discarded =
          co_await discard_actor_join_recovery_on_node_async (node, stable_type + ":" + target.key);
        if (discarded)
            fail_handoff_backlog (node, std::move (*discarded));
    };
    return discard (_state, std::move (stable_type), std::move (target));
}

namespace
{
std::optional<runtime::actor_authority_payload_t>
actor_relocation_authority_on_lane (const detail::spot_node_builder_state_t &state,
                                    const runtime::stateful::object_ref_t &target)
{
    if (target.kind != runtime::stateful::object_kind_t::actor)
        return std::nullopt;
    const auto type = state.actor_types_by_id.find (target.key);
    const auto recovery = std::find_if (
      state.actor_join_relocation_recoveries.begin (),
      state.actor_join_relocation_recoveries.end (), [&target] (const auto &candidate) {
          return candidate.second.source_actor.actor_id ().value () == target.key
                 && candidate.second.source_actor.object_generation () == target.object_generation;
      });
    std::string spot_id;
    if (recovery != state.actor_join_relocation_recoveries.end ())
        spot_id = recovery->second.target_spot_id;
    else {
        if (!state.snapshot.entry_spot_name)
            return std::nullopt;
        const auto entry = state.spot_ids_by_name.find (*state.snapshot.entry_spot_name);
        if (entry == state.spot_ids_by_name.end ())
            return std::nullopt;
        spot_id = entry->second;
    }
    const auto found = state.spot_contexts_by_id.find (spot_id);
    if (found == state.spot_contexts_by_id.end ())
        return std::nullopt;
    const auto context = spot_context_access_t::state (found->second);
    if (!context || !context->spot_instance)
        return std::nullopt;
    const auto &spot = *context;
    return runtime::actor_authority_payload_t{
      .stable_type = recovery != state.actor_join_relocation_recoveries.end ()
                       ? std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (
                           recovery->second.source_actor))
                       : (type != state.actor_types_by_id.end () ? type->second : std::string{}),
      .actor_id = target.key,
      .current_spot_id = spot_id,
      .current_spot_generation = recovery != state.actor_join_relocation_recoveries.end ()
                                   ? recovery->second.target_spot_generation
                                   : spot.object_generation,
      .current_spot_kind = spot.is_entry_spot () ? runtime::actor_authority_spot_kind_t::entry
                                                 : runtime::actor_authority_spot_kind_t::user,
      .mesh_name = target.mesh_name,
      .node_rid = node_rid_t::from_string (target.node_id)};
}
} // namespace

std::optional<runtime::actor_authority_payload_t> spot_node_runtime_t::actor_relocation_authority (
  const runtime::stateful::object_ref_t &target) const
{
    return _state->lane.run ([&] { return actor_relocation_authority_on_lane (*_state, target); })
      .get ();
}

task_t<bool> spot_node_runtime_t::materialize_relocation_state (
  const runtime::stateful::frozen_object_state_t &frozen,
  const runtime::stateful::object_ref_t &target,
  const std::optional<runtime::stateful::object_ref_t> &target_spot,
  std::stop_token cancellation)
{
    return materialize_relocation_state_in (spot_node_runtime_t (_state), frozen, target,
                                            target_spot, std::move (cancellation));
}

task_t<bool> spot_node_runtime_t::materialize_relocation_state_in (
  spot_node_runtime_t self,
  runtime::stateful::frozen_object_state_t frozen,
  runtime::stateful::object_ref_t target,
  std::optional<runtime::stateful::object_ref_t> target_spot,
  std::stop_token cancellation)
{
    if (target.kind != runtime::stateful::object_kind_t::actor)
        co_return co_await self.restore_spot_relocation_state (frozen, target, cancellation);

    // MeshNode §5.1: a standalone Actor Restore holds one activation admission until the
    // target commit or abort. An Actor restored into a Spot whose own Restore holds an
    // admission belongs to that Spot's Restore.
    auto &admission = *self._state->activation_admission;
    if (target_spot && admission.holds (activation_admission_t::spot_key (target_spot->key)))
        co_return co_await self.materialize_actor_relocation_state (
          std::move (frozen), std::move (target), std::move (target_spot), cancellation);
    const auto admission_key = activation_admission_t::actor_key (target.key);
    if (!admission.try_enter (admission_key))
        co_return false;
    bool materialized = false;
    try {
        materialized = co_await self.materialize_actor_relocation_state (
          std::move (frozen), std::move (target), std::move (target_spot), cancellation);
    }
    catch (...) {
        admission.leave (admission_key);
        throw;
    }
    if (!materialized)
        admission.leave (admission_key);
    co_return materialized;
}

task_t<bool> spot_node_runtime_t::materialize_actor_relocation_state (
  runtime::stateful::frozen_object_state_t frozen,
  runtime::stateful::object_ref_t target,
  std::optional<runtime::stateful::object_ref_t> target_spot,
  std::stop_token cancellation)
{
    co_await _state->actor_transfer_coordinator->wait_for_source_completion (frozen.stable_type
                                                                             + ":" + target.key);
    detail::spot_node_builder_state_t::actor_factory_registration_t factory;
    std::shared_ptr<spot_context_state_t> context;
    actor_gateway_runtime_t *actor_gateway = nullptr;
    std::optional<std::pair<std::string, actor_ref_t>> join_recovery_identity;
    runtime::stateful::object_ref_t resolved_target_spot;
    std::optional<service_provider_t> root_services;
    const auto preparation_result = co_await _state->lane.run_task ([&] () -> std::optional<bool> {
        // A single-Actor relocation unit (e.g. an Entry Spot Actor moving
        // alone) never carries a Spot on the wire (spec 28: the Entry Spot
        // is already present on the target node). Resolve this node's own
        // local Entry Spot as the target Spot in that case, the same way
        // the join path does.
        const auto actor_key_value = frozen.stable_type + ":" + target.key;
        const auto join_recovery = _state->actor_join_relocation_recoveries.find (actor_key_value);
        if (join_recovery != _state->actor_join_relocation_recoveries.end ())
            join_recovery_identity.emplace (join_recovery->second.handoff_id,
                                            join_recovery->second.source_actor);
        if (target_spot) {
            if (target_spot->kind != runtime::stateful::object_kind_t::user_spot)
                return false;
            resolved_target_spot = *target_spot;
        } else {
            const auto membership = actor_relocation_authority_on_lane (*_state, target);
            if (!membership) {
                return std::make_optional (
                  !join_recovery_identity && !_state->snapshot.entry_spot_name
                  && !_state->actor_factories.contains (frozen.stable_type));
            }
            resolved_target_spot =
              runtime::stateful::object_ref_t{runtime::stateful::object_kind_t::user_spot,
                                              membership->current_spot_id,
                                              membership->current_spot_generation,
                                              0,
                                              target.mesh_name,
                                              target.node_id};
        }
        if (join_recovery_identity) {
            if (resolved_target_spot.key != std::string (join_recovery->second.target_spot_id)
                || resolved_target_spot.object_generation
                     != join_recovery->second.target_spot_generation) {
                return false;
            }
        }
        return std::nullopt;
    });
    if (preparation_result)
        co_return *preparation_result;
    if (join_recovery_identity) {
        const auto &[handoff, source_actor] = *join_recovery_identity;
        auto [admitted, discarded] =
          co_await _state->actor_transfer_coordinator->begin_commit_async (
            handoff, source_actor, spot_id_t (resolved_target_spot.key));
        if (discarded)
            fail_handoff_backlog (_state, std::move (*discarded));
        if (!admitted)
            co_return false;
    }
    const auto prepared = co_await _state->lane.run_task ([&] {
        if (join_recovery_identity) {
            const auto current =
              _state->actor_join_relocation_recoveries.find (frozen.stable_type + ":" + target.key);
            if (current == _state->actor_join_relocation_recoveries.end ()
                || current->second.handoff_id != join_recovery_identity->first)
                return false;
        }
        const auto configured = _state->actor_factories.find (frozen.stable_type);
        const auto found_context = _state->spot_contexts_by_id.find (resolved_target_spot.key);
        if (configured == _state->actor_factories.end ()
            || found_context == _state->spot_contexts_by_id.end () || !found_context->second._state
            || !found_context->second._state->spot_instance)
            return false;
        factory = configured->second;
        context = found_context->second._state;
        root_services = _state->root_services;
        return true;
    });
    if (!prepared)
        co_return false;
    if (root_services)
        actor_gateway = &root_services->get_required<actor_gateway_runtime_t> ();

    if (frozen.application_state.size () > max_spot_relocation_state_bytes)
        co_return false;
    if (factory.relocation.kind == detail::factory_relocation_kind_t::disabled
        || factory.relocation.kind == detail::factory_relocation_kind_t::unspecified)
        co_return false;
    if (factory.relocation.kind == detail::factory_relocation_kind_t::recreate
        && !frozen.application_state.empty ())
        co_return false;
    if (factory.relocation.kind == detail::factory_relocation_kind_t::preserve_state
        && !factory.restore)
        co_return false;

    const auto committed = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (target.node_id), frozen.stable_type, target.key,
      target.object_generation);
    actor_gateway_runtime_t local_actor_gateway;
    auto actor_context = actor_gateway != nullptr ? actor_gateway->actor_context (committed)
                                                  : local_actor_gateway.actor_context (committed);
    std::shared_ptr<void> actor;
    try {
        actor = factory.create_context_instance
                  ? factory.create_context_instance (std::move (actor_context))
                  : factory.create_instance (target.key);
        if (actor && !factory.create_context_instance && factory.configure_instance)
            factory.configure_instance (actor.get (), committed, &actor_context);
        if (!actor)
            co_return false;
        if (factory.relocation.kind == detail::factory_relocation_kind_t::preserve_state) {
            std::vector<std::byte> payload;
            payload.reserve (frozen.application_state.size ());
            for (const auto value : frozen.application_state)
                payload.push_back (static_cast<std::byte> (value));
            // A failed restore throws here and ends the materialization below.
            co_await factory.restore (actor.get (), std::move (payload), cancellation);
        }
    }
    catch (...) {
        co_return false;
    }

    struct return_relocation_remnant_t
    {
        actor_ref_t source_actor;
        runtime::protocol::actor_route_fence_t source_fence;
        std::string transfer_id;
        std::shared_ptr<void> actor;
        std::shared_ptr<spot_context_state_t> context;
        std::function<task_t<void> (void *, void *)> leave_callback;
        bool submitted_leave = false;
    };

    std::function<result_t<void> (const actor_ref_t &)> update_registry;
    const auto key = actor_key (committed);
    std::optional<return_relocation_remnant_t> return_remnant;
    const auto remnant_prepared =
      _state->lane
        .run ([&] {
            const auto existing_actor = _state->actor_instances.find (key);
            const auto existing_spot = _state->actor_spot_ids.find (key);
            if (existing_actor != _state->actor_instances.end ()
                || existing_spot != _state->actor_spot_ids.end ()) {
                const auto current_fence = _state->actor_authority_fences.find (key);
                const auto existing_route = _state->actor_routes.find (key);
                const auto existing_generation = _state->actor_generations.find (key);
                const auto local_node = zlink::routing_id_t::from (target.node_id).to_bytes ();
                const bool has_source_cleanup = std::ranges::any_of (
                  _state->pending_remote_source_cleanups, [&] (const auto &candidate) {
                      return candidate.source_actor.actor_id ().value () == target.key
                             && ::zlink::framework::detail::actor_ref_access_t::actor_type (
                                  candidate.source_actor)
                                  == frozen.stable_type
                             && candidate.source_actor.object_generation ()
                                  == target.object_generation
                             && candidate.source_actor.node_rid ().value () == target.node_id
                             && candidate.source_fence.actor_id == target.key
                             && candidate.source_fence.object_generation == target.object_generation
                             && candidate.source_fence.target_node_routing_id == local_node
                             && candidate.source_fence.authority_owner_generation
                                  < target.authority_owner_generation;
                  });
                const bool exact_generation =
                  existing_generation != _state->actor_generations.end ()
                  && existing_generation->second == frozen.owner.object_generation;
                const bool replaceable_departed_cache =
                  !has_source_cleanup && current_fence == _state->actor_authority_fences.end ()
                  && exact_generation
                  && ((existing_route != _state->actor_routes.end ()
                       && existing_route->second.node_rid.value () == frozen.owner.node_id)
                      || (existing_actor == _state->actor_instances.end ()
                          && existing_spot != _state->actor_spot_ids.end ()
                          && existing_route == _state->actor_routes.end ()));
                if (replaceable_departed_cache) {
                    // Source membership cleanup already erased the old instance;
                    // or its local map erase lagged behind the already-remote
                    // route. With no local authority fence, the Store-validated
                    // frozen source is the exact current-owner route, so the newer
                    // returning target may replace both forms of departed cache.
                    if (existing_actor != _state->actor_instances.end ()) {
                        detail::erase_actor_instance_index_unlocked (*_state, frozen.stable_type,
                                                                     target.key);
                        _state->actor_instances.erase (existing_actor);
                    }
                    erase_actor_route_unlocked (*_state, key);
                } else {
                    auto cleanup = std::find_if (
                      _state->pending_remote_source_cleanups.begin (),
                      _state->pending_remote_source_cleanups.end (), [&] (const auto &candidate) {
                          return candidate.source_actor.actor_id ().value () == target.key
                                 && ::zlink::framework::detail::actor_ref_access_t::actor_type (
                                      candidate.source_actor)
                                      == frozen.stable_type
                                 && candidate.source_actor.object_generation ()
                                      == target.object_generation
                                 && candidate.source_actor.node_rid ().value () == target.node_id
                                 && candidate.source_fence.actor_id == target.key
                                 && candidate.source_fence.object_generation
                                      == target.object_generation
                                 && candidate.source_fence.target_node_routing_id == local_node
                                 && candidate.source_fence.authority_owner_generation
                                      < target.authority_owner_generation;
                      });
                    if (cleanup == _state->pending_remote_source_cleanups.end ()) {
                        return false;
                    }
                    if (frozen.owner.object_generation != cleanup->source_fence.object_generation
                        || frozen.owner.authority_owner_generation
                             <= cleanup->source_fence.authority_owner_generation
                        || frozen.owner.authority_owner_generation
                             > target.authority_owner_generation
                        || (current_fence != _state->actor_authority_fences.end ()
                            && current_fence->second != cleanup->source_fence))
                        return false;

                    return_remnant = return_relocation_remnant_t{
                      .source_actor = cleanup->source_actor,
                      .source_fence = cleanup->source_fence,
                      .transfer_id = cleanup->transfer_id,
                      .actor = existing_actor != _state->actor_instances.end ()
                                 ? existing_actor->second
                                 : std::shared_ptr<void>{},
                      .submitted_leave = cleanup->leave_submitted};
                    if (!cleanup->leave_submitted) {
                        cleanup->leave_submitted = true;
                        const auto source_context =
                          _state->spot_contexts_by_id.find (std::string (cleanup->source_spot_id));
                        if (source_context != _state->spot_contexts_by_id.end ()
                            && source_context->second._state
                            && source_context->second._state->spot_instance) {
                            return_remnant->context = source_context->second._state;
                            decrement_actor_count_unlocked (*return_remnant->context);
                            const auto source_admission =
                              return_remnant->context->actor_admissions.find (factory.actor_type);
                            if (source_admission
                                != return_remnant->context->actor_admissions.end ()) {
                                return_remnant->leave_callback =
                                  source_admission->second.on_leave_actor;
                            }
                        }
                        if (!return_remnant->leave_callback) {
                            cleanup->leave_completed = true;
                            if (const auto host = _state->native_node.lock ())
                                host->signal_dispatch_activity ();
                        }
                    }
                }
            }
            return true;
        })
        .get ();
    if (!remnant_prepared)
        co_return false;

    if (return_remnant && !return_remnant->submitted_leave && return_remnant->leave_callback
        && return_remnant->context && return_remnant->actor) {
        // The materialization continues when the source leave item completes;
        // its outcome does not change the return (as before).
        try {
            co_await return_remnant->context->run_serial_task (
              "spot-actor-return-relocation-leave", [&] {
                  return return_remnant->leave_callback (
                    return_remnant->context->spot_instance.get (), return_remnant->actor.get ());
              });
        }
        catch (...) {
            report_actor_leave_failure (
              _state, detail::result_access_t::failure<void> (std::current_exception ()),
              return_remnant->context->spot_id, return_remnant->source_fence.actor_id,
              return_remnant->transfer_id);
        }
        _state->lane
          .run ([&] {
              const auto cleanup = std::find_if (
                _state->pending_remote_source_cleanups.begin (),
                _state->pending_remote_source_cleanups.end (), [&] (const auto &candidate) {
                    return candidate.transfer_id == return_remnant->transfer_id
                           && candidate.source_fence == return_remnant->source_fence;
                });
              if (cleanup != _state->pending_remote_source_cleanups.end ()) {
                  cleanup->leave_completed = true;
                  if (const auto host = _state->native_node.lock ())
                      host->signal_dispatch_activity ();
              }
          })
          .get ();
    }

    const auto target_native_node = _state->lane
                                      .run ([&] {
                                          return context->node
                                                   ? context->node->native_node.lock ()
                                                   : std::shared_ptr<service::mesh_node_t>{};
                                      })
                                      .get ();
    const auto target_node_generation =
      target_native_node ? target_native_node->status ().lifecycle_generation () : 0;
    const auto installed =
      _state->lane
        .run ([&] {
            const auto found_context = _state->spot_contexts_by_id.find (resolved_target_spot.key);
            const auto existing_actor = _state->actor_instances.find (key);
            if (return_remnant) {
                if (existing_actor != _state->actor_instances.end ()
                    && existing_actor->second != return_remnant->actor) {
                    return false;
                }
                const auto current_fence = _state->actor_authority_fences.find (key);
                if (current_fence != _state->actor_authority_fences.end ()
                    && current_fence->second != return_remnant->source_fence) {
                    return false;
                }
                if (existing_actor != _state->actor_instances.end ()) {
                    detail::erase_actor_instance_index_unlocked (
                      *_state,
                      ::zlink::framework::detail::actor_ref_access_t::actor_type (
                        return_remnant->source_actor),
                      return_remnant->source_actor.actor_id ().value ());
                    _state->actor_instances.erase (existing_actor);
                }
                erase_actor_route_unlocked (*_state, key);
            }
            if (found_context == _state->spot_contexts_by_id.end ()
                || found_context->second._state.get () != context.get ()
                || _state->actor_instances.contains (key)
                || _state->actor_spot_ids.contains (key)) {
                return false;
            }
            const auto found_admission = context->actor_admissions.find (factory.actor_type);
            if (found_admission == context->actor_admissions.end ())
                return false;
            detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
            _state->actor_instances.emplace (key, actor);
            _state->actor_types_by_id.insert_or_assign (target.key, frozen.stable_type);
            _state->destroyed_actor_keys.erase (key);
            detail::record_actor_context_route_unlocked (*_state, key, target.node_id, *context,
                                                         committed.object_generation ());
            const auto staged_fence = runtime::protocol::actor_route_fence_t{
              target.key,
              target.object_generation,
              zlink::routing_id_t::from (target.node_id).to_bytes (),
              target_node_generation,
              target.authority_owner_generation,
              0};
            const auto current_fence = _state->actor_authority_fences.find (key);
            const bool current_is_newer_same_target =
              current_fence != _state->actor_authority_fences.end ()
              && current_fence->second.actor_id == staged_fence.actor_id
              && current_fence->second.object_generation == staged_fence.object_generation
              && current_fence->second.target_node_routing_id == staged_fence.target_node_routing_id
              && current_fence->second.target_node_generation == staged_fence.target_node_generation
              && (current_fence->second.authority_owner_generation
                    > staged_fence.authority_owner_generation
                  || (current_fence->second.authority_owner_generation
                        == staged_fence.authority_owner_generation
                      && current_fence->second.owner_lease_generation != 0));
            if (!current_is_newer_same_target)
                _state->actor_authority_fences.insert_or_assign (key, staged_fence);
            if (const auto coordinator = context->ensure_spot_serial_executor ())
                (void) coordinator->ensure_actor_queue (key);
            update_registry = _state->update_actor_registry_ref;
            return true;
        })
        .get ();
    if (!installed)
        co_return false;

    auto updated = result_t<void>::success ();
    try {
        if (update_registry)
            updated = update_registry (committed);
    }
    catch (...) {
        updated = detail::current_exception_result<void> ();
    }
    if (!updated) {
        std::vector<runtime::stateful::object_ref_t> aborted_targets{target};
        co_await abort_relocation_materialization_async (std::move (aborted_targets));
        co_return false;
    }
    co_return true;
}

bool spot_node_runtime_t::commit_relocation_materialization (
  const std::vector<runtime::stateful::object_ref_t> &targets)
{
    end_relocation_activation_admissions (targets);
    std::vector<std::shared_ptr<spot_context_state_t>> ready;
    std::vector<std::pair<std::string, runtime::stateful::object_ref_t>> owner_fences;
    struct actor_join_completion_record_t
    {
        std::string handoff_id;
        actor_ref_t source_actor;
        actor_ref_t actor;
        spot_id_t source_spot_id;
        spot_id_t target_spot_id;
        std::uint64_t target_node_generation = 0;
        std::uint64_t target_authority_owner_generation = 0;
        std::uint64_t target_owner_lease_generation = 0;
        std::uint64_t operation_high = 0;
        std::uint64_t operation_low = 0;
        std::vector<std::uint8_t> reply;
        std::shared_ptr<spot_context_state_t> target_context;
        std::shared_ptr<void> actor_instance;
        detail::spot_actor_admission_callbacks_t admission;
    };
    std::vector<actor_join_completion_record_t> actor_join_completions;
    std::shared_ptr<runtime::stateful::authority_relocation_port_t> authority_store;
    const auto prepared =
      _state->lane
        .run ([&] {
            authority_store = _state->relocation_authority;
            for (const auto &target : targets) {
                if (target.kind == runtime::stateful::object_kind_t::actor) {
                    const auto type = _state->actor_types_by_id.find (target.key);
                    if (type == _state->actor_types_by_id.end ())
                        continue;
                    const auto key = type->second + ":" + target.key;
                    const auto fence = _state->actor_authority_fences.find (key);
                    if (fence == _state->actor_authority_fences.end ())
                        return false;
                    owner_fences.emplace_back (key, target);
                    const auto recovery = _state->actor_join_relocation_recoveries.find (key);
                    if (recovery != _state->actor_join_relocation_recoveries.end ()) {
                        const auto context = _state->spot_contexts_by_id.find (
                          std::string (recovery->second.target_spot_id));
                        const auto instance = _state->actor_instances.find (key);
                        const auto factory = _state->actor_factories.find (type->second);
                        if (context == _state->spot_contexts_by_id.end () || !context->second._state
                            || instance == _state->actor_instances.end ()
                            || factory == _state->actor_factories.end ())
                            return false;
                        const auto admission = context->second._state->actor_admissions.find (
                          factory->second.actor_type);
                        if (admission == context->second._state->actor_admissions.end ())
                            return false;
                        actor_join_completions.push_back (
                          {recovery->second.handoff_id, recovery->second.source_actor,
                           ::zlink::framework::detail::actor_ref_access_t::make (
                             node_rid_t::from_string (target.node_id), type->second, target.key,
                             target.object_generation),
                           recovery->second.source_spot_id, recovery->second.target_spot_id,
                           recovery->second.target_node_generation,
                           target.authority_owner_generation, 0,
                           recovery->second.completion_operation_id_high,
                           recovery->second.completion_operation_id_low,
                           recovery->second.admission_reply, context->second._state,
                           instance->second, admission->second});
                    }
                    continue;
                }
                if (target.kind != runtime::stateful::object_kind_t::user_spot)
                    continue;
                const auto context = _state->spot_contexts_by_id.find (target.key);
                if (context != _state->spot_contexts_by_id.end () && context->second._state) {
                    ready.push_back (context->second._state);
                    owner_fences.emplace_back (target.key, target);
                }
            }
            return true;
        })
        .get ();
    if (!prepared)
        return false;
    if (!owner_fences.empty () && !authority_store)
        return false;
    for (const auto &[key, target] : owner_fences) {
        const auto authority = authority_store->read (target.kind, target.key);
        if (!authority || authority->target.kind != target.kind
            || authority->target.key != target.key
            || authority->target.object_generation != target.object_generation
            || authority->target.mesh_name != target.mesh_name
            || authority->target.node_id != target.node_id
            || authority->target.authority_owner_generation < target.authority_owner_generation
            || authority->target_owner.owner_id.empty ()
            || authority->target_owner.lease_generation <= 0)
            return false;
        const auto updated =
          _state->lane
            .run ([&] {
                if (target.kind != runtime::stateful::object_kind_t::actor) {
                    const auto context = _state->spot_contexts_by_id.find (key);
                    if (context == _state->spot_contexts_by_id.end () || !context->second._state)
                        return false;
                    // Restore predicted a generation; admission uses the committed authority.
                    context->second._state->authority_owner_generation =
                      authority->target.authority_owner_generation;
                    return true;
                }
                const auto fence = _state->actor_authority_fences.find (key);
                if (fence == _state->actor_authority_fences.end ())
                    return false;
                fence->second.authority_owner_generation =
                  authority->target.authority_owner_generation;
                fence->second.owner_lease_generation = authority->target_owner.lease_generation;
                for (auto &completion : actor_join_completions) {
                    if (completion.actor.actor_id ().value () == target.key
                        && completion.actor.object_generation () == target.object_generation) {
                        completion.target_authority_owner_generation =
                          authority->target.authority_owner_generation;
                        completion.target_owner_lease_generation =
                          authority->target_owner.lease_generation;
                        break;
                    }
                }
                return true;
            })
            .get ();
        if (!updated)
            return false;
    }
    for (const auto &state : ready) {
        if (const auto preparation = state->spot_serial_executor->preparation_lifecycle ()) {
            const auto initialized =
              preparation
                ->run ([state] () -> task_t<void> {
                    if (state->lifecycle.on_initialize && state->spot_instance)
                        co_await state->lifecycle.on_initialize (state->spot_instance.get ());
                    co_return;
                })
                .result ();
            if (!initialized)
                report_spot_close_diagnostic (_state, std::string (state->spot_id),
                                              "on_initialize_failed", initialized.exception ());
            state->complete_preparation (true);
        }
        _state->lane
          .run ([this, state] {
              state->relocation_boundary_active = false;
              if (_state->location_lifecycle) {
                  const runtime::stateful::object_ref_t object{
                    state->is_instance_spot () ? runtime::stateful::object_kind_t::instance_spot
                                               : runtime::stateful::object_kind_t::user_spot,
                    std::string (state->spot_id),
                    state->object_generation,
                    state->authority_owner_generation,
                    state->mesh_name,
                    std::string (state->node_rid.value ())};
                  if (auto gate = execution_queue_on_lane (object))
                      gate->confirm_execution_ready (
                        _state->location_lifecycle->execution_runtime ().execution_clock () ());
              }
          })
          .get ();
        state->complete_relocation_ready (spot_relocation_ready_outcome_t::relocated);
    }
    for (auto &completion : actor_join_completions) {
        if (actor_transfer_marker_enabled ()) {
            emit_actor_transfer_marker ("location_committed", completion.actor,
                                        completion.handoff_id, completion.target_spot_id);
        }
        auto completion_record =
          std::make_shared<actor_join_completion_record_t> (std::move (completion));
        auto fail_commit = [node = _state, completion_record] (result_t<void> failed) noexcept {
            try {
                (void) node->actor_transfer_coordinator->fail_commit (completion_record->handoff_id,
                                                                      true);
            }
            catch (...) {
                failed = detail::current_exception_result<void> ();
            }
            detail::dispatch_error_reporter_t (node->dispatch).report_lazy ([&] {
                return message_dispatch_error_event_t{
                  .surface = dispatch_error_surface_t::spot_actor,
                  .message_kind = dispatch_message_kind_t::actor_request,
                  .reason = dispatch_error_reason_t::handler_exception,
                  .action = dispatch_error_action_t::fail_caller,
                  .packet_name = "JoinSpot",
                  .spot_id = std::string (completion_record->target_spot_id),
                  .actor_id = std::string (completion_record->actor.actor_id ().value ()),
                  .correlation_id = completion_record->handoff_id,
                  .exception = failed.exception ()};
            });
        };
        auto complete_join = [node = _state, completion_record, fail_commit] () mutable {
            auto &completion = *completion_record;
            std::shared_ptr<join_completion_delivery_fence_scope_t> delivery_scope;
            try {
                auto root_services = node->lane.run ([&] { return node->root_services; }).get ();
                if (!root_services) {
                    fail_commit (result_t<void>::failure (
                      framework_error_kind_t::internal_failure,
                      "Actor Join completion root services are unavailable"));
                    return;
                }
                delivery_scope = join_completion_delivery_fence_scope_t::begin (
                  root_services->get_required<actor_gateway_runtime_t> (), completion.actor,
                  [fail_commit] {
                      fail_commit (result_t<void>::failure (
                        framework_error_kind_t::internal_failure, "Actor Join commit failed"));
                  });
            }
            catch (...) {
                fail_commit (detail::current_exception_result<void> ());
                return;
            }
            auto fail_join = [delivery_scope, fail_commit] (result_t<void> failed) mutable {
                delivery_scope->settle (std::move (failed), fail_commit);
            };
            auto finish_join = [node, completion_record, delivery_scope, fail_join,
                                fail_commit] (result_t<void> joined) mutable {
                auto &completion = *completion_record;
                auto runtime = spot_node_runtime_t (node);
                std::optional<message_t> reply;
                auto report_source_leave_failure = [node, completion_record] (auto &&build_error) {
                    detail::message_flow_tracer_t (node->dispatch)
                      .trace (
                        message_flow_outcome_t::completed, message_flow_result_t::failed, [&] {
                            return message_flow_event_t{
                              .outcome = message_flow_outcome_t::completed,
                              .surface = dispatch_error_surface_t::spot_actor,
                              .message_kind = dispatch_message_kind_t::control,
                              .packet_name = detail::spot_actor_leave_route_command_t::packet_name,
                              .correlation_id = completion_record->handoff_id,
                              .spot_id = std::string (completion_record->source_spot_id),
                              .actor_id =
                                std::string (completion_record->source_actor.actor_id ().value ()),
                              .exception = build_error (),
                              .result = message_flow_result_t::failed};
                        });
                };
                if (joined) {
                    try {
                        auto *serializers = node->lane
                                              .run ([&] {
                                                  return node->channel_runtime
                                                           ? node->channel_runtime->serializers
                                                           : nullptr;
                                              })
                                              .get ();
                        if (serializers == nullptr) {
                            throw framework_exception_t (
                              framework_error_kind_t::protocol_error,
                              "Actor Join completion serializer registry is unavailable");
                        }
                        try {
                            runtime::messaging::envelope_header_t leave_header;
                            leave_header.kind = runtime::messaging::message_kind_t::command;
                            leave_header.channel_name =
                              detail::spot_actor_leave_route_command_t::channel_name;
                            leave_header.message_name =
                              detail::spot_actor_leave_route_command_t::packet_name;
                            auto leave_parts = runtime::messaging::envelope_codec_t{}.encode_parts (
                              leave_header,
                              detail::spot_actor_leave_route_command_t{
                                .transfer_id = completion.handoff_id,
                                .actor_node_rid =
                                  std::string (completion.source_actor.node_rid ().value ()),
                                .actor_type = std::string (
                                  ::zlink::framework::detail::actor_ref_access_t::actor_type (
                                    completion.source_actor)),
                                .actor_id =
                                  std::string (completion.source_actor.actor_id ().value ()),
                                .actor_generation = completion.source_actor.object_generation (),
                                .source_spot_id = std::string (completion.source_spot_id),
                                .source_spot_generation = 0,
                                .target_spot_id = std::string (completion.target_spot_id),
                                .target_node_rid =
                                  std::string (completion.actor.node_rid ().value ()),
                                .target_node_generation = completion.target_node_generation,
                                .target_authority_owner_generation =
                                  completion.target_authority_owner_generation,
                                .target_owner_lease_generation =
                                  completion.target_owner_lease_generation},
                              *serializers);
                            auto notification = std::make_shared<task_t<zlink::submit_result_t>> (
                              runtime.send_actor_leave_notification (
                                zlink::routing_id_t::from (
                                  std::string (completion.source_actor.node_rid ().value ())),
                                std::move (leave_parts)));
                            detail::observe_task_completion (
                              *notification, [notification, report_source_leave_failure] (
                                               const result_t<zlink::submit_result_t> &submitted) {
                                  if (submitted && submitted.value () == zlink::submit_result_t::ok)
                                      return;
                                  report_source_leave_failure ([&] {
                                      if (!submitted && submitted.error ())
                                          return std::make_exception_ptr (*submitted.error ());
                                      return std::make_exception_ptr (framework_exception_t (
                                        submitted
                                          ? runtime::messaging::map_submit_result_error_kind (
                                              submitted.value ())
                                          : submitted.error_kind (),
                                        "source Actor leave submission failed"));
                                  });
                              });
                        }
                        catch (...) {
                            report_source_leave_failure ([] { return std::current_exception (); });
                        }
                        if (!completion.reply.empty ()) {
                            reply = message_t::from_raw (
                              zlink::message_t::from (std::move (completion.reply)), serializers);
                        }
                    }
                    catch (...) {
                        joined = detail::current_exception_result<void> ();
                    }
                }
                const auto actor = completion.actor;
                actor_join_completion_t outcome =
                  joined
                    ? actor_join_completion_t (actor_join_accepted_t{completion.operation_high,
                                                                     completion.operation_low,
                                                                     actor, std::move (reply)})
                    : actor_join_completion_t (actor_join_failed_t{
                        completion.operation_high, completion.operation_low, joined.error_kind ()});
                try {
                    runtime.deliver_actor_join_completion_async (
                      actor, std::move (outcome), completion.target_spot_id,
                      [node, completion_record, joined = std::move (joined),
                       fail_commit] (result_t<void> delivered) mutable noexcept {
                          if (!delivered || !joined) {
                              fail_commit (!delivered ? std::move (delivered) : std::move (joined));
                              return;
                          }
                          try {
                              const auto commit = [] (auto node,
                                                      auto completion_record) -> task_t<void> {
                                  const auto &completion = *completion_record;
                                  const auto key = actor_key (completion.actor);
                                  auto replay = co_await node->actor_transfer_coordinator
                                                  ->complete_commit_and_take_backlog_async (
                                                    completion.handoff_id, completion.source_actor,
                                                    completion.target_spot_id);
                                  auto replay_services = co_await node->lane.run_task ([&] {
                                      if (replay) {
                                          const auto context = node->spot_contexts_by_id.find (
                                            std::string (completion.target_spot_id));
                                          if (context != node->spot_contexts_by_id.end ()
                                              && node->location_lifecycle) {
                                              const auto state =
                                                spot_context_access_t::state (context->second);
                                              state->ensure_spot_serial_executor ()
                                                ->actor_executor (key)
                                                ->queue ()
                                                ->confirm_execution_ready (
                                                  node->location_lifecycle->execution_runtime ()
                                                    .execution_clock () ());
                                          }
                                          const auto found =
                                            node->actor_join_relocation_recoveries.find (key);
                                          if (found != node->actor_join_relocation_recoveries.end ()
                                              && found->second.handoff_id == completion.handoff_id)
                                              node->actor_join_relocation_recoveries.erase (found);
                                      }
                                      return node->root_services;
                                  });
                                  if (!replay)
                                      throw framework_exception_t (
                                        framework_error_kind_t::internal_failure,
                                        "Actor Join completion commit failed");
                                  if (!replay->empty ()) {
                                      if (replay_services) {
                                          spot_node_runtime_t (node).enqueue_actor_handoff_replay (
                                            completion.actor, std::move (*replay), *replay_services,
                                            completion.handoff_id);
                                      } else {
                                          auto runtime = spot_node_runtime_t (node);
                                          if (runtime.actor_transfer_marker_enabled (
                                                message_flow_result_t::failed)) {
                                              runtime.emit_actor_transfer_marker (
                                                "handoff_replay_unavailable", completion.actor,
                                                completion.handoff_id, std::nullopt, std::nullopt,
                                                std::make_exception_ptr (framework_exception_t (
                                                  framework_error_kind_t::unavailable,
                                                  "Actor handoff replay owner is unavailable")));
                                          }
                                      }
                                  }
                                  co_return;
                              };
                              auto committed = commit (node, completion_record);
                              detail::observe_task_completion (
                                committed, [fail_commit] (const result_t<void> &result) {
                                    if (!result)
                                        fail_commit (result);
                                });
                          }
                          catch (...) {
                              fail_commit (detail::current_exception_result<void> ());
                          }
                      },
                      [delivery_scope] (result_t<void> result,
                                        std::function<void (result_t<void>)> settled) mutable {
                          delivery_scope->settle (std::move (result), std::move (settled));
                      });
                }
                catch (...) {
                    fail_join (detail::current_exception_result<void> ());
                }
            };
            if (completion.admission.on_actor_joined) {
                auto context = completion.target_context;
                auto callback = completion.admission.on_actor_joined;
                auto actor = completion.actor_instance;
                try {
                    context->run_serial_task_async (
                      "spot-relocation-actor-joined",
                      [context, callback, actor] {
                          return callback (context->spot_instance.get (), actor.get ());
                      },
                      [finish_join, fail_join] (result_t<void> value) mutable {
                          try {
                              finish_join (std::move (value));
                          }
                          catch (...) {
                              fail_join (detail::current_exception_result<void> ());
                          }
                      });
                }
                catch (...) {
                    finish_join (detail::current_exception_result<void> ());
                }
                return;
            }
            finish_join (result_t<void>::success ());
        };
        try {
            complete_join ();
        }
        catch (...) {
            fail_commit (detail::current_exception_result<void> ());
        }
    }
    return true;
}

task_t<void> spot_node_runtime_t::retire_relocation_source (
  const std::vector<runtime::stateful::object_ref_t> &sources)
{
    return retire_relocation_source_in (spot_node_runtime_t (_state), sources);
}

task_t<void> spot_node_runtime_t::retire_relocation_source_in (
  spot_node_runtime_t self, std::vector<runtime::stateful::object_ref_t> sources)
{
    // Relocation §10: retire application ownership once, retaining native Follow routes.
    // Members leave the local registry before their source Spot scope is closed.
    const auto report_failure = [&self] (const runtime::stateful::object_ref_t &source) {
        // Target commit remains successful; Host diagnostics retain cleanup errors even at Off.
        self._state->runtime_failures ()->report (std::current_exception ());
        const bool actor = source.kind == runtime::stateful::object_kind_t::actor;
        report_spot_dispatch_trace (
          self._state, message_flow_outcome_t::completed,
          dispatch_error_surface_t::actor_relocation, dispatch_message_kind_t::control,
          "relocation_cleanup", {}, actor ? std::string_view{} : source.key,
          actor ? source.key : std::string_view{}, {}, message_flow_result_t::failed, std::nullopt,
          "source_cleanup_failed", std::current_exception ());
    };
    for (const auto &source : sources) {
        if (source.kind != runtime::stateful::object_kind_t::actor)
            continue;
        auto actor = actor_ref_access_t::make (node_rid_t::from_string (source.node_id), {},
                                               source.key, source.object_generation);
        try {
            const auto released = co_await self._state->lane.run_task ([&self, &actor, source] {
                const auto type = self._state->actor_types_by_id.find (source.key);
                if (type == self._state->actor_types_by_id.end ())
                    return result_t<bool>::success (false);
                actor =
                  actor_ref_access_t::make (node_rid_t::from_string (source.node_id), type->second,
                                            source.key, source.object_generation);
                return self.release_local_actor_on_lane (actor);
            });
            if (!released)
                throw *released.error ();
        }
        catch (...) {
            report_failure (source);
        }
        try {
            if (self._state->root_services) {
                const auto removed =
                  co_await self._state->root_services->get_required<actor_gateway_runtime_t> ()
                    .destroy_actor_async (actor);
                if (!removed)
                    throw *removed.error ();
            }
        }
        catch (...) {
            report_failure (source);
        }
    }
    for (const auto &source : sources) {
        if (source.kind == runtime::stateful::object_kind_t::actor)
            continue;
        std::shared_ptr<spot_context_state_t> context;
        try {
            context = co_await self._state->lane.run_task ([&self, source] {
                const auto found = self._state->spot_contexts_by_id.find (source.key);
                return found == self._state->spot_contexts_by_id.end ()
                         ? std::shared_ptr<spot_context_state_t>{}
                         : found->second._state;
            });
        }
        catch (...) {
            report_failure (source);
        }
        if (!context)
            continue;
        try {
            context->complete_relocation_ready (spot_relocation_ready_outcome_t::relocated);
        }
        catch (...) {
            report_failure (source);
        }
        try {
            auto closed = std::make_shared<task_completion_source_t<bool>> ();
            context->request_close (
              spot_context_state_t::local_close_step (),
              [closed] (result_t<bool> result) { closed->complete (std::move (result)); },
              spot_close_reason_t::relocation_out);
            (void) co_await closed->task ();
        }
        catch (...) {
            report_failure (source);
        }
    }
}

task_t<void> spot_node_runtime_t::abort_relocation_materialization_async (
  std::vector<runtime::stateful::object_ref_t> targets)
{
    const auto abort = [] (spot_node_runtime_t self,
                           std::vector<runtime::stateful::object_ref_t> targets) -> task_t<void> {
        std::function<void ()> finish = [state = self._state, targets] {
            spot_node_runtime_t (state).end_relocation_activation_admissions (targets);
        };
        std::vector<std::shared_ptr<spot_context_state_t>> spots;
        std::vector<detail::handoff_packet_t> discarded_backlog;
        try {
            for (const auto &target : targets) {
                if (target.kind != runtime::stateful::object_kind_t::actor)
                    continue;
                const auto stable_type =
                  co_await self._state->lane.run_task ([&] () -> std::optional<std::string> {
                      const auto type = self._state->actor_types_by_id.find (target.key);
                      return type == self._state->actor_types_by_id.end ()
                               ? std::nullopt
                               : std::make_optional (type->second);
                  });
                if (!stable_type)
                    continue;
                auto discarded = co_await discard_actor_join_recovery_on_node_async (
                  self._state, *stable_type + ":" + target.key, [&] {
                      const auto type = self._state->actor_types_by_id.find (target.key);
                      if (type == self._state->actor_types_by_id.end ()
                          || type->second != *stable_type)
                          return;
                      const auto key = *stable_type + ":" + target.key;
                      if (const auto spot = self._state->actor_spot_ids.find (key);
                          spot != self._state->actor_spot_ids.end ()) {
                          const auto context =
                            self._state->spot_contexts_by_id.find (std::string (spot->second));
                          if (context != self._state->spot_contexts_by_id.end ()
                              && context->second._state)
                              decrement_actor_count_unlocked (*context->second._state);
                      }
                      const auto location = self._state->actor_spot_ids.find (key);
                      if (location != self._state->actor_spot_ids.end ()) {
                          const auto context =
                            self._state->spot_contexts_by_id.find (std::string (location->second));
                          if (context != self._state->spot_contexts_by_id.end ()
                              && context->second._state
                              && context->second._state->spot_serial_executor) {
                              context->second._state->spot_serial_executor->erase_actor_queue (key);
                          }
                      }
                      erase_actor_route_unlocked (*self._state, key);
                      self._state->actor_instances.erase (key);
                      detail::erase_actor_instance_index_unlocked (*self._state, type->second,
                                                                   target.key);
                      self._state->actor_types_by_id.erase (type);
                      self._state->destroyed_actor_keys.erase (key);
                  });
                if (discarded)
                    discarded_backlog.insert (discarded_backlog.end (),
                                              std::make_move_iterator (discarded->begin ()),
                                              std::make_move_iterator (discarded->end ()));
            }
            co_await self._state->lane.run_task ([&] {
                for (const auto &target : targets) {
                    if (target.kind != runtime::stateful::object_kind_t::user_spot
                        && target.kind != runtime::stateful::object_kind_t::instance_spot)
                        continue;
                    const auto context = self._state->spot_contexts_by_id.find (target.key);
                    if (context != self._state->spot_contexts_by_id.end ()
                        && context->second._state)
                        spots.push_back (context->second._state);
                }
            });
            fail_handoff_backlog (self._state, std::move (discarded_backlog));
        }
        catch (...) {
            finish ();
            co_return;
        }
        // The aborted target never owned the authority, so only the local
        // activation is released, on its lifecycle lane.
        for (auto spot = spots.rbegin (); spot != spots.rend (); ++spot) {
            finish = [spot = *spot, next = std::move (finish)] () mutable {
                spot->complete_preparation (false);
                spot->request_close (
                  spot_context_state_t::local_close_step (),
                  [next = std::move (next)] (result_t<bool>) mutable { next (); });
            };
        }
        finish ();
    };
    return abort (spot_node_runtime_t (_state), std::move (targets));
}

void spot_node_runtime_t::end_relocation_activation_admissions (
  const std::vector<runtime::stateful::object_ref_t> &targets) noexcept
{
    for (const auto &target : targets)
        _state->activation_admission->leave (target.kind == runtime::stateful::object_kind_t::actor
                                               ? activation_admission_t::actor_key (target.key)
                                               : activation_admission_t::spot_key (target.key));
}

void spot_node_runtime_t::on_destroy_actor (
  std::function<task_t<result_t<void>> (actor_ref_t)> destroy_actor)
{
    _state->lane.run ([&] { _state->destroy_actor_registry = std::move (destroy_actor); }).get ();
}

void spot_node_runtime_t::on_actor_ref_updated (
  std::function<result_t<void> (const actor_ref_t &)> update_actor)
{
    _state->lane.run ([&] { _state->update_actor_registry_ref = std::move (update_actor); }).get ();
}

std::optional<spot_id_t> spot_node_runtime_t::actor_spot (const actor_ref_t &actor_ref) const
{
    return _state->lane.run_sync ([&] () -> std::optional<spot_id_t> {
        const auto found = _state->actor_spot_ids.find (actor_key (actor_ref));
        if (found == _state->actor_spot_ids.end ()) {
            return std::nullopt;
        }
        return found->second;
    });
}

task_t<result_t<bool>> spot_node_runtime_t::release_local_actor_async (actor_ref_t actor)
{
    const auto state = _state;
    if (co_await state->actor_transfer_coordinator->blocks_dispatch_async (actor_key (actor)))
        co_return result_t<bool>::failure (framework_error_kind_t::unavailable,
                                           "Actor transfer is in progress");
    co_return co_await state->lane.run_task (
      [state, actor] { return spot_node_runtime_t (state).release_local_actor_on_lane (actor); });
}

result_t<bool>
spot_node_runtime_t::validate_local_actor_release_on_lane (const actor_ref_t &actor_ref) const
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<bool>::failure (framework_error_kind_t::invalid_operation,
                                        "ActorRef must identify an actor to destroy");
    }

    const auto local_node_rid = detail::effective_spot_node_rid (_state->snapshot);
    if (actor_ref.node_rid ().empty () || actor_ref.node_rid ().value () != local_node_rid) {
        return result_t<bool>::failure (framework_error_kind_t::invalid_operation,
                                        "ActorRef does not identify an actor on this node");
    }

    const auto key = actor_key (actor_ref);
    const auto generation = _state->actor_generations.find (key);
    if (generation == _state->actor_generations.end ()) {
        return result_t<bool>::success (false);
    }
    if (generation->second != actor_ref.object_generation ()) {
        return result_t<bool>::failure (framework_error_kind_t::invalid_operation,
                                        "ActorRef generation does not match the current actor");
    }
    if (_state->destroying_actors.contains (key)) {
        return result_t<bool>::success (false);
    }

    return result_t<bool>::success (true);
}

result_t<bool> spot_node_runtime_t::release_local_actor_on_lane (const actor_ref_t &actor_ref)
{
    const auto selected = validate_local_actor_release_on_lane (actor_ref);
    if (!selected || !selected.value ())
        return selected;
    const auto key = actor_key (actor_ref);
    const auto location = _state->actor_spot_ids.find (key);
    if (location != _state->actor_spot_ids.end ()) {
        const auto context = _state->spot_contexts_by_id.find (std::string (location->second));
        if (context != _state->spot_contexts_by_id.end () && context->second._state) {
            decrement_actor_count_unlocked (*context->second._state);
            if (context->second._state->spot_serial_executor)
                context->second._state->spot_serial_executor->erase_actor_queue (key);
        }
    }
    erase_actor_route_unlocked (*_state, key);
    _state->actor_created_keys.erase (key);
    _state->destroyed_actor_keys.insert (key);
    _state->actor_instances.erase (key);
    detail::erase_actor_instance_index_unlocked (
      *_state, ::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref),
      actor_ref.actor_id ().value ());
    _state->actor_types_by_id.erase (std::string (actor_ref.actor_id ().value ()));
    _state->mesh_runtime_owned_native_actor_ids.erase (
      std::string (actor_ref.actor_id ().value ()));
    _state->core_actor_membership_epochs.erase (std::string (actor_ref.actor_id ().value ()));
    (void) _state->dispatched_request_replies.erase_if ([&] (const auto &request_key) {
        return request_key.starts_with (actor_request_dedup_prefix (key));
    });
    return result_t<bool>::success (true);
}

task_t<result_t<void>> cleanup_local_actor_resources (std::shared_ptr<service::mesh_node_t> native,
                                                      std::optional<service_provider_t> services,
                                                      actor_ref_t actor)
{
    if (services) {
        if (auto gateway = services->get<actor_gateway_runtime_t> ()) {
            const auto cleaned = co_await gateway->get ().destroy_actor_async (actor);
            if (!cleaned)
                co_return cleaned;
        }
    }
    if (native) {
        const auto cleaned = co_await native->destroy_application_actor_async (
          std::string (actor.actor_id ().value ()), actor.object_generation ());
        if (cleaned != runtime::stateful::stateful_error_t::none
            && cleaned != runtime::stateful::stateful_error_t::not_found)
            co_return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                               "Local Actor native cleanup failed");
    }
    co_return result_t<void>::success ();
}

task_t<result_t<bool>> spot_node_runtime_t::discard_local_actor_async (
  actor_ref_t actor, runtime::serial_execution_queue_t::execution_status_t status)
{
    const auto state = _state;
    auto [selected, queue] = co_await state->lane.run_task ([state, actor] {
        const auto selected =
          spot_node_runtime_t (state).validate_local_actor_release_on_lane (actor);
        std::shared_ptr<runtime::serial_execution_queue_t> queue;
        if (selected && selected.value ()) {
            const auto key = actor_key (actor);
            const auto member = state->actor_spot_ids.find (key);
            if (member != state->actor_spot_ids.end ()) {
                const auto context = state->spot_contexts_by_id.find (std::string (member->second));
                if (context != state->spot_contexts_by_id.end () && context->second._state) {
                    const auto executor = context->second._state->spot_serial_executor;
                    if (executor) {
                        if (const auto mailbox = executor->find_actor_executor (key))
                            queue = mailbox->queue ();
                    }
                }
            }
        }
        return std::pair{selected, std::move (queue)};
    });
    if (!selected || !selected.value ())
        co_return selected;
    if (queue)
        co_await queue->cancel_pending_async (status);
    auto [released, native, services] = co_await state->lane.run_task ([state, actor] {
        auto released = spot_node_runtime_t (state).release_local_actor_on_lane (actor);
        return std::tuple{std::move (released), state->native_node.lock (), state->root_services};
    });
    if (!released || !released.value ())
        co_return released;
    const auto cleaned =
      co_await cleanup_local_actor_resources (std::move (native), std::move (services), actor);
    if (!cleaned)
        co_return detail::propagate_failure<bool> (cleaned, "Local Actor cleanup failed");
    co_return result_t<bool>::success (true);
}

task_t<result_t<bool>> spot_node_runtime_t::destroy_actor (actor_ref_t actor_ref)
{
    const auto state = _state;
    spot_node_runtime_t runtime (state);
    const auto selected = co_await runtime.release_local_actor_async (actor_ref);
    if (!selected || !selected.value ())
        co_return selected;
    co_await release_actor_location_async (state, actor_ref);
    const auto destroy_registry =
      co_await state->lane.run_task ([state] { return state->destroy_actor_registry; });
    if (destroy_registry) {
        const auto destroyed = co_await destroy_registry (actor_ref);
        if (!destroyed)
            co_return result_t<bool>::failure (
              destroyed.error_kind (),
              destroyed.error () ? destroyed.error ()->what () : "Actor registry cleanup failed");
    }
    co_return result_t<bool>::success (true);
}

void spot_node_runtime_t::record_actor_spot (const actor_ref_t &actor_ref, spot_id_t spot_id)
{
    _state->lane
      .run ([&] {
          const auto key = actor_key (actor_ref);
          auto name = spot_name_for_unlocked (spot_id).value_or ("");
          detail::record_actor_route_unlocked (
            *_state, key,
            spot_route_t{
              node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot)),
              std::move (spot_id), std::move (name)},
            actor_ref.object_generation ());
      })
      .get ();
}

std::optional<spot_route_t> spot_node_runtime_t::actor_route (const actor_ref_t &actor_ref) const
{
    return _state->lane.run_sync ([&] () -> std::optional<spot_route_t> {
        const auto found = _state->actor_routes.find (actor_key (actor_ref));
        if (found == _state->actor_routes.end ()) {
            return std::nullopt;
        }
        return found->second;
    });
}

void spot_node_runtime_t::record_actor_route (const actor_ref_t &actor_ref, spot_route_t route)
{
    _state->lane
      .run ([&] {
          const auto key = actor_key (actor_ref);
          detail::record_actor_route_unlocked (*_state, key, std::move (route),
                                               actor_ref.object_generation ());
      })
      .get ();
}

std::optional<actor_ref_t>
spot_node_runtime_t::current_actor_ref (const actor_ref_t &actor_ref) const
{
    return _state->lane.run_sync ([&] () -> std::optional<actor_ref_t> {
        const auto key = actor_key (actor_ref);
        const auto found = _state->actor_generations.find (key);
        if (found == _state->actor_generations.end ()) {
            return std::nullopt;
        }
        // The recorded location can be this node's own local spot, or (after a
        // relocation this node followed away, e.g. a Message Follow route) a
        // route naming the remote current owner; report whichever this node
        // actually knows rather than always claiming local ownership.
        auto node_rid_value = detail::effective_spot_node_rid (_state->snapshot);
        if (const auto routed = _state->actor_routes.find (key);
            routed != _state->actor_routes.end ()) {
            node_rid_value = std::string (routed->second.node_rid.value ());
        }
        return ::zlink::framework::detail::actor_ref_access_t::make (
          node_rid_t::from_string (node_rid_value),
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
          std::string (actor_ref.actor_id ().value ()), found->second);
    });
}

task_t<runtime::serial_execution_queue_t::ready_confirmation_t>
spot_node_runtime_t::prepare_store_ready (runtime::stateful::object_ref_t object)
{
    const auto state = _state;
    auto target = co_await state->lane.run_task ([state, &object] {
        return std::make_pair (spot_node_runtime_t (state).execution_queue_on_lane (object),
                               state->native_node.lock ());
    });
    if (!target.first)
        co_return runtime::serial_execution_queue_t::ready_confirmation_t{};
    if (target.second)
        co_await target.second->objects ().bind_execution_gate (object, target.first);
    co_return target.first->ready_confirmation ();
}

std::shared_ptr<runtime::serial_execution_queue_t>
spot_node_runtime_t::execution_queue_on_lane (const runtime::stateful::object_ref_t &object)
{
    assert (_state->lane.is_on_lane ());
    const bool actor = object.kind == runtime::stateful::object_kind_t::actor;
    auto context = _state->spot_contexts_by_id.end ();
    std::string key;
    if (actor) {
        const auto type = _state->actor_types_by_id.find (object.key);
        if (type == _state->actor_types_by_id.end ())
            return {};
        key = actor_key (type->second, object.key);
        const auto member = _state->actor_spot_ids.find (key);
        const auto generation = _state->actor_generations.find (key);
        if (member == _state->actor_spot_ids.end ()
            || generation == _state->actor_generations.end ()
            || generation->second != object.object_generation)
            return {};
        context = _state->spot_contexts_by_id.find (std::string (member->second));
    } else {
        context = _state->spot_contexts_by_id.find (object.key);
    }
    if (context == _state->spot_contexts_by_id.end () || !context->second._state)
        return {};
    const auto spot = context->second._state;
    if (!actor
        && (spot->object_generation != object.object_generation
            || spot->authority_owner_generation != object.authority_owner_generation))
        return {};
    if (spot->mesh_name != object.mesh_name || spot->node_rid.value () != object.node_id)
        return {};
    // A delayed CAS confirmation must still name the current steady incarnation.
    if (spot->closed || spot->close_reservation != 0 || spot->relocation_boundary_active)
        return {};
    if (actor) {
        const auto fence = _state->actor_authority_fences.find (key);
        if ((fence != _state->actor_authority_fences.end ()
             && fence->second.authority_owner_generation != object.authority_owner_generation)
            || _state->retiring_actor_keys.contains (key))
            return {};
    }
    const auto executor = spot->ensure_spot_serial_executor ();
    return executor ? (actor ? executor->actor_executor (key)->queue () : executor->spot_queue ())
                    : std::shared_ptr<runtime::serial_execution_queue_t>{};
}

void spot_node_runtime_t::bind_location_lifecycle (runtime::location_lifecycle_t &lifecycle)
{
    _state->lane
      .run ([&] {
          _state->location_lifecycle = &lifecycle;
          _state->discarded_authority_cleanup =
            lifecycle.execution_runtime ().discarded_authority_cleanup ();
      })
      .get ();
}

void spot_node_runtime_t::observe_location_authority (authority_key_t key,
                                                      authority_snapshot_t snapshot,
                                                      std::string expected_owner)
{
    const auto state = _state;
    state->lane.try_post ([state, key = std::move (key), snapshot = std::move (snapshot),
                           expected_owner = std::move (expected_owner)] {
        const auto identity = runtime::authority_key_codec_detail::decode_authority_key (key.value);
        const auto native = state->native_node.lock ();
        if (!identity || !native)
            return;
        runtime::stateful::object_ref_t observed{
          identity->kind == 'a' ? runtime::stateful::object_kind_t::actor
          : snapshot.allocation.object_kind == placement_object_kind_t::instance_spot
            ? runtime::stateful::object_kind_t::instance_spot
            : runtime::stateful::object_kind_t::user_spot,
          identity->object_id,
          snapshot.object_generation,
          snapshot.authority_owner_generation,
          snapshot.allocation.target.mesh_name,
          std::string (snapshot.allocation.target.node_rid.value ())};
        auto fenced = native->objects ().fence_location_authority (
          std::move (observed), snapshot.owner.owner_id == expected_owner);
        detail::observe_task_terminal (fenced, [state, native] (const result_t<bool> &result) {
            if (!result) {
                report_spot_close_diagnostic (state, {}, "location_fence_failed",
                                              result.exception ());
                return;
            }
            if (result.value ()) {
                const auto submitted = state->worker_executor->try_submit_internal ([state] {
                    spot_node_runtime_t (state).request_location_execution_discard (
                      [] (const auto &queue) {
                          using status_t = runtime::serial_execution_queue_t::execution_status_t;
                          return queue.execution_status () == status_t::ownership_lost
                                   ? status_t::ownership_lost
                                   : status_t::allowed;
                      });
                });
                if (!submitted)
                    report_spot_close_diagnostic (
                      state, {}, "location_discard_rejected",
                      std::make_exception_ptr (detail::make_framework_origin_exception (
                        framework_error_kind_t::shutting_down,
                        "Local discard executor is closed")));
            }
        });
    });
}

void spot_node_runtime_t::resume_location_execution ()
{
    const auto state = _state;
    state->lane.try_post ([state] {
        for (const auto &[_, context] : state->spot_contexts_by_id) {
            const auto spot = context._state;
            if (!spot || !spot->spot_serial_executor)
                continue;
            spot->spot_serial_executor->execution_operation_changed ();
            for (const auto &timer : spot->timers) {
                bool pending;
                {
                    std::lock_guard lock (timer->mutex);
                    pending = !timer->disposed && !timer->pending_fire && !timer->running
                              && timer->pending_fire_count != 0;
                }
                if (pending)
                    timer_runtime_t::post_fire_count (spot, timer, timer->serial_queue, 0);
            }
        }
    });
}

task_t<void> spot_node_runtime_t::request_location_execution_discard (
  runtime::serial_execution_queue_t::execution_admission_t allowed)
{
    using discarded_spot_t =
      std::pair<std::shared_ptr<spot_context_state_t>, std::vector<task_t<void>>>;
    struct cleanup_t
    {
        static task_t<void> prepare (
          std::shared_ptr<service::mesh_node_t> native,
          runtime::serial_execution_queue_t::execution_admission_t allowed,
          std::function<task_t<void> (authority_key_t, authority_snapshot_t, task_t<void>)> cleanup,
          std::shared_ptr<task_completion_source_t<void>> settled)
        {
            if (!native)
                co_return;
            auto authorities =
              co_await native->objects ().discard_location_execution (std::move (allowed));
            if (cleanup)
                for (auto &[key, fence] : authorities)
                    co_await cleanup (std::move (key), std::move (fence), settled->task ());
        }

        static task_t<void> run (
          std::shared_ptr<spot_node_builder_state_t> state,
          std::shared_ptr<task_t<void>> prepared,
          std::shared_ptr<std::vector<discarded_spot_t>> spots,
          std::vector<std::pair<actor_ref_t, runtime::serial_execution_queue_t::execution_status_t>>
            actors,
          runtime::serial_execution_queue_t::execution_admission_t retained_fence)
        {
            // Preserve authority fences before local Close removes their objects.
            co_await *prepared;
            // Keep the discarded episode's fence until the existing cleanup continuation finishes.
            (void) retained_fence;
            for (const auto &[actor, status] : actors) {
                const auto result =
                  co_await spot_node_runtime_t (state).discard_local_actor_async (actor, status);
                if (!result)
                    report_spot_close_diagnostic (state, {}, "location_actor_discard_failed",
                                                  result.exception ());
            }
            for (auto &[spot, completions] : *spots) {
                for (auto &completion : completions)
                    co_await completion;
                const auto closed =
                  co_await runtime::await_result (runtime::run_blocking_step<bool> ([spot] {
                      auto completion = std::make_shared<task_completion_source_t<bool>> ();
                      auto result = completion->task ();
                      spot->close_now (
                        spot_context_state_t::local_close_step (),
                        [completion] (result_t<bool> closed) {
                            completion->complete (std::move (closed));
                        },
                        {}, spot_close_reason_t::host_shutdown);
                      return result;
                  }));
                if (!closed)
                    report_spot_close_diagnostic (state, std::string (spot->spot_id),
                                                  "location_discard_failed", closed.exception ());
            }
        }
    };
    const auto state = _state;
    auto applied = std::make_shared<task_completion_source_t<void>> ();
    auto settled = std::make_shared<task_completion_source_t<void>> ();
    // Publish the actual close turn before returning, even when the lane is busy.
    // Claim waits for terminal application; repository cleanup waits for local drain.
    auto abandon = [applied, settled] (std::exception_ptr error) {
        const auto result = detail::result_access_t::failure<void> (std::move (error));
        applied->complete (result);
        settled->complete (result);
    };
    const auto posted = state->lane.try_post (
      [state, allowed = std::move (allowed), applied, settled, abandon] {
          try {
              auto prepared = std::make_shared<task_t<void>> (cleanup_t::prepare (
                state->native_node.lock (), allowed, state->discarded_authority_cleanup, settled));
              auto spots = std::make_shared<std::vector<discarded_spot_t>> ();
              std::vector<
                std::pair<actor_ref_t, runtime::serial_execution_queue_t::execution_status_t>>
                actors;
              for (const auto &[_, context] : state->spot_contexts_by_id) {
                  const auto spot = context._state;
                  if (!spot || !spot->spot_serial_executor)
                      continue;
                  if (!spot->is_entry_spot () && spot->serial_queue
                      && allowed (*spot->serial_queue)
                           != runtime::serial_execution_queue_t::execution_status_t::allowed) {
                      std::vector<task_t<void>> completions;
                      const auto status = allowed (*spot->serial_queue);
                      // Timer state can retain a queue outside the coordinator's named queues.
                      // Apply its terminal before a new lease can reopen execution admission.
                      for (const auto &timer : spot->timers)
                          if (timer->serial_queue)
                              completions.push_back (
                                timer->serial_queue->cancel_pending_async (status));
                      spot->spot_serial_executor->close (&completions, status);
                      spots->emplace_back (spot, std::move (completions));
                      continue;
                  }
                  for (const auto &[id, type] : state->actor_types_by_id) {
                      const auto key = actor_key (type, id);
                      const auto member = state->actor_spot_ids.find (key);
                      if (member == state->actor_spot_ids.end () || member->second != spot->spot_id)
                          continue;
                      const auto executor = spot->spot_serial_executor->find_actor_executor (key);
                      if (executor
                          && allowed (*executor->queue ())
                               != runtime::serial_execution_queue_t::execution_status_t::allowed) {
                          executor->queue ()->cancel_pending (allowed (*executor->queue ()));
                          actors.emplace_back (
                            ::zlink::framework::detail::actor_ref_access_t::make (
                              spot->node_rid, type, id, state->actor_generations.at (key)),
                            allowed (*executor->queue ()));
                      }
                  }
              }
              if (spots->empty () && actors.empty ()) {
                  settled->complete (result_t<void>::success ());
              } else {
                  // Existing Close uses its infrastructure executor outside the owner turn.
                  const auto submitted = state->worker_executor->try_submit_internal (
                    [state, prepared, spots = std::move (spots), actors = std::move (actors),
                     allowed = std::move (allowed), settled] () mutable {
                        auto cleanup = cleanup_t::run (state, prepared, std::move (spots),
                                                       std::move (actors), std::move (allowed));
                        detail::observe_task_terminal (
                          cleanup, [state, settled] (const result_t<void> &result) {
                              settled->complete (result);
                              if (!result)
                                  report_spot_close_diagnostic (
                                    state, {}, "location_discard_failed", result.exception ());
                          });
                    });
                  if (!submitted) {
                      const auto result = result_t<void>::failure (
                        framework_error_kind_t::shutting_down, "Local discard executor is closed");
                      applied->complete (result);
                      settled->complete (result);
                      report_spot_close_diagnostic (
                        state, {}, "location_discard_rejected",
                        std::make_exception_ptr (detail::make_framework_origin_exception (
                          framework_error_kind_t::shutting_down,
                          "Local discard executor is closed")));
                      return;
                  }
              }
              detail::observe_task_terminal (
                *prepared,
                [applied, prepared] (const result_t<void> &result) { applied->complete (result); });
          }
          catch (...) {
              abandon (std::current_exception ());
          }
      },
      abandon);
    if (!posted)
        abandon (std::make_exception_ptr (detail::make_framework_origin_exception (
          framework_error_kind_t::shutting_down, "Local discard lane is closed")));
    return applied->task ();
}

std::vector<actor_ref_t> spot_node_runtime_t::local_actor_refs () const
{
    const auto inventory_result = local_actor_inventory_async ().result ();
    const auto &inventory = inventory_result.value ();
    std::vector<actor_ref_t> refs;
    refs.reserve (inventory.size ());
    for (const auto &actor : inventory) {
        refs.push_back (::zlink::framework::detail::actor_ref_access_t::make (
          node_rid_t::from_string (actor.owner.node_id), actor.stable_type, actor.owner.key,
          actor.owner.object_generation));
    }
    return refs;
}

void spot_node_runtime_t::filter_local_actor_inventory_on_lane (
  const spot_node_builder_state_t &state,
  std::vector<runtime::stateful::object_inventory_t> &inventory)
{
    const auto node_rid = detail::effective_spot_node_rid (state.snapshot);
    std::erase_if (inventory, [&] (const auto &actor) {
        if (actor.owner.node_id != node_rid
            || (actor.state != runtime::stateful::object_state_t::ready
                && actor.state != runtime::stateful::object_state_t::moving))
            return true;
        const auto instance =
          state.actor_instances.find (actor_key (actor.stable_type, actor.owner.key));
        return instance == state.actor_instances.end () || !instance->second;
    });
}
task_t<std::vector<runtime::stateful::object_inventory_t>>
spot_node_runtime_t::local_actor_inventory_async () const
{
    const auto state = _state;
    const auto native =
      co_await state->lane.run_task ([state] { return state->native_node.lock (); });
    if (!native)
        co_return std::vector<runtime::stateful::object_inventory_t>{};
    auto inventory = co_await native->objects ().actor_inventory ();
    co_return co_await state->lane.run_task ([state, inventory = std::move (inventory)] () mutable {
        filter_local_actor_inventory_on_lane (*state, inventory);
        return std::move (inventory);
    });
}

std::optional<zlink::message_t>
spot_node_runtime_t::serialize_actor_snapshot (const actor_ref_t &actor_ref) const
{
    struct snapshot_plan_t
    {
        std::shared_ptr<void> actor;
        std::function<std::optional<zlink::message_t> (void *, serializer_registry_t &)> serialize;
        serializer_registry_t *serializers = nullptr;
    };
    const auto plan =
      _state->lane
        .run ([&] {
            snapshot_plan_t result;
            const auto actor = _state->actor_instances.find (actor_key (actor_ref));
            const auto factory = _state->actor_factories.find (
              std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
            if (actor != _state->actor_instances.end () && actor->second
                && factory != _state->actor_factories.end () && _state->channel_runtime
                && _state->channel_runtime->serializers) {
                result.actor = actor->second;
                result.serialize = factory->second.serialize_instance;
                result.serializers = _state->channel_runtime->serializers;
            }
            return result;
        })
        .get ();
    if (!plan.actor || !plan.serialize || !plan.serializers) {
        return std::nullopt;
    }
    return plan.serialize (plan.actor.get (), *plan.serializers);
}

task_t<bool> spot_node_runtime_t::relocation_ready_async () const
{
    const auto state = _state;
    return state->lane.run_task ([state] {
        for (const auto &[_, context] : state->spot_contexts_by_id) {
            const auto state = context._state;
            if (!state || state->is_entry_spot () || state->is_instance_spot ()
                || state->execution_mode != user_spot_execution_mode_t::spot_wide
                || state->relocation_coordination_mode
                     != spot_relocation_coordination_mode_t::application_signaled)
                continue;
            if (!state->relocation_ready_deferred)
                return false;
        }
        return true;
    });
}

task_t<spot_node_runtime_t::relocation_units_t> spot_node_runtime_t::relocation_units () const
{
    const auto state = _state;
    const auto native =
      co_await state->lane.run_task ([state] { return state->native_node.lock (); });
    if (!native)
        co_return relocation_units_t{};
    auto inventory = co_await native->objects ().try_begin_maintenance_inventory (true);
    if (!inventory)
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "Actor maintenance inventory is already sealed");
    relocation_units_t units;
    std::exception_ptr error;
    try {
        units =
          co_await state->lane.run_task ([state, inventory = std::move (*inventory)] () mutable {
              filter_local_actor_inventory_on_lane (*state, inventory);
              relocation_units_t result;
              for (const auto &[_, context] : state->spot_contexts_by_id) {
                  const auto state = context._state;
                  if (!state || state->is_entry_spot () || state->is_instance_spot ()
                      || state->execution_mode != user_spot_execution_mode_t::spot_wide)
                      continue;
                  result.spots.push_back ({state->spot_id, state->spot_name, {}});
              }
              for (const auto &actor : inventory) {
                  auto ref = ::zlink::framework::detail::actor_ref_access_t::make (
                    node_rid_t::from_string (actor.owner.node_id), actor.stable_type,
                    actor.owner.key, actor.owner.object_generation);
                  const auto spot = std::find_if (
                    result.spots.begin (), result.spots.end (),
                    [&] (const auto &unit) { return unit.spot_id == actor.membership; });
                  if (spot != result.spots.end ())
                      spot->actors.push_back (std::move (ref));
                  else
                      result.standalone_actors.push_back (std::move (ref));
              }
              return result;
          });
    }
    catch (...) {
        error = std::current_exception ();
    }
    co_await native->objects ().end_maintenance_inventory ();
    if (error)
        std::rethrow_exception (error);
    co_return units;
}

void spot_node_runtime_t::begin_relocation_readiness ()
{
    begin_relocation_readiness_async ().result ().value ();
}

task_t<void> spot_node_runtime_t::begin_relocation_readiness_async ()
{
    const auto node = _state;
    return node->lane.run_task ([node] {
        for (const auto &[_, context] : node->spot_contexts_by_id) {
            const auto state = context._state;
            if (!state || state->is_entry_spot () || state->is_instance_spot ()
                || state->execution_mode != user_spot_execution_mode_t::spot_wide
                || state->relocation_coordination_mode
                     != spot_relocation_coordination_mode_t::application_signaled)
                continue;
            if (state->serial_queue)
                state->serial_queue->invalidate_execution_ready ();
            state->relocation_boundary_active = true;
        }
    });
}

void spot_node_runtime_t::end_relocation_readiness (const std::vector<spot_id_t> &relocated_spots)
{
    auto states = _state->lane
                    .run ([&] {
                        std::vector<std::shared_ptr<spot_context_state_t>> result;
                        for (const auto &[_, context] : _state->spot_contexts_by_id) {
                            const auto state = context._state;
                            if (!state || !state->relocation_boundary_active)
                                continue;
                            state->relocation_boundary_active = false;
                            result.push_back (state);
                        }
                        return result;
                    })
                    .get ();
    for (const auto &state : states) {
        const auto relocated =
          std::find (relocated_spots.begin (), relocated_spots.end (), state->spot_id)
          != relocated_spots.end ();
        state->complete_relocation_ready (relocated ? spot_relocation_ready_outcome_t::relocated
                                                    : spot_relocation_ready_outcome_t::continued);
    }
}

} // namespace detail

task_t<actor_ref_t> spot_context_t::leave_actor_erased (
  std::shared_ptr<detail::spot_context_state_t> state,
  actor_ref_t actor_ref,
  actor_context_t &actor_context,
  std::type_index actor_type,
  void *actor,
  std::function<void (void *, const actor_ref_t &)> update_actor_ref)
{
    report_spot_dispatch_trace (state ? state->node : nullptr, message_flow_outcome_t::received,
                                dispatch_error_surface_t::spot_actor,
                                dispatch_message_kind_t::actor_request, "actor_leave", {},
                                state ? state->spot_id : std::string_view{},
                                ::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)
                                  ? std::string_view{}
                                  : actor_ref.actor_id ().value ());
    spot_context_t (state).ensure_submission_open ();
    if (!state || !state->node
        || ::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        co_return result_t<actor_ref_t>::failure (framework_error_kind_t::not_found,
                                                  "actor ref is empty");
    }
    if (state->is_current_callback_thread ()) {
        report_spot_dispatch_trace (state->node, message_flow_outcome_t::dispatched,
                                    dispatch_error_surface_t::spot_actor,
                                    dispatch_message_kind_t::actor_request, "actor_leave_deferred",
                                    {}, state->spot_id, actor_ref.actor_id ().value ());
        const auto deferred_ref = actor_ref;
        const auto posted = state->try_post_serial_after_current_turn (
          "spot-actor-leave-after-handler",
          [state, deferred_ref, actor_type, actor, &actor_context,
           update_actor_ref = std::move (update_actor_ref)] () mutable {
              report_spot_dispatch_trace (state->node, message_flow_outcome_t::dispatched,
                                          dispatch_error_surface_t::spot_actor,
                                          dispatch_message_kind_t::actor_request,
                                          "actor_leave_deferred_execute", {}, state->spot_id,
                                          deferred_ref.actor_id ().value ());
              /* Start the leave operation outside the source handler turn.
               * Observe its terminal without retaining a call-executor worker
               * while the remote membership commit is pending. */
              const auto submitted = detail::submit_blocking_call (
                [state, deferred_ref, actor_type, actor, &actor_context,
                 update_actor_ref = std::move (update_actor_ref)] () mutable {
                    try {
                        auto leaving =
                          leave_actor_erased (state, deferred_ref, actor_context, actor_type, actor,
                                              std::move (update_actor_ref));
                        detail::observe_task_completion (
                          leaving, [state, deferred_ref] (const result_t<actor_ref_t> &completed) {
                              report_spot_dispatch_trace (
                                state->node, message_flow_outcome_t::completed,
                                dispatch_error_surface_t::spot_actor,
                                dispatch_message_kind_t::actor_request,
                                completed ? "actor_leave_deferred_complete"
                                          : "actor_leave_deferred_failed",
                                {}, state->spot_id, deferred_ref.actor_id ().value (), {},
                                completed ? message_flow_result_t::succeeded
                                          : message_flow_result_t::failed,
                                std::nullopt, {}, completed.exception ());
                          });
                    }
                    catch (...) {
                        report_spot_dispatch_trace (
                          state->node, message_flow_outcome_t::completed,
                          dispatch_error_surface_t::spot_actor,
                          dispatch_message_kind_t::actor_request, "actor_leave_deferred_exception",
                          {}, state->spot_id, deferred_ref.actor_id ().value (), {},
                          message_flow_result_t::failed, std::nullopt, {},
                          std::current_exception ());
                    }
                });
              if (!submitted) {
                  report_spot_dispatch_trace (
                    state->node, message_flow_outcome_t::backpressured,
                    dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
                    "actor_leave_deferred_rejected", {}, state->spot_id,
                    deferred_ref.actor_id ().value (), {}, message_flow_result_t::backpressured,
                    message_flow_reason_t::backpressure);
              }
          },
          runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle});
        if (!posted) {
            co_return result_t<actor_ref_t>::failure (framework_error_kind_t::shutting_down,
                                                      "Actor leave queue is closed");
        }
        co_return result_t<actor_ref_t>::success (actor_ref);
    }
    /* Lifecycle callbacks may close the source Spot. Keep both the node and
     * each selected callback target alive after its state-lane turn ends. */
    auto node = state->node;
    const auto stable_actor_type =
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref));
    const auto key = stable_actor_type + ":" + std::string (actor_ref.actor_id ().value ());

    struct leave_plan_t
    {
        enum class destination_t
        {
            no_op,
            remote_entry,
            local_entry
        };

        destination_t destination = destination_t::no_op;
        std::shared_ptr<detail::spot_context_state_t> source_state;
        std::shared_ptr<detail::spot_context_state_t> entry_state;
        std::shared_ptr<void> source_spot_instance;
        std::shared_ptr<void> actor_instance;
        std::function<task_t<void> (void *, void *)> source_leave;
    };

    auto planned =
      node->lane
        .run ([&] () -> result_t<leave_plan_t> {
            const auto found_location = node->actor_spot_ids.find (key);
            if (found_location == node->actor_spot_ids.end ())
                return result_t<leave_plan_t>::success (leave_plan_t{});
            if (found_location->second != state->spot_id) {
                return result_t<leave_plan_t>::failure (framework_error_kind_t::not_found,
                                                        "actor is not joined to this SPOT");
            }

            const auto found_generation = node->actor_generations.find (key);
            if (found_generation != node->actor_generations.end ()
                && found_generation->second != actor_ref.object_generation ()) {
                return detail::boundary_failure<leave_plan_t> (
                  detail::boundary_error_t::stale_generation, "actor generation is stale");
            }

            leave_plan_t plan;
            plan.source_state = state;
            const bool remote_entry = state->node_rid.empty ()
                                      || actor_ref.node_rid ().value () != state->node_rid.value ();
            if (remote_entry) {
                plan.destination = leave_plan_t::destination_t::remote_entry;
            } else {
                if (!node->snapshot.entry_spot_name) {
                    return result_t<leave_plan_t>::failure (framework_error_kind_t::not_found,
                                                            "entry spot is not registered");
                }
                const auto entry_id = node->spot_ids_by_name.find (*node->snapshot.entry_spot_name);
                if (entry_id == node->spot_ids_by_name.end ()) {
                    return result_t<leave_plan_t>::failure (framework_error_kind_t::not_found,
                                                            "entry spot is not created");
                }
                const auto entry_context = node->spot_contexts_by_id.find (entry_id->second);
                if (entry_context == node->spot_contexts_by_id.end ()
                    || !entry_context->second._state
                    || !entry_context->second._state->spot_instance) {
                    return result_t<leave_plan_t>::failure (framework_error_kind_t::not_found,
                                                            "entry spot context is not registered");
                }
                plan.destination = leave_plan_t::destination_t::local_entry;
                plan.entry_state = entry_context->second._state;
            }

            auto &source_state = *state;
            const auto source_admission = source_state.actor_admissions.find (actor_type);
            if (source_admission != source_state.actor_admissions.end ()
                && source_admission->second.on_leave_actor && source_state.spot_instance) {
                plan.source_leave = source_admission->second.on_leave_actor;
                plan.source_spot_instance = source_state.spot_instance;
            } else {
                const auto source_left = source_state.on_leave_actor_callbacks.find (actor_type);
                if (source_left != source_state.on_leave_actor_callbacks.end ()
                    && source_state.spot_instance) {
                    plan.source_leave = source_left->second;
                    plan.source_spot_instance = source_state.spot_instance;
                }
            }
            if (plan.source_leave) {
                const auto actor_instance = node->actor_instances.find (key);
                if (actor_instance != node->actor_instances.end ())
                    plan.actor_instance = actor_instance->second;
            }
            return result_t<leave_plan_t>::success (std::move (plan));
        })
        .get ();
    if (!planned) {
        co_return detail::propagate_failure<actor_ref_t> (planned,
                                                          "actor leave state admission failed");
    }
    auto plan = std::move (planned.value ());
    if (plan.destination == leave_plan_t::destination_t::no_op) {
        report_spot_dispatch_trace (node, message_flow_outcome_t::replied,
                                    dispatch_error_surface_t::spot_actor,
                                    dispatch_message_kind_t::actor_request, "actor_leave_noop", {},
                                    state->spot_id, actor_ref.actor_id ().value ());
        co_return result_t<actor_ref_t>::success (actor_ref);
    }

    const auto commit_source_departure = [&] {
        node->lane
          .run ([&] {
              decrement_actor_count_unlocked (*plan.source_state);
              erase_actor_route_unlocked (*node, key);
          })
          .get ();
    };
    const auto submit_source_leave = [&] {
        if (!plan.source_leave)
            return;
        plan.source_state->run_serial_task_async (
          "spot-lifecycle-leave",
          [callback = plan.source_leave, spot = plan.source_spot_instance,
           actor_instance = plan.actor_instance, actor] {
              return callback (spot.get (), actor_instance ? actor_instance.get () : actor);
          },
          [node, spot_id = plan.source_state->spot_id] (result_t<void> left) {
              if (!left) {
                  report_spot_dispatch_trace (
                    node, message_flow_outcome_t::completed, dispatch_error_surface_t::spot_actor,
                    dispatch_message_kind_t::control, "spot_actor_leave", {}, spot_id, {}, {},
                    message_flow_result_t::failed, std::nullopt, "leave_failed", left.exception ());
              }
          });
    };

    if (plan.destination == leave_plan_t::destination_t::remote_entry) {
        try {
            auto context = std::shared_ptr<actor_context_t> (new actor_context_t (
              actor_context._state, actor_ref, actor_context._source_binding_generation,
              actor_context._mesh_name));
            context->_actor_ref = actor_context._actor_ref;
            auto joined = co_await actor_context_t::join_entry_spot_erased (
              std::move (context), zlink::message_t{}, actor_join_call_t::default_timeout);
            if (joined.result_code != detail::actor_join_reply_t::accepted) {
                co_return result_t<actor_ref_t>::failure (framework_error_kind_t::rejected,
                                                          "remote entry spot join was rejected");
            }
            if (update_actor_ref)
                update_actor_ref (actor, joined.actor);
            commit_source_departure ();
            submit_source_leave ();
            co_return result_t<actor_ref_t>::success (joined.actor);
        }
        catch (const framework_exception_t &error) {
            co_return detail::result_access_t::failure<actor_ref_t> (error);
        }
        catch (const std::exception &error) {
            co_return result_t<actor_ref_t>::failure (framework_error_kind_t::internal_failure,
                                                      error.what ());
        }
    }

    commit_source_departure ();
    try {
        struct location_update_plan_t
        {
            actor_ref_t committed;
            runtime::location_lifecycle_t *lifecycle = nullptr;
            std::shared_ptr<service::mesh_node_t> native_node;
            std::shared_ptr<service::spot_t> native_spot;
            std::string mesh_name;
            node_rid_t node_rid;
            spot_id_t spot_id;
            std::uint64_t membership_epoch = 0;
            bool context_attached = false;
        };
        auto location_plan =
          node->lane
            .run ([&] {
                location_update_plan_t selected{
                  .committed = ::zlink::framework::detail::actor_ref_access_t::make (
                    node_rid_t::from_string (std::string (state->node_rid.value ())),
                    stable_actor_type, std::string (actor_ref.actor_id ().value ()),
                    actor_ref.object_generation ())};
                selected.lifecycle = node->location_lifecycle;
                selected.context_attached = plan.entry_state->node != nullptr;
                if (selected.lifecycle && selected.context_attached) {
                    selected.native_node = node->native_node.lock ();
                    selected.native_spot = plan.entry_state->native_spot.lock ();
                    selected.mesh_name = node->snapshot.name;
                    selected.node_rid = plan.entry_state->node_rid;
                    selected.spot_id = plan.entry_state->spot_id;
                    const auto membership = node->core_actor_membership_epochs.find (
                      std::string (actor_ref.actor_id ().value ()));
                    if (membership != node->core_actor_membership_epochs.end ())
                        selected.membership_epoch = membership->second;
                }
                return selected;
            })
            .get ();

        if (location_plan.lifecycle && location_plan.context_attached) {
            const auto owner_generation =
              location_plan.native_node
                ? location_plan.native_node->status ().lifecycle_generation ()
                : 0;
            const auto spot_generation_value =
              location_plan.native_spot
                ? location_plan.native_spot->status ().lifecycle_generation ()
                : 0;
            auto location = actor_location_t{
              .mesh_name = location_plan.mesh_name,
              .actor_id = std::string (location_plan.committed.actor_id ().value ()),
              .actor_type = stable_actor_type,
              .actor_ref = location_plan.committed,
              .owner_node_rid =
                zlink::routing_id_t::from (std::string (location_plan.node_rid.value ())),
              .owner_node_generation = owner_generation,
              .spot_id = location_plan.spot_id,
              .spot_generation = spot_generation_value,
              .spot_kind = zlink::spot_kind::entry,
              .membership_epoch = location_plan.membership_epoch};
            (void) location_plan.lifecycle->owns_actor (actor_location_key_t{
              location.mesh_name, std::string (location_plan.committed.actor_id ().value ())});
            (void) location_plan.lifecycle->update_actor_location (std::move (location));
        }

        auto update_registry =
          node->lane
            .run ([&] {
                detail::record_actor_context_route_unlocked (
                  *node, key, std::string (state->node_rid.value ()), *plan.entry_state,
                  location_plan.committed.object_generation ());
                return node->update_actor_registry_ref;
            })
            .get ();
        actor_context._state->sync ([&] { *actor_context._actor_ref = location_plan.committed; });
        if (update_actor_ref)
            update_actor_ref (actor, location_plan.committed);
        if (update_registry) {
            auto updated = update_registry (location_plan.committed);
            if (!updated) {
                const auto *error = updated.error ();
                co_return result_t<actor_ref_t>::failure (
                  updated.error_kind (),
                  error != nullptr ? error->what () : "actor registry ref update failed");
            }
        }

        struct joined_callback_plan_t
        {
            std::function<task_t<void> (void *, void *)> callback;
            std::shared_ptr<void> spot_instance;
        };
        const auto joined_plan =
          node->lane
            .run ([&] {
                joined_callback_plan_t selected;
                const auto entry_admission = plan.entry_state->actor_admissions.find (actor_type);
                if (entry_admission != plan.entry_state->actor_admissions.end ()
                    && entry_admission->second.on_actor_joined && plan.entry_state->spot_instance) {
                    selected.callback = entry_admission->second.on_actor_joined;
                    selected.spot_instance = plan.entry_state->spot_instance;
                } else {
                    const auto entry_joined =
                      plan.entry_state->on_actor_joined_callbacks.find (actor_type);
                    if (entry_joined != plan.entry_state->on_actor_joined_callbacks.end ()
                        && plan.entry_state->spot_instance) {
                        selected.callback = entry_joined->second;
                        selected.spot_instance = plan.entry_state->spot_instance;
                    }
                }
                return selected;
            })
            .get ();
        auto result = result_t<actor_ref_t>::success (location_plan.committed);
        if (joined_plan.callback) {
            try {
                co_await plan.entry_state->run_serial_task ("spot-lifecycle-join", [&] {
                    return joined_plan.callback (joined_plan.spot_instance.get (), actor);
                });
            }
            catch (...) {
                result = detail::result_access_t::failure<actor_ref_t> (std::current_exception ());
            }
        }
        submit_source_leave ();
        co_return std::move (result);
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<actor_ref_t> (error);
    }
    catch (const std::exception &error) {
        co_return result_t<actor_ref_t>::failure (framework_error_kind_t::internal_failure,
                                                  error.what ());
    }
}

task_t<void> entry_spot_context_t::destroy_actor_instance_erased (const void *instance)
{
    if (!_state || !_state->node || instance == nullptr) {
        return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_found,
                                                      "actor instance is not registered"));
    }
    /* Resolve one immutable ref in a state turn, then enter the public destroy
     * surface after the turn has ended. */
    const auto actor =
      _state->node->lane
        .run ([&] () -> std::optional<actor_ref_t> {
            const auto found = _state->node->actor_instance_index.find (instance);
            if (found == _state->node->actor_instance_index.end ()) {
                return std::nullopt;
            }
            const auto key = found->second.first + ":" + found->second.second;
            const auto found_generation = _state->node->actor_generations.find (key);
            return ::zlink::framework::detail::actor_ref_access_t::make (
              _state->node_rid, found->second.first, found->second.second,
              found_generation != _state->node->actor_generations.end () ? found_generation->second
                                                                         : 1);
        })
        .get ();
    if (!actor) {
        /* Instance not registered on this node: already destroyed or
         * superseded — duplicate destroy is a successful no-op. */
        return task_t<void> (result_t<void>::success ());
    }
    return destroy_actor_erased (*actor);
}

task_t<void> entry_spot_context_t::destroy_actor_erased (const actor_ref_t &actor)
{
    const auto run =
      +[] (std::shared_ptr<detail::spot_context_state_t> state, actor_ref_t actor) -> task_t<void> {
        if (!state || !state->node
            || ::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
            throw framework_exception_t (framework_error_kind_t::not_found, "actor ref is empty");
        }
        /* A lifecycle callback receives a reference to the actor instance. Erasing
     * actor_instances from that callback would destroy the object before the
     * callback (and its coroutine frame) has returned. Queue the destructive
     * part behind the current serial turn so the borrowed reference remains
     * valid for the complete callback. */
        if (state->is_current_callback_thread ()) {
            const auto deferred_actor = actor;
            const auto key =
              std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)) + ":"
              + std::string (actor.actor_id ().value ());
            const auto accepted = co_await state->node->lane.run_task ([&] {
                const auto found_location = state->node->actor_spot_ids.find (key);
                const auto found_generation = state->node->actor_generations.find (key);
                if (found_location == state->node->actor_spot_ids.end ()
                    || (found_generation != state->node->actor_generations.end ()
                        && found_generation->second != actor.object_generation ())) {
                    return false;
                }
                state->node->retiring_actor_keys.insert (key);
                return true;
            });
            if (!accepted)
                co_return;
            const auto finish = +[] (std::shared_ptr<detail::spot_context_state_t> retained,
                                     actor_ref_t target, std::string retiring) -> task_t<void> {
                (void) co_await runtime::await_result (
                  entry_spot_context_t (retained).destroy_actor_erased (target));
                co_await retained->node->lane.run_task (
                  [&] { retained->node->retiring_actor_keys.erase (retiring); });
            };
            const auto posted = state->try_post_serial_task_after_current_turn (
              "entry-spot-actor-destroy-after-handler",
              [state, deferred_actor, key, finish] { return finish (state, deferred_actor, key); },
              runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle});
            if (!posted) {
                co_await state->node->lane.run_task (
                  [&] { state->node->retiring_actor_keys.erase (key); });
                throw framework_exception_t (framework_error_kind_t::shutting_down,
                                             "Actor destroy queue is closed");
            }
            co_return;
        }
        std::function<task_t<result_t<void>> (actor_ref_t)> destroy_registry;
        const auto key =
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)) + ":"
          + std::string (actor.actor_id ().value ());
        const auto blocked =
          co_await state->node->actor_transfer_coordinator->blocks_dispatch_async (key);
        const auto selected = co_await state->node->lane.run_task ([&] {
            if (actor.node_rid ().empty ()
                || actor.node_rid ().value () != state->node_rid.value ()) {
                return result_t<bool>::failure (framework_error_kind_t::not_found,
                                                "actor is not owned by this Entry SPOT");
            }

            const auto found_location = state->node->actor_spot_ids.find (key);
            if (found_location != state->node->actor_spot_ids.end ()
                && found_location->second != state->spot_id) {
                return result_t<bool>::failure (framework_error_kind_t::not_found,
                                                "actor must leave its current SPOT before destroy");
            }

            const auto found_generation = state->node->actor_generations.find (key);
            if (found_generation != state->node->actor_generations.end ()
                && found_generation->second != actor.object_generation ()) {
                return result_t<bool>::success (false);
            }
            if (state->node->destroying_actors.contains (key)) {
                return result_t<bool>::success (false);
            }
            // A destroy scheduled for the retained entry-spot record must not
            // race a transfer that is re-admitting the same incarnation on this
            // node (A→B→A): the transfer commit owns the instance lifecycle.
            if (blocked) {
                return result_t<bool>::failure (framework_error_kind_t::unavailable,
                                                "Actor transfer is in progress");
            }

            const auto destroys_local_actor = found_location != state->node->actor_spot_ids.end ();
            if (destroys_local_actor) {
                state->node->destroying_actors.insert (key);
                destroy_registry = state->node->destroy_actor_registry;
            }
            return result_t<bool>::success (destroys_local_actor);
        });
        if (!selected) {
            throw framework_exception_t (selected.error_kind (), selected.error () != nullptr
                                                                   ? selected.error ()->what ()
                                                                   : "actor destroy failed");
        }
        if (!selected.value ())
            co_return;

        co_await release_actor_location_async (state->node, actor);
        co_await state->node->lane.run_task ([&] {
            if (state->node->destroying_actors.contains (key)) {
                erase_actor_route_unlocked (*state->node, key);
                state->node->actor_created_keys.erase (key);
                state->node->destroyed_actor_keys.insert (key);
                state->node->actor_instances.erase (key);
                detail::erase_actor_instance_index_unlocked (
                  *state->node, ::zlink::framework::detail::actor_ref_access_t::actor_type (actor),
                  actor.actor_id ().value ());
                (void) state->node->dispatched_request_replies.erase_if (
                  [&] (const auto &request_key) {
                      return request_key.starts_with (actor_request_dedup_prefix (key));
                  });
                decrement_actor_count_unlocked (*state);
            }
        });

        if (destroy_registry) {
            auto cleanup = co_await destroy_registry (actor);
            co_await state->node->lane.run_task (
              [&] { state->node->destroying_actors.erase (key); });
            if (!cleanup) {
                const auto *error = cleanup.error ();
                throw framework_exception_t (cleanup.error_kind (),
                                             error != nullptr ? error->what ()
                                                              : "actor registry cleanup failed");
            }
        } else {
            co_await state->node->lane.run_task (
              [&] { state->node->destroying_actors.erase (key); });
        }

        co_return;
    };
    return run (_state, actor);
}

} // namespace zlink::framework
