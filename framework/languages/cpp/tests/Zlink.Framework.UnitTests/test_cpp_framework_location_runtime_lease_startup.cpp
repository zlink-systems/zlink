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
