/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/streams/stream_host_service_internal.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/diagnostics/session_binding_diagnostics.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/timers/async_delay.hpp"
#include <boost/asio/post.hpp>
#include <exception>
#include <tuple>

namespace zlink::framework::runtime
{

using stream_host_detail::session_creation_failed_diagnostic;

namespace
{

constexpr auto session_binding_retirement_timeout = std::chrono::seconds (5);

} // namespace

stream_host_service_t::listener_t::replacement_session_state_t::replacement_session_state_t (
  zlink::routing_id_t session_rid_,
  stream_t stream_,
  std::function<void ()> close_connection_,
  detail::stream_runtime_t::actor_binding_replaced_dispatch_t dispatch_replacement_,
  detail::actor_gateway_runtime_t actor_gateway_,
  std::shared_ptr<runtime_failure_collector_t> failures_) :
    session_rid (std::move (session_rid_)),
    stream (std::move (stream_)),
    close_connection (std::move (close_connection_)),
    dispatch_replacement (std::move (dispatch_replacement_)),
    actor_gateway (std::move (actor_gateway_)),
    failures (std::move (failures_))
{
}

stream_host_service_t::listener_t::replacement_session_state_t::~replacement_session_state_t ()
{
    deactivate ();
}

void stream_host_service_t::listener_t::replacement_session_state_t::deactivate () noexcept
{
    std::shared_ptr<detail::bound_session_replacement_handler_t> registered;
    {
        const std::lock_guard lock (gate);
        accepting_replacements = false;
        registered = std::move (handler);
    }
    if (!registered)
        return;
    try {
        actor_gateway.unregister_bound_session_replacement_handler (session_rid, registered);
    }
    catch (...) {
        failures->report (std::current_exception ());
    }
}

task_t<void> stream_host_service_t::listener_t::replacement_session_state_t::dispatch_if_active (
  stream_t &dispatch_stream, std::string actor_id)
{
    detail::stream_runtime_t::actor_binding_replaced_dispatch_t dispatch;
    {
        const std::lock_guard lock (gate);
        if (!accepting_replacements || !dispatch_replacement) {
            return task_t<void> (result_t<void>::success ());
        }
        dispatch = dispatch_replacement;
    }
    return dispatch (dispatch_stream, std::move (actor_id));
}

bool stream_host_service_t::listener_t::replacement_session_state_t::exact (
  const runtime::protocol::bound_session_replaced_t &replacement)
{
    const auto &retired = replacement.retired_session;
    const auto found = actor_bindings.find (replacement.actor_authority.actor_id);
    return found != actor_bindings.end ()
           && found->second.object_generation == replacement.actor_authority.object_generation
           && found->second.binding_generation == retired.retired_binding_generation;
}

stream_host_service_t::listener_t::stream_connection_state_t::stream_connection_state_t (
  detail::service_scope_t scope,
  packet_stream_session_t *session,
  stream_t stream,
  session_actor_manager_t *actors,
  std::optional<runtime::stateful::stream_connection_t> transport_connection) :
    scope (std::move (scope)),
    session (session),
    stream (std::move (stream)),
    actors (actors),
    transport_connection (std::move (transport_connection))
{
}

stream_host_service_t::listener_t::stream_connection_state_t::~stream_connection_state_t ()
{
    if (replacement) {
        replacement->deactivate ();
    }
    if (actors != nullptr) {
        detail::session_actor_manager_access_t::disconnect (*actors);
    }
    scope.close ();
}

void stream_host_service_t::listener_t::begin_connection_retirement (
  const std::shared_ptr<stream_connection_state_t> &state)
{
    if (state->replacement)
        state->replacement->deactivate ();
    auto &stream = state->stream;
    _runtime.mark_disconnected (stream);
    // Publish retirement before removing the active entry so the Host's
    // drain observes the session until its final callback settles.
    track_retiring_session (stream.session_id (), state, stream);
    unregister_active_stream (stream);
}

// The connection reader owns retirement of the native registry entry.
void stream_host_service_t::listener_t::retire_connection (
  const std::shared_ptr<stream_connection_state_t> &state,
  std::function<void (std::function<void ()>)> dispatch_final)
{
    auto completed = [self = shared_from_this (), retirement_id = state->stream.session_id (),
                      state] { self->forget_retiring_session (retirement_id, state.get ()); };
    if (!state->transport_connection) {
        dispatch_final (std::move (completed));
        return;
    }
    const auto session_rid = state->stream.routing_id ().value_or (
      zlink::routing_id_t::from (state->stream.session_id ()));
    retire_actor_session_bindings_async (
      *state->transport_connection, session_rid, *state->actors,
      [completed = std::move (completed), dispatch_final = std::move (dispatch_final)] () mutable {
          dispatch_final (std::move (completed));
      });
}

/* zlink.stream.connections.* (runtime-metrics §4.1): session accept and
 * close on the server edge; reconnect attempts belong to the connector. */
void stream_host_service_t::listener_t::register_active_stream (
  const stream_t &stream,
  std::function<void (stream_close_reason_t, std::string)> request_terminate)
{
    _active_streams_lane
      .run ([this, stream, request_terminate = std::move (request_terminate)] () mutable {
          _active_streams.push_back ({stream, std::move (request_terminate)});
      })
      .get ();
}

void stream_host_service_t::listener_t::unregister_active_stream (const stream_t &stream)
{
    _active_streams_lane
      .run ([this, &stream] {
          for (auto it = _active_streams.begin (); it != _active_streams.end (); ++it) {
              if (it->stream.session_id () == stream.session_id ()) {
                  _active_streams.erase (it);
                  break;
              }
          }
      })
      .get ();
}

std::size_t stream_host_service_t::listener_t::active_session_count ()
{
    const auto active =
      _active_streams_lane.run ([this] { return _active_streams.size (); }).get ();
    const std::lock_guard lock (_core_sessions_mutex);
    return active + _retiring_sessions.size ();
}

void stream_host_service_t::listener_t::begin_drain_sessions ()
{
    // The host has sealed new admission. An ingress that accepted a
    // session before the seal must publish it before the drain sweep.
    const std::lock_guard lock (_core_sessions_mutex);
    force_close_sessions (stream_close_reason_t::server_drain, "server drain");
}

void stream_host_service_t::listener_t::notify_sessions_closing (stream_close_reason_t reason,
                                                                 std::string_view diagnostic)
{
    auto sessions = _active_streams_lane.run ([this] { return _active_streams; }).get ();
    for (auto &entry : sessions) {
        _runtime.send_session_closing (entry.stream, reason, diagnostic);
    }
}

/* Forced teardown (graceful-drain-handoff §7): the notified sessions are
 * closed so the reason the client keeps is the forced one — a lingering
 * connection would later be re-labeled by the liveness loop. */
void stream_host_service_t::listener_t::force_close_sessions (stream_close_reason_t reason,
                                                              std::string_view diagnostic)
{
    _active_streams_lane
      .run ([this, reason, diagnostic = std::string (diagnostic)] {
#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
          if (_shutdown_test_hooks.before_terminate)
              _shutdown_test_hooks.before_terminate (state_lane_t::current ()
                                                     == &_active_streams_lane);
#endif
          for (auto &entry : _active_streams)
              entry.request_terminate (reason, diagnostic);
      })
      .get ();
}

const char *
stream_host_service_t::listener_t::liveness_close_label (stream_close_reason_t reason) noexcept
{
    switch (reason) {
        case stream_close_reason_t::idle_timeout:
            return "idle_timeout";
        case stream_close_reason_t::heartbeat_timeout:
            return "heartbeat_timeout";
        case stream_close_reason_t::server_drain:
            return "server_shutdown";
        case stream_close_reason_t::protocol_error:
            return "protocol_error";
        default:
            return "transport_error";
    }
}

void stream_host_service_t::listener_t::record_connection_opened () const
{
    if (_opened_metric)
        _opened_metric->Add (1, {{"transport", _metric_transport}});
    if (_active_metric)
        _active_metric->Add (1, {{"transport", _metric_transport}});
}

void stream_host_service_t::listener_t::record_connection_closed (const char *close_reason) const
{
    if (_closed_metric)
        _closed_metric->Add (1, {{"transport", _metric_transport}, {"close_reason", close_reason}});
    if (_active_metric)
        _active_metric->Add (-1, {{"transport", _metric_transport}});
}

bool stream_host_service_t::listener_t::draining () const noexcept
{
    return _host._admission.load (std::memory_order_acquire) == admission_t::sealed
           || (_drain_flag && _drain_flag->load (std::memory_order_acquire));
}

/* flow-correlation §4.3: session dispatch failures surface as error
 * events carrying the inbound flow pair, so one grep catches the
 * healthy and failing lines of the same flow. */
template <typename FailureFactory>
void stream_host_service_t::listener_t::report_packet_failure (
  const stream_header_t &header, const FailureFactory &failure_factory) const
{
    detail::dispatch_error_reporter_t (_runtime.dispatch_options_ref ()).report_lazy ([&] {
        auto [reason, exception] = failure_factory ();
        message_dispatch_error_event_t event{
          dispatch_error_surface_t::stream_session,
          header.kind () == stream_message_kind_t::request ? dispatch_message_kind_t::request
                                                           : dispatch_message_kind_t::send,
          reason,
          header.kind () == stream_message_kind_t::request ? dispatch_error_action_t::reply_error
                                                           : dispatch_error_action_t::drop,
          std::string (header.packet_name ()),
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          header.correlation_id () ? std::make_optional (std::string (*header.correlation_id ()))
                                   : std::nullopt,
          std::move (exception)};
        if (auto flow = header.flow_id ()) {
            event.flow_id = std::string (*flow);
            event.flow_origin = header.flow_origin ();
        }
        return event;
    });
}

void stream_host_service_t::listener_t::report_packet_dispatch_error (
  const stream_header_t &header, const result_t<void> &dispatched) const
{
    report_packet_failure (header, [&] {
        return std::pair{detail::dispatch_reason_from_error (dispatched.error ()),
                         dispatched.error () ? std::make_exception_ptr (*dispatched.error ())
                                             : std::exception_ptr{}};
    });
}

packet_stream_session_t *
stream_host_service_t::listener_t::create_session (service_provider_t &services,
                                                   const stream_header_t *first_packet)
{
    try {
        return &_session_factory (services);
    }
    catch (...) {
        auto exception = std::current_exception ();
        if (_monitoring && _monitoring->diagnostics_logger.is_enabled (log_level_t::error)) {
            auto summary = detail::diagnostic_event_sink_t::exception_summary (exception);
            detail::diagnostic_event_sink_t::log_if_configured (
              _monitoring->diagnostics_logger, log_level_t::error,
              session_creation_failed_diagnostic,
              {{"stream_node", _stream.name},
               {"session_type", _stream.packet_session_name},
               {"error_type", std::move (summary.type)},
               {"error_message", std::move (summary.message)}});
        }
        if (first_packet) {
            const auto mode = detail::dispatch_options_access_t::effective_message_flow (
              _runtime.dispatch_options_ref ());
            auto flow_scope = flow_context_t::enter (
              flow_context_t::tracing (mode) && first_packet->flow_id ()
                ? std::optional<std::string> (*first_packet->flow_id ())
                : std::nullopt,
              first_packet->flow_origin (), mode, flow_origin_t::inbound, std::nullopt);
            report_packet_failure (*first_packet, [&] {
                return std::pair{dispatch_error_reason_t::session_creation_failed, exception};
            });
        }
        return nullptr;
    }
}

bool stream_host_service_t::listener_t::begin_actor_binding_replacement (
  const std::shared_ptr<replacement_session_state_t> &state,
  const runtime::protocol::bound_session_replaced_t &replacement)
{
    std::unique_lock admission_lock (state->gate);
    if (!state->accepting_replacements || state->closing.load (std::memory_order_acquire)) {
        return false;
    }
    const auto local = _mesh_node->native_node ().status ();
    const auto owner = _mesh_node->session_route_owner ();
    const auto &retired = replacement.retired_session;
    if (!owner) {
        detail::trace_session_binding_control_drop (
          _runtime.dispatch_options_ref (), replacement.actor_authority.target_node_routing_id,
          replacement.actor_authority.actor_id, "boundSessionReplaced",
          message_flow_reason_t::location_unavailable);
        return false;
    }
    // Session-Actor binding §4: compare with this host's current descriptor identity.
    if (owner->lease_generation <= 0
        || !runtime::protocol::same_session_owner_identity (
          retired, {local.routing_id ().to_bytes (),
                    local.lifecycle_generation (),
                    owner->owner_id,
                    static_cast<std::uint64_t> (owner->lease_generation),
                    {},
                    0})
        || retired.session_routing_id != state->session_rid.to_bytes ()) {
        trace_stream_host ("actor-binding-replacement-rejected", _stream, std::nullopt,
                           "reason=session-fence-mismatch");
        detail::trace_session_binding_control_drop (
          _runtime.dispatch_options_ref (), replacement.actor_authority.target_node_routing_id,
          replacement.actor_authority.actor_id, "boundSessionReplaced",
          owner->lease_generation <= 0 ? message_flow_reason_t::location_unavailable
                                       : message_flow_reason_t::stale_target);
        return false;
    }
    if (!state->exact (replacement)) {
        trace_stream_host ("actor-binding-replacement-rejected", _stream, std::nullopt,
                           "reason=binding-fence-mismatch");
        return false;
    }
    state->closing.store (true, std::memory_order_release);
    trace_stream_host ("actor-binding-replacement-admitted", _stream, std::nullopt,
                       "actor=" + replacement.actor_authority.actor_id);

    const auto replacement_copy = replacement;
    const auto weak = std::weak_ptr<replacement_session_state_t> (state);
    auto deadline = std::make_shared<asio::steady_timer> (asio::system_executor{});
    deadline->expires_after (_session_replacement_callback_timeout);
    deadline->async_wait (
      [weak, replacement_copy, deadline] (const boost::system::error_code &error) {
          if (error)
              return;
          const auto current = weak.lock ();
          if (!current)
              return;
          std::function<void ()> close;
          {
              const std::lock_guard lock (current->gate);
              if (!current->closing.load (std::memory_order_acquire)
                  || !current->exact (replacement_copy))
                  return;
              close = current->close_connection;
          }
          if (close)
              close ();
      });

    const auto completed = [weak, replacement_copy, deadline] (const result_t<void> &) {
        try {
            deadline->cancel ();
        }
        catch (const boost::system::system_error &) {
        }
        auto delay = std::make_shared<asio::steady_timer> (asio::system_executor{});
        delay->expires_after (std::chrono::milliseconds (100));
        delay->async_wait (
          [weak, replacement_copy, delay] (const boost::system::error_code &error) {
              if (error)
                  return;
              const auto current = weak.lock ();
              if (!current)
                  return;
              std::function<void ()> close;
              {
                  const std::lock_guard lock (current->gate);
                  if (!current->closing.load (std::memory_order_acquire)
                      || !current->exact (replacement_copy))
                      return;
                  close = current->close_connection;
              }
              if (close)
                  close ();
          });
    };
    const auto submitted = _runtime.dispatch_actor_binding_replaced_async (
      state->stream, replacement_copy.actor_authority.actor_id,
      [weak] (stream_t &dispatch_stream, std::string actor_id) {
          const auto current = weak.lock ();
          if (!current) {
              return task_t<void> (result_t<void>::success ());
          }
          return current->dispatch_if_active (dispatch_stream, std::move (actor_id));
      },
      completed);
    admission_lock.unlock ();
    if (!submitted)
        completed (submitted);
    return true;
}

std::shared_ptr<stream_host_service_t::listener_t::replacement_session_state_t>
stream_host_service_t::listener_t::register_replacement_session (
  const zlink::routing_id_t &session_rid,
  const stream_t &stream,
  std::function<void ()> close_connection,
  detail::stream_runtime_t::actor_binding_replaced_dispatch_t dispatch_replacement)
{
    auto actor_gateway = _services->get_required<detail::actor_gateway_runtime_t> ();
    auto state = std::make_shared<replacement_session_state_t> (
      session_rid, stream, std::move (close_connection), std::move (dispatch_replacement),
      actor_gateway, _runtime_failures);
    auto registered = actor_gateway.register_bound_session_replacement_handler (
      session_rid, [this, weak = std::weak_ptr<replacement_session_state_t> (state)] (
                     const runtime::protocol::bound_session_replaced_t &replacement) {
          if (const auto current = weak.lock ())
              return begin_actor_binding_replacement (current, replacement);
          return false;
      });
    {
        const std::lock_guard lock (state->gate);
        state->handler = std::move (registered);
    }
    return state;
}

void stream_host_service_t::listener_t::configure_disconnect (
  session_actor_manager_t &actors,
  runtime::stateful::stream_connection_t connection,
  zlink::routing_id_t session_rid)
{
    const auto mesh = _mesh_node;
    auto gateway = _services->get_required<detail::actor_gateway_runtime_t> ();
    detail::session_actor_manager_access_t::bind_disconnect (
      actors,
      [mesh, connection] (const actor_ref_t &actor, std::uint64_t token) {
          auto binding = mesh->native_node ().sessions ().current_binding (
            std::string (actor.actor_id ().value ()));
          if (!binding || binding->connection != connection || binding->binding_generation != token)
              binding.reset ();
          return binding;
      },
      [mesh, gateway,
       session_rid] (runtime::stateful::stream_binding_t binding) mutable -> task_t<void> {
          auto removed = mesh->native_node ().sessions ().unbind (binding);
          if (!removed.second)
              co_return;
          binding = std::move (*removed.second);
          const auto local = mesh->native_node ().status ();
          if (binding.actor.node_id == local.routing_id ().to_string ()) {
              const auto actor = detail::actor_ref_access_t::make (
                node_rid_t::from_string (binding.actor.node_id), {}, binding.actor.key,
                binding.actor.object_generation);
              (void) gateway.retire_bound_session_route (actor, local.routing_id (), session_rid,
                                                         binding.binding_generation);
          } else {
              co_await mesh->retire_application_actor_session (binding, session_rid,
                                                               session_binding_retirement_timeout);
          }
      });
}

request_task_t<void> stream_host_service_t::listener_t::bind_actor_session (
  runtime::stateful::stream_connection_t transport_connection,
  zlink::routing_id_t session_rid,
  actor_ref_t actor,
  std::shared_ptr<replacement_session_state_t> replacement,
  std::uint64_t binding_generation,
  std::function<void ()> publish_client_binding)
{
    const auto _mesh_node = this->_mesh_node;
    auto *const services = _services;
    const auto binding_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    if (!_mesh_node) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "STREAM Actor dispatch MeshNode is not started");
    }
    const auto local_node = co_await _mesh_node->routing_id_async ();
    if (!local_node) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "STREAM Actor dispatch MeshNode has no routing id");
    }
    auto actor_route = co_await services->get_required<actor_address_resolver_t> ()
                         .resolve_actor_session_binding_address (actor);
    if (!actor_route) {
        throw framework_exception_t (framework_error_kind_t::unavailable,
                                     "Actor session binding route is unavailable");
    }
    runtime::stateful::stream_binding_t binding;
    runtime::stateful::stateful_error_t error;
    if (local_node->to_hex () == actor_route->node_rid.to_hex ()) {
        const auto native_actor = runtime::host::public_host_runtime_t::remote_actor_ref (
          actor_route->node_rid, std::string (actor.actor_id ().value ()),
          actor.object_generation ());
        const auto resolved =
          co_await _mesh_node->native_node ().resolve_actor_async (native_actor);
        if (!resolved) {
            throw framework_exception_t (framework_error_kind_t::unavailable,
                                         "Framework STREAM Actor is not ready for session binding");
        }
        const auto verified_actor =
          detail::make_local_application_actor_session_ref (*resolved, actor, *actor_route);
        if (!verified_actor) {
            throw verified_actor.error ()
              ? *verified_actor.error ()
              : framework_exception_t (framework_error_kind_t::internal_failure,
                                       "Local Actor session binding failed");
        }
        binding = {transport_connection, binding_generation, verified_actor.value (),
                   actor_route->node_generation,
                   static_cast<std::uint64_t> (actor_route->owner.lease_generation)};
    } else {
        binding = {transport_connection, binding_generation,
                   runtime::stateful::object_ref_t{
                     runtime::stateful::object_kind_t::actor,
                     std::string (actor.actor_id ().value ()), actor.object_generation (),
                     actor_route->authority_owner_generation, actor_route->mesh_name,
                     actor_route->node_rid.to_string ()},
                   actor_route->node_generation,
                   static_cast<std::uint64_t> (actor_route->owner.lease_generation)};
    }

    result_t<void> recorded = result_t<void>::success ();
    decltype (replacement->actor_bindings)::node_type prepared_replacement;
    if (replacement) {
        decltype (replacement->actor_bindings) prepared;
        prepared.emplace (std::string (actor.actor_id ().value ()),
                          replacement_session_state_t::actor_binding_t{actor.object_generation (),
                                                                       binding_generation});
        prepared_replacement = prepared.extract (prepared.begin ());
    }
    const auto publish_session_route = [&] {
        const auto local_status = _mesh_node->native_node ().status ();
        // Session-Actor binding §4: this host is the Session owner; its
        // identity is its own descriptor value. Without one there is no binding.
        const auto local_owner = _mesh_node->session_route_owner ();
        if (!local_owner || local_owner->lease_generation <= 0)
            return result_t<void>::failure (framework_error_kind_t::unavailable,
                                            "Session owner has no live descriptor");
        auto actor_gateway = services->get_required<detail::actor_gateway_runtime_t> ();
        auto transition = actor_gateway.record_bound_session_route_transition (
          actor,
          detail::actor_bound_session_route_t{
            *local_node, session_rid, actor.object_generation (),
            local_status.lifecycle_generation (), binding.actor.authority_owner_generation,
            static_cast<std::uint64_t> (actor_route->owner.lease_generation),
            binding.binding_generation, 0, 0, false, local_owner->owner_id,
            static_cast<std::uint64_t> (local_owner->lease_generation)},
          [&] {
              if (publish_client_binding)
                  publish_client_binding ();
              if (replacement) {
                  const std::lock_guard lock (replacement->gate);
                  prepared_replacement.mapped ().binding_generation = binding.binding_generation;
                  const auto previous =
                    replacement->actor_bindings.find (prepared_replacement.key ());
                  if (previous != replacement->actor_bindings.end ())
                      previous->second = prepared_replacement.mapped ();
                  else
                      replacement->actor_bindings.insert (std::move (prepared_replacement));
              }
          });
        if (!transition) {
            return result_t<void>::failure (transition.error_kind (),
                                            transition.error ()
                                              ? transition.error ()->what ()
                                              : "bound Session route registration failed");
        }
        if (actor_gateway.trace_bound_session_send_stage_enabled ()) {
            actor_gateway.trace_bound_session_send_stage (
              actor.actor_id ().value (), "session_owner_route_publish",
              [&] {
                  return "session_rid=" + session_rid.to_hex ()
                         + " binding_generation=" + std::to_string (binding.binding_generation)
                         + " replaced=" + (transition.value ().changed ? "true" : "false");
              },
              &session_rid);
        }
        return result_t<void>::success ();
    };
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
      binding_deadline - std::chrono::steady_clock::now ());
    if (remaining <= std::chrono::milliseconds::zero ()) {
        recorded = result_t<void>::failure (framework_error_kind_t::deadline_exceeded,
                                            "Actor session binding deadline elapsed");
    } else {
        const auto accepted_binding =
          co_await detail::request_submission (_mesh_node->bind_application_actor_session (
            actor, session_rid, binding.binding_generation, *actor_route, remaining));
        binding.binding_generation = accepted_binding.binding_generation;
        binding.actor.authority_owner_generation = accepted_binding.authority_owner_generation;
    }
    if (recorded) {
        std::tie (error, binding) = _mesh_node->native_node ().sessions ().bind_remote (
          transport_connection, binding.actor, binding.target_node_generation,
          binding.owner_lease_generation,
          [&] (const runtime::stateful::stream_binding_t &committed) {
              binding = committed;
              recorded = publish_session_route ();
              return recorded ? runtime::stateful::stateful_error_t::none
                              : runtime::stateful::stateful_error_t::conflict;
          },
          binding.binding_generation);
        if (error != runtime::stateful::stateful_error_t::none)
            recorded =
              result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                       "Framework rejected STREAM actor binding publication");
    }
    if (recorded) {
        co_return;
    }

    throw recorded.error () ? *recorded.error ()
                            : framework_exception_t (framework_error_kind_t::internal_failure,
                                                     "Framework STREAM actor binding failed");
}

/* Coroutine: parameters are taken by value because the detached async
 * wrapper's frame unwinds before the first resume. */
task_t<void> stream_host_service_t::listener_t::retire_actor_session_bindings (
  runtime::stateful::stream_connection_t connection,
  zlink::routing_id_t session_rid,
  session_actor_manager_t &actors)
{
    /* Detached coroutine (retire_actor_session_bindings_async): copy the
     * mesh-node handle into the frame so iteration 2+ after a co_await
     * never reads `this` (session-reconnect-and-coroutine-lifetime doc). */
    const auto _mesh_node = this->_mesh_node;
    if (!_mesh_node)
        co_return;
    const auto local = _mesh_node->native_node ().status ();
    auto actor_gateway = _services->get_required<detail::actor_gateway_runtime_t> ();
    std::vector<runtime::stateful::stream_binding_t> bindings;
    co_await detail::session_actor_manager_access_t::disconnect_async (actors, [&] {
        auto closed = _mesh_node->native_node ().sessions ().close (connection);
        bindings = std::move (closed.bindings);
        return bindings;
    });
    const auto retire_connection_details = [&connection] {
        return "connection_id=" + connection.connection_id
               + " connection_generation=" + std::to_string (connection.connection_generation);
    };
    std::vector<std::pair<runtime::stateful::stream_binding_t, task_t<void>>> remote_retirements;
    remote_retirements.reserve (bindings.size ());
    for (const auto &binding : bindings) {
        try {
            if (binding.actor.node_id == local.routing_id ().to_string ()) {
                const auto actor = detail::actor_ref_access_t::make (
                  node_rid_t::from_string (binding.actor.node_id), {}, binding.actor.key,
                  binding.actor.object_generation);
                const auto retired = actor_gateway.retire_bound_session_route (
                  actor, local.routing_id (), session_rid, binding.binding_generation);
                if (!retired)
                    continue;
            } else {
                remote_retirements.emplace_back (
                  binding, _mesh_node->retire_application_actor_session (
                             binding, session_rid, session_binding_retirement_timeout));
                continue;
            }
            if (actor_gateway.trace_bound_session_send_stage_enabled ()) {
                actor_gateway.trace_bound_session_send_stage (
                  binding.actor.key, "session_owner_route_tombstone",
                  [&] {
                      return retire_connection_details () + " session_rid=" + session_rid.to_hex ()
                             + " binding_generation=" + std::to_string (binding.binding_generation);
                  },
                  &session_rid);
            }
        }
        catch (const std::exception &error) {
            if (actor_gateway.trace_bound_session_send_stage_enabled ()) {
                actor_gateway.trace_bound_session_send_stage (
                  binding.actor.key, "session_owner_route_tombstone_failed",
                  [&] {
                      return retire_connection_details () + " session_rid=" + session_rid.to_hex ()
                             + " binding_generation=" + std::to_string (binding.binding_generation)
                             + " error=" + error.what ();
                  },
                  &session_rid);
            }
        }
    }
    for (auto &[binding, retirement] : remote_retirements) {
        try {
            co_await retirement;
            if (actor_gateway.trace_bound_session_send_stage_enabled ()) {
                actor_gateway.trace_bound_session_send_stage (
                  binding.actor.key, "session_owner_route_tombstone",
                  [&] {
                      return retire_connection_details () + " session_rid=" + session_rid.to_hex ()
                             + " binding_generation=" + std::to_string (binding.binding_generation);
                  },
                  &session_rid);
            }
        }
        catch (const std::exception &error) {
            if (actor_gateway.trace_bound_session_send_stage_enabled ()) {
                actor_gateway.trace_bound_session_send_stage (
                  binding.actor.key, "session_owner_route_tombstone_failed",
                  [&] {
                      return retire_connection_details () + " session_rid=" + session_rid.to_hex ()
                             + " binding_generation=" + std::to_string (binding.binding_generation)
                             + " error=" + error.what ();
                  },
                  &session_rid);
            }
        }
    }
    co_return;
}

void stream_host_service_t::listener_t::retire_actor_session_bindings_async (
  runtime::stateful::stream_connection_t connection,
  zlink::routing_id_t session_rid,
  session_actor_manager_t &actors,
  std::function<void ()> completed)
{
    auto pending = std::make_shared<task_t<void>> (
      retire_actor_session_bindings (connection, session_rid, actors));
    detail::observe_task_completion (
      *pending, [pending, completed = std::move (completed)] (const result_t<void> &) mutable {
          if (completed)
              completed ();
      });
}

void stream_host_service_t::listener_t::track_retiring_session (std::string retirement_id,
                                                                std::shared_ptr<const void> owner,
                                                                stream_t stream)
{
    {
        const std::lock_guard lock (_core_sessions_mutex);
        _retiring_sessions.emplace (std::move (retirement_id),
                                    retiring_session_t{std::move (owner), std::move (stream)});
        ++_core_sessions_revision;
    }
    _core_sessions_changed.notify_all ();
}

void stream_host_service_t::listener_t::forget_retiring_session (const std::string &retirement_id,
                                                                 const void *owner) noexcept
{
    {
        const std::lock_guard lock (_core_sessions_mutex);
        const auto found = _retiring_sessions.find (retirement_id);
        if (found != _retiring_sessions.end () && found->second.owner.get () == owner) {
            _retiring_sessions.erase (found);
            ++_core_sessions_revision;
        }
    }
    _core_sessions_changed.notify_all ();
}

void stream_host_service_t::listener_t::wait_for_retiring_sessions () noexcept
{
    for (;;) {
        std::vector<stream_t> retiring;
        std::uint64_t observed_revision = 0;
        {
            std::lock_guard lock (_core_sessions_mutex);
            if (_retiring_sessions.empty ()) {
                return;
            }
            observed_revision = _core_sessions_revision;
            retiring.reserve (_retiring_sessions.size ());
            for (const auto &[_, session] : _retiring_sessions)
                retiring.push_back (session.stream);
        }
        for (auto &stream : retiring) {
            _runtime.drain_async_dispatch (stream);
        }
        std::unique_lock lock (_core_sessions_mutex);
        if (_retiring_sessions.empty ()) {
            return;
        }
        _core_sessions_changed.wait (lock, [this, observed_revision] {
            return _retiring_sessions.empty () || _core_sessions_revision != observed_revision;
        });
    }
}

std::shared_ptr<void> stream_host_service_test_access_t::retain_connection (
  service_provider_t &services, stream_t stream, host::public_host_runtime_t &node)
{
    auto connection = node.sessions ().open (stream.session_id ());
    auto state = std::make_shared<stream_host_service_t::listener_t::stream_connection_state_t> (
      detail::service_scope_t::create (services, detail::service_scope_kind_t::stream_session),
      nullptr, std::move (stream), nullptr, connection);
    // Retirement has already closed the registry entry. A dispatch callback
    // can still hold the state when MeshNode releases its host.
    (void) node.sessions ().close (connection);
    return state;
}

} // namespace zlink::framework::runtime
