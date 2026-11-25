/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/execution/actor_execution_context.hpp"
#include "runtime/execution/serial_handler_participant.hpp"
#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/session_disconnect_probe_spot.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <atomic>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
int session_disconnect_callback_owns_spot_lifecycle_gate ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace rt = zlink::framework::runtime;
    for (const bool entry : {false, true}) {
        serializer_registry_t serializers;
        auto node =
          test::runtime_failure_fixture<spot_node_builder_state_t> ("disconnect-gate-node");
        node->worker_executor = std::make_shared<rt::offload_executor_t> (1, "disconnect-gate");
        auto spot = std::make_shared<spot_context_state_t> ();
        spot->node = node;
        spot->node_rid = node_rid_t::from_string ("disconnect-gate-node");
        spot->spot_id = spot_id_t ("disconnect-gate-spot");
        spot->spot_name = "disconnect-gate";
        spot->serial_executor = node->worker_executor;
        spot->execution_mode =
          entry ? user_spot_execution_mode_t::spot_wide : user_spot_execution_mode_t::per_actor;
        spot->lifecycle_domain =
          entry ? spot_lifecycle_domain_t::entry () : spot_lifecycle_domain_t::user ();
        spot->serial_queue = std::make_shared<rt::serial_execution_queue_t> (
          *node->worker_executor, rt::serial_execution_queue_options_t{},
          rt::serial_execution_queue_t::error_handler_t{},
          entry ? rt::serial_lane_policy_t::entry_spot ()
                : rt::serial_lane_policy_t::per_actor_spot ());
        node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));
        node->spot_ids_by_name.emplace (spot->spot_name, spot->spot_id);
        if (entry)
            node->snapshot.entry_spot_name = spot->spot_name;
        std::atomic_int callbacks{0};
        std::atomic_bool owned_gate{false};
        std::atomic_bool owned_actor_context{false};
        spot_actor_admission_callbacks_t admission;
        admission.on_disconnect_actor = [&] (void *, void *) -> task_t<void> {
            const auto turn = capture_current_serial_turn ();
            owned_gate.store (turn && turn->belongs_to (spot->serial_queue.get ()),
                              std::memory_order_release);
            owned_actor_context.store (rt::current_actor_execution.actor_key == "player:gate-player"
                                         && rt::current_actor_execution.spot_id
                                              == std::string (spot->spot_id),
                                       std::memory_order_release);
            callbacks.fetch_add (1, std::memory_order_acq_rel);
            co_return;
        };
        test::register_session_disconnect_probe (spot, std::move (admission.on_disconnect_actor));
        spot_node_builder_state_t::actor_factory_registration_t factory;
        factory.actor_type = std::type_index (typeid (int));
        factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
        factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
        node->actor_factories.emplace ("player", std::move (factory));
        const auto actor = test_actor_ref ("disconnect-gate-node", "player", "gate-player", 1);
        node->actor_instances.emplace ("player:gate-player", std::make_shared<int> (7));
        node->actor_spot_ids.emplace ("player:gate-player", spot->spot_id);
        node->actor_generations.emplace ("player:gate-player", 1);
        record_actor_instance_index_unlocked (
          *node, actor, node->actor_instances.at ("player:gate-player").get ());
        service_collection_t services;
        services.add_singleton<actor_gateway_runtime_t> ();
        auto provider = services.build_provider ();
        auto &gateway = provider.get_required<actor_gateway_runtime_t> ();
        auto release = std::make_shared<task_completion_source_t<void>> ();
        const auto coordinator = spot->ensure_spot_serial_executor ();
        if (!coordinator || coordinator->uses_spot_execution_gate ())
            return 2;
        std::promise<bool> holding;
        auto hold_signalled = std::make_shared<std::atomic_bool> (false);
        auto held_started = holding.get_future ();
        auto held =
          spot->run_serial_task ("hold-disconnect-spot-lifecycle", [&] () -> task_t<void> {
              if (!hold_signalled->exchange (true))
                  holding.set_value (true);
              co_await release->task ();
          });
        // The observer may run after held.result () returns and this scope
        // ends, so it owns the flag and touches the promise only before then.
        observe_task_completion (held, [&holding, hold_signalled] (const auto &) {
            if (!hold_signalled->exchange (true))
                holding.set_value (false);
        });
        if (!held_started.get ()) {
            release->complete (result_t<void>::success ());
            (void) held.result ();
            return 2;
        }
        std::promise<bool> admitted;
        auto accepted = admitted.get_future ();
        auto signalled = std::make_shared<std::atomic_bool> (false);
        auto notification = spot_node_runtime_t (node).relay_actor_packet (
          actor, gateway.actor_context (actor), stream_message_kind_t::request,
          rt::protocol::session_disconnected_packet_name,
          zlink::message_t::from (rt::protocol::encode_session_disconnected (
            {zlink::routing_id_t::from ("session-owner").to_bytes (), 2, 7})),
          provider, serializers, {}, nullptr, {}, [&] {
              if (!signalled->exchange (true))
                  admitted.set_value (true);
          });
        observe_task_completion (notification, [&admitted, signalled] (const auto &) {
            if (!signalled->exchange (true))
                admitted.set_value (false);
        });
        const bool entered_actor = accepted.get ();
        const bool ran_while_held =
          callbacks.load (std::memory_order_acquire) != 0 || notification.await_ready ();
        release->complete (result_t<void>::success ());
        const auto held_result = held.result ();
        const auto notified = notification.result ();
        const bool owns_gate = owned_gate.load (std::memory_order_acquire);
        const bool owns_actor_context = owned_actor_context.load (std::memory_order_acquire);
        std::cerr << "Session disconnect lifecycle gate entry=" << entry
                  << " admitted=" << entered_actor << " ran_while_held=" << ran_while_held
                  << " owns_spot_turn=" << owns_gate << " owns_actor_context=" << owns_actor_context
                  << std::endl;
        spot->serial_queue->drain ();
        node->worker_executor->drain ();
        if (!held_result || !notified || !notified.value () || !notified.value ()->is_empty ()
            || !entered_actor || ran_while_held || !owns_gate || !owns_actor_context
            || callbacks != 1)
            return 1;
        spot_node_runtime_t (node).release_native_handles ();
    }
    return 0;
}

int session_disconnect_resolves_context_without_ingress_metadata (bool replay)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace rt = zlink::framework::runtime;
    serializer_registry_t serializers;
    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    auto &gateway = provider.get_required<actor_gateway_runtime_t> ();
    auto node =
      test::runtime_failure_fixture<spot_node_builder_state_t> ("disconnect-context-node");
    node->worker_executor = std::make_shared<rt::offload_executor_t> (1, "disconnect-context");
    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("disconnect-context-node");
    spot->spot_id = spot_id_t ("disconnect-context-spot");
    spot->spot_name = "disconnect-context";
    spot->serial_executor = node->worker_executor;
    spot->serial_queue = spot->ensure_spot_serial_executor ()->spot_queue ();
    spot->channel_runtime = test::runtime_failure_fixture<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    auto release = std::make_shared<task_completion_source_t<void>> ();
    auto entered = std::make_shared<std::promise<bool>> ();
    auto callback_started = entered->get_future ();
    auto entered_once = std::make_shared<std::atomic_bool> (false);
    auto callbacks = std::make_shared<std::atomic_int> (0);
    test::register_session_disconnect_probe (
      spot, [callbacks, entered_once, entered, release] (void *, void *) -> task_t<void> {
          callbacks->fetch_add (1, std::memory_order_acq_rel);
          if (!entered_once->exchange (true))
              entered->set_value (true);
          co_await release->task ();
      });
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));
    const auto actor = test_actor_ref ("disconnect-context-node", "player", "context-player", 1);
    const auto instance = std::make_shared<int> (7);
    node->actor_instances.emplace ("player:context-player", instance);
    node->actor_spot_ids.emplace ("player:context-player", spot->spot_id);
    node->actor_generations.emplace ("player:context-player", 1);
    node->actor_types_by_id.emplace ("context-player", "player");
    record_actor_instance_index_unlocked (*node, actor, instance.get ());
    (void) gateway.actor_context (actor);
    if (!gateway.record_bound_session_route (actor, zlink::routing_id_t::from ("session-owner"),
                                             zlink::routing_id_t::from ("session"), 2, 1, 1, 7))
        return 1;
    const auto body = zlink::message_t::from (rt::protocol::encode_session_disconnected (
      {zlink::routing_id_t::from ("session-owner").to_bytes (), 2, 7}));
    spot_node_runtime_t spots (node);
    spots.bind_service_provider (provider);
    if (!replay) {
        release->complete (result_t<void>::success ());
        auto typed_spot =
          std::static_pointer_cast<test::session_disconnect_probe_spot_t> (spot->spot_instance);
        std::cerr << "Session disconnect public invocation without ingress metadata starts"
                  << std::endl;
        const auto delivered =
          spot_context_access_t::create (spot).handlers ().invoke_actor_packet (
            rt::protocol::session_disconnected_packet_name, *typed_spot, *instance, provider,
            serializers, body);
        const auto mismatched =
          spot_context_access_t::create (spot).handlers ().invoke_actor_packet (
            rt::protocol::session_disconnected_packet_name, *typed_spot, *instance, provider,
            serializers,
            zlink::message_t::from (rt::protocol::encode_session_disconnected (
              {zlink::routing_id_t::from ("session-owner").to_bytes (), 2, 8})));
        int unregistered = 9;
        const auto missing = spot_context_access_t::create (spot).handlers ().invoke_actor_packet (
          rt::protocol::session_disconnected_packet_name, *typed_spot, unregistered, provider,
          serializers, body);
        node->worker_executor->drain ();
        const auto passed = delivered && delivered.value ().is_empty () && mismatched
                                && mismatched.value ().is_empty () && callbacks->load () == 1
                                && !missing
                                && missing.error_kind () == framework_error_kind_t::not_found
                              ? 0
                              : 2;
        spots.release_native_handles ();
        return passed;
    }

    auto terminal_source = std::make_shared<task_completion_source_t<zlink::message_t>> ();
    auto terminal = terminal_source->task ();
    auto terminals = std::make_shared<std::atomic_int> (0);
    auto preserved_route = std::make_shared<std::atomic_bool> (false);
    node->actor_handoff_terminal_sender =
      [terminals, preserved_route, entered_once, entered,
       terminal_source] (const zlink::routing_id_t &parking, const zlink::routing_id_t &owner,
                         const rt::protocol::wire_operation_id_t &operation, std::uint64_t reply,
                         const rt::protocol::actor_route_fence_t &,
                         const result_t<zlink::message_t> &completed) -> task_t<bool> {
        preserved_route->store (parking.to_string () == "parking-node"
                                  && owner.to_string () == "request-owner" && operation.high == 83
                                  && operation.low == 91 && reply == 17,
                                std::memory_order_release);
        terminals->fetch_add (1, std::memory_order_acq_rel);
        if (!entered_once->exchange (true))
            entered->set_value (false);
        terminal_source->complete (completed);
        co_return true;
    };
    const auto reserved = spots.reserve_actor_join_barrier (actor);
    if (!reserved) {
        release->complete (result_t<void>::success ());
        return 3;
    }
    const auto barrier = reserved.value ();
    // Reservation retains the Actor FIFO; only the actual source seal moves
    // later ingress into the handoff hold.
    const auto sealed =
      spots
        .seal_actor_application_suffix (
          actor,
          [&] {
              if (!node->actor_transfer_coordinator->try_begin_local ("player:context-player"))
                  throw std::runtime_error ("disconnect replay source did not seal");
          })
        .result ();
    if (!sealed) {
        release->complete (result_t<void>::success ());
        barrier->cancel ();
        node->actor_transfer_coordinator->cancel_move ("player:context-player");
        return 8;
    }
    spot_inbound_message_t metadata;
    metadata.values.emplace (std::string (actor_handoff_source_node_key),
                             zlink::routing_id_t::from ("request-owner").to_hex ());
    metadata.values.emplace (std::string (actor_handoff_parking_node_key),
                             zlink::routing_id_t::from ("parking-node").to_hex ());
    metadata.values.emplace (std::string (actor_handoff_operation_high_key), "83");
    metadata.values.emplace (std::string (actor_handoff_operation_low_key), "91");
    metadata.values.emplace (std::string (actor_handoff_reply_route_key), "17");
    auto captured = spots.relay_actor_packet (actor, gateway.actor_context (actor),
                                              stream_message_kind_t::request,
                                              rt::protocol::session_disconnected_packet_name, body,
                                              provider, serializers, std::move (metadata));
    if (!captured.await_ready ()) {
        release->complete (result_t<void>::success ());
        barrier->cancel ();
        (void) captured.result ();
        node->worker_executor->drain ();
        return 4;
    }
    const auto parked = captured.result ();
    if (!parked || parked.value () || callbacks->load () != 0) {
        release->complete (result_t<void>::success ());
        barrier->cancel ();
        node->worker_executor->drain ();
        return 5;
    }
    std::cerr << "Session disconnect actual handoff replay without ingress metadata starts"
              << std::endl;
    const auto activated = barrier->activate_async (
      [spots, actor] (deferred_barrier_t::async_completion_t complete) mutable {
          auto opened = spots.open_local_actor_handoff (actor);
          observe_task_terminal (opened, [complete = std::move (complete)] (
                                           const result_t<result_t<void>> &outcome) mutable {
              complete (outcome ? outcome.value ()
                                : result_t<void>::failure (outcome.error_kind (),
                                                           outcome.error ()->what ()));
          });
      });
    if (!activated) {
        release->complete (result_t<void>::success ());
        barrier->cancel ();
        node->worker_executor->drain ();
        return 6;
    }
    const bool began = callback_started.get ();
    const bool acknowledged_early = terminal.await_ready ();
    // A reply can precede the replay turn's final continuation. Shutdown must
    // wait for that handler terminal rather than cancel an accepted turn.
    const auto replay_handler = spot->serial_queue->observe_handler_terminal ().value ();
    release->complete (result_t<void>::success ());
    const auto completed = terminal.result ();
    barrier->terminal ().result ().value ();
    replay_handler->run ().result ().value ();
    replay_handler->cancel ();
    drain_spot_node_executors (*node);
    const auto passed = began && !acknowledged_early && completed && completed.value ().is_empty ()
                            && callbacks->load () == 1 && terminals->load () == 1
                            && preserved_route->load (std::memory_order_acquire)
                            && node->actor_transfer_coordinator->pending_count () == 0
                          ? 0
                          : 7;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}

int physical_disconnect_submits_all_before_waiting_for_terminal ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    for (const bool non_std : {false, true}) {
        actor_gateway_runtime_t gateway;
        auto manager = gateway.manager ();
        session_actor_manager_access_t::attach (manager, stream_t{});
        const auto first = test_actor_ref ("owner", "player", "first", 1);
        const auto second = test_actor_ref ("owner", "player", "second", 1);
        auto first_bound = manager.bind (first).async ().result ().value ();
        auto second_bound = manager.bind (second).async ().result ().value ();
        auto release = std::make_shared<task_completion_source_t<void>> ();
        std::vector<std::string> started;
        gateway.on_disconnect (
          [&] (const actor_ref_t &actor,
               const std::optional<runtime::stateful::stream_binding_t> &) -> task_t<void> {
              started.emplace_back (actor.actor_id ().value ());
              if (actor.actor_id ().value () == "first") {
                  co_await release->task ();
                  if (non_std)
                      throw 7;
                  throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                               "first callback failed");
              }
              co_return;
          });
        auto pending = session_actor_manager_access_t::disconnect_async (manager);
        const bool all_started =
          started == std::vector<std::string>{"first", "second"} && !pending.await_ready ();
        release->complete (result_t<void>::success ());
        const auto completed = pending.result ();
        const auto first_again = first_bound.notify_disconnected ().result ();
        const auto second_again = second_bound.notify_disconnected ().result ();
        const bool cleaned = !first_again && !second_again;
        std::cerr << "Session disconnect terminal non_std=" << non_std
                  << " all_started=" << all_started << " cleaned=" << cleaned
                  << " completed=" << static_cast<bool> (completed) << std::endl;
        if (!all_started || !completed || !cleaned || started.size () != 2)
            return 1;
    }
    return 0;
}
int session_disconnect_dispatcher_preserves_identity_snapshot ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace sf = zlink::framework::runtime::stateful;
    for (const int mode : {0, 1, 2, 3, 4, 5, 6, 7}) {
        const bool physical = mode != 0 && (mode < 5 || mode == 7);
        const bool racing = mode >= 2 && mode < 5;
        const bool failing = mode == 3;
        actor_gateway_runtime_t gateway;
        auto manager = gateway.manager ();
        session_actor_manager_access_t::attach (manager, stream_t{});
        const auto actor = test_actor_ref ("actor-owner", "player", "snapshot-player", 1);
        const sf::object_ref_t target{
          sf::object_kind_t::actor, "snapshot-player", 1, 9, "mesh", "actor-owner"};
        sf::stream_session_registry_t registry (
          [&] (const auto &) { return std::make_optional (target); });
        const auto connection = registry.open (
          "connection", {}, zlink::routing_id_t::from ("original-owner"), UINT64_MAX);
        session_actor_manager_access_t::bind_native (
          manager, [&] (actor_ref_t, std::uint64_t token) -> task_t<void> {
              const auto result = registry.bind_remote (connection, target, 11, 13, {}, token);
              if (result.first != sf::stateful_error_t::none)
                  throw std::runtime_error ("canonical bind failed");
              co_return;
          });
        std::optional<sf::stream_binding_t> retired;
        session_actor_manager_access_t::bind_disconnect (
          manager,
          [&] (const actor_ref_t &, std::uint64_t) {
              return registry.current_binding (target.key);
          },
          [&] (sf::stream_binding_t binding) -> task_t<void> {
              const auto removed = registry.unbind (binding);
              retired = removed.second;
              co_return;
          });
        auto bound = manager.bind (actor).async ().result ().value ();
        const auto original = *registry.current_binding (target.key);
        int callbacks = 0;
        bool exact = false;
        auto release = std::make_shared<task_completion_source_t<void>> ();
        gateway.on_disconnect (
          [&] (const actor_ref_t &,
               const std::optional<sf::stream_binding_t> &snapshot) -> task_t<void> {
              ++callbacks;
              exact = snapshot && *snapshot == original;
              co_await release->task ();
              // The coroutine's snapshot argument remains alive until terminal.
              exact = exact && snapshot && *snapshot == original;
              if (failing)
                  throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                               "callback failure");
          });
        std::optional<task_t<void>> logical;
        if (racing && mode != 4)
            logical.emplace (bound.notify_disconnected ());
        std::optional<task_t<void>> pending;
        bool closed_exactly = true;
        if (physical) {
            pending.emplace (session_actor_manager_access_t::disconnect_async (manager, [&] {
                auto closed = registry.close (connection);
                closed_exactly =
                  closed && closed.bindings == std::vector<sf::stream_binding_t>{original}
                  && !registry.current_binding (target.key) && !registry.close (connection);
                // Force logical capture into the exact gap after canonical
                // removal and before physical snapshot dispatch.
                if (mode == 4)
                    logical.emplace (bound.notify_disconnected ());
                return std::move (closed.bindings);
            }));
        } else
            pending.emplace (bound.notify_disconnected ());
        bool route_changed = true;
        if (mode == 5) {
            const auto sealed = registry.try_seal_actor (target);
            auto moved = target;
            moved.node_id = "moved-actor-owner";
            moved.authority_owner_generation = 17;
            const auto committed =
              registry.commit_remote_route (connection.connection_id, original.binding_generation,
                                            target.key, target.object_generation, moved, 19, 23);
            route_changed = sealed.first == sf::stateful_error_t::none
                            && committed.error == sf::stateful_error_t::none;
        } else if (mode == 6) {
            const auto rebound = registry.bind_remote (connection, target, 11, 13, {},
                                                       original.binding_generation + 1);
            route_changed = rebound.first == sf::stateful_error_t::none;
        }
        const bool held = callbacks == 1 && !pending->await_ready ();
        if (mode == 7)
            manager = session_actor_manager_t ();
        release->complete (result_t<void>::success ());
        const auto completed = pending->result ();
        const auto logical_result = logical ? logical->result () : result_t<void>::success ();
        const auto remaining = registry.current_binding (target.key);
        const bool expected_remaining =
          mode == 6 ? remaining && remaining->binding_generation == original.binding_generation + 1
                    : !remaining;
        const bool cleanup_route = mode == 5
                                     ? retired && retired->actor.node_id == "moved-actor-owner"
                                         && retired->target_node_generation == 19
                                         && retired->owner_lease_generation == 23
                                   : mode == 6 ? !retired
                                               : true;
        if (!completed || !closed_exactly || !route_changed || !cleanup_route
            || (static_cast<bool> (logical_result) == failing) || !held || !exact || callbacks != 1
            || !expected_remaining)
            return 2;
        if (remaining)
            (void) registry.unbind (*remaining);
        const auto repeated = bound.notify_disconnected ().result ();
        if (callbacks != 1 || (mode == 7 && repeated))
            return 3;
    }
    return 0;
}
} // namespace zlink::framework::test::actor_gateway
