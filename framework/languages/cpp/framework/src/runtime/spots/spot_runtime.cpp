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
using detail::callback_context_scope_t;
using detail::is_blank;
using detail::report_actor_dispatch_stage_trace;
using detail::report_actor_dispatch_stage_trace_lazy;
using detail::report_spot_dispatch_error;
using detail::resolve_target_spot_generation;

const char *metric_spot_kind (const detail::spot_context_state_t &context) noexcept
{
    return context.is_entry_spot () ? "entry" : context.is_instance_spot () ? "instance" : "user";
}

struct spot_handler_terminal_t : task_completion_source_t<zlink::message_t>
{
    std::function<void ()> release_admission;
    runtime::handler_entry_t before_application_handler;
    std::optional<std::pair<zlink::message_t, spot_inbound_message_t>> carrier;
};

void settle_handler_admission_once (
  const std::shared_ptr<spot_handler_terminal_t> &terminal) noexcept
{
    if (!terminal)
        return;
    terminal->before_application_handler = {};
    auto owned = std::move (terminal->release_admission);
    if (!owned)
        return;
    try {
        owned ();
    }
    catch (...) {
    }
}

std::string spot_mesh_channel_name (const std::shared_ptr<detail::spot_context_state_t> &state)
{
    if (state && !state->spot_name.empty ()) {
        return state->spot_name;
    }
    return "spot-mesh";
}

std::optional<std::string>
optional_spot_route_channel_name (const std::shared_ptr<detail::spot_context_state_t> &state)
{
    if (!state || !state->node || !state->channel_runtime) {
        return std::nullopt;
    }
    struct route_channel_projection_t
    {
        std::shared_ptr<detail::channel_runtime_state_t> channel_runtime;
        std::optional<std::string> configured;
        std::optional<std::string> accepted;
    };
    const auto projection =
      state->node->lane
        .run ([&] {
            route_channel_projection_t result{.channel_runtime = state->channel_runtime,
                                              .configured =
                                                state->node->snapshot.spot_route_channel_name};
            if (state->node->snapshot.accepted_route_channels.size () == 1) {
                result.accepted =
                  state->node->snapshot.accepted_route_channels.front ().channel_name;
            }
            return result;
        })
        .get ();
    if (!projection.channel_runtime)
        return std::nullopt;
    return projection.channel_runtime->lane
      .run ([&] () -> std::optional<std::string> {
          if (projection.configured) {
              if (projection.channel_runtime->route_channels.find (*projection.configured)
                  != projection.channel_runtime->route_channels.end ()) {
                  return projection.configured;
              }
          }
          if (projection.channel_runtime->route_channels.size () == 1) {
              return projection.channel_runtime->route_channels.begin ()->first;
          }
          if (projection.accepted) {
              if (projection.channel_runtime->route_channels.find (*projection.accepted)
                  != projection.channel_runtime->route_channels.end ()) {
                  return projection.accepted;
              }
          }
          return std::nullopt;
      })
      .get ();
}

runtime::messaging::message_parts_t
encode_spot_route_parts (runtime::messaging::message_kind_t kind,
                         const std::string &route_channel_name,
                         const std::string &packet_name,
                         zlink::message_t payload,
                         std::chrono::milliseconds timeout,
                         std::map<std::string, std::string> metadata)
{
    runtime::messaging::client_call_codec_t codec;
    auto header = codec.create_envelope (kind, route_channel_name, packet_name, timeout);
    header.metadata = std::move (metadata);
    runtime::messaging::envelope_codec_t envelope;
    return envelope.encode_raw_body_parts (header, std::move (payload));
}

std::exception_ptr
spot_request_terminal_exception (runtime::foundation::operation_terminal_t terminal)
{
    switch (terminal) {
        case runtime::foundation::operation_terminal_t::timed_out:
            return std::make_exception_ptr (detail::make_boundary_exception (
              detail::boundary_error_t::timed_out, "SPOT mesh request timed out"));
        case runtime::foundation::operation_terminal_t::cancelled:
            return detail::make_cancellation_exception ("SPOT mesh request was cancelled");
        case runtime::foundation::operation_terminal_t::transport_failed:
        case runtime::foundation::operation_terminal_t::route_unavailable:
            return std::make_exception_ptr (detail::make_boundary_exception (
              detail::boundary_error_t::disconnected, "SPOT mesh request lost its connection"));
        case runtime::foundation::operation_terminal_t::shutdown:
            return std::make_exception_ptr (detail::make_boundary_exception (
              detail::boundary_error_t::shutdown,
              "SPOT mesh request stopped because the runtime is shutting down"));
        case runtime::foundation::operation_terminal_t::completed:
            break;
    }
    return std::make_exception_ptr (
      framework_exception_t (framework_error_kind_t::internal_failure,
                             "SPOT mesh request completed without a terminal result"));
}

request_task_t<runtime::messaging::message_parts_t>
request_spot_parts_async (service::spot_handle_t egress,
                          const zlink::routing_id_t &target_node_rid,
                          const spot_id_t &target_spot_id,
                          std::uint64_t target_generation,
                          runtime::messaging::message_parts_t parts,
                          std::chrono::milliseconds timeout)
{
    auto source =
      std::make_shared<task_completion_source_t<runtime::messaging::message_parts_t>> ();
    auto output = source->task ();
    auto admitted = co_await detail::request_admission_source_t{};
    detail::observe_request_failure (output, admitted);
    try {
        const auto &native_parts = parts.items ();
        if (native_parts.empty ()) {
            source->complete (result_t<runtime::messaging::message_parts_t>::failure (
              framework_error_kind_t::protocol_error,
              "SPOT mesh request requires at least one message part"));
            co_return co_await output;
        }
        service::pending_operation_t operation_id;
        const auto submitted = co_await egress.request_to_spot (
          target_node_rid, target_spot_id, target_generation, native_parts, operation_id,
          zlink::send_flags_t::none, timeout, {},
          [source] (runtime::foundation::operation_terminal_t terminal,
                    result_t<std::vector<zlink::message_t>> decoded) mutable {
              if (terminal != runtime::foundation::operation_terminal_t::completed) {
                  source->complete (
                    detail::result_access_t::failure<runtime::messaging::message_parts_t> (
                      spot_request_terminal_exception (terminal)));
                  return;
              }
              if (!decoded) {
                  source->complete (detail::propagate_failure<runtime::messaging::message_parts_t> (
                    decoded, "SPOT mesh request reply decode failed"));
                  return;
              }
              source->complete (result_t<runtime::messaging::message_parts_t>::success (
                runtime::messaging::message_parts_t (std::move (decoded.value ()))));
          });
        if (submitted != zlink::submit_result_t::ok) {
            source->complete (
              detail::result_access_t::failure<runtime::messaging::message_parts_t> (
                runtime::messaging::map_submit_result_exception (
                  submitted, "SPOT mesh request was not submitted")));
            co_return co_await output;
        }
        co_await detail::request_admitted_t{};
    }
    catch (const framework_exception_t &error) {
        source->complete (
          detail::result_access_t::failure<runtime::messaging::message_parts_t> (error));
    }
    catch (const zlink::request_error_t &error) {
        source->complete (detail::result_access_t::failure<runtime::messaging::message_parts_t> (
          runtime::messaging::map_request_result_exception (error.result (), error.what ())));
    }
    catch (const zlink::submit_error_t &error) {
        source->complete (detail::result_access_t::failure<runtime::messaging::message_parts_t> (
          runtime::messaging::map_submit_result_exception (error.result (), error.what ())));
    }
    catch (const std::exception &error) {
        source->complete (
          detail::is_cancellation_exception (error)
            ? detail::result_access_t::failure<runtime::messaging::message_parts_t> (
                std::current_exception ())
            : result_t<runtime::messaging::message_parts_t>::failure (
                framework_error_kind_t::internal_failure, error.what ()));
    }
    co_return co_await output;
}

request_task_t<runtime::messaging::message_parts_t>
request_spot_mesh_parts (std::shared_ptr<detail::spot_context_state_t> state,
                         node_rid_t node_rid,
                         spot_id_t spot_id,
                         runtime::messaging::message_parts_t parts,
                         std::chrono::milliseconds timeout)
{
    if (!state) {
        co_return result_t<runtime::messaging::message_parts_t>::failure (
          framework_error_kind_t::protocol_error, "SPOT context is not configured");
    }
    auto native = state->native_spot.lock ();
    if (!native) {
        co_return result_t<runtime::messaging::message_parts_t>::failure (
          framework_error_kind_t::not_found, "SPOT mesh route requires a running native Spot");
    }
    try {
        const auto target_node_rid = zlink::routing_id_t::from (std::string (node_rid.value ()));
        const auto target_generation =
          co_await resolve_target_spot_generation (state->node, target_node_rid, spot_id);
        if (!target_generation) {
            co_return result_t<runtime::messaging::message_parts_t>::failure (
              framework_error_kind_t::not_found,
              "SPOT mesh request target generation is unavailable");
        }
        co_return co_await detail::request_submission (request_spot_parts_async (
          *native, target_node_rid, spot_id, *target_generation, std::move (parts), timeout));
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<runtime::messaging::message_parts_t> (error);
    }
    catch (const std::exception &error) {
        co_return result_t<runtime::messaging::message_parts_t>::failure (
          framework_error_kind_t::internal_failure, error.what ());
    }
}

request_task_t<zlink::message_t>
request_spot_mesh_message (const std::shared_ptr<detail::spot_context_state_t> &state,
                           node_rid_t node_rid,
                           spot_id_t spot_id,
                           runtime::messaging::message_parts_t parts,
                           std::chrono::milliseconds timeout)
{
    auto source = std::make_shared<task_completion_source_t<zlink::message_t>> ();
    auto output = source->task ();
    auto reply = request_spot_mesh_parts (state, std::move (node_rid), std::move (spot_id),
                                          std::move (parts), timeout);
    auto admitted = detail::request_task_access_t::admission_source (reply);
    auto physical_reply = detail::request_task_access_t::physical_reply (reply);
    detail::observe_task_completion (
      physical_reply,
      [source] (const result_t<runtime::messaging::message_parts_t> &reply_result) mutable {
          try {
              if (!reply_result) {
                  source->complete (detail::propagate_failure<zlink::message_t> (
                    reply_result, "SPOT mesh request failed"));
                  return;
              }
              runtime::messaging::envelope_codec_t envelope;
              auto reply_header = envelope.decode_header (reply_result.value (), false);
              if (!reply_header) {
                  source->complete (result_t<zlink::message_t>::failure (
                    reply_header.error_kind (), reply_header.error ()
                                                  ? reply_header.error ()->what ()
                                                  : "SPOT route reply header decode failed"));
                  return;
              }
              if (reply_header.value ().kind == runtime::messaging::message_kind_t::error) {
                  runtime::messaging::request_failure_mapper_t failure_mapper;
                  source->complete (detail::result_access_t::failure<zlink::message_t> (
                    failure_mapper.error_header_exception (
                      reply_header.value ().error_code.value_or ("request_failed"),
                      reply_header.value ().error_message.value_or ("SPOT route request failed"),
                      "SPOT route request")));
                  return;
              }
              auto body = envelope.decode_body (reply_result.value ());
              if (!body) {
                  source->complete (detail::propagate_failure<zlink::message_t> (
                    body, "SPOT route reply body decode failed"));
                  return;
              }
              source->complete (result_t<zlink::message_t>::success (body.value ()));
          }
          catch (const framework_exception_t &error) {
              source->complete (detail::result_access_t::failure<zlink::message_t> (error));
          }
          catch (const std::exception &error) {
              source->complete (result_t<zlink::message_t>::failure (
                framework_error_kind_t::internal_failure, error.what ()));
          }
          catch (...) {
              source->complete (result_t<zlink::message_t>::failure (
                framework_error_kind_t::internal_failure, "SPOT route reply processing failed"));
          }
      });
    return request_task_t<zlink::message_t> (std::move (output), std::move (admitted));
}
} // namespace

namespace detail
{

transferred_owner_reservation_guard_t::transferred_owner_reservation_guard_t (
  std::function<void ()> settle) :
    _settle (std::move (settle))
{
}

transferred_owner_reservation_guard_t::~transferred_owner_reservation_guard_t () noexcept
{
    settle ();
}

transferred_owner_reservation_guard_t::operator bool () const noexcept
{
    return static_cast<bool> (_settle);
}

const std::function<void ()> &transferred_owner_reservation_guard_t::callback () const noexcept
{
    return _settle;
}

std::function<void ()> transferred_owner_reservation_guard_t::release () noexcept
{
    return std::exchange (_settle, {});
}

void transferred_owner_reservation_guard_t::dismiss () noexcept
{
    _settle = {};
}

void transferred_owner_reservation_guard_t::settle () noexcept
{
    auto callback = std::exchange (_settle, {});
    if (!callback)
        return;
    try {
        callback ();
    }
    catch (...) {
    }
}

thread_local constinit const spot_context_state_t *current_callback_context = nullptr;

callback_context_scope_t::callback_context_scope_t (const spot_context_state_t *owner) noexcept :
    _previous (std::exchange (current_callback_context, owner))
{
}

callback_context_scope_t::~callback_context_scope_t ()
{
    current_callback_context = _previous;
}

namespace
{
struct spot_execution_snapshot_t
{
    std::vector<std::shared_ptr<spot_context_state_t>> contexts;
    std::vector<task_t<void>> creations;
};

spot_execution_snapshot_t snapshot_spot_execution_owned (spot_node_builder_state_t &node)
{
    spot_execution_snapshot_t result;
    result.contexts.reserve (node.spot_contexts_by_id.size ()
                             + node.pending_spot_creations_by_id.size ());
    const auto include = [&] (std::shared_ptr<spot_context_state_t> state) {
        if (state
            && std::find (result.contexts.begin (), result.contexts.end (), state)
                 == result.contexts.end ())
            result.contexts.push_back (std::move (state));
    };
    for (const auto &[_, context] : node.spot_contexts_by_id)
        include (spot_context_access_t::state (context));
    for (const auto &[_, creation] : node.pending_spot_creations_by_id) {
        include (creation.context.lock ());
        if (creation.completion)
            result.creations.push_back (creation.completion->task ());
    }
    return result;
}
} // namespace

void drain_spot_node_executors (spot_node_builder_state_t &node,
                                std::chrono::steady_clock::time_point deadline)
{
    struct drain_plan_t
    {
        struct context_t
        {
            std::shared_ptr<spot_context_state_t> state;
            std::vector<std::shared_ptr<runtime::serial_execution_queue_t>> retained_queues;
            std::shared_ptr<runtime::serial_execution_queue_t> serial_queue;
            task_t<void> admitted_callbacks;
        };
        std::vector<context_t> contexts;
        std::vector<task_t<void>> creations;
        std::shared_ptr<runtime::offload_executor_t> deadline_executor;
        std::shared_ptr<runtime::offload_executor_t> worker_executor;
        std::stop_source cancellation;
    };
    auto plan = node.lane.run_sync ([&] {
        drain_plan_t result;
        node.stopping.store (true, std::memory_order_release);
        auto execution = snapshot_spot_execution_owned (node);
        result.creations = std::move (execution.creations);
        result.contexts.reserve (execution.contexts.size ());
        for (const auto &state : execution.contexts) {
            result.contexts.push_back (drain_plan_t::context_t{
              state, {}, state->serial_queue, state->admitted_callbacks_terminal ()});
            auto &context_plan = result.contexts.back ();
            if (state->spot_serial_executor) {
                context_plan.retained_queues = state->spot_serial_executor->timer_queues ();
                const auto actors = state->spot_serial_executor->actor_executor_snapshot ();
                for (const auto &[_, actor] : *actors)
                    if (actor && actor->queue ())
                        context_plan.retained_queues.push_back (actor->queue ());
            }
        }
        result.cancellation = node.worker_cancellation;
        result.deadline_executor = node.deadline_executor;
        result.worker_executor = node.worker_executor;
        return result;
    });

    // Host relocation §14: accepted callbacks retain their execution resources
    // until the host deadline. Only deadline expiry cancels their owned waits.
    bool expired = false;
    for (const auto &context : plan.contexts) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds> (
          deadline - std::chrono::steady_clock::now ());
        if (!context.admitted_callbacks.result_for (
              std::max (remaining, std::chrono::milliseconds::zero ()))) {
            expired = true;
            break;
        }
    }
    if (expired || std::chrono::steady_clock::now () >= deadline) {
        plan.cancellation.request_stop ();
        if (plan.worker_executor)
            plan.worker_executor->request_stop ();
        if (plan.deadline_executor)
            plan.deadline_executor->request_stop ();
        for (const auto &context : plan.contexts) {
            for (const auto &queue : context.retained_queues)
                if (queue)
                    queue->cancel_pending ();
            if (context.serial_queue)
                context.serial_queue->cancel_pending ();
        }
    }
    for (const auto &context : plan.contexts) {
        for (const auto &queue : context.retained_queues)
            if (queue)
                queue->drain ();
        if (context.serial_queue)
            context.serial_queue->drain ();
        detail::task_result_access_t::read (context.admitted_callbacks).value ();
    }
    // The creation caller owns its result. Keep its execution resources until
    // that existing terminal has settled, including cancellation cleanup.
    for (const auto &creation : plan.creations)
        (void) detail::task_result_access_t::read (creation);
    if (plan.deadline_executor)
        plan.deadline_executor->drain ();
    if (plan.worker_executor)
        plan.worker_executor->drain ();

    node.lane.run_sync ([&] {
        for (const auto &context : plan.contexts) {
            context.state->serial_executor.reset ();
            if (context.state->spot_serial_executor)
                context.state->spot_serial_executor->close ();
            context.state->worker_scheduler.reset ();
        }
        node.deadline_executor.reset ();
        node.worker_executor.reset ();
    });
}

void visit_spot_node_dispatch_queues (
  spot_node_builder_state_t &node,
  const std::function<void (runtime::serial_execution_queue_t &)> &visitor)
{
    const auto queues = node.lane.run_sync ([&] {
        std::vector<std::shared_ptr<runtime::serial_execution_queue_t>> result;
        const auto execution = snapshot_spot_execution_owned (node);
        result.reserve (execution.contexts.size ());
        for (const auto &state : execution.contexts)
            if (state->serial_queue)
                result.push_back (state->serial_queue);
        return result;
    });
    for (const auto &queue : queues)
        if (queue)
            visitor (*queue);
}

void report_spot_dispatch_error (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                 dispatch_error_surface_t surface,
                                 dispatch_message_kind_t message_kind,
                                 dispatch_error_reason_t reason,
                                 dispatch_error_action_t action,
                                 std::optional<std::string> packet_name,
                                 std::optional<std::string> topic,
                                 std::optional<std::string> spot_id,
                                 std::optional<std::string> actor_id,
                                 std::exception_ptr exception,
                                 std::optional<std::string> correlation_id)
{
    if (!state) {
        return;
    }
    detail::dispatch_error_reporter_t (state->dispatch).report_lazy ([&] {
        return message_dispatch_error_event_t{surface,
                                              message_kind,
                                              reason,
                                              action,
                                              std::move (packet_name),
                                              std::nullopt,
                                              std::move (topic),
                                              std::move (spot_id),
                                              std::move (actor_id),
                                              std::nullopt,
                                              std::move (correlation_id),
                                              std::move (exception),
                                              std::nullopt,
                                              std::nullopt};
    });
}

void report_actor_control_terminal (const std::shared_ptr<detail::spot_node_builder_state_t> &node,
                                    const result_t<void> &terminal,
                                    std::optional<std::string> spot_id,
                                    std::optional<std::string> actor_id)
{
    if (terminal)
        return;
    report_spot_dispatch_error (
      node, dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::control,
      detail::dispatch_reason_from_error (terminal.error_kind ()), dispatch_error_action_t::drop,
      std::nullopt, std::nullopt, std::move (spot_id), std::move (actor_id),
      terminal.error () ? std::make_exception_ptr (*terminal.error ()) : nullptr);
}

void report_spot_dispatch_trace (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                 message_flow_outcome_t outcome,
                                 dispatch_error_surface_t surface,
                                 dispatch_message_kind_t message_kind,
                                 std::string_view packet_name,
                                 std::string_view topic,
                                 std::string_view spot_id,
                                 std::string_view actor_id,
                                 std::string_view correlation_id,
                                 std::optional<message_flow_result_t> result,
                                 std::optional<message_flow_reason_t> reason,
                                 std::string_view detail_result,
                                 std::exception_ptr error)
{
    if (!state) {
        return;
    }
    // string_view params + lazy build: callers pass cheap views; std::string is
    // only allocated inside the lambda after the gate passes (zero cost when off).
    if (detail::message_flow_tracer_t (state->dispatch).enabled_for (outcome, result)) {
        detail::message_flow_tracer_t (state->dispatch).trace (outcome, result, [&] {
            auto field = [] (std::string_view value) -> std::optional<std::string> {
                if (value.empty ()) {
                    return std::nullopt;
                }
                return std::string (value);
            };
            auto event = message_flow_event_t{outcome,
                                              surface,
                                              message_kind,
                                              field (packet_name),
                                              std::nullopt,
                                              field (topic),
                                              field (correlation_id),
                                              std::nullopt,
                                              field (spot_id),
                                              field (actor_id),
                                              std::nullopt};
            event.result = result;
            event.reason = reason;
            event.detail_result = field (detail_result);
            event.exception = error;
            return event;
        });
    }
}

void report_actor_dispatch_stage_trace (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  message_flow_outcome_t outcome,
  dispatch_message_kind_t message_kind,
  std::string_view packet_name,
  std::string_view spot_id,
  std::string_view actor_id,
  std::string_view stage,
  std::string_view result)
{
    if (state
        && detail::message_flow_tracer_t (state->dispatch)
             .enabled (message_flow_log_mode_t::detailed)) {
        report_actor_dispatch_stage_trace_lazy (state, outcome, message_kind, packet_name, spot_id,
                                                actor_id, stage,
                                                [&] { return std::string (result); });
    }
}

bool spot_context_state_t::idle_quiescent () const
{
    auto owner = state_lane_owner ();
    if (!owner)
        return false;

    struct snapshot_t
    {
        bool state_quiescent = false;
        std::shared_ptr<runtime::serial_execution_queue_t> queue;
        std::vector<std::shared_ptr<timer_state_t>> timers;
    };
    auto snapshot =
      owner->lane
        .run ([this] {
            return snapshot_t{actor_count == 0 && !relocation_boundary_active
                                && !relocation_ready_deferred && queued_routed_packets.empty (),
                              serial_queue, timers};
        })
        .get ();
    if (!snapshot.state_quiescent || !snapshot.queue)
        return false;
    if (snapshot.queue->pending_count (runtime::serial_work_lane_t::application) != 0
        || snapshot.queue->pending_count (runtime::serial_work_lane_t::lifecycle) != 0) {
        return false;
    }
    for (const auto &timer : snapshot.timers) {
        if (!timer)
            continue;
        std::lock_guard<std::mutex> timer_lock (timer->mutex);
        if (!timer->disposed
            && (timer->running || timer->pending_fire || timer->pending_fire_count != 0)) {
            return false;
        }
    }
    return true;
}

bool spot_context_state_t::admit_core (
  bool claim, const instance_spot_retained_message_t *retained_record) noexcept
{
    if ((claim && node
         && (node->host_draining () || node->stopping.load (std::memory_order_acquire)))
        || admission_sealed || (closed && claim && !retained_record))
        return false;
    if (claim)
        ++callback_depth;
    return true;
}

bool spot_context_state_t::admit (bool claim,
                                  const instance_spot_retained_message_t *retained_record)
{
    return state_sync (
      [this, claim, retained_record] { return admit_core (claim, retained_record); });
}

framework_exception_t spot_context_state_t::target_not_found (std::string message)
{
    return detail::make_framework_origin_exception (framework_error_kind_t::not_found,
                                                    std::move (message));
}

framework_exception_t spot_context_state_t::admission_error () const
{
    return state_sync ([this] {
        // Spot 주소 메시징 §9의 Closing·Draining 분류를 이곳에서 결정합니다.
        // Relocation ingress는 이 분류 전에 native seal의 hold에 보관됩니다.
        // 미확정 idle reservation과 relocation 정리의 기존 오류는 유지합니다.
        if ((node && (node->host_draining () || node->stopping.load (std::memory_order_acquire)))
            || closed == spot_close_reason_t::host_shutdown)
            return detail::make_framework_origin_exception (framework_error_kind_t::shutting_down,
                                                            "SPOT is closed.");
        if (closed == spot_close_reason_t::explicit_close
            || closed == spot_close_reason_t::idle_evicted)
            return target_not_found ("SPOT is closing.");
        return detail::make_framework_origin_exception (framework_error_kind_t::rejected,
                                                        "Spot admission is sealed");
    });
}

task_t<void>
spot_context_state_t::discard_preparation (std::shared_ptr<service::spot_t> native,
                                           std::function<void ()> release_admitted_work)
{
    struct discard_preparation_t
    {
        static task_t<void>
        run (std::shared_ptr<spot_context_state_t> self,
             std::decay_t<decltype (native)> native,
             std::decay_t<decltype (std::move (release_admitted_work))> release_admitted_work)
        {
            const auto owner = self->state_lane_owner ();
            auto callbacks = owner->lane
                               .run ([self] {
                                   self->closed = spot_close_reason_t::explicit_close;
                                   return self->admitted_callbacks_terminal ();
                               })
                               .get ();
            self->cancel_timers ();
            // Only the operation that owns preparation may release its admission.
            // A borrowed Close keeps the old incarnation's pending messages held.
            if (release_admitted_work)
                release_admitted_work ();
            co_await callbacks;
            // Completion can arrive on the state lane; synchronous teardown
            // remains on the existing infrastructure executor.
            std::exception_ptr teardown_failure;
            struct teardown_preparation_t
            {
                static task_t<void>
                run (decltype (self) self, decltype (owner) owner, decltype (native) native)
                {
                    const auto id_value = std::string (self->spot_id);
                    owner->lane
                      .run ([&] {
                          self->native_spot.reset ();
                          if (const auto found = owner->native_spots_by_id.find (id_value);
                              found != owner->native_spots_by_id.end () && found->second == native)
                              owner->native_spots_by_id.erase (found);
                          const auto release_context = [self, owner,
                                                        id_value] (const result_t<void> &) {
                              self->state_sync ([&] {
                                  if (const auto found = owner->spot_contexts_by_id.find (id_value);
                                      found != owner->spot_contexts_by_id.end ()
                                      && spot_context_access_t::state (found->second) == self)
                                      owner->spot_contexts_by_id.erase (found);
                              });
                          };
                          const auto preparation =
                            self->spot_serial_executor
                              ? self->spot_serial_executor->preparation_operation ()
                              : nullptr;
                          if (preparation) {
                              // Preparation cleanup remains in the node's execution snapshot
                              // until its existing lifecycle terminal releases the queue.
                              auto terminal = preparation->lifecycle->terminal ();
                              detail::observe_task_terminal (terminal, release_context);
                          } else {
                              release_context (result_t<void>::success ());
                          }
                          if (const auto found = owner->spot_names_by_id.find (id_value);
                              found != owner->spot_names_by_id.end ()
                              && found->second == self->spot_name)
                              owner->spot_names_by_id.erase (found);
                          if (const auto found = owner->spot_ids_by_name.find (self->spot_name);
                              found != owner->spot_ids_by_name.end ()
                              && found->second == self->spot_id)
                              owner->spot_ids_by_name.erase (found);
                      })
                      .get ();
                    if (native)
                        (void) native->close ();
                    try {
                        self->detach_application_instance (false);
                    }
                    catch (...) {
                        const auto detach_failure = std::current_exception ();
                        report_spot_close_diagnostic (owner, id_value, "preparation_detach_failed",
                                                      detach_failure);
                        throw;
                    }
                    co_return;
                }
            };

            try {
                co_await runtime::run_blocking_step<void> ([self, owner, native] {
                    return teardown_preparation_t::run (self, owner, native);
                });
            }
            catch (...) {
                teardown_failure = std::current_exception ();
            }
            if (teardown_failure)
                std::rethrow_exception (teardown_failure);
            co_return;
        }
    };
    return runtime::run_blocking_step<void> (
      [self = shared_from_this (), native,
       release_admitted_work = std::move (release_admitted_work)] () -> task_t<void> {
          return discard_preparation_t::run (self, native, std::move (release_admitted_work));
      });
}

task_t<void> spot_context_state_t::complete_preparation (bool ready)
{
    const auto coordinator = spot_serial_executor;
    if (!coordinator)
        return task_t<void> (result_t<void>::success ());
    return coordinator->finish_preparation (
      ready
        ? detail::deferred_barrier_t::async_work_t{}
        : [self = shared_from_this (), native = native_spot.lock (), coordinator] (auto complete) {
              auto cleanup = std::make_shared<task_t<void>> (self->discard_preparation (
                native, [coordinator] { coordinator->open_preparation_admission (); }));
              detail::observe_task_terminal (
                *cleanup,
                [cleanup, complete] (const result_t<void> &result) mutable { complete (result); });
          });
}

bool spot_context_state_t::admit_timer_fire (
  const std::shared_ptr<runtime::serial_execution_queue_t> &queue)
{
    return state_sync ([this, &queue] {
        const auto &execution = queue ? queue : serial_queue;
        return channel_runtime && channel_runtime->serializers
               && (!execution
                   || execution->execution_status ()
                        == runtime::serial_execution_queue_t::execution_status_t::allowed)
               && admit_core (true);
    });
}

void spot_context_state_t::enter_callback ()
{
    state_sync ([this] { ++callback_depth; });
}

void spot_context_state_t::leave_callback (std::function<void ()> settle_owner_state) noexcept
{
    struct leave_decision_t
    {
        std::exception_ptr settle_error;
    };
    leave_decision_t decision;
    try {
        decision = state_sync ([this, &settle_owner_state] {
            leave_decision_t current;
            if (callback_depth > 0)
                --callback_depth;
            if (settle_owner_state) {
                try {
                    settle_owner_state ();
                }
                catch (...) {
                    current.settle_error = std::current_exception ();
                }
            }
            settle_admitted_callbacks_core ();
            return current;
        });
    }
    catch (...) {
        return;
    }
    if (decision.settle_error)
        report_spot_dispatch_trace (
          node, message_flow_outcome_t::completed, dispatch_error_surface_t::spot_actor,
          dispatch_message_kind_t::control, "spot_turn_settle", {}, spot_id, {}, {},
          message_flow_result_t::failed, std::nullopt, "failed", decision.settle_error);
}

void spot_context_state_t::request_close (service::spot_close_begin_t begin,
                                          service::spot_close_done_t done,
                                          spot_close_reason_t reason,
                                          std::chrono::system_clock::time_point deadline,
                                          std::stop_token cleanup_cancellation)
{
    /* The lifecycle item ends when the Close result is decided. A queue that
     * discards the item before it runs reports ShuttingDown once. */
    struct close_report_t
    {
        explicit close_report_t (service::spot_close_done_t value) : done (std::move (value)) {}
        ~close_report_t ()
        {
            if (!reported.exchange (true))
                done (result_t<bool>::failure (framework_error_kind_t::shutting_down,
                                               "Spot lifecycle lane closed before Close ran"));
        }
        void operator() (result_t<bool> result)
        {
            if (!reported.exchange (true))
                done (std::move (result));
        }
        service::spot_close_done_t done;
        std::atomic_bool reported{false};
    };
    auto report = std::make_shared<close_report_t> (std::move (done));
    // 진행 중인 Close 합류는 닫힐 수 있는 lifecycle queue에 접근하기 전에 결정합니다.
    if (state_sync ([this, report] {
            return merge_into_running_close_core (
              [report] (result_t<bool> result) { (*report) (std::move (result)); });
        }))
        return;
    auto shared_begin = std::make_shared<service::spot_close_begin_t> (std::move (begin));
    auto run = [state = shared_from_this (), shared_begin, reason, deadline, cleanup_cancellation,
                report] (std::function<void ()> finished, detail::task_scheduler_t resume,
                         std::shared_ptr<spot_serial_executor_t::preparation_t> operation) {
        state->close_now (
          std::move (*shared_begin),
          [report, finished = std::move (finished)] (result_t<bool> result) {
              (*report) (std::move (result));
              finished ();
          },
          std::move (resume), reason, std::move (operation), deadline, cleanup_cancellation);
    };
    const auto queue = serial_queue;
    if (!queue) {
        run ([] {}, {}, {});
        return;
    }
    const auto coordinator = ensure_spot_serial_executor ();
    if (!coordinator) {
        (*report) (result_t<bool>::failure (framework_error_kind_t::shutting_down,
                                            "Spot lifecycle coordinator is unavailable"));
        return;
    }
    const auto reserved = queue->reserve_barrier_next ("spot-close");
    if (!reserved) {
        (*report) (result_t<bool>::failure (framework_error_kind_t::shutting_down,
                                            "Spot lifecycle lane is closed"));
        return;
    }
    const auto barrier = reserved.value ();
    // Preparation policy cleanup still uses the node lane after the Close
    // continuation releases its activation and node references.
    auto terminal = barrier->terminal ();
    detail::observe_task_terminal (terminal,
                                   [owner = state_lane_owner ()] (const result_t<void> &) {});
    auto executing = std::make_shared<task_t<void>> (barrier->run ([run, coordinator, barrier] {
        const auto operation = coordinator->begin_preparation (barrier);
        auto completion = std::make_shared<task_completion_source_t<void>> ();
        const auto turn = detail::capture_current_serial_turn ();
        run ([completion] { completion->complete (result_t<void>::success ()); },
             turn ? turn->resume_scheduler () : detail::task_scheduler_t{}, operation);
        return completion->task ();
    }));
    detail::observe_task_terminal (
      *executing, [executing, coordinator, barrier, report] (const result_t<void> &result) {
          if (!result)
              (*report) (result_t<bool>::failure (result.error_kind (), result.error ()->what ()));
          const auto operation = coordinator->preparation_operation ();
          if (operation && operation->lifecycle == barrier)
              (void) coordinator->finish_preparation ();
      });
}

service::spot_close_begin_t spot_context_state_t::local_close_step ()
{
    return [] {
        return task_t<service::spot_close_commit_t> (
          result_t<service::spot_close_commit_t>::success (
            service::spot_close_commit_t{result_t<bool>::success (true), [] {
                                             return task_t<bool> (result_t<bool>::success (true));
                                         }}));
    };
}

namespace
{

/* Runs a Close Store step (step 1 or 4). A Close that holds its own
 * lifecycle item (`resume`) runs the step on the Framework blocking-call
 * executor, so the item returns its turn while the Location Store works and
 * resumes through the gate (handler turn and execution gate §7); a step that
 * cannot start there ends with ShuttingDown. A Close without its own item
 * runs in its caller's turn, and the step runs there. */
template <typename T>
task_t<T> run_close_step (std::function<task_t<T> ()> step, const detail::task_scheduler_t &resume)
{
    if (!resume)
        return step ();
    return runtime::run_blocking_step<T> (std::move (step));
}

} // namespace

void spot_context_state_t::close_now (
  service::spot_close_begin_t begin,
  service::spot_close_done_t done,
  detail::task_scheduler_t resume,
  spot_close_reason_t reason,
  std::shared_ptr<spot_serial_executor_t::preparation_t> operation,
  std::chrono::system_clock::time_point deadline,
  std::stop_token cleanup_cancellation)
{
    auto owner = state_lane_owner ();
    if (!owner) {
        done (result_t<bool>::success (false));
        return;
    }
    const auto start =
      owner->lane
        .run ([this, &owner, reason] {
            close_start_t result;
            // Idle cleanup sealed the activation first.
            if (close_reservation != 0)
                return result;
            if (node.get () != owner.get ())
                return result;
            if (closed) {
                result.path = close_path_t::failed;
                return result;
            }
            // Actor membership keeps admission and authority. A Join or leave
            // accepted earlier on this lane has decided its membership.
            if (reason != spot_close_reason_t::host_shutdown && actor_count != 0)
                return result;
            result.path = close_path_t::start;
            result.kind = metric_spot_kind (*this);
            result.token = reserve_close_core (*owner, close_reservation_kind_t::explicit_close);
            if (reason == spot_close_reason_t::host_shutdown) {
                result.authority_begin = local_close_step ();
            } else if (is_instance_spot () && owner->begin_instance_spot_close) {
                result.authority_begin = [begin = owner->begin_instance_spot_close, id = spot_id,
                                          name = spot_name, generation = object_generation,
                                          owner_generation = authority_owner_generation] {
                    return begin (id, name, generation, owner_generation);
                };
            } else if (!is_instance_spot () && !is_entry_spot () && owner->begin_user_spot_close) {
                result.authority_begin = [begin = owner->begin_user_spot_close, id = spot_id,
                                          generation = object_generation,
                                          owner_generation = authority_owner_generation] {
                    return begin (id, generation, owner_generation);
                };
            } else {
                result.authority_begin = local_close_step ();
            }
            return result;
        })
        .get ();
    if (start.path == close_path_t::not_executed) {
        done (result_t<bool>::success (false));
        return;
    }
    if (start.path == close_path_t::failed) {
        done (result_t<bool>::failure (framework_error_kind_t::unavailable,
                                       "Spot Close failed after admission was sealed"));
        return;
    }

    // The Close ends its reservation in the same lane turn that takes the
    // merged requests, so every merged request receives this result.
    const auto token = start.token;
    auto settle = [self = shared_from_this (), owner, done, token,
                   kind = start.kind] (result_t<bool> result) {
        auto merged = owner->lane
                        .run ([&] {
                            self->clear_close_reservation_core (token);
                            return std::exchange (self->merged_close_results, {});
                        })
                        .get ();
        if (result && result.value ()) {
            runtime::runtime_metrics_t metrics (owner->monitoring);
            if (metrics.enabled ())
                metrics.updown ("zlink.spot.count", "{spot}", -1,
                                {{"mesh_name", owner->snapshot.name}, {"spot_kind", kind}});
        }
        done (result);
        for (auto &waiter : merged)
            waiter (result);
    };
    service::after_close_step (
      run_close_step<service::spot_close_commit_t> (
        begin ? std::move (begin) : start.authority_begin, resume),
      resume,
      [self = shared_from_this (), owner, token, settle, reason, resume, operation, deadline,
       cleanup_cancellation] (result_t<service::spot_close_commit_t> commit) mutable {
          if (!commit) {
              settle (result_t<bool>::failure (commit.error_kind (),
                                               commit.error () ? commit.error ()->what ()
                                                               : "Spot Close step 1 failed"));
              return;
          }
          if (!commit.value ().release) {
              // Step 1 ended the Close and left the authority unchanged.
              settle (std::move (commit.value ().result));
              return;
          }
          self->run_local_close_steps (owner, token, std::move (commit.value ().release),
                                       std::move (commit.value ().reincarnate),
                                       std::move (commit.value ().discard_reincarnation),
                                       std::move (settle), std::move (resume), reason, operation,
                                       deadline, cleanup_cancellation);
      });
}

void spot_context_state_t::run_local_close_steps (
  const std::shared_ptr<spot_node_builder_state_t> &owner,
  std::uint64_t token,
  std::function<task_t<bool> ()> release,
  std::function<task_t<authority_snapshot_t> ()> reincarnate,
  std::function<task_t<bool> (const authority_snapshot_t &)> discard_reincarnation,
  service::spot_close_done_t done,
  detail::task_scheduler_t resume,
  spot_close_reason_t reason,
  std::shared_ptr<spot_serial_executor_t::preparation_t> operation,
  std::chrono::system_clock::time_point deadline,
  std::stop_token cleanup_cancellation)
{
    struct initialize_reincarnation_t
    {
        static task_t<bool> run (std::shared_ptr<spot_context_state_t> self,
                                 std::shared_ptr<spot_node_builder_state_t> owner,
                                 authority_snapshot_t next,
                                 std::shared_ptr<const void> original,
                                 std::shared_ptr<spot_serial_executor_t::preparation_t> operation)
        {
            spot_node_runtime_t node (owner);
            auto creating = [&] {
                const auto command =
                  std::static_pointer_cast<const instance_spot_retained_message_t> (original);
                auto flow = runtime::flow_context_t::enter (
                  command->original_activation->application_payload.flow_id,
                  command->original_activation->application_payload.flow_origin,
                  message_flow_tracer_t (owner->dispatch).mode (), flow_origin_t::inbound,
                  std::nullopt);
                return node.create_spot_context (
                  command->original_activation->activation.target.stable_type, self->spot_id,
                  zlink::message_t::from (
                    command->original_activation->application_payload.payload_bytes ()),
                  next.object_generation, next.allocation.target.mesh_name, {},
                  next.authority_owner_generation, self->serial_queue, nullptr, operation);
            }();
            try {
                const auto created = co_await creating;
                co_return created->state == spot_create_state_t::created;
            }
            catch (const framework_exception_t &) {
                throw;
            }
            catch (const std::exception &error) {
                // Async creation bypasses run_blocking_step's synchronous
                // catch; Close consumers require the same Framework failure.
                throw framework_exception_t (framework_error_kind_t::internal_failure,
                                             error.what ());
            }
            catch (...) {
                throw framework_exception_t (framework_error_kind_t::internal_failure,
                                             "Spot creation failed");
            }
        }
    };
    // Steps 3 and 4: OnClosing once, local cleanup, then the fenced release.
    auto finish = [self = shared_from_this (), owner, token, release = std::move (release),
                   reincarnate = std::move (reincarnate), done,
                   discard_reincarnation = std::move (discard_reincarnation), resume, reason,
                   operation, deadline, cleanup_cancellation] () mutable {
        const bool sealed =
          owner->lane.run ([&] { return self->close_reservation == token && self->closed; }).get ();
        if (!sealed) {
            done (result_t<bool>::failure (framework_error_kind_t::internal_failure,
                                           "Spot Close lost its sealed activation"));
            return;
        }
        try {
            self->close_application_then_release_location (owner, reason, token, deadline,
                                                           cleanup_cancellation);
        }
        catch (...) {
            // Closing stays committed after local release failure.
            done (::zlink::framework::detail::result_access_t::failure<bool> (
              std::current_exception ()));
            return;
        }
        auto pending = self->serial_queue ? self->serial_queue->first_pending_message_or_close ()
                                          : task_t<std::shared_ptr<const void>> (
                                              result_t<std::shared_ptr<const void>>::success ({}));
        service::after_close_step (
          std::move (pending), resume,
          [self, owner, done, release, reincarnate, discard_reincarnation, resume,
           operation] (result_t<std::shared_ptr<const void>> probed) mutable {
              if (!probed) {
                  done (result_t<bool>::failure (probed.error_kind (), probed.error ()->what ()));
                  return;
              }
              const auto original = probed.value ();
              const auto fail_pending = [self, owner, resume, done] (const result_t<bool> &failure,
                                                                     result_t<bool> terminal) {
                  auto pending =
                    self->serial_queue
                      ? self->serial_queue->pending_messages ()
                      : task_t<std::vector<std::shared_ptr<const void>>> (
                          result_t<std::vector<std::shared_ptr<const void>>>::success ({}));
                  service::after_close_step (
                    std::move (pending), resume,
                    [self, owner, failure, done, terminal] (
                      result_t<std::vector<std::shared_ptr<const void>>> messages) mutable {
                        if (!messages) {
                            report_spot_close_diagnostic (owner, std::string (self->spot_id),
                                                          "pending_query_failed",
                                                          messages.exception ());
                            done (result_t<bool>::failure (messages.error_kind (),
                                                           messages.error ()->what ()));
                            return;
                        }
                        const auto kind = failure ? framework_error_kind_t::internal_failure
                                                  : failure.error_kind ();
                        const auto message =
                          failure.error () ? std::string (failure.error ()->what ())
                                           : "Spot initialization did not create an incarnation";
                        for (const auto &retained : messages.value ()) {
                            const auto command =
                              std::static_pointer_cast<const instance_spot_retained_message_t> (
                                retained);
                            if (command->completion)
                                command->completion->complete (
                                  result_t<zlink::message_t>::failure (kind, message));
                        }
                        owner->lane
                          .run ([&] {
                              const auto found =
                                owner->spot_contexts_by_id.find (std::string (self->spot_id));
                              if (found != owner->spot_contexts_by_id.end ()
                                  && spot_context_access_t::state (found->second) == self)
                                  owner->spot_contexts_by_id.erase (found);
                          })
                          .get ();
                        done (terminal);
                    });
              };
              // Spot address messaging §7 step 3: drain or relocation forbids
              // Reincarnate, and its kind ends the Instance intent messages left after
              // the release. Accepted messages are never placed again.
              const auto boundary =
                owner->lane
                  .run ([&] {
                      const auto phase = owner->host_lifecycle
                                           ? std::optional{owner->host_lifecycle ().phase}
                                           : std::nullopt;
                      return phase == framework_runtime_state_t::draining
                               ? std::optional{framework_error_kind_t::shutting_down}
                             : phase == framework_runtime_state_t::relocating
                                 || phase == framework_runtime_state_t::relocated
                               ? std::optional{framework_error_kind_t::unavailable}
                               : std::optional<framework_error_kind_t>{};
                  })
                  .get ();
              if (original && !boundary && reincarnate) {
                  service::after_close_step (
                    run_close_step<authority_snapshot_t> (reincarnate, resume), resume,
                    [self, owner, done, resume, original, discard_reincarnation, fail_pending,
                     operation] (result_t<authority_snapshot_t> snapshot) {
                        if (!snapshot) {
                            const auto failure = result_t<bool>::failure (
                              snapshot.error_kind (), snapshot.error ()
                                                        ? snapshot.error ()->what ()
                                                        : "Spot Reincarnate failed");
                            fail_pending (failure, failure);
                            return;
                        }
                        const auto next = snapshot.value ();
                        service::after_close_step (
                          run_close_step<bool> (
                            [self, owner, next, original, operation] () -> task_t<bool> {
                                return initialize_reincarnation_t::run (self, owner, next, original,
                                                                        operation);
                            },
                            resume),
                          resume,
                          [done, next, discard_reincarnation, resume,
                           fail_pending] (result_t<bool> initialized) {
                              if (initialized && initialized.value ()) {
                                  done (initialized);
                                  return;
                              }
                              service::after_close_step (
                                run_close_step<bool> (
                                  [discard_reincarnation, next] {
                                      return discard_reincarnation (next);
                                  },
                                  resume),
                                resume, [done, initialized, fail_pending] (result_t<bool> deleted) {
                                    const auto failure =
                                      !deleted ? deleted
                                      : !initialized
                                        ? initialized
                                        : result_t<bool>::failure (
                                            framework_error_kind_t::internal_failure,
                                            "Spot initialization did not create an incarnation");
                                    fail_pending (failure, failure);
                                });
                          });
                    });
                  return;
              }
              service::after_close_step (
                run_close_step<bool> (release, resume), resume,
                [done, owner, self, original, boundary,
                 fail_pending] (result_t<bool> result) mutable {
                    if (original && boundary) {
                        fail_pending (
                          result_t<bool>::failure (
                            *boundary, *boundary == framework_error_kind_t::shutting_down
                                         ? "Spot Close released its authority during host drain"
                                         : "Spot Close released its authority during relocation"),
                          result);
                        return;
                    }
                    owner->lane
                      .run ([&] {
                          const auto found =
                            owner->spot_contexts_by_id.find (std::string (self->spot_id));
                          if (found != owner->spot_contexts_by_id.end ()
                              && spot_context_access_t::state (found->second) == self)
                              owner->spot_contexts_by_id.erase (found);
                      })
                      .get ();
                    done (result);
                });
          });
    };
    auto after_closing = [self = shared_from_this (), owner, reason, finish, done,
                          resume] () mutable {
        if (reason != spot_close_reason_t::host_shutdown) {
            finish ();
            return;
        }
        // Host relocation §14: keep membership through OnClosing, then use
        // the Actor owner to retire each member before releasing the Spot.
        service::after_close_step (
          run_close_step<bool> (
            [self, owner] () -> task_t<bool> {
                const auto node = owner;
                const auto context = self;
                auto members = co_await node->lane.run_task ([node, context] {
                    std::vector<actor_ref_t> result;
                    for (const auto &[id, type] : node->actor_types_by_id) {
                        const auto key = type + ":" + id;
                        const auto membership = node->actor_spot_ids.find (key);
                        if (membership != node->actor_spot_ids.end ()
                            && membership->second == context->spot_id) {
                            result.push_back (::zlink::framework::detail::actor_ref_access_t::make (
                              context->node_rid, type, id, node->actor_generations.at (key)));
                        }
                    }
                    return result;
                });
                spot_node_runtime_t runtime (node);
                for (const auto &member : members) {
                    const auto removed = co_await runtime.discard_local_actor_async (member);
                    if (!removed)
                        co_return removed;
                }
                co_return true;
            },
            resume),
          resume, [finish, done] (result_t<bool> result) mutable {
              if (!result) {
                  done (std::move (result));
                  return;
              }
              finish ();
          });
    };
    owner->lane.run ([&] { closed = reason; }).get ();
    cancel_timers ();
    try {
        const auto instance = spot_instance;
        auto on_closing =
          owner->lane.run ([&] { return std::exchange (lifecycle.on_closing, {}); }).get ();
        if (on_closing && instance) {
            service::after_close_step (
              invoke_spot_closing (std::move (on_closing), instance.get (),
                                   spot_closing_context_t{reason, deadline}, cleanup_cancellation),
              resume,
              [self = shared_from_this (), owner, instance,
               after_closing] (result_t<void> result) mutable {
                  if (!result)
                      report_spot_close_diagnostic (owner, std::string (self->spot_id),
                                                    "on_closing_failed", result.exception ());
                  after_closing ();
              });
            return;
        }
    }
    catch (...) {
        report_spot_close_diagnostic (owner, std::string (spot_id), "on_closing_failed",
                                      std::current_exception ());
    }
    after_closing ();
}

void report_spot_close_diagnostic (const std::shared_ptr<spot_node_builder_state_t> &owner,
                                   std::string_view spot_id,
                                   std::string_view result,
                                   std::exception_ptr error) noexcept
{
    try {
        report_spot_dispatch_trace (owner, message_flow_outcome_t::completed,
                                    dispatch_error_surface_t::spot_actor,
                                    dispatch_message_kind_t::control, "spot_close", {}, spot_id, {},
                                    {}, message_flow_result_t::failed, std::nullopt, result, error);
    }
    catch (...) {
    }
}

bool spot_context_state_t::is_current_callback_thread () const
{
    return current_callback_context == this;
}

bool spot_context_state_t::try_post_serial (std::string name,
                                            std::function<void ()> work,
                                            runtime::serial_work_options_t options)
{
    if (!admit ())
        return false;
    const auto queue = serial_queue;
    if (!queue) {
        work ();
        return true;
    }
    return queue->try_post (std::move (name), std::move (work), std::move (options));
}

bool spot_context_state_t::try_post_serial_after_current_turn (
  std::string name, std::function<void ()> work, runtime::serial_work_options_t options)
{
    return try_post_serial_task_after_current_turn (
      std::move (name),
      [work = std::move (work)] () mutable {
          work ();
          return task_t<void> (result_t<void>::success ());
      },
      std::move (options));
}

bool spot_context_state_t::try_post_serial_task_after_current_turn (
  std::string name, std::function<task_t<void> ()> work, runtime::serial_work_options_t options)
{
    if (!admit ())
        return false;
    const auto queue = serial_queue;
    if (!queue) {
        auto pending = std::make_shared<task_t<void>> (work ());
        detail::observe_task_completion (*pending, [pending, work = std::move (work), owner = node,
                                                    id = spot_id] (const result_t<void> &terminal) {
            report_actor_control_terminal (owner, terminal, id);
        });
        return true;
    }
    const auto current_turn = detail::capture_current_serial_turn ();
    if (owns_current_serial_turn () && current_turn && !current_turn->is_after_active_phase ())
        return queue->try_post_deferred_task (std::move (name), std::move (work));
    return queue->try_post_async (
      std::move (name),
      [work = std::move (work)] (auto resume) mutable {
          auto pending = std::make_shared<task_t<void>> (work ());
          detail::observe_task_completion (
            *pending, [pending, work = std::move (work),
                       resume = std::move (resume)] (const result_t<void> &terminal) mutable {
                resume ([terminal] { terminal.value (); });
            });
      },
      std::move (options));
}

bool spot_context_state_t::try_post_serial_async (
  std::string name,
  runtime::serial_execution_queue_t::async_work_t work,
  runtime::serial_work_options_t options)
{
    // The caller admitted this work and holds its claim (admit).
    const auto queue = serial_queue;
    if (!queue) {
        work ([] (std::function<void ()> completion) {
            if (completion) {
                completion ();
            }
        });
        return true;
    }
    return queue->try_post_async (std::move (name), std::move (work), std::move (options));
}

void spot_context_state_t::run_serial_task_async (
  std::string name,
  std::function<task_t<void> ()> work,
  std::function<void (result_t<void>)> completion,
  std::function<void (const std::shared_ptr<runtime::serial_execution_queue_t> &,
                      runtime::serial_submission_id_t)> submission_callback,
  std::function<void ()> activation_callback,
  std::function<void (bool)> cancellation_observed,
  std::function<void ()> transfer_owner_reservation,
  std::size_t transferred_owner_byte_cost,
  std::shared_ptr<serial_turn_t> accepted_turn)
{
    transferred_owner_reservation_guard_t owner_reservation (
      std::move (transfer_owner_reservation));
    if (!completion)
        return;
    if (!work) {
        owner_reservation.settle ();
        completion (result_t<void>::success ());
        return;
    }

    if (!accepted_turn && !admit ()) {
        completion (detail::result_access_t::failure<void> (admission_error ()));
        return;
    }
    const auto queue = serial_queue;
    const auto current_turn = detail::capture_current_serial_turn ();
    const bool released_current_turn =
      queue && current_turn && current_turn->released () && current_turn->belongs_to (queue.get ());
    const bool run_inline = !queue || (!released_current_turn && owns_current_serial_turn ());
    if (run_inline) {
        owner_reservation.settle ();
        try {
            if (activation_callback)
                activation_callback ();
            callback_context_scope_t callback_scope (this);
            auto observed = std::make_shared<task_t<void>> (work ());
            // The work object owns the Spot and Actor the callback references
            // until the callback terminal.
            detail::observe_task_completion (*observed, [observed, work = std::move (work),
                                                         completion = std::move (completion)] (
                                                          const result_t<void> &value) mutable {
                completion (value ? result_t<void>::success ()
                                  : result_t<void>::failure (value.error_kind (),
                                                             value.error () != nullptr
                                                               ? value.error ()->what ()
                                                               : "spot lifecycle callback failed"));
            });
        }
        catch (const framework_exception_t &error) {
            completion (detail::result_access_t::failure<void> (error));
        }
        catch (const std::exception &error) {
            completion (
              result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ()));
        }
        catch (...) {
            completion (result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                 "spot lifecycle callback failed"));
        }
        return;
    }

    struct async_state_t
    {
        std::mutex mutex;
        bool started = false;
        bool cancellation_requested = false;
        bool settled = false;
        std::function<void (result_t<void>)> completion;
    };
    auto state = std::make_shared<async_state_t> ();
    state->completion = std::move (completion);
    auto settle = [state] (result_t<void> value) mutable {
        std::function<void (result_t<void>)> callback;
        {
            std::lock_guard lock (state->mutex);
            if (state->settled)
                return;
            state->settled = true;
            if (state->cancellation_requested) {
                value = result_t<void>::failure (
                  framework_error_kind_t::shutting_down,
                  "spot serial lifecycle task was cancelled during shutdown");
            }
            callback = std::move (state->completion);
        }
        if (callback)
            callback (std::move (value));
    };
    auto request_cancel = [state, settle,
                           cancellation_observed = std::move (cancellation_observed)] () mutable {
        bool settle_queued = false;
        {
            std::lock_guard lock (state->mutex);
            if (state->settled)
                return;
            state->cancellation_requested = true;
            settle_queued = !state->started;
            if (cancellation_observed)
                cancellation_observed (state->started);
        }
        if (settle_queued) {
            settle (result_t<void>::failure (
              framework_error_kind_t::shutting_down,
              "spot serial lifecycle task was cancelled before activation"));
        }
    };
    const std::weak_ptr<spot_context_state_t> weak_owner = weak_from_this ();
    runtime::serial_execution_queue_t::async_work_t execute =
      [weak_owner, accepted_turn, work = std::move (work), state, settle,
       activation_callback = std::move (activation_callback)] (auto complete) mutable {
          bool cancelled_before_start = false;
          {
              std::lock_guard lock (state->mutex);
              cancelled_before_start = state->settled || state->cancellation_requested;
              if (!cancelled_before_start)
                  state->started = true;
          }
          if (cancelled_before_start) {
              complete ([] {});
              return;
          }
          if (activation_callback)
              activation_callback ();
          auto owner = weak_owner.lock ();
          if (!owner) {
              complete ([settle] () mutable {
                  settle (result_t<void>::failure (
                    framework_error_kind_t::shutting_down,
                    "spot lifecycle owner was released before activation"));
              });
              return;
          }
          owner->enter_callback ();
          try {
              callback_context_scope_t callback_scope (owner.get ());
              auto observed = std::make_shared<task_t<void>> (work ());
              detail::observe_task_completion (
                *observed, [owner, accepted_turn, observed, work = std::move (work), settle,
                            complete] (const result_t<void> &value) mutable {
                    const auto final_result =
                      value
                        ? result_t<void>::success ()
                        : detail::propagate_failure<void> (value, "spot lifecycle callback failed");
                    auto finish = [owner, settle, final_result] () mutable {
                        owner->leave_callback ();
                        settle (final_result);
                        final_result.value ();
                    };
                    complete (std::move (finish));
                });
          }
          catch (const framework_exception_t &error) {
              complete ([owner, settle, error] () mutable {
                  owner->leave_callback ();
                  settle (detail::result_access_t::failure<void> (error));
                  throw error;
              });
          }
          catch (const std::exception &error) {
              const auto message = std::string (error.what ());
              complete ([owner, settle, message] () mutable {
                  owner->leave_callback ();
                  settle (
                    result_t<void>::failure (framework_error_kind_t::internal_failure, message));
                  throw framework_exception_t (framework_error_kind_t::internal_failure, message);
              });
          }
          catch (...) {
              const auto error = std::current_exception ();
              complete ([owner, settle, error] () mutable {
                  owner->leave_callback ();
                  settle (result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                   "spot lifecycle callback failed"));
                  std::rethrow_exception (error);
              });
          }
      };
    if (accepted_turn) {
        owner_reservation.settle ();
        if (!queue->try_post_continuation (std::move (name), std::move (execute), accepted_turn,
                                           true))
            settle (result_t<void>::failure (framework_error_kind_t::shutting_down,
                                             "Spot continuation executor is stopping"));
        return;
    }
    const auto submitted = queue->try_post_cancellable_async (
      std::move (name), std::move (execute), std::move (request_cancel),
      runtime::serial_work_options_t{.lane = runtime::serial_work_lane_t::lifecycle,
                                     .byte_cost = transferred_owner_byte_cost,
                                     .transfer_owner_reservation = owner_reservation.callback (),
                                     .holds_application_while_waiting = true});
    if (submitted)
        owner_reservation.dismiss ();
    if (submitted && submission_callback)
        submission_callback (queue, submitted.value ());
    if (!submitted) {
        owner_reservation.settle ();
        settle (result_t<void>::failure (
          submitted.error_kind (), submitted.error () != nullptr ? submitted.error ()->what ()
                                                                 : "spot serial queue is closed"));
    }
}

task_t<void> spot_context_state_t::run_serial_task (std::string name,
                                                    std::function<task_t<void> ()> work)
{
    auto completion = std::make_shared<task_completion_source_t<void>> ();
    auto result = completion->task ();
    run_serial_task_async (
      std::move (name), std::move (work),
      [completion] (result_t<void> value) mutable { completion->complete (std::move (value)); });
    return result;
}

bool spot_context_state_t::run_serial_sync (std::string name, std::function<void ()> work)
{
    if (!work) {
        return true;
    }
    if (!admit ()) {
        return false;
    }
    if (is_current_callback_thread () || owns_current_serial_turn ()) {
        callback_context_scope_t callback_scope (this);
        work ();
        return true;
    }

    /* The caller waits for its own lifecycle item, not for the whole queue:
     * an earlier accepted lifecycle operation can keep its FIFO position while
     * it waits for infrastructure, and this item still runs after it. The
     * guard completes the wait when the queue runs or discards the item. */
    struct item_released_t
    {
        std::promise<void> released;
        ~item_released_t () { released.set_value (); }
    };
    auto item_guard = std::make_shared<item_released_t> ();
    auto item_released = item_guard->released.get_future ();
    std::exception_ptr error;
    bool ran = false;
    std::function<void ()> item = [&, item_guard] {
        ran = true;
        enter_callback ();
        try {
            callback_context_scope_t callback_scope (this);
            work ();
        }
        catch (...) {
            error = std::current_exception ();
        }
        leave_callback ();
    };
    const auto queue = serial_queue;
    bool posted = true;
    if (queue)
        posted =
          queue->try_post (std::move (name), std::move (item),
                           runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle});
    else
        item ();
    item = {};
    item_guard.reset ();
    item_released.wait ();
    if (!posted) {
        return false;
    }
    if (error) {
        std::rethrow_exception (error);
    }
    return ran;
}

bool spot_context_state_t::owns_current_serial_turn () const
{
    const auto turn = detail::capture_current_serial_turn ();
    const auto queue = serial_queue;
    return turn && queue && turn->belongs_to (queue.get ());
}

void spot_context_state_t::defer_relocation_ready ()
{
    if (execution_mode != user_spot_execution_mode_t::spot_wide
        || relocation_coordination_mode != spot_relocation_coordination_mode_t::application_signaled
        || is_entry_spot () || is_instance_spot () || !owns_current_serial_turn ()) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "relocation readiness can be deferred only from an "
                                     "application-signaled SpotWide User Spot turn");
    }
    if (state_sync ([this] { return relocation_ready_deferred; })) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "relocation readiness is already deferred");
    }
    // Collect membership when this lifecycle item reaches its FIFO position:
    // earlier Join callbacks have settled and later ones remain behind it.
    auto reserved =
      serial_queue->reserve_barrier_next ("relocation-ready", true, [self = shared_from_this ()] {
          return self->state_lane_owner ()->lane.run_task ([self] {
              std::vector<std::shared_ptr<runtime::serial_execution_queue_t>> queues;
              const auto coordinator = self->ensure_spot_serial_executor ();
              for (const auto &[key, membership] : self->node->actor_spot_ids) {
                  if (membership == self->spot_id)
                      queues.push_back (coordinator->actor_executor (key)->queue ());
              }
              return queues;
          });
      });
    if (!reserved) {
        throw framework_exception_t (reserved.error_kind (),
                                     "relocation readiness barrier queue is closed");
    }
    const auto registering_turn = detail::capture_current_serial_turn ();
    const auto complete_without_relocation =
      state_sync ([this, barrier = reserved.value (), registering_turn] {
          relocation_ready_deferred = true;
          relocation_ready_barrier = barrier;
          relocation_ready_turn = registering_turn;
          return !relocation_boundary_active;
      });
    if (complete_without_relocation)
        complete_relocation_ready (spot_relocation_ready_outcome_t::continued);
}

void spot_context_state_t::ensure_relocation_turn_open () const
{
    const auto current_turn = detail::capture_current_serial_turn ();
    if (!owns_current_serial_turn () || !current_turn || current_turn->is_after_active_phase ())
        return;
    const auto deferring_turn = state_sync ([this] { return relocation_ready_turn.lock (); });
    if (deferring_turn == current_turn) {
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "Framework operations are not allowed after relocation "
                                     "readiness is deferred in the current Spot turn");
    }
}

void spot_context_state_t::complete_relocation_ready (spot_relocation_ready_outcome_t outcome)
{
    auto barrier = state_sync ([this] {
        if (!relocation_ready_deferred)
            return std::shared_ptr<deferred_barrier_t>{};
        return std::exchange (relocation_ready_barrier, {});
    });
    const auto complete = [this, outcome] {
        std::shared_ptr<void> instance;
        std::function<void (void *, const spot_relocation_ready_completion_t &)> callback;
        std::tie (instance, callback) = state_sync ([this] {
            std::shared_ptr<void> current_instance;
            std::function<void (void *, const spot_relocation_ready_completion_t &)>
              current_callback;
            if (!relocation_ready_deferred)
                return std::make_pair (std::move (current_instance), std::move (current_callback));
            relocation_ready_deferred = false;
            relocation_ready_turn.reset ();
            current_instance = spot_instance;
            current_callback = lifecycle.on_relocation_ready_completed;
            return std::make_pair (std::move (current_instance), std::move (current_callback));
        });
        if (instance && callback)
            callback (instance.get (), spot_relocation_ready_completion_t{outcome});
    };
    if (barrier) {
        const auto activated = barrier->activate_async (
          [complete] (deferred_barrier_t::async_completion_t release) mutable {
              try {
                  complete ();
                  release (result_t<void>::success ());
              }
              catch (const framework_exception_t &error) {
                  release (detail::result_access_t::failure<void> (error));
              }
              catch (const std::exception &error) {
                  release (result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                    error.what ()));
              }
              catch (...) {
                  release (
                    result_t<void>::failure (framework_error_kind_t::internal_failure,
                                             "relocation readiness completion callback failed"));
              }
          });
        if (!activated) {
            throw framework_exception_t (activated.error_kind (),
                                         "relocation readiness barrier is already terminal");
        }
        return;
    }
    (void) run_serial_sync ("relocation-ready-completed", complete);
}

void spot_context_state_t::drain_serial ()
{
    if (const auto queue = serial_queue) {
        queue->drain ();
    }
}

task_t<std::shared_ptr<runtime::serial_execution_queue_t>>
spot_node_runtime_t::spot_execution_queue (std::string spot_key) const
{
    return _state->lane.run_task ([state = _state, key = std::move (spot_key)] {
        const auto found = state->spot_contexts_by_id.find (key);
        const auto context = found == state->spot_contexts_by_id.end ()
                               ? nullptr
                               : spot_context_access_t::state (found->second);
        return context ? context->serial_queue
                       : std::shared_ptr<runtime::serial_execution_queue_t>{};
    });
}

task_t<std::shared_ptr<runtime::serial_application_target_t>>
spot_node_runtime_t::prepare_spot_dispatch (
  std::string key, std::optional<runtime::protocol::spot_route_fence_t> route) const
{
    auto state = _state;
    co_return co_await state->lane.run_task ([state, key = std::move (key),
                                              route = std::move (route)] {
        const auto found = state->spot_contexts_by_id.find (key);
        const auto context = found == state->spot_contexts_by_id.end ()
                               ? nullptr
                               : spot_context_access_t::state (found->second);
        if (!context)
            throw detail::make_framework_origin_exception (
              framework_error_kind_t::unavailable, "Spot route owner is no longer registered");
        if (route) {
            const auto lifecycle = context->node ? context->node->location_lifecycle : nullptr;
            const auto queue = context->ensure_spot_serial_executor ()->spot_queue ();
            const auto lease = lifecycle
                                 ? lifecycle->execution_runtime ().bound_execution_owner (*queue)
                                 : std::optional<location_owner_token_t>{};
            if (!context->accepts_route_fence (*route, lease))
                throw detail::make_framework_origin_exception (framework_error_kind_t::unavailable,
                                                               "Spot route fence is stale");
        }
        (void) context->ensure_spot_serial_executor ();
        auto target = std::make_shared<runtime::serial_application_target_t> ();
        if (!context->admit (true))
            throw context->admission_error ();
        target->queue = context->serial_queue;
        target->context = context;
        target->instance = context->spot_instance;
        target->terminal = [state, context] {
            if (state->lane.is_on_lane ()) {
                context->leave_callback ();
            } else if (!state->lane.try_post ([context] { context->leave_callback (); })) {
                state->runtime_failures ()->report (std::make_exception_ptr (
                  std::runtime_error ("Spot admission owner lane is closed")));
            }
        };
        return target;
    });
}

} // namespace detail

node_rid_t::node_rid_t (std::string value) : _value (std::move (value))
{
}

node_rid_t node_rid_t::from_string (std::string value)
{
    return node_rid_t (std::move (value));
}

std::string_view node_rid_t::value () const noexcept
{
    return _value;
}

bool node_rid_t::empty () const noexcept
{
    return _value.empty ();
}

spot_context_t::erased_request_call_t::erased_request_call_t (framework_exception_t error) :
    _error (std::move (error))
{
}

spot_context_t::erased_request_call_t::erased_request_call_t (
  std::string packet_name,
  serializer_registry_t *serializers,
  std::function<request_task_t<zlink::message_t> (
    const std::string &,
    std::chrono::milliseconds,
    const request_call_t<zlink::message_t>::metadata_map_t &)> submit,
  request_call_t<zlink::message_t>::preflight_fn_t preflight) :
    _packet_name (std::move (packet_name)),
    _serializers (serializers),
    _submit (std::move (submit)),
    _preflight (std::move (preflight))
{
}

spot_context_t::spot_context_t () : _state (std::make_shared<detail::spot_context_state_t> ())
{
}

spot_context_t::spot_context_t (std::shared_ptr<detail::spot_context_state_t> state) :
    _state (std::move (state))
{
    if (_state) {
        _worker_scheduler = _state->worker_scheduler;
    }
}

spot_context_t::~spot_context_t () = default;
spot_context_t::spot_context_t (spot_context_t &&) noexcept = default;

spot_relocation_ready_call_t::spot_relocation_ready_call_t (
  std::shared_ptr<detail::spot_context_state_t> state) :
    _state (std::move (state))
{
}

spot_relocation_ready_call_t::~spot_relocation_ready_call_t () = default;
spot_relocation_ready_call_t::spot_relocation_ready_call_t (
  spot_relocation_ready_call_t &&) noexcept = default;

void spot_relocation_ready_call_t::defer ()
{
    if (!_state) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "relocation readiness call is not bound to a Spot");
    }
    _state->defer_relocation_ready ();
}

entry_spot_context_t::entry_spot_context_t () = default;

entry_spot_context_t::entry_spot_context_t (const spot_context_t &context) :
    spot_context_t (context._state)
{
}

entry_spot_context_t::entry_spot_context_t (std::shared_ptr<detail::spot_context_state_t> state) :
    spot_context_t (std::move (state))
{
}

entry_spot_context_t::~entry_spot_context_t () = default;
entry_spot_context_t::entry_spot_context_t (entry_spot_context_t &&) noexcept = default;

instance_spot_context_t::instance_spot_context_t () = default;

instance_spot_context_t::instance_spot_context_t (const spot_context_t &context) :
    spot_context_t (context._state)
{
}

instance_spot_context_t::instance_spot_context_t (
  std::shared_ptr<detail::spot_context_state_t> state) :
    spot_context_t (std::move (state))
{
}

instance_spot_context_t::~instance_spot_context_t () = default;
instance_spot_context_t::instance_spot_context_t (instance_spot_context_t &&) noexcept = default;

node_rid_t spot_context_t::node_rid () const
{
    return _state->node_rid;
}

std::string_view spot_context_t::mesh_name () const
{
    return _state->mesh_name;
}

spot_id_t spot_context_t::spot_id () const
{
    return _state->spot_id;
}

std::uint64_t spot_context_t::object_generation () const noexcept
{
    return _state ? _state->object_generation : 0;
}

bool spot_context_t::has_same_source_fence (const spot_context_t &other) const noexcept
{
    return _state && _state == other._state;
}

std::string spot_context_t::spot_name () const
{
    return _state->spot_name;
}

spot_handler_registry_t spot_context_t::handlers ()
{
    return spot_handler_registry_t (_state);
}

spot_relocation_ready_call_t spot_context_t::relocation_ready ()
{
    return spot_relocation_ready_call_t (_state);
}

spot_manager_t spot_context_t::manager () const
{
    return spot_manager_t (_state->node, _state);
}

route_client_t spot_context_t::spot_route_client () const
{
    if (!_state || !_state->node) {
        return route_client_t ();
    }
    const auto route_client =
      std::atomic_load_explicit (&_state->node->route_client, std::memory_order_acquire);
    return route_client ? *route_client : route_client_t ();
}

channel_client_t spot_context_t::outbound () const
{
    if (!_state->channel_runtime) {
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "SPOT channel outbound runtime is not configured");
    }
    auto state = std::weak_ptr<detail::spot_context_state_t> (_state);
    return channel_client_t (message_bus_t (_state->channel_runtime, [state] {
        const auto locked = state.lock ();
        if (!locked)
            return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                            "Spot execution context is no longer available");
        try {
            locked->ensure_relocation_turn_open ();
            return result_t<void>::success ();
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<void> (error);
        }
    }));
}

void spot_context_t::ensure_submission_open () const
{
    if (!_state) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Spot execution context is not configured");
    }
    _state->ensure_relocation_turn_open ();
}

std::function<result_t<void> ()> spot_context_t::submission_preflight () const
{
    auto state = std::weak_ptr<detail::spot_context_state_t> (_state);
    return [state] {
        const auto locked = state.lock ();
        if (!locked) {
            return result_t<void>::failure (framework_error_kind_t::not_configured,
                                            "Spot execution context is no longer available");
        }
        try {
            locked->ensure_relocation_turn_open ();
            return result_t<void>::success ();
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<void> (error);
        }
    };
}

task_t<bool> spot_context_t::close ()
{
    return close_erased ();
}

task_t<bool> spot_context_t::close_erased ()
{
    if (!_state || !_state->node)
        return task_t<bool> (result_t<bool>::success (false));
    const auto state = _state;
    state->ensure_relocation_turn_open ();
    if (state->serial_queue && state->serial_queue->is_current_lifecycle_turn ())
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "A Spot cannot close itself from its lifecycle callback.");
    auto completion = std::make_shared<task_completion_source_t<bool>> ();
    auto closed = completion->task ();
    state->request_close (
      {}, [completion] (result_t<bool> result) { completion->complete (std::move (result)); });
    return closed;
}

void detail::spot_context_state_t::cancel_timers () noexcept
{
    detail::timer_runtime_t::cancel_all (*this);
}

serializer_registry_t *spot_context_t::serializer_registry () const noexcept
{
    if (!_state || !_state->channel_runtime) {
        return nullptr;
    }
    return _state->channel_runtime->serializers;
}

send_call_t spot_context_t::send_to_erased (node_rid_t node_rid,
                                            spot_id_t spot_id,
                                            std::string packet_name,
                                            zlink::message_t payload)
{
    if (node_rid.empty () || spot_id.empty ()) {
        return send_call_t (result_t<void>::failure (framework_error_kind_t::not_found,
                                                     "target spot route is empty"));
    }
    auto state = _state;
    return send_call_t (
      std::move (packet_name),
      [state, node_rid = std::move (node_rid), spot_id = std::move (spot_id),
       payload = std::move (payload)] (
        const std::string &submitted_packet_name,
        const send_call_t::metadata_map_t &metadata) mutable -> task_t<void> {
          if (!state) {
              throw framework_exception_t (framework_error_kind_t::protocol_error,
                                           "SPOT context is not configured");
          }
          try {
              state->ensure_relocation_turn_open ();
          }
          catch (const framework_exception_t &error) {
              throw error;
          }
          if (auto route_channel_name = optional_spot_route_channel_name (state)) {
              detail::channel_runtime_manager_t manager (state->channel_runtime);
              auto &runtime = manager.get_route_channel (*route_channel_name);
              auto parts = encode_spot_route_parts (
                runtime::messaging::message_kind_t::command, *route_channel_name,
                submitted_packet_name, payload, std::chrono::milliseconds::zero (), metadata);
              co_await runtime.submit_spot_send_parts (
                zlink::routing_id_t::from (std::string (node_rid.value ())), spot_id,
                std::move (parts));
              co_return;
          }
          auto native = state->native_spot.lock ();
          if (!native) {
              throw framework_exception_t (framework_error_kind_t::not_found,
                                           "SPOT mesh route requires a running native Spot");
          }
          try {
              const auto channel_name = spot_mesh_channel_name (state);
              auto parts = encode_spot_route_parts (runtime::messaging::message_kind_t::command,
                                                    channel_name, submitted_packet_name, payload,
                                                    std::chrono::milliseconds::zero (), metadata);
              auto native_parts = parts.items ();
              if (native_parts.empty ()) {
                  throw framework_exception_t (framework_error_kind_t::protocol_error,
                                               "SPOT mesh send requires at least one message part");
              }
              const auto target_node_rid =
                zlink::routing_id_t::from (std::string (node_rid.value ()));
              const auto target_spot_id = spot_id;
              const auto target_generation = co_await resolve_target_spot_generation (
                state->node, target_node_rid, target_spot_id);
              if (!target_generation) {
                  throw framework_exception_t (framework_error_kind_t::not_found,
                                               "SPOT mesh send target generation is unavailable");
              }
              const auto submitted = co_await native->send_to_spot (
                target_node_rid, target_spot_id, *target_generation, native_parts);
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
      });
}

spot_context_t::erased_request_call_t spot_context_t::request_to_erased (node_rid_t node_rid,
                                                                         spot_id_t spot_id,
                                                                         std::string packet_name,
                                                                         zlink::message_t payload)
{
    if (node_rid.empty () || spot_id.empty ()) {
        return erased_request_call_t (
          framework_exception_t (framework_error_kind_t::not_found, "target spot route is empty"));
    }
    auto state = _state;
    const auto target_spot_id = spot_id;
    return erased_request_call_t (
      std::move (packet_name), serializer_registry (),
      [state, node_rid = std::move (node_rid), spot_id = std::move (spot_id),
       payload = std::move (payload)] (
        const std::string &submitted_packet_name, std::chrono::milliseconds timeout,
        const request_call_t<zlink::message_t>::metadata_map_t &metadata) mutable
      -> request_task_t<zlink::message_t> {
          if (!state) {
              return request_task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
                framework_error_kind_t::protocol_error, "SPOT context is not configured"));
          }
          const auto request_started = std::chrono::steady_clock::now ();
          try {
              if (auto route_channel_name = optional_spot_route_channel_name (state)) {
                  detail::channel_runtime_manager_t manager (state->channel_runtime);
                  auto &runtime = manager.get_route_channel (*route_channel_name);
                  const auto effective_timeout = timeout > std::chrono::milliseconds::zero ()
                                                   ? timeout
                                                   : runtime.default_request_timeout ();
                  auto parts = encode_spot_route_parts (runtime::messaging::message_kind_t::request,
                                                        *route_channel_name, submitted_packet_name,
                                                        payload, effective_timeout, metadata);
                  auto pending = runtime.request_reply_spot_parts (
                    zlink::routing_id_t::from (std::string (node_rid.value ())), spot_id,
                    std::move (parts), request_started + effective_timeout);
                  return detail::project_request_reply<zlink::message_t> (
                    pending,
                    [state, submitted_packet_name, route_channel_name = *route_channel_name,
                     spot_id] (const result_t<runtime::messaging::message_parts_t> &reply)
                      -> result_t<zlink::message_t> {
                        if (!reply) {
                            if (reply.error_kind () == framework_error_kind_t::not_found) {
                                report_spot_dispatch_error (
                                  state->node, dispatch_error_surface_t::spot_route,
                                  dispatch_message_kind_t::request,
                                  dispatch_error_reason_t::handler_missing,
                                  dispatch_error_action_t::reply_error, submitted_packet_name,
                                  route_channel_name, std::string (spot_id), std::nullopt,
                                  reply.error () ? std::make_exception_ptr (*reply.error ())
                                                 : std::exception_ptr{});
                            }
                            return detail::propagate_failure<zlink::message_t> (
                              reply, "SPOT route request failed");
                        }
                        runtime::messaging::envelope_codec_t envelope;
                        auto body = envelope.decode_body (reply.value ());
                        if (!body) {
                            return result_t<zlink::message_t>::failure (
                              body.error_kind (), body.error ()
                                                    ? body.error ()->what ()
                                                    : "SPOT route reply body decode failed");
                        }
                        return result_t<zlink::message_t>::success (body.value ());
                    });
              }
              auto native = state->native_spot.lock ();
              if (!native) {
                  return request_task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::not_found,
                    "SPOT mesh route requires a running native Spot"));
              }
              const auto effective_timeout =
                timeout > std::chrono::milliseconds::zero () ? timeout : std::chrono::seconds (30);
              const auto channel_name = spot_mesh_channel_name (state);
              auto parts = encode_spot_route_parts (runtime::messaging::message_kind_t::request,
                                                    channel_name, submitted_packet_name, payload,
                                                    effective_timeout, metadata);
              return request_spot_mesh_message (state, std::move (node_rid), std::move (spot_id),
                                                std::move (parts), effective_timeout);
          }
          catch (const framework_exception_t &error) {
              return request_task_t<zlink::message_t> (
                detail::result_access_t::failure<zlink::message_t> (error));
          }
      },
      [state, target_spot_id] (bool release_turn) {
          if (state) {
              try {
                  state->ensure_relocation_turn_open ();
              }
              catch (const framework_exception_t &error) {
                  return detail::result_access_t::failure<void> (error);
              }
          }
          if (!release_turn && state && state->spot_id == target_spot_id
              && state->owns_current_serial_turn ()) {
              return result_t<void>::failure (
                framework_error_kind_t::invalid_operation,
                "awaited request requires the current Spot execution gate");
          }
          return result_t<void>::success ();
      });
}

spot_context_t &spot_context_t::register_packet_erased (std::string packet_name,
                                                        std::type_index payload_type)
{
    const auto duplicate = std::any_of (_state->packets.begin (), _state->packets.end (),
                                        [&] (const spot_packet_descriptor_t &descriptor) {
                                            return descriptor.packet_name == packet_name;
                                        });
    if (duplicate) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "duplicate spot packet registration");
    }
    _state->packets.push_back (spot_packet_descriptor_t{std::move (packet_name), payload_type});
    return *this;
}

std::vector<spot_packet_descriptor_t> spot_context_t::packet_registry () const
{
    return _state->packets;
}

spot_handler_registry_t::spot_handler_registry_t () :
    _state (std::make_shared<detail::spot_context_state_t> ())
{
}

spot_handler_registry_t::spot_handler_registry_t (
  std::shared_ptr<detail::spot_context_state_t> state) :
    _state (std::move (state))
{
}

spot_handler_registry_t::~spot_handler_registry_t () = default;
spot_handler_registry_t::spot_handler_registry_t (spot_handler_registry_t &&) noexcept = default;
spot_handler_registry_t &
spot_handler_registry_t::operator= (spot_handler_registry_t &&) noexcept = default;

spot_handler_registry_t &spot_handler_registry_t::add_handler_erased (spot_handler_kind_t kind,
                                                                      std::string packet_name,
                                                                      std::string topic,
                                                                      std::type_index handler_type,
                                                                      std::type_index payload_type,
                                                                      std::type_index actor_type,
                                                                      std::type_index reply_type,
                                                                      invoker_t invoker,
                                                                      std::string channel_name)
{
    if (kind == spot_handler_kind_t::subscription
        && (channel_name.empty () || is_blank (channel_name))) {
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "SPOT subscription channel name is required");
    }
    if (kind == spot_handler_kind_t::subscription && topic.empty ()) {
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "SPOT subscription handler topic is required for '"
                                       + std::string (handler_type.name ()) + "' (packet '"
                                       + packet_name + "')");
    }
    const auto duplicate = find_handler (kind, packet_name, topic, actor_type, channel_name);
    if (duplicate) {
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "duplicate spot handler registration");
    }

    if (kind == spot_handler_kind_t::subscription) {
        const auto native =
          _state->node ? _state->node->lane.run ([&] { return _state->native_spot.lock (); }).get ()
                       : std::shared_ptr<service::spot_t>{};
        if (native) {
            native->set_subscription (channel_name, topic);
        }
    }
    _state->handlers.push_back (
      spot_handler_descriptor_t{kind, std::move (packet_name), std::move (topic), handler_type,
                                payload_type, actor_type, reply_type, std::move (channel_name)});
    _state->handler_invokers.push_back (std::move (invoker));
    return *this;
}

void spot_handler_registry_t::register_actor_admission_erased (
  std::type_index actor_type, detail::spot_actor_admission_callbacks_t callbacks)
{
    const bool registered = _state->actor_admissions.contains (actor_type);
    _state->actor_admissions[actor_type] = std::move (callbacks);
    if (!registered) {
        const auto owner = std::weak_ptr<detail::spot_node_builder_state_t> (_state->node);
        for (const auto kind :
             {spot_handler_kind_t::actor_request, spot_handler_kind_t::actor_send})
            add_handler_erased (
              kind, runtime::protocol::session_disconnected_packet_name, {},
              std::type_index (typeid (void)), std::type_index (typeid (zlink::message_t)),
              actor_type, std::type_index (typeid (zlink::message_t)),
              [owner, actor_type] (void *, void *actor, service_provider_t &services,
                                   serializer_registry_t &, const zlink::message_t &body,
                                   const spot_inbound_message_t &) {
                  return detail::spot_node_runtime_t (owner.lock ())
                    .notify_actor_disconnected_erased (
                      actor, actor_type, services.get_required<detail::actor_gateway_runtime_t> (),
                      body);
              });
    }
}

std::vector<spot_handler_descriptor_t> spot_handler_registry_t::descriptors () const
{
    return _state->handlers;
}

spot_handler_kind_t
spot_handler_registry_t::resolve_actor_packet_kind (std::string_view packet_name,
                                                    std::type_index actor_type) const
{
    for (const auto &descriptor : _state->handlers) {
        if (descriptor.kind == spot_handler_kind_t::actor_request
            && descriptor.packet_name == packet_name && descriptor.actor_type == actor_type) {
            return spot_handler_kind_t::actor_request;
        }
    }
    for (const auto &descriptor : _state->handlers) {
        if (descriptor.kind == spot_handler_kind_t::actor_send
            && descriptor.packet_name == packet_name && descriptor.actor_type == actor_type) {
            return spot_handler_kind_t::actor_send;
        }
    }
    return spot_handler_kind_t::actor_request;
}

std::optional<std::size_t>
spot_handler_registry_t::find_handler (spot_handler_kind_t kind,
                                       std::string_view packet_name,
                                       std::string_view topic,
                                       std::type_index actor_type,
                                       std::string_view channel_name) const
{
    for (std::size_t index = 0; index < _state->handlers.size (); ++index) {
        const auto &descriptor = _state->handlers[index];
        if (descriptor.kind == kind && descriptor.packet_name == packet_name
            && descriptor.topic == topic && descriptor.actor_type == actor_type
            && descriptor.channel_name == channel_name)
            return index;
    }
    return std::nullopt;
}

task_t<zlink::message_t>
spot_handler_registry_t::invoke_erased (spot_handler_kind_t kind,
                                        std::string_view packet_name,
                                        std::string_view topic,
                                        std::type_index actor_type,
                                        void *spot,
                                        void *actor,
                                        service_provider_t &services,
                                        serializer_registry_t &serializers,
                                        zlink::message_t message,
                                        spot_inbound_message_t metadata) const
{
    return invoke_erased ({}, kind, packet_name, topic, actor_type, spot, actor, services,
                          serializers, std::move (message), std::move (metadata));
}

task_t<zlink::message_t> spot_handler_registry_t::invoke_erased (
  runtime::handler_entry_t before_application_handler,
  spot_handler_kind_t kind,
  std::string_view packet_name,
  std::string_view topic,
  std::type_index actor_type,
  void *spot,
  void *actor,
  service_provider_t &services,
  serializer_registry_t &serializers,
  zlink::message_t message,
  spot_inbound_message_t metadata,
  bool serial_dispatch,
  std::string actor_execution_key,
  std::string actor_execution_spot_id,
  std::function<std::optional<result_t<zlink::message_t>> (
    const zlink::message_t &, const spot_inbound_message_t &)> before_invoke,
  actor_queue_dispatch_t actor_queue_dispatch,
  std::function<void ()> after_application_admission,
  std::function<void ()> transfer_owner_reservation,
  std::size_t transferred_owner_byte_cost,
  std::function<void ()> application_admission_terminal,
  std::function<std::optional<bool> (std::function<bool ()>)> admit_actor_queue,
  std::shared_ptr<const void> retained_message,
  std::shared_ptr<runtime::serial_application_record_t> application_record,
  std::function<void ()> on_application_export,
  std::string_view actor_execution_id) const
{
    const auto selected = find_handler (
      kind, packet_name, topic, actor_type,
      metadata.channel_name ? std::string_view (*metadata.channel_name) : std::string_view{});
    const auto retained_trace_owner = _state->node
                                        ? std::shared_ptr<detail::spot_node_builder_state_t>{}
                                        : _state->state_lane_owner ();
    const auto &trace_owner = _state->node ? _state->node : retained_trace_owner;
    if (selected) {
        const auto handler_index = *selected;
        const auto trace_message_kind = kind == spot_handler_kind_t::actor_send
                                          ? dispatch_message_kind_t::actor_send
                                          : dispatch_message_kind_t::actor_request;
        std::string trace_packet_name;
        std::string trace_actor_id;
        std::string trace_spot_id;
        if (trace_owner) {
            if (detail::message_flow_tracer_t (trace_owner->dispatch)
                  .enabled (message_flow_log_mode_t::detailed)) {
                detail::message_flow_tracer_t (trace_owner->dispatch)
                  .trace (message_flow_log_mode_t::detailed, message_flow_outcome_t::received, [&] {
                      trace_packet_name = packet_name;
                      trace_actor_id = actor_execution_id;
                      trace_spot_id = actor_execution_spot_id;
                      return message_flow_event_t{
                        .outcome = message_flow_outcome_t::received,
                        .surface = dispatch_error_surface_t::spot_actor,
                        .message_kind = trace_message_kind,
                        .packet_name = trace_packet_name,
                        .spot_id = trace_spot_id.empty () ? std::nullopt
                                                          : std::make_optional (trace_spot_id),
                        .actor_id = trace_actor_id.empty () ? std::nullopt
                                                            : std::make_optional (trace_actor_id),
                        .detail_stage = "invoke_erased.enter",
                        .detail_result =
                          std::string ("serial_dispatch=") + (serial_dispatch ? "true" : "false")};
                  });
            }
        }
        std::string callback_spot_id;
        if (trace_owner && trace_owner->handler_invocation_for_tests) {
            callback_spot_id = actor_execution_spot_id;
            trace_owner->handler_invocation_for_tests (
              detail::handler_invocation_phase_t::before_admission, packet_name,
              actor_execution_spot_id, actor_execution_id);
        }
        auto completion = std::make_shared<spot_handler_terminal_t> ();
        if (application_record && before_application_handler)
            application_record->admission.retain_initial (std::move (before_application_handler));
        auto task = completion->task ();
        if (retained_message) {
            const auto original =
              std::static_pointer_cast<const detail::instance_spot_retained_message_t> (
                retained_message);
            original->completion = completion;
        }
        auto state = _state;
        const auto coordinator = state->ensure_spot_serial_executor ();
        const bool requires_spot_serial = coordinator && coordinator->uses_spot_execution_gate ();
        if (actor_queue_dispatch == actor_queue_dispatch_t::acquire && !actor_execution_key.empty ()
            && coordinator) {
            serial_dispatch = true;
        }
        if (trace_owner
            && detail::message_flow_tracer_t (trace_owner->dispatch)
                 .enabled (message_flow_log_mode_t::detailed)) {
            report_actor_dispatch_stage_trace_lazy (
              trace_owner, message_flow_outcome_t::received, trace_message_kind, trace_packet_name,
              trace_spot_id, trace_actor_id, "invoke_erased.queue_select", [&] {
                  return std::string ("actor_queue=coordinator")
                         + " spot_serial=" + (requires_spot_serial ? "true" : "false");
              });
        }
        if (!serial_dispatch) {
            const bool admission_preclaimed = static_cast<bool> (application_admission_terminal);
            if (!admission_preclaimed
                && !state->admit (true,
                                  static_cast<const detail::instance_spot_retained_message_t *> (
                                    retained_message.get ()))) {
                return task_t<zlink::message_t> (
                  detail::result_access_t::failure<zlink::message_t> (state->admission_error ()));
            }
            completion->release_admission =
              admission_preclaimed ? std::move (application_admission_terminal)
                                   : std::function<void ()>{[state] { state->leave_callback (); }};
            const auto admission_terminal = completion;
            try {
                detail::callback_context_scope_t callback_scope (state.get ());
                std::shared_ptr<std::pair<zlink::message_t, spot_inbound_message_t>> carrier;
                if (application_record) {
                    application_record->set_invocation (std::move (message), std::move (metadata));
                    carrier = {application_record, &*application_record->invocation ()};
                } else {
                    completion->carrier.emplace (std::move (message), std::move (metadata));
                    carrier = {completion, &*completion->carrier};
                }
                runtime::actor_execution_scope_t actor_execution (
                  std::move (actor_execution_key), std::move (actor_execution_spot_id));
                if (after_application_admission)
                    after_application_admission ();
                if (application_record)
                    application_record->retire_invocation = {};
                if (application_record)
                    application_record->admission.enter_handler ();
                else if (before_application_handler)
                    before_application_handler ();
                auto handler_task = state->handler_invokers[handler_index](
                  spot, actor, services, serializers, carrier->first, carrier->second);
                detail::observe_task_completion (
                  handler_task, [state, completion, carrier, admission_terminal, trace_message_kind,
                                 trace_packet_name, trace_spot_id, trace_actor_id] (
                                  const result_t<zlink::message_t> &result) mutable {
                      if (state->node
                          && detail::message_flow_tracer_t (state->node->dispatch)
                               .enabled (message_flow_log_mode_t::detailed)) {
                          report_actor_dispatch_stage_trace_lazy (
                            state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                            trace_packet_name, trace_spot_id, trace_actor_id,
                            "invoke_erased.application_handler", [&] {
                                return std::string ("result=finished success=")
                                       + (result ? "true" : "false");
                            });
                      }
                      settle_handler_admission_once (admission_terminal);
                      if (result) {
                          completion->complete (
                            result_t<zlink::message_t>::success (result.value ()));
                          return;
                      }
                      completion->complete (result_t<zlink::message_t>::failure (
                        result.error_kind (), result.error () != nullptr ? result.error ()->what ()
                                                                         : "spot handler failed"));
                  });
            }
            catch (const framework_exception_t &error) {
                report_actor_dispatch_stage_trace (
                  state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                  trace_packet_name, trace_spot_id, trace_actor_id,
                  "invoke_erased.application_handler", "result=finished success=false");
                settle_handler_admission_once (admission_terminal);
                completion->complete (detail::result_access_t::failure<zlink::message_t> (error));
            }
            catch (const std::exception &error) {
                report_actor_dispatch_stage_trace (
                  state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                  trace_packet_name, trace_spot_id, trace_actor_id,
                  "invoke_erased.application_handler", "result=finished success=false");
                settle_handler_admission_once (admission_terminal);
                completion->complete (result_t<zlink::message_t>::failure (
                  framework_error_kind_t::internal_failure, error.what ()));
            }
            catch (...) {
                report_actor_dispatch_stage_trace (
                  state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                  trace_packet_name, trace_spot_id, trace_actor_id,
                  "invoke_erased.application_handler", "result=finished success=false");
                settle_handler_admission_once (admission_terminal);
                completion->complete (result_t<zlink::message_t>::failure (
                  framework_error_kind_t::internal_failure, "spot handler threw an exception"));
            }
            return task;
        }
        const bool has_transferred_owner_reservation =
          static_cast<bool> (transfer_owner_reservation);
        runtime::serial_work_options_t work_options{
          runtime::serial_work_lane_t::application, transferred_owner_byte_cost,
          admit_actor_queue ? std::function<void ()>{} : transfer_owner_reservation};
        work_options.retained_message = retained_message;
        std::shared_ptr<std::pair<zlink::message_t, spot_inbound_message_t>> carrier;
        if (application_record) {
            application_record->set_invocation (std::move (message), std::move (metadata));
            carrier = {application_record, &*application_record->invocation ()};
        } else {
            completion->carrier.emplace (std::move (message), std::move (metadata));
            carrier = {completion, &*completion->carrier};
        }
        if (application_record)
            work_options.application_record = application_record;
        auto dispatch_flow = runtime::flow_context_t::current ();
        auto trace_serial_queue = coordinator ? coordinator->spot_queue () : state->serial_queue;
        std::function<bool (std::string, runtime::serial_execution_queue_t::async_work_t,
                            runtime::serial_work_options_t)>
          post_serial;
        if (coordinator && !actor_execution_key.empty ()) {
            const auto current_turn = actor_queue_dispatch == actor_queue_dispatch_t::current_turn;
            // Executor preparation may enter the node lane. Complete it before
            // the transfer coordinator atomically retains or posts this packet.
            const auto executor = current_turn
                                    ? detail::spot_serial_executor_t::actor_executor_ptr_t{}
                                    : coordinator->prepare_actor_executor (actor_execution_key);
            post_serial = [coordinator, executor, completion, current_turn] (
                            std::string name, runtime::serial_execution_queue_t::async_work_t work,
                            runtime::serial_work_options_t options) {
                auto rejected = [completion] {
                    completion->complete (result_t<zlink::message_t>::failure (
                      framework_error_kind_t::shutting_down,
                      "spot serial queue is closed or stopping"));
                };
                return coordinator->execute_actor (executor, std::move (name), std::move (work),
                                                   std::move (options), current_turn,
                                                   std::move (rejected));
            };
        } else {
            post_serial = [state] (std::string name,
                                   runtime::serial_execution_queue_t::async_work_t work,
                                   runtime::serial_work_options_t options) {
                return state->try_post_serial_async (std::move (name), std::move (work),
                                                     std::move (options));
            };
        }
        const bool admission_preclaimed = static_cast<bool> (application_admission_terminal);
        // The work is admitted here, before its queue: a turn that starts
        // after the seal still runs it (Spot address messaging §7 step 2).
        if (!admission_preclaimed
            && !state->admit (true, static_cast<const detail::instance_spot_retained_message_t *> (
                                      retained_message.get ()))) {
            return task_t<zlink::message_t> (
              detail::result_access_t::failure<zlink::message_t> (state->admission_error ()));
        }
        completion->release_admission =
          admission_preclaimed ? std::move (application_admission_terminal)
                               : std::function<void ()>{[state] { state->leave_callback (); }};
        const auto admission_terminal = completion;
        if (application_record) {
            application_record->retire_invocation =
              [completion, admission_terminal,
               on_application_export =
                 std::move (on_application_export)] (result_t<void> terminal) {
                  if (terminal && on_application_export)
                      on_application_export ();
                  settle_handler_admission_once (admission_terminal);
                  completion->complete (terminal
                                          ? result_t<zlink::message_t>::success (zlink::message_t{})
                                          : detail::propagate_failure<zlink::message_t> (
                                              terminal, "Actor application queue stopped"));
              };
        }
        completion->before_application_handler = std::move (before_application_handler);
        runtime::serial_execution_queue_t::async_work_t work =
          [state, handler_index, spot, actor, &services, &serializers,
           carrier = std::move (carrier), completion, dispatch_flow = std::move (dispatch_flow),
           actor_execution_key = std::move (actor_execution_key),
           actor_execution_spot_id = std::move (actor_execution_spot_id),
           before_invoke = std::move (before_invoke), trace_message_kind, trace_packet_name,
           trace_actor_id, trace_spot_id, admission_terminal, retained_message, application_record,
           actor_queue_dispatch] (auto complete) mutable {
              runtime::flow_context_t::scope_t callback_flow (std::move (dispatch_flow));
              // incarnation을 넘길 때는 원본 activation record를 그대로 사용한다.
              if (state->closed) {
                  if (retained_message) {
                      const auto original =
                        std::static_pointer_cast<const detail::instance_spot_retained_message_t> (
                          retained_message);
                      if (original->completion) {
                          auto pending = original->completion->task ();
                          if (pending.await_ready ()) {
                              const auto terminal = detail::task_result_access_t::read (pending);
                              complete ([state, completion, admission_terminal, terminal] {
                                  settle_handler_admission_once (admission_terminal);
                                  completion->complete (terminal);
                              });
                              return;
                          }
                      }
                  }
                  const auto owner = state->state_lane_owner ();
                  const auto current = owner ? owner->lane.run_sync ([&] {
                      const auto found =
                        owner->spot_contexts_by_id.find (std::string (state->spot_id));
                      return found == owner->spot_contexts_by_id.end ()
                               ? std::shared_ptr<detail::spot_context_state_t>{}
                               : detail::spot_context_access_t::state (found->second);
                  })
                                             : std::shared_ptr<detail::spot_context_state_t>{};
                  if (retained_message && !current) {
                      const auto original =
                        std::static_pointer_cast<const detail::instance_spot_retained_message_t> (
                          retained_message);
                      if (original->resume_missing) {
                          auto running = std::make_shared<task_t<zlink::message_t>> (
                            original->resume_missing ());
                          detail::observe_task_terminal (
                            *running, [state, running, completion, admission_terminal,
                                       complete] (const result_t<zlink::message_t> &result) {
                                complete ([state, completion, admission_terminal, result] {
                                    settle_handler_admission_once (admission_terminal);
                                    completion->complete (result);
                                });
                            });
                          return;
                      }
                  }
                  if (!retained_message || !current || current == state
                      || !current->spot_instance) {
                      complete ([state, completion, admission_terminal, retained_message] {
                          settle_handler_admission_once (admission_terminal);
                          completion->complete (
                            retained_message
                              ? result_t<zlink::message_t>::failure (
                                  framework_error_kind_t::internal_failure, "SPOT is closed.")
                              : detail::result_access_t::failure<zlink::message_t> (
                                  state->admission_error ()));
                      });
                      return;
                  }
                  const auto original =
                    std::static_pointer_cast<const detail::instance_spot_retained_message_t> (
                      retained_message);
                  const auto now = std::chrono::duration_cast<std::chrono::milliseconds> (
                                     std::chrono::system_clock::now ().time_since_epoch ())
                                     .count ();
                  if (original->original_activation->activation.target.authority_owner_generation
                        == 0
                      && original->original_activation->activation.target.deadline_unix_ms <= now) {
                      complete ([state, completion, admission_terminal] {
                          settle_handler_admission_once (admission_terminal);
                          completion->complete (result_t<zlink::message_t>::failure (
                            framework_error_kind_t::deadline_exceeded,
                            "Instance Spot activation deadline expired"));
                      });
                      return;
                  }
                  auto empty_services = current->activation_scope
                                          ? std::shared_ptr<service_provider_t>{}
                                          : std::make_shared<service_provider_t> ();
                  auto &provider = current->activation_scope
                                     ? current->activation_scope->provider ()
                                     : *empty_services;
                  auto replay = spot_handler_registry_t (current).invoke_erased (
                    {}, spot_handler_kind_t::packet,
                    original->original_activation->application_payload.packet_name (), {},
                    std::type_index (typeid (void)), current->spot_instance.get (), nullptr,
                    provider, *current->channel_runtime->serializers, carrier->first,
                    carrier->second, false);
                  detail::observe_task_completion (
                    replay, [state, current, carrier, completion, admission_terminal, complete,
                             empty_services] (const result_t<zlink::message_t> &result) mutable {
                        complete ([state, completion, admission_terminal, result] {
                            settle_handler_admission_once (admission_terminal);
                            completion->complete (result);
                        });
                    });
                  return;
              }

              runtime::actor_execution_scope_t actor_execution (
                std::move (actor_execution_key), std::move (actor_execution_spot_id));
              report_actor_dispatch_stage_trace (
                state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                trace_packet_name, trace_spot_id, trace_actor_id,
                "invoke_erased.serial_queue_execute", "started=true");
              // This adapter delivers the task result in both execution modes.
              // A current-turn replay belongs to its enclosing handler task;
              // only a newly acquired queued handler reports a gate terminal.
              auto finish_handler = [completion, admission_terminal, complete,
                                     turn =
                                       actor_queue_dispatch == actor_queue_dispatch_t::acquire
                                         ? detail::capture_current_serial_turn ()
                                         : nullptr] (result_t<zlink::message_t> result) mutable {
                  complete (
                    [completion, admission_terminal, turn, result = std::move (result)] () mutable {
                        const auto terminal =
                          result ? result_t<void>::success ()
                                 : detail::propagate_failure<void> (result, "spot handler failed");
                        settle_handler_admission_once (admission_terminal);
                        completion->complete (std::move (result));
                        if (turn)
                            terminal.value ();
                    });
              };
              try {
                  if (before_invoke) {
                      auto redirected = before_invoke (carrier->first, carrier->second);
                      if (redirected) {
                          complete ([completion, admission_terminal,
                                     result = std::move (*redirected)] () mutable {
                              settle_handler_admission_once (admission_terminal);
                              completion->complete (std::move (result));
                          });
                          return;
                      }
                  }
                  detail::callback_context_scope_t callback_scope (state.get ());
                  if (application_record)
                      application_record->retire_invocation = {};
                  if (application_record)
                      application_record->admission.enter_handler ();
                  else if (completion->before_application_handler)
                      completion->before_application_handler ();
                  auto handler_task = state->handler_invokers[handler_index](
                    spot, actor, services, serializers, carrier->first, carrier->second);
                  detail::observe_task_completion (
                    handler_task, [state, finish_handler, carrier, trace_message_kind,
                                   trace_packet_name, trace_spot_id, trace_actor_id] (
                                    const result_t<zlink::message_t> &result) mutable {
                        if (state->node
                            && detail::message_flow_tracer_t (state->node->dispatch)
                                 .enabled (message_flow_log_mode_t::detailed)) {
                            report_actor_dispatch_stage_trace_lazy (
                              state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                              trace_packet_name, trace_spot_id, trace_actor_id,
                              "invoke_erased.application_handler", [&] {
                                  return std::string ("result=finished success=")
                                         + (result ? "true" : "false");
                              });
                        }
                        result_t<zlink::message_t> final_result =
                          result ? result_t<zlink::message_t>::success (result.value ())
                                 : result_t<zlink::message_t>::failure (result.error_kind (),
                                                                        result.error () != nullptr
                                                                          ? result.error ()->what ()
                                                                          : "spot handler failed");
                        finish_handler (std::move (final_result));
                    });
              }
              catch (const framework_exception_t &error) {
                  report_actor_dispatch_stage_trace (
                    state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                    trace_packet_name, trace_spot_id, trace_actor_id,
                    "invoke_erased.application_handler", "result=finished success=false");
                  finish_handler (detail::result_access_t::failure<zlink::message_t> (error));
              }
              catch (const std::exception &error) {
                  report_actor_dispatch_stage_trace (
                    state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                    trace_packet_name, trace_spot_id, trace_actor_id,
                    "invoke_erased.application_handler", "result=finished success=false");
                  finish_handler (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::internal_failure, error.what ()));
              }
              catch (...) {
                  report_actor_dispatch_stage_trace (
                    state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                    trace_packet_name, trace_spot_id, trace_actor_id,
                    "invoke_erased.application_handler", "result=finished success=false");
                  finish_handler (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::internal_failure, "spot handler threw an exception"));
              }
          };
        work_options.completion = [admission_terminal] (result_t<void> terminal) {
            if (!terminal) {
                settle_handler_admission_once (admission_terminal);
                admission_terminal->complete (detail::propagate_failure<zlink::message_t> (
                  terminal, "Spot handler queue stopped"));
            }
        };
        const auto submit = [&] {
            return post_serial ("spot-handler", std::move (work), std::move (work_options));
        };
        const auto admission =
          admit_actor_queue ? admit_actor_queue (submit) : std::make_optional (submit ());
        if (!admission) {
            settle_handler_admission_once (admission_terminal);
            if (after_application_admission)
                after_application_admission ();
            return task_t<zlink::message_t> (
              result_t<zlink::message_t>::success (zlink::message_t{}));
        }
        const bool posted = *admission;
        if (posted && admit_actor_queue && transfer_owner_reservation)
            transfer_owner_reservation ();
        if (trace_owner
            && detail::message_flow_tracer_t (trace_owner->dispatch)
                 .enabled (message_flow_log_mode_t::detailed)) {
            report_actor_dispatch_stage_trace_lazy (
              trace_owner,
              posted && (kind == spot_handler_kind_t::packet || !application_record)
                ? message_flow_outcome_t::admitted
                : message_flow_outcome_t::received,
              trace_message_kind, trace_packet_name, trace_spot_id, trace_actor_id,
              "invoke_erased.post_serial", [&] {
                  return std::string ("posted=") + (posted ? "true" : "false")
                         + (trace_serial_queue
                              ? " closed="
                                  + std::string (trace_serial_queue->closed () ? "true" : "false")
                                  + " pending="
                                  + std::to_string (trace_serial_queue->pending_count ())
                              : " queue=inline");
              });
        }
        if (trace_owner && trace_owner->handler_invocation_for_tests)
            trace_owner->handler_invocation_for_tests (
              detail::handler_invocation_phase_t::after_queue_submission, packet_name,
              callback_spot_id, actor_execution_id);
        if (!posted) {
            // The queue did not take the work: its admission ends here.
            if (!admission_preclaimed)
                settle_handler_admission_once (admission_terminal);
            /* Spot address messaging §7 step 3: the Close that releases this
                 * incarnation closed its queue in the same decision, so a retained
                 * Instance intent message is refused before admission with the
                 * owner fence. */
            if (retained_message && state->state_sync ([&state] { return state->closed; }))
                return task_t<zlink::message_t> (
                  detail::result_access_t::failure<zlink::message_t> (detail::with_failure_code (
                    detail::make_framework_origin_exception (
                      framework_error_kind_t::unavailable,
                      "The Instance Spot incarnation released its authority."),
                    static_cast<std::uint32_t> (
                      runtime::protocol::framework_error_code::spotMoving))));
            /* Dispatch rejection is framework-generated (zlink.origin
             * marker on the resulting error reply). */
            return task_t<zlink::message_t> (detail::result_access_t::failure<zlink::message_t> (
              detail::make_framework_origin_exception (framework_error_kind_t::shutting_down,
                                                       "spot serial queue is closed or stopping")));
        }
        if (after_application_admission)
            after_application_admission ();
        return task;
    }
    if (kind == spot_handler_kind_t::actor_send && trace_owner) {
        detail::dispatch_error_reporter_t (trace_owner->dispatch).report_lazy ([&] {
            return message_dispatch_error_event_t{
              .surface = dispatch_error_surface_t::spot_actor,
              .message_kind = dispatch_message_kind_t::actor_send,
              .reason = dispatch_error_reason_t::handler_missing,
              .action = dispatch_error_action_t::drop,
              .packet_name = std::string (packet_name),
              .spot_id = actor_execution_spot_id,
              .actor_id = actor_execution_id.empty ()
                            ? std::nullopt
                            : std::make_optional (std::string (actor_execution_id)),
              .correlation_id = metadata.correlation_id,
              .mesh_name = trace_owner->snapshot.name};
        });
        runtime::runtime_metrics_t metrics (trace_owner->monitoring);
        if (metrics.enabled ())
            metrics.counter ("zlink.mesh_node.messages.dropped", "{message}", 1,
                             {{"mesh_name", trace_owner->snapshot.name},
                              {"surface", "actor"},
                              {"message_kind", "send"},
                              {"reason", "no_handler"}});
    }
    std::ostringstream error_message;
    error_message << "spot handler is not registered: packet='" << packet_name << "', topic='"
                  << topic << "', kind=" << static_cast<int> (kind) << ", actor_type='"
                  << actor_type.name () << "', registered=[";
    for (std::size_t index = 0; index < _state->handlers.size (); ++index) {
        if (index > 0) {
            error_message << "; ";
        }
        const auto &descriptor = _state->handlers[index];
        error_message << "packet='" << descriptor.packet_name << "', topic='" << descriptor.topic
                      << "', kind=" << static_cast<int> (descriptor.kind) << ", actor_type='"
                      << descriptor.actor_type.name () << "'";
    }
    error_message << "]";
    /* Route resolution failure is framework-generated (zlink.origin marker on
     * the resulting error reply); it must stay distinguishable from an
     * application handler's own not_found. */
    return task_t<zlink::message_t> (
      detail::result_access_t::failure<zlink::message_t> (detail::make_framework_origin_exception (
        framework_error_kind_t::not_found, error_message.str ())));
}

} // namespace zlink::framework
