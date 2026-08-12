/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "actor_gateway_runtime.hpp"
#include "actor_ref_access.hpp"

#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include "runtime/streams/stream_runtime.hpp"

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

task_t<void> enqueue_session_relay (
  const std::shared_ptr<detail::actor_gateway_state_t> &state,
  std::string actor_id,
  std::function<result_t<void> ()> dispatch)
{
    auto completion =
      std::make_shared<detail::task_completion_source_t<void>> ();
    auto task = completion->task ();
    bool start_drain = false;
    {
        const std::lock_guard lock (state->mutex);
        state->pending_session_relays[actor_id].push_back (
          {std::move (dispatch), completion});
        start_drain = state->active_session_relays.insert (actor_id).second;
    }
    if (!start_drain)
        return task;

    const auto scheduled = detail::submit_blocking_call (
      [state, actor_id] {
          for (;;) {
              detail::actor_gateway_state_t::pending_session_relay_t pending;
              {
                  const std::lock_guard lock (state->mutex);
                  const auto found = state->pending_session_relays.find (actor_id);
                  if (found == state->pending_session_relays.end ()
                      || found->second.empty ()) {
                      state->pending_session_relays.erase (actor_id);
                      state->active_session_relays.erase (actor_id);
                      return;
                  }
                  pending = std::move (found->second.front ());
                  found->second.pop_front ();
              }
              try {
                  pending.completion->complete (pending.dispatch ());
              }
              catch (const framework_exception_t &error) {
                  pending.completion->complete (
                    detail::result_access_t::failure<void> (error));
              }
              catch (const std::exception &error) {
                  pending.completion->complete (result_t<void>::failure (
                    framework_error_kind_t::internal_failure, error.what ()));
              }
          }
      });
    if (!scheduled) {
        std::deque<detail::actor_gateway_state_t::pending_session_relay_t> rejected;
        {
            const std::lock_guard lock (state->mutex);
            rejected = std::move (state->pending_session_relays[actor_id]);
            state->pending_session_relays.erase (actor_id);
            state->active_session_relays.erase (actor_id);
        }
        for (auto &pending : rejected) {
            pending.completion->complete (result_t<void>::failure (
              framework_error_kind_t::capacity_exceeded,
              "bound Session relay executor is full"));
        }
    }
    return task;
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
    const std::lock_guard lock (_state->mutex);
    const auto found = _state->actors_by_id.find (std::string (_actor_ref->actor_id ().value ()));
    if (found != _state->actors_by_id.end ()) {
        if (found->second.ref.object_generation () != _actor_ref->object_generation ()) {
            return task_t<void> (result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                                          "actor generation is stale"));
        }
        found->second.bound = false;
        found->second.disconnected = true;
    }
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
    {
        const std::lock_guard lock (_state->mutex);
        const auto actor_id = std::string (_actor_ref->actor_id ().value ());
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
        }
    }
    if (!sink && !remote_sender) {
        return bound_session_send_call_t (send_call_t (result_t<void>::failure (
          framework_error_kind_t::not_configured, "actor bound session has no send sink")));
    }
    return bound_session_send_call_t (send_call_t (
      std::move (packet_name),
      [sink = std::move (sink), remote_sender = std::move (remote_sender), actor_ref = *_actor_ref,
       expected_binding_generation = _expected_binding_generation, header = std::move (header),
       payload, codec] (const std::string &name, const send_call_t::metadata_map_t &) mutable {
          try {
              if (sink)
                  return (*sink) (name, codec, payload).result ();
              return remote_sender (actor_ref, expected_binding_generation, header, payload);
          }
          catch (const framework_exception_t &error) {
              return detail::result_access_t::failure<void> (error);
          }
          catch (const std::exception &error) {
              return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                              error.what ());
          }
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

result_t<detail::actor_join_reply_t> actor_context_t::join_spot_erased (
  spot_id_t spot_id, const zlink::message_t &request, std::chrono::milliseconds timeout)
{
    detail::actor_gateway_state_t::join_spot_dispatcher_t dispatcher;
    {
        const std::lock_guard lock (_state->mutex);
        if (!_actor_ref || ::zlink::framework::detail::actor_ref_access_t::empty (*_actor_ref)) {
            return result_t<detail::actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                                  "actor ref is empty");
        }
        if (spot_id.empty ()) {
            return result_t<detail::actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                                  "spot id is empty");
        }
        if (!_state->join_spot_dispatcher) {
            return result_t<detail::actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "actor join spot dispatcher is not configured");
        }
        dispatcher = _state->join_spot_dispatcher;
    }
    detail::message_flow_tracer_t (_state->dispatch).trace (message_flow_outcome_t::sent, [&] {
        return message_flow_event_t{message_flow_outcome_t::sent,
                                    dispatch_error_surface_t::spot_actor,
                                    dispatch_message_kind_t::actor_request,
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt,
                                    std::string (spot_id),
                                    std::string (_actor_ref->actor_id ().value ()),
                                    std::nullopt};
    });
    auto joined = dispatcher (*_actor_ref, spot_id, request, timeout);
    if (!joined) {
        const auto *error = joined.error ();
        return result_t<detail::actor_join_reply_t>::failure (
          joined.error_kind (), error != nullptr ? error->what () : "actor join spot failed");
    }
    detail::message_flow_tracer_t (_state->dispatch)
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
                                      std::string (_actor_ref->actor_id ().value ()),
                                      std::nullopt};
      });

    if (joined.value ().result_code == 0) {
        const std::lock_guard lock (_state->mutex);
        *_actor_ref = joined.value ().actor;
        auto found = _state->actors_by_id.find (std::string (_actor_ref->actor_id ().value ()));
        if (found != _state->actors_by_id.end ()) {
            found->second.ref = *_actor_ref;
        }
    }
    return joined;
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
        const std::lock_guard lock (_state->mutex);
        if (::zlink::framework::detail::actor_ref_access_t::empty (_ref)) {
            return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_found,
                                                          "session actor is not bound"));
        }
        const auto found = _state->actors_by_id.find (std::string (_ref.actor_id ().value ()));
        if (found != _state->actors_by_id.end () && found->second.disconnected) {
            return task_t<void> (detail::boundary_failure<void> (
              detail::boundary_error_t::disconnected, "actor session is disconnected"));
        }
        if (found == _state->actors_by_id.end ()) {
            return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_found,
                                                          "actor route is not found"));
        }
        if (!found->second.bound) {
            return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_configured,
                                                          "actor session is not bound"));
        }
        if (_binding_token != 0 && found->second.binding_token != _binding_token) {
            return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_configured,
                                                          "actor session binding is stale"));
        }
        if (found->second.ref.object_generation () != _ref.object_generation ()) {
            return task_t<void> (result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                                          "actor generation is stale"));
        }
        _ref = found->second.ref;
        if (!_state->relay_dispatcher) {
            _state->relayed_frames.push_back (detail::relayed_frame_t{_ref, header, payload});
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
        if (const auto flow_id = header.flow_id ())
            relay_header.with_flow (std::string (*flow_id), *header.flow_origin ());
    }

    const auto actor_id = std::string (_ref.actor_id ().value ());
    auto actor_context = std::make_shared<actor_context_t> (context ());
    return enqueue_session_relay (
      _state, actor_id,
      [dispatcher = std::move (dispatcher), actor = _ref,
       actor_context = std::move (actor_context), relay_header = std::move (relay_header),
       payload, relay_source = std::move (relay_source)] () mutable {
          auto dispatched = dispatcher (actor, std::move (*actor_context), relay_header,
                                        payload, std::move (relay_source));
          if (!dispatched)
              return detail::propagate_failure<void> (dispatched, "actor relay failed");
          return result_t<void>::success ();
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
            _state->relayed_frames.push_back (detail::relayed_frame_t{_ref, *header, payload});
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
        if (const auto flow_id = header->flow_id ())
            relay_header.with_flow (std::string (*flow_id), *header->flow_origin ());
    }
    auto dispatched = dispatcher (_ref, context (), relay_header, payload,
                                  std::move (relay_source));
    if (!dispatched) {
        return relay_request_call_t (
          detail::propagate_failure<zlink::message_t> (dispatched, "actor relay failed"));
    }
    if (!dispatched.value ()) {
        /* framework API §2.4.3: reply frame이 없는 local 경로다. caller의 task를 framework
         * 오류로 완료하고, 그 실패를 fail_caller 액션으로 관측할 수 있게 남긴다. */
        const framework_exception_t missing_reply (framework_error_kind_t::protocol_error,
                                                   "actor relay request has no reply");
        detail::dispatch_error_reporter_t (_state->dispatch)
          .report (message_dispatch_error_event_t{
            dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
            dispatch_error_reason_t::reply_path_missing, dispatch_error_action_t::fail_caller,
            std::string (header->packet_name ()), std::nullopt, std::nullopt, std::nullopt,
            std::string (_ref.actor_id ().value ()), std::nullopt, std::nullopt,
            std::make_exception_ptr (missing_reply)});
        return relay_request_call_t (
          detail::result_access_t::failure<zlink::message_t> (missing_reply));
    }
    return relay_request_call_t (
      result_t<zlink::message_t>::success (std::move (*dispatched.value ())));
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
        const std::lock_guard lock (_state->mutex);
        const auto found = _state->actors_by_id.find (std::string (_ref.actor_id ().value ()));
        if (found == _state->actors_by_id.end ()) {
            return task_t<void> (result_t<void>::success ());
        }
        if (_binding_token != 0 && found->second.binding_token != _binding_token) {
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::not_configured, "actor session binding is stale"));
        }
        if (found->second.ref.object_generation () != _ref.object_generation ()) {
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::invalid_operation, "actor generation is stale"));
        }
        if (found->second.disconnected)
            return task_t<void> (result_t<void>::success ());
        _ref = found->second.ref;
        found->second.bound = false;
        found->second.disconnected = true;
        session_id = found->second.binding_session_id;
        binding_token = found->second.binding_token;
        dispatcher = _state->disconnect_dispatcher;
    }
    result_t<void> notified = result_t<void>::success ();
    if (dispatcher) {
        try {
            notified = dispatcher (_ref);
        }
        catch (const framework_exception_t &error) {
            notified = detail::result_access_t::failure<void> (error);
        }
        catch (const std::exception &error) {
            notified = result_t<void>::failure (framework_error_kind_t::not_found, error.what ());
        }
    }
    if (binding_token != 0) {
        detail::actor_gateway_runtime_t (_state).unbind_session_stream (
          std::string (_ref.actor_id ().value ()), std::move (session_id), binding_token);
    }
    return task_t<void> (std::move (notified));
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
    try {
        const auto token = bind_current_session (actor_ref);
        return request_call_t<session_actor_t> (
          result_t<session_actor_t>::success (session_actor_t (_state, actor_ref, token)));
    }
    catch (const framework_exception_t &error) {
        return request_call_t<session_actor_t> (
          detail::result_access_t::failure<session_actor_t> (error));
    }
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
    try {
        const auto token = bind_current_session (actor_ref);
        return request_call_t<session_actor_t> (
          result_t<session_actor_t>::success (session_actor_t (_state, actor_ref, token)));
    }
    catch (const framework_exception_t &error) {
        return request_call_t<session_actor_t> (
          detail::result_access_t::failure<session_actor_t> (error));
    }
}

std::uint64_t session_actor_manager_t::bind_current_session (const actor_ref_t &actor_ref)
{
    const auto actor_id = std::string (actor_ref.actor_id ().value ());
    const auto publish_without_stream = [&] {
        const std::lock_guard lock (_state->mutex);
        auto found = _state->actors_by_id.find (actor_id);
        if (found == _state->actors_by_id.end ()) {
            _state->actors_by_id.emplace (
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
    if (!_binding_context) {
        publish_without_stream ();
        return 0;
    }
    const std::lock_guard binding_lock (_binding_context->mutex);
    if (!_binding_context->stream) {
        publish_without_stream ();
        return 0;
    }
    std::uint64_t token;
    {
        const std::lock_guard lock (_state->mutex);
        token = _state->next_binding_token++;
    }
    const auto prior_context_token =
      _binding_context->actor_tokens.find (actor_id);
    const auto previous_context_token =
      prior_context_token == _binding_context->actor_tokens.end ()
        ? std::optional<std::uint64_t>{}
        : std::make_optional (prior_context_token->second);
    _binding_context->actor_tokens[actor_id] = token;
    std::optional<detail::actor_session_binding_snapshot_t> previous;
    try {
        previous = detail::actor_gateway_runtime_t (_state).bind_session_stream (
          actor_id, *_binding_context->stream, _binding_context->codec,
          _binding_context->session_id, token, actor_ref);
        if (_binding_context->native_binder) {
            auto bound = _binding_context->native_binder (actor_ref);
            if (!bound) {
                throw framework_exception_t (bound.error_kind (),
                                             bound.error () ? bound.error ()->what ()
                                                            : "Core STREAM actor binding failed");
            }
        }
    }
    catch (...) {
        const auto current = _binding_context->actor_tokens.find (actor_id);
        if (current != _binding_context->actor_tokens.end () && current->second == token) {
            if (previous_context_token) {
                current->second = *previous_context_token;
            } else {
                _binding_context->actor_tokens.erase (current);
            }
        }
        if (previous) {
            detail::actor_gateway_runtime_t (_state).restore_session_stream (
              actor_id, _binding_context->session_id, token,
              std::move (*previous));
        }
        throw;
    }
    return token;
}

void detail::session_actor_manager_access_t::attach (session_actor_manager_t &manager,
                                                     stream_t stream)
{
    stream._state->actors.store (&manager, std::memory_order_release);
    const std::lock_guard lock (manager._binding_context->mutex);
    manager._binding_context->session_id = stream.session_id ();
    manager._binding_context->stream = std::move (stream);
}

void detail::session_actor_manager_access_t::set_codec (session_actor_manager_t &manager,
                                                        stream_codec_t codec)
{
    const std::lock_guard lock (manager._binding_context->mutex);
    manager._binding_context->codec = codec;
}

void detail::session_actor_manager_access_t::bind_native (
  session_actor_manager_t &manager, std::function<result_t<void> (const actor_ref_t &)> binder)
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
        session_id = manager._binding_context->session_id;
        if (manager._binding_context->stream) {
            manager._binding_context->stream->_state->actors.store (nullptr,
                                                                    std::memory_order_release);
        }
        manager._binding_context->stream.reset ();
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
           && left.binding_generation == right.binding_generation;
}

actor_bound_session_route_t
merge_bound_session_route_fence (const actor_bound_session_route_t &current,
                                 actor_bound_session_route_t next)
{
    if (next.binding_token == 0)
        next.binding_token = current.binding_token;
    next.session_sequence = std::max (
      current.session_sequence, next.session_sequence);
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
              const std::lock_guard lock (state->mutex);
              const auto found = state->actors_by_id.find (actor_id);
              if (found == state->actors_by_id.end ()
                  || !found->second.bound
                  || found->second.disconnected
                  || !found->second.bound_session_route
                  || *found->second.bound_session_route
                       != staged_route) {
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
              return task_t<void> (sender (
                staged_actor, binding_generation, header, payload));
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
        keep_existing_sink = found->second.bound && !found->second.disconnected
                             && (found->second.bound_session_stream_sink || !replace_existing)
                             && existing_sink != state->bound_session_sinks.end ();
        actor_ref = merge_actor_type (actor_ref, found->second.ref);
        found->second.ref = actor_ref;
        found->second.bound = true;
        found->second.disconnected = false;
        found->second.bound_session_codec = codec;
        if (!keep_existing_sink)
            found->second.bound_session_stream_sink = false;
        if (route) {
            if (route->binding_generation == 0)
                route->binding_generation = found->second.source_binding_generation;
            if (route->binding_token == 0)
                route->binding_token = found->second.binding_token;
            route->object_generation = actor_ref.object_generation ();
            if (found->second.bound_session_route
                && same_physical_bound_session (*found->second.bound_session_route, *route)) {
                *route = merge_bound_session_route_fence (
                  *found->second.bound_session_route, std::move (*route));
                found->second.bound_session_route = *route;
                if (!keep_existing_sink) {
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
    if (!keep_existing_sink)
        state->bound_session_sinks[actor_id] =
          std::make_shared<bound_session_sink_t> (std::move (sink));
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
              const std::lock_guard lock (state->mutex);
              const auto found = state->actors_by_id.find (actor_id);
              if (found == state->actors_by_id.end () || !found->second.bound) {
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
                                                     std::uint64_t session_sequence)
{
    auto transition = record_bound_session_route_transition (
      actor_ref, actor_bound_session_route_t{std::move (node_rid), std::move (session_rid),
                                             actor_ref.object_generation (), node_generation,
                                             authority_owner_generation, owner_lease_generation,
                                             binding_generation, binding_token, session_sequence});
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
        if (actor_ref_access_t::actor_type (found->second.ref).empty ()
            && !actor_ref_access_t::actor_type (actor_ref).empty ()) {
            found->second.ref = merge_actor_type (actor_ref, found->second.ref);
        }
        found->second.bound = true;
        found->second.disconnected = false;
    }
    if (route.binding_generation == 0)
        route.binding_generation = found->second.source_binding_generation;
    if (route.binding_token == 0)
        route.binding_token = found->second.binding_token;
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
    if (session_sequence != route.session_sequence
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
  const runtime::stateful::stream_binding_t &target,
  std::uint64_t sealed_high_water)
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
             != route.route.target_node_generation
        || sealed_high_water != route.route.replayed_high_water)
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
             != route.binding_generation
        || gateway_route.session_sequence != sealed_high_water)
        return false;

    found->second.ref = actor_ref_access_t::make (
      node_rid_t::from_string (target.actor.node_id),
      std::string (actor_ref_access_t::actor_type (found->second.ref)),
      actor_id, route.actor.object_generation);
    gateway_route.authority_owner_generation =
      target.actor.authority_owner_generation;
    gateway_route.owner_lease_generation = target.owner_lease_generation;
    gateway_route.session_sequence = sealed_high_water;
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
    {
        const std::lock_guard lock (_state->mutex);
        const auto actor_id = std::string (
          actor_ref.actor_id ().value ());
        const auto found = _state->actors_by_id.find (actor_id);
        if (found == _state->actors_by_id.end ()
            || !found->second.bound
            || !actor_types_compatible (
              found->second.ref, actor_ref)
            || found->second.ref.object_generation ()
                 != actor_ref.object_generation ())
            return std::nullopt;
        if (found->second.bound_session_route) {
            const auto &route = *found->second.bound_session_route;
            if (route.object_generation
                  != actor_ref.object_generation ()
                || (binding_generation != 0
                    && route.binding_generation != 0
                    && binding_generation
                         != route.binding_generation))
                return std::nullopt;
        }
        const auto found_sink =
          _state->bound_session_sinks.find (actor_id);
        if (found_sink == _state->bound_session_sinks.end ())
            return std::nullopt;
        sink = found_sink->second;
    }
    return admitted_bound_session_delivery_t{
      [sink = std::move (sink)] (
        std::string packet_name,
        stream_codec_t codec,
        const zlink::message_t &payload) {
          auto sent = (*sink) (
            std::move (packet_name), codec, payload).result ();
          if (!sent) {
              return result_t<void>::failure (
                sent.error_kind (),
                sent.error () ? sent.error ()->what ()
                              : "actor bound session dispatch failed");
          }
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
    _state->bound_session_replacement_handlers.insert_or_assign (session_rid.to_hex (), registered);
    return registered;
}

void actor_gateway_runtime_t::unregister_bound_session_replacement_handler (
  const zlink::routing_id_t &session_rid,
  const std::shared_ptr<bound_session_replacement_handler_t> &handler)
{
    const std::lock_guard lock (_state->mutex);
    const auto found = _state->bound_session_replacement_handlers.find (session_rid.to_hex ());
    if (found != _state->bound_session_replacement_handlers.end () && found->second == handler) {
        _state->bound_session_replacement_handlers.erase (found);
    }
}

bool actor_gateway_runtime_t::dispatch_bound_session_replaced (
  const runtime::protocol::bound_session_replaced_t &replacement) const
{
    std::shared_ptr<bound_session_replacement_handler_t> handler;
    {
        const std::lock_guard lock (_state->mutex);
        const auto session_rid =
          zlink::routing_id_t::from (replacement.retired_session.session_routing_id);
        const auto found = _state->bound_session_replacement_handlers.find (session_rid.to_hex ());
        if (found == _state->bound_session_replacement_handlers.end ())
            return false;
        handler = found->second;
    }
    (*handler) (replacement);
    return true;
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
