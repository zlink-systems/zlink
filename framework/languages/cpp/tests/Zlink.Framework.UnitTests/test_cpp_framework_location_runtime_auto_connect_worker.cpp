/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "../support/location_runtime_test_access.hpp"
#include "../support/location_reclaim_clock.hpp"

#include "../support/runtime_metric_contract.hpp"
#include "../support/manual_deadlines.hpp"
#include "../support/allocation_counter.hpp"
#include "../support/manual_internal_scheduler.hpp"

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
#include "../support/location_runtime_auto_connect_fixture.hpp"

namespace
{
using namespace zlink::framework;
using namespace zlink::framework::runtime;

template <typename Cache> task_t<void> recover_auto_cache (Cache *cache)
{
    if constexpr (requires { cache->invalidate_all_routes_after_store_recovery_async (); })
        co_await cache->invalidate_all_routes_after_store_recovery_async ();
    else
        cache->invalidate_all_routes_after_store_recovery ();
    co_return;
}

template <typename Raw> task_t<void> raw_auto_update (Raw *raw, int operation)
{
    const auto rid = zlink::routing_id_t::from ("remote");
    mesh::service_node_descriptor_t descriptor;
    descriptor.node_routing_id = rid.to_bytes ();
    descriptor.advertised_endpoint = "inproc://l4-peer";
    if constexpr (requires { raw->expect_peer_async (descriptor); }) {
        if (operation == 0)
            co_await raw->connect_peer_intent_async ("inproc://l4-peer", rid, 1, "default", true);
        else if (operation == 1)
            co_await raw->disconnect_peer_intent_async (rid, "inproc://l4-peer", true);
        else if (operation == 2)
            co_await raw->expect_peer_async (descriptor);
        else
            co_await raw->forget_peer_async (rid.to_bytes (), "inproc://l4-peer");
    } else {
        if (operation == 0)
            raw->connect_peer_intent ("inproc://l4-peer", rid, 1, "default", true);
        else if (operation == 1)
            raw->disconnect_peer_intent (rid, "inproc://l4-peer", true);
        else if (operation == 2)
            raw->expect_peer (descriptor);
        else
            raw->forget_peer (rid.to_bytes (), "inproc://l4-peer");
    }
    co_return;
}

template <typename Node> task_t<void> auto_mesh_name (Node *node)
{
    if constexpr (requires {
                      node->connect_peer_async (zlink::routing_id_t::from ("remote"), std::string{},
                                                1, std::string{}, true);
                  }) {
        const auto identity = co_await node->identity_async ();
        EXPECT_EQ ("l4-mesh", identity.first);
    } else {
        EXPECT_EQ ("l4-mesh", node->mesh_name ());
    }
    co_return;
}

// Occupy the owner from an external caller, leaving exactly one infrastructure
// worker available. Submission must return a pending task and free that worker.
void check_auto_owner_wait (state_lane_t &lane, std::function<task_t<void> ()> work)
{
    auto executor = infrastructure_executor ();
    auto workers = test::reserve_single_infrastructure_worker (*executor);
    std::promise<void> entered, release, returned, probe;
    auto released = release.get_future ().share ();
    auto held = std::async (std::launch::async, [&] {
        lane.run_sync ([&] {
            entered.set_value ();
            released.wait ();
        });
    });
    entered.get_future ().get ();
    auto terminal = std::make_shared<task_completion_source_t<void>> ();
    executor->submit ([&, terminal] {
        auto pending = work ();
        EXPECT_FALSE (pending.await_ready ());
        detail::observe_task_terminal (
          pending, [terminal] (const result_t<void> &result) { terminal->complete (result); });
        returned.set_value ();
    });
    auto return_status = returned.get_future ().wait_for (auto_connect_wait);
    executor->submit ([&] { probe.set_value (); });
    auto probe_status = probe.get_future ().wait_for (auto_connect_wait);
    release.set_value ();
    held.get ();
    EXPECT_EQ (std::future_status::ready, return_status);
    EXPECT_EQ (std::future_status::ready, probe_status);
    auto result = terminal->task ().result_for (auto_connect_wait);
    ASSERT_TRUE (result);
    EXPECT_TRUE (*result);
}

void check_auto_route (bool remove)
{
    detail::route_channel_runtime_t route ("l4-mesh");
    if (remove)
        ASSERT_TRUE (route.connect (zlink::routing_id_t::from ("remote"), "inproc://l4-peer"));
    production_auto_fixture_t fixture;
    check_auto_owner_wait (auto_connect_owner_lane (route), [&] {
        return location_auto_connect_test_access_t::route_target (fixture.service, route, remove);
    });
    EXPECT_EQ (remove ? 0u : 1u, route.list_connections ().size ());
}

void check_auto_raw (int operation)
{
    mesh::raw_mesh_node_options_t options;
    options.descriptor = {"l4-mesh", zlink::routing_id_t::from ("local").to_bytes (), 1, 1,
                          "inproc://l4-local"};
    mesh::raw_mesh_node_owner_t raw (test::runtime_failure_options (std::move (options)));
    check_auto_owner_wait (auto_connect_owner_lane (raw),
                           [&] { return raw_auto_update (&raw, operation); });
}
void check_production_identity ()
{
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
    auto node = std::make_shared<detail::mesh_node_runtime_t> (state);
    production_auto_fixture_t fixture ({node});
    detail::route_channel_runtime_t route ("l4-mesh");
    check_auto_owner_wait (state->lane, [&] {
        return location_auto_connect_test_access_t::route_target (fixture.service, route, false);
    });
}

void check_mesh_and_host_async_wrappers ()
{
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
    state->routing_id = zlink::routing_id_t::from ("local");
    state->core_context = std::make_shared<zlink::context_t> ();
    auto node = std::make_shared<detail::mesh_node_runtime_t> (state);
    node->start ().result ().value ();
    auto &host = node->native_node ();
    auto &raw_lane = auto_connect_owner_lane (host.transport ());
    const auto rid = zlink::routing_id_t::from ("remote");
    const std::string endpoint = "inproc://l4-peer";
    const std::pair<const char *, std::function<task_t<void> ()>> wrappers[] = {
      {"mesh.connect_peer_async",
       [&] { return node->connect_peer_async (rid, endpoint, 1, "default", true); }},
      {"mesh.expect_peer_async",
       [&] { return node->expect_peer_async (rid, endpoint, 1, "default"); }},
      {"mesh.forget_peer_async", [&] { return node->forget_peer_async (rid, endpoint); }},
      {"mesh.disconnect_peer_async.raw",
       [&] { return node->disconnect_peer_async (rid, endpoint, true); }},
      {"host.expect_peer_async",
       [&] { return host.expect_peer_async (endpoint, rid, 1, "default"); }},
      {"host.forget_peer_async", [&] { return host.forget_peer_async (endpoint, rid); }}};
    for (const auto &[name, work] : wrappers) {
        SCOPED_TRACE (name);
        check_auto_owner_wait (raw_lane, work);
    }
    check_auto_owner_wait (state->lane,
                           [&] { return node->disconnect_peer_async (rid, endpoint, true); });
    production_auto_fixture_t fixture ({node});
    detail::route_channel_runtime_t route ("l4-mesh");
    for (bool remove : {false, true}) {
        SCOPED_TRACE (remove);
        check_auto_owner_wait (raw_lane, [&] {
            return location_auto_connect_test_access_t::route_target (fixture.service, route,
                                                                      remove);
        });
        check_auto_owner_wait (raw_lane, [&] {
            return location_auto_connect_test_access_t::mesh_target (node, remove);
        });
    }
    node->stop ();
}

class ready_auto_store_t : public in_memory_location_repository_t
{
  public:
    task_t<location_page_t<mesh_node_descriptor_t>>
    list_mesh_nodes_core (std::string, location_page_request_t) override
    {
        mesh_node_descriptor_t peer;
        peer.mesh_name = "l4-mesh";
        peer.rid = zlink::routing_id_t::from ("remote");
        peer.endpoint = "inproc://l4-peer";
        peer.owner_id = "remote-owner";
        peer.lease_generation = 1;
        peer.lifecycle_generation = 1;
        location_page_t<mesh_node_descriptor_t> page;
        page.items.push_back (std::move (peer));
        return task_t<location_page_t<mesh_node_descriptor_t>> (
          result_t<location_page_t<mesh_node_descriptor_t>>::success (std::move (page)));
    }
    task_t<owner_lease_read_result_t> read_owner_lease (std::string owner) override
    {
        const auto now = std::chrono::system_clock::now ();
        return task_t<owner_lease_read_result_t> (result_t<owner_lease_read_result_t>::success (
          owner_lease_found_t{{std::move (owner), 1}, now + std::chrono::hours (3), now}));
    }
};

void check_auto_loop_ready (bool later, bool recovery)
{
    ready_auto_store_t store;
    location_options_t options;
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    options.polling_interval = std::chrono::hours (1);
    location_runtime_t runtime (store, options, "local-owner");
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    runtime.start (zlink::routing_id_t::from ("local"));
    live_location_reader_t reader (store, options);
    store_location_resolvers_t cache (store);
    if (recovery)
        auto_connect_cache_test_access_t::seed (cache);
    detail::route_channel_runtime_t route ("l4-mesh");
    auto builder = test::runtime_failure_builder ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    auto failures = std::make_shared<runtime_failure_collector_t> ();
    location_auto_connect_host_service_t service (
      failures, builder.message_bus (), {}, handlers, serializers,
      [] { return framework_runtime_state_t::serving; }, {}, {}, {}, {});
    unsigned calls = 0;
    location_auto_connect_test_access_t::configure_probe (
      service, runtime, store, reader, route, calls, later, recovery ? &cache : nullptr);
    check_auto_owner_wait (
      recovery ? auto_connect_cache_test_access_t::lane (cache) : auto_connect_owner_lane (route),
      [&] { return location_auto_connect_test_access_t::run_probe (service); });
    EXPECT_EQ (later ? 2u : 1u, calls);
    if (recovery)
        auto_connect_cache_test_access_t::expect_recovered (cache);
    runtime.stop ();
    failures->rethrow_if_failed ();
}

void check_provider_auto_loop (bool hold_route)
{
    auto executor = infrastructure_executor ();
    ASSERT_EQ (1u, executor->live_worker_count ());
    auto_connect_probe_store_t provider;
    provider_location_repository_t store (provider);
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    location_options_t options;
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    options.polling_interval = std::chrono::hours (1);
    const auto claim =
      store.claim_owner_lease ("remote-owner", options.owner_lease_ttl).result ().value ();
    const auto token = std::get<owner_lease_claimed_t> (claim).token;
    mesh_node_descriptor_t peer;
    peer.mesh_name = "l4-mesh";
    peer.rid = zlink::routing_id_t::from ("remote");
    peer.endpoint = "inproc://l4-peer";
    peer.owner_id = token.owner_id;
    peer.lease_generation = token.lease_generation;
    peer.lifecycle_generation = 1;
    peer.descriptor_revision = 1;
    peer.state = framework_runtime_state_t::serving;
    ASSERT_EQ (
      location_write_status_t::stored,
      store.update_mesh_node (peer, location_write_intent_t::new_claim).result ().value ().status);
    location_runtime_t runtime (store, options, "local-owner");
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    runtime.start (zlink::routing_id_t::from ("local"));
    live_location_reader_t reader (store, options);
    store_location_resolvers_t cache (store);
    auto_connect_cache_test_access_t::seed (cache);
    detail::route_channel_runtime_t route ("l4-mesh");
    auto builder = test::runtime_failure_builder ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    auto failures = std::make_shared<runtime_failure_collector_t> ();
    location_auto_connect_host_service_t service (
      failures, builder.message_bus (), {}, handlers, serializers,
      [] { return framework_runtime_state_t::serving; });
    location_auto_connect_test_access_t::provider_probe_t probe{provider, store, runtime, route,
                                                                peer};
    location_auto_connect_test_access_t::configure_provider_probe (service, probe, reader, cache);
    if (hold_route) {
        check_auto_owner_wait (auto_connect_owner_lane (route), [&] {
            return location_auto_connect_test_access_t::run_probe (service);
        });
    } else {
        auto terminal = std::make_shared<task_completion_source_t<void>> ();
        executor->submit ([&service, terminal] {
            auto pending = location_auto_connect_test_access_t::run_probe (service);
            detail::observe_task_terminal (
              pending, [terminal] (const result_t<void> &result) { terminal->complete (result); });
        });
        const auto result = terminal->task ().result_for (auto_connect_wait);
        ASSERT_TRUE (result);
        EXPECT_TRUE (*result);
    }
    EXPECT_EQ (2u, probe.connects);
    EXPECT_EQ (1u, probe.disconnects);
    EXPECT_EQ (5u, probe.ticks);
    EXPECT_TRUE (probe.later_wait_ready);
    EXPECT_TRUE (probe.saw_failure);
    EXPECT_FALSE (location_auto_connect_test_access_t::has_tick_failure (service));
    EXPECT_EQ (1u, provider.failures);
    EXPECT_GE (provider.scans, 4u);
    EXPECT_GT (provider.reads, 0u);
    EXPECT_GT (provider.writes, 0u);
    EXPECT_EQ (1u, route.list_connections ().size ());
    auto_connect_cache_test_access_t::expect_recovered (cache);
    service.stop ();
    EXPECT_TRUE (route.list_connections ().empty ());
    runtime.stop ();
    failures->rethrow_if_failed ();
}

template <typename Node> task_t<void> auto_manual_disconnect (Node *node, zlink::routing_id_t rid)
{
    if constexpr (requires { node->disconnect_peer_async (rid, std::string{}, true); })
        co_await node->disconnect_peer_async (rid, "inproc://l4-manual", true);
    else
        node->disconnect_peer (rid, "inproc://l4-manual", true);
    co_return;
}

void check_auto_manual ()
{
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
    state->routing_id = zlink::routing_id_t::from ("local");
    state->core_context = std::make_shared<zlink::context_t> ();
    const auto rid = zlink::routing_id_t::from ("remote");
    state->peer_connections.push_back (
      {.expected_routing_id = rid, .endpoint = "inproc://l4-manual"});
    detail::mesh_node_runtime_t node (state);
    node.start ().result ().value ();
    auto &topology = node.native_node ().transport ().topology ();
    auto peer = topology.local_descriptor ();
    peer.node_routing_id = rid.to_bytes ();
    peer.advertised_endpoint = "inproc://l4-manual";
    peer.state = mesh::service_node_state_t::serving;
    ASSERT_EQ (mesh::peer_admission_result_t::admitted, topology.admit (peer, {1}));
    // Manual endpoint remains configured even when discovery forgets its descriptor.
    check_auto_owner_wait (state->lane, [&] { return auto_manual_disconnect (&node, rid); });
    EXPECT_NE (nullptr, topology.peer (rid.to_bytes ()));
    EXPECT_EQ (1u, state->peer_connections.size ());
    EXPECT_EQ ("inproc://l4-manual", state->peer_connections.front ().endpoint);
    node.stop ();
}

task_t<void> disconnect_and_check_notification (mesh::raw_mesh_node_owner_t *raw, unsigned *changes)
{
    co_await raw_auto_update (raw, 1);
    EXPECT_EQ (1u, *changes);
}

void check_auto_notifications ()
{
    mesh::raw_mesh_node_options_t options;
    options.descriptor = {"l4-mesh", zlink::routing_id_t::from ("local").to_bytes (), 1, 1,
                          "inproc://l4-local"};
    mesh::raw_mesh_node_owner_t raw (test::runtime_failure_options (std::move (options)));
    raw.start ();
    const auto rid = zlink::routing_id_t::from ("remote");
    raw.connect_peer_intent ("inproc://l4-peer", rid, 1, "default", true);
    auto peer = raw.topology ().local_descriptor ();
    peer.node_routing_id = rid.to_bytes ();
    peer.advertised_endpoint = "inproc://l4-peer";
    peer.state = mesh::service_node_state_t::serving;
    ASSERT_EQ (mesh::peer_admission_result_t::admitted,
               raw.admit_peer (peer, {1}, mesh::service_liveness_registry_t::clock_t::now ()));
    unsigned changes = 0;
    raw.topology ().set_change_handler ([&] {
        EXPECT_EQ (nullptr, state_lane_t::current ());
        EXPECT_TRUE (raw.topology ().peers ().empty ());
        ++changes;
    });
    check_auto_owner_wait (auto_connect_owner_lane (raw),
                           [&] { return disconnect_and_check_notification (&raw, &changes); });
    EXPECT_EQ (1u, changes);
    EXPECT_EQ (nullptr, raw.topology ().peer (rid.to_bytes ()));
    raw.topology ().set_change_handler ({});
    raw.close ();
}

void check_auto_stale_peer ()
{
    detail::route_channel_runtime_t route ("l4-mesh");
    const auto old_rid = zlink::routing_id_t::from ("old");
    const auto replacement = zlink::routing_id_t::from ("remote");
    ASSERT_TRUE (route.connect (replacement, "inproc://l4-peer"));
    const auto removed = route.disconnect_async (old_rid, "inproc://l4-peer").result ().value ();
    EXPECT_FALSE (removed);
    const auto targets = route.list_connection_targets ();
    ASSERT_EQ (1u, targets.size ());
    EXPECT_EQ (replacement, targets.front ().peer_rid);
}

}

TEST (AutoConnectLaneDeathTest, OneWorkerProviderBackedStoreLoop)
{
#if defined(__linux__)
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT (
      {
          // The process affinity is the existing shared-pool size input. A fresh
          // death-test child has no pool yet, so this creates exactly one worker.
          cpu_set_t available;
          CPU_ZERO (&available);
          if (sched_getaffinity (getpid (), sizeof (available), &available) != 0)
              std::_Exit (2);
          int cpu = 0;
          while (cpu < CPU_SETSIZE && !CPU_ISSET (cpu, &available))
              ++cpu;
          cpu_set_t single;
          CPU_ZERO (&single);
          CPU_SET (cpu, &single);
          if (sched_setaffinity (getpid (), sizeof (single), &single) != 0)
              std::_Exit (2);
          check_provider_auto_loop (false);
          check_provider_auto_loop (true);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
#else
    GTEST_SKIP () << "One-worker probe uses the Linux process-affinity pool-size input";
#endif
}

TEST (AutoConnectLaneDeathTest, ProductionRouteCallbackReleasesIdentityWorker)
{
    ASSERT_EXIT (
      {
          check_production_identity ();
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (AutoConnectLaneDeathTest, MeshAndHostAsyncWrappersReleaseWorker)
{
    ASSERT_EXIT (
      {
          check_mesh_and_host_async_wrappers ();
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (AutoConnectLaneDeathTest, RouteConnectionAddReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_route (false);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RouteConnectionRemoveReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_route (true);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RecoveryCacheReleasesWorker)
{
    ASSERT_EXIT (
      {
          in_memory_location_repository_t store;
          store_location_resolvers_t cache (store);
          check_auto_owner_wait (auto_connect_cache_test_access_t::lane (cache),
                                 [&] { return recover_auto_cache (&cache); });
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, MeshIdentityReleasesWorker)
{
    ASSERT_EXIT (
      {
          auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
          detail::mesh_node_runtime_t node (state);
          check_auto_owner_wait (state->lane, [&] { return auto_mesh_name (&node); });
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RawConnectIntentReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_raw (0);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RawDisconnectIntentReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_raw (1);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RawExpectationReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_raw (2);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RawForgetReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_raw (3);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (AutoConnectLaneDeathTest, FirstIterationAllReadyReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_loop_ready (false, false);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, LaterReadyLeaseChangeReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_loop_ready (true, false);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, RecoveryIterationReleasesCacheWorker)
{
    ASSERT_EXIT (
      {
          check_auto_loop_ready (false, true);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, ManualRouteSnapshotReleasesWorker)
{
    ASSERT_EXIT (
      {
          check_auto_manual ();
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (AutoConnectLaneDeathTest, DisconnectNotifiesOutsideOwnerBeforeTaskCompletion)
{
    ASSERT_EXIT (
      {
          check_auto_notifications ();
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
TEST (AutoConnectLaneDeathTest, StaleRouteDisconnectRetainsReplacement)
{
    ASSERT_EXIT (
      {
          check_auto_stale_peer ();
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}
