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

// A contended synchronous wrapper must return after its owner turn even while
// every completion-dispatcher worker is occupied. Queue admission orders release.
void check_sync_owner_wait (const char *name, state_lane_t &lane, std::function<void ()> work)
{
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    auto held = std::async (std::launch::async, [&] {
        lane.run_sync ([&] {
            entered.set_value ();
            released.wait ();
        });
    });
    entered.get_future ().get ();

    completion_owner_t completion_owner;
    auto dispatcher = detail::borrow_completion_dispatcher ();
    const auto workers = detail::completion_dispatcher_worker_count ();
    std::latch occupied (workers), release_dispatcher (1), dispatched (workers);
    for (std::size_t index = 0; index < workers; ++index)
        detail::dispatch_completion ([&, dispatcher] {
            occupied.count_down ();
            release_dispatcher.wait ();
            dispatched.count_down ();
        });
    occupied.wait ();

    auto invocation = std::async (std::launch::async, std::move (work));
    const auto deadline = std::chrono::steady_clock::now () + auto_connect_wait;
    while (state_lane_test_access_t::pending_work (lane) == 0
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    const bool queued = state_lane_test_access_t::pending_work (lane) != 0;
    release.set_value ();
    held.get ();
    const auto returned = invocation.wait_for (auto_connect_wait);
    release_dispatcher.count_down ();
    dispatched.wait ();
    invocation.get ();
    if (!queued || returned != std::future_status::ready)
        std::fprintf (stderr, "%s: queued=%d returned_without_dispatcher=%d\n", name,
                      static_cast<int> (queued),
                      static_cast<int> (returned == std::future_status::ready));
    EXPECT_TRUE (queued);
    EXPECT_EQ (std::future_status::ready, returned);
}

void check_synchronous_stop (bool mesh, bool raw_owner)
{
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
    state->routing_id = zlink::routing_id_t::from ("local");
    state->core_context = std::make_shared<zlink::context_t> ();
    auto node = std::make_shared<detail::mesh_node_runtime_t> (state);
    node->start ().result ().value ();
    production_auto_fixture_t fixture ({node});
    detail::route_channel_runtime_t route ("l4-mesh");
    const auto rid = zlink::routing_id_t::from ("remote");
    route.connect (rid, "inproc://l4-peer");
    auto &raw = node->native_node ().transport ();
    node->connect_peer (rid, "inproc://l4-peer", 1, "default", true);
    auto peer = raw.topology ().local_descriptor ();
    peer.node_routing_id = rid.to_bytes ();
    peer.advertised_endpoint = "inproc://l4-peer";
    peer.state = mesh::service_node_state_t::serving;
    ASSERT_EQ (mesh::peer_admission_result_t::admitted,
               raw.admit_peer (peer, {1}, mesh::service_liveness_registry_t::clock_t::now ()));
    ASSERT_NE (nullptr, raw.topology ().peer (rid.to_bytes ()));
    location_auto_connect_test_access_t::register_stop_probe (fixture.service, route, node, mesh);
    auto &lane = raw_owner ? auto_connect_owner_lane (node->native_node ().transport ())
                           : (mesh ? state->lane : auto_connect_owner_lane (route));
    check_sync_owner_wait ("auto-connect.stop", lane, [&] { fixture.service.stop (); });
    if (!mesh)
        EXPECT_TRUE (route.list_connections ().empty ());
    else
        EXPECT_EQ (nullptr, raw.topology ().peer (rid.to_bytes ()));
    fixture.failures->rethrow_if_failed ();
    node->stop ();
}

// Keep every infrastructure worker occupied until the synchronous caller
// returns. A contended lane must grant that caller its turn rather than run
// the disconnect on the thread releasing the predecessor's turn.
void check_mesh_stop_without_infrastructure (int operation)
{
    auto executor = infrastructure_executor ();
    auto reserved = test::reserve_single_infrastructure_worker (*executor);
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
    state->routing_id = zlink::routing_id_t::from ("local");
    state->core_context = std::make_shared<zlink::context_t> ();
    auto node = std::make_shared<detail::mesh_node_runtime_t> (state);
    node->start ().result ().value ();
    auto &raw = node->native_node ().transport ();
    const auto rid = zlink::routing_id_t::from ("remote");
    const std::string endpoint = "inproc://l4-peer";
    node->expect_peer (rid, endpoint, 1, "default");
    node->connect_peer (rid, endpoint, 1, "default", true);
    auto peer = raw.topology ().local_descriptor ();
    peer.node_routing_id = rid.to_bytes ();
    peer.advertised_endpoint = endpoint;
    peer.state = mesh::service_node_state_t::serving;
    ASSERT_EQ (mesh::peer_admission_result_t::admitted,
               raw.admit_peer (peer, {1}, mesh::service_liveness_registry_t::clock_t::now ()));
    production_auto_fixture_t fixture ({node});
    detail::route_channel_runtime_t route ("l4-mesh");
    location_auto_connect_test_access_t::register_stop_probe (fixture.service, route, node, true);
    auto &lane = operation == 0 ? state->lane : auto_connect_owner_lane (raw);
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    auto held = std::async (std::launch::async, [&] {
        lane.run_sync ([&] {
            entered.set_value ();
            released.wait ();
        });
    });
    entered.get_future ().get ();
    std::latch occupied (1), release_executor (1), returned_executor (1);
    executor->submit ([&] {
        occupied.count_down ();
        release_executor.wait ();
        returned_executor.count_down ();
    });
    occupied.wait ();
    auto invocation = std::async (std::launch::async, [&] {
        if (operation < 2)
            fixture.service.stop ();
        else if (operation == 2)
            raw.disconnect_peer_intent (endpoint);
        else if (operation == 3)
            raw.disconnect_peer_intent (rid, endpoint, true);
        else
            raw.forget_peer (rid.to_bytes (), endpoint);
    });
    const auto deadline = std::chrono::steady_clock::now () + auto_connect_wait;
    while (state_lane_test_access_t::pending_work (lane) == 0
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    EXPECT_GT (state_lane_test_access_t::pending_work (lane), 0u);
    const bool caller_turn = state_lane_test_access_t::pending_synchronous_turn (lane);
    if (!caller_turn)
        std::fprintf (stderr, "mesh stop operation=%d did not grant the synchronous caller\n",
                      operation);
    EXPECT_TRUE (caller_turn);
    release.set_value ();
    const auto returned = invocation.wait_for (auto_connect_wait);
    // Always release resources before reporting a failed return bound.
    release_executor.count_down ();
    returned_executor.wait ();
    held.get ();
    invocation.get ();
    EXPECT_EQ (std::future_status::ready, returned);
    if (operation < 4)
        EXPECT_EQ (nullptr, raw.topology ().peer (rid.to_bytes ()));
    fixture.failures->rethrow_if_failed ();
    fixture.service.stop ();
    node->stop ();
}

void check_synchronous_auto_wrappers ()
{
    detail::route_channel_runtime_t route ("l4-mesh");
    const auto rid = zlink::routing_id_t::from ("remote");
    const std::string endpoint = "inproc://l4-sync-peer";
    {
        SCOPED_TRACE ("route.connect");
        check_sync_owner_wait ("route.connect", auto_connect_owner_lane (route),
                               [&] { EXPECT_TRUE (route.connect (rid, endpoint)); });
    }
    {
        SCOPED_TRACE ("route.disconnect_endpoint");
        check_sync_owner_wait ("route.disconnect_endpoint", auto_connect_owner_lane (route),
                               [&] { EXPECT_TRUE (route.disconnect (rid, endpoint)); });
    }
    in_memory_location_repository_t store;
    store_location_resolvers_t cache (store);
    {
        SCOPED_TRACE ("cache.invalidate_all_routes_after_store_recovery");
        check_sync_owner_wait ("cache.invalidate_all_routes_after_store_recovery",
                               auto_connect_cache_test_access_t::lane (cache),
                               [&] { cache.invalidate_all_routes_after_store_recovery (); });
    }
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("l4-mesh");
    state->routing_id = zlink::routing_id_t::from ("local");
    state->core_context = std::make_shared<zlink::context_t> ();
    detail::mesh_node_runtime_t node (state);
    node.start ().result ().value ();
    auto &host = node.native_node ();
    auto &raw = host.transport ();
    auto &raw_lane = auto_connect_owner_lane (raw);
    mesh::service_node_descriptor_t descriptor = raw.topology ().local_descriptor ();
    descriptor.node_routing_id = rid.to_bytes ();
    descriptor.advertised_endpoint = endpoint;
    const std::pair<const char *, std::function<void ()>> raw_wrappers[] = {
      {"raw.connect_peer_intent",
       [&] { raw.connect_peer_intent (endpoint, rid, 1, "default", true); }},
      {"raw.disconnect_peer_intent", [&] { raw.disconnect_peer_intent (rid, endpoint, true); }},
      {"raw.expect_peer", [&] { raw.expect_peer (descriptor); }},
      {"raw.forget_peer", [&] { raw.forget_peer (rid.to_bytes (), endpoint); }},
      {"mesh.connect_peer", [&] { node.connect_peer (rid, endpoint, 1, "default", true); }},
      {"mesh.expect_peer", [&] { node.expect_peer (rid, endpoint, 1, "default"); }},
      {"mesh.forget_peer", [&] { node.forget_peer (rid, endpoint); }},
      {"host.expect_peer", [&] { host.expect_peer (endpoint, rid, 1, "default"); }},
      {"host.forget_peer", [&] { host.forget_peer (endpoint, rid); }}};
    for (const auto &[name, work] : raw_wrappers) {
        SCOPED_TRACE (name);
        check_sync_owner_wait (name, raw_lane, work);
    }
    {
        SCOPED_TRACE ("mesh.disconnect_peer");
        check_sync_owner_wait ("mesh.disconnect_peer", state->lane,
                               [&] { node.disconnect_peer (rid, endpoint, true); });
    }
    node.stop ();
}
} // namespace

TEST (AutoConnectSyncTest, ReconcileStopsBeforeTouchingActiveAfterTargetCompletion)
{
    for (int operation : {0, 1, 2}) {
        production_auto_fixture_t fixture;
        location_auto_connect_test_access_t::check_stop_between_targets (fixture.service,
                                                                         operation);
    }
}

TEST (AutoConnectSyncTest, StopDisconnectsConnectCompletedWhileStopIsPending)
{
    in_memory_location_repository_t store;
    location_runtime_t runtime (store, {}, "auto-connect-stop");
    production_auto_fixture_t fixture;
    detail::route_channel_runtime_t route ("l4-mesh");
    location_auto_connect_test_access_t::check_stop_after_pending_connect (fixture.service, runtime,
                                                                           route);
}

TEST (AutoConnectSyncTest, StopTimeoutLeavesPendingLoopToDisconnectTargets)
{
    for (int operation : {0, 1, 2}) {
        SCOPED_TRACE (operation);
        in_memory_location_repository_t store;
        location_options_t options;
        options.owner_lease_renew_timeout = std::chrono::milliseconds::zero ();
        location_runtime_t runtime (store, options, "timeout-owner");
        production_auto_fixture_t fixture;
        detail::route_channel_runtime_t route ("l4-mesh");
        auto release = std::make_shared<task_completion_source_t<void>> ();
        auto disconnects = std::make_shared<std::map<std::string, unsigned>> ();
        auto finished = location_auto_connect_test_access_t::start_timeout_probe (
          fixture.service, runtime, route, release, operation, disconnects);
        fixture.service.stop ();
        try {
            fixture.failures->rethrow_if_failed ();
            ADD_FAILURE () << "stop did not report its timeout";
        }
        catch (const framework_exception_t &error) {
            EXPECT_EQ (framework_error_kind_t::deadline_exceeded, error.kind ());
        }
        EXPECT_EQ (1u, location_auto_connect_test_access_t::loop_count (fixture.service));
        release->complete (result_t<void>::success ());
        const auto result = finished.result_for (auto_connect_wait);
        ASSERT_TRUE (result);
        EXPECT_TRUE (*result);
        EXPECT_TRUE (route.list_connections ().empty ());
        ASSERT_EQ (1u, disconnects->size ());
        EXPECT_EQ (1u, disconnects->at ("inproc://l4-timeout"));
    }
}

TEST (AutoConnectSyncTest, StopTimeoutStopsIndependentLocationRuntimesAndDrainsLoop)
{
    in_memory_location_repository_t repository;
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds::zero ();
    location_runtime_t locations (repository, options, "timeout-owner");
    auto builder = test::runtime_failure_builder ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    service_provider_t services;
    auto bus = builder.message_bus ();
    auto channel_runtime = detail::channel_runtime_t::from (bus);
    channel_runtime.bind_core_context (std::make_shared<zlink::context_t> ());
    channel_snapshot_t server_channel;
    server_channel.name = "timeout-server";
    server_channel.server.enabled = true;
    server_channel.server.bind_endpoints = {"tcp://127.0.0.1:0"};
    channel_snapshot_t publisher_channel;
    publisher_channel.name = "timeout-fanout";
    publisher_channel.publisher.enabled = true;
    publisher_channel.publisher.discovery = true;
    publisher_channel.publisher.routing_id = zlink::routing_id_t::from ("timeout-publisher");
    publisher_channel.publisher.bind_endpoints = {"inproc://timeout-fanout"};
    const std::vector<channel_snapshot_t> channels{server_channel, publisher_channel};
    auto listener_statuses = std::make_shared<listener_status_registry_t> ();
    auto client_server_runtime = std::make_shared<client_server::client_server_location_runtime_t> (
      bus, channels, &locations, &repository, &repository, services, serializers, handlers,
      [] { return framework_runtime_state_t::serving; }, std::map<std::string, std::string>{},
      listener_statuses);
    auto fanout_runtime = std::make_shared<fanout::fanout_location_runtime_t> (
      bus, channels, locations, repository, repository, services, serializers, handlers,
      [] { return framework_runtime_state_t::serving; }, detail::handler_invocation_executor ());
    client_server_runtime->start ();
    fanout_runtime->start ();
    ASSERT_TRUE (listener_statuses->find (listener_kind_t::client_server, "timeout-server"));
    auto failures = std::make_shared<runtime_failure_collector_t> ();
    location_auto_connect_host_service_t service (
      failures, bus, channels, handlers, serializers,
      [] { return framework_runtime_state_t::serving; }, {}, {}, {}, {}, {}, client_server_runtime,
      fanout_runtime, listener_statuses);
    detail::route_channel_runtime_t route ("l4-mesh");
    auto release = std::make_shared<task_completion_source_t<void>> ();
    auto finished = location_auto_connect_test_access_t::start_timeout_probe (service, locations,
                                                                              route, release, 0);

    service.stop ();

    EXPECT_TRUE (client_server::client_server_location_runtime_test_access_t::stop_requested (
      *client_server_runtime));
    EXPECT_TRUE (fanout::fanout_location_runtime_test_access_t::stop_requested (*fanout_runtime));
    EXPECT_FALSE (listener_statuses->find (listener_kind_t::client_server, "timeout-server"));
    EXPECT_EQ (1u, location_auto_connect_test_access_t::loop_count (service));
    EXPECT_TRUE (client_server_runtime->republish_after_store_recovery ().result ().value ());
    EXPECT_FALSE (fanout_runtime->republish_after_store_recovery ().result ().value ());

    auto replacement_publisher = std::make_shared<fanout::fanout_location_runtime_t> (
      bus, channels, locations, repository, repository, services, serializers, handlers,
      [] { return framework_runtime_state_t::serving; }, detail::handler_invocation_executor ());
    EXPECT_NO_THROW (replacement_publisher->start ());
    replacement_publisher->stop ();

    release->complete (result_t<void>::success ());
    const auto result = finished.result_for (auto_connect_wait);
    ASSERT_TRUE (result);
    EXPECT_TRUE (*result);
    EXPECT_TRUE (route.list_connections ().empty ());
}

TEST (AutoConnectSyncTest, DestructorDrainsPendingLoopAfterStopTimeout)
{
    auto executor = infrastructure_executor ();
    auto workers = test::reserve_single_infrastructure_worker (*executor);
    in_memory_location_repository_t store;
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds::zero ();
    location_runtime_t runtime (store, options, "timeout-owner");
    std::promise<void> destructor_wait_logged;
    logging_builder_t logging;
    logging.use_callback_sink ([&] (const log_record_t &record) {
        if (record.message == "Location coordinator destructor is waiting for the loop to finish")
            destructor_wait_logged.set_value ();
    });
    auto monitoring = std::make_shared<detail::monitoring_runtime_state_t> ();
    monitoring->diagnostics_logger = logging.create_logger ("zlink.framework.runtime");
    runtime.bind_monitoring (monitoring);
    auto builder = test::runtime_failure_builder ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    detail::route_channel_runtime_t route ("l4-mesh");
    auto failures = std::make_shared<runtime_failure_collector_t> ();
    auto service = std::make_unique<location_auto_connect_host_service_t> (
      failures, builder.message_bus (), std::vector<channel_snapshot_t>{}, handlers, serializers,
      [] { return framework_runtime_state_t::serving; });
    auto release = std::make_shared<task_completion_source_t<void>> ();
    auto finished = location_auto_connect_test_access_t::start_timeout_probe (*service, runtime,
                                                                              route, release, 0);
    service->stop ();
    std::promise<void> destructor_stopping;
    auto destroying = std::async (std::launch::async, [&] {
        destructor_stopping.set_value ();
        service.reset ();
    });
    destructor_stopping.get_future ().wait ();
    EXPECT_EQ (std::future_status::ready,
               destructor_wait_logged.get_future ().wait_for (auto_connect_wait));
    EXPECT_EQ (std::future_status::timeout,
               destroying.wait_for (std::chrono::milliseconds::zero ()));
    // Host destruction runs on its owning caller. The pending loop must still
    // be able to use the sole available infrastructure worker while it waits.
    executor->submit ([release] { release->complete (result_t<void>::success ()); });
    EXPECT_EQ (std::future_status::ready, destroying.wait_for (auto_connect_wait));
    destroying.get ();
    EXPECT_TRUE (finished.result ());
    EXPECT_TRUE (route.list_connections ().empty ());
}

TEST (AutoConnectSyncDeathTest, MeshStopCompletesWithoutInfrastructureExecutor)
{
    ASSERT_EXIT (
      {
          for (int operation : {0, 1, 2, 3, 4}) {
              SCOPED_TRACE (operation);
              check_mesh_stop_without_infrastructure (operation);
          }
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (AutoConnectSyncDeathTest, StopCompletesWithoutCompletionDispatcher)
{
    ASSERT_EXIT (
      {
          for (bool mesh : {false, true})
              for (bool raw_owner : {false, true})
                  check_synchronous_stop (mesh, raw_owner);
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (AutoConnectSyncDeathTest, ContendedWrappersCompleteWithoutCompletionDispatcher)
{
    ASSERT_EXIT (
      {
          check_synchronous_auto_wrappers ();
          std::_Exit (::testing::Test::HasFailure () ? 1 : 0);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (ZLinkFrameworkLocationRuntime, DrainingPublicationDoesNotHoldRecoveryMutex)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto state =
      test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("publication-mesh");
    state->routing_id = zlink::routing_id_t::from ("local");
    state->core_context = std::make_shared<zlink::context_t> ();
    auto store = std::make_shared<in_memory_location_repository_t> ();
    location_runtime_t runtime (*store);
    runtime.renew_owner_lease_once ();
    serializer_registry_t serializers;
    mesh_node_host_service_t service (std::make_shared<runtime_failure_collector_t> (), {state},
                                      serializers);
    mesh_node_host_service_test_access_t::configure (service, store, runtime, true);
    auto node = service.nodes ().front ();
    node->start ().result ().value ();
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    auto &topology = node->native_node ().transport ().topology ();
    topology.set_change_handler ([&] {
        entered.set_value ();
        released.wait ();
    });
    auto draining = std::async (std::launch::async, [&] {
        return service.publish_descriptor_state (framework_runtime_state_t::draining);
    });
    const auto pending = entered.get_future ().wait_for (std::chrono::seconds (5));
    const bool available =
      mesh_node_host_service_test_access_t::publication_mutex_available (service);
    auto recovery = std::async (std::launch::async, [&] {
        return service
          .republish_after_store_recovery (location_write_intent_t::new_claim, {},
                                           std::chrono::steady_clock::time_point::max ())
          .result ()
          .value ();
    });
    const bool recovered_while_pending =
      recovery.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    release.set_value ();
    const bool recovered = recovery.get ();
    const bool drained = draining.get ();
    topology.set_change_handler ({});
    const auto descriptors = store->list_mesh_nodes ("publication-mesh").result ().value ();
    EXPECT_EQ (std::future_status::ready, pending);
    EXPECT_TRUE (available);
    EXPECT_TRUE (recovered_while_pending);
    EXPECT_TRUE (recovered);
    EXPECT_TRUE (drained);
    ASSERT_EQ (1u, descriptors.items.size ());
    EXPECT_EQ (framework_runtime_state_t::draining, descriptors.items.front ().state);
    service.stop ();
    runtime.stop ();
}

namespace
{
// Holds the next MeshNode descriptor write until the test releases it.
class held_state_publication_store_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    std::atomic_bool hold{false};
    std::promise<void> entered;
    zlink::framework::task_completion_source_t<void> release;

    zlink::framework::task_t<zlink::framework::location_write_result_t>
    update_mesh_node (zlink::framework::mesh_node_descriptor_t descriptor,
                      zlink::framework::location_write_intent_t intent) override
    {
        if (!hold.exchange (false))
            return in_memory_location_repository_t::update_mesh_node (std::move (descriptor),
                                                                      intent);
        entered.set_value ();
        return zlink::framework::runtime::continue_task<zlink::framework::location_write_result_t> (
          release.task (), [this, descriptor = std::move (descriptor), intent] () mutable {
              return this->in_memory_location_repository_t::update_mesh_node (
                std::move (descriptor), intent);
          });
    }
};
} // namespace

// #4308: the renew completion takes the descriptor publication mutex. When the
// worker that completes a state publication's Store write runs that renew
// continuation first, the write completes only after the continuation does.
TEST (ZLinkFrameworkLocationRuntime, StatePublicationDoesNotHoldDescriptorMutexAcrossStoreWrite)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<held_state_publication_store_t> ();
    location_runtime_t runtime (*store);
    runtime.renew_owner_lease_once ();
    serializer_registry_t serializers;
    mesh_node_host_service_t service (std::make_shared<runtime_failure_collector_t> (), {},
                                      serializers);
    mesh_node_host_service_test_access_t::configure (service, store, runtime, true);
    store->hold = true;
    auto entered = store->entered.get_future ();
    auto relocating = std::async (std::launch::async, [&] {
        return service.publish_descriptor_state (framework_runtime_state_t::relocating);
    });
    const auto pending = entered.wait_for (std::chrono::seconds (5));
    const bool available =
      mesh_node_host_service_test_access_t::publication_mutex_available (service);
    auto renewal = std::async (std::launch::async, [&] {
        return service
          .republish_after_store_recovery (location_write_intent_t::renew, {},
                                           std::chrono::steady_clock::time_point::max ())
          .result ()
          .value ();
    });
    const bool renewed_while_pending =
      renewal.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    // Released after RED is recorded too, so the old implementation also unwinds.
    store->release.complete (result_t<void>::success ());
    const bool renewed = renewal.get ();
    const bool published = relocating.get ();
    const auto descriptors = store->list_mesh_nodes ("publication-mesh").result ().value ();
    EXPECT_EQ (std::future_status::ready, pending);
    EXPECT_TRUE (available);
    EXPECT_TRUE (renewed_while_pending);
    EXPECT_TRUE (renewed);
    EXPECT_TRUE (published);
    ASSERT_EQ (1u, descriptors.items.size ());
    EXPECT_EQ (framework_runtime_state_t::relocating, descriptors.items.front ().state);
    service.stop ();
    runtime.stop ();
}
