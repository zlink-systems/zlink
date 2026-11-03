/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "../support/location_runtime_test_access.hpp"

#include "../support/runtime_metric_contract.hpp"
#include "../support/manual_deadlines.hpp"
#include "../support/manual_internal_scheduler.hpp"

#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include <zlink/framework/contracts/configuration/detail/framework_options_validation.hpp>

#include <zlink/framework/contracts/configuration/app.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <future>
#include <latch>
#include <stop_token>
#include <thread>

namespace zlink::framework::runtime
{
struct state_lane_test_access_t
{
    static std::size_t pending_work (state_lane_t &lane)
    {
        const std::lock_guard lock (lane._mailbox_mutex);
        return lane._mailbox.size ();
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

struct location_auto_connect_test_access_t
{
    template <typename Route>
    static task_t<void> finish_probe (location_auto_connect_host_service_t *service, Route *route)
    {
        if constexpr (requires {
                          route->connect_async (zlink::routing_id_t::from ("remote"),
                                                std::string{});
                      })
            (void) co_await route->connect_async (zlink::routing_id_t::from ("remote"),
                                                  "inproc://l4-peer");
        else
            (void) route->connect (zlink::routing_id_t::from ("remote"), "inproc://l4-peer");
        service->_stop_requested.request_stop ();
        co_return;
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
          [&service, &route, &calls, &runtime, later, cache] (const auto &) {
              ++calls;
              if (cache) {
                  service._stop_requested.request_stop ();
                  return task_t<void> (result_t<void>::success ());
              }
              if (later && calls == 1) {
                  runtime.peer_reconcile_requester () ({});
                  return task_t<void> (result_t<void>::success ());
              }
              return finish_probe (&service, &route);
          },
          [] (const auto &) { return task_t<void> (result_t<void>::success ()); });
        service._loops.front ().recovering_from_store_failure = cache != nullptr;
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
          });
    }
};

struct mesh_node_host_service_test_access_t
{
    static void configure (mesh_node_host_service_t &service,
                           std::shared_ptr<location_repository_t> store,
                           location_runtime_t &runtime,
                           bool published = false)
    {
        service._location_store = std::move (store);
        service._location_runtime = &runtime;
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "publication-mesh";
        descriptor.rid = zlink::routing_id_t::from ("local");
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "inproc://publication-mesh";
        descriptor.security_identity = "default";
        descriptor.activation_concurrency.limit = 1;
        if (published) {
            const auto owner = runtime.current_owner_token ().value ();
            descriptor.owner_id = owner.owner_id;
            descriptor.lease_generation = owner.lease_generation;
            EXPECT_EQ (location_write_status_t::stored,
                       service._location_store
                         ->update_mesh_node (descriptor, location_write_intent_t::new_claim)
                         .result ()
                         .value ()
                         .status);
            service._published_mesh_nodes.push_back ({descriptor.mesh_name, descriptor.rid});
        }
        service._published_mesh_descriptors.push_back (std::move (descriptor));
    }
    static bool publication_mutex_available (mesh_node_host_service_t &service)
    {
        if (!service._descriptor_publish_mutex.try_lock ())
            return false;
        service._descriptor_publish_mutex.unlock ();
        return true;
    }
    // This adapter also compiles against the original synchronous implementation
    // for the RED run. Only its return shape is normalized, never its scheduling.
    template <typename Service>
    static task_t<bool> publish (Service &service,
                                 std::stop_token cancellation,
                                 std::chrono::steady_clock::time_point deadline)
    {
        if constexpr (requires {
                          service.republish_after_store_recovery (
                            location_write_intent_t::new_claim, cancellation, deadline);
                      })
            return service.republish_after_store_recovery (location_write_intent_t::new_claim,
                                                           cancellation, deadline);
        else
            return task_t<bool> (result_t<bool>::success (
              service.republish_after_store_recovery (location_write_intent_t::new_claim)));
    }
};

const auto requested_peer_rid = zlink::routing_id_t::from ("requested-peer");
} // namespace zlink::framework::runtime

namespace
{

using zlink::framework::location_options_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::location_runtime_t;

constexpr auto heartbeat_observation_timeout = std::chrono::seconds (5);
constexpr auto admission_observation_timeout = std::chrono::milliseconds (100);
const location_options_t asynchronous_heartbeat_options{
  .owner_lease_renew_interval = std::chrono::milliseconds (5),
  .owner_lease_ttl = std::chrono::seconds (15)};

class requested_peer_store_t final : public in_memory_location_repository_t
{
  public:
    std::atomic<unsigned> reads = 0;
    std::atomic<bool> visible = false;
    std::promise<void> first_read;
    std::latch release_first_read{1};
    zlink::framework::location_owner_token_t owner;

    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t>>
    list_mesh_nodes (std::string mesh, zlink::framework::location_page_request_t = {}) override
    {
        const bool include_peer = visible.load ();
        if (++reads == 1) {
            first_read.set_value ();
            release_first_read.wait ();
        }
        zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t> page;
        if (include_peer) {
            zlink::framework::mesh_node_descriptor_t descriptor;
            descriptor.mesh_name = std::move (mesh);
            descriptor.rid = zlink::framework::runtime::requested_peer_rid;
            descriptor.endpoint = "inproc://requested-peer";
            descriptor.lifecycle_generation = 1;
            descriptor.descriptor_revision = 1;
            descriptor.owner_id = owner.owner_id;
            descriptor.lease_generation = owner.lease_generation;
            page.items.push_back (std::move (descriptor));
        }
        return zlink::framework::task_t<decltype (page)> (
          zlink::framework::result_t<decltype (page)>::success (std::move (page)));
    }
};

class held_descriptor_store_t final : public in_memory_location_repository_t
{
  public:
    zlink::framework::task_completion_source_t<zlink::framework::location_write_result_t> write;
    std::promise<void> entered;
    std::atomic<unsigned> publications = 0;

    zlink::framework::task_t<zlink::framework::location_write_result_t>
    update_mesh_node (zlink::framework::mesh_node_descriptor_t,
                      zlink::framework::location_write_intent_t) override
    {
        if (++publications == 1)
            entered.set_value ();
        return write.task ();
    }

    void release ()
    {
        write.complete (
          zlink::framework::result_t<zlink::framework::location_write_result_t>::success (
            {zlink::framework::location_write_status_t::stored, 1, {}}));
    }
};

TEST (ZLinkFrameworkLocationRuntime, MeshPublicationSuspendsWithoutHoldingDescriptorMutex)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<held_descriptor_store_t> ();
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::seconds (2);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    location_runtime_t runtime (*store, options);
    runtime.start (zlink::routing_id_t::from ("mesh-publication"));
    serializer_registry_t serializers;
    mesh_node_host_service_t service (std::make_shared<runtime_failure_collector_t> (), {},
                                      serializers);
    mesh_node_host_service_test_access_t::configure (service, store, runtime);
    std::stop_source stop;
    auto started = store->entered.get_future ();
    auto invocation = std::async (std::launch::async, [&] {
        return mesh_node_host_service_test_access_t::publish (
          service, stop.get_token (),
          std::chrono::steady_clock::now () + options.owner_lease_renew_timeout);
    });
    const auto started_status = started.wait_for (std::chrono::seconds (5));
    const auto returned = invocation.wait_for (std::chrono::milliseconds (100));
    const auto mutex_available =
      mesh_node_host_service_test_access_t::publication_mutex_available (service);
    stop.request_stop ();
    // The old method must also be released after RED is recorded.
    if (returned != std::future_status::ready)
        store->release ();
    auto publication = invocation.get ();
    const auto canceled = publication.result_for (options.owner_lease_renew_timeout);
    store->release ();
    EXPECT_EQ (std::future_status::ready, started_status);
    EXPECT_EQ (std::future_status::ready, returned);
    EXPECT_TRUE (mutex_available);
    ASSERT_TRUE (canceled.has_value ());
    ASSERT_TRUE (*canceled);
    EXPECT_FALSE (canceled->value ());
    EXPECT_EQ (1u, store->publications.load ());
    service.stop ();
    runtime.stop ();
}

// The provider controls completion; the timeout observes stop, never releases I/O.
TEST (ZLinkFrameworkLocationRuntime, AutoConnectStopCancelsPendingPublication)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<in_memory_location_repository_t> ();
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds (200);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    options.polling_interval = std::chrono::hours (1);
    auto runtime = std::make_shared<location_runtime_t> (*store, options);
    runtime->start (zlink::routing_id_t::from ("coordinator"));
    auto builder = test::runtime_failure_builder ();
    service_collection_t services;
    services.add_factory<location_repository_t> (
      [store] (service_provider_t &) {
          return std::static_pointer_cast<location_repository_t> (store);
      },
      service_lifetime_t::singleton);
    services.add_factory<live_location_reader_t> (
      [store, options] (service_provider_t &) {
          return std::make_shared<live_location_reader_t> (*store, options);
      },
      service_lifetime_t::singleton);
    services.add_factory<location_runtime_t> ([runtime] (service_provider_t &) { return runtime; },
                                              service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    task_completion_source_t<bool> write;
    std::promise<void> entered;
    auto started = entered.get_future ();
    std::atomic<unsigned> publications = 0;
    location_auto_connect_host_service_t service (
      std::make_shared<runtime_failure_collector_t> (), builder.message_bus (), {}, handlers,
      serializers, [] { return framework_runtime_state_t::serving; }, {}, {}, {}, {},
      [&] (location_write_intent_t, std::stop_token cancellation,
           std::chrono::steady_clock::time_point deadline) -> task_t<bool> {
          ++publications;
          entered.set_value ();
          const auto result = co_await observe_until (write.task (), deadline, cancellation);
          co_return result &&result.value ();
      });
    service.start (provider).result ().value ();
    const auto publication_started = started.wait_for (std::chrono::seconds (5));
    auto stopped = std::async (std::launch::async, [&] { service.stop (); });
    const auto stop_status = stopped.wait_for (options.owner_lease_renew_timeout);
    // Release only after observing the bound, so RED also tears down cleanly.
    write.complete (result_t<bool>::success (true));
    stopped.get ();
    EXPECT_EQ (std::future_status::ready, publication_started);
    EXPECT_EQ (std::future_status::ready, stop_status);
    EXPECT_EQ (1u, publications.load ());
    EXPECT_FALSE (service.descriptors_published ().result ().value ());
    runtime->stop ();
}

class held_coordinator_read_store_t final : public in_memory_location_repository_t
{
  public:
    using page_t = zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t>;
    bool hold_capacity = false;
    zlink::framework::task_completion_source_t<page_t> listed;
    zlink::framework::task_completion_source_t<void> refreshed;
    std::promise<void> entered;
    std::promise<void> second_entered;
    std::atomic<unsigned> reads = 0;
    std::atomic<unsigned> lease_reads = 0;
    std::atomic<unsigned> refreshes = 0;

    zlink::framework::task_t<page_t>
    list_mesh_nodes (std::string, zlink::framework::location_page_request_t = {}) override
    {
        const auto round = ++reads;
        if (!hold_capacity) {
            if (round == 1)
                entered.set_value ();
            if (round == 2)
                second_entered.set_value ();
        }
        if (!hold_capacity)
            return listed.task ();
        zlink::framework::mesh_node_descriptor_t peer;
        peer.mesh_name = "ordered-mesh";
        peer.rid = zlink::routing_id_t::from ("snapshot-peer");
        peer.endpoint = "inproc://snapshot-peer";
        peer.owner_id = "snapshot-owner";
        peer.lifecycle_generation = 1;
        peer.descriptor_revision = 1;
        peer.lease_generation = 1;
        peer.state = zlink::framework::framework_runtime_state_t::serving;
        page_t page;
        page.items.push_back (std::move (peer));
        return zlink::framework::task_t<page_t> (
          zlink::framework::result_t<page_t>::success (std::move (page)));
    }

    zlink::framework::task_t<zlink::framework::owner_lease_read_result_t>
    read_owner_lease (std::string owner) override
    {
        using namespace zlink::framework;
        ++lease_reads;
        const auto now = std::chrono::system_clock::now ();
        return task_t<owner_lease_read_result_t> (result_t<owner_lease_read_result_t>::success (
          owner_lease_found_t{{std::move (owner), 1}, now + std::chrono::hours (1), now}));
    }

    zlink::framework::task_t<void> refresh_capacity_metrics (std::string,
                                                             zlink::routing_id_t) override
    {
        const auto round = ++refreshes;
        if (round == 1)
            entered.set_value ();
        if (round == 2)
            second_entered.set_value ();
        return refreshed.task ();
    }

    void release ()
    {
        page_t page;
        zlink::framework::mesh_node_descriptor_t peer;
        peer.mesh_name = "ordered-mesh";
        peer.rid = zlink::routing_id_t::from ("late-peer");
        peer.owner_id = "late-owner";
        peer.lifecycle_generation = 1;
        peer.lease_generation = 1;
        page.items.push_back (std::move (peer));
        listed.complete (zlink::framework::result_t<page_t>::success (std::move (page)));
        refreshed.complete (zlink::framework::result_t<void>::success ());
    }
};

void check_auto_connect_stop_cancels_pending_read (bool hold_capacity, bool expire_first = false)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<held_coordinator_read_store_t> ();
    store->hold_capacity = hold_capacity;
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds (200);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    options.polling_interval = std::chrono::hours (1);
    auto runtime = std::make_shared<location_runtime_t> (*store, options);
    runtime->start (zlink::routing_id_t::from ("read-stop"));
    auto builder = test::runtime_failure_builder ();
    service_collection_t services;
    services.add_factory<location_repository_t> (
      [store] (service_provider_t &) {
          return std::static_pointer_cast<location_repository_t> (store);
      },
      service_lifetime_t::singleton);
    auto reader = std::make_shared<live_location_reader_t> (*store, options);
    services.add_factory<live_location_reader_t> (
      [reader] (service_provider_t &) { return reader; }, service_lifetime_t::singleton);
    services.add_factory<location_runtime_t> ([runtime] (service_provider_t &) { return runtime; },
                                              service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    auto failures = std::make_shared<runtime_failure_collector_t> ();
    std::atomic<unsigned> publications = 0;
    std::atomic<unsigned> connections = 0;
    std::promise<void> published;
    auto publication_entered = published.get_future ();
    std::atomic<bool> tick_failed = false;
    location_auto_connect_host_service_t service (
      failures, builder.message_bus (), {}, handlers, serializers,
      [] { return framework_runtime_state_t::serving; }, {}, {}, {}, {},
      [&] (location_write_intent_t, std::stop_token,
           std::chrono::steady_clock::time_point) -> task_t<bool> {
          tick_failed = location_auto_connect_test_access_t::has_tick_failure (service);
          if (++publications == 1)
              published.set_value ();
          co_return true;
      });
    location_auto_connect_test_access_t::add_test_loop (
      service, [&] (std::string) { ++connections; }, [] {}, hold_capacity);
    auto entered = store->entered.get_future ();
    service.start (provider).result ().value ();
    const auto read_started = entered.wait_for (options.owner_lease_renew_timeout);
    if (expire_first) {
        EXPECT_EQ (std::future_status::ready,
                   publication_entered.wait_for (2 * options.owner_lease_renew_timeout));
        EXPECT_TRUE (tick_failed.load ());
        auto second_read = store->second_entered.get_future ();
        runtime->peer_reconcile_requester () ({});
        EXPECT_EQ (std::future_status::ready,
                   second_read.wait_for (options.owner_lease_renew_timeout));
    }
    const auto before_stop = std::chrono::steady_clock::now ();
    auto stopped = std::async (std::launch::async, [&] { service.stop (); });
    const auto stop_status = stopped.wait_until (before_stop + options.owner_lease_renew_timeout);
    // The provider stays pending until the stop bound has been measured, including on RED.
    store->release ();
    stopped.get ();
    EXPECT_EQ (std::future_status::ready, read_started);
    EXPECT_EQ (std::future_status::ready, stop_status);
    const auto rounds = expire_first ? 2u : 1u;
    EXPECT_EQ (rounds, store->reads.load ());
    EXPECT_EQ (hold_capacity ? rounds : 0u, store->refreshes.load ());
    EXPECT_EQ (hold_capacity ? rounds : 0u, store->lease_reads.load ());
    EXPECT_EQ (expire_first ? 1u : 0u, publications.load ());
    EXPECT_EQ (hold_capacity && expire_first ? 1u : 0u, connections.load ());
    EXPECT_FALSE (reader->cached_request_source (
      "ordered-mesh", zlink::routing_id_t::from ("late-peer").to_bytes (), 1));
    EXPECT_NO_THROW (failures->rethrow_if_failed ());
    runtime->stop ();
}

TEST (ZLinkFrameworkLocationRuntime, AutoConnectStopCancelsPendingListAndIgnoresLateCompletion)
{
    check_auto_connect_stop_cancels_pending_read (false);
}

TEST (ZLinkFrameworkLocationRuntime, AutoConnectStopCancelsPendingCapacityRefresh)
{
    check_auto_connect_stop_cancels_pending_read (true);
}

TEST (ZLinkFrameworkLocationRuntime, AutoConnectListTimeoutFailsTickAndAllowsNextPoll)
{
    check_auto_connect_stop_cancels_pending_read (false, true);
}

TEST (ZLinkFrameworkLocationRuntime,
      AutoConnectCapacityTimeoutFailsTickAndReconcilesPolledSnapshotBeforeNextPoll)
{
    check_auto_connect_stop_cancels_pending_read (true, true);
}

TEST (ZLinkFrameworkLocationRuntime, AutoConnectStopReportsLoopCompletionTimeout)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<in_memory_location_repository_t> ();
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds (200);
    location_runtime_t runtime (*store, options);
    auto builder = test::runtime_failure_builder ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    auto failures = std::make_shared<runtime_failure_collector_t> ();
    location_auto_connect_host_service_t service (
      failures, builder.message_bus (), {}, handlers, serializers,
      [] { return framework_runtime_state_t::serving; });
    // Isolate the final wait from Store cancellation; there is no running coroutine.
    auto finished = location_auto_connect_test_access_t::hold_loop_completion (service, runtime);
    auto stopped = std::async (std::launch::async, [&] { service.stop (); });
    const auto stop_status = stopped.wait_for (2 * options.owner_lease_renew_timeout);
    finished->complete (result_t<void>::success ());
    stopped.get ();
    EXPECT_EQ (std::future_status::ready, stop_status);
    EXPECT_THROW (failures->rethrow_if_failed (), framework_exception_t);
}

class ordered_coordinator_store_t final : public in_memory_location_repository_t
{
  public:
    using write_result = zlink::framework::location_write_result_t;
    zlink::framework::task_completion_source_t<write_result> writes[2];
    zlink::framework::task_completion_source_t<void> entered[2];
    zlink::framework::location_owner_token_t owner;
    std::atomic<unsigned> reads = 0;
    std::atomic<unsigned> publications = 0;
    std::mutex mutex;
    std::vector<std::string> events;

    void record (std::string event)
    {
        std::lock_guard lock (mutex);
        events.push_back (std::move (event));
    }
    std::vector<std::string> observed ()
    {
        std::lock_guard lock (mutex);
        return events;
    }
    zlink::framework::task_t<
      zlink::framework::location_page_t<zlink::framework::mesh_node_descriptor_t>>
    list_mesh_nodes (std::string mesh, zlink::framework::location_page_request_t = {}) override
    {
        using namespace zlink::framework;
        const auto round = ++reads;
        record ("snapshot-" + std::to_string (round));
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = std::move (mesh);
        descriptor.rid = zlink::routing_id_t::from ("peer-" + std::to_string (round));
        descriptor.endpoint = "inproc://peer-" + std::to_string (round);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.state = framework_runtime_state_t::serving;
        location_page_t<mesh_node_descriptor_t> page;
        page.items.push_back (std::move (descriptor));
        return task_t<decltype (page)> (result_t<decltype (page)>::success (std::move (page)));
    }
    zlink::framework::task_t<write_result>
    update_mesh_node (zlink::framework::mesh_node_descriptor_t,
                      zlink::framework::location_write_intent_t) override
    {
        const auto round = publications++;
        record ("publish-" + std::to_string (round + 1));
        if (round >= 2)
            throw std::logic_error ("unexpected publication after stop");
        entered[round].complete (zlink::framework::result_t<void>::success ());
        return writes[round].task ();
    }
    void release (unsigned round)
    {
        writes[round].complete (zlink::framework::result_t<write_result>::success (
          {zlink::framework::location_write_status_t::stored, round + 1, {}}));
    }
};

void check_auto_connect_snapshot_order_and_late_completion (bool expire_first)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<ordered_coordinator_store_t> ();
    location_options_t options;
    options.owner_lease_renew_timeout =
      expire_first ? std::chrono::milliseconds (200) : std::chrono::milliseconds (2000);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    options.polling_interval = std::chrono::hours (1);
    auto runtime = std::make_shared<location_runtime_t> (*store, options);
    runtime->start (zlink::routing_id_t::from ("ordered-local"));
    store->owner = runtime->current_owner_token ().value ();
    auto builder = test::runtime_failure_builder ();
    service_collection_t services;
    services.add_factory<location_repository_t> (
      [store] (service_provider_t &) {
          return std::static_pointer_cast<location_repository_t> (store);
      },
      service_lifetime_t::singleton);
    services.add_factory<live_location_reader_t> (
      [store, options] (service_provider_t &) {
          return std::make_shared<live_location_reader_t> (*store, options);
      },
      service_lifetime_t::singleton);
    services.add_factory<location_runtime_t> ([runtime] (service_provider_t &) { return runtime; },
                                              service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    location_auto_connect_host_service_t service (
      std::make_shared<runtime_failure_collector_t> (), builder.message_bus (), {}, handlers,
      serializers, [] { return framework_runtime_state_t::serving; }, {}, {}, {}, {},
      [store] (location_write_intent_t intent, std::stop_token cancellation,
               std::chrono::steady_clock::time_point deadline) -> task_t<bool> {
          const auto result =
            co_await observe_until (store->update_mesh_node ({}, intent), deadline, cancellation);
          co_return result &&result.value ().status == location_write_status_t::stored;
      });
    location_auto_connect_test_access_t::add_test_loop (
      service, [store] (std::string rid) { store->record ("connect-" + rid); },
      [store] { store->record ("disconnect"); });
    service.start (provider).result ().value ();
    const auto first = store->entered[0].task ().result_for (std::chrono::seconds (5));
    const auto request = runtime->peer_reconcile_requester ();
    request ({});
    const auto before_completion = store->observed ();
    if (!expire_first)
        store->release (0);
    const auto second = store->entered[1].task ().result_for (std::chrono::seconds (5));
    const auto before_stop = store->observed ();
    auto stopped = std::async (std::launch::async, [&] { service.stop (); });
    const auto stop_status = stopped.wait_for (options.owner_lease_renew_timeout);
    // Finish the provider only after stop's bound has been observed.
    store->release (0);
    store->release (1);
    stopped.get ();
    request ({});
    EXPECT_TRUE (first.has_value ());
    EXPECT_TRUE (second.has_value ());
    EXPECT_EQ ((std::vector<std::string>{"snapshot-1", "publish-1"}), before_completion);
    std::vector<std::string> ordered{"snapshot-1", "publish-1"};
    if (!expire_first)
        ordered.push_back ("connect-peer-1");
    ordered.push_back ("snapshot-2");
    ordered.push_back ("publish-2");
    EXPECT_EQ (ordered, before_stop);
    EXPECT_EQ (std::future_status::ready, stop_status);
    EXPECT_EQ (2u, store->reads.load ());
    EXPECT_EQ (2u, store->publications.load ());
    auto expected = before_stop;
    if (!expire_first)
        expected.push_back ("disconnect");
    EXPECT_EQ (expected, store->observed ());
    runtime->stop ();
}

TEST (ZLinkFrameworkLocationRuntime, AutoConnectOrdersSnapshotsAndIgnoresLateStoreCompletion)
{
    check_auto_connect_snapshot_order_and_late_completion (false);
}

TEST (ZLinkFrameworkLocationRuntime, AutoConnectPublicationTimeoutAllowsNextRenewal)
{
    check_auto_connect_snapshot_order_and_late_completion (true);
}

TEST (ZLinkFrameworkLocationRuntime,
      DescriptorPublicationCancellationAndDeadlineUseExistingObservation)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<held_descriptor_store_t> ();
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds (100);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    location_runtime_t runtime (*store, options);
    runtime.start (zlink::routing_id_t::from ("timeout-owner"));
    std::stop_source stopped;
    auto entered = store->entered.get_future ();
    auto publication = runtime.descriptor_write_confirmed (
      [store] { return store->update_mesh_node ({}, location_write_intent_t::new_claim); },
      std::chrono::steady_clock::now, stopped.get_token ());
    EXPECT_EQ (std::future_status::ready, entered.wait_for (std::chrono::seconds (5)));
    const auto timed_out = publication.result_for (std::chrono::seconds (5));
    EXPECT_TRUE (timed_out.has_value ());
    if (timed_out && *timed_out)
        EXPECT_FALSE (timed_out->value ());
    stopped.request_stop ();
    unsigned submissions = 0;
    auto canceled = runtime.descriptor_write_confirmed (
      [&] {
          ++submissions;
          return store->write.task ();
      },
      std::chrono::steady_clock::now, stopped.get_token ());
    EXPECT_FALSE (canceled.result ().value ());
    EXPECT_EQ (0u, submissions);
    store->release ();
    runtime.stop ();
}

class held_domain_descriptor_store_t final : public in_memory_location_repository_t
{
  public:
    using write_result = zlink::framework::location_write_result_t;
    zlink::framework::task_completion_source_t<write_result> write;
    std::promise<void> entered;
    std::atomic<unsigned> publications = 0;
    std::atomic<unsigned> held_writes = 0;
    std::atomic_bool hold_writes = true;
    zlink::framework::task_completion_source_t<void> cleanup_entered;

    zlink::framework::task_t<write_result> pending ()
    {
        ++publications;
        if (!hold_writes.load ())
            return zlink::framework::task_t<write_result> (
              zlink::framework::result_t<write_result>::success (
                {zlink::framework::location_write_status_t::stored, 1, {}}));
        if (++held_writes == 1)
            entered.set_value ();
        else
            cleanup_entered.complete (zlink::framework::result_t<void>::success ());
        return write.task ();
    }
    zlink::framework::task_t<write_result>
    update_client_server (zlink::framework::client_server_server_descriptor_t,
                          zlink::framework::location_write_intent_t) override
    {
        return pending ();
    }
    zlink::framework::task_t<write_result>
    update_fanout_publisher (zlink::framework::fanout_publisher_descriptor_t,
                             zlink::framework::location_write_intent_t) override
    {
        return pending ();
    }
    void release ()
    {
        write.complete (zlink::framework::result_t<write_result>::success (
          {zlink::framework::location_write_status_t::stored, 1, {}}));
    }
};

void check_auto_connect_domain_stop (bool fanout, bool previously_published = false)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<held_domain_descriptor_store_t> ();
    store->hold_writes.store (!previously_published);
    location_options_t options;
    options.owner_lease_renew_timeout = std::chrono::milliseconds (200);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    options.polling_interval = std::chrono::milliseconds (5);
    auto runtime = std::make_shared<location_runtime_t> (*store, options);
    auto builder = test::runtime_failure_builder ();
    auto channel = builder.channel ("held-domain");
    if (fanout)
        channel.enable_publisher ()
          .set_routing_id (zlink::routing_id_t::from ("held-domain-publisher"))
          .bind ("tcp://127.0.0.1:0");
    else
        channel.enable_server ().bind ("tcp://127.0.0.1:0");
    service_collection_t services;
    services.add_factory<location_repository_t> (
      [store] (service_provider_t &) {
          return std::static_pointer_cast<location_repository_t> (store);
      },
      service_lifetime_t::singleton);
    services.add_factory<live_location_reader_t> (
      [store, options] (service_provider_t &) {
          return std::make_shared<live_location_reader_t> (*store, options);
      },
      service_lifetime_t::singleton);
    services.add_factory<location_runtime_t> ([runtime] (service_provider_t &) { return runtime; },
                                              service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    location_auto_connect_host_service_t service (
      std::make_shared<runtime_failure_collector_t> (), builder.message_bus (),
      detail::channel_runtime_t::from (builder.message_bus ()).channel_snapshots (), handlers,
      serializers, [] { return framework_runtime_state_t::serving; });
    // Resources start without an owner; the first descriptor write belongs to
    // recovery, after the owner lease below becomes usable.
    service.start (provider).result ().value ();
    auto entered = store->entered.get_future ();
    runtime->start (zlink::routing_id_t::from ("held-domain-owner"));
    if (previously_published) {
        const auto published =
          service.descriptors_published ().result_for (std::chrono::seconds (5));
        EXPECT_TRUE (published && *published && published->value ());
        store->hold_writes.store (true);
        runtime->set_draining (true);
        runtime->peer_reconcile_requester () ({});
    }
    const auto started = entered.wait_for (std::chrono::seconds (5));
    auto stopped = std::async (std::launch::async, [&] { service.stop (); });
    if (previously_published && fanout) {
        // Teardown has its own Store write. Expire that observation with the
        // existing scheduler test clock, without completing the provider.
        const auto cleanup = store->cleanup_entered.task ().result_for (std::chrono::seconds (5));
        EXPECT_TRUE (cleanup.has_value ());
        detail::deadline_scheduler_test_access_t::advance_to (std::chrono::steady_clock::now ()
                                                              + options.owner_lease_renew_timeout);
    }
    const auto stop_status = stopped.wait_for (options.owner_lease_renew_timeout);
    const auto before_late_completion = store->publications.load ();
    store->release ();
    stopped.get ();
    EXPECT_EQ (std::future_status::ready, started);
    EXPECT_EQ (std::future_status::ready, stop_status);
    EXPECT_EQ (previously_published ? (fanout ? 3u : 2u) : 1u, before_late_completion);
    EXPECT_EQ (before_late_completion, store->publications.load ());
    runtime->stop ();
}

TEST (ZLinkFrameworkLocationRuntime, ClientServerCoordinatorStopCancelsHeldDescriptorWrite)
{
    check_auto_connect_domain_stop (false);
}

TEST (ZLinkFrameworkLocationRuntime, FanoutCoordinatorStopCancelsHeldDescriptorWrite)
{
    check_auto_connect_domain_stop (true);
}

TEST (ZLinkFrameworkLocationRuntime, ClientServerStopAfterPublishedDescriptorIgnoresLateRenewal)
{
    check_auto_connect_domain_stop (false, true);
}

TEST (ZLinkFrameworkLocationRuntime, FanoutStopSharesDeadlineWithDescriptorCleanup)
{
    check_auto_connect_domain_stop (true, true);
}

void check_discovery_reconcile_without_periodic_wait (bool request_during_initial_read)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    auto store = std::make_shared<requested_peer_store_t> ();
    location_options_t options;
    options.polling_interval = std::chrono::hours (1);
    options.owner_lease_renew_interval = std::chrono::hours (1);
    options.owner_lease_ttl = std::chrono::hours (3);
    auto runtime = std::make_shared<location_runtime_t> (*store, options);
    runtime->start (zlink::routing_id_t::from ("a-local"));
    store->owner = runtime->current_owner_token ().value ();

    auto builder = test::runtime_failure_builder ();
    builder.route_channel ("wake-mesh").set_routing_id (zlink::routing_id_t::from ("a-local"));
    auto manager = zlink::framework::detail::channel_runtime_manager_t::from (builder);
    manager.initialize_route_channels (builder);
    auto &route = manager.get_route_channel ("wake-mesh");
    service_collection_t services;
    services.add_factory<location_repository_t> (
      [store] (service_provider_t &) {
          return std::static_pointer_cast<location_repository_t> (store);
      },
      service_lifetime_t::singleton);
    services.add_factory<live_location_reader_t> (
      [store, options] (service_provider_t &) {
          return std::make_shared<live_location_reader_t> (*store, options);
      },
      service_lifetime_t::singleton);
    services.add_factory<location_runtime_t> ([runtime] (service_provider_t &) { return runtime; },
                                              service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    handler_registry_t handlers;
    serializer_registry_t serializers;
    std::promise<void> initial_publication;
    auto initially_published = initial_publication.get_future ();
    std::promise<bool> connected;
    auto connection_observed = connected.get_future ();
    auto first_read = store->first_read.get_future ();
    const auto request = runtime->peer_reconcile_requester ();
    unsigned publications = 0;
    location_auto_connect_host_service_t service (
      std::make_shared<runtime_failure_collector_t> (), builder.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (builder.message_bus ())
        .channel_snapshots (),
      handlers, serializers,
      [&provider] {
          return provider.get_required<zlink::framework::framework_runtime_t> ().status ().state;
      },
      {}, {}, {}, {},
      [&] (location_write_intent_t, std::stop_token,
           std::chrono::steady_clock::time_point) -> task_t<bool> {
          if (++publications == 1) {
              initial_publication.set_value ();
          } else if (publications == 2) {
              // The callback precedes reconcile. A second request schedules an
              // observation tick after the first requested tick has connected.
              request (requested_peer_rid.to_bytes ());
          } else if (publications == 3) {
              const auto targets = route.list_connection_targets ();
              connected.set_value (targets.size () == 1 && targets.front ().peer_rid
                                   && *targets.front ().peer_rid == requested_peer_rid
                                   && targets.front ().endpoint == "inproc://requested-peer");
          }
          co_return true;
      });
    service.start (provider).result ().value ();
    const auto initial_read_status = first_read.wait_for (heartbeat_observation_timeout);
    store->visible.store (true);
    if (request_during_initial_read)
        request (requested_peer_rid.to_bytes ());
    store->release_first_read.count_down ();
    ASSERT_EQ (std::future_status::ready, initial_read_status);
    ASSERT_EQ (std::future_status::ready,
               initially_published.wait_for (heartbeat_observation_timeout));
    if (!request_during_initial_read) {
        // This bounded observation detects an unsolicited reconcile during the
        // window; it cannot establish that none would occur at a later time.
        EXPECT_EQ (std::future_status::timeout,
                   connection_observed.wait_for (admission_observation_timeout));
        EXPECT_EQ (1u, store->reads.load ());
        EXPECT_TRUE (route.list_connection_targets ().empty ());
        request (requested_peer_rid.to_bytes ());
    }
    const auto observed = connection_observed.wait_for (heartbeat_observation_timeout);
    EXPECT_EQ (std::future_status::ready, observed);
    if (observed == std::future_status::ready)
        EXPECT_TRUE (connection_observed.get ());
    service.stop ();
    runtime->stop ();
}

TEST (ZLinkFrameworkLocationRuntime, PeerRequestRunsReconcileWithoutPeriodicWait)
{
    check_discovery_reconcile_without_periodic_wait (false);
}

TEST (ZLinkFrameworkLocationRuntime, PeerRequestBeforeWaitRunsReconcileWithoutPeriodicWait)
{
    check_discovery_reconcile_without_periodic_wait (true);
}

class advancing_claim_store_t : public in_memory_location_repository_t
{
  public:
    advancing_claim_store_t (std::chrono::steady_clock::time_point &now,
                             std::chrono::milliseconds claim_duration,
                             bool fail_claim) :
        _now (now), _claim_duration (claim_duration), _fail_claim (fail_claim)
    {
    }

    zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t>
    claim_owner_lease (std::string owner_id, std::chrono::milliseconds ttl) override
    {
        _now += _claim_duration;
        if (_fail_claim)
            throw std::runtime_error ("Store unavailable");
        return in_memory_location_repository_t::claim_owner_lease (std::move (owner_id), ttl);
    }

    zlink::framework::task_t<zlink::framework::owner_lease_read_result_t>
    read_owner_lease (std::string owner_id) override
    {
        if (_fail_claim)
            throw std::runtime_error ("Store unavailable");
        return in_memory_location_repository_t::read_owner_lease (std::move (owner_id));
    }

  private:
    std::chrono::steady_clock::time_point &_now;
    std::chrono::milliseconds _claim_duration;
    bool _fail_claim;
};

TEST (ZLinkFrameworkLocationRuntime, FirstHeartbeatIsDueFromInitialLeaseOperationStart)
{
    using access_t = zlink::framework::runtime::location_runtime_test_access_t;
    for (bool fail_claim : {false, true}) {
        for (auto duration : {std::chrono::milliseconds (50), std::chrono::milliseconds (150)}) {
            SCOPED_TRACE (::testing::Message ()
                          << "fail_claim=" << fail_claim << " claim_ms=" << duration.count ());
            auto now = std::chrono::steady_clock::now ();
            advancing_claim_store_t store (now, duration, fail_claim);
            location_runtime_t runtime (
              store,
              location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (100)},
              "owner-first-heartbeat");
            EXPECT_EQ (access_t::first_heartbeat_delay (runtime, [&] { return now; }),
                       std::max (std::chrono::milliseconds::zero (),
                                 std::chrono::milliseconds (100) - duration));
            EXPECT_TRUE (runtime.cleanup_owner ());
        }
    }
}

TEST (ZLinkFrameworkLocationRuntime, ReceiveAdmissionDoesNotJoinBusyLocationLane)
{
    in_memory_location_repository_t store;
    location_runtime_t runtime (store, {}, "owner-admission-snapshot");
    runtime.renew_owner_lease_once ();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ().share ();
    auto &lane = zlink::framework::runtime::location_runtime_test_access_t::lane (runtime);
    ASSERT_TRUE (lane.try_post ([&] {
        entered.set_value ();
        released.wait ();
    }));
    entered.get_future ().wait ();
    auto admission = std::async (std::launch::async, [&] {
        const auto token = runtime.current_owner_token ();
        const auto pending = runtime.current_owner_token_task ();
        if (!token || !pending.await_ready ())
            return false;
        const auto task_token = pending.result ().value ();
        return runtime.owner_lease_usable () && task_token.has_value ()
               && task_token->owner_id == token->owner_id
               && task_token->lease_generation == token->lease_generation
               && runtime.owner_lease_healthy () && runtime.owner_lease_renewed_at ().has_value ();
    });
    const auto status = admission.wait_for (admission_observation_timeout);
    release.set_value ();
    EXPECT_EQ (status, std::future_status::ready);
    EXPECT_TRUE (admission.get ());
    EXPECT_TRUE (runtime.cleanup_owner ());
    EXPECT_FALSE (runtime.owner_lease_usable ());
}

TEST (ZLinkFrameworkLocationRuntime, AdmissionSnapshotPreservesStrictExpiryAndRelease)
{
    using access_t = zlink::framework::runtime::location_runtime_test_access_t;
    in_memory_location_repository_t store;
    location_runtime_t runtime (store, {}, "owner-admission-expiry");
    EXPECT_FALSE (runtime.owner_lease_usable ());
    runtime.renew_owner_lease_once ();
    auto previous = access_t::snapshot (runtime);
    const auto deadline = std::chrono::steady_clock::now ();
    access_t::set_deadline (runtime, deadline);
    EXPECT_TRUE (access_t::usable_at (runtime, deadline - std::chrono::steady_clock::duration (1)));
    EXPECT_FALSE (access_t::usable_at (runtime, deadline));
    EXPECT_FALSE (
      access_t::usable_at (runtime, deadline + std::chrono::steady_clock::duration (1)));
    EXPECT_FALSE (runtime.owner_lease_usable ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    runtime.renew_owner_lease_once ();
    EXPECT_TRUE (runtime.owner_lease_usable ());
    EXPECT_NE (previous, access_t::snapshot (runtime));
    EXPECT_TRUE (runtime.cleanup_owner ());
    EXPECT_FALSE (runtime.owner_lease_usable ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    EXPECT_TRUE (previous->token.has_value ());
    const auto retained = std::weak_ptr{previous};
    previous.reset ();
    EXPECT_TRUE (retained.expired ());
}

TEST (ZLinkFrameworkLocationRuntime, RejectsLeaseTimeoutLongerThanFencedRenewalWindow)
{
    location_options_t options;
    options.owner_lease_renew_interval = std::chrono::seconds (1);
    options.owner_lease_renew_timeout = std::chrono::seconds (8);
    EXPECT_THROW (zlink::framework::detail::validate_location_options (options),
                  zlink::framework::framework_exception_t);
    EXPECT_NO_THROW (zlink::framework::detail::validate_location_options (location_options_t{}));
}

TEST (ZLinkFrameworkLocationRuntime, RejectsLeaseRenewalWindowOverflow)
{
    location_options_t options;
    options.owner_lease_renew_interval = std::chrono::milliseconds (1);
    options.owner_lease_renew_timeout =
      std::chrono::milliseconds::max () / 2 + std::chrono::milliseconds (1);
    EXPECT_THROW (zlink::framework::detail::validate_location_options (options),
                  zlink::framework::framework_exception_t);
    options.owner_lease_renew_timeout = std::chrono::milliseconds::max ();
    EXPECT_THROW (zlink::framework::detail::validate_location_options (options),
                  zlink::framework::framework_exception_t);
    options.owner_lease_renew_interval = std::chrono::milliseconds::max ();
    options.owner_lease_renew_timeout = std::chrono::milliseconds (1);
    EXPECT_THROW (zlink::framework::detail::validate_location_options (options),
                  zlink::framework::framework_exception_t);
}

class timed_renew_repository_t final : public in_memory_location_repository_t
{
  public:
    std::future<std::chrono::steady_clock::time_point> second_renew_called ()
    {
        return _second_renew_called.get_future ();
    }

    zlink::framework::task_t<zlink::framework::owner_lease_renew_result_t>
    renew_owner_lease (zlink::framework::location_owner_token_t token,
                       std::chrono::milliseconds ttl) override
    {
        if (++_calls == 2)
            _second_renew_called.set_value (std::chrono::steady_clock::now ());
        return in_memory_location_repository_t::renew_owner_lease (std::move (token), ttl);
    }

  private:
    int _calls = 0;
    std::promise<std::chrono::steady_clock::time_point> _second_renew_called;
};

TEST (ZLinkFrameworkLocationRuntime, CompletedHeartbeatWaitsUntilPreviousStartPlusInterval)
{
    timed_renew_repository_t store;
    const auto interval = std::chrono::milliseconds (400);
    location_runtime_t runtime (store,
                                location_options_t{.owner_lease_renew_interval = interval,
                                                   .owner_lease_renew_timeout = interval / 4},
                                "owner-spacing");
    auto second_renew_called = store.second_renew_called ();
    // The runtime starts a renewal before its state lane hands the request to the store, so a
    // store call time is later than the runtime's start time by a scheduling delay that differs
    // per renewal. Only a time taken before startup bounds the runtime's start times from below:
    // the first heartbeat starts one interval after startup, and the second starts one interval
    // after the first started.
    const auto start_requested_at = std::chrono::steady_clock::now ();
    runtime.start (zlink::routing_id_t::from ("node-spacing"));
    ASSERT_EQ (std::future_status::ready, second_renew_called.wait_for (std::chrono::seconds (5)));
    EXPECT_GE (second_renew_called.get () - start_requested_at, 2 * interval);
    runtime.stop ();
}

class slow_claim_repository_t final : public in_memory_location_repository_t
{
  public:
    explicit slow_claim_repository_t (std::chrono::milliseconds claim_delay) :
        _claim_delay (claim_delay)
    {
    }

    std::future<void> first_renew_called () { return _first_renew_called.get_future (); }

    zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t>
    claim_owner_lease (std::string owner_id, std::chrono::milliseconds ttl) override
    {
        std::this_thread::sleep_for (_claim_delay);
        return in_memory_location_repository_t::claim_owner_lease (std::move (owner_id), ttl);
    }

    zlink::framework::task_t<zlink::framework::owner_lease_renew_result_t>
    renew_owner_lease (zlink::framework::location_owner_token_t token,
                       std::chrono::milliseconds ttl) override
    {
        if (++_renew_calls == 1)
            _first_renew_called.set_value ();
        return in_memory_location_repository_t::renew_owner_lease (std::move (token), ttl);
    }

  private:
    std::chrono::milliseconds _claim_delay;
    std::atomic_int _renew_calls = 0;
    std::promise<void> _first_renew_called;
};

TEST (ZLinkFrameworkLocationRuntime, RenewLatenessIsMeasuredFromTheHeartbeatSchedule)
{
    // Lateness compares the renewal start with the scheduled deadline (the initial lease
    // operation start plus one interval), rather than with the claim start itself. A slow claim
    // must not be reported in full as heartbeat lateness.
    metric_test::provider_t metrics;
    const auto claim_delay = std::chrono::milliseconds (300);
    slow_claim_repository_t store (claim_delay);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (100),
                         .owner_lease_renew_timeout = std::chrono::seconds (1)},
      "owner-lateness");
    runtime.bind_monitoring (zlink::framework::test::runtime_failure_monitoring ());
    runtime.register_metric_scope ("mesh", "lateness-mesh").result ().value ();
    auto first_renew_called = store.first_renew_called ();
    runtime.start (zlink::routing_id_t::from ("node-lateness"));
    ASSERT_EQ (std::future_status::ready, first_renew_called.wait_for (std::chrono::seconds (5)));

    std::optional<metric_test::sdk::HistogramPointData> lateness;
    const auto data = metrics.collect ();
    metric_test::validate_metric_contract (data);
    for (const auto &metric : data) {
        if (metric.instrument_descriptor.name_ != "zlink.location.owner_lease.renew.lateness")
            continue;
        for (const auto &point : metric.point_data_attr_) {
            EXPECT_EQ ("mesh",
                       opentelemetry::nostd::get<std::string> (point.attributes.at ("scope_kind")));
            EXPECT_EQ ("lateness-mesh",
                       opentelemetry::nostd::get<std::string> (point.attributes.at ("scope_name")));
            if (const auto *histogram =
                  opentelemetry::nostd::get_if<metric_test::sdk::HistogramPointData> (
                    &point.point_data))
                lateness = *histogram;
        }
    }
    runtime.stop ();
    ASSERT_TRUE (lateness.has_value ());
    EXPECT_LT (opentelemetry::nostd::get<double> (lateness->max_),
               std::chrono::duration<double> (claim_delay).count ());
}

class pending_renew_repository_t final : public in_memory_location_repository_t
{
  public:
    std::future<void> renew_started () { return _renew_started.get_future (); }
    std::future<void> next_renew_started () { return _next_renew_started.get_future (); }

    int renew_calls () const noexcept { return _renew_calls.load (); }

    zlink::framework::task_t<zlink::framework::owner_lease_renew_result_t>
    renew_owner_lease (zlink::framework::location_owner_token_t, std::chrono::milliseconds) override
    {
        const auto call = _renew_calls.fetch_add (1);
        if (call != 0) {
            if (call == 1)
                _next_renew_started.set_value ();
            return zlink::framework::task_t<zlink::framework::owner_lease_renew_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_renew_result_t>::success (
                zlink::framework::owner_lease_renew_result_t{
                  zlink::framework::owner_lease_stale_t{}}));
        }
        _renew_started.set_value ();
        return _renew_completion.task ();
    }

    void complete_renew (zlink::framework::owner_lease_renew_result_t result)
    {
        _renew_completion.complete (
          zlink::framework::result_t<zlink::framework::owner_lease_renew_result_t>::success (
            std::move (result)));
    }

  private:
    std::promise<void> _renew_started;
    std::promise<void> _next_renew_started;
    std::atomic_int _renew_calls = 0;
    zlink::framework::task_completion_source_t<zlink::framework::owner_lease_renew_result_t>
      _renew_completion;
};

TEST (ZLinkFrameworkLocationRuntime, PendingHeartbeatDoesNotOverlapAndCompletionWakesNextRenewal)
{
    pending_renew_repository_t store;
    const auto interval = std::chrono::milliseconds (400);
    location_runtime_t runtime (store, location_options_t{.owner_lease_renew_interval = interval},
                                "owner-pending");
    auto started = store.renew_started ();
    auto next = store.next_renew_started ();
    runtime.start (zlink::routing_id_t::from ("node-pending"));
    ASSERT_EQ (std::future_status::ready, started.wait_for (std::chrono::seconds (5)));
    EXPECT_EQ (std::future_status::timeout, next.wait_for (interval + interval / 4));
    const auto lease = store.read_owner_lease ("owner-pending").result ().value ();
    const auto &found = std::get<zlink::framework::owner_lease_found_t> (lease);
    store.complete_renew (
      zlink::framework::owner_lease_renewed_t{found.lease_expires_at, found.store_now});
    EXPECT_EQ (std::future_status::ready, next.wait_for (interval / 4));
    runtime.stop ();
}

TEST (ZLinkFrameworkLocationRuntime, HeartbeatContinuesWhenRenewCompletesAsynchronously)
{
    pending_renew_repository_t store;
    const auto &options = asynchronous_heartbeat_options;
    location_runtime_t runtime (store, options, "owner-a");
    auto renew_started = store.renew_started ();
    const auto observation_deadline =
      std::chrono::steady_clock::now () + options.owner_lease_renew_timeout;
    runtime.start (zlink::routing_id_t::from ("node-a"));
    ASSERT_EQ (std::future_status::ready, renew_started.wait_for (heartbeat_observation_timeout));
    const auto lease = store.read_owner_lease ("owner-a").result ().value ();
    const auto &found = std::get<zlink::framework::owner_lease_found_t> (lease);
    const auto renewed_at = found.store_now + std::chrono::seconds (1);
    store.complete_renew (zlink::framework::owner_lease_renewed_t{
      found.lease_expires_at + std::chrono::seconds (1), renewed_at});
    while (store.renew_calls () == 1 && std::chrono::steady_clock::now () < observation_deadline)
        std::this_thread::yield ();
    EXPECT_LT (std::chrono::steady_clock::now (), observation_deadline);
    EXPECT_GT (store.renew_calls (), 1);
    runtime.stop ();
}

TEST (ZLinkFrameworkLocationRuntime, StopDiscardsLateHeartbeatRenewCompletion)
{
    pending_renew_repository_t store;
    location_runtime_t runtime (store, asynchronous_heartbeat_options, "owner-a");
    auto renew_started = store.renew_started ();
    runtime.start (zlink::routing_id_t::from ("node-a"));
    const auto initial_renewed_at = runtime.owner_lease_renewed_at ();
    ASSERT_EQ (std::future_status::ready, renew_started.wait_for (heartbeat_observation_timeout));

    runtime.stop ();
    const auto late_store_now = *initial_renewed_at + std::chrono::seconds (1);
    store.complete_renew (zlink::framework::owner_lease_renewed_t{
      late_store_now + std::chrono::seconds (15), late_store_now});
    EXPECT_EQ (initial_renewed_at, runtime.owner_lease_renewed_at ());
}

TEST (ZLinkFrameworkLocationRuntime, HostShutdownDiscardsLateHeartbeatStoreCompletion)
{
    using namespace zlink::framework;
    runtime::configure_handler_coroutine_executor (1);
    runtime::install_host_context_hooks ();
    pending_renew_repository_t store;
    location_runtime_t location (store, asynchronous_heartbeat_options, "owner-late-host");
    auto started = store.renew_started ();
    location.start (zlink::routing_id_t::from ("node-late-host"));
    EXPECT_EQ (std::future_status::ready, started.wait_for (heartbeat_observation_timeout));
    const auto renewed_at = location.owner_lease_renewed_at ();
    location.stop ();
    runtime::shutdown_handler_coroutine_executor ();
    EXPECT_NO_THROW (store.complete_renew (owner_lease_stale_t{}));
    EXPECT_EQ (renewed_at, location.owner_lease_renewed_at ());
}

class startup_owner_lease_repository_t final : public in_memory_location_repository_t
{
  public:
    enum class claim_mode_t
    {
        fail_first,
        hold_first,
        commit_then_fail,
        never_complete,
        generation_exhausted,
        succeed
    };

    ~startup_owner_lease_repository_t () override
    {
        if (_delayed_claim.joinable ())
            _delayed_claim.join ();
    }

    void set_claim_mode (claim_mode_t mode) { _claim_mode = mode; }

    void hold_first_claim () { _claim_mode = claim_mode_t::hold_first; }

    // Commits the held first claim from another thread, as a late provider reply would.
    void release_first_claim ()
    {
        _delayed_claim = std::thread ([this] {
            auto committed = in_memory_location_repository_t::claim_owner_lease (
              std::move (_held_owner_id), _held_lease_ttl);
            _held_claim->complete (committed.result ());
        });
    }

    void cancel_after_next_claim (std::stop_source &cancellation) { _cancellation = &cancellation; }

    void fail_owner_lease_reads () noexcept { _fail_reads = true; }

    void fail_owner_lease_releases () noexcept { _fail_releases = true; }

    void block_owner_lease_releases ()
    {
        _block_releases = true;
        _pending_release = std::make_shared<zlink::framework::task_completion_source_t<
          zlink::framework::owner_lease_release_result_t>> ();
    }

    auto release_started () { return _release_started.task (); }

    void complete_pending_release ()
    {
        _pending_release->complete (
          zlink::framework::result_t<zlink::framework::owner_lease_release_result_t>::success (
            zlink::framework::owner_lease_release_result_t{
              zlink::framework::owner_lease_released_t{}}));
    }

    int claim_calls () const noexcept { return _claim_calls.load (); }

    int release_calls () const noexcept { return _release_calls.load (); }

    int read_calls () const noexcept { return _read_calls.load (); }

    zlink::framework::task_t<zlink::framework::owner_lease_read_result_t>
    read_owner_lease (std::string owner_id) override
    {
        _read_calls.fetch_add (1);
        if (_fail_reads) {
            return zlink::framework::task_t<zlink::framework::owner_lease_read_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_read_result_t>::failure (
                zlink::framework::framework_error_kind_t::unavailable,
                "injected owner lease confirmation read failure"));
        }
        return in_memory_location_repository_t::read_owner_lease (std::move (owner_id));
    }

    zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t>
    claim_owner_lease (std::string owner_id, std::chrono::milliseconds lease_ttl) override
    {
        const auto call = _claim_calls.fetch_add (1);
        if (_claim_mode == claim_mode_t::fail_first && call == 0) {
            return zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_claim_result_t>::failure (
                zlink::framework::framework_error_kind_t::unavailable,
                "injected owner lease provider failure"));
        }
        if (_claim_mode == claim_mode_t::hold_first && call == 0) {
            _held_claim = std::make_shared<zlink::framework::task_completion_source_t<
              zlink::framework::owner_lease_claim_result_t>> ();
            _held_owner_id = std::move (owner_id);
            _held_lease_ttl = lease_ttl;
            return _held_claim->task ();
        }
        if (_claim_mode == claim_mode_t::commit_then_fail) {
            in_memory_location_repository_t::claim_owner_lease (std::move (owner_id), lease_ttl)
              .result ()
              .value ();
            if (_cancellation != nullptr) {
                _cancellation->request_stop ();
                _cancellation = nullptr;
            }
            return zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_claim_result_t>::failure (
                zlink::framework::framework_error_kind_t::unavailable,
                "injected owner lease response failure"));
        }
        if (_claim_mode == claim_mode_t::never_complete) {
            _pending_claim = std::make_shared<zlink::framework::task_completion_source_t<
              zlink::framework::owner_lease_claim_result_t>> ();
            if (_cancellation != nullptr) {
                _cancellation->request_stop ();
                _cancellation = nullptr;
            }
            return _pending_claim->task ();
        }
        if (_claim_mode == claim_mode_t::generation_exhausted) {
            return zlink::framework::task_t<zlink::framework::owner_lease_claim_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_claim_result_t>::success (
                zlink::framework::owner_lease_claim_result_t{
                  zlink::framework::owner_lease_generation_exhausted_t{}}));
        }
        auto result =
          in_memory_location_repository_t::claim_owner_lease (std::move (owner_id), lease_ttl);
        if (_cancellation != nullptr) {
            _cancellation->request_stop ();
            _cancellation = nullptr;
        }
        return result;
    }

    zlink::framework::task_t<zlink::framework::owner_lease_release_result_t>
    release_owner_lease (zlink::framework::location_owner_token_t token) override
    {
        _release_calls.fetch_add (1);
        _release_started.complete (zlink::framework::result_t<void>::success ());
        if (_block_releases) {
            return _pending_release->task ();
        }
        if (_fail_releases) {
            return zlink::framework::task_t<zlink::framework::owner_lease_release_result_t> (
              zlink::framework::result_t<zlink::framework::owner_lease_release_result_t>::failure (
                zlink::framework::framework_error_kind_t::unavailable,
                "injected owner lease release failure"));
        }
        return in_memory_location_repository_t::release_owner_lease (std::move (token));
    }

  private:
    claim_mode_t _claim_mode = claim_mode_t::fail_first;
    std::stop_source *_cancellation = nullptr;
    std::thread _delayed_claim;
    std::shared_ptr<
      zlink::framework::task_completion_source_t<zlink::framework::owner_lease_claim_result_t>>
      _held_claim;
    std::string _held_owner_id;
    std::chrono::milliseconds _held_lease_ttl{0};
    std::shared_ptr<
      zlink::framework::task_completion_source_t<zlink::framework::owner_lease_claim_result_t>>
      _pending_claim;
    std::shared_ptr<
      zlink::framework::task_completion_source_t<zlink::framework::owner_lease_release_result_t>>
      _pending_release;
    zlink::framework::task_completion_source_t<void> _release_started;
    std::atomic_int _claim_calls = 0;
    std::atomic_int _release_calls = 0;
    std::atomic_int _read_calls = 0;
    bool _fail_reads = false;
    bool _fail_releases = false;
    bool _block_releases = false;
};

TEST (ZLinkFrameworkLocationRuntime, ClaimsAndReleasesOwnerLease)
{
    in_memory_location_repository_t store;
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");

    runtime.start (zlink::routing_id_t::from ("node-a"));
    EXPECT_TRUE (runtime.owner_lease_healthy ());
    EXPECT_TRUE (std::holds_alternative<zlink::framework::owner_lease_found_t> (
      store.read_owner_lease ("owner-a").result ().value ()));

    runtime.stop ();
    EXPECT_TRUE (std::holds_alternative<zlink::framework::owner_lease_missing_t> (
      store.read_owner_lease ("owner-a").result ().value ()));
}

TEST (ZLinkFrameworkLocationRuntime, BoundsNonCooperativeOwnerCleanup)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::succeed);
    store.block_owner_lease_releases ();
    const auto cleanup_timeout = std::chrono::milliseconds (20);
    location_runtime_t runtime (store, location_options_t{}, "owner-a");
    // Prepare the lease with the normal startup deadline, without a heartbeat.
    // The cleanup under test keeps its separate 20ms absolute deadline.
    runtime.renew_owner_lease_once ();
    ASSERT_TRUE (runtime.current_owner_token ().has_value ());
    auto release_started = store.release_started ();

    auto cleanup = std::async (std::launch::async, [&] {
        return runtime.cleanup_owner (std::chrono::steady_clock::now () + cleanup_timeout);
    });
    const auto started = release_started.result_for (std::chrono::seconds (1));
    if (!started) {
        store.complete_pending_release ();
        EXPECT_FALSE (cleanup.get ());
        FAIL () << "owner lease release did not start";
    }
    ASSERT_TRUE (*started);
    const auto status = cleanup.wait_for (cleanup_timeout + std::chrono::seconds (1));
    EXPECT_EQ (std::future_status::timeout, status);
    store.complete_pending_release ();
    EXPECT_FALSE (cleanup.get ());
    ASSERT_TRUE (runtime.last_error ().has_value ());
    EXPECT_NE (std::string::npos, runtime.last_error ()->find ("timed out"));
}

TEST (ZLinkFrameworkLocationRuntime, HostCleanupUsesShutdownDeadline)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::succeed);
    store.block_owner_lease_releases ();
    auto runtime = std::make_unique<location_runtime_t> (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::seconds (1),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = std::chrono::seconds (2)},
      "owner-a");
    runtime->start (zlink::routing_id_t::from ("node-a"));
    auto *location = runtime.get ();
    auto host = zlink::framework::app_t::create ();
    host.advanced ().services ().add_singleton (std::move (runtime));
    const auto deadline = std::chrono::milliseconds (200);

    auto shutdown = host.shutdown (deadline);
    auto completed = zlink::framework::detail::observe_task_result_for (
      shutdown, deadline + std::chrono::seconds (1), std::stop_token{});
    EXPECT_FALSE (completed.has_value ());
    store.complete_pending_release ();
    completed = zlink::framework::detail::observe_task_result_for (
      shutdown, std::chrono::seconds (1), std::stop_token{});
    ASSERT_TRUE (completed.has_value ());
    ASSERT_TRUE (completed->has_value ());
    EXPECT_EQ (zlink::framework::termination_outcome_t::force_stopped, completed->value ().outcome);
    EXPECT_EQ (zlink::framework::termination_reason_t::deadline_exceeded,
               completed->value ().reason);
    EXPECT_EQ (1, store.release_calls ());
    ASSERT_TRUE (location->last_error ().has_value ());
    EXPECT_NE (std::string::npos, location->last_error ()->find ("timed out"));
}

TEST (ZLinkFrameworkLocationRuntime, HostCleanupPreservesProviderFailure)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::succeed);
    store.fail_owner_lease_releases ();
    auto runtime = std::make_unique<location_runtime_t> (store, location_options_t{}, "owner-a");
    runtime->start (zlink::routing_id_t::from ("node-a"));
    auto host = zlink::framework::app_t::create ();
    host.advanced ().services ().add_singleton (std::move (runtime));

    const auto completed = host.shutdown (std::chrono::seconds (1)).result ().value ();
    EXPECT_EQ (zlink::framework::termination_outcome_t::force_stopped, completed.outcome);
    EXPECT_EQ (zlink::framework::termination_reason_t::teardown_failed, completed.reason);
    EXPECT_EQ (1, store.release_calls ());
}

TEST (ZLinkFrameworkLocationRuntime, CleanupStopsPendingHeartbeatWithinDeadline)
{
    pending_renew_repository_t store;
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");
    auto renew_started = store.renew_started ();
    runtime.start (zlink::routing_id_t::from ("node-a"));
    ASSERT_EQ (std::future_status::ready, renew_started.wait_for (std::chrono::seconds (5)));
    const auto started_at = std::chrono::steady_clock::now ();
    const auto deadline = std::chrono::milliseconds (20);

    EXPECT_TRUE (runtime.cleanup_owner (started_at + deadline));
    EXPECT_LT (std::chrono::steady_clock::now () - started_at, deadline + std::chrono::seconds (1));
    store.complete_renew (zlink::framework::owner_lease_stale_t{});
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
}

TEST (ZLinkFrameworkLocationRuntime, StartsDegradedAfterInitialOwnerLeaseClaimFailure)
{
    startup_owner_lease_repository_t store;
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");

    EXPECT_NO_THROW (runtime.start (zlink::routing_id_t::from ("node-a")));
    EXPECT_FALSE (runtime.owner_lease_healthy ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());

    for (int attempt = 0; attempt != 50 && !runtime.current_owner_token (); ++attempt)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    EXPECT_TRUE (runtime.owner_lease_healthy ());
    EXPECT_TRUE (runtime.current_owner_token ().has_value ());
    EXPECT_GE (store.claim_calls (), 2);

    runtime.stop ();
}

TEST (ZLinkFrameworkLocationRuntime, RecordsInitialClaimConfirmationReadFailure)
{
    startup_owner_lease_repository_t store;
    store.fail_owner_lease_reads ();
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::seconds (1),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");

    EXPECT_NO_THROW (runtime.start (zlink::routing_id_t::from ("node-a")));
    ASSERT_TRUE (runtime.last_error ().has_value ());
    EXPECT_EQ ("injected owner lease confirmation read failure", *runtime.last_error ());
    EXPECT_EQ (1, store.read_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());

    runtime.stop ();
}

TEST (ZLinkFrameworkLocationRuntime, InstallsLateCommittedClaimAfterHeartbeatConflict)
{
    startup_owner_lease_repository_t store;
    store.hold_first_claim ();
    const auto renew_timeout = std::chrono::milliseconds (20);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (40),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = renew_timeout},
      "owner-a");

    // The first claim stays pending, so start() returns only through the renew timeout.
    runtime.start (zlink::routing_id_t::from ("node-a"));

    EXPECT_FALSE (runtime.owner_lease_healthy ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());

    store.release_first_claim ();
    for (int attempt = 0; attempt != 250 && !runtime.current_owner_token (); ++attempt)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    EXPECT_EQ (0, store.release_calls ());
    EXPECT_TRUE (runtime.owner_lease_healthy ());
    ASSERT_TRUE (runtime.current_owner_token ().has_value ());
    EXPECT_EQ (1, runtime.current_owner_token ()->lease_generation);

    runtime.stop ();
}

TEST (ZLinkFrameworkLocationRuntime, RejectsInitialOwnerLeaseClaimConflict)
{
    in_memory_location_repository_t store;
    const auto existing =
      store.claim_owner_lease ("owner-a", std::chrono::seconds (15)).result ().value ();
    ASSERT_TRUE (std::holds_alternative<zlink::framework::owner_lease_claimed_t> (existing));
    location_runtime_t runtime (store, {}, "owner-a");

    EXPECT_THROW (runtime.start (zlink::routing_id_t::from ("node-a")), std::runtime_error);
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
}

TEST (ZLinkFrameworkLocationRuntime, RejectsInitialOwnerLeaseClaimGenerationExhaustion)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::generation_exhausted);
    location_runtime_t runtime (store, {}, "owner-a");

    EXPECT_THROW (runtime.start (zlink::routing_id_t::from ("node-a")), std::runtime_error);
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
}

TEST (ZLinkFrameworkLocationRuntime, ReleasesCommittedClaimWhenStartupIsCancelled)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::succeed);
    std::stop_source cancellation;
    store.cancel_after_next_claim (cancellation);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15)},
      "owner-a");

    try {
        runtime.start (zlink::routing_id_t::from ("node-a"), cancellation.get_token ());
        FAIL () << "startup cancellation was not propagated";
    }
    catch (const std::system_error &error) {
        EXPECT_EQ (std::make_error_code (std::errc::operation_canceled), error.code ());
    }
    EXPECT_EQ (1, store.release_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    EXPECT_EQ (1, store.claim_calls ());
}

TEST (ZLinkFrameworkLocationRuntime, BoundsNonCooperativeClaimAndPreservesCancellation)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::never_complete);
    std::stop_source cancellation;
    store.cancel_after_next_claim (cancellation);
    const auto renew_timeout = std::chrono::milliseconds (20);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = renew_timeout},
      "owner-a");

    const auto started_at = std::chrono::steady_clock::now ();
    try {
        runtime.start (zlink::routing_id_t::from ("node-a"), cancellation.get_token ());
        FAIL () << "startup cancellation was not propagated";
    }
    catch (const std::system_error &error) {
        EXPECT_EQ (std::make_error_code (std::errc::operation_canceled), error.code ());
    }
    EXPECT_LT (std::chrono::steady_clock::now () - started_at, renew_timeout);
    EXPECT_EQ (1, store.read_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    EXPECT_EQ (1, store.claim_calls ());
}

TEST (ZLinkFrameworkLocationRuntime, RecordsCleanupFailureAndPreservesCancellation)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::succeed);
    store.fail_owner_lease_releases ();
    std::stop_source cancellation;
    store.cancel_after_next_claim (cancellation);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = std::chrono::milliseconds (20)},
      "owner-a");

    try {
        runtime.start (zlink::routing_id_t::from ("node-a"), cancellation.get_token ());
        FAIL () << "startup cancellation was not propagated";
    }
    catch (const std::system_error &error) {
        EXPECT_EQ (std::make_error_code (std::errc::operation_canceled), error.code ());
    }
    ASSERT_TRUE (runtime.last_error ().has_value ());
    EXPECT_EQ ("injected owner lease release failure", *runtime.last_error ());
    EXPECT_EQ (1, store.release_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    EXPECT_EQ (1, store.claim_calls ());
}

TEST (ZLinkFrameworkLocationRuntime, BoundsNonCooperativeCancellationRelease)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::succeed);
    store.block_owner_lease_releases ();
    std::stop_source cancellation;
    store.cancel_after_next_claim (cancellation);
    const auto renew_timeout = std::chrono::milliseconds (20);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = renew_timeout},
      "owner-a");

    const auto started_at = std::chrono::steady_clock::now ();
    try {
        runtime.start (zlink::routing_id_t::from ("node-a"), cancellation.get_token ());
        FAIL () << "startup cancellation was not propagated";
    }
    catch (const std::system_error &error) {
        EXPECT_EQ (std::make_error_code (std::errc::operation_canceled), error.code ());
    }
    EXPECT_LT (std::chrono::steady_clock::now () - started_at,
               renew_timeout + std::chrono::milliseconds (60));
    EXPECT_EQ (1, store.release_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    EXPECT_EQ (1, store.claim_calls ());
}

TEST (ZLinkFrameworkLocationRuntime, ReleasesLateCommitWithoutInstalledTokenOnCancellation)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::commit_then_fail);
    std::stop_source cancellation;
    store.cancel_after_next_claim (cancellation);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (5),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = std::chrono::milliseconds (20)},
      "owner-a");

    try {
        runtime.start (zlink::routing_id_t::from ("node-a"), cancellation.get_token ());
        FAIL () << "startup cancellation was not propagated";
    }
    catch (const std::system_error &error) {
        EXPECT_EQ (std::make_error_code (std::errc::operation_canceled), error.code ());
    }
    EXPECT_EQ (1, store.read_calls ());
    EXPECT_EQ (1, store.release_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());
    const auto remaining = store.read_owner_lease ("owner-a").result ().value ();
    EXPECT_TRUE (std::holds_alternative<zlink::framework::owner_lease_missing_t> (remaining));
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    EXPECT_EQ (1, store.claim_calls ());
}

TEST (ZLinkFrameworkLocationRuntime, DoesNotReadAfterClaimConsumesRenewDeadline)
{
    startup_owner_lease_repository_t store;
    store.set_claim_mode (startup_owner_lease_repository_t::claim_mode_t::never_complete);
    const auto renew_timeout = std::chrono::milliseconds (20);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::seconds (1),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = renew_timeout},
      "owner-a");

    const auto started_at = std::chrono::steady_clock::now ();
    EXPECT_NO_THROW (runtime.start (zlink::routing_id_t::from ("node-a")));
    EXPECT_GE (std::chrono::steady_clock::now () - started_at, renew_timeout);
    EXPECT_EQ (0, store.read_calls ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());

    runtime.stop ();
}

} // namespace

TEST (ZLinkFrameworkLocationRuntime, ForceStop2646ObservesPendingCleanupBeforeReturn)
{
    using namespace zlink::framework;
    class pending_cleanup_t final : public in_memory_location_repository_t
    {
      public:
        task_completion_source_t<std::int64_t> completion;
        std::promise<void> entered;
        std::atomic<int> releases{0};
        task_t<std::int64_t> remove_all_by_owner (location_owner_token_t) override
        {
            entered.set_value ();
            return completion.task ();
        }
        task_t<owner_lease_release_result_t>
        release_owner_lease (location_owner_token_t owner) override
        {
            ++releases;
            return in_memory_location_repository_t::release_owner_lease (std::move (owner));
        }
    } store;
    location_runtime_t runtime (store, {}, "cleanup-owner");
    runtime.renew_owner_lease_once ();
    auto entered = store.entered.get_future ();
    EXPECT_FALSE (runtime.cleanup_owner (std::chrono::steady_clock::now ()));
    EXPECT_EQ (std::future_status::timeout, entered.wait_for (std::chrono::milliseconds (0)));
    EXPECT_EQ (0, store.releases.load ());
    auto stopping = std::async (std::launch::async, [&] {
        return runtime.cleanup_owner (std::chrono::steady_clock::now ()
                                      + std::chrono::milliseconds (20));
    });
    EXPECT_EQ (std::future_status::ready, entered.wait_for (std::chrono::seconds (1)));
    EXPECT_EQ (std::future_status::timeout, stopping.wait_for (std::chrono::milliseconds (40)));
    store.completion.complete (result_t<std::int64_t>::success (0));
    EXPECT_FALSE (stopping.get ());
    EXPECT_EQ (0, store.releases.load ());
}


namespace
{
using namespace zlink::framework;
using namespace zlink::framework::runtime;
constexpr auto auto_connect_wait = std::chrono::seconds (5);

// The old synchronous implementation is normalized only at the test boundary.
template <typename Route> task_t<void> route_auto_update (Route *route, bool remove)
{
    const auto rid = zlink::routing_id_t::from ("remote");
    if constexpr (requires { route->connect_async (rid, std::string{}); }) {
        if (remove)
            EXPECT_TRUE (co_await route->disconnect_async (rid, "inproc://l4-peer"));
        else
            EXPECT_TRUE (co_await route->connect_async (rid, "inproc://l4-peer"));
    } else {
        if (remove)
            EXPECT_TRUE (route->disconnect (rid, "inproc://l4-peer"));
        else
            EXPECT_TRUE (route->connect (rid, "inproc://l4-peer"));
    }
}

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
    check_auto_owner_wait (auto_connect_owner_lane (route),
                           [&] { return route_auto_update (&route, remove); });
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
class ready_auto_store_t : public in_memory_location_repository_t
{
  public:
    task_t<location_page_t<mesh_node_descriptor_t>>
    list_mesh_nodes (std::string, location_page_request_t) override
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

namespace
{
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
    node->start ();
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
