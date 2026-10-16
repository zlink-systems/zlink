/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework.hpp>
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/fanout/fanout_location_runtime.hpp"
#include <opentelemetry/metrics/noop.h>
#include <opentelemetry/metrics/provider.h>
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
namespace metrics = opentelemetry::metrics;
namespace nostd = opentelemetry::nostd;

// The host creates this gauge after every hosted service has completed start().
// Observing its creation gives the test a startup barrier without polling or sleeps.
class startup_meter_t final : public metrics::Meter
{
  public:
    std::promise<void> started;
    void notify_started_or_exited ()
    {
        std::call_once (_started, [&] { started.set_value (); });
    }

#define FORWARD_INSTRUMENT(Name)                                                                   \
    auto Name (nostd::string_view name, nostd::string_view description = "",                       \
               nostd::string_view unit = "") noexcept                                              \
      -> decltype (std::declval<metrics::NoopMeter &> ().Name (name, description, unit)) override  \
    {                                                                                              \
        return _inner.Name (name, description, unit);                                              \
    }
    FORWARD_INSTRUMENT (CreateUInt64Counter)
    FORWARD_INSTRUMENT (CreateDoubleCounter)
    FORWARD_INSTRUMENT (CreateInt64ObservableCounter)
    FORWARD_INSTRUMENT (CreateDoubleObservableCounter)
    FORWARD_INSTRUMENT (CreateUInt64Histogram)
    FORWARD_INSTRUMENT (CreateDoubleHistogram)
    FORWARD_INSTRUMENT (CreateInt64ObservableGauge)
    FORWARD_INSTRUMENT (CreateInt64UpDownCounter)
    FORWARD_INSTRUMENT (CreateDoubleUpDownCounter)
    FORWARD_INSTRUMENT (CreateInt64ObservableUpDownCounter)
    FORWARD_INSTRUMENT (CreateDoubleObservableUpDownCounter)
#if OPENTELEMETRY_ABI_VERSION_NO >= 2
    FORWARD_INSTRUMENT (CreateInt64Gauge)
    FORWARD_INSTRUMENT (CreateDoubleGauge)
    uintptr_t
    RegisterCallback (metrics::MultiObservableCallbackPtr callback,
                      void *state,
                      nostd::span<metrics::ObservableInstrument *> instruments) noexcept override
    {
        return _inner.RegisterCallback (callback, state, instruments);
    }
    void DeregisterCallback (uintptr_t token) noexcept override
    {
        _inner.DeregisterCallback (token);
    }
#endif
#undef FORWARD_INSTRUMENT

    nostd::shared_ptr<metrics::ObservableInstrument>
    CreateDoubleObservableGauge (nostd::string_view name,
                                 nostd::string_view description = "",
                                 nostd::string_view unit = "") noexcept override
    {
        if (name == "zlink.drain.state")
            notify_started_or_exited ();
        return _inner.CreateDoubleObservableGauge (name, description, unit);
    }

  private:
    metrics::NoopMeter _inner;
    std::once_flag _started;
};

class startup_metrics_t final : public metrics::MeterProvider
{
  public:
    startup_metrics_t () : _previous (metrics::Provider::GetMeterProvider ())
    {
        metrics::Provider::SetMeterProvider (nostd::shared_ptr<metrics::MeterProvider> (
          std::shared_ptr<metrics::MeterProvider> (this, [] (metrics::MeterProvider *) {})));
    }
    ~startup_metrics_t () override { metrics::Provider::SetMeterProvider (_previous); }
#if OPENTELEMETRY_ABI_VERSION_NO >= 2
    nostd::shared_ptr<metrics::Meter>
    GetMeter (nostd::string_view,
              nostd::string_view,
              nostd::string_view,
              const opentelemetry::common::KeyValueIterable *) noexcept override
    {
        return nostd::shared_ptr<metrics::Meter> (std::static_pointer_cast<metrics::Meter> (meter));
    }
    void RemoveMeter (nostd::string_view, nostd::string_view, nostd::string_view) noexcept override
    {
    }
#else
    nostd::shared_ptr<metrics::Meter>
    GetMeter (nostd::string_view, nostd::string_view, nostd::string_view) noexcept override
    {
        return nostd::shared_ptr<metrics::Meter> (std::static_pointer_cast<metrics::Meter> (meter));
    }
#endif
    std::shared_ptr<startup_meter_t> meter = std::make_shared<startup_meter_t> ();

  private:
    nostd::shared_ptr<metrics::MeterProvider> _previous;
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
    task_completion_source_t<void> release_descriptor;
    task_completion_source_t<void> release_fanout;
    std::promise<void> fanout_requested;
    std::promise<void> fanout_stored;
    std::promise<void> fanout_rejected;
    std::atomic_bool reject_fanout{false};
    std::atomic_int fanout_claims{0};
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
    task_t<location_write_result_t> update_mesh_node (mesh_node_descriptor_t descriptor,
                                                      location_write_intent_t intent) override
    {
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
    void release ()
    {
        reject_fanout.store (false);
        release_claim.complete (result_t<void>::success ());
        release_descriptor.complete (result_t<void>::success ());
        release_fanout.complete (result_t<void>::success ());
    }

  private:
    mode_t _mode;
    std::once_flag _requested;
    std::once_flag _stored;
    std::once_flag _fanout_requested;
    std::once_flag _fanout_stored;
    std::once_flag _fanout_rejected;
};

class runtime_probe_t final : public hosted_service_t
{
  public:
    std::promise<framework_runtime_t *> framework_runtime;
    std::promise<runtime::fanout::fanout_location_runtime_t *> fanout;
    task_t<void> start (service_provider_t &services) override
    {
        framework_runtime.set_value (&services.get_required<framework_runtime_t> ());
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
        _app (app), _store (std::move (store)), _host ([this, &meter] {
            char program[] = "startup-lease";
            char *argv[] = {program};
            exit_code.store (_app.run (1, argv));
            meter.notify_started_or_exited ();
        })
    {
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
