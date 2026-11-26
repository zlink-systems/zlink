/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "location_runtime_mesh_host_test_access.hpp"
#include "location_runtime_wait.hpp"
#include "runtime_failure_fixture.hpp"
#include "location_runtime_test_access.hpp"
#include "location_reclaim_clock.hpp"

#include "runtime_metric_contract.hpp"
#include "manual_deadlines.hpp"
#include "allocation_counter.hpp"
#include "manual_internal_scheduler.hpp"

#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/utils/available_processor_count.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/execution/serial_handler_participant.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include <zlink/framework/contracts/configuration/detail/framework_options_validation.hpp>

#include <zlink/framework/contracts/configuration/app.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <future>
#include <latch>
#include <stop_token>
#include <thread>

namespace zlink::framework::runtime
{
namespace client_server
{
struct client_server_location_runtime_test_access_t
{
    static bool stop_requested (const client_server_location_runtime_t &runtime)
    {
        return runtime._stop->stop_requested ();
    }
};
} // namespace client_server

namespace fanout
{
struct fanout_location_runtime_test_access_t
{
    static bool stop_requested (const fanout_location_runtime_t &runtime)
    {
        return runtime._stop.stop_requested ();
    }
};
} // namespace fanout

struct state_lane_test_access_t
{
    static std::size_t pending_work (state_lane_t &lane)
    {
        const std::lock_guard lock (lane._mailbox_mutex);
        return lane._mailbox.size ();
    }
    static bool pending_synchronous_turn (state_lane_t &lane)
    {
        const std::lock_guard lock (lane._mailbox_mutex);
        return !lane._mailbox.empty () && !lane._mailbox.front ().work;
    }
    static bool owns_turn (state_lane_t &lane)
    {
        return lane._scheduled.load (std::memory_order_acquire);
    }
};

struct auto_connect_cache_test_access_t
{
    static state_lane_t &lane (store_location_resolvers_t &cache) { return cache._lane; }

    static void seed (store_location_resolvers_t &cache)
    {
        cache._lane.run_sync ([&] {
            cache._spot_routes.try_emplace ("spot");
            cache._actor_routes.try_emplace ("actor");
        });
    }
    static void expect_recovered (store_location_resolvers_t &cache)
    {
        const auto snapshot = cache._lane.run_sync ([&] {
            return std::tuple (cache._spot_routes.size (), cache._actor_routes.size (),
                               cache._store_recovery_generation);
        });
        EXPECT_EQ (std::make_tuple (std::size_t{0}, std::size_t{0}, std::uint64_t{1}), snapshot);
    }
};

// Forward all Store operations to the real provider; inject one scan failure only.
class auto_connect_probe_store_t final : public location_store_t
{
  public:
    in_memory_location_store_t provider;
    bool fail_next_scan = false;
    unsigned reads = 0, writes = 0, scans = 0, failures = 0;

    task_t<store_read_result_t> read (store_key_t key) override
    {
        ++reads;
        return provider.read (std::move (key));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        ++writes;
        return provider.write (std::move (request));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        ++scans;
        if (std::exchange (fail_next_scan, false)) {
            ++failures;
            return task_t<store_scan_result_t> (result_t<store_scan_result_t>::failure (
              framework_error_kind_t::internal_failure, "injected one-worker Store scan failure"));
        }
        return provider.scan (std::move (request));
    }
};

struct location_auto_connect_test_access_t
{
    struct provider_probe_t
    {
        auto_connect_probe_store_t &provider;
        provider_location_repository_t &store;
        location_runtime_t &runtime;
        detail::route_channel_runtime_t &route;
        mesh_node_descriptor_t peer;
        unsigned connects = 0, disconnects = 0, ticks = 0;
        bool later_wait_ready = false, saw_failure = false;
    };

    static task_t<void>
    provider_probe_update (location_auto_connect_host_service_t *service,
                           provider_probe_t *probe,
                           location_auto_connect_host_service_t::target_t target,
                           bool remove)
    {
        co_await service->update_route_target (service, &probe->route, {}, target, remove);
        if (remove) {
            ++probe->disconnects;
            EXPECT_EQ (location_write_status_t::stored,
                       (co_await probe->store.update_mesh_node (probe->peer,
                                                                location_write_intent_t::new_claim))
                         .status);
            probe->provider.fail_next_scan = true;
        } else if (++probe->connects == 1) {
            EXPECT_EQ (location_write_status_t::stored,
                       co_await probe->store.remove_mesh_node (
                         {probe->peer.mesh_name, probe->peer.rid},
                         {probe->peer.owner_id, probe->peer.lease_generation}));
        }
        // Request reconciliation before run_loop reaches its wait. This exercises
        // the ready lease-change path without a periodic timer or a sleep.
        probe->runtime.peer_reconcile_requester () ({});
        if (probe->ticks == 1) {
            probe->later_wait_ready =
              probe->runtime.wait_for_owner_lease_change (probe->runtime.owner_lease_observation ())
                .await_ready ();
            probe->runtime.peer_reconcile_requester () ({});
        }
    }

    static void configure_provider_probe (location_auto_connect_host_service_t &service,
                                          provider_probe_t &probe,
                                          live_location_reader_t &reader,
                                          store_location_resolvers_t &cache)
    {
        service._runtime = &probe.runtime;
        service._store = &probe.store;
        service._live_store = &reader;
        service._route_cache = &cache;
        service._republish_after_store_recovery = [&service, &probe] (auto, auto, auto) {
            ++probe.ticks;
            if (probe.ticks == 5)
                service._stop_requested.request_stop ();
            if (probe.ticks == 3) {
                probe.saw_failure = has_tick_failure (service);
                probe.runtime.peer_reconcile_requester () ({});
            }
            return task_t<bool> (result_t<bool>::success (true));
        };
        service.add_loop (
          "l4-mesh", std::nullopt, {}, std::nullopt, false,
          [&service, &probe] (auto target) {
              return provider_probe_update (&service, &probe, std::move (target), false);
          },
          [&service, &probe] (auto target) {
              return provider_probe_update (&service, &probe, std::move (target), true);
          },
          [&service, &probe] (const auto &target) {
              service.disconnect_route_target (&service, probe.route, {}, target);
          });
    }

    static location_auto_connect_host_service_t::target_t peer_target ()
    {
        location_auto_connect_host_service_t::target_t target;
        target.node_rid = zlink::routing_id_t::from ("remote");
        target.endpoint = "inproc://l4-peer";
        target.lifecycle_generation = 1;
        target.security_identity = "default";
        return target;
    }

    static task_t<void> route_target (location_auto_connect_host_service_t &service,
                                      detail::route_channel_runtime_t &route,
                                      bool remove)
    {
        return service.update_route_target (&service, &route, {}, peer_target (), remove);
    }

    static task_t<void> mesh_target (std::shared_ptr<detail::mesh_node_runtime_t> node, bool remove)
    {
        return location_auto_connect_host_service_t::update_mesh_target (std::move (node),
                                                                         peer_target (), remove);
    }

    static void register_stop_probe (location_auto_connect_host_service_t &service,
                                     detail::route_channel_runtime_t &route,
                                     std::shared_ptr<detail::mesh_node_runtime_t> node,
                                     bool mesh)
    {
        if (mesh)
            service.add_mesh_loop (node);
        else
            service.add_route_loop (route, std::nullopt, std::nullopt, false, {});
        service._loops.front ().active.emplace ("remote", peer_target ());
    }

    static task_t<void> finish_probe (location_auto_connect_host_service_t *service,
                                      detail::route_channel_runtime_t *route,
                                      location_auto_connect_host_service_t::target_t target)
    {
        co_await service->update_route_target (service, route, {}, std::move (target), false);
        service->_stop_requested.request_stop ();
    }

    static void check_stop_between_targets (location_auto_connect_host_service_t &service,
                                            int operation)
    {
        auto completion = std::make_shared<task_completion_source_t<void>> ();
        unsigned calls = 0;
        std::map<std::string, unsigned> disconnects;
        service.add_loop (
          "l4-mesh", std::nullopt, {}, std::nullopt, false,
          [&] (auto) {
              ++calls;
              return completion->task ();
          },
          [&] (auto target) {
              ++calls;
              ++disconnects[target.endpoint];
              return completion->task ();
          },
          [&] (const auto &target) { ++disconnects[target.endpoint]; });
        auto &loop = service._loops.front ();
        auto sources = std::make_shared<live_location_reader_t::mesh_sources_t> ();
        for (auto name : {"remote", "remote-two"}) {
            auto target = peer_target ();
            target.key = name;
            target.node_rid = zlink::routing_id_t::from (name);
            auto descriptor = std::make_shared<mesh_node_descriptor_t> ();
            descriptor->mesh_name = "l4-mesh";
            descriptor->rid = target.node_rid;
            descriptor->endpoint = target.endpoint + name;
            descriptor->owner_id = "remote-owner";
            descriptor->lease_generation = 1;
            descriptor->lifecycle_generation = 1;
            descriptor->state = framework_runtime_state_t::serving;
            target.original = descriptor;
            target.endpoint = descriptor->endpoint;
            sources->push_back (
              {*descriptor, std::chrono::steady_clock::now () + std::chrono::hours (1)});
        }
        if (operation != 0)
            loop.active = service.compute_desired (loop, sources);
        if (operation == 1)
            sources->clear ();
        if (operation == 2)
            for (auto &source : *sources)
                source.descriptor.endpoint += "-replacement";
        auto pending = service.finish_loop (service.reconcile (&loop, sources, true));
        ASSERT_FALSE (pending.await_ready ());
        ASSERT_EQ (1u, calls);
        service._stop_requested.request_stop ();
        // Stop waits for the loop before cleaning active targets. Finish the
        // owner operation first, then model that cleanup after its effect is recorded.
        completion->complete (result_t<void>::success ());
        const auto result = pending.result_for (std::chrono::seconds (5));
        ASSERT_TRUE (result);
        EXPECT_TRUE (*result);
        EXPECT_EQ (1u, calls);
        EXPECT_TRUE (loop.active.empty ());
        EXPECT_EQ (operation == 0 ? 1u : 2u, disconnects.size ());
        for (const auto &[endpoint, count] : disconnects) {
            SCOPED_TRACE (endpoint);
            EXPECT_EQ (1u, count);
        }
    }

    static void check_stop_after_pending_connect (location_auto_connect_host_service_t &service,
                                                  location_runtime_t &runtime,
                                                  detail::route_channel_runtime_t &route)
    {
        service._runtime = &runtime;
        service._loop_finished = std::make_shared<task_completion_source_t<void>> ();
        unsigned disconnects = 0;
        service.add_loop (
          "l4-mesh", std::nullopt, {}, std::nullopt, false,
          [&service, &route] (auto target) {
              return service.update_route_target (&service, &route, {}, std::move (target), false);
          },
          [&service, &route] (auto target) {
              return service.update_route_target (&service, &route, {}, std::move (target), true);
          },
          [&service, &route, &disconnects] (const auto &target) {
              ++disconnects;
              service.disconnect_route_target (&service, route, {}, target);
          });

        auto &loop = service._loops.front ();
        auto target = peer_target ();
        auto descriptor = std::make_shared<mesh_node_descriptor_t> ();
        descriptor->mesh_name = "l4-mesh";
        descriptor->rid = target.node_rid;
        descriptor->endpoint = target.endpoint;
        descriptor->owner_id = "remote-owner";
        descriptor->lease_generation = 1;
        descriptor->lifecycle_generation = 1;
        descriptor->state = framework_runtime_state_t::serving;
        target.key = descriptor->rid.to_string ();
        target.original = descriptor;
        loop.last_desired.emplace (target.key, target);
        auto sources = std::make_shared<live_location_reader_t::mesh_sources_t> ();
        sources->push_back (
          {*descriptor, std::chrono::steady_clock::now () + std::chrono::hours (1)});

        std::promise<void> owner_entered, release_owner;
        auto released = release_owner.get_future ().share ();
        auto &owner = auto_connect_owner_lane (route);
        auto held_owner = std::async (std::launch::async, [&] {
            owner.run_sync ([&] {
                owner_entered.set_value ();
                released.wait ();
            });
        });
        owner_entered.get_future ().wait ();

        auto pending = service.reconcile (&loop, sources, true);
        const auto owner_deadline = std::chrono::steady_clock::now () + auto_connect_wait;
        while (state_lane_test_access_t::pending_work (owner) == 0
               && std::chrono::steady_clock::now () < owner_deadline)
            std::this_thread::yield ();
        EXPECT_GT (state_lane_test_access_t::pending_work (owner), 0u);
        EXPECT_FALSE (pending.await_ready ());

        auto finished = service._loop_finished;
        auto loop_runner =
          std::async (std::launch::async, [pending = std::move (pending), finished] () mutable {
              const auto result = pending.result_for (auto_connect_wait);
              if (result)
                  finished->complete (result_t<void>::success ());
              else
                  finished->complete (result_t<void>::failure (
                    framework_error_kind_t::deadline_exceeded, "test reconcile did not finish"));
          });
        auto stopping = std::async (std::launch::async, [&] { service.stop (); });
        const auto stop_deadline = std::chrono::steady_clock::now () + auto_connect_wait;
        while (!service._stop_requested.stop_requested ()
               && std::chrono::steady_clock::now () < stop_deadline)
            std::this_thread::yield ();
        EXPECT_TRUE (service._stop_requested.stop_requested ());

        release_owner.set_value ();
        held_owner.get ();
        EXPECT_EQ (std::future_status::ready, loop_runner.wait_for (auto_connect_wait));
        loop_runner.get ();
        EXPECT_EQ (std::future_status::ready, stopping.wait_for (auto_connect_wait));
        stopping.get ();
        EXPECT_EQ (1u, disconnects);
        EXPECT_TRUE (route.list_connections ().empty ());
    }

    static task_t<void>
    pending_target (detail::route_channel_runtime_t *route,
                    location_auto_connect_host_service_t::target_t target,
                    std::shared_ptr<task_completion_source_t<void>> release,
                    bool remove,
                    std::shared_ptr<std::map<std::string, unsigned>> disconnects)
    {
        co_await release->task ();
        if (remove) {
            if (disconnects)
                ++(*disconnects)[target.endpoint];
            (void) route->disconnect (target.node_rid, target.endpoint);
        } else
            (void) route->connect (target.node_rid, target.endpoint);
    }

    static auto
    start_timeout_probe (location_auto_connect_host_service_t &service,
                         location_runtime_t &runtime,
                         detail::route_channel_runtime_t &route,
                         std::shared_ptr<task_completion_source_t<void>> release,
                         int operation,
                         std::shared_ptr<std::map<std::string, unsigned>> disconnects = {})
    {
        service._runtime = &runtime;
        service._loop_finished = std::make_shared<task_completion_source_t<void>> ();
        service.add_loop (
          "l4-mesh", std::nullopt, {}, std::nullopt, false,
          [&route, release, disconnects] (auto target) {
              return pending_target (&route, target, release, false, disconnects);
          },
          [&route, release, disconnects] (auto target) {
              return pending_target (&route, target, release, true, disconnects);
          },
          [&service, &route, disconnects] (const auto &target) {
              if (disconnects)
                  ++(*disconnects)[target.endpoint];
              service.disconnect_route_target (&service, route, {}, target);
          });
        auto &loop = service._loops.front ();
        auto descriptor = std::make_shared<mesh_node_descriptor_t> ();
        descriptor->mesh_name = "l4-mesh";
        descriptor->rid = zlink::routing_id_t::from ("remote");
        descriptor->endpoint = "inproc://l4-timeout";
        descriptor->owner_id = "remote-owner";
        descriptor->lease_generation = 1;
        descriptor->lifecycle_generation = 1;
        descriptor->state = framework_runtime_state_t::serving;
        auto sources = std::make_shared<live_location_reader_t::mesh_sources_t> ();
        sources->push_back (
          {*descriptor, std::chrono::steady_clock::now () + std::chrono::hours (1)});
        if (operation != 0) {
            loop.active = service.compute_desired (loop, sources);
            (void) route.connect (descriptor->rid, descriptor->endpoint);
        }
        if (operation == 1)
            sources->clear ();
        if (operation == 2)
            sources->front ().descriptor.endpoint += "-replacement";
        auto pending = service.finish_loop (service.reconcile (&loop, sources, true));
        EXPECT_FALSE (pending.await_ready ());
        auto finished = service._loop_finished;
        detail::observe_task_terminal (
          pending, [finished] (const result_t<void> &result) { finished->complete (result); });
        return finished->task ();
    }

    static std::size_t loop_count (const location_auto_connect_host_service_t &service)
    {
        return service._loops.size ();
    }

    static void configure_probe (location_auto_connect_host_service_t &service,
                                 location_runtime_t &runtime,
                                 location_repository_t &store,
                                 live_location_reader_t &reader,
                                 detail::route_channel_runtime_t &route,
                                 unsigned &calls,
                                 bool later,
                                 store_location_resolvers_t *cache)
    {
        service._runtime = &runtime;
        service._store = &store;
        service._live_store = &reader;
        service._route_cache = cache;
        service.add_loop (
          "l4-mesh", std::nullopt, {}, std::nullopt, false,
          [&service, &route, &calls, &runtime, later, cache] (const auto &target) {
              ++calls;
              if (cache) {
                  service._stop_requested.request_stop ();
                  return task_t<void> (result_t<void>::success ());
              }
              if (later && calls == 1) {
                  runtime.peer_reconcile_requester () ({});
                  return task_t<void> (result_t<void>::success ());
              }
              return finish_probe (&service, &route, target);
          },
          [] (const auto &) { return task_t<void> (result_t<void>::success ()); },
          [] (const auto &) {});
        service._loops.front ().recovering_from_store_failure = cache != nullptr;
    }

    static task_t<bool> publish_without_start (location_auto_connect_host_service_t &service,
                                               location_runtime_t &runtime)
    {
        service._runtime = &runtime;
        return service.republish_after_store_recovery (location_write_intent_t::renew);
    }

    static task_t<void> run_probe (location_auto_connect_host_service_t &service)
    {
        return service.run_loop (service._runtime->owner_lease_observation ());
    }

    static bool has_tick_failure (const location_auto_connect_host_service_t &service)
    {
        return service._loops.front ().failure_started_at.has_value ();
    }

    static auto hold_loop_completion (location_auto_connect_host_service_t &service,
                                      location_runtime_t &runtime)
    {
        service._runtime = &runtime;
        service._loop_finished = std::make_shared<task_completion_source_t<void>> ();
        return service._loop_finished;
    }

    static void add_test_loop (location_auto_connect_host_service_t &service,
                               std::function<void (std::string)> connected,
                               std::function<void ()> disconnected,
                               bool refresh_capacity = false)
    {
        service.add_loop (
          "ordered-mesh",
          refresh_capacity ? std::optional{zlink::routing_id_t::from ("local")} : std::nullopt, {},
          std::nullopt, false,
          [connected] (const auto &target) {
              connected (target.node_rid.to_string ());
              return task_t<void> (result_t<void>::success ());
          },
          [disconnected] (const auto &) {
              disconnected ();
              return task_t<void> (result_t<void>::success ());
          },
          [disconnected] (const auto &) { disconnected (); });
    }
};

struct production_auto_fixture_t
{
    decltype (test::runtime_failure_builder ()) builder{test::runtime_failure_builder ()};
    handler_registry_t handlers;
    serializer_registry_t serializers;
    std::shared_ptr<runtime_failure_collector_t> failures =
      std::make_shared<runtime_failure_collector_t> ();
    location_auto_connect_host_service_t service;

    explicit production_auto_fixture_t (
      std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> nodes = {}) :
        service (
          failures,
          builder.message_bus (),
          {},
          handlers,
          serializers,
          [] { return framework_runtime_state_t::serving; },
          {},
          {},
          {},
          std::move (nodes))
    {
    }
};
} // namespace zlink::framework::runtime
