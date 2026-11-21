/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/opaque_store_descriptor_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "../support/location_reclaim_clock.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/host/app_test_access.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include <fstream>
#include <condition_variable>
#include <zlink/framework/contracts/configuration/app.hpp>

#include <gtest/gtest.h>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../support/opaque_store_record_fixture.hpp"
#include "../support/opaque_store_startup_fixture.hpp"

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace zlink::framework::test::opaque_store;

class reclaim_count_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        for (const auto &mutation : request.mutations)
            if (const auto *put = std::get_if<store_put_t> (&mutation);
                put && put->key.value.starts_with ("mesh-node"))
                mesh_claims.fetch_add (1);
        return inner.write (std::move (request));
    }
    in_memory_location_store_t inner;
    std::atomic<unsigned> mesh_claims{0};
};

class reclaim_count_memory_repository_t final : public in_memory_location_repository_t
{
  public:
    explicit reclaim_count_memory_repository_t (std::atomic<unsigned> &claims) : claims (claims) {}
    task_t<location_write_result_t> update_mesh_node (mesh_node_descriptor_t descriptor,
                                                      location_write_intent_t intent) override
    {
        auto result = co_await in_memory_location_repository_t::update_mesh_node (
          std::move (descriptor), intent);
        if ((intent == location_write_intent_t::new_claim
             || intent == location_write_intent_t::fixed_rid_new_claim)
            && result.status == location_write_status_t::stored)
            claims.fetch_add (1);
        co_return result;
    }

  private:
    std::atomic<unsigned> &claims;
};

class fixed_rid_startup_probe_t : public hosted_service_t
{
  public:
    std::function<void (framework_runtime_state_t)> status_changed;
    task_t<void> start (service_provider_t &services) override
    {
        runtime = &services.get_required<location_runtime_t> ();
        observation = services.get_required<zlink::framework::framework_runtime_t> ().observe (
          1, [this] (const auto &event) {
              {
                  const std::lock_guard lock (mutex);
                  ready = event.status.state == framework_runtime_state_t::serving;
                  changed.notify_all ();
              }
              if (status_changed)
                  status_changed (event.status.state);
          });
        const std::lock_guard lock (mutex);
        started = true;
        changed.notify_all ();
        co_return;
    }
    void stop () noexcept override
    {
        if (observation)
            observation->close ();
    }
    void clock_observed ()
    {
        const std::lock_guard lock (mutex);
        ++reads;
        changed.notify_all ();
    }
    bool wait_started ()
    {
        std::unique_lock lock (mutex);
        return changed.wait_for (lock, 5s, [&] { return started; });
    }
    bool wait_reads (unsigned count)
    {
        std::unique_lock lock (mutex);
        return changed.wait_for (lock, 5s, [&] { return reads >= count; });
    }
    bool wait_observed_or_ready ()
    {
        std::unique_lock lock (mutex);
        return changed.wait_for (lock, 5s, [&] { return reads > 0 || ready; });
    }
    bool owner_lease_healthy () { return runtime->owner_lease_healthy (); }
    auto current_owner () { return runtime->current_owner_token (); }
    unsigned read_count ()
    {
        const std::lock_guard lock (mutex);
        return reads;
    }
    bool wait_ready ()
    {
        std::unique_lock lock (mutex);
        return changed.wait_for (lock, 5s, [&] { return ready; });
    }

  private:
    location_runtime_t *runtime = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool ready = false;
    unsigned reads = 0;
    std::unique_ptr<runtime_observation_t> observation;
};

void configure_review_host (app_t &app,
                            const std::shared_ptr<startup_review_store_t> &store,
                            const std::shared_ptr<location_repository_t> &repository,
                            unsigned meshes = 1)
{
    auto &options = app.add_zlink_framework ();
    options.add_location_store (store);
    options.add_relocation_store (std::make_shared<in_memory_relocation_store_t> ());
    auto &locations = options.configure_locations ();
    locations.owner_lease_renew_interval = 10ms;
    locations.owner_lease_fencing_margin = 7ms;
    for (unsigned index = 0; index < meshes; ++index) {
        const auto name = "startup-review-" + std::to_string (index);
        auto mesh = options.add_route_mesh (name);
        mesh.channel_name (name + "-channel").server ();
        mesh.set_object_role (object_role_t::none)
          .set_routing_id (zlink::routing_id_t::from (name))
          .listen ("inproc://" + name);
    }
    app.advanced ().services ().add_factory<location_repository_t> (
      [repository] (service_provider_t &) { return repository; }, service_lifetime_t::singleton);
}

TEST (CppFrameworkOpaqueLocationStore, FixedRidCompetingStartupRejectsLoserAfterPreparing)
{
    const auto verify = [] (unsigned mode) {
        SCOPED_TRACE (mode); // in-memory, provider, provider CAS loser
        auto store = std::make_shared<startup_review_store_t> ();
        store->hold_claims = mode == 2;
        std::shared_ptr<location_repository_t> repository =
          mode ? std::static_pointer_cast<location_repository_t> (
                   std::make_shared<provider_location_repository_t> (*store))
               : std::static_pointer_cast<location_repository_t> (
                   std::make_shared<in_memory_location_repository_t> ());
        location_reclaim_test_access_t clock;
        clock.bind (*repository);
        auto first = app_t::create ();
        auto second = app_t::create ();
        std::array<app_t *, 2> apps{&first, &second};
        std::array<fixed_rid_startup_probe_t *, 2> probes;
        std::array<location_owner_token_t, 2> owners;
        std::array<std::jthread, 2> running;
        std::array<int, 2> exits{-1, -1};
        std::array<framework_runtime_state_t, 2> states{framework_runtime_state_t::preparing,
                                                        framework_runtime_state_t::preparing};
        std::mutex mutex;
        std::condition_variable changed;
        struct stop_on_exit_t
        {
            std::array<app_t *, 2> &apps;
            startup_review_store_t &store;
            std::array<std::jthread, 2> &running;
            ~stop_on_exit_t ()
            {
                store.release_all ();
                for (auto *app : apps)
                    app->request_stop ();
                for (auto &thread : running)
                    if (thread.joinable ())
                        thread.join ();
            }
        } stop{apps, *store, running};
        for (unsigned index = 0; index < apps.size (); ++index) {
            configure_review_host (*apps[index], store, repository);
            auto probe = std::make_unique<fixed_rid_startup_probe_t> ();
            probes[index] = probe.get ();
            probe->status_changed = [&, index] (auto state) {
                const std::lock_guard lock (mutex);
                states[index] = state;
                changed.notify_all ();
            };
            apps[index]->add_hosted_service (std::move (probe));
            running[index] = std::jthread ([&, index] {
                char program[] = "competing-startup";
                char *arguments[] = {program, nullptr};
                exits[index] = apps[index]->run (1, arguments);
                const std::lock_guard lock (mutex);
                states[index] = apps[index]->runtime_state ();
                changed.notify_all ();
            });
            ASSERT_TRUE (probes[index]->wait_started ());
            EXPECT_EQ (apps[index]->runtime_state (), framework_runtime_state_t::preparing);
            const auto owner = probes[index]->current_owner ();
            ASSERT_TRUE (owner.has_value ());
            owners[index] = *owner;
        }
        clock.advance (21ms);
        if (store->hold_claims) {
            ASSERT_TRUE (store->wait_held ());
            store->releases[0].complete (result_t<void>::success ());
            std::unique_lock lock (mutex);
            ASSERT_TRUE (changed.wait_for (lock, 5s, [&] {
                return states[0] == framework_runtime_state_t::serving
                       || states[1] == framework_runtime_state_t::serving;
            }));
            lock.unlock ();
            store->releases[1].complete (result_t<void>::success ());
        }
        unsigned winner;
        {
            std::unique_lock lock (mutex);
            ASSERT_TRUE (changed.wait_for (lock, 5s, [&] {
                return (states[0] == framework_runtime_state_t::serving
                        && states[1] == framework_runtime_state_t::error)
                       || (states[1] == framework_runtime_state_t::serving
                           && states[0] == framework_runtime_state_t::error);
            }));
            winner = states[0] == framework_runtime_state_t::serving ? 0 : 1;
        }
        running[1 - winner].join ();
        EXPECT_EQ (exits[1 - winner], 1);
        EXPECT_EQ (apps[winner]->runtime_state (), framework_runtime_state_t::serving);
        const auto descriptors =
          repository->list_mesh_nodes ("startup-review-0").result ().value ();
        ASSERT_EQ (descriptors.items.size (), 1u);
        EXPECT_EQ (descriptors.items.front ().owner_id, owners[winner].owner_id);
        EXPECT_EQ (descriptors.items.front ().lease_generation, owners[winner].lease_generation);
        if (store->hold_claims)
            EXPECT_GE (store->conflicts.load (), 1u);
        apps[winner]->request_stop ();
        running[winner].join ();
        EXPECT_EQ (exits[winner], 0);
    };
    for (unsigned mode = 0; mode < 3; ++mode)
        verify (mode);
}

class startup_shutdown_probe_t final : public fixed_rid_startup_probe_t,
                                       public hosted_service_lifecycle_t
{
  public:
    task_completion_source_t<void> start_release;
    std::function<void ()> before_stop, on_seal;

    task_t<void> start (service_provider_t &services) override
    {
        co_await fixed_rid_startup_probe_t::start (services);
        co_await start_release.task ();
    }
    int shutdown_stop_priority () const noexcept override
    {
        return std::numeric_limits<int>::max ();
    }
    void stop () noexcept override
    {
        if (before_stop)
            before_stop ();
        fixed_rid_startup_probe_t::stop ();
    }
    void seal_application_dispatch () noexcept override
    {
        if (on_seal)
            on_seal ();
    }
};

void verify_startup_failure_joins_shutdown (bool handoff_before_run_check)
{
    auto store = std::make_shared<startup_review_store_t> ();
    auto repository = std::make_shared<provider_location_repository_t> (*store);
    location_reclaim_test_access_t clock;
    clock.bind (*repository);
    auto first = app_t::create ();
    auto second = app_t::create ();
    std::array<app_t *, 2> apps{&first, &second};
    std::array<startup_shutdown_probe_t *, 2> probes{};
    std::array<std::jthread, 2> running;
    std::array<int, 2> exits{-1, -1};
    std::jthread handoff_watch;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread::id shutdown_thread;
    bool scan_held = false, scan_released = false, stop_started = false, handed_off = false;
    std::atomic_bool hold_mesh_reads{false};
    unsigned held_reads = 0;
    std::optional<unsigned> winner_index, loser_index;
    std::array<task_completion_source_t<void>, 2> read_release;
    task_completion_source_t<void> scan_release;
    std::vector<task_t<bool>> publications;
    struct stop_on_exit_t
    {
        std::array<app_t *, 2> &apps;
        std::array<startup_shutdown_probe_t *, 2> &probes;
        std::array<std::jthread, 2> &running;
        std::jthread &handoff_watch;
        std::array<task_completion_source_t<void>, 2> &read_release;
        task_completion_source_t<void> &scan_release;
        ~stop_on_exit_t ()
        {
            for (auto &release : read_release)
                release.complete (result_t<void>::success ());
            scan_release.complete (result_t<void>::success ());
            for (auto *probe : probes)
                if (probe)
                    probe->start_release.complete (result_t<void>::success ());
            for (auto *app : apps)
                app->request_stop ();
            for (auto &thread : running)
                if (thread.joinable ())
                    thread.join ();
            handoff_watch.request_stop ();
            if (handoff_watch.joinable ())
                handoff_watch.join ();
        }
    } stop{apps, probes, running, handoff_watch, read_release, scan_release};
    store->before_read = [&] (const auto &key) -> task_t<void> {
        if (hold_mesh_reads.load () && key.value.starts_with ("mesh-node")) {
            unsigned index;
            {
                const std::lock_guard lock (mutex);
                index = held_reads++;
                changed.notify_all ();
            }
            if (index < read_release.size ())
                co_await read_release[index].task ();
        }
    };
    store->before_scan = [&] (const auto &request) -> task_t<void> {
        bool hold;
        {
            const std::lock_guard lock (mutex);
            hold = std::this_thread::get_id () == shutdown_thread
                   && request.prefix.starts_with ("mesh-node") && !scan_held;
            if (hold) {
                scan_held = true;
                changed.notify_all ();
            }
        }
        if (hold) {
            co_await scan_release.task ();
            const std::lock_guard lock (mutex);
            scan_released = true;
            changed.notify_all ();
        }
    };
    for (unsigned index = 0; index < apps.size (); ++index) {
        configure_review_host (*apps[index], store, repository);
        auto probe = std::make_unique<startup_shutdown_probe_t> ();
        probes[index] = probe.get ();
        probe->on_seal = [&] {
            const std::lock_guard lock (mutex);
            shutdown_thread = std::this_thread::get_id ();
        };
        probe->before_stop = [&, index] {
            std::unique_lock lock (mutex);
            if (loser_index != index)
                return;
            stop_started = true;
            changed.notify_all ();
            EXPECT_TRUE (scan_released)
              << "hosted-service stop preceded the shutdown descriptor read completion";
            EXPECT_TRUE (detail::app_test_access_t::stop_requested (*apps[index])
                           .load (std::memory_order_acquire));
            // Let the old implementation fail without racing descriptor destruction.
            if (!scan_released) {
                lock.unlock ();
                scan_release.complete (result_t<void>::success ());
                lock.lock ();
            }
            EXPECT_TRUE (changed.wait_for (lock, 5s, [&] { return handed_off; }));
        };
        apps[index]->add_hosted_service (std::move (probe));
        running[index] = std::jthread ([&, index] {
            char program[] = "startup-shutdown";
            char *arguments[] = {program, nullptr};
            exits[index] = apps[index]->run (1, arguments);
        });
        ASSERT_TRUE (probes[index]->wait_started ());
        EXPECT_EQ (apps[index]->runtime_state (), framework_runtime_state_t::preparing);
        publications.push_back (detail::app_test_access_t::startup_publication (*apps[index]));
        detail::observe_task_terminal (publications.back (), [&, index] (const auto &result) {
            if (result && result.value ()) {
                const std::lock_guard lock (mutex);
                winner_index = index;
                changed.notify_all ();
            }
        });
        if (!handoff_before_run_check)
            probes[index]->start_release.complete (result_t<void>::success ());
    }
    hold_mesh_reads.store (true);
    clock.advance (21ms);
    {
        std::unique_lock lock (mutex);
        ASSERT_TRUE (changed.wait_for (lock, 5s, [&] { return held_reads >= 2; }));
    }
    read_release[0].complete (result_t<void>::success ());
    unsigned winner, loser;
    {
        std::unique_lock lock (mutex);
        ASSERT_TRUE (changed.wait_for (lock, 5s, [&] { return winner_index.has_value (); }));
        winner = *winner_index;
        loser = 1 - winner;
        loser_index = loser;
    }
    probes[winner]->start_release.complete (result_t<void>::success ());
    ASSERT_TRUE (probes[winner]->wait_ready ());
    const auto winner_owner = probes[winner]->current_owner ();
    ASSERT_TRUE (winner_owner.has_value ());
    const auto &stop_requested = detail::app_test_access_t::stop_requested (*apps[loser]);
    handoff_watch = std::jthread ([&] (std::stop_token cancelled) {
        while (!cancelled.stop_requested ()) {
            if (stop_requested.load (std::memory_order_acquire)) {
                const std::lock_guard lock (mutex);
                handed_off = true;
                changed.notify_all ();
                return;
            }
            std::this_thread::yield ();
        }
    });
    auto termination = apps[loser]->shutdown ();
    {
        std::unique_lock lock (mutex);
        ASSERT_TRUE (changed.wait_for (lock, 5s, [&] { return scan_held; }));
    }
    read_release[1].complete (result_t<void>::success ());
    const auto failed = publications[loser].result_for (5s);
    ASSERT_TRUE (failed.has_value ());
    ASSERT_FALSE (*failed);
    EXPECT_EQ (failed->error_kind (), framework_error_kind_t::not_configured);
    {
        std::unique_lock lock (mutex);
        if (!handoff_before_run_check)
            EXPECT_FALSE (changed.wait_for (lock, 100ms, [&] { return stop_started; }));
    }
    scan_release.complete (result_t<void>::success ());
    if (handoff_before_run_check) {
        std::unique_lock lock (mutex);
        ASSERT_TRUE (changed.wait_for (lock, 5s, [&] { return handed_off; }));
        EXPECT_FALSE (stop_started);
        lock.unlock ();
        probes[loser]->start_release.complete (result_t<void>::success ());
    }
    const auto terminated = termination.result_for (5s);
    ASSERT_TRUE (terminated.has_value ());
    EXPECT_TRUE (*terminated);
    EXPECT_EQ (terminated->value ().outcome, termination_outcome_t::stopped);
    EXPECT_EQ (terminated->value ().reason, termination_reason_t::none);
    running[loser].join ();
    EXPECT_EQ (exits[loser], 1);
    EXPECT_EQ (apps[loser]->runtime_state (), framework_runtime_state_t::error);
    EXPECT_TRUE (stop_started);
    const auto descriptors = repository->list_mesh_nodes ("startup-review-0").result ().value ();
    ASSERT_EQ (descriptors.items.size (), 1u);
    EXPECT_EQ (descriptors.items.front ().owner_id, winner_owner->owner_id);
    EXPECT_EQ (descriptors.items.front ().lease_generation, winner_owner->lease_generation);
    EXPECT_EQ (apps[winner]->runtime_state (), framework_runtime_state_t::serving);
    apps[winner]->request_stop ();
    running[winner].join ();
    EXPECT_EQ (exits[winner], 0);
}

TEST (CppFrameworkOpaqueLocationStore, StartupPublicationFailureWaitsForShutdownResourceHandoff)
{
    verify_startup_failure_joins_shutdown (false);
}

TEST (CppFrameworkOpaqueLocationStore, StartupPublicationFailureSurvivesShutdownBeforeRunCheck)
{
    verify_startup_failure_joins_shutdown (true);
}

TEST (CppFrameworkOpaqueLocationStore, LeaseLessStartupObservesAllFixedRidsOnFirstRecovery)
{
    auto store = std::make_shared<startup_review_store_t> ();
    store->unavailable = true;
    auto repository = std::make_shared<provider_location_repository_t> (*store);
    auto app = app_t::create ();
    configure_review_host (app, store, repository, 2);
    auto probe = std::make_unique<fixed_rid_startup_probe_t> ();
    auto *view = probe.get ();
    location_reclaim_test_access_t clock;
    clock.bind (*repository, [view] { view->clock_observed (); });
    app.add_hosted_service (std::move (probe));
    int exit_code = -1;
    std::jthread running ([&] {
        char program[] = "lease-less-startup";
        char *arguments[] = {program, nullptr};
        exit_code = app.run (1, arguments);
    });
    struct stop_on_exit_t
    {
        app_t &app;
        ~stop_on_exit_t () { app.request_stop (); }
    } stop{app};
    ASSERT_TRUE (view->wait_started ());
    EXPECT_EQ (app.runtime_state (), framework_runtime_state_t::preparing);
    EXPECT_FALSE (view->owner_lease_healthy ());
    EXPECT_EQ (location_reclaim_test_access_t::observations (*repository), 0u);
    store->unavailable = false;
    ASSERT_TRUE (view->wait_reads (2));
    EXPECT_EQ (location_reclaim_test_access_t::observations (*repository), 2u);
    EXPECT_EQ (app.runtime_state (), framework_runtime_state_t::preparing);
    clock.advance (20ms);
    const auto reads = view->read_count ();
    ASSERT_TRUE (view->wait_reads (reads + 2));
    EXPECT_EQ (app.runtime_state (), framework_runtime_state_t::preparing);
    clock.advance (1ms);
    ASSERT_TRUE (view->wait_ready ());
    for (unsigned index = 0; index < 2; ++index)
        EXPECT_EQ (repository->list_mesh_nodes ("startup-review-" + std::to_string (index))
                     .result ()
                     .value ()
                     .items.size (),
                   1u);
    app.request_stop ();
    running.join ();
    EXPECT_EQ (exit_code, 0);
}

TEST (CppFrameworkOpaqueLocationStore, FixedRidStartupStaysPreparingWithoutClaimUntilReleaseWait)
{
    for (const bool use_provider : {false, true}) {
        SCOPED_TRACE (use_provider ? "provider" : "in-memory");
        auto store = std::make_shared<reclaim_count_store_t> ();
        std::shared_ptr<location_repository_t> repository =
          use_provider
            ? std::static_pointer_cast<location_repository_t> (
                std::make_shared<provider_location_repository_t> (*store))
            : std::static_pointer_cast<location_repository_t> (
                std::make_shared<reclaim_count_memory_repository_t> (store->mesh_claims));
        repository->configure_reclaim_margin (0ms);
        const auto owner =
          std::get<owner_lease_claimed_t> (
            repository->claim_owner_lease ("old-fixed-owner", 60s).result ().value ())
            .token;
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "fixed-restart";
        descriptor.rid = zlink::routing_id_t::from ("fixed-restart-node");
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "inproc://fixed-restart-node";
        descriptor.security_identity = "fixed-restart-node";
        descriptor.activation_concurrency.limit = 1;
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.object_role = object_role_t::none;
        descriptor.state = framework_runtime_state_t::serving;
        ASSERT_EQ (repository->update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
        repository->release_owner_lease (owner).result ().value ();
        const auto claims = store->mesh_claims.load ();
        const auto stored_owner = [&] {
            const auto page = repository->list_mesh_nodes (descriptor.mesh_name).result ().value ();
            EXPECT_EQ (page.items.size (), 1u);
            return page.items.empty () ? std::string{} : page.items.front ().owner_id;
        };
        auto app = app_t::create ();
        auto &options = app.add_zlink_framework ();
        options.add_location_store (store);
        options.add_relocation_store (std::make_shared<in_memory_relocation_store_t> ());
        auto &locations = options.configure_locations ();
        locations.owner_lease_renew_interval = 10ms;
        locations.owner_lease_fencing_margin = 7ms;
        auto mesh = options.add_route_mesh ("fixed-restart");
        mesh.channel_name ("fixed-restart-channel").server ();
        mesh.set_object_role (object_role_t::none)
          .set_routing_id (descriptor.rid)
          .listen (descriptor.endpoint);
        app.advanced ().services ().add_factory<location_repository_t> (
          [repository] (service_provider_t &) { return repository; },
          service_lifetime_t::singleton);
        auto probe = std::make_unique<fixed_rid_startup_probe_t> ();
        auto *view = probe.get ();
        location_reclaim_test_access_t clock;
        clock.bind (*repository, [view] { view->clock_observed (); });
        app.add_hosted_service (std::move (probe));
        int exit_code = -1;
        std::jthread running ([&] {
            char program[] = "fixed-restart";
            char *arguments[] = {program, nullptr};
            exit_code = app.run (1, arguments);
        });
        struct stop_on_exit_t
        {
            app_t &app;
            ~stop_on_exit_t () { app.request_stop (); }
        } stop_on_exit{app};
        ASSERT_TRUE (view->wait_started ());
        ASSERT_TRUE (view->wait_reads (1));
        EXPECT_FALSE (app.is_ready ());
        EXPECT_EQ (app.runtime_state (), framework_runtime_state_t::preparing);
        EXPECT_EQ (store->mesh_claims.load (), claims);
        EXPECT_EQ (stored_owner (), owner.owner_id);
        clock.advance (20ms);
        // Two observations fence a renewal already in flight before the clock advance.
        const auto observed = view->read_count ();
        ASSERT_TRUE (view->wait_reads (observed + 2));
        EXPECT_TRUE (view->owner_lease_healthy ());
        EXPECT_FALSE (app.is_ready ());
        EXPECT_EQ (app.runtime_state (), framework_runtime_state_t::preparing);
        EXPECT_EQ (store->mesh_claims.load (), claims);
        EXPECT_EQ (stored_owner (), owner.owner_id);
        clock.advance (1ms);
        ASSERT_TRUE (view->wait_ready ());
        EXPECT_TRUE (app.is_ready ());
        EXPECT_EQ (app.runtime_state (), framework_runtime_state_t::serving);
        EXPECT_NE (stored_owner (), owner.owner_id);
        EXPECT_EQ (store->mesh_claims.load (), claims + 1);
        app.request_stop ();
        running.join ();
        EXPECT_EQ (exit_code, 0);
    }
}

TEST (CppFrameworkOpaqueLocationStore, FixedRidNormalRestartWaitsWithMissingDescriptor)
{
    for (const bool use_provider : {false, true}) {
        SCOPED_TRACE (use_provider ? "provider" : "in-memory");
        auto store = std::make_shared<reclaim_count_store_t> ();
        std::shared_ptr<location_repository_t> repository =
          use_provider
            ? std::static_pointer_cast<location_repository_t> (
                std::make_shared<provider_location_repository_t> (*store))
            : std::static_pointer_cast<location_repository_t> (
                std::make_shared<reclaim_count_memory_repository_t> (store->mesh_claims));
        location_reclaim_test_access_t clock;
        for (const bool restarting : {false, true}) {
            auto app = app_t::create ();
            auto &options = app.add_zlink_framework ();
            options.add_location_store (store);
            options.add_relocation_store (std::make_shared<in_memory_relocation_store_t> ());
            auto &locations = options.configure_locations ();
            locations.owner_lease_renew_interval = 10ms;
            locations.owner_lease_fencing_margin = restarting ? 7ms : 1ms;
            auto mesh = options.add_route_mesh ("normal-restart");
            mesh.channel_name ("normal-restart-channel").server ();
            mesh.set_object_role (object_role_t::none)
              .set_routing_id (zlink::routing_id_t::from ("normal-restart-node"))
              .listen ("inproc://normal-restart-node");
            app.advanced ().services ().add_factory<location_repository_t> (
              [repository] (service_provider_t &) { return repository; },
              service_lifetime_t::singleton);
            auto probe = std::make_unique<fixed_rid_startup_probe_t> ();
            auto *view = probe.get ();
            if (restarting)
                clock.bind (*repository, [view] { view->clock_observed (); });
            app.add_hosted_service (std::move (probe));
            const auto claims = store->mesh_claims.load ();
            int exit_code = -1;
            std::jthread running ([&] {
                char program[] = "normal-restart";
                char *arguments[] = {program, nullptr};
                exit_code = app.run (1, arguments);
            });
            struct stop_on_exit_t
            {
                app_t &app;
                ~stop_on_exit_t () { app.request_stop (); }
            } stop{app};
            ASSERT_TRUE (view->wait_started ());
            if (restarting) {
                // Serving on the old implementation exposes the bypass without waiting
                // for a clock callback that the buggy Missing path never makes.
                const auto observed_or_ready = [&] { return view->wait_observed_or_ready (); };
                ASSERT_TRUE (observed_or_ready ());
                EXPECT_EQ (app.runtime_state (), framework_runtime_state_t::preparing);
                EXPECT_EQ (store->mesh_claims.load (), claims);
                EXPECT_TRUE (
                  repository->list_mesh_nodes ("normal-restart").result ().value ().items.empty ());
                EXPECT_TRUE (view->owner_lease_healthy ());
                clock.advance (20ms);
                if (!app.is_ready ()) {
                    const auto reads = view->read_count ();
                    ASSERT_TRUE (view->wait_reads (reads + 2));
                }
                EXPECT_EQ (app.runtime_state (), framework_runtime_state_t::preparing);
                EXPECT_EQ (store->mesh_claims.load (), claims);
                EXPECT_TRUE (
                  repository->list_mesh_nodes ("normal-restart").result ().value ().items.empty ());
                EXPECT_TRUE (view->owner_lease_healthy ());
                clock.advance (1ms);
            }
            ASSERT_TRUE (view->wait_ready ());
            EXPECT_EQ (app.runtime_state (), framework_runtime_state_t::serving);
            EXPECT_EQ (
              repository->list_mesh_nodes ("normal-restart").result ().value ().items.size (), 1u);
            EXPECT_EQ (store->mesh_claims.load (), claims + 1);
            app.request_stop ();
            running.join ();
            EXPECT_EQ (exit_code, 0);
            EXPECT_TRUE (
              repository->list_mesh_nodes ("normal-restart").result ().value ().items.empty ());
        }
    }
}

TEST (CppFrameworkOpaqueLocationStore, AutomaticRidClaimDoesNotWaitForMissingOrExpiredDescriptor)
{
    for (const bool use_provider : {false, true}) {
        SCOPED_TRACE (use_provider ? "provider" : "in-memory");
        in_memory_location_store_t store;
        std::unique_ptr<location_repository_t> repository =
          use_provider ? std::unique_ptr<location_repository_t> (
                           std::make_unique<provider_location_repository_t> (store))
                       : std::unique_ptr<location_repository_t> (
                           std::make_unique<in_memory_location_repository_t> ());
        const auto owner =
          std::get<owner_lease_claimed_t> (
            repository->claim_owner_lease ("automatic-owner", 60s).result ().value ())
            .token;
        auto descriptor = review_mesh_descriptor (owner);
        EXPECT_EQ (repository->update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
        repository->release_owner_lease (owner).result ().value ();
        const auto successor =
          std::get<owner_lease_claimed_t> (
            repository->claim_owner_lease ("automatic-successor", 60s).result ().value ())
            .token;
        descriptor.owner_id = successor.owner_id;
        descriptor.lease_generation = successor.lease_generation;
        ++descriptor.lifecycle_generation;
        EXPECT_EQ (repository->update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
        EXPECT_EQ (location_reclaim_test_access_t::observations (*repository), 0u);
    }
}

TEST (CppFrameworkOpaqueLocationStore, AutomaticRidHostServesWithoutReclaimWait)
{
    for (const bool use_provider : {false, true}) {
        SCOPED_TRACE (use_provider ? "provider" : "in-memory");
        auto store = std::make_shared<reclaim_count_store_t> ();
        std::shared_ptr<location_repository_t> repository =
          use_provider
            ? std::static_pointer_cast<location_repository_t> (
                std::make_shared<provider_location_repository_t> (*store))
            : std::static_pointer_cast<location_repository_t> (
                std::make_shared<reclaim_count_memory_repository_t> (store->mesh_claims));
        auto app = app_t::create ();
        auto &options = app.add_zlink_framework ();
        options.add_location_store (store);
        options.add_relocation_store (std::make_shared<in_memory_relocation_store_t> ());
        auto mesh = options.add_route_mesh ("automatic-startup");
        mesh.channel_name ("automatic-channel").server ();
        mesh.set_object_role (object_role_t::none).listen ("inproc://automatic-startup");
        mesh.set_automatic_routing_id_prefix ("automatic");
        app.advanced ().services ().add_factory<location_repository_t> (
          [repository] (service_provider_t &) { return repository; },
          service_lifetime_t::singleton);
        auto probe = std::make_unique<fixed_rid_startup_probe_t> ();
        auto *view = probe.get ();
        app.add_hosted_service (std::move (probe));
        int exit_code = -1;
        std::jthread running ([&] { exit_code = app.run (0, nullptr); });
        struct stop_on_exit_t
        {
            app_t &app;
            ~stop_on_exit_t () { app.request_stop (); }
        } stop{app};
        ASSERT_TRUE (view->wait_started ());
        ASSERT_TRUE (view->wait_ready ());
        EXPECT_EQ (app.runtime_state (), framework_runtime_state_t::serving);
        EXPECT_EQ (store->mesh_claims.load (), 1u);
        EXPECT_EQ (location_reclaim_test_access_t::observations (*repository), 0u);
        app.request_stop ();
        running.join ();
        EXPECT_EQ (exit_code, 0);
    }
}

TEST (CppFrameworkOpaqueLocationStore, PreparingHostStopsBeforeFixedRidClaim)
{
    for (const bool use_provider : {false, true}) {
        SCOPED_TRACE (use_provider ? "provider" : "in-memory");
        auto store = std::make_shared<reclaim_count_store_t> ();
        std::shared_ptr<location_repository_t> repository =
          use_provider
            ? std::static_pointer_cast<location_repository_t> (
                std::make_shared<provider_location_repository_t> (*store))
            : std::static_pointer_cast<location_repository_t> (
                std::make_shared<reclaim_count_memory_repository_t> (store->mesh_claims));
        auto app = app_t::create ();
        auto &options = app.add_zlink_framework ();
        options.add_location_store (store);
        options.add_relocation_store (std::make_shared<in_memory_relocation_store_t> ());
        auto mesh = options.add_route_mesh ("preparing-stop");
        mesh.channel_name ("preparing-stop-channel").server ();
        mesh.set_object_role (object_role_t::none)
          .set_routing_id (zlink::routing_id_t::from ("preparing-stop-node"))
          .listen ("inproc://preparing-stop");
        app.advanced ().services ().add_factory<location_repository_t> (
          [repository] (service_provider_t &) { return repository; },
          service_lifetime_t::singleton);
        auto probe = std::make_unique<fixed_rid_startup_probe_t> ();
        auto *view = probe.get ();
        location_reclaim_test_access_t clock;
        clock.bind (*repository, [view] { view->clock_observed (); });
        app.add_hosted_service (std::move (probe));
        int exit_code = -1;
        std::jthread running ([&] { exit_code = app.run (0, nullptr); });
        struct stop_on_exit_t
        {
            app_t &app;
            ~stop_on_exit_t () { app.request_stop (); }
        } stop{app};
        ASSERT_TRUE (view->wait_started ());
        ASSERT_TRUE (view->wait_reads (1));
        EXPECT_EQ (app.runtime_state (), framework_runtime_state_t::preparing);
        EXPECT_EQ (store->mesh_claims.load (), 0u);
        EXPECT_TRUE (
          repository->list_mesh_nodes ("preparing-stop").result ().value ().items.empty ());
        app.request_stop ();
        running.join ();
        EXPECT_EQ (exit_code, 0);
    }
}
}
