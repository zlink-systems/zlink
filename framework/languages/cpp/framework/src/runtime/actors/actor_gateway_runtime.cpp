/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "actor_gateway_runtime.hpp"
#include "actor_ref_access.hpp"

#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace zlink::framework
{

using detail::stream_header_flags_t;
using detail::stream_header_t;
using detail::stream_message_kind_t;

namespace
{

actor_ref_t merge_actor_type (const actor_ref_t &candidate, const actor_ref_t &current)
{
    const auto candidate_type = detail::actor_ref_access_t::actor_type (candidate);
    const auto current_type = detail::actor_ref_access_t::actor_type (current);
    if (candidate_type.empty () && !current_type.empty ()) {
        return detail::actor_ref_access_t::with_actor_type (candidate, std::string (current_type));
    }
    return candidate;
}

bool actor_types_compatible (const actor_ref_t &left, const actor_ref_t &right) noexcept
{
    const auto left_type = detail::actor_ref_access_t::actor_type (left);
    const auto right_type = detail::actor_ref_access_t::actor_type (right);
    return left_type.empty () || right_type.empty () || left_type == right_type;
}

void trace_detached_bound_session_send_failure (
  const std::shared_ptr<detail::actor_gateway_state_t> &state,
  const std::string &actor_id,
  std::string_view result)
{
    detail::message_flow_tracer_t (state->dispatch)
      .trace (message_flow_outcome_t::dropped, [&] {
          auto event = message_flow_event_t{
            message_flow_outcome_t::dropped,
            dispatch_error_surface_t::stream_session,
            dispatch_message_kind_t::send,
            std::string ("bound_session_push")};
          event.actor_id = actor_id;
          event.detail_stage = "detached_delivery";
          event.detail_result = std::string (result);
          event.reason = message_flow_reason_t::target_closed;
          return event;
      });
}

void trace_detached_bound_session_send_stage (
  const std::shared_ptr<detail::actor_gateway_state_t> &state,
  const std::string &actor_id,
  std::string_view stage,
  std::string_view result)
{
    const detail::message_flow_tracer_t tracer (state->dispatch);
    tracer.trace (message_flow_log_mode_t::detailed,
                  message_flow_outcome_t::admitted, [&] {
        auto event = message_flow_event_t{
          message_flow_outcome_t::admitted,
          dispatch_error_surface_t::stream_session,
          dispatch_message_kind_t::send,
          std::string ("bound_session_push")};
        event.actor_id = actor_id;
        event.detail_stage = std::string (stage);
        if (!result.empty ())
            event.detail_result = std::string (result);
        return event;
    });
}

bool drain_bound_session_sends (
  const std::shared_ptr<detail::actor_gateway_state_t> &state,
  const std::string &queue_key,
  const std::string &actor_id)
{
    for (;;) {
        detail::actor_gateway_state_t::pending_bound_session_send_t pending;
        {
            const std::lock_guard lock (state->mutex);
            const auto found = state->pending_bound_session_sends.find (
              queue_key);
            if (found == state->pending_bound_session_sends.end ()
                || found->second.empty ()) {
                state->pending_bound_session_sends.erase (queue_key);
                state->active_bound_session_sends.erase (queue_key);
                return true;
            }
            pending = std::move (found->second.front ());
            found->second.pop_front ();
        }
        if (detail::submit_blocking_call (
              [state, queue_key, actor_id,
               pending = std::move (pending)] () mutable {
                  try {
                      /* One stage event per program point: the former
                       * offload_sender_begin/send_bound_session_enter pair
                       * described the same instant and paid the detailed-gate
                       * assembly twice. */
                      trace_detached_bound_session_send_stage (
                        state, actor_id, "send_bound_session_enter", "entered");
                      auto task =
                        std::make_shared<task_t<result_t<void>>> (
                          pending.dispatch ());
                      detail::observe_task_completion (
                        *task,
                        [state, queue_key, actor_id, task] (
                          const result_t<result_t<void>> &terminal) {
                            trace_detached_bound_session_send_stage (
                              state, actor_id, "detached_delivery_complete",
                              terminal && terminal.value () ? "ok" : "failed");
                            if (!terminal || !terminal.value ()) {
                                trace_detached_bound_session_send_failure (
                                  state, actor_id,
                                  "accepted=true detached=true");
                            }
                            (void) drain_bound_session_sends (
                              state, queue_key, actor_id);
                        });
                  }
                  catch (...) {
                      trace_detached_bound_session_send_failure (
                        state, actor_id,
                        "accepted=true detached=true exception=true");
                      (void) drain_bound_session_sends (
                        state, queue_key, actor_id);
                  }
              })) {
            return true;
        }
        trace_detached_bound_session_send_failure (
          state, actor_id,
          "accepted=false detached_queue_full=true");
        // The executor refused this head. Drop it and continue so the Actor's
        // per-session FIFO cannot remain permanently marked active.
    }
}

bool enqueue_bound_session_send (
  const std::shared_ptr<detail::actor_gateway_state_t> &state,
  const std::string &queue_key,
  const std::string &actor_id,
  detail::actor_gateway_state_t::pending_bound_session_send_t pending)
{
    constexpr std::size_t capacity = 1024;
    bool start = false;
    {
        const std::lock_guard lock (state->mutex);
        auto &queue = state->pending_bound_session_sends[queue_key];
        if (queue.size () >= capacity)
            return false;
        queue.push_back (std::move (pending));
        start = state->active_bound_session_sends.insert (
          queue_key).second;
    }
    trace_detached_bound_session_send_stage (
      state, actor_id, "fifo_accepted", "accepted");
    return !start
           || drain_bound_session_sends (state, queue_key, actor_id);
}

void drain_session_relay (
  const std::shared_ptr<detail::actor_gateway_state_t> &state,
  const std::string &actor_id)
{
    /* Trampoline: a dispatch that settles synchronously continues the FIFO
     * inside this loop instead of recursing, so a burst of immediate
     * completions cannot grow the stack with the queue length. Only a
     * completion that fires after this frame returned re-enters the drain. */
    for (;;) {
        detail::actor_gateway_state_t::pending_session_relay_t pending;
        {
            const std::lock_guard lock (state->mutex);
            const auto found = state->pending_session_relays.find (actor_id);
            if (found == state->pending_session_relays.end () || found->second.empty ()) {
                state->pending_session_relays.erase (actor_id);
                state->active_session_relays.erase (actor_id);
                return;
            }
            pending = std::move (found->second.front ());
            found->second.pop_front ();
        }

        /* Frame-owned copies: the catch blocks must not read through the
         * pending object after it moved into the completion observer. */
        const auto completion = pending.completion;
        const auto packet_name = pending.packet_name;
        try {
            /* The running coroutine frame references the closure inside
             * pending.dispatch; the observer below keeps `pending` (and that
             * closure's heap storage) alive until completion — do not shrink
             * the dispatch closure below SBO size. */
            auto dispatched = std::make_shared<task_t<void>> (pending.dispatch ());
            /* Whoever loses the exchange race hands the next turn to the
             * winner: a synchronous completion lets this loop continue, an
             * asynchronous one re-enters the drain from its own frame. */
            auto continue_gate = std::make_shared<std::atomic<bool>> (false);
            detail::observe_task_completion (
              *dispatched,
              [state, actor_id, pending = std::move (pending), dispatched,
               continue_gate] (const result_t<void> &result) mutable {
                  if (!result) {
                      detail::dispatch_error_reporter_t (state->dispatch).report_lazy ([&] {
                          return message_dispatch_error_event_t{
                            .surface = dispatch_error_surface_t::spot_actor,
                            .message_kind = dispatch_message_kind_t::actor_send,
                            .reason = detail::dispatch_reason_from_error (result.error ()),
                            .action = dispatch_error_action_t::drop,
                            .packet_name = pending.packet_name,
                            .actor_id = actor_id,
                            .exception = result.error ()
                              ? std::make_exception_ptr (*result.error ())
                              : std::exception_ptr{}};
                      });
                  }
                  pending.completion->complete (result);
                  if (continue_gate->exchange (true))
                      drain_session_relay (state, actor_id);
              });
            if (!continue_gate->exchange (true)) {
                /* Still dispatching: the completion observer owns the next
                 * drain turn. */
                return;
            }
            continue;
        }
        catch (const framework_exception_t &error) {
            detail::dispatch_error_reporter_t (state->dispatch).report_lazy ([&] {
                return message_dispatch_error_event_t{
                  .surface = dispatch_error_surface_t::spot_actor,
                  .message_kind = dispatch_message_kind_t::actor_send,
                  .reason = detail::dispatch_reason_from_error (&error),
                  .action = dispatch_error_action_t::drop,
                  .packet_name = packet_name,
                  .actor_id = actor_id,
                  .exception = std::make_exception_ptr (error)};
            });
            completion->complete (detail::result_access_t::failure<void> (error));
        }
        catch (const std::exception &error) {
            const framework_exception_t failure (
              framework_error_kind_t::internal_failure, error.what ());
            detail::dispatch_error_reporter_t (state->dispatch).report_lazy ([&] {
                return message_dispatch_error_event_t{
                  .surface = dispatch_error_surface_t::spot_actor,
                  .message_kind = dispatch_message_kind_t::actor_send,
                  .reason = dispatch_error_reason_t::handler_exception,
                  .action = dispatch_error_action_t::drop,
                  .packet_name = packet_name,
                  .actor_id = actor_id,
                  .exception = std::make_exception_ptr (failure)};
            });
            completion->complete (result_t<void>::failure (
              framework_error_kind_t::internal_failure, error.what ()));
        }
        catch (...) {
            const auto exception = std::current_exception ();
            detail::dispatch_error_reporter_t (state->dispatch).report_lazy ([&] {
                return message_dispatch_error_event_t{
                  .surface = dispatch_error_surface_t::spot_actor,
                  .message_kind = dispatch_message_kind_t::actor_send,
                  .reason = dispatch_error_reason_t::handler_exception,
                  .action = dispatch_error_action_t::drop,
                  .packet_name = packet_name,
                  .actor_id = actor_id,
                  .exception = exception};
            });
            completion->complete (result_t<void>::failure (
              framework_error_kind_t::internal_failure,
              "actor session relay threw a non-standard exception"));
        }
    }
}

task_t<void> enqueue_session_relay (
  const std::shared_ptr<detail::actor_gateway_state_t> &state,
  std::string actor_id,
  std::string packet_name,
  std::function<task_t<void> ()> dispatch)
{
    /* Session Actor relay shares the bound-session waiter bound
     * (async-execution-policy §1.3): when the bounded waiter capacity is
     * fully used a new payload is not held — the call completes immediately
     * with DeadlineExceeded and the message is never submitted later. */
    constexpr std::size_t capacity = 1024;
    auto completion =
      std::make_shared<detail::task_completion_source_t<void>> ();
    auto task = completion->task ();
    bool start_drain = false;
    bool admitted = false;
    {
        const std::lock_guard lock (state->mutex);
        auto &queue = state->pending_session_relays[actor_id];
        if (queue.size () < capacity) {
            queue.push_back (
              {std::move (dispatch), completion, std::move (packet_name)});
            start_drain = state->active_session_relays.insert (actor_id).second;
            admitted = true;
        }
    }
    if (!admitted) {
        const framework_exception_t rejected (
          framework_error_kind_t::deadline_exceeded,
          "actor session relay waiter capacity is exhausted");
        detail::dispatch_error_reporter_t (state->dispatch).report_lazy ([&] {
            return message_dispatch_error_event_t{
              .surface = dispatch_error_surface_t::spot_actor,
              .message_kind = dispatch_message_kind_t::actor_send,
              .reason = dispatch_error_reason_t::backpressure,
              .action = dispatch_error_action_t::drop,
              .packet_name = packet_name,
              .actor_id = actor_id,
              .exception = std::make_exception_ptr (rejected)};
        });
        completion->complete (
          detail::result_access_t::failure<void> (rejected));
        return task;
    }
    if (start_drain)
        drain_session_relay (state, actor_id);
    return task;
}

task_t<zlink::message_t> complete_session_actor_relay_request (
  std::shared_ptr<detail::actor_gateway_state_t> state,
  detail::actor_gateway_state_t::relay_dispatcher_t dispatcher,
  actor_ref_t actor,
  actor_context_t actor_context,
  detail::stream_header_t relay_header,
  zlink::message_t payload,
  std::optional<detail::bound_session_relay_source_t> relay_source)
{
    const auto dispatched = co_await dispatcher (
      actor, std::move (actor_context), relay_header, payload,
      std::move (relay_source));
    if (!dispatched) {
        const framework_exception_t missing_reply (
          framework_error_kind_t::protocol_error,
          "actor relay request has no reply");
        detail::dispatch_error_reporter_t (state->dispatch)
          .report_lazy ([&] { return message_dispatch_error_event_t{
            dispatch_error_surface_t::spot_actor,
            dispatch_message_kind_t::actor_request,
            dispatch_error_reason_t::reply_path_missing,
            dispatch_error_action_t::fail_caller, {}, std::nullopt,
            std::nullopt, std::nullopt,
            std::string (actor.actor_id ().value ()), std::nullopt,
            std::nullopt, std::make_exception_ptr (missing_reply)}; });
        throw missing_reply;
    }
    co_return std::move (*dispatched);
}

task_t<void> complete_session_actor_disconnect_notification (
  std::shared_ptr<detail::actor_gateway_state_t> state,
  detail::actor_gateway_state_t::disconnect_dispatcher_t dispatcher,
  actor_ref_t actor,
  std::string session_id,
  std::uint64_t binding_token)
{
    if (dispatcher)
        co_await dispatcher (actor);
    if (binding_token != 0) {
        detail::actor_gateway_runtime_t (state).unbind_session_stream (
          std::string (actor.actor_id ().value ()), std::move (session_id),
          binding_token);
    }
    co_return;
}

} // namespace

namespace detail
{

namespace
{
thread_local std::vector<stream_header_t> stream_relay_headers;

stream_header_t actor_relay_header (stream_message_kind_t kind, std::string packet_name)
{
    return stream_header_t (kind, stream_codec_t::json, stream_header_flags_t::none, std::nullopt,
                            std::move (packet_name));
}

}

stream_relay_dispatch_scope_t::stream_relay_dispatch_scope_t (stream_header_t header)
{
    stream_relay_headers.push_back (std::move (header));
}

stream_relay_dispatch_scope_t::~stream_relay_dispatch_scope_t () noexcept
{
    if (!stream_relay_headers.empty ()) {
        stream_relay_headers.pop_back ();
    }
}

std::optional<stream_header_t> current_stream_relay_dispatch ()
{
    return stream_relay_headers.empty () ? std::nullopt
                                         : std::make_optional (stream_relay_headers.back ());
}

} // namespace detail

actor_ref_t::actor_ref_t (actor_id_t actor_id,
                          std::uint64_t object_generation,
                          std::string mesh_name,
                          node_rid_t node_rid) :
    _actor_id (std::move (actor_id)),
    _object_generation (object_generation),
    _mesh_name (std::move (mesh_name)),
    _node_rid (std::move (node_rid))
{
    if (object_generation == 0
        || object_generation
             > static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ())) {
        throw std::invalid_argument ("ActorRef ObjectGeneration must be from 1 through INT64_MAX");
    }
}

const node_rid_t &actor_ref_t::node_rid () const noexcept
{
    return _node_rid;
}

const actor_id_t &actor_ref_t::actor_id () const noexcept
{
    return _actor_id;
}

std::uint64_t actor_ref_t::object_generation () const noexcept
{
    return _object_generation;
}

std::string_view actor_ref_t::mesh_name () const noexcept
{
    return _mesh_name;
}

bound_session_t::bound_session_t () : _state (std::make_shared<detail::actor_gateway_state_t> ())
{
}

bound_session_t::bound_session_t (std::shared_ptr<detail::actor_gateway_state_t> state,
                                  actor_ref_t actor_ref,
                                  std::uint64_t expected_binding_generation) :
    _state (std::move (state)),
    _actor_ref (std::make_shared<actor_ref_t> (std::move (actor_ref))),
    _expected_binding_generation (expected_binding_generation)
{
}

bound_session_t::~bound_session_t () = default;
bound_session_t::bound_session_t (bound_session_t &&) noexcept = default;
bound_session_t &bound_session_t::operator= (bound_session_t &&) noexcept = default;

bound_session_send_call_t bound_session_t::send (const message_t &payload)
{
    if (!_state) {
        return bound_session_send_call_t (send_call_t (
          result_t<void>::failure (framework_error_kind_t::protocol_error,
                                   "bound session send requires actor gateway state")));
    }
    if (!_state->serializers) {
        return bound_session_send_call_t (send_call_t (
          result_t<void>::failure (framework_error_kind_t::protocol_error,
                                   "bound session send requires a serializer registry")));
    }
    try {
        const auto codec = detail::stream_codec_from_content_type (
          _state->serializers->content_type (payload._type));
        return send_erased ("actor.push", codec,
                            detail::message_to_raw (payload, *_state->serializers));
    }
    catch (const framework_exception_t &error) {
        return bound_session_send_call_t (
          send_call_t (detail::result_access_t::failure<void> (error)));
    }
}

bound_session_send_call_t bound_session_t::send_typed (
  std::string packet_name,
  std::type_index message_type,
  std::function<encoded_payload_t (serializer_registry_t &)> encode_payload)
{
    if (!_state || !_state->serializers) {
        return bound_session_send_call_t (send_call_t (
          result_t<void>::failure (framework_error_kind_t::protocol_error,
                                   "bound session send requires a serializer registry")));
    }
    try {
        auto payload = detail::encoded_payload_to_raw (encode_payload (*_state->serializers));
        const auto codec =
          detail::stream_codec_from_content_type (_state->serializers->content_type (message_type));
        return send_erased (std::move (packet_name), codec, payload);
    }
    catch (const framework_exception_t &error) {
        return bound_session_send_call_t (
          send_call_t (detail::result_access_t::failure<void> (error)));
    }
}

bound_session_send_call_t bound_session_t::send_typed (std::string packet_name,
                                                       std::type_index message_type,
                                                       const void *message)
{
    if (!_state || !_state->serializers) {
        return bound_session_send_call_t (send_call_t (
          result_t<void>::failure (framework_error_kind_t::protocol_error,
                                   "bound session send requires a serializer registry")));
    }
    try {
        auto payload =
          detail::encoded_payload_to_raw (_state->serializers->serialize (message_type, message));
        const auto codec =
          detail::stream_codec_from_content_type (_state->serializers->content_type (message_type));
        return send_erased (std::move (packet_name), codec, payload);
    }
    catch (const framework_exception_t &error) {
        return bound_session_send_call_t (
          send_call_t (detail::result_access_t::failure<void> (error)));
    }
}

task_t<void> bound_session_t::disconnect ()
{
    if (!_state || !_actor_ref) {
        return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_configured,
                                                      "bound session does not reference an actor"));
    }
    std::unique_lock lock (_state->mutex);
    const auto found = _state->actors_by_id.find (std::string (_actor_ref->actor_id ().value ()));
    if (found != _state->actors_by_id.end ()) {
        if (found->second.ref.object_generation () != _actor_ref->object_generation ()) {
            lock.unlock ();
            return task_t<void> (result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                                          "actor generation is stale"));
        }
        found->second.bound = false;
        found->second.disconnected = true;
    }
    lock.unlock ();
    return task_t<void> (result_t<void>::success ());
}

bound_session_send_call_t bound_session_t::send_erased (std::string packet_name,
                                                        stream_codec_t codec,
                                                        const zlink::message_t &payload)
{
    if (!_state || !_actor_ref) {
        return bound_session_send_call_t (send_call_t (result_t<void>::failure (
          framework_error_kind_t::not_configured, "bound session does not reference an actor")));
    }
    std::shared_ptr<detail::bound_session_sink_t> sink;
    detail::actor_gateway_state_t::bound_session_sender_t remote_sender;
    stream_header_t header;
    const auto actor_id = std::string (_actor_ref->actor_id ().value ());
    std::string queue_key;
    {
        const std::lock_guard lock (_state->mutex);
        const auto found = _state->actors_by_id.find (actor_id);
        remote_sender = _state->bound_session_sender;
        if (found != _state->actors_by_id.end () && found->second.disconnected) {
            return bound_session_send_call_t (send_call_t (detail::boundary_failure<void> (
              detail::boundary_error_t::disconnected, "actor session is disconnected")));
        }
        if (found == _state->actors_by_id.end () || !found->second.bound) {
            if (!remote_sender) {
                return bound_session_send_call_t (send_call_t (result_t<void>::failure (
                  framework_error_kind_t::not_configured, "actor session is not bound")));
            }
            header = stream_header_t (stream_message_kind_t::send, codec,
                                      stream_header_flags_t::none, std::nullopt, packet_name);
        } else if (!actor_types_compatible (found->second.ref, *_actor_ref)) {
            return bound_session_send_call_t (send_call_t (result_t<void>::failure (
              framework_error_kind_t::type_mismatch, "actor id is already bound to another type")));
        } else if (found->second.ref.object_generation () != _actor_ref->object_generation ()) {
            return bound_session_send_call_t (send_call_t (result_t<void>::failure (
              framework_error_kind_t::invalid_operation, "actor generation is stale")));
        } else {
            /* A bound Session send consumes the route selected by its owner;
             * it must not publish a location carried by the caller's Actor
             * context. Route relocation is committed together with the
             * Session fence by command 44. */
            found->second.ref = merge_actor_type (found->second.ref, *_actor_ref);
            header = stream_header_t (stream_message_kind_t::send, codec,
                                      stream_header_flags_t::none, std::nullopt, packet_name);
            _state->bound_session_pushes.push_back (
              detail::relayed_frame_t{found->second.ref, header, payload});
            const auto found_sink = _state->bound_session_sinks.find (actor_id);
            if (found_sink != _state->bound_session_sinks.end ())
                sink = found_sink->second;
            if (found->second.bound_session_route) {
                const auto &route = *found->second.bound_session_route;
                queue_key = actor_id + "/" + route.node_rid.to_hex () + "/"
                  + (route.session_rid ? route.session_rid->to_hex ()
                                       : std::string ("none"))
                  + "/" + std::to_string (route.binding_generation)
                  + "/" + std::to_string (route.binding_token);
            }
        }
    }
    if (!sink && !remote_sender) {
        return bound_session_send_call_t (send_call_t (result_t<void>::failure (
          framework_error_kind_t::not_configured, "actor bound session has no send sink")));
    }
    if (queue_key.empty ()) {
        queue_key = actor_id + "/remote/"
          + std::to_string (_expected_binding_generation);
    }
    return bound_session_send_call_t (send_call_t (
      std::move (packet_name),
      [state = _state, queue_key = std::move (queue_key), actor_id,
       sink = std::move (sink), remote_sender = std::move (remote_sender),
       actor_ref = *_actor_ref,
       expected_binding_generation = _expected_binding_generation,
       header = std::move (header), payload,
       codec] (const std::string &name,
               const send_call_t::metadata_map_t &) mutable -> task_t<void> {
          // One-way submit accepts into the Framework-owned per-binding FIFO.
          // Sink coroutines are eager, so even constructing one on the Actor
          // handler stack could otherwise enter native ROUTER admission and
          // retain the serial turn until relocation sealing or timeout.
          const auto submitted = enqueue_bound_session_send (
            state, queue_key, actor_id,
            {[sink, remote_sender, actor_ref,
              expected_binding_generation, header, payload, codec,
              name] () mutable -> task_t<result_t<void>> {
                try {
                    if (sink) {
                        co_await (*sink) (name, codec, payload);
                        co_return result_t<void>::success ();
                    }
                    co_return co_await remote_sender (
                      actor_ref, expected_binding_generation,
                      header, payload);
                }
                catch (const framework_exception_t &error) {
                    co_return detail::result_access_t::failure<void> (
                      error);
                }
                catch (const std::exception &error) {
                    co_return result_t<void>::failure (
                      framework_error_kind_t::internal_failure,
                      error.what ());
                }
            }});
          if (!submitted) {
              return task_t<void> (detail::boundary_failure<void> (
                detail::boundary_error_t::timed_out,
                "bound Session detached send queue did not accept the push"));
          }
          return task_t<void> (result_t<void>::success ());
      }));
}

actor_context_t::actor_context_t () : _state (std::make_shared<detail::actor_gateway_state_t> ())
{
}

actor_context_t::actor_context_t (std::shared_ptr<detail::actor_gateway_state_t> state,
                                  actor_ref_t actor_ref,
                                  std::uint64_t source_binding_generation,
                                  std::string mesh_name) :
    _state (std::move (state)),
    _actor_ref (std::make_shared<actor_ref_t> (std::move (actor_ref))),
    _source_binding_generation (source_binding_generation),
    _mesh_name (std::move (mesh_name))
{
}

actor_context_t::~actor_context_t () = default;
actor_context_t::actor_context_t (actor_context_t &&) noexcept = default;

const actor_ref_t &actor_context_t::actor_ref () const noexcept
{
    return *_actor_ref;
}

const actor_id_t &actor_context_t::actor_id () const noexcept
{
    return _actor_ref->actor_id ();
}

std::uint64_t actor_context_t::object_generation () const noexcept
{
    return _actor_ref ? _actor_ref->object_generation () : 0;
}

std::string_view actor_context_t::mesh_name () const noexcept
{
    return _mesh_name;
}

bool actor_context_t::has_same_source_fence (const actor_context_t &other) const noexcept
{
    return _state == other._state && _actor_ref && other._actor_ref
           && _actor_ref->node_rid ().value () == other._actor_ref->node_rid ().value ()
           && actor_types_compatible (*_actor_ref, *other._actor_ref)
           && _actor_ref->actor_id () == other._actor_ref->actor_id ()
           && _actor_ref->object_generation () == other._actor_ref->object_generation ()
           && _mesh_name == other._mesh_name;
}

std::optional<spot_id_t> actor_context_t::spot_id () const
{
    detail::actor_gateway_state_t::membership_query_t query;
    {
        const std::lock_guard lock (_state->mutex);
        query = _state->membership_query;
    }
    if (!query || !_actor_ref
        || ::zlink::framework::detail::actor_ref_access_t::empty (*_actor_ref)) {
        return std::nullopt;
    }
    return query (*_actor_ref);
}

serializer_registry_t *actor_context_t::serializer_registry () const noexcept
{
    return _state->serializers;
}

std::optional<zlink::message_t> actor_context_t::create_payload () const
{
    const std::lock_guard lock (_state->mutex);
    if (!_actor_ref) {
        return std::nullopt;
    }
    const auto found = _state->actors_by_id.find (std::string (_actor_ref->actor_id ().value ()));
    if (found == _state->actors_by_id.end ()) {
        return std::nullopt;
    }
    return found->second.create_payload;
}

bound_session_t actor_context_t::bound_session () const
{
    if (!_actor_ref) {
        return bound_session_t ();
    }
    return bound_session_t (_state, *_actor_ref, _source_binding_generation);
}

task_t<detail::actor_join_reply_t> actor_context_t::join_spot_erased (
  spot_id_t spot_id, const zlink::message_t &request,
  std::string packet_name, std::string content_type,
  std::chrono::milliseconds timeout)
{
    /* Coroutine on a caller-owned context: copy the shared state and actor
     * ref into the frame so resumes after the owner unwinds never touch
     * `this` (session-reconnect-and-coroutine-lifetime doc). */
    const auto state = _state;
    const auto actor_ref = _actor_ref;
    detail::actor_gateway_state_t::join_spot_dispatcher_t dispatcher;
    std::optional<result_t<detail::actor_join_reply_t>> rejected;
    {
        const std::lock_guard lock (state->mutex);
        if (!actor_ref || ::zlink::framework::detail::actor_ref_access_t::empty (*actor_ref)) {
            rejected = result_t<detail::actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "actor ref is empty");
        } else if (spot_id.empty ()) {
            rejected = result_t<detail::actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "spot id is empty");
        } else if (!state->join_spot_dispatcher) {
            rejected = result_t<detail::actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "actor join spot dispatcher is not configured");
        } else {
            dispatcher = state->join_spot_dispatcher;
        }
    }
    if (rejected)
        co_return std::move (*rejected);
    detail::message_flow_tracer_t (state->dispatch).trace (message_flow_outcome_t::sent, [&] {
        return message_flow_event_t{message_flow_outcome_t::sent,
                                    dispatch_error_surface_t::spot_actor,
                                    dispatch_message_kind_t::actor_request,
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt,
                                    std::string (spot_id),
                                    std::string (actor_ref->actor_id ().value ()),
                                    std::nullopt};
    });
    /* The request reference must not cross the suspension: hand the
     * dispatcher a frame-owned copy (message payloads share their buffer). */
    const zlink::message_t request_frame = request;
    auto joined = co_await dispatcher (
      *actor_ref, spot_id, request_frame, std::move (packet_name),
      std::move (content_type), timeout);
    detail::message_flow_tracer_t (state->dispatch)
      .trace (message_flow_outcome_t::reply_received, [&] {
          return message_flow_event_t{message_flow_outcome_t::reply_received,
                                      dispatch_error_surface_t::spot_actor,
                                      dispatch_message_kind_t::actor_request,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::string (spot_id),
                                      std::string (actor_ref->actor_id ().value ()),
                                      std::nullopt};
      });

    if (joined.result_code == 0) {
        const std::lock_guard lock (state->mutex);
        *actor_ref = joined.actor;
        auto found = state->actors_by_id.find (std::string (actor_ref->actor_id ().value ()));
        if (found != state->actors_by_id.end ()) {
            found->second.ref = *actor_ref;
        }
    }
    co_return joined;
}

result_t<std::shared_ptr<detail::deferred_barrier_t>> actor_context_t::reserve_join_barrier () const
{
    detail::actor_gateway_state_t::join_barrier_reserver_t reserver;
    std::optional<actor_ref_t> actor;
    {
        const std::lock_guard lock (_state->mutex);
        if (!_actor_ref || ::zlink::framework::detail::actor_ref_access_t::empty (*_actor_ref)) {
            return result_t<std::shared_ptr<detail::deferred_barrier_t>>::failure (
              framework_error_kind_t::not_found, "Actor join barrier source is empty");
        }
        if (!_state->join_barrier_reserver) {
            return result_t<std::shared_ptr<detail::deferred_barrier_t>>::failure (
              framework_error_kind_t::not_configured,
              "Actor join barrier runtime is not configured");
        }
        actor = *_actor_ref;
        reserver = _state->join_barrier_reserver;
    }
    return reserver (*actor);
}

actor_join_call_t actor_context_t::join_entry_spot_payload (const zlink::message_t &request)
{
    auto context = std::shared_ptr<actor_context_t> (
      new actor_context_t (_state, *_actor_ref, _source_binding_generation, _mesh_name));
    context->_actor_ref = _actor_ref;
    return actor_join_call_t (
      [context, request] (std::chrono::milliseconds timeout) mutable {
          detail::actor_gateway_state_t::join_entry_spot_dispatcher_t dispatcher;
          zlink::message_t effective_request = request;
          {
              const std::lock_guard lock (context->_state->mutex);
              if (!context->_actor_ref
                  || ::zlink::framework::detail::actor_ref_access_t::empty (*context->_actor_ref)) {
                  throw framework_exception_t (framework_error_kind_t::not_found,
                                               "actor ref is empty");
              }
              if (!context->_state->join_entry_spot_dispatcher) {
                  throw framework_exception_t (
                    framework_error_kind_t::not_found,
                    "actor join entry spot dispatcher is not configured");
              }
              dispatcher = context->_state->join_entry_spot_dispatcher;
              if (effective_request.to_string ().empty ()) {
                  const auto found = context->_state->actors_by_id.find (
                    std::string (context->_actor_ref->actor_id ().value ()));
                  if (found != context->_state->actors_by_id.end ()
                      && found->second.create_payload) {
                      effective_request = *found->second.create_payload;
                  }
              }
          }

          auto joined = dispatcher (*context->_actor_ref, effective_request, timeout);
          if (!joined) {
              const auto *error = joined.error ();
              throw framework_exception_t (joined.error_kind (),
                                           error != nullptr ? error->what ()
                                                            : "actor join entry spot failed");
          }

          if (joined.value ().result_code == 0) {
              const std::lock_guard lock (context->_state->mutex);
              *context->_actor_ref = joined.value ().actor;
              auto found = context->_state->actors_by_id.find (
                std::string (context->_actor_ref->actor_id ().value ()));
              if (found != context->_state->actors_by_id.end ()) {
                  found->second.ref = *context->_actor_ref;
              }
          }
      },
      [context] { return context->reserve_join_barrier (); });
}

session_actor_t::session_actor_t (std::shared_ptr<detail::actor_gateway_state_t> state,
                                  actor_ref_t ref,
                                  std::uint64_t binding_token) :
    _state (std::move (state)), _ref (std::move (ref)), _binding_token (binding_token)
{
}

session_actor_t::~session_actor_t () = default;
session_actor_t::session_actor_t (session_actor_t &&) noexcept = default;
session_actor_t &session_actor_t::operator= (session_actor_t &&) noexcept = default;

const actor_ref_t &session_actor_t::ref () const noexcept
{
    return _ref;
}

std::string_view session_actor_t::actor_id () const noexcept
{
    return _ref.actor_id ().value ();
}

actor_context_t session_actor_t::context () const
{
    return actor_context_t (_state, _ref);
}

bound_session_t session_actor_t::bound_session () const
{
    return bound_session_t (_state, _ref);
}

result_t<std::uint64_t> session_actor_t::reserve_relay_sequence ()
{
    const std::lock_guard lock (_state->mutex);
    const auto found = _state->actors_by_id.find (std::string (_ref.actor_id ().value ()));
    if (found == _state->actors_by_id.end ()) {
        return result_t<std::uint64_t>::failure (framework_error_kind_t::not_found,
                                                 "actor route is not found");
    }
    if (found->second.next_session_relay_sequence == std::numeric_limits<std::uint64_t>::max ()) {
        return result_t<std::uint64_t>::failure (framework_error_kind_t::capacity_exceeded,
                                                 "actor session relay sequence is exhausted");
    }
    return result_t<std::uint64_t>::success (found->second.next_session_relay_sequence++);
}

task_t<void> session_actor_t::relay_internal (detail::stream_header_t header,
                                              std::uint64_t relay_sequence,
                                              const zlink::message_t &payload)
{
    detail::actor_gateway_state_t::relay_dispatcher_t dispatcher;
    detail::stream_header_t relay_header;
    std::optional<detail::bound_session_relay_source_t> relay_source;
    {
        std::unique_lock lock (_state->mutex);
        if (::zlink::framework::detail::actor_ref_access_t::empty (_ref)) {
            lock.unlock ();
            return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_found,
                                                          "session actor is not bound"));
        }
        const auto found = _state->actors_by_id.find (std::string (_ref.actor_id ().value ()));
        if (found != _state->actors_by_id.end () && found->second.disconnected) {
            lock.unlock ();
            return task_t<void> (detail::boundary_failure<void> (
              detail::boundary_error_t::disconnected, "actor session is disconnected"));
        }
        if (found == _state->actors_by_id.end ()) {
            lock.unlock ();
            return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_found,
                                                          "actor route is not found"));
        }
        if (!found->second.bound) {
            lock.unlock ();
            return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_configured,
                                                          "actor session is not bound"));
        }
        if (_binding_token != 0 && found->second.binding_token != _binding_token) {
            lock.unlock ();
            return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_configured,
                                                          "actor session binding is stale"));
        }
        if (found->second.ref.object_generation () != _ref.object_generation ()) {
            lock.unlock ();
            return task_t<void> (result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                                          "actor generation is stale"));
        }
        _ref = found->second.ref;
        if (!_state->relay_dispatcher) {
            /* Bounded parking (async-execution-policy §1.3, same 1024 bound
             * as the session relay waiters): beyond the bound the frame is
             * not held — the call fails immediately with DeadlineExceeded and
             * the drop is reported. */
            if (_state->relayed_frames.size () >= detail::relayed_frame_capacity) {
                lock.unlock ();
                const framework_exception_t rejected (
                  framework_error_kind_t::deadline_exceeded,
                  "actor relay frame capacity is exhausted");
                detail::dispatch_error_reporter_t (_state->dispatch).report_lazy ([&] {
                    return message_dispatch_error_event_t{
                      .surface = dispatch_error_surface_t::spot_actor,
                      .message_kind = dispatch_message_kind_t::actor_send,
                      .reason = dispatch_error_reason_t::backpressure,
                      .action = dispatch_error_action_t::drop,
                      .packet_name = std::string (header.packet_name ()),
                      .actor_id = std::string (_ref.actor_id ().value ()),
                      .exception = std::make_exception_ptr (rejected)};
                });
                return task_t<void> (
                  detail::result_access_t::failure<void> (rejected));
            }
            _state->relayed_frames.push_back (detail::relayed_frame_t{_ref, header, payload});
            lock.unlock ();
            return task_t<void> (result_t<void>::success ());
        }
        dispatcher = _state->relay_dispatcher;
        if (found->second.source_session_rid
            && found->second.source_binding_generation != 0) {
            relay_source = detail::bound_session_relay_source_t{
              *found->second.source_session_rid,
              found->second.source_binding_generation,
              relay_sequence};
        }
        auto metadata = header.metadata ().values ();
        metadata.insert_or_assign (std::string (detail::bound_session_relay_binding_key),
                                   std::to_string (found->second.binding_token));
        metadata.insert_or_assign (std::string (detail::bound_session_relay_sequence_key),
                                   std::to_string (relay_sequence));
        relay_header = detail::stream_header_t (
          header.kind (), header.codec (), header.flags (), header.request_seq (),
          std::string (header.packet_name ()), detail::stream_metadata_t (std::move (metadata)));
        if (const auto correlation = header.correlation_id ())
            relay_header.with_correlation_id (std::string (*correlation));
        /* flow-correlation §5: an intermediate runtime forwards the flow pair
         * only while tracing is on; at Off the inbound pair is not copied. */
        if (detail::message_flow_tracer_t (_state->dispatch).capture_enabled ()) {
            if (const auto flow_id = header.flow_id ())
                relay_header.with_flow (std::string (*flow_id),
                                        *header.flow_origin ());
        }
    }

    const auto actor_id = std::string (_ref.actor_id ().value ());
    const auto packet_name = std::string (relay_header.packet_name ());
    auto actor_context = std::make_shared<actor_context_t> (context ());
    return enqueue_session_relay (
      _state, actor_id, packet_name,
      [dispatcher = std::move (dispatcher), actor = _ref,
       actor_context = std::move (actor_context), relay_header = std::move (relay_header),
       payload, relay_source = std::move (relay_source)] () mutable -> task_t<void> {
          const auto dispatched = co_await dispatcher (
            actor, std::move (*actor_context), relay_header, payload,
            std::move (relay_source));
          (void) dispatched;
          co_return;
      });
}

task_t<void> session_actor_t::relay (const zlink::message_t &payload)
{
    const auto header = detail::current_stream_relay_dispatch ();
    if (!header) {
        return task_t<void> (
          result_t<void>::failure (framework_error_kind_t::protocol_error,
                                   "actor relay requires current stream dispatch state"));
    }
    const auto sequence = reserve_relay_sequence ();
    if (!sequence) {
        return task_t<void> (
          detail::propagate_failure<void> (sequence, "actor relay sequence failed"));
    }
    return relay_internal (*header, sequence.value (), payload);
}

task_t<void> session_actor_t::relay (std::string packet_name, const zlink::message_t &payload)
{
    auto header = detail::actor_relay_header (stream_message_kind_t::send, std::move (packet_name));
    const auto sequence = reserve_relay_sequence ();
    if (!sequence) {
        return task_t<void> (
          detail::propagate_failure<void> (sequence, "actor relay sequence failed"));
    }
    return relay_internal (std::move (header), sequence.value (), payload);
}

relay_request_call_t session_actor_t::relay_request (const zlink::message_t &payload)
{
    const auto header = detail::current_stream_relay_dispatch ();
    if (!header) {
        return relay_request_call_t (result_t<zlink::message_t>::failure (
          framework_error_kind_t::protocol_error,
          "actor relay request requires current stream dispatch state"));
    }
    const auto sequence = reserve_relay_sequence ();
    if (!sequence) {
        return relay_request_call_t (
          detail::propagate_failure<zlink::message_t> (
            sequence, "actor relay request sequence failed"));
    }
    detail::actor_gateway_state_t::relay_dispatcher_t dispatcher;
    detail::stream_header_t relay_header;
    std::optional<detail::bound_session_relay_source_t> relay_source;
    {
        const std::lock_guard lock (_state->mutex);
        if (::zlink::framework::detail::actor_ref_access_t::empty (_ref)) {
            return relay_request_call_t (result_t<zlink::message_t>::failure (
              framework_error_kind_t::not_found, "session actor is not bound"));
        }
        const auto found = _state->actors_by_id.find (std::string (_ref.actor_id ().value ()));
        if (found != _state->actors_by_id.end () && found->second.disconnected) {
            return relay_request_call_t (detail::boundary_failure<zlink::message_t> (
              detail::boundary_error_t::disconnected, "actor session is disconnected"));
        }
        if (found == _state->actors_by_id.end ()) {
            return relay_request_call_t (result_t<zlink::message_t>::failure (
              framework_error_kind_t::not_found, "actor route is not found"));
        }
        if (!found->second.bound) {
            return relay_request_call_t (result_t<zlink::message_t>::failure (
              framework_error_kind_t::not_configured, "actor session is not bound"));
        }
        if (_binding_token != 0 && found->second.binding_token != _binding_token) {
            return relay_request_call_t (result_t<zlink::message_t>::failure (
              framework_error_kind_t::not_configured, "actor session binding is stale"));
        }
        if (found->second.ref.object_generation () != _ref.object_generation ()) {
            return relay_request_call_t (result_t<zlink::message_t>::failure (
              framework_error_kind_t::invalid_operation, "actor generation is stale"));
        }
        _ref = found->second.ref;
        if (!_state->relay_dispatcher) {
            /* Bounded parking (async-execution-policy §1.3): beyond the 1024
             * bound the frame is dropped instead of retained. The call keeps
             * its immediate not_found failure either way; the drop event
             * records the discarded frame. */
            if (_state->relayed_frames.size () >= detail::relayed_frame_capacity) {
                const framework_exception_t rejected (
                  framework_error_kind_t::deadline_exceeded,
                  "actor relay frame capacity is exhausted");
                detail::dispatch_error_reporter_t (_state->dispatch).report_lazy ([&] {
                    return message_dispatch_error_event_t{
                      .surface = dispatch_error_surface_t::spot_actor,
                      .message_kind = dispatch_message_kind_t::actor_request,
                      .reason = dispatch_error_reason_t::backpressure,
                      .action = dispatch_error_action_t::drop,
                      .packet_name = std::string (header->packet_name ()),
                      .actor_id = std::string (_ref.actor_id ().value ()),
                      .exception = std::make_exception_ptr (rejected)};
                });
            } else {
                _state->relayed_frames.push_back (
                  detail::relayed_frame_t{_ref, *header, payload});
            }
            return relay_request_call_t (result_t<zlink::message_t>::failure (
              framework_error_kind_t::not_found, "actor relay dispatcher is not configured"));
        }
        dispatcher = _state->relay_dispatcher;
        if (found->second.source_session_rid
            && found->second.source_binding_generation != 0) {
            relay_source = detail::bound_session_relay_source_t{
              *found->second.source_session_rid,
              found->second.source_binding_generation,
              sequence.value ()};
        }
        auto metadata = header->metadata ().values ();
        metadata.insert_or_assign (std::string (detail::bound_session_relay_binding_key),
                                   std::to_string (found->second.binding_token));
        metadata.insert_or_assign (std::string (detail::bound_session_relay_sequence_key),
                                   std::to_string (sequence.value ()));
        relay_header = detail::stream_header_t (
          header->kind (), header->codec (), header->flags (), header->request_seq (),
          std::string (header->packet_name ()), detail::stream_metadata_t (std::move (metadata)));
        if (const auto correlation = header->correlation_id ())
            relay_header.with_correlation_id (std::string (*correlation));
        /* flow-correlation §5: an intermediate runtime forwards the flow pair
         * only while tracing is on; at Off the inbound pair is not copied. */
        if (detail::message_flow_tracer_t (_state->dispatch).capture_enabled ()) {
            if (const auto flow_id = header->flow_id ())
                relay_header.with_flow (std::string (*flow_id),
                                        *header->flow_origin ());
        }
    }
    return relay_request_call_t (complete_session_actor_relay_request (
      _state, std::move (dispatcher), _ref, context (),
      std::move (relay_header), payload, std::move (relay_source)));
}

relay_request_call_t session_actor_t::relay_request (std::string packet_name,
                                                     const zlink::message_t &payload)
{
    const detail::stream_relay_dispatch_scope_t relay_scope (
      detail::actor_relay_header (stream_message_kind_t::request, std::move (packet_name)));
    return relay_request (payload);
}

task_t<void> session_actor_t::notify_disconnected ()
{
    detail::actor_gateway_state_t::disconnect_dispatcher_t dispatcher;
    std::string session_id;
    std::uint64_t binding_token = 0;
    {
        std::unique_lock lock (_state->mutex);
        const auto found = _state->actors_by_id.find (std::string (_ref.actor_id ().value ()));
        if (found == _state->actors_by_id.end ()) {
            lock.unlock ();
            return task_t<void> (result_t<void>::success ());
        }
        if (_binding_token != 0 && found->second.binding_token != _binding_token) {
            lock.unlock ();
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::not_configured, "actor session binding is stale"));
        }
        if (found->second.ref.object_generation () != _ref.object_generation ()) {
            lock.unlock ();
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::invalid_operation, "actor generation is stale"));
        }
        if (found->second.disconnected) {
            lock.unlock ();
            return task_t<void> (result_t<void>::success ());
        }
        _ref = found->second.ref;
        found->second.bound = false;
        found->second.disconnected = true;
        session_id = found->second.binding_session_id;
        binding_token = found->second.binding_token;
        dispatcher = _state->disconnect_dispatcher;
    }
    return complete_session_actor_disconnect_notification (
      _state, std::move (dispatcher), _ref, std::move (session_id),
      binding_token);
}

session_actor_manager_t::session_actor_manager_t () :
    _state (std::make_shared<detail::actor_gateway_state_t> ()),
    _binding_context (std::make_shared<detail::session_actor_binding_context_t> ())
{
}

session_actor_manager_t::session_actor_manager_t (
  std::shared_ptr<detail::actor_gateway_state_t> state) :
    _state (std::move (state)),
    _binding_context (std::make_shared<detail::session_actor_binding_context_t> ())
{
}

session_actor_manager_t::~session_actor_manager_t () = default;
session_actor_manager_t::session_actor_manager_t (session_actor_manager_t &&) noexcept = default;
session_actor_manager_t &
session_actor_manager_t::operator= (session_actor_manager_t &&) noexcept = default;

result_t<session_actor_t> session_actor_manager_t::create (std::string actor_type,
                                                           std::string actor_id)
{
    return create_erased (std::move (actor_type), std::move (actor_id), std::nullopt);
}

result_t<session_actor_t> session_actor_manager_t::create (std::string actor_type,
                                                           std::string actor_id,
                                                           const zlink::message_t &request)
{
    return create_erased (std::move (actor_type), std::move (actor_id), request);
}

result_t<session_actor_t> session_actor_manager_t::create (std::string actor_type,
                                                           std::string actor_id,
                                                           const message_t &request)
{
    if (!_state || !_state->serializers) {
        return result_t<session_actor_t>::failure (framework_error_kind_t::protocol_error,
                                                   "actor create requires a serializer registry");
    }
    try {
        return create_erased (std::move (actor_type), std::move (actor_id),
                              detail::message_to_raw (request, *_state->serializers));
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<session_actor_t> (error);
    }
}

result_t<session_actor_t> session_actor_manager_t::create_erased (
  std::string actor_type, std::string actor_id, std::optional<zlink::message_t> request)
{
    if (actor_type.empty () || actor_id.empty ()) {
        return result_t<session_actor_t>::failure (framework_error_kind_t::protocol_error,
                                                   "actor type and id are required");
    }
    detail::actor_gateway_state_t::create_dispatcher_t dispatcher;
    {
        const std::lock_guard lock (_state->mutex);
        if (_state->actors_by_id.find (actor_id) != _state->actors_by_id.end ()) {
            return result_t<session_actor_t>::failure (framework_error_kind_t::already_exists,
                                                       "actor already exists");
        }
        dispatcher = _state->create_dispatcher;
    }
    std::optional<actor_ref_t> ref;
    if (dispatcher) {
        auto created = dispatcher (actor_type, actor_id, request);
        if (!created) {
            return result_t<session_actor_t>::failure (created.error_kind (),
                                                       created.error () ? created.error ()->what ()
                                                                        : "Actor creation failed");
        }
        ref = created.value ();
    } else {
        ref = ::zlink::framework::detail::actor_ref_access_t::make (
          node_rid_t::from_string (std::string (detail::local_actor_node_placeholder)), actor_type,
          actor_id, 1);
    }
    detail::actor_record_t record{*ref, false, false};
    record.create_payload = std::move (request);
    {
        const std::lock_guard lock (_state->mutex);
        const auto [_, inserted] = _state->actors_by_id.emplace (actor_id, std::move (record));
        if (!inserted) {
            return result_t<session_actor_t>::failure (framework_error_kind_t::already_exists,
                                                       "actor already exists");
        }
    }
    return result_t<session_actor_t>::success (session_actor_t (_state, *ref));
}

std::optional<session_actor_t> session_actor_manager_t::find (std::string actor_id) const
{
    std::uint64_t session_binding_token = 0;
    std::string session_id;
    if (_binding_context) {
        const std::lock_guard binding_lock (_binding_context->mutex);
        const auto binding = _binding_context->actor_tokens.find (actor_id);
        if (binding == _binding_context->actor_tokens.end ())
            return std::nullopt;
        session_binding_token = binding->second;
        session_id = _binding_context->session_id;
    }
    const std::lock_guard lock (_state->mutex);
    const auto found = _state->actors_by_id.find (actor_id);
    if (found == _state->actors_by_id.end () || !found->second.bound
        || found->second.disconnected
        || (session_binding_token != 0
            && (found->second.binding_token != session_binding_token
                || found->second.binding_session_id != session_id))) {
        return std::nullopt;
    }
    return session_actor_t (_state, found->second.ref,
                            session_binding_token != 0
                              ? session_binding_token
                              : found->second.binding_token);
}

result_t<session_actor_t> session_actor_manager_t::get_or_create (std::string actor_type,
                                                                  std::string actor_id)
{
    return get_or_create_erased (std::move (actor_type), std::move (actor_id), std::nullopt);
}

result_t<session_actor_t> session_actor_manager_t::get_or_create (std::string actor_type,
                                                                  std::string actor_id,
                                                                  const zlink::message_t &request)
{
    return get_or_create_erased (std::move (actor_type), std::move (actor_id), request);
}

result_t<session_actor_t> session_actor_manager_t::get_or_create (std::string actor_type,
                                                                  std::string actor_id,
                                                                  const message_t &request)
{
    if (!_state || !_state->serializers) {
        return result_t<session_actor_t>::failure (
          framework_error_kind_t::protocol_error,
          "actor get or create requires a serializer registry");
    }
    try {
        return get_or_create_erased (std::move (actor_type), std::move (actor_id),
                                     detail::message_to_raw (request, *_state->serializers));
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<session_actor_t> (error);
    }
}

result_t<session_actor_t> session_actor_manager_t::get_or_create_erased (
  std::string actor_type, std::string actor_id, std::optional<zlink::message_t> request)
{
    std::string current_session_id;
    if (_binding_context) {
        const std::lock_guard binding_lock (_binding_context->mutex);
        current_session_id = _binding_context->session_id;
    }
    {
        const std::lock_guard lock (_state->mutex);
        const auto found = _state->actors_by_id.find (actor_id);
        if (found != _state->actors_by_id.end ()) {
            const bool bound_to_other_session =
              found->second.bound && !found->second.binding_session_id.empty ()
              && !current_session_id.empty ()
              && found->second.binding_session_id != current_session_id;
            const auto stored_type =
              ::zlink::framework::detail::actor_ref_access_t::actor_type (found->second.ref);
            if (!stored_type.empty () && stored_type != actor_type) {
                return result_t<session_actor_t>::failure (
                  framework_error_kind_t::type_mismatch,
                  "actor id is already bound to another type");
            }
            if (stored_type.empty ()) {
                found->second.ref =
                  ::zlink::framework::detail::actor_ref_access_t::with_actor_type (
                    found->second.ref, actor_type);
            }
            if (!found->second.disconnected && !bound_to_other_session) {
                return result_t<session_actor_t>::success (
                  session_actor_t (_state, found->second.ref, found->second.binding_token));
            }

            /* A record owned by another session, or a disconnected record,
             * must not supply the new session with an old route snapshot.
             * Re-resolve the Actor through the create dispatcher so the
             * Location Store remains the authority for the current ref. */
            _state->bound_session_sinks.erase (actor_id);
            found->second.bound_session_stream_sink = false;
            found->second.bound_session_route.reset ();
            found->second.binding_session_id.clear ();
            found->second.binding_token = 0;
            _state->actors_by_id.erase (found);
        }
    }
    return create_erased (std::move (actor_type), std::move (actor_id), std::move (request));
}

zlink::message_t session_actor_manager_t::serialize_request (std::type_index request_type,
                                                             const void *request) const
{
    if (!_state || !_state->serializers) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "actor create requires a serializer registry");
    }
    return detail::encoded_payload_to_raw (_state->serializers->serialize (request_type, request));
}

request_call_t<session_actor_t> session_actor_manager_t::bind (actor_ref_t actor_ref)
{
    {
        const std::lock_guard lock (_state->mutex);
        if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
            return request_call_t<session_actor_t> (result_t<session_actor_t>::failure (
              framework_error_kind_t::not_found, "actor ref is empty"));
        }
        auto found = _state->actors_by_id.find (std::string (actor_ref.actor_id ().value ()));
        if (found != _state->actors_by_id.end ()) {
            if (!actor_types_compatible (found->second.ref, actor_ref)) {
                return request_call_t<session_actor_t> (
                  result_t<session_actor_t>::failure (framework_error_kind_t::type_mismatch,
                                                      "actor id is already bound to another type"));
            }
            if (found->second.ref.object_generation () != actor_ref.object_generation ()) {
                return request_call_t<session_actor_t> (result_t<session_actor_t>::failure (
                  framework_error_kind_t::invalid_operation, "actor generation is stale"));
            }
            actor_ref = merge_actor_type (actor_ref, found->second.ref);
        }
    }
    return request_call_t<session_actor_t> (
      "BindSessionActor",
      [manager = *this, actor_ref] (const std::string &,
                                   std::chrono::milliseconds,
                                   const request_call_t<session_actor_t>::metadata_map_t &)
        mutable -> task_t<session_actor_t> {
          return manager.bind_current_session (actor_ref, false);
      });
}

request_call_t<session_actor_t> session_actor_manager_t::bind_or_get (actor_ref_t actor_ref)
{
    {
        const std::lock_guard lock (_state->mutex);
        if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
            return request_call_t<session_actor_t> (result_t<session_actor_t>::failure (
              framework_error_kind_t::not_found, "actor ref is empty"));
        }
        auto found = _state->actors_by_id.find (std::string (actor_ref.actor_id ().value ()));
        if (found != _state->actors_by_id.end ()) {
            if (!actor_types_compatible (found->second.ref, actor_ref)) {
                return request_call_t<session_actor_t> (
                  result_t<session_actor_t>::failure (framework_error_kind_t::type_mismatch,
                                                      "actor id is already bound to another type"));
            }
            actor_ref = merge_actor_type (actor_ref, found->second.ref);
            if (found->second.ref.object_generation ()
                > actor_ref.object_generation ()) {
                actor_ref = found->second.ref;
            }
        }
    }
    return request_call_t<session_actor_t> (
      "BindOrGetSessionActor",
      [manager = *this, actor_ref] (const std::string &,
                                   std::chrono::milliseconds,
                                   const request_call_t<session_actor_t>::metadata_map_t &)
        mutable -> task_t<session_actor_t> {
          return manager.bind_current_session (actor_ref, true);
      });
}

task_t<session_actor_t>
session_actor_manager_t::bind_current_session (actor_ref_t actor_ref,
                                               bool reuse_current)
{
    const auto state = _state;
    const auto binding_context = _binding_context;
    const auto actor_id = std::string (actor_ref.actor_id ().value ());
    const auto publish_without_stream = [state, actor_id, actor_ref] {
        const std::lock_guard lock (state->mutex);
        auto found = state->actors_by_id.find (actor_id);
        if (found == state->actors_by_id.end ()) {
            state->actors_by_id.emplace (
              actor_id, detail::actor_record_t{actor_ref, true, false});
            return;
        }
        if (!actor_types_compatible (found->second.ref, actor_ref)) {
            throw framework_exception_t (
              framework_error_kind_t::type_mismatch,
              "actor id is already bound to another type");
        }
        if (found->second.ref.object_generation () != actor_ref.object_generation ()) {
            throw framework_exception_t (
              framework_error_kind_t::invalid_operation,
              "actor generation changed during Session binding");
        }
        found->second.ref = merge_actor_type (actor_ref, found->second.ref);
        found->second.bound = true;
        found->second.disconnected = false;
    };
    if (!binding_context) {
        publish_without_stream ();
        co_return session_actor_t (state, actor_ref, 0);
    }
    std::unique_lock binding_lock (binding_context->mutex);
    if (!binding_context->stream) {
        binding_lock.unlock ();
        publish_without_stream ();
        co_return session_actor_t (state, actor_ref, 0);
    }
    if (reuse_current) {
        const auto current = binding_context->actor_tokens.find (actor_id);
        const auto bound_stream = binding_context->actor_streams.find (actor_id);
        if (current != binding_context->actor_tokens.end ()
            && binding_context->ready_actors.contains (actor_id)
            && bound_stream != binding_context->actor_streams.end ()
            && bound_stream->second.lock () == binding_context->stream_state) {
            bool current_binding_matches = false;
            {
                const std::lock_guard lock (state->mutex);
                const auto record = state->actors_by_id.find (actor_id);
                current_binding_matches =
                  record != state->actors_by_id.end ()
                  && record->second.bound
                  && !record->second.disconnected
                  && record->second.binding_session_id
                       == binding_context->session_id
                  && record->second.binding_token == current->second
                  && record->second.ref.object_generation ()
                       == actor_ref.object_generation ();
            }
            if (current_binding_matches) {
                const auto token = current->second;
                binding_lock.unlock ();
                co_return session_actor_t (state, actor_ref, token);
            }
        }
    }
    std::uint64_t token;
    {
        const std::lock_guard lock (state->mutex);
        if (state->next_binding_token == 0
            || state->next_binding_token
                 == std::numeric_limits<std::uint64_t>::max ()) {
            throw framework_exception_t (
              framework_error_kind_t::invalid_operation,
              "Session binding generation is exhausted");
        }
        token = state->next_binding_token++;
    }
    const auto prior_context_token =
      binding_context->actor_tokens.find (actor_id);
    const auto previous_context_token =
      prior_context_token == binding_context->actor_tokens.end ()
        ? std::optional<std::uint64_t>{}
        : std::make_optional (prior_context_token->second);
    const auto previous_ready =
      binding_context->ready_actors.contains (actor_id);
    const auto prior_stream = binding_context->actor_streams.find (actor_id);
    const auto had_previous_stream =
      prior_stream != binding_context->actor_streams.end ();
    const auto previous_stream = had_previous_stream
      ? prior_stream->second
      : std::weak_ptr<detail::stream_state_t>{};
    binding_context->actor_tokens[actor_id] = token;
    binding_context->ready_actors.erase (actor_id);
    binding_context->actor_streams[actor_id] = binding_context->stream_state;
    std::optional<detail::actor_session_binding_snapshot_t> previous;
    try {
        previous = detail::actor_gateway_runtime_t (state).bind_session_stream (
          actor_id, *binding_context->stream, binding_context->codec,
          binding_context->session_id, token, actor_ref);
        auto native_binder = binding_context->native_binder;
        binding_lock.unlock ();
        if (native_binder)
            co_await native_binder (actor_ref, token);
        binding_lock.lock ();
        const auto current = binding_context->actor_tokens.find (actor_id);
        if (current != binding_context->actor_tokens.end ()
            && current->second == token) {
            binding_context->ready_actors.insert (actor_id);
        }
    }
    catch (...) {
        if (!binding_lock.owns_lock ())
            binding_lock.lock ();
        const auto current = binding_context->actor_tokens.find (actor_id);
        if (current != binding_context->actor_tokens.end () && current->second == token) {
            if (previous_context_token) {
                current->second = *previous_context_token;
            } else {
                binding_context->actor_tokens.erase (current);
            }
        }
        if (previous_ready) {
            binding_context->ready_actors.insert (actor_id);
        } else {
            binding_context->ready_actors.erase (actor_id);
        }
        if (had_previous_stream) {
            binding_context->actor_streams[actor_id] = previous_stream;
        } else {
            binding_context->actor_streams.erase (actor_id);
        }
        if (previous) {
            detail::actor_gateway_runtime_t (state).restore_session_stream (
              actor_id, binding_context->session_id, token,
              std::move (*previous));
        }
        throw;
    }
    binding_lock.unlock ();
    co_return session_actor_t (state, actor_ref, token);
}

void detail::session_actor_manager_access_t::attach (session_actor_manager_t &manager,
                                                     stream_t stream)
{
    stream._state->actors.store (&manager, std::memory_order_release);
    const std::lock_guard lock (manager._binding_context->mutex);
    manager._binding_context->session_id = stream.session_id ();
    manager._binding_context->stream_state = stream._state;
    manager._binding_context->stream = std::move (stream);
}

void detail::session_actor_manager_access_t::set_codec (session_actor_manager_t &manager,
                                                        stream_codec_t codec)
{
    const std::lock_guard lock (manager._binding_context->mutex);
    manager._binding_context->codec = codec;
}

void detail::session_actor_manager_access_t::bind_native (
  session_actor_manager_t &manager,
  std::function<task_t<void> (actor_ref_t, std::uint64_t)> binder)
{
    const std::lock_guard lock (manager._binding_context->mutex);
    manager._binding_context->native_binder = std::move (binder);
}

void detail::session_actor_manager_access_t::disconnect (session_actor_manager_t &manager) noexcept
{
    if (!manager._binding_context) {
        return;
    }
    std::map<std::string, std::uint64_t> bindings;
    std::string session_id;
    {
        const std::lock_guard lock (manager._binding_context->mutex);
        bindings = manager._binding_context->actor_tokens;
        manager._binding_context->actor_tokens.clear ();
        manager._binding_context->ready_actors.clear ();
        manager._binding_context->actor_streams.clear ();
        session_id = manager._binding_context->session_id;
        if (manager._binding_context->stream) {
            manager._binding_context->stream->_state->actors.store (nullptr,
                                                                    std::memory_order_release);
        }
        manager._binding_context->stream.reset ();
        manager._binding_context->stream_state.reset ();
    }
    for (const auto &[actor_id, token] : bindings) {
        detail::actor_gateway_state_t::disconnect_dispatcher_t dispatcher;
        std::optional<actor_ref_t> actor;
        {
            const std::lock_guard lock (manager._state->mutex);
            const auto found = manager._state->actors_by_id.find (actor_id);
            if (found != manager._state->actors_by_id.end ()
                && found->second.binding_session_id == session_id
                && found->second.binding_token == token && found->second.bound
                && !found->second.disconnected) {
                actor = found->second.ref;
                found->second.bound = false;
                found->second.disconnected = true;
                dispatcher = manager._state->disconnect_dispatcher;
            }
        }
        if (dispatcher && actor
            && !::zlink::framework::detail::actor_ref_access_t::empty (*actor)) {
            try {
                (void) dispatcher (*actor);
            }
            catch (...) {
            }
        }
        detail::actor_gateway_runtime_t (manager._state)
          .unbind_session_stream (actor_id, session_id, token);
    }
}

} // namespace zlink::framework

namespace zlink::framework::detail
{

namespace
{

bool same_physical_bound_session (const actor_bound_session_route_t &left,
                                  const actor_bound_session_route_t &right)
{
    // Actor authority changes on relocation while the physical Session and
    // its binding generation remain current. Only a physical identity change
    // retires the existing stream binding.
    return left.node_rid.to_bytes () == right.node_rid.to_bytes ()
           && left.session_rid == right.session_rid
           && left.object_generation == right.object_generation
           && left.node_generation == right.node_generation
           && left.binding_generation == right.binding_generation
           && left.binding_token == right.binding_token;
}

actor_bound_session_route_t
merge_bound_session_route_fence (const actor_bound_session_route_t &current,
                                 actor_bound_session_route_t next)
{
    if (next.binding_token == 0)
        next.binding_token = current.binding_token;
    next.session_sequence = std::max (
      current.session_sequence, next.session_sequence);
    //  A known baseline survives a re-registration that arrives without one.
    next.session_sequence_baseline_unknown =
      next.session_sequence_baseline_unknown
      && current.session_sequence_baseline_unknown;
    return next;
}

std::shared_ptr<bound_session_sink_t>
make_session_owner_sink (
  std::weak_ptr<actor_gateway_state_t> weak_state,
  actor_ref_t staged_actor,
  actor_bound_session_route_t staged_route)
{
    const auto actor_id =
      std::string (staged_actor.actor_id ().value ());
    return std::make_shared<bound_session_sink_t> (
      [weak_state = std::move (weak_state),
       staged_actor = std::move (staged_actor),
       staged_route = std::move (staged_route),
       actor_id] (std::string packet_name,
                  stream_codec_t codec,
                  const zlink::message_t &payload) mutable {
          const auto state = weak_state.lock ();
          if (!state) {
              return task_t<void> (result_t<void>::failure (
                framework_error_kind_t::shutting_down,
                "bound Session route owner was released"));
          }
          actor_gateway_state_t::bound_session_sender_t sender;
          std::uint64_t binding_generation = 0;
          {
              std::unique_lock lock (state->mutex);
              const auto found = state->actors_by_id.find (actor_id);
              //  The fence guards against the route being REPLACED between
              //  staging and send. Relay progress (session_sequence and its
              //  baseline flag) advances on the same route while it stays
              //  current, so identity/authority fields alone participate.
              const auto same_route_fence =
                [] (const actor_bound_session_route_t &live,
                    const actor_bound_session_route_t &staged) {
                    return live.node_rid.to_bytes ()
                             == staged.node_rid.to_bytes ()
                           && live.session_rid == staged.session_rid
                           && live.object_generation == staged.object_generation
                           && live.node_generation == staged.node_generation
                           && live.authority_owner_generation
                                == staged.authority_owner_generation
                           && live.owner_lease_generation
                                == staged.owner_lease_generation
                           && live.binding_generation == staged.binding_generation
                           && live.binding_token == staged.binding_token;
                };
              if (found == state->actors_by_id.end ()
                  || !found->second.bound
                  || found->second.disconnected
                  || !found->second.bound_session_route
                  || !same_route_fence (*found->second.bound_session_route,
                                        staged_route)) {
                  //  spec 26 Detailed diagnostics: name the exact reason a
                  //  bound-session push was refused so a silent drop at the
                  //  application layer stays attributable.
                  const auto describe =
                    [] (const actor_bound_session_route_t &fence) {
                        return fence.node_rid.to_hex () + "/bg="
                               + std::to_string (fence.binding_generation)
                               + "/bt=" + std::to_string (fence.binding_token)
                               + "/og=" + std::to_string (fence.object_generation)
                               + "/ng=" + std::to_string (fence.node_generation)
                               + "/ag="
                               + std::to_string (fence.authority_owner_generation)
                               + "/lg="
                               + std::to_string (fence.owner_lease_generation);
                    };
                  detail::message_flow_tracer_t (state->dispatch)
                    .trace (message_flow_log_mode_t::detailed,
                            message_flow_outcome_t::dropped, [&] {
                        auto event = message_flow_event_t{
                          message_flow_outcome_t::dropped,
                          dispatch_error_surface_t::stream_session,
                          dispatch_message_kind_t::send,
                          std::string ("bound_session_push")};
                        event.detail_stage = "route_fence";
                        event.detail_result = found == state->actors_by_id.end ()
                            ? std::string ("reason=actor-missing")
                            : !found->second.bound
                                ? std::string ("reason=not-bound")
                                : found->second.disconnected
                                    ? std::string ("reason=disconnected")
                                    : !found->second.bound_session_route
                                        ? std::string ("reason=no-route")
                                        : "live="
                                            + describe (
                                                *found->second
                                                   .bound_session_route)
                                             + " staged=" + describe (staged_route);
                        event.actor_id = actor_id;
                        event.reason = message_flow_reason_t::stale_target;
                        return event;
                    });
                  lock.unlock ();
                  return task_t<void> (result_t<void>::failure (
                    framework_error_kind_t::not_configured,
                    "bound Session route fence changed before send"));
              }
              sender = state->bound_session_sender;
              binding_generation =
                found->second.bound_session_route
                  ->binding_generation;
          }
          if (!sender) {
              return task_t<void> (result_t<void>::failure (
                framework_error_kind_t::not_configured,
                "bound Session route sender is not configured"));
          }
          try {
              const stream_header_t header (
                stream_message_kind_t::send, codec,
                stream_header_flags_t::none, std::nullopt,
                std::move (packet_name));
              auto sent = sender (
                staged_actor, binding_generation, header, payload);
              return [] (task_t<result_t<void>> pending) -> task_t<void> {
                  const auto result = co_await pending;
                  if (!result) {
                      throw result.error ()
                              ? *result.error ()
                              : framework_exception_t (
                                  framework_error_kind_t::internal_failure,
                                  "bound Session route send failed");
                  }
              } (std::move (sent));
          }
          catch (const framework_exception_t &error) {
              return task_t<void> (
                result_access_t::failure<void> (error));
          }
          catch (const std::exception &error) {
              return task_t<void> (result_t<void>::failure (
                framework_error_kind_t::internal_failure,
                error.what ()));
          }
      });
}

result_t<actor_bound_session_route_t *>
exact_session_relay_route (actor_gateway_state_t &state,
                           const actor_ref_t &actor_ref,
                           const zlink::routing_id_t &source_node_rid,
                           const zlink::routing_id_t &session_rid,
                           std::uint64_t binding_generation)
{
    const auto found = state.actors_by_id.find (
      std::string (actor_ref.actor_id ().value ()));
    if (found == state.actors_by_id.end () || !found->second.bound_session_route) {
        return result_t<actor_bound_session_route_t *>::failure (
          framework_error_kind_t::not_configured,
          "bound Session relay route is not registered");
    }
    auto &route = *found->second.bound_session_route;
    if (found->second.ref.object_generation () != actor_ref.object_generation ()
        || route.node_rid.to_hex () != source_node_rid.to_hex () || !route.session_rid
        || route.session_rid->to_hex () != session_rid.to_hex ()
        || route.binding_generation != binding_generation) {
        return result_t<actor_bound_session_route_t *>::failure (
          framework_error_kind_t::invalid_operation,
          "bound Session relay source fence is stale");
    }
    return result_t<actor_bound_session_route_t *>::success (&route);
}

result_t<void> bind_session_components (const std::shared_ptr<actor_gateway_state_t> &state,
                                        actor_ref_t actor_ref,
                                        bound_session_sink_t sink,
                                        stream_codec_t codec,
                                        bool replace_existing,
                                        std::optional<actor_bound_session_route_t> route,
                                        actor_bound_session_transition_t *transition = nullptr)
{
    const auto actor_id = std::string (actor_ref.actor_id ().value ());
    const std::lock_guard lock (state->mutex);
    auto found = state->actors_by_id.find (actor_id);
    bool keep_existing_sink = false;
    if (found == state->actors_by_id.end ()) {
        actor_record_t record{actor_ref, true, false, codec};
        if (route) {
            route->object_generation = actor_ref.object_generation ();
            record.bound_session_route = route;
            if (transition != nullptr) {
                transition->current = *route;
                transition->changed = true;
            }
        }
        state->actors_by_id.emplace (actor_id, std::move (record));
    } else {
        if (!actor_types_compatible (found->second.ref, actor_ref)) {
            return result_t<void>::failure (framework_error_kind_t::type_mismatch,
                                            "actor id is already bound to another type");
        }
        if (found->second.ref.object_generation () != actor_ref.object_generation ()) {
            return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                            "actor generation is stale");
        }
        const auto existing_sink = state->bound_session_sinks.find (actor_id);
        if (route && !replace_existing && found->second.bound && !found->second.disconnected
            && existing_sink != state->bound_session_sinks.end ()) {
            /* A non-replacing route bind must preserve the complete existing
             * sink/fence pair. Updating only the route would make later sends
             * advance a new fence through the old sink. */
            return result_t<void>::success ();
        }
        if (route && route->binding_generation == 0)
            route->binding_generation = found->second.source_binding_generation;
        if (route && found->second.bound_session_route
            && route->binding_generation != 0
            && found->second.bound_session_route->binding_generation
                 > route->binding_generation) {
            if (transition != nullptr) {
                transition->current =
                  *found->second.bound_session_route;
                transition->changed = false;
            }
            return result_t<void>::success ();
        }
        /* A replacing bind owns the route and its delivery capability as one
         * atomic value.  Preserving a direct STREAM sink while publishing a
         * different physical route leaves Actor pushes targeting the retired
         * connection even though the route fence reports the successor. */
        keep_existing_sink = !replace_existing && found->second.bound
                             && !found->second.disconnected
                             && existing_sink != state->bound_session_sinks.end ();
        actor_ref = merge_actor_type (actor_ref, found->second.ref);
        found->second.ref = actor_ref;
        found->second.bound = true;
        found->second.disconnected = false;
        found->second.bound_session_codec = codec;
        if (route) {
            if (route->binding_token == 0) {
                route->binding_token = found->second.bound_session_stream_sink
                    && found->second.binding_token != 0
                  ? found->second.binding_token
                : found->second.bound_session_route
                  ? found->second.bound_session_route->binding_token
                  : 0;
            }
            route->object_generation = actor_ref.object_generation ();
            if (found->second.bound_session_route
                && same_physical_bound_session (*found->second.bound_session_route, *route)) {
                /* Relocation can refresh authority for the same physical
                 * Session.  Its already-attached direct writer remains the
                 * right capability; only a physical identity replacement
                 * must swap it. */
                keep_existing_sink = found->second.bound_session_stream_sink
                                     && existing_sink
                                          != state->bound_session_sinks.end ();
                *route = merge_bound_session_route_fence (
                  *found->second.bound_session_route, std::move (*route));
                found->second.bound_session_route = *route;
                if (!keep_existing_sink) {
                    found->second.bound_session_stream_sink = false;
                    state->bound_session_sinks[actor_id] =
                      std::make_shared<bound_session_sink_t> (
                        std::move (sink));
                }
                if (transition != nullptr) {
                    transition->current = *route;
                    transition->changed = false;
                }
                return result_t<void>::success ();
            }
            if (transition != nullptr) {
                transition->previous = found->second.bound_session_route;
                transition->current = *route;
                transition->changed = true;
            }
            found->second.bound_session_route = route;
        }
    }
    if (!keep_existing_sink) {
        const auto current = state->actors_by_id.find (actor_id);
        if (current != state->actors_by_id.end ())
            current->second.bound_session_stream_sink = false;
        state->bound_session_sinks[actor_id] =
          std::make_shared<bound_session_sink_t> (std::move (sink));
    }
    return result_t<void>::success ();
}

} // namespace

actor_gateway_runtime_t::actor_gateway_runtime_t () :
    _state (std::make_shared<actor_gateway_state_t> ())
{
}

actor_gateway_runtime_t::actor_gateway_runtime_t (std::shared_ptr<actor_gateway_state_t> state) :
    _state (std::move (state))
{
}

session_actor_manager_t actor_gateway_runtime_t::manager () const
{
    return session_actor_manager_t (_state);
}

std::vector<relayed_frame_t> actor_gateway_runtime_t::relayed_frames () const
{
    const std::lock_guard lock (_state->mutex);
    return _state->relayed_frames;
}

std::vector<relayed_frame_t> actor_gateway_runtime_t::bound_session_pushes () const
{
    const std::lock_guard lock (_state->mutex);
    return _state->bound_session_pushes;
}

std::optional<actor_bound_session_route_t>
actor_gateway_runtime_t::bound_session_route (const actor_ref_t &actor_ref) const
{
    const std::lock_guard lock (_state->mutex);
    const auto found = _state->actors_by_id.find (std::string (actor_ref.actor_id ().value ()));
    if (found == _state->actors_by_id.end () || !found->second.bound
        || !actor_types_compatible (found->second.ref, actor_ref)
        || found->second.ref.object_generation () != actor_ref.object_generation ()
        || (found->second.bound_session_route
            && found->second.bound_session_route->object_generation
                 != actor_ref.object_generation ())) {
        return std::nullopt;
    }
    return found->second.bound_session_route;
}

std::optional<actor_bound_session_route_t>
actor_gateway_runtime_t::resolve_bound_session_push_route (
  const actor_ref_t &actor_ref,
  const actor_bound_session_route_t &staged_route) const
{
    const auto actor_id = std::string (
      actor_ref.actor_id ().value ());
    std::optional<actor_bound_session_route_t> current;
    {
        const std::lock_guard lock (_state->mutex);
        const auto found = _state->actors_by_id.find (actor_id);
        if (found != _state->actors_by_id.end ()
            && found->second.bound
            && !found->second.disconnected
            && actor_types_compatible (found->second.ref, actor_ref)
            && found->second.ref.object_generation ()
                 == actor_ref.object_generation ()
            && found->second.bound_session_route
            && found->second.bound_session_route->object_generation
                 == actor_ref.object_generation ()) {
            current = *found->second.bound_session_route;
        }
    }

    const auto describe = [] (
                            const std::optional<actor_bound_session_route_t> &route) {
        if (!route)
            return std::string ("none");
        return "session_rid="
               + (route->session_rid ? route->session_rid->to_hex ()
                                     : std::string ("none"))
               + "/bg=" + std::to_string (route->binding_generation);
    };
    trace_detached_bound_session_send_stage (
      _state, actor_id, "actor_owner_push_target",
      "current=" + describe (current)
        + " staged=" + describe (staged_route));
    return current;
}

bool actor_gateway_runtime_t::actor_bound (std::string actor_id) const
{
    const std::lock_guard lock (_state->mutex);
    const auto found = _state->actors_by_id.find (actor_id);
    return found != _state->actors_by_id.end () && found->second.bound;
}

bool actor_gateway_runtime_t::actor_disconnected (std::string actor_id) const
{
    const std::lock_guard lock (_state->mutex);
    const auto found = _state->actors_by_id.find (actor_id);
    return found != _state->actors_by_id.end () && found->second.disconnected;
}

actor_context_t
actor_gateway_runtime_t::actor_context (const actor_ref_t &actor_ref,
                                        std::uint64_t source_binding_generation) const
{
    const std::lock_guard lock (_state->mutex);
    const auto found = _state->actors_by_id.find (std::string (actor_ref.actor_id ().value ()));
    if (found == _state->actors_by_id.end ())
        return actor_context_t (_state, actor_ref, source_binding_generation);
    if (source_binding_generation != 0)
        found->second.source_binding_generation = source_binding_generation;
    return actor_context_t (_state, actor_ref, found->second.source_binding_generation);
}

bool actor_gateway_runtime_t::same_context_source_fence (
  const actor_context_t &left, const actor_context_t &right) const noexcept
{
    return left.has_same_source_fence (right);
}

result_t<void> actor_gateway_runtime_t::update_actor_ref (const actor_ref_t &actor_ref)
{
    const std::lock_guard lock (_state->mutex);
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<void>::failure (framework_error_kind_t::not_found, "actor ref is empty");
    }
    const auto found = _state->actors_by_id.find (std::string (actor_ref.actor_id ().value ()));
    if (found == _state->actors_by_id.end ()) {
        return result_t<void>::success ();
    }
    if (!actor_types_compatible (found->second.ref, actor_ref)) {
        return result_t<void>::failure (framework_error_kind_t::type_mismatch,
                                        "actor id is already bound to another type");
    }
    if (found->second.ref.object_generation () != actor_ref.object_generation ()) {
        return result_t<void>::failure (
          framework_error_kind_t::invalid_operation,
          "actor generation is stale. actor=" + std::string (actor_ref.actor_id ().value ())
            + ", current=" + std::to_string (found->second.ref.object_generation ())
            + ", received=" + std::to_string (actor_ref.object_generation ()));
    }
    if (found->second.bound && !found->second.disconnected
        && found->second.ref.node_rid ().value ()
             != actor_ref.node_rid ().value ()) {
        /* The Session route transaction publishes the ActorRef and its
         * authority fence together. A target materialization callback can
         * arrive before command 44 reaches the Session owner; retaining the
         * previous ref here prevents that callback from exposing a mixed
         * old-Session/new-Actor route. */
        return result_t<void>::success ();
    }
    found->second.ref = merge_actor_type (actor_ref, found->second.ref);
    return result_t<void>::success ();
}

result_t<void> actor_gateway_runtime_t::destroy_actor (const actor_ref_t &actor_ref)
{
    const std::lock_guard lock (_state->mutex);
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<void>::failure (framework_error_kind_t::not_found, "actor ref is empty");
    }
    const auto found = _state->actors_by_id.find (std::string (actor_ref.actor_id ().value ()));
    if (found == _state->actors_by_id.end ()) {
        return result_t<void>::success ();
    }
    if (!actor_types_compatible (found->second.ref, actor_ref)) {
        return result_t<void>::failure (framework_error_kind_t::type_mismatch,
                                        "actor id is already bound to another type");
    }
    if (found->second.ref.object_generation () != actor_ref.object_generation ()) {
        return result_t<void>::success ();
    }
    _state->bound_session_sinks.erase (std::string (actor_ref.actor_id ().value ()));
    _state->actors_by_id.erase (found);
    return result_t<void>::success ();
}

actor_session_binding_snapshot_t
actor_gateway_runtime_t::bind_session_stream (
  std::string actor_id,
  stream_t stream,
  stream_codec_t codec,
  std::string session_id,
  std::uint64_t binding_token,
  std::optional<actor_ref_t> actor_ref)
{
    actor_session_binding_snapshot_t previous;
    std::optional<actor_ref_t> registered_actor;
    actor_gateway_state_t::bound_session_registrar_t registrar;
    {
        const std::lock_guard lock (_state->mutex);
        auto found = _state->actors_by_id.find (actor_id);
        if (found != _state->actors_by_id.end ()) {
            previous.record = found->second;
        }
        if (const auto sink = _state->bound_session_sinks.find (actor_id);
            sink != _state->bound_session_sinks.end ()) {
            previous.sink = sink->second;
        }
        if (found == _state->actors_by_id.end ()) {
            if (!actor_ref
                || ::zlink::framework::detail::actor_ref_access_t::empty (
                  *actor_ref)) {
                throw framework_exception_t (
                  framework_error_kind_t::not_found,
                  "actor record is missing during Session binding");
            }
            found = _state->actors_by_id
                      .emplace (
                        actor_id,
                        actor_record_t{.ref = *actor_ref,
                                       .bound = false,
                                       .disconnected = true})
                      .first;
        } else if (actor_ref) {
            if (!actor_types_compatible (found->second.ref, *actor_ref)) {
                throw framework_exception_t (
                  framework_error_kind_t::type_mismatch,
                  "actor id is already bound to another type");
            }
            if (found->second.ref.object_generation ()
                != actor_ref->object_generation ()) {
                throw framework_exception_t (
                  framework_error_kind_t::invalid_operation,
                  "actor generation changed during Session binding");
            }
            found->second.ref = merge_actor_type (*actor_ref, found->second.ref);
        }
        found->second.bound = true;
        found->second.disconnected = false;
        found->second.bound_session_codec = codec;
        found->second.bound_session_stream_sink = true;
        registered_actor = found->second.ref;
        found->second.binding_session_id = session_id;
        found->second.binding_token = binding_token;
        found->second.next_session_relay_sequence = 1;
        _state->bound_session_sinks[actor_id] = std::make_shared<detail::bound_session_sink_t> (
          [stream = std::move (stream)] (std::string packet_name, stream_codec_t payload_codec,
                                         const zlink::message_t &payload) mutable {
              stream_header_t header (stream_message_kind_t::send, payload_codec,
                                      stream_header_flags_t::none, std::nullopt,
                                      std::move (packet_name));
              try {
                  stream.write_packet_with_header (std::move (header), payload)
                    .submit ()
                    .result ()
                    .value ();
                  return task_t<void> (result_t<void>::success ());
              }
              catch (const framework_exception_t &error) {
                  return task_t<void> (detail::result_access_t::failure<void> (error));
              }
          });
        registrar = _state->bound_session_registrar;
    }
    if (registrar && registered_actor
        && !::zlink::framework::detail::actor_ref_access_t::empty (
          *registered_actor)) {
        auto registered = registrar (*registered_actor);
        if (!registered) {
            restore_session_stream (
              actor_id, session_id, binding_token, std::move (previous));
            throw framework_exception_t (registered.error_kind (),
                                         registered.error ()
                                           ? registered.error ()->what ()
                                           : "bound session route registration failed");
        }
    }
    return previous;
}

result_t<void>
actor_gateway_runtime_t::bind_session_route (actor_ref_t actor_ref,
                                             route_client_t route_client,
                                             std::string route_channel_name,
                                             zlink::routing_id_t target_node_rid,
                                             stream_codec_t codec,
                                             bool replace_existing,
                                             std::optional<zlink::routing_id_t> session_rid)
{
    const auto actor_id = std::string (actor_ref.actor_id ().value ());
    actor_bound_session_route_t route{target_node_rid, std::move (session_rid),
                                      actor_ref.object_generation ()};
    return bind_session_components (
      _state, std::move (actor_ref),
      [state = _state, actor_id, route_client = std::move (route_client),
       route_channel_name = std::move (route_channel_name),
       target_node_rid = std::move (target_node_rid)] (std::string packet_name,
                                                       stream_codec_t payload_codec,
                                                       const zlink::message_t &payload) mutable {
          std::optional<actor_ref_t> current_actor_ref;
          {
              std::unique_lock lock (state->mutex);
              const auto found = state->actors_by_id.find (actor_id);
              if (found == state->actors_by_id.end () || !found->second.bound) {
                  lock.unlock ();
                  return task_t<void> (result_t<void>::failure (
                    framework_error_kind_t::not_configured, "actor session is not bound"));
              }
              current_actor_ref = found->second.ref;
          }
          try {
              route_client
                .send_to_node (route_channel_name, target_node_rid,
                               make_actor_bound_session_route_request (
                                 *current_actor_ref, packet_name, payload_codec, payload))
                .submit ()
                .result ()
                .value ();
              return task_t<void> (result_t<void>::success ());
          }
          catch (const framework_exception_t &error) {
              return task_t<void> (detail::result_access_t::failure<void> (error));
          }
      },
      codec, replace_existing, std::move (route));
}

result_t<void> actor_gateway_runtime_t::bind_session_sink (
  actor_ref_t actor_ref,
  std::function<task_t<void> (std::string, stream_codec_t, const zlink::message_t &)> sink,
  stream_codec_t codec,
  bool replace_existing)
{
    return bind_session_components (_state, std::move (actor_ref), std::move (sink), codec,
                                    replace_existing, std::nullopt);
}

result_t<void> actor_gateway_runtime_t::bind_session_route (
  actor_ref_t actor_ref,
  std::function<task_t<void> (std::string, stream_codec_t, const zlink::message_t &)> sink,
  actor_bound_session_route_t route,
  stream_codec_t codec,
  bool replace_existing)
{
    return bind_session_components (_state, std::move (actor_ref), std::move (sink), codec,
                                    replace_existing, std::move (route));
}

result_t<actor_bound_session_transition_t>
actor_gateway_runtime_t::replace_session_route (actor_ref_t actor_ref,
                                                bound_session_sink_t sink,
                                                actor_bound_session_route_t route,
                                                stream_codec_t codec)
{
    actor_bound_session_transition_t transition;
    const auto bound = bind_session_components (_state, std::move (actor_ref), std::move (sink),
                                                codec, true, std::move (route), &transition);
    if (!bound) {
        return result_t<actor_bound_session_transition_t>::failure (
          bound.error_kind (),
          bound.error () ? bound.error ()->what () : "bound Session route replacement failed");
    }
    return result_t<actor_bound_session_transition_t>::success (std::move (transition));
}

result_t<void>
actor_gateway_runtime_t::record_bound_session_route (const actor_ref_t &actor_ref,
                                                     zlink::routing_id_t node_rid,
                                                     std::optional<zlink::routing_id_t> session_rid,
                                                     std::uint64_t node_generation,
                                                     std::uint64_t authority_owner_generation,
                                                     std::uint64_t owner_lease_generation,
                                                     std::uint64_t binding_generation,
                                                     std::uint64_t binding_token,
                                                     std::uint64_t session_sequence,
                                                     bool session_sequence_baseline_unknown)
{
    auto transition = record_bound_session_route_transition (
      actor_ref, actor_bound_session_route_t{std::move (node_rid), std::move (session_rid),
                                             actor_ref.object_generation (), node_generation,
                                             authority_owner_generation, owner_lease_generation,
                                             binding_generation, binding_token, session_sequence,
                                             session_sequence_baseline_unknown});
    if (!transition) {
        return result_t<void>::failure (transition.error_kind (),
                                        transition.error ()
                                          ? transition.error ()->what ()
                                          : "bound Session route registration failed");
    }
    return result_t<void>::success ();
}

result_t<actor_bound_session_transition_t>
actor_gateway_runtime_t::record_bound_session_route_transition (const actor_ref_t &actor_ref,
                                                                actor_bound_session_route_t route)
{
    const auto actor_id = std::string (actor_ref.actor_id ().value ());
    const std::lock_guard lock (_state->mutex);
    auto found = _state->actors_by_id.find (actor_id);
    if (found == _state->actors_by_id.end ()) {
        found = _state->actors_by_id
                  .emplace (actor_id,
                            actor_record_t{.ref = actor_ref, .bound = true, .disconnected = false})
                  .first;
    } else {
        if (!actor_types_compatible (found->second.ref, actor_ref)) {
            return result_t<actor_bound_session_transition_t>::failure (
              framework_error_kind_t::type_mismatch, "actor id is already bound to another type");
        }
        if (found->second.ref.object_generation () != actor_ref.object_generation ()) {
            return result_t<actor_bound_session_transition_t>::failure (
              framework_error_kind_t::invalid_operation, "actor generation is stale");
        }
        if (route.binding_generation == 0)
            route.binding_generation = found->second.source_binding_generation;
        if (found->second.bound_session_route
            && route.binding_generation != 0
            && found->second.bound_session_route->binding_generation
                 > route.binding_generation) {
            actor_bound_session_transition_t transition;
            transition.current = *found->second.bound_session_route;
            return result_t<actor_bound_session_transition_t>::success (
              std::move (transition));
        }
        if (actor_ref_access_t::actor_type (found->second.ref).empty ()
            && !actor_ref_access_t::actor_type (actor_ref).empty ()) {
            found->second.ref = merge_actor_type (actor_ref, found->second.ref);
        }
        found->second.bound = true;
        found->second.disconnected = false;
    }
    if (route.binding_generation == 0)
        route.binding_generation = found->second.source_binding_generation;
    if (route.binding_token == 0) {
        route.binding_token = found->second.bound_session_stream_sink
            && found->second.binding_token != 0
          ? found->second.binding_token
        : found->second.bound_session_route
          ? found->second.bound_session_route->binding_token
          : 0;
    }
    route.object_generation = actor_ref.object_generation ();
    actor_bound_session_transition_t transition;
    if (found->second.bound_session_route
        && same_physical_bound_session (*found->second.bound_session_route, route)) {
        route = merge_bound_session_route_fence (
          *found->second.bound_session_route, std::move (route));
        found->second.bound_session_route = route;
        if (_state->bound_session_sender
            && !found->second.bound_session_stream_sink) {
            _state->bound_session_sinks[actor_id] =
              make_session_owner_sink (
                _state, actor_ref, route);
        }
        transition.current = route;
        return result_t<actor_bound_session_transition_t>::success (std::move (transition));
    }
    transition.current = route;
    transition.previous = found->second.bound_session_route;
    transition.changed = true;
    found->second.bound_session_route = route;
    if (_state->bound_session_sender
        && !found->second.bound_session_stream_sink) {
        _state->bound_session_sinks[actor_id] =
          make_session_owner_sink (
            _state, actor_ref, std::move (route));
    }
    return result_t<actor_bound_session_transition_t>::success (std::move (transition));
}

result_t<void>
actor_gateway_runtime_t::record_session_relay_source (const actor_ref_t &actor_ref,
                                                      zlink::routing_id_t session_rid,
                                                      std::uint64_t binding_generation)
{
    if (binding_generation == 0 || session_rid.to_bytes ().empty ()) {
        return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                        "bound Session relay source fence is invalid");
    }
    const std::lock_guard lock (_state->mutex);
    const auto found = _state->actors_by_id.find (
      std::string (actor_ref.actor_id ().value ()));
    if (found == _state->actors_by_id.end ()
        || found->second.ref.object_generation () != actor_ref.object_generation ()) {
        return result_t<void>::failure (framework_error_kind_t::not_found,
                                        "bound Session relay actor is not current");
    }
    found->second.source_session_rid = std::move (session_rid);
    found->second.source_binding_generation = binding_generation;
    found->second.next_session_relay_sequence = 1;
    return result_t<void>::success ();
}

result_t<void>
actor_gateway_runtime_t::admit_session_relay (const actor_ref_t &actor_ref,
                                              const zlink::routing_id_t &source_node_rid,
                                              const zlink::routing_id_t &session_rid,
                                              std::uint64_t binding_generation,
                                              std::uint64_t session_sequence)
{
    if (binding_generation == 0 || session_sequence == 0) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "bound Session relay sequence fence is invalid");
    }
    const std::lock_guard lock (_state->mutex);
    const auto exact = exact_session_relay_route (
      *_state, actor_ref, source_node_rid, session_rid, binding_generation);
    if (!exact)
        return detail::propagate_failure<void> (
          exact, "bound Session relay admission failed");
    auto &route = *exact.value ();
    if (route.session_sequence_baseline_unknown) {
        //  Relocation-target routes learn the source's relay high-water from
        //  the first sequence admitted on the exact route (spec 20 keeps
        //  command 43/44 free of numeric high-water); afterwards the strict
        //  next check applies.
        route.session_sequence = session_sequence;
        route.session_sequence_baseline_unknown = false;
        return result_t<void>::success ();
    }
    if (route.session_sequence == std::numeric_limits<std::uint64_t>::max ()
        || session_sequence != route.session_sequence + 1) {
        return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                        "bound Session relay sequence is not next");
    }
    route.session_sequence = session_sequence;
    return result_t<void>::success ();
}

result_t<void>
actor_gateway_runtime_t::begin_session_relay_completion (
  const actor_ref_t &actor_ref,
  const zlink::routing_id_t &source_node_rid,
  const zlink::routing_id_t &session_rid,
  std::uint64_t binding_generation,
  std::uint64_t session_sequence)
{
    if (binding_generation == 0 || session_sequence == 0) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "bound Session relay completion fence is invalid");
    }
    const std::lock_guard lock (_state->mutex);
    const auto exact = exact_session_relay_route (
      *_state, actor_ref, source_node_rid, session_rid,
      binding_generation);
    if (!exact) {
        return detail::propagate_failure<void> (
          exact, "bound Session relay completion admission failed");
    }
    const auto &route = *exact.value ();
    if (!route.session_sequence_baseline_unknown
        && session_sequence != route.session_sequence
        && (route.session_sequence
              == std::numeric_limits<std::uint64_t>::max ()
            || session_sequence != route.session_sequence + 1)) {
        return result_t<void>::failure (
          framework_error_kind_t::invalid_operation,
          "bound Session relay completion is not current or next");
    }
    const session_relay_completion_fence_t fence{
      std::string (actor_ref.actor_id ().value ()),
      actor_ref.object_generation (), source_node_rid.to_hex (),
      session_rid.to_hex (), binding_generation, session_sequence};
    if (!_state->active_session_relay_completions.insert (fence).second) {
        return result_t<void>::failure (
          framework_error_kind_t::invalid_operation,
          "bound Session relay completion is already active");
    }
    return result_t<void>::success ();
}

result_t<void>
actor_gateway_runtime_t::complete_session_relay (const actor_ref_t &actor_ref,
                                                 const zlink::routing_id_t &source_node_rid,
                                                 const zlink::routing_id_t &session_rid,
                                                 std::uint64_t binding_generation,
                                                 std::uint64_t session_sequence)
{
    if (binding_generation == 0 || session_sequence == 0) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "bound Session relay completion fence is invalid");
    }
    const std::lock_guard lock (_state->mutex);
    const session_relay_completion_fence_t fence{
      std::string (actor_ref.actor_id ().value ()),
      actor_ref.object_generation (), source_node_rid.to_hex (),
      session_rid.to_hex (), binding_generation, session_sequence};
    const auto active =
      _state->active_session_relay_completions.find (fence);
    if (active == _state->active_session_relay_completions.end ()) {
        return result_t<void>::failure (
          framework_error_kind_t::invalid_operation,
          "bound Session relay completion was not admitted");
    }
    const auto exact = exact_session_relay_route (
      *_state, actor_ref, source_node_rid, session_rid, binding_generation);
    if (!exact) {
        /* The application handler may retire the Actor before the admitted
         * send callback publishes its terminal high-water. The completion
         * fence remains the owner of that terminal and is consumed here
         * without recreating the retired Actor route. */
        _state->active_session_relay_completions.erase (active);
        return result_t<void>::success ();
    }
    auto &route = *exact.value ();
    if (route.session_sequence_baseline_unknown) {
        route.session_sequence = session_sequence;
        route.session_sequence_baseline_unknown = false;
        _state->active_session_relay_completions.erase (active);
        return result_t<void>::success ();
    }
    if (session_sequence == route.session_sequence) {
        _state->active_session_relay_completions.erase (active);
        return result_t<void>::success ();
    }
    if (route.session_sequence == std::numeric_limits<std::uint64_t>::max ()
        || session_sequence != route.session_sequence + 1) {
        _state->active_session_relay_completions.erase (active);
        return result_t<void>::failure (
          framework_error_kind_t::invalid_operation,
          "bound Session relay completion is not next");
    }
    route.session_sequence = session_sequence;
    _state->active_session_relay_completions.erase (active);
    return result_t<void>::success ();
}

result_t<void>
actor_gateway_runtime_t::retire_bound_session_route (const actor_ref_t &actor_ref,
                                                     const zlink::routing_id_t &session_owner_node,
                                                     const zlink::routing_id_t &session_rid,
                                                     std::uint64_t retired_binding_generation)
{
    const auto actor_id = std::string (actor_ref.actor_id ().value ());
    const std::lock_guard lock (_state->mutex);
    const auto found = _state->actors_by_id.find (actor_id);
    if (found == _state->actors_by_id.end ()
        || found->second.ref.object_generation () != actor_ref.object_generation ()
        || !found->second.bound_session_route) {
        return result_t<void>::failure (framework_error_kind_t::not_found,
                                        "bound Session route is not current");
    }
    const auto &route = *found->second.bound_session_route;
    if (route.node_rid.to_hex () != session_owner_node.to_hex () || !route.session_rid
        || route.session_rid->to_hex () != session_rid.to_hex ()
        || route.binding_generation != retired_binding_generation) {
        return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                        "bound Session route fence is stale");
    }
    found->second.bound_session_route.reset ();
    found->second.bound = false;
    found->second.disconnected = true;
    _state->bound_session_sinks.erase (actor_id);
    return result_t<void>::success ();
}

bool actor_gateway_runtime_t::commit_session_relocation_route (
  const runtime::protocol::session_relocation_route_t &route,
  const runtime::stateful::stream_binding_t &previous,
  const runtime::stateful::stream_binding_t &target)
{
    if (route.route.action
          != runtime::protocol::session_relocation_route_action_t::commit
        || previous.connection != target.connection
        || previous.binding_generation != target.binding_generation
        || previous.binding_generation != route.binding_generation
        || previous.actor.kind
             != runtime::stateful::object_kind_t::actor
        || target.actor.kind
             != runtime::stateful::object_kind_t::actor
        || previous.actor.key != route.actor.actor_id
        || target.actor.key != route.actor.actor_id
        || previous.actor.object_generation
             != route.actor.object_generation
        || target.actor.object_generation
             != route.actor.object_generation
        || previous.actor.authority_owner_generation
             != route.route.previous_authority_owner_generation
        || target.actor.authority_owner_generation
             != route.route.target_authority_owner_generation
        || target.actor.node_id
             != zlink::routing_id_t::from (
                  route.route.target_node_routing_id)
                  .to_string ()
        || target.target_node_generation
             != route.route.target_node_generation)
        return false;

    const auto actor_id = route.actor.actor_id;
    const std::lock_guard lock (_state->mutex);
    const auto found = _state->actors_by_id.find (actor_id);
    if (found == _state->actors_by_id.end ()
        || !found->second.bound || found->second.disconnected
        || found->second.ref.object_generation ()
             != route.actor.object_generation
        || found->second.ref.node_rid ().value ()
             != previous.actor.node_id
        || !found->second.bound_session_route)
        return false;
    auto &gateway_route = *found->second.bound_session_route;
    const auto session_rid =
      zlink::routing_id_t::from (route.session_routing_id);
    if (!gateway_route.session_rid
        || gateway_route.session_rid->to_bytes ()
             != session_rid.to_bytes ()
        || gateway_route.node_rid.to_bytes ()
             != route.session_owner_node_routing_id
        || gateway_route.node_generation
             != route.session_owner_node_generation
        || gateway_route.object_generation
             != route.actor.object_generation
        || gateway_route.authority_owner_generation
             != route.route.previous_authority_owner_generation
        || gateway_route.owner_lease_generation
             != previous.owner_lease_generation
        || gateway_route.binding_generation
             != route.binding_generation)
        return false;

    found->second.ref = actor_ref_access_t::make (
      node_rid_t::from_string (target.actor.node_id),
      std::string (actor_ref_access_t::actor_type (found->second.ref)),
      actor_id, route.actor.object_generation);
    gateway_route.authority_owner_generation =
      target.actor.authority_owner_generation;
    gateway_route.owner_lease_generation = target.owner_lease_generation;
    return true;
}

void actor_gateway_runtime_t::unbind_session_stream (std::string actor_id,
                                                     std::string session_id,
                                                     std::uint64_t binding_token)
{
    const std::lock_guard lock (_state->mutex);
    auto found = _state->actors_by_id.find (actor_id);
    if (found != _state->actors_by_id.end ()) {
        if (binding_token != 0
            && (found->second.binding_token != binding_token
                || (!session_id.empty () && found->second.binding_session_id != session_id))) {
            return;
        }
        found->second.bound_session_stream_sink = false;
        found->second.bound_session_route.reset ();
        found->second.binding_session_id.clear ();
        found->second.binding_token = 0;
        found->second.bound = false;
        found->second.disconnected = true;
    }
    _state->bound_session_sinks.erase (actor_id);
}

void actor_gateway_runtime_t::restore_session_stream (
  std::string actor_id,
  const std::string &session_id,
  std::uint64_t binding_token,
  actor_session_binding_snapshot_t snapshot)
{
    const std::lock_guard lock (_state->mutex);
    const auto current = _state->actors_by_id.find (actor_id);
    if (current == _state->actors_by_id.end ()
        || current->second.binding_token != binding_token
        || current->second.binding_session_id != session_id) {
        return;
    }
    if (snapshot.record) {
        current->second = std::move (*snapshot.record);
    } else {
        _state->actors_by_id.erase (current);
    }
    if (snapshot.sink) {
        _state->bound_session_sinks[actor_id] = std::move (snapshot.sink);
    } else {
        _state->bound_session_sinks.erase (actor_id);
    }
}

result_t<void>
actor_gateway_runtime_t::dispatch_bound_session_send (const actor_ref_t &actor_ref,
                                                      std::string packet_name,
                                                      stream_codec_t codec,
                                                      const zlink::message_t &payload) const
{
    std::shared_ptr<detail::bound_session_sink_t> sink;
    {
        const std::lock_guard lock (_state->mutex);
        const auto actor_id = std::string (actor_ref.actor_id ().value ());
        const auto found = _state->actors_by_id.find (actor_id);
        if (found == _state->actors_by_id.end () || !found->second.bound) {
            return result_t<void>::failure (framework_error_kind_t::not_configured,
                                            "actor session is not bound");
        }
        if (!actor_types_compatible (found->second.ref, actor_ref)) {
            return result_t<void>::failure (framework_error_kind_t::type_mismatch,
                                            "actor id is already bound to another type");
        }
        if (found->second.ref.object_generation () != actor_ref.object_generation ()) {
            return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                            "actor generation is stale");
        }
        if (found->second.bound_session_route) {
            auto &route = *found->second.bound_session_route;
            if (route.object_generation != actor_ref.object_generation ()
                || (route.binding_generation != 0 && found->second.source_binding_generation != 0
                    && route.binding_generation != found->second.source_binding_generation)
                || (route.binding_token != 0 && found->second.binding_token != 0
                    && route.binding_token != found->second.binding_token)) {
                return result_t<void>::failure (framework_error_kind_t::not_configured,
                                                "actor bound session route fence is stale");
            }
        }
        const auto found_sink = _state->bound_session_sinks.find (actor_id);
        if (found_sink == _state->bound_session_sinks.end ()) {
            return result_t<void>::failure (framework_error_kind_t::not_configured,
                                            "actor session stream is not bound");
        }
        sink = found_sink->second;
    }
    auto sent = (*sink) (std::move (packet_name), codec, payload).result ();
    if (!sent) {
        return result_t<void>::failure (sent.error_kind (),
                                        sent.error () ? sent.error ()->what ()
                                                      : "actor bound session dispatch failed");
    }
    return result_t<void>::success ();
}

std::optional<actor_gateway_runtime_t::admitted_bound_session_delivery_t>
actor_gateway_runtime_t::admit_bound_session_delivery (
  const actor_ref_t &actor_ref,
  std::uint64_t binding_generation) const
{
    std::shared_ptr<detail::bound_session_sink_t> sink;
    const auto actor_id = std::string (
      actor_ref.actor_id ().value ());
    /* The resolution string exists only for tracing. Build it exclusively
     * while tracing can emit (spec 26 §4: off pays no allocation), and never
     * pay the concatenations under the lock on the silent hot path. */
    const bool trace_resolution =
      detail::message_flow_tracer_t (_state->dispatch)
        .capture_enabled ();
    std::string resolution;
    {
        const std::lock_guard lock (_state->mutex);
        const auto found = _state->actors_by_id.find (actor_id);
        if (found == _state->actors_by_id.end ()) {
            if (trace_resolution)
                resolution = "binding_present=false reason=actor_missing";
        }
        else if (!found->second.bound) {
            if (trace_resolution)
                resolution = "binding_present=false reason=not_bound";
        }
        else if (!actor_types_compatible (
                   found->second.ref, actor_ref)) {
            if (trace_resolution)
                resolution = "binding_present=true reason=type_mismatch";
        }
        else if (found->second.ref.object_generation ()
                   != actor_ref.object_generation ()) {
            if (trace_resolution)
                resolution = "binding_present=true reason=object_generation_mismatch";
        }
        else if (!found->second.bound_session_route) {
            if (trace_resolution)
                resolution = "binding_present=true route_present=false";
        }
        else {
            const auto &route = *found->second.bound_session_route;
            const auto found_sink =
              _state->bound_session_sinks.find (actor_id);
            if (trace_resolution) {
                resolution = "binding_present=true route_present=true session_rid="
                  + (route.session_rid ? route.session_rid->to_hex ()
                                       : std::string ("none"))
                  + " binding_generation="
                  + std::to_string (route.binding_generation)
                  + " expected_binding_generation="
                  + std::to_string (binding_generation)
                  + " sink_present="
                  + (found_sink != _state->bound_session_sinks.end ()
                       ? "true" : "false");
            }
            if (route.object_generation
                  == actor_ref.object_generation ()
                && (binding_generation == 0
                    || route.binding_generation == 0
                    || binding_generation == route.binding_generation)
                && found_sink != _state->bound_session_sinks.end ()) {
                sink = found_sink->second;
                if (trace_resolution)
                    resolution += " match=true";
            }
            else if (trace_resolution) {
                resolution += " match=false";
            }
        }
    }
    if (trace_resolution) {
        trace_detached_bound_session_send_stage (
          _state, actor_id, "session_node_binding_resolve", resolution);
    }
    if (!sink) {
        if (trace_resolution) {
            trace_detached_bound_session_send_failure (
              _state, actor_id, "session_node_binding_resolve " + resolution);
        }
        return std::nullopt;
    }
    return admitted_bound_session_delivery_t{
      [state = _state, actor_id, sink = std::move (sink)] (
        std::string packet_name,
        stream_codec_t codec,
        const zlink::message_t &payload) {
          trace_detached_bound_session_send_stage (
            state, actor_id, "session_node_stream_write_submit", "begin");
          auto sent = (*sink) (
            std::move (packet_name), codec, payload).result ();
          if (!sent) {
              trace_detached_bound_session_send_stage (
                state, actor_id, "session_node_stream_write_terminal",
                "failed error_kind="
                  + std::to_string (
                    static_cast<int> (sent.error_kind ())));
              trace_detached_bound_session_send_failure (
                state, actor_id,
                "session_node_stream_write_terminal failed");
              return result_t<void>::failure (
                sent.error_kind (),
                sent.error () ? sent.error ()->what ()
                              : "actor bound session dispatch failed");
          }
          trace_detached_bound_session_send_stage (
            state, actor_id, "session_node_stream_write_terminal", "ok");
          return result_t<void>::success ();
      }};
}

std::shared_ptr<bound_session_replacement_handler_t>
actor_gateway_runtime_t::register_bound_session_replacement_handler (
  const zlink::routing_id_t &session_rid, bound_session_replacement_handler_t handler)
{
    if (!handler)
        throw std::invalid_argument ("bound Session replacement handler is empty");
    auto registered = std::make_shared<bound_session_replacement_handler_t> (std::move (handler));
    const std::lock_guard lock (_state->mutex);
    _state->bound_session_replacement_handlers[session_rid.to_hex ()]
      .push_back (registered);
    return registered;
}

void actor_gateway_runtime_t::unregister_bound_session_replacement_handler (
  const zlink::routing_id_t &session_rid,
  const std::shared_ptr<bound_session_replacement_handler_t> &handler)
{
    const std::lock_guard lock (_state->mutex);
    const auto found = _state->bound_session_replacement_handlers.find (session_rid.to_hex ());
    if (found != _state->bound_session_replacement_handlers.end ()) {
        std::erase (found->second, handler);
        if (found->second.empty ())
            _state->bound_session_replacement_handlers.erase (found);
    }
}

bool actor_gateway_runtime_t::dispatch_bound_session_replaced (
  const runtime::protocol::bound_session_replaced_t &replacement) const
{
    std::vector<std::shared_ptr<bound_session_replacement_handler_t>> handlers;
    {
        const std::lock_guard lock (_state->mutex);
        const auto session_rid =
          zlink::routing_id_t::from (replacement.retired_session.session_routing_id);
        const auto found = _state->bound_session_replacement_handlers.find (session_rid.to_hex ());
        if (found == _state->bound_session_replacement_handlers.end ())
            return false;
        handlers = found->second;
    }
    for (const auto &handler : handlers) {
        if ((*handler) (replacement))
            return true;
    }
    return false;
}

void actor_gateway_runtime_t::on_join_spot (
  actor_gateway_state_t::join_spot_dispatcher_t dispatcher)
{
    const std::lock_guard lock (_state->mutex);
    _state->join_spot_dispatcher = std::move (dispatcher);
}

void actor_gateway_runtime_t::on_create (actor_gateway_state_t::create_dispatcher_t dispatcher)
{
    const std::lock_guard lock (_state->mutex);
    _state->create_dispatcher = std::move (dispatcher);
}

void actor_gateway_runtime_t::on_join_entry_spot (
  actor_gateway_state_t::join_entry_spot_dispatcher_t dispatcher)
{
    const std::lock_guard lock (_state->mutex);
    _state->join_entry_spot_dispatcher = std::move (dispatcher);
}

void actor_gateway_runtime_t::on_relay (actor_gateway_state_t::relay_dispatcher_t dispatcher)
{
    const std::lock_guard lock (_state->mutex);
    _state->relay_dispatcher = std::move (dispatcher);
}

void actor_gateway_runtime_t::on_disconnect (
  actor_gateway_state_t::disconnect_dispatcher_t dispatcher)
{
    const std::lock_guard lock (_state->mutex);
    _state->disconnect_dispatcher = std::move (dispatcher);
}

void actor_gateway_runtime_t::on_bound_session (
  actor_gateway_state_t::bound_session_registrar_t registrar)
{
    const std::lock_guard lock (_state->mutex);
    _state->bound_session_registrar = std::move (registrar);
}

void actor_gateway_runtime_t::on_bound_session_send (
  actor_gateway_state_t::bound_session_sender_t sender)
{
    const std::lock_guard lock (_state->mutex);
    _state->bound_session_sender = std::move (sender);
}

bool actor_gateway_runtime_t::trace_bound_session_send_stage_enabled () const noexcept
{
    return detail::message_flow_tracer_t (_state->dispatch)
      .enabled (message_flow_log_mode_t::detailed);
}

void actor_gateway_runtime_t::trace_bound_session_send_stage (
  const std::string &actor_id,
  std::string_view stage,
  std::string_view result) const
{
    trace_detached_bound_session_send_stage (_state, actor_id, stage, result);
}

void actor_gateway_runtime_t::on_membership (actor_gateway_state_t::membership_query_t query)
{
    const std::lock_guard lock (_state->mutex);
    _state->membership_query = std::move (query);
}

void actor_gateway_runtime_t::on_join_barrier (
  actor_gateway_state_t::join_barrier_reserver_t reserver)
{
    const std::lock_guard lock (_state->mutex);
    _state->join_barrier_reserver = std::move (reserver);
}

void actor_gateway_runtime_t::bind_serializers (serializer_registry_t &serializers)
{
    const std::lock_guard lock (_state->mutex);
    _state->serializers = &serializers;
}

void actor_gateway_runtime_t::set_dispatch (dispatch_options_t options)
{
    const std::lock_guard lock (_state->mutex);
    _state->dispatch = std::move (options);
}

} // namespace zlink::framework::detail
