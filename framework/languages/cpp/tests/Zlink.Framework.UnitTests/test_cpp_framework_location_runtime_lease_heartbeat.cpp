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
#include "../support/location_runtime_pending_renew_fixture.hpp"
#include "../support/location_runtime_lease_timing.hpp"

namespace
{
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
    options.owner_lease_ttl = std::chrono::seconds (15);
    options.owner_lease_renew_interval = std::chrono::seconds (1);
    options.owner_lease_renew_timeout = std::chrono::seconds (8);
    EXPECT_THROW (zlink::framework::detail::validate_location_options (options),
                  zlink::framework::framework_exception_t);
    options.owner_lease_ttl = std::chrono::seconds (30);
    EXPECT_NO_THROW (zlink::framework::detail::validate_location_options (options));
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
} // namespace
