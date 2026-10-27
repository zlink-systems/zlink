/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/messaging/failure_origin_wire.hpp"
#include "runtime/streams/stream_header_encoder.hpp"

namespace zlink::framework::detail
{
using detail::stream_header_flags_t;
using detail::stream_header_t;
using detail::stream_message_kind_t;
using detail::stream_relay_dispatch_scope_t;
using detail::submit_once_t;
// Raw baseline from main 3841c24541. The fixture preserves its three owning
// dispatch objects and native relay result while sharing current transport APIs.
class stream_allocation_test_access_t
{
  public:
    static void drain_session_relay (const std::shared_ptr<detail::actor_gateway_state_t> &state,
                                     const std::string &actor_id)
    {
        /* Trampoline: a dispatch that settles synchronously continues the FIFO
     * inside this loop instead of recursing, so a burst of immediate
     * completions cannot grow the stack with the queue length. Only a
     * completion that fires after this frame returned re-enters the drain. */
        for (;;) {
            const auto pending = state->sync (
              [&] () -> std::optional<detail::actor_gateway_state_t::pending_session_relay_t> {
                  const auto found = state->pending_session_relays.find (actor_id);
                  if (found == state->pending_session_relays.end () || found->second.empty ()) {
                      state->pending_session_relays.erase (actor_id);
                      state->active_session_relays.erase (actor_id);
                      return std::nullopt;
                  }
                  auto pending = std::move (found->second.front ());
                  found->second.pop_front ();
                  return pending;
              });
            if (!pending)
                return;

            /* Frame-owned copies: the catch blocks must not read through the
         * pending object after it moved into the completion observer. */
            const auto completion = pending->completion;
            const auto packet_name = pending->packet_name;
            try {
                /* The running coroutine frame references the closure inside
             * pending.dispatch; the observer below keeps `pending` (and that
             * closure's heap storage) alive until completion — do not shrink
             * the dispatch closure below SBO size. */
                auto dispatch =
                  std::make_shared<std::function<task_t<void> ()>> (std::move (pending->dispatch));
                auto dispatched = std::make_shared<task_t<void>> ((*dispatch) ());
                /* Whoever loses the exchange race hands the next turn to the
             * winner: a synchronous completion lets this loop continue, an
             * asynchronous one re-enters the drain from its own frame. */
                auto continue_gate = std::make_shared<std::atomic<bool>> (false);
                detail::observe_task_completion (
                  *dispatched, [state, actor_id, pending = std::move (*pending), dispatch,
                                dispatched, continue_gate] (const result_t<void> &result) mutable {
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
                const framework_exception_t failure (framework_error_kind_t::internal_failure,
                                                     error.what ());
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
                completion->complete (
                  result_t<void>::failure (framework_error_kind_t::internal_failure,
                                           "actor session relay threw a non-standard exception"));
            }
        }
    }

    static task_t<void>
    enqueue_session_relay (const std::shared_ptr<detail::actor_gateway_state_t> &state,
                           std::string actor_id,
                           std::string packet_name,
                           std::function<task_t<void> ()> dispatch)
    {
        auto completion = std::make_shared<task_completion_source_t<void>> ();
        auto task = completion->task ();
        const auto start_drain = state->sync ([&] {
            auto &queue = state->pending_session_relays[actor_id];
            queue.push_back ({std::move (dispatch), completion, std::move (packet_name)});
            return state->active_session_relays.insert (actor_id).second;
        });
        if (start_drain)
            drain_session_relay (state, actor_id);
        return task;
    }

    static request_task_t<zlink::message_t> complete_session_actor_relay_request (
      std::shared_ptr<detail::actor_gateway_state_t> state,
      detail::actor_gateway_state_t::relay_dispatcher_t submit,
      actor_ref_t actor,
      actor_context_t actor_context,
      detail::stream_header_t relay_header,
      zlink::message_t payload,
      std::optional<detail::bound_session_relay_source_t> relay_source,
      std::chrono::milliseconds timeout,
      std::shared_ptr<task_completion_source_t<void>> admitted = {})
    {
        if (admitted) {
            auto source = co_await detail::request_admission_source_t{};
            auto stage = source->task ();
            detail::observe_task_terminal (
              stage, [admitted] (const auto &result) { admitted->complete (result); });
        }
        // This dispatcher submits this relay operation. Its explicit connection
        // excludes any infrastructure request awaited inside the submission.
        auto dispatcher = [submit = std::move (submit)] (auto &&...args) mutable {
            return detail::request_submission (submit (std::forward<decltype (args)> (args)...));
        };
        const auto dispatched = co_await dispatcher (actor, std::move (actor_context), relay_header,
                                                     payload, std::move (relay_source), timeout);
        if (!dispatched) {
            const framework_exception_t missing_reply (framework_error_kind_t::protocol_error,
                                                       "actor relay request has no reply");
            detail::dispatch_error_reporter_t (state->dispatch).report_lazy ([&] {
                return message_dispatch_error_event_t{dispatch_error_surface_t::spot_actor,
                                                      dispatch_message_kind_t::actor_request,
                                                      dispatch_error_reason_t::reply_path_missing,
                                                      dispatch_error_action_t::fail_caller,
                                                      {},
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::string (actor.actor_id ().value ()),
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::make_exception_ptr (missing_reply)};
            });
            throw missing_reply;
        }
        co_return std::move (*dispatched);
    }

    class actor_t : public session_actor_t
    {
      public:
        explicit actor_t (session_actor_t actor) : session_actor_t (std::move (actor)) {}

        task_t<void> relay_internal (detail::stream_header_t header,
                                     std::uint64_t relay_sequence,
                                     const zlink::message_t &payload)
        {
            detail::actor_gateway_state_t::relay_dispatcher_t dispatcher;
            detail::stream_header_t relay_header;
            std::optional<detail::bound_session_relay_source_t> relay_source;
            std::optional<result_t<void>> rejected;
            _state->sync ([&] {
                if (::zlink::framework::detail::actor_ref_access_t::empty (_ref)) {
                    rejected = result_t<void>::failure (framework_error_kind_t::not_found,
                                                        "session actor is not bound");
                    return;
                }
                const auto found =
                  _state->actors_by_id.find (std::string (_ref.actor_id ().value ()));
                if (found != _state->actors_by_id.end () && found->second.disconnected) {
                    rejected = detail::boundary_failure<void> (
                      detail::boundary_error_t::disconnected, "actor session is disconnected");
                    return;
                }
                if (found == _state->actors_by_id.end ()) {
                    rejected = result_t<void>::failure (framework_error_kind_t::not_found,
                                                        "actor route is not found");
                    return;
                }
                if (!found->second.bound) {
                    rejected = result_t<void>::failure (framework_error_kind_t::not_configured,
                                                        "actor session is not bound");
                    return;
                }
                if (_binding_token != 0 && found->second.binding_token != _binding_token) {
                    rejected = result_t<void>::failure (framework_error_kind_t::not_configured,
                                                        "actor session binding is stale");
                    return;
                }
                if (found->second.ref.object_generation () != _ref.object_generation ()) {
                    rejected = result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                                        "actor generation is stale");
                    return;
                }
                _ref = found->second.ref;
                if (!_state->relay_dispatcher) {
                    _state->relayed_frames.push_back (
                      detail::relayed_frame_t{_ref, header, payload});
                    rejected = result_t<void>::success ();
                    return;
                }
                dispatcher = _state->relay_dispatcher;
                if (found->second.source_session_rid
                    && found->second.source_binding_generation != 0) {
                    relay_source = detail::bound_session_relay_source_t{
                      *found->second.source_session_rid, found->second.source_binding_generation,
                      relay_sequence};
                }
                auto metadata = header.metadata ().values ();
                metadata.insert_or_assign (std::string (detail::bound_session_relay_binding_key),
                                           std::to_string (found->second.binding_token));
                metadata.insert_or_assign (std::string (detail::bound_session_relay_sequence_key),
                                           std::to_string (relay_sequence));
                relay_header = detail::stream_header_t (
                  header.kind (), header.codec (), header.flags (), header.request_seq (),
                  std::string (header.packet_name ()),
                  detail::stream_metadata_t (std::move (metadata)));
                if (const auto correlation = header.correlation_id ())
                    relay_header.with_correlation_id (std::string (*correlation));
                /* flow-correlation §5: an intermediate runtime forwards the flow pair
         * only while tracing is on; at Off the inbound pair is not copied. */
                if (detail::message_flow_tracer_t (_state->dispatch).flow_tracing ()) {
                    if (const auto flow_id = header.flow_id ())
                        relay_header.with_flow (std::string (*flow_id), *header.flow_origin ());
                }
            });
            if (rejected)
                return task_t<void> (std::move (*rejected));

            const auto actor_id = std::string (_ref.actor_id ().value ());
            const auto packet_name = std::string (relay_header.packet_name ());
            auto actor_context = std::make_shared<actor_context_t> (context ());
            return enqueue_session_relay (
              _state, actor_id, packet_name,
              [state = _state, dispatcher = std::move (dispatcher), actor = _ref,
               actor_context = std::move (actor_context), relay_header = std::move (relay_header),
               payload, relay_source = std::move (relay_source)] () mutable -> task_t<void> {
                  bool offload_session_relay = false;
                  offload_session_relay =
                    state->sync ([&] { return state->offload_session_relay; });
                  if (!offload_session_relay) {
                      const auto dispatched = co_await dispatcher (
                        actor, std::move (*actor_context), relay_header, payload,
                        std::move (relay_source), std::chrono::milliseconds::zero ());
                      (void) dispatched;
                      co_return;
                  }
                  /* task_t coroutines start eagerly. Submit the relay before
           * constructing that coroutine so a STREAM callback cannot enter an
           * Actor delivery turn inline; the Actor runtime then selects its
           * PerActor or SpotWide execution gate as usual. */
                  auto offloaded = runtime::handler_coroutine_executor ().submit<void> (
                    [dispatcher = std::move (dispatcher), actor = std::move (actor),
                     actor_context = std::move (actor_context),
                     relay_header = std::move (relay_header), payload,
                     relay_source = std::move (
                       relay_source)] () mutable -> boost::asio::awaitable<result_t<void>> {
                        const auto dispatched = co_await runtime::await_task_result (dispatcher (
                          actor, std::move (*actor_context), relay_header, payload,
                          std::move (relay_source), std::chrono::milliseconds::zero ()));
                        if (!dispatched) {
                            co_return result_t<void>::failure (dispatched.error_kind (),
                                                               dispatched.error () != nullptr
                                                                 ? dispatched.error ()->what ()
                                                                 : "actor session relay failed");
                        }
                        co_return result_t<void>::success ();
                    });
                  co_return co_await offloaded;
              });
        }

        task_t<void> relay (const zlink::message_t &payload)
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

        request_call_t<zlink::message_t> relay_request (const zlink::message_t &payload)
        {
            const auto header = detail::current_stream_relay_dispatch ();
            if (!header) {
                return request_call_t<zlink::message_t> (result_t<zlink::message_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "actor relay request requires current stream dispatch state"));
            }
            return request_call_t<zlink::message_t> (
              std::string (header->packet_name ()),
              [actor = *this, header = *header,
               payload] (const std::string &, std::chrono::milliseconds timeout,
                         const request_call_t<zlink::message_t>::metadata_map_t &) mutable {
                  const auto relay_sequence = actor.reserve_relay_sequence ();
                  if (!relay_sequence) {
                      return request_task_t<zlink::message_t> (
                        detail::propagate_failure<zlink::message_t> (
                          relay_sequence, "actor relay request sequence failed"));
                  }
                  return actor.relay_request_internal (header, relay_sequence.value (), payload,
                                                       timeout);
              });
        }

        request_task_t<zlink::message_t> relay_request_internal (detail::stream_header_t header,
                                                                 std::uint64_t relay_sequence,
                                                                 zlink::message_t payload,
                                                                 std::chrono::milliseconds timeout)
        {
            detail::actor_gateway_state_t::relay_dispatcher_t dispatcher;
            detail::stream_header_t relay_header;
            std::optional<detail::bound_session_relay_source_t> relay_source;
            bool offload_session_relay = false;
            std::optional<result_t<zlink::message_t>> rejected;
            _state->sync ([&] {
                if (::zlink::framework::detail::actor_ref_access_t::empty (_ref)) {
                    rejected = result_t<zlink::message_t>::failure (
                      framework_error_kind_t::not_found, "session actor is not bound");
                    return;
                }
                const auto found =
                  _state->actors_by_id.find (std::string (_ref.actor_id ().value ()));
                if (found != _state->actors_by_id.end () && found->second.disconnected) {
                    rejected = detail::boundary_failure<zlink::message_t> (
                      detail::boundary_error_t::disconnected, "actor session is disconnected");
                    return;
                }
                if (found == _state->actors_by_id.end ()) {
                    rejected = result_t<zlink::message_t>::failure (
                      framework_error_kind_t::not_found, "actor route is not found");
                    return;
                }
                if (!found->second.bound) {
                    rejected = result_t<zlink::message_t>::failure (
                      framework_error_kind_t::not_configured, "actor session is not bound");
                    return;
                }
                if (_binding_token != 0 && found->second.binding_token != _binding_token) {
                    rejected = result_t<zlink::message_t>::failure (
                      framework_error_kind_t::not_configured, "actor session binding is stale");
                    return;
                }
                if (found->second.ref.object_generation () != _ref.object_generation ()) {
                    rejected = result_t<zlink::message_t>::failure (
                      framework_error_kind_t::invalid_operation, "actor generation is stale");
                    return;
                }
                _ref = found->second.ref;
                if (!_state->relay_dispatcher) {
                    _state->relayed_frames.push_back (
                      detail::relayed_frame_t{_ref, header, payload});
                    rejected = result_t<zlink::message_t>::failure (
                      framework_error_kind_t::not_found,
                      "actor relay dispatcher is not configured");
                    return;
                }
                dispatcher = _state->relay_dispatcher;
                offload_session_relay = _state->offload_session_relay;
                if (found->second.source_session_rid
                    && found->second.source_binding_generation != 0) {
                    relay_source = detail::bound_session_relay_source_t{
                      *found->second.source_session_rid, found->second.source_binding_generation,
                      relay_sequence};
                }
                auto metadata = header.metadata ().values ();
                metadata.insert_or_assign (std::string (detail::bound_session_relay_binding_key),
                                           std::to_string (found->second.binding_token));
                metadata.insert_or_assign (std::string (detail::bound_session_relay_sequence_key),
                                           std::to_string (relay_sequence));
                relay_header = detail::stream_header_t (
                  header.kind (), header.codec (), header.flags (), header.request_seq (),
                  std::string (header.packet_name ()),
                  detail::stream_metadata_t (std::move (metadata)));
                if (const auto correlation = header.correlation_id ())
                    relay_header.with_correlation_id (std::string (*correlation));
                /* flow-correlation §5: an intermediate runtime forwards the flow pair
         * only while tracing is on; at Off the inbound pair is not copied. */
                if (detail::message_flow_tracer_t (_state->dispatch).flow_tracing ()) {
                    if (const auto flow_id = header.flow_id ())
                        relay_header.with_flow (std::string (*flow_id), *header.flow_origin ());
                }
            });
            if (rejected)
                return request_task_t<zlink::message_t> (std::move (*rejected));
            auto admitted = std::make_shared<task_completion_source_t<void>> ();
            if (!offload_session_relay) {
                return request_task_t<zlink::message_t> (
                  complete_session_actor_relay_request (
                    _state, std::move (dispatcher), _ref, context (), std::move (relay_header),
                    payload, std::move (relay_source), timeout, admitted),
                  admitted);
            }
            auto actor_context = std::make_shared<actor_context_t> (context ());
            return request_task_t<zlink::message_t> (
              runtime::handler_coroutine_executor ().submit<zlink::message_t> (
                [state = _state, dispatcher = std::move (dispatcher), actor = _ref,
                 actor_context = std::move (actor_context), relay_header = std::move (relay_header),
                 payload, relay_source = std::move (relay_source), timeout,
                 admitted] () mutable -> boost::asio::awaitable<result_t<zlink::message_t>> {
                    co_return co_await runtime::await_task_result (
                      complete_session_actor_relay_request (
                        std::move (state), std::move (dispatcher), std::move (actor),
                        std::move (*actor_context), std::move (relay_header), payload,
                        std::move (relay_source), timeout, admitted));
                }),
              admitted);
        }
    };
    class stream_t : public zlink::framework::stream_t
    {
      public:
        explicit stream_t (zlink::framework::stream_t stream) :
            zlink::framework::stream_t (std::move (stream))
        {
        }

        stream_write_call_t reply_packet (const zlink::message_t &payload)
        {
            const auto request_header = _reply_header;
            if (!request_header) {
                return stream_write_call_t (
                  result_t<void>::failure (framework_error_kind_t::protocol_error,
                                           "STREAM reply requires current dispatch state"));
            }
            if (!request_header->request_seq ()) {
                return stream_write_call_t (
                  result_t<void>::failure (framework_error_kind_t::protocol_error,
                                           "STREAM reply requires request sequence"));
            }
            auto reply_header = detail::stream_runtime_t::make_terminal_header (
              *this, stream_message_kind_t::response, *request_header);
            auto call = write_packet_with_header (std::move (reply_header), payload);
            if (!_reply_submission->try_claim ())
                throw std::runtime_error ("raw baseline reply already consumed");
            return call;
        }
    };
    class session_t
    {
      public:
        virtual ~session_t () = default;
        virtual task_t<void> on_connected (zlink::framework::stream_t &) = 0;
        virtual task_t<void> on_disconnected (zlink::framework::stream_t &) = 0;
        virtual task_t<void> on_error (zlink::framework::stream_t &, const stream_error_t &) = 0;
        virtual task_t<void>
        on_packet (stream_t &, const session_message_context_t &, const zlink::message_t &) = 0;
    };

    static message_flow_event_t stream_packet_flow_event (message_flow_outcome_t outcome,
                                                          const stream_header_t &header)
    {
        return message_flow_event_t{
          .outcome = outcome,
          .surface = dispatch_error_surface_t::stream_session,
          .message_kind = header.kind () == stream_message_kind_t::request
                            ? dispatch_message_kind_t::request
                            : dispatch_message_kind_t::send,
          .packet_name = std::string (header.packet_name ()),
          .correlation_id = header.correlation_id ()
                              ? std::make_optional (std::string (*header.correlation_id ()))
                              : std::nullopt};
    }

    static task_t<void>
    dispatch_packet_session_callback (session_t *session,
                                      stream_t stream,
                                      std::shared_ptr<stream_header_t> header,
                                      std::shared_ptr<session_message_context_t> context,
                                      std::shared_ptr<zlink::message_t> payload)
    {
        auto task = [&] {
            const stream_relay_dispatch_scope_t relay_scope (*header);
            return session->on_packet (stream, *context, *payload);
        }();
        co_return co_await task;
    }

    static task_t<void> dispatch_packet_session (session_t *session,
                                                 stream_t stream,
                                                 std::shared_ptr<stream_header_t> header,
                                                 std::shared_ptr<session_message_context_t> context,
                                                 std::shared_ptr<zlink::message_t> payload,
                                                 session_actor_manager_t *actors,
                                                 detail::message_flow_tracer_t flow_tracer)
    {
        if (auto actor_slot = header->actor_slot ()) {
            auto actor =
              actors ? detail::session_actor_manager_access_t::find_slot (*actors, *actor_slot)
                     : std::nullopt;
            if (!actor) {
                if (header->kind () == stream_message_kind_t::send) {
                    flow_tracer.trace (message_flow_outcome_t::dropped, [&] {
                        auto event =
                          stream_packet_flow_event (message_flow_outcome_t::dropped, *header);
                        event.reason = message_flow_reason_t::stale_target;
                        return event;
                    });
                    return task_t<void> (result_t<void>::success ());
                }
                return task_t<void> (detail::result_access_t::failure<void> (
                  detail::make_origin_exception (framework_error_kind_t::invalid_operation,
                                                 detail::failure_origin_t::stale_actor_slot,
                                                 "STREAM Actor slot is not a current binding")));
            }
            context->actor = std::make_shared<session_actor_t> (std::move (*actor));
        }
        flow_tracer.trace (message_flow_outcome_t::dispatched, [&] {
            return stream_packet_flow_event (message_flow_outcome_t::dispatched, *header);
        });
        return dispatch_packet_session_callback (session, std::move (stream), std::move (header),
                                                 std::move (context), std::move (payload));
    }

    class runtime_t : public detail::stream_runtime_t
    {
      public:
        explicit runtime_t (detail::stream_runtime_t runtime) :
            detail::stream_runtime_t (std::move (runtime))
        {
        }

        result_t<void> dispatch_packet_async (session_t &session,
                                              stream_t &stream,
                                              const stream_header_t &header,
                                              const zlink::message_t &payload,
                                              async_dispatch_completion_t completion = {},
                                              async_dispatch_started_t started = {},
                                              async_dispatch_cancel_t cancelled = {}) const
        {
            if (auto valid = validate_header (header); !valid) {
                return valid;
            }
            auto handler_payload = payload;
            if ((static_cast<unsigned> (header.flags ())
                 & static_cast<unsigned> (stream_header_flags_t::payload_compressed))
                != 0)
                throw std::runtime_error (
                  "raw allocation baseline requires an uncompressed packet");
            stream._state->application_codec.store (header.codec (), std::memory_order_release);
            const detail::message_flow_tracer_t flow_tracer (_state->dispatch);
            const auto flow_mode = flow_tracer.mode ();
            std::optional<std::string> inbound_flow_id;
            if (runtime::flow_context_t::tracing (flow_mode)) {
                if (auto id = header.flow_id ())
                    inbound_flow_id = std::string (*id);
            }
            auto flow_scope = runtime::flow_context_t::enter (
              std::move (inbound_flow_id), header.flow_origin (), flow_mode, flow_origin_t::inbound,
              !runtime::flow_context_t::tracing (flow_mode) || !stream.routing_id ()
                ? std::nullopt
                : std::optional<std::string> (stream.routing_id ()->to_hex ()));
            flow_tracer.trace (message_flow_outcome_t::received, [&] {
                return stream_packet_flow_event (message_flow_outcome_t::received, header);
            });
            auto current_flow = runtime::flow_context_t::current ();
            auto dispatch_stream = stream;
            dispatch_stream._reply_header = header;
            dispatch_stream._reply_submission = std::make_shared<submit_once_t> ();
            auto dispatch_header = std::make_shared<stream_header_t> (header);
            auto dispatch_context = std::make_shared<session_message_context_t> ();
            dispatch_context->packet_name = std::string (header.packet_name ());
            dispatch_context->metadata = message_metadata_t (header.metadata ().values ());
            dispatch_context->can_reply = header.request_seq ().has_value ();
            auto dispatch_payload =
              std::make_shared<zlink::message_t> (std::move (handler_payload));
            return dispatch_application_async (
              stream, "packet:" + std::string (header.packet_name ()),
              [session = &session, dispatch_stream = std::move (dispatch_stream),
               dispatch_header = std::move (dispatch_header),
               dispatch_context = std::move (dispatch_context),
               dispatch_payload = std::move (dispatch_payload),
               current_flow = std::move (current_flow), flow_tracer] () mutable {
                  runtime::flow_context_t::scope_t callback_flow (std::move (current_flow));
                  auto *actors = dispatch_stream._state->actors.load (std::memory_order_acquire);
                  return dispatch_packet_session (
                    session, std::move (dispatch_stream), std::move (dispatch_header),
                    std::move (dispatch_context), std::move (dispatch_payload), actors,
                    flow_tracer);
              },
              std::move (completion), std::move (started), std::move (cancelled));
        }
    };
};
}
