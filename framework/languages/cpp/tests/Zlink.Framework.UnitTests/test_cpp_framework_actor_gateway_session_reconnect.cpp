/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/host/bound_session_bind.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include "runtime/streams/stream_runtime.hpp"

namespace zlink::framework::runtime
{
struct state_lane_test_access_t
{
    static bool has_pending (state_lane_t &lane)
    {
        const std::lock_guard lock (lane._mailbox_mutex);
        return !lane._mailbox.empty ();
    }
};
}

namespace zlink::framework::test::actor_gateway
{
int rebound_session_keeps_prior_ingress_exact_fence ()
{
    using namespace zlink::framework::runtime::stateful;

    const object_ref_t actor{object_kind_t::actor, "reconnect-player", 1, 7, "player", "node-a"};
    stream_session_registry_t sessions (
      [actor] (const std::string &actor_id) -> std::optional<object_ref_t> {
          return actor_id == actor.key ? std::make_optional (actor) : std::nullopt;
      });
    const auto connection = sessions.open ("reconnect-session");
    const auto [first_error, first] = sessions.bind (connection, actor, 11, 13);
    if (first_error != stateful_error_t::none)
        return 1;
    const auto [admit_error, admitted] = sessions.admit_inbound (first);
    if (admit_error != stateful_error_t::none || !admitted)
        return 2;

    const auto [rebind_error, rebound] = sessions.bind (connection, actor, 11, 13);
    if (rebind_error != stateful_error_t::none
        || rebound.binding_generation == first.binding_generation) {
        return 3;
    }
    const auto seal =
      sessions.seal_remote_route (connection.connection_id, rebound.binding_generation, actor.key,
                                  actor.object_generation, rebound.owner_lease_generation);
    if (seal.error != stateful_error_t::backpressured
        || sessions.remote_route_seal_ready (seal.barrier)) {
        return 4;
    }
    if (sessions.complete_inbound (*admitted) != stateful_error_t::none
        || !sessions.remote_route_seal_ready (seal.barrier)) {
        return 5;
    }
    return sessions.abort_barrier (seal.barrier) == stateful_error_t::none ? 0 : 6;
}

int reconnect_push_reaches_new_session_across_two_hosts ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto actor_owner_state = std::make_shared<actor_gateway_state_t> ();
    auto session_owner_state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    actor_owner_state->serializers = &serializers;
    session_owner_state->serializers = &serializers;
    actor_gateway_runtime_t actor_owner (actor_owner_state);
    actor_gateway_runtime_t session_owner (session_owner_state);
    const auto actor = test_actor_ref ("actor-owner", "player", "two-host-reconnect", 7);
    const auto session_owner_rid = zlink::routing_id_t::from ("session-owner");
    const zlink::framework::runtime::stateful::object_ref_t session_actor{
      zlink::framework::runtime::stateful::object_kind_t::actor,
      "two-host-reconnect",
      7,
      13,
      "player",
      "actor-owner"};
    zlink::framework::runtime::stateful::stream_session_registry_t stream_sessions (
      [session_actor] (const std::string &actor_id)
        -> std::optional<zlink::framework::runtime::stateful::object_ref_t> {
          return actor_id == session_actor.key ? std::make_optional (session_actor) : std::nullopt;
      });
    const auto old_connection = stream_sessions.open ("same-session-rid");
    const auto [old_bind_error, old_binding] =
      stream_sessions.bind_remote (old_connection, session_actor, 11, 17);
    const auto new_connection = stream_sessions.open ("same-session-rid");
    const auto [new_bind_error, new_binding] =
      stream_sessions.bind_remote (new_connection, session_actor, 11, 17, {});
    if (old_bind_error != zlink::framework::runtime::stateful::stateful_error_t::none
        || new_bind_error != zlink::framework::runtime::stateful::stateful_error_t::none
        || new_binding.binding_generation <= old_binding.binding_generation) {
        return 1;
    }
    const actor_bound_session_route_t old_route{session_owner_rid,
                                                zlink::routing_id_t::from ("same-session-rid"),
                                                7,
                                                11,
                                                13,
                                                17,
                                                old_binding.binding_generation,
                                                0,
                                                0};
    const actor_bound_session_route_t new_route{session_owner_rid,
                                                zlink::routing_id_t::from ("same-session-rid"),
                                                7,
                                                11,
                                                13,
                                                17,
                                                new_binding.binding_generation,
                                                0,
                                                0};

    // The sinks can run after this scope ends, so they own their counters and gateway handles.
    auto old_client_received = std::make_shared<std::atomic_int> (0);
    auto new_client_received = std::make_shared<std::atomic_int> (0);
    if (!session_owner.replace_session_route (
          actor,
          [old_client_received] (std::string, stream_codec_t, const zlink::message_t &) {
              old_client_received->fetch_add (1, std::memory_order_acq_rel);
              return task_t<void> (result_t<void>::success ());
          },
          old_route)) {
        return 2;
    }

    // The gateway state outlives this scope, so the observer owns what it reads
    // and writes.
    auto saw_correlated_split = std::make_shared<std::atomic_bool> (false);
    const auto expected_current = "current=session_rid=" + new_route.session_rid->to_hex ()
                                  + "/bg=" + std::to_string (new_binding.binding_generation);
    const auto expected_staged = "staged=session_rid=" + old_route.session_rid->to_hex ()
                                 + "/bg=" + std::to_string (old_binding.binding_generation);
    dispatch_options_t trace_options;
    trace_options.message_flow (message_flow_log_mode_t::detailed);
    dispatch_options_access_t::set_observer_for_tests (
      trace_options, [saw_correlated_split, expected_current,
                      expected_staged] (const message_flow_event_t &event) {
          if (event.outcome == message_flow_outcome_t::admitted
              && event.surface == dispatch_error_surface_t::stream_session
              && event.message_kind == dispatch_message_kind_t::send
              && event.detail_stage == std::optional<std::string> ("actor_owner_push_target")
              && event.detail_result
              && event.detail_result->find (expected_current) != std::string::npos
              && event.detail_result->find (expected_staged) != std::string::npos) {
              saw_correlated_split->store (true, std::memory_order_release);
          }
      });
    actor_owner.set_dispatch (std::move (trace_options));

    // The sink lives in the actor owner's state, so it refers back to that
    // state weakly instead of holding it.
    const std::weak_ptr<actor_gateway_state_t> weak_actor_owner = actor_owner_state;
    const auto make_remote_sink = [weak_actor_owner, session_owner,
                                   actor] (actor_bound_session_route_t staged_route) {
        return [weak_actor_owner, session_owner, actor, staged_route = std::move (staged_route)] (
                 std::string packet_name, stream_codec_t codec,
                 const zlink::message_t &payload) mutable -> task_t<void> {
            const auto owner_state = weak_actor_owner.lock ();
            if (!owner_state) {
                throw framework_exception_t (framework_error_kind_t::not_configured,
                                             "actor owner is gone");
            }
            const auto current = actor_gateway_runtime_t (owner_state)
                                   .resolve_bound_session_push_route (actor, staged_route);
            if (!current) {
                throw framework_exception_t (framework_error_kind_t::not_configured,
                                             "current reconnect route is unavailable");
            }
            auto delivery =
              session_owner.admit_bound_session_delivery (actor, current->binding_generation);
            if (!delivery) {
                throw framework_exception_t (framework_error_kind_t::not_configured,
                                             "new Session host rejected the push");
            }
            co_await (*delivery) (std::move (packet_name), codec, payload);
            co_return;
        };
    };

    if (!actor_owner.replace_session_route (actor, make_remote_sink (old_route), old_route)) {
        return 3;
    }
    const auto staged_old_sink = actor_owner_state->sync (
      [&] { return actor_owner_state->bound_session_sinks.at ("two-host-reconnect"); });

    if (!session_owner.replace_session_route (
          actor,
          [new_client_received] (std::string, stream_codec_t, const zlink::message_t &) {
              new_client_received->fetch_add (1, std::memory_order_acq_rel);
              return task_t<void> (result_t<void>::success ());
          },
          new_route)
        || !actor_owner.replace_session_route (actor, make_remote_sink (new_route), new_route)) {
        return 4;
    }
    const auto stale_capability_push =
      (*staged_old_sink) ("reconnected-push", stream_codec_t::message_pack,
                          zlink::message_t::from ("payload"))
        .result ();
    if (!stale_capability_push)
        return 6;

    const auto public_push = actor_owner.actor_context (actor)
                               .bound_session ()
                               .send (std::string ("public-push"))
                               .async ()
                               .result ();
    if (!public_push)
        return 7;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while ((new_client_received->load (std::memory_order_acquire) != 2
            || !saw_correlated_split->load (std::memory_order_acquire))
           && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::yield ();
    }
    return old_client_received->load (std::memory_order_acquire) == 0
               && new_client_received->load (std::memory_order_acquire) == 2
               && saw_correlated_split->load (std::memory_order_acquire)
             ? 0
             : 8;
}

int old_stream_disconnect_does_not_retire_reconnected_binding ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using namespace zlink::framework::runtime::stateful;

    const object_ref_t native_actor{
      object_kind_t::actor, "disconnect-fenced-actor", 7, 13, "player", "actor-owner"};
    stream_session_registry_t sessions (
      [native_actor] (const std::string &actor_id) -> std::optional<object_ref_t> {
          return actor_id == native_actor.key ? std::make_optional (native_actor) : std::nullopt;
      });
    const auto old_connection = sessions.open ("old-stream-rid");
    const auto [old_error, old_binding] =
      sessions.bind_remote (old_connection, native_actor, 11, 17);
    const auto disconnect_snapshot = sessions.bindings (old_connection);
    const auto new_connection = sessions.open ("new-stream-rid");
    const auto [new_error, new_binding] =
      sessions.bind_remote (new_connection, native_actor, 11, 17);
    const auto current_binding = sessions.current_binding (native_actor.key);
    if (old_error != stateful_error_t::none || new_error != stateful_error_t::none
        || disconnect_snapshot.size () != 1 || !sessions.bindings (old_connection).empty ()
        || sessions.bindings (new_connection).size () != 1 || !current_binding
        || *current_binding != new_binding || current_binding->connection != new_connection
        || current_binding->connection == old_connection
        || new_binding.binding_generation <= old_binding.binding_generation) {
        return 1;
    }

    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref ("actor-owner", "player", native_actor.key, 7);
    const auto session_owner = zlink::routing_id_t::from ("session-owner");
    const auto old_rid = zlink::routing_id_t::from ("old-stream-rid");
    const auto new_rid = zlink::routing_id_t::from ("new-stream-rid");
    std::atomic_int new_stream_received{0};
    if (!gateway.replace_session_route (
          actor,
          [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (result_t<void>::success ());
          },
          actor_bound_session_route_t{session_owner, old_rid, 7, 11, 13, 17,
                                      old_binding.binding_generation, 0, 0})
        || !gateway.replace_session_route (
          actor,
          [&new_stream_received] (std::string, stream_codec_t, const zlink::message_t &) {
              ++new_stream_received;
              return task_t<void> (result_t<void>::success ());
          },
          actor_bound_session_route_t{session_owner, new_rid, 7, 11, 13, 17,
                                      new_binding.binding_generation, 0, 0})) {
        return 2;
    }

    for (const auto &retiring : disconnect_snapshot) {
        if (sessions.is_current (retiring)) {
            (void) gateway.retire_bound_session_route (actor, session_owner, old_rid,
                                                       retiring.binding_generation);
        }
    }
    if (gateway.retire_bound_session_route (actor, session_owner, old_rid,
                                            old_binding.binding_generation)) {
        return 3;
    }
    const auto current = sessions.current_binding (native_actor.key);
    const auto route = gateway.bound_session_route_async (actor).result ().value ();
    const auto delivered = gateway.dispatch_bound_session_send (
      actor, "after-reconnect", stream_codec_t::message_pack, zlink::message_t::from ("payload"));
    return current && *current == new_binding && route && route->session_rid == new_rid
               && route->binding_generation == new_binding.binding_generation && delivered
               && new_stream_received.load () == 1
             ? 0
             : 4;
}

int command_38_rebind_is_owned_only_by_new_connection ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using namespace zlink::framework::runtime;

    const stateful::object_ref_t native_actor{
      stateful::object_kind_t::actor, "command-38-rebind-actor", 7, 13, "player", "actor-owner"};
    stateful::stream_session_registry_t sessions (
      [native_actor] (const std::string &actor_id) -> std::optional<stateful::object_ref_t> {
          return actor_id == native_actor.key ? std::make_optional (native_actor) : std::nullopt;
      });
    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref ("actor-owner", "player", native_actor.key, 7);
    const auto session_owner = zlink::routing_id_t::from ("session-owner");

    const auto receive_command_38 = [&] (const protocol::bound_session_bind_t &outbound) {
        const auto received =
          protocol::decode_bound_session_bind (protocol::encode_bound_session_bind (outbound));
        const auto session_rid = zlink::routing_id_t::from (received.session_routing_id);
        return gateway.record_bound_session_route (
          actor, session_owner, session_rid, 11, received.actor.authority_owner_generation,
          received.actor.owner_lease_generation, received.binding.generation, 0, 0);
    };
    const auto bind_through_native_and_38 =
      [&] (const stateful::stream_connection_t &connection, const zlink::routing_id_t &session_rid,
           std::uint64_t binding_generation) -> std::optional<stateful::stream_binding_t> {
        auto [error, binding] =
          sessions.bind_remote (connection, native_actor, 11, 17, {}, binding_generation);
        if (error != stateful::stateful_error_t::none)
            return std::nullopt;
        const protocol::bound_session_bind_t command_38{
          binding.binding_generation,
          protocol::actor_route_fence_t{
            native_actor.key, native_actor.object_generation,
            zlink::routing_id_t::from (native_actor.node_id).to_bytes (), 11,
            native_actor.authority_owner_generation, 17},
          session_rid.to_bytes (),
          {protocol::bound_session_binding_state_t::active, binding.binding_generation}};
        if (!receive_command_38 (command_38))
            return std::nullopt;
        return binding;
    };

    const auto old_connection = sessions.open ("command-38-old");
    const auto old_binding =
      bind_through_native_and_38 (old_connection, zlink::routing_id_t::from ("session-old"), 1);
    const auto new_connection = sessions.open ("command-38-new");
    const auto new_binding =
      bind_through_native_and_38 (new_connection, zlink::routing_id_t::from ("session-new"), 2);
    if (!old_binding || !new_binding)
        return 1;
    const auto current_binding = sessions.current_binding (native_actor.key);
    const auto route = gateway.bound_session_route_async (actor).result ().value ();
    return sessions.bindings (old_connection).empty ()
               && sessions.bindings (new_connection).size () == 1 && current_binding
               && *current_binding == *new_binding && current_binding->connection == new_connection
               && current_binding->connection != old_connection && route
               && route->session_rid == zlink::routing_id_t::from ("session-new")
               && route->binding_generation == 2
             ? 0
             : 2;
}

int late_lower_generation_bind_and_record_are_rejected ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using namespace zlink::framework::runtime::stateful;

    const object_ref_t native_actor{
      object_kind_t::actor, "monotonic-binding-actor", 7, 13, "player", "actor-owner"};
    stream_session_registry_t sessions (
      [native_actor] (const std::string &actor_id) -> std::optional<object_ref_t> {
          return actor_id == native_actor.key ? std::make_optional (native_actor) : std::nullopt;
      });
    const auto current_connection = sessions.open ("current-stream-rid");
    const auto [current_error, current_binding] =
      sessions.bind_remote (current_connection, native_actor, 11, 17, {}, 22);
    const auto stale_connection = sessions.open ("stale-stream-rid");
    const auto [stale_error, stale_binding] =
      sessions.bind_remote (stale_connection, native_actor, 11, 17, {}, 21);
    const auto registry_current = sessions.current_binding (native_actor.key);
    if (current_error != stateful_error_t::none || stale_error != stateful_error_t::conflict
        || stale_binding.binding_generation != 0 || !registry_current
        || *registry_current != current_binding) {
        return 1;
    }

    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref ("actor-owner", "player", native_actor.key, 7);
    const auto session_owner = zlink::routing_id_t::from ("session-owner");
    const auto current_rid = zlink::routing_id_t::from ("current-stream-rid");
    const auto stale_rid = zlink::routing_id_t::from ("stale-stream-rid");
    std::atomic_int current_stream_received{0};
    std::atomic_int stale_stream_received{0};
    if (!gateway.replace_session_route (
          actor,
          [&current_stream_received] (std::string, stream_codec_t, const zlink::message_t &) {
              ++current_stream_received;
              return task_t<void> (result_t<void>::success ());
          },
          actor_bound_session_route_t{session_owner, current_rid, 7, 11, 13, 17, 22, 0, 0})) {
        return 2;
    }
    const auto stale_publish = gateway.replace_session_route (
      actor,
      [&stale_stream_received] (std::string, stream_codec_t, const zlink::message_t &) {
          ++stale_stream_received;
          return task_t<void> (result_t<void>::success ());
      },
      actor_bound_session_route_t{session_owner, stale_rid, 7, 11, 13, 17, 21, 0, 0});
    const auto stale_record = gateway.record_bound_session_route_transition (
      actor, actor_bound_session_route_t{session_owner, stale_rid, 7, 11, 13, 17, 20, 0, 0});
    const auto route = gateway.bound_session_route_async (actor).result ().value ();
    const auto delivered = gateway.dispatch_bound_session_send (actor, "after-stale-publish",
                                                                stream_codec_t::message_pack,
                                                                zlink::message_t::from ("payload"));
    return !stale_publish && stale_publish.error_kind () == framework_error_kind_t::unavailable
               && !stale_record && stale_record.error_kind () == framework_error_kind_t::unavailable
               && route && route->session_rid == current_rid && route->binding_generation == 22
               && delivered && current_stream_received.load () == 1
               && stale_stream_received.load () == 0
             ? 0
             : 3;
}

int same_identity_registration_keeps_logical_binding_current ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using namespace zlink::framework::runtime::stateful;

    const object_ref_t native_actor{object_kind_t::actor, "defensive-rebind", 7, 13, "player",
                                    "actor-owner"};
    stream_session_registry_t sessions (
      [native_actor] (const std::string &actor_id) -> std::optional<object_ref_t> {
          return actor_id == native_actor.key ? std::make_optional (native_actor) : std::nullopt;
      });
    const auto old_connection = sessions.open ("reused-session-rid");
    const auto [old_error, old_binding] =
      sessions.bind_remote (old_connection, native_actor, 11, 17, {}, 41);
    const auto new_connection = sessions.open ("reused-session-rid");
    const auto [new_error, new_binding] =
      sessions.bind_remote (new_connection, native_actor, 11, 17, {}, 41);
    const auto current = sessions.current_binding (native_actor.key);
    if (old_error != stateful_error_t::none || new_error != stateful_error_t::none || !current
        || current->connection != new_connection || current->binding_generation != 41)
        return 1;

    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref ("actor-owner", "player", "defensive-rebind", 7);
    const auto owner_rid = zlink::routing_id_t::from ("session-owner");
    const auto session_rid = zlink::routing_id_t::from ("reused-session-rid");
    std::atomic_int old_stream_received{0};
    std::atomic_int new_stream_received{0};
    const actor_bound_session_route_t old_route{owner_rid, session_rid, 7, 11, 13, 17, 41, 1, 0};
    const actor_bound_session_route_t new_route{owner_rid, session_rid, 7, 11, 13, 17, 41, 2, 0};
    if (!gateway.replace_session_route (
          actor,
          [&old_stream_received] (std::string, stream_codec_t, const zlink::message_t &) {
              ++old_stream_received;
              return task_t<void> (result_t<void>::success ());
          },
          old_route))
        return 2;
    const auto replaced = gateway.replace_session_route (
      actor,
      [&new_stream_received] (std::string, stream_codec_t, const zlink::message_t &) {
          ++new_stream_received;
          return task_t<void> (result_t<void>::success ());
      },
      new_route);
    // Session-Actor binding §5: a local delivery token is not another binding
    // identity. Refreshing its sink does not replace the logical binding.
    if (!replaced || replaced.value ().changed || replaced.value ().previous
        || !replaced.value ().current
        || replaced.value ().current->binding_token != old_route.binding_token)
        return 3;
    const auto delivered = gateway.dispatch_bound_session_send (
      actor, "defensive-push", stream_codec_t::message_pack, zlink::message_t::from ("payload"));
    return delivered && old_stream_received.load () == 0 && new_stream_received.load () == 1 ? 0
                                                                                             : 4;
}

int same_rid_registration_retains_retired_close_handler ()
{
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    const auto session_rid = zlink::routing_id_t::from ("reused-handler-rid");
    std::atomic_int retired_calls{0};
    std::atomic_int current_calls{0};
    auto retired = gateway.register_bound_session_replacement_handler (
      session_rid, [&retired_calls] (const auto &) {
          ++retired_calls;
          return true;
      });
    auto current = gateway.register_bound_session_replacement_handler (
      session_rid, [&current_calls] (const auto &) {
          ++current_calls;
          return true;
      });
    zlink::framework::runtime::protocol::bound_session_replaced_t replacement;
    replacement.retired_session.session_routing_id = session_rid.to_bytes ();
    if (!gateway.dispatch_bound_session_replaced (replacement) || retired_calls.load () != 1
        || current_calls.load () != 0)
        return 1;
    gateway.unregister_bound_session_replacement_handler (session_rid, retired);
    if (!gateway.dispatch_bound_session_replaced (replacement) || retired_calls.load () != 1
        || current_calls.load () != 1)
        return 2;
    gateway.unregister_bound_session_replacement_handler (session_rid, current);
    return !gateway.dispatch_bound_session_replaced (replacement) ? 0 : 3;
}

int binding_context_uses_gateway_state_lane ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ().share ();
    auto holder = std::async (std::launch::async, [&] {
        state->sync ([&] {
            entered.set_value ();
            released.wait ();
        });
    });
    entered.get_future ().wait ();
    std::atomic_bool returned{false};
    auto configure = std::async (std::launch::async, [&] {
        session_actor_manager_access_t::bind_native (manager, [] (actor_ref_t, std::uint64_t) {
            return task_t<void> (result_t<void>::success ());
        });
        returned.store (true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    bool queued = false;
    while (!(queued = runtime::state_lane_test_access_t::has_pending (state->lane))
           && !returned.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    const bool completed_while_held = returned.load (std::memory_order_acquire);
    release.set_value ();
    holder.get ();
    configure.get ();
    return queued && !completed_while_held ? 0 : 1;
}
} // namespace zlink::framework::test::actor_gateway
