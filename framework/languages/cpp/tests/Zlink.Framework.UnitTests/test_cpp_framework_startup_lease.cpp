/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework.hpp>
#include "../support/manual_deadlines.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/fanout/fanout_location_runtime.hpp"
#include "runtime/locations/location_runtime.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <stdexcept>
#include <utility>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
using namespace zlink::framework;

struct startup_message_t
{
};
struct startup_handler_t
{
    using message_type = startup_message_t;
    void handle (const startup_message_t &) {}
};

// The last hosted service supplies the startup barrier. The tests do not
// depend on a metric being registered as a side effect of startup.
class startup_meter_t
{
  public:
    std::promise<void> started;
    void notify_started_or_exited ()
    {
        std::call_once (_started, [&] { started.set_value (); });
    }

  private:
    std::once_flag _started;
};
class startup_metrics_t
{
  public:
    std::shared_ptr<startup_meter_t> meter = std::make_shared<startup_meter_t> ();
};
class startup_barrier_t final : public hosted_service_t
{
  public:
    explicit startup_barrier_t (startup_meter_t &meter) : _meter (meter) {}
    task_t<void> start (service_provider_t &) override
    {
        _meter.notify_started_or_exited ();
        co_return;
    }
    void stop () noexcept override {}

  private:
    startup_meter_t &_meter;
};

class held_lease_repository_t final : public runtime::in_memory_location_repository_t
{
  public:
    enum class mode_t
    {
        held,
        unavailable,
        conflict,
        exhausted
    };
    explicit held_lease_repository_t (mode_t mode) : _mode (mode) {}
    task_completion_source_t<void> release_claim;
    task_completion_source_t<void> release_renew;
    bool hold_renewals = false;
    task_completion_source_t<void> release_descriptor;
    task_completion_source_t<void> release_fanout;
    std::promise<void> fanout_requested;
    std::promise<void> fanout_stored;
    std::promise<void> fanout_rejected;
    std::atomic_bool reject_fanout{false};
    std::atomic_int fanout_claims{0};
    std::atomic_int fail_mesh_writes{0};
    std::atomic_int fail_client_writes{0};
    std::atomic_int fail_fanout_writes{0};
    std::atomic_int client_attempts{0};
    task_completion_source_t<void> release_client;
    std::promise<void> client_requested;
    std::atomic_int mesh_attempts{0};
    location_write_status_t mesh_status = location_write_status_t::stored;
    location_write_status_t client_status = location_write_status_t::stored;
    location_write_status_t fanout_status = location_write_status_t::stored;
    bool cancel_mesh_write = false;
    std::promise<void> descriptor_requested;
    std::promise<void> descriptor_stored;

    task_t<owner_lease_claim_result_t> claim_owner_lease (std::string owner,
                                                          std::chrono::milliseconds ttl) override
    {
        if (_mode == mode_t::conflict)
            co_return owner_lease_claim_result_t{owner_lease_conflict_t{}};
        if (_mode == mode_t::exhausted)
            co_return owner_lease_claim_result_t{owner_lease_generation_exhausted_t{}};
        if (_mode == mode_t::unavailable)
            throw std::runtime_error ("injected Store failure");
        co_await release_claim.task ();
        co_return co_await in_memory_location_repository_t::claim_owner_lease (std::move (owner),
                                                                               ttl);
    }
    task_t<owner_lease_renew_result_t> renew_owner_lease (location_owner_token_t token,
                                                          std::chrono::milliseconds ttl) override
    {
        if (hold_renewals)
            co_await release_renew.task ();
        co_return co_await in_memory_location_repository_t::renew_owner_lease (std::move (token),
                                                                               ttl);
    }
    task_t<owner_lease_read_result_t> read_owner_lease (std::string owner_id) override
    {
        // A timed-out background renewal must not confirm itself before the test
        // releases the renewal input. Discovery can report this error without blocking startup.
        if (hold_renewals && !release_renew.task ().await_ready ())
            co_return result_t<owner_lease_read_result_t>::failure (
              framework_error_kind_t::unavailable, "renewal confirmation held by test");
        co_return co_await in_memory_location_repository_t::read_owner_lease (std::move (owner_id));
    }
    task_t<location_write_result_t> update_mesh_node (mesh_node_descriptor_t descriptor,
                                                      location_write_intent_t intent) override
    {
        ++mesh_attempts;
        if (cancel_mesh_write)
            std::rethrow_exception (
              detail::make_cancellation_exception ("injected startup cancellation"));
        if (fail_mesh_writes.load () > 0) {
            --fail_mesh_writes;
            co_return result_t<location_write_result_t>::failure (
              framework_error_kind_t::internal_failure, "injected unknown descriptor write");
        }
        if (mesh_status != location_write_status_t::stored)
            co_return location_write_result_t{mesh_status, 0, {}};
        const auto first = intent == location_write_intent_t::new_claim;
        if (first) {
            std::call_once (_requested, [&] { descriptor_requested.set_value (); });
            co_await release_descriptor.task ();
        }
        auto result = co_await in_memory_location_repository_t::update_mesh_node (
          std::move (descriptor), intent);
        if (first)
            std::call_once (_stored, [&] { descriptor_stored.set_value (); });
        co_return result;
    }
    task_t<location_write_result_t>
    update_fanout_publisher (fanout_publisher_descriptor_t descriptor,
                             location_write_intent_t intent) override
    {
        if (intent == location_write_intent_t::new_claim) {
            ++fanout_claims;
            if (fanout_status != location_write_status_t::stored)
                co_return location_write_result_t{fanout_status, 0, {}};
            if (fail_fanout_writes.load () > 0) {
                --fail_fanout_writes;
                co_return result_t<location_write_result_t>::failure (
                  framework_error_kind_t::internal_failure, "injected unknown Fanout write");
            }
            std::call_once (_fanout_requested, [&] { fanout_requested.set_value (); });
            co_await release_fanout.task ();
        }
        if (reject_fanout.load ()) {
            std::call_once (_fanout_rejected, [&] { fanout_rejected.set_value (); });
            co_return location_write_result_t{location_write_status_t::ignored_stale, 0, {}};
        }
        auto result = co_await in_memory_location_repository_t::update_fanout_publisher (
          std::move (descriptor), intent);
        if (result.status == location_write_status_t::stored)
            std::call_once (_fanout_stored, [&] { fanout_stored.set_value (); });
        co_return result;
    }
    task_t<location_write_result_t>
    update_client_server (client_server_server_descriptor_t descriptor,
                          location_write_intent_t intent) override
    {
        ++client_attempts;
        if (client_status != location_write_status_t::stored)
            co_return location_write_result_t{client_status, 0, {}};
        if (fail_client_writes.load () > 0) {
            --fail_client_writes;
            co_return result_t<location_write_result_t>::failure (
              framework_error_kind_t::internal_failure, "injected unknown ClientServer write");
        }
        if (intent == location_write_intent_t::new_claim) {
            std::call_once (_client_requested, [&] { client_requested.set_value (); });
            co_await release_client.task ();
        }
        co_return co_await in_memory_location_repository_t::update_client_server (
          std::move (descriptor), intent);
    }
    void release ()
    {
        reject_fanout.store (false);
        release_claim.complete (result_t<void>::success ());
        release_renew.complete (result_t<void>::success ());
        release_descriptor.complete (result_t<void>::success ());
        release_fanout.complete (result_t<void>::success ());
        release_client.complete (result_t<void>::success ());
    }

  private:
    mode_t _mode;
    std::once_flag _requested;
    std::once_flag _stored;
    std::once_flag _fanout_requested;
    std::once_flag _fanout_stored;
    std::once_flag _fanout_rejected;
    std::once_flag _client_requested;
};

class runtime_probe_t final : public hosted_service_t
{
  public:
    std::promise<framework_runtime_t *> framework_runtime;
    std::promise<runtime::fanout::fanout_location_runtime_t *> fanout;
    std::promise<runtime::location_runtime_t *> location;
    task_t<void> start (service_provider_t &services) override
    {
        framework_runtime.set_value (&services.get_required<framework_runtime_t> ());
        location.set_value (&services.get_required<runtime::location_runtime_t> ());
        auto value = services.get<fanout_runtime_t> ();
        fanout.set_value (
          value ? dynamic_cast<runtime::fanout::fanout_location_runtime_t *> (&value->get ())
                : nullptr);
        co_return;
    }
    void stop () noexcept override {}
};

void configure (app_t &app, const std::shared_ptr<held_lease_repository_t> &store, bool mesh = true)
{
    auto &options = app.add_zlink_framework ();
    // Use the repository port so claim rejection and pending responses are controlled
    // directly; the production Location runtime and descriptor publishers run unchanged.
    app.advanced ().services ().add_factory<location_repository_t> (
      [store] (service_provider_t &) {
          return std::static_pointer_cast<location_repository_t> (store);
      },
      service_lifetime_t::singleton);
    if (mesh)
        options.add_route_mesh ("startup-lease")
          .listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from ("startup-lease"));
}

class running_host_t
{
  public:
    running_host_t (app_t &app,
                    std::shared_ptr<held_lease_repository_t> store,
                    startup_meter_t &meter) :
        _app (app), _store (std::move (store))
    {
        _app.add_hosted_service (std::make_unique<startup_barrier_t> (meter));
        _host = std::thread ([this, &meter] {
            char program[] = "startup-lease";
            char *argv[] = {program};
            exit_code.store (_app.run (1, argv));
            meter.notify_started_or_exited ();
        });
    }
    ~running_host_t ()
    {
        _store->release ();
        _app.stop ();
        _host.join ();
        EXPECT_EQ (0, exit_code.load ());
    }
    std::atomic_int exit_code{-1};

  private:
    app_t &_app;
    std::shared_ptr<held_lease_repository_t> _store;
    std::thread _host;
};

TEST (ZLinkFrameworkStartupLease, PreparingUntilServingDescriptorIsStored)
{
    startup_metrics_t metrics;
    auto started = metrics.meter->started.get_future ();
    auto store = std::make_shared<held_lease_repository_t> (held_lease_repository_t::mode_t::held);
    auto requested = store->descriptor_requested.get_future ();
    auto stored = store->descriptor_stored.get_future ();
    auto fanout_requested = store->fanout_requested.get_future ();
    auto fanout_stored = store->fanout_stored.get_future ();
    auto fanout_rejected = store->fanout_rejected.get_future ();
    store->reject_fanout.store (true);
    auto app = app_t::create ();
    configure (app, store);
    app.add_zlink_framework ()
      .add_fanout_channel ("startup-fanout")
      .enable_publisher (0)
      .set_routing_id (zlink::routing_id_t::from ("startup-fanout"));
    auto probe = std::make_unique<runtime_probe_t> ();
    auto runtime_future = probe->framework_runtime.get_future ();
    auto fanout_future = probe->fanout.get_future ();
    app.add_hosted_service (std::move (probe));
    running_host_t host (app, store, *metrics.meter);
    started.wait ();
    ASSERT_EQ (-1, host.exit_code.load ());
    auto *runtime = runtime_future.get ();
    EXPECT_EQ (framework_runtime_state_t::preparing, app.runtime_state ());
    EXPECT_FALSE (app.is_ready ());
    EXPECT_TRUE (store->list_mesh_nodes ("startup-lease").result ().value ().items.empty ());
    const auto blocked =
      app.relocate ({.mode = relocation_mode_t::planned_maintenance}).result ().value ();
    EXPECT_EQ (relocation_outcome_t::blocked, blocked.outcome);
    EXPECT_EQ (relocation_reason_t::runtime_not_ready, blocked.reason);

    std::promise<void> serving;
    auto ready = serving.get_future ();
    std::once_flag observed;
    auto observation = runtime->observe (16, [&] (const auto &status) {
        if (status.status.state == framework_runtime_state_t::serving)
            std::call_once (observed, [&] { serving.set_value (); });
    });
    store->release_claim.complete (result_t<void>::success ());
    requested.wait ();
    EXPECT_EQ (framework_runtime_state_t::preparing, app.runtime_state ());
    EXPECT_FALSE (app.is_ready ());
    store->release_descriptor.complete (result_t<void>::success ());
    stored.wait ();
    fanout_requested.wait ();
    EXPECT_EQ (framework_runtime_state_t::preparing, app.runtime_state ());
    EXPECT_FALSE (app.is_ready ());
    store->release_fanout.complete (result_t<void>::success ());
    fanout_rejected.wait ();
    // Rejected publication cannot count as completion merely because a lease exists.
    auto *fanout_runtime = fanout_future.get ();
    ASSERT_NE (nullptr, fanout_runtime);
    EXPECT_FALSE (fanout_runtime->republish_after_store_recovery ());
    EXPECT_EQ (framework_runtime_state_t::preparing, app.runtime_state ());
    EXPECT_FALSE (app.is_ready ());
    store->reject_fanout.store (false);
    fanout_stored.wait ();
    ready.wait ();
    EXPECT_TRUE (app.is_ready ());
    const auto published_claims = store->fanout_claims.load ();
    EXPECT_TRUE (fanout_runtime->republish_after_store_recovery ());
    EXPECT_EQ (published_claims, store->fanout_claims.load ());
    const auto descriptors = store->list_mesh_nodes ("startup-lease").result ().value ();
    EXPECT_EQ (1u, descriptors.items.size ());
    if (!descriptors.items.empty ()) {
        EXPECT_EQ (framework_runtime_state_t::serving, descriptors.items.front ().state);
        EXPECT_GT (descriptors.items.front ().lease_generation, 0);
    }
}

TEST (ZLinkFrameworkStartupLease, HostWithoutMeshAlsoWaitsForLeaseRecovery)
{
    startup_metrics_t metrics;
    auto started = metrics.meter->started.get_future ();
    auto store = std::make_shared<held_lease_repository_t> (held_lease_repository_t::mode_t::held);
    auto app = app_t::create ();
    configure (app, store, false);
    auto probe = std::make_unique<runtime_probe_t> ();
    auto runtime_future = probe->framework_runtime.get_future ();
    app.add_hosted_service (std::move (probe));
    running_host_t host (app, store, *metrics.meter);
    started.wait ();
    ASSERT_EQ (-1, host.exit_code.load ());
    EXPECT_EQ (framework_runtime_state_t::preparing, app.runtime_state ());
    EXPECT_FALSE (app.is_ready ());
    std::promise<void> serving;
    auto ready = serving.get_future ();
    std::once_flag observed;
    auto observation = runtime_future.get ()->observe (16, [&] (const auto &status) {
        if (status.status.state == framework_runtime_state_t::serving)
            std::call_once (observed, [&] { serving.set_value (); });
    });
    store->release_claim.complete (result_t<void>::success ());
    ready.wait ();
    EXPECT_TRUE (app.is_ready ());
}

TEST (ZLinkFrameworkStartupLease, ProviderErrorStartsPreparingAndStopsWithoutLease)
{
    startup_metrics_t metrics;
    auto started = metrics.meter->started.get_future ();
    auto store =
      std::make_shared<held_lease_repository_t> (held_lease_repository_t::mode_t::unavailable);
    auto app = app_t::create ();
    configure (app, store);
    running_host_t host (app, store, *metrics.meter);
    started.wait ();
    EXPECT_EQ (-1, host.exit_code.load ());
    EXPECT_EQ (framework_runtime_state_t::preparing, app.runtime_state ());
    EXPECT_FALSE (app.is_ready ());
    EXPECT_TRUE (store->list_mesh_nodes ("startup-lease").result ().value ().items.empty ());
}

TEST (ZLinkFrameworkStartupLease, DefinitiveClaimRejectionFailsStartup)
{
    for (auto mode :
         {held_lease_repository_t::mode_t::conflict, held_lease_repository_t::mode_t::exhausted}) {
        auto store = std::make_shared<held_lease_repository_t> (mode);
        auto app = app_t::create ();
        configure (app, store);
        char program[] = "startup-lease-rejected";
        char *argv[] = {program};
        EXPECT_NE (0, app.run (1, argv));
        EXPECT_EQ (framework_runtime_state_t::error, app.runtime_state ());
        EXPECT_FALSE (app.is_ready ());
        EXPECT_TRUE (store->list_mesh_nodes ("startup-lease").result ().value ().items.empty ());
    }
}
} // namespace

TEST (ZLinkFrameworkStartupLease, UnknownDescriptorPublicationRetriesAfterRenewal)
{
    for (const auto kind : {"MeshNode", "ClientServer", "Fanout"}) {
        SCOPED_TRACE (kind);
        startup_metrics_t metrics;
        auto started = metrics.meter->started.get_future ();
        auto store =
          std::make_shared<held_lease_repository_t> (held_lease_repository_t::mode_t::held);
        store->release_claim.complete (result_t<void>::success ());
        store->hold_renewals = true;
        auto app = app_t::create ();
        configure (app, store);
        std::future<void> requested;
        task_completion_source_t<void> *release = nullptr;
        std::atomic_int *attempts = nullptr;
        if (std::string_view (kind) == "MeshNode") {
            store->fail_mesh_writes.store (1);
            requested = store->descriptor_requested.get_future ();
            release = &store->release_descriptor;
            attempts = &store->mesh_attempts;
        } else {
            store->release_descriptor.complete (result_t<void>::success ());
            if (std::string_view (kind) == "ClientServer") {
                app.add_zlink_framework ()
                  .add_client_server_channel ("startup-client")
                  .server ()
                  .listen (0)
                  .add_send_handler<startup_handler_t, startup_message_t> ();
                store->fail_client_writes.store (1);
                requested = store->client_requested.get_future ();
                release = &store->release_client;
                attempts = &store->client_attempts;
            } else {
                app.add_zlink_framework ()
                  .add_fanout_channel ("startup-fanout")
                  .enable_publisher (0)
                  .set_routing_id (zlink::routing_id_t::from ("startup-fanout"));
                store->fail_fanout_writes.store (1);
                requested = store->fanout_requested.get_future ();
                release = &store->release_fanout;
                attempts = &store->fanout_claims;
            }
        }
        auto probe = std::make_unique<runtime_probe_t> ();
        auto location = probe->location.get_future ();
        auto runtime = probe->framework_runtime.get_future ();
        app.add_hosted_service (std::move (probe));
        running_host_t host (app, store, *metrics.meter);
        started.wait ();
        EXPECT_EQ (-1, host.exit_code.load ());
        if (host.exit_code.load () != -1)
            continue;
        EXPECT_EQ (framework_runtime_state_t::preparing, app.runtime_state ());
        EXPECT_FALSE (app.is_ready ());
        EXPECT_EQ (1, attempts->load ());
        std::promise<void> serving;
        auto ready = serving.get_future ();
        std::once_flag observed;
        auto observation = runtime.get ()->observe (16, [&] (const auto &status) {
            if (status.status.state == framework_runtime_state_t::serving)
                std::call_once (observed, [&] { serving.set_value (); });
        });
        store->release_renew.complete (result_t<void>::success ());
        location.get ()->renew_owner_lease_once ();
        requested.wait ();
        EXPECT_EQ (framework_runtime_state_t::preparing, app.runtime_state ());
        EXPECT_FALSE (app.is_ready ());
        release->complete (result_t<void>::success ());
        ready.wait ();
        EXPECT_TRUE (app.is_ready ());
    }
}

TEST (ZLinkFrameworkStartupLease, ConfirmedDescriptorConflictFailsStartup)
{
    for (const auto kind : {"MeshNode", "ClientServer", "Fanout"}) {
        SCOPED_TRACE (kind);
        startup_metrics_t metrics;
        auto store =
          std::make_shared<held_lease_repository_t> (held_lease_repository_t::mode_t::held);
        store->release ();
        auto app = app_t::create ();
        configure (app, store);
        if (std::string_view (kind) == "MeshNode") {
            store->mesh_status = location_write_status_t::rejected_conflict;
        } else if (std::string_view (kind) == "ClientServer") {
            app.add_zlink_framework ()
              .add_client_server_channel ("startup-client")
              .server ()
              .listen (0)
              .add_send_handler<startup_handler_t, startup_message_t> ();
            store->client_status = location_write_status_t::rejected_conflict;
        } else {
            app.add_zlink_framework ()
              .add_fanout_channel ("startup-fanout")
              .enable_publisher (0)
              .set_routing_id (zlink::routing_id_t::from ("startup-fanout"));
            store->fanout_status = location_write_status_t::rejected_conflict;
        }
        char program[] = "startup-conflict";
        char *argv[] = {program};
        EXPECT_NE (0, app.run (1, argv));
        if (std::string_view (kind) == "MeshNode")
            EXPECT_EQ (1, store->mesh_attempts.load ());
        else if (std::string_view (kind) == "ClientServer")
            EXPECT_EQ (1, store->client_attempts.load ());
        else
            EXPECT_EQ (1, store->fanout_claims.load ());
        EXPECT_EQ (framework_runtime_state_t::error, app.runtime_state ());
        EXPECT_FALSE (app.is_ready ());
    }
}

TEST (ZLinkFrameworkStartupLease, AcquiredLeaseStorePendingKeepsPreparing)
{
    startup_metrics_t metrics;
    auto started = metrics.meter->started.get_future ();
    auto store = std::make_shared<held_lease_repository_t> (held_lease_repository_t::mode_t::held);
    store->release_claim.complete (result_t<void>::success ());
    auto requested = store->descriptor_requested.get_future ();
    auto app = app_t::create ();
    configure (app, store);
    running_host_t host (app, store, *metrics.meter);
    requested.wait ();
    EXPECT_EQ (framework_runtime_state_t::preparing, app.runtime_state ());
    EXPECT_FALSE (app.is_ready ());
    store->release_descriptor.complete (result_t<void>::success ());
    started.wait ();
}

TEST (ZLinkFrameworkStartupLease, DescriptorCancellationFailsStartup)
{
    startup_metrics_t metrics;
    auto store = std::make_shared<held_lease_repository_t> (held_lease_repository_t::mode_t::held);
    store->release ();
    store->cancel_mesh_write = true;
    auto app = app_t::create ();
    configure (app, store);
    char program[] = "startup-cancelled";
    char *argv[] = {program};
    EXPECT_NE (0, app.run (1, argv));
    EXPECT_EQ (framework_runtime_state_t::error, app.runtime_state ());
    EXPECT_FALSE (app.is_ready ());
}


TEST (ZLinkFrameworkStartupLease, UnansweredPublicationStartsPreparingAndRecoversAfterRenewal)
{
    for (const auto kind : {"MeshNode", "ClientServer", "Fanout"}) {
        SCOPED_TRACE (kind);
        startup_metrics_t metrics;
        auto started = metrics.meter->started.get_future ();
        auto store =
          std::make_shared<held_lease_repository_t> (held_lease_repository_t::mode_t::held);
        store->release_claim.complete (result_t<void>::success ());
        store->hold_renewals = true;
        auto app = app_t::create ();
        configure (app, store);
        std::future<void> requested;
        task_completion_source_t<void> *release = nullptr;
        std::atomic_int *attempts = nullptr;
        if (std::string_view (kind) == "MeshNode") {
            requested = store->descriptor_requested.get_future ();
            release = &store->release_descriptor;
            attempts = &store->mesh_attempts;
        } else {
            store->release_descriptor.complete (result_t<void>::success ());
            if (std::string_view (kind) == "ClientServer") {
                app.add_zlink_framework ()
                  .add_client_server_channel ("unanswered-client")
                  .server ()
                  .listen (0)
                  .add_send_handler<startup_handler_t, startup_message_t> ();
                requested = store->client_requested.get_future ();
                release = &store->release_client;
                attempts = &store->client_attempts;
            } else {
                app.add_zlink_framework ()
                  .add_fanout_channel ("unanswered-fanout")
                  .enable_publisher (0)
                  .set_routing_id (zlink::routing_id_t::from ("unanswered-fanout"));
                requested = store->fanout_requested.get_future ();
                release = &store->release_fanout;
                attempts = &store->fanout_claims;
            }
        }
        auto probe = std::make_unique<runtime_probe_t> ();
        auto location = probe->location.get_future ();
        auto framework = probe->framework_runtime.get_future ();
        app.add_hosted_service (std::move (probe));
        running_host_t host (app, store, *metrics.meter);
        requested.wait ();
        EXPECT_EQ (framework_runtime_state_t::preparing, app.runtime_state ());
        EXPECT_FALSE (app.is_ready ());
        // Common publication registers its deadline before invoking the provider.
        detail::deadline_scheduler_test_access_t::advance_to (
          std::chrono::steady_clock::now () + location_options_t{}.owner_lease_renew_timeout);
        started.wait ();
        ASSERT_EQ (-1, host.exit_code.load ());
        auto *runtime = location.get ();
        EXPECT_EQ (framework_runtime_state_t::preparing, app.runtime_state ());
        EXPECT_FALSE (app.is_ready ());
        EXPECT_TRUE (runtime->owner_lease_usable ());
        EXPECT_EQ (1, attempts->load ());
        std::promise<void> serving;
        auto ready = serving.get_future ();
        std::once_flag observed;
        auto observation = framework.get ()->observe (16, [&] (const auto &status) {
            if (status.status.state == framework_runtime_state_t::serving)
                std::call_once (observed, [&] { serving.set_value (); });
        });
        release->complete (result_t<void>::success ());
        EXPECT_EQ (framework_runtime_state_t::preparing, app.runtime_state ());
        store->release_renew.complete (result_t<void>::success ());
        runtime->renew_owner_lease_once ();
        ready.wait ();
        EXPECT_TRUE (app.is_ready ());
        EXPECT_GE (attempts->load (), 2);
    }
}
