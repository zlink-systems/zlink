/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_relocation_store_fixture.hpp"
#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_task.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

void test_relocation_ready_boundary (test_context_t &test, ready_boundary_case_t mode)
{
    using namespace std::chrono_literals;
    namespace f = zlink::framework;
    namespace mesh = f::runtime::mesh;
    namespace host = f::runtime::host;
    namespace p = f::runtime::protocol;
    f::runtime::install_host_context_hooks ();
    const bool expired =
      mode == ready_boundary_case_t::expired_remote || mode == ready_boundary_case_t::expired_local;
    const bool remote_session = mode != ready_boundary_case_t::expired_local;
    const bool stop_restore = mode == ready_boundary_case_t::shutdown_restore;
    const bool stop_route = mode == ready_boundary_case_t::shutdown_route;
    const auto bytes = [] (std::string_view s) {
        return std::vector<std::uint8_t> (s.begin (), s.end ());
    };
    const auto descriptor = [&] (std::string_view rid) {
        return mesh::service_node_descriptor_t{"ready-boundary-mesh",
                                               bytes (rid),
                                               1,
                                               1,
                                               "tcp://127.0.0.1:0",
                                               {},
                                               mesh::service_node_state_t::preparing};
    };
    auto authority = std::make_shared<memory_authority_store_t> ();
    host::host_options_t options;
    options.mesh.runtime_failures = std::make_shared<f::runtime::runtime_failure_collector_t> ();
    options.mesh.descriptor = descriptor ("boundary-target");
    options.object_stable_types = {"production.actor"};
    auto target = std::make_shared<host::public_host_runtime_t> (options);
    target->configure_maintenance ({authority,
                                    std::make_shared<memory_aggregate_authority_t> (authority),
                                    std::make_shared<memory_relocation_repository_t> ()});
    target->configure_session_route_owner (
      [] { return std::optional<f::location_owner_token_t>{{"target-owner", 9}}; });
    target->configure_stateful_dispatch (
      [] (accepted_record_authority_query_t)
        -> f::task_t<std::optional<accepted_record_authority_t>> { co_return std::nullopt; });
    auto restoring = std::make_shared<f::task_completion_source_t<void>> ();
    f::task_completion_source_t<bool> restore_release;
    std::atomic<unsigned> aborts{0};
    std::stop_token restore_cancellation;
    target->objects ().configure_relocation_materialization (
      [&] (const auto &, const auto &, const auto &, std::stop_token token) -> f::task_t<bool> {
          restore_cancellation = token;
          restoring->complete (f::result_t<void>::success ());
          if (stop_restore)
              co_return co_await restore_release.task ();
          co_return true;
      },
      [] (const auto &) { return true; },
      [&] (const auto &) -> f::task_t<void> {
          ++aborts;
          co_return;
      });
    mesh::raw_mesh_node_owner_t source (f::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("boundary-source")}));
    mesh::raw_mesh_node_owner_t session (f::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("boundary-session")}));
    target->start ();
    source.start ();
    session.start ();
    const auto source_node = source.topology ().local_descriptor ();
    const auto target_node = target->transport ().topology ().local_descriptor ();
    const auto session_node = session.topology ().local_descriptor ();
    auto reconciled = std::make_shared<f::task_completion_source_t<void>> ();
    std::atomic<bool> reconciled_carried_owner{true};
    std::atomic<unsigned> reconciles{0};
    target->configure_peer_reconcile ([&] (const auto &rid) {
        if (rid != session_node.node_routing_id)
            reconciled_carried_owner.store (false, std::memory_order_release);
        if (++reconciles == 1)
            reconciled->complete (f::result_t<void>::success ());
    });
    constexpr auto early_control_marker = "pre-ready-control-marker";
    auto early_control_drained = std::make_shared<f::task_completion_source_t<void>> ();
    std::atomic<bool> early_control_observed{false};
    std::atomic<bool> stop{false};
    std::thread target_dispatch ([&] {
        while (!stop.load (std::memory_order_acquire)) {
            bool saw_marker = false;
            await_task (target->dispatch_ready ([&] (const auto &, const auto &, auto parts, auto) {
                if (parts.size () == 1 && parts.front ().to_string () == early_control_marker)
                    saw_marker = true;
            }));
            if (saw_marker) {
                await_task (
                  target->dispatch_ready ([] (const auto &, const auto &, auto, auto) {}));
                early_control_observed.store (true, std::memory_order_release);
                early_control_drained->complete (f::result_t<void>::success ());
            }
            std::this_thread::yield ();
        }
    });
    const auto pump = [&] (mesh::raw_mesh_node_owner_t &node) {
        while (!stop.load (std::memory_order_acquire)) {
            (void) await_task (node.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
            std::this_thread::yield ();
        }
    };
    std::thread source_pump ([&] { pump (source); });
    std::thread session_pump ([&] { pump (session); });
    const auto await_peer = [] (mesh::service_topology_registry_t::peer_observer_t observer,
                                std::uint64_t generation) -> f::task_t<void> {
        for (;;) {
            auto observed = observer ();
            if (observed.peer && observed.peer->ready
                && observed.peer->descriptor.lifecycle_generation == generation)
                co_return;
            co_await observed.changed;
        }
    };
    (void) source.connect_peer (target->status ().local_endpoint (), target_node);
    await_task (await_peer (source.topology ().observe_peer (target_node.node_routing_id),
                            target_node.lifecycle_generation));
    await_task (
      await_peer (target->transport ().topology ().observe_peer (source_node.node_routing_id),
                  source_node.lifecycle_generation));
    const object_ref_t actor{object_kind_t::actor,  "boundary-actor", 1, 1,
                             "ready-boundary-mesh", "boundary-source"};
    {
        std::lock_guard lock (authority->mutex);
        authority->participant_identities = {
          relocation_participant_identity_t{actor, "production.actor", std::nullopt}};
    }
    const p::relocation_id_t relocation{0x1775, static_cast<std::uint64_t> (mode) + 1};
    const p::relocation_coordinator_fence_t coordinator{
      "source-owner", 1, source_node.node_routing_id, source_node.lifecycle_generation,
      "authority-v1"};
    std::vector<relocation_session_binding_t> bindings;
    if (remote_session)
        bindings.push_back ({{actor.key, actor.object_generation},
                             {0, session_node.node_routing_id, session_node.lifecycle_generation,
                              "session-owner", 17, bytes ("bound-session"), 1}});
    const auto payload = maintenance_runtime_t::encode_envelope (
      {frozen_object_state_t{actor, "production.actor", {}, {}, {}}}, relocation, 1, bindings);
    const auto manifest = plan_relocation_payload (payload, 1024);
    const p::relocation_object_t object{p::relocation_object_kind_t::actor, "production.actor",
                                        actor.key, 1, 1};
    const p::relocation_prepare_t prepare{
      relocation,
      target_node.lifecycle_generation,
      coordinator,
      {target_node.node_routing_id, target_node.lifecycle_generation, "target-owner", 9},
      p::relocation_role_t::source,
      object,
      source_node.node_routing_id,
      source_node.lifecycle_generation,
      manifest.total_length,
      manifest.chunk_count,
      manifest.checksum_crc32c,
      1,
      expired ? std::uint64_t{1}
              : static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ())};
    p::frozen_application_record_t accepted;
    accepted.kind = p::frozen_record_kind_t::actor_send;
    accepted.source_kind = p::frozen_source_kind_t::node;
    accepted.source = {"source-owner", 1, source_node.node_routing_id,
                       source_node.lifecycle_generation};
    accepted.operation = {0x1775, 9};
    accepted.body = p::frozen_actor_application_body_t{
      {actor.key, actor.object_generation, target_node.node_routing_id,
       target_node.lifecycle_generation, actor.authority_owner_generation, 9},
      p::application_payload_t::from_parts (
        {zlink::message_t::from (std::string ("early-relay"))})};
    const p::relocation_data_t relay_data{
      relocation,  target_node.lifecycle_generation,
      coordinator, p::relocation_role_t::source,
      object,      p::encode_frozen_application_record (accepted)};
    const auto relay_record = mesh::service_mailbox_record_t::from_parts (
      "boundary-target", mesh::service_mailbox_domain_t::infrastructure,
      {zlink::message_t::from (p::encode_relocation_control (relay_data))},
      source_node.node_routing_id, std::nullopt, source_node.lifecycle_generation);
    auto ready = source.request_relocation_prepare (target_node.node_routing_id, prepare, 5s);
    for (std::uint32_t ordinal = 0; ordinal != manifest.chunk_count; ++ordinal)
        (void) await_task (source.send_relocation_control (
          target_node.node_routing_id,
          make_relocation_state_chunk (relocation, target_node.lifecycle_generation, coordinator,
                                       object, payload, ordinal, 1024)));
    // The observer can run after this test returns; it owns its sources.
    f::detail::observe_task_completion (
      ready, [restoring, reconciled, early_control_drained] (const auto &) {
          restoring->complete (f::result_t<void>::success ());
          reconciled->complete (f::result_t<void>::success ());
          early_control_drained->complete (f::result_t<void>::success ());
      });
    await_task (restoring->task ());
    if (stop_restore) {
        (void) await_task (
          source.send_relocation_control (target_node.node_routing_id, relay_data));
        test.require (await_task (target->relocation_wire ().process (relay_record))
                        == raw_relocation_replay_result_t::restore_failed,
                      "pre-ready Data must not enter the pending Restore attempt");
        (void) await_task (source.send_relocation_control (
          target_node.node_routing_id,
          p::relocation_cutover_t{relocation, target_node.lifecycle_generation, coordinator,
                                  p::relocation_role_t::source, object, 0, 0}));
        (void) await_task (source.send_to_node_result (
          target_node.node_routing_id,
          p::application_payload_t::from_parts (
            {zlink::message_t::from (std::string (early_control_marker))})));
        await_task (early_control_drained->task ());
        test.require (early_control_observed.load (std::memory_order_acquire),
                      "ordered marker must follow the pre-ready Data and cutover");
        test.require (!ready.await_ready () && aborts == 0,
                      "pre-ready control must retain the pending Restore without finalization");
        std::lock_guard lock (authority->mutex);
        test.require (authority->publish_count == 0,
                      "pre-ready cutover must not submit target authority");
    }
    std::optional<f::task_t<mesh::relocation_prepare_response_t>> duplicate;
    if (!expired && !stop_restore) {
        await_task (reconciled->task ());
        test.require (reconciles != 0 && reconciled_carried_owner.load (std::memory_order_acquire),
                      "target must request reconciliation of the carried Session owner");
        test.require (!ready.await_ready (), "relay-ready must wait for the remote Session route");
        if (!stop_route) {
            duplicate.emplace (
              source.request_relocation_prepare (target_node.node_routing_id, prepare, 5s));
            auto conflict = prepare;
            --conflict.deadline_unix_ms;
            const auto rejected = await_task (
              source.request_relocation_prepare (target_node.node_routing_id, conflict, 5s));
            test.require (
              rejected.failed.has_value (),
              "a duplicate with a changed deadline must fail without replacing the attempt");
            test.require (!ready.await_ready () && !duplicate->await_ready (),
                          "original and duplicate must share the pending ready boundary");
            // Release the existing peer owner only after both requests are held.
            (void) target->connect_peer (session.endpoint (),
                                         zlink::routing_id_t::from (session_node.node_routing_id),
                                         session_node.lifecycle_generation);
        }
    }
    if (stop_restore || stop_route) {
        auto sealed = target->seal_relocation_targets ();
        if (stop_restore) {
            test.require (restore_cancellation.stop_requested (),
                          "shutdown must cancel the pending Restore");
            test.require (!sealed.await_ready (),
                          "shutdown must retain the running Restore terminal");
            restore_release.complete (f::result_t<bool>::success (true));
        }
        await_task (std::move (sealed));
    }
    const auto response = await_task (std::move (ready));
    if (expired || stop_restore || stop_route) {
        test.require (response.failed.has_value () && !response.ready,
                      "deadline or stop must send explicit failure before readiness");
        if (expired && response.failed)
            test.require (response.failed->failure_code
                            == static_cast<std::uint32_t> (p::framework_error_code::workerTimedOut),
                          "an expired source deadline must preserve DeadlineExceeded");
        test.require (aborts == 1
                        && target->objects ().inventory_async ().result ().value ().empty (),
                      "failed target readiness must discard staging exactly once");
    } else {
        test.require (response.ready.has_value (),
                      "ready must complete after Session peer admission");
        if (duplicate)
            test.require (await_task (std::move (*duplicate)).ready.has_value (),
                          "the identical duplicate must use the same ready gate");
        test.require (await_task (target->relocation_wire ().process (relay_record))
                        == raw_relocation_replay_result_t::applied,
                      "the same valid Data must be accepted after wire-ready publication");
    }
    await_task (target->seal_relocation_targets ());
    stop.store (true, std::memory_order_release);
    target_dispatch.join ();
    source_pump.join ();
    session_pump.join ();
    target->close ();
    source.close ();
    session.close ();
}

} // namespace m6c_test
