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
#include "../support/location_runtime_lease_timing.hpp"

namespace zlink::framework::runtime
{
const auto requested_peer_rid = zlink::routing_id_t::from ("requested-peer");
} // namespace zlink::framework::runtime

namespace
{
using zlink::framework::location_options_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::location_runtime_t;

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
    list_mesh_nodes_core (std::string mesh, zlink::framework::location_page_request_t = {}) override
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
    list_mesh_nodes_core (std::string, zlink::framework::location_page_request_t = {}) override
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
    list_mesh_nodes_core (std::string mesh, zlink::framework::location_page_request_t = {}) override
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
} // namespace
