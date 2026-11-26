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
inline constexpr std::string_view actor_handoff_terminal_packet = "__zlink.actorHandoffTerminal";

inline constexpr std::string_view actor_handoff_terminal_success_key =
  "__zlink.actorHandoffTerminalSuccess";

inline constexpr std::string_view actor_handoff_terminal_error_kind_key =
  "__zlink.actorHandoffTerminalErrorKind";

inline constexpr std::string_view actor_handoff_terminal_error_message_key =
  "__zlink.actorHandoffTerminalErrorMessage";

detail::spot_node_builder_state_t::pending_handoff_request_key_t
handoff_pending_key (const zlink::routing_id_t &source_node,
                     const runtime::protocol::wire_operation_id_t &operation,
                     runtime::protocol::actor_route_fence_t source_fence)
{
    return {source_node.to_hex (), operation.high, operation.low, std::move (source_fence)};
}

void report_handoff_terminal_drop (const std::shared_ptr<detail::spot_node_builder_state_t> &,
                                   std::string_view reason)
{
    if (std::getenv ("ZLINK_CPP_HANDOFF_TRACE"))
        std::cerr << "zlink-cpp-handoff-terminal dropped reason=" << reason << '\n';
}

runtime::messaging::message_parts_t
handoff_reply_envelope (const runtime::messaging::envelope_header_t &request_header,
                        const result_t<zlink::message_t> &completed)
{
    detail::channel_reply_writer_t replies;
    auto terminal =
      completed
        ? detail::encode_actor_reply (
            replies.create_reply_header (runtime::messaging::message_kind_t::response,
                                         request_header.channel_name, request_header),
            completed.value ())
        : detail::encode_actor_reply (
            replies.create_error_header (
              request_header.channel_name, request_header,
              completed.error ()
                ? *completed.error ()
                : framework_exception_t (completed.error_kind (), "Actor handoff request failed")),
            zlink::message_t{});
    return terminal;
}

void stamp_handoff_actor_route (std::map<std::string, std::string> &metadata,
                                const runtime::protocol::actor_route_fence_t &route,
                                std::optional<std::uint64_t> hop_count = std::nullopt)
{
    metadata[std::string (detail::actor_handoff_route_actor_id_key)] = route.actor_id;
    metadata[std::string (detail::actor_handoff_route_object_generation_key)] =
      std::to_string (route.object_generation);
    metadata[std::string (detail::actor_handoff_route_target_node_key)] =
      zlink::routing_id_t::from (route.target_node_routing_id).to_hex ();
    metadata[std::string (detail::actor_handoff_route_target_node_generation_key)] =
      std::to_string (route.target_node_generation);
    metadata[std::string (detail::actor_handoff_route_authority_generation_key)] =
      std::to_string (route.authority_owner_generation);
    metadata[std::string (detail::actor_handoff_route_lease_generation_key)] =
      std::to_string (route.owner_lease_generation);
    if (hop_count)
        metadata[std::string (detail::actor_handoff_hop_count_key)] = std::to_string (*hop_count);
}

std::optional<std::string> actor_type_from_authority (runtime::live_location_reader_t &store,
                                                      std::string_view actor_id)
{
    const auto read = runtime::infrastructure_result (
      [&] { return store.read_authority (runtime::actor_authority_key (actor_id)); });
    const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
    if (!snapshot || snapshot->allocation.state != placement_allocation_state_t::active
        || snapshot->allocation.object_kind != placement_object_kind_t::actor)
        return std::nullopt;
    const auto projection =
      runtime::decode_actor_authority_payload (snapshot->payload, snapshot->object_generation);
    if (!projection || projection->actor.actor_id ().value () != actor_id)
        return std::nullopt;
    return std::string (
      ::zlink::framework::detail::actor_ref_access_t::actor_type (projection->actor));
}

void report_actor_handoff_request_trace (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  std::string marker,
  const actor_ref_t &actor_ref,
  std::string request_id,
  std::string transfer_id)
{
    if (!state || request_id.empty () || transfer_id.empty ()) {
        return;
    }
    if (detail::message_flow_tracer_t (state->dispatch)
          .enabled_for (message_flow_outcome_t::dispatched)) {
        detail::message_flow_tracer_t (state->dispatch)
          .trace (message_flow_outcome_t::dispatched, transfer_id, [&] {
              return message_flow_event_t{.outcome = message_flow_outcome_t::dispatched,
                                          .surface = dispatch_error_surface_t::spot_actor,
                                          .message_kind = dispatch_message_kind_t::actor_request,
                                          .packet_name = marker,
                                          .channel_name = "request",
                                          .correlation_id = request_id,
                                          .actor_id = std::string (actor_ref.actor_id ().value ()),
                                          .flow_id = transfer_id,
                                          .flow_origin = flow_origin_t::lifecycle};
          });
    }
}
} // namespace

namespace detail
{

std::string actor_request_dedup_key (std::string_view actor_key,
                                     const runtime::protocol::wire_operation_id_t &operation)
{
    auto result = actor_request_dedup_prefix (actor_key);
    result += std::to_string (operation.high);
    result += ":";
    result += std::to_string (operation.low);
    return result;
}

std::optional<std::uint64_t> handoff_u64 (const std::map<std::string, std::string> &metadata,
                                          std::string_view key)
{
    const auto found = metadata.find (std::string (key));
    if (found == metadata.end () || found->second.empty ())
        return std::nullopt;
    std::uint64_t value = 0;
    const auto parsed =
      std::from_chars (found->second.data (), found->second.data () + found->second.size (), value);
    if (parsed.ec != std::errc{} || parsed.ptr != found->second.data () + found->second.size ())
        return std::nullopt;
    return value;
}

std::optional<handoff_terminal_route_t>
handoff_terminal_route (const std::map<std::string, std::string> &metadata)
{
    const auto parking = handoff_routing_id (metadata, detail::actor_handoff_parking_node_key);
    const auto source = handoff_routing_id (metadata, detail::actor_handoff_source_node_key);
    const auto high = handoff_u64 (metadata, detail::actor_handoff_operation_high_key);
    const auto low = handoff_u64 (metadata, detail::actor_handoff_operation_low_key);
    const auto reply_route = handoff_u64 (metadata, detail::actor_handoff_reply_route_key);
    const auto source_fence =
      handoff_actor_route (metadata).value_or (runtime::protocol::actor_route_fence_t{});
    /* Clean-break policy: a terminal without the recording parking-node key
     * is explicitly dropped. It must never be redirected to the requester. */
    if (!parking || !source || !high || !low || (!*high && !*low) || !reply_route)
        return std::nullopt;
    return handoff_terminal_route_t{
      *parking, *source, {*high, *low}, *reply_route, std::move (source_fence), *parking};
}

void settle_pending_handoff_request (
  detail::spot_node_builder_state_t::pending_handoff_request_t pending,
  const result_t<zlink::message_t> &completed)
{
    auto terminal = handoff_reply_envelope (pending.request_header, completed);
    (void) service::reply (*pending.reply_token, terminal.items ());
}

task_t<bool> send_handoff_terminal (std::shared_ptr<detail::spot_node_builder_state_t> state,
                                    std::optional<handoff_terminal_route_t> route,
                                    result_t<zlink::message_t> completed)
{
    if (!route) {
        report_handoff_terminal_drop (state, "missing_parking_node");
        co_return true;
    }
    const auto local_node_rid = co_await state->lane.run_task (
      [&] { return detail::effective_spot_node_rid (state->snapshot); });
    if (route->source_node.to_string () == local_node_rid) {
        const auto pending = co_await state->lane.run_task (
          [&] () -> std::optional<detail::spot_node_builder_state_t::pending_handoff_request_t> {
              const auto found = state->pending_handoff_requests.find (handoff_pending_key (
                route->source_owner_node, route->operation, route->source_fence));
              if (found == state->pending_handoff_requests.end ()
                  || found->second.reply_route_id != route->reply_route_id
                  || route->parking_node.to_string () != local_node_rid) {
                  return std::nullopt;
              }
              auto pending = std::move (found->second);
              state->pending_handoff_requests.erase (found);
              return pending;
          });
        if (!pending) {
            co_return true;
        }
        settle_pending_handoff_request (std::move (*pending), completed);
        co_return true;
    }
    const auto terminal_sender =
      co_await state->lane.run_task ([&] { return state->actor_handoff_terminal_sender; });
    if (!terminal_sender)
        co_return false;
    co_return co_await terminal_sender (route->source_node, route->source_owner_node,
                                        route->operation, route->reply_route_id,
                                        route->source_fence, completed);
}

task_t<bool> settle_handoff_packet (std::shared_ptr<detail::spot_node_builder_state_t> state,
                                    std::shared_ptr<runtime::serial_application_record_t> record,
                                    bool request,
                                    std::optional<handoff_terminal_route_t> route,
                                    result_t<zlink::message_t> completed)
{
    if (record && record->native_reply) {
        // Native terminal shape belongs to the accepted wire record. A Session
        // command relay can carry a request terminal without an application reply.
        // The frozen identity keeps the accepted kind without restoring the body.
        const auto *identity = record->identity ();
        if (!identity)
            throw runtime::protocol::service_wire_error_t (
              "accepted record has no payload identity");
        const bool native_request =
          runtime::stateful::with_accepted_application (*identity, [] (auto kind, const auto &) {
              return kind == runtime::protocol::frozen_record_kind_t::actor_request
                     || kind == runtime::protocol::frozen_record_kind_t::spot_request;
          });
        if (!native_request)
            co_return record->complete_native (std::nullopt);
        auto header = detail::decode_actor_packet_header (
          record->application_header (), detail::stream_message_kind_t::request, false);
        if (!header)
            std::rethrow_exception (header.exception ());
        auto terminal = handoff_reply_envelope (header.value (), completed);
        co_return record->complete_native (
          runtime::protocol::application_payload_t::from_parts (terminal.items ()));
    }
    if (!request)
        co_return true;
    co_return co_await send_handoff_terminal (state, route, completed);
}

void fail_handoff_backlog (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                           std::vector<detail::handoff_packet_t> discarded)
{
    for (auto &packet : discarded) {
        auto terminal = settle_handoff_packet (
          state, packet.application_record, packet.is_request,
          handoff_terminal_route (packet.metadata),
          result_t<zlink::message_t>::failure (framework_error_kind_t::unavailable,
                                               "Actor transfer ended before backlog replay"));
        detail::observe_task_completion (terminal, [state] (const result_t<bool> &result) {
            if (!result || !result.value ()) {
                report_handoff_terminal_drop (state, "discarded_backlog_terminal_failed");
            }
        });
    }
}

std::optional<zlink::routing_id_t>
handoff_routing_id (const std::map<std::string, std::string> &metadata, std::string_view key)
{
    const auto found = metadata.find (std::string (key));
    if (found == metadata.end () || found->second.empty ())
        return std::nullopt;
    try {
        auto value = zlink::routing_id_t::from_hex (found->second);
        const auto bytes = value.to_bytes ();
        if (bytes.empty ()
            || std::all_of (bytes.begin (), bytes.end (), [] (auto byte) { return byte == 0; })) {
            return std::nullopt;
        }
        return value;
    }
    catch (...) {
        return std::nullopt;
    }
}

std::optional<runtime::protocol::actor_route_fence_t>
handoff_actor_route (const std::map<std::string, std::string> &metadata)
{
    const auto actor_id = metadata.find (std::string (detail::actor_handoff_route_actor_id_key));
    const auto target_node =
      handoff_routing_id (metadata, detail::actor_handoff_route_target_node_key);
    const auto object_generation =
      handoff_u64 (metadata, detail::actor_handoff_route_object_generation_key);
    const auto target_node_generation =
      handoff_u64 (metadata, detail::actor_handoff_route_target_node_generation_key);
    const auto authority_generation =
      handoff_u64 (metadata, detail::actor_handoff_route_authority_generation_key);
    const auto lease_generation =
      handoff_u64 (metadata, detail::actor_handoff_route_lease_generation_key);
    if (actor_id == metadata.end () || actor_id->second.empty () || !target_node
        || !object_generation || !target_node_generation || !authority_generation
        || !lease_generation || *object_generation == 0 || *target_node_generation == 0
        || *authority_generation == 0 || *lease_generation == 0) {
        return std::nullopt;
    }
    return runtime::protocol::actor_route_fence_t{actor_id->second,         *object_generation,
                                                  target_node->to_bytes (), *target_node_generation,
                                                  *authority_generation,    *lease_generation};
}

class actor_dispatch_admission_token_t final
    : public std::enable_shared_from_this<actor_dispatch_admission_token_t>
{
  public:
    using actor_message_follow_relay_t =
      decltype (std::declval<spot_node_builder_state_t> ().actor_message_follow_relay);

    struct phase_snapshot_t
    {
        std::optional<runtime::protocol::actor_route_fence_t> authority_fence;
        struct warm_materialization_t
        {
            spot_node_builder_state_t::actor_factory_registration_t factory;
            spot_id_t found_location;
            std::shared_ptr<void> actor_instance;
            std::shared_ptr<spot_context_state_t> context_state;
            std::shared_ptr<void> spot_instance;
            std::string mesh_name;
            std::uint64_t current_generation = 0;
            bool dispatch_on_spot_serial = true;
        };
        std::optional<warm_materialization_t> warm_materialization;
    };

    actor_dispatch_admission_token_t (
      std::shared_ptr<spot_node_builder_state_t> state,
      std::string actor_key,
      bool retiring,
      std::optional<runtime::protocol::actor_route_fence_t> authority_fence,
      actor_message_follow_relay_t actor_message_follow_relay) :
        _state (std::move (state)),
        _actor_key (std::move (actor_key)),
        _initial_retiring (retiring),
        _initial_authority_fence (std::move (authority_fence)),
        _actor_message_follow_relay (std::move (actor_message_follow_relay))
    {
    }

    ~actor_dispatch_admission_token_t () noexcept { settle (); }

    bool initial_retiring () const noexcept { return _initial_retiring; }

    const std::optional<runtime::protocol::actor_route_fence_t> &
    initial_authority_fence () const noexcept
    {
        return _initial_authority_fence;
    }

    actor_message_follow_relay_t actor_message_follow_relay () const
    {
        return _actor_message_follow_relay;
    }

    void reserve_handoff_reply (spot_node_builder_state_t::pending_handoff_request_key_t key,
                                spot_node_builder_state_t::pending_handoff_request_t pending)
    {
        _pending_handoff_key = std::move (key);
        _pending_handoff = std::move (pending);
    }

    result_t<phase_snapshot_t> acquire_dispatch_phase (const actor_ref_t &actor_ref)
    {
        try {
            return _state->lane
              .run ([this, &actor_ref] {
                  if (_state->retiring_actor_keys.contains (_actor_key)) {
                      return result_t<phase_snapshot_t>::failure (framework_error_kind_t::not_found,
                                                                  "actor destruction is pending");
                  }

                  phase_snapshot_t snapshot;
                  const auto current = _state->actor_authority_fences.find (_actor_key);
                  if (current != _state->actor_authority_fences.end ())
                      snapshot.authority_fence = current->second;

                  const auto found_location = _state->actor_spot_ids.find (_actor_key);
                  if (found_location != _state->actor_spot_ids.end ()) {
                      const auto found_context =
                        _state->spot_contexts_by_id.find (std::string (found_location->second));
                      if (found_context != _state->spot_contexts_by_id.end ()
                          && found_context->second._state
                          && found_context->second._state->spot_instance) {
                          const auto inherited =
                            inherit_materialized_context (found_context->second._state);
                          if (!inherited) {
                              return detail::propagate_failure<phase_snapshot_t> (
                                inherited, "actor spot lifecycle admission failed");
                          }

                          const auto actor_type = std::string (
                            ::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref));
                          const auto found_factory = _state->actor_factories.find (actor_type);
                          const auto found_instance = _state->actor_instances.find (_actor_key);
                          if (found_factory != _state->actor_factories.end ()
                              && found_instance != _state->actor_instances.end ()
                              && found_instance->second) {
                              auto warm = phase_snapshot_t::warm_materialization_t{};
                              warm.factory = found_factory->second;
                              warm.found_location = found_location->second;
                              warm.actor_instance = found_instance->second;
                              warm.context_state = found_context->second._state;
                              warm.spot_instance = warm.context_state->spot_instance;
                              warm.mesh_name = warm.context_state->mesh_name;
                              const auto found_generation =
                                _state->actor_generations.find (_actor_key);
                              warm.current_generation =
                                found_generation != _state->actor_generations.end ()
                                  ? found_generation->second
                                  : actor_ref.object_generation ();
                              if (_state->snapshot.entry_spot_name) {
                                  const auto entry_id = _state->spot_ids_by_name.find (
                                    *_state->snapshot.entry_spot_name);
                                  warm.dispatch_on_spot_serial =
                                    entry_id == _state->spot_ids_by_name.end ()
                                    || entry_id->second != warm.found_location;
                              }
                              if (warm.context_state->execution_mode
                                  == user_spot_execution_mode_t::per_actor) {
                                  warm.dispatch_on_spot_serial = false;
                              }
                              detail::record_actor_instance_index_unlocked (
                                *_state, actor_ref, warm.actor_instance.get ());
                              snapshot.warm_materialization.emplace (std::move (warm));
                          }
                      }
                  }

                  if (_pending_handoff_key && _pending_handoff) {
                      if (_state->pending_handoff_requests.contains (*_pending_handoff_key)) {
                          _pending_handoff.reset ();
                          return result_t<phase_snapshot_t>::failure (
                            framework_error_kind_t::unavailable,
                            "actor handoff reply operation is already pending");
                      }
                      const auto [_, inserted] = _state->pending_handoff_requests.emplace (
                        *_pending_handoff_key, std::move (*_pending_handoff));
                      _state->management_next_due.store (
                        spot_node_builder_state_t::management_due_now, std::memory_order_release);
                      if (!inserted) {
                          _pending_handoff.reset ();
                          return result_t<phase_snapshot_t>::failure (
                            framework_error_kind_t::unavailable,
                            "actor handoff reply reservation was not acquired");
                      }
                      _handoff_reserved.store (true, std::memory_order_release);
                      _pending_handoff.reset ();
                  }
                  return result_t<phase_snapshot_t>::success (std::move (snapshot));
              })
              .get ();
        }
        catch (const std::exception &error) {
            return result_t<phase_snapshot_t>::failure (framework_error_kind_t::unavailable,
                                                        error.what ());
        }
    }

    bool protects_context (const std::shared_ptr<spot_context_state_t> &context) const noexcept
    {
        return !_lifecycle_claimed || _context == context;
    }

    result_t<void>
    inherit_materialized_context (const std::shared_ptr<spot_context_state_t> &context)
    {
        assert (_state->lane.is_on_lane ());
        if (!context || !context->spot_instance) {
            return result_t<void>::failure (framework_error_kind_t::not_found,
                                            "actor spot context is not materialized");
        }
        if (_lifecycle_claimed) {
            return _context == context
                     ? result_t<void>::success ()
                     : result_t<void>::failure (framework_error_kind_t::unavailable,
                                                "actor spot changed after lifecycle admission");
        }
        if (_settled.load (std::memory_order_acquire)) {
            return result_t<void>::failure (framework_error_kind_t::unavailable,
                                            "actor spot lifecycle admission is closed");
        }
        if (!context->admit (true))
            return detail::result_access_t::failure<void> (context->admission_error ());
        _context = context;
        _lifecycle_claimed = true;
        return result_t<void>::success ();
    }

    bool lifecycle_claimed () const noexcept { return _lifecycle_claimed; }

    bool handoff_reserved () const noexcept
    {
        return _handoff_reserved.load (std::memory_order_acquire);
    }

    std::function<void ()> handler_terminal ()
    {
        if (!_lifecycle_claimed)
            return {};
        return [self = shared_from_this ()] { self->settle (); };
    }

    void settle () noexcept
    {
        if (_settled.exchange (true, std::memory_order_acq_rel))
            return;
        try {
            if (_lifecycle_claimed && _context)
                _context->leave_callback ();
        }
        catch (...) {
        }
    }

  private:
    std::shared_ptr<spot_node_builder_state_t> _state;
    std::string _actor_key;
    bool _initial_retiring = false;
    std::optional<runtime::protocol::actor_route_fence_t> _initial_authority_fence;
    actor_message_follow_relay_t _actor_message_follow_relay;
    std::optional<spot_node_builder_state_t::pending_handoff_request_key_t> _pending_handoff_key;
    std::optional<spot_node_builder_state_t::pending_handoff_request_t> _pending_handoff;
    std::shared_ptr<spot_context_state_t> _context;
    bool _lifecycle_claimed = false;
    std::atomic_bool _handoff_reserved{false};
    std::atomic_bool _settled{false};
};

void spot_node_runtime_t::on_actor_packet_relay (
  std::function<
    task_t<std::optional<zlink::message_t>> (const actor_ref_t &,
                                             actor_context_t,
                                             stream_message_kind_t,
                                             std::string_view,
                                             const zlink::message_t &,
                                             service_provider_t &,
                                             serializer_registry_t &,
                                             spot_inbound_message_t,
                                             const runtime::protocol::actor_route_fence_t *)> relay)
{
    _state->lane.run ([&] { _state->actor_packet_relay = std::move (relay); }).get ();
}

void spot_node_runtime_t::on_actor_message_follow (
  std::function<
    task_t<std::optional<zlink::message_t>> (const actor_ref_t &,
                                             const runtime::messaging::envelope_header_t &,
                                             const zlink::message_t &,
                                             std::chrono::milliseconds,
                                             const zlink::routing_id_t &,
                                             const runtime::protocol::actor_route_fence_t &,
                                             std::uint8_t,
                                             const runtime::protocol::wire_operation_id_t &,
                                             std::uint64_t)> relay)
{
    _state->lane.run ([&] { _state->actor_message_follow_relay = std::move (relay); }).get ();
}

void spot_node_runtime_t::on_actor_handoff_terminal (
  std::function<task_t<bool> (const zlink::routing_id_t &,
                              const zlink::routing_id_t &,
                              const runtime::protocol::wire_operation_id_t &,
                              std::uint64_t,
                              const runtime::protocol::actor_route_fence_t &,
                              const result_t<zlink::message_t> &)> sender)
{
    _state->lane.run ([&] { _state->actor_handoff_terminal_sender = std::move (sender); }).get ();
}

void spot_node_runtime_t::on_actor_leave_notification (
  std::function<task_t<zlink::submit_result_t> (const zlink::routing_id_t &,
                                                std::vector<zlink::message_t>)> sender)
{
    _state->lane.run ([&] { _state->actor_leave_notification_sender = std::move (sender); }).get ();
}

void spot_node_runtime_t::invalidate_message_follow_route (
  const runtime::protocol::message_follow_notice_t &notice)
{
    const auto *source = std::get_if<runtime::protocol::spot_route_fence_t> (&notice.source);
    if (!source)
        return;
    const auto resolver = _state->lane.run ([&] { return _state->spot_location_resolver; }).get ();
    if (!resolver)
        return;
    runtime::spot_address_t expected;
    expected.spot_id = source->spot_id;
    expected.node_rid = zlink::routing_id_t::from (source->target_node_routing_id);
    expected.node_generation = source->target_node_generation;
    expected.object_generation = source->object_generation;
    expected.authority_owner_generation = source->authority_owner_generation;
    expected.owner.lease_generation = static_cast<std::int64_t> (source->owner_lease_generation);
    (void) resolver->invalidate_spot_address_if_matches (source->spot_id, expected);
}

task_t<std::optional<zlink::message_t>>
spot_node_runtime_t::relay_actor_packet (const actor_ref_t &actor_ref,
                                         actor_context_t actor_context,
                                         std::string_view packet_name,
                                         const zlink::message_t &message,
                                         service_provider_t &services,
                                         serializer_registry_t &serializers,
                                         spot_inbound_message_t metadata)
{
    co_return co_await relay_actor_packet (actor_ref, std::move (actor_context),
                                           stream_message_kind_t::request, packet_name, message,
                                           services, serializers, std::move (metadata));
}

task_t<std::optional<zlink::message_t>> spot_node_runtime_t::relay_actor_packet (
  const actor_ref_t &actor_ref,
  actor_context_t actor_context,
  stream_message_kind_t message_kind,
  std::string_view packet_name,
  const zlink::message_t &message,
  service_provider_t &services,
  serializer_registry_t &serializers,
  spot_inbound_message_t metadata,
  const runtime::protocol::actor_route_fence_t *admitted_message_follow_target,
  runtime::handler_entry_t before_application_handler,
  std::function<void ()> after_application_admission,
  zlink::routing_id_t inbound_source_node_rid,
  runtime::protocol::wire_operation_id_t inbound_operation,
  std::uint64_t inbound_reply_route_id,
  std::optional<std::string> inbound_deadline,
  std::function<void ()> transfer_owner_reservation,
  std::size_t transferred_owner_byte_cost,
  std::shared_ptr<actor_dispatch_admission_token_t> admission_token,
  std::shared_ptr<runtime::serial_application_record_t> application_record,
  spot_handler_registry_t::actor_queue_dispatch_t actor_queue_dispatch)
{
    // This member coroutine can be entered through a short-lived runtime
    // wrapper. Keep the node state in the frame for the terminal path below:
    // a handler may suspend for a relocating Join, after which completion
    // updates the request deduplication table.
    const auto state_owner = _state;
    if (application_record && before_application_handler)
        application_record->admission.retain_initial (std::move (before_application_handler));
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        co_return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::not_found, "actor ref is empty");
    }

    const auto key = actor_key (actor_ref);
    const auto dispatch_kind = message_kind == stream_message_kind_t::send
                                 ? dispatch_message_kind_t::actor_send
                                 : dispatch_message_kind_t::actor_request;
    report_actor_dispatch_stage_trace (
      _state, message_flow_outcome_t::received, dispatch_kind, packet_name, {},
      actor_ref.actor_id ().value (), "relay_actor_packet.enter",
      admitted_message_follow_target == nullptr ? "follow_fence=none" : "follow_fence=present");
    struct actor_relay_prefence_state_snapshot_t
    {
        bool retiring = false;
        bool authority_projected = false;
        bool has_current_authority = false;
        std::optional<runtime::protocol::actor_route_fence_t> authority_fence;
    };
    const auto prefence_snapshot =
      admission_token
        ? actor_relay_prefence_state_snapshot_t{admission_token->initial_retiring (), true,
                                                admission_token->initial_authority_fence ()
                                                  .has_value (),
                                                admission_token->initial_authority_fence ()}
        : _state->lane.run_sync ([&] {
              actor_relay_prefence_state_snapshot_t snapshot;
              snapshot.retiring = _state->retiring_actor_keys.contains (key);
              if (snapshot.retiring)
                  return snapshot;
              snapshot.authority_projected = admitted_message_follow_target != nullptr;
              const auto current = _state->actor_authority_fences.find (key);
              snapshot.has_current_authority = current != _state->actor_authority_fences.end ();
              if (snapshot.has_current_authority && snapshot.authority_projected)
                  snapshot.authority_fence = current->second;
              return snapshot;
          });
    if (prefence_snapshot.retiring) {
        co_return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::not_found, "actor destruction is pending");
    }
    // A cold hit carries no follow fence (e.g. a client probing the actor's
    // former owner directly, unaware it already relocated). Relocation
    // preserves ObjectGeneration, so a retained Message Follow route for this
    // key at the actor's believed generation is this node's own source fence
    // for that route; borrow it so the fence-validated relay path below
    // (which normally only runs for an already-fenced hop) also covers the
    // very first hop instead of falling through to a local spot lookup that
    // can never resolve (the route points at the remote target's spot).
    runtime::protocol::actor_route_fence_t synthesized_follow_fence;
    if (admitted_message_follow_target == nullptr) {
        if (const auto found =
              _state->actor_transfer_coordinator->message_follow_source_for_generation (
                key, actor_ref.object_generation ())) {
            synthesized_follow_fence = *found;
            admitted_message_follow_target = &synthesized_follow_fence;
        }
    }
    if (admitted_message_follow_target != nullptr) {
        auto current_fence = prefence_snapshot.authority_fence;
        if (!prefence_snapshot.authority_projected) {
            current_fence = _state->lane.run_sync ([&] {
                const auto current = _state->actor_authority_fences.find (key);
                return current == _state->actor_authority_fences.end ()
                         ? std::optional<runtime::protocol::actor_route_fence_t>{}
                         : std::make_optional (current->second);
            });
        }
        // The exact adoption fence gates only when this node retains state
        // that could be confused with an older incarnation: a recorded
        // adoption fence or a retained Message Follow source route. A
        // fenced packet the mesh route admission already accepted for an
        // actor without either dispatches to the current local authority.
        bool targets_current_authority =
          current_fence ? *current_fence == *admitted_message_follow_target
                        : !_state->actor_transfer_coordinator->has_message_follow_route (key);
        if (!targets_current_authority && current_fence) {
            //  Spec 21 §6.3: a followed message is deliverable when the
            //  new owner's AuthorityOwnerGeneration is greater than the
            //  edge value the previous owner retained at move
            //  completion — forwarding can traverse several moves, so
            //  the final owner's fence is ordered against, not equal
            //  to, the followed edge's fence. The object incarnation
            //  and the fenced target node must still match exactly.
            const auto &m = *admitted_message_follow_target;
            targets_current_authority =
              current_fence->actor_id == m.actor_id
              && current_fence->object_generation == m.object_generation
              && current_fence->target_node_routing_id == m.target_node_routing_id
              && current_fence->authority_owner_generation > m.authority_owner_generation;
        }
        if (!targets_current_authority
            && !matches_actor_message_follow_source (actor_ref, *admitted_message_follow_target)) {
            report_actor_dispatch_stage_trace (
              _state, message_flow_outcome_t::received, dispatch_kind, packet_name, {},
              actor_ref.actor_id ().value (), "relay_actor_packet.follow_admission",
              "accepted=false");
            {
                //  Spec 26 Detailed-scope diagnostics: retain both fence
                //  tuples in the Detailed-only stage/result extension without
                //  overloading any standard address attribute.
                detail::message_flow_tracer_t (_state->dispatch)
                  .trace (message_flow_outcome_t::dropped, [&, current_fence] {
                      const auto describe = [] (
                                              const runtime::protocol::actor_route_fence_t &fence) {
                          return fence.actor_id + "/og=" + std::to_string (fence.object_generation)
                                 + "/ng=" + std::to_string (fence.target_node_generation)
                                 + "/ag=" + std::to_string (fence.authority_owner_generation)
                                 + "/lg=" + std::to_string (fence.owner_lease_generation);
                      };
                      auto event = message_flow_event_t{
                        message_flow_outcome_t::dropped, dispatch_error_surface_t::spot_actor,
                        dispatch_message_kind_t::actor_send, std::string ("follow_fence")};
                      event.detail_stage = "admission";
                      event.detail_result = !current_fence ? std::string ("local=none")
                                                           : "local=" + describe (*current_fence);
                      event.detail_result = *event.detail_result + " message="
                                            + describe (*admitted_message_follow_target);
                      event.actor_id = std::string (actor_ref.actor_id ().value ());
                      event.reason = message_flow_reason_t::stale_target;
                      return event;
                  });
            }
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::unavailable,
              "Actor Message Follow target fence is not admitted");
        }
        report_actor_dispatch_stage_trace (_state, message_flow_outcome_t::received, dispatch_kind,
                                           packet_name, {}, actor_ref.actor_id ().value (),
                                           "relay_actor_packet.follow_admission", "accepted=true");
    } else {
        report_actor_dispatch_stage_trace (_state, message_flow_outcome_t::received, dispatch_kind,
                                           packet_name, {}, actor_ref.actor_id ().value (),
                                           "relay_actor_packet.follow_admission", "required=false");
    }
    if (!prefence_snapshot.has_current_authority
        && (admitted_message_follow_target == nullptr
            || !matches_actor_message_follow_source (actor_ref, *admitted_message_follow_target))) {
        if (const auto host = _state->native_node.lock ();
            host
            && host->objects ().finalized_relocation_source (
              runtime::stateful::object_kind_t::actor, std::string (actor_ref.actor_id ().value ()),
              actor_ref.object_generation ())) {
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::unavailable,
              "Actor relocation source has no Message Follow route");
        }
    }
    const auto handoff = metadata.values.find ("__zlink.actorHandoffBacklog");
    const auto handoff_transfer_id = metadata.values.find ("__zlink.actorTransferId");
    if (handoff != metadata.values.end () && handoff->second == "true"
        && handoff_transfer_id != metadata.values.end () && !handoff_transfer_id->second.empty ()) {
        if (actor_transfer_marker_enabled ()) {
            emit_actor_transfer_marker ("backlog_enqueued", actor_ref, handoff_transfer_id->second,
                                        actor_spot (actor_ref));
        }
    }
    // A node can become the current owner again while an older source route
    // for the same Actor incarnation is still retained. Resolve that exact
    // authority before choosing the handoff backlog: packets admitted through
    // a committed source Message Follow edge must relay immediately. Keeping
    // those post-commit arrivals in the source backlog lets a continuous
    // producer prevent finish_move_replay() from ever observing it empty.
    std::optional<runtime::protocol::actor_route_fence_t> phase_authority_fence;
    std::optional<actor_dispatch_admission_token_t::phase_snapshot_t::warm_materialization_t>
      warm_materialization;
    if (admission_token) {
        const auto phase = admission_token->acquire_dispatch_phase (actor_ref);
        if (!phase) {
            co_return detail::propagate_failure<std::optional<zlink::message_t>> (
              phase, "actor dispatch admission failed");
        }
        phase_authority_fence = phase.value ().authority_fence;
        warm_materialization = std::move (phase.value ().warm_materialization);
    }
    bool targets_current_authority = admitted_message_follow_target == nullptr;
    if (admitted_message_follow_target != nullptr) {
        const auto current_fence =
          admission_token ? phase_authority_fence : _state->lane.run_sync ([&] {
              const auto current = _state->actor_authority_fences.find (key);
              return current == _state->actor_authority_fences.end ()
                       ? std::optional<runtime::protocol::actor_route_fence_t>{}
                       : std::make_optional (current->second);
          });
        targets_current_authority =
          current_fence ? *current_fence == *admitted_message_follow_target
                        : !_state->actor_transfer_coordinator->has_message_follow_route (key);
        if (!targets_current_authority && current_fence) {
            const auto &m = *admitted_message_follow_target;
            targets_current_authority =
              current_fence->actor_id == m.actor_id
              && current_fence->object_generation == m.object_generation
              && current_fence->target_node_routing_id == m.target_node_routing_id
              && current_fence->authority_owner_generation > m.authority_owner_generation;
        }
    }
    // Both initial and final admission use the same Message Follow operation,
    // preserving its identity, payload, reply route and original deadline.
    auto relay_message_follow =
      [&] (const runtime::protocol::actor_route_fence_t &source_fence,
           const detail::handoff_packet_t &packet) -> task_t<std::optional<zlink::message_t>> {
        auto relay =
          admission_token
            ? admission_token->actor_message_follow_relay ()
            : state_owner->lane.run_sync ([&] { return state_owner->actor_message_follow_relay; });
        if (!relay)
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::unavailable, "Actor Message Follow relay is unavailable");
        std::uint8_t incoming_hop_count = 0;
        if (const auto hop = packet.metadata.find ("__zlink.messageFollowHopCount");
            hop != packet.metadata.end ()) {
            const auto parsed = std::stoul (hop->second);
            if (parsed > runtime::protocol::messageFollowHopCount)
                co_return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::protocol_error,
                  "Actor Message Follow hop count is invalid");
            incoming_hop_count = static_cast<std::uint8_t> (parsed);
        }
        runtime::messaging::envelope_header_t header;
        header.kind = packet.is_request ? runtime::messaging::message_kind_t::request
                                        : runtime::messaging::message_kind_t::command;
        header.channel_name = "actor";
        header.message_name = packet.packet_name;
        header.content_type = packet.content_type;
        header.metadata = packet.metadata;
        header.deadline = std::move (inbound_deadline);
        co_return co_await relay (actor_ref, header, message, std::chrono::seconds (30),
                                  inbound_source_node_rid, source_fence, incoming_hop_count,
                                  inbound_operation, inbound_reply_route_id);
    };
    // Initial admission may release this packet while materialization prepares
    // the Actor queue. Final admission rechecks and posts in one coordinator turn.
    std::optional<detail::handoff_packet_t> dispatch_packet;
    {
        if (message_kind == stream_message_kind_t::request && inbound_deadline)
            metadata.values[std::string (detail::actor_handoff_deadline_key)] = *inbound_deadline;
        // In-flight handoff (§10.2-1): actor packets that arrive while the actor
        // is moving are preserved in arrival order and travel to the target with
        // the commit. Sends co_return
        // the empty success shape so preservation is indistinguishable from
        // immediate dispatch. A preserved request keeps its source reply token;
        // the target sends one internal terminal envelope after replay.
        const bool is_request = message_kind == stream_message_kind_t::request;
        auto packet_admission = _state->actor_transfer_coordinator->admit_dispatch_packet (
          key, admitted_message_follow_target, targets_current_authority,
          detail::handoff_packet_t{
            std::string (packet_name),
            application_record ? std::vector<std::uint8_t>{} : message.to_bytes (),
            metadata.content_type, metadata.values, is_request, application_record});
        const auto targets_committed_source = !targets_current_authority
                                              && admitted_message_follow_target != nullptr
                                              && packet_admission.matches_message_follow_source;
        const auto append_result = packet_admission.append_result;
        dispatch_packet = std::move (packet_admission.released_packet);
        const auto append_result_name = [&] {
            switch (append_result) {
                case detail::handoff_append_result_t::not_moving:
                    return "not_moving";
                case detail::handoff_append_result_t::appended:
                    return "appended";
                case detail::handoff_append_result_t::duplicate_request:
                    return "duplicate_request";
            }
            return "unknown";
        }();
        if (_state
            && detail::message_flow_tracer_t (_state->dispatch)
                 .enabled (message_flow_log_mode_t::detailed)) {
            report_actor_dispatch_stage_trace_lazy (
              _state,
              append_result == detail::handoff_append_result_t::appended && !application_record
                ? message_flow_outcome_t::admitted
                : message_flow_outcome_t::received,
              dispatch_kind, packet_name, {}, actor_ref.actor_id ().value (),
              "relay_actor_packet.try_append_backlog",
              [&] { return "result=" + std::string (append_result_name); });
        }
        if (append_result == detail::handoff_append_result_t::appended) {
            if (actor_transfer_marker_enabled ()) {
                emit_actor_transfer_marker (
                  "handoff_backlog", actor_ref,
                  _state->actor_transfer_coordinator->transfer_id (key).value_or (key));
            }
            if (is_request) {
                const auto request_id = metadata.values.find ("__zlink.actorRequestId");
                if (request_id != metadata.values.end ()) {
                    if (detail::message_flow_tracer_t (_state->dispatch)
                          .enabled_for (message_flow_outcome_t::dispatched)) {
                        report_actor_handoff_request_trace (
                          _state, "handoff_request_frame", actor_ref, request_id->second,
                          _state->actor_transfer_coordinator->transfer_id (key).value_or (key));
                    }
                }
            }
            if (after_application_admission)
                after_application_admission ();
            if (!is_request) {
                co_return result_t<std::optional<zlink::message_t>>::success (zlink::message_t{});
            }
            co_return result_t<std::optional<zlink::message_t>>::success (std::nullopt);
        }
        if (append_result != detail::handoff_append_result_t::not_moving) {
            co_return detail::result_access_t::failure<std::optional<zlink::message_t>> (
              detail::make_origin_exception (framework_error_kind_t::unavailable,
                                             detail::failure_origin_t::actor_transfer_in_progress,
                                             "actor transfer is in progress"));
        }

        // Only the exact committed target fence may bypass an older Message
        // Follow route; matching the node or ObjectGeneration alone is
        // insufficient (Spec 21 §6.3).
        if (!targets_current_authority && !targets_committed_source) {
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::unavailable,
              "Actor Message Follow target fence is no longer current");
        }
        if (!targets_current_authority && targets_committed_source)
            co_return co_await relay_message_follow (*admitted_message_follow_target,
                                                     *dispatch_packet);
    }

    using actor_factory_registration_t =
      detail::spot_node_builder_state_t::actor_factory_registration_t;
    struct actor_dispatch_plan_t
    {
        spot_id_t current_spot_id;
        std::uint64_t current_generation = 0;
        std::optional<runtime::protocol::actor_route_fence_t> authority_fence;
        std::optional<actor_ref_t> current_actor_ref;
        std::shared_ptr<detail::spot_context_state_t> context_state;
        std::shared_ptr<void> spot_instance;
        std::string mesh_name;
        bool dispatch_on_spot_serial = true;
    };
    const auto dispatch_flow = message_kind == stream_message_kind_t::send
                                   && runtime::flow_context_t::tracing (
                                     detail::message_flow_tracer_t (state_owner->dispatch).mode ())
                                 ? runtime::flow_context_t::current ()
                                 : std::optional<runtime::flow_value_t>{};
    const auto report_missing_actor_target = [&] {
        if (message_kind != stream_message_kind_t::send)
            return;
        detail::dispatch_error_reporter_t (state_owner->dispatch).report_lazy ([&] {
            return message_dispatch_error_event_t{
              .surface = dispatch_error_surface_t::spot_actor,
              .message_kind = dispatch_message_kind_t::actor_send,
              .reason = dispatch_error_reason_t::stale_target,
              .action = dispatch_error_action_t::drop,
              .packet_name = std::string (packet_name),
              .actor_id = std::string (actor_ref.actor_id ().value ()),
              .source_rid = inbound_source_node_rid.to_hex (),
              .correlation_id = metadata.correlation_id,
              .flow_id = dispatch_flow ? std::make_optional (dispatch_flow->flow_id) : std::nullopt,
              .flow_origin =
                dispatch_flow ? std::make_optional (dispatch_flow->origin) : std::nullopt,
              .mesh_name = state_owner->snapshot.name};
        });
        runtime::runtime_metrics_t metrics (state_owner->monitoring);
        if (metrics.enabled ())
            metrics.counter ("zlink.mesh_node.messages.dropped", "{message}", 1,
                             {{"mesh_name", state_owner->snapshot.name},
                              {"surface", "actor"},
                              {"message_kind", "send"},
                              {"reason", "stale_target"}});
    };
    auto project_actor_dispatch_state =
      [&] (const std::optional<spot_id_t> &found_location) -> result_t<actor_dispatch_plan_t> {
        if (!found_location) {
            report_missing_actor_target ();
            return result_t<actor_dispatch_plan_t>::failure (
              framework_error_kind_t::not_found, "actor spot route disappeared before dispatch");
        }

        actor_dispatch_plan_t plan;
        if (const auto authority = _state->actor_authority_fences.find (key);
            authority != _state->actor_authority_fences.end ())
            plan.authority_fence = authority->second;
        // Actor movement can replace or erase the route while a user callback
        // is suspended. Keep the complete derived dispatch projection in this turn.
        plan.current_spot_id = *found_location;
        const auto found_generation = _state->actor_generations.find (key);
        plan.current_generation = found_generation != _state->actor_generations.end ()
                                    ? found_generation->second
                                    : actor_ref.object_generation ();
        const auto current_actor_node_rid = actor_ref.node_rid ().empty ()
                                              ? detail::effective_spot_node_rid (_state->snapshot)
                                              : std::string (actor_ref.node_rid ().value ());
        plan.current_actor_ref = ::zlink::framework::detail::actor_ref_access_t::make (
          node_rid_t::from_string (current_actor_node_rid),
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
          std::string (actor_ref.actor_id ().value ()), plan.current_generation);
        const auto context = _state->spot_contexts_by_id.find (std::string (plan.current_spot_id));
        if (context == _state->spot_contexts_by_id.end () || !context->second._state
            || !context->second._state->spot_instance) {
            report_missing_actor_target ();
            return result_t<actor_dispatch_plan_t>::failure (
              framework_error_kind_t::not_found,
              "actor spot context is not registered. node="
                + detail::effective_spot_node_rid (_state->snapshot) + ", actor="
                + std::string (actor_ref.actor_id ().value ()) + ", spot=" + plan.current_spot_id);
        }
        plan.context_state = context->second._state;
        plan.spot_instance = plan.context_state->spot_instance;
        plan.mesh_name = plan.context_state->mesh_name;
        if (admission_token) {
            const auto inherited =
              admission_token->inherit_materialized_context (plan.context_state);
            if (!inherited) {
                return detail::propagate_failure<actor_dispatch_plan_t> (
                  inherited, "actor dispatch lifecycle admission failed");
            }
        }
        if (_state->snapshot.entry_spot_name) {
            const auto entry_id = _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
            plan.dispatch_on_spot_serial = entry_id == _state->spot_ids_by_name.end ()
                                           || entry_id->second != plan.current_spot_id;
        }
        if (plan.context_state->execution_mode == user_spot_execution_mode_t::per_actor) {
            plan.dispatch_on_spot_serial = false;
        }
        return result_t<actor_dispatch_plan_t>::success (std::move (plan));
    };
    struct actor_state_snapshot_t
    {
        actor_factory_registration_t factory;
        std::optional<spot_id_t> found_location;
        std::shared_ptr<void> actor_instance;
        std::string actor_mesh_name;
        std::optional<result_t<actor_dispatch_plan_t>> dispatch;
    };
    auto materialized = [&] () -> result_t<actor_state_snapshot_t> {
        if (warm_materialization) {
            auto warm = std::move (*warm_materialization);
            actor_dispatch_plan_t dispatch;
            dispatch.current_spot_id = warm.found_location;
            dispatch.current_generation = warm.current_generation;
            dispatch.authority_fence = std::move (phase_authority_fence);
            const auto current_actor_node_rid =
              actor_ref.node_rid ().empty () ? detail::effective_spot_node_rid (_state->snapshot)
                                             : std::string (actor_ref.node_rid ().value ());
            dispatch.current_actor_ref = ::zlink::framework::detail::actor_ref_access_t::make (
              node_rid_t::from_string (current_actor_node_rid),
              std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
              std::string (actor_ref.actor_id ().value ()), dispatch.current_generation);
            dispatch.context_state = std::move (warm.context_state);
            dispatch.spot_instance = std::move (warm.spot_instance);
            dispatch.mesh_name = std::move (warm.mesh_name);
            dispatch.dispatch_on_spot_serial = warm.dispatch_on_spot_serial;

            actor_state_snapshot_t plan;
            plan.factory = std::move (warm.factory);
            plan.found_location = std::move (warm.found_location);
            plan.actor_instance = std::move (warm.actor_instance);
            plan.dispatch.emplace (result_t<actor_dispatch_plan_t>::success (std::move (dispatch)));
            return result_t<actor_state_snapshot_t>::success (std::move (plan));
        }

        return _state->lane
          .run ([&] () -> result_t<actor_state_snapshot_t> {
              const auto actor_type_key = std::string (
                ::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref));
              const auto found_factory = _state->actor_factories.find (actor_type_key);
              if (found_factory == _state->actor_factories.end ()) {
                  return result_t<actor_state_snapshot_t>::failure (
                    framework_error_kind_t::not_found, "actor factory is not registered");
              }

              actor_state_snapshot_t plan;
              plan.factory = found_factory->second;
              const auto found_location = _state->actor_spot_ids.find (key);
              if (found_location != _state->actor_spot_ids.end ()) {
                  plan.found_location = found_location->second;
              } else if (_state->destroyed_actor_keys.contains (key)) {
                  return result_t<actor_state_snapshot_t>::failure (
                    framework_error_kind_t::not_found, "actor has been destroyed");
              }

              const auto found_instance = _state->actor_instances.find (key);
              if (found_instance != _state->actor_instances.end () && found_instance->second) {
                  plan.actor_instance = found_instance->second;
                  detail::record_actor_instance_index_unlocked (*_state, actor_ref,
                                                                plan.actor_instance.get ());
                  if (plan.found_location) {
                      plan.dispatch = project_actor_dispatch_state (plan.found_location);
                  }
              } else if (plan.factory.create_context_instance) {
                  plan.actor_mesh_name = _state->snapshot.name;
                  if (plan.found_location) {
                      const auto actor_spot_context =
                        _state->spot_contexts_by_id.find (std::string (*plan.found_location));
                      if (actor_spot_context != _state->spot_contexts_by_id.end ()
                          && actor_spot_context->second._state) {
                          plan.actor_mesh_name = actor_spot_context->second._state->mesh_name;
                      }
                  }
              }
              return result_t<actor_state_snapshot_t>::success (std::move (plan));
          })
          .get ();
    }();
    if (!materialized) {
        co_return detail::propagate_failure<std::optional<zlink::message_t>> (
          materialized, "actor materialization failed");
    }
    auto materialization = std::move (materialized.value ());
    auto factory = std::move (materialization.factory);
    const auto initial_location = materialization.found_location;

    /* Same double-checked registration as the join paths. */
    std::shared_ptr<void> actor_instance = std::move (materialization.actor_instance);
    if (!actor_instance) {
        std::shared_ptr<void> created_instance;
        if (factory.create_context_instance) {
            auto materialization_context = actor_context_t (
              actor_context._state, actor_ref, actor_context._source_binding_generation,
              std::move (materialization.actor_mesh_name));
            created_instance =
              factory.create_context_instance (std::move (materialization_context));
        } else {
            created_instance =
              factory.create_instance (std::string (actor_ref.actor_id ().value ()));
        }
        if (!created_instance) {
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::not_found, "actor factory returned null");
        }
        actor_instance =
          _state->lane
            .run ([&] {
                auto &slot = _state->actor_instances[key];
                if (!slot) {
                    if (_state->destroyed_actor_keys.contains (key)) {
                        _state->actor_instances.erase (key);
                        return std::shared_ptr<void>{};
                    }
                    slot = std::move (created_instance);
                }
                detail::record_actor_instance_index_unlocked (*_state, actor_ref, slot.get ());
                return slot;
            })
            .get ();
        if (!actor_instance) {
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::not_found, "actor has been destroyed");
        }
    }

    if (!initial_location) {
        struct entry_admission_plan_t
        {
            std::shared_ptr<detail::spot_context_state_t> context_state;
            std::shared_ptr<void> spot_instance;
            std::optional<spot_actor_admission_callbacks_t> admission;
            bool creation_reserved = false;
        };
        auto entry_planned =
          _state->lane
            .run ([&] () -> result_t<entry_admission_plan_t> {
                if (!_state->snapshot.entry_spot_name) {
                    return result_t<entry_admission_plan_t>::failure (
                      framework_error_kind_t::not_found, "entry spot is not registered");
                }
                const auto entry_id =
                  _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
                if (entry_id == _state->spot_ids_by_name.end ()) {
                    return result_t<entry_admission_plan_t>::failure (
                      framework_error_kind_t::not_found, "entry spot is not created");
                }
                const auto entry_context =
                  _state->spot_contexts_by_id.find (std::string (entry_id->second));
                if (entry_context == _state->spot_contexts_by_id.end ()
                    || !entry_context->second._state
                    || !entry_context->second._state->spot_instance) {
                    return result_t<entry_admission_plan_t>::failure (
                      framework_error_kind_t::not_found, "entry spot context is not registered");
                }

                entry_admission_plan_t plan;
                plan.context_state = entry_context->second._state;
                plan.spot_instance = plan.context_state->spot_instance;
                detail::record_actor_context_route_unlocked (
                  *_state, key, detail::effective_spot_node_rid (_state->snapshot),
                  *plan.context_state, actor_ref.object_generation ());
                const auto admission =
                  plan.context_state->actor_admissions.find (factory.actor_type);
                if (admission != plan.context_state->actor_admissions.end ()) {
                    plan.admission = admission->second;
                    plan.creation_reserved = plan.admission->on_create_actor
                                             && _state->actor_created_keys.insert (key).second;
                }
                return result_t<entry_admission_plan_t>::success (std::move (plan));
            })
            .get ();
        if (!entry_planned) {
            co_return detail::propagate_failure<std::optional<zlink::message_t>> (
              entry_planned, "entry spot admission failed");
        }
        auto entry_plan = std::move (entry_planned.value ());
        bool created_entry_actor = false;
        if (entry_plan.admission) {
            if (entry_plan.creation_reserved) {
                const auto create_request =
                  actor_context.create_payload ().value_or (zlink::message_t{});
                const auto created =
                  entry_plan.admission
                    ->on_create_actor (entry_plan.spot_instance.get (), actor_instance.get (),
                                       create_request, serializers)
                    .result ();
                if (!created || !created.value ().accepted) {
                    _state->lane
                      .run ([&] {
                          _state->actor_created_keys.erase (key);
                          erase_actor_route_unlocked (*_state, key);
                      })
                      .get ();
                    if (!created)
                        co_return detail::propagate_failure<std::optional<zlink::message_t>> (
                          created, "Entry Spot Actor creation failed");
                    co_return result_t<std::optional<zlink::message_t>>::failure (
                      framework_error_kind_t::rejected, "Entry Spot rejected Actor creation");
                }
                created_entry_actor = true;
            }
            if (!created_entry_actor && entry_plan.admission->on_actor_joined) {
                // The relay continues when the lifecycle item completes.
                std::optional<framework_exception_t> join_failure;
                try {
                    co_await entry_plan.context_state->run_serial_task ("spot-lifecycle-join", [&] {
                        return entry_plan.admission->on_actor_joined (
                          entry_plan.spot_instance.get (), actor_instance.get ());
                    });
                }
                catch (const framework_exception_t &error) {
                    join_failure = error;
                }
                if (join_failure)
                    co_return detail::result_access_t::failure<std::optional<zlink::message_t>> (
                      *join_failure);
            }
        }
    }

    auto dispatch_planned = std::move (materialization.dispatch);
    if (!dispatch_planned) {
        dispatch_planned = _state->lane
                             .run ([&] {
                                 const auto found_location = _state->actor_spot_ids.find (key);
                                 return project_actor_dispatch_state (
                                   found_location == _state->actor_spot_ids.end ()
                                     ? std::optional<spot_id_t>{}
                                     : std::make_optional (found_location->second));
                             })
                             .get ();
    }
    if (!*dispatch_planned) {
        co_return detail::propagate_failure<std::optional<zlink::message_t>> (
          *dispatch_planned, "actor dispatch state projection failed");
    }
    auto dispatch_plan = std::move (dispatch_planned->value ());
    if (admission_token && admission_token->lifecycle_claimed ()
        && !admission_token->protects_context (dispatch_plan.context_state)) {
        co_return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::unavailable, "actor spot changed after lifecycle admission");
    }
    const auto current_spot_id = dispatch_plan.current_spot_id;
    const auto &current_actor_ref = *dispatch_plan.current_actor_ref;
    if (!factory.create_context_instance) {
        auto current_actor_context =
          actor_context_t (actor_context._state, current_actor_ref,
                           actor_context._source_binding_generation, dispatch_plan.mesh_name);
        factory.configure_instance (actor_instance.get (), current_actor_ref,
                                    &current_actor_context);
    }
    const bool dispatch_on_spot_serial = dispatch_plan.dispatch_on_spot_serial;

    const auto handler_kind = message_kind == stream_message_kind_t::send
                                ? spot_handler_kind_t::actor_send
                                : spot_handler_kind_t::actor_request;
    // Submit §6: terminal identity is the complete original OperationId.
    const auto terminal_route = message_kind == stream_message_kind_t::request
                                  ? handoff_terminal_route (metadata.values)
                                  : std::nullopt;
    const auto dedup_operation =
      terminal_route ? std::make_optional (terminal_route->operation) : std::nullopt;
    auto request_claim = runtime::exactly_once_claim_state::pending;
    auto claim_request =
      [&] (const zlink::message_t &,
           const spot_inbound_message_t &) -> std::optional<result_t<zlink::message_t>> {
        if (!dedup_operation)
            return std::nullopt;
        auto claim = state_owner->dispatched_request_replies.claim (
          actor_request_dedup_key (key, *dedup_operation));
        request_claim = claim.state;
        if (claim.state == runtime::exactly_once_claim_state::completed && claim.value)
            return result_t<zlink::message_t>::success (*claim.value);
        if (claim.state == runtime::exactly_once_claim_state::pending)
            return result_t<zlink::message_t>::failure (framework_error_kind_t::unavailable,
                                                        "actor request dispatch is in flight");
        return std::nullopt;
    };
    report_spot_dispatch_trace (_state, message_flow_outcome_t::received,
                                dispatch_error_surface_t::spot_actor, dispatch_kind, packet_name,
                                {}, current_spot_id, actor_ref.actor_id ().value ());
    const bool request_delivery = message_kind == stream_message_kind_t::request;
    std::optional<detail::actor_transfer_packet_admission_t> final_admission;
    std::function<std::optional<bool> (std::function<bool ()>)> admit_queue;
    if (actor_queue_dispatch == spot_handler_registry_t::actor_queue_dispatch_t::acquire)
        admit_queue = [&] (std::function<bool ()> submit) {
            final_admission = state_owner->actor_transfer_coordinator->admit_dispatch_packet (
              key, admitted_message_follow_target, targets_current_authority,
              std::move (*dispatch_packet), std::move (submit),
              dispatch_plan.authority_fence ? &*dispatch_plan.authority_fence : nullptr,
              current_actor_ref.object_generation ());
            if (final_admission->append_result == detail::handoff_append_result_t::appended)
                report_actor_dispatch_stage_trace (
                  state_owner,
                  application_record ? message_flow_outcome_t::received
                                     : message_flow_outcome_t::admitted,
                  dispatch_kind, packet_name, {}, actor_ref.actor_id ().value (),
                  "relay_actor_packet.try_append_backlog", "result=appended");
            return final_admission->queue_submission;
        };
    auto invoked = result_t<zlink::message_t>::success ({});
    try {
        invoked = result_t<zlink::message_t>::success (
          co_await spot_handler_registry_t (dispatch_plan.context_state)
            .invoke_erased (
              std::move (before_application_handler), handler_kind, packet_name, {},
              factory.actor_type, dispatch_plan.spot_instance.get (), actor_instance.get (),
              services, serializers, message, std::move (metadata), dispatch_on_spot_serial, key,
              current_spot_id, claim_request, actor_queue_dispatch, after_application_admission,
              std::move (transfer_owner_reservation), transferred_owner_byte_cost,
              admission_token ? admission_token->handler_terminal () : std::function<void ()>{},
              admit_queue, {}, application_record,
              [&final_admission] {
                  final_admission.emplace ();
                  final_admission->append_result = detail::handoff_append_result_t::appended;
              },
              actor_ref.actor_id ().value ()));
    }
    catch (const framework_exception_t &error) {
        invoked = detail::result_access_t::failure<zlink::message_t> (error);
    }
    catch (const std::exception &error) {
        invoked = result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                       error.what ());
    }
    if (invoked && final_admission && !final_admission->queue_submission) {
        if (final_admission->message_follow_source)
            co_return co_await relay_message_follow (*final_admission->message_follow_source,
                                                     *final_admission->released_packet);
        if (final_admission->append_result == detail::handoff_append_result_t::appended) {
            if (actor_transfer_marker_enabled ()) {
                emit_actor_transfer_marker (
                  "handoff_backlog", actor_ref,
                  state_owner->actor_transfer_coordinator->transfer_id (key).value_or (key));
            }
            co_return result_t<std::optional<zlink::message_t>>::success (
              request_delivery ? std::optional<zlink::message_t>{}
                               : std::make_optional (zlink::message_t{}));
        }
        co_return detail::result_access_t::failure<std::optional<zlink::message_t>> (
          detail::make_origin_exception (framework_error_kind_t::unavailable,
                                         detail::failure_origin_t::actor_transfer_in_progress,
                                         "actor transfer is in progress"));
    }
    if (message_kind == stream_message_kind_t::send && application_record
        && application_record->admission.durable () && application_record->native_reply) {
        // Ordinary delivery and replay use the same native terminal owner.
        (void) co_await settle_handoff_packet (state_owner, application_record, false, std::nullopt,
                                               invoked);
    }
    if (!invoked) {
        if (dedup_operation && request_claim == runtime::exactly_once_claim_state::claimed)
            (void) state_owner->dispatched_request_replies.erase (
              actor_request_dedup_key (key, *dedup_operation));
        co_return detail::propagate_failure<std::optional<zlink::message_t>> (
          invoked, "Actor handler failed");
    }
    if (dedup_operation && request_claim == runtime::exactly_once_claim_state::claimed) {
        (void) state_owner->dispatched_request_replies.complete (
          actor_request_dedup_key (key, *dedup_operation), invoked.value ());
    }
    co_return result_t<std::optional<zlink::message_t>>::success (invoked.value ());
}

task_t<zlink::message_t>
spot_node_runtime_t::notify_actor_disconnected_erased (void *instance,
                                                       std::type_index actor_type,
                                                       actor_gateway_runtime_t gateway,
                                                       zlink::message_t body) const
{
    const auto state_owner = _state;
    struct disconnect_plan_t
    {
        actor_ref_t actor;
        std::shared_ptr<spot_context_state_t> context_state;
        std::shared_ptr<void> spot_instance;
        std::shared_ptr<void> actor_instance;
        std::function<task_t<void> (void *, void *)> disconnect_callback;
    };
    // Resolve the invoked instance through the existing node index. Ordinary
    // ingress, saved-work replay and direct handler invocation share this owner.
    auto planned =
      co_await state_owner->lane.run_task ([&] () -> result_t<std::optional<disconnect_plan_t>> {
          const auto indexed = state_owner->actor_instance_index.find (instance);
          if (indexed == state_owner->actor_instance_index.end ())
              return result_t<std::optional<disconnect_plan_t>>::failure (
                framework_error_kind_t::not_found, "actor instance is not registered");
          const auto key = indexed->second.first + ":" + indexed->second.second;
          const auto actor = state_owner->actor_instances.find (key);
          const auto generation = state_owner->actor_generations.find (key);
          if (actor == state_owner->actor_instances.end () || actor->second.get () != instance
              || generation == state_owner->actor_generations.end ())
              return result_t<std::optional<disconnect_plan_t>>::failure (
                framework_error_kind_t::not_found, "actor instance is not registered");
          const auto location = state_owner->actor_spot_ids.find (key);
          if (location == state_owner->actor_spot_ids.end ())
              return result_t<std::optional<disconnect_plan_t>>::success (std::nullopt);
          const auto context =
            state_owner->spot_contexts_by_id.find (std::string (location->second));
          if (context == state_owner->spot_contexts_by_id.end () || !context->second._state
              || !context->second._state->spot_instance)
              return result_t<std::optional<disconnect_plan_t>>::success (std::nullopt);
          const auto context_state = context->second._state;
          const auto admission = context_state->actor_admissions.find (actor_type);
          if (admission == context_state->actor_admissions.end ()
              || !admission->second.on_disconnect_actor)
              return result_t<std::optional<disconnect_plan_t>>::success (std::nullopt);
          return result_t<std::optional<disconnect_plan_t>>::success (disconnect_plan_t{
            actor_ref_access_t::make (context_state->node_rid, indexed->second.first,
                                      indexed->second.second, generation->second),
            context_state, context_state->spot_instance, actor->second,
            admission->second.on_disconnect_actor});
      });
    if (!planned)
        co_return detail::propagate_failure<zlink::message_t> (
          planned, "actor disconnect state admission failed");
    if (!planned.value ())
        co_return zlink::message_t{};
    auto plan = std::move (*planned.value ());
    runtime::protocol::session_disconnected_identity_t identity;
    try {
        identity = runtime::protocol::decode_session_disconnected (body.to_bytes ());
    }
    catch (const runtime::protocol::service_wire_error_t &error) {
        detail::dispatch_error_reporter_t (state_owner->dispatch).report_lazy ([&] {
            return message_dispatch_error_event_t{
              .surface = dispatch_error_surface_t::spot_actor,
              // This notification is dropped with a successful reply (binding §7).
              .message_kind = dispatch_message_kind_t::actor_send,
              .reason = dispatch_error_reason_t::invalid_frame,
              .action = dispatch_error_action_t::drop,
              .packet_name = runtime::protocol::session_disconnected_packet_name,
              .actor_id = std::string (plan.actor.actor_id ().value ()),
              .exception = std::make_exception_ptr (
                framework_exception_t (framework_error_kind_t::protocol_error, error.what ()))};
        });
        co_return zlink::message_t{};
    }
    const auto current = co_await gateway.bound_session_route_async (plan.actor);
    if (current
        && (current->node_rid.to_bytes () != identity.session_owner_node_rid
            || current->node_generation != identity.session_owner_node_generation
            || current->binding_generation != identity.binding_generation))
        co_return zlink::message_t{};
    const auto key = actor_key (plan.actor);
    co_await plan.context_state->run_serial_task (
      "spot-lifecycle-disconnect",
      [key, context_state = plan.context_state, spot_instance = plan.spot_instance,
       actor_instance = plan.actor_instance, disconnect_callback = plan.disconnect_callback] {
          runtime::actor_execution_scope_t actor_execution (key,
                                                            std::string (context_state->spot_id));
          return disconnect_callback (spot_instance.get (), actor_instance.get ());
      });
    co_return zlink::message_t{};
}

template <typename TJoinResult>
task_t<void> spot_node_runtime_t::run_actor_join_admission_task (
  spot_node_runtime_t runtime,
  std::shared_ptr<TJoinResult> join_result,
  actor_ref_t actor,
  service::actor_control_t control,
  std::string join_spot_id,
  std::vector<zlink::message_t> parts,
  bool remote_transfer,
  zlink::message_t request,
  bool targets_entry_spot,
  std::string local_node_rid,
  service_provider_t *services,
  serializer_registry_t *serializers,
  std::optional<std::chrono::steady_clock::time_point> deadline)
{
    join_result->joined.emplace (co_await runtime::await_result (runtime.run_actor_join_control (
      actor, control, join_spot_id, parts, request, remote_transfer, targets_entry_spot,
      local_node_rid, *services, *serializers, &join_result->source_leave, deadline)));
}

namespace
{

template <typename TTarget>
task_t<void> run_actor_handler_turn_task (
  TTarget target,
  std::function<task_t<result_t<bool>> (actor_t &, std::stop_token)> work,
  std::stop_token cancellation,
  std::shared_ptr<std::optional<result_t<bool>>> outcome,
  std::string actor_key)
{
    runtime::actor_execution_scope_t actor_scope (std::move (actor_key), target.context->spot_id);
    target.context->enter_callback ();
    try {
        outcome->emplace (co_await work (*target.actor, cancellation));
    }
    catch (...) {
        outcome->emplace (detail::current_exception_result<bool> ());
    }
    target.context->leave_callback ();
    co_return;
}

} // namespace

task_t<result_t<bool>> spot_node_runtime_t::run_actor_turn (
  actor_ref_t actor_ref,
  std::function<task_t<result_t<bool>> (actor_t &, std::stop_token)> work,
  std::stop_token cancellation)
{
    if (!work)
        co_return result_t<bool>::failure (framework_error_kind_t::protocol_error,
                                           "Actor handler-turn work must not be empty");
    const auto state = _state;
    struct actor_turn_target_t
    {
        std::shared_ptr<void> instance;
        actor_t *actor = nullptr;
        std::shared_ptr<spot_context_state_t> context;
        actor_ref_t actor_ref;
    };
    auto target = co_await state->lane.run_task (
      [state, actor_ref = std::move (actor_ref)] () mutable -> std::optional<actor_turn_target_t> {
          auto actor_type =
            std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref));
          if (actor_type.empty ()) {
              const auto type =
                state->actor_types_by_id.find (std::string (actor_ref.actor_id ().value ()));
              if (type == state->actor_types_by_id.end ())
                  return std::nullopt;
              actor_type = type->second;
              actor_ref = ::zlink::framework::detail::actor_ref_access_t::with_actor_type (
                actor_ref, actor_type);
          }
          const auto key = actor_key (actor_ref);
          if (state->retiring_actor_keys.contains (key) || state->destroying_actors.contains (key))
              return std::nullopt;
          const auto factory = state->actor_factories.find (actor_type);
          const auto instance = state->actor_instances.find (key);
          const auto membership = state->actor_spot_ids.find (key);
          if (factory == state->actor_factories.end () || !factory->second.as_actor
              || instance == state->actor_instances.end () || !instance->second
              || membership == state->actor_spot_ids.end ())
              return std::nullopt;
          const auto context = state->spot_contexts_by_id.find (std::string (membership->second));
          if (context == state->spot_contexts_by_id.end () || !context->second._state)
              return std::nullopt;
          return actor_turn_target_t{instance->second,
                                     factory->second.as_actor (instance->second.get ()),
                                     context->second._state, std::move (actor_ref)};
      });
    if (!target || !target->actor || !target->context)
        co_return result_t<bool>::failure (framework_error_kind_t::not_found,
                                           "Actor handler-turn target is not registered");

    const auto actor_context = target->context;
    if (!actor_context->admit ())
        co_return detail::result_access_t::failure<bool> (actor_context->admission_error ());
    const auto executor = actor_context->spot_serial_executor;
    if (!executor)
        co_return result_t<bool>::failure (framework_error_kind_t::not_found,
                                           "Actor handler-turn executor is unavailable");
    const auto key = actor_key (target->actor_ref);
    auto outcome = std::make_shared<std::optional<result_t<bool>>> ();
    auto completion = std::make_shared<task_completion_source_t<void>> ();
    const auto submitted = executor->execute_actor_lifecycle (
      key, "actor-handler-turn",
      [target = std::move (*target), work = std::move (work), cancellation, outcome, completion,
       key] (auto complete) mutable {
          callback_context_scope_t callback_scope (target.context.get ());
          auto running = std::make_shared<task_t<void>> (run_actor_handler_turn_task (
            std::move (target), std::move (work), cancellation, outcome, std::move (key)));
          detail::observe_task_completion (
            *running, [running, complete, completion] (const result_t<void> &result) mutable {
                complete ([result, completion] {
                    completion->complete (result);
                    result.value ();
                });
            });
      },
      [completion] {
          completion->complete (result_t<void>::failure (framework_error_kind_t::shutting_down,
                                                         "Actor handler-turn gate is closed"));
      });
    if (!submitted)
        co_return result_t<bool>::failure (framework_error_kind_t::shutting_down,
                                           "Actor handler-turn queue is closed");
    try {
        co_await completion->task ();
    }
    catch (...) {
        co_return detail::current_exception_result<bool> ();
    }
    if (!*outcome)
        co_return result_t<bool>::failure (framework_error_kind_t::internal_failure,
                                           "Actor handler turn completed without a result");
    co_return std::move (**outcome);
}

bool spot_node_runtime_t::matches_actor_message_follow_source (
  const actor_ref_t &actor_ref, const runtime::protocol::actor_route_fence_t &source_fence) const
{
    return _state->actor_transfer_coordinator->matches_message_follow_source (actor_key (actor_ref),
                                                                              source_fence);
}

result_t<std::optional<actor_message_follow_target_t>>
spot_node_runtime_t::try_acquire_actor_message_follow (
  const actor_ref_t &actor_ref,
  std::size_t payload_bytes,
  std::size_t hop_count,
  const runtime::protocol::actor_route_fence_t &source_fence)
{
    return _state->actor_transfer_coordinator->try_acquire_message_follow (
      actor_key (actor_ref), actor_ref.object_generation (), payload_bytes, hop_count,
      source_fence);
}

void spot_node_runtime_t::release_actor_message_follow (
  const actor_ref_t &actor_ref,
  const runtime::protocol::actor_route_fence_t &source_fence,
  std::size_t payload_bytes) noexcept
{
    _state->actor_transfer_coordinator->release_message_follow (actor_key (actor_ref), source_fence,
                                                                payload_bytes);
}

bool spot_node_runtime_t::try_begin_actor_message_follow_notification (
  const actor_ref_t &actor_ref,
  const runtime::protocol::actor_route_fence_t &source_fence,
  const runtime::protocol::actor_route_fence_t &target_fence)
{
    return _state->actor_transfer_coordinator->try_begin_message_follow_notification (
      actor_key (actor_ref), source_fence, target_fence);
}

bool spot_node_runtime_t::complete_actor_message_follow_notification (
  const actor_ref_t &actor_ref,
  const runtime::protocol::actor_route_fence_t &source_fence,
  const runtime::protocol::actor_route_fence_t &target_fence,
  bool transport_accepted)
{
    return _state->actor_transfer_coordinator->complete_message_follow_notification (
      actor_key (actor_ref), source_fence, target_fence, transport_accepted);
}

std::optional<std::string> spot_node_runtime_t::actor_route_transport_name () const
{
    return _state->lane
      .run ([&] () -> std::optional<std::string> {
          if (_state->snapshot.spot_route_channel_name
              && !_state->snapshot.spot_route_channel_name->empty ()) {
              return _state->snapshot.spot_route_channel_name;
          }
          if (_state->snapshot.accepted_route_channels.size () == 1) {
              return _state->snapshot.accepted_route_channels.front ().channel_name;
          }
          if (_state->snapshot.discovery_channel_name
              && !_state->snapshot.discovery_channel_name->empty ()) {
              return _state->snapshot.discovery_channel_name;
          }
          return std::nullopt;
      })
      .get ();
}

bool spot_node_runtime_t::dispatch_mesh_record (
  const service::ready_record_t &owner,
  const service::receive_record_t &record,
  std::vector<zlink::message_t> &parts,
  const std::optional<runtime::messaging::envelope_header_t> &decoded_header,
  service_provider_t &services,
  serializer_registry_t &serializers,
  std::function<void ()> deferred_terminal,
  bool *terminal_deferred,
  runtime::handler_entry_t before_application_handler,
  runtime::serial_execution_queue_t::async_completion_t application_completed,
  runtime::application_dispatch_terminal_owner_t *local_terminal)
{
    std::shared_ptr<runtime::application_dispatch_terminal_owner_t> promoted_terminal;
    const auto defer_terminal = [&] () -> std::function<void ()> {
        if (!local_terminal)
            return std::move (deferred_terminal);
        promoted_terminal = std::make_shared<runtime::application_dispatch_terminal_owner_t> (
          std::move (*local_terminal));
        local_terminal = nullptr;
        return [owned = promoted_terminal] { owned->settle (); };
    };
    if (terminal_deferred)
        *terminal_deferred = false;
    if (owner.owner_kind == service::owner_kind_t::spot
        && record.kind == service::record_kind_t::spot_multicast) {
        const auto binding =
          _state->lane
            .run ([&] {
                auto context = find_context_core (spot_id_t (owner.spot_id));
                if (!context) {
                    const auto pending = _state->pending_spot_creations_by_id.find (owner.spot_id);
                    if (pending != _state->pending_spot_creations_by_id.end ()) {
                        if (auto preparing = pending->second.context.lock ())
                            context.emplace (spot_context_t (std::move (preparing)));
                    }
                }
                auto native = context ? context->_state->native_spot.lock ()
                                      : std::shared_ptr<service::spot_t>{};
                if (context && record.spot_route
                    && (!native || native->spot_id () != record.spot_route->spot_id
                        || native->status ().lifecycle_generation ()
                             != record.spot_route->object_generation))
                    context.reset ();
                return std::make_pair (std::move (context), std::move (native));
            })
            .get ();
        const auto &context = binding.first;
        if (!context)
            return true;
        auto submission = dispatch_subscription (
          *context, record.topic, parts, services, serializers,
          std::move (before_application_handler), record.release_mailbox_reservation,
          record.transferred_owner_byte_cost, &record);
        auto dispatched =
          submission && submission.value ()
            ? std::move (*submission.value ())
            : task_t<zlink::message_t> (
                submission ? result_t<zlink::message_t>::success ({})
                           : result_t<zlink::message_t>::failure (
                               submission.error_kind (), submission.error ()
                                                           ? submission.error ()->what ()
                                                           : "SPOT subscription dispatch failed"));
        if (terminal_deferred && (deferred_terminal || local_terminal))
            *terminal_deferred = true;
        detail::observe_task_terminal (
          dispatched,
          [state = _state, topic = record.topic, context_state = context->_state,
           deferred_terminal = defer_terminal ()] (const result_t<zlink::message_t> &result) {
              if (!result) {
                  const framework_exception_t error (
                    result.error_kind (), result.error () ? result.error ()->what ()
                                                          : "SPOT subscription dispatch failed");
                  report_spot_dispatch_error (
                    state, dispatch_error_surface_t::spot_route, dispatch_message_kind_t::send,
                    dispatch_reason_from_error (error.kind ()), dispatch_error_action_t::drop,
                    std::nullopt, topic, std::string (context_state->spot_id), std::nullopt,
                    std::make_exception_ptr (error));
              }
              if (deferred_terminal)
                  deferred_terminal ();
          });
        return true;
    }
    const bool route_send = record.kind == service::record_kind_t::spot_send
                            || record.kind == service::record_kind_t::node_send
                            || record.kind == service::record_kind_t::channel_send;
    const bool route_request = record.kind == service::record_kind_t::spot_request
                               || record.kind == service::record_kind_t::node_request
                               || record.kind == service::record_kind_t::channel_request;
    const bool spot_record = owner.owner_kind == service::owner_kind_t::spot
                             && (record.kind == service::record_kind_t::spot_send
                                 || record.kind == service::record_kind_t::spot_request);
    const bool node_record = owner.owner_kind == service::owner_kind_t::node
                             && (record.kind == service::record_kind_t::node_send
                                 || record.kind == service::record_kind_t::node_request);
    if (route_send || route_request) {
        const auto route_client =
          std::atomic_load_explicit (&_state->route_client, std::memory_order_acquire);
        runtime::messaging::message_parts_t encoded (std::move (parts));
        runtime::messaging::envelope_codec_t codec;
        const auto &header = *decoded_header;
        if (!route_client) {
            parts = std::move (encoded).take_items ();
            return false;
        }

        if (node_record && record.kind == service::record_kind_t::node_send
            && header.message_name == actor_handoff_terminal_packet) {
            const auto terminal_route = handoff_terminal_route (header.metadata.values ());
            const auto success =
              handoff_u64 (header.metadata.values (), actor_handoff_terminal_success_key);
            if (!terminal_route || !success || *success > 1) {
                if (!terminal_route)
                    report_handoff_terminal_drop (_state, "missing_parking_node");
                return true;
            }
            auto settle = [] (std::shared_ptr<spot_node_builder_state_t> node,
                              runtime::messaging::message_parts_t encoded,
                              runtime::messaging::envelope_header_t header,
                              zlink::routing_id_t source_node_rid, auto terminal_route,
                              std::uint64_t success) -> task_t<void> {
                const auto key =
                  handoff_pending_key (terminal_route->source_owner_node, terminal_route->operation,
                                       terminal_route->source_fence);
                using identity_t = std::tuple<actor_ref_t, runtime::protocol::actor_route_fence_t,
                                              std::chrono::steady_clock::time_point>;
                const auto identity =
                  co_await node->lane.run_task ([&] () -> std::optional<identity_t> {
                      const auto found = node->pending_handoff_requests.find (key);
                      if (found == node->pending_handoff_requests.end ()
                          || found->second.reply_route_id != terminal_route->reply_route_id
                          || terminal_route->parking_node.to_string ()
                               != detail::effective_spot_node_rid (node->snapshot))
                          return std::nullopt;
                      return identity_t{found->second.actor, found->second.source_fence,
                                        found->second.deadline};
                  });
                if (!identity)
                    co_return;
                const auto admitted =
                  co_await node->actor_transfer_coordinator->message_follow_targets_node_async (
                    actor_key (std::get<0> (*identity)), source_node_rid, std::get<1> (*identity));
                if (!admitted)
                    co_return;
                const auto pending = co_await node->lane.run_task ([&] {
                    const auto found = node->pending_handoff_requests.find (key);
                    if (found == node->pending_handoff_requests.end ()
                        || found->second.reply_route_id != terminal_route->reply_route_id
                        || actor_key (found->second.actor) != actor_key (std::get<0> (*identity))
                        || found->second.source_fence != std::get<1> (*identity)
                        || found->second.deadline != std::get<2> (*identity))
                        return std::shared_ptr<
                          spot_node_builder_state_t::pending_handoff_request_t>{};
                    auto result =
                      std::make_shared<spot_node_builder_state_t::pending_handoff_request_t> (
                        std::move (found->second));
                    node->pending_handoff_requests.erase (found);
                    return result;
                });
                if (!pending)
                    co_return;
                result_t<zlink::message_t> completed = result_t<zlink::message_t>::failure (
                  framework_error_kind_t::internal_failure, "Actor handoff request failed");
                if (success == 1) {
                    auto body = runtime::messaging::envelope_codec_t{}.decode_body (encoded);
                    completed = body
                                  ? result_t<zlink::message_t>::success (std::move (body.value ()))
                                  : result_t<zlink::message_t>::failure (
                                      framework_error_kind_t::protocol_error,
                                      "Actor handoff terminal body missing");
                } else {
                    const auto error_kind_value = handoff_u64 (
                      header.metadata.values (), actor_handoff_terminal_error_kind_key);
                    const auto error_message =
                      header.metadata.find (std::string (actor_handoff_terminal_error_message_key));
                    completed = result_t<zlink::message_t>::failure (
                      error_kind_value ? static_cast<framework_error_kind_t> (*error_kind_value)
                                       : framework_error_kind_t::internal_failure,
                      error_message == header.metadata.end () ? "Actor handoff request failed"
                                                              : error_message->second);
                }
                settle_pending_handoff_request (std::move (*pending), completed);
                co_return;
            }(_state, std::move (encoded), header, record.source_node_rid, terminal_route,
                                                     *success);
            if (deferred_terminal || local_terminal) {
                detail::observe_task_completion (
                  settle, [node = _state,
                           deferred_terminal = defer_terminal ()] (const result_t<void> &terminal) {
                      const auto failures = node->runtime_failures ();
                      if (!terminal)
                          failures->report (terminal.exception ());
                      failures->capture ([&] { deferred_terminal (); });
                  });
                if (terminal_deferred)
                    *terminal_deferred = true;
            } else {
                settle.result ().value ();
            }
            return true;
        }

        /* Native SPOT delivery bypasses the RouteMesh packet dispatcher after
         * the envelope has been decoded. Re-enter the wire flow here so the
         * remote Spot handler observes the same flow as the originating
         * STREAM/session request. */
        const auto flow_mode = detail::message_flow_tracer_t (_state->dispatch).mode ();
        auto flow_scope = runtime::flow_context_t::enter (
          header.flow_id, header.flow_origin, flow_mode, flow_origin_t::inbound,
          runtime::flow_context_t::tracing (flow_mode) && record.source_session_rid
            ? std::optional<std::string> (record.source_session_rid->to_hex ())
            : std::nullopt);

        auto &actor_gateway = services.get_required<actor_gateway_runtime_t> ();
        spot_route_internal_dispatcher_t dispatcher (*this, actor_gateway, *route_client,
                                                     serializers);
        if (route_send && dispatcher.can_handle_send (header.message_name)) {
            const bool transfer_actor_leave_owner_reservation =
              node_record && header.message_name == spot_actor_leave_route_command_t::packet_name
              && (deferred_terminal || local_terminal);
            route_received_packet_t received{record.source_node_rid,
                                             record.operation_id.low == 0
                                               ? std::nullopt
                                               : std::make_optional (record.operation_id.low),
                                             std::move (encoded), std::nullopt, record.received_at};
            const auto report = [node = _state, header, spot_id = owner.spot_id,
                                 operation_low =
                                   record.operation_id.low] (const result_t<void> &dispatched) {
                if (!dispatched) {
                    const framework_exception_t error (dispatched.error_kind (),
                                                       dispatched.error () != nullptr
                                                         ? dispatched.error ()->what ()
                                                         : "SPOT route send failed");
                    report_spot_dispatch_error (
                      node, dispatch_error_surface_t::spot_route, dispatch_message_kind_t::send,
                      dispatch_reason_from_error (error.kind ()), dispatch_error_action_t::drop,
                      header.message_name, std::nullopt,
                      spot_id.empty () ? std::nullopt : std::make_optional (spot_id), std::nullopt,
                      std::make_exception_ptr (error),
                      operation_low == 0 ? std::nullopt
                                         : std::make_optional (std::to_string (operation_low)));
                }
            };
            auto dispatched =
              transfer_actor_leave_owner_reservation
                ? dispatcher.dispatch_actor_leave_async (std::move (received), defer_terminal (),
                                                         record.transferred_owner_byte_cost)
                : dispatcher.dispatch_send (received, header, services);
            detail::observe_task_completion (
              dispatched, [report] (const result_t<result_t<void>> &terminal) {
                  report (terminal ? terminal.value ()
                                   : detail::propagate_failure<void> (
                                       terminal, "SPOT route send dispatch failed"));
              });
            if (transfer_actor_leave_owner_reservation && terminal_deferred)
                *terminal_deferred = true;
            return true;
        }
        if (route_request && dispatcher.can_handle_request (header.message_name)) {
            route_received_packet_t received{record.source_node_rid,
                                             record.operation_id.low == 0
                                               ? std::nullopt
                                               : std::make_optional (record.operation_id.low),
                                             std::move (encoded), std::nullopt, record.received_at};
            detail::channel_reply_writer_t replies;
            if (deferred_terminal || local_terminal) {
                const auto async_dispatched = dispatcher.dispatch_request_async (
                  received, header, services,
                  [reply_token = record.reply_token, request_header = header,
                   deferred_terminal =
                     defer_terminal ()] (result_t<zlink::message_t> response) mutable {
                      try {
                          detail::channel_reply_writer_t async_replies;
                          const auto reply_parts =
                            response ? async_replies.reply_raw_envelope (
                                         async_replies.create_reply_header (
                                           runtime::messaging::message_kind_t::response,
                                           request_header.channel_name, request_header),
                                         std::move (response.value ()))
                                     : async_replies.reply_raw_envelope (
                                         async_replies.create_error_header (
                                           request_header.channel_name, request_header,
                                           /* Internal route dispatcher failures are
                                     * framework-generated (zlink.origin marker). */
                                           detail::make_framework_origin_exception (
                                             response.error_kind (),
                                             response.error () ? response.error ()->what ()
                                                               : "SPOT route request failed")),
                                         zlink::message_t::from (""));
                          (void) service::reply (reply_token, reply_parts.items ());
                      }
                      catch (...) {
                      }
                      deferred_terminal ();
                  },
                  runtime::protocol::wire_operation_id_t{record.operation_id.high,
                                                         record.operation_id.low},
                  record.reply_route_id);
                if (async_dispatched) {
                    if (terminal_deferred)
                        *terminal_deferred = true;
                    return true;
                }
            }
            auto response = dispatcher.dispatch_request (received, header, services);
            const auto reply_parts =
              response
                ? replies.reply_raw_envelope (
                    replies.create_reply_header (runtime::messaging::message_kind_t::response,
                                                 header.channel_name, header),
                    std::move (response.value ()))
                : replies.reply_raw_envelope (
                    replies.create_error_header (
                      header.channel_name, header,
                      /* Internal route dispatcher failures are framework-
                       * generated (zlink.origin marker). */
                      detail::make_framework_origin_exception (
                        response.error_kind (), response.error () ? response.error ()->what ()
                                                                  : "SPOT route request failed")),
                    zlink::message_t::from (""));
            (void) service::reply (record.reply_token, reply_parts.items ());
            return true;
        }
        if (spot_record) {
            detail::channel_reply_writer_t replies;
            auto reply_error = [&] (const framework_exception_t &error) {
                if (record.kind != service::record_kind_t::spot_request) {
                    return;
                }
                auto reply = replies.reply_raw_envelope (
                  replies.create_error_header (header.channel_name, header, error),
                  zlink::message_t::from (""));
                (void) service::reply (record.reply_token, reply.items ());
            };
            auto target = record.application_record ? record.application_record->target : nullptr;
            if (!target) {
                const auto prepared =
                  prepare_spot_dispatch (owner.spot_id, record.spot_route).result ();
                if (!prepared) {
                    reply_error (prepared.error ()
                                   ? *prepared.error ()
                                   : framework_exception_t (prepared.error_kind (),
                                                            "Spot dispatch target unavailable"));
                    return true;
                }
                target = prepared.value ();
            }
            if (const auto error = std::get_if<framework_exception_t> (&target->context)) {
                reply_error (*error);
                return true;
            }
            const auto context_state = std::static_pointer_cast<spot_context_state_t> (
              std::get<std::shared_ptr<void>> (target->context));
            try {
                /* Complete route admission before creating the typed body. */
                auto body = codec.decode_body (encoded);
                if (!body) {
                    reply_error (detail::make_framework_origin_exception (
                      body.error_kind (), body.error () ? body.error ()->what ()
                                                        : "Spot request envelope body is invalid"));
                    return true;
                }
                report_spot_dispatch_trace (_state, message_flow_outcome_t::received,
                                            dispatch_error_surface_t::spot_route,
                                            record.kind == service::record_kind_t::spot_request
                                              ? dispatch_message_kind_t::request
                                              : dispatch_message_kind_t::send,
                                            header.message_name, {}, owner.spot_id);
                using terminal_t = std::function<void (const result_t<zlink::message_t> &)>;
                std::shared_ptr<terminal_t> terminal;
                if (record.kind == service::record_kind_t::spot_request
                    || runtime::flow_context_t::tracing (flow_mode)) {
                    terminal = std::make_shared<terminal_t> (
                      [state = _state, header, spot_id = owner.spot_id, kind = record.kind,
                       reply_token =
                         record.reply_token] (const result_t<zlink::message_t> &result) {
                          if (kind == service::record_kind_t::spot_request) {
                              detail::channel_reply_writer_t writer;
                              auto reply =
                                result ? writer.reply_raw_envelope (
                                           writer.create_reply_header (
                                             runtime::messaging::message_kind_t::response,
                                             header.channel_name, header),
                                           result.value ().copy ())
                                       : writer.reply_raw_envelope (
                                           writer.create_error_header (
                                             header.channel_name, header,
                                             result.error ()
                                               ? *result.error ()
                                               : framework_exception_t (result.error_kind (),
                                                                        "Spot handler failed")),
                                           zlink::message_t::from (""));
                              (void) service::reply (reply_token, reply.items ());
                          }
                          report_spot_dispatch_trace (state,
                                                      kind == service::record_kind_t::spot_request
                                                        ? message_flow_outcome_t::replied
                                                        : message_flow_outcome_t::dispatched,
                                                      dispatch_error_surface_t::spot_route,
                                                      kind == service::record_kind_t::spot_request
                                                        ? dispatch_message_kind_t::response
                                                        : dispatch_message_kind_t::send,
                                                      header.message_name, {}, spot_id);
                      });
                }
                const auto release_target = [weak = std::weak_ptr (record.application_record)] {
                    if (const auto accepted = weak.lock ())
                        accepted->target.reset ();
                };
                std::function<void ()> on_export = release_target;
                if (terminal)
                    on_export = [terminal, release_target] {
                        *terminal = {};
                        release_target ();
                    };
                auto handled =
                  spot_handler_registry_t (context_state)
                    .invoke_erased (
                      std::move (before_application_handler), spot_handler_kind_t::packet,
                      header.message_name, {}, std::type_index (typeid (void)),
                      target->instance.get (), nullptr, services, serializers, body.value (),
                      spot_inbound_message_t{.content_type = header.content_type,
                                             .values = header.metadata.values (),
                                             .received_at = record.received_at},
                      true, {}, {}, {}, spot_handler_registry_t::actor_queue_dispatch_t::acquire,
                      [target] { target->release (); }, record.release_mailbox_reservation,
                      record.transferred_owner_byte_cost, {}, {}, {}, record.application_record,
                      std::move (on_export));
                if (terminal_deferred)
                    *terminal_deferred = true;
                detail::observe_task_completion (
                  handled, [terminal, deferred_terminal = defer_terminal ()] (
                             const result_t<zlink::message_t> &result) mutable {
                      try {
                          if (terminal) {
                              if (auto complete = std::exchange (*terminal, {}))
                                  complete (result);
                          }
                      }
                      catch (...) {
                          if (deferred_terminal)
                              deferred_terminal ();
                          throw;
                      }
                      if (deferred_terminal)
                          deferred_terminal ();
                  });
                return true;
            }
            catch (const framework_exception_t &error) {
                reply_error (error);
                return true;
            }
            catch (const std::exception &error) {
                reply_error (
                  framework_exception_t (framework_error_kind_t::internal_failure, error.what ()));
                return true;
            }
            catch (...) {
                reply_error (framework_exception_t (framework_error_kind_t::internal_failure,
                                                    "Spot handler dispatch failed"));
                return true;
            }
        }
        parts = std::move (encoded).take_items ();
        return false;
    }
    if (owner.owner_kind == service::owner_kind_t::actor
        && (record.kind == service::record_kind_t::actor_send
            || record.kind == service::record_kind_t::actor_request)) {
        const auto actor_id = std::string (owner.actor->actor_id ().value ());
        const bool has_bound_session_source = record.source_session_rid.has_value ()
                                              || record.source_binding_generation != 0
                                              || record.source_session_sequence != 0;
        using actor_route_admission_t = decltype (_state->actor_route_admission);
        using actor_message_follow_relay_t = decltype (_state->actor_message_follow_relay);
        // These node-owned values all feed this mesh-record dispatch phase.
        // Calls through the projected ports remain outside the state lane.
        struct actor_mesh_dispatch_state_snapshot_t
        {
            std::string actor_type;
            std::string actor_key;
            std::shared_ptr<service::mesh_node_t> native_node;
            std::shared_ptr<runtime::stateful::authority_relocation_port_t> relocation_authority;
            actor_route_admission_t actor_route_admission;
            actor_message_follow_relay_t actor_message_follow_relay;
            std::optional<runtime::protocol::actor_route_fence_t> authority_fence;
            std::string relay_parking_node_rid;
            std::shared_ptr<actor_dispatch_admission_token_t> admission_token;
        };
        const auto project_actor_mesh_dispatch_state =
          [&] (std::optional<std::string_view> resolved_type) {
              return _state->lane
                .run ([&] {
                    actor_mesh_dispatch_state_snapshot_t snapshot;
                    const auto found = _state->actor_types_by_id.find (actor_id);
                    if (resolved_type && !resolved_type->empty ()) {
                        snapshot.actor_type = *resolved_type;
                        _state->actor_types_by_id[actor_id] = snapshot.actor_type;
                    } else if (found != _state->actor_types_by_id.end ()
                               && !found->second.empty ()) {
                        snapshot.actor_type = found->second;
                    }
                    if (snapshot.actor_type.empty ())
                        return snapshot;

                    snapshot.actor_key = snapshot.actor_type + ":" + actor_id;
                    snapshot.native_node = _state->native_node.lock ();
                    snapshot.relocation_authority = _state->relocation_authority;
                    snapshot.actor_route_admission = _state->actor_route_admission;
                    snapshot.actor_message_follow_relay = _state->actor_message_follow_relay;
                    std::optional<runtime::protocol::actor_route_fence_t> token_authority_fence;
                    const auto current = _state->actor_authority_fences.find (snapshot.actor_key);
                    if (current != _state->actor_authority_fences.end ()) {
                        token_authority_fence = current->second;
                        if (record.actor_route && !has_bound_session_source)
                            snapshot.authority_fence = current->second;
                    }
                    snapshot.relay_parking_node_rid =
                      detail::effective_spot_node_rid (_state->snapshot);
                    snapshot.admission_token = std::make_shared<actor_dispatch_admission_token_t> (
                      _state, snapshot.actor_key,
                      _state->retiring_actor_keys.contains (snapshot.actor_key),
                      std::move (token_authority_fence), snapshot.actor_message_follow_relay);
                    return snapshot;
                })
                .get ();
          };
        auto dispatch_snapshot = project_actor_mesh_dispatch_state (std::nullopt);
        auto actor_type = dispatch_snapshot.actor_type;
        if (actor_type.empty ()) {
            const auto located = actor_type_from_authority (
              services.get_required<runtime::live_location_reader_t> (), actor_id);
            if (located && !located->empty ()) {
                dispatch_snapshot = project_actor_mesh_dispatch_state (
                  std::make_optional<std::string_view> (*located));
                actor_type = dispatch_snapshot.actor_type;
            }
        }
        runtime::messaging::message_parts_t encoded (std::move (parts));
        runtime::messaging::envelope_codec_t codec;
        const auto &header = *decoded_header;
        const auto flow_mode = detail::message_flow_tracer_t (_state->dispatch).mode ();
        std::optional<runtime::flow_context_t::scope_t> flow_scope;
        if (runtime::flow_context_t::tracing (flow_mode))
            flow_scope.emplace (runtime::flow_context_t::enter (
              header.flow_id, header.flow_origin, flow_mode, flow_origin_t::inbound,
              record.source_session_rid
                ? std::optional<std::string> (record.source_session_rid->to_hex ())
                : std::nullopt));
        detail::channel_reply_writer_t replies;
        auto reply_error = [&] (const framework_exception_t &error) {
            if (record.kind != service::record_kind_t::actor_request)
                return;
            auto reply = detail::encode_actor_reply (
              replies.create_error_header (header.channel_name, header, error),
              zlink::message_t::from (""));
            (void) service::reply (record.reply_token, reply.items ());
        };
        if (actor_type.empty ()) {
            reply_error (framework_exception_t (framework_error_kind_t::not_found,
                                                "Actor type is not registered"));
            return true;
        }
        const actor_ref_t actor = ::zlink::framework::detail::actor_ref_access_t::make (
          node_rid_t::from_string (std::string (owner.actor->node_rid ().value ())), actor_type,
          std::string (owner.actor->actor_id ().value ()), owner.actor->object_generation ());
        const auto actor_dispatch_kind = record.kind == service::record_kind_t::actor_send
                                           ? dispatch_message_kind_t::actor_send
                                           : dispatch_message_kind_t::actor_request;
        auto &actor_gateway = services.get_required<actor_gateway_runtime_t> ();
        if (has_bound_session_source
            && (!record.source_session_rid || record.source_binding_generation == 0
                || record.source_session_sequence == 0)) {
            reply_error (framework_exception_t (framework_error_kind_t::protocol_error,
                                                "Bound Session relay source fence is incomplete"));
            return true;
        }
        std::optional<result_t<actor_context_t>> session_relay_admission;
        bool targets_current_authority = false;
        bool admitted_message_follow_route = false;
        auto native_node = std::move (dispatch_snapshot.native_node);
        auto relocation_authority = std::move (dispatch_snapshot.relocation_authority);
        auto actor_route_admission = std::move (dispatch_snapshot.actor_route_admission);
        const auto authority_fence = std::move (dispatch_snapshot.authority_fence);
        const auto relay_parking_node_rid = std::move (dispatch_snapshot.relay_parking_node_rid);
        auto admission_token = std::move (dispatch_snapshot.admission_token);
        if (record.actor_route) {
            const auto &route = *record.actor_route;
            const auto local =
              native_node ? native_node->status () : runtime::host::node_status_t{};
            const bool targets_local_actor =
              native_node && route.actor_id == actor.actor_id ().value ()
              && route.target_node_routing_id == local.routing_id ().to_bytes ()
              && route.target_node_generation == local.lifecycle_generation ();
            const bool targets_exact_incarnation =
              targets_local_actor && route.object_generation == actor.object_generation ();
            const auto coordinator_snapshot =
              targets_exact_incarnation
                ? _state->actor_transfer_coordinator->project_dispatch_state (
                    dispatch_snapshot.actor_key, &route)
                : detail::actor_transfer_dispatch_state_snapshot_t{};
            const bool follows_committed_source =
              targets_exact_incarnation && coordinator_snapshot.matches_message_follow_source;
            const bool follows_in_flight_source =
              targets_exact_incarnation && coordinator_snapshot.transfer_in_progress;
            admitted_message_follow_route = follows_committed_source || follows_in_flight_source;
            const bool requires_exact_incarnation =
              header.message_name == actor_bound_session_bind_route_request_t::packet_name
              || header.message_name == actor_bound_session_route_request_t::packet_name;
            if (has_bound_session_source && targets_exact_incarnation) {
                session_relay_admission.emplace (actor_gateway.admit_session_relay_context (
                  actor, record.source_node_rid, *record.source_session_rid,
                  record.source_binding_generation, record.source_session_sequence, &route));
            }
            // Binding admission owns its terminal error. Its result on the exact
            // Actor route must reach the session terminal path below instead of
            // being replaced with a generic route rejection.
            const bool targets_current_bound_route = session_relay_admission.has_value ();
            if (!has_bound_session_source) {
                targets_current_authority =
                  authority_fence && authority_fence->actor_id == route.actor_id
                  && authority_fence->target_node_routing_id == route.target_node_routing_id
                  && authority_fence->target_node_generation == route.target_node_generation
                  && authority_fence->authority_owner_generation == route.authority_owner_generation
                  && authority_fence->owner_lease_generation == route.owner_lease_generation;
                if (!targets_current_authority && relocation_authority) {
                    try {
                        const auto committed = relocation_authority->read (
                          runtime::stateful::object_kind_t::actor, route.actor_id);
                        targets_current_authority =
                          committed
                          && committed->target.kind == runtime::stateful::object_kind_t::actor
                          && committed->target.key == route.actor_id
                          && committed->target.node_id
                               == zlink::routing_id_t::from (route.target_node_routing_id)
                                    .to_string ()
                          && committed->target.authority_owner_generation
                               == route.authority_owner_generation
                          && committed->target_owner.lease_generation > 0
                          && static_cast<std::uint64_t> (committed->target_owner.lease_generation)
                               == route.owner_lease_generation;
                    }
                    catch (...) {
                        targets_current_authority = false;
                    }
                }
                if (!targets_current_authority && actor_route_admission)
                    targets_current_authority = actor_route_admission (route);
            }
            const bool admitted = targets_local_actor
                                  && (!requires_exact_incarnation || targets_exact_incarnation)
                                  && (follows_committed_source || targets_current_authority
                                      || targets_current_bound_route
                                      || (follows_in_flight_source && !requires_exact_incarnation));
            if (_state
                && detail::message_flow_tracer_t (_state->dispatch)
                     .enabled (message_flow_log_mode_t::detailed)) {
                report_actor_dispatch_stage_trace_lazy (
                  _state, message_flow_outcome_t::received, actor_dispatch_kind,
                  header.message_name, owner.spot_id, actor.actor_id ().value (),
                  "dispatch_mesh_record.route_admission", [&] {
                      return std::string ("accepted=") + (admitted ? "true" : "false")
                             + " local=" + (targets_local_actor ? "true" : "false")
                             + " incarnation=" + (targets_exact_incarnation ? "true" : "false")
                             + " committed_source=" + (follows_committed_source ? "true" : "false")
                             + " current_authority="
                             + (targets_current_authority ? "true" : "false")
                             + " current_bound_route="
                             + (targets_current_bound_route ? "true" : "false")
                             + " in_flight_source=" + (follows_in_flight_source ? "true" : "false");
                  });
            }
            if (!admitted) {
                const bool exact_generation_mismatch =
                  requires_exact_incarnation && targets_local_actor && !targets_exact_incarnation;
                reply_error (framework_exception_t (
                  exact_generation_mismatch ? framework_error_kind_t::invalid_operation
                                            : framework_error_kind_t::unavailable,
                  exact_generation_mismatch
                    ? "Bound session route Actor generation does not match"
                    : "Actor route owner identity is stale or not admitted"));
                return true;
            }
        } else {
            report_actor_dispatch_stage_trace (
              _state, message_flow_outcome_t::received, actor_dispatch_kind, header.message_name,
              owner.spot_id, actor.actor_id ().value (), "dispatch_mesh_record.route_admission",
              "required=false");
        }
        if (has_bound_session_source) {
            if (!session_relay_admission) {
                session_relay_admission.emplace (actor_gateway.admit_session_relay_context (
                  actor, record.source_node_rid, *record.source_session_rid,
                  record.source_binding_generation, record.source_session_sequence));
            }
            if (_state
                && detail::message_flow_tracer_t (_state->dispatch)
                     .enabled (message_flow_log_mode_t::detailed)) {
                report_actor_dispatch_stage_trace_lazy (
                  _state, message_flow_outcome_t::received, actor_dispatch_kind,
                  header.message_name, owner.spot_id, actor.actor_id ().value (),
                  "dispatch_mesh_record.session_admission", [&] {
                      if (*session_relay_admission)
                          return std::string ("accepted=true");
                      return std::string ("accepted=false reason=")
                             + (session_relay_admission->error ()
                                  ? session_relay_admission->error ()->what ()
                                  : "Bound Session relay source fence was rejected");
                  });
            }
            if (!*session_relay_admission) {
                const auto error =
                  framework_exception_t (session_relay_admission->error_kind (),
                                         session_relay_admission->error ()
                                           ? session_relay_admission->error ()->what ()
                                           : "Bound Session relay source fence was rejected");
                if (record.kind == service::record_kind_t::actor_request) {
                    detail::dispatch_error_reporter_t (_state->dispatch).report_lazy ([&] {
                        return message_dispatch_error_event_t{
                          .surface = dispatch_error_surface_t::stream_session,
                          .message_kind = dispatch_message_kind_t::request,
                          .reason = dispatch_error_reason_t::stale_target,
                          .action = dispatch_error_action_t::reply_error,
                          .packet_name = header.message_name,
                          .actor_id = std::string (actor.actor_id ().value ()),
                          .source_rid = record.source_node_rid.to_hex (),
                          .correlation_id = header.correlation_id,
                          .exception = std::make_exception_ptr (error)};
                    });
                } else {
                    detail::message_flow_tracer_t (_state->dispatch)
                      .trace (message_flow_outcome_t::dropped, [&] {
                          auto event = message_flow_event_t{
                            message_flow_outcome_t::dropped,
                            dispatch_error_surface_t::stream_session, dispatch_message_kind_t::send,
                            header.message_name};
                          event.actor_id = std::string (actor.actor_id ().value ());
                          event.source_rid = record.source_node_rid.to_hex ();
                          event.stream_session_id = record.source_session_rid->to_hex ();
                          event.correlation_id = header.correlation_id;
                          event.reason = message_flow_reason_t::stale_target;
                          event.exception = std::make_exception_ptr (error);
                          return event;
                      });
                }
                reply_error (error);
                return true;
            }
        } else {
            report_actor_dispatch_stage_trace (
              _state, message_flow_outcome_t::received, actor_dispatch_kind, header.message_name,
              owner.spot_id, actor.actor_id ().value (), "dispatch_mesh_record.session_admission",
              "required=false");
        }
        /* Route admission is complete before creating the typed body. This
         * keeps stale authority, owner, and node generations out of the
         * application deserializer and handler path. */
        auto body = codec.decode_body (encoded);
        if (!body) {
            reply_error (framework_exception_t (framework_error_kind_t::protocol_error,
                                                "Actor request envelope body is invalid"));
            return true;
        }
        if (header.message_name == actor_bound_session_bind_route_request_t::packet_name) {
            const auto route_client =
              std::atomic_load_explicit (&_state->route_client, std::memory_order_acquire);
            if (!route_client) {
                reply_error (framework_exception_t (
                  framework_error_kind_t::unavailable,
                  "Remote Actor session binding requires a RouteMesh client"));
                return true;
            }
            spot_route_internal_dispatcher_t dispatcher (*this, actor_gateway, *route_client,
                                                         serializers);
            route_received_packet_t received{record.source_node_rid,
                                             record.operation_id.low == 0
                                               ? std::nullopt
                                               : std::make_optional (record.operation_id.low),
                                             std::move (encoded), std::nullopt, record.received_at};
            auto response = dispatcher.dispatch_request (received, header, services);
            const auto reply_parts =
              response ? detail::encode_actor_reply (replies.create_reply_header (
                                                       runtime::messaging::message_kind_t::response,
                                                       header.channel_name, header),
                                                     std::move (response.value ()))
                       : detail::encode_actor_reply (
                           replies.create_error_header (
                             header.channel_name, header,
                             framework_exception_t (response.error_kind (),
                                                    response.error ()
                                                      ? response.error ()->what ()
                                                      : "Remote Actor session binding failed")),
                           zlink::message_t::from (""));
            (void) service::reply (record.reply_token, reply_parts.items ());
            return true;
        }
        auto actor_context =
          session_relay_admission
            ? std::move (session_relay_admission->value ())
            : actor_gateway.actor_context (actor, record.source_binding_generation);
        const auto handoff_operation =
          runtime::protocol::wire_operation_id_t{record.operation_id.high, record.operation_id.low};
        const auto handoff_source_fence =
          record.actor_route.value_or (runtime::protocol::actor_route_fence_t{});
        const auto handoff_pending =
          handoff_pending_key (record.source_node_rid, handoff_operation, handoff_source_fence);
        bool deferred_handoff_request = false;
        /* Native accepted records carry their original reply terminal. For
         * records without that owner, a request operation travels with a handoff
         * terminal route naming THIS node as the parking node (see the relay
         * metadata below), so its reply may return as a handoff terminal
         * whenever any downstream hop parks it — during the local transfer,
         * or on the follow target while its join is still completing. The
         * pending entry that restores the original reply token must exist
         * under exactly that stamping condition: gating it on the local
         * transfer state orphaned the terminal of a request that this node
         * relayed immediately but the target parked (the reply was replaced
         * by an empty one). An entry whose reply arrives inline instead is
         * erased by the relay observer below; an untouched entry expires at
         * its 30s deadline. A request without an attached route fence (e.g.
         * a local requester on the current owner) parks and replays exactly
         * like a fenced one; requiring the fence here silently orphaned its
         * terminal, leaving the requester waiting forever. */
        if (record.kind == service::record_kind_t::actor_request
            && (!record.application_record || !record.application_record->native_reply)
            && (record.operation_id.high != 0 || record.operation_id.low != 0)
            && !header.metadata.contains (std::string (detail::actor_handoff_source_node_key))) {
            const auto now = std::chrono::steady_clock::now ();
            auto pending_request = spot_node_builder_state_t::pending_handoff_request_t{
              actor,
              handoff_source_fence,
              record.reply_route_id,
              record.reply_token,
              header,
              now + spot_node_builder_state_t::pending_handoff_request_t::default_timeout};
            if (admission_token) {
                admission_token->reserve_handoff_reply (handoff_pending,
                                                        std::move (pending_request));
            } else {
                _state->lane
                  .run ([&] {
                      if (_state->pending_handoff_requests.size ()
                          < detail::max_pending_handoff_requests) {
                          const auto [_, inserted] = _state->pending_handoff_requests.emplace (
                            handoff_pending, std::move (pending_request));
                          _state->management_next_due.store (
                            spot_node_builder_state_t::management_due_now,
                            std::memory_order_release);
                          deferred_handoff_request = inserted;
                      }
                  })
                  .get ();
            }
        }
        const auto request_header = header;
        auto terminal_claimed = std::make_shared<std::atomic_bool> (false);
        auto terminal_owner = std::make_shared<std::function<void ()>> (defer_terminal ());
        std::function<void ()> after_application_admission;
        if (record.kind == service::record_kind_t::actor_send && *terminal_owner) {
            // Spec 05 section 4.3 defines one-way acceptance at the target
            // mailbox/relay queue. Do not retain the source Session ingress
            // until application-handler completion: relocation sealing may be
            // queued behind that same handler.
            after_application_admission = [terminal_claimed, terminal_owner] {
                if (!terminal_claimed->exchange (true, std::memory_order_acq_rel))
                    (*terminal_owner) ();
            };
        }
        auto relayed = [&] {
            auto relay_metadata = spot_inbound_message_t{.content_type = header.content_type,
                                                         .values = header.metadata.values (),
                                                         .received_at = record.received_at};
            if (record.kind == service::record_kind_t::actor_request
                && !header.correlation_id.empty ()) {
                // Application correlation remains distinct from the service
                // OperationId used for terminal deduplication.
                relay_metadata.values.insert_or_assign (detail::actor_request_id_key,
                                                        header.correlation_id);
            }
            if (record.kind == service::record_kind_t::actor_request
                && (record.operation_id.high != 0 || record.operation_id.low != 0)) {
                relay_metadata.values[std::string (detail::actor_handoff_source_node_key)] =
                  record.source_node_rid.to_hex ();
                relay_metadata.values[std::string (detail::actor_handoff_parking_node_key)] =
                  zlink::routing_id_t::from (relay_parking_node_rid).to_hex ();
                relay_metadata.values[std::string (detail::actor_handoff_operation_high_key)] =
                  std::to_string (record.operation_id.high);
                relay_metadata.values[std::string (detail::actor_handoff_operation_low_key)] =
                  std::to_string (record.operation_id.low);
                relay_metadata.values[std::string (detail::actor_handoff_reply_route_key)] =
                  std::to_string (record.reply_route_id);
            }
            if (record.actor_route) {
                stamp_handoff_actor_route (relay_metadata.values, *record.actor_route,
                                           record.message_follow_hop_count);
            }
            return relay_actor_packet (
              actor, std::move (actor_context),
              record.kind == service::record_kind_t::actor_send ? stream_message_kind_t::send
                                                                : stream_message_kind_t::request,
              header.message_name, body.value (), services, serializers, std::move (relay_metadata),
              admitted_message_follow_route && record.actor_route ? &*record.actor_route : nullptr,
              std::move (before_application_handler), std::move (after_application_admission),
              record.source_node_rid, handoff_operation, record.reply_route_id, header.deadline,
              record.release_mailbox_reservation, record.transferred_owner_byte_cost,
              admission_token, record.application_record,
              application_completed ? spot_handler_registry_t::actor_queue_dispatch_t::current_turn
                                    : spot_handler_registry_t::actor_queue_dispatch_t::acquire);
        }();
        if (!*terminal_owner && record.kind != service::record_kind_t::actor_request) {
            parts = std::move (encoded).take_items ();
            return false;
        }
        detail::observe_task_completion (
          relayed, [state = _state, admission_token, deferred_handoff_request, handoff_pending,
                    request_header, record_kind = record.kind, reply_token = record.reply_token,
                    terminal_claimed, terminal_owner, application_completed,
                    application_record = record.application_record] (
                     const result_t<std::optional<zlink::message_t>> &result) mutable {
              /* An empty relay result retains the existing native reply owner
               * or the legacy pending entry while handoff replay owns the request.
               * For the legacy path, the reply token stays
               * in pending_handoff_requests and the handoff terminal from the
               * follow target restores the reply. Replying (or erasing the
               * entry) here would orphan that terminal. Any other outcome —
               * an immediate relayed reply or a relay failure — is terminal
               * right now, so the entry is settled and the requester answered
               * directly; without this, a cross-node request relayed through
               * the committed Message Follow source between the relocation
               * commit and the source cleanup lost its reply forever (the
               * requester's wire operation simply timed out). */
              const auto finish_application = [application_completed, result] {
                  if (application_completed)
                      application_completed ([result] { result.value (); });
              };
              const bool handoff_reserved =
                deferred_handoff_request
                || (admission_token && admission_token->handoff_reserved ());
              const bool parked =
                (handoff_reserved || (application_record && application_record->native_reply))
                && result && !result.value ();
              if (!parked && handoff_reserved) {
                  try {
                      state->lane
                        .run ([&] { state->pending_handoff_requests.erase (handoff_pending); })
                        .get ();
                  }
                  catch (...) {
                  }
              }
              if (terminal_claimed->exchange (true, std::memory_order_acq_rel)) {
                  finish_application ();
                  return;
              }
              if (!parked && record_kind == service::record_kind_t::actor_request) {
                  try {
                      detail::channel_reply_writer_t writer;
                      const auto reply =
                        result
                          ? detail::encode_actor_reply (
                              writer.create_reply_header (
                                runtime::messaging::message_kind_t::response,
                                request_header.channel_name, request_header),
                              result.value () ? *result.value () : zlink::message_t{})
                          : detail::encode_actor_reply (
                              writer.create_error_header (
                                request_header.channel_name, request_header,
                                framework_exception_t (result.error_kind (),
                                                       result.error () ? result.error ()->what ()
                                                                       : "Actor handler failed")),
                              zlink::message_t{});
                      (void) service::reply (reply_token, reply.items ());
                  }
                  catch (...) {
                  }
              }
              if (*terminal_owner)
                  (*terminal_owner) ();
              finish_application ();
          });
        if (terminal_deferred)
            *terminal_deferred = true;
        return true;
    }
    if (owner.owner_kind != service::owner_kind_t::spot
        || record.kind != service::record_kind_t::spot_control || !record.actor_control) {
        return false;
    }
    const auto &control = *record.actor_control;
    if (control.kind != service::lifecycle_kind_t::joined) {
        return true;
    }
    if (record.operation_kind != service::operation_kind_t::actor_join) {
        // The remote source emits a post-commit JOINED lifecycle notification
        // after the target has already admitted and materialized the actor.
        // It has no admission payload or reply token and must not re-enter the
        // admission handler.
        return true;
    }

    struct actor_join_dispatch_state_snapshot_t
    {
        std::string actor_type;
        bool targets_entry_spot = false;
        std::string local_node_rid;
    };
    auto join_dispatch_snapshot =
      _state->lane
        .run ([&] {
            actor_join_dispatch_state_snapshot_t result;
            const auto found = _state->actor_types_by_id.find (
              std::string (control.current_actor.actor_id ().value ()));
            if (found != _state->actor_types_by_id.end ())
                result.actor_type = found->second;
            if (_state->snapshot.entry_spot_name) {
                const auto entry =
                  _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
                result.targets_entry_spot =
                  entry != _state->spot_ids_by_name.end () && entry->second == owner.spot_id;
            }
            result.local_node_rid = detail::effective_spot_node_rid (_state->snapshot);
            return result;
        })
        .get ();
    auto &actor_type = join_dispatch_snapshot.actor_type;
    try {
        if (actor_type.empty ()) {
            const auto actor_id = std::string (control.current_actor.actor_id ().value ());
            const auto located = actor_type_from_authority (
              services.get_required<runtime::live_location_reader_t> (), actor_id);
            if (located && !located->empty ()) {
                actor_type = *located;
                _state->lane.run ([&] { _state->actor_types_by_id[actor_id] = actor_type; }).get ();
            }
        }
    }
    catch (...) {
        (void) service::actor_join_reply (
          record.reply_token,
          detail::result_access_t::failure<service::actor_join_result_t> (
            std::current_exception ()),
          {});
        return true;
    }
    if (actor_type.empty ()) {
        (void) service::actor_join_reply (
          record.reply_token,
          result_t<service::actor_join_result_t>::failure (
            framework_error_kind_t::not_found, "Actor Join target Actor type was not found"),
          {});
        return true;
    }

    const actor_ref_t actor = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (std::string (control.current_actor.node_rid ().value ())),
      actor_type, std::string (control.current_actor.actor_id ().value ()),
      control.current_actor.object_generation ());
    /* The Join runs as the target Spot's lifecycle item and owns the reply and
     * the record terminal from here (Spot-Actor membership: Join on the
     * lifecycle lane). The Spot lifecycle drain waits for it at shutdown. */
    auto reply = std::make_shared<std::function<void (result_t<actor_join_reply_t>)>> (
      [reply_token = record.reply_token,
       deferred_terminal = defer_terminal ()] (result_t<actor_join_reply_t> joined) {
          if (!joined) {
              (void) service::actor_join_reply (
                reply_token,
                detail::propagate_failure<service::actor_join_result_t> (
                  joined, "target Actor Join failed"),
                {});
          } else {
              const std::vector<zlink::message_t> reply_parts =
                joined.value ().reply.to_string ().empty ()
                  ? std::vector<zlink::message_t>{}
                  : std::vector<zlink::message_t>{joined.value ().reply};
              (void) service::actor_join_reply (reply_token,
                                                result_t<service::actor_join_result_t>::success (
                                                  joined.value ().result_code == 0
                                                    ? service::actor_join_result_t::accepted
                                                    : service::actor_join_result_t::rejected),
                                                reply_parts);
          }
          if (deferred_terminal)
              deferred_terminal ();
      });
    const auto settle_once = [reply] (result_t<actor_join_reply_t> joined) {
        if (auto owned = std::exchange (*reply, {}))
            owned (std::move (joined));
    };
    struct join_result_t
    {
        std::optional<result_t<actor_join_reply_t>> joined;
        std::function<void (std::function<void (result_t<void>)>)> source_leave;
    };
    auto join_result = std::make_shared<join_result_t> ();
    const bool remote_transfer =
      parts.size () >= 10 && owner.spot_id != join_dispatch_snapshot.local_node_rid;
    dispatch_wire_actor_join_admission (
      spot_id_t (owner.spot_id), remote_transfer ? actor.actor_id ().value () : std::string_view{},
      [runtime = spot_node_runtime_t (_state), join_result, actor, control,
       join_spot_id = owner.spot_id, parts, remote_transfer,
       request = parts.empty () ? zlink::message_t{} : parts.front (),
       targets_entry_spot = join_dispatch_snapshot.targets_entry_spot,
       local_node_rid = join_dispatch_snapshot.local_node_rid, services = &services,
       serializers = &serializers,
       deadline = record.operation_deadline] () mutable -> task_t<void> {
          return run_actor_join_admission_task (
            std::move (runtime), std::move (join_result), std::move (actor), std::move (control),
            std::move (join_spot_id), std::move (parts), remote_transfer, std::move (request),
            targets_entry_spot, std::move (local_node_rid), services, serializers, deadline);
      },
      [join_result, settle_once] (result_t<void> rejected) {
          if (!join_result->joined)
              settle_once (detail::propagate_failure<actor_join_reply_t> (
                rejected, "Actor Join target admission failed"));
      },
      [join_result, settle_once] {
          if (!join_result->joined)
              return;
          auto joined = std::move (*join_result->joined);
          if (join_result->source_leave)
              join_result->source_leave ({});
          settle_once (std::move (joined));
      });
    if (terminal_deferred)
        *terminal_deferred = true;
    return true;
}

result_t<std::optional<task_t<zlink::message_t>>>
spot_node_runtime_t::dispatch_subscription (const spot_context_t &context,
                                            std::string topic,
                                            const std::vector<zlink::message_t> &parts,
                                            service_provider_t &services,
                                            serializer_registry_t &serializers,
                                            runtime::handler_entry_t before_application_handler,
                                            std::function<void ()> transfer_owner_reservation,
                                            std::size_t transferred_owner_byte_cost,
                                            const service::receive_record_t *multicast) const
{
    using submission_t = std::optional<task_t<zlink::message_t>>;
    if (!context._state || !context._state->spot_instance) {
        return result_t<submission_t>::failure (framework_error_kind_t::not_found,
                                                "spot context is not registered");
    }
    if (parts.empty ()) {
        return result_t<submission_t>::failure (framework_error_kind_t::protocol_error,
                                                "spot subscription frame is empty");
    }
    /* Fan-out wire envelope (flow-correlation §4.1): the decoded header
     * carries the publisher's flow pair, so every subscriber line — including
     * skip/drop lines — shares the tree's flow id. The frame is either the
     * framework's self-delimited single part (['Z''L''F''E'][u32 BE
     * header_len][header JSON][body]), a true two-part envelope from a
     * parts-preserving wire, or a bare payload from a non-framework publisher
     * (dispatched without a flow pair). */
    const runtime::messaging::envelope_codec_t codec;
    /* flow-correlation §4: at Off the publisher's flow pair is neither
     * validated nor materialized from the fan-out envelope. */
    const bool capture_flow = detail::message_flow_tracer_t (_state->dispatch).flow_tracing ();
    zlink::message_t body = parts.front ();
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;
    std::optional<std::string> packet_name;
    std::string channel_name;
    std::string content_type = runtime::messaging::envelope_codec_t::default_content_type;
    bool report_decode_failure = false;
    if (parts.size () >= 2) {
        const runtime::messaging::message_parts_t envelope_parts{std::vector (parts)};
        auto header = codec.decode_header (envelope_parts, capture_flow);
        auto decoded_body = codec.decode_body (envelope_parts);
        if (header && decoded_body) {
            body = decoded_body.value ();
            flow_id = header.value ().flow_id;
            flow_origin = header.value ().flow_origin;
            content_type = header.value ().content_type;
            channel_name = std::move (header.value ().channel_name);
            if (!header.value ().message_name.empty ()) {
                packet_name = header.value ().message_name;
            }
        } else {
            report_decode_failure = true;
        }
    } else {
        const auto &frame = parts.front ();
        const auto bytes = frame.to_bytes ();
        const bool framed = bytes.size () >= 8 && bytes[0] == static_cast<std::uint8_t> ('Z')
                            && bytes[1] == static_cast<std::uint8_t> ('L')
                            && bytes[2] == static_cast<std::uint8_t> ('F')
                            && bytes[3] == static_cast<std::uint8_t> ('E');
        if (framed) {
            /* The 'ZLFE' prefix is reserved by the fanout wire contract for
             * framework frames — a raw publisher whose payload begins with
             * it is out of contract (CPP-FANOUT-WIRE-001 tracks the final
             * cross-language wire). On a magic match any failure past this
             * point drops the message instead of handing a corrupted body
             * to the application handler. header_size is compared against
             * the remainder (never added to the prefix width) so an
             * adversarial length cannot wrap std::size_t. */
            const std::size_t header_size = (static_cast<std::size_t> (bytes[4]) << 24)
                                            | (static_cast<std::size_t> (bytes[5]) << 16)
                                            | (static_cast<std::size_t> (bytes[6]) << 8)
                                            | static_cast<std::size_t> (bytes[7]);
            report_decode_failure = true;
            if (header_size > 0 && header_size <= bytes.size () - 8) {
                auto header = codec.decode_header (
                  zlink::message_t::from (std::vector<std::uint8_t> (
                    bytes.begin () + 8,
                    bytes.begin () + 8 + static_cast<std::ptrdiff_t> (header_size))),
                  capture_flow);
                if (header) {
                    body = zlink::message_t::from (std::vector<std::uint8_t> (
                      bytes.begin () + 8 + static_cast<std::ptrdiff_t> (header_size),
                      bytes.end ()));
                    flow_id = header.value ().flow_id;
                    flow_origin = header.value ().flow_origin;
                    content_type = header.value ().content_type;
                    channel_name = std::move (header.value ().channel_name);
                    if (!header.value ().message_name.empty ()) {
                        packet_name = header.value ().message_name;
                    }
                    report_decode_failure = false;
                }
            }
        }
    }
    if (report_decode_failure) {
        return result_t<submission_t>::success (std::nullopt);
    }
    if (multicast)
        channel_name = multicast->channel_name;
    const auto diagnostics_mode = detail::message_flow_tracer_t (_state->dispatch).mode ();
    auto flow_scope = runtime::flow_context_t::enter (
      std::move (flow_id), flow_origin, diagnostics_mode, flow_origin_t::inbound, std::nullopt);
    const auto &message = body;
    const auto registry = spot_handler_registry_t (context._state);
    const auto handler_found =
      packet_name
      && registry.find_handler (spot_handler_kind_t::subscription, *packet_name, topic,
                                std::type_index (typeid (void)), channel_name);
    if (!handler_found) {
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_route, dispatch_message_kind_t::send,
          dispatch_error_reason_t::handler_missing, dispatch_error_action_t::drop, packet_name,
          topic, std::string (context._state->spot_id));
        return result_t<submission_t>::success (std::nullopt);
    }
    spot_inbound_message_t inbound{.received_at = multicast != nullptr
                                                    ? multicast->received_at
                                                    : std::chrono::system_clock::now ()};
    inbound.content_type = std::move (content_type);
    inbound.channel_name = std::move (channel_name);
    if (multicast && multicast->metadata
        && !detail::mesh_metadata_codec_t::decode (*multicast->metadata, inbound.values))
        return result_t<submission_t>::failure (framework_error_kind_t::protocol_error,
                                                "logical multicast metadata is invalid");
    return result_t<submission_t>::success (registry.invoke_erased (
      std::move (before_application_handler), spot_handler_kind_t::subscription, *packet_name,
      topic, std::type_index (typeid (void)), context._state->spot_instance.get (), nullptr,
      services, serializers, message, std::move (inbound), true, {}, {}, {},
      spot_handler_registry_t::actor_queue_dispatch_t::acquire, {},
      std::move (transfer_owner_reservation), transferred_owner_byte_cost));
}

} // namespace detail

std::optional<actor_ref_t> spot_manager_t::current_actor_ref (const actor_ref_t &actor_ref) const
{
    return detail::spot_node_runtime_t (_state).current_actor_ref (actor_ref);
}

task_t<std::optional<zlink::message_t>>
spot_manager_t::relay_actor_packet (const actor_ref_t &actor_ref,
                                    actor_context_t actor_context,
                                    std::string_view packet_name,
                                    const zlink::message_t &message,
                                    service_provider_t &services,
                                    serializer_registry_t &serializers,
                                    spot_inbound_message_t metadata)
{
    co_return co_await relay_actor_packet (actor_ref, std::move (actor_context),
                                           detail::stream_message_kind_t::request, packet_name,
                                           message, services, serializers, std::move (metadata));
}

task_t<std::optional<zlink::message_t>> spot_manager_t::relay_actor_packet (
  const actor_ref_t &actor_ref,
  actor_context_t actor_context,
  detail::stream_message_kind_t message_kind,
  std::string_view packet_name,
  const zlink::message_t &message,
  service_provider_t &services,
  serializer_registry_t &serializers,
  spot_inbound_message_t metadata,
  const runtime::protocol::actor_route_fence_t *admitted_message_follow_target)
{
    const auto actor_packet_relay =
      _state->lane.run_sync ([&] { return _state->actor_packet_relay; });
    if (actor_packet_relay) {
        co_return co_await actor_packet_relay (
          actor_ref, std::move (actor_context), message_kind, packet_name, message, services,
          serializers, std::move (metadata), admitted_message_follow_target);
    }
    // `relay_actor_packet` is a coroutine.  Do not invoke that member on a
    // temporary: after its first suspension the coroutine frame retains the
    // receiver (`this`), not the temporary object.  In particular, a Join can
    // relocate the Actor before the handler task completes, so its completion
    // resumes this call after the full handler turn has unwound.  Keep the
    // runtime wrapper in this coroutine frame until that resume is finished.
    auto runtime = detail::spot_node_runtime_t (_state);
    co_return co_await runtime.relay_actor_packet (
      actor_ref, std::move (actor_context), message_kind, packet_name, message, services,
      serializers, std::move (metadata), admitted_message_follow_target);
}

} // namespace zlink::framework
