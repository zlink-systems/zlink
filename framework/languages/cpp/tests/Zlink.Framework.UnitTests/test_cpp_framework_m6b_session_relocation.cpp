/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6b_cases.hpp"
#include <zlink/framework/contracts/configuration/logging.hpp>
#include <functional>
#include "../support/m6b_relocation_repository.hpp"
#include "../support/m6b_host_access.hpp"
#include "../support/m6b_task.hpp"
#include "../support/m6b_mesh_fixture.hpp"
#include "../support/m6b_stateful_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/framework/contracts/configuration/app.hpp>
#include <zlink/framework/contracts/configuration/framework_options.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace foundation = zlink::framework::runtime::foundation;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;
namespace host = zlink::framework::runtime::host;
using namespace std::chrono_literals;

namespace zlink::framework::test::m6b
{

struct blocking_log_sink_state_t
{
    std::promise<void> copy_entered;
    std::promise<void> release_copy;
    std::shared_future<void> released = release_copy.get_future ().share ();
    std::atomic_bool block_next_copy{false};
    std::function<bool ()> on_terminal_lane;
    std::atomic_bool diagnostic_logged_inside_terminal{false};
};

struct blocking_log_sink_t
{
    explicit blocking_log_sink_t (std::shared_ptr<blocking_log_sink_state_t> state_) :
        state (std::move (state_))
    {
    }
    blocking_log_sink_t (const blocking_log_sink_t &other) : state (other.state)
    {
        if (state->block_next_copy.exchange (false, std::memory_order_acq_rel)) {
            state->copy_entered.set_value ();
            state->released.wait ();
        }
    }
    void operator() (const zlink::framework::log_record_t &record) const
    {
        if (record.message == "late_session_relocation_seal" && state->on_terminal_lane
            && state->on_terminal_lane ())
            state->diagnostic_logged_inside_terminal.store (true, std::memory_order_release);
    }

    std::shared_ptr<blocking_log_sink_state_t> state;
};

void verify_session_relocation_gateway_commit_is_atomic ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const auto source = actor_ref_access_t::make (node_rid_t::from_string ("source-node"),
                                                  "game.actor", "relocating-actor", 7);
    const auto session_owner = zlink::routing_id_t::from (std::string ("session-owner"));
    const auto session = zlink::routing_id_t::from (std::string ("session-rid"));
    assert (gateway.bind_session_sink (source,
                                       [] (std::string, stream_codec_t, const zlink::message_t &) {
                                           return task_t<void> (result_t<void>::success ());
                                       }));
    assert (
      gateway.record_bound_session_route (source, session_owner, session, 5, 11, 13, 17, 0, 19));

    const stateful::stream_binding_t previous{
      {session.to_string (), 3},
      17,
      {stateful::object_kind_t::actor, "relocating-actor", 7, 11, "game", "source-node"},
      4,
      14};
    const stateful::stream_binding_t target{
      previous.connection,
      previous.binding_generation,
      {stateful::object_kind_t::actor, "relocating-actor", 7, 12, "game", "target-node"},
      23,
      29};
    protocol::session_relocation_route_t route{
      {31, 37},
      {"source-owner", 13, zlink::routing_id_t::from (std::string ("source-node")).to_bytes (), 4,
       "authority-v1"},
      protocol::relocation_role_t::target,
      {"relocating-actor", 7},
      session_owner.to_bytes (),
      5,
      "session-owner-lease",
      41,
      session.to_bytes (),
      17,
      {protocol::session_relocation_route_action_t::commit, 11, 12,
       zlink::routing_id_t::from (std::string ("target-node")).to_bytes (), 23, 0, 37}};
    route.coordinator.owner_id = "transport-coordinator";
    route.route.previous_authority_owner_generation = 10;
    const auto target_ref = actor_ref_access_t::make (node_rid_t::from_string ("target-node"),
                                                      "game.actor", "relocating-actor", 7);
    assert (gateway.update_actor_ref (target_ref));
    {
        const auto ref_is_source = state->sync ([&] {
            const auto actor = state->actors_by_id.find ("relocating-actor");
            return actor != state->actors_by_id.end ()
                   && actor->second.ref.node_rid ().value () == "source-node";
        });
        assert (ref_is_source);
    }
    assert (gateway.commit_session_relocation_route (route, previous, target));
    const auto committed_route = gateway.bound_session_route_async (source).result ().value ();
    assert (
      committed_route && committed_route->node_rid == session_owner
      && committed_route->session_rid == std::make_optional (session)
      && committed_route->node_generation == 5 && committed_route->authority_owner_generation == 12
      && committed_route->owner_lease_generation == 29 && committed_route->binding_generation == 17
      && committed_route->session_sequence == 19);
    {
        const auto ref_is_target = state->sync ([&] {
            const auto actor = state->actors_by_id.find ("relocating-actor");
            return actor != state->actors_by_id.end ()
                   && actor->second.ref.node_rid ().value () == "target-node";
        });
        assert (ref_is_target);
    }

    /* A later relocation can return the same Actor incarnation to its first
     * node. The physical Session and binding generation stay fixed while the
     * Actor authority, owner lease, and admitted high-water advance again. */
    assert (gateway.admit_session_relay (target_ref, session_owner, session, 17, 20));
    const stateful::stream_binding_t returned{
      target.connection,
      target.binding_generation,
      {stateful::object_kind_t::actor, "relocating-actor", 7, 13, "game", "source-node"},
      4,
      31};
    auto return_route = route;
    return_route.relocation = {41, 43};
    return_route.actor = {"relocating-actor", 7};
    return_route.route.previous_authority_owner_generation = 12;
    return_route.route.target_authority_owner_generation = 13;
    return_route.route.target_node_routing_id =
      zlink::routing_id_t::from (std::string ("source-node")).to_bytes ();
    return_route.route.target_node_generation = 4;
    assert (gateway.commit_session_relocation_route (return_route, target, returned));
    const auto returned_ref = actor_ref_access_t::make (node_rid_t::from_string ("source-node"),
                                                        "game.actor", "relocating-actor", 7);
    const auto returned_route = gateway.bound_session_route_async (returned_ref).result ().value ();
    assert (returned_route && returned_route->node_rid == session_owner
            && returned_route->session_rid == std::make_optional (session)
            && returned_route->node_generation == 5
            && returned_route->authority_owner_generation == 13
            && returned_route->owner_lease_generation == 31
            && returned_route->binding_generation == 17 && returned_route->session_sequence == 20);
}
void verify_session_binding_and_terminal_once ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates ({{"mesh-a", "node-a", {"player"}, 100, 16, 0, 8, 0}});
    const auto actor = create_ready (
      runtime,
      {stateful::object_kind_t::actor, "session-actor", "player", std::nullopt, {}, false, false});
    const auto second_actor = create_ready (runtime, {stateful::object_kind_t::actor,
                                                      "session-actor-2",
                                                      "player",
                                                      std::nullopt,
                                                      {},
                                                      false,
                                                      false});

    std::size_t authority_reads = 0;
    stateful::stream_session_registry_t sessions ([&] (const std::string &actor_id) {
        ++authority_reads;
        return runtime.find (stateful::object_kind_t::actor, actor_id);
    });
    const auto connection = sessions.open ("stream-rid");
    const auto [bind_error, binding] = sessions.bind (connection, actor);
    assert (bind_error == stateful::stateful_error_t::none);
    const auto [second_bind_error, second_binding] = sessions.bind (connection, second_actor);
    assert (second_bind_error == stateful::stateful_error_t::none);
    assert (sessions.is_current (binding));
    assert (sessions.is_current (second_binding));
    assert (authority_reads == 2);
    const auto [dispatch_error, dispatch] = sessions.admit_inbound (binding);
    assert (dispatch_error == stateful::stateful_error_t::none);
    assert (dispatch && dispatch->inbound_sequence == 1);
    const auto [second_dispatch_error, second_dispatch] = sessions.admit_inbound (second_binding);
    assert (second_dispatch_error == stateful::stateful_error_t::none);
    assert (second_dispatch && second_dispatch->inbound_sequence == 1);
    assert (authority_reads == 2);

    assert (sessions.complete_inbound (*dispatch) == stateful::stateful_error_t::none);
    assert (sessions.complete_inbound (*second_dispatch) == stateful::stateful_error_t::none);

    const auto replacement_connection = sessions.open ("stream-rid-b");
    const auto [replacement_error, replacement] = sessions.bind (replacement_connection, actor);
    assert (replacement_error == stateful::stateful_error_t::none);
    assert (authority_reads == 3);
    assert (!sessions.is_current (binding));
    assert (sessions.is_current (replacement));
    assert (sessions.admit_inbound (binding).first == stateful::stateful_error_t::conflict);

    const auto [barrier_error, barrier] = sessions.try_seal_actor (actor);
    assert (barrier_error == stateful::stateful_error_t::none);
    auto relocated_actor = actor;
    relocated_actor.node_id = "node-relocated";
    ++relocated_actor.authority_owner_generation;
    assert (sessions.commit_barrier (barrier, relocated_actor) == stateful::stateful_error_t::none);
    const auto relocated_binding = sessions.current_binding (actor.key);
    assert (relocated_binding);
    assert (relocated_binding->binding_generation == replacement.binding_generation);
    assert (relocated_binding->actor == relocated_actor);
    const auto unchanged_binding = sessions.current_binding (second_actor.key);
    assert (unchanged_binding && unchanged_binding->actor == second_actor);
    assert (unchanged_binding->binding_generation == second_binding.binding_generation);
    assert (authority_reads == 3);

    assert (sessions.close (connection));
    assert (sessions.is_current (*relocated_binding));
    assert (authority_reads == 3);

    const auto reconnect = sessions.open ("stream-rid");
    assert (reconnect.connection_generation == connection.connection_generation + 1);
    assert (!sessions.is_current (binding));
    assert (sessions.admit_inbound (binding).first == stateful::stateful_error_t::conflict);
    assert (!sessions.is_current (second_binding));

    foundation::operation_registry_t operations;
    foundation::call_id_t id{};
    id.low = 1;
    std::atomic_size_t terminal_count{0};
    assert (operations.register_operation (
      id, foundation::operation_registry_t::clock_t::now () + 1s,
      [&] (foundation::operation_terminal_t, ::zlink::framework::result_t<zlink::message_t>) {
          terminal_count.fetch_add (1, std::memory_order_release);
      }));
    assert (operations.complete (id, zlink::message_t::from (std::vector<std::uint8_t>{1})));
    assert (!operations.cancel (id));
    const auto terminal_deadline = std::chrono::steady_clock::now () + 5s;
    while (terminal_count.load (std::memory_order_acquire) != 1
           && std::chrono::steady_clock::now () < terminal_deadline) {
        std::this_thread::yield ();
    }
    assert (terminal_count.load (std::memory_order_acquire) == 1);
}

void verify_session_ingress_sequence_is_scoped_by_actor_binding ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates ({{"mesh-a", "node-a", {"player"}, 100, 16, 0, 8, 0}});
    const auto actor_a = create_ready (
      runtime,
      {stateful::object_kind_t::actor, "session-a", "player", std::nullopt, {}, false, false});
    const auto actor_b = create_ready (
      runtime,
      {stateful::object_kind_t::actor, "session-b", "player", std::nullopt, {}, false, false});

    stateful::stream_session_registry_t sessions ([&] (const std::string &actor_id) {
        return runtime.find (stateful::object_kind_t::actor, actor_id);
    });
    const auto connection = sessions.open ("shared-stream-rid");
    const auto [bind_a_error, binding_a] = sessions.bind (connection, actor_a, 11, 21);
    const auto [bind_b_error, binding_b] = sessions.bind (connection, actor_b, 12, 22);
    assert (bind_a_error == stateful::stateful_error_t::none);
    assert (bind_b_error == stateful::stateful_error_t::none);

    const auto [admit_a_error, admitted_a] = sessions.admit_inbound (
      connection.connection_id, binding_a.binding_generation, actor_a.key, 1, 0ms);
    const auto [admit_b_error, admitted_b] = sessions.admit_inbound (
      connection.connection_id, binding_b.binding_generation, actor_b.key, 1, 0ms);
    assert (admit_a_error == stateful::stateful_error_t::none);
    assert (admit_b_error == stateful::stateful_error_t::none);
    assert (admitted_a && admitted_a->inbound_sequence == 1);
    assert (admitted_b && admitted_b->inbound_sequence == 1);
    assert (sessions.complete_inbound (*admitted_a) == stateful::stateful_error_t::none);
    assert (sessions.complete_inbound (*admitted_b) == stateful::stateful_error_t::none);

    assert (
      sessions
        .admit_inbound (connection.connection_id, binding_a.binding_generation, actor_a.key, 1, 0ms)
        .first
      == stateful::stateful_error_t::conflict);

    const auto sealed_a = sessions.seal_remote_route (
      connection.connection_id, binding_a.binding_generation, actor_a.key,
      actor_a.object_generation, binding_a.owner_lease_generation);
    assert (sealed_a.error == stateful::stateful_error_t::none);
    assert (sealed_a.last_accepted_sequence == 1);

    const auto [admit_b_second_error, admitted_b_second] = sessions.admit_inbound (
      connection.connection_id, binding_b.binding_generation, actor_b.key, 2, 0ms);
    assert (admit_b_second_error == stateful::stateful_error_t::none);
    assert (admitted_b_second && admitted_b_second->inbound_sequence == 2);
    assert (sessions.complete_inbound (*admitted_b_second) == stateful::stateful_error_t::none);
    const auto sealed_b = sessions.seal_remote_route (
      connection.connection_id, binding_b.binding_generation, actor_b.key,
      actor_b.object_generation, binding_b.owner_lease_generation);
    assert (sealed_b.error == stateful::stateful_error_t::none);
    assert (sealed_b.last_accepted_sequence == 2);
    assert (sessions.abort_barrier (sealed_b.barrier) == stateful::stateful_error_t::none);
    assert (sessions.abort_barrier (sealed_a.barrier) == stateful::stateful_error_t::none);

    const auto [rebind_error, rebound_a] = sessions.bind (connection, actor_a, 11, 21);
    assert (rebind_error == stateful::stateful_error_t::none);
    assert (rebound_a.binding_generation > binding_a.binding_generation);
    const auto [rebound_admit_error, rebound_admitted] = sessions.admit_inbound (
      connection.connection_id, rebound_a.binding_generation, actor_a.key, 1, 0ms);
    assert (rebound_admit_error == stateful::stateful_error_t::none);
    assert (rebound_admitted && rebound_admitted->inbound_sequence == 1);
    assert (sessions.complete_inbound (*rebound_admitted) == stateful::stateful_error_t::none);
}

void verify_session_route_supports_repeated_relocation ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates ({{"mesh-a", "node-a", {"player"}, 100, 16, 0, 8, 0}});
    const auto source = create_ready (runtime, {stateful::object_kind_t::actor,
                                                "session-round-trip",
                                                "player",
                                                std::nullopt,
                                                {},
                                                false,
                                                false});

    stateful::stream_session_registry_t sessions ([&] (const std::string &actor_id) {
        return runtime.find (stateful::object_kind_t::actor, actor_id);
    });
    const auto connection = sessions.open ("round-trip-session-rid");
    const auto [bind_error, binding] = sessions.bind (connection, source, 11, 21);
    assert (bind_error == stateful::stateful_error_t::none);

    const auto [first_error, first] = sessions.admit_inbound (
      connection.connection_id, binding.binding_generation, source.key, 1, 0ms);
    assert (first_error == stateful::stateful_error_t::none);
    assert (first && first->inbound_sequence == 1);
    assert (sessions.complete_inbound (*first) == stateful::stateful_error_t::none);

    const auto first_seal =
      sessions.seal_remote_route (connection.connection_id, binding.binding_generation, source.key,
                                  source.object_generation, binding.owner_lease_generation);
    assert (first_seal.error == stateful::stateful_error_t::none);
    assert (first_seal.last_accepted_sequence == 1);
    auto remote = source;
    remote.node_id = "node-b";
    ++remote.authority_owner_generation;
    const auto first_commit =
      sessions.commit_remote_route (connection.connection_id, binding.binding_generation,
                                    source.key, source.object_generation, remote, 12, 23);
    assert (first_commit.error == stateful::stateful_error_t::none);
    assert (first_commit.binding);
    assert (first_commit.binding->actor == remote);
    assert (!sessions.remote_route_sealed (source.key));

    const auto [second_error, second] = sessions.admit_inbound (
      connection.connection_id, binding.binding_generation, source.key, 2, 0ms);
    assert (second_error == stateful::stateful_error_t::none);
    assert (second && second->inbound_sequence == 2);
    assert (sessions.complete_inbound (*second) == stateful::stateful_error_t::none);

    const auto return_seal = sessions.seal_remote_route (
      connection.connection_id, binding.binding_generation, remote.key, remote.object_generation,
      first_commit.binding->owner_lease_generation);
    assert (return_seal.error == stateful::stateful_error_t::none);
    assert (return_seal.last_accepted_sequence == 2);
    auto returned = remote;
    returned.node_id = source.node_id;
    ++returned.authority_owner_generation;
    const auto return_commit =
      sessions.commit_remote_route (connection.connection_id, binding.binding_generation,
                                    source.key, source.object_generation, returned, 11, 31);
    assert (return_commit.error == stateful::stateful_error_t::none);
    assert (return_commit.binding);
    assert (return_commit.binding->actor == returned);
    assert (return_commit.binding->connection == binding.connection);
    assert (return_commit.binding->binding_generation == binding.binding_generation);
    assert (!sessions.remote_route_sealed (source.key));
}

void verify_session_route_commit_stores_target_lease ()
{
    stateful::stream_session_registry_t sessions (
      [] (const std::string &) { return std::optional<stateful::object_ref_t>{}; });
    const auto connection = sessions.open ("deferred-lease-session");
    const stateful::object_ref_t source{
      stateful::object_kind_t::actor, "deferred-lease-actor", 7, 11, "mesh-a", "node-a"};
    const auto [bind_error, binding] = sessions.bind_remote (connection, source, 13, 17);
    assert (bind_error == stateful::stateful_error_t::none);
    const auto sealed =
      sessions.seal_remote_route (connection.connection_id, binding.binding_generation, source.key,
                                  source.object_generation, binding.owner_lease_generation);
    assert (sealed.error == stateful::stateful_error_t::none);

    auto target = source;
    target.node_id = "node-b";
    ++target.authority_owner_generation;
    bool projection_hook_called = false;
    bool projection_owns_registry_lane = false;
    bool registry_reentry_rejected = false;
    const auto committed = sessions.commit_remote_route (
      connection.connection_id, binding.binding_generation, source.key, source.object_generation,
      target, 19, 23, [&] (const stateful::stream_route_admission_t &projected) {
          projection_hook_called = true;
          assert (projected.binding);
          assert (projected.binding->owner_lease_generation == 23);
          projection_owns_registry_lane =
            zlink::framework::runtime::state_lane_t::current () != nullptr;
          try {
              (void) sessions.current_binding (source.key);
          }
          catch (const std::logic_error &) {
              registry_reentry_rejected = true;
          }
          return false;
      });
    assert (committed.error == stateful::stateful_error_t::none);
    assert (committed.binding);
    assert (committed.binding->owner_lease_generation == 23);
    assert (projection_hook_called);
    assert (projection_owns_registry_lane);
    assert (registry_reentry_rejected);
    assert (sessions.current_binding (source.key) == committed.binding);
    assert (!sessions.remote_route_sealed (source.key));
}

void verify_displaced_stream_binding_can_be_restored ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates ({{"mesh-a", "node-a", {"player"}, 100, 16, 0, 8, 0}});
    const auto actor = create_ready (runtime, {stateful::object_kind_t::actor,
                                               "restore-session-actor",
                                               "player",
                                               std::nullopt,
                                               {},
                                               false,
                                               false});
    stateful::stream_session_registry_t sessions ([&] (const std::string &actor_id) {
        return runtime.find (stateful::object_kind_t::actor, actor_id);
    });

    const auto first_connection = sessions.open ("restore-stream-a");
    const auto second_connection = sessions.open ("restore-stream-b");
    const auto [first_error, first_binding] = sessions.bind (first_connection, actor);
    assert (first_error == stateful::stateful_error_t::none);
    const auto [second_error, second_binding] = sessions.bind (second_connection, actor);
    assert (second_error == stateful::stateful_error_t::none);
    assert (sessions.current_binding (actor.key) == second_binding);

    const auto removed = sessions.unbind (second_binding);
    assert (removed.first == stateful::stateful_error_t::none);
    assert (removed.second == second_binding);
    assert (sessions.restore (first_binding) == stateful::stateful_error_t::none);
    assert (sessions.current_binding (actor.key) == first_binding);

    const auto [third_error, third_binding] = sessions.bind (second_connection, actor);
    assert (third_error == stateful::stateful_error_t::none);
    assert (sessions.restore (first_binding) == stateful::stateful_error_t::conflict);
    assert (sessions.current_binding (actor.key) == third_binding);
}

void verify_verified_remote_stream_binding ()
{
    stateful::stream_session_registry_t sessions (
      [] (const std::string &) { return std::optional<stateful::object_ref_t>{}; });
    const auto connection = sessions.open ("remote-stream");
    const stateful::object_ref_t remote_actor{
      stateful::object_kind_t::actor, "remote-actor", 7, 11, "mesh-a", "actor-node"};
    const auto [error, binding] = sessions.bind_remote (connection, remote_actor, 13, 17);
    assert (error == stateful::stateful_error_t::none);
    assert (binding.actor == remote_actor);
    assert (binding.target_node_generation == 13);
    assert (binding.owner_lease_generation == 17);
    assert (sessions.current_binding (remote_actor.key) == binding);

    auto invalid = remote_actor;
    invalid.authority_owner_generation = 0;
    assert (sessions.bind_remote (connection, invalid, 13, 17).first
            == stateful::stateful_error_t::invalid);
}
void verify_root_location_session_seal_timeout_is_startup_snapshot ()
{
    auto app = zlink::framework::app_t::create ();
    auto store = std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    zlink::framework::location_options_t *configured_locations = nullptr;

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (store);
        configured_locations = &options.configure_locations ();
        configured_locations->session_relocation_seal_timeout = 17ms;
    });
    assert (configured_locations != nullptr);

    configured_locations->session_relocation_seal_timeout = 29ms;
    auto provider = app.advanced ().services ().build_provider ();
    assert (provider.get_required<zlink::framework::runtime::location_runtime_t> ()
              .options ()
              .session_relocation_seal_timeout
            == 17ms);
}

void verify_same_node_session_seal_waits_for_active_ingress ()
{
    auto local = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("same-node-session-seal"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      "entry",
      {"player"}});
    auto relocation_store = std::make_shared<memory_relocation_repository_t> ();
    local->configure_session_relocation_store (relocation_store);
    local->configure_session_route_owner ([] {
        return std::make_optional (
          zlink::framework::location_owner_token_t{"same-node-session-owner", 19});
    });
    local->start ();
    const auto status = local->status ();
    const auto actor = local->create_actor ("player", "same-node-session-actor");
    const auto actor_object = local->resolve_actor_async (actor.ref ()).result ().value ();
    assert (actor_object);

    const auto session_rid = bytes ("same-node-session-rid");
    const auto connection =
      local->sessions ().open (zlink::routing_id_t::from (session_rid).to_hex ());
    const auto [bind_error, binding] =
      local->sessions ().bind (connection, *actor_object, status.lifecycle_generation (), 23);
    assert (bind_error == stateful::stateful_error_t::none);
    const auto [ingress_error, ingress] = local->sessions ().admit_inbound (binding);
    assert (ingress_error == stateful::stateful_error_t::none);
    assert (ingress && ingress->inbound_sequence == 1);

    const protocol::session_relocation_seal_t seal{
      {81, 82},
      {"same-node-coordinator", 7, status.routing_id ().to_bytes (), status.lifecycle_generation (),
       "store-v1"},
      protocol::relocation_role_t::coordinator,
      {actor_object->key, actor_object->object_generation, status.routing_id ().to_bytes (),
       status.lifecycle_generation (), actor_object->authority_owner_generation, 23},
      status.routing_id ().to_bytes (),
      status.lifecycle_generation (),
      "same-node-session-owner",
      19,
      session_rid,
      binding.binding_generation,
      status.routing_id ().to_bytes (),
      status.lifecycle_generation ()};
    using seal_completion_t = std::pair<foundation::operation_terminal_t,
                                        std::optional<host::session_relocation_seal_result_t>>;
    std::size_t journal_capture_count = 0;
    std::promise<seal_completion_t> completion;
    auto completed = completion.get_future ();
    assert (local
              ->seal_session_remote (
                status.routing_id (), seal, 2s, std::chrono::steady_clock::now () + 2s,
                [&journal_capture_count] {
                    ++journal_capture_count;
                    return std::vector<std::uint8_t>{0x41};
                },
                [&completion] (foundation::operation_terminal_t terminal,
                               std::optional<host::session_relocation_seal_result_t> result) {
                    completion.set_value ({terminal, std::move (result)});
                })
              .result ()
              .value ());

    const auto dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                              std::vector<zlink::message_t>, auto) {};
    // Observe the seal-registration wake before the dispatch turn can
    // consume it. The turn must leave the active ingress unfinished.
    assert (local->transport ().wait_for_activity (-1ms, false));
    await_task (local->dispatch_ready (dispatch));
    assert (completed.wait_for (0ms) != std::future_status::ready);
    assert (journal_capture_count == 0);

    assert (local->sessions ().complete_inbound (*ingress) == stateful::stateful_error_t::none);
    assert (local->wait_for_dispatch_activity (-1ms, false, std::nullopt));
    // Completion of the ingress only wakes management. Advance that turn
    // explicitly before observing seal completion and journal capture.
    assert (completed.wait_for (0ms) != std::future_status::ready);
    assert (journal_capture_count == 0);
    await_task (local->dispatch_ready (dispatch));
    assert (completed.wait_for (0ms) == std::future_status::ready);
    const auto result = completed.get ();
    assert (result.first == foundation::operation_terminal_t::completed);
    assert (result.second);
    assert (journal_capture_count == 1);
    assert (relocation_store->size () == 1);

    const auto target_authority_owner_generation = actor_object->authority_owner_generation + 7;
    protocol::session_relocation_route_t route{
      seal.relocation,
      seal.coordinator,
      protocol::relocation_role_t::target,
      {actor_object->key, actor_object->object_generation},
      status.routing_id ().to_bytes (),
      status.lifecycle_generation (),
      "same-node-session-owner",
      19,
      session_rid,
      binding.binding_generation,
      {protocol::session_relocation_route_action_t::commit,
       actor_object->authority_owner_generation, target_authority_owner_generation,
       status.routing_id ().to_bytes (), status.lifecycle_generation (), 0, 13}};
    route.coordinator.owner_id = "transport-coordinator";
    ++route.route.previous_authority_owner_generation;
    assert (local->route_session_remote (status.routing_id (), route).result ().value ());
    assert (local->wait_for_dispatch_activity (-1ms, false, std::nullopt));
    auto current = local->sessions ().current_binding (actor_object->key);
    assert (current
            && current->actor.authority_owner_generation
                 == actor_object->authority_owner_generation);
    assert (local->sessions ().remote_route_sealed (actor_object->key));
    await_task (local->dispatch_ready (dispatch));
    current = local->sessions ().current_binding (actor_object->key);
    assert (current
            && current->actor.authority_owner_generation == target_authority_owner_generation);
    assert (current->target_node_generation == status.lifecycle_generation ());
    assert (current->owner_lease_generation == 13);
    assert (!local->sessions ().remote_route_sealed (actor_object->key));

    local->close ();
}

void verify_absent_session_seal ()
{
    auto local = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("absent-session-owner"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      "entry",
      {"player"}});
    local->configure_session_relocation_store (std::make_shared<memory_relocation_repository_t> ());
    local->configure_session_route_owner ([] {
        return std::make_optional (zlink::framework::location_owner_token_t{"absent-owner", 19});
    });
    zlink::framework::logging_builder_t logging;
    logging.set_min_level (zlink::framework::log_level_t::warn);
    auto log_state = std::make_shared<blocking_log_sink_state_t> ();
    auto *local_host = local.get ();
    log_state->on_terminal_lane = [local_host] {
        return host::public_host_runtime_test_access_t::on_terminal_lane (*local_host);
    };
    logging.use_callback_sink (blocking_log_sink_t{log_state});
    auto logger = logging.create_logger ("session-native");
    auto dispatch = zlink::framework::dispatch_options_t{};
    zlink::framework::detail::dispatch_options_access_t::set_logger (dispatch, logger);
    host::public_host_runtime_test_access_t::set_dispatch (*local, std::move (dispatch));
    int late_routes = 0;
    local->configure_late_session_route_update ([&] (const auto &) { ++late_routes; });
    local->start ();
    const auto status = local->status ();
    const auto actor = local->create_actor ("player", "absent-actor");
    const auto object = local->resolve_actor_async (actor.ref ()).result ().value ().value ();
    const auto rid = zlink::routing_id_t::from ("absent-session");
    const auto original_connection = local->sessions ().open (rid.to_hex ());
    const auto [original_error, original] =
      local->sessions ().bind (original_connection, object, status.lifecycle_generation (), 23);
    assert (original_error == stateful::stateful_error_t::none);
    assert (local->sessions ().unbind (original).first == stateful::stateful_error_t::none);
    const protocol::session_relocation_seal_t seal{
      {81, 82},
      {"coordinator", 7, status.routing_id ().to_bytes (), status.lifecycle_generation (),
       "store-v1"},
      protocol::relocation_role_t::source,
      {object.key, object.object_generation, status.routing_id ().to_bytes (),
       status.lifecycle_generation (), object.authority_owner_generation, 23},
      status.routing_id ().to_bytes (),
      status.lifecycle_generation (),
      "absent-owner",
      19,
      rid.to_bytes (),
      original.binding_generation,
      status.routing_id ().to_bytes (),
      status.lifecycle_generation ()};
    const auto submit = [&] (const auto &request) {
        std::optional<host::session_relocation_seal_result_t> result;
        const bool admitted =
          local
            ->seal_session_remote (
              status.routing_id (), request, 2s, std::chrono::steady_clock::now () + 2s,
              [] { return std::vector<std::uint8_t>{0x41}; },
              [&] (foundation::operation_terminal_t terminal, auto value) {
                  assert (terminal == foundation::operation_terminal_t::completed);
                  result = std::move (value);
              })
            .result ()
            .value ();
        return std::pair{admitted, result};
    };
    const auto [admitted, result] = submit (seal);
    assert (admitted && result);
    const auto encoded = protocol::encode_session_relocation_sealed (result->sealed);
    assert (protocol::decode_session_relocation_sealed_43 (encoded).seal_result
            == protocol::service_wire_pilot_session_seal_result::binding_absent);
    assert (!local->sessions ().remote_route_sealed (object.key));
    const auto connection = original_connection;
    const auto [error, binding] =
      local->sessions ().bind (connection, object, status.lifecycle_generation (), 23);
    assert (error == stateful::stateful_error_t::none);
    // A retained absence result is replayed even after a binding appears.
    auto changed = seal;
    ++changed.binding_generation;
    assert (!submit (changed).first);
    const auto repeated = submit (seal);
    assert (repeated.first && repeated.second && repeated.second->sealed == result->sealed);
    auto mismatch = seal;
    ++mismatch.relocation.low;
    mismatch.binding_generation = original.binding_generation;
    assert (!submit (mismatch).first);
    assert (local->sessions ().is_current (binding));
    host::public_host_runtime_test_access_t::expire_seal_results (*local);
    assert (!host::public_host_runtime_test_access_t::admit_seal (*local, seal).first);
    assert (!local->sessions ().remote_route_sealed (object.key));
    assert (local->sessions ().unbind (binding).first == stateful::stateful_error_t::none);
    const auto absent_again = host::public_host_runtime_test_access_t::admit_seal (*local, seal);
    assert (absent_again.first && absent_again.second);
    assert (protocol::decode_session_relocation_sealed_43 (
              protocol::encode_session_relocation_sealed (*absent_again.second))
              .seal_result
            == protocol::service_wire_pilot_session_seal_result::binding_absent);
    const protocol::session_relocation_route_t late_route{
      seal.relocation,
      seal.coordinator,
      protocol::relocation_role_t::target,
      {object.key, object.object_generation},
      seal.session_owner_node_routing_id,
      seal.session_owner_node_generation,
      seal.session_owner_id,
      seal.session_owner_lease_generation,
      seal.session_routing_id,
      seal.binding_generation,
      {protocol::session_relocation_route_action_t::commit, object.authority_owner_generation,
       object.authority_owner_generation + 1, status.routing_id ().to_bytes (),
       status.lifecycle_generation (), 0, 23}};
    assert (local->route_session_remote (status.routing_id (), late_route).result ().value ());
    local->dispatch_ready ([] (const auto &, const auto &, auto, auto) {}).result ().value ();
    assert (late_routes == 1);
    assert (!local->sessions ().current_binding (object.key));
    const auto [next_error, next_binding] =
      local->sessions ().bind (connection, object, status.lifecycle_generation (), 23);
    assert (next_error == stateful::stateful_error_t::none);
    auto next_seal = seal;
    ++next_seal.relocation.low;
    next_seal.binding_generation = next_binding.binding_generation;
    auto stale_owner = next_seal;
    --stale_owner.actor.owner_lease_generation;
    assert (!host::public_host_runtime_test_access_t::admit_seal (*local, stale_owner).first);
    const auto sealed = host::public_host_runtime_test_access_t::admit_seal (*local, next_seal);
    assert (sealed.first && sealed.second);
    assert (protocol::decode_session_relocation_sealed_43 (
              protocol::encode_session_relocation_sealed (*sealed.second))
              .seal_result
            == protocol::service_wire_pilot_session_seal_result::sealed);
    auto committed = late_route;
    committed.relocation = next_seal.relocation;
    committed.binding_generation = next_binding.binding_generation;
    assert (local->route_session_remote (status.routing_id (), committed).result ().value ());
    local->dispatch_ready ([] (const auto &, const auto &, auto, auto) {}).result ().value ();
    assert (!local->sessions ().remote_route_sealed (object.key));
    log_state->block_next_copy.store (true, std::memory_order_release);
    std::thread occupying_logger_lane ([&] { logger.warn ("occupy_session_log_lane"); });
    assert (log_state->copy_entered.get_future ().wait_for (5s) == std::future_status::ready);
    std::promise<void> terminal_turn_entered;
    auto entered = terminal_turn_entered.get_future ();
    std::promise<void> release_terminal_turn;
    const auto terminal_released = release_terminal_turn.get_future ().share ();
    assert (host::public_host_runtime_test_access_t::post_terminal_turn (*local, [&] {
        terminal_turn_entered.set_value ();
        terminal_released.wait ();
    }));
    assert (entered.wait_for (5s) == std::future_status::ready);
    std::jthread unblock_workers ([&] {
        std::this_thread::sleep_for (25ms);
        release_terminal_turn.set_value ();
        std::this_thread::sleep_for (100ms);
        log_state->release_copy.set_value ();
    });
    const auto caller_thread = std::this_thread::get_id ();
    const auto closed_record =
      host::public_host_runtime_test_access_t::admit_seal (*local, next_seal);
    unblock_workers.join ();
    occupying_logger_lane.join ();
    assert (closed_record.first && !closed_record.second);
    const auto &records = logging.captured_records ();
    assert (records.size () == 2);
    assert (records.back ().level == zlink::framework::log_level_t::warn);
    assert (records.back ().category == "session-native");
    assert (records.back ().message == "late_session_relocation_seal");
    assert (records.back ().fields.empty ());
    assert (records.back ().thread_id == caller_thread);
    assert (!log_state->diagnostic_logged_inside_terminal.load (std::memory_order_acquire));
    const auto current = local->sessions ().current_binding (object.key);
    assert (current
            && local->sessions ().unbind (*current).first == stateful::stateful_error_t::none);
    const auto [replacement_error, replacement] = local->sessions ().bind_remote (
      connection, current->actor, current->target_node_generation, current->owner_lease_generation);
    assert (replacement_error == stateful::stateful_error_t::none);
    assert (replacement.binding_generation != current->binding_generation);
    assert (!host::public_host_runtime_test_access_t::admit_seal (*local, next_seal).first);
    assert (local->sessions ().is_current (replacement));
    assert (local->sessions ().unbind (replacement).first == stateful::stateful_error_t::none);
    const auto removed = host::public_host_runtime_test_access_t::admit_seal (*local, next_seal);
    assert (removed.first && removed.second);
    assert (protocol::decode_session_relocation_sealed_43 (
              protocol::encode_session_relocation_sealed (*removed.second))
              .seal_result
            == protocol::service_wire_pilot_session_seal_result::binding_absent);
    local->close ();
}

void verify_configured_session_seal_timeout_closes_actual_owner ()
{
    auto options = host::host_options_t{
      mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("configured-session-seal-timeout"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      "entry",
      {"player"}};
    options.session_relocation_seal_timeout = 17ms;
    auto local = std::make_shared<host::public_host_runtime_t> (std::move (options));
    auto relocation_store = std::make_shared<memory_relocation_repository_t> ();
    local->configure_session_relocation_store (relocation_store);
    local->configure_session_route_owner ([] {
        return std::make_optional (
          zlink::framework::location_owner_token_t{"configured-session-owner", 19});
    });
    std::atomic_int late_session_route_update_count{0};
    local->configure_late_session_route_update (
      [&late_session_route_update_count] (const protocol::session_relocation_route_t &) {
          late_session_route_update_count.fetch_add (1, std::memory_order_acq_rel);
      });
    local->start ();

    const auto status = local->status ();
    const auto actor = local->create_actor ("player", "configured-session-timeout-actor");
    const auto actor_object = local->resolve_actor_async (actor.ref ()).result ().value ();
    assert (actor_object);
    const auto session_rid = bytes ("configured-session-timeout-rid");
    std::atomic_int physical_close_count{0};
    const auto connection = local->sessions ().open (
      zlink::routing_id_t::from (session_rid).to_hex (),
      [&physical_close_count] { physical_close_count.fetch_add (1, std::memory_order_acq_rel); });
    const auto [bind_error, binding] =
      local->sessions ().bind (connection, *actor_object, status.lifecycle_generation (), 23);
    assert (bind_error == stateful::stateful_error_t::none);
    const auto [ingress_error, ingress] = local->sessions ().admit_inbound (binding);
    assert (ingress_error == stateful::stateful_error_t::none);
    assert (ingress);

    const protocol::session_relocation_seal_t seal{
      {91, 92},
      {"configured-session-coordinator", 7, status.routing_id ().to_bytes (),
       status.lifecycle_generation (), "store-v1"},
      protocol::relocation_role_t::coordinator,
      {actor_object->key, actor_object->object_generation, status.routing_id ().to_bytes (),
       status.lifecycle_generation (), actor_object->authority_owner_generation, 23},
      status.routing_id ().to_bytes (),
      status.lifecycle_generation (),
      "configured-session-owner",
      19,
      session_rid,
      binding.binding_generation,
      status.routing_id ().to_bytes (),
      status.lifecycle_generation ()};
    std::atomic_int journal_capture_count{0};
    std::atomic_int completion_count{0};
    std::atomic_int completion_terminal{-1};
    std::atomic_bool completion_has_result{false};
    assert (local
              ->seal_session_remote (
                status.routing_id (), seal, 2s, std::chrono::steady_clock::now () + 2s,
                [&journal_capture_count] {
                    journal_capture_count.fetch_add (1, std::memory_order_acq_rel);
                    return std::vector<std::uint8_t>{0x51};
                },
                [&completion_count, &completion_terminal, &completion_has_result] (
                  foundation::operation_terminal_t terminal,
                  std::optional<host::session_relocation_seal_result_t> result) {
                    completion_terminal.store (static_cast<int> (terminal),
                                               std::memory_order_release);
                    completion_has_result.store (result.has_value (), std::memory_order_release);
                    completion_count.fetch_add (1, std::memory_order_acq_rel);
                })
              .result ()
              .value ());
    assert (completion_count.load (std::memory_order_acquire) == 0);
    assert (local->sessions ().remote_route_sealed (actor_object->key));

    std::atomic_int held_settlement_count{0};
    std::atomic_bool held_delivered{true};
    const auto [held_error, held_dispatch] = local->sessions ().admit_inbound (
      binding.connection.connection_id, binding.binding_generation, actor_object->key,
      ingress->inbound_sequence + 1, 0ms, [&] {
          return
            [&held_settlement_count, &held_delivered] (
              std::optional<stateful::stream_dispatch_t> delivered,
              zlink::framework::message_flow_reason_t reason,
              std::optional<zlink::framework::result_t<void>>) -> zlink::framework::task_t<void> {
                assert (reason == zlink::framework::message_flow_reason_t::target_closed);
                held_delivered.store (delivered.has_value (), std::memory_order_release);
                held_settlement_count.fetch_add (1, std::memory_order_acq_rel);
                co_return;
            };
      });
    assert (held_error == stateful::stateful_error_t::none);
    assert (!held_dispatch);

    const auto dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                              std::vector<zlink::message_t>, auto) {};
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (local->sessions ().current_binding (actor_object->key)
           && std::chrono::steady_clock::now () < deadline) {
        (void) local->dispatch_ready (dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (!local->sessions ().current_binding (actor_object->key));
    assert (!local->sessions ().remote_route_sealed (actor_object->key));
    assert (held_settlement_count.load (std::memory_order_acquire) == 1);
    assert (!held_delivered.load (std::memory_order_acquire));
    assert (journal_capture_count.load (std::memory_order_acquire) == 0);
    assert (completion_count.load (std::memory_order_acquire) == 1);
    assert (completion_terminal.load (std::memory_order_acquire)
            == static_cast<int> (foundation::operation_terminal_t::timed_out));
    assert (!completion_has_result.load (std::memory_order_acquire));
    assert (physical_close_count.load (std::memory_order_acquire) == 1);
    assert (local->sessions ().complete_inbound (*ingress) != stateful::stateful_error_t::none);

    const auto target_authority_owner_generation = actor_object->authority_owner_generation + 1;
    const protocol::session_relocation_route_t late_route{
      seal.relocation,
      seal.coordinator,
      protocol::relocation_role_t::target,
      {actor_object->key, actor_object->object_generation},
      status.routing_id ().to_bytes (),
      status.lifecycle_generation (),
      "configured-session-owner",
      19,
      session_rid,
      binding.binding_generation,
      {protocol::session_relocation_route_action_t::commit,
       actor_object->authority_owner_generation, target_authority_owner_generation,
       status.routing_id ().to_bytes (), status.lifecycle_generation (), 0, 13}};
    assert (local->route_session_remote (status.routing_id (), late_route).result ().value ());
    (void) local->dispatch_ready (dispatch);
    assert (!local->sessions ().current_binding (actor_object->key));
    assert (late_session_route_update_count.load (std::memory_order_acquire) == 1);
    assert (local->route_session_remote (status.routing_id (), late_route).result ().value ());
    (void) local->dispatch_ready (dispatch);
    assert (late_session_route_update_count.load (std::memory_order_acquire) == 2);
    assert (held_settlement_count.load (std::memory_order_acquire) == 1);

    local->close ();
    assert (held_settlement_count.load (std::memory_order_acquire) == 1);
    assert (physical_close_count.load (std::memory_order_acquire) == 1);
}
}
