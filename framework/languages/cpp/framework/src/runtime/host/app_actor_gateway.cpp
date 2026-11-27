/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/host/app_internal.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/configuration/service_scope.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/host/bound_session_bind.hpp"
#include "runtime/host/bound_session_send_stage_trace.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/utils/weak_runtime_owner.hpp"
#include <algorithm>
#include <cstdint>
#include <exception>
#include <type_traits>
#include <typeindex>
#include <variant>

namespace zlink::framework::detail
{

zlink::framework::result_t<void> one_way_native_submit_result (zlink::submit_result_t result,
                                                               std::string_view operation)
{
    using namespace zlink::framework;
    switch (result) {
        case zlink::submit_result_t::ok:
            return result_t<void>::success ();
        case zlink::submit_result_t::backpressured:
            // Backpressure is never a public terminal: it completes at the send
            // deadline as DeadlineExceeded (spec 05-async-execution-policy;
            // 32-framework-error-model:70), represented as a timeout boundary
            // like a request timeout — matching submit_result_mapper.
            return detail::boundary_failure<void> (detail::boundary_error_t::timed_out,
                                                   std::string (operation) + " is backpressured");
        case zlink::submit_result_t::not_found:
            return result_t<void>::failure (framework_error_kind_t::not_found,
                                            std::string (operation) + " target was not found");
        case zlink::submit_result_t::not_admitted:
            return result_t<void>::failure (framework_error_kind_t::rejected,
                                            std::string (operation) + " admission was rejected");
        case zlink::submit_result_t::not_connected:
            return result_t<void>::failure (framework_error_kind_t::unavailable,
                                            std::string (operation) + " route is not connected");
        case zlink::submit_result_t::terminated:
            return detail::boundary_failure<void> (detail::boundary_error_t::shutdown,
                                                   std::string (operation) + " runtime is stopped");
        case zlink::submit_result_t::invalid_argument:
        case zlink::submit_result_t::invalid_handle:
        case zlink::submit_result_t::invalid_state:
        case zlink::submit_result_t::thread_violation:
            return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                            std::string (operation) + " rejected an invalid call");
        case zlink::submit_result_t::not_supported:
        case zlink::submit_result_t::out_of_memory:
        case zlink::submit_result_t::seq_exhausted:
        case zlink::submit_result_t::internal_error:
            return result_t<void>::failure (
              runtime::messaging::map_submit_result_error_kind (result),
              std::string (operation) + " was not submitted");
        default:
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            std::string (operation) + " was not submitted");
    }
}

namespace
{

std::vector<zlink::message_t> encode_bound_session_frame (const stream_runtime_t &runtime,
                                                          const stream_header_t &header,
                                                          const zlink::message_t &payload)
{
    auto encoded_frame = runtime.encode_frame (header, payload);
    if (!encoded_frame)
        throw framework_exception_t (encoded_frame.error_kind (),
                                     encoded_frame.error () ? encoded_frame.error ()->what ()
                                                            : "bound Session frame encode failed");
    return {zlink::message_t::from (std::move (encoded_frame.value ()))};
}


class session_ingress_completion_t final
{
  public:
    session_ingress_completion_t () = default;
    ~session_ingress_completion_t () noexcept
    {
        if (_registry && _dispatch)
            (void) _registry->complete_inbound (*_dispatch);
    }

    session_ingress_completion_t (const session_ingress_completion_t &) = delete;
    session_ingress_completion_t &operator= (const session_ingress_completion_t &) = delete;

    void arm (runtime::stateful::stream_session_registry_t &registry,
              runtime::stateful::stream_dispatch_t dispatch)
    {
        _registry = &registry;
        _dispatch = std::move (dispatch);
    }

    runtime::stateful::stateful_error_t complete () noexcept
    {
        if (!_registry || !_dispatch)
            return runtime::stateful::stateful_error_t::none;
        auto dispatch = std::exchange (_dispatch, std::nullopt);
        return _registry->complete_inbound (*dispatch);
    }

  private:
    runtime::stateful::stream_session_registry_t *_registry = nullptr;
    std::optional<runtime::stateful::stream_dispatch_t> _dispatch;
};

zlink::framework::task_t<zlink::framework::result_t<void>> send_bound_session_from_app_task (
  std::shared_ptr<zlink::framework::detail::mesh_node_runtime_t> application_mesh,
  zlink::framework::detail::actor_gateway_runtime_t actor_gateway_runtime,
  zlink::framework::detail::stream_runtime_t stream_runtime,
  zlink::framework::actor_ref_t actor,
  std::uint64_t expected_binding_generation,
  zlink::framework::detail::stream_header_t header,
  zlink::message_t payload)
{
    using namespace zlink::framework;
    try {
        const auto route = co_await actor_gateway_runtime.bound_session_route_async (actor);
        if (!route || !route->session_rid || route->binding_generation == 0
            || (expected_binding_generation != 0
                && expected_binding_generation != route->binding_generation)) {
            co_return result_t<void>::failure (framework_error_kind_t::not_configured,
                                               "Actor bound Session route is not ready");
        }
        const auto local = application_mesh->native_node ().status ();
        if (actor_gateway_runtime.trace_bound_session_send_stage_enabled ()) {
            actor_gateway_runtime.trace_bound_session_send_stage (
              actor.actor_id ().value (), "actor_owner_push_target",
              [&] {
                  return "session_rid=" + route->session_rid->to_hex ()
                         + " binding_generation=" + std::to_string (route->binding_generation);
              },
              &*route->session_rid);
        }
        const auto local_actor = detail::actor_ref_access_t::make (
          node_rid_t::from_string (local.routing_id ().to_string ()),
          std::string (detail::actor_ref_access_t::actor_type (actor)),
          std::string (actor.actor_id ().value ()), actor.object_generation ());
        /* The deferred raw-send completion can outlive this frame, so the
         * trace factory must own every value it needs. */
        detail::bound_session_send_stage_trace_context_t stage_trace_context{
          &actor_gateway_runtime, local_actor.actor_id ().value (), &*route->session_rid};
        auto stage_trace = detail::make_bound_session_send_stage_trace (stage_trace_context);
        const auto submitted = co_await application_mesh->native_node ().send_bound_session (
          local_actor, route->node_rid, route->binding_generation,
          route->authority_owner_generation, route->owner_lease_generation,
          detail::encode_bound_session_frame (stream_runtime, header, payload),
          std::move (stage_trace));
        co_return one_way_native_submit_result (submitted, "Framework Actor bound Session send");
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<void> (error);
    }
    catch (const std::exception &error) {
        co_return result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ());
    }
}

} // namespace

zlink::framework::task_t<zlink::framework::runtime::host::bound_session_bind_operation_result_t>
apply_bound_session_bind_async (
  std::weak_ptr<zlink::framework::detail::actor_gateway_state_t> weak_actor_gateway_runtime,
  std::weak_ptr<zlink::framework::detail::mesh_node_runtime_t> weak_application_mesh,
  std::shared_ptr<zlink::framework::detail::spot_node_builder_state_t> spot_state,
  zlink::framework::detail::stream_runtime_t stream_runtime,
  zlink::framework::actor_ref_t actor,
  zlink::framework::runtime::protocol::bound_session_bind_t bind,
  zlink::routing_id_t session_owner,
  std::uint64_t session_owner_generation,
  zlink::framework::location_owner_token_t session_owner_token,
  std::function<bool (std::uint32_t,
                      std::uint32_t,
                      std::optional<zlink::framework::runtime::protocol::stream_bind_reply_tail_t>)>
    submit_terminal_reply,
  zlink::framework::detail::delay_owner_t &delays,
  std::chrono::milliseconds callback_timeout)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using bind_result_t = runtime::host::bound_session_bind_operation_result_t;
    actor_gateway_runtime_t actor_gateway_runtime (lock_runtime_owner (weak_actor_gateway_runtime));
    const auto session_rid = zlink::routing_id_t::from (bind.session_routing_id);
    if (actor_gateway_runtime.trace_bound_session_send_stage_enabled ()) {
        actor_gateway_runtime.trace_bound_session_send_stage (
          bind.actor.actor_id, "bound_session_bind_receive",
          [&] {
              return "new_session_rid=" + session_rid.to_hex ()
                     + " new_binding_generation=" + std::to_string (bind.binding.generation);
          },
          &session_rid);
    }
    if (bind.binding.state == runtime::protocol::bound_session_binding_state_t::tombstone) {
        const auto retired = actor_gateway_runtime.retire_bound_session_route (
          actor, session_owner, session_rid, bind.binding.generation);
        co_return bind_result_t{retired ? runtime::stateful::stateful_error_t::none
                                        : runtime::stateful::stateful_error_t::conflict,
                                std::nullopt};
    }

    auto sink = [weak_application_mesh, weak_actor_gateway_runtime, stream_runtime, actor,
                 session_owner, session_rid, binding_generation = bind.binding.generation,
                 authority_owner_generation = bind.actor.authority_owner_generation,
                 owner_lease_generation = bind.actor.owner_lease_generation] (
                  std::string packet_name, stream_codec_t codec,
                  const zlink::message_t &payload) mutable -> task_t<void> {
        const auto application_mesh = lock_runtime_owner (weak_application_mesh);
        actor_gateway_runtime_t actor_gateway_runtime (
          lock_runtime_owner (weak_actor_gateway_runtime));
        const stream_header_t header (stream_message_kind_t::send, codec,
                                      stream_header_flags_t::none, std::nullopt,
                                      std::move (packet_name));
        const actor_bound_session_route_t staged_route{session_owner,
                                                       session_rid,
                                                       actor.object_generation (),
                                                       0,
                                                       authority_owner_generation,
                                                       owner_lease_generation,
                                                       binding_generation,
                                                       0,
                                                       0};
        const auto current_route =
          actor_gateway_runtime.resolve_bound_session_push_route (actor, staged_route);
        if (!current_route || !current_route->session_rid)
            throw framework_exception_t (framework_error_kind_t::not_configured,
                                         "Framework Actor bound Session route is unavailable");
        bound_session_send_stage_trace_context_t trace_context{
          &actor_gateway_runtime, actor.actor_id ().value (), &*current_route->session_rid};
        auto stage_trace = make_bound_session_send_stage_trace (trace_context);
        const auto submitted = co_await application_mesh->native_node ().send_bound_session (
          actor, current_route->node_rid, current_route->binding_generation,
          current_route->authority_owner_generation, current_route->owner_lease_generation,
          encode_bound_session_frame (stream_runtime, header, payload), std::move (stage_trace));
        const auto result =
          one_way_native_submit_result (submitted, "Framework Actor bound Session send");
        if (!result)
            throw result.error ()
              ? *result.error ()
              : framework_exception_t (framework_error_kind_t::internal_failure,
                                       "Framework Actor bound Session send failed");
        co_return;
    };

    actor_bound_session_route_t incoming_route{
      session_owner,
      session_rid,
      bind.actor.object_generation,
      session_owner_generation,
      bind.actor.authority_owner_generation,
      bind.actor.owner_lease_generation,
      bind.binding.generation,
      0,
      0,
      false,
      session_owner_token.owner_id,
      static_cast<std::uint64_t> (session_owner_token.lease_generation)};
    auto replace_binding =
      [actor_gateway_runtime, actor, bind, incoming_route, sink = std::move (sink),
       submit_terminal_reply] () mutable -> ::zlink::framework::result_t<bind_result_t> {
        const auto transition = actor_gateway_runtime.replace_session_route (
          actor, std::move (sink), incoming_route, stream_codec_t::message_pack,
          [submit_terminal_reply] (const actor_bound_session_route_t &accepted) {
              return submit_terminal_reply (
                static_cast<std::uint32_t> (runtime::protocol::request_terminal_result::ok),
                static_cast<std::uint32_t> (runtime::protocol::framework_error_code::none),
                runtime::protocol::stream_bind_reply_tail_t{accepted.binding_generation,
                                                            accepted.authority_owner_generation});
          });
        if (!transition)
            return ::zlink::framework::result_t<bind_result_t>::failure (
              transition.error_kind (), transition.error ()
                                          ? transition.error ()->what ()
                                          : "Actor bound Session route replacement failed");

        std::optional<runtime::protocol::bound_session_replaced_t> replacement;
        const auto &change = transition.value ();
        if (actor_gateway_runtime.trace_bound_session_send_stage_enabled ()) {
            actor_gateway_runtime.trace_bound_session_send_stage (
              std::string (actor.actor_id ().value ()), "actor_owner_route_publish",
              [&] {
                  return "session_rid=" + incoming_route.session_rid->to_hex ()
                         + " binding_generation="
                         + std::to_string (incoming_route.binding_generation)
                         + " replaced=" + (change.changed ? "true" : "false");
              },
              &*incoming_route.session_rid);
        }
        if (change.changed && change.previous && change.previous->session_rid
            && change.previous->node_generation != 0 && change.previous->binding_generation != 0
            && !change.previous->session_owner_id.empty ()
            && change.previous->session_owner_lease_generation != 0) {
            replacement = runtime::protocol::bound_session_replaced_t{
              bind.actor,
              runtime::protocol::retired_bound_session_route_fence_t{
                change.previous->node_rid.to_bytes (), change.previous->node_generation,
                change.previous->session_owner_id, change.previous->session_owner_lease_generation,
                change.previous->session_rid->to_bytes (), change.previous->binding_generation}};
        }
        return ::zlink::framework::result_t<bind_result_t>::success (
          bind_result_t{runtime::stateful::stateful_error_t::none, std::move (replacement)});
    };
    auto is_duplicate = [incoming_route = std::move (incoming_route)] (
                          const std::optional<actor_bound_session_route_t> &current) {
        return current && current->session_rid && current->node_generation != 0
               && current->binding_generation != 0
               && !current->is_same_physical_session (incoming_route);
    };
    const auto callbacks = co_await spot_state->lane.run_task ([spot_state, actor] {
        auto actor_type = std::string (actor_ref_access_t::actor_type (actor));
        if (actor_type.empty ()) {
            const auto type =
              spot_state->actor_types_by_id.find (std::string (actor.actor_id ().value ()));
            if (type == spot_state->actor_types_by_id.end ())
                return std::pair{false, false};
            actor_type = type->second;
        }
        const auto factory = spot_state->actor_factories.find (actor_type);
        const auto instance = spot_state->actor_instances.find (
          actor_type + ":" + std::string (actor.actor_id ().value ()));
        if (factory == spot_state->actor_factories.end () || !factory->second.as_actor
            || instance == spot_state->actor_instances.end () || !instance->second)
            return std::pair{false, false};
        const auto *actor_instance = factory->second.as_actor (instance->second.get ());
        // A factory may return a subtype with overrides absent from its registered type.
        const bool exact_type =
          actor_instance
          && std::type_index (typeid (*actor_instance)) == factory->second.actor_type;
        return std::pair{exact_type && factory->second.duplicate_bind_is_default,
                         exact_type && factory->second.session_bound_is_default};
    });
    const bool has_duplicate_callback = !callbacks.first;
    const bool has_session_bound_callback = !callbacks.second;
    bool requires_actor_turn = has_session_bound_callback;
    if (!requires_actor_turn && has_duplicate_callback) {
        const auto current = co_await actor_gateway_runtime.bound_session_route_async (actor);
        requires_actor_turn = is_duplicate (current);
    }
    if (!requires_actor_turn)
        co_return replace_binding ();
    const auto failures = spot_state->runtime_failures ();
    const auto shutdown = spot_state->worker_cancellation.get_token ();
    struct callback_race_t
    {
        std::stop_source cancellation;
        bool try_finish (std::uint8_t terminal)
        {
            std::uint8_t pending = 0;
            return decision.compare_exchange_strong (pending, terminal);
        }

      private:
        std::atomic<std::uint8_t> decision{0};
    };
    auto race = std::make_shared<callback_race_t> ();
    auto completion = std::make_shared<task_completion_source_t<bind_result_t>> ();
    auto start_callback_timeout = [&delays, callback_timeout, completion, failures] (
                                    const std::shared_ptr<callback_race_t> &race, bool committed) {
        auto timeout = delay (delays, callback_timeout, race->cancellation.get_token ());
        observe_task_completion (
          timeout, [race, completion, failures, committed] (const result_t<void> &elapsed) {
              if (!race->try_finish (2))
                  return;
              if (committed) {
                  if (elapsed)
                      failures->report (std::make_exception_ptr (
                        framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                               "Actor session-bound callback timed out")));
                  race->cancellation.request_stop ();
                  return;
              }
              race->cancellation.request_stop ();
              if (!elapsed) {
                  completion->complete (::zlink::framework::result_t<bind_result_t>::failure (
                    elapsed.error_kind (),
                    elapsed.error () ? elapsed.error ()->what () : "Actor callback timer failed"));
                  return;
              }
              completion->complete (
                ::zlink::framework::result_t<bind_result_t>::success (bind_result_t{
                  runtime::stateful::stateful_error_t::unavailable, std::nullopt,
                  static_cast<std::uint32_t> (runtime::protocol::request_terminal_result::timedOut),
                  static_cast<std::uint32_t> (runtime::protocol::framework_error_code::none),
                  std::nullopt}));
          });
    };
    auto spot_runtime = spot_node_runtime_t (spot_state);
    struct accept_session_binding_t
    {
        static task_t<::zlink::framework::result_t<bool>>
        run (actor_session_binding_identity_t incoming_identity,
             std::decay_t<decltype (replace_binding)> replace_binding,
             std::decay_t<decltype (race)> race,
             std::decay_t<decltype (completion)> completion,
             std::decay_t<decltype (start_callback_timeout)> start_callback_timeout,
             actor_t *actor_instance,
             std::stop_token cancellation,
             bool has_duplicate_callback,
             bool has_session_bound_callback,
             std::shared_ptr<runtime::runtime_failure_collector_t> failures,
             std::stop_token shutdown,
             std::shared_ptr<spot_node_builder_state_t> owner,
             actor_gateway_runtime_t gateway,
             actor_ref_t actor,
             std::decay_t<decltype (is_duplicate)> is_duplicate)
        {
            std::optional<actor_bound_session_route_t> current;
            if (has_duplicate_callback)
                current = co_await gateway.bound_session_route_async (actor);
            if (is_duplicate (current)) {
                const actor_session_binding_identity_t current_identity{
                  node_rid_t::from_string (current->node_rid.to_string ()),
                  current->node_generation, current->binding_generation};
                start_callback_timeout (race, false);
                const auto rejection = co_await actor_instance->on_duplicate_bind (
                  current_identity, incoming_identity, cancellation);
                if (!race->try_finish (1))
                    co_return ::zlink::framework::result_t<bool>::success (false);
                if (rejection && !rejection->empty ()) {
                    completion->complete (
                      ::zlink::framework::result_t<bind_result_t>::success (bind_result_t{
                        runtime::stateful::stateful_error_t::conflict, std::nullopt,
                        static_cast<std::uint32_t> (
                          runtime::protocol::request_terminal_result::rejected),
                        static_cast<std::uint32_t> (runtime::protocol::framework_error_code::none),
                        runtime::protocol::stream_bind_reply_tail_t{0, 0, *rejection}}));
                    co_return ::zlink::framework::result_t<bool>::success (false);
                }
                if (cancellation.stop_requested ())
                    co_return ::zlink::framework::result_t<bool>::success (false);

                race->cancellation.request_stop ();
            }
            auto transition = replace_binding ();
            if (!transition)
                co_return ::zlink::framework::result_t<bool>::failure (
                  transition.error_kind (), transition.error ()
                                              ? transition.error ()->what ()
                                              : "Actor bound Session route replacement failed");
            completion->complete (std::move (transition));
            if (has_session_bound_callback) {
                auto notification = std::make_shared<callback_race_t> ();
                std::stop_callback stop (shutdown, [notification] {
                    (void) notification->try_finish (2);
                    notification->cancellation.request_stop ();
                });
                start_callback_timeout (notification, true);
                auto outcome = result_t<void>::success ();
                bool cancelled = false;
                try {
                    co_await actor_instance->on_session_bound (
                      incoming_identity, notification->cancellation.get_token ());
                }
                catch (const std::exception &error) {
                    cancelled = detail::is_cancellation_exception (error);
                    outcome = detail::current_exception_result<void> ();
                }
                catch (...) {
                    outcome = detail::current_exception_result<void> ();
                }
                const bool terminal = notification->try_finish (1);
                notification->cancellation.request_stop ();
                if (terminal && !outcome) {
                    bool actor_ending = false;
                    if (cancelled)
                        actor_ending = co_await owner->lane.run_task ([owner, actor_instance] {
                            const auto &ref = actor_instance->context ().actor_ref ();
                            const auto key = std::string (actor_ref_access_t::actor_type (ref))
                                             + ":" + std::string (ref.actor_id ().value ());
                            return owner->destroying_actors.contains (key)
                                   || !owner->actor_instances.contains (key);
                        });
                    if (!cancelled || (!shutdown.stop_requested () && !actor_ending))
                        failures->report (outcome.exception ());
                }
            }
            co_return ::zlink::framework::result_t<bool>::success (true);
        }
    };
    auto turn = spot_runtime.run_actor_turn (
      actor,
      [replace_binding = std::move (replace_binding),
       incoming_identity =
         actor_session_binding_identity_t{node_rid_t::from_string (session_owner.to_string ()),
                                          session_owner_generation, bind.binding.generation},
       race, completion, start_callback_timeout, has_duplicate_callback, has_session_bound_callback,
       failures, shutdown, spot_state, actor_gateway_runtime, actor,
       is_duplicate = std::move (is_duplicate)] (
        actor_t &actor_instance,
        std::stop_token cancellation) mutable -> task_t<::zlink::framework::result_t<bool>> {
          // run_actor_turn owns the Actor instance until this task completes.
          return accept_session_binding_t::run (
            incoming_identity, std::move (replace_binding), race, completion,
            start_callback_timeout, &actor_instance, cancellation, has_duplicate_callback,
            has_session_bound_callback, failures, shutdown, spot_state, actor_gateway_runtime,
            actor, std::move (is_duplicate));
      },
      race->cancellation.get_token ());
    observe_task_completion (
      turn, [race, completion] (
              const ::zlink::framework::result_t<::zlink::framework::result_t<bool>> &finished) {
          (void) race->try_finish (1);
          race->cancellation.request_stop ();
          if (!finished) {
              completion->complete (::zlink::framework::result_t<bind_result_t>::failure (
                finished.error_kind (), finished.error () ? finished.error ()->what ()
                                                          : "Actor duplicate-bind turn failed"));
              return;
          }
          const auto &turn_result = finished.value ();
          if (!turn_result) {
              completion->complete (::zlink::framework::result_t<bind_result_t>::failure (
                turn_result.error_kind (), turn_result.error ()
                                             ? turn_result.error ()->what ()
                                             : "Actor duplicate-bind turn failed"));
              return;
          }
      });
    co_return co_await completion->task ();
}

void configure_app_actor_gateway (
  app_state_t *_state,
  zlink_framework_options_t &options,
  actor_gateway_runtime_t &actor_gateway_runtime,
  runtime::mesh_node_host_service_t *mesh_node_service,
  const std::shared_ptr<mesh_node_runtime_t> &application_mesh,
  std::vector<std::weak_ptr<mesh_node_runtime_t>> &weak_mesh_nodes,
  const stream_runtime_t &stream_runtime,
  const std::chrono::seconds request_timeout,
  const std::vector<std::shared_ptr<mesh_node_builder_state_t>>::iterator
    &application_mesh_registration)
{
    actor_gateway_runtime.on_join_entry_spot (
      [weak_application_mesh = std::weak_ptr (application_mesh),
       weak_actor_gateway_runtime = actor_gateway_runtime.weak_state (), mesh_node_service] (
        actor_ref_t actor, zlink::message_t request,
        std::chrono::milliseconds timeout) -> task_t<detail::actor_join_reply_t> {
          const auto application_mesh = detail::lock_runtime_owner (weak_application_mesh);
          const auto gateway = detail::actor_gateway_runtime_t (
            detail::lock_runtime_owner (weak_actor_gateway_runtime));
          const auto deadline = std::chrono::steady_clock::now () + timeout;
          const auto target =
            co_await runtime::await_result (mesh_node_service->select_actor_entry_spot (actor));
          if (!target)
              co_return detail::propagate_failure<detail::actor_join_reply_t> (
                target, "Entry Spot target selection failed");
          const auto bound_session = co_await gateway.bound_session_route_async (actor);
          const auto remaining = std::chrono::ceil<std::chrono::milliseconds> (
            deadline - std::chrono::steady_clock::now ());
          if (remaining <= std::chrono::milliseconds::zero ())
              co_return result_t<detail::actor_join_reply_t>::failure (
                framework_error_kind_t::deadline_exceeded,
                "Entry Spot join deadline elapsed during selection");
          co_return co_await application_mesh->join_application_actor_to_entry_spot (
            actor, target.value (), request, remaining,
            bound_session ? std::make_optional (bound_session->node_rid) : std::nullopt,
            bound_session ? bound_session->session_rid : std::nullopt);
      });
    actor_gateway_runtime.on_join_spot (
      [weak_application_mesh = std::weak_ptr (application_mesh),
       weak_actor_gateway_runtime = actor_gateway_runtime.weak_state (),
       spot_locations =
         [&] {
             auto provider = _state->services.build_provider ();
             return detail::service_scope_access_t::get_required_shared<
               runtime::spot_address_resolver_t> (provider);
         }()] (const actor_ref_t &actor, spot_id_t target_spot, const zlink::message_t &request,
               std::string packet_name, std::string content_type,
               std::chrono::milliseconds timeout) -> task_t<detail::actor_join_reply_t> {
          const auto resolver = spot_locations;
          const auto mesh = detail::lock_runtime_owner (weak_application_mesh);
          const auto gateway = detail::actor_gateway_runtime_t (
            detail::lock_runtime_owner (weak_actor_gateway_runtime));
          const auto bound_session = co_await gateway.bound_session_route_async (actor);
          co_return co_await mesh->join_application_actor_to_spot (
            actor, std::move (target_spot), resolver, request, timeout,
            bound_session ? std::make_optional (bound_session->node_rid) : std::nullopt,
            bound_session ? bound_session->session_rid : std::nullopt, std::move (packet_name),
            std::move (content_type));
      });
    actor_gateway_runtime.on_join_barrier (
      [weak_application_mesh = std::weak_ptr (application_mesh)] (const actor_ref_t &actor) {
          const auto application_mesh = detail::lock_runtime_owner (weak_application_mesh);
          return application_mesh->reserve_application_actor_join_barrier (actor);
      });
    actor_gateway_runtime.on_bound_session ([] (const actor_ref_t &) {
        // The STREAM native binder owns the exact physical Session RID
        // and binding generation, so it performs the remote bind.
        return result_t<void>::success ();
    });
    actor_gateway_runtime.on_bound_session_close (
      [weak_application_mesh = std::weak_ptr (application_mesh)] (
        actor_ref_t actor, detail::actor_bound_session_route_t route,
        detail::actor_gateway_state_t::bound_session_close_submit_t submit) -> task_t<void> {
          const auto application_mesh = detail::lock_runtime_owner (weak_application_mesh);
          // Session-Actor binding §4: command 54 carries the stored identity unchanged.
          if (route.session_owner_id.empty () || route.session_owner_lease_generation == 0)
              throw framework_exception_t (framework_error_kind_t::unavailable,
                                           "Bound Session route has no Session owner identity");
          co_return co_await application_mesh->native_node ()
            .transport ()
            .request_bound_session_close (
              runtime::protocol::bound_session_close_t{
                0,
                std::string (actor.actor_id ().value ()),
                actor.object_generation (),
                {route.node_rid.to_bytes (), route.node_generation, route.session_owner_id,
                 route.session_owner_lease_generation, route.session_rid->to_bytes (),
                 route.binding_generation}},
              application_mesh->default_request_timeout (),
              [application_mesh, submit = std::move (submit)] (
                std::function<task_t<bool> ()> work,
                std::function<void (result_t<void>)> complete) mutable {
                  return submit (
                    [application_mesh, work = std::move (work)] () mutable -> task_t<bool> {
                        co_return co_await work ();
                    },
                    std::move (complete));
              });
      });
    actor_gateway_runtime.on_bound_session_send (
      [weak_application_mesh = std::weak_ptr (application_mesh),
       weak_actor_gateway_runtime = actor_gateway_runtime.weak_state (),
       stream_runtime] (const actor_ref_t &actor, std::uint64_t expected_binding_generation,
                        const detail::stream_header_t &header, const zlink::message_t &payload) {
          const auto application_mesh = detail::lock_runtime_owner (weak_application_mesh);
          auto actor_gateway_runtime = detail::actor_gateway_runtime_t (
            detail::lock_runtime_owner (weak_actor_gateway_runtime));
          return send_bound_session_from_app_task (
            application_mesh, actor_gateway_runtime, stream_runtime, actor,
            expected_binding_generation, header, payload.copy ());
      });
    auto deliver_relay =
      [weak_application_mesh = std::weak_ptr (application_mesh)] (
        actor_ref_t actor, detail::stream_header_t header, zlink::message_t payload,
        std::optional<detail::bound_session_relay_source_t> bound_session_source,
        std::optional<runtime::stateful::stream_dispatch_t> dispatch,
        std::chrono::milliseconds request_timeout,
        std::shared_ptr<runtime::host::public_host_runtime_t> session_owner_runtime) mutable
      -> request_task_t<std::optional<zlink::message_t>> {
        const auto application_mesh = detail::lock_runtime_owner (weak_application_mesh);
        auto ingress = std::make_shared<detail::session_ingress_completion_t> ();
        auto routed_actor = actor;
        runtime::protocol::actor_route_fence_t stale_route;
        const auto reply_submission =
          bound_session_source ? bound_session_source->reply_submission : nullptr;
        if (bound_session_source) {
            auto &sessions = session_owner_runtime->sessions ();
            ingress->arm (sessions, *dispatch);
            routed_actor = detail::actor_ref_access_t::make (
              node_rid_t::from_string (dispatch->binding.actor.node_id),
              std::string (detail::actor_ref_access_t::actor_type (actor)),
              dispatch->binding.actor.key, dispatch->binding.actor.object_generation);
            stale_route = runtime::protocol::actor_route_fence_t{
              dispatch->binding.actor.key,
              dispatch->binding.actor.object_generation,
              zlink::routing_id_t::from (dispatch->binding.actor.node_id).to_bytes (),
              dispatch->binding.target_node_generation,
              dispatch->binding.actor.authority_owner_generation,
              dispatch->binding.owner_lease_generation};
            bound_session_source->session_sequence = dispatch->inbound_sequence;
        }
        // The native Session registry owns ingress completion and drain.
        // Receiver admission alone advances the Actor's relay ordinal.
        std::exception_ptr relay_failure;
        std::optional<zlink::message_t> relayed;
        try {
            if (header.kind () == detail::stream_message_kind_t::request
                && request_timeout <= std::chrono::milliseconds::zero ())
                throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                             "bound Session request original deadline elapsed");
            relayed =
              co_await detail::request_submission (application_mesh->relay_application_actor (
                routed_actor, header, payload, request_timeout, true,
                std::move (bound_session_source), std::move (stale_route)));
        }
        catch (...) {
            relay_failure = std::current_exception ();
        }
        if (relay_failure) {
            (void) ingress->complete ();
            std::rethrow_exception (relay_failure);
        }
        if (header.kind () == detail::stream_message_kind_t::request && relayed
            && reply_submission) {
            auto terminal = std::make_shared<task_t<void>> (reply_submission->terminal ());
            detail::observe_task_completion (
              *terminal, [ingress, terminal, session_owner_runtime] (const result_t<void> &) {
                  (void) terminal;
                  (void) session_owner_runtime;
                  (void) ingress->complete ();
              });
        } else if (ingress->complete () != runtime::stateful::stateful_error_t::none) {
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::internal_failure,
              "bound Session ingress completion lost its exact fence");
        }
        co_return relayed;
    };
    struct relay_bound_session_t
    {
        static request_task_t<std::optional<zlink::message_t>>
        run (std::decay_t<decltype (std::weak_ptr (application_mesh))> weak_application_mesh,
             std::decay_t<decltype (deliver_relay)> deliver_relay,
             std::decay_t<decltype (options.configure_dispatch ())> dispatch_options,
             std::decay_t<decltype (request_timeout)> request_timeout,
             actor_ref_t actor,
             detail::stream_header_t header,
             zlink::message_t payload,
             std::optional<detail::bound_session_relay_source_t> source,
             std::chrono::milliseconds timeout)
        {
            const auto application_mesh = detail::lock_runtime_owner (weak_application_mesh);
            using reply_t = std::optional<zlink::message_t>;
            const auto budget =
              timeout.count () > 0
                ? timeout
                : std::chrono::duration_cast<std::chrono::milliseconds> (request_timeout);
            const auto deadline = std::chrono::steady_clock::now () + budget;
            if (!source)
                co_return co_await detail::request_submission (
                  deliver_relay (actor, header, std::move (payload), source, {}, budget, {}));
            std::shared_ptr<task_completion_source_t<reply_t>> completion;
            std::optional<runtime::foundation::call_id_t> operation;
            auto session_owner_runtime = application_mesh->native_node ().shared_from_this ();
            auto &native = *session_owner_runtime;
            const bool request = header.kind () == detail::stream_message_kind_t::request;
            std::optional<runtime::mesh::raw_mesh_node_owner_t::local_operation_reservation_t>
              prepared_operation;
            if (request)
                prepared_operation = co_await native.transport ().prepare_local_operation_async ();
            auto retain = [&, prepared_operation = std::move (prepared_operation)] ()
              -> runtime::stateful::stream_relay_delivery_t {
                completion = std::make_shared<task_completion_source_t<reply_t>> ();
                operation =
                  request && prepared_operation
                      && native.transport ().register_prepared_local_operation (
                        *prepared_operation, deadline,
                        [completion] (runtime::foundation::operation_terminal_t terminal,
                                      result_t<zlink::message_t>) {
                            completion->complete (result_t<reply_t>::failure (
                              terminal == runtime::foundation::operation_terminal_t::timed_out
                                ? framework_error_kind_t::deadline_exceeded
                                : framework_error_kind_t::unavailable,
                              "bound Session relay request did not complete within its "
                              "original "
                              "operation"));
                        })
                    ? std::make_optional (prepared_operation->first)
                    : std::nullopt;

                if (request && !operation)
                    completion->complete (result_t<reply_t>::failure (
                      framework_error_kind_t::unavailable,
                      "bound Session request operation is unavailable"));
                auto drop = [header, source, actor,
                             dispatch_options] (message_flow_reason_t reason) {
                    detail::message_flow_tracer_t (dispatch_options)
                      .trace (message_flow_outcome_t::dropped, message_flow_result_t::dropped, [&] {
                          message_flow_event_t event{};
                          event.outcome = message_flow_outcome_t::dropped;
                          event.surface = dispatch_error_surface_t::stream_session;
                          event.message_kind = dispatch_message_kind_t::send;
                          event.packet_name = std::string (header.packet_name ());
                          event.actor_id = std::string (actor.actor_id ().value ());
                          event.stream_session_id = source->session_rid.to_hex ();
                          event.reason = reason;
                          if (header.correlation_id ())
                              event.correlation_id = std::string (*header.correlation_id ());
                          if (header.flow_id ())
                              event.flow_id = std::string (*header.flow_id ());
                          event.flow_origin = header.flow_origin ();
                          return event;
                      });
                };
                return [application_mesh, session_owner_runtime, deliver_relay, actor, header,
                        payload, source, deadline, budget, request, operation, completion, drop,
                        dispatch_options] (
                         std::optional<runtime::stateful::stream_dispatch_t> dispatch,
                         message_flow_reason_t reason,
                         std::optional<result_t<void>> failure) mutable -> task_t<void> {
                    if (failure) {
                        detail::dispatch_error_reporter_t (dispatch_options).report_lazy ([&] {
                            message_dispatch_error_event_t event{};
                            event.surface = dispatch_error_surface_t::stream_session;
                            event.message_kind = request ? dispatch_message_kind_t::request
                                                         : dispatch_message_kind_t::send;
                            event.reason = detail::dispatch_reason_from_error (failure->error ());
                            event.action = dispatch_error_action_t::drop;
                            event.packet_name = std::string (header.packet_name ());
                            event.actor_id = std::string (actor.actor_id ().value ());
                            event.source_rid = source->session_rid.to_hex ();
                            if (header.correlation_id ())
                                event.correlation_id = std::string (*header.correlation_id ());
                            if (header.flow_id ())
                                event.flow_id = std::string (*header.flow_id ());
                            event.flow_origin = header.flow_origin ();
                            if (failure->error ())
                                event.exception = std::make_exception_ptr (*failure->error ());
                            return event;
                        });
                        co_return;
                    }
                    auto &owner = *session_owner_runtime;
                    if (!dispatch) {
                        if (request && operation)
                            (void) owner.transport ().fail_local_operation (
                              *operation,
                              runtime::foundation::operation_terminal_t::route_unavailable);
                        else if (!request)
                            drop (reason);
                        else
                            completion->complete (result_t<reply_t>::failure (
                              framework_error_kind_t::unavailable,
                              "bound Session request operation is unavailable"));
                        co_return;
                    }
                    const auto remaining =
                      request
                          && (!operation
                              || !owner.transport ().unregister_local_operation (*operation))
                        ? std::chrono::milliseconds::zero ()
                        : std::chrono::duration_cast<std::chrono::milliseconds> (
                            deadline - std::chrono::steady_clock::now ());
                    auto execute = std::make_shared<std::remove_cvref_t<decltype (deliver_relay)>> (
                      deliver_relay);
                    auto delivery = std::make_shared<task_t<reply_t>> (
                      (*execute) (actor, header, payload, source, dispatch,
                                  request ? remaining : budget, session_owner_runtime));
                    detail::observe_task_completion (
                      *delivery, [execute, delivery, completion, request,
                                  drop] (const result_t<reply_t> &result) {
                          if (request)
                              completion->complete (result);
                          else {
                              if (!result)
                                  drop (detail::message_flow_reason_from_error (result.error ()));
                              completion->complete (result_t<reply_t>::success (std::nullopt));
                          }
                      });
                    if (!request)
                        (void) co_await completion->task ();
                    co_return;
                };
            };
            auto [admission, dispatch] = native.sessions ().admit_inbound (
              source->session_rid.to_hex (), source->binding_generation,
              std::string (actor.actor_id ().value ()), source->session_sequence,
              std::chrono::milliseconds{0}, std::move (retain));
            if (admission != runtime::stateful::stateful_error_t::none) {
                if (operation)
                    (void) native.transport ().unregister_local_operation (*operation);
                co_return result_t<reply_t>::failure (framework_error_kind_t::invalid_operation,
                                                      "bound Session ingress was not admitted");
            }
            co_await detail::request_admitted_t{};
            if (!dispatch) {
                if (request)
                    co_return co_await completion->task ();
                co_return std::nullopt;
            }
            co_return co_await detail::request_submission (
              deliver_relay (actor, header, std::move (payload), source, dispatch,
                             std::chrono::duration_cast<std::chrono::milliseconds> (
                               deadline - std::chrono::steady_clock::now ()),
                             session_owner_runtime));
        }
    };
    actor_gateway_runtime.on_relay (
      [weak_application_mesh = std::weak_ptr (application_mesh), deliver_relay,
       dispatch_options = options.configure_dispatch (), request_timeout] (
        const actor_ref_t &actor, actor_context_t, const detail::stream_header_t &header,
        const zlink::message_t &payload, std::optional<detail::bound_session_relay_source_t> source,
        std::chrono::milliseconds timeout) mutable
      -> request_task_t<std::optional<zlink::message_t>> {
          return relay_bound_session_t::run (weak_application_mesh, deliver_relay, dispatch_options,
                                             request_timeout, actor, header, payload,
                                             std::move (source), timeout);
      });
    actor_gateway_runtime.offload_session_relay ();
    actor_gateway_runtime.on_disconnect (
      [weak_mesh_nodes, request_timeout] (
        const actor_ref_t &actor,
        const std::optional<runtime::stateful::stream_binding_t> &binding) -> task_t<void> {
          if (!binding)
              co_return;
          const auto mesh = std::find_if (
            weak_mesh_nodes.begin (), weak_mesh_nodes.end (), [&] (const auto &candidate) {
                return detail::lock_runtime_owner (candidate)->mesh_name ()
                       == binding->actor.mesh_name;
            });
          if (mesh == weak_mesh_nodes.end ())
              throw framework_exception_t (framework_error_kind_t::not_found,
                                           "Actor disconnect RouteMesh was not found");
          const auto owner = detail::lock_runtime_owner (*mesh);
          co_await owner->notify_application_actor_disconnected (actor, *binding, request_timeout);
          co_return;
      });
    application_mesh->configure_bound_session_operations (runtime::host::bound_session_operations_t{
      [descriptors = _state->services.build_provider ().get<runtime::store_location_resolvers_t> (),
       mesh_name = application_mesh->mesh_name ()] (
        const zlink::routing_id_t &node_rid,
        std::uint64_t node_generation) -> task_t<std::optional<location_owner_token_t>> {
          // Session-Actor binding §4: the admitted peer descriptor of this
          // lifecycle that passed the Location runtime §4.1 owner lease check.
          if (!descriptors)
              co_return std::nullopt;
          auto live =
            co_await runtime::await_result (descriptors->get ().list_live_mesh_nodes (mesh_name));
          if (!live.has_value ())
              co_return std::nullopt;
          for (const auto &descriptor : live.value ()) {
              if (descriptor.rid == node_rid && descriptor.lifecycle_generation == node_generation)
                  co_return location_owner_token_t{descriptor.owner_id,
                                                   descriptor.lease_generation};
          }
          co_return std::nullopt;
      },
      [weak_actor_gateway_runtime = actor_gateway_runtime.weak_state (),
       weak_application_mesh = std::weak_ptr (application_mesh),
       spot_state = (*application_mesh_registration)->spot_state, stream_runtime,
       delays = &_state->delays,
       callback_timeout = options.session_replacement_callback_timeout ()] (
        const runtime::protocol::bound_session_bind_t &bind,
        const zlink::routing_id_t &session_owner, std::uint64_t session_owner_generation,
        const location_owner_token_t &session_owner_token,
        std::function<bool (std::uint32_t, std::uint32_t,
                            std::optional<runtime::protocol::stream_bind_reply_tail_t>)>
          submit_terminal_reply) mutable {
          const auto actor = detail::actor_ref_access_t::make (
            node_rid_t::from_string (
              zlink::routing_id_t::from (bind.actor.target_node_routing_id).to_string ()),
            {}, bind.actor.actor_id, bind.actor.object_generation);
          return detail::apply_bound_session_bind_async (
            weak_actor_gateway_runtime, weak_application_mesh, spot_state, stream_runtime, actor,
            bind, session_owner, session_owner_generation, session_owner_token,
            std::move (submit_terminal_reply), *delays, callback_timeout);
      },
      [weak_actor_gateway_runtime = actor_gateway_runtime.weak_state (),
       stream_runtime] (const runtime::protocol::bound_session_send_t &send,
                        std::vector<zlink::message_t> parts) mutable {
          auto actor_gateway_runtime = detail::actor_gateway_runtime_t (
            detail::lock_runtime_owner (weak_actor_gateway_runtime));
          try {
              if (actor_gateway_runtime.trace_bound_session_send_stage_enabled ()) {
                  actor_gateway_runtime.trace_bound_session_send_stage (
                    send.actor.actor_id, "session_node_receive",
                    [&] {
                        return "binding_generation="
                               + std::to_string (send.expected_binding_generation);
                    },
                    nullptr);
              }
              const auto actor = detail::actor_ref_access_t::make (
                node_rid_t::from_string (
                  zlink::routing_id_t::from (send.actor.target_node_routing_id).to_string ()),
                {}, send.actor.actor_id, send.actor.object_generation);
              auto [header, payload] = stream_runtime.decode_frame (parts);
              const auto dispatched = actor_gateway_runtime.dispatch_bound_session_send (
                actor, std::string (header.packet_name ()), header.codec (), payload);
              return dispatched ? runtime::stateful::stateful_error_t::none
                                : runtime::stateful::stateful_error_t::conflict;
          }
          catch (...) {
              return runtime::stateful::stateful_error_t::invalid;
          }
      },
      [weak_actor_gateway_runtime = actor_gateway_runtime.weak_state ()] (
        const runtime::protocol::bound_session_replaced_t &replacement) mutable {
          auto actor_gateway_runtime = detail::actor_gateway_runtime_t (
            detail::lock_runtime_owner (weak_actor_gateway_runtime));
          (void) actor_gateway_runtime.dispatch_bound_session_replaced (replacement);
      },
      [weak_actor_gateway_runtime = actor_gateway_runtime.weak_state ()] (
        const runtime::protocol::session_relocation_route_t &route,
        const runtime::stateful::stream_binding_t &previous,
        const runtime::stateful::stream_binding_t &target) mutable {
          auto actor_gateway_runtime = detail::actor_gateway_runtime_t (
            detail::lock_runtime_owner (weak_actor_gateway_runtime));
          return actor_gateway_runtime.commit_session_relocation_route (route, previous, target);
      },
      [weak_actor_gateway_runtime = actor_gateway_runtime.weak_state ()] (
        const runtime::protocol::session_relocation_route_t &route,
        std::uint64_t target_owner_lease_generation) mutable {
          auto actor_gateway_runtime = detail::actor_gateway_runtime_t (
            detail::lock_runtime_owner (weak_actor_gateway_runtime));
          return actor_gateway_runtime.prepare_session_relocation_target_route (
            route, target_owner_lease_generation);
      },
      [weak_actor_gateway_runtime = actor_gateway_runtime.weak_state (),
       stream_runtime] (const runtime::protocol::bound_session_send_t &send) mutable
      -> std::optional<runtime::host::bound_session_operations_t::delivery_capability_t> {
          auto actor_gateway_runtime = detail::actor_gateway_runtime_t (
            detail::lock_runtime_owner (weak_actor_gateway_runtime));
          if (actor_gateway_runtime.trace_bound_session_send_stage_enabled ()) {
              actor_gateway_runtime.trace_bound_session_send_stage (
                send.actor.actor_id, "session_node_receive",
                [&] {
                    return "binding_generation="
                           + std::to_string (send.expected_binding_generation);
                },
                nullptr);
          }
          const auto actor = detail::actor_ref_access_t::make (
            node_rid_t::from_string (
              zlink::routing_id_t::from (send.actor.target_node_routing_id).to_string ()),
            {}, send.actor.actor_id, send.actor.object_generation);
          auto admitted = actor_gateway_runtime.admit_bound_session_delivery (
            actor, send.expected_binding_generation);
          if (!admitted)
              return std::nullopt;
          return [admitted = std::move (*admitted),
                  stream_runtime] (std::vector<zlink::message_t> parts) mutable {
              try {
                  auto [header, payload] = stream_runtime.decode_frame (parts);
                  auto dispatched =
                    admitted (std::string (header.packet_name ()), header.codec (), payload);
                  return detail::project_task_terminal<runtime::stateful::stateful_error_t> (
                    dispatched, [] (const result_t<void> &terminal) {
                        return result_t<runtime::stateful::stateful_error_t>::success (
                          terminal ? runtime::stateful::stateful_error_t::none
                                   : runtime::stateful::stateful_error_t::conflict);
                    });
              }
              catch (...) {
                  return task_t<runtime::stateful::stateful_error_t> (
                    result_t<runtime::stateful::stateful_error_t>::success (
                      runtime::stateful::stateful_error_t::invalid));
              }
          };
      }});
    application_mesh->configure_late_session_route_update (
      [dispatch = options.dispatch_options ()] (
        const runtime::protocol::session_relocation_route_t &route) mutable {
          const framework_exception_t warning (framework_error_kind_t::invalid_operation,
                                               "late_session_route_update");
          detail::dispatch_error_reporter_t (dispatch).report_lazy ([&] {
              return message_dispatch_error_event_t{
                .surface = dispatch_error_surface_t::stream_session,
                .message_kind = dispatch_message_kind_t::actor_send,
                .reason = dispatch_error_reason_t::invalid_frame,
                .action = dispatch_error_action_t::drop,
                .packet_name = "sessionRelocationRoute",
                .actor_id = route.actor.actor_id,
                .exception = std::make_exception_ptr (warning)};
          });
      });
}

} // namespace zlink::framework::detail
