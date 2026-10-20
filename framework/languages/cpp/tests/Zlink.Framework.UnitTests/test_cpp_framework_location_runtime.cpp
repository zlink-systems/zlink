/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"

#include "../support/runtime_metric_contract.hpp"

#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include <zlink/framework/contracts/configuration/detail/framework_options_validation.hpp>

#include <zlink/framework/contracts/configuration/app.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <future>
#include <latch>
#include <stop_token>
#include <thread>

namespace zlink::framework::runtime
{
const auto requested_peer_rid = zlink::routing_id_t::from ("requested-peer");

struct location_runtime_test_access_t
{
    template <typename Now>
    static std::chrono::milliseconds first_heartbeat_delay (location_runtime_t &runtime, Now now)
    {
        const auto claim_started_at = now ();
        runtime.renew_owner_lease_once ();
        auto heartbeat = std::make_shared<location_runtime_t::heartbeat_owner_t> ();
        auto delay = std::chrono::milliseconds::zero ();
        runtime.heartbeat_loop (heartbeat, claim_started_at, now,
                                [&] (auto &, auto &, auto remaining, auto, auto...) {
                                    delay = remaining;
                                    heartbeat->stop.store (true, std::memory_order_release);
                                });
        return delay;
    }
    static state_lane_t &lane (location_runtime_t &runtime) { return runtime._lane; }
    static auto snapshot (const location_runtime_t &runtime)
    {
        return std::atomic_load_explicit (&runtime._owner_lease, std::memory_order_acquire);
    }
    static void set_deadline (location_runtime_t &runtime,
                              std::chrono::steady_clock::time_point deadline)
    {
        runtime._lane
          .run_checked ([&] {
              runtime.update_owner_lease_on_lane (
                [&] (auto &lease) { lease.admission_deadline = deadline; });
          })
          .get ();
    }
    static bool usable_at (const location_runtime_t &runtime,
                           std::chrono::steady_clock::time_point now)
    {
        return location_runtime_t::owner_lease_usable (*snapshot (runtime), now);
    }
};
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
      handlers, serializers, {}, {}, {}, {}, [&] (location_write_intent_t) {
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
          return true;
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
    runtime.register_metric_scope ("mesh", "lateness-mesh");
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
        delay_first,
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

    void delay_first_claim (std::chrono::milliseconds delay)
    {
        _claim_mode = claim_mode_t::delay_first;
        _claim_delay = delay;
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
        if (_claim_mode == claim_mode_t::delay_first && call == 0) {
            auto completion = std::make_shared<zlink::framework::task_completion_source_t<
              zlink::framework::owner_lease_claim_result_t>> ();
            auto pending = completion->task ();
            _delayed_claim = std::thread (
              [this, completion, owner_id = std::move (owner_id), lease_ttl] () mutable {
                  std::this_thread::sleep_for (_claim_delay);
                  auto committed = in_memory_location_repository_t::claim_owner_lease (
                    std::move (owner_id), lease_ttl);
                  completion->complete (committed.result ());
              });
            return pending;
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
    std::chrono::milliseconds _claim_delay{0};
    std::thread _delayed_claim;
    std::shared_ptr<
      zlink::framework::task_completion_source_t<zlink::framework::owner_lease_claim_result_t>>
      _pending_claim;
    std::shared_ptr<
      zlink::framework::task_completion_source_t<zlink::framework::owner_lease_release_result_t>>
      _pending_release;
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
    const auto renew_timeout = std::chrono::milliseconds (20);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::seconds (1),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = renew_timeout},
      "owner-a");
    runtime.start (zlink::routing_id_t::from ("node-a"));

    auto cleanup = std::async (std::launch::async, [&] { return runtime.cleanup_owner (); });
    const auto status = cleanup.wait_for (renew_timeout + std::chrono::seconds (1));
    EXPECT_EQ (std::future_status::ready, status);
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
    store.complete_pending_release ();
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
    store.delay_first_claim (std::chrono::milliseconds (50));
    const auto renew_timeout = std::chrono::milliseconds (20);
    location_runtime_t runtime (
      store,
      location_options_t{.owner_lease_renew_interval = std::chrono::milliseconds (40),
                         .owner_lease_ttl = std::chrono::seconds (15),
                         .owner_lease_renew_timeout = renew_timeout},
      "owner-a");

    const auto started_at = std::chrono::steady_clock::now ();
    runtime.start (zlink::routing_id_t::from ("node-a"));
    const auto startup_duration = std::chrono::steady_clock::now () - started_at;

    EXPECT_LT (startup_duration, renew_timeout + std::chrono::milliseconds (60));
    EXPECT_FALSE (runtime.owner_lease_healthy ());
    EXPECT_FALSE (runtime.current_owner_token ().has_value ());

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
